# Testing without (and with) a headset

## Build

`./build.sh` produces `build/xso-bridge`, `build/uWindowCapture.dll`, `build/shmtest`,
`build/harness.exe`.

## 1. Native side only (no Wine)

```bash
./build/xso-bridge -vv &          # -vv logs every capture with timings
./build/shmtest desktop /tmp/desk.ppm      # capture the primary desktop, 60 timed captures, cursor
./build/shmtest Firefox /tmp/win.ppm       # first window whose title contains "Firefox"
python3 tools/ppm2png.py /tmp/win.ppm /tmp/win.png
```

`shmtest` with no argument just dumps the header, monitors (with EDID) and the window table.

## 2. The DLL under Proton, no SteamVR, no XSOverlay

`harness.exe` creates a D3D11 device (DXVK), fakes `IUnityInterfaces`/`IUnityGraphicsD3D11`,
loads `uWindowCapture.dll` the way Unity does and drives the API: lists windows, requests
captures of a target, creates the BGRA32 texture on `WindowSizeChanged`, calls the render
event function, reads the texture back through a staging texture and writes a PPM.

```bash
mkdir -p /tmp/xsotest && cp build/uWindowCapture.dll build/harness.exe /tmp/xsotest/
export STEAM_COMPAT_DATA_PATH=/tmp/xsotest/compatdata STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam
mkdir -p "$STEAM_COMPAT_DATA_PATH"
"$HOME/.local/share/Steam/steamapps/common/Proton 9.0 (Beta)/proton" run /tmp/xsotest/harness.exe Firefox "Z:/tmp/xsotest/out.ppm" 30
```

With `XSO_TEST_INPUT=1` the harness also calls `SetCursorPos`/`SendInput` through the hooked
user32; run a second helper with `XSO_BRIDGE_SHM=/dev/shm/xso-test ./build/xso-bridge --dry-input -v`
(and the same `XSO_BRIDGE_SHM` for the harness) to see the decoded events logged instead of
injected.

## 3. The real XSOverlay, headless

Start SteamVR first (or set `VR_OVERRIDE=/nonexistent` to keep it off; then the WebSocket API
will not answer because XSOverlay's main loop is busy retrying VR, but plugin loading, the
window list and the Chromium UI can still be checked in the logs).

Launch exactly like Steam does (inside the Steam Linux Runtime container — required for the
Chromium UI to start):

```bash
G=~/.local/share/Steam/steamapps/common/XSOverlay_Beta
SLR=~/.local/share/Steam/steamapps/common/SteamLinuxRuntime_4
PX="$HOME/.local/share/Steam/steamapps/common/Proton - Experimental"
cd "$G" && env STEAM_COMPAT_DATA_PATH=~/.local/share/Steam/steamapps/compatdata/1173510 \
  STEAM_COMPAT_CLIENT_INSTALL_PATH=~/.local/share/Steam STEAM_COMPAT_INSTALL_PATH="$G" \
  STEAM_COMPAT_LIBRARY_PATHS=~/.local/share/Steam STEAM_COMPAT_TOOL_PATHS="$PX:$SLR" \
  STEAM_COMPAT_APP_ID=1173510 SteamAppId=1173510 SteamGameId=1173510 XSO_UWC_DEBUG=1 \
  "$SLR/_v2-entry-point" --verb=waitforexitandrun -- "$PX/proton" waitforexitandrun "$G/XSOverlay.exe" -batchmode
```

Then drive it over the API (needs `pip install websockets` in a venv):

```bash
python3 tools/xsoapi.py RequestWindowList        # window list JSON (icons as base64 PNG)
python3 tools/xsoapi.py Create                   # spawn an overlay window → desktop capture starts
python3 tools/xsoapi.py LoadLayout=0
```

and watch `/dev/shm/xso-uwc.log` (`XSO_UWC_DEBUG=1` prints a `stats:` line every 5 s with
`requests`, `captured`, `nulltex`, `sizeerr`, `sizechanged`, `input`, `msgs`).

To stop it cleanly: `WINEPREFIX=<compatdata>/pfx "$PX/files/bin/wineserver" -k`.

## 4. Tracing a stall

`XSO_UWC_TRACE=1` records every exported call in an in-memory ring (8192 entries, µs
timestamps, thread id) and starts a watchdog thread that dumps the last 400 entries to the
DLL log when `UwcUpdate` has not been called for 3 s, plus a `watchdog:` line every 10 s with
update/render counts. This is how the window-list recursion was found (the last entries were
`IsDesktop → RequestUpdateWindowTitle → GetWindowTitleLength → GetWindowTitle` cycling over the
same ids).

`XSO_HOOKS=SendInput,SetCursorPos` installs only the named user32 hooks; `XSO_INPUT=0` none.

## 5. Benchmarks

`tools/bench.c` measures `XShmGetImage` on the root window and on a composite pixmap:
`gcc -O2 -o bench tools/bench.c -lX11 -lXext -lXcomposite && ./bench 0x4c00004`.
