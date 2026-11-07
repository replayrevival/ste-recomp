/* SPDX-License-Identifier: GPL-2.0-or-later
 * ste_sdlinput.c - SDL3 gamepad input for the Godzilla STE recomp (a Replay Revival recomp).
 *
 * Replaces the XInput-only host pad source: Xbox, PlayStation, Switch and generic pads through SDL3's gamepad API (zlib licence; docs\CREDITS_AND_SOURCES.md),
 * the launcher's mapping file (STE_INPUT_MAP, format: launcher docs INPUT_MAPPING.md, version 1), per-port deadzone and rumble scale,
 * the Guide/Home button (plus F1 and a Back+Start chord) as the overlay request, and keyboard key bindings (key:<SDL name>).
 * XInput stays as the fallback: ste_sdl_init() returns 0 when SDL cannot start or STE_INPUT_BACKEND=xinput (ini [input] backend = auto | sdl | xinput).
 * The host side (recomp_manual.c) owns the Xbox ports; this file only answers "which pad", "its state as an Xbox pad" and "rumble".
 * No game code or data here.
 */
#include <SDL3/SDL.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "xinput_xbox.h"
#include "ste_sdlinput.h"

/* ---------- actions ---------- */
enum { A_A, A_B, A_X, A_Y, A_BLACK, A_WHITE, A_LT, A_RT, A_START, A_BACK, A_LTHUMB, A_RTHUMB, A_DUP, A_DDOWN, A_DLEFT, A_DRIGHT, A_MOVE, A_LOOK, A_N };
static const char *k_act[A_N] = { "a", "b", "x", "y", "black", "white", "left_trigger", "right_trigger", "start", "back", "left_thumb", "right_thumb", "dpad_up", "dpad_down", "dpad_left", "dpad_right", "move", "look" };
static const char *k_def[A_N] = { "a", "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "start", "back", "leftstick", "rightstick", "dpup", "dpdown", "dpleft", "dpright", "leftstick", "rightstick" };
/* source = (kind << 16) | value */
enum { K_NONE, K_BTN, K_TRIG, K_STICK, K_KEY };
#define SRC(k, v) (((k) << 16) | (v))

typedef struct { float dz; int rumble; int src[A_N]; int has_keys; } PortCfg;

/* ---------- minimal JSON ---------- */
typedef struct J { int t; double n; char *s; struct J **kid; char **key; int nk; } J;   /* t: 0 null 1 bool 2 num 3 str 4 arr 5 obj */
static const char *jp;
static void jws(void) { while (*jp == ' ' || *jp == '\t' || *jp == '\r' || *jp == '\n') jp++; }
static J *jnew(int t) { J *j = (J *)calloc(1, sizeof *j); j->t = t; return j; }
static char *jstr(void)
{
    char *o = (char *)malloc(strlen(jp) + 1); size_t n = 0;
    jp++;
    while (*jp && *jp != '"') {
        if (*jp == '\\' && jp[1]) {
            jp++;
            switch (*jp) { case 'n': o[n++] = '\n'; break; case 't': o[n++] = '\t'; break; case 'r': o[n++] = '\r'; break; case 'b': o[n++] = '\b'; break; case 'f': o[n++] = '\f'; break;
                case 'u': { unsigned cp = 0; int i; for (i = 1; i <= 4 && jp[i]; ++i) cp = cp * 16 + (unsigned)(jp[i] <= '9' ? jp[i] - '0' : (jp[i] | 32) - 'a' + 10); jp += 4; o[n++] = cp < 128 ? (char)cp : '?'; break; }
                default: o[n++] = *jp; }
            jp++;
        } else o[n++] = *jp++;
    }
    if (*jp == '"') jp++;
    o[n] = 0; return o;
}
static J *jval(int depth)
{
    J *j;
    jws();
    if (depth > 16) return NULL;
    if (*jp == '{') {
        j = jnew(5); jp++; jws();
        while (*jp && *jp != '}') {
            char *k; J *v;
            jws(); if (*jp != '"') return NULL;
            k = jstr(); jws(); if (*jp != ':') return NULL; jp++;
            v = jval(depth + 1); if (!v) return NULL;
            j->kid = (J **)realloc(j->kid, (size_t)(j->nk + 1) * sizeof *j->kid); j->key = (char **)realloc(j->key, (size_t)(j->nk + 1) * sizeof *j->key);
            j->kid[j->nk] = v; j->key[j->nk] = k; j->nk++;
            jws(); if (*jp == ',') jp++;
        }
        if (*jp != '}') return NULL; jp++; return j;
    }
    if (*jp == '[') {
        j = jnew(4); jp++; jws();
        while (*jp && *jp != ']') {
            J *v = jval(depth + 1); if (!v) return NULL;
            j->kid = (J **)realloc(j->kid, (size_t)(j->nk + 1) * sizeof *j->kid); j->kid[j->nk++] = v;
            jws(); if (*jp == ',') jp++;
        }
        if (*jp != ']') return NULL; jp++; return j;
    }
    if (*jp == '"') { j = jnew(3); j->s = jstr(); return j; }
    if (!strncmp(jp, "true", 4)) { j = jnew(1); j->n = 1; jp += 4; return j; }
    if (!strncmp(jp, "false", 5)) { j = jnew(1); jp += 5; return j; }
    if (!strncmp(jp, "null", 4)) { jp += 4; return jnew(0); }
    if (*jp == '-' || (*jp >= '0' && *jp <= '9')) { char *e; j = jnew(2); j->n = strtod(jp, &e); if (e == jp) return NULL; jp = e; return j; }
    return NULL;
}
static J *jget(const J *o, const char *k) { int i; if (!o || o->t != 5) return NULL; for (i = 0; i < o->nk; ++i) if (!strcmp(o->key[i], k)) return o->kid[i]; return NULL; }
static const char *jstrv(const J *o, const char *k) { J *v = jget(o, k); return v && v->t == 3 ? v->s : NULL; }

/* ---------- state ---------- */
#define MAXPADS 8
typedef struct { SDL_JoystickID id; SDL_Gamepad *gp; char guid[40]; unsigned seq; int open; } Pad;
static Pad g_pad[MAXPADS];
static unsigned g_seq; static unsigned char g_guide_was[MAXPADS];
static int g_on, g_backend_sdl;
static CRITICAL_SECTION g_cs;
static J *g_map;                       /* parsed STE_INPUT_MAP, NULL = built-in defaults */
static unsigned char g_key[256];       /* VK down state, fed by the window procedure (focus-aware) */
static volatile LONG g_ovl_req;        /* overlay request counter */
static int g_ovl_guide = 1, g_ovl_chord = 1, g_ovl_vk = VK_F1; static DWORD g_chord_ms = 1000;
static volatile LONG g_ovl_why;        /* 1 guide, 2 key, 3 chord */
static volatile LONG g_ovl_port;

static int dbg(void) { static int v = -1; if (v < 0) { const char *e = getenv("STE_INPUT_TRACE"); v = e && *e && *e != '0'; } return v; }

static int pad_by_slot(int s) { return s >= 0 && s < MAXPADS && g_pad[s].open ? 1 : 0; }

/* ---------- names ---------- */
static int vk_from_name(const char *n)
{
    static const struct { const char *n; int vk; } t[] = {
        { "Left", VK_LEFT }, { "Right", VK_RIGHT }, { "Up", VK_UP }, { "Down", VK_DOWN }, { "Return", VK_RETURN }, { "Space", VK_SPACE }, { "Tab", VK_TAB }, { "Backspace", VK_BACK },
        { "Left Shift", VK_LSHIFT }, { "Right Shift", VK_RSHIFT }, { "Left Ctrl", VK_LCONTROL }, { "Right Ctrl", VK_RCONTROL }, { "Left Alt", VK_LMENU }, { "Right Alt", VK_RMENU },
        { "Insert", VK_INSERT }, { "Delete", VK_DELETE }, { "Home", VK_HOME }, { "End", VK_END }, { "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT },
        { "Keypad Enter", VK_RETURN }, { "Keypad +", VK_ADD }, { "Keypad -", VK_SUBTRACT }, { "Keypad *", VK_MULTIPLY }, { "Keypad /", VK_DIVIDE }, { "Keypad .", VK_DECIMAL },
        { ",", VK_OEM_COMMA }, { ".", VK_OEM_PERIOD }, { "-", VK_OEM_MINUS }, { "=", VK_OEM_PLUS }, { ";", VK_OEM_1 }, { "/", VK_OEM_2 }, { "`", VK_OEM_3 }, { "[", VK_OEM_4 }, { "\\", VK_OEM_5 }, { "]", VK_OEM_6 }, { "'", VK_OEM_7 },
        { "CapsLock", VK_CAPITAL }, { NULL, 0 } };
    int i;
    if (!n || !*n) return 0;
    if (n[1] == 0) { char c = n[0]; if (c >= 'a' && c <= 'z') c = (char)(c - 32); if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c; }
    if (!_strnicmp(n, "Keypad ", 7) && n[7] >= '0' && n[7] <= '9' && !n[8]) return VK_NUMPAD0 + (n[7] - '0');
    if ((n[0] == 'F' || n[0] == 'f') && n[1] >= '1' && n[1] <= '9') { int f = atoi(n + 1); if (f >= 1 && f <= 12) return VK_F1 + f - 1; }
    for (i = 0; t[i].n; ++i) if (!_stricmp(n, t[i].n)) return t[i].vk;
    return 0;
}
static int src_parse(const char *s)
{
    static const struct { const char *n; int src; } t[] = {
        { "a", SRC(K_BTN, SDL_GAMEPAD_BUTTON_SOUTH) }, { "b", SRC(K_BTN, SDL_GAMEPAD_BUTTON_EAST) }, { "x", SRC(K_BTN, SDL_GAMEPAD_BUTTON_WEST) }, { "y", SRC(K_BTN, SDL_GAMEPAD_BUTTON_NORTH) },
        { "back", SRC(K_BTN, SDL_GAMEPAD_BUTTON_BACK) }, { "start", SRC(K_BTN, SDL_GAMEPAD_BUTTON_START) },
        { "leftshoulder", SRC(K_BTN, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) }, { "rightshoulder", SRC(K_BTN, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) },
        { "dpup", SRC(K_BTN, SDL_GAMEPAD_BUTTON_DPAD_UP) }, { "dpdown", SRC(K_BTN, SDL_GAMEPAD_BUTTON_DPAD_DOWN) }, { "dpleft", SRC(K_BTN, SDL_GAMEPAD_BUTTON_DPAD_LEFT) }, { "dpright", SRC(K_BTN, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) },
        { "leftstick", SRC(K_STICK, 0) }, { "rightstick", SRC(K_STICK, 1) },
        { "lefttrigger", SRC(K_TRIG, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) }, { "righttrigger", SRC(K_TRIG, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) }, { NULL, 0 } };
    int i;
    if (!s || !*s) return 0;
    if (!_strnicmp(s, "key:", 4)) { int vk = vk_from_name(s + 4); if (!vk && dbg()) fprintf(stderr, "[INPUT] unsupported key name '%s'\n", s + 4); return vk ? SRC(K_KEY, vk) : 0; }
    for (i = 0; t[i].n; ++i) if (!_stricmp(s, t[i].n)) return t[i].src;
    return 0;   /* includes "guide": never bound */
}

/* ---------- config ---------- */
static void cfg_defaults(PortCfg *c) { int i; c->dz = 0.0f; c->rumble = 100; c->has_keys = 0; for (i = 0; i < A_N; ++i) c->src[i] = src_parse(k_def[i]); }
static void cfg_from(PortCfg *c, const J *st)
{
    const J *b, *v; int i;
    if (!st || st->t != 5) return;
    if ((v = jget(st, "deadzone")) && v->t == 2) { c->dz = (float)v->n; if (c->dz < 0) c->dz = 0; if (c->dz > 0.5f) c->dz = 0.5f; }
    if ((v = jget(st, "rumble")) && v->t == 2) { c->rumble = (int)v->n; if (c->rumble < 0) c->rumble = 0; if (c->rumble > 100) c->rumble = 100; }
    b = jget(st, "bindings");
    if (b && b->t == 5) for (i = 0; i < A_N; ++i) { const char *s = jstrv(b, k_act[i]); if (s) { c->src[i] = src_parse(s); if ((c->src[i] >> 16) == K_KEY) c->has_keys = 1; } }
}
/* effective settings of a port for the device that is in it (guid = pad GUID, "" / NULL = keyboard) */
static void cfg_for(int port, const char *guid, PortCfg *c)
{
    const J *ports, *pe = NULL, *prof; int i;
    cfg_defaults(c);
    if (!g_map) return;
    ports = jget(g_map, "ports");
    if (ports && ports->t == 4) for (i = 0; i < ports->nk; ++i) { const J *pn = jget(ports->kid[i], "port"); if (pn && pn->t == 2 && (int)pn->n == port + 1) { pe = ports->kid[i]; break; } }
    prof = jget(g_map, "profiles");
    if (pe) {
        const char *pg = jstrv(pe, "guid"), *pp = jstrv(pe, "profile");
        int same = guid ? (pg && !_stricmp(pg, guid)) : (pp && !_stricmp(pp, "keyboard"));
        if (same && jget(pe, "settings")) { cfg_from(c, jget(pe, "settings")); return; }
    }
    if (guid && prof && jget(prof, guid)) cfg_from(c, jget(prof, guid));
    else if (!guid && prof && jget(prof, "keyboard")) cfg_from(c, jget(prof, "keyboard"));
    else if (prof && jget(prof, "default")) cfg_from(c, jget(prof, "default"));
}

/* ---------- pads ---------- */
static void pads_refresh_locked(void)
{
    int n = 0, i, s; SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (s = 0; s < MAXPADS; ++s) {   /* drop pads that are gone */
        int still = 0;
        if (!g_pad[s].open) continue;
        for (i = 0; ids && i < n; ++i) if (ids[i] == g_pad[s].id) still = 1;
        if (!still) { fprintf(stderr, "[INPUT] pad removed: slot %d (%s)\n", s, g_pad[s].guid); fflush(stderr); SDL_CloseGamepad(g_pad[s].gp); g_pad[s].open = 0; g_pad[s].gp = NULL; }
    }
    for (i = 0; ids && i < n; ++i) {   /* add new ones in connection order */
        int have = 0;
        for (s = 0; s < MAXPADS; ++s) if (g_pad[s].open && g_pad[s].id == ids[i]) have = 1;
        if (have) continue;
        for (s = 0; s < MAXPADS && g_pad[s].open; ++s) ;
        if (s == MAXPADS) break;
        g_pad[s].gp = SDL_OpenGamepad(ids[i]);
        if (!g_pad[s].gp) continue;
        g_pad[s].id = ids[i]; g_pad[s].open = 1; g_pad[s].seq = ++g_seq;
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), g_pad[s].guid, sizeof g_pad[s].guid);
        fprintf(stderr, "[INPUT] pad added: slot %d \"%s\" guid=%s type=%d\n", s, SDL_GetGamepadName(g_pad[s].gp), g_pad[s].guid, (int)SDL_GetGamepadType(g_pad[s].gp)); fflush(stderr);
    }
    SDL_free(ids);
}

