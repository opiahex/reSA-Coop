// Pickups both players see. Both games start from the same save, so their pickups match by model and place (the
// position is stored in eighths of a metre). What one player takes goes in the other game too:
//  * a pickup that comes back (weapons, health, armour on the street) is gone there for the same time;
//  * one that does not is removed and recorded as collected, so a script waiting for it (the host's mission) goes on;
//    a horseshoe or an oyster is counted there as well (the engine counts those, not the scripts);
//  * a snapshot photographed here counts there, an asset's takings collected here are gone there;
//  * a spray tag painted here is painted there.
// Pickups that appear in one game only are announced: the loot of a ped killed here, a dropped jetpack, the pickups of
// the host's mission. Pickups of the scripts both games run (the street weapons, the collectibles) are not, and neither
// are save disks, info icons and houses for sale, whose scripts react in each game (houses go through Property.cpp).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <set>
#include <map>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "entities/session/Flow.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/story-stage/StoryStage.h"
#include "shared/api/story-scripts/StoryScripts.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

// CPickups::aPickUps[620], 0x20 bytes: +0 revenue, +4 object, +8 ammo/money, +0xC regeneration time, +0x10 int16 x/y/z
// (eighths of a metre), +0x16 money per day, +0x18 model, +0x1A reference index, +0x1C type, +0x1D flags (1 disabled)
constexpr uintptr_t kPickups = 0x9788C0;
constexpr int kNumPickups = 620;
constexpr uintptr_t kTags = 0xA9A8C0;     // CTagManager::ms_tagDesc: {CEntity*, uint8 alpha}, 8 bytes
constexpr uintptr_t kNumTags = 0xA9AD70;  // CTagManager::ms_numTags
enum : uint8_t {
    PU_ON_STREET = 2, PU_ONCE = 3, PU_ONCE_TIMEOUT = 4, PU_ONCE_TIMEOUT_SLOW = 5, PU_MONEY = 8, PU_ON_STREET_SLOW = 15,
    PU_FLOATINGPACKAGE = 13, PU_FLOATINGPACKAGE_FLOATING = 14, PU_ASSET_REVENUE = 16, PU_MONEY_DOESNTDISAPPEAR = 19, PU_SNAPSHOT = 20, PU_ONCE_FOR_MISSION = 22,
};
enum : uint8_t { EV_TAKEN = 1, EV_TAKINGS = 2, EV_PHOTOGRAPHED = 3, EV_TAG = 4 };
constexpr int kModelInfo = 1239, kModelSave = 1277;  // info icon, save disk

uintptr_t Pickup(int i) { return kPickups + (uintptr_t)i * 0x20; }
uint8_t& Type(uintptr_t p) { return Ref<uint8_t>(p + 0x1C); }
int16_t Model(uintptr_t p) { return Ref<int16_t>(p + 0x18); }
int16_t* Pos(uintptr_t p) { return &Ref<int16_t>(p + 0x10); }
int16_t ModelIndexVar(uintptr_t a) { return Ref<int16_t>(a); }  // CModelInfo MI_* variables

bool g_applying = false;               // this module creates or takes a pickup itself
std::set<int> g_story;
std::set<int> g_personal;
std::vector<int> g_announced;          // handles of pickups announced from here (for a restarted partner)

bool Shared(uint8_t type, int model) {
    switch (type) {
    case PU_ON_STREET: case PU_ONCE: case PU_ONCE_TIMEOUT: case PU_ONCE_TIMEOUT_SLOW: case PU_MONEY:
    case PU_ON_STREET_SLOW: case PU_ASSET_REVENUE: case PU_MONEY_DOESNTDISAPPEAR: case PU_ONCE_FOR_MISSION:
    case PU_FLOATINGPACKAGE: case PU_FLOATINGPACKAGE_FLOATING:
        return model != kModelInfo && model != kModelSave;
    }
    return false;
}

