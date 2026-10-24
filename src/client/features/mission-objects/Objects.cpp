// Objects of the host's mission (the bottles of "Nines and AKs", boxes, barriers) for the partner. The host's game
// knows what its mission script created (CTheScripts::MissionCleanUp lists them, objects as type 3): it announces
// those, sends their moves and their end, and applies the hits the partner's player lands on the copies. The copies
// are frozen, and nothing in the partner's game damages them: a hit by its player goes to the host as
// CObject::ObjectDamage, and a copy breaks when the host's object did.
#include <cmath>
#include "shared/lib/sync/IdAllocator.h"
#include <map>
#include <set>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/profile/Profile.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/player/PlayerExtras.h"
#include "entities/vehicle/VehicleState.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/lib/sync/ObjectMotion.h"
#include "shared/lib/sync/EventWindow.h"
#include "shared/api/mission-objects/SharedObject.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/story-actions/StoryActions.h"
#include "shared/game/Anim.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

enum : uint8_t { REMOVE_GONE = 0, REMOVE_BROKE = 1 };

struct NetObject {
    uint16_t netId = 0;
    CObject* obj = nullptr;
    int handle = -1;
    uint16_t model = 0;
    CVector pos;
    Quat rot;
    int8_t area = 0;
    bool broken = false;
    uint8_t flags=3;
    bool adopted = false;  // partner: its world had this object already (the host's world brought it)
    uint64_t sentMs = 0;
    ObjectMotion motion;
    uint64_t lease=0,claimSent=0,claimBegan=0,claimDeniedUntil=0,cargoTouch=0,cargoAsk=0;
};

std::map<uint16_t, NetObject> g_owned;    // host: announced
std::map<uint16_t, NetObject> g_mirrors;  // partner: the copies
std::map<uint16_t, NetObject> g_pending;  // partner: waiting for the model
std::set<int>g_storyObjects;
IdAllocator<65536> g_ids;
uint64_t g_scanMs = 0;
bool g_applying = false;  // a damage this module applies itself
uint32_t g_hitSequence=0;EventWindow g_hitsSeen[3],g_pushSeen[3];
struct CargoPush {uint16_t object=0,vehicle=0;uint32_t revision=0;CVector move,turn;};
std::map<uint32_t,CargoPush> g_pushes;
std::map<uint32_t,uint64_t> g_nativeCargo;
uint64_t g_pushSent=0;uint32_t g_pushSequence=0;

bool Alive(const NetObject& no) { return no.obj && IsObjectValid(no.obj) && GetObjectRef(no.obj) == no.handle; }
bool Broken(CObject* o) { return !o->IsVisible() && !(o->Flags() & 1); }  // smashed: no rendering, no collision
uint8_t ObjectFlags(CObject*o){return (o->IsVisible()?1:0)|((o->Flags()&1u)?2:0);}
void ApplyFlags(CObject*o,uint8_t f){o->Flags()=(o->Flags()&~129u)|((f&1)?128u:0)|((f&2)?1u:0);}
Quat RotationOf(CObject* o) { return o->Matrix() ? Quat::FromMatrix(*o->Matrix()) : Quat{}; }

void Place(CObject* o, const CVector& pos, const Quat& rot) {
    CMatrix* m = o->Matrix();
    if (!m) return;
    WorldRemove(o);
    rot.ToMatrix(*m);
    m->pos = pos;
    CallMethod<0x446F90>(o);  // CEntity::UpdateRW
    CallMethod<0x532B00>(o);  // CEntity::UpdateRwFrame
    Call<0x563220>(o);        // CWorld::Add
}

void SendCreate(const NetObject& no) {
    Writer w(proto::G_OBJ_CREATE);
    w.Entity(ENTITY_OBJECT,no.netId,ENTITY_BIRTH|ENTITY_SNAPSHOT);
    w.U16(no.model);
    w.Vec({no.pos.x, no.pos.y, no.pos.z});
    no.rot.Write(w);
    w.I8(no.area);
    w.U8(no.flags|(no.broken?4:0));no.motion.Write(w);
    Net().Send(proto::CH_RELIABLE, w);
}

void SendRemove(uint16_t netId, uint8_t kind, uint8_t weapon) {
    Writer w(proto::G_OBJ_REMOVE);
    w.Entity(ENTITY_OBJECT,netId,kind==REMOVE_GONE?ENTITY_REMOVE:ENTITY_USE);
    w.U8(kind);
    w.U8(weapon);
    Net().Send(proto::CH_RELIABLE, w);
}

NetObject* FindByObject(std::map<uint16_t, NetObject>& m, CObject* o) {
    for (auto& [id, no] : m)
        if (no.obj == o && Alive(no)) return &no;
    return nullptr;
}

void TrackObject(int handle){
    auto*o=GetObject(handle);
    if(!o||FindByObject(g_owned,o))return;
    NetObject no;
    no.netId=g_ids.Take(0,NowMs(),[](uint16_t v){return g_owned.count(v)!=0;});if(!no.netId)return;
    no.obj = o;
    no.handle = handle;
    no.model = o->ModelIndex();
    no.pos = o->GetPosition();
    no.rot = RotationOf(o);
    no.area = o->AreaCode();
    no.broken = Broken(o);
    no.flags=ObjectFlags(o);
    no.motion.dynamic=!(o->Flags()&4u);
    no.sentMs = NowMs();
    Net().BeginEntity(ENTITY_OBJECT,no.netId);g_owned[no.netId] = no;
    SendCreate(no);
    LOGI("Mission object %04X (model %d) announced", no.netId, no.model);
}


