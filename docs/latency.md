# Latency

Short version: it's close to playing in a normal window, and Alt+Tab is instant.

## What actually happens to a frame

1. H1Z1 draws a frame into its window.
2. Windows composes that window like any windowed game. This is where the capture grabs it.
3. StretchScaler copies it on the GPU, stretches it and presents it right away.
4. The stretched frame goes to the screen on a hardware overlay plane, so Windows doesn't compose it a second time.

On my setup (RTX 4060 Ti, 180 Hz) StretchScaler presents the stretched frame 4 to 9 ms before the refresh the
game's frame was scheduled for. So in the best case it shows up on that same refresh, and at worst it's one refresh
later (5.6 ms at 180 Hz). Compared to exclusive fullscreen, the extra cost is mostly the same one you'd pay for
any windowed game.

The status line in the app shows `output: hardware overlay (direct)` when this is working. If it says
`composed by DWM (+1 refresh)`, something is sitting on top of the game (see below) and you lose that refresh.

## Things that help

- **Close or disable overlays on the gaming monitor.** Discord overlay, the NVIDIA/Steam overlays, Xbox Game Bar
  widgets, or any other always-on-top window can knock the output back to "composed".
- **Screen recording and screenshots do it too.** Desktop capture (OBS display capture, Discord screen share,
  screenshot tools) makes Windows compose the screen while it's running. Capturing just the game window in OBS
  is fine.
- **NVIDIA Control Panel > Manage 3D settings > Program settings > H1Z1: Low Latency Mode = On or Ultra.**
  It shortens the game's own render queue, which matters more than anything StretchScaler does.
- **Keep the game's FPS high.** The capture can only grab a new frame when the game has drawn one.
- **Present mode: Lowest latency** (the default). VSync adds waiting.
- **Optional:** Windows 11 > Settings > System > Display > Graphics > *Optimizations for windowed games*. Can help
  windowed DX11 games, try it both ways.

## What StretchScaler already does

- GPU only, no copying through system RAM
- Never queues more than one frame
- Presents as soon as a captured frame arrives, without waiting on a timer
- Raises its GPU priority so its tiny copy and stretch don't wait behind a whole game frame
- Runs at above-normal CPU priority while scaling

Have a way to measure end-to-end latency (LDAT, a high speed camera, etc.)? Numbers would be really welcome in an
issue.
