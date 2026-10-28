#include "shared/api/net-client/NetClient.h"

#include <enet/enet.h>

#include "common/api/nat-traversal/NatTraversal.h"
#include "common/api/net/Net.h"
#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/diagnostics/PassengerTrace.h"
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/lib/sync/ReceiveBacklog.h"

namespace coop {

namespace {
// joining by invite code: STUN answers and the host's punch packets come to our ENet socket
uint8_t g_stunTx[12];
nat::Endpoint g_mapped;
std::atomic<bool> g_stunWaiting{false};
ENetAddress g_lanServer{};  // answer to the LAN discovery

int ENET_CALLBACK Intercept(ENetHost* h, ENetEvent*) {
    const uint8_t* d = h->receivedData;
    size_t n = h->receivedDataLength;
    if (n >= sizeof(nat::kPunchMagic) - 1 && !memcmp(d, nat::kPunchMagic, sizeof(nat::kPunchMagic) - 1)) return 1;
    size_t hereLen = sizeof(nat::kHereMagic) - 1;
    if (n > hereLen && n < hereLen + 6 && !memcmp(d, nat::kHereMagic, hereLen)) {
        g_lanServer.host = h->receivedAddress.host;
        g_lanServer.port = (enet_uint16)atoi(std::string((const char*)d + hereLen, n - hereLen).c_str());
        return 1;
    }
    if (!nat::IsStunPacket(d, n)) return 0;
    nat::Endpoint ep;
    if (g_stunWaiting && nat::ParseStunResponse(d, n, g_stunTx, ep)) {
        g_mapped = ep;
        g_stunWaiting = false;
    }
    return 1;
}

// Tells the host (through the signalling channel) where our socket is seen from the internet, so it can
// open its NAT for us, and opens ours towards it. A host with UPnP is reachable without this.
// The host's TURN relay (if it runs one) is announced on the same channel: "RELAY ip:port".
ENetAddress FindRelay(const std::string& code) {
    ENetAddress relay{};
    nat::HttpResponse r =
        nat::HttpRequest("GET", std::string(nat::kDefaultSignalUrl) + "/" + nat::SignalTopic(code) + "/json?poll=1&since=all");
    for (size_t at = 0; r.status == 200 && (at = r.body.find("\"message\":\"RELAY ", at)) != std::string::npos; ++at) {
        size_t start = at + 17, end = r.body.find('"', start);
        nat::Endpoint ep;
        if (end != std::string::npos && nat::Endpoint::Parse(r.body.substr(start, end - start), ep)) relay = {ep.ip, ep.port};
    }
    return relay;
}

void PrepareNat(ENetHost* client, ENetAddress& server, ENetAddress& relay, const std::string& code) {
    client->intercept = &Intercept;
    g_mapped = {};
    for (const std::string& s : nat::StunServers()) {
        size_t colon = s.rfind(':');
        ENetAddress stun{};
        if (enet_address_set_host(&stun, s.substr(0, colon).c_str()) != 0) continue;
        stun.port = (enet_uint16)atoi(s.c_str() + colon + 1);
        uint8_t req[20];
        nat::BuildStunRequest(req, g_stunTx);
        g_stunWaiting = true;
        ENetBuffer buf{sizeof(req), req};
        enet_socket_send(client->socket, &stun, &buf, 1);
        ENetEvent ev;
        for (int i = 0; i < 30 && g_stunWaiting; ++i) enet_host_service(client, &ev, 50);
        if (g_mapped.Valid()) break;
    }
    g_stunWaiting = false;
    if (g_mapped.Valid()) {
        bool ok = nat::SignalPublish(nat::kDefaultSignalUrl, nat::SignalTopic(code), "PUNCH " + g_mapped.ToString());
        LOGI("Наш внешний адрес: %s. Хост %s", g_mapped.ToString().c_str(), ok ? "оповещён" : "не оповещён (нет связи с ntfy)");
        if (ok) relay = FindRelay(code);  // the host allows our address on its relay when it gets the message
        if (relay.host) LOGI("У хоста есть ретранслятор: %s", nat::Endpoint{relay.host, relay.port}.ToString().c_str());
    } else {
        LOGW("Внешний адрес не определён (STUN недоступен). Подключаемся напрямую");
    }
    if (g_mapped.Valid() && g_mapped.ip == server.host) {
        // the same public address as the host: most NATs cannot loop back, look for it in our network
        g_lanServer = {};
        enet_socket_set_option(client->socket, ENET_SOCKOPT_BROADCAST, 1);
        ENetBuffer disc{sizeof(nat::kDiscoverMagic) - 1, (void*)nat::kDiscoverMagic};
        for (enet_uint16 port : {(enet_uint16)7788, server.port}) {
            ENetAddress bc{ENET_HOST_BROADCAST, port};
            enet_socket_send(client->socket, &bc, &disc, 1);
        }
        ENetEvent ev;
        for (int i = 0; i < 20 && !g_lanServer.host; ++i) enet_host_service(client, &ev, 50);
        if (g_lanServer.host) {
            char ip[64];
            enet_address_get_host_ip(&g_lanServer, ip, sizeof(ip));
            LOGI("Хост найден в локальной сети: %s:%u", ip, g_lanServer.port);
            server = g_lanServer;
            return;
        }
    }
    ENetBuffer punch{sizeof(nat::kPunchMagic) - 1, (void*)nat::kPunchMagic};
    for (int i = 0; i < 3; ++i) enet_socket_send(client->socket, &server, &punch, 1);
}
}  // namespace

bool NetClient::Start(const std::string& host, uint16_t port) {
    Stop();
    m_failure = Failure::None;
    m_expiredReceivePackets = 0;
    m_lastPoll = NowMs();
    m_connectEvent = false;
    m_disconnectEvent = false;
    if (!net::Init()) {
        LOGE("enet init failed");
        m_failure = Failure::Setup;
        return false;
    }
    m_quit = false;
    m_state = State::Connecting;
    m_thread = std::thread(&NetClient::ThreadMain, this, host, port);
    return true;
}

void NetClient::Stop() {
    if (m_thread.joinable()) {
        m_quit = true;
        m_thread.join();
        net::Shutdown();
    }
    m_state = State::Idle;
    std::lock_guard<std::mutex> a(m_outMutex);
    for(const auto&p:m_out)passenger_trace::Emit(p.diagnostic,passenger_trace::OutQueued,passenger_trace::NotConnected,p.diagnostic.source);
    m_out.clear();
    m_outBytes=0;
    // what came after the last poll (a refusal's reason, a world chunk) belonged to that connection: it was handled
    // in the next one ("the world arrived damaged")
    std::lock_guard<std::mutex> b(m_inMutex);
    for(const auto&p:m_in)passenger_trace::Emit(p.diagnostic,passenger_trace::TransportReceive,passenger_trace::NotConnected);
    m_in.clear();
    m_inBytes=0;
}

void NetClient::Send(uint8_t channel, const Writer& w) { Send(channel, w.Data(), w.Size()); }

void NetClient::Send(uint8_t channel, const uint8_t* data, size_t size,flight::Context diagnostic) {
    if (m_state != State::Connected || size == 0){passenger_trace::Emit(diagnostic,passenger_trace::OutQueued,passenger_trace::NotConnected,diagnostic.source);return;}
    std::lock_guard<std::mutex> lock(m_outMutex);
    if(size>32*1024*1024||m_outBytes+size>32*1024*1024||m_out.size()>=32768){
        passenger_trace::Emit(diagnostic,passenger_trace::OutQueued,passenger_trace::QueueLimit,diagnostic.source);
        LOGW("Network outgoing queue limit reached packets=%u bytes=%llu incoming=%llu", (unsigned)m_out.size(),
             (unsigned long long)m_outBytes.load(), (unsigned long long)m_inBytes.load());
        m_failure=Failure::OutgoingBacklog;m_quit=true;m_state=State::Disconnected;m_disconnectEvent=true;return;
    }
    m_out.push_back(Packet{channel, std::vector<uint8_t>(data, data + size),0,diagnostic});
    passenger_trace::Emit(diagnostic,passenger_trace::OutQueued,passenger_trace::Ok,diagnostic.source);
    m_outBytes+=size;
}

void NetClient::Poll(std::vector<Packet>& out) {
    std::lock_guard<std::mutex> lock(m_inMutex);
    m_lastPoll = NowMs();
    m_inBytes=0;
    if (m_in.empty()) return;
    if (out.empty()) {
        out.swap(m_in);
    } else {
        for (auto& p : m_in) out.push_back(std::move(p));
        m_in.clear();
    }
}

void NetClient::ThreadMain(std::string host, uint16_t port) {
    auto makePacket=[&](const Packet&p)->ENetPacket*{
        auto*pkt=net::MakePacket(p.data.data(),p.data.size(),p.channel);
        if(pkt&&(pkt->flags&ENET_PACKET_FLAG_RELIABLE)){
            if(m_reliableBytes+pkt->dataLength>32*1024*1024){
                enet_packet_destroy(pkt);LOGW("ENet reliable queue limit reached bytes=%llu", (unsigned long long)m_reliableBytes.load());
                m_failure=Failure::ReliableBacklog;m_quit=true;m_state=State::Disconnected;m_disconnectEvent=true;return nullptr;
            }
            m_reliableBytes+=pkt->dataLength;pkt->userData=&m_reliableBytes;
            pkt->freeCallback=[](ENetPacket*p){((std::atomic<uint64_t>*)p->userData)->fetch_sub(p->dataLength);};
        }
        return pkt;
    };
    ENetHost* client = enet_host_create(nullptr, 1, proto::CH_COUNT, 0, 0);
    if (!client) {
        LOGE("enet_host_create failed");
        m_failure = Failure::Setup;
        m_state = State::Disconnected;
        m_disconnectEvent = true;
        return;
    }
    enet_host_compress_with_range_coder(client);
    client->mtu = net::kMtu;
    ENetAddress addr{}, relay{};
    nat::Endpoint codeEp;
    uint8_t key = 0;
    if (nat::ParseInviteCode(host, codeEp, key)) {
        addr.host = codeEp.ip;
        addr.port = codeEp.port;
        LOGI("Код приглашения: хост %s", codeEp.ToString().c_str());
        PrepareNat(client, addr, relay, host);
    } else if (enet_address_set_host(&addr, host.c_str()) != 0) {
        LOGE("Не удалось определить адрес сервера \"%s\"", host.c_str());
        enet_host_destroy(client);
        m_failure = Failure::Setup;
        m_state = State::Disconnected;
        m_disconnectEvent = true;
        return;
    }
    if (!codeEp.Valid()) addr.port = port;
    // direct (or punched) first; the relay when that does not answer within 4 s
    bool useRelay = relay.host && Cfg().forceRelay;
    uint64_t relayAt = relay.host && !useRelay ? GetTickCount64() + 4000 : 0;
    ENetPeer* peer = enet_host_connect(client, useRelay ? &relay : &addr, proto::CH_COUNT, 0);
    if (!peer) {
        enet_host_destroy(client);
        m_failure = Failure::Setup;
        m_state = State::Disconnected;
        m_disconnectEvent = true;
        return;
    }
    enet_peer_timeout(peer, 0, 10000, 20000);
    LOGI("Подключение к %s:%u...", host.c_str(), port);

    std::vector<Packet> outLocal;
    bool connected = false;
    // the connection just before it ended, for the log: ENet resets the peer before it reports the end
    enet_uint32 lastLoss = 0, lastInTransit = 0, lastHeard = 0;
    uint64_t servicedAt = NowMs(), longestServiceGap = 0;
    auto logHealth = [&](const char* cause) {
        auto now = NowMs();
        LOGW("Network end cause=%s rtt=%u receive_age=%u game_poll_age=%llu service_gap_max=%llu queued_in=%llu queued_out=%llu reliable=%llu expired_state=%llu",
             cause, m_rtt.load(), lastHeard ? enet_time_get() - lastHeard : 0,
             (unsigned long long)(now - m_lastPoll.load()), (unsigned long long)longestServiceGap,
             (unsigned long long)m_inBytes.load(), (unsigned long long)m_outBytes.load(),
             (unsigned long long)m_reliableBytes.load(), (unsigned long long)m_expiredReceivePackets.load());
    };
    while (!m_quit) {
        auto now = NowMs();
        auto serviceGap = now - servicedAt;
        if (serviceGap > longestServiceGap) longestServiceGap = serviceGap;
        if (connected && serviceGap > 1000)
            LOGW("Network service resumed after %llu ms without servicing the socket", (unsigned long long)serviceGap);
        servicedAt = now;
        // flush outgoing
        {
            std::lock_guard<std::mutex> lock(m_outMutex);
            outLocal.swap(m_out);m_outBytes=0;
        }
        if (connected) {
            for (auto& p : outLocal) {
                ENetPacket* pkt = makePacket(p);if(!pkt){passenger_trace::Emit(p.diagnostic,passenger_trace::TransportSend,passenger_trace::AllocationFailed,p.diagnostic.source);if(m_quit)break;continue;}
                int sent=enet_peer_send(peer,p.channel,pkt);passenger_trace::Emit(p.diagnostic,passenger_trace::TransportSend,sent?passenger_trace::SendFailed:passenger_trace::Ok,p.diagnostic.source);
                if (sent != 0) enet_packet_destroy(pkt);
                m_bytesOut += p.data.size();
            }
        }
        else for(const auto&p:outLocal)passenger_trace::Emit(p.diagnostic,passenger_trace::TransportSend,passenger_trace::NotConnected,p.diagnostic.source);
        outLocal.clear();

        if (!connected && relayAt && (GetTickCount64() >= relayAt || peer->state == ENET_PEER_STATE_DISCONNECTED)) {
            LOGI("Напрямую сервер не отвечает. Подключаемся через ретранслятор");
            enet_peer_reset(peer);
            peer = enet_host_connect(client, &relay, proto::CH_COUNT, 0);
            relayAt = 0;
            if (!peer) {
                LOGE("Could not allocate an ENet peer for the relay connection");
                m_failure = Failure::Setup;
                m_state = State::Disconnected;
                m_disconnectEvent = true;
                break;
            }
            enet_peer_timeout(peer, 0, 10000, 20000);
        }

        ENetEvent ev;
        int rc = enet_host_service(client, &ev, 2);
        while (rc > 0) {
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    connected = true;
                    m_state = State::Connected;
                    m_connectEvent = true;
                    LOGI("Соединение с сервером установлено");
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    if (!connected && relayAt) {
                        relayAt = 1;  // the direct attempt gave up: switch to the relay above
                        break;
                    }
                    LOGW("Соединение с сервером %s", connected ? "потеряно" : "не установлено");
                    if (connected)
                        LOGI("Перед разрывом: пинг %u мс, потери %.1f%%, без подтверждения %u байт, сервер молчал %u мс, код %u",
                             m_rtt.load(), lastLoss * 100.0 / (double)ENET_PEER_PACKET_LOSS_SCALE, lastInTransit,
                             enet_time_get() - lastHeard, ev.data);
                    connected = false;
                    m_failure = Failure::Transport;
                    logHealth("transport-disconnect");
                    m_state = State::Disconnected;
                    m_disconnectEvent = true;
                    m_quit = true;
                    break;
                case ENET_EVENT_TYPE_RECEIVE: {
                    m_bytesIn += ev.packet->dataLength;
                    Packet p{ev.channelID, std::vector<uint8_t>(ev.packet->data, ev.packet->data + ev.packet->dataLength),NowMs()};
                    p.diagnostic=passenger_trace::Packet(p.data.data(),p.data.size(),p.channel,true);
                    passenger_trace::Emit(p.diagnostic,passenger_trace::TransportReceive);
                    enet_packet_destroy(ev.packet);
                    std::lock_guard<std::mutex> lock(m_inMutex);
                    if (m_in.size() >= 4000 || m_inBytes + p.data.size() > 32 * 1024 * 1024) {
                        uint64_t bytes = m_inBytes.load();
                        m_expiredReceivePackets += ExpireReceiveBacklog(m_in, p.received, bytes,[](const Packet&expired){passenger_trace::Emit(expired.diagnostic,passenger_trace::TransportReceive,passenger_trace::Expired);});
                        m_inBytes = bytes;
                    }
                    if(m_inBytes+p.data.size()>32*1024*1024||m_in.size()>=32768){
                        passenger_trace::Emit(p.diagnostic,passenger_trace::TransportReceive,passenger_trace::QueueLimit);
                        LOGW("Network incoming queue limit reached packets=%u bytes=%llu poll_age=%llu", (unsigned)m_in.size(),
                             (unsigned long long)m_inBytes.load(), (unsigned long long)(NowMs() - m_lastPoll.load()));
                        m_failure=Failure::IncomingBacklog;m_quit=true;m_state=State::Disconnected;m_disconnectEvent=true;break;
                    }
                    m_inBytes+=p.data.size();
                    m_in.push_back(std::move(p));
                    break;
                }
                default:
                    break;
            }
            if (m_quit) break;
            rc = enet_host_check_events(client, &ev);
        }
        if (rc < 0) {
            LOGE("ENet socket service failed (WSA %d)", WSAGetLastError());
            m_failure = Failure::Socket;
            m_state = State::Disconnected;
            m_disconnectEvent = true;
            m_quit = true;
            logHealth("socket-service-error");
        }
        if (connected) {
            m_rtt = peer->roundTripTime;
            lastLoss = peer->packetLoss;
            lastInTransit = peer->reliableDataInTransit;
            lastHeard = peer->lastReceiveTime;
        }
    }