void* Holding(CPed*p,CObject*o){
    if(!p||!p->Intelligence())return nullptr;
    for(int type:{307,308,309})if(auto*t=CallMethod<0x681740,void*>((uint8_t*)p->Intelligence()+4,type))if(Field<CEntity*>(t,8)==o)return t;
    return nullptr;
}
Quat Product(const Quat&a,const Quat&b){return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};}
void CaptureMotion(NetObject&n){
    auto*o=n.obj;n.motion.velocity=o->MoveSpeed();n.motion.turn=o->TurnSpeed();
    n.motion.dynamic=!(o->Flags()&4u);n.motion.parentKind=0;n.motion.parentId=0;
    auto*parent=o->AttachedTo();
    if(parent&&parent->IsVehicle()){if(auto*v=Vehicles::Instance().EnsureNetworked((CVehicle*)parent)){n.motion.parentKind=1;n.motion.parentId=v->netId;}}
    else if(parent&&parent->IsPed()){
        if(parent==FindPlayerPed(0)){n.motion.parentKind=3;n.motion.parentId=Net().MyId();}
        else if(auto*p=Players::Instance().FindByPed((CPed*)parent)){n.motion.parentKind=3;n.motion.parentId=p->id;}
        else if(auto*p=Peds::Instance().Find((CPed*)parent)){n.motion.parentKind=2;n.motion.parentId=p->netId;}
    }
    if(parent&&parent->IsObject()){if(auto id=SharedMissionObjectId((CObject*)parent)){n.motion.parentKind=4;n.motion.parentId=id;}}
    if(n.motion.parentKind){n.motion.offset=Field<CVector>(o,0x100);n.motion.euler=Field<CVector>(o,0x10C);n.motion.rotation=Field<Quat>(o,0x118);
        if(auto*m=parent->Matrix()){
            auto d=o->GetPosition()-m->pos;
            n.motion.bodyOffset={d.x*m->right.x+d.y*m->right.y+d.z*m->right.z,d.x*m->forward.x+d.y*m->forward.y+d.z*m->forward.z,d.x*m->up.x+d.y*m->up.y+d.z*m->up.z};
            auto q=Quat::FromMatrix(*m);q.x=-q.x;q.y=-q.y;q.z=-q.z;n.motion.bodyRotation=Product(q,RotationOf(o));
        }
    }
}
CPhysical* Parent(const ObjectMotion&m,uint16_t object){
    if(m.parentKind&&m.parentKind!=3){uint8_t kind=m.parentKind==1?ENTITY_VEHICLE:m.parentKind==2?ENTITY_PED:ENTITY_OBJECT;if(!Net().EntityLink(ENTITY_OBJECT,object,kind,m.parentId))return nullptr;}
    if(m.parentKind==1){auto*v=Vehicles::Instance().Find(m.parentId);return v&&Vehicles::Alive(*v)?v->veh:nullptr;}
    if(m.parentKind==2)return GetPed(Peds::Instance().HandleOf(m.parentId));
    if(m.parentKind==3){if(m.parentId==Net().MyId())return FindPlayerPed(0);auto*p=Players::Instance().Get((uint8_t)m.parentId);return p?p->ped:nullptr;}
    if(m.parentKind==4)return SharedMissionObjectById(m.parentId);
    return nullptr;
}
using AttachedPositionFn=void(__fastcall*)(CPhysical*,void*);AttachedPositionFn originalAttachedPosition=nullptr;
void __fastcall AttachedPosition(CPhysical*object,void*edx){
    NetObject*n=object&&object->IsObject()?FindByObject(Net().IsHost()?g_owned:g_mirrors,(CObject*)object):nullptr;
    bool replica=n&&(!Net().IsHost()||n->motion.simulator!=255)&&n->motion.simulator!=Net().MyId()&&n->motion.holder!=Net().MyId();
    if(replica&&n->motion.parentKind){
        auto*parent=Parent(n->motion,n->netId);auto*m=parent?parent->Matrix():nullptr;auto*om=object->Matrix();if(!m||!om)return;
        auto d=n->motion.bodyOffset;auto p=m->pos+m->right*d.x+m->forward*d.y+m->up*d.z;
        // GTA uses the forklift's rendered fork-node LTM for this attachment.
        // A culled forklift never updates that node, giving each camera a
        // different cargo height. Replicas use the simulator's physical body
        // relation and never independently decide to detach the cargo.
        Product(Quat::FromMatrix(*m),n->motion.bodyRotation).ToMatrix(*om);om->pos=p;
        CallMethod<0x446F90>(object);CallMethod<0x532B00>(object);return;
    }
    originalAttachedPosition(object,edx);
}
void ApplyMotion(NetObject&n,bool simulator=false){
    if(!Alive(n))return;auto*o=n.obj;
    if((n.motion.holder==Net().MyId()&&Holding(FindPlayerPed(0),o))||(!simulator&&n.motion.simulator==Net().MyId()))return;
    if(n.motion.parentKind){
        uint8_t kind=n.motion.parentKind==1?ENTITY_VEHICLE:n.motion.parentKind==2?ENTITY_PED:ENTITY_OBJECT;
        if(n.motion.parentKind!=3&&!Net().EntityLink(ENTITY_OBJECT,n.netId,kind,n.motion.parentId)){if(o->AttachedTo())Cmd(0x0682).I(n.handle).F(0).F(0).F(0).I(0).Run();return;}
        auto*parent=Parent(n.motion,n.netId);if(!parent)return; // retain the relation while its dependency streams
        if(o->AttachedTo()!=parent){if(o->AttachedTo())Cmd(0x0682).I(n.handle).F(0).F(0).F(0).I(0).Run();CallMethod<0x54D690>(o,parent,&n.motion.offset,&n.motion.rotation);}
        Field<CVector>(o,0x100)=n.motion.offset;Field<CVector>(o,0x10C)=n.motion.euler;Field<Quat>(o,0x118)=n.motion.rotation;AttachedPosition(o,nullptr);
    }else if(o->AttachedTo())Cmd(0x0682).I(n.handle).F(0).F(0).F(0).I(0).Run();
    if(simulator){Cmd(0x0550).I(n.handle).I(n.motion.dynamic?0:1).Run();o->MoveSpeed()=n.motion.velocity;o->TurnSpeed()=n.motion.turn;}
}
enum:uint8_t{CLAIM=1,GRANT=2,CARRY_POSE=3,RELEASE=4,DENY=5,PUSH=6,CARGO_CLAIM=7,CARGO_POSE=8,CARGO_RELEASE=9};
void Decision(NetObject&n,uint8_t op=GRANT){
    Writer w(proto::G_OBJ_CARRY);w.U64(StoryRunId());w.U8(op);w.Entity(ENTITY_OBJECT,n.netId);w.U32(n.motion.revision);w.U8(n.motion.holder);w.U8(n.motion.simulator);w.Entity(ENTITY_VEHICLE,n.motion.supportVehicle);Net().Send(proto::CH_RELIABLE,w);
}
bool Claim(NetObject&n,uint8_t from){
    if(!Alive(n)||n.broken)return false;
    if(n.motion.holder==from){n.lease=NowMs()+2500;Decision(n);return true;}
    if(n.motion.holder!=255){Decision(n,DENY);return false;}
    CVector at;int8_t area;
    if(from==Net().MyId()){at=FindPlayerPed(0)->GetPosition();area=FindPlayerPed(0)->AreaCode();}
    else{auto*p=Players::Instance().Get(from);if(!p||!p->hasState||NowMs()-p->stateRecvMs>1500)return false;at=p->state.pos;area=p->state.area;}
    if((area!=n.obj->AreaCode()&&n.obj->AreaCode()!=13)||(at-n.obj->GetPosition()).Length()>5){LOGD("Shared object %04X claim too far: peer %u area %d/%d distance %.2f",n.netId,from,area,n.obj->AreaCode(),(at-n.obj->GetPosition()).Length());return false;}
    CaptureMotion(n);n.motion.simulator=255;n.motion.supportVehicle=0;n.motion.holder=from;++n.motion.revision;n.lease=NowMs()+2500;
    if(from!=Net().MyId())Cmd(0x0550).I(n.handle).I(1).Run();
    Decision(n);
    LOGI("Shared object %04X carry granted to %u revision %u",n.netId,from,n.motion.revision);return true;
}
using HoldFn=bool(__fastcall*)(void*,void*,CPed*);HoldFn originalHold=nullptr,originalHoldPosition=nullptr;
int HoldGate(void*task,CPed*ped){
    auto*e=Field<CEntity*>(task,8);
    if(Net().Online()&&core::IsPlayerReady()&&ped==FindPlayerPed(0)&&e&&e->IsObject()){
        auto&map=Net().IsHost()?g_owned:g_mirrors;auto*n=FindByObject(map,(CObject*)e);
        if(n){
            if(n->broken){CallMethod<0x6916E0>(task);return -1;}
            if(n->motion.holder!=Net().MyId()){
                if(n->motion.holder!=255){CallMethod<0x6916E0>(task);return -1;}
                if(Net().IsHost())Claim(*n,Net().MyId());
                else if(NowMs()<n->claimDeniedUntil){CallMethod<0x6916E0>(task);return -1;}
                else if(n->claimBegan&&NowMs()-n->claimBegan>2000){n->claimBegan=0;n->claimDeniedUntil=NowMs()+300;CallMethod<0x6916E0>(task);return -1;}
                else if(NowMs()-n->claimSent>300){if(!n->claimBegan)n->claimBegan=NowMs();Writer w(proto::G_OBJ_CARRY);w.U64(StoryRunId());w.U8(CLAIM);w.Entity(ENTITY_OBJECT,n->netId);w.U32(n->motion.revision);Net().Send(proto::CH_RELIABLE,w);n->claimSent=NowMs();}
                if(n->motion.holder!=Net().MyId())return 0;
            }
            n->claimBegan=0;n->lease=NowMs()+2500;
        }
    }
    return 1;
}
bool __fastcall Hold(void*task,void*edx,CPed*ped){int gate=HoldGate(task,ped);return gate<0?true:gate?originalHold(task,edx,ped):false;}
bool __fastcall HoldPosition(void*task,void*edx,CPed*ped){
    if(HoldGate(task,ped)!=1)return false;
    auto*e=Field<CEntity*>(task,8);
    // SetPedPosition is called independently of ProcessPed. Moving an object
    // while its claim is pending leaves collision enabled and can lift the
    // carrier on the very object he is trying to pick up.
    if(ped==FindPlayerPed(0)&&e&&e->IsObject()&&IsSharedMissionObject((CObject*)e)&&(e->Flags()&1u))return false;
    return originalHoldPosition(task,edx,ped);
}
CVector LimitCargo(CVector v,float max){auto size=v.Length();return size>max?v*(max/size):v;}
bool PhysicalCargo(const NetObject&n){return !n.broken&&n.motion.holder==255&&n.motion.simulator==255&&!n.motion.parentKind&&
    (n.motion.dynamic||(n.obj->Mass()>0&&n.obj->Mass()<99998.f))&&!(n.obj->PhysicalFlags()&(32u|64u));}
