#pragma once
#include <cstdint>
#include "shared/game/Types.h"

namespace game { struct CPed; struct CEntity; }
namespace coop {
// Call only from the native kill registration. The NPC's simulator confirms
// the death; a shooter's delayed visual replica cannot credit it a second time.
void RecordCooperativeGangKill(game::CPed* victim, game::CEntity* killer);
// The one authoritative battle's area, independently of distance to the host.
bool CooperativeGangWarFightingAt(const game::CVector& position, int8_t area);
}
