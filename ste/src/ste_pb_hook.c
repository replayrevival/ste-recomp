/* STE pushbuffer -> Kelvin hook (our own code; approach only, nothing copied from other repos).
 *
 * ste_pb_frame_hook() is called at D3DDevice_Swap entry (guest 0x222630, found with XbSymbolDatabase), on the
 * game thread, once per frame. The XDK D3D device (guest 0x22E280, pointer at [0x22E278]) holds, for XDK 5849
 * (measured with the candidate14p probe, see logs\CKPT14p_*.err):
 *   [dev+0x00] put cursor (guest VA in the contiguous window, 0x8xxxxxxx; phys = & 0x0FFFFFFF)
 *   [dev+0x24] ring start, [dev+0x28] ring end (0x80002000 .. 0x80202000)
 * The walker follows JUMP/CALL/RETURN, feeds every method to kelvin_method(), and ends a Kelvin frame at each
 * FLIP_STALL (0x0130) it passes. On first attach it replays from the ring start. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include "d3d8_xbox.h"
#include "nv2a_kelvin.h"
#include "d3d8_video.h"
#include "keyboard.h"
#include "ste_sdlinput.h"
static void ste_dump_dir(char *dir, size_t n);
/* c258: default directory of the debug screenshots (STE_SHOT_DIR overrides): <data>\logs\dumps\shots, never a development path */
static const char *ste_shot_dir_default(void) { static char d[MAX_PATH]; ste_dump_dir(d, sizeof d - 8); strcat_s(d, sizeof d, "\\shots"); return d; }

extern ptrdiff_t g_xbox_mem_offset;
extern uintptr_t g_kelvin_mem_base;
extern uint32_t g_kelvin_pb_addr, g_kelvin_pb_in_call;
extern void d3d8_capture_backbuffer_bmp_public(const char *path);

#define DEV_PTR_VA   0x0022E278u
#define DEV_PUT      0x00u
#define DEV_RING_LO  0x24u
#define DEV_RING_HI  0x28u
#define PHYS_MASK    0x0FFFFFFCu
#define M_FLIP_STALL 0x0130u

static long long sp_km, sp_pf; static long long g_t_km, g_t_ef, g_t_pf; static long long ste_q(void) { static int fine = -1; LARGE_INTEGER t; if (fine < 0) { const char *e = getenv("STE_PROF_FINE"); fine = (e && *e && *e != 0x30); } if (!fine) return 0; QueryPerformanceCounter(&t); return t.QuadPart; }   /* c95: per-method QPC pairs cost ~25% of render-thread busy time; only taken when STE_PROF_FINE=1 (else the [PROF] kelvin_method/end_frame/present columns read 0) */   /* c63 profile: kelvin_method / end_frame / present ticks */
static uint32_t rd(uint32_t va) { return *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + va); }
/* physical pushbuffer word, through the contiguous window at guest 0x80000000 */
static uint32_t rdp(uint32_t phys) { return rd(0x80000000u | (phys & PHYS_MASK)); }

static int       g_state;            /* 0 = not attached, 1 = attached, -1 = disabled/failed */
static uint32_t  g_last = 0xFFFFFFFFu;
static unsigned  g_frame;
static unsigned  g_log_every = 600;       /* STE_PB_LOG_EVERY */
static DWORD     g_shot_ms = 0;           /* STE_SHOT_SEC (c258: default off, was 10 s; periodic debug screenshots only on request) */
static HWND      g_hwnd;
static uint32_t  g_unh[8][0x2000 / 4];
static uint32_t  g_subch_methods[8];
static uint32_t  g_obj_logged;
static uint32_t  g_sem_dma, g_sem_off, g_sem_writes, g_sem_rejected;   /* NV097 semaphore: 0x01A8 DMA ctx, 0x1D6C offset, 0x1D70 release */

/* BACK_END_WRITE_SEMAPHORE_RELEASE: store the value at (semaphore DMA object base + SET_SEMAPHORE_OFFSET).
 * The title's semaphore DMA context is handle 0x0C, which XDK 5849 CDevice init (sub_00227220) creates as
 * class 0x3D with base 0x80000000 and limit 0x10000000 (the physical-memory window), so the semaphore
 * address is the physical address == the offset. Written through the contiguous window; other handles or
 * offsets past 64 MB are refused and counted. */
static void ste_sem_release(uint32_t value)
{
    volatile uint32_t *p;
    if (g_sem_dma != 0x0C || (g_sem_off & 3) || g_sem_off + 4 > 0x04000000u) {
        if (g_sem_rejected++ < 8) fprintf(stderr, "[PB-SEM] REFUSED release value=%08X dma=%08X off=%08X\n", value, g_sem_dma, g_sem_off);
        return;
    }
    p = (volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x80000000u + g_sem_off);
    if (g_sem_writes < 12 || (g_sem_writes % 2000) == 0) fprintf(stderr, "[PB-SEM] release #%u value=%08X -> phys %08X (was %08X, [dev+2C]=%08X)\n", g_sem_writes + 1, value, g_sem_off, *p, rd(rd(DEV_PTR_VA) + 0x2C));
    *p = value;
    g_sem_writes++;
}
static LRESULT CALLBACK ste_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) { ExitProcess(0); }
    switch (m) {   /* c134: keyboard as a port-0 pad (keyboard.c; ste_video.ini [input] keyboard, RECOMP_KEYBOARD) */
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        xbox_InputKeySet((int)w, 1); ste_sdl_key((int)w, 1);   /* c248: key bindings + overlay key */
        if (getenv("STE_KEY_TRACE")) { static unsigned kn; if (kn++ < 60) { fprintf(stderr, "[KEY] down vk=0x%02X\n", (unsigned)w); fflush(stderr); } }
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;   /* Alt+F4 and Alt+Space keep working */
    case WM_KEYUP: case WM_SYSKEYUP:
        xbox_InputKeySet((int)w, 0); ste_sdl_key((int)w, 0);
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;
    case WM_KILLFOCUS:
        xbox_InputKeysClear(); ste_sdl_keys_clear();
        break;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) { xbox_InputKeysClear(); ste_sdl_keys_clear(); }
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

static int ste_attach(void)
{
    const D3D8VideoCfg *vc = d3d8_video_cfg();   /* ste_video.ini / STE_VIDEO_* : output size, window mode, aspect */
    WNDCLASSA wc; RECT r = { 0, 0, (LONG)vc->out_w, (LONG)vc->out_h };
    DWORD wstyle = vc->mode == D3D8V_WINDOW ? WS_OVERLAPPEDWINDOW : WS_POPUP;
    /* c252: open on the monitor chosen by [video] display (primary when not set / out of range); borderless / fullscreen are centred on it (a full-size one lands exactly on its corner) */
    int wx = vc->mode == D3D8V_WINDOW ? vc->mon_x + 100 : vc->mon_x + ((int)vc->mon_w - (int)vc->out_w) / 2;
    int wy = vc->mode == D3D8V_WINDOW ? vc->mon_y + 100 : vc->mon_y + ((int)vc->mon_h - (int)vc->out_h) / 2;
    IDirect3D8 *d3d; IDirect3DDevice8 *dev = NULL; D3DPRESENT_PARAMETERS pp; HRESULT hr;
    const char *off = getenv("STE_KELVIN");
    if (off && off[0] == '0') { g_state = -1; fprintf(stderr, "[PB-ATTACH] disabled by STE_KELVIN=0\n"); return -1; }
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = ste_wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "STE_KELVIN";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);
    if (vc->mode == D3D8V_WINDOW) AdjustWindowRect(&r, wstyle, FALSE);
    g_hwnd = CreateWindowA("STE_KELVIN", "Godzilla STE - Kelvin", wstyle | WS_VISIBLE, wx, wy,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) { fprintf(stderr, "[PB-ATTACH] CreateWindow failed %lu\n", GetLastError()); g_state = -1; return -1; }
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = vc->out_w; pp.BackBufferHeight = vc->out_h; pp.BackBufferCount = 1; pp.Windowed = TRUE;
    pp.hDeviceWindow = g_hwnd; pp.EnableAutoDepthStencil = TRUE;
    d3d = xbox_Direct3DCreate8(0);
    hr = d3d ? d3d->lpVtbl->CreateDevice(d3d, 0, 1, g_hwnd, 0, &pp, &dev) : E_FAIL;
    if (FAILED(hr) || !dev) { fprintf(stderr, "[PB-ATTACH] D3D8 CreateDevice failed 0x%08lX\n", (unsigned long)hr); g_state = -1; return -1; }
    /* physical pushbuffer/texture/vertex addresses live in the contiguous window at guest 0x80000000 */
    g_kelvin_mem_base = (uintptr_t)g_xbox_mem_offset + 0x80000000u;
    kelvin_init();
    {
        uint32_t d = rd(DEV_PTR_VA);
        g_last = rd(d + DEV_RING_LO) & PHYS_MASK;
        fprintf(stderr, "[PB-ATTACH] tid=%lu dev=%08X ring %08X..%08X cursor=%08X; replay from ring start, kelvin mem base=%p\n",
                GetCurrentThreadId(), d, rd(d + DEV_RING_LO), rd(d + DEV_RING_HI), rd(d + DEV_PUT), (void *)g_kelvin_mem_base);
    }
    { const char *e = getenv("STE_PB_LOG_EVERY"); if (e && atoi(e) > 0) g_log_every = (unsigned)atoi(e); e = getenv("STE_SHOT_SEC"); if (e) g_shot_ms = (DWORD)atoi(e) * 1000u;   /* 0 = no periodic screenshots */ }
    g_state = 1;
    return 0;
}

