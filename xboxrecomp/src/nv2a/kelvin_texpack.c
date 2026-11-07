/* SPDX-License-Identifier: GPL-2.0-or-later
 * Kelvin HD texture packs (c128). See kelvin_texpack.h.
 *
 * Third-party: stb_image.h v2.28 (Sean Barrett, public domain / MIT) is compiled in for PNG decoding (third_party\stb_image.h).
 * The PNG writer below is our own (fixed-Huffman deflate). */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kelvin_texpack.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"

/* ------------------------------------------------------------------ config */
static int g_init, g_packs_active, g_dump_on, g_want_mips;
static unsigned g_aniso; static float g_mip_bias;
static char g_dump_dir[MAX_PATH];

static void ini_path(char *ini)
{
    const char *e = getenv("STE_VIDEO_INI"); char *p;
    ini[0] = 0;
    if (e && e[0]) strncpy(ini, e, MAX_PATH - 1);
    else { GetModuleFileNameA(NULL, ini, MAX_PATH); p = strrchr(ini, 0x5C); if (p) strcpy(p + 1, "ste_video.ini"); }
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) ini[0] = 0;
}
static void cfg_get(const char *envname, const char *key, const char *ini, char *out, size_t n)
{
    const char *e = getenv(envname);
    out[0] = 0;
    if (e && e[0]) { strncpy(out, e, n - 1); out[n - 1] = 0; return; }
    if (ini[0]) GetPrivateProfileStringA("video", key, "", out, (DWORD)n, ini);
}

/* ------------------------------------------------------------------ pack index: key -> file path (later packs win) */
typedef struct { uint64_t key; char *path; uint8_t *data; uint32_t w, h; volatile LONG state; } PackEnt;   /* c178: state 0 = not loaded, 1 = background thread decoding, 2 = decoded (data), 3 = handed out */
static PackEnt *g_tab; static size_t g_tab_cap, g_tab_n;
static void tab_put(uint64_t key, const char *path)
{
    size_t i;
    if (!g_tab_cap) { g_tab_cap = 1u << 15; g_tab = (PackEnt *)calloc(g_tab_cap, sizeof *g_tab); if (!g_tab) return; }
    if (g_tab_n * 2 >= g_tab_cap) {   /* grow */
        PackEnt *old = g_tab; size_t oc = g_tab_cap, k;
        g_tab_cap *= 2; g_tab = (PackEnt *)calloc(g_tab_cap, sizeof *g_tab); if (!g_tab) { g_tab = old; g_tab_cap = oc; return; }
        g_tab_n = 0;
        for (k = 0; k < oc; ++k) if (old[k].path) { size_t j = (size_t)(old[k].key * 0x9E3779B97F4A7C15ull >> 20) & (g_tab_cap - 1); while (g_tab[j].path) j = (j + 1) & (g_tab_cap - 1); g_tab[j] = old[k]; g_tab_n++; }
        free(old);
    }
    i = (size_t)(key * 0x9E3779B97F4A7C15ull >> 20) & (g_tab_cap - 1);
    while (g_tab[i].path && g_tab[i].key != key) i = (i + 1) & (g_tab_cap - 1);
    if (g_tab[i].path) free(g_tab[i].path); else g_tab_n++;
    g_tab[i].key = key; g_tab[i].path = _strdup(path);
}
static const char *tab_get(uint64_t key)
{
    size_t i;
    if (!g_tab_cap) return NULL;
    i = (size_t)(key * 0x9E3779B97F4A7C15ull >> 20) & (g_tab_cap - 1);
    while (g_tab[i].path) { if (g_tab[i].key == key) return g_tab[i].path; i = (i + 1) & (g_tab_cap - 1); }
    return NULL;
}

/* file stem "<16 hex>[_anything].png" -> key */
static int stem_key(const char *name, uint64_t *key)
{
    uint64_t k = 0; int i;
    for (i = 0; i < 16; ++i) {
        char c = name[i]; int v;
        if (c >= '0' && c <= '9') v = c - '0'; else if (c >= 'a' && c <= 'f') v = c - 'a' + 10; else if (c >= 'A' && c <= 'F') v = c - 'A' + 10; else return 0;
        k = (k << 4) | (uint64_t)v;
    }
    if (name[16] != 0 && name[16] != '.' && name[16] != '_') return 0;
    *key = k; return 1;
}

