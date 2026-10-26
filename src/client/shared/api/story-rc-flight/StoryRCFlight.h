#pragma once
#include <cstdint>
#include "common/lib/buffer/Buffer.h"
namespace coop {
void ObserveCooperativeRCFlightScript(void*script);
void ObserveCooperativeRCFlightCommand(void*script,int opcode,const int32_t*parameters);
bool CooperativeRCFlightCondition(void*script,int opcode,const int32_t*parameters,bool&value);
// Temporary HUD overlays only; never modifies the actual SCM fuel variable.
bool CooperativeRCFlightCounter(uint32_t variableOffset,int32_t&value);
bool CooperativeRCFlightFuel(uint8_t player,float&remaining,float&capacity,bool&failed);
// Native Supply Lines has one finite aircraft attempt, not free role respawns.
bool CooperativeRCFlightPreventsRecovery(uint16_t source,uint64_t generation);
void CooperativeRCFlightRoleFailed(uint8_t player,uint64_t run,uint32_t phase,uint32_t assignment);
// G_STORY_COOP sub-operation 6; token is the shared story run.
void ReceiveCooperativeRCFlight(uint8_t from,uint64_t token,Reader&r);
}
