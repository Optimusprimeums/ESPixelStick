/*
* OutputRmtV5.cpp - driver code for ESPixelStick RMT Channel
*
* Project: ESPixelStick - An ESP8266 / ESP32 and E1.31 based pixel driver
* Copyright (c) 2026, 2026 Shelby Merrick
* http://www.forkineye.com
*
*  This program is provided free for you to use in any way that you wish,
*  subject to the laws and regulations where you are using it.  Due diligence
*  is strongly suggested before using this code.  Please give credit where due.
*
*  The Author makes no warranty of any kind, express or implied, with regard
*  to this program or the documentation contained in this document.  The
*  Author shall not be liable in any event for incidental or consequential
*  damages in connection with, or arising out of, the furnishing, performance
*  or use of these programs.
*
*/
#include "ESPixelStick.h"
#if defined(SUPPORT_RMT)
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)

#include "output/OutputRmt.hpp"
#include <driver/rmt_tx.h>

#ifdef USE_RMT_DEBUG_COUNTERS
static uint32_t         RawIsrCounter = 0;
#endif // def USE_RMT_DEBUG_COUNTERS

static TaskHandle_t     SendFrameTaskHandle = NULL;
static uint32_t         FrameCompletes = 0;
static uint32_t         FrameTimeouts = 0;
static c_OutputRmt *    rmt_isr_ThisPtrs[MAX_NUM_RMT_CHANNELS];

//----------------------------------------------------------------------------
void RMT_Task (void *arg)
{
    while(1)
    {
        bool AnyFrameInFlight = false;

        // Launch every ready channel before waiting. ESP32-S3 RMT channels are
        // independent hardware engines, so this allows parallel pixel output.
        for (c_OutputRmt * pRmt : rmt_isr_ThisPtrs)
        {
            if(nullptr == pRmt)
            {
                continue;
            }

            if(pRmt->IsFrameInFlight())
            {
                if(pRmt->IsFrameComplete())
                {
                    pRmt->CompleteFrame();
                    ++FrameCompletes;
                }
                else if((xTaskGetTickCount() - pRmt->GetFrameStartTick()) > pdMS_TO_TICKS(100))
                {
                    pRmt->TimeoutFrame();
                    ++FrameTimeouts;
                }
            }

            if(!pRmt->IsFrameInFlight())
            {
                bool OtherFrameActive = AnyFrameInFlight;
                if(pRmt->StartNextFrame() && OtherFrameActive)
                {
                    pRmt->NoteConcurrentStart();
                }
            }

            AnyFrameInFlight |= pRmt->IsFrameInFlight();
        }

        // Hardware TX completion wakes this task immediately. The short timeout is
        // also a recovery/poll path for channels with no current frame.
        ulTaskNotifyTake(pdTRUE, AnyFrameInFlight ? pdMS_TO_TICKS(2) : pdMS_TO_TICKS(1));
    }
} // RMT_Task

//----------------------------------------------------------------------------
c_OutputRmt::c_OutputRmt()
{
    // DEBUG_START;

    memset((void *)&SendBuffer[0], 0x00, sizeof(SendBuffer));

    // DEBUG_END;
} // c_OutputRmt

//----------------------------------------------------------------------------
c_OutputRmt::~c_OutputRmt ()
{
    // DEBUG_START;

    if (HasBeenInitialized)
    {
        // Remove the instance from the scheduler before stopping hardware so
        // the worker cannot start another frame during destruction.
        rmt_isr_ThisPtrs[OutputRmtConfig.RmtChannelId] = (c_OutputRmt*)nullptr;
        FrameInFlight = false;
        FrameEncodingComplete = false;
        ISR_ResetRmtBlockPointers (); // Stop transmitter
        rmt_disable(rmt_channel_handle);
        yield();
    }

    // DEBUG_END;
} // ~c_OutputRmt

//----------------------------------------------------------------------------
/* shell function to set the 'this' pointer of the real ISR
   This allows me to use non static variables in the ISR.
 */
