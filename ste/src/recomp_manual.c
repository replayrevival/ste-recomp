/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <stdint.h>
#include <xbox/xboxrecomp.h>
#include "xinput_xbox.h"
#include "ste_sdlinput.h"   /* c248: SDL3 gamepad backend, launcher mapping file, overlay hotkeys */
#include "d3d8_video.h"

/* ?????? ICALL trace ring buffer ??????????????????????????????????????????????????????????????????????????????????????????????????? */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ?????? Register state (defined in xbox_memory_layout.c) ???????????????????????? */

extern RECOMP_TLS uint32_t g_eax, g_esp, g_ecx, g_edx;
extern ptrdiff_t g_xbox_mem_offset;

/* ?????? Manual function overrides ????????????????????????????????????????????????????????????????????????????????????????????? */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
extern void sub_00015512(void);
static void wrap_00015512(void)
{
    /* debug trace: only when STE_STARTUP_TRACE=<file> is set; release runs write nothing */
    const char *tp = getenv("STE_STARTUP_TRACE");
    FILE *f = (tp && *tp) ? fopen(tp, "a") : NULL;
    if (f) { fprintf(f, "ENTER 0x00015512 tid=%lu\n", (unsigned long)GetCurrentThreadId()); fflush(f); fclose(f); }
    sub_00015512();
    f = (tp && *tp) ? fopen(tp, "a") : NULL;
    if (f) { fprintf(f, "EXIT 0x00015512 tid=%lu eax=0x%08X\n", (unsigned long)GetCurrentThreadId(), g_eax); fflush(f); fclose(f); }
}

/* ======================================================================
 * XPP (USB / XID) host overrides -- mission-l6 candidate3, 2026-10-16
 *
 * The XDK XPP library drives the OHCI host controller at 0xFED00000 directly
 * (XInitDevices -> sub_002D85EA stores the base), which is zeroed memory here.
 * These replace the public entry points. VAs are XDK 5849 signature matches from
 * XbSymbolDatabase (see RESUME_PACKET.md); stdcall `ret N` sizes were read from the
 * XBE bytes. They are DEFINED here, so the recompiler runs with --exclude-manual and
 * does not generate their bodies; direct callers dispatch through recomp_lookup_manual
 * / recomp_lookup and land on these.
 * ====================================================================== */
extern uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);
extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

#define G32(a)  (*(volatile uint32_t *)((uintptr_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define GPTR(a) ((void *)((uintptr_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define XPP_GAMEPAD_TYPE   0x002D7ABCu   /* device-type record the game passes to XInputOpen/XGetDevices */
#define XPP_NTSETEVENT_VA  (0xFE000000u + 36u * 4u)  /* kernel thunk slot 36 = ordinal 225 NtSetEvent in this XBE */

static void xpp_seen(const char *name, int *n)
{
    if ((*n)++ < 3) { fprintf(stderr, "[XPP] %s called\n", name); fflush(stderr); }
}

/* stdcall return: result in eax, pop the return address plus `argbytes` of arguments */
static void xpp_ret(uint32_t argbytes, uint32_t val) { g_eax = val; g_esp += 4 + argbytes; }

/* Signal a guest event token exactly as a guest call to NtSetEvent would: push the args and a
 * dummy return address and enter the kernel thunk dispatcher. */
static void xpp_signal(uint32_t hevent)
{
    recomp_func_t f; uint32_t saved_esp = g_esp, saved_eax = g_eax;
    if (!hevent) return;
    f = recomp_lookup_kernel(XPP_NTSETEVENT_VA);
    if (!f) return;
    g_esp -= 4; G32(g_esp) = 0;          /* PreviousState = NULL */
    g_esp -= 4; G32(g_esp) = hevent;     /* EventHandle */
    g_esp -= 4; G32(g_esp) = 0;          /* dummy return address */
    f();
    g_esp = saved_esp; g_eax = saved_eax;
}

void sub_002D803E(void) { static int n; xpp_seen("XInitDevices", &n); xpp_ret(8, 0); }            /* (count, types) ret 8 */

/* ======================================================================
 * c192: multi-port XInput (local multiplayer). The XDK device APIs are emulated per Xbox port 0..3:
 *   XGetDevices(type)                  -> bitmask of connected ports (bit n = port n)
 *   XGetDeviceChanges(type,*ins,*rem)  -> insertions/removals since the previous call (the first call reports every present pad)
 *   XInputOpen/Close(type,port,...)    -> one handle per connected port (NULL if nothing is connected there)
 *   XInputGetState/SetState(handle,..) -> the host pad mapped to that port; packet number per port; rumble via XINPUT_FEEDBACK
 * Port sources (ste_enhance.ini [input] port1..port4, env STE_INPUT_PORT1..4):
 *   auto (default) = the Nth connected host XInput pad not claimed by an explicit mapping (first connected host pad = port 0, ...); port 0 falls back to the keyboard/mouse
 *   xinput0..xinput3 = that host pad;  keyboard = keyboard/mouse;  none = disconnected.
 * The keyboard is merged into port 0 on top of its pad as before. Test pads without hardware: STE_VIRTUAL_PADS=<hex port mask> (neutral pads) and pad-script entries
 * addressed to a port ("p2:120:A" = port 1; entries without a prefix are port 0; a port with script entries is connected as a virtual pad).
 * Host connectivity is re-probed at most every 500 ms (XInputGetState on an absent user is slow), so plugging/unplugging is noticed within ~0.5 s.
 * ====================================================================== */
#define STE_NPORTS 4
#define STE_SRC_NONE (-1)
#define STE_SRC_KEYB 4
#define STE_SRC_VIRT 5
#define STE_SRC_SDL 16   /* c248: 16 + SDL pad slot */
static int g_sdl;        /* SDL3 backend active (STE_INPUT_BACKEND / ini [input] backend; XInput otherwise) */
extern DWORD WINAPI XInputGetState(DWORD dwUserIndex, void *pState);   /* xinput.lib is already linked by the input lib; only the connectivity probe is used here */
extern int xbox_InputKeyboardPoll(XBOX_INPUT_STATE *out);
extern int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState);
extern DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration);

static struct { int mode; int xi; } g_pcfg[STE_NPORTS];   /* mode: 0 auto, 1 xinput<xi>, 2 keyboard, 3 none */
static int g_pcfg_init, g_nohost, g_vpad0;
static uint32_t g_vpad_mask;                               /* ports connected as virtual neutral pads */
static int g_port_src[STE_NPORTS] = { STE_SRC_KEYB, STE_SRC_NONE, STE_SRC_NONE, STE_SRC_NONE };
static uint32_t g_port_conn = 1u, g_port_rep;              /* connected mask; mask reported by the previous XGetDeviceChanges */
static uint32_t g_port_handle[STE_NPORTS];
static ULONGLONG g_port_probe_t;
static volatile int g_host_conn[4];
static volatile LONG g_probe_started;
static double g_vwin[STE_NPORTS][4][2]; static int g_vwin_on[STE_NPORTS];   /* STE_VPAD_WINDOWS="port:t0-t1;port:t0-t1..." (ports 1..4, up to 4 windows per port, seconds since process start): virtual pad plugged in only inside a window (hot-plug test) */
static double ste_proc_age(void);

/* c243: one resolver for ste_enhance.ini: env STE_ENHANCE_INI (set by the launcher) else <exe dir>\ste_enhance.ini. Logs the choice once so a run shows which file was used. */
static void ste_enhance_ini_path(char *ini)
{
    static int logged; const char *iv = getenv("STE_ENHANCE_INI"); char *p;
    if (iv && *iv) { strncpy(ini, iv, MAX_PATH - 1); ini[MAX_PATH - 1] = 0; }
    else { GetModuleFileNameA(NULL, ini, MAX_PATH); p = strrchr(ini, 0x5C); if (p) strcpy(p + 1, "ste_enhance.ini"); }
    if (!logged) { logged = 1; fprintf(stderr, "[CONFIG] ste_enhance.ini=%s (%s) exists=%d\n", ini, (iv && *iv) ? "env STE_ENHANCE_INI" : "exe dir", GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES); fflush(stderr); }
}
static void ste_inp_cfg(void)
{
    char ini[MAX_PATH]; int i; const char *e;
    if (g_pcfg_init) return;
    g_pcfg_init = 1;
    ste_enhance_ini_path(ini);
    for (i = 0; i < STE_NPORTS; ++i) {
        char key[16], en[24], v[32] = "auto";
        sprintf(key, "port%d", i + 1); sprintf(en, "STE_INPUT_PORT%d", i + 1);
        e = getenv(en);
        if (e && *e) { strncpy(v, e, sizeof v - 1); v[sizeof v - 1] = 0; } else GetPrivateProfileStringA("input", key, "auto", v, sizeof v, ini);
        if (!_strnicmp(v, "xinput", 6) && v[6] >= 0x30 && v[6] <= 0x33) { g_pcfg[i].mode = 1; g_pcfg[i].xi = v[6] - 0x30; }
        else if (!_stricmp(v, "keyboard")) g_pcfg[i].mode = 2;
        else if (!_stricmp(v, "none")) g_pcfg[i].mode = 3;
        else g_pcfg[i].mode = 0;
    }
    e = getenv("STE_NO_HOST_PAD"); g_nohost = e && *e && *e != 0x30;
    e = getenv("STE_VIRTUAL_PAD"); g_vpad0 = e && *e && *e != 0x30; if (g_vpad0) { g_nohost = 1; g_vpad_mask |= 1u; }
    if (!g_nohost) g_sdl = ste_sdl_init();   /* c248: SDL3 gamepads (Xbox / PlayStation / Switch / generic), XInput as the fallback */
    if (g_nohost) ste_buttons_resolve(0);   /* c355: no SDL host pad: auto stays xbox_original */
    e = getenv("STE_VIRTUAL_PADS"); if (e && *e) g_vpad_mask |= (uint32_t)strtoul(e, NULL, 0) & 0xFu;
    e = getenv("STE_VPAD_WINDOWS");
    if (e && *e) { const char *q = e; while (*q) { int pn; double a, b; if (sscanf(q, "%d:%lf-%lf", &pn, &a, &b) == 3 && pn >= 1 && pn <= 4 && g_vwin_on[pn - 1] < 4) { int k = g_vwin_on[pn - 1]++; g_vwin[pn - 1][k][0] = a; g_vwin[pn - 1][k][1] = b; } while (*q && *q != 0x3B) q++; if (*q) q++; } }
    fprintf(stderr, "[XPP] input ports: port1=%d/%d port2=%d/%d port3=%d/%d port4=%d/%d (mode 0 auto,1 xinput,2 keyboard,3 none) nohost=%d vpad_mask=0x%X\n", g_pcfg[0].mode, g_pcfg[0].xi, g_pcfg[1].mode, g_pcfg[1].xi,
            g_pcfg[2].mode, g_pcfg[2].xi, g_pcfg[3].mode, g_pcfg[3].xi, g_nohost, g_vpad_mask); fflush(stderr);
}

/* Host pad connectivity is probed on a helper thread (XInputGetState on an absent user can take milliseconds; the guest's frame thread must never wait for it). */
static DWORD WINAPI ste_pad_probe_thread(LPVOID arg)
{
    (void)arg;
    for (;;) { int i; for (i = 0; i < 4; ++i) { unsigned char tmp[32]; g_host_conn[i] = !g_nohost && XInputGetState((DWORD)i, tmp) == 0; } Sleep(500); }
    return 0;
}

/* resolve the port->source map */
static void ste_port_refresh(int force)
{
    ULONGLONG now = GetTickCount64(); int i, p, claimed = 0, next = 0, src[STE_NPORTS]; uint32_t conn = 0; static int first = 1;
    ste_inp_cfg();
    if (g_sdl) {   /* c248: SDL3 backend: ports resolve against SDL pads (mapping file device > ini/env mode); xinputN = N-th connected pad */
        unsigned claimed = 0; int kind[STE_NPORTS], s2, sdlsrc[STE_NPORTS]; const char *dev[STE_NPORTS];
        ste_sdl_update();
        for (p = 0; p < STE_NPORTS; ++p) {
            dev[p] = ste_sdl_map_device(p); sdlsrc[p] = STE_SRC_NONE;
            if (dev[p]) kind[p] = !_stricmp(dev[p], "auto") ? 0 : !_stricmp(dev[p], "keyboard") ? 2 : !_stricmp(dev[p], "none") ? 3 : 4;
            else kind[p] = g_pcfg[p].mode == 1 ? 1 : g_pcfg[p].mode;
        }
        for (p = 0; p < STE_NPORTS; ++p) {
            if (kind[p] == 4) { s2 = ste_sdl_pick(claimed, dev[p]); if (s2 >= 0) { claimed |= 1u << s2; sdlsrc[p] = STE_SRC_SDL + s2; } }
            else if (kind[p] == 1) { s2 = ste_sdl_nth(g_pcfg[p].xi); if (s2 >= 0) { claimed |= 1u << s2; sdlsrc[p] = STE_SRC_SDL + s2; } }
        }
        for (p = 0; p < STE_NPORTS; ++p) {
            int s = sdlsrc[p];
            if (kind[p] == 2) s = STE_SRC_KEYB;
            else if (kind[p] == 0) { s2 = ste_sdl_pick(claimed, NULL); if (s2 >= 0) { claimed |= 1u << s2; s = STE_SRC_SDL + s2; } else if (p == 0) s = STE_SRC_KEYB; }
            if (s == STE_SRC_NONE && g_vwin_on[p]) { double age = ste_proc_age(); int k; for (k = 0; k < g_vwin_on[p]; ++k) if (age >= g_vwin[p][k][0] && age < g_vwin[p][k][1]) s = STE_SRC_VIRT; }
            else if (s == STE_SRC_NONE && ((g_vpad_mask >> p) & 1u)) s = STE_SRC_VIRT;
            src[p] = s; if (s != STE_SRC_NONE) conn |= 1u << p;
        }
        if (first || memcmp(src, g_port_src, sizeof src)) {
            fprintf(stderr, "[XPP] port map: p0=%d p1=%d p2=%d p3=%d (16+n SDL pad slot n, 0..3 host XInput user, 4 keyboard, 5 virtual, -1 none) connected=0x%X\n", src[0], src[1], src[2], src[3], conn); fflush(stderr);
            memcpy(g_port_src, src, sizeof src); g_port_conn = conn;
        }
        first = 0; return;
    }
    if (!g_nohost && InterlockedCompareExchange(&g_probe_started, 1, 0) == 0) {
        for (i = 0; i < 4; ++i) { unsigned char tmp[32]; g_host_conn[i] = XInputGetState((DWORD)i, tmp) == 0; }   /* first answer synchronously so the first XGetDevices is right */
        CloseHandle(CreateThread(NULL, 0, ste_pad_probe_thread, NULL, 0, NULL));
    }
    (void)force; (void)now;
    for (p = 0; p < STE_NPORTS; ++p) if (g_pcfg[p].mode == 1) claimed |= 1 << g_pcfg[p].xi;
    for (p = 0; p < STE_NPORTS; ++p) {
        int s = STE_SRC_NONE;
        if (g_pcfg[p].mode == 1) s = g_host_conn[g_pcfg[p].xi] ? g_pcfg[p].xi : STE_SRC_NONE;
        else if (g_pcfg[p].mode == 2) s = STE_SRC_KEYB;
        else if (g_pcfg[p].mode == 0) {
            while (next < 4 && (!g_host_conn[next] || (claimed & (1 << next)))) next++;
            if (next < 4) s = next++;
            else if (p == 0) s = STE_SRC_KEYB;                  /* the keyboard stands in for an absent pad on port 0 */
        }
        if (s == STE_SRC_NONE && g_vwin_on[p]) { double age = ste_proc_age(); int k; for (k = 0; k < g_vwin_on[p]; ++k) if (age >= g_vwin[p][k][0] && age < g_vwin[p][k][1]) s = STE_SRC_VIRT; }
        else if (s == STE_SRC_NONE && ((g_vpad_mask >> p) & 1u)) s = STE_SRC_VIRT;
        src[p] = s; if (s != STE_SRC_NONE) conn |= 1u << p;
    }
    if (first || memcmp(src, g_port_src, sizeof src)) {
        fprintf(stderr, "[XPP] port map: p0=%d p1=%d p2=%d p3=%d (0..3 host XInput user, 4 keyboard, 5 virtual, -1 none) connected=0x%X\n", src[0], src[1], src[2], src[3], conn); fflush(stderr);
        memcpy(g_port_src, src, sizeof src); g_port_conn = conn;
    }
    first = 0;
}