void CargoTick(){
    if(NowMs()-g_pushSent>=33){g_pushSent=NowMs();int budget=8;
        for(auto i=g_pushes.begin();i!=g_pushes.end()&&budget-->0;){auto c=i->second;i=g_pushes.erase(i);
            auto*v=Vehicles::Instance().Find(c.vehicle);auto o=g_mirrors.find(c.object);
            if(!v||!Vehicles::Alive(*v)||v->owner!=Net().MyId()||o==g_mirrors.end()||!Alive(o->second)||!PhysicalCargo(o->second))continue;
            Writer w(proto::G_OBJ_CARRY);w.U64(StoryRunId());w.U8(PUSH);w.Entity(ENTITY_OBJECT,c.object);w.U32(c.revision);w.Entity(ENTITY_VEHICLE,c.vehicle);
            if(++g_pushSequence==0)++g_pushSequence;w.U32(g_pushSequence);auto m=LimitCargo(c.move,20),t=LimitCargo(c.turn,12);
            w.VecQ({m.x,m.y,m.z},30);w.VecQ({t.x,t.y,t.z},15);Net().Send(proto::CH_RELIABLE,w);
        }
    }
    for(auto i=g_nativeCargo.begin();i!=g_nativeCargo.end();)i=NowMs()-i->second>1000?g_nativeCargo.erase(i):std::next(i);
}
void SimulateCargo(){
    auto&map=Net().IsHost()?g_owned:g_mirrors;auto now=NowMs();
    for(auto&[id,n]:map){
        if(!Alive(n)||n.broken||n.motion.holder!=255)continue;
        if(n.motion.simulator==Net().MyId()){
            auto*v=Vehicles::Instance().Find(n.motion.supportVehicle);auto*me=FindPlayerPed(0);
            bool valid=v&&Vehicles::Alive(*v)&&v->owner==Net().MyId()&&me->VehicleIfIn()==v->veh&&
                (n.obj->GetPosition()-v->veh->GetPosition()).Length()<25;
            bool held=valid&&(n.obj->AttachedTo()==v->veh||now-n.cargoTouch<1800);
            if(now-n.claimSent<50)continue;n.claimSent=now;CaptureMotion(n);
            Writer w(proto::G_OBJ_CARRY);w.U64(StoryRunId());w.U8(held?CARGO_POSE:CARGO_RELEASE);w.Entity(ENTITY_OBJECT,id,ENTITY_SNAPSHOT);w.U32(n.motion.revision);
            auto p=n.obj->GetPosition();w.Vec({p.x,p.y,p.z});RotationOf(n.obj).Write(w);n.motion.Write(w);w.U8(ObjectFlags(n.obj));
            Net().Send(held?proto::CH_STATE:proto::CH_RELIABLE,w);
            if(!held){n.motion.simulator=255;Cmd(0x0550).I(n.handle).I(1).Run();}
            continue;
        }
        if(Net().IsHost()){
            if(n.motion.simulator!=255){
                if(now>n.lease){n.motion.simulator=255;n.motion.supportVehicle=0;++n.motion.revision;ApplyMotion(n,true);Decision(n);}
                else if((n.obj->GetPosition()-n.pos).Length()>.005f)Place(n.obj,n.pos,n.rot);
            }
            continue;
        }
        // Pre-authorize a liftable box under the local forklift's forks. GTA's
        // moving-component contacts need the cargo to be dynamic before they
        // can lift it, so forwarding only velocity impulses is insufficient.
        if(n.motion.simulator==255&&!n.motion.parentKind&&(n.motion.dynamic||(n.obj->Mass()>0&&n.obj->Mass()<99998.f))&&now-n.cargoAsk>500){
            auto*car=FindPlayerPed(0)->VehicleIfIn();auto*v=car?Vehicles::Instance().Find(car):nullptr;
            if(v&&v->owner==Net().MyId()&&v->model==530&&car->Matrix()){
                auto d=n.obj->GetPosition()-car->GetPosition();auto&m=*car->Matrix();float x=d.x*m.right.x+d.y*m.right.y;float y=d.x*m.forward.x+d.y*m.forward.y;
                if(std::fabs(x)<1.5f&&y>.3f&&y<4.f&&std::fabs(d.z)<3.5f){Writer w(proto::G_OBJ_CARRY);w.U64(StoryRunId());w.U8(CARGO_CLAIM);w.Entity(ENTITY_OBJECT,id);w.U32(n.motion.revision);w.Entity(ENTITY_VEHICLE,v->netId);Net().Send(proto::CH_RELIABLE,w);n.cargoAsk=now;}
            }
        }
    }
}
void CarryTick(){
    CargoTick();SimulateCargo();
    auto&map=Net().IsHost()?g_owned:g_mirrors;
    for(auto&[id,n]:map){
        if(!Alive(n))continue;
        if(n.motion.holder==Net().MyId()){
            bool held=Holding(FindPlayerPed(0),n.obj)!=nullptr;
            if(!held&&NowMs()-n.claimSent<300)continue;
            if(Net().IsHost()){
                if(held)n.lease=NowMs()+2500;
                else{n.motion.holder=255;++n.motion.revision;CaptureMotion(n);Decision(n);}
            }else if(NowMs()-n.claimSent>=50){
                n.claimSent=NowMs();CaptureMotion(n);auto p=n.obj->GetPosition();Writer w(proto::G_OBJ_CARRY);w.U64(StoryRunId());
                w.U8(held?CARRY_POSE:RELEASE);w.Entity(ENTITY_OBJECT,id,ENTITY_SNAPSHOT);w.U32(n.motion.revision);w.Vec({p.x,p.y,p.z});RotationOf(n.obj).Write(w);n.motion.Write(w);w.U8(ObjectFlags(n.obj));Net().Send(held?proto::CH_STATE:proto::CH_RELIABLE,w);
                if(!held)n.motion.holder=255;
            }
        }else if(Net().IsHost()&&n.motion.holder!=255&&NowMs()>n.lease){
            n.motion.holder=255;++n.motion.revision;ApplyMotion(n,true);Decision(n);LOGW("Shared object %04X carry lease expired",id);
        }
        if(!Net().IsHost()||n.motion.holder!=255)ApplyMotion(n);
        // Native soft contacts can displace even frozen replicas. Only the
        // authority may retain that displacement; the driver's contact impulse
        // is already forwarded above. Do not let a third observer keep a box
        // at the previous fork height after the truck has stopped.
        if(!Net().IsHost()&&n.motion.holder==255&&n.motion.simulator!=Net().MyId()&&!n.motion.parentKind&&
           (n.obj->GetPosition()-n.pos).Length()>.005f)Place(n.obj,n.pos,n.rot);
    }
}
void HandleCarry(uint8_t from,Reader&r){
    auto run=r.U64();if(run!=StoryRunId())return;
    uint8_t op=r.U8();uint16_t id=r.U16();uint32_t revision=r.U32();
    if(!r.Ok()||!core::IsPlayerReady())return;
    if(Net().IsHost()){
        auto i=g_owned.find(id);if(i==g_owned.end()||!Alive(i->second))return;auto&n=i->second;
        if(op==CARGO_CLAIM){
            uint16_t vehicle=r.U16();auto*v=Vehicles::Instance().Find(vehicle);
            if(!r.Ok()||revision!=n.motion.revision||n.motion.holder!=255||n.motion.parentKind||
               (n.motion.simulator!=255&&n.motion.simulator!=from)||!v||!Vehicles::Alive(*v)||v->owner!=from||
               (n.obj->AreaCode()!=13&&v->veh->AreaCode()!=n.obj->AreaCode())||
               (v->veh->GetPosition()-n.obj->GetPosition()).Length()>10||
               !(n.motion.dynamic||(n.obj->Mass()>0&&n.obj->Mass()<99998.f)))return;
            CaptureMotion(n);n.motion.simulator=from;n.motion.supportVehicle=vehicle;++n.motion.revision;n.lease=NowMs()+2500;
            n.pos=n.obj->GetPosition();n.rot=RotationOf(n.obj);Cmd(0x0550).I(n.handle).I(1).Run();Decision(n);
            LOGI("Shared cargo %04X simulator granted to %u vehicle %04X",id,from,vehicle);return;
        }
        if(op==CARGO_POSE||op==CARGO_RELEASE){
            auto p=r.Vec();Quat q;q.Read(r);ObjectMotion m;if(!m.Read(r))return;uint8_t flags=r.U8()&3;
            auto*v=Vehicles::Instance().Find(n.motion.supportVehicle);CVector pos{p.x,p.y,p.z};
            if(!r.Ok()||n.motion.holder!=255||n.motion.simulator!=from||revision!=n.motion.revision||!v||!Vehicles::Alive(*v)||
               (op==CARGO_POSE&&v->owner!=from)||(pos-v->veh->GetPosition()).Length()>50||(m.parentKind&&(m.parentKind!=1||m.parentId!=n.motion.supportVehicle)))return;
            uint16_t support=n.motion.supportVehicle;n.motion=m;n.motion.simulator=from;n.motion.supportVehicle=support;n.motion.revision=revision;
            n.pos=pos;n.rot=q;n.area=v->veh->AreaCode();n.obj->AreaCode()=n.area;n.flags=flags;n.lease=NowMs()+2500;
            Place(n.obj,pos,q);ApplyFlags(n.obj,flags);ApplyMotion(n);n.sentMs=0;
            if(op==CARGO_RELEASE){n.motion.simulator=255;n.motion.supportVehicle=0;++n.motion.revision;ApplyMotion(n,true);Decision(n);}
            return;
        }
        if(op==PUSH){
            uint16_t vehicle=r.U16();uint32_t serial=r.U32();auto m=r.VecQ(30),t=r.VecQ(15);auto*v=Vehicles::Instance().Find(vehicle);
            if(!r.Ok()||from>=3||!PhysicalCargo(n)||revision!=n.motion.revision||!v||!Vehicles::Alive(*v)||v->owner!=from||
               (n.obj->AreaCode()!=13&&v->veh->AreaCode()!=n.obj->AreaCode())||(v->veh->GetPosition()-n.obj->GetPosition()).Length()>20||!g_pushSeen[from].Accept(serial))return;
            uint32_t key=((uint32_t)vehicle<<16)|id;
            if(auto i=g_nativeCargo.find(key);i!=g_nativeCargo.end()&&NowMs()-i->second<120)return;
            Cmd(0x0392).I(n.handle).I(1).Run();Cmd(0x0550).I(n.handle).I(0).Run();
            n.obj->MoveSpeed()+=LimitCargo({m.x,m.y,m.z},20)*(1.f/50.f);
            n.obj->TurnSpeed()+=LimitCargo({t.x,t.y,t.z},12)*(1.f/50.f);n.motion.dynamic=true;n.sentMs=0;
            LOGD("Shared cargo %04X native impulse from car %04X player %u",id,vehicle,from);return;
        }
        if(op==CLAIM){LOGD("Shared object %04X claim from %u revision %u current %u",id,from,revision,n.motion.revision);if(revision!=n.motion.revision){Decision(n);return;}Claim(n,from);return;}
        if(op!=CARRY_POSE&&op!=RELEASE)return;
        auto p=r.Vec();Quat q;q.Read(r);ObjectMotion motion;
        if(!motion.Read(r))return;uint8_t flags=r.U8()&3;
        if(!r.Ok()||n.broken||n.motion.holder!=from||revision!=n.motion.revision)return;
        auto*player=Players::Instance().Get(from);CVector pos{p.x,p.y,p.z};
        if(!player||!player->hasState||NowMs()-player->stateRecvMs>1500||(pos-player->state.pos).Length()>6)return;
        n.pos=pos;n.rot=q;n.motion=motion;n.motion.holder=from;n.motion.revision=revision;n.lease=NowMs()+2500;
        Place(n.obj,pos,q);n.area=player->state.area;n.obj->AreaCode()=n.area;n.flags=flags;ApplyFlags(n.obj,flags);n.sentMs=0;
        if(op==RELEASE){n.motion.holder=255;++n.motion.revision;ApplyMotion(n,true);Decision(n);}
    }else if(from==Net().HostId()&&(op==GRANT||op==DENY)){
        uint8_t holder=r.U8(),simulator=r.U8();uint16_t support=r.U16();if(!r.Ok()||(holder>=3&&holder!=255)||(simulator>=3&&simulator!=255))return;
        for(auto*map:{&g_mirrors,&g_pending})if(auto i=map->find(id);i!=map->end()&&(int32_t)(revision-i->second.motion.revision)>=0){auto&n=i->second;n.motion.holder=holder;n.motion.revision=revision;n.motion.simulator=simulator;n.motion.supportVehicle=support;
            if(Alive(n)&&simulator==Net().MyId()){Cmd(0x0392).I(n.handle).I(1).Run();Cmd(0x0550).I(n.handle).I(0).Run();n.motion.dynamic=true;n.cargoTouch=NowMs();}
            else if(Alive(n)&&holder!=Net().MyId())Cmd(0x0550).I(n.handle).I(1).Run();}
    }
}
// host: announce new mission objects, report moves, breaks and ends
void HostScan() {
    if (HostReplicating()) {
        auto handles=g_storyObjects;
        for(int i=0;i<75;++i){uintptr_t e=0xA90850+(uintptr_t)i*8;if(Ref<uint8_t>(e)==3)handles.insert(Ref<int32_t>(e+4));}
        for(int h:handles){
            auto*o=GetObject(h);
            if(!o){g_storyObjects.erase(h);continue;}
            if(FindByObject(g_owned,o))continue;
            TrackObject(h);
        }
    }
    for (auto it = g_owned.begin(); it != g_owned.end();) {
        NetObject& no = it->second;
        if (!Alive(no) || no.obj->ModelIndex() != no.model) {
            SendRemove(no.netId, REMOVE_GONE, 0);g_storyObjects.erase(no.handle);
            g_ids.Retire(it->first,NowMs());it = g_owned.erase(it);
            continue;
        }
        if (Broken(no.obj)) {
            if (!no.broken) {
                no.broken = true;
                SendRemove(no.netId, REMOVE_BROKE, Field<uint8_t>(no.obj, 0x148));  // CObject::m_nLastWeaponDamage
                LOGI("Mission object %04X broke", no.netId);
            }
        } else {
            no.broken = false;
            CVector pos = no.obj->GetPosition();
            Quat rot = RotationOf(no.obj);
            float turn = std::fabs(rot.x - no.rot.x) + std::fabs(rot.y - no.rot.y) + std::fabs(rot.z - no.rot.z) +
                         std::fabs(rot.w - no.rot.w);
            auto previous=no.motion;
            if(no.motion.simulator==255&&(no.motion.holder==255||no.motion.holder==Net().MyId()))CaptureMotion(no);
            bool discrete=no.area!=no.obj->AreaCode()||no.flags!=ObjectFlags(no.obj)||previous.parentKind!=no.motion.parentKind||previous.parentId!=no.motion.parentId||previous.revision!=no.motion.revision;
            if ((discrete||(pos - no.pos).Length() > 0.02f || turn > 0.01f||NowMs()-no.sentMs>1000) && NowMs() - no.sentMs >= 100) {
                no.pos = pos;
                no.rot = rot;
                no.area=no.obj->AreaCode();no.flags=ObjectFlags(no.obj);
                no.sentMs = NowMs();
                Writer w(proto::G_OBJ_SYNC);
                w.Entity(ENTITY_OBJECT,no.netId,ENTITY_SNAPSHOT);
                w.Vec({pos.x, pos.y, pos.z});
                rot.Write(w);w.I8(no.area);w.U8(no.flags);no.motion.Write(w);
                Net().Send(discrete?proto::CH_RELIABLE:proto::CH_STATE, w);
            }
        }
        ++it;
    }
}

