// The safe houses' garages, which keep the cars left in them, belong to the host's world. What they keep is part of
// the host's game and its save, and the partner's game loads that world again and again (story progress), so the
// partner's game neither puts cars away nor brings any out: both games doing it made every kept car twice, and a car
// the partner had put away was gone with the next world. The partner's game asks the host's instead. When a garage
// closes behind a car of the partner's, the host's game keeps it (as it would its own) and the partner's lets it go.
// When a garage opens for the partner, the host's game hands over what it keeps there and the partner's brings the
// cars out (where the partner is, the ground is loaded). The host's snapshots of its world for the partners are saves too: they no longer slam an open garage
// shut on the cars in it.
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/lib/sync/GarageDoor.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

constexpr uintptr_t kGarages = 0x96C048, kNumGarages = 0x96C024;  // CGarages::aGarages (CGarage 0xD8 each), NumGarages
constexpr uintptr_t kStoredCars = 0x96ABD8;                         // CGarages::aCarsInSafeHouse: 4 CStoredCar (0x40) a house
constexpr size_t kGarageSize = 0xD8;
enum : uint8_t { GARAGE_STORE = 1, GARAGE_KEPT = 2, GARAGE_BRING_OUT = 3, GARAGE_CARS = 4, GARAGE_BACK = 5 };
constexpr size_t kStoredCarSize = 0x40;

uint8_t TypeOf(void* g) { return Field<uint8_t>(g, 0x4C); }  // CGarage::m_nType
bool IsHideout(uint8_t t) { return (t >= 16 && t <= 18) || (t >= 24 && t <= 32) || (t >= 39 && t <= 42) || t == 45; }
void* GarageAt(int i) {
    return i >= 0 && i < 50 && i < Ref<int32_t>(kNumGarages) ? (void*)(kGarages + (uintptr_t)i * kGarageSize) : nullptr;
}
uint8_t IndexOf(void* g) { return (uint8_t)(((uintptr_t)g - kGarages) / kGarageSize); }
std::string NameOf(void* g) { return std::string(Field<char[8]>(g, 0x44), strnlen(Field<char[8]>(g, 0x44), 8)); }
uint8_t* StoreOf(void* g) {  // CGarages::FindSafeHouseIndexForGarageType
    return (uint8_t*)(kStoredCars + (uintptr_t)Call<0x4489F0, int>((int)TypeOf(g)) * 0x100);
}
bool Inside(void* g, const CVector& p) { return CallMethod<0x448740, bool>(g, p); }  // CGarage::IsPointInsideGarage

// the partner's game in the host's world: its garages keep nothing of their own
bool Partner() { return Net().Online() && !Net().IsHost() && flow::InHostWorld(); }

struct StoredReceipt {uint8_t owner=255,index=0;int handle=-1;};
std::map<uint16_t,StoredReceipt>g_storedReceipts;

bool Occupied(CVehicle* v) {
    if (v->Driver()) return true;
    for (int i = 0; i < 8; ++i)
        if (v->Passengers()[i]) return true;
    return false;
}

// what CGarage::StoreAndRemoveCarsForThisHideOut would put away of ours goes to the host's store; our cars stay here
// until the host has them
void AskToKeep(void* garage) {
    if (!core::IsPlayerReady()) return;  // a world loading: nothing of ours in it
    CPoolHeader* pool = VehiclePool();
    std::vector<uint16_t> ids;
    for (int i = 0; pool && i < pool->size && ids.size() < 4; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        NetVehicle* nv = Vehicles::Instance().Find(v);
        if (nv && !Vehicles::Instance().IsMirror(*nv) && !Occupied(v) && Inside(garage, v->GetPosition())) ids.push_back(nv->netId);
    }
    if (ids.empty()) return;
    Writer w(proto::G_GARAGE);
    w.U8(GARAGE_STORE);
    w.U8(IndexOf(garage));
    w.U32(Net().WorldEpoch());
    w.U8((uint8_t)ids.size());
    for (uint16_t id : ids) w.Entity(ENTITY_VEHICLE,id);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Garage %s: %zu car(s) of ours go to the host's store", NameOf(garage).c_str(), ids.size());
}

