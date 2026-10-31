// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <array>

// Fixed-size diagnostic metadata only. A fingerprint is a correlation hint,
// not a globally unique packet ID; never put packet contents or user text here.
namespace coop::flight {
struct Context {
    uint64_t fingerprint=0,generation=0;
    uint32_t wireTime=0,incarnation=0,world=0;
    uint16_t entity=0;
    uint8_t msg=0,channel=0,source=255,recipient=255;
    uint16_t diagnosticFlags=0; // local capture hints only, never a network field

};
enum ContextDiagnosticFlags:uint16_t { CriticalState=1, StateCacheBusy=2, StateCacheEvicted=4 };
enum DetailKnown:uint16_t { SpanKnown=1, MissionKnown=2, ScriptBirthKnown=4, InstructionKnown=8, OwnerKnown=16, OwnerEpochKnown=32, OpcodeKnown=64, ScriptOffsetKnown=128, CriticalStateHint=256, StateCacheBusyHint=512, StateCacheEvictedHint=1024 };
enum DiagnosticStage:uint16_t { ScmProcessEnter=256,ScmProcessLeave,ScmCommandEnter,ScmCommandLeave };
// Optional observations of real native state. No opaque pointer/payload values.
// Missing keys are unavailable; do not encode unknown measurements as zero.
enum class Metric:uint16_t {
    None=0,EntityKind=1,StateClock,ActionRevision,RequestedModel,ObservedModel,
    ParentKind,ParentId,ParentGeneration,RequestedArea,ObservedArea,
    RequestedTask=16,ObservedTask,ObservedPedState,NativeFlags,
    RequestedAnimBlock,RequestedAnimIndex,RequestedAnimTime,
    ObservedAnimBlock,ObservedAnimIndex,ObservedAnimTime,RequestedBlend,ObservedBlend,
    RequestedX=32,RequestedY,RequestedZ,ObservedX,ObservedY,ObservedZ,
    RequestedVx,RequestedVy,RequestedVz,ObservedVx,ObservedVy,ObservedVz,
    RequestedHeading,ObservedHeading,RequestedEngine,ObservedEngine,RequestedRotor,ObservedRotor,
    RequestedDamage=64,Weapon,ActorKind,ActorId,ActorGeneration,TargetKind,TargetId,TargetGeneration,
    ProjectileId,ExplosionId,ExplosionType,RequestedOwner,
    ModelId=80,IplId,RequestedLoaded,ObservedLoaded,RequestId,CancelReason,Operation,Revision,ObservedStreamingState,BeforeCount,AfterCount,
    ZoneId=96,ZoneLeader,PreviousLeader,PopulationCount,
    FromRun=112,ToRun,FromPhase,ToPhase,ControlFlags,ProposedRun,ProposedPhase,BeforeControlFlags,
    AttachmentFlags=128,ObservedMotionFlags,NativeResult,
    TargetX=144,TargetY,TargetZ,RequestedDecision,ObservedDecision,
    ReferenceKind=160,
    BracketBeforeClock=176,BracketAfterClock,InterpolationAlpha,PredictionSeconds,RequestedVehicleFlags,ObservedVehicleFlags,
    RequestedSteer,ObservedSteer,RequestedGas,ObservedGas,RequestedBrake,ObservedBrake,RequestedWater,ObservedWater,ObservedVehicleStatus,ThrottledObservations,SuppressedErrors,
    RequestedMoveState=193,ObservedMoveState,RequestedPedFlags,ObservedPedFlags,LayerIndex,RequestedAnimSpeed,ObservedAnimSpeed,
    RequestedHealth=200,ObservedHealth,RequestedArmour,ObservedArmour,
    TaskParentSpan=208,TaskRecordingToken=209,TaskLocalId=210,TaskType=211,TaskOperation=212,TaskSlot=213,TaskReason=214,TaskEventType=215,TaskAbortPriority=216,TaskCallerImageKind=217,TaskCallerRva=218,TaskSubjectKind=219,TaskNativePedRef=220,TaskEventPresent=221,
    TaskTreeSlotKind=222,TaskTreeNode=223,TaskTreeDepth=224,TaskTreeLinkState=225,TaskTreeTargetNode=226,TaskTreeEdgeKind=227,TaskHistoryRole=228,TaskEventPriority=229,TaskTreeFlags=230,TaskTreeComplete=231,TaskTreeNodeCount=232,TaskTreeSimple=233,TaskTreeRowsAttempted=234,TaskTreeRowsAdmitted=235
};
enum class NumberType:uint8_t { Unavailable=0,Unsigned=1,Signed=2,Float=3 };
struct Number {Metric key=Metric::None;NumberType type=NumberType::Unsigned;uint64_t bits=0;};
constexpr size_t kNumbers=8;
enum NumberFlags:uint8_t { NumbersTruncated=1,NumbersInvalid=2,DuplicateNumber=4,InvalidMetric=8,InvalidNumberType=16,NonFiniteNumber=32 };
const char* MetricName(Metric key) noexcept;
NumberType MetricType(Metric key) noexcept;
bool ValidNumber(const Number&value) noexcept;
struct Event {
    Context context{};
    uint16_t stage=0,reason=0;
    float health=0,maxHealth=0,armour=0,beforeHealth=0,afterHealth=0;
    uint32_t motionEpoch=0,nativeHandle=0;
    uint8_t localPeer=255;
    uint64_t ticks=0,localSeq=0; // QPC before lock; localSeq is commit order
    uint32_t pid=0,tid=0;       // assigned by Record
    // Local diagnostic metadata, never serialized into a network packet.
    // A span is scoped by pid+tid, not a global packet/event identity.
    uint64_t span=0,missionRun=0,scriptBirth=0;
    uint32_t missionPhase=0,scriptOffset=0,ownerEpoch=0;
    uint16_t opcode=0,detailKnown=0;
    uint8_t owner=255;
    uint8_t numberCount=0,numberFlags=0;
    std::array<Number,kNumbers> numbers{};
};
// Bounded, allocation-free. False also marks incomplete metadata in the event.
bool AddUnsigned(Event&event,Metric key,uint64_t value) noexcept;
bool AddSigned(Event&event,Metric key,int64_t value) noexcept;
bool AddFloat(Event&event,Metric key,float value) noexcept;
struct Counters {
    // 32-bit cumulative counters wrap after UINT32_MAX; samples are concurrent.
    uint32_t accepted=0,busyDrops=0,lifecycleDrops=0,historyOverwrites=0,drainDrops=0,ioErrors=0;
    uint32_t segmentsEvicted=0,eventsEvicted=0,rotationFailures=0,exportFailed=0;
};
struct SnapshotResult { size_t count=0; bool busy=false; Counters counters{}; uint64_t recordingStartTicks=0;uint32_t recordingEpoch=0;bool recordingEnabled=false; };

// Start/Stop must be serialized by lifecycle code; they may allocate, wait and
// perform file IO. The containing module is pinned for the process lifetime.
// never from a hook or exception path. A new file is created exclusively.
// Stop returns false on timeout or if its finite tail cutoff could not be
// drained. On timeout the containing module must not unload while the worker
// runs; an incomplete cutoff also sets exportFailed even if loss is unknown.
bool Start(const std::wstring& path);
bool Stop(uint32_t timeoutMs=1000);
bool Enabled() noexcept;
// Lock-free active recording fence for bounded local transition metadata. Zero
// means disabled/changing; it is not a wire epoch or globally unique session ID.
uint32_t ActiveRecordingToken() noexcept;
// No disk IO, allocation, std::mutex or retry/spin. Critical events can enter
// a bounded reserve when history is busy; accepted counts history commits,
// criticalPending exposes admitted events awaiting their local sequence.
// False means disabled/busy/full. Record(true) is not proof of disk persistence.
bool Record(Event event) noexcept;
// Nonblocking best-effort copy of the newest cap committed entries. If busy,
// returns immediately with busy=true; crash callers must tolerate missing tail.
SnapshotResult Snapshot(Event* out,size_t cap) noexcept;
Counters GetCounters() noexcept;
uint64_t TickFrequency() noexcept;
uint64_t NextDiagnosticSpan() noexcept;

enum class Category:uint8_t { Net,Packet,Sync,Entity,Ownership,Vehicle,Ped,Object,Mission,Scm,Hook,Memory,Streaming,Reconnect,Assert,Crash,Perf,Count };
constexpr uint32_t kAllCategories=(1u<<unsigned(Category::Count))-1;
Category EventCategory(const Event&event) noexcept;
const char* CategoryName(Category category) noexcept;
bool CriticalEvent(const Event&event) noexcept;
struct CapturePolicy {uint32_t categories=kAllCategories,stateEvery=4,scmEvery=16,scmSpansPerSecond=512,nativeObservationsPerSecond=2048;uint8_t peer=255,packet=0;uint16_t entity=0;};
struct CaptureStatistics {
    uint32_t attempted=0,filtered=0,sampledOut=0,criticalDrops=0;
    uint32_t scmPreAttempted=0,scmPreSampled=0,scmPreFiltered=0,scmPreRateLimited=0,scmPreBusy=0;
    uint32_t criticalBuffered=0,criticalPending=0,criticalReserveFull=0;
    uint32_t incompleteMetadata=0;
    uint32_t nativePreAttempted=0,nativePreFiltered=0,nativePreSampled=0,nativePreBusy=0,nativePreBudget=0;
    uint64_t recordingStartTicks=0;uint32_t recordingEpoch=0;bool recordingEnabled=false;
    bool busy=false;CapturePolicy policy{};
};
// Admit before querying native script memory. One try-lock; a declined ordinary
// span is intentionally unobserved, not a successfully replayable command.
// Known errors bypass admission; callers must also retain errors discovered on
// return, with explicit missing-entry metadata. Zero budget means unlimited.
bool AdmitScmSpan(uint64_t span,bool critical=false) noexcept;
enum class NativeAdmission:uint8_t { Admitted,Disabled,Filtered,Sampled,Busy,Budget };
// Use before additional task/association/model/native-state queries. Admission
// never controls gameplay. Critical observations bypass display/capture filters
// and sampling but still report Busy/Budget; callers keep a bounded pending
// observation or emit an explicit unavailable outcome, never fake native proof.
NativeAdmission AdmitNativeObservation(const Context&context,uint16_t stage,uint8_t localPeer,bool critical=false) noexcept;
// Single try-lock; no IO/allocation. Critical lifecycle/reject events bypass
// filtering/sampling, but contention remains an explicit criticalDrops loss.
bool SetCapturePolicy(CapturePolicy policy) noexcept;
CaptureStatistics CaptureStats() noexcept;

// Optional client metadata sidecar, invoked on the existing writer, never on
// Record/UI/exception callers. Register during serialized, disabled lifecycle.
// Receives a trusted local basename; owns a CREATE_NEW sidecar. The worker also
// attempts automatic session metadata at1s, at most3 times with distinct names.
// This does not consume the four explicit recent-snapshot requests.
// False makes the overall export incomplete even if recent events were saved.
using SnapshotSupplement = bool (*)(const wchar_t* snapshotPath) noexcept;
bool SetSnapshotSupplement(SnapshotSupplement exporter) noexcept;

enum class SnapshotExportResult : uint8_t { None, Queued, Writing, Saved, Busy, Disabled, Limit, IoError, Stopped };
struct SnapshotExportStatus {
    uint32_t requested=0,completed=0;
    SnapshotExportResult result=SnapshotExportResult::None;
    bool busy=false; // no coherent status sample; numeric fields are unavailable
};
// A single try-lock only: no allocation, file IO, thread creation or waiting.
// The existing writer saves up to 4096 recent entries, not the complete trace.
// At most four requests per Start; each file is capped at 8 MiB and CREATE_NEW.
// Busy/Disabled/Limit reject this request; an accepted request returns Queued.
SnapshotExportResult RequestSnapshotExport() noexcept;
// Concurrent progress sample; request numbers are local to this recording.
SnapshotExportStatus GetSnapshotExportStatus() noexcept;
}
