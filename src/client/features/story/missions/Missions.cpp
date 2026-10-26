// Story missions run only in the host's game; the partner sees what the mission shows.
//
//  * partner: mission triggers of main.scm are refused (CPlayerPed::CanPlayerStartMission returns false
//    while a main.scm thread asks). Shops, food, barbers... ask from streamed scripts and keep working;
//  * host: a trigger is refused too until the partner stands in the same marker (G_MISSION_WAIT shows it);
//  * host -> partner while a mission (or its trigger) runs:
//      texts       - CMessages small/big messages and help boxes (G_TEXT);
//      radar blips - blips created after the mission started, on the same vehicles/peds or coordinates (G_BLIP);
//      markers     - "locate" destination cylinders (CTheScripts::HighlightImportantArea, G_MARKER), race
//                    checkpoints and script spheres (G_CHECKPOINTS);
//      weapons     - what the mission gives its CJ (a spray can for the tags) and takes back (G_MISSION_WEAPON);
//      cutscenes   - the partner loads and plays the same cutscene (G_CUTSCENE);
//      screen      - fades and widescreen borders (G_FADE), onscreen timers/counters (G_ONSCREEN).
// Mission peds and vehicles are ordinary host entities (Peds/Vehicles).
#include <algorithm>
#include "shared/api/story-world/StoryWorld.h"
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "features/story/missions/Missions.h"
#include "shared/api/story-actions/StoryActions.h"
#include "shared/api/story-scripts/StoryScripts.h"
#include "shared/api/story-loadout/StoryLoadout.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/story-rc/StoryRC.h"
#include "shared/api/story-rc-flight/StoryRCFlight.h"
#include "shared/api/airport-gates/AirportGates.h"
#include "shared/api/story-stage/StoryStage.h"
#include "shared/lib/sync/StoryPosition.h"
#include "shared/lib/sync/StoryContinuation.h"
#include "shared/api/native-interaction/NativeInteraction.h"
#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/profile/Profile.h"
#include "shared/lib/sync/SyncTrace.h"
#include "shared/lib/sync/ScriptValues.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/audio/AudioTarget.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "entities/vehicle/Vehicles.h"
#include "features/story/cutscene-partner/ScenePartner.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Anim.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/sync/SceneBarrier.h"
#include "shared/lib/sync/DialogueQueue.h"
#include "shared/lib/sync/GroupReadiness.h"
#include "shared/lib/sync/SchoolPolicy.h"
#include "shared/api/progress/SchoolProgress.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop {
void AlignSharedSoundtrack(uint32_t milliseconds,bool hard);

namespace {
using namespace game;
using game::script::Cmd;

// ---------------------------------------------------------------- engine
constexpr uintptr_t kActiveScripts = 0xA8B42C;  // CTheScripts::pActiveScripts
constexpr uintptr_t kMainScriptSize = 0xA444C4;
constexpr uintptr_t kRadarTraces = 0xBA86F0;    // CRadar::ms_RadarTrace[175], 0x28 bytes
constexpr int kNumTraces = 175;  // vec2DRadarOrigin follows at 0xBAA248, then the HUD
constexpr uintptr_t kOnscreenTimer = 0xBA1788;  // CUserDisplay::OnscnTimer (COnscreenTimer, 0x154 bytes)
constexpr size_t kOnscreenSize = 0x154;
constexpr uintptr_t kCutsceneRunning = 0xB5F851, kCutsceneLoadStatus = 0xB5F84C, kCutsceneOffset = 0xBC4034;
constexpr int BLIP_CAR = 1, BLIP_CHAR = 2, BLIP_OBJECT = 3, BLIP_COORD = 4;

bool CutsceneRunning() { return Ref<uint8_t>(kCutsceneRunning) != 0; }
uintptr_t Trace(int i) { return kRadarTraces + (uintptr_t)i * 0x28; }

void* FindMissionScript() {
    for (void* s = Ref<void*>(kActiveScripts); s; s = Field<void*>(s, 0x00))
        if (Field<uint8_t>(s, 0xDC)) return s;  // m_bIsMission
    return nullptr;
}

// a property purchase (BUYPRO1) is no mission for the partner: it only shows the house, Property.cpp shares the purchase
// The save points' thread (PSAVE1) raises the mission flag over its save menu: no mission either (the partner's screen
// went dark with the host's save and "the host started a mission" came up).
void* g_currentScript = nullptr;
bool g_personalFlag = false;
bool g_schoolFlag = false;
bool g_sceneBypassed=false;
bool g_saveFlag = false;  // the flag up is PSAVE1's
int g_storySubtitlePreference=-1;
uint64_t g_localActivityUntil=0;void*g_localActivityScript=nullptr;
void StopScriptCamera();
bool SharedSceneOwnsScreen();
bool PersonalActivity(void* script) {
    if (!script) return false;
    const char* name = Field<char[8]>(script, 0x08);
    // A newly allocated thread has not executed SCRIPT_NAME yet.
    if (!_strnicmp(name, "noname", 7)) {
        const auto* ip = Field<const uint8_t*>(script, 0x14);
        if (ip && Field<uint16_t>(ip, 0) == 0x03A4 && ip[2] == 9) name = (const char*)ip + 3;
    }
    static const char* const own[] = {"none", "dual", "shtr", "grav", "otb", "pool2", "lowr", "zero5", "truck", "boat", "blood", "tria", "bcour", "stunt", "oddveh", "taxiodd", "ambulan", "firetru", "copcar", "pimp", "freight", "trainsl",
        "trace", "hotr", "kicks", "kicksta", "bloodr", "dskool", "bskool", "bschoo", "psch", "desert5", "bikes",
        "gym", "gymls", "gymsf", "gymlv", "shoot", "shrange", "trucks", "quarry", "quarrys", "burgjb", "valet_l", "mtbiker", "cprace", "bbchal", "barb", "tatto", "blackj", "roulete", "bandit", "vidpok", "wheelo"};
    static const char*const prefixes[]={"gym","shop","food","pizza","chick","burger","barber","tattoo","wardrob","warrobe","psave","buypro","intman","entext","barguy","barstaf","amunat","browse","car_mod","carmod","vending","fodvend","hotdog","gfdate","gfmeet"};
    for(auto prefix:prefixes)if(!_strnicmp(name,prefix,strlen(prefix)))return true;
    for (const char* activity : own) if (!_strnicmp(name, activity, 8)) return true;
    return false;
}

// Main SCM helpers do not have m_bIsMission set. Ownership comes from the
// native thread that spawned them, and lasts only for that cooperative run.
struct StoryThread {
    uint64_t run = 0, birth = 0, parentBirth = 0;
    void* parent = nullptr;
    const uint8_t* base = nullptr;
    bool bound = false;
};
std::map<void*, StoryThread> g_storyThreads;
uint64_t g_storyThreadBirth = 0;

void ForgetStoryThread(void* script) {
    if (!g_storyThreads.erase(script)) return;
    CancelCooperativeStage(script);
    ClearCooperativeStoryScript(script);
}
void ClearStoryThreads() {
    while (!g_storyThreads.empty()) ForgetStoryThread(g_storyThreads.begin()->first);
}
void PruneStoryThreads() {
    const auto run = StoryRunId();
    for (auto i = g_storyThreads.begin(); i != g_storyThreads.end();) {
        auto* script = i->first;
        const auto& entry = i->second;
        bool stale = !run || entry.run != run || PersonalActivity(script) ||
                     !Field<uint8_t>(script, 0xC4) ||
                     (entry.bound && entry.base != Field<const uint8_t*>(script, 0x10));
        ++i;
        if (stale) ForgetStoryThread(script);
    }
}
bool SharedStoryScript(void* script) {
    if (!script || !Net().Online() || !Net().IsHost() || PersonalActivity(script)) return false;
    auto i = g_storyThreads.find(script);
    if (i == g_storyThreads.end() || !StoryRunId() || i->second.run != StoryRunId()) return false;
    if (!Field<uint8_t>(script, 0xC4) && script != g_currentScript) return false;
    const auto* base = Field<const uint8_t*>(script, 0x10);
    if (i->second.bound && i->second.base != base) return false;
    return true;
}
void BindStoryThread(void* script) {
    auto i = g_storyThreads.find(script);
    if (i != g_storyThreads.end() && !i->second.bound) {
        i->second.base = Field<const uint8_t*>(script, 0x10);
        i->second.bound = true;
    }
}
void TrackStoryRoot(void* script) {
    if (!StoryRunId()) return;
    auto i = g_storyThreads.find(script);
    if (i == g_storyThreads.end() || i->second.run != StoryRunId()) {
        g_storyThreads[script] = {StoryRunId(), ++g_storyThreadBirth, 0, nullptr,
                                 Field<const uint8_t*>(script, 0x10), true};
    }
}
using ScriptLifeFn = void(__fastcall*)(void*, void*);
ScriptLifeFn g_origScriptInit = nullptr, g_origScriptShutdown = nullptr;
void __fastcall HookScriptInit(void* script, void* edx) {
    // Init also runs for restored/indexed threads and for a reused pool slot.
    ForgetStoryThread(script);
    g_origScriptInit(script, edx);
}
void __fastcall HookScriptShutdown(void* script, void* edx) {
    g_origScriptShutdown(script, edx);
    ForgetStoryThread(script);
}

// Gym scripts use the same held action key to enter equipment and to cancel
// its selection menu after twenty frames. At high FPS (and while an enter key
// is still held) that delay elapses before the player can choose anything.
// Require release of the opening keys; later presses retain native behaviour.
struct GymSelection { void* script=nullptr; uint8_t held=0; };
GymSelection g_gymSelections[2];
bool GymEquipment(void*s){
    if(!s)return false;const char*n=Field<char[8]>(s,8);
    return !_strnicmp(n,"gymbenc",7)||!_strnicmp(n,"gymdumb",7)||
           !_strnicmp(n,"gymbike",7)||!_strnicmp(n,"gymtrea",7);
}
uint8_t GymHeldKeys(){
    auto&pad=GetPad(0)->NewState;
    return (pad.ButtonTriangle?1:0)|(pad.ButtonCross?2:0);
}
using CreateMenuFn=uint8_t(__cdecl*)(uint8_t,const char*,float,float,float,uint8_t,bool,bool,int);
using DeleteMenuFn=void(__cdecl*)(uint8_t);
CreateMenuFn g_origCreateMenu=nullptr;DeleteMenuFn g_origDeleteMenu=nullptr;
uint8_t __cdecl GymCreateMenu(uint8_t type,const char*title,float x,float y,float width,uint8_t cols,bool interactive,bool bg,int alignment){
    auto id=g_origCreateMenu(type,title,x,y,width,cols,interactive,bg,alignment);
    if(id<2){g_gymSelections[id]={};if(GymEquipment(g_currentScript)){
        g_gymSelections[id]={g_currentScript,GymHeldKeys()};
        LOGD("Gym selection opened (%.*s), menu %u, opening keys %u",8,Field<char[8]>(g_currentScript,8),id,g_gymSelections[id].held);
    }}
    return id;
}
void __cdecl GymDeleteMenu(uint8_t id){
    if(id<2)g_gymSelections[id]={};g_origDeleteMenu(id);
}
bool GymOpeningKey(void*script,int button){
    uint8_t key=button==15?1:button==16?2:0;if(!key)return false;
    bool blocked=false;
    for(auto&s:g_gymSelections)if(s.script==script){s.held&=GymHeldKeys();blocked|=(s.held&key)!=0;}
    return blocked;
}
bool HostOnMission() {
    void* s = FindMissionScript();
    if (PersonalActivity(s) || (s && !strncmp(Field<char[8]>(s, 0x08), "buypro1", 8))) return false;
    if (!core::IsOnMission()) g_saveFlag = false;
    if (!core::IsOnMission()) g_personalFlag = false;
    return s || (core::IsOnMission() && !g_saveFlag && !g_personalFlag);
}

uint64_t g_inheritedFlagMs = 0;
void HealInheritedFlag() {
    if (!g_inheritedFlagMs) return;
    if (!core::IsOnMission() || FindMissionScript()) {
        g_inheritedFlagMs = 0;  // its holder let it go, or a mission runs
    } else if (NowMs() - g_inheritedFlagMs > 60000) {
        Ref<int32_t>(addr::ScriptSpace + (uintptr_t)Ref<int32_t>(0xA476AC)) = 0;  // CTheScripts::OnAMissionFlag: $ONMISSION
        g_inheritedFlagMs = 0;
        LOGW("The loaded world kept the mission flag up with no script holding it: lowered");
    }
}

void Fade(float seconds, int16_t dir) { CallMethod<0x50AC20>((void*)addr::TheCamera, seconds, dir); }  // dir: 0 out, 1 in
float FadeAlpha() { return Ref<float>(addr::TheCamera + 0xBFC); }
bool Fading() { return Ref<uint8_t>(addr::TheCamera + 0x51) != 0; }
int16_t FadeDirection() { return Ref<int16_t>(addr::TheCamera + 0xC30); }
bool Darkening(){return FadeAlpha()>1.0f||(Fading()&&FadeDirection()==0);}
float FadeDuration() { return Ref<float>(addr::TheCamera + 0xC04); }
bool TheCameraWide() { return Ref<uint8_t>(addr::TheCamera + 0x3D) != 0; }
void SetWideScreen(bool on) {
    if (on) CallMethod<0x50C140>((void*)addr::TheCamera);
    else CallMethod<0x50C150>((void*)addr::TheCamera);
}

// ---------------------------------------------------------------- state
struct BlipRecord {  // what the host sends for one blip
    uint8_t index = 0;
    uint8_t type = 0, entKind = 0;  // entKind: 0 coordinates, 1 vehicle netId, 2 ped netId, 3 player id
    uint16_t entId = 0;
    CVector pos;
    uint32_t colour = 0;
    uint16_t size = 0;
    uint8_t sprite = 0, flags25 = 0, display = 0;

    void Write(Writer& w) const {
        w.U8(index);
        w.U8(type);
        w.U8(entKind);
        w.U16(entId);
        w.Vec({pos.x, pos.y, pos.z});
        w.U32(colour);
        w.U16(size);
        w.U8(sprite);
        w.U8(flags25);
        w.U8(display);
    }
    void Read(Reader& r) {
        index = r.U8();
        type = r.U8();
        entKind = r.U8();
        entId = r.U16();
        Vec3 p = r.Vec();
        pos = {p.x, p.y, p.z};
        colour = r.U32();
        size = r.U16();
        sprite = r.U8();
        flags25 = r.U8();
        display = r.U8();
    }
    bool operator==(const BlipRecord& o) const {
        // an entity's blip goes with the copy of the entity: its place counts only for a partner without that copy, and
        // every moving car or person with a blip went out again every 250 ms on the reliable channel
        return type == o.type && entKind == o.entKind && entId == o.entId && (pos - o.pos).Length() < (entKind ? 20.0f : 0.5f) &&
               colour == o.colour && size == o.size && sprite == o.sprite && flags25 == o.flags25 && display == o.display;
    }
};

enum : uint8_t { AREA_LOCATE = 0, AREA_SPHERE = 1 };
struct Area {
    uint32_t id;
    float x1, y1, x2, y2, z;  // a sphere: centre x1, y1, z and radius x2
    uint64_t seenMs;
    uint8_t kind = AREA_LOCATE;
};

enum : uint8_t { BLIP_SET = 1, BLIP_REMOVE = 2, BLIP_CLEAR = 3 };
enum : uint8_t { CS_LOAD = 1, CS_START = 2, CS_END = 3, CS_SKIP_VOTE = 4, CS_SKIP_STATE = 5, CS_SPOT = 6, CS_READY = 7, CS_CLOCK = 8 };
enum : uint8_t { TXT_MSG = 1, TXT_MSG_Q, TXT_BIG, TXT_BIG_Q, TXT_HELP, TXT_CLEAR, TXT_CLEAR_SMALL };
enum : uint8_t { FADE_ACTIVE = 1, FADE_WIDE = 2, FADE_BLACK = 4 };

// host
bool g_active = false;                        // a mission or its trigger runs
std::set<uint32_t> g_baseline;                // (counter << 8 | index) of blips that existed before
std::map<uint8_t, BlipRecord> g_sentBlips;
std::map<uint32_t, Area> g_areas;
std::vector<uint8_t> g_lastAreasMsg, g_lastOnscreen;
uint64_t g_lastAreasSent=0,g_lastMarkedSent=0;
uint64_t g_lastTick = 0, g_lastFadeMs = 0;
bool g_hostCutscene = false;
std::string g_missionName;
int64_t g_missionPayout=0;
char g_cutsceneName[32] = {};  // last loaded cutscene, for partners joining during it

// mission markers: the host's mission starts once the partner stands in the marker too
uint64_t g_waitMs = 0;      // host: the last time a trigger was refused for that
bool g_waitSent = false;
bool g_hostWaits = false;   // partner
Area g_waitArea{};

// partner
struct PartnerBlip {
    int handle = 0;
    uint8_t entKind = 0;
    uint16_t entId = 0;
};
std::map<uint8_t, PartnerBlip> g_blips;
std::vector<BlipRecord> g_contacts,g_contactHistory;
std::map<uint8_t,int> g_contactCopies;
uint64_t g_contactsSent=0,g_contactsApplied=0;
std::vector<Area> g_partnerAreas;
uint64_t g_partnerAreasMs = 0;
bool g_partnerMission = false, g_weFaded = false, g_weWide = false, g_onscreenSet = false;
uint64_t g_partnerMissionEndedAt = 0;
int g_csState = 0;  // 0 none, 1 loading, 2 running, 3 ending
int32_t g_csArea = 0;  // interior the host's cutscene plays in
bool g_csStartRequested = false;
bool g_sharedScene = false;
bool g_sceneScriptCamera = false;
bool g_scriptBlackResetRequested=false;
uint32_t g_screenGeneration=0,g_receivedScreenGeneration=0;
SceneBarrier g_sceneBarrier;
uint32_t g_sceneToken = 0;
bool g_sceneReadySent = false, g_startPending = false;
void* g_startScript = nullptr;
std::vector<std::vector<uint8_t>> g_pendingScenes;
uint8_t g_hudBefore = 1, g_radarBefore = 0;
bool g_savedHud = false;

void SaveSceneHud() {
    if (g_savedHud) return;
    g_hudBefore = Ref<uint8_t>(0xBAA45D);  // CHud::m_Wants_To_Draw_Hud
    g_radarBefore = Ref<uint8_t>(0xBAA3FB);  // CHud::bScriptDontDisplayRadar
    g_savedHud = true;
}
void RestoreSceneHud() {
    if (!g_savedHud) return;
    Ref<uint8_t>(0xBAA45D) = g_hudBefore;
    Ref<uint8_t>(0xBAA3FB) = g_radarBefore;
    g_savedHud = false;
}

bool Replicating() { return Net().Online() && Net().IsHost() && !Net().Peers().empty() && (!g_currentScript || SharedStoryScript(g_currentScript)) && (g_active || g_sharedScene || CutsceneRunning() || HostOnMission()); }

// ---------------------------------------------------------------- partner: no mission triggers
void ScriptFaded(int16_t dir, float seconds);
enum : uint8_t { WEAPON_GIVE = 1, WEAPON_TAKE = 2, WEAPON_FINAL = 3, WEAPON_STRIP = 4 };
std::set<int> g_hostMissionWeapons;
uint64_t g_hostWeaponRun = 0;
uint32_t g_storyStripSequence = 0;

// What the host's mission does to its CJ's weapons (GIVE_WEAPON_TO_CHAR $PLAYER_ACTOR, REMOVE_WEAPON_FROM_CHAR) the
// partner gets as well: the spray can of "Tagging Up Turf", the guns some missions hand out. Seen as the change of CJ's
// slots over a mission script's commands.
struct WeaponSlots {
    int32_t type[NUM_WEAPON_SLOTS], ammo[NUM_WEAPON_SLOTS];
    void Take(CPed* p) {
        for (int i = 0; i < NUM_WEAPON_SLOTS; ++i) type[i] = p->Weapons()[i].type, ammo[i] = p->Weapons()[i].ammoTotal;
    }
};
void SendMissionWeapon(uint8_t op, int weapon, int ammo, bool inHand, uint8_t to = proto::kServerId) {
    const auto run = StoryRunId();
    if (!run) return;
    if (g_hostWeaponRun != run) { g_hostMissionWeapons.clear(); g_hostWeaponRun = run; }
    if (op == WEAPON_GIVE || op == WEAPON_TAKE) g_hostMissionWeapons.insert(weapon);
    Writer w(proto::G_MISSION_WEAPON);
    w.U64(run);
    w.U8(to);
    w.U8(op);
    w.U8((uint8_t)weapon);
    w.I32(ammo);
    w.U8(inHand);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Mission weapon %d %s (%d)", weapon, op == WEAPON_GIVE ? "given" : op == WEAPON_TAKE ? "taken" : "settled", ammo);
}
// The count goes as it is now, not what was added: missions lower it too (Emmet's pistol 30000 for the practice and 10
// after it, the spray can given again with 1000 at the end), and the partner kept the big one (no number on the HUD).
void SendWeaponChanges(const WeaponSlots& was, CPed* cj) {
    for (int i = 0; i < NUM_WEAPON_SLOTS; ++i) {
        const CWeapon& now = cj->Weapons()[i];
        if (was.type[i] > 0 && now.type != was.type[i]) SendMissionWeapon(WEAPON_TAKE, was.type[i], 0, false);
        if (now.type > 0 && (now.type != was.type[i] || now.ammoTotal != was.ammo[i]))
            SendMissionWeapon(WEAPON_GIVE, now.type, now.ammoTotal, cj->ActiveWeapon().type == now.type);
    }
}

using ScriptProcessFn = void(__fastcall*)(void*, void*);
ScriptProcessFn g_origScriptProcess = nullptr;
int g_deadMissionPartner=-1;
bool Replicating();
void SendStoryWarp(CPlayerPed* cj,uint8_t target=255,bool advance=true);
CVector g_processStoryPosition;int8_t g_processStoryArea=0;CPlayerPed*g_processStoryPed=nullptr;
void __fastcall HookScriptProcess(void* script, void* edx) {
    profile::Scope timing(profile::SCRIPT_PROCESS);
    // a world load from inside the game: the old scripts would run frames without a player (null peds)
    if (core::IsRestarting() || (g_startPending && script == g_startScript)) return;
    void* prev = g_currentScript;
    g_currentScript = script;
    bool personal=PersonalActivity(script);
    if (Net().IsHost() && Net().Online() && !personal && Field<uint8_t>(script, 0xDC)) {
        if (!StoryRunId()) BeginCooperativeStory(std::string(Field<char[8]>(script, 8), strnlen(Field<char[8]>(script, 8), 8)));
        TrackStoryRoot(script);
    }
    BindStoryThread(script);
    const bool shared = SharedStoryScript(script);
    if(personal&&script==g_localActivityScript&&!Net().IsHost()){
        if(!SharedSceneOwnsScreen()&&(GetPad(0)->DisablePlayerControls||Fading()||Ref<uint8_t>(addr::TheCamera+0x2B)==0))g_localActivityUntil=NowMs()+500;
        else g_localActivityScript=nullptr;
    }
    if(Net().Online()&&!Net().IsHost()&&personal&&Field<uint8_t>(script,0xDC)){
        if(core::IsOnMission()){StopScriptCamera();g_localActivityUntil=NowMs()+500;}
    }
    auto cameraBefore=cam::Source();auto frontBefore=cam::Front();
    bool controlledBefore=Ref<uint8_t>(addr::TheCamera+0x2B)==0;
    bool fading = Fading();
    int16_t dir = FadeDirection();
    float seconds = FadeDuration();
    CPlayerPed* cj = shared && Replicating() ? FindPlayerPed(0) : nullptr;
    CVector positionBefore=cj?StoryPosition(cj):CVector{};
    auto*previousStoryPed=g_processStoryPed;auto previousStoryPosition=g_processStoryPosition;auto previousStoryArea=g_processStoryArea;
    g_processStoryPed=cj;g_processStoryPosition=positionBefore;g_processStoryArea=cj?cj->AreaCode():0;
    int8_t areaBefore=cj?cj->AreaCode():0;
    WeaponSlots weapons;
    if (cj) weapons.Take(cj);
    bool save = !strncmp(Field<char[8]>(script, 0x08), "psave1", 8), flag = save && core::IsOnMission();
    bool onMissionBefore = core::IsOnMission();
    int8_t priorFailure = Ref<int8_t>(0xA444B0); // CTheScripts::FailCurrentMission
    bool partnerFailed = false;
    // Use the mission's native failure/cleanup return path. Remote deaths must
    // not kill CJ or terminate the mission thread before its cleanup runs.
    if (Net().Online() && Net().IsHost() && onMissionBefore && HostOnMission() &&
        Field<uint8_t>(script,0xDC) && Field<uint8_t>(script,0xC6) && !PersonalActivity(script)) {
        for (const auto& [id, peer] : Net().Peers()) {
            auto* rp=Players::Instance().Get(id);
            if (!rp || !rp->hasState || NowMs()-rp->stateRecvMs>=1500 ||
                !(rp->state.flags & OnFootState::F_DEAD)) continue;
            g_deadMissionPartner=id;
            break;
        }
        // Cutscenes can temporarily disable native death checks. Remember the
        // death until the script enables its cleanup path, even after respawn.
        if(g_deadMissionPartner>=0 && Field<uint8_t>(script,0xD3) &&
           Field<uint16_t>(script,0x38)>0 && Field<uint16_t>(script,0x38)<=8) {
            Ref<int32_t>(addr::ScriptSpace+Ref<int32_t>(0xA476AC))=0;
            Field<uint8_t>(script,0xD4)=1; // native HAS_DEATHARREST_BEEN_EXECUTED
            Field<uint32_t>(script,0xCC)=0;
            Ref<int8_t>(0xA444B0)=1;
            CancelCooperativeStage(script);partnerFailed=true;
            LOGI("Mission failure: partner %u died (%.*s)",g_deadMissionPartner,8,Field<char[8]>(script,0x08));
            g_deadMissionPartner=-1;
        }
    }
    auto timersBefore=Field<std::array<int32_t,2>>(script,0xBC);
    bool personalDriving=BeginPersonalDrivingSchool(script);
    BeginAirportGateScript(script);
    g_origScriptProcess(script, edx);
    EndAirportGateScript(script);
    if(personalDriving)EndPersonalDrivingSchool();
    if(!Field<uint8_t>(script,0xC4))CancelCooperativeStage(script);
    if(CooperativeStageWaiting(script))Field<std::array<int32_t,2>>(script,0xBC)=timersBefore;
    if(partnerFailed)Ref<int8_t>(0xA444B0)=priorFailure;
    if (save && core::IsOnMission() != flag) g_saveFlag = !flag;
    if (core::IsOnMission() != onMissionBefore) {
        g_personalFlag = core::IsOnMission() && PersonalActivity(script);
        g_schoolFlag = core::IsOnMission() && IsSchoolScriptName(Field<char[8]>(script,0x08));
    }
    if (cj && !PersonalActivity(script) && FindPlayerPed(0) == cj) {
        SendWeaponChanges(weapons,cj);
        // Physics does not run inside Process: a large displacement here came
        // from this shared story script, rather than ordinary player movement.
        if(!CutsceneRunning()&&(cj->AreaCode()!=areaBefore||(StoryPosition(cj)-positionBefore).Length()>8)){SendStoryWarp(cj);ArmCooperativeStage(script);}
    }
    g_processStoryPed=previousStoryPed;g_processStoryPosition=previousStoryPosition;g_processStoryArea=previousStoryArea;
    // a fade that started during this script's commands is the script's (DO_FADE)
    bool sharedScript = shared && !PersonalActivity(script);
    if (sharedScript && Net().IsHost()) {
        bool controlled=Ref<uint8_t>(addr::TheCamera+0x2B)==0;
        if(controlled&&!CutsceneRunning()&&(!controlledBefore||(cam::Source()-cameraBefore).Length()>.01f||(cam::Front()-frontBefore).Length()>.001f))g_sceneScriptCamera=true;
        if (Fading() && (!fading || FadeDirection() != dir || FadeDuration() != seconds)) ScriptFaded(FadeDirection(), FadeDuration());
    }
    if(personal&&((Fading()&&(!fading||FadeDirection()!=dir||FadeDuration()!=seconds))||
       (Ref<uint8_t>(addr::TheCamera+0x2B)==0&&(!controlledBefore||(cam::Source()-cameraBefore).Length()>.01f)))){
        if(Net().IsHost()){g_sceneScriptCamera=false;g_scriptBlackResetRequested=true;}
        else {g_localActivityUntil=NowMs()+1500;g_localActivityScript=script;}
    }
    g_currentScript = prev;
}

// CLEAR_AREA of the host's mission: alone, the game removes the empty ordinary cars from the place where the mission
// puts its own next, but a car the partner left there stayed (its copy is a mission car here): "Ryder" put Ryder's car
// on it at the pizza place, the car was wrecked and the mission failed. Those copies go now, their owners delete them.
using ClearAreaFn = void(__cdecl*)(CVector*, float, uint8_t);
ClearAreaFn g_origClearArea = nullptr;
void __cdecl HookScriptClearArea(CVector* at, float radius, uint8_t flag) {  // CWorld::ClearExcitingStuffFromArea
    g_origClearArea(at, radius, flag);
    if (!SharedStoryScript(g_currentScript) || !Replicating()) return;
    std::vector<uint16_t> ids = Vehicles::Instance().ClearMirrors(*at, radius);
    if (ids.empty()) return;
    if (ids.size() > 255) ids.resize(255);
    Writer w(proto::G_VEH_CLEAR);
    w.U8((uint8_t)ids.size());
    for(uint16_t id:ids)w.Entity(ENTITY_VEHICLE,id);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("CLEAR_AREA at %.0f %.0f, radius %.0f: %zu cars of the other players cleared", at->x, at->y, radius, ids.size());
}

// The partner takes the host's inventory changes only for the current story run.
void HandleMissionWeapon(uint8_t from, Reader& r) {
    uint64_t run = r.U64();
    uint8_t target = r.U8();
    uint8_t op = r.U8();
    int weapon = r.U8();
    int ammo = r.I32();
    bool inHand = r.U8() != 0;
    if (op == WEAPON_STRIP) {
        auto world = r.U32(), phase = r.U32(), sequence = r.U32(), incarnation = r.U32();
        if (r.Ok() && weapon == 0 && ammo == 0 && !inHand)
            ApplyStoryMissionStrip(from, world, run, phase, sequence, target, incarnation);
        return;
    }
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld() ||
        !run || run != StoryRunId() || (target != proto::kServerId && target != Net().MyId()) || op < WEAPON_GIVE || op > WEAPON_FINAL || weapon <= 0 || weapon > 46 ||
        ammo < (op == WEAPON_FINAL ? -1 : 0)) return;
    ApplyStoryMissionWeapon(op, weapon, ammo, inHand);
}

