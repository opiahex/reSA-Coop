#include "common/lib/log/Log.h"
#include "common/lib/profile/Profile.h"
#include "common/lib/diagnostics/FlightRecorder.h"
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <new>
#include <vector>

namespace coop::log {
namespace {
constexpr size_t kQueue=512,kLine=4300,kLegacyPath=1024,kLegacyLimit=8*1024*1024;
struct Line { Level level{}; char text[kLine]{}; uint32_t legacyToken=0;size_t legacyLength=0; wchar_t legacyPath[kLegacyPath]{}; };
struct WorkerState {
    std::atomic_flag lock=ATOMIC_FLAG_INIT;
    Line queue[kQueue]{}; size_t head=0,count=0;
    HANDLE wake=nullptr,thread=nullptr;
    FILE* file=nullptr; bool console=false;
    std::wstring path;
    DiagnosticCommand command=DiagnosticCommand::Stop;
    bool commandPending=false;
    std::atomic<bool> shutdown{false};
    DiagnosticsStatus status{};
    std::atomic<uint32_t> dropped{0},critical{0},ioErrors{0},reentrant{0},callbackErrors{0},workerErrors{0};
    std::atomic<uint32_t> legacyActive{0},legacyDropped{0},legacyStale{0},legacyBeginFailures{0},legacyIoErrors{0},legacyRotations{0};
    uint32_t legacyNext=0,legacyFileToken=0;wchar_t legacyPath[kLegacyPath]{};
    FILE* legacyFile=nullptr;std::wstring legacyFilePath;size_t legacyWritten=0;ULONGLONG legacyLastFlush=0;bool legacyDirty=false;
    std::atomic<bool> shutdownTimedOut{false},shutdownClean{false};
};
std::atomic<WorkerState*> state{nullptr};
std::atomic<Level> minimum{Level::Debug};
std::atomic<void(*)(Level,const char*)> sink{nullptr};
std::atomic<bool(*)(bool)> prepare{nullptr};
std::atomic<bool> startupRequested{false};
std::mutex startupMutex;
FILE* startupFile=nullptr; bool startupConsole=false;
std::wstring startupPath;
thread_local bool writing=false;

bool TryLock(WorkerState& s) noexcept { return !s.lock.test_and_set(std::memory_order_acquire); }
void Unlock(WorkerState& s) noexcept { s.lock.clear(std::memory_order_release); }
// Worker only: producers and UI never wait for this lock.
void WorkerLock(WorkerState& s) noexcept { while(!TryLock(s)) SwitchToThread(); }
TextSubmit Enqueue(WorkerState& s,const Line& line) noexcept {
    if(!TryLock(s)){s.dropped.fetch_add(1,std::memory_order_relaxed);if(line.level>=Level::Warn)s.critical.fetch_add(1,std::memory_order_relaxed);return TextSubmit::Busy;}
    TextSubmit result=TextSubmit::Queued;
    if(s.shutdown.load(std::memory_order_acquire))result=TextSubmit::ShuttingDown;
    else if(s.count==kQueue)result=TextSubmit::Full;
    if(result!=TextSubmit::Queued){s.dropped.fetch_add(1,std::memory_order_relaxed);if(line.level>=Level::Warn)s.critical.fetch_add(1,std::memory_order_relaxed);}
    else{s.queue[(s.head+s.count)%kQueue]=line;++s.count;SetEvent(s.wake);}
    Unlock(s);return result;
}
void Output(WorkerState& s,const Line& line) {
    profile::Scope timing(profile::LOG_IO);
    if(s.file){if(fputs(line.text,s.file)<0)s.ioErrors.fetch_add(1,std::memory_order_relaxed);
        static ULONGLONG lastFlush=0;auto now=GetTickCount64();
        if(line.level>=Level::Warn||now-lastFlush>=250){if(fflush(s.file)!=0)s.ioErrors.fetch_add(1,std::memory_order_relaxed);lastFlush=now;}}
    if(s.console){if(fputs(line.text,stdout)<0||fflush(stdout)!=0)s.ioErrors.fetch_add(1,std::memory_order_relaxed);}
    if(auto callback=sink.load(std::memory_order_acquire)){
        writing=true;try{callback(line.level,line.text);}catch(...){s.callbackErrors.fetch_add(1,std::memory_order_relaxed);}writing=false;
    }
}
bool CloseLegacy(WorkerState& s) {
    bool ok=true;
    if(s.legacyFile){if(fflush(s.legacyFile)!=0){s.legacyIoErrors.fetch_add(1,std::memory_order_relaxed);ok=false;}
        if(fclose(s.legacyFile)!=0){s.legacyIoErrors.fetch_add(1,std::memory_order_relaxed);ok=false;}s.legacyFile=nullptr;}
    s.legacyFileToken=0;s.legacyWritten=0;s.legacyDirty=false;s.legacyFilePath.clear();
    return ok;
}
void OpenLegacy(WorkerState& s,const Line& line) {
    CloseLegacy(s);s.legacyFileToken=line.legacyToken;s.legacyFilePath=line.legacyPath;
    auto separator=s.legacyFilePath.find_last_of(L"\\/");
    if(separator==std::wstring::npos){s.legacyIoErrors.fetch_add(1,std::memory_order_relaxed);return;}
    auto dir=s.legacyFilePath.substr(0,separator+1);
    if(!CreateDirectoryW(dir.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS){s.legacyIoErrors.fetch_add(1,std::memory_order_relaxed);return;}
    std::vector<std::filesystem::path> files;std::error_code ec;
    for(std::filesystem::directory_iterator it(dir,ec),end;!ec&&it!=end;it.increment(ec))
        if(it->is_regular_file(ec)&&it->path().filename().wstring().rfind(L"sync-",0)==0)files.push_back(it->path());
    if(ec)s.legacyIoErrors.fetch_add(1,std::memory_order_relaxed);
    std::sort(files.begin(),files.end());while(files.size()>10){ec.clear();std::filesystem::remove(files.front(),ec);if(ec)s.legacyIoErrors.fetch_add(1,std::memory_order_relaxed);files.erase(files.begin());}
    s.legacyFile=_wfopen(s.legacyFilePath.c_str(),L"wb");if(!s.legacyFile)s.legacyIoErrors.fetch_add(1,std::memory_order_relaxed);
}
void OutputLegacy(WorkerState& s,const Line& line) {
    profile::Scope timing(profile::LOG_IO);
    if(s.legacyFileToken!=line.legacyToken)OpenLegacy(s,line);
    if(!s.legacyFile){s.legacyDropped.fetch_add(1,std::memory_order_relaxed);return;}
    if(s.legacyWritten+line.legacyLength>kLegacyLimit){
        if(fclose(s.legacyFile)!=0)s.legacyIoErrors.fetch_add(1,std::memory_order_relaxed);s.legacyFile=nullptr;
        std::wstring old=s.legacyFilePath+L".previous";
        if(!MoveFileExW(s.legacyFilePath.c_str(),old.c_str(),MOVEFILE_REPLACE_EXISTING)){s.legacyIoErrors.fetch_add(1,std::memory_order_relaxed);s.legacyDropped.fetch_add(1,std::memory_order_relaxed);return;}
        s.legacyFile=_wfopen(s.legacyFilePath.c_str(),L"wb");s.legacyWritten=0;
        if(!s.legacyFile){s.legacyIoErrors.fetch_add(1,std::memory_order_relaxed);s.legacyDropped.fetch_add(1,std::memory_order_relaxed);return;}
        s.legacyRotations.fetch_add(1,std::memory_order_relaxed);
    }
    size_t written=fwrite(line.text,1,line.legacyLength,s.legacyFile);s.legacyWritten+=written;
    if(written!=line.legacyLength){s.legacyIoErrors.fetch_add(1,std::memory_order_relaxed);s.legacyDropped.fetch_add(1,std::memory_order_relaxed);
        if(fclose(s.legacyFile)!=0)s.legacyIoErrors.fetch_add(1,std::memory_order_relaxed);s.legacyFile=nullptr;s.legacyDirty=false;return;}
    s.legacyDirty=true;
    auto now=GetTickCount64();if(now-s.legacyLastFlush>=250){if(fflush(s.legacyFile)!=0)s.legacyIoErrors.fetch_add(1,std::memory_order_relaxed);s.legacyLastFlush=now;s.legacyDirty=false;}
}
bool Prepare(WorkerState& s,bool(*callback)(bool),bool start) noexcept {
    if(!callback)return true;
    try{return callback(start);}catch(...){s.callbackErrors.fetch_add(1,std::memory_order_relaxed);return false;}
}
void Complete(WorkerState& s,DiagnosticPhase phase,DWORD error,bool configured,bool ready) {
    WorkerLock(s);s.status.phase=phase;s.status.error=error;s.status.tailConfigured=configured;
    s.status.tailReady=ready;s.status.enabled=flight::Enabled();++s.status.completed;s.commandPending=false;Unlock(s);
}
void Execute(WorkerState& s,DiagnosticCommand command) {
    auto cb=prepare.load(std::memory_order_acquire);bool configured=cb!=nullptr;
    if(command==DiagnosticCommand::Start){
        if(flight::Enabled()){WorkerLock(s);bool prior=s.status.tailReady;Unlock(s);Complete(s,DiagnosticPhase::Recording,ERROR_ALREADY_EXISTS,configured,prior);return;}
        bool ready=Prepare(s,cb,true);
        if(!ready){Complete(s,DiagnosticPhase::Failed,ERROR_BUSY,configured,false);return;}
        LARGE_INTEGER stamp{};QueryPerformanceCounter(&stamp);
        auto name=s.path+L".flight-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(stamp.QuadPart)+L".jsonl";
        if(!flight::Start(name)){bool disarmed=Prepare(s,cb,false);Complete(s,DiagnosticPhase::Failed,disarmed?ERROR_GEN_FAILURE:ERROR_BUSY,configured,!disarmed);return;}
        Complete(s,DiagnosticPhase::Recording,0,configured,configured);return;
    }
    bool stopped=flight::Stop();bool disarmed=Prepare(s,cb,false);
    Complete(s,!stopped?DiagnosticPhase::TimedOut:!disarmed?DiagnosticPhase::Failed:DiagnosticPhase::Stopped,
             !stopped?ERROR_TIMEOUT:!disarmed?ERROR_BUSY:0,configured,!disarmed);
}
DWORD WINAPI Worker(void* arg) {
    auto& s=*static_cast<WorkerState*>(arg);
    for(;;){
        WaitForSingleObject(s.wake,250);
        for(;;){
            Line line{};bool hasLine=false,hasCommand=false,quit=false,closeLegacy=false;DiagnosticCommand command{};
            WorkerLock(s);
            if(s.commandPending){command=s.command;hasCommand=true;}
            else if(s.count){line=s.queue[s.head];s.head=(s.head+1)%kQueue;--s.count;hasLine=true;}
            else {quit=s.shutdown.load(std::memory_order_acquire);closeLegacy=s.legacyFileToken&&s.legacyFileToken!=s.legacyActive.load(std::memory_order_acquire);}
            Unlock(s);
            if(hasLine){try{if(line.legacyToken)OutputLegacy(s,line);else Output(s,line);}catch(...){writing=false;s.workerErrors.fetch_add(1,std::memory_order_relaxed);if(line.legacyToken){s.legacyIoErrors.fetch_add(1,std::memory_order_relaxed);s.legacyDropped.fetch_add(1,std::memory_order_relaxed);}}continue;}
            if(hasCommand){try{Execute(s,command);}catch(...){
                    s.workerErrors.fetch_add(1,std::memory_order_relaxed);auto cb=prepare.load(std::memory_order_acquire);
                    bool disarmed=command!=DiagnosticCommand::Start||Prepare(s,cb,false);
                    Complete(s,DiagnosticPhase::Failed,ERROR_UNHANDLED_EXCEPTION,cb!=nullptr,!disarmed);
                }continue;}
            if(quit){bool legacyClosed=CloseLegacy(s);bool stopped=false;try{stopped=flight::Stop();}catch(...){s.workerErrors.fetch_add(1,std::memory_order_relaxed);}
                auto cb=prepare.load(std::memory_order_acquire);bool disarmed=Prepare(s,cb,false);
                bool flushed=!s.file||fflush(s.file)==0;if(!flushed)s.ioErrors.fetch_add(1,std::memory_order_relaxed);
                s.shutdownClean.store(stopped&&disarmed&&flushed&&legacyClosed,std::memory_order_release);return 0;}
            if(closeLegacy)CloseLegacy(s);
            else if(s.legacyFile&&s.legacyDirty){if(fflush(s.legacyFile)!=0)s.legacyIoErrors.fetch_add(1,std::memory_order_relaxed);s.legacyDirty=false;}
            break;
        }
    }
}
}

void Open(const std::wstring& path,bool alsoConsole) {
    std::lock_guard<std::mutex> guard(startupMutex);
    // Open is a once-per-process startup operation; the worker owns FILE after start.
    if(state.load(std::memory_order_acquire))return;
    if(startupFile)fclose(startupFile);
    std::wstring old=path;size_t dot=old.rfind(L'.');old.insert(dot==std::wstring::npos?old.size():dot,L".old");
    MoveFileExW(path.c_str(),old.c_str(),MOVEFILE_REPLACE_EXISTING);
    startupFile=_wfopen(path.c_str(),L"wb");if(startupFile)fputs("\xEF\xBB\xBF",startupFile);
    startupConsole=alsoConsole;startupPath=path;
}
void StartDiagnostics(){
    static std::atomic_flag once=ATOMIC_FLAG_INIT;
    if(once.test_and_set(std::memory_order_acq_rel)){
        if(startupRequested.load(std::memory_order_acquire)){
            auto result=SubmitDiagnostics(DiagnosticCommand::Start);
            if(result!=DiagnosticSubmit::Busy)startupRequested.store(false,std::memory_order_release);
        }
        return;
    }
    std::lock_guard<std::mutex> guard(startupMutex);
    HMODULE pinned=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)&Worker,&pinned))return;
    void* storage=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(WorkerState));if(!storage)return;
    auto* s=new(storage) WorkerState;s->file=startupFile;s->console=startupConsole;
    try{s->path=startupPath;}catch(...){s->~WorkerState();HeapFree(GetProcessHeap(),0,storage);return;}
    s->wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!s->wake){s->~WorkerState();HeapFree(GetProcessHeap(),0,storage);return;}
    s->thread=CreateThread(nullptr,0,Worker,s,0,nullptr);if(!s->thread){CloseHandle(s->wake);s->~WorkerState();HeapFree(GetProcessHeap(),0,storage);return;}
    state.store(s,std::memory_order_release);
    char env[8]{};if(GetEnvironmentVariableA("COOP_FLIGHT_RECORDER",env,sizeof(env))==1&&env[0]=='1'){
        startupRequested.store(true,std::memory_order_release);
        auto result=SubmitDiagnostics(DiagnosticCommand::Start);
        if(result!=DiagnosticSubmit::Busy)startupRequested.store(false,std::memory_order_release);
    }
}
bool StopDiagnostics(){
    auto* s=state.load(std::memory_order_acquire);if(!s)return !flight::Enabled();
    s->shutdown.store(true,std::memory_order_release);SetEvent(s->wake);
    if(WaitForSingleObject(s->thread,1000)==WAIT_OBJECT_0)return s->shutdownClean.load(std::memory_order_acquire);
    s->shutdownTimedOut.store(true,std::memory_order_release);CancelSynchronousIo(s->thread);return false;
}
void SetDiagnosticsPrepare(bool(*callback)(bool)){prepare.store(callback,std::memory_order_release);}
DiagnosticSubmit SubmitDiagnostics(DiagnosticCommand command) noexcept {
    auto* s=state.load(std::memory_order_acquire);if(!s)return DiagnosticSubmit::Unavailable;
    if(!TryLock(*s))return DiagnosticSubmit::Busy;
    if(s->shutdown.load(std::memory_order_acquire)){Unlock(*s);return DiagnosticSubmit::ShuttingDown;}
    if(s->commandPending){Unlock(*s);return DiagnosticSubmit::Busy;}
    s->command=command;s->commandPending=true;++s->status.requested;
    s->status.phase=command==DiagnosticCommand::Start?DiagnosticPhase::Starting:DiagnosticPhase::Stopping;
    Unlock(*s);SetEvent(s->wake);return DiagnosticSubmit::Queued;
}
DiagnosticsStatus GetDiagnosticsStatus() noexcept {
    DiagnosticsStatus result{};auto* s=state.load(std::memory_order_acquire);if(!s){result.busy=true;return result;}
    if(!TryLock(*s)){result.busy=true;return result;}
    result=s->status;Unlock(*s);
    result.tailConfigured=prepare.load(std::memory_order_acquire)!=nullptr;
    result.logDropped=s->dropped.load(std::memory_order_relaxed);result.criticalDropped=s->critical.load(std::memory_order_relaxed);
    result.ioErrors=s->ioErrors.load(std::memory_order_relaxed);result.reentrantDropped=s->reentrant.load(std::memory_order_relaxed);
    result.callbackErrors=s->callbackErrors.load(std::memory_order_relaxed);result.workerErrors=s->workerErrors.load(std::memory_order_relaxed);
    result.legacyDropped=s->legacyDropped.load(std::memory_order_relaxed);result.legacyStale=s->legacyStale.load(std::memory_order_relaxed);
    result.legacyBeginFailures=s->legacyBeginFailures.load(std::memory_order_relaxed);result.legacyIoErrors=s->legacyIoErrors.load(std::memory_order_relaxed);result.legacyRotations=s->legacyRotations.load(std::memory_order_relaxed);
    if(s->shutdownTimedOut.load(std::memory_order_acquire))result.phase=DiagnosticPhase::TimedOut;
    return result;
}
uint32_t BeginLegacySync(const wchar_t* path,size_t length) noexcept {
    auto* s=state.load(std::memory_order_acquire);if(!s)return 0;
    if(!path||!length||length>=kLegacyPath||!TryLock(*s)){s->legacyBeginFailures.fetch_add(1,std::memory_order_relaxed);return 0;}
    if(s->shutdown.load(std::memory_order_acquire)||s->legacyNext==UINT32_MAX){s->legacyBeginFailures.fetch_add(1,std::memory_order_relaxed);Unlock(*s);return 0;}
    const uint32_t token=++s->legacyNext;memcpy(s->legacyPath,path,length*sizeof(wchar_t));s->legacyPath[length]=0;
    s->legacyActive.store(token,std::memory_order_release);Unlock(*s);return token;
}
bool WriteLegacySync(uint32_t token,const char* data,size_t length) noexcept {
    auto* s=state.load(std::memory_order_acquire);if(!s)return false;
    if(!token||!data||!length||length>=kLine||!TryLock(*s)){s->legacyDropped.fetch_add(1,std::memory_order_relaxed);return false;}
    if(s->legacyActive.load(std::memory_order_acquire)!=token||s->shutdown.load(std::memory_order_acquire)){
        s->legacyStale.fetch_add(1,std::memory_order_relaxed);s->legacyDropped.fetch_add(1,std::memory_order_relaxed);Unlock(*s);return false;}
    if(s->count==kQueue){s->legacyDropped.fetch_add(1,std::memory_order_relaxed);Unlock(*s);return false;}
    auto& line=s->queue[(s->head+s->count)%kQueue];line.legacyToken=token;line.legacyLength=length;
    memcpy(line.text,data,length);line.text[length]=0;memcpy(line.legacyPath,s->legacyPath,sizeof(line.legacyPath));
    ++s->count;Unlock(*s);SetEvent(s->wake);return true;
}
void EndLegacySync(uint32_t token) noexcept {
    auto* s=state.load(std::memory_order_acquire);if(!s||!token)return;
    if(!s->legacyActive.compare_exchange_strong(token,0,std::memory_order_acq_rel))s->legacyStale.fetch_add(1,std::memory_order_relaxed);
    SetEvent(s->wake);
}
void SetMinLevel(Level level){minimum.store(level,std::memory_order_release);}
void SetSink(void(*callback)(Level,const char*)){sink.store(callback,std::memory_order_release);}
namespace {
TextSubmit VWrite(Level lvl,const char* fmt,va_list ap,bool runtimeOnly){
    if(lvl<minimum.load(std::memory_order_acquire))return TextSubmit::Filtered;
    auto* s=state.load(std::memory_order_acquire);
    if(writing){if(s){s->reentrant.fetch_add(1,std::memory_order_relaxed);s->dropped.fetch_add(1,std::memory_order_relaxed);if(lvl>=Level::Warn)s->critical.fetch_add(1,std::memory_order_relaxed);}return TextSubmit::Reentrant;}
    if(runtimeOnly&&!s)return TextSubmit::Unavailable;
    writing=true;char msg[4096];vsnprintf(msg,sizeof(msg),fmt,ap);
    SYSTEMTIME st;GetLocalTime(&st);static const char* names[]={"DBG","INF","WRN","ERR"};
    Line line{};line.level=lvl;unsigned index=(unsigned)lvl;if(index>3)index=3;
    snprintf(line.text,sizeof(line.text),"[%02d:%02d:%02d.%03d] %s %s\n",st.wHour,st.wMinute,st.wSecond,st.wMilliseconds,names[index],msg);
    TextSubmit result=TextSubmit::Unavailable;
    if(s)result=Enqueue(*s,line);
    else{
        std::lock_guard<std::mutex> guard(startupMutex);
        s=state.load(std::memory_order_acquire);
        if(s)result=Enqueue(*s,line);
        else{
            // Legacy early-start fallback; ordinary runtime uses the bounded worker.
            if(startupFile){fputs(line.text,startupFile);static ULONGLONG lastFlush=0;auto now=GetTickCount64();if(lvl>=Level::Warn||now-lastFlush>=250){fflush(startupFile);lastFlush=now;}}
            if(startupConsole){fputs(line.text,stdout);fflush(stdout);}
            if(auto callback=sink.load(std::memory_order_acquire))callback(lvl,line.text);
        }
    }
    writing=false;return result;
}
}
void Write(Level lvl,const char* fmt,...){
    va_list ap;va_start(ap,fmt);(void)VWrite(lvl,fmt,ap,false);va_end(ap);
}
TextSubmit TryWrite(Level lvl,const char* fmt,...) noexcept {
    va_list ap;va_start(ap,fmt);auto result=VWrite(lvl,fmt,ap,true);va_end(ap);return result;
}
} // namespace coop::log
