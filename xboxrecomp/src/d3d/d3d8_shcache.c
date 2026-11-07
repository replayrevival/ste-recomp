/* SPDX-License-Identifier: GPL-2.0-or-later
 * Persistent shader cache + asynchronous compile queue (see d3d8_shcache.h). */
#include "d3d8_internal.h"
#include "d3d8_shcache.h"
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHC_VERSION 1u   /* bump when the generators change in a way that keeps the HLSL text identical */

volatile long g_shc_disk_hits, g_shc_compiles, g_shc_pending;

uint64_t shc_key(const char *hlsl, size_t len, const char *profile, unsigned flags)
{
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < len; ++i) { h ^= (unsigned char)hlsl[i]; h *= 1099511628211ull; }
    for (i = 0; profile[i]; ++i) { h ^= (unsigned char)profile[i]; h *= 1099511628211ull; }
    h ^= flags; h *= 1099511628211ull;
    h ^= SHC_VERSION; h *= 1099511628211ull;
    return h;
}

static char g_dir[MAX_PATH];
static int g_dir_state;   /* 0 = unknown, 1 = ready, -1 = disabled */
static int shc_dir_ready(void)
{
    if (g_dir_state) return g_dir_state > 0;
    if (getenv("KELVIN_SHADER_NOCACHE")) { g_dir_state = -1; return 0; }
    {
        char *p;
        GetModuleFileNameA(NULL, g_dir, MAX_PATH);
        p = strrchr(g_dir, 0x5C);
        if (p) strcpy(p + 1, "shadercache"); else strcpy(g_dir, "shadercache");
        CreateDirectoryA(g_dir, NULL);
        g_dir_state = GetFileAttributesA(g_dir) != INVALID_FILE_ATTRIBUTES ? 1 : -1;
    }
    return g_dir_state > 0;
}

static void shc_path(char *out, size_t n, const char *kind, uint64_t key)
{
    _snprintf(out, n, "%s\\%s_%016llX.cso", g_dir, kind, (unsigned long long)key);
    out[n - 1] = 0;
}

void *shc_load(const char *kind, uint64_t key, size_t *size)
{
    char path[MAX_PATH]; FILE *f; uint32_t hdr[4]; void *buf;
    if (!shc_dir_ready()) return NULL;
    shc_path(path, sizeof(path), kind, key);
    f = fopen(path, "rb");
    if (!f) return NULL;
    if (fread(hdr, 4, 4, f) != 4 || hdr[0] != 0x31434853u /* 'SHC1' */ || hdr[1] != (uint32_t)key || hdr[2] != (uint32_t)(key >> 32) || hdr[3] == 0 || hdr[3] > (16u << 20)) { fclose(f); return NULL; }
    buf = malloc(hdr[3]);
    if (!buf || fread(buf, 1, hdr[3], f) != hdr[3]) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = hdr[3];
    InterlockedIncrement(&g_shc_disk_hits);
    return buf;
}

void shc_free(void *p) { free(p); }

void shc_store(const char *kind, uint64_t key, const void *code, size_t size)
{
    char path[MAX_PATH], tmp[MAX_PATH + 8]; FILE *f; uint32_t hdr[4];
    if (!shc_dir_ready()) return;
    shc_path(path, sizeof(path), kind, key);
    _snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) return;
    hdr[0] = 0x31434853u; hdr[1] = (uint32_t)key; hdr[2] = (uint32_t)(key >> 32); hdr[3] = (uint32_t)size;
    if (fwrite(hdr, 4, 4, f) == 4 && fwrite(code, 1, size, f) == size) { fclose(f); MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING); }
    else { fclose(f); DeleteFileA(tmp); }
}

typedef struct ShcJob {
    char kind[8], name[64], profile[8];
    char *hlsl; size_t len; unsigned flags; uint64_t key;
    ShcDone done; void *ctx;
    struct ShcJob *next;
} ShcJob;

static CRITICAL_SECTION g_q_cs; static HANDLE g_q_evt; static ShcJob *g_q_head, *g_q_tail; static int g_q_init;
static INIT_ONCE g_q_once = INIT_ONCE_STATIC_INIT;

static void run_job(ShcJob *j)
{
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = D3DCompile(j->hlsl, j->len, j->name, NULL, NULL, "main", j->profile, j->flags, 0, &blob, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[SHC] compile of %s failed (hr=%08lX): %.300s\n", j->name, (unsigned long)hr, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        j->done(j->ctx, NULL, 0);
    } else {
        if (err) ID3D10Blob_Release(err);
        InterlockedIncrement(&g_shc_compiles);
        shc_store(j->kind, j->key, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob));
        j->done(j->ctx, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob));
        ID3D10Blob_Release(blob);
    }
    free(j->hlsl);
    free(j);
}

static DWORD WINAPI worker(LPVOID p)
{
    (void)p;
    for (;;) {
        ShcJob *j;
        WaitForSingleObject(g_q_evt, INFINITE);
        for (;;) {
            EnterCriticalSection(&g_q_cs);
            j = g_q_head;
            if (j) { g_q_head = j->next; if (!g_q_head) g_q_tail = NULL; }
            LeaveCriticalSection(&g_q_cs);
            if (!j) break;
            run_job(j);
            InterlockedDecrement(&g_shc_pending);
        }
    }
}

static BOOL CALLBACK q_init(PINIT_ONCE o, PVOID a, PVOID *c)
{
    int i, n = 2;
    (void)o; (void)a; (void)c;
    InitializeCriticalSection(&g_q_cs);
    g_q_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (getenv("KELVIN_SHADER_THREADS")) n = atoi(getenv("KELVIN_SHADER_THREADS"));
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    for (i = 0; i < n; ++i) {
        HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); }
    }
    g_q_init = 1;
    return TRUE;
}

void shc_compile_async(const char *kind, const char *hlsl, size_t len, const char *name, const char *profile, unsigned flags, uint64_t key, ShcDone done, void *ctx)
{
    ShcJob *j = (ShcJob *)calloc(1, sizeof(*j));
    if (!j) { done(ctx, NULL, 0); return; }
    strncpy(j->kind, kind, sizeof(j->kind) - 1); strncpy(j->name, name, sizeof(j->name) - 1); strncpy(j->profile, profile, sizeof(j->profile) - 1);
    j->hlsl = (char *)malloc(len + 1);
    if (!j->hlsl) { free(j); done(ctx, NULL, 0); return; }
    memcpy(j->hlsl, hlsl, len); j->hlsl[len] = 0; j->len = len; j->flags = flags; j->key = key; j->done = done; j->ctx = ctx;
    if (getenv("KELVIN_SHADER_SYNC")) { run_job(j); return; }
    InitOnceExecuteOnce(&g_q_once, q_init, NULL, NULL);
    InterlockedIncrement(&g_shc_pending);
    EnterCriticalSection(&g_q_cs);
    if (g_q_tail) g_q_tail->next = j; else g_q_head = j;
    g_q_tail = j;
    LeaveCriticalSection(&g_q_cs);
    SetEvent(g_q_evt);
}
