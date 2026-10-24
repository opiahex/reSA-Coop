// NPC synchronisation. Every game owns the peds it created (random population, mission peds); peds near
// the other player are announced and follow the owner on the other side as "mirrors": script-created
// peds with an empty decision maker and all proofs. What the owner's ped does is replayed with the
// game's own means so it looks native:
//  * walking/running   - "go to point" tasks towards the owner's position;
//  * aiming/shooting   - aim/shoot-at-coord tasks (gun pose, muzzle flash, sound, tracers);
//  * entering/leaving  - enter/leave car tasks (door and seat animations);
//  * anything else     - the owner's current action animation (chat, phone, benches, fights, falls).
#include "entities/ped/Peds.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/profile/Profile.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/mission/Mission.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/lib/sync/PopulationShare.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Anim.h"
#include "shared/game/Script.h"
#include "shared/lib/memory/Memory.h"
#include "shared/lib/sync/WorldResync.h"
#include "shared/api/mission-objects/SharedObject.h"

namespace coop {

using namespace game;
using game::script::Cmd;

Peds& Peds::Instance() {
    static Peds p;
    return p;
}

namespace {
struct Reg {
    Reg() { core::RegisterModule(&Peds::Instance()); }
} g_reg;

// owned peds closer than "in" to the other player are announced and removed there beyond "out";
// vehicle occupants use the vehicle ranges so traffic is never seen without its driver
bool StoryPed(CPed*p){return Net().IsHost()&&p&&p->CreatedBy()==PED_MISSION&&HostReplicating();}
bool PrivateInteriorPed(CPed*p){return p&&p->CreatedBy()==PED_MISSION&&AreaOf(p)!=0&&AreaOf(p)!=13&&!StoryPed(p);}
float SyncIn(CPed* p) { return p->InVehicle() ? 170.0f : 120.0f; }
float SyncOut(CPed* p) { return p->InVehicle() ? 220.0f : 150.0f; }
constexpr int kSpecialFirst = 290, kSpecialLast = 299;
constexpr int kEmptyDecisionMaker = 65542;  // SET_CHAR_DECISION_MAKER template "Empty"
constexpr int TASK_ENTER_CAR_AS_PASSENGER = 700, TASK_ENTER_CAR_AS_DRIVER = 701, TASK_LEAVE_CAR = 704;
constexpr int TASK_SIMPLE_USE_GUN = 1017;
constexpr uint64_t kCarTaskGraceMs = 6000;  // let enter/leave animations finish before warping
constexpr uint16_t kNoAnim = 0xFFFF;
void RequestDrivebyAnimations() {
    static int block=-1;
    if(block<0)block=Call<0x4D3990,int>("drivebys");
    if(block>=0&&!anim::BlockLoaded(block))RequestModel(anim::kIfpBase+block,2);
}

Vec3 V(const CVector& v) { return {v.x, v.y, v.z}; }
CVector G(const Vec3& v) { return {v.x, v.y, v.z}; }

void WriteState(Writer&w,const PedSyncState&s){s.Write(w);}
bool ReadState(Reader&r,PedSyncState&s){return s.Read(r);}

using RequestSpecialFn = void(__cdecl*)(int, const char*, int);
RequestSpecialFn g_origRequestSpecial = nullptr;
// another character into the slot a player's copy wears (a cutscene, a mission, a mirror): CStreaming::
// RequestSpecialModel deletes the slot's model while that copy still has it on. It goes back to CJ first and gets its
// look again.
void MakeWay(int model, const char* name) {
    if (!name || !_stricmp(Peds::Instance().SpecialName(model).c_str(), name)) return;
    Peds::Instance().RespawnCopiesOf(model);
    for (auto& [id, other] : Players::Instance().All())
        if (RemotePlayer* rp = Players::Instance().Get(id); rp && rp->ped && rp->ped->ModelIndex() == model) {
            Players::SetModel(rp->ped, 0);
            rp->lookStale = true;
            LOGI("Player %d's special character makes way for %s", id, name);
        }
}

void __cdecl HookRequestSpecialModel(int model, const char* name, int flags) {
    MakeWay(model, name);
    if (name) Peds::Instance().RememberSpecialModel(model, name);
    g_origRequestSpecial(model, name, flags);
}

uint8_t LoadState(int model) { return Ref<uint8_t>(0x8E4CC0 + (uintptr_t)model * 0x14 + 0x10); }  // CStreaming::ms_aInfoForModel

uint8_t Clamp8(float v) { return (uint8_t)std::clamp(v, 0.0f, 255.0f); }

bool IsPedDead(CPed* p) {
    int st = p->PedState();
    return p->Health() <= 0.0f || st == 54 /*PEDSTATE_DIE*/ || st == 55 /*PEDSTATE_DEAD*/;
}

bool IsAlive(const NetPed& np) { return np.ped && IsPedValid(np.ped) && GetPedRef(np.ped) == np.handle; }

void HoldCorpseOutside(NetPed&np,bool hold){
    if(!IsAlive(np))return;auto*p=np.ped;
    if(hold){
        if(!np.awayHeld){np.awayHeld=true;np.awayPhysicsMask=(~p->PhysicalFlags())&0x2000u;np.awayGravity=(p->PhysicalFlags()&2)!=0;}
        p->PhysicalFlags()=(p->PhysicalFlags()|np.awayPhysicsMask)&~2u;p->MoveSpeed()={};p->TurnSpeed()={};
    }else if(np.awayHeld){p->PhysicalFlags()&=~np.awayPhysicsMask;if(np.awayGravity)p->PhysicalFlags()|=2;np.awayHeld=false;np.awayPhysicsMask=0;}
}

// CTaskManager::FindActiveTaskByType: searches the active primary chain and the secondary tasks
void* FindTask(CPed* ped, int type) {
    void* intel = ped->Intelligence();
    return intel ? CallMethod<0x681740, void*>((uint8_t*)intel + 4, type) : nullptr;
}

// The action animation the ped plays: anything but its own walk/run/idle set (CPed::m_nAnimGroup).
// Full-body animations while moving are turns and start/stop steps, also locomotion. A partial one (a punch, a guard)
// plays over a full-body one (the fight stance) and is the action while it lasts: the mirror keeps the stance under it.
struct HandContact{int handle=-1,target=-1;uint64_t at=0;uint8_t arm=255;int16_t bone=0;CVector offset;float speed=.25f;int blend=250;};
std::map<uint16_t,HandContact>handContacts;
void* CoupleTask(CPed*ped){
    if(!ped||!ped->Intelligence())return nullptr;
    auto*manager=(uint8_t*)ped->Intelligence()+4;
    for(int slot=0;slot<5;++slot)if(auto*task=CallMethod<0x6817D0,void*>(manager,slot,1215))return task;
    return nullptr;
}
using PointArmFn=void(__fastcall*)(void*,void*,const char*,int,CPed*,CEntity*,int,CVector*,float,int,float);
PointArmFn originalPointArm=nullptr;
void __fastcall PointArm(void*manager,void*edx,const char*purpose,int arm,CPed*ped,CEntity*target,int bone,CVector*offset,float speed,int blend,float distance){
    originalPointArm(manager,edx,purpose,arm,ped,target,bone,offset,speed,blend,distance);
    if(!Net().Online()||arm<0||arm>1)return;
    auto*couple=CoupleTask(ped);if(!couple)return;
    auto*partner=target&&target->IsPed()?(CPed*)target:!target&&bone==-1?Field<CPed*>(couple,0x10):nullptr;
    if(!partner||!IsPedValid(partner))return;
    auto*n=Peds::Instance().Find(ped);if(!n||n->owner!=Net().MyId())return;
    CVector point=offset?*offset:CVector{};
    if(!target){auto d=point-partner->GetPosition();float h=partner->CurrentRotation();point={d.x*std::cos(h)+d.y*std::sin(h),-d.x*std::sin(h)+d.y*std::cos(h),d.z};}
    handContacts[n->netId]={GetPedRef(ped),GetPedRef(partner),NowMs(),(uint8_t)arm,(int16_t)bone,point,speed,blend};
}

void CaptureInteraction(NetPed&np,PedSyncState&s){
    auto*ped=np.ped;
    auto&out=s.interaction;
    void*leaf=nullptr;CPed*partner=nullptr;
    if(auto*intel=ped->Intelligence()){
        void*manager=(uint8_t*)intel+4;void*task=CallMethod<0x681720,void*>(manager);
        for(int i=0;task&&i<12;++i){
            auto type=(uint16_t)CallVirtual<4,int>(task);if(type>=4096)break;
            out.chain[out.depth++]=type;if(!i)out.task=type;out.leaf=type;leaf=task;
            if(type==244)out.sequenceStep=(int8_t)std::clamp(Field<int32_t>(task,0xC),-1,7);
            if(type==275){out.sequenceStep=(int8_t)std::clamp(Field<int32_t>(task,0x10),-1,7);out.sequenceRepeat=(uint16_t)std::clamp(Field<int32_t>(task,0x18),0,65535);}
            if(type==256||type==1202||type==1203||type==1204||type==1209)partner=Field<CPed*>(task,0x38);
            if(type==1215){partner=Field<CPed*>(task,0x10);out.couple=true;}
            if(type==1027)partner=Field<CPed*>(task,0xC);
            if(type==1100)partner=Field<CPed*>(task,8);
            task=CallVirtual<2,void*>(task);
        }
    }
    // Temporary obstacle avoidance can pre-empt the couple's primary task
    // without ending the relationship or its IK contact.
    if(!partner)if(auto*couple=CoupleTask(ped)){partner=Field<CPed*>(couple,0x10);out.couple=true;}
    // Story handshakes also use two independently scripted clips, without
    // a native Partner task. Bind only a unique complementary, phase-matched
    // pair; unrelated nearby actors keep their ordinary world-space motion.
    if(!partner&&s.animBlock!=kNoAnim){
        int block=Call<0x4D3990,int>("gangs");
        if(block>=0&&anim::BlockLoaded(block)){
            void*left=Call<0x4D42F0,void*>("hndshkca",(void*)anim::Block(block));void*right=Call<0x4D42F0,void*>("hndshkcb",(void*)anim::Block(block));
            auto*own=anim::Resolve(s.animBlock,s.animIdx);void*wanted=own==left?right:own==right?left:nullptr;
            int candidates=0;
            if(wanted)for(auto&[id,other]:Peds::Instance().All()){
                if(other.ped==ped||other.owner!=np.owner||!IsAlive(other)||other.ped->Health()<=0||AreaOf(other.ped)!=AreaOf(ped)||(other.ped->GetPosition()-ped->GetPosition()).Length()>2.5f)continue;
                for(void*a=anim::First(other.ped->RwObject());a;a=anim::Next(a))if(anim::Hierarchy(a)==wanted&&anim::BlendAmount(a)>.1f&&anim::BlendDelta(a)>=0&&std::fabs(anim::CurrentTime(a)-s.animTime)<.35f){partner=other.ped;++candidates;break;}
            }
            if(candidates!=1)partner=nullptr;
        }
    }
    if(partner&&partner!=ped&&IsPedValid(partner)){
        if(out.couple)if(auto i=handContacts.find(np.netId);i!=handContacts.end()&&i->second.handle==np.handle&&i->second.target==GetPedRef(partner)&&CallStd<0x6182B0,bool>((int)i->second.arm,ped)){
            auto&h=i->second;out.handArm=h.arm;out.handBone=h.bone;out.handOffset=h.offset;out.handSpeed=h.speed;out.handBlend=h.blend;}
        if(partner==FindPlayerPed(0)){out.partnerKind=2;out.partnerId=Net().MyId();}
        else if(auto*rp=Players::Instance().FindByPed(partner)){out.partnerKind=2;out.partnerId=rp->id;}
        else if(auto*n=Peds::Instance().EnsureNetworked(partner)){out.partnerKind=1;out.partnerId=n->netId;}
        if(out.partnerKind){auto delta=ped->GetPosition()-partner->GetPosition();float h=partner->CurrentRotation();
            out.partnerOffset={delta.x*std::cos(h)+delta.y*std::sin(h),-delta.x*std::sin(h)+delta.y*std::cos(h),delta.z};
            out.partnerHeading=std::remainder(ped->CurrentRotation()-h,6.2831853f);
            if(out.partnerOffset.Length()>30)out.partnerKind=0;
        }
    }
    if(auto*parent=ped->AttachedTo();parent&&parent!=ped){
        if(parent->IsVehicle()){if(auto*n=Vehicles::Instance().EnsureNetworked((CVehicle*)parent)){out.parentKind=1;out.parentId=n->netId;}}
        else if(parent->IsPed()){
            if(parent==FindPlayerPed(0)){out.parentKind=3;out.parentId=Net().MyId();}
            else if(auto*n=Players::Instance().FindByPed((CPed*)parent)){out.parentKind=3;out.parentId=n->id;}
            else if(auto*n=Peds::Instance().EnsureNetworked((CPed*)parent)){out.parentKind=2;out.parentId=n->netId;}
        }else if(parent->IsObject()){if(auto id=SharedMissionObjectId((CObject*)parent)){out.parentKind=4;out.parentId=id;}}
        if(out.parentKind){
            // CPed::AttachPedToEntity uses CPed's turret offset, distinct from
            // CPhysical::AttachEntityToEntity's offset/quaternion.
            out.turret=true;out.offset=Field<CVector>(ped,0x76C);
            out.turretAngle=Field<uint16_t>(ped,0x778);out.turretRange=Field<float>(ped,0x77C);out.turretMode=Field<float>(ped,0x780);
        }
    }
    void* candidates[PedInteraction::LayerCount]{};float weights[PedInteraction::LayerCount]{};
    int gait=Field<int32_t>(ped,0x4D4);
    for(void*a=anim::First(ped->RwObject());a;a=anim::Next(a)){
        if(anim::BlendDelta(a)<0||anim::BlendAmount(a)<.02f||(anim::Flags(a)&anim::F_INDESTRUCTIBLE)||
           (anim::Group(a)==gait&&((anim::Flags(a)&0x20)||(anim::Id(a)>=0&&anim::Id(a)<=10))))continue;
        auto pose=anim::Capture(a);if(!pose.Present()||pose.Same(s.motion)||(pose.block==s.animBlock&&pose.index==s.animIdx))continue;
        for(int i=0;i<PedInteraction::LayerCount;++i)if(anim::BlendAmount(a)>weights[i]){
            for(int j=PedInteraction::LayerCount-1;j>i;--j){weights[j]=weights[j-1];candidates[j]=candidates[j-1];}
            weights[i]=anim::BlendAmount(a);candidates[i]=a;break;
        }
    }
    for(int i=0;i<PedInteraction::LayerCount;++i)if(candidates[i]){out.layers[i]=anim::Capture(candidates[i]);out.weights[i]=std::clamp(weights[i],0.f,1.f);}
    out.leader=CooperativeAllyLeader(ped,np.state.interaction.leader);
    uint32_t signature=2166136261u;auto mix=[&](uint32_t value){signature=(signature^value)*16777619u;};
    mix(out.leader);for(unsigned i=0;i<out.depth;++i)mix(out.chain[i]);mix((uint8_t)out.sequenceStep);mix(out.sequenceRepeat);mix(out.partnerKind);mix(out.partnerId);mix(s.animBlock);mix(s.animIdx);
    for(auto&layer:out.layers){mix(layer.block);mix(layer.index);}
    bool restart=np.actionBlock==s.animBlock&&np.actionIndex==s.animIdx&&s.animTime+.15f<np.actionTime;
    np.actionRevision=std::max(np.actionRevision,np.state.interaction.revision);
    if(!np.actionRevision||np.actionSignature!=signature||np.actionLeaf!=(uintptr_t)leaf||restart){if(++np.actionRevision==0)++np.actionRevision;np.actionSignature=signature;np.actionLeaf=(uintptr_t)leaf;}
    np.actionBlock=s.animBlock;np.actionIndex=s.animIdx;np.actionTime=s.animTime;out.revision=np.actionRevision;
}
bool ApplyInteraction(NetPed&np,float age){
    auto*ped=np.ped;const auto&state=np.state.interaction;bool wasExtra=np.extraFull;np.extraFull=false;np.interactionPending=false;
    if(np.handArm<2&&(state.handArm!=np.handArm||!state.couple||!state.partnerKind))CallStd<0x6182F0>((int)np.handArm,ped,250);
    np.handArm=state.handArm;
    if(state.couple&&state.partnerKind==1){auto*partner=GetPed(Peds::Instance().HandleOf(state.partnerId));
        if(partner&&IsPedValid(partner)&&Net().EntityLink(ENTITY_PED,np.netId,ENTITY_PED,state.partnerId)&&partner->Health()>0&&(partner->GetPosition()-ped->GetPosition()).Length()<4){
            if(state.handArm<2){auto offset=state.handOffset;CEntity*target=(CEntity*)partner;
                if(state.handBone==-1){float h=partner->CurrentRotation();offset=partner->GetPosition()+CVector{offset.x*std::cos(h)-offset.y*std::sin(h),offset.x*std::sin(h)+offset.y*std::cos(h),offset.z};target=nullptr;}
                originalPointArm((void*)0xC15448,nullptr,"coop couple",state.handArm,ped,target,(int)state.handBone,&offset,state.handSpeed,state.handBlend,999.f);}
            if(np.state.motion.Present())anim::Replay(ped->RwObject(),np.state.motion,age);
        }}
    if(np.state.motion.Present()&&np.state.animBlock!=kNoAnim&&(np.state.animFlags&anim::F_PARTIAL))
        anim::Replay(ped->RwObject(),np.state.motion,age);
    for(int i=0;i<PedInteraction::LayerCount;++i){
        auto&old=np.extraLayers[i];const auto&pose=state.layers[i];
        bool retained=false;for(auto&layer:state.layers)if(layer.Present()&&layer.Same(old))retained=true;
        if(old.Present()&&!retained)if(auto*a=anim::Find(ped->RwObject(),old))anim::FadeOut(a);
        if(pose.Present())if(auto*a=anim::Replay(ped->RwObject(),pose,age)){
            if(std::fabs(anim::BlendAmount(a)-state.weights[i])>.1f)anim::BlendAmount(a)=state.weights[i];
            if(!(pose.flags&anim::F_PARTIAL))np.extraFull=true;
        }
        old=pose;
    }
    if(wasExtra&&!np.extraFull&&!np.animFull)anim::RestoreMovement(ped);
    if(ped->RwObject()&&!anim::First(ped->RwObject()))anim::RestoreMovement(ped);
    CPhysical*parent=nullptr;
    if(state.parentKind==1){auto*n=Vehicles::Instance().Find(state.parentId);if(n&&Vehicles::Alive(*n))parent=n->veh;}
    else if(state.parentKind==2)parent=GetPed(Peds::Instance().HandleOf(state.parentId));
    else if(state.parentKind==3){if(state.parentId==Net().MyId())parent=FindPlayerPed(0);else if(auto*p=Players::Instance().Get((uint8_t)state.parentId))parent=p->ped;}
    else if(state.parentKind==4)parent=SharedMissionObjectById(state.parentId);
    if(state.parentKind&&state.parentKind!=3){uint8_t kind=state.parentKind==1?ENTITY_VEHICLE:state.parentKind==2?ENTITY_PED:ENTITY_OBJECT;
        if(!Net().EntityLink(ENTITY_PED,np.netId,kind,state.parentId)){parent=nullptr;if(np.interactionAttached&&ped->AttachedTo()){CallMethod<0x5E7EC0>(ped);np.interactionAttached=false;}}
    }
    if(parent&&parent!=ped){
        if(ped->AttachedTo()!=parent){
            if(np.interactionAttached&&ped->AttachedTo())CallMethod<0x5E7EC0>(ped);
            if(state.turret)CallMethod<0x5E7CB0,CEntity*>(ped,parent,state.offset,state.turretAngle,state.turretRange,(int)np.state.weapon);
            else{auto offset=state.offset;auto rotation=state.rotation;CallMethod<0x54D690>(ped,parent,&offset,&rotation);}
        }
        if(state.turret){Field<CVector>(ped,0x76C)=state.offset;Field<uint16_t>(ped,0x778)=state.turretAngle;Field<float>(ped,0x77C)=state.turretRange;Field<float>(ped,0x780)=state.turretMode;CallMethod<0x5DFDF0>(ped);}
        else{Field<CVector>(ped,0x100)=state.offset;Field<CVector>(ped,0x10C)=state.euler;Field<Quat>(ped,0x118)=state.rotation;}
        np.interactionAttached=true;
        return true;
    }
    if(!state.parentKind&&np.interactionAttached){if(ped->AttachedTo())CallMethod<0x5E7EC0>(ped);np.interactionAttached=false;}
    if(state.parentKind){np.interactionPending=true;ped->Flags()&=~129u;}
    if(!state.parentKind&&state.partnerKind&&!(np.state.flags&(PedSyncState::F_DEAD|PedSyncState::F_IN_VEHICLE))){
        CPed*partner=nullptr;CVector point,velocity;float heading=0;bool fresh=false;
        if(state.partnerKind==1&&Net().EntityLink(ENTITY_PED,np.netId,ENTITY_PED,state.partnerId)){
            auto*other=Peds::Instance().Find(GetPed(Peds::Instance().HandleOf(state.partnerId)));
            // One canonical anchor prevents a pair from correcting each other
            // in a feedback loop. Use owner samples, never the rendered copy.
            if(other&&other->hasState&&other->owner==np.owner&&state.partnerId<np.netId&&!(other->state.flags&PedSyncState::F_DEAD)&&
               std::abs((int32_t)(other->state.time-np.state.time))<=250){partner=other->ped;point=state.couple?partner->GetPosition():other->state.pos;velocity=other->state.vel;heading=state.couple?partner->CurrentRotation():other->state.heading;fresh=NowMs()-other->recvMs<1000;}
        }else if(state.partnerKind==2){
            if(state.partnerId==Net().MyId()){partner=FindPlayerPed(0);if(partner){point=partner->GetPosition();heading=partner->CurrentRotation();fresh=true;}}
            else if(auto*rp=Players::Instance().Get((uint8_t)state.partnerId);rp&&rp->hasState&&rp->ped){partner=rp->ped;point=rp->state.pos;velocity=rp->state.vel;heading=rp->state.rotation;fresh=NowMs()-rp->stateRecvMs<1000;}
        }
        bool action=state.couple||np.state.animBlock!=kNoAnim;for(auto&layer:state.layers)action|=layer.Present();
        if(partner&&fresh&&action&&partner->Health()>0&&AreaOf(partner)==np.state.area&&state.partnerOffset.Length()<3.5f){
            if(state.partnerKind==2){
                // The local native attacker moves toward this victim. Moving
                // the victim back relative to that attacker creates a chase
                // feedback loop. Its owner position is the canonical anchor.
                SetEntityPosition(ped,np.state.pos+np.state.vel*age);
                ped->CurrentRotation()=ped->AimingRotation()=np.state.heading;
                ped->MoveSpeed()=np.state.vel*(1.f/50.f);ped->Flags()&=~1u;return true;
            }
            auto offset=state.partnerOffset;
            if(state.couple){if(np.pairAnchor!=state.partnerId){np.pairOffset=offset;np.pairAnchor=state.partnerId;}else np.pairOffset=np.pairOffset+(offset-np.pairOffset)*std::min(1.f,TimeStep()*.2f);offset=np.pairOffset;}
            float c=std::cos(heading),s=std::sin(heading);auto target=point+CVector{c*offset.x-s*offset.y,s*offset.x+c*offset.y,offset.z};
            // Do not reattach a grabbed actor after the authoritative contact
            // has ended or if the two snapshots are from different locations.
            if((target-np.state.pos).Length()<1.5f){SetEntityPosition(ped,target);ped->CurrentRotation()=ped->AimingRotation()=heading+state.partnerHeading;ped->MoveSpeed()=velocity*(1.f/50.f);ped->Flags()&=~1u;return true;}
        }
    }
    return state.parentKind!=0; // keep a pending binding until its entity appears
}

void CaptureAnim(CPed* ped, PedSyncState& s) {
    int motion = Field<int32_t>(ped, 0x4D4);
    bool still = ped->MoveState() <= PEDMOVE_STILL;
    void* scripted = anim::ActionAssociation(ped);
    void* best = scripted;
    bool bestPartial = false;
    for (void* a = anim::First(ped->RwObject()); a; a = anim::Next(a)) {
        if (scripted) break;
        if (anim::BlendAmount(a) < 0.3f || anim::BlendDelta(a) < 0.0f || anim::Group(a) == motion) continue;
        if (anim::Flags(a) & anim::F_INDESTRUCTIBLE) continue;  // the face's own (talk, gum: ANIMATION_FACIAL), not an action
        bool partial = (anim::Flags(a) & anim::F_PARTIAL) != 0;
        if (!partial && !still) continue;
        if (!best || partial > bestPartial || (partial == bestPartial && anim::BlendAmount(a) > anim::BlendAmount(best)))
            best = a, bestPartial = partial;
    }
    int block, idx;
    if (!best || !anim::Locate(anim::Hierarchy(best), block, idx)) return;
    s.animBlock = (uint16_t)block;
    s.animIdx = (uint16_t)idx;
    s.animFlags = anim::Flags(best);
    s.animTime = anim::CurrentTime(best);
    s.animSpeed = anim::Speed(best);
}

// Bound actions and death return before the ordinary action replay below.
// Preserve the same authoritative clip/clock on those paths as well.
void ReplayBoundAction(NetPed&np,float age){
    auto&st=np.state;auto*ped=np.ped;
    if(np.animBlock!=kNoAnim&&(np.animBlock!=st.animBlock||np.animIdx!=st.animIdx)){
        AnimationState old;old.block=np.animBlock;old.index=np.animIdx;
        bool retained=false;for(auto&layer:st.interaction.layers)retained|=old.Same(layer);
        if(!retained)if(auto*a=anim::Find(ped->RwObject(),old))anim::FadeOut(a);
    }
    np.animBlock=st.animBlock;np.animIdx=st.animIdx;np.animTime=st.animTime;
    // Some native contact clips carry PARTIAL but their task still owns the
    // actor's movement (knife victim, handshakes); do not restart locomotion.
    np.animFull=st.animBlock!=kNoAnim&&(!(st.animFlags&anim::F_PARTIAL)||st.interaction.partnerKind);
    if(np.animFull){np.baseBlock=st.animBlock;np.baseIdx=st.animIdx;}
    else np.baseBlock=kNoAnim;
    if(st.animBlock!=kNoAnim){
        AnimationState pose;pose.block=st.animBlock;pose.index=st.animIdx;pose.flags=st.animFlags;pose.time=st.animTime;pose.speed=st.animSpeed;
        anim::Replay(ped->RwObject(),pose,age);
        if(st.interaction.partnerKind||(st.flags&PedSyncState::F_DEAD)){
            for(void*a=anim::First(ped->RwObject());a;a=anim::Next(a)){
                auto present=anim::Capture(a);bool retained=pose.Same(present)||st.motion.Same(present);
                for(auto&layer:st.interaction.layers)retained|=layer.Same(present);
                if(!st.motion.Present()&&anim::Group(a)==Field<int32_t>(ped,0x4D4)&&anim::Id(a)>=0&&anim::Id(a)<=10)retained=true;
                if(!retained&&!(anim::Flags(a)&anim::F_INDESTRUCTIBLE))anim::FadeOut(a);
            }
        }
    }
}

void AlignContactHeading(NetPed&np,float heading){
    // CurrentRotation alone leaves a former passenger's render matrix at its
    // old car heading. The native setter updates both the ped and RW matrix.
    if(std::fabs(std::remainder(np.ped->GetHeading()-heading,6.2831853f))>.001f)
        Cmd(0x0173).I(np.handle).F(heading*57.2957795f).Run();
    np.ped->CurrentRotation()=np.ped->AimingRotation()=heading;
}

void* FindPlaying(CPed* ped, int block, int idx) {
    int b, i;
    for (void* a = anim::First(ped->RwObject()); a; a = anim::Next(a))
        if (anim::Locate(anim::Hierarchy(a), b, i) && b == block && i == idx) return a;
    return nullptr;
}

// Every task a script gives takes one of 128 records (CPedScriptedTaskRecord::ms_scriptedTasks) until the ped is
// done with it. With all of them taken the game writes the new one in front of the array and crashes later. When
// that happens, finished records are let go first; failing that, one record goes (a script may then see that ped's
// task as over) and the log tells which commands hold them.
constexpr uintptr_t kTaskRecords = 0xC0B1E8;  // 20 bytes each: opcode, event, task, ped, status
using VacantSlotFn = int(__cdecl*)();
VacantSlotFn g_origVacantSlot = nullptr;
int __cdecl HookGetVacantSlot() {
    int i = g_origVacantSlot();
    if (i >= 0) return i;
    Call<0x6085E0>();  // CPedScriptedTaskRecord::Process
    if ((i = g_origVacantSlot()) >= 0) return i;
    static uint64_t loggedAt = 0;
    if (NowMs() - loggedAt > 10000) {
        loggedAt = NowMs();
        std::map<int, int> byOpcode;
        int mirrors = 0;
        for (int k = 0; k < 128; ++k) {
            uintptr_t r = kTaskRecords + (uintptr_t)k * 20;
            ++byOpcode[Ref<int32_t>(r)];
            if (CPed* p = Ref<CPed*>(r + 0x0C); p && Peds::Instance().Find(p) && Peds::Instance().Find(p)->owner != Net().MyId()) ++mirrors;
        }
        std::string s;
        for (auto& [op, n] : byOpcode) s += Format(" %04X:%d", op & 0xFFFF, n);
        LOGW("All 128 scripted task records are taken (%d by mirrored peds), by opcode:%s", mirrors, s.c_str());
    }
    CallMethod<0x608350>((void*)kTaskRecords);  // CPedScriptedTaskRecordData::Flush
    return 0;
}

// A task given by script that the ped has not taken up yet: its event waits (a ped this game does not process now,
// out where its collision is not loaded). Another one would only take another record.
bool TaskPending(CPed* ped) {
    for (int k = 0; k < 128; ++k) {
        uintptr_t r = kTaskRecords + (uintptr_t)k * 20;
        if (Ref<CPed*>(r + 0x0C) == ped && Ref<void*>(r + 0x04)) return true;  // m_ped, m_event
    }
    return false;
}
// A mirror replays its owner's full-body action (a fight stance) with no task behind it, so the game's own move
// animation (CPed::SetMoveAnim) blends its walk or idle over it whenever its move state changes: the stance faded
// and started over after every punch. While such an action holds the mirror, its move animation stays as it is.
using SetMoveAnimFn = void(__fastcall*)(CPed*, void*);
SetMoveAnimFn g_origSetMoveAnim = nullptr;
void __fastcall HookSetMoveAnim(CPed* ped, void* edx) {
    if (NetPed* np = Peds::Instance().Find(ped); np && np->owner != Net().MyId() && (np->animFull||np->extraFull)) return;
    if (RemotePlayer* rp = Players::Instance().FindByPed(ped); rp && rp->scriptAnimFull) return;
    g_origSetMoveAnim(ped, edx);
}
}  // namespace

// The people an interior brings (InteriorGroup_c::SetupPeds: a police station's officers, a shop's customers) keep
// area 0 inside: they count as in the interior of a player near them, ours or the other one's (the owner who walked
// out took them out with it: they vanished for the player who stayed inside, and never went to its game)
int8_t AreaOf(CPed* ped) {
    int8_t area=ped->AreaCode();
    if(area!=0)return area;
    auto p=ped->GetPosition();auto*me=FindPlayerPed(0);
    if(me&&me->AreaCode()!=0&&me->AreaCode()!=13&&(p-me->GetPosition()).Length()<80)return me->AreaCode();
    for(auto&[id,rp]:Players::Instance().All())
        if(rp.hasState&&NowMs()-rp.stateRecvMs<1500&&rp.state.area!=0&&rp.state.area!=13&&(p-rp.state.pos).Length()<80)return rp.state.area;
    return 0;
}

void Peds::RememberSpecialModel(int model, const char* name) {
    if (model >= kSpecialFirst && model <= kSpecialLast) m_specialNames[model] = name;
}

std::string Peds::SpecialName(int model) const {
    auto it = m_specialNames.find(model);
    return it == m_specialNames.end() ? std::string() : it->second;
}

int Peds::SpecialModelFor(const std::string& name) {
    if (name.empty()) return -1;
    for (int m = kSpecialFirst; m <= kSpecialLast; ++m)
        if (!_stricmp(SpecialName(m).c_str(), name.c_str()) && LoadState(m) != 0) return IsModelLoaded(m) ? m : -1;
    // a name this game's archives lack (another player's mods): RequestSpecialModel takes the garbage position and size
    // of a failed CDirectory::FindItem (0x409F76) and the streamer crashed on it
    void* dir = Ref<void*>(0x8E48D0);  // CStreaming::ms_pExtraObjectsDir
    if (!dir || !CallMethod<0x532450, void*>(dir, name.c_str())) return -1;  // CDirectory::FindItem
    for (int m = kSpecialLast; m >= kSpecialFirst; --m)
        if (LoadState(m) == 0) {  // nothing in it, nothing asked for
            g_origRequestSpecial(m, name.c_str(), 2);  // STREAMING_GAME_REQUIRED: kept while the copy wears it
            m_specialNames[m] = name;
            LOGI("Special character %s asked for in slot %d", name.c_str(), m);
            return -1;
        }
    return -1;
}

void Peds::OnInstallHooks() {
    mem::HookFn(0x618B60,&PointArm,&originalPointArm,"IKChainManager::PointArm couple contact");
    mem::HookFn(0x409D10, &HookRequestSpecialModel, &g_origRequestSpecial, "CStreaming::RequestSpecialModel");
    mem::HookFn(0x608540, &HookGetVacantSlot, &g_origVacantSlot, "CPedScriptedTaskRecord::GetVacantSlot");
    mem::HookFn(0x5E4A00, &HookSetMoveAnim, &g_origSetMoveAnim, "CPed::SetMoveAnim");
    Net().SetHandler(proto::G_PED_CREATE, [](uint8_t from, Reader& r) { Instance().HandleCreate(from, r); });
    Net().SetHandler(proto::G_PED_REMOVE, [](uint8_t from, Reader& r) { Instance().HandleRemove(from, r); });
    Net().SetHandler(proto::G_PED_ACTION,[](uint8_t from,Reader&r){Instance().HandleAction(from,r);});
    Net().SetHandler(proto::G_PED_SYNC, [](uint8_t from, Reader& r) { Instance().HandleSync(from, r); });
    Net().SetHandler(proto::G_PED_OWNER, [](uint8_t from, Reader& r) { Instance().HandleOwner(from, r); });
    Net().SetHandler(proto::G_RESYNC, [](uint8_t from, Reader& r) {
        WorldResync resync;
        if (!resync.Read(r)) return;
        if (!core::IsPlayerReady()) return;
        // its game started over: what it owned before is gone there
        Instance().DropMirrorsOf(from, false);
        Vehicles::Instance().ForgetOwnedBy(from, resync.vehicles);
        Vehicles::Instance().Reannounce();  // vehicles first: peds inside them need the vehicle
        Instance().Reannounce();
        core::PeerResynced(from);
    });
}

NetPed* Peds::Find(CPed* ped) {
    auto it = m_byPtr.find(ped);
    if (it == m_byPtr.end()) return nullptr;
    auto it2 = m_byId.find(it->second);
    return it2==m_byId.end()||it2->second.ped!=ped||!IsAlive(it2->second)?nullptr:&it2->second;
}

CPed* Peds::FindOwned(uint16_t netId) {
    auto it = m_byId.find(netId);
    return it != m_byId.end() && it->second.owner == Net().MyId() && IsAlive(it->second) ? it->second.ped : nullptr;
}

NetPed* Peds::EnsureNetworked(CPed* ped) {
    if(!Net().Online()||!ped||!IsPedValid(ped)||ped->IsPlayer()||Players::Instance().FindByPed(ped))return nullptr;
    if(auto*np=Find(ped))return np;
    uint16_t id=m_ids.Take((uint16_t)((Net().MyId()+1)<<12),NowMs(),[&](uint16_t v){return m_byId.count(v)!=0;});if(!id)return nullptr;
    auto&n=m_byId[id];n.netId=id;n.ped=ped;n.handle=GetPedRef(ped);n.owner=Net().MyId();n.model=ped->ModelIndex();n.pedType=(uint8_t)ped->PedType();
    if(auto it=m_specialNames.find(n.model);it!=m_specialNames.end())n.special=it->second;
    m_byPtr[ped]=id;Net().BeginEntity(ENTITY_PED,id);Announce(n);return &n;
}

int Peds::HandleOf(uint16_t netId) {
    auto it = m_byId.find(netId);
    return it != m_byId.end() && IsAlive(it->second) ? it->second.handle : -1;
}

PedSyncState Peds::Capture(NetPed& np) {
    CPed* ped = np.ped;
    uint64_t now = NowMs();
    PedSyncState s;
    s.time = (uint32_t)now;
    s.pos = ped->GetPosition();
    s.vel = ped->MoveSpeed() * 50.0f;
    s.heading = ped->CurrentRotation();
    s.moveState = (uint8_t)std::clamp(ped->MoveState(), 0, 7);
    CaptureAnim(ped,s);
    void* gait = nullptr;
    int motionGroup = Field<int32_t>(ped, 0x4D4);
    for (void* a = anim::First(ped->RwObject()); a; a = anim::Next(a)) {
        bool baseIdle=s.animBlock!=kNoAnim&&(s.animFlags&anim::F_PARTIAL)&&!(anim::Flags(a)&anim::F_PARTIAL)&&anim::Id(a)>=0&&anim::Id(a)<=10;
        if (anim::Group(a) != motionGroup || (!(anim::Flags(a) & 0x20)&&!baseIdle) || anim::BlendDelta(a) < 0) continue;
        if (!gait || anim::BlendAmount(a) > anim::BlendAmount(gait)) gait = a;
    }
    s.motion = anim::Capture(gait);
    s.health = std::clamp(ped->Health(),0.f,1000000.f);
    s.armour = std::clamp(ped->Armour(),0.f,1000000.f);
    s.weapon = (uint8_t)ped->ActiveWeapon().type;
    s.area = AreaOf(ped);
    s.entityFlags=(ped->IsVisible()?1:0)|((ped->Flags()&1u)?2:0)|((ped->PhysicalFlags()&0x2000u)?4:0)|((Field<uint8_t>(ped,0x475)&1)?8:0)|(StoryPed(ped)?16:0);
    if(s.entityFlags&4)s.vel={}; // native frozen peds can retain a stale falling velocity
    s.flags = (IsPedDead(ped) ? PedSyncState::F_DEAD : 0) | (ped->IsDucking() ? PedSyncState::F_DUCKING : 0);

    CaptureInteraction(np,s);
    if (CVehicle* v = ped->VehicleIfIn()) {
        // Simple traffic can leave the occupant's matrix behind. Keep its
        // stream position/area tied to the vehicle until the occupant exits.
        s.pos=v->GetPosition();s.area=v->AreaCode();s.vel=v->MoveSpeed()*50.0f;
        if (NetVehicle* nv = Vehicles::Instance().EnsureNetworked(v)) {
            s.flags |= PedSyncState::F_IN_VEHICLE;
            s.vehNetId = nv->netId;
            int seat=PassengerSeatOf(v,ped);
            if(seat < -1){s.flags&=~PedSyncState::F_IN_VEHICLE;s.seat=-1;}else s.seat=(int8_t)seat;
            if (FindTask(ped, TASK_LEAVE_CAR)) s.flags |= PedSyncState::F_LEAVING;
        }
        if (void* drive = FindTask(ped, 1022)) {
            RequestDrivebyAnimations();
            CEntity* target = Field<CEntity*>(drive, 0x34);
            CVector point = target ? target->GetPosition() : Field<CVector>(drive, 0x38);
            s.flags |= PedSyncState::F_AIMING | PedSyncState::F_DRIVEBY;
            s.aim = point - s.pos;
            s.drivebyStyle = std::min<uint8_t>(Field<uint8_t>(drive,0x12),8);
            s.drivebyFrequency = std::min<uint8_t>(Field<uint8_t>(drive,0x13),100);
            if (target && target->IsPed()) {
                if (target == FindPlayerPed(0)) { s.aimKind=1; s.aimId=Net().MyId(); }
                else if (auto* player=Players::Instance().FindByPed((CPed*)target)) { s.aimKind=1; s.aimId=player->id; }
                else if (auto* npc=Find((CPed*)target)) { s.aimKind=2; s.aimId=npc->netId; }
            }
            auto& weapon=ped->ActiveWeapon();
            if (weapon.type != np.lastWeapon) np.lastClip=-1;
            if (np.lastClip>=0 && weapon.ammoInClip<np.lastClip) np.lastShotMs=now;
            np.lastClip=weapon.ammoInClip; np.lastWeapon=(uint8_t)weapon.type;
            if (now-np.lastShotMs<600) s.flags |= PedSyncState::F_FIRING;
        }
        return s;
    }

    void* enter = FindTask(ped, TASK_ENTER_CAR_AS_DRIVER);
    bool asDriver = enter != nullptr;
    if (!enter) enter = FindTask(ped, TASK_ENTER_CAR_AS_PASSENGER);
    if (enter) {
        auto* target = Field<CVehicle*>(enter, 0x0C);  // CTaskComplexEnterCar::m_Car
        if (target && IsVehicleValid(target) && IsSyncableVehicle(target))
            if (NetVehicle* nv = Vehicles::Instance().EnsureNetworked(target)) {
                s.flags |= PedSyncState::F_ENTERING;
                s.vehNetId = nv->netId;
                // m_TargetDoor once at the door, before that the door the go-to-door task was given (m_TargetSeat)
                int door = Field<int32_t>(enter, 0x14) ? Field<int32_t>(enter, 0x14) : Field<int32_t>(enter, 0x1C);
                s.seat = asDriver ? -1 : (int8_t)SeatOfTargetDoor(door);
            }
        return s;
    }

    // guns: shots show up as the clip emptying (the gun task fires on its own schedule)
    CWeapon& w = ped->ActiveWeapon();
    if (w.type != np.lastWeapon) np.lastClip = -1;
    if (np.lastClip >= 0 && w.ammoInClip < np.lastClip) np.lastShotMs = now;
    np.lastClip = w.ammoInClip;
    np.lastWeapon = (uint8_t)w.type;
    if (void* gun = FindTask(ped, TASK_SIMPLE_USE_GUN)) {
        auto* target = Field<CEntity*>(gun, 0x1C);  // CTaskSimpleUseGun::m_pTarget, else m_vecTarget
        CVector tp = target ? target->GetPosition() : Field<CVector>(gun, 0x20);
        if (!target && tp.Length() < 1.0f)
            tp = s.pos + CVector{-std::sin(s.heading) * 10.0f, std::cos(s.heading) * 10.0f, 0.0f};
        s.flags |= PedSyncState::F_AIMING;
        s.aim = tp - s.pos;
        if (target && target->IsPed()) {  // a ped target is tracked by the mirror's own task
            auto* tped = (CPed*)target;
            if (tped == FindPlayerPed(0)) {
                s.aimKind = 1;
                s.aimId = Net().MyId();
            } else if (RemotePlayer* rp = Players::Instance().FindByPed(tped)) {
                s.aimKind = 1;
                s.aimId = rp->id;
            } else if (NetPed* tn = Find(tped)) {
                s.aimKind = 2;
                s.aimId = tn->netId;
            }
        }
        if (now - np.lastShotMs < 600) s.flags |= PedSyncState::F_FIRING;
        return s;
    }
    return s;
}

void Peds::Announce(NetPed& np) {
    if(!np.firstAnnouncedMs)np.firstAnnouncedMs=NowMs();
    PedSyncState s = Capture(np);
    Writer w(proto::G_PED_CREATE);
    w.Entity(ENTITY_PED,np.netId,ENTITY_BIRTH|ENTITY_SNAPSHOT);
    w.U16(np.model);
    w.U8(np.pedType);
    w.Str(np.special, 16);
    WriteState(w, s);
    Net().Send(proto::CH_RELIABLE, w);
    np.lastSendMs = NowMs();
}

void Peds::ReannounceOne(uint16_t id) { auto it=m_byId.find(id);if(it!=m_byId.end()&&it->second.owner==Net().MyId()&&IsAlive(it->second))Announce(it->second); }

void Peds::Reannounce() {
    for (auto& [id, np] : m_byId)
        if (np.owner == Net().MyId() && IsAlive(np)) Announce(np);
}

void Peds::SendState(NetPed& np) {
    Writer w(proto::G_PED_SYNC);
    w.Entity(ENTITY_PED,np.netId,ENTITY_SNAPSHOT);
    auto state=Capture(np);WriteState(w,state);
    bool actionActive=state.animBlock!=kNoAnim||state.interaction.parentKind||state.interaction.partnerKind||state.interaction.sequenceStep>=0;
    for(auto&layer:state.interaction.layers)actionActive|=layer.Present();
    if((actionActive||np.actionSentActive)&&state.interaction.revision!=np.actionSentRevision){
        Writer action(proto::G_PED_ACTION);action.Entity(ENTITY_PED,np.netId);state.Write(action);Net().Send(proto::CH_RELIABLE,action);np.actionSentRevision=state.interaction.revision;
    }
    np.actionSentActive=actionActive;Net().Send(proto::CH_STATE, w);
    np.lastSendMs = NowMs();
}

void Peds::RespawnCopiesOf(int model) {
    for (auto& [id, np] : m_byId) {
        if (np.owner == Net().MyId() || !IsAlive(np) || np.ped->ModelIndex() != model) continue;
        Cmd(0x009B).I(np.handle).Run();  // DELETE_CHAR: SpawnMirror makes it again
        m_byPtr.erase(np.ped);
        np.ped = nullptr;
        np.handle = -1;
        LOGI("Mirror ped %04X makes way for another special character", id);
    }
}

void Peds::Forget(uint16_t netId, bool deletePed) {
    auto it = m_byId.find(netId);
    if (it == m_byId.end()) return;
    NetPed& np = it->second;
    if(!deletePed)HoldCorpseOutside(np,false);
    if (deletePed && IsAlive(np)) Cmd(0x009B).I(np.handle).Run();  // DELETE_CHAR
    if(auto ptr=m_byPtr.find(np.ped);ptr!=m_byPtr.end()&&ptr->second==netId)m_byPtr.erase(ptr);
    m_ids.Retire(netId,NowMs());
    m_byId.erase(it);
}

void Peds::ScanOwned() {
    CPoolHeader* pool = PedPool();
    if (!pool) return;
    CPlayerPed* me = FindPlayerPed(0);
    uint8_t myId = Net().MyId();
    uint64_t now = NowMs();

    // Interior changes do not always run ManagePed for the old area's population.
    for (auto& [id, np] : m_byId) {
        if (np.owner != myId || !IsAlive(np) || np.ped->InVehicle()) continue;
        CPed* ped = np.ped;
        const auto preferred=PopulationOwnerAt(ped->GetPosition(),AreaOf(ped));
        if(preferred==Net().HostId()&&preferred!=myId&&(np.netId>>12)==Net().HostId()+1)
            QueueGiveAway(ped,preferred);
        if (AreaOf(ped) == me->AreaCode() && (ped->GetPosition() - me->GetPosition()).Length2D() < 110) continue;
        uint8_t closest=PopulationOwnerAt(ped->GetPosition(),AreaOf(ped));
        if (closest != proto::kServerId && closest != myId) QueueGiveAway(ped, closest);
    }

    // peds this game would have dropped (CPopulation::ManagePed) while the partner is nearer go to the partner's game
    for (auto& [ped, to] : m_giveAway)
        if (NetPed* np = IsPedValid(ped) ? Find(ped) : nullptr; np && np->owner == myId && IsAlive(*np)) GiveAway(np->netId, to);
    m_giveAway.clear();

    // owned peds that vanished or went out of the partner's range
    std::vector<uint16_t> gone;
    for (auto& [id, np] : m_byId)
        if (np.owner == myId && (!IsAlive(np) || (PrivateInteriorPed(np.ped)||(!StoryPed(np.ped)&&now-np.firstAnnouncedMs>3000&&RemoteDistance(np.ped->VehicleIfIn()?np.ped->Vehicle()->GetPosition():np.ped->GetPosition(), np.ped->VehicleIfIn()?np.ped->Vehicle()->AreaCode():AreaOf(np.ped)) > SyncOut(np.ped)))))
            gone.push_back(id);
    for (uint16_t id : gone) {
        Writer w(proto::G_PED_REMOVE);
        w.Entity(ENTITY_PED,id,ENTITY_REMOVE);
        Net().Send(proto::CH_RELIABLE, w);
        Forget(id, false);
    }

    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* ped = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        if (ped == me || ped->IsPlayer() || Players::Instance().FindByPed(ped)) continue;
        NetPed* np = Find(ped);
        if (np && np->owner != myId) continue;  // mirror
        auto*in=ped->VehicleIfIn();
        float d = RemoteDistance(in?in->GetPosition():ped->GetPosition(), in?in->AreaCode():AreaOf(ped));
        if (!np) {
            // Announcing a duplicate before the budgeted cleanup can create a native
            // mirror over the authority's original (CREATE_CHAR clears that space).
            if(ped->CreatedBy()==PED_GAME&&SharesPopulationAt(ped->GetPosition(),AreaOf(ped))){
                auto*v=ped->VehicleIfIn();auto*car=v?Vehicles::Instance().Find(v):nullptr;
                if(!v||!car||(!car->playerUsed&&!car->script))continue;
            }
            if(PrivateInteriorPed(ped))continue; // each local activity keeps its own native clerk/fighter script
            if (!StoryPed(ped)&&d > SyncIn(ped)) continue;
            if (CVehicle* v = ped->VehicleIfIn(); v && !IsSyncableVehicle(v)) continue;
            uint16_t id=m_ids.Take((uint16_t)((myId+1)<<12),now,[&](uint16_t v){return m_byId.count(v)!=0;});
            if(!id)continue;
            NetPed& n = m_byId[id];
            n.netId = id;Net().BeginEntity(ENTITY_PED,id);
            n.ped = ped;
            n.handle = GetPedRef(ped);
            n.owner = myId;
            n.model = ped->ModelIndex();
            n.pedType = (uint8_t)ped->PedType();
            if (auto it = m_specialNames.find(n.model); it != m_specialNames.end()) n.special = it->second;
            m_byPtr[ped] = id;
            Announce(n);
            continue;
        }
        uint64_t interval = StoryPed(ped)?100:d < 40.0f ? 100 : (d < 80.0f ? 200 : 400);
        if (now - np->lastSendMs >= interval) SendState(*np);
    }
}

