#pragma once
#include <array>
#include <cstdint>
namespace coop {
struct EpochFence {
    std::array<uint32_t,3> peers{};
    uint64_t discarded=0;
    bool Accept(uint8_t peer,uint32_t epoch,bool barrier){
        if(peer>=peers.size()||!epoch){++discarded;return false;}
        auto&current=peers[peer];
        if(!current){current=epoch;return true;}
        if(epoch==current)return true;
        if(barrier&&(int32_t)(epoch-current)>0){current=epoch;return true;}
        ++discarded;return false;
    }
};
}
