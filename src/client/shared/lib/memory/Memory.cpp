#include "shared/lib/memory/Memory.h"

#include <MinHook.h>
#include <windows.h>
#include <cstdio>
#include <limits>

#include "common/lib/log/Log.h"
#include "shared/lib/memory/HookRegistry.h"

namespace coop::mem {

bool Init() {
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        LOGE("MinHook init failed: %s", MH_StatusToString(s));
        return false;
    }
    return true;
}

void Shutdown() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

namespace {
volatile LONG unsafePatchFailure = 0;
volatile LONG patchFailureSerial = 0;
bool ObservationImage(uintptr_t addr, char* module, size_t moduleSize, uint32_t& rva, uint32_t& timestamp) noexcept {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery((const void*)addr, &m, sizeof(m)) || m.Type != MEM_IMAGE || !m.AllocationBase) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(m.AllocationBase);
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS32 nt{}; SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void*)base, &dos, sizeof(dos), &got) || got != sizeof(dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 1024 * 1024 ||
        static_cast<uintptr_t>(dos.e_lfanew) > UINTPTR_MAX - base) return false;
    if (!ReadProcessMemory(GetCurrentProcess(), (void*)(base + dos.e_lfanew), &nt, sizeof(nt), &got) || got != sizeof(nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || !nt.OptionalHeader.SizeOfImage ||
        addr < base || addr - base >= nt.OptionalHeader.SizeOfImage) return false;
    char path[MAX_PATH]{}; const DWORD length = GetModuleFileNameA((HMODULE)base, path, sizeof(path));
    if (!length || length >= sizeof(path)) return false;
    const char* name = path;
    for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') name = p + 1;
    const size_t len = strlen(name);
    if (!len || len >= moduleSize) return false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 32 || c > 126 || c == ':' || c == '/' || c == '\\') return false;
    }
    memcpy(module, name, len + 1);
    rva = static_cast<uint32_t>(addr - base); timestamp = nt.FileHeader.TimeDateStamp;
    return true;
}
struct Observation {
    hookreg::Ticket ticket{};
    bool active = false;
    Observation(hookreg::Kind kind, const char* label, uintptr_t addr, size_t size,
                const void* desired = nullptr, const char* purpose = nullptr, const void* expected = nullptr) noexcept {
        if (!addr || !size || size > std::numeric_limits<uint32_t>::max()) return;
        hookreg::Spec spec{};
        uint32_t rva = 0, timestamp = 0;
        if (ObservationImage(addr, spec.module, sizeof(spec.module), rva, timestamp))
            std::snprintf(spec.id, sizeof(spec.id), "%s-%s-%08X-%08X", label, spec.module, timestamp, rva);
        else {
            std::snprintf(spec.module, sizeof(spec.module), "private-memory");
            std::snprintf(spec.id, sizeof(spec.id), "runtime-%s-%llX", label, (unsigned long long)addr);
        }
        std::snprintf(spec.owner, sizeof(spec.owner), "memory-helper");
        if (purpose) {
            SIZE_T got = 0;
            (void)ReadProcessMemory(GetCurrentProcess(), purpose, spec.purpose, sizeof(spec.purpose) - 1, &got);
            if (!got || !memchr(spec.purpose, 0, got)) spec.purpose[0] = 0;
        }
        spec.site = addr; spec.span = static_cast<uint32_t>(size); spec.kind = kind;
        // Input can be an invalid pointer; never dereference it for diagnostics.
        if (desired && size <= hookreg::kBytes) {
            uint8_t bytes[hookreg::kBytes]{}; SIZE_T got = 0;
            if (ReadProcessMemory(GetCurrentProcess(), desired, bytes, size, &got) && got == size)
                hookreg::ManifestBytes(spec.desired, bytes, size);
        }
        if (expected && size <= hookreg::kBytes) {
            uint8_t bytes[hookreg::kBytes]{}; SIZE_T got = 0;
            if (ReadProcessMemory(GetCurrentProcess(), expected, bytes, size, &got) && got == size)
                hookreg::ManifestBytes(spec.expected, bytes, size);
        }
        active = hookreg::CaptureBefore(spec, ticket) == hookreg::Put::Stored;
    }
    void Finish(hookreg::Outcome outcome, uint32_t error = 0, uintptr_t previous = 0,
                uintptr_t trampoline = 0) noexcept {
        if (active) (void)hookreg::CaptureAfter(ticket, outcome, error, previous, trampoline);
    }
};
hookreg::Outcome WriteOutcome(const WriteResult& r) noexcept {
    if (r.Ok()) return hookreg::Outcome::ApiSuccess;
    if (r.changed || (!r.restored && (r.failure == WriteFailure::RestoreFailed || r.restoreError)))
        return hookreg::Outcome::Indeterminate;
    if (r.failure == WriteFailure::Conflict) return hookreg::Outcome::Conflict;
    return hookreg::Outcome::FailedUnchanged;
}
uintptr_t ReadTrampoline(void** original) noexcept {
    uintptr_t value = 0; SIZE_T got = 0;
    return original && ReadProcessMemory(GetCurrentProcess(), original, &value, sizeof(value), &got) && got == sizeof(value) ? value : 0;
}
uint32_t WriteError(const WriteResult& r) noexcept {
    return r.queryError ? r.queryError : r.protectError ? r.protectError : r.copyError ? r.copyError :
           r.restoreError ? r.restoreError : r.flushError;
}
WriteResult Mutate(uintptr_t addr, const void* data, size_t size, bool nop) {
    WriteResult result;
    if (!addr || !size || size - 1 > UINTPTR_MAX - addr || (!nop && !data)) {
        result.failure = WriteFailure::InvalidRange;
        return result;
    }
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery((const void*)addr, &region, sizeof(region))) {
        result.failure = WriteFailure::QueryFailed;
        result.queryError = GetLastError();
        return result;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(region.BaseAddress);
    if (addr < base || addr - base >= region.RegionSize ||
        size > region.RegionSize - (addr - base) || region.State != MEM_COMMIT ||
        (region.Protect & PAGE_GUARD) || (region.Protect & 0xff) == PAGE_NOACCESS) {
        result.failure = WriteFailure::UnsupportedRegion;
        return result;
    }
    DWORD old = 0;
    if (!VirtualProtect((void*)addr, size, PAGE_EXECUTE_READWRITE, &old)) {
        result.failure = WriteFailure::ProtectFailed;
        result.protectError = GetLastError();
        return result;
    }
    uint8_t nops[256];
    if (nop) memset(nops, 0x90, sizeof(nops));
    const HANDLE process = GetCurrentProcess();
    while (result.bytesWritten < size) {
        const size_t remaining = size - result.bytesWritten;
        const size_t count = nop && remaining > sizeof(nops) ? sizeof(nops) : remaining;
        const void* source = nop ? (const void*)nops : data;
        SIZE_T written = 0;
        const BOOL ok = WriteProcessMemory(process, (void*)(addr + result.bytesWritten), source, count, &written);
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        if (written > count) written = count;
        result.bytesWritten += written;
        result.changed = result.bytesWritten != 0;
        if (!ok || written != count) {
            result.failure = result.changed ? WriteFailure::PartialCopy : WriteFailure::CopyFailed;
            result.copyError = error ? error : ERROR_PARTIAL_COPY;
            break;
        }
        if (!nop) break;
    }
    DWORD unused = 0;
    result.restored = VirtualProtect((void*)addr, size, old, &unused) != FALSE;
    if (!result.restored) {
        result.restoreError = GetLastError();
        if (result.failure == WriteFailure::None) result.failure = WriteFailure::RestoreFailed;
    }
    result.flushed = FlushInstructionCache(process, (void*)addr, size) != FALSE;
    if (!result.flushed) {
        result.flushError = GetLastError();
        if (result.failure == WriteFailure::None) result.failure = WriteFailure::FlushFailed;
    }
    if ((!result.Ok() && result.changed) || !result.restored)
        InterlockedExchange(&unsafePatchFailure, 1);
    return result;
}

