#pragma once
#include <string>

namespace coop::log {

enum class Level { Debug, Info, Warn, Error };

// Opens (truncates) the log file. Safe to call once per process.
void Open(const std::wstring& path, bool alsoConsole = false);
// Outside DllMain only. Open merely remembers the destination.
void StartDiagnostics();
bool StopDiagnostics();
void SetMinLevel(Level lvl);
void Write(Level lvl, const char* fmt, ...);
// Callback for mirroring log lines (e.g. to the server console UI).
void SetSink(void (*sink)(Level, const char* line));

}  // namespace coop::log

#define LOGD(...) ::coop::log::Write(::coop::log::Level::Debug, __VA_ARGS__)
#define LOGI(...) ::coop::log::Write(::coop::log::Level::Info, __VA_ARGS__)
#define LOGW(...) ::coop::log::Write(::coop::log::Level::Warn, __VA_ARGS__)
#define LOGE(...) ::coop::log::Write(::coop::log::Level::Error, __VA_ARGS__)
