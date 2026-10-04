// StretchScaler - external 4:3 -> 16:9 stretch presenter for windowed games (built for H1Z1).
#include <Unknwn.h>
#include <windows.h>
#include <commctrl.h>
#include <magnification.h>
#include <shellapi.h>
#include <windowsx.h>
#include <winrt/base.h>
#include <cmath>

#include <string>
#include <vector>

#include "Enum.h"
#include "Log.h"
#include "Scaler.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "Magnification.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

enum : UINT { WM_APP_LOG = WM_APP + 1, WM_APP_STOPPED, WM_APP_TRAY, WM_APP_SHOWUI };
enum : int {
    ID_SOURCE = 100, ID_REFRESH, ID_MATCH, ID_MONITOR, ID_SCALE, ID_FILTER, ID_PRESENT,
    ID_ALIGN, ID_CURSOR, ID_CLIP, ID_BORDER, ID_FPS, ID_FPSCORNER, ID_MINIMIZE, ID_AUTOSCALE, ID_TRAY_OPEN, ID_TRAY_EXIT, ID_START, ID_LOG, ID_INFO, ID_STATUS,
    HK_TOGGLE = 1, HK_PANIC = 2, TIMER_TICK = 1
};

struct Settings {
    std::wstring match = L"H1Z1";
    std::wstring monDevice, monName;
    int monW = 0, monH = 0;
    int scale = 0;    // 0 stretch, 1 fit
    int filter = 0;   // 0 bilinear, 1 point
    int present = 0;  // 0 no vsync, 1 vsync
    bool align = true, cursor = true, clip = true, border = true;
    bool fps = false;
    bool minimize = true;
    bool autoScale = true;  // start scaling automatically when the game window is detected
    int fpsCorner = 0;
};

std::wstring IniPath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L'\\') + 1) + L"StretchScaler.ini";
}

std::wstring IniGet(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[512];
    GetPrivateProfileStringW(L"Settings", key, def, buf, 512, IniPath().c_str());
    return buf;
}
int IniGetInt(const wchar_t* key, int def) { return GetPrivateProfileIntW(L"Settings", key, def, IniPath().c_str()); }
void IniSet(const wchar_t* key, const std::wstring& v) { WritePrivateProfileStringW(L"Settings", key, v.c_str(), IniPath().c_str()); }

struct App {
    HWND wnd = nullptr;
    HWND cbSource, btnRefresh, stInfo, edMatch, cbMonitor, cbScale, cbFilter, cbPresent;
    HWND chkAlign, chkCursor, chkClip, chkBorder, chkFps, cbFpsCorner, chkMinimize, chkAutoScale, btnStart, stStatus, edLog;
    HFONT font = nullptr;
    UINT dpi = 96;

    Settings cfg;
    std::vector<SourceWindow> sources;
    std::vector<MonitorEntry> monitors;
    Scaler scaler;

    HWND activeSrc = nullptr;
    RECT activeMon{};
    bool wasFocused = false;
    bool overlayShown = false, cursorHidden = false, clipped = false;
    bool magInit = false;
    HWINEVENTHOOK hookFg = nullptr, hookMin = nullptr;
    int tick = 0;

    // tray
    NOTIFYICONDATAW nid{};
    bool trayAdded = false, exiting = false, hideHintShown = false;
    UINT taskbarCreatedMsg = 0;
    // auto-start: require the detected window to be stable for a few seconds; never auto-restart a window the
    // user stopped manually or one that failed to start.
    HWND suppressHwnd = nullptr, stableHwnd = nullptr;
    int stableCount = 0, stableW = 0, stableH = 0;

    int S(int v) const { return MulDiv(v, dpi, 96); }
};
App g;