/* ---- c84: asynchronous Kelvin (KELVIN_SYNC=1 restores the synchronous hook) ---- */
extern volatile int g_nv2a_get_async;
extern volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);
static int g_async = -1;               /* 1 = render thread, 0 = synchronous */
static void ste_set_get(uint32_t phys) { volatile uint32_t *g = xbox_Nv2aRegPtr(0x800044); if (g) *g = phys & PHYS_MASK; }
/* Walk physical [from, cur). Returns number of methods fed. */
static void ste_walk(uint32_t from, uint32_t cur, uint32_t ring_lo, uint32_t ring_hi,
                     unsigned *methods, unsigned *unhandled, unsigned *malformed, unsigned *flips, unsigned *jumps, unsigned *calls)
{
    uint32_t pos = from & PHYS_MASK, ret = 0, in_call = 0;
    unsigned budget = 0x400000;
    unsigned since_get = 0;
    while (budget--) {
        uint32_t w;
        if (g_async == 1 && !in_call && ++since_get >= 2048u) { since_get = 0; ste_set_get(pos); }   /* c84: tell the guest how far the pushbuffer has really been consumed */
        if (!in_call && pos == (cur & PHYS_MASK)) return;
        if (!in_call && (pos < ring_lo || pos >= ring_hi)) { (*malformed)++; return; }
        w = rdp(pos);
        if ((w & 0xE0000003u) == 0x20000000u) { (*jumps)++; pos = w & 0x1FFFFFFCu; continue; }
        if ((w & 3u) == 1u)                   { (*jumps)++; pos = w & PHYS_MASK; continue; }
        if ((w & 3u) == 2u) {
            (*calls)++;
            if (in_call) { (*malformed)++; pos += 4; continue; }
            ret = pos + 4; in_call = 1; pos = w & PHYS_MASK; continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {
            if (in_call) { pos = ret; in_call = 0; } else { (*malformed)++; pos += 4; }
            continue;
        }
        if ((w & 0x00030003u) == 0u && w != 0) {
            uint32_t count = (w >> 18) & 0x7FFu, subch = (w >> 13) & 7u, method = w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;
            uint32_t i;
            pos += 4;
            for (i = 0; i < count; i++) {
                uint32_t m = noninc ? method : method + i * 4u, param = rdp(pos);
                if (!in_call && pos == (cur & PHYS_MASK)) { (*malformed)++; return; }   /* command crossed the cursor */
                g_kelvin_pb_addr = pos; g_kelvin_pb_in_call = in_call;
                g_subch_methods[subch]++;
                if (m == 0 && g_obj_logged < 16) { g_obj_logged++; fprintf(stderr, "[PB-OBJ] subch=%u SET_OBJECT 0x%08X at %08X\n", subch, param, pos); }
                (*methods)++;
                if (m == 0x01A8) { if (g_sem_dma != param) fprintf(stderr, "[PB-SEM] SET_CONTEXT_DMA_SEMAPHORE handle=%08X\n", param); g_sem_dma = param; }
                if (m == 0x1D6C) { if (g_sem_off != param && g_sem_writes < 12) fprintf(stderr, "[PB-SEM] SET_SEMAPHORE_OFFSET %08X\n", param); g_sem_off = param; }
                else if (m == 0x1D70) ste_sem_release(param);
                else if (m == M_FLIP_STALL) { (*flips)++; { long long _t = ste_q(); kelvin_end_frame(); g_t_ef += ste_q() - _t; } }
    else { long long _t = ste_q(); int _r = kelvin_method(subch, m, param); g_t_km += ste_q() - _t; if (!_r) { (*unhandled)++; if (m < 0x2000) g_unh[subch][m / 4]++; } }
                pos += 4;
                if (!in_call && pos >= ring_hi) pos = ring_lo;
            }
            continue;
        }
        if (w == 0) { pos += 4; continue; }   /* padding NOP */
        (*malformed)++;
        pos += 4;
    }
    (*malformed)++;
}

static void ste_print_unhandled(void)
{
    struct { uint32_t n, key; } top[10]; int i, s; uint32_t m, k, tot = 0;
    memset(top, 0, sizeof(top));
    for (s = 0; s < 8; s++) for (m = 0; m < 0x2000 / 4; m++) {
        uint32_t n = g_unh[s][m]; if (!n) continue; tot += n;
        for (i = 0; i < 10; i++) if (n > top[i].n) { for (k = 9; k > (uint32_t)i; k--) top[k] = top[k - 1]; top[i].n = n; top[i].key = (uint32_t)s << 16 | (m * 4); break; }
    }
    fprintf(stderr, "[PB-UNHANDLED] total=%u top:", tot);
    for (i = 0; i < 10 && top[i].n; i++) fprintf(stderr, " s%u/%04X x%u", top[i].key >> 16, top[i].key & 0xFFFF, top[i].n);
    fprintf(stderr, "\n[PB-SUBCH] methods per subch:");
    for (i = 0; i < 8; i++) fprintf(stderr, " %u", g_subch_methods[i]);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* STE c62 probe: frame-time statistics between consecutive Swap entries, vblanks per frame, optional artificial slowdown.
 * Every 10 s: [FT] n avg p50 p99 (1% low) max, counts over 16.7/33.3/50 ms, histogram of vblanks per frame (0,1,2,3,4,5,6,7+).
 * STE_FRAME_SLEEP_MS=n sleeps n ms at every Swap (test: does game speed follow the frame rate?). */
/* STE: stats go to STE_STATS_FILE when set (so stderr can be silenced / NUL for a clean measurement), else stderr. */
static FILE *ste_statf(void) { static FILE *f; static int init; if (!init) { const char *p = getenv("STE_STATS_FILE"); init = 1; f = p && p[0] ? fopen(p, "w") : NULL; if (!f) f = stderr; else setvbuf(f, NULL, _IOFBF, 1 << 16); } return f; }
extern uint32_t xbox_VblankCount(void);
static int ste_cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void ste_ft_stats(void)
{
    static LARGE_INTEGER last, freq, win; static double buf[8192]; static unsigned n, vh[8]; static uint32_t last_vb; static int sl = -1;
    LARGE_INTEGER now; uint32_t vb = xbox_VblankCount();
    if (sl < 0) { const char *e = getenv("STE_FRAME_SLEEP_MS"); sl = e ? atoi(e) : 0; QueryPerformanceFrequency(&freq); }
    if (sl > 0) Sleep((DWORD)sl);
    QueryPerformanceCounter(&now);
    if (last.QuadPart) {
        unsigned dv = vb - last_vb;
        if (n < 8192) buf[n++] = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
        vh[dv > 7 ? 7 : dv]++;
    }
    last = now; last_vb = vb;
    if (!win.QuadPart) win = now;
    if ((now.QuadPart - win.QuadPart) >= 10 * freq.QuadPart && n > 10) {
        double sum = 0, mx = 0; unsigned o17 = 0, o33 = 0, o50 = 0, k; double sorted[8192];
        for (k = 0; k < n; k++) { sum += buf[k]; if (buf[k] > mx) mx = buf[k]; if (buf[k] > 16.9) o17++; if (buf[k] > 33.5) o33++; if (buf[k] > 50.0) o50++; sorted[k] = buf[k]; }
        qsort(sorted, n, sizeof(double), ste_cmp_d);
        fprintf(ste_statf(), "[FT] n=%u fps=%.1f avg=%.2f p50=%.2f p99=%.2f (1%%low=%.1f fps) max=%.2f >16.7:%u >33.3:%u >50:%u vb/frame[0..7+]=%u,%u,%u,%u,%u,%u,%u,%u sleep=%d\n",
                n, 1000.0 * n / sum, sum / n, sorted[n / 2], sorted[(unsigned)(n * 0.99)], 1000.0 / sorted[(unsigned)(n * 0.99)], mx, o17, o33, o50,
                vh[0], vh[1], vh[2], vh[3], vh[4], vh[5], vh[6], vh[7], sl);
        fflush(ste_statf()); n = 0; memset(vh, 0, sizeof(vh)); win = now;
    }
}

/* Walk one recorded range [from, cur), end the frame (present, screenshots, log). Runs on the render thread (async) or the game thread (sync). */
static void ste_process_range(uint32_t from, uint32_t cur, uint32_t lo, uint32_t hi)
{
    unsigned methods = 0, unhandled = 0, malformed = 0, flips = 0, jumps = 0, calls = 0;
    KelvinStats a, b;
    static DWORD t_shot;
    kelvin_get_stats(&a);
    ste_walk(from, cur, lo, hi, &methods, &unhandled, &malformed, &flips, &jumps, &calls);
    kelvin_get_stats(&b);
    g_frame++;
    if (g_frame <= 20 || (g_frame % g_log_every) == 0)
        fprintf(stderr, "[PB-FRAME] sem_writes=%u refused=%u\n", g_sem_writes, g_sem_rejected), fprintf(stderr, "[PB-FRAME] #%u seg %08X..%08X methods=%u draws=%u (prog=%u ff=%u) verts=%u clears=%u flips=%u jumps=%u calls=%u unhandled=%u malformed=%u texdec=%u badtexfmt=%u\n",
                g_frame, from, cur, methods, b.draws - a.draws, b.program_draws - a.program_draws, b.ff_draws - a.ff_draws,
                b.vertices - a.vertices, b.clears - a.clears, flips, jumps, calls, unhandled, malformed,
                b.textures_decoded - a.textures_decoded, b.unsupported_texture_formats - a.unsupported_texture_formats);
    if (g_frame == 20 || (g_frame % 600) == 0) ste_print_unhandled();
    { long long _t = ste_q(); d3d8_PresentFrame(); g_t_pf += ste_q() - _t; }
    {   /* c135 probe: STE_SHOT_FIGHT="n1,n2,..." (+ STE_SHOT_DIR): frame-synchronised screenshots for A/B runs of the fight intro. The trigger is the first frame with game-state word 4 (in-game)
         * and >= 350 draws; shots are taken n frames after it (file fs_<n>.bmp) and the draw count is logged ([FSHOT]). Probe only. */
        static int init, nn, trig, done_n; static unsigned list[512]; static unsigned trig_frame;
        if (!init) { const char *e = getenv("STE_SHOT_FIGHT"); init = 1;
            if (e && *e) { char b[4096], *tok, *ctx = NULL; strncpy(b, e, sizeof(b) - 1); b[sizeof(b) - 1] = 0; for (tok = strtok_s(b, ",", &ctx); tok && nn < 512; tok = strtok_s(NULL, ",", &ctx)) list[nn++] = (unsigned)atoi(tok); } }
        if (nn && done_n < nn) {
            unsigned dr = b.draws - a.draws;
            if (!trig && rd(0x003562B4u) == 4u && dr >= 350) { trig = 1; trig_frame = g_frame; fprintf(stderr, "[FSHOT] trigger frame=%u draws=%u\n", g_frame, dr); }
            if (trig && g_frame - trig_frame >= list[done_n]) {
                char path[MAX_PATH]; const char *dir = getenv("STE_SHOT_DIR");
                if (!dir) dir = ste_shot_dir_default();
                CreateDirectoryA(dir, NULL);
                _snprintf(path, sizeof(path), "%s\\fs_%04u.bmp", dir, list[done_n]);
                d3d8_capture_backbuffer_bmp_public(path);
                fprintf(stderr, "[FSHOT] n=%u frame=%u draws=%u file=%s\n", list[done_n], g_frame, dr, path);
                done_n++;
            }
        }
    }
    {
        DWORD now = GetTickCount();
        if (!t_shot) t_shot = now;
        if (g_shot_ms && now - t_shot >= g_shot_ms) {
            char path[MAX_PATH]; const char *dir = getenv("STE_SHOT_DIR");
            static unsigned shot;
            t_shot = now;
            if (!dir) dir = ste_shot_dir_default();
            CreateDirectoryA(dir, NULL);
            _snprintf(path, sizeof(path), "%s\\k_%03u_f%u.bmp", dir, shot++, g_frame);
            d3d8_capture_backbuffer_bmp_public(path);
            fprintf(stderr, "[PB-SHOT] frame=%u file=%s\n", g_frame, path);
        }
    }
}

/* Render-thread frame statistics: interval between consecutive completed frames (what the display receives), busy time per frame, cold/warm
 * shader counters.  [RFT] every 10 s to the stats file. */
static void ste_rft(double busy_ms)
{
    static LARGE_INTEGER last, freq, win; static double buf[8192], bsum; static unsigned n;
    LARGE_INTEGER now; extern unsigned g_psh_compiles;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (last.QuadPart) { if (n < 8192) { buf[n++] = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart; bsum += busy_ms; } }
    last = now;
    if (!win.QuadPart) win = now;
    if ((now.QuadPart - win.QuadPart) >= 10 * freq.QuadPart && n > 10) {
        double sum = 0, mx = 0; unsigned o17 = 0, o33 = 0, o50 = 0, k; static double sorted[8192];
        for (k = 0; k < n; k++) { sum += buf[k]; if (buf[k] > mx) mx = buf[k]; if (buf[k] > 16.9) o17++; if (buf[k] > 33.5) o33++; if (buf[k] > 50.0) o50++; sorted[k] = buf[k]; }
        qsort(sorted, n, sizeof(double), ste_cmp_d);
        fprintf(ste_statf(), "[RFT] n=%u fps=%.1f avg=%.2f p50=%.2f p99=%.2f max=%.2f >16.7:%u >33.3:%u >50:%u busy=%.2f psh_compiles=%u\n",
                n, 1000.0 * n / sum, sum / n, sorted[n / 2], sorted[(unsigned)(n * 0.99)], mx, o17, o33, o50, bsum / n, g_psh_compiles);
        fflush(ste_statf()); n = 0; bsum = 0; win = now;
    }
}

typedef struct { uint32_t from, cur, lo, hi, seq; } PbJob;   /* c89: seq = contiguous-pool sequence at submit (RECOMP_ASYNC_UAF guard) */
extern volatile int g_xbox_fence_on; extern volatile long g_xbox_fence_enq; extern void xbox_FenceFrameDone(long done);   /* c90: contiguous-free fence */
extern uint32_t xbox_ContigSeq(void); extern void xbox_AsyncUafBegin(uint32_t seq, uint32_t frame);
#define PBQ 64
static PbJob g_q[PBQ];
static volatile LONG g_q_w, g_q_r, g_enq_cnt, g_done_cnt;   /* single producer (game thread) / single consumer (render thread) */
static HANDLE g_ev_job, g_ev_done, g_ev_attached;
static uint32_t g_enq_from;
static int g_async_depth = 2;

static DWORD WINAPI ste_render_thread(LPVOID unused)
{
    (void)unused;
    if (ste_attach() < 0) g_state = -1; else ste_set_get(g_last);
    SetEvent(g_ev_attached);
    for (;;) {
        WaitForSingleObject(g_ev_job, INFINITE);
        while (g_q_r != g_q_w) {
            PbJob j = g_q[(unsigned)g_q_r % PBQ];
            LARGE_INTEGER q0, q1; double busy;
            QueryPerformanceCounter(&q0);
            if (g_state > 0) { xbox_AsyncUafBegin(j.seq, g_frame + 1); ste_process_range(j.from, j.cur, j.lo, j.hi); }
            QueryPerformanceCounter(&q1);
            busy = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / 10000000.0;   /* QPC is 10 MHz here; only used for the average in [RFT] */
            ste_set_get(j.cur);
            ste_rft(busy);
            InterlockedIncrement(&g_q_r);
            InterlockedIncrement(&g_done_cnt); xbox_FenceFrameDone(g_done_cnt);
            SetEvent(g_ev_done);
        }
    }
}

static void ste_watchdog_start(void);
static void ste_pb_frame_hook_impl(void)
{
    uint32_t dev, cur, lo, hi;
    { static int tc; const char *t = getenv("STE_CRASH_TEST"); if (t && t[0] == '1' && ++tc == 600) *(volatile int *)(uintptr_t)0x0000700000000010ULL = 1; }  /* c55: dump-path test */
    if (g_async < 0) {
        const char *e = getenv("KELVIN_SYNC");
        g_async = (e && e[0] && e[0] != '0') ? 0 : 1;
        g_async_depth = d3d8_video_cfg()->render_ahead;   /* ste_video.ini render_ahead (KELVIN_ASYNC_DEPTH / STE_VIDEO_RENDER_AHEAD override) */
        fprintf(stderr, "[PB-ASYNC] %s (depth %d)\n", g_async ? "asynchronous Kelvin render thread" : "KELVIN_SYNC=1: synchronous", g_async_depth);
    }
    if (!g_async) {
        if (g_state < 0) return;
        if (g_state == 0 && ste_attach() < 0) return;
        dev = rd(DEV_PTR_VA);
        cur = rd(dev + DEV_PUT) & PHYS_MASK;
        lo = rd(dev + DEV_RING_LO) & PHYS_MASK; hi = rd(dev + DEV_RING_HI) & PHYS_MASK;
        { LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0); ste_process_range(g_last, cur, lo, hi); QueryPerformanceCounter(&q1); ste_rft((double)(q1.QuadPart - q0.QuadPart) * 1000.0 / 10000000.0); }
        g_last = cur;
        return;
    }
    if (g_state < 0) return;
    if (!g_ev_job) {
        g_ev_job = CreateEventA(NULL, FALSE, FALSE, NULL); g_ev_done = CreateEventA(NULL, FALSE, FALSE, NULL); g_ev_attached = CreateEventA(NULL, TRUE, FALSE, NULL);
        { const char *fe = getenv("RECOMP_ASYNC_FENCE"); g_xbox_fence_on = !(fe && fe[0] == '0'); }   /* c90 */
        g_nv2a_get_async = 1;   /* DMA_GET now follows what the render thread has consumed */
        CloseHandle(CreateThread(NULL, 0, ste_render_thread, NULL, 0, NULL));
        ste_watchdog_start();
        WaitForSingleObject(g_ev_attached, INFINITE);
        if (g_state < 0) return;
        g_enq_from = g_last;
    }
    dev = rd(DEV_PTR_VA);
    cur = rd(dev + DEV_PUT) & PHYS_MASK;
    lo = rd(dev + DEV_RING_LO) & PHYS_MASK; hi = rd(dev + DEV_RING_HI) & PHYS_MASK;
    while (g_enq_cnt - g_done_cnt >= g_async_depth) WaitForSingleObject(g_ev_done, 2);   /* frame-latency throttle */
    g_q[(unsigned)g_q_w % PBQ].from = g_enq_from; g_q[(unsigned)g_q_w % PBQ].cur = cur; g_q[(unsigned)g_q_w % PBQ].lo = lo; g_q[(unsigned)g_q_w % PBQ].hi = hi; g_q[(unsigned)g_q_w % PBQ].seq = xbox_ContigSeq();
    g_enq_from = cur;
    MemoryBarrier();
    InterlockedIncrement(&g_enq_cnt); g_xbox_fence_enq = g_enq_cnt;
    InterlockedIncrement(&g_q_w);
    SetEvent(g_ev_job);
}
/* ---- STE c55: automatic crash dumps -------------------------------------------------------------
 * Unhandled exceptions (the candidate's veh_handler only logs and continues the search) write a
 * minidump (data segments + memory referenced from stacks/registers; STE_DUMP_FULL=1 = full memory, ~2.6 GB) plus a log line to STE_DUMP_DIR (default ..\logs\dumps, falling
 * back to .\dumps).  Installed from a CRT initialiser so it is active before WinMain, i.e. during
 * the early-boot file loads where an intermittent memmove fault has been seen. */
#include <timeapi.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
/* c256: where crash / watchdog dumps go. STE_DUMP_DIR (the launcher can set it to <data>\logs\dumps) wins; else <data>\logs\dumps next to the launcher's settings (STE_VIDEO_INI is
 * <data>\<settings folder>\ste_video.ini); else <exe dir>\data\logs\dumps (portable layout); else the old ..\logs\dumps; last resort .\dumps. Nothing outside the user's own folders is touched:
 * the old "less than 5 GB free -> redirect to a roomier drive" behaviour is now opt-in (STE_DUMP_FALLBACK_DIR). */
static int ste_dir_ok(const char *d) { DWORD a = GetFileAttributesA(d); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static void ste_dump_dir(char *dir, size_t n)
{
    const char *e = getenv("STE_DUMP_DIR"), *vi = getenv("STE_VIDEO_INI"), *fb = getenv("STE_DUMP_FALLBACK_DIR"), *mf = getenv("STE_DUMP_MINFREE_GB");
    char logs[MAX_PATH], exe[MAX_PATH], *p; dir[0] = 0;
    if (e && e[0]) strncpy(dir, e, n - 1);
    else {
        if (vi && vi[0]) {   /* <data>\<settings>\ste_video.ini -> <data>\logs */
            strncpy(logs, vi, sizeof logs - 1); logs[sizeof logs - 1] = 0;
            p = strrchr(logs, '\\'); if (p) { *p = 0; p = strrchr(logs, '\\'); if (p) { strcpy(p + 1, "logs"); if (ste_dir_ok(logs)) _snprintf(dir, n, "%s\\dumps", logs); } }
        }
        if (!dir[0]) {
            GetModuleFileNameA(NULL, exe, sizeof exe); p = strrchr(exe, '\\'); if (p) { strcpy(p + 1, "data\\logs"); if (ste_dir_ok(exe)) _snprintf(dir, n, "%s\\dumps", exe); }
        }
        if (!dir[0]) strncpy(dir, "..\\logs\\dumps", n - 1);
        if (fb && fb[0]) { ULARGE_INTEGER fr; double min_gb = mf && mf[0] ? atof(mf) : 5.0; if (GetDiskFreeSpaceExA("..\\logs", &fr, NULL, NULL) && (double)fr.QuadPart < min_gb * 1073741824.0) { CreateDirectoryA(fb, NULL); if (ste_dir_ok(fb)) strncpy(dir, fb, n - 1); } }
    }
    dir[n - 1] = 0;
    CreateDirectoryA(dir, NULL);
    if (!ste_dir_ok(dir)) { strncpy(dir, "dumps", n - 1); CreateDirectoryA(dir, NULL); }
}
static LONG WINAPI ste_crash_filter(EXCEPTION_POINTERS *ep)
{
    static volatile LONG once;
    char dir[MAX_PATH], path[MAX_PATH], exe[MAX_PATH], mod[MAX_PATH], *base; SYSTEMTIME st; HANDLE f; HMODULE hm = NULL; DWORD code = ep->ExceptionRecord->ExceptionCode;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    ste_dump_dir(dir, sizeof dir);
    GetModuleFileNameA(NULL, exe, sizeof(exe)); base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
    GetLocalTime(&st);
    _snprintf(path, sizeof(path), "%s\\%s_%04d%02d%02d-%02d%02d%02d_%08lX.dmp", dir, base, st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond, (unsigned long)code);
    /* c256: module+offset of the faulting address (the exe holds the recompiled game code, so exe+offset is enough to find the function) */
    strcpy(mod, "?");
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &hm) && hm) {
        char mp[MAX_PATH]; const char *mb; GetModuleFileNameA(hm, mp, sizeof mp); mb = strrchr(mp, '\\'); mb = mb ? mb + 1 : mp;
        _snprintf(mod, sizeof mod, "%s+0x%llX", mb, (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)hm)); mod[sizeof mod - 1] = 0;
    }
    fprintf(stderr, "[CRASH] code=0x%08lX addr=%p %s thread=%lu%s%s\n", (unsigned long)code, ep->ExceptionRecord->ExceptionAddress, mod, (unsigned long)GetCurrentThreadId(),
            ep->ExceptionRecord->NumberParameters > 1 && (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) ? (ep->ExceptionRecord->ExceptionInformation[0] ? " access=write" : " access=read") : "",
            code == EXCEPTION_INT_DIVIDE_BY_ZERO ? " (integer divide by zero)" : code == EXCEPTION_ACCESS_VIOLATION ? " (access violation)" : code == EXCEPTION_STACK_OVERFLOW ? " (stack overflow)" : "");
    fprintf(stderr, "[CRASHDUMP] exception %08lX at %p -> %s\n", (unsigned long)code, ep->ExceptionRecord->ExceptionAddress, path);
    fflush(stderr);
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        mei.ThreadId = GetCurrentThreadId(); mei.ExceptionPointers = ep; mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          (MINIDUMP_TYPE)((getenv("STE_DUMP_FULL") ? MiniDumpWithFullMemory : (MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory)) | MiniDumpWithThreadInfo | MiniDumpWithHandleData | MiniDumpWithUnloadedModules),
                          &mei, NULL, NULL);
        CloseHandle(f);
        fprintf(stderr, "[CRASHDUMP] written\n");
    } else fprintf(stderr, "[CRASHDUMP] could not create %s (error %lu)\n", path, GetLastError());
    fprintf(stderr, "[CRASH] exiting with code 0x%08lX (the Windows exception code; see docs\\LAUNCHER_INTERFACE.md)\n", (unsigned long)code); fflush(stderr);
    if (getenv("STE_CRASH_CONTINUE")) return EXCEPTION_CONTINUE_SEARCH;   /* debugging: hand over to WER / the debugger */
    TerminateProcess(GetCurrentProcess(), code);   /* exit code = exception code, no Windows error-report dialog that could keep the process (and the launcher's "game stopped" box) waiting */
    return EXCEPTION_EXECUTE_HANDLER;
}
/* c252: the launcher lists real monitor pixels and writes `display` in SDL3 order. Make the process per-monitor DPI aware before any window / metric is touched (without it a 4K screen at 150% reports
 * 2560x1440 and a 3840x2160 borderless request would not fit), and give the video layer the SDL3 display lookup. */
