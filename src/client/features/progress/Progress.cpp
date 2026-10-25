#include "shared/api/story-loadout/StoryLoadout.h"
#include "shared/lib/sync/ModelRequests.h"
// Separate progression and looks.
//  * the host progresses in its own save; the partner keeps a profile on the server (money, every stat:
//    fat, muscle, stamina, lung capacity, weapon/driving skills..., clothes with haircut and tattoos,
//    weapons, fighting style, max health/armour and the rewards of side missions: no tiring, fast reload,
//    fireproof, free jail and hospital). It is applied over the host's world after each world load and saved back
//    when it changes, and at once when the host saves its game. A partner without a profile starts from what the
//    world gave him; a new game of the host starts every profile over (the server keeps the old ones aside);
//  * both players broadcast their look (clothes description incl. fat/muscle) and their stats block: the
//    other game rebuilds the remote ped's clothes and uses those stats while processing it;
//  * each player has a look of its own: a partner brings the CJ of its own game into the host's world (the game it
//    played before joining, else its newest save) instead of keeping the host's CJ the world came with.
#include <windows.h>

#include <cstring>
#include <array>
#include <map>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/profile/Profile.h"
#include "shared/api/progress/SchoolProgress.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "entities/session/Flow.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

constexpr uint32_t kProfileMagic = 0x46504353;  // "SCPF"
constexpr uint16_t kProfileVersion = 6;  // 6: personal service-job completion; older profiles remain readable
constexpr uintptr_t kStatsBlock = 0xB79000;  // CStats::StatTypesInt + StatTypesFloat
constexpr size_t kStatsSize = 0x4C8;
// stats the world owns, not the player (CStats::StatTypesInt, id - 120): the profile keeps the values of the world it was
// saved in, and laid over a newer one it took the city back (STAT_CITY_UNLOCKED: wanted stars over the bridges, the
// hospitals, the radio) and the story's accomplished missions, gang strength and territory
constexpr int kWorldStats[][2] = {{146, 148}, {180, 181}, {302, 342}};  // missions in game; no more hurricanes, city; story
constexpr size_t kClothesSize = sizeof(CPedClothesDesc);
constexpr std::array<uint16_t,13> kDrivingGlobals={344,428,420,392,384,376,412,364,404,368,408,400,388};
using DrivingRecords=std::array<int32_t,kDrivingGlobals.size()>;
DrivingRecords g_driving{},g_drivingWorld{};
bool g_drivingReady=false;unsigned g_drivingDepth=0;int32_t g_drivingSkillBefore=0;
DrivingRecords ReadDrivingGlobals(){DrivingRecords result{};for(size_t i=0;i<result.size();++i)result[i]=Ref<int32_t>(addr::ScriptSpace+kDrivingGlobals[i]);return result;}
void PutDrivingGlobals(const DrivingRecords& values){for(size_t i=0;i<values.size();++i)Ref<int32_t>(addr::ScriptSpace+kDrivingGlobals[i])=values[i];}
void ResetDrivingRecords(){g_driving={};g_drivingReady=false;g_drivingDepth=0;}
void WriteDrivingRecords(Writer&w){w.U32(0x4C4F4F48);for(auto value:g_driving)w.I32(value);}
bool ReadDrivingRecords(Reader&r,bool stored){
    DrivingRecords values{};
    if(stored){if(r.U32()!=0x4C4F4F48)return false;for(auto&value:values)value=r.I32();
        for(size_t i=0;i<values.size();++i)if(values[i]<0||values[i]>(i?100:1))return false;}
    if(!r.Ok())return false;g_driving=values;g_drivingReady=true;return true;
}

