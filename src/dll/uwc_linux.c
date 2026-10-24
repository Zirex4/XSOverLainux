/*
 * uWindowCapture.dll replacement for XSOverlay under Proton/Wine.
 *
 * Implements the exported C API of hecomi's uWindowCapture (as used by
 * XSOverlay, including its EDID/desktop extensions) on top of a shared-memory
 * bridge fed by the native xso-bridge helper (see ../helper/xso-bridge.c).
 * Frames are uploaded into Unity's D3D11 textures from the render thread.
 *
 * Build with winegcc (clang backend), see ../../build.sh.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "xso_shm.h"

#define EXPORT __declspec(dllexport)
#define API    __stdcall

/* ------------------------------------------------------------------------ */
/* Unity native plugin interfaces (C flavour)                               */
/* ------------------------------------------------------------------------ */

typedef struct UnityInterfaceGUID { unsigned long long m_GUIDHigh, m_GUIDLow; } UnityInterfaceGUID;
typedef void IUnityInterface;
typedef struct IUnityInterfaces {
    IUnityInterface *(API *GetInterface)(UnityInterfaceGUID guid);
    void (API *RegisterInterface)(UnityInterfaceGUID guid, IUnityInterface *ptr);
} IUnityInterfaces;
typedef enum { kUnityGfxDeviceEventInitialize = 0, kUnityGfxDeviceEventShutdown = 1,
               kUnityGfxDeviceEventBeforeReset = 2, kUnityGfxDeviceEventAfterReset = 3 } UnityGfxDeviceEventType;
typedef void (API *IUnityGraphicsDeviceEventCallback)(UnityGfxDeviceEventType eventType);
typedef struct IUnityGraphics {
    int (API *GetRenderer)(void);
    void (API *RegisterDeviceEventCallback)(IUnityGraphicsDeviceEventCallback callback);
    void (API *UnregisterDeviceEventCallback)(IUnityGraphicsDeviceEventCallback callback);
} IUnityGraphics;
typedef struct IUnityGraphicsD3D11 { ID3D11Device *(API *GetDevice)(void); } IUnityGraphicsD3D11;
typedef void (API *UnityRenderingEvent)(int eventId);
static const UnityInterfaceGUID IUnityGraphics_GUID      = { 0x7CBA0A9CA4DDB544ULL, 0x8C5AD4926EB17B11ULL };
static const UnityInterfaceGUID IUnityGraphicsD3D11_GUID = { 0xAAB37EF87A87D748ULL, 0xBF76967F07EFB177ULL };

/* ------------------------------------------------------------------------ */
/* uWindowCapture public types                                              */
/* ------------------------------------------------------------------------ */

enum MessageType { MSG_None = -1, MSG_WindowAdded = 0, MSG_WindowRemoved = 1, MSG_WindowCaptured = 2,
                   MSG_WindowSizeChanged = 3, MSG_IconCaptured = 4, MSG_CursorCaptured = 5,
                   MSG_Error = 1000, MSG_TextureNullError = 1001, MSG_TextureSizeError = 1002 };
typedef struct Message { int type; int windowId; void *userData; } Message;
typedef struct MonitorEdidInfo { uint16_t mfgId; uint16_t productCode; WCHAR devicePath[128]; WCHAR friendlyName[64]; } MonitorEdidInfo;
typedef void (API *DebugLogFuncPtr)(const char *);

/* ------------------------------------------------------------------------ */
/* state                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct pe_window {
    int      id;                 /* -1 = unused */
    uint32_t xid, gen;
    ID3D11Texture2D *tex;
    ID3D11Texture2D *icon_tex;
    uint32_t seen_seq;           /* last frame_seq noticed by Update */
    volatile uint32_t pending_seq;  /* frame waiting for upload (0 = none) */
    int      tex_w, tex_h;       /* size reported through GetWindowTextureWidth/Height */
    volatile int icon_req;
    uint32_t icon_gen_seen;
    uint32_t title_gen;
    WCHAR    title[XSO_TITLE_LEN];
    char     klass[XSO_CLASS_LEN];
    BYTE    *buffer_copy;
    size_t   buffer_cap;
    BYTE    *swizzle;            /* scratch for RGBA textures */
    size_t   swizzle_cap;
    uint32_t null_tex_reported_seq;
} pe_window;

static int              g_initialized;
static IUnityInterfaces *g_unity;
static IUnityGraphics   *g_gfx;
static ID3D11Device     *g_device;
static HANDLE           g_file = INVALID_HANDLE_VALUE, g_mapping;
static xso_shm         *g_shm;
static DWORD            g_last_open_try;
static int              g_helper_alive;
static pe_window        g_win[XSO_MAX_WINDOWS];
static int              g_next_id;
static CRITICAL_SECTION g_cs;              /* messages + window table */
static Message         *g_msgs;
static UINT             g_msg_count, g_msg_cap;
static DebugLogFuncPtr  g_log_func, g_err_func;
static int              g_debug_mode;
static FILE            *g_logfile;
/* cursor */
static ID3D11Texture2D *g_cursor_tex;
static uint32_t         g_cursor_seen;
static volatile int     g_cursor_pending;
static int              g_cursor_x, g_cursor_y, g_cursor_w, g_cursor_h;
static BYTE            *g_cursor_swizzle;
static uint32_t         g_stat_req, g_stat_captured, g_stat_null, g_stat_size, g_stat_size_changed;
static DWORD            g_stat_last;

/* ------------------------------------------------------------------------ */
/* logging                                                                  */
/* ------------------------------------------------------------------------ */

EXPORT int API XsoPushInput(uint32_t type, int a, int b, int c, int d);

static void xlog(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (!g_logfile) {
        const char *p = getenv("XSO_BRIDGE_LOG");
        char path[MAX_PATH];
        if (p && *p) snprintf(path, sizeof path, "%s", p);
        else snprintf(path, sizeof path, "Z:\\dev\\shm\\xso-uwc.log");
        for (char *c = path; *c; c++) if (*c == '/') *c = '\\';
        g_logfile = fopen(path, "a");
    }
    if (g_logfile) { fprintf(g_logfile, "[uwc %lu] %s\n", (unsigned long)GetTickCount(), buf); fflush(g_logfile); }
    OutputDebugStringA(buf);
}

