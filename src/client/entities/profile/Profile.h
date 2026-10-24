#pragma once
// The partner's profile on the host's server: money, skills and look of its own. The progress feature
// (features/progress) keeps it and answers this.

namespace coop {

// partner: send the profile (money, skills, look) now and keep it for the next world load
void SaveProfileNow(bool always = false);

}  // namespace coop
