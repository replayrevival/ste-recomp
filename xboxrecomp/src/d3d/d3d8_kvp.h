/* SPDX-License-Identifier: GPL-2.0-or-later
 * Kelvin vertex programs on the GPU (STE): the translator (nv2a_kelvin.c) turns an NV2A vertex program into HLSL and hands it
 * to this layer, which owns every D3D11 call (shader/layout cache, buffer rings, stream-out verification). The HLSL is plain
 * SM5-level code (no D3DCompile-only features) so it also compiles with DXC for a later Vulkan backend. */
#ifndef D3D8_KVP_H
#define D3D8_KVP_H
#include <stdint.h>
#include <windows.h>

typedef struct D3D8KvpProgram D3D8KvpProgram;

enum { KVP_TOPO_POINTS = 0, KVP_TOPO_LINES = 1, KVP_TOPO_LINESTRIP = 2, KVP_TOPO_TRIS = 3, KVP_TOPO_TRISTRIP = 4 };

/* floats per vertex written by the verification variant: window pos x,y,z/clip_max,w(4) diffuse(4) specular(4) tex0-3(16) fog(1) */
#define KVP_VERIFY_FLOATS 29

typedef struct D3D8KvpDraw {
    const void     *verts;          /* nverts * (attr_count * 16) bytes, float4 per attribute in ascending attribute order */
    uint32_t        nverts;
    const uint32_t *indices;        /* NULL = plain Draw over nverts */
    uint32_t        nidx;
    int             topo;           /* KVP_TOPO_* */
    const float    *constants;      /* 192 x float4 */
    float           clip_max;
    float           fog_en, fog_p[3];
    uint32_t        fog_mode;
    float           tscale[8];      /* per-stage s,t scale (linear textures) */
    uint32_t        dotraw;         /* bit per stage: pass raw s,t,r,q (DOT_* stage modes) */
} D3D8KvpDraw;

/* Compile hlsl (entry "main", vs_5_0). input_mask = attribute registers the program reads (v0..v15). NULL on failure. */
D3D8KvpProgram *d3d8_kvp_create(const char *hlsl, const char *name, uint32_t input_mask);
/* Pixel state is applied here (nv2a combiner pixel shader + d3d8 render states). Returns 0 if the draw could not be made
 * (caller falls back to the CPU transform). */
int d3d8_kvp_draw(D3D8KvpProgram *p, const D3D8KvpDraw *d);   /* 1 drawn, 2 skipped (pixel shader compiling), 0 use the CPU path */
/* 0 = still compiling on the shader worker (draw on the CPU path meanwhile), 1 = ready, -1 = failed (stay on the CPU path) */
int d3d8_kvp_status(D3D8KvpProgram *p);
/* Runs the program on the GPU for d->verts through stream-out (no rasterisation) and returns KVP_VERIFY_FLOATS floats per vertex
 * in out (nverts * KVP_VERIFY_FLOATS floats). Needs the program HLSL compiled with KVP_VERIFY defined (done on demand). */
int d3d8_kvp_verify(D3D8KvpProgram *p, const D3D8KvpDraw *d, float *out);
#endif

