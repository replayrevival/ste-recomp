/**
 * D3D8→D3D11 Compatibility Device Implementation
 *
 * Implements the Xbox D3D8 IDirect3DDevice8 interface using D3D11.
 * The game's translated RenderWare code calls D3D8 methods through
 * COM vtables; this layer translates those calls to D3D11 equivalents.
 *
 * Architecture:
 * - D3D11 device and swap chain created during initialization
 * - Render state tracking: D3D8 states mapped to D3D11 state objects
 * - Texture/buffer management: D3D8 resource handles wrap D3D11 resources
 * - Fixed-function pipeline: emulated via D3D11 shaders (the Xbox D3D8
 *   pipeline is configurable but not fully programmable)
 *
 * Build: Requires Windows SDK with d3d11.h and dxgi.h
 */

#include "d3d8_internal.h"
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <math.h>
void d3d8_gpubusy_mark(void);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_exit_hook.h"
#include "d3d8_video.h"
#include <d3dcompiler.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

/* ================================================================
 * STE resolution scaling: configuration (ste_video.ini next to the exe, env overrides)
 *
 * The guest keeps its 640x480 logical space.  Host surfaces are scaled: the display surface and every 640x480 guest
 * surface are render_w x render_h; other guest surfaces (256x256 passes, ...) are scaled by the same factor.  The
 * scan-out thread blits the finished frame to the swap chain (pillar-boxed, stretched, or 1:1).
 * ================================================================ */
static D3D8VideoCfg g_vcfg;
static int g_vcfg_loaded;
D3D8DisplayResolver d3d8_display_resolver;
static char g_mon_dev[40];

/* c252: monitor the window opens on. display = -1 / out of range -> the primary monitor (a log line says so). Fills the monitor rectangle and the GDI device name (for the mode list). */
static void vid_pick_monitor(int display, RECT *mr, char *devname, size_t dn)
{
    RECT b; int n = 0, ok = 0; HMONITOR hm; MONITORINFOEXA mi;
    mr->left = mr->top = 0; mr->right = GetSystemMetrics(SM_CXSCREEN); mr->bottom = GetSystemMetrics(SM_CYSCREEN); devname[0] = 0;
    if (display >= 0) {
        if (d3d8_display_resolver && d3d8_display_resolver(display, &b, &n) && b.right > b.left && b.bottom > b.top) { *mr = b; ok = 1; }
        else fprintf(stderr, "[VIDEO] display=%d is not available (%d display%s found): using the primary monitor\n", display, n, n == 1 ? "" : "s");
    }
    { POINT pt; pt.x = ok ? (mr->left + mr->right) / 2 : 0; pt.y = ok ? (mr->top + mr->bottom) / 2 : 0; hm = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY); }
    memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    if (hm && GetMonitorInfoA(hm, (MONITORINFO *)&mi)) { strncpy(devname, mi.szDevice, dn - 1); devname[dn - 1] = 0; if (!ok) *mr = mi.rcMonitor; }
    fprintf(stderr, "[VIDEO] display request=%d -> monitor %s at %ld,%ld size %ldx%ld%s\n", display, devname[0] ? devname : "?", mr->left, mr->top, mr->right - mr->left, mr->bottom - mr->top, ok ? " (SDL3 order)" : " (primary)");
}

/* c252: does the monitor report a W x H mode? */
static int vid_mode_listed(const char *devname, UINT w, UINT h)
{
    DEVMODEA dm; DWORD i;
    for (i = 0; ; ++i) {
        memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsA(devname[0] ? devname : NULL, i, &dm)) break;
        if (dm.dmPelsWidth == w && dm.dmPelsHeight == h && dm.dmBitsPerPel >= 24) return 1;
    }
    return 0;
}

void d3d8_video_fullscreen_failed(const char *why)
{
    if (g_vcfg.mode != D3D8V_FULLSCREEN) return;
    fprintf(stderr, "[VIDEO] fullscreen %ux%u failed (%s), using borderless\n", g_vcfg.out_w, g_vcfg.out_h, why); fflush(stderr);
    g_vcfg.mode = D3D8V_BORDERLESS;
}
static float g_xmap_s = 1.0f, g_xmap_o = 0.0f;
void  d3d8_set_kelvin_xmap(float s, float o) { g_xmap_s = s; g_xmap_o = o; }
/* Camera aspect scale for aspect=wide: (4/3) / (render aspect); 1.0 otherwise. The recomp_manual wrapper of the title's
 * SetPerspective (sub_0001F740) multiplies the camera's height/width ratio by it (Hor+: vertical FOV kept). */
extern ptrdiff_t g_xbox_mem_offset;
int d3d8_front_end_active(void)
{
    static uint32_t last = 0xFFFFFFFFu;
    uint32_t v = *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x003562B4u);
    if (v != last) { last = v; { static ULONGLONG t0; ULONGLONG n = GetTickCount64(); if (!t0) t0 = n; fprintf(stderr, "[FRONTEND] t=%.1fs game-state word 0x003562B4 = 0x%X (front-end=%d)\n", (double)(n - t0) / 1000.0, v, v == 0x27u); } fflush(stderr); }
    return v == 0x27u;
}
float d3d8_video_cam_aspect_scale(void)
{
    const D3D8VideoCfg *c = d3d8_video_cfg();
    if (c->aspect != D3D8V_ASPECT_WIDE) return 1.0f;
    if (c->menu_aspect == D3D8V_ASPECT_PILLAR && d3d8_front_end_active()) return 1.0f;   /* c102: menu cameras keep their native 4:3 projection */
    return (4.0f / 3.0f) / ((float)c->render_w / (float)c->render_h);
}
float d3d8_xmap_s(void) { return g_xmap_s; }
float d3d8_xmap_o(void) { return g_xmap_o; }

static void vid_get(const char *envname, const char *key, const char *ini, char *out, size_t n)
{
    const char *e = getenv(envname);
    out[0] = 0;
    if (e && e[0]) { strncpy(out, e, n - 1); out[n - 1] = 0; return; }
    if (ini[0]) GetPrivateProfileStringA("video", key, "", out, (DWORD)n, ini);
}

const D3D8VideoCfg *d3d8_video_cfg(void)
{
    if (!g_vcfg_loaded) {
        char ini[MAX_PATH] = "", b[64], *p;
        const char *e = getenv("STE_VIDEO_INI");
        float sc; UINT W, H, rw, rh; int mode = D3D8V_WINDOW, asp = D3D8V_ASPECT_PILLAR; float eff;
        if (e && e[0]) { strncpy(ini, e, MAX_PATH - 1); }
        else { GetModuleFileNameA(NULL, ini, MAX_PATH); p = strrchr(ini, 0x5C); if (p) strcpy(p + 1, "ste_video.ini"); }
        if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) ini[0] = 0;
        vid_get("STE_VIDEO_SCALE", "scale", ini, b, sizeof(b));  sc = b[0] ? (float)atof(b) : 0.0f;
        vid_get("STE_VIDEO_WIDTH", "width", ini, b, sizeof(b));  W = b[0] ? (UINT)atoi(b) : 0;
        vid_get("STE_VIDEO_HEIGHT", "height", ini, b, sizeof(b)); H = b[0] ? (UINT)atoi(b) : 0;
        vid_get("STE_VIDEO_MODE", "mode", ini, b, sizeof(b));
        if (!_stricmp(b, "borderless")) mode = D3D8V_BORDERLESS; else if (!_stricmp(b, "fullscreen")) mode = D3D8V_FULLSCREEN;
        { int dsp = -1; RECT mr; char mdev[40]; vid_get("STE_VIDEO_DISPLAY", "display", ini, b, sizeof(b)); if (b[0] >= '0' && b[0] <= '9') dsp = atoi(b);   /* c252 */
          vid_pick_monitor(dsp, &mr, mdev, sizeof mdev); g_vcfg.display = dsp; g_vcfg.mon_x = mr.left; g_vcfg.mon_y = mr.top; g_vcfg.mon_w = mr.right - mr.left; g_vcfg.mon_h = mr.bottom - mr.top;
          strncpy(g_mon_dev, mdev, sizeof g_mon_dev - 1); }
        vid_get("STE_VIDEO_ASPECT", "aspect", ini, b, sizeof(b));
        /* c255: aspect = original | native (launcher: "Original (4:3)" / "Fit my screen"). "wide" and "stretch" are hidden aliases of native (stretching is gone; the 3D view always widens, Hor+). Unknown / empty = original. */
        if (!_stricmp(b, "native") || !_stricmp(b, "wide") || !_stricmp(b, "stretch")) asp = D3D8V_ASPECT_WIDE;
        else if (b[0] && _stricmp(b, "original") && _stricmp(b, "pillar")) fprintf(stderr, "[VIDEO] aspect=\"%s\" not recognised, using original\n", b);
        { int fc = 60, vs = 1; vid_get("STE_VIDEO_FPS_CAP", "fps_cap", ini, b, sizeof(b)); if (b[0]) fc = atoi(b); if (fc < 0) fc = 0;
          vid_get("STE_VIDEO_VSYNC", "vsync", ini, b, sizeof(b)); if (b[0]) vs = !_stricmp(b, "on") || !_stricmp(b, "true") || !_stricmp(b, "yes") || !strcmp(b, "1");   /* c251: default On (no key = On) */
          g_vcfg.fps_cap = fc; g_vcfg.vsync = vs;
          vid_get("STE_VIDEO_RENDER_AHEAD", "render_ahead", ini, b, sizeof(b)); g_vcfg.render_ahead = b[0] ? atoi(b) : 1;
          { const char *ed = getenv("KELVIN_ASYNC_DEPTH"); if (ed && atoi(ed) > 0) g_vcfg.render_ahead = atoi(ed); }
          if (g_vcfg.render_ahead < 1) g_vcfg.render_ahead = 1; if (g_vcfg.render_ahead > 4) g_vcfg.render_ahead = 4; }
        if (mode != D3D8V_WINDOW && !W && !H) { W = (UINT)g_vcfg.mon_w; H = (UINT)g_vcfg.mon_h; }
        if (!W && !H) {
            float bs = sc > 0.0f ? sc : 1.0f;
            H = (UINT)(480.0f * bs + 0.5f);
            W = asp == D3D8V_ASPECT_WIDE ? (UINT)((float)H * 16.0f / 9.0f + 0.5f) : (UINT)(640.0f * bs + 0.5f);
        } else if (!H) H = (UINT)((float)W * (asp == D3D8V_ASPECT_WIDE ? 9.0f / 16.0f : 0.75f) + 0.5f);
        else if (!W) W = (UINT)((float)H * (asp == D3D8V_ASPECT_WIDE ? 16.0f / 9.0f : 4.0f / 3.0f) + 0.5f);
        if (W < 320) W = 320; if (H < 240) H = 240; if (W > 16384) W = 16384; if (H > 16384) H = 16384;
        if (mode == D3D8V_FULLSCREEN && !vid_mode_listed(g_mon_dev, W, H)) {   /* c252: a mode the monitor does not report would make DXGI fail or stretch the desktop mode */
            fprintf(stderr, "[VIDEO] fullscreen mode %ux%u not supported, using borderless\n", W, H); mode = D3D8V_BORDERLESS;
        }
        {   /* c176: log the session every run (never assume the desktop size: it follows the RDP / Guacamole client) */
            RECT wa2 = { 0, 0, 0, 0 }; DEVMODEA dm; UINT dpi = 0; typedef UINT (WINAPI *GetDpiForSystem_t)(void); GetDpiForSystem_t gd = (GetDpiForSystem_t)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForSystem");
            SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa2, 0); memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm; EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm); if (gd) dpi = gd();
            fprintf(stderr, "[SESSION] desktop=%dx%d work=%ldx%ld display_mode=%lux%lu@%luHz bpp=%lu dpi=%u remote_session=%d monitors=%d\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), wa2.right - wa2.left, wa2.bottom - wa2.top,
                    (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight, (unsigned long)dm.dmDisplayFrequency, (unsigned long)dm.dmBitsPerPel, dpi, GetSystemMetrics(SM_REMOTESESSION) ? 1 : 0, GetSystemMetrics(SM_CMONITORS));
        }
        eff = sc > 0.0f ? sc : (float)H / 480.0f;   /* render scale follows the REQUESTED height (a 4K request on a smaller screen still renders at 4K, supersampled into the window) */
        {   /* c175 (F): a window larger than the screen is clamped by Windows to the screen, and the old render aspect (taken from the requested size) was then letter-/pillar-boxed into it (black side bars, "ultrawide does not fill").
             * Fit the window to the usable screen area up front, keeping the requested aspect, and take the render aspect from the window that will really exist. STE_VIDEO_CLAMP=0 keeps the requested size. */
            const char *ce = getenv("STE_VIDEO_CLAMP");
            if (!(ce && ce[0] == 0x30) && mode != D3D8V_FULLSCREEN) {
                RECT wa; int ncw = 0, nch = 0, mw, mh;
                if (mode == D3D8V_BORDERLESS) { wa.left = 0; wa.top = 0; wa.right = g_vcfg.mon_w; wa.bottom = g_vcfg.mon_h; }
                else { RECT r = { 0, 0, 100, 100 }; MONITORINFO mwi; POINT pt; pt.x = g_vcfg.mon_x + g_vcfg.mon_w / 2; pt.y = g_vcfg.mon_y + g_vcfg.mon_h / 2; mwi.cbSize = sizeof mwi;   /* c252: work area of the chosen monitor */
                       if (GetMonitorInfoA(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mwi)) wa = mwi.rcWork; else if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) { wa.left = 0; wa.top = 0; wa.right = g_vcfg.mon_w; wa.bottom = g_vcfg.mon_h; }
                       AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE); ncw = (r.right - r.left) - 100; nch = (r.bottom - r.top) - 100; }
                mw = (wa.right - wa.left) - ncw; mh = (wa.bottom - wa.top) - nch;
                if (mw >= 320 && mh >= 240 && ((int)W > mw || (int)H > mh)) {
                    float f = (float)mw / (float)W; if ((float)mh / (float)H < f) f = (float)mh / (float)H;
                    fprintf(stderr, "[VIDEO] requested window %ux%u is larger than the usable screen area %dx%d: window fitted to %ux%u (aspect kept, render scale unchanged; STE_VIDEO_CLAMP=0 disables)\n", W, H, mw, mh, (UINT)((float)W * f), (UINT)((float)H * f));
                    W = (UINT)((float)W * f); H = (UINT)((float)H * f); if (W < 320) W = 320; if (H < 240) H = 240;
                }
            }
        }
        {   /* c180: render_scale (fraction of the final window height; the scan-out upscales, FSR 1.0 by default) */
            float rsc = 0.0f; vid_get("STE_VIDEO_RENDER_SCALE", "render_scale", ini, b, sizeof(b)); if (b[0]) rsc = (float)atof(b);
            if (rsc < 0.25f) rsc = 0.0f; if (rsc > 1.0f) rsc = 1.0f;
            g_vcfg.render_scale = rsc;
            if (rsc > 0.0f && sc <= 0.0f) eff = (float)H * rsc / 480.0f;
            { int up = rsc > 0.0f && rsc < 0.999f; vid_get("STE_VIDEO_UPSCALE", "upscale", ini, b, sizeof(b)); if (!_stricmp(b, "bilinear") || !_stricmp(b, "off")) up = 0; else if (!_stricmp(b, "fsr1") || !_stricmp(b, "fsr")) up = 1; g_vcfg.upscale = up; }
            vid_get("STE_VIDEO_FSR_SHARPNESS", "fsr_sharpness", ini, b, sizeof(b)); g_vcfg.fsr_sharp = b[0] ? (float)atof(b) : 0.25f;
        }
        rh = (UINT)(480.0f * eff + 0.5f);
        rw = asp == D3D8V_ASPECT_WIDE ? (UINT)((float)rh * (float)W / (float)H + 0.5f) : (UINT)(640.0f * eff + 0.5f);
        if (rh < 480) rh = 480; if (rw < 640 && asp != D3D8V_ASPECT_WIDE) rw = 640;
        if (rw > 16384) rw = 16384; if (rh > 16384) rh = 16384;
        g_vcfg.scale = (float)rh / 480.0f; g_vcfg.out_w = W; g_vcfg.out_h = H; g_vcfg.render_w = rw; g_vcfg.render_h = rh;
        g_vcfg.mode = mode; g_vcfg.aspect = asp;
        { int ma = D3D8V_ASPECT_PILLAR; vid_get("STE_VIDEO_MENU_ASPECT", "menu_aspect", ini, b, sizeof(b)); if (!_stricmp(b, "stretch")) ma = D3D8V_ASPECT_STRETCH; else if (!_stricmp(b, "wide")) ma = D3D8V_ASPECT_WIDE; g_vcfg.menu_aspect = ma; }
        { /* c119: ste_video.ini render_threads (0 = auto): vertex-pack worker threads of the Kelvin translator; KELVIN_THREADS in the environment wins */
          vid_get("STE_VIDEO_RENDER_THREADS", "render_threads", ini, b, sizeof(b));
          if (b[0] && atoi(b) > 0 && !getenv("KELVIN_THREADS")) _putenv_s("KELVIN_THREADS", b); }
        g_vcfg.logical_w = asp == D3D8V_ASPECT_WIDE ? 480.0f * (float)rw / (float)rh : 640.0f;
        g_vcfg_loaded = 1;
        fprintf(stderr, "[VIDEO] ini=%s scale=%.3f out=%ux%u render=%ux%u mode=%d aspect=%d logical_w=%.2f fps_cap=%d vsync=%d menu_aspect=%d\n", ini[0] ? ini : "(none)",
                g_vcfg.scale, W, H, rw, rh, mode, asp, g_vcfg.logical_w, g_vcfg.fps_cap, g_vcfg.vsync, g_vcfg.menu_aspect);
    }
    return &g_vcfg;
}

/* STE diagnostics: D3D8_DEBUG_LAYER=1 creates the devices with D3D11_CREATE_DEVICE_DEBUG (falls back
 * quietly if the SDK layers are not installed) and prints ID3D11InfoQueue messages to stderr. */
static ID3D11InfoQueue *g_infoq[2];
static const char *g_infoq_name[2] = { "render", "scanout" };
static int d3d8_debug_wanted(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("D3D8_DEBUG_LAYER"); on = (e && e[0] && e[0] != '0'); }
    return on;
}
static void d3d8_debug_attach(int i, ID3D11Device *dev)
{
    if (!dev || !d3d8_debug_wanted() || g_infoq[i]) return;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11InfoQueue, (void **)&g_infoq[i])))
        fprintf(stderr, "[D3D-DEBUG] info queue attached to %s device\n", g_infoq_name[i]);
    else
        fprintf(stderr, "[D3D-DEBUG] no info queue on %s device (debug layer not active)\n", g_infoq_name[i]);
}
void d3d8_debug_drain(void)
{
    static unsigned printed;
    int i;
    for (i = 0; i < 2; i++) {
        UINT64 n, k;
        if (!g_infoq[i]) continue;
        n = ID3D11InfoQueue_GetNumStoredMessages(g_infoq[i]);
        for (k = 0; k < n; k++) {
            SIZE_T len = 0; D3D11_MESSAGE *m;
            if (FAILED(ID3D11InfoQueue_GetMessage(g_infoq[i], k, NULL, &len)) || !len) continue;
            m = (D3D11_MESSAGE *)malloc(len);
            if (m && SUCCEEDED(ID3D11InfoQueue_GetMessage(g_infoq[i], k, m, &len)) && printed < 400) {
                printed++;
                fprintf(stderr, "[D3D-DEBUG] %s sev=%d id=%d: %s\n", g_infoq_name[i], (int)m->Severity, (int)m->ID, m->pDescription);
            }
            free(m);
        }
        ID3D11InfoQueue_ClearStoredMessages(g_infoq[i]);
    }
}
void d3d8_log_hr(const char *what, HRESULT hr)
{
    static unsigned n;
    if (n < 600) { n++; fprintf(stderr, "[D3D-HR] %s = 0x%08lX%s\n", what, (unsigned long)hr, FAILED(hr) ? "  FAILED" : ""); }
    d3d8_debug_drain();
}

