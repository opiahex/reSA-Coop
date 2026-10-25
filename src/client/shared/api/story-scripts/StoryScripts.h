#pragma once
namespace coop {
// Host-only: a current story mission or a native child of its interpreter.
// Ownership expires at termination, slot reuse, or the cooperative run boundary.
bool IsSharedStoryScript(void* script);
}
