#pragma once
#include <cstdint>
#include "common/lib/buffer/Buffer.h"
#include "shared/game/Game.h"
namespace coop {
// Called after the real command by the owned shared SCM interpreter. Equipment
// selection is an availability boundary, not a copy of the host's active role.
void ObserveStoryEquipmentCommand(void*script,int opcode,const int32_t*parameters);
// G_PLAYER_EXTRAS op 3; transport and this reader both validate its authority.
void HandleStoryEquipmentEvent(uint8_t from,Reader&reader);
// An explicit local goggles toggle supersedes a borrowed mission effect.
void ObserveLocalStoryVision();
// Host story inventory changes are provisional until native mission cleanup
// confirms which weapons remain. Personal inventory is tracked separately.
void ApplyStoryMissionWeapon(uint8_t operation,int weapon,int ammo,bool inHand);
// Explicit REMOVE_ALL_CHAR_WEAPONS from the shared host script. Targeted to
// the participating connection, independently of temporary weapon loans.
void ApplyStoryMissionStrip(uint8_t from,uint32_t world,uint64_t run,uint32_t phase,
                            uint32_t sequence,uint8_t target,uint32_t targetIncarnation);
void RestoreStoryMissionWeapons();
void PersonalStoryWeapon(int slot,int&weapon,int&ammo);
// Snapshot for a role's eventual release. Resolve a story-selected loan back
// to the personal selection that preceded it; do not change the current weapon.
int PersonalStoryWeaponSelection();
// Native attached-gunner setup grants its own ammunition; native detach
// writes an old turret-ammo snapshot back. Neither is a personal pickup.
class StoryNativeInventoryScope {
public:
    StoryNativeInventoryScope();
    explicit StoryNativeInventoryScope(int weapon);
    ~StoryNativeInventoryScope();
    StoryNativeInventoryScope(const StoryNativeInventoryScope&)=delete;
    StoryNativeInventoryScope&operator=(const StoryNativeInventoryScope&)=delete;
private:
    uint32_t token=0;
};
}