constexpr std::array<uint16_t,3> kJobGlobals={5948,5952,5956}; // paramedic, vigilante, firefighter
using JobRecords=std::array<uint8_t,kJobGlobals.size()>;
JobRecords ReadJobGlobals(){
    JobRecords values{};
    for(size_t i=0;i<values.size();++i){int value=Ref<int32_t>(addr::ScriptSpace+kJobGlobals[i]);values[i]=value==1||value==2;}
    return values;
}
void PutJobGlobals(const JobRecords&values){
    // 1 is the pending completion notification in the two service scripts;
    // restore their settled value, rather than replaying that notification.
    for(size_t i=0;i<values.size();++i)Ref<int32_t>(addr::ScriptSpace+kJobGlobals[i])=values[i]?(i==1?1:2):0;
}
constexpr std::array<int,6> kJobStats={151,152,153,157,158,159};
struct JobBaseline {
    JobRecords completed{};std::array<int32_t,kJobStats.size()> stats{};
    float maxHealthStat=569.f;uint8_t maxHealth=100,maxArmour=100,fireproof=0;
};
JobBaseline ownJobs;
void CaptureOwnJobs(){
    ownJobs.completed=ReadJobGlobals();
    for(size_t i=0;i<kJobStats.size();++i)ownJobs.stats[i]=Ref<int32_t>(kStatsBlock+(kJobStats[i]-120)*4);
    ownJobs.maxHealthStat=Ref<float>(kStatsBlock+0x380+24*4);
    ownJobs.maxHealth=PlayerInfos()[0].maxHealth;ownJobs.maxArmour=PlayerInfos()[0].maxArmour;
    ownJobs.fireproof=Field<uint8_t>(&PlayerInfos()[0],0x14E);
}
void InitialJobBaseline(bool reset=false){
    // CStats::Init and CPlayerInfo::Clear supply these fresh-game defaults.
    // If the player joined from their own game, retain its earned benefits.
    const auto&initial=reset?JobBaseline{}:ownJobs;
    PutJobGlobals(initial.completed);
    for(size_t i=0;i<kJobStats.size();++i)Ref<int32_t>(kStatsBlock+(kJobStats[i]-120)*4)=initial.stats[i];
    Ref<float>(kStatsBlock+0x380+24*4)=initial.maxHealthStat;
    PlayerInfos()[0].maxHealth=initial.maxHealth;PlayerInfos()[0].maxArmour=initial.maxArmour;
    Field<uint8_t>(&PlayerInfos()[0],0x14E)=initial.fireproof;
}
void WriteJobRecords(Writer&w){w.U32(0x53424F4A);for(auto value:ReadJobGlobals())w.U8(value);}
bool ReadJobRecords(Reader&r,JobRecords&values){
    if(r.U32()!=0x53424F4A)return false;
    for(auto&value:values){value=r.U8();if(value>1)return false;}
    return r.Ok();
}
JobRecords LegacyJobRecords(const std::vector<uint8_t>&stats,uint8_t maxArmour,bool fireproof){
    auto integer=[&](int id){int32_t value=0;memcpy(&value,stats.data()+(id-120)*4,sizeof value);return value;};
    float maxHealth=0;memcpy(&maxHealth,stats.data()+0x380+24*4,sizeof maxHealth);
    // Use only the saved player's own completed level and native benefit.
    // The newly loaded host's SCM flags cannot establish personal completion.
    return {uint8_t(integer(158)>=12&&maxHealth>=1000.f),uint8_t(integer(157)>=12&&maxArmour>=150),uint8_t(integer(159)>=12&&fireproof)};
}

void RebuildPlayer(CPlayerPed* ped, bool keepDescFatMuscle) { Call<0x5A82C0>(ped, keepDescFatMuscle); }  // CClothes::RebuildPlayer

// The partner's look in the host's world is its own, or one it was given (the host's CJ the world came with, on a
// first visit with no CJ of its own to bring, or in a profile older than version 3): that one gives way to its own CJ
// once there is one, unless the partner changed its clothes meanwhile.
bool g_lookOwn = false;
uint32_t g_givenKeys[28] = {};   // the clothes (model and texture keys) of a given look
std::vector<uint8_t> g_ownLook;  // the CJ of the last game of its own this player played (not a host's world)
bool g_foreignGame = false;      // the game was started as a partner: a host's world, kept after leaving it too