static int scan_dir(const char *dir, int depth)
{
    char pat[MAX_PATH + 8]; WIN32_FIND_DATAA fd; HANDLE h; int n = 0;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char path[MAX_PATH];
        if (fd.cFileName[0] == '.') continue;
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 3) n += scan_dir(path, depth + 1); continue; }
        { size_t l = strlen(fd.cFileName); uint64_t key;
          if (l > 4 && !_stricmp(fd.cFileName + l - 4, ".png") && stem_key(fd.cFileName, &key)) { tab_put(key, path); n++; } }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* tiny manifest reader: the manifests are generated/pretty JSON; look a key up textually */
static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb"); char *b; long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (8 << 20)) { fclose(f); return NULL; }
    b = (char *)malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
    n = (long)fread(b, 1, (size_t)n, f); b[n] = 0; fclose(f); if (len) *len = (size_t)n; return b;
}
static const char *find_key(const char *j, const char *key)
{
    char pat[64]; const char *p; snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(j, pat); if (!p) return NULL;
    p += strlen(pat); while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return NULL; p++; while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}
static void get_str(const char *j, const char *key, char *out, size_t n)
{
    const char *p = find_key(j, key); size_t i = 0; out[0] = 0;
    if (!p || *p != '"') return;
    p++; while (*p && *p != '"' && i + 1 < n) { if (*p == '\\' && p[1]) p++; out[i++] = *p++; }
    out[i] = 0;
}
typedef struct { char id[96], ver[32], name[96], dir[MAX_PATH]; int enabled, prio; } PackInfo;
static int cmp_pack(const void *a, const void *b) { const PackInfo *x = (const PackInfo *)a, *y = (const PackInfo *)b; return x->prio != y->prio ? x->prio - y->prio : strcmp(x->id, y->id); }

static int id_listed(const char *list, const char *id)
{
    const char *p = list; size_t l = strlen(id);
    while (*p) { const char *e = strchr(p, ','); size_t n = e ? (size_t)(e - p) : strlen(p); if (n == l && !_strnicmp(p, id, n)) return 1; if (!e) break; p = e + 1; }
    return 0;
}

void texpack_init(void)
{
    char ini[MAX_PATH], b[MAX_PATH], dir[MAX_PATH]; const char *off, *en; PackInfo packs[64]; int np = 0, i;
    if (g_init) return;
    g_init = 1;
    ini_path(ini);
    cfg_get("STE_VIDEO_ANISOTROPY", "anisotropy", ini, b, sizeof b);
    g_aniso = b[0] ? (unsigned)atoi(b) : 0; if (g_aniso != 2 && g_aniso != 4 && g_aniso != 8 && g_aniso != 16) g_aniso = 0;
    cfg_get("STE_VIDEO_MIP_BIAS", "mip_bias", ini, b, sizeof b);
    g_mip_bias = b[0] ? (float)atof(b) : 0.0f; if (g_mip_bias < -2.0f) g_mip_bias = -2.0f; if (g_mip_bias > 2.0f) g_mip_bias = 2.0f;
    cfg_get("STE_TEXDUMP", "texture_dump", ini, g_dump_dir, sizeof g_dump_dir);
    if (g_dump_dir[0] == '0' && !g_dump_dir[1]) g_dump_dir[0] = 0;
    if (g_dump_dir[0]) { CreateDirectoryA(g_dump_dir, NULL); g_dump_on = 1; }

    off = getenv("STE_TEXPACKS");
    cfg_get("STE_TEXTUREPACKS", "texturepacks", ini, dir, sizeof dir);
    if (!dir[0]) { char *p; GetModuleFileNameA(NULL, dir, MAX_PATH); p = strrchr(dir, 0x5C); if (p) strcpy(p + 1, "texturepacks"); }
    en = getenv("STE_TEXPACKS_ENABLE");
    if (!(off && off[0] == '0' && !off[1])) {
        char pat[MAX_PATH + 8]; WIN32_FIND_DATAA fd; HANDLE h;
        snprintf(pat, sizeof pat, "%s\\*", dir);
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char mp[MAX_PATH]; char *j; PackInfo *pi;
                if (fd.cFileName[0] == '.' || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || np >= 64) continue;
                pi = &packs[np]; memset(pi, 0, sizeof *pi);
                snprintf(pi->dir, sizeof pi->dir, "%s\\%s", dir, fd.cFileName);
                snprintf(mp, sizeof mp, "%s\\manifest.json", pi->dir);
                j = read_all(mp, NULL);
                if (!j) { fprintf(stderr, "[TEXPACK] %s: no manifest.json, skipped\n", fd.cFileName); continue; }
                get_str(j, "id", pi->id, sizeof pi->id); if (!pi->id[0]) strncpy(pi->id, fd.cFileName, sizeof pi->id - 1);
                get_str(j, "version", pi->ver, sizeof pi->ver); get_str(j, "name", pi->name, sizeof pi->name);
                { const char *p = find_key(j, "enabled"); pi->enabled = p && !strncmp(p, "true", 4); }
                { const char *p = find_key(j, "priority"); pi->prio = p ? atoi(p) : 100; }
                if (en && en[0]) pi->enabled = id_listed(en, pi->id);
                free(j); np++;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        qsort(packs, (size_t)np, sizeof packs[0], cmp_pack);
        for (i = 0; i < np; ++i) {
            if (!packs[i].enabled) { fprintf(stderr, "[TEXPACK] %s %s: disabled\n", packs[i].id, packs[i].ver); continue; }
            { int n = scan_dir(packs[i].dir, 0);
              fprintf(stderr, "[TEXPACK] %s %s (%s) ACTIVE priority %d: %d texture file(s)\n", packs[i].id, packs[i].ver, packs[i].name, packs[i].prio, n);
              if (n) g_packs_active = 1; }
        }
        fprintf(stderr, "[TEXPACK] dir %s: %d pack(s), %u distinct replacement key(s)\n", dir, np, (unsigned)g_tab_n);
    }
    g_want_mips = g_packs_active || g_aniso || g_mip_bias != 0.0f;
    { extern void texpack_start_preload(void); texpack_start_preload(); }   /* c178 */
    fprintf(stderr, "[TEXPACK] anisotropy=%u mip_bias=%.2f dump=%s mips=%d\n", g_aniso, g_mip_bias, g_dump_on ? g_dump_dir : "off", g_want_mips);
    fflush(stderr);
}

