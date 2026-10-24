#pragma once
#include <array>
#include <cmath>
#include "common/lib/buffer/Buffer.h"
namespace coop {
// Pointer-free native tRadioSettings. Padding is never sent over the wire.
struct RadioSettings {
    std::array<int32_t,5> queue{};
    int32_t current=-1, previous=-1, playTime=0, length=0;
    int8_t flags=2, station=13, bass=0;
    float gain=0;
    std::array<int8_t,5> types{};
    int8_t currentType=6, previousType=6;
    std::array<int8_t,5> indices{};
    int8_t currentIndex=-1, previousIndex=-1;
    void Write(Writer& w)const {
        for(auto v:queue)w.I32(v);
        w.I32(current);w.I32(previous);w.I32(playTime);w.I32(length);
        w.I8(flags);w.I8(station);w.I8(bass);w.F32(gain);
        for(auto v:types)w.I8(v);
        w.I8(currentType);w.I8(previousType);
        for(auto v:indices)w.I8(v);
        w.I8(currentIndex);w.I8(previousIndex);
    }
    bool Read(Reader& r) {
        for(auto&v:queue)v=r.I32();
        current=r.I32();previous=r.I32();playTime=r.I32();length=r.I32();
        flags=r.I8();station=r.I8();bass=r.I8();gain=r.F32();
        for(auto&v:types)v=r.I8();
        currentType=r.I8();previousType=r.I8();
        for(auto&v:indices)v=r.I8();
        currentIndex=r.I8();previousIndex=r.I8();
        if(!r.Ok()||station<0||station>13||bass<0||bass>2||!std::isfinite(gain)||
           std::fabs(gain)>100||playTime< -10||playTime>3600000||length<0||length>3600000)return false;
        if(current< -1||current>4096||previous< -1||previous>4096||
           currentType<0||currentType>7||previousType<0||previousType>7)return false;
        for(auto v:queue)if(v< -1||v>4096)return false;
        for(auto v:types)if(v<0||v>7)return false;
        return true;
    }
};
static_assert(sizeof(RadioSettings)==0x3C);
}
