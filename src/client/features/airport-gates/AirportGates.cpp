#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/airport-gates/AirportGates.h"
#include "shared/api/net-session/Session.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/sync/SnapshotBuffer.h"

namespace coop { namespace {
using namespace game;

// APCHECK creates these eight script objects; APGATE moves the same objects.
// They have neither a map dummy nor a mission-cleanup entry. Never create a
// replacement barrier or identify one by its current (sliding) position alone.
struct Definition { CVector origin,open; };
const std::array<Definition,8> definitions{{
    {{-1543.742f,-432.703f,6.039f},{-1540.660f,-435.786f,6.039f}},
    {{-1547.625f,-428.820f,6.039f},{-1550.709f,-425.736f,6.039f}},
    {{-1222.953f,53.826f,14.134f},{-1226.590f,50.189f,14.134f}},
    {{-1218.206f,68.883f,14.134f},{-1214.392f,72.697f,14.134f}},
    {{1964.342f,-2189.776f,13.533f},{1968.697f,-2189.776f,13.553f}},
    {{1958.851f,-2189.777f,13.553f},{1954.571f,-2189.777f,13.553f}},
    {{1704.777f,1605.165f,10.058f},{1703.400f,1600.518f,10.058f}},
    {{1706.364f,1610.422f,10.058f},{1707.722f,1614.937f,10.058f}}
}};
struct Pose { uint32_t time=0;CVector position;Quat rotation; };
struct RemoteState {
    uint64_t birth=0,received=0;
    uint32_t sequence=0;
    bool active=false,authorized=false;
    uint8_t phase=0,flags=129;
    Pose pose;
    SnapshotBuffer<Pose> history;
};
struct Gate {
    CObject* object=nullptr;int handle=-1;
    uint64_t birth=0,sent=0;
    bool authorized=false,direct=false,active=false,dirty=false,transfer=false;
    uint8_t phase=0,simulator=255,lastSimulator=255;
    uint64_t lastAuthorityAt=0;
    uint64_t localAuthorityAt=0;
    uint8_t lastAuthorityPhase=0;
    uint8_t lastAuthorityFlags=129;
    Pose lastAuthorityPose;
    std::array<RemoteState,3> peers;
};
struct ScriptFrame {uint8_t gate=255,before=0;int handle=-1;};
std::array<Gate,8> gates;
std::map<void*,ScriptFrame> scriptFrames;
uint64_t scanned=0;
uint32_t birthCounter=0,sequence=0;
int gateModel=0;
uint16_t permissionOffset=0,accessOffset=0;
bool hostPermissionKnown=false,hostPermission=false;
uint64_t hostPermissionAt=0,hostPermissionRun=0,permissionSent=0,sentPermissionRun=0;
uint32_t permissionSequence=0;
bool sentPermissionKnown=false,sentPermission=false;

bool Named(void*script,const char*name){return script&&!_strnicmp(Field<char[8]>(script,8),name,8);}
bool OnlineWorld(){return Net().Online()&&core::IsPlayerReady()&&!core::IsRestarting()&&flow::InHostWorld();}
bool Alive(const Gate&g){return g.object&&GetObject(g.handle)==g.object&&g.object->ModelIndex()==gateModel;}
CMatrix* Matrix(Gate&g){
    // Distant native script objects can still use CPlaceable's simple transform.
    // GetMatrix materializes that transform on the same object, as native SCM
    // GET_OFFSET_FROM_OBJECT_IN_WORLD_COORDS does when a player approaches.
    return CallMethod<0x411990,CMatrix*>(g.object);
}
bool Point(const CVector&p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&p.Length()<25000;}
float PathDistance(uint8_t id,const CVector&p){
    auto a=definitions[id].origin,d=definitions[id].open-a;float length=d.Dot(d);
    float t=length>0?std::clamp((p-a).Dot(d)/length,0.0f,1.0f):0;
    return (p-(a+d*t)).Length();
}
bool PlayerPoint(uint8_t id,CVector&point,int8_t&area){
    if(id==Net().MyId()){
        auto*me=core::IsPlayerReady()?FindPlayerPed(0):nullptr;
        if(!me||me->Health()<=0)return false;
        area=me->AreaCode();point=me->GetPosition();
        if(auto*v=me->VehicleIfIn();v&&IsVehicleValid(v))point=v->GetPosition();
        return true;
    }
    auto*p=Players::Instance().Get(id);
    if(!p||!p->hasState||NowMs()-p->stateRecvMs>1500||p->state.health<=0)return false;
    area=p->state.area;point=p->state.pos;
    if(p->mode!=RemotePlayer::Mode::OnFoot)if(auto*v=Vehicles::Instance().Find(p->vehNetId);v&&Vehicles::Alive(*v)&&
        (v->owner==Net().MyId()||NowMs()-v->recvMs<1500))point=v->veh->GetPosition();
    return true;
}
bool Near(uint8_t id,uint8_t gate,float radius){
    CVector point;int8_t area;
    return PlayerPoint(id,point,area)&&area==0&&(point-definitions[gate].origin).Length()<radius;
}
bool Fresh(const RemoteState&s){return s.received&&NowMs()-s.received<1500;}
void LearnPermission(){
    if(permissionOffset||!core::IsPlayerReady())return;
    auto size=Ref<uint32_t>(0xA444C4); // CTheScripts::MainScriptSize
    if(size<11||size>200000)return;
    const auto*data=(const uint8_t*)addr::ScriptSpace;
    const uint8_t name[]={0xA4,0x03,0x09,'A','P','C','H','E','C','K',0};
    for(uint32_t i=0;i+sizeof(name)<=size;++i)if(!memcmp(data+i,name,sizeof name)){
        // Learn the actual native permission operand, rather than relying on
        // the byte offset used by one installation of main.scm.
        auto end=std::min<uint32_t>(size,i+0x240);
        for(uint32_t j=i+sizeof(name);j+7<=end;++j)if(data[j]==0x38&&data[j+1]==0&&data[j+2]==2){
            uint16_t offset;memcpy(&offset,data+j+3,2);
            bool one=data[j+5]==4&&data[j+6]==1;
            if(data[j+5]==1&&j+10<=end){int32_t value;memcpy(&value,data+j+6,4);one=value==1;}
            if(one&&offset>=8&&!(offset&3u)){
                permissionOffset=offset;LOGI("Airport native permission operand=%u",offset);return;
            }
        }
        return;
    }
}
void LearnAccess(){
    if(accessOffset||!core::IsPlayerReady())return;
    auto size=Ref<uint32_t>(0xA444C4);if(size<11||size>200000)return;
    const auto*data=(const uint8_t*)addr::ScriptSpace;
    const uint8_t name[]={0xA4,0x03,0x09,'A','P','G','A','T','E',0,0};
    for(uint32_t i=0;i+sizeof(name)<=size;++i)if(!memcmp(data+i,name,sizeof name)){
        auto end=std::min<uint32_t>(size,i+0x240);
        for(uint32_t j=i+sizeof(name);j+7<=end;++j)
            if(data[j]==0x18&&data[j+1]==0&&data[j+2]==2&&data[j+5]==4&&data[j+6]==0){
                uint16_t offset;memcpy(&offset,data+j+3,2);
                if(offset>=8&&!(offset&3u)){accessOffset=offset;LOGI("Airport native access operand=%u",offset);return;}
            }
        return;
    }
}
bool CurrentPermission(bool&value){
    if(!permissionOffset||!core::IsPlayerReady())return false;
    int flag=Ref<int32_t>(addr::ScriptSpace+permissionOffset);
    if(flag!=0&&flag!=1)return false;
    value=flag==1;return true;
}
bool SharedPermission(){
    if(Net().IsHost()){bool value=false;return CurrentPermission(value)&&value;}
    return hostPermissionKnown&&hostPermission&&NowMs()-hostPermissionAt<1500&&
        hostPermissionRun&&hostPermissionRun==StoryRunId();
}
void SendPermission(bool force=false){
    if(!Net().IsHost()||!OnlineWorld())return;LearnPermission();
    bool value=false;bool known=CurrentPermission(value);uint64_t run=StoryRunId();value=value&&run!=0;
    if(!force&&known==sentPermissionKnown&&value==sentPermission&&run==sentPermissionRun&&NowMs()-permissionSent<500)return;
    if(++sequence==0)++sequence;
    Writer w(proto::G_AIRPORT_GATE);w.U8(2);w.U32(sequence);w.U64(run);w.Bool(known);w.Bool(value);
    Net().Send(proto::CH_RELIABLE,w);permissionSent=NowMs();sentPermissionKnown=known;sentPermission=value;sentPermissionRun=run;
}
void Elect(uint8_t id){
    auto&g=gates[id];uint8_t best=255;int bestRank=2;
    for(uint8_t peer=0;peer<3;++peer){
        bool local=peer==Net().MyId();auto&s=g.peers[peer];
        bool available=local?(Alive(g)&&Near(peer,id,g.active?63.0f:60.0f)):
            (s.active&&Fresh(s)&&Near(peer,id,66));
        if(!available)continue;
        int rank=(local?g.authorized:s.authorized)?0:1;
        if(rank<bestRank||(rank==bestRank&&peer<best)){best=peer;bestRank=rank;}
    }
    if(best==Net().MyId())g.localAuthorityAt=NowMs();
    if(best==g.simulator)return;
    uint8_t old=g.simulator;
    if(old<3&&old!=Net().MyId()&&g.peers[old].active&&Fresh(g.peers[old])){
        auto&s=g.peers[old];g.lastAuthorityPhase=s.phase;g.lastAuthorityAt=s.received;g.lastSimulator=old;
        g.lastAuthorityPose=s.pose;g.lastAuthorityFlags=s.flags;
    }
    g.simulator=best;g.transfer=best==Net().MyId()&&g.lastSimulator<3&&g.lastSimulator!=best&&NowMs()-g.lastAuthorityAt<2000;
    g.dirty=true;
    LOGI("Airport gate %u simulator %u -> %u native-access=%u",id,old,best,bestRank==0);
}
bool Following(uint8_t id){
    auto&g=gates[id];auto source=g.simulator;
    return source<3&&source!=Net().MyId()&&g.peers[source].active&&Fresh(g.peers[source]);
}
int Identify(CObject*object){
    if(!object||!IsObjectValid(object)||object->ModelIndex()!=gateModel)return -1;
    for(uint8_t i=0;i<gates.size();++i)if(Alive(gates[i])&&gates[i].object==object)return i;
    float best=1.25f;int found=-1;
    for(uint8_t i=0;i<definitions.size();++i){float distance=PathDistance(i,object->GetPosition());if(distance<best){best=distance;found=i;}}
    return found;
}
void Bind(uint8_t id,CObject*object){
    auto&g=gates[id];int handle=GetObjectRef(object);
    if(g.object==object&&g.handle==handle)return;
    g.object=object;g.handle=handle;g.phase=0;g.authorized=false;g.direct=false;g.active=false;g.transfer=false;
    if(++birthCounter==0)++birthCounter;g.birth=((uint64_t)Net().WorldEpoch()<<32)|birthCounter;
    g.dirty=true;
    // A loaded host world can already have a legitimately open native gate.
    if(Net().IsHost()&&(object->GetPosition()-definitions[id].open).Length()<.2f){g.phase=2;g.authorized=true;g.direct=true;}
    LOGI("Airport gate %u adopted handle=%d model=%d birth=%llu matrix=%u area=%d",id,handle,gateModel,(unsigned long long)g.birth,object->Matrix()!=nullptr,object->AreaCode());
}
void Scan(){
    if(!core::IsPlayerReady()||NowMs()-scanned<100)return;scanned=NowMs();
    LearnPermission();LearnAccess();
    int model=Ref<int32_t>(0xA44B70+50*0x1C+0x18);
    if(model<=0||!IsValidModel(model))return;gateModel=model;
    auto*pool=ObjectPool();
    for(int i=0;pool&&i<pool->size;++i)if(pool->byteMap[i]>=0){
        auto*object=(CObject*)(pool->objects+(size_t)i*kObjectPoolElemSize);
        // Native CObject::Init uses area13 (visible in every area), including
        // these exterior APCHECK objects. It does not mean an interior gate.
        if(object->ModelIndex()!=gateModel||(object->AreaCode()!=0&&object->AreaCode()!=13))continue;
        int gate=Identify(object);if(gate>=0&&!Alive(gates[gate]))Bind((uint8_t)gate,object);
    }
}
void Place(Gate&g,const Pose&pose,uint8_t flags){
    if(!Alive(g)||!Point(pose.position))return;
    auto*o=g.object;auto*matrix=Matrix(g);if(!matrix)return;auto old=Quat::FromMatrix(*matrix);
    float turn=std::fabs(old.x-pose.rotation.x)+std::fabs(old.y-pose.rotation.y)+std::fabs(old.z-pose.rotation.z)+std::fabs(old.w-pose.rotation.w);
    if((o->GetPosition()-pose.position).Length()>.001f||turn>.0001f){
        WorldRemove(o);pose.rotation.ToMatrix(*matrix);matrix->pos=pose.position;
        CallMethod<0x446F90>(o);CallMethod<0x532B00>(o);WorldAdd(o);
    }
    o->MoveSpeed()={};o->TurnSpeed()={};o->Flags()=(o->Flags()&~129u)|(flags&129u);
}
void FinishReset(uint8_t id){
    auto&g=gates[id];if(g.simulator!=255||!Alive(g)||g.lastSimulator>=3||g.lastSimulator==Net().MyId())return;
    auto&s=g.peers[g.lastSimulator];
    if(!Fresh(s)||s.active||s.phase!=0||(s.pose.position-definitions[id].origin).Length()>=.1f)return;
    if(g.lastAuthorityPhase==0&&g.lastAuthorityAt==s.received)return;
    // The reliable object reset may arrive before the player's departure
    // state. Keep the native terminal pose and retry as that state catches up.
    for(uint8_t peer=0;peer<3;++peer)if(Near(peer,id,66))return;
    Place(g,s.pose,s.flags);g.phase=0;g.authorized=false;g.direct=false;g.dirty=true;
    g.lastAuthorityPhase=0;g.lastAuthorityPose=s.pose;g.lastAuthorityFlags=s.flags;g.lastAuthorityAt=s.received;
    LOGI("Airport gate %u native reset from peer %u",id,g.lastSimulator);
}
void Follow(uint8_t id){
    auto&g=gates[id];if(!Following(id)||!Alive(g))return;
    auto&s=g.peers[g.simulator];auto pose=s.history.At(NowMs(),[](const Pose&a,const Pose&b,float t){return Pose{b.time,a.position+(b.position-a.position)*t,Quat::Slerp(a.rotation,b.rotation,t)};},[](Pose&,float,bool){});
    Place(g,pose,s.flags);g.phase=s.phase;
    if(!s.phase){g.authorized=false;g.direct=false;}
    g.lastAuthorityPhase=s.phase;g.lastAuthorityAt=s.received;g.lastSimulator=g.simulator;
    g.lastAuthorityPose=s.pose;g.lastAuthorityFlags=s.flags;
}
void Send(uint8_t id,bool reliable=false){
    auto&g=gates[id];if(!Alive(g))return;
    if(++sequence==0)++sequence;
    bool active=OnlineWorld()&&Near(Net().MyId(),id,g.active?63.0f:60.0f);
    Writer w(proto::G_AIRPORT_GATE);w.U8(1);w.U8(id);w.U16((uint16_t)gateModel);auto origin=definitions[id].origin;w.Vec({origin.x,origin.y,origin.z});
    w.U32(sequence);w.U64(g.birth);w.U32((uint32_t)NowMs());w.Bool(active);w.Bool(g.authorized);w.U8(g.phase);w.U8((uint8_t)(g.object->Flags()&129u));
    auto*matrix=Matrix(g);if(!matrix)return;
    auto at=g.object->GetPosition();w.Vec({at.x,at.y,at.z});Quat::FromMatrix(*matrix).Write(w);
    Net().Send(reliable?proto::CH_RELIABLE:proto::CH_STATE,w);g.sent=NowMs();g.active=active;g.dirty=false;
}
void Receive(uint8_t from,Reader&r){
    auto op=r.U8();
    if(op==2){
        auto serial=r.U32();auto run=r.U64();bool known=r.Bool(),permission=r.Bool();
        if(!r.Ok()||Net().IsHost()||from!=Net().HostId()||!serial||(permissionSequence&&(int32_t)(serial-permissionSequence)<=0))return;
        permissionSequence=serial;hostPermissionKnown=known;hostPermission=permission;hostPermissionRun=run;hostPermissionAt=NowMs();return;
    }
    if(op!=1)return;
    auto id=r.U8();auto model=r.U16();auto origin=r.Vec();auto serial=r.U32();auto birth=r.U64();Pose pose;pose.time=r.U32();bool active=r.Bool(),authorized=r.Bool();auto phase=r.U8(),flags=r.U8();auto at=r.Vec();pose.position={at.x,at.y,at.z};pose.rotation.Read(r);
    if(!r.Ok()||from>=3||from==Net().MyId()||id>=gates.size()||!serial||!birth||phase>3||(flags&~129u)||
       !IsValidModel(model)||!Point(pose.position)||(CVector{origin.x,origin.y,origin.z}-definitions[id].origin).Length()>.03f||PathDistance(id,pose.position)>1.5f)return;
    if(gateModel&&model!=gateModel)return;
    auto&g=gates[id];auto&s=g.peers[from];if(s.sequence&&(int32_t)(serial-s.sequence)<=0)return;
    if(active&&!Near(from,id,70))return;
    if(s.birth!=birth)s.history.Clear();
    if(!s.history.Push(pose,NowMs()))return;
    s.sequence=serial;s.birth=birth;s.received=NowMs();s.active=active;s.authorized=authorized;s.phase=phase;s.flags=flags;s.pose=pose;
    if(OnlineWorld()){Elect(id);FinishReset(id);Follow(id);}
}
void SetPhase(void*script,uint8_t id,uint8_t phase){
    Field<int32_t>(script,0x3C+6*4)=phase;
    auto origin=definitions[id].origin;
    Field<float>(script,0x3C+11*4)=origin.x;Field<float>(script,0x3C+14*4)=origin.y;Field<float>(script,0x3C+17*4)=origin.z;
}
void Clear(){
    gates={};scriptFrames.clear();scanned=0;birthCounter=sequence=0;gateModel=0;
    permissionOffset=accessOffset=0;hostPermissionKnown=hostPermission=false;hostPermissionAt=hostPermissionRun=permissionSent=sentPermissionRun=0;
    permissionSequence=0;sentPermissionKnown=sentPermission=false;
}
class AirportGates:public Module {
    const char*Name()const override{return "AirportGates";}
    void OnInstallHooks()override{Net().SetHandler(proto::G_AIRPORT_GATE,&Receive);}
    void OnPreProcess()override{
        if(!OnlineWorld())return;Scan();SendPermission();
        for(uint8_t i=0;i<gates.size();++i){Elect(i);FinishReset(i);Follow(i);}
    }
    void OnPostProcess()override{
        if(!OnlineWorld())return;Scan();
        for(uint8_t i=0;i<gates.size();++i){auto&g=gates[i];if(!Alive(g))continue;Elect(i);FinishReset(i);Follow(i);
            bool nearby=Near(Net().MyId(),i,g.active?63.0f:60.0f);
            // APCHECK can perform its real reset a few frames after the first
            // inactive/open packet. Publish that dirty closed result as well.
            bool reset=g.dirty&&g.phase==0&&g.localAuthorityAt&&NowMs()-g.localAuthorityAt<1500&&
                (g.object->GetPosition()-definitions[i].origin).Length()<.1f;
            if((nearby||g.active||reset)&&(g.dirty||NowMs()-g.sent>=100))Send(i,g.dirty||!nearby);
        }
    }
    void OnPeerResync(uint8_t)override{if(OnlineWorld()){Scan();SendPermission(true);for(uint8_t i=0;i<gates.size();++i)if(Alive(gates[i])&&Near(Net().MyId(),i,63))Send(i,true);}}
    void OnPeerLeave(uint8_t id)override{if(id<3)for(auto&g:gates)g.peers[id]={};}
    void OnPeerJoin(uint8_t id)override{OnPeerLeave(id);}
    void OnHostChanged(uint8_t)override{Clear();}
    void OnGameShutdown()override{Clear();}
    void OnSessionEnd()override{Clear();}
}feature;struct Reg{Reg(){core::RegisterModule(&feature);}}reg;
}

void BeginAirportGateScript(void*script){
    if(!OnlineWorld()||!Named(script,"APGATE"))return;
    Scan();auto*object=GetObject(Field<int32_t>(script,0x3C));int id=Identify(object);if(id<0)return;
    auto&g=gates[id];Bind((uint8_t)id,object);Elect((uint8_t)id);
    int phase=Field<int32_t>(script,0x3C+6*4);if(phase<0||phase>3)return;
    if(Following((uint8_t)id))SetPhase(script,(uint8_t)id,g.peers[g.simulator].phase);
    else if(g.transfer){
        if(!g.authorized||phase==0){
            Place(g,g.lastAuthorityPose,g.lastAuthorityFlags);
            SetPhase(script,(uint8_t)id,g.lastAuthorityPhase);
        }
        g.transfer=false;
    }
    scriptFrames[script]={(uint8_t)id,(uint8_t)Field<int32_t>(script,0x3C+6*4),g.handle};
}
void EndAirportGateScript(void*script){
    auto it=scriptFrames.find(script);if(it==scriptFrames.end())return;auto frame=it->second;scriptFrames.erase(it);
    if(!Named(script,"APGATE")||frame.gate>=gates.size())return;
    auto&g=gates[frame.gate];if(!Alive(g)||g.handle!=frame.handle)return;
    if(Following(frame.gate)){
        // A distant local player must not advance the copied phase2 to closing
        // or replace the elected simulator's remembered closed coordinates.
        SetPhase(script,frame.gate,g.peers[g.simulator].phase);Follow(frame.gate);
        return;
    }
    int phase=Field<int32_t>(script,0x3C+6*4);if(phase<0||phase>3)return;
    bool changed=g.phase!=phase;g.phase=(uint8_t)phase;
    if(frame.before==0&&phase==1){g.authorized=true;g.direct=false;g.dirty=true;LOGI("Airport gate %u native access peer=%u",frame.gate,Net().MyId());}
    if(phase==0&&g.authorized&&!g.direct){g.authorized=false;g.dirty=true;}
    if(changed)g.dirty=true;
    Elect(frame.gate);
    if(g.dirty&&Near(Net().MyId(),frame.gate,63))Send(frame.gate,true);
}
int AirportGateOperandCount(void*script,int opcode){
    if(!OnlineWorld())return 0;opcode&=0x7FFF;
    if(Named(script,"APCHECK"))return opcode==0x01BC?4:opcode==0x0038?2:0;
    if(Named(script,"APGATE"))return opcode==0x034E?8:opcode==0x01BC?4:opcode==0x0471?5:opcode==0x0018?2:0;
    return 0;
}
bool HandleAirportGateCommand(void*script,int opcode,const int32_t*p,bool&value){
    if(!p||!OnlineWorld())return false;opcode&=0x7FFF;bool check=Named(script,"APCHECK"),gate=Named(script,"APGATE");
    if(!check&&!gate)return false;
    if(check&&opcode==0x0038&&p[1]==1){
        auto*ip=Field<const uint8_t*>(script,0x14);
        if(!ip||ip[0]!=2)return false;uint16_t offset;memcpy(&offset,ip+1,2);
        if(!permissionOffset||permissionOffset!=offset)return false;
        if(Net().IsHost())return false;
        value=SharedPermission();return true;
    }
    if(gate&&opcode==0x0018&&p[1]==0&&!Net().IsHost()){
        auto*ip=Field<const uint8_t*>(script,0x14);
        if(!ip||ip[0]!=2)return false;
        uint16_t offset;memcpy(&offset,ip+1,2);if(!accessOffset||accessOffset!=offset)return false;
        int id=Identify(GetObject(Field<int32_t>(script,0x3C)));if(id<0)return false;
        // APCHECK may already be waiting inside the entrance when the host's
        // mission grants access. Let its existing APGATE react immediately,
        // without editing this game's globals or the player's flying skill.
        value=id<2&&SharedPermission();return true;
    }
    int handle=opcode==0x0471?p[1]:p[0];auto*object=GetObject(handle);int id=Identify(object);if(id<0)return false;
    auto&g=gates[id];Bind((uint8_t)id,object);
    Elect((uint8_t)id);
    if((opcode==0x034E||opcode==0x01BC)&&Following((uint8_t)id)){value=false;return true;}
    if(check&&opcode==0x01BC){
        CVector target;memcpy(&target,p+1,sizeof target);
        if((target-definitions[id].origin).Length()<.05f){g.authorized=false;g.direct=false;g.phase=0;g.dirty=true;}
        else if((target-definitions[id].open).Length()<.05f){
            if(Net().IsHost()||SharedPermission()){g.authorized=true;g.direct=true;g.phase=2;g.dirty=true;}
            else {value=false;return true;}
        }
    }
    Elect((uint8_t)id);
    if(opcode==0x0471){
        float dx,dy;memcpy(&dx,p+2,4);memcpy(&dy,p+3,4);
        if(!gate||p[0]!=GetPedRef(FindPlayerPed(0))||std::fabs(dx-20)>.01f||std::fabs(dy-20)>.01f||g.simulator!=Net().MyId())return false;
        for(auto&[peer,_]:Net().Peers()){
            CVector at;int8_t area;
            if(peer==Net().MyId()||!PlayerPoint(peer,at,area)||area!=0||
               (object->AreaCode()!=0&&object->AreaCode()!=13))continue;
            auto d=at-object->GetPosition();if(std::fabs(d.x)<=dx&&std::fabs(d.y)<=dy){value=true;return true;}
        }
        return false;
    }
    return false;
}
bool IsAirportGate(game::CObject*object){return OnlineWorld()&&Identify(object)>=0;}
uint8_t AirportGateSimulator(game::CObject*object){
    if(!OnlineWorld())return 255;int id=Identify(object);if(id<0)return 255;Elect((uint8_t)id);return gates[id].simulator;
}
}
