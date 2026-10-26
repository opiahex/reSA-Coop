#include "entities/vehicle/VehicleState.h"

#include <algorithm>
#include <cmath>

namespace coop {

using game::CMatrix;
using game::CVector;

static Vec3 V(const CVector& v) { return {v.x, v.y, v.z}; }
static CVector G(const Vec3& v) { return {v.x, v.y, v.z}; }

VehicleState VehicleState::Capture(game::CVehicle* v) {
    VehicleState s;
    s.pos = v->GetPosition();
    s.lock = (uint8_t)v->DoorLock();
    s.area=v->AreaCode();
    if (game::CMatrix* m = v->Matrix()) s.rot = Quat::FromMatrix(*m);
    s.vel = v->MoveSpeed() * 50.0f;
    s.turn = v->TurnSpeed() * 50.0f;
    s.steer = v->SteerAngle();
    s.gas = v->GasPedal();
    s.brake = v->BrakePedal();
    s.health = v->Health();s.water=((v->PhysicalFlags()&0x100u)?1:0)|((v->VehicleFlagsLo()&0x40000000u)?2:0);
    s.flags = (v->EngineOn() ? F_ENGINE : 0) | (v->LightsOn() ? F_LIGHTS : 0) | (v->SirenOn() ? F_SIREN : 0) |
              (v->Status() == 5 ? F_WRECKED : 0) | (v->HandbrakeOn() ? F_HANDBRAKE : 0) | (!v->IsVisible() ? F_HIDDEN : 0);
    s.wheels=WheelState::Capture(v);
    if(s.wheels.count)s.extras|=X_WHEELS;
    bool localDriver = v->Driver() == game::FindPlayerPed(0);
    if (localDriver ? game::GetPad(0)->NewState.ShockButtonL != 0 : game::Field<uint32_t>(v, 0x514) != 0)
        s.flags |= v->SirenOn() ? F_FAST_SIREN : F_HORN;
    int type = v->VehicleSubClass();
    if (type <= game::VEHICLE_TYPE_PLANE || type == game::VEHICLE_TYPE_TRAILER) {  // CAutomobile and its subclasses
        s.extras |= X_MISC;
        s.misc = game::Field<uint16_t>(v, 0x86C);
    }
    if (type == game::VEHICLE_TYPE_PLANE) {
        s.extras |= X_GEAR;
        s.gear = game::Field<float>(v, 0x9CC);
    }
    int model = v->ModelIndex();
    if (type == game::VEHICLE_TYPE_HELI) {
        s.extras |= X_SEARCHLIGHT | X_ROTOR;
        s.rotorSpeed=std::clamp(game::Field<float>(v,0x84C),0.0f,5.0f);
        s.searchTarget = game::Field<CVector>(v, 0x9F4);
        s.searchIntensity = std::clamp(game::Field<float>(v, 0xA00), 0.0f, 1.0f);
    }
    if (model == 432 || model == 601 || model == 407) {
        s.extras |= X_TURRET;
        s.turretYaw = game::Field<float>(v, 0x94C);   // CAutomobile::m_fDoomVerticalRotation
        s.turretPitch = game::Field<float>(v, 0x950);  // m_fDoomHorizontalRotation
    }
    if(auto*parent=v->AttachedTo();parent&&parent->IsVehicle()&&((game::CVehicle*)parent)->IsTrain()&&DescribeTrainAttachment((game::CVehicle*)parent,s.train)){
        s.trainOffset=game::Field<CVector>(v,0x100);s.trainRotation=game::Field<Quat>(v,0x118);
    }
    return s;
}

void VehicleState::Write(Writer& w) const {
    w.U32(time);
    w.Entity(ENTITY_VEHICLE,towChild);w.Entity(ENTITY_VEHICLE,towParent);
    w.Vec(V(pos));
    rot.Write(w);
    w.VecQ(V(vel), 150.0f);
    w.VecQ(V(turn), 30.0f);
    w.I8((int8_t)std::clamp(steer / 1.0f * 127.0f, -127.0f, 127.0f));
    w.SUnit8(gas);
    w.Unit8(brake);
    w.U16((uint16_t)std::clamp(health, 0.0f, 65535.0f));
    w.U8(flags);w.U8(water);
    w.U8(lock);
    w.I8(area);
    w.U8(extras);
    if (extras & X_MISC) w.U16(misc);
    if (extras & X_GEAR) w.I16(Writer::Quant(gear, 1.0f));
    if (extras & X_TURRET) {
        w.I16(Writer::Quant(turretYaw, 3.2f));
        w.I16(Writer::Quant(turretPitch, 3.2f));
    }
    if (extras & X_SEARCHLIGHT) { w.Vec(V(searchTarget)); w.Unit8(searchIntensity); }
    if(extras & X_ROTOR)w.F32(rotorSpeed);
    if(extras & X_WHEELS)wheels.Write(w);
    w.Bool(train.Valid());if(train.Valid()){train.Write(w);w.Vec(V(trainOffset));trainRotation.Write(w);}
    w.U32(attachmentRevision);if(attachmentRevision){w.U64(attachmentRun);w.U32(attachmentPhase);}
    w.Entity(ENTITY_VEHICLE,parentVehicle,ENTITY_USE|ENTITY_OPTIONAL,255,parentGeneration);
    if(parentVehicle){w.U64(parentGeneration);w.Vec(V(parentOffset));w.Bool(parentUsesEuler);
        if(parentUsesEuler)w.Vec(V(parentEuler));else parentRotation.Write(w);}
}

bool VehicleState::Read(Reader& r) {
    time = r.U32();
    towChild=r.U16();towParent=r.U16();
    pos = G(r.Vec());
    rot.Read(r);
    vel = G(r.VecQ(150.0f));
    turn = G(r.VecQ(30.0f));
    steer = r.I8() / 127.0f;
    gas = r.SUnit8();
    brake = r.Unit8();
    health = (float)r.U16();
    flags = r.U8();water=r.U8();if(water>3)return false;
    lock = r.U8();
    area=r.I8();
    extras = r.U8();
    if (extras & X_MISC) misc = r.U16();
    if (extras & X_GEAR) gear = r.I16() / 32767.0f;
    if (extras & X_TURRET) {
        turretYaw = r.I16() / 32767.0f * 3.2f;
        turretPitch = r.I16() / 32767.0f * 3.2f;
    }
    if (extras & X_SEARCHLIGHT) { searchTarget = G(r.Vec()); searchIntensity = r.Unit8(); }
    if(extras & X_ROTOR){r.Raw(&rotorSpeed,4);if(!std::isfinite(rotorSpeed)||rotorSpeed<0||rotorSpeed>5)return false;}
    if((extras & X_WHEELS)&&!wheels.Read(r))return false;
    if(r.Bool()){if(!train.Read(r)||!train.Valid())return false;trainOffset=G(r.Vec());trainRotation.Read(r);if(!(trainOffset.Length()<100))return false;}else train={};
    attachmentRevision=r.U32();attachmentRun=attachmentRevision?r.U64():0;attachmentPhase=attachmentRevision?r.U32():0;
    if(attachmentRevision&&!attachmentRun)return false;
    parentVehicle=r.U16();parentGeneration=0;parentOffset={};parentRotation={};parentUsesEuler=false;parentEuler={};
    if(parentVehicle){parentGeneration=r.U64();parentOffset=G(r.Vec());parentUsesEuler=r.Bool();
        if(parentUsesEuler)parentEuler=G(r.Vec());else parentRotation.Read(r);
        if(!parentGeneration||parentVehicle==0xFFFF||train.Valid()||!(parentOffset.Length()<128.f)||!(parentEuler.Length()<1000.f))return false;}
    return r.Ok();
}

void VehicleSpawn::Write(Writer& w) const {
    w.Entity(ENTITY_VEHICLE,netId,ENTITY_BIRTH|ENTITY_SNAPSHOT);
    w.U16(model);
    w.U8(owner);
    w.Vec(V(pos));
    rot.Write(w);
    w.Raw(colors, 4);
    for(auto c:components)w.I8(c);
    w.U16((uint16_t)std::clamp(health, 0.0f, 65535.0f));
    w.I8(area);
    w.U8(lockState);
    w.U8(script);
    w.Bool(playerUsed);
}

bool VehicleSpawn::Read(Reader& r) {
    netId = r.U16();
    model = r.U16();
    owner = r.U8();
    pos = G(r.Vec());
    rot.Read(r);
    r.Raw(colors, 4);
    for(auto&c:components){c=r.I8();if(c < -1 || c > 5)return false;}
    health = (float)r.U16();
    area = r.I8();
    lockState = r.U8();
    script = r.U8() != 0;
    playerUsed = r.Bool();
    return r.Ok();
}

}  // namespace coop
