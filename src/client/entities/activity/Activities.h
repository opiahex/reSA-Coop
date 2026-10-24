#pragma once
#include <cstdint>
#include "shared/game/Game.h"

namespace coop {
// Local training exercises keep native scripts and collision with the map;
// participants of another game cannot obstruct or damage those exercises.
bool PlayerTraining(uint8_t id);
bool PopulationAvailable(uint8_t id);
bool TrainingIgnoresContact(game::CEntity* a, game::CEntity* b);
bool TrainingVehicleRestricted(game::CVehicle* vehicle);
}
