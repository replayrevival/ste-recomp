/* SPDX-License-Identifier: GPL-2.0-or-later
 * ste_photo.c - photo mode (0.3.1): a paused free camera with HUD hide, FOV / roll and full-resolution PNG screenshots.
 *
 * What it does and how (docs\FEATURE_PHOTO_MODE.md, docs\formats\photo_mode.md):
 *  - The fight camera manager (guest sub_001E9E60) hands the camera its view matrix p (12 floats, D3D layout) through the call at
 *    0x001EA1D8. A generator hook (before) calls ste_photo_update(p, ...) there; while photo mode is active p is replaced by the free
 *    camera, so the camera object, its frustum and every camera-relative query (draw distance, query volume) follow it.
 *  - The world update sub_00084800(dt) has a generator `return` hook: while photo mode is active it returns at once, so the simulation
 *    (animation, AI, timers, physics) is frozen EXACTLY while the frame keeps rendering. Nothing of the game's state is written;
 *    on exit the next update continues from the same state.
 *  - The game gets a released pad (recomp_manual.c, same place as the overlay mask); the free camera reads the raw pad / keyboard / mouse.
 *  - HUD hide: g_ste_hide_2d makes the Kelvin layer skip the canonical-640x480-mapping draws (HUD, banners, flash and fade quads).
 *  - FOV: the camera object's projection matrix (cam+0x60, built by sub_001846B0) has its x/y focal terms scaled and restored on exit.
 * Portable C apart from the QueryPerformanceCounter and path helpers below (kept small and in this file). No game data in here. */
#include <windows.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xinput_xbox.h"
#include "keyboard.h"

extern volatile int g_ste_hide_2d;
extern void d3d8_capture_backbuffer_bmp_public(const char *path);

typedef struct { float x, y, z; } V3;
static V3 v_add(V3 a, V3 b) { V3 r = { a.x + b.x, a.y + b.y, a.z + b.z }; return r; }
static V3 v_mul(V3 a, float s) { V3 r = { a.x * s, a.y * s, a.z * s }; return r; }
static float v_dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 v_cross(V3 a, V3 b) { V3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; return r; }
static V3 v_rot(V3 v, V3 k, float ang) { float c = cosf(ang), s = sinf(ang); return v_add(v_add(v_mul(v, c), v_mul(v_cross(k, v), s)), v_mul(k, v_dot(k, v) * (1.0f - c))); }

#define PI_F 3.14159265f
static struct {
    int init, enabled, active, hud_hidden, shot_req, shot_delay;
    volatile int req;                       /* 1 = open requested, 2 = close requested (overlay entry, hotkey) */
    int vk_toggle, vk_shot, vk_hud;
    float speed, range, fov_step;           /* units per second, flying radius, per-second FOV change (log scale) */
    V3 R0, U0, F0, W, eye0, eye;
    float yaw, pitch, roll, zoom;           /* zoom: focal scale (1 = the game's FOV); > 1 narrower */
    float m00, m11; void *proj_ptr; int proj_saved;
    XBOX_INPUT_STATE pad[4]; int pad_seen[4];
    LARGE_INTEGER t_prev, qpf; int have_t;
    unsigned prev_keys, prev_btn;
    float mdx, mdy; int mdrag, mx, my;
    unsigned n; char shot_path[MAX_PATH]; unsigned shots;
} P;

/* test script (STE_PHOTO_SCRIPT=<file>), counted in world-camera updates n of the current process: "open n", "close n", "shot n", "hud n 0|1", "pose n dx dy dz yaw_deg pitch_deg"
 * (offsets from the pose photo mode was opened at, world axes), "zoom n k", "roll n deg". For proof runs only. */