bool OrdinarySupply(int model) {
    // Use the game's model classification, including modded weapon models.
    // WeaponForModel also recognizes health, armour and adrenaline pickups.
    return Call<0x454AE0, int>(model) != 0 || model == ModelIndexVar(0x8CD59C) ||
           model == ModelIndexVar(0x8CD5D0);  // money, police bribe
}

// the local pickup matching one of the other game (same model, within 1.5 m), or -1
int Find(int model, const int16_t* pos, uint8_t type) {
    int best = -1, bestDist = 13;
    for (int i = 0; i < kNumPickups; ++i) {
        uintptr_t p = Pickup(i);
        int handle=i|(int)(Ref<uint16_t>(p+0x1A)<<16);
        if (g_personal.count(handle)||!Type(p) || Model(p) != model || (Type(p) == PU_SNAPSHOT) != (type == PU_SNAPSHOT)) continue;
        int16_t* q = Pos(p);
        int d = std::max({std::abs(q[0] - pos[0]), std::abs(q[1] - pos[1]), std::abs(q[2] - pos[2])});
        if (d < bestDist) best = i, bestDist = d;
    }
    return best;
}

struct StoryPickup {
    uint32_t source=0;uint16_t model=0;int16_t pos[3]{};uint8_t type=0;uint32_t ammo=0;uint16_t perDay=0;uint8_t flags=0;int8_t area=0;
};
std::map<uint32_t,StoryPickup> g_wantedStory;
std::map<uint32_t,int> g_storyCopies;
uint64_t g_storySentMs=0;
bool PickupAlive(int h){int i=h&0xffff;return h>=0&&i<kNumPickups&&Type(Pickup(i))&&Ref<uint16_t>(Pickup(i)+0x1A)==(uint16_t)(h>>16);}
struct EquipmentOffer{bool offered=false,taken=false;int handle=-1;CVector position;int8_t area=0;};
std::array<EquipmentOffer,3> g_equipment;
uint64_t g_equipmentRun=0,g_equipmentSent=0;uint32_t g_equipmentRevision=0,g_equipmentReceived=0;
void ClearEquipment(bool remove){
    if(remove)for(auto&e:g_equipment)if(PickupAlive(e.handle))CallMethod<0x4556C0>((void*)Pickup(e.handle&0xffff));
    g_equipment={};g_personal.clear();g_equipmentRun=0;g_equipmentSent=0;g_equipmentRevision=g_equipmentReceived=0;
}
void SendEquipment(){
    Writer w(proto::G_STORY_EQUIPMENT);w.U8(1);w.U64(g_equipmentRun);w.U32(++g_equipmentRevision);
    for(auto&e:g_equipment){w.Bool(e.offered);w.Bool(e.taken);w.Vec({e.position.x,e.position.y,e.position.z});w.I8(e.area);}
    Net().Send(proto::CH_RELIABLE,w);g_equipmentSent=NowMs();
}
void EquipmentTaken(int handle){
    if(Net().IsHost()||Net().MyId()>=3||!g_personal.count(handle))return;
    auto&e=g_equipment[Net().MyId()];e.taken=true;e.handle=-1;g_personal.erase(handle);
    Writer w(proto::G_STORY_EQUIPMENT);w.U8(2);w.U64(g_equipmentRun);Net().Send(proto::CH_RELIABLE,w);
    LOGI("Story jetpack collected run=%llu player=%u",(unsigned long long)g_equipmentRun,Net().MyId());
}
void HandleEquipment(uint8_t from,Reader&r){
    auto op=r.U8();auto token=r.U64();
    if(op==2){if(Net().IsHost()&&from<3&&r.Ok()&&token==g_equipmentRun&&g_equipment[from].offered){g_equipment[from].taken=true;SendEquipment();}return;}
    if(op!=1||Net().IsHost()||from!=Net().HostId()||!token||token!=StoryRunId())return;
    auto revision=r.U32();std::array<EquipmentOffer,3> incoming;
    for(auto&e:incoming){e.offered=r.Bool();e.taken=r.Bool();auto p=r.Vec();e.position={p.x,p.y,p.z};e.area=r.I8();if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||e.position.Length()>10000)return;}
    if(!r.Ok()||(g_equipmentRun==token&&(int32_t)(revision-g_equipmentReceived)<=0))return;
    if(g_equipmentRun!=token)ClearEquipment(core::IsPlayerReady());
    for(size_t i=0;i<3;++i){incoming[i].handle=g_equipment[i].handle;incoming[i].taken|=g_equipment[i].taken;}
    g_equipment=incoming;g_equipmentRun=token;g_equipmentReceived=revision;
}
void UpdateEquipment(){
    auto token=StoryRunId();if(g_equipmentRun&&token!=g_equipmentRun)ClearEquipment(true);if(!token)return;
    if(Net().IsHost()){
        g_equipmentRun=token;
        auto*me=FindPlayerPed(0);bool jetpack=me->Intelligence()&&CallMethod<0x601110,void*>(me->Intelligence());CVector source=me->GetPosition();int8_t area=me->AreaCode();
        // Offer equipment as soon as its native mission pickup is available,
        // including the underground jetpack before the host takes it.
        if(!jetpack)for(int h:g_story)if(PickupAlive(h)&&Model(Pickup(h&0xffff))==370){auto*p=Pos(Pickup(h&0xffff));source={p[0]/8.f,p[1]/8.f,p[2]/8.f};if(auto*o=Ref<CObject*>(Pickup(h&0xffff)+4))area=o->AreaCode();jetpack=true;break;}
        bool changed=false;
        if(jetpack)for(auto&[id,peer]:Net().Peers())if(id<3&&id!=Net().MyId()&&!g_equipment[id].offered){
            auto p=source+CVector{id==1?3.f:-3.f,-2.f,0};bool floor=false;float z=Call<0x5696C0,float>(p.x,p.y,p.z+3.f,&floor,(CEntity**)nullptr);
            if(!floor||std::fabs(z-source.z)>4.f)continue;
            auto&e=g_equipment[id];e.offered=true;e.position={p.x,p.y,z+.7f};e.area=area;changed=true;
            LOGI("Story jetpack offered run=%llu player=%u area=%d at=%.2f,%.2f,%.2f",(unsigned long long)token,id,area,p.x,p.y,z+.7f);
        }
        if(changed||NowMs()-g_equipmentSent>1000)SendEquipment();return;
    }
    if(g_equipmentRun!=token||Net().MyId()>=3||!flow::InHostWorld())return;auto&e=g_equipment[Net().MyId()];
    if(!e.offered||e.taken)return;
    if(!PickupAlive(e.handle)){
        if(FindPlayerPed(0)->AreaCode()!=e.area)return;
        if(!IsModelLoaded(370)){RequestModel(370);return;}
        g_applying=true;e.handle=Call<0x456F20,int>(e.position.x,e.position.y,e.position.z,(uint32_t)370,(int)PU_ONCE,(uint32_t)0,(uint32_t)0,0,(char*)nullptr);g_applying=false;
        if(e.handle<0)return;g_personal.insert(e.handle);LOGI("Personal mission jetpack ready player=%u handle=%d",Net().MyId(),e.handle);
    }
    if(auto*o=Ref<CObject*>(Pickup(e.handle&0xffff)+4))o->AreaCode()=e.area;
}
void SendStoryPickups(bool force=false){
    if(!Net().IsHost()||!flow::InSharedWorld()||!core::IsPlayerReady()||(!force&&NowMs()-g_storySentMs<500))return;
    g_storySentMs=NowMs();
    if(!HostReplicating()&&!core::IsOnMission())g_story.clear();
    std::vector<int> live;
    for(auto it=g_story.begin();it!=g_story.end();)if(!PickupAlive(*it))it=g_story.erase(it);else{live.push_back(*it);++it;}
    Writer w(proto::G_STORY_PICKUPS);w.U8((uint8_t)std::min<size_t>(live.size(),64));
    for(size_t n=0;n<live.size()&&n<64;++n){auto p=Pickup(live[n]&0xffff);w.U32(live[n]);w.U16(Model(p));for(int k=0;k<3;++k)w.I16(Pos(p)[k]);w.U8(Type(p));w.U32(Ref<uint32_t>(p+8));w.U16(Ref<uint16_t>(p+0x16));w.U8(Ref<uint8_t>(p+0x1D)&3);auto*object=Ref<CObject*>(p+4);w.I8(object?object->AreaCode():FindPlayerPed(0)->AreaCode());}
    Net().Send(proto::CH_RELIABLE,w);
}
void HandleStoryPickups(uint8_t from,Reader&r){
    auto n=r.U8();if(from!=Net().HostId()||Net().IsHost()||n>64)return;
    std::map<uint32_t,StoryPickup> wanted;
    for(int i=0;i<n&&r.Ok();++i){StoryPickup p;p.source=r.U32();p.model=r.U16();for(auto&v:p.pos)v=r.I16();p.type=r.U8();p.ammo=r.U32();p.perDay=r.U16();p.flags=r.U8();p.area=r.I8();if(Shared(p.type,p.model)&&p.model>0&&p.model<20000)wanted[p.source]=p;}
    if(r.Ok())g_wantedStory=std::move(wanted);
}
void ApplyStoryPickups(){
    if(!flow::InHostWorld()||Net().IsHost()||!core::IsPlayerReady())return;
    for(auto it=g_storyCopies.begin();it!=g_storyCopies.end();)if(!g_wantedStory.count(it->first)){
        if(PickupAlive(it->second))CallMethod<0x4556C0>((void*)Pickup(it->second&0xffff));
        g_story.erase(it->second);it=g_storyCopies.erase(it);
    }else ++it;
    int made=0;
    for(const auto&[source,p]:g_wantedStory){
        int h=g_storyCopies.count(source)?g_storyCopies[source]:-1;
        if(!PickupAlive(h)){
            if(made>=2)continue;
            if(!IsModelLoaded(p.model)){RequestModel(p.model);continue;}
            int existing=Find(p.model,p.pos,p.type);
            if(existing>=0)h=existing|(int)(Ref<uint16_t>(Pickup(existing)+0x1A)<<16);
            else{g_applying=true;h=Call<0x456F20,int>(p.pos[0]/8.0f,p.pos[1]/8.0f,p.pos[2]/8.0f,(uint32_t)p.model,(int)p.type,p.ammo,(uint32_t)p.perDay,(int)((p.flags&2)!=0),(char*)nullptr);g_applying=false;++made;}
            if(h<0)continue;
            g_storyCopies[source]=h;g_story.insert(h);
            LOGI("Quest pickup %08X (model %u) copied",source,p.model);
        }
        auto pickup=Pickup(h&0xffff);Ref<uint8_t>(pickup+0x1D)=(Ref<uint8_t>(pickup+0x1D)&~3u)|(p.flags&3u);
        if(auto*object=Ref<CObject*>(pickup+4))object->AreaCode()=p.area;
    }
}

