// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#include "widgets/overlay/DiagnosticNative.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"

namespace coop::ui::diagnostic {
namespace native_detail {
template<class T> bool Read(const void*base,size_t offset,T&out){
    static_assert(std::is_trivially_copyable_v<T>);
    auto address=(uintptr_t)base;if(!address||offset>UINTPTR_MAX-address||sizeof(T)-1>UINTPTR_MAX-address-offset)return false;
    size_t checked=0;
    while(checked<sizeof(T)){
        uintptr_t pos=address+offset+checked;MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery((void*)pos,&region,sizeof(region))||region.State!=MEM_COMMIT||(region.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        uintptr_t regionBase=(uintptr_t)region.BaseAddress;
        if(pos<regionBase||pos-regionBase>=region.RegionSize)return false;
        checked+=std::min<size_t>(sizeof(T)-checked,region.RegionSize-(pos-regionBase));
    }
    SIZE_T read=0;T value{};
    if(!ReadProcessMemory(GetCurrentProcess(),(const void*)(address+offset),&value,sizeof(value),&read)||read!=sizeof(value))return false;
    std::memcpy(&out,&value,sizeof(value));return true;
}
static_assert(sizeof(void*)==4&&sizeof(game::CPoolHeader)==0x14,"GTA 1.0 US Win32 pool layout");
constexpr uint32_t kMaxPoolSlots=65536; // diagnostic safety cap, not an engine capacity claim
struct PoolSources {uintptr_t ped=game::addr::PedPool,vehicle=game::addr::VehiclePool;};
bool PoolMember(const void*object,uintptr_t global,uint32_t stride,uint8_t expectedType){
    if(!object||(stride!=game::kPedPoolElemSize&&stride!=game::kVehiclePoolElemSize))return false;
    game::CPoolHeader*pointer=nullptr;game::CPoolHeader header{};
    if(!Read((void*)global,0,pointer)||!Read(pointer,0,header)||!header.objects||!header.byteMap||header.size<=0||uint32_t(header.size)>kMaxPoolSlots)return false;
    uintptr_t base=(uintptr_t)header.objects,map=(uintptr_t)header.byteMap,address=(uintptr_t)object;
    const size_t count=(uint32_t)header.size;
    if((base%alignof(void*))||count>(UINTPTR_MAX-base)/stride||count-1>UINTPTR_MAX-map||address<base)return false;
    uintptr_t offset=address-base;if(offset%stride||offset/stride>=count)return false;
    const size_t index=offset/stride;uint8_t flag=0,type=0;
    if(!Read(header.byteMap,index,flag)||(flag&0x80)||!Read(object,0x36,type)||(type&7)!=expectedType)return false;
    // Best-effort stability check only, not lifetime pinning or ABA proof.
    game::CPoolHeader*againPointer=nullptr;game::CPoolHeader again{};uint8_t againFlag=0;
    return Read((void*)global,0,againPointer)&&againPointer==pointer&&Read(pointer,0,again)&&
        again.objects==header.objects&&again.byteMap==header.byteMap&&again.size==header.size&&
        Read(again.byteMap,index,againFlag)&&againFlag==flag;
}
bool PedValid(game::CPed*p,uintptr_t global=game::addr::PedPool){return PoolMember(p,global,game::kPedPoolElemSize,game::ENTITY_TYPE_PED);}
bool VehicleValid(game::CVehicle*v,uintptr_t global=game::addr::VehiclePool){return PoolMember(v,global,game::kVehiclePoolElemSize,game::ENTITY_TYPE_VEHICLE);}
bool Finite(const game::CVector&p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
bool Position(const void*object,game::CVector&out){
    game::CMatrix*matrix=nullptr;if(!Read(object,0x14,matrix))return false;
    return (matrix?Read(matrix,offsetof(game::CMatrix,pos),out):Read(object,0x04,out))&&Finite(out);
}
std::string Number(double value,bool known=true){if(!known||!std::isfinite(value))return "UNKNOWN";char text[40];std::snprintf(text,sizeof(text),"%.6g",value);return text;}
std::string Vector(const game::CVector&p,bool known){if(!known||!Finite(p))return "UNKNOWN";return Number(p.x)+" "+Number(p.y)+" "+Number(p.z);}
bool InVehicle(game::CPed*p,game::CVehicle*&vehicle,bool&in){
    uint32_t flags=0;if(!Read(p,0x46C,flags))return false;in=(flags&(1u<<8))!=0;vehicle=nullptr;
    return !in||Read(p,0x58C,vehicle);
}
bool Seat(game::CVehicle*v,game::CPed*p,int&seat){
    game::CPed*driver=nullptr;if(!Read(v,0x460,driver))return false;if(driver==p){seat=-1;return true;}
    game::CPed*passengers[8]{};if(!Read(v,0x464,passengers))return false;
    for(int n=0;n<8;++n)if(passengers[n]==p){seat=n;return true;}return false;
}
bool Task(game::CPed*,int&){return false;} // no validated data-only type mapping; never invoke native/virtual code
void CarRows(NativeSnapshot&out,game::CPed*me,uint8_t peer,const PoolSources&pools={}){
    if(!PedValid(me,pools.ped)){out.Add(NativeKind::LocalCar,peer,false,0,"my car: UNKNOWN (local ped absent/invalid)");return;}
    game::CVehicle*v=nullptr;bool in=false;
    bool vehicleRead=InVehicle(me,v,in);
    if(!vehicleRead||!in||!VehicleValid(v,pools.vehicle)){out.Add(NativeKind::LocalCar,peer,false,0,"my car: UNKNOWN (%s)",!vehicleRead?"vehicle reference read failed":!in?"ped not in vehicle":"vehicle absent/invalid");return;}
    uint16_t model=0;uint8_t status=0,passengers=0,getIn=0,getInFlags=0,getOut=0,wheels=0;
    uint32_t flagsLo=0,flagsHi=0;float gas=0;game::CVector velocity{},position{};game::CPed*driver=nullptr;
    bool basic=Read(v,0x22,model)&&Read(v,0x36,status)&&Read(v,0x428,flagsLo)&&Read(v,0x460,driver)&&Read(v,0x484,passengers);
    bool gasKnown=Read(v,0x49C,gas)&&std::isfinite(gas),speedKnown=Read(v,0x44,velocity)&&Finite(velocity);
    double speed=speedKnown?std::sqrt(double(velocity.x)*velocity.x+double(velocity.y)*velocity.y+double(velocity.z)*velocity.z)*50:0;
    int seat=0,task=0;bool seatKnown=Seat(v,me,seat),taskKnown=Task(me,task);
    const char*driverText=!basic?"UNKNOWN":!driver?"none":driver==me?"me":PedValid(driver,pools.ped)?"other":"UNKNOWN";
    if(basic)out.Add(NativeKind::LocalCar,peer,false,0,"my car: model %u status %u engine %u driver %s gas %s speed %s seat %s task %s pass %u",model,status>>3,(flagsLo>>4)&1,driverText,Number(gas,gasKnown).c_str(),Number(speed,speedKnown).c_str(),Number(seat,seatKnown).c_str(),Number(task,taskKnown).c_str(),passengers);
    else out.Add(NativeKind::LocalCar,peer,false,0,"my car: model/status/engine/driver/pass UNKNOWN (read failed); gas %s speed %s seat %s task %s",Number(gas,gasKnown).c_str(),Number(speed,speedKnown).c_str(),Number(seat,seatKnown).c_str(),Number(task,taskKnown).c_str());
    bool flagsKnown=Read(v,0x428,flagsLo)&&Read(v,0x42C,flagsHi)&&Read(v,0x485,getIn)&&Read(v,0x486,getInFlags)&&Read(v,0x487,getOut)&&Read(v,0x961,wheels);
    bool posKnown=Position(v,position);
    if(flagsKnown)out.Add(NativeKind::LocalCar,peer,false,0,"car flags %08X %08X wheelsOnGround %u getIn %u/%02X getOut %02X pos %s",flagsLo,flagsHi,wheels,getIn,getInFlags,getOut,Vector(position,posKnown).c_str());
    else out.Add(NativeKind::LocalCar,peer,false,0,"car flags/entry/wheels UNKNOWN; pos %s",Vector(position,posKnown).c_str());
    game::CControllerState pad{};bool padKnown=Read(game::GetPad(0),0,pad);
    if(padKnown)out.Add(NativeKind::LocalCar,peer,false,0,"pad cross %d LX %d LY %d RS1 %d Sq %d",pad.ButtonCross,pad.LeftStickX,pad.LeftStickY,pad.RightShoulder1,pad.ButtonSquare);
    else out.Add(NativeKind::LocalCar,peer,false,0,"pad: UNKNOWN");
}
void RemoteRows(NativeSnapshot&out,uint8_t peer,const RemotePlayer&rp,game::CPed*me,const PoolSources&pools={}){
    const bool valid=PedValid(rp.ped,pools.ped);game::CVector position{},local{};
    bool posKnown=valid&&Position(rp.ped,position),localKnown=PedValid(me,pools.ped)&&Position(me,local);
    double distance=posKnown&&localKnown?std::sqrt(std::pow(double(position.x)-local.x,2)+std::pow(double(position.y)-local.y,2)+std::pow(double(position.z)-local.z,2)):0;
    int task=0,seat=0;bool taskKnown=valid&&Task(rp.ped,task),in=false;game::CVehicle*vehicle=nullptr;
    bool vehicleKnown=valid&&InVehicle(rp.ped,vehicle,in),seatKnown=vehicleKnown&&in&&VehicleValid(vehicle,pools.vehicle)&&Seat(vehicle,rp.ped,seat);
    bool ref=rp.hasState&&rp.mode!=RemotePlayer::Mode::OnFoot&&rp.vehNetId!=0;
    char reference[24];if(rp.hasState)std::snprintf(reference,sizeof(reference),"%04X",rp.vehNetId);else std::snprintf(reference,sizeof(reference),"UNKNOWN");
    out.Add(NativeKind::Remote,peer,ref,rp.vehNetId,"remote %u: ped %s pos %s net %s dist %s mode %s veh %s seat %s here %s task %s",peer,valid?"valid":"UNKNOWN",Vector(position,posKnown).c_str(),Vector(rp.state.pos,rp.hasState&&Finite(rp.state.pos)).c_str(),Number(distance,posKnown&&localKnown).c_str(),Number((int)rp.mode,rp.hasState).c_str(),reference,Number(rp.seat,rp.hasState).c_str(),!vehicleKnown?"UNKNOWN":!in?"none":Number(seat,seatKnown).c_str(),Number(task,taskKnown).c_str());
}
}

bool CaptureNativeSnapshot(NativeSnapshot&out,uint32_t owningThread){
    if(!owningThread||owningThread!=GetCurrentThreadId())return false;
    out={};auto&net=Net();auto&transport=net.Net();
    out.Add(NativeKind::Network,255,false,0,"net in %llu KB / out %llu KB, state %d, phase %d",(unsigned long long)(transport.BytesIn()/1024),(unsigned long long)(transport.BytesOut()/1024),(int)net.GetState(),(int)flow::GetPhase());
    int32_t state=0;game::CPlayerPed*me=nullptr;
    if(!native_detail::Read((void*)game::addr::GameState,0,state)||state!=game::GAME_STATE_IDLE||!native_detail::Read((void*)game::addr::Players,0,me)||!me){out.Add(NativeKind::LocalCar,255,false,0,"Native car/remote state UNKNOWN (player not ready)");return true;}
    native_detail::CarRows(out,me,net.MyId());
    size_t peers=0;for(auto&[peer,rp]:Players::Instance().All()){
        if(peers++==2){out.truncated=true;break;}native_detail::RemoteRows(out,peer,rp,me);
    }
    return true;
}
}
