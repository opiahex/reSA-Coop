#pragma once
// Views over the animation system: CAnimBlendAssociation (playing instance), CAnimBlendHierarchy
// (animation data, CAnimManager::ms_aAnimations) and CAnimBlock (one IFP file, ms_aAnimBlocks).
// An animation is identified across games by (IFP block index, index inside the block): both games
// register the same IFP files in the same order, while hierarchy addresses differ.
#include <cstdint>

#include "shared/game/Game.h"
#include "shared/lib/sync/AnimationState.h"

namespace game::anim {

constexpr uintptr_t kHierarchies = 0xB4EA40;  // CAnimManager::ms_aAnimations, 0x18 bytes each
constexpr uintptr_t kBlocks = 0xB5D4A0;       // CAnimManager::ms_aAnimBlocks, 0x20 bytes each
constexpr int kIfpBase = 25575;               // streaming id of the first IFP resource
constexpr uint16_t F_LOOPED = 0x2, F_AUTO_REMOVE = 0x4, F_PARTIAL = 0x10, F_BLOCK_REFERENCED = 0x4000, F_INDESTRUCTIBLE = 0x8000;

inline void* First(void* clump) { return clump ? Call<0x4D15E0, void*>(clump) : nullptr; }  // RpAnimBlendClumpGetFirstAssociation
inline void* Next(void* a) { return Call<0x4D6AB0, void*>(a); }                              // RpAnimBlendGetNextAssociation
inline int16_t Group(void* a) { return Field<int16_t>(a, 0x0E); }
inline void* Hierarchy(void* a) { return Field<void*>(a, 0x14); }
inline float& BlendAmount(void* a) { return Field<float>(a, 0x18); }
inline float& BlendDelta(void* a) { return Field<float>(a, 0x1C); }
inline float CurrentTime(void* a) { return Field<float>(a, 0x20); }
inline float& Speed(void* a) { return Field<float>(a, 0x24); }
inline int16_t Id(void* a) { return Field<int16_t>(a, 0x2C); }
inline uint16_t& Flags(void* a) { return Field<uint16_t>(a, 0x2E); }
inline void SetCurrentTime(void* a, float t) { CallMethod<0x4CEA80>(a, t); }
// blends it out and deletes it once out (CAnimBlendAssociation::UpdateBlend removes only auto-remove ones)
inline void FadeOut(void* a) {
    BlendDelta(a) = -4.0f;
    Flags(a) |= F_AUTO_REMOVE;
}
inline float TotalTime(void* hierarchy) { return Field<float>(hierarchy, 0x10); }

inline uintptr_t Block(int i) { return kBlocks + (uintptr_t)i * 0x20; }
inline bool BlockLoaded(int i) { return Ref<uint8_t>(Block(i) + 0x10) != 0; }

// hierarchy -> (block, index); false for animations outside the IFP blocks. A cutscene's animations are in a
// temporary last block (CCutsceneMgr loads them with LoadAnimFile): the same index means another one elsewhere.
inline bool Locate(void* h, int& block, int& idx) {
    if (!h) return false;
    block = Field<int32_t>(h, 0x0C);
    if (block < 0 || block >= Ref<int32_t>(0xB4EA30)) return false;  // ms_numAnimBlocks
    intptr_t off = (intptr_t)h - (intptr_t)(kHierarchies + (uintptr_t)Ref<int32_t>(Block(block) + 0x14) * 0x18);
    idx = (int)(off / 0x18);
    return off >= 0 && off % 0x18 == 0 && idx < Ref<int32_t>(Block(block) + 0x18);
}

// (block, index) -> hierarchy. The IFP is requested (kept in memory) when it is not loaded yet: nullptr then.
inline void* Resolve(int block, int idx) {
    if (block < 0 || block >= Ref<int32_t>(0xB4EA30) || idx < 0) return nullptr;
    if (!BlockLoaded(block)) {
        RequestModel(kIfpBase + block, 2);
        return nullptr;
    }
    if (idx >= Ref<int32_t>(Block(block) + 0x18)) return nullptr;
    return (void*)(kHierarchies + (uintptr_t)(Ref<int32_t>(Block(block) + 0x14) + idx) * 0x18);
}

// CAnimManager::BlendAnimation(clump, hierarchy, flags, delta). The association references the IFP
// itself, so a copied BLOCK_REFERENCED flag would release that reference twice; an indestructible
// copy would never be freed after blending out.
inline void* Blend(void* clump, void* h, uint16_t flags, float delta) {
    return Call<0x4D4410, void*>(clump, h, (int)(flags & ~(F_BLOCK_REFERENCED | F_INDESTRUCTIBLE)), delta);
}

inline coop::AnimationState Capture(void* a) {
    coop::AnimationState s;
    int block, index;
    if (!a || !Locate(Hierarchy(a), block, index)) return s;
    s.block = (uint16_t)block; s.index = (uint16_t)index; s.flags = Flags(a);
    s.group = Group(a); s.id = Id(a); s.time = CurrentTime(a); s.speed = Speed(a);
    return s;
}

inline void* Find(void* clump, const coop::AnimationState& s) {
    if (!s.Present()) return nullptr;
    for (void* a = First(clump); a; a = Next(a)) {
        int block, index;
        if (Locate(Hierarchy(a), block, index) && block == s.block && index == s.index) return a;
    }
    return nullptr;
}

inline void* Replay(void* clump, const coop::AnimationState& s, float age) {
    if (!clump || !s.Present()) return nullptr;
    void* a = Find(clump, s);
    bool fresh = a == nullptr;
    if (!a) {
        void* hierarchy = Resolve(s.block, s.index);
        if (!hierarchy || !(a = Blend(clump, hierarchy, s.flags, 8.0f))) return nullptr;
        // Preserve the locomotion identifiers: SetMoveAnim searches by animation id.
        Field<int16_t>(a, 0x0E) = s.group;
        Field<int16_t>(a, 0x2C) = s.id;
    }
    Speed(a) = s.speed;
    Flags(a) = (Flags(a) & ~1u) | (s.flags & 1u);
    BlendDelta(a) = 8.0f;
    float total = TotalTime(Hierarchy(a)), t = s.At(age, total);
    float error = std::fabs(CurrentTime(a) - t);
    if ((s.flags & F_LOOPED) && total > 0) error = std::min(error, std::fabs(total - error));
    if (fresh || error > 0.18f) SetCurrentTime(a, t);
    return a;
}

// TASK_PLAY_ANIM / sequences use CTaskSimpleRunNamedAnim (401), derived from
// CTaskSimpleAnim: its association pointer is at +8. Only accept a live clump member.
inline void* ScriptAssociation(CPed* ped) {
    void* intelligence = ped->Intelligence();
    void* task = intelligence ? CallMethod<0x681740, void*>((uint8_t*)intelligence + 4, 401) : nullptr;
    if (!task) return nullptr;
    void* wanted = Field<void*>(task, 8);
    for (void* a = First(ped->RwObject()); a; a = Next(a)) if (a == wanted) return a;
    return nullptr;
}

inline void* FindTask(CPed* ped, int type) {
    void* intelligence = ped ? ped->Intelligence() : nullptr;
    return intelligence ? CallMethod<0x681740, void*>((uint8_t*)intelligence + 4, type) : nullptr;
}

// Native physical responses are not produced by a replayed pad. Mirror their real
// association (fall, get-up, hit, dying, dragged from a seat), including held last frames.
inline void* ActionAssociation(CPed* ped) {
    if (void* scripted = ScriptAssociation(ped)) return scripted;
    if(void*task=FindTask(ped,1027)){void*wanted=Field<void*>(task,0x18);for(void*a=First(ped->RwObject());a;a=Next(a))if(a==wanted)return a;}

    constexpr int responseTasks[] = {400,402,403,404,405,406,407,408,409,410,411,412,413,414,416,418,419,420,421,422,423,424,425,426,427,428,429,430,205, 206, 207, 208, 209, 210, 211, 212, 214, 215, 217, 218, 228, 229, 240, 241, 250,
                                     254, 256, 267, 268, 277,1202,1203,1204,1209,1215, 307, 807, 817, 818, 820, 821, 823, 824, 825, 826,1304,1305,1600};
    bool reacting = ped->AttachedTo()!=nullptr;
    for (int type : responseTasks) if (void*task=FindTask(ped,type)) {
        reacting=true;
        // Simple native animation tasks keep their actual association at +8.
        // Validate membership before using it: complex tasks have a subtask there.
        // A partial hit/get-up must win over the stronger underlying idle/walk.
        void*wanted=Field<void*>(task,8);
        for(void*a=First(ped->RwObject());a;a=Next(a))if(a==wanted)return a;
    }
    if (!reacting) {
        // A pad cannot reproduce the owner's melee target/command selection:
        // without that target native Fight picks basic jabs even with the
        // learned style set. Replay the real attack/stance association, using
        // the existing timed action stream (no task pointers or hit callbacks).
        if(void*fight=FindTask(ped,1016))for(int offset:{0x1C,0x20}){
            void*wanted=Field<void*>(fight,offset);
            for(void*a=First(ped->RwObject());a;a=Next(a))
                if(a==wanted&&BlendDelta(a)>=0&&BlendAmount(a)>.01f)return a;
        }
        return nullptr;
    }
    void* best = nullptr;
    for (void* a = First(ped->RwObject()); a; a = Next(a)) {
        if ((Flags(a) & F_INDESTRUCTIBLE) || BlendAmount(a) < 0.05f || BlendDelta(a) < 0) continue;
        if (!best || BlendAmount(a) > BlendAmount(best)) best = a;
    }
    return best;
}

inline void RestoreMovement(CPed* ped) {
    if (!ped || !ped->RwObject() || ped->InVehicle() || ped->Health() <= 0) return;
    // A hierarchy-blended action faded out the previous idle/walk. The native
    // SetMoveAnim cache otherwise assumes that now-missing association still exists.
    Field<int32_t>(ped, 0x538) = -1;  // m_nSwimmingMoveState (previous move state)
    CallMethod<0x5E4A00>(ped);
    if(!First(ped->RwObject()))Call<0x4D4610,void*>(ped->RwObject(),0,3,8.0f);  // default idle
}

}  // namespace game::anim