void SendEvent(uint8_t what, uintptr_t p, uint8_t type) {
    Writer w(proto::G_PICKUP_EVENT);
    w.U8(what);
    w.U16((uint16_t)Model(p));
    for (int k = 0; k < 3; ++k) w.I16(Pos(p)[k]);
    w.U8(type);
    Net().Send(proto::CH_RELIABLE, w);
}

// ---------------------------------------------------------------- this game's player takes a pickup
using PickupUpdateFn = bool(__fastcall*)(void*, void*, CPlayerPed*, CVehicle*, int);
PickupUpdateFn g_origPickupUpdate = nullptr;
bool __fastcall HookPickupUpdate(void* pickup, void* edx, CPlayerPed* player, CVehicle* veh, int playerId) {
    auto p = (uintptr_t)pickup;
    uint8_t type = Type(p);
    int handle = (int)((p - kPickups) / 0x20) | (int)(Ref<uint16_t>(p + 0x1A) << 16);
    if (Net().Online() && !g_applying && player == FindPlayerPed(0) && g_story.count(handle)) {
        const auto& pos = player->GetPosition();
        float x = Pos(p)[0] / 8.0f, y = Pos(p)[1] / 8.0f, z = Pos(p)[2] / 8.0f;
        if (std::hypot(pos.x-x, pos.y-y) < 6.0f && std::fabs(pos.z-z) < 4.0f &&
            (!Net().IsHost() || !StoryGroupReady(x,y,z,player->AreaCode()))) {
            // Collection is gated, rendering is not. Returning before native
            // Update also skipped creation of its object when spawning nearby.
            auto*&object=Ref<CObject*>(p+4);
            if(!object&&!(Ref<uint8_t>(p+0x1D)&1)&&IsModelLoaded(Model(p))&&HasRoom(ObjectPool(),kKeepObjects,"object")){
                CallMethod<0x4567E0>((void*)p,&object,-1);
                if(object)WorldAdd(object);
            }
            return false;
        }
    }
    float takings = Ref<float>(p);
    uint8_t before[0x20];
    memcpy(before, pickup, sizeof(before));  // Remove() clears the type and the object
    bool taken = g_origPickupUpdate(pickup, edx, player, veh, playerId);
    if(g_personal.count(handle)){if(taken&&player==FindPlayerPed(0))EquipmentTaken(handle);return taken;}
    if (!flow::InSharedWorld() || g_applying || player != FindPlayerPed(0) || !Shared(type, Model((uintptr_t)before)))
        return taken;
    if (taken) {
        SendEvent(EV_TAKEN, (uintptr_t)before, type);
    } else if (type == PU_ASSET_REVENUE && takings >= 10.0f && Ref<float>(p) == 0.0f) {
        SendEvent(EV_TAKINGS, p, type);
    }
    return taken;
}

