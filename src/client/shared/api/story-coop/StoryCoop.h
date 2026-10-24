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
void SetCooperativeStoryCamera(bool mandatory);
uint8_t CooperativeReservedSeats(game::CVehicle*car);
uint8_t LocalStoryRoleMode();
void PrepareCooperativeStoryStage();
void PrepareCooperativeRelocation(int8_t area,const game::CVector&position);
void BeginCooperativeInventoryRestore();
void CompleteCooperativeInventoryRestore();
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
