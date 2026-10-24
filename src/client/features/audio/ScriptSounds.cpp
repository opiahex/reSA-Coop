#include <deque>
#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "entities/audio/AudioTarget.h"
#include "entities/mission/Mission.h"
#include "shared/lib/memory/Memory.h"

namespace coop {
namespace {
using namespace game;
using SoundFn=void(__fastcall*)(void*,void*,int,CVector*,CPhysical*,float,float);
using TrackLoadFn=void(__fastcall*)(void*,void*,int16_t,bool);
using TrackPlayFn=void(__fastcall*)(void*,void*);
using TrackStopFn=void(__fastcall*)(void*,void*,bool);
SoundFn originalSound=nullptr;
TrackLoadFn originalLoad=nullptr;
TrackPlayFn originalPlay=nullptr;
TrackStopFn originalStop=nullptr;
bool applying=false;
int nativeTrack=-1;
uint32_t trackToken=0, receivedToken=0;
bool trackPlaying=false;
std::deque<std::vector<uint8_t>> pending;
bool Shared() { return !applying && Net().Online() && Net().IsHost() && HostReplicating(); }
void __fastcall ScriptSound(void* audio,void* edx,int event,CVector* pos,CPhysical* entity,float volume,float speed) {
    if(Shared()&&pos) {
        Writer w(proto::G_SCRIPT_SOUND);w.U8(1);w.I32(event);
        AudioTarget::Describe(entity,*pos).Write(w);w.F32(volume);w.F32(speed);
        Net().Send(proto::CH_RELIABLE,w);
    }
    originalSound(audio,edx,event,pos,entity,volume,speed);
}
void SendTrack(uint8_t op,uint8_t to=proto::kServerId) {
    Writer w(proto::G_SCRIPT_SOUND);w.U8(op);w.U32(trackToken);w.I16((int16_t)nativeTrack);w.U8(to);
    Net().Send(proto::CH_RELIABLE,w);
}
void __fastcall LoadTrack(void* audio,void* edx,int16_t track,bool wait) {
    nativeTrack=track;trackPlaying=false;
    loadingSharedSoundtrack=true;
    originalLoad(audio,edx,track,wait);
    loadingSharedSoundtrack=false;
    if(Shared()){if(++trackToken==0)++trackToken;SendTrack(2);}
}
void __fastcall PlayTrack(void* audio,void* edx) {
    originalPlay(audio,edx);trackPlaying=true;
    if(Shared()&&nativeTrack>=0)SendTrack(3);
}
void __fastcall StopTrack(void* audio,void* edx,bool wait) {
    if(Shared()&&nativeTrack>=0)SendTrack(4);
    trackPlaying=false;originalStop(audio,edx,wait);
}
void Receive(Reader& r) {
    uint8_t op=r.U8();
    if(op==1) {
        int event=r.I32();AudioTarget target;
        if(!target.Read(r))return;
        float volume=r.F32(),speed=r.F32();
        if(!r.Ok()||event<0||event>=100000||!std::isfinite(volume)||!std::isfinite(speed))return;
        CVector point{target.position.x,target.position.y,target.position.z};
        originalSound((void*)(0xB6BC90+0x2A0),nullptr,event,&point,target.Resolve(),
                      std::clamp(volume,-100.0f,20.0f),std::clamp(speed,0.1f,4.0f));
        LOGD("Shared script sound %d replayed",event);
    } else if(op>=2&&op<=4) {
        uint32_t token=r.U32();int track=r.I16();uint8_t to=r.U8();
        if(!r.Ok()||(to!=proto::kServerId&&to!=Net().MyId())||track<0||track>4096||
           (receivedToken&&(int32_t)(token-receivedToken)<0))return;
        receivedToken=token;
        if(op==2) {
            if(nativeTrack!=track)LoadTrack((void*)0xB6BC90,nullptr,(int16_t)track,false);
            nativeTrack=track;
            LOGI("Shared soundtrack %d preloaded (token %u)",track,token);
        } else if(op==3) {
            if(nativeTrack!=track)LoadTrack((void*)0xB6BC90,nullptr,(int16_t)track,false);
            nativeTrack=track;originalPlay((void*)0xB6BC90,nullptr);trackPlaying=true;
            LOGI("Shared soundtrack %d play requested (token %u)",track,token);
        } else { originalStop((void*)0xB6BC90,nullptr,false);trackPlaying=false; }
    }
}
class ScriptSounds:public Module {
public:
    const char* Name()const override{return "ScriptSounds";}
    void OnInstallHooks()override {
        mem::HookFn(0x4ECCF0,&ScriptSound,&originalSound,"CAEScriptAudioEntity::ProcessMissionAudioEvent");
        mem::HookFn(0x507E30,&LoadTrack,&originalLoad,"CAudioEngine::PreloadCutsceneTrack");
        mem::HookFn(0x507070,&PlayTrack,&originalPlay,"CAudioEngine::PlayPreloadedCutsceneTrack");
        mem::HookFn(0x507080,&StopTrack,&originalStop,"CAudioEngine::StopCutsceneTrack");
        Net().SetHandler(proto::G_SCRIPT_SOUND,[](uint8_t from,Reader&r){
            if(Net().IsHost()||from!=Net().HostId())return;
            if(pending.size()<128)pending.emplace_back(r.Cursor(),r.Cursor()+r.Remaining());
        });
    }
    void OnPreProcess()override {
        if(!core::IsPlayerReady())return;
        auto messages=std::move(pending);pending.clear();applying=true;
        for(auto&message:messages){Reader r(message.data(),message.size());Receive(r);}
        applying=false;
    }
    void OnPeerResync(uint8_t id)override {
        if(Net().IsHost()&&nativeTrack>=0&&HostReplicating()){
            SendTrack(2,id);if(trackPlaying)SendTrack(3,id);
        }
    }
    void OnSessionEnd()override {pending.clear();receivedToken=trackToken=0;nativeTrack=-1;trackPlaying=false;}
    void OnGameShutdown()override {pending.clear();nativeTrack=-1;trackPlaying=false;}
} sounds;
struct Register{Register(){core::RegisterModule(&sounds);}} registration;
}
}
