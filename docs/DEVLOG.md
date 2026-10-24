# Development log — 2026-10-24

A chronological account of how this was built in one session, so the reasoning can be
checked and the approach reused for other Wine/Proton apps.

1. **Why XSOverlay fails on Linux.** Public reports (Proton issue #3741, XSOverlay issue
   tracker #198, Steam forums) and the user's own September sessions agreed: UI works,
   captures are transparent. The install contains `uWindowCapture.dll` (hecomi's plugin:
   PrintWindow/BitBlt/Windows Graphics Capture) and `uTouchInjection.dll`. In Wine those APIs
   only see Wine's own windows.
2. **Choosing where to patch.** Options: implement Windows Graphics Capture in Wine (huge,
   Proton-coupled), a BepInEx/Harmony patch of the C# (fragile across updates), or replace
   the native plugin keeping its C API (zero Wine coupling, survives Proton updates). The
   last one was chosen; frames travel through a shared file so the X11 work can run as a
   normal Linux process outside the Steam container (which shares `/dev/shm`, the X socket
   and `$XDG_RUNTIME_DIR`).
3. **Recovering the exact API.** `winedump -j export` on the shipped DLL gave 83 exports;
   `dnfile` dumped the P/Invoke signatures from `Assembly-CSharp.dll` (`tools/dump_pinvoke.py`),
   including XSOverlay's additions (`UwcGetMonitorEdid` returning a 388-byte Unicode struct,
   `UwcGetDesktopUid`, `UwcIsPrimaryDesktop`). `dncil` IL dumps showed the texture format
   (BGRA32), the message struct and the layout file schema.
4. **Helper.** `xso-bridge`: `_NET_CLIENT_LIST_STACKING`, EWMH properties, RandR + EDID,
   XComposite `NameWindowPixmap` + `XShmGetImage`, root capture for monitors, XFixes cursor,
   `_NET_WM_ICON`, XTest. Benchmarks on the RTX 2080 Ti: 25 ms per 1440p readback idle,
   100–150 ms under VR load.
5. **DLL.** Plain C with winegcc/clang (no C++ runtime available for PE); the Unity native
   plugin interface re-declared in C; D3D11 via Wine's COM C macros; one `CRITICAL_SECTION`
   for the message list; `UpdateSubresource` from the render callback.
6. **Harness.** A PE program faking Unity proved the whole path under Proton 9 + DXVK with
   pixel-exact readbacks before touching XSOverlay.
7. **Real app, first contact.** Through Steam the Process Manager crashed (NullReference
   after a message box); `XSOverlay.exe -batchmode` works. The plugin loaded, listed 15
   windows, and after a WebSocket `Create` captured 700+ desktop frames in a minute.
8. **UI.** Launched by hand the Chromium GPU process never started; launched through
   Steam's runtime container it does — so the wrapper keeps Steam's own command line.
9. **Input.** user32 `SendInput`/`SetCursorPos`/`SetForegroundWindow`/`ShowWindow` hooked
   in-process and translated to XTest; verified with a dry-run helper.
10. **The crash.** With the UI alive XSOverlay froze after ~5 s and died minutes later with
    "Fatal error in GC". Bisection: hooks on/off made no difference, the original DLL did
    not freeze. A ring-buffer trace + watchdog showed the main thread looping over
    `IsDesktop/RequestUpdateWindowTitle/GetWindowTitle` for the same 7 windows forever:
    `UwcWindowList.UpdateWindowTitles` recursing through `EventWindowListUpdated` →
    `OnRequestWindowList`. It only terminates when a desktop entry is in the list, and
    entries only exist after `IconCaptured`. Our desktop had no icon. Fix: synthesized icons
    for desktops and icon-less windows; desktops uploaded first.
11. **Result.** From Steam, with SteamVR running: UI alive, API answering, 300+ frames
    captured and ~1000 input events forwarded during the user's VR session.
12. **Usability pass (04:30–04:45).** XSOverlay's virtual keyboard sends VKs with scancodes
    (WindowsInput `KeyDown/KeyUp`, never Unicode), so the helper now injects by scancode →
    X keycode, matching the labels XSOverlay computes through Wine from the same X layout.
    Capture became incremental: XDamage regions per window/root, multi-rectangle `XShmGetImage`,
    dirty-rect `UpdateSubresource` in the DLL with an ack handshake (protocol v2). A Claude
    (Electron) window went from 3 Mpx to ~0.1 Mpx read per frame; static windows cost nothing.
    Optional XRender downscale (`--scale`) for full-screen games. Verified with `shmtest`,
    the Proton harness (pixel-correct after 60 incremental frames) and live in VR.
