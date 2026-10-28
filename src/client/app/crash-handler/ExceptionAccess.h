// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#pragma once
#include <cstdint>
namespace coop {
constexpr const char* AccessViolationOperation(uintptr_t value) noexcept {
    switch(value){case 0:return "read from";case 1:return "write to";case 8:return "execute at";default:return "unknown operation at";}
}
}
