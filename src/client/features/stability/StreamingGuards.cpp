#include "common/lib/log/Log.h"
#include "common/lib/profile/Profile.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {
namespace {
using namespace game;
using SaveFn=bool(__cdecl*)();SaveFn originalSave=nullptr;
bool __cdecl TimedSave(){profile::Scope timing(profile::SAVE_WORLD);return originalSave();}
using LoadModelsFn=void(__cdecl*)(bool);
LoadModelsFn originalLoadModels=nullptr;
void __cdecl TimedLoadModels(bool priority){profile::Scope timing(profile::STREAMING);originalLoadModels(priority);}
using ShadowPointFn = void(__fastcall*)(void*, void*, CVector*, int);
ShadowPointFn originalShadow = nullptr;
void __fastcall ShadowPoint(void* data, void* edx, CVector* out, int vertex) {
    // A streamed-out/corrupt shadow mesh can retain its triangle count but lose
    // its vertices. Explosions expose this path (reported crash 0040F64C).
    if (!data || !Field<void*>(data, 0x28) || vertex < 0 ||
        (uint32_t)vertex >= Field<uint32_t>(data, 0x24)) {
        if (out) *out = {};
        static int reports = 0;
        if (reports++ < 4) LOGW("Skipped missing shadow vertex %d", vertex);
        return;
    }
    originalShadow(data, edx, out, vertex);
}
using CreateFn = void*(__fastcall*)(void*, void*);
CreateFn originalCreate = nullptr;
using FrameFn=void(__cdecl*)(void*,void*);
FrameFn originalFrame=nullptr;
void __cdecl SkinnedFrame(void* frame,void* data) {
    // A native vehicle task can remove its final association during a carjack.
    // The velocity callback assumes at least one node (reported 004D1750).
    // Keep the previous pose for this frame until the task adds its next anim.
    if(!data || !Field<void*>(data,4))return;
    originalFrame(frame,data);
}
void* __fastcall VehicleInstance(void* model, void* edx) {
    // CAutomobile::Fix also recreates its clump. The model can have streamed out
    // while a mirror is in another area (reported crash 004C9691).
    if (!Field<void*>(model, 0x1C) || !Field<void*>(model, 0x5C)) {
        for (int id = 400; id <= 611; ++id) {
            if (Ref<void*>(addr::ModelInfoPtrs + id * 4) != model) continue;
            RequestModel(id, 2 | 0x10);
            LoadAllRequestedModels(false);
            if (!Field<void*>(model, 0x1C) || !Field<void*>(model, 0x5C)) {
                LOGW("Vehicle model %d has no render data; instance deferred", id);
                return nullptr;
            }
            LOGI("Vehicle model %d reloaded before clump creation", id);
            break;
        }
    }
    return originalCreate(model, edx);
}
class StreamingGuards : public Module {
public:
    const char* Name() const override { return "StreamingGuards"; }
    uint64_t frameStart=0;
    void OnPreProcess()override{frameStart=profile::Micros();}
    void OnPostProcess()override{if(frameStart)profile::Metrics()[profile::NATIVE_FRAME].Add(profile::Micros()-frameStart);}
    void OnInstallHooks() override {
        mem::HookFn(0x5D13E0,&TimedSave,&originalSave,"CGenericGameStorage::GenericSave (timing)");
        mem::HookFn(0x40EA10,&TimedLoadModels,&originalLoadModels,"CStreaming::LoadAllRequestedModels (timing)");
        mem::HookFn(0x40F640, &ShadowPoint, &originalShadow, "CCollisionData::GetShadTrianglePoint");
        mem::HookFn(0x4C9680, &VehicleInstance, &originalCreate, "CVehicleModelInfo::CreateInstance");
        mem::HookFn(0x4D2B90,&SkinnedFrame,&originalFrame,"FrameUpdateCallBackSkinned");
    }
} guards;
struct Register { Register() { core::RegisterModule(&guards); } } registration;
}
}
