#pragma once
#include <cstdint>
#include "shared/lib/sync/SpawnWait.h"
#include "shared/lib/sync/IdAllocator.h"
#include <map>
#include <string>

#include "common/lib/buffer/Buffer.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/sync/AnimationState.h"
#include "shared/lib/sync/PedInteraction.h"
#include "shared/lib/sync/SnapshotBuffer.h"

namespace coop {

struct PedSyncState {
    uint32_t time = 0;
    game::CVector pos, vel;
    float heading = 0;
    uint8_t moveState = 1;
    float health=100,armour=0;
    uint8_t weapon=0;
    uint8_t flags = 0;  // F_*
    uint8_t entityFlags = 3; // visible=1, collision=2, script-frozen position=4, killed-by-stealth=8
    uint16_t vehNetId = 0;
    int8_t seat = -1;  // F_IN_VEHICLE: current seat, F_ENTERING: target seat (-1 driver)
    int8_t area = 0;
    game::CVector aim;  // F_AIMING: gun target relative to pos
    uint8_t aimKind = 0;  // F_AIMING: 0 the point above, 1 player aimId, 2 ped netId aimId
    uint16_t aimId = 0;
    uint8_t drivebyStyle = 8, drivebyFrequency = 50;
    // action animation (anything but locomotion): IFP block + index inside it, 0xFFFF = none
    uint16_t animBlock = 0xFFFF, animIdx = 0, animFlags = 0;
    float animTime = 0;
    float animSpeed = 1;
    AnimationState motion;  // the owner's actual gait and playback rate
    PedInteraction interaction;

    void Write(Writer& w)const;
    bool Read(Reader& r);
    enum : uint8_t {
        F_DEAD = 1,
        F_DUCKING = 2,
        F_IN_VEHICLE = 4,
        F_AIMING = 8,
        F_FIRING = 16,
        F_ENTERING = 32,  // walking to / opening vehicle vehNetId
        F_LEAVING = 64,
        F_DRIVEBY = 128,
    };
};

// NPC known to both players. owner == my id: simulated here and announced; otherwise a mirror.
struct NetPed {
    struct Pose {uint32_t time=0;game::CVector pos,vel;float heading=0;};
    SnapshotBuffer<Pose> poses;
    uint16_t netId = 0;
    game::CPed* ped = nullptr;  // mirror: nullptr until it could be spawned (model streaming, vehicle missing)
    int handle = -1;
    uint8_t owner = 0;
    uint16_t model = 0;
    uint8_t pedType = 4;
    std::string special;  // model name for special character slots 290-299
    // owner side
    uint64_t firstAnnouncedMs = 0;
    uint64_t lastSendMs = 0;
    uint64_t handoverUntil = 0; // let authority/area snapshots catch up after takeover
    int lastClip = -1;  // shots are detected by the clip emptying
    uint8_t lastWeapon = 0;
    uint64_t lastShotMs = 0;
    // mirror side
    bool hasState = false;
    bool pendingAdoption=false;
    SpawnWait spawnWait;
    bool awayHeld=false,awayGravity=false;
    uint32_t awayPhysicsMask=0;
    PedSyncState state;
    uint64_t recvMs = 0,lastRelevantMs=0;
    uint64_t lastTaskMs = 0;
    game::CVector lastTaskTarget;
    uint8_t lastTaskMove = 0;
    bool deadApplied = false;
    uint16_t deathBlock=0xFFFF,deathIndex=0;
    uint64_t lastHitMs = 0;  // our player hit it (sent to the owner): its loot falls here if it dies of it
    uint64_t lastRamMs = 0;  // our player's car hit it (sent once per contact)
    uint8_t gunMode = 0;  // 0 none, 1 aiming, 2 shooting
    int gunTargetHandle = -1;  // ped aimed at, -1: gunTarget point
    game::CVector gunTarget;
    uint64_t gunTaskMs = 0;
    uint64_t duckMs=0;
    uint64_t weaponMs = 0; // weapon streaming must not share the walking task's timer
    uint16_t enterVeh = 0;
    uint64_t enterTaskMs = 0, leaveTaskMs = 0;
    uint16_t animBlock = 0xFFFF, animIdx = 0;  // action animation replayed on the mirror
    uint16_t baseBlock = 0xFFFF, baseIdx = 0;  // the full-body one under it (a fight stance under punches)
    float animTime = 0;                         // owner's time of it in the latest state (restarts go back)
    bool animFull = false;                      // it is not partial: locomotion tasks must wait
    bool extraFull=false,interactionAttached=false,interactionPending=false,pairedAction=false;
    float interactionAge=0; // signed sender-clock offset for a paired pose
    AnimationState extraLayers[PedInteraction::LayerCount];
    uint8_t handArm=255;uint16_t pairAnchor=0;game::CVector pairOffset;
    bool actionSentActive=false;
    bool carContact=false; // native carjacker's contact/entry pose is owned by its simulator
    uint32_t actionRevision=0,actionSignature=0,actionSentRevision=0;
    uintptr_t actionLeaf=0;float actionTime=0;
    uint16_t actionBlock=0xFFFF,actionIndex=0;
};

class Peds : public Module {
public:
    NetPed* EnsureNetworked(game::CPed* ped);
    const char* Name() const override { return "Peds"; }
    void OnEntityReplaced(uint8_t kind,uint8_t scope,uint16_t id) override;
    void OnInstallHooks() override;
    void OnSessionEnd() override;
    void OnPeerLeave(uint8_t id) override;
    void OnGameStarted() override;
    void OnGameShutdown() override;
    void OnPreProcess() override;
    void OnPostProcess() override;

