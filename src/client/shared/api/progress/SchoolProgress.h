#pragma once
namespace coop {
// Only the guest's own school interpreters borrow personal records.
// The shared world's story unlock flags are restored before another script runs.
bool BeginPersonalDrivingSchool(void* script);
void EndPersonalDrivingSchool();
}
