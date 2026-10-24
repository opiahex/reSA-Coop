#include "shared/api/vehicle-effects/VehicleEffects.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/mission-objects/SharedObject.h"
#include "entities/player/Players.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <cmath>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "shared/lib/sync/SeatClaims.h"
#include "shared/lib/sync/TrainSupport.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/session/Flow.h"
#include "entities/vehicle/Vehicles.h"
#include "entities/activity/Activities.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Script.h"
#include "shared/game/PlayerContext.h"
#include "shared/game/Anim.h"
#include "shared/game/Transform.h"
#include "shared/game/VehicleSurface.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop {

using namespace game;
using game::script::Cmd;

Players& Players::Instance() {
    static Players p;
    return p;
}

namespace {
struct Reg {
    Reg() { core::RegisterModule(&Players::Instance()); }
} g_reg;

constexpr int kRemoteSlot = 1;  // CWorld::Players index used for the remote player while it is processed
constexpr int kLastWeapon = 46;  // eWeaponType: the parachute
constexpr int kCellphone = 330;  // MODEL_CELLPHONE
constexpr float kTeleportDist = 5.0f;

// Native Fight::ProcessPed falls back to the basic combo while a gym IFP is
// loading, then keeps that idle association. Stream the three small style
// blocks before combat (also after a saved profile/world reload), keeping one
// explicit reference each for this session. No synchronous streaming stall.
int g_meleeBlocks[3]={-1,-1,-1};bool g_meleeRefs[3]{};
void KeepMeleeBlocks(){
    static const char*names[]={"fight_b","fight_c","fight_d"};
    for(int i=0;i<3;++i){
        auto&idx=g_meleeBlocks[i];if(idx<0)idx=Call<0x4D3990,int>(names[i]);
        if(idx<0||g_meleeRefs[i])continue;
        if(anim::BlockLoaded(idx)){Call<0x4D3FB0>(idx);g_meleeRefs[i]=true;}
        else RequestModel(anim::kIfpBase+idx,0);
    }
}
void ReleaseMeleeBlocks(){
    for(int i=0;i<3;++i){if(g_meleeRefs[i])Call<0x4D3FD0>(g_meleeBlocks[i]);g_meleeRefs[i]=false;g_meleeBlocks[i]=-1;}
}

// ------------------------------------------------------------------ context switching
// CStats::StatTypesInt (0xB79000, 223 ints) followed by CStats::StatTypesFloat (0xB79380, 82 floats): the remote
// player's skills, stamina, fat... while it is processed. Its running, swimming and shooting advance the progress
// counters after them (m_SprintStaminaCounter.. 0xB794D8, UpdateFatAndMuscleStats' state 0xB79534): those stay ours,
// and so do the stat update boxes (CStats::bShowUpdateStats, 0x8CDE56: "Stamina +", "Respect - Upgraded" texts).
constexpr uintptr_t kStatsBlock = 0xB79000;
constexpr size_t kStatsBlockSize = 0x4C8, kStatsKeptSize = 0x538;
constexpr uintptr_t kShowUpdateStats = 0x8CDE56;

// The remote player is a PLAYER1 ped as well, and what its processing does to the player's aim stays its own:
// CCamera::m_PlayerWeaponMode (cleared when it does not aim), m_vecAimingTargetCoors, and the lock-on crosshair
// CWeaponEffects gCrossHair[0] (the engine picks it by the ped type)
constexpr uintptr_t kCamWeaponMode = addr::TheCamera + 0x830, kCamAimingCoors = addr::TheCamera + 0x854;
constexpr uintptr_t kCrossHair = 0xC8A838;
// Goggles it puts on switch the screen's night or thermal vision (CPed::AddGogglesModel points the ped's
// m_pGogglesState, +0x500, at CPostEffects::m_bNightVision / m_bInfraredVision): the screen is this game's player's
constexpr uintptr_t kVision = 0xC402B8;
// its sniper shots shake the camera (CamShakeNoPos: TheCamera +0x12C force, +0x5C start) and every gun the pads
constexpr uintptr_t kCamShake = addr::TheCamera + 0x12C, kCamShakeStart = addr::TheCamera + 0x5C;

struct LocalContext {
    uint8_t stats[kStatsKeptSize];
    uint8_t showUpdateStats;
    CPad pads[2];  // whole: buttons, controls, shake
    int8_t focus;
    uint16_t camMode;
    uint8_t weaponMode[12], aimingCoors[12], crossHair[0x2C], vision[2];
    float shake;
    uint32_t shakeStart;
    float myHealth;  // an ambulance it gets into heals "the player" (CVehicle::SetDriver's freebies: Players[0])
    float fov, orientation, alpha, beta;
    CVector front, source, up;
    uint8_t mouse3rd;
    float chairX, chairY;
};

void SaveLocal(LocalContext& c) {
    memcpy(c.stats, (void*)kStatsBlock, kStatsKeptSize);
    c.showUpdateStats = Ref<uint8_t>(kShowUpdateStats);
    memcpy(c.pads, GetPad(0), sizeof c.pads);
    c.focus = PlayerInFocus();
    c.camMode = cam::Mode();
    memcpy(c.weaponMode, (void*)kCamWeaponMode, sizeof c.weaponMode);
    memcpy(c.aimingCoors, (void*)kCamAimingCoors, sizeof c.aimingCoors);
    memcpy(c.crossHair, (void*)kCrossHair, sizeof c.crossHair);
    memcpy(c.vision, (void*)kVision, sizeof c.vision);
    c.shake = Ref<float>(kCamShake);
    c.shakeStart = Ref<uint32_t>(kCamShakeStart);
    c.myHealth = PlayerInfo(0)->ped ? PlayerInfo(0)->ped->Health() : 0.0f;
    c.fov = cam::Fov();
    c.orientation = cam::Orientation();
    c.alpha = cam::Alpha();
    c.beta = cam::Beta();
    c.front = cam::Front();
    c.source = cam::Source();
    c.up = cam::Up();
    c.mouse3rd = cam::UseMouse3rdPerson();
    c.chairX = cam::CrosshairMultX();
    c.chairY = cam::CrosshairMultY();
}

void RestoreLocal(const LocalContext& c) {
    memcpy((void*)kStatsBlock, c.stats, kStatsKeptSize);
    Ref<uint8_t>(kShowUpdateStats) = c.showUpdateStats;
    memcpy(GetPad(0), c.pads, sizeof c.pads);
    PlayerInFocus() = c.focus;
    cam::Mode() = c.camMode;
    memcpy((void*)kCamWeaponMode, c.weaponMode, sizeof c.weaponMode);
    memcpy((void*)kCamAimingCoors, c.aimingCoors, sizeof c.aimingCoors);
    memcpy((void*)kCrossHair, c.crossHair, sizeof c.crossHair);
    memcpy((void*)kVision, c.vision, sizeof c.vision);
    Ref<float>(kCamShake) = c.shake;
    Ref<uint32_t>(kCamShakeStart) = c.shakeStart;
    if (PlayerInfo(0)->ped) PlayerInfo(0)->ped->Health() = c.myHealth;
    cam::Fov() = c.fov;
    cam::Orientation() = c.orientation;
    cam::Alpha() = c.alpha;
    cam::Beta() = c.beta;
    cam::Front() = c.front;
    cam::Source() = c.source;
    cam::Up() = c.up;
    cam::UseMouse3rdPerson() = c.mouse3rd;
    cam::CrosshairMultX() = c.chairX;
    cam::CrosshairMultY() = c.chairY;
}

void ApplyRemote(RemotePlayer& rp) {
    rp.ped->FightingStyle() = rp.state.fightingStyle;
    rp.ped->AllowedAttackMoves() = rp.state.attackMoves;
    if (rp.stats.size() == kStatsBlockSize) memcpy((void*)kStatsBlock, rp.stats.data(), kStatsBlockSize);  // its skills, stamina...
    Ref<uint8_t>(kShowUpdateStats) = 0;
    CPad* p0 = GetPad(0);
    CPad* p1 = GetPad(1);
    p0->NewState = p1->NewState = rp.padNew;
    p0->OldState = p1->OldState = rp.padOld;
    p0->DisablePlayerControls = 0;
    memset(&p0->bApplyBrakes, 0, 8);
    PlayerInFocus() = (int8_t)kRemoteSlot;
    const CameraState& c = rp.state.cam;
    cam::Mode() = c.mode;
    cam::WeaponMode() = c.weaponMode;
    cam::Fov() = c.fov;
    cam::Orientation() = c.orientation;
    cam::Alpha() = c.alpha;
    cam::Beta() = c.beta;
    cam::Front() = c.front;
    cam::Source() = rp.ped->GetPosition() + c.sourceOffset;
    cam::Up() = c.up;
    Ref<CVector>(kCamAimingCoors) = rp.ped->GetPosition() + c.aimOffset;
    rp.ped->AimingRotation() = rp.state.aimRotation;
    cam::UseMouse3rdPerson() = c.useMouse3rd ? 1 : 0;
    cam::CrosshairMultX() = c.chairX;
    cam::CrosshairMultY() = c.chairY;
    if (rp.ped->PlayerData()) rp.ped->LookPitch() = c.lookPitch;
}

// A driver's drive-by (CVehicle::DoDriveByShootings: looking aside and firing out of the window) runs only for a car of
// STATUS_PLAYER, and the remote player's car is a mirror here: it runs for it under its pad and camera. The shots only
// show: the shooter's game reports its hits.
void DriveBy(CPlayerPed* ped) {
    CVehicle* v = ped->InVehicle() ? ped->Vehicle() : nullptr;
    if (!v || v->Driver() != ped) return;
    uint8_t& typeStatus = Field<uint8_t>(v, 0x36);
    uint8_t saved = typeStatus;
    typeStatus &= 0x07;  // m_nStatus (bits 3..7): STATUS_PLAYER
    // with the usual car camera the side comes from the camera's looking left/right, and the camera is ours: the
    // remote player's look buttons set it for the call
    auto* camLook = (uint8_t*)(cam::Active() + 0x08);
    uint8_t look[2] = {camLook[0], camLook[1]};
    const CControllerState& pad = GetPad(0)->NewState;
    camLook[0] = pad.LeftShoulder2 && !pad.RightShoulder2;
    camLook[1] = pad.RightShoulder2 && !pad.LeftShoulder2;
    int clip = ped->ActiveWeapon().ammoInClip;
    CallMethod<0x741FD0>(v);
    camLook[0] = look[0], camLook[1] = look[1];
    typeStatus = saved;
    if (ped->ActiveWeapon().ammoInClip != clip) LOGD("Remote player drive-by: a shot");
}

// A passenger's drive-by is the game's split-screen mode's (CTaskSimpleCarDrive::ProcessPed 0x6449AB: a player in a
// passenger seat leans out of the window, CTaskSimpleGangDriveBy). In co-op it starts for a passenger holding the right
// mouse button (R1 on a pad) with any gun in hand, not only an SMG (the task takes any instant-hit weapon), and ends
// when the button is let go. The checks of the SMG slot and of the split-screen camera after the call are dropped.
// The player processed is the one in focus: the other player's copy is processed under its own pad, so its drive-by
// shows here too (its game folds the mouse button into R1, FillCommon).
std::map<CPed*, bool> g_ourDriveBy;  // passengers whose drive-by this started (a mission's is left alone)
bool AimFromSeat(const CPad* pad) { return pad->NewState.RightShoulder1 || pad->NewState.m_bVehicleMouseLook; }

bool __cdecl PassengerDriveBy() {
    const CPad* pad = GetPad(0);
    CPlayerPed* ped = FindPlayerPed(-1);
    if (!Net().Online() || !ped || pad->DisablePlayerControls || !AimFromSeat(pad) || cam::Mode() == 0x37) return false;  // MODE_AIMWEAPON_FROMCAR
    const CWeapon& w = ped->ActiveWeapon();
    void* info = w.type ? Call<0x743C60, void*>(w.type, (uint8_t)1) : nullptr;  // CWeaponInfo::GetWeaponInfo
    if (!info || Field<int32_t>(info, 0) != 1 || w.ammoTotal <= 0) return false;  // m_nWeaponFire: FIRE_INSTANT_HIT
    g_ourDriveBy[ped] = true;
    return true;
}

using GangDriveByFn = bool(__fastcall*)(void*, void*, CPed*);
GangDriveByFn g_origGangDriveBy = nullptr;
bool __fastcall HookGangDriveByProcess(void* task, void* edx, CPed* ped) {
    auto ours = g_ourDriveBy.find(ped);
    if (ours != g_ourDriveBy.end() && !AimFromSeat(GetPad(0))) Field<int8_t>(task, 0x0F) = 7;  // m_bIsFinished: let go
    bool done = g_origGangDriveBy(task, edx, ped);
    if (done && ours != g_ourDriveBy.end()) g_ourDriveBy.erase(ours);
    return done;
}

void SetSupportReference(CEntity*& slot,CEntity* entity){
    if(slot==entity)return;
    if(slot)CallMethod<0x571A00>(slot,&slot);
    slot=entity;
    if(entity)CallMethod<0x571B70>(entity,&slot);
}
void ClearChangedSupport(CPed* ped,uint16_t previous,uint16_t next) {
    if(!ped||!previous||previous==next)return;
    auto*nv=Vehicles::Instance().Find(previous);
    auto*support=nv&&IsVehicleValid(nv->veh)?nv->veh:ResolveTrainSupport(previous);
    if(!support)return;
    for(int off:{0x568,0x584})if(Field<CEntity*>(ped,off)==support)
        SetSupportReference(Field<CEntity*>(ped,off),nullptr);
}
// Two players dead or busted at about the same time come back at the same hospital or police station door, one inside
// the other: the one whose game was done first stood stuck in the other, still in its respawn view, until that ended.
// Ours comes back 1.5 m to the right when the other player's copy is at that door already (both at once: KeepApart).
using RestoreFn = void(__cdecl*)(CPlayerPed*, CVector, float);
RestoreFn g_origRestore = nullptr;
void __cdecl HookRestorePlayer(CPlayerPed* player, CVector pos, float heading) {
    if(Net().Online()&&player==FindPlayerPed(0)){
        CVector original=pos;float h=heading*0.0174533f;
        for(float side:{0.0f,1.75f,-1.75f,3.5f,-3.5f}){
            CVector candidate=original+CVector{std::cos(h),std::sin(h),0}*side;
            bool free=true;
            for(auto&[id,rp]:Players::Instance().All())if(rp.ped){
                auto d=rp.ped->GetPosition()-candidate;
                if(d.Length2D()<1.5f&&std::fabs(d.z)<3){free=false;break;}
            }
            if(free){pos=candidate;break;}
        }
    }
    g_origRestore(player, pos, heading);
}

// DELETE_CHAR deletes whoever it is given (CTheScripts::RemoveThisPed). A mission that clears "the player's car" for
// the people it brings deletes its passengers: in "Sweet's Girl" the host rode with the partner driving, the host's own
// CJ was deleted, and the host's game crashed on its next look at him (the partner's screen stayed dark). A player, ours
// or the other one's copy, only gets out of the car; vanilla scripts never delete a player.
using RemovePedFn = void(__cdecl*)(CPed*);
RemovePedFn g_origRemoveThisPed = nullptr;
void __cdecl HookDeleteChar(CPed* ped) {
    if (ped && ped->IsPlayer()) {
        if (ped->InVehicle()) {
            CVector p = ped->GetPosition();
            Cmd(0x0362).I(GetPedRef(ped)).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        }
        LOGW("A script deleted a player (%s): out of its car instead", ped == FindPlayerPed(0) ? "ours" : "a copy");
        return;
    }
    g_origRemoveThisPed(ped);
}

using JetpackInputFn=void(__fastcall*)(void*,void*,CPlayerPed*);
JetpackInputFn originalJetpackInput=nullptr,originalJetpackThrust=nullptr;
void __fastcall JetpackInput(void*task,void*edx,CPlayerPed*ped){
    originalJetpackInput(task,edx,ped);
    if(auto*rp=Players::Instance().FindByPed(ped);rp&&rp->hasState&&(rp->state.flags&OnFootState::F_JETPACK)&&NowMs()-rp->stateRecvMs<1000)rp->renderedJetpack.Apply(task);
}
void __fastcall JetpackThrust(void*task,void*edx,CPlayerPed*ped){
    auto*rp=Players::Instance().FindByPed(ped);auto velocity=ped->MoveSpeed();
    if(rp&&rp->hasState&&(rp->state.flags&OnFootState::F_JETPACK))rp->renderedJetpack.Apply(task);
    originalJetpackThrust(task,edx,ped);
    // Keep native legs, exhaust and audio; only the owner supplies flight velocity.
    if(rp)ped->MoveSpeed()=velocity;
}
using ProcessControlFn = void(__fastcall*)(CPlayerPed*, void*);
ProcessControlFn g_origProcessControl = nullptr;
bool g_inRemoteContext = false;

void __fastcall HookPlayerProcessControl(CPlayerPed* self, void* edx) {
    RemotePlayer* rp = g_inRemoteContext ? nullptr : Players::Instance().FindByPed(self);
    if (!rp) {
        g_origProcessControl(self, edx);
        return;
    }
    LocalContext saved;
    SaveLocal(saved);
    {
        PlayerSlotContext remoteSlot(*PlayerInfo(kRemoteSlot), rp->info);
        ApplyRemote(*rp);
        g_inRemoteContext = true;
        g_origProcessControl(self, edx);
        DriveBy(self);  // still under its pad and camera
        g_inRemoteContext = false;
    }
    RestoreLocal(saved);
    Field<bool*>(self, 0x500) = nullptr;  // CPed::m_pGogglesState: taking its goggles off must not switch ours off
}

// The remote player's gang work (CPlayerPed::ProcessGroupBehaviour, called from its ProcessControl): recruiting this
// game's Grove members with its buttons, and this game's Grove gang reacting
// to its wanted level. Its own game runs its gang.
using GroupBehaviourFn = void(__fastcall*)(CPlayerPed*, void*, CPad*);
GroupBehaviourFn g_origGroupBehaviour = nullptr;
void __fastcall HookGroupBehaviour(CPlayerPed* self, void* edx, CPad* pad) {
    if (!g_inRemoteContext) g_origGroupBehaviour(self, edx, pad);
}

// Its photos are its game's (a snapshot it gets there comes over as a pickup): here CWeapon::TakePhotograph flashed
// this screen, saved it to this player's gallery and paid this player for a snapshot in this view
using TakePhotoFn = bool(__fastcall*)(void*, void*, CEntity*, CVector*);
TakePhotoFn g_origTakePhoto = nullptr;
bool __fastcall HookTakePhotograph(void* weapon, void* edx, CEntity* owner, CVector* point) {
    return !g_inRemoteContext && g_origTakePhoto(weapon, edx, owner, point);
}

using GetInfoFn = CPlayerInfo*(__fastcall*)(CPlayerPed*, void*);
GetInfoFn g_origGetPlayerInfo = nullptr;
CPlayerInfo* __fastcall HookGetPlayerInfoForThisPlayerPed(CPlayerPed* self, void* edx) {
    if (RemotePlayer* rp = Players::Instance().FindByPed(self))
        return PlayerInfo(kRemoteSlot)->ped == self ? PlayerInfo(kRemoteSlot) : &rp->info;
    return g_origGetPlayerInfo(self, edx);
}

// Every PLAYER1 ped gets real-time shadow number 0, and the remote player is one too (enemies attack it). The two
// players took that shadow from each other, and the one left holding it without owning it crashed the game when it was
// deleted: CPhysical::~CPhysical returns the shadow to its owner (a world load, the game's exit). The remote player
// takes a free shadow like any other ped.
using GetShadowFn = void*(__fastcall*)(void*, void*, CPed*);
GetShadowFn g_origGetShadow = nullptr;
void* __fastcall HookGetRealTimeShadow(void* manager, void* edx, CPed* e) {  // CRealTimeShadowManager::GetRealTimeShadow
    bool remote = e->IsPed() && e->PedType() == PED_TYPE_PLAYER1 && e != FindPlayerPed(0);
    if (remote) e->PedType() = PED_TYPE_PLAYER2;
    void* shadow = g_origGetShadow(manager, edx, e);
    if (remote) e->PedType() = PED_TYPE_PLAYER1;
    return shadow;
}

// Every vehicle's sound (CAEVehicleAudioEntity::Service) takes a PLAYER1 driver for the player: the remote player's
// car turned this game's radio on and took over the player's engine sounds, and with both players driving the two
// cars fought over one radio. For its sound the remote driver is not a player.
using VehicleAudioFn = void(__fastcall*)(void*, void*);
VehicleAudioFn g_origVehicleAudio = nullptr;
void __fastcall HookVehicleAudioService(void* audio, void* edx) {
    auto* veh = Field<CVehicle*>(audio, 0x4);  // CAEAudioEntity::m_pEntity
    CPed* driver = veh ? veh->Driver() : nullptr;
    bool remote = driver && driver->PedType() == PED_TYPE_PLAYER1 && driver != FindPlayerPed(0);
    if (veh) if (auto* nv = Vehicles::Instance().Find(veh); nv && Vehicles::Instance().IsMirror(*nv) && nv->hasState) {
        if(nv->state.extras&VehicleState::X_WHEELS)nv->renderedWheels.Apply(veh);
        bool horn = RemoteVehicleHorn(veh,false);
        static std::map<uint16_t,bool> horns;
        if (horn != horns[nv->netId]) { horns[nv->netId]=horn; LOGI("Vehicle %04X horn %s in native audio service",nv->netId,horn?"on":"off"); }
        int type = veh->VehicleSubClass();
        if (type != VEHICLE_TYPE_BOAT && type != VEHICLE_TYPE_PLANE && type != VEHICLE_TYPE_HELI) {
            Field<uint32_t>(veh, 0x514) = horn ? 1 : 0;
            // veh + 0x1F6 is audio.m_IsHornOn, not an input flag. Native
            // playback needs its previous value to stop an allocated sound.
        }
    }
    if (remote) driver->PedType() = PED_TYPE_PLAYER_NETWORK;
    g_origVehicleAudio(audio, edx);
    if (remote) driver->PedType() = PED_TYPE_PLAYER1;
}

// The other players' weapon icon goes next to the health bar over their heads. The HUD's sprite code draws it right
// after the HUD: the icons are textures of the weapon models' dictionaries ("<model>ICON"), the fist is a HUD sprite.
void DrawWeaponIcon(int weapon, float x, float y, float w, float h, uint8_t alpha) {
    if (weapon < 0 || weapon > kLastWeapon) return;  // from the network
    struct { void* texture; } sprite{};  // CSprite2d
    void* info = Call<0x743C60, void*>(weapon, (uint8_t)1);  // CWeaponInfo::GetWeaponInfo, standard skill
    int model = info ? Field<int32_t>(info, 0x0C) : -1;     // m_nModelId1
    if (model <= 0) {
        sprite.texture = Ref<void*>(0xBAB1FC);  // CHud::Sprites[SPRITE_FIST]
    } else {
        void* mi = Ref<void*>(addr::ModelInfoPtrs + (uintptr_t)model * 4);
        auto* txds = Ref<CPoolHeader*>(0xC8800C);  // CTxdStore::ms_pTxdPool
        int slot = mi ? Field<int16_t>(mi, 0x0A) : -1;  // m_nTxdIndex
        if (!txds || slot < 0 || slot >= txds->size || txds->byteMap[slot] < 0) return;
        void* dict = Ref<void*>((uintptr_t)txds->objects + (uintptr_t)slot * 0xC);  // TxdDef::m_pRwDictionary
        uint32_t key = Call<0x53CF70, uint32_t>(Field<uint32_t>(mi, 0x04), "ICON");  // CKeyGen::AppendStringToKey
        if (dict) sprite.texture = Call<0x734E50, void*>(dict, key);               // RwTexDictionaryFindHashNamedTexture
    }
    if (!sprite.texture) return;
    float rect[4] = {x, y + h, x + w, y};  // CRect: left, bottom, right, top
    CRGBA color{255, 255, 255, alpha};
    CallMethod<0x728350>(&sprite, rect, &color);  // CSprite2d::Draw(const CRect&, const CRGBA&)
}

// and its wanted level left of the health bar: the HUD's own stars (CHud::DrawWanted: "]" of FONT_GOTHIC in gold),
// smaller. CFont::DrawFonts prints them right after the HUD.
void DrawWantedStars(int level, float right, float midY, uint8_t alpha) {
    if (level <= 0) return;
    float scaleX = ScreenWidth() / 640.0f * 0.605f * 0.6f, scaleY = ScreenHeight() / 448.0f * 1.21f * 0.6f;
    CRGBA gold = Ref<CRGBA>(0xBAB22C + 6 * 4);  // CHudColours HudColour: HUD_COLOUR_GOLD
    gold.a = alpha;
    Call<0x7195C0>(false, false);          // CFont::SetBackground
    Call<0x719380>(scaleX, scaleY);        // CFont::SetScale
    Call<0x719610>(2);                     // CFont::SetAlignment: right
    Call<0x7195B0>(true);                  // CFont::SetProportional
    Call<0x719490>(0);                     // CFont::SetFontStyle: FONT_GOTHIC
    Call<0x719590>(1);                     // CFont::SetOutlinePosition
    Call<0x719510>(CRGBA{0, 0, 0, alpha});  // CFont::SetDropColor
    Call<0x719430>(gold);                  // CFont::SetColor
    char text[8] = {}, gxt[8] = {};
    memset(text, ']', (size_t)std::min(level, 6));
    Call<0x718600>((const char*)text, gxt);              // AsciiToGxtChar
    Call<0x71A700>(right, midY - 8.0f * scaleY, (const char*)gxt);  // CFont::PrintString
}

using HudDrawFn = void(__cdecl*)();
HudDrawFn g_origHudDraw = nullptr;
void __cdecl HookHudDraw() {
    g_origHudDraw();
    if (Net().Online()) Players::Instance().DrawWeaponIcons();
}

// A save keeps every PLAYER1 ped on foot (CPools::SavePedPool), and the remote player is one: the host's saves and the
// worlds it sent carried a copy of the partner. Loading made every such ped a CPlayerPed of player 0 sharing its data,
// so a second CJ walked by this game's pad, took the player's clothes and cleared its crosshair. Saves leave the remote
// players out, and a save that has copies already loads only the story's CJ ($PLAYER_ACTOR, main.scm's $12).
using BoolFn = bool(__cdecl*)();
BoolFn g_origSavePedPool = nullptr, g_origLoadPedPool = nullptr;
bool __cdecl HookSavePedPool() {
    std::vector<CPed*> hidden;
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.ped && IsPedValid(rp.ped) && rp.ped->PedType() == PED_TYPE_PLAYER1) {
            rp.ped->PedType() = PED_TYPE_PLAYER_NETWORK;
            hidden.push_back(rp.ped);
        }
    bool ok = g_origSavePedPool();
    for (CPed* p : hidden) p->PedType() = PED_TYPE_PLAYER1;
    return ok;
}