static float stick_axis(SDL_Gamepad *gp, int which, int y)
{
    SDL_GamepadAxis ax = which ? (y ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_RIGHTX) : (y ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_LEFTX);
    float v = (float)SDL_GetGamepadAxis(gp, ax) / 32767.0f;
    if (v < -1) v = -1; if (v > 1) v = 1;
    return v;
}
/* 0..1 value of a source for a button-like action */
static float src_btn(SDL_Gamepad *gp, int src, int *pressed)
{
    int k = src >> 16, v = src & 0xFFFF; float f = 0;
    if (k == K_BTN) f = SDL_GetGamepadButton(gp, (SDL_GamepadButton)v) ? 1.0f : 0.0f;
    else if (k == K_STICK) f = SDL_GetGamepadButton(gp, v ? SDL_GAMEPAD_BUTTON_RIGHT_STICK : SDL_GAMEPAD_BUTTON_LEFT_STICK) ? 1.0f : 0.0f;
    else if (k == K_TRIG) { f = (float)SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)v) / 32767.0f; if (f < 0) f = 0; }
    *pressed = k == K_TRIG ? f > 0.5f : f > 0.0f;
    return f;
}
static void apply_dz(float *x, float *y, float dz)
{
    float m = sqrtf(*x * *x + *y * *y), s;
    if (dz <= 0.0f) return;   /* raw (XInput-like) stick, no gate */
    if (m <= dz || m <= 0.0f) { *x = 0; *y = 0; return; }
    if (m > 1.0f) m = 1.0f;
    s = (m - dz) / (1.0f - dz) / (sqrtf(*x * *x + *y * *y));
    *x *= s; *y *= s;
    if (*x > 1) *x = 1; if (*x < -1) *x = -1; if (*y > 1) *y = 1; if (*y < -1) *y = -1;
}
static SHORT sh(float v) { return (SHORT)(v >= 1.0f ? 32767 : v <= -1.0f ? -32768 : v * 32767.0f); }

