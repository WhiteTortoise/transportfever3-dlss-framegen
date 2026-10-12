# DLSS Frame Generation for Transport Fever 3

An unofficial mod that adds **NVIDIA DLSS Frame Generation** (with NVIDIA Reflex) to Transport Fever 3, which has no DLSS support of its own. After each frame the game renders, DLSS generates an extra frame. Movement looks smoother and the frame rate you see goes up.

> **Made by AI:** this mod was written by **Claude** (Anthropic's AI, working in Claude Code): the reverse-engineering of the game's renderer, all of the code, the installer and these docs. 

This is not a screen-capture tool like Lossless Scaling or Smooth Motion. The mod hooks into the game's renderer and gives DLSS the depth buffer, motion vectors and the 3D scene *before* the UI is drawn. Menus and windows stay sharp and are not warped, and city names, station signs and vehicle icons move smoothly with the camera.

**Example** (RTX 4050 Laptop GPU, 1080p, Very High): about 40 FPS without the mod, about 60 FPS with Frame Generation on.

## Requirements

- **NVIDIA GeForce RTX 40 or RTX 50 series GPU**, desktop or laptop. RTX 30 and older, AMD and Intel GPUs can't run DLSS Frame Generation.
- Windows 10 (version 2004 or newer) or Windows 11, with an up-to-date NVIDIA driver.
- **Hardware-accelerated GPU scheduling** turned on: Windows Settings > System > Display > Graphics > Change default graphics settings. Restart after changing it.
- Transport Fever 3 (Steam), with these graphics settings:
  - **Resolution Scale: 100%** (FSR off)
  - **Anti-aliasing (MSAA): on.** It was tested with 2x.

  Other combinations are not supported yet, and Frame Generation simply stays off with them.

## Install

1. Download **TF3-FrameGen-Setup.exe** from the [Releases](../../releases) page and close Transport Fever 3.
2. Run **TF3-FrameGen-Setup.exe**.
   - Windows may show "Windows protected your PC" because the installer isn't code-signed. Click **More info > Run anyway**.
3. The installer finds the game through Steam. If it doesn't, click **Browse** and pick the folder that contains `TransportFever3.exe`.
4. Click **Install**. It downloads NVIDIA's Streamline package (about 260 MB) directly from [NVIDIA's GitHub](https://github.com/NVIDIA-RTX/Streamline/releases), checks its checksum, and keeps only the 7 DLLs it needs.
5. Start the game and load a map. Frame Generation turns on by itself.

If the installer says it has no permission to write to the game folder, right-click it and choose **Run as administrator**.

## Using it

- **Ctrl + Alt + \\** (backslash) turns Frame Generation on and off while playing. You can change the key in the settings file.
- It switches itself off in menus and on loading screens.
- Check the frame rate with the **Steam FPS counter** (Steam > Settings > In Game). The NVIDIA overlay shows "N/A" in this game.
- If you enabled **NVIDIA Smooth Motion** for Transport Fever 3, turn it off. Two frame generators at once will conflict.

### Settings

The mod creates `tf3_framegen.ini` in the game folder the first time the game starts. Open it with Notepad. If Notepad can't save it, run Notepad as administrator.

```ini
start_enabled=on   ; on = Frame Generation starts on when a map loads, off = starts off
hotkey=\           ; Ctrl+Alt+<key>: a letter, a digit, a symbol such as \, or F1-F12
```

Hotkey changes take effect within a couple of seconds, even while the game is running.

## Good to know

- **VSync is turned off while the mod is active.** NVIDIA doesn't support VSync together with Frame Generation in Vulkan games. You may see some screen tearing.
- **The real frame rate goes down a little.** Generating frames costs GPU time; for example, 40 rendered frames can become 30 rendered + 30 generated.
- **Higher input latency:** your input still responds at the real frame rate. NVIDIA Reflex is on to keep latency down. In a city builder this is barely noticeable.
- **On-screen frame rate is limited by your monitor's refresh rate.** If the game runs your screen at 60 Hz, you get at most 60 FPS on screen.
- **Moving vehicles:** the mod's motion vectors only describe camera movement. DLSS's optical flow handles moving trains, and no smearing showed up in testing, but very fast objects could show minor artifacts.
- **Station building and editing** are slowed down by the game's own CPU-heavy construction code. Frame Generation can't help there.
- **Game updates** can change how the game renders and stop the mod working until it's updated. If that happens, uninstall it.

## Changelog

- **0.2.0:** City names, station signs and vehicle icons now move smoothly with the camera in generated frames instead of jittering while panning.
- **0.1.0:** First release.

## Uninstall

Run **TF3-FrameGen-Setup.exe** and click **Uninstall**. It removes only the mod's files: `winhttp.dll`, the `framegen` folder, `tf3_framegen.ini` and `tf3_framegen.log`. The game runs exactly as before.

To switch the mod off temporarily without uninstalling:
- create an empty file named `tf3_framegen.nofg` in the game folder to turn off only Frame Generation, or
- create `tf3_framegen.disable` to turn off the whole mod.

Delete the file to switch it back on.

## Troubleshooting

| Problem | What to try |
|---|---|
| Game crashes at startup | Create `tf3_framegen.disable` in the game folder to play without the mod. Then report the problem and include the log files (see below). |
| No FPS increase | Check the requirements: RTX 40/50, **Resolution Scale exactly 100%** (any lower value turns on FSR, and Frame Generation then stays off), MSAA on, hardware GPU scheduling on. The game window must be focused, because Frame Generation pauses when it isn't. If you aren't sure, `tf3_framegen.log` says "frame generation inputs found" when the mod detects a map; if that line is missing, a graphics setting is the cause. |
| Hotkey does nothing | Make sure you edited `tf3_framegen.ini` in the game folder (next to `TransportFever3.exe`) and that it was saved. |
| Installer can't find the game | Use **Browse** and select the folder containing `TransportFever3.exe`. |

The mod writes two log files to the game folder, which are useful for bug reports:
- `tf3_framegen.log`, from the mod
- `framegen\sl.log`, from NVIDIA Streamline

## How it works (short version)

`winhttp.dll` is loaded by the game from its own folder. It passes all real web calls through to Windows, and hooks the game's Vulkan renderer:

1. It finds the depth buffer, the camera matrices and the 3D scene before the UI each frame.
2. It calculates camera motion vectors on the GPU.
3. It hands everything to NVIDIA Streamline, which runs DLSS Frame Generation and Reflex.

## Building from source

You need Windows, [Visual Studio Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/) with the "Desktop development with C++" workload (includes CMake, Ninja and MASM), the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home), and Python 3 only if you regenerate the winhttp proxy stubs.