static size_t IRAM_ATTR ISR_encoder_callback(const void *data, size_t data_size,
                                             size_t symbols_written, size_t symbols_free,
                                             rmt_item32_t *symbols, bool *done, void *arg)
{
#ifdef USE_RMT_DEBUG_COUNTERS
    ++RawIsrCounter;
#endif // def USE_RMT_DEBUG_COUNTERS

    return reinterpret_cast<c_OutputRmt*>(arg)->ISR_Handler(data, data_size,
                                             symbols_written, symbols_free,
                                             symbols, done);
} // ISR_encoder_callback

//----------------------------------------------------------------------------
// The encoder callback only means all source symbols have been produced.
// This callback is the authoritative end-of-wire transaction completion.
static bool IRAM_ATTR ISR_tx_done_callback(rmt_channel_handle_t tx_chan,
                                           const rmt_tx_done_event_data_t *edata,
                                           void *arg)
{
    (void)tx_chan;
    (void)edata;

    c_OutputRmt *pRmt = reinterpret_cast<c_OutputRmt*>(arg);
    pRmt->MarkFrameTransmissionCompleteFromISR();

    BaseType_t HigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(SendFrameTaskHandle, &HigherPriorityTaskWoken);
    return (HigherPriorityTaskWoken == pdTRUE);
}

//----------------------------------------------------------------------------
void c_OutputRmt::Begin (OutputRmtConfig_t config, c_OutputCommon * _pParent )
{
    // DEBUG_START;

    do // once
    {
        // save the new config
        OutputRmtConfig = config;

        if (nullptr == OutputRmtConfig.ISR_GetNextIntensityBit ||
            nullptr == OutputRmtConfig.StartNewDataFrame)
        {
            String Reason = (F("Invalid RMT configuration parameters. Rebooting"));
            RequestReboot(Reason, 10000);
            break;
        }

        // DEBUG_V (String ("                    DataPin: ") + String (OutputRmtConfig.DataPin));
        // DEBUG_V (String ("               RmtChannelId: ") + String (OutputRmtConfig.RmtChannelId));
        // DEBUG_V (String ("           RMT_TickLengthNS: ") + String (RMT_TickLengthNS));
        // DEBUG_V (String ("     RMT_TICK_RESOLUTION_HZ: ") + String (RMT_TICK_RESOLUTION_HZ));

        // Configure RMT channel
        rmt_tx_channel_config_t tx_chan_config =
        {
            .gpio_num = OutputRmtConfig.DataPin,
            .clk_src = RMT_CLK_SRC_DEFAULT,         // select source clock
            .resolution_hz = uint32_t(RMT_TICK_RESOLUTION_HZ),
            .mem_block_symbols = NUM_RMT_SLOTS,     // increase the block size can make the LED less flickering
            .trans_queue_depth = 2,                 // one active frame per channel; avoid wasting internal RAM on an oversized transaction queue
            .intr_priority = 0,                     // auto set interrupt priority
            .flags =
            {
                .invert_out = false,    /*!< Whether to invert the RMT channel signal before output to GPIO pad */
                .with_dma = false,      /*!< If set, the driver will allocate an RMT channel with DMA capability */
                .io_loop_back = false,  /*!< The signal output from the GPIO will be fed to the input path as well */
                .io_od_mode = false,    /*!< Configure the GPIO as open-drain mode */
                .init_level = LOW,      /*!< Set the initial level of the RMT channel signal */
            }
        };
        ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &rmt_channel_handle));
        DEBUG_V();

        const rmt_tx_event_callbacks_t tx_callbacks =
        {
            .on_trans_done = ISR_tx_done_callback
        };
        ESP_ERROR_CHECK(rmt_tx_register_event_callbacks(rmt_channel_handle, &tx_callbacks, this));
        DEBUG_V();

        const rmt_simple_encoder_config_t encoder_cfg =
        {
            .callback = ISR_encoder_callback,
            .arg      = this
            //Note we don't set min_chunk_size here as the default of 64 is good enough.
        };
        ESP_ERROR_CHECK(rmt_new_simple_encoder(&encoder_cfg, &rmt_encoder_handle));
        DEBUG_V();

        tx_config.flags.eot_level = OutputRmtConfig.idle_level == rmt_idle_level_t::RMT_IDLE_LEVEL_HIGH;

        // reset the internal and external pointers to the start of the mem block
        ISR_ResetRmtBlockPointers ();
        // DEBUG_V();

        if(!SendFrameTaskHandle)
        {
            // DEBUG_V();
            // DEBUG_V("Start SendFrameTask");
            xTaskCreatePinnedToCore(RMT_Task, "RMT_Task", 4096, NULL, 5, &SendFrameTaskHandle, 1);
            // DEBUG_V();
            vTaskPrioritySet(SendFrameTaskHandle, 5);
        }
        // DEBUG_V();
        // DEBUG_V("Add this instance to the running list");
        pParent = _pParent;
        rmt_isr_ThisPtrs[OutputRmtConfig.RmtChannelId] = this;
        // DEBUG_V();

        HasBeenInitialized = true;
    } while (false);

    // DEBUG_END;

} // Begin