/* ---------- public ---------- */
int ste_sdl_init(void)
{
    const char *be = getenv("STE_INPUT_BACKEND"), *mp = getenv("STE_INPUT_MAP"); char ini[MAX_PATH], v[32] = "auto", *p; const char *iv = getenv("STE_ENHANCE_INI");
    if (g_on) return g_backend_sdl;
    g_on = 1;
    InitializeCriticalSection(&g_cs);
    if (!(be && *be)) {
        if (iv && *iv) { strncpy(ini, iv, MAX_PATH - 1); ini[MAX_PATH - 1] = 0; } else { GetModuleFileNameA(NULL, ini, MAX_PATH); p = strrchr(ini, 0x5C); if (p) strcpy(p + 1, "ste_enhance.ini"); }
        GetPrivateProfileStringA("input", "backend", "auto", v, sizeof v, ini); be = v;
        g_ovl_guide = GetPrivateProfileIntA("input", "overlay_guide", 1, ini) != 0; g_ovl_chord = GetPrivateProfileIntA("input", "overlay_chord", 1, ini) != 0;
        { char kn[32] = "F1"; GetPrivateProfileStringA("input", "overlay_key", "F1", kn, sizeof kn, ini); g_ovl_vk = !_stricmp(kn, "none") ? 0 : vk_from_name(kn); }
        g_chord_ms = (DWORD)GetPrivateProfileIntA("input", "overlay_chord_ms", 1000, ini);
    }
    if (!_stricmp(be, "xinput")) { fprintf(stderr, "[INPUT] backend=xinput (configured)\n"); return 0; }
    if (mp && *mp) {   /* launcher mapping file */
        FILE *f = fopen(mp, "rb");
        if (!f) fprintf(stderr, "[INPUT] STE_INPUT_MAP=%s: cannot open, using built-in defaults\n", mp);
        else {
            long n; char *buf;
            fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
            buf = (char *)malloc((size_t)n + 1);
            if (buf && n > 0 && n < (8 << 20) && fread(buf, 1, (size_t)n, f) == (size_t)n) {
                J *r; buf[n] = 0; jp = buf; if ((unsigned char)buf[0] == 0xEF && n > 3) jp = buf + 3;
                r = jval(0);
                if (r && r->t == 5 && jget(r, "format") && (int)jget(r, "format")->n == 1) { g_map = r; fprintf(stderr, "[INPUT] mapping file %s: format 1, %d ports\n", mp, jget(r, "ports") ? jget(r, "ports")->nk : 0); }
                else fprintf(stderr, "[INPUT] STE_INPUT_MAP=%s: not a valid format-1 mapping file, using built-in defaults\n", mp);
            } else fprintf(stderr, "[INPUT] STE_INPUT_MAP=%s: unreadable, using built-in defaults\n", mp);
            fclose(f);
        }
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { fprintf(stderr, "[INPUT] SDL_Init failed (%s): falling back to XInput\n", SDL_GetError()); return 0; }
    g_backend_sdl = 1;
    fprintf(stderr, "[INPUT] backend=sdl3 %s (SDL %d.%d.%d) overlay: guide=%d key=0x%02X chord=%d(%lu ms)\n", g_map ? "with mapping file" : "built-in defaults", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION, g_ovl_guide, g_ovl_vk, g_ovl_chord, (unsigned long)g_chord_ms);
    {   /* test pads without hardware: STE_SDL_VIRTUAL=<n> attaches n virtual gamepads (buttons/axes stay neutral; tests\sdl_input_selftest.c drives them) */
        const char *vn = getenv("STE_SDL_VIRTUAL"); int n = vn ? atoi(vn) : 0, i;
        for (i = 0; i < n && i < MAXPADS; ++i) {
            SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d);
            d.type = SDL_JOYSTICK_TYPE_GAMEPAD; d.naxes = 6; d.nbuttons = 15; d.nhats = 0; d.vendor_id = 0x045E; d.product_id = 0x02FF + (Uint16)i; d.name = "STE virtual pad";
            SDL_AttachVirtualJoystick(&d);
        }
    }
    EnterCriticalSection(&g_cs); pads_refresh_locked(); LeaveCriticalSection(&g_cs);
    return 1;
}
int ste_sdl_active(void) { return g_backend_sdl; }

