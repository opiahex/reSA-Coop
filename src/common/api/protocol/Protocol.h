#pragma once
// SA:COOP network protocol. See docs/PROTOCOL.md.
//
// Every ENet packet starts with a one-byte message id.
//  * ids < 64  — service messages, handled by the server itself;
//  * ids >= 64 — game messages: the client sends [id][payload], the server relays them to the other
//    players as [id][u8 senderId][u32 connection token][payload]. The server never parses game payloads.
#include <cstdint>
#include <string>
#include <vector>

#include "common/lib/buffer/Buffer.h"

namespace coop::proto {

constexpr int kGamePlayers = 3;
constexpr int kMaxPlayers = kGamePlayers;  // one host and two partners, supported by both client and server
constexpr uint8_t kServerId = 255;
constexpr size_t kMaxChatLen = 160;
constexpr size_t kRelayHeader = 6;
constexpr size_t kWorldChunk = 16 * 1024;

// Stable partner number (0 or 1) even when another partner disconnects or is still loading.
constexpr int PartnerIndex(uint8_t player, uint8_t host) { return player > host ? player - 1 : player; }

enum Channel : uint8_t {
    CH_RELIABLE = 0,  // ordered, reliable: events
    CH_STATE = 1,     // unreliable sequenced: positions/states
    CH_VOICE = 2,     // unreliable: voice frames
    CH_BULK = 3,      // ordered, reliable: file transfers (save game)
    // Server -> clients: separate sequenced streams so a newer state of one sender cannot discard another's
    // delayed state. Clients still upload all their state on CH_STATE.
    CH_STATE_PLAYER1 = 4,
    CH_STATE_PLAYER2 = 5,
    CH_COUNT = 6,
};

constexpr uint8_t RelayedChannel(uint8_t channel, uint8_t sender) {
    return channel == CH_STATE && sender > 0 ? (uint8_t)(CH_STATE_PLAYER1 + sender - 1) : channel;
}

enum MsgId : uint8_t {
    // ---- client -> server ----
    C_HELLO = 1,
    C_CHAT = 2,              // str text (may be a /command)
    C_PROFILE_SAVE = 3,      // bytes profile (opaque for the server)
    C_WORLD_BEGIN = 4,       // host uploads its save: u32 size, u32 crc, str label
    C_WORLD_CHUNK = 5,       // u32 offset, bytes
    C_WORLD_END = 6,
    C_WORLD_REQUEST = 7,     // ask the server to send the current world save
    C_STATUS = 8,            // u8 ClientStatus
    C_WORLD_KEEP = 9,        // host: a fresh snapshot is impossible now (mission), send the stored world
    C_WORLD_NEWGAME = 10,    // host: it plays a new game's prologue (no save of it can exist yet)
    C_WORLD_FOLLOW = 11,     // host: the prologue is over but cannot be saved yet: partners follow in a new game

    // ---- server -> client ----
    S_WELCOME = 20,          // see Welcome
    S_REJECT = 21,           // str reason, str reasonEn (the same in English; empty when there is nothing to translate)
    S_PLAYER_JOIN = 22,      // u8 id, str name, u8 isHost
    S_PLAYER_LEAVE = 23,     // u8 id, u8 reason
    S_CHAT = 24,             // u8 fromId (255 = server), u32 rgba, str text, str textEn (the server's own texts)
    S_PROFILE = 25,          // bytes profile (empty when the player is new)
    S_WORLD_BEGIN = 26,      // u32 size, u32 crc, str label
    S_WORLD_CHUNK = 27,      // u32 offset, bytes
    S_WORLD_END = 28,
    S_WORLD_NONE = 29,       // the host has not uploaded a save yet
    S_HOST_CHANGED = 30,     // u8 newHostId
    S_PINGS = 31,            // u8 count, {u8 id, u16 ping}
    S_KICKED = 32,           // str reason, str reasonEn
    S_WORLD_WANTED = 33,     // to the host: a partner needs the world, please upload a fresh snapshot
    S_INVITE = 34,           // str invite code for internet play (when the server learned its public address)
    S_WORLD_NEWGAME = 35,    // to a partner: start a new game and join the host where the prologue left it
    S_PROFILE_SAVED = 37,    // u32 CRC: profile persisted successfully
    S_WORLD_WAIT = 36,       // to a partner: the host plays a new game's prologue, keep waiting