WriteResult SwapPointer(uintptr_t addr, void* expected, void* value) {
    WriteResult result;
    if (!addr || (addr % alignof(void*)) || sizeof(void*) - 1 > UINTPTR_MAX - addr) {
        result.failure = WriteFailure::InvalidRange;
        return result;
    }
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery((const void*)addr, &region, sizeof(region))) {
        result.failure = WriteFailure::QueryFailed;
        result.queryError = GetLastError();
        return result;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(region.BaseAddress);
    if (addr < base || addr - base >= region.RegionSize ||
        sizeof(void*) > region.RegionSize - (addr - base) || region.State != MEM_COMMIT ||
        (region.Protect & PAGE_GUARD) || (region.Protect & 0xff) == PAGE_NOACCESS) {
        result.failure = WriteFailure::UnsupportedRegion;
        return result;
    }
    DWORD old = 0;
    if (!VirtualProtect((void*)addr, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        result.failure = WriteFailure::ProtectFailed;
        result.protectError = GetLastError();
        return result;
    }
    // This only makes the aligned pointer update atomic. The region must remain mapped
    // throughout the operation; unrelated concurrent VirtualProtect/unmap is unsupported.
    void* actual = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(addr), value, expected);
    if (actual != expected) result.failure = WriteFailure::Conflict;
    else { result.changed = true; result.bytesWritten = sizeof(void*); }
    DWORD ignored = 0;
    result.restored = VirtualProtect((void*)addr, sizeof(void*), old, &ignored) != FALSE;
    if (!result.restored) {
        result.restoreError = GetLastError();
        if (result.failure == WriteFailure::None) result.failure = WriteFailure::RestoreFailed;
    }
    result.flushed = FlushInstructionCache(GetCurrentProcess(), (void*)addr, sizeof(void*)) != FALSE;
    if (!result.flushed) {
        result.flushError = GetLastError();
        if (result.failure == WriteFailure::None) result.failure = WriteFailure::FlushFailed;
    }
    if ((!result.Ok() && result.changed) || !result.restored) InterlockedExchange(&unsafePatchFailure, 1);
    return result;
}

