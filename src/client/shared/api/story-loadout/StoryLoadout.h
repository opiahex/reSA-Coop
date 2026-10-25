#pragma once
#include <cstdint>
namespace coop {
// Host story inventory changes are provisional until native mission cleanup
// confirms which weapons remain. Personal inventory is tracked separately.
void ApplyStoryMissionWeapon(uint8_t operation,int weapon,int ammo,bool inHand);
void RestoreStoryMissionWeapons();
void PersonalStoryWeapon(int slot,int&weapon,int&ammo);
}