typedef struct { unsigned n; int cmd; float a[5]; } SCmd;
static SCmd g_sc[128]; static int g_nsc; static unsigned g_wn;
static void script_load(void)
{
    const char *e = getenv("STE_PHOTO_SCRIPT"); FILE *f; char line[256];
    if (!e || !*e || !(f = fopen(e, "r"))) return;
    while (fgets(line, sizeof line, f) && g_nsc < 128) {
        unsigned n; float a[5] = { 0, 0, 0, 0, 0 }; SCmd *c = &g_sc[g_nsc];
        if (sscanf(line, "open %u", &n) == 1) c->cmd = 1;
        else if (sscanf(line, "close %u", &n) == 1) c->cmd = 2;
        else if (sscanf(line, "shot %u", &n) == 1) c->cmd = 3;
        else if (sscanf(line, "hud %u %f", &n, &a[0]) >= 1) c->cmd = 4;
        else if (sscanf(line, "pose %u %f %f %f %f %f", &n, &a[0], &a[1], &a[2], &a[3], &a[4]) >= 1) c->cmd = 5;
        else if (sscanf(line, "zoom %u %f", &n, &a[0]) >= 1) c->cmd = 6;
        else if (sscanf(line, "roll %u %f", &n, &a[0]) >= 1) c->cmd = 7;
        else if (sscanf(line, "scan %u", &n) == 1) c->cmd = 8;
        else continue;
        c->n = n; memcpy(c->a, a, sizeof a); g_nsc++;
    }
    fclose(f); fprintf(stderr, "[PHOTO] test script %s: %d commands\n", e, g_nsc); fflush(stderr);
}
/* The match clock is NOT advanced by World_Update / Game::Update: it is a presented-frame clock (guest 0x00442708 = frame count, 0x0044270C = frames / 60 s, what the script's
 * getGameTime() returns and the HUD clock shows), so it keeps running while the frames keep rendering. While photo mode is open both words are written back to the value
 * they had when it opened, once per camera update; the game continues from there on exit. */
extern ptrdiff_t g_xbox_mem_offset;
#define GW(a) (*(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + (uintptr_t)(a)))
extern volatile unsigned g_ste_hide_2d_cnt;
static uint32_t g_pin_frame, g_pin_time; static int g_post = -1;
static void clk_dump(const char *why)
{
    uint32_t a; char b[512]; int o = 0; float f;
    for (a = 0x00425B90u; a <= 0x00425BB8u && o < 440; a += 4u) { uint32_t v = GW(a); memcpy(&f, &v, 4); o += _snprintf(b + o, sizeof b - o, " %X=%08X(%.3f)", a & 0xFFFu, v, f); }
    { uint32_t v = GW(0x0044270Cu); memcpy(&f, &v, 4); fprintf(stderr, "[PHOTOCLK] %s: t=%.4f frame=%u game@425B9x:%s\n", why, f, GW(0x00442708u), b); fflush(stderr); }
}
/* debug (STE_PHOTO_SCAN=<seconds>): which guest words keep changing while the simulation is frozen? Snapshot at open, report after <seconds>: floats that moved by +-elapsed,
 * integers that moved by 1000*elapsed (ms) or 60*elapsed (frames). Research only: finds counters/clocks that are not stopped by the freeze hooks. */
