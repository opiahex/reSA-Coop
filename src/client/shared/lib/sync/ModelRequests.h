#pragma once
#include <map>
#include "shared/game/Game.h"
#include "common/lib/util/Util.h"
namespace coop {
// KEEP_IN_MEMORY survives native wanted-level model cleanup; release only our own bit.
struct ModelRequest {uint64_t last=0;bool retained=false;};
inline std::map<int,ModelRequest> pendingModelRequests;
inline void RequestSharedModel(int model,bool priority=false,bool retainLoaded=false){
    if(!game::IsValidModel(model)||(!retainLoaded&&game::IsModelLoaded(model)))return;
    auto retain=[](int id){auto[i,added]=pendingModelRequests.try_emplace(id);auto&r=i->second;if(added)r.retained=!(game::Ref<uint8_t>(game::addr::StreamingInfo+(uintptr_t)id*0x14+6)&8);r.last=NowMs();};
    retain(model);
    // RequestModel propagates the keep flag to the texture dictionary chain.
    // Track those leases too, so traffic does not pin every texture forever.
    auto*info=game::Ref<uint8_t*>(game::addr::ModelInfoPtrs+(uintptr_t)model*4);
    int txd=info?game::Field<int16_t>(info,0xA):-1;auto*pool=game::Ref<game::CPoolHeader*>(0xC8800C);
    for(int depth=0;pool&&depth<16&&txd>=0&&txd<pool->size&&pool->byteMap[txd]>=0;++depth){retain(20000+txd);txd=game::Field<int16_t>(pool->objects+(size_t)txd*12,6);}
    game::RequestModel(model,8|(priority?0x10:0));
}
inline void ReleaseSharedModelRequests(bool all=false){
    auto now=NowMs();for(auto i=pendingModelRequests.begin();i!=pendingModelRequests.end();){
        if(all||now-i->second.last>1000){if(i->second.retained){auto&flags=game::Ref<uint8_t>(game::addr::StreamingInfo+(uintptr_t)i->first*0x14+6);flags&=~8u;if(!(flags&6))game::Call<0x409C10>(i->first);}i=pendingModelRequests.erase(i);}else ++i;
    }
}
}
