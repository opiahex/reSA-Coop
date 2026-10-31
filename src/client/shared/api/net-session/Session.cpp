#include <windows.h>
#include "shared/api/net-session/Session.h"

#include "common/config/version/Version.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "common/lib/profile/Profile.h"
#include "common/lib/diagnostics/PassengerTrace.h"
#include "shared/config/Config.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop {

using namespace proto;

namespace {
// Receiver provenance is valid only while processing that packet. In particular,
// later model/streaming work must not borrow the last completed network message.
struct ScopedDiagnosticContext {
    flight::Context& current;
    const flight::Context previous;
    ScopedDiagnosticContext(flight::Context& slot,const flight::Context& value) noexcept
        :current(slot),previous(slot){current=value;}
    ~ScopedDiagnosticContext(){current=previous;}
    ScopedDiagnosticContext(const ScopedDiagnosticContext&)=delete;
    ScopedDiagnosticContext&operator=(const ScopedDiagnosticContext&)=delete;
};
}

Session& Net() {
    static Session s;
    return s;
}

void Session::Connect(const std::string& host, uint16_t port, const std::string& nick, const std::string& password,
                      bool wantsHost) {
    Disconnect();
    m_epochs={};m_authority.clear();m_authorityPending.clear();m_authorityOutgoing.clear();m_authorityLookups.clear();m_authorityResynced=false;
    m_entities.Reset((GetTickCount64()<<24)^((uint64_t)GetCurrentProcessId()<<8));m_entityWaiting.clear();m_entityContext.clear();
    m_nick = nick;
    m_password = password;
    m_wantsHost = wantsHost;
    m_failReason.clear();
    m_peers.clear();
    m_inviteCode.clear();
    m_worldRetries = 0;
    m_myId = m_hostId = 255;
    m_myIncarnation = 0;
    m_state = State::Connecting;
    if (!m_net.Start(host, port)) {
        m_state = State::Failed;
        m_failReason = TR("Не удалось запустить сеть.", "Could not start the network.");
    }
}