static int ste_display_resolver(int index, RECT *b, int *count)
{
    int x = 0, y = 0, w = 0, h = 0;
    if (!ste_sdl_display_bounds(index, &x, &y, &w, &h, count)) return 0;
    b->left = x; b->top = y; b->right = x + w; b->bottom = y + h; return 1;
}
static void __cdecl ste_early_init(void)
{
    typedef BOOL (WINAPI *SetCtx_t)(HANDLE);
    SetCtx_t f = (SetCtx_t)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
    BOOL ok = f ? f((HANDLE)(INT_PTR)-4) : FALSE;   /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
    if (!ok && !f) ok = SetProcessDPIAware();
    d3d8_display_resolver = ste_display_resolver;
    fprintf(stderr, "[VIDEO] DPI awareness: %s\n", ok ? "per-monitor" : "not set (already fixed by the system)"); fflush(stderr);
}
static void __cdecl ste_crash_init(void)
{
    const char *off = getenv("STE_NO_CRASHDUMP");
    if (off && off[0] == '1') return;
    SetUnhandledExceptionFilter(ste_crash_filter);
}
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) static void (__cdecl *ste_crash_init_p)(void) = ste_crash_init;
__declspec(allocate(".CRT$XCU")) static void (__cdecl *ste_early_init_p)(void) = ste_early_init;
/* STE_CRASH_TEST=1 raises an access violation from the first frame hook (used to verify the dump path). */

