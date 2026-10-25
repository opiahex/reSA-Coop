#pragma once
namespace game {struct CPed;struct CVehicle;}
namespace coop {
// A shared population source must also see the wanted guest beside it.
// Only this synchronous generation call borrows the guest's native context.
void GeneratePoliceAwareTraffic(void(*generate)());
void ProcessPoliceVehicle(game::CVehicle* vehicle,void(*process)(game::CVehicle*));
// One native intelligence hook supplies both personal police targeting and
// abseil frame tracking. Off-area SWAT must use the same wanted context.
void ObservePoliceRopeIntelligence(game::CPed* ped);
void ProcessPolicePedIntelligence(game::CPed* ped);
}
