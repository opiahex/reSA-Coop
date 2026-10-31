// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
#pragma once
#include "features/sync-diagnostics/TaskTreeSnapshot.h"
namespace coop::task_lifecycle::tree_trace {
enum class Stage:uint16_t {Begin=850,Root,Node,Edge,History,End};
struct Result {
 uint64_t span=0;
 uint32_t recordingToken=0;
 uint16_t attempted=0,admitted=0;
 bool inputComplete=false,beginAdmitted=false,endAdmitted=false,recordingStable=false;
 bool invalidInput=false,metadataInvalid=false;
 // Admission is NOT persistence, native task completion or atomic capture.
 bool AllRowsAdmitted()const noexcept {
  return !invalidInput&&!metadataInvalid&&recordingStable&&beginAdmitted&&endAdmitted&&attempted==admitted;
 }
};
// Caller supplies an already admitted/throttled COPIED selected-ped snapshot.
// Fixed<=211rows, no memory reads/hooks/heap/IO or another recording backend.
// span/node indices describe only this copied graph, never task lifetimes.
// End row's attempted/admitted counts cover preceding rows (exclude End).
Result RecordSnapshot(const tree::Snapshot&,uint32_t expectedRecordingToken,Span parent={}) noexcept;
}