CPed* Peds::SpawnMirror(NetPed& np) {
    profile::Scope timing(profile::ENTITY_CREATE);
    if(m_spawnBudget<=0)return nullptr;
    const PedSyncState& st = np.state;
    int model = np.model;
    if (model >= kSpecialFirst && model <= kSpecialLast) {
        if (np.special.empty()) return nullptr;
        // in whichever slot holds that name here, or a free one: the owner's slot can belong to someone here (a
        // mission's person, another copy), and RequestSpecialModel took the model from under them
        model = SpecialModelFor(np.special);
        if (model < 0) return nullptr;
    } else if (!IsPedModel(model)) {
        return nullptr;
    }
    if (!IsModelLoaded(model)) {
        // streamed in the background, the spawn is retried (a blocking load per ped stalls the frame)
        RequestModel(model, 0);
        return nullptr;
    }
    if (!HasRoom(PedPool(), kKeepPeds, "ped")) return nullptr;
    if (!HasRoom(Ref<CPoolHeader*>(0xB744A8),64,"task")) return nullptr;
    // mirrors are civilians whatever the owner's type: cop/gang/emergency classes add their own AI and
    // the cop class even replaces the model
    int pedType = np.pedType == PED_TYPE_CIVFEMALE ? PED_TYPE_CIVFEMALE : PED_TYPE_CIVMALE;
    int handle = -1;
    if (st.flags & PedSyncState::F_IN_VEHICLE) {
        NetVehicle* nv = (Net().EntityLink(ENTITY_PED,np.netId,ENTITY_VEHICLE,st.vehNetId)?Vehicles::Instance().Find(st.vehNetId):nullptr);
        if (!nv || !Vehicles::Alive(*nv)) return nullptr;  // wait for the vehicle
        if (st.seat < 0) {
            // A local ambient task can occupy a remote car before its streamed
            // driver arrives. That unrelated occupant must not block the real
            // driver forever (nor replace a player or a mission character).
            if(auto*d=nv->veh->Driver();d&&!d->IsPlayer()&&!Players::Instance().FindByPed(d)){
                auto*old=Find(d);
                if(!old&&d->CreatedBy()==PED_GAME)Cmd(0x009B).I(GetPedRef(d)).Run();
                else if(old&&old->owner!=Net().MyId()&&old->hasState&&
                    (!(old->state.flags&PedSyncState::F_IN_VEHICLE)||old->state.vehNetId!=st.vehNetId||old->state.seat>=0)){
                    auto p=d->GetPosition();Cmd(0x0362).I(GetPedRef(d)).F(p.x).F(p.y).F(p.z).Run();
                }
            }
            if (nv->veh->Driver()) return nullptr;
            --m_spawnBudget;
            Cmd(0x0129).I(nv->handle).I(pedType).I(model).Out(&handle).Run();  // CREATE_CHAR_INSIDE_CAR
        } else {
            if (st.seat >= nv->veh->MaxPassengers() || st.seat >= 8 || nv->veh->Passengers()[st.seat]) return nullptr;
            --m_spawnBudget;
            Cmd(0x01C8).I(nv->handle).I(pedType).I(model).I(st.seat).Out(&handle).Run();  // CREATE_CHAR_AS_PASSENGER
        }
    } else {
        --m_spawnBudget;
        Cmd(0x009A).I(pedType).I(model).F(st.pos.x).F(st.pos.y).F(st.pos.z - 1.0f).Out(&handle).Run();  // CREATE_CHAR
    }
    CPed* ped = GetPed(handle);
    if (!ped) return nullptr;
    Cmd(0x060B).I(handle).I(kEmptyDecisionMaker).Run();  // SET_CHAR_DECISION_MAKER: reacts to nothing by itself
    Cmd(0x02AB).I(handle).I(1).I(1).I(1).I(1).I(1).Run();  // SET_CHAR_PROOFS: damage is decided by the owner
    Cmd(0x087E).I(handle).I(0).Run();                      // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD false
    ped->CurrentRotation() = ped->AimingRotation() = st.heading;
    ped->AreaCode() = st.area;
    if(!(st.flags&PedSyncState::F_ENTERING))np.carContact=false;
    np.ped = ped;np.deadApplied=false;
    np.handle = handle;
    m_byPtr[ped] = np.netId;
    return ped;
}