/* handle -> port (-1 if unknown) */
static int ste_port_of_handle(uint32_t h)
{
    int p; for (p = 0; p < STE_NPORTS; ++p) if (g_port_handle[p] && g_port_handle[p] == h) return p;
    return -1;
}

/* XGetDevices(type) ret 4 */
void sub_002D8E87(void)
{
    static int n; xpp_seen("XGetDevices", &n);
    ste_port_refresh(0);
    xpp_ret(4, G32(g_esp + 4) == XPP_GAMEPAD_TYPE ? g_port_conn : 0u);
}

/* XGetDeviceChanges(type,*ins,*rem) ret 0xC */
void sub_002D8EA9(void)
{
    static int n; uint32_t t, pi, pr, ins = 0, rem = 0, r = 0;
    xpp_seen("XGetDeviceChanges", &n);
    t = G32(g_esp + 4); pi = G32(g_esp + 8); pr = G32(g_esp + 12);
    ste_port_refresh(0);
    if (t == XPP_GAMEPAD_TYPE) {
        ins = g_port_conn & ~g_port_rep; rem = g_port_rep & ~g_port_conn; g_port_rep = g_port_conn; r = (ins | rem) ? 1u : 0u;
        if (r) { fprintf(stderr, "[XPP] XGetDeviceChanges: insertions=0x%X removals=0x%X (connected 0x%X)\n", ins, rem, g_port_conn); fflush(stderr); }
    }
    if (pi) G32(pi) = ins;
    if (pr) G32(pr) = rem;
    xpp_ret(12, r);
}

/* XInputOpen(type,port,slot,poll) ret 0x10: one handle per connected port, NULL when nothing is there */
void sub_002D8B44(void)
{
    static int n; uint32_t t, port, r = 0;
    xpp_seen("XInputOpen", &n);
    t = G32(g_esp + 4); port = G32(g_esp + 8);
    ste_port_refresh(0);
    if (t == XPP_GAMEPAD_TYPE && port < STE_NPORTS && ((g_port_conn >> port) & 1u)) {
        if (!g_port_handle[port]) g_port_handle[port] = xbox_ContiguousAlloc(32, 16);
        r = g_port_handle[port];
    }
    xpp_ret(16, r);
}

void sub_002D8B9A(void) { static int n; xpp_seen("XInputClose", &n); xpp_ret(4, 0); }               /* (handle) ret 4; handles are per port and reused */

/* XInputGetCapabilities(handle, caps) ret 8 */
void sub_002D8BA6(void)
{
    static int n; uint32_t c; xpp_seen("XInputGetCapabilities", &n);
    c = G32(g_esp + 8);
    if (c) {
        uint8_t *p = (uint8_t *)GPTR(c); int i;
        memset(p, 0, 25);                         /* the original zeroes 6 dwords + 1 byte */
        p[0] = 1;                                 /* SubType = XINPUT_DEVSUBTYPE_GAMEPAD */
        /* In.Gamepad at +4 (natural layout, ASSUMED): every digital/analog button and axis supported */
        p[4] = 0xFF; p[5] = 0x00;                 /* wButtons mask 0x00FF */
        for (i = 0; i < 8; i++) p[6 + i] = 0xFF;  /* bAnalogButtons */
        for (i = 14; i < 22; i++) p[i] = 0xFF;    /* sThumbLX/LY/RX/RY = 0xFFFF */
        for (i = 22; i < 26; i++) p[i] = 0xFF;    /* c192: Out.Rumble wLeft/wRightMotorSpeed = 0xFFFF: a gamepad with both motors (the title's caps buffer is 26 bytes: it is followed by the handle at +0xC8 of its pad record) */
    }
    xpp_ret(8, 0);
}

/* XInputGetState(handle, pState) ret 8
 * STE c34: passes the first connected host XInput pad through (ports 0..3, first that answers; port 0 also
 * answers for the keyboard fallback in xbox_InputGetState). Button mapping is the lib's (xinput_device.c):
 * d-pad/Start/Back/thumbs in wButtons, A/B/X/Y/LB(black)/RB(white)/triggers in bAnalogButtons, sticks as-is. */
/* c92: ste_enhance.ini [enhance] unlock_all_monsters / unlock_all_cities / unlock_all_challenges (0/1, default 0; env STE_ENHANCE_<KEY>).
 * The title's cheat-code handler (ShellData\mainmenu.bsf doCheatCode -> setShellCheats/setGameCheats) accumulates the unlocks in the word at 0x0042D310:
 * snapshots (tmp\q1\cheatsnap) read 0 -> 0x082 -> 0x186 -> 0x1A6 after 525955 / 659996 / 975013, i.e. monsters = 0x082 (VERIFIED: the full 18-monster
 * Versus grid appears with only this OR-ed in), cities = 0x104, challenges = 0x020 (both inferred from the snapshot deltas, NOT yet verified in the UI).
 * We OR the selected bits in from the pad poll; nothing is written to the save by us (the game saves them only if its own profile save does). */
static void ste_tune_poll(void);   /* c112: defined with the camera hook below */
static int ste_enh_int(const char *key, const char *envname)
{
    char ini[MAX_PATH]; const char *e = getenv(envname);
    if (e && *e) return atoi(e);
    ste_enhance_ini_path(ini);
    return (int)GetPrivateProfileIntA("enhance", key, 0, ini);
}
static uint32_t g_ste_unlock_mask; static int g_ste_unlock_init;
/* c193 (goal 3c, STEM gameplay toggles, ste_enhance.ini [enhance] / env STE_ENHANCE_<KEY>, default 0 = original STE). Credit: STEM / NastySoft pnach (docs\community\STEM_TOGGLES.md).
 *   stem_no_min_health      'Remove minimum health limit / Allows throws and military weapons to KO monsters': sub_00112AC0 (PS2 0x003BF7EC) caps a hit so health stays >= 0.025 * X; the 0.025f constant at 0x00313050
 *                           is used only there, STEM zeroes it -> we write 0.0f to it.
 *   stem_no_knockback_scaling 'Remove knockback scaling': sub_0010D2E0 (PS2 0x003C2260/78) esi-- / esi++ gated in the patched chunk candidate193\generated\recomp_0008.c (tools\mk_stem_chunk.py)
 *   stem_no_juggle_limit    'Remove the juggle limit': sub_0011E490 (PS2 0x003ABA5C) juggle counter [monster+0x938] increment gated likewise */
int g_ste_stem_noknock, g_ste_stem_nojuggle, g_ste_stem_nodim, g_ste_stem_veh, g_ste_stem_vknock, g_ste_stem_size, g_ste_stem_size_slots = 0xF, g_ste_stem_shake, g_ste_stem_rage, g_ste_stem_empty; static int g_ste_stem_nominhp;
/* c193 logging probe (STE_STEM_LOG=1, no behaviour change): counts from the patched chunk. k: 0/1 knockback class -1/+1 applied-or-skipped sites, 2 juggle increments (v = counter after),
 * 3 health-floor: hit not clamped, 4 floor below zero, 5 hit clamped to the floor. Printed every 5 s from the pad poll as "[STEM] ...". */
static unsigned g_stem_cnt[6], g_stem_skip[2], g_stem_jug[8]; static int g_stem_log = -1; static uint32_t g_stem_mon[4];
void ste_stem_note(int k, uint32_t v)
{
    if (g_stem_log < 0) { const char *e = getenv("STE_STEM_LOG"); g_stem_log = e && *e && *e != 0x30; }
    if (!g_stem_log) return;
    if ((unsigned)k < 2u && !v) g_stem_skip[k]++; else if ((unsigned)k < 6u) g_stem_cnt[k]++;
    if (k == 6) { static int nraw; if (nraw < 6 && v != g_stem_mon[0]) { nraw++; fprintf(stderr, "[STEM] entry ecx=0x%08X\n", v); fflush(stderr); } }
    if (k == 6 && v > 0x10000u && v < 0x90000000u) { int i; for (i = 0; i < 4; ++i) { if (g_stem_mon[i] == v) break; if (!g_stem_mon[i]) { g_stem_mon[i] = v; break; } } }
    if (k == 2) g_stem_jug[v > 7u ? 7u : v]++;
}
/* c198: the STEM gates are instruction hooks now (hooks\ste_hooks.toml -> the generator emits these calls into sub_0010D2E0 / sub_0011E490 / sub_00112AC0;
 * docs\formats\RECOMP_HOOKS.md). replace-hooks return non-zero to skip the original instruction. */
extern RECOMP_TLS uint32_t g_esi, g_ebx;   /* callee-saved register globals live in recomp_funcs.h; the hooks read them */
int ste_hook_knock_dec(void)  { ste_stem_note(0, !g_ste_stem_noknock); return g_ste_stem_noknock; }   /* esi-- of the knockback class */
int ste_hook_knock_inc(void)  { ste_stem_note(1, !g_ste_stem_noknock); return g_ste_stem_noknock; }   /* esi++ */
static unsigned g_stem_cls[10];   /* c201 probe: hit class [esi] after the size adjustments of sub_0010D2E0 (index 0 = below zero, 1..8 = class 0..7, 9 = 8+) */
int ste_hook_hitclass(void) { int c = (int)g_esi + 1; g_stem_cls[c < 0 ? 0 : (c > 9 ? 9 : c)]++; return 0; }
int ste_hook_juggle(void)     { return g_ste_stem_nojuggle; }                                              /* inc [monster+0x938] */
int ste_hook_juggle_after(void) { ste_stem_note(2, G32(g_esi + 0x938u)); return 0; }
int ste_hook_floor_entry(void)     { ste_stem_note(6, g_ecx); return 0; }
int ste_hook_floor_unclamped(void) { ste_stem_note(3, 0); return 0; }
int ste_hook_floor_neg(void)       { ste_stem_note(4, 0); return 0; }
int ste_hook_floor_clamped(void)   { ste_stem_note(5, 0); return 0; }
/* c199 hooks (hooks\ste_hooks.toml). stem_no_results_dim: sub_000E0F00 pushes the dim alpha (0.6 / 0.4) right before `call [vtbl+0x68]`; zero it. */
static unsigned g_stem_nodim_calls, g_stem_veh_max, g_stem_veh_limit, g_stem_veh_forced, g_stem_vk_nonwpn, g_stem_vk_nonzero;
int ste_hook_nodim(void) { if (g_ste_stem_nodim) { G32(g_esp) = 0u; g_stem_nodim_calls++; } return 0; }
/* stem_more_ai_vehicles: sub_000EA940 spawns only if the cooldown [mgr+0x78] <= 0 (x87 status bits tested after fnstsw); force them "elapsed" so a vehicle spawns every update until count [mgr+0x20] reaches the limit [0x00442508] */
int ste_hook_veh_force(void) { if (g_ste_stem_veh) { g_eax = (g_eax & 0xFFFF00FFu) | 0x0100u; g_stem_veh_forced++; } return 0; }
int ste_hook_veh_log(void) { unsigned c = G32(g_esi + 0x20u); if (c > g_stem_veh_max) g_stem_veh_max = c; g_stem_veh_limit = G32(0x00442508u); return 0; }
/* stem_weapon_vertical_knockback: sub_0016D460 copies the hit's vertical component [src+0x4C] into the knockback vector [esp+0x48] only when [src+0x88] == 2 (weapon); do it for every type.
 * ebp is a local in that function: src = [esi+8]. */
int ste_hook_vknock(void)
{
    uint32_t src = G32(g_esi + 8u);
    if (G32(src + 0x88u) != 2u) {
        g_stem_vk_nonwpn++; if (G32(src + 0x4Cu)) g_stem_vk_nonzero++;
        if (g_ste_stem_vknock) G32(g_esp + 0x48u) = G32(src + 0x4Cu);
    }
    return 0;
}
/* c200 stem_rule_player_size (STEM Rule 21 Player Size, percent of normal size, 0 = off): sub_00121A20 (per-frame monster update, esi = monster) is hooked at 0x00121B4B, just before it derives the
 * scaled flag [m+0x88D] from |1.0 - [m+0x890]|; we overwrite the live size scale [m+0x890] there. [m+0xDC] = player slot; ste_enhance.ini stem_rule_player_size_slots = bit mask of slots (default 0xF = all). */
static unsigned g_stem_size_calls; static uint32_t g_stem_size_last;
/* c202 stem_rule_rage_meter (STEM Rule 16): the game already has rage gain rates in the stat record [m+0xCEB0]: +0x1C4 per damage received, +0x1C0 per damage dealt, +0x1BC per second (all 0 in
 * stock STE, so the meter [m+0x8C4] never fills; sub_00114880 adds damage * [rec+0x1C4]). STEM writes 0.003 / 0.003 / 0.01 there by mode: 1 received, 2 dealt, 3 both, 4 over time, 5 all three.
 * We write them from the per-frame monster hook below. Probe: per-slot meter / in-rage flag. */