/* ================================================================
 * Internal device state
 * ================================================================ */

/* Maximum tracked render states, texture stages, and transforms */
#define MAX_RENDER_STATES    256
#define MAX_TEXTURE_STAGES   4
#define MAX_TSS_STATES       32
#define MAX_TRANSFORMS       512
#define MAX_LIGHTS           8

typedef struct D3D8DeviceState {
    /* D3D11 objects */
    ID3D11Device            *d3d11_device;
    ID3D11DeviceContext     *d3d11_context;
    ID3D11Device            *scanout_device;
    ID3D11DeviceContext     *scanout_context;
    IDXGISwapChain          *swap_chain;

    /* Default render targets */
    ID3D11RenderTargetView  *default_rtv;
    ID3D11DepthStencilView  *default_dsv;
    ID3D11Texture2D         *default_depth;
    ID3D11Texture2D         *render_texture;
    ID3D11Texture2D         *ready_texture;
    ID3D11Texture2D         *readback_texture;
    /* GPU-side publication (c65): render device copies each finished frame into shared_texture under a keyed mutex; the scan-out
     * device reads it through shared_on_scan.  Replaces the synchronous staging Map + CPU copy (which made the guest thread wait
     * for the whole frame on the GPU).  STE_PRESENT_CPU=1 keeps the old path. */
    ID3D11Texture2D         *shared_texture;
    IDXGIKeyedMutex         *km_prod;
    ID3D11Texture2D         *shared_on_scan;
    IDXGIKeyedMutex         *km_scan;
    HANDLE                   scanout_thread;
    volatile LONG            scanout_active;
    volatile LONG            scanout_ready;
    volatile LONG            scanout_generation;
    volatile LONG            scanout_cpu_generation;
    volatile LONG            scanout_visible;
    CRITICAL_SECTION         scanout_cpu_lock;
    BOOL                     scanout_cpu_lock_initialized;
    BYTE                    *scanout_cpu_frame;

    /* Window */
    HWND                    hwnd;
    UINT                    width;      /* host render size of the display surface */
    UINT                    height;
    UINT                    out_w, out_h; /* swap chain / window size */
    D3DFORMAT               backbuffer_format;

    /* State tracking */
    DWORD                   render_states[MAX_RENDER_STATES];
    DWORD                   tss[MAX_TEXTURE_STAGES][MAX_TSS_STATES];
    D3DMATRIX               transforms[MAX_TRANSFORMS];
    D3DVIEWPORT8            viewport;
    D3DMATERIAL8            material;
    D3DLIGHT8               lights[MAX_LIGHTS];
    BOOL                    light_enable[MAX_LIGHTS];

    /* Current shader/FVF */
    DWORD                   vertex_shader;
    DWORD                   pixel_shader;

    /* Scene state */
    BOOL                    in_scene;

    /* Reference count */
    LONG                    ref_count;
} D3D8DeviceState;

/* Global device instance (Xbox has a single D3D device) */
static D3D8DeviceState g_device_state;
static IDirect3DDevice8 g_device;
static BOOL g_device_initialized = FALSE;

/* Current resource bindings */
static IDirect3DVertexBuffer8 *g_cur_vb = NULL;
static UINT                    g_cur_vb_stride = 0;
static IDirect3DIndexBuffer8  *g_cur_ib = NULL;
static UINT                    g_cur_ib_base_vertex = 0;
static IDirect3DBaseTexture8  *g_cur_textures[4] = { NULL };

/* Bound offscreen render target / depth stencil (NULL = default) */
static D3D8Surface *g_cur_rt = NULL;
static D3D8Surface *g_cur_ds = NULL;

/* Palettized texture palettes (256 ARGB entries per stage) */
#define D3D8_PALETTE_ENTRIES 256
static DWORD g_palettes[D3D8_MAX_PALETTES][D3D8_PALETTE_ENTRIES];
static BOOL  g_palettes_initialized = FALSE;

static void d3d8_palettes_init(void)
{
    int p, i;
    if (g_palettes_initialized) return;
    for (p = 0; p < D3D8_MAX_PALETTES; p++)
        for (i = 0; i < D3D8_PALETTE_ENTRIES; i++)
            g_palettes[p][i] = 0xFF000000u | ((DWORD)i * 0x010101u); /* gray ramp */
    g_palettes_initialized = TRUE;
}

void d3d8_SetPalette(DWORD stage, const DWORD *entries)
{
    d3d8_palettes_init();
    if (stage >= D3D8_MAX_PALETTES) return;
    if (entries)
        memcpy(g_palettes[stage], entries, sizeof(g_palettes[stage]));
    else {
        for (int i = 0; i < D3D8_PALETTE_ENTRIES; i++)
            g_palettes[stage][i] = 0xFF000000u | ((DWORD)i * 0x010101u);
    }
}

const DWORD *d3d8_GetPalette(DWORD stage)
{
    d3d8_palettes_init();
    return (stage < D3D8_MAX_PALETTES) ? g_palettes[stage] : g_palettes[0];
}

/* Forward declarations */
static const IDirect3DDevice8Vtbl g_device_vtbl;
static void up_ring_shutdown(void);
static BOOL d3d8_capture_backbuffer_bmp(const char *path);

/* Scan-out: publish ready_texture (render size) to the swap-chain back buffer (window size).  Same size = plain copy;
 * otherwise a fullscreen-triangle blit with linear filtering, pillar-boxed to the render aspect when aspect=4:3-pillar. */
static ID3D11VertexShader *g_bvs;
static ID3D11PixelShader *g_bps;
static ID3D11SamplerState *g_bss;
static ID3D11ShaderResourceView *g_bsrv;
static int g_bfail;
static const char g_blit_hlsl[] =
    "struct V { float4 p : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) { V o; float2 t = float2((id << 1) & 2, id & 2); o.uv = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "Texture2D tx : register(t0); SamplerState sm : register(s0);\n"
    "float4 ps(V i) : SV_TARGET { return float4(tx.Sample(sm, i.uv).rgb, 1.0); }\n";

static int scanout_blit_init(D3D8DeviceState *s)
{
    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL; D3D11_SAMPLER_DESC sd;
    if (g_bfail) return 0;
    if (FAILED(D3DCompile(g_blit_hlsl, sizeof(g_blit_hlsl) - 1, "blit", NULL, NULL, "vs", "vs_5_0", 0, 0, &vb, &err)) ||
        FAILED(D3DCompile(g_blit_hlsl, sizeof(g_blit_hlsl) - 1, "blit", NULL, NULL, "ps", "ps_5_0", 0, 0, &pb, &err))) { g_bfail = 1; return 0; }
    ID3D11Device_CreateVertexShader(s->scanout_device, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &g_bvs);
    ID3D11Device_CreatePixelShader(s->scanout_device, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &g_bps);
    memset(&sd, 0, sizeof(sd)); sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(s->scanout_device, &sd, &g_bss);
    ID3D11Device_CreateShaderResourceView(s->scanout_device, (ID3D11Resource *)s->ready_texture, NULL, &g_bsrv);
    if (!g_bvs || !g_bps || !g_bss || !g_bsrv) { g_bfail = 1; return 0; }
    return 1;
}

/* ---- c180: AMD FidelityFX Super Resolution 1.0 (EASU upscale + RCAS sharpen), (c) 2021 Advanced Micro Devices, Inc., MIT licence. The HLSL is the unmodified ffx_a.h / ffx_fsr1.h from
 * GPUOpen-Effects/FidelityFX-FSR (commit a21ffb8) loaded from <exe dir>\fsr1\ (STE_FSR_DIR overrides) and compiled at run time; only the two thin pixel-shader wrappers below are ours. Two passes on
 * the scan-out device: EASU render-size -> output-size intermediate, RCAS intermediate -> back buffer (letterboxed viewport). Falls back to the bilinear blit if anything fails. */
typedef struct { const struct ID3DIncludeVtbl *lpVtbl; char dir[MAX_PATH]; } FsrInc;
static HRESULT __stdcall fsr_inc_open(ID3DInclude *self, D3D_INCLUDE_TYPE t, LPCSTR name, LPCVOID parent, LPCVOID *pdata, UINT *pbytes)
{
    FsrInc *fi = (FsrInc *)self; char path[MAX_PATH * 2]; FILE *f; long n; void *buf; (void)t; (void)parent;
    _snprintf(path, sizeof path, "%s\\%s", fi->dir, name); f = fopen(path, "rb"); if (!f) return E_FAIL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); buf = malloc((size_t)n + 1); if (!buf) { fclose(f); return E_OUTOFMEMORY; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return E_FAIL; }
    fclose(f); *pdata = buf; *pbytes = (UINT)n; return S_OK;
}
static HRESULT __stdcall fsr_inc_close(ID3DInclude *self, LPCVOID data) { (void)self; free((void *)data); return S_OK; }
static const struct ID3DIncludeVtbl g_fsr_inc_vtbl = { fsr_inc_open, fsr_inc_close };
static ID3D11PixelShader *g_fsr_easu, *g_fsr_rcas; static ID3D11Texture2D *g_fsr_tex; static ID3D11RenderTargetView *g_fsr_rtv; static ID3D11ShaderResourceView *g_fsr_srv; static ID3D11Buffer *g_fsr_cb;
static UINT g_fsr_w, g_fsr_h; static int g_fsr_fail;
static const char g_fsr_head[] =
    "cbuffer C : register(b0) { uint4 con0; uint4 con1; uint4 con2; uint4 con3; uint4 rcas; float4 off; };\n"
    "Texture2D tx : register(t0); SamplerState sm : register(s0);\n#define A_GPU 1\n#define A_HLSL 1\n#include \"ffx_a.h\"\n";
static const char g_fsr_easu_src[] =
    "#define FSR_EASU_F 1\nAF4 FsrEasuRF(AF2 p){return tx.GatherRed(sm,p,ASU2(0,0));}\nAF4 FsrEasuGF(AF2 p){return tx.GatherGreen(sm,p,ASU2(0,0));}\nAF4 FsrEasuBF(AF2 p){return tx.GatherBlue(sm,p,ASU2(0,0));}\n#include \"ffx_fsr1.h\"\n"
    "float4 ps(float4 pos : SV_POSITION) : SV_TARGET { AF3 c; FsrEasuF(c, AU2(pos.xy - off.xy), con0, con1, con2, con3); return float4(c, 1.0); }\n";
static const char g_fsr_rcas_src[] =
    "#define FSR_RCAS_F 1\nAF4 FsrRcasLoadF(ASU2 p){return tx.Load(int3(p,0));}\nvoid FsrRcasInputF(inout AF1 r,inout AF1 g,inout AF1 b){}\n#include \"ffx_fsr1.h\"\n"
    "float4 ps(float4 pos : SV_POSITION) : SV_TARGET { AF3 c; FsrRcasF(c.r, c.g, c.b, AU2(pos.xy - off.xy), rcas); return float4(c, 1.0); }\n";
static int fsr_compile(D3D8DeviceState *s, const char *body, ID3D11PixelShader **out)
{
    char *src; ID3DBlob *pb = NULL, *err = NULL; FsrInc fi; const char *e = getenv("STE_FSR_DIR"); HRESULT hr; size_t n = sizeof g_fsr_head + strlen(body) + 8;
    if (e && e[0]) strncpy(fi.dir, e, MAX_PATH - 1); else { GetModuleFileNameA(NULL, fi.dir, MAX_PATH); { char *p = strrchr(fi.dir, '\\'); if (p) *p = 0; } strncat(fi.dir, "\\fsr1", MAX_PATH - strlen(fi.dir) - 1); }
    fi.dir[MAX_PATH - 1] = 0; fi.lpVtbl = &g_fsr_inc_vtbl;
    src = (char *)malloc(n); if (!src) return 0; strcpy(src, g_fsr_head); strcat(src, body);
    hr = D3DCompile(src, strlen(src), "fsr", NULL, (ID3DInclude *)&fi, "ps", "ps_5_0", 0, 0, &pb, &err); free(src);
    if (FAILED(hr)) { fprintf(stderr, "[FSR] shader compile failed 0x%08lX dir=%s: %s\n", (unsigned long)hr, fi.dir, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
    hr = ID3D11Device_CreatePixelShader(s->scanout_device, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, out);
    return SUCCEEDED(hr) && *out;
}
static int fsr_present(D3D8DeviceState *s, ID3D11RenderTargetView *bbrtv, const D3D11_VIEWPORT *vp)
{
    UINT dw = (UINT)(vp->Width + 0.5f), dh = (UINT)(vp->Height + 0.5f); float sharp = d3d8_video_cfg()->fsr_sharp; ID3D11DeviceContext *c = s->scanout_context; float black[4] = { 0, 0, 0, 1 };
    struct { UINT con[4][4]; UINT rcas[4]; float off[4]; } cb; union { float f; UINT u; } u; D3D11_VIEWPORT v2; ID3D11ShaderResourceView *nul = NULL;
    float iw = (float)s->width, ih = (float)s->height, ow = (float)dw, oh = (float)dh;
    if (g_fsr_fail || dw < 16 || dh < 16) return 0;
    if (!g_fsr_easu) {
        if (!fsr_compile(s, g_fsr_easu_src, &g_fsr_easu) || !fsr_compile(s, g_fsr_rcas_src, &g_fsr_rcas)) { g_fsr_fail = 1; fprintf(stderr, "[FSR] disabled (bilinear fallback); headers expected in <exe dir>\\fsr1 or STE_FSR_DIR\n"); return 0; }
        { D3D11_BUFFER_DESC bd; memset(&bd, 0, sizeof bd); bd.ByteWidth = 96; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; ID3D11Device_CreateBuffer(s->scanout_device, &bd, NULL, &g_fsr_cb); }
        fprintf(stderr, "[FSR] FidelityFX FSR 1.0 active: %.0fx%.0f -> %ux%u, sharpness %.2f stops\n", iw, ih, dw, dh, sharp);
    }
    if (!g_fsr_cb) { g_fsr_fail = 1; return 0; }
    if (!g_fsr_tex || g_fsr_w != dw || g_fsr_h != dh) {
        D3D11_TEXTURE2D_DESC td; memset(&td, 0, sizeof td);
        if (g_fsr_rtv) { ID3D11RenderTargetView_Release(g_fsr_rtv); g_fsr_rtv = NULL; } if (g_fsr_srv) { ID3D11ShaderResourceView_Release(g_fsr_srv); g_fsr_srv = NULL; } if (g_fsr_tex) { ID3D11Texture2D_Release(g_fsr_tex); g_fsr_tex = NULL; }
        td.Width = dw; td.Height = dh; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s->scanout_device, &td, NULL, &g_fsr_tex)) || FAILED(ID3D11Device_CreateRenderTargetView(s->scanout_device, (ID3D11Resource *)g_fsr_tex, NULL, &g_fsr_rtv)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(s->scanout_device, (ID3D11Resource *)g_fsr_tex, NULL, &g_fsr_srv))) { g_fsr_fail = 1; return 0; }
        g_fsr_w = dw; g_fsr_h = dh;
    }
    /* FsrEasuCon / FsrRcasCon of ffx_fsr1.h, written out (the CPU side of the header is not compiled here) */
#define FU(x) (u.f = (x), u.u)
    memset(&cb, 0, sizeof cb);
    cb.con[0][0] = FU(iw / ow); cb.con[0][1] = FU(ih / oh); cb.con[0][2] = FU(0.5f * iw / ow - 0.5f); cb.con[0][3] = FU(0.5f * ih / oh - 0.5f);
    cb.con[1][0] = FU(1.0f / iw); cb.con[1][1] = FU(1.0f / ih); cb.con[1][2] = FU(1.0f / iw); cb.con[1][3] = FU(-1.0f / ih);
    cb.con[2][0] = FU(-1.0f / iw); cb.con[2][1] = FU(2.0f / ih); cb.con[2][2] = FU(1.0f / iw); cb.con[2][3] = FU(2.0f / ih);
    cb.con[3][0] = FU(0.0f); cb.con[3][1] = FU(4.0f / ih);
    cb.rcas[0] = FU(exp2f(-sharp));
#undef FU
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); ID3D11DeviceContext_IASetInputLayout(c, NULL); ID3D11DeviceContext_VSSetShader(c, g_bvs, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &g_fsr_cb); ID3D11DeviceContext_PSSetSamplers(c, 0, 1, &g_bss);
    /* pass 1: EASU into the output-sized intermediate */
    cb.off[0] = cb.off[1] = 0.0f; ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)g_fsr_cb, 0, NULL, &cb, 0, 0);
    v2.TopLeftX = 0; v2.TopLeftY = 0; v2.Width = (FLOAT)dw; v2.Height = (FLOAT)dh; v2.MinDepth = 0; v2.MaxDepth = 1;
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &g_fsr_rtv, NULL); ID3D11DeviceContext_RSSetViewports(c, 1, &v2);
    ID3D11DeviceContext_PSSetShader(c, g_fsr_easu, NULL, 0); ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &g_bsrv); ID3D11DeviceContext_Draw(c, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &nul);
    /* pass 2: RCAS into the back buffer (letterboxed viewport) */
    cb.off[0] = vp->TopLeftX; cb.off[1] = vp->TopLeftY; ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)g_fsr_cb, 0, NULL, &cb, 0, 0);
    ID3D11DeviceContext_ClearRenderTargetView(c, bbrtv, black);
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &bbrtv, NULL); ID3D11DeviceContext_RSSetViewports(c, 1, vp);
    ID3D11DeviceContext_PSSetShader(c, g_fsr_rcas, NULL, 0); ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &g_fsr_srv); ID3D11DeviceContext_Draw(c, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &nul); ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, NULL);
    return 1;
}

static int g_scan_flip;   /* c180: flip-model swap chain with ALLOW_TEARING (vrr=auto and supported) */
static int g_tear_supported;   /* c251: DXGI_FEATURE_PRESENT_ALLOW_TEARING as reported by the system (logged at start-up) */
static char g_vrr_str[16] = "auto";
static int g_present_safe;     /* c251: 1 after a Present failure: interval 1, no tearing flag */
#define STE_EXIT_DEVICE_LOST 10   /* documented in docs\LAUNCHER_INTERFACE.md: the graphics device was removed / reset (driver crash, GPU unplugged) */

/* c251: the only place that presents. DXGI_PRESENT_ALLOW_TEARING is valid only with SyncInterval 0 on a flip-model chain created with the tearing flag, never in exclusive fullscreen.
 * vsync=1 -> Present(1, 0). Every failure is logged; device removed/reset exits with STE_EXIT_DEVICE_LOST; anything else drops to the safe path (1, 0) and carries on. */
