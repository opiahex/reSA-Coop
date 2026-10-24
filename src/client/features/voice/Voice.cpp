// Voice chat: push-to-talk (or always on) capture with WinMM, 16 kHz mono, 20 ms frames, IMA ADPCM
// (~68 kbit/s), sent unreliable on the voice channel. Every remote player gets its own waveOut stream
// with a small jitter buffer; the volume fades with distance down to a "radio" floor.
// WinMM runs without callbacks: buffers are polled once per game frame (8 x 20 ms of capture headroom).
#include "features/voice/Voice.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/ini/Ini.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "features/voice/Adpcm.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop {

namespace {
constexpr int kRate = 16000, kFrame = 320;  // 20 ms
constexpr int kInBuffers = 8, kOutBuffers = 16;
constexpr size_t kMaxQueue = 12;  // 240 ms: older audio is dropped
constexpr size_t kStartQueue = 3;  // 60 ms of buffering before a stream starts

WAVEFORMATEX Format16k() {
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 1;
    f.nSamplesPerSec = kRate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 2;
    f.nAvgBytesPerSec = kRate * 2;
    return f;
}

struct Stream {
    HWAVEOUT out = nullptr;
    WAVEHDR hdr[kOutBuffers] = {};
    int16_t buf[kOutBuffers][kFrame] = {};
    std::deque<std::vector<int16_t>> queue;
    bool playing = false;
    uint16_t lastSeq = 0;
    bool hasSeq = false;
    uint64_t lastHeardMs = 0;

