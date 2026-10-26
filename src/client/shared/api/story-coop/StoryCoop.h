#pragma once
#include <string>
#include "common/lib/buffer/Buffer.h"
#include "shared/game/Game.h"
namespace coop {
uint64_t StoryRunId();
void BeginCooperativeStory(const std::string&name);
void MarkCooperativeStoryPassed();
void UpdateCooperativeStoryName(const std::string&name);
void EndCooperativeStory(int32_t reward);
void ResyncCooperativeStory(uint8_t player);
bool StoryRoleOwnsCamera();
// Restore the native RC or attached-weapon view of a still-current assignment.
bool RestoreCooperativeRoleCamera();
void SetCooperativeStoryCamera(bool mandatory);
uint8_t CooperativeReservedSeats(game::CVehicle*car);
uint8_t LocalStoryRoleMode();
struct StoryRemoteRole {
    uint64_t run=0,vehicleGeneration=0,sourceGeneration=0;
    uint32_t phase=0,assignment=0;
    uint8_t player=255;
    uint16_t vehicle=0,source=0;
    game::CVehicle*car=nullptr;
    game::CVehicle*sourceCar=nullptr;
};
// Returns only a current native RC source or its assigned cooperative copy.
bool CooperativeRemoteRole(uint8_t player,StoryRemoteRole&role);
// Retire one failed native RC attempt without offering a fresh copy of it.
bool RetireCooperativeRemoteRole(uint8_t player,uint64_t run,uint32_t phase,uint32_t assignment);
bool CooperativeRemoteMission(const char*name);
// A current mission support may be in mid-air without an ordinary floor.
bool LocalCooperativeRelocationReady(uint16_t sourceVehicle,int8_t area,const game::CVector&point);
void PrepareCooperativeStoryStage();
void PrepareCooperativeRelocation(int8_t area,const game::CVector&position);
void BeginCooperativeInventoryRestore();
void CompleteCooperativeInventoryRestore();
bool CooperativeStoryInventoryReady();
bool CooperativeInventoryParticipant(uint8_t player);
void RetireCooperativeStoryRoles();
bool IsCooperativeRemoteVehicle(game::CVehicle*vehicle,uint8_t player);
game::CPed* CooperativeRemoteController(game::CVehicle*vehicle);
bool IsCooperativeRoleVehicle(game::CVehicle*car);
void CooperativeStoryShot(uint8_t player,int weapon,const game::CVector&start,const game::CVector&end);
void RecordStoryAggressor(game::CPed*ped,uint8_t player);
void ObserveStoryCommand(int opcode,const int32_t*parameters);
uint8_t CooperativeAllyLeader(game::CPed*ped,uint8_t fallback=255);
void RestoreCooperativeAlly(game::CPed*ped,uint8_t leader);
void WriteStoryRewardHistory(Writer&w);
bool ReadStoryRewardHistory(Reader&r);
void StoryProfileReady(bool ready);
void StoryRewardProfileSaved();
// Called at a run boundary, not on every camera change or personal activity.
void ReleaseStoryCargo();
uint8_t SharedStoryObjectHolder(game::CObject*object);
}
