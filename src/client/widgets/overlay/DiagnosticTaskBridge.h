// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
#pragma once
#include <cstdint>
#include "features/sync-diagnostics/TaskLifecycleNative.h"
#include "features/sync-diagnostics/TaskTreeTrace.h"

namespace coop::ui::diagnostic {
struct NativeSnapshot;
namespace task_bridge {
struct Request {
    bool open=false,valid=true,peerSet=false,entitySet=false;
    uint64_t peer=0,entity=0;
};
// body is transient resolver input, never retained in the controller or output.
struct Prepared {
    task_lifecycle::native::Selection selection{};
    flight::Context context{};
    uintptr_t body=0;
    bool referenceKnown=false;
    uint32_t localWorld=0;
    uint8_t localPeer=255;
};
enum class State:uint8_t {
    None,Closed,Invalid,WrongThread,Disabled,NotReady,OwnershipConflict,
    TargetUnavailable,ReferenceUnavailable,RecordingChanged,SubjectChanged,
    AdmissionDisabled,AdmissionFiltered,AdmissionSampled,AdmissionBusy,AdmissionBudget,
    SelectionRejected,CaptureUnselected,CaptureNotReady,LayoutUnavailable,CaptureChanged,
    Copied,TracePartial,Stale,InvalidSnapshot
};
struct Services {
    bool(*thread)(uint32_t)noexcept=nullptr;
    uint32_t(*token)()noexcept=nullptr;
    task_lifecycle::native::Status(*status)()noexcept=nullptr;
    bool(*prepare)(const Request&,Prepared&)noexcept=nullptr; // metadata only
    flight::NativeAdmission(*admit)(const Prepared&)noexcept=nullptr;
    bool(*reference)(Prepared&)noexcept=nullptr; // guarded pool read, after admit
    bool(*select)(const task_lifecycle::native::Selection*,size_t)noexcept=nullptr;
    void(*clear)()noexcept=nullptr;
    task_lifecycle::native::TreeCaptureStatus(*capture)(size_t,task_lifecycle::tree::Snapshot&)noexcept=nullptr;
    task_lifecycle::tree_trace::Result(*record)(const task_lifecycle::tree::Snapshot&,uint32_t)noexcept=nullptr;
};
class Controller final {
public:
    void Update(const Request&,uint32_t owningThread,uint64_t now,const Services&)noexcept;
    void Append(NativeSnapshot&,uint64_t now,const Services&)const noexcept;
    State Status()const noexcept{return state_;}
private:
    void Release(const Services&)noexcept;
    Request request_{};
    task_lifecycle::native::Selection selection_{};
    task_lifecycle::tree::Snapshot snapshot_{};
    task_lifecycle::tree_trace::Result trace_{};
    uint64_t attemptedAt_=0,copiedAt_=0;
    uint32_t revision_=0,token_=0,localWorld_=0,owningThread_=0;
    State state_=State::None;
    bool hasRequest_=false,owned_=false,blocked_=false,attempted_=false,copied_=false;
};
// Both functions are called only by the existing owner-thread diagnostic paths.
void Update(const Request&,uint32_t owningThread,uint64_t now)noexcept;
void Append(NativeSnapshot&,uint64_t now)noexcept;
// Existing DiagnosticNative safe reader implements these two narrow services.
bool PrepareTarget(const Request&,Prepared&)noexcept;
bool ResolveReference(Prepared&)noexcept;
}
}