    ~Stream() {
        if (!out) return;
        waveOutReset(out);
        for (WAVEHDR& h : hdr)
            if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(out, &h, sizeof(h));
        waveOutClose(out);
    }
};

// WinMM drivers can block for a full second even while returning a capture
// buffer. No device API or device destruction runs on the simulation thread.
// The detached worker owns its context until all queued device cleanup ends.
struct VoiceIo : std::enable_shared_from_this<VoiceIo> {
    std::atomic<bool> quit{false},talk{false};
    std::atomic<int> micStatus{0};
    std::atomic<unsigned> slowMs{0};
    int mic=-1,speaker=-1;bool capture=true;
    std::mutex mutex;
    std::deque<std::vector<int16_t>> recorded;
    std::map<uint8_t,std::deque<std::vector<int16_t>>> outgoing;
    unsigned dropMask=0;
    void Start(){auto self=shared_from_this();std::thread([self]{self->Run();}).detach();}
    void Drop(uint8_t id){std::lock_guard<std::mutex> lock(mutex);if(id<3)dropMask|=1u<<id;outgoing.erase(id);}
    void Run(){
        HWAVEIN in=nullptr;WAVEHDR headers[kInBuffers]{};int16_t samples[kInBuffers][kFrame]{};int next=0;
        std::map<uint8_t,std::unique_ptr<Stream>> streams;uint64_t retry=0;
        while(!quit){auto began=GetTickCount64();
            if(capture&&!in&&began>=retry){retry=began+10000;auto format=Format16k();
                if(waveInOpen(&in,mic<0?WAVE_MAPPER:(UINT)mic,&format,0,0,CALLBACK_NULL)==MMSYSERR_NOERROR){
                    for(int i=0;i<kInBuffers;++i){headers[i]={};headers[i].lpData=(LPSTR)samples[i];headers[i].dwBufferLength=sizeof(samples[i]);waveInPrepareHeader(in,&headers[i],sizeof(WAVEHDR));waveInAddBuffer(in,&headers[i],sizeof(WAVEHDR));}
                    auto result=waveInStart(in);micStatus=result==MMSYSERR_NOERROR?1:-1;
                    if(result!=MMSYSERR_NOERROR){waveInReset(in);for(auto&h:headers)if(h.dwFlags&WHDR_PREPARED)waveInUnprepareHeader(in,&h,sizeof(h));waveInClose(in);in=nullptr;}
                }else{in=nullptr;micStatus=-1;}
            }
            if(in)for(int n=0;n<kInBuffers;++n){auto&h=headers[next];if(!(h.dwFlags&WHDR_DONE))break;
                if(talk&&h.dwBytesRecorded==sizeof(samples[next])){std::lock_guard<std::mutex> lock(mutex);recorded.emplace_back(samples[next],samples[next]+kFrame);while(recorded.size()>kMaxQueue)recorded.pop_front();}
                h.dwFlags&=~WHDR_DONE;h.dwBytesRecorded=0;waveInAddBuffer(in,&h,sizeof(h));next=(next+1)%kInBuffers;
            }
            std::map<uint8_t,std::deque<std::vector<int16_t>>> pending;unsigned drops;
            {std::lock_guard<std::mutex> lock(mutex);pending.swap(outgoing);drops=dropMask;dropMask=0;}
            for(int id=0;id<3;++id)if(drops&(1u<<id))streams.erase((uint8_t)id);
            for(auto&[id,frames]:pending){auto&slot=streams[id];if(!slot)slot=std::make_unique<Stream>();for(auto&pcm:frames)slot->queue.push_back(std::move(pcm));while(slot->queue.size()>kMaxQueue)slot->queue.pop_front();}
            for(auto&[id,slot]:streams){auto&s=*slot;if(!s.out){if(s.queue.empty())continue;auto format=Format16k();if(waveOutOpen(&s.out,speaker<0?WAVE_MAPPER:(UINT)speaker,&format,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR){s.out=nullptr;s.queue.clear();continue;}for(auto&h:s.hdr)h.dwFlags=WHDR_DONE;}
                if(!s.playing&&s.queue.size()<kStartQueue)continue;bool active=false;
                for(int i=0;i<kOutBuffers;++i){auto&h=s.hdr[i];if(!(h.dwFlags&WHDR_DONE)){active=true;continue;}if(s.queue.empty())continue;
                    if(h.dwFlags&WHDR_PREPARED)waveOutUnprepareHeader(s.out,&h,sizeof(h));memcpy(s.buf[i],s.queue.front().data(),sizeof(s.buf[i]));s.queue.pop_front();h={};h.lpData=(LPSTR)s.buf[i];h.dwBufferLength=sizeof(s.buf[i]);waveOutPrepareHeader(s.out,&h,sizeof(h));waveOutWrite(s.out,&h,sizeof(h));active=true;
                }s.playing=active||!s.queue.empty();
            }
            auto elapsed=(unsigned)(GetTickCount64()-began);if(elapsed>slowMs.load())slowMs=elapsed;Sleep(5);
        }
        if(in){waveInReset(in);for(auto&h:headers)if(h.dwFlags&WHDR_PREPARED)waveInUnprepareHeader(in,&h,sizeof(h));waveInClose(in);}
        streams.clear();
    }
};

class Voice : public Module {
public:
    const char* Name() const override { return "Voice"; }
    ~Voice(){if(m_io)m_io->quit=true;}

    void OnInstallHooks() override {
        Net().SetHandler(proto::G_VOICE, [](uint8_t from, Reader& r) { Instance().Handle(from, r); });
        Ini ini;  // test setups: a 440 Hz tone instead of the microphone (no device needed), transmitted all the time
        ini.Load(Cfg().iniPath);
        m_testTone = ini.GetBool("debug", "voice_test_tone", false);
    }

    void OnSessionStart() override {
        m_micWarned = m_silenceWarned = false;
        if(Cfg().voiceEnabled){m_io=std::make_shared<VoiceIo>();m_io->mic=Cfg().micDevice;m_io->speaker=Cfg().speakerDevice;m_io->capture=!m_testTone;m_io->Start();}
    }
    void OnSessionEnd() override {
        if(m_io){m_io->quit=true;m_io.reset();}m_transmitting=false;
        m_streams.clear();
    }
    void OnPeerLeave(uint8_t id) override { m_streams.erase(id);if(m_io)m_io->Drop(id); }

