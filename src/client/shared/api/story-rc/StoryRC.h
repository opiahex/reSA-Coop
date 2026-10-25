#pragma once
#include "common/lib/buffer/Buffer.h"
namespace coop {
// G_STORY_COOP sub-operation 5. The token is the source story run.
void ReceiveCooperativeRCMagnet(uint8_t from,uint64_t token,Reader&r);
// Called only while the authoritative shared mission interpreter is current.
void ObserveCooperativeRCMissionScript(void*script);
// Preserve native pickup tests while another RC pilot holds the same cargo.
bool CooperativeRCCondition(void*script,int opcode,const int32_t*parameters,bool&value);
}
