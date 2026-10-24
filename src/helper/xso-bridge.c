/*
 * xso-bridge - native X11 capture/input helper for XSOverlay running under Proton.
 *
 * Enumerates top-level X11 windows and monitors, captures their contents with
 * XComposite + MIT-SHM, grabs the cursor with XFixes and publishes everything
 * into a shared-memory file that the replacement uWindowCapture.dll maps from
 * inside the Wine process. Input events coming back from the DLL are injected
 * with XTest.
 *
 * Build: see ../../build.sh
 */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/XF86keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/XTest.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include "xso_shm.h"

/* ------------------------------------------------------------------------ */
/* globals                                                                  */
/* ------------------------------------------------------------------------ */

static Display *dpy;
static Window   root;
static int      screen;
static xso_shm *shm;
static int      shm_fd = -1;
static volatile sig_atomic_t g_quit;
static int      g_verbose;
static int      g_keep_alpha;
static int      g_dry_input;   /* log input events instead of injecting them */
static int      g_xerrors;
static int      g_has_composite, g_has_shm, g_has_fixes, g_has_randr, g_has_xtest;
static int      g_randr_event_base;
static uint32_t g_gen_counter = 1;

static Atom A_NET_CLIENT_LIST_STACKING, A_NET_WM_NAME, A_UTF8_STRING, A_NET_WM_PID,
            A_NET_WM_STATE, A_NET_WM_STATE_HIDDEN, A_NET_WM_STATE_MAX_H, A_NET_WM_STATE_MAX_V,
            A_NET_WM_STATE_SKIP_TASKBAR, A_NET_WM_WINDOW_TYPE, A_TYPE_NORMAL, A_TYPE_DIALOG,
            A_TYPE_DOCK, A_TYPE_DESKTOP, A_TYPE_UTILITY, A_TYPE_TOOLBAR, A_TYPE_MENU,
            A_TYPE_SPLASH, A_TYPE_NOTIFICATION, A_NET_FRAME_EXTENTS, A_NET_WM_ICON, A_WM_STATE,
            A_NET_ACTIVE_WINDOW, A_NET_WM_STATE_FOCUSED, A_WM_CHANGE_STATE, A_EDID;

typedef struct {
    int      owner;       /* window table index, -1 = free */
    uint32_t owner_xid;
} frame_slot_t;
static frame_slot_t g_slots[XSO_FRAME_SLOTS];

typedef struct {
    XImage *img;
    XShmSegmentInfo info;
    int w, h, depth;
    Visual *visual;
    uint32_t last_use;
} shmimg_t;
#define N_IMGS 8
static shmimg_t g_imgs[N_IMGS];

/* per table index helper state */
static struct {
    int      redirected;
    uint32_t served_req;
    uint32_t last_capture_ms;
} g_wst[XSO_MAX_WINDOWS];

static XFixesCursorImage *g_cursor_img;   /* cached */
static uint32_t g_cursor_serial_ms;

/* ------------------------------------------------------------------------ */
/* utils                                                                    */
/* ------------------------------------------------------------------------ */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void logf_(const char *fmt, ...)
{
    va_list ap;
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
    fprintf(stderr, "[xso-bridge %s] ", ts);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
#define LOG(...) logf_(__VA_ARGS__)
#define DBG(...) do { if (g_verbose) logf_(__VA_ARGS__); } while (0)

static int xerror_handler(Display *d, XErrorEvent *e)
{
    (void)d;
    g_xerrors++;
    if (g_verbose > 1) {
        char buf[256];
        XGetErrorText(dpy, e->error_code, buf, sizeof buf);
        LOG("X error: %s (req %d.%d res 0x%lx)", buf, e->request_code, e->minor_code, e->resourceid);
    }
    return 0;
}

static void on_signal(int s) { (void)s; g_quit = 1; }

static unsigned char *get_prop(Window w, Atom prop, Atom type, int *fmt, unsigned long *nitems)
{
    Atom rtype = None; int rfmt = 0; unsigned long n = 0, after = 0; unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, prop, 0, 1L << 22, False, type, &rtype, &rfmt, &n, &after, &data) != Success)
        return NULL;
    if (!data) return NULL;
    if (rtype == None || n == 0) { XFree(data); return NULL; }
    if (fmt) *fmt = rfmt;
    if (nitems) *nitems = n;
    return data;
}

/* UTF-8 -> UTF-16LE into fixed buffer; returns code units written (excl. NUL) */
static int utf8_to_utf16(const unsigned char *s, size_t len, uint16_t *out, int cap)
{
    int o = 0; size_t i = 0;
    while (i < len && s[i] && o < cap - 1) {
        uint32_t c = s[i]; int n = 0;
        if (c < 0x80) { n = 1; }
        else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 4; }
        else { i++; continue; }
        if (i + n > len) break;
        for (int k = 1; k < n; k++) c = (c << 6) | (s[i + k] & 0x3F);
        i += n;
        if (c >= 0x10000) {
            if (o >= cap - 2) break;
            c -= 0x10000;
            out[o++] = (uint16_t)(0xD800 | (c >> 10));
            out[o++] = (uint16_t)(0xDC00 | (c & 0x3FF));
        } else {
            out[o++] = (uint16_t)c;
        }
    }
    out[o] = 0;
    return o;
}

static int latin1_to_utf16(const unsigned char *s, size_t len, uint16_t *out, int cap)
{
    int o = 0;
    for (size_t i = 0; i < len && s[i] && o < cap - 1; i++) out[o++] = s[i];
    out[o] = 0;
    return o;
}