/* c252: monitor lookup for ste_video.ini [video] display=<n>. Same SDL3 build and call as the launcher (SDL_GetDisplays), so the index means the same monitor. The video subsystem is
 * started only for this query and shut down again. */
int ste_sdl_display_bounds(int index, int *x, int *y, int *w, int *h, int *count)
{
    int n = 0, ok = 0; SDL_DisplayID *ids; SDL_Rect r;
    if (count) *count = 0;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { fprintf(stderr, "[VIDEO] SDL video init failed (%s): display lookup unavailable\n", SDL_GetError()); return 0; }
    ids = SDL_GetDisplays(&n);
    if (count) *count = n;
    if (ids && index >= 0 && index < n && SDL_GetDisplayBounds(ids[index], &r)) { *x = r.x; *y = r.y; *w = r.w; *h = r.h; ok = 1; }
    SDL_free(ids);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return ok;
}

/* call from the host's port refresh (every XInput poll): keeps the pad list and the SDL state current */
/* test only (STE_SDL_VIRTUAL_CHURN=1): a virtual pad is attached and detached every 4 s to exercise hot-plug of the port map */
static void churn_locked(DWORD now)
{
    static int en = -1; static SDL_JoystickID vid; static DWORD at;
    if (en < 0) { const char *e = getenv("STE_SDL_VIRTUAL_CHURN"); en = e && *e && *e != '0'; }
    if (!en || now - at < 4000) return;
    at = now;
    if (vid) { SDL_DetachVirtualJoystick(vid); vid = 0; fprintf(stderr, "[INPUT] churn: virtual pad detached\n"); }
    else {
        SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD; d.naxes = 6; d.nbuttons = 15; d.vendor_id = 0x045E; d.product_id = 0x0300; d.name = "STE churn pad";
        vid = SDL_AttachVirtualJoystick(&d); fprintf(stderr, "[INPUT] churn: virtual pad attached (id %u)\n", (unsigned)vid);
    }
    fflush(stderr);
}
void ste_sdl_update(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (!g_backend_sdl) return;
    EnterCriticalSection(&g_cs);
    churn_locked(now);
    SDL_UpdateGamepads();
    if (now - last > 250) { last = now; pads_refresh_locked(); }
    LeaveCriticalSection(&g_cs);
}