bool LookIsOwn() { return g_lookOwn || memcmp(FindPlayerPed(0)->ClothesDesc(), g_givenKeys, sizeof g_givenKeys) != 0; }

// CJ's clothes in a save of this game: block 2 holds the player peds (pool ref, model, ped type, the CPed part with its
// size, then the CPlayerPedDataSaveStructure with its size: 0x84 bytes, the clothes at +8)
bool LookFromSave(const std::wstring& path, std::vector<uint8_t>& look) {
    std::vector<uint8_t> b;
    if (!ReadFileBytes(path, b)) return false;
    auto next = [&](size_t from) {
        for (size_t i = from; i + 5 <= b.size(); ++i)
            if (!memcmp(&b[i], "BLOCK", 5)) return i;
        return b.size();
    };
    size_t p = next(next(next(0) + 5) + 5) + 5;
    auto u32 = [&](size_t at) { return at + 4 <= b.size() ? *(const uint32_t*)&b[at] : 0xFFFFFFFFu; };
    uint32_t peds = u32(p);
    p += 4;
    for (uint32_t i = 0; i < peds && i < 8; ++i) {
        uint32_t type = u32(p + 8), size = u32(p + 12);
        if (size > 0x1000) return false;
        p += 16 + size;
        uint32_t dataSize = u32(p);
        p += 4;
        if (type == PED_TYPE_PLAYER1 && dataSize == 0x84 && p + 8 + kClothesSize <= b.size()) {
            look.assign(&b[p + 8], &b[p + 8] + kClothesSize);
            return true;
        }
        if (dataSize > 0x1000) return false;
        p += dataSize;
    }
    return false;
}

// Puts the player's own CJ on: with its body on a first visit (later the body is that of the profile's stats)
bool WearOwnLook(bool body) {
    std::vector<uint8_t> look = g_ownLook;
    std::wstring newest;
    if (look.empty()) {
        FILETIME newestTime{};
        for (int slot = 1; slot <= 8; ++slot) {
            WIN32_FILE_ATTRIBUTE_DATA a;
            std::wstring path = core::SlotPath(slot);
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) &&
                (newest.empty() || CompareFileTime(&a.ftLastWriteTime, &newestTime) > 0))
                newest = path, newestTime = a.ftLastWriteTime;
        }
        if (newest.empty() || !LookFromSave(newest, look)) return false;
    }
    const auto* desc = (const CPedClothesDesc*)look.data();
    if (body) Ref<float>(0xB793D4) = desc->fatStat, Ref<float>(0xB793DC) = desc->muscleStat;  // CStats: fat, muscle
    CPlayerPed* me = FindPlayerPed(0);
    memcpy(me->ClothesDesc(), look.data(), kClothesSize);
    RebuildPlayer(me, false);  // fat and muscle from the stats
    LOGI("Own look put on (%s): torso %08X, head %08X", newest.empty() ? "the game played before" : WideToUtf8(newest).c_str(),
         desc->textureKeys[0], desc->textureKeys[1]);
    return true;
}

