#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <set>
#include <windows.h>
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/story-stage/StoryStage.h"
#include "shared/api/mission-objects/SharedObject.h"
#include "shared/api/net-session/Session.h"
#include "shared/api/story-actions/StoryActions.h"
#include "shared/lib/sync/StoryCoopPolicy.h"
#include "shared/lib/sync/StoryContinuation.h"
#include "shared/lib/core/Core.h"
#include "shared/game/Anim.h"
#include "shared/game/Script.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "entities/profile/Profile.h"
#include "entities/session/Flow.h"
#include "entities/ped/Peds.h"
#include "entities/vehicle/Vehicles.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/ui/chat/Chat.h"
namespace coop {namespace {
using namespace game;using game::script::Cmd;
enum:uint8_t{SNAPSHOT=1,REWARD=2,RECEIPT=3,LEAVE_ROLE=4};
struct Role {uint32_t assignment=0;uint64_t objectGeneration=0,driverGeneration=0;uint8_t player=255,mode=0;uint16_t vehicle=0,source=0,driver=0;uint64_t generation=0,sourceGeneration=0;int handle=-1,driverHandle=-1;bool combat=true;uint16_t object=0;CVector offset;uint16_t angle=0;float range=0;int weapon=0;};
struct Enemy {uint64_t generation=0,changed=0,hit=0;uint8_t target=255,aggressor=255;int opcode=0;};
struct Credit {uint64_t token=0;int32_t money=0;std::string name;uint64_t sent=0;};
struct Trail {CVector position,velocity;Quat rotation;float distance=0;};
uint64_t run=0,runCounter=0,receivedRun=0,started=0,lastSnapshot=0,lastAI=0,localRoleRun=0;
uint32_t snapshotRevision=0,receivedRevision=0,roleCounter=0,localAssignment=0,declinedAssignment=0;uint64_t declinedSent=0;
struct Declined{uint16_t source=0,object=0;uint64_t generation=0;};std::array<Declined,3>declined;std::array<uint64_t,3>repairAt{};std::array<uint8_t,3>repairCount{};
std::string missionName;
bool active=false,passed=false,profileReady=false,mandatoryCamera=false;
struct AllySeat{uint16_t ped=0,vehicle=0;uint64_t pedGeneration=0,vehicleGeneration=0,until=0;int8_t seat=-1;bool inferred=false;};
std::map<uint16_t,AllySeat>allySeats;
std::set<uint16_t>releasedAllies;
std::array<Role,3> roles;
std::map<uint16_t,Enemy> enemies;
std::map<uint16_t,uint8_t> allies;
std::map<int,uint8_t> groups;
std::map<uint16_t,int> memberGroups;
std::map<uint8_t,int> adoptedGroups;
std::deque<Trail> trail;
uint16_t trailVehicle=0,plannedGunner=0;uint64_t trailGeneration=0,plannedGunnerGeneration=0,plannedGunnerUntil=0;
std::set<std::pair<uint16_t,uint64_t>> allocated;
std::deque<Credit> outbox,pendingCredits;
std::set<uint64_t> awaitingProfile;
StoryRewardLedger ledger;
uint64_t lastNoise[3]{};
uint8_t localMode=0;bool savedCollision=true;
int localRoleHandle=-1;int savedRoleState=0,savedRoleActiveWeapon=0;
CWeapon savedRoleWeapon{};int roleWeaponType=28,roleAmmoAdded=0;bool roleWeaponSaved=false;
std::array<int,3> roleBlips{-1,-1,-1};uint64_t gunnerAbsent=0;
void Snapshot();
void SaveOutbox(){
    if(!Net().IsHost())return;Writer w;w.U32(0x32524353);w.U8((uint8_t)outbox.size());for(auto&c:outbox){w.U64(c.token);w.I32(c.money);w.Str(c.name,24);}
    if(!WriteFileBytes(Cfg().coopDir+L"reward-outbox.bin",w.Data(),w.Size()))LOGW("Story reward outbox could not be persisted");
}
void LoadOutbox(){
    if(!Net().IsHost())return;std::vector<uint8_t>bytes;if(!ReadFileBytes(Cfg().coopDir+L"reward-outbox.bin",bytes)||bytes.size()>4096)return;Reader r(bytes.data(),bytes.size());if(r.U32()!=0x32524353)return;auto count=r.U8();if(count>64)return;std::deque<Credit>loaded;
    for(int i=0;i<count;++i){Credit c;c.token=r.U64();c.money=r.I32();c.name=r.Str(24);if(!c.token||c.money<=0||c.money>10000000||c.name.empty())return;loaded.push_back(c);}if(r.Ok())outbox=std::move(loaded);
}
CPed* Player(uint8_t id){if(id==Net().MyId())return core::IsPlayerReady()?FindPlayerPed(0):nullptr;auto*p=Players::Instance().Get(id);return p&&p->hasState&&NowMs()-p->stateRecvMs<1500?p->ped:nullptr;}
bool Participant(uint8_t id){auto*p=Player(id);return p&&IsPedValid(p)&&p->Health()>0;}
uint8_t PlayerId(CPed*p){if(p==FindPlayerPed(0))return Net().MyId();auto*rp=Players::Instance().FindByPed(p);return rp?rp->id:255;}
void ReleaseLocalRole(){
    if(localMode==3&&core::IsPlayerReady()&&Cmd(0x0241).I(0).Run())Cmd(0x04DB).Run();
    if(core::IsPlayerReady()&&localRoleHandle>=0){auto*me=FindPlayerPed(0);
        if(GetPedRef(me)==localRoleHandle){if(me->AttachedTo())Cmd(0x0465).I(localRoleHandle).Run();
            if(roleWeaponSaved){auto&weapon=me->Weapons()[4];
                if(weapon.type==roleWeaponType){if(savedRoleWeapon.type==roleWeaponType)weapon.ammoTotal=std::max(0,weapon.ammoTotal-roleAmmoAdded);
                    else{Cmd(0x0555).I(localRoleHandle).I(roleWeaponType).Run();if(savedRoleWeapon.type)Cmd(0x01B2).I(localRoleHandle).I(savedRoleWeapon.type).I(savedRoleWeapon.ammoTotal).Run();}}}
            if((localMode==2||localMode==5)&&me->InVehicle()&&me->Health()>0){if(localMode==2){auto*n=Vehicles::Instance().Find(roles[Net().MyId()].vehicle);if(n&&Vehicles::Alive(*n)&&me->Vehicle()==n->veh)Cmd(0x0687).I(localRoleHandle).Run();}if(!mandatoryCamera)Cmd(0x02EB).Run();if(!mandatoryCamera)Cmd(0x01B4).I(0).I(1).Run();localRoleHandle=-1;localRoleRun=0;localMode=0;localAssignment=0;roleWeaponSaved=false;return;}
            if(me->Health()>0){if(me->InVehicle()){auto p=me->GetPosition();Cmd(0x0362).I(localRoleHandle).F(p.x+2).F(p.y).F(p.z).Run();}me->PedState()=savedRoleState;me->MoveState()=1;Cmd(0x01B9).I(localRoleHandle).I(savedRoleActiveWeapon).Run();}
            if(localMode==4){auto&flags=Field<uint32_t>(me,0x1C);flags=(flags&~1u)|(savedCollision?1u:0u);}
            if(!mandatoryCamera)Cmd(0x02EB).Run();if(!mandatoryCamera)Cmd(0x01B4).I(0).I(1).Run();Cmd(0x0619).I(localRoleHandle).I(localMode==4?savedCollision:1).Run();}}
    localRoleHandle=-1;localRoleRun=0;localMode=0;localAssignment=0;roleWeaponSaved=false;
}
bool HumanIn(CVehicle*car,uint8_t excluding=255){auto human=[&](CPed*p){auto id=PlayerId(p);return id<3&&id!=excluding;};bool occupied=human(car->Driver());for(int i=0;i<car->MaxPassengers();++i)occupied|=human(car->Passengers()[i]);return occupied;}
void RetireRole(Role&r,bool deleteNative,uint8_t excluding=255){
    if(deleteNative&&Net().IsHost()&&core::IsPlayerReady()){
        auto*v=Vehicles::Instance().Find(r.vehicle);bool valid=v&&Vehicles::Alive(*v)&&Net().EntityGeneration(ENTITY_VEHICLE,r.vehicle)==r.generation;
        bool occupied=valid&&HumanIn(v->veh,excluding);
        auto*np=Peds::Instance().Find(GetPed(Peds::Instance().HandleOf(r.driver)));
        if(np&&np->netId==r.driver&&np->owner==Net().MyId()&&Net().EntityGeneration(ENTITY_PED,r.driver)==r.driverGeneration){if(occupied)Cmd(0x01C2).I(np->handle).Run();else Cmd(0x009B).I(np->handle).Run();}
        if(r.mode!=4&&valid){if(occupied){Vehicles::Instance().RememberUsed(v->veh);Cmd(0x01C3).I(v->handle).Run();}
            else{Vehicles::Instance().ShareScriptDeletion(v->veh);Cmd(0x00A6).I(v->handle).Run();}}
    }
    allocated.erase({r.source,r.sourceGeneration});r={};
}
void RetireRoles(bool deleteNative){
    ReleaseLocalRole();
    for(auto&b:roleBlips){if(b>=0&&core::IsPlayerReady())Cmd(0x0164).I(b).Run();b=-1;}
    for(auto&r:roles)RetireRole(r,deleteNative);
    if(deleteNative&&Net().IsHost())Snapshot();
    trail.clear();trailVehicle=0;trailGeneration=0;
}
void DeclineLocalRole(){
    if(Net().MyId()>=3)return;auto&r=roles[Net().MyId()];if(!r.assignment)return;
    declinedAssignment=r.assignment;ReleaseLocalRole();
    if(NowMs()-declinedSent>500){Writer w(proto::G_STORY_COOP);w.U8(LEAVE_ROLE);w.U64(run);w.U32(r.assignment);Net().Send(proto::CH_RELIABLE,w);declinedSent=NowMs();}
}
bool DeclinedSource(uint8_t id,uint16_t source,uint64_t generation,uint16_t object=0){auto&d=declined[id];return (d.source||d.object)&&d.source==source&&d.generation==generation&&d.object==object;}
void Snapshot(){
    if(!Net().IsHost()||!run)return;
    Writer w(proto::G_STORY_COOP);w.U8(SNAPSHOT);w.U64(run);w.U32(++snapshotRevision);w.Bool(active);w.Str(missionName,16);w.Bool(mandatoryCamera);
    for(auto&r:roles){w.U8(r.mode);w.U32(r.assignment);w.Entity(ENTITY_VEHICLE,r.vehicle);w.Entity(ENTITY_VEHICLE,r.source);w.Entity(ENTITY_PED,r.driver);w.Bool(r.combat);w.Entity(ENTITY_OBJECT,r.object);w.Vec({r.offset.x,r.offset.y,r.offset.z});w.U16(r.angle);w.F32(r.range);w.U8((uint8_t)r.weapon);}
    w.U8((uint8_t)allySeats.size());for(auto&[id,a]:allySeats){w.Entity(ENTITY_PED,a.ped);w.Entity(ENTITY_VEHICLE,a.vehicle);w.I8(a.seat);}
    Net().Send(proto::CH_RELIABLE,w);lastSnapshot=NowMs();
}
void SendCredit(Credit&c,uint8_t target){Writer w(proto::G_STORY_COOP);w.U8(REWARD);w.U64(c.token);w.U8(target);w.I32(c.money);Net().Send(proto::CH_RELIABLE,w);c.sent=NowMs();}
void Acknowledge(uint64_t token){Writer w(proto::G_STORY_COOP);w.U8(RECEIPT);w.U64(token);Net().Send(proto::CH_RELIABLE,w);}
void ApplyCredits(){
    if(!profileReady||!flow::InHostWorld()||Net().IsHost())return;
    bool changed=false;
    for(auto&c:pendingCredits){
        if(ledger.Credit(c.token)){auto&money=PlayerInfos()[0].money;money=(int32_t)std::min<int64_t>(999999999,(int64_t)money+c.money);
            ui::TheChat().AddSystem(Format(TR("Награда за миссию: $%d.","Mission reward: $%d."),c.money));changed=true;
            LOGI("Story reward credited token=%llu amount=%d",(unsigned long long)c.token,c.money);}
        awaitingProfile.insert(c.token);
    }
    pendingCredits.clear();
    // Receipt is sent only after the server confirms the profile containing
    // both the new balance and token. A disconnect before that resends safely.
    if(changed||!awaitingProfile.empty())SaveProfileNow(true);
}
bool Visible(CPed*from,CPed*to){auto a=from->GetPosition()+CVector{0,0,.7f},b=to->GetPosition()+CVector{0,0,.7f};return Cmd(0x06BD).F(a.x).F(a.y).F(a.z).F(b.x).F(b.y).F(b.z).I(1).I(0).I(0).I(1).I(0).Run();}
void UpdateEnemies(){
    auto now=NowMs();if(!Net().IsHost()||!active||now-lastAI<500)return;lastAI=now;
    int budget=12;
    for(auto it=enemies.begin();it!=enemies.end();){auto*n=Peds::Instance().Find(GetPed(Peds::Instance().HandleOf(it->first)));
        if(!n||!n->ped||n->ped->Health()<=0||it->second.generation!=Net().EntityGeneration(ENTITY_PED,it->first)){it=enemies.erase(it);continue;}
        auto&e=it->second;++it;if(n->owner!=Net().MyId()||budget--<=0||n->ped->InVehicle()||now-e.changed<1500)continue;
        // Only a still-active native kill/aim/shoot task can be retargeted.
        // A later script animation, flee, surrender or cutscene wins immediately.
        bool attack=anim::FindTask(n->ped,1000)||anim::FindTask(n->ped,1022)||anim::FindTask(n->ped,1020);
        if(!attack)continue;
        std::array<StoryTargetCandidate,3> choices{};
        for(uint8_t id=0;id<3;++id){auto*p=Player(id);auto&c=choices[id];c.id=id;c.alive=p&&p->Health()>0;
            if(!c.alive)continue;c.sameArea=AreaOf(p)==AreaOf(n->ped)||AreaOf(n->ped)==13;c.distance=(p->GetPosition()-n->ped->GetPosition()).Length();
            c.visible=c.sameArea&&c.distance<120&&Visible(n->ped,p);}
        auto selected=ChooseStoryTarget(choices,e.target,now-e.hit<8000?e.aggressor:255);
        if(selected<3&&selected!=e.target){auto*target=Player(selected);if(!target)continue;
            if(e.opcode==0x0635)Cmd(0x0635).I(n->handle).I(GetPedRef(target)).I(2000).Run();
            else if(e.opcode==0x074D)Cmd(0x074D).I(n->handle).I(GetPedRef(target)).I(2000).Run();
            else Cmd(0x05E2).I(n->handle).I(GetPedRef(target)).Run();
            e.target=selected;e.changed=now;LOGI("Story enemy %04X chose participant %u",n->netId,selected);}
    }
}
bool SpawnPoint(CVehicle*source,uint8_t player,CVector&point){
    if(!source->Matrix())return false;const bool air=source->VehicleSubClass()==VEHICLE_TYPE_PLANE||source->VehicleSubClass()==VEHICLE_TYPE_HELI;
    for(int attempt=0;attempt<8;++attempt){auto*m=source->Matrix();float side=((player+attempt)&1)?1.f:-1.f;
        auto p=m->pos+m->right*(side*(5.f+(attempt/2)*3.f))-m->forward*(4.f+player*3.f);
        if(air){p=m->pos+m->right*(side*(18.f+(attempt/2)*12.f))-m->forward*(20.f+player*12.f);if(source->GetPosition().z>30.f)p.z+=3.f;}else{bool ground=false;p.z=Call<0x5696C0,float>(p.x,p.y,p.z+8.f,&ground,(CEntity**)nullptr)+.7f;if(!ground)continue;}
        bool blocked=false;auto*pool=VehiclePool();for(int i=0;pool&&i<pool->size;++i)if(pool->byteMap[i]>=0){auto*v=(CVehicle*)(pool->objects+(size_t)i*kVehiclePoolElemSize);if((v->GetPosition()-p).Length()<(air?15.f:4.f)){blocked=true;break;}}
        if(!blocked){point=p;return true;}}
    return false;
}
bool CreateRole(CVehicle*source,uint8_t player,uint8_t mode){
    const bool gunner=mode==2;
    auto*src=Vehicles::Instance().EnsureNetworked(source);if(!src)return false;CVector p;if(!SpawnPoint(source,player,p))return false;
    auto previousComponents=Ref<uint16_t>(0x8A6458);for(int i=0;i<2;++i)Ref<int8_t>(0x8A6458+i)=Field<int8_t>(source,0x438+i);
    int h=-1;Cmd(0x00A5).I(source->ModelIndex()).F(p.x).F(p.y).F(p.z).Out(&h).Run();Ref<uint16_t>(0x8A6458)=previousComponents;auto*car=GetVehicle(h);if(!car)return false;
    Cmd(0x0175).I(h).F(source->GetHeading()*57.29578f).Run();
    memcpy((uint8_t*)car+0x434,(uint8_t*)source+0x434,4);car->DoorLock()=0;car->Health()=1000;car->SetEngineOn(true);
    car->SetStreamingDontDelete(true);auto*n=Vehicles::Instance().EnsureNetworked(car);if(!n){Cmd(0x00A6).I(h).Run();return false;}
    Vehicles::Instance().CloneAppearance(source,car);
    if(mode==3||mode==5){WorldRemove(car);if(source->Matrix()&&car->Matrix()){*car->Matrix()=*source->Matrix();car->Matrix()->pos=p;}else car->SetPositionRaw(p);CallMethod<0x446F90>(car);CallMethod<0x532B00>(car);WorldAdd(car);car->MoveSpeed()=source->MoveSpeed();car->TurnSpeed()=source->TurnSpeed();if(car->VehicleSubClass()==VEHICLE_TYPE_HELI)Field<float>(car,0x84C)=Field<float>(source,0x84C);}
    Role role;role.player=player;role.mode=mode;role.vehicle=n->netId;role.source=src->netId;role.handle=h;
    role.generation=Net().EntityGeneration(ENTITY_VEHICLE,n->netId);role.sourceGeneration=Net().EntityGeneration(ENTITY_VEHICLE,src->netId);
    if(gunner){
        if(!IsModelLoaded(7)){RequestModel(7,0);Cmd(0x00A6).I(h).Run();return false;}
        int driver=-1;Cmd(0x0129).I(h).I(4).I(7).Out(&driver).Run();auto*ped=GetPed(driver);
        if(!ped){Cmd(0x00A6).I(h).Run();return false;}ped->Health()=1000;ped->SetStreamingDontDelete(true);
        auto*np=Peds::Instance().EnsureNetworked(ped);if(!np){Cmd(0x009B).I(driver).Run();Cmd(0x00A6).I(h).Run();return false;}
        role.driver=np->netId;role.driverHandle=driver;role.driverGeneration=Net().EntityGeneration(ENTITY_PED,np->netId);
    }
    role.assignment=++roleCounter;if(!role.assignment)role.assignment=++roleCounter;roles[player]=role;LOGI("Story role created run=%llu player=%u mode=%u vehicle=%04X source=%04X",(unsigned long long)run,player,role.mode,role.vehicle,role.source);Snapshot();if(mode==3&&!Vehicles::Instance().AssignStoryRemote(car,player)){roles[player]={};Vehicles::Instance().ShareScriptDeletion(car);Cmd(0x00A6).I(h).Run();Snapshot();return false;}return true;
}
void MoveRoleBike(Role&r,CVehicle*source){
    auto*n=Vehicles::Instance().Find(r.vehicle);if(!n||!Vehicles::Alive(*n)||n->veh->Health()<=0)return;
    auto*driver=GetPed(r.driverHandle);if(!driver||driver->Health()<=0)return;
    auto target=trail.back();float remaining=r.player==1?7.f:14.f;
    for(size_t i=trail.size();i>1&&remaining>0;--i){const auto&a=trail[i-1],&b=trail[i-2];float segment=(a.position-b.position).Length();
        if(segment>=remaining&&segment>.001f){float t=remaining/segment;target.position=a.position+(b.position-a.position)*t;target.rotation=a.rotation;remaining=0;break;}
        remaining-=segment;target=b;}
    CMatrix basis{};target.rotation.ToMatrix(basis);if(remaining>0)target.position=target.position-basis.forward*remaining;
    auto*car=n->veh;WorldRemove(car);if(auto*m=car->Matrix()){auto position=target.position;target.rotation.ToMatrix(*m);m->pos=position;}else car->SetPositionRaw(target.position);
    CallMethod<0x446F90>(car);CallMethod<0x532B00>(car);WorldAdd(car);car->MoveSpeed()=source->MoveSpeed();car->TurnSpeed()=source->TurnSpeed();
    car->GasPedal()=source->GasPedal();car->SetEngineOn(source->EngineOn());n->lastChangeMs=NowMs();
}
void NativeAllySeats(){
    static uint64_t scanned=0;if(!active||!Net().IsHost()||NowMs()-scanned<150)return;scanned=NowMs();
    for(auto&[id,n]:Peds::Instance().All()){
        auto*p=n.ped;if(n.owner!=Net().MyId()||!p||!IsPedValid(p)||p->CreatedBy()!=PED_MISSION||p->Health()<=0)continue;
        auto*task=anim::FindTask(p,700);if(!task)continue;
        auto*car=Field<CVehicle*>(task,0xC);if(!car||!IsVehicleValid(car)||CooperativeAllyLeader(p)>=3)continue;
        auto*nv=Vehicles::Instance().EnsureNetworked(car);if(!nv)continue;
        auto found=allySeats.find(id);if(found!=allySeats.end()&&!found->second.inferred)continue;
        int door=Field<int32_t>(task,0x14);if(!door)door=Field<int32_t>(task,0x1C);int seat=door?SeatOfTargetDoor(door):-1;
        if(seat<0){uint8_t used=0;for(auto&[other,a]:allySeats)if(other!=id&&a.vehicle==nv->netId&&NowMs()<a.until)used|=1u<<a.seat;
            for(int i=0;i<car->MaxPassengers();++i)if(!car->Passengers()[i]&&!(used&(1u<<i))){seat=i;break;}}
        if(seat<0||seat>=car->MaxPassengers()||seat>=8||allySeats.size()>=32)continue;
        bool changed=found==allySeats.end()||found->second.vehicle!=nv->netId||found->second.seat!=seat;
        allySeats[id]={id,nv->netId,Net().EntityGeneration(ENTITY_PED,id),Net().EntityGeneration(ENTITY_VEHICLE,nv->netId),NowMs()+1500,(int8_t)seat,true};
        if(changed){Snapshot();LOGI("Native ally seat reserved ped=%04X vehicle=%04X seat=%d",id,nv->netId,seat);}
    }
}
void HostRoles(bool preparing=false){
    if(!active||!Net().IsHost()||(!preparing&&NowMs()-started<1500)||Ref<uint8_t>(0xB5F851))return;
    auto*me=FindPlayerPed(0);
    for(uint8_t id=0;id<3;++id)if(roles[id].mode&&roles[id].mode!=4){auto&r=roles[id];auto*n=Vehicles::Instance().Find(r.vehicle);
        bool broken=!n||!Vehicles::Alive(*n)||n->veh->Health()<=0||Net().EntityGeneration(ENTITY_VEHICLE,r.vehicle)!=r.generation;
        if(r.mode==2){auto*d=GetPed(Peds::Instance().HandleOf(r.driver));broken|=!d||d->Health()<=0;}
        if(broken&&NowMs()>=repairAt[id]){auto old=r;RetireRole(r,true);repairAt[id]=NowMs()+3000;
            if(++repairCount[id]>3)declined[id]={old.source,old.object,old.sourceGeneration};Snapshot();LOGI("Story role recovery player=%u attempt=%u",id,repairCount[id]);}}
    if(me->AttachedTo()&&me->AttachedTo()->Matrix()&&!me->InVehicle()){
        auto*parent=me->AttachedTo();uint16_t vehicle=0,object=0;
        if(parent->IsVehicle()){if(auto*n=Vehicles::Instance().EnsureNetworked((CVehicle*)parent))vehicle=n->netId;}
        else if(parent->IsObject()){ShareMissionObject(GetObjectRef((CObject*)parent));object=SharedMissionObjectId((CObject*)parent);}
        if(vehicle||object){bool change=false;for(auto&r:roles)change|=r.mode&&(r.mode!=4||r.vehicle!=vehicle||r.object!=object);if(change){RetireRoles(true);allocated.clear();}
            for(uint8_t id=0;id<3;++id)if(id!=Net().MyId()&&Participant(id)&&!roles[id].mode&&!DeclinedSource(id,vehicle,vehicle?Net().EntityGeneration(ENTITY_VEHICLE,vehicle):Net().EntityGeneration(ENTITY_OBJECT,object),object)){Role r;r.player=id;r.mode=4;r.vehicle=vehicle;r.object=object;r.source=vehicle;r.generation=vehicle?Net().EntityGeneration(ENTITY_VEHICLE,vehicle):0;r.offset=Field<CVector>(me,0x76C);r.offset.x+=(id==1?1.5f:-1.5f);r.angle=Field<uint16_t>(me,0x778);r.range=Field<float>(me,0x77C);r.weapon=me->ActiveWeapon().type;r.objectGeneration=object?Net().EntityGeneration(ENTITY_OBJECT,object):0;r.sourceGeneration=vehicle?r.generation:r.objectGeneration;r.assignment=++roleCounter;roles[id]=r;Snapshot();}
            return;}
    }
    int remote=-1;bool rc=Cmd(0x0241).I(0).Run();if(rc)Cmd(0x0484).I(0).Out(&remote).Run();
    auto*car=rc?GetVehicle(remote):me->VehicleIfIn();if(!car&&me->AttachedTo()&&me->AttachedTo()->IsVehicle())car=(CVehicle*)me->AttachedTo();
    if(!car||!IsVehicleValid(car)||me->AreaCode()!=0||car->Health()<=0){
        bool gunner=false;for(auto&r:roles)gunner|=r.mode>=2;if(gunner){if(!gunnerAbsent)gunnerAbsent=NowMs();if(NowMs()-gunnerAbsent>1500){RetireRoles(true);Snapshot();}}return;}
    auto*src=Vehicles::Instance().EnsureNetworked(car);if(!src)return;
    if(rc){bool changed=false;for(auto&r:roles)changed|=r.mode&&(r.mode!=3||r.source!=src->netId);if(changed){RetireRoles(true);allocated.clear();}
        gunnerAbsent=0;auto key=std::make_pair(src->netId,Net().EntityGeneration(ENTITY_VEHICLE,src->netId));
        for(uint8_t id=0;id<3;++id)if(id!=Net().MyId()&&Participant(id)&&!roles[id].mode&&NowMs()>=repairAt[id]&&!DeclinedSource(id,src->netId,key.second)&&!allocated.count(key))CreateRole(car,id,3);
        bool all=true;for(auto&[id,p]:Net().Peers())if(id!=Net().MyId()&&!roles[id].mode)all=false;if(all)allocated.insert(key);return;}
    bool oldSpecial=false;for(auto&r:roles)oldSpecial|=r.mode==3||r.mode==4;if(oldSpecial){RetireRoles(true);allocated.clear();Snapshot();}
    bool nativeDriver=car->Driver()&&car->Driver()!=me&&PlayerId(car->Driver())>=3;
    bool driveBy=anim::FindTask(me,1022)!=nullptr||(plannedGunner==src->netId&&plannedGunnerGeneration==Net().EntityGeneration(ENTITY_VEHICLE,src->netId)&&NowMs()<plannedGunnerUntil);
    bool recorded=Cmd(0x060E).I(GetVehicleRef(car)).Run();
    bool air=car->VehicleSubClass()==VEHICLE_TYPE_PLANE||car->VehicleSubClass()==VEHICLE_TYPE_HELI;
    bool supported=car->IsBike()||car->VehicleSubClass()==VEHICLE_TYPE_AUTOMOBILE;
    bool configured=(!_stricmp(missionName.c_str(),"DRUGS1"))&&car->ModelIndex()==581;
    bool continuing=false;for(auto&r:roles)continuing|=r.mode==2&&r.source==src->netId;
    bool shooting=GetPad(0)->DisablePlayerControls==0&&!mandatoryCamera;
    bool gunner=supported&&nativeDriver&&StoryGunnerSection(active,driveBy||(continuing&&!shooting),recorded||me->AttachedTo()==car||configured,shooting||continuing||preparing,false);
    bool changedCombat=false;for(auto&r:roles)if(r.mode==2&&r.combat!=shooting){r.combat=shooting;changedCombat=true;}if(changedCombat)Snapshot();
    int available=car->MaxPassengers();for(int i=0;i<car->MaxPassengers();++i)if(car->Passengers()[i]&&PlayerId(car->Passengers()[i])>=3)--available;
    int needed=0;for(auto&[id,p]:Net().Peers())if(id!=Net().MyId())++needed;
    if(nativeDriver)++needed; // the host also occupies one passenger seat
    bool overflow=(src->script||Vehicles::ScriptControlled(car))&&available<needed&&(car->Driver()==me||nativeDriver)&&(supported||air);
    if((gunner||overflow)){bool different=false;for(auto&r:roles)different|=r.mode&&r.source!=src->netId;if(different){RetireRoles(true);allocated.clear();Snapshot();}}
    if(!gunner&&!overflow){bool hadGunner=false;for(auto&r:roles)hadGunner|=r.mode==2;
        if(hadGunner){if(!gunnerAbsent)gunnerAbsent=NowMs();if(NowMs()-gunnerAbsent>1500){RetireRoles(true);Snapshot();}}return;}
    if(gunner){bool changed=false;for(auto&r:roles)changed|=r.mode&&(r.source!=src->netId||r.mode!=2);if(changed){RetireRoles(true);allocated.clear();}}
    if(gunner){gunnerAbsent=0;
        RequestModel(7,0);auto generation=Net().EntityGeneration(ENTITY_VEHICLE,src->netId);
        if(trailVehicle!=src->netId||trailGeneration!=generation){trail.clear();trailVehicle=src->netId;trailGeneration=generation;}
        Trail t;t.position=car->GetPosition();t.rotation=car->Matrix()?Quat::FromMatrix(*car->Matrix()):Quat{};t.velocity=car->MoveSpeed();
        if(trail.empty()||(trail.back().position-t.position).Length()>.08f)trail.push_back(t);if(trail.empty())trail.push_back(t);while(trail.size()>512)trail.pop_front();
    }
    auto key=std::make_pair(src->netId,Net().EntityGeneration(ENTITY_VEHICLE,src->netId));
    for(uint8_t id=0;id<3;++id)if(id!=Net().MyId()&&Participant(id)){
        if(!roles[id].mode&&NowMs()>=repairAt[id]&&!DeclinedSource(id,src->netId,key.second)&&!allocated.count(key)){if(CreateRole(car,id,gunner?2:air?5:1)&&preparing&&gunner&&!shooting){roles[id].combat=false;Snapshot();}}
        if(gunner&&roles[id].mode==2&&roles[id].source==src->netId&&!trail.empty())MoveRoleBike(roles[id],car);
    }
    bool all=true;for(auto&[id,p]:Net().Peers())if(id!=Net().MyId()&&!roles[id].mode)all=false;if(all)allocated.insert(key);
}
void LocalRole(){
    if(Net().IsHost()||!active||!flow::InHostWorld()||Ref<uint8_t>(0xB5F851))return;
    auto&r=roles[Net().MyId()];
    if(declinedAssignment&&declinedAssignment==r.assignment){DeclineLocalRole();return;}
    if(localRoleRun==run&&localAssignment==r.assignment&&localMode){
        auto*me=FindPlayerPed(0);auto*n=Vehicles::Instance().Find(r.vehicle);
        bool left=(localMode==2||localMode==5)?(!me->InVehicle()||!n||me->Vehicle()!=n->veh):localMode==3?!Cmd(0x0241).I(0).Run():localMode==4?!me->AttachedTo():false;
        if(left&&me->Health()>0){DeclineLocalRole();return;}}
    if(r.mode==3||r.mode==4||r.mode==5){auto*me=FindPlayerPed(0);auto*n=Vehicles::Instance().Find(r.vehicle);CEntity*parent=r.object?(CEntity*)SharedMissionObjectById(r.object):n&&Vehicles::Alive(*n)?(CEntity*)n->veh:nullptr;
        if(me->Health()<=0||!parent)return;
        if(localRoleRun==run&&localMode==r.mode){if(r.mode==3&&Cmd(0x0241).I(0).Run())return;if(r.mode==4&&me->AttachedTo()==parent)return;if(r.mode==5)return;}
        if(r.mode==3&&(!n||n->owner!=Net().MyId()))return;
        if(localRoleRun)ReleaseLocalRole();savedRoleState=me->PedState();savedRoleActiveWeapon=me->ActiveWeapon().type;savedCollision=(Field<uint32_t>(me,0x1C)&1u)!=0;localRoleHandle=GetPedRef(me);localMode=r.mode;
        if(me->InVehicle()){auto p=me->GetPosition();Cmd(0x0362).I(localRoleHandle).F(p.x+2).F(p.y).F(p.z).Run();}
        if(r.mode==3)Cmd(0x0715).I(0).I(n->handle).Run();
        else if(r.mode==5)Cmd(0x036A).I(localRoleHandle).I(n->handle).Run();
        else CallMethod<0x5E7CB0,CEntity*>(me,parent,r.offset,r.angle,r.range,r.weapon);
        Cmd(0x01B4).I(0).I(mandatoryCamera?0:1).Run();localRoleRun=run;localAssignment=r.assignment;LOGI("Story native role ready mode=%u vehicle=%04X object=%04X",r.mode,r.vehicle,r.object);return;}
    if(r.mode!=2){if(localRoleRun)ReleaseLocalRole();
        if(r.mode==1&&roleBlips[Net().MyId()]<0)if(auto*n=Vehicles::Instance().Find(r.vehicle);n&&Vehicles::Alive(*n)){
            Cmd(0x0186).I(n->handle).Out(&roleBlips[Net().MyId()]).Run();ui::TheChat().AddSystem(TR("Транспорт для сопровождения отмечен на радаре.","Your escort vehicle is marked on the radar."));}
        return;}
    if(!r.combat&&localRoleRun){Cmd(0x01B4).I(0).I(0).Run();return;}
    auto*n=Vehicles::Instance().Find(r.vehicle);auto*me=FindPlayerPed(0);
    if(!n||!Vehicles::Alive(*n)||n->veh->Health()<=0||me->Health()<=0)return;
    if(localRoleRun==run&&me->InVehicle()&&me->Vehicle()==n->veh&&PassengerSeatOf(n->veh,me)==0)return;
    if(n->veh->Passengers()[0]&&n->veh->Passengers()[0]!=me)return;
    if(localRoleRun)ReleaseLocalRole();
    int h=GetPedRef(me);if(me->InVehicle())Cmd(0x0362).I(h).F(n->veh->GetPosition().x+2).F(n->veh->GetPosition().y).F(n->veh->GetPosition().z+1).Run();
    localRoleHandle=h;savedRoleState=me->PedState();savedRoleActiveWeapon=me->ActiveWeapon().type;savedRoleWeapon=me->Weapons()[4];roleWeaponType=savedRoleWeapon.type?savedRoleWeapon.type:28;roleWeaponSaved=true;
    Cmd(0x02EB).Run();
    // This is a real passenger, not an on-foot turret attached above the bike.
    // Native seating supplies the same pose, parent and seat to every observer.
    roleAmmoAdded=std::max(0,3000-(savedRoleWeapon.type==roleWeaponType?savedRoleWeapon.ammoTotal:0));
    if(roleAmmoAdded){int model=-1;Cmd(0x0781).I(roleWeaponType).Out(&model).Run();if(model>0)LoadModelNow(model);Cmd(0x01B2).I(h).I(roleWeaponType).I(roleAmmoAdded).Run();}
    Cmd(0x01B9).I(h).I(roleWeaponType).Run();
    Cmd(0x0430).I(h).I(n->handle).I(0).Run();
    if(!me->InVehicle()||me->Vehicle()!=n->veh){ReleaseLocalRole();return;}
    Cmd(0x01B9).I(h).I(roleWeaponType).Run();
    Cmd(0x0713).I(h).I(-1).I(-1).F(0).F(0).F(0).F(300).I(8).I(1).I(100).Run();
    Cmd(0x01B4).I(0).I(r.combat&&!mandatoryCamera?1:0).Run();localRoleRun=run;localAssignment=r.assignment;localRoleHandle=h;localMode=2;
    LOGI("Story gunner ready run=%llu vehicle=%04X controls=%u",(unsigned long long)run,r.vehicle,GetPad(0)->DisablePlayerControls);
}
class CooperativeStory:public Module{
 const char*Name()const override{return "CooperativeStory";}
 void OnInstallHooks()override{Net().SetHandler(proto::G_STORY_COOP,[](uint8_t from,Reader&r){
    auto op=r.U8();auto token=r.U64();
    if(op==LEAVE_ROLE){auto assignment=r.U32();if(!r.Ok()||!Net().IsHost()||from>=3||!active||token!=run||roles[from].assignment!=assignment||!Net().Peer(from))return;
        auto old=roles[from];declined[from]={old.source,old.object,old.sourceGeneration};RetireRole(roles[from],true);Snapshot();LOGI("Story role voluntarily released player=%u assignment=%u",from,assignment);return;}
    if(op==RECEIPT){if(Net().IsHost()&&r.Ok()){auto before=outbox.size();for(auto i=outbox.begin();i!=outbox.end();)i=i->token==token&&i->name==Net().PeerName(from)?outbox.erase(i):std::next(i);if(before!=outbox.size())SaveOutbox();}return;}
    if(from!=Net().HostId()||Net().IsHost()||!token)return;
    if(op==REWARD){auto target=r.U8();auto money=r.I32();if(!r.Ok()||target!=Net().MyId()||money<=0||money>10000000)return;
        if(pendingCredits.size()<64&&!std::any_of(pendingCredits.begin(),pendingCredits.end(),[&](auto&c){return c.token==token;}))pendingCredits.push_back({token,money});return;}
    if(op!=SNAPSHOT)return;auto revision=r.U32();bool nextActive=r.Bool();auto name=r.Str(16);bool nextCamera=r.Bool();std::array<Role,3> incoming;
    for(uint8_t id=0;id<3;++id){auto&v=incoming[id];v.player=id;v.mode=r.U8();v.assignment=r.U32();v.vehicle=r.U16();v.source=r.U16();v.driver=r.U16();v.combat=r.Bool();v.generation=Net().ExpectedEntityGeneration(ENTITY_VEHICLE,v.vehicle);v.sourceGeneration=Net().ExpectedEntityGeneration(ENTITY_VEHICLE,v.source);v.object=r.U16();v.objectGeneration=Net().ExpectedEntityGeneration(ENTITY_OBJECT,v.object);v.driverGeneration=Net().ExpectedEntityGeneration(ENTITY_PED,v.driver);auto offset=r.Vec();v.offset={offset.x,offset.y,offset.z};v.angle=r.U16();v.range=r.F32();v.weapon=r.U8();if(v.mode>5||v.weapon>46||!std::isfinite(v.range)||v.offset.Length()>100)return;}
    std::map<uint16_t,AllySeat>incomingSeats;auto seats=r.U8();if(seats>32)return;for(int i=0;i<seats;++i){AllySeat a;a.ped=r.U16();a.vehicle=r.U16();a.seat=r.I8();a.pedGeneration=Net().ExpectedEntityGeneration(ENTITY_PED,a.ped);a.vehicleGeneration=Net().ExpectedEntityGeneration(ENTITY_VEHICLE,a.vehicle);a.until=NowMs()+6000;if(a.seat<0||a.seat>7)return;incomingSeats[a.ped]=a;}
    if(!r.Ok()||(receivedRun==token&&receivedRevision&&(int32_t)(revision-receivedRevision)<=0))return;
    if(receivedRun&&token<receivedRun)return;
    if(Net().MyId()<3){auto&a=roles[Net().MyId()];auto&b=incoming[Net().MyId()];
        if(a.assignment!=b.assignment||!SameStoryRole(a.mode,a.vehicle,a.generation,a.object,b.mode,b.vehicle,b.generation,b.object)||a.objectGeneration!=b.objectGeneration){ReleaseLocalRole();auto&blip=roleBlips[Net().MyId()];if(blip>=0&&core::IsPlayerReady())Cmd(0x0164).I(blip).Run();blip=-1;}}
    if(run!=token)declinedAssignment=0;
    if(run!=token||!nextActive){ReleaseLocalRole();for(auto&b:roleBlips){if(b>=0&&core::IsPlayerReady())Cmd(0x0164).I(b).Run();b=-1;}if(active)ReleaseStoryCargo();}
    if(nextActive&&!nextCamera&&Net().MyId()<3&&incoming[Net().MyId()].mode>0&&incoming[Net().MyId()].combat){
        ReleaseStoryScriptCamera();if(roles[Net().MyId()].mode==2&&!roles[Net().MyId()].combat&&localRoleRun)Cmd(0x01B4).I(0).I(1).Run();}
    receivedRun=run=token;receivedRevision=revision;active=nextActive;missionName=name;roles=incoming;mandatoryCamera=nextCamera;allySeats=std::move(incomingSeats);
    LOGD("Story role snapshot run=%llu revision=%u active=%d",(unsigned long long)run,revision,active);
 });}
 void OnSessionStart()override{LoadOutbox();}
 void OnGameStarted()override{if(Net().IsHost()&&core::InPrologue()){outbox.clear();SaveOutbox();}}
 void OnPostProcess()override{if(!Net().Online()||!core::IsPlayerReady())return;
    ApplyCredits();if(Net().IsHost())for(auto it=allySeats.begin();it!=allySeats.end();){auto&a=it->second;auto*p=GetPed(Peds::Instance().HandleOf(a.ped));auto*v=Vehicles::Instance().Find(a.vehicle);
        bool valid=p&&p->Health()>0&&v&&Vehicles::Alive(*v)&&a.pedGeneration==Net().EntityGeneration(ENTITY_PED,a.ped)&&a.vehicleGeneration==Net().EntityGeneration(ENTITY_VEHICLE,a.vehicle);
        if(valid&&p->InVehicle()&&p->Vehicle()==v->veh)a.until=NowMs()+45000;it=(!valid||NowMs()>a.until)?allySeats.erase(it):std::next(it);}
    if(Net().IsHost()){NativeAllySeats();HostRoles();UpdateEnemies();if(run&&NowMs()-lastSnapshot>1000)Snapshot();
        for(auto&c:outbox)if(NowMs()-c.sent>2000)for(auto&[id,p]:Net().Peers())if(p.name==c.name)SendCredit(c,id);
    }else LocalRole();}
 void OnPeerResync(uint8_t id)override{ResyncCooperativeStory(id);}
 void OnPeerLeave(uint8_t id)override{if(Net().IsHost()&&core::IsPlayerReady()&&id<3){RetireRole(roles[id],true,id);declined[id]={};repairAt[id]=0;repairCount[id]=0;Snapshot();}
    if(Net().IsHost()&&active)for(auto&[ped,leader]:allies)if(leader==id){leader=Net().MyId();if(auto*p=GetPed(Peds::Instance().HandleOf(ped)))RestoreCooperativeAlly(p,leader);}
    if(auto i=adoptedGroups.find(id);i!=adoptedGroups.end()){if(core::IsPlayerReady())Cmd(0x0632).I(i->second).Run();adoptedGroups.erase(i);}}
 void OnGameShutdown()override{RetireRoles(false);declined={};repairAt={};repairCount={};localAssignment=declinedAssignment=0;active=false;plannedGunner=0;plannedGunnerUntil=0;mandatoryCamera=false;allySeats.clear();releasedAllies.clear();run=receivedRun=0;receivedRevision=0;enemies.clear();allies.clear();groups.clear();memberGroups.clear();adoptedGroups.clear();allocated.clear();profileReady=false;}
 void OnSessionEnd()override{OnGameShutdown();outbox.clear();pendingCredits.clear();awaitingProfile.clear();ledger.paid.clear();}
} feature;
struct Register{Register(){core::RegisterModule(&feature);}} registration;
}
void PrepareCooperativeStoryStage(){if(Net().IsHost()&&active)HostRoles(true);}
uint64_t StoryRunId(){return active?run:0;}
void BeginCooperativeStory(const std::string&name){if(!Net().IsHost())return;RetireRoles(true);declined={};repairAt={};repairCount={};enemies.clear();allies.clear();groups.clear();memberGroups.clear();allySeats.clear();releasedAllies.clear();mandatoryCamera=false;allocated.clear();plannedGunner=0;plannedGunnerUntil=0;
    run=std::max(run+1,(uint64_t(GetTickCount64())<<20)|((uint64_t(GetCurrentProcessId())&0xFFF)<<8)|(++runCounter&0xFF));
    active=true;passed=false;missionName=name;started=NowMs();snapshotRevision=0;Snapshot();LOGI("Story run begin %llu %s",(unsigned long long)run,name.c_str());}
void UpdateCooperativeStoryName(const std::string&name){if(Net().IsHost()&&active&&!name.empty()&&missionName!=name){missionName=name;Snapshot();}}
void MarkCooperativeStoryPassed(){if(Net().IsHost()&&active)passed=true;}
void EndCooperativeStory(int32_t reward){if(!Net().IsHost()||!active)return;
    if(passed&&reward>0&&reward<=10000000)for(auto&[id,p]:Net().Peers())if(id!=Net().MyId()){
        if(outbox.size()>=64){LOGW("Story reward outbox full; retaining unpaid records");break;}outbox.push_back({run,reward,p.name});SaveOutbox();SendCredit(outbox.back(),id);}
    ReleaseStoryCargo();RetireRoles(true);active=false;enemies.clear();allies.clear();groups.clear();memberGroups.clear();allySeats.clear();releasedAllies.clear();mandatoryCamera=false;Snapshot();LOGI("Story run end %llu passed=%d reward=%d",(unsigned long long)run,passed,passed?reward:0);}
void ResyncCooperativeStory(uint8_t player){if(!Net().IsHost())return;Snapshot();for(auto&c:outbox)if(c.name==Net().PeerName(player))SendCredit(c,player);}
bool LocalCooperativeRoleReady(){if(!active||Net().IsHost()||Net().MyId()>=3)return true;auto&r=roles[Net().MyId()];if(r.mode==0||r.mode==1||r.assignment==declinedAssignment)return true;if(r.mode==3)return localRoleRun==run&&localMode==3&&Cmd(0x0241).I(0).Run();if(r.mode==4)return localRoleRun==run&&localMode==4&&FindPlayerPed(0)->AttachedTo();auto*n=Vehicles::Instance().Find(r.vehicle);auto*me=FindPlayerPed(0);return n&&Vehicles::Alive(*n)&&me&&me->InVehicle()&&me->Vehicle()==n->veh;}
bool StoryRoleOwnsCamera(){return active&&!Net().IsHost()&&Net().MyId()<3&&roles[Net().MyId()].mode>0&&!mandatoryCamera&&roles[Net().MyId()].combat;}
void SetCooperativeStoryCamera(bool mandatory){if(!Net().IsHost()||mandatoryCamera==mandatory)return;mandatoryCamera=mandatory;if(run)Snapshot();}
uint8_t CooperativeReservedSeats(CVehicle*car){if(!active||!car)return 0;auto*n=Vehicles::Instance().Find(car);if(!n)return 0;uint8_t mask=0;
    for(auto&[id,a]:allySeats)if(a.vehicle==n->netId&&a.vehicleGeneration==Net().EntityGeneration(ENTITY_VEHICLE,a.vehicle)&&NowMs()<a.until){auto*p=GetPed(Peds::Instance().HandleOf(a.ped));if(p&&p->Health()>0&&a.pedGeneration==Net().EntityGeneration(ENTITY_PED,a.ped))mask|=(uint8_t)(1u<<a.seat);}return mask;
}
uint8_t LocalStoryRoleMode(){return active&&Net().MyId()<3?roles[Net().MyId()].mode:0;}
bool IsCooperativeRemoteVehicle(CVehicle*car,uint8_t player){if(!active||player>=3||roles[player].mode!=3)return false;auto*n=Vehicles::Instance().Find(roles[player].vehicle);return n&&Vehicles::Alive(*n)&&n->veh==car&&Net().EntityGeneration(ENTITY_VEHICLE,n->netId)==roles[player].generation;}
CPed* CooperativeRemoteController(CVehicle*car){if(!car||!active)return nullptr;for(auto&r:roles)if(r.mode==3){if(IsCooperativeRemoteVehicle(car,r.player))return Player(r.player);auto*n=Vehicles::Instance().Find(r.source);if(n&&Vehicles::Alive(*n)&&n->veh==car&&Net().EntityGeneration(ENTITY_VEHICLE,n->netId)==r.sourceGeneration)return Player(Net().HostId());}return nullptr;}
bool IsCooperativeRoleVehicle(CVehicle*car){for(auto&r:roles)if(r.vehicle)if(auto*n=Vehicles::Instance().Find(r.vehicle);n&&Vehicles::Alive(*n)&&n->veh==car)return true;return false;}
void CooperativeStoryShot(uint8_t player,int weapon,const CVector&start,const CVector&end){
    if(!active||!core::IsPlayerReady()||player>=3||weapon<22||weapon>38||NowMs()-lastNoise[player]<80||Ref<uint8_t>(0xB5F851))return;
    auto*shooter=Player(player);if(!shooter||shooter->Health()<=0||(start-shooter->GetPosition()).Length()>10)return;lastNoise[player]=NowMs();
    int delivered=0,budget=24;
    for(auto&[id,n]:Peds::Instance().All())if(n.owner==Net().MyId()&&n.ped&&IsPedValid(n.ped)&&n.ped->CreatedBy()==PED_MISSION&&n.ped->Health()>0&&n.ped->Intelligence()&&
        (AreaOf(n.ped)==AreaOf(shooter)||AreaOf(n.ped)==13)&&(n.ped->GetPosition()-start).Length()<120&&budget-->0){
        // Native CEventGunShot applies hearing, silencer, FOV, group and
        // decision-maker rules; it does not turn every NPC hostile to everyone.
        alignas(4)uint8_t event[0x34]{};CallMethod<0x4AC610>(event,(CEntity*)shooter,start,end,weapon==23);
        if(CallMethod<0x4AB420,void*>((uint8_t*)n.ped->Intelligence()+0x68,(void*)event,false))++delivered;
        CallVirtual<0>(event,0);
    }
    LOGD("Story shot event source=%u weapon=%d accepted=%d",player,weapon,delivered);
}
void RecordStoryAggressor(CPed*ped,uint8_t player){auto*n=Peds::Instance().Find(ped);if(!n||!active||player>=3)return;auto i=enemies.find(n->netId);if(i!=enemies.end()){i->second.aggressor=player;i->second.hit=NowMs();}}
void ObserveStoryCommand(int opcode,const int32_t*p){if(!Net().IsHost()||!active)return;
    if(opcode==0x0713&&p[0]==GetPedRef(FindPlayerPed(0))){auto*me=FindPlayerPed(0);auto*v=me->VehicleIfIn();if(!v&&me->AttachedTo()&&me->AttachedTo()->IsVehicle())v=(CVehicle*)me->AttachedTo();if(auto*n=Vehicles::Instance().EnsureNetworked(v)){plannedGunner=n->netId;plannedGunnerGeneration=Net().EntityGeneration(ENTITY_VEHICLE,n->netId);plannedGunnerUntil=NowMs()+5000;}return;}
    if((opcode==0x0687||opcode==0x0792||opcode==0x05CD||opcode==0x0622)&&p[0]==GetPedRef(FindPlayerPed(0))){plannedGunner=0;plannedGunnerUntil=0;}
    if(opcode==0x0318){MarkCooperativeStoryPassed();return;}
    if(opcode==0x0630){auto*leader=GetPed(p[1]);auto id=leader?PlayerId(leader):255;if(id<3)groups[p[0]]=id;return;}
    if(opcode==0x0633){if(auto*n=Peds::Instance().Find(GetPed(p[0]))){memberGroups.erase(n->netId);allies.erase(n->netId);releasedAllies.insert(n->netId);allySeats.erase(n->netId);}return;}
    if(opcode==0x0632){groups.erase(p[0]);for(auto i=memberGroups.begin();i!=memberGroups.end();)if(i->second==p[0]){releasedAllies.insert(i->first);allies.erase(i->first);allySeats.erase(i->first);i=memberGroups.erase(i);}else ++i;return;}
    if(opcode==0x0631){if(auto*ped=GetPed(p[1]))if(auto*n=Peds::Instance().Find(ped)){releasedAllies.erase(n->netId);memberGroups[n->netId]=p[0];}if(auto i=groups.find(p[0]);i!=groups.end())if(auto*ped=GetPed(p[1]))if(auto*n=Peds::Instance().EnsureNetworked(ped))allies[n->netId]=i->second;return;}
    if(opcode==0x05CA){auto*ped=GetPed(p[0]);auto*car=GetVehicle(p[1]);if(!ped||!car||ped->CreatedBy()!=PED_MISSION)return;auto*np=Peds::Instance().EnsureNetworked(ped);auto*nv=Vehicles::Instance().EnsureNetworked(car);if(!np||!nv||allySeats.size()>=32)return;
        int seat=p[3];if(seat<0)for(int i=0;i<car->MaxPassengers();++i)if(!car->Passengers()[i]&&!(CooperativeReservedSeats(car)&(1u<<i))){seat=i;break;}
        if(seat>=0&&seat<car->MaxPassengers())allySeats[np->netId]={np->netId,nv->netId,Net().EntityGeneration(ENTITY_PED,np->netId),Net().EntityGeneration(ENTITY_VEHICLE,nv->netId),NowMs()+45000,(int8_t)seat};Snapshot();return;}
    if(opcode==0x05CD||opcode==0x0622||opcode==0x05BA||opcode==0x05D3||opcode==0x0605||opcode==0x0792||opcode==0x0687){if(auto*n=Peds::Instance().Find(GetPed(p[0])))allySeats.erase(n->netId);return;}
    if(opcode!=0x05E2&&opcode!=0x0634&&opcode!=0x0635&&opcode!=0x074D&&opcode!=0x07A5)return;
    auto*ped=GetPed(p[0]);auto*target=GetPed(p[1]);if(!ped||target!=FindPlayerPed(0)||ped->CreatedBy()!=PED_MISSION)return;
    auto*n=Peds::Instance().EnsureNetworked(ped);if(!n)return;auto&e=enemies[n->netId];e.generation=Net().EntityGeneration(ENTITY_PED,n->netId);e.target=Net().MyId();e.opcode=opcode;
}
uint8_t CooperativeAllyLeader(CPed*ped,uint8_t fallback){if(!ped)return 255;if(auto*n=Peds::Instance().Find(ped)){if(releasedAllies.count(n->netId))return 255;}if(auto*n=Peds::Instance().Find(ped))if(auto i=allies.find(n->netId);i!=allies.end())return i->second;
    int group=-1;Cmd(0x07AF).I(0).Out(&group).Run();if(group>=0&&Cmd(0x06EE).I(GetPedRef(ped)).I(group).Run())return Net().MyId();
    for(auto&[id,g]:adoptedGroups)if(Cmd(0x06EE).I(GetPedRef(ped)).I(g).Run())return id;return fallback;
}
void RestoreCooperativeAlly(CPed*ped,uint8_t leader){if(!ped||leader>=3||!Participant(leader))return;
    int group=-1;
    if(leader==Net().MyId()){Cmd(0x07AF).I(0).Out(&group).Run();if(group>=0&&Cmd(0x06EE).I(GetPedRef(ped)).I(group).Run())return;}
    else if(auto i=adoptedGroups.find(leader);i!=adoptedGroups.end()&&Cmd(0x06EE).I(GetPedRef(ped)).I(i->second).Run())return;
    if(leader==Net().MyId())Cmd(0x07AF).I(0).Out(&group).Run();else{
        auto i=adoptedGroups.find(leader);if(i==adoptedGroups.end()){Cmd(0x062F).I(0).Out(&group).Run();if(group>=0){Cmd(0x0630).I(group).I(GetPedRef(Player(leader))).Run();adoptedGroups[leader]=group;}}else group=i->second;}
    if(group>=0){Cmd(0x0631).I(group).I(GetPedRef(ped)).Run();LOGI("NPC follower restored leader=%u ped=%d",leader,GetPedRef(ped));}
}
void WriteStoryRewardHistory(Writer&w){w.U8((uint8_t)ledger.paid.size());for(auto token:ledger.paid)w.U64(token);}
bool ReadStoryRewardHistory(Reader&r){auto count=r.U8();if(count>128)return false;std::vector<uint64_t>paid;for(int i=0;i<count;++i){auto token=r.U64();if(!token)return false;paid.push_back(token);}if(!r.Ok())return false;ledger.paid=std::move(paid);return true;}
void StoryProfileReady(bool ready){profileReady=ready;}
void StoryRewardProfileSaved(){if(!profileReady)return;for(auto token:awaitingProfile)Acknowledge(token);awaitingProfile.clear();}
}
