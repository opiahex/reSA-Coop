// Gang territories follow the host. The host's game runs the gang wars and the story; whenever a territory's gangs,
// colour or radar mode (flashing while it is attacked) change there, the partner's copy of that zone info changes too.
// GangWarCoop forwards confirmed kills and borrows the participating player's
// native context on the host. Guests never pick a second set of waves or winners.
#include <algorithm>
#include <cstring>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "entities/session/Flow.h"
#include "common/lib/util/Util.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kZoneInfos = 0xBA1DF0;  // CTheZones::ZoneInfoArray[380], 0x11 bytes each
constexpr uintptr_t kZoneInfoCount = 0xBA1DE8;  // int16 CTheZones::TotalNumberOfZoneInfos
constexpr int kMaxZones = 380;

// a zone info without its drug dealers' counter (+0x0A, each game bumps it at random): gang densities (+0..9),
// colour (+0x0B..0x0E), population type and radar mode (+0x0F), races (+0x10)
struct ZoneBytes {
    uint8_t b[16];
    bool operator==(const ZoneBytes& o) const { return !memcmp(b, o.b, sizeof(b)); }
};

int Count() { return std::clamp<int>(Ref<int16_t>(kZoneInfoCount), 0, kMaxZones); }
ZoneBytes ReadZone(int i) {
    auto z = (const uint8_t*)(kZoneInfos + (uintptr_t)i * 0x11);
    ZoneBytes r;
    memcpy(r.b, z, 10);
    memcpy(r.b + 10, z + 0x0B, 6);
    return r;
}
void WriteZone(int i, const ZoneBytes& r) {
    auto z = (uint8_t*)(kZoneInfos + (uintptr_t)i * 0x11);
    memcpy(z, r.b, 10);
    memcpy(z + 0x0B, r.b + 10, 6);
}

std::vector<ZoneBytes> g_sent;  // host: what the partner has
uint64_t g_scanMs = 0;

void Send(const std::vector<int>& zones) {
    for (size_t at = 0; at < zones.size(); at += 100) {
        size_t n = std::min<size_t>(100, zones.size() - at);
        Writer w(proto::G_ZONE);
        w.U16((uint16_t)n);
        for (size_t k = at; k < at + n; ++k) {
            w.U16((uint16_t)zones[k]);
            w.Raw(ReadZone(zones[k]).b, 16);
        }
        Net().Send(proto::CH_RELIABLE, w);
    }
}

using VoidFn = void(__cdecl*)();
VoidFn g_origOffensive = nullptr, g_origDefensive = nullptr;
bool PartnerGame() { return Net().Online() && !Net().IsHost(); }
void __cdecl HookStartOffensiveGangWar() {
    if (!PartnerGame()) g_origOffensive();
}
void __cdecl HookStartDefensiveGangWar() {
    if (!PartnerGame()) g_origDefensive();
}

class Zones : public Module {
public:
    const char* Name() const override { return "Zones"; }

    void OnInstallHooks() override {
        // CGangWars::Update: the provocation and the attack countdown start the wars
        mem::RedirectCallFn(0x4466DC, &HookStartOffensiveGangWar, &g_origOffensive);
        mem::RedirectCallFn(0x446E46, &HookStartDefensiveGangWar, &g_origDefensive);
        Net().SetHandler(proto::G_ZONE, [](uint8_t from, Reader& r) {
            int n = r.U16();
            if (from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld()) return;
            for (int k = 0; k < n; ++k) {
                int i = r.U16();
                ZoneBytes z;
                r.Raw(z.b, 16);
                if (!r.Ok() || i >= Count()) return;
                WriteZone(i, z);
            }
            LOGD("Territories: %d zone infos from the host", n);
        });
    }
    void OnSessionEnd() override { g_sent.clear(); }
    void OnGameStarted() override { g_sent.clear(); }
    void OnPeerResync(uint8_t) override { g_sent.clear(); }  // the next scan sends everything

    void OnPostProcess() override {
        if (!Net().Online() || !Net().IsHost() || !core::IsPlayerReady() || NowMs() - g_scanMs < 500) return;
        g_scanMs = NowMs();
        int n = Count();
        bool all = g_sent.size() != (size_t)n;
        g_sent.resize(n);
        std::vector<int> changed;
        for (int i = 0; i < n; ++i) {
            ZoneBytes z = ReadZone(i);
            if (all || !(z == g_sent[i])) {
                g_sent[i] = z;
                changed.push_back(i);
            }
        }
        if (!changed.empty() && !Net().Peers().empty()) {
            Send(changed);
            if (!all) LOGI("Territories: %zu zone infos changed", changed.size());
        }
    }

    static Zones& Instance() {
        static Zones m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Zones::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
