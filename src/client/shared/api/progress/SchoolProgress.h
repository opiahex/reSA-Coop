#pragma once
namespace coop {
// Only the guest's own DSKOOL/TRACE interpreter borrows personal records.
// The shared world's story unlock flags are restored before another script runs.
bool BeginPersonalDrivingSchool(void* script);
void EndPersonalDrivingSchool();
}