void ReportWriteFailure(const char* name, uintptr_t addr, size_t size, const WriteResult& r) {
    LOGE("%s @%08X size=%u failed stage=%u written=%u changed=%u restored=%u flushed=%u errors=%u/%u/%u/%u/%u",
         name, (unsigned)addr, (unsigned)size, (unsigned)r.failure, (unsigned)r.bytesWritten,
         (unsigned)r.changed, (unsigned)r.restored, (unsigned)r.flushed,
         r.queryError, r.protectError, r.copyError, r.restoreError, r.flushError);
}
} // namespace

bool HasUnsafePatchFailure() { return InterlockedCompareExchange(&unsafePatchFailure, 0, 0) != 0; }
uint32_t PatchFailureSerial() { return static_cast<uint32_t>(InterlockedCompareExchange(&patchFailureSerial, 0, 0)); }
WriteResult TryWriteObserved(uintptr_t addr, const void* data, size_t size,
                             hookreg::Kind kind, const char* label, bool observe) {
    Observation obs(kind, label, observe ? addr : 0, size, data);
    const auto result = Mutate(addr, data, size, false);
    if (!result.Ok()) InterlockedIncrement(&patchFailureSerial);
    obs.Finish(WriteOutcome(result), WriteError(result));
    return result;
}
WriteResult TryWrite(uintptr_t addr, const void* data, size_t size) {
    return TryWriteObserved(addr, data, size, hookreg::Kind::Write, "write", true);
}
WriteResult TryNop(uintptr_t addr, size_t size) {
    uint8_t bytes[hookreg::kBytes]; memset(bytes, 0x90, sizeof(bytes));
    Observation obs(hookreg::Kind::Nop, "nop", addr, size, size <= sizeof(bytes) ? bytes : nullptr);
    const auto result = Mutate(addr, nullptr, size, true);
    if (!result.Ok()) InterlockedIncrement(&patchFailureSerial);
    obs.Finish(WriteOutcome(result), WriteError(result));
    return result;
}
void Write(uintptr_t addr, const void* data, size_t size) {
    const auto result = TryWrite(addr, data, size);
    if (!result.Ok()) ReportWriteFailure("Write", addr, size, result);
}
void Nop(uintptr_t addr, size_t size) {
    const auto result = TryNop(addr, size);
    if (!result.Ok()) ReportWriteFailure("Nop", addr, size, result);
}

void Jump(uintptr_t addr, const void* target) {
    uint8_t code[5];
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)target - (addr + 5));
    memcpy(code + 1, &rel, 4);
    const auto result = TryWriteObserved(addr, code, 5, hookreg::Kind::Jump, "jump", true);
    if (!result.Ok()) ReportWriteFailure("Write", addr, 5, result);
}

void Ret(uintptr_t addr, uint16_t popBytes) {
    if (popBytes == 0) {
        const uint8_t code = 0xC3;
        const auto result = TryWriteObserved(addr, &code, 1, hookreg::Kind::Other, "ret", true);
        if (!result.Ok()) ReportWriteFailure("Write", addr, 1, result);
    } else {
        uint8_t code[3] = {0xC2, (uint8_t)(popBytes & 0xFF), (uint8_t)(popBytes >> 8)};
        const auto result = TryWriteObserved(addr, code, 3, hookreg::Kind::Other, "ret", true);
        if (!result.Ok()) ReportWriteFailure("Write", addr, 3, result);
    }
}