    // ---- relayed game messages (client <-> client) ----
    G_FIRST = 64,
    G_PLAYER_ONFOOT = 64,     // unreliable
    G_PLAYER_DRIVER = 65,     // unreliable
    G_PLAYER_PASSENGER = 66,  // unreliable
    G_PLAYER_AIM = 67,        // unreliable
    G_PLAYER_SPAWN = 68,      // reliable: (re)spawn, model, interior
    G_PLAYER_DEATH = 69,
    G_PLAYER_LOOK = 70,       // reliable: clothes, fat, muscle
    G_PLAYER_STATS = 71,      // reliable: stats affecting the remote ped (skills, stamina, fight style)
    G_PLAYER_WEAPONS = 72,    // reliable: weapon inventory
    G_BULLET = 73,            // reliable: actual shooter ray/impact (ShotVisual), visual FX only
    G_DAMAGE = 74,            // damage event addressed to the owner of the victim
    G_EXPLOSION = 75,         // reliable: a blast went off (creator kind and id, type, position...), see Fire.cpp
    G_PROJECTILE = 76,        // reliable: a grenade, a rocket... was thrown (creator, weapon, position, speed, target)
    G_PLAYER_ENTER_CAR = 77,
    G_PLAYER_EXIT_CAR = 78,
    G_PLAYER_ANIM = 79,       // reliable: one-shot animations (eating, drinking, gestures)

    G_VEH_CREATE = 80,
    G_VEH_REMOVE = 81,
    G_VEH_SYNC = 82,          // unreliable
    G_VEH_OWNER = 83,         // reliable: u16 vehicle, u8 owner, u32 sender claim time
    G_VEH_STATUS = 84,        // reliable: damage, colours, mods, locks, siren
    G_VEH_HANDOVER = 85,      // reliable: u16 netId, u8 to: the owner gives its car away (its peds go before it)
    G_VEH_SCRIPT_FREE = 86,   // reliable: u16 netId: a script of this game let go of its car (a mirror here)
    G_PED_CREATE = 90,
    G_PED_REMOVE = 91,
    G_PED_SYNC = 92,          // unreliable
    G_PED_OWNER = 93,         // reliable: u16 netId, u8 to: the owner gives its ped away (the mirror becomes its own)
    G_PED_STATUS = 94,        // reliable: death, weapon, task hints
    G_RESYNC = 95,            // reliable: world restarted + stable player-car ids (WorldResync); reannounce other entities
    G_OBJ_CREATE = 100,       // reliable: an object of the host's mission
    G_OBJ_REMOVE = 101,       // reliable: it is gone (or broke)
    G_OBJ_SYNC = 102,         // unreliable: it moved
    G_PICKUP_EVENT = 103,     // reliable: a pickup was collected
    G_PICKUP_CREATE = 104,    // reliable: a pickup appeared (loot, the host's mission)
    G_OBJ_DAMAGE = 105,       // reliable: the partner hit a mirror object, its owner applies it
    G_FIRE = 106,             // reliable: a fire started / went out
    G_ZONE = 107,             // reliable: gang densities of a territory
    G_CHEAT = 108,            // reliable: a cheat that changes the shared world
    G_ENEX = 109,             // reliable: entrances the host's scripts opened or closed

    G_WORLD_STATE = 110,      // time, weather, gravity... (host -> all)
    G_WANTED = 111,

