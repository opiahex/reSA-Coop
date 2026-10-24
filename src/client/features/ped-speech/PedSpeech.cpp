// What people say outside missions and cutscenes: a passer-by's remark, two peds chatting, a cop's shout, a scream
// when shots ring out, CJ's own lines. A ped picks a line of its voice at random, and only within 40 m of its game's
// camera, so each game had its own lines and the other's copies stayed silent. The game of the ped's owner reports the
// line its ped starts (sound bank and sound, CAEPedSpeechAudioEntity::LoadAndPlaySpeech), and the other game plays that
// very line on its copy of the ped or of the player. The copies say nothing of their own: what makes the real one
// talk happens in its owner's game.
#include <algorithm>
#include <cmath>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kSlots = 0xB61C38;      // CAEPedSpeechAudioEntity::s_PedSpeechSlots, 6 x 0x1C (the last: players)
constexpr int kPedSlots = 5;
constexpr int kSpeechOffset = 0x294;        // CPed::m_pedSpeech
constexpr float kHeardWithin = 40.0f;       // CAEPedSpeechAudioEntity::AddSayEvent: no farther from the camera
enum : uint8_t { BY_PLAYER = 0, BY_PED = 1 };
enum : int16_t { SLOT_FREE = 0, SLOT_RESERVED = 4 };

int16_t SlotStatus(int slot) { return Ref<int16_t>(kSlots + (uintptr_t)slot * 0x1C); }

// the copy here of another game's ped or player
bool IsCopy(CEntity* e) {
    if (!e || !e->IsPed()) return false;
    if (Players::Instance().FindByPed((CPed*)e)) return true;
    NetPed* np = Peds::Instance().Find((CPed*)e);
    return np && np->owner != Net().MyId();
}

bool g_injecting = false;  // this module starts a line on a copy

using SayFn = int16_t(__fastcall*)(void*, void*, int, int16_t, uint32_t, float, uint8_t, uint8_t, uint8_t);
SayFn g_origSay = nullptr;
int16_t __fastcall HookAddSayEvent(void* s, void* edx, int event, int16_t ctx, uint32_t delay, float probability,
                                   uint8_t overrideSilence, uint8_t forceAudible, uint8_t frontEnd) {
    if (Net().Online() && IsCopy(Field<CEntity*>(s, 0x04))) return -1;  // CAEAudioEntity::m_pEntity
    return g_origSay(s, edx, event, ctx, delay, probability, overrideSilence, forceAudible, frontEnd);
}

using PlayFn = void(__fastcall*)(void*, void*, uint32_t);
PlayFn g_origPlay = nullptr;
void __fastcall HookLoadAndPlaySpeech(void* s, void* edx, uint32_t delay) {
    int16_t slot = Field<int16_t>(s, 0xA8);  // m_PedSpeechSlotID
    // it loads only into a free or reserved slot
    bool starts = slot >= 0 && slot <= kPedSlots && (SlotStatus(slot) == SLOT_FREE || SlotStatus(slot) == SLOT_RESERVED);
    g_origPlay(s, edx, delay);
    if (g_injecting || !starts || !Net().Online() || !core::IsPlayerReady()) return;
    auto* e = Field<CEntity*>(s, 0x04);  // CAEAudioEntity::m_pEntity
    if (!e || !e->IsPed()) return;       // the pedless speech of police boats and helicopters
    uint8_t by;
    uint16_t id = 0;
    if (e == FindPlayerPed(0)) {
        by = BY_PLAYER;
    } else if (NetPed* np = Peds::Instance().Find((CPed*)e); np && np->owner == Net().MyId()) {
        by = BY_PED;
        id = np->netId;
    } else {
        return;  // a ped the other game has no copy of
    }
    Writer w(proto::G_PED_SPEECH);
    w.U8(by);
    if(by==BY_PED)w.Entity(ENTITY_PED,id);else w.U16(id);
    w.I16(Field<int16_t>(s, 0xB0));  // m_LastGCtx
    w.I16(Field<int16_t>(s, 0xA6));  // m_BankID
    w.I16(Field<int16_t>(s, 0xA4));  // m_SoundID
    w.U32(delay);
    w.F32(Field<float>(s, 0xAC));    // m_EventVolume
    w.U8(Field<uint8_t>(s, 0x9C));   // m_IsForcedAudible
    Net().Send(proto::CH_RELIABLE, w);
}

// a free slot of the peds' ones, in turn as CAEPedSpeechAudioEntity::CanWePlayGlobalSpeechContext takes them, without
// its pause after a line of the same context: the owner's game already kept that
int16_t FreeSlot() {
    int16_t& next = Ref<int16_t>(0xB61414);  // CAEPedSpeechAudioEntity::s_NextSpeechSlot
    for (int i = 0; i < kPedSlots; ++i) {
        int slot = (next + i) % kPedSlots;
        if (SlotStatus(slot) != SLOT_FREE) continue;
        next = (int16_t)((slot + 1) % kPedSlots);
        return (int16_t)slot;
    }
    return -1;
}

