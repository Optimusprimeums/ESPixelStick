#pragma once
/*
 * GPIO_Defs_ESP32C3_DevkitM1.hpp
 *
 * Generic ESP32-C3 DevKitM-1 profile.
 * Keep the default profile intentionally conservative: the C3 has fewer GPIOs
 * and one RMT TX channel group, so avoid assuming classic ESP32 pin maps or SD.
 */

// Output Manager
// GPIO 2, 8 and 9 are strapping pins on ESP32-C3 and are intentionally avoided.
// GPIO 18/19 are used by native USB/JTAG on many C3 designs and are left free.
// Keep two serial outputs to match the C3's smaller RMT resource budget.
const OM_OutputPortDefinition_t OM_OutputPortDefinitions[] =
{
    {OM_PortId_t(0), OM_PortType_t::OM_SERIAL, {gpio_num_t::GPIO_NUM_4}},
    {OM_PortId_t(0), OM_PortType_t::OM_RELAY,  {gpio_num_t::GPIO_NUM_4}},
    {OM_PortId_t(1), OM_PortType_t::OM_SERIAL, {gpio_num_t::GPIO_NUM_5}},
    {OM_PortId_t(1), OM_PortType_t::OM_RELAY,  {gpio_num_t::GPIO_NUM_5}},
    {OM_PortId_t(2), OM_PortType_t::OM_I2C,    {gpio_num_t::GPIO_NUM_6, gpio_num_t::GPIO_NUM_7}},
};

// No default SD mapping: C3 boards vary significantly and GPIO budget is tight.

// Output Types
#define SUPPORT_OutputProtocol_TLS3001
#define SUPPORT_OutputProtocol_DMX
#define SUPPORT_OutputProtocol_GECE
#define SUPPORT_OutputProtocol_GS8208
#define SUPPORT_OutputProtocol_Renard
#define SUPPORT_OutputProtocol_Serial
#define SUPPORT_OutputProtocol_TM1814
#define SUPPORT_OutputProtocol_UCS1903
#define SUPPORT_OutputProtocol_UCS8903
#define SUPPORT_OutputProtocol_WS2811
#define SUPPORT_OutputProtocol_Relay
#define SUPPORT_OutputProtocol_Servo_PCA9685
#define SUPPORT_OutputProtocol_FireGod
