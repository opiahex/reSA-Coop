// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
#pragma once
#include <cstddef>
#include <cstdint>
#include "features/sync-diagnostics/TaskLifecycle.h"
#include "features/sync-diagnostics/TaskTreeSnapshot.h"
namespace coop::task_lifecycle::native {
constexpr size_t kSelections=4;
// Existing six install bits retain their positions. The new observer is bit6.
constexpr uint32_t kLegacyInstalledMask=0x3f,kIntelligenceFlushInstalledBit=0x40;
constexpr uint32_t kTeleportInstalledBit=0x80;
enum class InstallState:uint8_t { NotAttempted,Ready,Unsupported,Conflict,RegistryUnavailable,FailedUnchanged,Indeterminate };
struct Selection {
    SubjectKind kind=SubjectKind::Player;
    uint8_t peer=255;              // Player peer, or actual NPC authority.
    uint16_t entity=0;             // NPC netId; ignored for Player.
    uint32_t nativePedRef=0;
    uint32_t incarnation=0,world=0,motionEpoch=0;
    uint64_t generation=0,expiresMs=0; // NPC generation only; maximum60s explicit lease.
};
struct Status {
    InstallState install=InstallState::NotAttempted;
    uint32_t installAttempted=0,installedMask=0,registryFailures=0;
    uint32_t selected=0,selectionRevision=0,expired=0,invalidSelection=0,wrongThread=0;
    uint32_t rejectedAdmission=0,unknownTask=0,unknownEvent=0,readFailed=0,afterUnavailable=0;
    uint32_t ordinaryThrottled=0;
};
// These are game-thread-only writes; a foreign caller is rejected. No implicit
// rebinding across respawn, world/authority changes or recording restart.
bool SetSelections(const Selection*selections,size_t count) noexcept;
void ClearSelections() noexcept;
Status SnapshotStatus() noexcept; // Cumulative wrapping counters, not a transaction.
InstallState InstallNativeAdapters() noexcept; // One attempt only, optional observer group.
enum class TreeCaptureStatus:uint8_t {Copied,Unselected,WrongThread,NotReady,LayoutUnavailable,SubjectChanged};
// Explicit synchronous game-thread observation only; no selection, hook or engine
// writes. Copied does not mean complete. Pool witnesses are read-time checks,
// not persistent task IDs, complete destructor coverage or same-address ABA proof.
TreeCaptureStatus CaptureSelectedTree(size_t index,tree::Snapshot&out,tree::Limits={}) noexcept;

class IntelligenceScope final {
public:
    explicit IntelligenceScope(void*intelligence,const void*caller=nullptr) noexcept;
    ~IntelligenceScope() noexcept;
    IntelligenceScope(const IntelligenceScope&)=delete;
    IntelligenceScope&operator=(const IntelligenceScope&)=delete;
private:
    alignas(Scope) unsigned char scopeStorage_[sizeof(Scope)]{};
    Scope*scope_=nullptr;
    Subject subject_{};
    void*body_=nullptr;
    void*intelligence_=nullptr;
    uint32_t recording_=0,revision_=0,motionEpoch_=0,localWorld_=0;
    Selection selection_{};
};
}