int ste_sdl_pad_count(void) { int s, n = 0; for (s = 0; s < MAXPADS; ++s) n += g_pad[s].open; return n; }
int ste_sdl_slot_open(int slot) { return pad_by_slot(slot); }
const char *ste_sdl_slot_guid(int slot) { return pad_by_slot(slot) ? g_pad[slot].guid : ""; }

/* port device from the mapping file: "auto" | "keyboard" | "none" | <guid>; NULL = no mapping file (use the ini/env mode) */
const char *ste_sdl_map_device(int port)
{
    const J *ports = g_map ? jget(g_map, "ports") : NULL; int i;
    if (!ports || ports->t != 4) return NULL;
    for (i = 0; i < ports->nk; ++i) { const J *pn = jget(ports->kid[i], "port"); if (pn && pn->t == 2 && (int)pn->n == port + 1) return jstrv(ports->kid[i], "device"); }
    return NULL;
}
/* slot of the first open pad that is not in `claimed` (bit mask of slots), in connection order; with guid != NULL only pads of that GUID */
int ste_sdl_pick(unsigned claimed, const char *guid)
{
    int s, best = -1;
    for (s = 0; s < MAXPADS; ++s) {
        if (!g_pad[s].open || ((claimed >> s) & 1u)) continue;
        if (guid && _stricmp(guid, g_pad[s].guid)) continue;
        if (best < 0 || g_pad[s].seq < g_pad[best].seq) best = s;
    }
    return best;
}
/* n-th (0-based) open pad in connection order, ignoring claims (legacy "xinputN" ports) */
int ste_sdl_nth(int n)
{
    int idx[MAXPADS], m = 0, i, j, s;
    for (s = 0; s < MAXPADS; ++s) if (g_pad[s].open) idx[m++] = s;
    for (i = 0; i < m; ++i) for (j = i + 1; j < m; ++j) if (g_pad[idx[j]].seq < g_pad[idx[i]].seq) { int tt = idx[i]; idx[i] = idx[j]; idx[j] = tt; }
    return n >= 0 && n < m ? idx[n] : -1;
}

