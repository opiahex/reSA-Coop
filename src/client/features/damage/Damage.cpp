// Damage between the two games. Who decides a hit:
//  * NPCs and vehicles are damaged only by their owner's game: a hit of the local player on a mirror is
//    sent to the owner, which applies it with the game's own damage functions (the NPC then reacts to
//    the remote player as the attacker: flees, fights back, dies);
//  * the remote gun pose/sound use pad emulation; actual bullet traces/impacts are replayed by Bullets.cpp.
//    Their damage is dropped here; the
//    shooter's game reports its hits itself. Vehicle collisions are the exception: they are simulated
//    by the victim's owner anyway, so they are never forwarded;
//  * everything else (NPC shots, explosions, fire) hurts whatever this game owns, the local player too;
//  * the other player's blasts go off here as well (Fire.cpp) and hurt the peds this game owns: its copies of them are
//    proof against blasts in the shooter's game. Cars take a blast there and the hit is forwarded, as a bullet's;
//  * an NPC's punch or blade on the remote player is sent to that player's game: the remote ped here has every proof,
//    and the NPC's mirror there only plays the animation (its gunfire needs nothing: the mirror there shoots for real);
//  * a ped killed by the other player drops its money and weapon in the killer's game, where the killer is;
//  * a crime is charged by the game of whoever committed it: the owner keeps its own player's wanted level when it
//    applies the other player's hit, and the hitter's game reports the crime itself;
//  * a kill counts (stats, respect, rampages, gang war provocation) in the killer's game: the owner of the ped tells it.
//    Gang-war provocation is an owner-confirmed fact evaluated in that killer's native zone context;
//  * the players hurt each other only with friendly fire (the host's choice). The shooter's game reports its bullets
//    and blows on the other player (it sees them hit), the victim's game lets the other player's explosions, fire and
//    car hurt its player (the copy here is proof against them, so they never reach the shooter's game);
//  * during a scripted scene (the camera's wide screen) the other player's side hurts nothing here: its player keeps
//    playing while this one watches, and a mission failed when its people got hurt in the scene.
#include <algorithm>
#include <cmath>
#include <map>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/diagnostics/PassengerTrace.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "entities/activity/Activities.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/game/Anim.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "features/damage/HitEvidence.h"
#include "shared/api/story-actions/StoryActions.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/gangwar/GangWarCoop.h"
#include "shared/lib/sync/EventWindow.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

constexpr int WEAPON_LAST_MELEE = 15, WEAPON_PISTOL = 22, WEAPON_SNIPERRIFLE = 34, WEAPON_MINIGUN = 38,
              WEAPON_SPRAYCAN = 41, WEAPON_EXTINGUISHER = 42, WEAPON_RAMMEDBYCAR = 49, WEAPON_RUNOVERBYCAR = 50,
              WEAPON_EXPLOSION = 51, WEAPON_UZI_DRIVEBY = 52;
// TARGET_PLAYER: an NPC hit the player (netId is the NPC), TARGET_PLAYER_BY_PLAYER: the sender hit the player itself,
// KILLED_BY_YOU: the receiver's hit killed the sender's ped (piece is the headshot).
enum : uint8_t { TARGET_PED = 1, TARGET_VEHICLE = 2, TARGET_PLAYER = 3, TARGET_PLAYER_BY_PLAYER = 4, KILLED_BY_YOU = 5 };
constexpr int CRIME_DAMAGED_PED = 2, CRIME_DAMAGED_COP = 3;

// The engine charges a crime to the player in focus (CEventGroup::Add -> ReportCriminalEvent -> FindPlayerWanted):
// the other player's hits, applied here, raised this player's wanted level. It is put back as it was (CWanted up to
// its police scanner, an audio entity left alone).
struct KeepWanted {
    uint8_t* wanted = nullptr;
    uint8_t saved[0x21C];
    KeepWanted() {
        CPlayerPed* me = FindPlayerPed(0);
        if (me && me->PlayerData()) wanted = (uint8_t*)me->Wanted();
        if (wanted) memcpy(saved, wanted, sizeof saved);
    }
    ~KeepWanted() {
        if (wanted) memcpy(wanted, saved, sizeof saved);
    }
};

// GTA's "only damaged by player" flag recognises its local CJ. A remote
// player's validated hit is a player's hit too; retain every other proof.
struct AsPlayersHit {
    CPhysical* entity;
    bool had;
    explicit AsPlayersHit(CPhysical* e,bool player=true) : entity(e), had(player&&(e->PhysicalFlags() & CPhysical::PF_INVULNERABLE)) {
        if(player)entity->PhysicalFlags() &= ~CPhysical::PF_INVULNERABLE;
    }
    ~AsPlayersHit() {
        if (had) entity->PhysicalFlags() |= CPhysical::PF_INVULNERABLE;
    }
};

bool IsCollision(int weapon) { return weapon == WEAPON_RAMMEDBYCAR || weapon == WEAPON_RUNOVERBYCAR; }
// a bullet or a blow: the shooter's game sees where it hits
bool IsDirect(int weapon) {
    return weapon <= WEAPON_LAST_MELEE || (weapon >= WEAPON_PISTOL && weapon <= WEAPON_SNIPERRIFLE) || weapon == WEAPON_MINIGUN ||
           weapon == WEAPON_UZI_DRIVEBY;
}
// a spray never reaches a mirror (mirrors are fire-proof and CShotInfo skips them): the owner applies the remote
// player's own spray on its peds
bool IsSpray(int weapon) { return weapon == WEAPON_SPRAYCAN || weapon == WEAPON_EXTINGUISHER; }

CPed* DriverOf(CEntity* e) { return e && e->IsVehicle() ? ((CVehicle*)e)->Driver() : nullptr; }

CVehicle* DamageSourceVehicle(CEntity* source,int weapon) {
    if(!source)return nullptr;
    if(source->IsVehicle())return (CVehicle*)source;
    // A direct shot is resolved inside the current fire call. A delayed
    // projectile must not inherit whichever car its creator entered later.
    if(source->IsPed()&&IsDirect(weapon))return ((CPed*)source)->VehicleIfIn();
    return nullptr;
}

bool IsLocalPlayerEntity(CEntity* e) {
    CPlayerPed* me = FindPlayerPed(0);
    return e && me && (e == me || DriverOf(e) == me || (e->IsVehicle()&&CooperativeRemoteController((CVehicle*)e)==me));
}