bool InArea(const Area& a, const CVector& p, float slack) {
    return p.x > std::min(a.x1, a.x2) - slack && p.x < std::max(a.x1, a.x2) + slack &&
           p.y > std::min(a.y1, a.y2) - slack && p.y < std::max(a.y1, a.y2) + slack &&
           (a.z < -50.0f || std::fabs(p.z - a.z) < 3.0f);  // -100: a 2D locate
}

// The marker the host stands in: the yellow ones of the story are the radar's contact points (the radar draws
// them, the trigger only checks where the player is); a locate command's marker drawn in the last second counts too.
bool MarkerAt(const CVector& p, Area& out) {
    for (int i = 0; i < kNumTraces; ++i) {
        uintptr_t t = Trace(i);
        if (!(Ref<uint8_t>(t + 0x25) & 2) || ((Ref<uint8_t>(t + 0x26) >> 2) & 0xF) != 5) continue;  // BLIP_CONTACT_POINT
        // a save house's or a property's marker is a contact point too: the host there held no mission, yet the partner
        // was told it waited
        int8_t sprite = Ref<int8_t>(t + 0x24);
        if (sprite == 31 || sprite == 32 || sprite == 35) continue;  // RADAR_SPRITE_PROPERTYG, PROPERTYR, SAVEGAME
        CVector c = Ref<CVector>(t + 0x08);
        if ((c - p).Length2D() < 2.0f && std::fabs(c.z - p.z) < 3.0f) {
            out = {0x40000002u, c.x - 1.5f, c.y - 1.5f, c.x + 1.5f, c.y + 1.5f, c.z, NowMs()};
            return true;
        }
    }
    for (auto& [id, a] : g_areas)
        if (a.kind == AREA_LOCATE && NowMs() - a.seenMs < 1000 && InArea(a, p, 0.0f)) {
            out = a;
            return true;
        }
    return false;
}

// host, in a trigger: the marker the host stands in holds every partner as well. Without a marker (a phone call,
// a story event) the mission starts as usual.
bool PartnersAtMarker(CPed* host) {
    Area marker;
    if (!MarkerAt(host->GetPosition(), marker)) return true;
    for (auto& [id, peer] : Net().Peers()) {
        RemotePlayer* rp = Players::Instance().Get(id);
        if (!rp || !rp->ped || NowMs() - rp->stateRecvMs >= 1500 || rp->state.area != host->AreaCode() ||
            !InArea(marker, rp->state.pos, 1.0f)) {
            g_waitMs = NowMs();
            g_waitArea = marker;
            return false;
        }
    }
    return true;
}

// Only positive, displayed story locate checks are group checkpoints. Combat,
// traffic, stealth and ordinary position tests retain their native semantics.
struct LocateCheck {
    bool valid = false, threeD = false;
    int ped = -1, draw = 0;
    float x=0, y=0, z=0, dx=0, dy=0, dz=4;
} g_locateCheck;
LocateCheck ReadLocate(void* script, uint16_t opcode) {
    bool two = opcode >= 0x00EC && opcode <= 0x00F1;
    bool three = opcode >= 0x00FE && opcode <= 0x0103;
    LocateCheck check;
    if ((!two && !three) || script != g_currentScript || !SharedStoryScript(script) || Field<uint8_t>(script,0xD2)) return check;
    // LocateCharCommand collects the actor and coordinates in separate batches,
    // overwriting ScriptParams. Read its native parameter stream on a script copy.
    alignas(16) uint8_t copy[0xE0]; memcpy(copy,script,sizeof copy);
    uint32_t saved[32];memcpy(saved,(void*)0xA43C78,sizeof saved);
    CallMethod<0x464080>(copy, three ? 8 : 6);
    constexpr uintptr_t p=0xA43C78;
    check.valid=true; check.threeD=three; check.ped=Ref<int32_t>(p);
    check.x=Ref<float>(p+4);check.y=Ref<float>(p+8);
    check.z=three?Ref<float>(p+12):FindPlayerPed(0)->GetPosition().z;
    check.dx=std::fabs(Ref<float>(p+(three?4:3)*4));
    check.dy=std::fabs(Ref<float>(p+(three?5:4)*4));
    check.dz=three?std::max(1.0f,std::fabs(Ref<float>(p+24))):100000.0f;
    check.draw=Ref<int32_t>(p+(three?7:5)*4);
    memcpy((void*)p,saved,sizeof saved);
    return check;
}
bool GroupLocateReady(void* script) {
    const auto& c=g_locateCheck;
    if (!c.valid || !c.draw || script!=g_currentScript || !Replicating()) return true;
    auto* me=FindPlayerPed(0);const auto& pos=me->GetPosition();
    if(c.ped!=GetPedRef(me)||std::fabs(pos.x-c.x)>c.dx+1||std::fabs(pos.y-c.y)>c.dy+1||std::fabs(pos.z-c.z)>c.dz)return true;
    return StoryGroupReady(c.x,c.y,c.z,me->AreaCode(),std::max(c.dx,c.dy)+1,c.dz);
}
using CommandsFn = int8_t(__fastcall*)(void*, void*, int);
// Track actual object operands of story commands, not every world object marked
// MISSION_OBJECT by the engine (ordinary shop doors use that flag as well).
bool ObjectOperand(int opcode){switch(opcode&0x7FFF){
        case 0x0108: // DELETE_OBJECT
        case 0x0176: // GET_OBJECT_HEADING
        case 0x0177: // SET_OBJECT_HEADING
        case 0x0188: // ADD_BLIP_FOR_OBJECT
        case 0x01BB: // GET_OBJECT_COORDINATES
        case 0x01BC: // SET_OBJECT_COORDINATES
        case 0x01C4: // MARK_OBJECT_AS_NO_LONGER_NEEDED
        case 0x01C7: // DONT_REMOVE_OBJECT
        case 0x02CC: // IS_OBJECT_ON_SCREEN
        case 0x034D: // ROTATE_OBJECT
        case 0x034E: // SLIDE_OBJECT
        case 0x035C: // PLACE_OBJECT_RELATIVE_TO_CAR
        case 0x035D: // MAKE_OBJECT_TARGETTABLE
        case 0x0366: // HAS_OBJECT_BEEN_DAMAGED
        case 0x0381: // SET_OBJECT_VELOCITY
        case 0x0382: // SET_OBJECT_COLLISION
        case 0x038C: // ADD_TO_OBJECT_VELOCITY
        case 0x0392: // SET_OBJECT_DYNAMIC
        case 0x0400: // GET_OFFSET_FROM_OBJECT_IN_WORLD_COORDS
        case 0x0418: // SET_OBJECT_DRAW_LAST
        case 0x0453: // SET_OBJECT_ROTATION
        case 0x04D9: // SET_OBJECT_RECORDS_COLLISIONS
        case 0x04DA: // HAS_OBJECT_COLLIDED_WITH_ANYTHING
        case 0x04E5: // LOCATE_OBJECT_2D
        case 0x04E6: // LOCATE_OBJECT_3D
        case 0x04E7: // IS_OBJECT_IN_WATER
        case 0x04E9: // IS_OBJECT_IN_AREA_2D
        case 0x04EA: // IS_OBJECT_IN_AREA_3D
        case 0x050E: // SORT_OUT_OBJECT_COLLISION_WITH_CAR
        case 0x0550: // FREEZE_OBJECT_POSITION
        case 0x0566: // SET_OBJECT_AREA_VISIBLE
        case 0x059F: // GET_OBJECT_VELOCITY
        case 0x05A1: // ADD_TO_OBJECT_ROTATION_VELOCITY
        case 0x05A2: // SET_OBJECT_ROTATION_VELOCITY
        case 0x05A3: // IS_OBJECT_STATIC
        case 0x05A6: // GET_OBJECT_ROTATION_VELOCITY
        case 0x05A7: // ADD_VELOCITY_RELATIVE_TO_OBJECT_VELOCITY
        case 0x05A8: // GET_OBJECT_SPEED
        case 0x0654: // SET_OBJECT_RENDER_SCORCHED
        case 0x0681: // ATTACH_OBJECT_TO_CAR
        case 0x0682: // DETACH_OBJECT
        case 0x0685: // IS_OBJECT_ATTACHED
        case 0x069A: // ATTACH_OBJECT_TO_OBJECT
        case 0x069B: // ATTACH_OBJECT_TO_CHAR
        case 0x071E: // GET_OBJECT_HEALTH
        case 0x071F: // SET_OBJECT_HEALTH
        case 0x0723: // BREAK_OBJECT
        case 0x0750: // SET_OBJECT_VISIBLE
        case 0x075A: // PLAY_OBJECT_ANIM
        case 0x0796: // GET_ROPE_HEIGHT_FOR_OBJECT
        case 0x0797: // SET_ROPE_HEIGHT_FOR_OBJECT
        case 0x0798: // GRAB_ENTITY_ON_ROPE_FOR_OBJECT
        case 0x0799: // RELEASE_ENTITY_FROM_ROPE_FOR_OBJECT
        case 0x07C3: // GET_OBJECT_QUATERNION
        case 0x07C4: // SET_OBJECT_QUATERNION
        case 0x07F7: // SET_OBJECT_COLLISION_DAMAGE_EFFECT
        case 0x080A: // GET_LEVEL_DESIGN_COORDS_FOR_OBJECT
        case 0x0815: // SET_OBJECT_COORDINATES_AND_VELOCITY
        case 0x0827: // CONNECT_LODS
        case 0x0833: // HAS_OBJECT_BEEN_PHOTOGRAPHED
        case 0x0836: // SET_OBJECT_ANIM_SPEED
        case 0x0837: // IS_OBJECT_PLAYING_ANIM
        case 0x0839: // GET_OBJECT_ANIM_CURRENT_TIME
        case 0x083A: // SET_OBJECT_ANIM_CURRENT_TIME
        case 0x0875: // SET_OBJECT_ONLY_DAMAGED_BY_PLAYER
        case 0x08D2: // SET_OBJECT_SCALE
        case 0x08E3: // IS_OBJECT_IN_ANGLED_AREA_2D
        case 0x08E4: // IS_OBJECT_IN_ANGLED_AREA_3D
        case 0x08E9: // SET_OBJECT_AS_STEALABLE
        case 0x08FF: // HAS_OBJECT_BEEN_DAMAGED_BY_WEAPON
        case 0x0900: // CLEAR_OBJECT_LAST_WEAPON_DAMAGE
        case 0x0905: // LOCK_DOOR
        case 0x0906: // SET_OBJECT_MASS
        case 0x0907: // GET_OBJECT_MASS
        case 0x0908: // SET_OBJECT_TURN_MASS
        case 0x0909: // GET_OBJECT_TURN_MASS
        case 0x0916: // WINCH_CAN_PICK_OBJECT_UP
        case 0x095B: // HAS_OBJECT_BEEN_UPROOTED
        case 0x0977: // IS_OBJECT_WITHIN_BRAIN_ACTIVATION_RANGE
        case 0x097B: // REPORT_MISSION_AUDIO_EVENT_AT_OBJECT
        case 0x0984: // GET_OBJECT_MODEL
        case 0x09A2: // REMOVE_OBJECT_ELEGANTLY
        case 0x09CA: // SET_OBJECT_PROOFS
        case 0x09CC: // DOES_OBJECT_HAVE_THIS_MODEL
        case 0x09FC: // IS_OBJECT_INTERSECTING_WORLD
        case 0x0A0A: // ENABLE_DISABLED_ATTRACTORS_ON_OBJECT
            return true;default:return false;}}
