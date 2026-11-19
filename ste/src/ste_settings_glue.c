/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ste_settings_glue.c (c378): the game side of the launcher's in-game overlay (ste_overlay.dll, plain C interface ste_overlay_c.h) and the two settings that need no overlay.
 *
 * The D3D scan-out path calls ste_overlay_frame(device, rtv, w, h) once per presented frame, on the render thread, right before Present (after the game frame and any upscale).
 *   - first frame: [audio] master_volume and [window] hide_cursor are read from the settings ini (STE_VIDEO_INI); the overlay DLL is loaded from the folder of the exe
 *     (ste_overlay.dll with ui\, assets\, config\ next to it, shipped by the launcher package). A missing DLL just means no overlay (logged once). STE_OVERLAY=0 turns it off.
 *   - requests from the input module (F1 key, Guide button, Back+Start held: ste_overlay_poll) open and close it; while it is open the input module hands the game an empty
 *     pad (ste_overlay_visible), the game thread is held at its frame hook (ste_overlay_wait_if_paused) and pad / keyboard / mouse input goes to the overlay.
 *   - "Return to launcher" exits with code 20, "Quit game" with 0. Settings the overlay changes arrive in apply_setting(): master_volume and hide_cursor apply at once.
 * The game exe itself stays plain C (only VCRUNTIME140.dll); the C++ runtime the DLL needs ships with the launcher package next to it. */
#include <windows.h>
#define COBJMACROS
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STE_OVERLAY_WANT_LOADER
#include "ste_overlay_c.h"
extern void recomp_exit_hook_run(const char *why);
extern void ste_cursor_force_visible(int on);

extern void ste_audio_set_master_volume(int percent);
extern int ste_overlay_poll(int *why, int *port);
extern int ste_sdl_slot_open(int slot);
extern const char *ste_sdl_slot_guid(int slot);
extern int ste_sdl_raw_buttons(int slot, unsigned *mask, int *lx, int *ly);
extern const char *ste_sdl_poll_source(void);
extern void ste_sdl_keys_clear(void);
extern void ste_sdl_update(void);   /* c419: refresh the pad state ourselves while the overlay is open (the game thread, which normally polls, is held) */
extern void ste_sdl_rumble(int slot, int port, unsigned lo, unsigned hi);

/* ---------------- cursor ---------------- */
static int g_cursor_mode = 0;   /* 0 auto, 1 always hidden over the window, 2 never */
static int g_cursor_hidden = 0;
static HCURSOR g_arrow;
static POINT g_last_pt;
static ULONGLONG g_moved;
static HWND g_wnd;

static BOOL CALLBACK find_wnd(HWND h, LPARAM lp)
{
    DWORD pid = 0; RECT r;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == NULL) {
        GetClientRect(h, &r);
        if (r.right > 100) { *(HWND *)lp = h; return FALSE; }
    }
    return TRUE;
}
static void find_window(void)
{
    static ULONGLONG chk; ULONGLONG t = GetTickCount64();
    if (!g_wnd || t - chk > 2000) { HWND h = NULL; chk = t; EnumWindows(find_wnd, (LPARAM)&h); if (h) g_wnd = h; }
}
static void cursor_tick(int overlay_open)
{
    ULONGLONG t = GetTickCount64(); POINT pt; int want_hide;
    if (g_cursor_mode == 2 && !g_cursor_hidden) return;
    find_window();
    if (!g_wnd) return;
    if (!g_arrow) g_arrow = LoadCursorA(NULL, IDC_ARROW);
    GetCursorPos(&pt);
    if (pt.x != g_last_pt.x || pt.y != g_last_pt.y) { g_last_pt = pt; g_moved = t; }
    want_hide = !overlay_open && ((g_cursor_mode == 1) || (g_cursor_mode == 0 && t - g_moved > 3000));
    if (want_hide != g_cursor_hidden) {
        POINT p2;
        g_cursor_hidden = want_hide;
        SetClassLongPtrA(g_wnd, GCLP_HCURSOR, (LONG_PTR)(want_hide ? NULL : g_arrow));
        GetCursorPos(&p2); SetCursorPos(p2.x, p2.y);   /* makes Windows send WM_SETCURSOR again */
    }
}
static void read_ini_defaults(void)
{
    const char *ini = getenv("STE_VIDEO_INI"); char b[64] = ""; int v;
    if (!ini || !*ini) return;
    GetPrivateProfileStringA("window", "hide_cursor", "auto", b, sizeof b, ini);
    g_cursor_mode = !_stricmp(b, "always") ? 1 : !_stricmp(b, "never") ? 2 : 0;
    v = (int)GetPrivateProfileIntA("audio", "master_volume", 100, ini);
    if (v < 0) v = 0; if (v > 100) v = 100;
    ste_audio_set_master_volume(v);
    fprintf(stderr, "[SETTINGS] [audio] master_volume=%d  [window] hide_cursor=%s\n", v, g_cursor_mode == 1 ? "always" : g_cursor_mode == 2 ? "never" : "auto");
    fflush(stderr);
}