// a snapshot photographed here (CPickups::PictureTaken removes it without recording)
using VoidFn = void(__cdecl*)();
VoidFn g_origPictureTaken = nullptr;
void __cdecl HookPictureTaken() {
    std::vector<std::pair<int, std::vector<uint8_t>>> snapshots;
    for (int i = 0; i < kNumPickups; ++i)
        if (Type(Pickup(i)) == PU_SNAPSHOT)
            snapshots.emplace_back(i, std::vector<uint8_t>((uint8_t*)Pickup(i), (uint8_t*)Pickup(i) + 0x20));
    g_origPictureTaken();
    if (!flow::InSharedWorld()) return;
    for (auto& [i, bytes] : snapshots)
        if (Type(Pickup(i)) != PU_SNAPSHOT) SendEvent(EV_PHOTOGRAPHED, (uintptr_t)bytes.data(), PU_SNAPSHOT);
}

// a tag painted here (CWorld::SprayPaintWorld's call of CTagManager::SetAlpha)
using SetAlphaFn = void(__cdecl*)(CEntity*, uint8_t);
// the game's own until redirected: a site another mod changed is left alone, and this is still called
SetAlphaFn g_origSetAlpha = (SetAlphaFn)0x49CEC0;
int TagIndex(CEntity* e) {
    int n = std::min(Ref<int32_t>(kNumTags), 150);
    for (int i = 0; i < n; ++i)
        if (Ref<CEntity*>(kTags + (uintptr_t)i * 8) == e) return i;
    return -1;
}
void __cdecl HookSprayTag(CEntity* tag, uint8_t alpha) {
    int i = TagIndex(tag);
    uint8_t was = i >= 0 ? Ref<uint8_t>(kTags + (uintptr_t)i * 8 + 4) : 0;
    g_origSetAlpha(tag, alpha);
    if (!flow::InSharedWorld() || g_applying || !tag || was > 228 || alpha <= 228) return;  // counts as sprayed above 228
    CVector pos = tag->GetPosition();
    Writer w(proto::G_PICKUP_EVENT);
    w.U8(EV_TAG);
    w.Vec({pos.x, pos.y, pos.z});
    Net().Send(proto::CH_RELIABLE, w);
}