struct ObjectCommand {int handle=-1,model=-1;bool create=false;};
ObjectCommand ReadStoryObject(void*script,int opcode){
    ObjectCommand value;opcode&=0x7FFF;
    bool create=opcode==0x0107||opcode==0x029B;
    bool closest=opcode==0x0363; // coordinate-based object visibility used by story scripts
    if(!create&&!closest&&!ObjectOperand(opcode))return value;
    if(script!=g_currentScript||!SharedStoryScript(script)||!Replicating())return value;
    value.create=create;
    alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);
    uint32_t saved[32];memcpy(saved,(void*)0xA43C78,sizeof saved);CallMethod<0x464080>(copy,closest?6:1);
    if(closest){
        CVector at{Ref<float>(0xA43C78),Ref<float>(0xA43C7C),Ref<float>(0xA43C80)};
        float distance=Ref<float>(0xA43C84);int model=Ref<int32_t>(0xA43C88);auto*pool=ObjectPool();
        for(int i=0;pool&&i<pool->size;++i)if(pool->byteMap[i]>=0){
            auto*o=(CObject*)(pool->objects+(size_t)i*kObjectPoolElemSize);float d=(o->GetPosition()-at).Length();
            if(o->ModelIndex()==model&&d<distance){distance=d;value.handle=GetObjectRef(o);}
        }
    }else if(value.create)value.model=Ref<int32_t>(0xA43C78);else value.handle=Ref<int32_t>(0xA43C78);
    memcpy((void*)0xA43C78,saved,sizeof saved);
    if(value.handle>=0)ShareMissionObject(value.handle);
    return value;
}
CommandsFn g_objectCommands[27]{};
template<int Group>int8_t __fastcall HookStoryObjects(void*script,void*edx,int opcode){
    const int chainOp=opcode&0x7FFF;
    // These are native main.scm helpers, not children of a story mission.
    // Only their qualified airport commands may replace native movement.
    if (script == g_currentScript) {
        int count = AirportGateOperandCount(script, chainOp);
        if (count > 0 && count <= 8) {
            alignas(16) uint8_t copy[0xE0]; memcpy(copy, script, sizeof copy);
            uint32_t saved[32]; memcpy(saved, (void*)0xA43C78, sizeof saved);
            int32_t parameters[8]{};
            CallMethod<0x464080>(copy, count);
            memcpy(parameters, (void*)0xA43C78, count * 4);
            auto* next = Field<uint8_t*>(copy, 0x14);
            memcpy((void*)0xA43C78, saved, sizeof saved);
            bool value = false;
            if (HandleAirportGateCommand(script, chainOp, parameters, value)) {
                Field<uint8_t*>(script, 0x14) = next;
                if (chainOp == 0x0018 || chainOp == 0x0038 || chainOp == 0x034E || chainOp == 0x0471) CallMethod<0x4859D0>(script, value);
                return 0;
            }
        }
    }
    bool story=script==g_currentScript&&SharedStoryScript(script)&&Replicating();
    if(story){ObserveCooperativeRCMissionScript(script);ObserveCooperativeRCFlightScript(script);}
    if(story&&(chainOp==0x05EB||chainOp==0x0705||chainOp==0x085E||chainOp==0x014E||chainOp==0x03C3||chainOp==0x01B4||chainOp==0x0715||(chainOp==0x01BD&&CooperativeStageArmed(script)))){
        uint32_t saved[32];memcpy(saved,(void*)0xA43C78,sizeof saved);alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);int count=chainOp==0x01B4||chainOp==0x0715?2:1;CallMethod<0x464080>(copy,count);int first=Ref<int32_t>(0xA43C78),second=Ref<int32_t>(0xA43C7C);memcpy((void*)0xA43C78,saved,sizeof saved);
        // Scripts such as SYN1 enable control in their per-frame loop. Only
        // the transition out of a held scene needs a readiness barrier.
        bool gate=chainOp!=0x01B4||(first==0&&second==1&&CooperativeStageArmed(script)&&GetPad(0)->DisablePlayerControls);
        uint16_t source=0;
        if(chainOp==0x05EB||chainOp==0x0705||chainOp==0x085E||chainOp==0x0715)if(auto*v=GetVehicle(chainOp==0x0715?second:first))if(auto*n=Vehicles::Instance().EnsureNetworked(v))source=n->netId;
        bool relocated=g_processStoryPed&&FindPlayerPed(0)==g_processStoryPed&&!CutsceneRunning()&&
           (g_processStoryPed->AreaCode()!=g_processStoryArea||(StoryPosition(g_processStoryPed)-g_processStoryPosition).Length()>8);
        // A native recording can complete the relocation itself (Stowaway's
        // bike enters the cargo bay through START + SKIP_TO_END). Let that
        // atomic entry finish; the next fade/control gate waits at its result.
        if(relocated&&(chainOp==0x05EB||chainOp==0x0705||chainOp==0x085E))gate=false;
        if(gate&&relocated)SendStoryWarp(g_processStoryPed);
        if(gate&&!AwaitCooperativeStage(script,chainOp,source)){Field<uint8_t*>(script,0x14)-=2;Field<uint32_t>(script,0xCC)=Ref<uint32_t>(addr::TimeInMs)+1;return 1;}
    }
    if(Net().Online()&&script==g_currentScript&&chainOp==0x01B4){
        alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);uint32_t saved[32];memcpy(saved,(void*)0xA43C78,sizeof saved);
        CallMethod<0x464080>(copy,2);int player=Ref<int32_t>(0xA43C78),enabled=Ref<int32_t>(0xA43C7C);memcpy((void*)0xA43C78,saved,sizeof saved);
        if(player==0&&!enabled&&!BeginNativeInteraction(script)){Field<uint8_t*>(script,0x14)-=2;Field<uint32_t>(script,0xCC)=Ref<uint32_t>(addr::TimeInMs)+1;return 1;}
        if(player==0&&enabled)EndNativeInteraction(script);
    }
    if (story && chainOp == 0x0672) { // TASK_DESTROY_CAR: retain the car which caused the story condition.
        alignas(16) uint8_t copy[0xE0]; memcpy(copy, script, sizeof copy);
        uint32_t saved[32]; memcpy(saved, (void*)0xA43C78, sizeof saved);
        CallMethod<0x464080>(copy, 2);
        int32_t parameters[2]; memcpy(parameters, (void*)0xA43C78, sizeof parameters);
        auto* next = Field<uint8_t*>(copy, 0x14);
        memcpy((void*)0xA43C78, saved, sizeof saved);
        int source = CooperativeStorySourceVehicle(script, parameters[1]);
        if (source >= 0) {
            parameters[1] = source;
            uint8_t args[10]{};
            for (int i = 0; i < 2; ++i) { args[i * 5] = 1; memcpy(args + i * 5 + 1, parameters + i, 4); }
            Field<uint8_t*>(script, 0x14) = args;
            auto result = g_objectCommands[Group](script, edx, opcode);
            Field<uint8_t*>(script, 0x14) = next;
            return result;
        }
    }
    int follow=chainOp==0x05BF?3:(chainOp==0x0639||chainOp==0x0859)?2:(chainOp==0x0677||chainOp==0x0823)?4:0;
    if(story&&follow){
        alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);uint32_t saved[32];memcpy(saved,(void*)0xA43C78,sizeof saved);int32_t parameters[4]{};
        CallMethod<0x464080>(copy,follow);memcpy(parameters,(void*)0xA43C78,follow*4);auto*next=Field<uint8_t*>(copy,0x14);memcpy((void*)0xA43C78,saved,sizeof saved);
        auto*npc=GetPed(parameters[0]);auto*n=npc?Peds::Instance().Find(npc):nullptr;int actor=CooperativeStoryActor(script,0x00A0,parameters[1]);
        if(actor>=0&&n&&n->owner==Net().MyId()&&npc->CreatedBy()==PED_MISSION&&npc->Health()>0){
            parameters[1]=actor;uint8_t args[20]{};for(int i=0;i<follow;++i){args[i*5]=1;memcpy(args+i*5+1,parameters+i,4);}
            Field<uint8_t*>(script,0x14)=args;auto result=g_objectCommands[Group](script,edx,opcode);Field<uint8_t*>(script,0x14)=next;
            LOGD("Story follow-up opcode=%04X npc=%d participant=%d",chainOp,parameters[0],actor);return result;
        }
    }
    int participantCheck = chainOp == 0x0448 ? 2 : (chainOp == 0x044B || chainOp == 0x047A) ? 1 : 0;
    if (story && participantCheck) {
        alignas(16) uint8_t copy[0xE0]; memcpy(copy, script, sizeof copy);
        uint32_t saved[32]; memcpy(saved, (void*)0xA43C78, sizeof saved);
        int32_t parameters[2]{};
        CallMethod<0x464080>(copy, participantCheck);
        memcpy(parameters, (void*)0xA43C78, participantCheck * 4);
        auto* next = Field<uint8_t*>(copy, 0x14);
        memcpy((void*)0xA43C78, saved, sizeof saved);
        int actor = CooperativeStoryActor(script, chainOp, parameters[0]);
        if (actor >= 0) {
            // Evaluate the exact participant in native code, including NOT.
            // OR-ing a guest result cannot represent NOT IS_CHAR_ON_FOOT.
            parameters[0] = actor;
            uint8_t args[10]{};
            for (int i = 0; i < participantCheck; ++i) { args[i * 5] = 1; memcpy(args + i * 5 + 1, parameters + i, 4); }
            Field<uint8_t*>(script, 0x14) = args;
            auto result = g_objectCommands[Group](script, edx, opcode);
            Field<uint8_t*>(script, 0x14) = next;
            return result;
        }
    }
    if (story && chainOp == 0x0605) {
        alignas(16) uint8_t copy[0xE0]; memcpy(copy, script, sizeof copy);
        uint32_t saved[32]; memcpy(saved, (void*)0xA43C78, sizeof saved);
        CallMethod<0x464080>(copy, 1); int actor = Ref<int32_t>(0xA43C78);
        // Use the game's typed string reader on a script copy. Pascal labels
        // may exceed the native task's small name buffer, so keep full space.
        char name[256]{}, block[256]{};
        CallMethod<0x463D50>(copy, name, (uint8_t)255);
        CallMethod<0x463D50>(copy, block, (uint8_t)255);
        int32_t parameters[6]{};
        CallMethod<0x464080>(copy, 6); memcpy(parameters, (void*)0xA43C78, sizeof parameters);
        auto* next = Field<uint8_t*>(copy, 0x14);
        memcpy((void*)0xA43C78, saved, sizeof saved);
        float blend; memcpy(&blend, parameters, sizeof blend);
        if (CooperativeStoryAnimation(script, actor, name, block, blend,
                                      parameters[1], parameters[2], parameters[3], parameters[4], parameters[5])) {
            Field<uint8_t*>(script, 0x14) = next;
            return 0;
        }
    }
    auto query=StoryContinuationQuery(chainOp);
    if(story&&query.inputs){
        alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);uint32_t saved[32];memcpy(saved,(void*)0xA43C78,sizeof saved);
        CallMethod<0x464080>(copy,query.inputs);int params[4]{};memcpy(params,(void*)0xA43C78,query.inputs*4);int cj=params[0];auto*out=Field<uint8_t*>(copy,0x14);memcpy((void*)0xA43C78,saved,sizeof saved);
        int outputs=query.outputs;bool descriptors=true;for(int i=0;i<outputs;++i)descriptors&=out[i*3]==2||out[i*3]==3;
        int actor=CooperativeStoryActor(script,chainOp,cj);
        if(actor>=0&&descriptors){
            auto*ped=GetPed(actor);bool carQuery=query.vehicle;auto*vehicle=CooperativeStoryVehicle(script);
            if(!ped||(carQuery&&!vehicle))return g_objectCommands[Group](script,edx,opcode);
            auto*previous=ped->Vehicle();bool wasInside=ped->InVehicle();
            struct RestoreVehicleQuery{CPed*ped;CVehicle*previous;bool inside,apply;~RestoreVehicleQuery(){if(!apply)return;Field<CVehicle*>(ped,0x58C)=previous;auto&flags=Field<uint32_t>(ped,0x46C);flags=(flags&~0x100u)|(inside?0x100u:0);}} restore{ped,previous,wasInside,carQuery};
            if(carQuery){Field<CVehicle*>(ped,0x58C)=vehicle;Field<uint32_t>(ped,0x46C)|=0x100u;}
            uint8_t args[32]{};params[0]=actor;for(int i=0;i<query.inputs;++i){args[i*5]=1;memcpy(args+i*5+1,params+i,4);}memcpy(args+query.inputs*5,out,outputs*3);
            Field<uint8_t*>(script,0x14)=args;auto result=g_objectCommands[Group](script,edx,opcode);Field<uint8_t*>(script,0x14)=out+outputs*3;
            LOGD("Story chain opcode=%04X actor=%d originalCJ=%d",chainOp,actor,cj);return result;
        }
    }
    if(story&&chainOp==0x02E3){
        alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);
        uint32_t saved[32];memcpy(saved,(void*)0xA43C78,sizeof saved);
        CallMethod<0x464080>(copy,1);int car=Ref<int32_t>(0xA43C78);
        memcpy((void*)0xA43C78,saved,sizeof saved);int32_t*out=nullptr;
        auto*ip=Field<uint8_t*>(copy,0x14);
        if(*ip==2||*ip==3){out=CallMethod<0x464790,int32_t*>(copy,0);
            if(out&&(uint8_t*)out>=copy&&(uint8_t*)out<copy+sizeof copy)out=(int32_t*)((uint8_t*)script+((uint8_t*)out-copy));}
        auto result=g_objectCommands[Group](script,edx,opcode);
        float speed;if(out&&CooperativeStoryContactSpeed(script,car,speed))memcpy(out,&speed,4);
        return result;
    }
    if(story&&(chainOp==0x01BD||chainOp==0x0801)){
        alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);
        auto*ip=Field<uint8_t*>(copy,0x14);int32_t*out=nullptr;
        if(*ip==2||*ip==3){out=CallMethod<0x464790,int32_t*>(copy,0);if(out&&(uint8_t*)out>=copy&&(uint8_t*)out<copy+sizeof copy)out=(int32_t*)((uint8_t*)script+((uint8_t*)out-copy));}
        auto result=g_objectCommands[Group](script,edx,opcode);
        if(out){if(chainOp==0x01BD)*out=(int32_t)CooperativeStoryClock();else{float fov;if(CooperativePhotoFov(fov))memcpy(out,&fov,4);}}
        return result;
    }
    if(story&&(chainOp==0x004E||chainOp==0x00D8)){CancelCooperativeStage(script);ClearCooperativeStoryScript(script);}
    if(story&&(chainOp==0x016A||chainOp==0x03CB||chainOp==0x04BB||chainOp==0x01B4)){
        // Arm the next native timestamp at a real scene/control transition;
        // ordinary per-frame timer reads and free driving do not open barriers.
        alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);uint32_t saved[32];memcpy(saved,(void*)0xA43C78,sizeof saved);
        CallMethod<0x464080>(copy,chainOp==0x03CB?3:chainOp==0x04BB?1:2);bool arm=chainOp!=0x01B4||(Ref<int32_t>(0xA43C78)==0&&Ref<int32_t>(0xA43C7C)==0);memcpy((void*)0xA43C78,saved,sizeof saved);if(arm)ArmCooperativeStage(script);
    }
    if(story){
        int taskCount=chainOp==0x0713?10:chainOp==0x05CA?4:(chainOp==0x0632||chainOp==0x0633||chainOp==0x05CD||chainOp==0x0622||chainOp==0x05BA||chainOp==0x05D3||chainOp==0x0605||chainOp==0x0792||chainOp==0x0687||chainOp==0x0465)?1:(chainOp==0x05E2||chainOp==0x0630||chainOp==0x0631)?2:(chainOp==0x0635||chainOp==0x074D||chainOp==0x07A5)?3:chainOp==0x0634?5:0;
        if(taskCount){alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);uint32_t saved[32];memcpy(saved,(void*)0xA43C78,sizeof saved);int32_t params[10]{};
            CallMethod<0x464080>(copy,taskCount);memcpy(params,(void*)0xA43C78,taskCount*4);memcpy((void*)0xA43C78,saved,sizeof saved);ObserveStoryCommand(chainOp,params);}
        if(chainOp==0x0318)MarkCooperativeStoryPassed();
    }
    auto object=ReadStoryObject(script,opcode);
    uint8_t before=Field<uint8_t>(script,0xC5);int16_t logic=Field<int16_t>(script,0xD0);bool coop=story&&chainOp==0x09C8;bool rcOverride=false,rcValue=false;int32_t*killOutput=nullptr;int killModel=-1;
    int op=opcode&0x7FFF;int count=(op==0x00A3||op==0x02D6||op==0x0474||(op>=0x00EC&&op<=0x00EE))?6:(op==0x00A4||op==0x04E6||op==0x01AF||(op>=0x00FE&&op<=0x0100))?8:(op==0x0480||op==0x00E1||op==0x0179||op==0x023B||op==0x031E||op==0x051C||op==0x051D||op==0x0448||op==0x08FF||op==0x09ED||op==0x06B7||op==0x02D8||op==0x0737||op==0x0364||op==0x031D||op==0x00DB||op==0x00DD||op==0x051A||op==0x0298||op==0x0897)?2:(op==0x04C5||op==0x0833||op==0x054E||op==0x0467||op==0x0468||op==0x054F||op==0x0900||op==0x0366||op==0x0297||op==0x044B||op==0x00DF||op==0x02CC)?1:0;
    int clearedDamageCar = -1;
    if((count||op==0x04DC)&&script==g_currentScript&&SharedStoryScript(script)&&Replicating()){
        alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);uint32_t saved[32];memcpy(saved,(void*)0xA43C78,sizeof saved);
        int32_t params[10]{};if(count){CallMethod<0x464080>(copy,count);memcpy(params,(void*)0xA43C78,count*4);}memcpy((void*)0xA43C78,saved,sizeof saved);
        if(op==0x054E||op==0x0467)ClearStoryDamage(params[0]);
        else if(op==0x0468)ClearStoryEntityDamage(ENTITY_VEHICLE,params[0],false);
        else if(op==0x054F)clearedDamageCar=params[0];
        else if(op==0x0900)ClearStoryEntityDamage(ENTITY_OBJECT,params[0]);
        else if(op==0x0297&&params[0]==0)ResetCooperativeKills();
        else if(op==0x0298&&params[0]==0){
            auto&ip=Field<uint8_t*>(copy,0x14);uint8_t type=*ip;
            if(type==2||type==3){killOutput=CallMethod<0x464790,int32_t*>(copy,0);
                // Mission locals live in the shared mission-local bank, while
                // ordinary script locals live inside CRunningScript itself.
                if(killOutput&&(uint8_t*)killOutput>=copy&&(uint8_t*)killOutput<copy+sizeof(copy))
                    killOutput=(int32_t*)((uint8_t*)script+((uint8_t*)killOutput-copy));
                killModel=params[1];}
        }else{
            ObserveCooperativeRCFlightCommand(script, op, params);
            rcOverride=CooperativeRCCondition(script,op,params,rcValue);
            if(!rcOverride)rcOverride=CooperativeRCFlightCondition(script,op,params,rcValue);
            if(!rcOverride&&op==0x00E1)rcOverride=CooperativeStoryButton(script,params[0],params[1],rcValue);
            if(!rcOverride)coop=CooperativeStoryCondition(op,params,script);
        }
    }
    int mapParams[6]{};bool mapEdit=story&&(chainOp==0x0363||chainOp==0x088D||chainOp==0x03B6);
    if(mapEdit){alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);uint32_t saved[32];memcpy(saved,(void*)0xA43C78,sizeof saved);CallMethod<0x464080>(copy,6);memcpy(mapParams,(void*)0xA43C78,sizeof mapParams);memcpy((void*)0xA43C78,saved,sizeof saved);}
    if(mapEdit)mapEdit=ResolveStoryMapTarget(chainOp,mapParams);
    std::vector<std::pair<uint8_t,uint32_t>> stripParticipants;
    uint64_t stripRun = 0; uint32_t stripWorld = 0, stripPhase = 0;
    if (story && chainOp == 0x048F) {
        alignas(16) uint8_t copy[0xE0]; memcpy(copy, script, sizeof copy);
        uint32_t saved[32]; memcpy(saved, (void*)0xA43C78, sizeof saved);
        CallMethod<0x464080>(copy, 1); int actor = Ref<int32_t>(0xA43C78);
        memcpy((void*)0xA43C78, saved, sizeof saved);
        auto* cj = FindPlayerPed(0);
        if (cj && actor == GetPedRef(cj)) {
            stripRun = StoryRunId(); stripWorld = Net().WorldEpoch(); stripPhase = CooperativeStoryPhase();
            for (const auto& [id, peer] : Net().Peers())
                if (peer.incarnation && CooperativeInventoryParticipant(id)) stripParticipants.push_back({id, peer.incarnation});
        }
    }
    auto savedSubtitleSetting=Ref<uint8_t>(0xBA678C);int previousPreference=g_storySubtitlePreference;
    if(story){g_storySubtitlePreference=savedSubtitleSetting;Ref<uint8_t>(0xBA678C)=1;}
    auto result=g_objectCommands[Group](script,edx,opcode);
    if(story){Ref<uint8_t>(0xBA678C)=savedSubtitleSetting;g_storySubtitlePreference=previousPreference;}
    if (stripRun && stripRun == StoryRunId() && stripWorld == Net().WorldEpoch() && stripPhase == CooperativeStoryPhase()) {
        // Native loss is permanent. Retire role snapshots before issuing the
        // strip, so later detach/profile restore cannot return the old guns.
        RetireCooperativeStoryRoles();
        if (++g_storyStripSequence == 0) ++g_storyStripSequence;
        for (auto [id, incarnation] : stripParticipants) {
            Writer w(proto::G_MISSION_WEAPON);
            w.U64(stripRun); w.U8(id); w.U8(WEAPON_STRIP); w.U8(0); w.I32(0); w.U8(0);
            w.U32(stripWorld); w.U32(stripPhase); w.U32(g_storyStripSequence); w.U32(incarnation);
            Net().Send(proto::CH_RELIABLE, w);
        }
        LOGI("Story native inventory stripped run=%llu phase=%u participants=%zu", (unsigned long long)stripRun, stripPhase, stripParticipants.size());
    }
    if (clearedDamageCar >= 0) {
        ClearStoryEntityDamage(ENTITY_VEHICLE, clearedDamageCar);
        if (auto* car = GetVehicle(clearedDamageCar))
            if (auto* nv = Vehicles::Instance().Find(car); nv && nv->owner == Net().MyId()) RecordStoryVehicleDamage(car, nullptr);
    }
    if(mapEdit)RecordStoryMapCommand(chainOp,mapParams);
    if(story&&(chainOp==0x015A||chainOp==0x02EB))g_sceneScriptCamera=false;
    if(killOutput)*killOutput=std::clamp<int64_t>((int64_t)*killOutput+CooperativeGuestKills(killModel),0,1000000);
    if(rcOverride||coop){Field<uint8_t>(script,0xC5)=before;Field<int16_t>(script,0xD0)=logic;CallMethod<0x4859D0>(script,rcOverride?rcValue:true);}
    if(object.create){int h=Ref<int32_t>(0xA43C78);auto*o=GetObject(h);if(o&&(object.model<0||o->ModelIndex()==object.model)){ShareMissionObject(h);if(story)ArmCooperativeStage(script);}}
    return result;
}
void InstallStoryObjectCommands(){
    g_objectCommands[0]=Ref<CommandsFn>(0x8A6168+0*4);mem::Put(0x8A6168+0*4,&HookStoryObjects<0>);
    g_objectCommands[1]=Ref<CommandsFn>(0x8A6168+1*4);mem::Put(0x8A6168+1*4,&HookStoryObjects<1>);
    g_objectCommands[2]=Ref<CommandsFn>(0x8A6168+2*4);mem::Put(0x8A6168+2*4,&HookStoryObjects<2>);
    g_objectCommands[3]=Ref<CommandsFn>(0x8A6168+3*4);mem::Put(0x8A6168+3*4,&HookStoryObjects<3>);
    g_objectCommands[4]=Ref<CommandsFn>(0x8A6168+4*4);mem::Put(0x8A6168+4*4,&HookStoryObjects<4>);
    g_objectCommands[6]=Ref<CommandsFn>(0x8A6168+6*4);mem::Put(0x8A6168+6*4,&HookStoryObjects<6>);
    g_objectCommands[7]=Ref<CommandsFn>(0x8A6168+7*4);mem::Put(0x8A6168+7*4,&HookStoryObjects<7>);
    g_objectCommands[8]=Ref<CommandsFn>(0x8A6168+8*4);mem::Put(0x8A6168+8*4,&HookStoryObjects<8>);
    g_objectCommands[9]=Ref<CommandsFn>(0x8A6168+9*4);mem::Put(0x8A6168+9*4,&HookStoryObjects<9>);
    g_objectCommands[10]=Ref<CommandsFn>(0x8A6168+10*4);mem::Put(0x8A6168+10*4,&HookStoryObjects<10>);
    g_objectCommands[11]=Ref<CommandsFn>(0x8A6168+11*4);mem::Put(0x8A6168+11*4,&HookStoryObjects<11>);

    g_objectCommands[12]=Ref<CommandsFn>(0x8A6168+12*4);mem::Put(0x8A6168+12*4,&HookStoryObjects<12>);
    g_objectCommands[13]=Ref<CommandsFn>(0x8A6168+13*4);mem::Put(0x8A6168+13*4,&HookStoryObjects<13>);
    g_objectCommands[14]=Ref<CommandsFn>(0x8A6168+14*4);mem::Put(0x8A6168+14*4,&HookStoryObjects<14>);
    g_objectCommands[15]=Ref<CommandsFn>(0x8A6168+15*4);mem::Put(0x8A6168+15*4,&HookStoryObjects<15>);
    g_objectCommands[16]=Ref<CommandsFn>(0x8A6168+16*4);mem::Put(0x8A6168+16*4,&HookStoryObjects<16>);
    g_objectCommands[18]=Ref<CommandsFn>(0x8A6168+18*4);mem::Put(0x8A6168+18*4,&HookStoryObjects<18>);
    g_objectCommands[19]=Ref<CommandsFn>(0x8A6168+19*4);mem::Put(0x8A6168+19*4,&HookStoryObjects<19>);
    g_objectCommands[20]=Ref<CommandsFn>(0x8A6168+20*4);mem::Put(0x8A6168+20*4,&HookStoryObjects<20>);
    g_objectCommands[21]=Ref<CommandsFn>(0x8A6168+21*4);mem::Put(0x8A6168+21*4,&HookStoryObjects<21>);
    g_objectCommands[22]=Ref<CommandsFn>(0x8A6168+22*4);mem::Put(0x8A6168+22*4,&HookStoryObjects<22>);
    g_objectCommands[23]=Ref<CommandsFn>(0x8A6168+23*4);mem::Put(0x8A6168+23*4,&HookStoryObjects<23>);
    g_objectCommands[24]=Ref<CommandsFn>(0x8A6168+24*4);mem::Put(0x8A6168+24*4,&HookStoryObjects<24>);
    g_objectCommands[25]=Ref<CommandsFn>(0x8A6168+25*4);mem::Put(0x8A6168+25*4,&HookStoryObjects<25>);
}

CommandsFn g_origCommands100=nullptr;
int8_t __fastcall HookCommands100(void*script,void*edx,int opcode){
    if(opcode==0x00A6&&Net().Online()&&Net().IsHost()&&script==g_currentScript&&SharedStoryScript(script)){
        alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);
        uint32_t saved[32];memcpy(saved,(void*)0xA43C78,sizeof saved);CallMethod<0x464080>(copy,1);
        auto*v=GetVehicle(Ref<int32_t>(0xA43C78));memcpy((void*)0xA43C78,saved,sizeof saved);
        if(v)Vehicles::Instance().ShareScriptDeletion(v);
    }
    return g_origCommands100(script,edx,opcode);
}
CommandsFn g_origCommands500=nullptr;
int8_t __fastcall HookCommands500(void*script,void*edx,int opcode){
    int handle=-1;
    if(opcode==0x020A&&script==g_currentScript&&Replicating()&&SharedStoryScript(script)){
        alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);
        uint32_t saved[32];memcpy(saved,(void*)0xA43C78,sizeof saved);
        CallMethod<0x464080>(copy,2);handle=Ref<int32_t>(0xA43C78);memcpy((void*)0xA43C78,saved,sizeof saved);
    }
    auto result=g_origCommands500(script,edx,opcode);
    if(opcode==0x020A)LOGD("Story lock opcode: script=%p current=%p handle=%d",script,g_currentScript,handle);
    if(handle>=0)if(auto*v=GetVehicle(handle))Vehicles::Instance().ShareScriptLock(v);
    return result;
}
CommandsFn g_origCommands200 = nullptr;
int8_t __fastcall HookCommands200(void* script, void* edx, int opcode) {
    auto oldLocate=g_locateCheck;
    g_locateCheck=ReadLocate(script,(uint16_t)opcode);
    uint8_t before=Field<uint8_t>(script,0xC5);
    int16_t logic=Field<int16_t>(script,0xD0);
    bool gymOpening=false;
    if(opcode==0x00E1&&script==g_currentScript){
        alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);
        uint32_t saved[32];memcpy(saved,(void*)0xA43C78,sizeof saved);
        CallMethod<0x464080>(copy,2);
        if(Ref<int32_t>(0xA43C78)==0)gymOpening=GymOpeningKey(script,Ref<int32_t>(0xA43C7C))||!NativeInteractionAllowed(script,Ref<int32_t>(0xA43C7C));
        memcpy((void*)0xA43C78,saved,sizeof saved);
    }
    bool payout=opcode==0x0109&&Net().Online()&&Net().IsHost()&&script==g_currentScript&&SharedStoryScript(script);
    int32_t moneyBefore=payout?PlayerInfos()[0].money:0;
    int8_t result=g_origCommands200(script,edx,opcode);
    if(payout){
        int64_t increase=(int64_t)PlayerInfos()[0].money-moneyBefore;
        if(increase>0&&increase<=10000000){g_missionPayout=std::min<int64_t>(10000000,g_missionPayout+increase);LOGI("Story payout +%d (total %d)",(int)increase,(int)g_missionPayout);}
    }
    if(gymOpening||!GroupLocateReady(script)) {
        Field<uint8_t>(script,0xC5)=before;
        Field<int16_t>(script,0xD0)=logic;
        CallMethod<0x4859D0>(script,false);
    }
    g_locateCheck=oldLocate;
    return result;
}

using CanStartFn = bool(__fastcall*)(CPlayerPed*, void*);
CanStartFn g_origCanStart = nullptr;
bool __fastcall HookCanPlayerStartMission(CPlayerPed* player, void* edx) {
    if (!player) return false;
    bool ok = g_origCanStart(player, edx);
    if (!ok || !Net().Online() || !g_currentScript) return ok;
    // the threads that start only side activities, no story missions: as in the single-player game for either player
    // (the partner plays it in his own game, while the host is not on a mission), and nobody waits at their markers
    static const char* const kSideThreads[] = {
        "gym",                                // gyms (GYMLS, GYMSF, GYMLV)
        "trace", "hotr", "kicks", "bloodr",   // race tournament and driving school, hotring, Kick Start, Blood Bowl
        "tri", "psch", "bikes", "bschoo",     // TRIA, flight school, bike school, boat school
        "shoot", "trucks", "quarrys",         // Ammu-Nation range, trucking, quarry
        "trainsl",                            // the airports' and stations' travel
        "psave1",                             // the save points: each saves his own game, nobody waits for the other
    };
    const char* thread = Field<char[8]>(g_currentScript, 0x08);
    for (const char* side : kSideThreads)
        if (!strcmp(thread, side)) return Net().IsHost() || !HostMissionRecent(0);
    auto* ip = Field<uint8_t*>(g_currentScript, 0x14);
    // main.scm threads ask it in mission triggers (not only inside markers, so no hint here)
    if (ip < (uint8_t*)addr::ScriptSpace || ip >= (uint8_t*)addr::ScriptSpace + Ref<int32_t>(kMainScriptSize)) return ok;
    return Net().IsHost() && PartnersAtMarker(player);
}

// A partner who joins the host after a new game's prologue does so in a new game of its own (flow::IsFollower), which
// must not play the prologue again: main.scm launches it ("INTRO") by itself, with no trigger. The mission's code is in
// the mission block by the time its thread starts: the thread ends at once instead.
using StartScriptFn = void*(__cdecl*)(uint8_t*);
StartScriptFn g_origStartScript = nullptr;
void* __cdecl HookStartNewScript(uint8_t* ip) {
    static const uint8_t kIntro[] = {0xA4, 0x03, 0x09, 'I', 'N', 'T', 'R', 'O', 0};  // SCRIPT_NAME 'INTRO'
    if (ip && flow::IsFollower() && !memcmp(ip, kIntro, sizeof kIntro)) {
        ip[0] = 0x4E, ip[1] = 0x00;  // TERMINATE_THIS_SCRIPT (it also ends "a mission runs")
        Fade(1.0f, 1);  // main.scm blacks the screen out before the prologue, which would fade it in
        LOGI("The prologue mission is the host's: this game's copy ends at once");
    }
    const auto parent = g_storyThreads.find(g_currentScript);
    const bool inherit = SharedStoryScript(g_currentScript);
    const uint64_t run = inherit ? parent->second.run : 0;
    const uint64_t parentBirth = inherit ? parent->second.birth : 0;
    void* child = g_origStartScript(ip);
    if (child && inherit && run == StoryRunId() && !PersonalActivity(child)) {
        // A streamed loader may set BaseIP after StartNewScript returns.
        // Bind it on first Process; Init invalidates any previous slot owner.
        g_storyThreads[child] = {run, ++g_storyThreadBirth, parentBirth, g_currentScript, nullptr, false};
        LOGD("Story helper created script=%p parent=%p run=%llu birth=%llu parentBirth=%llu",
             child, g_currentScript, (unsigned long long)run,
             (unsigned long long)g_storyThreadBirth, (unsigned long long)parentBirth);
    }
    return child;
}

void HandleMissionWait(uint8_t from, Reader& r) {
    bool on = r.U8() != 0;
    Area a{};
    if (on) a = {0x40000001u, r.F32(), r.F32(), r.F32(), r.F32(), r.F32(), 0};
    if (!r.Ok() || from != Net().HostId() || Net().IsHost()) return;
    if (on != g_hostWaits) LOGI(on ? "The host waits at a mission marker" : "The host does not wait at the marker any more");
    g_hostWaits = on;
    g_waitArea = a;  // the partner's game may not have this marker yet (story progress comes on reconnect)
}

// ---------------------------------------------------------------- texts
char g_textPool[256][400];  // the game keeps text pointers (message queue, briefs): stable storage
unsigned g_textNext = 0;
const char* PoolText(const std::string& s) {
    char* p = g_textPool[g_textNext++ & 255];
    size_t n = std::min(s.size(), sizeof(g_textPool[0]) - 1);
    memcpy(p, s.data(), n);
    p[n] = 0;
    return p;
}

// GXT texts travel as (mission table, key hash) and each player shows his own game's text: the host and the
// partner may run different translations, whose raw strings come out as gibberish in the other font.
constexpr uintptr_t kTheText = 0xC1B340;  // CText: tkeyMain +0, tkeyMission +0x10, missionTableName +0x24
struct GxtKey {
    const char* text;
    uint32_t hash;
};
struct GxtKeys {
    GxtKey* data;
    uint32_t size;
};

std::string MissionTable() { return std::string((const char*)kTheText + 0x24, strnlen((const char*)kTheText + 0x24, 8)); }

uint32_t GxtHashOf(const char* text, bool& mission) {
    for (int m = 0; m < 2; ++m) {
        const GxtKeys& keys = Ref<GxtKeys>(kTheText + m * 0x10);
        for (uint32_t i = 0; i < keys.size; ++i)
            if (keys.data[i].text == text) {
                mission = m == 1;
                return keys.data[i].hash;
            }
    }
    return 0;  // not a GXT entry (built by a script or a mod): sent as is
}

// Keep the host's translated table independent of local gym/shop scripts.
// Loading another native CText normally clears all HUD messages: suppress that
// side effect only during this private, synchronous table read.
bool g_loadingPrivateText=false;
using ClearAllFn=void(__cdecl*)(bool);ClearAllFn g_originalClearAll=nullptr;
void __cdecl PrivateTextClear(bool flag){if(!g_loadingPrivateText)g_originalClearAll(flag);}
std::map<std::string,std::map<uint32_t,std::string>>g_sharedTranslations;
std::string g_sharedMissionTable,g_cutsceneTextTable;
uint8_t g_translationLanguage=255;
const std::map<uint32_t,std::string>&TranslatedTable(const std::string&table){
    auto language=Ref<uint8_t>(kTheText+0x20);
    if(language!=g_translationLanguage){g_sharedTranslations.clear();g_translationLanguage=language;}
    auto existing=g_sharedTranslations.find(table);if(existing!=g_sharedTranslations.end())return existing->second;
    auto&out=g_sharedTranslations[table];
    if(table.empty())return out;
    alignas(4)uint8_t copy[0xA90];memcpy(copy,(void*)kTheText,sizeof copy);
    memset(copy+0x10,0,16);memset(copy+0x24,0,8);copy[0x23]=0;
    char name[9]{};memcpy(name,table.data(),std::min<size_t>(8,table.size()));
    g_loadingPrivateText=true;CallMethod<0x69FBF0>(copy,(const char*)name);g_loadingPrivateText=false;
    const auto&keys=Field<GxtKeys>(copy,0x10);
    for(uint32_t i=0;i<keys.size;++i)if(keys.data[i].text)out[keys.data[i].hash]=keys.data[i].text;
    CallMethod<0x69F510>(copy+0x10);CallMethod<0x69F640>(copy+0x18);
    return out;
}
void UseMissionTable(const std::string&table){g_sharedMissionTable=table;TranslatedTable(table);}
const char*GxtLocal(uint32_t hash,const std::string&table){
    if(!hash)return nullptr;
    if(!table.empty()){const auto&keys=TranslatedTable(table);auto i=keys.find(hash);return i==keys.end()?nullptr:i->second.c_str();}
    const auto&keys=Ref<GxtKeys>(kTheText);uint32_t lo=0,hi=keys.size;
    while(lo<hi){auto mid=(lo+hi)/2;if(keys.data[mid].hash==hash)return keys.data[mid].text;if(keys.data[mid].hash<hash)lo=mid+1;else hi=mid;}return nullptr;
}
using TextGetFn=const char*(__fastcall*)(void*,void*,const char*);TextGetFn g_originalTextGet=nullptr;
const char*__fastcall SharedTextGet(void*self,void*edx,const char*key){
    // Script code keeps its own table. Only the remote on-screen counters use
    // the host table, looked up by the native game at display time.
    if((uintptr_t)self==kTheText&&Net().Online()&&!Net().IsHost()&&!CurrentScript()&&((g_onscreenSet&&NowMs()>=g_localActivityUntil)||CutsceneRunning())&&key){
        const auto&keys=TranslatedTable(CutsceneRunning()&&!g_cutsceneTextTable.empty()?g_cutsceneTextTable:g_sharedMissionTable);
        // CKeyGen::GetUppercaseKey uses GTA's CRC32, not std::hash.
        auto hash=Call<0x53CF30,uint32_t>(key);auto i=keys.find(hash);if(i!=keys.end())return i->second.c_str();
    }
    return g_originalTextGet(self,edx,key);
}