    if (m_failure == Failure::OutgoingBacklog) logHealth("outgoing-backlog");
    else if (m_failure == Failure::IncomingBacklog) logHealth("incoming-backlog");
    else if (m_failure == Failure::ReliableBacklog) logHealth("reliable-backlog");

    if (connected && m_failure == Failure::None) {
        // what the game queued last goes first (the partner's profile on leaving): enet_peer_disconnect drops the
        // queues, enet_peer_disconnect_later sends them and then disconnects
        {
            std::lock_guard<std::mutex> lock(m_outMutex);
            outLocal.swap(m_out);m_outBytes=0;
        }
        for (auto& p : outLocal) {
            ENetPacket* pkt = makePacket(p);if(!pkt){passenger_trace::Emit(p.diagnostic,passenger_trace::TransportSend,passenger_trace::AllocationFailed,p.diagnostic.source);if(m_quit)break;continue;}
            int sent=enet_peer_send(peer,p.channel,pkt);passenger_trace::Emit(p.diagnostic,passenger_trace::TransportSend,sent?passenger_trace::SendFailed:passenger_trace::Ok,p.diagnostic.source);
            if (sent != 0) enet_packet_destroy(pkt);
        }
        outLocal.clear();
        // graceful quit: data = 1 tells the server it was not a timeout
        enet_peer_disconnect_later(peer, 1);
        ENetEvent ev;
        uint64_t deadline = GetTickCount64() + 1000;
        while (GetTickCount64() < deadline && enet_host_service(client, &ev, 50) >= 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
            if (ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
        }
        enet_peer_reset(peer);
    } else if (connected) {
        enet_peer_disconnect_now(peer, 2);
    }
    enet_host_destroy(client);
    if (m_state != State::Disconnected) {
        m_state = State::Disconnected;
    }
}

}  // namespace coop
