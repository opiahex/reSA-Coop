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
constexpr uintptr_t kCutsceneTrackManager=0x8AE554;
// CAECutsceneTrackManager keeps the playing track id at +0x24, followed by
// the playback clock at +0x28. A track id (e.g. INTRO1A's 703) is not a time.
int32_t TrackPlaybackTime(){return Ref<int32_t>(kCutsceneTrackManager+0x28);}
struct Sound{int event=0;AudioTarget target;float volume=0,speed=1;uint64_t received=0;};
std::deque<Sound> pending;
std::vector<uint8_t> pendingTrack;
uint32_t controlSequence=0,receivedControl=0,pendingControl=0;uint64_t lastAlignment=0;uint64_t droppedSounds=0;
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
    Writer w(proto::G_SCRIPT_SOUND);w.U8(op);w.U32(++controlSequence);w.U32(trackToken);w.I16((int16_t)nativeTrack);w.U8(to);w.U32(trackPlaying?(uint32_t)std::max(0,TrackPlaybackTime()):0);
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
    if(op>=2&&op<=4) {
        auto seq=r.U32();uint32_t token=r.U32();int track=r.I16();uint8_t to=r.U8();auto offset=r.U32();
        if(!r.Ok()||(to!=proto::kServerId&&to!=Net().MyId())||track<0||track>4096||offset>3600000||
           (receivedControl&&(int32_t)(seq-receivedControl)<=0)||(receivedToken&&(int32_t)(token-receivedToken)<0))return;
        receivedControl=seq;
        receivedToken=token;
        if(op==2) {
            if(nativeTrack!=track)LoadTrack((void*)0xB6BC90,nullptr,(int16_t)track,false);
            nativeTrack=track;
            LOGI("Shared soundtrack %d preloaded (token %u)",track,token);
        } else if(op==3) {
            if(nativeTrack!=track||offset>350){LoadTrack((void*)0xB6BC90,nullptr,(int16_t)track,false);Ref<uint32_t>(0x8AE554+0x14)=offset;}
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
            auto*begin=r.Cursor();auto size=r.Remaining();auto op=r.U8();
            if(op>=2&&op<=4){
                // Coalesce desired soundtrack state independently of one-shot
                // sound pressure. PLAY includes its preload; STOP is never lost.
                auto seq=r.U32();r.U32();auto track=r.I16();auto to=r.U8();auto offset=r.U32();
                if(r.Ok()&&track>=0&&track<=4096&&offset<=3600000&&(to==proto::kServerId||to==Net().MyId())&&(!receivedControl||(int32_t)(seq-receivedControl)>0)&&(!pendingControl||(int32_t)(seq-pendingControl)>0)){pendingControl=seq;pendingTrack.assign(begin,begin+size);}
                return;
            }
            if(op!=1)return;Sound s;s.event=r.I32();if(!s.target.Read(r))return;s.volume=r.F32();s.speed=r.F32();s.received=NowMs();
            if(!r.Ok()||s.event<0||s.event>=100000||!std::isfinite(s.volume)||!std::isfinite(s.speed))return;
            if(pending.size()>=256){pending.pop_front();++droppedSounds;}
            pending.push_back(std::move(s));
        });
    }
    void OnPreProcess()override {
        if(!core::IsPlayerReady())return;applying=true;
        if(!pendingTrack.empty()){auto bytes=std::move(pendingTrack);pendingTrack.clear();Reader r(bytes.data(),bytes.size());Receive(r);}
        int budget=16;
        while(!pending.empty()&&budget>0){auto s=std::move(pending.front());pending.pop_front();
            if(NowMs()-s.received>1500){++droppedSounds;continue;}
            --budget;CVector point{s.target.position.x,s.target.position.y,s.target.position.z};
            originalSound((void*)(0xB6BC90+0x2A0),nullptr,s.event,&point,s.target.Resolve(),std::clamp(s.volume,-100.f,20.f),std::clamp(s.speed,.1f,4.f));
        }
        if(droppedSounds){LOGW("Shared sound backlog discarded stale/overflow one-shots=%llu; soundtrack state retained",(unsigned long long)droppedSounds);droppedSounds=0;}
        applying=false;
    }

    void OnPeerResync(uint8_t id)override {
        if(Net().IsHost()&&nativeTrack>=0&&HostReplicating()){
            SendTrack(2,id);if(trackPlaying)SendTrack(3,id);
        }
    }
    void OnSessionEnd()override {pending.clear();pendingTrack.clear();pendingControl=0;lastAlignment=0;receivedControl=controlSequence=0;receivedToken=trackToken=0;nativeTrack=-1;trackPlaying=false;}
    void OnGameShutdown()override {pending.clear();pendingTrack.clear();pendingControl=0;lastAlignment=0;receivedControl=0;nativeTrack=-1;trackPlaying=false;}
} sounds;
struct Register{Register(){core::RegisterModule(&sounds);}} registration;
}
void AlignSharedSoundtrack(uint32_t milliseconds,bool hard){
    if(Net().IsHost()||nativeTrack<0||!trackPlaying||NowMs()-lastAlignment<3000)return;
    int actual=TrackPlaybackTime();
    // Loading/stopping clocks are negative native status values. Do not start
    // another preload while that operation is still in progress: even the
    // native non-waiting preload waits synchronously for the old stream to stop.
    if(actual<0||Ref<int32_t>(kCutsceneTrackManager+8)!=3||
       Ref<int32_t>(kCutsceneTrackManager+0x24)!=nativeTrack)return;
    if(!hard&&std::abs((int64_t)actual-(int64_t)milliseconds)<600)return;
    lastAlignment=NowMs();applying=true;LoadTrack((void*)0xB6BC90,nullptr,(int16_t)nativeTrack,false);
    Ref<uint32_t>(0x8AE554+0x14)=milliseconds;originalPlay((void*)0xB6BC90,nullptr);trackPlaying=true;applying=false;
    LOGI("Shared soundtrack aligned offset=%u previous=%d",milliseconds,actual);
}

}
