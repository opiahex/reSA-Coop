#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
namespace coop {
// -2 absent/on foot, -1 driver, 0..7 passenger. Vehicle authority owns the
// decision; host/id ordering breaks simultaneous passenger claims consistently.
inline std::array<int8_t,3> ResolveSeatClaims(std::array<int8_t,3> wanted,uint8_t host,uint8_t driver,int seats,uint8_t reserved=0){
    std::array<int8_t,3> result{-2,-2,-2};std::array<bool,8> used{};seats=std::clamp(seats,0,8);
    for(int i=0;i<seats;++i)used[i]=(reserved&(1u<<i))!=0;
    if(driver<3)result[driver]=-1;
    uint8_t order[4]={host,0,1,2};
    for(auto id:order){if(id>=3||id==driver||wanted[id]==-2||result[id]!=-2)continue;
        int seat=wanted[id];if(seat<0||seat>=seats||used[seat]){seat=0;while(seat<seats&&used[seat])++seat;}
        if(seat<seats){result[id]=(int8_t)seat;used[seat]=true;}
    }
    return result;
}
}