bool IsRemotePlayerEntity(CEntity* e) {
    if (!e) return false;
    CPed* ped = e->IsPed() ? (CPed*)e : DriverOf(e);
    if(!ped&&e->IsVehicle())ped=CooperativeRemoteController((CVehicle*)e);
    return ped && Players::Instance().FindByPed(ped);
}

// the other player, its car, or a car or ped its game owns (a copy here)
bool FromOtherSide(CEntity* e) {
    if (!e) return false;
    if (IsRemotePlayerEntity(e)) return true;
    if (e->IsVehicle()) {
        NetVehicle* nv = Vehicles::Instance().Find((CVehicle*)e);
        return nv && (Vehicles::Instance().IsMirror(*nv)||nv->authorityHeld);
    }
    NetPed* np = e->IsPed() ? Peds::Instance().Find((CPed*)e) : nullptr;
    return np && (np->owner != Net().MyId()||np->authorityHeld);
}

uint32_t damageSequence=0;EventWindow damageSeen[3];
enum DamageDiagnosticReason:uint16_t {
    DD_OK=0,DD_MALFORMED,DD_NOT_AUTHORITY,DD_NOT_READY,DD_DAMAGE_RANGE,DD_BODY_RANGE,
    DD_CUTSCENE,DD_EVIDENCE,DD_DUPLICATE,DD_TRAINING,DD_SOURCE_REQUIRED,DD_NATIVE_MISSING,
    DD_PROOF,DD_FRIENDLY_FIRE,DD_UNSUPPORTED,DD_CONTEXT,DD_HEALTH_OBSERVED,
    DD_NO_IMMEDIATE_CHANGE,DD_GENERATION,DD_OUTCOME_UNAVAILABLE,DD_NATIVE_FALSE,DD_SENDER,
    DD_SOURCE_UNAVAILABLE,DD_NATIVE_FILTERED,DD_NATIVE_SAMPLED,DD_NATIVE_BUSY,DD_NATIVE_BUDGET,DD_NATIVE_DISABLED,DD_KILL_CREDIT_RETURNED
};
uint8_t DamageTargetKind(uint8_t kind){return kind==TARGET_PED?ENTITY_PED:kind==TARGET_VEHICLE?ENTITY_VEHICLE:0;}
struct HeldDamageCreator {CEntity*body=nullptr;int handle=-1;uint16_t model=0;uint8_t type=0,targetKind=0;uint16_t target=0;uint64_t generation=0,at=0;uint32_t connection=0,world=0;};
std::map<uint32_t,HeldDamageCreator> heldDamageCreators;
void RememberHeldCreator(uint32_t sequence,uint8_t kind,uint16_t id,CEntity*creator){
    const auto now=NowMs();for(auto i=heldDamageCreators.begin();i!=heldDamageCreators.end();)if(now-i->second.at>1500)i=heldDamageCreators.erase(i);else++i;
    if(heldDamageCreators.size()>=128)heldDamageCreators.erase(heldDamageCreators.begin());
    HeldDamageCreator c{};c.targetKind=kind;c.target=id;c.generation=Net().EntityGeneration(DamageTargetKind(kind),id);c.at=now;c.connection=Net().MyIncarnation();c.world=Net().WorldEpoch();
    if(creator&&creator->IsPed()&&IsPedValid((CPed*)creator)){c.body=creator;c.type=1;c.handle=GetPedRef((CPed*)creator);c.model=creator->ModelIndex();}
    else if(creator&&creator->IsVehicle()&&IsVehicleValid((CVehicle*)creator)){c.body=creator;c.type=2;c.handle=GetVehicleRef((CVehicle*)creator);c.model=creator->ModelIndex();}
    heldDamageCreators[sequence]=c;
}
bool HeldDamageCurrent(uint32_t sequence,uint8_t kind,uint16_t id){
    auto i=heldDamageCreators.find(sequence);if(i==heldDamageCreators.end())return false;const auto&c=i->second;
    return c.targetKind==kind&&c.target==id&&c.connection==Net().MyIncarnation()&&c.world==Net().WorldEpoch()&&NowMs()-c.at<=1500&&c.generation==Net().EntityGeneration(DamageTargetKind(kind),id);
}
CEntity* HeldCreator(uint32_t sequence,uint8_t kind,uint16_t id){
    if(!HeldDamageCurrent(sequence,kind,id))return nullptr;const auto&c=heldDamageCreators.find(sequence)->second;
    if(c.handle<0)return nullptr;
    CEntity*body=c.type==1?static_cast<CEntity*>(GetPed(c.handle)):c.type==2?static_cast<CEntity*>(GetVehicle(c.handle)):nullptr;
    return body&&body==c.body&&body->ModelIndex()==c.model?body:nullptr;
}
void DamageNumbers(flight::Event&e,uint32_t sequence,uint8_t kind,uint16_t id,uint64_t generation,int weapon,float damage){
    flight::AddUnsigned(e,flight::Metric::RequestId,sequence);
    if(std::isfinite(damage))flight::AddFloat(e,flight::Metric::RequestedDamage,damage);
    flight::AddUnsigned(e,flight::Metric::Weapon,(uint32_t)weapon);
    if(auto target=DamageTargetKind(kind)){
        flight::AddUnsigned(e,flight::Metric::TargetKind,target);flight::AddUnsigned(e,flight::Metric::TargetId,id);
        if(generation)flight::AddUnsigned(e,flight::Metric::TargetGeneration,generation);
    }
}
struct DamageDiagnostic {
    flight::Context context{};uint32_t sequence=0,localWorld=0;uint16_t id=0;
    uint8_t kind=0,owner=255;int weapon=0;float requested=0;bool enabled=false,packet=false,decoded=true;
    DamageDiagnostic(uint8_t from,uint32_t token,uint8_t target,uint8_t who,uint16_t entity,int gun,float amount,bool validHeader=true)
        :sequence(token),id(entity),kind(target),owner(who),weapon(gun),requested(amount),decoded(validHeader){
        if(!(enabled=flight::Enabled()))return;
        localWorld=Net().WorldEpoch();context=Net().DiagnosticContext();
        packet=context.msg==proto::G_DAMAGE&&context.source==from;
        if(!packet){context={};context.msg=proto::G_DAMAGE;context.source=from;}
        if(decoded){if(auto targetKind=DamageTargetKind(kind)){
            context.entity=id;
            context.generation=packet?Net().ExpectedEntityGeneration(targetKind,id):0;
        }else{context.entity=0;context.generation=0;}}
    }
    flight::Event BaseEvent(uint16_t stage,uint16_t reason)const{
        flight::Event e{};e.context=context;e.stage=stage;e.reason=reason;e.localPeer=Net().MyId();
        if(decoded)flight::AddUnsigned(e,flight::Metric::RequestedOwner,owner);return e;
    }
    flight::Event Event(uint16_t stage,uint16_t reason)const{
        auto e=BaseEvent(stage,reason);if(decoded)DamageNumbers(e,sequence,kind,id,context.generation,weapon,requested);return e;
    }
    void Record(uint16_t stage,uint16_t reason)const{
        if(enabled&&flight::Enabled())(void)flight::Record(Event(stage,reason));
    }
    bool Current()const{
        if(!decoded||!packet||Net().WorldEpoch()!=localWorld)return false;
        auto target=DamageTargetKind(kind);
        if(target){if(!context.generation||Net().EntityGeneration(target,id)!=context.generation)return false;}
        else if(kind!=TARGET_PLAYER&&kind!=TARGET_PLAYER_BY_PLAYER)return false;
        if(context.source==Net().MyId())return context.incarnation==Net().MyIncarnation()&&context.world==Net().WorldEpoch();
        auto*peer=Net().Peer(context.source);return peer&&peer->incarnation==context.incarnation&&Net().PeerWorldEpoch(context.source)==context.world;
    }
    void Source(uint16_t sourceId,uint64_t generation,bool valid)const{
        if(!enabled||!flight::Enabled())return;
        auto e=BaseEvent(804,valid?DD_OK:DD_SOURCE_UNAVAILABLE);
        // A separate row keeps the six target/request fields below the eight
        // numeric limit. This is the claimed source dependency, not a pointer.
        flight::AddUnsigned(e,flight::Metric::RequestId,sequence);
        flight::AddUnsigned(e,flight::Metric::ActorKind,ENTITY_VEHICLE);
        flight::AddUnsigned(e,flight::Metric::ActorId,sourceId);
        if(generation)flight::AddUnsigned(e,flight::Metric::ActorGeneration,generation);
        if(auto target=DamageTargetKind(kind)){flight::AddUnsigned(e,flight::Metric::TargetKind,target);flight::AddUnsigned(e,flight::Metric::TargetId,id);if(context.generation)flight::AddUnsigned(e,flight::Metric::TargetGeneration,context.generation);}
        (void)flight::Record(e);
    }
};
// Observes the existing call; it never decides whether the engine may run it.
// A true GenerateDamageEvent return can queue a future event without changing
// HP now. The result and observed HP are recorded separately, never equated.
struct DamageNativeObservation {
    const DamageDiagnostic& diagnostic;CPed*ped=nullptr;CVehicle*vehicle=nullptr;int handle=-1;
    float before=0;uint16_t unavailable=DD_NATIVE_MISSING;bool bound=false;
    DamageNativeObservation(const DamageDiagnostic&d,CPed*p):diagnostic(d),ped(p){Begin();}
    DamageNativeObservation(const DamageDiagnostic&d,CPed*p,float observedBefore):diagnostic(d),ped(p){Begin(true,observedBefore);}
    DamageNativeObservation(const DamageDiagnostic&d,CVehicle*v):diagnostic(d),vehicle(v){Begin();}
    bool Alive()const{return ped?IsPedValid(ped)&&GetPedRef(ped)==handle:vehicle&&IsVehicleValid(vehicle)&&GetVehicleRef(vehicle)==handle;}
    float Health()const{return ped?ped->Health():vehicle->Health();}
    flight::Event BoundEvent(uint16_t stage,uint16_t reason)const{
        auto e=diagnostic.Event(stage,reason);uint8_t owner=255;
        if(ped&&(diagnostic.kind==TARGET_PLAYER||diagnostic.kind==TARGET_PLAYER_BY_PLAYER)){
            if(ped==FindPlayerPed(0))owner=Net().MyId();
        }else if(ped){auto*n=Peds::Instance().Find(ped);if(n&&n->netId==diagnostic.id)owner=n->owner;}
        else if(vehicle){auto*n=Vehicles::Instance().Find(vehicle);if(n&&n->netId==diagnostic.id)owner=n->owner;}
        if(owner<3){e.owner=owner;e.detailKnown|=flight::OwnerKnown;}return e;
    }
    uint16_t AdmissionReason(uint16_t stage)const{
        const auto admission=flight::AdmitNativeObservation(diagnostic.context,stage,Net().MyId(),true);
        if(admission==flight::NativeAdmission::Admitted)return DD_OK;
        uint16_t reason=admission==flight::NativeAdmission::Filtered?DD_NATIVE_FILTERED:
            admission==flight::NativeAdmission::Sampled?DD_NATIVE_SAMPLED:
            admission==flight::NativeAdmission::Busy?DD_NATIVE_BUSY:
            admission==flight::NativeAdmission::Budget?DD_NATIVE_BUDGET:DD_NATIVE_DISABLED;
        return reason;
    }
    void Begin(bool hasBefore=false,float observedBefore=0){
        if(!diagnostic.enabled||!flight::Enabled())return;
        if(auto reason=AdmissionReason(802)){unavailable=reason;diagnostic.Record(802,reason);return;}
        if(!diagnostic.Current()){unavailable=DD_CONTEXT;diagnostic.Record(802,unavailable);return;}
        if(ped&&IsPedValid(ped))handle=GetPedRef(ped);else if(vehicle&&IsVehicleValid(vehicle))handle=GetVehicleRef(vehicle);
        if(handle<0||!Alive()){diagnostic.Record(802,unavailable);return;}
        before=hasBefore?observedBefore:Health();bound=true;auto e=BoundEvent(802,DD_OK);e.nativeHandle=(uint32_t)handle;e.health=e.beforeHealth=before;
        if(ped)e.armour=ped->Armour();(void)flight::Record(e);
    }
    void Complete(bool hasResult=false,bool result=false)const{
        if(!diagnostic.enabled||!flight::Enabled())return;
        if(!bound){auto e=diagnostic.Event(803,unavailable);if(hasResult)flight::AddSigned(e,flight::Metric::NativeResult,result?1:0);(void)flight::Record(e);return;}
        if(auto reason=AdmissionReason(803)){auto e=diagnostic.Event(803,reason);if(hasResult)flight::AddSigned(e,flight::Metric::NativeResult,result?1:0);(void)flight::Record(e);return;}
        if(!diagnostic.Current()){diagnostic.Record(803,DD_CONTEXT);return;}
        if(!Alive()){diagnostic.Record(803,DD_NATIVE_MISSING);return;}
        const float after=Health();auto e=BoundEvent(803,hasResult&&!result?DD_NATIVE_FALSE:after!=before?DD_HEALTH_OBSERVED:DD_NO_IMMEDIATE_CHANGE);
        e.nativeHandle=(uint32_t)handle;e.beforeHealth=before;e.health=e.afterHealth=after;if(ped)e.armour=ped->Armour();
        if(hasResult)flight::AddSigned(e,flight::Metric::NativeResult,result?1:0);
        (void)flight::Record(e);
    }
};
bool SendDamage(uint8_t kind, uint8_t owner, uint16_t netId, int weapon, float damage, int piece, int dir, const CVector& pos,CVehicle* sourceVehicle=nullptr,CEntity*heldCreator=nullptr,bool victimComputed=false) {
    Writer w(proto::G_DAMAGE);
    if(++damageSequence==0)++damageSequence;w.U32(damageSequence);
    w.U8(kind);
    w.U8(owner);
    if(kind==TARGET_PED)w.Entity(ENTITY_PED,netId);else if(kind==TARGET_VEHICLE)w.Entity(ENTITY_VEHICLE,netId);else w.U16(netId);
    w.U8((uint8_t)weapon);
    w.F32(damage);
    w.U8((uint8_t)piece);
    w.U8((uint8_t)dir);
    w.Vec({pos.x, pos.y, pos.z});
    // Protocol46: current victim simulator may report a native NPC/environment
    // hit while quiesced. The server aborts that transfer and echoes it here;
    // native creator refs stay local and are never attributed to a player.
    w.U8(victimComputed?1:0);
    CEntity* target=nullptr;
    if(kind==TARGET_PED)target=GetPed(Peds::Instance().HandleOf(netId));
    if(kind==TARGET_VEHICLE)if(auto*n=Vehicles::Instance().Find(netId))target=n->veh;
    if(kind==TARGET_PLAYER_BY_PLAYER&&!IsCollision(weapon))if(auto*p=Players::Instance().Get(owner))target=p->ped;
    WriteHitEvidence(w,target);
    // Keep the actual source captured at the hit. Later RC role/seat changes
    // must not turn an unrelated player's old shot into a mission RC attack.
    bool sourceForTarget=kind==TARGET_VEHICLE||(kind==TARGET_PED&&weapon==WEAPON_RAMMEDBYCAR);
    auto*source=sourceForTarget&&sourceVehicle&&IsVehicleValid(sourceVehicle)&&IsLocalPlayerEntity(sourceVehicle)?Vehicles::Instance().Find(sourceVehicle):nullptr;
    bool hasSource=source&&Vehicles::Alive(*source)&&source->owner==Net().MyId()&&Net().EntityLive(ENTITY_VEHICLE,source->netId);
    if(kind==TARGET_PED&&weapon==WEAPON_RAMMEDBYCAR&&!hasSource&&!victimComputed){
        if(flight::Enabled()){flight::Event e{};e.stage=801;e.reason=DD_SOURCE_REQUIRED;e.localPeer=Net().MyId();e.context.msg=proto::G_DAMAGE;e.context.source=Net().MyId();e.context.incarnation=Net().MyIncarnation();e.context.world=Net().WorldEpoch();e.context.entity=netId;e.context.generation=Net().EntityGeneration(ENTITY_PED,netId);flight::AddUnsigned(e,flight::Metric::RequestedOwner,owner);DamageNumbers(e,damageSequence,kind,netId,e.context.generation,weapon,damage);(void)flight::Record(e);}
        LOGD("NPC ram not sent id=%04X: authoritative source unavailable",netId);
        return false;
    }
    w.Bool(hasSource);
    if(hasSource){
        auto generation=Net().EntityGeneration(ENTITY_VEHICLE,source->netId);
        // Non-collision hits can still apply after their source disappears;
        // ram reactions require this exact car instead of a later driver seat.
        uint8_t sourceUse=kind==TARGET_PED&&weapon==WEAPON_RAMMEDBYCAR?ENTITY_USE:ENTITY_OPTIONAL;
        w.Entity(ENTITY_VEHICLE,source->netId,sourceUse,255,generation);w.U16(source->model);w.U64(generation);w.U32((uint32_t)NowMs());
        StoryRemoteRole role;bool rc=CooperativeRemoteRole(Net().MyId(),role)&&role.car==sourceVehicle;
        w.U64(rc?role.run:0);w.U32(rc?role.phase:0);w.U32(rc?role.assignment:0);
    }
    passenger_trace::Publication diagnosticPublication(0,0,0,0);
    if(flight::Enabled()){DamageNumbers(passenger_trace::publication,damageSequence,kind,netId,DamageTargetKind(kind)?Net().EntityGeneration(DamageTargetKind(kind),netId):0,weapon,damage);flight::AddUnsigned(passenger_trace::publication,flight::Metric::RequestedOwner,owner);}
    if(victimComputed)RememberHeldCreator(damageSequence,kind,netId,heldCreator);
    Net().Send(proto::CH_RELIABLE, w);
    return true; // producer validation passed and Send was called; not remote/native acknowledgement
}

