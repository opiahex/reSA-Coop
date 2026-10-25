// What people carry: an object a ped holds (CTaskSimpleHoldEntity: the tray a fast food server lifts, a phone) or one
// attached to it (ATTACH_OBJECT_TO_CHAR: the parachute's canopy). A script or a task of the owner's game makes them,
// so the other game showed the person without the thing. Each game reports such objects of its player and of the peds
// it shares (Peds): the model, how the object hangs on the person, its own animation, size and visibility. The other
// game hangs the same object on its copy of that person, without collision. Let go (the canopy on landing), the object
// stays where the owner's is until that one is gone.
#include <algorithm>
#include "shared/api/native-interaction/NativeInteraction.h"
#include "shared/lib/sync/SnapshotBuffer.h"
#include <cmath>
#include "shared/lib/sync/IdAllocator.h"
#include "shared/api/mission-objects/SharedObject.h"
#include <map>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/vehicle/VehicleState.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Anim.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

constexpr uintptr_t kMissionCleanUp = 0xA90850;  // CTheScripts::MissionCleanUp: 75 x {uint8 type, int32 handle}
constexpr int TASK_SIMPLE_HOLD_ENTITY = 307, TASK_SIMPLE_PICKUP_ENTITY = 308, TASK_SIMPLE_PUTDOWN_ENTITY = 309;
constexpr uint8_t HOLD_ENTITY_UPDATE_TRANSLATION_ONLY = 0x10;
constexpr int kPedNodes = 19;
constexpr uint64_t kLooseKeepMs = 30000;  // a let-go object is followed this long at most
enum : uint8_t { OP_STATE = 1, OP_GONE = 2 };
enum : uint8_t { BY_PLAYER = 0, BY_PED = 1 };
enum : uint8_t { HANG_ATTACHED = 0, HANG_HELD = 1, HANG_LOOSE = 2 };

struct Prop {
    uint32_t time=0;CVector velocity;
    uint16_t model = 0;
    uint8_t by = BY_PLAYER;
    uint16_t holder = 0;  // BY_PED: the ped's net id
    uint8_t hang = HANG_ATTACHED;
    // ATTACHED: CPhysical::m_vecAttachOffset, m_vecAttachedEntityRotation, m_qAttachedEntityRotation (zero: the
    // angles count); HELD: the hold task's offset; LOOSE: the position and the rotation
    CVector offset, euler;
    Quat rot;
    uint8_t bone = 0, boneFlags = 0;  // HELD: ePedNode, eHoldEntityBoneFlags
    float turn = 0;                   // HELD: CTaskSimpleHoldEntity::m_fRotation
    int8_t area = 0;
    bool visible = true;
    float scale = 1.0f;
    int16_t animBlock = -1, animIdx = -1;  // its own animation (IFP block and index), -1: none
    uint16_t animFlags = 0;
    float animTime = 0;
    uint8_t animRev = 0;  // counts the owner's starts of an animation: the same one again is a restart
};

void Write(Writer& w, const Prop& p) {
    w.U32((uint32_t)NowMs());w.Vec({p.velocity.x,p.velocity.y,p.velocity.z});
    w.U16(p.model);
    w.U8(p.by);
    if(p.by==BY_PED)w.Entity(ENTITY_PED,p.holder);else w.U16(p.holder);
    w.U8(p.hang);
    w.Vec({p.offset.x, p.offset.y, p.offset.z});
    w.Vec({p.euler.x, p.euler.y, p.euler.z});
    w.F32(p.rot.x);  // raw: an attachment's quaternion may be all zero
    w.F32(p.rot.y);
    w.F32(p.rot.z);
    w.F32(p.rot.w);
    w.U8(p.bone);
    w.U8(p.boneFlags);
    w.F32(p.turn);
    w.I8(p.area);
    w.U8(p.visible);
    w.F32(p.scale);
    w.I16(p.animBlock);
    w.I16(p.animIdx);
    w.U16(p.animFlags);
    w.F32(p.animTime);
    w.U8(p.animRev);
}

