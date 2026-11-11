/* SPDX-License-Identifier: GPL-2.0-or-later */
/* d3d8_perf.c -- opt-in (STE_PERF=1) counting and timing shim for the render device's immediate context (c308 probe).
 *
 * d3d8_perf_install() gives the immediate context a private copy of its method table in which the draw, state, resource-update and map calls are
 * replaced by thunks that count the call, time it (QueryPerformanceCounter around the real call) and, for the state setters, compare the arguments with
 * the previous call of the same method ("redundant" = would have been a no-op).  The thunks forward eight pointer-sized values, which is exact for every
 * wrapped method on the Windows x64 ABI (all integer/pointer class, at most six real arguments); methods taking float arguments are not wrapped.
 * d3d8_perf_report() prints one [PERFD] block per call and clears the counters.  Nothing here runs unless STE_PERF is set. */
#include "d3d8_internal.h"
#include <d3d11_4.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERF_METHODS(X) \
    X(Draw) X(DrawIndexed) X(DrawInstanced) X(DrawIndexedInstanced) X(Map) X(Unmap) X(UpdateSubresource) \
    X(VSSetShader) X(PSSetShader) X(VSSetConstantBuffers) X(PSSetConstantBuffers) X(VSSetShaderResources) X(PSSetShaderResources) \
    X(VSSetSamplers) X(PSSetSamplers) X(IASetVertexBuffers) X(IASetIndexBuffer) X(IASetInputLayout) X(IASetPrimitiveTopology) \
    X(OMSetBlendState) X(OMSetDepthStencilState) X(OMSetRenderTargets) X(RSSetState) X(RSSetViewports) X(RSSetScissorRects) \
    X(ClearRenderTargetView) X(CopyResource) X(CopySubresourceRegion) X(ResolveSubresource) X(GenerateMips) X(Begin) X(End) X(Flush)

enum {
#define X(n) PM_##n,
    PERF_METHODS(X)
#undef X
    PM_COUNT
};
static const char *const g_pm_name[PM_COUNT] = {
#define X(n) #n,
    PERF_METHODS(X)
#undef X
};

typedef UINT64 (__stdcall *PerfFn8)(UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64);

/* the object also serves ID3D11DeviceContext1..4 (d3d8_kvp.c calls VSSetConstantBuffers1 on the same pointer), so the copy must cover the longest table */
static ID3D11DeviceContext4Vtbl g_pm_vt;
static const ID3D11DeviceContext4Vtbl *g_pm_orig;
static unsigned long long g_pm_cnt[PM_COUNT], g_pm_tk[PM_COUNT], g_pm_red[PM_COUNT], g_pm_work[PM_COUNT];
static unsigned char g_pm_last[PM_COUNT][208];
static size_t g_pm_last_n[PM_COUNT];
static int g_pm_on;

/* true when the arguments of this state-setter call equal those of its previous call (nothing would change) */
static int pm_redundant(int id, UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4, UINT64 a5)
{
    unsigned char b[208]; size_t n = 0; UINT64 i;
#define PUT(v) do { UINT64 v_ = (UINT64)(v); memcpy(b + n, &v_, 8); n += 8; } while (0)
    switch (id) {
    case PM_VSSetShader: case PM_PSSetShader: case PM_IASetInputLayout: case PM_IASetPrimitiveTopology: case PM_RSSetState:
        PUT(a1); break;
    case PM_OMSetDepthStencilState: PUT(a1); PUT(a2); break;
    case PM_OMSetBlendState:
        PUT(a1); PUT(a3);
        if (a2) { memcpy(b + n, (const void *)(uintptr_t)a2, 16); n += 16; } else { PUT(0); PUT(0); }
        break;
    case PM_VSSetShaderResources: case PM_PSSetShaderResources: case PM_VSSetSamplers: case PM_PSSetSamplers:
    case PM_VSSetConstantBuffers: case PM_PSSetConstantBuffers:
        if (a2 > 16 || !a3) return 0;
        PUT(a1); PUT(a2); for (i = 0; i < a2; i++) PUT(((const UINT64 *)(uintptr_t)a3)[i]);
        break;
    case PM_IASetVertexBuffers:
        if (a2 > 4 || !a3 || !a4 || !a5) return 0;
        PUT(a1); PUT(a2);
        for (i = 0; i < a2; i++) { PUT(((const UINT64 *)(uintptr_t)a3)[i]); PUT(((const UINT32 *)(uintptr_t)a4)[i]); PUT(((const UINT32 *)(uintptr_t)a5)[i]); }
        break;
    case PM_IASetIndexBuffer: PUT(a1); PUT(a2); PUT(a3); break;
    case PM_OMSetRenderTargets:
        if (a1 > 8) return 0;
        PUT(a1); PUT(a3); for (i = 0; i < a1 && a2; i++) PUT(((const UINT64 *)(uintptr_t)a2)[i]);
        break;
    case PM_RSSetViewports:
        if (a1 > 4 || !a2) return 0;
        PUT(a1); memcpy(b + n, (const void *)(uintptr_t)a2, (size_t)a1 * 24); n += (size_t)a1 * 24;
        break;
    case PM_RSSetScissorRects:
        if (a1 > 4 || !a2) return 0;
        PUT(a1); memcpy(b + n, (const void *)(uintptr_t)a2, (size_t)a1 * 16); n += (size_t)a1 * 16;
        break;
    default: return 0;
    }
#undef PUT
    {
        int same = g_pm_last_n[id] == n && memcmp(g_pm_last[id], b, n) == 0;
        memcpy(g_pm_last[id], b, n); g_pm_last_n[id] = n;
        return same;
    }
}

