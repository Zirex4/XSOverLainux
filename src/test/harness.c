/*
 * harness.exe - exercises uWindowCapture.dll the way Unity would, under Proton/DXVK.
 * usage: harness.exe <title-substring|desktop> <out.ppm> [frames]
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef struct UnityInterfaceGUID { unsigned long long hi, lo; } UnityInterfaceGUID;
typedef void IUnityInterface;
typedef struct IUnityInterfaces { IUnityInterface *(__stdcall *GetInterface)(UnityInterfaceGUID); void (__stdcall *RegisterInterface)(UnityInterfaceGUID, IUnityInterface *); } IUnityInterfaces;
typedef void (__stdcall *DevCb)(int);
typedef struct IUnityGraphics { int (__stdcall *GetRenderer)(void); void (__stdcall *Register)(DevCb); void (__stdcall *Unregister)(DevCb); } IUnityGraphics;
typedef struct IUnityGraphicsD3D11 { ID3D11Device *(__stdcall *GetDevice)(void); } IUnityGraphicsD3D11;
typedef struct Message { int type; int windowId; void *userData; } Message;
typedef struct MonitorEdidInfo { uint16_t mfgId, productCode; WCHAR devicePath[128]; WCHAR friendlyName[64]; } MonitorEdidInfo;

static ID3D11Device *g_dev; static ID3D11DeviceContext *g_ctx; static DevCb g_cb;
static int __stdcall gfx_GetRenderer(void) { return 2; }
static void __stdcall gfx_Register(DevCb cb) { g_cb = cb; }
static void __stdcall gfx_Unregister(DevCb cb) { (void)cb; g_cb = NULL; }
static ID3D11Device *__stdcall d3d_GetDevice(void) { return g_dev; }
static IUnityGraphics g_gfx = { gfx_GetRenderer, gfx_Register, gfx_Unregister };
static IUnityGraphicsD3D11 g_d3d = { d3d_GetDevice };
static IUnityInterface *__stdcall ifc_Get(UnityInterfaceGUID g)
{
    if (g.hi == 0x7CBA0A9CA4DDB544ULL && g.lo == 0x8C5AD4926EB17B11ULL) return &g_gfx;
    if (g.hi == 0xAAB37EF87A87D748ULL && g.lo == 0xBF76967F07EFB177ULL) return &g_d3d;
    printf("GetInterface unknown guid %llx %llx\n", g.hi, g.lo); return NULL;
}
static void __stdcall ifc_Reg(UnityInterfaceGUID g, IUnityInterface *p) { (void)g; (void)p; }
static IUnityInterfaces g_ifc = { ifc_Get, ifc_Reg };

#define LOAD(name) name##_t name = (name##_t)GetProcAddress(dll, #name); if (!name) { printf("missing export %s\n", #name); return 1; }
typedef void (__stdcall *UnityPluginLoad_t)(IUnityInterfaces *);
typedef void (__stdcall *UwcInitialize_t)(void);
typedef void (__stdcall *UwcUpdate_t)(float);
typedef UINT (__stdcall *UwcGetMessageCount_t)(void);
typedef const Message *(__stdcall *UwcGetMessages_t)(void);
typedef void (__stdcall *UwcClearMessages_t)(void);
typedef void (__stdcall *UwcExcludeRemovedWindowEvents_t)(void);
typedef const WCHAR *(__stdcall *UwcGetWindowTitle_t)(int);
typedef int (__stdcall *UwcIsDesktop_t)(int);
typedef int (__stdcall *UwcIsAltTabWindow_t)(int);
typedef UINT (__stdcall *UwcGetWindowWidth_t)(int);
typedef UINT (__stdcall *UwcGetWindowHeight_t)(int);
typedef UINT (__stdcall *UwcGetWindowX_t)(int);
typedef UINT (__stdcall *UwcGetWindowY_t)(int);
typedef UINT (__stdcall *UwcGetWindowTextureWidth_t)(int);
typedef UINT (__stdcall *UwcGetWindowTextureHeight_t)(int);
typedef void (__stdcall *UwcRequestCaptureWindow_t)(int, int);
typedef void (__stdcall *UwcSetWindowTexturePtr_t)(int, ID3D11Texture2D *);
typedef void (__stdcall *RenderFn)(int);
typedef RenderFn (__stdcall *UwcGetRenderEventFunc_t)(void);
typedef MonitorEdidInfo (__stdcall *UwcGetMonitorEdid_t)(int);
typedef UINT (__stdcall *UwcGetDesktopUid_t)(int);
typedef int (__stdcall *UwcIsPrimaryDesktop_t)(int);
typedef UINT (__stdcall *UwcGetWindowIconWidth_t)(int);
typedef UINT (__stdcall *UwcGetWindowIconHeight_t)(int);
typedef void (__stdcall *UwcRequestCaptureIcon_t)(int);
typedef void (__stdcall *UwcSetWindowIconTexturePtr_t)(int, ID3D11Texture2D *);
typedef void (__stdcall *UwcRequestCaptureCursor_t)(void);
typedef UINT (__stdcall *UwcGetCursorWidth_t)(void);
typedef UINT (__stdcall *UwcGetCursorHeight_t)(void);
typedef UINT (__stdcall *UwcGetCursorX_t)(void);
typedef void (__stdcall *UwcSetCursorTexturePtr_t)(ID3D11Texture2D *);
typedef int (__stdcall *UwcGetWindowIdUnderCursor_t)(void);
typedef UINT (__stdcall *UwcGetScreenWidth_t)(void);
typedef void (__stdcall *UwcSetDebugMode_t)(int);
typedef const char *(__stdcall *UwcGetWindowClassName_t)(int);

static ID3D11Texture2D *make_tex(UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC d; memset(&d, 0, sizeof d);
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *t = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &t);
    if (FAILED(hr)) printf("CreateTexture2D failed %08lx\n", hr);
    return t;
}

static int dump_tex(ID3D11Texture2D *t, const char *path)
{
    D3D11_TEXTURE2D_DESC d; ID3D11Texture2D_GetDesc(t, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &st))) { printf("staging failed\n"); return 0; }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)st, (ID3D11Resource *)t);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) { printf("map failed\n"); return 0; }
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot open %s\n", path); return 0; }
    fprintf(f, "P6\n%u %u\n255\n", d.Width, d.Height);
    unsigned long long sum = 0;
    for (UINT y = 0; y < d.Height; y++) {
        const uint32_t *row = (const uint32_t *)((const BYTE *)m.pData + y * m.RowPitch);
        for (UINT x = 0; x < d.Width; x++) { uint32_t p = row[x]; unsigned char rgb[3] = { (p >> 16) & 255, (p >> 8) & 255, p & 255 }; fwrite(rgb, 1, 3, f); sum += rgb[0] + rgb[1] + rgb[2]; }
    }
    fclose(f);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
    ID3D11Texture2D_Release(st);
    printf("wrote %s (%ux%u, pixel sum %llu)\n", path, d.Width, d.Height, sum);
    return 1;
}

int main(int argc, char **argv)
{
    const char *want = argc > 1 ? argv[1] : "desktop";
    const char *out = argc > 2 ? argv[2] : "Z:\\tmp\\harness.ppm";
    int frames_wanted = argc > 3 ? atoi(argv[3]) : 30;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice failed %08lx\n", hr); return 1; }
    printf("D3D11 device ok, feature level %x\n", fl);

    HMODULE dll = LoadLibraryA("uWindowCapture.dll");
    if (!dll) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    LOAD(UnityPluginLoad) LOAD(UwcInitialize) LOAD(UwcUpdate) LOAD(UwcGetMessageCount) LOAD(UwcGetMessages) LOAD(UwcClearMessages)
    LOAD(UwcExcludeRemovedWindowEvents) LOAD(UwcGetWindowTitle) LOAD(UwcIsDesktop) LOAD(UwcIsAltTabWindow) LOAD(UwcGetWindowWidth) LOAD(UwcGetWindowHeight)
    LOAD(UwcGetWindowX) LOAD(UwcGetWindowY) LOAD(UwcGetWindowTextureWidth) LOAD(UwcGetWindowTextureHeight) LOAD(UwcRequestCaptureWindow)
    LOAD(UwcSetWindowTexturePtr) LOAD(UwcGetRenderEventFunc) LOAD(UwcGetMonitorEdid) LOAD(UwcGetDesktopUid) LOAD(UwcIsPrimaryDesktop)
    LOAD(UwcGetWindowIconWidth) LOAD(UwcGetWindowIconHeight) LOAD(UwcRequestCaptureIcon) LOAD(UwcSetWindowIconTexturePtr)
    LOAD(UwcRequestCaptureCursor) LOAD(UwcGetCursorWidth) LOAD(UwcGetCursorHeight) LOAD(UwcGetCursorX) LOAD(UwcSetCursorTexturePtr)
    LOAD(UwcGetWindowIdUnderCursor) LOAD(UwcGetScreenWidth) LOAD(UwcSetDebugMode) LOAD(UwcGetWindowClassName)

    UnityPluginLoad(&g_ifc);
    if (g_cb) g_cb(0);
    UwcSetDebugMode(1);
    UwcInitialize();
    RenderFn render = UwcGetRenderEventFunc();
    printf("screen width %u\n", UwcGetScreenWidth());

    int target = -1, desktop_id = -1, captured = 0, icon_done = 0, cursor_done = 0;
    ID3D11Texture2D *tex = NULL, *icon_tex = NULL, *cursor_tex = NULL;
    DWORD t_start = GetTickCount(), t_first = 0;
    for (int it = 0; it < 600 && captured < frames_wanted; it++) {
        UwcUpdate(0.016f);
        UwcExcludeRemovedWindowEvents();
        UINT n = UwcGetMessageCount();
        const Message *m = UwcGetMessages();
        for (UINT i = 0; i < n; i++) {
            int id = m[i].windowId;
            switch (m[i].type) {
            case 0: {
                const WCHAR *t = UwcGetWindowTitle(id);
                int isd = UwcIsDesktop(id);
                printf("  + window %d desktop=%d alttab=%d %ux%u+%u+%u class=%s title='%ls'\n", id, isd, UwcIsAltTabWindow(id),
                       UwcGetWindowWidth(id), UwcGetWindowHeight(id), UwcGetWindowX(id), UwcGetWindowY(id), UwcGetWindowClassName(id), t ? t : L"");
                if (isd && desktop_id < 0) desktop_id = id;
                if (target < 0) {
                    char a[256]; int k; for (k = 0; k < 255 && t && t[k]; k++) a[k] = t[k] < 128 ? (char)t[k] : '?'; a[k] = 0;
                    if ((!strcmp(want, "desktop") && isd) || (strcmp(want, "desktop") && strstr(a, want))) { target = id; printf("  target = %d\n", id); }
                }
                break; }
            case 1: printf("  - window %d removed\n", id); break;
            case 2: if (id == target) { captured++; if (!t_first) t_first = GetTickCount(); } break;
            case 3: {
                UINT w = UwcGetWindowTextureWidth(id), h = UwcGetWindowTextureHeight(id);
                printf("  ~ window %d size changed -> %ux%u\n", id, w, h);
                if (id == target) { if (tex) ID3D11Texture2D_Release(tex); tex = make_tex(w, h); UwcSetWindowTexturePtr(id, tex); }
                break; }
            case 4: printf("  * icon captured for %d\n", id); icon_done = 1; break;
            case 5: printf("  * cursor captured\n"); cursor_done = 1; break;
            case 1001: printf("  ! TextureNullError %d\n", id); break;
            case 1002: printf("  ! TextureSizeError %d\n", id); break;
            default: printf("  ? message %d for %d\n", m[i].type, id); break;
            }
        }
        UwcClearMessages();
        if (target >= 0) {
            UwcRequestCaptureWindow(target, 0);
            if (!icon_tex && UwcGetWindowIconWidth(target) > 0) {
                icon_tex = make_tex(UwcGetWindowIconWidth(target), UwcGetWindowIconHeight(target));
                UwcSetWindowIconTexturePtr(target, icon_tex);
                UwcRequestCaptureIcon(target);
            }
        }
        if (it == 5) UwcRequestCaptureCursor();
        if (!cursor_tex && UwcGetCursorWidth() > 0) { cursor_tex = make_tex(UwcGetCursorWidth(), UwcGetCursorHeight()); UwcSetCursorTexturePtr(cursor_tex); UwcRequestCaptureCursor(); }
        render(0);
        Sleep(16);
    }
    if (getenv("XSO_TEST_INPUT")) {
        printf("input test: SetCursorPos + SendInput through hooked user32\n");
        SetCursorPos(123, 456);
        INPUT in[4]; memset(in, 0, sizeof in);
        in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE; in[0].mi.dx = 32768; in[0].mi.dy = 32768;
        in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        in[2].type = INPUT_MOUSE; in[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = 'A';
        UINT n = SendInput(4, in, sizeof(INPUT));
        INPUT up; memset(&up, 0, sizeof up); up.type = INPUT_KEYBOARD; up.ki.wVk = 'A'; up.ki.dwFlags = KEYEVENTF_KEYUP;
        n += SendInput(1, &up, sizeof(INPUT));
        INPUT wh; memset(&wh, 0, sizeof wh); wh.type = INPUT_MOUSE; wh.mi.dwFlags = MOUSEEVENTF_WHEEL; wh.mi.mouseData = (DWORD)-120;
        n += SendInput(1, &wh, sizeof(INPUT));
        printf("SendInput returned %u (expect 6)\n", n);
        Sleep(300);
    }
    DWORD t_end = GetTickCount();
    printf("captured %d frames of window %d in %lu ms (first after %lu ms)\n", captured, target, t_end - (t_first ? t_first : t_start), t_first ? t_first - t_start : 0);
    printf("window under cursor: %d; icon_done=%d cursor_done=%d cursor %ux%u at x=%u\n", UwcGetWindowIdUnderCursor(), icon_done, cursor_done, UwcGetCursorWidth(), UwcGetCursorHeight(), UwcGetCursorX());
    if (desktop_id >= 0) {
        MonitorEdidInfo e = UwcGetMonitorEdid(desktop_id);
        printf("desktop %d: uid=%u primary=%d mfg=%04x prod=%04x path='%ls' friendly='%ls'\n", desktop_id, UwcGetDesktopUid(desktop_id), UwcIsPrimaryDesktop(desktop_id), e.mfgId, e.productCode, e.devicePath, e.friendlyName);
    }
    if (tex) dump_tex(tex, out);
    if (icon_tex && icon_done) dump_tex(icon_tex, "Z:\\tmp\\harness-icon.ppm");
    if (cursor_tex && cursor_done) dump_tex(cursor_tex, "Z:\\tmp\\harness-cursor.ppm");
    return captured > 0 ? 0 : 2;
}