void Read(Reader& r, Prop& p) {
    p.time=r.U32();auto v=r.Vec();p.velocity={v.x,v.y,v.z};
    p.model = r.U16();
    p.by = r.U8();
    p.holder = r.U16();
    p.hang = r.U8();
    Vec3 o = r.Vec(), e = r.Vec();
    p.offset = {o.x, o.y, o.z};
    p.euler = {e.x, e.y, e.z};
    p.rot.x = r.F32();
    p.rot.y = r.F32();
    p.rot.z = r.F32();
    p.rot.w = r.F32();
    p.bone = r.U8();
    p.boneFlags = r.U8();
    p.turn = r.F32();
    p.area = r.I8();
    p.visible = r.U8() != 0;
    p.scale = r.F32();
    p.animBlock = r.I16();
    p.animIdx = r.I16();
    p.animFlags = r.U16();
    p.animTime = r.F32();
    p.animRev = r.U8();
}

bool Alive(CObject* o, int h) { return o && IsObjectValid(o) && GetObjectRef(o) == h; }
bool Near(const CVector& a, const CVector& b, float d) { return (a - b).Length() <= d; }

bool Differs(const Prop& a, const Prop& b) {
    if (a.model != b.model || a.by != b.by || a.holder != b.holder || a.hang != b.hang || a.bone != b.bone ||
        a.boneFlags != b.boneFlags || a.area != b.area || a.visible != b.visible || a.animRev != b.animRev)
        return true;
    float turned = std::fabs(a.rot.x - b.rot.x) + std::fabs(a.rot.y - b.rot.y) + std::fabs(a.rot.z - b.rot.z) +
                   std::fabs(a.rot.w - b.rot.w);
    return std::fabs(a.scale - b.scale) > 0.01f || !Near(a.offset, b.offset, a.hang == HANG_LOOSE ? 0.02f : 0.001f) ||
           !Near(a.euler, b.euler, 0.001f) || turned > 0.002f || std::fabs(a.turn - b.turn) > 0.001f;
}

// ---------------------------------------------------------------- ours
struct Owned {
    bool pool=false;
    CObject* obj = nullptr;
    int handle = -1;
    Prop sent;
    float animTime = 0;    // the latest seen: a non-looped one going back started again
    uint64_t looseMs = 0;  // let go at
};
std::map<uint16_t, Owned> g_owned;
IdAllocator<65536> g_ids;
uint64_t g_scanMs = 0;
bool g_resend = false;  // a peer (re)started its game: everything again

// the host's mission objects go over as such (mission-objects), with their moves
bool MissionObject(int handle) {
    for (int i = 0; i < 75; ++i) {
        uintptr_t e = kMissionCleanUp + (uintptr_t)i * 8;
        if (Ref<uint8_t>(e) == 3 && Ref<int32_t>(e + 4) == handle) return true;  // MISSION_CLEANUP_ENTITY_TYPE_OBJECT
    }
    return false;
}

// what a ped holds: CTaskSimpleHoldEntity in its partial animation slot, or the pick up or put down that carries it
void* HoldTask(CPed* ped) {
    void* intel = ped->Intelligence();
    if (!intel) return nullptr;
    for (int type : {TASK_SIMPLE_HOLD_ENTITY, TASK_SIMPLE_PICKUP_ENTITY, TASK_SIMPLE_PUTDOWN_ENTITY})
        if (void* t = CallMethod<0x681740, void*>((uint8_t*)intel + 4, type)) return t;  // CTaskManager::FindActiveTaskByType
    return nullptr;
}

// how it looks: visibility, size and the strongest animation of its own (the canopy opening, steering, landing);
// false when that animation cannot be told (no RenderWare object while far from the camera)
bool ReadLook(CObject* o, Prop& p) {
    p.model = o->ModelIndex();
    p.area = o->AreaCode();
    p.visible = o->IsVisible();
    p.scale = Field<float>(o, 0x15C);  // CObject::m_fScale
    p.animBlock = p.animIdx = -1;
    auto* clump = (uint8_t*)o->RwObject();
    if (!clump) return false;
    if (clump[0] != 2 || !Call<0x4D6760, bool>(clump)) return true;  // RpClump, RpAnimBlendClumpIsInitialized
    void* best = nullptr;
    for (void* a = anim::First(clump); a; a = anim::Next(a)) {
        int block, idx;
        if (anim::BlendDelta(a) < 0.0f || !anim::Locate(anim::Hierarchy(a), block, idx)) continue;
        if (best && anim::BlendAmount(a) <= anim::BlendAmount(best)) continue;
        best = a;
        p.animBlock = (int16_t)block;
        p.animIdx = (int16_t)idx;
    }
    if (best) {
        p.animFlags = anim::Flags(best);
        p.animTime = anim::CurrentTime(best);
    }
    return true;
}