// ---------------------------------------------------------------- pickups that appear in one game only
using GenerateFn = int(__cdecl*)(float, float, float, uint32_t, int, uint32_t, uint32_t, int, char*);
GenerateFn g_origGenerate = nullptr;
bool Announced(int type, int model) {
    if (g_applying || !flow::InSharedWorld() || !core::IsPlayerReady() || PlayerInFocus() != 0) return false;  // the remote
    if (!Shared((uint8_t)type, model)) return false;                                  // player's drops are its game's
    if (void* script = CurrentScript())
        return Net().IsHost() && HostReplicating() && IsSharedStoryScript(script);
    return true;  // the engine: loot, a dropped jetpack, gang war rewards
}
void SendCreate(uintptr_t p) {
    Writer w(proto::G_PICKUP_CREATE);
    w.U16((uint16_t)Model(p));
    for (int k = 0; k < 3; ++k) w.I16(Pos(p)[k]);
    w.U8(Type(p));
    w.U32(Ref<uint32_t>(p + 0x08));  // ammo, money
    w.U16(Ref<uint16_t>(p + 0x16));  // money per day
    w.U8(Ref<uint8_t>(p + 0x1D) & 2);  // empty
    int handle = (int)((p - kPickups) / 0x20) | (int)(Ref<uint16_t>(p + 0x1A) << 16);
    w.Bool(g_story.count(handle) != 0);
    Net().Send(proto::CH_RELIABLE, w);
}
int __cdecl HookGenerateNewOne(float x, float y, float z, uint32_t model, int type, uint32_t ammo, uint32_t moneyPerDay,
                               int isEmpty, char* message) {
    int handle = g_origGenerate(x, y, z, model, type, ammo, moneyPerDay, isEmpty, message);
    if (handle >= 0 && !g_applying && Net().Online() && Net().IsHost() && HostReplicating() &&
        Shared((uint8_t)type, (int)model) && !OrdinarySupply((int)model) &&
        ((type & 0xFF) == PU_ONCE_FOR_MISSION || (type & 0xFF) == PU_ONCE || (type&0xFF)==PU_FLOATINGPACKAGE || (type&0xFF)==PU_FLOATINGPACKAGE_FLOATING)) {
        if (void* script = CurrentScript(); script && !game::script::Running() && IsSharedStoryScript(script)) g_story.insert(handle);
    }
    if (handle >= 0 && !g_story.count(handle) && Announced(type & 0xFF, (int)model)) {
        SendCreate(Pickup(handle & 0xFFFF));
        g_announced.push_back(handle);
    }
    return handle;
}

