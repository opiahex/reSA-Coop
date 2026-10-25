#pragma once
#include "shared/game/Game.h"
namespace coop {
// Called only by the host's positive story predicates, never by private
// activities, inverted conditions or a guest's local script interpreter.
bool CooperativeStoryCondition(int opcode,const int32_t*parameters,void*script=nullptr);
int CooperativeStoryActor(void*script,int opcode,int cjHandle);
game::CVehicle* CooperativeStoryVehicle(void*script);
void RecordStoryDamage(game::CPed*victim,uint8_t guest,int weapon=54);
void ClearStoryDamage(int handle);
void RecordStoryEntityDamage(game::CEntity*victim,uint8_t player,int weapon);
void ClearStoryEntityDamage(uint8_t kind,int handle);
// Candidate after native CPhysical processing; the local driver verifies
// stock touching after physics before the receipt is sent.
void RecordStoryVehicleObjectContact(game::CVehicle*vehicle,game::CObject*object);
// Read-only speed of the exact car selected by a fresh contact continuation.
bool CooperativeStoryContactSpeed(void*script,int carHandle,float&speed);
void RecordStoryKill(game::CPed*victim,uint8_t player);
void ResetCooperativeKills();
int CooperativeGuestKills(int model);
void ClearCooperativeStoryScript(void*script);
bool CooperativePhotoFov(float&fov);
}
