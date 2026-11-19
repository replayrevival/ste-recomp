/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ste_textentry.c (c401): typing on the game's on-screen keyboard (profile name screen) with the PC keyboard.
 *
 * The game's keyboard is a script-driven grid (ShellData\ProfileSelect.bsf): rows "A B C D E F G H 1 2 3 / I J K L M N O P 4 5 6 / Q R S T U V W X 7 8 9 / Y Z SPACE END 0", A selects the key under the
 * cursor. Typing therefore means moving that cursor with the pad and pressing A. This module does exactly that, like a very patient player, and reads the screen to know where the cursor is:
 *   - a letter / digit / Space key pressed outside a match is held back for one frame and the frame is looked at (d3d8_grab_frame_rgba): if the four text rows of the keyboard are on screen
 *     (11 + 11 + 11 + 5 glyph groups; the key under the cursor is drawn white) the key becomes a queued character, otherwise the key goes on to the game as before;
 *   - while characters are queued, every ~12 frames the cursor is located again and ONE pad press (D-pad or A) is injected for port 0; Tab = END (finish the name).
 * Nothing is written into the game's memory or scripts, the rules of the grid are never assumed (every move is checked against the screen), and with no keyboard grid on screen the keyboard
 * behaves exactly as before. [input] keyboard_text_entry (env STE_TEXT_ENTRY=0 turns it off). STE_TE_TEST="ABC D" queues characters as soon as the keyboard is seen (test harness, no keyboard needed). */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;
extern int d3d8_grab_frame_rgba(float fx0, float fy0, float fx1, float fy1, const unsigned char **px, unsigned *w, unsigned *h);
extern void xbox_InputKeySet(int vk, int down);

static int g_on = -1;
static char g_q[64]; static volatile LONG g_qh, g_qt;     /* character queue (producer: window thread, consumer: frame thread) */
static volatile LONG g_vis_until;                          /* frame number until which the keyboard grid counts as visible (last positive look + 90 frames), 0 = not seen */
static volatile LONG g_fr;                                 /* presented frame counter, written by the frame hook */
static volatile LONG g_pend;                               /* a held-back key waits for a look (vk, 0 = none) */
static volatile LONG g_btn_mask, g_btn_a;                  /* injected pad state read by the pad poll */
static unsigned char g_phys[256];                          /* keys physically down (window thread) */
static unsigned char g_fwd[256];                           /* held-back keys that were handed on to the game after a negative look */
static unsigned g_hold, g_rel, g_wait;                     /* frame counters of the press state machine */
static int g_last_row = -1, g_last_idx = -1, g_stuck;
static char g_test[64]; static int g_test_done;

static int g_slow(void) { static int v = -1; if (v < 0) { const char *e = getenv("STE_TE_SLOW"), *f = getenv("STE_TE_SPEED"); v = (e && e[0] == (char)49) ? 1 : (f && f[0] == (char)50) ? 2 : 3; } return v; }
static int te_enabled(void) { if (g_on < 0) { const char *e = getenv("STE_TEXT_ENTRY"); g_on = !(e && e[0] == '0'); if (g_on) { const char *t = getenv("STE_TE_TEST"); if (t) strncpy(g_test, t, sizeof g_test - 1); } } return g_on; }

static uint32_t guest_rd(uint32_t a) { return *(volatile uint32_t *)((uintptr_t)a + (uintptr_t)g_xbox_mem_offset); }
static int in_match(void) { return guest_rd(0x003562B4u) == 4u; }   /* the game-mode word the shot harness uses for "a fight is running" */

static char vk_to_char(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return (char)vk;
    if (vk >= '0' && vk <= '9') return (char)vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (char)('0' + (vk - VK_NUMPAD0));
    if (vk == VK_SPACE) return ' ';
    if (vk == VK_TAB) return '\t';   /* END */
    return 0;
}
static void q_push(char c) { LONG t = g_qt; if (((t + 1) & 63) == g_qh) return; g_q[t & 63] = c; g_qt = (t + 1) & 63; }
static int q_peek(char *c) { if (g_qh == g_qt) return 0; *c = g_q[g_qh & 63]; return 1; }
static void q_pop(void) { if (g_qh != g_qt) g_qh = (g_qh + 1) & 63; }
static void q_clear(void) { g_qh = g_qt; }