static HRESULT scan_present(D3D8DeviceState *s)
{
    const D3D8VideoCfg *vc = d3d8_video_cfg();
    UINT sync = (vc->vsync || g_present_safe) ? 1u : 0u;
    UINT fl = (!sync && g_scan_flip && !g_present_safe && vc->mode != D3D8V_FULLSCREEN) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    HRESULT hr = IDXGISwapChain_Present(s->swap_chain, sync, fl);
    if (SUCCEEDED(hr)) return hr;
    {   static unsigned nfail; if (nfail++ < 20) { fprintf(stderr, "[VIDEO] Present(sync=%u, flags=0x%X) failed: 0x%08lX\n", sync, fl, (unsigned long)hr); fflush(stderr); } }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        HRESULT why = s->scanout_device ? ID3D11Device_GetDeviceRemovedReason(s->scanout_device) : hr;
        fprintf(stderr, "[VIDEO] graphics device lost (Present 0x%08lX, reason 0x%08lX): exiting with code %d\n", (unsigned long)hr, (unsigned long)why, STE_EXIT_DEVICE_LOST);
        fflush(stderr); ExitProcess(STE_EXIT_DEVICE_LOST);
    }
    if (!g_present_safe) {
        g_present_safe = 1;
        fprintf(stderr, "[VIDEO] present failed: switching to the safe path (SyncInterval 1, no tearing flag)\n"); fflush(stderr);
        hr = IDXGISwapChain_Present(s->swap_chain, 1u, 0u);
        if (FAILED(hr)) { fprintf(stderr, "[VIDEO] safe-path Present failed too: 0x%08lX\n", (unsigned long)hr); fflush(stderr); }
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) { fprintf(stderr, "[VIDEO] graphics device lost: exiting with code %d\n", STE_EXIT_DEVICE_LOST); fflush(stderr); ExitProcess(STE_EXIT_DEVICE_LOST); }
    }
    return hr;
}
static void scanout_present_frame(D3D8DeviceState *s, ID3D11Texture2D *bb)
{
    D3D11_TEXTURE2D_DESC bd; ID3D11RenderTargetView *rtv = NULL; D3D11_VIEWPORT vp; float black[4] = { 0, 0, 0, 1 };
    float ra, oa, dw, dh; ID3D11ShaderResourceView *nul = NULL;
    ID3D11Texture2D_GetDesc(bb, &bd);
    if (bd.Width == s->width && bd.Height == s->height) {
        ID3D11DeviceContext_CopyResource(s->scanout_context, (ID3D11Resource *)bb, (ID3D11Resource *)s->ready_texture);
        return;
    }
    if (!g_bvs && !scanout_blit_init(s)) return;
    if (FAILED(ID3D11Device_CreateRenderTargetView(s->scanout_device, (ID3D11Resource *)bb, NULL, &rtv))) return;
    ra = (float)s->width / (float)s->height; oa = (float)bd.Width / (float)bd.Height;
    dw = (float)bd.Width; dh = (float)bd.Height;
    if (d3d8_video_cfg()->aspect != D3D8V_ASPECT_STRETCH) {   /* c133: bars instead of a stretched frame whenever the window/swap-chain aspect differs from the render aspect (user resize/maximise) */
        if (oa > ra) dw = dh * ra; else dh = dw / ra;
    }
    vp.TopLeftX = ((float)bd.Width - dw) * 0.5f; vp.TopLeftY = ((float)bd.Height - dh) * 0.5f;
    vp.Width = dw; vp.Height = dh; vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    if (d3d8_video_cfg()->upscale && (UINT)s->width < (UINT)(dw + 0.5f) && fsr_present(s, rtv, &vp)) { ID3D11RenderTargetView_Release(rtv); return; }   /* c180: FSR 1.0 only when the render surface is smaller than the viewport */
    ID3D11DeviceContext_ClearRenderTargetView(s->scanout_context, rtv, black);
    ID3D11DeviceContext_OMSetRenderTargets(s->scanout_context, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(s->scanout_context, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(s->scanout_context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(s->scanout_context, NULL);
    ID3D11DeviceContext_VSSetShader(s->scanout_context, g_bvs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s->scanout_context, g_bps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s->scanout_context, 0, 1, &g_bsrv);
    ID3D11DeviceContext_PSSetSamplers(s->scanout_context, 0, 1, &g_bss);
    ID3D11DeviceContext_Draw(s->scanout_context, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s->scanout_context, 0, 1, &nul);
    ID3D11DeviceContext_OMSetRenderTargets(s->scanout_context, 0, NULL, NULL);
    ID3D11RenderTargetView_Release(rtv);
}

static DWORD WINAPI d3d8_scanout_thread(void *opaque)
{
    D3D8DeviceState *state = (D3D8DeviceState *)opaque;
    LARGE_INTEGER frequency, now;
    LONGLONG next_present, stats_start, period;
    unsigned presented = 0, fresh = 0, repeated = 0;
    unsigned repeat_run = 0, max_repeat_run = 0;
    LONG copied_generation = -1;
    LONG presented_generation = -1;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    next_present = stats_start = now.QuadPart;
    {   /* display rate: 60 Hz (120 when uncapped); with vsync on the blocking Present(1) paces the loop */
        const D3D8VideoCfg *vc = d3d8_video_cfg();
        period = vc->vsync ? 0 : frequency.QuadPart / (vc->fps_cap == 0 ? 120 : 60);
        if (g_scan_flip && !vc->vsync) period = frequency.QuadPart / 1000;   /* c180 vrr: poll for a new frame every 1 ms instead of a fixed 60 Hz present clock */
        if (period <= 0) period = frequency.QuadPart / 1000;   /* c251: vsync on left period at 0 and the catch-up division below raised INT_DIVIDE_BY_ZERO (0xC0000094) on the first frame */
    }

    while (InterlockedCompareExchange(&state->scanout_active, 0, 0)) {
        ID3D11Texture2D *back_buffer = NULL;
        HRESULT hr = E_FAIL;
        LONG generation;

        QueryPerformanceCounter(&now);
        while (!d3d8_video_cfg()->vsync && now.QuadPart < next_present &&   /* c251: with vsync on the blocking Present(1) is the pacing */
               InterlockedCompareExchange(&state->scanout_active, 0, 0)) {
            LONGLONG remaining = next_present - now.QuadPart;
            if (remaining > frequency.QuadPart / 500 || g_scan_flip)   /* c181: flip/vrr mode polls every 1 ms; spinning (SwitchToThread) there burned one full core */
                Sleep(1);
            else
                SwitchToThread();
            QueryPerformanceCounter(&now);
        }
        if (!InterlockedCompareExchange(&state->scanout_active, 0, 0))
            break;

        {   /* c133: the user resized / maximised the window: follow the client area with the swap chain (otherwise DXGI stretches the old-size buffer, distorting the frame).
             * The picture keeps the render aspect (bars, see scanout_present_frame); the render size / logical width are fixed at start-up. */
            RECT cr; static UINT bad_w, bad_h;
            if (state->hwnd && d3d8_video_cfg()->mode != D3D8V_FULLSCREEN && !IsIconic(state->hwnd) && GetClientRect(state->hwnd, &cr)) {
                UINT cw = (UINT)(cr.right - cr.left), ch = (UINT)(cr.bottom - cr.top);
                if (cw > 0 && ch > 0 && (cw != state->out_w || ch != state->out_h) && !(cw == bad_w && ch == bad_h)) {
                    HRESULT rh = IDXGISwapChain_ResizeBuffers(state->swap_chain, 0, cw, ch, DXGI_FORMAT_UNKNOWN, g_scan_flip ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
                    fprintf(stderr, "[VIDEO] window resize %ux%u -> %ux%u: ResizeBuffers = 0x%08lX\n", state->out_w, state->out_h, cw, ch, (unsigned long)rh);
                    if (SUCCEEDED(rh)) { state->out_w = cw; state->out_h = ch; bad_w = bad_h = 0; } else { bad_w = cw; bad_h = ch; }
                }
            }
        }
        generation = InterlockedCompareExchange(
            &state->scanout_generation, 0, 0);
        /* c180 vrr: present only when a new frame is ready (an unchanged frame is re-presented at most every 100 ms, to keep the window alive) */
        { static LONGLONG last_flip_present; int skip_flip = g_scan_flip && generation == copied_generation && now.QuadPart - last_flip_present < frequency.QuadPart / 10;
        if (!skip_flip && g_scan_flip) last_flip_present = now.QuadPart;
        if (!skip_flip &&
            InterlockedCompareExchange(&state->scanout_ready, 0, 0) &&
            state->swap_chain && state->ready_texture &&
            SUCCEEDED(IDXGISwapChain_GetBuffer(
                state->swap_chain, 0, &IID_ID3D11Texture2D,
                (void **)&back_buffer))) {
            /* DXGI_SWAP_EFFECT_SEQUENTIAL preserves the backbuffer.  Reuse it
             * while the guest is stalled instead of taking the D3D11
             * immediate-context lock to copy the same texture 30 times/sec. */
            if (generation != copied_generation && state->shared_on_scan && state->km_scan) {
                /* new frame published through the shared texture: pull it into ready_texture (GPU copy), then present */
                if (IDXGIKeyedMutex_AcquireSync(state->km_scan, 1, 0) == S_OK) {
                    ID3D11DeviceContext_CopyResource(state->scanout_context, (ID3D11Resource *)state->ready_texture,
                                                     (ID3D11Resource *)state->shared_on_scan);
                    IDXGIKeyedMutex_ReleaseSync(state->km_scan, 0);
                    scanout_present_frame(state, back_buffer);
                    copied_generation = generation;
                }
            } else
            if (generation != copied_generation) {
                if (generation == InterlockedCompareExchange(
                        &state->scanout_cpu_generation, 0, 0) &&
                    state->scanout_cpu_frame) {
                    if (TryEnterCriticalSection(&state->scanout_cpu_lock)) {
                        ID3D11DeviceContext_UpdateSubresource(
                            state->scanout_context,
                            (ID3D11Resource *)state->ready_texture,
                            0, NULL, state->scanout_cpu_frame,
                            state->width * 4u,
                            state->width * state->height * 4u);
                        LeaveCriticalSection(&state->scanout_cpu_lock);
                    } else {
                        /* A producer is still completing this generation.
                         * Present the last complete backbuffer on schedule and
                         * retry the new frame at the next 30 Hz slot. */
                        ID3D11Texture2D_Release(back_buffer);
                        hr = scan_present(state);
                        goto scanout_presented;
                    }
                }
                scanout_present_frame(state, back_buffer);
                copied_generation = generation;
            } else if (g_scan_flip) scanout_present_frame(state, back_buffer);   /* flip model: the buffers rotate, so every present is drawn again */
            {   /* c133 probe: STE_SWAPCHAIN_DUMP=<dir>: every 3 s write what is really presented (the swap-chain back buffer, i.e. window client size incl. letterbox bars) as sc_<n>_<w>x<h>.bmp */
                static const char *dd = (const char *)1; static ULONGLONG last; static unsigned n;
                if (dd == (const char *)1) dd = getenv("STE_SWAPCHAIN_DUMP");
                static ULONGLONG every = 0; if (!every) { const char *em = getenv("STE_SWAPCHAIN_DUMP_MS"); every = (em && atoi(em) > 0) ? (ULONGLONG)atoi(em) : 3000; }   /* c257: dump interval in ms (default 3000) */
                if (dd && *dd && GetTickCount64() - last >= every) {
                    D3D11_TEXTURE2D_DESC sd; ID3D11Texture2D *st = NULL; last = GetTickCount64();
                    ID3D11Texture2D_GetDesc(back_buffer, &sd); sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
                    if (SUCCEEDED(ID3D11Device_CreateTexture2D(state->scanout_device, &sd, NULL, &st))) {
                        D3D11_MAPPED_SUBRESOURCE mp;
                        ID3D11DeviceContext_CopyResource(state->scanout_context, (ID3D11Resource *)st, (ID3D11Resource *)back_buffer);
                        if (SUCCEEDED(ID3D11DeviceContext_Map(state->scanout_context, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &mp))) {
                            char path[MAX_PATH]; FILE *fp; unsigned w = sd.Width, h = sd.Height, y;
                            CreateDirectoryA(dd, NULL); _snprintf(path, sizeof(path), "%s\\sc_%03u_%ux%u.bmp", dd, n++, w, h);
                            fp = fopen(path, "wb");
                            if (fp) { unsigned char hd[54] = { 'B', 'M' }; unsigned sz = 54 + w * h * 4; memcpy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40; memcpy(hd + 18, &w, 4); { int nh = -(int)h; memcpy(hd + 22, &nh, 4); } hd[26] = 1; hd[28] = 32;
                                fwrite(hd, 1, 54, fp); for (y = 0; y < h; ++y) fwrite((const unsigned char *)mp.pData + (size_t)y * mp.RowPitch, 1, (size_t)w * 4, fp); fclose(fp); }
                            ID3D11DeviceContext_Unmap(state->scanout_context, (ID3D11Resource *)st, 0);
                        }
                        ID3D11Texture2D_Release(st);
                    }
                }
            }
            ID3D11Texture2D_Release(back_buffer);
            hr = scan_present(state);
scanout_presented:
            if (SUCCEEDED(hr) && presented == 0u) {
                char capture_first[2];
                if (GetEnvironmentVariableA("XBOXRECOMP_CAPTURE_FIRST_SCANOUT",
                                            capture_first,
                                            sizeof(capture_first)))
                    d3d8_capture_backbuffer_bmp(
                        "build-release\\first_scanout.bmp");
            }
        }
        }   /* c180 skip_flip scope */
        if (d3d8_video_cfg()->vsync && FAILED(hr)) Sleep(1);   /* c251: nothing was presented (no new frame yet): do not spin, the blocking Present is not there to wait */

        next_present += period;
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= next_present) {
            /* Preserve the original 30 Hz phase after a temporary scheduling
             * delay.  Rebasing to now+period compounds one late present into
             * another full-frame pause; advancing by whole periods catches
             * the next real scanout slot without emitting burst frames. */
            const LONGLONG missed =
                (now.QuadPart - next_present) / period + 1;
            next_present += missed * period;
        }
        if (SUCCEEDED(hr)) {
            /* A healthy refresh thread can still repeat a stalled game's
             * last frame. Report publication progress separately; this is
             * not a claim that every submitted frame has different pixels. */
            if (copied_generation != presented_generation) {
                ++fresh;
                repeat_run = 0;
                presented_generation = copied_generation;
            } else {
                ++repeated;
                if (++repeat_run > max_repeat_run)
                    max_repeat_run = repeat_run;
            }
        }
        if (SUCCEEDED(hr) && ++presented == 300u) {
            LONGLONG elapsed = now.QuadPart - stats_start;
            unsigned fps_milli = elapsed > 0
                ? (unsigned)(((uint64_t)presented * 1000u *
                              frequency.QuadPart) / (uint64_t)elapsed)
                : 0u;
            fprintf(stderr,
                    "[SCANOUT-CADENCE] frames=%u fps=%u.%03u fresh=%u "
                    "repeated=%u max-repeat-run=%u\n",
                    presented, fps_milli / 1000u, fps_milli % 1000u,
                    fresh, repeated, max_repeat_run);
            fflush(stderr);
            presented = 0;
            fresh = repeated = 0;
            max_repeat_run = repeat_run;
            stats_start = now.QuadPart;
        }
    }
    return 0;
}

static void d3d8_start_scanout(D3D8DeviceState *state)
{
    if (!state->scanout_thread) {
        InterlockedExchange(&state->scanout_active, 1);
        state->scanout_thread = CreateThread(
            NULL, 0, d3d8_scanout_thread, state, 0, NULL);
        if (!state->scanout_thread)
            InterlockedExchange(&state->scanout_active, 0);
        else
            /* Scanout must remain schedulable while the guest thread parses
             * and uploads a submenu bundle.  It only performs one bounded
             * upload/present per 33.3 ms, so time-critical priority is safe
             * and prevents asset work from starving the display clock. */
            SetThreadPriority(state->scanout_thread,
                              THREAD_PRIORITY_TIME_CRITICAL);
    }
    if (!InterlockedExchange(&state->scanout_visible, 1) && state->hwnd) {
        ShowWindowAsync(state->hwnd, SW_SHOW);
        UpdateWindow(state->hwnd);
    }
}

void d3d8_SubmitRGBAFrame(const void *pixels, UINT row_pitch,
                          UINT slice_pitch)
{
    D3D8DeviceState *state = &g_device_state;
    LONG generation;
    UINT y;
    const BYTE *source = (const BYTE *)pixels;
    (void)slice_pitch;
    if (!pixels || !state->scanout_cpu_frame ||
        !state->scanout_cpu_lock_initialized)
        return;
    /* Rendering and movie decode must never queue behind scanout.  A newer
     * frame arrives on the next tick, so dropping only a contended mailbox
     * write is preferable to stalling either producer and cascading latency. */
    if (!TryEnterCriticalSection(&state->scanout_cpu_lock))
        return;
    for (y = 0; y < state->height; ++y)
        memcpy(state->scanout_cpu_frame + (size_t)y * state->width * 4u,
               source + (size_t)y * row_pitch, state->width * 4u);
    generation = InterlockedIncrement(&state->scanout_generation);
    InterlockedExchange(&state->scanout_cpu_generation, generation);
    LeaveCriticalSection(&state->scanout_cpu_lock);
    InterlockedExchange(&state->scanout_ready, 1);
    d3d8_start_scanout(state);
}

/* Temporary bring-up aid: capture the actual D3D11 backbuffer so projects can
 * verify visual progress even when desktop window capture is unavailable. */
static BOOL d3d8_capture_backbuffer_bmp(const char *path)
{
    ID3D11Texture2D *back_buffer = NULL;
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    BITMAPFILEHEADER file_header;
    BITMAPINFOHEADER info_header;
    unsigned char *row = NULL;
    FILE *file = NULL;
    ID3D11Device *capture_device = g_device_state.d3d11_device;
    ID3D11DeviceContext *capture_context = g_device_state.d3d11_context;
    HRESULT hr;
    BOOL ok = FALSE;

    if (g_device_state.scanout_ready && g_device_state.ready_texture) {
        back_buffer = g_device_state.ready_texture;
        capture_device = g_device_state.scanout_device;
        capture_context = g_device_state.scanout_context;
        ID3D11Texture2D_AddRef(back_buffer);
        hr = S_OK;
    } else if (g_device_state.render_texture) {
        back_buffer = g_device_state.render_texture;
        ID3D11Texture2D_AddRef(back_buffer);
        hr = S_OK;
    } else {
        hr = IDXGISwapChain_GetBuffer(g_device_state.swap_chain, 0,
                                      &IID_ID3D11Texture2D,
                                      (void **)&back_buffer);
        capture_device = g_device_state.scanout_device;
        capture_context = g_device_state.scanout_context;
    }
    if (FAILED(hr)) goto cleanup;

    ID3D11Texture2D_GetDesc(back_buffer, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(capture_device, &desc,
                                      NULL, &staging);
    if (FAILED(hr)) goto cleanup;

    ID3D11DeviceContext_CopyResource(capture_context,
                                     (ID3D11Resource *)staging,
                                     (ID3D11Resource *)back_buffer);
    memset(&mapped, 0, sizeof(mapped));
    hr = ID3D11DeviceContext_Map(capture_context,
                                 (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) goto cleanup;

    row = (unsigned char *)malloc((size_t)desc.Width * 4u);
    if (!row) {
        ID3D11DeviceContext_Unmap(capture_context,
                                  (ID3D11Resource *)staging, 0);
        goto cleanup;
    }

    file = fopen(path, "wb");
    if (!file) {
        ID3D11DeviceContext_Unmap(capture_context,
                                  (ID3D11Resource *)staging, 0);
        goto cleanup;
    }

    memset(&file_header, 0, sizeof(file_header));
    memset(&info_header, 0, sizeof(info_header));
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(file_header) + sizeof(info_header);
    file_header.bfSize = file_header.bfOffBits + desc.Width * desc.Height * 4u;
    info_header.biSize = sizeof(info_header);
    info_header.biWidth = (LONG)desc.Width;
    info_header.biHeight = (LONG)desc.Height;
    info_header.biPlanes = 1;
    info_header.biBitCount = 32;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = desc.Width * desc.Height * 4u;

    fwrite(&file_header, sizeof(file_header), 1, file);
    fwrite(&info_header, sizeof(info_header), 1, file);
    for (LONG y = (LONG)desc.Height - 1; y >= 0; --y) {
        const unsigned char *src =
            (const unsigned char *)mapped.pData + (size_t)y * mapped.RowPitch;
        for (UINT x = 0; x < desc.Width; ++x) {
            row[x * 4u + 0u] = src[x * 4u + 2u];
            row[x * 4u + 1u] = src[x * 4u + 1u];
            row[x * 4u + 2u] = src[x * 4u + 0u];
            row[x * 4u + 3u] = 0xFFu;
        }
        fwrite(row, (size_t)desc.Width * 4u, 1, file);
    }
    ok = TRUE;
    ID3D11DeviceContext_Unmap(capture_context,
                              (ID3D11Resource *)staging, 0);

cleanup:
    if (file) fclose(file);
    free(row);
    if (staging) ID3D11Texture2D_Release(staging);
    if (back_buffer) ID3D11Texture2D_Release(back_buffer);
    fprintf(stderr, "D3D8: framebuffer capture %s: %s\n",
            ok ? "written" : "failed", path);
    fflush(stderr);
    return ok;
}

/* Public: save the current backbuffer (debug captures from title code). */
void d3d8_capture_backbuffer_bmp_public(const char *path)
{
    d3d8_capture_backbuffer_bmp(path);
}

void d3d8_video_pace(void)
{
    static LARGE_INTEGER f; static LONGLONG next; static int inited;
    const D3D8VideoCfg *c = d3d8_video_cfg();
    LARGE_INTEGER now; LONGLONG period;
    if (c->fps_cap <= 0 || c->fps_cap >= 60) return;
    if (!inited) { QueryPerformanceFrequency(&f); timeBeginPeriod(1); inited = 1; }
    period = f.QuadPart / c->fps_cap;
    QueryPerformanceCounter(&now);
    if (!next) next = now.QuadPart;
    next += period;
    while (now.QuadPart < next) {
        if (next - now.QuadPart > f.QuadPart / 500) Sleep(1); else YieldProcessor();   /* coarse sleep, spin the last 2 ms */
        QueryPerformanceCounter(&now);
    }
    if (now.QuadPart - next > period) next = now.QuadPart;   /* far behind: resynchronise, never burst */
}

static HRESULT d3d8_present(void)
{
    HRESULT hr = d3d8_gamma_begin(g_device_state.default_rtv);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Gamma presentation failed: 0x%08lX\n", hr);
        return hr;
    }
    hr = IDXGISwapChain_Present(g_device_state.swap_chain, 1, 0);
    d3d8_gamma_end(g_device_state.default_rtv);
    return hr;
}

/* ================================================================
 * Public frame pump (called from recompiled game code)
 * ================================================================ */
extern void d3d8_states_invalidate_bindings(void);
static void gpustat_frame(void);
void d3d8_PresentFrame(void)
{
    gpustat_frame();   /* c139b: the frame boundary of the guest render device (d3d8_rt_present is not reached in every path) */
    d3d8_states_invalidate_bindings();
    d3d8_debug_drain();
    static DWORD present_count = 0;

    present_count++;
    {
        /* XBOXRECOMP_CAPTURE="n,n,..." overrides the default capture frames. */
        static DWORD frames[32];
        static int frame_count = -1;
        if (frame_count < 0) {
            char spec[256];
            frame_count = 0;
            if (GetEnvironmentVariableA("XBOXRECOMP_CAPTURE", spec, sizeof(spec))) {
                char *p = spec;
                frame_count = 0;
                while (*p && frame_count < 32) {
                    frames[frame_count++] = strtoul(p, &p, 10);
                    while (*p == ',' || *p == ' ') p++;
                }
            }
        }
        for (int i = 0; i < frame_count; ++i) {
            if (present_count == frames[i]) {
                char path[64];
                sprintf_s(path, sizeof(path), "build-release\\frame_%04lu.bmp",
                          (unsigned long)present_count);
                d3d8_capture_backbuffer_bmp(path);
            }
        }
    }

    /* Pump Windows messages */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            /* ExitProcess skips atexit, where the summary would print. */
            recomp_exit_hook_run("window");
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    /* Publish a complete frame atomically.  The independent 30 Hz scanout
     * thread repeats this image if guest emulation or asset loading stalls,
     * so host presentation never inherits guest scheduling jitter. */
    if (g_device_state.shared_texture && g_device_state.km_prod && g_device_state.render_texture) {
        /* Mailbox: if the scan-out side still holds the texture, drop this frame (a newer one follows on the next swap). */
        if (IDXGIKeyedMutex_AcquireSync(g_device_state.km_prod, 0, 0) == S_OK) {
            ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context, (ID3D11Resource *)g_device_state.shared_texture,
                                             (ID3D11Resource *)g_device_state.render_texture);
            IDXGIKeyedMutex_ReleaseSync(g_device_state.km_prod, 1);
            InterlockedIncrement(&g_device_state.scanout_generation);
            InterlockedExchange(&g_device_state.scanout_ready, 1);
            d3d8_start_scanout(&g_device_state);
        }
    } else
    if (g_device_state.render_texture && g_device_state.readback_texture &&
        g_device_state.scanout_cpu_frame) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        UINT y;
        LONG generation;
        ID3D11DeviceContext_CopyResource(
            g_device_state.d3d11_context,
            (ID3D11Resource *)g_device_state.readback_texture,
            (ID3D11Resource *)g_device_state.render_texture);
        memset(&mapped, 0, sizeof(mapped));
        if (SUCCEEDED(ID3D11DeviceContext_Map(
                g_device_state.d3d11_context,
                (ID3D11Resource *)g_device_state.readback_texture,
                0, D3D11_MAP_READ, 0, &mapped))) {
            EnterCriticalSection(&g_device_state.scanout_cpu_lock);
            for (y = 0; y < g_device_state.height; ++y)
                memcpy(g_device_state.scanout_cpu_frame +
                           (size_t)y * g_device_state.width * 4u,
                       (const BYTE *)mapped.pData +
                           (size_t)y * mapped.RowPitch,
                       g_device_state.width * 4u);
            generation = InterlockedIncrement(
                &g_device_state.scanout_generation);
            InterlockedExchange(&g_device_state.scanout_cpu_generation,
                                generation);
            LeaveCriticalSection(&g_device_state.scanout_cpu_lock);
            ID3D11DeviceContext_Unmap(
                g_device_state.d3d11_context,
                (ID3D11Resource *)g_device_state.readback_texture, 0);
            InterlockedExchange(&g_device_state.scanout_ready, 1);
            d3d8_start_scanout(&g_device_state);
        }
    }
}

/* ================================================================
 * Internal accessors (used by d3d8_resources/shaders/states)
 * ================================================================ */

IDirect3DDevice8    *d3d8_GetDevice(void) { return &g_device; }
ID3D11Device        *d3d8_GetD3D11Device(void) { return g_device_state.d3d11_device; }
ID3D11DeviceContext *d3d8_GetD3D11Context(void) { return g_device_state.d3d11_context; }
IDXGISwapChain      *d3d8_GetSwapChain(void) { return g_device_state.swap_chain; }
ID3D11RenderTargetView *d3d8_GetDefaultRTV(void) { return g_device_state.default_rtv; }
HWND                 d3d8_GetHWND(void) { return g_device_state.hwnd; }
/* ================================================================
 * Offscreen render targets for the NV2A translator.
 *
 * NV2A titles render into arbitrary RAM surfaces (SET_SURFACE_COLOR_OFFSET)
 * and later sample them as textures; xemu keeps a surface cache for this.
 * Here each non-display surface address gets a D3D11 colour + depth target,
 * and the translator binds it while that surface is current and samples it
 * through d3d8_rt_texture() when a texture points at the same address.
 * "Current target" state (RTV/DSV/size) is what Clear, the viewport and the
 * pre-transformed vertex mapping (ScreenSize) follow.
 * ================================================================ */
typedef struct {
    uint32_t key;
    UINT w, h;               /* guest size */
    UINT hw, hh;             /* host size (scaled) */
    ID3D11Texture2D *tex, *dtex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    ID3D11DepthStencilView *dsv;
    IDirect3DTexture8 *wrap;
    unsigned last_use;       /* g_rt_tick when last selected as a target */
    int stale;               /* c172: the guest freed the memory this target lived in: not sampled as a texture any more, but kept so a new target of the same address and size reuses it (re-creating 4K targets on every free/alloc stalled the GPU for seconds) */
} D3D8RtEntry;
static D3D8RtEntry g_rts[48];
static unsigned g_rt_tick;
static ID3D11RenderTargetView *g_cur_rtv;
static ID3D11DepthStencilView *g_cur_dsv;
static UINT g_cur_w, g_cur_h;

static ID3D11RenderTargetView *cur_rtv(void) { return g_cur_rtv ? g_cur_rtv : g_device_state.default_rtv; }
static ID3D11DepthStencilView *cur_dsv(void) { return g_cur_rtv ? g_cur_dsv : g_device_state.default_dsv; }

static void bind_target(ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv, UINT w, UINT h)
{
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *nulls[4] = { NULL, NULL, NULL, NULL };
    /* a target cannot stay bound as a shader input while drawn into */
    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context, 0, 4, nulls);
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context, 1, &rtv, dsv);
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = (FLOAT)w; vp.Height = (FLOAT)h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &vp);
}