using StoreFn = void(__fastcall*)(void*, void*, void*, int);
StoreFn g_origStoreClosing = nullptr, g_origStoreBeforeSave = nullptr;
void __fastcall HookStoreClosing(void* garage, void* edx, void* stored, int maxSlot) {  // the door closed (CGarage::Update)
    if (!Partner()) return g_origStoreClosing(garage, edx, stored, maxSlot);
    AskToKeep(garage);
}
void __fastcall HookStoreBeforeSave(void* garage, void* edx, void* stored, int maxSlot) {  // CloseHideOutGaragesBeforeSave
    if (!Partner()) return g_origStoreBeforeSave(garage, edx, stored, maxSlot);
    AskToKeep(garage);
}

using RestoreFn = bool(__fastcall*)(void*, void*, void*);
// the game's own until redirected: a site another mod changed is left alone, and this is still called
RestoreFn g_origRestore = (RestoreFn)0x448550;
bool __fastcall HookRestore(void* garage, void* edx, void* stored) {  // CGarage::RestoreCarsForThisHideOut
    if (!Partner()) return g_origRestore(garage, edx, stored);
    if (core::IsPlayerReady()) {
        Writer w(proto::G_GARAGE);
        w.U8(GARAGE_BRING_OUT);
        w.U8(IndexOf(garage));w.U32(Net().WorldEpoch());
        Net().Send(proto::CH_RELIABLE, w);
    }
    return true;  // nothing to wait for here: the door opens (CGarage::Update asks until all cars are out)
}

using CloseFn = void(__cdecl*)();
CloseFn g_origCloseBeforeSave = nullptr;
void __cdecl HookCloseBeforeSave() {  // CGarages::Save
    if (core::SavingSnapshot()) return;  // the partners' copy of the world: an open garage stays open with its cars
    g_origCloseBeforeSave();
}