using AddMsgFn = void(__cdecl*)(const char*, uint32_t, uint16_t, bool);
using AddMsgStringFn=void(__cdecl*)(const char*,uint32_t,uint16_t,const char*,bool);
AddMsgStringFn g_origMsgString=nullptr,g_origMsgQString=nullptr;
using AddMsgNumFn = void(__cdecl*)(const char*, uint32_t, uint16_t, int, int, int, int, int, int, bool);
using AddBigFn = void(__cdecl*)(const char*, uint32_t, uint16_t);
using AddBigNumFn = void(__cdecl*)(const char*, uint32_t, uint16_t, int, int, int, int, int, int);
using HelpFn = void(__cdecl*)(const char*, bool, bool, bool);
using ClearFn = void(__cdecl*)(bool);
using VoidFn = void(__cdecl*)();
AddMsgFn g_origMsg, g_origMsgQ;
AddMsgNumFn g_origMsgNum, g_origMsgQNum;
AddBigFn g_origBig, g_origBigQ;
AddBigNumFn g_origBigNum, g_origBigQNum;
HelpFn g_origHelp;
ClearFn g_origClear;
VoidFn g_origClearSmall;

struct RecentMissionText {std::vector<uint8_t>packet;size_t timeOffset=0;uint64_t expires=0;};
std::map<uint8_t,RecentMissionText>g_recentMissionText;
void ReplayMissionText(uint8_t to){
    const auto now=NowMs();
    for(auto it=g_recentMissionText.begin();it!=g_recentMissionText.end();){
        auto&c=it->second;if(c.expires&&now>=c.expires){it=g_recentMissionText.erase(it);continue;}
        auto bytes=c.packet;bytes.back()=to;
        if(c.expires){auto remaining=(uint32_t)std::min<uint64_t>(c.expires-now,60000);memcpy(bytes.data()+c.timeOffset,&remaining,4);}
        Writer w;w.Raw(bytes.data(),bytes.size());Net().Send(proto::CH_RELIABLE,w);++it;
    }
}

bool g_nativeSubtitle=false,g_textIsSubtitle=false;
struct SubtitlePacket{std::vector<uint8_t>bytes;uint64_t deadline=0,run=0;uint32_t phase=0;};
std::vector<SubtitlePacket>g_pendingSubtitles;uint64_t g_replayingSubtitleDeadline=0;
bool SubtitlesEnabled(){return g_storySubtitlePreference>=0?g_storySubtitlePreference!=0:Ref<uint8_t>(0xBA678C)!=0;}
std::pair<int,uint32_t>SubtitleAudio();
bool SubtitleWaiting(int slot,uint32_t token,uint32_t&time);
bool DialogueBusy();
void __cdecl NativeSubtitle(const char*t,uint32_t time,uint16_t flag,bool brief);
void SendText(uint8_t kind, const char* text, uint32_t time, uint16_t flag, const int* nums, uint8_t bools,const char*insert=nullptr) {
    g_textIsSubtitle=g_nativeSubtitle||((kind==TXT_MSG||kind==TXT_MSG_Q)&&text&&!strncmp(text,"~z~",3));
    if (!Replicating()) return;
    // a running cutscene prints its subtitles itself, outside any script (CCutsceneMgr's text list, AddMessageJumpQ at
    // 0x5B187E), and so does the partner's copy of it: sent too, each line came twice
    if (Ref<uint8_t>(0xB5F851) && !CurrentScript()) return;  // CCutsceneMgr::ms_running
    Writer w(proto::G_TEXT);
    w.U8(kind);
    w.Str(text ? text : "", 399);
    size_t timeOffset=w.Size();w.U32(time);
    w.U16(flag);
    w.U8(nums ? 1 : 0);
    if (nums)
        for (int i = 0; i < 6; ++i) w.I32(nums[i]);
    w.U8(bools);
    bool mission = false;
    w.U32(text ? GxtHashOf(text, mission) : 0);
    w.Str(mission ? MissionTable() : std::string(), 8);w.Bool(g_textIsSubtitle);auto binding=g_textIsSubtitle?SubtitleAudio():std::pair<int,uint32_t>{-1,0};w.I8((int8_t)binding.first);w.U32(binding.second);bool insertMission=false;w.Str(insert?insert:"",399);w.U32(insert?GxtHashOf(insert,insertMission):0);w.Str(insertMission?MissionTable():std::string(),8);w.U8(proto::kServerId);
    if(kind==TXT_CLEAR)g_recentMissionText.clear();
    else if(kind==TXT_CLEAR_SMALL)g_recentMissionText.erase(TXT_MSG);
    else if(kind==TXT_HELP||kind==TXT_MSG||kind==TXT_MSG_Q){
        uint8_t key=kind==TXT_HELP?TXT_HELP:TXT_MSG;
        if(!text||!*text)g_recentMissionText.erase(key);
        else g_recentMissionText[key]={w.Buffer(),timeOffset,kind==TXT_HELP&&(bools&2)?0:NowMs()+(time?time:5000)};
    }
    Net().Send(proto::CH_RELIABLE, w);
}

void __cdecl HookMsg(const char* t, uint32_t time, uint16_t flag, bool brief) {
    SendText(TXT_MSG, t, time, flag, nullptr, brief);
    if(!g_textIsSubtitle||SubtitlesEnabled())g_origMsg(t, time, flag, brief);
}
void __cdecl HookMsgQ(const char* t, uint32_t time, uint16_t flag, bool brief) {
    SendText(TXT_MSG_Q, t, time, flag, nullptr, brief);
    if(!g_textIsSubtitle||SubtitlesEnabled())g_origMsgQ(t, time, flag, brief);
}
void __cdecl HookMsgNum(const char* t, uint32_t time, uint16_t flag, int a, int b, int c, int d, int e, int f, bool brief) {
    int n[6] = {a, b, c, d, e, f};
    SendText(TXT_MSG, t, time, flag, n, brief);
    if(!g_textIsSubtitle||SubtitlesEnabled())g_origMsgNum(t, time, flag, a, b, c, d, e, f, brief);
}
void __cdecl HookMsgQNum(const char* t, uint32_t time, uint16_t flag, int a, int b, int c, int d, int e, int f, bool brief) {
    int n[6] = {a, b, c, d, e, f};
    SendText(TXT_MSG_Q, t, time, flag, n, brief);
    if(!g_textIsSubtitle||SubtitlesEnabled())g_origMsgQNum(t, time, flag, a, b, c, d, e, f, brief);
}
void __cdecl HookMsgString(const char*t,uint32_t time,uint16_t flag,const char*insert,bool brief){
    SendText(TXT_MSG,t,time,flag,nullptr,brief,insert);
    if(!g_textIsSubtitle||SubtitlesEnabled())g_origMsgString(t,time,flag,insert,brief);
}
void __cdecl HookMsgQString(const char*t,uint32_t time,uint16_t flag,const char*insert,bool brief){
    SendText(TXT_MSG_Q,t,time,flag,nullptr,brief,insert);
    if(!g_textIsSubtitle||SubtitlesEnabled())g_origMsgQString(t,time,flag,insert,brief);
}
// The host's own WASTED and BUSTED (CPlayerInfo::KillPlayer, CGameLogic) are not the mission's: the partner's HUD keeps
// that style's text until its own player respawns (CHud::ResetWastedText), so it stayed on the partner's screen.
bool HostDeathText(const char* t) {
    return t && (t == CallMethod<0x6A0050, const char*>((void*)kTheText, "DEAD") ||  // CText::Get
                 t == CallMethod<0x6A0050, const char*>((void*)kTheText, "BUSTED"));
}
void __cdecl HookBig(const char* t, uint32_t time, uint16_t style) {
    if (!HostDeathText(t)) SendText(TXT_BIG, t, time, style, nullptr, 0);
    g_origBig(t, time, style);
}
void __cdecl HookBigQ(const char* t, uint32_t time, uint16_t style) {
    SendText(TXT_BIG_Q, t, time, style, nullptr, 0);
    g_origBigQ(t, time, style);
}
void __cdecl HookBigNum(const char* t, uint32_t time, uint16_t style, int a, int b, int c, int d, int e, int f) {
    int n[6] = {a, b, c, d, e, f};
    SendText(TXT_BIG, t, time, style, n, 0);
    g_origBigNum(t, time, style, a, b, c, d, e, f);
}
void __cdecl HookBigQNum(const char* t, uint32_t time, uint16_t style, int a, int b, int c, int d, int e, int f) {
    int n[6] = {a, b, c, d, e, f};
    SendText(TXT_BIG_Q, t, time, style, n, 0);
    g_origBigQNum(t, time, style, a, b, c, d, e, f);
}
void __cdecl HookHelp(const char* t, bool quick, bool permanent, bool addToBrief) {
    SendText(TXT_HELP, t, 0, 0, nullptr, (quick ? 1 : 0) | (permanent ? 2 : 0) | (addToBrief ? 4 : 0));
    g_origHelp(t, quick, permanent, addToBrief);
}
void __cdecl HookClear(bool flag) {
    SendText(TXT_CLEAR, nullptr, 0, flag, nullptr, 0);
    g_origClear(flag);
}
void __cdecl HookClearSmall() {
    SendText(TXT_CLEAR_SMALL, nullptr, 0, 0, nullptr, 0);
    g_origClearSmall();
}

void __cdecl NativeSubtitle(const char*t,uint32_t time,uint16_t flag,bool brief){
    if(!SubtitlesEnabled())return;
    g_nativeSubtitle=true;HookMsgQ(t,time,flag,brief);g_nativeSubtitle=false;
}
void HandleText(uint8_t from, Reader& r) {
    auto*packetStart=r.Cursor();auto packetSize=r.Remaining();
    uint8_t kind = r.U8();
    std::string text = r.Str(399);
    uint32_t time = r.U32();
    uint16_t flag = r.U16();
    int n[6] = {};
    bool hasNums = r.U8() != 0;
    if (hasNums)
        for (int& v : n) v = r.I32();
    uint8_t bools = r.U8();
    uint32_t hash = r.U32();
    std::string table = r.Str(8);
    bool subtitle=r.Bool();int slot=r.I8();auto token=r.U32();auto insert=r.Str(399);auto insertHash=r.U32();auto insertTable=r.Str(8);
    auto to=r.U8();if(to!=proto::kServerId&&to!=Net().MyId())return;
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld()) return;
    if(subtitle&&!SubtitlesEnabled())return;
    if(subtitle&&SubtitleWaiting(slot,token,time)){
        auto deadline=g_replayingSubtitleDeadline?g_replayingSubtitleDeadline:NowMs()+15000;
        if(NowMs()<deadline)g_pendingSubtitles.push_back({std::vector<uint8_t>(packetStart,packetStart+packetSize),deadline,StoryRunId(),CooperativeStoryPhase()});
        return;
    }
    if(subtitle&&token&&!time)return;
    if(kind==TXT_CLEAR_SMALL&&DialogueBusy())return;
    if(kind==TXT_CLEAR){g_pendingSubtitles.clear();}
    if (const char* own = GxtLocal(hash, table)) text = own;
    if(const char*own=GxtLocal(insertHash,insertTable))insert=own;
    if(!insert.empty()){size_t pos=0;while((pos=text.find("~1~",pos))!=std::string::npos){text.replace(pos,3,insert);pos+=insert.size();if(text.size()>399){text.resize(399);break;}}}
    if(subtitle)LOGD("Subtitle displayed slot=%d token=%u duration=%u",slot,token,time);
    const char* t = text.empty() ? nullptr : PoolText(text);
    switch (kind) {
    case TXT_MSG:
        if (!t) return;
        if (hasNums) g_origMsgNum(t, time, flag, n[0], n[1], n[2], n[3], n[4], n[5], bools & 1);
        else g_origMsg(t, time, flag, bools & 1);
        break;
    case TXT_MSG_Q:
        if (!t) return;
        if (hasNums) g_origMsgQNum(t, time, flag, n[0], n[1], n[2], n[3], n[4], n[5], bools & 1);
        else g_origMsgQ(t, time, flag, bools & 1);
        break;
    case TXT_BIG:
        if (!t) return;
        if (hasNums) g_origBigNum(t, time, flag, n[0], n[1], n[2], n[3], n[4], n[5]);
        else g_origBig(t, time, flag);
        break;
    case TXT_BIG_Q:
        if (!t) return;
        if (hasNums) g_origBigQNum(t, time, flag, n[0], n[1], n[2], n[3], n[4], n[5]);
        else g_origBigQ(t, time, flag);
        break;
    case TXT_HELP: g_origHelp(t, bools & 1, (bools & 2) != 0, (bools & 4) != 0); break;
    case TXT_CLEAR: g_origClear(flag != 0); break;
    case TXT_CLEAR_SMALL: g_origClearSmall(); break;
    }
}

// ---------------------------------------------------------------- markers
using HighlightFn = void(__cdecl*)(uint32_t, float, float, float, float, float);
HighlightFn g_origHighlight = nullptr;
void __cdecl HookHighlight(uint32_t id, float x1, float y1, float x2, float y2, float z) {
    if (Net().Online() && Net().IsHost()) {
        if (!g_areas.count(id)) LOGD("Script marker %08X at %.1f,%.1f,%.1f", id, (x1 + x2) * 0.5f, (y1 + y2) * 0.5f, z);
        g_areas[id] = {id, x1, y1, x2, y2, z, NowMs()};
    }
    g_origHighlight(id, x1, y1, x2, y2, z);
}

// DRAW_SPHERE: a red circle a script draws in the frames it wants it (OG Loc's at Freddy's door), C3dMarkers::
// PlaceMarkerSet(id, type 1, position, radius, 255, 0, 0, 228, pulse 2048, 0.1, no rotation) from its command
using PlaceMarkerSetFn = void(__cdecl*)(uint32_t, uint16_t, CVector*, float, uint8_t, uint8_t, uint8_t, uint8_t, uint16_t,
                                        float, int16_t);
// the game's own until redirected: a site another mod changed is left alone, and this is still called
PlaceMarkerSetFn g_origDrawSphere = (PlaceMarkerSetFn)0x725BA0;
void __cdecl HookDrawSphere(uint32_t id, uint16_t type, CVector* pos, float radius, uint8_t r, uint8_t g, uint8_t b,
                            uint8_t a, uint16_t pulse, float pulseFraction, int16_t rotate) {
    if (Net().Online() && Net().IsHost() && pos) {
        if (!g_areas.count(id)) LOGD("Script sphere %08X at %.1f,%.1f,%.1f", id, pos->x, pos->y, pos->z);
        g_areas[id] = {id, pos->x, pos->y, radius, 0.0f, pos->z, NowMs(), AREA_SPHERE};
    }
    g_origDrawSphere(id, type, pos, radius, r, g, b, a, pulse, pulseFraction, rotate);
}

void DrawSphere(uint32_t id, const Area& a) {
    CVector at{a.x1, a.y1, a.z};
    g_origDrawSphere(id, 1, &at, a.x2, 255, 0, 0, 228, 2048, 0.1f, 0);
}

// The radar leaves the mission start points out (their letters and markers) while CTheScripts::IsPlayerOnAMission:
// the partner, on no mission of its own, kept seeing them during the host's mission.
using OnMissionFn = bool(__cdecl*)();
OnMissionFn g_origRadarOnMission = nullptr;
bool __cdecl RadarOnMission() {
    return g_origRadarOnMission() || (Net().Online() && !Net().IsHost() && HostMissionRecent(0));
}

void HandleMarkers(uint8_t from, Reader& r) {
    uint8_t n = r.U8();
    std::vector<Area> areas;
    for (uint8_t i = 0; i < n && r.Ok(); ++i) {
        Area a{};
        a.id = r.U32();
        a.kind = r.U8();
        a.x1 = r.F32();
        a.y1 = r.F32();
        a.x2 = r.F32();
        a.y2 = r.F32();
        a.z = r.F32();
        areas.push_back(a);
    }
    if (!r.Ok() || from != Net().HostId() || Net().IsHost()) return;
    if (areas.size() != g_partnerAreas.size())
        LOGD("Host markers: %zu (spheres %d)", areas.size(),
             (int)std::count_if(areas.begin(), areas.end(), [](const Area& a) { return a.kind == AREA_SPHERE; }));
    g_partnerAreas = std::move(areas);
    g_partnerAreasMs = NowMs();
    static uint64_t loggedMs = 0;
    if (!g_partnerAreas.empty() && g_partnerAreasMs - loggedMs >= 5000) {
        loggedMs = g_partnerAreasMs;
        LOGD("Host markers: %zu, still drawn", g_partnerAreas.size());
    }
}

// Race checkpoints (CREATE_CHECKPOINT: CTheScripts::ScriptCheckpointArray, 20 x {used, id, CCheckpoint*}) and script
// spheres (ADD_SPHERE: CTheScripts::ScriptSphereArray, 16 x 0x18) of the host's scripts: the partner places the same.
constexpr uintptr_t kScriptCheckpoints = 0xA44070, kScriptSpheres = 0xA91268;
struct Marked {  // one record of G_CHECKPOINTS
    uint8_t slot = 0;   // 0..19 checkpoints, 32..47 spheres
    uint16_t type = 0;  // checkpoint type
    CVector pos, dir;
    float size = 0;
    bool operator==(const Marked& o) const {
        return slot == o.slot && type == o.type && (pos - o.pos).Length() < 0.05f && (dir - o.dir).Length() < 0.05f &&
               std::fabs(size - o.size) < 0.01f;
    }
};
std::vector<Marked> g_sentMarked;            // host
std::map<uint8_t, std::pair<Marked, int>> g_placed;  // partner: slot -> what and its script handle

std::vector<Marked> HostMarked() {
    std::vector<Marked> out;
    for (int i = 0; i < 20; ++i) {
        uintptr_t e = kScriptCheckpoints + (uintptr_t)i * 8;
        auto cp = Ref<uintptr_t>(e + 4);  // CCheckpoint*
        if (!Ref<uint8_t>(e) || !cp) continue;
        Marked m;
        m.slot = (uint8_t)i;
        m.type = Ref<uint16_t>(cp);  // the type it was made with, unless the player is in it right now (no matter here)
        if (m.type > 8) continue;
        m.pos = Ref<CVector>(cp + 0x10);
        m.dir = Ref<CVector>(cp + 0x1C);
        m.size = Ref<float>(cp + 0x2C);
        out.push_back(m);
    }
    for (int i = 0; i < 16; ++i) {
        uintptr_t s = kScriptSpheres + (uintptr_t)i * 0x18;
        if (!Ref<uint8_t>(s)) continue;
        Marked m;
        m.slot = (uint8_t)(32 + i);
        m.pos = Ref<CVector>(s + 8);
        m.size = Ref<float>(s + 0x14);
        out.push_back(m);
    }
    return out;
}

void HostSendMarked(bool force) {
    std::vector<Marked> now = Replicating() ? HostMarked() : std::vector<Marked>{};
    if (!force && now == g_sentMarked && NowMs()-g_lastMarkedSent<750) return;
    g_lastMarkedSent=NowMs();
    g_sentMarked = now;
    Writer w(proto::G_CHECKPOINTS);
    w.U8((uint8_t)now.size());
    for (const Marked& m : now) {
        w.U8(m.slot);
        w.U16(m.type);
        w.Vec({m.pos.x, m.pos.y, m.pos.z});
        w.Vec({m.dir.x, m.dir.y, m.dir.z});
        w.F32(m.size);
    }
    Net().Send(proto::CH_RELIABLE, w);
}

void Unplace(int slot, int handle) {
    if (slot >= 32) Cmd(0x03BD).I(handle).Run();  // REMOVE_SPHERE
    else Cmd(0x06D6).I(handle).Run();             // DELETE_CHECKPOINT
}
bool PlacedAlive(int slot,int handle){
    int i=handle&0xffff;bool sphere=slot>=32;
    if(handle<0||i>=(sphere?16:20))return false;
    auto entry=(sphere?kScriptSpheres:kScriptCheckpoints)+(uintptr_t)i*(sphere?0x18:8);
    return Ref<uint8_t>(entry)&&Ref<uint16_t>(entry+2)==(uint16_t)(handle>>16);
}

void ClearPartnerMarked() {
    for (auto& [slot, placed] : g_placed) Unplace(slot, placed.second);
    g_placed.clear();
}

void HandleMarked(uint8_t from, Reader& r) {
    uint8_t n = r.U8();
    std::vector<Marked> now;
    for (uint8_t i = 0; i < n && r.Ok(); ++i) {
        Marked m;
        m.slot = r.U8();
        m.type = r.U16();
        Vec3 p = r.Vec(), d = r.Vec();
        m.pos = {p.x, p.y, p.z};
        m.dir = {d.x, d.y, d.z};
        m.size = r.F32();
        if (m.type <= 8 && (m.slot < 20 || (m.slot >= 32 && m.slot < 48))) now.push_back(m);
    }
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld()) return;
    for (auto it = g_placed.begin(); it != g_placed.end();) {  // gone or changed
        auto same = std::find_if(now.begin(), now.end(), [&](const Marked& m) { return m == it->second.first; });
        if (same != now.end() && PlacedAlive(it->first,it->second.second)) {
            ++it;
            continue;
        }
        if(PlacedAlive(it->first,it->second.second))Unplace(it->first, it->second.second);
        it = g_placed.erase(it);
    }
    for (const Marked& m : now) {
        if (g_placed.count(m.slot)) continue;
        // the engine writes past its arrays when they are full (CTheScripts::AddScriptCheckpoint): a free slot first
        bool sphere = m.slot >= 32, room = false;
        for (int i = 0; i < (sphere ? 16 : 20) && !room; ++i)
            room = !Ref<uint8_t>((sphere ? kScriptSpheres : kScriptCheckpoints) + (uintptr_t)i * (sphere ? 0x18 : 8));
        if (!room) continue;
        int h = -1;
        // the checkpoint keeps the direction to the point it was made to face (CCheckpoints::PlaceMarker normalises
        // point - pos): that point is a step along it
        CVector to = m.pos + m.dir;
        if (m.slot >= 32) Cmd(0x03BC).F(m.pos.x).F(m.pos.y).F(m.pos.z).F(m.size).Out(&h).Run();  // ADD_SPHERE
        else Cmd(0x06D5).I(m.type).F(m.pos.x).F(m.pos.y).F(m.pos.z).F(to.x).F(to.y).F(to.z).F(m.size).Out(&h).Run();  // CREATE_CHECKPOINT
        if (h != -1) g_placed[m.slot] = {m, h};
    }
}

// ---------------------------------------------------------------- cutscenes
using LoadCutsceneFn = void(__cdecl*)(const char*);
LoadCutsceneFn g_origLoadCutscene = nullptr;
VoidFn g_origStartCutscene = nullptr;

uint64_t g_sceneClockReceived=0,g_sceneClockSent=0;float g_sceneClockSeconds=0;uint32_t g_sceneClockSequence=0,g_sceneClockLatest=0,g_sceneMapRevision=0;
void SendCutscene(uint8_t op, const char* name = "", int32_t area = 0, uint8_t to = proto::kServerId) {
    Writer w(proto::G_CUTSCENE);
    w.U8(op);
    w.Str(name, 32);
    w.I32(area);
    w.U8(to);
    w.U32(Net().IsHost() ? g_sceneBarrier.generation : g_sceneToken);
    if(op==CS_LOAD){w.Str(MissionTable(),8);w.U32(StoryWorldRevision());}
    if(op==CS_START||op==CS_CLOCK){w.U32(++g_sceneClockSequence);w.F32(CutsceneRunning()?Ref<float>(0xBC3F08):0.f);}
    Net().Send(proto::CH_RELIABLE, w);
}
// the host's choice of the partner's spot for a shot, and when (the camera's clock) the shot begins
void SendSpot(const scene::Spot& primary, float at, uint8_t to = proto::kServerId) {
    for (auto& [id, peer] : Net().Peers()) {
        scene::Spot s;
        if (to == proto::kServerId || !scene::PlayerSpot(id, s)) {
            s = scene::CompanionSpot(primary, proto::PartnerIndex(id, Net().HostId()));
            scene::SetPlayerSpot(id, s);
        }
        Writer w(proto::G_CUTSCENE);
        w.U8(CS_SPOT);
        w.Str("", 32);
        w.I32(0);
        w.U8(to);
        w.U32(g_sceneBarrier.generation);
        w.U8(id);
        w.F32(at);
        w.Vec({s.ground.x, s.ground.y, s.ground.z});
        w.F32(s.heading);
        w.U8(s.pose);
        w.U8(s.act);
        Net().Send(proto::CH_RELIABLE, w);
    }
}

// The cutscene CJ (model 1) takes its textures by name from the "player" TXD it shares with model 0, and that TXD
// holds whoever was dressed last: the remote player, whenever its look came in. Before a cutscene loads, model 0 is
// built for the story's CJ (the host) from that ped's clothes, with no ped rebuilt.
void DressCutsceneCj(CPlayerPed* cj) {
    auto* clothes = cj->ClothesDesc();
    auto* built = (CPedClothesDesc*)0xBC1C78;  // PlayerClothes: what model 0 was built with last
    MuscleFrom muscle(cj != FindPlayerPed(0) ? clothes : nullptr);  // the host's body on the partner's screen
    Call<0x5A81E0>(0u, clothes, built, false);  // CClothes::ConstructPedModel
    memcpy(built, clothes, sizeof(CPedClothesDesc));
}

// LoadCutsceneData builds the cutscene CJ from its own player (CClothes::RebuildCutscenePlayer, called at 0x4D5EA3):
// in a host's cutscene on the partner's screen it is built from the host's ped.
using CutscenePlayerFn = void(__cdecl*)(CPlayerPed*, int);
CutscenePlayerFn g_origCutscenePlayer = nullptr;
CPlayerPed* g_cutsceneCj = nullptr;
void __cdecl HookRebuildCutscenePlayer(CPlayerPed* ped, int model) {
    if (!g_cutsceneCj) return g_origCutscenePlayer(ped, model);
    // the host's body as its look came: the original writes this game's fat and muscle stats into the description first
    MuscleFrom muscle(g_cutsceneCj->ClothesDesc());
    Call<0x5A81E0>((uint32_t)model, g_cutsceneCj->ClothesDesc(), (CPedClothesDesc*)nullptr, true);  // CClothes::ConstructPedModel
}

void __cdecl HookLoadCutscene(const char* name) {
    if (Net().Online() && Net().IsHost() && PersonalActivity(g_currentScript)) {
        g_sharedScene = false;
        return g_origLoadCutscene(name);
    }
    g_sharedScene = Net().Online() && Net().IsHost();
    if (Net().Online() && Net().IsHost() && name) {
        // a script that clears one cutscene and loads the next in the same frame (the prologue does): the end goes
        // out first, otherwise the partner takes it for the end of the one it is loading
        if (g_hostCutscene) {
            SendCutscene(CS_END);
            g_hostCutscene = false;
            LOGI("Cutscene ended (replicated before the next one)");
        }
        strncpy(g_cutsceneName, name, sizeof(g_cutsceneName) - 1);
        g_sceneBarrier.Begin(NowMs());
        g_startPending = false;
        SendCutscene(CS_LOAD, name);
        LOGI("Cutscene %s loading (replicated)", name);
    }
    if (g_sharedScene) {
        scene::Prepare();  // before the cutscene's own streaming requests
        DressCutsceneCj(FindPlayerPed(0));
    }
    g_origLoadCutscene(name);
}
int32_t& CurrentArea() { return Ref<int32_t>(0xB72914); }  // CGame::currArea: the interior being drawn

