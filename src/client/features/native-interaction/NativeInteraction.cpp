#include <array>
#include <map>
#include <cmath>
#include "shared/api/native-interaction/NativeInteraction.h"
#include "shared/api/net-session/Session.h"
#include "entities/player/Players.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "common/lib/util/Util.h"
#include "common/lib/log/Log.h"
namespace coop {namespace {
using namespace game;
struct Lease{uint8_t owner=255;uint32_t revision=0;uint64_t until=0,received=0;};
std::map<uint64_t,Lease>leases;
uint64_t localKey=0,asked=0,granted=0;uint16_t localModel=0;int8_t localArea=0;CVector localPoint;uint32_t revision=0,lastDecision=0;
bool Model(uint16_t m){return m==65500||m==65501||(m>=1830&&m<=1838)||m==1948||m==1978||m==2188||m==1947||m==2947||(m>=2627&&m<=2630)||m==1209||m==1302||m==1775||m==1776||m==1977||m==2778||m==2779||m==2964||m==3094;}
void Decision(uint64_t key,Lease&lease){Writer w(proto::G_INTERACTION);w.U8(2);w.U64(key);w.U32(lease.revision);w.U8(lease.owner);Net().Send(proto::CH_RELIABLE,w);}
uint8_t Acquire(uint64_t key,uint8_t player){auto i=leases.find(key);if(i==leases.end()&&leases.size()>=128)return 255;auto&lease=leases[key];auto now=NowMs();
    if(lease.owner==255||now>=lease.until||lease.owner==player){bool changed=lease.owner!=player||now>=lease.until;lease.owner=player;lease.until=now+6000;lease.received=now;lease.revision=++revision;if(changed)LOGI("Native interaction acquired key=%llu player=%u",(unsigned long long)key,player);}
    Decision(key,lease);return lease.owner;
}
void Release(uint64_t key,uint8_t player){auto i=leases.find(key);if(i==leases.end()||i->second.owner!=player)return;i->second.owner=255;i->second.until=0;i->second.revision=++revision;Decision(key,i->second);LOGI("Native interaction released key=%llu player=%u",(unsigned long long)key,player);}
bool KnownScript(void*script,uint16_t&model,CVector&point){
    if(!script)return false;auto*n=Field<char[8]>(script,8);model=0;
    if(!_strnicmp(n,"gymbenc",7))model=2629;else if(!_strnicmp(n,"gymdumb",7))model=2628;else if(!_strnicmp(n,"gymbike",7))model=2630;else if(!_strnicmp(n,"gymtrea",7))model=2627;
    if(model){int index=model==2628?4:5;point={Field<float>(script,0x3C+index*4),Field<float>(script,0x3C+(index+1)*4),Field<float>(script,0x3C+(index+2)*4)};return std::isfinite(point.x)&&std::isfinite(point.y)&&std::isfinite(point.z)&&(point-FindPlayerPed(0)->GetPosition()).Length()<5;}
    // Native service scripts enter by walking into a marker, without a
    // button query. Their own provider position is the stable station key.
    if(!_strnicmp(n,"BARB",4)||!_strnicmp(n,"TATTO",5)){
        bool barber=!_strnicmp(n,"BARB",4);int off=barber?38980:38556;
        point={Ref<float>(addr::ScriptSpace+off),Ref<float>(addr::ScriptSpace+off+4),Ref<float>(addr::ScriptSpace+off+8)};model=barber?65500:65501;
        return std::isfinite(point.x)&&std::isfinite(point.y)&&std::isfinite(point.z)&&(point-FindPlayerPed(0)->GetPosition()).Length()<6;
    }
    bool casino=!_strnicmp(n,"ROULETE",7)||!_strnicmp(n,"BLACKJ",6)||!_strnicmp(n,"BANDIT",6)||!_strnicmp(n,"VIDPOK",6);
    if(casino){auto*object=GetObject(Field<int32_t>(script,0x3C));if(!object||!Model(object->ModelIndex()))return false;model=object->ModelIndex();point=object->GetPosition();return (point-FindPlayerPed(0)->GetPosition()).Length()<6;}
    if(_strnicmp(n,"vend",4)&&_strnicmp(n,"fodvend",7)&&_strnicmp(n,"pool2",5)&&_strnicmp(n,"dual",4)&&_strnicmp(n,"grav",4)&&_strnicmp(n,"shtr",4))return false;
    std::array<CEntity*,64>objects{};int16_t count=0;auto pos=FindPlayerPed(0)->GetPosition();
    using Find=void(__cdecl*)(const CVector&,float,bool,int16_t*,int16_t,CEntity**,bool,bool,bool,bool,bool);
    reinterpret_cast<Find>(0x564A20)(pos,4.f,false,&count,64,objects.data(),true,false,false,true,true);
    float best=4.f;for(int i=0;i<count&&i<64;++i)if(objects[i]&&Model(objects[i]->ModelIndex())){float d=(objects[i]->GetPosition()-pos).Length();if(d<best){best=d;model=objects[i]->ModelIndex();point=objects[i]->GetPosition();}}return model!=0;
}
class Interactions:public Module{const char*Name()const override{return "NativeInteractions";}
 void OnInstallHooks()override{Net().SetHandler(proto::G_INTERACTION,[](uint8_t from,Reader&r){auto op=r.U8();auto key=r.U64();if(!key||from>=3)return;
    if(op==2){auto rev=r.U32();auto owner=r.U8();if(!r.Ok()||Net().IsHost()||from!=Net().HostId()||(owner>=3&&owner!=255))return;if(lastDecision&&(int32_t)(rev-lastDecision)<0)return;lastDecision=rev;auto i=leases.find(key);if(i==leases.end()&&leases.size()>=128)return;auto&l=leases[key];if(l.received&&(int32_t)(rev-l.revision)<0)return;l={owner,rev,NowMs()+6000,NowMs()};if(key==localKey&&owner==Net().MyId()&&!granted)granted=NowMs();return;}
    if(!Net().IsHost()||!Net().Peer(from))return;if(op==3){if(r.Ok())Release(key,from);return;}if(op!=1)return;
    auto model=r.U16();auto area=r.I8();auto p=r.Vec();auto*rp=Players::Instance().Get(from);if(!r.Ok()||!Model(model)||key!=NativeInteractionKey(model,area,p.x,p.y,p.z)||!rp||!rp->hasState||NowMs()-rp->stateRecvMs>1500||rp->state.area!=area||(rp->state.pos-CVector{p.x,p.y,p.z}).Length()>6)return;Acquire(key,from);
 });}
 void OnPostProcess()override{if(!Net().Online()||!core::IsPlayerReady())return;auto now=NowMs();
    if(Net().IsHost())for(auto&[key,l]:leases)if(l.owner<3&&now>=l.until)Release(key,l.owner);
    for(auto i=leases.begin();i!=leases.end();)i=(i->first!=localKey&&now>i->second.until+6000)?leases.erase(i):std::next(i);
    if(localKey&&!granted&&now-asked>1500)ReleaseNativeInteraction();
    if(localKey){auto*me=FindPlayerPed(0);bool menu=Ref<uint8_t>(0xBA82E0)||Ref<uint8_t>(0xBA82E1);
        bool usingIt=me->AreaCode()==localArea&&(me->GetPosition()-localPoint).Length()<6&&(GetPad(0)->DisablePlayerControls||menu||now-granted<2000);
        if(granted&&!usingIt)ReleaseNativeInteraction();else if(now-asked>2000)RequestNativeInteraction(localModel,localArea,localPoint.x,localPoint.y,localPoint.z);
    }
 }
 void OnPeerResync(uint8_t player)override{OnPeerLeave(player);}
 void OnPeerLeave(uint8_t player)override{if(Net().IsHost())for(auto&[key,l]:leases)Release(key,player);}
 void OnGameShutdown()override{ReleaseNativeInteraction();leases.clear();}
 void OnSessionEnd()override{OnGameShutdown();revision=lastDecision=0;}
}feature;struct Register{Register(){core::RegisterModule(&feature);}}registration;
}
uint64_t NativeInteractionKey(uint16_t model,int8_t area,float x,float y,float z){
    if(!Model(model)||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||std::fabs(x)>20000||std::fabs(y)>20000||std::fabs(z)>5000)return 0;
    uint64_t h=1469598103934665603ull;for(int32_t value:{(int32_t)model,(int32_t)area,(int32_t)std::lround(x*4),(int32_t)std::lround(y*4),(int32_t)std::lround(z*4)}){h^=(uint32_t)value;h*=1099511628211ull;}return h?h:1;
}
uint8_t NativeInteractionOwner(uint64_t key){auto i=leases.find(key);return i!=leases.end()&&NowMs()<i->second.until?i->second.owner:255;}
bool RequestNativeInteraction(uint16_t model,int8_t area,float x,float y,float z){
    auto key=NativeInteractionKey(model,area,x,y,z);if(!key||!Net().Online())return true;if(localKey&&localKey!=key){if(granted)return false;ReleaseNativeInteraction();}
    localKey=key;localModel=model;localArea=area;localPoint={x,y,z};auto now=NowMs();
    if(Net().IsHost()){asked=now;if(Acquire(key,Net().MyId())==Net().MyId()){if(!granted)granted=now;return true;}return false;}
    if(now-asked>250){Writer w(proto::G_INTERACTION);w.U8(1);w.U64(key);w.U16(model);w.I8(area);w.Vec({x,y,z});Net().Send(proto::CH_RELIABLE,w);asked=now;}
    return NativeInteractionOwner(key)==Net().MyId();
}
bool OwnsNativePoolTable(){return localKey&&localModel==2964&&NativeInteractionOwner(localKey)==Net().MyId();}
void ReleaseNativeInteraction(){if(localKey&&Net().Online()){if(Net().IsHost())Release(localKey,Net().MyId());else{Writer w(proto::G_INTERACTION);w.U8(3);w.U64(localKey);Net().Send(proto::CH_RELIABLE,w);}}localKey=asked=granted=0;}
bool BeginNativeInteraction(void*script){
    if(!Net().Online()||!core::IsPlayerReady())return true;
    uint16_t model=0;CVector point;if(!KnownScript(script,model,point))return true;
    auto key=NativeInteractionKey(model,FindPlayerPed(0)->AreaCode(),point.x,point.y,point.z);
    if(key==localKey&&NativeInteractionOwner(key)==Net().MyId())return true;
    return RequestNativeInteraction(model,FindPlayerPed(0)->AreaCode(),point.x,point.y,point.z);
}
void EndNativeInteraction(void*script){
    uint16_t model=0;CVector point;if(!core::IsPlayerReady()||!KnownScript(script,model,point))return;
    if(localKey==NativeInteractionKey(model,FindPlayerPed(0)->AreaCode(),point.x,point.y,point.z))ReleaseNativeInteraction();
}
bool NativeInteractionAllowed(void*script,int button){
    if(!Net().Online()||!core::IsPlayerReady()||(button!=15&&button!=16))return true;
    uint16_t model=0;CVector point;if(!KnownScript(script,model,point))return true;
    if(localKey&&NativeInteractionOwner(localKey)==Net().MyId())return true;
    if(!game::script::Cmd(0x00E1).I(0).I(button).Run())return true;
    return RequestNativeInteraction(model,FindPlayerPed(0)->AreaCode(),point.x,point.y,point.z);
}
}
