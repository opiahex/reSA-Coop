#pragma once
#include "shared/game/Game.h"
namespace coop {
// Script commands can move an occupied vehicle before its ped matrix updates.
inline game::CVector StoryPosition(game::CPlayerPed*ped){
    if(auto*car=ped->VehicleIfIn();car&&game::IsVehicleValid(car))return car->GetPosition();
    return ped->GetPosition();
}
}
