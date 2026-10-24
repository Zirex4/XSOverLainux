# XSOverLainux — XSOverlay desktop/window capture on Linux (Steam Proton)

[XSOverlay](https://store.steampowered.com/app/1173510/XSOverlay/) runs under Proton:
the UI, the keyboard, the wrist overlay and the SteamVR integration all work. What does
**not** work is the point of the app: desktop and window overlays show up transparent,
because XSOverlay's capture plugin (`uWindowCapture.dll`) uses Windows capture APIs
(PrintWindow, BitBlt, Windows Graphics Capture) that, inside Wine, can only see Wine's
own windows, never your Linux desktop.

XSOverLainux replaces that plugin with one that talks to a small native helper running
on your X11 session. Nothing in XSOverlay itself is modified; Proton is not modified.

```
XSOverlay (Unity/Mono, inside Proton)              Linux host (X11)
┌──────────────────────────────────┐   shared    ┌──────────────────────────────┐
│ Assembly-CSharp (untouched)      │   memory    │ xso-bridge                   │
│   └─ P/Invoke Uwc*() ──────────► │ /dev/shm/  │   XComposite + MIT-SHM frames │
│ uWindowCapture.dll (this repo)   │ xso-bridge │   XRandR monitors + EDID      │
│   ├─ window list / messages      │ ◄────────► │   XFixes cursor, _NET_WM_ICON │
│   ├─ UpdateSubresource → D3D11   │  frames,   │   XTest mouse/keyboard        │
│   └─ user32 hooks (SendInput…)   │  windows,  │                              │
└──────────────────────────────────┘  input     └──────────────────────────────┘
```

Spanish version of this document: [README.es.md](README.es.md).

## Status (2026-10-24)

Verified on CachyOS (Arch), xfwm4 on X11, NVIDIA RTX 2080 Ti (driver 615), Valve Index,
SteamVR 2.18.4, XSOverlay build 20263849, Proton Experimental (also loads fine under
Proton 9.0 and GE-Proton10):

* Window list with titles, classes and icons appears in XSOverlay's UI, including the
  desktop ("1 - Primary").
* Desktop and window overlays receive frames (verified by frame counters inside the real
  app and by pixel-exact readbacks in the test harness).
* Mouse and keyboard from the VR laser pointer are forwarded to the X11 desktop through
  XTest (counters verified; see limitations).
* XSOverlay stays alive with the Chromium UI running (an earlier crash, "Fatal error in
  GC", was root-caused and fixed — see [docs/INTERNALS.md](docs/INTERNALS.md#icons-are-mandatory)).

## Requirements

* X11 session (Wayland is not supported yet; see Limitations).
* Steam with XSOverlay installed, SteamVR.
* Build tools: `gcc`, X11 development headers (libX11, libXcomposite, libXext, libXfixes,
  libXrandr, libXtst), `wine` (for `winegcc` and its PE import libraries), `clang`, `lld`.
  On Arch: `sudo pacman -S base-devel libx11 libxcomposite libxext libxfixes libxrandr libxtst wine clang lld`.

## Quick start

```bash
git clone https://github.com/Zirex4/XSOverLainux.git
cd XSOverLainux
./build.sh
```

1. In Steam: XSOverlay → Properties → **Launch options**:

   ```
   /path/to/XSOverLainux/xso-linux.sh %command%
   ```

2. Properties → Compatibility → force **Proton Experimental** (Proton 9 / Hotfix / GE also work).
3. **Start SteamVR first**, then launch XSOverlay from Steam (or let SteamVR auto-start it as
   an overlay app). With Proton Experimental, XSOverlay launched while SteamVR is down gets
   OpenVR error 105, retries for ~6 s and then stays VR-less.
4. Put on the headset, create a window/desktop overlay. Done.

What the wrapper does on every launch:

* starts `build/xso-bridge` if it is not running (and stops it when XSOverlay exits);
* copies `build/uWindowCapture.dll` over the game's plugin, keeping the original as
  `uWindowCapture.dll.windows` (Steam restores the original on verify/update; the wrapper
  re-installs ours next time);
* launches `XSOverlay.exe -batchmode` directly instead of `XSOverlay Process Manager.exe`
  (that .NET launcher dies with a NullReferenceException under Wine; `XSO_DIRECT=0` keeps it).

Logs: `xso-linux.log` (wrapper), `xso-bridge.log` (helper), `/dev/shm/xso-uwc.log` (DLL),
and XSOverlay's own `output_logs/` under
`<prefix>/drive_c/users/steamuser/AppData/LocalLow/Xiexe/XSOverlay/`.

## Environment variables

| Variable | Effect |
|---|---|
| `XSO_BRIDGE_SHM` | shared-memory file (default `/dev/shm/xso-bridge`) |
| `XSO_BRIDGE_ARGS` | extra args for `xso-bridge` (default `-v`; `-vv` logs every capture, `--keep-alpha`, `--dry-input`) |
| `XSO_DIRECT=0` | keep the Process Manager launcher |
| `XSO_INPUT=0` | do not hook user32 input (no mouse/keyboard forwarding) |
| `XSO_HOOKS=SendInput,SetCursorPos` | install only the listed hooks (bisecting) |
| `XSO_UWC_DEBUG=1` | verbose DLL log with 5 s statistics (`requests/captured/input…`) |
| `XSO_UWC_TRACE=1` | ring-buffer trace of every exported call + a watchdog that dumps it if `UwcUpdate` stops |
| `XSO_NO_HELPER=1` | wrapper does not start/stop the helper |

## Repository layout

```
src/common/xso_shm.h      shared-memory protocol (both sides)          docs/PROTOCOL.md
src/helper/xso-bridge.c   native X11 helper (capture, monitors, cursor, XTest)
src/dll/uwc_linux.c       replacement uWindowCapture.dll (PE, winegcc/clang)
src/test/shmtest.c        native client: list windows, capture, timing
src/test/harness.c        PE program that fakes Unity+D3D11 to test the DLL under Proton
xso-linux.sh              Steam launch wrapper
build.sh                  builds everything into build/
tools/                    reverse-engineering + API scripts (see tools/README.md)
docs/INTERNALS.md         everything learned about XSOverlay's internals
docs/TESTING.md           how to test without a headset, trace, bisect
docs/TROUBLESHOOTING.md   symptoms → causes → fixes
docs/DEVLOG.md            chronological account of the investigation
```

## Known limitations

* **Performance.** Frames are read back through the X server (`XShmGetImage`). On the
  test machine a 2560x1440 desktop takes ~25 ms when the GPU is idle but 100–150 ms while
  VRChat and the SteamVR compositor are loading the GPU (7–10 fps for a full-desktop
  overlay). Smaller windows are proportionally faster. Ideas: server-side scaling with
  XRender before readback, or GLX `texture_from_pixmap` + PBO readback.
* **UI only when launched by Steam.** The Vuplex Chromium webview that draws XSOverlay's UI
  spawns its GPU process fine inside Steam's runtime container but not when XSOverlay is
  started by hand with `proton run` (see docs/TESTING.md for how to test anyway).
* **Alpha.** Captures are forced opaque (`--keep-alpha` keeps ARGB windows' alpha).
* **Keyboard.** Virtual-key → keysym mapping covers common keys; exotic ones may be
  missing. XSOverlay reads modifier state with `GetAsyncKeyState`, which does not reflect
  what we injected into X11.
* **Process names.** `UwcGetWindowProcessId` returns XSOverlay's own PID for every window
  (otherwise `Process.GetProcessById` throws inside XSOverlay's layout/window-list code), so
  the UI shows "XSOverlay" as the app name of every window.
* **Media manager.** `XSOverlay Media Manager.exe` (WinRT media sessions) always crashes
  under Wine; harmless.
* **X11 only.** A Wayland backend would live in the helper (PipeWire / xdg-desktop-portal).

## If SteamVR itself crashes

If the headset goes black and XSOverlay exits a few minutes after you start interacting, check
`~/.local/share/Steam/logs/vrcompositor.txt` for `ASSERT: "Vulkan transfer ring buffer overflow"`.
That is SteamVR bug [#1006](https://github.com/ValveSoftware/SteamVR-for-Linux/issues/1006), not
this project; see [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) for the camera workaround.

## Uninstall

```bash
cd ~/.local/share/Steam/steamapps/common/XSOverlay_Beta/XSOverlay_Data/Plugins/x86_64
mv uWindowCapture.dll.windows uWindowCapture.dll
```

and remove the launch option (or verify game files in Steam).

## Credits / license

MIT, see [LICENSE](LICENSE). The DLL re-implements the public C API of
[hecomi/uWindowCapture](https://github.com/hecomi/uWindowCapture) (MIT) — no code from it is
included, only its interface. Built with winegcc/clang; tested with DXVK.