void __cdecl HookStartCutscene() {
    if (g_sharedScene) {
        std::vector<uint8_t> peers;
        for (auto& [id, peer] : Net().Peers()) peers.push_back(id);
        if (!g_sceneBarrier.CanStart(NowMs(), peers)) {
            g_startPending = true;
            g_startScript = g_currentScript;
            LOGI("Cutscene waits for %zu partner loaders", peers.size());
            return;
        }
        SendCutscene(CS_START, "", CurrentArea());
    }
    g_origStartCutscene();
}

// ---- the partner in cutscenes (ScenePartner.cpp picks the spot and the animation in each shot);
// on the partner's screen the cutscene CJ wears the host's clothes and the host's own ped is hidden
constexpr uintptr_t kCutsceneObjects = 0xBC3F18, kNumCutsceneObjects = 0xBC3FE4;  // CCutsceneMgr::ms_pCutsceneObjects
bool g_inScene = false, g_sceneHeld = false;
std::string g_lastPassed;      // host: CStats::LastMissionPassedName as last seen
uint64_t g_progressAt = 0;     // host: when the story's progress goes to the partners (0: nothing to send)
int g_csCar = -1, g_csSeat = -1;  // the vehicle (handle) and seat (-1: driver) a cutscene's loading took us out of
uint16_t g_csVehicle=0;
CVector g_scenePos;
float g_sceneHeading = 0;
int8_t g_sceneArea = 0;

CEntity* CutsceneCj() {
    int n = std::clamp(Ref<int32_t>(kNumCutsceneObjects), 0, 50);
    for (int i = 0; i < n; ++i)
        if (CEntity* o = Ref<CEntity*>(kCutsceneObjects + (uintptr_t)i * 4); o && o->ModelIndex() == 1) return o;  // "csplay"
    return nullptr;
}

// Our player waits at the cutscene's place from its start (hidden and frozen): the collision there loads around him
// and stays, so where the scene puts him he stands on the floor instead of falling (a fall cancels the animation).
void EnterCutsceneScene(const CVector& at, int32_t area) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!g_inScene) {
        g_inScene = true;
        g_scenePos = me->GetPosition();
        g_sceneHeading = me->CurrentRotation();
        g_sceneArea = me->AreaCode();
    }
    int h = GetPedRef(me);
    if(me->InVehicle())Cmd(0x0362).I(h).F(me->GetPosition().x).F(me->GetPosition().y).F(me->GetPosition().z-1).Run();
    me->AreaCode() = (int8_t)area;
    Cmd(0x04D7).I(h).I(1).Run();                     // FREEZE_CHAR_POSITION
    Cmd(0x00A1).I(h).F(at.x).F(at.y).F(at.z).Run();  // SET_CHAR_COORDINATES
}

void StandNextToCutsceneCj() {
    scene::Spot spot;
    if (!scene::PlayerSpot(Net().MyId(), spot)) return;
    CPlayerPed* me = FindPlayerPed(0);
    if (CEntity* cj = CutsceneCj()) me->AreaCode() = cj->AreaCode();
    scene::Hold(me, spot);
    for (auto& [id, rp] : Players::Instance().All()) {
        if (id == Net().HostId() || !rp.ped || !scene::PlayerSpot(id, spot)) continue;
        if (CEntity* cj = CutsceneCj()) rp.ped->AreaCode() = cj->AreaCode();
        scene::Hold(rp.ped, spot);
    }
    if (const RemotePlayer* host = Players::Instance().Get(Net().HostId()); host && host->ped) {
        host->ped->Flags() &= ~0x80u;  // the cutscene CJ is the host
        Cmd(0x04D7).I(host->pedHandle).I(1).Run();  // FREEZE_CHAR_POSITION: no collision around it now
    }
}

void LeaveCutsceneScene() {
    if (const RemotePlayer* host = Players::Instance().Get(Net().HostId()); host && host->ped) {
        host->ped->Flags() |= 0x80;
        Cmd(0x04D7).I(host->pedHandle).I(0).Run();  // FREEZE_CHAR_POSITION off
    }
    scene::ReleaseAll();
    if (!g_inScene) return;
    g_inScene = false;
    CPlayerPed* me = FindPlayerPed(0);
    int h = GetPedRef(me);
    Cmd(0x04BB).I(g_sceneArea).Run();      // SET_AREA_VISIBLE
    Cmd(0x0860).I(h).I(g_sceneArea).Run();  // SET_CHAR_AREA_VISIBLE
    Cmd(0x04D7).I(h).I(0).Run();  // FREEZE_CHAR_POSITION off (a scene that ended before its first spot held nobody)
    Cmd(0x00A1).I(h).F(g_scenePos.x).F(g_scenePos.y).F(g_scenePos.z - 1.0f).Run();
    me->CurrentRotation() = me->AimingRotation() = g_sceneHeading;
}

// back into the vehicle the cutscene's loading took us out of, as a mission script does with CJ
void ReturnToCutsceneCar() {
    auto*saved=Vehicles::Instance().Find(g_csVehicle);
    CVehicle* v = saved&&Vehicles::Alive(*saved)&&saved->handle==g_csCar?saved->veh:nullptr;
    int h = GetPedRef(FindPlayerPed(0));
    // only where it was left: a car that moved or sank meanwhile (its ground was not loaded) stays where it is
    bool there = v && (v->GetPosition() - g_scenePos).Length() < 6.0f;
    if (there && v->Health() > 0.0f && !(g_csSeat < 0 ? v->Driver() : v->Passengers()[g_csSeat])) {
        if (g_csSeat < 0) Cmd(0x036A).I(h).I(g_csCar).Run();  // WARP_CHAR_INTO_CAR
        else Cmd(0x0430).I(h).I(g_csCar).I(g_csSeat).Run();    // WARP_CHAR_INTO_CAR_AS_PASSENGER
        LOGI("Back in the vehicle the cutscene took us out of");
    }
    g_csCar = g_csSeat = -1;
    g_csVehicle=0;
}

void PartnerEndCutscene() {
    if (CutsceneRunning()) Call<0x5B1700>();  // CCutsceneMgr::SkipCutscene
    g_csState = 3;
}

// what the mission script does after a cutscene: CLEAR_CUTSCENE resets ms_running
void PartnerClearCutscene() {
    Cmd(0x02EA).Run();  // CLEAR_CUTSCENE
    LeaveCutsceneScene();
    ReturnToCutsceneCar();
    Cmd(0x02EB).Run();            // RESTORE_CAMERA_JUMPCUT
    // the cutscene fades out at its end; in a mission the host's fades follow (G_FADE), otherwise the screen
    // would stay black
    if (!g_partnerMission && FadeAlpha() > 1.0f) Fade(0.5f, 1);
    Cmd(0x01B4).I(0).I(1).Run();  // SET_PLAYER_CONTROL on
    if (g_weWide || TheCameraWide()) SetWideScreen(false);
    g_weWide = false;
    RestoreSceneHud();
    g_csStartRequested = false;
    g_csState = 0;
    LOGI("Host cutscene finished here");
}

// ---- skipping a cutscene takes everybody: the skip button only asks, the host skips once all players said yes
// (the partner's copy of the cutscene ends with the host's)
using SkipButtonFn = bool(__cdecl*)();
SkipButtonFn g_origSkipButton = nullptr;
bool g_skipHeld = false;         // the button was down in the last frame: one press, one vote
bool g_skipOpen = false;         // a vote is on
uint64_t g_skipSince = 0;        // host: when it began
std::set<uint8_t> g_skipYes;     // who said yes (as the host last told)
uint8_t g_skipNo = 0xFF;         // who wants to watch on (shown for a moment)
uint64_t g_skipNoMs = 0;

void SendSkipState() {
    Writer w(proto::G_CUTSCENE);
    w.U8(CS_SKIP_STATE);
    w.Str("", 32);
    w.I32(0);
    w.U8(g_skipOpen);
    w.U8(g_skipNo);
    w.U8((uint8_t)g_skipYes.size());
    for (uint8_t id : g_skipYes) w.U8(id);
    Net().Send(proto::CH_RELIABLE, w);
}

void CloseSkipVote(uint8_t no) {
    if (!g_skipOpen && no == 0xFF) return;
    g_skipOpen = false;
    g_skipYes.clear();
    g_skipNo = no;
    g_skipNoMs = no == 0xFF ? 0 : NowMs();
    if (Net().IsHost()) SendSkipState();
}

// a vote from a player (the host's own too); the host runs the vote
void SkipVote(uint8_t who, bool yes) {
    if (!Net().IsHost()) {
        Writer w(proto::G_CUTSCENE);
        w.U8(CS_SKIP_VOTE);
        w.Str("", 32);
        w.I32(0);
        w.U8(yes);
        Net().Send(proto::CH_RELIABLE, w);
        if (yes) g_skipYes.insert(who);  // shown at once, the host's answer follows
        return;
    }
    if (!CutsceneRunning()) return;
    if (!yes) return CloseSkipVote(who);
    if (!g_skipOpen) g_skipSince = NowMs(), g_skipNo = 0xFF, g_skipNoMs = 0;
    g_skipOpen = true;
    g_skipYes.insert(who);
    LOGI("Cutscene skip: player %u agreed (%zu of %zu)", who, g_skipYes.size(), Net().Peers().size() + 1);
    SendSkipState();
}

// CCutsceneMgr::Update_overlay asks every frame of a cutscene whether to skip it (0x5B1947)
bool __cdecl HookSkipButton() {
    bool down = g_origSkipButton();
    if (!Net().Online()) return down;
    down = down && game::IsForeground();  // the game also says yes for a window in the background
    bool press = down && !g_skipHeld;
    g_skipHeld = down;
    if (press && !g_skipYes.count(Net().MyId())) SkipVote(Net().MyId(), true);
    return false;
}

// a player whose game is in the host's world (a partner still waiting in the menu, say during a new game's prologue,
// sees no cutscene and has no say)
bool InHostWorld(uint8_t id) {
    if (id == Net().MyId()) return true;
    const RemotePlayer* rp = Players::Instance().Get(id);
    return rp && rp->ped;
}

void HostSkipUpdate() {
    if (!g_skipOpen) return;
    if (!CutsceneRunning() || NowMs() - g_skipSince > 15000) return CloseSkipVote(0xFF);  // over, or no answer
    if (!g_skipYes.count(Net().MyId()) || game::cam::Mode() != 17) return;  // MODE_FLYBY: the cutscene plays
    for (auto& [id, peer] : Net().Peers())
        if (InHostWorld(id) && !g_skipYes.count(id)) return;
    LOGI("Cutscene skipped: everybody agreed");
    CloseSkipVote(0xFF);
    Call<0x5B1700>();  // CCutsceneMgr::SkipCutscene
}

void HandleSkip(uint8_t from, uint8_t op, Reader& r) {
    if (op == CS_SKIP_VOTE) {
        bool yes = r.U8() != 0;
        if (r.Ok() && Net().IsHost()) SkipVote(from, yes);
        return;
    }
    bool open = r.U8() != 0;
    uint8_t no = r.U8(), n = r.U8();
    std::set<uint8_t> yes;
    for (uint8_t i = 0; i < n; ++i) yes.insert(r.U8());
    if (!r.Ok() || from != Net().HostId() || Net().IsHost()) return;
    g_skipOpen = open;
    g_skipYes = std::move(yes);
    g_skipNo = no;
    g_skipNoMs = !open && no != 0xFF ? NowMs() : 0;  // a vote closes with the name of who wants to watch on
}


void StopScriptCamera();

void HandleCutscene(uint8_t from, Reader& r) {
    std::vector<uint8_t> packet(r.Cursor(), r.Cursor() + r.Remaining());
    uint8_t op = r.U8();
    std::string name = r.Str(32);
    int32_t area = r.I32();
    if (op == CS_SKIP_VOTE || op == CS_SKIP_STATE) return HandleSkip(from, op, r);
    uint8_t to = r.U8();
    if (to != proto::kServerId && to != Net().MyId()) return;
    uint32_t token = r.U32();
    if (!r.Ok()) return;
    if (op == CS_READY) {
        if (Net().IsHost() && Net().Peer(from)) g_sceneBarrier.Ack(from, token);
        return;
    }
    if (from != Net().HostId() || Net().IsHost()) return;
    if (!core::IsPlayerReady() || flow::GetPhase() == flow::Phase::LoadingWorld) {
        if (g_pendingScenes.size() < 128) g_pendingScenes.push_back(std::move(packet));
        return;
    }
    if(!flow::InHostWorld())return;
    if (op == CS_LOAD) {
        auto table=r.Str(8);auto mapRevision=r.U32();if(!r.Ok())return;g_sceneMapRevision=mapRevision;g_cutsceneTextTable=table;TranslatedTable(table);
        if (token == g_sceneToken && g_csState != 0) return;
        StopScriptCamera();
        g_sceneToken = token;
        g_sceneReadySent = false;
        // the previous one is cleared first, even when its end came in the same frame: otherwise our ped stays
        // where the previous scene put it (an interior's exit marker fires and takes the camera)
        if (g_csState == 2) PartnerEndCutscene();
        if (g_csState != 0) PartnerClearCutscene();
        auto* me = FindPlayerPed(0);
        int type = me->VehicleIfIn() ? me->VehicleIfIn()->VehicleSubClass() : -1;
        g_sceneBypassed = type == VEHICLE_TYPE_HELI || type == VEHICLE_TYPE_PLANE || type == VEHICLE_TYPE_BOAT ||
                          type == VEHICLE_TYPE_FHELI || type == VEHICLE_TYPE_FPLANE;
        if (type < 0 && !me->AttachedTo()) {
            // Standing is unset after a scene or a fresh spawn, even on a
            // loaded floor. Preserve actual flight/swimming, not that bit.
            bool found = false;
            const auto at = me->GetPosition();
            float floor = Call<0x5696C0, float>(at.x, at.y, at.z + .5f, &found, (CEntity**)nullptr);
            bool airborne = found && std::isfinite(floor) && at.z - floor > 2.0f;
            bool closeFloor = found && std::isfinite(floor) && std::fabs(at.z - floor) <= 2.0f;
            bool flying = !closeFloor && me->IsInTheAir() && (me->ActiveWeapon().type == 46 ||
                          std::fabs(me->MoveSpeed().z) > .03f ||
                          (me->Intelligence() && CallMethod<0x601110, void*>(me->Intelligence())));
            g_sceneBypassed = airborne || flying || anim::FindTask(me, 1304);
        }
        if(g_sceneBypassed){SendCutscene(CS_READY,"",0,Net().HostId());LOGI("Host cutscene bypassed: player in air/on water");return;}
        LOGI("Host cutscene %s: loading here", name.c_str());
        // the loading sets us out of a vehicle (CCutsceneMgr::LoadCutsceneData_preload): we get back in afterwards
        if (CPlayerPed* me = FindPlayerPed(0); me->InVehicle() && me->Vehicle() && g_csCar < 0) {
            Vehicles::Instance().RememberUsed(me->Vehicle());
            if(auto*saved=Vehicles::Instance().Find(me->Vehicle()))g_csVehicle=saved->netId;
            g_csCar = GetVehicleRef(me->Vehicle());
            g_csSeat = me->Vehicle()->Driver() == me ? -1 : PassengerSeatOf(me->Vehicle(), me);
            // Detach before native cutscene loading or coordinate changes can
            // move the seated player's vehicle along with the actor.
            auto at=me->GetPosition();
            Cmd(0x0362).I(GetPedRef(me)).F(at.x).F(at.y).F(at.z-1).Run();
        }
        SaveSceneHud();
        scene::Prepare();  // before the cutscene's own streaming requests
        if (const RemotePlayer* host = Players::Instance().Get(Net().HostId()); host && host->ped) {
            DressCutsceneCj(host->ped);  // the story's CJ is the host
            g_cutsceneCj = host->ped;
        }
        g_origLoadCutscene(name.c_str());
        g_cutsceneCj = nullptr;
        g_csState = 1;
        g_csStartRequested = false;
    } else if (token != g_sceneToken) {
        return;
    } else if (op == CS_START||op==CS_CLOCK) {
        auto seq=r.U32();float seconds=r.F32();
        if(!r.Ok()||!std::isfinite(seconds)||seconds<0||seconds>7200||token!=g_sceneToken||(g_sceneClockLatest&&(int32_t)(seq-g_sceneClockLatest)<=0))return;
        g_sceneClockLatest=seq;g_sceneClockSeconds=seconds;g_sceneClockReceived=NowMs();
        if(op==CS_CLOCK)return;
        if(g_sceneBypassed)return;
        g_csStartRequested = true;
        g_csArea = area;
    } else if (op == CS_SPOT) {
        if(g_sceneBypassed)return;
        uint8_t player = r.U8();
        float at = r.F32();
        Vec3 p = r.Vec();
        scene::Spot s;
        s.ground = {p.x, p.y, p.z};
        s.heading = r.F32();
        s.pose = (scene::Pose)std::min<uint8_t>(r.U8(), scene::POSE_WALK);
        s.act = r.U8();
        if (r.Ok() && player < proto::kMaxPlayers && player != Net().HostId()) scene::FollowSpot(s, at, player);
    } else if (op == CS_END) {
        if(g_sceneBypassed){g_sceneBypassed=false;return;}
        LOGI("Host cutscene ended (state %d, running %d)", g_csState, CutsceneRunning());
        if (g_csState == 2) PartnerEndCutscene();
        else if (g_csState == 1) g_csState = 3;
    }
}

void UpdatePartnerCutscene() {
    if (!g_pendingScenes.empty()) {
        auto pending = std::move(g_pendingScenes);
        g_pendingScenes.clear();
        for (auto& packet : pending) {
            Reader r(packet.data(), packet.size());
            HandleCutscene(Net().HostId(), r);
        }
    }
    if(g_csState==2&&CutsceneRunning()&&g_sceneClockReceived&&NowMs()-g_sceneClockReceived<3000){
        float target=g_sceneClockSeconds+(NowMs()-g_sceneClockReceived+std::min<uint32_t>(Net().Rtt()/2,500))/1000.f;
        auto&clock=Ref<float>(0xBC3F08);float error=target-clock;
        if(std::fabs(error)>.12f){
            bool hard=std::fabs(error)>1.f;float corrected=hard?target:clock+std::clamp(error,-.035f,.035f);
            clock=std::max(0.f,corrected);
            auto length=CallMethod<0x50AD90,uint32_t>((void*)addr::TheCamera);
            if(length)CallMethod<0x50C070>((void*)addr::TheCamera,std::clamp(clock*100000.f/length,0.f,100.f));
            for(int i=0;i<std::clamp(Ref<int32_t>(kNumCutsceneObjects),0,50);++i)if(auto*o=Ref<CEntity*>(kCutsceneObjects+i*4);o&&o->RwObject()&&Field<uint8_t>(o->RwObject(),0)==2)
                for(void*a=anim::First(o->RwObject());a;a=anim::Next(a))if(auto*h=anim::Hierarchy(a)){
                    float duration=anim::TotalTime(h);if(duration<=0)continue;float t=clock*anim::Speed(a);
                    anim::SetCurrentTime(a,(anim::Flags(a)&anim::F_LOOPED)?std::fmod(t,duration):std::min(t,std::max(0.f,duration-.001f)));
                }
            // Skip obsolete subtitle starts after a seek; retain the line at
            // the destination instead of replaying every missed line in turn.
            int count=std::clamp(Ref<int32_t>(0xBC3FEC),0,64),next=0;int ms=(int)(clock*1000);
            while(next<count&&Ref<int32_t>(0xBC2EC0+next*4)+Ref<int32_t>(0xBC2DC0+next*4)<ms)++next;
            if(hard)Ref<int32_t>(0xBC3FF0)=next;
            AlignSharedSoundtrack((uint32_t)ms,hard);
            if(hard)LOGI("Cutscene timeline restored at=%.3f error=%.3f",clock,error);
        }
    }
    if (g_csState == 1 && !g_sceneReadySent && Ref<int32_t>(kCutsceneLoadStatus) == 2 && StoryWorldReady(g_sceneMapRevision,Ref<CVector>(kCutsceneOffset))) {
        g_sceneReadySent = true;
        SendCutscene(CS_READY, "", 0, Net().HostId());
    }
    if (g_csState == 1 && g_csStartRequested && Ref<int32_t>(kCutsceneLoadStatus) == 2) {
        CVector o = Ref<CVector>(kCutsceneOffset);
        Cmd(0x01B4).I(0).I(0).Run();                   // SET_PLAYER_CONTROL off
        Cmd(0x04BB).I(g_csArea).Run();                 // SET_AREA_VISIBLE: cutscenes in interiors
        EnterCutsceneScene(o, g_csArea);
        Cmd(0x03CB).F(o.x).F(o.y).F(o.z).Run();        // LOAD_SCENE around the cutscene
        g_origStartCutscene();
        g_csState = 2;
        LOGI("Host cutscene started here");
    } else if (g_csState == 2) {
        StandNextToCutsceneCj();
    } else if (g_csState == 3 && (!CutsceneRunning() || Call<0x5B0570, bool>())) {  // CCutsceneMgr::HasCutsceneFinished
        PartnerClearCutscene();
    }
}

// Story dialogue is streamed separately from subtitles. Each guest waits for its own disk load.
DialogueQueue g_audioSlots[4];
AudioTarget g_hostAudioTarget[4];
std::map<uint32_t, AudioTarget> g_audioTargets[4];
bool g_applyingAudio=false;
bool SharedAudioSlot(uint8_t slot) {
    return !g_applyingAudio && Net().Online() && !Net().IsHost() && slot<4 && !g_audioSlots[slot].clips.empty() &&
        (g_csState!=0||(!PersonalActivity(g_currentScript)&&NowMs()>=g_localActivityUntil));
}
uint32_t g_hostAudioToken[4] = {};
int g_hostAudio[4] = {-1, -1, -1, -1};
bool g_hostAudioPlaying[4] = {};
uint64_t g_hostAudioStamp[4]{};
std::pair<int,uint32_t>SubtitleAudio(){
    int slot=-1;uint64_t latest=0;
    for(int i=0;i<4;++i)if(g_hostAudio[i]>=0&&g_hostAudioStamp[i]>latest&&NowMs()-g_hostAudioStamp[i]<15000&&(!g_hostAudioPlaying[i]||!CallMethod<0x5072C0,bool>((void*)0xB6BC90,(uint8_t)i))){slot=i;latest=g_hostAudioStamp[i];}
    return {slot,slot<0?0:g_hostAudioToken[slot]};
}
bool SubtitleWaiting(int slot,uint32_t token,uint32_t&time){
    if(slot<0||slot>=4||!token)return false;
    const auto&q=g_audioSlots[slot];
    for(const auto&c:q.clips)if(c.token==token){if(!c.started||!c.observedPlaying)return true;auto age=NowMs()-c.startedAt;time=age<time?time-(uint32_t)age:1500;return false;}
    if(q.latest&&(int32_t)(token-q.latest)<=0){time=0;return false;}
    return true;
}
bool DialogueBusy(){for(auto&q:g_audioSlots)if(!q.clips.empty()&&q.clips.front().play)return true;return !g_pendingSubtitles.empty();}
void UpdateSubtitles(){
    auto pending=std::move(g_pendingSubtitles);g_pendingSubtitles.clear();
    for(auto&p:pending)if(NowMs()<p.deadline&&p.run==StoryRunId()&&p.phase==CooperativeStoryPhase()){
        g_replayingSubtitleDeadline=p.deadline;Reader r(p.bytes.data(),p.bytes.size());HandleText(Net().HostId(),r);
    }
    g_replayingSubtitleDeadline=0;
}
std::vector<std::vector<uint8_t>> g_pendingAudio;
using LoadAudioFn = void(__fastcall*)(void*, void*, uint8_t, int32_t);
using SlotAudioFn = void(__fastcall*)(void*, void*, uint8_t);
LoadAudioFn g_origLoadAudio = nullptr;
SlotAudioFn g_origPlayAudio = nullptr, g_origClearAudio = nullptr;

void SendMissionAudio(uint8_t op, uint8_t slot, int event, uint8_t to = proto::kServerId) {
    Writer w(proto::G_GAME_EVENT);
    w.U8(op); w.U8(slot); w.I32(event); w.U32(g_hostAudioToken[slot]); w.U8(to); g_hostAudioTarget[slot].Write(w);
    if(op==2)LOGD("Dialogue send slot %u: placed %d, target %u/%04X",slot,g_hostAudioTarget[slot].placed,g_hostAudioTarget[slot].kind,g_hostAudioTarget[slot].id);
    Net().Send(proto::CH_RELIABLE, w);
    SyncTrace().Push({"dialogue_send",NowMs(),g_hostAudioToken[slot],event,to,slot,op});
}
void AudioReceipt(uint8_t slot,uint32_t token,int event,uint8_t stage){
    SyncTrace().Push({"dialogue_local",NowMs(),token,event,Net().MyId(),slot,stage});
    Writer w(proto::G_AUDIO_RECEIPT);w.U8(slot);w.U32(token);w.I32(event);w.U8(stage);Net().Send(proto::CH_RELIABLE,w);
}
void __fastcall HookLoadAudio(void* audio, void* edx, uint8_t slot, int32_t event) {
    if(Net().Online()&&!Net().IsHost()&&slot<4&&g_csState==0&&PersonalActivity(g_currentScript)){
        g_localActivityUntil=NowMs()+1500;g_localActivityScript=g_currentScript;
        auto&q=g_audioSlots[slot];
        if(!q.clips.empty()){
            if(q.clips.front().started)q.clips.pop_front();
            else {q.clips.front().loaded=false;q.clips.front().observedPlaying=false;}
        }
    }
    if(SharedAudioSlot(slot))return;
    g_origLoadAudio(audio, edx, slot, event);
    if (slot >= 4 || !SharedStoryScript(g_currentScript)) return;
    if (++g_hostAudioToken[slot] == 0) ++g_hostAudioToken[slot];
    g_hostAudio[slot] = event;g_hostAudioStamp[slot]=NowMs();
    g_hostAudioTarget[slot] = {};
    g_hostAudioPlaying[slot] = false;
    if (Replicating()) SendMissionAudio(1, slot, event);
}
void __fastcall HookPlayAudio(void* audio, void* edx, uint8_t slot) {
    if(SharedAudioSlot(slot))return;
    g_origPlayAudio(audio, edx, slot);
    if (slot < 4 && SharedStoryScript(g_currentScript)) {
        g_hostAudioPlaying[slot] = true;g_hostAudioStamp[slot]=NowMs();
        if (Replicating() && g_hostAudio[slot] >= 0) SendMissionAudio(2, slot, g_hostAudio[slot]);
    }
}
void __fastcall HookClearAudio(void* audio, void* edx, uint8_t slot) {
    if(SharedAudioSlot(slot))return;
    g_origClearAudio(audio, edx, slot);
    if (slot < 4 && Net().Online() && Net().IsHost() && (!g_currentScript || SharedStoryScript(g_currentScript))) {
        if (g_hostAudio[slot] >= 0) SendMissionAudio(3, slot, g_hostAudio[slot]);
        g_hostAudio[slot] = -1;
        g_hostAudioPlaying[slot] = false;
    }
}
using AudioPositionFn = void(__fastcall*)(void*,void*,uint8_t,CVector*);
using AudioAttachFn = CVector*(__fastcall*)(void*,void*,uint8_t,CPhysical*);
AudioPositionFn g_originalAudioPosition=nullptr;
AudioAttachFn g_originalAudioAttach=nullptr;
void __fastcall HookAudioPosition(void* audio,void* edx,uint8_t slot,CVector* point) {
    if(SharedAudioSlot(slot))return;
    g_originalAudioPosition(audio,edx,slot,point);
    if(slot<4&&point&&SharedStoryScript(g_currentScript))g_hostAudioTarget[slot]=AudioTarget::Describe(nullptr,*point);
}
CVector* __fastcall HookAudioAttach(void* audio,void* edx,uint8_t slot,CPhysical* entity) {
    if(SharedAudioSlot(slot))return &Field<CVector>(audio,0x330+slot*0x20);
    if(slot<4&&entity&&SharedStoryScript(g_currentScript))g_hostAudioTarget[slot]=AudioTarget::Describe(entity,entity->GetPosition());
    return g_originalAudioAttach(audio,edx,slot,entity);
}
VoidFn g_originalSkipScene=nullptr;
void __cdecl SharedSkipScene(){
    if(Net().Online()&&Net().IsHost()&&g_sharedScene){
        Writer w(proto::G_GAME_EVENT);w.U8(5);for(auto token:g_hostAudioToken)w.U32(token);Net().Send(proto::CH_RELIABLE,w);
        for(auto&playing:g_hostAudioPlaying)playing=false;for(auto&event:g_hostAudio)event=-1;
        LOGI("Shared dialogue cancelled through current tokens after scene skip");
    }
    g_originalSkipScene();
}