void SendState(uint16_t id, const Prop& p) {
    Writer w(proto::G_PROP);
    w.U8(OP_STATE);
    w.Entity(ENTITY_PROP,id,ENTITY_BIRTH|ENTITY_SNAPSHOT);
    Write(w, p);
    Net().Send(proto::CH_RELIABLE, w);
}

void SendGone(uint16_t id) {
    Writer w(proto::G_PROP);
    w.U8(OP_GONE);
    w.Entity(ENTITY_PROP,id,ENTITY_REMOVE);
    Net().Send(proto::CH_RELIABLE, w);
}

void Update(uint16_t id, Owned& ow, Prop p) {
    const Prop& s = ow.sent;
    bool other = p.animBlock != s.animBlock || p.animIdx != s.animIdx;
    bool again = !other && p.animBlock >= 0 && !(p.animFlags & anim::F_LOOPED) && p.animTime + 0.25f < ow.animTime;
    ow.animTime = p.animTime;
    p.animRev = (uint8_t)(s.animRev + (other || again ? 1 : 0));
    if (other || again)
        LOGD("Prop %04X animation %s/%d (flags %04X, %.2f s)", id, p.animBlock >= 0 ? (const char*)anim::Block(p.animBlock) : "-",
             p.animIdx, p.animFlags, p.animTime);
    if (!g_resend && !Differs(p, s)) return;
    SendState(id, p);
    ow.sent = p;
}

