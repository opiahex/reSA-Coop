// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Fixed-size diagnostic metadata only. A fingerprint is a correlation hint,
// not a globally unique packet ID; never put packet contents or user text here.
namespace coop::flight {
struct Context {
    uint64_t fingerprint=0,generation=0;
    uint32_t wireTime=0,incarnation=0,world=0;
    uint16_t entity=0;
    uint8_t msg=0,channel=0,source=255,recipient=255;
};
struct Event {
    Context context{};
    uint16_t stage=0,reason=0;
    float health=0,maxHealth=0,armour=0,beforeHealth=0,afterHealth=0;
    uint32_t motionEpoch=0,nativeHandle=0;
    uint8_t localPeer=255;
    uint64_t ticks=0,localSeq=0; // QPC before lock; localSeq is commit order
    uint32_t pid=0,tid=0;       // assigned by Record
};
struct Counters {
    // 32-bit cumulative counters wrap after UINT32_MAX; samples are concurrent.
    uint32_t accepted=0,busyDrops=0,lifecycleDrops=0,historyOverwrites=0,drainDrops=0,ioErrors=0;
    uint32_t segmentsEvicted=0,eventsEvicted=0,rotationFailures=0,exportFailed=0;
};
struct SnapshotResult { size_t count=0; bool busy=false; Counters counters{}; };

// Start/Stop must be serialized by lifecycle code; they may allocate, wait and
// perform file IO. The containing module is pinned for the process lifetime.
// never from a hook or exception path. A new file is created exclusively.
// Stop returns false if an OS I/O operation did not finish within timeoutMs:
// in that case the containing module must not be unloaded while the worker runs.
bool Start(const std::wstring& path);
bool Stop(uint32_t timeoutMs=1000);
bool Enabled() noexcept;
// No disk IO, allocation, std::mutex or retry/spin. False means disabled/busy.
bool Record(Event event) noexcept;
// Nonblocking best-effort copy of the newest cap committed entries. If busy,
// returns immediately with busy=true; crash callers must tolerate missing tail.
SnapshotResult Snapshot(Event* out,size_t cap) noexcept;
Counters GetCounters() noexcept;
uint64_t TickFrequency() noexcept;
}
