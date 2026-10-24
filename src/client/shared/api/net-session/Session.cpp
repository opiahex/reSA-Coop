#include <windows.h>
#include "shared/api/net-session/Session.h"

#include "common/config/version/Version.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "common/lib/profile/Profile.h"
#include "shared/config/Config.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop {

using namespace proto;

Session& Net() {
    static Session s;
    return s;
}

void Session::Connect(const std::string& host, uint16_t port, const std::string& nick, const std::string& password,
                      bool wantsHost) {
    Disconnect();
    m_epochs={};
    m_entities.Reset((GetTickCount64()<<24)^((uint64_t)GetCurrentProcessId()<<8));m_entityWaiting.clear();m_entityContext.clear();
    m_nick = nick;
    m_password = password;
    m_wantsHost = wantsHost;
    m_failReason.clear();
    m_peers.clear();
    m_inviteCode.clear();
    m_worldRetries = 0;
    m_myId = m_hostId = 255;
    m_state = State::Connecting;
    if (!m_net.Start(host, port)) {
        m_state = State::Failed;
        m_failReason = TR("Не удалось запустить сеть.", "Could not start the network.");
    }
}

void Session::Disconnect(const std::string& reason) {
    bool wasOnline = m_state == State::Online;
    m_net.Stop();
    m_net.TakeConnectEvent();
    m_net.TakeDisconnectEvent();
    if (m_state == State::Online || m_state == State::Connecting) m_state = State::Offline;
    m_peers.clear();
    m_world.clear();m_entityWaiting.clear();m_entityContext.clear();
    m_worldExpected = m_worldRecv = 0;
    if (wasOnline && onOffline) onOffline(reason);
}

const PeerInfo* Session::Peer(uint8_t id) const {
    auto it = m_peers.find(id);
    return it == m_peers.end() ? nullptr : &it->second;
}

std::string Session::PeerName(uint8_t id) const {
    if (id == m_myId) return m_nick;
    auto* p = Peer(id);
    return p ? p->name : Format(TR("игрок %d", "player %d"), id);
}

void Session::SendChat(const std::string& text) {
    Writer w(C_CHAT);
    w.Str(text, kMaxChatLen * 4);
    Send(CH_RELIABLE, w);
}
void Session::Send(uint8_t channel,const Writer&w){
    auto*b=w.Data();auto size=w.Size();
    if(!size||!proto::IsRelayed(b[0])){m_net.Send(channel,w);return;}
    auto refs=w.Entities();if(refs.size()>128||!m_entities.Stamp(refs,m_myId)){++m_entities.discarded;LOGD("Entity generation: outgoing references unavailable msg=%u",b[0]);return;}
    Writer framed(b[0]);framed.U32(m_worldEpoch);framed.U8((uint8_t)refs.size());
    for(auto&ref:refs){framed.U8(ref.kind);framed.U8(ref.scope);framed.U16(ref.id);framed.U64(ref.generation);framed.U8(ref.flags);}
    m_entities.Commit(refs,[](uint8_t,uint8_t,uint16_t){});framed.Raw(b+1,size-1);m_net.Send(channel,framed);m_entities.Complete(refs);
}
bool Session::DispatchGame(uint8_t from,uint8_t id,uint8_t channel,uint32_t epoch,uint32_t connection,const std::vector<EntityRef>&refs,const uint8_t*data,size_t size,bool queue){
    auto verdict=m_entities.Check(refs,from,m_hostId);
    if(verdict==EntityGenerations::Wait){
        // Announce identities before waiting for their native dependencies.
        // This also breaks legitimate A<->B paired-action creation cycles.
        std::vector<EntityRef> births;for(auto ref:refs)if(ref.flags&ENTITY_BIRTH){ref.flags=ENTITY_BIRTH;births.push_back(ref);}
        m_entities.Commit(births,[](uint8_t kind,uint8_t scope,uint16_t entity){for(auto*module:SyncAuditModules())module->OnEntityReplaced(kind,scope,entity);});
        if(queue&&m_entityWaiting.size()<128&&size<=65536){
            size_t bytes=size;for(auto&p:m_entityWaiting)bytes+=p.data.size();
            if(bytes<=2*1024*1024)m_entityWaiting.push_back({from,id,channel,epoch,connection,NowMs(),refs,{data,data+size}});
        }
        return false;
    }
    if(verdict==EntityGenerations::Reject){++m_entities.discarded;LOGD("Entity generation rejected msg=%u from=%u refs=%u",id,from,(unsigned)refs.size());return true;}
    m_entities.Commit(refs,[](uint8_t kind,uint8_t scope,uint16_t entity){for(auto*module:SyncAuditModules())module->OnEntityReplaced(kind,scope,entity);});
    m_entityContext=refs;Reader payload(data,size);if(m_handlers[id])m_handlers[id](from,payload);m_entityContext.clear();m_entities.Complete(refs);return true;
}