static int utf16_eq(const uint16_t *a, const uint16_t *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void ascii_to_utf16(const char *s, uint16_t *out, int cap)
{
    latin1_to_utf16((const unsigned char *)s, strlen(s), out, cap);
}

/* ------------------------------------------------------------------------ */
/* shared memory                                                            */
/* ------------------------------------------------------------------------ */

static int shm_open_file(const char *path)
{
    shm_fd = open(path, O_RDWR | O_CREAT, 0600);
    if (shm_fd < 0) { LOG("cannot open %s: %s", path, strerror(errno)); return -1; }
    if (ftruncate(shm_fd, (off_t)XSO_TOTAL_SIZE) != 0) { LOG("ftruncate failed: %s", strerror(errno)); return -1; }
    shm = mmap(NULL, XSO_TOTAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) { LOG("mmap failed: %s", strerror(errno)); shm = NULL; return -1; }
    /* reset the control area (not the frame area, which is sparse) */
    memset(shm, 0, sizeof(xso_shm));
    for (int i = 0; i < XSO_MAX_WINDOWS; i++) {
        shm->windows[i].frame_slot = -1;
        shm->windows[i].reader_buf = XSO_NO_INDEX;
    }
    shm->hdr.version = XSO_SHM_VERSION;
    shm->hdr.total_size = XSO_TOTAL_SIZE;
    shm->hdr.helper_pid = (uint32_t)getpid();
    shm->hdr.cursor_window = XSO_NO_INDEX;
    __atomic_store_n(&shm->hdr.magic, XSO_SHM_MAGIC, __ATOMIC_RELEASE);
    return 0;
}

static void punch_frame_slot(int slot)
{
#ifdef FALLOC_FL_PUNCH_HOLE
    off_t off = (off_t)(XSO_FRAME_AREA_OFFSET + (size_t)slot * 2u * XSO_FRAME_BYTES);
    if (fallocate(shm_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off, (off_t)(2u * XSO_FRAME_BYTES)) != 0)
        DBG("punch hole failed: %s", strerror(errno));
#else
    (void)slot;
#endif
}

/* ------------------------------------------------------------------------ */
/* monitors                                                                 */
/* ------------------------------------------------------------------------ */

static void parse_edid(xso_monitor *m, const uint8_t *e, int len)
{
    m->mfg_id = 0; m->product_code = 0; m->friendly[0] = 0;
    char serial[32] = "";
    if (len >= 128 && e[0] == 0x00 && e[1] == 0xFF) {
        m->mfg_id = (uint16_t)((e[8] << 8) | e[9]);
        m->product_code = (uint16_t)(e[10] | (e[11] << 8));
        for (int d = 54; d + 18 <= 126; d += 18) {
            if (e[d] == 0 && e[d + 1] == 0 && e[d + 2] == 0) {
                char buf[14]; int n = 0;
                for (int k = 5; k < 18 && e[d + k] != 0x0A; k++) buf[n++] = (char)e[d + k];
                while (n > 0 && buf[n - 1] == ' ') n--;
                buf[n] = 0;
                if (e[d + 3] == 0xFC && !m->friendly[0]) snprintf(m->friendly, sizeof m->friendly, "%s", buf);
                if (e[d + 3] == 0xFF && !serial[0]) snprintf(serial, sizeof serial, "%s", buf);
            }
        }
    }
    char mfg[4] = { (char)('A' - 1 + ((m->mfg_id >> 10) & 0x1F)),
                    (char)('A' - 1 + ((m->mfg_id >> 5) & 0x1F)),
                    (char)('A' - 1 + (m->mfg_id & 0x1F)), 0 };
    if (!m->mfg_id) strcpy(mfg, "XXX");
    if (!m->friendly[0]) snprintf(m->friendly, sizeof m->friendly, "%s", m->name);
    snprintf(m->device_path, sizeof m->device_path, "\\\\?\\DISPLAY#%s%04X#%s#%s",
             mfg, m->product_code, serial[0] ? serial : "0", m->name);
}

static void update_monitors(void)
{
    xso_monitor tmp[XSO_MAX_MONITORS];
    int n = 0;
    memset(tmp, 0, sizeof tmp);

    if (g_has_randr) {
        XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
        RROutput primary = XRRGetOutputPrimary(dpy, root);
        if (res) {
            for (int i = 0; i < res->noutput && n < XSO_MAX_MONITORS; i++) {
                XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
                if (!oi) continue;
                if (oi->connection == RR_Connected && oi->crtc) {
                    XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
                    if (ci && ci->width > 0 && ci->height > 0) {
                        xso_monitor *m = &tmp[n];
                        m->x = ci->x; m->y = ci->y; m->w = (int)ci->width; m->h = (int)ci->height;
                        m->primary = (res->outputs[i] == primary);
                        m->uid = (uint32_t)res->outputs[i];
                        snprintf(m->name, sizeof m->name, "%s", oi->name);
                        /* EDID */
                        Atom actual; int fmt; unsigned long nitems, after; unsigned char *data = NULL;
                        if (XRRGetOutputProperty(dpy, res->outputs[i], A_EDID, 0, 256, False, False,
                                                 AnyPropertyType, &actual, &fmt, &nitems, &after, &data) == Success && data) {
                            int len = (int)nitems; if (len > (int)sizeof m->edid) len = sizeof m->edid;
                            memcpy(m->edid, data, (size_t)len); m->edid_len = len;
                            XFree(data);
                        }
                        parse_edid(m, m->edid, m->edid_len);
                        n++;
                    }
                    if (ci) XRRFreeCrtcInfo(ci);
                }
                XRRFreeOutputInfo(oi);
            }
            XRRFreeScreenResources(res);
        }
    }
    if (n == 0) {
        xso_monitor *m = &tmp[0];
        m->x = 0; m->y = 0; m->w = DisplayWidth(dpy, screen); m->h = DisplayHeight(dpy, screen);
        m->primary = 1; m->uid = 1; strcpy(m->name, "SCREEN"); parse_edid(m, NULL, 0);
        n = 1;
    }
    /* primary first, keep RandR order otherwise */
    for (int i = 1; i < n; i++) {
        if (tmp[i].primary && !tmp[0].primary) { xso_monitor t = tmp[0]; tmp[0] = tmp[i]; tmp[i] = t; break; }
    }
    if (!tmp[0].primary) tmp[0].primary = 1;

    int changed = (n != (int)shm->hdr.n_monitors) || memcmp(tmp, shm->monitors, sizeof(xso_monitor) * (size_t)n) != 0;
    if (changed) {
        memcpy(shm->monitors, tmp, sizeof tmp);
        shm->hdr.n_monitors = (uint32_t)n;
        shm->hdr.screen_x = 0; shm->hdr.screen_y = 0;
        shm->hdr.screen_w = DisplayWidth(dpy, screen); shm->hdr.screen_h = DisplayHeight(dpy, screen);
        LOG("monitors: %d (screen %dx%d)", n, shm->hdr.screen_w, shm->hdr.screen_h);
        for (int i = 0; i < n; i++)
            LOG("  [%d] %s %dx%d+%d+%d %s %s", i, tmp[i].name, tmp[i].w, tmp[i].h, tmp[i].x, tmp[i].y,
                tmp[i].primary ? "primary" : "", tmp[i].friendly);
    }
}

static int monitor_for_point(int x, int y)
{
    for (int i = 0; i < (int)shm->hdr.n_monitors; i++) {
        const xso_monitor *m = &shm->monitors[i];
        if (x >= m->x && y >= m->y && x < m->x + m->w && y < m->y + m->h) return i;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* window enumeration                                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint32_t xid;
    int x, y, w, h;          /* client rect */
    int fx, fy, fw, fh;      /* frame rect */
    uint32_t flags;
    uint32_t pid, owner;
    uint16_t title[XSO_TITLE_LEN];
    char klass[XSO_CLASS_LEN];
} winfo_t;

static int has_state(Atom *states, unsigned long n, Atom a)
{
    for (unsigned long i = 0; i < n; i++) if (states[i] == a) return 1;
    return 0;
}

static int gather_window(Window w, winfo_t *o)
{
    XWindowAttributes a;
    memset(o, 0, sizeof *o);
    int before = g_xerrors;
    if (!XGetWindowAttributes(dpy, w, &a)) return 0;
    if (g_xerrors != before) return 0;
    if (a.class == InputOnly) return 0;

    int ax = 0, ay = 0; Window child;
    XTranslateCoordinates(dpy, w, root, 0, 0, &ax, &ay, &child);
    o->xid = (uint32_t)w;
    o->x = ax; o->y = ay; o->w = a.width; o->h = a.height;
    o->fx = ax; o->fy = ay; o->fw = a.width; o->fh = a.height;

    unsigned long n; int fmt; unsigned char *d;
    if ((d = get_prop(w, A_NET_FRAME_EXTENTS, XA_CARDINAL, &fmt, &n)) != NULL) {
        if (n >= 4 && fmt == 32) {
            long *v = (long *)d;
            o->fx = ax - (int)v[0]; o->fw = a.width + (int)v[0] + (int)v[1];
            o->fy = ay - (int)v[2]; o->fh = a.height + (int)v[2] + (int)v[3];
        }
        XFree(d);
    }

    int mapped = (a.map_state == IsViewable);
    int iconic = 0, hidden = 0, maxh = 0, maxv = 0, skip = 0;
    if ((d = get_prop(w, A_WM_STATE, A_WM_STATE, &fmt, &n)) != NULL) {
        if (n >= 1 && fmt == 32) iconic = (((long *)d)[0] == IconicState);
        XFree(d);
    }
    if ((d = get_prop(w, A_NET_WM_STATE, XA_ATOM, &fmt, &n)) != NULL) {
        Atom *st = (Atom *)d;
        hidden = has_state(st, n, A_NET_WM_STATE_HIDDEN);
        maxh = has_state(st, n, A_NET_WM_STATE_MAX_H);
        maxv = has_state(st, n, A_NET_WM_STATE_MAX_V);
        skip = has_state(st, n, A_NET_WM_STATE_SKIP_TASKBAR);
        XFree(d);
    }
    if (hidden) iconic = 1;
    if (!mapped && !iconic) return 0;   /* e.g. on another workspace */

    int alttab = !skip;
    if ((d = get_prop(w, A_NET_WM_WINDOW_TYPE, XA_ATOM, &fmt, &n)) != NULL) {
        Atom t = ((Atom *)d)[0];
        if (t == A_TYPE_DOCK || t == A_TYPE_DESKTOP || t == A_TYPE_UTILITY || t == A_TYPE_TOOLBAR ||
            t == A_TYPE_MENU || t == A_TYPE_SPLASH || t == A_TYPE_NOTIFICATION)
            alttab = 0;
        XFree(d);
    }

    o->flags = XSO_WF_VISIBLE | (mapped ? XSO_WF_MAPPED : 0) | (iconic ? XSO_WF_ICONIC : 0) |
               ((maxh && maxv) ? XSO_WF_ZOOMED : 0) | (alttab ? XSO_WF_ALTTAB : 0);

    /* title */
    if ((d = get_prop(w, A_NET_WM_NAME, A_UTF8_STRING, &fmt, &n)) != NULL) {
        utf8_to_utf16(d, n, o->title, XSO_TITLE_LEN);
        XFree(d);
    } else if ((d = get_prop(w, XA_WM_NAME, AnyPropertyType, &fmt, &n)) != NULL) {
        latin1_to_utf16(d, n, o->title, XSO_TITLE_LEN);
        XFree(d);
    }

    XClassHint ch;
    memset(&ch, 0, sizeof ch);
    if (XGetClassHint(dpy, w, &ch)) {
        if (ch.res_class) snprintf(o->klass, sizeof o->klass, "%s", ch.res_class);
        else if (ch.res_name) snprintf(o->klass, sizeof o->klass, "%s", ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
        if (ch.res_name) XFree(ch.res_name);
    }

    if ((d = get_prop(w, A_NET_WM_PID, XA_CARDINAL, &fmt, &n)) != NULL) {
        o->pid = (uint32_t)((long *)d)[0];
        XFree(d);
    }
    Window tf = 0;
    if (XGetTransientForHint(dpy, w, &tf)) o->owner = (uint32_t)tf;
    return 1;
}

/* XSOverlay only lists windows/desktops once they have an icon, so synthesize one
   for X11 windows without _NET_WM_ICON and for monitor pseudo-windows. */
static void make_default_icon(uint32_t *dst, int size, int desktop)
{
    const uint32_t bg = 0xFF2B2F36u, frame = 0xFF8A94A6u, accent = 0xFF14B8A6u, dark = 0xFF1A1D22u;
    for (int i = 0; i < size * size; i++) dst[i] = 0x00000000u;
    int m = size / 8;
    if (desktop) {
        /* monitor: screen rectangle + stand */
        int top = m, bottom = size - 3 * m, left = m / 2, right = size - m / 2;
        for (int y = top; y < bottom; y++)
            for (int x = left; x < right; x++) {
                int border = (y < top + m / 2 || y >= bottom - m / 2 || x < left + m / 2 || x >= right - m / 2);
                dst[y * size + x] = border ? frame : accent;
            }
        for (int y = bottom; y < bottom + m; y++)
            for (int x = size / 2 - m / 2; x < size / 2 + m / 2; x++) dst[y * size + x] = frame;
        for (int y = bottom + m; y < bottom + m + m / 2 + 1 && y < size; y++)
            for (int x = size / 4; x < size - size / 4; x++) dst[y * size + x] = frame;
    } else {
        /* generic window: title bar + body */
        int top = m, bottom = size - m, left = m, right = size - m;
        for (int y = top; y < bottom; y++)
            for (int x = left; x < right; x++) {
                int border = (y < top + 1 || y >= bottom - 1 || x < left + 1 || x >= right - 1);
                int titlebar = y < top + 2 * m;
                dst[y * size + x] = border ? frame : (titlebar ? dark : bg);
            }
        for (int k = 0; k < 3; k++)
            for (int y = top + m / 2 + 1; y < top + m / 2 + m; y++)
                for (int x = left + m + k * (m + m / 2); x < left + m + k * (m + m / 2) + m / 2 + 1; x++)
                    if (x < right - 1) dst[y * size + x] = accent;
    }
}

/* pick the best _NET_WM_ICON (<= XSO_ICON_DIM, largest) and store BGRA */
static void fetch_icon(Window w, int idx)
{
    xso_window *e = &shm->windows[idx];
    unsigned long n; int fmt;
    unsigned char *d = get_prop(w, A_NET_WM_ICON, XA_CARDINAL, &fmt, &n);
    e->icon_w = e->icon_h = 0;
    if (!d) { make_default_icon(shm->icons[idx], 48, 0); e->icon_w = e->icon_h = 48; e->icon_gen++; return; }
    long *v = (long *)d;
    unsigned long i = 0; long bw = 0, bh = 0; const long *best = NULL; long bestscore = -1;
    while (i + 2 <= n) {
        long iw = v[i], ih = v[i + 1];
        if (iw <= 0 || ih <= 0 || i + 2 + (unsigned long)(iw * ih) > n) break;
        long score;
        if (iw <= XSO_ICON_DIM && ih <= XSO_ICON_DIM) score = 100000 + iw * ih;    /* prefer largest that fits */
        else score = 100000 - (iw * ih) / 64;                                       /* else smallest big one */
        if (score > bestscore) { bestscore = score; best = &v[i + 2]; bw = iw; bh = ih; }
        i += 2 + (unsigned long)(iw * ih);
    }
    if (best) {
        uint32_t *dst = shm->icons[idx];
        if (bw <= XSO_ICON_DIM && bh <= XSO_ICON_DIM) {
            for (long y = 0; y < bh; y++)
                for (long x = 0; x < bw; x++)
                    dst[y * bw + x] = (uint32_t)best[y * bw + x];
            e->icon_w = (int32_t)bw; e->icon_h = (int32_t)bh;
        } else {
            /* nearest-neighbour downscale to fit */
            long ow = bw, oh = bh;
            if (ow > XSO_ICON_DIM) { oh = oh * XSO_ICON_DIM / ow; ow = XSO_ICON_DIM; }
            if (oh > XSO_ICON_DIM) { ow = ow * XSO_ICON_DIM / oh; oh = XSO_ICON_DIM; }
            if (ow < 1) ow = 1; if (oh < 1) oh = 1;
            for (long y = 0; y < oh; y++)
                for (long x = 0; x < ow; x++)
                    dst[y * ow + x] = (uint32_t)best[(y * bh / oh) * bw + (x * bw / ow)];
            e->icon_w = (int32_t)ow; e->icon_h = (int32_t)oh;
        }
    }
    XFree(d);
    if (!e->icon_w) { make_default_icon(shm->icons[idx], 48, 0); e->icon_w = e->icon_h = 48; }
    e->icon_gen++;
}

static int find_table_index(uint32_t xid)
{
    for (int i = 0; i < XSO_MAX_WINDOWS; i++) if (shm->windows[i].xid == xid) return i;
    return -1;
}

static void release_slot(int idx);

static void remove_window(int idx)
{
    xso_window *e = &shm->windows[idx];
    DBG("remove window 0x%x (%d)", e->xid, idx);
    release_slot(idx);
    if (g_wst[idx].redirected && !(e->flags & XSO_WF_DESKTOP)) {
        XCompositeUnredirectWindow(dpy, e->xid, CompositeRedirectAutomatic);
        g_wst[idx].redirected = 0;
    }
    __atomic_store_n(&e->xid, 0u, __ATOMIC_RELEASE);
    e->flags = 0;
    memset(&g_wst[idx], 0, sizeof g_wst[idx]);
}

static int add_window(uint32_t xid)
{
    for (int i = 0; i < XSO_MAX_WINDOWS; i++) {
        xso_window *e = &shm->windows[i];
        if (e->xid == 0) {
            memset(e, 0, sizeof *e);
            e->gen = g_gen_counter++;
            if (g_gen_counter == 0) g_gen_counter = 1;
            e->frame_slot = -1;
            e->reader_buf = XSO_NO_INDEX;
            e->title_gen = 1;
            memset(&g_wst[i], 0, sizeof g_wst[i]);
            __atomic_store_n(&e->xid, xid, __ATOMIC_RELEASE);
            return i;
        }
    }
    return -1;
}

static void enumerate_windows(void)
{
    unsigned long n = 0; int fmt = 0;
    unsigned char *d = get_prop(root, A_NET_CLIENT_LIST_STACKING, XA_WINDOW, &fmt, &n);
    Window *list = (Window *)d;
    static winfo_t infos[XSO_MAX_WINDOWS];
    int cnt = 0;
    int changed = 0;

    /* top-most last in the list; gather from top so zorder = index from top among mapped */
    uint32_t z = 0;
    for (long i = (long)n - 1; i >= 0 && cnt < XSO_MAX_WINDOWS - XSO_MAX_MONITORS; i--) {
        winfo_t *o = &infos[cnt];
        if (!gather_window(list[i], o)) continue;
        cnt++;
        /* zorder assigned below */
    }
    if (d) XFree(d);

    /* mark & update */
    static unsigned char present[XSO_MAX_WINDOWS];
    memset(present, 0, sizeof present);

    for (int k = 0; k < cnt; k++) {
        winfo_t *o = &infos[k];
        int idx = find_table_index(o->xid);
        int isnew = 0;
        if (idx < 0) {
            idx = add_window(o->xid);
            if (idx < 0) break;
            isnew = 1;
        }
        xso_window *e = &shm->windows[idx];
        present[idx] = 1;
        e->flags = o->flags;
        e->x = o->fx; e->y = o->fy; e->w = o->fw; e->h = o->fh;
        /* XSOverlay maps clicks using the window rect; we capture the client area, so
           report the client rect as the window rect and keep offsets at zero. */
        e->x = o->x; e->y = o->y; e->w = o->w; e->h = o->h;
        e->cx = o->x; e->cy = o->y; e->cw = o->w; e->ch = o->h;
        e->zorder = (o->flags & XSO_WF_MAPPED) ? z++ : 0xFFFF;
        e->pid = o->pid;
        e->owner_xid = o->owner;
        e->monitor = monitor_for_point(o->x + o->w / 2, o->y + o->h / 2);
        if (isnew || !utf16_eq(e->title, o->title)) {
            memcpy(e->title, o->title, sizeof e->title);
            e->title_gen++;
        }
        if (strcmp(e->klass, o->klass) != 0) snprintf(e->klass, sizeof e->klass, "%s", o->klass);
        if (isnew) {
            fetch_icon(o->xid, idx);
            changed = 1;
            char t[64]; int j;
            for (j = 0; j < 63 && o->title[j]; j++) t[j] = (o->title[j] < 128) ? (char)o->title[j] : '?';
            t[j] = 0;
            DBG("add window 0x%x (%d) '%s' class=%s %dx%d+%d+%d flags=%x", o->xid, idx, t, o->klass, o->w, o->h, o->x, o->y, o->flags);
        }
    }

    /* desktops */
    for (int m = 0; m < (int)shm->hdr.n_monitors; m++) {
        uint32_t xid = XSO_DESKTOP_XID(m);
        int idx = find_table_index(xid);
        int isnew = 0;
        if (idx < 0) { idx = add_window(xid); if (idx < 0) break; isnew = 1; }
        xso_window *e = &shm->windows[idx];
        const xso_monitor *mon = &shm->monitors[m];
        present[idx] = 1;
        e->flags = XSO_WF_DESKTOP | XSO_WF_VISIBLE | XSO_WF_MAPPED | (mon->primary ? XSO_WF_PRIMARY : 0);
        e->x = e->cx = mon->x; e->y = e->cy = mon->y; e->w = e->cw = mon->w; e->h = e->ch = mon->h;
        e->zorder = 0xFFFF; e->monitor = m; e->pid = 0; e->owner_xid = 0;
        char name[64];
        snprintf(name, sizeof name, "\\\\.\\DISPLAY%d", m + 1);
        uint16_t t16[64];
        ascii_to_utf16(name, t16, 64);
        if (isnew || !utf16_eq(e->title, t16)) { memset(e->title, 0, sizeof e->title); memcpy(e->title, t16, sizeof t16); e->title_gen++; }
        e->klass[0] = 0;
        if (isnew) { make_default_icon(shm->icons[idx], 64, 1); e->icon_w = e->icon_h = 64; e->icon_gen++; changed = 1; DBG("add desktop %d (%d)", m, idx); }
    }

    for (int i = 0; i < XSO_MAX_WINDOWS; i++) {
        if (shm->windows[i].xid && !present[i]) { remove_window(i); changed = 1; }
    }
    if (changed) __atomic_add_fetch(&shm->hdr.list_gen, 1u, __ATOMIC_RELEASE);

    /* window under the pointer */
    Window rr, cr; int rx = 0, ry = 0, wx, wy; unsigned int mask;
    if (XQueryPointer(dpy, root, &rr, &cr, &rx, &ry, &wx, &wy, &mask)) {
        int best = -1; uint32_t bestz = 0xFFFFFFFFu;
        for (int i = 0; i < XSO_MAX_WINDOWS; i++) {
            xso_window *e = &shm->windows[i];
            if (!e->xid || (e->flags & XSO_WF_DESKTOP) || !(e->flags & XSO_WF_MAPPED)) continue;
            if (rx >= e->x && ry >= e->y && rx < e->x + e->w && ry < e->y + e->h && e->zorder < bestz) { best = i; bestz = e->zorder; }
        }
        if (best < 0) {
            int m = monitor_for_point(rx, ry);
            best = find_table_index(XSO_DESKTOP_XID(m));
        }
        shm->hdr.cursor_window = (best >= 0) ? (uint32_t)best : XSO_NO_INDEX;
        shm->hdr.pointer_x = rx; shm->hdr.pointer_y = ry;
    }
}

/* ------------------------------------------------------------------------ */
/* XShm images                                                              */
/* ------------------------------------------------------------------------ */

static void free_img(shmimg_t *s)
{
    if (!s->img) return;
    XShmDetach(dpy, &s->info);
    XDestroyImage(s->img);
    shmdt(s->info.shmaddr);
    memset(s, 0, sizeof *s);
}

static shmimg_t *get_img(int w, int h, int depth, Visual *visual)
{
    shmimg_t *lru = NULL;
    for (int i = 0; i < N_IMGS; i++) {
        shmimg_t *s = &g_imgs[i];
        if (s->img && s->w == w && s->h == h && s->depth == depth && s->visual == visual) { s->last_use = now_ms(); return s; }
        if (!lru || !s->img || s->last_use < lru->last_use) { if (!lru || !s->img || (lru->img && s->last_use < lru->last_use)) lru = s; }
    }
    free_img(lru);
    XImage *img = XShmCreateImage(dpy, visual, (unsigned)depth, ZPixmap, NULL, &lru->info, (unsigned)w, (unsigned)h);
    if (!img) { LOG("XShmCreateImage failed %dx%d depth %d", w, h, depth); return NULL; }
    size_t size = (size_t)img->bytes_per_line * (size_t)img->height;
    lru->info.shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
    if (lru->info.shmid < 0) { LOG("shmget failed: %s", strerror(errno)); XDestroyImage(img); return NULL; }
    lru->info.shmaddr = img->data = shmat(lru->info.shmid, NULL, 0);
    lru->info.readOnly = False;
    if (!XShmAttach(dpy, &lru->info)) { LOG("XShmAttach failed"); }
    XSync(dpy, False);
    shmctl(lru->info.shmid, IPC_RMID, NULL);
    lru->img = img; lru->w = w; lru->h = h; lru->depth = depth; lru->visual = visual; lru->last_use = now_ms();
    return lru;
}

/* ------------------------------------------------------------------------ */
/* frame slots & publishing                                                 */
/* ------------------------------------------------------------------------ */

static void release_slot(int idx)
{
    xso_window *e = &shm->windows[idx];
    int s = e->frame_slot;
    if (s < 0) return;
    e->frame_slot = -1;
    g_slots[s].owner = -1; g_slots[s].owner_xid = 0;
    punch_frame_slot(s);
}

static int ensure_slot(int idx)
{
    xso_window *e = &shm->windows[idx];
    if (e->frame_slot >= 0 && g_slots[e->frame_slot].owner == idx) return e->frame_slot;
    for (int s = 0; s < XSO_FRAME_SLOTS; s++) {
        if (g_slots[s].owner < 0) { g_slots[s].owner = idx; g_slots[s].owner_xid = e->xid; e->frame_slot = s; return s; }
    }
    /* evict the least recently requested */
    int victim = -1; uint32_t oldest = 0xFFFFFFFFu;
    for (int s = 0; s < XSO_FRAME_SLOTS; s++) {
        int o = g_slots[s].owner;
        uint32_t t = shm->windows[o].last_req_ms;
        if (t < oldest) { oldest = t; victim = s; }
    }
    if (victim >= 0 && now_ms() - oldest > 1000) {
        release_slot(g_slots[victim].owner);
        g_slots[victim].owner = idx; g_slots[victim].owner_xid = e->xid; e->frame_slot = victim;
        return victim;
    }
    return -1;
}

static void refresh_cursor_image(void)
{
    if (!g_has_fixes) return;
    uint32_t t = now_ms();
    if (g_cursor_img && t - g_cursor_serial_ms < 30) return;
    XFixesCursorImage *ci = XFixesGetCursorImage(dpy);
    if (!ci) return;
    if (g_cursor_img) XFree(g_cursor_img);
    g_cursor_img = ci;
    g_cursor_serial_ms = t;
}

/* blend the (premultiplied ARGB) cursor into a BGRA frame whose origin is (ox,oy) on screen */
static void draw_cursor_into(uint32_t *frame, int fw, int fh, int ox, int oy)
{
    refresh_cursor_image();
    XFixesCursorImage *ci = g_cursor_img;
    if (!ci) return;
    int cx = ci->x - ci->xhot - ox, cy = ci->y - ci->yhot - oy;
    for (int y = 0; y < (int)ci->height; y++) {
        int fy = cy + y; if (fy < 0 || fy >= fh) continue;
        for (int x = 0; x < (int)ci->width; x++) {
            int fx = cx + x; if (fx < 0 || fx >= fw) continue;
            uint32_t s = (uint32_t)ci->pixels[y * ci->width + x];
            uint32_t sa = s >> 24; if (!sa) continue;
            uint32_t *dp = &frame[fy * fw + fx];
            uint32_t dcol = *dp;
            uint32_t inv = 255 - sa;
            uint32_t r = ((s >> 16) & 0xFF) + (((dcol >> 16) & 0xFF) * inv) / 255;
            uint32_t g = ((s >> 8) & 0xFF) + (((dcol >> 8) & 0xFF) * inv) / 255;
            uint32_t b = (s & 0xFF) + ((dcol & 0xFF) * inv) / 255;
            if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
            *dp = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

static int publish_frame(int idx, XImage *img, int w, int h, int ox, int oy)
{
    xso_window *e = &shm->windows[idx];
    int slot = e->frame_slot;
    if (slot < 0) return 0;
    uint32_t half = __atomic_load_n(&e->frame_buf, __ATOMIC_ACQUIRE) ^ 1u;
    if (__atomic_load_n(&e->reader_buf, __ATOMIC_ACQUIRE) == half) return 0;   /* reader busy with it; retry later */
    if (w > XSO_FRAME_MAX_W) w = XSO_FRAME_MAX_W;
    if (h > XSO_FRAME_MAX_H) h = XSO_FRAME_MAX_H;
    uint32_t *dst = (uint32_t *)XSO_FRAME_PTR(shm, slot, half);
    int stride = w * 4;
    if (img->bits_per_pixel == 32) {
        const uint32_t amask = g_keep_alpha && img->depth == 32 ? 0u : 0xFF000000u;
        for (int y = 0; y < h; y++) {
            const uint32_t *src = (const uint32_t *)(img->data + (size_t)y * (size_t)img->bytes_per_line);
            uint32_t *d = dst + (size_t)y * (size_t)w;
            for (int x = 0; x < w; x++) d[x] = src[x] | amask;
        }
    } else {
        for (int y = 0; y < h; y++) {
            uint32_t *d = dst + (size_t)y * (size_t)w;
            for (int x = 0; x < w; x++) d[x] = 0xFF000000u | (uint32_t)XGetPixel(img, x, y);
        }
    }
    if (e->draw_cursor) draw_cursor_into(dst, w, h, ox, oy);
    e->frame_w = w; e->frame_h = h; e->frame_stride = stride;
    __atomic_store_n(&e->frame_buf, half, __ATOMIC_RELEASE);
    __atomic_add_fetch(&e->frame_seq, 1u, __ATOMIC_RELEASE);
    return 1;
}

static int capture_desktop(int idx)
{
    xso_window *e = &shm->windows[idx];
    const xso_monitor *m = &shm->monitors[e->monitor];
    if (ensure_slot(idx) < 0) return 0;
    shmimg_t *si = get_img(m->w, m->h, DefaultDepth(dpy, screen), DefaultVisual(dpy, screen));
    if (!si) return 0;
    int before = g_xerrors;
    uint64_t t0 = now_us();
    if (!XShmGetImage(dpy, root, si->img, m->x, m->y, AllPlanes) || g_xerrors != before) { DBG("desktop XShmGetImage failed"); return 0; }
    uint64_t t1 = now_us();
    int r = publish_frame(idx, si->img, m->w, m->h, m->x, m->y);
    if (g_verbose >= 2) LOG("desktop capture: getimage %llu us, publish %llu us (%s)", (unsigned long long)(t1 - t0), (unsigned long long)(now_us() - t1), r ? "ok" : "skipped");
    return r;
}

static int capture_window(int idx)
{
    xso_window *e = &shm->windows[idx];
    if (e->flags & XSO_WF_DESKTOP) return capture_desktop(idx);
    XWindowAttributes a;
    int before = g_xerrors;
    if (!XGetWindowAttributes(dpy, e->xid, &a) || g_xerrors != before) return 0;
    if (a.map_state != IsViewable || a.width <= 0 || a.height <= 0) return 0;
    if (ensure_slot(idx) < 0) return 0;

    int ok = 0;
    if (g_has_composite) {
        if (!g_wst[idx].redirected) {
            XCompositeRedirectWindow(dpy, e->xid, CompositeRedirectAutomatic);
            XSync(dpy, False);
            g_wst[idx].redirected = 1;
            DBG("redirected 0x%x", e->xid);
        }
        before = g_xerrors;
        Pixmap pm = XCompositeNameWindowPixmap(dpy, e->xid);
        XSync(dpy, False);
        if (pm && g_xerrors == before) {
            shmimg_t *si = get_img(a.width, a.height, a.depth, a.visual);
            if (si) {
                before = g_xerrors;
                uint64_t t0 = now_us();
                if (XShmGetImage(dpy, pm, si->img, 0, 0, AllPlanes) && g_xerrors == before) {
                    uint64_t t1 = now_us();
                    ok = publish_frame(idx, si->img, a.width, a.height, e->x, e->y);
                    if (g_verbose >= 2) LOG("window 0x%x capture: getimage %llu us, publish %llu us (%s)", e->xid, (unsigned long long)(t1 - t0), (unsigned long long)(now_us() - t1), ok ? "ok" : "skipped");
                }
            }
            XFreePixmap(dpy, pm);
        }
    }
    if (!ok) {
        /* fallback: whatever is on screen at the window rect */
        int x = e->x, y = e->y, w = a.width, h = a.height;
        int sw = shm->hdr.screen_w, sh = shm->hdr.screen_h;
        if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
        if (x + w > sw) w = sw - x; if (y + h > sh) h = sh - y;
        if (w <= 0 || h <= 0) return 0;
        shmimg_t *si = get_img(w, h, DefaultDepth(dpy, screen), DefaultVisual(dpy, screen));
        if (!si) return 0;
        before = g_xerrors;
        if (XShmGetImage(dpy, root, si->img, x, y, AllPlanes) && g_xerrors == before)
            ok = publish_frame(idx, si->img, w, h, x, y);
    }
    return ok;
}

static void capture_cursor(void)
{
    xso_cursor *c = &shm->cursor;
    if (!g_has_fixes) { c->w = c->h = 0; __atomic_add_fetch(&c->seq, 1u, __ATOMIC_RELEASE); return; }
    XFixesCursorImage *ci = XFixesGetCursorImage(dpy);
    if (!ci) return;
    int w = ci->width, h = ci->height;
    if (w > XSO_CURSOR_DIM) w = XSO_CURSOR_DIM;
    if (h > XSO_CURSOR_DIM) h = XSO_CURSOR_DIM;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = (uint32_t)ci->pixels[y * ci->width + x];
            uint32_t a = p >> 24;
            uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            if (a && a < 255) { r = r * 255 / a; g = g * 255 / a; b = b * 255 / a; if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255; }
            c->pixels[y * w + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    c->x = ci->x; c->y = ci->y; c->hot_x = ci->xhot; c->hot_y = ci->yhot;
    c->w = w; c->h = h; c->visible = 1;
    XFree(ci);
    __atomic_add_fetch(&c->seq, 1u, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------------ */
/* input injection                                                          */
/* ------------------------------------------------------------------------ */

static KeySym vk_to_keysym(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return (KeySym)(XK_a + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return (KeySym)(XK_0 + (vk - '0'));
    if (vk >= 0x70 && vk <= 0x87) return (KeySym)(XK_F1 + (vk - 0x70));
    if (vk >= 0x60 && vk <= 0x69) return (KeySym)(XK_KP_0 + (vk - 0x60));
    switch (vk) {
    case 0x08: return XK_BackSpace; case 0x09: return XK_Tab; case 0x0D: return XK_Return;
    case 0x10: return XK_Shift_L; case 0x11: return XK_Control_L; case 0x12: return XK_Alt_L;
    case 0xA0: return XK_Shift_L; case 0xA1: return XK_Shift_R; case 0xA2: return XK_Control_L; case 0xA3: return XK_Control_R;
    case 0xA4: return XK_Alt_L; case 0xA5: return XK_Alt_R;
    case 0x13: return XK_Pause; case 0x14: return XK_Caps_Lock; case 0x1B: return XK_Escape; case 0x20: return XK_space;
    case 0x21: return XK_Page_Up; case 0x22: return XK_Page_Down; case 0x23: return XK_End; case 0x24: return XK_Home;
    case 0x25: return XK_Left; case 0x26: return XK_Up; case 0x27: return XK_Right; case 0x28: return XK_Down;
    case 0x2C: return XK_Print; case 0x2D: return XK_Insert; case 0x2E: return XK_Delete;
    case 0x5B: return XK_Super_L; case 0x5C: return XK_Super_R; case 0x5D: return XK_Menu;
    case 0x6A: return XK_KP_Multiply; case 0x6B: return XK_KP_Add; case 0x6D: return XK_KP_Subtract;
    case 0x6E: return XK_KP_Decimal; case 0x6F: return XK_KP_Divide;
    case 0x90: return XK_Num_Lock; case 0x91: return XK_Scroll_Lock;
    case 0xBA: return XK_semicolon; case 0xBB: return XK_equal; case 0xBC: return XK_comma; case 0xBD: return XK_minus;
    case 0xBE: return XK_period; case 0xBF: return XK_slash; case 0xC0: return XK_grave;
    case 0xDB: return XK_bracketleft; case 0xDC: return XK_backslash; case 0xDD: return XK_bracketright; case 0xDE: return XK_apostrophe;
    case 0xE2: return XK_less;
    case 0xAD: return XF86XK_AudioMute; case 0xAE: return XF86XK_AudioLowerVolume; case 0xAF: return XF86XK_AudioRaiseVolume;
    case 0xB0: return XF86XK_AudioNext; case 0xB1: return XF86XK_AudioPrev; case 0xB2: return XF86XK_AudioStop; case 0xB3: return XF86XK_AudioPlay;
    default: return NoSymbol;
    }
}

/* find a keycode producing keysym (any level); returns 0 if none */
static KeyCode keycode_for_keysym(KeySym ks, int *need_shift)
{
    *need_shift = 0;
    if (ks == NoSymbol) return 0;
    int lo, hi; XDisplayKeycodes(dpy, &lo, &hi);
    int per; KeySym *map = XGetKeyboardMapping(dpy, (KeyCode)lo, hi - lo + 1, &per);
    if (!map) return 0;
    KeyCode found = 0;
    for (int kc = lo; kc <= hi && !found; kc++) {
        for (int l = 0; l < per && l < 4; l++) {
            if (map[(kc - lo) * per + l] == ks) { found = (KeyCode)kc; *need_shift = (l & 1); break; }
        }
    }
    XFree(map);
    return found;
}

static KeyCode g_spare_keycode;

static void inject_keysym(KeySym ks, int pressed)
{
    int shift = 0;
    KeyCode kc = keycode_for_keysym(ks, &shift);
    if (!kc) {
        /* temporarily bind a spare keycode */
        if (!g_spare_keycode) {
            int lo, hi; XDisplayKeycodes(dpy, &lo, &hi);
            int per; KeySym *map = XGetKeyboardMapping(dpy, (KeyCode)lo, hi - lo + 1, &per);
            if (map) {
                for (int k = hi; k >= lo; k--) {
                    int empty = 1;
                    for (int l = 0; l < per; l++) if (map[(k - lo) * per + l] != NoSymbol) { empty = 0; break; }
                    if (empty) { g_spare_keycode = (KeyCode)k; break; }
                }
                XFree(map);
            }
        }
        if (!g_spare_keycode) return;
        KeySym syms[2] = { ks, ks };
        XChangeKeyboardMapping(dpy, g_spare_keycode, 2, syms, 1);
        XSync(dpy, False);
        kc = g_spare_keycode;
    }
    if (shift && pressed) XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Shift_L), True, 0);
    XTestFakeKeyEvent(dpy, kc, pressed ? True : False, 0);
    if (shift && !pressed) XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Shift_L), False, 0);
}

static void activate_window(Window w)
{
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = A_NET_ACTIVE_WINDOW;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 2;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

static void process_input(void)
{
    if (!g_has_xtest) { shm->hdr.input_tail = shm->hdr.input_head; return; }
    uint32_t head = __atomic_load_n(&shm->hdr.input_head, __ATOMIC_ACQUIRE);
    uint32_t tail = shm->hdr.input_tail;
    int any = 0;
    while (tail != head) {
        xso_input_event ev = shm->input[tail % XSO_INPUT_RING];
        tail++;
        any = 1;
        if (g_dry_input || g_verbose >= 2) LOG("input: type=%u a=%d b=%d c=%d d=%d", ev.type, ev.a, ev.b, ev.c, ev.d);
        if (g_dry_input) continue;
        switch (ev.type) {
        case XSO_IN_MOUSE_MOVE_ABS: XTestFakeMotionEvent(dpy, -1, ev.a, ev.b, 0); break;
        case XSO_IN_MOUSE_MOVE_REL: XTestFakeRelativeMotionEvent(dpy, ev.a, ev.b, 0); break;
        case XSO_IN_MOUSE_BUTTON: if (ev.a >= 1 && ev.a <= 9) XTestFakeButtonEvent(dpy, (unsigned)ev.a, ev.b ? True : False, 0); break;
        case XSO_IN_MOUSE_WHEEL: {
            int notches = ev.a / 120; if (notches == 0) notches = ev.a > 0 ? 1 : -1;
            unsigned btn = ev.b ? (ev.a > 0 ? 7u : 6u) : (ev.a > 0 ? 4u : 5u);
            for (int i = 0; i < abs(notches); i++) { XTestFakeButtonEvent(dpy, btn, True, 0); XTestFakeButtonEvent(dpy, btn, False, 0); }
            break; }
        case XSO_IN_KEY: {
            KeySym ks = vk_to_keysym(ev.a);
            if (ks == NoSymbol && ev.d) ks = (ev.d < 0x100) ? (KeySym)ev.d : (KeySym)(0x01000000 | ev.d);
            if (ks != NoSymbol) inject_keysym(ks, ev.b);
            break; }
        case XSO_IN_KEY_UNICODE: {
            KeySym ks = (ev.a < 0x100) ? (KeySym)ev.a : (KeySym)(0x01000000 | ev.a);
            inject_keysym(ks, ev.b);
            break; }
        case XSO_IN_FOCUS_WINDOW: if (ev.a && !(ev.a & 0x80000000u)) activate_window((Window)(uint32_t)ev.a); break;
        case XSO_IN_SHOW_WINDOW:
            if (ev.a && !(ev.a & 0x80000000u)) {
                if (ev.b == 6 /*SW_MINIMIZE*/ || ev.b == 2 /*SW_SHOWMINIMIZED*/) XIconifyWindow(dpy, (Window)(uint32_t)ev.a, screen);
                else { XMapRaised(dpy, (Window)(uint32_t)ev.a); activate_window((Window)(uint32_t)ev.a); }
            }
            break;
        default: break;
        }
    }
    if (any) XFlush(dpy);
    __atomic_store_n(&shm->hdr.input_tail, tail, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------------ */
/* main loop                                                                */
/* ------------------------------------------------------------------------ */

static void init_atoms(void)
{
#define GA(v, s) v = XInternAtom(dpy, s, False)
    GA(A_NET_CLIENT_LIST_STACKING, "_NET_CLIENT_LIST_STACKING"); GA(A_NET_WM_NAME, "_NET_WM_NAME");
    GA(A_UTF8_STRING, "UTF8_STRING"); GA(A_NET_WM_PID, "_NET_WM_PID"); GA(A_NET_WM_STATE, "_NET_WM_STATE");
    GA(A_NET_WM_STATE_HIDDEN, "_NET_WM_STATE_HIDDEN"); GA(A_NET_WM_STATE_MAX_H, "_NET_WM_STATE_MAXIMIZED_HORZ");
    GA(A_NET_WM_STATE_MAX_V, "_NET_WM_STATE_MAXIMIZED_VERT"); GA(A_NET_WM_STATE_SKIP_TASKBAR, "_NET_WM_STATE_SKIP_TASKBAR");
    GA(A_NET_WM_WINDOW_TYPE, "_NET_WM_WINDOW_TYPE"); GA(A_TYPE_NORMAL, "_NET_WM_WINDOW_TYPE_NORMAL");
    GA(A_TYPE_DIALOG, "_NET_WM_WINDOW_TYPE_DIALOG"); GA(A_TYPE_DOCK, "_NET_WM_WINDOW_TYPE_DOCK");
    GA(A_TYPE_DESKTOP, "_NET_WM_WINDOW_TYPE_DESKTOP"); GA(A_TYPE_UTILITY, "_NET_WM_WINDOW_TYPE_UTILITY");
    GA(A_TYPE_TOOLBAR, "_NET_WM_WINDOW_TYPE_TOOLBAR"); GA(A_TYPE_MENU, "_NET_WM_WINDOW_TYPE_MENU");
    GA(A_TYPE_SPLASH, "_NET_WM_WINDOW_TYPE_SPLASH"); GA(A_TYPE_NOTIFICATION, "_NET_WM_WINDOW_TYPE_NOTIFICATION");
    GA(A_NET_FRAME_EXTENTS, "_NET_FRAME_EXTENTS"); GA(A_NET_WM_ICON, "_NET_WM_ICON"); GA(A_WM_STATE, "WM_STATE");
    GA(A_NET_ACTIVE_WINDOW, "_NET_ACTIVE_WINDOW"); GA(A_NET_WM_STATE_FOCUSED, "_NET_WM_STATE_FOCUSED");
    GA(A_WM_CHANGE_STATE, "WM_CHANGE_STATE"); GA(A_EDID, "EDID");
#undef GA
}

static void cleanup(void)
{
    if (shm) {
        for (int i = 0; i < XSO_MAX_WINDOWS; i++) {
            if (shm->windows[i].xid && g_wst[i].redirected && !(shm->windows[i].flags & XSO_WF_DESKTOP))
                XCompositeUnredirectWindow(dpy, shm->windows[i].xid, CompositeRedirectAutomatic);
        }
        __atomic_store_n(&shm->hdr.magic, 0u, __ATOMIC_RELEASE);
        munmap(shm, XSO_TOTAL_SIZE);
    }
    for (int i = 0; i < N_IMGS; i++) free_img(&g_imgs[i]);
    if (g_spare_keycode) { KeySym none = NoSymbol; XChangeKeyboardMapping(dpy, g_spare_keycode, 1, &none, 1); }
    if (dpy) { XSync(dpy, False); XCloseDisplay(dpy); }
}

int main(int argc, char **argv)
{
    const char *path = getenv("XSO_BRIDGE_SHM");
    if (!path || !*path) path = XSO_SHM_DEFAULT_PATH;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose++;
        else if (!strcmp(argv[i], "-vv")) g_verbose += 2;
        else if (!strcmp(argv[i], "--keep-alpha")) g_keep_alpha = 1;
        else if (!strcmp(argv[i], "--dry-input")) g_dry_input = 1;
        else if (!strcmp(argv[i], "--shm") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: xso-bridge [-v] [--keep-alpha] [--dry-input] [--shm PATH]\n"); return 0;
        }
    }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);

    dpy = XOpenDisplay(NULL);
    if (!dpy) { LOG("cannot open X display (DISPLAY=%s)", getenv("DISPLAY") ? getenv("DISPLAY") : "unset"); return 1; }
    XSetErrorHandler(xerror_handler);
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    init_atoms();

    int eb, erb, maj, min;
    g_has_composite = XCompositeQueryExtension(dpy, &eb, &erb) && XCompositeQueryVersion(dpy, &maj, &min);
    g_has_shm = XShmQueryExtension(dpy);
    g_has_fixes = XFixesQueryExtension(dpy, &eb, &erb);
    g_has_randr = XRRQueryExtension(dpy, &g_randr_event_base, &erb);
    g_has_xtest = XTestQueryExtension(dpy, &eb, &erb, &maj, &min);
    LOG("extensions: composite=%d shm=%d xfixes=%d randr=%d xtest=%d", g_has_composite, g_has_shm, g_has_fixes, g_has_randr, g_has_xtest);
    if (!g_has_shm) { LOG("MIT-SHM is required"); return 1; }
    if (g_has_randr) XRRSelectInput(dpy, root, RRScreenChangeNotifyMask);

    for (int s = 0; s < XSO_FRAME_SLOTS; s++) g_slots[s].owner = -1;
    if (shm_open_file(path) != 0) return 1;
    LOG("shared memory at %s (%zu MiB virtual)", path, (size_t)(XSO_TOTAL_SIZE >> 20));

    update_monitors();
    enumerate_windows();
    uint32_t last_enum = now_ms(), last_mon = last_enum, last_idle_check = last_enum, last_pe_seen = 0, last_ptr = last_enum;
    uint32_t cursor_served = shm->cursor.seq;

    while (!g_quit) {
        uint32_t t = now_ms();
        __atomic_store_n(&shm->hdr.heartbeat_ms, t, __ATOMIC_RELAXED);

        /* X events (RandR changes) */
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev);
            if (g_has_randr && ev.type == g_randr_event_base + RRScreenChangeNotify) {
                XRRUpdateConfiguration(&ev);
                update_monitors();
            }
        }
        if (t - last_mon >= 2000) { update_monitors(); last_mon = t; }
        if (t - last_enum >= 50) { enumerate_windows(); last_enum = t; }
        else if (t - last_ptr >= 8) {
            Window rr, cr; int rx, ry, wx, wy; unsigned int mask;
            if (XQueryPointer(dpy, root, &rr, &cr, &rx, &ry, &wx, &wy, &mask)) { shm->hdr.pointer_x = rx; shm->hdr.pointer_y = ry; }
            last_ptr = t;
        }

        int busy = 0;
        /* capture requests */
        for (int i = 0; i < XSO_MAX_WINDOWS; i++) {
            xso_window *e = &shm->windows[i];
            if (!e->xid) continue;
            uint32_t req = __atomic_load_n(&e->cap_req, __ATOMIC_ACQUIRE);
            if (req == g_wst[i].served_req) continue;
            e->last_req_ms = t;
            busy = 1;
            if (capture_window(i) || !(e->flags & XSO_WF_MAPPED) || e->frame_slot < 0) {
                g_wst[i].served_req = req;           /* served (or impossible right now) */
                g_wst[i].last_capture_ms = t;
                __atomic_store_n(&e->cap_done, req, __ATOMIC_RELEASE);
            } else {
                /* reader was busy with the target half: retry next loop */
            }
        }
        /* cursor */
        {
            uint32_t creq = __atomic_load_n(&shm->cursor.req, __ATOMIC_ACQUIRE);
            if (creq != cursor_served) { capture_cursor(); cursor_served = creq; busy = 1; }
        }
        /* input */
        process_input();

        /* release idle slots */
        if (t - last_idle_check >= 1000) {
            last_idle_check = t;
            for (int i = 0; i < XSO_MAX_WINDOWS; i++) {
                xso_window *e = &shm->windows[i];
                if (e->xid && e->frame_slot >= 0 && t - e->last_req_ms > 5000) {
                    DBG("slot idle, releasing window 0x%x", e->xid);
                    release_slot(i);
                    if (g_wst[i].redirected && !(e->flags & XSO_WF_DESKTOP)) {
                        XCompositeUnredirectWindow(dpy, e->xid, CompositeRedirectAutomatic);
                        g_wst[i].redirected = 0;
                    }
                }
            }
            uint32_t pe = shm->hdr.pe_heartbeat_ms;
            if (pe != last_pe_seen) { if (!last_pe_seen) LOG("DLL connected (pid %u)", shm->hdr.pe_pid); last_pe_seen = pe; }
        }

        struct timespec ts = { 0, busy ? 500000L : 2000000L };
        nanosleep(&ts, NULL);
    }
    LOG("exiting");
    cleanup();
    return 0;
}