uintptr_t CallTarget(uintptr_t addr) {
    uint8_t op = *(uint8_t*)addr;
    if (op != 0xE8 && op != 0xE9) return 0;
    return addr + 5 + *(int32_t*)(addr + 1);
}

namespace {
RedirectResult RedirectImpl(uintptr_t addr, const void* newTarget, void** chain) {
    RedirectResult result;
    uint8_t instruction[5]{};
    SIZE_T read = 0;
    if (!addr || addr > UINTPTR_MAX - sizeof(instruction)) {
        result.write.failure = WriteFailure::InvalidRange;
        return result;
    }
    const BOOL readOk = ReadProcessMemory(GetCurrentProcess(), (const void*)addr, instruction, sizeof(instruction), &read);
    if (!readOk || read != sizeof(instruction)) {
        result.write.failure = WriteFailure::ReadFailed;
        result.write.copyError = readOk ? ERROR_PARTIAL_COPY : GetLastError();
        return result;
    }
    if (instruction[0] != 0xE8) {
        LOGE("RedirectCall: no call instruction at %08X (byte %02X)", (unsigned)addr, instruction[0]);
        result.write.failure = WriteFailure::UnsupportedRegion;
        return result;
    }
    memcpy(result.originalBytes, instruction, sizeof(instruction));
    int32_t oldRel = 0;
    memcpy(&oldRel, instruction + 1, sizeof(oldRel));
    result.previous = (void*)(addr + 5 + oldRel);
    if (chain) *chain = result.previous;
    int32_t rel = (int32_t)((uintptr_t)newTarget - (addr + 5));
    result.write = TryWriteObserved(addr + 1, &rel, sizeof(rel), hookreg::Kind::Write, "write", false);
    result.state = result.write.Ok() ? PatchState::Installed :
                   result.write.changed ? PatchState::Indeterminate : PatchState::Unchanged;
    if (result.write.changed) {
        SIZE_T got = 0;
        result.readbackValid = ReadProcessMemory(GetCurrentProcess(), (const void*)addr, result.readbackBytes, sizeof(result.readbackBytes), &got) && got == sizeof(result.readbackBytes);
        uint8_t expected[5]; memcpy(expected, instruction, sizeof(expected)); memcpy(expected + 1, &rel, sizeof(rel));
        if (result.write.Ok() && (!result.readbackValid || memcmp(expected, result.readbackBytes, sizeof(expected)) != 0)) {
            result.write.failure = WriteFailure::VerifyFailed;
            result.state = PatchState::Indeterminate;
            InterlockedExchange(&unsafePatchFailure, 1);
        }
    }
    return result;
}
} // namespace

RedirectResult TryRedirectCall(uintptr_t addr, const void* newTarget, void** chain) {
    uint8_t desired[5] = {0xE8};
    const int32_t relative = static_cast<int32_t>(reinterpret_cast<uintptr_t>(newTarget) - (addr + 5));
    memcpy(desired + 1, &relative, sizeof(relative));
    Observation obs(hookreg::Kind::Call, "call", addr, sizeof(desired), desired);
    const auto result = RedirectImpl(addr, newTarget, chain);
    if (!result.write.Ok()) InterlockedIncrement(&patchFailureSerial);
    obs.Finish(WriteOutcome(result.write), WriteError(result.write), reinterpret_cast<uintptr_t>(result.previous));
    return result;
}

bool RedirectCall(uintptr_t addr, const void* newTarget, void** previous) {
    const auto result = TryRedirectCall(addr, newTarget, previous);
    if (!result.write.Ok()) {
        ReportWriteFailure("RedirectCall", addr, 5, result.write);
        return false;
    }
    return true;
}