// What CStoredCar::RestoreCar (0x447E40) can make here: it asks the streamer for the model and every part unchecked (a
// model of a mod this game lacks crashes it), and CVehicle::SetRemap reads the model's four paintjobs at the index.
bool Restorable(const uint8_t* car) {
    if (!IsVehicleModel(Ref<uint16_t>((uintptr_t)car + 0x12))) return false;  // m_wModelIndex
    for (int i = 0; i < 15; ++i) {  // m_awCarMods, -1 for none
        int part = Ref<int16_t>((uintptr_t)car + 0x14 + (uintptr_t)i * 2);
        if (part != -1 && !IsValidModel(part)) return false;
    }
    int8_t paintjob = Ref<int8_t>((uintptr_t)car + 0x3A);
    return paintjob >= -1 && paintjob < 4;
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8(), index = r.U8();
    void* g = GarageAt(index);
    if (!r.Ok() || from == Net().MyId() || !g || !IsHideout(TypeOf(g)) || !core::IsPlayerReady()) return;
    if (op == GARAGE_STORE && Net().IsHost()) {
        uint32_t requesterEpoch=r.U32();uint8_t n=r.U8();if(n>4||!requesterEpoch)return;
        std::vector<uint16_t>ids;for(int i=0;i<n;++i)ids.push_back(r.U16());
        if(!r.Ok())return; // validate the complete request before mutating garage storage
        uint8_t* store = StoreOf(g);
        std::vector<uint16_t> kept;
        for (auto id:ids) {
            NetVehicle* nv = Vehicles::Instance().Find(id);
            if (!r.Ok() || !nv || nv->owner != from || !nv->veh || !IsVehicleValid(nv->veh) || Occupied(nv->veh) ||
                !Inside(g, nv->veh->GetPosition()))
                continue;
            if(auto receipt=g_storedReceipts.find(id);receipt!=g_storedReceipts.end()&&
               receipt->second.owner==from&&receipt->second.index==index&&receipt->second.handle==nv->handle){kept.push_back(id);continue;}
            uint8_t* slot = nullptr;
            for (int k = 0; k < 4 && !slot; ++k)
                if (!Ref<uint16_t>((uintptr_t)store + (uintptr_t)k * 0x40 + 0x12)) slot = store + k * 0x40;  // CStoredCar::m_wModelIndex
            if (!slot) break;  // all four places taken: the car stays where it is
            CallMethod<0x449760>(slot, nv->veh);  // CStoredCar::StoreCar
            if(g_storedReceipts.size()>=512)g_storedReceipts.erase(g_storedReceipts.begin());
            g_storedReceipts[id]={from,index,nv->handle};kept.push_back(id);
        }
        if (kept.empty()) return;
        Writer w(proto::G_GARAGE);
        w.U8(GARAGE_KEPT);
        w.U8(index);w.U8(from);w.U32(requesterEpoch);
        w.U8((uint8_t)kept.size());
        for (uint16_t id : kept) w.Entity(ENTITY_VEHICLE,id);
        Net().Send(proto::CH_RELIABLE, w);
        LOGI("Garage %s keeps %zu car(s) of player %d", NameOf(g).c_str(), kept.size(), from);
    } else if (op == GARAGE_KEPT && from == Net().HostId() && !Net().IsHost()) {
        auto to=r.U8();auto epoch=r.U32();if(to!=Net().MyId()||epoch!=Net().WorldEpoch())return;
        uint8_t n = r.U8();if(n>4)return;
        std::vector<uint16_t>ids;for(int i=0;i<n;++i)ids.push_back(r.U16());if(!r.Ok())return;
        int gone = 0;
        for (auto id:ids) {
            NetVehicle* nv = Vehicles::Instance().Find(id);
            if (!r.Ok() || !nv || Vehicles::Instance().IsMirror(*nv) || !nv->veh || !IsVehicleValid(nv->veh) || Occupied(nv->veh)) continue;
            StopEntering(nv->veh);
            Cmd(0x00A6).I(GetVehicleRef(nv->veh)).Run();  // DELETE_CAR: the vehicle sync tells the host's game it is gone
            ++gone;
        }
        LOGI("Garage %s: the host keeps %d car(s) of ours now", NameOf(g).c_str(), gone);
    } else if (op == GARAGE_BRING_OUT && Net().IsHost()) {  // the cars it keeps go to the partner's game: out of our store
        auto epoch=r.U32();if(!r.Ok()||!epoch)return;
        uint8_t* store = StoreOf(g);
        Writer w(proto::G_GARAGE);
        w.U8(GARAGE_CARS);
        w.U8(index);
        w.U8(from);w.U32(epoch);  // only this world of the requesting guest restores the cars
        std::vector<uint8_t*> cars;
        for (int k = 0; k < 4; ++k)
            if (Ref<uint16_t>((uintptr_t)store + (uintptr_t)k * kStoredCarSize + 0x12)) cars.push_back(store + k * kStoredCarSize);
        if (cars.empty()) return;
        w.U8((uint8_t)cars.size());
        for (uint8_t* c : cars) {
            w.Bytes(c, kStoredCarSize);
            Ref<uint16_t>((uintptr_t)c + 0x12) = 0;
        }
        Net().Send(proto::CH_RELIABLE, w);
        LOGI("Garage %s opened for player %d: its %zu car(s) go out there", NameOf(g).c_str(), from, cars.size());
    } else if (op == GARAGE_CARS && from == Net().HostId() && !Net().IsHost()) {
        uint8_t to = r.U8();auto epoch=r.U32();
        if (!r.Ok() || to != Net().MyId()) return;
        uint8_t n = r.U8(), cars[4 * kStoredCarSize] = {};if(n>4)return;
        for (int k = 0; k < n && k < 4; ++k) {
            std::vector<uint8_t> c = r.Bytes(kStoredCarSize);
            if (c.size() == kStoredCarSize) memcpy(cars + k * kStoredCarSize, c.data(), kStoredCarSize);
        }
        if (!r.Ok()) return;
        if(epoch!=Net().WorldEpoch()){
            // The requester loaded a new world while the garage was opening.
            // Return the drained store rather than spawning obsolete cars or losing them.
            Writer back(proto::G_GARAGE);back.U8(GARAGE_BACK);back.U8(index);back.U8(n);
            back.Bytes(cars,(size_t)n*kStoredCarSize);Net().Send(proto::CH_RELIABLE,back);return;
        }
        uint8_t kept[4 * kStoredCarSize] = {};  // cars this game cannot make: they stay the host's as they are
        for (int k = 0; k < 4; ++k) {
            uint8_t* c = cars + k * kStoredCarSize;
            int model = Ref<uint16_t>((uintptr_t)c + 0x12);
            if (!model) continue;
            // CStoredCar::RestoreCar makes nothing of a model not in memory yet (it only asks for it)
            if (Restorable(c)) LoadModelNow(model);
            else std::swap_ranges(c, c + kStoredCarSize, kept + k * kStoredCarSize);
        }
        g_origRestore(g, nullptr, cars);  // CGarage::RestoreCarsForThisHideOut on the host's records: the cars are ours
        // what could not come out goes back into the host's store
        Writer w(proto::G_GARAGE);
        w.U8(GARAGE_BACK);
        w.U8(index);
        std::vector<uint8_t*> left;
        for (int k = 0; k < 4; ++k) {
            if (Ref<uint16_t>((uintptr_t)cars + (uintptr_t)k * kStoredCarSize + 0x12)) left.push_back(cars + k * kStoredCarSize);
            if (Ref<uint16_t>((uintptr_t)kept + (uintptr_t)k * kStoredCarSize + 0x12)) left.push_back(kept + k * kStoredCarSize);
        }
        if (!left.empty()) {
            w.U8((uint8_t)left.size());
            for (uint8_t* c : left) w.Bytes(c, kStoredCarSize);
            Net().Send(proto::CH_RELIABLE, w);
        }
        LOGI("Garage %s: %d car(s) of the host's store out here, %zu back", NameOf(g).c_str(), n - (int)left.size(), left.size());
    } else if (op == GARAGE_BACK && Net().IsHost()) {
        uint8_t n = r.U8(), *store = StoreOf(g);
        for (int k = 0; k < n && k < 4; ++k) {
            std::vector<uint8_t> c = r.Bytes(kStoredCarSize);
            uint8_t* slot = nullptr;
            for (int s = 0; s < 4 && !slot; ++s)
                if (!Ref<uint16_t>((uintptr_t)store + (uintptr_t)s * kStoredCarSize + 0x12)) slot = store + s * kStoredCarSize;
            if (r.Ok() && c.size() == kStoredCarSize && slot) memcpy(slot, c.data(), kStoredCarSize);
        }
        LOGI("Garage %s: %d car(s) back in the store", NameOf(g).c_str(), (int)n);
    }
}

