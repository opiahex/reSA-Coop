#pragma once
#include <cstdint>
#include "common/lib/sync/EntityAuthority.h"
#include <array>
#include "common/lib/diagnostics/FlightRecorder.h"
#include "shared/lib/sync/IdAllocator.h"
#include <map>
#include <set>
#include "shared/lib/sync/SpawnWait.h"

#include "entities/vehicle/VehicleState.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/sync/RecentVehicles.h"
#include "shared/lib/sync/SnapshotBuffer.h"

namespace coop {

// What a mod shop / pay'n'spray changes: colours, installed upgrades (model per slot), paintjob.
struct VehicleLook {
    uint8_t colors[4] = {};
    int16_t upgrades[15];
    int8_t paintjob = -1;
    // body damage: CDamageManager without its first float (engine, wheels, doors, lights, panels),
    // the 2 tyres of a bike
    uint8_t damage[20] = {};
    uint8_t bikeTyres[2] = {};
    uint8_t lock = 0;  // CVehicle::m_nDoorLock: a mission locks and unlocks its cars after they are known to both
    VehicleLook() {
        for (int16_t& u : upgrades) u = -1;
    }
    bool operator==(const VehicleLook& o) const;
};

// A vehicle known to both players under a network id.
//  * owner == my id  -> the local game simulates it and sends its state;
//  * owner != my id  -> "mirror": the vehicle follows the received states.
struct VehicleDiagnosticPoint {uint32_t clock=0,localWorld=0;flight::Context context{};};
// One bounded observation attempt, separate from the last successful native sample.
struct VehicleDiagnosticAttempt {VehicleDiagnosticPoint lower{},upper{};uint64_t span=0;uint32_t lowerClock=0,upperClock=0;uint16_t applyReason=0;uint8_t mode=0,flags=0;bool known=false,pending=false,critical=false,skipped=false;};
struct NetVehicle {
    uint64_t groundCheckMs = 0;  // mirror: whether this game has ground under its target (checked now and then)
    bool groundless = false;
    uint16_t netId = 0;
    game::CVehicle* veh = nullptr;
    int handle = -1;
    uint8_t owner = 0;
    uint8_t jackDriver=255;
    uint64_t jackUntil=0;
    uint16_t model = 0;
    bool createdByUs = false;  // we created it as a mirror (delete it when the owner removes it)
    bool playerUsed = false;
    bool script = false;       // a script's car in the game that announced it
    uint8_t origBy = 0;        // what this game had it created by before it became a mirror here (0: spawned as one)
    bool heldHere = false;     // a native script here holds this exact pool handle
    bool crashProofHere = false;  // made collision-proof here as a mirror (taken off when it is this game's again)
    bool physicsHeld=false,heldGravity=false,terrainHeld=false;
    bool crewStaged=false,authorityHeld=false,claimEpisode=false,claimRejected=false,awaitingLookup=false,lookupAwaitingState=false,lookupOwnedReady=false;
    int heldBoatStatus=-1,heldBoatHandle=-1;uint32_t orphanAttemptLease=0;
    VehicleCrew birthCrew,stagedCrew;
    VehicleState authoritySaved;bool authoritySavedKnown=false;
    bool trainAttached=false,trainCollisionHeld=false,trainHadCollision=false;
    bool parentAttached=false,parentCollisionHeld=false,parentHadCollision=false,parentWasVisible=false;
    int parentHandle=-1;uint64_t parentGeneration=0;
    uint32_t parentHeldPhysicsMask=0;bool parentHadGravity=false;
    VehicleState scriptAttachment;
    uint64_t attachmentSentMs=0;
    uint32_t heldPhysicsMask=0;

    bool hasState = false;
    VehicleState state;
    WheelState renderedWheels;
    uint64_t recvMs = 0;
    uint32_t lastTime = 0;
    SnapshotBuffer<VehicleState> history;
    uint64_t firstAnnouncedMs = 0;
    uint64_t lastSendMs = 0;
    bool lastEngine=false;float lastRotor=0;float sentHealth=1000;
    uint64_t handoverUntil = 0;
    uint64_t lastChangeMs = 0;  // owned: last time it moved or was used
    uint64_t lastLocalDriverMs=0;
    VehicleLook look;            // owned: last sent; mirror: wanted
    bool hasLook = false;
    bool applyingLook=false; // rebuilding a replica mesh is not a player repair
    uint64_t lookCheckMs = 0, lookSentMs = 0;
    uint32_t repairRevision=0;
    // Copied packet metadata only; a delayed model/state must keep its sender's
    // original generation, world and connection rather than the latest callback.
    flight::Context diagnosticState{},diagnosticRemove{};
    bool diagnosticStatePending=false;
    // Metadata mirrors only accepted history points; it never drives physics.
    std::array<VehicleDiagnosticPoint,32> diagnosticHistory{};
    VehicleDiagnosticPoint diagnosticLatest{};
    VehicleDiagnosticAttempt diagnosticTypedAttempt{};
    uint8_t diagnosticHistoryCount=0,diagnosticHistoryNext=0;
    uint32_t diagnosticTypedLower=0,diagnosticTypedUpper=0,diagnosticTypedThrottled=0;
    uint8_t diagnosticTypedMode=0,diagnosticTypedFlags=0;
    bool diagnosticTypedKnown=false,diagnosticTypedFlagsKnown=false,diagnosticTypedAttemptKnown=false;
    uint64_t diagnosticTypedAttemptMs=0;
    uint16_t diagnosticTypedError=0,diagnosticTypedApplyReason=0;
    uint16_t lastTowChild=0xFFFF,lastTowParent=0xFFFF;
};

class Vehicles : public Module {
public:
    void ForgetOwnedBy(uint8_t owner, const std::vector<uint16_t>& preserved);
    std::vector<uint16_t> PreservedIds() const;
    void RestorePlayerCar(game::CVehicle* vehicle, const VehicleSpawn& spawn);  // its game started over: its mirrors go, the others are this game's again
    const char* Name() const override { return "Vehicles"; }
    void OnEntityReplaced(uint8_t kind,uint8_t scope,uint16_t id) override;
    void OnInstallHooks() override;
    void OnSessionStart() override;
    void OnSessionEnd() override;
    void OnPeerLeave(uint8_t id) override;
    void OnGameStarted() override;
    void OnGameShutdown() override;
    void OnPreProcess() override;
    void OnPostProcess() override;