/* Release every resource of a render-target entry, including the texture wrapper (its SRV reference kept the
 * colour texture alive when the old code memset the entry without releasing it). */
static void rt_release(D3D8RtEntry *e)
{
    if (e->wrap) e->wrap->lpVtbl->Release(e->wrap);
    if (e->rtv) ID3D11RenderTargetView_Release(e->rtv);
    if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
    if (e->dsv) ID3D11DepthStencilView_Release(e->dsv);
    if (e->tex) ID3D11Texture2D_Release(e->tex);
    if (e->dtex) ID3D11Texture2D_Release(e->dtex);
    memset(e, 0, sizeof(*e));
}

/* Render targets are keyed by (guest address, size). A title that reuses an address for a different size gets a
 * second entry instead of the first being rebuilt (and cleared) in place -- that wiped the 640x480 scene surface
 * whenever a later pass picked a 256x256 clip. A lookup without create returns the most recently selected entry
 * for the address. When all slots are taken the least recently selected one that is not bound is evicted. */
static D3D8RtEntry *rt_get(uint32_t key, UINT w, UINT h, int create)
{
    int i, free_slot = -1, best = -1, lru = -1;
    for (i = 0; i < 48; ++i) {
        if (!g_rts[i].tex) { if (free_slot < 0) free_slot = i; continue; }
        if (g_rts[i].key == key) {
            if (create && g_rts[i].w == w && g_rts[i].h == h) { g_rts[i].last_use = ++g_rt_tick; g_rts[i].stale = 0; return &g_rts[i]; }
            if (!create && !g_rts[i].stale && (best < 0 || g_rts[i].last_use > g_rts[best].last_use)) best = i;
        }
        if (g_rts[i].rtv != g_cur_rtv && (lru < 0 || g_rts[i].last_use < g_rts[lru].last_use)) lru = i;
    }
    if (!create) return best >= 0 ? &g_rts[best] : NULL;
    if (!w || !h) return NULL;
    if (free_slot < 0) {
        if (lru < 0) return NULL;
        rt_release(&g_rts[lru]);
        free_slot = lru;
    }
    {
        D3D8RtEntry *e = &g_rts[free_slot];
        D3D11_TEXTURE2D_DESC td;
        ID3D11Device *dev = g_device_state.d3d11_device;
        if (e->rtv) ID3D11RenderTargetView_Release(e->rtv);
        if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
        if (e->dsv) ID3D11DepthStencilView_Release(e->dsv);
        if (e->tex) ID3D11Texture2D_Release(e->tex);
        if (e->dtex) ID3D11Texture2D_Release(e->dtex);
        memset(e, 0, sizeof(*e));
        memset(&td, 0, sizeof(td));
        {   /* screen-sized guest surfaces follow the display surface; the rest scale by the same factor */
            const D3D8VideoCfg *vc = d3d8_video_cfg();
            if (w == 640 && h == 480) { e->hw = vc->render_w; e->hh = vc->render_h; }
            else { e->hw = (UINT)((float)w * vc->scale + 0.5f); e->hh = (UINT)((float)h * vc->scale + 0.5f); }
            if (e->hw < 1) e->hw = 1; if (e->hh < 1) e->hh = 1;
        }
        td.Width = e->hw; td.Height = e->hh; td.MipLevels = 1; td.ArraySize = 1;
        /* Match the renderer's publication texture.  Render targets are
         * promoted with CopyResource at a guest frame boundary, which
         * requires an identical DXGI format.  The old BGRA surface could
         * neither be copied to the RGBA publication texture nor directly to
         * the swapchain owned by the separate scanout device. */
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &e->tex))) return NULL;
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)e->tex, NULL, &e->rtv);
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)e->tex, NULL, &e->srv);
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &e->dtex)))
            ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)e->dtex, NULL, &e->dsv);
        e->key = key; e->w = w; e->h = h; e->last_use = ++g_rt_tick;
        {
            /* expose as an ordinary IDirect3DTexture8 whose view is the RT */
            IDirect3DTexture8 *t = NULL;
            if (SUCCEEDED(d3d8_CreateTextureImpl(1, 1, 1, 0, D3DFMT_LIN_A8R8G8B8, &t)) && t) {
                D3D8Texture *dt = (D3D8Texture *)t;
                if (dt->srv) ID3D11ShaderResourceView_Release(dt->srv);
                ID3D11ShaderResourceView_AddRef(e->srv);
                dt->srv = e->srv;
                dt->width = w; dt->height = h;
                dt->dirty = FALSE;
                e->wrap = t;
            }
        }
        {
            float black[4] = {0, 0, 0, 0};
            ID3D11DeviceContext_ClearRenderTargetView(g_device_state.d3d11_context, e->rtv, black);
            if (e->dsv)
                ID3D11DeviceContext_ClearDepthStencilView(g_device_state.d3d11_context, e->dsv,
                    D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        }
        return e;
    }
}

/* Make guest surface `key` current (0 = the display back buffer). */
void d3d8_rt_select(uint32_t key, UINT w, UINT h)
{
    if (!key) {
        g_cur_rtv = NULL; g_cur_dsv = NULL; g_cur_w = g_cur_h = 0;
        bind_target(g_device_state.default_rtv, g_device_state.default_dsv,
                    g_device_state.width, g_device_state.height);
        return;
    }
    {
        D3D8RtEntry *e = rt_get(key, w, h, 1);
        if (!e) return;
        g_cur_rtv = e->rtv; g_cur_dsv = e->dsv; g_cur_w = e->w; g_cur_h = e->h;
        bind_target(e->rtv, e->dsv, e->hw, e->hh);
    }
}

/* STE c54 probe: write guest render target `key` (0 = display back buffer) to a BMP; returns mean luma 0..255 (-1 on failure). */
float d3d8_rt_capture_bmp(uint32_t key, const char *path, UINT *ow, UINT *oh)
{
    D3D8RtEntry *e = key ? rt_get(key, 0, 0, 0) : NULL;
    ID3D11Texture2D *src = key ? (e ? e->tex : NULL) : g_device_state.render_texture;
    D3D11_TEXTURE2D_DESC desc; D3D11_MAPPED_SUBRESOURCE mp; ID3D11Texture2D *st = NULL;
    ID3D11DeviceContext *ctx = g_device_state.d3d11_context; FILE *f; UINT x; LONG y; unsigned char *row;
    BITMAPFILEHEADER fh; BITMAPINFOHEADER ih; double sum = 0; float mean = -1.0f;
    if (!src || !ctx) return -1.0f;
    ID3D11Texture2D_GetDesc(src, &desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_device_state.d3d11_device, &desc, NULL, &st))) return -1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)src);
    memset(&mp, 0, sizeof(mp));
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &mp))) {
        f = fopen(path, "wb"); row = (unsigned char *)malloc((size_t)desc.Width * 4u);
        if (f && row) {
            memset(&fh, 0, sizeof(fh)); memset(&ih, 0, sizeof(ih));
            fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + desc.Width * desc.Height * 4u;
            ih.biSize = sizeof(ih); ih.biWidth = (LONG)desc.Width; ih.biHeight = (LONG)desc.Height; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
            fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
            for (y = (LONG)desc.Height - 1; y >= 0; --y) {
                const unsigned char *s = (const unsigned char *)mp.pData + (size_t)y * mp.RowPitch;
                for (x = 0; x < desc.Width; ++x) {
                    unsigned r = s[x*4+0], g = s[x*4+1], b = s[x*4+2];   /* RGBA8 or BGRA8: luma only, so order is irrelevant for the mean */
                    row[x*4+0] = (unsigned char)(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? r : b);
                    row[x*4+1] = (unsigned char)g;
                    row[x*4+2] = (unsigned char)(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? b : r);
                    row[x*4+3] = 0xFF;
                    sum += (r + g + b) / 3.0;
                }
                fwrite(row, (size_t)desc.Width * 4u, 1, f);
            }
            mean = (float)(sum / ((double)desc.Width * desc.Height));
        }
        if (f) fclose(f);
        free(row);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        if (ow) *ow = desc.Width;
        if (oh) *oh = desc.Height;
    }
    ID3D11Texture2D_Release(st);
    return mean;
}

/* c165/c172: mark the render targets whose guest surface overlaps [phys, phys+bytes) stale: that memory was freed, so a later texture at the same
 * address is ordinary guest memory again (get_texture no longer finds them). Entries are NOT released or re-keyed: one may still be referenced by an
 * earlier draw of the frame in progress, and a new target of the same address and size reuses the entry (rt_get create path clears the flag), which keeps
 * the number of 4K-sized targets bounded. Returns how many entries were marked. */
int d3d8_rt_invalidate_range(uint32_t phys, uint32_t bytes)
{
    int i, n = 0;
    for (i = 0; i < 48; ++i) {
        uint32_t k = g_rts[i].key, len;
        if (!g_rts[i].tex || g_rts[i].stale) continue;
        len = g_rts[i].w * g_rts[i].h * 4u;
        if (k < phys + bytes && phys < k + len) { g_rts[i].stale = 1; n++; }
    }
    return n;
}

/* Texture view of a guest surface rendered earlier, or NULL. */
IDirect3DTexture8 *d3d8_rt_texture(uint32_t key)
{
    D3D8RtEntry *e = rt_get(key, 0, 0, 0);
    return e ? e->wrap : NULL;
}

/* Copy a completed guest render target to the host display backbuffer.
 *
 * During initial pushbuffer catch-up the translator may encounter several
 * historical colour surfaces before it knows which one ended the frame.  The
 * frame-ending surface is authoritative, but its first rendering may already
 * have gone to an offscreen texture.  Promote that completed image here rather
 * than forcing every intermediate render-to-texture pass onto the display. */
