#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/buffer/Buffer.h"
#include "common/lib/diagnostics/FlightRecorder.h"
#include "shared/api/net-client/NetClient.h"
#include "shared/lib/sync/EpochFence.h"
#include "shared/lib/sync/EntityGenerations.h"

namespace coop {

struct PeerInfo {
    uint8_t id = 0;
    std::string name;
    bool isHost = false;
    uint16_t ping = 0;
    uint32_t incarnation=0;
};

// Network session of the local client: connection lifecycle, handshake, player list and
// routing of game messages to the registered handlers.
class Session {
public:
    enum class State { Offline, Connecting, Online, Failed };
    using Handler = void (*)(uint8_t from, Reader& r);

    void Connect(const std::string& host, uint16_t port, const std::string& nick, const std::string& password,
                 bool wantsHost);
    void Disconnect(const std::string& reason = "");
    void Update();  // once per frame, game thread

    State GetState() const { return m_state; }
    bool Online() const { return m_state == State::Online; }
    bool IsHost() const { return Online() && m_hostId == m_myId; }
    uint8_t MyId() const { return m_myId; }
    uint32_t MyIncarnation() const { return Online() ? m_myIncarnation : 0; }
    uint8_t HostId() const { return m_hostId; }
    const std::string& FailReason() const { return m_failReason; }
    const std::string& ServerName() const { return m_serverName; }
    bool FriendlyFire() const { return Online() && m_friendlyFire; }  // the host let the players hurt each other
    const std::map<uint8_t, PeerInfo>& Peers() const { return m_peers; }
    const PeerInfo* Peer(uint8_t id) const;
    std::string PeerName(uint8_t id) const;
    uint32_t Rtt() const { return m_net.Rtt(); }
    NetClient& Net() { return m_net; }

    void Send(uint8_t channel, const Writer& w);
    uint64_t BeginEntity(uint8_t kind,uint16_t id,uint8_t scope=255){return m_entities.Begin(kind,id,kind>ENTITY_OBJECT&&scope==255?m_myId:scope);}
    bool EntityLive(uint8_t kind,uint16_t id,uint8_t scope=255)const{return m_entities.Live(kind,id,scope);}
    // Authority-dependent removal (for example a handed-over train) is retired
    // only after its module has validated the sender and authority revision.
    bool RetireEntity(uint8_t kind,uint16_t id,uint8_t scope,uint64_t generation){
        auto i=m_entities.records.find(EntityRef{kind,scope,id}.Key());
        if(!generation||i==m_entities.records.end()||!i->second.live||i->second.generation!=generation)return false;
        i->second.live=false;i->second.links.clear();return true;
    }
    uint64_t ExpectedEntityGeneration(uint8_t kind,uint16_t id)const{for(auto&ref:m_entityContext)if(ref.kind==kind&&ref.id==id)return ref.generation;return m_entities.Current(kind,id);}
    uint64_t EntityGeneration(uint8_t kind,uint16_t id,uint8_t scope=255)const{return m_entities.Current(kind,id,scope);}
    bool EntityLink(uint8_t root,uint16_t id,uint8_t target,uint16_t other,uint8_t scope=255)const{return m_entities.Link(root,id,target,other,scope);}
    void RememberEntityLinks(uint8_t kind,uint16_t id){auto refs=m_entityContext;for(auto&ref:refs)if(ref.kind==kind&&ref.id==id)ref.flags|=ENTITY_SNAPSHOT;m_entities.Commit(refs,[](uint8_t,uint8_t,uint16_t){});}
    const std::vector<EntityRef>& EntityContext()const{return m_entityContext;}
    bool EntityContextValid(const std::vector<EntityRef>&refs)const{return m_entities.Match(refs);}
    uint64_t EntityPacketsDiscarded()const{return m_entities.discarded;}
    size_t EntityPacketsWaiting()const{return m_entityWaiting.size();}
    void AdvanceWorldEpoch(){if(++m_worldEpoch==0)++m_worldEpoch;}
    uint32_t WorldEpoch()const{return m_worldEpoch;}
    flight::Context DiagnosticContext()const{return m_diagnosticContext;}
    uint32_t PeerWorldEpoch(uint8_t peer)const{return peer<m_epochs.peers.size()?m_epochs.peers[peer]:0;}
    uint64_t StalePacketsDiscarded()const{return m_epochs.discarded;}
    uint64_t QueuedPacketsDiscarded()const{return m_expiredPackets+m_net.ExpiredReceivePackets();}
    void SendChat(const std::string& text);

    void SetHandler(uint8_t msgId, Handler h) { m_handlers[msgId] = h; }

    // Callbacks for the game flow
    std::function<void()> onOnline;                         // handshake completed
    std::function<void(const std::string&)> onOffline;      // connection closed (reason)
    std::function<void(const std::vector<uint8_t>&, const std::string&)> onWorld;  // world save received
    std::function<void()> onWorldNone;                      // server has no world yet
    std::function<void()> onWorldNewGame;                   // the host's prologue is over: start a new game, join it
    std::function<void()> onWorldWait;                      // the host plays a new game's prologue: wait
    std::function<void()> onWorldWanted;                    // host: a partner needs a fresh snapshot
    std::function<void(const std::vector<uint8_t>&)> onProfile;
    std::function<void(uint32_t)> onProfileSaved;
    std::function<void(uint8_t id)> onPeerJoin;
    std::function<void(uint8_t id)> onPeerLeave;
    std::function<void(uint8_t newHost)> onHostChanged;
    std::function<void(const std::string&)> onInvite;      // the server learned its invite code
    const std::string& InviteCode() const { return m_inviteCode; }

    // World transfer progress (0..1) while downloading, -1 when idle.
    float WorldProgress() const { return m_worldExpected ? (float)m_worldRecv / (float)m_worldExpected : -1.0f; }

private:
    void HandleService(uint8_t id, Reader& r);
    bool DispatchGame(uint8_t from,uint8_t id,uint8_t channel,uint32_t epoch,uint32_t connection,const std::vector<EntityRef>&refs,const uint8_t*data,size_t size,uint64_t received,bool queue);
    struct DeferredEntityPacket{uint8_t from=0,id=0,channel=0;uint32_t epoch=0,connection=0;uint64_t received=0;std::vector<EntityRef> refs;std::vector<uint8_t> data;flight::Context diagnostic{};};
    EntityGenerations m_entities;
    flight::Context m_diagnosticContext{};
    std::vector<EntityRef> m_entityContext;
    std::vector<DeferredEntityPacket> m_entityWaiting;

    NetClient m_net;
    State m_state = State::Offline;
    std::string m_failReason;
    std::string m_nick, m_password;
    bool m_wantsHost = false;
    uint8_t m_myId = 255, m_hostId = 255;
    uint32_t m_myIncarnation = 0;
    std::string m_serverName;
    bool m_friendlyFire = false;
    std::string m_inviteCode;
    std::map<uint8_t, PeerInfo> m_peers;
    Handler m_handlers[256] = {};
    std::vector<NetClient::Packet> m_inbox;
    uint32_t m_worldEpoch=1;
    EpochFence m_epochs;
    uint64_t m_expiredPackets=0;
    uint64_t m_connectionRevision=0;
    // world download
    std::vector<uint8_t> m_world;
    uint32_t m_worldExpected = 0, m_worldRecv = 0, m_worldCrc = 0;
    int m_worldRetries = 0;  // damaged worlds asked for again in a row
    std::string m_worldLabel;
};

Session& Net();

}  // namespace coop
