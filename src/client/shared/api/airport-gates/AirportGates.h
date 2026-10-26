#pragma once
#include <cstdint>
#include "shared/game/Game.h"

namespace coop {
// These global native helpers remain local scripts. Only the existing airport
// barriers they operate are shared with players near the same entrance.
void BeginAirportGateScript(void* script);
void EndAirportGateScript(void* script);
int AirportGateOperandCount(void* script,int opcode);
// The caller consumes the already-read operands when true. Only0018,0038,034E,0471
// are conditions; their positive native result is returned through value.
bool HandleAirportGateCommand(void* script,int opcode,const int32_t* parameters,bool& value);
bool IsAirportGate(game::CObject* object);
// Current nearby simulator, or255 when no local shared gate is available.
uint8_t AirportGateSimulator(game::CObject* object);
}
