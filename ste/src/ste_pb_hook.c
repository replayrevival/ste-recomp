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
#include <math.h>
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
/* c322: the pushbuffer ring is 2 MB and the game thread, now much faster, can lap the render thread (a frame can use >50% of the ring): the writer then overwrites commands that have not been consumed and the walker replays
 * stale data (c317, c321 run b: 24M methods from a 290k-dword frame; also the old x8 / 4K crashes). Every frame is therefore consumed from a private copy of its ring range, taken on the game thread when the frame is handed over
 * (before the game writes the next one); the copy mirrors the ring offsets, one buffer per job in flight. STE_PB_SNAP=0 reads the live ring as before. */
static const uint32_t *g_walk_snap; static uint32_t g_walk_lo, g_walk_hi; volatile int g_ste_pb_snap_on = -1;
static uint32_t wrd(uint32_t pos) { pos &= PHYS_MASK; if (g_walk_snap && pos >= g_walk_lo && pos < g_walk_hi) return g_walk_snap[(pos - g_walk_lo) >> 2]; return rdp(pos); }

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
/* c262: pointer auto-hide. Hidden while the window is active and the mouse has been still for 2 s; shown on movement and
 * whenever the window is not active. ste_cursor_force_visible() is the hook for the 0.1.4 overlay (count of requests;
 * >0 keeps the pointer visible). No clipping, no capture. */