    static Vehicles& Instance();
    const std::map<uint16_t,NetVehicle>& All() const { return m_byId; }
    // Logical metadata only; added native observation must gate before pool lookup.
    NetVehicle* DiagnosticRecord(uint16_t id){auto i=m_byId.find(id);return i==m_byId.end()?nullptr:&i->second;}
    const std::map<uint16_t,VehicleSpawn>& Pending() const { return m_pending; }

    const SpawnWait* PendingWait(uint16_t id)const{auto i=m_spawnWait.find(id);return i==m_spawnWait.end()?nullptr:&i->second;}
    NetVehicle* Find(uint16_t netId);
    // the record's vehicle is still there (one deleted in the last frame stays listed until OnPreProcess, after the
    // network handlers that use its handle)
    static bool Alive(const NetVehicle& nv) { return nv.veh && game::GetVehicle(nv.handle) == nv.veh; }
    NetVehicle* Find(game::CVehicle* v);
    // Makes a local vehicle known to the other player (announces it) and returns its record.
    NetVehicle* EnsureNetworked(game::CVehicle* v);
    static bool ScriptControlled(game::CVehicle* v);
    // Local player became the driver of `nv`: take ownership.
    void TakeOwnership(NetVehicle& nv);
    void ExpectCarjack(uint16_t id,uint8_t driver);
    bool IsMirror(const NetVehicle& nv) const;
    bool HandleAuthority(authority::Notice& notice);
    bool RequestCrewAuthority(NetVehicle& nv,uint8_t target,bool voluntary);
    bool RetainedSource(game::CVehicle* car,uint16_t& id,uint64_t& generation)const;
    bool IsRecent(uint16_t id) const;
    void RememberUsed(game::CVehicle* vehicle);
    void RenderSearchlights();
    VehicleState Capture(game::CVehicle* vehicle);
    void CloneAppearance(game::CVehicle*source,game::CVehicle*target);
    // Applies a state received from the vehicle's driver (Players module, G_PLAYER_DRIVER).
    void ApplyDriverState(uint8_t from, uint16_t netId, const VehicleState& st,const flight::Context* deferredDiagnostic=nullptr);
    // a partner (re)started its game: announce our vehicles again
    void Reannounce();
    void ReannounceOne(uint16_t id);
    void ShareScriptLock(game::CVehicle* vehicle);
    void ReleaseScriptLocks();
    void ShareScriptDeletion(game::CVehicle* vehicle);
    // Called after the native 0683/0684 opcode, retaining its actual result.
    void ShareScriptAttachment(game::CVehicle* vehicle,bool attached);
    int RemoveSharedAmbient(const game::CVector& centre,int8_t area,int budget=2);
    // this game would drop the car, the other player is nearer: it goes to that player's game (false: not ours to give)
    bool QueueGiveAway(game::CVehicle* v, uint8_t to);
    // Accept an exact story assignment while birth authority is still pending.
    bool AssignStoryRemote(game::CVehicle* v,uint8_t to);
    bool StoryRemoteReady(const NetVehicle& vehicle,uint8_t player)const;
    // The host's mission cleared an area: the other players' empty cars there go from this game at once (their copies
    // are mission cars here, CLEAR_AREA left them). Returns their ids for the owners (G_VEH_CLEAR).
    std::vector<uint16_t> ClearMirrors(const game::CVector& at, float radius);

private:
    struct StoryAssignment {
        uint64_t generation=0,sourceGeneration=0,run=0;
        uint32_t phase=0,assignment=0,connection=0,world=0,request=0;
        uint16_t source=0;uint8_t target=255;int handle=-1;
        game::CVehicle*body=nullptr;bool requested=false,committed=false,failed=false;
    };
    std::map<uint16_t,StoryAssignment> m_storyAssignments;
    void PollStoryAssignments();
    void ObserveStoryAssignment(const authority::Notice& notice);
    std::map<uint16_t,SpawnWait> m_spawnWait;
    void Announce(NetVehicle& nv);
    VehicleCrew CaptureCrew(game::CVehicle* car);
    bool CrewReady(const NetVehicle& nv,const VehicleCrew& crew)const;
    void UpdateCrewGate(NetVehicle& nv);
    void ApplyStaging(NetVehicle& nv);
    void RestoreHeldBoat(NetVehicle& nv);
    bool CreateMirror(const VehicleSpawn& sp);
    void ScanTraffic();
    void HandleCreate(uint8_t from, Reader& r);
    void HandleRemove(uint8_t from, Reader& r);
    void HandleSync(uint8_t from, Reader& r);
    void HandleOwner(uint8_t from, Reader& r);
    void HandleStatus(uint8_t from, Reader& r);
    void SendLook(NetVehicle& nv);
    void ApplyLook(NetVehicle& nv);
    void ApplyMirror(NetVehicle& nv, float dt);
    void ObserveScriptOwnership(NetVehicle& nv);
    bool ApplyVehicleBinding(NetVehicle& nv,const VehicleState& state);
    void UpdateVehicleBindings();
    void HandleScriptAttachment(uint8_t from,Reader& r);
    void SendScriptAttachment(NetVehicle& nv);
    void SendState(NetVehicle& nv,bool reliable=false);
    void ApplyTowLinks(NetVehicle& nv);
    void Forget(uint16_t netId, bool deleteVehicle);
    void Drop(uint16_t netId);  // the owner let it go: a spawned mirror goes, an instance of this game stays
    void Protect(NetVehicle& nv);
    void Release(NetVehicle& nv);
    void HoldUnloadedPhysics(NetVehicle& nv,bool hold);
    void GiveAway(NetVehicle& nv, uint8_t to);
    void HandleHandover(uint8_t from, Reader& r);
    void HandleScriptFree(uint8_t from, Reader& r);
    void HandleClear(uint8_t from, Reader& r);
    void HandleRecent(uint8_t from, Reader& r);
    void SendRecent();
    void UpdateRecentProtection();
    void KeepCarWhileInside(game::CPlayerPed* me);
    void ReleaseKeptCar();
    game::CVehicle* SpawnMirror(const VehicleSpawn& sp);
    void RecordPendingDiagnostic(uint16_t id,uint16_t reason,const flight::Context* diagnostic=nullptr);
    void RetirePendingDiagnostic(uint16_t id,uint16_t reason);
    game::CVehicle* FindAdoptable(uint16_t model, const game::CVector& pos, const game::CVehicle* excluded = nullptr);

