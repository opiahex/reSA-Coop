// Selected map operations from upstream 95d336a, with bounded world-scoped
// state, model resolution before replay, streaming retries and three clients.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>
#include "entities/mission/Mission.h"
#include "entities/session/Flow.h"
#include "shared/api/net-session/Session.h"
#include "shared/api/story-world/StoryWorld.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
namespace coop {namespace {
using namespace game;using game::script::Cmd;
enum:uint8_t{MAP=1,IPL=2,PROGRESS=3};
struct MapEdit{int op=0;CVector at;float radius=0;int model=0,value=0;};
std::vector<MapEdit> edits;std::map<std::string,uint8_t> ipls;
std::array<uint32_t,5> progress{},sentProgress{};bool haveProgress=false,progressSent=false;
uint64_t sentAt=0,appliedAt=0;uint32_t revision=0,receivedRevision=0;
bool Valid(const MapEdit&e){return(e.op==0x0363||e.op==0x088D||e.op==0x03B6)&&std::isfinite(e.at.x)&&std::isfinite(e.at.y)&&std::isfinite(e.at.z)&&e.at.Length()<25000&&std::isfinite(e.radius)&&e.radius>0&&e.radius<=200&&e.model>0&&e.model<20000&&(e.op!=0x03B6||(e.value>0&&e.value<20000));}
int ResolveModel(int m){return m<0&&m>-395?Ref<int32_t>(0xA44B70+(uintptr_t)(-m)*0x1C+0x18):m;}
void Remember(const MapEdit&e){
    for(auto i=edits.begin();i!=edits.end();)i=(i->op==e.op&&i->model==e.model&&(i->at-e.at).Length()<.25f)?edits.erase(i):std::next(i);
    if(edits.size()<512)edits.push_back(e);
}
void Send(const MapEdit&e){Writer w(proto::G_STORY_WORLD);w.U8(MAP);w.U16(e.op);w.Vec({e.at.x,e.at.y,e.at.z});w.F32(e.radius);w.I32(e.model);w.I32(e.value);Net().Send(proto::CH_RELIABLE,w);}
void SendIpl(const std::string&name,uint8_t op){Writer w(proto::G_STORY_WORLD);w.U8(IPL);w.U8(op);w.Str(name,32);Net().Send(proto::CH_RELIABLE,w);}
void SendProgress(){
    std::array<uint32_t,5> values{};memcpy(values.data(),(void*)(0xB79000+(146-120)*4),12);memcpy(values.data()+3,(void*)0xB79380,8);
    if(progressSent&&values==sentProgress&&NowMs()-sentAt<3000)return;
    sentProgress=values;progressSent=true;sentAt=NowMs();Writer w(proto::G_STORY_WORLD);w.U8(PROGRESS);w.U32(++revision);for(auto v:values)w.U32(v);Net().Send(proto::CH_RELIABLE,w);
}
using IplFn=void(__cdecl*)(int);IplFn originalIpl[3]{};
template<int Op>void __cdecl ScriptIpl(int slot){
    originalIpl[Op](slot);auto*s=CurrentScript();
    if(!Net().Online()||!Net().IsHost()||!HostReplicating()||!s||!Field<uint8_t>(s,0xDC)||LocalSchoolActivity())return;
    const char*name=Call<0x404A60,const char*>(slot);if(!name||!*name||strnlen(name,33)>32)return;
    if(ipls.size()<128||ipls.count(name)){ipls[name]=Op;SendIpl(name,Op);LOGI("Story map IPL sent name=%s op=%d",name,Op);}
}
void Receive(uint8_t from,Reader&r){
    if(from!=Net().HostId()||Net().IsHost()||!core::IsPlayerReady()||!flow::InHostWorld())return;
    auto op=r.U8();
    if(op==MAP){MapEdit e;e.op=r.U16();auto p=r.Vec();e.at={p.x,p.y,p.z};e.radius=r.F32();e.model=r.I32();e.value=r.I32();if(!r.Ok()||!Valid(e))return;Remember(e);appliedAt=0;LOGI("Story map edit received op=%04X model=%d",e.op,e.model);}
    else if(op==IPL){auto action=r.U8();auto name=r.Str(32);if(!r.Ok()||action>2||name.empty()||!std::all_of(name.begin(),name.end(),[](unsigned char c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';}))return;
        int slot=Call<0x404AC0,int>(name.c_str());if(slot<0)return;if(action==0)Call<0x405850>(slot);else if(action==1)Call<0x405890>(slot);else Call<0x4058D0>(slot);LOGI("Story map IPL applied name=%s op=%u",name.c_str(),action);}
    else if(op==PROGRESS){auto rev=r.U32();std::array<uint32_t,5> values;for(auto&v:values)v=r.U32();float f[2];memcpy(f,values.data()+3,8);
        if(!r.Ok()||(receivedRevision&&(int32_t)(rev-receivedRevision)<=0)||values[0]>1000000||values[1]>1000000||values[2]>1000000||!std::isfinite(f[0])||!std::isfinite(f[1])||f[0]<0||f[1]<0||f[0]>1e6f||f[1]>1e6f)return;
        progress=values;receivedRevision=rev;haveProgress=true;}
}
class StoryWorld:public Module{
    const char*Name()const override{return "StoryWorld";}
    void OnInstallHooks()override{
        Net().SetHandler(proto::G_STORY_WORLD,&Receive);
        mem::RedirectCallFn(0x46B6ED,&ScriptIpl<0>,&originalIpl[0]);
        mem::RedirectCallFn(0x46B715,&ScriptIpl<1>,&originalIpl[1]);
        mem::RedirectCallFn(0x46B73D,&ScriptIpl<2>,&originalIpl[2]);
    }
    void OnPostProcess()override{
        if(!Net().Online()||!core::IsPlayerReady()||!flow::InHostWorld())return;
        if(Net().IsHost()){if(NowMs()-sentAt>500)SendProgress();return;}
        if(haveProgress){memcpy((void*)(0xB79000+(146-120)*4),progress.data(),12);memcpy((void*)0xB79380,progress.data()+3,8);}
        if(NowMs()-appliedAt<500)return;appliedAt=NowMs();auto*me=FindPlayerPed(0);
        for(auto&e:edits)if((me->GetPosition()-e.at).Length()<160||(cam::Source()-e.at).Length()<160){
            if(!IsValidModel(e.model)||(e.op==0x03B6&&!IsValidModel(e.value)))continue;
            if(e.op==0x03B6&&!IsModelLoaded(e.value)){RequestModel(e.value,0x10);continue;}
            Cmd(e.op).F(e.at.x).F(e.at.y).F(e.at.z).F(e.radius).I(e.model).I(e.value).Run();
        }
    }
    void OnPeerResync(uint8_t)override{if(Net().IsHost()){for(auto&e:edits)Send(e);for(auto&[name,op]:ipls)SendIpl(name,op);progressSent=false;}}
    void OnPeerJoin(uint8_t)override{progressSent=false;}
    void OnGameShutdown()override{edits.clear();ipls.clear();haveProgress=progressSent=false;receivedRevision=0;sentAt=appliedAt=0;}
    void OnSessionEnd()override{OnGameShutdown();revision=0;}
}feature;struct Reg{Reg(){core::RegisterModule(&feature);}}reg;
}
bool ResolveStoryMapTarget(int opcode,int*p){
    if(!p)return false;float f[4];memcpy(f,p,16);int model=ResolveModel(p[4]);
    MapEdit e{opcode,{f[0],f[1],f[2]},f[3],model,opcode==0x03B6?ResolveModel(p[5]):p[5]};if(!Valid(e))return false;
    using Find=game::CEntity*(__cdecl*)(int,const game::CVector&,float,bool,bool,bool,bool,bool,bool);
    auto*target=((Find)0x5693F0)(model,e.at,e.radius,false,true,false,false,opcode!=0x03B6,true);
    if(!target)return false;auto at=target->GetPosition();float exact[4]={at.x,at.y,at.z,.25f};memcpy(p,exact,sizeof exact);p[4]=model;p[5]=e.value;
    // Replaying a radius search after replacing its nearest model could select
    // the next building on every retry. Keep the original target's exact point.
    return true;
}
void RecordStoryMapCommand(int opcode,const int*p){
    if(!Net().Online()||!Net().IsHost())return;float f[4];memcpy(f,p,16);
    MapEdit e{opcode,{f[0],f[1],f[2]},f[3],ResolveModel(p[4]),opcode==0x03B6?ResolveModel(p[5]):p[5]};
    if(!Valid(e))return;Remember(e);Send(e);LOGI("Story map edit sent op=%04X model=%d",opcode,e.model);
}
}
