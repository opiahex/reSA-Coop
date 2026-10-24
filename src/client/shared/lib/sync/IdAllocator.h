#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
namespace coop {
// Network IDs remain in quarantine longer than the ENet connection timeout.
// Exhaustion is a retry, never an infinite loop or an overwrite of a live ID.
template<size_t Slots>class IdAllocator {
    static_assert(Slots<=65536&&(Slots&(Slots-1))==0);
    std::array<uint64_t,Slots> retired{};
    uint32_t cursor=1;
public:
    template<class Used>uint16_t Take(uint16_t prefix,uint64_t now,Used used){
        for(size_t n=0;n<Slots;++n){
            size_t slot=cursor++&(Slots-1);uint16_t id=(uint16_t)(prefix|slot);
            if(!id||used(id)||(retired[slot]&&now<retired[slot]+120000))continue;
            return id;
        }
        return 0;
    }
    void Retire(uint16_t id,uint64_t now){retired[id&(Slots-1)]=now?now:1;}
    void Reset(uint32_t seed=1){retired.fill(0);cursor=seed;}
};
}