static int g_trace, g_update_calls, g_render_calls;
#define TRACE_RING 8192
static struct { long long t; const char *fn; int a, b; DWORD tid; } g_ring[TRACE_RING];
static volatile LONG g_ring_pos;
static void trace_fn(const char *fn, int a, int b)
{
    if (!g_trace) return;
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    LONG i = InterlockedIncrement(&g_ring_pos) - 1;
    unsigned k = (unsigned)i % TRACE_RING;
    g_ring[k].t = c.QuadPart; g_ring[k].fn = fn; g_ring[k].a = a; g_ring[k].b = b; g_ring[k].tid = GetCurrentThreadId();
}
static void trace_dump(const char *why)
{
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    LONG end = g_ring_pos; LONG start = end > TRACE_RING ? end - TRACE_RING : 0;
    xlog("=== trace dump (%s): entries %ld..%ld, update_calls=%d render_calls=%d", why, (long)start, (long)end, g_update_calls, g_render_calls);
    LONG from = end - 400 > start ? end - 400 : start;
    for (LONG i = from; i < end; i++) {
        unsigned k = (unsigned)i % TRACE_RING;
        xlog("T %lld.%06lld %s(%d,%d) tid=%lu", (long long)(g_ring[k].t / f.QuadPart), (long long)((g_ring[k].t % f.QuadPart) * 1000000 / f.QuadPart), g_ring[k].fn, g_ring[k].a, g_ring[k].b, g_ring[k].tid);
    }
}
static DWORD WINAPI watchdog_thread(LPVOID arg)
{
    (void)arg;
    int last = -1, same = 0, dumped = 0; DWORD t0 = GetTickCount();
    for (;;) {
        Sleep(1000);
        int cur = g_update_calls;
        if (cur == last) same++; else { same = 0; if (dumped) { xlog("watchdog: updates resumed (%d)", cur); dumped = 0; } }
        last = cur;
        if (same >= 3 && !dumped) { trace_dump("UwcUpdate not called for 3s"); dumped = 1; }
        if (((GetTickCount() - t0) / 1000) % 10 == 0) xlog("watchdog: updates=%d renders=%d msgs=%u", cur, g_render_calls, g_msg_count);
    }
    return 0;
}
#define TRACE_FN(a, b) trace_fn(__func__, (int)(a), (int)(b))

static void xerr(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    xlog("ERROR: %s", buf);
    if (g_err_func) g_err_func(buf);
}

/* ------------------------------------------------------------------------ */
/* messages                                                                 */
/* ------------------------------------------------------------------------ */

static void add_message(int type, int windowId, void *userData)
{
    EnterCriticalSection(&g_cs);
    if (g_msg_count == g_msg_cap) {
        UINT ncap = g_msg_cap ? g_msg_cap * 2 : 256;
        Message *nm = realloc(g_msgs, ncap * sizeof(Message));
        if (!nm) { LeaveCriticalSection(&g_cs); return; }
        g_msgs = nm; g_msg_cap = ncap;
    }
    g_msgs[g_msg_count].type = type;
    g_msgs[g_msg_count].windowId = windowId;
    g_msgs[g_msg_count].userData = userData;
    g_msg_count++;
    LeaveCriticalSection(&g_cs);
}

/* ------------------------------------------------------------------------ */
/* shared memory                                                            */
/* ------------------------------------------------------------------------ */

static void install_input_hooks(void);

static void shm_close(void)
{
    if (g_shm) { UnmapViewOfFile(g_shm); g_shm = NULL; }
    if (g_mapping) { CloseHandle(g_mapping); g_mapping = NULL; }
    if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; }
}

static int shm_try_open(void)
{
    const char *p = getenv("XSO_BRIDGE_SHM");
    char path[MAX_PATH];
    if (p && *p) {
        if (p[0] == '/') snprintf(path, sizeof path, "Z:%s", p); else snprintf(path, sizeof path, "%s", p);
    } else {
        snprintf(path, sizeof path, "Z:%s", XSO_SHM_DEFAULT_PATH);
    }
    for (char *c = path; *c; c++) if (*c == '/') *c = '\\';

    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { xlog("bridge file %s not available (err %lu)", path, GetLastError()); return 0; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || (size_t)sz.QuadPart != XSO_TOTAL_SIZE) {
        xlog("bridge file has unexpected size %lld (want %zu)", (long long)sz.QuadPart, (size_t)XSO_TOTAL_SIZE);
        CloseHandle(f); return 0;
    }
    HANDLE m = CreateFileMappingA(f, NULL, PAGE_READWRITE, 0, 0, NULL);
    if (!m) { xlog("CreateFileMapping failed %lu", GetLastError()); CloseHandle(f); return 0; }
    void *v = MapViewOfFile(m, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, 0);
    if (!v) { xlog("MapViewOfFile failed %lu", GetLastError()); CloseHandle(m); CloseHandle(f); return 0; }
    xso_shm *s = (xso_shm *)v;
    if (s->hdr.magic != XSO_SHM_MAGIC || s->hdr.version != XSO_SHM_VERSION) {
        xlog("bridge file has bad magic/version (%08x/%u); helper not running?", s->hdr.magic, s->hdr.version);
        UnmapViewOfFile(v); CloseHandle(m); CloseHandle(f); return 0;
    }
    g_file = f; g_mapping = m; g_shm = s;
    g_shm->hdr.pe_pid = GetCurrentProcessId();
    install_input_hooks();
    xlog("bridge mapped: %s helper pid %u, %u monitors, screen %dx%d", path, s->hdr.helper_pid, s->hdr.n_monitors, s->hdr.screen_w, s->hdr.screen_h);
    return 1;
}

static uint32_t helper_now_ms(void)
{
    /* the helper's heartbeat uses CLOCK_MONOTONIC; compare against its own clock by
       reading it again later. We only need "is it moving". */
    return g_shm ? g_shm->hdr.heartbeat_ms : 0;
}

/* ------------------------------------------------------------------------ */
/* window table sync (main thread)                                          */
/* ------------------------------------------------------------------------ */

static pe_window *win_by_id(int id)
{
    if (id < 0) return NULL;
    for (int i = 0; i < XSO_MAX_WINDOWS; i++) if (g_win[i].id == id) return &g_win[i];
    return NULL;
}

static int idx_of(pe_window *p) { return (int)(p - g_win); }