/* ---------------- overlay ---------------- */
static ste_overlay_api g_api;
static ste_overlay_handle *g_ov;
static volatile LONG g_visible;
static int g_failed;
static int g_w, g_h;
static ULONGLONG g_last;
static volatile LONG g_keyq[64]; static volatile LONG g_kq_w; static LONG g_kq_r;   /* VK presses from the window procedure, drained on the render thread */

void ste_overlay_key(int vk) { if (g_visible && vk > 0) { LONG w = g_kq_w; g_keyq[w % 64] = vk; g_kq_w = w + 1; } }
int ste_overlay_visible(void) { return g_visible ? 1 : 0; }
/* 0.1.5a: the window is pumped by the game thread itself (dev_Swap / dev_Present), so holding that thread without pumping froze the window: no close, no Alt+F4, no
 * cursor updates ("not responding"). The wait therefore keeps pumping messages; WM_QUIT ends the process like in the device's own pump. */
void ste_overlay_wait_if_paused(void)
{
    while (g_visible) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { recomp_exit_hook_run("window"); ExitProcess(0); }
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        Sleep(8);
    }
}

static int ends_with(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcmp(s + a - b, suf); }

static int cb_apply(void *u, const char *key, const char *value)
{
    (void)u;
    fprintf(stderr, "[OVERLAY] setting %s = '%s'\n", key, value); fflush(stderr);
    if (ends_with(key, "master_volume")) { int v = value[0] ? atoi(value) : 100; if (v < 0) v = 0; if (v > 100) v = 100; ste_audio_set_master_volume(v); return STE_APPLIED_NOW; }
    if (ends_with(key, "hide_cursor")) { g_cursor_mode = !strcmp(value, "always") ? 1 : !strcmp(value, "never") ? 2 : 0; return STE_APPLIED_NOW; }
    if (!strcmp(key, "input.mapping")) return STE_APPLIED_NOW;           /* the next pad poll re-reads nothing yet; used at the next start */
    if (!strcmp(key, "input.button_prompts")) return STE_APPLIED_NEXT_MATCH;
    if (!strncmp(key, "enhance.cheat_", 14)) {   /* 0.1.7: the in-match cheats switch on and off at once */
        extern int ste_cheat_live(const char *name, int on);
        int on = value[0] && strcmp(value, "0") && strcmp(value, "off") && strcmp(value, "false") && strcmp(value, "no");
        if (ste_cheat_live(key + 8, on)) return STE_APPLIED_NOW;
    }
    if (!strncmp(key, "enhance.", 8)) return STE_APPLIED_NEXT_MATCH;
    return STE_APPLIED_AFTER_RESTART;
}
static void cb_show(void *u) { (void)u; if (!g_visible) ste_cursor_force_visible(1); g_visible = 1; fprintf(stderr, "[OVERLAY] opened: game paused, input goes to the overlay\n"); fflush(stderr); }
static void cb_hide(void *u) { (void)u; if (g_visible) ste_cursor_force_visible(0); g_visible = 0; ste_sdl_keys_clear(); fprintf(stderr, "[OVERLAY] closed: game resumed\n"); fflush(stderr); }
static void cb_return(void *u) { (void)u; fprintf(stderr, "[OVERLAY] return to launcher: exit code 20\n"); fflush(stderr); ExitProcess(20); }
static void cb_quit(void *u) { (void)u; fprintf(stderr, "[OVERLAY] quit game\n"); fflush(stderr); ExitProcess(0); }
static int cb_rumble(void *u, unsigned pad_id, float strength, unsigned ms)
{
    unsigned v;
    (void)u;
    if (!pad_id || !ste_sdl_slot_open((int)pad_id - 1)) return 0;
    v = (unsigned)(strength * 60000.f); ste_sdl_rumble((int)pad_id - 1, 0, v, v); Sleep(ms > 1000 ? 1000 : ms); ste_sdl_rumble((int)pad_id - 1, 0, 0, 0);
    return 1;
}

