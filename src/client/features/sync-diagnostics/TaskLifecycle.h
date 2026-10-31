// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// Event collector without native hooks or game-memory access.
#pragma once
#include <cstdint>
#include "common/lib/diagnostics/FlightRecorder.h"

namespace coop::task_lifecycle {
template<class T> struct Known {
    T value{};
    bool available=false;
    static constexpr Known Of(T value) noexcept { return {value,true}; }
};
enum class SubjectKind:uint8_t { Player=1,Npc=2 };
struct Subject {
    flight::Context context{}; // Actual subject authority/lifetime, never an unrelated trigger producer/TLS fingerprint.
    Known<SubjectKind> kind{};
    Known<uint32_t> nativePedRef{}; // Validated local pool reference; known0 differs from unavailable.
    Known<uint8_t> owner{};
    Known<uint32_t> ownerEpoch{}; // Actual provided epoch only; never synthesized from owner or local order.
    Known<uint32_t> motionEpoch{}; // Actual player motion lifetime; unavailable for NPCs unless separately known.
    uint8_t localPeer=255;
};
enum class Operation:uint8_t {
    Unknown=0,IntelligenceCycle,EventResponse,AssignPrimary,AssignSecondary,
    ParentControl,NextSubtask,Abort,Completion,Replacement,Destruction,Flush,
    EventCycle,FlushImmediate,Relocation
};
struct Observation {
    Known<uint64_t> taskLocalId{}; // Adapter-validated local lifetime token, NEVER an opaque pointer/global ID.
    Known<int32_t> taskType{};
    Known<int32_t> taskSlot{};
};
struct Cause {
    Known<int32_t> why{}; // Actual known reason only; operation/result/null event do not fill WHY.
    Known<int32_t> eventType{};
    Known<int32_t> abortPriority{};
    Known<bool> eventPresent{};
    Known<uint8_t> callerImageKind{};
    Known<uint32_t> callerRva{};
};
enum class ParentRelation:uint8_t { SameSubjectOnly,ExplicitCurrent };
// Ordinary scopes must already have been admitted by their adapter. This core
// does not independently sample enter/leave or read any native memory.
enum class Capture:uint8_t { SelectedDiscrete,OrdinaryAdmitted };
enum class Stage:uint16_t {
    Before=840,After,Cause,AbortAttempt,AbortResult,Completion,Replacement,
    Destruction,ContextGap,CounterSnapshot
};
// Event.reason is collector structure/status, never a substitute for native WHY.
enum class Part:uint16_t {
    Observation=1,Cause=2,NativeReturn=3,ReplacementBefore=4,ReplacementAfter=5,
    UnobservedAfter=6,ExplicitCrossSubject=7,ContextGap=8
};
struct Span {
    uint64_t value=0;
    uint32_t recordingToken=0;
    bool available=false;
};
struct Counters {
    uint32_t opened=0,closed=0,disabled=0,overflow=0,suppressed=0,reentrant=0;
    uint32_t recordFalse=0,tokenChanged=0,unpaired=0,unobservedAfter=0,misnested=0;
    uint32_t invalidCall=0,wrongThread=0,crossSubjectParentOmitted=0;
};
// Fixed cumulative32bit counters wrap; concurrent snapshots are not transactions.
Counters SnapshotCounters() noexcept;
Span CurrentSpan() noexcept;

// Same-thread strict-LIFO use. No copying/moving/transfer of a native call scope.
// Runtime misuse is counted, never allowed to control native gameplay.
class Scope final {
public:
    Scope(const Subject&subject,Operation operation,const Observation&before={},
          ParentRelation parent=ParentRelation::SameSubjectOnly,
          Capture capture=Capture::SelectedDiscrete,
          uint32_t expectedRecordingToken=0) noexcept;
    ~Scope() noexcept;
    Scope(const Scope&)=delete;
    Scope&operator=(const Scope&)=delete;
    Scope(Scope&&)=delete;
    Scope&operator=(Scope&&)=delete;

    // Eligibility for additional adapter observations now, not merely an open
    // RAII frame. Finish/destructor still unwind after recording changes.
    bool Active()const noexcept;
    Span Identity()const noexcept { return {span_,token_,Active()}; }
    // At most one Cause/attempt/result/completion/replacement/destruction per
    // scope. A repeated call is counted and rejected, never silently duplicated.
    bool RecordCause(const Cause&cause) noexcept;
    bool AbortAttempt(const Observation&observation,const Cause&cause={}) noexcept;
    bool AbortResult(const Observation&observation,Known<int64_t> nativeResult={}) noexcept;
    bool Complete(const Observation&observation,const Cause&cause={}) noexcept;
    bool Replace(const Observation&before,const Observation&after,const Cause&cause={}) noexcept;
    bool Destroy(const Observation&observation,const Cause&cause={}) noexcept;
    // Finish emits exactly one After attempt and unwinds this frame. True means
    // both Before/After admitted with the checked token/LIFO, not persistence or
    // admission of all intermediate facts. Destructor otherwise emits an
    // explicitly UnobservedAfter; no fabricated native result.
    bool Finish(const Observation&after,Known<int64_t> nativeResult={}) noexcept;
private:
    Subject subject_{};
    Operation operation_=Operation::Unknown;
    Capture capture_=Capture::SelectedDiscrete;
    uint64_t span_=0,parentSpan_=0;
    uint32_t token_=0,ownerThread_=0;
    uint16_t seen_=0;
    uint8_t slot_=0;
    bool active_=false,closed_=false,suppressed_=false,parentKnown_=false,explicitCross_=false;
    bool beforeRecorded_=false,tokenNoted_=false,unpairedNoted_=false;
    bool Check(uint16_t bit) noexcept;
    bool Emit(Stage stage,Part part,const Observation*observation=nullptr,
              const Cause*cause=nullptr,Known<int64_t> nativeResult={}) noexcept;
    bool Close(const Observation*after,Known<int64_t> nativeResult) noexcept;
};
}
