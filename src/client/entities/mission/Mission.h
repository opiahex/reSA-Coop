#pragma once
// The host's mission as the rest of the game sees it. The story feature (features/story/missions) runs the missions
// and answers these.
#include <cstdint>

namespace coop {

// partner: the host's mission runs, or ended less than graceMs ago
bool HostMissionRecent(uint64_t graceMs);

// host: its mission (or the mission's trigger) runs and a partner is there to see it
bool HostReplicating();
bool LocalSchoolActivity();

// the script (CRunningScript) whose commands run right now, or null
void* CurrentScript();
void ReleaseStoryScriptCamera();
// Object referenced/created by the authoritative story interpreter.
void ShareMissionObject(int handle);
bool StoryGroupReady(float x, float y, float z, int8_t area, float radius = 4.0f, float vertical = 4.0f);

}  // namespace coop