/* ---- c149: stall watchdog (a tester's c144 hang, 2026-10-25) -------------------------------------------------
 * The scanout/vblank threads keep running when the game thread or the render thread wedges, so the process looks alive. This thread watches the
 * number of pushbuffer frames the game thread has enqueued. If it does not move for STE_WATCHDOG_SECS (default 120; 0 = off) it logs every
 * thread once (CPU time over a 1 s window = spinning vs blocked, and a host stack with symbols from the exe's pdb) and writes a minidump
 * (STE_DUMP_DIR / STE_DUMP_FULL as for the crash dumps). STE_WATCHDOG_FORCE=<secs> fires once at that process age (to test the path).
 * Stacks are captured with RtlVirtualUnwind while the target is suspended (no allocation, no locks), symbolised after it is resumed. */
#include <tlhelp32.h>
typedef struct { DWORD tid; int n; DWORD64 pc[24]; DWORD64 rsp; ULONGLONG cpu0, cpu1; } SteWdThread;
static ULONGLONG ste_ft64(FILETIME a, FILETIME b) { ULARGE_INTEGER x, y; x.LowPart = a.dwLowDateTime; x.HighPart = a.dwHighDateTime; y.LowPart = b.dwLowDateTime; y.HighPart = b.dwHighDateTime; return x.QuadPart + y.QuadPart; }
static void ste_wd_fire(double stalled_s)
{
    SteWdThread th[96]; int nth = 0, i, j; HANDLE snap; THREADENTRY32 te; DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    fprintf(stderr, "[WATCHDOG] no pushbuffer frame enqueued for %.0f s (enq=%ld done=%ld q_r=%ld q_w=%ld fence_enq=%ld). Process age %.0f s. Dumping threads.\n",
            stalled_s, g_enq_cnt, g_done_cnt, g_q_r, g_q_w, g_xbox_fence_enq, (double)GetTickCount64() / 1000.0);
    fflush(stderr);
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || nth >= 96) continue;
        th[nth].tid = te.th32ThreadID; th[nth].n = 0; th[nth].cpu0 = th[nth].cpu1 = 0; nth++;
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    for (i = 0; i < nth; i++) {   /* CPU time at t0 */
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, th[i].tid); FILETIME c, e, k, u;
        if (h) { if (GetThreadTimes(h, &c, &e, &k, &u)) th[i].cpu0 = ste_ft64(k, u); CloseHandle(h); }
    }
    Sleep(1000);
    for (i = 0; i < nth; i++) {   /* suspend, unwind, resume */
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, th[i].tid); FILETIME c, e, k, u; CONTEXT ctx;
        if (!h) continue;
        if (GetThreadTimes(h, &c, &e, &k, &u)) th[i].cpu1 = ste_ft64(k, u);
        if (SuspendThread(h) != (DWORD)-1) {
            memset(&ctx, 0, sizeof(ctx)); ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(h, &ctx)) {
                th[i].rsp = ctx.Rsp;
                for (j = 0; j < 24; j++) {
                    DWORD64 base = 0, ip = ctx.Rip; PRUNTIME_FUNCTION rf; PVOID hd = NULL; DWORD64 ef = 0;
                    th[i].pc[th[i].n++] = ip;
                    rf = RtlLookupFunctionEntry(ip, &base, NULL);
                    if (rf) RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ip, rf, &ctx, &hd, &ef, NULL);
                    else { if (!ctx.Rsp) break; ctx.Rip = *(DWORD64 *)ctx.Rsp; ctx.Rsp += 8; }
                    if (!ctx.Rip) break;
                }
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    }
    for (i = 0; i < nth; i++) {
        char sb[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO *si = (SYMBOL_INFO *)sb;
        fprintf(stderr, "[WATCHDOG] thread %lu cpu=%.0f ms/s rsp=%p %s\n", th[i].tid, (double)(th[i].cpu1 - th[i].cpu0) / 10000.0, (void *)th[i].rsp,
                th[i].cpu1 - th[i].cpu0 > 2000000 ? "RUNNING" : "idle/blocked");
        for (j = 0; j < th[i].n; j++) {
            DWORD64 disp = 0; HMODULE mod = NULL; char mn[MAX_PATH] = "?"; const char *bn;
            memset(sb, 0, sizeof(sb)); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)th[i].pc[j], &mod);
            if (mod) GetModuleFileNameA(mod, mn, sizeof(mn));
            bn = strrchr(mn, '\\'); bn = bn ? bn + 1 : mn;
            if (SymFromAddr(GetCurrentProcess(), th[i].pc[j], &disp, si)) fprintf(stderr, "[WATCHDOG]   #%02d %016llX %s!%s+0x%llX\n", j, th[i].pc[j], bn, si->Name, disp);
            else fprintf(stderr, "[WATCHDOG]   #%02d %016llX %s+0x%llX\n", j, th[i].pc[j], bn, mod ? th[i].pc[j] - (DWORD64)(uintptr_t)mod : 0ULL);
        }
    }
    fflush(stderr);
    {   /* minidump (same directory logic as the crash filter) */
        char dir[MAX_PATH], path[MAX_PATH], exe[MAX_PATH], *base; SYSTEMTIME st; HANDLE f;
        ste_dump_dir(dir, sizeof dir);
        GetModuleFileNameA(NULL, exe, sizeof(exe)); base = strrchr(exe, '\\'); base = base ? base + 1 : exe; GetLocalTime(&st);
        _snprintf(path, sizeof(path), "%s\\%s_%04d%02d%02d-%02d%02d%02d_WATCHDOG.dmp", dir, base, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), pid, f, (MINIDUMP_TYPE)((getenv("STE_DUMP_FULL") ? MiniDumpWithFullMemory : (MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory)) |
                                        MiniDumpWithThreadInfo | MiniDumpWithHandleData | MiniDumpWithUnloadedModules), NULL, NULL, NULL);
            CloseHandle(f); fprintf(stderr, "[WATCHDOG] minidump %s -> %s\n", ok ? "written" : "FAILED", path);
            {   /* c177: the guest RAM itself (main map 0..64 MB and the contiguous window 0x80000000..+64 MB) next to the minidump, so a hang can be analysed at guest level (heap/object state) without a 2.6 GB full dump.
                 * c258: ~128 MB, so only with STE_WATCHDOG_GUEST=1 (release runs keep the minidump + log lines). */
                static const struct { uint32_t base; const char *tag; } rg[2] = { { 0u, "ram" }, { 0x80000000u, "contig" } }; int ri;
                const char *wg = getenv("STE_WATCHDOG_GUEST");
                for (ri = 0; ri < 2 && wg && wg[0] == '1'; ++ri) {
                    char gp[MAX_PATH]; HANDLE gf; static unsigned char gbuf[65536]; uint32_t goff;
                    _snprintf(gp, sizeof(gp), "%s.guest_%s.bin", path, rg[ri].tag); gf = CreateFileA(gp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (gf == INVALID_HANDLE_VALUE) continue;
                    for (goff = 0; goff < 0x04000000u; goff += (uint32_t)sizeof(gbuf)) {
                        SIZE_T got = 0; DWORD wr;
                        if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)((uintptr_t)g_xbox_mem_offset + rg[ri].base + goff), gbuf, sizeof(gbuf), &got) || got != sizeof(gbuf)) memset(gbuf + got, 0, sizeof(gbuf) - got);
                        WriteFile(gf, gbuf, (DWORD)sizeof(gbuf), &wr, NULL);
                    }
                    CloseHandle(gf); fprintf(stderr, "[WATCHDOG] guest %s written -> %s\n", rg[ri].tag, gp);
                }
            }
        } else fprintf(stderr, "[WATCHDOG] cannot create %s\n", path);
        fflush(stderr);
    }
}
static DWORD WINAPI ste_watchdog_thread(LPVOID unused)
{
    const char *e = getenv("STE_WATCHDOG_SECS"), *fo = getenv("STE_WATCHDOG_FORCE"); double secs = e && e[0] ? atof(e) : 120.0, force = fo && fo[0] ? atof(fo) : 0.0;
    LONG last = -1; ULONGLONG t_last = GetTickCount64(), t0 = t_last; int fired = 0, forced = 0; (void)unused;
    if (secs <= 0.0 && force <= 0.0) return 0;
    for (;;) {
        ULONGLONG now; LONG c;
        Sleep(1000); now = GetTickCount64(); c = g_enq_cnt;
        if (c != last) { last = c; t_last = now; fired = 0; continue; }
        if (force > 0.0 && !forced && (double)(now - t0) / 1000.0 >= force) { forced = 1; ste_wd_fire((double)(now - t_last) / 1000.0); continue; }
        if (secs > 0.0 && !fired && (double)(now - t_last) / 1000.0 >= secs) { fired = 1; ste_wd_fire((double)(now - t_last) / 1000.0); }
    }
}
static void ste_watchdog_start(void) { CloseHandle(CreateThread(NULL, 0, ste_watchdog_thread, NULL, 0, NULL)); }

