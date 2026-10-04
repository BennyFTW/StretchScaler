<p align="center">
  <img src="docs/logo.png" width="128" alt="StretchScaler logo">
</p>

<h1 align="center">StretchScaler</h1>

<p align="center">
  <b>Stretched resolution for H1Z1 / Z1 Battle Royale with instant Alt+Tab.</b><br>
  Free, open source, no injection — a tiny external window scaler.
</p>

<p align="center">
  <a href="../../releases/latest"><b>⬇ Download the latest release</b></a> ·
  <a href="#quick-start">Quick start</a> ·
  <a href="#required-h1z1-settings">H1Z1 settings</a> ·
  <a href="#troubleshooting">Troubleshooting</a>
</p>

---

## The problem

Classic stretched res (e.g. 1440×1080 stretched to 1920×1080, or 1920×1440 stretched to 2560×1440) normally
means running H1Z1 in **exclusive fullscreen** with GPU scaling. That works, but:

- Alt+Tab takes seconds, flashes black, and often bounces you straight back into the game.
- CRU / custom resolutions / GPU scaling don't fix the Alt+Tab delay.
- **Windowed fullscreen** Alt+Tabs instantly, but the game renders 16:9 — no stretch.
- **Borderless Gaming**-style tools resize the game window, so H1Z1 re-renders at 16:9 — still no stretch.

## What StretchScaler does

H1Z1 stays a normal **4:3 window** (e.g. 1920×1440). StretchScaler captures that window on the GPU and shows
it **stretched to fill your whole monitor** in a separate borderless window on top. The game window is never
resized, so the game keeps rendering 4:3 → you get the stretch. Your desktop resolution never changes →
Alt+Tab is instant.

```
H1Z1 window 1920×1440 (4:3)  →  GPU capture  →  stretch  →  2560×1440 borderless output (16:9)
```

- ✅ Real stretched 4:3 image, full screen
- ✅ Instant Alt+Tab, no black screen, no resolution switching
- ✅ Second monitor untouched
- ✅ No frame generation, no upscaling filters — plain bilinear or point stretch
- ✅ Runs in the tray, starts scaling automatically when H1Z1 opens
- ✅ Optional in-game FPS counter (real game FPS)

---

## Quick start

