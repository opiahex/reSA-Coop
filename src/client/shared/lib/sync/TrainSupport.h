#pragma once
#include <map>
#include "common/lib/buffer/Buffer.h"
#include "shared/game/Game.h"
namespace coop {
struct TrainSupport {game::CVehicle*vehicle=nullptr;int handle=-1;uint64_t generation=0;};
struct TrainAttachment {
    uint16_t id=0;uint8_t owner=255,carriage=0;uint64_t generation=0;
    bool Valid()const{return id>0&&id<=255&&owner<3&&carriage<16&&generation;}
    void Write(Writer&w)const{w.Entity(ENTITY_TRAIN,id,ENTITY_USE|ENTITY_OPTIONAL,owner,generation);w.U8(owner);w.U8(carriage);w.U64(generation);}
    bool Read(Reader&r){id=r.U16();owner=r.U8();carriage=r.U8();generation=r.U64();return r.Ok()&&(Valid()||(!id&&owner==255&&!carriage&&!generation));}
};
// Attachments retain the train's incarnation, not just its reusable support key.
bool DescribeTrainAttachment(game::CVehicle*vehicle,TrainAttachment&out);
game::CVehicle* ResolveTrainAttachment(const TrainAttachment&attachment);
inline std::map<uint16_t,TrainSupport>&TrainSupports(){static std::map<uint16_t,TrainSupport>map;return map;}
inline uint16_t TrainSupportKey(uint8_t owner,uint16_t train,uint8_t carriage){return (uint16_t)(0x8000u|((owner+1u)<<12)|((train&255u)<<4)|(carriage&15u));}
inline game::CVehicle* ResolveTrainSupport(uint16_t id){auto it=TrainSupports().find(id);if(it==TrainSupports().end())return nullptr;auto&s=it->second;return game::IsVehicleValid(s.vehicle)&&game::GetVehicleRef(s.vehicle)==s.handle?s.vehicle:nullptr;}
inline uint16_t TrainSupportOf(game::CVehicle*v){for(auto&[id,s]:TrainSupports())if(s.vehicle==v&&ResolveTrainSupport(id))return id;return 0;}
}
