#pragma once
namespace coop {
// Only the guest's own school interpreters borrow personal records.
// The shared world's story unlock flags are restored before another script runs.
bool BeginPersonalDrivingSchool(void* script);
void EndPersonalDrivingSchool();
// A live native minigame or local personal mission owns its attempt, camera,
// vehicle and timers until native cleanup. It must finish before world refresh
// or a newly received cooperative assignment takes control of the player.
bool PersonalActivityAttemptActive();
}