// partner: break the copy as the host's object broke (the same damage path shows the same shards)
void BreakCopy(NetObject& no, uint8_t weapon) {
    if (!Alive(no) || Broken(no.obj)) return;
    CVector p = no.obj->GetPosition(), d{0, 0, 1};
    g_applying = true;
    CallMethod<0x5A0D90>(no.obj, 10000.0f, &p, &d, (CEntity*)nullptr, (int)weapon);  // CObject::ObjectDamage
    g_applying = false;
    if (!Broken(no.obj)) {  // nothing to smash on it: just gone
        Cmd(0x0750).I(no.handle).I(0).Run();  // SET_OBJECT_VISIBLE
        Cmd(0x0382).I(no.handle).I(0).Run();  // SET_OBJECT_COLLISION
    }
}

void DeleteCopy(NetObject& no) {
    if(!Alive(no))return;
    if(!no.adopted)Cmd(0x0108).I(no.handle).Run();
    else { // Preserve a local script's handle, but the host removed this barrier.
        Cmd(0x0750).I(no.handle).I(0).Run();Cmd(0x0382).I(no.handle).I(0).Run();
    }
}

// the same object in the same place, not a copy yet: the host's world, loaded here, brought it along
CObject* FindTwin(const NetObject& no) {
    CPoolHeader* pool = ObjectPool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* o = (CObject*)(pool->objects + (size_t)i * kObjectPoolElemSize);
        if (o->ModelIndex() != no.model || (o->GetPosition() - no.pos).Length() > 0.25f || FindByObject(g_mirrors, o)) continue;
        return o;
    }
    return nullptr;
}

