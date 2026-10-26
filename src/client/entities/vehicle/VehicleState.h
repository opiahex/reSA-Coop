#pragma once
#include <cstdint>

#include "common/lib/buffer/Buffer.h"
#include "shared/game/Game.h"
#include "entities/vehicle/WheelState.h"
#include "shared/lib/sync/Quat.h"
#include "shared/lib/sync/TrainSupport.h"

namespace coop {

struct VehicleState {
    uint32_t time = 0;
    game::CVector pos;
    Quat rot;
    game::CVector vel;   // m/s
    game::CVector turn;  // rad/s
    float steer = 0;     // radians
    float gas = 0;       // -1..1
    float brake = 0;     // 0..1
    float health = 1000;
    uint8_t flags = 0;   // F_*
    uint8_t water=0; // physical submerged / occupants drowning, owner decides
    uint8_t lock = 0;
    int8_t area=0;

    enum : uint8_t { F_ENGINE = 1, F_LIGHTS = 2, F_SIREN = 4, F_HORN = 8, F_HANDBRAKE = 16, F_WRECKED = 32, F_HIDDEN = 64, F_FAST_SIREN = 128 };
    // parts the driver moves; X_* says which this vehicle has
    uint8_t extras = 0;
    uint16_t misc = 0;  // X_MISC, CAutomobile::m_wMiscComponentAngle: Hydra nozzles, raised hydraulics, tow hoist, forks
    float gear = 0;     // X_GEAR, CPlane::m_fLandingGearStatus: 0 down, 1 up, negative while going down
    float turretYaw = 0, turretPitch = 0;  // X_TURRET: Rhino, SWAT van, fire truck
    enum : uint8_t { X_MISC = 1, X_GEAR = 2, X_TURRET = 4, X_SEARCHLIGHT = 8, X_ROTOR = 16, X_WHEELS = 32 };
    WheelState wheels;
    game::CVector searchTarget;
    float searchIntensity = 0;
    float rotorSpeed = 0; // CHeli: native rotor spool, separate from engine-on bit
    uint16_t towChild=0,towParent=0;
    TrainAttachment train;game::CVector trainOffset;Quat trainRotation;
    // Physical car-to-car binding (0683), distinct from a tow hitch or train.
    uint16_t parentVehicle=0;uint64_t parentGeneration=0;
    game::CVector parentOffset;Quat parentRotation;
    bool parentUsesEuler=false;game::CVector parentEuler;
    // A host native attach/detach fences older driver snapshots on both
    // channels without changing the actual driver's simulation ownership.
    uint64_t attachmentRun=0;uint32_t attachmentPhase=0,attachmentRevision=0;
    bool AttachmentInStory(uint64_t run,uint32_t phase)const {
        return !attachmentRevision||(attachmentRun==run&&attachmentPhase==phase);
    }
    bool FollowsAttachmentClaim(const VehicleState&claim)const {
        return !claim.attachmentRevision||(attachmentRun==claim.attachmentRun&&attachmentPhase==claim.attachmentPhase&&
               (int32_t)(attachmentRevision-claim.attachmentRevision)>=0);
    }

    static VehicleState Capture(game::CVehicle* v);
    void Write(Writer& w) const;
    bool Read(Reader& r);
};

// Static description used to create the vehicle on the other side.
struct VehicleSpawn {
    uint16_t netId = 0;
    uint16_t model = 0;
    uint8_t owner = 0;
    game::CVector pos;
    Quat rot;
    uint8_t colors[4] = {};
    int8_t components[2] = {-1,-1}; // optional model extras (cargo, roof, spare wheel)
    float health = 1000;
    int8_t area = 0;
    uint8_t lockState = 0;
    bool playerUsed = false;  // stable identity while parked, including across interior/world transitions
    bool script = false;  // a script's car in its owner's game (a mission car): never an ordinary car elsewhere

    void Write(Writer& w) const;
    bool Read(Reader& r);
};

}  // namespace coop