// ---------- system cursor hide/show ----------
void ShowRealCursor(bool show) {
    using ShowSystemCursorFn = BOOL(WINAPI*)(BOOL);
    static auto fn = (ShowSystemCursorFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "ShowSystemCursor");
    if (fn && fn(show ? TRUE : FALSE)) return;
    if (!g.magInit) g.magInit = MagInitialize() != FALSE;
    if (g.magInit) MagShowSystemCursor(show ? TRUE : FALSE);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS*) {
    ShowRealCursor(true);
    ClipCursor(nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------- UI helpers ----------
int ComboSel(HWND cb) { return (int)SendMessageW(cb, CB_GETCURSEL, 0, 0); }
bool Checked(HWND b) { return SendMessageW(b, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void SetChecked(HWND b, bool v) { SendMessageW(b, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0); }
std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    return s;
}

void UpdateSourceInfo() {
    int i = ComboSel(g.cbSource);
    if (i < 0 || i >= (int)g.sources.size()) { SetWindowTextW(g.stInfo, L"No source selected."); return; }
    auto& s = g.sources[i];
    RECT cr{};
    GetClientRect(s.hwnd, &cr);
    wchar_t buf[512];
    swprintf_s(buf, L"HWND 0x%08X  PID %lu  %s  client %ldx%ld%s", (unsigned)(uintptr_t)s.hwnd, s.pid, s.exe.c_str(),
               cr.right, cr.bottom, (cr.right == 1920 && cr.bottom == 1440) ? L"  (4:3 OK)" : L"");
    SetWindowTextW(g.stInfo, buf);
}

void RefreshSources() {
    HWND prev = nullptr;
    int cur = ComboSel(g.cbSource);
    if (cur >= 0 && cur < (int)g.sources.size()) prev = g.sources[cur].hwnd;
    g.cfg.match = GetText(g.edMatch);
    g.sources = EnumSourceWindows(g.cfg.match);
    SendMessageW(g.cbSource, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < g.sources.size(); ++i) {
        SendMessageW(g.cbSource, CB_ADDSTRING, 0, (LPARAM)g.sources[i].Label().c_str());
        if (g.sources[i].hwnd == prev) sel = (int)i;
    }
    if (sel < 0 && !g.sources.empty() && g.sources[0].score > 0) sel = 0; // auto-detect best match
    SendMessageW(g.cbSource, CB_SETCURSEL, sel, 0);
    UpdateSourceInfo();
}

void RefreshMonitors() {
    g.monitors = EnumMonitors();
    SendMessageW(g.cbMonitor, CB_RESETCONTENT, 0, 0);
    int sel = -1, byName = -1, byRes = -1;
    for (size_t i = 0; i < g.monitors.size(); ++i) {
        auto& m = g.monitors[i];
        SendMessageW(g.cbMonitor, CB_ADDSTRING, 0, (LPARAM)m.Label().c_str());
        bool resMatch = m.width == g.cfg.monW && m.height == g.cfg.monH;
        if (resMatch && m.device == g.cfg.monDevice && m.friendly == g.cfg.monName) sel = (int)i;
        if (resMatch && !g.cfg.monName.empty() && m.friendly == g.cfg.monName && byName < 0) byName = (int)i;
        if (resMatch && byRes < 0) byRes = (int)i;
    }
    if (sel < 0) sel = byName >= 0 ? byName : byRes;
    if (sel < 0) { // default: highest refresh rate monitor
        for (size_t i = 0; i < g.monitors.size(); ++i)
            if (sel < 0 || g.monitors[i].hz > g.monitors[sel].hz) sel = (int)i;
    }
    SendMessageW(g.cbMonitor, CB_SETCURSEL, sel, 0);
}

void SaveSettings() {
    auto& c = g.cfg;
    c.match = GetText(g.edMatch);
    int mi = ComboSel(g.cbMonitor);
    if (mi >= 0 && mi < (int)g.monitors.size()) {
        auto& m = g.monitors[mi];
        c.monDevice = m.device; c.monName = m.friendly; c.monW = m.width; c.monH = m.height;
    }
    c.scale = ComboSel(g.cbScale); c.filter = ComboSel(g.cbFilter); c.present = ComboSel(g.cbPresent);
    c.align = Checked(g.chkAlign); c.cursor = Checked(g.chkCursor); c.clip = Checked(g.chkClip); c.border = Checked(g.chkBorder);
    c.fps = Checked(g.chkFps); c.fpsCorner = ComboSel(g.cbFpsCorner); c.minimize = Checked(g.chkMinimize);
    c.autoScale = Checked(g.chkAutoScale);
    IniSet(L"Match", c.match);
    IniSet(L"MonitorDevice", c.monDevice);
    IniSet(L"MonitorName", c.monName);
    IniSet(L"MonitorWidth", std::to_wstring(c.monW));
    IniSet(L"MonitorHeight", std::to_wstring(c.monH));
    IniSet(L"Scaling", std::to_wstring(c.scale));
    IniSet(L"Filter", std::to_wstring(c.filter));
    IniSet(L"Present", std::to_wstring(c.present));
    IniSet(L"AlignSource", c.align ? L"1" : L"0");
    IniSet(L"DrawCursor", c.cursor ? L"1" : L"0");
    IniSet(L"ClipCursor", c.clip ? L"1" : L"0");
    IniSet(L"HideBorder", c.border ? L"1" : L"0");
    IniSet(L"FpsOverlay", c.fps ? L"1" : L"0");
    IniSet(L"MinimizeOnFocusLoss", c.minimize ? L"1" : L"0");
    IniSet(L"AutoStartScaling", c.autoScale ? L"1" : L"0");
    IniSet(L"FpsCorner", std::to_wstring(c.fpsCorner));
}

void LoadSettings() {
    auto& c = g.cfg;
    c.match = IniGet(L"Match", L"H1Z1");
    c.monDevice = IniGet(L"MonitorDevice", L"");
    c.monName = IniGet(L"MonitorName", L"");
    c.monW = IniGetInt(L"MonitorWidth", 0);
    c.monH = IniGetInt(L"MonitorHeight", 0);
    c.scale = IniGetInt(L"Scaling", 0);
    c.filter = IniGetInt(L"Filter", 0);
    c.present = IniGetInt(L"Present", 0);
    c.align = IniGetInt(L"AlignSource", 1) != 0;
    c.cursor = IniGetInt(L"DrawCursor", 1) != 0;
    c.clip = IniGetInt(L"ClipCursor", 1) != 0;
    c.border = IniGetInt(L"HideBorder", 1) != 0;
    c.fps = IniGetInt(L"FpsOverlay", 0) != 0;
    c.minimize = IniGetInt(L"MinimizeOnFocusLoss", 1) != 0;
    c.autoScale = IniGetInt(L"AutoStartScaling", 1) != 0;
    c.fpsCorner = IniGetInt(L"FpsCorner", 0);
}

void SetControlsEnabled(bool en) {
    for (HWND h : {g.cbSource, g.btnRefresh, g.edMatch, g.cbMonitor, g.cbScale, g.cbFilter, g.cbPresent,
                   g.chkAlign, g.chkCursor, g.chkClip, g.chkBorder, g.chkFps, g.cbFpsCorner, g.chkMinimize})
        EnableWindow(h, en);
    SetWindowTextW(g.btnStart, en ? L"Start Scaling  (Ctrl+Alt+S)" : L"Stop Scaling  (Ctrl+Alt+S)");
}

// ---------- icon ----------
HICON AppIcon(int size) {
    HICON icon = nullptr;
    if (FAILED(LoadIconWithScaleDown(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), size, size, &icon)) || !icon)
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    return icon;
}

// ---------- tray ----------
void SetTrayTip(const std::wstring& tip) {
    if (!g.trayAdded) return;
    g.nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(g.nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
}

void AddTray() {
    g.nid = {sizeof(g.nid)};
    g.nid.hWnd = g.wnd;
    g.nid.uID = 1;
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g.nid.uCallbackMessage = WM_APP_TRAY;
    g.nid.hIcon = AppIcon(GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(g.wnd)));
    wcscpy_s(g.nid.szTip, L"StretchScaler");
    g.trayAdded = Shell_NotifyIconW(NIM_ADD, &g.nid) != FALSE;
    if (g.trayAdded) {
        g.nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g.nid);
    }
}

void TrayBalloon(const wchar_t* title, const wchar_t* text) {
    if (!g.trayAdded) return;
    g.nid.uFlags = NIF_INFO;
    wcscpy_s(g.nid.szInfoTitle, title);
    wcscpy_s(g.nid.szInfo, text);
    g.nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
}

void ShowUI() {
    ShowWindow(g.wnd, SW_SHOW);
    if (IsIconic(g.wnd)) ShowWindow(g.wnd, SW_RESTORE);
    SetForegroundWindow(g.wnd);
}

void HideUI() {
    ShowWindow(g.wnd, SW_HIDE);
    if (!g.hideHintShown) {
        g.hideHintShown = true;
        TrayBalloon(L"StretchScaler is still running", L"It will stretch H1Z1 automatically. Right-click the tray icon to exit.");
    }
}

// ---------- focus handling ----------
void ReleaseInputTweaks() {
    if (g.cursorHidden) { ShowRealCursor(true); g.cursorHidden = false; }
    if (g.clipped) { ClipCursor(nullptr); g.clipped = false; }
}

void ApplyClip() {
    RECT cl = ClientRectOnScreen(g.activeSrc);
    RECT cur{};
    GetClipCursor(&cur);
    // Respect a tighter clip the game set itself (e.g. while aiming); otherwise confine to the game's client area.
    bool inside = cur.left >= cl.left && cur.top >= cl.top && cur.right <= cl.right && cur.bottom <= cl.bottom;
    if (!inside) ClipCursor(&cl);
    g.clipped = true;
}

void UpdateFocusState() {
    if (!g.scaler.Running()) return;
    if (!IsWindow(g.activeSrc)) return;
    HWND fg = GetForegroundWindow();
    bool focused = fg && (fg == g.activeSrc || GetAncestor(fg, GA_ROOTOWNER) == g.activeSrc) && !IsIconic(g.activeSrc);
    static const bool forceVisible = GetEnvironmentVariableW(L"STRETCHSCALER_FORCE_VISIBLE", nullptr, 0) > 0; // testing only
    if (forceVisible) focused = true;
    if (focused) {
        if (!g.overlayShown) { g.scaler.SetVisible(true); g.overlayShown = true; }
        if (g.cfg.cursor && !g.cursorHidden) { ShowRealCursor(false); g.cursorHidden = true; }
        if (g.cfg.clip) ApplyClip();
        g.wasFocused = true;
    } else {
        if (g.overlayShown) { g.scaler.SetVisible(false); g.overlayShown = false; }
        ReleaseInputTweaks();
        // Focus moved to another real window (Alt+Tab switcher, another app, the taskbar...): minimize the game.
        // SW_SHOWMINNOACTIVE so the window you switched to keeps focus. Capture simply pauses while minimized.
        if (g.wasFocused && fg && g.cfg.minimize && !IsIconic(g.activeSrc)) {
            ShowWindowAsync(g.activeSrc, SW_SHOWMINNOACTIVE);
            Log(L"Game lost focus - minimized it.");
        }
        g.wasFocused = false;
    }
}

void AlignSource(HWND src, const RECT& mon) {
    // Move only (never resize): put the client area's top-left on the output monitor's top-left.
    RECT wr{}, cl = ClientRectOnScreen(src);
    GetWindowRect(src, &wr);
    int x = mon.left - (cl.left - wr.left), y = mon.top - (cl.top - wr.top);
    if (wr.left != x || wr.top != y) {
        SetWindowPos(src, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        Log(L"Moved source window to (%d,%d) - size untouched.", x, y);
    }
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_MINIMIZEEND && hwnd == g.activeSrc && g.cfg.align) AlignSource(hwnd, g.activeMon);
    UpdateFocusState();
}

void StopScaling(bool byUser = false) {
    if (byUser && g.activeSrc) g.suppressHwnd = g.activeSrc; // don't auto-restart until the game is relaunched
    if (g.hookFg) { UnhookWinEvent(g.hookFg); g.hookFg = nullptr; }
    if (g.hookMin) { UnhookWinEvent(g.hookMin); g.hookMin = nullptr; }
    ReleaseInputTweaks();
    g.scaler.Stop();
    g.overlayShown = false;
    g.activeSrc = nullptr;
    SetControlsEnabled(true);
    SetWindowTextW(g.stStatus, L"Stopped.");
    SetTrayTip(L"StretchScaler - waiting for the game");
}

void StartScaling() {
    SaveSettings();
    int si = ComboSel(g.cbSource);
    if (si < 0 || si >= (int)g.sources.size() || !IsWindow(g.sources[si].hwnd)) {
        RefreshSources();
        si = ComboSel(g.cbSource);
    }
    if (si < 0 || si >= (int)g.sources.size()) { Log(L"No source window found (looking for \"%s\").", g.cfg.match.c_str()); return; }
    int mi = ComboSel(g.cbMonitor);
    if (mi < 0 || mi >= (int)g.monitors.size()) { Log(L"No output monitor selected."); return; }
    auto src = g.sources[si];
    auto mon = g.monitors[mi];
    Log(L"Source: \"%s\" (%s, PID %lu, HWND 0x%08X)", src.title.c_str(), src.exe.c_str(), src.pid, (unsigned)(uintptr_t)src.hwnd);
    Log(L"Output: %s", mon.Label().c_str());

    if (IsIconic(src.hwnd)) { // can't capture a minimized window
        ShowWindow(src.hwnd, SW_RESTORE);
        for (int i = 0; i < 50 && IsIconic(src.hwnd); ++i) Sleep(20);
    }
    if (g.cfg.align) AlignSource(src.hwnd, mon.rect);

    ScalerOptions o;
    o.stretch = g.cfg.scale == 0;
    o.pointFilter = g.cfg.filter == 1;
    o.vsync = g.cfg.present == 1;
    o.drawCursor = g.cfg.cursor;
    o.hideBorder = g.cfg.border;
    o.fpsOverlay = g.cfg.fps;
    o.fpsCorner = g.cfg.fpsCorner;
    if (!g.scaler.Start(src.hwnd, mon.hmon, mon.rect, o, g.wnd, WM_APP_STOPPED)) {
        SetWindowTextW(g.stStatus, L"Start failed - see log.");
        g.suppressHwnd = src.hwnd; // no auto-retry loop on this window
        return;
    }
    SetTrayTip(L"StretchScaler - scaling " + src.title);
    g.activeSrc = src.hwnd;
    g.activeMon = mon.rect;
    g.wasFocused = false;
    g.hookFg = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    g.hookMin = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    SetControlsEnabled(false);
    SetWindowTextW(g.stStatus, L"Scaling. Output shows while the game is the foreground window. Click the game to start.");
    UpdateFocusState();
}

void ToggleScaling() {
    if (g.scaler.Running()) { StopScaling(true); return; }
    g.suppressHwnd = nullptr;
    StartScaling();
}

// Called once a second while idle with a selected source: start scaling once the game window has been
// visible, not minimized and the same size for 3 s, and its aspect differs from the output (i.e. needs stretching).
void TryAutoStart() {
    if (!g.cfg.autoScale) return;
    int si = ComboSel(g.cbSource), mi = ComboSel(g.cbMonitor);
    if (si < 0 || si >= (int)g.sources.size() || mi < 0 || mi >= (int)g.monitors.size()) return;
    const auto& s = g.sources[si];
    const auto& m = g.monitors[mi];
    RECT cr{};
    bool ok = s.score > 0 && s.hwnd != g.suppressHwnd && IsWindowVisible(s.hwnd) && !IsIconic(s.hwnd) && GetClientRect(s.hwnd, &cr);
    int w = cr.right, h = cr.bottom;
    ok = ok && w >= 320 && h >= 240 && std::fabs((double)w / h - (double)m.width / m.height) > 0.02;
    if (!ok) { g.stableCount = 0; return; }
    if (s.hwnd == g.stableHwnd && w == g.stableW && h == g.stableH) ++g.stableCount;
    else { g.stableHwnd = s.hwnd; g.stableW = w; g.stableH = h; g.stableCount = 1; }
    if (g.stableCount >= 3) {
        g.stableCount = 0;
        Log(L"Game window ready (%dx%d) - starting scaling automatically.", w, h);
        StartScaling();
    }
}

// ---------- window ----------
HWND MakeCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, g.S(x), g.S(y), g.S(w), g.S(h), g.wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
    return c;
}

