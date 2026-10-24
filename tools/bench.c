#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xcomposite.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e3+t.tv_nsec/1e6; }
static int err(Display*d, XErrorEvent*e){ (void)d;(void)e; return 0; }
int main(int argc, char**argv){
  Display *d = XOpenDisplay(NULL); XSetErrorHandler(err); int s = DefaultScreen(d); Window root = RootWindow(d,s);
  int W = DisplayWidth(d,s), H = DisplayHeight(d,s);
  XShmSegmentInfo si; XImage *img = XShmCreateImage(d, DefaultVisual(d,s), DefaultDepth(d,s), ZPixmap, NULL, &si, W, H);
  si.shmid = shmget(IPC_PRIVATE, img->bytes_per_line*img->height, IPC_CREAT|0600); si.shmaddr = img->data = shmat(si.shmid,0,0); si.readOnly=0; XShmAttach(d,&si); XSync(d,0);
  double t0 = ms(); for (int i=0;i<20;i++) XShmGetImage(d, root, img, 0,0, AllPlanes); double t1 = ms();
  printf("root XShmGetImage %dx%d: %.1f ms/frame\n", W,H,(t1-t0)/20);
  /* region 1280x720 */
  XImage *img2 = XShmCreateImage(d, DefaultVisual(d,s), DefaultDepth(d,s), ZPixmap, NULL, &si, 1280, 720);
  XShmSegmentInfo si2; si2.shmid = shmget(IPC_PRIVATE, img2->bytes_per_line*img2->height, IPC_CREAT|0600); si2.shmaddr = img2->data = shmat(si2.shmid,0,0); si2.readOnly=0; XShmAttach(d,&si2); XSync(d,0);
  t0 = ms(); for (int i=0;i<20;i++) XShmGetImage(d, root, img2, 0,0, AllPlanes); t1 = ms();
  printf("root XShmGetImage 1280x720: %.1f ms/frame\n", (t1-t0)/20);
  /* plain XGetImage */
  t0 = ms(); for (int i=0;i<5;i++) { XImage *x = XGetImage(d, root, 0,0, W,H, AllPlanes, ZPixmap); XDestroyImage(x);} t1 = ms();
  printf("root XGetImage %dx%d: %.1f ms/frame\n", W,H,(t1-t0)/5);
  if (argc > 1) {
    Window w = (Window)strtoul(argv[1], NULL, 16);
    XWindowAttributes a; XGetWindowAttributes(d, w, &a);
    XCompositeRedirectWindow(d, w, CompositeRedirectAutomatic); XSync(d,0);
    XShmSegmentInfo si3; XImage *img3 = XShmCreateImage(d, a.visual, a.depth, ZPixmap, NULL, &si3, a.width, a.height);
    si3.shmid = shmget(IPC_PRIVATE, img3->bytes_per_line*img3->height, IPC_CREAT|0600); si3.shmaddr = img3->data = shmat(si3.shmid,0,0); si3.readOnly=0; XShmAttach(d,&si3); XSync(d,0);
    t0 = ms(); for (int i=0;i<20;i++){ Pixmap p = XCompositeNameWindowPixmap(d, w); XShmGetImage(d, p, img3, 0,0, AllPlanes); XFreePixmap(d,p);} t1 = ms();
    printf("composite pixmap XShmGetImage %dx%d depth %d: %.1f ms/frame\n", a.width, a.height, a.depth, (t1-t0)/20);
    Pixmap p = XCompositeNameWindowPixmap(d, w);
    t0 = ms(); for (int i=0;i<20;i++){ XShmGetImage(d, p, img3, 0,0, AllPlanes);} t1 = ms();
    printf("composite pixmap (cached) XShmGetImage: %.1f ms/frame\n", (t1-t0)/20);
    XCompositeUnredirectWindow(d, w, CompositeRedirectAutomatic);
  }
  return 0; }
