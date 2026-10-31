// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <string>
#include "shared/lib/memory/HookInspector.h"
namespace coop::ui::diagnostic {
constexpr size_t kNativeRows=32,kNativeText=384;
inline std::string DiagnosticAge(uint64_t now,uint64_t stamp,bool known){
    return known&&stamp&&now>=stamp?std::to_string(now-stamp):"UNKNOWN";
}
enum class NativeKind:uint8_t { Network,LocalCar,Remote };
struct NativeRow {
    std::array<char,kNativeText> text{};
    uint8_t peer=255;
    uint16_t entity=0;
    bool peerKnown=false,entityKnown=false;
    uint64_t mission=0;
    bool missionKnown=false;
    NativeKind kind=NativeKind::Remote;
};
// Owns only copied values/text; no native pointers, handles or borrowed strings.
struct NativeSnapshot {
    std::array<NativeRow,kNativeRows> rows{};
    size_t count=0;
    bool truncated=false;
    bool nativeReady=false; // copied readiness from this actual owning-thread capture
    uint64_t mission=0;
    bool missionKnown=false;
    void Add(NativeKind kind,uint8_t peer,bool entityKnown,uint16_t entity,const char*fmt,...){
        if(count==rows.size()){truncated=true;return;}
        auto&r=rows[count++];r={};r.kind=kind;r.peer=peer;r.peerKnown=peer!=255;r.entityKnown=entityKnown;r.entity=entity;
        r.mission=mission;r.missionKnown=missionKnown;
        va_list ap;va_start(ap,fmt);int size=std::vsnprintf(r.text.data(),r.text.size(),fmt,ap);va_end(ap);
        if(size<0){r.text[0]=0;truncated=true;}else if(size>=int(r.text.size()))truncated=true;
    }
};
// Manual/UI capture remains game-thread owned. The independent thread check
// happens before any native/game/session access; copied cache carries its recording token.
bool CaptureNativeSnapshot(NativeSnapshot&out,uint32_t owningThread,uint16_t selectedEntity=0);
// Core-owned ready hint and recorder atomics only per frame; due native captures
// run at most once per1000ms, with an immediate first eligible frame/new token.
void UpdateAutomaticNativeContext(uint32_t owningThread,bool owningReadyHint);
// Writer-thread export of copied game-thread data plus historical registry.
// No live native entity reads. False means a missing/incomplete sidecar.
bool ExportDiagnosticContext(const wchar_t* snapshotPath) noexcept;
bool CacheDiagnosticInspection(const hookinspect::Sample&sample,bool requested) noexcept;
}
