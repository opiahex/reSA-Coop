#include <array>
#include <cmath>
#include <cstring>
#include "shared/api/story-rc/StoryRC.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/story-stage/StoryStage.h"
#include "shared/api/story-scripts/StoryScripts.h"
#include "shared/api/mission-objects/SharedObject.h"
#include "shared/api/net-session/Session.h"
#include "entities/mission/Mission.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/sync/EventWindow.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"

namespace coop { namespace {
using namespace game;
using game::script::Cmd;
constexpr uint8_t kMagnetInput=5;
// These are the native ZERO4 cargo groups, not additional copies of its mission.
constexpr int kBombModel=1636,kBarrelModel=1217,kPlankModel=2937,kMagnetModel=3056;
struct Source {
    void*script=nullptr;uint8_t*base=nullptr;uint64_t run=0;bool verified=false;
};
struct Cargo {
    uint16_t id=0;uint64_t generation=0;int handle=-1,local=-1,group=0;CObject*object=nullptr;
};
struct Hold {
    Cargo cargo;uint64_t run=0,carGeneration=0;uint32_t phase=0,assignment=0;
    uint16_t car=0;int carHandle=-1;CVehicle*vehicle=nullptr;bool clearance=true,collisionChanged=false,hadCollision=true;
};
struct Magnet {uint64_t run=0,carGeneration=0;uint16_t car=0;int handle=-1;CObject*object=nullptr;};
Source source;
std::array<Hold,3> held;
std::array<Magnet,3> magnets;
std::array<EventWindow,3> inputs;
std::array<uint64_t,3> releasedAt{},pressedAt{};
uint64_t localRun=0,began=0;uint32_t localSequence=0;bool buttonDown=false;
struct LocalCargo {uint64_t generation=0,carGeneration=0;uint32_t assignment=0,phase=0;uint16_t id=0;bool clearance=true,bombChecked=false;};
LocalCargo localCargo;

int32_t* Local(void*script,uint16_t index) {
    if(!script||!Field<uint8_t>(script,0xDC)||index>=1024)return nullptr;
    // Ask the interpreter for this mission's actual local bank. Do not assume
    // that mission locals are embedded in the small CRunningScript instance.
    alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);
    uint8_t descriptor[3]={3,(uint8_t)index,(uint8_t)(index>>8)};
    Field<uint8_t*>(copy,0x14)=descriptor;
    auto*p=CallMethod<0x464790,int32_t*>(copy,0);
    if(!p||((uintptr_t)p&3)||((uint8_t*)p>=copy&&(uint8_t*)p<copy+sizeof copy))return nullptr;
    return p;
}
int ReadLocal(uint16_t index) {auto*p=Local(source.script,index);return p?*p:-1;}
bool IsSourceScript(void*script) {
    return script&&script==source.script&&source.run&&source.run==StoryRunId()&&
        source.base==Field<uint8_t*>(script,0x10)&&IsSharedStoryScript(script)&&
        !_strnicmp(Field<char[8]>(script,8),"ZERO4",8)&&Field<uint8_t>(script,0xDC);
}
bool ExpectedObjectSlot(int slot,const char*name,int model) {
    // The game's used-object table resolves negative SCM model operands.
    uintptr_t entry=0xA44B70+(uintptr_t)slot*0x1C;
    return !_strnicmp((const char*)entry,name,24)&&Ref<int32_t>(entry+0x18)==model;
}
bool DefinitionReady(StoryRemoteRole&native) {
    if(!Net().IsHost()||!IsSourceScript(source.script)||!CooperativeRemoteMission("ZERO4")||
       !CooperativeRemoteRole(Net().MyId(),native)||!native.car||native.car->ModelIndex()!=501||
       native.car->AreaCode()!=10||GetVehicle(ReadLocal(70))!=native.car)return false;
    // A changed SCM/model table must fail closed instead of interpreting some
    // other mission's locals as the bomb timer or cargo handles.
    if(!ExpectedObjectSlot(247,"RCBOMB",kBombModel)||
       !ExpectedObjectSlot(248,"BARREL2",kBarrelModel)||
       !ExpectedObjectSlot(249,"MINI_MAGNET",kMagnetModel)||
       !ExpectedObjectSlot(250,"KMB_PLANK",kPlankModel))return false;
    auto*magnet=GetObject(ReadLocal(94));
    bool valid=magnet&&IsObjectValid(magnet)&&magnet->ModelIndex()==kMagnetModel&&magnet->AttachedTo()==native.car;
    source.verified|=valid;return valid;
}
bool NativeCargoAlive(const Cargo&c) {
    return c.id&&c.generation&&c.object&&IsObjectValid(c.object)&&GetObject(c.handle)==c.object&&
        (c.object->IsVisible()||(c.object->Flags()&1u))&&c.object->ModelIndex()==
        (c.group==0?kBarrelModel:c.group==1?kPlankModel:kBombModel);
}
bool CargoAlive(const Cargo&c) {
    return NativeCargoAlive(c)&&Net().EntityGeneration(ENTITY_OBJECT,c.id)==c.generation&&SharedMissionObjectById(c.id)==c.object;
}
Cargo CargoAt(int local,int group) {
    Cargo c;auto*o=GetObject(ReadLocal((uint16_t)local));if(!o||!IsObjectValid(o))return c;
    int model=group==0?kBarrelModel:group==1?kPlankModel:kBombModel;
    if(o->ModelIndex()!=model||o->AreaCode()!=10)return c;
    c.handle=GetObjectRef(o);c.object=o;c.local=local;c.group=group;
    ShareMissionObject(c.handle);c.id=SharedMissionObjectId(o);
    c.generation=c.id?Net().EntityGeneration(ENTITY_OBJECT,c.id):0;
    return c;
}
bool SameRole(const Hold&h,const StoryRemoteRole&r) {
    return h.run==r.run&&h.phase==r.phase&&h.assignment==r.assignment&&h.car==r.vehicle&&
        h.carGeneration==r.vehicleGeneration&&h.vehicle==r.car&&GetVehicle(h.carHandle)==r.car;
}
bool StillHeld(const Hold&h) {return CargoAlive(h.cargo)&&h.cargo.object->AttachedTo()==h.vehicle;}
bool Claimed(CObject*object) {for(auto&h:held)if(h.cargo.object==object&&CargoAlive(h.cargo))return true;return false;}
CVector PickupPoint(CVehicle*car) {
    auto*m=car?car->Matrix():nullptr;return m?m->pos-m->up:CVector{};
}
void ArmBomb(const Cargo&c) {
    if(c.group!=2||!CargoAlive(c)||!IsSourceScript(source.script)||!source.verified||ReadLocal(92)!=c.handle)return;
    auto*armed=Local(source.script,121);auto*timer=Local(source.script,102);
    if(!armed||!timer)return;
    *armed=1;*timer=0;Cmd(0x04D9).I(c.handle).I(1).Run();
    // Native ZERO4 still decides collision, delay, explosion and replacement.
}
void Drop(uint8_t player,bool arm,bool requireClear) {
    if(player>=held.size())return;auto&h=held[player];
    // Another module may delete the role car first, which natively detaches its
    // cargo. Our temporary collision change still belongs to this exact object.
    bool alive=CargoAlive(h.cargo)||(!Net().Online()&&NativeCargoAlive(h.cargo));
    if(alive){
        auto*parent=h.cargo.object->AttachedTo();bool ours=!parent||parent==h.vehicle;
        if(ours){
            if(parent&&requireClear&&Cmd(0x09FC).I(h.cargo.handle).Run())return;
            if(h.collisionChanged)Cmd(0x0382).I(h.cargo.handle).I(h.hadCollision?1:0).Run();
            if(parent)Cmd(0x0682).I(h.cargo.handle).F(0).F(0).F(0).I(0).Run();
            if(arm)ArmBomb(h.cargo);
            LOGI("Cooperative RC cargo released peer=%u object=%04X run=%llu",player,h.cargo.id,(unsigned long long)h.run);
        }
    }
    h={};releasedAt[player]=NowMs();
}
void ClearMagnet(uint8_t player,bool native) {
    if(player>=magnets.size())return;auto&m=magnets[player];
    if(native&&m.object&&IsObjectValid(m.object)&&GetObject(m.handle)==m.object)Cmd(0x0108).I(m.handle).Run();
    m={};
}
void Clear(bool native) {
    for(uint8_t p=0;p<3;++p){if(native)Drop(p,false,false);else held[p]={};ClearMagnet(p,native);inputs[p]={};}
    releasedAt={};pressedAt={};source={};localRun=0;localSequence=0;buttonDown=false;began=0;localCargo={};
}
bool RoleReady(uint8_t player,StoryRemoteRole&role) {
    if(player>=3||player==Net().HostId()||!CooperativeRemoteRole(player,role)||
       !role.car||role.car->ModelIndex()!=501||role.car->AreaCode()!=10||!role.car->Matrix())return false;
    auto*v=Vehicles::Instance().Find(role.vehicle);
    return v&&Vehicles::Alive(*v)&&v->owner==player&&v->hasState&&NowMs()-v->recvMs<1500&&
        role.run==source.run&&role.phase==CooperativeStoryPhase();
}
void MakeMagnet(uint8_t player,const StoryRemoteRole&r) {
    auto&m=magnets[player];
    if(m.object&&IsObjectValid(m.object)&&GetObject(m.handle)==m.object&&m.run==r.run&&
       m.car==r.vehicle&&m.carGeneration==r.vehicleGeneration&&m.object->AttachedTo()==r.car)return;
    ClearMagnet(player,true);if(!IsModelLoaded(kMagnetModel))return;
    int handle=-1;auto at=r.car->GetPosition();
    Cmd(0x0107).I(kMagnetModel).F(at.x).F(at.y).F(at.z).Out(&handle).Run();
    auto*object=GetObject(handle);if(!object)return;
    object->AreaCode()=10;
    Cmd(0x0681).I(handle).I(GetVehicleRef(r.car)).F(0).F(0).F(-.2f).F(0).F(0).F(0).Run();
    Cmd(0x050E).I(handle).I(GetVehicleRef(r.car)).Run();
    ShareMissionObject(handle);m={r.run,r.vehicleGeneration,r.vehicle,handle,object};
}
bool BelowCargoFloor(CObject*object,CVehicle*car,int group) {
    auto*m=object?object->Matrix():nullptr;if(!m||!car)return false;
    auto at=car->GetPosition();float ground=0;Cmd(0x02CE).F(at.x).F(at.y).F(at.z).Out(&ground).Run();
    if(!std::isfinite(ground))return false;
    float x=group==0?1.f:.2f,y=group==0?1.f:1.7f;
    for(auto offset:{CVector{0,y,0},CVector{0,-y,0},CVector{x,0,0},CVector{-x,0,0}}){
        auto p=m->pos+m->right*offset.x+m->forward*offset.y;
        if(ground>p.z)return true;
    }
    return false;
}
void Lift(CVehicle*car,float amount) {
    if(car&&IsVehicleValid(car))Cmd(0x07D5).I(GetVehicleRef(car)).F(0).F(0).F(amount).F(0).F(0).F(0).Run();
}
void LocalCargoPhysics(const StoryRemoteRole&r) {
    // The parent and its generation came from the host's ordinary shared-object
    // snapshot. Only the RC owner applies the stock upward impulse; applying
    // it to a frozen observer's vehicle would not reach the real simulation.
    Cargo c;auto*pool=ObjectPool();
    for(int i=0;pool&&i<pool->size;++i)if(pool->byteMap[i]>=0){
        auto*o=(CObject*)(pool->objects+(size_t)i*kObjectPoolElemSize);
        if(o->AttachedTo()!=r.car||!IsSharedMissionObject(o))continue;
        int group=o->ModelIndex()==kBarrelModel?0:o->ModelIndex()==kPlankModel?1:o->ModelIndex()==kBombModel?2:-1;
        if(group<0)continue;c.id=SharedMissionObjectId(o);c.object=o;c.handle=GetObjectRef(o);c.group=group;
        c.generation=Net().EntityGeneration(ENTITY_OBJECT,c.id);break;
    }
    if(!CargoAlive(c)||!Net().EntityLink(ENTITY_OBJECT,c.id,ENTITY_VEHICLE,r.vehicle)){localCargo={};return;}
    if(localCargo.id!=c.id||localCargo.generation!=c.generation||localCargo.carGeneration!=r.vehicleGeneration||localCargo.assignment!=r.assignment||localCargo.phase!=r.phase)
        localCargo={c.generation,r.vehicleGeneration,r.assignment,r.phase,c.id,true,false};
    if(c.group==2){
        if(!localCargo.bombChecked){localCargo.bombChecked=true;if(Cmd(0x09FC).I(c.handle).Run())Lift(r.car,.1f);}
    }else if(localCargo.clearance){
        if(BelowCargoFloor(c.object,r.car,c.group))Lift(r.car,.005f);else localCargo.clearance=false;
    }
}
void Toggle(uint8_t player,const StoryRemoteRole&r) {
    auto now=NowMs();if(now-pressedAt[player]<150)return;pressedAt[player]=now;
    if(held[player].cargo.id){Drop(player,true,true);return;}
    if(now-began<1000||now-releasedAt[player]<1000)return;
    auto point=PickupPoint(r.car);
    for(int local=77;local<=92;++local){
        int group=local<87?0:local<92?1:2;auto c=CargoAt(local,group);
        if(!CargoAlive(c)||c.object->AttachedTo()||Claimed(c.object)||SharedStoryObjectHolder(c.object)!=255)continue;
        auto d=c.object->GetPosition()-point;
        if(std::fabs(d.x)>1||std::fabs(d.y)>1||std::fabs(d.z)>1)continue;
        // The stock loop picks the first object in these groups and uses the
        // same attachment offsets; it does not create or award extra cargo.
        bool collision=(c.object->Flags()&1u)!=0;
        Cmd(0x0681).I(c.handle).I(GetVehicleRef(r.car)).F(0).F(0).F(group==0?-1.2f:-1.f).F(0).F(group==0?90.f:0.f).F(0).Run();
        if(c.object->AttachedTo()!=r.car)return;
        Cmd(0x050E).I(c.handle).I(GetVehicleRef(r.car)).Run();
        Cmd(0x097B).I(c.handle).I(1009).Run();
        held[player]={c,r.run,r.vehicleGeneration,r.phase,r.assignment,r.vehicle,GetVehicleRef(r.car),r.car};
        held[player].hadCollision=collision;
        LOGI("Cooperative RC cargo attached peer=%u object=%04X car=%04X run=%llu",player,c.id,r.vehicle,(unsigned long long)r.run);
        return;
    }
}
void Tick() {
    if(!Net().Online()||!core::IsPlayerReady()||core::IsRestarting())return;
    auto run=StoryRunId();
    if(localRun!=run){Clear(Net().IsHost());localRun=run;began=NowMs();}
    if(!run||!CooperativeRemoteMission("ZERO4"))return;
    if(Net().IsHost()){
        StoryRemoteRole native;if(!DefinitionReady(native)){
            for(uint8_t p=0;p<3;++p){if(held[p].cargo.id)Drop(p,true,false);ClearMagnet(p,true);}return;
        }
        for(uint8_t p=0;p<3;++p){
            if(p==Net().MyId())continue;StoryRemoteRole r;
            if(!RoleReady(p,r)){if(held[p].cargo.id)Drop(p,true,false);ClearMagnet(p,true);continue;}
            if(held[p].cargo.id&&(!SameRole(held[p],r)||!StillHeld(held[p])))Drop(p,true,false);
            auto&hold=held[p];
            if(hold.cargo.id&&hold.cargo.group!=2&&hold.clearance){
                bool below=BelowCargoFloor(hold.cargo.object,r.car,hold.cargo.group);
                Cmd(0x0382).I(hold.cargo.handle).I(below?0:(hold.hadCollision?1:0)).Run();hold.collisionChanged=below;
                if(!below)hold.clearance=false;
            }
            MakeMagnet(p,r);
        }
    }else{
        StoryRemoteRole r;int controlled=-1;
        if(!CooperativeRemoteRole(Net().MyId(),r)||!r.car||r.car->ModelIndex()!=501||LocalStoryRoleMode()!=3||!Cmd(0x0241).I(0).Run()){buttonDown=false;localCargo={};return;}
        Cmd(0x0484).I(0).Out(&controlled).Run();
        if(GetVehicle(controlled)!=r.car){buttonDown=false;localCargo={};return;}
        LocalCargoPhysics(r);
        bool down=Cmd(0x00E1).I(0).I(17).Run();
        if(down&&!buttonDown){
            Writer w(proto::G_STORY_COOP);w.U8(kMagnetInput);w.U64(r.run);w.U32(r.phase);w.U32(r.assignment);
            w.Entity(ENTITY_VEHICLE,r.vehicle);w.U64(r.vehicleGeneration);w.Entity(ENTITY_VEHICLE,r.source);w.U64(r.sourceGeneration);
            if(++localSequence==0)++localSequence;w.U32(localSequence);w.U32((uint32_t)NowMs());auto point=PickupPoint(r.car);w.Vec({point.x,point.y,point.z});
            Net().Send(proto::CH_RELIABLE,w);
        }
        buttonDown=down;
    }
}
class RCMagnet:public Module {
public:
    const char*Name()const override{return "StoryRCMagnet";}
    void OnPostProcess()override{Tick();}
    void OnPeerLeave(uint8_t p)override{if(Net().IsHost()&&core::IsPlayerReady()){Drop(p,true,false);ClearMagnet(p,true);}if(p<3)inputs[p]={};}
    void OnPeerResync(uint8_t p)override{OnPeerLeave(p);}
    void OnHostChanged(uint8_t)override{Clear(core::IsPlayerReady()&&!core::IsRestarting());}
    void OnGameStarted()override{Clear(false);}
    void OnGameShutdown()override{Clear(false);}
    void OnSessionEnd()override{Clear(core::IsPlayerReady()&&!core::IsRestarting());}
};
RCMagnet feature;struct Register {Register(){core::RegisterModule(&feature);}}registration;
} // namespace