static uint32_t *g_scan_snap; static unsigned char *g_scan_ok; static LARGE_INTEGER g_scan_t0; static float g_scan_secs;
static int page_ok(uintptr_t a) { MEMORY_BASIC_INFORMATION mi; if (!VirtualQuery((void *)a, &mi, sizeof mi)) return 0; return mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)); }
#define SCAN_WORDS (0x04000000u / 4u)
static void scan_start(void)
{
    const char *e = getenv("STE_PHOTO_SCAN"); if (!e || atof(e) <= 0) return;
    g_scan_secs = (float)atof(e); if (!g_scan_snap) g_scan_snap = (uint32_t *)malloc(SCAN_WORDS * 4u);
    if (!g_scan_ok) g_scan_ok = (unsigned char *)calloc(SCAN_WORDS / 1024u, 1);
    if (g_scan_snap && g_scan_ok) { uint32_t pg; for (pg = 0; pg < SCAN_WORDS / 1024u; ++pg) { uintptr_t a = (uintptr_t)g_xbox_mem_offset + (uintptr_t)pg * 4096u; g_scan_ok[pg] = (unsigned char)page_ok(a); if (g_scan_ok[pg]) memcpy(g_scan_snap + (size_t)pg * 1024u, (void *)a, 4096u); }
        QueryPerformanceCounter(&g_scan_t0); fprintf(stderr, "[PHOTOSCAN] snapshot taken, report in %.1f s\n", g_scan_secs); fflush(stderr); }
}
static void scan_poll(void)
{
    LARGE_INTEGER n; double el; uint32_t i, hits = 0; const uint32_t *cur;
    if (!g_scan_snap || g_scan_secs <= 0) return;
    QueryPerformanceCounter(&n); el = (double)(n.QuadPart - g_scan_t0.QuadPart) / (double)P.qpf.QuadPart; if (el < g_scan_secs) return;
    cur = (const uint32_t *)((uintptr_t)g_xbox_mem_offset);
    for (i = 0x400000u / 4u; i < SCAN_WORDS && hits < 300; ++i) {
        uint32_t a, b; float fa, fb; int32_t d;
        if ((i & 1023u) == 0 && (!g_scan_ok[i >> 10] || !page_ok((uintptr_t)g_xbox_mem_offset + (uintptr_t)(i >> 10) * 4096u))) { i |= 1023u; continue; }
        a = g_scan_snap[i]; b = cur[i];
        if (a == b) continue;
        memcpy(&fa, &a, 4); memcpy(&fb, &b, 4); d = (int32_t)(b - a);
        if ((fa == fa && fb == fb && fabsf(fa) < 1e6f && fabsf(fb) < 1e6f && fabsf(fa) > 0.5f && fabsf(fa) < 200.0f && (fabs((double)fb - fa - el) < 0.06 * el || fabs((double)fb - fa + el) < 0.06 * el))
            || fabs((double)d - 1000.0 * el) < 60.0 * el || fabs((double)d + 1000.0 * el) < 60.0 * el || fabs((double)d + 60.0 * el) < 3.0 * el || fabs((double)d + el) < 0.06 * el) {
            fprintf(stderr, "[PHOTOSCAN] 0x%08X: %08X -> %08X (float %.4f -> %.4f, int delta %d, after %.2f s)\n", i * 4u, a, b, fa, fb, d, el); hits++;
        }
    }
    fprintf(stderr, "[PHOTOSCAN] done: %u candidates\n", hits); fflush(stderr); g_scan_secs = 0;
}
static void ini_path(char *ini, size_t n)
{
    const char *iv = getenv("STE_ENHANCE_INI"); char *p;
    if (iv && *iv) { strncpy(ini, iv, n - 1); ini[n - 1] = 0; return; }
    GetModuleFileNameA(NULL, ini, (DWORD)n); p = strrchr(ini, '\\'); if (p) strcpy(p + 1, "ste_enhance.ini");
}
static int vk_from(const char *s, int def)
{
    if (!s[0]) return def;
    if (!_stricmp(s, "none")) return 0;
    if ((s[0] == 'F' || s[0] == 'f') && atoi(s + 1) >= 1 && atoi(s + 1) <= 24) return VK_F1 + atoi(s + 1) - 1;
    if (strlen(s) == 1 && isalnum((unsigned char)s[0])) return toupper((unsigned char)s[0]);
    if (!_stricmp(s, "Tab")) return VK_TAB; if (!_stricmp(s, "Space")) return VK_SPACE; if (!_stricmp(s, "Escape")) return VK_ESCAPE;
    return def;
}
static void data_dir(char *d, size_t n)   /* the same <data> folder the overlay uses */
{
    const char *o = getenv("STE_OVERLAY_DATA"), *m = getenv("STE_INPUT_MAP"); char *p;
    if (o && *o) { strncpy(d, o, n - 1); d[n - 1] = 0; return; }
    if (m && strrchr(m, '\\')) { strncpy(d, m, n - 1); d[n - 1] = 0; p = strrchr(d, '\\'); *p = 0; return; }
    GetModuleFileNameA(NULL, d, (DWORD)n); p = strrchr(d, '\\'); if (p) *p = 0; strncat(d, "\\data", n - strlen(d) - 1);
}
static void init(void)
{
    char ini[MAX_PATH], v[32];
    P.init = 1; ini_path(ini, sizeof ini); QueryPerformanceFrequency(&P.qpf);
    GetPrivateProfileStringA("camera", "photo_mode", "on", v, sizeof v, ini);
    P.enabled = !(!_stricmp(v, "off") || !_stricmp(v, "0") || !_stricmp(v, "false") || !_stricmp(v, "no"));
    GetPrivateProfileStringA("camera", "photo_key", "F8", v, sizeof v, ini); P.vk_toggle = vk_from(v, VK_F8);
    GetPrivateProfileStringA("camera", "photo_screenshot_key", "F12", v, sizeof v, ini); P.vk_shot = vk_from(v, VK_F12);
    GetPrivateProfileStringA("camera", "photo_hud_key", "H", v, sizeof v, ini); P.vk_hud = vk_from(v, 'H');
    GetPrivateProfileStringA("camera", "photo_speed", "400", v, sizeof v, ini); P.speed = (float)atof(v); if (P.speed < 10.0f) P.speed = 400.0f;
    GetPrivateProfileStringA("camera", "photo_range", "8000", v, sizeof v, ini); P.range = (float)atof(v); if (P.range < 200.0f) P.range = 8000.0f;
    P.fov_step = 0.8f; script_load();
    fprintf(stderr, "[PHOTO] photo mode %s (key vk=0x%02X, screenshot vk=0x%02X, hud vk=0x%02X, speed %.0f u/s, range %.0f)\n", P.enabled ? "on" : "off", P.vk_toggle, P.vk_shot, P.vk_hud, P.speed, P.range); fflush(stderr);
}