/* STE c62 profile wrapper: hook (= pushbuffer walk + Kelvin) wall/CPU time per frame, guest-thread CPU per frame, Kelvin sub-timers. */
extern long long g_kelvin_perf[4];
static long long vtot_now(void) { KelvinStats ks; kelvin_get_stats(&ks); return (long long)ks.vertices; }
extern unsigned g_psh_compiles;
static unsigned long long ste_cyc(void) { ULONG64 c = 0; QueryThreadCycleTime(GetCurrentThread(), &c); return (unsigned long long)c; }

/* c78 probe (STE_SNAP_T="t1,t2,..." process-age seconds, STE_SNAP_DIR=<dir>): copies guest RAM [0x100000,0x4000000) at the first frame after each
 * time and writes it to <dir>\snap_<t>.bin on a worker thread (used to locate fighter state for the fps_cap physics check). */
extern ptrdiff_t g_xbox_mem_offset;
typedef struct { char path[MAX_PATH]; unsigned char *buf; size_t n; } SteSnap;
static DWORD WINAPI ste_snap_writer(LPVOID p) { SteSnap *s = (SteSnap *)p; FILE *f = fopen(s->path, "wb"); if (f) { fwrite(s->buf, 1, s->n, f); fclose(f); } VirtualFree(s->buf, 0, MEM_RELEASE); free(s); return 0; }
static void ste_snap_poll(void)
{
    static int init; static double ts[32]; static int nt, next; static char dir[MAX_PATH];
    if (!init) { const char *e = getenv("STE_SNAP_T"), *d = getenv("STE_SNAP_DIR"); init = 1;
        if (e && d) { char b[512], *tok, *ctx = NULL; strncpy(b, e, sizeof(b) - 1); b[sizeof(b) - 1] = 0; strncpy(dir, d, sizeof(dir) - 1);
            for (tok = strtok_s(b, ",", &ctx); tok && nt < 32; tok = strtok_s(NULL, ",", &ctx)) ts[nt++] = atof(tok); } }
    if (next < nt) {
        FILETIME c, x, k, u, now; ULARGE_INTEGER cu, nu; double age;
        GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u); GetSystemTimeAsFileTime(&now);
        cu.LowPart = c.dwLowDateTime; cu.HighPart = c.dwHighDateTime; nu.LowPart = now.dwLowDateTime; nu.HighPart = now.dwHighDateTime;
        age = (double)(nu.QuadPart - cu.QuadPart) / 1e7;
        if (age >= ts[next]) {
            SteSnap *s = (SteSnap *)calloc(1, sizeof(*s)); size_t n = 0x4000000u - 0x100000u;
            if (s) { s->buf = (unsigned char *)VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (s->buf) { {   /* guest RAM is only partly committed: copy page-chunk-wise with ReadProcessMemory (zero-fill what faults) */
                      size_t off; for (off = 0; off < n; off += 65536) { SIZE_T got = 0; size_t len = n - off < 65536 ? n - off : 65536;
                          if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)((uintptr_t)g_xbox_mem_offset + 0x100000u + off), s->buf + off, len, &got) || got != len) { if (got < len) memset(s->buf + off + got, 0, len - got); } } }
                    s->n = n;
                    snprintf(s->path, sizeof(s->path), "%s\\snap_%03d_%.1f.bin", dir, next, age);
                    fprintf(stderr, "[SNAP] %s at age %.2f\n", s->path, age); fflush(stderr);
                    CloseHandle(CreateThread(NULL, 0, ste_snap_writer, s, 0, NULL)); } else free(s); }
            next++;
        }
    }
}

