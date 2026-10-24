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
uint32_t CooperativeStoryPhase();
void AdoptCooperativeStoryPhase(uint64_t run,uint32_t phase);
void AdvanceCooperativeStoryPhase(void*script,const char*reason);
void ResetCooperativeStages();
bool LocalStoryTransitionPending();
void SetLocalStoryTransitionPending(bool pending);
bool LocalStoryRecoveryReady();
void SetLocalStoryRecoveryPending(bool pending);
}