// ---------------------------------------------------------------- the other game's player took one
void Take(int i, uint8_t type) {
    uintptr_t p = Pickup(i);
    if (type == PU_ON_STREET || type == PU_ON_STREET_SLOW) {
        // as if taken here: back after the same time (CPickup::Update)
        if (Ref<uint8_t>(p + 0x1D) & 1) return;
        uint32_t wait = type == PU_ON_STREET ? 30000 : Model(p) == ModelIndexVar(0x8CD5D0) ? 300000 : 360000;  // bribe
        Ref<uint32_t>(p + 0x0C) = TimeMs() + wait;
        CallMethod<0x454CF0>((void*)p);  // CPickup::GetRidOfObjects
        Ref<uint8_t>(p + 0x1D) |= 1;
        return;
    }
    // The other player's find counts here too, as CPickups::PickedUpHorseShoe / PickedUpOyster count it (its luck share
    // of all of them for a horseshoe). The $100, the $100 000 for the last one and the message are the finder's: this
    // game paid them as well.
    int model = Model(p);
    if (model == ModelIndexVar(0x8CD754)) {
        Call<0x55C180>((uint16_t)241, 1.0f);  // CStats::IncrementStat: horseshoes collected
        Call<0x55C180>((uint16_t)233, 1000.0f / std::max(Call<0x558E40, float>((uint16_t)242), 1.0f));  // luck / all of them
    } else if (model == ModelIndexVar(0x8CD750)) {
        Call<0x55C180>((uint16_t)243, 1.0f);  // oysters collected
    }
    CallMethod<0x4556C0>((void*)p);  // CPickup::Remove
    Call<0x455240>(i);                // CPickups::AddToCollectedPickupsArray: HAS_PICKUP_BEEN_COLLECTED
}

