#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <vector>
#include "common/lib/buffer/Buffer.h"

namespace coop::authority {
constexpr uint8_t NoOwner=255;
constexpr int8_t VehicleSeat=-2;
constexpr size_t MaxGroup=10; // one car, driver and eight native passenger slots
enum class ClientOp:uint8_t { Request=1,StopAck=2,NativeReady=3,Cancel=4,Lookup=5 };
enum class ServerOp:uint8_t { Stop=1,Prepare=2,Commit=3,Reject=4,Abort=5,Lookup=6 };
enum class Reason:uint16_t { Ok=0,Malformed,Connection,World,Missing,Retired,Generation,Owner,Lease,Busy,Group,Target,Timeout,Cancelled,Disconnected,WorldChanged,LegacyMutation,Limit,Interaction };

struct Member {
    EntityRef ref{};
    uint32_t lease=0;
    uint8_t owner=NoOwner;
    bool transfer=true; // false: required native dependency, never relinquishes its own simulation
    int8_t seat=VehicleSeat; // vehicle=-2, NPC driver=-1, NPC passengers=0..7
    bool SameIdentity(const Member&b)const{return ref.Key()==b.ref.Key()&&ref.generation==b.ref.generation;}
    void Write(Writer&w)const {w.U8(ref.kind);w.U8(ref.scope);w.U16(ref.id);w.U64(ref.generation);w.U32(lease);w.U8(owner);w.Bool(transfer);w.I8(seat);}
    bool Read(Reader&r){ref.kind=r.U8();ref.scope=r.U8();ref.id=r.U16();ref.generation=r.U64();ref.flags=0;lease=r.U32();owner=r.U8();transfer=r.Bool();seat=r.I8();return r.Ok();}
};
inline void WriteMembers(Writer&w,const std::vector<Member>&m){w.U8((uint8_t)m.size());for(auto&v:m)v.Write(w);}
inline bool ReadMembers(Reader&r,std::vector<Member>&m,bool empty=false){auto n=r.U8();if((!n&&!empty)||n>MaxGroup)return false;m.clear();for(unsigned i=0;i<n;++i){Member v;if(!v.Read(r))return false;m.push_back(v);}return r.Ok();}
struct Command {
    ClientOp op=ClientOp::Request;
    uint32_t world=0,request=0,transaction=0;
    uint8_t target=NoOwner;
    bool voluntary=false; // giver has quiesced every member BEFORE sending Request
    std::vector<Member> members;
    void Write(Writer&w)const{w.U8((uint8_t)op);w.U32(world);w.U32(request);w.U32(transaction);w.U8(target);w.Bool(voluntary);WriteMembers(w,members);}
    bool Read(Reader&r){op=(ClientOp)r.U8();world=r.U32();request=r.U32();transaction=r.U32();target=r.U8();voluntary=r.Bool();return ReadMembers(r,members)&&r.Remaining()==0&&(uint8_t)op>=1&&(uint8_t)op<=5;}
};
struct Notice {
    ServerOp op=ServerOp::Reject;
    uint32_t request=0,transaction=0;
    uint8_t source=NoOwner,owner=NoOwner,target=NoOwner;
    uint32_t ownerConnection=0,ownerWorld=0,targetConnection=0,targetWorld=0;
    Reason reason=Reason::Ok;
    std::vector<Member> members;
    void Write(Writer&w)const{w.U8((uint8_t)op);w.U32(request);w.U32(transaction);w.U8(source);w.U8(owner);w.U8(target);w.U32(ownerConnection);w.U32(ownerWorld);w.U32(targetConnection);w.U32(targetWorld);w.U16((uint16_t)reason);WriteMembers(w,members);}
    bool Read(Reader&r){op=(ServerOp)r.U8();request=r.U32();transaction=r.U32();source=r.U8();owner=r.U8();target=r.U8();ownerConnection=r.U32();ownerWorld=r.U32();targetConnection=r.U32();targetWorld=r.U32();reason=(Reason)r.U16();return ReadMembers(r,members,true)&&r.Remaining()==0&&(uint8_t)op>=1&&(uint8_t)op<=6;}
};
struct Delivery {uint8_t to=NoOwner;Notice notice;}; // NoOwner = all joined clients

// Only simulation-owner mutations use the primary lease. Damage, requests,
// mirror-origin SCRIPT_FREE and host story policy retain their own contracts.
inline uint8_t PrimaryKind(uint8_t msg){switch(msg){
    case 65:case 80:case 81:case 82:case 84:case 141:case 172:return ENTITY_VEHICLE;
    case 90:case 91:case 92:case 94:case 167:return ENTITY_PED;
    default:return 0;
}}
inline bool LegacyOwner(uint8_t msg){return msg==83||msg==85||msg==93;}
struct GameFrame {
    uint32_t world=0,lease=0;
    std::vector<EntityRef> refs;
    const uint8_t* body=nullptr;
    size_t bodySize=0,leaseOffset=0;
};
inline bool ReadGameFrame(const uint8_t*data,size_t size,GameFrame&f){
    if(!data||size<10)return false;
    Reader r(data+1,size-1);f.world=r.U32();auto count=r.U8();if(count>128)return false;f.refs.clear();
    for(unsigned i=0;i<count;++i){EntityRef ref;ref.kind=r.U8();ref.scope=r.U8();ref.id=r.U16();ref.generation=r.U64();ref.flags=r.U8();f.refs.push_back(ref);}
    f.leaseOffset=(size_t)(r.Cursor()-data);f.lease=r.U32();f.body=r.Cursor();f.bodySize=r.Remaining();return r.Ok();
}
struct Identity {
    uint32_t key=0;uint64_t generation=0;
    bool operator<(const Identity&b)const{return key<b.key||(key==b.key&&generation<b.generation);}
};
struct Record {
    EntityRef ref{};
    uint8_t creator=NoOwner,owner=NoOwner;
    uint32_t creatorConnection=0,creatorWorld=0,ownerConnection=0,ownerWorld=0,lease=1,pending=0;
    bool live=true,playerUsed=false,story=false;
    uint16_t vehicle=0;uint64_t vehicleGeneration=0;int8_t seat=VehicleSeat;
    std::vector<Member> crew; // native vehicle intent: exact NPC identities/seat roles
};
struct Admission {bool allow=false;Reason reason=Reason::Malformed;uint32_t lease=0;bool echoSelf=false;};

// Canonical V/P identity and lease state, used by the production server relay.
// It never creates native entities or executes their AI/physics.
class Coordinator {
public:
    struct Peer {uint32_t connection=0,world=0;bool joined=false;};
    struct Transaction {
        uint32_t id=0,request=0;uint8_t source=NoOwner,owner=NoOwner,target=NoOwner;
        uint32_t ownerConnection=0,ownerWorld=0,targetConnection=0,targetWorld=0;
        uint64_t deadline=0;bool prepared=false;
        std::vector<Member> members;
    };
    std::array<Peer,3> peers{};
    std::map<Identity,Record> records;
    std::map<uint32_t,uint64_t> current;
    std::map<uint32_t,std::array<uint64_t,8>> retired;
    std::map<uint32_t,Transaction> transactions;
    static constexpr uint64_t TransactionMs=5000;
    void Connect(uint8_t peer,uint32_t connection){if(peer>=3||!connection)return;if(peers[peer].joined)Disconnect(peer);peers[peer]={connection,0,true};}
    const Record* Find(const EntityRef&ref)const{auto i=records.find({ref.Key(),ref.generation});return i==records.end()?nullptr:&i->second;}
    Record* Find(const EntityRef&ref){auto i=records.find({ref.Key(),ref.generation});return i==records.end()?nullptr:&i->second;}
    std::vector<Delivery> TakeDeliveries(){auto r=std::move(outbox);outbox.clear();return r;}
    void Snapshot(uint8_t to){for(auto&[key,r]:records)if(r.live&&Current(r.ref))Grant(r,to);}
    void Disconnect(uint8_t peer){
        if(peer>=3)return;
        peers[peer].joined=false;
        std::vector<uint32_t> abort;for(auto&[id,t]:transactions)if(t.owner==peer||t.target==peer||t.source==peer)abort.push_back(id);
        for(auto id:abort)Abort(id,Reason::Disconnected);
        for(auto&[key,r]:records)if(r.live&&r.owner==peer){r.owner=NoOwner;r.ownerConnection=r.ownerWorld=0;Bump(r.lease);Grant(r);}
        peers[peer]={};
    }
    void Tick(uint64_t now){std::vector<uint32_t> expired;for(auto&[id,t]:transactions)if(now>=t.deadline)expired.push_back(id);for(auto id:expired)Abort(id,Reason::Timeout);}
    Admission Admit(uint8_t from,uint32_t connection,uint8_t msg,const GameFrame&f,bool host,uint64_t now){
        Tick(now);
        auto deny=[](Reason r){return Admission{false,r,0};};
        if(from>=3||!peers[from].joined||peers[from].connection!=connection)return deny(Reason::Connection);
        if(!f.world)return deny(Reason::World);
        if(!peers[from].world)peers[from].world=f.world;
        if(peers[from].world!=f.world){
            if(msg!=95||(int32_t)(f.world-peers[from].world)<=0)return deny(Reason::World);
            std::vector<uint16_t> keep;Reader p(f.body,f.bodySize);auto n=p.U8();if(n>110)return deny(Reason::Malformed);for(unsigned i=0;i<n;++i)keep.push_back(p.U16());if(!p.Ok()||p.Remaining())return deny(Reason::Malformed);
            WorldChanged(from,f.world,keep);
        }
        if(LegacyOwner(msg))return deny(Reason::LegacyMutation);
        if(msg==74||msg==158){
            if(msg==74){
                if(f.bodySize<28||f.body[27]>1)return deny(Reason::Malformed);
                if(f.body[27]==1){
                    // A victim simulator reports its held NPC/environment hit.
                    // Validate the authority at computation time before a hit
                    // can abort anyone's pending transaction or be echoed.
                    const auto kind=f.body[4]==1?ENTITY_PED:f.body[4]==2?ENTITY_VEHICLE:0;
                    const auto victim=(uint16_t)f.body[6]|((uint16_t)f.body[7]<<8);
                    auto ref=std::find_if(f.refs.begin(),f.refs.end(),[&](auto&r){return r.kind==kind&&r.scope==255&&r.id==victim;});
                    if(!kind||ref==f.refs.end())return deny(Reason::Malformed);
                    auto*r=Find(*ref);if(!r||!r->live||!Current(*ref))return deny(Reason::Generation);
                    if(f.body[5]!=from||r->owner!=from||r->ownerConnection!=connection||r->ownerWorld!=f.world)return deny(Reason::Owner);
                    if(!f.lease||f.lease!=r->lease)return deny(Reason::Lease);
                }
            }
            // A held native owner must receive the real hit now, before any
            // new owner can commit a cached pre-hit snapshot. Retained story
            // passengers are not held and do not trigger this transition.
            std::vector<uint32_t> affected;
            for(auto&ref:f.refs)if(auto*r=Find(ref);r&&r->live&&Current(ref)&&r->pending){auto t=transactions.find(r->pending);if(t!=transactions.end()&&TransferMember(t->second,ref)&&std::find(affected.begin(),affected.end(),t->first)==affected.end())affected.push_back(t->first);}
            uint8_t victimKind=ENTITY_VEHICLE;uint16_t victimId=0;
            Reader p(f.body,f.bodySize);if(msg==74){p.U32();auto kind=p.U8();p.U8();victimKind=kind==1?ENTITY_PED:kind==2?ENTITY_VEHICLE:0;victimId=p.U16();}else{p.U16();victimId=p.U16();}
            bool echo=false;if(p.Ok()&&victimKind)for(auto&ref:f.refs)if(ref.kind==victimKind&&ref.id==victimId)if(auto*r=Find(ref);r&&r->live&&Current(ref)&&r->pending&&r->owner==from&&r->ownerConnection==connection&&r->ownerWorld==f.world){auto t=transactions.find(r->pending);if(t!=transactions.end()&&TransferMember(t->second,ref))echo=true;}
            for(auto id:affected)Abort(id,Reason::Interaction);
            return {true,Reason::Ok,f.lease,echo};
        }
        auto kind=PrimaryKind(msg);
        // Host's explicit story deletion is independent of the physical driver.
        bool storyRemove=msg==155;
        if(storyRemove){if(!host)return deny(Reason::Owner);kind=ENTITY_VEHICLE;}
        if(!kind)return {true,Reason::Ok,0};
        if(f.bodySize<2)return deny(Reason::Malformed);
        uint16_t id=(uint16_t)f.body[0]|((uint16_t)f.body[1]<<8);
        auto at=std::find_if(f.refs.begin(),f.refs.end(),[&](auto&r){return r.kind==kind&&r.scope==255&&r.id==id;});
        if(at==f.refs.end()||!Valid(*at))return deny(Reason::Malformed);
        const bool birth=msg==80||msg==90,remove=msg==81||msg==91||storyRemove;
        if(birth&&!(at->flags&ENTITY_BIRTH))return deny(Reason::Malformed);
        if(remove&&!(at->flags&ENTITY_REMOVE))return deny(Reason::Malformed);
        for(auto&ref:f.refs)if(ref.kind==ENTITY_VEHICLE||ref.kind==ENTITY_PED){
            if(!Valid(ref))return deny(Reason::Malformed);
            if(auto*r=Find(ref);r&&(!r->live||!Current(ref))&&!(ref.flags&ENTITY_OPTIONAL))return deny(Reason::Retired);
            auto known=current.find(ref.Key());if(known!=current.end()&&known->second!=ref.generation&&!(ref.flags&ENTITY_OPTIONAL)&&!(birth&&ref.Key()==at->Key()))return deny(Reason::Generation);
        }
        // Decode the relationship before changing the ledger. A malformed
        // CREATE must not leave a canonical ghost behind.
        uint16_t vehicle=0;uint64_t vehicleGeneration=0;int8_t seat=VehicleSeat;bool story=false;
        bool pedState=kind==ENTITY_PED&&(msg==90||msg==92||msg==167);
        std::vector<Member> crew;bool vehicleState=msg==80||msg==82||msg==65;
        if(msg==80&&(f.bodySize<38||f.body[4]!=from))return deny(Reason::Malformed);
        if(pedState){Reader p(f.body,f.bodySize);p.U16();if(msg==90){p.U16();p.U8();p.Str(16);}
            uint8_t prefix[34]{};p.Raw(prefix,sizeof prefix);auto flags=p.U8();story=(p.U8()&16)!=0;vehicle=p.U16();seat=p.I8();
            if(!p.Ok()||seat< -1||seat>7)return deny(Reason::Malformed);
            if((flags&4)&&!(flags&1)){auto v=std::find_if(f.refs.begin(),f.refs.end(),[&](auto&r){return r.kind==ENTITY_VEHICLE&&r.id==vehicle;});if(!vehicle||v==f.refs.end())return deny(Reason::Group);vehicleGeneration=v->generation;}
            else{vehicle=0;seat=VehicleSeat;}
        }
        if(vehicleState){Reader p(f.body,f.bodySize);uint8_t ignored[37]{};if(msg==80)p.Raw(ignored,37);else{p.U16();p.U32();}
            auto n=p.U8();if(n>9)return deny(Reason::Group);std::array<bool,9> seats{};
            for(unsigned i=0;i<n;++i){auto ped=p.U16();auto generation=p.U64();auto slot=p.I8();if(!ped||!generation||slot< -1||slot>7||seats[slot+1])return deny(Reason::Group);seats[slot+1]=true;
                auto link=std::find_if(f.refs.begin(),f.refs.end(),[&](auto&r){return r.kind==ENTITY_PED&&r.id==ped&&r.generation==generation;});if(link==f.refs.end())return deny(Reason::Group);
                for(auto&m:crew)if(m.ref.id==ped)return deny(Reason::Group);Member m;m.ref=*link;m.ref.flags=0;m.seat=slot;crew.push_back(m);
            }if(!p.Ok())return deny(Reason::Malformed);
        }
        auto*record=Find(*at);
        if(!record){
            if(!birth)return deny(Reason::Missing);
            if(Retired(*at))return deny(Reason::Retired);
            if((id>>12)!=from+1||f.lease!=0)return deny(Reason::Owner);
            auto old=current.find(at->Key());if(old!=current.end()){auto previous=records.find({at->Key(),old->second});if(previous!=records.end()&&previous->second.live)return deny(Reason::Generation);}
            if(records.size()>=8192){for(auto i=records.begin();i!=records.end();){if(!i->second.live)i=records.erase(i);else ++i;}}
            if(records.size()>=8192)return deny(Reason::Limit); // concurrent live registry bound, not lifetime churn
            Record r;r.ref=*at;r.ref.flags=0;r.creator=r.owner=from;r.creatorConnection=r.ownerConnection=connection;r.creatorWorld=r.ownerWorld=f.world;
            auto pair=records.emplace(Identity{at->Key(),at->generation},r);record=&pair.first->second;current[at->Key()]=at->generation;
        }else{
            if(!record->live||!Current(*at))return deny(Reason::Retired);
            if(!storyRemove&&(record->owner!=from||record->ownerConnection!=connection||record->ownerWorld!=f.world))return deny(Reason::Owner);
            if(!storyRemove&&f.lease!=record->lease)return deny(Reason::Lease);
            if(record->pending){auto t=transactions.find(record->pending);if(t!=transactions.end()&&t->second.prepared&&TransferMember(t->second,*at))return deny(Reason::Busy);}
        }
        if(pedState){
            if(Prepared(record->pending)&&(record->vehicle!=vehicle||record->vehicleGeneration!=vehicleGeneration||record->seat!=seat))Abort(record->pending,Reason::Group);
            record->vehicle=vehicle;record->vehicleGeneration=vehicleGeneration;record->seat=seat;
            record->story=story;
        }
        if(vehicleState){if(Prepared(record->pending)&&!SameCrew(record->crew,crew))Abort(record->pending,Reason::Group);record->crew=std::move(crew);if(msg==80)record->playerUsed=f.body[36]!=0;}
        const auto acceptedLease=record->lease;
        if(remove){if(record->pending)Abort(record->pending,Reason::Retired);Retire(*record);}
        if(birth)Grant(*record);
        return {true,Reason::Ok,acceptedLease};
    }
    void Handle(uint8_t from,uint32_t connection,const Command&c,uint64_t now){
        Tick(now);
        if(from>=3||!peers[from].joined||peers[from].connection!=connection){Reject(from,c,Reason::Connection);return;}
        if(!c.world||peers[from].world!=c.world){Reject(from,c,Reason::World);return;}
        if(c.op==ClientOp::Lookup){Lookup(from,c);return;}
        if(c.op==ClientOp::Request){Begin(from,c,now);return;}
        auto i=transactions.find(c.transaction);
        if(i==transactions.end()){Reject(from,c,Reason::Missing);return;}
        auto&t=i->second;
        if(c.request!=t.request||c.target!=t.target){Reject(from,c,Reason::Group);return;}
        if(c.op!=ClientOp::StopAck&&!SameMembers(c.members,t.members)){Reject(from,c,Reason::Group);return;}
        if(c.op==ClientOp::Cancel){if(from==t.source||from==t.owner||from==t.target)Abort(t.id,Reason::Cancelled);else Reject(from,c,Reason::Owner);return;}
        if(c.op==ClientOp::StopAck){
            if(from!=t.owner||connection!=t.ownerConnection||c.world!=t.ownerWorld||t.prepared){Reject(from,c,Reason::Owner);return;}
            // A real player claim can displace an ambient driver during Stop.
            // Its reliable Foot + fresh car crew precede this ACK. Refresh only
            // this authenticated giver's complete current crew, never fabricate
            // a seated NPC or transfer the displaced Foot actor to the claimant.
            if(c.members.empty()||!c.members.front().SameIdentity(t.members.front())||CheckGroup(c.members,t.owner)!=Reason::Ok){Reject(from,c,Reason::Group);return;}
            for(auto&m:c.members){auto*r=Find(m.ref);if(r->pending&&r->pending!=t.id){Reject(from,c,Reason::Busy);return;}}
            for(auto&m:t.members){bool kept=false;for(auto&n:c.members)kept|=m.SameIdentity(n);if(!kept){auto&r=*Find(m.ref);r.pending=0;if(m.transfer)Bump(r.lease);Grant(r);}}
            t.members=c.members;for(auto&m:t.members)Find(m.ref)->pending=t.id;
            t.prepared=true;t.deadline=now+TransactionMs;Emit(t,ServerOp::Prepare);return;
        }
        if(c.op!=ClientOp::NativeReady||!t.prepared||from!=t.target||connection!=t.targetConnection||c.world!=t.targetWorld){Reject(from,c,Reason::Owner);return;}
        if(CheckGroup(t.members,t.owner)!=Reason::Ok){Abort(t.id,Reason::Group);return;}
        // All members are checked before any owner changes. Native-ready only
        // authorizes transfer members; retained story passengers keep their AI.
        for(auto&m:t.members)if(m.transfer){auto&r=*Find(m.ref);r.owner=t.target;r.ownerConnection=t.targetConnection;r.ownerWorld=t.targetWorld;Bump(r.lease);}
        auto complete=t;for(auto&m:complete.members){auto&r=*Find(m.ref);r.pending=0;m.lease=r.lease;m.owner=r.owner;}
        complete.owner=complete.target;complete.ownerConnection=complete.targetConnection;complete.ownerWorld=complete.targetWorld;
        transactions.erase(i);Emit(complete,ServerOp::Commit);
    }
private:
    uint32_t serial=0;
    std::vector<Delivery> outbox;
    static void Bump(uint32_t&lease){if(++lease==0)++lease;}
    static bool Valid(const EntityRef&r){return (r.kind==ENTITY_VEHICLE||r.kind==ENTITY_PED)&&r.scope==255&&r.id&&r.id!=0xFFFF&&r.generation&&r.flags<=15;}
    bool Current(const EntityRef&r)const{auto i=current.find(r.Key());return i!=current.end()&&i->second==r.generation;}
    bool Retired(const EntityRef&r)const{auto i=retired.find(r.Key());if(i==retired.end())return false;for(auto old:i->second)if(old&&(old>>32)==(r.generation>>32)&&r.generation<=old)return true;return false;}
    void Retire(Record&r){
        if(r.pending)Abort(r.pending,Reason::Retired);
        auto&history=retired[r.ref.Key()];bool found=false;for(auto&old:history)if(old&&(old>>32)==(r.ref.generation>>32)){old=std::max(old,r.ref.generation);found=true;break;}if(!found){std::rotate(history.rbegin(),history.rbegin()+1,history.rend());history[0]=r.ref.generation;}
        r.live=false;r.pending=0;Bump(r.lease);
        if(r.ref.kind==ENTITY_PED){
            // A world reset can retain a player-used car but retire its old
            // native NPC. Remove that exact dead dependency, not surviving
            // cross-owner story passengers and not another generation's seat.
            for(auto&[key,car]:records)if(car.live&&car.ref.kind==ENTITY_VEHICLE)
                car.crew.erase(std::remove_if(car.crew.begin(),car.crew.end(),[&](auto&m){return m.ref.Key()==r.ref.Key()&&m.ref.generation==r.ref.generation;}),car.crew.end());
        }
    }
    static bool SameMembers(const std::vector<Member>&a,const std::vector<Member>&b){if(a.size()!=b.size())return false;for(size_t i=0;i<a.size();++i)if(!a[i].SameIdentity(b[i])||a[i].lease!=b[i].lease||a[i].owner!=b[i].owner||a[i].transfer!=b[i].transfer||a[i].seat!=b[i].seat)return false;return true;}
    static bool SameCrew(const std::vector<Member>&a,const std::vector<Member>&b){if(a.size()!=b.size())return false;for(auto&m:a){bool found=false;for(auto&n:b)found|=m.SameIdentity(n)&&m.seat==n.seat;if(!found)return false;}return true;}
    static bool TransferMember(const Transaction&t,const EntityRef&r){for(auto&m:t.members)if(m.transfer&&m.ref.Key()==r.Key()&&m.ref.generation==r.generation)return true;return false;}
    bool Prepared(uint32_t id)const{auto i=transactions.find(id);return id&&i!=transactions.end()&&i->second.prepared;}
    void Grant(const Record&r,uint8_t to=NoOwner){Notice n;n.op=ServerOp::Commit;n.owner=n.target=r.owner;n.ownerConnection=n.targetConnection=r.ownerConnection;n.ownerWorld=n.targetWorld=r.ownerWorld;n.members.push_back({r.ref,r.lease,r.owner,true,r.seat});outbox.push_back({to,std::move(n)});}
    void Emit(const Transaction&t,ServerOp op,Reason reason=Reason::Ok){Notice n;n.op=op;n.request=t.request;n.transaction=t.id;n.source=t.source;n.owner=t.owner;n.target=t.target;n.ownerConnection=t.ownerConnection;n.ownerWorld=t.ownerWorld;n.targetConnection=t.targetConnection;n.targetWorld=t.targetWorld;n.reason=reason;n.members=t.members;outbox.push_back({NoOwner,std::move(n)});}
    void Reject(uint8_t to,const Command&c,Reason reason){Notice n;n.op=ServerOp::Reject;n.request=c.request;n.transaction=c.transaction;n.source=to;n.target=c.target;n.reason=reason;if(c.target<3&&peers[c.target].joined){n.targetConnection=peers[c.target].connection;n.targetWorld=peers[c.target].world;}bool first=true;for(auto&m:c.members)if(auto*r=Find(m.ref)){Member canonical=m;canonical.lease=r->lease;canonical.owner=r->owner;n.members.push_back(canonical);if(first){first=false;n.owner=r->owner;n.ownerConnection=r->ownerConnection;n.ownerWorld=r->ownerWorld;}}outbox.push_back({to,std::move(n)});}
    void Lookup(uint8_t from,const Command&c){
        // A reconnect resolves one exact saved car or NPC identity. This query never
        // quiesces, changes a lease, claims an ID or creates a native entity.
        if(!c.request||c.transaction||c.target!=from||c.voluntary||c.members.size()!=1||!Valid(c.members.front().ref)){Reject(from,c,Reason::Malformed);return;}
        Notice n;n.op=ServerOp::Lookup;n.request=c.request;n.source=n.target=from;n.targetConnection=peers[from].connection;n.targetWorld=peers[from].world;
        auto ref=c.members.front().ref;ref.flags=0;auto*r=Find(ref);
        if(!r||!r->live||!Current(ref)){
            auto previous=current.find(ref.Key());
            n.reason=(r||Retired(ref)||(previous!=current.end()&&previous->second!=ref.generation))?Reason::Retired:Reason::Missing;
            n.members.push_back({ref,0,NoOwner,false,VehicleSeat});
        }else{
            n.owner=r->owner;n.ownerConnection=r->ownerConnection;n.ownerWorld=r->ownerWorld;n.reason=r->pending?Reason::Busy:Reason::Ok;
            n.members.push_back({r->ref,r->lease,r->owner,true,r->ref.kind==ENTITY_PED?r->seat:VehicleSeat});
            if(r->ref.kind==ENTITY_VEHICLE)LookupCrew(n,*r,nullptr);
            else if(r->vehicle){
                EntityRef parent{ENTITY_VEHICLE,255,r->vehicle,r->vehicleGeneration,0};auto*car=Find(parent);
                if(!car||!car->live||!Current(parent))n.reason=Reason::Busy;
                else{
                    bool reciprocal=false;for(auto&seat:car->crew)reciprocal|=seat.ref.Key()==r->ref.Key()&&seat.ref.generation==r->ref.generation&&seat.seat==r->seat;
                    if(!reciprocal||car->pending)n.reason=Reason::Busy;
                    n.members.push_back({car->ref,car->lease,car->owner,true,VehicleSeat});
                    LookupCrew(n,*car,&r->ref);
                }
            }
        }
        outbox.push_back({from,std::move(n)});
    }
    void LookupCrew(Notice&n,const Record&car,const EntityRef*exclude){
        for(auto&seat:car.crew){
            if(exclude&&seat.ref.Key()==exclude->Key()&&seat.ref.generation==exclude->generation)continue;
            if(n.members.size()>=MaxGroup){n.reason=Reason::Busy;break;}
            auto*ped=Find(seat.ref);if(!ped||!ped->live||!Current(seat.ref)){n.reason=Reason::Busy;continue;}
            if(ped->vehicle!=car.ref.id||ped->vehicleGeneration!=car.ref.generation||ped->seat!=seat.seat||ped->pending)n.reason=Reason::Busy;
            n.members.push_back({ped->ref,ped->lease,ped->owner,ped->owner==car.owner&&!(ped->story&&seat.seat>=0),seat.seat});
        }
    }
    Reason CheckGroup(const std::vector<Member>&members,uint8_t owner)const{
        if(members.empty()||members.size()>MaxGroup)return Reason::Group;
        const auto&root=members.front();if(!root.transfer)return Reason::Group;
        std::array<bool,9> seats{};
        for(size_t i=0;i<members.size();++i){const auto&m=members[i];auto*r=Find(m.ref);
            if(!Valid(m.ref)||!r||!r->live||!Current(m.ref))return Reason::Generation;
            if(r->lease!=m.lease)return Reason::Lease;if(r->owner!=m.owner)return Reason::Owner;
            if(m.transfer&&r->owner!=owner)return Reason::Owner;
            if(!m.transfer&&r->owner==owner&&!(r->story&&m.seat>=0))return Reason::Owner;
            for(size_t j=0;j<i;++j)if(m.SameIdentity(members[j]))return Reason::Group;
            if(root.ref.kind==ENTITY_PED){if(members.size()!=1||r->vehicle||m.seat!=VehicleSeat)return Reason::Group;}
            else if(root.ref.kind==ENTITY_VEHICLE){
                if(!i){if(m.seat!=VehicleSeat)return Reason::Group;continue;}
                if(m.ref.kind!=ENTITY_PED||r->vehicle!=root.ref.id||r->vehicleGeneration!=root.ref.generation||r->seat!=m.seat||m.seat< -1||m.seat>7)return Reason::Group;
                // Cross-owner passengers are dependencies. A cross-owner AI
                // driver needs a distinct simulation contract and is rejected.
                if(m.seat==-1&&!m.transfer)return Reason::Group;
                if(seats[m.seat+1])return Reason::Group;seats[m.seat+1]=true;
            }else return Reason::Group;
        }
        if(root.ref.kind==ENTITY_VEHICLE){
            auto*car=Find(root.ref);if(!car||car->crew.size()+1!=members.size())return Reason::Group;
            for(auto&expected:car->crew){bool found=false;for(auto&m:members)found|=m.SameIdentity(expected)&&m.seat==expected.seat;if(!found)return Reason::Group;}
            for(auto&[key,r]:records)if(r.live&&Current(r.ref)&&r.ref.kind==ENTITY_PED&&r.vehicle==root.ref.id&&r.vehicleGeneration==root.ref.generation){bool found=false;for(auto&m:members)found|=m.SameIdentity(Member{r.ref,r.lease,r.owner,true,r.seat});if(!found)return Reason::Group;}
        }
        return Reason::Ok;
    }
    void Begin(uint8_t from,const Command&c,uint64_t now){
        for(auto&[id,t]:transactions)if(t.source==from&&t.request==c.request){Reject(from,c,Reason::Busy);return;}
        if(!c.request||c.transaction||c.target>=3||!peers[c.target].joined||!peers[c.target].world){Reject(from,c,Reason::Target);return;}
        if(c.members.empty()){Reject(from,c,Reason::Group);return;}
        auto*root=Find(c.members.front().ref);if(!root){Reject(from,c,Reason::Missing);return;}
        auto why=CheckGroup(c.members,root->owner);if(why!=Reason::Ok){Reject(from,c,why);return;}
        if(c.target==root->owner||(c.voluntary&&from!=root->owner)||(!c.voluntary&&from!=c.target)){Reject(from,c,Reason::Owner);return;}
        for(auto&m:c.members)if(Find(m.ref)->pending){Reject(from,c,Reason::Busy);return;}
        Transaction t;if(++serial==0)++serial;t.id=serial;t.request=c.request;t.source=from;t.owner=root->owner;t.target=c.target;t.ownerConnection=root->ownerConnection;t.ownerWorld=root->ownerWorld;t.targetConnection=peers[c.target].connection;t.targetWorld=peers[c.target].world;t.members=c.members;t.deadline=now+TransactionMs;
        t.prepared=c.voluntary||t.owner==NoOwner;
        transactions.emplace(t.id,t);for(auto&m:t.members)Find(m.ref)->pending=t.id;
        Emit(t,t.prepared?ServerOp::Prepare:ServerOp::Stop);
    }
    void Abort(uint32_t id,Reason reason){
        auto i=transactions.find(id);if(i==transactions.end())return;auto t=i->second;transactions.erase(i);
        bool ownerAlive=t.owner<3&&peers[t.owner].joined&&peers[t.owner].connection==t.ownerConnection&&peers[t.owner].world==t.ownerWorld;
        for(auto&m:t.members)if(auto*r=Find(m.ref)){r->pending=0;if(m.transfer){if(!ownerAlive){r->owner=NoOwner;r->ownerConnection=r->ownerWorld=0;}Bump(r->lease);}m.lease=r->lease;m.owner=r->owner;}
        if(!ownerAlive){t.owner=NoOwner;t.ownerConnection=t.ownerWorld=0;}
        Emit(t,ServerOp::Abort,reason);
    }
    void WorldChanged(uint8_t peer,uint32_t world,const std::vector<uint16_t>&keep){
        std::vector<uint32_t> abort;for(auto&[id,t]:transactions)if(t.owner==peer||t.target==peer||t.source==peer)abort.push_back(id);for(auto id:abort)Abort(id,Reason::WorldChanged);
        peers[peer].world=world;
        for(auto&[key,r]:records)if(r.live&&r.owner==peer){
            bool kept=r.ref.kind==ENTITY_VEHICLE&&std::find(keep.begin(),keep.end(),r.ref.id)!=keep.end();
            if(kept){r.ownerWorld=world;Bump(r.lease);Grant(r);}
            else if(r.ref.kind==ENTITY_VEHICLE&&r.playerUsed){r.owner=NoOwner;r.ownerConnection=r.ownerWorld=0;Bump(r.lease);Grant(r);}
            else Retire(r);
        }
    }
};
}
