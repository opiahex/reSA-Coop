#pragma once
#include <cstdint>
namespace coop {
// An explicit owned-script REMOVE_IPL starts a new native incarnation. Ordinary
// streaming out/in keeps its world damage. The host's durable IPL snapshot and
// map-object records carry this same incarnation, even when REMOVE and REQUEST
// occurred in one interpreter frame.
// Until nativeResetApplied, old local entities must not publish damage under
// the new incarnation while the deferred map operation is still pending.
void ResetWorldObjectIpl(const char* name, uint32_t incarnation, bool nativeResetApplied = false);
uint32_t WorldObjectIplIncarnation(const char* name);
}
