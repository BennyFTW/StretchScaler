#pragma once
#include <windows.h>
#include <memory>
#include <cstdint>

struct ScalerOptions {
    bool pointFilter = false;  // false = bilinear
    bool vsync = false;        // false = Present(0) (+ALLOW_TEARING when supported), true = Present(1)
    bool stretch = true;       // true = stretch to fill (ignore aspect), false = fit (letterbox)
    bool drawCursor = true;    // draw the (hidden) system cursor ourselves at the stretched position
    bool hideBorder = true;    // ask WGC not to draw the yellow capture border
    bool composedOutput = false; // true: let Windows compose the output (no hardware overlay/MPO, no GPU priority
                                 // boost). +1 refresh latency, but avoids MPO stutter and keeps driver color settings.
    bool fpsOverlay = false;   // draw an FPS counter into the output
    int fpsCorner = 0;         // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
};

struct ScalerStats {
    uint32_t captured = 0;     // capture frames received since last call
    uint32_t presented = 0;    // presents since last call
    double avgLeadMs = 0;      // avg (source frame DWM display time - our Present time); positive = ahead
    int presentMode = -1;      // DXGI_FRAME_PRESENTATION_MODE of our output: 0 composed, 1 overlay, 2 none/direct,
                               // 3 composition failure, -1 unknown
};

// Captures `source` with Windows.Graphics.Capture, crops its client area, and presents it
// stretched into a topmost, click-through, non-activating borderless window covering `monitorRect`.
// Start/Stop/SetVisible must be called on the UI thread (the overlay window lives there).
class Scaler {
public:
    Scaler();
    ~Scaler();
    bool Start(HWND source, HMONITOR monitor, const RECT& monitorRect, const ScalerOptions& opt,
               HWND notifyWnd, UINT stoppedMsg /* posted if the capture item closes or device is lost */);
    void Stop();
    bool Running() const;
    void SetVisible(bool visible);
    HWND OverlayHwnd() const;
    ScalerStats TakeStats();
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
