# XSOverlay internals (as relevant to running it on Linux)

Everything below was obtained from the Steam build 20263849 (depot 1173512) of XSOverlay
(app id 1173510) on 2026-10-24, by reading its managed assembly with
[dnfile](https://github.com/malwarefrank/dnfile)/[dncil](https://github.com/mandiant/dncil)
(scripts in `tools/`), disassembling the shipped native plugins, and watching the app run
under Proton. XSOverlay is a Unity 2022.3.62f2 **Mono** build (`XSOverlay_Data/Managed/`),
so everything is in `Assembly-CSharp.dll`.

## Launch configuration

Steam has two launch options:

| Steam launch | Executable | Notes |
|---|---|---|
| default ("XSOverlay") | `Launcher/XSOverlay Process Manager.exe` | .NET Framework WinForms tray launcher |
| "Old Launch Method (DEPRECATED)" | `XSOverlay.exe -batchmode` | what the Process Manager itself runs |

`XSOApplicationContext.StartXSOverlay()` checks `File.Exists(<launcher dir>/../XSOverlay.exe)`
and `IsReparsePoint(...)`; on failure it calls `ShowMessageBox()` → `MessageBox.Show` →
`CleanupXSOverlayProcesses()` → `trayIcon.Visible = false` with `trayIcon == null` →
**NullReferenceException** (unhandled, the process exits). Under Wine this path is taken
every time, so the wrapper launches `XSOverlay.exe -batchmode` directly. Nothing of value
is lost: the tray icon and the Windows Task Scheduler autostart are useless on Linux.

`XSOverlay.exe` also spawns `XSOverlay Media Manager.exe` (uses WinRT
`Windows.Media.Control`), which dies with a `TypeLoadException` under Wine — harmless.

## Native plugins

`XSOverlay_Data/Plugins/x86_64/`:

* `uWindowCapture.dll` — XSOverlay's fork of hecomi's uWindowCapture (C++/WinRT 2.0.220110.5,
  capture modes PrintWindow / BitBlt / Windows Graphics Capture). **Replaced by this project.**
* `uTouchInjection.dll` — hecomi's uTouchInjection (`Initialize, Touch, Hover, SetPosition,
  SetPressure, SetAreaSize, SetOrientation, Update, Release, Finalize, Set*Func, *LogOutput`).
  Windows `InjectTouchInput`; unimplemented in Wine, not replaced (XSOverlay falls back to mouse).
* `VuplexWebViewWindows.dll` + `VuplexWebViewChromium/` — Vuplex 3D WebView (CEF). Draws the
  whole UI. Remote debugging is enabled on `http://localhost:9000`. Its GPU subprocess only
  starts when XSOverlay runs inside Steam's runtime container (pressure-vessel).
* `openvr_api.dll`, `steam_api64.dll`, `NativeNamedPipe.dll` (Discord RPC), Backtrace crashpad.

### uWindowCapture.dll exports used by XSOverlay (P/Invoke signatures)

All `__stdcall` (irrelevant on x64). `bool` returns are marshalled as 4-byte BOOL, so the
replacement returns `int`.

```
void  UwcInitialize();  void UwcFinalize();  void UwcUpdate(float dt);
void  UwcSetDebugMode(int);  void UwcSetLogFunc(fn);  void UwcSetErrorFunc(fn);   // fn: void(const char*)
IntPtr UwcGetRenderEventFunc();                 // used with GL.IssuePluginEvent(fn, 0)
int   UwcGetMessageCount();  Message* UwcGetMessages();  void UwcClearMessages();  void UwcExcludeRemovedWindowEvents();
bool  UwcCheckWindowExistence(int id);
HWND  UwcGetWindowHandle(id); HWND UwcGetWindowOwnerHandle(id); HWND UwcGetWindowParentHandle(id); HINSTANCE UwcGetWindowInstance(id);
int   UwcGetWindowParentId(id); int UwcGetWindowProcessId(id); int UwcGetWindowThreadId(id);
int   UwcGetWindowX/Y/Width/Height/ZOrder(id);
int   UwcGetWindowTextureWidth/Height/OffsetX/OffsetY(id);  int UwcGetWindowIconWidth/Height(id);
int   UwcGetWindowTitleLength(id);  const WCHAR* UwcGetWindowTitle(id);
int   UwcGetWindowClassNameLength(id);  const char* UwcGetWindowClassName(id);
void  UwcRequestUpdateWindowTitle(id);  void UwcRequestCaptureWindow(id, CapturePriority);  void UwcRequestCaptureIcon(id);
IntPtr UwcGetWindowTexturePtr(id);  void UwcSetWindowTexturePtr(id, IntPtr);  // ID3D11Texture2D* from Texture2D.GetNativeTexturePtr()
IntPtr UwcGetWindowIconTexturePtr(id);  void UwcSetWindowIconTexturePtr(id, IntPtr);
CaptureMode UwcGetWindowCaptureMode(id);  void UwcSetWindowCaptureMode(id, CaptureMode);
bool  UwcGetWindowCursorDraw(id);  void UwcSetWindowCursorDraw(id, bool);
IntPtr UwcGetWindowBuffer(id);  Color32 UwcGetWindowPixel(id, x, y);  bool UwcGetWindowPixels(id, IntPtr out, x, y, w, h);
bool  UwcIsWindow/IsAltTabWindow/IsDesktop/IsPrimaryDesktop/IsWindowVisible/IsWindowEnabled/IsWindowUnicode/
      IsWindowZoomed/IsWindowIconic/IsWindowHungUp/IsWindowTouchable/IsWindowApplicationFrameWindow/IsWindowUWP/IsWindowBackground(id);
bool  UwcIsWindowsGraphicsCaptureAvailable(id);  bool UwcIsWindowsGraphicsCaptureSupported();  bool UwcIsWindowsGraphicsCaptureCursorCaptureEnabledApiSupported();
uint  UwcGetDesktopUid(id);  MonitorEdidInfo UwcGetMonitorEdid(id);          // XSOverlay additions
Point UwcGetCursorPosition();  int UwcGetWindowIdFromPoint(x, y);  int UwcGetWindowIdUnderCursor();
void  UwcRequestCaptureCursor();  int UwcGetCursorX/Y/Width/Height();  void UwcSetCursorTexturePtr(IntPtr);
int   UwcGetScreenX/Y/Width/Height();
```
Also declared in C# but absent from the shipped DLL: `StartCaptureWindow`, `StopCaptureWindow`.

Structures (C# `StructLayout(Sequential)`):

```c
struct Message { int type; int windowId; void* userData; };            // 16 bytes
enum MessageType { WindowAdded=0, WindowRemoved=1, WindowCaptured=2, WindowSizeChanged=3,
                   IconCaptured=4, CursorCaptured=5, Error=1000, TextureNullError=1001, TextureSizeError=1002 };
enum CaptureMode { None=-1, PrintWindow=0, BitBlt=1, WindowsGraphicsCapture=2, Auto=3 };
struct Point { int x, y; };
struct MonitorEdidInfo {           // CharSet.Unicode, returned by value (hidden pointer, 388 bytes)
    uint16_t mfgId; uint16_t productCode;
    WCHAR devicePath[128];         // ByValTStr 128  — stored in layouts to re-identify monitors
    WCHAR friendlyName[64];        // ByValTStr 64
};
```

Semantics that matter:

* Textures are created by C# as `Texture2D(w, h, TextureFormat.BGRA32 (=14), mipChain:false)`
  with `w,h = UwcGetWindowTextureWidth/Height` after a `WindowSizeChanged` message; the
  plugin fills them from the render thread (`GL.IssuePluginEvent`). Under Unity's batch mode
  that "render thread" is the main thread.
* `UwcGetWindowPixels` must return rows bottom-up and RGBA (the original flips rows and swaps
  R/B); `UwcGetWindowPixel` packs that as `Color32`.
* Z-order: 0 = top-most (count of visible windows above).
* Desktop pseudo-windows: `IsDesktop`, `IsPrimaryDesktop`, `GetDesktopUid`, `GetMonitorEdid`;
  the original titles them `\\.\DISPLAYn`. XSOverlay's UI shows "1 - Primary" style names.
* `UwcSetDebugMode(2)` is called at startup.
* `UwcWindow.processId` is passed to `Process.GetProcessById()` in `LayoutHandler.SaveLayout`
  and `UwcWindowList.SetupWindowListItem` without try/catch, so it must be a PID Mono can
  resolve inside the Wine process list — we return XSOverlay's own PID.

### Icons are mandatory

`UwcWindowList.OnWindowAdded(window)` only inserts a window into `CurrentWindowList` once
`window.hasIconTexture` is true, which happens after an `IconCaptured` message
(`RequestCaptureIcon` → plugin → `IconCaptured` → `UpdateWindowEntry`). Desktops go through
the same path.

`UwcWindowList.UpdateWindowTitles()` iterates `CurrentWindowList`, returns early as soon as
it meets a desktop entry, and otherwise ends with `XSOEventSystem.EventWindowListUpdated()`.
`ApiHandler` subscribes to that event and, for every UI client subscribed to `WindowList`,
calls `OnRequestWindowList` → `UwcWindowList.UpdateWindowTitles()` again. **If no desktop
entry exists the recursion never terminates**: stack overflow in the main thread, XSOverlay
stops updating (API dead, overlays frozen) and Mono aborts a few minutes later with
"Fatal error in GC". On Windows the desktop always has an icon, so the early return saves
the day. Therefore the helper synthesizes icons for monitors (64x64) and for X11 windows
without `_NET_WM_ICON` (48x48), and the DLL uploads desktop icons/frames before windows.

## WebSocket / HTTP API

From `output_log`: UDP 42069 (legacy notifications), WebSocket `ws://127.0.0.1:42070/?client=<name>`,
HTTP server on 42071 (serves the UI from `StreamingAssets/Plugins/Applications`), OSC 9001/9000.
Messages are JSON `ApiObject { sender, target, command, jsonData, rawData, commandLevel }`.
Commands (from `ApiHandler.InitializeAPI`): `ToggleMediaPlayer ToggleLayoutMode TogglePerformanceStats
Create Recenter Keyboard Layouts Settings DeleteAll WindowSwitcher MediaPlayPause MediaPrevious MediaNext
WindowSettings WindowSelect WindowDelete LoadLayout SaveLayout DeleteLayout FavoriteLayout RequestLayoutInfo
RequestDateTime RequestSetTheme RequestThemeUpdate RequestSteamAvatar RequestDeviceInformation
RequestLayoutModeState RequestMediaPlayerUpdate RequestUpdateCursorCollisionTexture RequestUpdateOverlayCanvasSize
RequestGetSettings RequestUpdatedLanguageList RequestUpdatedLanguageMap UpdateMediaPlayerInformation
RequestAchievementInformation RequestWindowList SetXSOverlaySystemSetting RequestActiveOverlayInformation
SetActiveOverlaySetting RequestShowTooltip Log OpenSteamVRBindings SendNotification PlayAudio SubscribeToEvents
UnsubscribeToEvents RequestRuntimePerformance RequestOverlayIDs PlayDeviceHaptics`.
`Create` spawns a new overlay window (defaults to desktop capture) — handy for headless tests
(`tools/xsoapi.py Create`). `LoadLayout` takes the slot number in `rawData`.

## Files

* Settings: `<prefix>/drive_c/users/steamuser/AppData/LocalLow/Xiexe/XSOverlay/config/overlaySettings.json`
* Layouts: `.../Xiexe/XSOverlay/layouts/Layout_<n>.json` — `LayoutContainer {name, version (3), count, windows[]}`,
  `LayoutObject {title, className, imageName, processName, devicePath, isAutoHide, isLocked, isInputBlocked,
  isPinned, isDesktopCapture, isWindowCapture, isWebApp, isRotated90/180/270, isAttachedToDevice, maxFPS, minFPS,
  attachedIndex, attachedCustomIndex, desktopIndex, scale, opacity, curvature, resolution[2], position[3], rotation[4]}`
* Logs: `.../Xiexe/XSOverlay/output_logs/output_log_<date>.txt`
* Chromium: `XSOverlay_Data/Plugins/x86_64/VuplexWebViewChromium/{log-chromium.txt,debug.log}`

## SteamVR and Proton versions

* Proton Experimental (Wine 11 based, Oct 2026): if SteamVR is not running when XSOverlay
  initialises OpenVR, `VR_Init` fails with `Interface Not Found (105)`, XSOverlay retries for
  ~6 s and then runs without VR. Proton 9 instead returned `Connect to VR Server Failed (301)`
  and SteamVR auto-started. Start SteamVR first.
* `VR_OVERRIDE=/nonexistent` makes OpenVR init fail deliberately — useful to run XSOverlay
  headless for plugin/UI tests without powering the HMD, but then its main loop is busy with
  VR retries and the WebSocket API does not answer.
* Proton writes `C:\users\steamuser\AppData\Local\openvr\openvrpaths.vrpath` inside the
  prefix pointing at `C:\vrclient`; a prefix created with a bogus `VR_OVERRIDE` keeps working
  later because Proton rewrites the vrclient files every launch.
