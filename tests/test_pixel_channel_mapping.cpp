#include "PixelChannelMapping.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
using PixelChannelMapping::intensityOffset;

static void verify(const uint8_t *order, uint8_t bpp, uint32_t groups, uint32_t groupSize,
                   uint16_t w, uint16_t h, bool vertical=false, bool serpentine=true) {
    std::vector<uint32_t> starts;
    for (uint32_t ch=0; ch<groups*bpp; ++ch) {
        uint32_t off=intensityOffset(ch,bpp,groupSize,order,w,h,true,serpentine,vertical,false,false,0);
        assert(off < groups*groupSize*bpp);
        if ((ch%bpp)==0) starts.push_back(off-order[0]);
        for (uint32_t g=0;g<groupSize;++g)
            assert(off+g*bpp < groups*groupSize*bpp);
    }
    std::sort(starts.begin(),starts.end());
    for(uint32_t i=0;i<groups;++i) assert(starts[i]==i*groupSize*bpp);
}
int main() {
    const uint8_t rgb[4]={0,1,2,3}, grb[4]={1,0,2,3};
    const uint8_t rgbw[4]={0,1,2,3}, wrgb[4]={1,2,3,0};
    // Explicit color-order channel checks.
    assert(intensityOffset(0,3,1,grb,2,2,true,false,false,false,false,0)==1);
    assert(intensityOffset(1,3,1,grb,2,2,true,false,false,false,false,0)==0);
    assert(intensityOffset(0,4,1,wrgb,2,2,true,false,false,false,false,0)==1);
    assert(intensityOffset(3,4,1,wrgb,2,2,true,false,false,false,false,0)==0);
    for(uint32_t gs: {1u,2u,4u,8u}) {
        verify(rgb,3,16,gs,4,4); verify(grb,3,16,gs,4,4,true);
        verify(rgbw,4,16,gs,4,4); verify(wrgb,4,16,gs,4,4,true);
    }
    // Last logical RGBW channel in an 8x8 grouped matrix remains in bounds.
    uint32_t last=intensityOffset(64*4-1,4,3,rgbw,8,8,true,true,false,false,false,0);
    assert(last < 64*3*4);
    std::cout<<"RGB/RGBW grouped matrix channel mapping tests passed\n";
}