static void exe_dir(char *out, size_t n)
{
    char *p; DWORD k = GetModuleFileNameA(NULL, out, (DWORD)n);
    if (!k || k >= n) { strcpy(out, "."); return; }
    p = strrchr(out, '\\'); if (p) *p = 0; else strcpy(out, ".");
}
static void set_pads(void)
{
    ste_overlay_pad pads[8]; char names[8][40]; int n = 0, s;
    for (s = 0; s < 8; ++s) if (ste_sdl_slot_open(s)) {
        _snprintf(names[n], sizeof names[n], "Controller %d", s + 1); names[n][39] = 0;
        pads[n].guid = ste_sdl_slot_guid(s); pads[n].name = names[n]; pads[n].kind = STE_DEVICE_XBOX; pads[n].id = (unsigned)s + 1; n++;
    }
    g_api.set_pads(g_ov, pads, n);
}
static void make(void *device)
{
    char dir[MAX_PATH], dll[MAX_PATH], data[MAX_PATH], err[256]; wchar_t wdll[MAX_PATH]; ste_overlay_init info; const char *e, *m;
    e = getenv("STE_OVERLAY"); if (e && e[0] == '0') { g_failed = 1; return; }
    exe_dir(dir, sizeof dir);
    _snprintf(dll, sizeof dll, "%s\\ste_overlay.dll", dir); dll[MAX_PATH - 1] = 0;
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) { fprintf(stderr, "[OVERLAY] no ste_overlay.dll next to the game: no in-game overlay\n"); fflush(stderr); g_failed = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, dll, -1, wdll, MAX_PATH);
    if (!ste_overlay_load(&g_api, wdll, err, sizeof err)) { fprintf(stderr, "[OVERLAY] %s\n", err); fflush(stderr); g_failed = 1; return; }
    m = getenv("STE_INPUT_MAP");
    if (getenv("STE_OVERLAY_DATA")) { strncpy(data, getenv("STE_OVERLAY_DATA"), sizeof data - 1); data[sizeof data - 1] = 0; }
    else if (m && strrchr(m, '\\')) { strncpy(data, m, sizeof data - 1); data[sizeof data - 1] = 0; *strrchr(data, '\\') = 0; }
    else { _snprintf(data, sizeof data, "%s\\data", dir); data[MAX_PATH - 1] = 0; }
    CreateDirectoryA(data, NULL);
    memset(&info, 0, sizeof info);
    info.struct_size = sizeof info; info.resource_dir = getenv("STE_OVERLAY_RES") ? getenv("STE_OVERLAY_RES") : dir; info.data_dir = data; info.device = device; info.width = g_w; info.height = g_h; info.content_scale = 1.0f;
    info.callbacks.apply_setting = cb_apply; info.callbacks.on_show = cb_show; info.callbacks.on_hide = cb_hide; info.callbacks.return_to_launcher = cb_return; info.callbacks.quit_game = cb_quit; info.callbacks.rumble = cb_rumble;
    g_ov = g_api.create(&info, err, sizeof err);
    if (!g_ov) { fprintf(stderr, "[OVERLAY] could not start: %s (overlay disabled)\n", err); fflush(stderr); g_failed = 1; return; }
    set_pads();
    fprintf(stderr, "[OVERLAY] ready (%s, resources %s, data %s)\n", g_api.build_id ? g_api.build_id() : "?", info.resource_dir, data); fflush(stderr);
}

