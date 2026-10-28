#pragma once
// Memory patching and function hooking helpers for gta_sa.exe 1.0 US.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace coop::mem {

bool Init();      // MH_Initialize
void Shutdown();  // disable & remove all hooks

enum class WriteFailure : uint8_t {
    None, InvalidRange, QueryFailed, UnsupportedRegion, ProtectFailed, ReadFailed, Conflict, VerifyFailed,
    CopyFailed, PartialCopy, RestoreFailed, FlushFailed
};
struct WriteResult {
    WriteFailure failure = WriteFailure::None;
    size_t bytesWritten = 0;
    bool changed = false;
    bool restored = false;
    bool flushed = false;
    uint32_t queryError = 0, protectError = 0, copyError = 0, restoreError = 0, flushError = 0;
    bool Ok() const { return failure == WriteFailure::None; }
};
enum class PatchState : uint8_t { Unchanged, Installed, Indeterminate };
struct RedirectResult {
    void* previous = nullptr;
    uint8_t originalBytes[5]{};
    uint8_t readbackBytes[5]{};
    bool readbackValid = false;
    WriteResult write{};
    PatchState state = PatchState::Unchanged;
};
struct SwapResult {
    void* previous = nullptr;
    void* readback = nullptr;
    bool readbackValid = false;
    WriteResult write{};
    PatchState state = PatchState::Unchanged;
};
struct SlotReadResult { void* value = nullptr; uint32_t error = 0; bool ok = false; };
// Sticky until process exit; an earlier failed write/protection restoration requires attention.
bool HasUnsafePatchFailure();
// Monotonic modulo 2^32 count of failed patch/hook attempts; compare snapshots around installs.
uint32_t PatchFailureSerial();

// Rejects invalid or mixed-protection regions. Failure after writing does not roll back bytes.
WriteResult TryWrite(uintptr_t addr, const void* data, size_t size);
WriteResult TryNop(uintptr_t addr, size_t size);
// Legacy wrappers log failures; callers needing status must use the checked variants.
void Write(uintptr_t addr, const void* data, size_t size);
template <typename T>
inline void Put(uintptr_t addr, T value) {
    Write(addr, &value, sizeof(T));
}
template <typename T>
inline T Get(uintptr_t addr) {
    return *reinterpret_cast<T*>(addr);
}
void Nop(uintptr_t addr, size_t size);
// Writes "jmp target" (5 bytes) at addr.
void Jump(uintptr_t addr, const void* target);
// Writes "ret N" at addr (useful to disable a function).
void Ret(uintptr_t addr, uint16_t popBytes = 0);

// Absolute target of a "call rel32" / "jmp rel32" instruction located at addr, or 0.
uintptr_t CallTarget(uintptr_t addr);

// Redirects an existing "call rel32" at addr to newTarget. The previous call target (which may
// already be another mod's hook) is stored in *previous so the detour can chain to it.
// Checked variants retain the original target even if a mutation is unconfirmed. In that
// case the patch may already be active: the caller must preserve the original chain.
// chain, when provided, is populated before writing so an immediately executed detour
// can call the original target even if a later protection or cache operation fails.
RedirectResult TryRedirectCall(uintptr_t addr, const void* newTarget, void** chain = nullptr);
// Publishes *previous before mutation. False does not guarantee unchanged bytes.
bool RedirectCall(uintptr_t addr, const void* newTarget, void** previous);

// Inline hook through MinHook. *original receives the trampoline.
bool Hook(uintptr_t addr, void* detour, void** original, const char* name);

template <typename F>
inline bool HookFn(uintptr_t addr, F detour, F* original, const char* name) {
    return Hook(addr, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), name);
}
template <typename F>
inline bool RedirectCallFn(uintptr_t addr, F detour, F* previous) {
    return RedirectCall(addr, reinterpret_cast<const void*>(detour), reinterpret_cast<void**>(previous));
}

SlotReadResult ReadVTableSlot(void** vtable, int index);
SwapResult TrySwapVTable(void** vtable, int index, void* fn, void* expected, bool checkExpected, void** chain = nullptr);
// Returns nullptr unless the write and readback were confirmed.
void* SwapVTable(void** vtable, int index, void* fn);

// Checks that the expected bytes are at addr (used to detect other versions / conflicting mods).
bool Check(uintptr_t addr, const void* expected, size_t size);

}  // namespace coop::mem
