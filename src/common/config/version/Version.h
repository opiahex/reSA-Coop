#pragma once
#include <cstdint>

#define COOP_NAME "SA:COOP"
#define COOP_VERSION_STR "0.2.33-3p"

namespace coop {
// Bumped on any incompatible change of the network protocol.
constexpr uint32_t kProtocolVersion = 43;
constexpr uint16_t kDefaultPort = 7788;
// Size of gta_sa.exe 1.0 US (HOODLUM) вЂ” the only supported executable.
constexpr uint32_t kSupportedExeSize = 14383616;
}  // namespace coop