/* edge + key-repeat for a held pad direction/button: 400 ms, then every 120 ms */
typedef struct { ULONGLONG next; int down; } Rep;
static int step(Rep *r, int now, ULONGLONG t)
{
    if (!now) { r->down = 0; return 0; }
    if (!r->down) { r->down = 1; r->next = t + 400; return 1; }
    if (t >= r->next) { r->next = t + 120; return 1; }
    return 0;
}
static void feed_nav(int nav) { ste_overlay_input in; memset(&in, 0, sizeof in); in.type = STE_INPUT_NAV; in.nav = nav; g_api.feed(g_ov, &in); }
static void feed_release(int nav) { ste_overlay_input in; memset(&in, 0, sizeof in); in.type = STE_INPUT_NAV_RELEASE; in.nav = nav; g_api.feed(g_ov, &in); }   /* c420: after the first release the overlay repeats held directions itself */
static int g_dev_cur = -1;   /* c423: device kind last told to the overlay (hint bar pictures); -1 = not set since the menu opened */
static void set_dev(int kind) { if (kind != g_dev_cur) { g_api.set_input_device(g_ov, kind); g_dev_cur = kind; } }
static void feed_source(const char *s) { ste_overlay_input in; memset(&in, 0, sizeof in); in.type = STE_INPUT_SOURCE; in.source = s; g_api.feed(g_ov, &in); }
static const char *key_source(unsigned vk, char *b, size_t n)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { _snprintf(b, n, "key:%c", (char)vk); return b; }
    switch (vk) {
        case VK_LEFT: return "key:Left"; case VK_RIGHT: return "key:Right"; case VK_UP: return "key:Up"; case VK_DOWN: return "key:Down";
        case VK_RETURN: return "key:Return"; case VK_SPACE: return "key:Space"; case VK_TAB: return "key:Tab"; case VK_BACK: return "key:Backspace";
        case VK_SHIFT: case VK_LSHIFT: return "key:Left Shift"; case VK_RSHIFT: return "key:Right Shift"; case VK_CONTROL: case VK_LCONTROL: return "key:Left Ctrl"; case VK_RCONTROL: return "key:Right Ctrl";
        case VK_OEM_COMMA: return "key:,"; case VK_OEM_PERIOD: return "key:."; default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) { _snprintf(b, n, "key:F%u", vk - VK_F1 + 1); return b; }
    return NULL;
}
static void pump_input(ULONGLONG t)
{
    static int pu, pd, pl, pr, pa, pb, plb, prb, pst, lmb, mx = -1, my = -1;
    int U = 0, D = 0, L = 0, R = 0, A = 0, B = 0, LB = 0, RB = 0, ST = 0, s; char nb[16];
    for (;;) {   /* keyboard */
        LONG w = g_kq_w; unsigned vk;
        if (g_kq_r == w) break;
        vk = (unsigned)g_keyq[g_kq_r % 64]; g_kq_r++;
        if (g_api.listening(g_ov)) { const char *n = vk == VK_ESCAPE ? "cancel" : key_source(vk, nb, sizeof nb); if (n) feed_source(n); continue; }
        set_dev(STE_DEVICE_KEYBOARD);
        switch (vk) {
            case VK_UP: feed_nav(STE_NAV_UP); feed_release(STE_NAV_UP); break; case VK_DOWN: feed_nav(STE_NAV_DOWN); feed_release(STE_NAV_DOWN); break; case VK_LEFT: feed_nav(STE_NAV_LEFT); feed_release(STE_NAV_LEFT); break; case VK_RIGHT: feed_nav(STE_NAV_RIGHT); feed_release(STE_NAV_RIGHT); break;
            case VK_PRIOR: feed_nav(STE_NAV_PREV_TAB); break; case VK_NEXT: feed_nav(STE_NAV_NEXT_TAB); break;
            case VK_RETURN: case VK_SPACE: feed_nav(STE_NAV_CONFIRM); break;
            case VK_ESCAPE: case VK_BACK: case VK_F1: feed_nav(STE_NAV_BACK); break;
            default: break;
        }
    }
    ste_sdl_update();
    for (s = 0; s < 8; ++s) {   /* pads */
        unsigned m = 0; int lx = 0, ly = 0;
        if (!ste_sdl_raw_buttons(s, &m, &lx, &ly)) continue;
        U |= (m & 1u) || ly < -16000; D |= (m & 2u) || ly > 16000; L |= (m & 4u) || lx < -16000; R |= (m & 8u) || lx > 16000; A |= (m & 16u) != 0; B |= (m & 32u) != 0; ST |= (m & 64u) != 0; LB |= (m & 256u) != 0; RB |= (m & 512u) != 0;
        if (g_api.listening(g_ov)) { const char *n = ste_sdl_poll_source(); if (n) feed_source(n); }
    }
    if (!g_api.listening(g_ov)) {
        /* c420: first press only + release on let-go (the overlay repeats held directions itself); A confirm, B back, LB/RB tabs, Start closes */
        if ((U | D | L | R | A | B | LB | RB | ST) && !(pu | pd | pl | pr | pa | pb | plb | prb | pst)) set_dev(STE_DEVICE_XBOX);   /* c423: the last device used decides the hint bar */
        if (U != pu) { if (U) feed_nav(STE_NAV_UP); else feed_release(STE_NAV_UP); pu = U; }
        if (D != pd) { if (D) feed_nav(STE_NAV_DOWN); else feed_release(STE_NAV_DOWN); pd = D; }
        if (L != pl) { if (L) feed_nav(STE_NAV_LEFT); else feed_release(STE_NAV_LEFT); pl = L; }
        if (R != pr) { if (R) feed_nav(STE_NAV_RIGHT); else feed_release(STE_NAV_RIGHT); pr = R; }
        if (A != pa) { if (A) feed_nav(STE_NAV_CONFIRM); pa = A; }
        if (B != pb) { if (B) feed_nav(STE_NAV_BACK); pb = B; }
        if (LB != plb) { if (LB) feed_nav(STE_NAV_PREV_TAB); plb = LB; }
        if (RB != prb) { if (RB) feed_nav(STE_NAV_NEXT_TAB); prb = RB; }
        if (ST != pst) { if (ST) g_api.hide(g_ov); pst = ST; }
    }
    find_window();
    if (g_wnd && GetForegroundWindow() == g_wnd) {   /* mouse: position in back-buffer pixels, left button */
        POINT p; RECT cr;
        if (GetCursorPos(&p) && ScreenToClient(g_wnd, &p) && GetClientRect(g_wnd, &cr) && cr.right > 0 && cr.bottom > 0) {
            int x = (int)((long long)p.x * g_w / cr.right), y = (int)((long long)p.y * g_h / cr.bottom), down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (x != mx || y != my) { ste_overlay_input in; memset(&in, 0, sizeof in); in.type = STE_INPUT_MOUSE_MOVE; in.x = (float)x; in.y = (float)y; g_api.feed(g_ov, &in); mx = x; my = y; }
            if (down != lmb) { ste_overlay_input in; memset(&in, 0, sizeof in); in.type = STE_INPUT_MOUSE_BUTTON; in.x = (float)x; in.y = (float)y; in.button = 0; in.down = down; g_api.feed(g_ov, &in); lmb = down; }
        }
    }
}