void Peds::HandleCreate(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    uint16_t model = r.U16();
    uint8_t pedType = r.U8();
    std::string special = r.Str(16);
    PedSyncState st;
    if(!ReadState(r,st))return;
    if (!r.Ok() || from == Net().MyId() || !core::IsPlayerReady()) return;
    if(auto it=m_byId.find(id);it!=m_byId.end()){if(it->second.owner==from&&it->second.model==model&&(!it->second.hasState||(int32_t)(st.time-it->second.state.time)>=0)){it->second.state=st;it->second.hasState=true;it->second.recvMs=NowMs();}return;}
    NetPed& np = m_byId[id];
    np.netId = id;
    np.owner = from;
    np.model = model;
    np.pedType = pedType;
    np.special = special;
    if(np.hasState&&(int32_t)(st.interaction.revision-np.state.interaction.revision)<0){st.interaction=np.state.interaction;st.animBlock=np.state.animBlock;st.animIdx=np.state.animIdx;st.animTime=np.state.animTime;st.animSpeed=np.state.animSpeed;st.animFlags=np.state.animFlags;}
    np.state = st;
    np.hasState = true;
    np.recvMs = NowMs();np.firstAnnouncedMs=NowMs();
    // Spawn from the bounded native-frame pass, never a burst of network callbacks.
}