using GenerateDamageFn = bool(__cdecl*)(CPed*, CEntity*, int, int, int, int);
GenerateDamageFn g_origGenerateDamage = nullptr;

// CWeapon::GenerateDamageEvent(victim, creator, weapon, damage, bodyPart, direction): every ped damage
// (bullets, melee, explosions, fire, vehicle hits) goes through it. It tells whether the hit counts (blood, a fight
// hit): one decided in the other game still does here.
bool __cdecl HookGenerateDamageEvent(CPed* victim, CEntity* creator, int weapon, int damage, int piece, int dir) {
    if(TrainingIgnoresContact(victim,creator))return true;
    if (Net().Online() && victim && core::IsPlayerReady()) {
        if (cam::WideScreenOn() && FromOtherSide(creator)) return true;  // see the top of the file
        bool fromRemote = IsRemotePlayerEntity(creator);
        if (fromRemote && victim == FindPlayerPed(0))  // see the top of the file
            return Net().FriendlyFire() && !IsDirect(weapon) && !IsCollision(weapon) ? g_origGenerateDamage(victim, creator, weapon, damage, piece, dir) : true;
        if (fromRemote && weapon == WEAPON_EXPLOSION) {  // see the top of the file
            NetPed* np = Peds::Instance().Find(victim);
            if ((np && (np->owner != Net().MyId()||np->authorityHeld)) || Players::Instance().FindByPed(victim)) return true;
            KeepWanted keep;
            return g_origGenerateDamage(victim, creator, weapon, damage, piece, dir);
        }
        if (!IsCollision(weapon) && fromRemote && !IsSpray(weapon)) return true;
        if(auto*np=Peds::Instance().Find(victim);np&&np->authorityHeld&&np->owner==Net().MyId()&&!IsLocalPlayerEntity(creator)&&!fromRemote&&!IsCollision(weapon)){
            SendDamage(TARGET_PED,np->owner,np->netId,weapon,(float)damage,piece,dir,victim->GetPosition(),nullptr,creator,true);
            return true;
        }
        if (RemotePlayer* rp = Players::Instance().FindByPed(victim)) {
            if (weapon <= WEAPON_LAST_MELEE && creator && creator->IsPed() && !IsLocalPlayerEntity(creator)) {
                NetPed* attacker = Peds::Instance().Find((CPed*)creator);
                SendDamage(TARGET_PLAYER, rp->id, attacker ? attacker->netId : 0, weapon, (float)damage, piece, dir,
                           victim->GetPosition());
                LOGD("NPC %04X hit player %d: weapon %d, damage %d", attacker ? attacker->netId : 0, rp->id, weapon, damage);
                return true;
            }
            if (Net().FriendlyFire() && IsDirect(weapon) && IsLocalPlayerEntity(creator)) {
                SendDamage(TARGET_PLAYER_BY_PLAYER, rp->id, 0, weapon, (float)damage, piece, dir, victim->GetPosition());
                LOGI("Hit player %d: weapon %d, damage %d", rp->id, weapon, damage);
                return true;
            }
        }
        if (NetPed* np = Peds::Instance().Find(victim); np && (np->owner != Net().MyId()||np->authorityHeld)) {
            if (!IsCollision(weapon) && IsLocalPlayerEntity(creator) && !np->deadApplied) {
                SendDamage(TARGET_PED, np->owner, np->netId, weapon, (float)damage, piece, dir, victim->GetPosition());
                np->lastHitMs = NowMs();
                // the crime is ours (the owner applies the hit without charging anybody): CCrime::ReportCrime
                Call<0x532010>(np->pedType == PED_TYPE_COP ? CRIME_DAMAGED_COP : CRIME_DAMAGED_PED, (CEntity*)victim, (CPed*)FindPlayerPed(0));
                LOGI("Hit ped %04X of player %d: weapon %d, damage %d", np->netId, np->owner, weapon, damage);
            }
            return true;
        }
    }
    return g_origGenerateDamage(victim, creator, weapon, damage, piece, dir);
}