void Scan() {
    CPlayerPed* me = FindPlayerPed(0);
    uint8_t my = Net().MyId();
    // the people the other game has copies of: our player and our shared peds
    std::map<CPhysical*, std::pair<uint8_t, uint16_t>> holders;
    if (me) holders[me] = {BY_PLAYER, 0};
    if (CPoolHeader* pool = PedPool())
        for (int i = 0; i < pool->size; ++i) {
            if (pool->byteMap[i] < 0) continue;
            auto* ped = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
            NetPed* np = ped != me ? Peds::Instance().Find(ped) : nullptr;
            if (np && np->owner == my) holders[ped] = {BY_PED, np->netId};
        }

    std::vector<std::pair<CObject*, Prop>> found;
    for (auto& [ped, who] : holders) {
        void* t = HoldTask((CPed*)ped);
        auto* e = t ? Field<CEntity*>(t, 0x08) : nullptr;  // m_pEntityToHold
        if (!e || !e->IsObject()) continue;
        Prop p;
        p.by = who.first;
        p.holder = who.second;
        p.hang = HANG_HELD;
        p.offset = Field<CVector>(t, 0x0C);     // m_vecPosition
        p.bone = Field<uint8_t>(t, 0x18);       // m_nBoneFrameId
        p.boneFlags = Field<uint8_t>(t, 0x19);  // m_bBoneFlags
        p.turn = Field<float>(t, 0x1C);         // m_fRotation
        found.push_back({(CObject*)e, p});
    }
    if (CPoolHeader* pool = ObjectPool())
        for (int i = 0; i < pool->size; ++i) {
            if (pool->byteMap[i] < 0) continue;
            auto* o = (CObject*)(pool->objects + (size_t)i * kObjectPoolElemSize);
            auto h = holders.find(o->AttachedTo());
            if (h == holders.end()) {
                // A police roadblock's barriers and burnt-out cars (CRoadBlocks::CreateRoadBlockBetween2Points: temporary
                // objects of two models) are made by the wanted player's game only: the other one had the police cars
                // and the SWAT team standing behind nothing. Seen there where they stand here, knocked over here too.
                int model = o->ModelIndex();
                bool roadblock = Field<uint8_t>(o, 0x13C) == 3 &&  // OBJECT_TEMPORARY (our copies are 6)
                                 (model == Ref<int16_t>(0x8CD57C) || model == Ref<int16_t>(0x8CD580));  // MI_DYN_ROADBARRIER_6, MI_LA_FUCKCAR1
                int8_t area = o->AreaCode() == 13 ? 0 : o->AreaCode();  // 13: seen in every area (the roadblock's are)
                bool poolBall=IsNativePoolBall(GetObjectRef(o))&&((model>=2995&&model<=3003)||(model>=3100&&model<=3106))&&
                    (o->GetPosition()-me->GetPosition()).Length()<8&&Field<uint8_t>(o,0x13C)!=6;
                if ((!roadblock&&!poolBall) || RemoteDistance(o->GetPosition(), area) > 150.0f) continue;
                Prop p;
                p.by = BY_PLAYER;
                p.hang = HANG_LOOSE;
                p.offset = o->GetPosition();
                if (CMatrix* m = o->Matrix()) p.rot = Quat::FromMatrix(*m);
                found.push_back({o, p});
                continue;
            }
            Prop p;
            p.by = h->second.first;
            p.holder = h->second.second;
            p.hang = HANG_ATTACHED;
            p.offset = Field<CVector>(o, 0x100);  // CPhysical::m_vecAttachOffset
            p.euler = Field<CVector>(o, 0x10C);   // m_vecAttachedEntityRotation
            p.rot = Field<Quat>(o, 0x118);        // m_qAttachedEntityRotation
            found.push_back({o, p});
        }

    uint64_t now = NowMs();
    std::vector<uint16_t> gone;
    for (auto& [id, ow] : g_owned) {
        if(ow.pool&&!OwnsNativePoolTable()){gone.push_back(id);continue;}
        if(IsSharedMissionObject(ow.obj)){gone.push_back(id);continue;}
        if (!Alive(ow.obj, ow.handle)) {
            gone.push_back(id);
            continue;
        }
        auto f = std::find_if(found.begin(), found.end(), [&](const auto& x) { return x.first == ow.obj; });
        Prop p;
        if(ow.pool&&f==found.end()){gone.push_back(id);continue;}
        if (f != found.end()) {
            p = f->second;
            found.erase(f);
            ow.looseMs = 0;
        } else {  // let go: mission objects return to their world sync
            if(MissionObject(ow.handle)){gone.push_back(id);continue;}
            if (!ow.looseMs) ow.looseMs = now;
            if (now - ow.looseMs > kLooseKeepMs) {
                gone.push_back(id);
                continue;
            }
            p.by = ow.sent.by;
            p.holder = ow.sent.holder;
            p.hang = HANG_LOOSE;
            p.offset = ow.obj->GetPosition();
            if (CMatrix* m = ow.obj->Matrix()) p.rot = Quat::FromMatrix(*m);
        }
        if(p.hang==HANG_LOOSE)p.velocity=ow.obj->MoveSpeed()*50.f;
        if (!ReadLook(ow.obj, p)) {  // the animation as it was
            p.animBlock = ow.sent.animBlock;
            p.animIdx = ow.sent.animIdx;
            p.animFlags = ow.sent.animFlags;
            p.animTime = ow.animTime;
        }
        Update(id, ow, p);
    }
    for (uint16_t id : gone) {
        SendGone(id);
        g_ids.Retire(id,NowMs());g_owned.erase(id);
        LOGI("Prop %04X gone", id);
    }
    for (auto& [o, p] : found) {
        if(IsSharedMissionObject(o))continue;
        int h = GetObjectRef(o);
        if (h < 0) continue;
        uint16_t id=g_ids.Take(0,NowMs(),[](uint16_t v){return g_owned.count(v)!=0;});if(!id)continue;
        Net().BeginEntity(ENTITY_PROP,id);Owned& ow = g_owned[id];
        ow.obj = o;
        ow.handle = h;
        ReadLook(o, p);if(p.hang==HANG_LOOSE)p.velocity=o->MoveSpeed()*50.f;
        ow.pool=OwnsNativePoolTable()&&p.hang==HANG_LOOSE&&((p.model>=2995&&p.model<=3003)||(p.model>=3100&&p.model<=3106));
        p.animRev = 1;
        ow.sent = p;
        ow.animTime = p.animTime;
        SendState(id, p);
        LOGI("Prop %04X: model %d %s by %s %04X", id, p.model,
             p.hang == HANG_HELD ? "held" : p.hang == HANG_ATTACHED ? "attached" : "standing in the street",
             p.by == BY_PLAYER ? "our player" : "ped", p.holder);
    }
    g_resend = false;
}

