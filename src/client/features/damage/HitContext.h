#pragma once
#include "shared/game/Game.h"
namespace coop {
// Scoped around the actual native bullet collision, including the tyre path
// which never calls CVehicle::InflictDamage.
struct BulletHitContext {
    game::CEntity* target=nullptr;
    game::CVector start,end;
    uint32_t sequence=0;
    uint8_t weapon=0;
};
inline BulletHitContext g_bulletHit;
}