/* the pad in `slot` as an Xbox pad for `port`; returns 0 when the slot is not open */
int ste_sdl_get_state(int slot, int port, XBOX_INPUT_STATE *st)
{
    PortCfg c; SDL_Gamepad *gp; WORD w = 0; float lx, ly, rx, ry; int i, pr; float v;
    if (!pad_by_slot(slot)) return 0;
    EnterCriticalSection(&g_cs);
    gp = g_pad[slot].gp;
    cfg_for(port, g_pad[slot].guid, &c);
    memset(st, 0, sizeof *st);
    {   /* buttons */
        static const struct { int act; WORD bit; } dig[] = { { A_DUP, XBOX_GAMEPAD_DPAD_UP }, { A_DDOWN, XBOX_GAMEPAD_DPAD_DOWN }, { A_DLEFT, XBOX_GAMEPAD_DPAD_LEFT }, { A_DRIGHT, XBOX_GAMEPAD_DPAD_RIGHT },
            { A_START, XBOX_GAMEPAD_START }, { A_BACK, XBOX_GAMEPAD_BACK }, { A_LTHUMB, XBOX_GAMEPAD_LEFT_THUMB }, { A_RTHUMB, XBOX_GAMEPAD_RIGHT_THUMB } };
        for (i = 0; i < 8; ++i) { src_btn(gp, c.src[dig[i].act], &pr); if (pr) w |= dig[i].bit; }
        st->Gamepad.wButtons = w;
        for (i = A_A; i <= A_WHITE; ++i) { src_btn(gp, c.src[i], &pr); st->Gamepad.bAnalogButtons[i] = pr ? 255 : 0; }   /* A B X Y Black White = analog slots 0..5 */
        for (i = A_LT; i <= A_RT; ++i) { v = src_btn(gp, c.src[i], &pr); st->Gamepad.bAnalogButtons[i] = (BYTE)(((c.src[i] >> 16) == K_TRIG) ? (v * 255.0f + 0.5f) : (pr ? 255 : 0)); }
    }
    {   /* sticks: move -> Xbox left stick, look -> Xbox right stick; SDL's up is negative, Xbox's is positive */
        int mv = (c.src[A_MOVE] >> 16) == K_STICK ? (c.src[A_MOVE] & 0xFFFF) : 0, lk = (c.src[A_LOOK] >> 16) == K_STICK ? (c.src[A_LOOK] & 0xFFFF) : 1;
        lx = stick_axis(gp, mv, 0); ly = -stick_axis(gp, mv, 1); rx = stick_axis(gp, lk, 0); ry = -stick_axis(gp, lk, 1);
        apply_dz(&lx, &ly, c.dz); apply_dz(&rx, &ry, c.dz);
        st->Gamepad.sThumbLX = sh(lx); st->Gamepad.sThumbLY = sh(ly); st->Gamepad.sThumbRX = sh(rx); st->Gamepad.sThumbRY = sh(ry);
    }
    {   /* Guide/Home/PS button: edge-triggered overlay request */
        int gd = g_ovl_guide && SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_GUIDE);
        if (gd && !g_guide_was[slot]) { g_ovl_why = 1; g_ovl_port = port; InterlockedIncrement(&g_ovl_req); fprintf(stderr, "[OVERLAY] request: guide button (port %d)\n", port + 1); fflush(stderr); }
        g_guide_was[slot] = (unsigned char)gd;
    }
    LeaveCriticalSection(&g_cs);
    return 1;
}