// ---------------------------------------------------------------- the other game's
struct Copy {
    uint64_t holderGeneration=0;
    uint8_t from = 0;
    Prop p;
    uint64_t recvMs = 0;
    SnapshotBuffer<Prop> history;
    CObject* obj = nullptr;
    int handle = -1;
    int8_t shown = -1;              // the visibility set on it
    uint8_t animRev = 0;            // of the animation started on it
    void* animApplied = nullptr;    // that animation's hierarchy
    void* animClump = nullptr;      // the RenderWare clump it was started on (a far object has none, rebuilt: anew)
    bool placed = false;            // LOOSE: put where p says
};
std::map<uint32_t, Copy> g_copies;  // (from << 16) | id

uint32_t Key(uint8_t from, uint16_t id) { return ((uint32_t)from << 16) | id; }

// our copy of the person who has it, nullptr while there is none
CPed* HolderOf(const Copy& c) {
    if (c.p.by == BY_PLAYER) {
        RemotePlayer* rp = Players::Instance().Get(c.from);
        return rp ? rp->ped : nullptr;
    }
    if(!Net().EntityContextValid({EntityRef{ENTITY_PED,255,c.p.holder,c.holderGeneration,0}}))return nullptr;
    int h = Peds::Instance().HandleOf(c.p.holder);
    CPed* ped = h >= 0 ? GetPed(h) : nullptr;
    NetPed* np = ped ? Peds::Instance().Find(ped) : nullptr;
    return np && np->owner == c.from ? ped : nullptr;
}

void Detach(Copy& c) { Cmd(0x0682).I(c.handle).F(0.0f).F(0.0f).F(0.0f).I(0).Run(); }  // DETACH_OBJECT

void Delete(Copy& c) {
    if (Alive(c.obj, c.handle)) {
        if (c.obj->AttachedTo()) Detach(c);
        Cmd(0x0108).I(c.handle).Run();  // DELETE_OBJECT
    }
    c.obj = nullptr;
    c.handle = -1;
}

bool Create(Copy& c, CPed* holder) {
    c.obj = nullptr;
    if (!IsObjectModel(c.p.model)) return false;
    if (!IsModelLoaded(c.p.model)) {
        RequestModel(c.p.model);
        return false;
    }
    if (!HasRoom(ObjectPool(), kKeepObjects, "object")) return false;
    CVector at = holder ? holder->GetPosition() : c.p.offset;
    int h = -1;
    Cmd(0x029B).I(c.p.model).F(at.x).F(at.y).F(at.z).Out(&h).Run();  // CREATE_OBJECT_NO_OFFSET
    CObject* o = h >= 0 ? GetObject(h) : nullptr;
    if (!o) return false;
    // OBJECT_MISSION2 (an external script's object): a mission object to the game, but CPools::SaveObjectPool keeps only
    // OBJECT_MISSION ones, and the host's saves and world snapshots kept the copies floating where they were
    Field<uint8_t>(o, 0x13C) = 6;
    Cmd(0x0382).I(h).I(0).Run();  // SET_OBJECT_COLLISION: only to be seen
    Cmd(0x0550).I(h).I(1).Run();  // FREEZE_OBJECT_POSITION: it goes where this module or its attachment puts it
    MarkModelNotNeeded(c.p.model);
    c.obj = o;
    c.handle = h;
    c.shown = -1;
    c.animRev = 0;
    c.animApplied = nullptr;
    c.animClump = nullptr;
    c.placed = false;
    LOGI("Player %d's prop (model %d) copied", c.from, c.p.model);
    return true;
}

