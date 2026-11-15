/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ste_settings_glue.c (c376): the part of the overlay hook that does not need the overlay.
 *
 * The D3D scan-out path calls ste_overlay_frame(device, rtv, w, h) once per presented frame (right before Present). In the default release
 * build the in-game overlay library is NOT linked (it needs a newer C++ runtime and still crashes at start-up inside the game process, see
 * docs\NOTE_FOR_LAUNCHER_overlay_blocker_0.1.4.md), so this file provides the hook with the two settings that never needed it:
 *   [audio]  master_volume = 0..100   (settings ini named by STE_VIDEO_INI; XAudio2 master volume)
 *   [window] hide_cursor   = auto | always | never   (auto: the pointer disappears after 3 s without movement)
 * and the small interface the input module and the frame hook expect (ste_overlay_visible() is always 0 here).
 * The overlay build is candidate376\src\ste_overlay_glue.cpp + candidate376\relink376b_overlay.bat. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void ste_audio_set_master_volume(int percent);

static int g_cursor_mode = 0;   /* 0 auto, 1 always hidden over the window, 2 never */
static int g_cursor_hidden = 0;
static HCURSOR g_arrow;
static POINT g_last_pt;
static ULONGLONG g_moved;

void ste_overlay_key(int vk) { (void)vk; }
int ste_overlay_visible(void) { return 0; }
void ste_overlay_wait_if_paused(void) { }

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

static void cursor_tick(void)
{
    static HWND wnd; static ULONGLONG chk; ULONGLONG t = GetTickCount64(); POINT pt; int want_hide;
    if (g_cursor_mode == 2 && !g_cursor_hidden) return;   /* never: nothing to do unless we hid it earlier */
    if (!wnd || t - chk > 2000) { HWND h = NULL; chk = t; EnumWindows(find_wnd, (LPARAM)&h); if (h) wnd = h; }
    if (!wnd) return;
    if (!g_arrow) g_arrow = LoadCursorA(NULL, IDC_ARROW);
    GetCursorPos(&pt);
    if (pt.x != g_last_pt.x || pt.y != g_last_pt.y) { g_last_pt = pt; g_moved = t; }
    want_hide = (g_cursor_mode == 1) || (g_cursor_mode == 0 && t - g_moved > 3000);
    if (want_hide != g_cursor_hidden) {
        POINT p2;
        g_cursor_hidden = want_hide;
        SetClassLongPtrA(wnd, GCLP_HCURSOR, (LONG_PTR)(want_hide ? NULL : g_arrow));
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

void ste_overlay_frame(void *device, void *rtv, int w, int h)
{
    static int ini_done;
    (void)device; (void)rtv; (void)w; (void)h;
    if (!ini_done) { ini_done = 1; read_ini_defaults(); }
    cursor_tick();
}