int g_loadCount = 0, g_loadSeen = 0;  // CPools::LoadPedPool: its player peds (it loads no other ones)
CPlayerPed* g_loadKept = nullptr;
std::vector<CPlayerPed*> g_loadDropped;

using LoadDataFn = void(__cdecl*)(void*, int);
LoadDataFn g_origLoadPedCount = nullptr;
void __cdecl HookLoadPedCount(void* dst, int size) {  // CGenericGameStorage::LoadDataFromWorkBuffer: the count
    g_origLoadPedCount(dst, size);
    g_loadCount = size == 4 ? *(int32_t*)dst : 0;
    g_loadSeen = 0;
}

using PlayerCtorFn = CPlayerPed*(__fastcall*)(CPlayerPed*, void*, int, bool);
PlayerCtorFn g_origLoadedPlayerCtor = nullptr;
CPlayerPed* __fastcall HookLoadedPlayerCtor(CPlayerPed* self, void* edx, int playerId, bool groupCreated) {
    ++g_loadSeen;
    // the one $PLAYER_ACTOR names, or the last as the game would have it
    bool keep = g_loadCount <= 1 ||
                (!g_loadKept && (GetPedRef(self) == Ref<int32_t>(addr::ScriptSpace + 12) || g_loadSeen >= g_loadCount));
    if (keep) {
        g_loadKept = self;
        return g_origLoadedPlayerCtor(self, edx, playerId, groupCreated);
    }
    g_loadDropped.push_back(self);  // a ped of player 1 with no group: nothing of player 0's is touched, it goes after
    return g_origLoadedPlayerCtor(self, edx, kRemoteSlot, true);
}

bool __cdecl HookLoadPedPool() {
    g_loadCount = g_loadSeen = 0;
    g_loadKept = nullptr;
    g_loadDropped.clear();
    bool ok = g_origLoadPedPool();
    if (g_loadKept && !g_loadDropped.empty()) {
        PlayerInfo(0)->ped = g_loadKept;  // every loaded player ped was made player 0 in turn
        for (CPlayerPed* p : g_loadDropped)
            if (IsPedValid(p)) {
                WorldRemove(p);
                CallVirtual<0, void>(p, 1u);  // scalar deleting destructor
            }
        LOGI("The save had %d player characters: the story's CJ stays, the copies are removed", g_loadCount);
    }
    g_loadDropped.clear();
    g_loadKept = nullptr;
    g_loadCount = 0;
    return ok;
}

// a player's jetpack is the simplest active task (CPedIntelligence::GetTaskJetPack)
bool HasJetpack(CPed* ped) { return ped->Intelligence() && CallMethod<0x601110, void*>(ped->Intelligence()); }

int WeaponModel(int weaponType) {
    int model = -1;
    Cmd(0x0781).I(weaponType).Out(&model).Run();  // GET_WEAPONTYPE_MODEL
    return model;
}

uint8_t ClampByte(float v) { return (uint8_t)std::clamp(v, 0.0f, 255.0f); }

