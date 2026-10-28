#pragma once
#include <cstdint>
#include <deque>

namespace coop {
// Preserve a requested line until this guest has loaded and played it. A host's CLEAR can
// arrive in the same receive batch as PLAY; it must not erase a line still loading here.
struct DialogueQueue {
    struct Clip {
        int event = -1;
        uint32_t token = 0;
        bool play = false, clear = false, loaded = false, started = false;
        uint64_t since = 0, startedAt = 0, loadSince=0;
        bool observedPlaying = false;
        bool readyReported=false,playingReported=false;
    };
    std::deque<Clip> clips;
    uint32_t latest = 0;
    bool Receive(uint8_t op, int event, uint32_t token, uint64_t now) {
        if (!token || op < 1 || op > 3) return false;
        for (auto& c : clips) if (c.token == token) {
            if (c.event != event) return false;
            if (op == 2) c.play = true;
            if (op == 3) c.clear = true;
            return true;
        }
        if (op == 3 || (latest && (int32_t)(token - latest) <= 0)) return false;
        latest = token;
        // An unused preload may be replaced; a line with a PLAY request may not.
        if (!clips.empty() && !clips.back().play) clips.pop_back();
        // Requested lines retire/cancel explicitly; a loading guest must not
        // silently lose the seventeenth spoken line.
        clips.push_back({event, token, op == 2, false, false, false, now, 0});
        return true;
    }
    bool Retire(uint64_t now, bool finished) {
        if (clips.empty()) return false;
        auto& c = clips.front();
        if(c.started && !finished)c.observedPlaying=true;
        // PLAY is asynchronous. "Finished" can still describe the previous line
        // before this one reaches the hardware; do not CLEAR the pending sound.
        bool done = c.started && c.observedPlaying && now - c.startedAt >= 150 && finished;
        if(c.started && now-c.startedAt>30000)done=true; // missing/broken local audio asset
        bool cancelled = c.clear && !c.play;
        bool failed = c.play && !c.started && c.loadSince && now - c.loadSince > 15000;
        if (!done && !cancelled && !failed) return false;
        clips.pop_front();
        return true;
    }
    // Retain the watermark so a late PLAY from the cancelled scene cannot
    // repopulate an empty queue. World/session reset explicitly clears it.
    void Cancel(){clips.clear();}
    bool CancelThrough(uint32_t token){
        if(!token)return false;
        bool front=!clips.empty()&&(int32_t)(clips.front().token-token)<=0;
        for(auto it=clips.begin();it!=clips.end();)it=(int32_t)(it->token-token)<=0?clips.erase(it):std::next(it);
        if(!latest||(int32_t)(token-latest)>0)latest=token;
        return front;
    }
};
}  // namespace coop
