#pragma once
#include "shared/game/Game.h"
#include <string>
namespace coop {
// Called only by the host's positive story predicates, never by private
// activities, inverted conditions or a guest's local script interpreter.
bool CooperativeStoryCondition(int opcode,const int32_t*parameters,void*script=nullptr);
int CooperativeStoryActor(void*script,int opcode,int cjHandle);
// Resolves both pressed and released input for one already selected guest.
bool CooperativeStoryButton(void*script,int player,int button,bool&pressed);
game::CVehicle* CooperativeStoryVehicle(void*script);
void RecordStoryDamage(game::CPed*victim,uint8_t guest,int weapon=54);
void ClearStoryDamage(int handle);
void RecordStoryEntityDamage(game::CEntity*victim,uint8_t player,int weapon,game::CVehicle*sourceVehicle=nullptr);
void ClearStoryEntityDamage(uint8_t kind,int handle,bool clearSource=true);
// Victim-owner fact from native HAS_CAR_BEEN_DAMAGED_BY_CAR. A null source
// records native clearing; contact or impulse alone is not damage evidence.
void RecordStoryVehicleDamage(game::CVehicle*victim,game::CVehicle*source);
// Only the exact original source of an accepted story damage condition may
// continue with its actual guest vehicle in subsequent read-only/task queries.
int CooperativeStorySourceVehicle(void*script,int originalCarHandle);
// Native snatch/pickup animations after a scoped guest object/button action.
// Returns true only when execution was handed to the player's own game.
bool CooperativeStoryAnimation(void*script,int cjHandle,const std::string&name,const std::string&block,
    float blend,int loop,int lockX,int lockY,int freeze,int duration);
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