    G_MISSION_START = 120,
    G_MISSION_END = 121,
    G_TEXT = 122,             // subtitles / help / big messages
    G_BLIP = 123,             // add/update/remove radar blip
    G_MARKER = 124,           // 3d markers, checkpoints, script spheres
    G_ONSCREEN = 125,         // timers and counters
    G_FADE = 126,
    G_CUTSCENE = 127,         // load/start/finish/skip
    G_SCRIPT_VARS = 128,      // story progress (script global variables)
    G_MISSION_REWARD = 129,
    G_TELEPORT = 130,         // teleport request/command
    G_GAME_EVENT = 131,       // mission audio load/play/clear (targeted), or fixed script-camera state
    G_MISSION_WAIT = 132,     // the host stands in a mission marker and waits for the partner
    G_WAYPOINT = 133,         // reliable: the player's map waypoint (set or cleared)
    G_PROPERTY = 134,         // reliable: the player bought a property (its index in main.scm)
    G_HOST_SAVED = 135,       // reliable: the host saved its game, the partners save their profiles
    G_MISSION_WEAPON = 136,   // reliable: u8 op (1 give, 2 take), u8 weapon, i32 ammo (CJ's count after the change):
                              // the host's mission armed its CJ
    G_CHECKPOINTS = 137,      // reliable: the host's script checkpoints and spheres (u8 n, records)
    G_WORLD_OBJ = 138,        // a map object broke or moved (u16 model, origin, stage, flags, weapon[, pose, speeds]);
                              // reliable for changes, unreliable while it moves
    G_GARAGE = 139,           // reliable: u8 op, u8 garage: the partner's cars into the host's safe house store
                              // (u8 n, u16 net ids), the host has them (same), bring the stored ones out
    G_VEH_CLEAR = 140,        // reliable: u8 n, u16 net ids: the host's mission cleared an area (CLEAR_AREA), the
                              // owners delete these empty cars of theirs there
    G_WATER_CANNON = 141,     // unreliable: u16 vehicle, the jet's offset from it and speed (a fire truck shoots water)
    G_TRAIN = 142,            // a train of the sender's game (u8 op, u16 id; state: kind, track, direction, rails, speed,
                              // pos), unreliable; gone: reliable

    G_PLAYER_CARJACK = 143,   // reliable: u16 vehicle, u8 victim, u8 door, u32 sender time
    G_VEH_RECENT = 144,       // reliable: u8 count (max 3), u16 vehicle ids most recently used by sender
    G_SCRIPT_SOUND = 145,     // reliable: shared script sound / native movie soundtrack state
    G_RADIO = 146,            // reserved: shared radio disabled; each player uses native local radio
    G_PLAYER_EXTRAS = 147,    // reliable: held props, phone, inventory, host-authorised reconnect restore
    G_SCRIPT_LOCK = 148, // reliable host story lock independent of driver ownership
    G_STORY_WARP = 149, // reliable scripted CJ relocation, shared by the story group
    G_GARAGE_DOOR = 150, // reliable shop door state; authority chosen among all three players
    G_PROP = 151,            // reliable: held/attached/loose object, namespaced by sender
    G_PED_SPEECH = 152,      // reliable: owned ped/player speech identity and local bank/sound
    G_STORY_PICKUPS = 153,   // reliable: authoritative quest-pickup set, including removal and retries
    G_STORY_CONTACTS = 154,  // reliable: host story contact points; retire stale mission-start markers

    G_SCRIPT_VEH_REMOVE = 155, // reliable: host story explicitly deletes a vehicle, regardless of current driver

    G_SYNC_AUDIT = 156, // bounded diagnostic owner manifest; never changes authority
    G_ENTITY_REQUEST = 157, // reliable request for one authoritative entity announcement
    G_VEH_CONTACT = 158, // reliable, once-resolved native cross-owner collision impulse
    G_ACTIVITY_STATE = 159, // reliable: readiness, population availability and private school activity
    G_AUDIO_RECEIPT = 160, // reliable: guest slot/token/event and playback milestone; diagnostics only
    G_MISSION_STATE = 161, // reliable periodic mission revision + active/name; no script memory copied
    G_SEAT_ASSIGN = 162, // reliable vehicle-owner decision for simultaneous player seat claims
    G_TYRE_HIT = 163, // reliable native bullet wheel hit; the current vehicle owner punctures once
    G_SIM_FILES = 164, // diagnostic simulation file digests; no file contents are transferred
    G_OBJ_CARRY = 165, // host arbitration and versioned pose of one shared mission object
    G_STORY_ACTION = 166, // native guest photo evidence and cooperative actor conditions