/* c139 probe: STE_GPU_STATS=1 measures, per Kelvin frame, the GPU time (timestamp queries) and the pipeline statistics (CInvocations = triangles reaching the clipper, CPrimitives = triangles
 * that survive clip + cull and are rasterised) of the guest render device; a line [GPUSTAT] is printed every 300 frames.  Logging only. */
/* c178: GPU-busy span per frame (STE_GPU_BUSY=1, needs STE_GPU_STATS=1): a timestamp after the first draw and one after the last draw of the frame (d3d8_gpubusy_mark is called after every
 * host draw), read back a few frames later. busy_ms = last - first = the GPU timeline from the first to the last draw of the guest frame (an UPPER bound of true busy time: it also holds any
 * GPU starvation inside the frame; frame-boundary wall time, the old gpu_ms, is not used). Also logs the process VRAM (DXGI QueryVideoMemoryInfo) and the texture-pack upload time per window. */
static ID3D11Query *g_gb_qf[6], *g_gb_ql[6]; static int g_gb_slot = -1, g_gb_has[6]; static unsigned g_gb_draws[6]; static int g_gb_on = -1;
void d3d8_gpubusy_mark(void)
{
    int s = g_gb_slot; ID3D11DeviceContext *c;
    if (s < 0 || !g_gb_qf[s]) return;
    c = g_device_state.d3d11_context;
    if (!g_gb_has[s]) { ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)g_gb_qf[s]); g_gb_has[s] = 1; }
    ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)g_gb_ql[s]); g_gb_draws[s]++;
}
static int gb_cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void gpustat_frame(void)
{
    enum { N = 6 };
    static int on = -1, wr, rd, begun, inited, nfr; static ID3D11Query *qd[N], *qa[N], *qb[N], *qp[N];
    static double sum_ms, max_ms; static unsigned long long sum_inv, sum_prim, sum_vs, sum_ps; static unsigned samples;
    static int tag[N]; static double t_ms[3]; static unsigned long long t_inv[3], t_prim[3], t_ps[3]; static unsigned t_n[3];   /* c144: per cull-mode tag (1 = mode 1, 2 = mode 2, 0 = not alternating) */
    extern int kelvin_cull_alt_tag(void);
    ID3D11DeviceContext *c = g_device_state.d3d11_context; ID3D11Device *d = g_device_state.d3d11_device;
    if (on < 0) { const char *e = getenv("STE_GPU_STATS"); on = (e && *e && *e != '0'); }
    if (!on || !c || !d) return;
    if (!inited) {
        D3D11_QUERY_DESC q; int i; inited = 1; fprintf(stderr, "[GPUSTAT] enabled\n"); fflush(stderr);
        for (i = 0; i < N; ++i) {
            q.MiscFlags = 0; q.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; ID3D11Device_CreateQuery(d, &q, &qd[i]);
            q.Query = D3D11_QUERY_TIMESTAMP; ID3D11Device_CreateQuery(d, &q, &qa[i]); ID3D11Device_CreateQuery(d, &q, &qb[i]);
            q.Query = D3D11_QUERY_PIPELINE_STATISTICS; ID3D11Device_CreateQuery(d, &q, &qp[i]);
            q.Query = D3D11_QUERY_TIMESTAMP; ID3D11Device_CreateQuery(d, &q, &g_gb_qf[i]); ID3D11Device_CreateQuery(d, &q, &g_gb_ql[i]);
        }
        { const char *e = getenv("STE_GPU_BUSY"); g_gb_on = (e && *e && *e != '0'); }
    }
    if (begun) {   /* close the frame that started at the previous call */
        ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)qb[wr]); ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)qp[wr]); ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)qd[wr]);
        wr = (wr + 1) % N; begun = 0; g_gb_slot = -1;
    }
    while (rd != wr) {   /* collect finished frames without stalling */
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 t0, t1; D3D11_QUERY_DATA_PIPELINE_STATISTICS ps;
        if (ID3D11DeviceContext_GetData(c, (ID3D11Asynchronous *)qd[rd], &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        if (ID3D11DeviceContext_GetData(c, (ID3D11Asynchronous *)qa[rd], &t0, sizeof t0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        if (ID3D11DeviceContext_GetData(c, (ID3D11Asynchronous *)qb[rd], &t1, sizeof t1, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        if (ID3D11DeviceContext_GetData(c, (ID3D11Asynchronous *)qp[rd], &ps, sizeof ps, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        if (g_gb_on > 0 && g_gb_has[rd] && !dj.Disjoint && dj.Frequency) {   /* c178 GPU-busy span */
            UINT64 f0 = 0, l0 = 0;
            if (ID3D11DeviceContext_GetData(c, (ID3D11Asynchronous *)g_gb_qf[rd], &f0, sizeof f0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                ID3D11DeviceContext_GetData(c, (ID3D11Asynchronous *)g_gb_ql[rd], &l0, sizeof l0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && l0 >= f0) {
                static double bs[4096]; static unsigned bn; double ms = (double)(l0 - f0) * 1000.0 / (double)dj.Frequency;
                if (bn < 4096) bs[bn++] = ms;
                if (bn >= 300) {
                    double sum = 0, mx = 0; unsigned k; for (k = 0; k < bn; ++k) { sum += bs[k]; if (bs[k] > mx) mx = bs[k]; }
                    qsort(bs, bn, sizeof(double), gb_cmp);
                    { UINT64 vram = 0, budget = 0; IDXGIDevice *dxd = NULL; IDXGIAdapter *ad = NULL; IDXGIAdapter3 *a3 = NULL;
                      if (SUCCEEDED(ID3D11Device_QueryInterface(d, &IID_IDXGIDevice, (void **)&dxd)) && dxd) {
                          if (SUCCEEDED(IDXGIDevice_GetAdapter(dxd, &ad)) && ad) {
                              if (SUCCEEDED(IDXGIAdapter_QueryInterface(ad, &IID_IDXGIAdapter3, (void **)&a3)) && a3) {
                                  DXGI_QUERY_VIDEO_MEMORY_INFO mi; if (SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(a3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) { vram = mi.CurrentUsage; budget = mi.Budget; }
                                  IDXGIAdapter3_Release(a3); }
                              IDXGIAdapter_Release(ad); }
                          IDXGIDevice_Release(dxd); }
                      fprintf(stderr, "[GPUBUSY] frames=%u busy_ms avg=%.3f p50=%.3f p99=%.3f max=%.3f draws_last=%u vram_used_MB=%.0f budget_MB=%.0f\n", bn, sum / bn, bs[bn / 2], bs[(bn * 99) / 100], mx, g_gb_draws[rd], (double)vram / 1048576.0, (double)budget / 1048576.0); }
                    fflush(stderr); bn = 0;
                }
            }
        }
        if (!dj.Disjoint && dj.Frequency) {
            double ms = (double)(t1 - t0) * 1000.0 / (double)dj.Frequency; sum_ms += ms; if (ms > max_ms) max_ms = ms;
            sum_inv += ps.CInvocations; sum_prim += ps.CPrimitives; sum_vs += ps.VSInvocations; sum_ps += ps.PSInvocations; ++samples;
            { int tg = tag[rd] & 3; if (tg > 2) tg = 0; t_ms[tg] += ms; t_inv[tg] += ps.CInvocations; t_prim[tg] += ps.CPrimitives; t_ps[tg] += ps.PSInvocations; t_n[tg]++; }
        }
        rd = (rd + 1) % N;
    }
    if (((wr + 1) % N) != rd) {   /* start the next frame unless the ring is full */
        ID3D11DeviceContext_Begin(c, (ID3D11Asynchronous *)qd[wr]); ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)qa[wr]); ID3D11DeviceContext_Begin(c, (ID3D11Asynchronous *)qp[wr]);
        tag[wr] = kelvin_cull_alt_tag(); begun = 1;
        g_gb_has[wr] = 0; g_gb_draws[wr] = 0; g_gb_slot = (g_gb_on > 0) ? wr : -1;
    }
    if (++nfr >= 300) {
        if (t_n[1] && t_n[2]) { int a; for (a = 1; a <= 2; ++a) fprintf(stderr, "[GPUALT] mode=%d frames=%u gpu_ms=%.3f tris_in=%.0f tris_raster=%.0f ps_inv=%.0f\n", a, t_n[a], t_ms[a] / t_n[a], (double)t_inv[a] / t_n[a], (double)t_prim[a] / t_n[a], (double)t_ps[a] / t_n[a]); }
        memset(t_ms, 0, sizeof t_ms); memset(t_inv, 0, sizeof t_inv); memset(t_prim, 0, sizeof t_prim); memset(t_ps, 0, sizeof t_ps); memset(t_n, 0, sizeof t_n);
        if (samples) fprintf(stderr, "[GPUSTAT] frames=%u gpu_ms avg=%.3f max=%.3f  tris_in_avg=%.0f tris_raster_avg=%.0f (culled %.1f%%)  vs_avg=%.0f ps_avg=%.0f\n", samples, sum_ms / samples, max_ms,
            (double)sum_inv / samples, (double)sum_prim / samples, sum_inv ? 100.0 * (1.0 - (double)sum_prim / (double)sum_inv) : 0.0, (double)sum_vs / samples, (double)sum_ps / samples);
        fflush(stderr); nfr = 0; samples = 0; sum_ms = max_ms = 0; sum_inv = sum_prim = sum_vs = sum_ps = 0;
    }
}

void d3d8_rt_present(uint32_t key)
{
    D3D8RtEntry *e = rt_get(key, 0, 0, 0);
    D3D11_TEXTURE2D_DESC source_desc, target_desc;
    ID3D11DeviceContext *context = g_device_state.d3d11_context;

    if (!e || !e->tex || !g_device_state.render_texture || !context)
        return;

    ID3D11Texture2D_GetDesc(e->tex, &source_desc);
    ID3D11Texture2D_GetDesc(g_device_state.render_texture, &target_desc);
    if (source_desc.Width == target_desc.Width &&
        source_desc.Height == target_desc.Height &&
        source_desc.Format == target_desc.Format) {
        /* Both resources belong to the guest-render device.  PresentFrame
         * subsequently reads render_texture and publishes it atomically to
         * the independently paced scanout device. */
        ID3D11DeviceContext_OMSetRenderTargets(context, 0, NULL, NULL);
        ID3D11DeviceContext_CopyResource(context,
                                         (ID3D11Resource *)g_device_state.render_texture,
                                         (ID3D11Resource *)e->tex);
    }
}

/* Logical (guest-space) size of the current target: 640 x 480 for the display surface and for 640x480 guest surfaces
 * (logical width grows with the aspect when aspect=wide), the guest size for everything else. */
int   d3d8_cur_target_screen_sized(void) { return g_cur_rtv ? (g_cur_w == 640 && g_cur_h == 480) : 1; }
float d3d8_screen_logical_w(void) { return d3d8_cur_target_screen_sized() ? d3d8_video_cfg()->logical_w : (float)g_cur_w; }
float d3d8_screen_logical_h(void) { return d3d8_cur_target_screen_sized() ? 480.0f : (float)g_cur_h; }
UINT                 d3d8_GetBackbufferWidth(void) { return (UINT)(d3d8_screen_logical_w() + 0.5f); }
UINT                 d3d8_GetBackbufferHeight(void) { return (UINT)(d3d8_screen_logical_h() + 0.5f); }
const DWORD         *d3d8_GetRenderStates(void) { return g_device_state.render_states; }
const DWORD         *d3d8_GetTSS(DWORD stage) { return (stage < MAX_TEXTURE_STAGES) ? g_device_state.tss[stage] : NULL; }
IDirect3DBaseTexture8 *d3d8_GetStageTexture(DWORD stage) { return (stage < 4) ? g_cur_textures[stage] : NULL; }
const D3DMATRIX     *d3d8_GetTransform(D3DTRANSFORMSTATETYPE type) {
    return ((DWORD)type < MAX_TRANSFORMS) ? &g_device_state.transforms[(DWORD)type] : NULL;
}

const D3DLIGHT8     *d3d8_GetLight(DWORD index) {
    return (index < MAX_LIGHTS) ? &g_device_state.lights[index] : NULL;
}

BOOL                 d3d8_GetLightEnable(DWORD index) {
    return (index < MAX_LIGHTS) ? g_device_state.light_enable[index] : FALSE;
}

const D3DMATERIAL8  *d3d8_GetMaterial(void) {
    return &g_device_state.material;
}

DWORD                d3d8_GetCurrentFVF(void) {
    return g_cur_vb ? ((D3D8VertexBuffer *)g_cur_vb)->fvf : 0;
}

UINT                 d3d8_GetNumLights(void) {
    return MAX_LIGHTS;
}

/* ================================================================
 * D3D11 initialization helpers
 * ================================================================ */

static HRESULT d3d11_create_device_and_swap_chain(
    D3D8DeviceState *state,
    D3DPRESENT_PARAMETERS *pp)
{
    DXGI_SWAP_CHAIN_DESC scd;
    D3D_FEATURE_LEVEL feature_level;
    UINT create_flags = 0;
    HRESULT hr;

#ifdef _DEBUG
    create_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    if (d3d8_debug_wanted()) {
        HRESULT probe = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, create_flags | D3D11_CREATE_DEVICE_DEBUG,
                                          NULL, 0, D3D11_SDK_VERSION, NULL, NULL, NULL);
        fprintf(stderr, "[D3D-DEBUG] debug-layer device probe = 0x%08lX\n", (unsigned long)probe);
        if (SUCCEEDED(probe)) create_flags |= D3D11_CREATE_DEVICE_DEBUG;
    }

    memset(&scd, 0, sizeof(scd));
    scd.BufferCount = pp->BackBufferCount ? pp->BackBufferCount : 1;
    scd.BufferDesc.Width = d3d8_video_cfg()->out_w;
    scd.BufferDesc.Height = d3d8_video_cfg()->out_h;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = pp->hDeviceWindow;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = pp->Windowed;
    /* Preserve the last completed frame so the independent 30 Hz scanout can
     * present it repeatedly while guest emulation loads or transitions. */
    scd.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
    {   /* c180: ste_video.ini vrr = auto (default) | off. auto: flip-model swap chain + DXGI_PRESENT_ALLOW_TEARING when the system supports it (windowed / borderless; needed for variable refresh rate
         * displays and gives lower latency); the scan-out then presents only when a new frame is ready (repeats are re-presented at most every 100 ms). off = the old SEQUENTIAL chain at a fixed 60 Hz. */
        char vb[32]; const char *ve = getenv("STE_VIDEO_VRR"); int want = 1; IDXGIFactory5 *f5 = NULL; BOOL tear = FALSE;
        vb[0] = 0; if (ve && ve[0]) { strncpy(vb, ve, sizeof vb - 1); vb[sizeof vb - 1] = 0; } else { char ini[MAX_PATH]; const char *ie = getenv("STE_VIDEO_INI"); if (ie && ie[0]) { strncpy(ini, ie, MAX_PATH - 1); ini[MAX_PATH - 1] = 0; GetPrivateProfileStringA("video", "vrr", "", vb, sizeof vb, ini); } else { char *p; GetModuleFileNameA(NULL, ini, MAX_PATH); p = strrchr(ini, '\\'); if (p) { strcpy(p + 1, "ste_video.ini"); GetPrivateProfileStringA("video", "vrr", "", vb, sizeof vb, ini); } } }
        if (!_stricmp(vb, "off") || !_stricmp(vb, "0") || !_stricmp(vb, "false")) want = 0;
        if (want && d3d8_video_cfg()->mode != D3D8V_FULLSCREEN && pp->Windowed && SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory5, (void **)&f5)) && f5) {
            if (FAILED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof tear))) tear = FALSE;
            IDXGIFactory5_Release(f5);
        }
        g_tear_supported = (int)tear; strncpy(g_vrr_str, want ? "auto" : "off", sizeof g_vrr_str - 1);
        if (want && tear) { scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; if (scd.BufferCount < 2) scd.BufferCount = 2; scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING; g_scan_flip = 1; }
        fprintf(stderr, "[VIDEO] vrr=%s: %s (tearing supported=%d, windowed=%d, fullscreen mode=%d)\n", vb[0] ? vb : "auto", g_scan_flip ? "flip-model swap chain, ALLOW_TEARING, present-on-new-frame" : "SEQUENTIAL swap chain, fixed 60 Hz scan-out", (int)tear, (int)pp->Windowed, d3d8_video_cfg()->mode == D3D8V_FULLSCREEN);
    }

    hr = D3D11CreateDeviceAndSwapChain(
        NULL,
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        create_flags,
        NULL, 0,
        D3D11_SDK_VERSION,
        &scd,
        &state->swap_chain,
        &state->scanout_device,
        &feature_level,
        &state->scanout_context
    );

    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Failed to create D3D11 device: 0x%08lX\n", hr);
        return hr;
    }

    /* Use a second D3D11 device for guest rendering.  The swapchain device is
     * owned exclusively by the 30 Hz scanout thread, so shader compilation,
     * texture uploads, and synchronous guest readback can never stall host
     * presentation. */
    hr = D3D11CreateDevice(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, create_flags,
        NULL, 0, D3D11_SDK_VERSION,
        &state->d3d11_device, &feature_level, &state->d3d11_context);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Failed to create render device: 0x%08lX\n",
                hr);
        return hr;
    }
    d3d8_debug_attach(0, state->d3d11_device);
    d3d8_debug_attach(1, state->scanout_device);

    /* Diagnostic capture can also read the scanout texture from the guest
     * thread. Serialize both immediate contexts: protecting only the render
     * device leaves capture racing scanout's CopyResource/UpdateSubresource. */
    {
        ID3D11DeviceContext *contexts[2] = {
            state->d3d11_context, state->scanout_context
        };
        unsigned i;
        for (i = 0; i < 2; ++i) {
            ID3D11Multithread *multithread = NULL;
            if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(
                    contexts[i], &IID_ID3D11Multithread,
                    (void **)&multithread))) {
                ID3D11Multithread_SetMultithreadProtected(multithread, TRUE);
                ID3D11Multithread_Release(multithread);
            }
        }
    }

    {   /* c251: one start-up line with everything that decides the present path, plus the real display mode of the output the window is on */
        const D3D8VideoCfg *vc = d3d8_video_cfg(); IDXGIOutput *out = NULL; char dn[64] = "?"; DEVMODEA dm; int have_dm = 0; static const char *const mn[3] = { "window", "borderless", "fullscreen" };
        memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
        if (SUCCEEDED(IDXGISwapChain_GetContainingOutput(state->swap_chain, &out)) && out) {
            DXGI_OUTPUT_DESC od;
            if (SUCCEEDED(IDXGIOutput_GetDesc(out, &od))) {
                char dev[64]; WideCharToMultiByte(CP_ACP, 0, od.DeviceName, -1, dev, sizeof dev, NULL, NULL); dev[sizeof dev - 1] = 0; strncpy(dn, dev, sizeof dn - 1);
                have_dm = EnumDisplaySettingsA(dev, ENUM_CURRENT_SETTINGS, &dm);
            }
            IDXGIOutput_Release(out);
        }
        fprintf(stderr, "[VIDEO] present: flip_model=%s tearing_supported=%s tearing_used=%s vsync=%s vrr=%s window_mode=%s swap_effect=%s buffers=%u output=%s display_mode=%lux%lu@%luHz\n",
                g_scan_flip ? "yes" : "no", g_tear_supported ? "yes" : "no", (g_scan_flip && !vc->vsync && vc->mode != D3D8V_FULLSCREEN) ? "yes" : "no", vc->vsync ? "on" : "off", g_vrr_str,
                mn[vc->mode > 2 ? 0 : vc->mode], g_scan_flip ? "FLIP_SEQUENTIAL" : "SEQUENTIAL", (unsigned)scd.BufferCount, dn,
                have_dm ? (unsigned long)dm.dmPelsWidth : 0ul, have_dm ? (unsigned long)dm.dmPelsHeight : 0ul, have_dm ? (unsigned long)dm.dmDisplayFrequency : 0ul);
        fflush(stderr);
    }
    state->hwnd = pp->hDeviceWindow;
    state->out_w = scd.BufferDesc.Width;
    state->out_h = scd.BufferDesc.Height;
    state->width = d3d8_video_cfg()->render_w;
    state->height = d3d8_video_cfg()->render_h;
    if (d3d8_video_cfg()->mode == D3D8V_FULLSCREEN) {
        HRESULT fh = E_FAIL; int try_n;
        for (try_n = 0; try_n < 5; ++try_n) {   /* c252: DXGI_ERROR_NOT_CURRENTLY_AVAILABLE (0x887A0022) when the window is not the foreground / active window yet: ask for the foreground and retry briefly */
            if (state->hwnd) {
                HWND fg = GetForegroundWindow(); DWORD fgt = fg ? GetWindowThreadProcessId(fg, NULL) : 0, me = GetCurrentThreadId(); int att = (fgt && fgt != me) ? AttachThreadInput(me, fgt, TRUE) : 0;   /* a process that was just started may not be allowed to take the foreground; sharing the input queue with the current foreground thread is the documented way */
                ShowWindow(state->hwnd, SW_SHOW); BringWindowToTop(state->hwnd); SetForegroundWindow(state->hwnd); SetActiveWindow(state->hwnd); SetFocus(state->hwnd);
                if (att) AttachThreadInput(me, fgt, FALSE);
            }
            fh = IDXGISwapChain_SetFullscreenState(state->swap_chain, TRUE, NULL);
            if (fh != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) break;
            Sleep(150);
        }
        fprintf(stderr, "[VIDEO] SetFullscreenState(TRUE) = 0x%08lX (%d attempt%s)\n", (unsigned long)fh, try_n + 1 > 5 ? 5 : try_n + 1, try_n == 0 ? "" : "s");
        if (SUCCEEDED(fh)) {   /* c252: switch the output to the requested size (a plain SetFullscreenState keeps the desktop mode), then match the buffers */
            DXGI_MODE_DESC md; HRESULT rt, rb; memset(&md, 0, sizeof md);
            md.Width = state->out_w; md.Height = state->out_h; md.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rt = IDXGISwapChain_ResizeTarget(state->swap_chain, &md);
            rb = SUCCEEDED(rt) ? IDXGISwapChain_ResizeBuffers(state->swap_chain, 0, 0, 0, DXGI_FORMAT_UNKNOWN, 0) : rt;
            fprintf(stderr, "[VIDEO] fullscreen %ux%u: ResizeTarget = 0x%08lX, ResizeBuffers = 0x%08lX\n", state->out_w, state->out_h, (unsigned long)rt, (unsigned long)rb);
            if (FAILED(rb)) { IDXGISwapChain_SetFullscreenState(state->swap_chain, FALSE, NULL); fh = rb; }
        }
        if (FAILED(fh)) {   /* never exit: carry on as a borderless window of the requested size, centred on its monitor */
            char why[48]; _snprintf(why, sizeof why, "HRESULT 0x%08lX", (unsigned long)fh); why[sizeof why - 1] = 0;
            d3d8_video_fullscreen_failed(why);
            if (state->hwnd) {
                const D3D8VideoCfg *c = d3d8_video_cfg(); int px = c->mon_x + ((int)c->mon_w - (int)state->out_w) / 2, py = c->mon_y + ((int)c->mon_h - (int)state->out_h) / 2;
                SetWindowPos(state->hwnd, HWND_TOP, px, py, (int)state->out_w, (int)state->out_h, SWP_SHOWWINDOW);
            }
        }
    }

    return S_OK;
}

