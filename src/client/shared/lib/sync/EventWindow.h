#pragma once
#include <cstdint>
namespace coop {
// Bounded duplicate filter, including sequence wrap and limited reordering.
struct EventWindow {
    uint32_t newest=0;uint64_t bits=0;
    bool Accept(uint32_t sequence){
        if(!sequence)return false;
        if(!newest){newest=sequence;bits=1;return true;}
        int32_t delta=(int32_t)(sequence-newest);
        if(delta>0){bits=delta>=64?1:(bits<<delta)|1;newest=sequence;return true;}
        uint32_t age=newest-sequence;if(age>=64)return false;
        auto bit=uint64_t{1}<<age;if(bits&bit)return false;bits|=bit;return true;
    }
};
}
