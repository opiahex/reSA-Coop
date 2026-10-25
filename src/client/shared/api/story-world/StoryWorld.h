#pragma once
#include <cstdint>
#include "shared/game/Game.h"
namespace coop {
uint32_t StoryWorldRevision();
bool StoryWorldReady(uint32_t revision,const game::CVector& point);
// Resolve the original target before the native command can replace its model.
bool ResolveStoryMapTarget(int opcode,int* parameters);
// Only the authoritative story interpreter calls this after a native map edit.
void RecordStoryMapCommand(int opcode,const int* parameters);
}