std::vector<uint8_t> CaptureProfile() {
    CPlayerPed* me = FindPlayerPed(0);
    CPlayerInfo& info = PlayerInfos()[0];
    Writer w;
    w.U32(kProfileMagic);
    w.U16(kProfileVersion);
    w.I32(info.money);
    w.Bytes((void*)kStatsBlock, kStatsSize);
    w.Bytes(me->ClothesDesc(), kClothesSize);
    w.U8(NUM_WEAPON_SLOTS);
    for (int i = 0; i < NUM_WEAPON_SLOTS; ++i) {
        const CWeapon& wpn = me->Weapons()[i];
        int type=wpn.type,ammo=wpn.ammoTotal;PersonalStoryWeapon(i,type,ammo);
        w.U8((uint8_t)type);
        w.I32(ammo);
    }
    w.U8(me->FightingStyle());
    w.U8(info.maxHealth);
    w.U8(info.maxArmour);
    const uint8_t* perks = (const uint8_t*)&info + 0x14C;  // m_bDoesNotGetTired, m_bFastReload, m_bFireProof
    w.U8(perks[0]);
    w.U8(perks[1]);
    w.U8(perks[2]);
    w.U8(perks[5]);  // m_bGetOutOfJailFree
    w.U8(perks[6]);  // m_bFreeHealthCare
    w.U8(LookIsOwn());
    WriteStoryRewardHistory(w);
    WriteDrivingRecords(w);
    WriteJobRecords(w);
    return w.Buffer();
}

bool ApplyProfile(const std::vector<uint8_t>& blob, bool& lookOwn) {
    Reader r(blob.data(), blob.size());
    if (r.U32() != kProfileMagic) return false;
    uint16_t version = r.U16();
    if (version < 1 || version > kProfileVersion) return false;
    int32_t money = r.I32();
    std::vector<uint8_t> stats = r.Bytes(kStatsSize);
    std::vector<uint8_t> clothes = r.Bytes(kClothesSize);
    uint8_t n = r.U8();
    struct Slot {
        uint8_t type;
        int32_t ammo;
    } slots[NUM_WEAPON_SLOTS] = {};
    for (int i = 0; i < n && i < NUM_WEAPON_SLOTS; ++i) slots[i] = {r.U8(), r.I32()};
    uint8_t fight = r.U8(), maxHealth = r.U8(), maxArmour = r.U8();
    uint8_t perks[5] = {};
    if (version >= 2)
        for (uint8_t& b : perks) b = r.U8();
    uint8_t own = version >= 3 ? r.U8() : 0;
    if(version>=4&&!ReadStoryRewardHistory(r))return false;
    if(!ReadDrivingRecords(r,version>=5))return false;
    JobRecords jobs{};
    if(version>=6&&!ReadJobRecords(r,jobs))return false;
    if(version<5)LOGI("Legacy profile has no personal driving-school records; initialized separately from host (skills retained)");
    if (!r.Ok() || stats.size() != kStatsSize || clothes.size() != kClothesSize) return false;
    if(version<6){jobs=LegacyJobRecords(stats,maxArmour,version>=2&&perks[2]!=0);LOGI("Legacy personal service-job completion restored from saved benefits");}
    lookOwn = own != 0;

    CPlayerPed* me = FindPlayerPed(0);
    CPlayerInfo& info = PlayerInfos()[0];
    int h = GetPedRef(me);
    info.money = info.displayMoney = money;
    int32_t world[kStatsSize / 4];
    memcpy(world, (void*)kStatsBlock, kStatsSize);
    memcpy((void*)kStatsBlock, stats.data(), kStatsSize);
    for (const auto& range : kWorldStats)
        for (int id = range[0]; id <= range[1]; ++id) Ref<int32_t>(kStatsBlock + (uintptr_t)(id - 120) * 4) = world[id - 120];
    // Progress made / total progress are campaign counters, not personal skills.
    memcpy((void*)(kStatsBlock+0x380),(uint8_t*)world+0x380,2*sizeof(float));
    memcpy(me->ClothesDesc(), clothes.data(), kClothesSize);
    RebuildPlayer(me, false);  // body from the profile's fat/muscle stats
    Cmd(0x048F).I(h).Run();    // REMOVE_ALL_CHAR_WEAPONS
    for (const Slot& s : slots) {
        if (s.type == 0 || s.type > 46) continue;  // eWeaponType ends with the parachute
        int model = -1;
        Cmd(0x0781).I(s.type).Out(&model).Run();  // GET_WEAPONTYPE_MODEL
        if (model > 0) LoadModelNow(model);
        Cmd(0x01B2).I(h).I(s.type).I(std::max(s.ammo, 1)).Run();  // GIVE_WEAPON_TO_CHAR
    }
    Cmd(0x01B9).I(h).I(0).Run();  // fists in hand
    me->FightingStyle() = fight;
    info.maxHealth = maxHealth;
    info.maxArmour = maxArmour;
    if (version >= 2) {  // the host's rewards came with its world: the partner has its own
        uint8_t* own = (uint8_t*)&info + 0x14C;
        own[0] = perks[0], own[1] = perks[1], own[2] = perks[2], own[5] = perks[3], own[6] = perks[4];
    }
    PutJobGlobals(jobs);
    return true;
}