void Peds::HandleRemove(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    if (!r.Ok()) return;
    auto it = m_byId.find(id);
    if (it == m_byId.end() || it->second.owner != from) return;
    Forget(id, true);
}

bool Peds::CanGiveAwayType(const NetPed& np) {
    return np.pedType != PED_TYPE_COP && np.pedType != PED_TYPE_MEDIC && np.pedType != PED_TYPE_FIREMAN;
}

// A passer-by of this game: alive, on foot, not a script's (a mission's ped stays with its script), not the player's
// recruit and not in view (CPed::CanBeDeleted and CEntity::GetIsOnScreen, as the engine's own removal)
bool Peds::QueueGiveAway(CPed* ped, uint8_t to) {
    NetPed* np = Find(ped);
    if (!np || np->owner != Net().MyId() || ped->InVehicle() || ped->CreatedBy() != 1 /* PED_GAME */ || IsPedDead(ped) ||
        !CanGiveAwayType(*np) || !CallMethod<0x5DF100, bool>(ped) || NowMs()<np->handoverUntil)
        return false;
    m_giveAway[ped] = to;
    return true;
}

void Peds::GiveAway(uint16_t netId, uint8_t to) {
    auto it=m_byId.find(netId);
    if(it==m_byId.end()||it->second.owner!=Net().MyId()||to==Net().MyId()||!Net().Peer(to)||!IsAlive(it->second))return;
    auto&np=it->second;
    HoldCorpseOutside(np,false);
    np.state=Capture(np);
    Writer w(proto::G_PED_OWNER);
    w.Entity(ENTITY_PED,netId);
    w.U8(to);
    Net().Send(proto::CH_RELIABLE, w);
    LOGD("Ped %04X handed over to player %d", netId, to);
    np.owner=to;np.hasState=false;np.handoverUntil=NowMs()+3000;
    Cmd(0x060B).I(np.handle).I(kEmptyDecisionMaker).Run();
    Cmd(0x02AB).I(np.handle).I(1).I(1).I(1).I(1).I(1).Run();
    Cmd(0x087E).I(np.handle).I(0).Run();
}

