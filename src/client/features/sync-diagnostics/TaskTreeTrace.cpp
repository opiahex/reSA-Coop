// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
#include "features/sync-diagnostics/TaskTreeTrace.h"
namespace coop::task_lifecycle::tree_trace {
namespace {
using flight::Metric;
bool ValidLink(const tree::Link&l,uint16_t count) noexcept {
 return unsigned(l.state)<unsigned(tree::State::Present)&&
        (l.state==tree::State::Node?l.node<count:l.node==tree::kNoNode);
}
bool Valid(const tree::Snapshot&s) noexcept {
 if(s.count>tree::kNodes||!s.subject.kind.available||
    (s.subject.kind.value!=SubjectKind::Player&&s.subject.kind.value!=SubjectKind::Npc)||
    !s.subject.nativePedRef.available||s.subject.localPeer>2)return false;
 for(const auto&l:s.primary)if(!ValidLink(l,s.count))return false;
 for(const auto&l:s.secondary)if(!ValidLink(l,s.count))return false;
 for(uint16_t i=0;i<s.count;++i){const auto&n=s.nodes[i];
  if(n.depth>=tree::kDepth||!ValidLink(n.parent,s.count)||!ValidLink(n.subtask,s.count))return false;
 }
 const tree::EventObservation*events[]={&s.history.nonTemporary,&s.history.temporary,&s.history.stored,&s.history.current};
 for(const auto*e:events)if(unsigned(e->state)>unsigned(tree::State::Present))return false;
 return unsigned(s.history.lastAborted.state)<=unsigned(tree::State::Present)&&
        ValidLink(s.history.lastAborted.inTree,s.count);
}
class Writer {
 const tree::Snapshot&s;
 Span parent;
 Result r{};
 void U(flight::Event&e,Metric key,uint64_t value)noexcept {if(!flight::AddUnsigned(e,key,value))r.metadataInvalid=true;}
 void I(flight::Event&e,Metric key,Known<int32_t> value)noexcept {if(value.available&&!flight::AddSigned(e,key,value.value))r.metadataInvalid=true;}
 flight::Event Row(Stage stage)noexcept {
  flight::Event e{};e.context=s.subject.context;e.localPeer=s.subject.localPeer;e.nativeHandle=s.subject.nativePedRef.value;
  if(s.subject.motionEpoch.available)e.motionEpoch=s.subject.motionEpoch.value;
  if(s.subject.owner.available){e.owner=s.subject.owner.value;e.detailKnown|=flight::OwnerKnown;}
  if(s.subject.ownerEpoch.available){e.ownerEpoch=s.subject.ownerEpoch.value;e.detailKnown|=flight::OwnerEpochKnown;}
  e.stage=uint16_t(stage);e.span=r.span;e.detailKnown|=flight::SpanKnown|flight::CriticalStateHint;
  U(e,Metric::TaskRecordingToken,r.recordingToken);
  if(parent.available)U(e,Metric::TaskParentSpan,parent.value);
  return e;
 }
 bool Put(flight::Event e)noexcept {
  if(flight::ActiveRecordingToken()!=r.recordingToken){r.recordingStable=false;return false;}
  ++r.attempted;const bool admitted=flight::Record(e);if(admitted)++r.admitted;
  if(flight::ActiveRecordingToken()!=r.recordingToken)r.recordingStable=false;
  return admitted&&r.recordingStable;
 }
 void Link(flight::Event&e,const tree::Link&l)noexcept {
  U(e,Metric::TaskTreeLinkState,unsigned(l.state));
  if(l.state==tree::State::Node)U(e,Metric::TaskTreeTargetNode,l.node);
 }
 void Root(const tree::Link&l,unsigned kind,unsigned slot)noexcept {
  auto e=Row(Stage::Root);U(e,Metric::TaskTreeSlotKind,kind);
  if(!flight::AddSigned(e,Metric::TaskSlot,slot))r.metadataInvalid=true;
  Link(e,l);(void)Put(e);
 }
 void Event(const tree::EventObservation&o,unsigned role)noexcept {
  auto e=Row(Stage::History);U(e,Metric::TaskHistoryRole,role);U(e,Metric::TaskTreeLinkState,unsigned(o.state));
  I(e,Metric::TaskEventType,o.type);I(e,Metric::TaskEventPriority,o.priority);(void)Put(e);
 }
public:
 Writer(const tree::Snapshot&snapshot,uint32_t token,Span p)noexcept:s(snapshot),parent(p){r.recordingToken=token;r.inputComplete=s.complete;}
 Result Run()noexcept {
  if(!r.recordingToken||flight::ActiveRecordingToken()!=r.recordingToken)return r;
  if(!Valid(s)||(parent.available&&(!parent.value||parent.recordingToken!=r.recordingToken))){r.invalidInput=true;return r;}
  r.span=flight::NextDiagnosticSpan();
  if(!r.span||flight::ActiveRecordingToken()!=r.recordingToken)return r;
  r.recordingStable=true;
  auto begin=Row(Stage::Begin);U(begin,Metric::TaskSubjectKind,unsigned(s.subject.kind.value));
  U(begin,Metric::TaskTreeFlags,s.flags);U(begin,Metric::TaskTreeComplete,s.complete?1:0);U(begin,Metric::TaskTreeNodeCount,s.count);
  r.beginAdmitted=Put(begin);
  for(unsigned i=0;i<tree::kPrimary&&r.recordingStable;++i)Root(s.primary[i],0,i);
  for(unsigned i=0;i<tree::kSecondary&&r.recordingStable;++i)Root(s.secondary[i],1,i);
  for(uint16_t i=0;i<s.count&&r.recordingStable;++i){const auto&n=s.nodes[i];
   auto e=Row(Stage::Node);U(e,Metric::TaskTreeNode,i);U(e,Metric::TaskTreeDepth,n.depth);I(e,Metric::TaskType,n.type);
   if(n.simple.available)U(e,Metric::TaskTreeSimple,n.simple.value?1:0);(void)Put(e);
   const tree::Link*edges[]={&n.parent,&n.subtask};
   for(unsigned kind=0;kind<2&&r.recordingStable;++kind){auto edge=Row(Stage::Edge);U(edge,Metric::TaskTreeNode,i);U(edge,Metric::TaskTreeEdgeKind,kind);Link(edge,*edges[kind]);(void)Put(edge);}
  }
  if(r.recordingStable)Event(s.history.nonTemporary,0);
  if(r.recordingStable)Event(s.history.temporary,1);
  if(r.recordingStable)Event(s.history.stored,2);
  if(r.recordingStable)Event(s.history.current,3);
  if(r.recordingStable){const auto&a=s.history.lastAborted;auto e=Row(Stage::History);U(e,Metric::TaskHistoryRole,4);
   U(e,Metric::TaskTreeLinkState,unsigned(a.state));I(e,Metric::TaskType,a.type);
   if(a.inTree.state==tree::State::Node)U(e,Metric::TaskTreeTargetNode,a.inTree.node);(void)Put(e);
  }
  if(r.recordingStable){auto e=Row(Stage::Edge);U(e,Metric::TaskHistoryRole,4);U(e,Metric::TaskTreeEdgeKind,2);Link(e,s.history.lastAborted.inTree);(void)Put(e);}
  if(r.recordingStable){auto e=Row(Stage::End);U(e,Metric::TaskTreeFlags,s.flags);U(e,Metric::TaskTreeComplete,s.complete?1:0);
   U(e,Metric::TaskTreeNodeCount,s.count);U(e,Metric::TaskTreeRowsAttempted,r.attempted);U(e,Metric::TaskTreeRowsAdmitted,r.admitted);r.endAdmitted=Put(e);
  }
  return r;
 }
};
}
Result RecordSnapshot(const tree::Snapshot&snapshot,uint32_t token,Span parent)noexcept {return Writer(snapshot,token,parent).Run();}
}
