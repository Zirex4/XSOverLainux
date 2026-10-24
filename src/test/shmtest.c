/* shmtest - native client exercising the xso-bridge shared memory (no Wine needed) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include "xso_shm.h"

static void title_ascii(const uint16_t *t, char *out, int cap) { int i; for (i = 0; i < cap - 1 && t[i]; i++) out[i] = t[i] < 128 ? (char)t[i] : '?'; out[i] = 0; }
static uint32_t now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint32_t)(ts.tv_sec*1000+ts.tv_nsec/1000000); }

static int write_ppm(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint32_t p = px[i]; unsigned char rgb[3] = { (p >> 16) & 255, (p >> 8) & 255, p & 255 }; fwrite(rgb, 1, 3, f); }
    fclose(f); return 0;
}

int main(int argc, char **argv)
{
    const char *path = getenv("XSO_BRIDGE_SHM"); if (!path) path = XSO_SHM_DEFAULT_PATH;
    const char *want = argc > 1 ? argv[1] : NULL;      /* title substring, or "desktop" */
    const char *out = argc > 2 ? argv[2] : "/tmp/xso-frame.ppm";
    int fd = open(path, O_RDWR); if (fd < 0) { perror("open"); return 1; }
    xso_shm *shm = mmap(NULL, XSO_TOTAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) { perror("mmap"); return 1; }
    printf("magic=%08x ver=%u helper=%u heartbeat=%u screen=%dx%d monitors=%u list_gen=%u cursor_window=%u ptr=%d,%d\n",
           shm->hdr.magic, shm->hdr.version, shm->hdr.helper_pid, shm->hdr.heartbeat_ms, shm->hdr.screen_w, shm->hdr.screen_h,
           shm->hdr.n_monitors, shm->hdr.list_gen, shm->hdr.cursor_window, shm->hdr.pointer_x, shm->hdr.pointer_y);
    for (unsigned i = 0; i < shm->hdr.n_monitors; i++) {
        xso_monitor *m = &shm->monitors[i];
        printf("monitor %u: %s %dx%d+%d+%d primary=%d uid=%u mfg=%04x prod=%04x friendly='%s' path='%s' edid=%d\n", i, m->name, m->w, m->h, m->x, m->y, m->primary, m->uid, m->mfg_id, m->product_code, m->friendly, m->device_path, m->edid_len);
    }
    int target = -1;
    for (int i = 0; i < XSO_MAX_WINDOWS; i++) {
        xso_window *e = &shm->windows[i];
        if (!e->xid) continue;
        char t[80]; title_ascii(e->title, t, sizeof t);
        printf("[%3d] xid=%08x gen=%u flags=%02x z=%-5u %4dx%-4d+%-5d+%-5d pid=%-7u mon=%d icon=%dx%d class='%s' title='%s'\n",
               i, e->xid, e->gen, e->flags, e->zorder, e->w, e->h, e->x, e->y, e->pid, e->monitor, e->icon_w, e->icon_h, e->klass, t);
        if (want && target < 0) {
            if (!strcmp(want, "desktop") && (e->flags & XSO_WF_DESKTOP)) target = i;
            else if (strcmp(want, "desktop") && strstr(t, want)) target = i;
        }
    }
    if (target < 0) { printf("no capture target\n"); return 0; }
    xso_window *e = &shm->windows[target];
    printf("capturing [%d]...\n", target);
    uint32_t seq0 = e->frame_seq;
    e->draw_cursor = 1;
    uint32_t t0 = now_ms();
    __atomic_add_fetch(&e->cap_req, 1u, __ATOMIC_RELEASE);
    while (__atomic_load_n(&e->frame_seq, __ATOMIC_ACQUIRE) == seq0 && now_ms() - t0 < 2000) usleep(1000);
    if (e->frame_seq == seq0) { printf("timeout (cap_req=%u cap_done=%u slot=%d)\n", e->cap_req, e->cap_done, e->frame_slot); return 1; }
    printf("frame in %u ms: %dx%d stride %d slot %d buf %u\n", now_ms() - t0, e->frame_w, e->frame_h, e->frame_stride, e->frame_slot, e->frame_buf);
    uint32_t half = e->frame_buf; e->reader_buf = half;
    const uint32_t *px = (const uint32_t *)XSO_FRAME_PTR(shm, e->frame_slot, half);
    write_ppm(out, px, e->frame_w, e->frame_h);
    e->reader_buf = XSO_NO_INDEX;
    printf("wrote %s\n", out);
    /* timing: 60 captures */
    t0 = now_ms();
    uint32_t mn = 1e9, mx = 0; int done = 0;
    for (int k = 0; k < 60 && now_ms() - t0 < 5000; k++) {
        uint32_t s = e->frame_seq, ts = now_ms();
        __atomic_add_fetch(&e->cap_req, 1u, __ATOMIC_RELEASE);
        while (__atomic_load_n(&e->frame_seq, __ATOMIC_ACQUIRE) == s && now_ms() - ts < 2000) usleep(200);
        uint32_t dt = now_ms() - ts; if (dt < mn) mn = dt; if (dt > mx) mx = dt; done++;
    }
    printf("%d captures in %u ms (min %u max %u avg %.1f ms)\n", done, now_ms() - t0, mn, mx, (double)(now_ms() - t0) / done);
    /* cursor */
    uint32_t cs = shm->cursor.seq; __atomic_add_fetch(&shm->cursor.req, 1u, __ATOMIC_RELEASE);
    t0 = now_ms(); while (shm->cursor.seq == cs && now_ms() - t0 < 1000) usleep(1000);
    printf("cursor: seq=%u pos=%d,%d hot=%d,%d size=%dx%d\n", shm->cursor.seq, shm->cursor.x, shm->cursor.y, shm->cursor.hot_x, shm->cursor.hot_y, shm->cursor.w, shm->cursor.h);
    if (e->icon_w) { write_ppm("/tmp/xso-icon.ppm", shm->icons[target], e->icon_w, e->icon_h); printf("wrote /tmp/xso-icon.ppm\n"); }
    return 0;
}