1. **Set H1Z1 to windowed 4:3** — see [Required H1Z1 settings](#required-h1z1-settings).
2. **Download** `StretchScaler.zip` from [Releases](../../releases/latest), extract it anywhere
   (e.g. `C:\Games\StretchScaler`).
3. Run **`StretchScaler.exe`** and accept the admin prompt (needed for the real FPS counter).
4. Check **Output monitor** is your gaming monitor (e.g. `2560×1440 @ 180 Hz`). It's saved for next time.
5. Launch H1Z1. After ~3 seconds it's stretched automatically. That's it.

Close the StretchScaler window and it keeps running in the **system tray**. Open it once per Windows session;
it handles every game launch after that.

| Hotkey | Action |
|---|---|
| `Ctrl+Alt+S` | Toggle scaling on/off |
| `Ctrl+Alt+X` | Panic stop (stops scaling, restores cursor) |

---

## Required H1Z1 settings

H1Z1 must run in **Windowed** mode (not Fullscreen, not Windowed Fullscreen) at a **4:3 size** that matches
your monitor's height.

Edit `UserOptions.ini` in your H1Z1 install folder (Steam → right-click the game → *Manage* →
*Browse local files*) **while the game is closed**:

```ini
[Display]
Maximized=0
Mode=Windowed
FullscreenMode=Windowed
WindowedWidth=1920
WindowedHeight=1440
HDPixelPlus=1.000000
```

Pick `WindowedWidth` / `WindowedHeight` for your monitor:

| Your monitor | H1Z1 window (4:3) | Result |
|---|---|---|
| 1920×1080 | **1440×1080** | classic 1440×1080 stretched |
| 2560×1440 | **1920×1440** | same look as 1440×1080 on a 1080p screen |
| 3840×2160 | **2880×2160** | same look on 4K |

Other aspect ratios (e.g. 1280×1024 5:4) work too — whatever size the window is gets stretched to fill.

> The window will look small/odd on your desktop before StretchScaler kicks in. That's expected — don't
> maximize or resize it.

---

## Required PC settings

- **Keep Windows at your monitor's native resolution and refresh rate** (e.g. 2560×1440 @ 180 Hz).
  No custom resolutions, CRU or GPU scaling needed — undo those if you set them up for stretched res.
- **Display scaling (Settings → System → Display → Scale):** 100% recommended on the gaming monitor.
  If you use another value, set `H1Z1.exe` → *Properties* → *Compatibility* → *Change high DPI settings* →
  ✅ *Override high DPI scaling behavior* → *Application*. Otherwise Windows blurs/enlarges the game window.
  StretchScaler shows the detected size — it should read exactly your 4:3 size, e.g. `client 1920x1440 (4:3 OK)`.
- **Windows 10 2004+ or Windows 11**, any DirectX 11 GPU.
- *Optional:* Windows 11 → Settings → System → Display → Graphics → *Optimizations for windowed games* → On.
  It can lower latency for windowed DX11 games. Try it on/off and keep what feels better.

---

## Options (in the app)

| Option | What it does |
|---|---|
| Output monitor | Which monitor the stretched image covers (shown as resolution + refresh + name). |
| Scaling | **Stretch to fill** (the point of this tool) or Fit (keep aspect, black bars). |
| Filter | **Bilinear** (smooth) or **Point** (sharp, nearest pixel). |
| Present mode | **Lowest latency** (no VSync, 1-frame queue) or VSync. |
| Align source window | Moves (never resizes) H1Z1 so it sits exactly behind the output. |
| Hide real cursor & draw stretched cursor | Menus line up: the cursor you see is where the game thinks it is. |
| Confine cursor to game | Keeps the cursor inside the game while it's focused. |
| Show FPS overlay | Real game FPS + frame time in a corner of the screen (needs admin, which is the default). |
| Minimize game on focus loss | Alt+Tab minimizes H1Z1 so you get a clean desktop; click it to come back. |
| Start scaling automatically | Starts when the game window appears, stops when it closes. |
| Auto-detect match | Text to find the game window by (default `H1Z1`). Works for other games too. |

Settings are saved in `StretchScaler.ini` next to the exe.

---

## Is it safe with BattlEye?

StretchScaler is **purely external**:

- no DLL injection, no hooks, no reading or writing game memory, no process handle opened to the game;
- it captures the window with the same Windows API that OBS, Discord and the Snipping Tool use
  (`Windows.Graphics.Capture`), and reads FPS from Windows' own present events (like PresentMon / FrameView);
- the only thing it does to the game window is move it (position only) and minimize it on Alt+Tab, if enabled.

**However, nobody but BattlEye can guarantee how BattlEye treats any third-party tool. Use at your own risk.**

---

## Latency — honest numbers

Any capture-based scaler (this one, Magpie, Lossless Scaling) goes through the Windows compositor twice, so
expect roughly **one extra refresh** of delay vs exclusive fullscreen (~5.6 ms at 180 Hz). StretchScaler keeps
it minimal: GPU-only copy, 1-frame queue, no VSync by default, and it presents each frame as soon as Windows
has it. The trade is instant Alt+Tab.

---

## Troubleshooting

- **Nothing happens when the game opens** — open StretchScaler from the tray and check the *Source window* line.
  Is it `client 1920x1440 (4:3 OK)`? If the size equals your monitor's shape (16:9), the game isn't in
  Windowed 4:3 mode, so there's nothing to stretch.
- **Black screen / not updating** — check the log (`StretchScaler.log` next to the exe, also shown in the app)
  for capture errors. Don't minimize H1Z1 manually while scaling.
- **FPS overlay says "(on-screen)"** — StretchScaler isn't running as admin, so it can only show how many frames
  reach the screen (max = your refresh rate).
- **Cursor stuck hidden or trapped** — `Ctrl+Alt+X`, or exit from the tray icon. The cursor is always restored.
- **Wrong monitor** — pick the right one under *Output monitor*.
- **A few dark pixels in the bottom corners** — Windows 11 rounds the game window's corners. Cosmetic.

Still stuck? [Open an issue](../../issues/new/choose) and attach your `StretchScaler.log`.

---

## How it works (technical)

- Capture: `Windows.Graphics.Capture` item created from the game HWND (`IGraphicsCaptureItemInterop::CreateForWindow`),
  free-threaded frame pool, cursor capture and yellow border disabled.
- The client area is cropped with `CopySubresourceRegion` into our own texture — GPU only, no CPU readback.
- A fullscreen-triangle shader samples it (bilinear or point) into a D3D11 `FLIP_DISCARD` swap chain,
  max frame latency 1, waitable, `Present(0)` + `ALLOW_TEARING` when supported.
- The output window is `WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP`:
  it never takes focus and all input falls through to the game.
- A `EVENT_SYSTEM_FOREGROUND` WinEvent hook (out-of-context) shows the output only while the game is focused.
- Menu cursor: real cursor hidden, confined to the game's client area, and drawn at the stretched position.
- Real FPS: ETW real-time session on Microsoft-Windows-DXGI / D3D9 `Present_Start` filtered to the game's PID.

Command line: `/background` (start in tray), `/autostart` (start scaling now), `/exitafter:N` (testing).

## Building from source

Needs **Visual Studio 2022** (or Build Tools 2022) with *Desktop development with C++* and a Windows 10/11 SDK.
No NuGet packages — C++/WinRT ships with the Windows SDK.

- Double-click **`build.bat`** → `bin\StretchScaler.exe`, or
- open the folder in Visual Studio (CMake).

Every push to `main` is also built by GitHub Actions; version tags (`v1.2.3`) publish a Release automatically.

```
StretchScaler/
├── src/
│   ├── main.cpp          UI, tray, settings, hotkeys, focus/cursor handling, auto-start
│   ├── Scaler.h/.cpp     capture, crop, stretch shader, output window, swap chain, FPS overlay
│   ├── FpsCounter.h/.cpp real game FPS from ETW present events
│   ├── Enum.h/.cpp       game window + monitor detection
│   ├── Log.h/.cpp        logging to StretchScaler.log and the UI
│   ├── app.ico / app.rc  icon
│   └── app.manifest      admin, PerMonitorV2 DPI, common controls
├── tools/make_icon.py    regenerates the icon (Python + Pillow)
├── build.bat             MSVC build, no CMake needed
├── publish.bat           maintainer: commit + push (+ optional release tag)
└── CMakeLists.txt
```

## Contributing

Bug reports, testing on other setups (other GPUs, refresh rates, games) and pull requests are very welcome —
see [CONTRIBUTING.md](CONTRIBUTING.md).

Also worth knowing: [Magpie](https://github.com/Blinue/Magpie) is a great general-purpose open-source window
scaler using the same capture technique.

## License

[MIT](LICENSE)