/* ---- window thread ---- */
/* returns 1 when the key is taken by the text entry (the game must not see it) */
int ste_te_key_down(int vk, int repeat)
{
    char c;
    if (!te_enabled() || vk < 0 || vk > 255) return 0;
    c = vk_to_char(vk); if (!c) return 0;
    if (repeat) return !g_fwd[vk] && (g_pend == vk || (g_phys[vk] && g_vis_until && (LONG)(g_vis_until - g_fr) > 0));   /* auto-repeat of a taken key: swallow, no new character */
    if (g_vis_until && (LONG)(g_vis_until - g_fr) > 0) { g_phys[vk] = 1; q_push(c); return 1; }   /* keyboard on screen: queue */
    if (in_match()) return 0;
    if (g_pend == 0) { g_phys[vk] = 1; g_pend = vk; return 1; }                          /* hold back, the frame hook decides */
    return 0;
}
/* returns 1 when the key-up belongs to a taken key */
int ste_te_key_up(int vk)
{
    int taken;
    if (vk < 0 || vk > 255 || g_on <= 0) return 0;
    if (g_fwd[vk]) { g_fwd[vk] = 0; g_phys[vk] = 0; xbox_InputKeySet(vk, 0); return 1; }   /* it was handed to the game after a negative look: release it there too */
    taken = vk_to_char(vk) && g_phys[vk]; g_phys[vk] = 0;
    return taken;
}

/* ---- pad poll thread ---- */
void ste_te_pad_apply(int port, uint16_t *wButtons, uint8_t *analog)
{
    LONG m, a; if (port != 0 || g_on <= 0) return;
    m = g_btn_mask; a = g_btn_a;
    if (m) *wButtons |= (uint16_t)m;
    if (a) analog[0] = 255;   /* XINPUT_GAMEPAD_A */
}

/* ---- keyboard grid recognition ---- */
typedef struct { int nrow; int cnt[4]; int xa[4][12], xb[4][12]; int white[4][12]; int ya[4], yb[4]; } Grid;

static int is_text_px(const unsigned char *p, int *white)
{
    int r = p[0], g = p[1], b = p[2];
    if (r > 215 && g > 215 && b > 215) { *white = 1; return 1; }
    *white = 0; return b > 190 && g > 110 && r < 120;
}

static int g_dbg_nb, g_dbg_c[4], g_dbg_x[2][12][2];

/* glyph groups of one text band: columns with text pixels, merged while the gap is <= gap pixels; groups thinner than minw (panel borders) or with fewer than minpx pixels are dropped */
static int group_row(const unsigned char *px, unsigned w, int ya, int yb, int gap, int minpx, int minw, Grid *g, int k)
{
    int n = 0, s = -1, last = -1000, cnt = 0, whc = 0; unsigned x;
    for (x = 0; x <= w; ++x) {
        int col = 0, wc = 0, yy;
        if (x < w) for (yy = ya; yy <= yb; ++yy) { int wh; if (is_text_px(px + ((size_t)yy * w + x) * 4u, &wh)) { col++; wc += wh; } }
        if (col) {
            if (s < 0) { s = (int)x; cnt = 0; whc = 0; }
            else if ((int)x - last > gap) { if (cnt >= minpx && last - s + 1 >= minw && n < 12) { g->xa[k][n] = s; g->xb[k][n] = last; g->white[k][n] = whc * 6 > cnt; n++; } s = (int)x; cnt = 0; whc = 0; }
            cnt += col; whc += wc; last = (int)x;
        }
    }
    if (s >= 0 && cnt >= minpx && last - s + 1 >= minw && n < 12) { g->xa[k][n] = s; g->xb[k][n] = last; g->white[k][n] = whc * 6 > cnt; n++; }
    return n;
}