struct RemoteLook {
    std::vector<uint8_t> desc;
    uint16_t model = 0;    // a skin mod's player model (SET_PLAYER_MODEL), 0 = CJ in his clothes
    std::string special;   // a special character's (models 290-299 are filled by name, each game its own)
    int appliedTo = -1;    // handle of the ped it was last rebuilt on: a respawned ped can reuse the old one's address
    uint64_t waitMs = 0;   // a special character asked for here since
};
std::map<uint8_t, RemoteLook> g_looks;

constexpr uint64_t kSpecialWaitMs = 10000;  // a special character that does not come in this long: CJ instead

void HandleLook(uint8_t from, Reader& r) {
    std::vector<uint8_t> desc = r.Bytes(kClothesSize);
    uint16_t model = r.U16();
    std::string special = r.Str(16);
    if (!r.Ok() || desc.size() != kClothesSize || from == Net().MyId()) return;
    RemoteLook& l = g_looks[from];
    if (l.desc != desc || l.model != model || l.special != special) l.appliedTo = -1, l.waitMs = 0;
    l.desc = std::move(desc);
    l.model = model;
    l.special = std::move(special);
}

void HandleStats(uint8_t from, Reader& r) {
    std::vector<uint8_t> stats = r.Bytes(kStatsSize);
    if (!r.Ok() || stats.size() != kStatsSize || from == Net().MyId()) return;
    if (RemotePlayer* rp = Players::Instance().Get(from)) rp->stats = std::move(stats);
}

