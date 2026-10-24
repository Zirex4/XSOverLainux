# Shared-memory protocol (`src/common/xso_shm.h`)

One file, by default `/dev/shm/xso-bridge`, created and truncated by the helper to
`XSO_TOTAL_SIZE` (≈1.5 GiB **virtual**; tmpfs only backs pages that are written, and the
helper punches holes when frame slots are released). The DLL opens it as
`Z:\dev\shm\xso-bridge` with `CreateFile` + `CreateFileMapping` + `MapViewOfFile`, which Wine
maps `MAP_SHARED`. Steam's pressure-vessel container shares `/dev/shm` with the host.

Both sides are x86-64; only fixed-width types are used and `typedef char` asserts pin the
struct sizes (`xso_monitor` 800, `xso_window` 764, `xso_header` 120, `xso_cursor` 36+64 KiB).
Cross-process synchronisation uses `__atomic_*` builtins on 32-bit fields; there are no locks.

```
offset 0        xso_header        magic 0x4F53584F ("OXSO"), version, helper pid + heartbeat,
                                  virtual screen rect, n_monitors, list_gen, cursor_window,
                                  input ring head/tail, pe pid + heartbeat, pointer x/y
                xso_monitor[8]    x,y,w,h, primary, uid (RandR output XID), EDID bytes,
                                  mfg/product code, connector name, friendly name, device_path
                xso_window[256]   see below
                xso_cursor        req/seq, position, hotspot, w,h, BGRA pixels (≤128x128)
                xso_input_event[512]
                icons[256][128*128] BGRA per table slot
page aligned    frame area: 24 slots × 2 halves × (3840*2160*4) bytes
```

## Window table

Each `xso_window` slot is identified by `xid` (X window id; `0x80000000|monitor` for desktop
pseudo-windows; `0` = empty) and `gen` (bumped on every (re)assignment, never 0). The DLL keys
its own records by `(slot index, gen)` and assigns XSOverlay-visible ids incrementally;
a slot whose `xid` becomes 0 or whose `gen` changes produces `WindowRemoved` (+`WindowAdded`).

Helper → DLL fields: `flags` (`VISIBLE, ICONIC, ZOOMED, ALTTAB, PRIMARY, DESKTOP, MAPPED`),
geometry (`x,y,w,h` = client rect, also mirrored as `cx..ch`), `zorder` (0 = top),
`pid`, `owner_xid` (WM_TRANSIENT_FOR), `title` (UTF-16, `title_gen` bumps on change),
`klass` (WM_CLASS), `icon_w/h` + pixels in `icons[slot]`, `monitor`.

DLL → helper fields: `cap_req` (incremented to ask for one capture; the DLL only increments
when `cap_req == cap_done`), `cap_mode` (informational), `draw_cursor`.

Frame publication (helper): `frame_slot` is a lazily assigned slot in the frame area, each
with two halves. The helper writes into `half = frame_buf ^ 1` unless the DLL has claimed
that half (`reader_buf == half`, then it retries on the next loop), stores `frame_w/h/stride`,
then `frame_buf = half` (release) and `frame_seq++` (release). Pixels are BGRA with alpha
forced to 255 unless `--keep-alpha`.

Frame consumption (DLL, render thread): on `frame_seq` change → if size differs from the
last reported texture size → `WindowSizeChanged`; then, if a texture is set and its
`D3D11_TEXTURE2D_DESC` matches, claim `reader_buf = frame_buf`, `UpdateSubresource`
(swizzling to RGBA if the texture is `R8G8B8A8_*`), release, and emit `WindowCaptured`.
Missing texture → `TextureNullError` once per frame sequence; size mismatch →
`TextureSizeError`.

Slots are released (and their pages punched) after 5 s without requests; the window is
un-redirected from XComposite at the same time.

## Cursor

`cursor.req++` by the DLL → helper fills position (root coordinates), hotspot, size and
straight-alpha BGRA pixels from `XFixesGetCursorImage` and bumps `cursor.seq`. The DLL uploads
to the cursor texture and emits `CursorCaptured` when the texture size matches.
`hdr.pointer_x/y` is refreshed every ~8 ms for `UwcGetCursorPosition`.

## Input ring (DLL → helper)

`input[512]` with `input_head` (producer, DLL) and `input_tail` (consumer, helper).
Event types: `MOUSE_MOVE_ABS(x,y)`, `MOUSE_MOVE_REL(dx,dy)`, `MOUSE_BUTTON(btn 1..9, pressed)`,
`MOUSE_WHEEL(delta ±120/notch, horizontal)`, `KEY(vk, pressed, scancode, unicode)`,
`KEY_UNICODE(codepoint, pressed)`, `FOCUS_WINDOW(xid)`, `SHOW_WINDOW(xid, SW_*)`.
The helper injects them with XTest (`XTestFakeMotionEvent/ButtonEvent/KeyEvent`; keysyms
without a keycode are bound to a spare keycode on the fly), `_NET_ACTIVE_WINDOW` and
`XIconifyWindow`/`XMapRaised`. The DLL produces events from in-process hooks of
`user32!SendInput`, `SetCursorPos`, `SetForegroundWindow` and `ShowWindow` (12-byte
`mov rax, imm64; jmp rax` patches; the last two fall back to `win32u!NtUser*` for Wine HWNDs).

## Liveness

The helper updates `hdr.heartbeat_ms` every loop; the DLL treats a heartbeat frozen for
3 s (or `magic == 0`, written on helper exit) as "helper gone" and removes all windows. The
DLL writes `pe_pid`/`pe_heartbeat_ms` so the helper can log "DLL connected".
