#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <vector>
#include <string>

#include "FpsCounter.h"
#include "Log.h"

#pragma comment(lib, "advapi32.lib")

namespace {

const wchar_t* kSessionName = L"StretchScalerGameFps";
// {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9} Microsoft-Windows-DXGI, Present_Start = 42
const GUID kDxgiProvider = {0xCA11C036, 0x0102, 0x4A2D, {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9}};
// {783ACA0A-790E-4D7F-8451-AA850511C6B9} Microsoft-Windows-D3D9, Present_Start = 1
const GUID kD3d9Provider = {0x783ACA0A, 0x790E, 0x4D7F, {0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9}};

std::vector<BYTE> MakeProps() {
    size_t size = sizeof(EVENT_TRACE_PROPERTIES) + (wcslen(kSessionName) + 1) * sizeof(wchar_t);
    std::vector<BYTE> buf(size, 0);
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
    p->Wnode.BufferSize = (ULONG)size;
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 1;          // QPC timestamps
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p->BufferSize = 4;                   // KB; small buffers so events arrive quickly
    p->MinimumBuffers = 4;
    p->FlushTimer = 1;                   // seconds; flush partially filled buffers (low fps)
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return buf;
}

void StopSession() {
    auto buf = MakeProps();
    ControlTraceW(0, kSessionName, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data()), EVENT_TRACE_CONTROL_STOP);
}

bool EnableProvider(TRACEHANDLE session, const GUID& provider, DWORD pid) {
    // Kernel-side PID filter keeps overhead to the target process's events only.
    EVENT_FILTER_DESCRIPTOR filter{(ULONGLONG)&pid, sizeof(pid), EVENT_FILTER_TYPE_PID};
    ENABLE_TRACE_PARAMETERS params{};
    params.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    params.EnableFilterDesc = &filter;
    params.FilterDescCount = 1;
    ULONG r = EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION,
                             ~0ULL, 0, 0, &params);
    if (r != ERROR_SUCCESS)
        r = EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, ~0ULL, 0, 0, nullptr);
    return r == ERROR_SUCCESS;
}

} // namespace

void WINAPI GameFpsCounter::OnEvent(PEVENT_RECORD rec) {
    auto* self = static_cast<GameFpsCounter*>(rec->UserContext);
    if (!self || rec->EventHeader.ProcessId != self->pid_) return;
    const auto& d = rec->EventHeader.EventDescriptor;
    if ((d.Id == 42 && IsEqualGUID(rec->EventHeader.ProviderId, kDxgiProvider)) ||
        (d.Id == 1 && IsEqualGUID(rec->EventHeader.ProviderId, kD3d9Provider)))
        self->Record(rec->EventHeader.TimeStamp.QuadPart);
}

void GameFpsCounter::Record(int64_t qpc) {
    std::lock_guard<std::mutex> lock(mtx_);
    stamps_.push_back(qpc);
    while (!stamps_.empty() && qpc - stamps_.front() > freq_) stamps_.pop_front();
}

bool GameFpsCounter::Start(DWORD pid) {
    Stop();
    pid_ = pid;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    freq_ = f.QuadPart;

    StopSession(); // leftover session from a crashed run
    auto buf = MakeProps();
    ULONG r = StartTraceW(&session_, kSessionName, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data()));
    if (r != ERROR_SUCCESS) {
        session_ = 0;
        if (r == ERROR_ACCESS_DENIED)
            Log(L"FPS overlay: true game FPS needs administrator rights (ETW). Showing on-screen FPS instead.");
        else
            Log(L"FPS overlay: StartTrace failed (%lu). Showing on-screen FPS instead.", r);
        return false;
    }
    bool dxgi = EnableProvider(session_, kDxgiProvider, pid);
    bool d3d9 = EnableProvider(session_, kD3d9Provider, pid);
    if (!dxgi && !d3d9) {
        Log(L"FPS overlay: could not enable present event providers.");
        Stop();
        return false;
    }

    EVENT_TRACE_LOGFILEW lf{};
    lf.LoggerName = const_cast<wchar_t*>(kSessionName);
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    lf.EventRecordCallback = &GameFpsCounter::OnEvent;
    lf.Context = this;
    trace_ = OpenTraceW(&lf);
    if (trace_ == INVALID_PROCESSTRACE_HANDLE) {
        Log(L"FPS overlay: OpenTrace failed (%lu).", GetLastError());
        Stop();
        return false;
    }
    thread_ = std::thread([this] {
        SetThreadDescription(GetCurrentThread(), L"StretchScaler ETW");
        ProcessTrace(&trace_, 1, nullptr, nullptr);
    });
    active_ = true;
    Log(L"FPS overlay: measuring real game FPS for PID %lu via ETW present events.", pid);
    return true;
}

void GameFpsCounter::Stop() {
    if (session_) { StopSession(); session_ = 0; }
    if (trace_ != INVALID_PROCESSTRACE_HANDLE) { CloseTrace(trace_); trace_ = INVALID_PROCESSTRACE_HANDLE; }
    if (thread_.joinable()) thread_.join();
    active_ = false;
    std::lock_guard<std::mutex> lock(mtx_);
    stamps_.clear();
}

double GameFpsCounter::Fps(double* avgMs) {
    std::lock_guard<std::mutex> lock(mtx_);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // Events can arrive up to ~1 s late (ETW buffer flush), so measure over the latest events' own timestamps,
    // but report 0 if nothing was presented for 2 s.
    if (stamps_.size() < 2 || now.QuadPart - stamps_.back() > 2 * freq_) {
        if (avgMs) *avgMs = 0;
        return 0;
    }
    double span = (double)(stamps_.back() - stamps_.front()) / freq_;
    double fps = span > 0 ? (stamps_.size() - 1) / span : 0;
    if (avgMs) *avgMs = fps > 0 ? 1000.0 / fps : 0;
    return fps;
}