/* c79 probe (STE_PROF_SAMPLE="t0:dur" process-age seconds): 1 kHz RIP sampler of the guest/main thread; at the end prints the top symbols to stderr. */
#include <timeapi.h>
#include <dbghelp.h>
static DWORD g_prof_tid; static HANDLE g_prof_h;
static int ste_cmp_u64(const void *a, const void *b) { unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b; return x < y ? -1 : x > y; }
static DWORD WINAPI ste_prof_thread(LPVOID p)
{
    double t0 = 0, dur = 0; static unsigned long long rips[400000]; unsigned n = 0; FILETIME c, x, k, u, now; ULARGE_INTEGER cu, nu; double age;
    const char *e = getenv("STE_PROF_SAMPLE"); (void)p;
    if (!e || sscanf(e, "%lf:%lf", &t0, &dur) != 2) return 0;
    for (;;) { GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u); GetSystemTimeAsFileTime(&now);
        cu.LowPart = c.dwLowDateTime; cu.HighPart = c.dwHighDateTime; nu.LowPart = now.dwLowDateTime; nu.HighPart = now.dwHighDateTime;
        age = (double)(nu.QuadPart - cu.QuadPart) / 1e7; if (age >= t0) break; Sleep(200); }
    timeBeginPeriod(1);
    { LARGE_INTEGER f, q, q0; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&q0);
      for (;;) { CONTEXT cx; QueryPerformanceCounter(&q); if ((double)(q.QuadPart - q0.QuadPart) / (double)f.QuadPart > dur || n >= 400000) break;
        memset(&cx, 0, sizeof(cx)); cx.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(g_prof_h) != (DWORD)-1) { if (GetThreadContext(g_prof_h, &cx)) rips[n++] = cx.Rip; ResumeThread(g_prof_h); }
        Sleep(1); } }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(GetCurrentProcess(), NULL, TRUE);
    qsort(rips, n, sizeof(rips[0]), ste_cmp_u64);
    {   static char names[4096][96]; static unsigned cnt[4096]; int nn = 0; unsigned i;
        for (i = 0; i < n; ) {   /* group identical RIP first, resolve once */
            unsigned j = i; char nm[96]; union { SYMBOL_INFO si; char b[sizeof(SYMBOL_INFO) + 128]; } sy; DWORD64 d = 0; int q;
            while (j < n && rips[j] == rips[i]) j++;
            memset(&sy, 0, sizeof(sy)); sy.si.SizeOfStruct = sizeof(SYMBOL_INFO); sy.si.MaxNameLen = 127;
            if (SymFromAddr(GetCurrentProcess(), rips[i], &d, &sy.si)) _snprintf(nm, sizeof(nm), "%s", sy.si.Name);
            else { HMODULE hm = NULL; char mod[64] = "?"; if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)rips[i], &hm)) GetModuleFileNameA(hm, mod, sizeof(mod)); _snprintf(nm, sizeof(nm), "?%s+%llX", strrchr(mod, 92) ? strrchr(mod, 92) + 1 : mod, (unsigned long long)(rips[i] - (uintptr_t)hm)); }
            nm[95] = 0;
            for (q = 0; q < nn && strcmp(names[q], nm); ++q) ;
            if (q == nn && nn < 4096) { strcpy(names[nn], nm); cnt[nn] = 0; nn++; }
            if (q < nn) cnt[q] += j - i;
            i = j; }
        for (int pass = 0; pass < 45; ++pass) { int best = -1; for (int q = 0; q < nn; ++q) if (cnt[q] && (best < 0 || cnt[q] > cnt[best])) best = q; if (best < 0) break;
            fprintf(stderr, "[PROF-SAMPLE] %5.2f%% %s\n", 100.0 * cnt[best] / n, names[best]); cnt[best] = 0; }
        fprintf(stderr, "[PROF-SAMPLE] samples=%u\n", n); fflush(stderr); }
    return 0;
}
static void ste_prof_start(void)
{
    static int started;
    if (started || !getenv("STE_PROF_SAMPLE")) return; started = 1;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_prof_h, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    CloseHandle(CreateThread(NULL, 0, ste_prof_thread, NULL, 0, NULL));
}