void ClearCopies() {
    for (auto& [id, no] : g_mirrors) DeleteCopy(no);
    g_mirrors.clear();
    g_pending.clear();
}

void CreatePending() {
    profile::Scope timing(profile::ENTITY_CREATE);
    int budget=4;
    for (auto it = g_pending.begin(); it != g_pending.end()&&budget>0;) {
        NetObject no = it->second;
        if (!IsModelLoaded(no.model)) {
            RequestModel(no.model,0);
            ++it;
            continue;
        }
        if(!HasRoom(ObjectPool(),kKeepObjects,"object"))break;
        --budget;
        int h = -1;
        CObject* o = FindTwin(no);
        no.adopted = o != nullptr;
        if (o) h = GetObjectRef(o);
        else Cmd(0x029B).I(no.model).F(no.pos.x).F(no.pos.y).F(no.pos.z).Out(&h).Run();  // CREATE_OBJECT_NO_OFFSET
        if (!o) {o = h >= 0 ? GetObject(h) : nullptr;if(o)Field<uint8_t>(o,0x13C)=6;}
        if (!o) {++it;continue;} // pool/model exhaustion is retried, not silently lost
        it=g_pending.erase(it);
        Cmd(0x0550).I(h).I(1).Run();  // FREEZE_OBJECT_POSITION
        o->AreaCode() = no.area;
        Place(o, no.pos, no.rot);
        ApplyFlags(o,no.flags);
        no.obj = o;
        no.handle = h;
        bool broken = no.broken;
        no.broken = false;
        NetObject& copy = g_mirrors[no.netId] = no;
        if (broken) BreakCopy(copy, 0);
        LOGI("Mission object %04X (model %d) %s", no.netId, no.model, no.adopted ? "found here" : "copied");
    }
}