constexpr int TASK_ENTER_CAR_AS_PASSENGER = 700;
constexpr int TASK_ENTER_CAR_AS_DRIVER = 701;
constexpr int TASK_LEAVE_CAR = 704;
// what the script commands (TASK_ENTER_CAR_AS_PASSENGER / _DRIVER, the G key's too) give: the plain one under it
constexpr int TASK_ENTER_CAR_AS_PASSENGER_TIMED = 712, TASK_ENTER_CAR_AS_DRIVER_TIMED = 713;

bool EnteringCar(CPed* ped) {
    return anim::FindTask(ped, TASK_ENTER_CAR_AS_PASSENGER) || anim::FindTask(ped, TASK_ENTER_CAR_AS_DRIVER) ||
           anim::FindTask(ped, TASK_ENTER_CAR_AS_PASSENGER_TIMED) || anim::FindTask(ped, TASK_ENTER_CAR_AS_DRIVER_TIMED);
}
constexpr uint64_t kEnterGraceMs = 4000;  // let the enter animation finish before warping

uint8_t WantedLevel(CPlayerPed* me) {
    void* wanted = me->PlayerData() ? me->Wanted() : nullptr;
    return wanted ? (uint8_t)std::clamp(Field<int32_t>(wanted, 0x2C), 0, 6) : 0;  // CWanted::m_nWantedLevel
}
uint8_t PoliceFlags(CPlayerPed* me) {
    void* wanted=me->PlayerData()?me->Wanted():nullptr;
    return wanted?Field<uint8_t>(wanted,0x1E)&7:0;
}

CVector CaptureAimOffset(CPlayerPed* me) {
    CVector origin, target;
    CallMethod<0x514970>((void*)addr::TheCamera, 100.0f, me->GetPosition() + CVector{0, 0, 0.7f}, &origin, &target);
    return target - me->GetPosition();
}

// AimGun reads its own task target, and several native branches assume the shooter is player 0.
// Supply a world point for remote arm/torso IK and firing, instead of its default straight-ahead target.
using AimGunFn = void(__fastcall*)(void*, void*, CPed*);
using FireGunFn = void(__fastcall*)(void*, void*, CPed*, bool);
AimGunFn g_originalAimGun = nullptr;
FireGunFn g_originalFireGun = nullptr;
struct RemoteGunTarget {
    void* task; CEntity* entity; CVector position; bool changed = false;
    RemoteGunTarget(void* t, CPed* ped) : task(t), entity(Field<CEntity*>(t, 0x1C)), position(Field<CVector>(t, 0x20)) {
        if (auto* rp = Players::Instance().FindByPed(ped)) {
            changed = true;
            Field<CEntity*>(task, 0x1C) = nullptr;
            Field<CVector>(task, 0x20) = ped->GetPosition() + rp->state.cam.aimOffset;
        }
    }
    ~RemoteGunTarget() { if (changed) { Field<CEntity*>(task, 0x1C) = entity; Field<CVector>(task, 0x20) = position; } }
};
void __fastcall HookAimGun(void* task, void* edx, CPed* ped) {
    RemoteGunTarget target(task, ped);
    g_originalAimGun(task, edx, ped);
}
void __fastcall HookFireGun(void* task, void* edx, CPed* ped, bool left) {
    RemoteGunTarget target(task, ped);
    g_originalFireGun(task, edx, ped, left);
}

void FillCommon(OnFootState& st, CPlayerPed* me) {
    st.fightingStyle = me->FightingStyle();
    st.attackMoves = me->AllowedAttackMoves();
    st.flags = me->Health() <= 0 || me->PedState() == 54 || me->PedState() == 55 ? OnFootState::F_DEAD : 0;
    st.health = ClampByte(me->Health());
    st.wanted = WantedLevel(me);
    st.policeFlags = PoliceFlags(me);
    st.armour = ClampByte(me->Armour());
    CWeapon& w = me->ActiveWeapon();
    st.weapon = (uint8_t)w.type;
    st.ammo = (uint16_t)std::clamp(w.ammoTotal, 0, 65535);
    st.area = me->AreaCode();
    CPad* pad = GetPad(0);
    if (pad->DisablePlayerControls == 0 && !game::MenuActive()) st.pad = PadState::From(pad->NewState);
    // a passenger aims out of the window with the mouse's look button, which the pad state has not: R1 (PassengerDriveBy)
    if (me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() != me && pad->NewState.m_bVehicleMouseLook)
        st.pad.buttons |= PadState::B_RS1;
    CameraState& c = st.cam;
    c.mode = cam::Mode();
    c.weaponMode = cam::WeaponMode();
    c.fov = cam::Fov();
    c.front = cam::Front();
    c.sourceOffset = cam::Source() - me->GetPosition();
    c.aimOffset = CaptureAimOffset(me);
    c.up = cam::Up();
    c.orientation = cam::Orientation();
    c.alpha = cam::Alpha();
    c.beta = cam::Beta();
    c.lookPitch = me->PlayerData() ? me->LookPitch() : 0.0f;
    c.useMouse3rd = cam::UseMouse3rdPerson() != 0;
    c.chairX = cam::CrosshairMultX();
    c.chairY = cam::CrosshairMultY();
}

void WriteCommon(Writer& w, const OnFootState& st) {
    st.pad.Write(w);
    st.cam.Write(w);
    w.U8(st.health);
    w.U8(st.armour);
    w.U8(st.weapon);
    w.U16(st.ammo);
    w.I8(st.area);
    w.U8(st.wanted);
    w.U8(st.policeFlags);
    w.U8(st.flags);
    w.U8(st.fightingStyle);
    w.U8(st.attackMoves);
    st.motion.Write(w);
}

bool ReadCommon(Reader& r, OnFootState& st) {
    st.pad.Read(r);
    st.cam.Read(r);
    st.health = r.U8();
    st.armour = r.U8();
    st.weapon = r.U8();
    st.ammo = r.U16();
    st.area = r.I8();
    st.wanted = r.U8();
    st.policeFlags = r.U8();
    if(st.wanted>6||(st.policeFlags&~7))return false;
    st.flags = r.U8();
    st.fightingStyle = r.U8();
    st.attackMoves = r.U8();
    if (st.fightingStyle < 4 || st.fightingStyle > 16) return false;
    return st.motion.Read(r);
}
}  // namespace

int ActivePrimaryTaskType(CPed* ped, void** taskOut) {
    if (taskOut) *taskOut = nullptr;
    void* intel = ped ? ped->Intelligence() : nullptr;
    if (!intel) return -1;
    for (int i = 0; i < 5; ++i) {
        void* task = Field<void*>(intel, 0x04 + i * 4);  // CTaskManager::m_aPrimaryTasks
        if (!task) continue;
        if (taskOut) *taskOut = task;
        return CallVirtual<4, int>(task);  // CTask::GetTaskType
    }
    return -1;
}

void StopEntering(CVehicle* v) {
    CPoolHeader* pool = PedPool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* ped = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        void* task = nullptr;
        int type = ActivePrimaryTaskType(ped, &task);
        bool entering = type == TASK_ENTER_CAR_AS_PASSENGER || type == TASK_ENTER_CAR_AS_DRIVER ||
                        type == TASK_ENTER_CAR_AS_PASSENGER_TIMED || type == TASK_ENTER_CAR_AS_DRIVER_TIMED;
        if (entering && Field<CVehicle*>(task, 0x0C) == v) CallMethod<0x601640>(ped->Intelligence(), true);  // FlushImmediately
    }
}

int PassengerSeatOf(CVehicle* v, CPed* ped) {
    if (!v || !ped) return -2;
    if (v->Driver() == ped) return -1;
    for (int i = 0; i < 8; ++i)
        if (v->Passengers()[i] == ped) return i;
    return -2;
}

int SeatOfTargetDoor(int door) {
    switch (door) {
        case 11: return 1;  // rear left
        case 9: return 2;   // rear right
        default: return 0;  // front right (8) and anything else
    }
}

int SeatOrAny(CVehicle* v, int seat) { return v && seat >= 0 && seat < v->MaxPassengers() ? seat : -1; }

float RemoteDistance(const CVector& p, int8_t area, uint8_t* who) {
    float best = 1e9f;
    for (auto& [id, rp] : Players::Instance().All())
        if (float d = (rp.state.pos - p).Length(); rp.hasState && NowMs() - rp.stateRecvMs < 5000 && rp.state.area == area && d < best) {
            best = d;
            if (who) *who = id;
        }
    return best;
}

// ------------------------------------------------------------------ module
void Players::OnInstallHooks() {
    mem::HookFn(0x61ED10, &HookAimGun, &g_originalAimGun, "CTaskSimpleUseGun::AimGun");
    mem::HookFn(0x61EB10, &HookFireGun, &g_originalFireGun, "CTaskSimpleUseGun::FireGun");
    mem::HookFn(0x67E7B0,&JetpackInput,&originalJetpackInput,"CTaskSimpleJetPack::ProcessControlInput");
    mem::HookFn(0x67EF20,&JetpackThrust,&originalJetpackThrust,"CTaskSimpleJetPack::ProcessThrust");
    mem::HookFn(0x60EA90, &HookPlayerProcessControl, &g_origProcessControl, "CPlayerPed::ProcessControl");
    void* coopGame = nullptr;  // CGameLogic::IsCoopGameGoingOn, always false (Patches)
    mem::RedirectCall(0x644A1A, (const void*)&PassengerDriveBy, &coopGame);
    mem::Nop(0x644A27, 28);  // the SMG slot holds a weapon with ammo (PassengerDriveBy checks the gun in hand)
    mem::Nop(0x644A69, 6);   // jne: the camera is MODE_TWOPLAYER_IN_CAR_AND_SHOOTING
    mem::HookFn(0x62D3B0, &HookGangDriveByProcess, &g_origGangDriveBy, "CTaskSimpleGangDriveBy::ProcessPed");
    mem::HookFn(0x442060, &HookRestorePlayer, &g_origRestore, "CGameLogic::RestorePlayerStuffDuringResurrection");
    mem::RedirectCallFn(0x4677A5, &HookDeleteChar, &g_origRemoveThisPed);  // DELETE_CHAR -> CTheScripts::RemoveThisPed
    mem::HookFn(0x609FF0, &HookGetPlayerInfoForThisPlayerPed, &g_origGetPlayerInfo,
                "CPlayerPed::GetPlayerInfoForThisPlayerPed");
    mem::HookFn(0x706970, &HookGetRealTimeShadow, &g_origGetShadow, "CRealTimeShadowManager::GetRealTimeShadow");
    mem::HookFn(0x502280, &HookVehicleAudioService, &g_origVehicleAudio, "CAEVehicleAudioEntity::Service");
    mem::HookFn(0x58FAE0, &HookHudDraw, &g_origHudDraw, "CHud::Draw");
    mem::RedirectCallFn(0x60F595, &HookGroupBehaviour, &g_origGroupBehaviour);  // CPlayerPed::ProcessControl
    mem::RedirectCallFn(0x74278B, &HookTakePhotograph, &g_origTakePhoto);      // CWeapon::Fire, the camera
    mem::RedirectCallFn(0x5D0880, &HookSavePedPool, &g_origSavePedPool);  // CPools::Save
    mem::RedirectCallFn(0x5D0890, &HookLoadPedPool, &g_origLoadPedPool);  // CPools::Load
    mem::RedirectCallFn(0x5D2D95, &HookLoadPedCount, &g_origLoadPedCount);  // in CPools::LoadPedPool
    mem::RedirectCallFn(0x5D2E07, &HookLoadedPlayerCtor, &g_origLoadedPlayerCtor);
    Net().SetHandler(proto::G_PLAYER_ONFOOT, [](uint8_t from, Reader& r) { Instance().HandleOnFoot(from, r); });
    Net().SetHandler(proto::G_TELEPORT, [](uint8_t from, Reader& r) { Instance().HandleTeleport(from, r); });
    Net().SetHandler(proto::G_PLAYER_DRIVER, [](uint8_t from, Reader& r) { Instance().HandleDriver(from, r); });
    Net().SetHandler(proto::G_PLAYER_PASSENGER, [](uint8_t from, Reader& r) { Instance().HandlePassenger(from, r); });
    Net().SetHandler(proto::G_PLAYER_ENTER_CAR, [](uint8_t from, Reader& r) { Instance().HandleEnterCar(from, r); });
    Net().SetHandler(proto::G_PLAYER_EXIT_CAR, [](uint8_t from, Reader& r) { Instance().HandleExitCar(from, r); });
    Net().SetHandler(proto::G_PLAYER_CARJACK, [](uint8_t from, Reader& r) { Instance().HandleCarjack(from, r); });
    Net().SetHandler(proto::G_SEAT_ASSIGN,[](uint8_t from,Reader&r){
        auto vehicle=r.U16();auto serial=r.U32();auto mask=r.U8();std::array<int8_t,3>seats;for(auto&s:seats)s=r.I8();
        if(!r.Ok()||(mask&~7u)||!serial||!core::IsPlayerReady())return;
        auto*nv=Vehicles::Instance().Find(vehicle);if(!nv||!Vehicles::Alive(*nv)||nv->owner!=from)return;
        for(auto s:seats)if(s < -2||s>=std::clamp((int)nv->veh->MaxPassengers(),0,8))return;
        auto&self=Instance();auto&last=self.m_seatReceived[((uint32_t)from<<16)|vehicle];
        if(last&&(int32_t)(serial-last)<=0)return;last=serial;self.ApplySeatAssignment(vehicle,mask,seats);
    });
}

RemotePlayer* Players::FindByPed(CPed* ped) {
    if (!ped) return nullptr;
    for (auto& [id, rp] : m_remote)
        if (rp.ped == ped) return &rp;
    return nullptr;
}

RemotePlayer* Players::Get(uint8_t id) {
    auto it = m_remote.find(id);
    return it == m_remote.end() ? nullptr : &it->second;
}

bool Players::SetModel(CPlayerPed* ped, int model) {
    if (ped->ModelIndex() == model) return true;
    bool special = model >= 290 && model <= 299;
    if (model != 0 && (!IsPedModel(model) || (special ? !IsModelLoaded(model) : !LoadModelNow(model)))) return false;
    int32_t walk = Field<int32_t>(ped, 0x4D4);  // CPed::m_nAnimGroup
    // The copy's tasks hold animations of the old clump (a car door's, a fight's, the look-at's bones) and touched them
    // freed later: SET_PLAYER_MODEL is for an idle player. Flushed first, out of a car first (the flush takes the drive
    // task, the next state seats it again).
    if (ped->InVehicle()) {
        CVector p = ped->GetPosition();
        Cmd(0x0362).I(GetPedRef(ped)).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    }
    CallMethod<0x601640>(ped->Intelligence(), true);  // CPedIntelligence::FlushImmediately
    CallVirtual<8>(ped);                        // CEntity::DeleteRwObject
    Field<uint16_t>(ped, 0x22) = 0xFFFF;        // CEntity::m_nModelIndex: SetModelIndex rebuilds only a new one
    CallVirtual<5>(ped, (uint32_t)model);       // CPed::SetModelIndex
    Field<int32_t>(ped, 0x4D4) = walk;
    return true;
}

void Players::OnSessionStart() {
    m_lastSend = 0;
    for (auto& [id, peer] : Net().Peers()) Touch(id);  // reliable stats can precede the first movement packet
}

