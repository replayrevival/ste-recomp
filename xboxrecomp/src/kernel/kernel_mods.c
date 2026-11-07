/*
 * kernel_mods.c - mods\ override folder (STE c117)
 *
 * A mod is a folder  <mods root>\<folder>\  holding manifest.json plus files at game-relative paths
 * (ActionData\x.bsf, ShellData\ControlRoom.bdl, bundle\Godzilla2k.zip ...).  When the title opens a path under the game
 * directory (xbox_translate_path) and an ENABLED mod ships a file with the same game-relative path, the mod's file is opened
 * instead.  The game directory is never written and never modified; a mod only changes which host file a read open resolves to.
 *
 * Config: mods root = STE_MODS_DIR or <exe dir>\mods; STE_MODS=0 turns the whole system off.
 *   mods\order.json (optional): {"api":1,"order":["id-a","id-b"],"enabled":{"id-a":true,"id-b":false}}
 *   manifest.json: see docs\formats\mod_manifest.md.
 * Load order: ids listed in order.json first (in that order), then the rest by manifest "priority" then id; later mods win.
 * Validation (per mod, in load order): enabled, game hash (sha256 of default.xbe) if the manifest lists game_hashes,
 * dependencies present and active earlier, conflicts with an already active mod. A mod that fails is skipped and the reason logged.
 * The dry-run report (stderr + mods\last_report.txt) lists every game-relative path a mod overrides or adds, and mod-vs-mod overlaps.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include "kernel.h"
#include <stdarg.h>
#include <wctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <bcrypt.h>

/* ---------------- tiny JSON (objects, arrays, strings, numbers, bools, null) ---------------- */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;
typedef struct jv { jtype t; double n; int b; char *s; struct jv **kids; char **keys; int nk; int cap; } jv;

