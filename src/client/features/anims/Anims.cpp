// Replication of animations that the pad replay cannot reproduce.
//
// Idle animations: CTaskSimplePlayerOnFoot::PlayIdleAnimations (0x6872C0) starts a random
// "playidles" animation after 20 s of inactivity, judged by the *local* pad touch timer. For the
// remote player ped this would use the wrong timer and a different random choice, so the function is
// skipped for it; instead the owner reports the idle animation it plays and the other side blends the
// same animation on the remote ped.
//
// The same channel (G_PLAYER_ANIM) is used for other one-shot animations (see kReplicatedGroups).
#include <algorithm>
#include <map>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Anim.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr int ANIM_GROUP_PLAYIDLES = 49;
constexpr uint16_t ANIMATION_IDLE_FLAG = 0x200;  // set by PlayIdleAnimations on its animations

enum : uint8_t { OP_PLAY = 1, OP_STOP_IDLE = 2, OP_SCRIPT = 3 };

// groups whose animations are replicated as events (they are not produced by the pad replay)
constexpr int kReplicatedGroups[] = {ANIM_GROUP_PLAYIDLES};

bool IsReplicatedGroup(int g) {
    for (int r : kReplicatedGroups)
        if (r == g) return true;
    return false;
}

bool IsReplicated(int group,int id) {
    return IsReplicatedGroup(group)||(group==0&&id>=145&&id<=147);
}

bool EnsureBlockLoaded(const char* block) {
    int idx = Call<0x4D3990, int>(block);  // CAnimManager::GetAnimationBlockIndex
    if (idx < 0) return false;
    if (anim::BlockLoaded(idx)) return true;
    RequestModel(anim::kIfpBase + idx, 2);
    return false;
}

const char* BlockForGroup(int group) { return group == ANIM_GROUP_PLAYIDLES ? "playidles" : group==0 ? "ped" : nullptr; }

bool HoldsMovement(const AnimationState&s){
    return s.Present()&&(!(s.flags&anim::F_PARTIAL)||(s.group==87&&s.id>=348&&s.id<=350));
}

using PlayIdleFn = void(__fastcall*)(void*, void*, CPlayerPed*);
PlayIdleFn g_origPlayIdle = nullptr;
void __fastcall HookPlayIdleAnimations(void* task, void* edx, CPlayerPed* player) {
    if (Players::Instance().FindByPed(player)) return;  // remote player: idles come from the network
    g_origPlayIdle(task, edx, player);
}

class Anims : public Module {
public:
    const char* Name() const override { return "Anims"; }

    void OnInstallHooks() override {
        mem::HookFn(0x6872C0, &HookPlayIdleAnimations, &g_origPlayIdle, "CTaskSimplePlayerOnFoot::PlayIdleAnimations");
        Net().SetHandler(proto::G_PLAYER_ANIM, [](uint8_t from, Reader& r) { Instance().Handle(from, r); });
    }