void Players::OnPeerJoin(uint8_t id) { Touch(id); }
void Players::OnPeerResync(uint8_t id) {
    if(auto*rp=Get(id)){
        rp->history.Clear();rp->hasState=false;rp->lastStateTime=0;
        rp->seatTransition={};rp->enteringVeh=0;rp->enterTaskMs=rp->exitTaskMs=0;
        rp->padNew=rp->padOld={};rp->padFresh=false;
    }
    m_lastSend=0;
}

void Players::OnSessionEnd() {
    m_seatReceived.clear();m_seatCheck=0;
    ReleaseMeleeBlocks();
    m_rider=-1;m_riderReleaseUntil=0;
    for (auto& [id, rp] : m_remote) DestroyRemote(rp);
    m_remote.clear();
    g_ourDriveBy.clear();
}

void Players::OnPeerLeave(uint8_t id) {
    auto it = m_remote.find(id);
    if (it == m_remote.end()) return;
    DestroyRemote(it->second);
    m_remote.erase(it);
}

void Players::OnGameStarted() {
    m_rider=-1;m_riderReleaseUntil=0;
    Cmd(0x0501).I(0).I(1).Run();  // SET_PLAYER_CAN_DO_DRIVE_BY: passengers may shoot from the window
    g_ourDriveBy.clear();
    m_enterReported = nullptr;
    m_lastCar = -1;  // another world
    m_exitReported = false;
    // world was (re)created: our peds are gone, they will be respawned from the next states
    for (auto& [id, rp] : m_remote) {
        rp.ped = nullptr;
        rp.pedHandle = -1;
        rp.blip = 0;
        rp.history.Clear();rp.hasState=false;rp.lastStateTime=0;
        rp.seatTransition={};rp.enteringVeh=0;rp.enterTaskMs=rp.exitTaskMs=0;
        rp.deadShown=false;rp.scriptAnimFull=false;rp.padNew=rp.padOld={};
    }
    // a reload of the host's world after its progress: back where the partner stood, as it was
    if (flow::SavedSpot s; Net().Online() && !Net().IsHost() && flow::TakeSavedSpot(s)) {
        CPlayerPed* me = FindPlayerPed(0);
        int h = GetPedRef(me);
        Cmd(0x04BB).I(s.area).Run();                          // SET_AREA_VISIBLE
        Cmd(0x0860).I(h).I(s.area).Run();                     // SET_CHAR_AREA_VISIBLE
        Cmd(0x03CB).F(s.x).F(s.y).F(s.z).Run();               // LOAD_SCENE: its ground first
        Cmd(0x00A1).I(h).F(s.x).F(s.y).F(s.z - 1.0f).Run();   // SET_CHAR_COORDINATES
        LightArea(s.area, {s.x, s.y, s.z});  // the host's world came with the light of where the host stood
        me->CurrentRotation() = me->AimingRotation() = s.heading;
        me->Health() = s.health;
        me->Armour() = s.armour;
        Cmd(0x048F).I(h).Run();  // REMOVE_ALL_CHAR_WEAPONS: the host's ones came with its world
        for (int i = 0; i < 13; ++i)
            if (s.weapons[i] > 0) {
                int model = WeaponModel(s.weapons[i]);
                if (model > 0 && !IsModelLoaded(model)) LoadModelNow(model);
                Cmd(0x01B2).I(h).I(s.weapons[i]).I(std::max(s.ammo[i], 1)).Run();  // GIVE_WEAPON_TO_CHAR
            }
        Cmd(0x01B9).I(h).I(s.activeWeapon).Run();  // SET_CURRENT_CHAR_WEAPON
        if (s.carModel > 0 && IsVehicleModel(s.carModel) && HasRoom(VehiclePool(), kKeepVehicles, "vehicle") &&
            LoadModelNow(s.carModel)) {  // the car left next to it
            // Vehicles restores recent cars during the same world reload.
            // Reuse that identity instead of creating another physical copy.
            auto* known=Vehicles::Instance().Find(s.carNetId);
            int car = known&&known->model==s.carModel?known->handle:-1;
            if(car<0)Cmd(0x00A5).I(s.carModel).F(s.carX).F(s.carY).F(s.carZ).Out(&car).Run();
            if (CVehicle* v = GetVehicle(car)) {
                Cmd(0x0175).I(car).F(s.carHeading * 57.29578f).Run();  // SET_CAR_HEADING
                memcpy(v->Colors(), s.carColors, 4);
                v->Health() = s.carHealth;
                Cmd(0x01C3).I(car).Run();  // MARK_CAR_AS_NO_LONGER_NEEDED: an ordinary car, as before
                v->AreaCode() = s.carArea;
                VehicleSpawn restored;
                restored.netId = s.carNetId;
                restored.model = (uint16_t)s.carModel;
                restored.owner = Net().MyId();
                restored.pos = v->GetPosition();
                if (v->Matrix()) restored.rot = Quat::FromMatrix(*v->Matrix());
                restored.area = s.carArea;
                restored.playerUsed = true;
                memcpy(restored.colors, s.carColors, 4);
                restored.health = s.carHealth;
                m_lastCar = car;
                Vehicles::Instance().RestorePlayerCar(v, restored);
                LOGI("The car left next to the partner (model %d) is back", s.carModel);
            }
        }
        LOGI("Back at the spot before the reload of the host's world");
        return;
    }
    // the host's world puts the partner where the host stands: a step aside, where nothing is in the way
    if (Net().Online() && !Net().IsHost()) {
        CPlayerPed* me = FindPlayerPed(0);
        CVector p = me->GetPosition();
        float h = me->CurrentRotation();
        float preferredSide = proto::PartnerIndex(Net().MyId(), Net().HostId()) ? -1.2f : 1.2f;
        for (float side : {preferredSide, -preferredSide}) {
            CVector q = p + CVector{std::cos(h) * side, std::sin(h) * side, 0};
            if (!Call<0x56A490, bool>(&p, &q, true, true, false, true, false, true, false)) continue;  // GetIsLineOfSightClear
            Cmd(0x00A1).I(GetPedRef(me)).F(q.x).F(q.y).F(q.z - 1.0f).Run();  // SET_CHAR_COORDINATES
            break;
        }
    }
}

void Players::OnGameShutdown() {
    m_seatReceived.clear();m_seatCheck=0;
    ReleaseMeleeBlocks();
    // the remote players go before the world is torn down (a world loaded in the game: the prologue's end)
    for (auto& [id, rp] : m_remote) {
        DestroyRemote(rp);
        rp.blip = 0;
    }
    PlayerInfo(kRemoteSlot)->ped = nullptr;
}

void Players::SpawnRemote(RemotePlayer& rp) {
    if (!HasRoom(PedPool(), 2, "ped")) return;
    if (rp.ped || !rp.hasState || !core::IsPlayerReady()) return;
    CPlayerInfo* slot = PlayerInfo(kRemoteSlot);
    if (slot->ped) return;  // slot busy (should not happen)
    if (!rp.infoInitialized) {
        // The constructor initialises the embedded data with null wanted/clothes pointers. CREATE_PLAYER then
        // allocates these for this remote alone; reusing slot 1's old pointers would alias both remote players.
        CallMethod<0x571920>(&rp.info);  // CPlayerInfo::CPlayerInfo
        rp.info.maxHealth = rp.info.maxArmour = 100;
        rp.infoInitialized = true;
    }
    CPlayerInfo savedSlot = *slot;
    *slot = rp.info;
    const CVector p = rp.state.pos;
    int playerIndex = -1, handle = -1;
    // made high above its place: CREATE_PLAYER clears the space around itself of this game's random peds and cars
    // (the taxi the other player sits in), and it goes where it is right after
    Cmd(0x0053).I(kRemoteSlot).F(p.x).F(p.y).F(p.z + 300.0f).Out(&playerIndex).Run();  // CREATE_PLAYER
    Cmd(0x01F5).I(kRemoteSlot).Out(&handle).Run();                            // GET_PLAYER_CHAR
    Cmd(0x0501).I(kRemoteSlot).I(1).Run();  // SET_PLAYER_CAN_DO_DRIVE_BY: its drive-bys show here (CPlayerInfo +0x153)
    auto* ped = (CPlayerPed*)GetPed(handle);
    rp.info = *slot;
    *slot = savedSlot;
    if (!ped) {
        LOGE("Failed to create the remote player ped");
        return;
    }
    ped->PlayerData() = rp.info.PlayerData();
    rp.group = Field<int32_t>(ped->PlayerData(), 0x38);  // CPlayerPedData::m_nPlayerGroup
    Cmd(0x00A1).I(handle).F(p.x).F(p.y).F(p.z - 1.0f).Run();  // SET_CHAR_COORDINATES (feet)
    ped->PedType() = PED_TYPE_PLAYER1;  // enemies hostile to the player attack the partner too
    ped->SetProofs(true, true, true, true, true);  // damage is decided by its owner
    ped->SetStreamingDontDelete(true);
    Cmd(0x039E).I(handle).I(0).Run();  // allow native car-jacking responses
    ped->AreaCode() = rp.state.area;
    rp.ped = ped;
    CallMethod<0x571B70>(ped, (CEntity**)&rp.ped);
    rp.pedHandle = handle;
    rp.padNew = rp.padOld = CControllerState{};
    // radar blip
    int blip = 0;
    Cmd(0x0187).I(handle).Out(&blip).Run();  // ADD_BLIP_FOR_CHAR
    if (blip) {
        Cmd(0x07E0).I(blip).I(1).Run();       // SET_BLIP_AS_FRIENDLY
        Cmd(0x0168).I(blip).I(2).Run();       // CHANGE_BLIP_SCALE
        Cmd(0x018B).I(blip).I(2).Run();       // CHANGE_BLIP_DISPLAY: radar only, no 3D arrow
        rp.blip = blip;
    }
    auto*local=FindPlayerPed(0);
    LOGI("Remote player %d spawned (ped %p, handle %d), wanted local=%u remote=%u separate=%d",rp.id,(void*)ped,handle,
        local&&local->PlayerData()?WantedLevel(local):0,WantedLevel(ped),!local||!local->PlayerData()||local->Wanted()!=ped->Wanted());
}

void Players::DestroyRemote(RemotePlayer& rp) {
    if (rp.blip) {
        Cmd(0x0164).I(rp.blip).Run();  // REMOVE_BLIP
        rp.blip = 0;
    }
    if (rp.group >= 0 && rp.group < 8) Call<0x5FB870>(rp.group);  // CPedGroups::RemoveGroup
    rp.group = -1;
    if (rp.ped && IsPedValid(rp.ped)) {
        CPed* ped = rp.ped;
        if(ped->InVehicle()){
            auto p=ped->GetPosition();Cmd(0x0362).I(rp.pedHandle).F(p.x).F(p.y).F(p.z+1).Run();
        }
        if (PlayerInfo(kRemoteSlot)->ped == ped) PlayerInfo(kRemoteSlot)->ped = nullptr;
        if (CPlayerPed* me = FindPlayerPed(0); me && Field<CEntity*>(me, 0x128) == ped) Field<CEntity*>(me, 0x128) = nullptr;  // KeepApart
        WorldRemove(ped);
        Call<0x565510>(ped);
        CallVirtual<0, void>(ped, 1u);  // scalar deleting destructor (returns the slot to the ped pool)
        g_ourDriveBy.erase(ped);
        LOGI("Remote player %d removed", rp.id);
    }
    rp.ped = nullptr;
    rp.pedHandle = -1;
    // CPed's destructor leaves CPlayerPedData to CPlayerInfo. Our private info is outside CWorld::Players.
    if (rp.infoInitialized) CallMethod<0x571840>(rp.info.PlayerData());  // CPlayerPedData::~CPlayerPedData
    rp.info = {};
    rp.infoInitialized = false;
}

void Players::HandleOnFoot(uint8_t from, Reader& r) {
    if (from == Net().MyId()) return;
    OnFootState st;
    if (!st.Read(r)) return;
    RemotePlayer& rp = Touch(from);
    // unreliable channel: ignore states older than the newest applied one
    if(rp.hasState&&(int32_t)(st.motion.epoch-rp.state.motion.epoch)<0)return;
    if (rp.hasState && (int32_t)(st.time - rp.lastStateTime) < 0) return;
    rp.lastStateTime = st.time;
    if(!rp.hasState||rp.mode!=RemotePlayer::Mode::OnFoot||rp.state.area!=st.area||
       rp.state.motion.epoch!=st.motion.epoch||(st.pos-rp.state.pos).Length()>25)rp.history.Clear();
    rp.history.Push(st,NowMs());
    // enter/exit are replicated as events; with the target held the button is the fight's second attack (a kick,
    // CTaskSimplePlayerOnFoot: MeleeAttackJustDown 4), which only the pad replay shows
    rp.previousStoryPad=rp.storyPad;rp.storyPad=st.pad;
    if (!(st.pad.buttons & PadState::B_RS1)) st.pad.buttons &= ~PadState::B_TRIANGLE;
    if (!rp.seatTransition.leaving && (rp.mode != RemotePlayer::Mode::OnFoot || (rp.ped && rp.ped->InVehicle())))
        rp.seatTransition.Begin(rp.vehNetId, st.time, NowMs());
    ClearChangedSupport(rp.ped,rp.state.motion.objectSupport?0:rp.state.motion.support,st.motion.objectSupport?0:st.motion.support);
    rp.state = st;
    rp.hasState = true;
    rp.stateRecvMs = NowMs();
    rp.padFresh = true;
    rp.mode = RemotePlayer::Mode::OnFoot;
    rp.seatTransition.OnFoot(NowMs());
}

RemotePlayer& Players::Touch(uint8_t from) {
    RemotePlayer& rp = m_remote[from];
    rp.id = from;
    return rp;
}

void Players::HandleDriver(uint8_t from, Reader& r) {
    if (from == Net().MyId()) return;
    uint16_t netId = r.U16();
    VehicleState vs;
    vs.Read(r);
    OnFootState st;
    if(!ReadCommon(r, st))return;
    if (!r.Ok()) return;
    RemotePlayer& rp = Touch(from);
    if(rp.hasState&&(int32_t)(st.motion.epoch-rp.state.motion.epoch)<0)return;
    if (rp.hasState && (int32_t)(vs.time - rp.lastStateTime) < 0) return;
    if (rp.mode == RemotePlayer::Mode::OnFoot && rp.seatTransition.confirmed) rp.seatTransition = {};
    if (rp.seatTransition.jacked && rp.seatTransition.HoldSeat(netId, NowMs())) return;
    rp.lastStateTime = vs.time;
    st.time = vs.time;
    st.pos = vs.pos;
    st.vel = vs.vel;
    rp.previousStoryPad=rp.storyPad;rp.storyPad=st.pad;
    st.pad.buttons &= ~PadState::B_TRIANGLE;
    ClearChangedSupport(rp.ped,rp.state.motion.objectSupport?0:rp.state.motion.support,st.motion.objectSupport?0:st.motion.support);
    rp.state = st;
    rp.hasState = true;
    rp.stateRecvMs = NowMs();
    rp.padFresh = true;
    rp.mode = RemotePlayer::Mode::Driver;
    rp.history.Clear();
    rp.vehNetId = netId;
    rp.seat = -1;
    Vehicles::Instance().ApplyDriverState(from, netId, vs);
}