static int find_grid(const unsigned char *px, unsigned w, unsigned h, Grid *G)
{
    static int *rs; static size_t rcap; unsigned x, y; int bands[64][2], nb = 0, in = 0, y0 = 0, i;
    if (rcap < h) { free(rs); rs = (int *)malloc(h * sizeof(int)); rcap = rs ? h : 0; } if (!rs) return 0;
    for (y = 0; y < h; ++y) { int n = 0, wh; const unsigned char *r = px + (size_t)y * w * 4u; for (x = 0; x < w; ++x) n += is_text_px(r + x * 4u, &wh); rs[y] = n; }
    for (y = 0; y <= h; ++y) {
        int on = y < h && rs[y] >= (int)(w / 30u);
        if (on && !in) { in = 1; y0 = (int)y; }
        else if (!on && in) { in = 0; if (nb < 64 && (int)y - y0 >= (int)(h / 90u) && (int)y - y0 <= (int)(h / 12u)) { bands[nb][0] = y0; bands[nb][1] = (int)y - 1; nb++; } }
    }
    g_dbg_nb = nb;
    for (i = 0; i + 3 < nb; ++i) {   /* four consecutive text bands of similar height and regular spacing */
        int k, ok = 1, bh = bands[i][1] - bands[i][0] + 1, pitch = bands[i + 1][0] - bands[i][0]; Grid g; memset(&g, 0, sizeof g);
        if (pitch < bh) continue;
        for (k = 0; k < 4 && ok; ++k) {
            static const int gapf[] = { 50, 30, 40, 60, 70, 80, 25 };   /* gap candidates in % of the band height: take the first that yields the expected group count */
            int ya = bands[i + k][0], yb = bands[i + k][1], hh = yb - ya + 1, want = k < 3 ? 11 : 5, minpx = (hh * hh) / 40 + 3, minw = (hh * 35) / 100, gi, n = 0;
            if (k && (bands[i + k][0] - bands[i + k - 1][0] > pitch * 5 / 4 + 2 || bands[i + k][0] - bands[i + k - 1][0] < pitch * 3 / 4 - 2)) { ok = 0; break; }
            g.ya[k] = ya; g.yb[k] = yb;
            for (gi = 0; gi < (int)(sizeof gapf / sizeof gapf[0]); ++gi) { int gap = hh * gapf[gi] / 100; if (gap < 2) gap = 2; memset(g.xa[k], 0, sizeof g.xa[k]); memset(g.xb[k], 0, sizeof g.xb[k]); memset(g.white[k], 0, sizeof g.white[k]); n = group_row(px, w, ya, yb, gap, minpx, minw, &g, k); if (n == want) break; }
            g.cnt[k] = n;
        }
        if (!ok) continue;
        g_dbg_c[0] = g.cnt[0]; g_dbg_c[1] = g.cnt[1]; g_dbg_c[2] = g.cnt[2]; g_dbg_c[3] = g.cnt[3];
        { int q; for (q = 0; q < 12; ++q) { g_dbg_x[0][q][0] = g.xa[0][q]; g_dbg_x[0][q][1] = g.xb[0][q]; g_dbg_x[1][q][0] = g.xa[3][q]; g_dbg_x[1][q][1] = g.xb[3][q]; } }
        if (g.cnt[0] == 11 && g.cnt[1] == 11 && g.cnt[2] == 11 && g.cnt[3] == 5) { g.nrow = 4; *G = g; return 1; }
    }
    return 0;
}

static const char *k_rows[3] = { "ABCDEFGH123", "IJKLMNOP456", "QRSTUVWX789" };
static int char_to_key(char c, int *row, int *idx)
{
    int r; const char *p;
    for (r = 0; r < 3; ++r) { p = strchr(k_rows[r], c); if (p && c) { *row = r; *idx = (int)(p - k_rows[r]); return 1; } }
    if (c == 'Y') { *row = 3; *idx = 0; return 1; } if (c == 'Z') { *row = 3; *idx = 1; return 1; }
    if (c == ' ') { *row = 3; *idx = 2; return 1; } if (c == '\t') { *row = 3; *idx = 3; return 1; } if (c == '0') { *row = 3; *idx = 4; return 1; }
    return 0;
}

#define BTN_UP 0x0001
#define BTN_DOWN 0x0002
#define BTN_LEFT 0x0004
#define BTN_RIGHT 0x0008