static float g_rage_meter[4], g_rage_max[4]; static unsigned g_rage_inrage[4], g_rage_calls;
static void ste_rage_tick(void)
{
    uint32_t slot = G32(g_esi + 0xDCu), rec; union { float f; uint32_t u; } c; static int once;
    if (slot > 3u) return;
    { union { float f; uint32_t u; } m; m.u = G32(g_esi + 0x8C4u); g_rage_meter[slot] = m.f; if (m.f > g_rage_max[slot]) g_rage_max[slot] = m.f; if (G32(g_esi + 0x8D0u) & 0xFFu) g_rage_inrage[slot]++; }
    if (g_ste_stem_rage <= 0) return;
    rec = G32(g_esi + 0xCEB0u);
    if (rec < 0x10000u || rec >= 0x90000000u) return;
    g_rage_calls++;
    if (g_ste_stem_rage == 1 || g_ste_stem_rage == 3 || g_ste_stem_rage == 5) { c.f = 0.003f; G32(rec + 0x1C4u) = c.u; }
    if (g_ste_stem_rage == 2 || g_ste_stem_rage == 3 || g_ste_stem_rage == 5) { c.f = 0.003f; G32(rec + 0x1C0u) = c.u; }
    if (g_ste_stem_rage == 4 || g_ste_stem_rage == 5) { c.f = 0.01f; G32(rec + 0x1BCu) = c.u; }
    if (!once) { once = 1; fprintf(stderr, "[STEM] rage rates written to record 0x%08X of monster 0x%08X (slot %u)\n", rec, g_esi, slot); fflush(stderr); }
}
/* HUD (PS2 0x0043383C): while not in rage, show meter * 30 in the rage-timer field of the HUD record being built ([esp+0x30], copied from [m+0x8CC] just before). */
static unsigned g_rage_hud_calls; static float g_rage_hud_last;
/* the getter the HUD really uses (sub_00102270, hook 0x0010228C after `mov [eax+4], edx`): eax = out record, ecx = monster */
static unsigned g_rage_hud2_calls, g_rage_hud2_over;
int ste_hook_rage_hud2(void)
{
    g_rage_hud2_calls++;
    if (g_ste_stem_rage > 0 && !(G32(g_ecx + 0x8D0u) & 0xFFu)) { union { float f; uint32_t u; } m, o; m.u = G32(g_ecx + 0x8C4u); o.f = m.f * 30.0f; G32(g_eax + 4u) = o.u; g_rage_hud2_over++; g_rage_hud_last = o.f; }
    return 0;
}
int ste_hook_rage_hud(void)
{
    if (g_ste_stem_rage > 0 && !(G32(g_esi + 0x8D0u) & 0xFFu)) { union { float f; uint32_t u; } m, o; m.u = G32(g_esi + 0x8C4u); o.f = m.f * 30.0f; G32(g_esp + 0x30u) = o.u; g_rage_hud_calls++; g_rage_hud_last = o.f; }
    return 0;
}
/* c202 stem_footstep_camera_shake (PS2 0x003C88B0/CC/E0/E4): footstep shake in sub_0011AC60. The monster is ebx - 0x8A24 (ebp is a local there). */
static unsigned g_shake_calls, g_shake_strafe, g_shake_neg; static float g_shake_sum, g_shake_last;
int ste_hook_shake_gate(void)   /* replaces `fnstsw ax` after `fcomp [1.0]`: AH bit0 (C0) = size < 1.0, 0x40 = equal */
{
    union { float f; uint32_t u; } s;
    if (!g_ste_stem_shake) return 0;
    s.u = G32(g_ebx - 0x8A24u + 0x890u);
    g_eax = (g_eax & 0xFFFF00FFu) | ((s.f > 0.775f) ? 0u : 0x0100u);   /* shake when size > 0.775 (STEM: lui v0,0x3F46) */
    return 1;
}
int ste_hook_shake_state(void)  /* replaces `cmp [[m+0xEC]+0x3C], 0x11` (skip while in that animation state); STEM: never skip */
{
    if (G32(g_eax + 0x3Cu) == 0x11u) g_shake_strafe++;
    return g_ste_stem_shake;
}
int ste_hook_shake_log(void) { union { float f; uint32_t u; } a; a.u = G32(g_esp); g_shake_calls++; g_shake_sum += a.f; g_shake_last = a.f; if (a.f < 0.0f) g_shake_neg++; return 0; }
int ste_hook_size(void)
{
    union { float f; uint32_t u; } c; uint32_t slot;
    ste_rage_tick();
    if (g_ste_stem_size <= 0) return 0;
    slot = G32(g_esi + 0xDCu);
    if (slot > 3u || !((g_ste_stem_size_slots >> slot) & 1)) return 0;
    c.f = (float)g_ste_stem_size / 100.0f; G32(g_esi + 0x890u) = c.u; g_stem_size_calls++; g_stem_size_last = g_esi;
    return 0;
}
static void ste_stem_report(void)
{
    static ULONGLONG t0; ULONGLONG now = GetTickCount64();
    static int ti; static float th, ts; static int tst; int i; union { float f; uint32_t u; } c;
    if (g_stem_log < 0) { const char *e = getenv("STE_STEM_LOG"); g_stem_log = e && *e && *e != 0x30; }
    if (g_stem_log <= 0) return;
    /* harness pokes (c194/c195; unset = nothing is written), every call: STE_STEM_TEST_HP=<float> health [m+0x85C] and STE_STEM_TEST_SCALE=<float> size scale [m+0x890] of the slot-0 (P1) monster,
     * STE_STEM_TEST_STATE=<int> hit-reaction state [m+0x96C] of the slot-1 monster; they provoke hits that need a low-health / resized / airborne target. */
    if (!ti) { const char *a = getenv("STE_STEM_TEST_HP"), *b = getenv("STE_STEM_TEST_SCALE"), *d = getenv("STE_STEM_TEST_STATE"); ti = 1; th = (a && *a) ? (float)atof(a) : 0.0f; ts = (b && *b) ? (float)atof(b) : 0.0f; tst = (d && *d) ? atoi(d) : 0; }
    for (i = 0; i < 4; ++i) { uint32_t m = g_stem_mon[i]; if (!m) continue;
        if (G32(m + 0xDCu) == 0u) { if (th > 0.0f) { c.f = th; G32(m + 0x85Cu) = c.u; } if (ts > 0.0f) { c.f = ts; G32(m + 0x890u) = c.u; } }
        else if (G32(m + 0xDCu) == 1u && tst) G32(m + 0x96Cu) = (uint32_t)tst; }
    if (now - t0 < 5000u) return;
    t0 = now;
    for (i = 0; i < 4; ++i) { uint32_t m = g_stem_mon[i]; union { float f; uint32_t u; } hp, sc;
        if (!m) continue;
        hp.u = G32(m + 0x85Cu); sc.u = G32(m + 0x890u);
        fprintf(stderr, "[STEM] mon %d at 0x%08X slot=%u health=%.2f scale=%.3f scaled_flag=%u juggle=%u state=%u\n", i, m, G32(m + 0xDCu), hp.f, sc.f, (unsigned)(G32(m + 0x88Cu) >> 8) & 0xFFu, G32(m + 0x938u), G32(m + 0x96Cu)); }
    fprintf(stderr, "[STEM] hit class histogram (<0, 0..7, 8+): %u %u %u %u %u %u %u %u %u %u\n", g_stem_cls[0], g_stem_cls[1], g_stem_cls[2], g_stem_cls[3], g_stem_cls[4], g_stem_cls[5], g_stem_cls[6], g_stem_cls[7], g_stem_cls[8], g_stem_cls[9]);
    fprintf(stderr, "[STEM] shake: toggle=%d calls=%u sum=%.3f last=%.3f negative=%u state0x11_seen=%u | rage: mode=%d calls=%u meter=%.3f/%.3f/%.3f/%.3f max=%.3f/%.3f/%.3f/%.3f inrage_frames=%u/%u/%u/%u hud_overrides=%u last=%.2f hud_getter_calls=%u overrides=%u\n", g_ste_stem_shake, g_shake_calls, g_shake_sum, g_shake_last, g_shake_neg, g_shake_strafe, g_ste_stem_rage, g_rage_calls,
            g_rage_meter[0], g_rage_meter[1], g_rage_meter[2], g_rage_meter[3], g_rage_max[0], g_rage_max[1], g_rage_max[2], g_rage_max[3], g_rage_inrage[0], g_rage_inrage[1], g_rage_inrage[2], g_rage_inrage[3], g_rage_hud_calls, g_rage_hud_last, g_rage_hud2_calls, g_rage_hud2_over);
    fprintf(stderr, "[STEM] size: percent=%d slots=0x%X hook_calls=%u last_monster=0x%08X\n", g_ste_stem_size, g_ste_stem_size_slots, g_stem_size_calls, g_stem_size_last);
    fprintf(stderr, "[STEM] veh max_count=%u limit=%u forced=%u | nodim_calls=%u | vertical-knockback: non-weapon hits=%u (vertical!=0: %u)\n", g_stem_veh_max, g_stem_veh_limit, g_stem_veh_forced, g_stem_nodim_calls, g_stem_vk_nonwpn, g_stem_vk_nonzero);
    { uint32_t fb = G32(0x00313050u); union { float f; uint32_t u; } ff; ff.u = fb; fprintf(stderr, "[STEM] health-floor factor @0x00313050 = %.4f\n", ff.f); }
    fprintf(stderr, "[STEM] knock_dec=%u(+%u skipped) knock_inc=%u(+%u skipped) juggle_inc=%u (counter after: 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7+:%u) floor: unclamped=%u neg=%u clamped=%u\n", g_stem_cnt[0], g_stem_skip[0], g_stem_cnt[1], g_stem_skip[1], g_stem_cnt[2],
            g_stem_jug[0], g_stem_jug[1], g_stem_jug[2], g_stem_jug[3], g_stem_jug[4], g_stem_jug[5], g_stem_jug[6], g_stem_jug[7], g_stem_cnt[3], g_stem_cnt[4], g_stem_cnt[5]); fflush(stderr);
}
static void ste_unlock_poll(void)
{
    if (!g_ste_unlock_init) {
        g_ste_unlock_init = 1;
        if (ste_enh_int("unlock_all_monsters", "STE_ENHANCE_UNLOCK_ALL_MONSTERS")) g_ste_unlock_mask |= 0x082u;
        if (ste_enh_int("unlock_all_cities", "STE_ENHANCE_UNLOCK_ALL_CITIES")) g_ste_unlock_mask |= 0x104u;
        if (ste_enh_int("unlock_all_challenges", "STE_ENHANCE_UNLOCK_ALL_CHALLENGES")) g_ste_unlock_mask |= 0x020u;
        if (ste_enh_int("unlock_gallery", "STE_ENHANCE_UNLOCK_GALLERY")) g_ste_unlock_mask |= 0x010u;   /* c117: code 294206 sets shell bit 0x10 = all Gallery items (verified in the UI) */
        if (ste_enh_int("unlock_final_wars", "STE_ENHANCE_UNLOCK_FINAL_WARS")) g_ste_unlock_mask |= 0x040u;   /* c120: code 409014 sets shell bit 0x40 = all Final Wars gallery items (verified in the UI, fe_fw_cmp.png) */
        if (ste_enh_int("unlock_biollante", "STE_ENHANCE_UNLOCK_BIOLLANTE")) g_ste_unlock_mask |= 0x002u;   /* c189: shell bit 0x002 alone (half of code 525955's 0x082) makes the hidden 10th-cell Biollante selectable while the other locked monsters stay '?' (fresh-profile UI test); 0x080 alone only reveals the locked ones as blue disabled tiles */
        g_ste_stem_nominhp = ste_enh_int("stem_no_min_health", "STE_ENHANCE_STEM_NO_MIN_HEALTH"); g_ste_stem_noknock = ste_enh_int("stem_no_knockback_scaling", "STE_ENHANCE_STEM_NO_KNOCKBACK_SCALING"); g_ste_stem_nojuggle = ste_enh_int("stem_no_juggle_limit", "STE_ENHANCE_STEM_NO_JUGGLE_LIMIT");
        g_ste_stem_nodim = ste_enh_int("stem_no_results_dim", "STE_ENHANCE_STEM_NO_RESULTS_DIM"); g_ste_stem_veh = ste_enh_int("stem_more_ai_vehicles", "STE_ENHANCE_STEM_MORE_AI_VEHICLES"); g_ste_stem_vknock = ste_enh_int("stem_weapon_vertical_knockback", "STE_ENHANCE_STEM_WEAPON_VERTICAL_KNOCKBACK");
        g_ste_stem_size = ste_enh_int("stem_rule_player_size", "STE_ENHANCE_STEM_RULE_PLAYER_SIZE"); if (g_ste_stem_size < 0) g_ste_stem_size = 0; if (g_ste_stem_size > 1000) g_ste_stem_size = 1000;
        { char ini[MAX_PATH]; const char *e = getenv("STE_ENHANCE_STEM_RULE_PLAYER_SIZE_SLOTS");
          if (e && *e) g_ste_stem_size_slots = (int)strtoul(e, NULL, 0) & 0xF;
          else { ste_enhance_ini_path(ini);
                 g_ste_stem_size_slots = (int)GetPrivateProfileIntA("enhance", "stem_rule_player_size_slots", 15, ini) & 0xF; } }
        g_ste_stem_shake = ste_enh_int("stem_footstep_camera_shake", "STE_ENHANCE_STEM_FOOTSTEP_CAMERA_SHAKE"); g_ste_stem_rage = ste_enh_int("stem_rule_rage_meter", "STE_ENHANCE_STEM_RULE_RAGE_METER"); g_ste_stem_empty = ste_enh_int("stem_rule_empty_arena", "STE_ENHANCE_STEM_RULE_EMPTY_ARENA"); if (g_ste_stem_empty) { fprintf(stderr, "[ENHANCE] stem_rule_empty_arena=1\n"); fflush(stderr); } if (g_ste_stem_rage < 0 || g_ste_stem_rage > 5) g_ste_stem_rage = 0;
        if (g_ste_stem_shake || g_ste_stem_rage) { fprintf(stderr, "[ENHANCE] stem_footstep_camera_shake=%d stem_rule_rage_meter=%d\n", g_ste_stem_shake, g_ste_stem_rage); fflush(stderr); }
        if (g_ste_stem_size) { fprintf(stderr, "[ENHANCE] stem_rule_player_size=%d%% slots=0x%X\n", g_ste_stem_size, g_ste_stem_size_slots); fflush(stderr); }
        if (g_ste_stem_nominhp || g_ste_stem_noknock || g_ste_stem_nojuggle || g_ste_stem_nodim || g_ste_stem_veh || g_ste_stem_vknock) { fprintf(stderr, "[ENHANCE] STEM toggles: no_min_health=%d no_knockback_scaling=%d no_juggle_limit=%d no_results_dim=%d more_ai_vehicles=%d weapon_vertical_knockback=%d\n", g_ste_stem_nominhp, g_ste_stem_noknock, g_ste_stem_nojuggle, g_ste_stem_nodim, g_ste_stem_veh, g_ste_stem_vknock); fflush(stderr); }
        { const char *rb = getenv("STE_ENHANCE_SHELL_BITS"); if (rb && *rb) g_ste_unlock_mask |= (uint32_t)strtoul(rb, NULL, 0); }   /* c117 debug: raw bits for experiments (e.g. 0x40) */
        fprintf(stderr, "[ENHANCE] unlock mask=0x%03X at 0x0042D310 (monsters=0x082 cities=0x104 challenges=0x020 gallery=0x010)\n", g_ste_unlock_mask);
    }
    if (g_ste_unlock_mask) G32(0x0042D310u) |= g_ste_unlock_mask;
    ste_stem_report();
    {   /* c203 harness probe: STE_RAM_DUMP_AT="s1,s2,..." (seconds after start) + STE_RAM_DUMP_DIR: write the guest globals region 0x00344000..0x004B0000 to <dir>\ram_<s>.bin (used to locate the STEM Rule 19 points structure) */
        static int dinit, dn; static unsigned ds[8]; static ULONGLONG d0; const char *da, *dd;
        if (!dinit) { dinit = 1; da = getenv("STE_RAM_DUMP_AT"); d0 = GetTickCount64(); if (da && *da) { char *b = _strdup(da), *c2 = NULL, *k; for (k = strtok_s(b, ",", &c2); k && dn < 8; k = strtok_s(NULL, ",", &c2)) ds[dn++] = (unsigned)atoi(k); } }
        dd = getenv("STE_RAM_DUMP_DIR");
        if (dn && dd && *dd) { int di; for (di = 0; di < dn; ++di) if (ds[di] && GetTickCount64() - d0 >= (ULONGLONG)ds[di] * 1000u) { char fn[MAX_PATH]; FILE *df; sprintf(fn, "%s\\ram_%u.bin", dd, ds[di]); df = fopen(fn, "wb"); if (df) { fwrite(GPTR(0x00344000u), 1, 0x16C000u, df); fclose(df); fprintf(stderr, "[RAMDUMP] wrote %s\n", fn); fflush(stderr); } ds[di] = 0; } }
    }
    if (g_ste_stem_empty) {   /* c203 stem_rule_empty_arena (STEM Rule 9): STEM zeroes the names of the level-file chunk classes that place objects (PS2 ObjectInstanceFixup .. FluffEffectFixup, 0x005A9980 etc.) so the chunk lookup (repe cmpsb against these strings, e.g. sub at 0x000FA3A2) never matches and the arena loads without them. Xbox name strings: */
        static const uint32_t nm[] = { 0x00314830u /*ObjectInstanceFixup*/, 0x003147D8u /*BuildingInstanceFixup*/, 0x003149ECu /*TransformInstanceFixup*/, 0x00308EECu /*FluffInstanceFixup*/, 0x00314A58u /*NodeInstanceFixup*/, 0x00314AC0u /*FarmInstanceFixup*/, 0x003149B8u /*FluffEffectFixup*/ };
        int ei; for (ei = 0; ei < 7; ++ei) G32(nm[ei]) = 0u; }
    if (g_ste_stem_shake) G32(0x00313234u) = 0x3F872B02u;   /* c202: footstep shake intensity constant (1.0996 -> STEM 1.0560), used only by sub_0011AC60 */
    if (g_ste_stem_nominhp) G32(0x00313050u) = 0u;   /* c193: the 0.025f health-floor factor of sub_00112AC0 -> 0.0f */
    {   /* c190: in-match cheat flags. The game's own cheat codes set bits in cheatflags[1] (0x003F0E1C) / cheatflags[2] (0x003F0E20), what getGameCheats0/1 read
         * (XBOX_CHEAT_MENU.md, code table); cheatflags[0] (0x003F0E18, mirrored at 0x00425BB0) holds the code-entered bit of 072866. STE_ENHANCE_GAME_BITS0/1/2 (hex) OR bits into them
         * from the pad poll (default 0 = nothing). Classification probe for goal 3e; the named ste_enhance.ini toggles come after the per-bit effects are verified. */
        static int ginit; static uint32_t gb0, gb1, gb2;
        if (!ginit) { const char *e0 = getenv("STE_ENHANCE_GAME_BITS0"), *e1 = getenv("STE_ENHANCE_GAME_BITS1"), *e2 = getenv("STE_ENHANCE_GAME_BITS2"); ginit = 1;
            if (e0 && *e0) gb0 = (uint32_t)strtoul(e0, NULL, 0); if (e1 && *e1) gb1 = (uint32_t)strtoul(e1, NULL, 0); if (e2 && *e2) gb2 = (uint32_t)strtoul(e2, NULL, 0);
            /* c191: named in-match cheat toggles (ste_enhance.ini [enhance] / env STE_ENHANCE_<KEY>, default 0). Slot 0 = P1, slot 1 = COM / second player. Evidence: tests\q23_cheatcls.ps1 runs cls190_* (health-bar fill by tests\clsbars.py) */
            if (ste_enh_int("cheat_invulnerable_p1", "STE_ENHANCE_CHEAT_INVULNERABLE_P1")) gb1 |= 0x01u;     /* code 338592: P1 health stays full (cf1_01: 99% for the whole match) */
            if (ste_enh_int("cheat_invulnerable_com", "STE_ENHANCE_CHEAT_INVULNERABLE_COM")) gb1 |= 0x02u;   /* code 259333: slot 1 health stays full (cf1_02) */
            if (ste_enh_int("cheat_super_damage_com", "STE_ENHANCE_CHEAT_SUPER_DAMAGE_COM")) gb1 |= 0x20u;   /* code 927281: slot 1 kills P1 within seconds (cf1_20, 'YOU LOSE' at 3:57) */
            if (ste_enh_int("cheat_red_p1", "STE_ENHANCE_CHEAT_RED_P1")) gb2 |= 0x10u;                       /* code 154044: P1 monster tinted red + red HUD ring (cf2_10) */
            if (ste_enh_int("cheat_red_com", "STE_ENHANCE_CHEAT_RED_COM")) gb2 |= 0x20u;                     /* code 549939: slot 1 monster tinted red (cf2_f0 showed both red) */
            if (gb0 || gb1 || gb2) { fprintf(stderr, "[ENHANCE] game cheat bits: cf0=0x%X cf1=0x%X cf2=0x%X (0x003F0E18/0x00425BB0, 0x003F0E1C, 0x003F0E20)\n", gb0, gb1, gb2); fflush(stderr); } }
        {   /* the script natives (sub_0009F9C0 get/set/clearGameCheats) and the in-match readers (e.g. sub_000F5A50) use the LIVE settings object at [0x004424F4] + 0x4C0/0x4C4/0x4C8;
             * 0x003F0E18.. is the snapshot copy made by sub_00085F00 (struct 0x003F0958, +0x4C0). Write both. */
            uint32_t so = G32(0x004424F4u);
            if (so > 0x10000u && so < 0x04000000u) { if (gb0) G32(so + 0x4C0u) |= gb0; if (gb1) G32(so + 0x4C4u) |= gb1; if (gb2) G32(so + 0x4C8u) |= gb2; }
        }
        if (gb0) { G32(0x003F0E18u) |= gb0; G32(0x00425BB0u) |= gb0; }
        if (gb1) G32(0x003F0E1Cu) |= gb1;
        if (gb2) G32(0x003F0E20u) |= gb2;
    }
}
/* c93: STE_WATCH_CHEAT=1 logs every change of the guest words in 0x0042D2C0..0x0042D3BF (the shell/game cheat flags live here) as "[CHEATW] t=<s> addr old -> new"; logging only. */
static void ste_cheat_watch(void)
{
    static int init, on; static uint32_t prev[64]; int i;
    if (!init) { const char *e = getenv("STE_WATCH_CHEAT"); init = 1; on = e && *e && *e != 0x30; if (on) for (i = 0; i < 64; ++i) prev[i] = G32(0x0042D2C0u + 4u * i); }
    if (!on) return;
    for (i = 0; i < 64; ++i) { uint32_t v = G32(0x0042D2C0u + 4u * i);
        if (v != prev[i]) { FILETIME c, x, k, u, now; ULARGE_INTEGER cu, nu; double age; GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u); GetSystemTimeAsFileTime(&now);
            cu.LowPart = c.dwLowDateTime; cu.HighPart = c.dwHighDateTime; nu.LowPart = now.dwLowDateTime; nu.HighPart = now.dwHighDateTime; age = (double)(nu.QuadPart - cu.QuadPart) / 1e7;
            fprintf(stderr, "[CHEATW] t=%.1f 0x%08X %08X -> %08X\n", age, 0x0042D2C0u + 4u * i, prev[i], v); fflush(stderr); prev[i] = v; } }
}/* c192: pad script entries ("[pN:]t:KEYS[:dur]", N = 1..4 addresses Xbox port N-1, no prefix = port 0) */
static struct { double t, d; unsigned w; unsigned char a[8]; int port; } g_pscr[4096];
static int g_pscr_n, g_pscr_init;
static void ste_pad_script_load(void)
{
    const char *e = getenv("STE_PAD_SCRIPT"); static char big[262144];
    if (g_pscr_init) return;
    g_pscr_init = 1;
    {   /* c86: STE_PAD_SCRIPT_FILE=<file>: same entries separated by commas or newlines (up to 4096), # comments */
        const char *pf = getenv("STE_PAD_SCRIPT_FILE");
        if (pf && *pf) { FILE *sf = fopen(pf, "rb"); if (sf) { size_t n = fread(big, 1, sizeof(big) - 1, sf); size_t i, o = 0; int com = 0; fclose(sf);
            for (i = 0; i < n; ++i) { char c = big[i]; if (c == 0x23) com = 1; if (c == 10 || c == 13) { com = 0; c = 0x2C; } if (!com) big[o++] = c; } big[o] = 0; e = big; } }
    }
    if (e && *e) { char *buf = (char *)malloc(strlen(e) + 1), *tok, *ctx = NULL; strcpy(buf, e);
        for (tok = strtok_s(buf, ",", &ctx); tok && g_pscr_n < 4096; tok = strtok_s(NULL, ",", &ctx)) {
            char keys[32]; double tt, dd = 0.4; int port = 0, k; const char *tp = tok;
            while (*tp == 0x20) tp++;
            if ((tp[0] == 0x70 || tp[0] == 0x50) && tp[1] >= 0x31 && tp[1] <= 0x34 && tp[2] == 0x3A) { port = tp[1] - 0x31; tp += 3; }   /* "p2:" */
            k = sscanf(tp, "%lf:%31[^:]:%lf", &tt, keys, &dd);
            if (k >= 2) { char *c; int j = g_pscr_n; memset(&g_pscr[j], 0, sizeof g_pscr[j]); g_pscr[j].t = tt; g_pscr[j].d = dd; g_pscr[j].port = port;
                for (c = keys; *c; ++c) switch (*c) { case 'S': g_pscr[j].w |= 0x10; break; case 'K': g_pscr[j].w |= 0x20; break; case 'U': g_pscr[j].w |= 0x01; break;
                    case 'D': g_pscr[j].w |= 0x02; break; case 'L': g_pscr[j].w |= 0x04; break; case 'R': g_pscr[j].w |= 0x08; break;
                    case 'A': g_pscr[j].a[0] = 255; break; case 'B': g_pscr[j].a[1] = 255; break; case 'X': g_pscr[j].a[2] = 255; break; case 'Y': g_pscr[j].a[3] = 255; break;
                    case 'b': g_pscr[j].a[4] = 255; break; case 'w': g_pscr[j].a[5] = 255; break; case 'l': g_pscr[j].a[6] = 255; break; case 'r': g_pscr[j].a[7] = 255; break; default: break; }
                if (port) g_vpad_mask |= 1u << port;   /* a port with script entries exists as a virtual pad even with no hardware */
                g_pscr_n++; } }
        fprintf(stderr, "[XPP] pad script: %d entries from STE_PAD_SCRIPT (virtual port mask 0x%X)\n", g_pscr_n, g_vpad_mask); fflush(stderr); }
}