    std::map<uint16_t, NetVehicle> m_byId;
    std::map<game::CVehicle*, uint16_t> m_byPtr;
    std::map<uint16_t, VehicleSpawn> m_pending;  // announced mirrors waiting for their model to stream in
    std::map<uint16_t,std::pair<uint8_t,VehicleState>> m_pendingStates;
    struct PendingDiagnostic {flight::Context spawn{},state{};uint16_t waitReason=0;};
    std::map<uint16_t,PendingDiagnostic> m_pendingDiagnostics; // subset of m_pending
    std::map<uint16_t,std::pair<VehicleLook,uint32_t>> m_pendingLooks;
    std::set<uint16_t> m_adoptPending;
    std::map<uint16_t,uint64_t> m_scriptRemoved;
    std::map<game::CVehicle*, uint8_t> m_giveAway;  // to hand over after the population code (vehicle -> player)
    IdAllocator<4096> m_ids;
    std::map<uint16_t,uint8_t> m_scriptLocks;
    uint64_t m_lastScan = 0;
    int m_lastDriven = -1, m_kept = -1;  // handles: the car the local player last drove, and it kept while inside
    uint8_t m_keptBy = 0;               // what it was created by before
    int8_t m_lastArea = 0;
    std::map<uint8_t, RecentVehicles> m_recent;
    std::vector<VehicleSpawn> m_restoreRecent;
    // A copy we could not safely delete on disconnect (for example, the car
    // the local player is sitting in). Rebind only its exact network birth.
    struct RetainedCopy { int handle=-1; uint16_t model=0; uint64_t generation=0, expires=0; bool localOrigin=false,worldLost=false;VehicleSpawn spawn;VehicleState state; };
    void RetainUsedCar(const NetVehicle& vehicle);
    void RebindRetainedCars();
    void ActivateRetainedOwned(NetVehicle& vehicle);
    std::map<uint16_t, RetainedCopy> m_retainedCopies;
};

// Vehicles that are synchronised at all (trains are left to each game). An RC toy is its player's game's car with
// nobody in it: that player stays on foot in it (CPed::bInVehicle), and the other game sees the toy fly or drive.
bool IsSyncableVehicle(game::CVehicle* v);

}  // namespace coop
