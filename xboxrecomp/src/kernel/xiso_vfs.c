/* SPDX-License-Identifier: GPL-2.0-or-later */
/* xiso_vfs.c (0.1.7): read-only XISO (Xbox disc image) reader for the file shim.
 *
 * The game folder can be an .iso / .xiso: the image is opened read-only and never written or unpacked. File and directory entries are found in the image's directory tree on
 * demand; the kernel file layer (kernel_file.c) hands out virtual handles for them (a dummy event object keeps the handle value unique) and answers read / query / enumerate calls here.
 *
 * Format (XDVDFS): a volume descriptor 2048 bytes at sector 32 of the data partition holds "MICROSOFT*XBOX*MEDIA", the root directory's start sector and size. A directory is a binary
 * search tree of entries stored in its sectors: { u16 left (dwords), u16 right (dwords), u32 sector, u32 size, u8 attributes, u8 name length, name }, padded with 0xFF to 4 bytes;
 * offset 0 means "none" except for the root of the tree. Redump-style images carry the partition after a video partition: tried at 0, 0x18300000, 0xFD90000, 0x2090000.
 * Names compare case-insensitively (ASCII). Portability: plain C99 + a small Win32 wrapper for the file (fopen on non-Windows); no 4 KB page-size assumptions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#include "xiso_vfs.h"

#define XSEC 2048u
static FILE *g_f;
static uint64_t g_base;
static uint32_t g_root_sector, g_root_size;
static wchar_t g_root[260];       /* virtual host-path prefix that stands for the image root, no trailing backslash */
static size_t g_root_len;
static uint64_t g_img_size;
#if defined(_WIN32)
static CRITICAL_SECTION g_cs; static int g_cs_init;
#define LOCK() do { if (!g_cs_init) { InitializeCriticalSection(&g_cs); g_cs_init = 1; } EnterCriticalSection(&g_cs); } while (0)
#define UNLOCK() LeaveCriticalSection(&g_cs)
#else
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif

static int rd_at(uint64_t off, void *buf, size_t n)
{
    if (!g_f) return 0;
#if defined(_WIN32)
    if (_fseeki64(g_f, (long long)off, SEEK_SET)) return 0;
#else
    if (fseeko(g_f, (off_t)off, SEEK_SET)) return 0;
#endif
    return fread(buf, 1, n, g_f) == n;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static unsigned le16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

int xiso_mount(const wchar_t *iso_path, const wchar_t *virtual_root)
{
    static const uint64_t bases[] = { 0u, 0x18300000ull, 0x0FD90000ull, 0x02090000ull };
    uint8_t vd[XSEC]; unsigned i;
    if (g_f) return 0;
#if defined(_WIN32)
    g_f = _wfopen(iso_path, L"rb");
#else
    { char p[1024]; size_t k = 0; while (iso_path[k] && k < 1023) { p[k] = (char)iso_path[k]; k++; } p[k] = 0; g_f = fopen(p, "rb"); }
#endif
    if (!g_f) return 0;
#if defined(_WIN32)
    _fseeki64(g_f, 0, SEEK_END); g_img_size = (uint64_t)_ftelli64(g_f);
#else
    fseeko(g_f, 0, SEEK_END); g_img_size = (uint64_t)ftello(g_f);
#endif
    for (i = 0; i < sizeof bases / sizeof bases[0]; ++i) {
        if (rd_at(bases[i] + 32ull * XSEC, vd, XSEC) && !memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20) && !memcmp(vd + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20)) {
            g_base = bases[i]; g_root_sector = le32(vd + 20); g_root_size = le32(vd + 24);
            wcsncpy(g_root, virtual_root, 259); g_root[259] = 0; g_root_len = wcslen(g_root);
            while (g_root_len > 3 && (g_root[g_root_len - 1] == L'\\' || g_root[g_root_len - 1] == L'/')) g_root[--g_root_len] = 0;
            return 1;
        }
    }
    fclose(g_f); g_f = NULL;
    return 0;
}

int xiso_active(void) { return g_f != NULL; }
const wchar_t *xiso_root(void) { return g_root; }

/* the directory tree of one directory: read it whole (directories are small; refuse absurd sizes) */
static uint8_t *dir_load(uint32_t sector, uint32_t size)
{
    uint8_t *b; if (size == 0 || size > (16u << 20)) return NULL;
    b = (uint8_t *)malloc(size); if (!b) return NULL;
    if (!rd_at(g_base + (uint64_t)sector * XSEC, b, size)) { free(b); return NULL; }
    return b;
}

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int name_cmp(const char *a, size_t an, const char *b, size_t bn)
{
    size_t i, n = an < bn ? an : bn;
    for (i = 0; i < n; ++i) { int d = lc((unsigned char)a[i]) - lc((unsigned char)b[i]); if (d) return d; }
    return an < bn ? -1 : an > bn ? 1 : 0;
}