#define STE_CURSOR_IDLE_MS 2000u
#define STE_CURSOR_TIMER   0x5C01
static volatile LONG g_cursor_force;
static volatile LONG g_active = 1;
static DWORD         g_last_move;
void ste_cursor_force_visible(int on) { if (on) InterlockedIncrement(&g_cursor_force); else if (g_cursor_force > 0) InterlockedDecrement(&g_cursor_force); }
static int ste_cursor_should_hide(void) { return g_active && g_cursor_force <= 0 && (GetTickCount() - g_last_move) >= STE_CURSOR_IDLE_MS; }
static LRESULT CALLBACK ste_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) { ExitProcess(0); }
    switch (m) {
    case WM_MOUSEMOVE: g_last_move = GetTickCount(); break;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && ste_cursor_should_hide()) { SetCursor(NULL); return TRUE; }
        break;
    case WM_TIMER:
        if (w == STE_CURSOR_TIMER) {
            POINT p; RECT cr;   /* the pointer sits still, so Windows sends no WM_SETCURSOR: apply the state here */
            if (GetCursorPos(&p) && ScreenToClient(h, &p) && GetClientRect(h, &cr) && PtInRect(&cr, p) && GetForegroundWindow() == h) {
                HWND under; POINT sp; GetCursorPos(&sp); under = WindowFromPoint(sp);
                if (under == h) SetCursor(ste_cursor_should_hide() ? NULL : LoadCursorA(NULL, (LPCSTR)IDC_ARROW));
            }
            return 0;
        }
        break;
    }
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
        g_active = LOWORD(w) != WA_INACTIVE; g_last_move = GetTickCount();
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
    g_last_move = GetTickCount(); SetTimer(g_hwnd, STE_CURSOR_TIMER, 250, NULL);
    {   /* c266: STE_CAPTURE_RING=1 (frame ring, F12 saves it): captures go to <data>\logs\captures unless STE_CAPTURE_DIR says otherwise */
        const char *rq = getenv("STE_CAPTURE_RING");
        if (rq && rq[0] == '1' && !(getenv("STE_CAPTURE_DIR") && getenv("STE_CAPTURE_DIR")[0])) {
            char cd[MAX_PATH], *sl; ste_dump_dir(cd, sizeof cd - 16);
            sl = strrchr(cd, '\\'); if (sl && !_stricmp(sl, "\\dumps")) strcpy(sl, "\\captures"); else strcat(cd, "\\captures");
            _putenv_s("STE_CAPTURE_DIR", cd);
        }
    }
    if (vc->mode == D3D8V_WINDOW) {   /* c261: AdjustWindowRect uses the system DPI; on a monitor with another DPI the frame differs and the client area came out smaller (1280x720 -> 1274x703). Correct the outer size from the real client rect. */
        RECT cr, wr; GetClientRect(g_hwnd, &cr); GetWindowRect(g_hwnd, &wr);
        if ((cr.right - cr.left) != (LONG)vc->out_w || (cr.bottom - cr.top) != (LONG)vc->out_h) {
            SetWindowPos(g_hwnd, NULL, 0, 0, (wr.right - wr.left) + ((LONG)vc->out_w - (cr.right - cr.left)), (wr.bottom - wr.top) + ((LONG)vc->out_h - (cr.bottom - cr.top)), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            fprintf(stderr, "[VIDEO] window client %ldx%ld corrected to %ux%u (frame differs at this monitor's DPI)\n", cr.right - cr.left, cr.bottom - cr.top, vc->out_w, vc->out_h);
        }
    }
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
static unsigned g_pf_mh[0x800]; static int g_pf_mh_on;   /* c312: STE_PERF method histogram (see ste_pf_mh_report) */
/* Walk physical [from, cur). Returns number of methods fed. */
extern int kelvin_method_run(uint32_t subchannel, uint32_t method, const uint32_t *params, uint32_t count);
static void ste_walk(uint32_t from, uint32_t cur, uint32_t ring_lo, uint32_t ring_hi,
                     unsigned *methods, unsigned *unhandled, unsigned *malformed, unsigned *flips, unsigned *jumps, unsigned *calls)
{
    uint32_t pos = from & PHYS_MASK, ret = 0, in_call = 0;
    if (g_pf_mh_on == 0) { const char *pe = getenv("STE_PERF"); g_pf_mh_on = (pe && pe[0] && pe[0] != 0x30) ? 1 : -1; }
    unsigned budget = 0x400000;
    unsigned since_get = 0;
    while (budget--) {
        uint32_t w;
        if (g_async == 1 && !in_call && ++since_get >= 2048u) { since_get = 0; ste_set_get(pos); }   /* c84: tell the guest how far the pushbuffer has really been consumed */
        if (!in_call && pos == (cur & PHYS_MASK)) return;
        if (!in_call && (pos < ring_lo || pos >= ring_hi)) { (*malformed)++; return; }
        if ((pos & PHYS_MASK) >= 0x04000000u) { (*malformed)++; return; }   /* c313: a CALL/JUMP into garbage (beyond the 64 MB of RAM) ends the range instead of faulting the render thread */
        w = wrd(pos);
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
            if (noninc && count >= 4 && (method == 0x1800 || method == 0x1818)) {   /* c317: ARRAY_ELEMENT16 / INLINE_ARRAY runs in one call (kelvin_method_run), unless the run touches the cursor, the ring end or memory beyond RAM */
                uint32_t end = pos + 4u * count, cur_p = cur & PHYS_MASK;
                if ((in_call || (end <= ring_hi && !(cur_p >= pos && cur_p < end))) && (end & PHYS_MASK) <= 0x04000000u) {
                    g_kelvin_pb_addr = pos; g_kelvin_pb_in_call = in_call;
                    if (kelvin_method_run(subch, method, (g_walk_snap && pos >= g_walk_lo && end <= g_walk_hi) ? g_walk_snap + ((pos - g_walk_lo) >> 2) : (const uint32_t *)((uintptr_t)g_xbox_mem_offset + (0x80000000u | (pos & PHYS_MASK))), count)) {
                        g_subch_methods[subch] += count; (*methods) += count; if (g_pf_mh_on > 0) g_pf_mh[(method >> 2) & 0x7FF] += count;
                        g_kelvin_pb_addr = end - 4u; pos = end;
                        if (!in_call && pos >= ring_hi) pos = ring_lo;
                        continue;
                    }
                }
            }
            for (i = 0; i < count; i++) {
                uint32_t m = noninc ? method : method + i * 4u, param = wrd(pos);
                if (!in_call && pos == (cur & PHYS_MASK)) { (*malformed)++; return; }   /* command crossed the cursor */
                g_kelvin_pb_addr = pos; g_kelvin_pb_in_call = in_call;
                g_subch_methods[subch]++;
                if (g_pf_mh_on > 0) g_pf_mh[(m >> 2) & 0x7FF]++;   /* c312: STE_PERF method histogram */
                if (m == 0 && g_obj_logged < 16) { g_obj_logged++; fprintf(stderr, "[PB-OBJ] subch=%u SET_OBJECT 0x%08X at %08X\n", subch, param, pos); }
                (*methods)++;
                if (m == 0x01A8) { if (g_sem_dma != param) fprintf(stderr, "[PB-SEM] SET_CONTEXT_DMA_SEMAPHORE handle=%08X\n", param); g_sem_dma = param; }
                if (m == 0x1D6C) { if (g_sem_off != param && g_sem_writes < 12) fprintf(stderr, "[PB-SEM] SET_SEMAPHORE_OFFSET %08X\n", param); g_sem_off = param; }
                else if (m == 0x1D70) ste_sem_release(param);
                else if (m == M_FLIP_STALL) { (*flips)++; { long long _t = ste_q(); kelvin_end_frame(); g_t_ef += ste_q() - _t; } }
    else { long long _t = ste_q(); int _r = kelvin_method(subch, m, param); g_t_km += ste_q() - _t; if (!_r) { (*unhandled)++; if (m < 0x2000) { if (g_unh[subch][m / 4]++ == 0 && g_frame > 100) { static int nu_on = -1; if (nu_on < 0) { const char *e = getenv("STE_NEWUNH_LOG"); nu_on = e && *e && *e != 0x30; } if (nu_on) fprintf(stderr, "[NEWUNH] frame=%u subch=%u method=%04X param=%08X (first time seen)\n", g_frame, subch, m, param); } }   /* c326 probe */ } }
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
    {   /* c319 diagnostic: more methods than dwords in the range = the walker went round the ring again (a replay of stale commands); logged for the first 20 such frames */
        uint32_t dw = (cur >= from ? cur - from : (hi - lo) - (from - cur)) >> 2; static unsigned rp;
        if (methods > dw * 2u + 2000u && rp < 20) { uint32_t dv = rd(DEV_PTR_VA), put_now = rd(dv + DEV_PUT) & PHYS_MASK; rp++;
            fprintf(stderr, "[PB-REPLAY] #%u frame=%u seg %08X..%08X ring %08X..%08X dwords=%u methods=%u jumps=%u calls=%u malformed=%u | game write pointer now %08X\n", rp, g_frame + 1, from, cur, lo, hi, dw, methods, jumps, calls, malformed, put_now); fflush(stderr); } }
    kelvin_get_stats(&b);
    g_frame++;
    {   /* c327: STE_RING_FIGHT_FROM=<n>[,<len>]: from n Kelvin frames after the in-game state word turned 4 (fight) for len frames (default 700), raise the flash counter every 45 frames so the frame ring (STE_RING_FLASH=1) saves through a scripted moment such as a missile launch */
        static int init; static unsigned from, len = 700, first4, last; extern unsigned g_kelvin_flash_hits;
        if (!init) { const char *e = getenv("STE_RING_FIGHT_FROM"); init = 1; if (e) { from = (unsigned)atoi(e); const char *c = strchr(e, 0x2C); if (c && atoi(c + 1) > 0) len = (unsigned)atoi(c + 1); } }
        if (from && rd(0x003562B4u) == 4u) { if (!first4) first4 = g_frame; if (g_frame - first4 >= from && g_frame - first4 < from + len && g_frame - last >= 45u) { last = g_frame; g_kelvin_flash_hits++; } }
    }
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
    {   /* c263 probe: STE_SHOT_FLASH=<n>: up to n shots (fl_<frame>.bmp) on frames in which the renderer saw a near-full-screen small draw (STE_FSQUAD_LOG must be set), >= 3 frames apart */
        extern unsigned g_kelvin_fsq_count; static int init; static unsigned lim, taken, seen_cnt, last_fr;
        if (!init) { const char *e = getenv("STE_SHOT_FLASH"); init = 1; lim = e ? (unsigned)atoi(e) : 0; }
        if (lim && taken < lim && g_kelvin_fsq_count != seen_cnt && (!last_fr || g_frame - last_fr >= 3)) {
            char path[MAX_PATH]; const char *dir = getenv("STE_SHOT_DIR");
            if (!dir) dir = ste_shot_dir_default();
            CreateDirectoryA(dir, NULL);
            _snprintf(path, sizeof(path), "%s\\fl_%05u.bmp", dir, g_frame);
            d3d8_capture_backbuffer_bmp_public(path); taken++; last_fr = g_frame;
            fprintf(stderr, "[FLASHSHOT] frame=%u fsq=%u file=%s\n", g_frame, g_kelvin_fsq_count, path);
        }
        seen_cnt = g_kelvin_fsq_count;
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
static unsigned g_pf_mh[0x800]; static int g_pf_mh_on;
static void ste_pf_mh_report(unsigned frames);
static volatile uint32_t g_pf_pbmax, g_pf_ringdw; static volatile float g_pf_outst_max;
/* c320: STE_TITLE_FPS=1 shows the frame rate in the window title (test builds): "Godzilla STE - 59.8 fps (16.7 ms avg, worst 21.0) | query scale 3.4/4 | pushbuffer 31%", refreshed twice a second.
 * Called from ste_rft on the render thread, which created and owns the window (async mode): a cross-thread SendMessage from the game thread stalled it and locked the game to 2 vblanks per frame (c319). */
static void ste_title_fps_rt(double interval_ms)
{
    static int on = -1; static unsigned n; static double worst, sum;
    if (on < 0) { const char *e = getenv("STE_TITLE_FPS"); on = (e && e[0] && e[0] != '0'); }
    if (!on || !g_hwnd) return;
    if (interval_ms > worst) worst = interval_ms;
    sum += interval_ms; n++;
    if (sum >= 500.0 && n) {
        extern float g_qv_scale_cur; extern volatile float g_ste_qv_max, g_ste_pb_fill; wchar_t w[200];
        _snwprintf(w, 200, L"Godzilla STE - %.1f fps (%.1f ms avg, worst %.1f) | query scale %.1f/%.1f | pushbuffer %.0f%%", 1000.0 * n / sum, sum / n, worst, (double)g_qv_scale_cur, (double)g_ste_qv_max, (double)g_ste_pb_fill * 100.0);
        w[199] = 0; SetWindowTextW(g_hwnd, w);
        sum = 0.0; worst = 0.0; n = 0;
    }
}
static void ste_rft(double busy_ms)
{
    static LARGE_INTEGER last, freq, win; static double buf[8192], bsum; static unsigned n;
    LARGE_INTEGER now; extern unsigned g_psh_compiles;
    { extern volatile float g_ste_render_busy_ms; g_ste_render_busy_ms = g_ste_render_busy_ms == 0.0f ? (float)busy_ms : g_ste_render_busy_ms * 0.8f + (float)busy_ms * 0.2f; }   /* c317 governor input */
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (last.QuadPart) { if (n < 8192) { buf[n++] = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart; bsum += busy_ms; } ste_title_fps_rt((double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart); }
    last = now;
    if (!win.QuadPart) win = now;
    if ((now.QuadPart - win.QuadPart) >= 10 * freq.QuadPart && n > 10) {
        double sum = 0, mx = 0; unsigned o17 = 0, o33 = 0, o50 = 0, k; static double sorted[8192];
        for (k = 0; k < n; k++) { sum += buf[k]; if (buf[k] > mx) mx = buf[k]; if (buf[k] > 16.9) o17++; if (buf[k] > 33.5) o33++; if (buf[k] > 50.0) o50++; sorted[k] = buf[k]; }
        qsort(sorted, n, sizeof(double), ste_cmp_d);
        fprintf(ste_statf(), "[RFT] n=%u fps=%.1f avg=%.2f p50=%.2f p99=%.2f max=%.2f >16.7:%u >33.3:%u >50:%u busy=%.2f psh_compiles=%u\n",
                n, 1000.0 * n / sum, sum / n, sorted[n / 2], sorted[(unsigned)(n * 0.99)], mx, o17, o33, o50, bsum / n, g_psh_compiles);
        { extern void d3d8_perf_report(unsigned frames, double busy_ms, double scale, unsigned opt_off); extern float g_qv_scale_cur; extern volatile uint32_t g_ste_opt_off; d3d8_perf_report(n, bsum, (double)g_qv_scale_cur, (unsigned)g_ste_opt_off); } ste_pf_mh_report(n);   /* c308: STE_PERF=1 */
        fflush(ste_statf()); n = 0; bsum = 0; win = now;
    }
}

/* c308 probe (opt-in): STE_PERF_SAMPLE=1 samples the instruction pointer of the game thread (which calls ste_pb_frame_hook) and of the render thread every ~1 ms
 * (SuspendThread / GetThreadContext / ResumeThread, nothing else runs while a thread is suspended) and every 10 s prints a per-module summary ([PERFS]) and appends the
 * hottest 30000 addresses per thread to STE_PERF_OUT (module, RVA, count) for offline symbolisation against the linker map.  Measurement only. */
static long long g_pg_thr, g_pg_pace;   /* game thread: QPC ticks waiting for the render thread / inside the fps pacer */
typedef struct { uintptr_t rip; uint32_t cnt; } PsEnt;
#define PS_N 65536u
static PsEnt g_ps[2][PS_N]; static HANDLE g_ps_h[2]; static int g_ps_on = -1; static unsigned g_ps_tot[2];
static void ste_ps_register(int which)
{
    if (g_ps_on < 0) { const char *e = getenv("STE_PERF_SAMPLE"); g_ps_on = (e && e[0] && e[0] != '0'); }
    if (g_ps_on && !g_ps_h[which]) DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_ps_h[which], 0, FALSE, DUPLICATE_SAME_ACCESS);
}
static int ps_cmp(const void *a, const void *b) { const PsEnt *x = (const PsEnt *)a, *y = (const PsEnt *)b; return x->cnt < y->cnt ? 1 : x->cnt > y->cnt ? -1 : 0; }
static void ps_dump(unsigned window)
{
    static const char *const tn[2] = { "game", "render" }; extern float g_qv_scale_cur; extern volatile uint32_t g_ste_opt_off; const char *outp = getenv("STE_PERF_OUT"); FILE *of = outp ? fopen(outp, "a") : NULL; int w;
    for (w = 0; w < 2; w++) {
        static PsEnt cp[PS_N]; unsigned n = 0, i, nm = 0; char names[8][64]; unsigned mc[8]; memset(mc, 0, sizeof mc);
        for (i = 0; i < PS_N; i++) if (g_ps[w][i].rip) cp[n++] = g_ps[w][i];
        memset(g_ps[w], 0, sizeof g_ps[w]);
        qsort(cp, n, sizeof(PsEnt), ps_cmp);
        if (of) fprintf(of, "W%u %s scale=%.2f opt=%x total=%u\n", window, tn[w], (double)g_qv_scale_cur, (unsigned)g_ste_opt_off, g_ps_tot[w]);
        for (i = 0; i < n; i++) {
            HMODULE hm = NULL; char path[MAX_PATH] = "?", *base; unsigned m;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)cp[i].rip, &hm) && hm) GetModuleFileNameA(hm, path, sizeof path);
            base = strrchr(path, '\\'); base = base ? base + 1 : path;
            if (of && i < 30000) fprintf(of, "%s %llx %u\n", base, (unsigned long long)(cp[i].rip - (uintptr_t)hm), cp[i].cnt);
            for (m = 0; m < nm; m++) if (!_stricmp(names[m], base)) break;
            if (m == nm && nm < 8) { strncpy(names[nm], base, 63); names[nm][63] = 0; mc[nm++] = 0; }
            if (m < nm) mc[m] += cp[i].cnt;
        }
        fprintf(ste_statf(), "[PERFS] W%u %s scale=%.2f samples=%u:", window, tn[w], (double)g_qv_scale_cur, g_ps_tot[w]);
        for (i = 0; i < nm; i++) fprintf(ste_statf(), " %s=%.1f%%", names[i], g_ps_tot[w] ? 100.0 * mc[i] / g_ps_tot[w] : 0.0);
        fprintf(ste_statf(), "\n"); fflush(ste_statf()); g_ps_tot[w] = 0;
    }
    if (of) fclose(of);
}
static DWORD WINAPI ste_ps_thread(LPVOID unused)
{
    HANDLE tm = CreateWaitableTimerExW(NULL, NULL, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS); LARGE_INTEGER due, t0, now, fq; unsigned window = 0; (void)unused;
    if (!tm) tm = CreateWaitableTimerW(NULL, FALSE, NULL);
    due.QuadPart = -10000; SetWaitableTimer(tm, &due, 1, NULL, NULL, FALSE);
    QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    for (;;) {
        int w; WaitForSingleObject(tm, 50);
        for (w = 0; w < 2; w++) if (g_ps_h[w] && SuspendThread(g_ps_h[w]) != (DWORD)-1) {
            CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(g_ps_h[w], &c)) {
                uintptr_t rip = (uintptr_t)c.Rip; unsigned h = (unsigned)((rip * 0x9E3779B1u) >> 7) & (PS_N - 1), k;
                for (k = 0; k < 64; k++, h = (h + 1) & (PS_N - 1)) { if (g_ps[w][h].rip == rip) { g_ps[w][h].cnt++; break; } if (!g_ps[w][h].rip) { g_ps[w][h].rip = rip; g_ps[w][h].cnt = 1; break; } }
                g_ps_tot[w]++;
            }
            ResumeThread(g_ps_h[w]);
        }
        QueryPerformanceCounter(&now);
        if (now.QuadPart - t0.QuadPart >= 10 * fq.QuadPart) { t0 = now; ps_dump(++window); }
    }
}
/* c312: STE_PERF=1 -> histogram of the pushbuffer methods the walker fed to Kelvin; [PERFM] top 12 per window (methods per frame) */
static void ste_pf_mh_report(unsigned frames)
{
    unsigned top[12], tn[12], i, k, j, tot = 0;
    if (g_pf_mh_on <= 0) return;
    for (k = 0; k < 12; k++) { top[k] = 0; tn[k] = 0; }
    for (i = 0; i < 0x800; i++) { unsigned c = g_pf_mh[i]; if (!c) continue; tot += c;
        for (k = 0; k < 12; k++) if (c > tn[k]) { for (j = 11; j > k; j--) { top[j] = top[j - 1]; tn[j] = tn[j - 1]; } top[k] = i << 2; tn[k] = c; break; } }
    fprintf(ste_statf(), "[PERFM] n=%u methods/frame=%.0f pushbuffer max=%u dwords of ring %u (%.0f%%) in-flight max=%.0f%% top:", frames, frames ? (double)tot / frames : 0.0, (unsigned)g_pf_pbmax, (unsigned)g_pf_ringdw, g_pf_ringdw ? 100.0 * g_pf_pbmax / g_pf_ringdw : 0.0, 100.0 * (double)g_pf_outst_max);
    for (k = 0; k < 12 && tn[k]; k++) fprintf(ste_statf(), " %04X=%.0f", top[k], (double)tn[k] / frames);
    fprintf(ste_statf(), "\n"); fflush(ste_statf()); memset(g_pf_mh, 0, sizeof g_pf_mh); g_pf_pbmax = 0; g_pf_outst_max = 0.0f;
}typedef struct { uint32_t from, cur, lo, hi, seq, snap; } PbJob;   /* c89: seq = contiguous-pool sequence at submit (RECOMP_ASYNC_UAF guard) */
extern volatile int g_xbox_fence_on; extern volatile long g_xbox_fence_enq; extern void xbox_FenceFrameDone(long done);   /* c90: contiguous-free fence */
extern uint32_t xbox_ContigSeq(void); extern void xbox_AsyncUafBegin(uint32_t seq, uint32_t frame);
#define PBQ 64
static PbJob g_q[PBQ];
#define PB_SNAPS 4
static uint32_t *g_snap[PB_SNAPS]; static uint32_t g_snap_bytes[PB_SNAPS];
static volatile LONG g_q_w, g_q_r, g_enq_cnt, g_done_cnt;   /* single producer (game thread) / single consumer (render thread) */
static HANDLE g_ev_job, g_ev_done, g_ev_attached;
static uint32_t g_enq_from;
static int g_async_depth = 2;

static DWORD WINAPI ste_render_thread(LPVOID unused)
{
    (void)unused; ste_ps_register(1);
    if (ste_attach() < 0) g_state = -1; else ste_set_get(g_last);
    SetEvent(g_ev_attached);
    for (;;) {
        WaitForSingleObject(g_ev_job, INFINITE);
        while (g_q_r != g_q_w) {
            PbJob j = g_q[(unsigned)g_q_r % PBQ];
            { uint32_t dw = (j.cur >= j.from ? (j.cur - j.from) : ((j.hi - j.lo) - (j.from - j.cur))) >> 2; if (dw > g_pf_pbmax) g_pf_pbmax = dw; g_pf_ringdw = (j.hi - j.lo) >> 2;
              { extern volatile float g_ste_pb_fill; static float h[3]; float f = g_pf_ringdw ? (float)dw / (float)g_pf_ringdw : 0.0f, m; h[2] = h[1]; h[1] = h[0]; h[0] = f; m = h[0] > h[1] ? h[0] : h[1]; if (h[2] > m) m = h[2]; g_ste_pb_fill = m; } }   /* c312: dwords in this frame's pushbuffer range; c318: pushbuffer fill of the last 3 frames for the query guard */
            LARGE_INTEGER q0, q1; double busy;
            QueryPerformanceCounter(&q0);
            if (g_state > 0) { xbox_AsyncUafBegin(j.seq, g_frame + 1); g_walk_snap = j.snap < PB_SNAPS ? g_snap[j.snap] : NULL; g_walk_lo = j.lo; g_walk_hi = j.hi; ste_process_range(j.from, j.cur, j.lo, j.hi); g_walk_snap = NULL; }
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
    {   /* c322: private copy of this frame's ring range (see wrd) */
        PbJob *jq = &g_q[(unsigned)g_q_w % PBQ]; unsigned slot = (unsigned)g_enq_cnt % PB_SNAPS; uint32_t ringb = hi - lo;
        if (g_ste_pb_snap_on < 0) { const char *e = getenv("STE_PB_SNAP"); g_ste_pb_snap_on = !(e && e[0] == '0'); fprintf(stderr, "[PB-SNAP] %s\n", g_ste_pb_snap_on ? "frames are consumed from a private copy of their ring range" : "STE_PB_SNAP=0: live ring"); fflush(stderr); }
        jq->snap = ~0u;
        if (g_ste_pb_snap_on && ringb && ringb <= 0x800000u && (lo & 3u) == 0 && g_enq_from >= lo && g_enq_from <= hi && cur >= lo && cur <= hi) {
            if (g_snap_bytes[slot] != ringb) { free(g_snap[slot]); g_snap[slot] = (uint32_t *)malloc(ringb); g_snap_bytes[slot] = g_snap[slot] ? ringb : 0; }
            if (g_snap[slot]) {
                const uint8_t *live = (const uint8_t *)((uintptr_t)g_xbox_mem_offset + 0x80000000u); uint8_t *dst = (uint8_t *)g_snap[slot] - lo;
                if (cur >= g_enq_from) memcpy(dst + g_enq_from, live + g_enq_from, cur - g_enq_from);
                else { memcpy(dst + g_enq_from, live + g_enq_from, hi - g_enq_from); memcpy(dst + lo, live + lo, cur - lo); }
                jq->snap = slot;
            }
        }
    }
    { LARGE_INTEGER tq0, tq1; QueryPerformanceCounter(&tq0);
    {   /* c322: pushbuffer data in flight when this frame is handed over = this frame's size + what the render thread has not consumed of the previous one (GET is what it last reported). Over 100% of the ring means the writer lapped the reader. */
        extern volatile float g_ste_pb_outst; static float h[3]; static unsigned lap_logged; volatile uint32_t *gp = xbox_Nv2aRegPtr(0x800044); uint32_t ringb = hi - lo;
        if (ringb && gp) {
            uint32_t get = *gp & PHYS_MASK, size = cur >= g_enq_from ? cur - g_enq_from : ringb - (g_enq_from - cur), rem = g_enq_from >= get ? g_enq_from - get : g_enq_from + ringb - get; float f = (float)((double)size + (double)rem) / (float)ringb, m;
            h[2] = h[1]; h[1] = h[0]; h[0] = f; m = h[0] > h[1] ? h[0] : h[1]; if (h[2] > m) m = h[2]; g_ste_pb_outst = m;
            if (f > g_pf_outst_max) g_pf_outst_max = f;
            if (f >= 1.0f && lap_logged < 20) { lap_logged++; fprintf(stderr, "[PBLAP] frame data in flight %.0f%% of the ring (this frame %u + unconsumed %u of %u bytes): the writer lapped the reader\n", f * 100.0, size, rem, ringb); fflush(stderr); }
        }
    }
    while (g_enq_cnt - g_done_cnt >= g_async_depth) WaitForSingleObject(g_ev_done, 2);   /* frame-latency throttle */
    QueryPerformanceCounter(&tq1); g_pg_thr += tq1.QuadPart - tq0.QuadPart; }
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
 * the old "less than 5 GB free -> a roomier drive" redirect is now opt-in (STE_DUMP_FALLBACK_DIR). */
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
    { extern void ste_tee_drain(void); ste_tee_drain(); }   /* c367: flush the --console mirror before the process ends */
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
/* c317: query_scale governor (opt-in: [video] query_scale_auto=1 / STE_VIDEO_QUERY_AUTO=1; query_scale then is the MAXIMUM). The wider scenery query costs CPU on both threads: the game thread (object setup, push
 * buffer writing) and the render thread (Kelvin translation). Every frame the game thread's CPU time (thread cycles, calibrated once against the QPC) and the render thread's busy time (g_ste_render_busy_ms,
 * set in ste_rft) are smoothed; if the larger of the two exceeds the budget (query_budget_ms, default 14 ms of the 16.7 ms frame) the cap g_ste_qv_auto_cap that ste_hook_qvol_pre applies to the scale is lowered
 * quickly, and it creeps back up (about +0.4% per frame) while the load stays below 90% of the budget. Heavy moments therefore shorten the draw distance for a while instead of dropping frames. */
volatile int g_ste_qv_auto_on, g_ste_qv_guard_on; volatile float g_ste_qv_auto_cap = 16.0f, g_ste_qv_budget_ms = 14.0f, g_ste_qv_max = 4.0f; volatile float g_ste_render_busy_ms, g_ste_pb_fill, g_ste_pb_outst, g_ste_qv_ramp = 5.0f;   /* g_ste_qv_ramp: largest change of the effective scale, percent per second (both directions) */
#define QV_FILL_HI 0.80   /* c322: pushbuffer data in flight (this frame + the unconsumed rest of the previous one) above this share of the ring: the writer is about to lap the reader */
#define QV_FILL_AIM 0.68
static void ste_qv_governor(unsigned long long dcyc)
{
    static double cyc_per_ms, game_ms, last_log; static LARGE_INTEGER qf; double load = 0.0, cap = (double)g_ste_qv_auto_cap, budget = (double)g_ste_qv_budget_ms, mx = (double)g_ste_qv_max, fill = (double)g_ste_pb_outst; int over = 0, calm = 1;
    if (!g_ste_qv_guard_on) return;
    if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    if (g_ste_qv_auto_on) {
        if (cyc_per_ms == 0.0) {   /* calibrate thread cycles per ms: spin 25 ms */
            LARGE_INTEGER q0, q1; unsigned long long c0, c1; QueryPerformanceCounter(&q0); c0 = ste_cyc();
            do { QueryPerformanceCounter(&q1); } while ((q1.QuadPart - q0.QuadPart) < qf.QuadPart / 40);
            c1 = ste_cyc(); cyc_per_ms = (double)(c1 - c0) / ((double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart);
            if (!(cyc_per_ms > 1000.0)) cyc_per_ms = 3.0e6;
            fprintf(stderr, "[QAUTO] load governor on: budget %.1f ms, thread cycles per ms %.0f\n", budget, cyc_per_ms); fflush(stderr);
            return;
        }
        { double ms = (double)dcyc / cyc_per_ms; if (ms > 100.0) ms = 100.0; game_ms = game_ms == 0.0 ? ms : game_ms * 0.8 + ms * 0.2; }
        load = game_ms > (double)g_ste_render_busy_ms ? game_ms : (double)g_ste_render_busy_ms;
        if (load > budget) { double f = pow(budget / load, 0.30); if (f < 0.97) f = 0.97; cap *= f; over = 1; }   /* at most 3% per frame */
        if (load > 0.9 * budget) calm = 0;
    }
    if (fill > QV_FILL_HI && g_ste_pb_snap_on <= 0) { double f = pow(QV_FILL_AIM / fill, 0.6); if (f < 0.97) f = 0.97; cap *= f; over = 1; }   /* only without the private frame copies */   /* pushbuffer guard: always while the query is widened; at most 3% per frame */
    if (fill > 0.68 && g_ste_pb_snap_on <= 0) calm = 0;   /* hold between 68% and 80%: no creep up, no push down */
    if (cap < 1.0) cap = 1.0;
    if (!over && calm) { cap *= 1.01; }   /* the targets above are fast; the applied value below follows them slowly */
    if (cap > mx) cap = mx;
    {   /* c326: slow ramp. `cap` is the target kept in g_ste_qv_target; the applied value g_ste_qv_auto_cap moves toward it by at most g_ste_qv_ramp percent per second, so the distance never jumps (no visible pop-in) */
        static double applied = 0.0, last_logged = 0.0; static unsigned steps; static double tgt = 0.0; double step, rate = (double)g_ste_qv_ramp / 100.0 / 60.0;
        if (applied == 0.0 || mx < applied - 1e-6 && g_ste_qv_ramp <= 0.0f) applied = cap; if (applied == 0.0) applied = cap;
        tgt = cap; if (applied > mx) applied = mx;
        if (g_ste_qv_ramp <= 0.0f) applied = tgt; else { step = applied * rate; if (tgt > applied + step) applied += step; else if (tgt < applied - step) applied -= step; else applied = tgt; }
        if (fabs(applied - last_logged) >= 0.1 || last_logged == 0.0) { steps++; fprintf(stderr, "[QGOV] frame=%u scale %.2f -> %.2f (target %.2f, %s) steps=%u\n", g_frame, last_logged, applied, tgt, over ? "load/fill above budget" : (calm ? "headroom" : "hold"), steps); fflush(stderr); last_logged = applied; }
        cap = applied;
    }
    g_ste_qv_auto_cap = (float)cap;
    { LARGE_INTEGER n; QueryPerformanceCounter(&n); if ((double)(n.QuadPart - (long long)last_log) / (double)qf.QuadPart >= 5.0) { last_log = (double)n.QuadPart; if (cap < mx - 0.01) { fprintf(stderr, "[QGUARD] cap=%.2f (max %.2f) pushbuffer fill=%.0f%% game_cpu=%.2f ms render_busy=%.2f ms\n", cap, mx, fill * 100.0, game_ms, (double)g_ste_render_busy_ms); fflush(stderr); } } }
}
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
    { static int ps_started; if (!ps_started) { ps_started = 1; ste_ps_register(0); if (g_ps_on > 0) CloseHandle(CreateThread(NULL, 0, ste_ps_thread, NULL, 0, NULL)); } }   /* c308 */
    { static unsigned long long gc0; unsigned long long gc = ste_cyc(); if (g_ste_qv_guard_on) ste_qv_governor(gc0 ? gc - gc0 : 0ull); gc0 = gc; }   /* c317/c318: query_scale pushbuffer guard + optional load governor */
    QueryPerformanceCounter(&q0); c0 = ste_cyc();
    if (cyc_per_ms == 0.0 && (q0.QuadPart - cps_q0.QuadPart) > freq.QuadPart) cyc_per_ms = (double)(c0 - cps_c0) / ((double)(q0.QuadPart - cps_q0.QuadPart) * 1000.0 / (double)freq.QuadPart);
    if (last_enter.QuadPart && cyc_per_ms > 0.0) {
        sum_wall += (double)(q0.QuadPart - last_enter.QuadPart) * 1000.0 / (double)freq.QuadPart;
        sum_cpu += (double)(c0 - last_cyc) / cyc_per_ms;
        nfr++;
    }
    ste_ft_stats();
    /* (c319: the title update moved to the render thread, ste_rft; a SendMessage from this thread locked the game to 2 vblanks per frame) */
    ste_prof_start();
    ste_snap_poll();
    ste_watch_poll();
    ste_cam_probe();
    { extern void ste_fe_camera_tick(void); ste_fe_camera_tick(); }   /* c168 */
    ste_pb_frame_hook_impl();
    { LARGE_INTEGER tp0, tp1; QueryPerformanceCounter(&tp0);
    d3d8_video_pace();   /* c76: ste_video.ini fps_cap (QPC pacing after the frame's work) */
    QueryPerformanceCounter(&tp1); g_pg_pace += tp1.QuadPart - tp0.QuadPart; }
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
        {   /* c308: game-thread split: wall per frame, time waiting for the render thread (throttle), time in the fps pacer, remainder = game thread work */
            static long long thr0, pace0; extern float g_qv_scale_cur; extern volatile uint32_t g_ste_opt_off;
            double wf = sum_wall / nfr, tf = (double)(g_pg_thr - thr0) * 1000.0 / (double)freq.QuadPart / nfr, pf = (double)(g_pg_pace - pace0) * 1000.0 / (double)freq.QuadPart / nfr;
            fprintf(ste_statf(), "[PERFG] n=%u scale=%.2f opt_off=%x wall=%.2f throttle_wait=%.2f pacer=%.2f game_work=%.2f ms/frame\n", nfr, (double)g_qv_scale_cur, (unsigned)g_ste_opt_off, wf, tf, pf, wf - tf - pf);
            thr0 = g_pg_thr; pace0 = g_pg_pace;
        }
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