/* 0.1.7: STE_RAND_SEED=<n> (showcase recordings, off by default): the game's C runtime rand() (seed word at guest 0x003F8984, LCG 214013/2531011, sub_001A5317) is reset to <n> on the frame a match
 * starts (game-mode word 0x003562B4 becomes 4), so two recordings of the same pad script draw the same random numbers. It does not make the fight identical by itself: the simulation is driven by the
 * frame time, so equal takes also need the same frame pacing (see REPORT_0.1.7). Only a harness option, nothing in the game files changes. */
static void seed_frame(unsigned frame)
{
    static int init, on; static uint32_t seed, last_mode; static unsigned n;
    uint32_t mode;
    if (!init) { const char *e = getenv("STE_RAND_SEED"); init = 1; if (e && *e) { seed = (uint32_t)strtoul(e, NULL, 0); on = 1; fprintf(stderr, "[SEED] STE_RAND_SEED=%u: rand() reset at every match start\n", seed); fflush(stderr); } }
    if (!on) return;
    mode = guest_rd(0x003562B4u);
    if (mode == 4u && last_mode != 4u) { *(volatile uint32_t *)((uintptr_t)0x003F8984u + (uintptr_t)g_xbox_mem_offset) = seed; if (n++ < 20u) { fprintf(stderr, "[SEED] f=%u match start: rand seed set to %u\n", frame, seed); fflush(stderr); } }
    last_mode = mode;
}