// as CTaskSimpleHoldEntity::SetPedPosition: on the bone (its orientation too), or upright and turned with the person
void InHand(CObject* o, CPed* ped, const Prop& p) {
    auto* clump = (uint8_t*)ped->RwObject();
    CMatrix *m = o->Matrix(), *pm = ped->Matrix();
    if (!clump || clump[0] != 2 || !m || !pm || p.bone >= kPedNodes) return;
    void* node = Field<void*>(ped, 0x488 + p.bone * 4);  // CPed::m_apBones: AnimBlendFrameData
    void* hier = Call<0x734A40, void*>(clump);           // GetAnimHierarchyFromSkinClump
    if (!node || !hier) return;
    int i = Call<0x7C51A0, int>(hier, Field<int32_t>(node, 0x14));  // RpHAnimIDGetIndex(AnimBlendFrameData::BoneTag)
    auto* bones = Call<0x7C5120, uint8_t*>(hier);                    // RpHAnimHierarchyGetMatrixArray: world matrices
    if (i < 0 || !bones) return;
    const CMatrix& b = *(const CMatrix*)(bones + (size_t)i * 0x40);
    WorldRemove(o);
    if (p.boneFlags & HOLD_ENTITY_UPDATE_TRANSLATION_ONLY) {
        m->right = b.right;
        m->forward = b.forward;
        m->up = b.up;
        m->pos = b.pos + b.right * p.offset.x + b.forward * p.offset.y + b.up * p.offset.z;
    } else {
        float c = std::cos(p.turn), s = std::sin(p.turn);
        auto turn = [&](const CVector& v) { return CVector{c * v.x - s * v.y, s * v.x + c * v.y, v.z}; };
        m->right = turn(pm->right);
        m->forward = turn(pm->forward);
        m->up = turn(pm->up);
        m->pos = b.pos + pm->right * p.offset.x + pm->forward * p.offset.y + pm->up * p.offset.z;
    }
    CallMethod<0x446F90>(o);  // CEntity::UpdateRW
    CallMethod<0x532B00>(o);  // CEntity::UpdateRwFrame
    Call<0x563220>(o);        // CWorld::Add
}

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

// its own animation, as PLAY_OBJECT_ANIM starts one (0x46E9FC), once the IFP is in memory
void Animate(Copy& c) {
    auto* clump = (uint8_t*)c.obj->RwObject();
    if (!clump) return;  // not built while far from the camera
    if (clump != c.animClump) {  // a new one: its animation again
        c.animClump = clump;
        c.animApplied = nullptr;
        c.animRev = 0;
    }
    if (c.animRev == c.p.animRev) return;
    if (clump[0] != 2) {  // an atomic: nothing in it moves
        c.animRev = c.p.animRev;
        return;
    }
    void* h = nullptr;
    if (c.p.animBlock >= 0 && !(h = anim::Resolve(c.p.animBlock, c.p.animIdx))) return;
    if (!Call<0x4D6760, bool>(clump)) Call<0x4D6720>(clump);  // RpAnimBlendClumpIsInitialized, RpAnimBlendClumpInit
    if (c.animApplied)
        for (void* a = anim::First(clump); a; a = anim::Next(a))
            if (anim::Hierarchy(a) == c.animApplied && anim::BlendDelta(a) >= 0.0f) anim::FadeOut(a);
    if (h)
        if (void* a = anim::Blend(clump, h, c.p.animFlags, 8.0f)) {
            float t = c.p.animTime + (float)(NowMs() - c.recvMs) / 1000.0f, total = anim::TotalTime(h);
            anim::SetCurrentTime(a, (c.p.animFlags & anim::F_LOOPED) && total > 0 ? std::fmod(t, total) : std::min(t, total));
        }
    c.animApplied = h;
    c.animRev = c.p.animRev;
    LOGD("Player %d's prop animation %s/%d", c.from, c.p.animBlock >= 0 ? (const char*)anim::Block(c.p.animBlock) : "-", c.p.animIdx);
}