static xso_window *shm_entry(pe_window *p)
{
    if (!g_shm || !p) return NULL;
    xso_window *e = &g_shm->windows[idx_of(p)];
    if (e->xid != p->xid || e->gen != p->gen) return NULL;
    return e;
}

static void copy_strings(pe_window *p, xso_window *e)
{
    memcpy(p->title, e->title, sizeof p->title);
    p->title[XSO_TITLE_LEN - 1] = 0;
    memcpy(p->klass, e->klass, sizeof p->klass);
    p->klass[XSO_CLASS_LEN - 1] = 0;
    p->title_gen = e->title_gen;
}

static void remove_pe_window(pe_window *p)
{
    add_message(MSG_WindowRemoved, p->id, (void *)(uintptr_t)p->xid);
    p->id = -1;
    p->tex = NULL; p->icon_tex = NULL;
    p->pending_seq = 0; p->icon_req = 0;
}

static void sync_windows(void)
{
    static uint32_t last_hb; static DWORD last_hb_change;
    DWORD now = GetTickCount();
    uint32_t hb = helper_now_ms();
    if (hb != last_hb) { last_hb = hb; last_hb_change = now; }
    int alive = (now - last_hb_change) < 3000 && g_shm->hdr.magic == XSO_SHM_MAGIC;
    if (alive != g_helper_alive) { xlog("helper %s", alive ? "alive" : "stopped responding"); g_helper_alive = alive; }

    for (int i = 0; i < XSO_MAX_WINDOWS; i++) {
        pe_window *p = &g_win[i];
        xso_window *e = &g_shm->windows[i];
        uint32_t xid = alive ? __atomic_load_n(&e->xid, __ATOMIC_ACQUIRE) : 0;
        uint32_t gen = e->gen;
        if (p->id >= 0 && (xid == 0 || gen != p->gen)) remove_pe_window(p);
        if (xid != 0 && p->id < 0) {
            p->id = g_next_id++;
            p->xid = xid; p->gen = gen;
            p->tex = NULL; p->icon_tex = NULL;
            p->seen_seq = __atomic_load_n(&e->frame_seq, __ATOMIC_ACQUIRE);
            p->pending_seq = 0; p->icon_req = 0; p->icon_gen_seen = e->icon_gen;
            p->tex_w = p->tex_h = 0;
            p->null_tex_reported_seq = 0;
            copy_strings(p, e);
            add_message(MSG_WindowAdded, p->id, (void *)(uintptr_t)xid);
            if (g_debug_mode) xlog("window added id=%d xid=%08x flags=%x '%ls'", p->id, xid, e->flags, p->title);
        }
        if (p->id < 0) continue;
        if (e->title_gen != p->title_gen) copy_strings(p, e);
        uint32_t seq = __atomic_load_n(&e->frame_seq, __ATOMIC_ACQUIRE);
        if (seq != p->seen_seq) {
            p->seen_seq = seq;
            int w = e->frame_w, h = e->frame_h;
            if (w > 0 && h > 0) {
                if (w != p->tex_w || h != p->tex_h) {
                    p->tex_w = w; p->tex_h = h;
                    add_message(MSG_WindowSizeChanged, p->id, (void *)(uintptr_t)xid);
                    g_stat_size_changed++;
                    if (g_debug_mode) xlog("window id=%d frame size %dx%d", p->id, w, h);
                }
                p->pending_seq = seq;
            }
        }
    }
    uint32_t cseq = __atomic_load_n(&g_shm->cursor.seq, __ATOMIC_ACQUIRE);
    if (cseq != g_cursor_seen) {
        g_cursor_seen = cseq;
        g_cursor_x = g_shm->cursor.x; g_cursor_y = g_shm->cursor.y;
        g_cursor_w = g_shm->cursor.w; g_cursor_h = g_shm->cursor.h;
        g_cursor_pending = 1;
    }
    g_shm->hdr.pe_heartbeat_ms = now;
}

/* ------------------------------------------------------------------------ */
/* D3D11 upload (render thread)                                             */
/* ------------------------------------------------------------------------ */

static int is_rgba(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}

static void swizzle_bgra_to_rgba(const BYTE *src, int src_stride, BYTE *dst, int w, int h)
{
    for (int y = 0; y < h; y++) {
        const uint32_t *s = (const uint32_t *)(src + (size_t)y * (size_t)src_stride);
        uint32_t *d = (uint32_t *)(dst + (size_t)y * (size_t)w * 4);
        for (int x = 0; x < w; x++) {
            uint32_t p = s[x];
            d[x] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
        }
    }
}

static int upload_pixels(ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, const BYTE *src, int stride, int w, int h,
                         BYTE **scratch, size_t *scratch_cap, const char *what, int id)
{
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(tex, &desc);
    if ((int)desc.Width != w || (int)desc.Height != h) {
        if (g_debug_mode) xlog("%s id=%d texture size %ux%u != frame %dx%d", what, id, desc.Width, desc.Height, w, h);
        return -1;
    }
    if (is_rgba(desc.Format)) {
        size_t need = (size_t)w * (size_t)h * 4;
        if (*scratch_cap < need) { BYTE *n = realloc(*scratch, need); if (!n) return 0; *scratch = n; *scratch_cap = need; }
        swizzle_bgra_to_rgba(src, stride, *scratch, w, h);
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)tex, 0, NULL, *scratch, (UINT)(w * 4), 0);
    } else {
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)tex, 0, NULL, src, (UINT)stride, 0);
    }
    return 1;
}

static void ensure_device(void)
{
    if (g_device || !g_unity) return;
    IUnityGraphicsD3D11 *d3d = (IUnityGraphicsD3D11 *)g_unity->GetInterface(IUnityGraphicsD3D11_GUID);
    if (d3d) g_device = d3d->GetDevice();
    xlog("D3D11 device: %p", (void *)g_device);
}