static jv *jnew(jtype t) { jv *v = (jv *)calloc(1, sizeof *v); if (v) v->t = t; return v; }
static void jfree(jv *v)
{
    int i; if (!v) return;
    for (i = 0; i < v->nk; i++) { jfree(v->kids[i]); if (v->keys) free(v->keys[i]); }
    free(v->kids); free(v->keys); free(v->s); free(v);
}
static void jadd(jv *c, const char *key, jv *k)
{
    if (c->nk == c->cap) {
        int nc = c->cap ? c->cap * 2 : 4;
        c->kids = (jv **)realloc(c->kids, (size_t)nc * sizeof(jv *));
        c->keys = (char **)realloc(c->keys, (size_t)nc * sizeof(char *));
        c->cap = nc;
    }
    c->kids[c->nk] = k; c->keys[c->nk] = key ? _strdup(key) : NULL; c->nk++;
}
static const char *jskip(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }
static const char *jparse(const char *p, jv **out, int depth);
static const char *jstr(const char *p, char **out)
{
    size_t cap = 32, n = 0; char *b = (char *)malloc(cap);
    if (*p != '"') { free(b); return NULL; }
    p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            c = *p++;
            if (c == 'n') c = '\n'; else if (c == 't') c = '\t'; else if (c == 'r') c = '\r';
            else if (c == 'u') { unsigned u = 0; int k; for (k = 0; k < 4 && p[k]; k++) { char h = p[k]; u = u * 16 + (unsigned)(h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0'); } p += (k < 4 ? k : 4); c = u < 128 ? (char)u : '?'; }
        }
        if (n + 2 > cap) { cap *= 2; b = (char *)realloc(b, cap); }
        b[n++] = c;
    }
    if (*p != '"') { free(b); return NULL; }
    b[n] = 0; *out = b; return p + 1;
}
static const char *jparse(const char *p, jv **out, int depth)
{
    jv *v; p = jskip(p);
    if (depth > 16 || !*p) return NULL;
    if (*p == '{') {
        v = jnew(J_OBJ); p = jskip(p + 1);
        while (*p && *p != '}') {
            char *key = NULL; jv *kid = NULL;
            p = jstr(jskip(p), &key); if (!p) { jfree(v); return NULL; }
            p = jskip(p); if (*p != ':') { free(key); jfree(v); return NULL; }
            p = jparse(p + 1, &kid, depth + 1); if (!p) { free(key); jfree(v); return NULL; }
            jadd(v, key, kid); free(key);
            p = jskip(p); if (*p == ',') p = jskip(p + 1);
        }
        if (*p != '}') { jfree(v); return NULL; }
        *out = v; return p + 1;
    }
    if (*p == '[') {
        v = jnew(J_ARR); p = jskip(p + 1);
        while (*p && *p != ']') {
            jv *kid = NULL; p = jparse(p, &kid, depth + 1); if (!p) { jfree(v); return NULL; }
            jadd(v, NULL, kid); p = jskip(p); if (*p == ',') p = jskip(p + 1);
        }
        if (*p != ']') { jfree(v); return NULL; }
        *out = v; return p + 1;
    }
    if (*p == '"') { char *s = NULL; p = jstr(p, &s); if (!p) return NULL; v = jnew(J_STR); v->s = s; *out = v; return p; }
    if (!strncmp(p, "true", 4)) { v = jnew(J_BOOL); v->b = 1; *out = v; return p + 4; }
    if (!strncmp(p, "false", 5)) { v = jnew(J_BOOL); v->b = 0; *out = v; return p + 5; }
    if (!strncmp(p, "null", 4)) { v = jnew(J_NULL); *out = v; return p + 4; }
    { char *e; double d = strtod(p, &e); if (e == p) return NULL; v = jnew(J_NUM); v->n = d; *out = v; return e; }
}
static jv *jget(const jv *o, const char *key)
{
    int i; if (!o || o->t != J_OBJ) return NULL;
    for (i = 0; i < o->nk; i++) if (o->keys[i] && !strcmp(o->keys[i], key)) return o->kids[i];
    return NULL;
}
static const char *jgets(const jv *o, const char *key, const char *def) { jv *v = jget(o, key); return (v && v->t == J_STR) ? v->s : def; }
static jv *jload(const WCHAR *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD sz, got = 0; char *buf; jv *v = NULL;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz > (4u << 20)) { CloseHandle(h); return NULL; }
    buf = (char *)malloc((size_t)sz + 1);
    if (buf && ReadFile(h, buf, sz, &got, NULL)) { buf[got] = 0; { const char *p = buf; if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB) p += 3; if (!jparse(p, &v, 0)) v = NULL; } }
    free(buf); CloseHandle(h); return v;
}

/* ---------------- mods ---------------- */
#define MODS_MAX 64
typedef struct {
    char id[96], name[128], version[32], folder[MAX_PATH];
    WCHAR dir[MAX_PATH];
    int priority, enabled, order_pos, active;
    jv *manifest;
    char why[192];            /* why it is not active */
} mod_t;

static mod_t g_mods[MODS_MAX]; static int g_nmods;
static int g_active[MODS_MAX]; static int g_nactive;        /* indices into g_mods, load order (later wins) */
static int g_mods_ready, g_mods_off;
static WCHAR g_game_dir[MAX_PATH];
static char g_cache_tag[24];   /* c124 */

/* override table: lowercase game-relative path -> host path of the winning mod file */
typedef struct { WCHAR *rel; WCHAR *host; int mod; } ov_t;
static ov_t *g_ov; static size_t g_ov_cap, g_ov_n;

