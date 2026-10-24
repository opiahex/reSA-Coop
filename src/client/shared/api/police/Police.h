#pragma once
namespace game {struct CVehicle;}
namespace coop {
// A shared population source must also see the wanted guest beside it.
// Only this synchronous generation call borrows the guest's native context.
void GeneratePoliceAwareTraffic(void(*generate)());
void ProcessPoliceVehicle(game::CVehicle* vehicle,void(*process)(game::CVehicle*));
}