#define X(n) \
static UINT64 __stdcall pmw_##n(UINT64 a0, UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4, UINT64 a5, UINT64 a6, UINT64 a7) \
{ \
    LARGE_INTEGER t0, t1; UINT64 r; \
    g_pm_red[PM_##n] += (unsigned long long)pm_redundant(PM_##n, a1, a2, a3, a4, a5); \
    if (PM_##n == PM_Draw || PM_##n == PM_DrawIndexed) g_pm_work[PM_##n] += a1; \
    QueryPerformanceCounter(&t0); \
    r = ((PerfFn8)(void *)g_pm_orig->n)(a0, a1, a2, a3, a4, a5, a6, a7); \
    QueryPerformanceCounter(&t1); \
    g_pm_cnt[PM_##n]++; g_pm_tk[PM_##n] += (unsigned long long)(t1.QuadPart - t0.QuadPart); \
    return r; \
}
PERF_METHODS(X)
#undef X

void d3d8_perf_install(ID3D11DeviceContext *c)
{
    const char *e = getenv("STE_PERF");
    if (!c || !e || !e[0] || e[0] == '0' || g_pm_on) return;
    g_pm_orig = (const ID3D11DeviceContext4Vtbl *)(const void *)c->lpVtbl; g_pm_vt = *g_pm_orig;
#define X(n) *(void **)&g_pm_vt.n = (void *)pmw_##n;
    PERF_METHODS(X)
#undef X
    c->lpVtbl = (ID3D11DeviceContextVtbl *)(void *)&g_pm_vt; g_pm_on = 1;
    fprintf(stderr, "[PERFD] STE_PERF: counting shim on the render context (%d methods)\n", (int)PM_COUNT); fflush(stderr);
}

/* one block per call: frames = frames in the window, busy_ms = render-thread busy time summed over them, scale = query_scale in force */
void d3d8_perf_report(unsigned frames, double busy_ms, double scale, unsigned opt_off)
{
    LARGE_INTEGER f; double ms_per_tick, tot_ms = 0.0, calls = 0.0; int i, k, used[PM_COUNT];
    if (!g_pm_on || frames == 0) return;
    QueryPerformanceFrequency(&f); ms_per_tick = 1000.0 / (double)f.QuadPart;
    for (i = 0; i < PM_COUNT; i++) { tot_ms += (double)g_pm_tk[i] * ms_per_tick; calls += (double)g_pm_cnt[i]; used[i] = 0; }
    fprintf(stderr, "[PERFD] n=%u scale=%.2f opt_off=%x render busy=%.3f ms/frame, d3d11 calls=%.3f ms/frame (%.0f calls/frame, %.1f%% of busy)\n",
            frames, scale, opt_off, busy_ms / frames, tot_ms / frames, calls / frames, busy_ms > 0 ? 100.0 * tot_ms / busy_ms : 0.0);
    for (k = 0; k < 14; k++) {   /* the 14 most expensive methods */
        int best = -1;
        for (i = 0; i < PM_COUNT; i++) if (!used[i] && g_pm_cnt[i] && (best < 0 || g_pm_tk[i] > g_pm_tk[best])) best = i;
        if (best < 0) break;
        used[best] = 1;
        fprintf(stderr, "[PERFD]   %-24s calls/frame=%8.1f ms/frame=%7.3f us/call=%6.2f redundant=%5.1f%%%s",
                g_pm_name[best], (double)g_pm_cnt[best] / frames, (double)g_pm_tk[best] * ms_per_tick / frames,
                (double)g_pm_tk[best] * ms_per_tick * 1000.0 / (double)g_pm_cnt[best], 100.0 * (double)g_pm_red[best] / (double)g_pm_cnt[best], "");
        if (best == PM_Draw || best == PM_DrawIndexed) fprintf(stderr, " avg_count=%.0f", (double)g_pm_work[best] / (double)g_pm_cnt[best]);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
    memset(g_pm_cnt, 0, sizeof g_pm_cnt); memset(g_pm_tk, 0, sizeof g_pm_tk); memset(g_pm_red, 0, sizeof g_pm_red); memset(g_pm_work, 0, sizeof g_pm_work);
}
