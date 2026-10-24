#pragma once
#include <cmath>
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
namespace coop {
inline bool loadingSharedSoundtrack = false;
struct AudioTarget {
    bool placed = false;
    uint8_t kind = 0; // point, player, NPC, vehicle
    uint16_t id = 0;
    Vec3 position;
    uint64_t generation=0;
    void Write(Writer& w) const { w.Bool(placed); w.U8(kind); if(kind==2)w.Entity(ENTITY_PED,id,ENTITY_OPTIONAL,255,generation);else if(kind==3)w.Entity(ENTITY_VEHICLE,id,ENTITY_OPTIONAL,255,generation);else w.U16(id); w.Vec(position); }
    bool Read(Reader& r) {
        placed=r.Bool();kind=r.U8();id=r.U16();position=r.Vec();if(kind==2||kind==3)generation=Net().ExpectedEntityGeneration(kind==2?ENTITY_PED:ENTITY_VEHICLE,id);
        return r.Ok() && kind<=3 && std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
    }
    static AudioTarget Describe(game::CEntity* entity, const game::CVector& point) {
        AudioTarget t; t.placed=true;t.position={point.x,point.y,point.z};
        if (!entity) return t;
        // Object-only script effects pass the sentinel {-1000,-1000,-1000}.
        // If no network identity exists, replay at the real physical position.
        auto actual=entity->GetPosition();t.position={actual.x,actual.y,actual.z};
        if(entity==game::FindPlayerPed(0)){t.kind=1;t.id=Net().MyId();}
        else if(entity->IsPed()) {
            if(auto* p=Players::Instance().FindByPed((game::CPed*)entity)){t.kind=1;t.id=p->id;}
            else if(auto* p=Peds::Instance().Find((game::CPed*)entity)){t.kind=2;t.id=p->netId;}
        } else if(entity->IsVehicle())
            if(auto* v=Vehicles::Instance().EnsureNetworked((game::CVehicle*)entity)){t.kind=3;t.id=v->netId;}
        if(t.kind==2||t.kind==3)t.generation=Net().EntityGeneration(t.kind==2?ENTITY_PED:ENTITY_VEHICLE,t.id);
        return t;
    }
    game::CPhysical* Resolve() const {
        if((kind==2||kind==3)&&(!generation||Net().EntityGeneration(kind==2?ENTITY_PED:ENTITY_VEHICLE,id)!=generation||!Net().EntityLive(kind==2?ENTITY_PED:ENTITY_VEHICLE,id)))return nullptr;
        if(kind==1) {
            if(id==Net().MyId())return game::FindPlayerPed(0);
            auto* p=Players::Instance().Get((uint8_t)id);return p?p->ped:nullptr;
        }
        if(kind==2)return game::GetPed(Peds::Instance().HandleOf(id));
        if(kind==3){auto* v=Vehicles::Instance().Find(id);return v&&game::IsVehicleValid(v->veh)?v->veh:nullptr;}
        return nullptr;
    }
};
}