int texpack_packs_active(void) { texpack_init(); return g_packs_active; }
int texpack_dump_on(void) { texpack_init(); return g_dump_on; }
unsigned texpack_aniso(void) { texpack_init(); return g_aniso; }
float texpack_mip_bias(void) { texpack_init(); return g_mip_bias; }
int texpack_want_mips(void) { texpack_init(); return g_want_mips; }

uint64_t texpack_key(uint64_t h, uint32_t format, uint32_t w, uint32_t hh)
{
    uint64_t k = h ^ ((uint64_t)format * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)w << 48) ^ ((uint64_t)hh << 32);
    k ^= k >> 29; k *= 0xBF58476D1CE4E5B9ull; k ^= k >> 32;
    return k;
}

/* ------------------------------------------------------------------ loading */
static uint8_t *texpack_decode_path(const char *path, uint32_t *w, uint32_t *h);
static PackEnt *tab_ent(uint64_t key)
{
    size_t i;
    if (!g_tab_cap) return NULL;
    i = (size_t)(key * 0x9E3779B97F4A7C15ull >> 20) & (g_tab_cap - 1);
    while (g_tab[i].path) { if (g_tab[i].key == key) return &g_tab[i]; i = (i + 1) & (g_tab_cap - 1); }
    return NULL;
}
/* c178: pre-decode every pack PNG on a background thread (low priority) so the first use of a replacement on the render thread costs a memcpy + upload, not a 2048x2048 PNG decode
 * (the 4x Tokyo pilot spent 3.8 s in 120 first-use uploads, one 2.5 s hitch at fight start). STE_TEXPACK_PRELOAD=0 disables; memory cap STE_TEXPACK_PRELOAD_MB (default 1536). */