    void OnPostProcess() override {
        if (!Net().Online()) return;
        m_transmitting=Cfg().voiceEnabled&&PushToTalk();if(!m_transmitting)m_enc={};
        if(m_testTone)PumpCapture();
        if(m_io){
            m_io->talk=m_transmitting;std::deque<std::vector<int16_t>> input;
            {std::lock_guard<std::mutex> lock(m_io->mutex);input.swap(m_io->recorded);for(auto&[id,s]:m_streams){auto&q=m_io->outgoing[id];while(!s->queue.empty()){q.push_back(std::move(s->queue.front()));s->queue.pop_front();}while(q.size()>kMaxQueue)q.pop_front();}}
            for(auto&pcm:input)if(m_transmitting)Send(pcm.data());else m_enc={};
            if(m_io->micStatus==-1&&!m_micWarned){m_micWarned=true;LOGW("Voice: microphone unavailable; device retries run off the game thread");}
            auto slow=m_io->slowMs.exchange(0);if(slow>=80)LOGW("Voice device worker took %u ms; simulation did not wait",slow);
            LogSent();
        }
        if (m_rxFrames && NowMs() - m_rxLogMs > 5000) {
            m_rxLogMs = NowMs();
            LOGI("Voice: %d frames received, rms %.0f", m_rxFrames, std::sqrt(m_rxEnergy / (m_rxFrames * (double)kFrame)));
            m_rxFrames = 0;
            m_rxEnergy = 0;
        }
    }

    void OnRender() override {
        if (!core::IsPlayerReady()) return;
        auto& g = ui::Gfx();
        float s = g.Scale(), y = g.Height() * 0.62f;
        if (m_transmitting) {
            g.Text(ui::FONT_SMALL, 24 * s, y, TR("{FF5050}●{FFFFFF} Микрофон", "{FF5050}●{FFFFFF} Mic"), 0xFFFFFFFF, ui::TEXT_OUTLINE);
            y += g.LineHeight(ui::FONT_SMALL);
        }
        for (auto& [id, stream] : m_streams)  // who is heard now, wherever they are (the name tag needs them in sight)
            if (Speaking(id)) {
                g.Text(ui::FONT_SMALL, 24 * s, y, "{FFDC50}((•)){FFFFFF} " + Net().PeerName(id), 0xFFFFFFFF, ui::TEXT_OUTLINE);
                y += g.LineHeight(ui::FONT_SMALL);
            }
    }

    bool Speaking(uint8_t id) const {
        auto it = m_streams.find(id);
        return it != m_streams.end() && NowMs() - it->second->lastHeardMs < 300;
    }
    bool Transmitting() const { return m_transmitting; }

    static Voice& Instance() {
        static Voice v;
        return v;
    }

private:
    bool PushToTalk() const {
        if (Cfg().voiceAlwaysOn || m_testTone) return true;
        // only while our window is active and nobody types in the chat
        return game::IsForeground() && !ui::TheChat().IsOpen() && (GetAsyncKeyState(Cfg().voiceKey) & 0x8000);
    }

    void PumpCapture() {
        m_transmitting = Cfg().voiceEnabled && PushToTalk();  // the indicator shows the key even without a microphone
        if (m_testTone) {
            uint64_t now = NowMs();
            if (now - m_toneMs > 200) m_toneMs = now;
            for (; now - m_toneMs >= 20; m_toneMs += 20) {  // a frame every 20 ms
                int16_t pcm[kFrame];
                for (int k = 0; k < kFrame; ++k, ++m_tonePhase) pcm[k] = (int16_t)(8000 * std::sin(m_tonePhase * 0.1727876f));
                Send(pcm);
            }
            LogSent();
            return;
        }
    }

    void Send(int16_t* pcm) {
        float gain = Cfg().micGain;
        for (int k = 0; k < kFrame; ++k) {
            if (gain != 1.0f) pcm[k] = (int16_t)std::clamp(pcm[k] * gain, -32768.0f, 32767.0f);
            m_txEnergy += (double)pcm[k] * pcm[k];
        }
        ++m_txFrames;
        std::vector<uint8_t> data = adpcm::Encode(pcm, kFrame, m_enc);
        Writer w(proto::G_VOICE);
        w.U16(m_seq++);
        w.Bytes(data.data(), data.size());
        Net().Send(proto::CH_VOICE, w);
    }