// Shop doors follow one common authority, while respray/money/wanted/control
// effects remain in the driver's own game. Each peer keeps both other peers'
// announcements; a third sender cannot overwrite the selected driver's door.
enum : uint8_t { DOOR_SHUT=0, DOOR_OPEN=1, DOOR_NONE=2 };
enum : uint8_t { STATE_CLOSED=0, STATE_OPEN=1, STATE_CLOSING=2, STATE_OPENING=3, STATE_OPEN_WAITING=4 };
struct Door {
    std::map<uint8_t,uint8_t> remote;
    uint8_t sent=0xFF;
    bool mirroring=false;
};
Door g_doors[50];
bool DoorShared(uint8_t type) { return (type>=2&&type<=5)||(type>=36&&type<=38); }

uint8_t DoorOwner(void* garage) {
    auto* me=FindPlayerPed(0);
    if(!me)return 255;
    CVector centre{(Field<float>(garage,0x28)+Field<float>(garage,0x2C))/2,
        (Field<float>(garage,0x30)+Field<float>(garage,0x34))/2,Field<float>(garage,8)};
    std::array<sync::GarageDoorCandidate,proto::kMaxPlayers> candidates{};
    size_t count=0;
    auto* car=FindPlayerVehicle();
    bool driver=car&&car->Driver()==me;
    if(me->AreaCode()==0)candidates[count++]={Net().MyId(),driver&&Inside(garage,car->GetPosition()),driver,(me->GetPosition()-centre).Length()};
    for(auto&[id,rp]:Players::Instance().All()) {
        if(count>=candidates.size()||!rp.hasState||NowMs()-rp.stateRecvMs>=1500||rp.state.area!=0)continue;
        bool drives=rp.mode==RemotePlayer::Mode::Driver;
        candidates[count++]={id,drives&&Inside(garage,rp.state.pos),drives,(rp.state.pos-centre).Length()};
    }
    return sync::GarageDoorOwner(candidates.data(),count,Net().HostId());
}