void Apply(Copy& c) {
    CPed* holder = c.p.hang == HANG_LOOSE ? nullptr : HolderOf(c);
    if (c.p.hang != HANG_LOOSE && !holder) {  // its person is not here (out of range, not made yet): nor is the thing
        Delete(c);
        return;
    }
    if (!Alive(c.obj, c.handle) && !Create(c, holder)) return;
    CObject* o = c.obj;
    o->AreaCode() = holder ? holder->AreaCode() : c.p.area;
    if (c.p.hang == HANG_ATTACHED) {
        if (o->AttachedTo() != holder) {
            if (o->AttachedTo()) Detach(c);
            Quat none{0, 0, 0, 0};
            CallMethod<0x54D690>(o, (CPhysical*)holder, &c.p.offset, &none);  // CPhysical::AttachEntityToEntity
        }
        // the owner's hang as it is: PositionAttachedEntity (CWorld::ProcessAttachedEntities) takes the quaternion
        // when it has an imaginary part, the angles otherwise
        Field<CVector>(o, 0x100) = c.p.offset;
        Field<CVector>(o, 0x10C) = c.p.euler;
        Field<Quat>(o, 0x118) = c.p.rot;
    } else {
        if (o->AttachedTo()) Detach(c);
        if (c.p.hang == HANG_HELD) {
            InHand(o, holder, c.p);
        } else if (!c.placed || c.history.Size()>1) {
            auto pose=c.history.At(NowMs(),[](const Prop&a,const Prop&b,float t){auto p=b;p.offset=a.offset+(b.offset-a.offset)*t;p.rot=Quat::Slerp(a.rot,b.rot,t);return p;},[](Prop&p,float age,bool stale){if(!stale)p.offset=p.offset+p.velocity*age;});
            Place(o, pose.offset, pose.rot);
            c.placed = true;
        }
    }
    bool show = c.p.visible && (!holder || holder->IsVisible());  // a held one shows with its holder, as the task does
    if (c.shown != (int8_t)show) {
        Cmd(0x0750).I(c.handle).I(show ? 1 : 0).Run();  // SET_OBJECT_VISIBLE
        c.shown = (int8_t)show;
    }
    Field<float>(o, 0x15C) = c.p.scale;  // CObject::m_fScale (CObject::PreRender scales it)
    Animate(c);
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    uint16_t id = r.U16();
    // not in the game yet: the owner sends everything again once we are (resync)
    if (!r.Ok() || !core::IsPlayerReady()) return;
    uint32_t k = Key(from, id);
    if (op == OP_GONE) {
        if (auto it = g_copies.find(k); it != g_copies.end()) {
            Delete(it->second);
            g_copies.erase(it);
        }
        return;
    }
    Prop p;
    Read(r, p);
    if (!std::isfinite(p.velocity.x)||!std::isfinite(p.velocity.y)||!std::isfinite(p.velocity.z)||p.velocity.Length()>200)return;
    if (!r.Ok() || op != OP_STATE || p.by > BY_PED || p.hang > HANG_LOOSE || p.bone >= kPedNodes || !std::isfinite(p.scale) || p.scale <= 0 || p.scale > 100) return;
    Copy& c = g_copies[k];
    bool remake = c.obj && c.p.model != p.model;
    if(remake||c.p.hang!=p.hang)c.history.Clear();
    c.history.Push(p,NowMs());c.from = from;
    c.p = p;c.holderGeneration=p.by==BY_PED?Net().EntityGeneration(ENTITY_PED,p.holder):0;
    c.recvMs = NowMs();
    c.placed = false;
    if (remake) Delete(c);
}

void DeleteCopies(int from) {
    for (auto it = g_copies.begin(); it != g_copies.end();) {
        if (from >= 0 && it->second.from != from) {
            ++it;
            continue;
        }
        Delete(it->second);
        it = g_copies.erase(it);
    }
}

class Props : public Module {
public:
    const char* Name() const override { return "Props"; }
    void OnEntityReplaced(uint8_t kind,uint8_t scope,uint16_t id)override{if(kind!=ENTITY_PROP)return;auto key=(uint32_t(scope)<<16)|id;if(auto i=g_copies.find(key);i!=g_copies.end()){Delete(i->second);g_copies.erase(i);}}

    void OnInstallHooks() override {
        Net().SetHandler(proto::G_PROP, [](uint8_t from, Reader& r) { Handle(from, r); });
    }

    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        if (NowMs() - g_scanMs >= 100) {
            g_scanMs = NowMs();
            Scan();
        }
        for (auto& [k, c] : g_copies) Apply(c);
    }

    void OnPeerResync(uint8_t id) override {  // its game started over: its props of before are gone there
        g_resend = true;
        DeleteCopies(id);
    }
    void OnPeerLeave(uint8_t id) override { DeleteCopies(id); }
    void OnSessionEnd() override {
        DeleteCopies(-1);
        g_owned.clear();
    }
    void OnGameStarted() override {  // a new world: the objects of the old one are gone
        g_copies.clear();
        g_owned.clear();
    }
    void OnGameShutdown() override { OnGameStarted(); }  // nothing to touch while the world loads

    static Props& Instance() {
        static Props m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Props::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
