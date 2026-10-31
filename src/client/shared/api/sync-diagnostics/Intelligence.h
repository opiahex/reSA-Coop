#pragma once

namespace coop::task_lifecycle::native {
using IntelligenceBody = void (*)(void* intelligence, void* context);

// Observe the complete existing call, including its early returns, without
// exposing diagnostic implementation types to another feature.
void ObserveIntelligenceCall(void* intelligence, void* context,
                             const void* caller, IntelligenceBody body);
}
