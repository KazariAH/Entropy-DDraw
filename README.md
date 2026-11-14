# Entropy-DDraw

A from-scratch DirectDraw implementation for **Command & Conquer: Yuri's Revenge** (and
Tiberian Sun-era engines). It ships as a single `ddraw.dll`: the game loads it instead of the
system library, Entropy-DDraw receives the game's DirectDraw calls and presents the finished
frame through **OpenGL 1.1**.

Nothing here is copied from another ddraw wrapper - the code was written for this project, and
the only dependencies are the Windows SDK and the system `opengl32.dll`.

## Features

- Windowed, borderless and exclusive-fullscreen modes, Alt+Enter / Alt+Tab handling
- Window position and size are remembered; DPI aware (100% - 200%)
- The game's dialog child windows (briefing, ESC menu, save / load, loading screen) stay visible
- Present sync: one upload per game frame, driven by the game's own Blt/Unlock calls
- Fast stretch blits (column table + memory copies) and an optional non-temporal copy kernel
- Crash reports with a minidump, a call chain with module-relative addresses and a short journal
  of what the DLL did in the seconds before the crash
- GDI fallback (`renderer=gdi`) and automatic renderer selection (`renderer=auto`)
- Targets Windows 7 SP1 through Windows 11, 32-bit, static CRT (`/MT`) - a single DLL, no runtime
  redistributable needed

## Build

Requirements: Windows, Visual Studio 2022 with the C++ x86 tools, PowerShell.

```
powershell -ExecutionPolicy Bypass -File build.ps1            # build ddraw.dll
powershell -ExecutionPolicy Bypass -File build.ps1 -UnitTest  # plus the window-free tests
powershell -ExecutionPolicy Bypass -File build.ps1 -Test      # plus the hidden-window self-test
```

The build prints the imported DLLs and fails if the import table contains anything other than
kernel32 / user32 / gdi32 / opengl32 / advapi32.

## Use

Drop `ddraw.dll` next to `gamemd.exe`, or let a launcher select it. Settings live in
`ddraw.ini` beside the DLL; every key is optional and the defaults are sane. A short subset:

| key | default | meaning |
|---|---|---|
| `windowed` | `true` | show exclusive-fullscreen requests in a window |
| `scale` | `aspect` | `stretch` / `aspect` / `integer` scaling |
| `vsync` | `false` | vertical sync through `wglSwapIntervalEXT` |
| `renderer` | `auto` | `opengl`, `gdi`, or `auto` |
| `noactivateapp` | `true` | keep running when the game loses focus |
| `presentsync` | `true` | upload once per game frame |
| `log` | `true` | write `ddraw.log` with counters and timings |

The full list with comments is in `ddraw.ini`.

## Tests

`tests/` contains the window-free unit tests (child-window handling, frame planning, copy
kernels), a hidden-window self-test covering the DirectDraw paths the game does not exercise
(8-bit palette, flip chains, colour keys), and small probes used during development.

## License

GPL-3.0. See `LICENSE`.

This project contains no game assets and no third-party code.