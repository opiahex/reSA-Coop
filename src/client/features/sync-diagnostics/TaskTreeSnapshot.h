// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// Copied task snapshots without GTA reads, hooks, virtual calls or logging.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "features/sync-diagnostics/TaskLifecycle.h"
namespace coop::task_lifecycle::tree {
constexpr size_t kPrimary=5,kSecondary=6,kNodes=64,kDepth=12,kCalls=512;
constexpr uint16_t kNoNode=UINT16_MAX;
// Borrowed local references/cookies exist only in callbacks and stack scratch.
// Neither is exported in Snapshot or used as a persistent task/event identity.
using Ref=Known<uintptr_t>;
enum class Read:uint8_t { Ok,Unavailable,Invalid,Changed };
struct TaskFacts {
 Known<int32_t> type{};Known<bool> simple{};
 Ref parent{},subtask{};Known<uint64_t> cookie{};
};
struct EventFacts {Known<int32_t> type{},priority{};Known<uint64_t> cookie{};};
struct Roots {std::array<Ref,kPrimary> primary{};std::array<Ref,kSecondary> secondary{};};
struct HistoryRefs {Ref nonTemporary{},temporary{},stored{},lastAbortedTask{};};
struct Services {
 void* user=nullptr;
 // All callbacks must be bounded/nonthrowing. Reads validate native pool/layout
 // BEFORE dereference; validation checks the captured local lifetime cookie.
 // The component performs no unchecked pointer or virtual-function read itself.
 bool(*subjectValid)(void*,const Subject&)noexcept=nullptr;
 Read(*roots)(void*,Roots&)noexcept=nullptr;
 Read(*history)(void*,HistoryRefs&)noexcept=nullptr;
 Read(*task)(void*,uintptr_t,TaskFacts&)noexcept=nullptr;
 Read(*event)(void*,uintptr_t,EventFacts&)noexcept=nullptr;
 Read(*taskValid)(void*,uintptr_t,uint64_t)noexcept=nullptr;
 Read(*eventValid)(void*,uintptr_t,uint64_t)noexcept=nullptr;
};
struct Limits {uint16_t nodes=kNodes,depth=kDepth,calls=kCalls;};
enum class State:uint8_t { Unavailable,Absent,Node,External,Truncated,Invalid,Changed,Present };
struct Link {State state=State::Unavailable;uint16_t node=kNoNode;};
struct Node {
 Known<int32_t> type{};Known<bool> simple{};
 Link parent{},subtask{};uint16_t depth=0;bool validatedAtRead=false;
};
struct EventObservation {
 State state=State::Unavailable;Known<int32_t> type{},priority{};
 Known<bool> scriptCommand{};bool validatedAtRead=false;
};
struct TaskObservation {State state=State::Unavailable;Known<int32_t> type{};Link inTree{};bool validatedAtRead=false;};
struct History {
 EventObservation nonTemporary{},temporary{},stored{},current{};
 TaskObservation lastAborted{};
};
enum Flag:uint32_t {
 InvalidInput=1u<<0,SubjectChanged=1u<<1,RootsUnavailable=1u<<2,HistoryUnavailable=1u<<3,
 ReadUnavailable=1u<<4,InvalidReference=1u<<5,LifetimeUnknown=1u<<6,LifetimeChanged=1u<<7,
 TypeUnknown=1u<<8,NodeLimit=1u<<9,DepthLimit=1u<<10,CallLimit=1u<<11,
 SubtaskCycle=1u<<12,ParentMismatch=1u<<13,ExternalParent=1u<<14,UnknownEdge=1u<<15,
 ContradictorySimple=1u<<16,ScriptInHistory=1u<<17,ParentCycle=1u<<18,PriorityUnknown=1u<<19
};
struct Counters {
 uint16_t serviceCalls=0,taskReads=0,eventReads=0,validations=0;
 uint16_t aliases=0,cycles=0,nodeTruncated=0,depthTruncated=0,readFailures=0;
};
struct Snapshot {
 Subject subject{};std::array<Link,kPrimary> primary{};std::array<Link,kSecondary> secondary{};
 std::array<Node,kNodes> nodes{};uint16_t count=0;History history{};
 uint32_t flags=0;Counters counters{};bool complete=false;
};
// One synchronous copied observation only, not an atomic world/task snapshot.
// Call before OR after an original native call with freshly reacquired services;
// never carry a borrowed pre-call pointer into a post-call reader.
// Complete means the requested bounded fields were observed, NEVER task/mission
// completion, cancellation WHY, a stable ABI, or a persistent task lifetime.
void Capture(const Subject&,const Services&,Snapshot&,Limits={}) noexcept;
}