// the end of CAEPedSpeechAudioEntity::AddSayEvent (0x4E6550) with the owner's line in place of a random one
bool Say(CPed* ped, int16_t ctx, int16_t bank, int16_t sound, uint32_t delay, float volume, bool forced) {
    void* s = (uint8_t*)ped + kSpeechOffset;
    if (!Field<uint8_t>(s, 0x90) || Ref<uint8_t>(0xB613E6) || cam::WideScreenOn()) return false;  // m_IsInitialized, s_bAllSpeechDisabled
    if ((ped->GetPosition() - cam::Source()).Length() >= kHeardWithin) return false;
    if (Field<uint8_t>(s, 0x98)) CallMethod<0x4E3FB0>(s);  // m_IsPlayingSpeech: StopCurrentSpeech, its owner's moved on
    int16_t slot = FreeSlot();
    if (slot < 0) return false;
    // Scripted speech uses context -1 and has no global context timer.
    if(ctx>=0)CallMethod<0x4E4A20>(s, ctx);
    CVehicle* v = ped->VehicleIfIn();
    Field<uint8_t>(s, 0x9C) = forced;                                      // m_IsForcedAudible
    Field<uint8_t>(s, 0x9B) = v && v == FindPlayerVehicle(0);              // m_IsFrontend: in our car, as the game does
    Field<float>(s, 0xAC) = volume;                                        // m_EventVolume
    Field<uint8_t>(s, 0x98) = 1;                                           // m_IsPlayingSpeech
    Field<int16_t>(s, 0xB0) = ctx;                                         // m_LastGCtx
    Field<int16_t>(s, 0xA8) = slot;                                        // m_PedSpeechSlotID
    Field<int16_t>(s, 0xA4) = sound;                                       // m_SoundID
    Field<int16_t>(s, 0xA6) = bank;                                        // m_BankID
    g_injecting = true;
    CallMethod<0x4E4840>(s, delay);  // LoadAndPlaySpeech
    g_injecting = false;
    return true;
}

void Handle(uint8_t from, Reader& r) {
    uint8_t by = r.U8();
    uint16_t id = r.U16();
    int16_t ctx = r.I16(), bank = r.I16(), sound = r.I16();
    uint32_t delay = r.U32();
    float volume = r.F32();
    bool forced = r.U8() != 0;
    if (!r.Ok() || !core::IsPlayerReady() || bank < 0 || sound < 0 || !std::isfinite(volume) || ctx < -1 || ctx >= 360 || delay > 10000) return;
    // CAEMP3BankLoader::LoadSound (0x4E07A0) lets a bank one past its lookup table through (it compares with >) and
    // reads that garbage: what the banks here do not have (other audio files) is not played
    auto* loader = Ref<uint8_t*>(0xB5F8B8 + 0xD98);  // AEAudioHardware.m_pMP3BankLoader
    if (!loader || bank >= Ref<int16_t>((uintptr_t)loader + 0x0E) || sound >= 400) return;  // m_BankLkupCnt, sounds a bank holds
    CPed* ped = nullptr;
    if (by == BY_PLAYER) {
        RemotePlayer* rp = Players::Instance().Get(from);
        ped = rp ? rp->ped : nullptr;
    } else if(by==BY_PED) {
        int h = Peds::Instance().HandleOf(id);
        ped = h >= 0 ? GetPed(h) : nullptr;
        auto*np=ped?Peds::Instance().Find(ped):nullptr;
        if(!np||np->owner!=from)return;
    }
    if (!ped || !IsCopy(ped)) return;
    bool said = Say(ped, ctx, bank, sound, std::min<uint32_t>(delay, 2000), volume, forced);
    LOGD("Speech of player %d's %s %04X: context %d, bank %d, sound %d%s", from, by == BY_PLAYER ? "player" : "ped",
         id, ctx, bank, sound, said ? "" : " (not heard here)");
}

class PedSpeech : public Module {
public:
    const char* Name() const override { return "PedSpeech"; }

    void OnInstallHooks() override {
        mem::HookFn(0x4E6550, &HookAddSayEvent, &g_origSay, "CAEPedSpeechAudioEntity::AddSayEvent");
        mem::HookFn(0x4E4840, &HookLoadAndPlaySpeech, &g_origPlay, "CAEPedSpeechAudioEntity::LoadAndPlaySpeech");
        Net().SetHandler(proto::G_PED_SPEECH, [](uint8_t from, Reader& r) { Handle(from, r); });
    }

    static PedSpeech& Instance() {
        static PedSpeech m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&PedSpeech::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
