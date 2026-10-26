// Objects of the map (lamp posts, signs, fences, bins, hydrants, trees, boxes, windows) in both games. Each game makes
// them from the same map data, so an object is known by its model and the place its map entry (CDummyObject) holds.
// What a player does to one, the other game repeats: it breaks (CObject::ObjectDamage: smashed, in pieces, its damaged
// model), its window cracks or shatters, or it is knocked loose and moves. The game where it moves sends its pose until
// it lies still, and the other game's copy follows. A game that meets such an object later (it was not streamed in
// then, or it streamed out and in again) receives the current world state.
// Records expire with the world, never independently on each participant.
#include <algorithm>
#include <cmath>
#include <vector>
#include <map>
#include <tuple>
#include "shared/lib/sync/SnapshotBuffer.h"

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/vehicle/VehicleState.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr float kRange = 150.0f;               // objects this far from our player are looked at
constexpr uint64_t kFollowMs = 1500;           // the other game's moves stop: it lies still (or the message got lost)
constexpr int kMaxSendsPerScan = 16;           // an explosion knocks dozens loose at once
constexpr uint32_t kTrainCrossEnabled = 0x800; // CObject::objectFlags.bTrainCrossEnabled
enum : uint8_t { F_MOVED = 1, F_MOVING = 2, F_CONTROLLED=4 };  // not where the map put it; still moving

struct MechanismPose{uint32_t time=0;CVector pos;Quat rot;};
struct Thing {
    uint16_t model = 0;
    CVector origin;        // where its map entry puts it: the same in both games
    uint8_t stage = 0;     // 0 whole, 1 damaged (its damaged model, a cracked window), 2 broken (smashed, in pieces)
    bool moved = false, moving = false,controlled=false;uint8_t entityFlags=129;
    uint32_t mechanismFlags = 0;
    SnapshotBuffer<MechanismPose>history;int heldHandle=-1;bool wasStatic=false;
    int nativeFlagsHandle=-1;uint32_t nativeFlagsApplied=~0u;uint64_t pathApplyMs=0;
    CVector pos, speed, turn;
    Quat rot;
    uint8_t weapon = 0;    // what broke it (CObject::m_nLastWeaponDamage): the same proofs decide in the other game
    uint8_t source=255;
    bool remote = false;   // the other game's: this one follows
    bool following = false;  // our copy plays the other game's moves
    uint64_t updatedMs = 0, sentMs = 0, appliedMs = 0;
};
std::vector<Thing> g_things;
using MapKey=std::tuple<uint16_t,int,int,int>;
MapKey Key(uint16_t model,const CVector&p){return{model,(int)std::lround(p.x*10),(int)std::lround(p.y*10),(int)std::lround(p.z*10)};}
std::map<MapKey,size_t>g_index;
Thing*Remember(uint16_t model,const CVector&origin){g_index[Key(model,origin)]=g_things.size();g_things.emplace_back();auto&t=g_things.back();t.model=model;t.origin=origin;return &t;}
void ClearObjects(){g_index.clear();g_things.clear();}
uint64_t g_scanMs = 0, g_statsMs = 0;size_t scanCursor=0;
int g_sent = 0, g_received = 0;  // messages since the last stats line

uint8_t ObjectType(CObject* o) { return Field<uint8_t>(o, 0x13C); }  // CObject::m_nObjectType: 1 the map's
CEntity* Dummy(CObject* o) {                                           // CObject::m_pDummyObject: its map entry
    auto* d = Field<CEntity*>(o, 0x170);
    auto* pool = Ref<CPoolHeader*>(0xB744A0);  // CPools::ms_pDummyPool, 0x38 bytes each
    uintptr_t off = (uintptr_t)d - (pool ? (uintptr_t)pool->objects : 0);
    if (!d || !pool || (uintptr_t)d < (uintptr_t)pool->objects || off % 0x38 || off / 0x38 >= (uintptr_t)pool->size ||
        pool->byteMap[off / 0x38] < 0)
        return nullptr;
    return d;
}