static void render_all(void)
{
    g_render_calls++;
    TRACE_FN(g_render_calls, 99);
    if (!g_shm || !g_initialized) return;
    ensure_device();
    if (!g_device) return;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Device_GetImmediateContext(g_device, &ctx);
    if (!ctx) return;

    /* desktops first: XSOverlay's window list must contain a desktop entry before any
       window entry, otherwise its title-update event chain recurses without end. */
    for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < XSO_MAX_WINDOWS; i++) {
        pe_window *p = &g_win[i];
        if (p->id < 0) continue;
        xso_window *e = &g_shm->windows[i];
        if (((e->flags & XSO_WF_DESKTOP) != 0) != (pass == 0)) continue;
        uint32_t seq = p->pending_seq;
        if (seq) {
            p->pending_seq = 0;
            if (e->xid != p->xid || e->gen != p->gen) continue;
            if (!p->tex) {
                if (p->null_tex_reported_seq != seq) { p->null_tex_reported_seq = seq; add_message(MSG_TextureNullError, p->id, NULL); g_stat_null++; }
                continue;
            }
            int slot = e->frame_slot;
            if (slot < 0 || slot >= XSO_FRAME_SLOTS) continue;
            /* claim the published half */
            uint32_t half;
            for (int tries = 0; tries < 4; tries++) {
                half = __atomic_load_n(&e->frame_buf, __ATOMIC_ACQUIRE);
                __atomic_store_n(&e->reader_buf, half, __ATOMIC_SEQ_CST);
                if (__atomic_load_n(&e->frame_buf, __ATOMIC_ACQUIRE) == half) break;
            }
            int w = e->frame_w, h = e->frame_h, stride = e->frame_stride;
            const BYTE *src = XSO_FRAME_PTR(g_shm, slot, half & 1u);
            int r = upload_pixels(ctx, p->tex, src, stride, w, h, &p->swizzle, &p->swizzle_cap, "window", p->id);
            __atomic_store_n(&e->reader_buf, XSO_NO_INDEX, __ATOMIC_RELEASE);
            if (r > 0) { add_message(MSG_WindowCaptured, p->id, (void *)(uintptr_t)p->xid); g_stat_captured++; }
            else if (r < 0) { add_message(MSG_TextureSizeError, p->id, NULL); g_stat_size++; }
        }
        if (p->icon_req) {
            p->icon_req = 0;
            if (p->icon_tex && e->icon_w > 0 && e->icon_h > 0) {
                int r = upload_pixels(ctx, p->icon_tex, (const BYTE *)g_shm->icons[i], e->icon_w * 4, e->icon_w, e->icon_h,
                                      &p->swizzle, &p->swizzle_cap, "icon", p->id);
                if (r > 0) add_message(MSG_IconCaptured, p->id, (void *)(uintptr_t)p->xid);
            }
        }
    }
    if (g_cursor_pending) {
        g_cursor_pending = 0;
        if (g_cursor_tex && g_cursor_w > 0 && g_cursor_h > 0) {
            static size_t cap;
            int r = upload_pixels(ctx, g_cursor_tex, (const BYTE *)g_shm->cursor.pixels, g_cursor_w * 4, g_cursor_w, g_cursor_h,
                                  &g_cursor_swizzle, &cap, "cursor", -1);
            if (r > 0) add_message(MSG_CursorCaptured, -1, NULL);
        }
    }
    ID3D11DeviceContext_Release(ctx);
}

static void API OnRenderEvent(int id)
{
    (void)id;
    render_all();
}


/* ------------------------------------------------------------------------ */
/* input hooks: route user32 SendInput/SetCursorPos/SetForegroundWindow/    */
/* ShowWindow to the X11 helper (XTest) when they target host windows.       */
/* ------------------------------------------------------------------------ */

typedef BOOL (WINAPI *pfn_NtUserSetForegroundWindow)(HWND);
typedef BOOL (WINAPI *pfn_NtUserShowWindow)(HWND, INT);
static pfn_NtUserSetForegroundWindow g_NtUserSetForegroundWindow;
static pfn_NtUserShowWindow g_NtUserShowWindow;
static int g_hooks_installed;
static uint32_t g_stat_input;

static int is_host_xid(HWND h)
{
    uintptr_t v = (uintptr_t)h;
    if (!v || v > 0xFFFFFFFFu) return 0;
    if (!g_shm) return 0;
    for (int i = 0; i < XSO_MAX_WINDOWS; i++) if (g_win[i].id >= 0 && g_win[i].xid == (uint32_t)v) return 1;
    return 0;
}

static UINT WINAPI hook_SendInput(UINT count, LPINPUT inputs, int size)
{
    if (!inputs || size != (int)sizeof(INPUT)) return 0;
    UINT sent = 0;
    for (UINT i = 0; i < count; i++) {
        const INPUT *in = &inputs[i];
        if (in->type == INPUT_MOUSE) {
            const MOUSEINPUT *m = &in->mi;
            DWORD f = m->dwFlags;
            if (f & MOUSEEVENTF_MOVE) {
                if (f & MOUSEEVENTF_ABSOLUTE) {
                    int sx = g_shm ? g_shm->hdr.screen_x : 0, sy = g_shm ? g_shm->hdr.screen_y : 0;
                    int sw = g_shm ? g_shm->hdr.screen_w : 0, sh = g_shm ? g_shm->hdr.screen_h : 0;
                    if (sw <= 0 || sh <= 0) { sw = GetSystemMetrics(SM_CXVIRTUALSCREEN); sh = GetSystemMetrics(SM_CYVIRTUALSCREEN); }
                    int x = sx + (int)(((int64_t)m->dx * sw + 32768) / 65536);
                    int y = sy + (int)(((int64_t)m->dy * sh + 32768) / 65536);
                    XsoPushInput(XSO_IN_MOUSE_MOVE_ABS, x, y, 0, 0);
                } else {
                    XsoPushInput(XSO_IN_MOUSE_MOVE_REL, m->dx, m->dy, 0, 0);
                }
            }
            if (f & MOUSEEVENTF_LEFTDOWN)   XsoPushInput(XSO_IN_MOUSE_BUTTON, 1, 1, 0, 0);
            if (f & MOUSEEVENTF_LEFTUP)     XsoPushInput(XSO_IN_MOUSE_BUTTON, 1, 0, 0, 0);
            if (f & MOUSEEVENTF_MIDDLEDOWN) XsoPushInput(XSO_IN_MOUSE_BUTTON, 2, 1, 0, 0);
            if (f & MOUSEEVENTF_MIDDLEUP)   XsoPushInput(XSO_IN_MOUSE_BUTTON, 2, 0, 0, 0);
            if (f & MOUSEEVENTF_RIGHTDOWN)  XsoPushInput(XSO_IN_MOUSE_BUTTON, 3, 1, 0, 0);
            if (f & MOUSEEVENTF_RIGHTUP)    XsoPushInput(XSO_IN_MOUSE_BUTTON, 3, 0, 0, 0);
            if (f & MOUSEEVENTF_XDOWN)      XsoPushInput(XSO_IN_MOUSE_BUTTON, m->mouseData == XBUTTON2 ? 9 : 8, 1, 0, 0);
            if (f & MOUSEEVENTF_XUP)        XsoPushInput(XSO_IN_MOUSE_BUTTON, m->mouseData == XBUTTON2 ? 9 : 8, 0, 0, 0);
            if (f & MOUSEEVENTF_WHEEL)      XsoPushInput(XSO_IN_MOUSE_WHEEL, (int)(short)(m->mouseData & 0xFFFF), 0, 0, 0);
            if (f & MOUSEEVENTF_HWHEEL)     XsoPushInput(XSO_IN_MOUSE_WHEEL, (int)(short)(m->mouseData & 0xFFFF), 1, 0, 0);
            sent++;
        } else if (in->type == INPUT_KEYBOARD) {
            const KEYBDINPUT *k = &in->ki;
            int pressed = !(k->dwFlags & KEYEVENTF_KEYUP);
            if (k->dwFlags & KEYEVENTF_UNICODE) {
                XsoPushInput(XSO_IN_KEY_UNICODE, k->wScan, pressed, 0, 0);
            } else {
                int vk = k->wVk;
                if (!vk && (k->dwFlags & KEYEVENTF_SCANCODE)) vk = (int)MapVirtualKeyW(k->wScan, MAPVK_VSC_TO_VK_EX);
                XsoPushInput(XSO_IN_KEY, vk, pressed, k->wScan, 0);
            }
            sent++;
        } else {
            sent++;
        }
    }
    g_stat_input += sent;
    return sent;
}