static unsigned long hashw(const WCHAR *s) { unsigned long h = 5381; while (*s) h = h * 33 + (unsigned long)*s++; return h; }
static void ov_put(const WCHAR *rel_lc, const WCHAR *host, int mod, int *replaced)
{
    size_t i;
    if ((g_ov_n + 1) * 2 > g_ov_cap) {
        size_t nc = g_ov_cap ? g_ov_cap * 2 : 256; ov_t *n = (ov_t *)calloc(nc, sizeof(ov_t)); size_t k;
        for (k = 0; k < g_ov_cap; k++) if (g_ov[k].rel) { size_t j = hashw(g_ov[k].rel) & (nc - 1); while (n[j].rel) j = (j + 1) & (nc - 1); n[j] = g_ov[k]; }
        free(g_ov); g_ov = n; g_ov_cap = nc;
    }
    i = hashw(rel_lc) & (g_ov_cap - 1);
    while (g_ov[i].rel) {
        if (!wcscmp(g_ov[i].rel, rel_lc)) { free(g_ov[i].host); g_ov[i].host = _wcsdup(host); if (replaced) *replaced = g_ov[i].mod; g_ov[i].mod = mod; return; }
        i = (i + 1) & (g_ov_cap - 1);
    }
    g_ov[i].rel = _wcsdup(rel_lc); g_ov[i].host = _wcsdup(host); g_ov[i].mod = mod; g_ov_n++;
    if (replaced) *replaced = -1;
}

static void sha256_file_hex(const WCHAR *path, char out[65])
{
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE h = NULL; HANDLE f; unsigned char buf[65536], dig[32]; DWORD got; int i;
    strcpy(out, "");
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 && BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        while (ReadFile(f, buf, sizeof buf, &got, NULL) && got) BCryptHashData(h, buf, got, 0);
        if (BCryptFinishHash(h, dig, 32, 0) == 0) for (i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", dig[i]);
    }
    if (h) BCryptDestroyHash(h); if (alg) BCryptCloseAlgorithmProvider(alg, 0); CloseHandle(f);
}

static void lcw(WCHAR *s) { for (; *s; s++) { if (*s == L'/') *s = L'\\'; *s = (WCHAR)towlower(*s); } }

static FILE *g_rep;
static void rep(const char *fmt, ...)
{
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fprintf(stderr, "[MODS] %s\n", b); if (g_rep) fprintf(g_rep, "%s\n", b);
}

