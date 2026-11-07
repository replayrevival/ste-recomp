/* SPDX-License-Identifier: GPL-2.0-or-later
 * Video configuration shared by the D3D8 layer, the NV2A translator and the host hook (STE resolution scaling). */
#ifndef D3D8_VIDEO_H
#define D3D8_VIDEO_H
#include <windows.h>

enum { D3D8V_WINDOW = 0, D3D8V_BORDERLESS = 1, D3D8V_FULLSCREEN = 2 };
enum { D3D8V_ASPECT_PILLAR = 0, D3D8V_ASPECT_STRETCH = 1, D3D8V_ASPECT_WIDE = 2 };

typedef struct D3D8VideoCfg {
    float scale;              /* render height / 480 */
    UINT  out_w, out_h;       /* window client area == swap chain size */
    UINT  render_w, render_h; /* size of the display surface and of every 640x480 guest surface */
    int   mode;               /* D3D8V_* */
    int   aspect;             /* D3D8V_ASPECT_* */
    float logical_w;          /* guest-space width of a screen-sized surface (640, or 480 * render aspect when wide) */
    int   fps_cap;            /* ste_video.ini fps_cap: 30 / 60 / 0 = unlimited (default 60); env STE_VIDEO_FPS_CAP */
    int   render_ahead;       /* ste_video.ini render_ahead = 1 | 2: frames the async Kelvin render thread may lag the guest (KELVIN_ASYNC_DEPTH / STE_VIDEO_RENDER_AHEAD override) */
    int   vsync;              /* ste_video.ini vsync = on/off (default off); env STE_VIDEO_VSYNC */
    int   menu_aspect;        /* c102: ste_video.ini menu_aspect = pillar (default) | stretch | wide: layout of front-end/shell screens when aspect = wide (D3D8V_ASPECT_* values) */
    int   upscale;            /* c180: ste_video.ini upscale = bilinear | fsr1 (AMD FidelityFX FSR 1.0 EASU+RCAS, MIT); default fsr1 when render_scale < 1, else bilinear */
    float render_scale;       /* c180: ste_video.ini render_scale = 0.25..1.0: internal render height as a fraction of the window height (0 = off: scale / window height decide) */
    float fsr_sharp;          /* c180: ste_video.ini fsr_sharpness = RCAS sharpness in stops (0 = sharpest, default 0.25) */
    int   display;            /* c252: ste_video.ini display = <n> (0-based, SDL3 SDL_GetDisplays order, as written by the launcher); -1 = not set (primary) */
    int   mon_x, mon_y;       /* c252: top-left of the monitor the window opens on, virtual-screen pixels */
    int   mon_w, mon_h;       /* c252: its size in pixels */
} D3D8VideoCfg;

/* c252: set by the host exe (ste_pb_hook.c, SDL3) before the config is read: fills `bounds` (virtual-screen pixels) of display `index` in SDL_GetDisplays order and the number of displays
 * in *count; returns 1 on success, 0 when index is out of range or the lookup failed. NULL = Win32 lookup only (primary monitor). */
typedef int (*D3D8DisplayResolver)(int index, RECT *bounds, int *count);
extern D3D8DisplayResolver d3d8_display_resolver;
/* c252: the fullscreen request could not be honoured: the window stays a borderless window of the render size (see the host's window placement). */
void d3d8_video_fullscreen_failed(const char *why);

/* Parsed once from ste_video.ini (next to the exe; STE_VIDEO_INI overrides the path) with env overrides
 * STE_VIDEO_SCALE / _WIDTH / _HEIGHT / _MODE / _ASPECT. Never NULL. */
const D3D8VideoCfg *d3d8_video_cfg(void);
/* Horizontal mapping of guest window x for pre-transformed vertices on screen-sized targets:
 * x' = x * s + o (guest units; the target's logical width is d3d8_video_cfg()->logical_w). */
void d3d8_set_kelvin_xmap(float s, float o);
/* Frame pacing from the guest's Swap hook: waits (QPC; coarse Sleep then spin) until 1/fps_cap after the previous frame. No-op for fps_cap 0 or >= 60 (the emulated 60 Hz vblank already limits the guest). */
void d3d8_video_pace(void);
/* c102: 1 while the title's front-end/shell is the active screen (guest word 0x003562B4 == 0x27: main menu, profiles, character/city select; in-game and loading = 4). */
int d3d8_front_end_active(void);

#endif