void Players::HandlePassenger(uint8_t from, Reader& r) {
    if (from == Net().MyId()) return;
    uint16_t netId = r.U16();
    int8_t seat = r.I8();
    uint32_t time = r.U32();
    Vec3 p = r.Vec();
    OnFootState st;
    if(!ReadCommon(r, st))return;
    if (!r.Ok()) return;
    RemotePlayer& rp = Touch(from);
    if(rp.hasState&&(int32_t)(st.motion.epoch-rp.state.motion.epoch)<0)return;
    if (rp.hasState && (int32_t)(time - rp.lastStateTime) < 0) return;
    if (rp.mode == RemotePlayer::Mode::OnFoot && rp.seatTransition.confirmed) rp.seatTransition = {};
    if (rp.seatTransition.jacked && rp.seatTransition.HoldSeat(netId, NowMs())) return;
    rp.lastStateTime = time;
    st.time = time;
    st.pos = {p.x, p.y, p.z};
    rp.previousStoryPad=rp.storyPad;rp.storyPad=st.pad;
    st.pad.buttons &= ~PadState::B_TRIANGLE;
    ClearChangedSupport(rp.ped,rp.state.motion.objectSupport?0:rp.state.motion.support,st.motion.objectSupport?0:st.motion.support);
    rp.state = st;
    rp.hasState = true;
    rp.stateRecvMs = NowMs();
    rp.padFresh = true;
    rp.mode = RemotePlayer::Mode::Passenger;
    rp.history.Clear();
    rp.vehNetId = netId;
    rp.seat = seat;
}

void Players::HandleEnterCar(uint8_t from, Reader& r) {
    uint16_t netId = r.U16();
    int8_t seat = r.I8();
    uint32_t time = r.U32();
    if (!r.Ok() || from == Net().MyId()) return;
    RemotePlayer* rp = Get(from);
    if (!rp||seat < -1||seat>7) return;
    if(rp->enterEventTime&&(int32_t)(time-rp->enterEventTime)<=0)return;
    if(rp->hasState&&(int32_t)(time-rp->lastStateTime)<0&&rp->mode==RemotePlayer::Mode::OnFoot)return;
    if (rp->seatTransition.leaving && (int32_t)(time - rp->seatTransition.senderTime) < 0) return;
    rp->enterEventTime=time;rp->enterPending=true;
    rp->seatTransition = {};
    rp->enteringVeh = netId;
    rp->enteringSeat = seat;
    StartPendingEnter(*rp);
}
void Players::StartPendingEnter(RemotePlayer&rp){
    if(!rp.enterPending||!rp.ped)return;
    if(rp.hasState&&(int32_t)(rp.lastStateTime-rp.enterEventTime)>0&&rp.mode!=RemotePlayer::Mode::OnFoot){rp.enterPending=false;return;}
    auto*nv=Vehicles::Instance().Find(rp.enteringVeh);if(!nv||!Vehicles::Alive(*nv))return;
    if(rp.enteringSeat>=0&&rp.enteringSeat>=nv->veh->MaxPassengers()){rp.enterPending=false;return;}
    rp.enterPending=false;rp.enterTaskMs=NowMs();
    if(rp.enteringSeat<0)Cmd(0x05CB).I(rp.pedHandle).I(nv->handle).I(10000).Run();
    else Cmd(0x05CA).I(rp.pedHandle).I(nv->handle).I(10000).I(rp.enteringSeat).Run();
}

void Players::HandleExitCar(uint8_t from, Reader& r) {
    uint16_t netId = r.U16();
    uint32_t time = r.U32();
    if (!r.Ok() || from == Net().MyId()) return;
    RemotePlayer* rp = Get(from);
    NetVehicle* nv = Vehicles::Instance().Find(netId);
    if (!rp || !rp->ped || !nv || !Vehicles::Alive(*nv)) return;
    if(rp->hasState&&(int32_t)(time-rp->lastStateTime)<0&&rp->mode==RemotePlayer::Mode::OnFoot)return;
    rp->enterPending=false;
    // The owner acknowledges a native drag-out with the ordinary exit event.
    // Do not replace that physical response with TASK_LEAVE_CAR halfway through
    // the pull, or clear its protection against stale driver snapshots.
    if(rp->seatTransition.jacked&&rp->seatTransition.HoldSeat(netId,NowMs()))return;
    if (!rp->seatTransition.Begin(netId, time, NowMs())) return;
    if (rp->ped->InVehicle() && rp->ped->Vehicle() == nv->veh) {
        rp->exitTaskMs = NowMs();
        Cmd(0x05CD).I(rp->pedHandle).I(nv->handle).Run();  // TASK_LEAVE_CAR
    }
}

void Players::HandleCarjack(uint8_t from, Reader& r) {
    uint16_t vehicle = r.U16();
    uint8_t victim = r.U8(), door = r.U8();
    uint32_t time = r.U32();
    if (!r.Ok() || from == Net().MyId() || victim == from || door < 8 || door > 11) return;
    auto* aggressor = Get(from);
    auto* nv = Vehicles::Instance().Find(vehicle);
    if (!aggressor || !aggressor->ped || !nv || !IsVehicleValid(nv->veh)) return;
    CPed* ped = victim == Net().MyId() ? FindPlayerPed(0) : (Get(victim) ? Get(victim)->ped : nullptr);
    if (!ped || !ped->InVehicle() || ped->Vehicle() != nv->veh || ped->AreaCode() != aggressor->state.area ||
        (aggressor->ped->GetPosition() - nv->veh->GetPosition()).Length() > 10.0f) return;
    if(nv->veh->Driver()==ped)Vehicles::Instance().ExpectCarjack(vehicle,from);
    if (auto* remote = Get(victim)) {
        remote->seatTransition.Begin(vehicle, remote->lastStateTime, NowMs(), true);
        remote->exitTaskMs = NowMs();
    }
    if (nv->veh->VehicleSubClass() == VEHICLE_TYPE_BIKE && !anim::FindTask(ped, 826)) {
        if (void* task = Call<0x61A5A0, void*>(0x2Cu)) {
            CallMethod<0x648B90>(task, nv->veh, (uint32_t)door, (uint32_t)TimeMs(), aggressor->ped, nv->veh->Driver() == ped);
            CallMethod<0x681AF0>((uint8_t*)ped->Intelligence() + 4, task, 0, false);
        }
    } else if (nv->veh->VehicleSubClass() != VEHICLE_TYPE_BIKE && !anim::FindTask(ped, 824) && !anim::FindTask(ped, 823)) {
        // Native slow drag-out + stand-up, allocated from GTA's task pool.
        if (void* task = Call<0x61A5A0, void*>(0x14u)) {
            CallMethod<0x648620>(task, nv->veh, (int)door);
            CallMethod<0x681AF0>((uint8_t*)ped->Intelligence() + 4, task, 0, false);
        }
    }
    LOGI("Player %u drags player %u from vehicle %04X (time %u)", from, victim, vehicle, time);
}

void Players::HandleTeleport(uint8_t from, Reader& r) {
    uint8_t target = r.U8();
    CVector pos{r.F32(), r.F32(), r.F32()};
    int8_t area = r.I8();
    if (!r.Ok() || target != Net().MyId() || !core::IsPlayerReady()) return;
    CPlayerPed* me = FindPlayerPed(0);
    int handle = GetPedRef(me);
    Cmd(0x04BB).I(area).Run();                 // SET_AREA_VISIBLE
    Cmd(0x0860).I(handle).I(area).Run();       // SET_CHAR_AREA_VISIBLE
    Cmd(0x00A1).I(handle).F(pos.x).F(pos.y).F(pos.z).Run();  // SET_CHAR_COORDINATES
    LightArea(area, pos);
    ui::TheChat().AddSystem(Format(TR("%s переместил вас к себе.", "%s brought you over."), Net().PeerName(from).c_str()));
}