static void scan_mod(int mi, const WCHAR *dir, const WCHAR *rel_prefix, int *nover, int *nnew)
{
    WIN32_FIND_DATAW fd; WCHAR pat[MAX_PATH]; HANDLE h;
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pat, &fd); if (h == INVALID_HANDLE_VALUE) return;
    do {
        WCHAR rel[MAX_PATH], full[MAX_PATH];
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (rel_prefix[0]) swprintf_s(rel, MAX_PATH, L"%s\\%s", rel_prefix, fd.cFileName); else wcscpy_s(rel, MAX_PATH, fd.cFileName);
        swprintf_s(full, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { scan_mod(mi, full, rel, nover, nnew); continue; }
        if (!rel_prefix[0] && (!_wcsicmp(fd.cFileName, L"manifest.json") || !_wcsicmp(fd.cFileName, L"README.md") || !_wcsicmp(fd.cFileName, L"manifest.md") || !_wcsicmp(fd.cFileName, L"README.txt"))) continue;
        {
            WCHAR lc[MAX_PATH], gp[MAX_PATH]; int rp = -1; DWORD ga;
            wcscpy_s(lc, MAX_PATH, rel); lcw(lc);
            swprintf_s(gp, MAX_PATH, L"%s\\%s", g_game_dir, rel); ga = GetFileAttributesW(gp);
            ov_put(lc, full, mi, &rp);
            if (rp >= 0) rep("  %S: %S also provided by mod '%s' (load order: '%s' wins)", L"overlap", rel, g_mods[rp].id, g_mods[mi].id);
            if (ga != INVALID_FILE_ATTRIBUTES) { (*nover)++; rep("  override %S  <- %s", rel, g_mods[mi].id); }
            else { (*nnew)++; rep("  NEW      %S  <- %s  (not in the game dir; reachable by exact name only, directory listings do not include it)", rel, g_mods[mi].id); }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int find_mod(const char *id) { int i; for (i = 0; i < g_nmods; i++) if (!strcmp(g_mods[i].id, id)) return i; return -1; }
static int cmp_mods(const void *a, const void *b)
{
    const mod_t *x = &g_mods[*(const int *)a], *y = &g_mods[*(const int *)b];
    int ox = x->order_pos < 0 ? 1 << 20 : x->order_pos, oy = y->order_pos < 0 ? 1 << 20 : y->order_pos;
    if (ox != oy) return ox - oy;
    if (x->priority != y->priority) return x->priority - y->priority;
    return strcmp(x->id, y->id);
}

void xbox_mods_init(const WCHAR *game_dir)
{
    WCHAR root[MAX_PATH], pat[MAX_PATH], path[MAX_PATH], xbe[MAX_PATH]; const char *e; WIN32_FIND_DATAW fd; HANDLE h;
    jv *order = NULL; int idx[MODS_MAX], i, j; char xbe_hash[65] = "";
    if (g_mods_ready) return;
    g_mods_ready = 1;
    e = getenv("STE_MODS"); if (e && *e == '0') { g_mods_off = 1; return; }
    wcsncpy_s(g_game_dir, MAX_PATH, game_dir, _TRUNCATE);
    e = getenv("STE_MODS_DIR");
    if (e && *e) MultiByteToWideChar(CP_UTF8, 0, e, -1, root, MAX_PATH);
    else { WCHAR *sl; if (!GetModuleFileNameW(NULL, root, MAX_PATH) || !(sl = wcsrchr(root, L'\\'))) return; *sl = 0; wcscat_s(root, MAX_PATH, L"\\mods"); }
    if (GetFileAttributesW(root) == INVALID_FILE_ATTRIBUTES) { g_mods_off = 1; return; }
    swprintf_s(pat, MAX_PATH, L"%s\\*", root);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            jv *m; mod_t *M; const char *id, *v; jv *pr, *en;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || fd.cFileName[0] == L'_' || g_nmods >= MODS_MAX) continue;
            swprintf_s(path, MAX_PATH, L"%s\\%s\\manifest.json", root, fd.cFileName);
            m = jload(path);
            if (!m) { fprintf(stderr, "[MODS] %S: no valid manifest.json, ignored\n", fd.cFileName); continue; }
            id = jgets(m, "id", NULL);
            if (!id || !*id) { fprintf(stderr, "[MODS] %S: manifest has no id, ignored\n", fd.cFileName); jfree(m); continue; }
            M = &g_mods[g_nmods++]; memset(M, 0, sizeof *M);
            strncpy_s(M->id, sizeof M->id, id, _TRUNCATE); strncpy_s(M->name, sizeof M->name, jgets(m, "name", id), _TRUNCATE);
            v = jgets(m, "version", ""); strncpy_s(M->version, sizeof M->version, v, _TRUNCATE);
            swprintf_s(M->dir, MAX_PATH, L"%s\\%s", root, fd.cFileName); WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, M->folder, MAX_PATH, NULL, NULL);
            pr = jget(m, "priority"); M->priority = (pr && pr->t == J_NUM) ? (int)pr->n : 100;
            en = jget(m, "enabled"); M->enabled = (en && en->t == J_BOOL) ? en->b : 1;
            M->order_pos = -1; M->manifest = m;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    swprintf_s(path, MAX_PATH, L"%s\\order.json", root); order = jload(path);
    if (order) {
        jv *ord = jget(order, "order"), *en = jget(order, "enabled");
        if (ord && ord->t == J_ARR) for (i = 0; i < ord->nk; i++) if (ord->kids[i]->t == J_STR) { j = find_mod(ord->kids[i]->s); if (j >= 0) g_mods[j].order_pos = i; else fprintf(stderr, "[MODS] order.json lists unknown mod '%s'\n", ord->kids[i]->s); }
        if (en && en->t == J_OBJ) for (i = 0; i < en->nk; i++) if (en->keys[i] && en->kids[i]->t == J_BOOL) { j = find_mod(en->keys[i]); if (j >= 0) g_mods[j].enabled = en->kids[i]->b; }
    }
    e = getenv("STE_MODS_ENABLE");   /* test/launcher override: ONLY these ids (comma separated) are enabled; order.json/manifest flags are ignored for enabling */
    if (e && *e) {
        char list[512]; char *tok, *ctx = NULL; strncpy_s(list, sizeof list, e, _TRUNCATE);
        for (i = 0; i < g_nmods; i++) g_mods[i].enabled = 0;
        for (tok = strtok_s(list, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) { j = find_mod(tok); if (j >= 0) g_mods[j].enabled = 1; else fprintf(stderr, "[MODS] STE_MODS_ENABLE names unknown mod '%s'\n", tok); }
    }
    for (i = 0; i < g_nmods; i++) idx[i] = i;
    qsort(idx, (size_t)g_nmods, sizeof(int), cmp_mods);
    swprintf_s(path, MAX_PATH, L"%s\\last_report.txt", root); g_rep = _wfopen(path, L"w");
    fprintf(stderr, "[MODS] root %S : %d mod(s) found\n", root, g_nmods);
    if (g_rep) fprintf(g_rep, "mods root %ls: %d mod(s) found\n", root, g_nmods);
    swprintf_s(xbe, MAX_PATH, L"%s\\default.xbe", g_game_dir);
    for (i = 0; i < g_nmods; i++) {
        mod_t *M = &g_mods[idx[i]]; jv *gh, *dep, *cf; int ok = 1, k;
        if (!M->enabled) { strcpy_s(M->why, sizeof M->why, "disabled"); rep("%s %s: disabled", M->id, M->version); continue; }
        gh = jget(M->manifest, "game_hashes");
        if (gh && gh->t == J_ARR && gh->nk > 0) {
            int match = 0;
            if (!xbe_hash[0]) sha256_file_hex(xbe, xbe_hash);
            for (k = 0; k < gh->nk; k++) if (gh->kids[k]->t == J_STR) { const char *s = gh->kids[k]->s; if (!_strnicmp(s, "sha256:", 7)) s += 7; if (!_stricmp(s, xbe_hash)) match = 1; }
            if (!match) { ok = 0; snprintf(M->why, sizeof M->why, "game hash mismatch (default.xbe sha256 %s)", xbe_hash); }
        }
        dep = jget(M->manifest, "dependencies");
        if (ok && dep && dep->t == J_ARR) for (k = 0; k < dep->nk && ok; k++) if (dep->kids[k]->t == J_STR) {
            int d = find_mod(dep->kids[k]->s);
            if (d < 0 || !g_mods[d].active) { ok = 0; snprintf(M->why, sizeof M->why, "dependency '%s' is missing or not active before it", dep->kids[k]->s); }
        }
        cf = jget(M->manifest, "conflicts");
        if (ok && cf && cf->t == J_ARR) for (k = 0; k < cf->nk && ok; k++) if (cf->kids[k]->t == J_STR) {
            int d = find_mod(cf->kids[k]->s);
            if (d >= 0 && g_mods[d].active) { ok = 0; snprintf(M->why, sizeof M->why, "conflicts with active mod '%s'", cf->kids[k]->s); }
        }
        if (ok && !g_nactive) {}   /* nothing */
        if (!ok) { rep("%s %s: SKIPPED - %s", M->id, M->version, M->why); continue; }
        M->active = 1; g_active[g_nactive++] = idx[i];
        { int nover = 0, nnew = 0; rep("%s %s (%s) ACTIVE, load position %d:", M->id, M->version, M->folder, g_nactive); scan_mod(idx[i], M->dir, L"", &nover, &nnew); rep("  = %d override(s), %d new file(s)", nover, nnew); }
    }
    {   /* c124: the title caches the loose game files it reads (Partition5 cache_N.bin = Cache\ in the save dir) and later reads the cache instead of the disc, so an override would never be seen. A non-empty tag moves the cache to Cache.<tag>: one cache per active override set. */
        size_t k; unsigned long acc = 0;
        for (k = 0; k < g_ov_cap; k++) if (g_ov[k].rel) {
            WIN32_FILE_ATTRIBUTE_DATA fa; unsigned long h = hashw(g_ov[k].rel) * 31u + hashw(g_ov[k].host);
            if (GetFileAttributesExW(g_ov[k].host, GetFileExInfoStandard, &fa)) h = h * 31u + fa.nFileSizeLow + fa.ftLastWriteTime.dwLowDateTime * 7u;
            acc += h * 2654435761u;
        }
        if (g_ov_n) snprintf(g_cache_tag, sizeof g_cache_tag, "mods_%08lx", acc & 0xFFFFFFFFul);
        if (g_cache_tag[0]) fprintf(stderr, "[MODS] cache dir tag %s (Cache.%s)\n", g_cache_tag, g_cache_tag);
    }
    if (g_rep) { fclose(g_rep); g_rep = NULL; }
    jfree(order);
}

/* 1 when path lies inside the mods root (mod files are read-only for the title). */
int xbox_mods_owns(const WCHAR *path)
{
    size_t i;
    if (!g_mods_ready || g_mods_off || !path) return 0;
    for (i = 0; i < (size_t)g_nactive; i++) { const WCHAR *d = g_mods[g_active[i]].dir; size_t n = wcslen(d); if (!_wcsnicmp(path, d, n) && (path[n] == L'\\' || !path[n])) return 1; }
    return 0;
}

/* If an active mod ships the file the title is opening, rewrite host_path to it. Called for every translated path under the game dir. */
int xbox_mods_resolve(WCHAR *host_path, DWORD size)
{
    size_t gl; WCHAR lc[MAX_PATH]; size_t i;
    if (!g_mods_ready) return 0;
    {   /* c121: STE_MODS_TRACE=1 logs every game-dir path the title opens (first 4000), to find which files a mod must provide */
        static int tr = -1; static volatile LONG tn;
        if (tr < 0) { const char *e = getenv("STE_MODS_TRACE"); tr = e && atoi(e) > 0; }
        if (tr && !g_mods_off) { size_t g0 = wcslen(g_game_dir); if (_wcsnicmp(host_path, g_game_dir, g0) == 0 && host_path[g0] == L'\\' && InterlockedIncrement(&tn) <= 4000) { fprintf(stderr, "[MODSTRACE] %S\n", host_path + g0 + 1); fflush(stderr); } }
    }
    if (g_mods_off || !g_ov_n) return 0;
    gl = wcslen(g_game_dir);
    if (_wcsnicmp(host_path, g_game_dir, gl) != 0 || host_path[gl] != L'\\') return 0;
    wcscpy_s(lc, MAX_PATH, host_path + gl + 1); lcw(lc);
    i = hashw(lc) & (g_ov_cap - 1);
    while (g_ov[i].rel) {
        if (!wcscmp(g_ov[i].rel, lc)) {
            static volatile LONG logged;
            if (InterlockedIncrement(&logged) <= 200) { fprintf(stderr, "[MODS] open %S -> %S (mod %s)\n", lc, g_ov[i].host, g_mods[g_ov[i].mod].id); fflush(stderr); }
            wcsncpy_s(host_path, size, g_ov[i].host, _TRUNCATE);
            return 1;
        }
        i = (i + 1) & (g_ov_cap - 1);
    }
    return 0;
}

/* c124: "" without overrides, else mods_<hash of the override set>; kernel_path.c appends it to the Cache directory name. */
const char *xbox_mods_cache_tag(void) { return g_mods_ready && !g_mods_off ? g_cache_tag : ""; }
