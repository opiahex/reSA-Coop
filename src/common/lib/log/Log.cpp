#include "common/lib/log/Log.h"
#include "common/lib/profile/Profile.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace coop::log {

static std::mutex g_mutex;
static FILE* g_file = nullptr;
static bool g_console = false;
static Level g_min = Level::Debug;
static void (*g_sink)(Level, const char*) = nullptr;

void Open(const std::wstring& path, bool alsoConsole) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) fclose(g_file);
    // the last run's log stays as *.old.log: a game restarted after a crash wrote over the log the report needed
    std::wstring old = path;
    size_t dot = old.rfind(L'.');
    old.insert(dot == std::wstring::npos ? old.size() : dot, L".old");  // coop.log -> coop.old.log
    MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = _wfopen(path.c_str(), L"wb");
    if (g_file) fputs("\xEF\xBB\xBF", g_file);  // UTF-8 BOM so Notepad shows Cyrillic correctly
    g_console = alsoConsole;
}

void SetMinLevel(Level lvl) { g_min = lvl; }
void SetSink(void (*sink)(Level, const char*)) { g_sink = sink; }

void Write(Level lvl, const char* fmt, ...) {
    if (lvl < g_min) return;
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    static const char* names[] = {"DBG", "INF", "WRN", "ERR"};
    char line[4300];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             names[(int)lvl], msg);

    // a fault in here with the lock held, reported by the crash handler on the same thread, waited for itself
    static thread_local bool writing = false;
    if (writing) return;
    profile::Scope timing(profile::LOG_IO);
    writing = true;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fputs(line, g_file);
        static ULONGLONG lastFlush=0;auto now=GetTickCount64();
        if(lvl>=Level::Warn||now-lastFlush>=250){fflush(g_file);lastFlush=now;}
    }
    if (g_console) {
        // Console output in UTF-8 (the server sets the console code page to UTF-8).
        fputs(line, stdout);
        fflush(stdout);
    }
    if (g_sink) g_sink(lvl, line);
    writing = false;
}

}  // namespace coop::log