void Players::ApplyState(RemotePlayer& rp) {
    CPlayerPed* ped = rp.ped;
    const OnFootState& st = rp.state;

    // pad: "old" is always the previous frame, "new" only changes when a packet arrived
    rp.padOld = rp.padNew;
    if (NowMs() - rp.stateRecvMs > 500) { rp.padNew = {}; rp.padFresh = false; }
    if (rp.padFresh) {
        st.pad.To(rp.padNew);
        rp.padFresh = false;
    }
    if (rp.scriptAnimFull) rp.padNew = {};

    // death is decided by the owner's game (the local ped has all proofs): play it once; after the
    // respawn a fresh ped replaces the dead one, which cannot be revived
    if (st.flags & OnFootState::F_DEAD) {
        if (!rp.deadShown) {
            rp.deadShown = true;
            LOGI("Remote player %d died", rp.id);
            if (!ped->InVehicle()) Cmd(0x05BE).I(rp.pedHandle).Run();  // owner sends native in-car death pose
        }
        return;
    }
    if (rp.deadShown) {
        rp.deadShown = false;
        DestroyRemote(rp);  // SpawnRemote runs next frame
        return;
    }

    ped->Health() = (float)st.health;
    ped->Armour() = (float)st.armour;
    // ScenePartner owns these native peds for the cutscene. Old seat, phone,
    // weapon and jetpack tasks must not replace its staged animation.
    if(Ref<uint8_t>(0xB5F851)){rp.padNew={};rp.padOld={};return;}
    if (!Ref<uint8_t>(0xB5F851)) ped->AreaCode() = st.area;  // CCutsceneMgr::ms_running: the scene sets it (Missions)

    // crouching toggles on the crouch button's press, which the copy replays from the owner's pad: a press lost on the
    // way (or a press and its release in one frame) left the copy standing while its owner crouched, and every next
    // press the other way round. A difference that lasts is the owner's state put right.
    bool duck = (st.flags & OnFootState::F_DUCKING) != 0;
    if (duck == ped->IsDucking() || rp.mode != RemotePlayer::Mode::OnFoot) {
        rp.duckOffMs = 0;
    } else if (!rp.duckOffMs) {
        rp.duckOffMs = NowMs();
    } else if (NowMs() - rp.duckOffMs > 400 && ped->Intelligence()) {
        if (duck) CallMethod<0x601230>(ped->Intelligence(), (uint16_t)0);  // CPedIntelligence::SetTaskDuckSecondary
        else CallMethod<0x601390>(ped->Intelligence());                    // CPedIntelligence::ClearTaskDuckSecondary
        rp.duckOffMs = 0;
        LOGD("Remote player %d: crouch %s to match its owner", rp.id, duck ? "on" : "off");
    }

    // a phone call (CTaskComplexUseMobilePhone): the phone is put in the hand as a weapon's model, and the weapon's
    // comes back after it (CPed__removeCellphone); the animations go with Anims
    bool phone = (st.flags & OnFootState::F_PHONE) != 0;
    if (phone != (Field<int32_t>(ped, 0x740) == kCellphone) && LoadModelNow(kCellphone)) {
        if (phone) {
            CallMethod<0x5E5ED0>(ped, kCellphone);  // CPed::AddWeaponModel
        } else {
            CallMethod<0x5E3990>(ped, kCellphone);                       // CPed::RemoveWeaponModel
            CallMethod<0x5E6280>(ped, (int)ped->ActiveWeapon().type);  // CPed::SetCurrentWeapon: its gun again
        }
        LOGI("Remote player %d: phone %s", rp.id, phone ? "out" : "away");
    }

    // the jetpack flies by the player's buttons, which this ped gets: it only needs one. Dropping it (the enter key)
    // happens here as there; whatever is left over goes with the flag
    bool jetpack = (st.flags & OnFootState::F_JETPACK) != 0;
    if (jetpack != HasJetpack(ped) && rp.mode == RemotePlayer::Mode::OnFoot && NowMs() - rp.jetpackMs > 250) {
        rp.jetpackMs = NowMs();
        if (jetpack) {
            RequestModel(370);
            if(IsModelLoaded(370))Cmd(0x07A7).I(rp.pedHandle).Run();   // TASK_JETPACK
        } else {
            if(auto*task=CallMethod<0x601110,void*>(ped->Intelligence()))CallMethod<0x67E6E0,bool>(task,ped,0,(void*)nullptr);
        }
        LOGI("Remote player %d: jetpack %s", rp.id, jetpack ? "on" : "off");
    }

    // weapon in hand
    CWeapon& active = ped->ActiveWeapon();
    int weapon = st.weapon <= kLastWeapon ? st.weapon : 0;  // from the network
    if (active.type != weapon && NowMs() - rp.lastWeaponTry > 500) {
        rp.lastWeaponTry = NowMs();
        if (weapon != 0) {
            int model = WeaponModel(weapon);
            if (model > 0 && !IsModelLoaded(model)) LoadModelNow(model);
            Cmd(0x01B2).I(rp.pedHandle).I(weapon).I(std::max<int>(st.ammo, 1)).Run();  // GIVE_WEAPON_TO_CHAR
        }
        Cmd(0x01B9).I(rp.pedHandle).I(weapon).Run();  // SET_CURRENT_CHAR_WEAPON
    } else if (weapon != 0) {
        active.ammoTotal = std::max<int>(st.ammo, 1);  // never run dry locally: the owner decides
        if (active.ammoInClip <= 0) active.ammoInClip = 1;
    }

    if (rp.mode != RemotePlayer::Mode::OnFoot) {
        ApplyVehicleSeat(rp);
        return;
    }
    if (st.flags & OnFootState::F_IN_TRAIN) {
        RideTrain(rp);
        return;
    }
    // on foot but still sitting in a vehicle here: the exit animation gets a few seconds, then warp out
    if (ped->InVehicle()) {
        rp.seatTransition.OnFoot(NowMs());
        if (!rp.exitTaskMs && !anim::FindTask(ped, TASK_LEAVE_CAR)) {
            rp.exitTaskMs = NowMs();
            Cmd(0x05CD).I(rp.pedHandle).I(GetVehicleRef(ped->Vehicle())).Run();
        }
        if (rp.seatTransition.MustDetach(NowMs()) || NowMs() - rp.exitTaskMs > kEnterGraceMs) {
            Cmd(0x0362).I(rp.pedHandle).F(st.pos.x).F(st.pos.y).F(st.pos.z - 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD (feet)
            rp.exitTaskMs = NowMs();
        }
        return;
    }

    if (Ref<uint8_t>(0xB5F851)) return;  // CCutsceneMgr::ms_running: Missions places the ped in the scene

    // position: predict, then correct smoothly (teleport on large errors)
    auto sampled=rp.history.At(NowMs(),[](const OnFootState&a,const OnFootState&b,float t){
        auto s=b;s.pos=a.pos+(b.pos-a.pos)*t;s.vel=a.vel+(b.vel-a.vel)*t;
        if(a.motion.support&&a.motion.support==b.motion.support&&a.motion.objectSupport==b.motion.objectSupport){
            s.motion.offset={a.motion.offset.x+(b.motion.offset.x-a.motion.offset.x)*t,
                a.motion.offset.y+(b.motion.offset.y-a.motion.offset.y)*t,a.motion.offset.z+(b.motion.offset.z-a.motion.offset.z)*t};
            s.motion.heading=LerpAngle(a.motion.heading,b.motion.heading,t);
        }
        s.rotation=LerpAngle(a.rotation,b.rotation,t);if((a.flags&b.flags&OnFootState::F_JETPACK)!=0)s.jetpack=JetpackState::Blend(a.jetpack,b.jetpack,t);return s;
    },[](OnFootState&s,float age,bool stale){s.pos=s.pos+s.vel*age;if(stale)s.vel={};});
    rp.renderedJetpack=rp.history.Size()?sampled.jetpack:st.jetpack;
    CVector target=rp.history.Size()?sampled.pos:st.pos;
    bool supported=false;
    CEntity*support=nullptr;
    if(st.motion.support&&st.motion.objectSupport)support=SharedMissionObjectById(st.motion.support);
    else if(st.motion.support){
        if(auto*nv=Vehicles::Instance().Find(st.motion.support);nv&&Vehicles::Alive(*nv))support=nv->veh;
        else support=ResolveTrainSupport(st.motion.support);
    }
    if(support&&support->Matrix()&&(Field<int8_t>(support,0x2F)==st.area||Field<int8_t>(support,0x2F)==13)){
        auto&motion=rp.history.Size()?sampled.motion:st.motion;
        target=TransformPoint(*support->Matrix(),{motion.offset.x,motion.offset.y,motion.offset.z});supported=true;
        if(st.motion.kind!=MotionState::MOUNTED){SetSupportReference(Field<CEntity*>(ped,0x568),support);SetSupportReference(Field<CEntity*>(ped,0x584),support);}
        ped->CurrentRotation()=ped->AimingRotation()=support->GetHeading()+motion.heading;
    }
    CVector cur = ped->GetPosition();
    CVector err = target - cur;
    float dist2d = err.Length2D();
    // Where this game has no ground under it (one of you indoors: the other's map is unloaded; or far away), its
    // physics would drop it through the map, and the pull would hold it there, under the teleport distance.
    CPlayerPed* me = FindPlayerPed(0);
    bool found = false;
    float ground = Call<0x5696C0, float>(target.x, target.y, target.z + 1.0f, &found, (CEntity**)nullptr);  // FindGroundZFor3DCoord
    bool groundless = st.area != me->AreaCode() || !found || (target - me->GetPosition()).Length2D() > 150.0f;
    bool sunk = found && cur.z < ground + 0.3f && target.z > ground + 0.5f;  // a standing ped is ~1 m above its ground
    bool special=(st.flags&OnFootState::F_JETPACK)||st.motion.kind==MotionState::SWIM||st.motion.kind==MotionState::CLIMB||st.motion.kind==MotionState::PARACHUTE;
    if(supported||special){
        SetEntityPosition(ped,target);ped->MoveSpeed()=(rp.history.Size()?sampled.vel:st.vel)*(1.0f/50.0f);
    } else if (dist2d > kTeleportDist || std::fabs(err.z) > kTeleportDist || sunk) {
        SetEntityPosition(ped, target);  // preserve the predicted location, not the older packet position
        ped->MoveSpeed() = st.vel * (1.0f / 50.0f);
    } else if (groundless) {
        SetEntityPosition(ped, target);
        ped->MoveSpeed() = CVector{};
    } else if (dist2d > 0.05f || std::fabs(err.z) > 1.0f) {
        float k = 1.0f - std::exp(-20.0f * TimeStep() / 50.0f);  // same convergence at different frame rates
        CVector corr{err.x * k, err.y * k, std::fabs(err.z) > 1.0f ? err.z * k : 0.0f};
        SetEntityPosition(ped, cur + corr);
        if (err.z > 1.0f && ped->MoveSpeed().z < 0.0f) ped->MoveSpeed().z = 0.0f;  // below its place: no falling on
    }
    // Network velocity prevents a different local timestep/input replay from drifting between packets.
    if (!groundless && !(st.flags & OnFootState::F_IN_AIR)&&!supported&&!special) {
        ped->MoveSpeed().x = sampled.vel.x / 50.0f;
        ped->MoveSpeed().y = sampled.vel.y / 50.0f;
    }
    if(supported)return;
    ped->AimingRotation() = st.aimRotation;
    if (st.pad.buttons & PadState::B_RS1)
        ped->CurrentRotation() = LerpAngle(ped->CurrentRotation(), st.rotation, std::min(1.0f, TimeStep() * 0.5f));
    // standing still: keep the facing in sync (movement/aim set it while active)
    bool moving = std::abs(st.pad.lx) > 10 || std::abs(st.pad.ly) > 10;
    if (!moving && !(st.pad.buttons & PadState::B_RS1)) {
        float r = LerpAngle(ped->CurrentRotation(), st.rotation, 0.2f);
        ped->CurrentRotation() = ped->AimingRotation() = r;
    }
}

// Trains are not networked (Trains.cpp shows the host's in the other games): the player riding one sits in the cab of
// the train here nearest to its place.
void Players::RideTrain(RemotePlayer& rp) {
    CVehicle* train=rp.state.motion.support?ResolveTrainSupport(rp.state.motion.support&~15u):Call<0x6F7090,CVehicle*>(rp.state.pos,true);  // CTrain::FindNearestTrain(position, an engine)
    CPlayerPed* ped = rp.ped;
    if (!train || (train->GetPosition() - rp.state.pos).Length() > 25.0f || (ped->InVehicle() && ped->Vehicle() == train) ||
        NowMs() - rp.enterTaskMs < 1500)
        return;
    rp.enterTaskMs = NowMs();
    CVector p = ped->GetPosition();
    if (ped->InVehicle()) {
        Cmd(0x0362).I(rp.pedHandle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        return;
    }
    if (CPed* d = train->Driver(); d && d->IsPlayer()) return;          // taken here
    else if (d) Cmd(0x009B).I(GetPedRef(d)).Run();                       // DELETE_CHAR: the copy's own driver
    Cmd(0x036A).I(rp.pedHandle).I(GetVehicleRef(train)).Run();          // WARP_CHAR_INTO_CAR
}

void Players::ApplyVehicleSeat(RemotePlayer& rp) {
    NetVehicle* nv = Vehicles::Instance().Find(rp.vehNetId);
    if (!nv || !Vehicles::Alive(*nv)) return;  // wait for a live vehicle
    CPlayerPed* ped = rp.ped;
    if (rp.seatTransition.HoldSeat(rp.vehNetId, NowMs())) return;
    int seat = PassengerSeatOf(nv->veh, ped);
    bool seated = ped->InVehicle() && ped->Vehicle() == nv->veh && seat == rp.seat;
    if (seated) return;
    // give the enter animation time; otherwise put the player in place directly
    if (NowMs() - rp.enterTaskMs < kEnterGraceMs && EnteringCar(ped)) return;
    if (ped->InVehicle()) {
        CVector p = ped->GetPosition();
        Cmd(0x0362).I(rp.pedHandle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    }
    // a player warped in as the driver of a car no player owned yet reports a car theft against FindPlayerPed, ours here
    // (CTaskSimpleCarSetPedInAsDriver, 0x64BBBA), and makes it a player's car: the local pad drove it until the next frame
    Field<uint8_t>(nv->veh, 0x42A) |= 2;  // CVehicle::bHasBeenOwnedByPlayer
    if (rp.seat < 0) {
        if (nv->veh->Driver() && nv->veh->Driver() != ped) return;  // seat taken locally
        Cmd(0x036A).I(rp.pedHandle).I(nv->handle).Run();  // WARP_CHAR_INTO_CAR
        if (nv->veh->Driver() == ped) nv->veh->SetStatus(3);  // STATUS_PHYSICS: its owner's state moves it
    } else {
        if (SeatOrAny(nv->veh, rp.seat) < 0 || nv->veh->Passengers()[rp.seat]) return;  // no such seat, or taken
        Cmd(0x0430).I(rp.pedHandle).I(nv->handle).I(rp.seat).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
    }
    rp.enterTaskMs = NowMs() - kEnterGraceMs + 1500;  // retry no more often than every 1.5 s
}

void Players::ApplySeatAssignment(uint16_t vehicle,uint8_t mask,const std::array<int8_t,3>&seats){
    auto*nv=Vehicles::Instance().Find(vehicle);if(!nv||!Vehicles::Alive(*nv))return;
    std::array<CPed*,3>peds{};
    for(uint8_t id=0;id<3;++id)if(mask&(1u<<id)){
        auto*rp=Get(id);auto*p=id==Net().MyId()?FindPlayerPed(0):rp?rp->ped:nullptr;
        if(!p)continue;
        // A delayed resolution must not pull a player back after it left.
        if(id==Net().MyId()&&(!p->InVehicle()||p->Vehicle()!=nv->veh))continue;
        peds[id]=p;
        if(p->InVehicle()&&p->Vehicle()==nv->veh&&PassengerSeatOf(nv->veh,p)!=seats[id]){
            auto at=nv->veh->GetPosition();Cmd(0x0362).I(GetPedRef(p)).F(at.x+2.f+id*.5f).F(at.y).F(at.z).Run();
        }
        if(rp){rp->seat=seats[id];rp->enterPending=false;rp->seatTransition={};
            rp->mode=seats[id]==-2?RemotePlayer::Mode::OnFoot:seats[id]<0?RemotePlayer::Mode::Driver:RemotePlayer::Mode::Passenger;}
    }
    for(uint8_t id=0;id<3;++id)if(auto*p=peds[id];p&&seats[id]>=0&&
        (!p->InVehicle()||p->Vehicle()!=nv->veh||PassengerSeatOf(nv->veh,p)!=seats[id])&&
        !nv->veh->Passengers()[seats[id]])Cmd(0x0430).I(GetPedRef(p)).I(nv->handle).I(seats[id]).Run();
    LOGI("Seat collision resolved: vehicle=%04X seats=%d,%d,%d mask=%u",vehicle,seats[0],seats[1],seats[2],mask);
}
void Players::ResolvePassengerConflicts(){
    auto now=NowMs();if(now-m_seatCheck<250)return;m_seatCheck=now;
    auto*me=FindPlayerPed(0);
    for(auto&[vehicle,nv]:Vehicles::Instance().All())if(nv.owner==Net().MyId()&&Vehicles::Alive(nv)){
        std::array<int8_t,3>wanted{-2,-2,-2};uint8_t mask=0,driver=255;
        if(me->InVehicle()&&me->Vehicle()==nv.veh){wanted[Net().MyId()]=(int8_t)PassengerSeatOf(nv.veh,me);mask|=1u<<Net().MyId();}
        for(auto&[id,rp]:m_remote)if(id<3&&rp.hasState&&now-rp.stateRecvMs<1500&&rp.vehNetId==vehicle&&rp.mode!=RemotePlayer::Mode::OnFoot){wanted[id]=rp.seat;mask|=1u<<id;}
        auto*d=nv.veh->Driver();if(d==me)driver=Net().MyId();else if(auto*rp=FindByPed(d))driver=rp->id;
        // Driver ownership is resolved by VEH_OWNER first. A driver snapshot
        // can overtake that reliable claim; never turn it into a passenger.
        if(driver!=Net().MyId())continue;
        bool changingDriver=false;for(uint8_t id=0;id<3;++id)if(id!=driver&&wanted[id]==-1)changingDriver=true;
        if(changingDriver)continue;
        auto resolved=ResolveSeatClaims(wanted,Net().HostId(),driver,nv.veh->MaxPassengers(),CooperativeReservedSeats(nv.veh));
        if(resolved==wanted)continue;
        Writer w(proto::G_SEAT_ASSIGN);w.Entity(ENTITY_VEHICLE,vehicle);w.U32(++m_seatSerial);w.U8(mask);for(auto s:resolved)w.I8(s);
        Net().Send(proto::CH_RELIABLE,w);ApplySeatAssignment(vehicle,mask,resolved);
    }
}

void Players::TryEnterAsPassenger() {
    if (!core::IsPlayerReady()) return;
    CPlayerPed* me = FindPlayerPed(0);
    if (me->InVehicle() || EnteringCar(me)) return;
    CPoolHeader* pool = VehiclePool();
    if (!pool) return;
    CVector mp = me->GetPosition();
    CVehicle* best = nullptr;
    int bestSeat = -1;
    float bestScore = 1e9f;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        float d = (v->GetPosition() - mp).Length();
        if (d > 10.0f || v->Status() == 5 /* wrecked */ || v->IsTrain() || TrainingVehicleRestricted(v)) continue;  // a train's copy is locked (Trains)
        int freeSeat = -1;
        int seats = std::clamp((int)v->MaxPassengers(), 0, 8);
        const RemotePlayer* driver = FindByPed(v->Driver());
        int preferred = seats ? proto::PartnerIndex(Net().MyId(), driver ? driver->id : proto::kServerId) % seats : 0;
        const NetVehicle* nv = Vehicles::Instance().Find(v);
        for (int offset = 0; offset < seats; ++offset) {
            int s = (preferred + offset) % seats;
            bool reserved = (CooperativeReservedSeats(v)&(1u<<s))!=0;
            if (nv) for (auto& [id, rp] : m_remote)
                if (rp.enteringVeh == nv->netId && rp.enteringSeat == s && NowMs() - rp.enterTaskMs < 10000 &&
                    rp.ped && EnteringCar(rp.ped)) reserved = true;
            if (!reserved && !v->Passengers()[s]) {
                freeSeat = s;
                break;
            }
        }
        if (freeSeat < 0) continue;
        bool partnerDrives = v->Driver() && FindByPed(v->Driver());
        float score = d - (partnerDrives ? 100.0f : 0.0f);
        if (score < bestScore) {
            bestScore = score;
            best = v;
            bestSeat = freeSeat;
        }
    }
    if (!best) return;
    Cmd(0x05CA).I(GetPedRef(me)).I(GetVehicleRef(best)).I(10000).I(bestSeat).Run();  // TASK_ENTER_CAR_AS_PASSENGER
}

void Players::DetectCarTasks() {
    CPlayerPed* me = FindPlayerPed(0);
    void* task = nullptr;
    int type = ActivePrimaryTaskType(me, &task);
    for (int candidate : {TASK_ENTER_CAR_AS_DRIVER, TASK_ENTER_CAR_AS_PASSENGER,
                           TASK_ENTER_CAR_AS_DRIVER_TIMED, TASK_ENTER_CAR_AS_PASSENGER_TIMED})
        if (void* nested = anim::FindTask(me, candidate)) { type = candidate; task = nested; break; }
    bool passenger = type == TASK_ENTER_CAR_AS_PASSENGER || type == TASK_ENTER_CAR_AS_PASSENGER_TIMED;
    if ((type == TASK_ENTER_CAR_AS_DRIVER || type == TASK_ENTER_CAR_AS_DRIVER_TIMED || passenger) && task) {
        auto* target = Field<CVehicle*>(task, 0x0C);  // CTaskComplexEnterCar::m_Car, ...AsPassengerTimed::m_TargetCar
        if (target && target != m_enterReported && IsVehicleValid(target)) {
            m_enterReported = target;
            if (NetVehicle* nv = Vehicles::Instance().EnsureNetworked(target)) {
                int seat = -1;
                if (passenger && target->VehicleSubClass()==VEHICLE_TYPE_BIKE) seat=0;
                else if (type == TASK_ENTER_CAR_AS_PASSENGER_TIMED)  // the door the command chose for its seat (0: any)
                    seat = SeatOfTargetDoor(Field<int32_t>(task, 0x14));
                else if (passenger)  // m_TargetDoor once at the door, else m_TargetSeat (a door too)
                    seat = SeatOfTargetDoor(Field<int32_t>(task, 0x14) ? Field<int32_t>(task, 0x14) : Field<int32_t>(task, 0x1C));
                Writer w(proto::G_PLAYER_ENTER_CAR);
                w.Entity(ENTITY_VEHICLE,nv->netId);
                w.I8((int8_t)seat);
                w.U32((uint32_t)NowMs());
                Net().Send(proto::CH_RELIABLE, w);
                if (auto* victim = !passenger && target->Driver() ? FindByPed(target->Driver()) : nullptr) m_jackVictim = victim->id;
                else m_jackVictim = 0xFF;
                m_jackReported = false;
            }
        }
    } else {
        m_enterReported = nullptr;
        m_jackReported = false;
    }
    if (passenger) m_jackVictim = 0xFF;
    bool bikeJack = m_enterReported && IsVehicleValid(m_enterReported) && m_enterReported->VehicleSubClass() == VEHICLE_TYPE_BIKE &&
        m_enterReported->DoorLock() <= 1 && m_enterReported->MoveSpeed().Length() < 0.04f &&
        (me->GetPosition() - m_enterReported->GetPosition()).Length2D() < 2.0f && anim::FindTask(me, 807);
    if (!passenger && !m_jackReported && m_jackVictim != 0xFF && m_enterReported &&
        (anim::FindTask(me, 820) || anim::FindTask(me, 817) || bikeJack)) {
        if (auto* nv = Vehicles::Instance().Find(m_enterReported)) {
            int door = task ? Field<int32_t>(task, 0x14) : 8;
            if (door < 8 || door > 11) door = 8;
            Writer jack(proto::G_PLAYER_CARJACK);
            jack.Entity(ENTITY_VEHICLE,nv->netId); jack.U8(m_jackVictim); jack.U8((uint8_t)door); jack.U32((uint32_t)NowMs());
            Vehicles::Instance().ExpectCarjack(nv->netId,Net().MyId());
            Net().Send(proto::CH_RELIABLE, jack);
            m_jackReported = true;
        }
    }
    bool leaving = anim::FindTask(me, TASK_LEAVE_CAR) || anim::FindTask(me, 714) ||
                   anim::FindTask(me, 824) || anim::FindTask(me, 825) || anim::FindTask(me, 826);
    if (leaving && me->InVehicle()) {
        if (!m_exitReported) {
            m_exitReported = true;
            if (NetVehicle* nv = Vehicles::Instance().Find(me->Vehicle())) {
                Writer w(proto::G_PLAYER_EXIT_CAR);
                w.Entity(ENTITY_VEHICLE,nv->netId);
                w.U32((uint32_t)NowMs());
                Net().Send(proto::CH_RELIABLE, w);
            }
        }
    } else if (!me->InVehicle()) {
        if (m_exitReported) m_lastSend = 0;
        m_exitReported = false;
    }
}

void Players::SendVehicleState(CVehicle* v) {
    CPlayerPed* me = FindPlayerPed(0);
    NetVehicle* nv = Vehicles::Instance().EnsureNetworked(v);
    if (!nv) return;
    OnFootState common;
    FillCommon(common, me);
    uint32_t previousEpoch=m_actionEpoch;
    CaptureMotion(common,me,((uint32_t)nv->netId<<4)|(uint32_t)(PassengerSeatOf(v,me)+2));
    bool transition=previousEpoch!=m_actionEpoch;
    uint64_t now = NowMs();
    bool padChanged = common.pad.buttons != m_lastSentPad.buttons || (!(common.pad == m_lastSentPad) && now - m_lastSend >= 33);
    bool driver = v->Driver() == me;
    uint64_t interval = driver ? 33 : 100;
    if (!transition&&!padChanged && now - m_lastSend < interval) return;
    m_lastSend = now;
    m_lastSentPad = common.pad;
    if (driver) {
        Vehicles::Instance().TakeOwnership(*nv);
        VehicleState vs = Vehicles::Instance().Capture(v);
        vs.time = (uint32_t)now;
        Writer w(proto::G_PLAYER_DRIVER);
        w.Entity(ENTITY_VEHICLE,nv->netId,ENTITY_SNAPSHOT);
        vs.Write(w);
        WriteCommon(w, common);
        Net().Send(proto::CH_STATE, w);
        if(transition)Net().Send(proto::CH_RELIABLE,w);
    } else {
        int seat = PassengerSeatOf(v, me);
        if(seat<0||seat>=v->MaxPassengers())return; // native exit has already released its slot
        Writer w(proto::G_PLAYER_PASSENGER);
        w.Entity(ENTITY_VEHICLE,nv->netId);
        w.I8((int8_t)std::max(seat, 0));
        w.U32((uint32_t)now);
        CVector p = v->GetPosition();
        w.Vec({p.x, p.y, p.z});
        WriteCommon(w, common);
        Net().Send(proto::CH_STATE, w);
        if(transition)Net().Send(proto::CH_RELIABLE,w);
    }
}

void Players::OnPreProcess() {
    if (!Net().Online() || !core::IsPlayerReady()) return;
    KeepMeleeBlocks();
    for (auto& [id, rp] : m_remote) {
        if ((rp.ped && !IsPedValid(rp.ped)) || (!rp.ped && rp.pedHandle >= 0)) {  // the engine deleted it
            LOGW("Remote player %d ped vanished, respawning", id);
            rp.ped = nullptr;
            rp.pedHandle = -1;
            rp.blip = 0;
        }
        // states older than 5 s mean the player is loading / in the menu; a cutscene here keeps its ped in the scene
        // (its game loads the same cutscene and can be silent that long)
        bool active = rp.hasState && (NowMs() - rp.stateRecvMs < 5000 || (rp.ped && Ref<uint8_t>(0xB5F851)));
        if (!active) {
            if (rp.ped) DestroyRemote(rp);
            continue;
        }
        if (!rp.ped) SpawnRemote(rp);
        if (rp.ped) StartPendingEnter(rp);
        if (rp.ped) ApplyState(rp);
        if (rp.ped) KeepApart(rp.ped);
    }
}

// A script that puts this game's player on a spot (a changing room, a mission's) puts it inside the other player's
// ped when that one stands there: the two peds got stuck in each other and the player's next scripted task never
// started (the shop waits for it with the controls taken). Two players inside each other ignore each other's collision
// until they part (CPhysical::m_pEntityIgnoredCollision, the game clears it once they stop touching).
void Players::KeepApart(CPed* other) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || me->InVehicle() || other->InVehicle()) return;
    CVector d = other->GetPosition() - me->GetPosition();
    if (d.x * d.x + d.y * d.y > 0.4f * 0.4f || std::fabs(d.z) > 1.5f) return;
    Field<CEntity*>(me, 0x128) = other;
    Field<CEntity*>(other, 0x128) = me;
}

