#include "widgets/overlay/Overlay.h"
#include "widgets/overlay/DiagnosticPanel.h"

#include <algorithm>

#include "common/config/version/Version.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "features/story/missions/Missions.h"
#include "features/voice/Voice.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop::ui {

namespace {
bool g_playerList = false;
}

void TogglePlayerList() { g_playerList = !g_playerList; }

void RenderOverlay() {
    auto& g = Gfx();
    float s = g.Scale();
    auto& net = Net();
    bool frontend = game::GameState() != game::GAME_STATE_IDLE;

    // status line, bottom-right
    std::string line;
    if (net.Online()) {
        line = Format(TR("SA:COOP · %s · %u мс", "SA:COOP · %s · %u ms"),
                      net.IsHost() ? TR("хост", "host") : TR("напарник", "partner"), net.Rtt());
    } else if (frontend) {
        line = TR("SA:COOP " COOP_VERSION_STR " · F9: совместная игра", "SA:COOP " COOP_VERSION_STR " · F9: co-op");
    }
    if (!line.empty())
        g.Text(FONT_SMALL, g.Width() - 16 * s, g.Height() - g.LineHeight(FONT_SMALL) - 10 * s, line, 0xC0FFFFFF,
               TEXT_RIGHT | TEXT_OUTLINE);

    // flow status in the centre while something is going on and the menu is closed
    auto phase = flow::GetPhase();
    std::string st, hint;
    if (phase == flow::Phase::WaitingWorld || phase == flow::Phase::StartingServer || phase == flow::Phase::Connecting)
        st = flow::StatusText();
    else if (!frontend && (st = CutsceneSkipText(hint)).empty())
        st = MissionWaitText();
    if (!core::ConnectMenuVisible() && !st.empty()) {
        float w = std::max(g.TextWidth(FONT_MENU, st), hint.empty() ? 0.0f : g.TextWidth(FONT_SMALL, hint)) + 48 * s;
        float h = g.LineHeight(FONT_MENU) + 24 * s + (hint.empty() ? 0.0f : g.LineHeight(FONT_SMALL) + 6 * s);
        float x = (g.Width() - w) * 0.5f, y = g.Height() * 0.18f;
        g.Rect(x, y, w, h, 0xD0000000);
        g.Text(FONT_MENU, g.Width() * 0.5f, y + 12 * s, st, w::kSaBlue, TEXT_CENTER | TEXT_SHADOW);
        if (!hint.empty())
            g.Text(FONT_SMALL, g.Width() * 0.5f, y + 18 * s + g.LineHeight(FONT_MENU), hint, w::kSaMuted, TEXT_CENTER | TEXT_SHADOW);
    }

    if (g_playerList && net.Online()) {
        float W = 520 * s;
        float lh = g.LineHeight(FONT_NORMAL) + 6 * s;
        int rows = 1 + (int)net.Peers().size();
        float H = lh * (rows + 1) + 30 * s;
        float x = (g.Width() - W) * 0.5f, y = g.Height() * 0.25f;
        g.Rect(x, y, W, H, 0xE0000000);
        g.RectOutline(x, y, W, H, w::kSaLine, std::max(1.0f, s));
        g.Text(FONT_MENU_SMALL, x + 16 * s, y + 12 * s,
               Format(TR("%s · игроков: %d", "%s · players: %d"), net.ServerName().c_str(), rows), w::kSaMuted);
        float cy = y + 10 * s + lh * 1.3f;
        auto row = [&](const std::string& name, bool host, uint32_t ping, bool me, bool talking) {
            std::string shown = (talking ? "{FFDC50}((•)){FFFFFF} " : "") + name + (me ? TR(" (вы)", " (you)") : "");
            g.Text(FONT_NORMAL, x + 16 * s, cy, shown, me ? 0xFFFFFFFF : 0xFFD0D0D0, TEXT_SHADOW);
            // the role ends before the ping column ("999 мс"): "напарник" in this font reached into it
            g.Text(FONT_MENU_SMALL, x + W - 104 * s, cy + 3 * s, host ? TR("хост", "host") : TR("напарник", "partner"),
                   host ? w::kSaBlue : w::kSaGreen, TEXT_RIGHT);
            g.Text(FONT_NORMAL, x + W - 16 * s, cy, Format(TR("%u мс", "%u ms"), ping), w::kSaMuted, TEXT_RIGHT | TEXT_SHADOW);
            cy += lh;
        };
        row(Cfg().nickname, net.IsHost(), net.Rtt(), true, VoiceIsTransmitting());
        for (auto& [id, p] : net.Peers()) row(p.name, p.isHost, p.ping, false, VoiceIsSpeaking(id));
    }

    if (!core::ConnectMenuVisible()) RenderDiagnosticPanel(false);
}

}  // namespace coop::ui