static DWORD WINAPI preload_thread(LPVOID unused)
{
    size_t k; unsigned long long bytes = 0, cap = 1536ull << 20; const char *e = getenv("STE_TEXPACK_PRELOAD_MB"); unsigned n = 0; ULONGLONG t0 = GetTickCount64(); (void)unused;
    if (e && atoi(e) > 0) cap = (unsigned long long)atoi(e) << 20;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (k = 0; k < g_tab_cap; ++k) {
        PackEnt *pe = &g_tab[k]; uint32_t w = 0, h = 0; uint8_t *px;
        if (!pe->path || bytes >= cap) continue;
        if (InterlockedCompareExchange(&pe->state, 1, 0) != 0) continue;
        px = texpack_decode_path(pe->path, &w, &h);
        if (px) { pe->data = px; pe->w = w; pe->h = h; bytes += (unsigned long long)w * h * 4u; n++; MemoryBarrier(); InterlockedExchange(&pe->state, 2); }
        else InterlockedExchange(&pe->state, 0);
    }
    fprintf(stderr, "[TEXPACK] preload: %u texture(s), %.0f MB decoded in %.1f s\n", n, (double)bytes / 1048576.0, (double)(GetTickCount64() - t0) / 1000.0); fflush(stderr);
    return 0;
}
void texpack_start_preload(void)
{
    const char *e = getenv("STE_TEXPACK_PRELOAD");
    if (e && e[0] == '0') return;
    if (g_packs_active && g_tab_n) CloseHandle(CreateThread(NULL, 0, preload_thread, NULL, 0, NULL));
}
uint8_t *texpack_load(uint64_t key, uint32_t *w, uint32_t *h)
{
    PackEnt *pe = tab_ent(key);
    if (!pe) return NULL;
    for (;;) {
        LONG st = pe->state;
        if (st == 2) { if (InterlockedCompareExchange(&pe->state, 3, 2) == 2) { uint8_t *d = pe->data; pe->data = NULL; *w = pe->w; *h = pe->h; return d; } continue; }
        if (st == 1) { Sleep(1); continue; }   /* the background thread is decoding exactly this one */
        break;
    }
    return texpack_decode_path(pe->path, w, h);
}
static uint8_t *texpack_decode_path(const char *path, uint32_t *w, uint32_t *h)
{
    FILE *f; unsigned char *file; long n; int iw, ih, ch; unsigned char *px; uint8_t *out; size_t i, cnt;
    if (!path) return NULL;
    f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (128 << 20)) { fclose(f); return NULL; }
    file = (unsigned char *)malloc((size_t)n); if (!file) { fclose(f); return NULL; }
    if (fread(file, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(file); return NULL; }
    fclose(f);
    px = stbi_load_from_memory(file, (int)n, &iw, &ih, &ch, 4);
    free(file);
    if (!px) { static int warned; if (warned++ < 8) fprintf(stderr, "[TEXPACK] cannot decode %s (%s)\n", path, stbi_failure_reason()); return NULL; }
    if (iw < 1 || ih < 1 || iw > 8192 || ih > 8192) { stbi_image_free(px); return NULL; }
    cnt = (size_t)iw * (size_t)ih; out = (uint8_t *)malloc(cnt * 4);
    if (!out) { stbi_image_free(px); return NULL; }
    for (i = 0; i < cnt; ++i) { out[i * 4 + 0] = px[i * 4 + 2]; out[i * 4 + 1] = px[i * 4 + 1]; out[i * 4 + 2] = px[i * 4 + 0]; out[i * 4 + 3] = px[i * 4 + 3]; }   /* RGBA -> BGRA */
    stbi_image_free(px);
    *w = (uint32_t)iw; *h = (uint32_t)ih;
    return out;
}
void texpack_free(void *p) { free(p); }

void texpack_downsample(const uint32_t *src, uint32_t w, uint32_t h, uint32_t *dst)
{
    uint32_t dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1, x, y;
    for (y = 0; y < dh; ++y) for (x = 0; x < dw; ++x) {
        uint32_t x0 = x * 2, y0 = y * 2, x1 = (x0 + 1 < w) ? x0 + 1 : x0, y1 = (y0 + 1 < h) ? y0 + 1 : y0;
        uint32_t p[4] = { src[y0 * w + x0], src[y0 * w + x1], src[y1 * w + x0], src[y1 * w + x1] }, c, out = 0;
        for (c = 0; c < 4; ++c) { uint32_t s = ((p[0] >> (8 * c)) & 255) + ((p[1] >> (8 * c)) & 255) + ((p[2] >> (8 * c)) & 255) + ((p[3] >> (8 * c)) & 255); out |= ((s + 2) >> 2) << (8 * c); }
        dst[y * dw + x] = out;
    }
}