// The owner handed its ped over: the mirror is this game's own from now on, an ordinary ped (its own decision maker
// and a wander task, CTheScripts::CleanUpThisPed); in a car handed over with it, it drives on.
void Peds::HandleOwner(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    uint8_t to = r.U8();
    if (!r.Ok()) return;
    auto it = m_byId.find(id);
    if (it == m_byId.end()) {
        if (to == Net().MyId()) {
            Writer gone(proto::G_PED_REMOVE);
            gone.Entity(ENTITY_PED,id,ENTITY_REMOVE);
            Net().Send(proto::CH_RELIABLE, gone);
        }
        return;
    }
    if (it->second.owner != from) return;
    NetPed& np = it->second;
    np.owner = to;
    np.hasState = false;  // the next state comes from a different clock
    if (to != Net().MyId()) return;  // a third player keeps the same mirror instead of deleting it
    if (!IsAlive(np)) {
        // The old viewer may have streamed the physical copy out. Retain the
        // record and spawn it before adoption instead of globally removing it.
        m_byPtr.erase(np.ped);np.ped=nullptr;np.handle=-1;np.pendingAdoption=true;
        return;
    }
    if (IsPedDead(np.ped)) {
        np.state.flags|=PedSyncState::F_DEAD;np.state.health=0;
    }
    Adopt(np);
    np.lastSendMs = 0;np.handoverUntil=NowMs()+2000;
    Announce(np);  // preserve the id, including its vehicle's occupant references
    SendState(np);
    LOGD("Ped %04X taken over from player %d", id, from);
}