int ste_photo_active(void) { return P.active; }
int ste_photo_enabled(void) { if (!P.init) init(); return P.enabled; }
/* overlay entry / API: 1 = open, 0 = close. Opening needs a running match; it is applied at the next world camera update. */
void ste_photo_request(int on) { if (!P.init) init(); if (on && !P.enabled) { fprintf(stderr, "[PHOTO] request ignored: [camera] photo_mode = off\n"); fflush(stderr); return; } P.req = on ? 1 : 2; }
void ste_photo_feed_pad(int port, const XBOX_INPUT_STATE *st) { if (port >= 0 && port < 4) { P.pad[port] = *st; P.pad_seen[port] = 1; } }
void ste_photo_mouse(int x, int y, int drag)
{
    if (!P.active) { P.mdrag = 0; P.mx = x; P.my = y; return; }
    if (drag && P.mdrag) { P.mdx += (float)(x - P.mx); P.mdy += (float)(y - P.my); }
    P.mdrag = drag; P.mx = x; P.my = y;
}
int ste_photo_hud_hidden(void) { return P.active && P.hud_hidden; }
/* a pending screenshot request: fills path, returns 1 once (called at the end of the frame by ste_pb_hook.c) */
int ste_photo_take_shot(char *path, size_t n)
{
    if (P.shot_req != 1) return 0; if (P.shot_delay > 0) { P.shot_delay--; return 0; } P.shot_req = 0; strncpy(path, P.shot_path, n - 1); path[n - 1] = 0; return 1;
}