    // what the microphone gives: a privacy block in Windows records pure silence
    void LogSent() {
        if (!m_txFrames || NowMs() - m_txLogMs < 5000) return;
        m_txLogMs = NowMs();
        double rms = std::sqrt(m_txEnergy / (m_txFrames * (double)kFrame));
        LOGI("Voice: %d frames sent, rms %.0f", m_txFrames, rms);
        if (rms < 1.0 && m_txFrames >= 100 && !m_silenceWarned) {
            m_silenceWarned = true;
            ui::TheChat().AddError(TR("Микрофон передаёт тишину. Проверьте, что в Windows разрешён доступ к микрофону для классических приложений, а нужный микрофон выбран устройством записи по умолчанию.",
                                      "The microphone sends only silence. Check that Windows lets desktop apps use the microphone and that the right one is the default recording device."));
        }
        m_txFrames = 0;
        m_txEnergy = 0;
    }

    void Handle(uint8_t from, Reader& r) {
        uint16_t seq = r.U16();
        std::vector<uint8_t> data = r.Bytes(1024);
        std::vector<int16_t> pcm;
        if (!r.Ok() || from == Net().MyId() || !Cfg().voiceEnabled || !adpcm::Decode(data.data(), data.size(), pcm) ||
            pcm.size() != kFrame)
            return;
        auto& slot = m_streams[from];
        if (!slot) slot = std::make_unique<Stream>();
        Stream& s = *slot;
        if (s.hasSeq && (int16_t)(seq - s.lastSeq) <= 0) return;  // late or duplicate
        for (int16_t v : pcm) m_rxEnergy += (double)v * v;
        ++m_rxFrames;
        s.lastSeq = seq;
        s.hasSeq = true;
        s.lastHeardMs = NowMs();
        if (RemotePlayer* rp = Players::Instance().Get(from)) rp->heardMs = s.lastHeardMs;  // its name tag
        float vol = Cfg().voiceVolume * Proximity(from);
        for (int16_t& v : pcm) v = (int16_t)std::clamp(v * vol, -32768.0f, 32767.0f);
        s.queue.push_back(std::move(pcm));
        while (s.queue.size() > kMaxQueue) s.queue.pop_front();
    }

    // full volume up to 15 m, then down to a radio-like floor at 80 m (co-op partners keep talking)
    static float Proximity(uint8_t id) {
        if (!Cfg().voiceProximity || !core::IsPlayerReady()) return 1.0f;
        const RemotePlayer* rp = Players::Instance().Get(id);
        if (!rp || !rp->hasState) return 1.0f;
        float d = (rp->state.pos - game::FindPlayerPed(0)->GetPosition()).Length();
        return std::clamp(1.0f - (d - 15.0f) / 65.0f, 0.35f, 1.0f);
    }

    std::shared_ptr<VoiceIo> m_io;
    bool m_micWarned=false,m_silenceWarned=false;
    adpcm::State m_enc;
    uint16_t m_seq = 0;
    bool m_transmitting = false;
    std::map<uint8_t, std::unique_ptr<Stream>> m_streams;
    bool m_testTone = false;
    unsigned m_tonePhase = 0;
    uint64_t m_toneMs = 0;
    int m_rxFrames = 0, m_txFrames = 0;
    double m_rxEnergy = 0, m_txEnergy = 0;
    uint64_t m_rxLogMs = 0, m_txLogMs = 0;
};

struct Reg {
    Reg() { core::RegisterModule(&Voice::Instance()); }
} g_reg;
}  // namespace

bool VoiceIsSpeaking(uint8_t id) { return Voice::Instance().Speaking(id); }
bool VoiceIsTransmitting() { return Voice::Instance().Transmitting(); }

}  // namespace coop