static double ste_proc_age(void)
{
    FILETIME c, x, k, u, now; ULARGE_INTEGER cu, nu;
    GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u); GetSystemTimeAsFileTime(&now);
    cu.LowPart = c.dwLowDateTime; cu.HighPart = c.dwHighDateTime; nu.LowPart = now.dwLowDateTime; nu.HighPart = now.dwHighDateTime;
    return (double)(nu.QuadPart - cu.QuadPart) / 1e7;
}

/* keyboard on top of a pad (same rule as the input lib's keyboard_merge, used when port 0's pad is not host pad 0) */
static void ste_kb_merge(XBOX_INPUT_STATE *s)
{
    XBOX_INPUT_STATE kb; int i;
    if (!xbox_InputKeyboardPoll(&kb)) return;
    if (g_sdl) ste_sdl_keyboard_apply(0, &kb);   /* c248: key bindings of the mapping file */
    s->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++) if (kb.Gamepad.bAnalogButtons[i] > s->Gamepad.bAnalogButtons[i]) s->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) s->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) s->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) s->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) s->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
}

void sub_002D8D7E(void)
{
    static int n, press_init; static double press_at = -1; static int pressed_logged; static uint32_t pkt[STE_NPORTS], last_btn[STE_NPORTS] = { ~0u, ~0u, ~0u, ~0u }; static int last_src[STE_NPORTS] = { -9, -9, -9, -9 };
    uint32_t h, ps; XBOX_INPUT_STATE st; int port, src;
    xpp_seen("XInputGetState", &n);
    h = G32(g_esp + 4); ps = G32(g_esp + 8);
    ste_pad_script_load();
    ste_port_refresh(0);
    port = ste_port_of_handle(h);
    if (port < 0) port = 0;                     /* unknown handle: treat as port 0 (the original single-pad behaviour) */
    if (port == 0 || !((g_port_conn >> 0) & 1u)) { ste_unlock_poll(); ste_cheat_watch(); ste_tune_poll(); }   /* once per frame: P1's poll (c112 camera live-tune is also polled here) */
    src = g_port_src[port];
    if (src == STE_SRC_NONE) { xpp_ret(8, 1167u); return; }   /* ERROR_DEVICE_NOT_CONNECTED */
    memset(&st, 0, sizeof st);
    if (src >= 0 && src <= 3) {
        if (xbox_InputGetState((DWORD)src, &st) != 0) { g_host_conn[src] = 0; memset(&st, 0, sizeof st); }   /* unplugged: the probe thread re-detects it */
        else if (port == 0 && src != 0) ste_kb_merge(&st);
    } else if (src >= STE_SRC_SDL) {   /* c248 */
        if (!ste_sdl_get_state(src - STE_SRC_SDL, port, &st)) memset(&st, 0, sizeof st);
        else if (port == 0) ste_kb_merge(&st);
        ste_sdl_chord_check(port, &st);
    } else if (src == STE_SRC_KEYB) {
        xbox_InputKeyboardPoll(&st); if (port == 0) mkb_merge(0, &st);
        if (g_sdl) { ste_sdl_keyboard_apply(port, &st); ste_sdl_chord_check(port, &st); }
    }
    if (!press_init) { const char *e = getenv("STE_AUTOPRESS_START"); press_init = 1; if (e && *e) press_at = atof(e); }
    if (port == 0 && press_at >= 0) {   /* STE c41: STE_AUTOPRESS_START=<seconds since process start> injects Start once (0.4 s) */
        double age = ste_proc_age();
        if (age >= press_at && age < press_at + 0.4) {
            st.Gamepad.wButtons |= 0x0010;   /* XINPUT_GAMEPAD_START */
            if (!pressed_logged) { pressed_logged = 1; fprintf(stderr, "[XPP] autopress: Start injected at t=%.1f s\n", age); fflush(stderr); }
        }
    }
    if (g_pscr_n) {   /* STE c50: STE_PAD_SCRIPT presses buttons like a player. KEYS: S=Start K=Back U/D/L/R=d-pad A/B/X/Y=face buttons b=black w=white l=left trigger r=right trigger */
        double age = ste_proc_age(); int i, j; static int logged[4096];
        for (i = 0; i < g_pscr_n; ++i) if (g_pscr[i].port == port && age >= g_pscr[i].t && age < g_pscr[i].t + g_pscr[i].d) {
            st.Gamepad.wButtons |= (uint16_t)g_pscr[i].w; for (j = 0; j < 8; ++j) if (g_pscr[i].a[j]) st.Gamepad.bAnalogButtons[j] = g_pscr[i].a[j];
            if (!logged[i]) { logged[i] = 1; fprintf(stderr, "[XPP] pad script: entry %d port %d (w=%04X a=%u%u%u%u) pressed at t=%.1f s\n", i, port, g_pscr[i].w, g_pscr[i].a[0] != 0, g_pscr[i].a[1] != 0, g_pscr[i].a[2] != 0, g_pscr[i].a[3] != 0, age); fflush(stderr); } }
    }
    { uint32_t cur = st.Gamepad.wButtons | ((uint32_t)(st.Gamepad.bAnalogButtons[0] != 0) << 16) | ((uint32_t)(st.Gamepad.bAnalogButtons[1] != 0) << 17) | ((uint32_t)(st.Gamepad.bAnalogButtons[2] != 0) << 18) | ((uint32_t)(st.Gamepad.bAnalogButtons[3] != 0) << 19) | ((uint32_t)(st.Gamepad.bAnalogButtons[4] != 0) << 20) | ((uint32_t)(st.Gamepad.bAnalogButtons[5] != 0) << 21) | ((uint32_t)(st.Gamepad.bAnalogButtons[6] != 0) << 22) | ((uint32_t)(st.Gamepad.bAnalogButtons[7] != 0) << 23);
      if (cur != last_btn[port]) { last_btn[port] = cur; pkt[port]++; } st.dwPacketNumber = pkt[port]; }
    {   /* STE_RUMBLE_SELFTEST=1: pulse full rumble for 1 s on port N at t = 60 + 5N s (host pads only), to check the port->pad mapping on real hardware */
        static int rt = -1, done[STE_NPORTS]; if (rt < 0) { const char *e = getenv("STE_RUMBLE_SELFTEST"); rt = e && *e && *e != 0x30; }
        if (rt && ((src >= 0 && src <= 3) || src >= STE_SRC_SDL)) { double age = ste_proc_age(); double t0 = 60.0 + 5.0 * port; XBOX_VIBRATION v;
            if (age >= t0 && age < t0 + 1.0 && !done[port]) { done[port] = 1; v.wLeftMotorSpeed = 0xFFFF; v.wRightMotorSpeed = 0xFFFF; if (src >= STE_SRC_SDL) ste_sdl_rumble(src - STE_SRC_SDL, port, 0xFFFF, 0xFFFF); else xbox_InputSetState((DWORD)src, &v); fprintf(stderr, "[RUMBLE] selftest on port %d (host pad %d) ON t=%.1f\n", port, src, age); fflush(stderr); }
            else if (age >= t0 + 1.0 && done[port] == 1) { done[port] = 2; v.wLeftMotorSpeed = 0; v.wRightMotorSpeed = 0; if (src >= STE_SRC_SDL) ste_sdl_rumble(src - STE_SRC_SDL, port, 0, 0); else xbox_InputSetState((DWORD)src, &v); fprintf(stderr, "[RUMBLE] selftest on port %d OFF\n", port); fflush(stderr); } }
    }
    if (src != last_src[port]) { last_src[port] = src; fprintf(stderr, "[XPP] XInputGetState: port %d source %d%s\n", port, src, src < 0 ? " (none)" : ""); fflush(stderr); }
    {   static unsigned shown[STE_NPORTS]; if ((st.Gamepad.wButtons || st.Gamepad.bAnalogButtons[0] || st.Gamepad.bAnalogButtons[1]) && shown[port]++ < 12)
            { fprintf(stderr, "[XPP] pad input: port %d wButtons=%04X A=%u B=%u\n", port, st.Gamepad.wButtons, st.Gamepad.bAnalogButtons[0], st.Gamepad.bAnalogButtons[1]); fflush(stderr); } }
    if (ps) memcpy(GPTR(ps), &st, 22);            /* Xbox XINPUT_STATE = 4 + 2 + 8 + 4*2 bytes */
    xpp_ret(8, 0);
}
/* XInputSetState(handle, feedback) ret 8. XINPUT_FEEDBACK = { XINPUT_FEEDBACK_HEADER { DWORD dwStatus; HANDLE hEvent; BYTE Reserved[58]; } (66 bytes, packed); XINPUT_RUMBLE { WORD wLeftMotorSpeed (+66), wRightMotorSpeed (+68) } }
 * (offsets confirmed from the title's own rumble starter sub_0001E900, which stores 0xFFFF at [feedback+0x42] and [feedback+0x44]).
 * STE_RUMBLE_LOG=1 dumps the first calls' struct so the Rumble offset can be confirmed. Rumble goes to the host pad mapped to the handle's port. */