bool Crossing(uint16_t model) {
    return model == Ref<uint16_t>(0x8CD6B4) || model == Ref<uint16_t>(0x8CD6B8);
}
bool CrossingLights(uint16_t model) { return model == Ref<uint16_t>(0x8CD6B8); }
bool Controlled(CObject*o){auto special=Field<uint8_t>(o,0x145);return Field<void*>(o,0x138)||(special>=6&&special<=8);}
uint8_t ControlSource(CObject*o){
    // A map crossing has AREA_CODE_13, but a player upstairs in an interior must
    // not become the simulator of a railway crossing on the street below.
    auto*d=Dummy(o);auto at=d?d->GetPosition():o->GetPosition();uint8_t source=255;
    auto add=[&](uint8_t id,int8_t area,const CVector&pos){
        bool sameArea=Crossing(o->ModelIndex())?area==0:(area==o->AreaCode()||o->AreaCode()==13);
        if(sameArea&&(pos-at).Length()<200)source=std::min(source,id);
    };
    if(auto*me=FindPlayerPed(0))add(Net().MyId(),me->AreaCode(),me->GetPosition());
    for(auto&[id,p]:Players::Instance().All())if(p.hasState&&NowMs()-p.stateRecvMs<1500)add(id,p.state.area,p.state.pos);
    return source;
}
// what is the players' to break and move: not what the game moves itself or what explodes in the breaker's name
bool Shared(CObject* o) {
    if (ObjectType(o) != 1 || !o->Matrix() || !Dummy(o)) return false;
    uint8_t special = Field<uint8_t>(o, 0x145);  // m_nSpecialColResponseCase
    if (special == 9) return false;  // swinging and locked doors, hanging things, pool balls
    void* info = Field<void*>(o, 0x160);  // m_pObjectInfo
    return !info || !Field<uint8_t>(info, 0x1F);  // m_bCausesExplosion: CObject::Explode names this game's player
}

uint8_t StageOf(CObject* o) {
    uint32_t f = o->Flags();
    if (!(f & 1) && !(f & 0x80)) return 2;  // no collision and not drawn: smashed, in pieces, shattered
    if ((f & 0x200) || (Field<uint32_t>(o, 0x140) & 0x10)) return 1;  // m_bRenderDamaged, bHasBrokenGlass
    return 0;
}
bool Moving(CObject* o) { return !(o->Flags() & 4); }  // CEntity::m_bIsStatic off: knocked loose

bool Turned(CObject* o, CEntity* d) {
    if (Field<uint32_t>(o, 0x140) & 0x100) return false;  // bIsLampPost: poles and trees sway in the wind (their matrix)
    const CMatrix* m = o->Matrix();
    if (const CMatrix* dm = d->Matrix()) return m->up.Dot(dm->up) < 0.999f || m->forward.Dot(dm->forward) < 0.999f;
    float a = std::remainder(o->GetHeading() - d->GetHeading(), 6.2831853f);
    return m->up.z < 0.999f || std::fabs(a) > 0.045f;
}

Thing* Find(uint16_t model, const CVector& origin) {
    auto i=g_index.find(Key(model,origin));return i==g_index.end()?nullptr:&g_things[i->second];
}

void Send(const Thing& t, bool reliable) {
    Writer w(proto::G_WORLD_OBJ);
    w.U16(t.model);w.U32((uint32_t)NowMs());
    w.Vec({t.origin.x, t.origin.y, t.origin.z});
    w.U8(t.stage);
    w.U8((t.moved ? F_MOVED : 0) | (t.moving ? F_MOVING : 0) | (t.controlled?F_CONTROLLED:0));
    w.U8(t.weapon);if(t.controlled){w.U8(t.entityFlags);w.U32(t.mechanismFlags);}
    if (t.moved) {
        w.Vec({t.pos.x, t.pos.y, t.pos.z});
        t.rot.Write(w);
        w.Vec({t.speed.x, t.speed.y, t.speed.z});
        w.Vec({t.turn.x, t.turn.y, t.turn.z});
    }
    Net().Send(reliable ? proto::CH_RELIABLE : proto::CH_STATE, w);
    ++g_sent;
}

