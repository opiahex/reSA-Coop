#pragma once
#include <map>
#include <array>
#include <set>
#include "shared/game/Game.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/api/net-session/Session.h"
#include "common/lib/diagnostics/FlightRecorder.h"
namespace coop {
// KEEP_IN_MEMORY survives native wanted-level model cleanup; release only our own bit.
struct ModelDiagnostic {
    flight::Context context{};uint64_t lastPoll=0;uint32_t localWorld=0;uint8_t outcome=255;bool active=false,requestRecorded=false,captured=false;
};
inline flight::Context SharedModelDiagnosticContext(){
    auto c=Net().DiagnosticContext();
    if(!c.msg){c={};c.source=Net().MyId();c.incarnation=Net().MyIncarnation();c.world=Net().WorldEpoch();}
    return c;
}
inline bool SharedModelDiagnosticCurrent(const ModelDiagnostic&d){
    if(d.localWorld!=Net().WorldEpoch())return false;
    if(d.context.source==Net().MyId())return d.context.incarnation==Net().MyIncarnation()&&d.context.world==Net().WorldEpoch();
    auto*p=Net().Peer(d.context.source);return p&&p->incarnation==d.context.incarnation&&Net().PeerWorldEpoch(d.context.source)==d.context.world;
}
inline bool SharedModelOriginMatches(const flight::Context&a,const flight::Context&b){
    return a.source==b.source&&a.incarnation==b.incarnation&&a.world==b.world&&a.msg==b.msg&&a.entity==b.entity&&a.generation==b.generation&&a.wireTime==b.wireTime&&a.fingerprint==b.fingerprint;
}
inline bool CanCaptureSharedModel(const ModelDiagnostic&d,const flight::Context*origin){
    return !d.captured||d.localWorld!=Net().WorldEpoch()||!SharedModelOriginMatches(d.context,origin?*origin:SharedModelDiagnosticContext());
}
inline void RecordSharedModel(ModelDiagnostic&d,int model,uint16_t stage,uint8_t reason,int loaded=-1){
    if(!flight::Enabled()||!d.active||d.outcome==reason)return;
    flight::Event e{};e.context=d.context;e.stage=stage;e.reason=reason;e.localPeer=Net().MyId();
    flight::AddUnsigned(e,flight::Metric::Operation,1);
    if(model>=0)flight::AddSigned(e,flight::Metric::ModelId,model);else flight::AddSigned(e,flight::Metric::RequestedModel,model);
    if(loaded>=0)flight::AddUnsigned(e,flight::Metric::ObservedLoaded,(uint32_t)loaded);
    (void)flight::Record(e);d.outcome=reason;
}
inline void ObserveLoadedSharedModel(ModelDiagnostic&d,int model){
    if(!d.active||!flight::Enabled())return;
    if(SharedModelDiagnosticCurrent(d))RecordSharedModel(d,model,682,2,1);
    else{RecordSharedModel(d,model,684,6);d.active=false;}
}
inline ModelDiagnostic BeginSharedModelDiagnostic(const flight::Context*origin=nullptr){ModelDiagnostic d;if(flight::Enabled()){d.context=origin?*origin:SharedModelDiagnosticContext();d.localWorld=Net().WorldEpoch();d.active=d.captured=true;}return d;}
// Bounded diagnostic-only coalescing. Counts are local repeated calls for an
// equivalent numeric origin, not unique packets or recorder drops. Flush on
// one-second cadence, eviction and clear; Record failure retains pending data.
struct InvalidSharedModelDiagnostic {
    flight::Context context{};uint64_t lastAttempt=0,repeats=0;uint32_t localWorld=0;int model=0;bool used=false,pending=false;
};
inline std::array<InvalidSharedModelDiagnostic,8> invalidSharedModelDiagnostics{};
inline void FlushInvalidSharedModelDiagnostic(InvalidSharedModelDiagnostic&d,uint64_t now){
    if(!d.used||!d.pending||!flight::Enabled())return;
    d.lastAttempt=now;
    flight::Event e{};e.context=d.context;e.stage=684;e.reason=4;e.localPeer=Net().MyId();
    flight::AddUnsigned(e,flight::Metric::Operation,1);
    flight::AddSigned(e,flight::Metric::RequestedModel,d.model);
    flight::AddUnsigned(e,flight::Metric::SuppressedErrors,d.repeats);
    if(flight::Record(e)){d.repeats=0;d.pending=false;}
}
inline void FlushInvalidSharedModelDiagnostics(uint64_t now,bool all=false){
    if(!flight::Enabled()){invalidSharedModelDiagnostics={};return;}
    for(auto&d:invalidSharedModelDiagnostics){
        if(all||now-d.lastAttempt>=1000)FlushInvalidSharedModelDiagnostic(d,now);
        if(all)d={};
    }
}
inline void RecordInvalidSharedModel(int model,const flight::Context*origin){
    if(!flight::Enabled()){invalidSharedModelDiagnostics={};return;}
    const auto context=origin?*origin:SharedModelDiagnosticContext();const auto now=NowMs();
    InvalidSharedModelDiagnostic*slot=nullptr;
    for(auto&d:invalidSharedModelDiagnostics){
        if(d.used&&d.model==model&&d.localWorld==Net().WorldEpoch()&&SharedModelOriginMatches(d.context,context)&&d.context.channel==context.channel&&d.context.recipient==context.recipient){
            if(d.repeats!=~uint64_t{0})++d.repeats;d.pending=true;
            if(now-d.lastAttempt>=1000)FlushInvalidSharedModelDiagnostic(d,now);
            return;
        }
        if(!slot||(!d.used&&slot->used)||(d.used&&slot->used&&d.lastAttempt<slot->lastAttempt))slot=&d;
    }
    FlushInvalidSharedModelDiagnostic(*slot,now); // best-effort final old-origin summary
    *slot={context,now,0,Net().WorldEpoch(),model,true,true};
    FlushInvalidSharedModelDiagnostic(*slot,now);
}
struct ModelRequest {uint64_t last=0;bool retained=false;ModelDiagnostic diagnostic{};};
inline void PollSharedModelDiagnostic(ModelRequest&r,int model,uint64_t now){
    auto&d=r.diagnostic;if(!flight::Enabled()||!d.active||!d.requestRecorded||d.outcome==2||now-d.lastPoll<100)return;
    d.lastPoll=now;
    if(!SharedModelDiagnosticCurrent(d)){RecordSharedModel(d,model,684,6);d.active=false;return;}
    auto admission=flight::AdmitNativeObservation(d.context,682,Net().MyId(),true);
    if(admission!=flight::NativeAdmission::Admitted){
        if(admission==flight::NativeAdmission::Busy||admission==flight::NativeAdmission::Budget)RecordSharedModel(d,model,684,admission==flight::NativeAdmission::Busy?8:9);
        return;
    }
    if(game::IsModelLoaded(model))ObserveLoadedSharedModel(d,model);else RecordSharedModel(d,model,680,7,0);
}
inline std::map<int,ModelRequest> pendingModelRequests;
inline std::map<int,ModelDiagnostic> unboundModelRequests;
inline void RequestSharedModel(int model,bool priority=false,bool retainLoaded=false,const flight::Context*diagnosticOrigin=nullptr){
    if(!game::IsValidModel(model)){RecordInvalidSharedModel(model,diagnosticOrigin);return;}
    bool loaded=game::IsModelLoaded(model);
    if(loaded){
        if(auto i=unboundModelRequests.find(model);i!=unboundModelRequests.end())ObserveLoadedSharedModel(i->second,model);
        if(auto i=pendingModelRequests.find(model);i!=pendingModelRequests.end())ObserveLoadedSharedModel(i->second.diagnostic,model);
    }
    if(!retainLoaded&&loaded)return;
    // A model-info entry does not imply an IMG resource. Dynamic special slots
    // must first be bound by RequestSpecialModel(name), and an absent resource
    // must not enter RequestModel's native file-read queue.
    if(!loaded&&!game::Ref<uint32_t>(game::addr::StreamingInfo+(uintptr_t)model*0x14+0xC)){
        auto[i,added]=unboundModelRequests.try_emplace(model);
        if(added)LOGW("Deferred model %d: archive resource is not bound",model);
        if((!i->second.active||!SharedModelDiagnosticCurrent(i->second))&&CanCaptureSharedModel(i->second,diagnosticOrigin)){
            if(i->second.active)RecordSharedModel(i->second,model,684,6);
            i->second=BeginSharedModelDiagnostic(diagnosticOrigin);
            if(i->second.active&&!SharedModelDiagnosticCurrent(i->second)){RecordSharedModel(i->second,model,684,6);i->second.active=false;}
            else RecordSharedModel(i->second,model,681,1,0);
        }
        return;
    }
    ModelDiagnostic resumed{};
    if(auto i=unboundModelRequests.find(model);i!=unboundModelRequests.end()){resumed=i->second;unboundModelRequests.erase(i);}

    if(resumed.active&&!SharedModelDiagnosticCurrent(resumed)){RecordSharedModel(resumed,model,684,6);resumed={};}
    auto retain=[](int id){auto[i,added]=pendingModelRequests.try_emplace(id);auto&r=i->second;if(added)r.retained=!(game::Ref<uint8_t>(game::addr::StreamingInfo+(uintptr_t)id*0x14+6)&8);r.last=NowMs();};
    retain(model);
    auto&diagnostic=pendingModelRequests[model].diagnostic;
    if(diagnostic.active&&!SharedModelDiagnosticCurrent(diagnostic)){RecordSharedModel(diagnostic,model,684,6);diagnostic.active=false;}
    if(!diagnostic.active&&CanCaptureSharedModel(diagnostic,diagnosticOrigin))diagnostic=resumed.active?resumed:BeginSharedModelDiagnostic(diagnosticOrigin);
    if(diagnostic.active&&!SharedModelDiagnosticCurrent(diagnostic)){RecordSharedModel(diagnostic,model,684,6);diagnostic.active=false;}
    if(!diagnostic.requestRecorded){RecordSharedModel(diagnostic,model,680,0,loaded?1:0);diagnostic.requestRecorded=true;}
    // RequestModel propagates the keep flag to the texture dictionary chain.
    // Track those leases too, so traffic does not pin every texture forever.
    auto*info=game::Ref<uint8_t*>(game::addr::ModelInfoPtrs+(uintptr_t)model*4);
    int txd=info?game::Field<int16_t>(info,0xA):-1;auto*pool=game::Ref<game::CPoolHeader*>(0xC8800C);
    for(int depth=0;pool&&depth<16&&txd>=0&&txd<pool->size&&pool->byteMap[txd]>=0;++depth){retain(20000+txd);txd=game::Field<int16_t>(pool->objects+(size_t)txd*12,6);}
    game::RequestModel(model,8|(priority?0x10:0));
}
inline void ReleaseSharedModelRequests(bool all=false){
    if(all){for(auto&[model,d]:unboundModelRequests)RecordSharedModel(d,model,683,3);unboundModelRequests.clear();}
    auto now=NowMs();FlushInvalidSharedModelDiagnostics(now,all);for(auto i=pendingModelRequests.begin();i!=pendingModelRequests.end();){
        if(!all)PollSharedModelDiagnostic(i->second,i->first,now);
        if(all||now-i->second.last>1000){if(i->second.retained){auto&flags=game::Ref<uint8_t>(game::addr::StreamingInfo+(uintptr_t)i->first*0x14+6);flags&=~8u;if(!(flags&6))game::Call<0x409C10>(i->first);}RecordSharedModel(i->second.diagnostic,i->first,683,all?3:5);i=pendingModelRequests.erase(i);}else ++i;
    }
}
}
