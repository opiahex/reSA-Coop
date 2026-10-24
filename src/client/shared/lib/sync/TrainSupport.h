#pragma once
#include <map>
#include "shared/game/Game.h"
namespace coop {
struct TrainSupport {game::CVehicle*vehicle=nullptr;int handle=-1;};
inline std::map<uint16_t,TrainSupport>&TrainSupports(){static std::map<uint16_t,TrainSupport>map;return map;}
inline uint16_t TrainSupportKey(uint8_t owner,uint16_t train,uint8_t carriage){return (uint16_t)(0x8000u|((owner+1u)<<12)|((train&255u)<<4)|(carriage&15u));}
inline game::CVehicle* ResolveTrainSupport(uint16_t id){auto it=TrainSupports().find(id);if(it==TrainSupports().end())return nullptr;auto&s=it->second;return game::IsVehicleValid(s.vehicle)&&game::GetVehicleRef(s.vehicle)==s.handle?s.vehicle:nullptr;}
inline uint16_t TrainSupportOf(game::CVehicle*v){for(auto&[id,s]:TrainSupports())if(s.vehicle==v&&ResolveTrainSupport(id))return id;return 0;}
}