static BOOL WINAPI hook_SetCursorPos(INT x, INT y)
{
    XsoPushInput(XSO_IN_MOUSE_MOVE_ABS, x, y, 0, 0);
    g_stat_input++;
    return TRUE;
}

static BOOL WINAPI hook_SetForegroundWindow(HWND h)
{
    if (is_host_xid(h)) { XsoPushInput(XSO_IN_FOCUS_WINDOW, (int)(uintptr_t)h, 0, 0, 0); return TRUE; }
    return g_NtUserSetForegroundWindow ? g_NtUserSetForegroundWindow(h) : FALSE;
}

static BOOL WINAPI hook_ShowWindow(HWND h, INT cmd)
{
    if (is_host_xid(h)) { XsoPushInput(XSO_IN_SHOW_WINDOW, (int)(uintptr_t)h, cmd, 0, 0); return TRUE; }
    return g_NtUserShowWindow ? g_NtUserShowWindow(h, cmd) : FALSE;
}

static int patch_jump(void *target, void *dest)
{
    unsigned char code[12] = { 0x48, 0xB8, 0,0,0,0,0,0,0,0, 0xFF, 0xE0 };   /* mov rax, imm64; jmp rax */
    memcpy(code + 2, &dest, 8);
    DWORD old;
    if (!VirtualProtect(target, sizeof code, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(target, code, sizeof code);
    VirtualProtect(target, sizeof code, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof code);
    return 1;
}

static void install_input_hooks(void)
{
    if (g_hooks_installed) return;
    const char *e = getenv("XSO_INPUT");
    if (e && (*e == '0' || *e == 'n' || *e == 'N')) { xlog("input hooks disabled by XSO_INPUT"); g_hooks_installed = 1; return; }
    HMODULE u32 = GetModuleHandleA("user32.dll");
    HMODULE w32u = GetModuleHandleA("win32u.dll");
    if (!u32) { xlog("user32 not loaded; input hooks skipped"); return; }
    if (w32u) {
        g_NtUserSetForegroundWindow = (pfn_NtUserSetForegroundWindow)GetProcAddress(w32u, "NtUserSetForegroundWindow");
        g_NtUserShowWindow = (pfn_NtUserShowWindow)GetProcAddress(w32u, "NtUserShowWindow");
    }
    struct { const char *name; void *dest; } hooks[] = {
        { "SendInput", (void *)hook_SendInput },
        { "SetCursorPos", (void *)hook_SetCursorPos },
        { "SetForegroundWindow", (void *)hook_SetForegroundWindow },
        { "ShowWindow", (void *)hook_ShowWindow },
    };
    const char *sel = getenv("XSO_HOOKS");   /* optional comma list to bisect */
    int ok = 0;
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++) {
        if (sel && *sel && !strstr(sel, hooks[i].name)) { xlog("hook: %s skipped (XSO_HOOKS)", hooks[i].name); continue; }
        void *t = (void *)GetProcAddress(u32, hooks[i].name);
        if (!t) { xlog("hook: %s not found", hooks[i].name); continue; }
        if (patch_jump(t, hooks[i].dest)) ok++; else xlog("hook: patching %s failed (%lu)", hooks[i].name, GetLastError());
    }
    g_hooks_installed = 1;
    xlog("input hooks installed: %d/4 (XTest via helper)", ok);
}

/* ------------------------------------------------------------------------ */
/* init / shutdown                                                          */
/* ------------------------------------------------------------------------ */

static void do_initialize(void)
{
    if (g_initialized) return;
    g_initialized = 1;
    { const char *d = getenv("XSO_UWC_DEBUG"); if (d && *d && *d != '0') g_debug_mode = 1; }
    { const char *d = getenv("XSO_UWC_TRACE"); if (d && *d && *d != '0') { g_trace = 1; CloseHandle(CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL)); } }
    for (int i = 0; i < XSO_MAX_WINDOWS; i++) { g_win[i].id = -1; }
    g_next_id = 0;
    xlog("UwcInitialize (linux bridge build " __DATE__ " " __TIME__ ")");
    shm_try_open();
    g_last_open_try = GetTickCount();
}

static void do_finalize(void)
{
    if (!g_initialized) return;
    g_initialized = 0;
    xlog("UwcFinalize");
    shm_close();
    for (int i = 0; i < XSO_MAX_WINDOWS; i++) {
        g_win[i].id = -1;
        free(g_win[i].buffer_copy); g_win[i].buffer_copy = NULL; g_win[i].buffer_cap = 0;
        free(g_win[i].swizzle); g_win[i].swizzle = NULL; g_win[i].swizzle_cap = 0;
    }
    EnterCriticalSection(&g_cs);
    g_msg_count = 0;
    LeaveCriticalSection(&g_cs);
}

