#pragma once
#include <windows.h>
#include <string>
#include <vector>

// A top-level window that could be the capture source.
struct SourceWindow {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring title;
    std::wstring exe;       // e.g. L"H1Z1.exe" (file name only, from Toolhelp snapshot - no process handle is opened)
    int clientW = 0;        // physical pixels (process is per-monitor DPI aware v2)
    int clientH = 0;
    int score = 0;          // > 0 when it matches the auto-detect string

    std::wstring Label() const; // "H1Z1 v1.0.326... | H1Z1.exe | 1920x1440 | HWND 0x00123456"
};

// Enumerates visible, uncloaked, unowned top-level windows with a title (excluding this process).
// Windows whose title or exe contains `match` (case-insensitive) get score > 0.
// Sorted by score desc, then title.
std::vector<SourceWindow> EnumSourceWindows(const std::wstring& match);

struct MonitorEntry {
    HMONITOR hmon = nullptr;
    std::wstring device;    // GDI name, e.g. L"\\\\.\\DISPLAY1"
    std::wstring friendly;  // e.g. L"DELL S2721DGF" (from QueryDisplayConfig), may be empty
    RECT rect{};            // rcMonitor in physical virtual-desktop coordinates
    int width = 0, height = 0;
    int hz = 0;             // current refresh rate (rounded)
    bool primary = false;

    std::wstring Label() const; // "2560x1440 @ 180 Hz - DELL S2721DGF (\\.\DISPLAY1) [primary]"
};

std::vector<MonitorEntry> EnumMonitors();

// Client rect of hwnd in screen coordinates.
RECT ClientRectOnScreen(HWND hwnd);
