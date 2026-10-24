#pragma once
#include <array>
#include <cstdint>

namespace coop {
struct SyncTraceEvent { const char* type=nullptr;uint64_t ms=0;uint32_t token=0;int32_t asset=0;uint8_t peer=255,slot=0,stage=0; };
// Game-thread only. Bounded and drained by diagnostics, with no disk/network
// work in engine callbacks. An overflowing trace never blocks game simulation.
struct SyncTraceBuffer {
    std::array<SyncTraceEvent,256> records{};size_t begin=0,count=0;uint32_t dropped=0;
    void Push(SyncTraceEvent e){if(count==records.size()){begin=(begin+1)%records.size();--count;++dropped;}records[(begin+count++)%records.size()]=e;}
    bool Pop(SyncTraceEvent&e){if(!count)return false;e=records[begin];begin=(begin+1)%records.size();--count;return true;}
};
inline SyncTraceBuffer& SyncTrace(){static SyncTraceBuffer b;return b;}
}