    void OnPostProcess() override {
        // in a cutscene both games animate the partner themselves (ScenePartner), idle variations included
        if (!Net().Online() || !core::IsPlayerReady() || Ref<uint8_t>(0xB5F851)) return;  // CCutsceneMgr::ms_running
        CPlayerPed* me = FindPlayerPed(0);
        const uint64_t now = NowMs();
        AnimationState scripted = anim::Capture(anim::ActionAssociation(me));
        bool changed = !scripted.Same(m_scripted) ||
            (scripted.Present() && scripted.time + 0.25f < m_scripted.time && !(scripted.flags & anim::F_LOOPED));
        if (changed || (scripted.Present() && now - m_scriptSent >= 100)) {
            Writer action(proto::G_PLAYER_ANIM);
            action.U32((uint32_t)now);
            action.U32(Players::Instance().MotionEpoch());
            action.U8(OP_SCRIPT);
            action.U8(me->InVehicle()?1:0);action.Bool(me->Health()<=0);
            scripted.Write(action);
            Net().Send(proto::CH_RELIABLE, action);
            m_scriptSent = now;
        }
        m_scripted = scripted;
        for(auto it=m_pendingIdle.begin();it!=m_pendingIdle.end();){
            auto*rp=Players::Instance().Get(it->first);auto idle=it->second;
            if(!rp||!rp->ped||now-idle.recv>1500){it=m_pendingIdle.erase(it);continue;}
            if(!rp->ped->RwObject()){++it;continue;}
            if(!EnsureBlockLoaded(BlockForGroup(idle.group))){++it;continue;}
            uintptr_t group=Ref<uintptr_t>(0xB4EA34)+(uintptr_t)idle.group*0x14;
            int first=(int)Ref<uint32_t>(group+0x0C),count=(int)Ref<uint32_t>(group+0x08);
            if(idle.id>=first&&idle.id<first+count)
                if(auto*a=Call<0x4D4610,void*>(rp->ped->RwObject(),idle.group,idle.id,std::clamp(idle.delta,1.0f,50.0f)))anim::Flags(a)|=ANIMATION_IDLE_FLAG;
            it=m_pendingIdle.erase(it);
        }
        for (auto& [id, state] : m_remoteScripted) {
            RemotePlayer* rp = Players::Instance().Get(id);
            if (!rp || !rp->ped) continue;
            if(rp->hasState&&state.epoch!=rp->state.motion.epoch){
                if((int32_t)(state.epoch-rp->state.motion.epoch)>0)continue;
                if(void*a=anim::Find(rp->ped->RwObject(),state.animation))anim::FadeOut(a);
                state.animation={};rp->scriptAnimFull=false;anim::RestoreMovement(rp->ped);continue;
            }
            if (now - state.recv > 1500) {
                if (void* a = anim::Find(rp->ped->RwObject(), state.animation)) anim::FadeOut(a);
                state.animation = {};
                rp->scriptAnimFull = false;
                anim::RestoreMovement(rp->ped);
            }
            rp->scriptAnimFull = HoldsMovement(state.animation);
            if (state.animation.Present()) {
                const auto* peer = Net().Peer(id);
                float age = std::min(0.25f, (now - state.recv) / 1000.0f + 0.0005f * (Net().Rtt() + (peer ? peer->ping : 0)));
                anim::Replay(rp->ped->RwObject(), state.animation, age);
            }
        }
        // the replicated animation currently playing on the local player (blending in or full)
        int group = -1, id = -1;
        for (void* a = anim::First(me->RwObject()); a; a = anim::Next(a)) {
            if (!IsReplicated(anim::Group(a), anim::Id(a)) || anim::BlendDelta(a) < 0.0f) continue;
            group = anim::Group(a);
            id = anim::Id(a);
            break;
        }
        if (group == m_lastGroup && id == m_lastId) return;
        Writer w(proto::G_PLAYER_ANIM);
        w.U32((uint32_t)now);
        w.U32(Players::Instance().MotionEpoch());
        if (id >= 0) {
            w.U8(OP_PLAY);
            w.I16((int16_t)group);
            w.I16((int16_t)id);
            w.F32(8.0f);
        } else {
            w.U8(OP_STOP_IDLE);
        }
        Net().Send(proto::CH_RELIABLE, w);
        LOGI("Local animation %s: group %d id %d", id >= 0 ? "started" : "stopped", id >= 0 ? group : m_lastGroup,
             id >= 0 ? id : m_lastId);
        m_lastGroup = group;
        m_lastId = id;
    }

