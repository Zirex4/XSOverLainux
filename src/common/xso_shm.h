/*
 * xso_shm.h - shared-memory protocol between the native X11 capture helper
 * (xso-bridge, Linux ELF) and the replacement uWindowCapture.dll that runs
 * inside XSOverlay under Proton.
 *
 * Both sides are x86-64; only fixed-width types are used so the layout is
 * identical for gcc (SysV) and clang-mingw (MS ABI).
 */
#ifndef XSO_SHM_H
#define XSO_SHM_H

#include <stdint.h>
#include <stddef.h>

#define XSO_SHM_MAGIC     0x4F53584Fu   /* "OXSO" */
#define XSO_SHM_VERSION   2u
#define XSO_SHM_DEFAULT_PATH "/dev/shm/xso-bridge"

#define XSO_MAX_WINDOWS   256
#define XSO_MAX_MONITORS  8
#define XSO_TITLE_LEN     256          /* UTF-16 code units incl. NUL */
#define XSO_CLASS_LEN     128
#define XSO_ICON_DIM      128          /* max icon side (BGRA) */
#define XSO_CURSOR_DIM    128          /* max cursor side (BGRA) */
#define XSO_FRAME_SLOTS   24           /* concurrently captured windows */
#define XSO_FRAME_MAX_W   3840
#define XSO_FRAME_MAX_H   2160
#define XSO_FRAME_BYTES   ((size_t)XSO_FRAME_MAX_W * XSO_FRAME_MAX_H * 4)
#define XSO_INPUT_RING    512
#define XSO_NO_INDEX      0xFFFFFFFFu

/* xso_window.flags */
#define XSO_WF_VISIBLE    (1u << 0)    /* mapped (or minimized but existing) */
#define XSO_WF_ICONIC     (1u << 1)    /* minimized */
#define XSO_WF_ZOOMED     (1u << 2)    /* maximized */
#define XSO_WF_ALTTAB     (1u << 3)    /* normal app window */
#define XSO_WF_PRIMARY    (1u << 4)    /* desktop entry on the primary monitor */
#define XSO_WF_DESKTOP    (1u << 5)    /* monitor pseudo-window */
#define XSO_WF_MAPPED     (1u << 6)    /* really viewable right now */

/* xso_input_event.type (PE -> helper) */
#define XSO_IN_MOUSE_MOVE_ABS 1   /* a=x b=y (virtual screen coords) */
#define XSO_IN_MOUSE_MOVE_REL 2   /* a=dx b=dy */
#define XSO_IN_MOUSE_BUTTON   3   /* a=button(1..5) b=pressed */
#define XSO_IN_MOUSE_WHEEL    4   /* a=delta (120 per notch) b=horizontal */
#define XSO_IN_KEY            5   /* a=win vk b=pressed c=scancode d=extended(1) */
#define XSO_IN_KEY_UNICODE    6   /* a=unicode b=pressed */
#define XSO_IN_FOCUS_WINDOW   7   /* a=xid */
#define XSO_IN_SHOW_WINDOW    8   /* a=xid b=cmd (SW_*) */

typedef struct xso_monitor {
    int32_t  x, y, w, h;
    int32_t  primary;
    uint32_t uid;                 /* RandR output XID (stable while plugged) */
    uint16_t mfg_id;
    uint16_t product_code;
    char     name[64];            /* connector name, e.g. DP-4 */
    char     friendly[64];        /* EDID monitor name */
    char     device_path[128];    /* stable identifier string */
    int32_t  edid_len;
    uint8_t  edid[512];
} xso_monitor;