// The profile's stats go over the world's at once: the game takes the jump for a rise and shows its "Respect -
// Upgraded" help (GXT hash 3B797709), over the cutscene too. That help goes for a few seconds after a profile.
bool ShowsRespectUpgrade() {
    struct Key {
        const char* text;
        uint32_t hash;
    };
    const auto* keys = Ref<Key*>(0xC1B340);   // TheText's main table: sorted by hash
    uint32_t n = Ref<uint32_t>(0xC1B340 + 4), lo = 0, hi = n;
    while (keys && lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (keys[mid].hash == 0x3B797709u) return !strncmp((const char*)0xBAA7A0, keys[mid].text, 40);  // CHud::m_pHelpMessage
        if (keys[mid].hash < 0x3B797709u) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

using GenericSaveFn = bool(__cdecl*)(int);
GenericSaveFn g_origMenuSave = nullptr;
void PartnerSaved();
bool __cdecl HookMenuSave(int file) {
    bool ok = g_origMenuSave(file);
    if (ok && Net().Online() && Net().IsHost()) {
        Net().Send(proto::CH_RELIABLE, Writer(proto::G_HOST_SAVED));
        flow::RequestFreshWorld();  // the server keeps the world as saved
        LOGI("The host saved its game: the partners save their profiles");
    } else if (ok && Net().Online()) {
        PartnerSaved();  // at a save point of his own game: his money, skills and look go to the host's server too
    }
    return ok;
}

class Progress : public Module {
    uint16_t m_sentModel=0xFFFF;
    std::string m_sentSpecial;
public:
    const char* Name() const override { return "Progress"; }

    void OnInstallHooks() override {
        mem::RedirectCallFn(0x619081, &HookMenuSave, &g_origMenuSave);  // C_PcSave::SaveSlot -> GenericSave
        Net().onProfileSaved=[](uint32_t crc){if(Instance().m_applied&&crc==Instance().m_pendingCrc)StoryRewardProfileSaved();};
        Net().onProfile = [](const std::vector<uint8_t>& blob) {
            StoryProfileReady(false);
            g_drivingReady=false;
            Progress& p = Instance();
            // an empty profile after a real one, or during a prologue, in the same session (the first one of a session
            // is the newcomer's): the host began a new game, which this game is not in yet
            if (blob.empty() && p.m_profileReceived && (!p.m_profile.empty() || !flow::InHostWorld())) p.m_resetPending = true;
            p.m_profile = blob;
            p.m_profileReceived = true;
            p.m_applied = false;
        };
        Net().SetHandler(proto::G_PLAYER_LOOK, &HandleLook);
        Net().SetHandler(proto::G_PLAYER_STATS, &HandleStats);
        Net().SetHandler(proto::G_HOST_SAVED, [](uint8_t from, Reader&) {
            if (from != Net().HostId() || Net().IsHost() || !Instance().m_applied || !core::IsPlayerReady()) return;
            Instance().SaveNow(true);
            ui::TheChat().AddSystem(TR("Хост сохранил игру. Ваши деньги, навыки и внешность тоже сохранены.",
                                       "The host saved the game. Your money, skills and look are saved too."));
        });
    }

    void OnSessionEnd() override {
        m_profileReceived = m_applied = m_resetPending = m_reset = false;
        m_profile.clear();
        g_looks.clear();
        ResetDrivingRecords();
        m_jobRecords={};
    }
    void OnPeerJoin(uint8_t) override { ResendAppearance(); }
    void OnPeerResync(uint8_t) override { ResendAppearance(); }
    void OnPeerLeave(uint8_t id) override { g_looks.erase(id); }
    void ResendAppearance() {
        m_sentLook.clear();
        m_sentStats.clear();
        m_lookSentMs = m_statsSentMs = m_lastLookMs = 0;
    }
    void OnGameStarted() override {  // a new world: the partner's profile goes on top again
        m_applied = false;StoryProfileReady(false);
        g_drivingReady=false;
        g_foreignGame = Net().Online() && !Net().IsHost();
        if (m_resetPending && flow::InHostWorld()) m_reset = true, m_resetPending = false;  // the new game's world
    }

    void OnPostProcess() override {
        if (core::IsPlayerReady() && !g_foreignGame) {  // a game of its own: the look it brings along
            const auto* d = (const uint8_t*)FindPlayerPed(0)->ClothesDesc();
            g_ownLook.assign(d, d + kClothesSize);
            CaptureOwnJobs();
        }
        if (!Net().Online() || !core::IsPlayerReady()) return;
        CPlayerPed* me = FindPlayerPed(0);
        uint64_t now = NowMs();

        // only in the host's world: a game of its own (joined from single player, waiting) or an old world during a
        // new game's prologue is no profile, it would have gone to the server as one
        bool hostWorld = flow::InHostWorld();
        if (!Net().IsHost() && hostWorld && m_profileReceived && !m_applied && !m_resetPending) {
            m_applied = true;
            bool own = false;
            if (m_profile.empty()) {
                InitialJobBaseline(m_reset);
                g_driving={};g_drivingReady=true;
                ui::TheChat().AddSystem(m_reset ? TR("Хост начал новую игру. Деньги, оружие и навыки у вас тоже начинаются заново.",
                                                     "The host started a new game. Your money, weapons and skills start over too.")
                                                : TR("Вы здесь впервые. Дальше деньги, навыки и внешность у вас будут свои.",
                                                     "You are here for the first time. From now on your money, skills and look "
                                                     "are your own."));
                m_reset = false;
            } else if (ApplyProfile(m_profile, own)) {
                LOGI("Profile applied (%zu bytes)", m_profile.size());
                m_quietUntil = now + 5000;
                ui::TheChat().AddSystem(TR("Загружены ваши деньги, навыки и внешность.", "Your money, skills and look are loaded."));
            } else {
                LOGW("Profile rejected (bad format), starting from the world's state");
                InitialJobBaseline();
                g_driving={};g_drivingReady=true;
            }
            g_lookOwn = own || WearOwnLook(m_profile.empty());
            if (!g_lookOwn) memcpy(g_givenKeys, me->ClothesDesc(), sizeof g_givenKeys);
            StoryProfileReady(true);
            m_saved = m_profile;
            m_lastSaveMs = now;
            m_jobRecords=ReadJobGlobals();
        }
        if(!Net().IsHost()&&hostWorld&&m_applied){
            auto jobs=ReadJobGlobals();
            if(jobs!=m_jobRecords){m_jobRecords=jobs;SaveNow(true);LOGI("Personal service-job completion saved: ambulance=%u vigilante=%u firefighter=%u",jobs[0],jobs[1],jobs[2]);}
        }

        if (m_quietUntil && Ref<uint8_t>(0xB5F851)) m_quietUntil = std::max(m_quietUntil, now + 3000);  // a cutscene: later
        if (now < m_quietUntil && ShowsRespectUpgrade()) {
            Cmd(0x03E6).Run();  // CLEAR_HELP
            m_quietUntil = 0;
            LOGI("The respect help the profile caused is cleared");
        }

        // look and stats for the other game: on change, and every 10 s for late joiners
        if (now - m_lastLookMs >= 1000) {
            m_lastLookMs = now;
            std::vector<uint8_t> desc((uint8_t*)me->ClothesDesc(), (uint8_t*)me->ClothesDesc() + kClothesSize);
            uint16_t model=me->ModelIndex();
            std::string special=model>=290&&model<=299?Peds::Instance().SpecialName(model):std::string();
            if (desc != m_sentLook || model!=m_sentModel || special!=m_sentSpecial || now - m_lookSentMs > 10000) {
                Writer w(proto::G_PLAYER_LOOK);
                w.Bytes(desc.data(), desc.size());
                w.U16(model);w.Str(special,16);
                Net().Send(proto::CH_RELIABLE, w);
                m_sentLook = std::move(desc);
                m_sentModel=model;m_sentSpecial=special;
                m_lookSentMs = now;
            }
            std::vector<uint8_t> stats((uint8_t*)kStatsBlock, (uint8_t*)kStatsBlock + kStatsSize);
            if ((stats != m_sentStats && now - m_statsSentMs > 3000) || now - m_statsSentMs > 10000) {
                Writer w(proto::G_PLAYER_STATS);
                w.Bytes(stats.data(), stats.size());
                Net().Send(proto::CH_RELIABLE, w);
                m_sentStats = std::move(stats);
                m_statsSentMs = now;
            }
        }

        int lookBudget=1; // native clothing caches are shared across player slots
        // remote looks: rebuild the remote ped once per new description (on foot: the rebuild swaps its clump)
        for (auto& [id, l] : g_looks) {
            RemotePlayer* rp = Players::Instance().Get(id);
            if (!rp || !rp->ped || (rp->pedHandle == l.appliedTo && !rp->lookStale) || rp->ped->InVehicle()) continue;
            if(!lookBudget||now-rp->spawnedAt<500||!rp->ped->RwObject()||!rp->ped->ClothesDesc())continue;
            int model = l.model;
            if (model >= 290 && model <= 299) {  // here it is in whichever slot holds that name
                model = Peds::Instance().SpecialModelFor(l.special);
                if (model < 0) {
                    if (!l.waitMs) l.waitMs = now;
                    if (now - l.waitMs < kSpecialWaitMs) continue;  // still coming in
                    model = 0;
                }
            }
            if(model!=0&&!IsPedModel(model))model=0;
            if(model!=0&&!IsModelLoaded(model)){RequestSharedModel(model,true);continue;}
            bool skin = model != 0 && Players::SetModel(rp->ped, model);
            if(model!=0&&!skin)continue;
            if (!skin) {
                Players::SetModel(rp->ped, 0);
                memcpy(rp->ped->ClothesDesc(), l.desc.data(), kClothesSize);
                MuscleFrom muscle(rp->ped->ClothesDesc());
                RebuildPlayer(rp->ped, true);  // fat/muscle from the description, not from our stats
            }
            --lookBudget;l.appliedTo = rp->pedHandle;
            rp->lookStale = false;
            LOGI("Look of player %d applied (model %d%s%s)", id, skin ? model : 0, l.special.empty() ? "" : ", ",
                 l.special.c_str());
        }

        // the partner's profile goes to the server when it changed (checked every 30 s)
        if (now - m_lastSaveMs >= 30000 && hostWorld) SaveNow();
    }

    void SaveNow(bool always = false) {
        if (!Net().Online() || Net().IsHost() || !m_applied || !core::IsPlayerReady() || !flow::InHostWorld()) return;
        m_lastSaveMs = NowMs();
        std::vector<uint8_t> blob = CaptureProfile();
        if (blob == m_saved && !always) return;
        m_pendingCrc=Crc32(blob.data(),blob.size());
        Writer w(proto::C_PROFILE_SAVE);
        w.Bytes(blob.data(), blob.size());
        Net().Send(proto::CH_RELIABLE, w);
        m_saved = std::move(blob);
        m_profile = m_saved;  // a reload of the world re-applies the latest state
        LOGI("Profile saved (%zu bytes)", m_saved.size());
    }

    static Progress& Instance() {
        static Progress p;
        return p;
    }

private:
    std::vector<uint8_t> m_profile, m_saved, m_sentLook, m_sentStats;
    JobRecords m_jobRecords{};
    uint32_t m_pendingCrc=0;
    bool m_profileReceived = false, m_applied = false;
    bool m_resetPending = false, m_reset = false;  // a new game emptied the profile: applied in the next world
    uint64_t m_lastSaveMs = 0, m_lastLookMs = 0, m_lookSentMs = 0, m_statsSentMs = 0, m_quietUntil = 0;
};

void PartnerSaved() {
    if (!flow::InHostWorld()) return;
    Progress::Instance().SaveNow(true);
    ui::TheChat().AddSystem(TR("Ваши деньги, навыки и внешность сохранены и у хоста.",
                               "Your money, skills and look are saved on the host's side too."));
}

struct Reg {
    Reg() { core::RegisterModule(&Progress::Instance()); }
} g_reg;
}  // namespace

void SaveProfileNow(bool always) { Progress::Instance().SaveNow(always); }

bool BeginPersonalDrivingSchool(void*script){
    if(!script||!g_drivingReady||!Net().Online()||Net().IsHost()||!flow::InHostWorld())return false;
    const auto*name=game::Field<char[8]>(script,8);
    if(_strnicmp(name,"dskool",8)&&_strnicmp(name,"trace",8))return false;
    if(g_drivingDepth++==0){
        g_drivingWorld=ReadDrivingGlobals();PutDrivingGlobals(g_driving);
        g_drivingSkillBefore=game::Ref<int32_t>(kStatsBlock+(160-120)*4);
    }
    return true;
}
void EndPersonalDrivingSchool(){
    if(!g_drivingDepth||--g_drivingDepth)return;
    auto now=ReadDrivingGlobals();bool changed=now!=g_driving||
        g_drivingSkillBefore!=game::Ref<int32_t>(kStatsBlock+(160-120)*4);
    g_driving=now;PutDrivingGlobals(g_drivingWorld);
    if(changed){LOGI("Personal driving-school progress changed; skill=%d completed=%d",game::Ref<int32_t>(kStatsBlock+(160-120)*4),g_driving[0]);SaveProfileNow(true);}
}

}  // namespace coop
