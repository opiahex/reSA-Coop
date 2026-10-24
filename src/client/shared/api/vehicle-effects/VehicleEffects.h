#pragma once
#include "shared/game/Game.h"
namespace coop {
void CaptureVehicleHorn(game::CVehicle*vehicle,bool sounding);
bool RemoteVehicleHorn(game::CVehicle*vehicle,bool fallback);
}
