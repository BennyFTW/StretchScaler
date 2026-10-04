#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <unordered_map>

#include "Enum.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")

namespace {

std::wstring ToLower(const std::wstring& s) {
    std::wstring r = s;
    if (!r.empty())
        CharLowerBuffW(&r[0], static_cast<DWORD>(r.size()));
    return r;
}

bool ContainsNoCase(const std::wstring& hay, const std::wstring& needleLower) {
    if (needleLower.empty()) return false;
    return ToLower(hay).find(needleLower) != std::wstring::npos;
}

// PID -> exe file name, from a Toolhelp snapshot (no process handles are opened).
std::unordered_map<DWORD, std::wstring> SnapshotExeNames() {
    std::unordered_map<DWORD, std::wstring> map;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return map;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            map[pe.th32ProcessID] = pe.szExeFile;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return map;
}

struct EnumCtx {
    DWORD selfPid;
    std::vector<SourceWindow>* out;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0)
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->selfPid) return TRUE;

    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return TRUE;
    std::wstring title(static_cast<size_t>(len) + 1, L'\0');
    int got = GetWindowTextW(hwnd, &title[0], len + 1);
    if (got <= 0) return TRUE;
    title.resize(static_cast<size_t>(got));

    SourceWindow w;
    w.hwnd = hwnd;
    w.pid = pid;
    w.title = std::move(title);
    RECT rc{};
    if (GetClientRect(hwnd, &rc)) {
        w.clientW = rc.right - rc.left;
        w.clientH = rc.bottom - rc.top;
    }
    ctx->out->push_back(std::move(w));
    return TRUE;
}

} // namespace

std::wstring SourceWindow::Label() const {
    std::wstring t = title;
    if (t.size() > 60) {
        t.resize(60);
        t += L"…";
    }
    wchar_t tail[128];
    swprintf_s(tail, L" | %d×%d | HWND 0x%08X", clientW, clientH,
               static_cast<unsigned int>(reinterpret_cast<UINT_PTR>(hwnd)));
    return t + L" | " + exe + tail;
}

std::vector<SourceWindow> EnumSourceWindows(const std::wstring& match) {
    std::vector<SourceWindow> list;
    EnumCtx ctx{GetCurrentProcessId(), &list};
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));

    auto exes = SnapshotExeNames();
    std::wstring m = ToLower(match);
    for (auto& w : list) {
        auto it = exes.find(w.pid);
        if (it != exes.end()) w.exe = it->second;
        w.score = 0;
        if (!m.empty()) {
            if (ContainsNoCase(w.title, m)) w.score += 2;
            if (ContainsNoCase(w.exe, m)) w.score += 2;
        }
    }

    std::sort(list.begin(), list.end(), [](const SourceWindow& a, const SourceWindow& b) {
        if (a.score != b.score) return a.score > b.score;
        int c = CompareStringOrdinal(a.title.c_str(), static_cast<int>(a.title.size()),
                                     b.title.c_str(), static_cast<int>(b.title.size()), TRUE);
        return c == CSTR_LESS_THAN;
    });
    return list;
}

std::wstring MonitorEntry::Label() const {
    wchar_t buf[512];
    swprintf_s(buf, L"%d×%d @ %d Hz - %s (%s)%s", width, height, hz,
               friendly.empty() ? L"Unknown monitor" : friendly.c_str(), device.c_str(),
               primary ? L" [primary]" : L"");
    return buf;
}

namespace {

BOOL CALLBACK MonitorEnumProc(HMONITOR hmon, HDC, LPRECT, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<MonitorEntry>*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, &mi)) return TRUE;

    MonitorEntry e;
    e.hmon = hmon;
    e.device = mi.szDevice;
    e.rect = mi.rcMonitor;
    e.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    e.width = mi.rcMonitor.right - mi.rcMonitor.left;
    e.height = mi.rcMonitor.bottom - mi.rcMonitor.top;

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        e.width = static_cast<int>(dm.dmPelsWidth);
        e.height = static_cast<int>(dm.dmPelsHeight);
        e.hz = static_cast<int>(dm.dmDisplayFrequency);
    }
    out->push_back(std::move(e));
    return TRUE;
}

void FillFromDisplayConfig(std::vector<MonitorEntry>& mons) {
    UINT32 numPaths = 0, numModes = 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG rc = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 4 && rc == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPaths, &numModes) != ERROR_SUCCESS)
            return;
        paths.resize(numPaths);
        modes.resize(numModes);
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPaths, paths.data(), &numModes, modes.data(), nullptr);
    }
    if (rc != ERROR_SUCCESS) return;
    paths.resize(numPaths);

    for (const auto& p : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = p.sourceInfo.adapterId;
        src.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;

        for (auto& m : mons) {
            if (_wcsicmp(m.device.c_str(), src.viewGdiDeviceName) != 0) continue;

            DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
            tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tgt.header.size = sizeof(tgt);
            tgt.header.adapterId = p.targetInfo.adapterId;
            tgt.header.id = p.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS && m.friendly.empty())
                m.friendly = tgt.monitorFriendlyDeviceName;

            const auto& rr = p.targetInfo.refreshRate;
            if (rr.Numerator != 0 && rr.Denominator != 0) {
                m.hz = static_cast<int>(std::lround(static_cast<double>(rr.Numerator) /
                                                    static_cast<double>(rr.Denominator)));
            }
        }
    }
}

} // namespace

std::vector<MonitorEntry> EnumMonitors() {
    std::vector<MonitorEntry> mons;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&mons));
    FillFromDisplayConfig(mons);
    std::stable_sort(mons.begin(), mons.end(), [](const MonitorEntry& a, const MonitorEntry& b) {
        if (a.primary != b.primary) return a.primary;
        return a.rect.left < b.rect.left;
    });
    return mons;
}

RECT ClientRectOnScreen(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    POINT tl{0, 0};
    ClientToScreen(hwnd, &tl);
    OffsetRect(&rc, tl.x, tl.y);
    return rc;
}
