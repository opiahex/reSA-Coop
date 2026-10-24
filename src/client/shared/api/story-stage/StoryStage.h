#pragma once
#include <cstdint>
namespace coop {
bool AwaitCooperativeStage(void*script,int opcode,uint16_t source=0);
bool CooperativeStageWaiting(void*script=nullptr);
bool LocalCooperativeRoleReady();
void ArmCooperativeStage(void*script);
bool CooperativeStageArmed(void*script);
uint32_t CooperativeStoryClock();
void CancelCooperativeStage(void*script);
}