void sub_002D8DF1(void)
{
    static int n, rl = -1; uint32_t h, f; int port;
    xpp_seen("XInputSetState", &n);
    h = G32(g_esp + 4); f = G32(g_esp + 8);
    if (rl < 0) { const char *e = getenv("STE_RUMBLE_LOG"); rl = e && *e && *e != 0x30; }
    port = ste_port_of_handle(h);
    if (f) {
        uint32_t hev = G32(f + 4);                /* XINPUT_FEEDBACK_HEADER.hEvent */
        uint16_t lo = *(volatile uint16_t *)((uintptr_t)(f + 66) + (uintptr_t)g_xbox_mem_offset), hi = *(volatile uint16_t *)((uintptr_t)(f + 68) + (uintptr_t)g_xbox_mem_offset);
        if (rl) { static int shown; if (shown++ < 40) { int i; fprintf(stderr, "[RUMBLE] port %d feedback@%08X:", port, f); for (i = 0; i < 80; i += 4) fprintf(stderr, " %08X", G32(f + i)); fprintf(stderr, "\n"); fflush(stderr); } }
        if (port >= 0 && g_port_src[port] >= STE_SRC_SDL) ste_sdl_rumble(g_port_src[port] - STE_SRC_SDL, port, lo, hi);   /* c248 */
        else if (port >= 0 && g_port_src[port] >= 0 && g_port_src[port] <= 3) { XBOX_VIBRATION v; v.wLeftMotorSpeed = lo; v.wRightMotorSpeed = hi; xbox_InputSetState((DWORD)g_port_src[port], &v); }
        if (hev) xpp_signal(hev);
        G32(f) = 0;                               /* dwStatus = ERROR_SUCCESS: completed */
    }
    xpp_ret(8, 0);
}

void sub_002D990F(void) { static int n; xpp_seen("XGetDeviceEnumerationStatus", &n); xpp_ret(0, 0); }  /* idle */
void sub_002D7C84(void) { static int n; xpp_seen("MU_Init", &n); xpp_ret(4, 0); }                      /* no memory units */

/* ?????? c61: Hor+ camera aspect ???????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
 * sub_0001F740 is the title's SetPerspective(this=ecx; fov, near, far, aspect_override) (stdcall, 4 args). With
 * aspect_override == 0 it derives the camera's height/width ratio ([this+0x148]) from the viewport size ([this+0x160],
 * [this+0x164]) times the global [0x43239C]; culling, the projection and the matrix all read that field.  For a full-screen
 * 640x480 camera we hand it an explicit override scaled to the wide aspect (vertical FOV kept).  Only active when
 * ste_video aspect=wide (d3d8_video_cam_aspect_scale() != 1).  The generated function is compiled under the name
 * ste_orig_sub_0001F740 (compile-time /D rename of recomp_0000.c; generated source untouched). */
extern void ste_orig_sub_0001F740(void);
extern float d3d8_video_cam_aspect_scale(void);

/* c92: ste_video.ini [video] draw_distance / fov_scale / fov_min_zoom (env STE_VIDEO_<KEY>). Defaults 1 / 1 / 0 = original. */
static float ste_ini_float(const char *key, const char *envname, float def)
{
    char b[64] = "", ini[MAX_PATH]; const char *e = getenv(envname); const char *iv = getenv("STE_VIDEO_INI"); char *p;
    if (e && *e) return (float)atof(e);
    if (iv && *iv) strncpy(ini, iv, MAX_PATH - 1), ini[MAX_PATH - 1] = 0;
    else { GetModuleFileNameA(NULL, ini, MAX_PATH); p = strrchr(ini, 0x5C); if (p) strcpy(p + 1, "ste_video.ini"); }
    GetPrivateProfileStringA("video", key, "", b, sizeof(b), ini);
    return b[0] ? (float)atof(b) : def;
}
static float ste_dd_parse(void); static float ste_dd_val(const char *s);   /* c225 draw_distance = original | N | unlimited, see drawdist.inc */
static float g_ste_draw_dist = 1.0f, g_ste_fov_scale = 1.0f, g_ste_fov_min = 0.0f; static int g_ste_cam_cfg;
static float g_ste_cull_margin = 1.0f; static int g_ste_tune_on; static unsigned g_ste_cull_offs[8]; static int g_ste_cull_noffs;   /* c113: cull_fields is live-tunable too */
/* c107: STE_TUNE_FILE=<ini>: [tune] draw_distance / cull_margin re-read every 500 ms while the game runs (live A/B comparisons of one frozen frame, e.g. the pause screen). Off unless set. */
/* c308: ship clamp 4 (x8 crashed once after ~7 min, open issue); STE_QV_MAX=<n> (<= 8) raises it for the stability test only */
static float ste_qv_max(void) { static float m = 0.0f; if (m <= 0.0f) { const char *e = getenv("STE_QV_MAX"); m = 4.0f; if (e && atof(e) >= 1.0 && atof(e) <= 8.0) m = (float)atof(e); } return m; }
static float g_qv_scale_live = -1.0f;   /* c305: STE_TUNE_FILE query_scale= (render-list query volume widening, see ste_hook_qvol_pre) */
static void ste_tune_poll(void)
{
    static int init; static char path[MAX_PATH]; static DWORD last;
    DWORD now;
    if (!init) { const char *e = getenv("STE_TUNE_FILE"); init = 1; if (e && *e) { strncpy(path, e, MAX_PATH - 1); fprintf(stderr, "[TUNE] live tuning file %s\n", path); fflush(stderr); g_ste_tune_on = 1; } }
    if (!path[0]) return;
    now = GetTickCount(); if (now - last < 500) return; last = now;
    {   char b[32], fb[512]; float v; HANDLE fh; DWORD got = 0; const char *p;
        /* c111: read the file ourselves (GetPrivateProfileString did not see the rewrites made while the game ran) */
        fb[0] = 0; fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh != INVALID_HANDLE_VALUE) { ReadFile(fh, fb, sizeof fb - 1, &got, NULL); CloseHandle(fh); fb[got] = 0; }
        b[0] = 0; p = strstr(fb, "draw_distance="); if (p) { p += 14; { int n = 0; while (n < 30 && p[n] > 0x20) { b[n] = p[n]; n++; } b[n] = 0; } }
        if (b[0]) { v = ste_dd_val(b); if (v != g_ste_draw_dist) { g_ste_draw_dist = v; fprintf(stderr, "[TUNE] tick=%lu draw_distance=%.3f\n", (unsigned long)now, v); fflush(stderr); } }
        b[0] = 0; p = strstr(fb, "fog_scale="); if (p) { p += 10; { int n = 0; while (n < 30 && p[n] > 0x20) { b[n] = p[n]; n++; } b[n] = 0; } }   /* c270: fog A/B on one frozen frame, independent of the draw-distance gates */
        if (b[0]) { extern void kelvin_fog_scale_default(float); kelvin_fog_scale_default((float)atof(b)); }
        b[0] = 0; p = strstr(fb, "query_scale="); if (p) { p += 12; { int n = 0; while (n < 30 && p[n] > 0x20) { b[n] = p[n]; n++; } b[n] = 0; } }
        if (b[0]) { g_qv_scale_live = (float)atof(b); if (g_qv_scale_live < 1.0f) g_qv_scale_live = 1.0f; if (g_qv_scale_live > ste_qv_max()) g_qv_scale_live = ste_qv_max(); }
        b[0] = 0; p = strstr(fb, "opt_off="); if (p) { extern volatile uint32_t g_ste_opt_off; p += 8; { int n = 0; while (n < 30 && p[n] > 0x20) { b[n] = p[n]; n++; } b[n] = 0; } if (b[0]) g_ste_opt_off = (uint32_t)strtoul(b, NULL, 16); }   /* c311: bit mask of optimisations switched back to the original code for A/B (nv2a_kelvin.c STE_OPT_*) */
        p = strstr(fb, "cull_fields=");
        if (p) { unsigned o2[8]; int n2 = 0; p += 12; while (*p && *p > 0x20 && n2 < 8) { o2[n2++] = (unsigned)strtoul(p, (char **)&p, 16); if (*p == 0x2C) p++; }
            if (n2 != g_ste_cull_noffs || memcmp(o2, g_ste_cull_offs, sizeof(unsigned) * (size_t)n2)) { memcpy(g_ste_cull_offs, o2, sizeof(unsigned) * (size_t)n2); g_ste_cull_noffs = n2; fprintf(stderr, "[TUNE] tick=%lu cull_fields=%d first=+0x%X\n", (unsigned long)now, n2, n2 ? o2[0] : 0u); fflush(stderr); } }
        b[0] = 0; p = strstr(fb, "cull_margin="); if (p) { p += 12; { int n = 0; while (n < 30 && p[n] > 0x20) { b[n] = p[n]; n++; } b[n] = 0; } }
        if (b[0]) { v = (float)atof(b); if (v < 0.25f) v = 0.25f; if (v > 16.0f) v = 16.0f; if (v != g_ste_cull_margin) { g_ste_cull_margin = v; fprintf(stderr, "[TUNE] tick=%lu cull_margin=%.3f\n", (unsigned long)now, v); fflush(stderr); } }
    }
}
extern void sub_0001F740(void);
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp;
extern RECOMP_TLS int g_fp_top;
static int g_hud25_pending; static DWORD g_hud25_tick;   /* c171: camera 0x425EB0 was kept native for the loading screen and still has to get the HUD (Hor+) projection */
/* Re-project a camera through the game's own SetPerspective with its stored fov/near/far (aspect_bits 0 = let SetPerspective derive it, i.e. the normal wrapper path incl. k). */
static void ste_cam_reproject(uint32_t cam, uint32_t aspect_bits, uint32_t ret)
{
    union { uint32_t u; float f; } fov, nr, fr;
    uint32_t s_eax = g_eax, s_ebx = g_ebx, s_ecx = g_ecx, s_edx = g_edx, s_esi = g_esi, s_edi = g_edi, s_ebp = g_ebp, s_seh = g_seh_ebp, s_esp = g_esp; int s_fp = g_fp_top;
    fov.u = G32(cam + 0x144); nr.u = G32(cam + 0x14C); fr.u = G32(cam + 0x150);
    g_esp -= 4; G32(g_esp) = aspect_bits;
    g_esp -= 4; G32(g_esp) = fr.u;
    g_esp -= 4; G32(g_esp) = nr.u;
    g_esp -= 4; G32(g_esp) = fov.u;
    g_esp -= 4; G32(g_esp) = ret;
    g_ecx = cam;
    sub_0001F740();
    g_eax = s_eax; g_ebx = s_ebx; g_ecx = s_ecx; g_edx = s_edx; g_esi = s_esi; g_edi = s_edi; g_ebp = s_ebp; g_seh_ebp = s_seh; g_esp = s_esp; g_fp_top = s_fp;
}
/* c174: apply a new stored aspect to a camera WITHOUT running SetPerspective: aspect [+0x148] = target, the horizontal extent [+0x130] (= A * aspect, any c130 widening / cull_margin factor
 * is preserved because it is scaled, not rebuilt), the inverse [+0x140] (~ 1/extent) is divided and the unwidened extent copy [+0x60] (verified by STE_CAMTEST: it equals A*aspect) is multiplied by the same ratio. Writes only those four dwords of that camera;
 * no device state, no other camera, no stack/registers. STE_CAMTEST=1: self-test that runs the real SetPerspective (as c169-c173 did) on the same camera, compares all 0x100 bytes of
 * camera state 0x60..0x15F and logs [CAMTEST] (the game call's result is kept in that mode). STE_CAMFIX_CALL=1 = always use the real call. */