    void Handle(uint8_t from, Reader& r) {
        uint32_t time=r.U32();
        uint32_t epoch=r.U32();
        uint8_t op = r.U8();
        RemotePlayer* rp = Players::Instance().Get(from);
        if (!rp || !core::IsPlayerReady() || Ref<uint8_t>(0xB5F851)) return;
        if(auto it=m_actionTime.find(from);it!=m_actionTime.end()&&(int32_t)(time-it->second)<0)return;
        m_actionTime[from]=time;
        if(!rp->ped)return;
        if(rp->hasState&&(int32_t)(epoch-rp->state.motion.epoch)<0)return;
        void* clump = rp->ped->RwObject();
        if (!clump) return;
        if (op == OP_SCRIPT) {
            bool seated=r.U8()!=0,dead=r.Bool();
            AnimationState s;
            if (!s.Read(r)) return;
            if(rp->hasState&&(int32_t)(time-rp->lastStateTime)<0&&
               (seated!=(rp->mode!=RemotePlayer::Mode::OnFoot)||dead!=bool(rp->state.flags&OnFootState::F_DEAD)))return;
            auto& previous = m_remoteScripted[from];
            if (!previous.animation.Same(s))
                if (void* a = anim::Find(clump, previous.animation)) anim::FadeOut(a);
            previous = {s, NowMs(),epoch};
            bool wasFull = rp->scriptAnimFull;
            rp->scriptAnimFull = HoldsMovement(s);
            if (wasFull && !rp->scriptAnimFull) anim::RestoreMovement(rp->ped);
        } else if (op == OP_PLAY) {
            int group = r.I16();
            int id = r.I16();
            float delta = r.F32();
            if (!r.Ok() || !IsReplicated(group,id)) return;
            const char* block = BlockForGroup(group);
            if (block && !EnsureBlockLoaded(block)){m_pendingIdle[from]={group,id,delta,NowMs()};return;}
            uintptr_t grp = Ref<uintptr_t>(0xB4EA34) + (uintptr_t)group * 0x14;  // CAnimManager::ms_aAnimAssocGroups
            int first = (int)Ref<uint32_t>(grp + 0x0C), count = (int)Ref<uint32_t>(grp + 0x08);  // m_IdOffset, m_NumAnims
            if (id < first || id >= first + count) return;  // from the network
            void* a = Call<0x4D4610, void*>(clump, group, id, std::clamp(delta, 1.0f, 50.0f));  // CAnimManager::BlendAnimation
            if (a) anim::Flags(a) |= ANIMATION_IDLE_FLAG;
            LOGI("Remote player %d animation: group %d id %d (%s)", from, group, id, a ? "ok" : "failed");
        } else if (op == OP_STOP_IDLE) {
            m_pendingIdle.erase(from);
            for (void* a = anim::First(clump); a; a = anim::Next(a))
                if ((anim::Flags(a) & ANIMATION_IDLE_FLAG) && IsReplicated(anim::Group(a),anim::Id(a))) {
                    anim::BlendDelta(a) = -8.0f;
                    anim::Flags(a) |= anim::F_AUTO_REMOVE;  // deleted once out
                }
        }
    }

    void OnGameStarted() override { Reset(); }
    void OnSessionEnd() override { Reset(); }
    void OnPeerLeave(uint8_t id) override { ForgetRemote(id); }
    void OnPeerResync(uint8_t id) override { m_scripted = {}; m_scriptSent = 0; ForgetRemote(id); }

    static Anims& Instance() {
        static Anims a;
        return a;
    }

private:
    void ForgetRemote(uint8_t id) {
        auto previous = m_remoteScripted.find(id);
        if (auto* rp = Players::Instance().Get(id)) {
            if (rp->ped && previous != m_remoteScripted.end())
                if (void* a = anim::Find(rp->ped->RwObject(), previous->second.animation)) anim::FadeOut(a);
            rp->scriptAnimFull = false;
            if (rp->ped) anim::RestoreMovement(rp->ped);
        }
        m_remoteScripted.erase(id);
        m_actionTime.erase(id);m_pendingIdle.erase(id);
    }
    void Reset() {
        for (auto& [id, state] : m_remoteScripted)
            if (auto* rp = Players::Instance().Get(id)) {
                if (rp->ped) if (void* a = anim::Find(rp->ped->RwObject(), state.animation)) anim::FadeOut(a);
                rp->scriptAnimFull = false;
            }
        m_lastGroup = m_lastId = -1;
        m_scripted = {}; m_scriptSent = 0; m_remoteScripted.clear();
        m_actionTime.clear();m_pendingIdle.clear();
    }
    struct PendingIdle{int group,id;float delta;uint64_t recv;};
    std::map<uint8_t,PendingIdle>m_pendingIdle;
    struct RemoteAction { AnimationState animation; uint64_t recv = 0; uint32_t epoch=0; };
    std::map<uint8_t, RemoteAction> m_remoteScripted;
    std::map<uint8_t,uint32_t> m_actionTime;
    AnimationState m_scripted;
    uint64_t m_scriptSent = 0;
    int m_lastGroup = -1, m_lastId = -1;
};

struct Reg {
    Reg() { core::RegisterModule(&Anims::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