//----------------------------------------------------------------------------
void c_OutputRmt::GetStatus (ArduinoJson::JsonObject& jsonStatus)
{
    // // DEBUG_START;

    jsonStatus[F("NumRmtSlotOverruns")] = NumRmtSlotOverruns;
#ifdef USE_RMT_DEBUG_COUNTERS
    jsonStatus[F("OutputIsPaused")] = OutputIsPaused;
    JsonObject debugStatus = jsonStatus["RMT Debug"].to<JsonObject>();
    debugStatus["RmtChannelId"]                 = OutputRmtConfig.RmtChannelId;
    debugStatus["GPIO"]                         = OutputRmtConfig.DataPin;
    #ifdef CONFIG_IDF_TARGET_ESP32S3
    debugStatus["conf0"]                        = "0x" + String(RMT.chnconf0[OutputRmtConfig.RmtChannelId].val, HEX);
    debugStatus["conf1"]                        = "0x" + String(RMT.chmconf[OutputRmtConfig.RmtChannelId].conf0.val, HEX);
    debugStatus["tx_lim_ch"]                    = String(RMT.chn_tx_lim[OutputRmtConfig.RmtChannelId].tx_lim_chn);

    #endif // def CONFIG_IDF_TARGET_ESP32S3

    debugStatus["ErrorIsr"]                     = RMT_DEBUG_COUNTER(ErrorIsr);
    debugStatus["FrameCompletes"]               = FrameCompletes;
    debugStatus["FrameStartCounter"]            = RMT_DEBUG_COUNTER(FrameStartCounter);
    debugStatus["FrameTimeouts"]                = FrameTimeouts;
    debugStatus["FailedToSendAllData"]          = RMT_DEBUG_COUNTER(FailedToSendAllData);
    debugStatus["IncompleteFrame"]              = RMT_DEBUG_COUNTER(IncompleteFrame);
    debugStatus["IntensityValuesSent"]          = RMT_DEBUG_COUNTER(IntensityValuesSent);
    debugStatus["IntensityValuesSentLastFrame"] = RMT_DEBUG_COUNTER(IntensityValuesSentLastFrame);
    debugStatus["IntensityBitsSent"]            = RMT_DEBUG_COUNTER(IntensityBitsSent);
    debugStatus["IntensityBitsSentLastFrame"]   = RMT_DEBUG_COUNTER(IntensityBitsSentLastFrame);
    debugStatus["IntTxEndIsrCounter"]           = RMT_DEBUG_COUNTER(IntTxEndIsrCounter);
    debugStatus["IntTxThrIsrCounter"]           = RMT_DEBUG_COUNTER(IntTxThrIsrCounter);
    debugStatus["ISRcounter"]                   = RMT_DEBUG_COUNTER(ISRcounter);
    debugStatus["RanOutOfData"]                 = RMT_DEBUG_COUNTER(RanOutOfData);
    debugStatus["RawIsrCounter"]                = RawIsrCounter;
    debugStatus["RmtEntriesTransfered"]         = RMT_DEBUG_COUNTER(RmtEntriesTransfered);
    debugStatus["RmtWhiteDetected"]             = RMT_DEBUG_COUNTER(RmtWhiteDetected);
    debugStatus["RmtXmtFills"]                  = RMT_DEBUG_COUNTER(RmtXmtFills);
    debugStatus["ISRpaused"]                    = RMT_DEBUG_COUNTER(ISRpaused);
    debugStatus["SendBlockIsrCounter"]          = RMT_DEBUG_COUNTER(SendBlockIsrCounter);
    debugStatus["UnknownISRcounter"]            = RMT_DEBUG_COUNTER(UnknownISRcounter);
    debugStatus["WriteToBuffer"]                = RMT_DEBUG_COUNTER(WriteToBuffer);
    debugStatus["FrameInFlight"]                 = FrameInFlight;
    debugStatus["ConcurrentFrameStarts"]         = ConcurrentFrameStarts;
    debugStatus["ChannelFrameCompletes"]         = ChannelFrameCompletes;
    debugStatus["ChannelFrameTimeouts"]          = ChannelFrameTimeouts;

#ifdef IncludeBufferData
    {
        uint32_t index = 0;
        uint32_t * CurrentPointer = (uint32_t*)const_cast<rmt_item32_t*>(&SendBuffer[0]);
        // for(index = 0; index < NUM_RMT_SLOTS; index++)
        for(index = 0; index < 2; index++)
        {
            uint32_t data = CurrentPointer[index];
            debugStatus[String("Buffer Data ") + String(index)] = String(data, HEX);
        }
    }
    {
        uint32_t index = 0;
        uint32_t * CurrentPointer = (uint32_t*)const_cast<rmt_item32_t*>(&RMTMEM.chan[OutputRmtConfig.RmtChannelId].data32[0]);
        // for(index = 0; index < NUM_RMT_SLOTS; index++)
        for(index = 0; index < 2; index++)
        {
            uint32_t data = CurrentPointer[index];
            debugStatus[String("RMT Data ") + String(index)] = String(data, HEX);
        }
    }
#endif // def IncludeBufferData
#endif // def USE_RMT_DEBUG_COUNTERS
    // // DEBUG_END;
} // GetStatus