// to a pose: out of the world and back (the sector lists, and the moving list a loose object is on)
void Place(CObject* o, const CVector& pos, const Quat& rot, bool loose, const CVector& speed, const CVector& turn) {
    WorldRemove(o);
    if (bool(Moving(o)) != loose) CallVirtual<4>(o, !loose);  // CObject::SetIsStatic
    rot.ToMatrix(*o->Matrix());
    o->Matrix()->pos = pos;
    CallMethod<0x446F90>(o);  // CEntity::UpdateRW
    CallMethod<0x532B00>(o);  // CEntity::UpdateRwFrame
    ((CPhysical*)o)->MoveSpeed() = loose ? speed : CVector{};
    ((CPhysical*)o)->TurnSpeed() = loose ? turn : CVector{};
    WorldAdd(o);
}

// as the other game broke it: the same damage path shows the same pieces
void Break(CObject* o, uint8_t stage, uint8_t weapon) {
    if(!o->RwObject()){if(stage>=2)o->Flags()&=~129u;return;}

    CVector p = o->GetPosition();
    if (Call<0x46A760, bool>(o)) {  // CGlass::IsObjectGlass: windows break in CGlass, not in ObjectDamage
        if (stage >= 2) Call<0x71BC40>((CEntity*)o, 1000.0f, 0.0f, 0.0f, 0.0f, p.x, p.y, p.z, false);  // WindowRespondsToCollision
        else Field<uint32_t>(o, 0x140) |= 0x10;  // bHasBrokenGlass: cracked
        return;
    }
    CVector up{0, 0, 1};
    for (int i = 0; i < 2 && StageOf(o) < stage; ++i)  // a model that changes first and then smashes takes two
        CallMethod<0x5A0D90>(o, 10000.0f, &p, &up, (CEntity*)nullptr, (int)weapon);  // CObject::ObjectDamage
    if (stage >= 2 && StageOf(o) < 2) o->Flags() &= ~(1u | 0x80u);  // nothing to break into (or proof here): just gone
}

// a copy here in the state the record has (it was made after the change, or the change is the other game's); while
// the other game moves it, our physics carries it on between its poses
void CatchUp(CObject* o, Thing& t, bool driven) {
    if (StageOf(o) < t.stage) Break(o, t.stage, t.weapon);
    if (!t.moved || StageOf(o) >= 2) return;
    if (driven) {
        if (t.appliedMs != t.updatedMs || !t.following) Place(o, t.pos, t.rot, true, t.speed, t.turn);
    } else if (t.following || Moving(o) || (o->GetPosition() - t.pos).Length() > 0.1f) {
        Place(o, t.pos, t.rot, false, {}, {});  // where it lies
    }
    t.appliedMs = t.updatedMs;
    t.following = driven;
}