// CPed::KillPedWithCar(car, impulse, ...): a car knocks a ped down. The driver's game decides it: a mirror that the
// local player's car hits is sent to its owner (a mirror is collision-proof and would stand like a stone), and the
// remote player's car here hits nobody by itself, but this game's player with friendly fire.
using KillWithCarFn = void(__fastcall*)(CPed*, void*, CVehicle*, float, int);
KillWithCarFn g_origKillWithCar = nullptr;
void __fastcall HookKillPedWithCar(CPed* ped, void* edx, CVehicle* car, float impulse, int flag) {
    if(TrainingIgnoresContact(ped,car))return;
    if (Net().Online() && car && core::IsPlayerReady()) {
        if (cam::WideScreenOn() && FromOtherSide(car)) return;
        CPed* driver = car->Driver();
        if (driver && Players::Instance().FindByPed(driver)) {
            // The driver's report is authoritative, as for bullets. A second collision
            // computed from the delayed mirror must not apply the same hit twice.
            return;
        }
        if (auto* rp = Players::Instance().FindByPed(ped); rp && driver == FindPlayerPed(0)) {
            static std::map<uint8_t, uint64_t> lastRam;
            if (Net().FriendlyFire() && NowMs() - lastRam[rp->id] >= 500) {
                lastRam[rp->id] = NowMs();
                auto* sourceCar = Vehicles::Instance().EnsureNetworked(car);
                SendDamage(TARGET_PLAYER_BY_PLAYER, rp->id, sourceCar ? sourceCar->netId : 0, WEAPON_RAMMEDBYCAR,
                           std::clamp(impulse, 1.0f, 5000.0f), flag?1:0, 0, ped->GetPosition());
                LOGI("Rammed player %u (impulse %.0f)", rp->id, impulse);
            }
            return;
        }
        if (NetPed* np = Peds::Instance().Find(ped); np && (np->owner != Net().MyId()||np->authorityHeld)) {
            if(np->authorityHeld&&np->owner==Net().MyId()&&driver!=FindPlayerPed(0)&&!np->deadApplied){
                if(NowMs()-np->lastRamMs>500&&SendDamage(TARGET_PED,np->owner,np->netId,WEAPON_RAMMEDBYCAR,std::clamp(impulse,1.0f,5000.0f),0,0,ped->GetPosition(),nullptr,car,true))np->lastRamMs=NowMs();
                return;
            }
            if (driver == FindPlayerPed(0) && !np->deadApplied && NowMs() - np->lastRamMs > 500) {
                auto*source=Vehicles::Instance().EnsureNetworked(car);
                // A rejected source must not consume the impact cooldown or
                // claim a crime/loot hit: the next native contact can retry
                // once this exact vehicle has become available.
                if(!SendDamage(TARGET_PED, np->owner, np->netId, WEAPON_RAMMEDBYCAR, std::clamp(impulse, 1.0f, 5000.0f), 0, 0,
                               ped->GetPosition(),source?source->veh:nullptr))return;
                np->lastRamMs = np->lastHitMs = NowMs();
                Call<0x532010>(np->pedType == PED_TYPE_COP ? CRIME_DAMAGED_COP : CRIME_DAMAGED_PED, (CEntity*)ped, driver);
                LOGI("Rammed ped %04X of player %d (impulse %.0f)", np->netId, np->owner, impulse);
            }
            return;
        }
    }
    g_origKillWithCar(ped, edx, car, impulse, flag);
}