/* c82 probe (STE_WATCH="addr,addr,..." hex guest addresses, STE_WATCH_FILE=<file>): one line per frame: wall seconds, then each address as a float */
static void ste_watch_poll(void)
{
    static int init; static unsigned addr[16]; static int na; static FILE *wf; static LARGE_INTEGER f, q0;
    if (!init) { const char *e = getenv("STE_WATCH"), *p = getenv("STE_WATCH_FILE"); init = 1;
        if (e && p) { char b[512], *tok, *ctx = NULL; strncpy(b, e, sizeof(b) - 1); b[sizeof(b) - 1] = 0;
            for (tok = strtok_s(b, ",", &ctx); tok && na < 16; tok = strtok_s(NULL, ",", &ctx)) addr[na++] = (unsigned)strtoul(tok, NULL, 16);
            wf = fopen(p, "w"); QueryPerformanceFrequency(&f); QueryPerformanceCounter(&q0); } }
    if (wf) {
        LARGE_INTEGER q; int i; QueryPerformanceCounter(&q);
        fprintf(wf, "%.4f", (double)(q.QuadPart - q0.QuadPart) / (double)f.QuadPart);
        for (i = 0; i < na; ++i) { float v = 0; SIZE_T got = 0; ReadProcessMemory(GetCurrentProcess(), (LPCVOID)((uintptr_t)g_xbox_mem_offset + addr[i]), &v, 4, &got); fprintf(wf, " %.5f", v); }
        fprintf(wf, "\n");
    }
}
/* c167 probe (STE_CAMPROBE=1, observation only): on every change of the game-state word 0x3562B4 (and every 10 s) log the stored aspect (+0x148), the extents (+0x130/+0x134) and fov (+0x144) of the shell cameras */
static void ste_cam_probe(void)
{
    static int init, on; static uint32_t last = 0xFFFFFFFFu; static unsigned long long lastt;
    static const uint32_t cams[4] = { 0x00425700u, 0x004259F0u, 0x0042D480u, 0x00425EB0u };
    uint32_t st; unsigned long long now = GetTickCount64(); int i, force;
    if (!init) { const char *e = getenv("STE_CAMPROBE"); init = 1; on = (e && *e && *e != '0'); }
    if (!on) return;
    st = rd(0x003562B4u); force = (now - lastt) > 10000;
    if (st == last && !force) return;
    last = st; lastt = now;
    fprintf(stderr, "[CAMPROBE] t=%llu state=0x%X", now, st);
    for (i = 0; i < 4; ++i) { union { uint32_t u; float f; } a, e0, e1, fv; a.u = rd(cams[i] + 0x148); e0.u = rd(cams[i] + 0x130); e1.u = rd(cams[i] + 0x134); fv.u = rd(cams[i] + 0x144);
        fprintf(stderr, " | %05X asp=%.4f ext=%.4f/%.4f fov=%.4f", cams[i] & 0xFFFFF, a.f, e0.f, e1.f, fv.f); }
    fprintf(stderr, "\n"); fflush(stderr);
}void ste_pb_frame_hook(void)
{
    static LARGE_INTEGER freq, win_start, last_enter; static unsigned long long last_cyc, cps_c0; static LARGE_INTEGER cps_q0; static double cyc_per_ms;
    static double sum_wall, sum_cpu, sum_hw, sum_hc, sum_tex, sum_xf, sum_sub; static unsigned nfr; static long long pk0[4], tk0, te0, tp0, v0;
    LARGE_INTEGER q0, q1; unsigned long long c0, c1;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&cps_q0); cps_c0 = ste_cyc(); }
    QueryPerformanceCounter(&q0); c0 = ste_cyc();
    if (cyc_per_ms == 0.0 && (q0.QuadPart - cps_q0.QuadPart) > freq.QuadPart) cyc_per_ms = (double)(c0 - cps_c0) / ((double)(q0.QuadPart - cps_q0.QuadPart) * 1000.0 / (double)freq.QuadPart);
    if (last_enter.QuadPart && cyc_per_ms > 0.0) {
        sum_wall += (double)(q0.QuadPart - last_enter.QuadPart) * 1000.0 / (double)freq.QuadPart;
        sum_cpu += (double)(c0 - last_cyc) / cyc_per_ms;
        nfr++;
    }
    ste_ft_stats();
    ste_prof_start();
    ste_snap_poll();
    ste_watch_poll();
    ste_cam_probe();
    { extern void ste_fe_camera_tick(void); ste_fe_camera_tick(); }   /* c168 */
    ste_pb_frame_hook_impl();
    d3d8_video_pace();   /* c76: ste_video.ini fps_cap (QPC pacing after the frame's work) */
    QueryPerformanceCounter(&q1); c1 = ste_cyc();
    sum_hw += (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    sum_hc += cyc_per_ms > 0.0 ? (double)(c1 - c0) / cyc_per_ms : 0.0;
    {   /* spike log: frames longer than 66 ms, with what the hook did and what was new this frame */
        static unsigned spikes, last_comp; static unsigned last_tex; KelvinStats ks; double fw;
        kelvin_get_stats(&ks);
        fw = last_enter.QuadPart ? (double)(q0.QuadPart - last_enter.QuadPart) * 1000.0 / (double)freq.QuadPart : 0.0;
        if (fw > 66.0 && spikes < 400) {
            spikes++;
            fprintf(ste_statf(), "[SPIKE] frame=%u wall=%.0f ms | this hook: wall=%.1f km=%.1f present=%.1f | new shaders=%u new tex decoded=%u clears=%u draws=%u\n",
                    g_frame, fw, (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)freq.QuadPart, (double)(g_t_km - sp_km) * 1000.0 / (double)freq.QuadPart,
                    (double)(g_t_pf - sp_pf) * 1000.0 / (double)freq.QuadPart, g_psh_compiles - last_comp, (unsigned)ks.textures_decoded - last_tex, (unsigned)ks.clears, (unsigned)ks.draws);
        }
        last_comp = g_psh_compiles; last_tex = (unsigned)ks.textures_decoded; sp_km = g_t_km; sp_pf = g_t_pf;
    }
    last_enter = q0; last_cyc = c0;   /* next interval starts at this entry; the hook's own time is part of the next interval's CPU */
    if (!win_start.QuadPart) { win_start = q1; memcpy(pk0, g_kelvin_perf, sizeof(pk0)); }
    if ((q1.QuadPart - win_start.QuadPart) >= 10 * freq.QuadPart && nfr > 10) {
        double k = 1000.0 / (double)freq.QuadPart / (double)nfr;
        sum_tex = (double)(g_kelvin_perf[0] - pk0[0]) * k; sum_xf = (double)(g_kelvin_perf[1] - pk0[1]) * k; sum_sub = (double)(g_kelvin_perf[2] - pk0[2]) * k;
        fprintf(ste_statf(), "[PROF] frames=%u per-frame ms: wall=%.1f threadCPU=%.1f blocked=%.1f | hook wall=%.1f hookCPU=%.1f guestCPU=%.1f | kelvin_method=%.2f (tex=%.2f xform=%.2f d3d-submit=%.2f) end_frame=%.2f present=%.2f draws/frame=%.0f verts/frame=%.0f\n",
                nfr, sum_wall / nfr, sum_cpu / nfr, (sum_wall - sum_cpu) / nfr, sum_hw / nfr, sum_hc / nfr, (sum_cpu - sum_hc) / nfr, (double)(g_t_km - tk0) * k, sum_tex, sum_xf, sum_sub, (double)(g_t_ef - te0) * k, (double)(g_t_pf - tp0) * k,
                (double)(g_kelvin_perf[3] - pk0[3]) / nfr, (double)(vtot_now() - v0) / nfr);
        fflush(ste_statf());
        sum_wall = sum_cpu = sum_hw = sum_hc = 0; nfr = 0; memcpy(pk0, g_kelvin_perf, sizeof(pk0)); win_start = q1; v0 = vtot_now(); tk0 = g_t_km; te0 = g_t_ef; tp0 = g_t_pf;
    }
}

