#pragma once
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

// Measures a process's real render rate from the OS present events (ETW: Microsoft-Windows-DXGI and
// Microsoft-Windows-D3D9 Present_Start), the same technique PresentMon / CapFrameX / FrameView use.
// Nothing touches the target process. Requires administrator rights (or "Performance Log Users").
class GameFpsCounter {
public:
    ~GameFpsCounter() { Stop(); }
    bool Start(DWORD pid);          // false if the trace session could not be created (e.g. not admin)
    void Stop();
    bool Active() const { return active_; }
    // Presents per second over the most recent ~1 s of events, 0 if none recently. avgMs = mean frame time.
    double Fps(double* avgMs = nullptr);

private:
    static void WINAPI OnEvent(PEVENT_RECORD rec);
    void Record(int64_t qpc);

    DWORD pid_ = 0;
    bool active_ = false;
    TRACEHANDLE session_ = 0;
    TRACEHANDLE trace_ = INVALID_PROCESSTRACE_HANDLE;
    std::thread thread_;
    std::mutex mtx_;
    std::deque<int64_t> stamps_;    // QPC timestamps of presents within the last second
    int64_t freq_ = 1;
};