static void ste_cam_apply_aspect(uint32_t cam, float target, uint32_t ret, uint32_t call_aspect_bits)
{
    static int tinit, test, usecall; union { uint32_t u; float f; } cur; float r; int i;
    if (!tinit) { tinit = 1; test = getenv("STE_CAMTEST") != NULL; usecall = getenv("STE_CAMFIX_CALL") != NULL; }
    cur.u = G32(cam + 0x148);
    if (!(cur.f > 0.0f) || !(target > 0.0f) || cur.f == target) return;
    if (usecall) { ste_cam_reproject(cam, call_aspect_bits, ret); return; }
    {   uint32_t snap[64], da[64], db[64]; if (test) for (i = 0; i < 64; ++i) snap[i] = G32(cam + 0x60 + 4u * (uint32_t)i);
        r = target / cur.f;
        { union { uint32_t u; float f; } v;
          v.f = target; G32(cam + 0x148) = v.u;
          v.u = G32(cam + 0x130); v.f *= r; G32(cam + 0x130) = v.u;
          v.u = G32(cam + 0x140); v.f /= r; G32(cam + 0x140) = v.u;
          v.u = G32(cam + 0x60);  v.f *= r; G32(cam + 0x60) = v.u; }
        if (test) {
            int bad = 0; static int nt;
            for (i = 0; i < 64; ++i) da[i] = G32(cam + 0x60 + 4u * (uint32_t)i);
            for (i = 0; i < 64; ++i) G32(cam + 0x60 + 4u * (uint32_t)i) = snap[i];
            ste_cam_reproject(cam, call_aspect_bits, ret);
            for (i = 0; i < 64; ++i) db[i] = G32(cam + 0x60 + 4u * (uint32_t)i);
            for (i = 0; i < 64; ++i) { union { uint32_t u; float f; } x, y; x.u = da[i]; y.u = db[i];
                if (x.u != y.u && !(fabsf(x.f - y.f) <= 1e-4f * (fabsf(y.f) + 1e-6f))) { bad++; if (nt < 40) { fprintf(stderr, "[CAMTEST] cam=0x%08X +0x%X direct=%.6g call=%.6g (before %.6g)\n", cam, 0x60 + 4 * i, x.f, y.f, *(float *)&snap[i]); nt++; } } }
            fprintf(stderr, "[CAMTEST] cam=0x%08X aspect %.4f -> %.4f : %d of 64 dwords differ between direct write and SetPerspective\n", cam, cur.f, target, bad); fflush(stderr);
        }
    }
}
static int cf_load_on(void) { static int init, on; if (!init) { const char *e = getenv("STE_LOADING_CAMFIX"); init = 1; on = !(e && e[0] == '0'); fprintf(stderr, "[CAMCFG] loading_camfix=%d\n", on); fflush(stderr); } return on; }/* c132 experiment: STE_CAMFIX_CAMS="hex,hex" limits the pillar camera fix to those camera objects (default: all). */
static int cf_cam_ok(uint32_t cam)
{
    static int init; static uint32_t list[8]; static int n;
    if (!init) { const char *e = getenv("STE_CAMFIX_CAMS"); const char *p = e; init = 1; while (p && *p && n < 8) { list[n++] = (uint32_t)strtoul(p, (char **)&p, 16); if (*p == 0x2C) p++; } }
    if (!n) return 1;
    for (int i = 0; i < n; ++i) if (list[i] == cam) return 1;
    return 0;
}
void sub_0001F740(void)
{
    float k = d3d8_video_cam_aspect_scale();
    {   /* c132: pillar menus: the boot/movie/menu cameras (made while the game-state word is not 4 = loading/in-game, and not the HUD camera 0x8816D) kept the Hor+ scale k=0.75 because the
         * state word was not yet 0x27 when they were built; the front-end UI projected through them was squeezed to (4/3)/(W/H). They now keep the native 4:3 projection like the 0x27 cameras.
         * Off: ste_video.ini pillar_camfix=0 or STE_VIDEO_PILLAR_CAMFIX=0. */
        static int cf_init, cf_on, cf_ex, cf_fe_seen; static int nlog;   /* cf_fe_seen: once the front-end (state 0x27) was reached, later state-0/1 calls are the fight/HUD set-up (they must keep the Hor+ k, or the HUD stretches across the wide screen) */
        if (!cf_init) { cf_init = 1; cf_on = ste_ini_float("pillar_camfix", "STE_VIDEO_PILLAR_CAMFIX", 1.0f) != 0.0f; cf_ex = getenv("STE_CAMFIX_EXEMPT_HUDCAM") != NULL; fprintf(stderr, "[CAMCFG] pillar_camfix=%d exempt_hudcam=%d\n", cf_on, cf_ex); fflush(stderr); }
        if (G32(0x003562B4u) == 0x27u) cf_fe_seen = 1;
        {   static int nl2, tr_init, tr_on; if (!tr_init) { tr_init = 1; tr_on = getenv("STE_CAMFIX_TRACE") != NULL; }   /* c140 probe; c166: every DISTINCT (caller, camera, state, k, aspect arg, stored w/h) is logged once (max 600) with a tick, so the whole run is covered, not only the first 200 calls */
            if (tr_on && nl2 < 600) { static struct { uint32_t ret, cam, gs, kb, a4, wh; } seen[600]; uint32_t kb; uint32_t ret = G32(g_esp), cam = (uint32_t)g_ecx, gs = G32(0x003562B4u), a4 = G32(g_esp + 0x10), wh = G32(g_ecx + 0x160) * 4096u + G32(g_ecx + 0x164); int i; memcpy(&kb, &k, 4);
                for (i = 0; i < nl2; ++i) if (seen[i].ret == ret && seen[i].cam == cam && seen[i].gs == gs && seen[i].kb == kb && seen[i].a4 == a4 && seen[i].wh == wh) break;
                if (i == nl2) { seen[nl2].ret = ret; seen[nl2].cam = cam; seen[nl2].gs = gs; seen[nl2].kb = kb; seen[nl2].a4 = a4; seen[nl2].wh = wh; nl2++;
                    fprintf(stderr, "[CAMTRACE] tick=%lu ret=0x%08X cam=0x%08X state=0x%X k=%.4f fe_active=%d seen=%d arg4=0x%08X wh=%u/%u\n", (unsigned long)GetTickCount(), ret, cam, gs, k, d3d8_front_end_active(), cf_fe_seen, a4, G32(g_ecx + 0x160), G32(g_ecx + 0x164)); fflush(stderr); } } }
        if (k != 1.0f && cf_on) {
            const D3D8VideoCfg *vc = d3d8_video_cfg(); uint32_t gs = G32(0x003562B4u), rr = G32(g_esp);
            if (vc->aspect == D3D8V_ASPECT_WIDE && vc->menu_aspect == D3D8V_ASPECT_PILLAR && gs != 4u && !cf_fe_seen && !(cf_ex && rr == 0x0008816Du) && cf_cam_ok((uint32_t)g_ecx)) {
                if (nlog++ < 16) { fprintf(stderr, "[CAMFIX] ret=0x%08X cam=0x%08X state=0x%X k %.4f -> 1\n", rr, (unsigned)g_ecx, gs, k); fflush(stderr); }
                k = 1.0f;
            } else if (vc->aspect == D3D8V_ASPECT_WIDE && vc->menu_aspect == D3D8V_ASPECT_PILLAR && gs == 1u && (uint32_t)g_ecx == 0x00425EB0u && rr == 0x0008816Du && cf_load_on()) {
                /* c170: the state-1 call re-projects the shared camera 0x425EB0 with the Hor+ aspect (0.5625) just before the LOADING screen, which is drawn through it (picture 75 % wide, attract
                 * matches and every match after a front-end visit). Keep it native here; STE_LOADING_CAMFIX=0 restores the squeeze. */
                if (nlog++ < 16) { fprintf(stderr, "[CAMFIX] loading camera: ret=0x%08X cam=0x%08X state=0x%X k %.4f -> 1 (HUD projection follows when the match scene is built)\n", rr, (unsigned)g_ecx, gs, k); fflush(stderr); }
                k = 1.0f; g_hud25_pending = 1; g_hud25_tick = GetTickCount();
            } else if (nlog++ < 16) { fprintf(stderr, "[CAMFIX] ret=0x%08X cam=0x%08X state=0x%X k=%.4f kept\n", rr, (unsigned)g_ecx, gs, k); fflush(stderr); }
        }
    }
    ste_tune_poll();
    uint32_t ste_cam = g_ecx, ste_ret0 = G32(g_esp), ste_gs0 = G32(0x003562B4u); int ste_world = (G32(g_esp) == 0x001EA0B1u || G32(g_esp) == 0x000CB4C1u); int ste_fecam = (G32(g_esp) == 0x0007F72Au);   /* c130: ret 0x7F72A = the front-end room camera (the only SetPerspective caller seen while the game-state word is 0x27) */   /* c106: for the cull_margin step after the original returns */
    if (k != 1.0f && G32(g_esp + 0x10) == 0 && G32(g_ecx + 0x160) == 640u && G32(g_ecx + 0x164) == 480u) {
        union { uint32_t u; float f; } g, a;
        g.u = G32(0x0043239Cu);
        if (g.f != g.f || g.f == 0.0f) g.f = 1.0f;
        a.f = (480.0f / 640.0f) * g.f * k;
        G32(g_esp + 0x10) = a.u;
        { static int n; if (n++ < 4) fprintf(stderr, "[WIDE] SetPerspective aspect_override=%.5f (k=%.4f)\n", a.f, k); }
    }
    if (ste_fecam && k == 1.0f && G32(g_esp + 0x10) != 0 && G32(g_ecx + 0x160) == 640u && G32(g_ecx + 0x164) == 480u) {
        /* c141: coming back from a fight the front-end room camera (0x425EB0, caller 0x7F72A) is re-projected with the aspect STORED in the camera, which still holds the fight-HUD squeeze set while the game-state word was 1
         * (camera 0x425EB0 is also the HUD camera): the whole main menu / versus select came back narrowed to (4/3)/(W/H).  Put the native 4:3 aspect back, exactly what the first visit passes (0.75).
         * Same scope as pillar_camfix (pillar + wide only); off with pillar_camfix=0 / STE_VIDEO_PILLAR_CAMFIX=0. */
        static int rf_init, rf_on, nlog;
        if (!rf_init) { rf_init = 1; rf_on = ste_ini_float("pillar_camfix", "STE_VIDEO_PILLAR_CAMFIX", 1.0f) != 0.0f; }
        const D3D8VideoCfg *vc2 = d3d8_video_cfg();
        if (rf_on && vc2->aspect == D3D8V_ASPECT_WIDE && vc2->menu_aspect == D3D8V_ASPECT_PILLAR && d3d8_front_end_active()) {
            union { uint32_t u; float f; } g2, cur, nat;
            g2.u = G32(0x0043239Cu); if (g2.f != g2.f || g2.f == 0.0f) g2.f = 1.0f;
            nat.f = (480.0f / 640.0f) * g2.f; cur.u = G32(g_esp + 0x10);
            if (cur.f != nat.f) { if (nlog++ < 8) { fprintf(stderr, "[CAMFIX] return-to-menu: cam=0x%08X aspect arg %.4f -> %.4f\n", (unsigned)g_ecx, cur.f, nat.f); fflush(stderr); } G32(g_esp + 0x10) = nat.u; }
        }
    }
    if (!g_ste_cam_cfg) {
        g_ste_cam_cfg = 1;
        g_ste_draw_dist = ste_dd_parse();
        g_ste_fov_scale = ste_ini_float("fov_scale", "STE_VIDEO_FOV_SCALE", 1.0f);
        g_ste_fov_min = ste_ini_float("fov_min_zoom", "STE_VIDEO_FOV_MIN_ZOOM", 0.0f);
        if (g_ste_draw_dist < 0.25f) g_ste_draw_dist = 0.25f; if (g_ste_draw_dist > 16.0f) g_ste_draw_dist = 16.0f;
        if (g_ste_fov_scale < 0.5f) g_ste_fov_scale = 0.5f; if (g_ste_fov_scale > 2.0f) g_ste_fov_scale = 2.0f;
        fprintf(stderr, "[CAMCFG] draw_distance=%.3f fov_scale=%.3f fov_min_zoom=%.3f\n", g_ste_draw_dist, g_ste_fov_scale, g_ste_fov_min);
    }
    {   /* applied to the arguments before the original runs: far plane, vertical fov (scale, then floor) */
        union { uint32_t u; float f; } fo, fr;
        int world_cam = (G32(g_esp) == 0x001EA0B1u || G32(g_esp) == 0x000CB4C1u);  /* c96: only the world/fight cameras (far 4096) are widened; the 0x8816D camera draws the HUD and clips at near x4 */
        fo.u = G32(g_esp + 4); fr.u = G32(g_esp + 12);
        if (world_cam && (g_ste_draw_dist != 1.0f || g_ste_tune_on) && fr.f > 1.0f) {
            /* some callers feed the camera's stored far back in (0x1EA0B1): do not compound, remember what we wrote per camera */
            static struct { uint32_t cam, out; float dd, orig_far, orig_near; } cams[16]; static int nc; int k; uint32_t cam = g_ecx;
            for (k = 0; k < nc; k++) if (cams[k].cam == cam) break;
            if (k == nc && nc < 16) { cams[nc].cam = cam; cams[nc].out = 0; nc++; }
            if (k < nc && cams[k].out == fr.u && cams[k].dd == g_ste_draw_dist) { /* already scaled by us */ }
            else { union { uint32_t u; float f; } nz;
                if (k < nc && cams[k].out == fr.u) {   /* c107: our own earlier output came back after draw_distance changed: rescale from the remembered originals */
                    union { uint32_t u; float f; } nn; fr.f = cams[k].orig_far * g_ste_draw_dist; nn.u = G32(g_esp + 8); nn.f = cams[k].orig_near; G32(g_esp + 8) = nn.u; }
                else if (k < nc) { union { uint32_t u; float f; } nn; cams[k].orig_far = fr.f; nn.u = G32(g_esp + 8); cams[k].orig_near = nn.f; fr.f *= g_ste_draw_dist; }
                else fr.f *= g_ste_draw_dist;
                if (k < nc) { cams[k].out = fr.u; cams[k].dd = g_ste_draw_dist; } G32(g_esp + 12) = fr.u;
                /* keep far/near ratio (depth precision): near scales with the distance multiplier (STE_VIDEO_NEAR_EXP, default 1.0 = full ratio) */
                nz.u = G32(g_esp + 8); if (nz.f > 0.5f) { nz.f *= powf(g_ste_draw_dist, ste_ini_float("near_exp", "STE_VIDEO_NEAR_EXP", 1.0f)); G32(g_esp + 8) = nz.u; } }
        }
        if (world_cam && (g_ste_fov_scale != 1.0f || g_ste_fov_min > 0.0f) && fo.f > 0.05f && fo.f < 2.5f) {
            float f = fo.f * g_ste_fov_scale; if (f < g_ste_fov_min) f = g_ste_fov_min; if (f > 2.6f) f = 2.6f;
            fo.f = f; G32(g_esp + 4) = fo.u;
        }
    }
    {   /* c92 probe: per (caller, near, far): call count + fov range, printed on first sight and every 600 calls */
        static struct { uint32_t ret, nr, far_; float fmin, fmax; unsigned long n; } seen[64]; static int ns; int i;
        union { uint32_t u; float f; } fo; uint32_t ret = G32(g_esp), nr = G32(g_esp + 8), fr = G32(g_esp + 12);
        fo.u = G32(g_esp + 4);
        for (i = 0; i < ns; i++) if (seen[i].ret == ret && seen[i].nr == nr && seen[i].far_ == fr) break;
        if (i == ns && ns < 64) { seen[ns].ret = ret; seen[ns].nr = nr; seen[ns].far_ = fr; seen[ns].fmin = seen[ns].fmax = fo.f; seen[ns].n = 0; ns++; }
        if (i < ns) { seen[i].n++; if (fo.f < seen[i].fmin) seen[i].fmin = fo.f; if (fo.f > seen[i].fmax) seen[i].fmax = fo.f;
            if (seen[i].n == 1 || (seen[i].n % 100) == 0) { union { uint32_t u; float f; } nn, ff; nn.u = nr; ff.u = fr;
                fprintf(stderr, "[PERSP] ret=0x%08X near=%.2f far=%.1f n=%lu fov=[%.4f..%.4f]\n", ret, nn.f, ff.f, seen[i].n, seen[i].fmin, seen[i].fmax); } }
    }    ste_orig_sub_0001F740();
    if (g_hud25_pending && ste_gs0 >= 4u && ste_gs0 <= 6u && ((ste_ret0 == 0x0008816Du && ste_cam == 0x0042D480u) || ste_world)) {
        /* c171: the match scene (HUD camera 0x42D480 / world camera) is being built = loading is over: now the shared camera 0x425EB0 gets the HUD projection the game asked for at state 1 */
        static int n171; g_hud25_pending = 0;
        if (n171++ < 8) { fprintf(stderr, "[CAMFIX] loading over (ret=0x%08X cam=0x%08X state=0x%X): camera 0x00425EB0 re-projected with the HUD aspect\n", ste_ret0, (unsigned)ste_cam, ste_gs0); fflush(stderr); }
        {   union { uint32_t u; float f; } gg; gg.u = G32(0x0043239Cu); if (gg.f != gg.f || gg.f == 0.0f) gg.f = 1.0f;
            ste_cam_apply_aspect(0x00425EB0u, (480.0f / 640.0f) * gg.f * d3d8_video_cam_aspect_scale(), 0x0008816Du, 0u); }
    }
    {   /* c106: ste_video.ini cull_margin (STE_VIDEO_CULL_MARGIN, default 1.0 = original): scales the camera's frustum extents after the title has built the projection from them
         * (SetPerspective stores half-height/half-width at camera+0x130/+0x134 and passes them to the matrix builder first), so the view stays identical and only code that reads
         * the stored extents later (visibility tests) sees a wider frustum. STE_CULL_FIELDS = comma list of hex camera offsets to scale (default "130,134"; for experiments). */
        static int cinit;
        if (!cinit) { const char *fe = getenv("STE_CULL_FIELDS"); const char *p; cinit = 1;
            g_ste_cull_margin = ste_ini_float("cull_margin", "STE_VIDEO_CULL_MARGIN", 1.0f); if (g_ste_cull_margin < 0.25f) g_ste_cull_margin = 0.25f; if (g_ste_cull_margin > 16.0f) g_ste_cull_margin = 16.0f;
            p = (fe && *fe) ? fe : "130"; while (*p && g_ste_cull_noffs < 8) { g_ste_cull_offs[g_ste_cull_noffs++] = (unsigned)strtoul(p, (char **)&p, 16); if (*p == 0x2C) p++; }
            fprintf(stderr, "[CAMCFG] cull_margin=%.3f fields=%d (first +0x%X)\n", g_ste_cull_margin, g_ste_cull_noffs, g_ste_cull_offs[0]); fflush(stderr); }
        {   /* c130: with aspect=wide + menu_aspect=pillar the room camera keeps its native 4:3 projection, but the game culls objects against that 4:3 frustum (camera+0x130/+0x134)
             * while the wide surface shows more: tubes/objects outside the 4:3 view were dropped (far-right cylinder glass missing). Widen the stored horizontal extent (camera+0x130 divided by (render aspect)/(4/3)) after the projection was built
             * (same mechanism as cull_margin); the projection itself is unchanged. STE_FE_CULL=0 disables. */
            static int fe_init, fe_on;
            if (!fe_init) { const char *e = getenv("STE_FE_CULL"); fe_on = !(e && e[0] == 0x30); fe_init = 1; fprintf(stderr, "[CAMCFG] frontend_cull_widen=%d\n", fe_on); fflush(stderr); }
            if (fe_on && ste_fecam && ste_cam) {
                const D3D8VideoCfg *vc = d3d8_video_cfg();
                if (vc->aspect == D3D8V_ASPECT_WIDE && vc->menu_aspect == D3D8V_ASPECT_PILLAR && d3d8_front_end_active()) {
                    float m = ((float)vc->render_w / (float)vc->render_h) / (4.0f / 3.0f);
                    if (m > 1.001f) {
                        union { uint32_t u; float f; } a, b; static int nlog;
                        a.u = G32(ste_cam + 0x130); b.u = G32(ste_cam + 0x134);
                        if (nlog++ < 6) { fprintf(stderr, "[FECULL] cam=0x%08X ext130=%.4f ext134=%.4f /%.4f\n", ste_cam, a.f, b.f, m); fflush(stderr); }
                        a.f /= m; G32(ste_cam + 0x130) = a.u;   /* 0x130 is a "cotangent"-style extent (smaller = wider): verified by experiment, x m made the tubes vanish, / m shows them; 0x134 stays */
                    }
                }
            }
        }
        if (ste_world && g_ste_cull_margin != 1.0f && ste_cam)
            for (int i = 0; i < g_ste_cull_noffs; ++i) { union { uint32_t u; float f; } v; v.u = G32(ste_cam + g_ste_cull_offs[i]); v.f /= g_ste_cull_margin; G32(ste_cam + g_ste_cull_offs[i]) = v.u;   /* c131: the stored extent is in inverse form (smaller = wider; found in c130 on the front-end camera), so cull_margin > 1 must DIVIDE to widen */ }
    }
}
/* c168/c169: front-end re-entry camera restore (ste_video.ini pillar_camfix=1 / STE_VIDEO_PILLAR_CAMFIX, pillar menus on a wide window only).
 * The fight HUD set-up (state word 1, caller 0x8816D) re-projects the shared room/HUD camera 0x425EB0 with the Hor+ aspect k (0.5625 = 0.75 * 0.75, "kept" so the HUD stays centred).
 * When the title returns to the front-end without rebuilding that camera (attract demo -> movies/menu: no 0x7F72A call is made, c141 only corrects calls the game makes), the
 * front-end draws through the squeezed camera: movies and screens come out 75 % wide. Once per return to the front-end (state change into 3, 0x15 or 0x27) the camera is re-projected through the game's own
 * SetPerspective with its stored fov/near/far and the native aspect, exactly what c141 does for the return-to-menu call. Off: STE_FE_CAMRESTORE=0. */