static HRESULT d3d11_create_render_targets(D3D8DeviceState *state)
{
    ID3D11Texture2D *back_buffer = NULL;
    D3D11_TEXTURE2D_DESC render_desc;
    D3D11_TEXTURE2D_DESC depth_desc;
    HRESULT hr;

    /* Render offscreen, then atomically publish completed frames to a second
     * texture.  A separately paced thread owns the actual swapchain. */
    hr = IDXGISwapChain_GetBuffer(state->swap_chain, 0,
                                   &IID_ID3D11Texture2D,
                                   (void **)&back_buffer);
    if (FAILED(hr)) return hr;

    ID3D11Texture2D_GetDesc(back_buffer, &render_desc);
    render_desc.Width = state->width;      /* render size, not swap-chain size */
    render_desc.Height = state->height;
    render_desc.Usage = D3D11_USAGE_DEFAULT;
    render_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    render_desc.CPUAccessFlags = 0;
    render_desc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(state->d3d11_device, &render_desc,
                                      NULL, &state->render_texture);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(back_buffer);
        return hr;
    }

    hr = ID3D11Device_CreateRenderTargetView(state->d3d11_device,
                                              (ID3D11Resource *)state->render_texture,
                                              NULL, &state->default_rtv);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(back_buffer);
        return hr;
    }

    render_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;   /* sampled by the scan-out blit */
    hr = ID3D11Device_CreateTexture2D(state->scanout_device, &render_desc,
                                      NULL, &state->ready_texture);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(back_buffer);
        return hr;
    }

    render_desc.BindFlags = 0;
    render_desc.Usage = D3D11_USAGE_STAGING;
    render_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(state->d3d11_device, &render_desc,
                                      NULL, &state->readback_texture);
    ID3D11Texture2D_Release(back_buffer);
    if (FAILED(hr)) return hr;

    if (!getenv("STE_PRESENT_CPU")) {
        D3D11_TEXTURE2D_DESC sd = render_desc;
        IDXGIResource *dxres = NULL; HANDLE sh = NULL; HRESULT shr;
        sd.Usage = D3D11_USAGE_DEFAULT; sd.BindFlags = D3D11_BIND_SHADER_RESOURCE; sd.CPUAccessFlags = 0;
        sd.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        shr = ID3D11Device_CreateTexture2D(state->d3d11_device, &sd, NULL, &state->shared_texture);
        if (SUCCEEDED(shr)) shr = ID3D11Texture2D_QueryInterface(state->shared_texture, &IID_IDXGIKeyedMutex, (void **)&state->km_prod);
        if (SUCCEEDED(shr)) shr = ID3D11Texture2D_QueryInterface(state->shared_texture, &IID_IDXGIResource, (void **)&dxres);
        if (SUCCEEDED(shr)) shr = IDXGIResource_GetSharedHandle(dxres, &sh);
        if (dxres) IDXGIResource_Release(dxres);
        if (SUCCEEDED(shr)) shr = ID3D11Device_OpenSharedResource(state->scanout_device, sh, &IID_ID3D11Texture2D, (void **)&state->shared_on_scan);
        if (SUCCEEDED(shr)) shr = ID3D11Texture2D_QueryInterface(state->shared_on_scan, &IID_IDXGIKeyedMutex, (void **)&state->km_scan);
        if (FAILED(shr)) {
            fprintf(stderr, "[VIDEO] shared-texture present unavailable (0x%08lX): using the CPU path\n", (unsigned long)shr);
            if (state->km_scan) { IDXGIKeyedMutex_Release(state->km_scan); state->km_scan = NULL; }
            if (state->shared_on_scan) { ID3D11Texture2D_Release(state->shared_on_scan); state->shared_on_scan = NULL; }
            if (state->km_prod) { IDXGIKeyedMutex_Release(state->km_prod); state->km_prod = NULL; }
            if (state->shared_texture) { ID3D11Texture2D_Release(state->shared_texture); state->shared_texture = NULL; }
        } else fprintf(stderr, "[VIDEO] present path: GPU shared texture (keyed mutex)\n");
    }
    state->scanout_cpu_frame = (BYTE *)malloc(
        (size_t)state->width * state->height * 4u);
    if (!state->scanout_cpu_frame) return E_OUTOFMEMORY;
    InitializeCriticalSection(&state->scanout_cpu_lock);
    state->scanout_cpu_lock_initialized = TRUE;
    state->scanout_cpu_generation = -1;

    /* Create depth stencil */
    memset(&depth_desc, 0, sizeof(depth_desc));
    depth_desc.Width = state->width;
    depth_desc.Height = state->height;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.SampleDesc.Quality = 0;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = ID3D11Device_CreateTexture2D(state->d3d11_device, &depth_desc,
                                       NULL, &state->default_depth);
    if (FAILED(hr)) return hr;

    hr = ID3D11Device_CreateDepthStencilView(state->d3d11_device,
                                              (ID3D11Resource *)state->default_depth,
                                              NULL, &state->default_dsv);
    if (FAILED(hr)) return hr;

    /* Bind default render targets */
    ID3D11DeviceContext_OMSetRenderTargets(state->d3d11_context, 1,
                                            &state->default_rtv,
                                            state->default_dsv);

    return S_OK;
}

static void d3d8_init_default_states(D3D8DeviceState *state)
{
    /* Set Xbox D3D8 default render states */
    memset(state->render_states, 0, sizeof(state->render_states));
    state->render_states[D3DRS_ZENABLE]           = 1;
    state->render_states[D3DRS_FILLMODE]          = D3DFILL_SOLID;
    state->render_states[D3DRS_SHADEMODE]         = 2; /* D3DSHADE_GOURAUD */
    state->render_states[D3DRS_ZWRITEENABLE]      = TRUE;
    state->render_states[D3DRS_ALPHATESTENABLE]    = FALSE;
    state->render_states[D3DRS_SRCBLEND]          = D3DBLEND_ONE;
    state->render_states[D3DRS_DESTBLEND]         = D3DBLEND_ZERO;
    state->render_states[D3DRS_CULLMODE]          = D3DCULL_CCW;
    state->render_states[D3DRS_ZFUNC]             = D3DCMP_LESSEQUAL;
    state->render_states[D3DRS_ALPHAREF]          = 0;
    state->render_states[D3DRS_ALPHAFUNC]         = D3DCMP_ALWAYS;
    state->render_states[D3DRS_ALPHABLENDENABLE]   = FALSE;
    state->render_states[D3DRS_FOGENABLE]         = FALSE;
    state->render_states[D3DRS_STENCILENABLE]     = FALSE;
    state->render_states[D3DRS_COLORWRITEENABLE]  = 0x0F;

    /* Default viewport */
    state->viewport.X = 0;
    state->viewport.Y = 0;
    state->viewport.Width = state->width;
    state->viewport.Height = state->height;
    state->viewport.MinZ = 0.0f;
    state->viewport.MaxZ = 1.0f;

    /* Default texture stage states:
     * By default each stage reads its own texcoord set (0,1,2,3). */
    memset(state->tss, 0, sizeof(state->tss));
    for (int s = 0; s < MAX_TEXTURE_STAGES; s++)
        state->tss[s][D3DTSS_TEXCOORDINDEX] = (DWORD)s;

    /* Identity matrices */
    for (int i = 0; i < MAX_TRANSFORMS; i++) {
        memset(&state->transforms[i], 0, sizeof(D3DMATRIX));
        state->transforms[i]._11 = 1.0f;
        state->transforms[i]._22 = 1.0f;
        state->transforms[i]._33 = 1.0f;
        state->transforms[i]._44 = 1.0f;
    }

    state->vertex_shader = 0;
    state->pixel_shader = 0;
    state->in_scene = FALSE;
}

/* ================================================================
 * IDirect3DDevice8 method implementations
 * ================================================================ */

static HRESULT __stdcall dev_QueryInterface(IDirect3DDevice8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall dev_AddRef(IDirect3DDevice8 *self)
{
    (void)self;
    return InterlockedIncrement(&g_device_state.ref_count);
}

static ULONG __stdcall dev_Release(IDirect3DDevice8 *self)
{
    (void)self;
    LONG ref = InterlockedDecrement(&g_device_state.ref_count);
    if (ref <= 0) {
        /* Cleanup subsystems first */
        up_ring_shutdown();
        d3d8_gamma_shutdown();
        d3d8_vsh_shutdown();
        d3d8_combiners_shutdown();
        d3d8_states_shutdown();
        d3d8_shaders_shutdown();

        /* Cleanup D3D11 resources */
        D3D8DeviceState *s = &g_device_state;
        InterlockedExchange(&s->scanout_active, 0);
        if (s->scanout_thread) {
            WaitForSingleObject(s->scanout_thread, 2000);
            CloseHandle(s->scanout_thread);
            s->scanout_thread = NULL;
        }
        if (s->scanout_cpu_lock_initialized) {
            DeleteCriticalSection(&s->scanout_cpu_lock);
            s->scanout_cpu_lock_initialized = FALSE;
        }
        free(s->scanout_cpu_frame);
        s->scanout_cpu_frame = NULL;
        if (s->default_dsv) { ID3D11DepthStencilView_Release(s->default_dsv); s->default_dsv = NULL; }
        if (s->default_depth) { ID3D11Texture2D_Release(s->default_depth); s->default_depth = NULL; }
        if (s->default_rtv) { ID3D11RenderTargetView_Release(s->default_rtv); s->default_rtv = NULL; }
        if (s->readback_texture) { ID3D11Texture2D_Release(s->readback_texture); s->readback_texture = NULL; }
        if (s->ready_texture) { ID3D11Texture2D_Release(s->ready_texture); s->ready_texture = NULL; }
        if (s->render_texture) { ID3D11Texture2D_Release(s->render_texture); s->render_texture = NULL; }
        if (s->swap_chain) { IDXGISwapChain_Release(s->swap_chain); s->swap_chain = NULL; }
        if (s->d3d11_context) { ID3D11DeviceContext_Release(s->d3d11_context); s->d3d11_context = NULL; }
        if (s->d3d11_device) { ID3D11Device_Release(s->d3d11_device); s->d3d11_device = NULL; }
        if (s->scanout_context) { ID3D11DeviceContext_Release(s->scanout_context); s->scanout_context = NULL; }
        if (s->scanout_device) { ID3D11Device_Release(s->scanout_device); s->scanout_device = NULL; }
        g_device_initialized = FALSE;
    }
    return (ULONG)ref;
}

static HRESULT __stdcall dev_GetDirect3D(IDirect3DDevice8 *self, IDirect3D8 **ppD3D8)
{
    (void)self; (void)ppD3D8;
    /* TODO: return the factory */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_GetDeviceCaps(IDirect3DDevice8 *self, void *pCaps)
{
    (void)self; (void)pCaps;
    /* TODO: fill with Xbox NV2A capabilities */
    return S_OK;
}

static HRESULT __stdcall dev_GetDisplayMode(IDirect3DDevice8 *self, void *pMode)
{
    (void)self; (void)pMode;
    return S_OK;
}

static HRESULT __stdcall dev_GetCreationParameters(IDirect3DDevice8 *self, void *pParams)
{
    (void)self; (void)pParams;
    return S_OK;
}

static HRESULT __stdcall dev_Reset(IDirect3DDevice8 *self, D3DPRESENT_PARAMETERS *pPP)
{
    (void)self; (void)pPP;
    /* TODO: resize swap chain */
    return S_OK;
}

static DWORD g_d3d_begin_count = 0;
static DWORD g_d3d_end_count = 0;
static DWORD g_d3d_clear_count = 0;
static DWORD g_d3d_draw_count = 0;
static DWORD g_d3d_settransform_count = 0;
static DWORD g_d3d_setrs_count = 0;
static DWORD g_d3d_settexture_count = 0;

static HRESULT __stdcall dev_Present(IDirect3DDevice8 *self, const RECT *src, const RECT *dst, HWND hWnd, void *pDirty)
{
    static DWORD frame_count = 0;
    static DWORD last_tick = 0;
    (void)self; (void)src; (void)dst; (void)hWnd; (void)pDirty;

    frame_count++;
    DWORD now = GetTickCount();
    if (last_tick == 0) last_tick = now;
    if (now - last_tick >= 2000) {
        fprintf(stderr, "  [D3D] %.1fs: %u present (%.1f fps), %u begin, %u end, "
                "%u clear, %u draw, %u xform, %u rs, %u tex\n",
                (now - last_tick) / 1000.0, frame_count,
                frame_count * 1000.0 / (now - last_tick),
                g_d3d_begin_count, g_d3d_end_count,
                g_d3d_clear_count, g_d3d_draw_count,
                g_d3d_settransform_count, g_d3d_setrs_count,
                g_d3d_settexture_count);
        fflush(stderr);
        frame_count = 0;
        g_d3d_begin_count = g_d3d_end_count = 0;
        g_d3d_clear_count = g_d3d_draw_count = 0;
        g_d3d_settransform_count = g_d3d_setrs_count = 0;
        g_d3d_settexture_count = 0;
        last_tick = now;
    }

    /* Pump Windows messages: the game's internal main loop drives rendering,
     * so our external message pump never runs. Process messages here to keep
     * the window responsive and handle input. */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            recomp_exit_hook_run("window");     /* ExitProcess skips atexit */
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return d3d8_present();
}

static HRESULT __stdcall dev_GetBackBuffer(IDirect3DDevice8 *self, INT iBackBuffer, DWORD Type, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)iBackBuffer; (void)Type;
    ID3D11Texture2D *back_buffer = NULL;
    HRESULT hr;

    if (!ppSurface) return E_INVALIDARG;

    hr = IDXGISwapChain_GetBuffer(g_device_state.swap_chain, 0,
                                   &IID_ID3D11Texture2D,
                                   (void **)&back_buffer);
    if (FAILED(hr)) return hr;

    *ppSurface = d3d8_surface_create(back_buffer, 0, 0,
                                     g_device_state.width,
                                     g_device_state.height,
                                     D3DFMT_X8R8G8B8,
                                     D3DPOOL_DEFAULT, 0,
                                     D3DMULTISAMPLE_NONE, NULL, 0);
    ID3D11Texture2D_Release(back_buffer);
    return *ppSurface ? S_OK : E_OUTOFMEMORY;
}

static HRESULT __stdcall dev_BeginScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = TRUE;
    g_d3d_begin_count++;
    return S_OK;
}

static HRESULT __stdcall dev_EndScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = FALSE;
    g_d3d_end_count++;
    return S_OK;
}