//----------------------------------------------------------------------------
void c_OutputRmt::SetBitDuration (double BitLenNs, rmt_item32_t & OutputBit, uint32_t & OutputNumBits)
{
    // DEBUG_START;

    double NumBitTicks        = BitLenNs / RMT_TickLengthNS;
    double MaxTicksPerBitHalf = 0b111111111111111;
    double MaxTicksPerBit     = MaxTicksPerBitHalf * 2;
           OutputNumBits      = ceil(NumBitTicks / MaxTicksPerBit);
    if(0 == OutputNumBits) {OutputNumBits = 1;}
    uint   NumTicksPerbit     = (NumBitTicks / OutputNumBits);

    OutputBit.duration0       = uint(NumTicksPerbit / 2);
    // OutputBit.level0          = 0;
    OutputBit.duration1       = OutputBit.duration0;
    // OutputBit.level1          = 0;

    // DEBUG_V(String("            BitLenNs: ") + String(BitLenNs));
    // DEBUG_V(String("         NumBitTicks: ") + String(NumBitTicks));
    // DEBUG_V(String("  MaxTicksPerBitHalf: ") + String(MaxTicksPerBitHalf));
    // DEBUG_V(String("      MaxTicksPerBit: ") + String(MaxTicksPerBit));
    // DEBUG_V(String("       OutputNumBits: ") + String(OutputNumBits));
    // DEBUG_V(String("      NumTicksPerbit: ") + String(NumTicksPerbit));
    // DEBUG_V(String(" OutputBit.duration0: ") + String(OutputBit.duration0));
    // DEBUG_V(String(" OutputBit.duration1: ") + String(OutputBit.duration1));
    // DEBUG_V(String("     OutputBit.Total: ") + String(OutputBit.duration0 + OutputBit.duration1));

    // DEBUG_END;
} // SetBitDuration