/* STE_OVERLAY_SHOT=<dir>: while the overlay is open, every 700 ms the presented picture (back buffer incl. the overlay) is written as ov_<n>.bmp (test aid, no game data besides what is drawn). */
static void shot_tick(void *dev, void *rtv)
{
    static ULONGLONG last; static int n; const char *dir = getenv("STE_OVERLAY_SHOT"); ULONGLONG t = GetTickCount64();
    ID3D11Resource *res = NULL; ID3D11Texture2D *tex = NULL, *stg = NULL; D3D11_TEXTURE2D_DESC d; ID3D11DeviceContext *ctx = NULL; D3D11_MAPPED_SUBRESOURCE mp;
    { static int once; if (!once++) { fprintf(stderr, "[OVERLAY] shot_tick first call, dir=%s\n", dir ? dir : "(unset)"); fflush(stderr); } }
    if (!dir || !*dir || n >= 40 || t - last < 700) return;
    last = t;
    ID3D11View_GetResource((ID3D11View *)rtv, &res);
    if (!res) { fprintf(stderr, "[OVERLAY] shot: no resource\n"); fflush(stderr); return; }
    if (SUCCEEDED(ID3D11Resource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) {
        ID3D11Texture2D_GetDesc(tex, &d); d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0; d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
        ID3D11Device_GetImmediateContext((ID3D11Device *)dev, &ctx);
        if (SUCCEEDED(ID3D11Device_CreateTexture2D((ID3D11Device *)dev, &d, NULL, &stg))) {
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stg, res);
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &mp))) {
                char fn[MAX_PATH]; FILE *f; BITMAPFILEHEADER fh; BITMAPINFOHEADER ih; UINT y;
                memset(&fh, 0, sizeof fh); memset(&ih, 0, sizeof ih);
                _snprintf(fn, sizeof fn, "%s%sov_%03d.bmp", dir, "\\", n++); fn[MAX_PATH - 1] = 0;
                f = fopen(fn, "wb");
                if (!f) { fprintf(stderr, "[OVERLAY] shot: cannot open %s\n", fn); fflush(stderr); }
                if (f) {
                    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
                    ih.biSize = sizeof ih; ih.biWidth = (LONG)d.Width; ih.biHeight = -(LONG)d.Height; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
                    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
                    for (y = 0; y < d.Height; ++y) fwrite((const unsigned char *)mp.pData + (size_t)y * mp.RowPitch, 4, d.Width, f);
                    fclose(f);
                }
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg, 0);
            }
            ID3D11Texture2D_Release(stg);
        }
        if (ctx) ID3D11DeviceContext_Release(ctx);
        ID3D11Texture2D_Release(tex);
    }
    ID3D11Resource_Release(res);
}

