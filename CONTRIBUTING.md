# Contributing to StretchScaler

Thanks for helping! This is a small community tool, so every bug report and test result helps.

## Reporting a bug

Use the **Bug report** issue template and please include:

- `StretchScaler.log` (next to the exe — it's overwritten on every start, so grab it right after the problem)
- Windows version, GPU + driver version
- Your monitor(s): resolution and refresh rate
- Your H1Z1 `[Display]` settings from `UserOptions.ini`
- What you expected vs what happened

The log contains lines like `ACTIVE | capture 179 fps | present 179 fps | present lead ...` every 30 s while
scaling — those numbers are the most useful thing for diagnosing stutter or black screens.

## Testing other setups

Results from other GPUs (AMD / Intel), refresh rates, multi-monitor layouts and other games are very welcome.
Open an issue with what worked and what didn't.

## Pull requests

1. Fork the repo and create a branch.
2. Build with `build.bat` (VS 2022 / Build Tools 2022, Desktop C++ workload).
3. Keep changes focused; match the existing style (Win32 + D3D11 + C++/WinRT, no extra dependencies).
4. **Keep it external:** no DLL injection, hooking, or reading game memory — the project's whole point is
   staying out of the game process for anti-cheat safety. PRs that touch the game process won't be merged.
5. Describe how you tested it (game, resolution, refresh rate). GitHub Actions builds every PR automatically.