//----------------------------------------------------------------------------
void IRAM_ATTR c_OutputRmt::ISR_CreateIntensityData ()
{
    /// DEBUG_START;
    // Serial.print('I');

    uint32_t NumAvailableBufferSlotsToFill = NumSendBufferSlots - NumUsedEntriesInSendBuffer;
    // Serial.print(String(NumAvailableBufferSlotsToFill));
    rmt_item32_t Data;
    while(NumAvailableBufferSlotsToFill)
    {
        // Serial.print('K');
        if(!OutputRmtConfig.ISR_GetNextIntensityBit(OutputRmtConfig.arg, Data))
        {
            // no more data to send
            break;
        }
        WriteToBuffer(Data);
        --NumAvailableBufferSlotsToFill;
    };

    ///DEBUG_END;

} // ISR_CreateIntensityData

//----------------------------------------------------------------------------
size_t IRAM_ATTR c_OutputRmt::ISR_Handler (const void *data, size_t data_size,
                                           size_t symbols_written, size_t symbols_free,
                                           rmt_item32_t *symbols, bool *done)
{
    /// DEBUG_START;
    size_t NumSymbolsTransfered = 0;

    // uint32_t int_st = RMT.int_raw.val;
    ///DEBUG_V(String("              int_st: 0x") + String(int_st, HEX));
    ///DEBUG_V(String("         RMT_INT_BIT: 0x") + String(RMT_INT_BIT, HEX));
    // ClearRmtInterrupts;

    RMT_DEBUG_INC_COUNTER(ISRcounter);

    do // once
    {
        if(OutputIsPaused)
        {
            RMT_DEBUG_INC_COUNTER(ISRpaused);
            // nothing more to send
            *done = true;
            break;
        }

        RMT_DEBUG_INC_COUNTER(SendBlockIsrCounter);
        // transfer any prefetched data to the hardware transmitter
        NumSymbolsTransfered = ISR_TransferIntensityDataToRMT( symbols, symbols_free );

        // refill the buffer
        ISR_CreateIntensityData();

        // is there any data left to enqueue?
        if (0 == NumUsedEntriesInSendBuffer)
        {
            RMT_DEBUG_INC_COUNTER(RanOutOfData);
            // Encoding is complete, but the final symbols may still be on the
            // wire. ISR_tx_done_callback owns transaction completion/wakeup.
            *done = true;
        }
        else
        {
            *done = false;
        }
    } while(false);

    ///DEBUG_END;
    return NumSymbolsTransfered;
} // ISR_Handler

//----------------------------------------------------------------------------
inline void IRAM_ATTR c_OutputRmt::ISR_ResetRmtBlockPointers()
{
    rmt_disable(rmt_channel_handle);

    RmtBufferWriteIndex  = 0;
    SendBufferWriteIndex = 0;
    SendBufferReadIndex  = 0;
    NumUsedEntriesInSendBuffer = 0;

    rmt_enable(rmt_channel_handle);
}

//----------------------------------------------------------------------------
inline void c_OutputRmt::StartNewDataFrame()
{
    // DEBUG_START;
    OutputRmtConfig.StartNewDataFrame(OutputRmtConfig.arg);
    // DEBUG_END;
} // StartNewDataFrame

//----------------------------------------------------------------------------
size_t IRAM_ATTR c_OutputRmt::ISR_TransferIntensityDataToRMT (rmt_item32_t *symbols, uint32_t MaxNumEntriesToTransfer)
{
    /// DEBUG_START;

    size_t NumEntriesTransfered = 0;
    size_t NumEntriesToTransfer = min(NumUsedEntriesInSendBuffer, MaxNumEntriesToTransfer);

#ifdef USE_RMT_DEBUG_COUNTERS
    if(NumEntriesToTransfer)
    {
        RMT_DEBUG_INC_COUNTER(RmtXmtFills);
        RMT_DEBUG_COUNTER(RmtEntriesTransfered) = NumEntriesToTransfer;
    }
#endif // def USE_RMT_DEBUG_COUNTERS
    while(NumEntriesToTransfer)
    {
        symbols[NumEntriesTransfered++] = SendBuffer[SendBufferReadIndex++];
        SendBufferReadIndex &= (NumSendBufferSlots - 1); // do wrap
        --NumEntriesToTransfer;
        --NumUsedEntriesInSendBuffer;
    }

    ///DEBUG_END;
    return NumEntriesTransfered;

} // ISR_TransferIntensityDataToRMT