```powershell
.\get_dependencies.ps1      # downloads NVIDIA Streamline 2.14.1 into external\ (checksum pinned)
.\build.ps1                 # builds build\winhttp.dll and the test programs
.\build.ps1 -Install        # copies the mod into the game folder (when this folder is inside it)
.\build_installer.ps1       # builds dist\TF3-FrameGen-Setup.exe
```

Tests (run from `build\`):

| Program | What it checks |
|---|---|
| `smoke_test.exe` | The winhttp pass-through and Vulkan interception load correctly. |
| `frame_test.exe` | Renders game-like frames (MSAA, depth resolve, camera block) under the Khronos validation layer, with a ray-traced ground-truth scene for the motion vectors. Exit code = number of validation errors. Streamline's own internal validation messages are expected. |
| `sdl_test.exe` | Creates the window surface through the game's SDL2.dll, as the game does. Needs this folder inside the game folder. |

Project layout:

| Path | Contents |
|---|---|
| `src/main.cpp` | winhttp proxy entry point, `GetProcAddress` import hooks (game exe and SDL2.dll), settings file, logging |
| `src/vk_hooks.cpp` | Vulkan interception: object tracking, command recording, per-frame detection of depth / UI-free image / camera, profiler, frame capture |
| `src/framegen.cpp` | The mod's own GPU work at present: motion vectors, optional self-check and debug view |
| `src/streamline.cpp` | NVIDIA Streamline integration (manual hooking): DLSS Frame Generation, Reflex markers, camera constants, tags |
| `src/shaders/` | Compute shaders (motion vectors, self-check, debug view) |
| `src/proxy/` | Generated winhttp export forwarding (`tools/gen_winhttp_proxy.py`) |
| `installer/` | The one-click installer (C#, .NET Framework 4) |
| `tests/` | Test programs |
| `AGENT_SUMMARY.md` | Technical write-up of the game's renderer and every integration pitfall, useful as context for a coding agent |

Debugging hotkeys in game: **Ctrl+Alt+M** shows the motion vectors in the top-right corner, and **Ctrl+Alt+C** writes a full frame capture to `tf3_framegen.log`.

## License

The mod's source code is released under the [MIT License](LICENSE). NVIDIA Streamline and the DLSS / Reflex runtime files are not part of this repository and are covered by NVIDIA's own licenses.

## Credits and legal

- Uses **NVIDIA DLSS Frame Generation** and **NVIDIA Reflex** through the NVIDIA Streamline SDK. NVIDIA, GeForce RTX, DLSS and Reflex are trademarks of NVIDIA Corporation. NVIDIA's runtime files are not included in this download; the installer gets them from NVIDIA's official Streamline release, and they are covered by NVIDIA's license.
- Unofficial mod. Not affiliated with or endorsed by Urban Games, the game's publisher, or NVIDIA. Use at your own risk.
- Written by Claude (Anthropic's AI) in Claude Code. Requested, tested and published by WhiteTortoise.
