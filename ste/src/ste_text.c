/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ste_text.c (c378): re-wording of the game's on-screen English text, applied to the strings AFTER the game has loaded them.
 *
 * Why not at load time (c373): the bundles hold the UI text as per-language value tables next to lower-case lookup ids; the game checks the data while it loads
 * it, and every edit made to a file buffer during the read broke the load. The finished value table in the game's memory is only read when a string is drawn
 * (verified: overwriting "PRESS START" in memory after loading changes the title screen text at once), so the strings are patched there.
 *
 * How: the kernel bridge calls ste_text_patch() with every buffer a file read has filled; it only LOOKS for an anchor (the title-screen string "PRESS START" or
 * "Press START to ..."), which tells that a bundle with a value table is being loaded, and keeps a scanner thread running for 45 s (a scan of the game's memory every 2 s).
 * A background thread scans the guest RAM above the exe image for table entries: a string is replaced only when it matches a rule as a WHOLE string (NUL before and
 * after) and the replacement fits in the old string plus its NUL padding; single words (START, BACK) only within 2000 bytes of an anchor, i.e. inside the same value
 * table, never in script identifiers or code data. Nothing is written to disk; the game files are never touched.
 *
 *  - button words follow [input] button_prompts (STE_BUTTON_PROMPTS_RESOLVED): xbox -> MENU / LT / RT, playstation -> OPTIONS / L2 / R2. (The footer word BACK next to the B picture means "go back", not the Back button: left alone.)
 *  - platform words ("Xbox console" ...) become computer wording for xbox and playstation.
 *  - xbox_original (and unknown) changes nothing: the text is exactly as shipped (no thread, no scan). STE_TEXT_PATCH=0 turns the feature off.
 * Every replaced string is logged once. The wording is ours; only the short strings needed to match are stored. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;
extern __declspec(thread) int g_ste_probe_guard;

typedef struct { const char *src; const char *xb[3]; const char *ps[3]; int single; } TextRule;   /* candidates are tried in order; single = only next to an anchor */
#define PLAT(s, d) { s, { d, 0, 0 }, { d, 0, 0 }, 0 }
static const TextRule k_rules[] = {
    /* button words */
    { "PRESS START",                    { "PRESS MENU", 0, 0 },                     { "PRESS OPTIONS", "PRESS MENU", 0 }, 0 },
    { "Press START to Continue.",       { "Press MENU to Continue.", 0, 0 },        { "Press OPTIONS to Continue.", "Press MENU to Continue.", 0 }, 0 },
    { "Press START to add COM",         { "Press MENU to add COM", 0, 0 },          { "Press OPTIONS to add COM", "Press MENU to add COM", 0 }, 0 },
    { "Press START to join",            { "Press MENU to join", 0, 0 },             { "Press OPTIONS to join", "Press MENU to join", 0 }, 0 },
    { "START",                          { "MENU", 0, 0 },                           { "OPTIONS", "MENU", 0 }, 1 },
    { "Pull left trigger or right trigger to dodge sideways.", { "Pull LT or RT to dodge sideways.", 0, 0 }, { "Pull L2 or R2 to dodge sideways.", 0, 0 }, 0 },
    { "Pull L and R triggers to dodge sideways.",              { "Pull LT and RT to dodge sideways.", 0, 0 }, { "Pull L2 and R2 to dodge sideways.", 0, 0 }, 0 },
    /* platform words */
    PLAT("Xbox console",                                              "Computer"),
    PLAT("Hard Disk",                                                 "Computer"),
    PLAT("Save %s to the Xbox console?",                              "Save %s to this computer?"),
    PLAT("Saving %s. Please don't turn off your Xbox console.",       "Saving %s. Please don't turn off your computer."),
    PLAT("Deleting %s.  Please don't turn off your Xbox console.",    "Deleting %s.  Please don't turn off your computer."),
    PLAT("Deleting %s. Please don't turn off your Xbox console.",     "Deleting %s. Please don't turn off your computer."),
    PLAT("Your Xbox does not have enough free blocks to save games.  You need to free %d more blocks.", "This PC does not have enough free blocks to save games.  You need to free %d more blocks."),
    PLAT("The game does not have enough openings for all the players on your Xbox console", "The game does not have enough openings for all the players on this computer"),
};
#define N_RULES (sizeof k_rules / sizeof k_rules[0])
static const char *k_anchors[] = { "PRESS START", "PRESS MENU", "PRESS OPTIONS", "Press START to", "Press MENU to", "Press OPTIONS to" };
#define N_ANCH (sizeof k_anchors / sizeof k_anchors[0])

static int g_style = -1;   /* 0 = none, 1 = xbox, 2 = playstation */
static unsigned g_logged[N_RULES];
static volatile LONG g_started;
static volatile ULONGLONG g_last_read;   /* GetTickCount64() of the last file read; the scanner runs for 20 s after it */
static volatile LONG g_total;

static void style_init(void)
{
    const char *s = getenv("STE_BUTTON_PROMPTS_RESOLVED"), *off = getenv("STE_TEXT_PATCH");
    g_style = 0;
    if (off && off[0] == '0') { fprintf(stderr, "[TEXT] wording replacement off (STE_TEXT_PATCH=0)\n"); return; }
    if (s && !strcmp(s, "xbox")) g_style = 1;
    else if (s && !strcmp(s, "playstation")) g_style = 2;
    fprintf(stderr, "[TEXT] wording style=%s (button_prompts resolved=%s)\n", g_style == 1 ? "xbox" : g_style == 2 ? "playstation" : "original (no changes)", s ? s : "(unset)");
}

static const unsigned char *find(const unsigned char *h, size_t hn, const char *needle, size_t nn)
{
    const unsigned char *p = h, *end = h + hn - nn + 1;
    if (hn < nn) return NULL;
    while (p < end && (p = (const unsigned char *)memchr(p, (unsigned char)needle[0], (size_t)(end - p))) != NULL) {
        if (!memcmp(p, needle, nn)) return p;
        ++p;
    }
    return NULL;
}

/* one region of readable+writable host memory */
static void patch_region(unsigned char *b, size_t n, uintptr_t guest_base, const unsigned char **anch, int nanch)
{
    size_t r;
    for (r = 0; r < N_RULES; r++) {
        const TextRule *t = &k_rules[r]; size_t sl = strlen(t->src); const char *const *cand = g_style == 1 ? t->xb : t->ps; size_t pos = 0;
        while (pos + sl + 1 < n) {
            const unsigned char *hit = find(b + pos, n - pos - 1, t->src, sl);
            size_t at; int ok = 1, c, a;
            if (!hit) break;
            at = (size_t)(hit - b); pos = at + 1;
            if (at == 0 || b[at + sl] != 0) continue;   /* a string ends with a NUL ... */
            if (t->single ? b[at - 1] != 0 : (b[at - 1] >= 0x20 && b[at - 1] < 0x7F)) continue;   /* ... and begins at a boundary: a NUL, or (copies kept inside heap objects, multi-word strings only) any non-text byte */
            if (t->single) { ok = 0; for (a = 0; a < nanch; a++) { ptrdiff_t dd = (ptrdiff_t)(anch[a] - b) - (ptrdiff_t)at; if (dd > -2000 && dd < 2000) { ok = 1; break; } } }
            if (!ok) continue;
            {
                size_t nul = 0, k;
                for (k = at + sl; k < n && b[k] == 0; k++) nul++;
                if (k >= n) nul = nul > 1 ? nul - 1 : 1;
                for (c = 0; c < 3 && cand[c]; c++) {
                    size_t nl = strlen(cand[c]);
                    if (nl <= sl + nul - 1) {
                        memcpy(b + at, cand[c], nl); memset(b + at + nl, 0, sl + nul - nl);
                        InterlockedIncrement(&g_total);
                        if (g_logged[r]++ < 2u) { fprintf(stderr, "[TEXT] '%s' -> '%s' (guest 0x%08X)\n", t->src, cand[c], (unsigned)(guest_base + at)); fflush(stderr); }
                        break;
                    }
                }
            }
        }
    }
}

static void scan_ram_inner(void)
{
    uintptr_t gstart = 0x00500000u, gend = 0x04000000u, p = (uintptr_t)g_xbox_mem_offset + gstart, end = (uintptr_t)g_xbox_mem_offset + gend;
    const unsigned char *anch[64]; int nanch = 0, pass;
    for (pass = 0; pass < 2; pass++) {   /* pass 0: anchors, pass 1: rules */
        uintptr_t q = p;
        while (q < end) {
            MEMORY_BASIC_INFORMATION mbi; size_t len; int rw, ro;
            if (!VirtualQuery((void *)q, &mbi, sizeof mbi)) break;
            len = mbi.RegionSize - (size_t)(q - (uintptr_t)mbi.BaseAddress);
            if (q + len > end) len = (size_t)(end - q);
            rw = mbi.State == MEM_COMMIT && (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE || mbi.Protect == PAGE_READONLY || mbi.Protect == PAGE_EXECUTE_READ) && !(mbi.Protect & PAGE_GUARD);
            ro = rw && (mbi.Protect == PAGE_READONLY || mbi.Protect == PAGE_EXECUTE_READ);   /* the game keeps some loaded data read-only: lifted for the edit, restored after */
            if (rw && len > 16) {
                if (pass == 0) { size_t i; for (i = 0; i < N_ANCH && nanch < 64; i++) { const unsigned char *h = (const unsigned char *)q, *e = h + len; size_t al = strlen(k_anchors[i]);
                                     while (nanch < 64 && h < e && (h = find(h, (size_t)(e - h), k_anchors[i], al)) != NULL) { if (h > (const unsigned char *)q && (h[-1] < 0x20 || h[-1] >= 0x7F)) anch[nanch++] = h; h++; } } }
                else {
                    DWORD oldp = 0; int lifted = 0;
                    if (ro) { int a2, has = 0; for (a2 = 0; a2 < nanch; a2++) if ((uintptr_t)anch[a2] >= q && (uintptr_t)anch[a2] < q + len) has = 1; if (has) lifted = VirtualProtect((void *)q, len, PAGE_READWRITE, &oldp); }
                    if (!ro || lifted) patch_region((unsigned char *)q, len, gstart + (q - p), anch, nanch);
                    if (lifted) { DWORD o2; VirtualProtect((void *)q, len, oldp, &o2); }
                }
            }
            q += len ? len : 0x1000;
        }
        if (pass == 0) { static int shown; if (shown++ < 3) { fprintf(stderr, "[TEXT] scan: %d anchor(s) in guest RAM\n", nanch); fflush(stderr); } }
        if (pass == 0 && nanch == 0) return;   /* no value table in memory: nothing to do */
    }
}

static void scan_ram(void)
{
    g_ste_probe_guard = 1;
    __try { scan_ram_inner(); } __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 3) { fprintf(stderr, "[TEXT] scan hit memory that went away, retried at the next scan\n"); fflush(stderr); } }
    g_ste_probe_guard = 0;
}

static DWORD WINAPI text_thread(LPVOID u)
{
    (void)u;
    for (;;) {   /* after a read that carried an anchor: scan the game's memory every 2 s for 45 s (the finished value table is placed there when the whole bundle has been read) */
        ULONGLONG now; static ULONGLONG next;
        Sleep(200);
        now = GetTickCount64();
        if (g_last_read && now >= next) { next = now + (now - g_last_read < 120000 ? 1500 : 4000); scan_ram(); }   /* text objects keep a heap copy of their string, made when a screen is created: rescan for the whole session */
    }
    return 0;
}

/* called by the kernel bridge with every buffer a file read has filled (game thread): only looks for an anchor, never writes */
void ste_text_patch(void *buf, uint32_t n)
{
    (void)buf;
    if (g_style < 0) style_init();
    if (!g_style || n < 4096u) return;   /* the value tables arrive in big sequential reads */
    g_last_read = GetTickCount64();
    if (!InterlockedCompareExchange(&g_started, 1, 0)) CloseHandle(CreateThread(NULL, 0, text_thread, NULL, 0, NULL));
}