static void basis(V3 *R, V3 *U, V3 *F)
{
    V3 r = v_rot(P.R0, P.W, P.yaw), u = v_rot(P.U0, P.W, P.yaw), f = v_rot(P.F0, P.W, P.yaw);
    f = v_rot(f, r, P.pitch); u = v_rot(u, r, P.pitch);
    if (P.roll != 0.0f) { r = v_rot(r, f, P.roll); u = v_rot(u, f, P.roll); }
    *R = r; *U = u; *F = f;
}
static int key(int vk) { return vk > 0 && xbox_FramebufferKeyDown(vk) != 0; }
static void restore_proj(void)
{
    if (P.proj_saved && P.proj_ptr) { float *m = (float *)P.proj_ptr; m[0] = P.m00; m[5] = P.m11; }
    P.proj_saved = 0;
}
static void deactivate(const char *why)
{
    { float rt; uint32_t rtu = GW(0x0044270Cu); memcpy(&rt, &rtu, 4); fprintf(stderr, "[PHOTOCLK] close: frame=%u time=%.4f\n", GW(0x00442708u), rt); fflush(stderr); }
    clk_dump("close"); g_post = 0;
    restore_proj(); P.active = 0; g_ste_hide_2d = 0; P.hud_hidden = 0; P.mdrag = 0;
    fprintf(stderr, "[PHOTO] closed (%s): simulation resumes, HUD on, FOV restored, %u updates\n", why, P.n); fflush(stderr);
}
static void activate(const float *p)
{
    float ax[3][3]; int i, j, wi = 0; float best = -1.0f, ups;
    for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) ax[j][i] = p[i * 3 + j];
    P.R0.x = ax[0][0]; P.R0.y = ax[0][1]; P.R0.z = ax[0][2]; P.U0.x = ax[1][0]; P.U0.y = ax[1][1]; P.U0.z = ax[1][2]; P.F0.x = ax[2][0]; P.F0.y = ax[2][1]; P.F0.z = ax[2][2];
    P.eye0.x = -(p[0] * p[9] + p[1] * p[10] + p[2] * p[11]); P.eye0.y = -(p[3] * p[9] + p[4] * p[10] + p[5] * p[11]); P.eye0.z = -(p[6] * p[9] + p[7] * p[10] + p[8] * p[11]);
    { float u[3]; u[0] = P.U0.x; u[1] = P.U0.y; u[2] = P.U0.z; for (i = 0; i < 3; ++i) if (fabsf(u[i]) > best) { best = fabsf(u[i]); wi = i; } ups = u[wi]; }
    P.W.x = P.W.y = P.W.z = 0.0f; if (wi == 0) P.W.x = ups < 0 ? -1.0f : 1.0f; else if (wi == 1) P.W.y = ups < 0 ? -1.0f : 1.0f; else P.W.z = ups < 0 ? -1.0f : 1.0f;
    clk_dump("open"); scan_start(); g_pin_frame = GW(0x00442708u); g_pin_time = GW(0x0044270Cu);
    P.eye = P.eye0; P.yaw = P.pitch = P.roll = 0.0f; P.zoom = 1.0f; P.hud_hidden = 0; P.mdx = P.mdy = 0; P.prev_keys = ~0u; P.prev_btn = ~0u; P.have_t = 0; P.active = 1; P.n = 0;
    fprintf(stderr, "[PHOTO] open: eye=(%.1f %.1f %.1f) up-axis=%c%c; simulation frozen, game input masked\n", P.eye0.x, P.eye0.y, P.eye0.z, ups < 0 ? '-' : '+', "xyz"[wi]); fflush(stderr);
}
/* One world-camera update. p = the 12 view floats (replaced while active), proj = guest pointer of the camera's projection matrix (cam+0x60, 16 floats) or 0.
 * Returns 1 while photo mode is active. in_match = the game is in a fight scene and this is the world camera. */