bool Hook(uintptr_t addr, void* detour, void** original, const char* name) {
    // MinHook chooses the final jump encoding and displaced length. Five bytes
    // are an observation prefix, not an assertion of full patch length or ABI.
    Observation obs(hookreg::Kind::Detour, "detour", addr, 5, nullptr, name);
    MH_STATUS s = MH_CreateHook((void*)addr, detour, original);
    if (s != MH_OK) {
        InterlockedIncrement(&patchFailureSerial);
        obs.Finish(hookreg::Outcome::FailedUnchanged, static_cast<uint32_t>(s));
        LOGE("Hook %s @%08X: create failed: %s", name, (unsigned)addr, MH_StatusToString(s));
        return false;
    }
    s = MH_EnableHook((void*)addr);
    if (s != MH_OK) {
        InterlockedIncrement(&patchFailureSerial);
        obs.Finish(hookreg::Outcome::Indeterminate, static_cast<uint32_t>(s), 0, ReadTrampoline(original));
        LOGE("Hook %s @%08X: enable failed: %s", name, (unsigned)addr, MH_StatusToString(s));
        return false;
    }
    LOGD("Hooked %s @%08X", name, (unsigned)addr);
    obs.Finish(hookreg::Outcome::ApiSuccess, 0, 0, ReadTrampoline(original));
    return true;
}

SlotReadResult ReadVTableSlot(void** vtable, int index) {
    SlotReadResult result;
    const uintptr_t base = reinterpret_cast<uintptr_t>(vtable);
    if (!base || index < 0 || static_cast<uintptr_t>(index) > (UINTPTR_MAX - base) / sizeof(void*)) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    const uintptr_t slot = base + static_cast<uintptr_t>(index) * sizeof(void*);
    SIZE_T read = 0;
    const BOOL readOk = ReadProcessMemory(GetCurrentProcess(), (const void*)slot, &result.value, sizeof(result.value), &read);
    if (!readOk || read != sizeof(result.value)) {
        result.value = nullptr;
        result.error = readOk ? ERROR_PARTIAL_COPY : GetLastError();
        return result;
    }
    result.ok = true;
    return result;
}

namespace {
SwapResult SwapImpl(void** vtable, int index, void* fn, void* expected, bool checkExpected, void** chain) {
    SwapResult result;
    const auto read = ReadVTableSlot(vtable, index);
    if (!read.ok) {
        result.write.failure = WriteFailure::ReadFailed;
        result.write.copyError = read.error;
        return result;
    }
    result.previous = read.value;
    if (checkExpected && result.previous != expected) {
        result.write.failure = WriteFailure::Conflict;
        return result;
    }
    const uintptr_t slot = reinterpret_cast<uintptr_t>(vtable) + static_cast<uintptr_t>(index) * sizeof(void*);
    if (chain) *chain = result.previous;
    result.write = SwapPointer(slot, result.previous, fn);
    result.state = result.write.Ok() ? PatchState::Installed :
                   result.write.changed ? PatchState::Indeterminate : PatchState::Unchanged;
    if (result.write.changed) {
        const auto after = ReadVTableSlot(vtable, index);
        result.readback = after.value;
        result.readbackValid = after.ok;
        if (result.write.Ok() && (!after.ok || after.value != fn)) {
            result.write.failure = WriteFailure::VerifyFailed;
            result.state = PatchState::Indeterminate;
            InterlockedExchange(&unsafePatchFailure, 1);
        }
    }
    return result;
}
} // namespace

SwapResult TrySwapVTable(void** vtable, int index, void* fn, void* expected, bool checkExpected, void** chain) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(vtable);
    const uintptr_t site = base && index >= 0 && static_cast<uintptr_t>(index) <= (UINTPTR_MAX - base) / sizeof(void*)
                           ? base + static_cast<uintptr_t>(index) * sizeof(void*) : 0;
    // This is the caller's expected pointer, not proof of a native executable or ABI.
    Observation obs(hookreg::Kind::Vtable, "vtable", site, sizeof(fn), &fn, nullptr, checkExpected ? &expected : nullptr);
    const auto result = SwapImpl(vtable, index, fn, expected, checkExpected, chain);
    if (!result.write.Ok()) InterlockedIncrement(&patchFailureSerial);
    obs.Finish(WriteOutcome(result.write), WriteError(result.write), reinterpret_cast<uintptr_t>(result.previous));
    return result;
}

void* SwapVTable(void** vtable, int index, void* fn) {
    const auto result = TrySwapVTable(vtable, index, fn, nullptr, false);
    if (!result.write.Ok()) {
        ReportWriteFailure("SwapVTable", reinterpret_cast<uintptr_t>(vtable), sizeof(fn), result.write);
        return nullptr;
    }
    return result.previous;
}

bool Check(uintptr_t addr, const void* expected, size_t size) { return memcmp((void*)addr, expected, size) == 0; }

}  // namespace coop::mem
