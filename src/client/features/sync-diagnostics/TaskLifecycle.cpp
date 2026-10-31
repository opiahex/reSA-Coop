// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
#include "features/sync-diagnostics/TaskLifecycle.h"
#include <windows.h>
#include <array>
#include <atomic>

namespace coop::task_lifecycle {
namespace {
constexpr uint8_t kDepth=16;
#define TASK_COUNTERS(X) X(opened) X(closed) X(disabled) X(overflow) X(suppressed) X(reentrant) \
 X(recordFalse) X(tokenChanged) X(unpaired) X(unobservedAfter) X(misnested) \
 X(invalidCall) X(wrongThread) X(crossSubjectParentOmitted)
struct AtomicCounters {
#define FIELD(name) std::atomic<uint32_t> name{0};
    TASK_COUNTERS(FIELD)
#undef FIELD
} counters;
static_assert(std::atomic<uint32_t>::is_always_lock_free);
struct Frame {Subject subject{};uint64_t span=0;uint32_t token=0;bool retired=false;};
struct ThreadState {
    std::array<Frame,kDepth> frames{};
    uint32_t suppressed=0;
    uint8_t depth=0;
    bool emitting=false,poisoned=false;
};
thread_local ThreadState threadState{};
void Count(std::atomic<uint32_t>&counter) noexcept {counter.fetch_add(1,std::memory_order_relaxed);}
bool SameSubject(const Subject&a,const Subject&b) noexcept {
    return a.kind.available&&b.kind.available&&
        (a.kind.value==SubjectKind::Player||a.kind.value==SubjectKind::Npc)&&a.kind.value==b.kind.value&&
        a.nativePedRef.available&&b.nativePedRef.available&&a.nativePedRef.value==b.nativePedRef.value&&
        a.localPeer==b.localPeer&&a.context.source==b.context.source&&
        a.context.incarnation==b.context.incarnation&&a.context.world==b.context.world&&
        a.context.entity==b.context.entity&&a.context.generation==b.context.generation&&
        a.owner.available==b.owner.available&&(!a.owner.available||a.owner.value==b.owner.value)&&
        a.ownerEpoch.available==b.ownerEpoch.available&&(!a.ownerEpoch.available||a.ownerEpoch.value==b.ownerEpoch.value)&&
        a.motionEpoch.available==b.motionEpoch.available&&(!a.motionEpoch.available||a.motionEpoch.value==b.motionEpoch.value);
}
bool AnyCause(const Cause&c) noexcept {
    return c.why.available||c.eventType.available||c.abortPriority.available||
        c.eventPresent.available||c.callerImageKind.available||c.callerRva.available;
}
void Prune(ThreadState&s) noexcept {
    while(s.depth&&s.frames[s.depth-1].retired){s.frames[s.depth-1]={};--s.depth;}
    if(!s.depth)s.poisoned=false;
}
constexpr uint16_t kCause=1,kAbortAttempt=2,kAbortResult=4,kCompletion=8,kReplacement=16,kDestruction=32;
}

Counters SnapshotCounters() noexcept {
    Counters result{};
#define LOAD(name) result.name=counters.name.load(std::memory_order_relaxed);
    TASK_COUNTERS(LOAD)
#undef LOAD
    return result;
}
Span CurrentSpan() noexcept {
    const auto token=flight::ActiveRecordingToken();
    if(!token)return {};
    auto&s=threadState;Prune(s);
    if(s.emitting||s.poisoned||s.suppressed||!s.depth)return {};
    const auto&frame=s.frames[s.depth-1];
    if(frame.token!=token)return {};
    return {frame.span,frame.token,true};
}

Scope::Scope(const Subject&subject,Operation operation,const Observation&before,
             ParentRelation parent,Capture capture,uint32_t expectedRecordingToken) noexcept
    :subject_(subject),operation_(operation),capture_(capture) {
    token_=flight::ActiveRecordingToken();
    if(expectedRecordingToken&&expectedRecordingToken!=token_){Count(counters.tokenChanged);return;}
    if(!token_){Count(counters.disabled);return;}
    ownerThread_=GetCurrentThreadId();
    auto&s=threadState;
    if(s.emitting){Count(counters.reentrant);return;}
    Prune(s);
    if(unsigned(operation)>unsigned(Operation::Relocation)||
       (capture!=Capture::SelectedDiscrete&&capture!=Capture::OrdinaryAdmitted)||
       (parent!=ParentRelation::SameSubjectOnly&&parent!=ParentRelation::ExplicitCurrent)){
        Count(counters.invalidCall);return;
    }
    if(s.suppressed||s.poisoned){++s.suppressed;suppressed_=true;Count(counters.suppressed);return;}
    span_=flight::NextDiagnosticSpan();
    if(!span_){Count(counters.invalidCall);return;}
    if(s.depth){
        const auto&outer=s.frames[s.depth-1];
        if(outer.token==token_){
            const bool same=SameSubject(subject_,outer.subject);
            if(same||parent==ParentRelation::ExplicitCurrent){parentSpan_=outer.span;parentKnown_=true;explicitCross_=!same;}
            else Count(counters.crossSubjectParentOmitted);
        }
    }
    if(s.depth==kDepth){
        ++s.suppressed;suppressed_=true;Count(counters.overflow);
        (void)Emit(Stage::ContextGap,Part::ContextGap);
        return;
    }
    slot_=s.depth;s.frames[s.depth++]={subject_,span_,token_,false};
    active_=true;Count(counters.opened);
    beforeRecorded_=Emit(Stage::Before,explicitCross_?Part::ExplicitCrossSubject:Part::Observation,&before);
}
Scope::~Scope() noexcept {
    if(suppressed_){
        if(GetCurrentThreadId()!=ownerThread_){Count(counters.wrongThread);return;}
        auto&s=threadState;if(s.suppressed)--s.suppressed;else Count(counters.misnested);
        return;
    }
    if(active_&&!closed_)(void)Close(nullptr,{});
}
bool Scope::Active()const noexcept {
    return active_&&!closed_&&GetCurrentThreadId()==ownerThread_&&flight::ActiveRecordingToken()==token_;
}

bool Scope::Check(uint16_t bit) noexcept {
    if(!active_||closed_){if(closed_)Count(counters.invalidCall);return false;}
    if(GetCurrentThreadId()!=ownerThread_){Count(counters.wrongThread);return false;}
    auto&s=threadState;
    if(s.emitting){Count(counters.reentrant);return false;}
    if(s.depth!=slot_+1||s.frames[slot_].span!=span_||s.frames[slot_].retired){Count(counters.misnested);return false;}
    if(seen_&bit){Count(counters.invalidCall);return false;}
    seen_|=bit;return true;
}

bool Scope::Emit(Stage stage,Part part,const Observation*observation,
                 const Cause*cause,Known<int64_t> nativeResult) noexcept {
    const auto current=flight::ActiveRecordingToken();
    if(!current||current!=token_){if(!tokenNoted_){tokenNoted_=true;Count(counters.tokenChanged);}return false;}
    auto&s=threadState;
    if(s.emitting){Count(counters.reentrant);return false;}
    flight::Event event{};
    event.context=subject_.context;event.localPeer=subject_.localPeer;
    event.stage=uint16_t(stage);event.reason=uint16_t(part);event.span=span_;
    // Paired events of an already-admitted scope are retained together. Ordinary
    // pre-admission belongs to the adapter, not per-side Record resampling.
    event.detailKnown=flight::SpanKnown|flight::CriticalStateHint;
    if(subject_.owner.available){event.owner=subject_.owner.value;event.detailKnown|=flight::OwnerKnown;}
    if(subject_.ownerEpoch.available){event.ownerEpoch=subject_.ownerEpoch.value;event.detailKnown|=flight::OwnerEpochKnown;}
    if(subject_.motionEpoch.available)event.motionEpoch=subject_.motionEpoch.value;
    if(subject_.nativePedRef.available)event.nativeHandle=subject_.nativePedRef.value;
    if(parentKnown_)flight::AddUnsigned(event,flight::Metric::TaskParentSpan,parentSpan_);
    flight::AddUnsigned(event,flight::Metric::TaskRecordingToken,token_);
    if(cause){
        if(cause->why.available)flight::AddSigned(event,flight::Metric::TaskReason,cause->why.value);
        if(cause->eventType.available)flight::AddSigned(event,flight::Metric::TaskEventType,cause->eventType.value);
        if(cause->abortPriority.available)flight::AddSigned(event,flight::Metric::TaskAbortPriority,cause->abortPriority.value);
        if(cause->eventPresent.available)flight::AddUnsigned(event,flight::Metric::TaskEventPresent,cause->eventPresent.value?1:0);
        if(cause->callerImageKind.available&&cause->callerImageKind.value){
            flight::AddUnsigned(event,flight::Metric::TaskCallerImageKind,cause->callerImageKind.value);
            if(cause->callerRva.available)flight::AddUnsigned(event,flight::Metric::TaskCallerRva,cause->callerRva.value);
        }else if(cause->callerRva.available)Count(counters.invalidCall);
    }else if(nativeResult.available){
        flight::AddSigned(event,flight::Metric::NativeResult,nativeResult.value);
    }else{
        if(operation_!=Operation::Unknown)flight::AddUnsigned(event,flight::Metric::TaskOperation,unsigned(operation_));
        if(subject_.kind.available){
            if(subject_.kind.value==SubjectKind::Player||subject_.kind.value==SubjectKind::Npc)
                flight::AddUnsigned(event,flight::Metric::TaskSubjectKind,unsigned(subject_.kind.value));
            else Count(counters.invalidCall);
        }
        if(subject_.nativePedRef.available)flight::AddUnsigned(event,flight::Metric::TaskNativePedRef,subject_.nativePedRef.value);
        if(observation){
            if(observation->taskLocalId.available)flight::AddUnsigned(event,flight::Metric::TaskLocalId,observation->taskLocalId.value);
            if(observation->taskType.available)flight::AddSigned(event,flight::Metric::TaskType,observation->taskType.value);
            if(observation->taskSlot.available)flight::AddSigned(event,flight::Metric::TaskSlot,observation->taskSlot.value);
        }
    }
    s.emitting=true;
    const bool recorded=flight::Record(event);
    s.emitting=false;
    if(!recorded)Count(counters.recordFalse);
    // Existing Record has no atomic expected-token API. A lifecycle race may
    // retain this typed old token in a new file: consumers MUST reject mismatch.
    const auto after=flight::ActiveRecordingToken();
    if(after!=token_){if(!tokenNoted_){tokenNoted_=true;Count(counters.tokenChanged);}return false;}
    return recorded;
}

bool Scope::RecordCause(const Cause&cause) noexcept {
    return Check(kCause)&&Emit(Stage::Cause,Part::Cause,nullptr,&cause);
}
bool Scope::AbortAttempt(const Observation&observation,const Cause&cause) noexcept {
    if(!Check(kAbortAttempt))return false;
    bool result=Emit(Stage::AbortAttempt,Part::Observation,&observation);
    if(AnyCause(cause))result=RecordCause(cause)&&result;
    return result;
}
bool Scope::AbortResult(const Observation&observation,Known<int64_t> nativeResult) noexcept {
    if(!Check(kAbortResult))return false;
    bool result=Emit(Stage::AbortResult,Part::Observation,&observation);
    if(nativeResult.available)result=Emit(Stage::AbortResult,Part::NativeReturn,nullptr,nullptr,nativeResult)&&result;
    return result;
}
bool Scope::Complete(const Observation&observation,const Cause&cause) noexcept {
    if(!Check(kCompletion))return false;
    bool result=Emit(Stage::Completion,Part::Observation,&observation);
    if(AnyCause(cause))result=RecordCause(cause)&&result;
    return result;
}
bool Scope::Replace(const Observation&before,const Observation&after,const Cause&cause) noexcept {
    if(!Check(kReplacement))return false;
    bool result=Emit(Stage::Replacement,Part::ReplacementBefore,&before);
    result=Emit(Stage::Replacement,Part::ReplacementAfter,&after)&&result;
    if(AnyCause(cause))result=RecordCause(cause)&&result;
    return result;
}
bool Scope::Destroy(const Observation&observation,const Cause&cause) noexcept {
    if(!Check(kDestruction))return false;
    bool result=Emit(Stage::Destruction,Part::Observation,&observation);
    if(AnyCause(cause))result=RecordCause(cause)&&result;
    return result;
}

bool Scope::Close(const Observation*after,Known<int64_t> nativeResult) noexcept {
    if(!active_||closed_){if(closed_)Count(counters.invalidCall);return false;}
    if(GetCurrentThreadId()!=ownerThread_){Count(counters.wrongThread);return false;}
    auto&s=threadState;
    if(s.emitting){Count(counters.reentrant);return false;}
    const bool nested=s.depth==slot_+1&&s.frames[slot_].span==span_&&!s.frames[slot_].retired;
    if(!nested){Count(counters.misnested);s.poisoned=true;(void)Emit(Stage::ContextGap,Part::ContextGap);}
    if(!after)Count(counters.unobservedAfter);
    const bool recorded=Emit(Stage::After,after?(explicitCross_?Part::ExplicitCrossSubject:Part::Observation):Part::UnobservedAfter,after);
    bool result=recorded;
    if(nativeResult.available)result=Emit(Stage::After,Part::NativeReturn,nullptr,nullptr,nativeResult)&&result;
    if((!beforeRecorded_||!recorded||!nested||tokenNoted_)&&!unpairedNoted_){unpairedNoted_=true;Count(counters.unpaired);}
    if(slot_<s.depth&&s.frames[slot_].span==span_)s.frames[slot_].retired=true;
    Prune(s);active_=false;closed_=true;Count(counters.closed);
    return beforeRecorded_&&result&&nested&&!tokenNoted_;
}
bool Scope::Finish(const Observation&after,Known<int64_t> nativeResult) noexcept {return Close(&after,nativeResult);}
#undef TASK_COUNTERS
}