void ReleaseMechanism(Thing&t){
    if(auto*o=GetObject(t.heldHandle);o&&!t.wasStatic&&o->ModelIndex()==t.model){
        if(auto*d=Dummy(o);d&&Key(t.model,d->GetPosition())==Key(t.model,t.origin))CallVirtual<4>(o,false);
    }
    t.heldHandle=-1;
}
void ApplyMechanismFlags(CObject*o,Thing&t){
    if(!Crossing(o->ModelIndex()))return;
    auto&flags=Field<uint32_t>(o,0x140);
    flags=(flags&~kTrainCrossEnabled)|(t.mechanismFlags&kTrainCrossEnabled);
    // The warning-light object also stops road traffic through path-link state.
    // Its matrix does not move, so copying the barrier's pose cannot do this.
    const auto now=NowMs();const int handle=GetObjectRef(o);
    if(CrossingLights(o->ModelIndex())&&(t.nativeFlagsHandle!=handle||t.nativeFlagsApplied!=t.mechanismFlags||now-t.pathApplyMs>=1000)){
        auto*d=Dummy(o);if(!d)return;const auto p=d->GetPosition();
        CallMethod<0x44D960>((void*)0x96F050,p.x-12.0f,p.x+12.0f,p.y-12.0f,p.y+12.0f,bool(flags&kTrainCrossEnabled));
        t.nativeFlagsHandle=handle;t.nativeFlagsApplied=t.mechanismFlags;t.pathApplyMs=now;
    }
}
void FollowMechanism(CObject*o,Thing&t){
    if(t.heldHandle!=GetObjectRef(o)){t.heldHandle=GetObjectRef(o);t.wasStatic=(o->Flags()&4)!=0;}
    auto pose=t.history.At(NowMs(),[](const MechanismPose&a,const MechanismPose&b,float f){return MechanismPose{b.time,a.pos+(b.pos-a.pos)*f,Quat::Slerp(a.rot,b.rot,f)};},[](MechanismPose&,float,bool){});
    Place(o,pose.pos,pose.rot,false,{},{});o->Flags()=(o->Flags()&~129u)|(t.entityFlags&129u);
    ApplyMechanismFlags(o,t);
}
void FollowMechanisms(){
    auto*pool=ObjectPool();if(!pool)return;
    for(int i=0;i<pool->size;++i)if(pool->byteMap[i]>=0){auto*o=(CObject*)(pool->objects+(size_t)i*kObjectPoolElemSize);
        auto*d=Dummy(o);if(!d)continue;auto*t=Find(o->ModelIndex(),d->GetPosition());
        if(t&&t->controlled&&t->remote){
            if(!Shared(o)){ReleaseMechanism(*t);continue;} // a story subsystem adopted this instance
            if(ControlSource(o)==Net().MyId()){ApplyMechanismFlags(o,*t);ReleaseMechanism(*t);t->remote=false;t->history.Clear();}
            else if(NowMs()-t->updatedMs<kFollowMs)FollowMechanism(o,*t);
            else ReleaseMechanism(*t);
        }
    }
}

using CrossingFn=void(__fastcall*)(CObject*,void*);
CrossingFn g_originalCrossing=nullptr;
void __fastcall ProcessCrossing(CObject*o,void*edx){
    if(Net().Online()&&core::IsPlayerReady()&&Shared(o)&&Crossing(o->ModelIndex())){
        auto*d=Dummy(o);auto*t=d?Find(o->ModelIndex(),d->GetPosition()):nullptr;
        if(t&&t->controlled&&t->remote){
            if(ControlSource(o)==Net().MyId()){
                // Continue the last accepted crossing state on handover. Native
                // train detection then owns both the barrier and warning state.
                ApplyMechanismFlags(o,*t);ReleaseMechanism(*t);t->remote=false;t->history.Clear();
            }else if(NowMs()-t->updatedMs<kFollowMs){
                ApplyMechanismFlags(o,*t);
                return; // SetIsStatic does not stop ProcessControlLogic.
            }else ReleaseMechanism(*t);
        }
    }
    g_originalCrossing(o,edx);
}