static void API OnGraphicsDeviceEvent(UnityGfxDeviceEventType ev)
{
    if (ev == kUnityGfxDeviceEventInitialize) { ensure_device(); }
    else if (ev == kUnityGfxDeviceEventShutdown) { g_device = NULL; }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_cs);
        for (int i = 0; i < XSO_MAX_WINDOWS; i++) g_win[i].id = -1;
    }
    return TRUE;
}

/* ------------------------------------------------------------------------ */
/* exports                                                                  */
/* ------------------------------------------------------------------------ */

EXPORT void API UnityPluginLoad(IUnityInterfaces *unity)
{
    g_unity = unity;
    g_gfx = (IUnityGraphics *)unity->GetInterface(IUnityGraphics_GUID);
    if (g_gfx) g_gfx->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
    xlog("UnityPluginLoad: gfx=%p renderer=%d", (void *)g_gfx, g_gfx ? g_gfx->GetRenderer() : -1);
}

EXPORT void API UnityPluginUnload(void)
{
    if (g_gfx) g_gfx->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
    g_gfx = NULL; g_unity = NULL; g_device = NULL;
}

EXPORT void API UwcInitialize(void) { do_initialize(); }
EXPORT void API UwcFinalize(void) { do_finalize(); }
EXPORT void API UwcSetDebugMode(int mode) { if (mode) g_debug_mode = mode; xlog("UwcSetDebugMode(%d)", mode); }
EXPORT void API UwcSetLogFunc(DebugLogFuncPtr f) { g_log_func = f; }
EXPORT void API UwcSetErrorFunc(DebugLogFuncPtr f) { g_err_func = f; }
EXPORT UnityRenderingEvent API UwcGetRenderEventFunc(void) { return OnRenderEvent; }

EXPORT void API UwcUpdate(float dt)
{
    g_update_calls++;
    TRACE_FN(g_update_calls, 0);
    (void)dt;
    if (!g_initialized) return;
    if (!g_shm) {
        DWORD now = GetTickCount();
        if (now - g_last_open_try >= 1000) { g_last_open_try = now; shm_try_open(); }
        if (!g_shm) return;
    }
    sync_windows();
    if (g_debug_mode) {
        DWORD now = GetTickCount();
        if (now - g_stat_last >= 5000) {
            g_stat_last = now;
            int n = 0, with_tex = 0; for (int i = 0; i < XSO_MAX_WINDOWS; i++) if (g_win[i].id >= 0) { n++; if (g_win[i].tex) with_tex++; }
            xlog("stats: windows=%d textured=%d requests=%u captured=%u nulltex=%u sizeerr=%u sizechanged=%u input=%u msgs=%u helper=%s",
                 n, with_tex, g_stat_req, g_stat_captured, g_stat_null, g_stat_size, g_stat_size_changed, g_stat_input, g_msg_count, g_helper_alive ? "ok" : "dead");
        }
    }
}

EXPORT UINT API UwcGetMessageCount(void) { TRACE_FN(g_msg_count, 0); return g_msg_count; }
EXPORT const Message *API UwcGetMessages(void) { TRACE_FN(g_msg_count, 1); return g_msgs; }
EXPORT void API UwcClearMessages(void) { TRACE_FN(0, 2); EnterCriticalSection(&g_cs); g_msg_count = 0; LeaveCriticalSection(&g_cs); }

EXPORT void API UwcExcludeRemovedWindowEvents(void)
{
    TRACE_FN(0, 3);
    EnterCriticalSection(&g_cs);
    UINT n = g_msg_count, o = 0;
    for (UINT i = 0; i < n; i++) {
        Message m = g_msgs[i];
        int drop = 0;
        if (m.type != MSG_WindowRemoved) {
            for (UINT j = 0; j < n; j++)
                if (g_msgs[j].type == MSG_WindowRemoved && g_msgs[j].windowId == m.windowId) { drop = 1; break; }
        }
        if (!drop) g_msgs[o++] = m;
    }
    g_msg_count = o;
    LeaveCriticalSection(&g_cs);
}

EXPORT int API UwcCheckWindowExistence(int id) { TRACE_FN(id, 4); return win_by_id(id) != NULL; }

#define WIN_OR(ret) TRACE_FN(id, 0); pe_window *p = win_by_id(id); xso_window *e = shm_entry(p); if (!p || !e) return ret

EXPORT int   API UwcGetWindowParentId(int id)
{
    WIN_OR(-1);
    if (!e->owner_xid) return -1;
    for (int i = 0; i < XSO_MAX_WINDOWS; i++) if (g_win[i].id >= 0 && g_win[i].xid == e->owner_xid) return g_win[i].id;
    return -1;
}
EXPORT HWND  API UwcGetWindowHandle(int id)        { WIN_OR(NULL); return (HWND)(uintptr_t)e->xid; }
EXPORT HWND  API UwcGetWindowOwnerHandle(int id)   { WIN_OR(NULL); return (HWND)(uintptr_t)e->owner_xid; }
EXPORT HWND  API UwcGetWindowParentHandle(int id)  { (void)id; return NULL; }
EXPORT HINSTANCE API UwcGetWindowInstance(int id)  { (void)id; return NULL; }
EXPORT DWORD API UwcGetWindowProcessId(int id)     { WIN_OR((DWORD)-1); return GetCurrentProcessId(); }
EXPORT DWORD API UwcGetWindowThreadId(int id)      { WIN_OR((DWORD)-1); return e->xid; }
EXPORT void  API UwcRequestUpdateWindowTitle(int id) { WIN_OR(); copy_strings(p, e); }

EXPORT void API UwcRequestCaptureWindow(int id, int priority)
{
    (void)priority;
    WIN_OR();
    uint32_t req = __atomic_load_n(&e->cap_req, __ATOMIC_ACQUIRE);
    uint32_t done = __atomic_load_n(&e->cap_done, __ATOMIC_ACQUIRE);
    if (req == done) __atomic_store_n(&e->cap_req, req + 1, __ATOMIC_RELEASE);
    g_stat_req++;
}
EXPORT void API StartCaptureWindow(int id, int priority) { UwcRequestCaptureWindow(id, priority); }
EXPORT void API StopCaptureWindow(int id) { (void)id; }
EXPORT void API UwcRequestCaptureIcon(int id) { WIN_OR(); p->icon_req = 1; }