void Peds::Adopt(NetPed& np) {
    if(IsAlive(np))np.ped->PhysicalFlags()&=~0x2000u;
    Cmd(0x02AB).I(np.handle).I(0).I(0).I(0).I(0).I(0).Run();  // SET_CHAR_PROOFS off
    Cmd(0x087E).I(np.handle).I(1).Run();                      // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD
    Cmd(0x01C2).I(np.handle).Run();                           // MARK_CHAR_AS_NO_LONGER_NEEDED
    // A streamed-out body recreated for handover starts at native health 100.
    // Ownership must retain the source's death/health rather than revive it.
    if(IsAlive(np)){
        np.ped->Health()=(np.state.flags&PedSyncState::F_DEAD)?0.f:(float)np.state.health;
        np.ped->Armour()=np.state.armour;
        if(np.state.flags&PedSyncState::F_DEAD){
            Field<uint32_t>(np.ped,0x750)=TimeMs(); // CPed::m_nDeathTimeMS, in this game's clock
            Cmd(0x05BE).I(np.handle).Run();
        }
    }
    // A copy is a civilian that reacts to nothing (SpawnMirror): ours now, it is what its owner had again and acts by its
    // kind (a gang member stood still under fire). Cops, medics and firemen are other classes: they stay civilians.
    if (np.pedType > PED_TYPE_COP && np.pedType != PED_TYPE_MEDIC && np.pedType != PED_TYPE_FIREMAN && np.pedType <= 31)
        np.ped->PedType() = np.pedType;
    CallMethod<0x5E06E0>(np.ped);  // CPed::SetPedDefaultDecisionMaker: the one of its stats, now that no script holds it
}

void Peds::HandleSync(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    PedSyncState st;
    if(!ReadState(r,st))return;
    if (!r.Ok()) return;
    auto it = m_byId.find(id);
    if (it == m_byId.end() || it->second.owner != from) return;
    NetPed& np = it->second;
    if (np.hasState && (int32_t)(st.time - np.state.time) < 0) return;  // reordered
    np.state = st;
    np.hasState = true;
    np.recvMs = NowMs();
}

void Peds::HandleAction(uint8_t from,Reader&r){
    auto id=r.U16();PedSyncState st;if(!st.Read(r))return;auto i=m_byId.find(id);if(i==m_byId.end()||i->second.owner!=from)return;
    auto&n=i->second;if(n.hasState&&(int32_t)(st.interaction.revision-n.state.interaction.revision)<=0){LOGD("NPC action revision ignored id=%04X incoming=%u current=%u",id,st.interaction.revision,n.state.interaction.revision);return;}
    if(!n.hasState){n.state=st;n.hasState=true;n.recvMs=NowMs();return;}
    Net().RememberEntityLinks(ENTITY_PED,id);n.state.interaction=st.interaction;n.state.animBlock=st.animBlock;n.state.animIdx=st.animIdx;n.state.animFlags=st.animFlags;
    n.state.animTime=st.animTime+std::clamp((int32_t)(n.state.time-st.time)/1000.f,0.f,.75f)*st.animSpeed;n.state.animSpeed=st.animSpeed;
}

