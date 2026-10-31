// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
#include "features/sync-diagnostics/TaskTreeSnapshot.h"
namespace coop::task_lifecycle::tree {
namespace {
struct Internal {uintptr_t ref=0;uint64_t cookie=0;Ref parent{},child{};bool valid=false;State failure=State::Unavailable;};
struct EventInternal {uintptr_t ref=0;uint64_t cookie=0;bool valid=false;};
class Collector {
 const Services&s;Snapshot&o;Limits limits;std::array<Internal,kNodes>internal{};
 std::array<EventInternal,3>events{};uintptr_t abortedRef=0;uint64_t abortedCookie=0;bool abortedValid=false;
 bool subjectLost=false,budgetLost=false;
 bool Budget()noexcept{if(o.counters.serviceCalls>=limits.calls){o.flags|=CallLimit;budgetLost=true;return false;}++o.counters.serviceCalls;return true;}
 bool SubjectNow()noexcept{
  if(subjectLost||budgetLost||!Budget())return false;
  if(!s.subjectValid(s.user,o.subject)){o.flags|=SubjectChanged;subjectLost=true;return false;}return true;
 }
 bool Ready()noexcept{return SubjectNow()&&Budget();}
 void Failure(Read r)noexcept{
  ++o.counters.readFailures;if(r==Read::Changed)o.flags|=LifetimeChanged;
  else if(r==Read::Invalid)o.flags|=InvalidReference;else o.flags|=ReadUnavailable;
 }
 State FailureState(Read r)noexcept{return r==Read::Changed?State::Changed:r==Read::Invalid?State::Invalid:State::Unavailable;}
 Read ValidateTask(uintptr_t ref,uint64_t cookie)noexcept{
  if(!Ready())return Read::Unavailable;++o.counters.validations;auto r=s.taskValid(s.user,ref,cookie);if(r!=Read::Ok)Failure(r);return r;
 }
 Read ValidateEvent(uintptr_t ref,uint64_t cookie)noexcept{
  if(!Ready())return Read::Unavailable;++o.counters.validations;auto r=s.eventValid(s.user,ref,cookie);if(r!=Read::Ok)Failure(r);return r;
 }
 int Find(uintptr_t ref)const noexcept{for(uint16_t i=0;i<o.count;++i)if(internal[i].valid&&internal[i].ref==ref)return i;return -1;}
 bool ReadTask(uintptr_t ref,TaskFacts&facts,State&failure)noexcept{
  if(!Ready()){failure=State::Unavailable;return false;}++o.counters.taskReads;
  auto r=s.task(s.user,ref,facts);if(r!=Read::Ok){Failure(r);failure=FailureState(r);return false;}
  if(!facts.cookie.available){o.flags|=LifetimeUnknown;failure=State::Unavailable;return false;}
  r=ValidateTask(ref,facts.cookie.value);if(r!=Read::Ok){failure=FailureState(r);return false;}return true;
 }
 void Walk(Ref ref,Link&target)noexcept{
  std::array<uint16_t,kDepth>path{};uint16_t depth=0;Link*link=&target;
  while(!subjectLost&&!budgetLost){
   if(!ref.available){link->state=State::Unavailable;o.flags|=UnknownEdge;break;}
   if(!ref.value){link->state=State::Absent;break;}
   int found=Find(ref.value);
   if(found>=0){
    auto&in=internal[(size_t)found];auto v=ValidateTask(in.ref,in.cookie);
    if(v!=Read::Ok){link->state=FailureState(v);break;}
    *link={State::Node,(uint16_t)found};bool cycle=false;for(uint16_t i=0;i<depth;++i)cycle|=path[i]==found;
    if(cycle){o.flags|=SubtaskCycle;++o.counters.cycles;}else ++o.counters.aliases;
    break;
   }
   if(depth>=limits.depth){*link={State::Truncated,kNoNode};o.flags|=DepthLimit;++o.counters.depthTruncated;break;}
   if(o.count>=limits.nodes){*link={State::Truncated,kNoNode};o.flags|=NodeLimit;++o.counters.nodeTruncated;break;}
   TaskFacts f{};State failure=State::Unavailable;if(!ReadTask(ref.value,f,failure)){link->state=failure;break;}
   auto index=o.count++;auto&node=o.nodes[index];auto&in=internal[index];in={ref.value,f.cookie.value,f.parent,f.subtask,true};
   node.type=f.type;node.simple=f.simple;node.depth=depth;node.validatedAtRead=true;*link={State::Node,index};path[depth]=index;++depth;
   if(!node.type.available)o.flags|=TypeUnknown;
   if(!node.simple.available){o.flags|=TypeUnknown;node.subtask={State::Unavailable,kNoNode};break;}
   if(node.simple.value){
    if(!f.subtask.available){o.flags|=UnknownEdge;node.subtask={State::Unavailable,kNoNode};}
    else if(f.subtask.value){o.flags|=ContradictorySimple;node.subtask={State::Invalid,kNoNode};}
    else node.subtask={State::Absent,kNoNode};
    break;
   }
   ref=f.subtask;link=&node.subtask;
  }
 }
 void Parents()noexcept{
  for(uint16_t i=0;i<o.count;++i){auto&n=o.nodes[i];const auto&p=internal[i].parent;
   if(!p.available){o.flags|=UnknownEdge;n.parent={State::Unavailable,kNoNode};}
   else if(!p.value)n.parent={State::Absent,kNoNode};
   else{int parent=Find(p.value);if(parent<0){n.parent={State::External,kNoNode};o.flags|=ExternalParent;}else n.parent={State::Node,(uint16_t)parent};}
   if(n.subtask.state==State::Node){auto child=n.subtask.node;const auto&back=internal[child].parent;
    if(back.available&&back.value!=internal[i].ref)o.flags|=ParentMismatch;
   }
  }
  for(uint16_t i=0;i<o.count;++i){uint64_t visited=0;uint16_t at=i;
   for(uint16_t steps=0;steps<=o.count;++steps){uint64_t bit=uint64_t(1)<<at;
    if(visited&bit){o.flags|=ParentCycle;++o.counters.cycles;break;}visited|=bit;
    const auto&parent=o.nodes[at].parent;if(parent.state!=State::Node)break;at=parent.node;
   }
  }
 }
 EventObservation Event(Ref ref,EventInternal&in)noexcept{
  EventObservation out{};
  if(!ref.available){o.flags|=UnknownEdge;return out;}if(!ref.value){out.state=State::Absent;return out;}
  if(!Ready())return out;EventFacts f{};++o.counters.eventReads;auto r=s.event(s.user,ref.value,f);
  if(r!=Read::Ok){Failure(r);out.state=FailureState(r);return out;}
  if(!f.cookie.available){o.flags|=LifetimeUnknown;return out;}
  r=ValidateEvent(ref.value,f.cookie.value);if(r!=Read::Ok){out.state=FailureState(r);return out;}
  in={ref.value,f.cookie.value,true};out.state=State::Present;out.type=f.type;out.priority=f.priority;out.validatedAtRead=true;
  if(!f.priority.available)o.flags|=PriorityUnknown;
  if(!f.type.available)o.flags|=TypeUnknown;
  else{out.scriptCommand=Known<bool>::Of(f.type.value==32);if(out.scriptCommand.value)o.flags|=ScriptInHistory;}
  return out;
 }
 void Aborted(Ref ref)noexcept{
  auto&out=o.history.lastAborted;if(!ref.available){o.flags|=UnknownEdge;return;}if(!ref.value){out.state=State::Absent;return;}
  TaskFacts f{};State failure=State::Unavailable;if(!ReadTask(ref.value,f,failure)){out.state=failure;return;}
  abortedRef=ref.value;abortedCookie=f.cookie.value;abortedValid=true;out.state=State::Present;out.type=f.type;out.validatedAtRead=true;
  if(!f.type.available)o.flags|=TypeUnknown;auto i=Find(ref.value);if(i>=0)out.inTree={State::Node,(uint16_t)i};else out.inTree={State::External,kNoNode};
 }
 void FinalValidation()noexcept{
  for(uint16_t i=0;i<o.count&&!subjectLost&&!budgetLost;++i){auto&in=internal[i];auto r=ValidateTask(in.ref,in.cookie);
   if(r!=Read::Ok){in.valid=false;in.failure=FailureState(r);o.nodes[i]={};o.nodes[i].parent.state=o.nodes[i].subtask.state=in.failure;}
  }
  auto invalidate=[&](Link&l){if(l.state==State::Node&&!internal[l.node].valid){l={internal[l.node].failure,kNoNode};}};
  for(auto&l:o.primary)invalidate(l);for(auto&l:o.secondary)invalidate(l);
  for(uint16_t i=0;i<o.count;++i){invalidate(o.nodes[i].parent);invalidate(o.nodes[i].subtask);}
  invalidate(o.history.lastAborted.inTree);
  std::array<EventObservation*,3>targets{{&o.history.nonTemporary,&o.history.temporary,&o.history.stored}};
  for(size_t i=0;i<events.size()&&!subjectLost&&!budgetLost;++i)if(events[i].valid){auto r=ValidateEvent(events[i].ref,events[i].cookie);if(r!=Read::Ok){*targets[i]={};targets[i]->state=FailureState(r);}}
  if(abortedValid&&!subjectLost&&!budgetLost){auto r=ValidateTask(abortedRef,abortedCookie);if(r!=Read::Ok){o.history.lastAborted={};o.history.lastAborted.state=FailureState(r);}}
 }
 void CurrentHistory()noexcept{
  // Exactly the native priority rule: non-null temporary, otherwise non-temp.
  // Unknown/invalid temporary must not silently fall back to non-temp.
  const auto&t=o.history.temporary;o.history.current=t.state==State::Absent?o.history.nonTemporary:t;
 }
public:
 Collector(const Services&services,Snapshot&out,Limits l)noexcept:s(services),o(out),limits(l){}
 void Run()noexcept{
  if(!SubjectNow())return;
  Roots roots{};
  if(!Ready())return;auto r=s.roots(s.user,roots);if(r!=Read::Ok){o.flags|=RootsUnavailable;Failure(r);}else{
   for(size_t i=0;i<kPrimary&&!subjectLost&&!budgetLost;++i)Walk(roots.primary[i],o.primary[i]);
   for(size_t i=0;i<kSecondary&&!subjectLost&&!budgetLost;++i)Walk(roots.secondary[i],o.secondary[i]);
   Parents();
  }
  HistoryRefs history{};
  if(Ready()){r=s.history(s.user,history);if(r!=Read::Ok){o.flags|=HistoryUnavailable;Failure(r);}else{
   o.history.nonTemporary=Event(history.nonTemporary,events[0]);o.history.temporary=Event(history.temporary,events[1]);o.history.stored=Event(history.stored,events[2]);Aborted(history.lastAbortedTask);
  }}
  FinalValidation();CurrentHistory();
  if(!SubjectNow()){
   auto flags=o.flags;auto counters=o.counters;auto subject=o.subject;o={};o.flags=flags;o.counters=counters;o.subject=subject;
  }
  o.complete=o.flags==0;
 }
};
}
void Capture(const Subject&subject,const Services&services,Snapshot&out,Limits limits)noexcept{
 out={};out.subject=subject;
 if(!subject.kind.available||(subject.kind.value!=SubjectKind::Player&&subject.kind.value!=SubjectKind::Npc)||!subject.nativePedRef.available||
    !limits.nodes||limits.nodes>kNodes||!limits.depth||limits.depth>kDepth||!limits.calls||limits.calls>kCalls||
    !services.subjectValid||!services.roots||!services.history||!services.task||!services.event||!services.taskValid||!services.eventValid){out.flags=InvalidInput;return;}
 Collector(services,out,limits).Run();
}
}