void Scan() {
    CPlayerPed* me = FindPlayerPed(0);
    CPoolHeader* pool = ObjectPool();
    if (!me || !pool) return;
    CVector mp = me->GetPosition();
    uint64_t now = NowMs();
    int sends = 0;
    for (int step = 0; step < pool->size; ++step) {
        int i=(scanCursor+step)%pool->size;
        if (pool->byteMap[i] < 0) continue;
        auto* o = (CObject*)(pool->objects + (size_t)i * kObjectPoolElemSize);
        if (!Shared(o) || (o->GetPosition() - mp).Length2D() > kRange) continue;
        CEntity* d = Dummy(o);
        CVector at = o->GetPosition(), origin = d->GetPosition();
        Thing* t = Find(o->ModelIndex(), origin);
        bool controlled=Controlled(o);
        if(controlled&&ControlSource(o)!=Net().MyId())continue;
        if(controlled&&t&&t->remote){ReleaseMechanism(*t);t->remote=false;}
        if (t && t->remote) {
            // the other game moves it right now, or this copy is behind its last word: as there
            bool driven = t->moving && now - t->updatedMs < kFollowMs;
            if (driven || (StageOf(o) <= t->stage && (!Moving(o) || t->following))) {
                CatchUp(o, *t, driven);
                continue;
            }
        } else if (t && !Moving(o) && (StageOf(o) < t->stage || (t->moved && !t->moving && (at - t->pos).Length() > 0.1f))) {
            CatchUp(o, *t, false);  // ours, made again since (streamed out and in): as it was left
            continue;
        }
        uint8_t stage = controlled?0:StageOf(o);
        bool moving = controlled||(Moving(o) && stage < 2), moved = moving || (at - origin).Length() > 0.05f || Turned(o, d);
        if (stage == 0 && !moved) continue;  // as the map made it
        if (!t) {
            t=Remember(o->ModelIndex(),origin);
        }
        const uint32_t mechanismFlags=Crossing(o->ModelIndex())?(Field<uint32_t>(o,0x140)&kTrainCrossEnabled):0;
        bool changed = t->remote || (controlled&&t->source!=Net().MyId()) || stage != t->stage || moved != t->moved || moving != t->moving || mechanismFlags!=t->mechanismFlags ||
                       (moved && !moving && (at - t->pos).Length() > 0.1f);
        if (!changed && !(moving && now - t->sentMs >= 100)) continue;
        if (sends >= kMaxSendsPerScan){scanCursor=i;break;}
        ++sends;
        if (t->remote) LOGD("World object %d at %.1f,%.1f: ours now", t->model, origin.x, origin.y);
        t->source=Net().MyId();t->controlled=controlled;t->entityFlags=(uint8_t)(o->Flags()&129u);t->mechanismFlags=mechanismFlags;
        t->remote = t->following = false;
        t->stage = stage;
        t->moved = moved;
        t->moving = moving;
        t->pos = at;
        t->rot = Quat::FromMatrix(*o->Matrix());
        t->speed = ((CPhysical*)o)->MoveSpeed();
        t->turn = ((CPhysical*)o)->TurnSpeed();
        t->weapon = Field<uint8_t>(o, 0x148);  // m_nLastWeaponDamage
        t->updatedMs = t->sentMs = now;
        Send(*t, changed);
        if (changed && !moving) LOGD("World object %d at %.1f,%.1f: stage %d%s", t->model, origin.x, origin.y, stage, moved ? ", moved" : "");
    }
    // Keep final states until the authoritative world is reset.

}

// Each game has a copy of the other player, and that copy pushes things too (a mirror car runs a little ahead of the
// real one): the game of the player nearer to it moves it, the host's when both are about as near
bool OursToMove(const CVector& at, uint8_t from) {
    CPlayerPed* me = FindPlayerPed(0);
    const RemotePlayer* rp = Players::Instance().Get(from);
    if (!me || !rp || !rp->hasState) return false;
    float mine = (me->GetPosition() - at).Length(), theirs = (rp->state.pos - at).Length();
    return mine + 5.0f < theirs || (std::fabs(mine - theirs) <= 5.0f && Net().IsHost());
}