using InflictDamageFn = void(__fastcall*)(CVehicle*, void*, CEntity*, int, float, CVector);
InflictDamageFn g_origInflictDamage = nullptr;

// CVehicle::InflictDamage(damager, weapon, damage, position): bullets, explosions and fire on vehicles
void __fastcall HookInflictDamage(CVehicle* v, void* edx, CEntity* damager, int weapon, float damage, CVector pos) {
    if(TrainingIgnoresContact(v,damager))return;
    auto*sourceVehicle=DamageSourceVehicle(damager,weapon);
    if (Net().Online() && v && core::IsPlayerReady()) {
        if (!IsCollision(weapon) && IsRemotePlayerEntity(damager)) return;
        if (cam::WideScreenOn() && FromOtherSide(damager)) return;
        if(auto*nv=Vehicles::Instance().Find(v);nv&&nv->authorityHeld&&nv->owner==Net().MyId()&&!IsLocalPlayerEntity(damager)){
            SendDamage(TARGET_VEHICLE,nv->owner,nv->netId,weapon,damage,0,0,pos,nullptr,damager,true);return;
        }
        if (NetVehicle* nv = Vehicles::Instance().Find(v); nv && (Vehicles::Instance().IsMirror(*nv)||nv->authorityHeld)) {
            if (!IsCollision(weapon) && IsLocalPlayerEntity(damager))
                SendDamage(TARGET_VEHICLE, nv->owner, nv->netId, weapon, damage, 0, 0, pos,sourceVehicle);
            return;
        }
    }
    g_origInflictDamage(v, edx, damager, weapon, damage, pos);
    if(damage>0&&IsLocalPlayerEntity(damager))RecordStoryEntityDamage(v,Net().MyId(),weapon,sourceVehicle);
    if (Net().Online()) if (auto* nv = Vehicles::Instance().Find(v); nv && !Vehicles::Instance().IsMirror(*nv)) {
        nv->lastChangeMs = NowMs(); nv->lastSendMs = 0; nv->lookCheckMs = 0;
    }
}