static int entry_at(const uint8_t *d, uint32_t size, uint32_t off, XisoEntry *e, unsigned *l, unsigned *r, char *name /*>=256*/)
{
    unsigned nl;
    if (off + 14u > size) return 0;
    *l = le16(d + off); *r = le16(d + off + 2);
    e->sector = le32(d + off + 4); e->size = le32(d + off + 8); e->is_dir = (d[off + 12] & 0x10u) != 0; nl = d[off + 13];
    if (off + 14u + nl > size) return 0;
    memcpy(name, d + off + 14, nl); name[nl] = 0; e->name_len = nl;
    return 1;
}

/* find one name in a directory (binary tree) */
static int dir_find(uint32_t sector, uint32_t size, const char *name, size_t nl, XisoEntry *out)
{
    uint8_t *d = dir_load(sector, size); uint32_t off = 0; int guard = 0, found = 0;
    if (!d) return 0;
    while (guard++ < 100000) {
        XisoEntry e; unsigned l, r; char nm[260]; int c;
        if (!entry_at(d, size, off, &e, &l, &r, nm)) break;
        c = name_cmp(name, nl, nm, e.name_len);
        if (c == 0) { *out = e; found = 1; break; }
        off = (uint32_t)(c < 0 ? l : r) * 4u;
        if (off == 0) break;
    }
    free(d);
    return found;
}

int xiso_lookup(const wchar_t *path, XisoEntry *out)
{
    XisoEntry cur; const wchar_t *p; int rc = 0;
    if (!g_f || !path) return 0;
    if (wcsncmp(path, g_root, g_root_len)) { size_t k; for (k = 0; k < g_root_len; ++k) { wchar_t a = path[k], b = g_root[k]; if (a >= L'a' && a <= L'z') a -= 32; if (b >= L'a' && b <= L'z') b -= 32; if (a != b && !((a == L'/' || a == L'\\') && (b == L'/' || b == L'\\'))) return 0; } }
    p = path + g_root_len;
    if (*p && *p != L'\\' && *p != L'/') return 0;
    LOCK();
    memset(&cur, 0, sizeof cur); cur.sector = g_root_sector; cur.size = g_root_size; cur.is_dir = 1;
    rc = 1;
    while (rc) {
        char name[260]; size_t n = 0;
        while (*p == L'\\' || *p == L'/') ++p;
        if (!*p) break;
        while (p[n] && p[n] != L'\\' && p[n] != L'/' && n < 255) { name[n] = (char)(p[n] < 0x80 ? p[n] : '?'); ++n; }
        p += n;
        if (!cur.is_dir || !dir_find(cur.sector, cur.size, name, n, &cur)) rc = 0;
    }
    UNLOCK();
    if (rc) *out = cur;
    return rc;
}

int xiso_read(const XisoEntry *e, uint64_t off, void *buf, uint32_t len, uint32_t *got)
{
    uint32_t n; int ok;
    *got = 0;
    if (!g_f || e->is_dir || off >= e->size) return 1;
    n = len; if ((uint64_t)n > (uint64_t)e->size - off) n = (uint32_t)((uint64_t)e->size - off);
    LOCK();
    ok = rd_at(g_base + (uint64_t)e->sector * XSEC + off, buf, n);
    UNLOCK();
    if (ok) *got = n;
    return ok;
}

/* nth child of a directory in name order: walk the tree in order, count */
typedef struct { uint8_t *d; uint32_t size; int want, seen, done; XisoEntry out; char name[260]; } Walk;
static void walk(Walk *w, uint32_t off, int depth)
{
    XisoEntry e; unsigned l, r; char nm[260];
    if (w->done || depth > 64) return;
    if (!entry_at(w->d, w->size, off, &e, &l, &r, nm)) return;
    if (l) walk(w, (uint32_t)l * 4u, depth + 1);
    if (w->done) return;
    if (w->seen == w->want) { w->out = e; memcpy(w->name, nm, e.name_len + 1u); w->done = 1; return; }
    w->seen++;
    if (r) walk(w, (uint32_t)r * 4u, depth + 1);
}
int xiso_dir_entry(const XisoEntry *dir, int index, XisoEntry *out, char *name /* >= 260 */)
{
    Walk w; int ok = 0;
    if (!g_f || !dir->is_dir) return 0;
    LOCK();
    memset(&w, 0, sizeof w); w.d = dir_load(dir->sector, dir->size);
    if (w.d) { w.size = dir->size; w.want = index; walk(&w, 0, 0); if (w.done) { *out = w.out; memcpy(name, w.name, w.out.name_len + 1u); ok = 1; } free(w.d); }
    UNLOCK();
    return ok;
}

int xiso_name_match(const char *pattern, const char *name)
{
    while (*pattern) {
        if (*pattern == '*') { while (*pattern == '*') ++pattern; if (!*pattern) return 1; for (; *name; ++name) if (xiso_name_match(pattern, name)) return 1; return 0; }
        if (!*name) return 0;
        if (*pattern != '?' && lc((unsigned char)*pattern) != lc((unsigned char)*name)) return 0;
        ++pattern; ++name;
    }
    return *name == 0;
}

uint64_t xiso_image_size(void) { return g_img_size; }