//----------------------------------------------------------------------------
void c_OutputRmt::PauseOutput(bool PauseOutput)
{
    /// DEBUG_START;

    if (OutputIsPaused == PauseOutput)
    {
        ///DEBUG_V("no change. Ignore the call");
    }
    else if (PauseOutput)
    {
        ///DEBUG_V("stop the output");
    }

    OutputIsPaused = PauseOutput;
    if(PauseOutput)
    {
        FrameInFlight = false;
        FrameEncodingComplete = false;
        ISR_ResetRmtBlockPointers();
    }

    ///DEBUG_END;
} // PauseOutput

//----------------------------------------------------------------------------
bool c_OutputRmt::StartNewFrame ()
{
    // DEBUG_START;

    bool Response = false;

    do // once
    {
        if(OutputIsPaused)
        {
            // DEBUG_V("Paused");
            // Stop the transmitter
            rmt_disable(rmt_channel_handle);
            ISR_ResetRmtBlockPointers ();
            break;
        }

		// Stop the transmitter
        ISR_ResetRmtBlockPointers ();

        #ifdef USE_RMT_DEBUG_COUNTERS
        RMT_DEBUG_INC_COUNTER(FrameStartCounter);
        RMT_DEBUG_COUNTER(IntensityValuesSentLastFrame) = RMT_DEBUG_COUNTER(IntensityValuesSent);
        RMT_DEBUG_COUNTER(IntensityValuesSent)          = 0;
        RMT_DEBUG_COUNTER(IntensityBitsSentLastFrame)   = RMT_DEBUG_COUNTER(IntensityBitsSent);
        RMT_DEBUG_COUNTER(IntensityBitsSent)            = 0;
        #endif // def USE_RMT_DEBUG_COUNTERS

        // set up to send a new frame
        StartNewDataFrame ();
        // DEBUG_V();

        // this fills the send buffer
        ISR_CreateIntensityData ();

        // DEBUG_V(String("             NUM_RMT_SLOTS: ") + String(NUM_RMT_SLOTS));
        // DEBUG_V(String("NumUsedEntriesInSendBuffer: ") + String(NumUsedEntriesInSendBuffer));
        // DEBUG_V(String("         IntensityBitsSent: ") + String(IntensityBitsSent));
        // DEBUG_V(String("       IntensityValuesSent: ") + String(IntensityValuesSent));
        // DEBUG_V(String("NumInterFrameRmtSlotsCount: ") + String(NumInterFrameRmtSlotsCount));
        // DEBUG_V(String("NumFrameStartRmtSlotsCount: ") + String(NumFrameStartRmtSlotsCount));

        // pinMode(42, OUTPUT);
        // digitalWrite(42, LOW);
        // DEBUG_V("start the transmitter");

        // rmt_enable(rmt_channel_handle);
    
        // DEBUG_V(String("rmt_channel_handle: ") + String(uint32_t(rmt_channel_handle)));
        // DEBUG_V(String("rmt_encoder_handle: ") + String(uint32_t(rmt_encoder_handle)));
        // DEBUG_V(String("       BufferStart: ") + String(uint32_t(OutputRmtConfig.BufferStart)));
        // DEBUG_V(String("   NumBytesInFrame: ") + String(uint32_t(OutputRmtConfig.NumBytesInFrame)));
        FrameEncodingComplete = false;
        FrameInFlight = true;
        FrameStartTick = xTaskGetTickCount();
        esp_err_t TransmitResult = rmt_transmit(rmt_channel_handle, rmt_encoder_handle, OutputRmtConfig.BufferStart, OutputRmtConfig.NumBytesInFrame, &tx_config);
        if(ESP_OK != TransmitResult)
        {
            FrameInFlight = false;
            ESP_ERROR_CHECK_WITHOUT_ABORT(TransmitResult);
            break;
        }

        // rmt_set_gpio (OutputRmtConfig.RmtChannelId, rmt_mode_t::RMT_MODE_TX, OutputRmtConfig.DataPin, false);
        // digitalWrite(42, HIGH);
        // delay(1);

        Response = true;
    } while(false);

    // DEBUG_END;
    return Response;

} // StartNewFrame

#endif // ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#endif // defined(SUPPORT_RMT)