void HandleMissionAudio(uint8_t from, Reader& r) {
    std::vector<uint8_t> packet(r.Cursor(), r.Cursor() + r.Remaining());
    uint8_t op=r.U8();
    if(op==5){
        std::array<uint32_t,4>limits;for(auto&t:limits)t=r.U32();
        if(!r.Ok()||from!=Net().HostId()||Net().IsHost())return;
        if(!core::IsPlayerReady()||flow::GetPhase()==flow::Phase::LoadingWorld){g_pendingAudio.push_back(std::move(packet));return;}
        for(uint8_t i=0;i<4;++i){auto&q=g_audioSlots[i];bool loaded=!q.clips.empty()&&q.clips.front().loaded;
            if(q.CancelThrough(limits[i])&&loaded&&NowMs()>=g_localActivityUntil)g_origClearAudio((void*)0xB6BC90,nullptr,i);
            for(auto it=g_audioTargets[i].begin();it!=g_audioTargets[i].end();)it=(int32_t)(it->first-limits[i])<=0?g_audioTargets[i].erase(it):std::next(it);
        }
        return;
    }
    uint8_t slot=r.U8();
    int event = r.I32();
    uint32_t token = r.U32();
    uint8_t to = r.U8();
    AudioTarget target; if (!target.Read(r)) return;
    if (to != proto::kServerId && to != Net().MyId()) return;
    if (!r.Ok() || slot >= 4 || from != Net().HostId() || Net().IsHost()) return;
    if (!core::IsPlayerReady() || flow::GetPhase() == flow::Phase::LoadingWorld) {
        if (g_pendingAudio.size() < 128) g_pendingAudio.push_back(std::move(packet));
        return;
    }
    if(!flow::InHostWorld())return;
    if (event >= 0 && event < 100000) {
        g_audioSlots[slot].Receive(op, event, token, NowMs());
        if(op==2)AudioReceipt(slot,token,event,0);
        if(g_audioTargets[slot].size()>32)g_audioTargets[slot].clear();
        g_audioTargets[slot][token]=target;
    }
}
void UpdateMissionAudio() {
    if(g_csState==0&&NowMs()<g_localActivityUntil)return;
    g_applyingAudio=true;
    auto pending = std::move(g_pendingAudio);
    g_pendingAudio.clear();
    for (auto& packet : pending) {
        Reader r(packet.data(), packet.size());
        HandleMissionAudio(Net().HostId(), r);
    }
    for (uint8_t slot = 0; slot < 4; ++slot) {
        auto& queue = g_audioSlots[slot];
        if (queue.clips.empty()) continue;
        const uint64_t now = NowMs();
        bool finished = CallMethod<0x5072C0, bool>((void*)0xB6BC90, slot);
        auto before=queue.clips.front();
        if (queue.Retire(now, finished)) {
            AudioReceipt(slot,before.token,before.event,before.started&&before.observedPlaying?5:before.clear&&!before.play?6:7);
            g_audioTargets[slot].erase(before.token);
            g_origClearAudio((void*)0xB6BC90, nullptr, slot);
            if (queue.clips.empty()) continue;
        }
        auto& a = queue.clips.front();
        if (!a.loaded) {
            g_origClearAudio((void*)0xB6BC90, nullptr, slot);
            g_origLoadAudio((void*)0xB6BC90, nullptr, slot, a.event);
            a.loaded = true;
            a.loadSince=now;AudioReceipt(slot,a.token,a.event,1);
        }
        // Native slots are 0..3; the SCM query subtracts one from its 1..4
        // operand. Passing a native slot to that opcode tested another slot.
        bool ready=CallMethod<0x5072A0,int8_t>((void*)0xB6BC90,slot)==1;
        if(ready&&!a.readyReported){a.readyReported=true;AudioReceipt(slot,a.token,a.event,2);}
        if(a.started&&a.observedPlaying&&!a.playingReported){a.playingReported=true;AudioReceipt(slot,a.token,a.event,4);}
        if (a.play && !a.started && ready) {
            const auto& target=g_audioTargets[slot][a.token];
            LOGD("Dialogue apply slot %u: placed %d, target %u/%04X, entity %p",slot,target.placed,target.kind,target.id,target.Resolve());
            if(target.placed) {
                if(auto* physical=target.Resolve()) CallMethod<0x507330>((void*)0xB6BC90,slot,physical);
                else { CVector point{target.position.x,target.position.y,target.position.z};CallMethod<0x507300>((void*)0xB6BC90,slot,&point); }
            }
            g_origPlayAudio((void*)0xB6BC90, nullptr, slot);
            a.started = true;
            a.startedAt = now;
            AudioReceipt(slot,a.token,a.event,3);
            LOGI("Host dialogue %d plays in slot %u (token %u)", a.event, slot, a.token);
        }
    }
    g_applyingAudio=false;UpdateSubtitles();
}

// Some story scenes use a fixed SCM camera rather than CCutsceneMgr (car conversations, mission outros).
// Their camera and UI must be replicated as well; subtitles alone leave guests looking at their normal game.
struct ScriptCamera {
    bool active = false;
    int32_t area = 0;
    CVector pos, target;
    float fov = 70;
    uint8_t hud = 1, radar = 0;
    uint16_t controls=0;
    uint64_t recv = 0;
    uint32_t time = 0;
    bool wide = false;
} g_scriptCamera;
bool g_cameraSent = false, g_cameraApplied = false;
bool g_warpHolding=false;
bool g_sharedControlsPending=false;
uint16_t g_lastCameraControls=0xFFFF;
uint64_t g_cameraSentMs = 0;
uint16_t g_cameraControls = 0;
float g_cameraFov = 70;
uint32_t g_cameraLatest = 0;
uint32_t g_cameraFlags[6] = {}, g_cameraMask[6] = {};
uint64_t g_screenReceived = 0, g_blackSince = 0;
bool g_blackExpired = false;
bool SharedSceneOwnsScreen(){return g_cameraApplied||g_csState!=0;}
CVector g_streamedCamera;
uint64_t g_cameraStreamed = 0;

void StopScriptCamera() {
    if (!g_cameraApplied) { g_scriptCamera.active = false; return; }
    g_cameraApplied = false;
    g_scriptCamera.active = false;
    auto* me = FindPlayerPed(0);
    Cmd(0x02EB).Run();  // RESTORE_CAMERA_JUMPCUT
    Cmd(0x04BB).I(me->AreaCode()).Run();
    Cmd(0x04D7).I(GetPedRef(me)).I(g_warpHolding?1:0).Run();
    // SET_PLAYER_CONTROL creates the native safe/camera bits while this shared
    // camera is active. Do not restore those temporary bits from an earlier
    // cutscene over the host's subsequent control-enable command.
    auto sharedMask=(uint16_t)0xA1;
    GetPad(0)->DisablePlayerControls = (g_cameraControls&~sharedMask)|
        ((NowMs()-g_scriptCamera.recv<2000?g_scriptCamera.controls:0)&sharedMask);
    g_sharedControlsPending=(GetPad(0)->DisablePlayerControls&sharedMask)!=0;
    uint32_t* fields[] = {&me->Flags(), &me->PhysicalFlags(), &me->PedFlags(), &me->PedFlags2(), &me->PedFlags3(), &me->PedFlags4()};
    for (size_t i = 0; i < 6; ++i) *fields[i] = (*fields[i] & ~g_cameraMask[i]) | (g_cameraFlags[i] & g_cameraMask[i]);
    if(g_warpHolding)Cmd(0x04D7).I(GetPedRef(me)).I(1).Run();
    cam::Fov() = g_cameraFov;
    SetWideScreen(false);
    RestoreSceneHud();
    RestoreCooperativeRoleCamera();
}
struct StoryWarp {
    bool pending=false,prepared=false,closing=false;uint32_t sequence=0,phase=0,world=0;uint64_t received=0,run=0,loaded=0,warned=0;
    CVector position;float heading=0;int8_t area=0;uint16_t vehicle=0;uint8_t flags=0;
    std::vector<EntityRef>refs;
}g_storyWarp;
uint32_t g_storyWarpSequence=0,g_storyWarpLatest=0;
struct SentStoryWarp { uint64_t run=0; uint32_t sequence=0; };
std::array<SentStoryWarp,proto::kMaxPlayers> g_sentStoryWarps{};
uint64_t g_lastWarpSent=0,g_lastWarpRun=0;CVector g_lastWarpPosition;int8_t g_lastWarpArea=0;
bool g_warpFrozen=false;uint16_t g_warpControls=0;
void ReleaseStoryWarpHold(){
    if(g_warpHolding&&core::IsPlayerReady()){
        auto*me=FindPlayerPed(0);Cmd(0x04D7).I(GetPedRef(me)).I(g_warpFrozen).Run();
        if(me->Health()>0&&!g_cameraApplied&&!CutsceneRunning()){
            auto controls=g_warpControls;
            if(!g_scriptCamera.active&&g_scriptCamera.recv&&NowMs()-g_scriptCamera.recv<2000)controls=(controls&~0xA1u)|(g_scriptCamera.controls&0xA1u);
            GetPad(0)->DisablePlayerControls=controls;
        }
    }
    g_warpHolding=false;SetLocalStoryTransitionPending(false);
}
void SendStoryWarp(CPlayerPed*cj,uint8_t target,bool advance){
    if(!cj||!Net().IsHost()||!Replicating()||!StoryRunId())return;
    auto p=StoryPosition(cj);auto now=NowMs();
    if(target==255&&g_lastWarpRun==StoryRunId()&&g_lastWarpArea==cj->AreaCode()&&(p-g_lastWarpPosition).Length()<.25f&&now-g_lastWarpSent<250)return;
    g_lastWarpSent=now;g_lastWarpRun=StoryRunId();g_lastWarpPosition=p;g_lastWarpArea=cj->AreaCode();
    if(advance){AdvanceCooperativeStoryPhase(g_currentScript,"scripted relocation");PrepareCooperativeRelocation(cj->AreaCode(),p);}
    uint16_t vehicle=0;if(auto*v=cj->VehicleIfIn())if(auto*n=Vehicles::Instance().EnsureNetworked(v))vehicle=n->netId;
    uint8_t flags=0;if(cj->Intelligence()&&CallMethod<0x601110,void*>(cj->Intelligence()))flags|=1;
    if(cj->IsInTheAir()&&cj->ActiveWeapon().type==46)flags|=2;
    Writer w(proto::G_STORY_WARP);w.U64(StoryRunId());w.U32(CooperativeStoryPhase());w.U32(++g_storyWarpSequence);w.U8(target);
    w.Vec({p.x,p.y,p.z});w.F32(cj->CurrentRotation());w.I8(cj->AreaCode());w.Entity(ENTITY_VEHICLE,vehicle);w.U8(flags);
    Net().Send(proto::CH_RELIABLE,w);
    for (const auto& [id, peer] : Net().Peers())
        if (id < g_sentStoryWarps.size() && (target == 255 || target == id)) g_sentStoryWarps[id] = {StoryRunId(), g_storyWarpSequence};
    LOGI("Story group warp %u sent: %.1f %.1f %.1f area=%d phase=%u vehicle=%04X",g_storyWarpSequence,p.x,p.y,p.z,cj->AreaCode(),CooperativeStoryPhase(),vehicle);
}
void HandleStoryWarp(uint8_t from,Reader&r){
    StoryWarp next;next.run=r.U64();next.phase=r.U32();next.sequence=r.U32();auto target=r.U8();auto p=r.Vec();next.heading=r.F32();next.area=r.I8();next.vehicle=r.U16();next.flags=r.U8();
    if(!r.Ok()||(target!=255&&target!=Net().MyId())||from!=Net().HostId()||Net().IsHost()||!next.run||next.run!=StoryRunId()||!next.phase||next.phase<CooperativeStoryPhase()||next.flags>3||!next.sequence||(int32_t)(next.sequence-g_storyWarpLatest)<=0||
       !std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||std::fabs(p.x)>20000||std::fabs(p.y)>20000||std::fabs(p.z)>5000||!std::isfinite(next.heading))return;
    AdoptCooperativeStoryPhase(next.run,next.phase);
    g_storyWarpLatest=next.sequence;next.position={p.x,p.y,p.z};next.pending=true;next.world=Net().WorldEpoch();next.received=NowMs();next.refs=Net().EntityContext();g_storyWarp=std::move(next);SetLocalStoryTransitionPending(true);
}
void SendMissionEnd(uint64_t run, uint32_t phase) {
    Writer w(proto::G_MISSION_END); w.U64(run); w.U32(phase);
    for (const auto& sent : g_sentStoryWarps) w.U32(sent.run == run ? sent.sequence : 0);
    Net().Send(proto::CH_RELIABLE, w);
}
bool StoryWarpFootSpot(const StoryWarp&next,CPlayerPed*me,CVector&out,bool&water){
    water=false;
    if(next.flags){out=next.position+CVector{Net().MyId()==1?2.f:-2.f,0,0};return true;}
    float sign=Net().MyId()%2?-1.0f:1.0f;
    float offsets[][2]={{sign*.35f,-.8f},{-sign*.35f,-.8f},{sign*.7f,0},{0,-1.2f},{0,0}};
    if(next.vehicle){offsets[0][0]=sign*2.8f;offsets[0][1]=0;offsets[1][0]=-sign*2.8f;offsets[1][1]=0;offsets[2][0]=0;offsets[2][1]=-3.5f;}
    for(auto&offset:offsets){
        auto p=next.position;float sn=std::sin(next.heading),cs=std::cos(next.heading);
        p.x+=cs*offset[0]-sn*offset[1];p.y+=sn*offset[0]+cs*offset[1];
        bool found=false;CEntity*floor=nullptr;float z=Call<0x5696C0,float>(p.x,p.y,p.z+.5f,&found,&floor);
        // Recorded vehicle entries may start above the actual interior floor.
        if(!found||!std::isfinite(z)||z>next.position.z+.5f||next.position.z-z>(next.vehicle?16.f:3.f))continue;
        p.z=z+1;
        if(Call<0x569E20,CEntity*>(p.x,p.y,p.z,.28f,(CEntity*)me,true,true,false,true,true,false))continue;
        out=p;return true;
    }
    // A scripted swim entry has no floor near the actor. Keep the ordinary
    // collision requirement above; only an actual native water surface at the
    // requested height can supply this alternative (Da Nang's crash entry).
    if (!next.vehicle && next.area == 0) for (auto& offset : offsets) {
        auto p=next.position;float sn=std::sin(next.heading),cs=std::cos(next.heading);
        p.x+=cs*offset[0]-sn*offset[1];p.y+=sn*offset[0]+cs*offset[1];
        float level=0;
        if (!Call<0x6EB690,bool>(p.x,p.y,p.z,&level,(uint8_t)0,(CVector*)nullptr) ||
            !std::isfinite(level) || std::fabs(next.position.z-level)>3.f) continue;
        if(Call<0x569E20,CEntity*>(p.x,p.y,level+.75f,.28f,(CEntity*)me,true,true,false,true,true,false))continue;
        p.z=level+.15f;out=p;water=true;return true;
    }
    return false; // never release a guest over unloaded or missing floor
}
void ApplyStoryWarp(){
    if(!g_storyWarp.pending)return;
    const auto activeRun = StoryRunId();
    bool current = g_storyWarp.run == activeRun && g_storyWarp.phase >= CooperativeStoryPhase();
    // The inactive role snapshot and the mission-end packet can arrive in the
    // same network drain as its final warp. Keep only the accepted, explicitly
    // acknowledged endpoint; later warps and another run remain inadmissible.
    bool finalEndpoint = !activeRun && g_storyWarp.closing;
    if((!current&&!finalEndpoint)||g_storyWarp.world!=Net().WorldEpoch()||!Net().EntityContextValid(g_storyWarp.refs)){
        LOGI("Obsolete story warp discarded seq=%u",g_storyWarp.sequence);g_storyWarp={};ReleaseStoryWarpHold();return;}
    if(g_csState!=0||CutsceneRunning())return;
    auto*me=FindPlayerPed(0);if(!me||me->Health()<=0)return;auto&next=g_storyWarp;
    auto*ownCar=me->VehicleIfIn();auto*networkCar=ownCar?Vehicles::Instance().Find(ownCar):nullptr;
    bool sameCar=next.vehicle&&networkCar&&networkCar->netId==next.vehicle;
    if (LocalCooperativeRelocationReady(next.vehicle, next.area, next.position)) {
        Cmd(0x04BB).I(next.area).Run(); Cmd(0x0860).I(GetPedRef(me)).I(next.area).Run();
        next.pending = false; ReleaseStoryWarpHold();
        LOGI("Story warp %u role support ready", next.sequence); return;
    }
    if(sameCar){
        if(ownCar->AreaCode()!=next.area||(ownCar->GetPosition()-next.position).Length()>10.f||!LocalCooperativeRoleReady())return;
        Cmd(0x04BB).I(next.area).Run();Cmd(0x0860).I(GetPedRef(me)).I(next.area).Run();
        next.pending=false;ReleaseStoryWarpHold();LOGI("Story warp %u vehicle ready",next.sequence);return;
    }
    if(!g_warpHolding){
        g_warpControls=GetPad(0)->DisablePlayerControls;g_warpFrozen=!g_cameraApplied&&(me->PhysicalFlags()&0x2000u)!=0;g_warpHolding=true;
        if(ownCar){auto old=ownCar->GetPosition();Vehicles::Instance().RememberUsed(ownCar);Cmd(0x0362).I(GetPedRef(me)).F(old.x).F(old.y).F(old.z).Run();}
    }
    Cmd(0x04D7).I(GetPedRef(me)).I(1).Run();GetPad(0)->DisablePlayerControls=1;me->MoveSpeed()={};
    if(!next.prepared||NowMs()-next.loaded>1500){
        Cmd(0x04BB).I(next.area).Run();Cmd(0x0860).I(GetPedRef(me)).I(next.area).Run();
        Cmd(0x04E4).F(next.position.x).F(next.position.y).Run();Cmd(0x03CB).F(next.position.x).F(next.position.y).F(next.position.z).Run();next.loaded=NowMs();next.prepared=true;
    }
    CVector p;bool water=false;if(!StoryWarpFootSpot(next,me,p,water)){
        if(NowMs()-next.warned>3000){LOGI("Story warp blocked seq=%u phase=%u reason=floor area=%d target=%.1f,%.1f,%.1f",next.sequence,next.phase,next.area,next.position.x,next.position.y,next.position.z);next.warned=NowMs();}return;
    }
    if(next.flags&1){if(!IsModelLoaded(370)){RequestModel(370);return;}Cmd(0x07A7).I(GetPedRef(me)).Run();}
    if(next.flags&2){if(!IsModelLoaded(371)){RequestModel(371);return;}Cmd(0x01B2).I(GetPedRef(me)).I(46).I(1).Run();Cmd(0x01B9).I(GetPedRef(me)).I(46).Run();}
    StopScriptCamera();g_localActivityUntil=0;g_localActivityScript=nullptr;
    if(!next.flags)Cmd(0x0792).I(GetPedRef(me)).Run();
    Cmd(0x00A1).I(GetPedRef(me)).F(p.x).F(p.y).F(p.z-1).Run();
    me->MoveSpeed()={};me->CurrentRotation()=me->AimingRotation()=next.heading;
    next.pending=false;ReleaseStoryWarpHold();
    LOGI("Story group warp %u applied: %.1f %.1f %.1f area=%d phase=%u surface=%s",next.sequence,p.x,p.y,p.z,next.area,next.phase,water?"water":"floor");
}

void HostSendScriptCamera() {
    bool controlled = Ref<uint8_t>(addr::TheCamera + 0x2B) == 0;
    if (!controlled) g_sceneScriptCamera = false;
    void* mission = FindMissionScript();
    auto*me=FindPlayerPed(0);
    // An attached camera is an interactive photographic viewpoint (SYN1),
    // not a fixed scene camera. Each participant aims their own weapon.
    bool photographing=me&&me->AttachedTo()&&me->ActiveWeapon().type==43&&!TheCameraWide();
    // An RC pilot owns this chase camera just like a driver owns theirs.
    // Fixed introductions still use a different native mode and remain shared.
    bool remoteView=false;
    if(me&&!GetPad(0)->DisablePlayerControls&&!TheCameraWide()&&cam::Mode()==18&&Cmd(0x0241).I(0).Run()){
        int remote=-1;Cmd(0x0484).I(0).Out(&remote).Run();auto*vehicle=GetVehicle(remote);
        remoteView=vehicle&&IsVehicleValid(vehicle)&&vehicle->Health()>0&&
                   (vehicle->AreaCode()==me->AreaCode()||vehicle->AreaCode()==13);
    }
    bool active = g_sceneScriptCamera && controlled && !photographing && !remoteView && !CutsceneRunning() && (!mission||!PersonalActivity(mission));
    SetCooperativeStoryCamera(active);
    auto controls=GetPad(0)->DisablePlayerControls;
    if (!active && !g_cameraSent&&controls==g_lastCameraControls) return;
    if (active == g_cameraSent && NowMs() - g_cameraSentMs < 50) return;
    Writer w(proto::G_GAME_EVENT);
    w.U8(4); w.U32((uint32_t)NowMs()); w.Bool(active);w.U16(GetPad(0)->DisablePlayerControls);
    if (active) {
        const CVector& p = cam::Source();
        CVector t = p + cam::Front() * 100.0f;
        w.I32(CurrentArea());
        w.Vec({p.x, p.y, p.z}); w.Vec({t.x, t.y, t.z}); w.F32(cam::Fov());
        w.U8(Ref<uint8_t>(0xBAA45D)); w.U8(Ref<uint8_t>(0xBAA3FB)); w.Bool(TheCameraWide());
    }
    Net().Send(active == g_cameraSent ? proto::CH_STATE : proto::CH_RELIABLE, w);
    g_cameraSent = active; g_cameraSentMs = NowMs();g_lastCameraControls=controls;
}
void HandleGameEvent(uint8_t from, Reader& input) {
    Reader r = input;
    if (r.U8() != 4) return HandleMissionAudio(from, input);
    if (from != Net().HostId() || Net().IsHost()) return;
    ScriptCamera c;
    c.time = r.U32();
    c.active = r.Bool();
    c.controls=r.U16();
    if (c.active) {
        c.area = r.I32();
        Vec3 p = r.Vec(), t = r.Vec();
        c.pos = {p.x, p.y, p.z}; c.target = {t.x, t.y, t.z};
        c.fov = std::clamp(r.F32(), 10.0f, 150.0f);
        c.hud = r.U8(); c.radar = r.U8(); c.wide = r.Bool();
    }
    if (!r.Ok() || !core::IsPlayerReady() || g_csState != 0) return;
    if (g_cameraLatest && (int32_t)(c.time - g_cameraLatest) < 0) return;
    g_cameraLatest = c.time;
    c.recv = NowMs(); g_scriptCamera = c;
    if (!c.active) StopScriptCamera();
    if(!c.active&&g_sharedControlsPending&&NowMs()>=g_localActivityUntil&&!g_warpHolding&&g_csState==0){
        GetPad(0)->DisablePlayerControls=(GetPad(0)->DisablePlayerControls&~0xA1u)|(c.controls&0xA1u);
        if(!(c.controls&0xA1u))g_sharedControlsPending=false;
    }
}
void UpdateScriptCamera() {
    if(NowMs()<g_localActivityUntil)return;
    auto& c = g_scriptCamera;
    if (StoryRoleOwnsCamera() || !c.active || NowMs() - c.recv > 2000 || g_csState != 0) return StopScriptCamera();
    if (!g_cameraApplied) {
        SaveSceneHud();
        g_cameraControls = GetPad(0)->DisablePlayerControls;
        auto* me = FindPlayerPed(0);
        uint32_t* fields[] = {&me->Flags(), &me->PhysicalFlags(), &me->PedFlags(), &me->PedFlags2(), &me->PedFlags3(), &me->PedFlags4()};
        for (size_t i = 0; i < 6; ++i) g_cameraFlags[i] = *fields[i];
        g_cameraFov = cam::Fov();
        Cmd(0x04D7).I(GetPedRef(me)).I(1).Run();
        for (size_t i = 0; i < 6; ++i) g_cameraMask[i] = g_cameraFlags[i] ^ *fields[i];
        g_cameraStreamed = 0;
        g_cameraApplied = true;
        LOGI("Host scripted camera started here");
    }
    Cmd(0x04BB).I(c.area).Run();
    if (!g_cameraStreamed || (c.pos - g_streamedCamera).Length() > 40.0f) {
        Cmd(0x03CB).F(c.pos.x).F(c.pos.y).F(c.pos.z).Run();
        g_streamedCamera = c.pos; g_cameraStreamed = NowMs();
    }
    if (NowMs() - g_cameraStreamed > 250) {
        Cmd(0x04E4).F(c.pos.x).F(c.pos.y).Run();
        g_cameraStreamed = NowMs();
    }
    Cmd(0x01B4).I(0).I(0).Run();
    Cmd(0x015F).F(c.pos.x).F(c.pos.y).F(c.pos.z).F(0).F(0).F(0).Run();  // SET_FIXED_CAMERA_POSITION
    Cmd(0x0160).F(c.target.x).F(c.target.y).F(c.target.z).I(2).Run();  // POINT_CAMERA_AT_POINT
    Cmd(0x0922).F(c.fov).F(c.fov).I(1000).I(1).Run();
    cam::Fov() = c.fov;
    SetWideScreen(c.wide);
    Ref<uint8_t>(0xBAA45D) = c.hud;
    Ref<uint8_t>(0xBAA3FB) = c.radar;
}