namespace {
bool EnteringVehicle(CPed*ped){
    for(int task:{700,701,702,703,704,712,713,715,717,718,800})if(anim::FindTask(ped,task))return true;
    return false;
}
CVehicle* SupportingVehicle(CPed* ped) {
    if(!ped||ped->InVehicle()||ped->Health()<=0||ped->IsInTheAir()||
       anim::FindTask(ped,210)||anim::FindTask(ped,211)||anim::FindTask(ped,254)||
       anim::FindTask(ped,1304)||HasJetpack(ped)||EnteringVehicle(ped))return nullptr;
    for(int offset:{0x568,0x584}) {
        auto*v=Field<CVehicle*>(ped,offset);
        if(!IsVehicleValid(v)||!v->Matrix()||v->AreaCode()!=ped->AreaCode())continue;
        if(VehicleSurfaceBelow(v,*v->Matrix(),ped->GetPosition()))return v;
    }
    return nullptr;
}
}

void Players::CaptureMotion(OnFootState& st,CPlayerPed* ped,uint32_t seatKey) {
    auto&m=st.motion;
    if(!ped->InVehicle()) {
        if(auto*task=anim::FindTask(ped,1304)) {
            m.kind=MotionState::SWIM;m.phase=(uint8_t)std::min<uint16_t>(5,Field<uint16_t>(task,0xA));
            m.pitch=Field<float>(task,0x28);m.roll=Field<float>(task,0x2C);st.flags|=OnFootState::F_SWIMMING;
        }else if(auto*task=anim::FindTask(ped,254)) {
            m.kind=MotionState::CLIMB;m.phase=Field<uint8_t>(task,0xD);
        }else if(ped->ActiveWeapon().type==46&&ped->IsInTheAir())m.kind=MotionState::PARACHUTE;
        else if(anim::FindTask(ped,205)||anim::FindTask(ped,206))m.kind=MotionState::GETUP;
        else if(anim::FindTask(ped,207)||anim::FindTask(ped,208)||anim::FindTask(ped,209))m.kind=MotionState::FALL;
        else if(anim::FindTask(ped,210)||anim::FindTask(ped,211))m.kind=MotionState::JUMP;
        if(auto*v=SupportingVehicle(ped)){
            if(v->IsTrain())m.support=TrainSupportOf(v);
            else if(auto*nv=Vehicles::Instance().EnsureNetworked(v))m.support=nv->netId;
            auto local=UntransformPoint(*v->Matrix(),ped->GetPosition());
            m.offset={local.x,local.y,local.z};m.heading=WrapAngle(ped->CurrentRotation()-v->GetHeading());
        }
    }
    if(!ped->InVehicle())if(auto*parent=ped->AttachedTo();parent&&parent->Matrix()){
        if(parent->IsObject()&&IsSharedMissionObject((CObject*)parent)){m.support=SharedMissionObjectId((CObject*)parent);m.objectSupport=true;}
        else if(parent->IsVehicle())if(auto*n=Vehicles::Instance().EnsureNetworked((CVehicle*)parent))m.support=n->netId;
        if(m.support){m.kind=MotionState::MOUNTED;auto local=UntransformPoint(*parent->Matrix(),ped->GetPosition());m.offset={local.x,local.y,local.z};m.heading=WrapAngle(ped->CurrentRotation()-parent->GetHeading());}
    }
    if(auto*v=ped->VehicleIfIn();v&&v->IsTrain())m.support=TrainSupportOf(v);
    uint32_t key=seatKey^((uint32_t)m.kind<<24)^((uint32_t)m.phase*0x9E3779B1u)^
        ((st.flags&OnFootState::F_DEAD)?0x80000000u:0)^((uint32_t)(uint8_t)st.area<<16)^((uint32_t)m.support*0x45D9F3Bu);
    if(key!=m_actionKey){m_actionKey=key;if(++m_actionEpoch==0)++m_actionEpoch;}
    m.epoch=m_actionEpoch;
}

void Players::CarryLocalOnVehicle(CVehicle* vehicle,const CMatrix& before) {
    auto*me=FindPlayerPed(0);
    if(!me||me->InVehicle()||me->Health()<=0||me->IsInTheAir()||!vehicle->Matrix()||Ref<uint8_t>(0xB5F851))return;
    if(Field<CEntity*>(me,0x568)!=vehicle&&Field<CEntity*>(me,0x584)!=vehicle)return;
    if(!VehicleSurfaceBelow(vehicle,before,me->GetPosition()))return;
    auto local=UntransformPoint(before,me->GetPosition());
    auto target=TransformPoint(*vehicle->Matrix(),local);
    if((target-me->GetPosition()).Length()<10) {
        SetEntityPosition(me,target);
    }
}

void Players::StabilizeLocalRider() {
    auto*me=FindPlayerPed(0);auto now=NowMs();auto*v=GetVehicle(m_rider);
    if(!me){m_rider=-1;return;}
    auto release=[&](bool leftSurface){
        // These references can have been installed by this stabilizer. Do not
        // let them serve as evidence of fresh contact after walking off a roof.
        if(v)for(int off:{0x568,0x584})if(Field<CEntity*>(me,off)==v)
            SetSupportReference(Field<CEntity*>(me,off),nullptr);
        m_rider=-1;
        if(leftSurface){m_riderReleaseUntil=now+250;me->PedFlags()&=~1u;}
    };
    bool jumping=GetPad(0)->NewState.ButtonSquare||anim::FindTask(me,210)||anim::FindTask(me,211);
    bool falling=anim::FindTask(me,205)||anim::FindTask(me,206)||anim::FindTask(me,207)||
        anim::FindTask(me,208)||anim::FindTask(me,209)||anim::FindTask(me,277);
    if(me->InVehicle()||me->Health()<=0||jumping||falling||EnteringVehicle(me)||anim::FindTask(me,254)||anim::FindTask(me,1304)||
       HasJetpack(me)||Ref<uint8_t>(0xB5F851)||GetPad(0)->DisablePlayerControls){release(false);return;}
    if(now<m_riderReleaseUntil)return;
    auto*contact=SupportingVehicle(me);
    if(v&&(!v->Matrix()||v->AreaCode()!=me->AreaCode()||v->Health()<=0))v=nullptr;
    // A grace timer cannot replace physical support. Validate both the actual
    // position and the proposed walking position before pinning either one.
    if(v&&!VehicleSurfaceBelow(v,*v->Matrix(),me->GetPosition())){release(true);return;}
    if(v&&contact==v){
        auto target=TransformPoint(*v->Matrix(),m_riderOffset);
        if((target-me->GetPosition()).Length()<3.5f){
            float dt=std::min(0.05f,TimeStep()/50.0f);
            auto&pad=GetPad(0)->NewState;auto nextOffset=m_riderOffset;
            float stick=std::min(1.0f,std::hypot((float)pad.LeftStickX,(float)pad.LeftStickY)/128);
            if(stick>.1f){
                float speed=(pad.ButtonCross?4.5f:pad.m_bPedWalk?1.3f:2.8f)*stick;
                CVector walk{-std::sin(me->CurrentRotation())*speed*dt,std::cos(me->CurrentRotation())*speed*dt,0};
                nextOffset.x+=Dot(walk,v->Matrix()->right);nextOffset.y+=Dot(walk,v->Matrix()->forward);
                target=TransformPoint(*v->Matrix(),nextOffset);
            }
            CVector surface;
            if(!VehicleSurfaceBelow(v,*v->Matrix(),target,&surface)){release(true);return;}
            target.z=surface.z+m_riderFootHeight;
            m_riderOffset=UntransformPoint(*v->Matrix(),target);
            SetEntityPosition(me,target);me->PedFlags()=(me->PedFlags()|1u)&~(1u<<9);
            // Gravity still runs in native physics. Do not accumulate a lethal
            // falling speed while the position is held on a valid roof/deck.
            auto r=target-v->GetPosition();auto w=v->TurnSpeed();
            me->MoveSpeed().z=v->MoveSpeed().z+w.x*r.y-w.y*r.x;
            SetSupportReference(Field<CEntity*>(me,0x568),v);SetSupportReference(Field<CEntity*>(me,0x584),v);
            float delta=WrapAngle(v->GetHeading()-m_riderHeading);
            me->CurrentRotation()=WrapAngle(me->CurrentRotation()+delta);me->AimingRotation()=WrapAngle(me->AimingRotation()+delta);
            m_riderHeading=v->GetHeading();return;
        }
    }
    release(false);
    if(contact&&contact->Matrix()){
        CVector surface;if(!VehicleSurfaceBelow(contact,*contact->Matrix(),me->GetPosition(),&surface))return;
        m_rider=GetVehicleRef(contact);m_riderOffset=UntransformPoint(*contact->Matrix(),me->GetPosition());
        m_riderHeading=contact->GetHeading();m_riderFootHeight=std::clamp(me->GetPosition().z-surface.z,0.8f,1.05f);
    }
}