using GarageUpdateFn=void(__fastcall*)(void*,void*,int);
GarageUpdateFn g_origGarageUpdate=nullptr;
void __fastcall HookGarageUpdate(void* garage,void* edx,int id) {
    if(id<0||id>=50||!DoorShared(TypeOf(garage)))return g_origGarageUpdate(garage,edx,id);
    auto& d=g_doors[id];
    auto& state=Field<uint8_t>(garage,0x4D);
    bool online=Net().Online()&&core::IsPlayerReady();
    uint8_t owner=online?DoorOwner(garage):255;
    auto remote=d.remote.find(owner);
    // The selected driver's first door announcement may trail its player state.
    // During that gap this game must not run a shop transaction against the
    // previous driver's closed door. Hold script-door control until it arrives.
    bool theirs=online&&owner!=255&&owner!=Net().MyId();
    if(theirs) {
        if(remote!=d.remote.end()&&remote->second!=DOOR_NONE) {
            bool open=remote->second==DOOR_OPEN;
            if(open?state!=STATE_OPEN:state!=STATE_CLOSED)state=open?STATE_OPENING:STATE_CLOSING;
        }
        auto& type=Field<uint8_t>(garage,0x4C);uint8_t saved=type;
        type=19; // GARAGE_SCRIPT_CONTROLLED moves/sounds the door, without a shop transaction.
        g_origGarageUpdate(garage,edx,id);type=saved;
    } else {
        if(d.mirroring&&TypeOf(garage)<=5&&state!=STATE_OPEN&&state!=STATE_OPENING&&state!=STATE_OPEN_WAITING)
            state=STATE_OPENING;
        g_origGarageUpdate(garage,edx,id);
    }
    if(d.mirroring!=theirs)LOGI("Garage %s door authority %u",NameOf(garage).c_str(),owner);
    d.mirroring=theirs;
    uint8_t value=theirs?DOOR_NONE:(state==STATE_OPEN||state==STATE_OPENING||state==STATE_OPEN_WAITING)?DOOR_OPEN:DOOR_SHUT;
    if(online&&value!=d.sent) {
        Writer w(proto::G_GARAGE_DOOR);w.U8((uint8_t)id);w.U8(value);
        Net().Send(proto::CH_RELIABLE,w);d.sent=value;
    }
}
void HandleDoor(uint8_t from,Reader& r) {
    uint8_t id=r.U8(),value=r.U8();
    if(!r.Ok()||from==Net().MyId()||!Net().Peers().count(from)||id>=50||value>DOOR_NONE)return;
    g_doors[id].remote[from]=value;
}

class Garages : public Module {
public:
    const char* Name() const override { return "Garages"; }
    void OnInstallHooks() override {
        mem::RedirectCallFn(0x44BE44, &HookStoreClosing, &g_origStoreClosing);        // CGarage::Update
        mem::RedirectCallFn(0x44A1EA, &HookStoreBeforeSave, &g_origStoreBeforeSave);  // CGarages::CloseHideOutGaragesBeforeSave
        mem::RedirectCallFn(0x44C1E1, &HookRestore, &g_origRestore);                  // CGarage::Update
        mem::RedirectCallFn(0x5D3165, &HookCloseBeforeSave, &g_origCloseBeforeSave);  // CGarages::Save
        Net().SetHandler(proto::G_GARAGE, &Handle);
        mem::RedirectCallFn(0x44C910,&HookGarageUpdate,&g_origGarageUpdate);
        Net().SetHandler(proto::G_GARAGE_DOOR,&HandleDoor);
    }
    void OnPeerResync(uint8_t) override { for(auto& d:g_doors)d.sent=0xFF; }
    void OnPeerLeave(uint8_t id) override { for(auto& d:g_doors)d.remote.erase(id); }
    void OnSessionEnd() override { g_storedReceipts.clear();for(auto& d:g_doors){d.remote.clear();d.sent=0xFF;} }
    void OnGameStarted() override { g_storedReceipts.clear();for(auto& d:g_doors)d=Door{}; }
};

Garages g_garages;
struct Reg {
    Reg() { core::RegisterModule(&g_garages); }
} g_reg;
}  // namespace

}  // namespace coop