void Session::Update() {
    if (m_net.TakeConnectEvent()) {
        Hello h;
        h.protocol = kProtocolVersion;
        h.version = COOP_VERSION_STR;
        h.name = m_nick;
        h.password = m_password;
        h.wantsHost = m_wantsHost;
        h.exeSize = kSupportedExeSize;
        Writer w(C_HELLO);
        h.Write(w);
        m_net.Send(CH_RELIABLE, w);
    }

    m_inbox.clear();
    {profile::Scope timing(profile::NETWORK_POLL);m_net.Poll(m_inbox);}
    profile::Scope dispatchTiming(profile::NETWORK_DISPATCH);
    for (auto& pkt : m_inbox) {
        if (pkt.data.empty()) continue;
        uint8_t id = pkt.data[0];
        if (IsRelayed(id)) {
            if (pkt.data.size() < 2 || m_state != State::Online) continue;
            uint8_t from = pkt.data[1];
            // a player who left: its late state made its character again (with its old time, so the next player under
            // that number had its states dropped as old ones)
            if (!m_peers.count(from)) continue;
            Reader r(pkt.data.data() + 2, pkt.data.size() - 2);
            auto connection=r.U32();
            if(!r.Ok()||connection!=m_peers.at(from).incarnation)continue;
            auto epoch=r.U32();
            if(!r.Ok()||!m_epochs.Accept(from,epoch,id==G_RESYNC))continue;
            uint8_t count=r.U8();if(count>128)continue;std::vector<EntityRef> refs;refs.reserve(count);
            for(unsigned n=0;n<count;++n){EntityRef ref;ref.kind=r.U8();ref.scope=r.U8();ref.id=r.U16();ref.generation=r.U64();ref.flags=r.U8();refs.push_back(ref);}
            if(!r.Ok())continue;
            if(pkt.received){
                uint64_t age=NowMs()-pkt.received;
                bool state=pkt.channel==CH_STATE||pkt.channel==CH_STATE_PLAYER1||pkt.channel==CH_STATE_PLAYER2;
                bool transient=id==G_DAMAGE||id==G_BULLET||id==G_TYRE_HIT||id==G_PROJECTILE||id==G_EXPLOSION||id==G_OBJ_DAMAGE||id==G_VEH_CONTACT;
                if(id==G_OBJ_CARRY){Reader probe=r;auto op=probe.U8();transient=op==3||op==6;}
                if((pkt.channel==CH_VOICE&&age>300)||(state&&age>750)||(transient&&age>2000)){++m_expiredPackets;continue;}
            }
            DispatchGame(from,id,pkt.channel,epoch,connection,refs,r.Cursor(),r.Remaining(),true);
        } else {
            Reader r(pkt.data.data() + 1, pkt.data.size() - 1);
            HandleService(id, r);
        }
    }

    for(size_t n=0;n<m_entityWaiting.size();){
        auto&p=m_entityWaiting[n];auto*peer=Peer(p.from);
        bool expired=NowMs()-p.received>((p.channel==CH_STATE||p.channel==CH_STATE_PLAYER1||p.channel==CH_STATE_PLAYER2)?750u:1500u);
        if(!peer||peer->incarnation!=p.connection||!m_epochs.Accept(p.from,p.epoch,false)||expired){++m_entities.discarded;m_entityWaiting.erase(m_entityWaiting.begin()+n);continue;}
        // Move out before dispatch: a handler is allowed to reset the session.
        auto packet=std::move(p);m_entityWaiting.erase(m_entityWaiting.begin()+n);
        if(!DispatchGame(packet.from,packet.id,packet.channel,packet.epoch,packet.connection,packet.refs,packet.data.data(),packet.data.size(),false)){
            m_entityWaiting.insert(m_entityWaiting.begin()+n,std::move(packet));++n;
        }
    }

    if (m_net.TakeDisconnectEvent()) {
        bool wasOnline = m_state == State::Online;
        if (m_state == State::Connecting) {
            m_state = State::Failed;
            if (m_failReason.empty())
                m_failReason = TR("Сервер не отвечает. Проверьте адрес, порт и брандмауэр.",
                                  "The server does not answer. Check the address, the port and the firewall.");
        } else if (m_state == State::Online) {
            m_state = State::Failed;
            if (m_failReason.empty()) m_failReason = TR("Соединение с сервером потеряно.", "Lost connection to the server.");
        }
        m_net.Stop();
        m_peers.clear();
        if (wasOnline && onOffline) onOffline(m_failReason);
    }
}