void HandleEvent(uint8_t from, Reader& r) {
    uint8_t what = r.U8();
    if (what == EV_TAG) {
        Vec3 v = r.Vec();
        if (!r.Ok() || !core::IsPlayerReady() || !flow::InSharedWorld()) return;
        int n = std::min(Ref<int32_t>(kNumTags), 150);
        for (int i = 0; i < n; ++i) {
            auto* tag = Ref<CEntity*>(kTags + (uintptr_t)i * 8);
            if (!tag || (tag->GetPosition() - CVector{v.x, v.y, v.z}).Length() > 1.0f) continue;
            if (Ref<uint8_t>(kTags + (uintptr_t)i * 8 + 4) <= 228) {
                g_applying = true;
                g_origSetAlpha(tag, 255);  // shows "tags x of 100" as here
                g_applying = false;
                LOGI("Tag %d painted by player %d", i, from);
            }
            return;
        }
        return;
    }
    int model = r.U16();
    int16_t pos[3];
    for (auto& c : pos) c = r.I16();
    uint8_t type = r.U8();
    if(r.Ok()&&from==Net().HostId()&&what==EV_TAKEN)
        for(auto it=g_wantedStory.begin();it!=g_wantedStory.end();) {
            const auto&p=it->second;
            it=p.model==model&&p.type==type&&std::equal(pos,pos+3,p.pos)?g_wantedStory.erase(it):std::next(it);
        }
    if (!r.Ok() || !core::IsPlayerReady() || !flow::InSharedWorld()) return;
    int i = Find(model, pos, type);
    if (i < 0) return;
    uintptr_t p = Pickup(i);
    g_applying = true;
    if (what == EV_TAKINGS) {
        Ref<float>(p) = 0.0f;
    } else if (what == EV_PHOTOGRAPHED) {
        CallMethod<0x4556C0>((void*)p);  // CPickup::Remove
        Call<0x55C180>((uint16_t)231, 1.0f);  // CStats::IncrementStat: snapshots taken
    } else {
        Take(i, Type(p));
    }
    g_applying = false;
    LOGI("Pickup %d (model %d) taken by player %d", i, model, from);
}

void HandleCreate(uint8_t from, Reader& r) {
    int model = r.U16();
    int16_t pos[3];
    for (auto& c : pos) c = r.I16();
    uint8_t type = r.U8();
    uint32_t ammo = r.U32();
    uint16_t perDay = r.U16();
    bool empty = r.U8() != 0;
    bool story = r.Bool();
    if (!r.Ok() || !core::IsPlayerReady() || !Shared(type, model)) return;
    if (int existing = Find(model, pos, type); existing >= 0) {
        if (story && from == Net().HostId()) g_story.insert(existing | (int)(Ref<uint16_t>(Pickup(existing) + 0x1A) << 16));
        return;
    }
    if (!IsModelLoaded(model)) RequestModel(model);
    g_applying = true;
    int handle = g_origGenerate(pos[0] / 8.0f, pos[1] / 8.0f, pos[2] / 8.0f, (uint32_t)model, type, ammo, perDay, empty, nullptr);
    if (handle >= 0 && story && from == Net().HostId()) g_story.insert(handle);
    g_applying = false;
    LOGD("Pickup (model %d, type %d) from player %d", model, type, from);
}