/* c70 EXPERIMENT (Option 1 consistency test): called at entry of the fight frame-timer sub_000A59F0 (this in ecx).
 * Counts calls and, with STE_FORCE_FC=1, forces the byte [this+0xFC] (dt = 1/60 when non-zero, 1/30 when zero) before the
 * function reads it.  Logs once per 10 s: calls, calls/s, the byte as found, and the implied simulation speed (calls * dt). */
extern ptrdiff_t g_xbox_mem_offset;
void ste_timer_enter(uint32_t self)
{
    static int force = -1; static LARGE_INTEGER f, t0, last; static unsigned calls, win_calls, ones, zeros;
    LARGE_INTEGER now; volatile uint8_t *b = (volatile uint8_t *)((uintptr_t)self + 0xFC + (uintptr_t)g_xbox_mem_offset);
    if (force < 0) { const char *e = getenv("STE_FORCE_FC"); force = e && e[0] == '1'; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); last = t0; }
    if (*b) ones++; else zeros++;
    if (force) *b = 1;
    calls++; win_calls++;
    QueryPerformanceCounter(&now);
    if ((now.QuadPart - last.QuadPart) >= 10 * f.QuadPart) {
        double s = (double)(now.QuadPart - last.QuadPart) / (double)f.QuadPart;
        double dt = force ? 1.0 / 60.0 : 0.0; (void)dt;
        fprintf(stderr, "[TIMER] t=%.0fs self=%08X calls=%u (%.1f/s) byte_found_nonzero=%u zero=%u force=%d\n", (double)(now.QuadPart - t0.QuadPart) / (double)f.QuadPart, self, win_calls, win_calls / s, ones, zeros, force);
        fflush(stderr); win_calls = 0; ones = zeros = 0; last = now;
    }
}

/* c72: per-VA entry counters (observation only), dumped with the 10 s [TIMER]-style line */
static unsigned g_tv[32], g_tc[32]; static int g_tn;
static double g_fsum, g_fmax; static unsigned g_fn, g_clampact;
static unsigned g_dt30, g_dt60, g_dtother; static uint32_t g_dtlast; static double g_dtsum, g_dtmax; static unsigned g_dtn;
void ste_trace_enter2(unsigned va, uint32_t reg) { if (va == 0x84834u) { union { uint32_t u; float f; } q; q.u = reg; g_fsum += q.f; g_fn++; if (q.f > g_fmax) g_fmax = q.f; } else if (va == 0x8481Cu) g_clampact++; if (va == 0x84800u) { union { uint32_t u; float f; } q; q.u = reg; g_dtsum += q.f; g_dtn++; if (q.f > g_dtmax) g_dtmax = q.f; if (reg == 0x3D088889u) g_dt30++; else if (reg == 0x3C888889u) g_dt60++; else { g_dtother++; g_dtlast = reg; } } }
void ste_trace_enter(unsigned va)
{
    static LARGE_INTEGER f, last; LARGE_INTEGER now; int i;
    for (i = 0; i < g_tn && g_tv[i] != va; ++i) ;
    if (i == g_tn && g_tn < 32) { g_tv[g_tn] = va; g_tc[g_tn] = 0; g_tn++; }
    g_tc[i]++;
    QueryPerformanceCounter(&now);
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); last = now; }
    if (now.QuadPart - last.QuadPart >= 10 * f.QuadPart) {
        fprintf(stderr, "[TRACE10] game_s=%.2f calls=%u mean_dt=%.5f max_dt=%.5f speed=%.3f |", g_dtsum, g_dtn, g_dtn ? g_dtsum / g_dtn : 0.0, g_dtmax, g_dtsum / ((double)(now.QuadPart - last.QuadPart) / (double)f.QuadPart)); fprintf(stderr, " final_dt: sum=%.2f max=%.5f speed_final=%.3f clamp_path_calls=%u |", g_fsum, g_fmax, g_fsum / ((double)(now.QuadPart - last.QuadPart) / (double)f.QuadPart), g_clampact); g_fsum = g_fmax = 0; g_fn = g_clampact = 0; g_dtsum = g_dtmax = 0; g_dtn = 0; for (i = 0; i < g_tn; ++i) { fprintf(stderr, " %05X=%u", g_tv[i], g_tc[i]); g_tc[i] = 0; } fprintf(stderr, "\n"); fflush(stderr); last = now;
    }
}

/* c177 probe (observation only): guest spin loop in sub_000B05A0 (the main thread polled sub_0025039B(obj, &flags) forever in the c176 idle hang, ~15M lock/unlock pairs per second).
 * Called at the loop-back label loc_000B05DD with the list object (esi), the index (edi), the polled object pointer and eax; prints at the 200000th, 5M and 20M iteration:
 * 24 dwords of the list object, of the polled object and of the object adjusted by -0x1C (what sub_0025039B actually uses). */
void ste_spin_probe(unsigned list, unsigned idx, unsigned obj, unsigned eax_)
{
    static unsigned long long n; static unsigned shown; int i;
    n++;
    if (!(n == 200000ull || n == 5000000ull || n == 20000000ull) || shown >= 3) return;
    shown++;
    fprintf(stderr, "[SPIN] iter=%llu list=0x%08X idx=%u obj=0x%08X eax=0x%08X\n", n, list, idx, obj, eax_);
    fprintf(stderr, "[SPIN]  list:"); for (i = 0; i < 24; ++i) fprintf(stderr, " %08X", rd(list + 4u * (unsigned)i)); fprintf(stderr, "\n");
    if (obj > 0x10000u && obj < 0x04000000u) {
        fprintf(stderr, "[SPIN]  obj :"); for (i = 0; i < 24; ++i) fprintf(stderr, " %08X", rd(obj + 4u * (unsigned)i)); fprintf(stderr, "\n");
        fprintf(stderr, "[SPIN]  obj-1C:"); for (i = 0; i < 24; ++i) fprintf(stderr, " %08X", rd(obj - 0x1Cu + 4u * (unsigned)i)); fprintf(stderr, "\n");
    }
    fflush(stderr);
}

/* c184 probe (instrumentation only, STE_KW=1): guest wait timing at the poll sites of the XDK D3D pushbuffer code.
 * 0/10 = CDevice_KickOff PFB write-buffer-flush spin (0x100410 bit 16), 10 also starts (2); 12 = end of the sub_0021FD00 loop; 1/11 = D3D_BlockOnTime final fence spin; 3 = BlockOnTime entry. Prints every 5 s. */
static __declspec(thread) LONGLONG kw_t0[3];
static volatile LONGLONG kw_ns[3], kw_max[3]; static volatile LONG kw_n[3], kw_entries; static volatile LONGLONG kw_next;
void ste_kw(int c)
{
    static int on = -1; static LONGLONG qf; LARGE_INTEGER q; int k;
    if (on < 0) { const char *e = getenv("STE_KW"); LARGE_INTEGER f; QueryPerformanceFrequency(&f); qf = f.QuadPart; on = e && e[0] && e[0] != '0'; }
    if (!on) return;
    QueryPerformanceCounter(&q);
    if (c == 3) { InterlockedIncrement(&kw_entries); return; }
    if (c == 0) { kw_t0[0] = q.QuadPart; return; }
    if (c == 1) { kw_t0[1] = q.QuadPart; return; }
    if (c == 10 || c == 11 || c == 12) {
        k = (c == 10) ? 0 : (c == 11) ? 1 : 2;
        { LONGLONG dt = (q.QuadPart - kw_t0[k]) * 1000000000LL / qf; InterlockedAdd64(&kw_ns[k], dt); InterlockedIncrement(&kw_n[k]); if (dt > kw_max[k]) kw_max[k] = dt; }
        if (c == 10) kw_t0[2] = q.QuadPart;
    }
    {   LONGLONG nx = kw_next;
        if (!nx) { InterlockedCompareExchange64(&kw_next, q.QuadPart + 5 * qf, 0); return; }
        if (q.QuadPart >= nx && InterlockedCompareExchange64(&kw_next, q.QuadPart + 5 * qf, nx) == nx) {
            int i; fprintf(stderr, "[KW] 5s:");
            for (i = 0; i < 3; ++i) { LONG n = InterlockedExchange(&kw_n[i], 0); LONGLONG ns = InterlockedExchange64(&kw_ns[i], 0), mx = kw_max[i]; kw_max[i] = 0;
                fprintf(stderr, " %s n=%ld avg=%.2fus max=%.1fus total=%.1fms%s", i == 0 ? "kick-flush" : i == 1 ? "fence-spin" : "put-loop", n, n ? ns / 1000.0 / n : 0.0, mx / 1000.0, ns / 1e6, i < 2 ? " |" : ""); }
            fprintf(stderr, " | BlockOnTime entries=%ld\n", InterlockedExchange(&kw_entries, 0)); fflush(stderr);
        }
    }
}