// The partner's player hits a copy: the host's object takes it. Nothing else damages a copy.
using ObjectDamageFn = void(__fastcall*)(CObject*, void*, float, CVector*, CVector*, CEntity*, int);
ObjectDamageFn g_origObjectDamage = nullptr;
// the other game's car or person, as this game's copy of it
bool OtherGames(CEntity* e) {
    if (e->IsVehicle()) {
        NetVehicle* nv = Vehicles::Instance().Find((CVehicle*)e);
        return nv && Vehicles::Instance().IsMirror(*nv);
    }
    if (!e->IsPed()) return false;
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.ped == e) return true;
    NetPed* np = Peds::Instance().Find((CPed*)e);
    return np && np->owner != Net().MyId();
}

void __fastcall HookObjectDamage(CObject* o, void* edx, float damage, CVector* origin, CVector* dir, CEntity* damager,
                                 int weapon) {
    // A map object (a lamp post, a sign) breaks in the game whose car or person really hit it, and WorldObjects sends
    // that over. This game's copy of the other game's car runs a little off the real one's path: it broke posts
    // the real one went past, and the other player saw them fall by themselves.
    // The host's mission objects the same: the other player's game sends its hits (G_OBJ_DAMAGE), and the replayed shots
    // of its copy here counted them twice.
    if (!g_applying && damager && Net().Online() && (Field<uint8_t>(o, 0x13C) == 1 || FindByObject(g_owned, o)) &&
        OtherGames(damager))
        return;
    if (!g_applying && Net().Online() && !g_mirrors.empty()) {
        if (NetObject* no = FindByObject(g_mirrors, o)) {
            CPlayerPed* me = FindPlayerPed(0);
            if (damager && me && (damager == me || (me->InVehicle() && damager == (CEntity*)me->Vehicle()))) {
                Writer w(proto::G_OBJ_DAMAGE);
                if(++g_hitSequence==0)++g_hitSequence;w.U32(g_hitSequence);
                w.Entity(ENTITY_OBJECT,no->netId);
                w.F32(damage);
                w.U8((uint8_t)weapon);
                Net().Send(proto::CH_RELIABLE, w);
            }
            return;
        }
    }
    g_origObjectDamage(o, edx, damage, origin, dir, damager, weapon);
}

class Objects : public Module {
public:
    const char* Name() const override { return "Objects"; }
    void CollectSyncAudit(std::vector<SyncAuditEntry>& out) override {
        auto append=[&](const NetObject& no,uint8_t owner){
            SyncAuditEntry e;e.kind=AUDIT_OBJECT;e.id=no.netId;e.owner=owner;e.model=no.model;
            e.area=no.area;e.pos=no.pos;e.alive=Alive(no);e.look=no.flags|(no.broken?4:0)|((uint32_t)no.motion.holder<<8)|((uint32_t)no.motion.simulator<<16);e.relation=((uint32_t)no.motion.parentKind<<28)|no.motion.parentId;out.push_back(e);
        };
        for(auto&[id,no]:g_owned)append(no,Net().MyId());
        for(auto&[id,no]:g_mirrors)append(no,Net().HostId());
        for(auto&[id,no]:g_pending)if(!g_mirrors.count(id))append(no,Net().HostId());
    }
    bool RepairSyncEntity(uint8_t kind,uint16_t id) override {
        if(kind!=AUDIT_OBJECT||!Net().IsHost())return false;
        auto it=g_owned.find(id);
        if(it!=g_owned.end()&&Alive(it->second))SendCreate(it->second);
        else SendRemove(id,REMOVE_GONE,0);
        return true;
    }