void Handle(uint8_t from, Reader& r) {
    uint16_t model = r.U16();auto tick=r.U32();
    Vec3 o = r.Vec();
    uint8_t stage = r.U8(), flags = r.U8(), weapon = r.U8();uint8_t entityFlags=flags&F_CONTROLLED?r.U8():129;
    uint32_t mechanismFlags=flags&F_CONTROLLED?r.U32():0;
    Vec3 p{}, s{}, tn{};
    Quat q;
    if (flags & F_MOVED) {
        p = r.Vec();
        q.Read(r);
        s = r.Vec();
        tn = r.Vec();
    }
    if (!r.Ok() || from == Net().MyId() || !core::IsPlayerReady() || stage > 2 || (flags&~7u) ||
        (entityFlags&~129u) || (mechanismFlags&~kTrainCrossEnabled) || (mechanismFlags&&!Crossing(model)) ||
        ((flags&F_CONTROLLED)&&!(flags&F_MOVED)) || !IsValidModel(model) || !(CVector{o.x,o.y,o.z}.Length()<25000) || ((flags&F_MOVED)&&(!(CVector{p.x,p.y,p.z}.Length()<25000)||!(CVector{s.x,s.y,s.z}.Length()<100)))) return;
    ++g_received;
    CVector origin{o.x, o.y, o.z};
    Thing* t = Find(model, origin);
    if (!(flags&F_CONTROLLED)&&(flags & F_MOVING) && OursToMove(origin, from)) return;  // our physics moves it; where it lies still comes
    if (!t) {
        t=Remember(model,origin);
    }
    if(flags&F_CONTROLLED){
        // Accept the elected simulator; reset its clock history on handover.
        auto*pool=ObjectPool();for(int i=0;pool&&i<pool->size;++i)if(pool->byteMap[i]>=0){auto*object=(CObject*)(pool->objects+(size_t)i*kObjectPoolElemSize);auto*d=Dummy(object);
            if(object->ModelIndex()==model&&d&&Key(model,d->GetPosition())==Key(model,origin)){auto source=ControlSource(object);if(source!=255&&source!=from)return;break;}}
        if(t->source!=from)t->history.Clear();
    }
    if(t->controlled&&!(flags&F_CONTROLLED))ReleaseMechanism(*t);
    if((flags&F_CONTROLLED)&&!t->history.Push({tick,{p.x,p.y,p.z},q},NowMs()))return;
    t->source=from;t->controlled=(flags&F_CONTROLLED)!=0;t->entityFlags=entityFlags;t->mechanismFlags=mechanismFlags;
    t->remote = true;
    t->stage = std::max(t->stage, stage);
    t->moved = (flags & F_MOVED) != 0;
    t->moving = (flags & F_MOVING) != 0;
    t->pos = {p.x, p.y, p.z};
    t->rot = q;
    t->speed = {s.x, s.y, s.z};
    t->turn = {tn.x, tn.y, tn.z};
    t->weapon = weapon;
    t->updatedMs = NowMs();
}

class WorldObjects : public Module {
public:
    const char* Name() const override { return "WorldObjects"; }
    void OnInstallHooks() override {
        Net().SetHandler(proto::G_WORLD_OBJ, &Handle);
        mem::HookFn(0x5A0B50,&ProcessCrossing,&g_originalCrossing,"CObject::ProcessTrainCrossingBehaviour shared authority");
    }
    void OnSessionEnd() override {for(auto&t:g_things)ReleaseMechanism(t);ClearObjects();}
    void OnGameShutdown()override{for(auto&t:g_things)ReleaseMechanism(t);ClearObjects();}
    void OnGameStarted() override {  // a new world: what was broken came back
        for (Thing& t : g_things) t.following = false;
    }
    void OnPeerResync(uint8_t) override {  // its world is new: what both games know goes again
        for (const Thing& t : g_things)
            if ((!t.controlled&&!t.moving)||(t.controlled&&t.source==Net().MyId()&&!t.remote)) Send(t, true);
    }
    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady())return;FollowMechanisms();
        if(NowMs()-g_scanMs<100)return;
        g_scanMs = NowMs();
        Scan();
        if ((g_sent || g_received) && NowMs() - g_statsMs >= 10000) {
            g_statsMs = NowMs();
            LOGI("World objects: %d messages sent, %d received, %zu known", g_sent, g_received, g_things.size());
            g_sent = g_received = 0;
        }
    }
};

WorldObjects g_worldObjects;
struct Reg {
    Reg() { core::RegisterModule(&g_worldObjects); }
} g_reg;
}  // namespace

}  // namespace coop