void Session::HandleService(uint8_t id, Reader& r) {
    auto& chat = ui::TheChat();
    switch (id) {
        case S_WELCOME: {
            Welcome w;
            if (!w.Read(r)) break;
            m_myId = w.yourId;
            m_hostId = w.hostId;
            m_serverName = w.serverName;
            m_friendlyFire = w.friendlyFire;
            m_peers.clear();
            for (auto& p : w.players) m_peers[p.id] = PeerInfo{p.id, p.name, p.isHost, 0,p.incarnation};
            m_state = State::Online;
            LOGI("Вошли на сервер \"%s\" как игрок %d, хост: %d", m_serverName.c_str(), m_myId, m_hostId);
            chat.AddSystem(IsHost() ? TR("Сервер запущен. Вы хост.", "The server is up. You are the host.")
                                    : Format(TR("Вы подключились к серверу «%s».", "You joined the server \"%s\"."),
                                             m_serverName.c_str()));
            if (!w.motd.empty()) chat.Add(Cfg().english && !w.motdEn.empty() ? w.motdEn : w.motd, 0xFFA0C8FF);
            if (m_friendlyFire)
                chat.AddSystem(TR("Урон по напарнику включён: вы можете ранить и убить друг друга.",
                                  "Friendly fire is on: you can hurt and kill each other."));
            if (onOnline) onOnline();
            break;
        }
        case S_REJECT:
        case S_KICKED: {
            std::string reason = r.Str(), en = r.Str();
            if (Cfg().english && !en.empty()) reason = en;
            if (id == S_REJECT) m_failReason = Format(TR("Не удалось войти: %s.", "Could not join: %s."), reason.c_str());
            else if (reason.empty()) m_failReason = TR("Вас выгнали с сервера.", "You were kicked from the server.");
            else m_failReason = Format(TR("Вас выгнали с сервера: %s.", "You were kicked from the server: %s."), reason.c_str());
            LOGW("%s", m_failReason.c_str());
            // shown once: a failed connect by the flow, a kick by onOffline when the connection closes
            break;
        }
        case S_PLAYER_JOIN: {
            PeerInfo p;
            p.id = r.U8();
            p.name = r.Str(64);
            p.isHost = r.Bool();
            p.incarnation=r.U32();
            if (!r.Ok()) break;
            if(p.id<m_epochs.peers.size())m_epochs.peers[p.id]=0;
            m_peers[p.id] = p;
            if (p.isHost) m_hostId = p.id;
            if (onPeerJoin) onPeerJoin(p.id);
            break;
        }
        case S_PLAYER_LEAVE: {
            uint8_t pid = r.U8();
            if(pid<m_epochs.peers.size())m_epochs.peers[pid]=0;
            if (onPeerLeave) onPeerLeave(pid);
            m_peers.erase(pid);
            break;
        }
        case S_HOST_CHANGED: {
            m_hostId = r.U8();
            for (auto& [pid, p] : m_peers) p.isHost = pid == m_hostId;
            if (onHostChanged) onHostChanged(m_hostId);
            break;
        }
        case S_CHAT: {
            uint8_t from = r.U8();
            uint32_t rgba = r.U32();
            std::string text = r.Str(), en = r.Str();
            if (!r.Ok()) break;
            if (from == kServerId) {
                chat.Add(Cfg().english && !en.empty() ? en : text, ui::RgbaToArgb(rgba));
            } else {
                uint32_t nameColor = from == m_hostId ? 0xFF64B4FF : 0xFF78DC78;
                char hex[16];
                snprintf(hex, sizeof(hex), "{%06X}", nameColor & 0xFFFFFF);
                chat.Add(std::string(hex) + PeerName(from) + "{FFFFFF}: " + text, 0xFFFFFFFF);
            }
            break;
        }
        case S_PINGS: {
            uint8_t n = r.U8();
            for (uint8_t i = 0; i < n && r.Ok(); ++i) {
                uint8_t pid = r.U8();
                uint16_t ping = r.U16();
                auto it = m_peers.find(pid);
                if (it != m_peers.end()) it->second.ping = ping;
            }
            break;
        }
        case S_PROFILE_SAVED: {
            auto crc=r.U32();if(r.Ok()&&onProfileSaved)onProfileSaved(crc);break;
        }
        case S_PROFILE: {
            auto blob = r.Bytes(64 * 1024);
            if (r.Ok() && onProfile) onProfile(blob);
            break;
        }
        case S_WORLD_BEGIN: {
            m_worldExpected = r.U32();
            m_worldCrc = r.U32();
            m_worldLabel = r.Str(128);
            m_worldRecv = 0;
            if (m_worldExpected > 8 * 1024 * 1024) m_worldExpected = 0;
            m_world.assign(m_worldExpected, 0);
            break;
        }
        case S_WORLD_CHUNK: {
            uint32_t off = r.U32();
            auto bytes = r.Bytes(kWorldChunk);
            // off + size wraps in 32 bits: the bound is checked without the sum
            if (!r.Ok() || off > m_world.size() || bytes.size() > m_world.size() - off) break;
            memcpy(m_world.data() + off, bytes.data(), bytes.size());
            m_worldRecv += (uint32_t)bytes.size();
            break;
        }
        case S_WORLD_END: {
            uint32_t crc = Crc32(m_world.data(), m_world.size());
            if (crc != m_worldCrc) {
                LOGE("Сохранение мира повреждено при передаче (crc %08X != %08X)", crc, m_worldCrc);
                // asked again three times at most: a world too big to take (over 8 MB) came "damaged" for ever
                if (++m_worldRetries <= 3) {
                    chat.AddError(TR("Мир пришёл с ошибкой. Запрашиваем ещё раз...", "The world arrived damaged. Asking for it again..."));
                    Writer w(C_WORLD_REQUEST);
                    Send(CH_BULK, w);
                } else {
                    chat.AddError(TR("Мир снова пришёл с ошибкой. Попробуйте подключиться заново.",
                                     "The world arrived damaged again. Try to connect again."));
                }
            } else {
                m_worldRetries = 0;
                if (onWorld) onWorld(m_world, m_worldLabel);
            }
            m_world.clear();
            m_worldExpected = m_worldRecv = 0;
            break;
        }
        case S_WORLD_NONE:
            if (onWorldNone) onWorldNone();
            break;
        case S_WORLD_NEWGAME:
            if (onWorldNewGame) onWorldNewGame();
            break;
        case S_WORLD_WAIT:
            if (onWorldWait) onWorldWait();
            break;
        case S_INVITE: {
            std::string code = r.Str(32);
            if (r.Ok() && code != m_inviteCode) {
                m_inviteCode = code;
                if (onInvite) onInvite(code);
            }
            break;
        }
        case S_WORLD_WANTED:
            if (onWorldWanted) onWorldWanted();
            break;
        default:
            LOGW("Unknown service message %u", id);
            break;
    }
}

}  // namespace coop