    static Peds& Instance();
    const std::map<uint16_t,NetPed>& All() const { return m_byId; }
    NetPed* Find(game::CPed* ped);
    game::CPed* FindOwned(uint16_t netId);  // our live ped announced under netId
    bool Known(uint16_t id)const{return m_byId.count(id)!=0;}
    int HandleOf(uint16_t netId);            // script handle of the live ped (ours or a mirror), -1 if none
    // special character slots (models 290-299) are filled by name at run time
    void RememberSpecialModel(int model, const char* name);
    std::string SpecialName(int model) const;
    int SpecialModelFor(const std::string& name);
    void RespawnCopiesOf(int model);
    // a partner (re)started its game: announce our peds again
    void Reannounce();
    void ReannounceOne(uint16_t id);
    int RemoveSharedAmbient(const game::CVector& centre,int8_t area,int budget=4);
    // Transfer simulation while retaining the old simulator's physical mirror.
    void GiveAway(uint16_t netId, uint8_t to);
    // this game would drop the ped, the other player is nearer: it goes to that player's game (false: not ours to give)
    bool QueueGiveAway(game::CPed* ped, uint8_t to);
    // a kind of ped the other game can take over (a mirror is a civilian there: no cops, medics or firemen)
    static bool CanGiveAwayType(const NetPed& np);

private:
    void ScanOwned();
    void Announce(NetPed& np);
    void SendState(NetPed& np);
    void HandleCreate(uint8_t from, Reader& r);
    void HandleRemove(uint8_t from, Reader& r);
    void HandleSync(uint8_t from, Reader& r);
    void HandleAction(uint8_t from,Reader&r);
    void HandleOwner(uint8_t from, Reader& r);
    void ApplyMirror(NetPed& np);
    game::CPed* SpawnMirror(NetPed& np);
    void Forget(uint16_t netId, bool deletePed);
    void Adopt(NetPed& np);
    void DropMirrorsOf(uint8_t id, bool keep);  // keep: this game's own people now (it left), else deleted (it restarted)
    PedSyncState Capture(NetPed& np);

    std::map<uint16_t, NetPed> m_byId;
    std::map<game::CPed*, uint16_t> m_byPtr;
    IdAllocator<4096> m_ids;
    std::map<int, std::string> m_specialNames;
    std::map<game::CPed*, uint8_t> m_giveAway;  // to hand over after the population code (ped -> player)
    uint64_t m_lastScan = 0, m_lastStatsMs = 0;
    bool m_wasReady = false;
    int m_mirrorShots = 0;  // diagnostics: shots fired by mirrors since the last stats line
    int m_spawnBudget = 2;
};

// the interior a ped is in, by its area or, for the interiors' own people (they keep area 0), by a player near it
int8_t AreaOf(game::CPed* ped);

}  // namespace coop
