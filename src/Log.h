#pragma once
#include <windows.h>

// Thread-safe logging. Every line is timestamped, appended to StretchScaler.log next to the exe
// (truncated at LogInit), written to OutputDebugString, and - if a notify window is set -
// posted to it as PostMessageW(notifyWnd, notifyMsg, 0, (LPARAM)new wchar_t[]) .
// The receiver must delete[] the LPARAM string.
void LogInit(HWND notifyWnd, UINT notifyMsg);
void LogShutdown();
void Log(const wchar_t* fmt, ...);