void ObserveCooperativeRCMissionScript(void*script) {
    if(!Net().IsHost()||!script||!StoryRunId()||!IsSharedStoryScript(script)||
       !Field<uint8_t>(script,0xDC)||_strnicmp(Field<char[8]>(script,8),"ZERO4",8))return;
    auto*base=Field<uint8_t*>(script,0x10);auto run=StoryRunId();
    if(source.script!=script||source.base!=base||source.run!=run)source={script,base,run,false};
}
void ReceiveCooperativeRCMagnet(uint8_t from,uint64_t token,Reader&r) {
    auto phase=r.U32(),assignment=r.U32();auto vehicle=r.U16();auto carGeneration=r.U64();auto sourceId=r.U16();auto sourceGeneration=r.U64();
    auto sequence=r.U32();auto tick=r.U32();auto p=r.Vec();CVector point{p.x,p.y,p.z};StoryRemoteRole role,native;
    if(!r.Ok()||!Net().IsHost()||from>=3||from==Net().MyId()||!token||token!=StoryRunId()||phase!=CooperativeStoryPhase()||
       !DefinitionReady(native)||!RoleReady(from,role)||token!=role.run||assignment!=role.assignment||vehicle!=role.vehicle||
       sourceId!=role.source||carGeneration!=role.vehicleGeneration||sourceGeneration!=role.sourceGeneration||
       !std::isfinite(point.x)||!std::isfinite(point.y)||!std::isfinite(point.z)||(point-PickupPoint(role.car)).Length()>3)return;
    auto*v=Vehicles::Instance().Find(role.vehicle);
    if(!v||(int32_t)(v->state.time-tick)>1200||(int32_t)(tick-v->state.time)>1500||!inputs[from].Accept(sequence))return;
    Toggle(from,role);
}
bool CooperativeRCCondition(void*script,int opcode,const int32_t*p,bool&value) {
    if(!p||!IsSourceScript(script))return false;StoryRemoteRole native;if(!DefinitionReady(native))return false;
    auto*object=GetObject(p[0]);if(!object||!Claimed(object))return false;
    if(opcode==0x02CC){value=true;return true;}
    if(opcode!=0x04E6||ReadLocal(122)!=0)return false;
    float f[6];memcpy(f,p+1,sizeof f);auto center=PickupPoint(native.car);
    if(!std::isfinite(f[0])||!std::isfinite(f[1])||!std::isfinite(f[2])||
       (CVector{f[0],f[1],f[2]}-center).Length()>.05f||p[7]!=0)return false;
    for(int i=3;i<6;++i)if(!std::isfinite(f[i])||std::fabs(f[i]-1.f)>.001f)return false;
    value=false;return true;
}
} // namespace coop
