#pragma once
#include "shared/game/Game.h"
namespace coop {
// Held story objects retain their world identity; Props must not create a
// second visual object while a player carries this one.
bool IsSharedMissionObject(game::CObject*object);
uint16_t SharedMissionObjectId(game::CObject*object);
game::CObject* SharedMissionObjectById(uint16_t id);
bool SharedMissionObjectReady(uint16_t id);
// Called only after the authoritative shared interpreter's native 0550.
void RecordStoryObjectFreeze(int handle,bool frozen);
using CargoContactFn=bool(__fastcall*)(game::CPhysical*,void*,game::CPhysical*,void*,float*,float*);
bool ProcessSharedCargoContact(CargoContactFn original,game::CPhysical*a,void*edx,game::CPhysical*b,void*point,float*x,float*y,bool&handled);
}
