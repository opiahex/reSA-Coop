#pragma once
#include <string>
#include <cstdint>
#include <cstddef>

namespace coop::log {

enum class Level { Debug, Info, Warn, Error };

// Opens (truncates) the log file. Safe to call once per process.
void Open(const std::wstring& path, bool alsoConsole = false);
// Outside DllMain only. Open merely remembers the destination.
void StartDiagnostics();
bool StopDiagnostics();
enum class DiagnosticCommand : uint8_t { Start, Stop };
enum class DiagnosticSubmit : uint8_t { Queued, Busy, Unavailable, ShuttingDown };
enum class DiagnosticPhase : uint8_t { Idle, Starting, Recording, Stopping, Stopped, Failed, TimedOut };
struct DiagnosticsStatus {
    uint32_t requested=0,completed=0,error=0,logDropped=0,criticalDropped=0,ioErrors=0,reentrantDropped=0,callbackErrors=0,workerErrors=0;
    uint32_t legacyDropped=0,legacyStale=0,legacyBeginFailures=0,legacyIoErrors=0,legacyRotations=0;
    DiagnosticPhase phase=DiagnosticPhase::Idle;
    bool tailConfigured=false,tailReady=false,enabled=false,busy=false;
};
// Callback executes on the log worker, never on the UI or crash thread.
// true prepares crash tail before start; false disarms after stop/failure.
void SetDiagnosticsPrepare(bool (*callback)(bool));
DiagnosticSubmit SubmitDiagnostics(DiagnosticCommand command) noexcept;
DiagnosticsStatus GetDiagnosticsStatus() noexcept;
// Shared worker, bounded nonblocking session API. 0 means no accepted session.
uint32_t BeginLegacySync(const wchar_t* path,size_t length) noexcept;
bool WriteLegacySync(uint32_t token,const char* data,size_t length) noexcept;
void EndLegacySync(uint32_t token) noexcept;
void SetMinLevel(Level lvl);
void Write(Level lvl, const char* fmt, ...);
enum class TextSubmit : uint8_t { Queued, Filtered, Unavailable, Busy, Full, ShuttingDown, Reentrant };
// Nonblocking runtime enqueue only: Queued is not confirmation of disk output.
TextSubmit TryWrite(Level lvl, const char* fmt, ...) noexcept;
// Callback runs on the log worker after StartDiagnostics; it must not block.
// Reentrant logging from this callback is dropped and counted.
void SetSink(void (*sink)(Level, const char* line));

}  // namespace coop::log

#define LOGD(...) ::coop::log::Write(::coop::log::Level::Debug, __VA_ARGS__)
#define LOGI(...) ::coop::log::Write(::coop::log::Level::Info, __VA_ARGS__)
#define LOGW(...) ::coop::log::Write(::coop::log::Level::Warn, __VA_ARGS__)
#define LOGE(...) ::coop::log::Write(::coop::log::Level::Error, __VA_ARGS__)