static HRESULT __stdcall dev_Clear(IDirect3DDevice8 *self, DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
    (void)self; (void)Count; (void)pRects; (void)Stencil;
    g_d3d_clear_count++;

    /* Clear the currently bound targets (default if none set) */
    ID3D11RenderTargetView *rtv = g_cur_rt ? g_cur_rt->rtv : g_device_state.default_rtv;
    ID3D11DepthStencilView *dsv = g_cur_ds ? g_cur_ds->dsv : g_device_state.default_dsv;

    if ((Flags & D3DCLEAR_TARGET) && rtv) {
        float clear_color[4] = {
            ((Color >> 16) & 0xFF) / 255.0f,  /* R */
            ((Color >>  8) & 0xFF) / 255.0f,  /* G */
            ((Color >>  0) & 0xFF) / 255.0f,  /* B */
            ((Color >> 24) & 0xFF) / 255.0f,  /* A */
        };
        ID3D11DeviceContext_ClearRenderTargetView(g_device_state.d3d11_context,
                                                   cur_rtv(), clear_color);
    }

    if ((Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && dsv) {
        UINT clear_flags = 0;
        if (Flags & D3DCLEAR_ZBUFFER) clear_flags |= D3D11_CLEAR_DEPTH;
        if (Flags & D3DCLEAR_STENCIL) clear_flags |= D3D11_CLEAR_STENCIL;

        if (cur_dsv())
            ID3D11DeviceContext_ClearDepthStencilView(g_device_state.d3d11_context,
                                                      cur_dsv(), clear_flags, Z, (UINT8)Stencil);
    }

    return S_OK;
}

static HRESULT __stdcall dev_SetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix)
{
    (void)self;
    g_d3d_settransform_count++;
    if ((DWORD)State < MAX_TRANSFORMS && pMatrix) {
        g_device_state.transforms[(DWORD)State] = *pMatrix;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix)
{
    (void)self;
    if ((DWORD)State < MAX_TRANSFORMS && pMatrix) {
        *pMatrix = g_device_state.transforms[(DWORD)State];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD Value)
{
    (void)self;
    g_d3d_setrs_count++;
    if ((DWORD)State < MAX_RENDER_STATES) {
        g_device_state.render_states[(DWORD)State] = Value;
    }
    /* Mark combiner state dirty if any PS register combiner state changed */
    if ((DWORD)State >= D3DRS_PSALPHAINPUTS0 && (DWORD)State <= D3DRS_PSINPUTTEXTURE) {
        d3d8_combiners_mark_dirty();
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD *pValue)
{
    (void)self;
    if ((DWORD)State < MAX_RENDER_STATES && pValue) {
        *pValue = g_device_state.render_states[(DWORD)State];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    (void)self;
    if (Stage < MAX_TEXTURE_STAGES && (DWORD)Type < MAX_TSS_STATES) {
        g_device_state.tss[Stage][(DWORD)Type] = Value;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
    (void)self;
    if (Stage < MAX_TEXTURE_STAGES && (DWORD)Type < MAX_TSS_STATES && pValue) {
        *pValue = g_device_state.tss[Stage][(DWORD)Type];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 *pTexture)
{
    (void)self;
    g_d3d_settexture_count++;
    if (Stage >= 4) return E_INVALIDARG;
    g_cur_textures[Stage] = pTexture;

    /* Bind SRV to pixel shader */
    if (pTexture) {
        ID3D11ShaderResourceView *srv = d3d8_base_srv(pTexture);
        /* P8 textures bake a palette; record which stage palette they use
         * so SetPalette can re-bake them later. */
        if (d3d8_format_is_palettized(d3d8_base_format(pTexture)))
            d3d8_base_set_palette(pTexture, Stage);
        if (srv) {
            ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                Stage, 1, &srv);
        }
        /* Mark texture stage as active */
        if (g_device_state.tss[Stage][D3DTSS_COLOROP] == D3DTOP_DISABLE)
            g_device_state.tss[Stage][D3DTSS_COLOROP] = D3DTOP_MODULATE;
    } else {
        ID3D11ShaderResourceView *null_srv = NULL;
        ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
            Stage, 1, &null_srv);
        g_device_state.tss[Stage][D3DTSS_COLOROP] = D3DTOP_DISABLE;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 **ppTexture)
{
    (void)self;
    if (!ppTexture || Stage >= MAX_TEXTURE_STAGES) return E_INVALIDARG;
    *ppTexture = g_cur_textures[Stage];
    if (*ppTexture) IDirect3DBaseTexture8_AddRef(*ppTexture);
    return S_OK;
}

static HRESULT __stdcall dev_SetStreamSource(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride)
{
    (void)self;
    if (StreamNumber != 0) return S_OK; /* Only stream 0 supported */
    g_cur_vb = pStreamData;
    g_cur_vb_stride = Stride;

    if (pStreamData) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)pStreamData;
        UINT offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &Stride, &offset);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetStreamSource(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride)
{
    (void)self;
    if (!ppStreamData || StreamNumber != 0) return E_INVALIDARG;
    *ppStreamData = g_cur_vb;
    if (*ppStreamData) (*ppStreamData)->lpVtbl->AddRef(*ppStreamData);
    if (pStride) *pStride = g_cur_vb_stride;
    return S_OK;
}

static HRESULT __stdcall dev_SetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex)
{
    (void)self;
    g_cur_ib = pIndexData;
    g_cur_ib_base_vertex = BaseVertexIndex;

    if (pIndexData) {
        D3D8IndexBuffer *ib = (D3D8IndexBuffer *)pIndexData;
        DXGI_FORMAT fmt = (ib->format == D3DFMT_INDEX32)
            ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
            ib->d3d11_buffer, fmt, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex)
{
    (void)self;
    if (!ppIndexData) return E_INVALIDARG;
    *ppIndexData = g_cur_ib;
    if (*ppIndexData) (*ppIndexData)->lpVtbl->AddRef(*ppIndexData);
    if (pBaseVertexIndex) *pBaseVertexIndex = g_cur_ib_base_vertex;
    return S_OK;
}

static D3D11_PRIMITIVE_TOPOLOGY map_primitive_type(D3DPRIMITIVETYPE pt, UINT count, UINT *out_count)
{
    switch (pt) {
    case D3DPT_TRIANGLELIST:  *out_count = count * 3; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_TRIANGLESTRIP: *out_count = count + 2; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case D3DPT_TRIANGLEFAN:   *out_count = count * 3; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_LINELIST:      *out_count = count * 2; return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case D3DPT_LINESTRIP:     *out_count = count + 1; return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case D3DPT_POINTLIST:     *out_count = count;     return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case D3DPT_QUADLIST:      *out_count = count * 6; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:                  *out_count = 0;          return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

/* ================================================================
 * Triangle fan / quad list → triangle list conversion
 *
 * D3D11 doesn't support triangle fans or quad lists.
 * Convert vertex data in-place to triangle list.
 * Returns malloc'd buffer (caller must free) or NULL if no conversion needed.
 * ================================================================ */

static void *convert_fan_or_quad(D3DPRIMITIVETYPE pt, const void *src,
                                  UINT prim_count, UINT stride,
                                  UINT *out_vertex_count)
{
    BYTE *dst;
    const BYTE *s = (const BYTE *)src;
    UINT i;

    if (pt == D3DPT_TRIANGLEFAN) {
        /* Fan: vertex 0 is the hub, each triangle is (0, i+1, i+2) */
        UINT tri_verts = prim_count * 3;
        dst = (BYTE *)malloc(tri_verts * stride);
        if (!dst) return NULL;

        for (i = 0; i < prim_count; i++) {
            memcpy(dst + (i * 3 + 0) * stride, s, stride);                      /* v0 (hub) */
            memcpy(dst + (i * 3 + 1) * stride, s + (i + 1) * stride, stride);   /* v[i+1] */
            memcpy(dst + (i * 3 + 2) * stride, s + (i + 2) * stride, stride);   /* v[i+2] */
        }
        *out_vertex_count = tri_verts;
        return dst;
    }

    if (pt == D3DPT_QUADLIST) {
        /* Quad list: each quad (v0,v1,v2,v3) → 2 triangles (v0,v1,v2), (v0,v2,v3) */
        UINT tri_verts = prim_count * 6;
        dst = (BYTE *)malloc(tri_verts * stride);
        if (!dst) return NULL;

        for (i = 0; i < prim_count; i++) {
            const BYTE *q = s + i * 4 * stride;
            memcpy(dst + (i * 6 + 0) * stride, q + 0 * stride, stride);  /* v0 */
            memcpy(dst + (i * 6 + 1) * stride, q + 1 * stride, stride);  /* v1 */
            memcpy(dst + (i * 6 + 2) * stride, q + 2 * stride, stride);  /* v2 */
            memcpy(dst + (i * 6 + 3) * stride, q + 0 * stride, stride);  /* v0 */
            memcpy(dst + (i * 6 + 4) * stride, q + 2 * stride, stride);  /* v2 */
            memcpy(dst + (i * 6 + 5) * stride, q + 3 * stride, stride);  /* v3 */
        }
        *out_vertex_count = tri_verts;
        return dst;
    }

    return NULL; /* no conversion needed */
}

/* ================================================================
 * DrawPrimitiveUP ring buffer
 *
 * Instead of creating and destroying a D3D11 buffer on every
 * DrawPrimitiveUP call, use a persistent ring buffer.
 * ================================================================ */

#define UP_RING_BUFFER_SIZE (4 * 1024 * 1024)  /* 4MB ring buffer */

static ID3D11Buffer *g_up_ring_buffer = NULL;
static UINT          g_up_ring_offset = 0;

static HRESULT up_ring_init(void)
{
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = UP_RING_BUFFER_SIZE;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, NULL, &g_up_ring_buffer);
}

static void up_ring_shutdown(void)
{
    if (g_up_ring_buffer) {
        ID3D11Buffer_Release(g_up_ring_buffer);
        g_up_ring_buffer = NULL;
    }
    g_up_ring_offset = 0;
}

/* Upload vertex data to ring buffer, returns offset. Returns (UINT)-1 on failure. */
static UINT up_ring_upload(const void *data, UINT size)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_MAP map_type;
    HRESULT hr;
    UINT offset;

    if (!g_up_ring_buffer) {
        if (FAILED(up_ring_init())) return (UINT)-1;
    }

    if (size > UP_RING_BUFFER_SIZE) return (UINT)-1;

    /* Wrap around if not enough space */
    if (g_up_ring_offset + size > UP_RING_BUFFER_SIZE) {
        g_up_ring_offset = 0;
        map_type = D3D11_MAP_WRITE_DISCARD;
    } else {
        map_type = D3D11_MAP_WRITE_NO_OVERWRITE;
    }

    hr = ID3D11DeviceContext_Map(g_device_state.d3d11_context,
        (ID3D11Resource *)g_up_ring_buffer, 0, map_type, 0, &mapped);
    if (FAILED(hr)) return (UINT)-1;

    offset = g_up_ring_offset;
    memcpy((BYTE *)mapped.pData + offset, data, size);

    ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
        (ID3D11Resource *)g_up_ring_buffer, 0);

    g_up_ring_offset = (offset + size + 15) & ~15;  /* 16-byte align */
    return offset;
}

static HRESULT __stdcall dev_DrawPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
    (void)self;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT vertex_count;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &vertex_count);
    if (vertex_count == 0) return E_INVALIDARG;

    /* Prepare pipeline: shaders, input layout, constant buffers, render states */
    d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_Draw(g_device_state.d3d11_context, vertex_count, StartVertex); d3d8_gpubusy_mark();
    return S_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount)
{
    (void)self; (void)MinVertexIndex; (void)NumVertices;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT index_count;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &index_count);
    if (index_count == 0) return E_INVALIDARG;

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, StartIndex, (INT)g_cur_ib_base_vertex); d3d8_gpubusy_mark();
    return S_OK;
}

/* c146 probe (STE_CULLDRAW, set from nv2a_kelvin.c): draw once for real, then twice as invisible passes (depth off, blend dest*1+src*0): with the bound rasterizer state and with cull NONE; print the pipeline statistics of both, so the primitives and
 * pixel shader invocations that the cull mode removed from this one draw are counted directly. */
int g_d3d8_cullprobe_req; unsigned g_d3d8_cullprobe_tag;
extern int d3d8_states_bound_cullmode(void);
void d3d8_cullprobe_draw(UINT vertex_count, int indexed)
{
    static ID3D11Query *q[2]; static ID3D11RasterizerState *rs_none; static ID3D11DepthStencilState *ds_off; static ID3D11BlendState *bs_off;
    ID3D11DeviceContext *c = g_device_state.d3d11_context; ID3D11Device *d = g_device_state.d3d11_device;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS s[2]; int i, cm = d3d8_states_bound_cullmode();
    ID3D11RasterizerState *old_rs = NULL; ID3D11DepthStencilState *old_ds = NULL; ID3D11BlendState *old_bs = NULL; UINT old_ref = 0, old_mask = 0; FLOAT old_bf[4];
    if (!q[0]) {
        D3D11_QUERY_DESC qd; D3D11_RASTERIZER_DESC rd; D3D11_DEPTH_STENCIL_DESC dd; D3D11_BLEND_DESC bd;
        memset(&qd, 0, sizeof qd); qd.Query = D3D11_QUERY_PIPELINE_STATISTICS; ID3D11Device_CreateQuery(d, &qd, &q[0]); ID3D11Device_CreateQuery(d, &qd, &q[1]);
        memset(&rd, 0, sizeof rd); rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; ID3D11Device_CreateRasterizerState(d, &rd, &rs_none);
        memset(&dd, 0, sizeof dd); dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; dd.DepthFunc = D3D11_COMPARISON_ALWAYS; ID3D11Device_CreateDepthStencilState(d, &dd, &ds_off);
        memset(&bd, 0, sizeof bd); bd.RenderTarget[0].BlendEnable = TRUE; bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO; bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; bd.RenderTarget[0].RenderTargetWriteMask = 0xF;   /* dest unchanged, but the colour write is not masked so the driver cannot skip the pixel shader */ ID3D11Device_CreateBlendState(d, &bd, &bs_off);
    }
    if (indexed) ID3D11DeviceContext_DrawIndexed(c, vertex_count, 0, 0); else ID3D11DeviceContext_Draw(c, vertex_count, 0);   /* the real draw */
    ID3D11DeviceContext_RSGetState(c, &old_rs); ID3D11DeviceContext_OMGetDepthStencilState(c, &old_ds, &old_ref); ID3D11DeviceContext_OMGetBlendState(c, &old_bs, old_bf, &old_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(c, ds_off, 0); ID3D11DeviceContext_OMSetBlendState(c, bs_off, old_bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_Begin(c, (ID3D11Asynchronous *)q[0]); if (indexed) ID3D11DeviceContext_DrawIndexed(c, vertex_count, 0, 0); else ID3D11DeviceContext_Draw(c, vertex_count, 0); ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)q[0]);
    ID3D11DeviceContext_RSSetState(c, rs_none);
    ID3D11DeviceContext_Begin(c, (ID3D11Asynchronous *)q[1]); if (indexed) ID3D11DeviceContext_DrawIndexed(c, vertex_count, 0, 0); else ID3D11DeviceContext_Draw(c, vertex_count, 0); ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)q[1]);
    ID3D11DeviceContext_RSSetState(c, old_rs); ID3D11DeviceContext_OMSetDepthStencilState(c, old_ds, old_ref); ID3D11DeviceContext_OMSetBlendState(c, old_bs, old_bf, old_mask);
    if (old_rs) ID3D11RasterizerState_Release(old_rs); if (old_ds) ID3D11DepthStencilState_Release(old_ds); if (old_bs) ID3D11BlendState_Release(old_bs);
    for (i = 0; i < 2; ++i) { int spin = 0; memset(&s[i], 0, sizeof s[i]); while (ID3D11DeviceContext_GetData(c, (ID3D11Asynchronous *)q[i], &s[i], sizeof s[i], 0) != S_OK && ++spin < 2000000) ; }
    fprintf(stderr, "[CULLDRAW] tag=%u verts=%u d3d11_cull=%d(1=NONE 2=FRONT 3=BACK) | bound: cin=%llu cprim=%llu ps=%llu | cull-none: cin=%llu cprim=%llu ps=%llu | removed: prims=%lld ps=%lld\n",
        g_d3d8_cullprobe_tag, vertex_count, cm, (unsigned long long)s[0].CInvocations, (unsigned long long)s[0].CPrimitives, (unsigned long long)s[0].PSInvocations,
        (unsigned long long)s[1].CInvocations, (unsigned long long)s[1].CPrimitives, (unsigned long long)s[1].PSInvocations,
        (long long)s[1].CPrimitives - (long long)s[0].CPrimitives, (long long)s[1].PSInvocations - (long long)s[0].PSInvocations);
    fflush(stderr);
}
static HRESULT __stdcall dev_DrawPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexData, UINT VertexStreamZeroStride)
{
    (void)self;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT vertex_count, vb_size, ring_offset;
    const void *draw_data = pVertexData;
    void *converted = NULL;

    if (!pVertexData || !VertexStreamZeroStride) return E_INVALIDARG;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &vertex_count);
    if (vertex_count == 0) return E_INVALIDARG;

    /* Convert triangle fans and quad lists to triangle lists */
    if (PrimitiveType == D3DPT_TRIANGLEFAN || PrimitiveType == D3DPT_QUADLIST) {
        converted = convert_fan_or_quad(PrimitiveType, pVertexData,
                                         PrimitiveCount, VertexStreamZeroStride,
                                         &vertex_count);
        if (converted) draw_data = converted;
    }

    vb_size = vertex_count * VertexStreamZeroStride;

    /* Upload to ring buffer */
    ring_offset = up_ring_upload(draw_data, vb_size);
    if (converted) free(converted);

    if (ring_offset == (UINT)-1) return E_OUTOFMEMORY;

    /* Bind ring buffer at the right offset */
    ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
        0, 1, &g_up_ring_buffer, &VertexStreamZeroStride, &ring_offset);

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    { extern int d3d8_nv2a_psh_take_skip(void); if (d3d8_nv2a_psh_take_skip()) return S_OK; }   /* c83: combiner shader still compiling on the worker: skip the draw */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    if (g_d3d8_cullprobe_req) { g_d3d8_cullprobe_req = 0; d3d8_cullprobe_draw(vertex_count, 0); } else
    ID3D11DeviceContext_Draw(g_device_state.d3d11_context, vertex_count, 0); d3d8_gpubusy_mark();

    /* Restore previous VB binding if any */
    if (g_cur_vb) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)g_cur_vb;
        UINT restore_offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &g_cur_vb_stride, &restore_offset);
    }
    return S_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexData, UINT VertexStreamZeroStride)
{
    (void)self; (void)MinVertexIndex;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Buffer *tmp_vb = NULL, *tmp_ib = NULL;
    UINT index_count, vb_size, ib_size, offset = 0;
    UINT idx_bytes;
    DXGI_FORMAT ib_fmt;
    HRESULT hr;

    if (!pVertexData || !pIndexData || !VertexStreamZeroStride) return E_INVALIDARG;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &index_count);
    if (index_count == 0) return E_INVALIDARG;

    idx_bytes = (IndexDataFormat == D3DFMT_INDEX32) ? 4 : 2;
    ib_fmt = (IndexDataFormat == D3DFMT_INDEX32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    vb_size = NumVertices * VertexStreamZeroStride;
    ib_size = index_count * idx_bytes;

    /* Create temp vertex buffer */
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = vb_size;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    memset(&sd, 0, sizeof(sd));
    sd.pSysMem = pVertexData;
    hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, &sd, &tmp_vb);
    if (FAILED(hr)) return hr;

    /* Create temp index buffer */
    bd.ByteWidth = ib_size;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = pIndexData;
    hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, &sd, &tmp_ib);
    if (FAILED(hr)) { ID3D11Buffer_Release(tmp_vb); return hr; }

    /* Bind, prepare, draw */
    ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
        0, 1, &tmp_vb, &VertexStreamZeroStride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
        tmp_ib, ib_fmt, 0);

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, 0, 0); d3d8_gpubusy_mark();

    /* Cleanup temp buffers */
    ID3D11Buffer_Release(tmp_ib);
    ID3D11Buffer_Release(tmp_vb);

    /* Restore previous bindings */
    if (g_cur_vb) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)g_cur_vb;
        offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &g_cur_vb_stride, &offset);
    }
    if (g_cur_ib) {
        D3D8IndexBuffer *ib = (D3D8IndexBuffer *)g_cur_ib;
        DXGI_FORMAT fmt = (ib->format == D3DFMT_INDEX32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
            ib->d3d11_buffer, fmt, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_CreateTexture(IDirect3DDevice8 *self, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8 **ppTexture)
{
    (void)self; (void)Pool;
    return d3d8_CreateTextureImpl(Width, Height, Levels, Usage, Format, ppTexture);
}

static HRESULT __stdcall dev_CreateImageSurface(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface8 **ppSurface)
{
    (void)self;
    return d3d8_CreateImageSurfaceImpl(Width, Height, Format, ppSurface);
}

static HRESULT __stdcall dev_CreateCubeTexture(IDirect3DDevice8 *self, UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture8 **ppCubeTexture)
{
    (void)self; (void)Pool;
    return d3d8_CreateCubeTextureImpl(EdgeLength, Levels, Usage, Format, ppCubeTexture);
}

static HRESULT __stdcall dev_CreateVolumeTexture(IDirect3DDevice8 *self, UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture8 **ppVolumeTexture)
{
    (void)self; (void)Pool;
    return d3d8_CreateVolumeTextureImpl(Width, Height, Depth, Levels, Usage, Format, ppVolumeTexture);
}

static HRESULT __stdcall dev_CreateVertexBuffer(IDirect3DDevice8 *self, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8 **ppVertexBuffer)
{
    (void)self; (void)Pool;
    return d3d8_CreateVertexBufferImpl(Length, Usage, FVF, ppVertexBuffer);
}

static HRESULT __stdcall dev_CreateIndexBuffer(IDirect3DDevice8 *self, UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer)
{
    (void)self; (void)Pool;
    return d3d8_CreateIndexBufferImpl(Length, Usage, Format, ppIndexBuffer);
}

static HRESULT __stdcall dev_CreateRenderTarget(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)Lockable;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *tex = NULL;
    HRESULT hr;
    UINT sample_count;

    if (!ppSurface || !Width || !Height) return E_INVALIDARG;

    sample_count = d3d8_msaa_sample_count(MultiSample);
    if (sample_count > 1) {
        UINT levels = 0;
        UINT maxq = 0;
        DXGI_FORMAT fmt = d3d8_to_dxgi_format(Format);
        /* Validate against D3D11 hardware limits; fall back gracefully. */
        hr = ID3D11Device_CheckMultisampleQualityLevels(d3d8_GetD3D11Device(),
                fmt, sample_count, &maxq);
        if (FAILED(hr) || maxq == 0) {
            UINT counts[] = { 9, 8, 4, 2, 1 };
            UINT i;
            for (i = 0; i < 5; i++) {
                if (counts[i] >= sample_count) continue;
                if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(
                        d3d8_GetD3D11Device(), fmt, counts[i], &levels)) && levels > 0) {
                    sample_count = counts[i];
                    break;
                }
            }
            if (sample_count > 1) {
                fprintf(stderr, "D3D8: MSAA %lu unsupported, falling back to %lu samples\n",
                        d3d8_msaa_sample_count(MultiSample) > 1 ? (unsigned long)d3d8_msaa_sample_count(MultiSample) : 0,
                        (unsigned long)sample_count);
            } else {
                sample_count = 1;
            }
        }
    }

    memset(&td, 0, sizeof(td));
    td.Width = Width;
    td.Height = Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = d3d8_to_dxgi_format(Format);
    td.SampleDesc.Count = sample_count;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    hr = ID3D11Device_CreateTexture2D(d3d8_GetD3D11Device(), &td, NULL, &tex);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: CreateRenderTarget failed: 0x%08lX (fmt=0x%X %ux%u)\n", hr, Format, Width, Height);
        return hr;
    }

    *ppSurface = d3d8_surface_create(tex, 0, 0, Width, Height, Format,
                                     D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET,
                                     MultiSample, NULL, 0);
    ID3D11Texture2D_Release(tex);
    return *ppSurface ? S_OK : E_OUTOFMEMORY;
}