/* ------------------------------------------------------------------ PNG writer (fixed-Huffman deflate) */
typedef struct { uint8_t *b; size_t n, cap; } Buf;
static void bput(Buf *o, const void *d, size_t n) { if (o->n + n > o->cap) { size_t c = o->cap ? o->cap * 2 : 65536; while (c < o->n + n) c *= 2; o->b = (uint8_t *)realloc(o->b, c); o->cap = c; } memcpy(o->b + o->n, d, n); o->n += n; }
static void bput8(Buf *o, uint8_t v) { bput(o, &v, 1); }
static void bput32be(Buf *o, uint32_t v) { uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; bput(o, t, 4); }
static uint32_t g_crc[256]; static int g_crc_ok;
static uint32_t crc32_upd(uint32_t c, const uint8_t *p, size_t n)
{
    size_t i;
    if (!g_crc_ok) { uint32_t k, j; for (k = 0; k < 256; ++k) { uint32_t v = k; for (j = 0; j < 8; ++j) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1; g_crc[k] = v; } g_crc_ok = 1; }
    c = ~c; for (i = 0; i < n; ++i) c = g_crc[(c ^ p[i]) & 255] ^ (c >> 8); return ~c;
}
static void chunk(Buf *o, const char *type, const uint8_t *d, size_t n)
{
    uint8_t t[4]; uint32_t c;
    bput32be(o, (uint32_t)n); memcpy(t, type, 4); bput(o, t, 4); if (n) bput(o, d, n);
    c = crc32_upd(0, t, 4); if (n) c = crc32_upd(c, d, n); bput32be(o, c);
}
typedef struct { Buf *o; uint32_t acc; int nb; } BitW;
static void bits(BitW *w, uint32_t v, int n) { w->acc |= v << w->nb; w->nb += n; while (w->nb >= 8) { bput8(w->o, (uint8_t)w->acc); w->acc >>= 8; w->nb -= 8; } }
static void huff(BitW *w, uint32_t code, int n) { uint32_t r = 0; int i; for (i = 0; i < n; ++i) r |= ((code >> i) & 1u) << (n - 1 - i); bits(w, r, n); }   /* Huffman codes go out MSB first */
static void lit(BitW *w, int sym)
{
    if (sym < 144) huff(w, 0x30u + (uint32_t)sym, 8);
    else if (sym < 256) huff(w, 0x190u + (uint32_t)(sym - 144), 9);
    else if (sym < 280) huff(w, (uint32_t)(sym - 256), 7);
    else huff(w, 0xC0u + (uint32_t)(sym - 280), 8);
}
static const uint16_t LB[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t LE[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t DB[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t DE[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
static void deflate_fixed(Buf *o, const uint8_t *d, size_t n)
{
    BitW w = { o, 0, 0 }; int32_t *head = (int32_t *)malloc(sizeof(int32_t) * 32768), *prev = (int32_t *)malloc(sizeof(int32_t) * 32768); size_t i = 0, k;
    uint32_t a = 1, b = 0;
    for (k = 0; k < 32768; ++k) head[k] = -1;
    bput8(o, 0x78); bput8(o, 0x01);
    bits(&w, 1, 1); bits(&w, 1, 2);   /* BFINAL, BTYPE=01 */
    while (i < n) {
        int best = 0, bd = 0;
        if (i + 3 <= n) {
            uint32_t hv = ((uint32_t)d[i] * 506832829u ^ (uint32_t)d[i + 1] * 2654435761u ^ (uint32_t)d[i + 2] * 40503u) & 32767u; int32_t c = head[hv]; int chain = 0;
            while (c >= 0 && i - (size_t)c <= 32768 && chain++ < 16) {
                int l = 0; size_t mx = n - i < 258 ? n - i : 258;
                while ((size_t)l < mx && d[(size_t)c + (size_t)l] == d[i + (size_t)l]) l++;
                if (l > best) { best = l; bd = (int)(i - (size_t)c); if (l == 258) break; }
                c = prev[(size_t)c & 32767];
            }
            prev[i & 32767] = head[hv]; head[hv] = (int32_t)i;
        }
        if (best >= 3) {
            int li = 28, di = 29;
            while (LB[li] > best) li--;
            while (DB[di] > bd) di--;
            lit(&w, 257 + li); if (LE[li]) bits(&w, (uint32_t)(best - LB[li]), LE[li]);
            huff(&w, (uint32_t)di, 5); if (DE[di]) bits(&w, (uint32_t)(bd - DB[di]), DE[di]);
            for (k = 1; k < (size_t)best; ++k) {   /* index the skipped positions too */
                size_t p = i + k;
                if (p + 3 <= n) { uint32_t hv = ((uint32_t)d[p] * 506832829u ^ (uint32_t)d[p + 1] * 2654435761u ^ (uint32_t)d[p + 2] * 40503u) & 32767u; prev[p & 32767] = head[hv]; head[hv] = (int32_t)p; }
            }
            i += (size_t)best;
        } else { lit(&w, d[i]); i++; }
    }
    lit(&w, 256);
    if (w.nb) bput8(o, (uint8_t)w.acc);
    for (k = 0; k < n; ++k) { a = (a + d[k]) % 65521u; b = (b + a) % 65521u; }
    bput32be(o, (b << 16) | a);
    free(head); free(prev);
}
static int write_png(const char *path, uint32_t w, uint32_t h, const uint8_t *bgra, uint32_t pitch)
{
    Buf raw = { 0 }, z = { 0 }, o = { 0 }; uint8_t hdr[13]; uint32_t y, x; FILE *f; static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    raw.b = (uint8_t *)malloc((size_t)(w * 4 + 1) * h); if (!raw.b) return 0; raw.cap = (size_t)(w * 4 + 1) * h;
    for (y = 0; y < h; ++y) {
        uint8_t *r = raw.b + (size_t)y * (w * 4 + 1); const uint8_t *s = bgra + (size_t)y * pitch;
        r[0] = 0; for (x = 0; x < w; ++x) { r[1 + x * 4] = s[x * 4 + 2]; r[2 + x * 4] = s[x * 4 + 1]; r[3 + x * 4] = s[x * 4 + 0]; r[4 + x * 4] = s[x * 4 + 3]; }
    }
    raw.n = raw.cap;
    deflate_fixed(&z, raw.b, raw.n);
    bput(&o, sig, 8);
    hdr[0] = (uint8_t)(w >> 24); hdr[1] = (uint8_t)(w >> 16); hdr[2] = (uint8_t)(w >> 8); hdr[3] = (uint8_t)w; hdr[4] = (uint8_t)(h >> 24); hdr[5] = (uint8_t)(h >> 16); hdr[6] = (uint8_t)(h >> 8); hdr[7] = (uint8_t)h;
    hdr[8] = 8; hdr[9] = 6; hdr[10] = 0; hdr[11] = 0; hdr[12] = 0;
    chunk(&o, "IHDR", hdr, 13); chunk(&o, "IDAT", z.b, z.n); chunk(&o, "IEND", NULL, 0);
    f = fopen(path, "wb"); if (f) { fwrite(o.b, 1, o.n, f); fclose(f); }
    free(raw.b); free(z.b); free(o.b);
    return f != NULL;
}

/* ------------------------------------------------------------------ dump */
static uint64_t g_seen[1 << 16]; static size_t g_seen_n;
static int seen_add(uint64_t key)
{
    size_t i = (size_t)(key * 0x9E3779B97F4A7C15ull >> 40) & ((1u << 16) - 1);
    if (g_seen_n > (1u << 15)) return 0;   /* table full: stop dumping new ones rather than loop */
    while (g_seen[i]) { if (g_seen[i] == key) return 0; i = (i + 1) & ((1u << 16) - 1); }
    g_seen[i] = key ? key : 1; g_seen_n++; return 1;
}
void texpack_dump(uint64_t key, uint32_t w, uint32_t h, uint32_t format, uint32_t frame, uint32_t guest_offset, const uint8_t *bgra, uint32_t pitch)
{
    char path[MAX_PATH], tmp[MAX_PATH];
    if (!g_dump_on || !w || !h || !bgra || !seen_add(key)) return;
    snprintf(path, sizeof path, "%s\\%016llx.png", g_dump_dir, (unsigned long long)key);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (write_png(tmp, w, h, bgra, pitch)) {
        FILE *ix; char ixp[MAX_PATH];
        MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
        snprintf(ixp, sizeof ixp, "%s\\index.tsv", g_dump_dir);
        ix = fopen(ixp, "ab");
        if (ix) { fprintf(ix, "%016llx\t%u\t%u\t%08X\t%u\t%08X\n", (unsigned long long)key, w, h, format, frame, guest_offset); fclose(ix); }
    }
}

/* ------------------------------------------------------------------ stats */
static unsigned g_replaced, g_last_logged;
void texpack_note_replaced(void) { g_replaced++; }
void texpack_log_stats(uint32_t frame)
{
    if (!g_packs_active) return;
    if (g_replaced != g_last_logged && (frame % 600) == 0) { fprintf(stderr, "[TEXPACK] frame=%u replacement textures uploaded so far: %u\n", frame, g_replaced); g_last_logged = g_replaced; }
}