void AddItems(HWND cb, std::initializer_list<const wchar_t*> items, int sel) {
    for (auto s : items) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)s);
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

void CreateControls() {
    g.dpi = GetDpiForWindow(g.wnd);
    g.font = CreateFontW(-MulDiv(9, g.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    const DWORD cbStyle = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
    int y = 12;
    MakeCtl(L"STATIC", L"Source window", 0, 12, y + 3, 100, 20, -1);
    g.cbSource = MakeCtl(L"COMBOBOX", L"", cbStyle, 115, y, 420, 300, ID_SOURCE);
    g.btnRefresh = MakeCtl(L"BUTTON", L"Refresh", WS_TABSTOP, 545, y - 1, 83, 25, ID_REFRESH);
    y += 30;
    g.stInfo = MakeCtl(L"STATIC", L"", 0, 115, y, 513, 20, ID_INFO);
    y += 26;
    MakeCtl(L"STATIC", L"Auto-detect match", 0, 12, y + 3, 100, 20, -1);
    g.edMatch = MakeCtl(L"EDIT", g.cfg.match.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 115, y, 150, 23, ID_MATCH, WS_EX_CLIENTEDGE);
    MakeCtl(L"STATIC", L"(title or exe contains, case-insensitive)", 0, 275, y + 3, 300, 20, -1);
    y += 34;
    MakeCtl(L"STATIC", L"Output monitor", 0, 12, y + 3, 100, 20, -1);
    g.cbMonitor = MakeCtl(L"COMBOBOX", L"", cbStyle, 115, y, 513, 300, ID_MONITOR);
    y += 34;
    MakeCtl(L"STATIC", L"Scaling", 0, 12, y + 3, 100, 20, -1);
    g.cbScale = MakeCtl(L"COMBOBOX", L"", cbStyle, 115, y, 200, 200, ID_SCALE);
    AddItems(g.cbScale, {L"Stretch to fill (4:3 → 16:9)", L"Fit (keep aspect, black bars)"}, g.cfg.scale);
    MakeCtl(L"STATIC", L"Filter", 0, 335, y + 3, 50, 20, -1);
    g.cbFilter = MakeCtl(L"COMBOBOX", L"", cbStyle, 385, y, 243, 200, ID_FILTER);
    AddItems(g.cbFilter, {L"Bilinear", L"Point (nearest)"}, g.cfg.filter);
    y += 34;
    MakeCtl(L"STATIC", L"Present mode", 0, 12, y + 3, 100, 20, -1);
    g.cbPresent = MakeCtl(L"COMBOBOX", L"", cbStyle, 115, y, 513, 200, ID_PRESENT);
    AddItems(g.cbPresent, {L"Lowest latency: no VSync, Present(0), tearing flag if supported, 1-frame queue",
                           L"VSync: Present(1), 1-frame queue"}, g.cfg.present);
    y += 36;
    g.chkAlign = MakeCtl(L"BUTTON", L"Align source window to output monitor (moves only, never resizes)", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 600, 20, ID_ALIGN);
    y += 24;
    g.chkCursor = MakeCtl(L"BUTTON", L"Hide real cursor && draw it at the stretched position (menus line up)", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 600, 20, ID_CURSOR);
    y += 24;
    g.chkClip = MakeCtl(L"BUTTON", L"Confine cursor to the game while it is focused", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 600, 20, ID_CLIP);
    y += 24;
    g.chkBorder = MakeCtl(L"BUTTON", L"Hide the yellow capture border (Windows 11)", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 600, 20, ID_BORDER);
    SetChecked(g.chkAlign, g.cfg.align); SetChecked(g.chkCursor, g.cfg.cursor);
    SetChecked(g.chkClip, g.cfg.clip); SetChecked(g.chkBorder, g.cfg.border);
    y += 24;
    g.chkFps = MakeCtl(L"BUTTON", L"Show FPS overlay in game", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 190, 20, ID_FPS);
    g.cbFpsCorner = MakeCtl(L"COMBOBOX", L"", cbStyle, 205, y - 2, 120, 200, ID_FPSCORNER);
    AddItems(g.cbFpsCorner, {L"Top-left", L"Top-right", L"Bottom-left", L"Bottom-right"}, g.cfg.fpsCorner);
    MakeCtl(L"STATIC", L"frames shown on screen (max = refresh rate)", 0, 335, y + 1, 290, 20, -1);
    SetChecked(g.chkFps, g.cfg.fps);
    y += 24;
    g.chkMinimize = MakeCtl(L"BUTTON", L"Minimize the game to the taskbar when it loses focus (Alt+Tab)", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 600, 20, ID_MINIMIZE);
    SetChecked(g.chkMinimize, g.cfg.minimize);
    y += 24;
    g.chkAutoScale = MakeCtl(L"BUTTON", L"Start scaling automatically when the game appears (stops when it closes)", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 600, 20, ID_AUTOSCALE);
    SetChecked(g.chkAutoScale, g.cfg.autoScale);
    y += 32;
    g.btnStart = MakeCtl(L"BUTTON", L"Start Scaling  (Ctrl+Alt+S)", BS_DEFPUSHBUTTON | WS_TABSTOP, 12, y, 220, 32, ID_START);
    MakeCtl(L"STATIC", L"Panic stop: Ctrl+Alt+X.  Closing this window keeps it running in the tray.", 0, 245, y + 8, 390, 20, -1);
    y += 40;
    g.stStatus = MakeCtl(L"STATIC", L"Stopped.", 0, 12, y, 616, 20, ID_STATUS);
    y += 26;
    g.edLog = MakeCtl(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 12, y, 616, 200, ID_LOG, WS_EX_CLIENTEDGE);
    SendMessageW(g.edLog, EM_SETLIMITTEXT, 0, 0);
}

void AppendLog(const wchar_t* line) {
    int len = GetWindowTextLengthW(g.edLog);
    if (len > 200000) { SetWindowTextW(g.edLog, L""); len = 0; }
    SendMessageW(g.edLog, EM_SETSEL, len, len);
    std::wstring s = line;
    s += L"\r\n";
    SendMessageW(g.edLog, EM_REPLACESEL, FALSE, (LPARAM)s.c_str());
}

LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g.taskbarCreatedMsg && msg == g.taskbarCreatedMsg) { AddTray(); return 0; } // Explorer restarted
    switch (msg) {
    case WM_CREATE:
        g.wnd = h;
        CreateControls();
        LogInit(h, WM_APP_LOG);
        Log(L"StretchScaler started. Log file: StretchScaler.log next to the exe.");
        RefreshMonitors();
        RefreshSources();
        if (!RegisterHotKey(h, HK_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S')) Log(L"Could not register Ctrl+Alt+S (in use by another app).");
        if (!RegisterHotKey(h, HK_PANIC, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'X')) Log(L"Could not register Ctrl+Alt+X (in use by another app).");
        SetTimer(h, TIMER_TICK, 250, nullptr);
        g.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
        AddTray();
        SetTrayTip(L"StretchScaler - waiting for the game");
        {
            // Command line: /autostart  -> start scaling immediately;  /exitafter:N -> quit after N seconds (testing).
            std::wstring cmd = GetCommandLineW();
            if (cmd.find(L"/autostart") != std::wstring::npos) PostMessageW(h, WM_COMMAND, ID_START, 0);
            size_t p = cmd.find(L"/exitafter:");
            if (p != std::wstring::npos) SetTimer(h, 99, (UINT)_wtoi(cmd.c_str() + p + 11) * 1000, nullptr);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_START: ToggleScaling(); break;
        case ID_REFRESH: RefreshSources(); RefreshMonitors(); break;
        case ID_SOURCE: if (HIWORD(wp) == CBN_SELCHANGE) UpdateSourceInfo(); break;
        case ID_SCALE: case ID_FILTER: case ID_PRESENT: case ID_MONITOR: case ID_FPSCORNER:
            if (HIWORD(wp) == CBN_SELCHANGE) SaveSettings();
            break;
        case ID_ALIGN: case ID_CURSOR: case ID_CLIP: case ID_BORDER: case ID_FPS: case ID_MINIMIZE: case ID_AUTOSCALE:
            SaveSettings();
            if (LOWORD(wp) == ID_AUTOSCALE && g.cfg.autoScale) g.suppressHwnd = nullptr;
            break;
        case ID_TRAY_OPEN: ShowUI(); break;
        case ID_TRAY_EXIT: g.exiting = true; DestroyWindow(h); break;
        }
        return 0;
    case WM_HOTKEY:
        if (wp == HK_TOGGLE) ToggleScaling();
        else if (wp == HK_PANIC) { Log(L"Panic stop (Ctrl+Alt+X)."); StopScaling(true); ShowRealCursor(true); ClipCursor(nullptr); }
        return 0;
    case WM_TIMER:
        if (wp == 99) { DestroyWindow(h); return 0; }
        if (g.scaler.Running()) {
            UpdateFocusState();
            if (++g.tick % 4 == 0) {
                auto st = g.scaler.TakeStats();
                wchar_t buf[256];
                static const wchar_t* modes[] = {L"composed by DWM (+1 refresh)", L"hardware overlay (direct)",
                                                 L"direct flip", L"composition failure"};
                const wchar_t* mode = (st.presentMode >= 0 && st.presentMode <= 3) ? modes[st.presentMode] : L"measuring...";
                static int lastMode = -2;
                if (g.overlayShown && st.presentMode >= 0 && st.presentMode != lastMode) {
                    lastMode = st.presentMode;
                    Log(L"Output presentation mode: %s", mode);
                }
                swprintf_s(buf, L"%s  |  capture %u fps  |  present %u fps  |  lead %+.1f ms  |  output: %s",
                           g.overlayShown ? L"ACTIVE" : L"standby", st.captured, st.presented, st.avgLeadMs, mode);
                SetWindowTextW(g.stStatus, buf);
                if (g.tick % 120 == 0 || g.tick == 24 || g.tick == 40) Log(L"%s", buf); // early samples, then every 30 s
                SetTrayTip(std::wstring(L"StretchScaler - ") + (g.overlayShown ? L"ACTIVE" : L"standby"));
            }
        } else if (++g.tick % 4 == 0) {
            // Auto-detect: rescan once a second until a source is selected, then stop scanning.
            // If that window goes away (game closed), scanning resumes automatically.
            int si = ComboSel(g.cbSource);
            bool haveSource = si >= 0 && si < (int)g.sources.size() && IsWindow(g.sources[si].hwnd);
            if (!haveSource) {
                RefreshSources();
                si = ComboSel(g.cbSource);
                if (si >= 0 && si < (int)g.sources.size()) {
                    Log(L"Auto-detected source: \"%s\" (%s). Auto-detect paused.", g.sources[si].title.c_str(), g.sources[si].exe.c_str());
                    TryAutoStart();
                } else {
                    SetWindowTextW(g.stInfo, (L"Waiting for a window matching \"" + GetText(g.edMatch) + L"\"...").c_str());
                }
            } else {
                UpdateSourceInfo();
                TryAutoStart();
            }
        }
        return 0;
    case WM_APP_LOG: {
        auto* s = (wchar_t*)lp;
        AppendLog(s);
        delete[] s;
        return 0;
    }
    case WM_APP_STOPPED:
        StopScaling();
        return 0;
    case WM_APP_SHOWUI:
        ShowUI();
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONDBLCLK:
            ShowUI();
            break;
        case WM_CONTEXTMENU: {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Open StretchScaler");
            AppendMenuW(m, MF_STRING | (g.scaler.Running() ? MF_CHECKED : 0), ID_START, L"Scaling on  (Ctrl+Alt+S)");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Exit");
            SetMenuDefaultItem(m, ID_TRAY_OPEN, FALSE);
            SetForegroundWindow(h);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, GET_X_LPARAM(wp), GET_Y_LPARAM(wp), 0, h, nullptr);
            PostMessageW(h, WM_NULL, 0, 0);
            DestroyMenu(m);
            break;
        }
        }
        return 0;
    case WM_CLOSE:
        if (!g.exiting && g.trayAdded) { HideUI(); return 0; }
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE && g.trayAdded) { HideUI(); return 0; }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g.edLog) break;
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY:
        if (g.trayAdded) Shell_NotifyIconW(NIM_DELETE, &g.nid);
        StopScaling();
        ShowRealCursor(true);
        if (g.magInit) MagUninitialize();
        UnregisterHotKey(h, HK_TOGGLE);
        UnregisterHotKey(h, HK_PANIC);
        LogShutdown();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show) {
    // Single instance: a second launch just opens the running one's window.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\StretchScalerSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"StretchScalerMain", nullptr)) PostMessageW(other, WM_APP_SHOWUI, 0, 0);
        return 0;
    }
    const bool background = wcsstr(cmdLine, L"/background") != nullptr; // start hidden in the tray
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetUnhandledExceptionFilter(CrashFilter);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    LoadSettings();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    UINT sysDpi = GetDpiForSystem();
    wc.hIcon = AppIcon(GetSystemMetricsForDpi(SM_CXICON, sysDpi));
    wc.hIconSm = AppIcon(GetSystemMetricsForDpi(SM_CXSMICON, sysDpi));
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"StretchScalerMain";
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    RECT r{0, 0, MulDiv(640, dpi, 96), MulDiv(714, dpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"StretchScaler - 4:3 stretch presenter",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!background) ShowWindow(wnd, show);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (mutex) CloseHandle(mutex);
    return 0;
}