EXPORT UINT API UwcGetWindowX(int id)      { WIN_OR(0); return (UINT)e->x; }
EXPORT UINT API UwcGetWindowY(int id)      { WIN_OR(0); return (UINT)e->y; }
EXPORT UINT API UwcGetWindowWidth(int id)  { WIN_OR(0); return (UINT)e->w; }
EXPORT UINT API UwcGetWindowHeight(int id) { WIN_OR(0); return (UINT)e->h; }
EXPORT UINT API UwcGetWindowZOrder(int id) { WIN_OR(0); return e->zorder == 0xFFFF ? 0xFFFF : e->zorder; }

EXPORT BYTE *API UwcGetWindowBuffer(int id)
{
    WIN_OR(NULL);
    int slot = e->frame_slot; int w = e->frame_w, h = e->frame_h;
    if (slot < 0 || w <= 0 || h <= 0) return NULL;
    size_t need = (size_t)w * (size_t)h * 4;
    if (p->buffer_cap < need) { BYTE *n = realloc(p->buffer_copy, need); if (!n) return NULL; p->buffer_copy = n; p->buffer_cap = need; }
    uint32_t half = __atomic_load_n(&e->frame_buf, __ATOMIC_ACQUIRE);
    const BYTE *src = XSO_FRAME_PTR(g_shm, slot, half & 1u);
    for (int y = 0; y < h; y++) memcpy(p->buffer_copy + (size_t)y * w * 4, src + (size_t)y * e->frame_stride, (size_t)w * 4);
    return p->buffer_copy;
}

EXPORT UINT API UwcGetWindowTextureWidth(int id)   { WIN_OR(0); return (UINT)p->tex_w; }
EXPORT UINT API UwcGetWindowTextureHeight(int id)  { WIN_OR(0); return (UINT)p->tex_h; }
EXPORT UINT API UwcGetWindowTextureOffsetX(int id) { (void)id; return 0; }
EXPORT UINT API UwcGetWindowTextureOffsetY(int id) { (void)id; return 0; }
EXPORT UINT API UwcGetWindowIconWidth(int id)      { WIN_OR(0); return (UINT)e->icon_w; }
EXPORT UINT API UwcGetWindowIconHeight(int id)     { WIN_OR(0); return (UINT)e->icon_h; }

EXPORT UINT API UwcGetWindowTitleLength(int id)    { WIN_OR(0); return (UINT)wcslen(p->title); }
EXPORT const WCHAR *API UwcGetWindowTitle(int id)  { WIN_OR(NULL); return p->title; }
EXPORT UINT API UwcGetWindowClassNameLength(int id){ WIN_OR(0); return (UINT)strlen(p->klass); }
EXPORT const char *API UwcGetWindowClassName(int id){ WIN_OR(NULL); return p->klass; }

EXPORT ID3D11Texture2D *API UwcGetWindowTexturePtr(int id) { WIN_OR(NULL); return p->tex; }
EXPORT void API UwcSetWindowTexturePtr(int id, ID3D11Texture2D *ptr) { WIN_OR(); p->tex = ptr; if (g_debug_mode) xlog("SetWindowTexturePtr id=%d %p", id, (void *)ptr); }
EXPORT ID3D11Texture2D *API UwcGetWindowIconTexturePtr(int id) { WIN_OR(NULL); return p->icon_tex; }
EXPORT void API UwcSetWindowIconTexturePtr(int id, ID3D11Texture2D *ptr) { WIN_OR(); p->icon_tex = ptr; }

EXPORT int  API UwcGetWindowCaptureMode(int id)    { WIN_OR(-1); return e->cap_mode; }
EXPORT void API UwcSetWindowCaptureMode(int id, int mode) { WIN_OR(); e->cap_mode = mode; }
EXPORT int  API UwcGetWindowCursorDraw(int id)     { WIN_OR(0); return e->draw_cursor != 0; }
EXPORT void API UwcSetWindowCursorDraw(int id, int draw) { WIN_OR(); e->draw_cursor = draw ? 1 : 0; }

EXPORT int API UwcIsWindow(int id)                   { WIN_OR(0); return 1; }
EXPORT int API UwcIsAltTabWindow(int id)             { WIN_OR(0); return (e->flags & XSO_WF_ALTTAB) && !(e->flags & XSO_WF_DESKTOP); }
EXPORT int API UwcIsDesktop(int id)                  { WIN_OR(0); return (e->flags & XSO_WF_DESKTOP) != 0; }
EXPORT int API UwcIsPrimaryDesktop(int id)           { WIN_OR(0); return (e->flags & XSO_WF_DESKTOP) && (e->flags & XSO_WF_PRIMARY); }
EXPORT int API UwcIsWindowVisible(int id)            { WIN_OR(0); return (e->flags & XSO_WF_VISIBLE) != 0; }
EXPORT int API UwcIsWindowEnabled(int id)            { WIN_OR(0); return 1; }
EXPORT int API UwcIsWindowUnicode(int id)            { WIN_OR(0); return 1; }
EXPORT int API UwcIsWindowZoomed(int id)             { WIN_OR(0); return (e->flags & XSO_WF_ZOOMED) != 0; }
EXPORT int API UwcIsWindowIconic(int id)             { WIN_OR(0); return (e->flags & XSO_WF_ICONIC) != 0; }
EXPORT int API UwcIsWindowHungUp(int id)             { (void)id; return 0; }
EXPORT int API UwcIsWindowTouchable(int id)          { (void)id; return 0; }
EXPORT int API UwcIsWindowApplicationFrameWindow(int id) { (void)id; return 0; }
EXPORT int API UwcIsWindowUWP(int id)                { (void)id; return 0; }
EXPORT int API UwcIsWindowBackground(int id)         { (void)id; return 0; }
EXPORT int API UwcIsWindowsGraphicsCaptureAvailable(int id) { WIN_OR(0); return 1; }
EXPORT int API UwcIsWindowsGraphicsCaptureSupported(void) { return 1; }
EXPORT int API UwcIsWindowsGraphicsCaptureCursorCaptureEnabledApiSupported(void) { return 1; }