class Pickups : public Module {
public:
    const char* Name() const override { return "Pickups"; }
    void CollectSyncAudit(std::vector<SyncAuditEntry>& out) override {
        SyncAuditEntry e;e.kind=AUDIT_PICKUPS;e.id=1;e.area=13;e.owner=Net().HostId();e.alive=true;e.look=2166136261u;
        if(Net().IsHost()){
            for(int h:g_story)if(PickupAlive(h)){e.look=AuditHash(e.look,(uint32_t)h);++e.relation;}
        }else for(auto&[id,p]:g_wantedStory){
            auto it=g_storyCopies.find(id);
            if(it!=g_storyCopies.end()&&PickupAlive(it->second)){e.look=AuditHash(e.look,id);++e.relation;}
            else e.alive=false;
        }
        out.push_back(e);
    }
    bool RepairSyncEntity(uint8_t kind,uint16_t id) override {
        if(kind!=AUDIT_PICKUPS||id!=1||!Net().IsHost())return false;
        SendStoryPickups(true);return true;
    }

    void OnInstallHooks() override {
        mem::HookFn(0x457410, &HookPickupUpdate, &g_origPickupUpdate, "CPickup::Update");
        mem::HookFn(0x456A70, &HookPictureTaken, &g_origPictureTaken, "CPickups::PictureTaken");
        mem::HookFn(0x456F20, &HookGenerateNewOne, &g_origGenerate, "CPickups::GenerateNewOne");
        mem::RedirectCallFn(0x565C5E, &HookSprayTag, &g_origSetAlpha);  // CWorld::SprayPaintWorld -> CTagManager::SetAlpha
        Net().SetHandler(proto::G_PICKUP_EVENT, &HandleEvent);
        Net().SetHandler(proto::G_PICKUP_CREATE, &HandleCreate);
        Net().SetHandler(proto::G_STORY_PICKUPS,&HandleStoryPickups);
        Net().SetHandler(proto::G_STORY_EQUIPMENT,&HandleEquipment);
    }
    void OnSessionEnd() override { ClearEquipment(core::IsPlayerReady());g_announced.clear(); g_story.clear();g_wantedStory.clear();g_storyCopies.clear();g_storySentMs=0; }
    void OnGameStarted() override { ClearEquipment(false);g_announced.clear(); g_story.clear();g_storyCopies.clear();g_storySentMs=0; }
    void OnGameShutdown()override{ClearEquipment(false);g_wantedStory.clear();g_storyCopies.clear();g_story.clear();}
    void OnPostProcess()override{if(!Net().Online()||!core::IsPlayerReady())return;UpdateEquipment();if(Net().IsHost())SendStoryPickups();else ApplyStoryPickups();}
    void OnPeerResync(uint8_t) override {
        if(Net().IsHost())SendStoryPickups(true);
        // a restarted game lost what was announced to it: what is still here goes again
        std::vector<int> alive;
        for (int h : g_announced) {
            int i = h & 0xFFFF;
            if (i >= kNumPickups || Ref<uint16_t>(Pickup(i) + 0x1A) != (uint16_t)(h >> 16) || !Type(Pickup(i))) continue;
            SendCreate(Pickup(i));
            alive.push_back(h);
        }
        g_announced = std::move(alive);
    }

    static Pickups& Instance() {
        static Pickups m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Pickups::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