using RegisterKillFn = void(__cdecl*)(CPed*, CEntity*, int, bool);
RegisterKillFn g_origRegisterKill = nullptr;

// CEventHandler::RegisterKill(ped, killer, weapon, headshot): a ped died of a hit. This game's ped killed by the other
// player is credited to that player's game and counts here as killed by somebody else (see the top of the file).
void __cdecl HookRegisterKill(CPed* ped, CEntity* killer, int weapon, bool headshot) {
    CPlayerPed* me = FindPlayerPed(0);
    RecordCooperativeGangKill(ped,killer);
    if(Net().Online()&&ped&&killer&&core::IsPlayerReady()){
        CPed*source=killer->IsPed()?(CPed*)killer:DriverOf(killer);
        if(source==me)RecordStoryKill(ped,Net().MyId());
        else if(auto*rp=source?Players::Instance().FindByPed(source):nullptr)RecordStoryKill(ped,rp->id);
    }
    if (Net().Online() && ped && killer && killer->IsPed() && core::IsPlayerReady()) {
        if (RemotePlayer* rp = Players::Instance().FindByPed((CPed*)killer)) {
            if (NetPed* np = Peds::Instance().Find(ped); np && np->owner == Net().MyId())
                SendDamage(KILLED_BY_YOU, rp->id, np->netId, weapon, 1.0f, headshot, 0, ped->GetPosition());
            killer = nullptr;
        }
    }
    g_origRegisterKill(ped, killer, weapon, headshot);
}

struct PendingRamReaction {int handle=-1;uint8_t direction=0;uint64_t since=0;uint32_t afterFrame=0;}g_ramReaction;
uint32_t g_damageFrame=0;
void UpdateRamReaction(){
    ++g_damageFrame;if(g_ramReaction.handle<0)return;
    auto*me=FindPlayerPed(0);
    if(!me||GetPedRef(me)!=g_ramReaction.handle||me->Health()<=0||me->InVehicle()||anim::FindTask(me,208)||anim::FindTask(me,205)){
        g_ramReaction={};return;
    }
    if(g_damageFrame<g_ramReaction.afterFrame||NowMs()-g_ramReaction.since<100)return;
    int direction=g_ramReaction.direction;g_ramReaction={};
    // The native queued event has now had two frames to react. Only a missed
    // mirror contact needs one native fall/get-up task; never preempt that event.
    if(void*task=Call<0x61A5A0,void*>(0x18u)){
        CallMethod<0x678700>(task,direction&3,1000);
        CallMethod<0x681AF0>((uint8_t*)me->Intelligence()+4,task,0,false);
        LOGD("Delayed native ram reaction restored (mirror contact missing)");
    }
}