typedef struct xso_window {
    /* --- identity (helper writes) --- */
    uint32_t xid;                 /* 0 = slot empty; desktops use 0x80000000|monitor */
    uint32_t gen;                 /* bumped whenever the slot is (re)assigned, never 0 */
    uint32_t flags;               /* XSO_WF_* */
    int32_t  monitor;             /* monitor index */
    int32_t  x, y, w, h;          /* window rect, virtual-screen coords */
    int32_t  cx, cy, cw, ch;      /* client rect */
    uint32_t zorder;              /* 0 = topmost */
    uint32_t pid;
    uint32_t owner_xid;
    uint32_t title_gen;           /* bumped when title changes */
    uint16_t title[XSO_TITLE_LEN];
    char     klass[XSO_CLASS_LEN];
    /* --- icon (helper fills when the window is added) --- */
    int32_t  icon_w, icon_h;      /* 0 = no icon; pixels in xso_shm.icons[slot] */
    uint32_t icon_gen;
    /* --- capture control --- */
    uint32_t cap_req;             /* PE increments to ask for one capture */
    uint32_t cap_done;            /* helper copies cap_req once served */
    int32_t  cap_mode;            /* CaptureMode (informational) */
    int32_t  draw_cursor;         /* PE sets */
    int32_t  frame_slot;          /* helper: frame slot index or -1 */
    uint32_t frame_seq;           /* helper bumps after publishing a frame */
    uint32_t frame_buf;           /* half (0/1) that holds the published frame */
    uint32_t reader_buf;          /* PE: half it is reading, or XSO_NO_INDEX */
    int32_t  frame_w, frame_h, frame_stride;
    uint32_t last_req_ms;         /* helper bookkeeping */
    /* --- incremental updates (v2) --- */
    int32_t  dirty_x, dirty_y, dirty_w, dirty_h;  /* region of the published frame that changed
                                                     since the DLL's last acknowledged upload */
    uint32_t ack_seq;             /* DLL: frame_seq it last uploaded */
} xso_window;

typedef struct xso_cursor {
    uint32_t req;                 /* PE bumps */
    uint32_t seq;                 /* helper bumps after a capture */
    int32_t  x, y;                /* pointer position, virtual-screen coords */
    int32_t  hot_x, hot_y;
    int32_t  w, h;                /* image size (0 = none) */
    int32_t  visible;
    uint32_t pixels[XSO_CURSOR_DIM * XSO_CURSOR_DIM];   /* BGRA straight alpha */
} xso_cursor;

typedef struct xso_input_event {
    uint32_t type;
    int32_t  a, b, c, d;
} xso_input_event;

typedef struct xso_header {
    uint32_t magic;
    uint32_t version;
    uint64_t total_size;
    uint32_t helper_pid;
    uint32_t heartbeat_ms;        /* CLOCK_MONOTONIC ms, helper updates every loop */
    int32_t  screen_x, screen_y, screen_w, screen_h;
    uint32_t n_monitors;
    uint32_t list_gen;            /* bumped when the window table changes */
    uint32_t cursor_window;       /* table index of the window under the pointer */
    uint32_t input_head;          /* PE writes events at head */
    uint32_t input_tail;          /* helper consumes from tail */
    uint32_t pe_pid;              /* PE announces itself (informational) */
    uint32_t pe_heartbeat_ms;
    int32_t  pointer_x, pointer_y;  /* helper: current pointer position */
    uint32_t reserved[11];
} xso_header;

typedef struct xso_shm {
    xso_header      hdr;
    xso_monitor     monitors[XSO_MAX_MONITORS];
    xso_window      windows[XSO_MAX_WINDOWS];
    xso_cursor      cursor;
    xso_input_event input[XSO_INPUT_RING];
    uint32_t        icons[XSO_MAX_WINDOWS][XSO_ICON_DIM * XSO_ICON_DIM];
} xso_shm;

#define XSO_FRAME_AREA_OFFSET ((((size_t)sizeof(xso_shm)) + 4095u) & ~(size_t)4095u)
#define XSO_FRAME_PTR(base, slot, half) \
    ((uint8_t *)(base) + XSO_FRAME_AREA_OFFSET + ((size_t)(slot) * 2u + (size_t)(half)) * XSO_FRAME_BYTES)
#define XSO_TOTAL_SIZE (XSO_FRAME_AREA_OFFSET + (size_t)XSO_FRAME_SLOTS * 2u * XSO_FRAME_BYTES)

#define XSO_DESKTOP_XID(mon) (0x80000000u | (uint32_t)(mon))

/* layout guards: both sides must agree */
typedef char xso_assert_monitor_size[(sizeof(xso_monitor) == 800) ? 1 : -1];
typedef char xso_assert_window_size[(sizeof(xso_window) == 784) ? 1 : -1];
typedef char xso_assert_cursor_size[(sizeof(xso_cursor) == 36 + XSO_CURSOR_DIM * XSO_CURSOR_DIM * 4) ? 1 : -1];
typedef char xso_assert_header_size[(sizeof(xso_header) == 120) ? 1 : -1];

#endif /* XSO_SHM_H */
