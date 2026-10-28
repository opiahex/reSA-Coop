// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Install-time observations only. The collector never writes target code,
// validates a native ABI, or decides whether a third-party chain is safe.
namespace coop::hookreg {
constexpr size_t kSlots=512,kBytes=32,kId=80,kModule=48,kOwner=48,kPurpose=128;
enum class Kind:uint8_t { Detour,Call,Jump,Nop,Write,Vtable,Scm,Other };
enum class Comparison:uint8_t { Unknown,Match,Mismatch };
enum class ReadStatus:uint8_t { NotRequested,Complete,Partial,Inaccessible,InvalidRange };
enum class Outcome:uint8_t { NotAttempted,ApiSuccess,FailedUnchanged,Conflict,Indeterminate };
enum class Check:uint8_t { NotChecked,Passed,Failed,NotApplicable };
enum class Chain:uint8_t { NotChecked,NativeExpected,ExplicitVerified,ExternalUnverified,Conflict,Indeterminate };
enum class Put:uint8_t { Stored,Busy,Capacity,Invalid,IdConflict,StaleAttempt,NotFound,AlreadyFinalized };
struct Bytes {
    std::array<uint8_t,kBytes> data{};
    uint32_t total=0;
    uint8_t captured=0;
    bool truncated=false;
    ReadStatus read=ReadStatus::NotRequested;
};
// For trusted local manifest bytes only; does not inspect game memory.
bool ManifestBytes(Bytes&out,const uint8_t*data,size_t size)noexcept;
Comparison Compare(const Bytes&a,const Bytes&b)noexcept;
struct PeIdentity {
    uintptr_t base=0;
    uint32_t imageSize=0,timestamp=0,checksum=0;
    uint16_t machine=0;
    bool valid=false; // PE header identity only; NOT a verified full SHA-256
};
struct Spec {
    char id[kId]{},module[kModule]{},owner[kOwner]{},purpose[kPurpose]{};
    uintptr_t site=0;
    uint32_t span=0;
    Kind kind=Kind::Other;
    Chain chain=Chain::NotChecked;
    Bytes expected{},desired{};
};
struct Record {
    Spec spec{};
    PeIdentity pe{};
    uint32_t rva=0,error=0;
    bool rvaKnown=false; // only an address inside a validated PE image
    uint64_t attempt=0;
    uintptr_t previous=0,trampoline=0;
    Bytes original{},readback{};
    Comparison pre=Comparison::Unknown,post=Comparison::Unknown;
    Outcome outcome=Outcome::NotAttempted;
    Check abi=Check::NotChecked; // collector cannot establish ABI compatibility
    bool fullSha256Verified=false; // deliberately false until separate proof
};
struct Ticket {Record record{};};
struct Counters {uint32_t busy=0,capacity=0,invalid=0,conflict=0,stale=0,alreadyFinalized=0;};
struct SnapshotResult {size_t count=0;bool busy=false,truncated=false;Counters counters{};};

// Caller serializes installation of the same site. Failed telemetry capture
// must never by itself imply a failed/unchanged hook or alter a previous chain.
// Each attempt finalizes once; Busy can be retried with the same ticket.
Put CaptureBefore(const Spec&spec,Ticket&ticket)noexcept;
Put CaptureAfter(Ticket&ticket,Outcome outcome,uint32_t error,uintptr_t previous,uintptr_t trampoline)noexcept;
Put Lookup(const char*id,Record&out)noexcept;
SnapshotResult Snapshot(Record*out,size_t capacity)noexcept;
Counters Stats()noexcept;
}