void HandleDamage(uint8_t from, Reader& r) {
    uint32_t sequence=r.U32();
    uint8_t kind = r.U8(), owner = r.U8();
    uint16_t netId = r.U16();
    int weapon = r.U8();
    float damage = r.F32();
    int piece = r.U8(), dir = r.U8();
    Vec3 p = r.Vec();
    const auto origin=r.U8();const bool victimComputed=origin==1;
    DamageDiagnostic diagnostic(from,sequence,kind,owner,netId,weapon,damage,r.Ok());
    diagnostic.Record(800,!diagnostic.decoded?DD_MALFORMED:diagnostic.packet?DD_OK:DD_CONTEXT);
    bool targetHere = owner == Net().MyId();
    if (kind == TARGET_PED && Peds::Instance().FindOwned(netId)) targetHere = true;
    if (kind == TARGET_VEHICLE) if (auto* nv = Vehicles::Instance().Find(netId); nv && !Vehicles::Instance().IsMirror(*nv)) targetHere = true;
    if (!r.Ok()||origin>1){diagnostic.Record(801,DD_MALFORMED);return;}
    if (!targetHere){diagnostic.Record(801,DD_NOT_AUTHORITY);return;}
    if (!core::IsPlayerReady()){diagnostic.Record(801,DD_NOT_READY);return;}
    if (!std::isfinite(damage) || damage <= 0.0f || damage > 10000.0f){diagnostic.Record(801,DD_DAMAGE_RANGE);return;}
    if (weapon > 58 || piece > 9 || dir > 3 || !std::isfinite(damage)){diagnostic.Record(801,DD_BODY_RANGE);return;}
    if (cam::WideScreenOn() && (kind == TARGET_PLAYER || kind == TARGET_PLAYER_BY_PLAYER)){diagnostic.Record(801,DD_CUTSCENE);return;}
    // Only the server can echo our own action after it aborts a pending lease.
    // Session validates that echo's connection/world; apply it once through the
    // same sequence window rather than losing damage on a quiesced local body.
    const bool localEcho=from==Net().MyId();
    if(victimComputed&&(!localEcho||!HeldDamageCurrent(sequence,kind,netId))){diagnostic.Record(801,DD_NOT_AUTHORITY);return;}
    const RemotePlayer* rp = localEcho?nullptr:Players::Instance().Get(from);
    auto*localAttacker=localEcho?FindPlayerPed(0):nullptr;
    CEntity* attacker = victimComputed?HeldCreator(sequence,kind,netId):localAttacker?static_cast<CEntity*>(localAttacker):rp?rp->ped:nullptr;
    CEntity* target=(kind==TARGET_PLAYER||kind==TARGET_PLAYER_BY_PLAYER)?FindPlayerPed(0):nullptr;
    if(kind==TARGET_PED)target=Peds::Instance().FindOwned(netId);
    if(kind==TARGET_VEHICLE)if(auto*nv=Vehicles::Instance().Find(netId))target=nv->veh;
    if(from>=3){diagnostic.Record(801,DD_SENDER);return;}
    if(!ReadHitEvidence(r,target)){diagnostic.Record(801,DD_EVIDENCE);return;}
    CVehicle*sourceVehicle=nullptr;
    if(victimComputed&&attacker&&attacker->IsVehicle()&&IsVehicleValid((CVehicle*)attacker))sourceVehicle=(CVehicle*)attacker;
    bool hasSource=r.Bool();
    if(hasSource){
        auto sourceId=r.U16(),sourceModel=r.U16();auto sourceGeneration=r.U64();auto sourceTime=r.U32();
        auto sourceRun=r.U64();auto sourcePhase=r.U32(),sourceAssignment=r.U32();
        auto*source=Vehicles::Instance().Find(sourceId);
        StoryRemoteRole role;bool roleMatches=!sourceRun||(CooperativeRemoteRole(from,role)&&role.car==(source?source->veh:nullptr)&&role.run==sourceRun&&role.phase==sourcePhase&&role.assignment==sourceAssignment);
        bool sourceForTarget=kind==TARGET_VEHICLE||(kind==TARGET_PED&&weapon==WEAPON_RAMMEDBYCAR);
        const bool boundSource=sourceForTarget&&source&&Vehicles::Alive(*source)&&source->owner==from&&source->model==sourceModel&&
            sourceGeneration&&Net().EntityGeneration(ENTITY_VEHICLE,sourceId)==sourceGeneration&&roleMatches;
        const bool remoteDriver=boundSource&&rp&&rp->hasState&&NowMs()-rp->stateRecvMs<1500&&
            (kind==TARGET_PED?rp->ClaimsDriver(sourceId,sourceGeneration,source->veh->AreaCode(),NowMs()):
             (rp->ped&&(source->veh->Driver()==rp->ped||CooperativeRemoteController(source->veh)==rp->ped)))&&
            (int32_t)(rp->lastStateTime-sourceTime)<=1500&&(int32_t)(sourceTime-rp->lastStateTime)<=1500;
        const auto localClock=(uint32_t)NowMs();
        const bool localDriver=boundSource&&localAttacker&&IsPedValid(localAttacker)&&
            ((localAttacker->VehicleIfIn()==source->veh&&source->veh->Driver()==localAttacker)||
             (kind==TARGET_VEHICLE&&CooperativeRemoteController(source->veh)==localAttacker))&&
            (int32_t)(localClock-sourceTime)<=1500&&(int32_t)(sourceTime-localClock)<=1500;
        if(remoteDriver||localDriver)
            sourceVehicle=source->veh;
        diagnostic.Source(sourceId,sourceGeneration,sourceVehicle!=nullptr);
    }
    if(!r.Ok()||r.Remaining()){diagnostic.Record(801,DD_MALFORMED);return;}
    if(!damageSeen[from].Accept(sequence)){diagnostic.Record(801,DD_DUPLICATE);return;}
    if(victimComputed)heldDamageCreators.erase(sequence);
    if(TrainingIgnoresContact(target,attacker)){diagnostic.Record(801,DD_TRAINING);return;}
    if (kind == TARGET_PED) {
        if (CPed* ped = Peds::Instance().FindOwned(netId)) {
            if(weapon==WEAPON_RAMMEDBYCAR&&!sourceVehicle){diagnostic.Record(801,DD_SOURCE_REQUIRED);return;}
            {
                KeepWanted keep;
                // Native NPC/environment hits retain ordinary invulnerability
                // and credit. Only a real player report receives player proofs.
                AsPlayersHit player(ped,!victimComputed);
                bool fire=false;if(!victimComputed&&ped->Health()>0&&CallMethod<0x5448B0,bool>(ped,weapon,&fire))RecordStoryDamage(ped,from,weapon);
                // The actual vehicle reference can arrive before its remote
                // driver's native seat is materialised. Losing that seat must
                // not discard a real bicycle/motorcycle impact.
                // RAM always uses the captured, validated source. A missing
                // source must never bind to a car entered after the impact.
                CVehicle* car = sourceVehicle;
                auto* me = FindPlayerPed(0);
                bool unloaded = (ped->AreaCode()!=13&&ped->AreaCode() != me->AreaCode()) || (ped->GetPosition() - me->GetPosition()).Length2D() > 120;
                if (unloaded) {
                    uint32_t proof = CPed::PF_INVULNERABLE;
                    if (weapon <= WEAPON_LAST_MELEE) proof |= CPed::PF_MELEE_PROOF;
                    else if (IsCollision(weapon)) proof |= CPed::PF_COLLISION_PROOF;
                    else if (weapon == WEAPON_EXPLOSION) proof |= CPed::PF_EXPLOSION_PROOF;
                    else if (weapon == 37 || weapon == 18) proof |= CPed::PF_FIRE_PROOF;
                    else proof |= CPed::PF_BULLET_PROOF;
                    if (ped->PhysicalFlags() & proof){diagnostic.Record(801,DD_PROOF);return;}
                    // Event tasks are not processed outside the owner's loaded world. Use
                    // the native calculator directly once; it applies armour and health now.
                    alignas(4) uint8_t calculator[0x14] = {}, response[0x0C] = {};
                    float was = ped->Health();
                    DamageNativeObservation observation(diagnostic,ped,was);
                    CallMethod<0x4AD3F0>(calculator, attacker, damage, weapon, std::clamp(piece, 0, 9), false);
                    CallMethod<0x4B5AC0>(calculator, ped, response, false);
                    observation.Complete();
                    if (was > 0 && ped->Health() <= 0) HookRegisterKill(ped, attacker, weapon, piece == 9);
                } else if (weapon == WEAPON_RAMMEDBYCAR && car){DamageNativeObservation observation(diagnostic,ped);g_origKillWithCar(ped, nullptr, car, damage, 0);observation.Complete();}
                else if (weapon != WEAPON_RAMMEDBYCAR){DamageNativeObservation observation(diagnostic,ped);const bool result=g_origGenerateDamage(ped, attacker, weapon, (int)damage, piece, dir);observation.Complete(true,result);}
            }
            LOGI("Player %d hit our ped %04X: weapon %d, damage %.0f, health now %.0f", from, netId, weapon, damage, ped->Health());
            if (!victimComputed&&from!=Net().MyId()&&ped->Health() <= 0.0f) {  // remote player's loot falls in the killer's game
                Cmd(0x087E).I(GetPedRef(ped)).I(0).Run();  // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD false
                ped->Money() = 0;
            }
        }else diagnostic.Record(801,DD_NATIVE_MISSING);
    } else if (kind == TARGET_PLAYER_BY_PLAYER) {
        if (!Net().FriendlyFire()){diagnostic.Record(801,DD_FRIENDLY_FIRE);return;}
        {
            KeepWanted keep;
            auto* car = rp && rp->ped && rp->ped->InVehicle() ? rp->ped->Vehicle() : nullptr;
            if (IsCollision(weapon) && netId) if (auto* nv = Vehicles::Instance().Find(netId)) car = nv->veh;
            if (IsCollision(weapon) && car) {
                auto* me = FindPlayerPed(0);
                DamageNativeObservation observation(diagnostic,me);g_origKillWithCar(me,nullptr,car,damage,piece!=0);observation.Complete();
                if(!me->InVehicle()&&me->Health()>0&&!anim::FindTask(me,208)&&!anim::FindTask(me,205))
                    g_ramReaction={GetPedRef(me),(uint8_t)dir,NowMs(),g_damageFrame+2};

            }
            else {auto*me=FindPlayerPed(0);DamageNativeObservation observation(diagnostic,me);const bool result=g_origGenerateDamage(me,attacker,weapon,(int)damage,piece,dir);observation.Complete(true,result);}
        }
        LOGI("Player %d hit us: weapon %d, damage %.0f, health now %.0f", from, weapon, damage, FindPlayerPed(0)->Health());
    } else if (kind == TARGET_PLAYER) {
        int h = Peds::Instance().HandleOf(netId);
        auto*me=FindPlayerPed(0);auto*source=h>=0?GetPed(h):nullptr;DamageNativeObservation observation(diagnostic,me);
        const bool result=g_origGenerateDamage(me,source,weapon,(int)damage,piece,dir);observation.Complete(true,result);
        LOGI("An NPC %04X of player %d hit us: weapon %d, damage %.0f", netId, from, weapon, damage);
    } else if (kind == TARGET_VEHICLE) {
        NetVehicle* nv = Vehicles::Instance().Find(netId);
        if (nv && !Vehicles::Instance().IsMirror(*nv) && IsVehicleValid(nv->veh)) {
            KeepWanted keep;
            AsPlayersHit player(nv->veh,!victimComputed);
            DamageNativeObservation observation(diagnostic,nv->veh);
            g_origInflictDamage(nv->veh, nullptr, attacker, weapon, damage, CVector{p.x, p.y, p.z});
            observation.Complete();
            if(!victimComputed)RecordStoryEntityDamage(nv->veh,from,weapon,sourceVehicle);
            nv->lastChangeMs = NowMs(); nv->lastSendMs = 0; nv->lookCheckMs = 0;
        }else diagnostic.Record(801,DD_NATIVE_MISSING);
    } else if (kind == KILLED_BY_YOU) {
        diagnostic.Record(805,DD_OUTCOME_UNAVAILABLE);
        int h = Peds::Instance().HandleOf(netId);  // its copy here: a civilian, it counts as what its owner has
        if (CPed* ped = h >= 0 ? GetPed(h) : nullptr) {
            NetPed* np = Peds::Instance().Find(ped);
            int32_t type = ped->PedType();
            if (np) ped->PedType() = np->pedType;
            HookRegisterKill(ped, FindPlayerPed(0), weapon, piece != 0);
            ped->PedType() = type;
            diagnostic.Record(805,DD_KILL_CREDIT_RETURNED);
        }
        LOGI("We killed ped %04X of player %d: weapon %d", netId, from, weapon);
    }else diagnostic.Record(801,DD_UNSUPPORTED);
}