EXPORT UINT API UwcGetDesktopUid(int id)
{
    WIN_OR(0);
    if (!(e->flags & XSO_WF_DESKTOP) || e->monitor < 0 || e->monitor >= (int)g_shm->hdr.n_monitors) return 0;
    return g_shm->monitors[e->monitor].uid;
}

EXPORT MonitorEdidInfo API UwcGetMonitorEdid(int id)
{
    TRACE_FN(id, 5);
    MonitorEdidInfo r;
    memset(&r, 0, sizeof r);
    pe_window *p = win_by_id(id); xso_window *e = shm_entry(p);
    if (!p || !e || e->monitor < 0 || e->monitor >= (int)g_shm->hdr.n_monitors) return r;
    const xso_monitor *m = &g_shm->monitors[e->monitor];
    r.mfgId = m->mfg_id; r.productCode = m->product_code;
    MultiByteToWideChar(CP_UTF8, 0, m->device_path, -1, r.devicePath, 128); r.devicePath[127] = 0;
    MultiByteToWideChar(CP_UTF8, 0, m->friendly, -1, r.friendlyName, 64); r.friendlyName[63] = 0;
    return r;
}

EXPORT int API UwcGetWindowPixels(int id, BYTE *output, int x, int y, int width, int height)
{
    WIN_OR(0);
    int slot = e->frame_slot, bw = e->frame_w, bh = e->frame_h;
    if (slot < 0 || !output) return 0;
    if (x < 0 || y < 0 || width <= 0 || height <= 0 || x + width > bw || y + height > bh) return 0;
    uint32_t half = __atomic_load_n(&e->frame_buf, __ATOMIC_ACQUIRE);
    const BYTE *src = XSO_FRAME_PTR(g_shm, slot, half & 1u);
    for (int j = 0; j < height; j++) {
        const uint32_t *row = (const uint32_t *)(src + (size_t)(y + (height - 1 - j)) * (size_t)e->frame_stride);
        for (int i = 0; i < width; i++) {
            uint32_t px = row[x + i];
            BYTE *o = output + ((size_t)j * width + i) * 4;
            o[0] = (px >> 16) & 0xFF; o[1] = (px >> 8) & 0xFF; o[2] = px & 0xFF; o[3] = (px >> 24) & 0xFF;
        }
    }
    return 1;
}

EXPORT UINT API UwcGetWindowPixel(int id, int x, int y)
{
    BYTE out[4] = { 0, 0, 0, 0 };
    UwcGetWindowPixels(id, out, x, y, 1, 1);
    UINT v; memcpy(&v, out, 4); return v;
}

EXPORT POINT API UwcGetCursorPosition(void)
{
    TRACE_FN(0, 6);
    POINT pt = { 0, 0 };
    if (g_shm) { pt.x = g_shm->hdr.pointer_x; pt.y = g_shm->hdr.pointer_y; }
    return pt;
}

static int id_from_index(uint32_t idx)
{
    if (idx >= XSO_MAX_WINDOWS) return -1;
    return g_win[idx].id;
}

EXPORT int API UwcGetWindowIdFromPoint(int x, int y)
{
    TRACE_FN(x, y);
    if (!g_shm) return -1;
    int best = -1; uint32_t bestz = 0xFFFFFFFFu; int desk = -1;
    for (int i = 0; i < XSO_MAX_WINDOWS; i++) {
        pe_window *p = &g_win[i]; xso_window *e = &g_shm->windows[i];
        if (p->id < 0 || e->xid != p->xid) continue;
        if (x < e->x || y < e->y || x >= e->x + e->w || y >= e->y + e->h) continue;
        if (e->flags & XSO_WF_DESKTOP) { if (desk < 0) desk = p->id; continue; }
        if (!(e->flags & XSO_WF_MAPPED)) continue;
        if (e->zorder < bestz) { bestz = e->zorder; best = p->id; }
    }
    return best >= 0 ? best : desk;
}

EXPORT int API UwcGetWindowIdUnderCursor(void)
{
    TRACE_FN(0, 7);
    if (!g_shm) return -1;
    return id_from_index(g_shm->hdr.cursor_window);
}

EXPORT void API UwcRequestCaptureCursor(void) { TRACE_FN(0, 8); if (g_shm) __atomic_add_fetch(&g_shm->cursor.req, 1u, __ATOMIC_RELEASE); }
EXPORT UINT API UwcGetCursorX(void)      { return (UINT)g_cursor_x; }
EXPORT UINT API UwcGetCursorY(void)      { return (UINT)g_cursor_y; }
EXPORT UINT API UwcGetCursorWidth(void)  { return (UINT)g_cursor_w; }
EXPORT UINT API UwcGetCursorHeight(void) { return (UINT)g_cursor_h; }
EXPORT void API UwcSetCursorTexturePtr(ID3D11Texture2D *ptr) { TRACE_FN(ptr != NULL, 9); g_cursor_tex = ptr; }

EXPORT UINT API UwcGetScreenX(void)      { return g_shm ? (UINT)g_shm->hdr.screen_x : 0; }
EXPORT UINT API UwcGetScreenY(void)      { return g_shm ? (UINT)g_shm->hdr.screen_y : 0; }
EXPORT UINT API UwcGetScreenWidth(void)  { return g_shm ? (UINT)g_shm->hdr.screen_w : 0; }
EXPORT UINT API UwcGetScreenHeight(void) { return g_shm ? (UINT)g_shm->hdr.screen_h : 0; }

/* ------------------------------------------------------------------------ */
/* input bridge (used by the input hooks; exported for tooling)             */
/* ------------------------------------------------------------------------ */

EXPORT int API XsoPushInput(uint32_t type, int a, int b, int c, int d)
{
    if (!g_shm) return 0;
    uint32_t head = __atomic_load_n(&g_shm->hdr.input_head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&g_shm->hdr.input_tail, __ATOMIC_ACQUIRE);
    if (head - tail >= XSO_INPUT_RING - 1) return 0;
    xso_input_event *ev = &g_shm->input[head % XSO_INPUT_RING];
    ev->type = type; ev->a = a; ev->b = b; ev->c = c; ev->d = d;
    __atomic_store_n(&g_shm->hdr.input_head, head + 1, __ATOMIC_RELEASE);
    return 1;
}