void ste_sdl_rumble(int slot, int port, unsigned lo, unsigned hi)
{
    PortCfg c; unsigned l, h;
    if (!pad_by_slot(slot)) return;
    EnterCriticalSection(&g_cs);
    cfg_for(port, g_pad[slot].guid, &c);
    l = lo * (unsigned)c.rumble / 100u; h = hi * (unsigned)c.rumble / 100u;
    SDL_RumbleGamepad(g_pad[slot].gp, (Uint16)l, (Uint16)h, (l || h) ? 60000u : 0u);
    LeaveCriticalSection(&g_cs);
}

/* ---------- keyboard ---------- */
void ste_sdl_key(int vk, int down) { if (vk > 0 && vk < 256) g_key[vk] = (unsigned char)down; if (down && g_ovl_vk && vk == g_ovl_vk) { g_ovl_why = 2; g_ovl_port = 0; InterlockedIncrement(&g_ovl_req); fprintf(stderr, "[OVERLAY] request: key 0x%02X\n", vk); fflush(stderr); } }
void ste_sdl_keys_clear(void) { memset(g_key, 0, sizeof g_key); }
static int key_down(int vk)
{
    if (vk == VK_LSHIFT) return g_key[VK_SHIFT] && (GetAsyncKeyState(VK_LSHIFT) & 0x8000);
    if (vk == VK_RSHIFT) return g_key[VK_SHIFT] && (GetAsyncKeyState(VK_RSHIFT) & 0x8000);
    if (vk == VK_LCONTROL) return g_key[VK_CONTROL] && (GetAsyncKeyState(VK_LCONTROL) & 0x8000);
    if (vk == VK_RCONTROL) return g_key[VK_CONTROL] && (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
    if (vk == VK_LMENU) return g_key[VK_MENU] && (GetAsyncKeyState(VK_LMENU) & 0x8000);
    if (vk == VK_RMENU) return g_key[VK_MENU] && (GetAsyncKeyState(VK_RMENU) & 0x8000);
    return vk > 0 && vk < 256 && g_key[vk];
}
/* keyboard bindings of the mapping file applied to the lib's keyboard state `st` (sticks stay on W A S D / I J K L). Returns 1 when key bindings were applied. */
int ste_sdl_keyboard_apply(int port, XBOX_INPUT_STATE *st)
{
    PortCfg c; WORD w = 0; int i;
    if (!g_map) return 0;
    cfg_for(port, NULL, &c);
    if (!c.has_keys) return 0;
    {
        static const struct { int act; WORD bit; } dig[] = { { A_DUP, XBOX_GAMEPAD_DPAD_UP }, { A_DDOWN, XBOX_GAMEPAD_DPAD_DOWN }, { A_DLEFT, XBOX_GAMEPAD_DPAD_LEFT }, { A_DRIGHT, XBOX_GAMEPAD_DPAD_RIGHT },
            { A_START, XBOX_GAMEPAD_START }, { A_BACK, XBOX_GAMEPAD_BACK }, { A_LTHUMB, XBOX_GAMEPAD_LEFT_THUMB }, { A_RTHUMB, XBOX_GAMEPAD_RIGHT_THUMB } };
        for (i = 0; i < 8; ++i) if ((c.src[dig[i].act] >> 16) == K_KEY && key_down(c.src[dig[i].act] & 0xFFFF)) w |= dig[i].bit;
        st->Gamepad.wButtons = w;
        for (i = 0; i < 8; ++i) st->Gamepad.bAnalogButtons[i] = ((c.src[i] >> 16) == K_KEY && key_down(c.src[i] & 0xFFFF)) ? 255 : 0;   /* A B X Y Black White LT RT */
    }
    return 1;
}

/* Back+Start held on the same port for g_chord_ms (call with the port's final state each poll) */
void ste_sdl_chord_check(int port, const XBOX_INPUT_STATE *st)
{
    static DWORD since[4]; static int fired[4]; DWORD now = GetTickCount();
    if (!g_ovl_chord || port < 0 || port > 3) return;
    if ((st->Gamepad.wButtons & (XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START)) == (XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START)) {
        if (!since[port]) since[port] = now ? now : 1;
        else if (!fired[port] && now - since[port] >= g_chord_ms) { fired[port] = 1; g_ovl_why = 3; g_ovl_port = port; InterlockedIncrement(&g_ovl_req); fprintf(stderr, "[OVERLAY] request: Back+Start chord (port %d)\n", port + 1); fflush(stderr); }
    } else { since[port] = 0; fired[port] = 0; }
}
/* overlay requests since the last call (0 = none); *why = 1 guide, 2 key, 3 chord; *port = Xbox port 0..3 */
int ste_overlay_poll(int *why, int *port)
{
    static LONG seen; LONG n = g_ovl_req;
    if (n == seen) return 0;
    seen = n; if (why) *why = (int)g_ovl_why; if (port) *port = (int)g_ovl_port;
    return 1;
}