    void OnEntityReplaced(uint8_t kind,uint8_t,uint16_t id)override{
        if(kind!=ENTITY_OBJECT)return;
        for(auto*map:{&g_owned,&g_mirrors,&g_pending})if(auto i=map->find(id);i!=map->end()){if(Alive(i->second))Cmd(0x0108).I(i->second.handle).Run();map->erase(i);}
    }
    void OnInstallHooks() override {
        mem::HookFn(0x546FF0,&AttachedPosition,&originalAttachedPosition,"CPhysical::PositionAttachedEntity");
        mem::HookFn(0x693C40,&Hold,&originalHold,"CTaskSimpleHoldEntity::ProcessPed");
        mem::HookFn(0x6940A0,&HoldPosition,&originalHoldPosition,"CTaskSimpleHoldEntity::SetPedPosition");
        Net().SetHandler(proto::G_OBJ_CARRY,&HandleCarry);
        mem::HookFn(0x5A0D90, &HookObjectDamage, &g_origObjectDamage, "CObject::ObjectDamage");
        Net().SetHandler(proto::G_OBJ_CREATE, [](uint8_t from, Reader& r) {
            NetObject no;
            no.netId = r.U16();
            no.model = r.U16();
            Vec3 p = r.Vec();
            no.pos = {p.x, p.y, p.z};
            no.rot.Read(r);
            no.area = r.I8();
            uint8_t flags=r.U8();no.broken=(flags&4)!=0;no.flags=flags&3;
            if(!no.motion.Read(r))return;
            // not in the game yet (a partner waits in the menu): the host announces them again once it is (resync)
            if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !IsObjectModel(no.model)) return;
            if (auto it = g_mirrors.find(no.netId); it != g_mirrors.end()) {
                // A repair/resync is an update of the same object. Recreating
                // a valid support drops standing peds and breaks attachments.
                auto& copy=it->second;
                if(Alive(copy)&&copy.model==no.model){
                    if((int32_t)(no.motion.revision-copy.motion.revision)<0)return;
                    copy.pos=no.pos;copy.rot=no.rot;copy.area=no.area;copy.flags=no.flags;copy.motion=no.motion;
                    copy.obj->AreaCode()=no.area;if(copy.motion.holder!=Net().MyId()&&copy.motion.simulator!=Net().MyId())Place(copy.obj,no.pos,no.rot);ApplyMotion(copy);
                    if(no.broken)BreakCopy(copy,0);else ApplyFlags(copy.obj,no.flags);
                    copy.broken=no.broken;return;
                }
                DeleteCopy(it->second);
                g_mirrors.erase(it);
            }
            g_pending[no.netId] = no;
            RequestModel(no.model,0);
        });
        Net().SetHandler(proto::G_OBJ_SYNC, [](uint8_t from, Reader& r) {
            uint16_t id = r.U16();
            Vec3 p = r.Vec();
            Quat q;
            q.Read(r);int8_t area=r.I8();uint8_t flags=r.U8()&3;ObjectMotion motion;if(!motion.Read(r))return;
            if (!r.Ok() || from != Net().HostId() || !core::IsPlayerReady()) return;
            if (auto it = g_pending.find(id); it != g_pending.end()) {
                if((int32_t)(motion.revision-it->second.motion.revision)<0)return;
                it->second.pos = {p.x, p.y, p.z};
                it->second.rot = q;it->second.area=area;it->second.flags=flags;it->second.motion=motion;
            } else if (auto m = g_mirrors.find(id); m != g_mirrors.end() && Alive(m->second)) {
                auto&copy=m->second;if((int32_t)(motion.revision-copy.motion.revision)<0)return;
                copy.pos={p.x,p.y,p.z};copy.rot=q;copy.area=area;copy.flags=flags;copy.motion=motion;
                copy.obj->AreaCode()=area;
                if(copy.motion.holder!=Net().MyId()&&copy.motion.simulator!=Net().MyId()){Place(copy.obj,copy.pos,q);ApplyFlags(copy.obj,flags);}
                ApplyMotion(copy);
            }
        });
        Net().SetHandler(proto::G_OBJ_REMOVE, [](uint8_t from, Reader& r) {
            uint16_t id = r.U16();
            uint8_t kind = r.U8(), weapon = r.U8();
            if (!r.Ok() || from != Net().HostId() || !core::IsPlayerReady()) return;
            if (kind == REMOVE_BROKE) {
                if (auto it = g_pending.find(id); it != g_pending.end()) it->second.broken = true;
                if (auto it = g_mirrors.find(id); it != g_mirrors.end()) {BreakCopy(it->second, weapon);it->second.broken=true;}
                return;
            }
            // CREATE and REMOVE can arrive in one batch before Copy runs. The
            // local level may already have the barrier even without our mirror.
            if(auto it=g_pending.find(id);it!=g_pending.end()){
                if(auto*original=FindTwin(it->second)){
                    auto removed=it->second;removed.obj=original;removed.handle=GetObjectRef(original);removed.adopted=true;
                    DeleteCopy(removed);
                }
                g_pending.erase(it);
            }
            if (auto it = g_mirrors.find(id); it != g_mirrors.end()) {
                DeleteCopy(it->second);
                g_mirrors.erase(it);
            }
        });
        Net().SetHandler(proto::G_OBJ_DAMAGE, [](uint8_t from, Reader& r) {
            uint32_t sequence=r.U32();
            uint16_t id = r.U16();
            float damage = r.F32();
            uint8_t weapon = r.U8();
            if (!r.Ok() || from>=3 || !Net().IsHost() || !core::IsPlayerReady()||damage<=0||damage>10000||weapon>58) return;
            auto it = g_owned.find(id);
            if (it == g_owned.end() || !Alive(it->second)) return;
            if(it->second.broken)return;
            auto*player=Players::Instance().Get(from);
            if(!player||!player->hasState||NowMs()-player->stateRecvMs>1500||(player->state.area!=it->second.area&&it->second.area!=13)||
               (player->state.pos-it->second.obj->GetPosition()).Length()>450||!g_hitsSeen[from].Accept(sequence))return;
            CObject* o = it->second.obj;
            CVector p = o->GetPosition(), d{0, 0, 1};
            // the mission counts its player's hits: the partner's count as his
            CallMethod<0x5A0D90>(o, damage, &p, &d, (CEntity*)FindPlayerPed(0), (int)weapon);  // CObject::ObjectDamage
            RecordStoryEntityDamage(o,from,weapon);
            LOGI("Mission object %04X hit by player %d (%.0f)", id, from, damage);
        });
    }

    void OnSessionEnd() override {
        ClearCopies();
        g_owned.clear();g_storyObjects.clear();
    }
    void OnPeerLeave(uint8_t id) override {
        if(id<3){g_hitsSeen[id]={};g_pushSeen[id]={};}
        if(Net().IsHost())for(auto&[other,n]:g_owned)if(n.motion.holder==id||n.motion.simulator==id){n.motion.holder=255;n.motion.simulator=255;n.motion.supportVehicle=0;n.motion.parentKind=0;n.motion.parentId=0;++n.motion.revision;ApplyMotion(n,true);Decision(n);}
        if (id == Net().HostId() || Net().IsHost()) ClearCopies();
    }
    void OnPeerResync(uint8_t id) override {
        if (!Net().IsHost()) {  // the host's game started over: the objects of its old world are gone
            if (id == Net().HostId()) ClearCopies();
            return;
        }
        // The returning native ped has no old hold task. Drop its cargo at
        // the last authoritative position and free the lease before replaying
        // the object snapshot; never attach it to a replacement player body.
        for(auto&[object,n]:g_owned)if(n.motion.holder==id||n.motion.simulator==id){n.motion.holder=n.motion.simulator=255;n.motion.supportVehicle=0;n.motion.parentKind=0;n.motion.parentId=0;++n.motion.revision;ApplyMotion(n,true);Decision(n);}
        for (auto& [netId, no] : g_owned)
            if (Alive(no)) SendCreate(no);
    }
    void OnGameStarted() override {  // a new world: the objects of the old one are gone
        for(auto&w:g_hitsSeen)w={};for(auto&w:g_pushSeen)w={};g_pushes.clear();g_nativeCargo.clear();
        g_owned.clear();g_storyObjects.clear();
        g_mirrors.clear();
        g_pending.clear();
    }
    void OnGameShutdown() override { OnGameStarted(); }  // nothing to touch while the world loads

    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        CarryTick();
        if (Net().IsHost()) {
            if (NowMs() - g_scanMs >= 100) {
                g_scanMs = NowMs();
                HostScan();
            }
        } else {
            for(auto it=g_mirrors.begin();it!=g_mirrors.end();){
                if(!Alive(it->second)){auto copy=it->second;copy.obj=nullptr;copy.handle=-1;g_pending[it->first]=copy;it=g_mirrors.erase(it);}
                else ++it;
            }
            if(!g_pending.empty())CreatePending();
        }
    }

    static Objects& Instance() {
        static Objects m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Objects::Instance()); }
} g_reg;
}  // namespace