    G_STORY_STAGE = 169, // native mission-stage dependency readiness
    G_INTERACTION = 170, // exclusive native activity leases
    G_STEALTH = 171, // owner validates a real native guest knife action
    G_STORY_COOP = 168, // mission roles, run snapshots and once-paid reward receipts
    G_PED_ACTION = 167, // reliable revisioned NPC action/sequence transition

    G_VEH_HORN = 172, // reliable native horn edges and active heartbeat
    G_STORY_EQUIPMENT = 173, // phase-scoped native special equipment
    G_NPC_CARJACK = 174, // authoritative NPC pull-out -> victim's native seat exit
    G_NPC_CAR_DOOR = 175, // NPC-owned native door animation, including guest-owned vehicles
    G_HELI_SHOT = 176, // authoritative helicopter gun round, generation + sequence
    G_STORY_WORLD = 177, // host map/IPL edits and shared campaign counters
    G_POLICE_RULES = 178, // nearby host gang-war and story police policy
    G_VOICE = 200,            // unreliable: voice frame
    G_LAST = 255,
};

inline bool IsRelayed(uint8_t id) { return id >= G_FIRST; }

enum ClientStatus : uint8_t {
    STATUS_MENU = 0,
    STATUS_LOADING = 1,
    STATUS_INGAME = 2,
    STATUS_PAUSED = 3,
};

enum LeaveReason : uint8_t { LEAVE_QUIT = 0, LEAVE_TIMEOUT = 1, LEAVE_KICKED = 2 };

struct Hello {
    uint32_t protocol = 0;
    std::string version;
    std::string name;
    std::string password;
    bool wantsHost = false;
    uint32_t exeSize = 0;

    void Write(Writer& w) const {
        w.U32(protocol);
        w.Str(version, 32);
        w.Str(name, 64);
        w.Str(password, 64);
        w.Bool(wantsHost);
        w.U32(exeSize);
    }
    bool Read(Reader& r) {
        protocol = r.U32();
        version = r.Str(32);
        name = r.Str(64);
        password = r.Str(64);
        wantsHost = r.Bool();
        exeSize = r.U32();
        return r.Ok();
    }
};

struct PlayerEntry {
    uint8_t id = 0;
    std::string name;
    bool isHost = false;
    uint32_t incarnation=0;
};

struct Welcome {
    uint8_t yourId = 0;
    uint8_t hostId = 0;
    std::string serverName;
    std::string motd, motdEn;
    bool worldAvailable = false;
    bool friendlyFire = false;  // the players can hurt each other (the host chose it creating the game)
    std::vector<PlayerEntry> players;  // already connected players (excluding you)

    void Write(Writer& w) const {
        w.U8(yourId);
        w.U8(hostId);
        w.Str(serverName, 64);
        w.Str(motd, 512);
        w.Bool(worldAvailable);
        w.U8((uint8_t)players.size());
        for (auto& p : players) {
            w.U8(p.id);
            w.Str(p.name, 64);
            w.Bool(p.isHost);
            w.U32(p.incarnation);
        }
        w.Str(motdEn, 512);
        w.Bool(friendlyFire);
    }
    bool Read(Reader& r) {
        yourId = r.U8();
        hostId = r.U8();
        serverName = r.Str(64);
        motd = r.Str(512);
        worldAvailable = r.Bool();
        uint8_t n = r.U8();
        players.clear();
        for (uint8_t i = 0; i < n && r.Ok(); ++i) {
            PlayerEntry p;
            p.id = r.U8();
            p.name = r.Str(64);
            p.isHost = r.Bool();
            p.incarnation=r.U32();
            players.push_back(p);
        }
        motdEn = r.Str(512);
        friendlyFire = r.Bool();
        return r.Ok();
    }
};

}  // namespace coop::proto