void Peds::ApplyMirror(NetPed& np) {
    if(!np.hasState)return;
    const PedSyncState& st = np.state;
    uint64_t now = NowMs();
    auto*focus=FindPlayerPed(0);bool camera=Ref<uint8_t>(addr::TheCamera+0x2B)==0;
    int area=camera?Ref<int32_t>(0xB72914):focus->AreaCode();
    CVector centre=camera?cam::Source():focus->GetPosition();
    bool nearPlayer=(st.area==focus->AreaCode()||st.area==13)&&(st.pos-focus->GetPosition()).Length2D()<280;
    // Wide scripted shots can show mission vehicles hundreds of metres away.
    // Keep their actors as long as the visible vehicle itself exists; normal
    // population still uses the bounded player/camera streaming distance.
    bool story=(st.entityFlags&16)&&np.owner==Net().HostId()&&HostMissionRecent(0);
    auto*sceneCar=story&&st.vehNetId?Vehicles::Instance().Find(st.vehNetId):nullptr;
    bool relevant=nearPlayer||((st.area==area||st.area==13)&&((st.pos-centre).Length2D()<(story?1100.f:280.f)||(sceneCar&&Vehicles::Alive(*sceneCar))));
    if(!relevant){
        if(IsAlive(np)&&now-np.lastRelevantMs>5000){auto*old=np.ped;m_byPtr.erase(old);Cmd(0x009B).I(np.handle).Run();np.ped=nullptr;np.handle=-1;np.animBlock=np.baseBlock=kNoAnim;np.animFull=false;np.lastTaskMove=0;np.gunMode=0;}
        return;
    }
    np.lastRelevantMs=now;
    if (!np.ped) {
        if (now - np.lastTaskMs >= 100) {
            np.lastTaskMs = now;
            SpawnMirror(np);
        }
        return;
    }
    CPed* ped = np.ped;
    if (!IsAlive(np)) {  // deleted by the game (should not happen to mission peds): spawn again
        m_byPtr.erase(ped);
        np.ped = nullptr;
        return;
    }
    ped->AreaCode() = st.area;
    ped->Flags()=(ped->Flags()&~(1u|128u))|((st.entityFlags&1)?128u:0)|((st.entityFlags&2)?1u:0);
    Field<uint8_t>(ped,0x475)=(Field<uint8_t>(ped,0x475)&~1u)|((st.entityFlags&8)?1u:0u);
    ped->PhysicalFlags()=(ped->PhysicalFlags()&~0x2000u)|((st.entityFlags&4)?0x2000u:0);
    if(st.entityFlags&4)ped->MoveSpeed()={};
    if (st.flags & PedSyncState::F_DEAD) {
        if(np.interactionAttached&&!st.interaction.parentKind){if(ped->AttachedTo())CallMethod<0x5E7EC0>(ped);np.interactionAttached=false;}
        if(st.flags&PedSyncState::F_IN_VEHICLE){
            auto*nv=(Net().EntityLink(ENTITY_PED,np.netId,ENTITY_VEHICLE,st.vehNetId)?Vehicles::Instance().Find(st.vehNetId):nullptr);
            if(!nv||!Vehicles::Alive(*nv))return; // retain death until its car exists
            bool inside=ped->InVehicle()&&ped->Vehicle()==nv->veh;
            if(!inside){
                if(st.seat<0&&!nv->veh->Driver())Cmd(0x036A).I(np.handle).I(nv->handle).Run();
                else if(st.seat>=0&&st.seat<nv->veh->MaxPassengers()&&!nv->veh->Passengers()[st.seat])
                    Cmd(0x0430).I(np.handle).I(nv->handle).I(st.seat).Run();
            }
        }else if(ped->InVehicle()){
            Cmd(0x0362).I(np.handle).F(st.pos.x).F(st.pos.y).F(st.pos.z-1).Run();
        }else{
            SetEntityPosition(ped,st.pos+st.vel*std::min(.2f,(now-np.recvMs)/1000.f));ped->MoveSpeed()=st.vel*(1.f/50.f);
        }
        if (!np.deadApplied) {
            np.deadApplied = true;
            Cmd(0x02AB).I(np.handle).I(0).I(0).I(0).I(0).I(0).Run();  // proofs off so it can die
            if (now - np.lastHitMs < 2000) {  // our player killed it: the loot falls here (its owner drops none)
                Cmd(0x087E).I(np.handle).I(1).Run();  // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD
                bool payless = np.pedType == PED_TYPE_COP || np.pedType == PED_TYPE_MEDIC || np.pedType == PED_TYPE_FIREMAN;
                // SET_CHAR_MONEY: a script ped drops only the money a script gave it
                Cmd(0x03FE).I(np.handle).I(payless ? 0 : ped->Money()).Run();
            }
            if(st.animBlock==kNoAnim||(st.flags&PedSyncState::F_IN_VEHICLE))Cmd(0x05BE).I(np.handle).Run(); // TASK_DIE fallback
        }
        if(!(st.flags&PedSyncState::F_IN_VEHICLE)&&st.animBlock!=kNoAnim&&ped->Intelligence()&&
           (np.deathBlock!=st.animBlock||np.deathIndex!=st.animIdx)){
            if(auto*h=anim::Resolve(st.animBlock,st.animIdx))if(void*task=Call<0x61A5A0,void*>((size_t)0x28)){
                // Native death with the owner's actual hierarchy, rather than
                // a generic fall underneath its partial knife animation.
                CallMethod<0x62FAF0>(task,h,(int)(st.animFlags&~(anim::F_BLOCK_REFERENCED|anim::F_INDESTRUCTIBLE)),8.f,st.animSpeed);
                CallMethod<0x681AF0>((uint8_t*)ped->Intelligence()+4,task,0,false);
                np.deathBlock=st.animBlock;np.deathIndex=st.animIdx;
            }
        }
        ped->Health()=0;
        float age=std::min(.2f,(now-np.recvMs)/1000.f);
        ApplyInteraction(np,age);ReplayBoundAction(np,age);
        if(!(st.flags&PedSyncState::F_IN_VEHICLE))AlignContactHeading(np,st.heading);
        return;
    }
    ped->Health() = std::max<float>(1.0f, st.health);
    ped->Armour() = st.armour;
    if (CWeapon& w = ped->ActiveWeapon(); w.type == np.lastWeapon && w.ammoInClip < np.lastClip) ++m_mirrorShots;
    np.lastClip = ped->ActiveWeapon().ammoInClip;
    np.lastWeapon = (uint8_t)ped->ActiveWeapon().type;

    if (ped->ActiveWeapon().type != st.weapon && now - np.weaponMs > 1000) {
        np.weaponMs = now;
        if (st.weapon != 0) {
            int wmodel = -1;
            Cmd(0x0781).I(st.weapon).Out(&wmodel).Run();  // GET_WEAPONTYPE_MODEL
            if (wmodel > 0 && !IsModelLoaded(wmodel)) {
                RequestModel(wmodel, 0);
                return;
            }
            Cmd(0x01B2).I(np.handle).I(st.weapon).I(500).Run();  // GIVE_WEAPON_TO_CHAR
        }
        Cmd(0x01B9).I(np.handle).I(st.weapon).Run();  // SET_CURRENT_CHAR_WEAPON
    }

    // A paired on-foot action may follow a passenger task. Release the old
    // native seat before its early return; otherwise the replay stays in the car.
    if(!(st.flags&PedSyncState::F_IN_VEHICLE)&&st.interaction.partnerKind&&st.animBlock!=kNoAnim&&ped->InVehicle()){
        Cmd(0x0362).I(np.handle).F(st.pos.x).F(st.pos.y).F(st.pos.z-1).Run();
        np.enterVeh=0;np.enterTaskMs=np.leaveTaskMs=0;
        LOGD("Paired actor released stale passenger seat id=%04X",np.netId);
    }
    bool paired=!(st.flags&PedSyncState::F_IN_VEHICLE)&&!st.interaction.parentKind&&st.interaction.partnerKind&&st.animBlock!=kNoAnim;
    if(paired&&!np.pairedAction){Cmd(0x0792).I(np.handle).Run();np.lastTaskMove=0;}
    np.pairedAction=paired;
    if(paired)AlignContactHeading(np,st.heading);
    bool bound=ApplyInteraction(np,std::min(.2f,(now-np.recvMs)/1000.f));
    if(np.interactionPending){if(np.gunMode){Cmd(0x0687).I(np.handle).Run();np.gunMode=0;}return;}
    if(bound&&!(st.flags&PedSyncState::F_AIMING)){
        ReplayBoundAction(np,std::min(.2f,(now-np.recvMs)/1000.f));
        return;
    }
    // ---- vehicles: the game's own enter/leave tasks, warps only as a fallback
    if (st.flags & PedSyncState::F_IN_VEHICLE) {
        NetVehicle* nv = (Net().EntityLink(ENTITY_PED,np.netId,ENTITY_VEHICLE,st.vehNetId)?Vehicles::Instance().Find(st.vehNetId):nullptr);
        if (!nv || !Vehicles::Alive(*nv)) return;
        bool inside = ped->InVehicle() && ped->Vehicle() == nv->veh;
        if (inside && (st.flags & PedSyncState::F_LEAVING) && !np.leaveTaskMs) {
            Cmd(0x05CD).I(np.handle).I(nv->handle).Run();  // TASK_LEAVE_CAR
            np.leaveTaskMs = now;
        }
        if (inside && (st.flags & PedSyncState::F_DRIVEBY) && !(st.flags & PedSyncState::F_LEAVING)) {
            RequestDrivebyAnimations();
            CVector target = st.pos + st.aim;
            int targetPed=-1;
            if (st.aimKind==1) {
                auto* other=Players::Instance().Get((uint8_t)st.aimId);
                CPed* aim=st.aimId==Net().MyId()?FindPlayerPed(0):(other?other->ped:nullptr);
                if(aim)targetPed=GetPedRef(aim);
            } else if(st.aimKind==2&&Net().EntityLink(ENTITY_PED,np.netId,ENTITY_PED,st.aimId))targetPed=HandleOf(st.aimId);
            bool moved=targetPed<0&&(target-np.gunTarget).Length()>3;
            if (!FindTask(ped,1022) || targetPed!=np.gunTargetHandle || moved) {
                // With an entity target GTA treats the point as a LOCAL offset.
                // Sending world coordinates here aims thousands of metres away.
                CVector point=targetPed>=0?CVector{}:target;
                Cmd(0x0713).I(np.handle).I(targetPed).I(-1).F(point.x).F(point.y).F(point.z)
                    .F(100).I(st.drivebyStyle).I(st.seat==0||st.seat==2).I(st.drivebyFrequency).Run();
                np.gunTarget=target;np.gunTargetHandle=targetPed;np.gunTaskMs=now;np.gunMode=3;
                LOGD("NPC %04X drive-by task replayed",np.netId);
            }
        } else if (inside && np.gunMode==3 && !(st.flags & PedSyncState::F_LEAVING)) {
            Cmd(0x0687).I(np.handle).Run(); // CLEAR_CHAR_TASKS, retains the seat
            np.gunMode=0;
        }
        int seat = inside ? PassengerSeatOf(nv->veh, ped) : -2;
        if (seat == st.seat || now - np.lastTaskMs < 1000) return;
        if (!inside && np.leaveTaskMs && now - np.leaveTaskMs < kCarTaskGraceMs) return;  // getting out like the owner
        if (!inside && np.enterVeh == st.vehNetId && now - np.enterTaskMs < kCarTaskGraceMs) return;  // still getting in
        np.lastTaskMs = now;
        if (ped->InVehicle()) {
            CVector p = ped->GetPosition();
            Cmd(0x0362).I(np.handle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        }
        if (st.seat < 0) {
            if (!nv->veh->Driver()) Cmd(0x036A).I(np.handle).I(nv->handle).Run();  // WARP_CHAR_INTO_CAR
        } else if (SeatOrAny(nv->veh, st.seat) >= 0 && !nv->veh->Passengers()[st.seat]) {
            Cmd(0x0430).I(np.handle).I(nv->handle).I(st.seat).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
        }
        return;
    }
    if (ped->InVehicle()) {
        if (!np.leaveTaskMs) {
            Cmd(0x05CD).I(np.handle).I(GetVehicleRef(ped->Vehicle())).Run();  // TASK_LEAVE_CAR
            np.leaveTaskMs = now;
        } else if (now - np.leaveTaskMs > kCarTaskGraceMs) {
            Cmd(0x0362).I(np.handle).F(st.pos.x).F(st.pos.y).F(st.pos.z - 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD (feet)
            np.leaveTaskMs = now;
        }
        return;
    }
    np.leaveTaskMs = 0;
    if (st.flags & PedSyncState::F_ENTERING) {
        bool pulling=false;for(unsigned i=0;i<st.interaction.depth;++i)
            pulling|=st.interaction.chain[i]==817||st.interaction.chain[i]==820;
        if(pulling||np.carContact){
            // Running another enter/jack task here lets each viewer independently
            // grab the victim and choose when the driver seat becomes free.
            // Once contact begins, show the owner's real clip and transform until
            // its seat snapshot arrives; only the victim owner runs the drag-out.
            if(!np.carContact){Cmd(0x0687).I(np.handle).Run();np.carContact=true;np.enterTaskMs=0;}
            SetEntityPosition(ped,st.pos);AlignContactHeading(np,st.heading);
            ped->MoveSpeed()={};ped->Flags()&=~1u;
            if(st.motion.Present())anim::Replay(ped->RwObject(),st.motion,std::min(.2f,(now-np.recvMs)/1000.f));
            ReplayBoundAction(np,std::min(.2f,(now-np.recvMs)/1000.f));
            return;
        }
        NetVehicle* nv = (Net().EntityLink(ENTITY_PED,np.netId,ENTITY_VEHICLE,st.vehNetId)?Vehicles::Instance().Find(st.vehNetId):nullptr);
        if (nv && Vehicles::Alive(*nv) && np.enterVeh != st.vehNetId) {
            if (st.seat < 0) Cmd(0x05CB).I(np.handle).I(nv->handle).I(10000).Run();  // TASK_ENTER_CAR_AS_DRIVER
            else Cmd(0x05CA).I(np.handle).I(nv->handle).I(10000).I(SeatOrAny(nv->veh, st.seat)).Run();  // TASK_ENTER_CAR_AS_PASSENGER
            np.enterVeh = st.vehNetId;
            np.enterTaskMs = now;
            np.lastTaskMove = 0;
        }
        if (np.enterVeh == st.vehNetId) return;  // the task walks to the car
    } else if (now - np.enterTaskMs > kCarTaskGraceMs) {
        np.enterVeh = 0;
    }
    np.carContact=false;

    // ---- on foot
    const auto* peer = Net().Peer(np.owner);
    float transit = 0.0005f * (Net().Rtt() + (peer ? peer->ping : 0));
    float age = std::min(0.2f, transit + (float)(now - np.recvMs) / 1000.0f);
    CVector target = st.pos + st.vel * age;
    CVector cur = ped->GetPosition();
    CVector err = target - cur;
    if(st.entityFlags&4){
        // A story script's exact frozen height is authoritative. Letting each
        // mirror settle on its local floor made the same actor float in only
        // one window, and predicted its stale velocity away from that height.
        SetEntityPosition(ped,target);ped->MoveSpeed()={};cur=target;err={};
    }
    float d = err.Length2D();
    CPlayerPed* me = FindPlayerPed(0);
    bool found = false;
    Call<0x5696C0, float>(target.x, target.y, target.z + 1.0f, &found, (CEntity**)nullptr);
    if (st.area != me->AreaCode() || !found || (target - me->GetPosition()).Length2D() > 150.0f) {
        SetEntityPosition(ped, target);
        ped->MoveSpeed() = {};
        ped->CurrentRotation() = ped->AimingRotation() = st.heading;
        return;
    }
    if (d > 6.0f || std::fabs(err.z) > 4.0f) {
        SetEntityPosition(ped, target);
        ped->MoveSpeed() = st.vel * (1.0f / 50.0f);
        np.lastTaskMove = 0;
        np.gunMode = 0;
        return;
    }

    bool duck=(st.flags&PedSyncState::F_DUCKING)!=0;
    if(duck!=ped->IsDucking()&&ped->Intelligence()&&now-np.duckMs>400){
        np.duckMs=now;
        if(duck)CallMethod<0x601230>(ped->Intelligence(),(uint16_t)0);
        else CallMethod<0x601390>(ped->Intelligence());
    }
    // guns
    if ((st.flags & PedSyncState::F_AIMING) && st.weapon != 0) {
        // Keep the owner's crouched/moving stance under the native gun task.
        // Returning before the normal gait replay made armed actors slide.
        if(st.motion.Present())anim::Replay(ped->RwObject(),st.motion,std::min(.2f,(now-np.recvMs)/1000.f));
        CVector aimAt = st.pos + st.aim;
        uint8_t mode = (st.flags & PedSyncState::F_FIRING) ? 2 : 1;
        int targetHandle = -1;
        if (st.aimKind == 1) {
            CPed* p = st.aimId == Net().MyId() ? FindPlayerPed(0) : nullptr;
            if (const RemotePlayer* rp = Players::Instance().Get((uint8_t)st.aimId); rp && st.aimId != Net().MyId()) p = rp->ped;
            targetHandle = p ? GetPedRef(p) : -1;
        } else if (st.aimKind == 2) {
            auto it = m_byId.find(st.aimId);
            if(it!=m_byId.end()&&IsAlive(it->second)&&Net().EntityLink(ENTITY_PED,np.netId,ENTITY_PED,st.aimId))targetHandle=it->second.handle;
        }
        bool moved = targetHandle < 0 && (aimAt - np.gunTarget).Length() > 1.5f;
        if (mode != np.gunMode || targetHandle != np.gunTargetHandle || moved || now - np.gunTaskMs > 1500) {
            if (targetHandle >= 0)  // TASK_SHOOT_AT_CHAR / TASK_AIM_GUN_AT_CHAR: follows a moving target itself
                Cmd(mode == 2 ? 0x074D : 0x0635).I(np.handle).I(targetHandle).I(2000).Run();
            else  // TASK_SHOOT_AT_COORD / TASK_AIM_GUN_AT_COORD
                Cmd(mode == 2 ? 0x0668 : 0x0667).I(np.handle).F(aimAt.x).F(aimAt.y).F(aimAt.z).I(2000).Run();
            np.gunMode = mode;
            np.gunTargetHandle = targetHandle;
            np.gunTarget = aimAt;
            np.gunTaskMs = now;
        }
        np.lastTaskMove = 0;
        if(st.animBlock!=kNoAnim){AnimationState pose;pose.block=st.animBlock;pose.index=st.animIdx;pose.flags=st.animFlags;pose.time=st.animTime;pose.speed=st.animSpeed;anim::Replay(ped->RwObject(),pose,age);}
        if (!bound&&d > 0.25f) SetEntityPosition(ped, cur + CVector{err.x * 0.2f, err.y * 0.2f, 0});
        return;
    }
    if (np.gunMode) {
        np.gunMode = 0;
        np.lastTaskMove = 0;
    }

    // action animation (a restart of the same one shows up as its time going back)
    // a looped one going back is its next lap, not a restart (a fight stance would start over every lap)
    bool wasFull = np.animFull;
    bool restarted = st.animBlock != kNoAnim && !(st.animFlags & anim::F_LOOPED) && st.animTime + 0.25f < np.animTime;
    np.animTime = st.animTime;
    if (st.animBlock != np.animBlock || st.animIdx != np.animIdx || restarted ||
        (st.animBlock != kNoAnim && !FindPlaying(ped, st.animBlock, st.animIdx))) {
        bool partial = st.animBlock != kNoAnim && (st.animFlags & anim::F_PARTIAL);
        bool sameBase = st.animBlock == np.baseBlock && st.animIdx == np.baseIdx;
        bool wasOverlay = np.animBlock != kNoAnim && (np.animBlock != np.baseBlock || np.animIdx != np.baseIdx);
        if (wasOverlay)  // a punch or a guard that ended or changed
            if (void* a = FindPlaying(ped, np.animBlock, np.animIdx)) anim::FadeOut(a);
        if (np.baseBlock != kNoAnim && !partial && (!sameBase || restarted)) {  // the full-body one changes or ends
            if (void* a = FindPlaying(ped, np.baseBlock, np.baseIdx)) anim::FadeOut(a);
            np.baseBlock = kNoAnim;
        }
        np.animBlock = kNoAnim;
        if (st.animBlock != kNoAnim) {
            // the stance an overlay played over is still there: it carries on instead of starting over
            void* a = np.baseBlock != kNoAnim && sameBase ? FindPlaying(ped, np.baseBlock, np.baseIdx) : nullptr;
            if (a) {
                anim::BlendDelta(a) = 4.0f;
            } else if (void* h = anim::Resolve(st.animBlock, st.animIdx)) {  // not loaded yet: retried next frame
                if ((a = anim::Blend(ped->RwObject(), h, st.animFlags, 4.0f))) {
                    float total = anim::TotalTime(h), t = st.animTime + age * st.animSpeed;
                    LOGD("Mirror %04X plays %d/%d at %.2f of %.2f s (flags %04X%s)", np.netId, st.animBlock, st.animIdx, t, total,
                         st.animFlags, restarted ? ", again" : "");
                    anim::SetCurrentTime(a, (st.animFlags & anim::F_LOOPED) && total > 0 ? std::fmod(t, total) : std::min(t, total));
                    np.lastTaskMove = 0;
                }
            }
            if (a) {
                np.animBlock = st.animBlock;
                np.animIdx = st.animIdx;
                if (!partial) np.baseBlock = st.animBlock, np.baseIdx = st.animIdx;
            }
        }
        np.animFull = np.baseBlock != kNoAnim;  // a full-body action holds it, also under an overlay
        if (wasFull && !np.animFull) anim::RestoreMovement(ped);
    }
    if (st.animBlock != kNoAnim) {
        AnimationState action;
        action.block = st.animBlock; action.index = st.animIdx; action.flags = st.animFlags;
        action.time = st.animTime; action.speed = st.animSpeed;
        anim::Replay(ped->RwObject(), action, age);
    }
    if (np.animFull||np.extraFull) {  // a full-body action plays: hold the position without locomotion tasks
        if (d > 0.25f) SetEntityPosition(ped, cur + CVector{err.x * 0.2f, err.y * 0.2f, 0});
        ped->CurrentRotation() = ped->AimingRotation() = LerpAngle(ped->CurrentRotation(), st.heading, 0.2f);
        return;
    }

    if(bound)return;
    // walking/running with the normal animations towards the owner's position
    float speed = st.vel.Length2D();
    if (speed > 0.4f || d > 1.2f) {
        uint8_t move = st.moveState >= 7 ? 7 : (st.moveState >= 6 ? 6 : 4);
        // Mirrors are mission civilians, so their default gait and random walk speed differ
        // from the real ped. Keep its animation clock alongside the positional correction.
        if (st.motion.Present()) {
            if (st.motion.group >= 0 && st.motion.group < Ref<int32_t>(0xB4EA28)) Field<int32_t>(ped, 0x4D4) = st.motion.group;
            ped->MoveState() = move;
            anim::Replay(ped->RwObject(), st.motion, age);
        }
        CVector goal = target + st.vel * 0.6f;  // aim slightly ahead so the walk does not stop between updates
        bool due = now - np.lastTaskMs > 400 || move != np.lastTaskMove || (goal - np.lastTaskTarget).Length2D() > 1.5f;
        if (due && !TaskPending(ped)) {
            Cmd(0x05D3).I(np.handle).F(goal.x).F(goal.y).F(goal.z).I(move).I(2000).Run();  // TASK_GO_STRAIGHT_TO_COORD
            np.lastTaskMs = now;
            np.lastTaskTarget = goal;
            np.lastTaskMove = move;
        }
        // the task alone leaves it behind (a waiting task, something in its way only here, a straight line)
        float k = 1.0f - std::exp(-20.0f * TimeStep() / 50.0f);
        if (d > 0.02f || std::fabs(err.z) > 0.3f)
            SetEntityPosition(ped, cur + CVector{err.x * k, err.y * k, std::fabs(err.z) > 0.3f ? err.z * k : 0});
    } else {
        if (np.lastTaskMove != 1 && !TaskPending(ped)) {
            Cmd(0x05BA).I(np.handle).I(1000).Run();  // TASK_STAND_STILL
            np.lastTaskMove = 1;
        }
        if (d > 0.25f) SetEntityPosition(ped, cur + CVector{err.x * 0.2f, err.y * 0.2f, 0});
        ped->CurrentRotation() = ped->AimingRotation() = LerpAngle(ped->CurrentRotation(), st.heading, 0.2f);
    }
}

int Peds::RemoveSharedAmbient(const CVector& centre,int8_t area,int budget) {
    auto*pool=PedPool();int removed=0;
    for(int i=0;pool&&i<pool->size&&removed<budget;++i){
        if(pool->byteMap[i]<0)continue;
        auto*p=(CPed*)(pool->objects+(size_t)i*kPedPoolElemSize);
        if(p->IsPlayer()||Players::Instance().FindByPed(p)||p->CreatedBy()!=PED_GAME||p->InVehicle()||p->Health()<=0||AreaOf(p)!=area||
           (p->GetPosition()-centre).Length2D()>300)continue;
        auto*np=Find(p);if(np&&(np->owner!=Net().MyId()||NowMs()<np->handoverUntil||(np->netId>>12)!=Net().MyId()+1))continue;
        // A group merge changes the generator; it must not erase visible peds.
        if(!np)np=EnsureNetworked(p);
        auto source=NearbyPopulationShare().source;
        if(np&&source!=255&&CanGiveAwayType(*np)&&QueueGiveAway(p,source)){GiveAway(np->netId,source);++removed;}
    }
    return removed;
}

void Peds::OnPreProcess() {
    m_spawnBudget=2;
    bool ready=Net().Online()&&core::IsPlayerReady();
    if(!ready)return;
    if(!m_wasReady){
        Writer w(proto::G_RESYNC);
        WorldResync{Vehicles::Instance().PreservedIds()}.Write(w);
        Net().Send(proto::CH_RELIABLE,w);
        m_wasReady=true;
    }
    // Transfer the previous area's ambient peds before the native frame can cull
    // them after an interior switch. Initial-world resync still precedes announcements.
    if (m_wasReady && NowMs() - m_lastScan >= 50) {
        m_lastScan = NowMs();
        ScanOwned();
    }
    for (auto& [id, np] : m_byId) {
        if(np.pendingAdoption){
            if(SpawnMirror(np)){np.pendingAdoption=false;Adopt(np);np.lastSendMs=0;np.handoverUntil=NowMs()+3000;Announce(np);SendState(np);}
            continue;
        }
        if(np.owner==Net().MyId()){
            if(IsAlive(np)){
                if(np.state.interaction.leader<3)RestoreCooperativeAlly(np.ped,np.state.interaction.leader);
                auto*me=FindPlayerPed(0);bool away=AreaOf(np.ped)!=me->AreaCode()||(np.ped->GetPosition()-me->GetPosition()).Length2D()>150;
                HoldCorpseOutside(np,IsPedDead(np.ped)&&away);
            }
            // A script can freeze a newly-created actor before its first native
            // movement task supplies a base animation. Initialise that base on
            // the owner as well, otherwise observers and owner show different poses.
            if(IsAlive(np)&&np.ped->RwObject()&&!np.ped->InVehicle()&&!IsPedDead(np.ped)&&!anim::First(np.ped->RwObject()))
                Call<0x4D4610,void*>(np.ped->RwObject(),Field<int32_t>(np.ped,0x4D4),3,4.0f);
            continue;
        }
        ApplyMirror(np);
        auto* ped=np.ped;
        if(!IsAlive(np)||!ped->RwObject()||ped->InVehicle()||IsPedDead(ped))continue;
        bool lasting=false;
        for(void* a=anim::First(ped->RwObject());a;a=anim::Next(a))
            if(!(anim::Flags(a)&anim::F_PARTIAL)&&anim::BlendDelta(a)>=0){lasting=true;break;}
        if(!lasting) {
            Call<0x4D4610,void*>(ped->RwObject(),Field<int32_t>(ped,0x4D4),3,4.0f);
            np.baseBlock=kNoAnim;np.animFull=false;
        }
    }
}

void Peds::OnPostProcess() {
    for(auto i=handContacts.begin();i!=handContacts.end();){auto*p=GetPed(i->second.handle);
        i=!p||!GetPed(i->second.target)||!CallStd<0x6182B0,bool>((int)i->second.arm,p)?handContacts.erase(i):std::next(i);
    }
    bool ready = Net().Online() && core::IsPlayerReady();
    // Native SetMoveAnim recalculates mission-ped speed during ProcessControl. Restore the
    // owner's rate afterwards so the rendered steps match its displacement and next frame.
    if (ready) for (auto& [id, np] : m_byId)
        if (np.owner != Net().MyId() && IsAlive(np) && !np.animFull && !np.extraFull &&
            !(np.state.flags & (PedSyncState::F_DEAD | PedSyncState::F_IN_VEHICLE | PedSyncState::F_ENTERING | PedSyncState::F_AIMING)))
            if (void* a = anim::Find(np.ped->RwObject(), np.state.motion)) anim::Speed(a) = np.state.motion.speed;
    if (!ready || NowMs() - m_lastScan < 50) return;
    m_lastScan = NowMs();
    ScanOwned();
    if (NowMs() - m_lastStatsMs >= 5000) {
        m_lastStatsMs = NowMs();
        int owned = 0, mirrors = 0, spawned = 0, acting = 0, aiming = 0, gunTasks = 0;
        for (auto& [id, np] : m_byId) {
            if (np.owner == Net().MyId()) {
                ++owned;
                continue;
            }
            ++mirrors;
            spawned += np.ped != nullptr;
            acting += np.animBlock != kNoAnim;
            aiming += np.gunMode != 0;
            gunTasks += IsAlive(np) && FindTask(np.ped, TASK_SIMPLE_USE_GUN) != nullptr;
        }
        LOGI("Peds: %d announced, %d mirrors (%d spawned, %d animated, %d aiming, %d with a gun task, %d shots)", owned,
             mirrors, spawned, acting, aiming, gunTasks, m_mirrorShots);
        m_mirrorShots = 0;
    }
}

void Peds::OnSessionEnd() {
    handContacts.clear();
    for(auto&[id,np]:m_byId)HoldCorpseOutside(np,false);
    // mirrors stay in the world as ordinary random peds
    for (auto& [id, np] : m_byId)
        if (np.owner != Net().MyId() && IsAlive(np)) {
            np.ped->PhysicalFlags()&=~0x2000u;
            Cmd(0x02AB).I(np.handle).I(0).I(0).I(0).I(0).I(0).Run();
            Cmd(0x01C2).I(np.handle).Run();  // MARK_CHAR_AS_NO_LONGER_NEEDED (default decision maker, wander task)
        }
    m_byId.clear();
    m_byPtr.clear();
    m_wasReady = false;
}

// The player left: its people stay in this game as its own (deleted, they left its cars driverless in the street, as MTA
// keeps the elements and picks a new syncer).
void Peds::OnPeerLeave(uint8_t id) {
    std::vector<uint8_t> survivors{Net().MyId()};
    for(auto&[peerId,peer]:Net().Peers())if(peerId!=id)survivors.push_back(peerId);
    uint8_t successor=VehicleSuccessor(Net().HostId(),id,survivors);
    for(auto&[pid,np]:m_byId)if(np.owner==id){
        np.owner=successor;np.hasState=false;np.handoverUntil=NowMs()+2000;
        if(successor==Net().MyId()&&IsAlive(np)&&!IsPedDead(np.ped)){
            Adopt(np);np.lastSendMs=0;Announce(np);
        }
    }
}

void Peds::DropMirrorsOf(uint8_t id, bool keep) {
    std::vector<uint16_t> mirrors;
    for (auto& [pid, np] : m_byId)
        if (np.owner == id) mirrors.push_back(pid);
    for (uint16_t pid : mirrors) {
        NetPed& np = m_byId[pid];
        bool adopt = keep && IsAlive(np) && !IsPedDead(np.ped);
        if (adopt) Adopt(np);
        Forget(pid, !adopt);
    }
}

void Peds::OnGameStarted() {
    m_byId.clear();
    m_byPtr.clear();
    m_giveAway.clear();
    m_wasReady = false;  // ask the partner for its peds again once the player is ready
}

void Peds::OnGameShutdown() {
    handContacts.clear();
    Net().AdvanceWorldEpoch(); OnGameStarted(); }

}  // namespace coop

namespace coop { void Peds::OnEntityReplaced(uint8_t kind,uint8_t,uint16_t id){if(kind!=ENTITY_PED)return;Forget(id,true);} }