class Damage : public Module {
public:
    const char* Name() const override { return "Damage"; }
    void OnPostProcess() override {if(Net().Online()&&core::IsPlayerReady())UpdateRamReaction();}
    void OnPeerLeave(uint8_t id)override{if(id<3)damageSeen[id]={};}
    void OnPeerResync(uint8_t id)override{OnPeerLeave(id);}
    void OnSessionEnd() override {g_ramReaction={};for(auto&w:damageSeen)w={};heldDamageCreators.clear();damageSequence=0;}
    void OnGameShutdown() override {g_ramReaction={};for(auto&w:damageSeen)w={};}
    void OnInstallHooks() override {
        mem::HookFn(0x73A530, &HookGenerateDamageEvent, &g_origGenerateDamage, "CWeapon::GenerateDamageEvent");
        mem::HookFn(0x6D7C90, &HookInflictDamage, &g_origInflictDamage, "CVehicle::InflictDamage");
        mem::HookFn(0x5F0360, &HookKillPedWithCar, &g_origKillWithCar, "CPed::KillPedWithCar");
        mem::HookFn(0x4B9340, &HookRegisterKill, &g_origRegisterKill, "CEventHandler::RegisterKill");
        Net().SetHandler(proto::G_DAMAGE, &HandleDamage);
    }
};

Damage g_damage;
struct Reg {
    Reg() { core::RegisterModule(&g_damage); }
} g_reg;
}  // namespace

}  // namespace coop
