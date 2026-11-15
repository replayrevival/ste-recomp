/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ste_text.c (c373): in-memory re-wording of the game's on-screen English text.
 *
 * The title keeps its UI text as NUL-terminated ASCII strings in the bundle files (.bdl / .bsf). The kernel bridge calls
 * ste_text_patch() on every buffer a file read has just filled. A string is replaced only when it matches a table entry as a
 * WHOLE string (NUL before and after) and the replacement fits in the old string plus its alignment padding, so no offset in the
 * file changes. Nothing is written back to disk; the game files are never touched.
 *
 *  - button words follow [input] button_prompts (STE_BUTTON_PROMPTS_RESOLVED): xbox -> MENU / VIEW / LT / RT, playstation -> OPTIONS / CREATE / L2 / R2.
 *  - platform words ("Xbox console" ...) become computer wording for xbox and playstation.
 *  - xbox_original (and anything unknown) changes nothing: the text is exactly as shipped.
 * Every replaced string is logged once ("[TEXT] ..."). The wording is ours; no game text is stored beyond the short strings needed to match. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *src; const char *xb[3]; const char *ps[3]; } TextRule;   /* candidates, tried in order; NULL = none */

/* Platform wording is the same for both styles (candidate 0). */
#define PLAT(s, d) { s, { d, 0, 0 }, { d, 0, 0 } }
static const TextRule k_rules[] = {
    /* button words */
    { "PRESS START",                    { "PRESS MENU", 0, 0 },                     { "PRESS OPTIONS", "PRESS MENU", 0 } },
    { "Press START to Continue.",       { "Press MENU to Continue.", 0, 0 },        { "Press OPTIONS to Continue.", "Press MENU to Continue.", 0 } },
    { "Press START to add COM",         { "Press MENU to add COM", 0, 0 },          { "Press OPTIONS to add COM", "Press MENU to add COM", 0 } },
    { "Press START to join",            { "Press MENU to join", 0, 0 },             { "Press OPTIONS to join", "Press MENU to join", 0 } },
    { "SELECT or BACK",                 { "SELECT or VIEW", 0, 0 },                 { "SELECT or CREATE", "SELECT or SHARE", 0 } },
    { "START",                          { "MENU", 0, 0 },                           { "OPTIONS", "MENU", 0 } },
    { "BACK",                           { "VIEW", 0, 0 },                           { "CREATE", "SHARE", 0 } },
    { "Back",                           { "View", 0, 0 },                           { "Create", "Share", 0 } },
    { "Pull left trigger or right trigger to dodge sideways.", { "Pull LT or RT to dodge sideways.", 0, 0 }, { "Pull L2 or R2 to dodge sideways.", 0, 0 } },
    { "Pull L and R triggers to dodge sideways.",              { "Pull LT and RT to dodge sideways.", 0, 0 }, { "Pull L2 and R2 to dodge sideways.", 0, 0 } },
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

static int g_style = -1;   /* 0 = none (xbox_original / unknown), 1 = xbox, 2 = playstation */
static unsigned g_logged[N_RULES];
static unsigned long g_mask = ~0ul;   /* debug: STE_TEXT_MASK=<hex bit per rule> enables only those rules */
static long g_total;

static void style_init(void)
{
    const char *s = getenv("STE_BUTTON_PROMPTS_RESOLVED");
    g_style = 0;
    if (!getenv("STE_TEXT_PATCH")) { fprintf(stderr, "[TEXT] wording patch is experimental and off (STE_TEXT_PATCH=1 enables it; see docs/REPORT_0.1.4.md)\n"); return; }
    if (s && !strcmp(s, "xbox")) g_style = 1;
    else if (s && !strcmp(s, "playstation")) g_style = 2;
    { const char *m = getenv("STE_TEXT_MASK"); if (m && *m) g_mask = strtoul(m, NULL, 16); }
    fprintf(stderr, "[TEXT] wording style=%s (button_prompts resolved=%s)\n", g_style == 1 ? "xbox" : g_style == 2 ? "playstation" : "original (no changes)", s ? s : "(unset)");
}

void ste_text_patch(void *buf, uint32_t n)
{
    unsigned char *b = (unsigned char *)buf;
    size_t r; uint32_t i;
    if (g_style < 0) style_init();
    if (!g_style || n < 8u) return;
    for (r = 0; r < N_RULES; r++) {
        const TextRule *t = &k_rules[r];
        if (!((g_mask >> r) & 1ul)) continue;
        size_t sl = strlen(t->src); const char *const *cand = g_style == 1 ? t->xb : t->ps;
        uint32_t pos = 0; int occ = 0;
        while (pos + sl < n) {
            const unsigned char *hit = memchr(b + pos, (unsigned char)t->src[0], n - pos - sl);
            uint32_t at;
            if (!hit) break;
            at = (uint32_t)(hit - b);
            pos = at + 1u;
            if (memcmp(b + at, t->src, sl) != 0 || b[at + sl] != 0 || (at > 0u && b[at - 1u] != 0)) continue;   /* whole string only */
            occ++;
            { static int want = -1; if (want < 0) { const char *e = getenv("STE_TEXT_OCC"); want = e ? atoi(e) : 0; } if (want > 0 && occ != want) continue; }   /* debug: patch only the Nth copy within a read */
            {
                uint32_t nul = 0, k; int c;
                for (k = at + (uint32_t)sl; k < n && b[k] == 0; k++) nul++;
                if (k >= n) nul = nul > 1u ? nul - 1u : 1u;   /* the buffer ended inside the padding: be conservative */
                for (c = 0; c < 3 && cand[c]; c++) {
                    size_t nl = strlen(cand[c]);
                    if (nl <= sl + nul - 1u) {
                        if (getenv("STE_TEXT_TEST_PERM") && nl == sl) { size_t q; const char *pm = getenv("STE_TEXT_TEST_PERM"); for (q = 0; q < nl; q++) b[at + q] = (unsigned char)(pm[0] == '2' ? t->src[q] : t->src[nl - 1 - q]); }   /* debug: same letters, reversed */
                        else
                        memcpy(b + at, cand[c], nl);
                        memset(b + at + nl, 0, sl + nul - nl);
                        g_total++;
                        if (g_logged[r]++ < 3u) fprintf(stderr, "[TEXT] '%s' -> '%s' (read n=%u at=%u first=%02X%02X%02X%02X)\n", t->src, cand[c], (unsigned)n, (unsigned)at, b[0], b[1], b[2], b[3]);
                        break;
                    }
                    if (!cand[c + 1] || c == 2) { if (g_logged[r]++ < 1u) fprintf(stderr, "[TEXT] '%s': no replacement fits (room %u)\n", t->src, (unsigned)(sl + nul - 1u)); }
                }
            }
        }
    }
}