// ---------------------------------------------------------------- blips
void HostScanBlips(bool force) {
    std::set<uint8_t> present;
    for (int i = 0; i < kNumTraces; ++i) {
        uintptr_t t = Trace(i);
        if (!(Ref<uint8_t>(t + 0x25) & 2)) continue;  // m_bInUse
        uint32_t key = ((uint32_t)Ref<uint16_t>(t + 0x14) << 8) | (uint32_t)i;
        if (g_baseline.count(key)) continue;
        BlipRecord b;
        b.index = (uint8_t)i;
        b.type = (Ref<uint8_t>(t + 0x26) >> 2) & 0xF;
        b.display = Ref<uint8_t>(t + 0x26) & 3;
        b.colour = Ref<uint32_t>(t + 0x00);
        b.size = Ref<uint16_t>(t + 0x1C);
        b.sprite = Ref<uint8_t>(t + 0x24);
        b.flags25 = Ref<uint8_t>(t + 0x25);
        b.pos = Ref<CVector>(t + 0x08);
        int handle = Ref<int32_t>(t + 0x04);
        if (b.type == BLIP_CAR) {
            CVehicle* v = GetVehicle(handle);
            if (!v) continue;
            b.pos = v->GetPosition();
            if (NetVehicle* nv = Vehicles::Instance().Find(v)) b.entKind = 1, b.entId = nv->netId;
        } else if (b.type == BLIP_CHAR) {
            CPed* p = GetPed(handle);
            if (!p) continue;
            b.pos = p->GetPosition();
            if (p == FindPlayerPed(0)) b.entKind = 3, b.entId = Net().MyId();
            else if (RemotePlayer* rp = Players::Instance().FindByPed(p)) b.entKind = 3, b.entId = rp->id;
            else if (NetPed* np = Peds::Instance().Find(p)) b.entKind = 2, b.entId = np->netId;
        } else if (b.type == BLIP_OBJECT) {
            CObject* o = GetObject(handle);
            if (!o) continue;
            b.pos = o->GetPosition();
        } else if (b.type != BLIP_COORD && b.type != 5 && b.type != 6) {
            continue;  // pickups: not replicated yet; airstrips (8): the game's own, for a player in a plane
        }
        present.insert(b.index);
        auto it = g_sentBlips.find(b.index);
        if (!force && it != g_sentBlips.end() && it->second == b) continue;
        g_sentBlips[b.index] = b;
        Writer w(proto::G_BLIP);
        w.U8(BLIP_SET);
        b.Write(w);
        Net().Send(proto::CH_RELIABLE, w);
    }
    for (auto it = g_sentBlips.begin(); it != g_sentBlips.end();) {
        if (present.count(it->first)) {
            ++it;
            continue;
        }
        Writer w(proto::G_BLIP);
        w.U8(BLIP_REMOVE);
        w.U8(it->first);
        Net().Send(proto::CH_RELIABLE, w);
        it = g_sentBlips.erase(it);
    }
}

// a blip handle is its trace's counter << 16 | index; the engine frees an entity's blip with the entity
bool BlipAlive(int handle) {
    int i = handle & 0xFFFF;
    return i < kNumTraces && (Ref<uint8_t>(Trace(i) + 0x25) & 2) && Ref<uint16_t>(Trace(i) + 0x14) == (uint16_t)(handle >> 16);
}

void RemovePartnerBlip(PartnerBlip& pb) {
    if (pb.handle) Cmd(0x0164).I(pb.handle).Run();  // REMOVE_BLIP
    pb.handle = 0;
}

void ClearPartnerBlips() {
    for (auto& [i, pb] : g_blips) RemovePartnerBlip(pb);
    g_blips.clear();
}

void ApplyBlip(const BlipRecord& b) {
    // the entity the blip belongs to, if this game has it
    int ent = -1;
    uint8_t kind = b.entKind;
    if (kind == 1) {
        NetVehicle* nv = Vehicles::Instance().Find(b.entId);
        ent = nv && IsVehicleValid(nv->veh) ? nv->handle : -1;
    } else if (kind == 2) {
        ent = Peds::Instance().HandleOf(b.entId);
    } else if (kind == 3) {
        CPed* p = b.entId == Net().MyId() ? (CPed*)FindPlayerPed(0) : nullptr;
        if (const RemotePlayer* rp = Players::Instance().Get((uint8_t)b.entId); rp && b.entId != Net().MyId()) p = rp->ped;
        ent = p ? GetPedRef(p) : -1;
    }
    if (ent < 0) kind = 0;  // not here (yet): a coordinate blip at its position

    PartnerBlip& pb = g_blips[b.index];
    if (pb.handle && !BlipAlive(pb.handle)) pb.handle = 0;  // went with its entity (a mirror made again)
    if (pb.handle && (pb.entKind != kind || (kind && pb.entId != b.entId))) RemovePartnerBlip(pb);
    if (!pb.handle) {
        int h = 0;
        if (kind == 1) Cmd(0x0186).I(ent).Out(&h).Run();       // ADD_BLIP_FOR_CAR
        else if (kind != 0) Cmd(0x0187).I(ent).Out(&h).Run();  // ADD_BLIP_FOR_CHAR
        else Cmd(0x018A).F(b.pos.x).F(b.pos.y).F(b.pos.z).Out(&h).Run();  // ADD_BLIP_FOR_COORD
        pb.handle = h;
        pb.entKind = kind;
        pb.entId = b.entId;
        if (!h) return;
    }
    // the rest of the look is copied field by field
    uintptr_t t = Trace(pb.handle & 0xFFFF);
    Ref<uint32_t>(t + 0x00) = b.colour;
    Ref<uint16_t>(t + 0x1C) = b.size;
    Ref<uint8_t>(t + 0x24) = b.sprite;
    Ref<uint8_t>(t + 0x25) = (uint8_t)(b.flags25 | 2);  // keep m_bInUse
    Ref<uint8_t>(t + 0x26) = (uint8_t)((Ref<uint8_t>(t + 0x26) & ~3) | (b.display & 3));
    if(kind==0&&(b.type==5||b.type==6))Ref<uint8_t>(t+0x26)=(Ref<uint8_t>(t+0x26)&0xc3)|(b.type<<2);
    if (kind == 0) Ref<CVector>(t + 0x08) = b.pos;
}

bool StoryContact(uintptr_t t){auto sprite=Ref<uint8_t>(t+0x24);return(Ref<uint8_t>(t+0x25)&2)&&((Ref<uint8_t>(t+0x26)>>2)&15)==5&&sprite!=31&&sprite!=32&&sprite!=35;}
bool SameContact(const BlipRecord&a,const BlipRecord&b){return a.sprite==b.sprite&&(a.pos-b.pos).Length()<.75f;}
BlipRecord ContactRecord(int i){auto t=Trace(i);BlipRecord b;b.index=(uint8_t)i;b.type=5;b.pos=Ref<CVector>(t+8);b.colour=Ref<uint32_t>(t);b.size=Ref<uint16_t>(t+0x1C);b.sprite=Ref<uint8_t>(t+0x24);b.flags25=Ref<uint8_t>(t+0x25);b.display=Ref<uint8_t>(t+0x26)&3;return b;}
void HostSendContacts(){
    if(NowMs()-g_contactsSent<500)return;g_contactsSent=NowMs();
    std::vector<BlipRecord> contacts;for(int i=0;i<kNumTraces&&contacts.size()<128;++i)if(StoryContact(Trace(i)))contacts.push_back(ContactRecord(i));
    Writer w(proto::G_STORY_CONTACTS);w.U8((uint8_t)contacts.size());for(auto&b:contacts)b.Write(w);Net().Send(proto::CH_RELIABLE,w);
}
void HandleContacts(uint8_t from,Reader&r){
    auto n=r.U8();if(from!=Net().HostId()||Net().IsHost()||n>128)return;
    std::vector<BlipRecord> contacts;for(int i=0;i<n&&r.Ok();++i){BlipRecord b;b.Read(r);if(b.type==5&&std::isfinite(b.pos.x)&&std::isfinite(b.pos.y)&&std::isfinite(b.pos.z))contacts.push_back(b);}
    if(!r.Ok())return;
    for(const auto&b:contacts)if(g_contactHistory.size()<512&&std::none_of(g_contactHistory.begin(),g_contactHistory.end(),[&](const auto&old){return SameContact(old,b);}))g_contactHistory.push_back(b);
    g_contacts=std::move(contacts);
}
void ApplyContacts(){
    if(!flow::InHostWorld()||NowMs()-g_contactsApplied<250)return;g_contactsApplied=NowMs();
    for(auto it=g_contactCopies.begin();it!=g_contactCopies.end();)if(std::none_of(g_contacts.begin(),g_contacts.end(),[&](const auto&b){return b.index==it->first;})){
        if(BlipAlive(it->second))Cmd(0x0164).I(it->second).Run();it=g_contactCopies.erase(it);
    }else ++it;
    // Only contacts previously confirmed by this host can be retired. Local
    // save/property markers and guest-only side activities retain their own state.
    for(int i=0;i<kNumTraces;++i)if(StoryContact(Trace(i))){auto b=ContactRecord(i);
        bool known=std::any_of(g_contactHistory.begin(),g_contactHistory.end(),[&](const auto&old){return SameContact(old,b);});
        bool current=std::any_of(g_contacts.begin(),g_contacts.end(),[&](const auto&now){return SameContact(now,b);});
        if(known&&!current)Cmd(0x0164).I(i|((int)Ref<uint16_t>(Trace(i)+0x14)<<16)).Run();
    }
    for(const auto&b:g_contacts){int found=-1;for(int i=0;i<kNumTraces;++i)if(StoryContact(Trace(i))&&SameContact(ContactRecord(i),b)){found=i;break;}
        if(found<0){int h=0;Cmd(0x018A).F(b.pos.x).F(b.pos.y).F(b.pos.z).Out(&h).Run();if(!h)continue;g_contactCopies[b.index]=h;found=h&0xffff;}
        auto t=Trace(found);Ref<uint32_t>(t)=b.colour;Ref<CVector>(t+8)=b.pos;Ref<uint16_t>(t+0x1C)=b.size;Ref<uint8_t>(t+0x24)=b.sprite;Ref<uint8_t>(t+0x25)=b.flags25|2;Ref<uint8_t>(t+0x26)=(Ref<uint8_t>(t+0x26)&0xc0)|(5<<2)|(b.display&3);
    }
}

std::map<uint8_t, BlipRecord> g_partnerBlipRecords;  // to retry entity attachment when mirrors appear

void HandleBlip(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    if (from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld()) return;
    if (op == BLIP_SET) {
        BlipRecord b;
        b.Read(r);
        if (!r.Ok()) return;
        g_partnerBlipRecords[b.index] = b;
        ApplyBlip(b);
    } else if (op == BLIP_REMOVE) {
        uint8_t idx = r.U8();
        if (!r.Ok()) return;
        if (auto it = g_blips.find(idx); it != g_blips.end()) {
            RemovePartnerBlip(it->second);
            g_blips.erase(it);
        }
        g_partnerBlipRecords.erase(idx);
    } else if (op == BLIP_CLEAR) {
        ClearPartnerBlips();
        g_partnerBlipRecords.clear();
    }
}

// ---------------------------------------------------------------- screen state, timers

// The host's fades the partner sees are those of its scripts (a fade started while a script runs). Its other
// fades are its own: going through a door, a respawn. Following those darkened the partner's screen at every door of
// the host and made the light pulse.
bool g_scriptBlack = false;  // the latest script fade of the host went to black
float g_scriptFadeSec = 0.5f;

void HostSendScreen(bool active, bool changesOnly=false) {
    bool black = active && g_scriptBlack && (FadeAlpha() > 100.0f || (Fading() && FadeDirection() == 0));  // dark or going dark
    Writer w(proto::G_FADE);
    w.U8((active ? FADE_ACTIVE : 0) | ((TheCameraWide() && (g_sceneScriptCamera || CutsceneRunning())) ? FADE_WIDE : 0) | (black ? FADE_BLACK : 0));
    w.F32(g_scriptFadeSec);w.U32(g_screenGeneration);
    static std::vector<uint8_t> sent;static uint64_t sentMs=0;
    if(changesOnly&&sent==w.Buffer()&&NowMs()-sentMs<2000)return;
    sent=w.Buffer();sentMs=NowMs();
    Net().Send(proto::CH_RELIABLE, w);
}

void ScriptFaded(int16_t dir, float seconds) {
    if (!SharedStoryScript(g_currentScript)) return;
    ++g_screenGeneration;g_scriptBlack = dir == 0;
    g_scriptFadeSec = std::clamp(seconds, 0.1f, 5.0f);
    if (!Replicating()) return;
    HostSendScreen(true);
    LOGD("Script fade %s (%.1f s) sent", g_scriptBlack ? "out" : "in", g_scriptFadeSec);
}

void HandleScreen(uint8_t from, Reader& r) {
    uint8_t flags = r.U8();
    float duration = r.F32();uint32_t generation=r.U32();
    if (!flow::InHostWorld() || !r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady()) return;
    if(g_sceneBypassed||NowMs()<g_localActivityUntil)return;
    if((int32_t)(generation-g_receivedScreenGeneration)<0)return;
    if(generation!=g_receivedScreenGeneration){g_receivedScreenGeneration=generation;g_blackSince=0;g_blackExpired=false;}
    g_screenReceived = NowMs();
    if (!(flags & FADE_ACTIVE)) {
        g_blackSince = 0; g_blackExpired = false;
        if (g_weFaded && Darkening()) Fade(0.5f, 1);
        if (g_weWide && g_csState == 0) SetWideScreen(false);
        g_weFaded = g_weWide = false;
        return;
    }
    bool wide = (flags & FADE_WIDE) != 0;
    if ((wide||g_weWide)&&wide != TheCameraWide() && g_csState == 0) {
        SetWideScreen(wide);
        g_weWide = wide;
    }
    duration = std::clamp(duration, 0.1f, 5.0f);
    if (!(flags & FADE_BLACK)) { g_blackSince = 0; g_blackExpired = false; }
    if ((flags & FADE_BLACK) && !g_blackExpired) {
        if (!g_blackSince) g_blackSince = NowMs();
        if (FadeAlpha() < 250.0f && !(Fading() && FadeDirection() == 0)) {
            Fade(duration, 0);
            LOGD("Host's script fade: out");
        }
        g_weFaded = true;
    } else if (g_weFaded) {
        if (Darkening() && !(Fading() && FadeDirection() == 1)) {
            Fade(duration, 1);
            LOGD("Host's script fade: in");
        } else if (!Darkening()) {
            g_weFaded = false;
        }
    }
}

std::array<int32_t,5>g_partnerTimerValues{};
using TimerFn=void(__fastcall*)(void*,void*);
TimerFn g_originalTimerProcess=nullptr,g_originalTimerDisplay=nullptr;
void TimerWithHostValues(TimerFn original,void*self,void*edx,bool tick){
    if(!g_onscreenSet||Net().IsHost()||!flow::InHostWorld()||NowMs()<g_localActivityUntil){original(self,edx);return;}
    std::array<uint32_t,5>offsets{};offsets[0]=Ref<uint32_t>(kOnscreenTimer);
    for(size_t i=0;i<4;++i)offsets[i+1]=Ref<uint32_t>(kOnscreenTimer+0x40+i*0x44);
    auto values = g_partnerTimerValues;
    std::array<bool,5> personal{};
    for (size_t i = 0; i < values.size(); ++i) personal[i] = CooperativeRCFlightCounter(offsets[i], values[i]);
    ScriptValueOverlay<5>scope((int32_t*)addr::ScriptSpace,(uint32_t)std::max(0,Ref<int32_t>(kMainScriptSize)),offsets,values);
    original(self,edx);
    if(tick){
        auto old = g_partnerTimerValues; scope.Capture(g_partnerTimerValues);
        for (size_t i = 0; i < values.size(); ++i) if (personal[i]) g_partnerTimerValues[i] = old[i];
    }
}
void __fastcall SharedTimerProcess(void*self,void*edx){if(CooperativeStageWaiting())return;TimerWithHostValues(g_originalTimerProcess,self,edx,true);}
void __fastcall SharedTimerDisplay(void*self,void*edx){TimerWithHostValues(g_originalTimerDisplay,self,edx,false);}

void HostSendOnscreen() {
    // the timer block refers to script variables: their values travel with it
    std::vector<uint8_t> block((uint8_t*)kOnscreenTimer, (uint8_t*)kOnscreenTimer + kOnscreenSize);
    bool any = Ref<uint8_t>(kOnscreenTimer + 0x38) != 0;  // clock enabled
    for (int i = 0; i < 4; ++i) any |= Ref<uint8_t>(kOnscreenTimer + 0x40 + i * 0x44 + 0x3E) != 0;
    if (!any && block == g_lastOnscreen) return;  // nothing shown and nothing changed
    Writer w(proto::G_ONSCREEN);
    w.Bytes(block.data(), block.size());
    auto var = [&](uint32_t off) { w.I32(off && off + 4 <= (uint32_t)Ref<int32_t>(kMainScriptSize) ? Ref<int32_t>(addr::ScriptSpace + off) : 0); };
    var(Ref<uint32_t>(kOnscreenTimer + 0x00));  // clock
    for (int i = 0; i < 4; ++i) {
        uintptr_t c = kOnscreenTimer + 0x40 + (uintptr_t)i * 0x44;
        var(Ref<uint32_t>(c + 0x00));
        w.I32(Ref<int32_t>(c + 0x04)); // constant maximum; never dereference as a script offset
    }
    w.Str(MissionTable(), 8);  // the labels are GXT keys, usually of the mission's table
    // nothing shown any more goes once, so reliably: lost, the partner kept the last timer until the mission's end
    Net().Send(any ? proto::CH_STATE : proto::CH_RELIABLE, w);
    g_lastOnscreen = std::move(block);
}

void HandleOnscreen(uint8_t from, Reader& r) {
    std::vector<uint8_t> block = r.Bytes(kOnscreenSize);
    int32_t values[9];
    for (int32_t& v : values) v = r.I32();
    std::string table = r.Str(8);
    if (!flow::InHostWorld() || !r.Ok() || block.size() != kOnscreenSize || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady()) return;
    if(NowMs()<g_localActivityUntil)return;
    UseMissionTable(table);
    uint8_t* b = block.data();
    g_partnerTimerValues[0]=values[0];
    for(size_t i=0;i<4;++i)g_partnerTimerValues[i+1]=values[1+i*2];
    // +4 in a counter is a constant maximum, not another script variable.
    for(size_t i=0;i<5;++i){auto at=i?0x40+(i-1)*0x44:0;auto&off=*(uint32_t*)(b+at);
        auto mainSize=Ref<int32_t>(kMainScriptSize);
        if(off&&(mainSize<4||(off&3)||off>(uint32_t)mainSize-4))off=0;
    }
    memcpy((void*)kOnscreenTimer, b, kOnscreenSize);
    g_onscreenSet = true;
}

void ClearPartnerOnscreen() {
    if (!g_onscreenSet) return;
    // the display flags come back every frame while a variable is set (COnscreenTimer::ProcessForDisplay), and the
    // host's mission clears its counters in its last frame, which never reached us
    CallMethod<0x44CBC0>((void*)kOnscreenTimer);  // COnscreenTimer::Init
    g_onscreenSet = false;
}

// ---------------------------------------------------------------- mission start/end
void HandleMissionStart(uint8_t from, Reader& r) {
    std::string name = r.Str(16);
    if (!flow::InHostWorld() || !r.Ok() || from != Net().HostId() || Net().IsHost()) return;
    if (!g_partnerMission) ui::TheChat().AddSystem(Format(TR("%s начал миссию.", "%s started a mission."), Net().PeerName(from).c_str()));
    g_partnerMission = true;
    LOGI("Host mission started (%s)", name.c_str());
}

void PartnerMissionCleanup() {
    for(uint8_t slot=0;slot<4;++slot){
        auto&q=g_audioSlots[slot];
        if(!q.clips.empty()){
            const auto&a=q.clips.front();AudioReceipt(slot,a.token,a.event,6);
            if(core::IsPlayerReady()&&NowMs()>=g_localActivityUntil&&q.clips.front().loaded)g_origClearAudio((void*)0xB6BC90,nullptr,slot);
        }
        q.Cancel();g_audioTargets[slot].clear();
    }
    g_pendingAudio.clear();
    StopScriptCamera();
    RestoreStoryMissionWeapons();
    ClearPartnerBlips();
    g_partnerBlipRecords.clear();
    g_partnerAreas.clear();
    ClearPartnerMarked();
    ClearPartnerOnscreen();
    if (g_csState == 2) PartnerEndCutscene();
    if (g_csState != 0) PartnerClearCutscene();
    RestoreSceneHud();
    if (g_weFaded && Darkening()) Fade(0.5f, 1);
    if (g_weWide) SetWideScreen(false);
    g_weFaded = g_weWide = false;
}

void HandleMissionEnd(uint8_t from, Reader& r) {
    auto run = r.U64(); auto phase = r.U32();
    std::array<uint32_t,proto::kMaxPlayers> sequences{};
    for (auto& sequence : sequences) sequence = r.U32();
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !flow::InHostWorld() ||
        (run && (!phase || (StoryRunId() && StoryRunId() != run)))) return;
    if (g_storyWarp.pending) {
        auto id = Net().MyId();
        g_storyWarp.closing = run && run == g_storyWarp.run && phase >= g_storyWarp.phase &&
                             id < sequences.size() && sequences[id] == g_storyWarp.sequence &&
                             g_storyWarp.world == Net().WorldEpoch();
        if (!g_storyWarp.closing) { g_storyWarp = {}; ReleaseStoryWarpHold(); }
    }
    if (g_partnerMission) ui::TheChat().AddSystem(TR("Миссия закончилась.", "The mission is over."));
    g_partnerMission = false;
    g_hostWaits=false;g_partnerAreasMs=0;
    g_partnerMissionEndedAt = NowMs();
    PartnerMissionCleanup();
    LOGI("Host mission ended");
}

// ---------------------------------------------------------------- module
void SendWait(bool waiting) {
    Writer w(proto::G_MISSION_WAIT);
    w.U8(waiting);
    if (waiting)
        for (float f : {g_waitArea.x1, g_waitArea.y1, g_waitArea.x2, g_waitArea.y2, g_waitArea.z}) w.F32(f);
    Net().Send(proto::CH_RELIABLE, w);
}

// host: a partner's game (re)started — send the running mission's state again
uint32_t g_missionRevision=0,g_partnerRevision=0;
uint64_t g_missionHeartbeat=0;
void SendMissionState(){
    Writer w(proto::G_MISSION_STATE);w.U32(g_missionRevision);w.Bool(g_active);w.Str(g_active?g_missionName:"",16);
    Net().Send(proto::CH_RELIABLE,w);g_missionHeartbeat=NowMs();
}
void MissionsResync(uint8_t player) {
    if(!Net().IsHost())return;
    ResyncCooperativeStory(player);
    if (g_hostWeaponRun == StoryRunId() && StoryRunId() && core::IsPlayerReady()) {
        auto* cj = FindPlayerPed(0);
        for (int weapon : g_hostMissionWeapons)
            for (int slot = 0; cj && slot < NUM_WEAPON_SLOTS; ++slot)
                if (cj->Weapons()[slot].type == weapon)
                    SendMissionWeapon(WEAPON_GIVE, weapon, cj->Weapons()[slot].ammoTotal,
                                      cj->ActiveWeapon().type == weapon, player);
    }
    if(StoryRunId()&&core::IsPlayerReady()&&!CutsceneRunning())SendStoryWarp(FindPlayerPed(0),player,false);
    SendMissionState();
    if(g_waitSent)SendWait(true);
    if(!g_active&&!CutsceneRunning()&&Ref<int32_t>(kCutsceneLoadStatus)==0)return;
    Writer w(proto::G_MISSION_START);
    w.Str(g_missionName, 16);
    Net().Send(proto::CH_RELIABLE, w);
    HostScanBlips(true);
    ReplayMissionText(player);
    HostSendMarked(true);
    g_lastAreasMsg.clear();
    g_lastOnscreen.clear();
    for (uint8_t slot = 0; slot < 4; ++slot) if (g_hostAudio[slot] >= 0&&(!g_hostAudioPlaying[slot]||!CallMethod<0x5072C0,bool>((void*)0xB6BC90,slot))) {
        SendMissionAudio(1, slot, g_hostAudio[slot], player);
        if (g_hostAudioPlaying[slot]) SendMissionAudio(2, slot, g_hostAudio[slot], player);
    }
    HostSendScreen(true);  // the fade it joins into: a new game starts black until its prologue fades in
    if((CutsceneRunning()||Ref<int32_t>(kCutsceneLoadStatus)!=0)&&g_cutsceneName[0]){
        SendCutscene(CS_LOAD,g_cutsceneName,0,player);
        if(CutsceneRunning()){
            SendCutscene(CS_START,"",CurrentArea(),player);
            if(scene::Spot spot;scene::CurrentSpot(spot))SendSpot(spot,0,player);
        }
    }
}

class Missions : public Module {
public:
    const char* Name() const override { return "Missions"; }
    void CollectSyncAudit(std::vector<SyncAuditEntry>&out)override{
        SyncAuditEntry e;e.kind=AUDIT_MISSION;e.id=1;e.owner=Net().HostId();e.area=13;e.alive=true;
        e.relation=Net().IsHost()?g_active:g_partnerMission;e.look=Net().IsHost()?g_missionRevision:g_partnerRevision;out.push_back(e);
        e.kind=AUDIT_MARKERS;e.relation=0;e.look=2166136261u;
        if(Net().IsHost()){for(auto&m:g_sentMarked){e.look=AuditHash(e.look,m.slot+1);++e.relation;}}
        else for(auto&[slot,p]:g_placed)if(PlacedAlive(slot,p.second)){e.look=AuditHash(e.look,slot+1);++e.relation;}
        out.push_back(e);
    }
    bool RepairSyncEntity(uint8_t kind,uint16_t id)override{
        if(!Net().IsHost()||id!=1)return false;
        if(kind==AUDIT_MARKERS){HostSendMarked(true);return true;}
        if(kind==AUDIT_MISSION){MissionsResync(proto::kServerId);if(g_active){HostSendOnscreen();HostSendScriptCamera();}return true;}
        return false;
    }
    void OnPeerResync(uint8_t id) override { MissionsResync(id); }
    void OnPeerLeave(uint8_t id) override {
        g_skipYes.erase(id);
        if(id==Net().HostId()&&core::IsPlayerReady()){
            PartnerMissionCleanup();g_partnerMission=false;g_pendingScenes.clear();g_pendingAudio.clear();
        }
    }

