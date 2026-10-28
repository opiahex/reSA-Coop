// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#pragma once
#include <cstdint>
namespace coop::ui {
// Bind/update from CommonFrame only. Render refuses a different thread.
// No input capture is changed here. The caller explicitly grants interaction
// only while its existing keyboard + mouse UI capture owns input.
void UpdateDiagnosticSnapshot(uint64_t nowMs);
void RenderDiagnosticPanel(bool interactive=false);
struct DiagnosticTimings {
    uint64_t draws=0,samples=0;
    double drawTotalUs=0,drawMaxUs=0,snapshotTotalUs=0,snapshotMaxUs=0;
    bool available=false;
};
// Copied CPU-side timings on the existing owner thread; not GPU time/FPS proof.
DiagnosticTimings GetDiagnosticTimings();
}
