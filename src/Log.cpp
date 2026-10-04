#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>

#include "Log.h"

namespace {
std::mutex g_logMutex;
FILE* g_logFile = nullptr;
HWND g_notifyWnd = nullptr;
UINT g_notifyMsg = 0;
} // namespace

void LogInit(HWND notifyWnd, UINT notifyMsg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_notifyWnd = notifyWnd;
    g_notifyMsg = notifyMsg;
    if (g_logFile) return;

    wchar_t exePath[MAX_PATH * 4] = {};
    DWORD n = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(sizeof(exePath) / sizeof(exePath[0])));
    std::wstring path(exePath, n);
    size_t slash = path.find_last_of(L"\\/");
    path = (slash == std::wstring::npos) ? std::wstring() : path.substr(0, slash + 1);
    path += L"StretchScaler.log";

    FILE* f = _wfopen(path.c_str(), L"w, ccs=UTF-8");
    g_logFile = f;
}

void LogShutdown() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        fflush(g_logFile);
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    g_notifyWnd = nullptr;
    g_notifyMsg = 0;
}

void Log(const wchar_t* fmt, ...) {
    wchar_t msg[2048];
    va_list args;
    va_start(args, fmt);
    int r = _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, args);
    va_end(args);
    if (r < 0) msg[_countof(msg) - 1] = L'\0';

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t prefix[32];
    swprintf_s(prefix, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::wstring line = prefix;
    line += msg;

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        fputws(line.c_str(), g_logFile);
        fputws(L"\n", g_logFile);
        fflush(g_logFile);
    }
    OutputDebugStringW((line + L"\r\n").c_str());

    if (g_notifyWnd) {
        size_t len = line.size();
        wchar_t* copy = new wchar_t[len + 1];
        wmemcpy(copy, line.c_str(), len + 1);
        if (!PostMessageW(g_notifyWnd, g_notifyMsg, 0, reinterpret_cast<LPARAM>(copy)))
            delete[] copy;
    }
}