void ste_fe_camera_tick(void)
{
    static int init, on; static uint32_t last_state = 0xFFFFFFFFu; static int nlog;
    const uint32_t cam = 0x00425EB0u; uint32_t st; union { uint32_t u; float f; } cur, nat, g, fov, nr, fr;
    if (!init) { const char *e = getenv("STE_FE_CAMRESTORE"); init = 1; on = !(e && e[0] == '0') && ste_ini_float("pillar_camfix", "STE_VIDEO_PILLAR_CAMFIX", 1.0f) != 0.0f; fprintf(stderr, "[CAMCFG] fe_camrestore=%d\n", on); fflush(stderr); }
    if (!on) return;
    st = G32(0x003562B4u);
    if (st == last_state) return;   /* act once per state change (the entry into 0x27), not every frame */
    last_state = st;
    if (st != 0x27u && st != 0x15u && st != 3u) return;   /* the return path is 0 -> 1 -> 3 -> 0x15 -> 0x27; the movie quad is built when the front-end opens, so restore as early as the state 1 squeeze is over */
    {   const D3D8VideoCfg *vc = d3d8_video_cfg();
        if (!(vc->aspect == D3D8V_ASPECT_WIDE && vc->menu_aspect == D3D8V_ASPECT_PILLAR)) return; }
    if (G32(cam + 0x160) != 640u || G32(cam + 0x164) != 480u) return;   /* camera not set up yet */
    g.u = G32(0x0043239Cu); if (g.f != g.f || g.f == 0.0f) g.f = 1.0f;
    nat.f = (480.0f / 640.0f) * g.f; cur.u = G32(cam + 0x148);
    fov.u = G32(cam + 0x144); nr.u = G32(cam + 0x14C); fr.u = G32(cam + 0x150);
    if (cur.f == nat.f || !(fov.f > 0.05f && fov.f < 2.6f) || !(fr.f > nr.f)) return;
    if (nlog++ < 8) { fprintf(stderr, "[CAMFIX] front-end entry: camera 0x%08X aspect %.4f -> %.4f (stored fields only)\n", cam, cur.f, nat.f); fflush(stderr); }
    ste_cam_apply_aspect(cam, nat.f, 0x0007F72Au, nat.u);
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (xbox_va == 0x00015512u) return wrap_00015512;
    return (recomp_func_t)0;
}

/* ?????? ICALL failure logging ????????????????????????????????????????????????????????????????????????????????????????????????????????? */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    /* candidate4: an unresolved indirect call is FATAL. Skipping it leaves the callee's stack
     * arguments on the guest stack and corrupts the caller's saved registers (0x223D7D, see
     * RESUME_PACKET.md), so stop here instead of continuing. ret = the guest return address the
     * ICALL macro pushed just before this call. */
    fprintf(stderr, "[ICALL FAIL] va=0x%08X ret=0x%08X esp=0x%08X (total calls: %llu)\n",
            va, g_esp ? G32(g_esp) : 0u, g_esp, (unsigned long long)g_icall_count);
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
    exit(6);
}
/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu) tid=%lu esp=%08X ret_on_stack=%08X\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count, GetCurrentThreadId(), g_esp, g_esp ? *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp) : 0u);
    fflush(stderr);
}



/* STE c41: loud null/wild indirect calls. The first 20 are logged in full: call site (return address on the stack and
 * the call instruction decoded from the guest code bytes before it), this pointer (ecx), vtable register value (only
 * eax/ecx/edx are globals; a callee-saved base register is a local of the generated function and shows n/a), slot
 * offset, esp before and after the skip, running total. The skip itself is unchanged. */
void recomp_icall_not_code_log_ex(uint32_t va, uint32_t esp_before, uint32_t esp_after, uint32_t site)
{
    static unsigned long long total; static unsigned logged;
    uint32_t ret = esp_before ? *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + esp_before) : 0, insn_va = 0, slot = 0, base_val = 0;
    const char *base_name = "?"; char desc[64] = "?"; int have_base = 0;
    total++;
    if (logged >= 20) { if ((total & (total - 1)) == 0) { fprintf(stderr, "[NULLCALL] running total %llu (target 0x%08X, ret %08X)\n", total, va, ret); fflush(stderr); } return; }
    logged++;
    if (ret > 8 && ret < 0x04000000u) {
        static const char *rn[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
        const uint8_t *c = (const uint8_t *)((uintptr_t)g_xbox_mem_offset + ret); int r = -1;
        if (c[-3] == 0xFF && (c[-2] & 0xF8) == 0x50) { r = c[-2] & 7; slot = (uint32_t)(int8_t)c[-1]; insn_va = ret - 3; sprintf(desc, "call [%s%+d]", rn[r], (int)(int8_t)c[-1]); }
        else if (c[-6] == 0xFF && (c[-5] & 0xF8) == 0x90) { r = c[-5] & 7; slot = *(const uint32_t *)(c - 4); insn_va = ret - 6; sprintf(desc, "call [%s+0x%X]", rn[r], slot); }
        else if (c[-2] == 0xFF && (c[-1] & 0xF8) == 0x10) { r = c[-1] & 7; slot = 0; insn_va = ret - 2; sprintf(desc, "call [%s]", rn[r]); }
        if (r >= 0) { base_name = rn[r]; if (r == 0) { base_val = g_eax; have_base = 1; } else if (r == 1) { base_val = g_ecx; have_base = 1; } else if (r == 2) { base_val = g_edx; have_base = 1; } }
    }
    fprintf(stderr, "[NULLCALL] #%u tid=%lu target=0x%08X site=0x%08X ret=0x%08X insn=%s at 0x%08X this(ecx)=0x%08X vtable(%s)=%s slot=0x%X esp before=0x%08X after=0x%08X total=%llu\n",
            logged, GetCurrentThreadId(), va, site, ret, desc, insn_va, g_ecx, base_name, "", slot, esp_before, esp_after, total);
    if (have_base) fprintf(stderr, "[NULLCALL]    vtable value=0x%08X\n", base_val);
    fflush(stderr);
}
/* appended from xboxrecomp v0.14 templates\new-game\src\recomp_manual.c (RECOMP_UNIMPL hook the v0.14 lifter calls) */
/* ?????? Untranslated instructions ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}









/* ---- c225: draw_distance = original | N | unlimited -- per-object cull override (SPDX-License-Identifier: GPL-2.0-or-later).
 * The game's draw-submit methods skip an object in two places (found by the c222-c224 census, docs\community\Q1_CAMERA_DRAW_DISTANCE.md):
 *   range test  sub_0013BE90 @0x13BF08 and sub_0013C0A0 @0x13C118:  fcomp [obj+0x88]  -> object skipped when depth*cam[0x140] > obj[0x88]
 *   size test   sub_0017D1F0 @0x17D27A (fcompp) and sub_0017CDD0 @0x17CE4C (fcom [set+0x38]) -> mesh 0 (= not drawn) when radius/depth <= threshold
 * hooks\ste_hooks.toml puts a call BEFORE the compare (range, 17D1F0: the hook edits the x87 operand that is about to be compared) or AFTER it (17CDD0: the hook edits the condition code).
 * N scales the range by N and divides the size threshold by N; unlimited never skips and falls through to the game's own LOD choice (lowest detail for tiny objects).
 * Only active in the in-game states 4/5 (fight cameras); every other state, and draw_distance=original, run the game's own compare untouched. Nothing here writes guest memory. */
#include "recomp_types.h"
unsigned g_ste_fc_n_pub; int g_ste_fc_dd = -1;   /* debug (freecam script "dd <n> <0|1>"): -1 = follow draw_distance, 0 = force original, 1 = force the override */
static int g_dd_unl; static float g_dd_mul = 1.0f; static unsigned long long g_dd_n[4], g_dd_rej[4], g_dd_pass[4]; static int g_dd_cfgd; static DWORD g_dd_last_fight; volatile int g_dd_win = -1;
static int g_dd_ab_on; static unsigned g_dd_ab_s; static DWORD g_dd_ab_t0;   /* STE_DD_AB=<seconds>: debug A/B, toggles the override every <seconds> (update-side safety check) */
/* parse one draw_distance value: "original" -> 1, "unlimited" -> far plane x4 + cull bypass, number -> far plane xN + cull xN. Returns the far-plane multiplier. */
static float ste_dd_val0(const char *s);
static float ste_dd_val(const char *s)
{
    float r = ste_dd_val0(s);
    /* c267/c270: unlimited = nothing fades into the fog either (fog starts ~10000 instead of ~1250); original / numeric: fog untouched (a live switch back through STE_TUNE_FILE restores the title's own fog) */
    { extern void kelvin_fog_scale_default(float); static int hinted; if (g_dd_unl) { kelvin_fog_scale_default(8.0f); hinted = 1; } else if (hinted) { kelvin_fog_scale_default(1.0f); hinted = 0; } }
    return r;
}
static float ste_dd_val0(const char *s)
{
    float v = 1.0f;
    g_dd_unl = 0; g_dd_mul = 1.0f;
    if (!s || !*s || !_strnicmp(s, "original", 8)) return 1.0f;
    if (!_strnicmp(s, "unlimited", 9)) { g_dd_unl = 1; g_dd_mul = 4.0f; return 4.0f; }
    v = (float)atof(s); if (v < 0.25f) v = 0.25f; if (v > 16.0f) v = 16.0f; g_dd_mul = v; return v;
}
static float ste_dd_parse(void)
{
    char b[64] = "", ini[MAX_PATH]; const char *e = getenv("STE_VIDEO_DRAW_DISTANCE"); const char *iv = getenv("STE_VIDEO_INI"); char *p; float v;
    if (e && *e) strncpy(b, e, sizeof b - 1);
    else {
        if (iv && *iv) strncpy(ini, iv, MAX_PATH - 1), ini[MAX_PATH - 1] = 0;
        else { GetModuleFileNameA(NULL, ini, MAX_PATH); p = strrchr(ini, 0x5C); if (p) strcpy(p + 1, "ste_video.ini"); }
        GetPrivateProfileStringA("video", "draw_distance", "", b, sizeof(b), ini);
    }
    v = ste_dd_val(b); g_dd_cfgd = 1;
    e = getenv("STE_DD_AB"); if (e && *e) { g_dd_ab_s = (unsigned)atoi(e); g_dd_ab_on = g_dd_ab_s > 0; }
    fprintf(stderr, "[DRAWDIST] draw_distance=\"%s\" -> far plane x%.2f, cull %s (x%.2f)%s\n", b, v, g_dd_unl ? "unlimited" : (g_dd_mul == 1.0f ? "original" : "scaled"), g_dd_mul, g_dd_ab_on ? " A/B alternating" : ""); fflush(stderr);
    return v;
}
static int dd_active(void)
{
    unsigned s = G32(0x003562B4u);
    if (s != 4u && s != 5u) return 0;
    if (g_ste_fc_dd == 0) return 0;
    if (g_ste_fc_dd == 1) return g_dd_unl || g_dd_mul != 1.0f;
    if (g_dd_ab_on) { DWORD now = GetTickCount(); static int lastw = -1; int w; if (!g_dd_ab_t0) g_dd_ab_t0 = now; w = (int)((((now - g_dd_ab_t0) / 1000u) / g_dd_ab_s) & 1u); if (w != lastw) { SYSTEMTIME st; GetLocalTime(&st); lastw = w; fprintf(stderr, "[DRAWDIST] A/B window: override %s from %02u:%02u:%02u.%03u\n", w ? "OFF (original)" : "ON", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds); fflush(stderr); } if (w) return 0; }   /* odd windows: original */
    return g_dd_unl || g_dd_mul != 1.0f;
}
static void dd_report(void)
{
    static DWORD last; DWORD now = GetTickCount(); { unsigned s = G32(0x003562B4u); if (s == 4u || s == 5u) { g_dd_last_fight = now; if (!g_dd_ab_t0) g_dd_ab_t0 = now; } }
    g_dd_win = (g_dd_last_fight && now - g_dd_last_fight < 1000u) ? (g_dd_ab_on ? (int)((((now - g_dd_ab_t0) / 1000u) / g_dd_ab_s) & 1u) : 0) : -1;   /* debug profile window: 0 override, 1 original, -1 not in a fight */
    if (!last) last = now;
    if (now - last >= 20000u) {
        unsigned cam = G32(0x4322F0u); union { uint32_t u; float f; } f130, f140, f148; last = now;
        f130.u = cam ? G32(cam + 0x130) : 0; f140.u = cam ? G32(cam + 0x140) : 0; f148.u = cam ? G32(cam + 0x148) : 0;
        fprintf(stderr, "[DRAWDIST] tick=%lu range: n=%llu orig-reject=%llu now-drawn=%llu | size: n=%llu orig-reject=%llu now-drawn=%llu | size2: n=%llu orig-reject=%llu now-drawn=%llu | sphere: n=%llu orig-reject=%llu now-drawn=%llu | cam[0x130]=%.4f [0x140]=%.5f [0x148]=%.4f viewport[0x160]=%u [0x164]=%u\n",
            (unsigned long)now, g_dd_n[0], g_dd_rej[0], g_dd_pass[0], g_dd_n[1], g_dd_rej[1], g_dd_pass[1], g_dd_n[2], g_dd_rej[2], g_dd_pass[2], g_dd_n[3], g_dd_rej[3], g_dd_pass[3], f130.f, f140.f, f148.f, cam ? G32(cam + 0x160) : 0u, cam ? G32(cam + 0x164) : 0u);
        fflush(stderr);
#ifdef STE_DD_PROFILE
        ddpf_dump();
#endif
    }
}
/* STE_DD_TRACE_N=<lo>-<hi> (debug, needs the free-cam build): logs every cull decision the game takes while the free-cam camera update counter n is in that range. */
static unsigned g_dd_tr_lo, g_dd_tr_hi; static int g_dd_tr_init;
static int dd_radial(unsigned posp, double *rad, double *cosf);
static void dd_trace(const char *site, unsigned obj, double depth, double thr, unsigned posp)
{
    static unsigned cnt; union { uint32_t u; float f; } x, y, z; double rr = 0, cc = 0;
    if (!g_dd_tr_init) { const char *e = getenv("STE_DD_TRACE_N"); g_dd_tr_init = 1; if (e) sscanf(e, "%u-%u", &g_dd_tr_lo, &g_dd_tr_hi); }
    { static const char *only; static int oi; if (!oi) { oi = 1; only = getenv("STE_DD_TRACE_SITE"); } if (only && *only && strcmp(only, site)) return; }
    if (!g_dd_tr_hi || g_ste_fc_n_pub < g_dd_tr_lo || g_ste_fc_n_pub > g_dd_tr_hi || cnt >= 40000u) return;
    if (posp) dd_radial(posp, &rr, &cc);
    cnt++; x.u = posp ? G32(posp) : 0; y.u = posp ? G32(posp + 4) : 0; z.u = posp ? G32(posp + 8) : 0;
    fprintf(stderr, "[DDTRACE] n=%u site=%s obj=0x%08X value=%.4f thr=%.4f pos=(%.1f %.1f %.1f) override=%d rad=%.4f cosf=%.4f\n", g_ste_fc_n_pub, site, obj, depth, thr, x.f, y.f, z.f, dd_active(), rr, cc);
}
/* radial distance of the point at guest address posp (x,y,z floats) from the camera ([0x4322F0], position +0x54), times cam[0x140] (same scale as the game's depth); cosf = forward depth / radial distance.
 * The game's tests use the FORWARD depth dot(p - cam, cam forward): at the same real distance an object at the side of a wide view has a smaller depth than one in the centre, so it is drawn farther out and
 * then vanishes when the camera turns it toward the centre. Numeric draw_distance values use the radial distance instead, so objects hide at the same distance anywhere on screen. */