void Session::Disconnect(const std::string& reason) {
    ++m_connectionRevision;
    bool wasOnline = m_state == State::Online;
    m_net.Stop();
    m_net.TakeConnectEvent();
    m_net.TakeDisconnectEvent();
    if (m_state == State::Online || m_state == State::Connecting) m_state = State::Offline;
    m_peers.clear();
    m_world.clear();m_entityWaiting.clear();m_entityContext.clear();m_authorityPending.clear();m_authorityOutgoing.clear();
    m_worldExpected = m_worldRecv = m_worldCrc = 0;
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
namespace {
const EntityRef* AuthorityPrimary(uint8_t id,const std::vector<EntityRef>&refs){
    const auto kind=authority::PrimaryKind(id);if(!kind)return nullptr;
    for(const auto&r:refs)if(r.kind==kind)return &r;return nullptr;
}
const EntityRef* AuthorityOutgoingPrimary(const uint8_t*data,size_t size,const std::vector<EntityRef>&refs,bool&victimComputed){
    victimComputed=size>=29&&data[0]==proto::G_DAMAGE&&data[28]==1;
    if(!victimComputed)return size?AuthorityPrimary(data[0],refs):nullptr;
    const uint8_t kind=data[5]==1?ENTITY_PED:data[5]==2?ENTITY_VEHICLE:0;
    const uint16_t id=uint16_t(data[7])|(uint16_t(data[8])<<8);
    for(const auto&ref:refs)if(ref.kind==kind&&ref.id==id)return &ref;
    return nullptr;
}
}
uint32_t Session::AuthorityLease(uint8_t kind,uint16_t id,uint64_t generation)const{
    auto i=m_authority.find(EntityRef{kind,255,id}.Key());
    if(i==m_authority.end()||(generation&&generation!=i->second.generation))return 0;return i->second.lease;
}
uint8_t Session::AuthorityOwner(uint8_t kind,uint16_t id,uint64_t generation)const{
    auto i=m_authority.find(EntityRef{kind,255,id}.Key());
    if(i==m_authority.end()||(generation&&generation!=i->second.generation))return authority::NoOwner;return i->second.owner;
}
void Session::LookupAuthority(const EntityRef&saved){
    if(!Online()||(saved.kind!=ENTITY_VEHICLE&&saved.kind!=ENTITY_PED)||!saved.id||!saved.generation)return;
    for(const auto&[id,lookup]:m_authorityLookups)if(lookup.saved.Key()==saved.Key()&&lookup.saved.generation==saved.generation)return;
    if(++m_authorityRequest==0)++m_authorityRequest;
    m_authorityLookups[m_authorityRequest]={saved,m_worldEpoch,m_myIncarnation,m_authorityRequest,false};
}

bool Session::BindAuthorityEntity(const EntityRef&ref){
    if(!ref.generation||!AuthorityLease(ref.kind,ref.id,ref.generation))return false;
    auto birth=ref;birth.flags=ENTITY_BIRTH;
    m_entities.Commit({birth},[](uint8_t kind,uint8_t scope,uint16_t id){for(auto*module:SyncAuditModules())module->OnEntityReplaced(kind,scope,id);});
    return m_entities.Live(ref.kind,ref.id,ref.scope)&&m_entities.Current(ref.kind,ref.id,ref.scope)==ref.generation;
}

bool Session::NotifyAuthority(authority::Notice&n){
    bool seen[8]{},ready=true;for(const auto&m:n.members)if(m.ref.kind<8)seen[m.ref.kind]=true;
    if(n.op==authority::ServerOp::Lookup&&seen[ENTITY_VEHICLE])seen[ENTITY_PED]=true; // Saved crew needs cleanup even for a car-only absence reply.
    // Quiesce/reconcile actual ped seats before the car's final snapshot. The
    // read-only readiness pass still verifies every member of the final group.
    if(seen[ENTITY_PED]){auto fn=m_authorityHandlers[ENTITY_PED];ready=fn&&fn(n);if(!ready)return false;}
    if(seen[ENTITY_VEHICLE]){auto fn=m_authorityHandlers[ENTITY_VEHICLE];ready=(fn&&fn(n))&&ready;}
    return ready;
}
void Session::SendAuthority(authority::Notice&n,authority::ClientOp op){
    authority::Command c;c.op=op;c.world=m_worldEpoch;c.request=n.request;c.transaction=n.transaction;c.target=n.target;c.members=n.members;
    Writer w(proto::C_ENTITY_AUTHORITY);c.Write(w);Send(proto::CH_RELIABLE,w);
}
bool Session::AuthorityStopped(uint8_t kind,uint16_t id)const{
    for(const auto&[tx,p]:m_authorityPending){
        for(const auto&m:p.notice.members){
            if(m.transfer&&m.ref.kind==kind&&m.ref.id==id)return true;
        }
    }
    return false;
}
bool Session::RequestAuthority(std::vector<authority::Member>members,uint8_t target,bool voluntary){
    if(!Online()||members.empty()||members.size()>authority::MaxGroup||target>=proto::kMaxPlayers)return false;
    for(auto&m:members){m.lease=AuthorityLease(m.ref.kind,m.ref.id,m.ref.generation);m.owner=AuthorityOwner(m.ref.kind,m.ref.id,m.ref.generation);
        if(!m.lease||AuthorityStopped(m.ref.kind,m.ref.id)||(m.transfer&&voluntary&&m.owner!=m_myId))return false;}
    authority::Notice n;n.op=authority::ServerOp::Stop;n.source=m_myId;n.owner=members[0].owner;n.target=target;n.members=members;
    if(++m_authorityRequest==0)++m_authorityRequest;n.request=m_authorityRequest;
    // No optimistic owner edit. The current simulator is inert before Request;
    // a claimant's local contested vehicle is also frozen until a decision.
    if(!NotifyAuthority(n)){
        // Request was never sent; retain the still-canonical owner's tasks and
        // release only this local provisional stop, without changing a lease.
        n.op=authority::ServerOp::Reject;NotifyAuthority(n);return false;
    }
    m_authorityPending[(uint64_t(1)<<32)|n.request]={n,false};
    authority::Command c;c.op=authority::ClientOp::Request;c.world=m_worldEpoch;c.request=n.request;c.target=target;c.voluntary=voluntary;c.members=members;
    Writer w(proto::C_ENTITY_AUTHORITY);c.Write(w);Send(proto::CH_RELIABLE,w);return true;
}
void Session::HandleAuthority(Reader&r){
    authority::Notice n;if(!n.Read(r))return;
    using Op=authority::ServerOp;
    if(n.op==Op::Lookup){
        auto lookup=m_authorityLookups.find(n.request);
        if(lookup==m_authorityLookups.end()||lookup->second.world!=m_worldEpoch||lookup->second.connection!=m_myIncarnation||
           n.source!=m_myId||n.target!=m_myId||n.targetConnection!=m_myIncarnation||n.targetWorld!=m_worldEpoch||n.members.empty()||
           n.members[0].ref.Key()!=lookup->second.saved.Key()||n.members[0].ref.generation!=lookup->second.saved.generation)return;
        if(n.reason!=authority::Reason::Ok&&n.reason!=authority::Reason::Missing&&n.reason!=authority::Reason::Retired)return;
        if(n.reason==authority::Reason::Ok){
            for(const auto&m:n.members){if(!m.lease||!m.ref.generation)return;auto old=m_authority.find(m.ref.Key());
                if(old!=m_authority.end()&&old->second.generation==m.ref.generation&&old->second.lease>m.lease)return;
            }
            std::vector<EntityRef> refs;
            for(const auto&m:n.members){m_authority[m.ref.Key()]={m.ref.generation,m.lease,m.owner};auto ref=m.ref;ref.flags=ENTITY_BIRTH;refs.push_back(ref);}
            // A verified lookup imports the server's exact current dependency
            // identities, not new births or guessed model/position matches.
            m_entities.Commit(refs,[](uint8_t kind,uint8_t scope,uint16_t id){for(auto*module:SyncAuditModules())module->OnEntityReplaced(kind,scope,id);});
            // A lookup describes only crew dependencies. Keep accepted tow,
            // attachment and interaction references until their real snapshot
            // changes them; importing identity must not erase those contracts.
            for(const auto&root:refs){auto&links=m_entities.records[root.Key()].links;
                for(auto link:refs)if(link.Key()!=root.Key()){
                    link.flags=ENTITY_USE;auto old=std::find_if(links.begin(),links.end(),[&](const EntityRef&r){return r.Key()==link.Key();});
                    if(old==links.end())links.push_back(link);else *old=link;
                }
            }
        }
        m_authorityLookups.erase(lookup);NotifyAuthority(n);return;
    }
    const bool terminal=n.op==Op::Commit||n.op==Op::Abort||n.op==Op::Reject;
    if((n.op==Op::Prepare&&n.target==m_myId&&
       (n.targetConnection!=m_myIncarnation||n.targetWorld!=m_worldEpoch))||
       (n.op==Op::Stop&&n.owner==m_myId&&
       (n.ownerConnection!=m_myIncarnation||n.ownerWorld!=m_worldEpoch)))return;
    // Service decisions are the sole source of owner/lease mutation. Reject a
    // stale completion rather than reviving an earlier A->B->A transaction.
    if(terminal){
        if((n.owner==m_myId&&(n.ownerConnection!=m_myIncarnation||n.ownerWorld!=m_worldEpoch))||
           (n.target==m_myId&&(n.targetConnection!=m_myIncarnation||n.targetWorld!=m_worldEpoch)))return;
        if(n.owner!=authority::NoOwner&&n.owner!=m_myId){auto*p=Peer(n.owner);if(!p||p->incarnation!=n.ownerConnection)return;}
        bool changed=false;
        for(const auto&m:n.members){auto i=m_authority.find(m.ref.Key());
            if(n.transaction&&(!m_entities.Live(m.ref.kind,m.ref.id,m.ref.scope)||m_entities.Current(m.ref.kind,m.ref.id,m.ref.scope)!=m.ref.generation))return;
            if(i!=m_authority.end()&&i->second.generation==m.ref.generation){if(i->second.lease>m.lease)return;changed|=i->second.lease!=m.lease||i->second.owner!=m.owner;}
            else changed=true;
        }
        const bool pending=m_authorityPending.count(n.transaction)||(n.source==m_myId&&m_authorityPending.count((uint64_t(1)<<32)|n.request));
        if(!changed&&!pending)return; // duplicate terminal service cannot restart native tasks/history

        for(const auto&m:n.members){auto&c=m_authority[m.ref.Key()];
            if(c.generation&&c.generation!=m.ref.generation&&m_entities.Current(m.ref.kind,m.ref.id,m.ref.scope)!=m.ref.generation)continue;
            c={m.ref.generation,m.lease,m.owner};}
        for(auto&[request,lookup]:m_authorityLookups)for(const auto&m:n.members)if(m.ref.Key()==lookup.saved.Key()&&m.ref.generation==lookup.saved.generation)lookup.sent=false;
        m_authorityPending.erase(n.transaction);if(n.source==m_myId)m_authorityPending.erase((uint64_t(1)<<32)|n.request);
        NotifyAuthority(n);return;
    }
    if(!n.transaction)return;
    if(n.source==m_myId)m_authorityPending.erase((uint64_t(1)<<32)|n.request);
    auto&pending=m_authorityPending[n.transaction];pending={n,false};
    m_authorityFinalSnapshot=n.op==Op::Stop&&n.owner==m_myId;
    const bool nativeReady=NotifyAuthority(n);m_authorityFinalSnapshot=false;pending.notice=n;
    if(n.op==Op::Stop&&n.owner==m_myId&&nativeReady){SendAuthority(n,authority::ClientOp::StopAck);pending.ready=true;}
}
void Session::PollAuthority(){
    if(m_authorityResynced)for(auto&[id,lookup]:m_authorityLookups)if(!lookup.sent&&lookup.world==m_worldEpoch&&lookup.connection==m_myIncarnation){
        authority::Command c;c.op=authority::ClientOp::Lookup;c.world=m_worldEpoch;c.request=lookup.request;c.target=m_myId;
        authority::Member m;m.ref=lookup.saved;c.members={m};Writer w(proto::C_ENTITY_AUTHORITY);c.Write(w);Send(proto::CH_RELIABLE,w);lookup.sent=true;
    }
    for(auto&[tx,p]:m_authorityPending){auto&n=p.notice;
        if(!p.ready&&n.op==authority::ServerOp::Prepare&&n.target==m_myId&&NotifyAuthority(n)){
            SendAuthority(n,authority::ClientOp::NativeReady);p.ready=true;}
    }
    auto queued=std::move(m_authorityOutgoing);m_authorityOutgoing.clear();
    for(auto&p:queued){if(p.world!=m_worldEpoch||NowMs()-p.queued>1500||!m_entities.Match({p.primary}))continue;
        if(AuthorityLease(p.primary.kind,p.primary.id,p.primary.generation))Send(p.channel,p.packet);
        else m_authorityOutgoing.push_back(std::move(p));}
}

void Session::Send(uint8_t channel,const Writer&w){
    auto*b=w.Data();auto size=w.Size();
    if(!size||!proto::IsRelayed(b[0])){auto context=passenger_trace::Packet(b,size,channel,false,m_myId,m_myIncarnation);passenger_trace::Emit(context,passenger_trace::Producer,passenger_trace::Ok,m_myId);passenger_trace::Describe(context,b,size,channel,false,m_myId);m_net.Send(channel,b,size,context);return;}
    auto refs=w.Entities();if(refs.size()>128||!m_entities.Stamp(refs,m_myId)){++m_entities.discarded;flight::Context c{};if(flight::Enabled()){c.msg=b[0];c.source=m_myId;c.incarnation=m_myIncarnation;c.world=m_worldEpoch;c.channel=passenger_trace::Channel(channel);if(b[0]==proto::G_PLAYER_PASSENGER&&size>=8){c.entity=uint16_t(b[1])|(uint16_t(b[2])<<8);c.wireTime=passenger_trace::U32(b+4);}}passenger_trace::Emit(c,passenger_trace::Framed,passenger_trace::ReferencesUnavailable,m_myId);LOGD("Entity generation: outgoing references unavailable msg=%u",b[0]);return;}
    bool victimComputed=false;const auto*primary=AuthorityOutgoingPrimary(b,size,refs,victimComputed);
    if(victimComputed&&(!primary||b[6]!=m_myId))return;
    uint32_t lease=primary?AuthorityLease(primary->kind,primary->id,primary->generation):0;
    if(victimComputed&&!lease)return; // A held native victim must already have a canonical lease.
    if(primary&&!lease&&!(primary->flags&ENTITY_BIRTH)){
        for(auto&i:m_authorityOutgoing)if(i.packet.Data()[0]==b[0]&&i.primary.Key()==primary->Key()){i={channel,w,*primary,m_worldEpoch,NowMs()};return;}
        if(m_authorityOutgoing.size()<128)m_authorityOutgoing.push_back({channel,w,*primary,m_worldEpoch,NowMs()});return;
    }
    if(primary&&lease&&AuthorityOwner(primary->kind,primary->id,primary->generation)!=m_myId)return;
    if(primary&&AuthorityStopped(primary->kind,primary->id)&&!m_authorityFinalSnapshot&&!victimComputed)return;
    Writer framed(b[0]);framed.U32(m_worldEpoch);framed.U8((uint8_t)refs.size());
    for(auto&ref:refs){framed.U8(ref.kind);framed.U8(ref.scope);framed.U16(ref.id);framed.U64(ref.generation);framed.U8(ref.flags);}
    m_entities.Commit(refs,[](uint8_t,uint8_t,uint16_t){});framed.U32(lease);framed.Raw(b+1,size-1);
    auto context=passenger_trace::Packet(framed.Data(),framed.Size(),channel,false,m_myId,m_myIncarnation);
    if(context.msg){auto event=passenger_trace::publication;event.context=context;event.localPeer=m_myId;event.stage=passenger_trace::Producer;flight::Record(event);passenger_trace::Emit(context,passenger_trace::Framed,passenger_trace::Ok,m_myId);}
    passenger_trace::Describe(context,framed.Data(),framed.Size(),channel,false,m_myId);
    m_net.Send(channel,framed.Data(),framed.Size(),context);m_entities.Complete(refs);
    if(b[0]==proto::G_RESYNC)m_authorityResynced=true;
}
bool Session::CheckPrimaryAuthority(uint8_t id,uint8_t from,const std::vector<EntityRef>&refs,uint32_t&lease){
    if(const auto*primary=AuthorityPrimary(id,refs)){
        auto i=m_authority.find(primary->Key());
        // The server grants on reliable0 before relaying this original birth.
        // Capture its effective lease now; deferred cycles never renormalise
        // an old wire0 birth against a later A->B->A ownership epoch.
        if(!lease){if(!(primary->flags&ENTITY_BIRTH)||i==m_authority.end()||i->second.generation!=primary->generation||i->second.owner!=from||!i->second.lease)return false;lease=i->second.lease;}
        if(i!=m_authority.end()&&i->second.generation==primary->generation){
            if(i->second.lease!=lease||i->second.owner!=from)return false;
        }else if(primary->flags&ENTITY_BIRTH)m_authority[primary->Key()]={primary->generation,lease,from};
        else return false;
    }
    return true;
}

bool Session::DispatchGame(uint8_t from,uint8_t id,uint8_t channel,uint32_t epoch,uint32_t connection,uint32_t lease,const std::vector<EntityRef>&refs,const uint8_t*data,size_t size,uint64_t received,bool queue){
    auto context=m_diagnosticContext;
    const auto revision=m_connectionRevision;
    if(!CheckPrimaryAuthority(id,from,refs,lease))return true;
    auto verdict=m_entities.Check(refs,from,m_hostId);
    if(verdict==EntityGenerations::Wait){
        // Announce identities before waiting for their native dependencies.
        // This also breaks legitimate A<->B paired-action creation cycles.
        std::vector<EntityRef> births;for(auto ref:refs)if(ref.flags&ENTITY_BIRTH){ref.flags=ENTITY_BIRTH;births.push_back(ref);}
        m_entities.Commit(births,[](uint8_t kind,uint8_t scope,uint16_t entity){for(auto*module:SyncAuditModules())module->OnEntityReplaced(kind,scope,entity);});
        if(revision!=m_connectionRevision){passenger_trace::Emit(context,passenger_trace::Fence,passenger_trace::SessionChanged,m_myId);return true;}
        bool waiting=!queue;
        if(queue&&m_entityWaiting.size()<128&&size<=65536){
            size_t bytes=size;for(auto&p:m_entityWaiting)bytes+=p.data.size();
            if(bytes<=2*1024*1024){m_entityWaiting.push_back({from,id,channel,epoch,connection,lease,received?received:NowMs(),refs,{data,data+size},context});waiting=true;}
            else passenger_trace::Emit(context,passenger_trace::Fence,passenger_trace::QueueLimit,m_myId);
        }
        else if(queue)passenger_trace::Emit(context,passenger_trace::Fence,passenger_trace::QueueLimit,m_myId);
        if(waiting)passenger_trace::Emit(context,passenger_trace::Fence,passenger_trace::DependencyWait,m_myId);return false;
    }
    if(verdict==EntityGenerations::Reject){++m_entities.discarded;passenger_trace::Emit(context,passenger_trace::Fence,passenger_trace::GenerationRejected,m_myId);LOGD("Entity generation rejected msg=%u from=%u refs=%u",id,from,(unsigned)refs.size());return true;}
    m_entities.Commit(refs,[](uint8_t kind,uint8_t scope,uint16_t entity){for(auto*module:SyncAuditModules())module->OnEntityReplaced(kind,scope,entity);});
    if(revision!=m_connectionRevision){passenger_trace::Emit(context,passenger_trace::Fence,passenger_trace::SessionChanged,m_myId);return true;}
    passenger_trace::Emit(context,passenger_trace::Fence,passenger_trace::Ok,m_myId);
    m_entityContext=refs;Reader payload(data,size);if(m_handlers[id]){
        passenger_trace::Emit(context,passenger_trace::DispatchBegin,passenger_trace::Ok,m_myId);
        m_handlers[id](from,payload);
        passenger_trace::Emit(context,passenger_trace::DispatchEnd,revision!=m_connectionRevision?passenger_trace::SessionChanged:!payload.Ok()?passenger_trace::ReaderMalformed:passenger_trace::HandlerReturned,m_myId);
    }else passenger_trace::Emit(context,passenger_trace::Decode,passenger_trace::NoHandler,m_myId);
    if(revision!=m_connectionRevision)return true;
    m_entityContext.clear();m_entities.Complete(refs);return true;
}

bool Session::SourceContextCurrent(uint8_t id,uint8_t from,uint32_t connection,uint32_t epoch){
    // Only the trusted server relays these two owner-directed actions back to
    // their originating client after aborting a transfer. No self state/claim.
    if(from==m_myId)return (id==proto::G_DAMAGE||id==proto::G_VEH_CONTACT)&&connection==m_myIncarnation&&epoch==m_worldEpoch;
    auto*p=Peer(from);return p&&p->incarnation==connection&&m_epochs.Accept(from,epoch,id==proto::G_RESYNC);
}

void Session::Update() {
    const auto revision=m_connectionRevision;
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
        ScopedDiagnosticContext diagnosticScope(m_diagnosticContext,passenger_trace::Packet(pkt.data.data(),pkt.data.size(),pkt.channel,true));
        auto trace=[&](passenger_trace::Reason reason){passenger_trace::Emit(m_diagnosticContext,passenger_trace::Receiver,reason,m_myId);};
        trace(passenger_trace::Ok);
        // A final refusal/kick still explains the failure, but an already closed
        // connection must not load a queued world or apply gameplay on its way out.
        if(m_net.GetState()==NetClient::State::Disconnected&&id!=S_REJECT&&id!=S_KICKED){trace(passenger_trace::NotConnected);continue;}
        if (IsRelayed(id)) {
            if (pkt.data.size() < 2 || m_state != State::Online){trace(pkt.data.size()<2?passenger_trace::Malformed:passenger_trace::NotConnected);continue;}
            uint8_t from = pkt.data[1];
            // a player who left: its late state made its character again (with its old time, so the next player under
            // that number had its states dropped as old ones)
            const bool selfAction=from==m_myId&&(id==G_DAMAGE||id==G_VEH_CONTACT);
            if(!selfAction&&!m_peers.count(from)){trace(passenger_trace::NotJoined);continue;}
            Reader r(pkt.data.data() + 2, pkt.data.size() - 2);
            auto connection=r.U32();
            auto epoch=r.U32();
            if(!r.Ok()||!SourceContextCurrent(id,from,connection,epoch)){trace(!r.Ok()?passenger_trace::Malformed:passenger_trace::WrongWorld);continue;}
            uint8_t count=r.U8();if(count>128){trace(passenger_trace::Malformed);continue;}std::vector<EntityRef> refs;refs.reserve(count);
            for(unsigned n=0;n<count;++n){EntityRef ref;ref.kind=r.U8();ref.scope=r.U8();ref.id=r.U16();ref.generation=r.U64();ref.flags=r.U8();refs.push_back(ref);}
            const auto lease=r.U32();
            if(!r.Ok()){trace(passenger_trace::Malformed);continue;}
            if(pkt.received){
                uint64_t age=NowMs()-pkt.received;
                bool state=pkt.channel==CH_STATE||pkt.channel==CH_STATE_PLAYER1||pkt.channel==CH_STATE_PLAYER2;
                bool transient=id==G_DAMAGE||id==G_BULLET||id==G_TYRE_HIT||id==G_PROJECTILE||id==G_EXPLOSION||id==G_OBJ_DAMAGE||id==G_VEH_CONTACT;
                if(id==G_OBJ_CARRY){Reader probe=r;auto op=probe.U8();transient=op==3||op==6;}
                if((pkt.channel==CH_VOICE&&age>300)||(state&&age>750)||(transient&&age>2000)){++m_expiredPackets;trace(passenger_trace::Expired);continue;}
            }
            DispatchGame(from,id,pkt.channel,epoch,connection,lease,refs,r.Cursor(),r.Remaining(),pkt.received,true);
        } else {
            Reader r(pkt.data.data() + 1, pkt.data.size() - 1);
            const auto diagnostic=m_diagnosticContext;const auto local=m_myId;
            passenger_trace::Emit(diagnostic,passenger_trace::ServiceDispatchBegin,passenger_trace::Ok,local);
            HandleService(id, r);
            passenger_trace::Emit(diagnostic,passenger_trace::ServiceDispatchEnd,revision!=m_connectionRevision?passenger_trace::SessionChanged:!r.Ok()?passenger_trace::ReaderMalformed:passenger_trace::HandlerReturned,local);
        }
        // World/profile/module callbacks can disconnect or start another session.
        // The rest of this polled batch belongs to the previous connection.
        if(revision!=m_connectionRevision)return;
    }

    for(size_t n=0;m_net.GetState()!=NetClient::State::Disconnected&&n<m_entityWaiting.size();){
        auto&p=m_entityWaiting[n];
        bool expired=NowMs()-p.received>((p.channel==CH_STATE||p.channel==CH_STATE_PLAYER1||p.channel==CH_STATE_PLAYER2)?750u:1500u);
        if(!SourceContextCurrent(p.id,p.from,p.connection,p.epoch)||expired){passenger_trace::Emit(p.diagnostic,passenger_trace::Fence,expired?passenger_trace::Expired:passenger_trace::WrongWorld,m_myId);++m_entities.discarded;m_entityWaiting.erase(m_entityWaiting.begin()+n);continue;}
        // Move out before dispatch: a handler is allowed to reset the session.
        auto packet=std::move(p);m_entityWaiting.erase(m_entityWaiting.begin()+n);
        ScopedDiagnosticContext diagnosticScope(m_diagnosticContext,packet.diagnostic);
        if(!DispatchGame(packet.from,packet.id,packet.channel,packet.epoch,packet.connection,packet.lease,packet.refs,packet.data.data(),packet.data.size(),packet.received,false)){
            m_entityWaiting.insert(m_entityWaiting.begin()+n,std::move(packet));++n;
        }
        if(revision!=m_connectionRevision)return;
    }

    PollAuthority();
    if (m_net.TakeDisconnectEvent()) {
        ++m_connectionRevision;
        bool wasOnline = m_state == State::Online;
        if (m_state == State::Connecting) {
            m_state = State::Failed;
            if (m_failReason.empty())
                m_failReason = TR("Сервер не отвечает. Проверьте адрес, порт и брандмауэр.",
                                  "The server does not answer. Check the address, the port and the firewall.");
        } else if (m_state == State::Online) {
            m_state = State::Failed;
            if (m_failReason.empty()) {
                switch(m_net.LastFailure()){
                    case NetClient::Failure::IncomingBacklog:
                        m_failReason=TR("Игра слишком долго не обрабатывала сетевые события. Подключитесь заново.",
                                        "The game stopped processing network events for too long. Please reconnect.");break;
                    case NetClient::Failure::OutgoingBacklog:
                    case NetClient::Failure::ReliableBacklog:
                        m_failReason=TR("Очередь отправки переполнена: сервер не успевает принимать данные. Подключитесь заново.",
                                        "The send queue filled while data could not be delivered. Please reconnect.");break;
                    default:m_failReason=TR("Соединение с сервером потеряно.", "Lost connection to the server.");break;
                }
            }
        }
        m_net.Stop();
        m_peers.clear();
        m_world.clear();m_worldExpected=m_worldRecv=m_worldCrc=0;
        m_entityWaiting.clear();m_entityContext.clear();
        if (wasOnline && onOffline) onOffline(m_failReason);
    }
}

void Session::HandleService(uint8_t id, Reader& r) {
    auto& chat = ui::TheChat();
    switch (id) {
        case S_ENTITY_AUTHORITY: HandleAuthority(r);break;
        case S_WELCOME: {
            if(m_state!=State::Connecting)break;
            Welcome w;
            if (!w.Read(r)) break;
            m_myId = w.yourId;
            m_myIncarnation = w.yourIncarnation;
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