void ShareMissionObject(int handle){
    if(!Net().IsHost()||!HostReplicating()||!GetObject(handle))return;
    g_storyObjects.insert(handle);
    // Capture before the command runs: its first reference can be DELETE_OBJECT.
    TrackObject(handle);
}

uint8_t SharedStoryObjectHolder(CObject*object){
    if(!object)return 255;auto*n=FindByObject(Net().IsHost()?g_owned:g_mirrors,object);return n?n->motion.holder:255;
}
void ReleaseStoryCargo(){
    if(!core::IsPlayerReady())return;
    auto&map=Net().IsHost()?g_owned:g_mirrors;
    for(auto&[id,n]:map)if(Alive(n)){
        auto*me=FindPlayerPed(0);if(Holding(me,n.obj)){Cmd(0x070B).I(GetPedRef(me)).I(1).Run();LOGI("Story carry released at run boundary object=%u",id);}
        if(Net().IsHost()){n.motion.holder=n.motion.simulator=255;n.motion.supportVehicle=0;n.motion.parentKind=0;n.motion.parentId=0;++n.motion.revision;ApplyMotion(n,true);Decision(n);}
    }
}

bool ProcessSharedCargoContact(CargoContactFn original,CPhysical*a,void*edx,CPhysical*b,void*point,float*x,float*y,bool&handled){
    CObject*object=a->IsObject()?(CObject*)a:b->IsObject()?(CObject*)b:nullptr;
    CVehicle*car=a->IsVehicle()?(CVehicle*)a:b->IsVehicle()?(CVehicle*)b:nullptr;
    if(!object||!car)return false;
    auto&map=Net().IsHost()?g_owned:g_mirrors;auto*n=FindByObject(map,object);auto*v=Vehicles::Instance().Find(car);
    if(!n||!v)return false;
    if(n->motion.simulator==Net().MyId()&&n->motion.supportVehicle==v->netId){n->cargoTouch=NowMs();return false;}
    if(!PhysicalCargo(*n)||n->motion.simulator!=255)return false;
    if(!Net().IsHost()&&v->owner==Net().MyId()&&NowMs()-n->cargoAsk>500){
        Writer w(proto::G_OBJ_CARRY);w.U64(StoryRunId());w.U8(CARGO_CLAIM);w.Entity(ENTITY_OBJECT,n->netId);w.U32(n->motion.revision);w.Entity(ENTITY_VEHICLE,v->netId);Net().Send(proto::CH_RELIABLE,w);n->cargoAsk=NowMs();
    }
    if(Net().IsHost()){
        if(v->owner==Net().MyId())return false;
        auto before=object->MoveSpeed();handled=true;bool result=original(a,edx,b,point,x,y);
        if((object->MoveSpeed()-before).Length()>.00001f)g_nativeCargo[((uint32_t)v->netId<<16)|n->netId]=NowMs();return result;
    }
    if(v->owner!=Net().MyId())return false;
    // Calculate the real mass/contact response on the driver's copy. Freeze
    // it again after the calculation; the host applies the resulting impulse.
    auto flags=object->Flags();auto physical=object->PhysicalFlags();auto before=object->MoveSpeed(),turn=object->TurnSpeed();
    object->Flags()&=~4u;object->PhysicalFlags()&=~0x2000u;
    handled=true;bool result=original(a,edx,b,point,x,y);
    auto move=(object->MoveSpeed()-before)*50.f,spin=(object->TurnSpeed()-turn)*50.f;
    object->Flags()=(object->Flags()&~4u)|(flags&4u);object->PhysicalFlags()=(object->PhysicalFlags()&~0x2000u)|(physical&0x2000u);
    object->MoveSpeed()=before;object->TurnSpeed()=turn;
    if(move.Length()>.00001f||spin.Length()>.00001f){uint32_t key=((uint32_t)v->netId<<16)|n->netId;
        if(g_pushes.size()<256||g_pushes.count(key)){auto&c=g_pushes[key];c.object=n->netId;c.vehicle=v->netId;c.revision=n->motion.revision;c.move+=move;c.turn+=spin;}}
    return result;
}

bool IsSharedMissionObject(game::CObject*object){return FindByObject(g_owned,object)||FindByObject(g_mirrors,object);}
uint16_t SharedMissionObjectId(game::CObject*object){auto*n=FindByObject(Net().IsHost()?g_owned:g_mirrors,object);return n?n->netId:0;}
game::CObject* SharedMissionObjectById(uint16_t id){auto&map=Net().IsHost()?g_owned:g_mirrors;auto i=map.find(id);return i!=map.end()&&Alive(i->second)?i->second.obj:nullptr;}
bool SharedMissionObjectReady(uint16_t id){
    auto&map=Net().IsHost()?g_owned:g_mirrors;auto i=map.find(id);if(i==map.end()||!Alive(i->second))return false;
    auto&n=i->second;if(n.broken)return true;
    if(n.motion.parentKind){auto*parent=Parent(n.motion,id);if(!parent||n.obj->AttachedTo()!=parent)return false;}
    if(n.motion.holder==Net().MyId()&&!Holding(FindPlayerPed(0),n.obj))return false;
    return true;
}

}  // namespace coop