static int dd_radial(unsigned posp, double *rad, double *cosf)
{
    unsigned cam = G32(0x4322F0u); union { uint32_t u; float f; } v[9]; double dx, dy, dz, r, fw;
    if (!cam || !posp) return 0;
    v[0].u = G32(posp); v[1].u = G32(posp + 4); v[2].u = G32(posp + 8); v[3].u = G32(cam + 0x54); v[4].u = G32(cam + 0x58); v[5].u = G32(cam + 0x5C);
    v[6].u = G32(cam + 0x48); v[7].u = G32(cam + 0x4C); v[8].u = G32(cam + 0x50);
    dx = (double)v[0].f - v[3].f; dy = (double)v[1].f - v[4].f; dz = (double)v[2].f - v[5].f;
    r = sqrt(dx * dx + dy * dy + dz * dz); if (!(r > 0.0)) return 0;
    fw = dx * v[6].f + dy * v[7].f + dz * v[8].f; v[0].u = G32(cam + 0x140);
    *rad = r * (double)v[0].f; *cosf = fw / r; return 1;
}/* before the fcomp [obj+0x88] at 0x13BF08 / 0x13C118 (ecx = object): st0 = depth*scale, compared with obj[0x88] */
int ste_hook_dd_range(void)
{
    union { uint32_t u; float f; } r; double *d = &g_fp_stack[g_fp_top]; int act;
    if (!g_dd_cfgd) ste_dd_parse();
    dd_report();
    if (G32(0x003562B4u) != 4u && G32(0x003562B4u) != 5u) return 0;
    r.u = G32(g_ecx + 0x88); g_dd_n[0]++;
    if (!(*d > (double)r.f)) return 0;
    g_dd_rej[0]++; dd_trace("range", g_ecx, *d, (double)r.f, g_ecx + 0x48);
    if (!dd_active()) return 0;
    if (g_dd_unl) *d = 0.0;
    else { double rad, cf; if (dd_radial(g_ecx + 0x48, &rad, &cf)) *d = rad / (double)g_dd_mul; else *d /= (double)g_dd_mul; }   /* numeric N: radial distance / N */
    if (!(*d > (double)r.f)) g_dd_pass[0]++;
    return 0;
}
/* before the fcompp at 0x17D27A: st0 = copy of ratio = size/depth, st1 = (double)set[4]; the draw continues when st0 > st1 */
int ste_hook_dd_size(void)
{
    double *a = &g_fp_stack[g_fp_top], *b = &g_fp_stack[(g_fp_top + 1) & 7];
    if (!g_dd_cfgd) ste_dd_parse();
    if (G32(0x003562B4u) != 4u && G32(0x003562B4u) != 5u) return 0;
    g_dd_n[1]++;
    if (*a > *b) return 0;
    g_dd_rej[1]++; dd_trace("size", g_ecx, *a, *b, G32(g_esp + 0x1C) + 0x24);
    if (!dd_active()) return 0;
    if (g_dd_unl) *a = 1.0e30;
    else { double rad, cf; if (dd_radial(G32(g_esp + 0x1C) + 0x24, &rad, &cf) && cf > 0.0) *a = *a * cf * (double)g_dd_mul; else *a *= (double)g_dd_mul; }   /* numeric N: size / radial distance, threshold / N */
    if (*a > *b) g_dd_pass[1]++;
    return 0;
}
/* after the fcom [set+0x38] at 0x17CE4C: st0 = ratio stays on the stack (the LOD pick below still uses it), g_fp_cc is the result; the draw continues when ratio > threshold */
int ste_hook_dd_size2(void)
{
    union { uint32_t u; float f; } t; double ratio = g_fp_stack[g_fp_top];
    if (!g_dd_cfgd) ste_dd_parse();
    if (G32(0x003562B4u) != 4u && G32(0x003562B4u) != 5u) return 0;
    t.u = G32(g_ecx + 0x38); g_dd_n[2]++;
    if (ratio > (double)t.f) return 0;
    g_dd_rej[2]++; dd_trace("size2", g_ecx, ratio, (double)t.f, 0);
    if (!dd_active()) return 0;
    if (g_dd_unl || ratio * (double)g_dd_mul > (double)t.f) { g_fp_cc = 0; g_dd_pass[2]++; }
    return 0;
}


/* before the `test al, al` at 0x13C347 in sub_0013C2D0 (the third draw variant of the scenery class): al = sub_0018B7B0(a, b, c) = 1 when the object is too far (c = squared distance, a/b = radii terms).
 * unlimited: al = 0 (draw). N: re-evaluate the same test with the squared distance divided by N*N (the args are still on the stack below esp: ret 0xc left them in place). */
int ste_hook_dd_sphere(void)
{
    union { uint32_t u; float f; } a, b, c, k; float t; int cull;
    if (!g_dd_cfgd) ste_dd_parse();
    if (G32(0x003562B4u) != 4u && G32(0x003562B4u) != 5u) return 0;
    g_dd_n[3]++;
    if (!(g_eax & 0xFFu)) return 0;
    g_dd_rej[3]++; dd_trace("sphere", g_ecx, 0, 0, 0);
    if (!dd_active()) return 0;
    if (g_dd_unl) { g_eax &= ~0xFFu; g_dd_pass[3]++; return 0; }
    a.u = G32(g_esp - 12); b.u = G32(g_esp - 8); c.u = G32(g_esp - 4); k.u = G32(0x002E1FACu);
    c.f /= g_dd_mul * g_dd_mul; t = c.f - a.f - b.f;
    cull = (t > 0.0f) && (a.f * b.f < t * t * k.f);
    if (!cull) { g_eax &= ~0xFFu; g_dd_pass[3]++; }
    return 0;
}

/* ---- c305: draw_distance = unlimited also widens the render-list query volume (SPDX-License-Identifier: GPL-2.0-or-later).
 * The scenery objects of a fight are found by a spatial-tree query (sub_0013DF10, called per render list) against a volume record at 0x3F1150 + list*0xCC: a frustum-shaped slab (near plane d0 at +0x5C, far bound d1 at +0x60, ~1230 units
 * deep in Tokyo = fog start 1250 minus near 20) plus its bounding box (6 floats at +0x64..+0x7C). Objects outside it are never put in the list that the draw loop (sub_0013D830 / sub_0013D770, vtable slot 0x24) walks, so
 * no per-object test can bring them back: that is the pop-in at ~1360 that the cull overrides above do not remove. The list is only read by those draw loops (sub_0013D590 calls a no-op slot).
 * Hook pair: entry of sub_0013DF10 widens d1 and the box about its centre by STE_VIDEO_QUERY_SCALE / [video] query_scale (default 1.5, 1 = off; measured fight cost on the dev PC at 1765x993: 1.5 -2 fps, 2 -8 fps, 4 -30 fps, see docs); the ret of sub_0013DF10 puts the original floats back, so nothing else sees the change.
 * Only while the draw-distance override is active (unlimited); original and numeric draw distances run the game's own query. */
float g_qv_scale_cur = 1.0f;
static struct { unsigned applied, widened, d1, box[6]; } g_qv_s[64]; static unsigned g_qv_last; static unsigned long long g_qv_n;
static void qv_restore(unsigned idx)
{
    unsigned tb, i;
    if (idx >= 64u || !g_qv_s[idx].applied) return;
    tb = 0x3F1150u + idx * 0xCCu; g_qv_s[idx].applied = 0;
    if (G32(tb + 0x60) != g_qv_s[idx].widened) return;   /* the game refilled the record meanwhile: keep the fresh values */
    G32(tb + 0x60) = g_qv_s[idx].d1; for (i = 0; i < 6; ++i) G32(tb + 0x64 + i * 4) = g_qv_s[idx].box[i];
}
int ste_hook_qvol_pre(void)
{
    static float scale = -1.0f;
    unsigned self = g_ecx, idx, tb, i; union { uint32_t u; float f; } d0, d1, v[6]; float c, h;
    if (scale < 0.0f) { scale = ste_ini_float("query_scale", "STE_VIDEO_QUERY_SCALE", 1.5f); if (scale < 1.0f) scale = 1.0f; if (scale > ste_qv_max()) scale = ste_qv_max(); fprintf(stderr, "[QVOL] query_scale=%g (applies with draw_distance=unlimited)\n", scale); fflush(stderr); }
    {   /* c318: query guard + optional load governor (ste_pb_hook.c ste_qv_governor). query_scale is the MAXIMUM: while one frame's pushbuffer fills more than ~40% of the 2 MB ring (two frames are in flight and the
         * writer can overwrite what the render thread has not consumed yet) the scale is pulled down, and (query_scale_auto, default 1; 0 = off) also while the frame load exceeds query_budget_ms (default 14: render-thread busy time or game-thread CPU per frame, smoothed). It creeps back up when calm. */
        static int auto_init; static float last_want; extern volatile int g_ste_qv_auto_on, g_ste_qv_guard_on; extern volatile float g_ste_qv_auto_cap, g_ste_qv_budget_ms, g_ste_qv_max;
        float want = g_qv_scale_live >= 1.0f ? g_qv_scale_live : scale;
        if (!auto_init) { extern volatile float g_ste_qv_ramp; auto_init = 1; g_ste_qv_ramp = ste_ini_float("query_ramp_pct", "STE_VIDEO_QUERY_RAMP", 5.0f); if (g_ste_qv_ramp < 0.0f) g_ste_qv_ramp = 0.0f; if (g_ste_qv_ramp > 100.0f) g_ste_qv_ramp = 100.0f; if (ste_ini_float("query_scale_auto", "STE_VIDEO_QUERY_AUTO", 1.0f) > 0.5f) { float b = ste_ini_float("query_budget_ms", "STE_VIDEO_QUERY_BUDGET_MS", 14.0f); if (b < 6.0f) b = 6.0f; if (b > 30.0f) b = 30.0f; g_ste_qv_budget_ms = b; g_ste_qv_auto_on = 1; } if (getenv("STE_QV_NOGUARD")) auto_init = 2; }
        if (auto_init == 1) { if (want != last_want) { if (want > last_want || g_ste_qv_auto_cap > want) g_ste_qv_auto_cap = want; g_ste_qv_max = want; last_want = want; g_ste_qv_guard_on = want > 1.02f; } }
    }    if (self < 0x10000u || self > 0x07F00000u) return 0;
    idx = G32(self + 0x10); if (idx >= 64u) return 0;
    qv_restore(idx); g_qv_last = idx;
    g_qv_scale_cur = (g_dd_unl && dd_active()) ? (g_qv_scale_live >= 1.0f ? g_qv_scale_live : scale) : 1.0f;   /* c308: read by the STE_PERF reports */
    {   extern volatile int g_ste_qv_guard_on; extern volatile float g_ste_qv_auto_cap; if (g_ste_qv_guard_on && g_qv_scale_cur > g_ste_qv_auto_cap) g_qv_scale_cur = g_ste_qv_auto_cap; }   /* c318 pushbuffer guard / governor cap */
    if (g_qv_scale_cur <= 1.02f) return 0;
    tb = 0x3F1150u + idx * 0xCCu; d0.u = G32(tb + 0x5C); d1.u = G32(tb + 0x60);
    if (!(d1.f - d0.f > 1000.0f && d1.f - d0.f < 1500.0f)) return 0;   /* only the record shape seen in the fights (near 20, far 1250) */
    g_qv_s[idx].d1 = d1.u; for (i = 0; i < 6; ++i) { v[i].u = G32(tb + 0x64 + i * 4); g_qv_s[idx].box[i] = v[i].u; }
    d1.f = d0.f + (d1.f - d0.f) * g_qv_scale_cur; G32(tb + 0x60) = d1.u; g_qv_s[idx].widened = d1.u;
    for (i = 0; i < 3; ++i) { c = 0.5f * (v[i].f + v[i + 3].f); h = 0.5f * (v[i + 3].f - v[i].f) * g_qv_scale_cur; v[i].f = c - h; v[i + 3].f = c + h; G32(tb + 0x64 + i * 4) = v[i].u; G32(tb + 0x70 + i * 4) = v[i + 3].u; }
    g_qv_s[idx].applied = 1;
    if (!g_qv_n++) { fprintf(stderr, "[QVOL] first widened query: list %u far bound %.0f -> %.0f\n", idx, (double)(d1.f - (d1.f - d0.f) * (g_qv_scale_cur - 1.0f) / g_qv_scale_cur - d0.f), (double)(d1.f - d0.f)); fflush(stderr); }
    return 0;
}
int ste_hook_qvol_post(void) { qv_restore(g_qv_last); return 0; }

/* c316: D3DDevice_Swap entry (guest 0x00222630, XbSymbolDatabase). The frame hook that drives the Kelvin pushbuffer replay used to be patched into the generated C by a script; it is a normal ste_hooks.toml hook now, so a build from the published recipe renders. */
extern void ste_pb_frame_hook(void);
int ste_hook_swap(void) { ste_pb_frame_hook(); return 0; }
