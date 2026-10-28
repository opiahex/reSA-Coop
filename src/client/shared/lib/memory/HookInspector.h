// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#pragma once
#include "shared/lib/memory/HookRegistry.h"

// Explicit on-demand samples. Caller owns object lifetime, cadence, context
// epochs and any history. This does not patch code or validate a native ABI.
namespace coop::hookinspect {
enum class Status:uint8_t {
    Busy,NotFound,Invalid,Pending,Unreadable,Partial,ChangedDuringRead,Unknown,Match,Mismatch
};
struct Sample {
    hookreg::Record install{};
    hookreg::Bytes current{};
    hookreg::Comparison desiredComparison=hookreg::Comparison::Unknown;
    uint64_t contextEpoch=0; // supplied label only; 0 means unknown lifetime
    uint64_t ticks=0;
    bool ticksValid=false;
    Status status=Status::Unknown;
};
// id is a trusted, NUL-terminated internal registry identifier. Two matching
// reads are not proof of stable memory, a live object, or absence of ABA.
// Pending means no captured final result; the native attempt may have run.
Sample Inspect(const char*id,uint64_t contextEpoch)noexcept;
}