void ste_overlay_frame(void *device, void *rtv, int w, int h)
{
    static int ini_done, tried; ULONGLONG t; int why = 0, port = 0;
    if (!ini_done) { ini_done = 1; read_ini_defaults(); }
    cursor_tick(g_visible != 0);
    if (g_failed || !device || !rtv) return;
    if (!g_ov) {
        if (tried++) return;
        g_w = w; g_h = h; make(device);
        if (!g_ov) return;
        g_last = GetTickCount64();
    }
    if (w != g_w || h != g_h) { g_w = w; g_h = h; g_api.resize(g_ov, w, h, 1.0f); }
    t = GetTickCount64();
    while (ste_overlay_poll(&why, &port)) { if (g_api.visible(g_ov)) g_api.hide(g_ov); else { set_pads(); g_dev_cur = -1; g_api.show(g_ov); } }
    if (g_api.visible(g_ov)) pump_input(t);
    {   /* test aid: STE_OVERLAY_TEST_NAV="down,down,confirm,back,up,left,right" feeds one navigation step every 900 ms while the overlay is open (scripted pads never reach the overlay) */
        static int idx; static ULONGLONG nxt; const char *l = getenv("STE_OVERLAY_TEST_NAV");
        if (l && *l && g_api.visible(g_ov) && t >= nxt) {
            const char *q = l; int k = 0; char w[16];
            while (*q && k < idx) { const char *c = strchr(q, ','); if (!c) { q = NULL; break; } q = c + 1; k++; }
            if (q && *q) { size_t n = strcspn(q, ","); if (n > 0 && n < sizeof w) { int nav = -1; memcpy(w, q, n); w[n] = 0;
                if (!strcmp(w, "up")) nav = STE_NAV_UP; else if (!strcmp(w, "down")) nav = STE_NAV_DOWN; else if (!strcmp(w, "left")) nav = STE_NAV_LEFT; else if (!strcmp(w, "right")) nav = STE_NAV_RIGHT;
                else if (!strcmp(w, "confirm")) nav = STE_NAV_CONFIRM; else if (!strcmp(w, "back")) nav = STE_NAV_BACK;
                if (nav >= 0) feed_nav(nav); fprintf(stderr, "[OVERLAY] test nav %s" "\n", w); fflush(stderr); } idx++; nxt = t + 900; }
        }
    }
    {   /* c420 test aid: STE_OVERLAY_TEST_VPAD="down,down,rb,a,lb,b,start" presses a virtual SDL pad (700 ms held 150 ms) while the overlay is open: a controller-only pass through the real SDL path */
        static int idx; static ULONGLONG nxt, rel; static int held = -1; const char *l = getenv("STE_OVERLAY_TEST_VPAD");
        extern void ste_sdl_testpad(int btn, int down);
        if (l && *l && g_api.visible(g_ov)) {
            if (held >= 0 && t >= rel) { ste_sdl_testpad(held, 0); held = -1; }
            if (held < 0 && t >= nxt) {
                const char *q = l; int k = 0; char w[16];
                while (*q && k < idx) { const char *c = strchr(q, ','); if (!c) { q = NULL; break; } q = c + 1; k++; }
                if (q && *q) { size_t n = strcspn(q, ","); if (n > 0 && n < sizeof w) { int b = -1; memcpy(w, q, n); w[n] = 0;
                    if (!strcmp(w, "a")) b = 0; else if (!strcmp(w, "b")) b = 1; else if (!strcmp(w, "back")) b = 4; else if (!strcmp(w, "start")) b = 6; else if (!strcmp(w, "lb")) b = 9; else if (!strcmp(w, "rb")) b = 10;
                    else if (!strcmp(w, "up")) b = 11; else if (!strcmp(w, "down")) b = 12; else if (!strcmp(w, "left")) b = 13; else if (!strcmp(w, "right")) b = 14;
                    if (b >= 0) { ste_sdl_testpad(b, 1); held = b; rel = t + 150; } fprintf(stderr, "[OVERLAY] test pad %s" "\n", w); fflush(stderr); } idx++; nxt = t + 700; }
            }
        }
    }
    g_api.update(g_ov, (double)(t - g_last) / 1000.0); g_last = t;
    g_api.render(g_ov, rtv);
    if (g_visible) shot_tick(device, rtv);
}
