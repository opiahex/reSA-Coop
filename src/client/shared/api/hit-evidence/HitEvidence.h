#pragma once
#include "shared/game/Game.h"
#include "common/lib/buffer/Buffer.h"
namespace coop {
void WriteHitEvidence(Writer&w,game::CEntity*target);
bool ReadHitEvidence(Reader&r,game::CEntity*target);
}