int ste_photo_update(float *p, int in_match, void *proj_host_ptr)
{
    LARGE_INTEGER now; float dt = 1.0f / 60.0f; V3 R, U, F; float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0; unsigned btn = 0, keys = 0; int i;
    if (!P.init) init();
    if (!P.enabled) return 0;
    if (!in_match) { if (P.active) deactivate("left the match"); P.req = 0; return 0; }
    g_wn++; scan_poll();
    {   int k; for (k = 0; k < g_nsc; ++k) if (g_sc[k].n == g_wn) {
            const SCmd *c = &g_sc[k];
            if (c->cmd == 1) P.req = 1; else if (c->cmd == 2) P.req = 2;
            else if (c->cmd == 3 && P.active) P.shot_req = 2;
            else if (c->cmd == 4 && P.active) P.hud_hidden = c->a[0] != 0.0f;
            else if (c->cmd == 5 && P.active) { P.eye.x = P.eye0.x + c->a[0]; P.eye.y = P.eye0.y + c->a[1]; P.eye.z = P.eye0.z + c->a[2]; P.yaw = c->a[3] * PI_F / 180.0f; P.pitch = c->a[4] * PI_F / 180.0f; }
            else if (c->cmd == 6 && P.active) P.zoom = c->a[0];
            else if (c->cmd == 7 && P.active) P.roll = c->a[0] * PI_F / 180.0f;
            else if (c->cmd == 8) scan_start();
        } }
    /* hotkey (works also while inactive: the camera update runs every frame in a fight) */
    {   /* optional pad chord Back+Y held 0.6 s opens photo mode (off by default: [camera] photo_pad_chord = on, or STE_PHOTO_PAD_CHORD=1) */
        static int ch = -1; static unsigned held; int i, both = 0;
        if (ch < 0) { char ini[MAX_PATH], v[16]; const char *e = getenv("STE_PHOTO_PAD_CHORD"); ini_path(ini, sizeof ini); GetPrivateProfileStringA("camera", "photo_pad_chord", "off", v, sizeof v, ini); ch = (e && *e && *e != '0') || !_stricmp(v, "on") || !_stricmp(v, "1"); }
        for (i = 0; i < 4; ++i) if (P.pad_seen[i] && (P.pad[i].Gamepad.wButtons & 0x20) && P.pad[i].Gamepad.bAnalogButtons[3]) both = 1;
        if (ch && !P.active) { if (both) { if (++held == 36u) { P.req = 1; held = 0; } } else held = 0; } else held = 0;
    }
    keys = (key(P.vk_toggle) ? 1u : 0u) | (key(P.vk_shot) ? 2u : 0u) | (key(P.vk_hud) ? 4u : 0u) | (key(VK_ESCAPE) ? 8u : 0u);
    if (!P.active) {
        if ((keys & 1u) && !(P.prev_keys & 1u)) P.req = 1;
        P.prev_keys = keys;
        if (P.req == 1) { P.req = 0; activate(p); }
        else { P.req = 0; if (g_post >= 0) { g_post++; if (g_post == 2 || g_post == 60 || g_post == 120 || g_post == 300) clk_dump("after-close"); /* debug */ } return 0; }
        P.prev_keys = keys;
    } else if (P.req == 2) { P.req = 0; deactivate("overlay"); return 0; }
    P.req = 0;
    { static int pas = -1; if (pas < 0) { const char *e = getenv("STE_PHOTO_PASSIVE"); pas = e && *e && *e != '0'; } if (pas) return P.active; }   /* bisect switch: state machine only, nothing written */
    QueryPerformanceCounter(&now);
    if (P.have_t) { dt = (float)((double)(now.QuadPart - P.t_prev.QuadPart) / (double)P.qpf.QuadPart); if (dt > 0.1f) dt = 0.1f; if (dt < 0.0005f) dt = 0.0005f; }
    P.t_prev = now; P.have_t = 1; P.n++;
    for (i = 0; i < 4; ++i) if (P.pad_seen[i]) {
        const XBOX_GAMEPAD *g = &P.pad[i].Gamepad;
        lx += g->sThumbLX / 32768.0f; ly += g->sThumbLY / 32768.0f; rx += g->sThumbRX / 32768.0f; ry += g->sThumbRY / 32768.0f;
        lt += g->bAnalogButtons[6] / 255.0f; rt += g->bAnalogButtons[7] / 255.0f;
        btn |= (g->bAnalogButtons[0] ? 1u : 0u) | (g->bAnalogButtons[1] ? 2u : 0u) | (g->bAnalogButtons[3] ? 4u : 0u) | (g->bAnalogButtons[4] ? 8u : 0u) | (g->bAnalogButtons[5] ? 16u : 0u)
             | ((g->wButtons & 1) ? 32u : 0u) | ((g->wButtons & 2) ? 64u : 0u) | ((g->wButtons & 4) ? 128u : 0u) | ((g->wButtons & 8) ? 256u : 0u);
    }
    if (key('W')) ly += 1.0f; if (key('S')) ly -= 1.0f; if (key('D')) lx += 1.0f; if (key('A')) lx -= 1.0f;
    if (key(VK_RIGHT)) rx += 1.0f; if (key(VK_LEFT)) rx -= 1.0f; if (key(VK_UP)) ry += 1.0f; if (key(VK_DOWN)) ry -= 1.0f;
    { float m = sqrtf(lx * lx + ly * ly); if (m > 1.0f) { lx /= m; ly /= m; } m = sqrtf(rx * rx + ry * ry); if (m > 1.0f) { rx /= m; ry /= m; } }
    if (lt > 1.0f) lt = 1.0f; if (rt > 1.0f) rt = 1.0f;
    { float dead = 0.12f; if (fabsf(lx) < dead && fabsf(ly) < dead) lx = ly = 0; if (fabsf(rx) < dead && fabsf(ry) < dead) rx = ry = 0; }
    /* buttons: pad A screenshot, B exit, Y HUD, dpad up/down FOV, left/right roll; keys: F12 / Esc / F8 / H, [ ] or +/- FOV, Z X roll */
    if ((btn & 1u) && !(P.prev_btn & 1u)) P.shot_req = 2;
    if ((btn & 2u) && !(P.prev_btn & 2u)) { deactivate("B"); P.prev_btn = btn; return 0; }
    if ((btn & 4u) && !(P.prev_btn & 4u)) { P.hud_hidden = !P.hud_hidden; fprintf(stderr, "[PHOTO] HUD %s\n", P.hud_hidden ? "hidden" : "shown"); fflush(stderr); }
    if ((keys & 2u) && !(P.prev_keys & 2u)) P.shot_req = 2;
    if ((keys & 4u) && !(P.prev_keys & 4u)) { P.hud_hidden = !P.hud_hidden; fprintf(stderr, "[PHOTO] HUD %s\n", P.hud_hidden ? "hidden" : "shown"); fflush(stderr); }
    if (((keys & 1u) && !(P.prev_keys & 1u)) || ((keys & 8u) && !(P.prev_keys & 8u))) { deactivate("key"); P.prev_keys = keys; P.prev_btn = btn; return 0; }
    P.prev_keys = keys; P.prev_btn = btn;
    {   /* FOV (log scale) and roll */
        float zi = 0.0f, ro = 0.0f;
        if (btn & 32u) zi += 1.0f; if (btn & 64u) zi -= 1.0f; if (key(VK_OEM_PLUS) || key(VK_ADD) || key(VK_OEM_6)) zi += 1.0f; if (key(VK_OEM_MINUS) || key(VK_SUBTRACT) || key(VK_OEM_4)) zi -= 1.0f;
        if (btn & 256u) ro += 1.0f; if (btn & 128u) ro -= 1.0f; if (key('X')) ro += 1.0f; if (key('Z')) ro -= 1.0f;
        if (zi != 0.0f) { P.zoom *= expf(zi * P.fov_step * dt); if (P.zoom < 0.35f) P.zoom = 0.35f; if (P.zoom > 6.0f) P.zoom = 6.0f; }
        if (ro != 0.0f) { P.roll += ro * 0.8f * dt; if (P.roll > PI_F) P.roll = PI_F; if (P.roll < -PI_F) P.roll = -PI_F; }
        if (btn & 128u && btn & 256u) P.roll = 0.0f;
    }
    {   /* look and move */
        float ra = 1.6f * dt, sp = P.speed * dt, mouse_k = 0.003f;
        if (btn & 8u) sp *= 0.25f; if (btn & 16u) sp *= 4.0f; if (key(VK_SHIFT)) sp *= 4.0f; if (key(VK_CONTROL)) sp *= 0.25f;
        P.yaw += rx * ra + P.mdx * mouse_k; P.pitch += -ry * ra + P.mdy * mouse_k; P.mdx = P.mdy = 0.0f;
        if (P.pitch > 1.45f) P.pitch = 1.45f; if (P.pitch < -1.45f) P.pitch = -1.45f;
        basis(&R, &U, &F);
        P.eye = v_add(P.eye, v_mul(F, ly * sp)); P.eye = v_add(P.eye, v_mul(R, lx * sp));
        { float vert = rt - lt; if (key('E')) vert += 1.0f; if (key('Q')) vert -= 1.0f; P.eye = v_add(P.eye, v_mul(P.W, vert * sp)); }
        {   /* range limit: stay inside a sphere around where photo mode was opened */
            V3 d = { P.eye.x - P.eye0.x, P.eye.y - P.eye0.y, P.eye.z - P.eye0.z }; float l = sqrtf(v_dot(d, d));
            if (l > P.range) P.eye = v_add(P.eye0, v_mul(d, P.range / l));
        }
    }
    basis(&R, &U, &F);
    p[0] = R.x; p[3] = R.y; p[6] = R.z; p[1] = U.x; p[4] = U.y; p[7] = U.z; p[2] = F.x; p[5] = F.y; p[8] = F.z;
    p[9] = -v_dot(R, P.eye); p[10] = -v_dot(U, P.eye); p[11] = -v_dot(F, P.eye);
    g_ste_hide_2d = P.hud_hidden;
    if (0) { float rt, pt; uint32_t rf = GW(0x00442708u), rtu = GW(0x0044270Cu); memcpy(&rt, &rtu, 4); memcpy(&pt, &g_pin_time, 4); fprintf(stderr, "[PHOTOCLK] n=%u frame raw=%u pinned=%u time raw=%.4f pinned=%.4f\n", P.n, rf, g_pin_frame, rt, pt); fflush(stderr); }
    { static int nopin = -1; if (nopin < 0) { const char *e = getenv("STE_PHOTO_NOPIN"); nopin = e && *e && *e != '0'; } if (!nopin) { GW(0x00442708u) = g_pin_frame; GW(0x0044270Cu) = g_pin_time; } }   /* the clock stands still (see above) */
    if (proj_host_ptr) {   /* FOV: scale the focal terms of the camera's projection matrix, originals kept for the exit */
        float *m = (float *)proj_host_ptr;
        if (!P.proj_saved || P.proj_ptr != proj_host_ptr) { restore_proj(); P.proj_ptr = proj_host_ptr; P.m00 = m[0]; P.m11 = m[5]; P.proj_saved = 1;
            fprintf(stderr, "[PHOTO] projection m00=%.4f m11=%.4f\n", m[0], m[5]); fflush(stderr); }
        m[0] = P.m00 * P.zoom; m[5] = P.m11 * P.zoom;
    }
    if (P.shot_req == 2) {   /* the capture runs at the end of this frame (ste_pb_hook.c) */
        char dir[MAX_PATH], sd[MAX_PATH]; SYSTEMTIME t; GetLocalTime(&t);
        data_dir(dir, sizeof dir); _snprintf(sd, sizeof sd, "%s\\screenshots", dir); sd[MAX_PATH - 1] = 0; CreateDirectoryA(dir, NULL); CreateDirectoryA(sd, NULL);
        _snprintf(P.shot_path, sizeof P.shot_path, "%s\\photo_%04d%02d%02d_%02d%02d%02d_%u.png", sd, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, ++P.shots); P.shot_path[MAX_PATH - 1] = 0;
        P.shot_req = 1; P.shot_delay = 3;   /* the render thread lags the game thread by a frame or two: capture 3 frame ends later */
    }
    scan_poll();
    if ((P.n % 120u) == 1u) { fprintf(stderr, "[PHOTO] n=%u eye=(%.1f %.1f %.1f) yaw=%.2f pitch=%.2f roll=%.2f zoom=%.2f hud=%d hidden2d=%u\n", P.n, P.eye.x, P.eye.y, P.eye.z, P.yaw, P.pitch, P.roll, P.zoom, P.hud_hidden, g_ste_hide_2d_cnt); fflush(stderr); }
    return 1;
}
