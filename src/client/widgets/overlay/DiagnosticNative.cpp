// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#include "widgets/overlay/DiagnosticNative.h"
#include "widgets/overlay/DiagnosticTaskBridge.h"
#include <windows.h>
#include <atomic>
#include <memory>
#include <new>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "entities/ped/Peds.h"
#include "entities/session/Flow.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/story-stage/StoryStage.h"
#include "common/lib/util/Util.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/memory/HookRegistry.h"
#include "common/config/version/Version.h"
#include "common/lib/diagnostics/FlightRecorder.h"

namespace coop::ui::diagnostic {
namespace native_detail {
template<class T> bool Read(const void*base,size_t offset,T&out){
    static_assert(std::is_trivially_copyable_v<T>);
    auto address=(uintptr_t)base;if(!address||offset>UINTPTR_MAX-address||sizeof(T)-1>UINTPTR_MAX-address-offset)return false;
    size_t checked=0;
    while(checked<sizeof(T)){
        uintptr_t pos=address+offset+checked;MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery((void*)pos,&region,sizeof(region))||region.State!=MEM_COMMIT||(region.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        uintptr_t regionBase=(uintptr_t)region.BaseAddress;
        if(pos<regionBase||pos-regionBase>=region.RegionSize)return false;
        checked+=std::min<size_t>(sizeof(T)-checked,region.RegionSize-(pos-regionBase));
    }
    SIZE_T read=0;T value{};
    if(!ReadProcessMemory(GetCurrentProcess(),(const void*)(address+offset),&value,sizeof(value),&read)||read!=sizeof(value))return false;
    std::memcpy(&out,&value,sizeof(value));return true;
}
static_assert(sizeof(void*)==4&&sizeof(game::CPoolHeader)==0x14,"GTA 1.0 US Win32 pool layout");
constexpr uint32_t kMaxPoolSlots=65536; // diagnostic safety cap, not an engine capacity claim
struct PoolSources {uintptr_t ped=game::addr::PedPool,vehicle=game::addr::VehiclePool;};
bool PoolMember(const void*object,uintptr_t global,uint32_t stride,uint8_t expectedType){
    if(!object||(stride!=game::kPedPoolElemSize&&stride!=game::kVehiclePoolElemSize))return false;
    game::CPoolHeader*pointer=nullptr;game::CPoolHeader header{};
    if(!Read((void*)global,0,pointer)||!Read(pointer,0,header)||!header.objects||!header.byteMap||header.size<=0||uint32_t(header.size)>kMaxPoolSlots)return false;
    uintptr_t base=(uintptr_t)header.objects,map=(uintptr_t)header.byteMap,address=(uintptr_t)object;
    const size_t count=(uint32_t)header.size;
    if((base%alignof(void*))||count>(UINTPTR_MAX-base)/stride||count-1>UINTPTR_MAX-map||address<base)return false;
    uintptr_t offset=address-base;if(offset%stride||offset/stride>=count)return false;
    const size_t index=offset/stride;uint8_t flag=0,type=0;
    if(!Read(header.byteMap,index,flag)||(flag&0x80)||!Read(object,0x36,type)||(type&7)!=expectedType)return false;
    // Best-effort stability check only, not lifetime pinning or ABA proof.
    game::CPoolHeader*againPointer=nullptr;game::CPoolHeader again{};uint8_t againFlag=0;
    return Read((void*)global,0,againPointer)&&againPointer==pointer&&Read(pointer,0,again)&&
        again.objects==header.objects&&again.byteMap==header.byteMap&&again.size==header.size&&
        Read(again.byteMap,index,againFlag)&&againFlag==flag;
}
bool PedValid(game::CPed*p,uintptr_t global=game::addr::PedPool){return PoolMember(p,global,game::kPedPoolElemSize,game::ENTITY_TYPE_PED);}
bool VehicleValid(game::CVehicle*v,uintptr_t global=game::addr::VehiclePool){return PoolMember(v,global,game::kVehiclePoolElemSize,game::ENTITY_TYPE_VEHICLE);}
bool Finite(const game::CVector&p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
bool Position(const void*object,game::CVector&out){
    game::CMatrix*matrix=nullptr;if(!Read(object,0x14,matrix))return false;
    return (matrix?Read(matrix,offsetof(game::CMatrix,pos),out):Read(object,0x04,out))&&Finite(out);
}
std::string Number(double value,bool known=true){if(!known||!std::isfinite(value))return "UNKNOWN";char text[40];std::snprintf(text,sizeof(text),"%.6g",value);return text;}
std::string Vector(const game::CVector&p,bool known){if(!known||!Finite(p))return "UNKNOWN";return Number(p.x)+" "+Number(p.y)+" "+Number(p.z);}
bool InVehicle(game::CPed*p,game::CVehicle*&vehicle,bool&in){
    uint32_t flags=0;if(!Read(p,0x46C,flags))return false;in=(flags&(1u<<8))!=0;vehicle=nullptr;
    return !in||Read(p,0x58C,vehicle);
}
bool Seat(game::CVehicle*v,game::CPed*p,int&seat){
    game::CPed*driver=nullptr;if(!Read(v,0x460,driver))return false;if(driver==p){seat=-1;return true;}
    game::CPed*passengers[8]{};if(!Read(v,0x464,passengers))return false;
    for(int n=0;n<8;++n)if(passengers[n]==p){seat=n;return true;}return false;
}
bool Task(game::CPed*,int&){return false;} // no validated data-only type mapping; never invoke native/virtual code
void CarRows(NativeSnapshot&out,game::CPed*me,uint8_t peer,const PoolSources&pools={}){
    if(!PedValid(me,pools.ped)){out.Add(NativeKind::LocalCar,peer,false,0,"my car: UNKNOWN (local ped absent/invalid)");return;}
    game::CVehicle*v=nullptr;bool in=false;
    bool vehicleRead=InVehicle(me,v,in);
    if(!vehicleRead||!in||!VehicleValid(v,pools.vehicle)){out.Add(NativeKind::LocalCar,peer,false,0,"my car: UNKNOWN (%s)",!vehicleRead?"vehicle reference read failed":!in?"ped not in vehicle":"vehicle absent/invalid");return;}
    uint16_t model=0;uint8_t status=0,passengers=0,getIn=0,getInFlags=0,getOut=0,wheels=0;
    uint32_t flagsLo=0,flagsHi=0;float gas=0;game::CVector velocity{},position{};game::CPed*driver=nullptr;
    bool basic=Read(v,0x22,model)&&Read(v,0x36,status)&&Read(v,0x428,flagsLo)&&Read(v,0x460,driver)&&Read(v,0x484,passengers);
    bool gasKnown=Read(v,0x49C,gas)&&std::isfinite(gas),speedKnown=Read(v,0x44,velocity)&&Finite(velocity);
    double speed=speedKnown?std::sqrt(double(velocity.x)*velocity.x+double(velocity.y)*velocity.y+double(velocity.z)*velocity.z)*50:0;
    int seat=0,task=0;bool seatKnown=Seat(v,me,seat),taskKnown=Task(me,task);
    const char*driverText=!basic?"UNKNOWN":!driver?"none":driver==me?"me":PedValid(driver,pools.ped)?"other":"UNKNOWN";
    if(basic)out.Add(NativeKind::LocalCar,peer,false,0,"my car: model %u status %u engine %u driver %s gas %s speed %s seat %s task %s pass %u",model,status>>3,(flagsLo>>4)&1,driverText,Number(gas,gasKnown).c_str(),Number(speed,speedKnown).c_str(),Number(seat,seatKnown).c_str(),Number(task,taskKnown).c_str(),passengers);
    else out.Add(NativeKind::LocalCar,peer,false,0,"my car: model/status/engine/driver/pass UNKNOWN (read failed); gas %s speed %s seat %s task %s",Number(gas,gasKnown).c_str(),Number(speed,speedKnown).c_str(),Number(seat,seatKnown).c_str(),Number(task,taskKnown).c_str());
    bool flagsKnown=Read(v,0x428,flagsLo)&&Read(v,0x42C,flagsHi)&&Read(v,0x485,getIn)&&Read(v,0x486,getInFlags)&&Read(v,0x487,getOut)&&Read(v,0x961,wheels);
    bool posKnown=Position(v,position);
    if(flagsKnown)out.Add(NativeKind::LocalCar,peer,false,0,"car flags %08X %08X wheelsOnGround %u getIn %u/%02X getOut %02X pos %s",flagsLo,flagsHi,wheels,getIn,getInFlags,getOut,Vector(position,posKnown).c_str());
    else out.Add(NativeKind::LocalCar,peer,false,0,"car flags/entry/wheels UNKNOWN; pos %s",Vector(position,posKnown).c_str());
    game::CControllerState pad{};bool padKnown=Read(game::GetPad(0),0,pad);
    if(padKnown)out.Add(NativeKind::LocalCar,peer,false,0,"pad cross %d LX %d LY %d RS1 %d Sq %d",pad.ButtonCross,pad.LeftStickX,pad.LeftStickY,pad.RightShoulder1,pad.ButtonSquare);
    else out.Add(NativeKind::LocalCar,peer,false,0,"pad: UNKNOWN");
}
void RemoteRows(NativeSnapshot&out,uint8_t peer,const RemotePlayer&rp,game::CPed*me,const PoolSources&pools={}){
    const bool valid=PedValid(rp.ped,pools.ped);game::CVector position{},local{};
    bool posKnown=valid&&Position(rp.ped,position),localKnown=PedValid(me,pools.ped)&&Position(me,local);
    double distance=posKnown&&localKnown?std::sqrt(std::pow(double(position.x)-local.x,2)+std::pow(double(position.y)-local.y,2)+std::pow(double(position.z)-local.z,2)):0;
    int task=0,seat=0;bool taskKnown=valid&&Task(rp.ped,task),in=false;game::CVehicle*vehicle=nullptr;
    bool vehicleKnown=valid&&InVehicle(rp.ped,vehicle,in),seatKnown=vehicleKnown&&in&&VehicleValid(vehicle,pools.vehicle)&&Seat(vehicle,rp.ped,seat);
    bool ref=rp.hasState&&rp.mode!=RemotePlayer::Mode::OnFoot&&rp.vehNetId!=0;
    char reference[24];if(rp.hasState)std::snprintf(reference,sizeof(reference),"%04X",rp.vehNetId);else std::snprintf(reference,sizeof(reference),"UNKNOWN");
    out.Add(NativeKind::Remote,peer,ref,rp.vehNetId,"remote %u: ped %s pos %s net %s dist %s mode %s veh %s seat %s here %s task %s",peer,valid?"valid":"UNKNOWN",Vector(position,posKnown).c_str(),Vector(rp.state.pos,rp.hasState&&Finite(rp.state.pos)).c_str(),Number(distance,posKnown&&localKnown).c_str(),Number((int)rp.mode,rp.hasState).c_str(),reference,Number(rp.seat,rp.hasState).c_str(),!vehicleKnown?"UNKNOWN":!in?"none":Number(seat,seatKnown).c_str(),Number(task,taskKnown).c_str());
}
}

bool CaptureNativeValues(NativeSnapshot&out,uint32_t owningThread,uint16_t selectedEntity){
    if(!owningThread||owningThread!=GetCurrentThreadId()||owningThread!=core::DiagnosticGameThreadId())return false;
    out={};const auto sampledNow=NowMs();auto&net=Net();auto&transport=net.Net();
    out.missionKnown=net.Online();if(out.missionKnown)out.mission=StoryRunId();
    out.Add(NativeKind::Network,255,false,0,"net in %llu KB / out %llu KB, state %d, phase %d",(unsigned long long)(transport.BytesIn()/1024),(unsigned long long)(transport.BytesOut()/1024),(int)net.GetState(),(int)flow::GetPhase());
    if(net.Online()){
        out.Add(NativeKind::Network,net.MyId(),false,0,"session: peer %u host %u connection %u world %u | current mission run %llu phase %u",net.MyId(),net.HostId(),net.MyIncarnation(),net.WorldEpoch(),(unsigned long long)out.mission,CooperativeStoryPhase());
        for(auto&[id,peer]:net.Peers())out.Add(NativeKind::Network,id,false,0,"peer %u: connection %u world %u RTT %u ms",id,peer.incarnation,net.PeerWorldEpoch(id),peer.ping);
    }
    out.Add(NativeKind::Network,255,false,0,"queues in %llu B / out %llu B | stale %llu expired %llu entity rejects %llu waiting %u",(unsigned long long)transport.QueuedInBytes(),(unsigned long long)transport.QueuedOutBytes(),(unsigned long long)net.StalePacketsDiscarded(),(unsigned long long)net.QueuedPacketsDiscarded(),(unsigned long long)net.EntityPacketsDiscarded(),(unsigned)net.EntityPacketsWaiting());
    auto&vehicles=Vehicles::Instance();auto&peds=Peds::Instance();
    out.Add(NativeKind::Remote,255,false,0,"network records: vehicles %u pending vehicles %u NPC %u | entity search selects exact record",(unsigned)vehicles.All().size(),(unsigned)vehicles.Pending().size(),(unsigned)peds.All().size());
    auto audit=core::GetDiagnosticAuditStatus();
    if(audit.available)out.Add(NativeKind::Remote,audit.source,audit.entity!=0,audit.entity,"logical audit: comparisons %llu warnings %llu | last kind %u entity %u source %u reason %u | comparison age %s ms, warning age %s ms",(unsigned long long)audit.comparisons,(unsigned long long)audit.warnings,audit.kind,audit.entity,audit.source,audit.reason,DiagnosticAge(sampledNow,audit.sampledAt,true).c_str(),DiagnosticAge(sampledNow,audit.warnedAt,audit.warnedAt!=0).c_str());
    if(selectedEntity){
        if(auto i=vehicles.All().find(selectedEntity);i!=vehicles.All().end()){auto*n=&i->second;out.Add(NativeKind::LocalCar,n->owner,true,selectedEntity,"vehicle %u: owner %u generation %llu cachedHandle %d poolValid %d state %d age %s ms physicsHeld %d | ownerEpoch UNKNOWN",selectedEntity,n->owner,(unsigned long long)net.EntityGeneration(ENTITY_VEHICLE,selectedEntity),n->handle,native_detail::VehicleValid(n->veh),n->hasState,DiagnosticAge(sampledNow,n->recvMs,n->hasState).c_str(),n->physicsHeld);}
        if(auto i=vehicles.Pending().find(selectedEntity);i!=vehicles.Pending().end()){auto*w=vehicles.PendingWait(selectedEntity);out.Add(NativeKind::LocalCar,i->second.owner,true,selectedEntity,"pending vehicle %u: owner %u model %u reason %s priority %d age %s ms",selectedEntity,i->second.owner,i->second.model,w?w->reason:"unobserved",w?w->priority:0,DiagnosticAge(sampledNow,w?w->since:0,w!=nullptr).c_str());}
        if(auto i=peds.All().find(selectedEntity);i!=peds.All().end()){auto*n=&i->second;out.Add(NativeKind::Remote,n->owner,true,selectedEntity,"NPC %u: owner %u generation %llu cachedHandle %d poolValid %d state %d age %s ms wait %s | ownerEpoch UNKNOWN",selectedEntity,n->owner,(unsigned long long)net.EntityGeneration(ENTITY_PED,selectedEntity),n->handle,native_detail::PedValid(n->ped),n->hasState,DiagnosticAge(sampledNow,n->recvMs,n->hasState).c_str(),n->spawnWait.reason);}
    }
    int32_t state=0;game::CPlayerPed*me=nullptr;
    if(!native_detail::Read((void*)game::addr::GameState,0,state)||state!=game::GAME_STATE_IDLE||!native_detail::Read((void*)game::addr::Players,0,me)||!me){out.Add(NativeKind::LocalCar,255,false,0,"Native car/remote state UNKNOWN (player not ready)");return true;}
    out.nativeReady=true;
    native_detail::CarRows(out,me,net.MyId());
    size_t peers=0;for(auto&[peer,rp]:Players::Instance().All()){
        if(peers++==2){out.truncated=true;break;}native_detail::RemoteRows(out,peer,rp,me);
    }
    return true;
}

namespace task_bridge {
bool PrepareTarget(const Request&r,Prepared&out)noexcept{
    out={};
    if(!core::DiagnosticGameThreadId()||GetCurrentThreadId()!=core::DiagnosticGameThreadId())return false;
    auto&net=Net();if(!net.Online()||net.MyId()>2||!net.WorldEpoch())return false;
    out.localPeer=net.MyId();out.localWorld=net.WorldEpoch();
    auto&selection=out.selection;
    if(r.entitySet){
        if(!r.entity||r.entity>65535)return false;
        const auto&all=Peds::Instance().All();auto i=all.find(uint16_t(r.entity));if(i==all.end())return false;
        const auto&ped=i->second;if(ped.owner>2||ped.handle<0||!ped.ped||(r.peerSet&&r.peer!=ped.owner))return false;
        selection.kind=task_lifecycle::SubjectKind::Npc;selection.entity=uint16_t(r.entity);selection.peer=ped.owner;
        selection.generation=net.EntityGeneration(ENTITY_PED,selection.entity);
        if(!selection.generation||!net.EntityLive(ENTITY_PED,selection.entity))return false;
        selection.nativePedRef=uint32_t(ped.handle);out.referenceKnown=true;out.body=(uintptr_t)ped.ped;
    }else{
        if(!r.peerSet||r.peer>2)return false;selection.kind=task_lifecycle::SubjectKind::Player;selection.peer=uint8_t(r.peer);
        if(selection.peer==net.MyId())selection.motionEpoch=Players::Instance().MotionEpoch();
        else{
            auto*player=Players::Instance().Get(selection.peer);
            if(!player||!player->hasState||!player->ped||player->pedHandle<0)return false;
            selection.motionEpoch=player->state.motion.epoch;selection.nativePedRef=uint32_t(player->pedHandle);
            out.referenceKnown=true;out.body=(uintptr_t)player->ped;
        }
        if(!selection.motionEpoch)return false;
    }
    auto*peer=selection.peer==net.MyId()?nullptr:net.Peer(selection.peer);
    selection.incarnation=selection.peer==net.MyId()?net.MyIncarnation():peer?peer->incarnation:0;
    selection.world=selection.peer==net.MyId()?net.WorldEpoch():net.PeerWorldEpoch(selection.peer);
    if(!selection.incarnation||!selection.world)return false;
    // This is a local subject observation, not a packet or the current SCM cause.
    out.context.source=selection.peer;out.context.incarnation=selection.incarnation;out.context.world=selection.world;
    out.context.entity=selection.entity;out.context.generation=selection.generation;return true;
}
bool ResolveReference(Prepared&target)noexcept{
    if(!core::DiagnosticGameThreadId()||GetCurrentThreadId()!=core::DiagnosticGameThreadId())return false;
    using native_detail::Read;
    int32_t state=0;if(!Read((void*)game::addr::GameState,0,state)||state!=game::GAME_STATE_IDLE)return false;
    if(!target.body){
        if(target.selection.kind!=task_lifecycle::SubjectKind::Player||target.selection.peer!=target.localPeer||
           !Read((void*)game::addr::Players,0,target.body)||!target.body)return false;
    }
    game::CPoolHeader*pointer=nullptr;game::CPoolHeader header{};
    if(!Read((void*)game::addr::PedPool,0,pointer)||!Read(pointer,0,header)||!header.objects||!header.byteMap||
       header.size<=0||uint32_t(header.size)>native_detail::kMaxPoolSlots)return false;
    auto base=(uintptr_t)header.objects,address=target.body;const size_t count=uint32_t(header.size),stride=game::kPedPoolElemSize;
    if(count>(UINTPTR_MAX-base)/stride||address<base||(address-base)%stride)return false;
    const size_t index=(address-base)/stride;if(index>=count||index>UINTPTR_MAX-(uintptr_t)header.byteMap||index>(INT32_MAX>>8))return false;
    uint8_t flag=0,type=0;
    if(!Read(header.byteMap,index,flag)||(flag&0x80)||!Read((void*)address,0x36,type)||(type&7)!=3)return false;
    const uint32_t reference=uint32_t(index<<8)|flag;
    if(target.referenceKnown&&target.selection.nativePedRef!=reference)return false;
    game::CPoolHeader*againPointer=nullptr;game::CPoolHeader again{};uint8_t againFlag=0;
    if(!Read((void*)game::addr::PedPool,0,againPointer)||againPointer!=pointer||!Read(pointer,0,again)||
       again.objects!=header.objects||again.byteMap!=header.byteMap||again.size!=header.size||
       !Read(again.byteMap,index,againFlag)||againFlag!=flag)return false;
    target.selection.nativePedRef=reference;target.referenceKnown=true;return true;
}
}

namespace {
std::atomic_flag contextGuard=ATOMIC_FLAG_INIT;
NativeSnapshot cachedContext{};
hookinspect::Sample cachedInspection{};
bool inspectionCached=false;
constexpr uint64_t kAutomaticNativeIntervalMs=1000;
constexpr uint64_t kNativeFreshnessTtlMs=1500; // existing native gate ceiling includes one-frame writer jitter
enum class NativeCacheState:uint8_t { Unknown,Ready,AwaitingReady,NotReady,CaptureRejected,RecordingChanged };
uint64_t contextCapturedAt=0;
uint32_t contextRecordingToken=0,contextStateToken=0;
NativeCacheState contextState=NativeCacheState::Unknown;
// Owning-game-thread scheduler values only. No worker reads these fields.
uint32_t automaticToken=0;
uint64_t automaticAttemptAt=0;
bool automaticReadyHint=false,automaticWaitingPublished=false;
std::atomic<uint32_t> contextCopyDrops{0};
// Escape only owned bounded metadata; never arbitrary script/packet strings.
template<size_t N>std::string JsonText(const char(&value)[N]){
    std::string out="\"";
    for(size_t i=0;i<N&&value[i];++i){unsigned char c=value[i];
        if(c=='"'||c=='\\'){out+='\\';out+=char(c);}
        else if(c<32||c>=127){char b[7];std::snprintf(b,sizeof b,"\\u%04x",unsigned(c));out+=b;}
        else out+=char(c);
    }
    return out+'"';
}
std::string ExportBytes(const hookreg::Bytes&b){
    std::string out;char hex[3];for(size_t i=0;i<std::min<size_t>(b.captured,hookreg::kBytes);++i){std::snprintf(hex,sizeof hex,"%02x",b.data[i]);out+=hex;}
    return "{\"hex\":\""+out+"\",\"total\":"+std::to_string(b.total)+",\"captured\":"+std::to_string(b.captured)+",\"read\":"+std::to_string((unsigned)b.read)+",\"truncated\":"+(b.truncated?"true":"false")+"}";
}
}

bool CaptureNativeSnapshot(NativeSnapshot&out,uint32_t owningThread,uint16_t selectedEntity){
    if(!owningThread||owningThread!=GetCurrentThreadId()||owningThread!=core::DiagnosticGameThreadId())return false;
    // Token reads are metadata only; CaptureNativeValues keeps the existing
    // owner-thread fence before any game/session/entity read.
    const auto tokenBefore=flight::ActiveRecordingToken();
    if(!CaptureNativeValues(out,owningThread,selectedEntity))return false;
    task_bridge::Append(out,NowMs());
    const auto tokenAfter=flight::ActiveRecordingToken();
    if(!contextGuard.test_and_set(std::memory_order_acquire)){
        cachedContext=out;contextCapturedAt=GetTickCount64();
        contextRecordingToken=tokenBefore&&tokenBefore==tokenAfter?tokenBefore:0;
        contextStateToken=tokenAfter;
        contextState=tokenBefore!=tokenAfter?NativeCacheState::RecordingChanged:
                     out.nativeReady?NativeCacheState::Ready:NativeCacheState::NotReady;
        contextGuard.clear(std::memory_order_release);
    }else contextCopyDrops.fetch_add(1,std::memory_order_relaxed);
    return true;
}

void UpdateAutomaticNativeContext(uint32_t owningThread,bool owningReadyHint){
    if(!owningThread||owningThread!=GetCurrentThreadId()||owningThread!=core::DiagnosticGameThreadId())return;
    const auto token=flight::ActiveRecordingToken();
    if(!token){automaticToken=0;automaticReadyHint=false;automaticWaitingPublished=false;automaticAttemptAt=0;return;}
    if(automaticToken!=token){automaticToken=token;automaticReadyHint=false;automaticWaitingPublished=false;automaticAttemptAt=0;}
    if(!owningReadyHint){
        automaticReadyHint=false;
        if(!automaticWaitingPublished&&!contextGuard.test_and_set(std::memory_order_acquire)){
            // This is an owned phase hint, not a native player-readiness query.
            // Keep an actual not-ready capture when already known for this token.
            if(contextStateToken!=token||contextState!=NativeCacheState::NotReady)contextState=NativeCacheState::AwaitingReady;
            contextStateToken=token;contextGuard.clear(std::memory_order_release);automaticWaitingPublished=true;
        }
        return;
    }
    const bool firstReady=!automaticReadyHint;automaticReadyHint=true;automaticWaitingPublished=false;
    const auto now=GetTickCount64();
    if(!firstReady&&automaticAttemptAt&&now>=automaticAttemptAt&&now-automaticAttemptAt<kAutomaticNativeIntervalMs)return;
    if(!contextGuard.test_and_set(std::memory_order_acquire)){
        const bool fresh=contextState==NativeCacheState::Ready&&contextStateToken==token&&contextRecordingToken==token&&cachedContext.nativeReady&&contextCapturedAt&&now>=contextCapturedAt&&now-contextCapturedAt<kAutomaticNativeIntervalMs;
        const auto captured=contextCapturedAt;contextGuard.clear(std::memory_order_release);
        if(fresh){automaticAttemptAt=captured;return;}
    }
    automaticAttemptAt=now;
    if(!core::IsPlayerReady()){
        if(!contextGuard.test_and_set(std::memory_order_acquire)){contextState=NativeCacheState::NotReady;contextStateToken=token;contextGuard.clear(std::memory_order_release);}
        else contextCopyDrops.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    NativeSnapshot snapshot{};
    if(!CaptureNativeSnapshot(snapshot,owningThread)){
        if(!contextGuard.test_and_set(std::memory_order_acquire)){contextState=NativeCacheState::CaptureRejected;contextStateToken=token;contextGuard.clear(std::memory_order_release);}
        else contextCopyDrops.fetch_add(1,std::memory_order_relaxed);
    }
}

bool CacheDiagnosticInspection(const hookinspect::Sample&sample,bool requested)noexcept{
    if(!core::DiagnosticGameThreadId()||core::DiagnosticGameThreadId()!=GetCurrentThreadId())return false;
    if(contextGuard.test_and_set(std::memory_order_acquire)){contextCopyDrops.fetch_add(1,std::memory_order_relaxed);return false;}
    cachedInspection=sample;inspectionCached=requested;contextGuard.clear(std::memory_order_release);return true;
}

bool ExportDiagnosticContext(const wchar_t*snapshotPath)noexcept{
    HANDLE file=INVALID_HANDLE_VALUE;
    try{
        const auto exportToken=flight::ActiveRecordingToken();
        if(!snapshotPath||!exportToken)return false;
        size_t length=wcsnlen(snapshotPath,2112);if(!length||length==2112)return false;
        const std::wstring path=std::wstring(snapshotPath,length)+L".context.jsonl";
        auto records=std::unique_ptr<hookreg::Record[]>(new(std::nothrow)hookreg::Record[hookreg::kSlots]);if(!records)return false;
        auto registry=hookreg::Snapshot(records.get(),hookreg::kSlots);
        // A busy collector is explicit incomplete evidence, not an empty PASS.
        if(registry.busy)return false;
        NativeSnapshot native{};hookinspect::Sample inspection{};bool inspected=false;uint64_t captured=0;uint32_t captureToken=0,stateToken=0;NativeCacheState state=NativeCacheState::Unknown;bool busy=contextGuard.test_and_set(std::memory_order_acquire);
        if(!busy){native=cachedContext;captured=contextCapturedAt;captureToken=contextRecordingToken;stateToken=contextStateToken;state=contextState;inspection=cachedInspection;inspected=inspectionCached;contextGuard.clear(std::memory_order_release);}
        const auto exportedAt=GetTickCount64();
        const bool ageKnown=!busy&&captured&&exportedAt>=captured;
        const uint64_t age=ageKnown?exportedAt-captured:0;
        const bool recordingMatch=!busy&&captureToken&&captureToken==exportToken;
        const bool stale=ageKnown&&age>kNativeFreshnessTtlMs;
        const bool available=ageKnown&&recordingMatch&&!stale&&native.nativeReady&&stateToken==exportToken&&state==NativeCacheState::Ready;
        const bool readinessKnown=!busy&&stateToken==exportToken&&(state==NativeCacheState::Ready||state==NativeCacheState::NotReady);
        const char*reason=busy?"cache_busy":stateToken!=exportToken?"not_captured_for_recording":state==NativeCacheState::AwaitingReady?"awaiting_owned_ready_frame":state==NativeCacheState::NotReady?"player_not_ready":state==NativeCacheState::CaptureRejected?"capture_rejected":state==NativeCacheState::RecordingChanged?"recording_changed_during_capture":!captured?"not_captured":!recordingMatch?"recording_mismatch":!ageKnown?"age_unknown":stale?"stale":!native.nativeReady?"native_not_ready":available?"available":"unknown";
        if(flight::ActiveRecordingToken()!=exportToken)return false;
        file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return false;
        size_t bytes=0;bool ok=true;
        auto write=[&](std::string line){
            if(!ok)return;if(flight::ActiveRecordingToken()!=exportToken){ok=false;return;}line+='\n';if(line.size()>4u*1024u*1024u-bytes){ok=false;return;}
            DWORD done=0;ok=WriteFile(file,line.data(),(DWORD)line.size(),&done,nullptr)&&done==line.size();if(ok)bytes+=done;
            if(flight::ActiveRecordingToken()!=exportToken)ok=false;
        };
        write("{\"event\":\"diagnostic_context\",\"schema\":1,\"strings_encoding\":\"byte_escaped\",\"version\":\"" COOP_VERSION_STR "\",\"protocol\":"+std::to_string(kProtocolVersion)+",\"pid\":"+std::to_string(GetCurrentProcessId())+",\"snapshot_only\":true,\"registry_historical\":true,\"native_available\":"+(available?"true":"false")+",\"native_busy\":"+(busy?"true":"false")+",\"native_captured_tick_ms\":"+std::to_string(captured)+",\"export_tick_ms\":"+std::to_string(exportedAt)+",\"native_copy_drops\":"+std::to_string(contextCopyDrops.load())+",\"native_recording_token\":"+std::to_string(captureToken)+",\"export_recording_token\":"+std::to_string(exportToken)+",\"native_recording_match\":"+(recordingMatch?"true":"false")+",\"native_age_ms\":"+(ageKnown?std::to_string(age):"null")+",\"native_stale\":"+(ageKnown?(stale?"true":"false"):"null")+",\"native_ready\":"+(readinessKnown?((state==NativeCacheState::Ready&&native.nativeReady)?"true":"false"):"null")+",\"native_unavailable_reason\":\""+reason+"\"}");
        for(size_t i=0;i<registry.count&&ok;++i){const auto&r=records[i];
            write("{\"event\":\"hook_install\",\"id\":"+JsonText(r.spec.id)+",\"module\":"+JsonText(r.spec.module)+",\"purpose\":"+JsonText(r.spec.purpose)+",\"owner\":"+JsonText(r.spec.owner)+
                ",\"kind\":"+std::to_string((unsigned)r.spec.kind)+",\"site\":"+std::to_string(r.spec.site)+",\"span\":"+std::to_string(r.spec.span)+",\"module_base\":"+std::to_string(r.pe.base)+",\"image_size\":"+std::to_string(r.pe.imageSize)+",\"pe_timestamp\":"+std::to_string(r.pe.timestamp)+",\"pe_checksum\":"+std::to_string(r.pe.checksum)+",\"machine\":"+std::to_string(r.pe.machine)+",\"pe_valid\":"+(r.pe.valid?"true":"false")+
                ",\"rva\":"+std::to_string(r.rva)+",\"rva_known\":"+(r.rvaKnown?"true":"false")+",\"attempt\":"+std::to_string(r.attempt)+",\"previous\":"+std::to_string(r.previous)+",\"trampoline\":"+std::to_string(r.trampoline)+",\"outcome\":"+std::to_string((unsigned)r.outcome)+",\"pre\":"+std::to_string((unsigned)r.pre)+",\"post\":"+std::to_string((unsigned)r.post)+",\"chain\":"+std::to_string((unsigned)r.spec.chain)+",\"abi\":"+std::to_string((unsigned)r.abi)+",\"full_sha256_verified\":"+(r.fullSha256Verified?"true":"false")+",\"error\":"+std::to_string(r.error)+",\"expected\":"+ExportBytes(r.spec.expected)+",\"original\":"+ExportBytes(r.original)+",\"desired\":"+ExportBytes(r.spec.desired)+",\"readback\":"+ExportBytes(r.readback)+"}");
        }
        for(size_t i=0;available&&i<native.count&&i<native.rows.size()&&ok;++i){auto&r=native.rows[i];char text[kNativeText]{};std::memcpy(text,r.text.data(),sizeof text);
            write("{\"event\":\"game_snapshot\",\"kind\":"+std::to_string((unsigned)r.kind)+",\"peer\":"+std::to_string(r.peer)+",\"peer_known\":"+(r.peerKnown?"true":"false")+",\"entity\":"+std::to_string(r.entity)+",\"entity_known\":"+(r.entityKnown?"true":"false")+",\"mission\":"+std::to_string(r.mission)+",\"mission_known\":"+(r.missionKnown?"true":"false")+",\"text\":"+JsonText(text)+"}");
        }
        if(inspected)write("{\"event\":\"hook_current_sample\",\"id\":"+JsonText(inspection.install.spec.id)+",\"attempt\":"+std::to_string(inspection.install.attempt)+",\"status\":"+std::to_string((unsigned)inspection.status)+",\"comparison\":"+std::to_string((unsigned)inspection.desiredComparison)+",\"ticks\":"+std::to_string(inspection.ticks)+",\"ticks_valid\":"+(inspection.ticksValid?"true":"false")+",\"context_epoch\":"+std::to_string(inspection.contextEpoch)+",\"lifetime_verified\":false,\"bytes\":"+ExportBytes(inspection.current)+"}");
        const auto policy=flight::CaptureStats();
        write("{\"event\":\"capture_policy\",\"busy\":"+std::string(policy.busy?"true":"false")+",\"attempted\":"+std::to_string(policy.attempted)+",\"filtered\":"+std::to_string(policy.filtered)+",\"sampled_out\":"+std::to_string(policy.sampledOut)+",\"critical_drops\":"+std::to_string(policy.criticalDrops)+",\"state_every\":"+std::to_string(policy.policy.stateEvery)+",\"scm_every\":"+std::to_string(policy.policy.scmEvery)+",\"scm_spans_per_second\":"+std::to_string(policy.policy.scmSpansPerSecond)+",\"scm_pre_attempted\":"+std::to_string(policy.scmPreAttempted)+",\"scm_pre_sampled\":"+std::to_string(policy.scmPreSampled)+",\"scm_pre_filtered\":"+std::to_string(policy.scmPreFiltered)+",\"scm_pre_rate_limited\":"+std::to_string(policy.scmPreRateLimited)+",\"scm_pre_busy\":"+std::to_string(policy.scmPreBusy)+",\"critical_buffered\":"+std::to_string(policy.criticalBuffered)+",\"critical_pending\":"+std::to_string(policy.criticalPending)+",\"critical_reserve_full\":"+std::to_string(policy.criticalReserveFull)+",\"incomplete_metadata\":"+std::to_string(policy.incompleteMetadata)+",\"native_observations_per_second\":"+std::to_string(policy.policy.nativeObservationsPerSecond)+",\"native_pre_attempted\":"+std::to_string(policy.nativePreAttempted)+",\"native_pre_filtered\":"+std::to_string(policy.nativePreFiltered)+",\"native_pre_sampled\":"+std::to_string(policy.nativePreSampled)+",\"native_pre_busy\":"+std::to_string(policy.nativePreBusy)+",\"native_pre_budget\":"+std::to_string(policy.nativePreBudget)+",\"category_mask\":"+std::to_string(policy.policy.categories)+",\"packet_filter\":"+std::to_string(policy.policy.packet)+",\"peer_filter\":"+std::to_string(policy.policy.peer)+",\"entity_filter\":"+std::to_string(policy.policy.entity)+"}");
        const auto&c=registry.counters;
        write("{\"event\":\"diagnostic_context_end\",\"complete\":true,\"registry_count\":"+std::to_string(registry.count)+",\"registry_truncated\":"+(registry.truncated?"true":"false")+",\"native_truncated\":"+(native.truncated?"true":"false")+",\"registry_busy_drops\":"+std::to_string(c.busy)+",\"registry_capacity_drops\":"+std::to_string(c.capacity)+",\"registry_invalid\":"+std::to_string(c.invalid)+",\"registry_conflicts\":"+std::to_string(c.conflict)+",\"registry_stale\":"+std::to_string(c.stale)+",\"registry_already_finalized\":"+std::to_string(c.alreadyFinalized)+"}");
        if(!CloseHandle(file))ok=false;file=INVALID_HANDLE_VALUE;return ok&&flight::ActiveRecordingToken()==exportToken;
    }catch(...){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);return false;}
}
}