static HRESULT __stdcall dev_CreateDepthStencilSurface(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface)
{
    (void)self;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *tex = NULL;
    DXGI_FORMAT dxgi;
    D3DFORMAT surface_format = Format;
    HRESULT hr;
    UINT sample_count;

    if (!ppSurface || !Width || !Height) return E_INVALIDARG;

    dxgi = d3d8_to_dxgi_format(Format);
    if (dxgi == DXGI_FORMAT_R16_FLOAT) {
        /* F16 has no D3D11 depth equivalent; fall back to D16. */
        fprintf(stderr, "D3D8: F16 depth surface falling back to D16\n");
        dxgi = DXGI_FORMAT_D16_UNORM;
        surface_format = D3DFMT_D16;
    }

    sample_count = d3d8_msaa_sample_count(MultiSample);
    if (sample_count > 1) {
        UINT levels = 0;
        hr = ID3D11Device_CheckMultisampleQualityLevels(d3d8_GetD3D11Device(),
                dxgi, sample_count, &levels);
        if (FAILED(hr) || levels == 0) {
            UINT counts[] = { 9, 8, 4, 2, 1 };
            UINT i;
            for (i = 0; i < 5; i++) {
                if (counts[i] >= sample_count) continue;
                if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(
                        d3d8_GetD3D11Device(), dxgi, counts[i], &levels)) && levels > 0) {
                    sample_count = counts[i];
                    break;
                }
            }
            if (sample_count > 1)
                fprintf(stderr, "D3D8: depth MSAA %lu unsupported, fallback %lu samples\n",
                        (unsigned long)d3d8_msaa_sample_count(MultiSample),
                        (unsigned long)sample_count);
            else
                sample_count = 1;
        }
    }

    memset(&td, 0, sizeof(td));
    td.Width = Width;
    td.Height = Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = dxgi;
    td.SampleDesc.Count = sample_count;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = ID3D11Device_CreateTexture2D(d3d8_GetD3D11Device(), &td, NULL, &tex);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: CreateDepthStencilSurface failed: 0x%08lX (fmt=0x%X)\n", hr, Format);
        return hr;
    }

    *ppSurface = d3d8_surface_create(tex, 0, 0, Width, Height, surface_format,
                                     D3DPOOL_DEFAULT, D3DUSAGE_DEPTHSTENCIL,
                                     MultiSample, NULL, 0);
    ID3D11Texture2D_Release(tex);
    return *ppSurface ? S_OK : E_OUTOFMEMORY;
}

static HRESULT __stdcall dev_SetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pZStencilSurface)
{
    (void)self;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv = NULL;

    if (g_cur_rt) { IDirect3DSurface8_Release(&g_cur_rt->iface); g_cur_rt = NULL; }
    if (g_cur_ds) { IDirect3DSurface8_Release(&g_cur_ds->iface); g_cur_ds = NULL; }

    if (pRenderTarget) {
        g_cur_rt = (D3D8Surface *)pRenderTarget;
        if (!g_cur_rt->rtv) {
            g_cur_rt = NULL;
            fprintf(stderr, "D3D8: SetRenderTarget on non-renderable surface\n");
            return E_INVALIDARG;
        }
        rtv = g_cur_rt->rtv;
        IDirect3DSurface8_AddRef(pRenderTarget);
    } else {
        rtv = g_device_state.default_rtv;
    }

    if (pZStencilSurface) {
        g_cur_ds = (D3D8Surface *)pZStencilSurface;
        if (!g_cur_ds->dsv) {
            g_cur_ds = NULL;
            fprintf(stderr, "D3D8: SetRenderTarget with non-depth stencil surface\n");
        } else {
            dsv = g_cur_ds->dsv;
            IDirect3DSurface8_AddRef(pZStencilSurface);
        }
    }

    if (!dsv) dsv = g_device_state.default_dsv;
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context, 1,
                                            &rtv, dsv);
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 **ppRenderTarget)
{
    (void)self;
    if (!ppRenderTarget) return E_INVALIDARG;
    if (g_cur_rt) {
        *ppRenderTarget = &g_cur_rt->iface;
        IDirect3DSurface8_AddRef(*ppRenderTarget);
    } else {
        *ppRenderTarget = NULL;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetDepthStencilSurface(IDirect3DDevice8 *self, IDirect3DSurface8 **ppZStencilSurface)
{
    (void)self;
    if (!ppZStencilSurface) return E_INVALIDARG;
    if (g_cur_ds) {
        *ppZStencilSurface = &g_cur_ds->iface;
        IDirect3DSurface8_AddRef(*ppZStencilSurface);
    } else {
        *ppZStencilSurface = NULL;
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetViewport(IDirect3DDevice8 *self, const D3DVIEWPORT8 *pViewport)
{
    (void)self;
    if (pViewport) {
        g_device_state.viewport = *pViewport;

        D3D11_VIEWPORT d3d11_vp;
        d3d11_vp.TopLeftX = (FLOAT)pViewport->X;
        d3d11_vp.TopLeftY = (FLOAT)pViewport->Y;
        d3d11_vp.Width    = (FLOAT)pViewport->Width;
        d3d11_vp.Height   = (FLOAT)pViewport->Height;
        d3d11_vp.MinDepth = pViewport->MinZ;
        d3d11_vp.MaxDepth = pViewport->MaxZ;
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &d3d11_vp);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetViewport(IDirect3DDevice8 *self, D3DVIEWPORT8 *pViewport)
{
    (void)self;
    if (pViewport) *pViewport = g_device_state.viewport;
    return S_OK;
}

static HRESULT __stdcall dev_SetMaterial(IDirect3DDevice8 *self, const D3DMATERIAL8 *pMaterial)
{
    (void)self;
    if (pMaterial) g_device_state.material = *pMaterial;
    return S_OK;
}

static HRESULT __stdcall dev_GetMaterial(IDirect3DDevice8 *self, D3DMATERIAL8 *pMaterial)
{
    (void)self;
    if (pMaterial) *pMaterial = g_device_state.material;
    return S_OK;
}

static HRESULT __stdcall dev_SetLight(IDirect3DDevice8 *self, DWORD Index, const D3DLIGHT8 *pLight)
{
    (void)self;
    if (Index < MAX_LIGHTS && pLight) g_device_state.lights[Index] = *pLight;
    return S_OK;
}

static HRESULT __stdcall dev_GetLight(IDirect3DDevice8 *self, DWORD Index, D3DLIGHT8 *pLight)
{
    (void)self;
    if (Index < MAX_LIGHTS && pLight) *pLight = g_device_state.lights[Index];
    return S_OK;
}

static HRESULT __stdcall dev_LightEnable(IDirect3DDevice8 *self, DWORD Index, BOOL Enable)
{
    (void)self;
    if (Index < MAX_LIGHTS) g_device_state.light_enable[Index] = Enable;
    return S_OK;
}

static HRESULT __stdcall dev_CreateVertexShader(IDirect3DDevice8 *self, const DWORD *pDeclaration, const DWORD *pFunction, DWORD *pHandle, DWORD Usage)
{
    (void)self; (void)pDeclaration; (void)Usage;
    if (!pHandle) return E_INVALIDARG;
    if (!pFunction) return E_INVALIDARG;
    /* Count instructions: each is 4 DWORDs, last has bit 0 of word[3] set (END flag) */
    {
        int i, num_insns = 0;
        for (i = 0; i < 136; i++) {
            num_insns++;
            if (pFunction[i * 4 + 3] & 1) break;  /* END bit in last word */
        }
        return d3d8_vsh_create_shader(pFunction, num_insns, pHandle);
    }
}

static HRESULT __stdcall dev_SetVertexShader(IDirect3DDevice8 *self, DWORD Handle)
{
    (void)self;
    g_device_state.vertex_shader = Handle;
    return S_OK;
}

static HRESULT __stdcall dev_GetVertexShader(IDirect3DDevice8 *self, DWORD *pHandle)
{
    (void)self;
    if (pHandle) *pHandle = g_device_state.vertex_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetVertexShaderConstant(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount)
{
    (void)self;
    d3d8_vsh_set_constant(Register, pConstantData, ConstantCount);
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShader(IDirect3DDevice8 *self, DWORD Handle)
{
    (void)self;
    g_device_state.pixel_shader = Handle;
    d3d8_combiners_set_pixel_shader(Handle);
    return S_OK;
}

static HRESULT __stdcall dev_GetPixelShader(IDirect3DDevice8 *self, DWORD *pHandle)
{
    (void)self;
    if (pHandle) *pHandle = g_device_state.pixel_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShaderConstant(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount)
{
    (void)self; (void)Register; (void)pConstantData; (void)ConstantCount;
    return S_OK;
}

static void __stdcall dev_SetGammaRamp(IDirect3DDevice8 *self, DWORD Flags, const D3DGAMMARAMP *pRamp)
{
    (void)self; (void)Flags; /* Applied at the next presentation on both flag paths. */
    d3d8_gamma_set(pRamp);
}

static void __stdcall dev_GetGammaRamp(IDirect3DDevice8 *self, D3DGAMMARAMP *pRamp)
{
    (void)self;
    d3d8_gamma_get(pRamp);
}

static HRESULT __stdcall dev_SetPalette(IDirect3DDevice8 *self, DWORD PaletteNumber, const void *pEntries)
{
    (void)self;
    if (PaletteNumber >= D3D8_MAX_PALETTES) return E_INVALIDARG;
    d3d8_SetPalette(PaletteNumber, (const DWORD *)pEntries);

    /* A P8 texture bound to this stage bakes the palette at upload; re-bake
     * the raw indices so animated / colorized palettes take effect. */
    if (PaletteNumber < 4) {
        IDirect3DBaseTexture8 *tex = d3d8_GetStageTexture(PaletteNumber);
        if (tex) d3d8_refresh_palette(tex);
    }
    return S_OK;
}

static HRESULT __stdcall dev_BeginPush(IDirect3DDevice8 *self, DWORD Count, DWORD **ppPush)
{
    (void)self; (void)Count; (void)ppPush;
    /* TODO: Xbox push buffer emulation */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_EndPush(IDirect3DDevice8 *self, DWORD *pPush)
{
    (void)self; (void)pPush;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_Swap(IDirect3DDevice8 *self, DWORD Flags)
{
    (void)self; (void)Flags;

    /* Pump Windows messages (same as dev_Present) */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            recomp_exit_hook_run("window");     /* ExitProcess skips atexit */
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return d3d8_present();
}

/* ================================================================
 * Vtable
 * ================================================================ */

static const IDirect3DDevice8Vtbl g_device_vtbl = {
    dev_QueryInterface,
    dev_AddRef,
    dev_Release,
    dev_GetDirect3D,
    dev_GetDeviceCaps,
    dev_GetDisplayMode,
    dev_GetCreationParameters,
    dev_Reset,
    dev_Present,
    dev_GetBackBuffer,
    dev_BeginScene,
    dev_EndScene,
    dev_Clear,
    dev_SetTransform,
    dev_GetTransform,
    dev_SetRenderState,
    dev_GetRenderState,
    dev_SetTextureStageState,
    dev_GetTextureStageState,
    dev_SetTexture,
    dev_GetTexture,
    dev_SetStreamSource,
    dev_GetStreamSource,
    dev_SetIndices,
    dev_GetIndices,
    dev_DrawPrimitive,
    dev_DrawIndexedPrimitive,
    dev_DrawPrimitiveUP,
    dev_DrawIndexedPrimitiveUP,
    dev_CreateTexture,
    dev_CreateVertexBuffer,
    dev_CreateIndexBuffer,
    dev_CreateRenderTarget,
    dev_CreateDepthStencilSurface,
    dev_SetRenderTarget,
    dev_GetRenderTarget,
    dev_GetDepthStencilSurface,
    dev_SetViewport,
    dev_GetViewport,
    dev_SetMaterial,
    dev_GetMaterial,
    dev_SetLight,
    dev_GetLight,
    dev_LightEnable,
    dev_SetVertexShader,
    dev_GetVertexShader,
    dev_SetVertexShaderConstant,
    dev_SetPixelShader,
    dev_GetPixelShader,
    dev_SetPixelShaderConstant,
    dev_SetGammaRamp,
    dev_GetGammaRamp,
    dev_SetPalette,
    dev_BeginPush,
    dev_EndPush,
    dev_Swap,
    /* Extensions appended by xboxrecomp (slots after the historical ones) */
    dev_CreateImageSurface,
    dev_CreateCubeTexture,
    dev_CreateVolumeTexture,
};

/* ================================================================
 * Public API
 * ================================================================ */

IDirect3DDevice8 *xbox_GetD3DDevice(void)
{
    return g_device_initialized ? &g_device : NULL;
}

/* ================================================================
 * IDirect3D8 factory implementation
 * ================================================================ */

static IDirect3D8 g_d3d8;
static LONG g_d3d8_ref = 0;

static HRESULT __stdcall d3d8_QueryInterface(IDirect3D8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall d3d8_AddRef(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedIncrement(&g_d3d8_ref);
}

static ULONG __stdcall d3d8_Release(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedDecrement(&g_d3d8_ref);
}

static HRESULT __stdcall d3d8_CreateDevice(IDirect3D8 *self, UINT Adapter, DWORD DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP, IDirect3DDevice8 **ppDevice)
{
    (void)self; (void)Adapter; (void)DeviceType; (void)BehaviorFlags;
    HRESULT hr;

    if (!pPP || !ppDevice) return E_INVALIDARG;

    memset(&g_device_state, 0, sizeof(g_device_state));
    g_device_state.ref_count = 1;

    if (!pPP->hDeviceWindow) pPP->hDeviceWindow = hFocusWindow;

    hr = d3d11_create_device_and_swap_chain(&g_device_state, pPP);
    if (FAILED(hr)) return hr;

    hr = d3d11_create_render_targets(&g_device_state);
    if (FAILED(hr)) return hr;

    d3d8_init_default_states(&g_device_state);

    /* Set initial viewport (D3D11 requires explicit viewport) */
    {
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width    = (FLOAT)g_device_state.width;
        vp.Height   = (FLOAT)g_device_state.height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &vp);
    }

    /* Initialize shader and state subsystems */
    hr = d3d8_shaders_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Shader init failed: 0x%08lX\n", hr);
        return hr;
    }

    hr = d3d8_states_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: State init failed: 0x%08lX\n", hr);
        return hr;
    }

    hr = d3d8_combiners_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Combiner init failed: 0x%08lX\n", hr);
        /* Non-fatal: fall back to fixed-function pixel shaders */
    }

    hr = d3d8_vsh_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: VSH init failed: 0x%08lX\n", hr);
        /* Non-fatal: fall back to FVF vertex shaders */
    }

    g_device.lpVtbl = &g_device_vtbl;
    g_device_initialized = TRUE;

    *ppDevice = &g_device;
    fprintf(stderr, "D3D8: Device created (%ux%u)\n", g_device_state.width, g_device_state.height);
    return S_OK;
}

static const IDirect3D8Vtbl g_d3d8_vtbl = {
    d3d8_QueryInterface,
    d3d8_AddRef,
    d3d8_Release,
    d3d8_CreateDevice,
};

IDirect3D8 *xbox_Direct3DCreate8(UINT SDKVersion)
{
    (void)SDKVersion;
    g_d3d8.lpVtbl = &g_d3d8_vtbl;
    g_d3d8_ref = 1;
    return &g_d3d8;
}