    void OnInstallHooks() override {
        mem::HookFn(0x5B1700,&SharedSkipScene,&g_originalSkipScene,"CCutsceneMgr::SkipCutscene (dialogue cancellation)");
        mem::HookFn(0x44CD30,&SharedTimerProcess,&g_originalTimerProcess,"COnscreenTimer::Process (isolated host values)");
        mem::HookFn(0x44CC20,&SharedTimerDisplay,&g_originalTimerDisplay,"COnscreenTimer::ProcessForDisplay (isolated host values)");
        Net().SetHandler(proto::G_AUDIO_RECEIPT,[](uint8_t from,Reader&r){
            auto slot=r.U8();auto token=r.U32();auto event=r.I32();auto stage=r.U8();
            if(!r.Ok()||!Net().IsHost()||!Net().Peer(from)||slot>=4||stage>7||!token)return;
            SyncTrace().Push({"dialogue_receipt",NowMs(),token,event,from,slot,stage});
        });
        Net().SetHandler(proto::G_MISSION_STATE,[](uint8_t from,Reader&r){
            auto revision=r.U32();auto active=r.Bool();auto name=r.Str(16);
            if(!r.Ok()||Net().IsHost()||from!=Net().HostId()||!flow::InHostWorld()||!core::IsPlayerReady()||
               (g_partnerRevision&&(int32_t)(revision-g_partnerRevision)<0))return;
            if(!active&&g_partnerMission){g_partnerMission=false;g_partnerMissionEndedAt=NowMs();PartnerMissionCleanup();}
            if(active&&!g_partnerMission){g_partnerMission=true;g_missionName=name;}
            g_partnerRevision=revision;
        });
        mem::HookFn(0x469F00, &HookScriptProcess, &g_origScriptProcess, "CRunningScript::Process");
        mem::HookFn(0x582300,&GymCreateMenu,&g_origCreateMenu,"CMenuSystem::CreateNewMenu (gym opening key)");
        mem::HookFn(0x580750,&GymDeleteMenu,&g_origDeleteMenu,"CMenuSystem::SwitchOffMenu (gym ownership)");
        // Both native execution paths dispatch through this table. CLEO can
        // point several ranges at one dispatcher: wrap the slots separately
        // instead of installing conflicting inline hooks on that shared code.
        g_origCommands100=Ref<CommandsFn>(0x8A6168+1*4);mem::Put(0x8A6168+1*4,&HookCommands100);
        g_origCommands500=Ref<CommandsFn>(0x8A6168+5*4);mem::Put(0x8A6168+5*4,&HookCommands500);
        g_origCommands200=Ref<CommandsFn>(0x8A6168+2*4);mem::Put(0x8A6168+2*4,&HookCommands200);
        InstallStoryObjectCommands();
        mem::RedirectCallFn(0x484361, &HookScriptClearArea, &g_origClearArea);  // CLEAR_AREA (opcode 0395)
        mem::HookFn(0x609590, &HookCanPlayerStartMission, &g_origCanStart, "CPlayerPed::CanPlayerStartMission");
        mem::HookFn(0x464C20, &HookStartNewScript, &g_origStartScript, "CTheScripts::StartNewScript");
        mem::HookFn(0x4648E0, &HookScriptInit, &g_origScriptInit, "CRunningScript::Init (story ownership)");
        mem::HookFn(0x465AA0, &HookScriptShutdown, &g_origScriptShutdown, "CRunningScript::ShutdownThisScript (story ownership)");
        {AddMsgFn previous=nullptr;mem::RedirectCallFn(0x5B187E,&NativeSubtitle,&previous);}
        mem::HookFn(0x69EDC0,&PrivateTextClear,&g_originalClearAll,"CText private table HUD preservation");
        mem::HookFn(0x6A0050,&SharedTextGet,&g_originalTextGet,"CText independent mission translations");
        mem::HookFn(0x69F0B0, &HookMsg, &g_origMsg, "CMessages::AddMessage");
        mem::HookFn(0x69F1E0, &HookMsgQ, &g_origMsgQ, "CMessages::AddMessageJumpQ");
        mem::HookFn(0x69E800,&HookMsgString,&g_origMsgString,"CMessages::AddMessageWithStringQ");
        mem::HookFn(0x69E950,&HookMsgQString,&g_origMsgQString,"CMessages::AddMessageJumpQWithString");
        mem::HookFn(0x69E360, &HookMsgNum, &g_origMsgNum, "CMessages::AddMessageWithNumber");
        mem::HookFn(0x69E4E0, &HookMsgQNum, &g_origMsgQNum, "CMessages::AddMessageJumpQWithNumber");
        mem::HookFn(0x69F2B0, &HookBig, &g_origBig, "CMessages::AddBigMessage");
        mem::HookFn(0x69F370, &HookBigQ, &g_origBigQ, "CMessages::AddBigMessageQ");
        mem::HookFn(0x69E5F0, &HookBigNum, &g_origBigNum, "CMessages::AddBigMessageWithNumber");
        mem::HookFn(0x69E6E0, &HookBigQNum, &g_origBigQNum, "CMessages::AddBigMessageWithNumberQ");
        mem::HookFn(0x588BE0, &HookHelp, &g_origHelp, "CHud::SetHelpMessage");
        mem::HookFn(0x69DCD0, &HookClear, &g_origClear, "CMessages::ClearMessages");
        mem::HookFn(0x69DD30, &HookClearSmall, &g_origClearSmall, "CMessages::ClearSmallMessagesOnly");
        mem::HookFn(0x485E00, &HookHighlight, &g_origHighlight, "CTheScripts::HighlightImportantArea");
        mem::RedirectCallFn(0x4845F8,&HookDrawSphere,&g_origDrawSphere);
        mem::RedirectCallFn(0x586D81,&RadarOnMission,&g_origRadarOnMission);
        mem::RedirectCallFn(0x585C4E,&RadarOnMission,&g_origRadarOnMission);
        mem::HookFn(0x507300, &HookAudioPosition, &g_originalAudioPosition, "CAudioEngine::SetMissionAudioPosition");
        mem::HookFn(0x4EC100, &HookAudioAttach, &g_originalAudioAttach, "CAEScriptAudioEntity::AttachMissionAudioToPhysical");
        mem::HookFn(0x507290, &HookLoadAudio, &g_origLoadAudio, "CAudioEngine::PreloadMissionAudio");
        mem::HookFn(0x5072B0, &HookPlayAudio, &g_origPlayAudio, "CAudioEngine::PlayLoadedMissionAudio");
        mem::HookFn(0x5072F0, &HookClearAudio, &g_origClearAudio, "CAudioEngine::ClearMissionAudio");
        Net().SetHandler(proto::G_GAME_EVENT, &HandleGameEvent);
        mem::HookFn(0x4D5E80, &HookLoadCutscene, &g_origLoadCutscene, "CCutsceneMgr::LoadCutsceneData");
        mem::HookFn(0x5B1460, &HookStartCutscene, &g_origStartCutscene, "CCutsceneMgr::StartCutscene");
        mem::RedirectCallFn(0x4D5EA3, &HookRebuildCutscenePlayer, &g_origCutscenePlayer);  // in LoadCutsceneData
        mem::RedirectCallFn(0x5B1947, &HookSkipButton, &g_origSkipButton);  // the skip check of CCutsceneMgr::Update_overlay
        Net().SetHandler(proto::G_TEXT, &HandleText);
        Net().SetHandler(proto::G_BLIP, &HandleBlip);
        Net().SetHandler(proto::G_MARKER, &HandleMarkers);
        Net().SetHandler(proto::G_MISSION_WAIT, &HandleMissionWait);
        Net().SetHandler(proto::G_CUTSCENE, &HandleCutscene);
        Net().SetHandler(proto::G_FADE, &HandleScreen);
        Net().SetHandler(proto::G_STORY_WARP,&HandleStoryWarp);
        Net().SetHandler(proto::G_ONSCREEN, &HandleOnscreen);
        Net().SetHandler(proto::G_MISSION_START, &HandleMissionStart);
        Net().SetHandler(proto::G_MISSION_END, &HandleMissionEnd);
        Net().SetHandler(proto::G_MISSION_WEAPON, &HandleMissionWeapon);
        Net().SetHandler(proto::G_CHECKPOINTS, &HandleMarked);
        Net().SetHandler(proto::G_STORY_CONTACTS,&HandleContacts);
    }

    void OnPreProcess() override {
        PruneStoryThreads();
        if (core::IsPlayerReady() && Net().Online() && !Net().IsHost()) PlacePartnerMarkers();
    }
    void OnPostProcess() override {
        if (!core::IsPlayerReady()) return;
        HealInheritedFlag();  // in a game of one's own too: such a save is broken there as well
        if (!Net().Online()) return;
        if (Net().IsHost()){HostUpdate();HostSendContacts();}
        else {PartnerUpdate();ApplyContacts();}
    }
    // the partner's spot in a cutscene is chosen with the camera of the frame being drawn
    void OnRender() override {
        if (!CutsceneRunning()) return;
        if (Net().Online() && !Net().IsHost()) scene::Follow();  // the host chooses the spot
        else scene::Analyse();
    }

    void OnSessionStart() override {
        if (!Net().IsHost())
            ui::TheChat().AddSystem(TR("Сюжетные миссии запускает хост, а вы проходите их вместе с ним.",
                                       "The host starts the story missions, and you play them together."));
    }
    void OnSessionEnd() override {
        for(auto&s:g_gymSelections)s={};
        g_schoolFlag=false;
        if(core::IsPlayerReady())for(auto&[id,h]:g_contactCopies)if(BlipAlive(h))Cmd(0x0164).I(h).Run();
        g_contacts.clear();g_contactHistory.clear();g_contactCopies.clear();g_contactsSent=g_contactsApplied=0;
        g_pendingScenes.clear();
        g_pendingAudio.clear();
        for(auto& targets:g_audioTargets)targets.clear();
        if (core::IsPlayerReady()) {  // the host's cutscene, blips, markers and fades stay no longer
            if (g_csState == 2) PartnerEndCutscene();
            if (g_csState != 0) PartnerClearCutscene();
            PartnerMissionCleanup();
        }
        Reset();
    }
    void OnGameStarted() override {
        g_schoolFlag=false;
        Reset();
        if (Net().Online() && !Net().IsHost() && !core::IsOnMission()) {
            // A snapshot may carry the host's transient script flags; no local story thread will undo them here.
            Ref<uint8_t>(0xBAA45D) = 1;
            Ref<uint8_t>(0xBAA3FB) = 0;
        }
        g_inheritedFlagMs=core::IsOnMission()?NowMs():0;
        g_lastPassed.assign((const char*)0xB78A00, strnlen((const char*)0xB78A00, 8));  // what the loaded game had
        g_progressAt = 0;
    }
    void OnGameShutdown() override {
        for(auto&s:g_gymSelections)s={};
        if(Net().Online()&&Net().IsHost()){
            if(g_hostCutscene)SendCutscene(CS_END);
            if(g_active){Writer w(proto::G_BLIP);w.U8(BLIP_CLEAR);Net().Send(proto::CH_RELIABLE,w);SendMissionEnd(0,0);}
            if(g_waitSent)SendWait(false);
        }
        g_cameraApplied = false;
        g_savedHud = false;  // the native world reset restores these flags
        Reset();
    }

private:
    static void Reset() {
        ClearStoryThreads();
        g_contacts.clear();g_contactHistory.clear();g_contactCopies.clear();g_contactsSent=g_contactsApplied=0;
        g_sceneClockReceived=g_sceneClockSent=0;g_sceneClockLatest=g_sceneClockSequence=0;
        g_pendingSubtitles.clear();
        g_sharedTranslations.clear();g_sharedMissionTable.clear();g_cutsceneTextTable.clear();g_translationLanguage=255;
        g_missionPayout=0;g_inheritedFlagMs=0;g_sceneBypassed=false;
        g_missionRevision=g_partnerRevision=0;g_missionHeartbeat=0;g_recentMissionText.clear();
        g_pendingAudio.clear();for(auto&targets:g_audioTargets)targets.clear();
        g_deadMissionPartner=-1;ReleaseStoryWarpHold();g_storyWarp={};g_storyWarpLatest=g_storyWarpSequence=0;g_sentStoryWarps={};g_lastWarpRun=g_lastWarpSent=0;
        for (auto& a : g_audioSlots) a = {};
        for (auto& token : g_hostAudioToken) token = 0;
        for (auto& event : g_hostAudio) event = -1;
        for (auto& playing : g_hostAudioPlaying) playing = false;
        g_personalFlag = g_saveFlag = false;g_localActivityUntil=0;g_localActivityScript=nullptr;g_scriptBlackResetRequested=false;g_screenGeneration=g_receivedScreenGeneration=0;
        g_active = g_hostCutscene = g_sharedScene = false;
        g_baseline.clear();
        g_sentBlips.clear();
        g_areas.clear();
        g_blips.clear();  // the game removes its blips with the world
        g_partnerBlipRecords.clear();
        g_partnerAreas.clear();
        g_placed.clear();  // checkpoints and spheres too
        g_sentMarked.clear();
        g_hostMissionWeapons.clear(); g_hostWeaponRun = 0;
        g_partnerMission = g_weFaded = g_weWide = g_onscreenSet = false;
        g_waitSent = g_hostWaits = false;
        g_waitMs = 0;
        g_scriptCamera = {};
        g_sceneScriptCamera = false; g_cameraLatest = 0;
        g_screenReceived = g_blackSince = 0; g_blackExpired = false;
        g_cameraApplied = g_cameraSent = false;
        g_sharedControlsPending=false;g_lastCameraControls=0xFFFF;
        g_csState = 0;
        g_csStartRequested = g_sceneReadySent = g_startPending = false;
        g_startScript = nullptr;
        g_inScene = false;
        g_csCar = g_csSeat = -1;
        g_csVehicle=0;
        g_skipOpen = false;
        g_skipYes.clear();
        g_skipNoMs = 0;
    }

    static void HostUpdate() {
        if(g_scriptBlackResetRequested){g_scriptBlackResetRequested=false;g_scriptBlack=false;++g_screenGeneration;HostSendScreen(false);}
        if (g_scriptBlack && !Fading() && FadeAlpha() < 1.0f) g_scriptBlack = false;  // clear again, whoever faded it in
        void* script = FindMissionScript();
        bool active = HostOnMission();
        std::string name = script ? std::string(Field<char[8]>(script, 0x08), strnlen(Field<char[8]>(script, 0x08), 8)) : "";
        // the flag up with no mission script for long (a modded build had it so for a whole session): the story's markers
        // stay hidden and the partners count the host as on a mission. Said once in the log.
        static uint64_t flagOnlySince = 0;
        static bool flagOnlyLogged = false;
        if (!active || script) {
            flagOnlySince = 0;
            flagOnlyLogged = false;
        } else if (!flagOnlySince) {
            flagOnlySince = NowMs();
        } else if (!flagOnlyLogged && NowMs() - flagOnlySince > 60000) {
            flagOnlyLogged = true;
            LOGW("The mission flag ($1636) has been up for a minute with no mission script running");
        }
        if (active && !g_active) {
            if(!StoryRunId())BeginCooperativeStory(name);
            ++g_missionRevision;
            // blips that exist now are not the mission's
            g_baseline.clear();
            for (int i = 0; i < kNumTraces; ++i)
                if (Ref<uint8_t>(Trace(i) + 0x25) & 2) g_baseline.insert(((uint32_t)Ref<uint16_t>(Trace(i) + 0x14) << 8) | (uint32_t)i);
            g_sentBlips.clear();
            g_missionName.clear();
            LOGI("Mission started (%s)", name.c_str());
        }
        if (active && name != g_missionName && !name.empty()) {
            g_missionName = name;UpdateCooperativeStoryName(name);
            Writer w(proto::G_MISSION_START);
            w.Str(name, 16);
            Net().Send(proto::CH_RELIABLE, w);
        } else if (active && !g_active) {
            Writer w(proto::G_MISSION_START);
            w.Str("", 16);
            Net().Send(proto::CH_RELIABLE, w);
        }
        if (!active && g_active) {
            const auto endingRun = StoryRunId(); const auto endingPhase = CooperativeStoryPhase();
            // Native cleanup has finished. Confirm only the supplies it kept.
            if (g_hostWeaponRun == StoryRunId()) {
                auto* cj = FindPlayerPed(0);
                for (int weapon : g_hostMissionWeapons) {
                    int ammo = -1;
                    for (int slot = 0; cj && slot < NUM_WEAPON_SLOTS; ++slot)
                        if (cj->Weapons()[slot].type == weapon) ammo = cj->Weapons()[slot].ammoTotal;
                    SendMissionWeapon(WEAPON_FINAL, weapon, ammo, false);
                }
            }
            g_hostMissionWeapons.clear(); g_hostWeaponRun = 0;
            EndCooperativeStory((int32_t)g_missionPayout);
            ClearStoryThreads();
            ++g_missionRevision;g_recentMissionText.clear();
            Vehicles::Instance().ReleaseScriptLocks();
            g_deadMissionPartner=-1;
            Writer w(proto::G_BLIP);
            w.U8(BLIP_CLEAR);
            Net().Send(proto::CH_RELIABLE, w);
            SendMissionEnd(endingRun, endingPhase);
            g_missionPayout=0;
            HostSendScreen(false);
            g_sentMarked.clear();  // the partner clears its copies at the mission's end
            g_scriptBlack = false;
            g_sentBlips.clear();
            g_areas.clear();
            LOGI("Mission ended (%s)", g_missionName.c_str());
            // the story moved on: a mission passed (CStats::LastMissionPassedName). The call that often follows one
            // (the next script) unlocks more, like the gym after Sweet's third: the partners reload the world once,
            // after both, so they wait 30 s for it, or until the next script is over
            std::string passed((const char*)0xB78A00, strnlen((const char*)0xB78A00, 8));
            if (passed != g_lastPassed) {
                g_lastPassed = passed;
                g_progressAt = NowMs() + 30000;
            } else if (g_progressAt) {
                g_progressAt = NowMs() + 1000;
            }
        }
        g_active = active;
        if(NowMs()-g_missionHeartbeat>=1000)SendMissionState();
        if (g_progressAt && !active && NowMs() >= g_progressAt) {
            g_progressAt = 0;
            flow::WorldProgressed();
        }

        // the partner's screen shows where the host waits
        bool waiting = NowMs() - g_waitMs < 1500;
        if (waiting != g_waitSent) {
            g_waitSent = waiting;
            LOGI(waiting ? "Mission marker at %.1f,%.1f: waiting for the partner" : "Mission marker at %.1f,%.1f: not waiting any more",
                 (g_waitArea.x1 + g_waitArea.x2) * 0.5f, (g_waitArea.y1 + g_waitArea.y2) * 0.5f);
            Writer w(proto::G_MISSION_WAIT);
            w.U8(waiting);
            if (waiting)
                for (float f : {g_waitArea.x1, g_waitArea.y1, g_waitArea.x2, g_waitArea.y2, g_waitArea.z}) w.F32(f);
            Net().Send(proto::CH_RELIABLE, w);
        }

        if (g_startPending) {
            std::vector<uint8_t> peers;
            for (auto& [id, peer] : Net().Peers()) peers.push_back(id);
            if (g_sceneBarrier.CanStart(NowMs(), peers)) {
                g_startPending = false;
                LOGI("Cutscene starts after partner loader barrier");
                SendCutscene(CS_START, "", CurrentArea());
                g_origStartCutscene();
            }
        }
        HostSendScriptCamera();
        HostSkipUpdate();
        // cutscene end
        bool cs = CutsceneRunning() && g_sharedScene;
        if(cs&&NowMs()-g_sceneClockSent>=500){SendCutscene(CS_CLOCK);g_sceneClockSent=NowMs();}
        if (g_hostCutscene && !cs) {
            SendCutscene(CS_END);
            // The next scene uses the story's actual CJ position. A guest may
            // still be restoring its old cutscene location, so queue the group
            // relocation after END and apply it after that restoration.
            if(active)SendStoryWarp(FindPlayerPed(0));
            g_sharedScene=false;
            LOGI("Cutscene ended (replicated)");
        }
        g_hostCutscene = cs;
        scene::Spot spot;
        float at;
        if (scene::TakeDecision(spot, at)) SendSpot(spot, at);
        if (cs && scene::CurrentSpot(spot)) {  // the partner joins the scene (and its game takes the same spot)
            CEntity* cj = CutsceneCj();
            for (auto& [id, rp] : Players::Instance().All())
                if (rp.ped && scene::PlayerSpot(id, spot)) {
                    // the scene's interior, not the one its game reports: before its cutscene starts there and after
                    // it ends it is elsewhere, and a ped of another area is not drawn (it vanished from the scene)
                    if (cj) rp.ped->AreaCode() = cj->AreaCode();
                    scene::Hold(rp.ped, spot);
                }
            g_sceneHeld = true;
        } else if (!cs && g_sceneHeld) {
            g_sceneHeld = false;
            scene::ReleaseAll();
        }

        if (!active || Net().Peers().empty()) return;
        uint64_t now = NowMs();
        if (now - g_lastFadeMs >= 200) {
            g_lastFadeMs = now;
            HostSendScreen(true, true);
        }
        if (now - g_lastTick < 250) return;
        g_lastTick = now;
        HostScanBlips(false);
        HostSendOnscreen();
        HostSendMarked(false);
        // destination markers drawn during the last frames
        Writer w(proto::G_MARKER);
        std::vector<Area> live;
        for (auto it = g_areas.begin(); it != g_areas.end();) {
            if (now - it->second.seenMs > 300) {
                it = g_areas.erase(it);
                continue;
            }
            live.push_back(it->second);
            ++it;
        }
        w.U8((uint8_t)std::min<size_t>(live.size(), 32));
        for (size_t i = 0; i < live.size() && i < 32; ++i) {
            const Area& a = live[i];
            w.U32(a.id);
            w.U8(a.kind);
            w.F32(a.x1);
            w.F32(a.y1);
            w.F32(a.x2);
            w.F32(a.y2);
            w.F32(a.z);
        }
        if (w.Buffer() != g_lastAreasMsg || now-g_lastAreasSent>=500) {
            g_lastAreasSent=now;
            g_lastAreasMsg = w.Buffer();
            Net().Send(proto::CH_RELIABLE, w);
        }
    }

    static void PlacePartnerMarkers() {
        // C3dMarkers::Update activates these during the game's process, before rendering.
        if (g_hostWaits) g_origHighlight(g_waitArea.id, g_waitArea.x1, g_waitArea.y1, g_waitArea.x2, g_waitArea.y2, g_waitArea.z);
        if (NowMs() - g_partnerAreasMs < 1000)
            for (const Area& a : g_partnerAreas) {
                if (a.kind == AREA_SPHERE) DrawSphere(a.id ^ 0x40000000u, a);
                else g_origHighlight(a.id ^ 0x40000000u, a.x1, a.y1, a.x2, a.y2, a.z);
            }
    }

    static void PartnerUpdate() {
        UpdatePartnerCutscene();
        static uint64_t withoutScene=0;
        if(!g_skipOpen||CutsceneRunning()||g_skipYes.count(Net().MyId()))withoutScene=0;
        else if(!withoutScene)withoutScene=NowMs();
        else if(NowMs()-withoutScene>3000)SkipVote(Net().MyId(),true);
        ApplyStoryWarp();
        UpdateScriptCamera();
        UpdateMissionAudio();
        if (g_csState == 0 && ((g_blackSince && NowMs() - g_blackSince > 8000) ||
            (g_screenReceived && NowMs() - g_screenReceived > 2500 && g_weFaded))) {
            if (FadeAlpha() > 1.0f) Fade(0.25f, 1);
            g_weFaded = false; g_blackSince = 0; g_blackExpired = true;
        }
        // entity blips whose mirror appeared later
        static uint64_t last = 0;
        if (NowMs() - last > 500) {
            last = NowMs();
            for (auto& [i, b] : g_partnerBlipRecords) {
                auto it = g_blips.find(i);
                bool coordFallback = it != g_blips.end() && it->second.entKind == 0 && b.entKind != 0;
                if (coordFallback || b.entKind == 0) ApplyBlip(b);  // also moves coordinate blips of moving entities
            }
        }
    }
};

Missions g_missions;
struct Reg {
    Reg() { core::RegisterModule(&g_missions); }
} g_reg;
}  // namespace

std::string CutsceneSkipText(std::string& hint) {
    hint.clear();
    if (!Net().Online()) return "";
    uint8_t me = Net().MyId();
    if (!g_skipOpen) {
        bool no = g_skipNo != 0xFF && g_skipNo != me && g_skipNoMs && NowMs() - g_skipNoMs < 3000;
        return no ? Format(TR("%s хочет досмотреть", "%s wants to keep watching"), Net().PeerName(g_skipNo).c_str()) : "";
    }
    std::vector<uint8_t> players{me};
    for (auto& [id, peer] : Net().Peers())
        if (!Net().IsHost() || InHostWorld(id)) players.push_back(id);
    std::string yes, wait;
    int yesCount = 0;
    for (uint8_t id : players) {
        if (id == me) continue;
        std::string& list = g_skipYes.count(id) ? yes : wait;
        list += (list.empty() ? "" : ", ") + Net().PeerName(id);
        yesCount += g_skipYes.count(id) ? 1 : 0;
    }
    if (g_skipYes.count(me)) return wait.empty() ? "" : Format(TR("Ждём ответа %s", "Waiting for %s"), wait.c_str());
    hint = TR("Enter — пропустить, Esc — смотреть дальше", "Enter: skip, Esc: keep watching");
    return Format(yesCount > 1 ? TR("%s хотят пропустить катсцену", "%s want to skip the cutscene")
                               : TR("%s хочет пропустить катсцену", "%s wants to skip the cutscene"),
                  yes.c_str());
}

void CutsceneSkipPress() {
    if (Net().Online() && !g_skipYes.count(Net().MyId())) SkipVote(Net().MyId(), true);
}

bool CutsceneSkipDecline() {
    if (!Net().Online() || !g_skipOpen || g_skipYes.count(Net().MyId())) return false;
    SkipVote(Net().MyId(), false);
    return true;
}

std::string MissionWaitText() {
    if (!Net().Online()) return "";
    if (Net().IsHost()) return g_waitSent ? TR("Ждём всех напарников", "Waiting for all partners") : "";
    return g_hostWaits ? Format(TR("%s ждёт вас у маркера миссии", "%s is waiting for you at the mission marker"),
                                Net().PeerName(Net().HostId()).c_str())
                       : "";
}

bool StoryGroupReady(float x, float y, float z, int8_t area, float radius, float vertical) {
    if (!Net().Online()) return true;
    if (!Net().IsHost() || !core::IsPlayerReady()) return false;
    std::vector<GroupMember> members;
    for (const auto& [id, peer] : Net().Peers()) {
        const auto* rp = Players::Instance().Get(id);
        GroupMember m;
        if (rp) {
            m.position = {rp->state.pos.x, rp->state.pos.y, rp->state.pos.z};
            m.area = rp->state.area; m.alive = rp->state.health > 0 && !(rp->state.flags & OnFootState::F_DEAD);
            m.fresh = rp->hasState && rp->ped && NowMs() - rp->stateRecvMs < 1500;
        }
        members.push_back(m);
    }
    if (GroupReady({x,y,z}, area, radius, members, vertical)) { g_waitMs = 0; return true; }
    g_waitMs = NowMs();
    g_waitArea = {0x40000003u, x-radius, y-radius, x+radius, y+radius, z, NowMs()};
    return false;
}

bool HostReplicating() { return Replicating(); }
bool IsSharedStoryScript(void* script) { return SharedStoryScript(script); }

bool LocalSchoolActivity() {
    if(!core::IsPlayerReady()||!core::IsOnMission())return false;
    if(g_schoolFlag)return true;
    auto*script=FindMissionScript();
    return script&&IsSchoolScriptName(Field<char[8]>(script,0x08));
}

bool LocalStoryPresentationBlocked() {
    return g_personalFlag||LocalSchoolActivity()||NowMs()<g_localActivityUntil||PersonalActivity(g_currentScript);
}

void* CurrentScript() { return g_currentScript; }
void ReleaseStoryScriptCamera(){if(core::IsPlayerReady())StopScriptCamera();}

bool HostMissionRecent(uint64_t graceMs) { return g_partnerMission || NowMs() - g_partnerMissionEndedAt < graceMs; }

}  // namespace coop
