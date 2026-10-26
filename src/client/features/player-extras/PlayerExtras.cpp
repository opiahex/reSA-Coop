#include <map>
#include <array>
#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/player/PlayerExtras.h"
#include "entities/session/Flow.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Anim.h"
#include "shared/game/Transform.h"
#include "shared/game/Script.h"
#include "shared/lib/memory/Memory.h"
#include "entities/player/PlayerExtrasState.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/story-stage/StoryStage.h"
#include "shared/api/story-loadout/StoryLoadout.h"

namespace coop {
namespace {
using namespace game;using game::script::Cmd;
Quat Multiply(const Quat&a,const Quat&b){return {
 a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
 a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};}
CObject* Held(CPed*ped){auto*t=anim::FindTask(ped,307);auto*o=t?Field<CObject*>(t,8):nullptr;return IsObjectValid(o)?o:nullptr;}
bool Attached(CObject*o,CPed*ped){return o&&o->AttachedTo()==ped;}
struct RemoteExtras {PlayerExtrasState state;uint32_t time=0;uint64_t received=0;std::array<int,3> objects{-1,-1,-1};bool phone=false;int pedHandle=-1;};
std::map<uint8_t,RemoteExtras> extras;
struct Resume {OnFootState state;uint16_t vehicle=0;int8_t seat=-2;PlayerExtrasState extras;AnimationState action;uint64_t saved=0,run=0;uint32_t phase=0;std::vector<EntityRef>refs;};
std::map<std::string,Resume> remembered;
std::map<uint8_t,Resume> rejoining;
Resume pendingResume;uint64_t restoreAt=0;
MotionState resumedMotion;uint64_t motionRestoreUntil=0;
uint64_t lastSent=0;std::vector<uint8_t> lastPacket;

void Bone(CPed*p,uint8_t bone,CVector&out){
    if(bone>=19){out=p->GetPosition();return;}
    auto*frame=Field<void*>(p,0x488+bone*4);
    if(!frame){out=p->GetPosition();return;}
    CallMethod<0x5E4280>(p,&out,(int)Field<uint32_t>(frame,0x14),true);
}
PlayerExtrasState CaptureExtras(CPlayerPed*ped){
    PlayerExtrasState state;state.phone=(ped->PedFlags4()&2)!=0;
    for(int i=0;i<13;++i){state.weapons[i]=(uint8_t)ped->Weapons()[i].type;state.ammo[i]=(uint32_t)std::clamp(ped->Weapons()[i].ammoTotal,0,1000000);}
    if(!ped->Matrix())return state;
    auto actor=Quat::FromMatrix(*ped->Matrix());Quat inverse{-actor.x,-actor.y,-actor.z,actor.w};
    auto add=[&](CObject*o,uint8_t bone){
        if(!o||!o->Matrix()||state.props.size()>=3)return;
        HeldProp prop;prop.model=o->ModelIndex();prop.bone=bone;
        CVector anchor;Bone(ped,bone,anchor);auto delta=o->GetPosition()-anchor;
        prop.offset={Dot(delta,ped->Matrix()->right),Dot(delta,ped->Matrix()->forward),Dot(delta,ped->Matrix()->up)};
        prop.rotation=Multiply(inverse,Quat::FromMatrix(*o->Matrix()));state.props.push_back(prop);
    };
    auto*held=Held(ped);if(held){auto*t=anim::FindTask(ped,307);auto bone=Field<uint8_t>(t,0x18);add(held,bone<19?bone:255);}
    if(auto*pool=ObjectPool())for(int i=0;i<pool->size&&state.props.size()<3;++i){
        if(pool->byteMap[i]<0)continue;
        auto*o=(CObject*)(pool->objects+(size_t)i*kObjectPoolElemSize);
        if(o!=held&&Attached(o,ped))add(o,255);
    }
    return state;
}
void DeleteProps(RemoteExtras&e){for(auto&h:e.objects){if(GetObject(h))Cmd(0x0108).I(h).Run();h=-1;}}
void DrawProps(uint8_t id,RemoteExtras&e){
    auto*rp=Players::Instance().Get(id);if(!rp||!rp->ped)return;
    if(e.pedHandle!=rp->pedHandle){DeleteProps(e);e.phone=false;e.pedHandle=rp->pedHandle;}
    if((rp->state.flags&OnFootState::F_DEAD)||NowMs()-e.received>5000){DeleteProps(e);return;}
    if(e.phone!=e.state.phone){Cmd(0x0729).I(rp->pedHandle).I(e.state.phone).Run();e.phone=e.state.phone;}
    // Props replicates the actual held/attached objects, including NPC props.
    // Keep this feature for phone tasks, inventory and reconnect restoration only.

}
using SwimFn=void(__fastcall*)(void*,void*,CPlayerPed*);SwimFn originalSwim=nullptr;
void __fastcall SwimInput(void*task,void*edx,CPlayerPed*ped){
    originalSwim(task,edx,ped);
    if(auto*rp=Players::Instance().FindByPed(ped);rp&&rp->state.motion.kind==MotionState::SWIM&&NowMs()-rp->stateRecvMs<1000){
        Field<uint16_t>(task,0xA)=rp->state.motion.phase;
        Field<float>(task,0x24)=rp->state.motion.pitch;Field<float>(task,0x28)=rp->state.motion.roll;Field<float>(task,0x2C)=rp->state.motion.torsoPitch;Field<float>(task,0x30)=rp->state.motion.swimHeading;
    }
}
CPlayerPed*breathActor=nullptr;float breathDose=0,drowningRemainder=0;int drowningHandle=-1;
using DrownFn=bool(__cdecl*)(CPed*,CEntity*,int,int,int,int);DrownFn originalDrown=nullptr;
bool __cdecl Drown(CPed*victim,CEntity*creator,int weapon,int damage,int piece,int dir){
    if(Net().Online()&&victim==breathActor&&weapon==53){
        int handle=GetPedRef(victim);if(handle!=drowningHandle){drowningHandle=handle;drowningRemainder=0;}
        drowningRemainder+=breathDose;damage=(int)drowningRemainder;drowningRemainder-=damage;
        if(!damage)return true;
    }
    return originalDrown(victim,creator,weapon,damage,piece,dir);
}
using BreathFn=void(__fastcall*)(CPlayerPed*,void*,bool,float);BreathFn originalBreath=nullptr;
void __fastcall Breath(CPlayerPed*p,void*edx,bool decrease,float multiplier){
    if(auto*rp=Players::Instance().FindByPed(p);rp&&rp->hasState){if(p->PlayerData())Field<float>(p->PlayerData(),0x44)=rp->state.breath;return;}
    auto*previous=breathActor;auto previousDose=breathDose;breathActor=p;breathDose=std::max(0.f,TimeStep()*multiplier*3.f);
    if(!decrease||(p->PlayerData()&&Field<float>(p->PlayerData(),0x44)>0))drowningRemainder=0;
    originalBreath(p,edx,decrease,multiplier);breathActor=previous;breathDose=previousDose;
}
using VisionFn=void(__cdecl*)(bool);VisionFn originalNightVision=nullptr,originalInfraredVision=nullptr;
void __cdecl NightVision(bool enabled){originalNightVision(enabled);ObserveLocalStoryVision();}
void __cdecl InfraredVision(bool enabled){originalInfraredVision(enabled);ObserveLocalStoryVision();}
class PlayerExtras:public Module {
public:
 const char*Name()const override{return "PlayerExtras";}
 void OnInstallHooks()override{
    mem::RedirectCallFn(0x60A943,&Drown,&originalDrown);
    mem::HookFn(0x60A8D0,&Breath,&originalBreath,"CPlayerPed::HandlePlayerBreath owner state");
    mem::HookFn(0x688A90,&SwimInput,&originalSwim,"CTaskSimpleSwim::ProcessControlInput");
    mem::HookFn(0x701120,&NightVision,&originalNightVision,"CPostEffects::ScriptNightVisionSwitch local equipment choice");
    mem::HookFn(0x701140,&InfraredVision,&originalInfraredVision,"CPostEffects::ScriptInfraredVisionSwitch local equipment choice");
    Net().SetHandler(proto::G_PLAYER_EXTRAS,[](uint8_t from,Reader&r){
        uint8_t op=r.U8();uint32_t time=r.U32();
        if(op==3){HandleStoryEquipmentEvent(from,r);return;}
        if(op==1){
            PlayerExtrasState state;if(!state.Read(r))return;
            auto&e=extras[from];if(e.received&&(int32_t)(time-e.time)<=0)return;
            e.state=std::move(state);e.time=time;e.received=NowMs();
        }else if(op==2){
            uint8_t target=r.U8();Resume state;state.run=r.U64();state.phase=r.U32();
            if(from!=Net().HostId()||target!=Net().MyId()||!state.state.Read(r))return;
            state.vehicle=r.U16();state.seat=r.I8();
            if(!state.extras.Read(r)||!state.action.Read(r)||state.seat< -2||state.seat>7)return;
            state.refs=Net().EntityContext();pendingResume=std::move(state);restoreAt=NowMs()+750;SetLocalStoryRecoveryPending(true);
            LOGI("Reconnect restore received: vehicle %04X seat %d",pendingResume.vehicle,pendingResume.seat);
        }
    });
 }
 void OnPeerJoin(uint8_t id)override{
    if(!Net().IsHost())return;
    auto it=remembered.find(Net().PeerName(id));
    if(it!=remembered.end()&&NowMs()-it->second.saved<180000)rejoining[id]=it->second;
 }
 void OnPeerResync(uint8_t id)override{
    lastSent=0;lastPacket.clear();
    if(!Net().IsHost())return;
    auto it=rejoining.find(id);if(it==rejoining.end())return;
    auto&resume=it->second;Writer w(proto::G_PLAYER_EXTRAS);w.U8(2);w.U32((uint32_t)NowMs());w.U8(id);w.U64(resume.run);w.U32(resume.phase);
    resume.state.Write(w);w.Entity(ENTITY_VEHICLE,resume.vehicle);w.I8(resume.seat);resume.extras.Write(w);resume.action.Write(w);Net().Send(proto::CH_RELIABLE,w);
    rejoining.erase(it);remembered.erase(Net().PeerName(id));
    LOGI("Reconnect restore sent to player %u",id);
 }
 void OnPeerLeave(uint8_t id)override{auto it=extras.find(id);if(it!=extras.end()){DeleteProps(it->second);extras.erase(it);}rejoining.erase(id);}
 void OnSessionEnd()override{breathActor=nullptr;breathDose=drowningRemainder=0;drowningHandle=-1;for(auto&[id,e]:extras)DeleteProps(e);extras.clear();remembered.clear();rejoining.clear();restoreAt=motionRestoreUntil=0;SetLocalStoryRecoveryPending(false);lastPacket.clear();lastSent=0;}
 void OnGameShutdown()override{breathActor=nullptr;breathDose=drowningRemainder=0;drowningHandle=-1;for(auto&[id,e]:extras){e.objects={-1,-1,-1};e.phone=false;}restoreAt=0;SetLocalStoryRecoveryPending(false);if(Net().IsHost()){remembered.clear();rejoining.clear();}}
 void OnPostProcess()override{
    if(!Net().Online()||!core::IsPlayerReady())return;
    auto now=NowMs();auto*me=FindPlayerPed(0);
    if(restoreAt&&now>=restoreAt&&flow::InHostWorld()&&!Ref<uint8_t>(0xB5F851)){
        auto&resume=pendingResume;if(resume.run&&StoryRunId()==0&&now<restoreAt+5000)return;
        if(resume.run!=StoryRunId()||resume.phase!=CooperativeStoryPhase()||!Net().EntityContextValid(resume.refs)){restoreAt=0;SetLocalStoryRecoveryPending(false);LOGI("Obsolete reconnect action discarded run=%llu current=%llu",(unsigned long long)resume.run,(unsigned long long)StoryRunId());return;}if(!LocalStoryRecoveryReady())return;auto*nv=Vehicles::Instance().Find(resume.vehicle);
        bool forced=LocalStoryRoleMode()>=2;
        bool wait=resume.vehicle&&(!nv||!Vehicles::Alive(*nv))&&now<restoreAt+5000&&!forced;
        if(!wait&&resume.vehicle&&(!nv||!Vehicles::Alive(*nv))&&!forced){restoreAt=0;SetLocalStoryRecoveryPending(false);LOGI("Reconnect restore cancelled: vehicle dependency removed");return;}
        if(!wait){
            restoreAt=0;if(forced)BeginCooperativeInventoryRestore();auto s=resume.state;auto p=s.pos;
            if(nv&&IsVehicleValid(nv->veh))p=nv->veh->GetPosition()+CVector{3,0,1};
            int area=nv&&IsVehicleValid(nv->veh)?nv->veh->AreaCode():s.area;
            if(!forced){Cmd(0x04BB).I(area).Run();Cmd(0x0860).I(GetPedRef(me)).I(area).Run();Cmd(0x03CB).F(p.x).F(p.y).F(p.z).Run();
            Cmd(0x00A1).I(GetPedRef(me)).F(p.x).F(p.y).F(p.z-1).Run();me->Health()=s.health?s.health:100;me->Armour()=s.armour;if(me->PlayerData())Field<float>(me->PlayerData(),0x44)=s.breath;
            me->CurrentRotation()=me->AimingRotation()=s.rotation;}
            Cmd(0x048F).I(GetPedRef(me)).Run();
            for(int i=0;i<13;++i)if(resume.extras.weapons[i]){
                int weapon=resume.extras.weapons[i];
                int model=-1;Cmd(0x0781).I(weapon).Out(&model).Run();
                if(model>0)LoadModelNow(model);
                Cmd(0x01B2).I(GetPedRef(me)).I(weapon).I((int)resume.extras.ammo[i]).Run();
            }
            Cmd(0x01B9).I(GetPedRef(me)).I(s.weapon).Run();
            if(!forced&&nv&&IsVehicleValid(nv->veh)&&resume.seat>=-1){
                CPed*occupied=resume.seat<0?nv->veh->Driver():nv->veh->Passengers()[resume.seat];
                if(!occupied||occupied==me){
                    if(resume.seat<0)Cmd(0x036A).I(GetPedRef(me)).I(nv->handle).Run();
                    else if(resume.seat<nv->veh->MaxPassengers())Cmd(0x0430).I(GetPedRef(me)).I(nv->handle).I(resume.seat).Run();
                }
            }
            if(forced)CompleteCooperativeInventoryRestore();
            if(!forced&&(s.flags&OnFootState::F_JETPACK)&&LoadModelNow(370))Cmd(0x07A7).I(GetPedRef(me)).Run();
            SetLocalStoryRecoveryPending(false);
            if(resume.extras.phone)Cmd(0x0729).I(GetPedRef(me)).I(1).Run();
            resumedMotion=s.motion;motionRestoreUntil=now+1500;
            if(!me->InVehicle()&&s.health&&s.motion.kind==MotionState::NORMAL){
                for(auto&prop:resume.extras.props)if(prop.bone<19){
                    if(LoadModelNow(prop.model))CallMethod<0x5E4390>(me,(int)prop.model,(uint8_t)1);
                    break;
                }
                // A continuing pose can resume; a stale jump/hit/death must be
                // rebuilt by current physics rather than replayed after a load.
                if(resume.action.Present()&&(resume.action.flags&anim::F_LOOPED))anim::Replay(me->RwObject(),resume.action,0);
            }
            LOGI("Reconnect restored: vehicle %04X seat %d weapon %u",resume.vehicle,PassengerSeatOf(nv?nv->veh:nullptr,me),s.weapon);
        }
    }
    if(motionRestoreUntil){
        if(now>motionRestoreUntil||me->InVehicle())motionRestoreUntil=0;
        else if(resumedMotion.kind==MotionState::SWIM){
            if(auto*t=anim::FindTask(me,1304)){
                Field<uint16_t>(t,0xA)=resumedMotion.phase;
                Field<float>(t,0x24)=resumedMotion.pitch;Field<float>(t,0x28)=resumedMotion.roll;Field<float>(t,0x2C)=resumedMotion.torsoPitch;Field<float>(t,0x30)=resumedMotion.swimHeading;
                motionRestoreUntil=0;
            }
        }else motionRestoreUntil=0;
    }
    if(Net().IsHost())for(auto&[id,rp]:Players::Instance().All())if(rp.hasState&&now-rp.stateRecvMs<1500&&!rejoining.count(id)){
        Resume value;value.run=StoryRunId();value.phase=CooperativeStoryPhase();value.state=rp.state;value.vehicle=rp.mode==RemotePlayer::Mode::OnFoot?0:rp.vehNetId;value.seat=rp.mode==RemotePlayer::Mode::OnFoot?-2:rp.seat;
        auto it=extras.find(id);if(it==extras.end()||!it->second.received)continue;
        value.extras=it->second.state;
        if(rp.ped)value.action=anim::Capture(anim::ActionAssociation(rp.ped));
        value.saved=now;remembered[Net().PeerName(id)]=value;
    }
    for(auto it=remembered.begin();it!=remembered.end();)it=now-it->second.saved>180000?remembered.erase(it):std::next(it);
    while(remembered.size()>8){auto oldest=std::min_element(remembered.begin(),remembered.end(),[](auto&a,auto&b){return a.second.saved<b.second.saved;});remembered.erase(oldest);}
    if(now-lastSent>=100&&!Ref<uint8_t>(0xB5F851)){
        auto value=CaptureExtras(me);Writer body;value.Write(body);
        if(body.Buffer()!=lastPacket||now-lastSent>=2000){
            Writer w(proto::G_PLAYER_EXTRAS);w.U8(1);w.U32((uint32_t)now);value.Write(w);Net().Send(proto::CH_RELIABLE,w);
            lastPacket=body.Buffer();lastSent=now;
        }
    }
    for(auto&[id,e]:extras)DrawProps(id,e);
 }
 void OnRender()override{if(core::IsPlayerReady())for(auto&[id,e]:extras)DrawProps(id,e);}
} module;
struct Register{Register(){core::RegisterModule(&module);}} registration;
}
bool IsPlayerHeldObject(game::CObject*object){
    if(!object)return false;
    for(auto&[id,e]:extras)for(int h:e.objects)if(game::GetObject(h)==object)return true;
    auto*me=game::FindPlayerPed(0);if(me&&(Held(me)==object||Attached(object,me)))return true;
    for(auto&[id,rp]:Players::Instance().All())if(rp.ped&&(Held(rp.ped)==object||Attached(object,rp.ped)))return true;
    return false;
}
}
