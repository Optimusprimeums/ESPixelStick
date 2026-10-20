#include "MatrixMapping.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <vector>
using MatrixMapping::map;
static std::vector<uint32_t> mapped(unsigned w, unsigned h, bool vertical=false,
    bool serpentine=true, bool rx=false, bool ry=false, unsigned rotation=0) {
    std::vector<uint32_t> out;
    for (unsigned i=0; i<w*h; ++i)
        out.push_back(map(i,w,h,true,serpentine,vertical,rx,ry,rotation));
    return out;
}
int main() {
    assert((mapped(4,4) == std::vector<uint32_t>{0,1,2,3,7,6,5,4,8,9,10,11,15,14,13,12}));
    assert((mapped(3,2,true) == std::vector<uint32_t>{0,3,4,1,2,5}));
    assert((mapped(3,2,false,false,false,false,180) == std::vector<uint32_t>{5,4,3,2,1,0}));
    assert(map(5,0,0,false,true,false,false,false,0)==5);
    for (unsigned w : {4u,16u,32u}) for (unsigned h : {4u,16u,32u})
      for (bool v : {false,true}) for (bool s : {false,true})
       for (bool rx : {false,true}) for (bool ry : {false,true})
        for (unsigned rot : {0u,90u,180u,270u}) {
            if (w!=h && (rot==90 || rot==270)) continue;
            auto out=mapped(w,h,v,s,rx,ry,rot);
            std::sort(out.begin(),out.end());
            for (unsigned i=0;i<w*h;++i) assert(out[i]==i);
        }
    std::cout << "Production C++ matrix mapper tests passed\n";
}