void Players::SendLocalState() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return;
    OnFootState st;
    st.fightingStyle = me->FightingStyle();
    st.attackMoves = me->AllowedAttackMoves();
    st.time = (uint32_t)NowMs();
    st.pos = me->GetPosition();
    st.vel = me->MoveSpeed() * 50.0f;
    st.rotation = me->CurrentRotation();
    st.aimRotation = me->AimingRotation();
    st.health = ClampByte(me->Health());
    st.armour = ClampByte(me->Armour());
    CWeapon& w = me->ActiveWeapon();
    st.weapon = (uint8_t)w.type;
    st.ammo = (uint16_t)std::clamp(w.ammoTotal, 0, 65535);
    st.flags = (me->IsDucking() ? OnFootState::F_DUCKING : 0) | (me->IsInTheAir() ? OnFootState::F_IN_AIR : 0) |
               (HasJetpack(me) ? OnFootState::F_JETPACK : 0) |
               (me->Health() <= 0.0f ? OnFootState::F_DEAD : 0) |
               (me->InVehicle() && me->Vehicle() && me->Vehicle()->IsTrain() ? OnFootState::F_IN_TRAIN : 0) | (Field<int32_t>(me,0x740)==kCellphone ? OnFootState::F_PHONE : 0);
    st.area = me->AreaCode();
    st.moveState = (uint8_t)me->MoveState();
    uint32_t previousEpoch=m_actionEpoch;
    if((st.flags&OnFootState::F_JETPACK)&&me->Intelligence())st.jetpack=JetpackState::Capture(CallMethod<0x601110,void*>(me->Intelligence()));
    CaptureMotion(st,me);
    bool transition=previousEpoch!=m_actionEpoch;
    st.wanted = WantedLevel(me);
    st.policeFlags = PoliceFlags(me);
    CPad* pad = GetPad(0);
    if (pad->DisablePlayerControls == 0 && !game::MenuActive()) st.pad = PadState::From(pad->NewState);
    CameraState& c = st.cam;
    c.mode = cam::Mode();
    c.weaponMode = cam::WeaponMode();
    c.fov = cam::Fov();
    c.front = cam::Front();
    c.sourceOffset = cam::Source() - st.pos;
    c.aimOffset = CaptureAimOffset(me);
    c.up = cam::Up();
    c.orientation = cam::Orientation();
    c.alpha = cam::Alpha();
    c.beta = cam::Beta();
    c.lookPitch = me->PlayerData() ? me->LookPitch() : 0.0f;
    c.useMouse3rd = cam::UseMouse3rdPerson() != 0;
    c.chairX = cam::CrosshairMultX();
    c.chairY = cam::CrosshairMultY();

    uint64_t now = NowMs();
    bool padChanged = st.pad.buttons != m_lastSentPad.buttons;
    if (!transition&&!padChanged && now - m_lastSend < 33) return;  // ~30 Hz, immediately on input changes
    m_lastSend = now;
    m_lastSentPad = st.pad;
    Writer wr(proto::G_PLAYER_ONFOOT);
    st.Write(wr);
    Net().Send(proto::CH_STATE, wr);
    if(transition)Net().Send(proto::CH_RELIABLE,wr);
}

void Players::OnPostProcess() {
    if(Net().Online()&&core::IsPlayerReady())ResolvePassengerConflicts();
    if (!Net().Online() || !core::IsPlayerReady()) return;
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return;
    StabilizeLocalRider();
    DetectCarTasks();
    FollowHost();
    if (me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() == me) m_lastCar = GetVehicleRef(me->Vehicle());
    if (me->InVehicle() && me->Vehicle() && !me->Vehicle()->IsTrain()) SendVehicleState(me->Vehicle());
    else SendLocalState();  // a train is not networked: the other game seats this player in its copy (RideTrain)
}

// Where the story moves the host by script, the partner keeps up: joining after a new game's prologue (in a new game of
// its own) it comes to the host, and when the host gets on a bike in a mission or just after one, the partner gets a
// bike too (the prologue's ride home, the bikes of "Big Smoke").
void Players::FollowHost() {
    RemotePlayer* host = Get(Net().HostId());
    CPlayerPed* me = FindPlayerPed(0);
    if (Net().IsHost() || Ref<uint8_t>(0xB5F851) || !host || !host->ped || !host->hasState || me->InVehicle() ||
        NowMs() - m_followAt < 1000)  // CCutsceneMgr::ms_running: the scene holds the partner meanwhile
        return;
    CPed* hp = host->ped;
    CVector hpos = hp->GetPosition(), mpos = me->GetPosition();
    if (flow::IsFollower() && (hpos - mpos).Length() > 25.0f) {
        float h = hp->CurrentRotation();
        float side = proto::PartnerIndex(Net().MyId(), Net().HostId()) ? -1.5f : 1.5f;
        CVector p = hpos + CVector{std::cos(h) * side, std::sin(h) * side, 0};
        int handle = GetPedRef(me);
        Cmd(0x04BB).I(hp->AreaCode()).Run();                      // SET_AREA_VISIBLE
        Cmd(0x0860).I(handle).I(hp->AreaCode()).Run();            // SET_CHAR_AREA_VISIBLE
        Cmd(0x00A1).I(handle).F(p.x).F(p.y).F(p.z - 1.0f).Run();  // SET_CHAR_COORDINATES
        Cmd(0x0173).I(handle).F(h * 57.29578f).Run();             // SET_CHAR_HEADING
        LightArea(hp->AreaCode(), p);
        m_followAt = NowMs();
        LOGI("Joined after the prologue: next to the host");
        return;
    }
    CVehicle* bike = hp->InVehicle() ? hp->Vehicle() : nullptr;
    int model = bike ? bike->ModelIndex() : 0;
    if (!bike || bike == m_bikeFor || bike->Driver() != hp || (model != 481 && model != 509 && model != 510)) return;  // BMX, bikes
    if ((!flow::IsFollower() && !HostMissionRecent(20000)) || (hpos - mpos).Length() > 60.0f) return;
    m_bikeFor = bike;
    m_followAt = NowMs();
    if (!IsModelLoaded(model)) {
        RequestModel(model);
        LoadAllRequestedModels(false);
    }
    float h = me->CurrentRotation();
    CVector p = mpos + CVector{std::cos(h) * 1.3f, std::sin(h) * 1.3f, -0.5f};  // at the partner's right hand
    int car = -1;
    if (!HasRoom(VehiclePool(), kKeepVehicles, "vehicle")) return;
    Cmd(0x00A5).I(model).F(p.x).F(p.y).F(p.z).Out(&car).Run();  // CREATE_CAR
    if (car < 0) return;
    Cmd(0x0175).I(car).F(h * 57.29578f).Run();  // SET_CAR_HEADING
    Cmd(0x01C3).I(car).Run();                   // MARK_CAR_AS_NO_LONGER_NEEDED: an ordinary vehicle from now on
    ui::TheChat().AddSystem(TR("Рядом с вами велосипед. Садитесь и езжайте за хостом.",
                               "There is a bike next to you. Get on and follow the host."));
    LOGI("The host got on a bike (model %d): one for the partner too", model);
}

// the point over rp's head the name tag hangs from (back buffer coordinates) and its fade with distance
bool Players::TagAnchor(RemotePlayer& rp, float& sx, float& sy, float& alpha) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!rp.ped || !me || rp.ped->AreaCode() != me->AreaCode()) return false;
    CVector head = rp.ped->GetPosition() + CVector{0, 0, 0.95f};
    float dist = (head - cam::Source()).Length();
    if (dist > 70.0f) return false;
    float depth;
    if (!WorldToScreen(head, sx, sy, depth)) return false;
    auto& g = ui::Gfx();
    // game screen space -> back buffer
    sx *= g.Width() / (float)std::max(1, ScreenWidth());
    sy *= g.Height() / (float)std::max(1, ScreenHeight());
    alpha = dist > 50.0f ? 1.0f - (dist - 50.0f) / 20.0f : 1.0f;
    return true;
}

void Players::DrawWeaponIcons() {
    auto& g = ui::Gfx();
    float kx = ScreenWidth() / std::max(1.0f, g.Width()), ky = ScreenHeight() / std::max(1.0f, g.Height());
    for (auto& [id, rp] : m_remote) {
        float sx, sy, alpha;
        if (!rp.hasState || !TagAnchor(rp, sx, sy, alpha)) continue;
        // right of the health bar (64 x 6 at sy - 6, see DrawNameTag), as tall as the bars
        float s = g.Scale(), h = 26 * s, w = h * 47.0f / 58.0f;
        float x = sx + 32 * s + 4 * s, y = sy - 3 * s - h / 2;
        DrawWeaponIcon(rp.state.weapon, x * kx, y * ky, w * kx, h * ky, (uint8_t)(alpha * 255));
        DrawWantedStars(rp.state.wanted, (sx - 32 * s - 4 * s) * kx, (sy - 3 * s) * ky, (uint8_t)(alpha * 255));
    }
}

void Players::DrawNameTag(RemotePlayer& rp) {
    float sx, sy, alpha;
    if (!TagAnchor(rp, sx, sy, alpha)) return;
    auto& g = ui::Gfx();
    float s = g.Scale();
    std::string name = Net().PeerName(rp.id);
    if (NowMs() - rp.heardMs < 300) name = "{FFDC50}((•)){FFFFFF} " + name;  // talking right now
    bool host = rp.id == Net().HostId();
    uint32_t nameColor = host ? 0xFF64B4FF : 0xFF78DC78;
    float lh = g.LineHeight(ui::FONT_SMALL);
    g.Text(ui::FONT_SMALL, sx, sy - lh - 8 * s, name, ui::WithAlpha(nameColor, alpha), ui::TEXT_CENTER | ui::TEXT_OUTLINE);
    float bw = 64 * s, bh = 6 * s;
    float bx = sx - bw / 2, by = sy - 6 * s;
    float hp = std::clamp(rp.ped->Health() / 100.0f, 0.0f, 1.0f);
    g.Rect(bx - 1, by - 1, bw + 2, bh + 2, ui::WithAlpha(0xFF000000, alpha));
    g.Rect(bx, by, bw, bh, ui::WithAlpha(0xFF501414, alpha));
    g.Rect(bx, by, bw * hp, bh, ui::WithAlpha(0xFFDC3232, alpha));
    if (rp.ped->Armour() > 0) {
        float ar = std::clamp(rp.ped->Armour() / 100.0f, 0.0f, 1.0f);
        by -= bh + 3 * s;
        g.Rect(bx - 1, by - 1, bw + 2, bh + 2, ui::WithAlpha(0xFF000000, alpha));
        g.Rect(bx, by, bw, bh, ui::WithAlpha(0xFF3C3C3C, alpha));
        g.Rect(bx, by, bw * ar, bh, ui::WithAlpha(0xFFE6E6E6, alpha));
    }
}

void Players::OnRender() {
    if (!Net().Online()) return;
    for (auto& [id, rp] : m_remote) DrawNameTag(rp);
}

bool Players::OnCommand(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd != "tp" && cmd != "tphere") return false;
    if (!core::IsPlayerReady()) return true;
    RemotePlayer* target = nullptr;
    float nearest = 1e9f;
    for (auto& [id, rp] : m_remote) {
        if (!rp.hasState || NowMs() - rp.stateRecvMs >= 5000) continue;
        if (!args.empty()) {
            if (ToLower(Net().PeerName(id)) == ToLower(args[0]) || std::to_string(id) == args[0]) target = &rp;
        } else {
            // Partners default to the host; the host defaults to the nearest active partner.
            float d = (rp.state.pos - FindPlayerPed(0)->GetPosition()).Length();
            if (d < nearest) target = &rp, nearest = d;
            if (!Net().IsHost() && id == Net().HostId()) { target = &rp; break; }
        }
    }
    if (!target) {
        ui::TheChat().AddError(TR("Игрок не найден или ещё не в игре. Используйте /tp <ник или номер>.",
                                  "Player not found or not in the game yet. Use /tp <nickname or id>."));
        return true;
    }
    CPlayerPed* me = FindPlayerPed(0);
    if (cmd == "tp") {
        CVector p = target->state.pos + CVector{1.0f, 0, 0};
        int handle = GetPedRef(me);
        Cmd(0x04BB).I(target->state.area).Run();
        Cmd(0x0860).I(handle).I(target->state.area).Run();
        Cmd(0x00A1).I(handle).F(p.x).F(p.y).F(p.z - 1.0f).Run();
        LightArea(target->state.area, p);
        ui::TheChat().AddSystem(Format(TR("Вы переместились к %s.", "You went over to %s."), Net().PeerName(target->id).c_str()));
    } else {
        CVector p = me->GetPosition() + CVector{1.0f, 0, 0};
        Writer w(proto::G_TELEPORT);
        w.U8(target->id);
        w.F32(p.x);
        w.F32(p.y);
        w.F32(p.z - 1.0f);
        w.I8(me->AreaCode());
        Net().Send(proto::CH_RELIABLE, w);
        ui::TheChat().AddSystem(Format(TR("%s перемещён к вам.", "%s was brought over to you."), Net().PeerName(target->id).c_str()));
    }
    return true;
}

}  // namespace coop

namespace coop {void Players::OnEntityReplaced(uint8_t kind,uint8_t scope,uint16_t id){
 if(kind==ENTITY_OBJECT){for(auto&[player,rp]:m_remote)if(rp.state.motion.objectSupport&&rp.state.motion.support==id){rp.state.motion={};rp.history.Clear();}return;}
 if(kind==ENTITY_TRAIN){for(auto&[player,rp]:m_remote){auto support=rp.state.motion.support;if((support&0x8000u)&&((support>>4)&255u)==id&&((support>>12)&7u)==scope+1u){ClearChangedSupport(rp.ped,support,0);rp.state.motion.support=0;rp.history.Clear();}}return;}
 if(kind!=ENTITY_VEHICLE)return;for(auto&[player,rp]:m_remote){
  if(rp.vehNetId==id){rp.vehNetId=0;rp.mode=RemotePlayer::Mode::OnFoot;rp.seatTransition={};rp.history.Clear();}
  if(!rp.state.motion.objectSupport&&rp.state.motion.support==id){ClearChangedSupport(rp.ped,id,0);rp.state.motion.support=0;rp.history.Clear();}
 }
}}