void ste_te_frame(unsigned frame)
{
    seed_frame(frame);
    const unsigned char *px; unsigned w, h; Grid G; char c;
    if (g_on <= 0 && !te_enabled()) return;
    if (g_on <= 0) return;
    g_fr = (LONG)frame;
    /* press state machine: hold the injected button, then release, then wait for the game to settle */
    if (g_hold) { if (--g_hold == 0) { g_btn_mask = 0; g_btn_a = 0; g_rel = g_slow() == 1 ? 5u : g_slow() == 2 ? 2u : 1u; } return; }
    if (g_rel) { --g_rel; return; }
    if (g_wait) { --g_wait; return; }
    if (!g_pend && g_qh == g_qt && !(g_test[0] && !g_test_done)) return;
    { static unsigned dn; int gok = d3d8_grab_frame_rgba(0.0f, 0.0f, 0.75f, 0.72f, &px, &w, &h); int fg = gok && find_grid(px, w, h, &G);
      { static int dumped; static unsigned lastd; const char *dd = getenv("STE_TE_DUMP"); if (gok && dd && dumped < 4 && frame > 10400u && frame - lastd > 700u) { char fn[MAX_PATH]; FILE *f; dumped++; lastd = frame; _snprintf(fn, sizeof fn, "%s\\te_look_%u.ppm", dd, frame); f = fopen(fn, "wb"); if (f) { unsigned yy, xx; fprintf(f, "P6\n%u %u\n255\n", w, h); for (yy = 0; yy < h; ++yy) for (xx = 0; xx < w; ++xx) fwrite(px + ((size_t)yy * w + xx) * 4u, 1, 3, f); fclose(f); fprintf(stderr, "[TEXTENTRY] dumped %s\n", fn); } } }
      if (!fg && (dn++ % 200u) == 0u && getenv("STE_TE_LOG")) { fprintf(stderr, "[TEXTENTRY] look f=%u grab=%d %ux%u bands=%d counts=%d,%d,%d,%d\n", frame, gok, gok ? w : 0, gok ? h : 0, g_dbg_nb, g_dbg_c[0], g_dbg_c[1], g_dbg_c[2], g_dbg_c[3]); if (g_dbg_c[0]) { int q; fprintf(stderr, "[TEXTENTRY]   row3:"); for (q = 0; q < g_dbg_c[3]; ++q) fprintf(stderr, " %d-%d", g_dbg_x[1][q][0], g_dbg_x[1][q][1]); fprintf(stderr, "  row0:"); for (q = 0; q < g_dbg_c[0]; ++q) fprintf(stderr, " %d-%d", g_dbg_x[0][q][0], g_dbg_x[0][q][1]); fprintf(stderr, "\n"); } fflush(stderr); }
      if (!fg) {
        /* no keyboard on screen: give the held-back key to the game and drop what was queued */
        int vk = g_pend; int was_vis = g_vis_until != 0; g_vis_until = 0;
        if (was_vis && gok && getenv("STE_TE_DUMP")) { static int fd; if (fd < 3) { char fn[MAX_PATH]; FILE *f; fd++; _snprintf(fn, sizeof fn, "%s\\te_fail_%u.ppm", getenv("STE_TE_DUMP"), frame); f = fopen(fn, "wb"); if (f) { unsigned yy, xx; fprintf(f, "P6\n%u %u\n255\n", w, h); for (yy = 0; yy < h; ++yy) for (xx = 0; xx < w; ++xx) fwrite(px + ((size_t)yy * w + xx) * 4u, 1, 3, f); fclose(f); fprintf(stderr, "[TEXTENTRY] grid lost, dumped %s counts=%d,%d,%d,%d bands=%d\n", fn, g_dbg_c[0], g_dbg_c[1], g_dbg_c[2], g_dbg_c[3], g_dbg_nb); } } }
        q_clear();
        if (vk) { g_pend = 0; if (g_phys[vk]) { g_fwd[vk] = 1; xbox_InputKeySet(vk, 1); } }
        return;
      } }
    g_vis_until = (LONG)(frame + 90u);
    if (g_pend) { int vk = g_pend; char cc = vk_to_char(vk); g_pend = 0; if (cc) q_push(cc); }
    if (g_test[0] && !g_test_done) { size_t i; for (i = 0; g_test[i]; ++i) q_push(g_test[i]); g_test_done = 1; fprintf(stderr, "[TEXTENTRY] test string \"%s\" queued\n", g_test); fflush(stderr); }
    if (!q_peek(&c)) return;
    {   int trow, tidx, crow = -1, cidx = -1, r, k, dir = 0, abtn = 0;
        if (!char_to_key(c, &trow, &tidx)) { q_pop(); return; }
        for (r = 0; r < 4; ++r) for (k = 0; k < G.cnt[r]; ++k) if (G.white[r][k]) { crow = r; cidx = k; }
        if (crow < 0) { if ((g_stuck % 10) == 0) { fprintf(stderr, "[TEXTENTRY] f=%u no highlighted key found (try %d)\n", frame, g_stuck); fflush(stderr); } if (++g_stuck > 40) { g_stuck = 0; q_clear(); } return; }   /* the highlight blinks: look again */
        g_stuck = 0;
        if (crow == trow && cidx == tidx) { abtn = 1; q_pop(); }
        else if (crow == trow) {
            int n = G.cnt[crow], d = (tidx - cidx + n) % n;   /* steps to the right with wrap */
            dir = (d <= n - d) ? BTN_RIGHT : BTN_LEFT;
            if (crow == g_last_row && cidx == g_last_idx && ++g_stuck > 2) { dir = (dir == BTN_RIGHT) ? BTN_LEFT : BTN_RIGHT; }
        } else dir = (trow > crow) ? BTN_DOWN : BTN_UP;
        g_last_row = crow; g_last_idx = cidx;
        fprintf(stderr, "[TEXTENTRY] f=%u char=%d cursor=(%d,%d) target=(%d,%d) -> %s\n", frame, (int)c, crow, cidx, trow, tidx, abtn ? "A" : dir == BTN_RIGHT ? "R" : dir == BTN_LEFT ? "L" : dir == BTN_DOWN ? "D" : "U");
        if (abtn) g_btn_a = 1; else g_btn_mask = dir;
        g_hold = g_slow() == 1 ? 4u : g_slow() == 2 ? 3u : 2u; g_wait = g_slow() == 1 ? 3u : g_slow() == 2 ? 1u : 0u;   /* c425: default fast = 3 frames per press; STE_TE_SPEED=2 the c421 6 frames, STE_TE_SLOW=1 the 0.1.7 12 frames */   /* c421: 12 -> 6 frames per press (every move is still checked against the screen); STE_TE_SLOW=1 = the 0.1.7 timing */
    }
}
