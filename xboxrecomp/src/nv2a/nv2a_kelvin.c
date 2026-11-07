/*
 * NV2A Kelvin (NV097) pushbuffer -> D3D8 layer translator.
 *
 * Method numbers, vertex-program microcode fields and texture formats follow
 * xemu (hw/xbox/nv2a/nv2a_regs.h, pgraph/vsh_regs.h, pgraph/glsl/vsh-prog.c,
 * pgraph/glsl/vsh-ff.c). Vertex processing happens on the CPU and the result
 * is submitted as pre-transformed XYZRHW geometry, which is what the NV2A
 * rasteriser consumes anyway: a vertex program's oPos is already in screen
 * space (the XDK appends the viewport transform), and the fixed-function
 * path is the composite matrix followed by the perspective divide.
 *
 * Deliberately approximate: register combiners are reduced to
 * texture-modulates-diffuse, lighting is not evaluated, and culling is left
 * off. Everything else aims to be exact.
 */

#include "nv2a_kelvin.h"
#include "kelvin_texpack.h"
#include "../d3d/d3d8_xbox.h"
#include "../d3d/d3d8_nv2a_psh.h"
#include "../d3d/d3d8_kvp.h"
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* c214: debug free-cam tag for STE_POPLOG (written by the title-side hook in recomp_manual.c, see the c214 tag record below) */
float g_ste_poplog_tag[8]; volatile uint32_t g_ste_poplog_tag_ver;

extern ptrdiff_t g_xbox_mem_offset;
extern IDirect3DDevice8 *xbox_GetD3DDevice(void);

/* Guest physical address -> host pointer. Contiguous allocations in this
 * runtime are identity-mapped; the 0x80000000 mirror and DMA-context high
 * bits are masked off. */
uintptr_t g_kelvin_mem_base; /* nonzero: offline replay of a RAM image */
/* getenv is a locked scan of the environment block and sat on the per-draw
 * (and per-vertex) path; debug toggles are read once and cached here. */
static const char *kenv(const char *name)
{
    static struct { const char *name; const char *val; } cache[32];
    static int used;
    for (int i = 0; i < used; ++i)
        if (cache[i].name == name) return cache[i].val;   /* c80: pointer match first (literals are merged); strcmp scans were 2% of the guest thread */
    for (int i = 0; i < used; ++i)
        if (!strcmp(cache[i].name, name)) return cache[i].val;
    {
        const char *v = getenv(name);
        if (used < 32) { cache[used].name = name; cache[used].val = v; used++; }
        return v;
    }
}

extern volatile int g_xbox_uaf_on;   /* c89: RECOMP_ASYNC_UAF=1 guard, see xbox_memory_layout.c */
extern void xbox_AsyncUafCheck(uint32_t phys, uint32_t len, const char *what);
static const uint8_t *guest_ptr(uint32_t phys)
{
    if (g_xbox_uaf_on) xbox_AsyncUafCheck(phys, 4, "attr/tex");
    return (const uint8_t *)((uintptr_t)(phys & 0x07FFFFFFu) +
                             (g_kelvin_mem_base ? g_kelvin_mem_base : (uintptr_t)g_xbox_mem_offset));
}

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
extern void d3d8_set_kelvin_vertex_fog(int on);
#include "../d3d/d3d8_video.h"
/* Wide mode: 3D draws stretch across the whole (wider) surface, 2D/HUD draws stay a centred 640x480 block.  A draw is 2D when
 * the title loaded the canonical screen mapping (c96..c99 = 1/320, -1/240) -- the D3D pre-transform state. */
static void kelvin_apply_xmap(int stretch, int untextured_fullscreen);
static int g_no_fog;          /* KELVIN_NO_FOG=1 */
static int g_fog_user_scale;  /* c267: fog_scale came from the env / ini: the draw_distance=unlimited hint (kelvin_fog_scale_default) does not override it */
static float g_fog_scale = 1.0f;/* ste_video.ini fog_scale / STE_VIDEO_FOG_SCALE: fog distances x scale (divides the fog slope g_fog_p[1]) */
static void kelvin_tex_forget(IDirect3DTexture8 *t);   /* c79: drop a released texture from the SetTexture dedupe cache */
static int g_rhw_legacy;   /* KELVIN_RHW_LEGACY=1, read once in kelvin_init() */

/* ------------------------------------------------------------------ */
/* Method numbers (xemu nv2a_regs.h)                                   */
/* ------------------------------------------------------------------ */
#define M_SURFACE_CLIP_H        0x0200
#define M_SURFACE_CLIP_V        0x0204
#define M_CONTEXT_DMA_A         0x0184
#define M_CONTEXT_DMA_B         0x0188
#define M_ALPHA_TEST_ENABLE     0x0300
#define M_BLEND_ENABLE          0x0304
#define M_CULL_FACE_ENABLE      0x0308
#define M_CULL_FACE             0x039C   /* c138: FRONT 0x404 / BACK 0x405 / FRONT_AND_BACK 0x408 */
#define M_FRONT_FACE            0x03A0   /* c138: CW 0x900 / CCW 0x901 */
#define M_DEPTH_TEST_ENABLE     0x030C
#define M_ALPHA_FUNC            0x033C
#define M_ALPHA_REF             0x0340
#define M_BLEND_SFACTOR         0x0344
#define M_BLEND_DFACTOR         0x0348
#define M_BLEND_EQUATION        0x0350
#define M_DEPTH_FUNC            0x0354
#define M_COLOR_MASK            0x0358
#define M_DEPTH_MASK            0x035C
#define M_CLIP_MIN              0x0394
#define M_CLIP_MAX              0x0398
#define M_COMPOSITE_MATRIX      0x0680 /* 16 floats */
#define M_VIEWPORT_OFFSET       0x0A20 /* 4 floats */
#define M_VIEWPORT_SCALE        0x0AF0 /* 4 floats */
#define M_TRANSFORM_PROGRAM     0x0B00 /* 32 dwords */
#define M_TRANSFORM_CONSTANT    0x0B80 /* 32 dwords */
#define M_VERTEX_ARRAY_OFFSET   0x1720 /* 16 attributes */
#define M_VERTEX_ARRAY_FORMAT   0x1760 /* 16 attributes */
#define M_BEGIN_END             0x17FC
#define M_ARRAY_ELEMENT16       0x1800
#define M_ARRAY_ELEMENT32       0x1808
#define M_DRAW_ARRAYS           0x1810
#define M_INLINE_ARRAY          0x1818
#define M_VERTEX_DATA2F         0x1880 /* 16 attrs x 2 floats */
#define M_VERTEX_DATA2S         0x1900 /* 16 attrs x 1 dword */
#define M_VERTEX_DATA4UB        0x1940 /* 16 attrs x 1 dword */
#define M_VERTEX_DATA4S         0x1980 /* 16 attrs x 2 dwords */
#define M_VERTEX_DATA4F         0x1A00 /* 16 attrs x 4 floats */
#define M_TEXTURE_BASE          0x1B00 /* +0x40 per stage */
#define M_ZSTENCIL_CLEAR_VALUE  0x1D8C
#define M_COLOR_CLEAR_VALUE     0x1D90
#define M_CLEAR_SURFACE         0x1D94
#define M_CLEAR_RECT_H          0x1D98
#define M_CLEAR_RECT_V          0x1D9C
#define M_TRANSFORM_EXEC_MODE   0x1E94
#define M_TRANSFORM_PROG_LOAD   0x1E9C
#define M_TRANSFORM_PROG_START  0x1EA0
#define M_TRANSFORM_CONST_LOAD  0x1EA4
#define M_TRANSFORM_DATA        0x1E80 /* four floats for vertex-state shaders */
#define M_LAUNCH_TRANSFORM      0x1E90

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
#define PROGRAM_SLOTS  136
#define CONST_ROWS     192
#define MAX_VERTS      65536
#define ATTR_DIFFUSE   3

typedef struct { float v[4]; } Vec4;

typedef struct {
    uint32_t offset, format, address, control0, control1, filter, image_rect, palette;
} TexStage;

typedef struct {
    float x, y, z, rhw;
    uint32_t diffuse, specular;
    float tex[4][4];   /* s/q, t/q, r/q, 1 (r for cube maps); raw s,t,r,q for DOT_* stages */
    float fogv[4];     /* x = fog factor (TEXCOORD4 in the host VS) */
} OutVertex;

static struct {
    int initialized;

    /* vertex program */
    uint32_t program[PROGRAM_SLOTS][4];
    uint32_t program_load;
    uint32_t program_start;
    int program_mode;               /* 1 = program, 0 = fixed function */
    float constants[CONST_ROWS][4];
    uint32_t const_load;
    float transform_data[4];
    float composite[4][4];
    float viewport_offset[4];
    float viewport_scale[4];
    float clip_min, clip_max;

    /* vertex data */
    uint32_t attr_format[16];
    uint32_t attr_offset[16];
    float attr_const[16][4];

    /* current primitive */
    uint32_t prim;
    int in_begin;
    uint32_t inline_data[MAX_VERTS * 4];
    uint32_t inline_count;
    uint32_t *indices;
    uint32_t index_count, index_cap;
    uint32_t array_start, array_count;
    int have_arrays;

    /* raster state */
    uint32_t cull_enable, cull_face, front_face;   /* c138: NV097 cull state (0 = hardware reset default: back / CCW) */
    uint32_t blend_enable, sfactor, dfactor, blend_eq;
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t depth_test, depth_func, depth_mask;
    uint32_t color_mask;
    uint32_t clip_h, clip_v;
    uint32_t dma_a, dma_b;

    /* clear */
    uint32_t clear_color, clear_zstencil, clear_rect_h, clear_rect_v;

    TexStage tex[4];
    NV2APshRegs psh;                /* register combiner state */

    KelvinStats stats;
    uint32_t frame;
} K;

/* Program decode cache is reset with the rest of Kelvin state.  Keep its
 * storage declaration above kelvin_init so the reset is visible to C. */
static uint8_t g_vdec_ok[PROGRAM_SLOTS];

static uint8_t g_vdec_ok[PROGRAM_SLOTS];   /* decoded-slot validity (defined with the decoder below) */
static uint32_t g_prog_gen = 1;   /* bumped whenever the title uploads program words (GPU program cache memo) */

void kelvin_init(void)
{
    memset(&K, 0, sizeof(K));
    memset(g_vdec_ok, 0, sizeof(g_vdec_ok));
    g_rhw_legacy = kenv("KELVIN_RHW_LEGACY") != NULL;
    g_no_fog = kenv("KELVIN_NO_FOG") != NULL;
    {   /* c92: fog_scale from env or ste_video.ini [video] (next to the exe; STE_VIDEO_INI overrides) */
        const char *e = getenv("STE_VIDEO_FOG_SCALE"); char b[64] = "", ini[MAX_PATH], *p; const char *iv = getenv("STE_VIDEO_INI");
        if (e && *e) strncpy(b, e, sizeof(b) - 1);
        else { if (iv && *iv) { strncpy(ini, iv, MAX_PATH - 1); ini[MAX_PATH - 1] = 0; } else { GetModuleFileNameA(NULL, ini, MAX_PATH); p = strrchr(ini, 0x5C); if (p) strcpy(p + 1, "ste_video.ini"); }
               GetPrivateProfileStringA("video", "fog_scale", "", b, sizeof(b), ini); }
        if (b[0]) { g_fog_user_scale = 1; g_fog_scale = (float)atof(b); if (g_fog_scale < 0.25f) g_fog_scale = 0.25f; if (g_fog_scale > 16.0f) g_fog_scale = 16.0f; }
        if (g_fog_scale != 1.0f) fprintf(stderr, "[CAMCFG] fog_scale=%.3f\n", g_fog_scale);
    }
    for (int i = 0; i < 16; ++i) {
        K.attr_const[i][0] = K.attr_const[i][1] = K.attr_const[i][2] = 0.0f;
        K.attr_const[i][3] = 1.0f;
        K.attr_format[i] = 0x02; /* float, size 0: disabled */
    }
    K.attr_const[ATTR_DIFFUSE][0] = K.attr_const[ATTR_DIFFUSE][1] = 1.0f;
    K.attr_const[ATTR_DIFFUSE][2] = K.attr_const[ATTR_DIFFUSE][3] = 1.0f;
    K.color_mask = 0x01010101u;
    K.depth_func = 0x203; /* GL_LEQUAL */
    K.depth_mask = 1;
    K.clip_max = 16777215.0f;
    K.sfactor = 1; K.dfactor = 0;
    K.clip_h = 640u << 16; K.clip_v = 480u << 16;
    K.initialized = 1;
    fprintf(stderr, "[KELVIN] translator initialized\n");
}

void kelvin_get_stats(KelvinStats *out) { if (out) *out = K.stats; }

/* ------------------------------------------------------------------ */
/* Vertex program interpreter (field layout from xemu vsh-prog.c)      */
/* ------------------------------------------------------------------ */
static uint32_t vsh_field(const uint32_t *t, int dw, int bit, int len)
{
    return (t[dw] >> bit) & ((1u << len) - 1u);
}

enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
       MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };

typedef struct {
    Vec4 r[13];      /* R0..R11, R12 = oPos */
    Vec4 o[16];      /* outputs; o[0] mirrors R12 */
    int a0;
    uint32_t written; /* bit per output register the program wrote */
} VshState;

/* Decoded program slot. Field extraction per instruction per vertex was most
 * of the translator's time; slots are decoded once and invalidated when the
 * title uploads new program words (see M_TRANSFORM_PROGRAM). */
typedef struct { uint8_t mux, reg, neg, vin, rel, sw[4]; uint8_t cidx; } VSrc;
typedef struct {
    uint8_t mac, ilu, out_r, mac_mask, ilu_mask, o_mask, orb, o_addr, o_mux, fin;
    VSrc src[3];
} VDec;
static VDec g_vdec[PROGRAM_SLOTS];

/* NV2A vertex-program constant addresses use the Xbox/D3D signed encoding:
 * the upper three bits select a 32-register bank spanning c[-96]..c[95].
 * Keep the interpreter's storage indexed as 0..191, matching Xemu's
 * VSH_D3DSCM_CORRECTION conversion.  Treating the encoded byte as a direct
 * array index selects unrelated constants for most programs (notably the
 * main-menu material/highlight shaders). */
static uint8_t vsh_const_index(uint8_t encoded)
{
    int idx = ((((int)(encoded >> 5) & 7) - 3) * 32)
              + ((int)encoded & 31) + 96;
    return (uint8_t)idx;
}

static void vdec_src(const uint32_t *t, int which, int scalar_c, VSrc *o)
{
    uint32_t sw[4];
    if (which == 0) {
        o->mux = (uint8_t)vsh_field(t, 2, 26, 2); o->reg = (uint8_t)vsh_field(t, 2, 28, 4);
        o->neg = (uint8_t)vsh_field(t, 1, 8, 1);
        sw[0] = vsh_field(t, 1, 6, 2); sw[1] = vsh_field(t, 1, 4, 2);
        sw[2] = vsh_field(t, 1, 2, 2); sw[3] = vsh_field(t, 1, 0, 2);
    } else if (which == 1) {
        o->mux = (uint8_t)vsh_field(t, 2, 11, 2); o->reg = (uint8_t)vsh_field(t, 2, 13, 4);
        o->neg = (uint8_t)vsh_field(t, 2, 25, 1);
        sw[0] = vsh_field(t, 2, 23, 2); sw[1] = vsh_field(t, 2, 21, 2);
        sw[2] = vsh_field(t, 2, 19, 2); sw[3] = vsh_field(t, 2, 17, 2);
    } else {
        o->mux = (uint8_t)vsh_field(t, 3, 28, 2);
        o->reg = (uint8_t)((vsh_field(t, 2, 0, 2) << 2) | vsh_field(t, 3, 30, 2));
        o->neg = (uint8_t)vsh_field(t, 2, 10, 1);
        sw[0] = vsh_field(t, 2, 8, 2); sw[1] = vsh_field(t, 2, 6, 2);
        sw[2] = vsh_field(t, 2, 4, 2); sw[3] = vsh_field(t, 2, 2, 2);
        if (scalar_c) sw[1] = sw[2] = sw[3] = sw[0];
    }
    for (int i = 0; i < 4; ++i) o->sw[i] = (uint8_t)sw[i];
    o->vin = (uint8_t)vsh_field(t, 1, 9, 4);
    o->cidx = vsh_const_index((uint8_t)vsh_field(t, 1, 13, 8));
    o->rel = (uint8_t)vsh_field(t, 3, 1, 1);
}

static const VDec *vdec(uint32_t slot)
{
    VDec *d = &g_vdec[slot];
    if (!g_vdec_ok[slot]) {
        const uint32_t *t = K.program[slot];
        uint32_t ilu = vsh_field(t, 1, 25, 3);
        d->mac = (uint8_t)vsh_field(t, 1, 21, 4);
        d->ilu = (uint8_t)ilu;
        vdec_src(t, 0, 0, &d->src[0]);
        vdec_src(t, 1, 0, &d->src[1]);
        vdec_src(t, 2, ilu >= ILU_RCP && ilu <= ILU_LOG, &d->src[2]);
        d->out_r = (uint8_t)vsh_field(t, 3, 20, 4);
        d->mac_mask = (uint8_t)vsh_field(t, 3, 24, 4);
        d->ilu_mask = (uint8_t)vsh_field(t, 3, 16, 4);
        d->o_mask = (uint8_t)vsh_field(t, 3, 12, 4);
        d->orb = (uint8_t)vsh_field(t, 3, 11, 1);
        /* Keep all eight address bits.  Output-register writes use the low
         * nibble, but vertex-state programs use the full encoded address to
         * write c[-96]..c[95].  Truncating here redirected c105/c106 writes
         * to c9/c10 and left the menu's reflection vectors at zero. */
        d->o_addr = (uint8_t)vsh_field(t, 3, 3, 8);
        d->o_mux = (uint8_t)vsh_field(t, 3, 2, 1);
        d->fin = (uint8_t)vsh_field(t, 3, 0, 1);
        g_vdec_ok[slot] = 1;
    }
    return d;
}

static Vec4 vsh_src(const VSrc *d, const Vec4 *v, const VshState *s)
{
    const float *src;
    static const float zero[4] = { 0, 0, 0, 0 };
    Vec4 out;
    if (d->mux == 1) src = (d->reg == 12) ? s->o[0].v : s->r[d->reg < 12 ? d->reg : 0].v;
    else if (d->mux == 2) src = v[d->vin].v;
    else if (d->mux == 3) {
        int idx = (int)d->cidx;
        if (d->rel) idx += s->a0;
        if (idx < 0) idx = 0;
        if (idx >= CONST_ROWS) idx = CONST_ROWS - 1;
        src = K.constants[idx];
    } else src = zero;
    if (d->neg) {
        out.v[0] = -src[d->sw[0]]; out.v[1] = -src[d->sw[1]];
        out.v[2] = -src[d->sw[2]]; out.v[3] = -src[d->sw[3]];
    } else {
        out.v[0] = src[d->sw[0]]; out.v[1] = src[d->sw[1]];
        out.v[2] = src[d->sw[2]]; out.v[3] = src[d->sw[3]];
    }
    return out;
}

static float vmul(float a, float b)
{
    /* hardware: anything times zero is zero, even inf/NaN */
    if (a == 0.0f || b == 0.0f) return 0.0f;
    return a * b;
}

static void write_masked(Vec4 *dst, const Vec4 *val, uint32_t mask)
{
    for (int i = 0; i < 4; ++i)
        if (mask & (8u >> i)) dst->v[i] = val->v[i];
}

static void vsh_run_from(uint32_t program_start, const Vec4 *v, VshState *s,
                         int context_writes)
{
    memset(s, 0, sizeof(*s));
    s->o[0].v[3] = 1.0f;
    s->o[3].v[3] = 1.0f; s->o[4].v[3] = 1.0f;
    for (int i = 9; i < 13; ++i) s->o[i].v[3] = 1.0f;

    for (uint32_t slot = program_start; slot < PROGRAM_SLOTS; ++slot) {
        const VDec *d = vdec(slot);
        uint32_t mac = d->mac;
        uint32_t ilu = d->ilu;
        Vec4 a = vsh_src(&d->src[0], v, s);
        Vec4 b = vsh_src(&d->src[1], v, s);
        Vec4 c = vsh_src(&d->src[2], v, s);
        Vec4 mr, ir;
        int mac_valid = 0, ilu_valid = 0, arl = 0;
        memset(&mr, 0, sizeof(mr)); memset(&ir, 0, sizeof(ir));

        switch (mac) {
        case MAC_MOV: mr = a; mac_valid = 1; break;
        case MAC_MUL: for (int i = 0; i < 4; ++i) mr.v[i] = vmul(a.v[i], b.v[i]); mac_valid = 1; break;
        case MAC_ADD: for (int i = 0; i < 4; ++i) mr.v[i] = a.v[i] + c.v[i]; mac_valid = 1; break;
        case MAC_MAD: for (int i = 0; i < 4; ++i) mr.v[i] = vmul(a.v[i], b.v[i]) + c.v[i]; mac_valid = 1; break;
        case MAC_DP3: { float d = vmul(a.v[0], b.v[0]) + vmul(a.v[1], b.v[1]) + vmul(a.v[2], b.v[2]);
                        for (int i = 0; i < 4; ++i) mr.v[i] = d; mac_valid = 1; } break;
        case MAC_DPH: { float d = vmul(a.v[0], b.v[0]) + vmul(a.v[1], b.v[1]) + vmul(a.v[2], b.v[2]) + b.v[3];
                        for (int i = 0; i < 4; ++i) mr.v[i] = d; mac_valid = 1; } break;
        case MAC_DP4: { float d = vmul(a.v[0], b.v[0]) + vmul(a.v[1], b.v[1]) + vmul(a.v[2], b.v[2]) + vmul(a.v[3], b.v[3]);
                        for (int i = 0; i < 4; ++i) mr.v[i] = d; mac_valid = 1; } break;
        case MAC_DST: mr.v[0] = 1.0f; mr.v[1] = vmul(a.v[1], b.v[1]); mr.v[2] = a.v[2]; mr.v[3] = b.v[3]; mac_valid = 1; break;
        case MAC_MIN: for (int i = 0; i < 4; ++i) mr.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; mac_valid = 1; break;
        case MAC_MAX: for (int i = 0; i < 4; ++i) mr.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; mac_valid = 1; break;
        case MAC_SLT: for (int i = 0; i < 4; ++i) mr.v[i] = a.v[i] < b.v[i] ? 1.0f : 0.0f; mac_valid = 1; break;
        case MAC_SGE: for (int i = 0; i < 4; ++i) mr.v[i] = a.v[i] >= b.v[i] ? 1.0f : 0.0f; mac_valid = 1; break;
        case MAC_ARL: arl = 1; break;
        default: break;
        }

        switch (ilu) {
        case ILU_MOV: ir = c; ilu_valid = 1; break;
        case ILU_RCP: { float r = 1.0f / c.v[0]; for (int i = 0; i < 4; ++i) ir.v[i] = r; ilu_valid = 1; } break;
        case ILU_RCC: { float r = 1.0f / c.v[0];
                        if (r > 0.0f) r = r < 5.42101e-20f ? 5.42101e-20f : r > 1.84467e+19f ? 1.84467e+19f : r;
                        else r = r > -5.42101e-20f ? -5.42101e-20f : r < -1.84467e+19f ? -1.84467e+19f : r;
                        for (int i = 0; i < 4; ++i) ir.v[i] = r; ilu_valid = 1; } break;
        case ILU_RSQ: { float x = fabsf(c.v[0]); float r = x == 0.0f ? INFINITY : 1.0f / sqrtf(x);
                        for (int i = 0; i < 4; ++i) ir.v[i] = r; ilu_valid = 1; } break;
        case ILU_EXP: { float x = c.v[0]; ir.v[0] = exp2f(floorf(x)); ir.v[1] = x - floorf(x);
                        ir.v[2] = exp2f(x); ir.v[3] = 1.0f; ilu_valid = 1; } break;
        case ILU_LOG: { float x = fabsf(c.v[0]);
                        if (x == 0.0f) { ir.v[0] = -INFINITY; ir.v[1] = 1.0f; ir.v[2] = -INFINITY; }
                        else { float e = floorf(log2f(x)); ir.v[0] = e; ir.v[1] = x / exp2f(e); ir.v[2] = log2f(x); }
                        ir.v[3] = 1.0f; ilu_valid = 1; } break;
        case ILU_LIT: { float x = c.v[0] > 0 ? c.v[0] : 0, y = c.v[1] > 0 ? c.v[1] : 0, w = c.v[3];
                        if (w < -127.9961f) w = -127.9961f; if (w > 127.9961f) w = 127.9961f;
                        ir.v[0] = 1.0f; ir.v[1] = x; ir.v[2] = x > 0 ? exp2f(w * log2f(y)) : 0.0f; ir.v[3] = 1.0f;
                        ilu_valid = 1; } break;
        default: break;
        }

        uint32_t out_r = d->out_r;
        uint32_t mac_mask = d->mac_mask;
        uint32_t ilu_mask = d->ilu_mask;
        uint32_t o_mask = d->o_mask;
        uint32_t orb = d->orb;
        uint32_t o_addr = d->o_addr;
        uint32_t o_mux = d->o_mux;

        if (arl) s->a0 = (int)floorf(a.v[0] + 0.001f);

        if (mac_valid) {
            uint32_t m = mac_mask;
            if (ilu != ILU_NOP && out_r == 1) m = 0; /* paired MAC to R1 is dropped */
            if (m) {
                if (out_r == 12) write_masked(&s->o[0], &mr, m);
                else if (out_r < 12) write_masked(&s->r[out_r], &mr, m);
            }
            if (o_mux == 0 && o_mask) {
                if (orb == 1) {
                    uint32_t output = o_addr & 0xFu;
                    write_masked(&s->o[output], &mr, o_mask);
                    s->written |= 1u << output;
                } else if (context_writes) {
                    uint32_t constant = vsh_const_index((uint8_t)o_addr);
                    if (constant < CONST_ROWS)
                        write_masked((Vec4 *)K.constants[constant], &mr, o_mask);
                }
            }
        }
        if (ilu_valid) {
            uint32_t r = (mac != MAC_NOP) ? 1 : out_r; /* paired ILU writes R1 */
            if (ilu_mask) {
                if (r == 12) write_masked(&s->o[0], &ir, ilu_mask);
                else if (r < 12) write_masked(&s->r[r], &ir, ilu_mask);
            }
            if (o_mux == 1 && o_mask) {
                if (orb == 1) {
                    uint32_t output = o_addr & 0xFu;
                    write_masked(&s->o[output], &ir, o_mask);
                    s->written |= 1u << output;
                } else if (context_writes) {
                    uint32_t constant = vsh_const_index((uint8_t)o_addr);
                    if (constant < CONST_ROWS)
                        write_masked((Vec4 *)K.constants[constant], &ir, o_mask);
                }
            }
        }

        if (d->fin) break; /* FINAL */
    }
}

static void vsh_run(const Vec4 *v, VshState *s)
{
    vsh_run_from(K.program_start, v, s, 0);
}

/* ------------------------------------------------------------------ */
/* Vertex program -> HLSL (GPU vertex processing, c69)                 */
/* ------------------------------------------------------------------ */
/* Straight-line translation of exactly what vsh_run_from() does (same decode, same pairing/masking rules); the program has no
 * branches so every register write, the `written` mask and the input set are known at translation time.  The post-processing
 * of transform_vertex() (window-space z scale, w clamp, colour quantise, texcoord divide, fog factor) is emitted after the
 * program.  Plain SM5 HLSL: scalar ternaries and cbuffers only, ATTRn user semantics, so it also builds with DXC for SPIR-V. */
typedef struct { char *p; size_t n, cap; } KStr;

static void ks_add(KStr *s, const char *fmt, ...)
{
    va_list ap, ap2;
    char tmp[512], *big = NULL, *src = tmp;
    int n;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof(tmp)) {
        big = (char *)malloc((size_t)n + 1);
        if (big) { vsnprintf(big, (size_t)n + 1, fmt, ap2); src = big; }
        else n = (int)sizeof(tmp) - 1;
    }
    va_end(ap2);
    if (n < 0) { free(big); return; }
    if (s->n + (size_t)n + 1 > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 8192;
        char *q;
        while (cap < s->n + (size_t)n + 1) cap *= 2;
        q = (char *)realloc(s->p, cap);
        if (!q) return;
        s->p = q; s->cap = cap;
    }
    memcpy(s->p + s->n, src, (size_t)n);
    s->n += (size_t)n;
    s->p[s->n] = 0;
    free(big);
}

static const char g_kvp_prelude[] =
    "cbuffer KvpConst : register(b4) { float4 C[192]; };\n"
    "cbuffer KvpDraw : register(b5) {\n"
    "    float2 ScreenSize; float XScale; float XOffset;\n"
    "    float ClipMax; float FogEn; uint FogMode; float FogP0;\n"
    "    float FogP1; float FogP2; uint DotRaw; float KvpPad;\n"
    "    float4 TScale[2];\n"
    "};\n"
    "float vm(float a, float b) { if (a == 0.0 || b == 0.0) return 0.0; return a * b; }\n"
    "float4 vm4(float4 a, float4 b) { return float4(vm(a.x, b.x), vm(a.y, b.y), vm(a.z, b.z), vm(a.w, b.w)); }\n"
    "float4 vmin4(float4 a, float4 b) { return float4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w); }\n"
    "float4 vmax4(float4 a, float4 b) { return float4(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w); }\n"
    "float4 vslt4(float4 a, float4 b) { return float4(a.x < b.x ? 1.0 : 0.0, a.y < b.y ? 1.0 : 0.0, a.z < b.z ? 1.0 : 0.0, a.w < b.w ? 1.0 : 0.0); }\n"
    "float4 vsge4(float4 a, float4 b) { return float4(a.x >= b.x ? 1.0 : 0.0, a.y >= b.y ? 1.0 : 0.0, a.z >= b.z ? 1.0 : 0.0, a.w >= b.w ? 1.0 : 0.0); }\n"
    "float vinf() { return asfloat(0x7f800000u); }\n"
    "float vrcc(float x) {\n"
    "    float r = 1.0 / x;\n"
    "    if (r > 0.0) r = r < 5.42101e-20 ? 5.42101e-20 : (r > 1.84467e+19 ? 1.84467e+19 : r);\n"
    "    else r = r > -5.42101e-20 ? -5.42101e-20 : (r < -1.84467e+19 ? -1.84467e+19 : r);\n"
    "    return r;\n"
    "}\n"
    "float vrsq(float x) { float ax = abs(x); if (ax == 0.0) return vinf(); return 1.0 / sqrt(ax); }\n"
    "float4 vexp(float x) { float f = floor(x); return float4(exp2(f), x - f, exp2(x), 1.0); }\n"
    "float4 vlog(float x) {\n"
    "    float ax = abs(x);\n"
    "    if (ax == 0.0) return float4(-vinf(), 1.0, -vinf(), 1.0);\n"
    "    float e = floor(log2(ax));\n"
    "    return float4(e, ax / exp2(e), log2(ax), 1.0);\n"
    "}\n"
    "float4 vlit(float4 c) {\n"
    "    float lx = c.x > 0.0 ? c.x : 0.0;\n"
    "    float ly = c.y > 0.0 ? c.y : 0.0;\n"
    "    float lw = c.w;\n"
    "    if (lw < -127.9961) lw = -127.9961;\n"
    "    if (lw > 127.9961) lw = 127.9961;\n"
    "    return float4(1.0, lx, lx > 0.0 ? exp2(lw * log2(ly)) : 0.0, 1.0);\n"
    "}\n"
    "float4 vnan1(float4 c) { return float4(isnan(c.x) ? 1.0 : c.x, isnan(c.y) ? 1.0 : c.y, isnan(c.z) ? 1.0 : c.z, isnan(c.w) ? 1.0 : c.w); }\n"
    "float4 vq8(float4 c) { return floor(saturate(vnan1(c)) * 255.0 + 0.5) / 255.0; }\n"
    "struct VS_OUT {\n"
    "#ifdef KVP_VERIFY\n"
    "    float4 wpos : TEXCOORD0; float4 diffuse : TEXCOORD1; float4 specular : TEXCOORD2;\n"
    "    float4 tex0 : TEXCOORD3; float4 tex1 : TEXCOORD4; float4 tex2 : TEXCOORD5; float4 tex3 : TEXCOORD6; float fog : TEXCOORD7;\n"
    "#else\n"
    "    float4 pos : SV_POSITION; float4 diffuse : COLOR0; float4 specular : COLOR1;\n"
    "    float4 tex0 : TEXCOORD0; float4 tex1 : TEXCOORD1; float4 tex2 : TEXCOORD2; float4 tex3 : TEXCOORD3; float fog : TEXCOORD4;\n"
    "    float4 viewpos : TEXCOORD5;\n"
    "#endif\n"
    "};\n";

static void kvp_wmask(char *dst, uint32_t m)
{
    int n = 0;
    if (m & 8) dst[n++] = 'x';
    if (m & 4) dst[n++] = 'y';
    if (m & 2) dst[n++] = 'z';
    if (m & 1) dst[n++] = 'w';
    dst[n] = 0;
}

/* assign val.<mask> to dst.<mask> */
static void kvp_write(KStr *s, const char *dst, const char *val, uint32_t m)
{
    char sw[8];
    if (!(m & 15u)) return;
    kvp_wmask(sw, m & 15u);
    ks_add(s, "    %s.%s = %s.%s;\n", dst, sw, val, sw);
}

static void kvp_src_expr(const VSrc *d, char *out, size_t cap, uint32_t *inmask)
{
    char base[64];
    char sw[5];
    static const char comp[4] = { 'x', 'y', 'z', 'w' };
    if (d->mux == 1) {
        if (d->reg == 12) snprintf(base, sizeof(base), "O[0]");
        else snprintf(base, sizeof(base), "R[%u]", d->reg < 12 ? d->reg : 0u);
    } else if (d->mux == 2) {
        snprintf(base, sizeof(base), "vin.a%u", (unsigned)d->vin);
        *inmask |= 1u << d->vin;
    } else if (d->mux == 3) {
        unsigned idx = d->cidx;
        if (d->rel) snprintf(base, sizeof(base), "C[clamp((int)%u + A0, 0, 191)]", idx);
        else snprintf(base, sizeof(base), "C[%u]", idx >= CONST_ROWS ? CONST_ROWS - 1u : idx);
    } else {
        snprintf(base, sizeof(base), "float4(0.0, 0.0, 0.0, 0.0)");
    }
    for (int i = 0; i < 4; ++i) sw[i] = comp[d->sw[i] & 3];
    sw[4] = 0;
    snprintf(out, cap, "%s(%s).%s", d->neg ? "-" : "", base, sw);
}

/* Returns the HLSL source (malloc'd) or NULL; *inmask receives the attribute registers the program reads. */
static char *kvp_translate(uint32_t start, uint32_t *inmask_out)
{
    KStr body = {0}, all = {0};
    uint32_t inmask = 0, written = 0;
    for (uint32_t slot = start; slot < PROGRAM_SLOTS; ++slot) {
        const VDec *d = vdec(slot);
        uint32_t mac = d->mac, ilu = d->ilu;
        int uA = 0, uB = 0, uC = 0;
        char ea[128], eb[128], ec[128];
        int mac_valid = mac >= MAC_MOV && mac <= MAC_SGE;
        int ilu_valid = ilu >= ILU_MOV && ilu <= ILU_LIT;
        switch (mac) {
        case MAC_MOV: case MAC_ARL: uA = 1; break;
        case MAC_MUL: uA = uB = 1; break;
        case MAC_ADD: uA = uC = 1; break;
        case MAC_MAD: uA = uB = uC = 1; break;
        case MAC_DP3: case MAC_DPH: case MAC_DP4: case MAC_DST: case MAC_MIN: case MAC_MAX: case MAC_SLT: case MAC_SGE:
            uA = uB = 1; break;
        default: break;
        }
        if (ilu_valid) uC = 1;
        ks_add(&body, "    { // slot %u mac=%u ilu=%u\n", slot, mac, ilu);
        if (uA) { kvp_src_expr(&d->src[0], ea, sizeof(ea), &inmask); ks_add(&body, "    float4 a = %s;\n", ea); }
        if (uB) { kvp_src_expr(&d->src[1], eb, sizeof(eb), &inmask); ks_add(&body, "    float4 b = %s;\n", eb); }
        if (uC) { kvp_src_expr(&d->src[2], ec, sizeof(ec), &inmask); ks_add(&body, "    float4 c = %s;\n", ec); }
        if (mac_valid) {
            const char *x = NULL;
            switch (mac) {
            case MAC_MOV: x = "a"; break;
            case MAC_MUL: x = "vm4(a, b)"; break;
            case MAC_ADD: x = "a + c"; break;
            case MAC_MAD: x = "vm4(a, b) + c"; break;
            case MAC_DP3: x = "(vm(a.x, b.x) + vm(a.y, b.y) + vm(a.z, b.z)).xxxx"; break;
            case MAC_DPH: x = "(vm(a.x, b.x) + vm(a.y, b.y) + vm(a.z, b.z) + b.w).xxxx"; break;
            case MAC_DP4: x = "(vm(a.x, b.x) + vm(a.y, b.y) + vm(a.z, b.z) + vm(a.w, b.w)).xxxx"; break;
            case MAC_DST: x = "float4(1.0, vm(a.y, b.y), a.z, b.w)"; break;
            case MAC_MIN: x = "vmin4(a, b)"; break;
            case MAC_MAX: x = "vmax4(a, b)"; break;
            case MAC_SLT: x = "vslt4(a, b)"; break;
            default:      x = "vsge4(a, b)"; break;
            }
            ks_add(&body, "    float4 mr = %s;\n", x);
        }
        if (ilu_valid) {
            const char *x = NULL;
            switch (ilu) {
            case ILU_MOV: x = "c"; break;
            case ILU_RCP: x = "(1.0 / c.x).xxxx"; break;
            case ILU_RCC: x = "vrcc(c.x).xxxx"; break;
            case ILU_RSQ: x = "vrsq(c.x).xxxx"; break;
            case ILU_EXP: x = "vexp(c.x)"; break;
            case ILU_LOG: x = "vlog(c.x)"; break;
            default:      x = "vlit(c)"; break;
            }
            ks_add(&body, "    float4 ir = %s;\n", x);
        }
        if (mac == MAC_ARL) ks_add(&body, "    A0 = (int)floor(a.x + 0.001);\n");
        if (mac_valid) {
            uint32_t m = d->mac_mask;
            char dst[16];
            if (ilu != ILU_NOP && d->out_r == 1) m = 0;      /* paired MAC to R1 is dropped */
            if (m) {
                if (d->out_r == 12) { kvp_write(&body, "O[0]", "mr", m); }
                else if (d->out_r < 12) { snprintf(dst, sizeof(dst), "R[%u]", (unsigned)d->out_r); kvp_write(&body, dst, "mr", m); }
            }
            if (d->o_mux == 0 && d->o_mask && d->orb == 1) {
                unsigned o = d->o_addr & 0xFu;
                snprintf(dst, sizeof(dst), "O[%u]", o);
                kvp_write(&body, dst, "mr", d->o_mask);
                written |= 1u << o;
            }
        }
        if (ilu_valid) {
            uint32_t r = (mac != MAC_NOP) ? 1u : d->out_r;   /* paired ILU writes R1 */
            char dst[16];
            if (d->ilu_mask) {
                if (r == 12) { kvp_write(&body, "O[0]", "ir", d->ilu_mask); }
                else if (r < 12) { snprintf(dst, sizeof(dst), "R[%u]", r); kvp_write(&body, dst, "ir", d->ilu_mask); }
            }
            if (d->o_mux == 1 && d->o_mask && d->orb == 1) {
                unsigned o = d->o_addr & 0xFu;
                snprintf(dst, sizeof(dst), "O[%u]", o);
                kvp_write(&body, dst, "ir", d->o_mask);
                written |= 1u << o;
            }
        }
        ks_add(&body, "    }\n");
        if (d->fin) break;
    }
    if (!body.p || !inmask) { free(body.p); return NULL; }

    ks_add(&all, "%s", g_kvp_prelude);
    ks_add(&all, "struct VS_IN {\n");
    for (int a = 0; a < 16; ++a)
        if (inmask & (1u << a)) ks_add(&all, "    float4 a%d : ATTR%d;\n", a, a);
    ks_add(&all, "};\nVS_OUT main(VS_IN vin) {\n");
    ks_add(&all, "    float4 R[12]; float4 O[16]; int A0 = 0;\n"
                 "    for (int i = 0; i < 12; ++i) R[i] = float4(0.0, 0.0, 0.0, 0.0);\n"
                 "    for (int j = 0; j < 16; ++j) O[j] = float4(0.0, 0.0, 0.0, 0.0);\n"
                 "    O[0].w = 1.0; O[3].w = 1.0; O[4].w = 1.0; O[9].w = 1.0; O[10].w = 1.0; O[11].w = 1.0; O[12].w = 1.0;\n");
    if (all.p && body.p) {
        /* append body */
        size_t need = all.n + body.n + 1;
        if (need > all.cap) { char *q = (char *)realloc(all.p, need + 8192); if (!q) { free(all.p); free(body.p); return NULL; } all.p = q; all.cap = need + 8192; }
        memcpy(all.p + all.n, body.p, body.n + 1);
        all.n += body.n;
    }
    free(body.p);
    /* ---- transform_vertex() epilogue ---- */
    ks_add(&all, "    VS_OUT o;\n");
    ks_add(&all, "    float4 pos = O[0];\n"
                 "    float cz = (ClipMax != 0.0) ? pos.z / ClipMax : pos.z;\n"
                 "    if (isnan(cz)) cz = 0.0;\n"
                 "    float w = pos.w;\n"
                 "    if (isnan(w)) w = 1.0;\n"
                 "    if (w > 0.0 || (w == 0.0 && asuint(w) == 0u)) w = w < 5.421011e-20 ? 5.421011e-20 : (w > 1.8446744e19 ? 1.8446744e19 : w);\n"
                 "    else w = w > -5.421011e-20 ? -5.421011e-20 : (w < -1.8446744e19 ? -1.8446744e19 : w);\n");
    ks_add(&all, "    float4 d0 = %s;\n", (written & (1u << 3)) ? "O[3]" : "float4(1.0, 1.0, 1.0, 1.0)");
    ks_add(&all, "    float fogc = %s;\n", (written & (1u << 5)) ? "O[5].x" : "0.0");
    ks_add(&all, "    float fogf = 1.0;\n"
                 "    if (FogEn != 0.0) {\n"
                 "        float fd = fogc;\n"
                 "        if (FogMode >= 0x802u) fd = abs(fd);\n"
                 "        float f;\n"
                 "        if (FogMode == 0x800u || FogMode == 0x802u) f = exp(-(FogP0 + FogP1 * fd));\n"
                 "        else if (FogMode == 0x801u || FogMode == 0x803u) { float fe = FogP0 + FogP1 * fd; f = exp(-fe * fe); }\n"
                 "        else f = FogP0 + FogP1 * fd;\n"
                 "        fogf = isnan(f) ? 1.0 : saturate(f);\n"
                 "    }\n");
    ks_add(&all, "    float4 tx[4];\n"
                 "    float4 ts[4];\n"
                 "    ts[0] = float4(TScale[0].xy, 0.0, 0.0); ts[1] = float4(TScale[0].zw, 0.0, 0.0);\n"
                 "    ts[2] = float4(TScale[1].xy, 0.0, 0.0); ts[3] = float4(TScale[1].zw, 0.0, 0.0);\n"
                 "    [unroll] for (int i = 0; i < 4; ++i) {\n"
                 "        float4 t = O[9 + i];\n"
                 "        float3 uvr = t.xyz;\n"
                 "        float q = t.w;\n"
                 "        if (q != 0.0 && q != 1.0 && !isnan(q)) uvr = float3(uvr.x / q, uvr.y / q, uvr.z / q);\n"
                 "        if (((DotRaw >> (uint)i) & 1u) != 0u) tx[i] = t;\n"
                 "        else tx[i] = float4(uvr.x * ts[i].x, uvr.y * ts[i].y, uvr.z, 1.0);\n"
                 "    }\n");
    ks_add(&all, "#ifdef KVP_VERIFY\n"
                 "    o.wpos = float4(pos.x, pos.y, cz, w);\n"
                 "#else\n"
                 "    float gx = pos.x * XScale + XOffset;\n"
                 "    o.pos = float4(((gx / ScreenSize.x) * 2.0 - 1.0) * w, (1.0 - (pos.y / ScreenSize.y) * 2.0) * w, cz * w, w);\n"
                 "    o.viewpos = float4(0.0, 0.0, 0.0, 1.0);\n"
                 "#endif\n"
                 "    o.diffuse = vq8(d0);\n"
                 "    o.specular = vq8(O[4]);\n"
                 "    o.tex0 = tx[0]; o.tex1 = tx[1]; o.tex2 = tx[2]; o.tex3 = tx[3];\n"
                 "    o.fog = fogf;\n"
                 "    return o;\n}\n");
    *inmask_out = inmask;
    return all.p;
}

/* ------------------------------------------------------------------ */
/* Vertex fetch                                                        */
/* ------------------------------------------------------------------ */
static uint32_t attr_type(int i) { return K.attr_format[i] & 0xF; }
static uint32_t attr_size(int i) { return (K.attr_format[i] >> 4) & 0xF; }
static uint32_t attr_stride(int i) { return K.attr_format[i] >> 8; }

static void decode_attr(int i, const uint8_t *p, float out[4])
{
    uint32_t size = attr_size(i);
    out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
    switch (attr_type(i)) {
    case 0: /* UB_D3D: stored B,G,R,A -> RGBA */
        if (size >= 4 || size == 3) {
            out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[0] / 255.0f;
            out[3] = size >= 4 ? p[3] / 255.0f : 1.0f;
        } else {
            for (uint32_t k = 0; k < size; ++k) out[k] = p[k] / 255.0f;
        }
        break;
    case 4: /* UB_OGL */
        for (uint32_t k = 0; k < size && k < 4; ++k) out[k] = p[k] / 255.0f;
        break;
    case 1: /* S1: signed normalized */
        for (uint32_t k = 0; k < size && k < 4; ++k) {
            int16_t s; memcpy(&s, p + 2 * k, 2);
            out[k] = (2.0f * s + 1.0f) / 65535.0f;
        }
        break;
    case 5: /* S32K */
        for (uint32_t k = 0; k < size && k < 4; ++k) {
            int16_t s; memcpy(&s, p + 2 * k, 2);
            out[k] = (float)s;
        }
        break;
    case 2: /* float */
        for (uint32_t k = 0; k < size && k < 4; ++k) memcpy(&out[k], p + 4 * k, 4);
        break;
    case 6: { /* CMP: 11:11:10 signed normalized */
        uint32_t w; memcpy(&w, p, 4);
        int32_t x = (int32_t)(w << 21) >> 21, y = (int32_t)(w << 10) >> 21, z = (int32_t)w >> 22;
        out[0] = x / 1023.0f; out[1] = y / 1023.0f; out[2] = z / 511.0f;
        break;
    }
    default: break;
    }
}

static uint32_t attr_inline_dwords(int i)
{
    uint32_t size = attr_size(i);
    if (!size) return 0;
    switch (attr_type(i)) {
    case 0: case 4: return 1;
    case 1: case 5: return (2 * size + 3) / 4;
    case 2: return size;
    case 6: return 1;
    default: return size;
    }
}

static uint32_t inline_vertex_dwords(void)
{
    uint32_t n = 0;
    for (int i = 0; i < 16; ++i) n += attr_inline_dwords(i);
    return n;
}

static void fetch_inline(uint32_t index, Vec4 v[16])
{
    uint32_t stride = inline_vertex_dwords();
    const uint32_t *base = K.inline_data + (size_t)index * stride;
    for (int i = 0; i < 16; ++i) {
        uint32_t n = attr_inline_dwords(i);
        if (n) { decode_attr(i, (const uint8_t *)base, v[i].v); base += n; }
        else memcpy(v[i].v, K.attr_const[i], sizeof(v[i].v));
    }
}

static void fetch_array(uint32_t index, Vec4 v[16])
{
    for (int i = 0; i < 16; ++i) {
        if (attr_size(i)) {
            const uint8_t *p = guest_ptr(K.attr_offset[i] + index * attr_stride(i));
            decode_attr(i, p, v[i].v);
        } else {
            memcpy(v[i].v, K.attr_const[i], sizeof(v[i].v));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Textures                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t key_offset, key_format, key_w, key_h, key_pitch, key_palette;
    uint64_t hash;
    uint32_t last_frame, checked_frame;
    IDirect3DTexture8 *texture;
    uint8_t repl;   /* c128: texture is an HD replacement from a texture pack (not the decoded original size) */
} TexCacheEntry;

#define TEX_CACHE_SIZE 1024
static TexCacheEntry g_tex_cache[TEX_CACHE_SIZE];
#define TEX_IDX_BITS 13
#define TEX_IDX_SIZE (1u << TEX_IDX_BITS)
static int16_t g_tex_idx[TEX_IDX_SIZE];   /* c98: key hash -> cache slot candidate (0 = slot 0 when unset; always validated against the entry's key) */


static int tex_is_linear(uint32_t color)
{
    switch (color) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x17: case 0x1B: case 0x1C:
    case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x24: case 0x25: case 0x2E:
    case 0x2F: case 0x30: case 0x31: case 0x35: case 0x3F: case 0x40: case 0x41:
        return 1;
    default:
        return 0;
    }
}

static int tex_is_dxt(uint32_t color) { return color == 0x0C || color == 0x0E || color == 0x0F; }

/* Bytes per pixel for uncompressed formats, 0 if unsupported. */
static uint32_t tex_bpp(uint32_t color)
{
    switch (color) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1B: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x17:
    case 0x1A: case 0x1C: case 0x1D: case 0x20: case 0x24: case 0x25: case 0x27:
    case 0x28: case 0x29: case 0x35:
        return 2;
    case 0x06: case 0x07: case 0x12: case 0x1E: case 0x3A: case 0x3B: case 0x3C:
    case 0x3F: case 0x40: case 0x41:
        return 4;
    default:
        return 0;
    }
}

static uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }
static uint32_t expand4(uint32_t v) { return (v << 4) | v; }

static uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static uint32_t yuv_to_argb(int y, int u, int v)
{
    int c = y - 16, d = u - 128, e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    return argb(255, clamp8(r), clamp8(g), clamp8(b));
}

static uint32_t decode_texel(uint32_t color, const uint8_t *p, const uint32_t *palette)
{
    uint16_t w;
    switch (color) {
    case 0x00: case 0x13: return argb(255, p[0], p[0], p[0]);                 /* Y8 */
    case 0x01: case 0x1B: return argb(p[0], p[0], p[0], p[0]);                /* AY8 */
    case 0x19: case 0x1F: return argb(p[0], 255, 255, 255);                   /* A8 */
    case 0x0B: return palette ? palette[p[0]] : argb(255, p[0], p[0], p[0]);  /* I8 */
    case 0x1A: case 0x20: return argb(p[1], p[0], p[0], p[0]);                /* A8Y8 */
    case 0x02: case 0x10: memcpy(&w, p, 2);                                   /* A1R5G5B5 */
        return argb((w & 0x8000) ? 255 : 0, expand5((w >> 10) & 31), expand5((w >> 5) & 31), expand5(w & 31));
    case 0x03: case 0x1C: memcpy(&w, p, 2);                                   /* X1R5G5B5 */
        return argb(255, expand5((w >> 10) & 31), expand5((w >> 5) & 31), expand5(w & 31));
    case 0x04: case 0x1D: memcpy(&w, p, 2);                                   /* A4R4G4B4 */
        return argb(expand4(w >> 12), expand4((w >> 8) & 15), expand4((w >> 4) & 15), expand4(w & 15));
    case 0x05: case 0x11: memcpy(&w, p, 2);                                   /* R5G6B5 */
        return argb(255, expand5(w >> 11), expand6((w >> 5) & 63), expand5(w & 31));
    case 0x27: memcpy(&w, p, 2);                                              /* R6G5B5 */
        return argb(255, expand6(w >> 10), expand5((w >> 5) & 31), expand5(w & 31));
    case 0x28: case 0x17: return argb(255, 0, p[1], p[0]);                    /* G8B8 */
    case 0x29: return argb(255, p[1], 0, p[0]);                               /* R8B8 */
    case 0x35: memcpy(&w, p, 2); return argb(255, w >> 8, w >> 8, w >> 8);    /* Y16 */
    case 0x06: case 0x12: return argb(p[3], p[2], p[1], p[0]);                /* A8R8G8B8 */
    case 0x07: case 0x1E: return argb(255, p[2], p[1], p[0]);                 /* X8R8G8B8 */
    case 0x3A: case 0x3F: return argb(p[3], p[0], p[1], p[2]);                /* A8B8G8R8 */
    case 0x3B: case 0x40: return argb(p[0], p[1], p[2], p[3]);                /* B8G8R8A8 */
    case 0x3C: case 0x41: return argb(p[0], p[3], p[2], p[1]);                /* R8G8B8A8 */
    default: return argb(255, 255, 0, 255);
    }
}

static void swizzle_masks(uint32_t w, uint32_t h, uint32_t *mx, uint32_t *my)
{
    uint32_t x = 0, y = 0, bit = 1, mb = 1;
    int done;
    do {
        done = 1;
        if (bit < w) { x |= mb; mb <<= 1; done = 0; }
        if (bit < h) { y |= mb; mb <<= 1; done = 0; }
        bit <<= 1;
    } while (!done);
    *mx = x; *my = y;
}

static uint32_t spread(uint32_t v, uint32_t mask)
{
    uint32_t r = 0, bit = 1;
    for (uint32_t m = mask; m; m &= m - 1) {
        uint32_t low = m & (~m + 1);
        if (v & bit) r |= low;
        bit <<= 1;
    }
    return r;
}

static void decode_dxt_block(uint32_t color, const uint8_t *blk, uint32_t out[16])
{
    const uint8_t *cb = color == 0x0C ? blk : blk + 8;
    uint16_t c0, c1; memcpy(&c0, cb, 2); memcpy(&c1, cb + 2, 2);
    uint32_t pal[4];
    uint32_t r0 = expand5(c0 >> 11), g0 = expand6((c0 >> 5) & 63), b0 = expand5(c0 & 31);
    uint32_t r1 = expand5(c1 >> 11), g1 = expand6((c1 >> 5) & 63), b1 = expand5(c1 & 31);
    pal[0] = argb(255, r0, g0, b0);
    pal[1] = argb(255, r1, g1, b1);
    if (c0 > c1 || color != 0x0C) {
        pal[2] = argb(255, (2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3);
        pal[3] = argb(255, (r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3);
    } else {
        pal[2] = argb(255, (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2);
        pal[3] = 0;
    }
    uint32_t bits; memcpy(&bits, cb + 4, 4);
    for (int i = 0; i < 16; ++i) out[i] = pal[(bits >> (2 * i)) & 3];

    if (color == 0x0E) {           /* DXT2/3 explicit alpha */
        for (int i = 0; i < 16; ++i) {
            uint32_t a = (blk[i / 2] >> ((i & 1) * 4)) & 15;
            out[i] = (out[i] & 0x00FFFFFFu) | (expand4(a) << 24);
        }
    } else if (color == 0x0F) {    /* DXT4/5 interpolated alpha */
        uint32_t a[8]; a[0] = blk[0]; a[1] = blk[1];
        if (a[0] > a[1]) for (int i = 2; i < 8; ++i) a[i] = ((8 - i) * a[0] + (i - 1) * a[1]) / 7;
        else { for (int i = 2; i < 6; ++i) a[i] = ((6 - i) * a[0] + (i - 1) * a[1]) / 5; a[6] = 0; a[7] = 255; }
        uint64_t ab = 0; for (int i = 0; i < 6; ++i) ab |= (uint64_t)blk[2 + i] << (8 * i);
        for (int i = 0; i < 16; ++i)
            out[i] = (out[i] & 0x00FFFFFFu) | (a[(ab >> (3 * i)) & 7] << 24);
    }
}

/* c99: four independent FNV-style lanes (the single-lane loop was latency-bound at ~1 GB/s and hash_bytes was ~6% of the render thread);
 * the value is only used as an in-memory change detector / cache key, never persisted. */
static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    const uint64_t P = 1099511628211ull;
    uint64_t h0 = 1469598103934665603ull, h1 = h0 ^ 0x9E3779B97F4A7C15ull, h2 = h0 ^ 0xC2B2AE3D27D4EB4Full, h3 = h0 ^ 0x165667B19E3779F9ull;
    const uint64_t *q = (const uint64_t *)p;
    size_t words = n / 8, i = 0;
    for (; i + 4 <= words; i += 4) {
        h0 = (h0 ^ q[i]) * P; h1 = (h1 ^ q[i + 1]) * P; h2 = (h2 ^ q[i + 2]) * P; h3 = (h3 ^ q[i + 3]) * P;
    }
    for (; i < words; ++i) h0 = (h0 ^ q[i]) * P;
    for (size_t j = words * 8; j < n; ++j) h0 = (h0 ^ p[j]) * P;
    h0 = (h0 ^ (h1 >> 29) ^ (h1 << 35)) * P; h0 = (h0 ^ (h2 >> 31) ^ (h2 << 33)) * P; h0 = (h0 ^ (h3 >> 27) ^ (h3 << 37)) * P;
    return h0 ^ (h0 >> 32);
}

static unsigned ste_nz_words(const uint8_t *p, size_t n)
{
    unsigned nz = 0; size_t i;
    if (!n || n > (64u << 20)) return 0;
    for (i = 0; i + 4 <= n; i += 4) if (*(const uint32_t *)(p + i)) nz++;
    return nz;
}

static void tex_geometry(const TexStage *t, uint32_t *w, uint32_t *h, uint32_t *pitch, size_t *bytes)
{
    uint32_t color = (t->format >> 8) & 0xFF;
    if (tex_is_linear(color)) {
        *w = t->image_rect >> 16;
        *h = t->image_rect & 0xFFFF;
        *pitch = t->control1 >> 16;
        *bytes = (size_t)*pitch * *h;
    } else {
        *w = 1u << ((t->format >> 20) & 0xF);
        *h = 1u << ((t->format >> 24) & 0xF);
        if (tex_is_dxt(color)) {
            uint32_t bw = (*w + 3) / 4, bh = (*h + 3) / 4;
            *pitch = bw * (color == 0x0C ? 8 : 16);
            *bytes = (size_t)*pitch * bh;
        } else {
            *pitch = *w * tex_bpp(color);
            *bytes = (size_t)*pitch * *h;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Render targets                                                      */
/* ------------------------------------------------------------------ */
/* SET_SURFACE_COLOR_OFFSET picks the colour surface. Surfaces current when
 * a frame ends are display buffers (front/back, alternating); any other
 * surface is an offscreen target the title will sample later as a texture
 * (bloom, the menu's render-to-texture passes), rendered via d3d8_rt_*. */
extern void d3d8_rt_select(uint32_t key, UINT w, UINT h);
extern IDirect3DTexture8 *d3d8_rt_texture(uint32_t key);
extern void d3d8_rt_present(uint32_t key);
#define M_SURFACE_COLOR_OFFSET 0x0210
static uint32_t g_surf_color;          /* physical address, current */
static uint32_t g_display[2];          /* recent display surfaces */
static uint32_t g_target_key;          /* 0 = back buffer */
/* The target is bound lazily, just before the first draw or clear that needs it. A title's SetRenderTarget
 * writes the clip rectangle (0x200/0x204) BEFORE the colour offset (0x210); applying the clip at once resized
 * the still-current offscreen surface (e.g. the 640x480 scene target) to the next pass's size and cleared it,
 * so the scene was gone by the time the composite pass sampled it. */
static int g_target_dirty;

/* c269 probe (STE_RTPROBE=1): small offscreen targets (clip width <= 128) and the draws that render into / sample them. Logging only. */
static uint32_t g_rtp_small[16]; static int g_rtp_nsmall; static uint32_t g_rtp_cur_w, g_rtp_cur_h;
static int g_rtp_on(void) { static int on = -1; if (on < 0) { const char *q = getenv("STE_RTPROBE"); on = q && q[0] == '1'; } return on; }
static int rtp_is_small(uint32_t key) { int i; for (i = 0; i < g_rtp_nsmall; ++i) if (g_rtp_small[i] == key) return 1; return 0; }
static void rtp_note_select(uint32_t key)
{
    static int nl; uint32_t w, h; int j;
    g_rtp_cur_w = g_rtp_cur_h = 0;
    if (!g_rtp_on() || !key) return;
    w = K.clip_h >> 16; h = K.clip_v >> 16; g_rtp_cur_w = w; g_rtp_cur_h = h;
    if (w <= 128 && h <= 128) {
        for (j = 0; j < g_rtp_nsmall; ++j) if (g_rtp_small[j] == key) break;
        if (j == g_rtp_nsmall && g_rtp_nsmall < 16) g_rtp_small[g_rtp_nsmall++] = key;
        { extern unsigned g_kelvin_fsq_count; g_kelvin_fsq_count++; }   /* the exe's STE_SHOT_FLASH shoots the frame when this moves */
        if (nl++ < 400) fprintf(stderr, "[RTSMALL] kf=%u select surface=%08X clip=%ux%u\n", K.frame, key, w, h);
    }
}

static void apply_target(void)
{
    if (!g_target_dirty) return;
    g_target_dirty = 0;
    d3d8_rt_select(g_target_key, K.clip_h >> 16, K.clip_v >> 16);
}

static int surf_is_display(uint32_t a)
{
    return !g_display[0] || a == g_display[0] || a == g_display[1];
}

static void select_target(void)
{
    uint32_t key = kenv("KELVIN_ALL_RT") ? g_surf_color
        : ((g_surf_color && !surf_is_display(g_surf_color)) ? g_surf_color : 0);
    if (kenv("KELVIN_NO_RT")) key = 0;
    if (key == g_target_key) return;
    {
        static int n, nn, seen_n; static uint32_t seen[128]; uint32_t sk = (K.clip_h >> 16) * 8191u + (K.clip_v >> 16) + (key ? 1u << 31 : 0u); int j, fresh = 1;
        for (j = 0; j < seen_n; ++j) if (seen[j] == sk) { fresh = 0; break; }
        if (fresh && seen_n < 128) { seen[seen_n++] = sk; }   /* c267: every new (target size, offscreen/backbuffer) combination is logged, however late (the first 40 switches as before) */
        if (n++ < 40 || (fresh && nn++ < 128))
            fprintf(stderr, "[RT] frame=%u surface=%08X -> %s (display %08X/%08X) clip=%ux%u\n",
                    K.frame, g_surf_color, key ? "offscreen" : "backbuffer",
                    g_display[0], g_display[1], K.clip_h >> 16, K.clip_v >> 16);
    }
    g_target_key = key;
    g_target_dirty = 1;
    rtp_note_select(key);
}

/* Decode one w x h image at src into A8R8G8B8 rows (dst, dst_pitch bytes). */
static void decode_image(uint32_t color, const uint8_t *src, uint32_t w, uint32_t h, uint32_t pitch,
                         uint32_t palette_addr, const uint32_t *palette, void *dst, size_t dst_pitch)
{

    if (tex_is_dxt(color)) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bsize = color == 0x0C ? 8 : 16;
        uint32_t block[16];
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                decode_dxt_block(color, src + (by * bw + bx) * bsize, block);
                for (uint32_t py = 0; py < 4 && by * 4 + py < h; ++py) {
                    uint32_t *row = (uint32_t *)((uint8_t *)dst + (size_t)(by * 4 + py) * dst_pitch);
                    for (uint32_t px = 0; px < 4 && bx * 4 + px < w; ++px)
                        row[bx * 4 + px] = block[py * 4 + px];
                }
            }
    } else if (color == 0x24 || color == 0x25) {
        /* YUY2 (CR8YB8CB8YA8) / UYVY (YB8CR8YA8CB8), two pixels per dword */
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t *s = src + (size_t)y * pitch;
            uint32_t *row = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_pitch);
            for (uint32_t x = 0; x + 1 < w; x += 2, s += 4) {
                int y0, y1, u, v;
                if (color == 0x24) { y0 = s[0]; u = s[1]; y1 = s[2]; v = s[3]; }
                else { u = s[0]; y0 = s[1]; v = s[2]; y1 = s[3]; }
                row[x] = yuv_to_argb(y0, u, v);
                row[x + 1] = yuv_to_argb(y1, u, v);
            }
        }
    } else if (tex_is_linear(color)) {
        uint32_t bpp = tex_bpp(color);
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t *s = src + (size_t)y * pitch;
            uint32_t *row = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_pitch);
            for (uint32_t x = 0; x < w; ++x) row[x] = decode_texel(color, s + x * bpp, palette_addr ? palette : NULL);
        }
    } else {
        uint32_t bpp = tex_bpp(color), mx, my;
        swizzle_masks(w, h, &mx, &my);
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t sy = spread(y, my);
            uint32_t *row = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_pitch);
            for (uint32_t x = 0; x < w; ++x)
                row[x] = decode_texel(color, src + (size_t)(spread(x, mx) | sy) * bpp,
                                      palette_addr ? palette : NULL);
        }
    }
}

/* Cube textures (SET_TEXTURE_FORMAT bit 2): six faces stored back to back,
 * each a full mip chain padded to 128 bytes (xemu NV2A_CUBEFACE_ALIGNMENT),
 * order +X -X +Y -Y +Z -Z as in D3D11. Only level 0 is uploaded; contents
 * are re-hashed at most once per frame per stage. */
static void bind_cube(int stage)
{
    const TexStage *t = &K.tex[stage];
    uint32_t color = (t->format >> 8) & 0xFF;
    uint32_t w, h, pitch, levels;
    size_t bytes, face = 0;
    static struct { uint32_t offset, format, frame; uint64_t key; } seen[4];
    if (!(t->control0 & (1u << 30)) || !t->offset || !(t->format & 4u) ||
        tex_is_linear(color) || (!tex_is_dxt(color) && !tex_bpp(color))) {
        d3d8_nv2a_psh_set_cube(stage, NULL, 0, 0);
        return;
    }
    tex_geometry(t, &w, &h, &pitch, &bytes);
    if (w != h || !w || w > 2048) { d3d8_nv2a_psh_set_cube(stage, NULL, 0, 0); return; }
    if (seen[stage].key && seen[stage].offset == t->offset && seen[stage].format == t->format &&
        seen[stage].frame == K.frame) {
        d3d8_nv2a_psh_set_cube(stage, NULL, 0, seen[stage].key);
        return;
    }
    levels = (t->format >> 16) & 0xF;
    if (!levels) levels = 1;
    for (uint32_t l = 0; l < levels; ++l) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        if (tex_is_dxt(color)) face += (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * (color == 0x0C ? 8 : 16);
        else face += (size_t)lw * lh * tex_bpp(color);
    }
    face = (face + 127) & ~(size_t)127;
    {
        const uint8_t *src = guest_ptr(t->offset);
        uint32_t palette_addr = color == 0x0B ? (t->palette & ~0x3Fu) : 0;
        uint32_t palette[256];
        uint64_t key = (hash_bytes(src, face * 6) ^ ((uint64_t)t->format << 32) ^ t->offset) | 1u;
        uint32_t *pix;
        seen[stage].offset = t->offset; seen[stage].format = t->format;
        seen[stage].frame = K.frame; seen[stage].key = key;
        if (d3d8_nv2a_psh_set_cube(stage, NULL, 0, key)) return;    /* cached */
        pix = (uint32_t *)malloc((size_t)w * h * 4 * 6);
        if (!pix) return;
        if (palette_addr) memcpy(palette, guest_ptr(palette_addr), sizeof(palette));
        for (int f = 0; f < 6; ++f)
            decode_image(color, src + face * f, w, h, pitch, palette_addr, palette,
                         pix + (size_t)f * w * h, (size_t)w * 4);
        if (kenv("KELVIN_DUMP_CUBE") &&
            (!strcmp(kenv("KELVIN_DUMP_CUBE"), "all") ||
             strtoul(kenv("KELVIN_DUMP_CUBE"), NULL, 0) == t->offset)) {
            static uint32_t dumped_frame = ~0u;
            static uint32_t dumped_offset;
            if (dumped_frame != K.frame || dumped_offset != t->offset) {
                dumped_frame = K.frame;
                dumped_offset = t->offset;
                for (int cf = 0; cf < 6; ++cf) {
                    size_t raw_nonzero = 0, decoded_nonblack = 0;
                    const uint8_t *raw = src + face * cf;
                    const uint32_t *decoded = pix + (size_t)cf * w * h;
                    char path[112];
                    FILE *df;
                    for (size_t i = 0; i < face; ++i) raw_nonzero += raw[i] != 0;
                    for (size_t i = 0; i < (size_t)w * h; ++i)
                        decoded_nonblack += (decoded[i] & 0x00FFFFFFu) != 0;
                    fprintf(stderr,
                            "[CUBE] frame=%u stage=%d off=%08X fmt=%08X face=%d "
                            "stride=%zu raw_nonzero=%zu decoded_nonblack=%zu/%zu\n",
                            K.frame, stage, t->offset, t->format, cf, face,
                            raw_nonzero, decoded_nonblack, (size_t)w * h);
                    sprintf(path, "build-release\\cube_%04u_%08X_f%d_%ux%u.bmp",
                            K.frame, t->offset, cf, w, h);
                    df = fopen(path, "wb");
                    if (df) {
                        BITMAPFILEHEADER fh = {0};
                        BITMAPINFOHEADER ih = {0};
                        fh.bfType = 0x4D42;
                        fh.bfOffBits = sizeof(fh) + sizeof(ih);
                        fh.bfSize = fh.bfOffBits + w * h * 4;
                        ih.biSize = sizeof(ih); ih.biWidth = (LONG)w; ih.biHeight = -(LONG)h;
                        ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
                        fwrite(&fh, sizeof(fh), 1, df);
                        fwrite(&ih, sizeof(ih), 1, df);
                        fwrite(decoded, (size_t)w * h * 4, 1, df);
                        fclose(df);
                    }
                }
            }
        }
        d3d8_nv2a_psh_set_cube(stage, pix, w, key);
        free(pix);
    }
}

/* c128 texture packs: mip chains for uploaded textures (packs, anisotropy or mip_bias on) and HD replacement uploads. */
extern void d3d8_set_sampler_quality(unsigned aniso, float bias, int mips);
extern void d3d8_sampler_quality_world(int on);
static uint32_t tex_mip_count(uint32_t w, uint32_t h) { uint32_t m = w > h ? w : h, l = 1; while (m > 1) { m >>= 1; l++; } return l > 12 ? 12 : l; }
/* fill levels 1.. of a texture created with Levels=0 from its BGRA8 level 0 (src, row pitch in bytes); level 0 must already be unlocked */
static void tex_build_mips(IDirect3DTexture8 *tex, uint32_t w, uint32_t h, const uint8_t *src, uint32_t src_pitch)
{
    uint32_t levels = tex_mip_count(w, h), l, cw = w, ch = h, y; uint32_t *cur, *nxt;
    if (levels < 2) return;
    cur = (uint32_t *)malloc((size_t)w * h * 4); nxt = (uint32_t *)malloc((size_t)((w + 1) / 2) * ((h + 1) / 2) * 4);
    if (!cur || !nxt) { free(cur); free(nxt); return; }
    for (y = 0; y < h; ++y) memcpy(cur + (size_t)y * w, src + (size_t)y * src_pitch, (size_t)w * 4);
    for (l = 1; l < levels; ++l) {
        uint32_t nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1; D3DLOCKED_RECT lr; uint32_t *t2;
        texpack_downsample(cur, cw, ch, nxt);
        if (tex->lpVtbl->LockRect(tex, l, &lr, NULL, 0) == 0 && lr.pBits) {
            for (y = 0; y < nh; ++y) memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, nxt + (size_t)y * nw, (size_t)nw * 4);
            tex->lpVtbl->UnlockRect(tex, l);
        }
        t2 = cur; cur = nxt; nxt = t2; cw = nw; ch = nh;
    }
    free(cur); free(nxt);
}

/* c150 probe: STE_TEXLOG=1 logs why get_texture returned NULL (first 40 per reason) and every NEW / CHG(content changed) / periodic SAME lookup of a large texture
 * (>= 100000 px: movie frames, loading and title pictures), max 8000 lines. Logging only. */
static int texlog_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("STE_TEXLOG"); v = (e && *e && *e != '0'); } return v; }
static void texlog_null(const char *why, int stage, uint32_t off, uint32_t fmt)
{
    static unsigned n[8]; static const char *names[8]; int i;
    if (!texlog_on()) return;
    for (i = 0; i < 8 && names[i] && names[i] != why; ++i) ;
    if (i == 8) return;
    names[i] = why;
    if (n[i]++ < 40) fprintf(stderr, "[TEXLOG] NULL %s frame=%u stage=%d off=%08X fmt=%08X\n", why, K.frame, stage, off, fmt);
}
static void texlog_big(const char *what, int stage, const TexStage *t, uint32_t w, uint32_t h, uint32_t pitch, uint64_t hash, int slot_idx)
{
    static unsigned lines;
    if (!texlog_on() || (uint64_t)w * h < 100000u || lines >= 8000u) return;
    if (!strcmp(what, "SAME") && (K.frame % 600u)) return;
    lines++;
    fprintf(stderr, "[TEXLOG] %s frame=%u stage=%d off=%08X fmt=%08X %ux%u pitch=%u hash=%016llX slot=%d\n", what, K.frame, stage, t->offset, t->format, w, h, pitch, (unsigned long long)hash, slot_idx);
}

/* c162 probe (observation only): STE_TEXDUMP_OFF=<hex texture offset> STE_TEXDUMP_DIR=<dir> writes the host texture that get_texture hands to the draw
 * (the decoded A8R8G8B8 copy, not guest memory) as BMP every 60th lookup, max 60 files, so what the draw samples can be compared with guest memory. */
static IDirect3DTexture8 *get_texture_impl(int stage);
static IDirect3DTexture8 *get_texture(int stage)
{
    IDirect3DTexture8 *r = get_texture_impl(stage);
    static int init, nfiles; static uint32_t want; static const char *dir; static unsigned calls;
    if (!init) { const char *o = getenv("STE_TEXDUMP_OFF"); init = 1; dir = getenv("STE_TEXDUMP_DIR"); want = (o && *o) ? (uint32_t)strtoul(o, NULL, 16) : 0; }
    if (r && want && dir && K.tex[stage].offset == want && nfiles < 90 && (calls++ % 45) == 0) {
        D3DLOCKED_RECT lr; D3DSURFACE_DESC dd;
        if (r->lpVtbl->GetLevelDesc(r, 0, &dd) == 0 && dd.Width == 640 && r->lpVtbl->LockRect(r, 0, &lr, NULL, D3DLOCK_READONLY) == 0 && lr.pBits) {
            char path[512]; FILE *f; BITMAPFILEHEADER fh = {0}; BITMAPINFOHEADER ih = {0}; uint32_t y;
            snprintf(path, sizeof path, "%s\\hosttex_f%u_%08X_%ux%u.bmp", dir, K.frame, want, dd.Width, dd.Height);
            f = fopen(path, "wb");
            if (f) {
                fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + dd.Width * dd.Height * 4;
                ih.biSize = sizeof(ih); ih.biWidth = (LONG)dd.Width; ih.biHeight = -(LONG)dd.Height; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
                fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
                for (y = 0; y < dd.Height; ++y) fwrite((const uint8_t *)lr.pBits + (size_t)y * lr.Pitch, 4, dd.Width, f);
                fclose(f); nfiles++;
            }
            r->lpVtbl->UnlockRect(r, 0);
        }
    }
    return r;
}
static IDirect3DTexture8 *get_texture_impl(int stage)
{
    const TexStage *t = &K.tex[stage];
    uint32_t color = (t->format >> 8) & 0xFF;
    uint32_t w, h, pitch;
    size_t bytes;
    IDirect3DDevice8 *dev = xbox_GetD3DDevice();

    if (!(t->control0 & (1u << 30)) || !t->offset || !dev) {
        if ((t->control0 & (1u << 30)) && texlog_on()) texlog_null("off0", stage, t->offset, t->format);
        return NULL;
    }
    { static int tp_inited; if (!tp_inited) { tp_inited = 1; texpack_init(); d3d8_set_sampler_quality(texpack_aniso(), texpack_mip_bias(), texpack_want_mips()); } }
    texpack_log_stats(K.frame);
    {
        IDirect3DTexture8 *rt = d3d8_rt_texture(t->offset & 0x03FFFFFFu);
        if (rt) {
            {   /* c164 probe (observation only): log each distinct (offset, format, stage) texture lookup that is served by an offscreen render target (STE_RTHIT_LOG=1), with the frame of first/latest hit */
                static struct { uint32_t off, fmt; int stage; uint32_t first, last; unsigned n; } tab[128]; static int ntab, init, on; int i;
                if (!init) { const char *e = getenv("STE_RTHIT_LOG"); init = 1; on = (e && *e && *e != '0'); }
                if (on) {
                    for (i = 0; i < ntab; ++i) if (tab[i].off == t->offset && tab[i].fmt == t->format && tab[i].stage == stage) break;
                    if (i == ntab && ntab < 128) { tab[ntab].off = t->offset; tab[ntab].fmt = t->format; tab[ntab].stage = stage; tab[ntab].first = K.frame; tab[ntab].n = 0; ++ntab;
                        fprintf(stderr, "[RTHIT] NEW frame=%u stage=%d off=%08X fmt=%08X color=%02X\n", K.frame, stage, t->offset, t->format, color); }
                    if (i < ntab) { tab[i].last = K.frame; if ((++tab[i].n % 5000) == 0) fprintf(stderr, "[RTHIT] frame=%u off=%08X fmt=%08X hits=%u (first frame %u)\n", K.frame, t->offset, t->format, tab[i].n, tab[i].first); }
                }
            }            if (kenv("KELVIN_LOG_RTSAMPLE")) {   /* KELVIN_LOG_RTSAMPLE=1: the first 20, then every 250th, draw that samples an offscreen render target (max 400 lines), with the state they draw under */
                static unsigned n, shown;
                n++;
                if ((n <= 20 || (n % 250) == 0) && shown++ < 400)
                    fprintf(stderr, "[RT-SAMPLE] frame=%u stage=%d off=%08X fmt=%08X target=%08X blend=%u %X/%X eq=%X depth=%u/%X zmask=%u atest=%u/%X/%02X stages=%08X rgb_in0=%08X rgb_out0=%08X a_in0=%08X fin=%08X/%08X f0=%08X,%08X f1=%08X,%08X\n",
                            K.frame, stage, t->offset, t->format, g_target_key, K.blend_enable, K.sfactor, K.dfactor, K.blend_eq, K.depth_test, K.depth_func, K.depth_mask,
                            K.alpha_test, K.alpha_func, K.alpha_ref, K.psh.shader_stage_program, K.psh.rgb_in[0], K.psh.rgb_out[0], K.psh.alpha_in[0], K.psh.final0, K.psh.final1,
                            K.psh.factor0[0], K.psh.factor0[1], K.psh.factor1[0], K.psh.factor1[1]);
            }
            return rt;
        }
    }
    if (!tex_is_dxt(color) && !tex_bpp(color)) {
        K.stats.unsupported_texture_formats++;
        texlog_null("unsup", stage, t->offset, t->format);
        return NULL;
    }
    tex_geometry(t, &w, &h, &pitch, &bytes);
    if (color == 0x24 || color == 0x25) {
        static uint32_t last_yuv_frame = ~0u;
        if (last_yuv_frame != K.frame && K.frame < 20) {
            last_yuv_frame = K.frame;
            fprintf(stderr,
                    "[TEX-YUV] frame=%u stage=%d off=%08X fmt=%08X ctl0=%08X ctl1=%08X "
                    "rect=%08X filter=%08X address=%08X -> %ux%u pitch=%u bytes=%zu\n",
                    K.frame, stage, t->offset, t->format, t->control0, t->control1,
                    t->image_rect, t->filter, t->address, w, h, pitch, bytes);
        }
    }
    if (!w || !h || w > 4096 || h > 4096 || bytes > 64u * 1024 * 1024) {
        texlog_null("size", stage, t->offset, t->format);
        return NULL;
    }

    const uint8_t *src = guest_ptr(t->offset);
    uint32_t palette_addr = color == 0x0B ? (t->palette & ~0x3Fu) : 0;

    TexCacheEntry *slot = NULL, *victim = &g_tex_cache[0];
    /* c98: one-candidate hash index in front of the linear scan (the scan over 1024 entries per stage per draw was ~5% of the render thread) */
    uint32_t hk = ((t->offset * 0x9E3779B1u) ^ (t->format * 0x85EBCA6Bu) ^ (w * 0xC2B2AE35u) ^ (h * 0x27D4EB2Fu) ^ (pitch * 0x165667B1u) ^ palette_addr) >> 19;
    {   int c = g_tex_idx[hk];
        if (c >= 0) {
            TexCacheEntry *e = &g_tex_cache[c];
            if (e->texture && e->key_offset == t->offset && e->key_format == t->format &&
                e->key_w == w && e->key_h == h && e->key_pitch == pitch && e->key_palette == palette_addr) slot = e;
        }
    }
    for (int i = 0; !slot && i < TEX_CACHE_SIZE; ++i) {
        TexCacheEntry *e = &g_tex_cache[i];
        if (e->texture && e->key_offset == t->offset && e->key_format == t->format &&
            e->key_w == w && e->key_h == h && e->key_pitch == pitch &&
            e->key_palette == palette_addr) { slot = e; g_tex_idx[hk] = (int16_t)i; break; }
        if (!e->texture || e->last_frame < victim->last_frame) victim = e;
    }
    uint64_t hash;
    if (slot && slot->checked_frame == K.frame) {
        slot->last_frame = K.frame;
        return slot->texture;
    }
    if (g_xbox_uaf_on) xbox_AsyncUafCheck(t->offset, (uint32_t)bytes, "texture");
    hash = hash_bytes(src, bytes) ^ (palette_addr ? hash_bytes(guest_ptr(palette_addr), 1024) : 0);
    if (slot && slot->hash == hash) {
        slot->checked_frame = slot->last_frame = K.frame;
        texlog_big("SAME", stage, t, w, h, pitch, hash, (int)(slot - g_tex_cache));
        return slot->texture;
    }
    texlog_big(slot ? "CHG" : "NEW", stage, t, w, h, pitch, hash, slot ? (int)(slot - g_tex_cache) : -1);
    if (!slot) {
        /* Never release a texture already handed out this frame: submit()
         * still holds the pointers for its other stages, and earlier draws'
         * wrappers are still referenced (a demo scene with >256 textures
         * evicted one mid-draw and SetTexture read freed memory). */
        if (victim->texture && victim->last_frame == K.frame) {
            static int warned;
            if (!warned++) fprintf(stderr, "[KELVIN] texture cache full this frame (%d)\n", TEX_CACHE_SIZE);
            texlog_null("full", stage, t->offset, t->format);
            return NULL;
        }
        slot = victim;
        if (slot->texture) { kelvin_tex_forget(slot->texture); slot->texture->lpVtbl->Release(slot->texture); slot->texture = NULL; }
        if (dev->lpVtbl->CreateTexture(dev, w, h, texpack_want_mips() ? 0 : 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_MANAGED,
                                       &slot->texture) != 0 || !slot->texture) {
            slot->texture = NULL;
            texlog_null("create", stage, t->offset, t->format);
            return NULL;
        }
        slot->repl = 0;
        slot->key_offset = t->offset; slot->key_format = t->format;
        slot->key_w = w; slot->key_h = h; slot->key_pitch = pitch; slot->key_palette = palette_addr;
        g_tex_idx[hk] = (int16_t)(slot - g_tex_cache);
    }

    int tp_fresh = (slot->last_frame != K.frame);   /* c128: replacing the texture object is only safe if it was not handed out to an earlier draw this frame */
    if (slot->repl) {   /* content changed under an HD replacement: go back to an original-size texture and re-decode into it */
        if (!tp_fresh) return slot->texture;
        kelvin_tex_forget(slot->texture); slot->texture->lpVtbl->Release(slot->texture); slot->texture = NULL; slot->repl = 0;
        if (dev->lpVtbl->CreateTexture(dev, w, h, texpack_want_mips() ? 0 : 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_MANAGED, &slot->texture) != 0 || !slot->texture) { slot->texture = NULL; return NULL; }
    }
    D3DLOCKED_RECT lr;
    if (slot->texture->lpVtbl->LockRect(slot->texture, 0, &lr, NULL, 0) != 0 || !lr.pBits) {
        texlog_null("lock", stage, t->offset, t->format);
        return NULL;
    }

    uint32_t palette[256];
    if (palette_addr) memcpy(palette, guest_ptr(palette_addr), sizeof(palette));
    decode_image(color, src, w, h, pitch, palette_addr, palette, lr.pBits, (size_t)lr.Pitch);
    if (color == 0x0E && w == 16 && h == 16) {
        /* c151: GFC_Globe_Halo (the glow ring around the city-select globe; content key 0C7F6C23EB1D7F04). Its DXT3 alpha is a 4-bit ramp (0, 17, 34 ... 170 per row, plus a dithered 17/0 row at the
         * outer edge). On a TV the original composite output blurred the 0 -> 17 step; at 1080p/4K the step and the dither read as a hard outer line and jagged speckles. The alpha is rebuilt at load
         * time as a smooth 8-bit ease-in ramp (170 * ((row - 1.5) / 12.5)^1.4: re-quantised to 4 bits it gives exactly the original steps, same shape and peak, 0 at the outer edge). No texture is shipped.
         * STE_GLOW_SMOOTH=0 restores the original alpha (debugging only). */
        static int en = -1;
        if (en < 0) { const char *e = getenv("STE_GLOW_SMOOTH"); en = !(e && e[0] == '0'); }
        if (en && texpack_key(hash, t->format, w, h) == 0x0C7F6C23EB1D7F04ull) {
            static int logged;
            for (uint32_t y = 0; y < 15; ++y) {
                double v = y > 1.5 ? pow((y - 1.5) / 12.5, 1.4) * 170.0 : 0.0;
                uint8_t a8 = (uint8_t)(v + 0.5);
                for (uint32_t x = 0; x < 16; ++x) ((uint8_t *)lr.pBits + (size_t)y * lr.Pitch)[x * 4 + 3] = a8;
            }
            if (!logged++) { fprintf(stderr, "[GLOW] GFC_Globe_Halo alpha ramp rebuilt as smooth 8-bit (off with STE_GLOW_SMOOTH=0)\n"); fflush(stderr); }
        }
    }
    {
        /* KELVIN_DUMP_TEX=<frame>: write each texture decoded in that frame. */
        static long dump_frame = -2;
        if (dump_frame == -2) {
            const char *e = kenv("KELVIN_DUMP_TEX");
            dump_frame = e ? strtol(e, NULL, 0) : -1;
        }
        static int dumped_any;
        /* KELVIN_DUMP_TEX=all dumps the first 64 decodes of any frame. */
        if (dump_frame == -2 + 0 && 0) dump_frame = -1;
        if ((long)K.frame == dump_frame ||
            (kenv("KELVIN_DUMP_TEX") && !strcmp(kenv("KELVIN_DUMP_TEX"), "all") &&
             dumped_any++ < 64)) {
            char path[96];
            FILE *f;
            sprintf(path, "build-release\\tex_%04u_%08X_%02X_%ux%u.bmp", K.frame,
                    t->offset, color, w, h);
            f = fopen(path, "wb");
            if (f) {
                BITMAPFILEHEADER fh = {0};
                BITMAPINFOHEADER ih = {0};
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + w * h * 4;
                ih.biSize = sizeof(ih); ih.biWidth = (LONG)w; ih.biHeight = -(LONG)h;
                ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                for (uint32_t y = 0; y < h; ++y)
                    fwrite((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, w * 4, 1, f);
                fclose(f);
                fprintf(stderr, "[KELVIN] dumped %s\n", path);
            }
        }
    }
    slot->texture->lpVtbl->UnlockRect(slot->texture, 0);   /* lr.pBits still points at the wrapper's system-memory copy of level 0 */
    slot->hash = hash;
    if (texpack_want_mips()) tex_build_mips(slot->texture, w, h, (const uint8_t *)lr.pBits, (uint32_t)lr.Pitch);
    if (texpack_dump_on() || texpack_packs_active()) {
        uint64_t key = texpack_key(hash, t->format, w, h);
        /* c178: movie frames (YUV 0x24/0x25 surfaces) are not dumped (2277 of the 2644 Tokyo keys were movie frames); STE_TEXDUMP_MOVIES=1 brings them back */
        if (texpack_dump_on() && (((t->format >> 8) & 0xFFu) != 0x24u && ((t->format >> 8) & 0xFFu) != 0x25u || getenv("STE_TEXDUMP_MOVIES")))
            texpack_dump(key, w, h, t->format, K.frame, t->offset, (const uint8_t *)lr.pBits, (uint32_t)lr.Pitch);
        if (texpack_packs_active() && tp_fresh) {
            uint32_t rw = 0, rh = 0; LARGE_INTEGER up_t0, up_t1, up_f; uint8_t *img; QueryPerformanceCounter(&up_t0); img = texpack_load(key, &rw, &rh);
            if (img) {
                IDirect3DTexture8 *nt = NULL; D3DLOCKED_RECT l2;
                if (dev->lpVtbl->CreateTexture(dev, rw, rh, 0, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_MANAGED, &nt) == 0 && nt) {
                    if (nt->lpVtbl->LockRect(nt, 0, &l2, NULL, 0) == 0 && l2.pBits) {
                        for (uint32_t yy = 0; yy < rh; ++yy) memcpy((uint8_t *)l2.pBits + (size_t)yy * l2.Pitch, img + (size_t)yy * rw * 4, (size_t)rw * 4);
                        nt->lpVtbl->UnlockRect(nt, 0);
                        tex_build_mips(nt, rw, rh, img, rw * 4);
                        kelvin_tex_forget(slot->texture); slot->texture->lpVtbl->Release(slot->texture);
                        slot->texture = nt; slot->repl = 1; texpack_note_replaced();
                    } else nt->lpVtbl->Release(nt);
                }
                texpack_free(img);
                {   /* c178 probe (STE_PACKTIME=1): cost of each replacement (PNG read + decode + mips + create) and the frame it happened in, first 120 lines */
                    static int pt_init, pt_on; static unsigned pt_n; double ms;
                    if (!pt_init) { const char *e = getenv("STE_PACKTIME"); pt_init = 1; pt_on = (e && *e && *e != '0'); }
                    QueryPerformanceCounter(&up_t1); QueryPerformanceFrequency(&up_f); ms = (double)(up_t1.QuadPart - up_t0.QuadPart) * 1000.0 / (double)up_f.QuadPart;
                    if (pt_on && pt_n < 120) { pt_n++; fprintf(stderr, "[PACKTIME] frame=%u upload %ux%u -> %ux%u %.2f ms\n", K.frame, w, h, rw, rh, ms); }
                }
            }
        }
    }
    slot->checked_frame = slot->last_frame = K.frame;
    K.stats.textures_decoded++;
    return slot->texture;
}

/* ------------------------------------------------------------------ */
/* Raster state                                                        */
/* ------------------------------------------------------------------ */
static DWORD map_blend(uint32_t f)
{
    switch (f) {
    case 0x0000: return D3DBLEND_ZERO;
    case 0x0001: return D3DBLEND_ONE;
    case 0x0300: return D3DBLEND_SRCCOLOR;
    case 0x0301: return D3DBLEND_INVSRCCOLOR;
    case 0x0302: return D3DBLEND_SRCALPHA;
    case 0x0303: return D3DBLEND_INVSRCALPHA;
    case 0x0304: return D3DBLEND_DESTALPHA;
    case 0x0305: return D3DBLEND_INVDESTALPHA;
    case 0x0306: return D3DBLEND_DESTCOLOR;
    case 0x0307: return D3DBLEND_INVDESTCOLOR;
    case 0x0308: return D3DBLEND_SRCALPHASAT;
    default: return D3DBLEND_ONE;
    }
}

static DWORD map_cmp(uint32_t f) { return (f >= 0x200 && f <= 0x207) ? (f - 0x200 + 1) : D3DCMP_ALWAYS; }

/* c79: apply_state()/SetTexture/SetVertexShader are skipped when nothing they depend on changed since the previous draw (the clear-quad
 * path, which sets its own states, invalidates the cache).  KELVIN_NO_STATE_CACHE=1 restores the per-draw calls. */
/* c138: NV2A face culling. The title draws some geometry double-sided on purpose and relies on the hardware cull (the front-end globe halo: 64 quads drawn once per winding with mirrored texture coordinates,
 * the clockwise copy is culled; without culling both copies blend over each other and the glow is a symmetric V instead of a ramp that peaks at the globe rim).
 * STE_KELVIN_CULL = 0 off (the old behaviour: everything two-sided) | 1 only while the front-end is active (c138 default) | 2 everywhere (fights too; default since c143).
 * Screen-space winding as in D3D with y down: NV2A front face CCW + cull BACK removes clockwise triangles = D3DCULL_CW. */
static int kelvin_cull_mode(void)
{
    static int mode = -1; uint32_t face, ff; int cull_ccw;
    if (mode < 0) { const char *e = getenv("STE_KELVIN_CULL"); mode = e && *e ? atoi(e) : 2; }   /* c143: default 2 (the NV2A culled everywhere and the title relies on it) */
    if (mode == 3) { /* c144 probe: STE_KELVIN_CULL=3 alternates mode 1 / mode 2 every frame so GPU cost can be compared inside one run (see kelvin_cull_alt_tag) */ if (!K.cull_enable) return D3DCULL_NONE; if (!(K.frame & 1u) && !d3d8_front_end_active()) return D3DCULL_NONE; }
    else {
    if (!mode || !K.cull_enable) return D3DCULL_NONE;
    if (mode == 1 && !d3d8_front_end_active()) return D3DCULL_NONE; }
    face = K.cull_face ? K.cull_face : 0x405u; ff = K.front_face ? K.front_face : 0x901u;
    if (face == 0x408u) return D3DCULL_NONE;   /* FRONT_AND_BACK: not emulated (would draw nothing) */
    cull_ccw = (face == 0x405u) ? (ff == 0x900u) : (ff == 0x901u);
    return cull_ccw ? D3DCULL_CCW : D3DCULL_CW;
}
/* c144: with STE_KELVIN_CULL=3 returns 1 (even frame = mode 1) or 2 (odd frame = mode 2) for the frame in progress, else 0 */
int kelvin_cull_alt_tag(void)
{
    static int alt = -1; if (alt < 0) { const char *e = getenv("STE_KELVIN_CULL"); alt = (e && atoi(e) == 3); }
    return alt ? ((K.frame & 1u) ? 2 : 1) : 0;
}
typedef struct { uint32_t v[11 + 4 + 1 + 1]; } AsKey;
static AsKey g_as_key; static int g_as_valid;
static IDirect3DTexture8 *g_set_tex[4]; static int g_fvf_set;
static void kelvin_tex_forget(IDirect3DTexture8 *t) { for (int i = 0; i < 4; ++i) if (g_set_tex[i] == t) g_set_tex[i] = NULL; }
static void kelvin_state_cache_invalidate(void) { g_as_valid = 0; g_fvf_set = 0; for (int i = 0; i < 4; ++i) g_set_tex[i] = NULL; }

static void apply_state(IDirect3DDevice8 *dev, int textured[4])
{
    static int cache_on = -1;
    if (cache_on < 0) cache_on = !kenv("KELVIN_NO_STATE_CACHE") && !kenv("KELVIN_SIMPLE");
    if (cache_on) {
        AsKey k; int last = -1;
        for (int s = 0; s < 4 && textured[s]; ++s) last = s;
        k.v[0] = K.depth_test; k.v[1] = K.depth_mask; k.v[2] = K.depth_func; k.v[3] = K.blend_enable; k.v[4] = K.sfactor; k.v[5] = K.dfactor;
        k.v[6] = K.blend_eq; k.v[7] = K.alpha_test; k.v[8] = K.alpha_func; k.v[9] = K.alpha_ref & 0xFF; k.v[10] = K.color_mask;
        for (int s = 0; s < 4; ++s) k.v[11 + s] = K.tex[s].address;
        k.v[15] = (uint32_t)(last + 1); k.v[16] = (uint32_t)kelvin_cull_mode();
        if (g_as_valid && !memcmp(&k, &g_as_key, sizeof(k))) return;
        g_as_key = k; g_as_valid = 1;
        for (int s = last + 1; s < 4; ++s) g_set_tex[s] = NULL;   /* the loop below unbinds these stages */
    }    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, (DWORD)kelvin_cull_mode());
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, K.depth_test ? TRUE : FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, K.depth_mask ? TRUE : FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZFUNC, map_cmp(K.depth_func));
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, K.blend_enable ? TRUE : FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, map_blend(K.sfactor));
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, map_blend(K.dfactor));
    {   /* NV097_SET_BLEND_EQUATION (GL values) -> D3DBLENDOP; it was stored but never applied, so every draw blended with ADD.
         * KELVIN_NO_BLENDOP=1 restores that. */
        DWORD bop = 1;                                  /* D3DBLENDOP_ADD */
        if (!kenv("KELVIN_NO_BLENDOP"))
            switch (K.blend_eq) {
            case 0x800A: bop = 2; break;                /* FUNC_SUBTRACT: src - dst */
            case 0x800B: bop = 3; break;                /* FUNC_REVERSE_SUBTRACT: dst - src */
            case 0x8007: bop = 4; break;                /* MIN */
            case 0x8008: bop = 5; break;                /* MAX */
            default: break;
            }
        dev->lpVtbl->SetRenderState(dev, D3DRS_BLENDOP, bop);
    }
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE, K.alpha_test ? TRUE : FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHAFUNC, map_cmp(K.alpha_func));
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHAREF, K.alpha_ref & 0xFF);
    {
        DWORD mask = 0;
        if (K.color_mask & 0x00010000u) mask |= 1; /* red */
        if (K.color_mask & 0x00000100u) mask |= 2; /* green */
        if (K.color_mask & 0x00000001u) mask |= 4; /* blue */
        if (K.color_mask & 0x01000000u) mask |= 8; /* alpha */
        dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE, mask);
    }
    if (kenv("KELVIN_SIMPLE")) {
        /* bisection aid: 1 = no depth, 2 = no blending, 4 = no alpha test */
        long bits = strtol(kenv("KELVIN_SIMPLE"), NULL, 0);
        if (bits & 1) dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
        if (bits & 2) dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
        if (bits & 4) dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    }

    int last = -1;
    for (int s = 0; s < 4; ++s) {
        if (!textured[s]) break;
        last = s;
    }
    for (int s = 0; s < 4; ++s) {
        if (s <= last) {
            DWORD addr_u = (K.tex[s].address & 0xF) == 1 ? D3DTADDRESS_WRAP :
                           (K.tex[s].address & 0xF) == 2 ? D3DTADDRESS_MIRROR : D3DTADDRESS_CLAMP;
            DWORD addr_v = ((K.tex[s].address >> 8) & 0xF) == 1 ? D3DTADDRESS_WRAP :
                           ((K.tex[s].address >> 8) & 0xF) == 2 ? D3DTADDRESS_MIRROR : D3DTADDRESS_CLAMP;
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_COLOROP, D3DTOP_MODULATE);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_COLORARG2, s ? D3DTA_CURRENT : D3DTA_DIFFUSE);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_ALPHAARG2, s ? D3DTA_CURRENT : D3DTA_DIFFUSE);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_TEXCOORDINDEX, s);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_ADDRESSU, addr_u);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_ADDRESSV, addr_v);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
        } else if (s == 0) {
            dev->lpVtbl->SetTexture(dev, 0, NULL);
            dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        } else {
            dev->lpVtbl->SetTexture(dev, s, NULL);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_COLOROP, D3DTOP_DISABLE);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */
/* Set by the title for frames it wants every draw logged ([DRAW] lines). */
int g_kelvin_log_draws;

static uint32_t pack_color(const float c[4])
{
    int r = (int)(c[0] * 255.0f + 0.5f), g = (int)(c[1] * 255.0f + 0.5f);
    int b = (int)(c[2] * 255.0f + 0.5f), a = (int)(c[3] * 255.0f + 0.5f);
    return argb(clamp8(a), clamp8(r), clamp8(g), clamp8(b));
}

static float nan_to_one(float f) { return f != f ? 1.0f : f; }

/* Fog (NV097_SET_FOG_ENABLE 0x2A4, FOG_MODE 0x29C, FOG_PARAMS 0x9C0..).  The vertex program's oFog is the fog coordinate;
 * the hardware turns it into a factor (1 = no fog, 0 = full fog colour) that the final combiner reads as fog.a.
 * KELVIN_NO_FOG=1 forces factor 1 (the pre-c56 behaviour). */
static uint32_t g_fog_en, g_fog_mode;
static float g_fog_p[3];
static float g_fog_p1_raw;   /* c267: the title's own slope, before fog_scale */
/* c267: draw_distance=unlimited asks for the fog to start beyond the farthest geometry (the exe calls this once, with 8). Original / numeric draw distance never call it: fog is then exactly the title's. */
void kelvin_fog_scale_default(float s)
{
    if (g_fog_user_scale || s < 0.25f || s > 16.0f || s == g_fog_scale) return;
    g_fog_scale = s;
    if (g_fog_p1_raw != 0.0f) g_fog_p[1] = g_fog_p1_raw / g_fog_scale;
    fprintf(stderr, "[FOG] draw_distance=unlimited: fog distances x%.2f\n", s);
}
static float fog_factor(float coord)
{
    float f, d = coord;
    if (!g_fog_en) return 1.0f;
    if (g_fog_mode >= 0x802) d = fabsf(d);
    switch (g_fog_mode) {
    case 0x800: case 0x802: f = expf(-(g_fog_p[0] + g_fog_p[1] * d)); break;                  /* EXP */
    case 0x801: case 0x803: { float e = g_fog_p[0] + g_fog_p[1] * d; f = expf(-e * e); break; } /* EXP2 */
    default:    f = g_fog_p[0] + g_fog_p[1] * d; break;                                         /* LINEAR (0x2601) / LINEAR_ABS */
    }
    if (f != f) return 1.0f;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

static void transform_vertex(const Vec4 v[16], OutVertex *o, const float tex_scale[4][2])
{
    Vec4 pos, d0, d1, t[4];
    float fogc = 0.0f;
    if (K.program_mode) {
        VshState s;
        vsh_run(v, &s);
        pos = s.o[0]; d0 = s.o[3]; d1 = s.o[4];
        fogc = (s.written & (1u << 5)) ? s.o[5].v[0] : 0.0f;
        /* Without combiner emulation a program that never writes oD0 would
         * modulate every texture by black; treat the colour as unused. */
        if (!(s.written & (1u << 3))) d0.v[0] = d0.v[1] = d0.v[2] = d0.v[3] = 1.0f;
        for (int i = 0; i < 4; ++i) t[i] = s.o[9 + i];
        /* oPos.xyz is already screen space divided by w (D3D's program
         * epilogue does the rcc/mad); w only carries 1/w for clipping and
         * perspective interpolation. xemu re-multiplies by w to build clip
         * coordinates; the D3D11 layer does the same from rhw. Vertices
         * behind the eye get huge x/y here and are clipped there. */
        o->x = pos.v[0];
        o->y = pos.v[1];
        o->z = K.clip_max != 0.0f ? pos.v[2] / K.clip_max : pos.v[2];
    } else {
        const Vec4 *p = &v[0];
        for (int r = 0; r < 4; ++r)
            pos.v[r] = p->v[0] * K.composite[r][0] + p->v[1] * K.composite[r][1] +
                       p->v[2] * K.composite[r][2] + p->v[3] * K.composite[r][3];
        float w = pos.v[3] == 0.0f ? 1.0f : pos.v[3];
        o->x = pos.v[0] / w + K.viewport_offset[0];
        o->y = pos.v[1] / w + K.viewport_offset[1];
        o->z = K.clip_max != 0.0f ? (pos.v[2] / w) / K.clip_max : pos.v[2] / w;
        d0 = v[3]; d1 = v[4];
        for (int i = 0; i < 4; ++i) t[i] = v[9 + i];
    }
    /* The XDK vertex-program epilogue (xsasm) is
     *     rcc r1.x, r12.w ; mul r12.xyz, r12, c[-38] ; mad oPos.xyz, r12, r1.x, c[-37]
     * so oPos.xyz is already window coordinates (divided by w) and oPos.w is
     * left as the ordinary clip-space W (decoded from this title's sky and
     * character programs; r1.x holds 1/w, o0.w holds w).  Both paths therefore
     * carry clip-W, and XYZRHW's rhw must be 1/W for the host shader to
     * rebuild clip space (pw = 1/rhw = W).  Passing W as rhw rebuilt 1/W
     * instead: screen positions stayed right, but perspective-correct
     * interpolation and near-plane clipping of polygons that cross the eye
     * plane (ground, walls) were wrong.  KELVIN_RHW_LEGACY=1 restores the old
     * program-mode behaviour (rhw = oPos.w). */
    float w = pos.v[3];
    if (w != w) w = 1.0f;
    if (w > 0.0f || w == 0.0f && !signbit(w)) w = w < 5.421011e-20f ? 5.421011e-20f : (w > 1.8446744e19f ? 1.8446744e19f : w);
    else w = w > -5.421011e-20f ? -5.421011e-20f : (w < -1.8446744e19f ? -1.8446744e19f : w);
    o->rhw = (K.program_mode && g_rhw_legacy) ? w : 1.0f / w;
    /* no z clamp: the D3D11 layer rebuilds clip space from (x, y, z, rhw)
     * and lets the rasteriser clip; clamping would fold geometry behind the
     * eye onto the near plane. */
    if (o->z != o->z) o->z = 0.0f;
    for (int i = 0; i < 4; ++i) { d0.v[i] = nan_to_one(d0.v[i]); d1.v[i] = nan_to_one(d1.v[i]); }
    o->diffuse = pack_color(d0.v);
    o->specular = pack_color(d1.v);
    unsigned dotraw = 0;
    for (int i = 0; i < 4; ++i) {
        uint32_t m = (K.psh.shader_stage_program >> (5 * i)) & 0x1F;
        if (m >= 0x09 && m != 0x0F && m != 0x10 && m <= 0x12) dotraw |= 1u << i;
    }
    for (int i = 0; i < 4; ++i) {
        float q = t[i].v[3];
        float u = t[i].v[0], vv = t[i].v[1];
        float r = t[i].v[2];
        if (q != 0.0f && q != 1.0f && q == q) { u /= q; vv /= q; r /= q; }
        if (dotraw & (1u << i)) {   /* DOT_* stages consume the raw homogeneous coordinate (eye vector in .w) */
            o->tex[i][0] = t[i].v[0]; o->tex[i][1] = t[i].v[1]; o->tex[i][2] = t[i].v[2]; o->tex[i][3] = t[i].v[3];
            continue;
        }
        o->tex[i][0] = u * tex_scale[i][0];
        o->tex[i][1] = vv * tex_scale[i][1];
        o->tex[i][2] = r;
        o->tex[i][3] = 1.0f;
    }
    o->fogv[0] = K.program_mode && !g_no_fog ? fog_factor(fogc) : 1.0f;   /* carried to the host VS (flag 0x20) as o.fog */
    o->fogv[1] = o->fogv[2] = o->fogv[3] = 0.0f;
}

static OutVertex *g_verts;
static uint32_t g_verts_cap;

static OutVertex *reserve_verts(uint32_t n)
{
    if (n > g_verts_cap) {
        free(g_verts);
        g_verts_cap = n + 1024;
        g_verts = (OutVertex *)malloc((size_t)g_verts_cap * sizeof(OutVertex));
        if (!g_verts) g_verts_cap = 0;
    }
    return g_verts;
}

/* Vertex transform worker pool. Large draws (the skinned Godzilla mesh is
 * 4602 vertices, drawn in several passes) dominate the translator; fetch and
 * transform only read translator state, so chunks run on worker threads.
 * KELVIN_THREADS=<n> overrides the worker count (0 = serial). */
#define XF_CHUNK 128
static struct {
    HANDLE start_sem, done_evt;
    volatile LONG next, pending;
    volatile LONG gen, slots, active, sleeping;   /* c67: spin-then-sleep helpers (a semaphore wake costs 20-60 us; ~1000 small draws per frame) */
    int nthreads, inited;
    int (*fetch)(uint32_t i, Vec4 v[16]);
    OutVertex *src;
    const float (*ts)[2];
    uint32_t count, chunk;
    float *pk; uint32_t pkmask; int pkkind;   /* c69: pack-only job (GPU vertex programs): decode attributes, no transform */
} XF;

static void pack_vertex(int kind, uint32_t i, uint32_t mask, float *out);

static void xf_work(void)
{
    Vec4 v[16];
    for (;;) {
        LONG c = InterlockedIncrement(&XF.next) - 1;
        uint32_t b = (uint32_t)c * XF.chunk, e;
        if (b >= XF.count) break;
        e = b + XF.chunk < XF.count ? b + XF.chunk : XF.count;
        if (XF.pk) {
            int na = 0; for (int q = 0; q < 16; ++q) na += (XF.pkmask >> q) & 1;
            for (uint32_t i = b; i < e; ++i) pack_vertex(XF.pkkind, i, XF.pkmask, XF.pk + (size_t)i * na * 4);
            continue;
        }
        for (uint32_t i = b; i < e; ++i) {
            if (!XF.fetch(i, v)) memset(v, 0, sizeof(v));
            transform_vertex(v, &XF.src[i], XF.ts);
        }
    }
}

/* Helpers join a job by (1) raising `active`, (2) taking a `slots` ticket.  The owner exhausts the chunks itself, CLOSES the job
 * (slots = 0) and then waits for `active` to drain, so a helper can never run xf_work against the next job's half-written
 * parameters: a late helper either sees slots <= 0 or raised `active` before the owner looked. */
static DWORD WINAPI xf_thread(LPVOID unused)
{
    LONG my_gen = XF.gen;
    (void)unused;
    for (;;) {
        unsigned spin = 0;
        static unsigned spin_max;   /* c119: KELVIN_SPIN=<n> (default 40000 = the old value) */
        if (!spin_max) { const char *se = getenv("KELVIN_SPIN"); spin_max = se && atoi(se) > 0 ? (unsigned)atoi(se) : 40000u; }
        while (XF.gen == my_gen) {   /* draws arrive in bursts: stay hot for ~100 us after each job, then sleep */
            if (++spin < spin_max) { YieldProcessor(); continue; }
            InterlockedIncrement(&XF.sleeping);
            if (XF.gen == my_gen) WaitForSingleObject(XF.start_sem, INFINITE);
            InterlockedDecrement(&XF.sleeping);
            spin = 0;
        }
        my_gen = XF.gen;
        InterlockedIncrement(&XF.active);
        if (InterlockedDecrement(&XF.slots) < 0) { InterlockedDecrement(&XF.active); continue; }
        xf_work();
        InterlockedDecrement(&XF.active);
    }
}

static int xf_parallel(uint32_t count, int (*fetch)(uint32_t, Vec4 *), OutVertex *src,
                       const float ts[4][2])
{
    if (!XF.inited) {
        SYSTEM_INFO si;
        const char *e = kenv("KELVIN_THREADS");
        GetSystemInfo(&si);
        XF.nthreads = e ? atoi(e) : (int)si.dwNumberOfProcessors - 1;
        if (!e && XF.nthreads > 2) XF.nthreads = 2;   /* c119: default cap 2 (was 8). With GPU vertex programs the workers only pack attributes; 4K Tokyo fight: 8 workers 5.5 cores avg, 2 workers 2.9, none 2.2, all at 59.3 fps. KELVIN_THREADS=<n> / ini render_threads override */
        if (XF.nthreads > 30) XF.nthreads = 30;
        if (XF.nthreads < 0) XF.nthreads = 0;
        XF.start_sem = CreateSemaphoreA(NULL, 0, 64, NULL);
        XF.done_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
        for (int i = 0; i < XF.nthreads; ++i) CreateThread(NULL, 0, xf_thread, NULL, 0, NULL);
        XF.inited = 1;
    }
    if (XF.nthreads <= 0) return 0;
    /* decode the program on this thread: the decoder caches lazily */
    if (K.program_mode)
        for (uint32_t slot = K.program_start; slot < PROGRAM_SLOTS; ++slot)
            if (vdec(slot)->fin) break;
    /* Demo gameplay is ~1000 draws of under 512 vertices: split every
     * draw worth a thread wake-up, and wake only as many workers as there
     * are chunks beyond the one this thread takes. */
    {
        uint32_t chunk = count / (uint32_t)(XF.nthreads + 1);
        int wake;
        if (chunk < 32) chunk = 32;
        if (chunk > XF_CHUNK) chunk = XF_CHUNK;
        wake = (int)((count + chunk - 1) / chunk) - 1;
        if (wake > XF.nthreads) wake = XF.nthreads;
        if (wake <= 0) return 0;
        XF.fetch = fetch; XF.src = src; XF.ts = ts; XF.count = count; XF.chunk = chunk;
        XF.next = 0;
        MemoryBarrier();
        InterlockedExchange(&XF.slots, wake);
        InterlockedIncrement(&XF.gen);
        { LONG sl = XF.sleeping; if (sl > 0) ReleaseSemaphore(XF.start_sem, sl < wake ? sl : wake, NULL); }
    }
    xf_work();
    InterlockedExchange(&XF.slots, 0);   /* close the job */
    {   unsigned w = 0;
        while (XF.active > 0) { if (++w > 2000u) { SwitchToThread(); w = 0; } else YieldProcessor(); }
    }
    return 1;
}

/* Smallest draw (vertices) handed to the helper threads; KELVIN_PAR_MIN=<n> overrides (default 80). */
static uint32_t xf_par_min(void)
{
    static int v = -1;
    if (v < 0) { const char *e = kenv("KELVIN_PAR_MIN"); v = e ? atoi(e) : 80; if (v < 1) v = 1; }
    return (uint32_t)v;
}

/* per-phase translator time (QPC ticks), read by the host's frame report */
long long g_kelvin_perf[4];   /* 0 textures, 1 vertex transform, 2 D3D submit, 3 draws */
static int g_kperf_on = -1;   /* c80: KELVIN_PERF=1 enables the per-phase timers (a QueryPerformanceCounter is ~0.2 us on this machine and ran ~14x per draw: 13% of the guest thread) */
static long long kperf_now(void)
{
    LARGE_INTEGER t;
    if (g_kperf_on < 0) g_kperf_on = getenv("KELVIN_PERF") != NULL;
    if (!g_kperf_on) return 0;
    QueryPerformanceCounter(&t); return t.QuadPart;
}

static uint32_t g_inline_src, g_inline_in_call;
static int fetch_indexed_cb(uint32_t i, Vec4 v[16]);
static int fetch_arrays_cb(uint32_t i, Vec4 v[16]);
static int fetch_inline_cb(uint32_t i, Vec4 v[16]);
static void fetch_array(uint32_t index, Vec4 v[16]);

/* Indexed draws name each shared vertex several times (~6 references per
 * vertex in a closed triangle mesh); the transform used to run per index.
 * Run it once per distinct index and copy. */
static uint32_t *g_uniq, g_uniq_cap;
static int32_t *g_umap; static uint32_t g_umap_cap;
static OutVertex *g_utmp; static uint32_t g_utmp_cap;
static int fetch_unique_cb(uint32_t i, Vec4 v[16]) { fetch_array(g_uniq[i], v); return 1; }

static int xf_indexed_unique(uint32_t count, OutVertex *src, float tex_scale[4][2])
{
    uint32_t lo = 0xFFFFFFFFu, hi = 0, range, n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t x = K.indices[i];
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    range = hi - lo + 1;
    if (range > 1u << 20 || range > count * 8u + 1024u) return 0;
    if (range > g_umap_cap) {
        int32_t *m = (int32_t *)realloc(g_umap, range * sizeof(int32_t));
        if (!m) return 0;
        g_umap = m; g_umap_cap = range;
    }
    if (count > g_uniq_cap) {
        uint32_t *u = (uint32_t *)realloc(g_uniq, count * sizeof(uint32_t));
        OutVertex *t = (OutVertex *)realloc(g_utmp, count * sizeof(OutVertex));
        if (u) g_uniq = u;
        if (t) g_utmp = t;
        if (!u || !t) return 0;
        g_uniq_cap = g_utmp_cap = count;
    }
    memset(g_umap, 0xFF, range * sizeof(int32_t));
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t k = K.indices[i] - lo;
        if (g_umap[k] < 0) { g_umap[k] = (int32_t)n; g_uniq[n++] = K.indices[i]; }
    }
    if (n * 4 > count * 3) return 0;   /* little sharing: plain path is as good */
    if (!(n >= xf_par_min() && xf_parallel(n, fetch_unique_cb, g_utmp, (const float (*)[2])tex_scale))) {
        Vec4 v[16];
        for (uint32_t i = 0; i < n; ++i) {
            fetch_array(g_uniq[i], v);
            transform_vertex(v, &g_utmp[i], tex_scale);
        }
    }
    for (uint32_t i = 0; i < count; ++i)
        src[i] = g_utmp[g_umap[K.indices[i] - lo]];
    return 1;
}

/* c263 probe (STE_FSQUAD_LOG=<max lines>): small draws (<= 16 vertices) whose window-space bbox covers (nearly) the whole 640x480, from both vertex paths. The CPU path widens only
 * untextured ones (fs); the GPU path never did (kelvin_apply_xmap(0, 0)). Logged once per (program, texture, colour, blend) key. g_kelvin_fsq_count counts every such draw (the exe can shoot a frame on it). */
unsigned g_kelvin_fsq_count;
static int g_fsq_lim(void) { static int lim = -1; if (lim < 0) { const char *q = getenv("STE_FSQUAD_LOG"); lim = q ? atoi(q) : 0; } return lim; }
static void fsquad_probe(const char *path, uint32_t n, uint32_t prim, float x0, float x1, float y0, float y1, uint32_t cd, uint32_t tex0)
{
    static int nl; static uint32_t seen[256][2]; static int nseen;
    if (g_rtp_on() && ((g_rtp_cur_w && g_rtp_cur_w <= 128) || (tex0 && rtp_is_small(tex0)))) {
        static int nd;
        if (nd++ < 500) fprintf(stderr, "[RTDRAW] %s kf=%u n=%u prim=%u box=(%.0f..%.0f,%.0f..%.0f) col=%08X tex0=%08X into=%ux%u samples_small=%d blend=%u %X/%X vp=%u c96=%.5f c97=%.5f c58=%.1f,%.1f\n",
                path, K.frame, n, prim, x0, x1, y0, y1, cd, tex0, g_rtp_cur_w, g_rtp_cur_h, tex0 && rtp_is_small(tex0), K.blend_enable, K.sfactor, K.dfactor, K.program_start,
                K.constants[96][0], K.constants[97][1], K.constants[58][0], K.constants[58][1]);
    }
    static int area_mode = -1;   /* c267: STE_FSQUAD_AREA=1: log every small draw whose bbox is >= 15% of 640x480 (centred sub-rectangles included), key includes the bbox */
    if (area_mode < 0) { const char *q = getenv("STE_FSQUAD_AREA"); area_mode = q && q[0] == '1'; }
    if (nl < g_fsq_lim() && (area_mode ? ((x1 - x0) * (y1 - y0) >= 0.15f * 640.0f * 480.0f && (x1 - x0) < 1e6f && (y1 - y0) < 1e6f) : (x0 <= 8.0f && x1 >= 632.0f && y0 <= 8.0f && y1 >= 472.0f))) {
        uint32_t key[2] = { (K.program_start << 24) ^ tex0 ^ (K.sfactor << 4) ^ K.dfactor ^ (K.blend_enable << 20) ^ (area_mode ? ((uint32_t)(int)(x0 / 16.0f) * 2654435761u ^ (uint32_t)(int)(x1 / 16.0f) * 40503u ^ (uint32_t)(int)(y0 / 16.0f) * 97u ^ (uint32_t)(int)(y1 / 16.0f) * 7919u) : 0u), cd };
        int hit = 0, j;
        if (K.blend_enable && K.dfactor == 1 && !d3d8_front_end_active()) g_kelvin_fsq_count++;   /* additive in-game flash only (the exe shoots a frame on it) */
        for (j = 0; j < nseen; ++j) if (seen[j][0] == key[0] && seen[j][1] == key[1]) { hit = 1; break; }
        if (!hit && nseen < 256) { seen[nseen][0] = key[0]; seen[nseen][1] = key[1]; nseen++; }
        if (!hit) { nl++; fprintf(stderr, "[FSQUAD] %s kf=%u fe=%d vp=%u n=%u prim=%u box=(%.0f..%.0f,%.0f..%.0f) col=%08X tex0=%08X blend=%u %X/%X depth=%u/%u cmask=%08X c96=%.5f c97=%.5f c58=%.1f,%.1f\n",
            path, K.frame, d3d8_front_end_active(), K.program_start, n, prim, x0, x1, y0, y1, cd, tex0, K.blend_enable, K.sfactor, K.dfactor, K.depth_test, K.depth_mask, K.color_mask,
            K.constants[96][0], K.constants[97][1], K.constants[58][0], K.constants[58][1]); }
    }
}

/* Pixel-side state for a Kelvin draw (render states, register combiners, textures, FVF) shared by the CPU and GPU vertex paths. */
extern void d3d8_set_scissor(int on, int l, int t, int r, int b);
unsigned g_kelvin_add_draws;   /* c290: in-game additive-blend draws issued so far (the frame ring's auto trigger takes per-frame deltas: heavy fire = many additive draws) */
static void setup_draw_state(IDirect3DDevice8 *dev, int textured[4], IDirect3DTexture8 *tex[4])
{
    if (K.blend_enable && K.dfactor == 1 && !d3d8_front_end_active()) g_kelvin_add_draws++;
    {   /* c264 probe (STE_FOG_LOG=<max lines>): every change of the fog state a draw is issued with (enable, mode, params, colour), as the title set it (g_fog_p already divided by fog_scale) */
        static int lim = -1, nl; static uint32_t l_en = 0xFFFFFFFFu, l_mode, l_col; static float l_p[3];
        if (lim < 0) { const char *q = getenv("STE_FOG_LOG"); lim = q ? atoi(q) : 0; }
        if (lim > 0 && nl < lim && (g_fog_en != l_en || g_fog_mode != l_mode || K.psh.fog_color != l_col || memcmp(l_p, g_fog_p, sizeof l_p))) {
            l_en = g_fog_en; l_mode = g_fog_mode; l_col = K.psh.fog_color; memcpy(l_p, g_fog_p, sizeof l_p); nl++;
            fprintf(stderr, "[FOGSTATE] kf=%u fe=%d vp=%u en=%u mode=0x%X p=(%g,%g,%g) color(argb)=%08X no_fog=%d fog_scale=%g\n", K.frame, d3d8_front_end_active(), K.program_mode ? K.program_start : 0,
                    g_fog_en, g_fog_mode, g_fog_p[0], g_fog_p[1], g_fog_p[2], K.psh.fog_color, g_no_fog, g_fog_scale);
        }
    }
    {   /* c136: with aspect=wide + menu_aspect=pillar the screen-space UI draws (vertex program start 17/34) are laid out as the centred 640x480 block, but some run far past the TV frame
         * (the CRT static overlay: quads up to x = 1779 in the 640-wide layout; bubble sprites) and bled onto the wall beside the TV. The 4:3 screen clipped them at its edge: scissor them to the block. STE_FE_STATIC_CLIP=0 disables. */
        static int on = -1; int clip = 0;
        if (on < 0) { const char *e = getenv("STE_FE_STATIC_CLIP"); on = !(e && e[0] == 0x30); }
        if (on && K.program_mode && K.program_start == 17 && K.tex[0].offset) {   /* UI program: only the CRT static (256x64) and the bubble sprites (32x32, 8x8) run past the TV; clipping every UI draw would also cut the panel overscan */
            const D3D8VideoCfg *vc = d3d8_video_cfg();
            if (vc->aspect == D3D8V_ASPECT_WIDE && vc->menu_aspect == D3D8V_ASPECT_PILLAR && vc->logical_w > 640.5f && d3d8_front_end_active()) {
                uint32_t tw = 0, th = 0, tp; size_t tb;
                tex_geometry(&K.tex[0], &tw, &th, &tp, &tb);
                if ((tw == 256 && th == 64) || (tw == 32 && th == 32) || (tw == 8 && th == 8)) {
                    float xo = (vc->logical_w - 640.0f) * 0.5f - 12.0f, sc = vc->scale;   /* 12 logical px of overscan each side (the TV picture itself is that much wider than the 640 block) */
                    d3d8_set_scissor(1, (int)(xo * sc + 0.5f), 0, (int)((xo + 640.0f + 24.0f) * sc + 0.5f), (int)(480.0f * sc + 0.5f));
                    clip = 1;
                }
            }
        }
        if (!clip) d3d8_set_scissor(0, 0, 0, 0, 0);
    }
    /* c128: anisotropy / mip_bias apply to world draws only: not the front-end, not the screen-space UI program (start 17) */
    d3d8_sampler_quality_world(!d3d8_front_end_active() && K.program_mode && K.program_start != 17);
    apply_state(dev, textured);
    if (kenv("KELVIN_NO_PSH")) {
        d3d8_nv2a_psh_set(NULL);
    } else {
        K.psh.alpha_test = K.alpha_test;
        K.psh.alpha_func = K.alpha_func;
        K.psh.alpha_ref = K.alpha_ref;
        K.psh.cube_mask = 0;
        for (int s = 0; s < 4; ++s) {
            uint32_t mode = (K.psh.shader_stage_program >> (5 * s)) & 0x1F;
            if ((mode == 0x03 || mode == 0x0C || mode == 0x0E || mode == 0x0B || mode == 0x0D) && (K.tex[s].format & 4u)) {
                bind_cube(s);
                K.psh.cube_mask |= 1u << s;
            } else {
                d3d8_nv2a_psh_set_cube(s, NULL, 0, 0);
            }
        }
        d3d8_nv2a_psh_set(&K.psh);
    }
    for (int s = 0; s < 4; ++s)
        if (textured[s] && (g_set_tex[s] != tex[s] || kenv("KELVIN_NO_STATE_CACHE"))) { dev->lpVtbl->SetTexture(dev, s, (IDirect3DBaseTexture8 *)tex[s]); g_set_tex[s] = tex[s]; }
    if (!g_fvf_set) {
        dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | 0x500u |
                                      0x02AA0000u /* TEXCOORDSIZE4 x5 */);
        g_fvf_set = !kenv("KELVIN_NO_STATE_CACHE");
    }
}

static void kelvin_apply_xmap(int stretch, int untextured_fullscreen)
{
    const D3D8VideoCfg *vc = d3d8_video_cfg();
    float xs = 1.0f, xo = 0.0f;
    if (vc->aspect == D3D8V_ASPECT_WIDE && vc->logical_w > 640.5f) {
        const float (*c)[4] = (const float (*)[4])K.constants;
        int canon2d = fabsf(c[96][0] - 0.003125f) < 2e-5f && fabsf(c[97][1] + 1.0f / 240.0f) < 2e-5f &&
                      c[96][1] == 0.0f && c[97][0] == 0.0f && c[99][3] == 1.0f;
        /* c58 = viewport scale: a 3D draw that does not use the full 640x480 viewport (menu model panels) is part of the 2D layout */
        int full_vp = fabsf(c[58][0] - 320.0f) < 0.5f && fabsf(c[58][1] + 240.0f) < 0.5f;
        /* an untextured quad covering the whole 640x480 (fade to black, flash) must cover the whole wide surface too */
        int two_d = !stretch && !(canon2d && untextured_fullscreen) && (canon2d || !full_vp);
        if (d3d8_front_end_active()) {   /* c102: menu_aspect for the shell/front-end (stretched "3D" menu elements bled out of the TV frame) */
            if (vc->menu_aspect == D3D8V_ASPECT_PILLAR) two_d = !stretch && !(canon2d && untextured_fullscreen);   /* everything is the centred 640x480 block */
            else if (vc->menu_aspect == D3D8V_ASPECT_STRETCH) two_d = 0;                                       /* everything stretched across the wide surface */
        }
        if (two_d) xo = (vc->logical_w - 640.0f) * 0.5f;
        else xs = vc->logical_w / 640.0f;
    }
    d3d8_set_kelvin_xmap(xs, xo);
}

/* ------------------------------------------------------------------ */
/* GPU vertex processing (c69): vertex programs run in a D3D11 vertex shader. */
/* ------------------------------------------------------------------ */
static void pack_vertex(int kind, uint32_t i, uint32_t mask, float *out)
{
    uint32_t idx = 0;
    const uint32_t *ibase = NULL;
    if (kind == 1) ibase = K.inline_data + (size_t)i * inline_vertex_dwords();
    else idx = kind == 0 ? K.array_start + i : kind == 2 ? g_uniq[i] : K.indices[i];
    for (int a = 0; a < 16; ++a) {
        uint32_t n = kind == 1 ? attr_inline_dwords(a) : 0;
        if (mask & (1u << a)) {
            if (kind == 1) {
                if (n) decode_attr(a, (const uint8_t *)ibase, out); else memcpy(out, K.attr_const[a], 16);
            } else if (attr_size(a)) {
                decode_attr(a, guest_ptr(K.attr_offset[a] + idx * attr_stride(a)), out);
            } else {
                memcpy(out, K.attr_const[a], 16);
            }
            out += 4;
        }
        if (kind == 1) ibase += n;
    }
}

typedef struct {
    uint64_t hash;
    uint32_t nwords, mask, vdraws;
    uint32_t *words;
    D3D8KvpProgram *prog;
    uint64_t vverts, vmism;
} KvpEntry;
static KvpEntry g_kvp[512];
static int g_kvp_n;
static KvpEntry *g_kvp_memo;
static uint32_t g_kvp_memo_gen, g_kvp_memo_start;

static int gpu_vp_mode(void)   /* 0 = CPU interpreter, 1 = GPU vertex programs */
{
    static int v = -1;
    if (v < 0) v = kenv("KELVIN_CPU_VP") ? 0 : 1;
    return v;
}

static KvpEntry *kvp_get(void)
{
    uint32_t start = K.program_start, nw = 0;
    uint64_t h = 1469598103934665603ull;
    KvpEntry *e;
    if (g_kvp_memo && g_kvp_memo_gen == g_prog_gen && g_kvp_memo_start == start) return g_kvp_memo;
    if (start >= PROGRAM_SLOTS) return NULL;
    for (uint32_t sl = start; sl < PROGRAM_SLOTS; ++sl) {
        for (int k = 0; k < 4; ++k) { h ^= K.program[sl][k]; h *= 1099511628211ull; }
        ++nw;
        if (K.program[sl][3] & 1u) break;
    }
    e = NULL;
    for (int i = 0; i < g_kvp_n; ++i)
        if (g_kvp[i].hash == h && g_kvp[i].nwords == nw && !memcmp(g_kvp[i].words, &K.program[start][0], (size_t)nw * 16)) { e = &g_kvp[i]; break; }
    if (!e) {
        char *hlsl;
        uint32_t mask = 0;
        char name[40];
        if (g_kvp_n >= (int)(sizeof(g_kvp) / sizeof(g_kvp[0]))) return NULL;
        e = &g_kvp[g_kvp_n++];
        memset(e, 0, sizeof(*e));
        e->hash = h; e->nwords = nw;
        e->words = (uint32_t *)malloc((size_t)nw * 16);
        if (e->words) memcpy(e->words, &K.program[start][0], (size_t)nw * 16);
        snprintf(name, sizeof(name), "kvp_%016llx", (unsigned long long)h);
        hlsl = kvp_translate(start, &mask);
        if (hlsl) {
            const char *dd = kenv("KELVIN_VP_DUMP_HLSL");
            if (dd) {
                char path[512]; FILE *fp;
                snprintf(path, sizeof(path), "%s\\%s.hlsl", dd, name);
                fp = fopen(path, "wb");
                if (fp) { fwrite(hlsl, 1, strlen(hlsl), fp); fclose(fp); }
            }
            e->prog = d3d8_kvp_create(hlsl, name, mask);
            e->mask = mask;
            fprintf(stderr, "[KVP] program %s start=%u words=%u inputs=%04X %s\n", name, start, nw, mask,
                    !e->prog ? "CPU fallback (create failed)" : d3d8_kvp_status(e->prog) == 1 ? "GPU (shader cache hit)" : "GPU (compiling on the worker; CPU path until ready)");
            free(hlsl);
        } else {
            fprintf(stderr, "[KVP] program %s start=%u words=%u: not translatable, CPU fallback\n", name, start, nw);
        }
    }
    g_kvp_memo = e; g_kvp_memo_gen = g_prog_gen; g_kvp_memo_start = start;
    return e;
}

static float *g_pk; static size_t g_pk_cap;      /* packed vertices (floats) */
static uint32_t *g_gidx; static size_t g_gidx_cap;
static uint32_t g_gu_lo;

/* index remap for indexed draws: returns the number of distinct vertices, 0 if the draw is not worth deduplicating */
static uint32_t gpu_build_unique(uint32_t count)
{
    uint32_t lo = 0xFFFFFFFFu, hi = 0, range, n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t x = K.indices[i];
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    range = hi - lo + 1;
    if (range > 1u << 20 || range > count * 8u + 1024u) return 0;
    if (range > g_umap_cap) {
        int32_t *m = (int32_t *)realloc(g_umap, range * sizeof(int32_t));
        if (!m) return 0;
        g_umap = m; g_umap_cap = range;
    }
    if (count > g_uniq_cap) {
        uint32_t *u = (uint32_t *)realloc(g_uniq, count * sizeof(uint32_t));
        OutVertex *tt = (OutVertex *)realloc(g_utmp, count * sizeof(OutVertex));
        if (u) g_uniq = u;
        if (tt) g_utmp = tt;
        if (!u || !tt) return 0;
        g_uniq_cap = g_utmp_cap = count;
    }
    memset(g_umap, 0xFF, range * sizeof(int32_t));
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t k = K.indices[i] - lo;
        if (g_umap[k] < 0) { g_umap[k] = (int32_t)n; g_uniq[n++] = K.indices[i]; }
    }
    if (n * 4 > count * 3) return 0;
    g_gu_lo = lo;
    return n;
}

static int kvp_close(float g, float c, float atol, float rtol)
{
    float err;
    if (g != g || c != c) return (g != g) && (c != c);
    err = fabsf(g - c);
    return err <= atol + rtol * fabsf(c);
}

/* Compares the GPU (stream-out) result for the packed vertices against the CPU interpreter; returns the mismatch count. */
static uint32_t gpu_vp_verify(KvpEntry *e, const D3D8KvpDraw *dd, int kind, uint32_t n, const float tex_scale[4][2], uint32_t draw_no)
{
    uint32_t bad = 0;
    float *gout = (float *)malloc((size_t)n * KVP_VERIFY_FLOATS * sizeof(float));
    if (!gout) return 0;
    if (!d3d8_kvp_verify(e->prog, dd, gout)) {
        fprintf(stderr, "[KVP-VERIFY] draw#%u: GPU verify pass unavailable\n", draw_no);
        free(gout);
        return 0;
    }
    for (uint32_t i = 0; i < n; ++i) {
        Vec4 v[16];
        OutVertex o;
        const float *g = gout + (size_t)i * KVP_VERIFY_FLOATS;
        float c[KVP_VERIFY_FLOATS];
        static const char *nm[KVP_VERIFY_FLOATS] = {
            "x","y","z","w", "d.r","d.g","d.b","d.a", "s.r","s.g","s.b","s.a",
            "t0.s","t0.t","t0.r","t0.q","t1.s","t1.t","t1.r","t1.q","t2.s","t2.t","t2.r","t2.q","t3.s","t3.t","t3.r","t3.q","fog" };
        int vbad = 0;
        if (kind == 1) fetch_inline(i, v);
        else fetch_array(kind == 0 ? K.array_start + i : kind == 2 ? g_uniq[i] : K.indices[i], v);
        transform_vertex(v, &o, tex_scale);
        c[0] = o.x; c[1] = o.y; c[2] = o.z; c[3] = 1.0f / o.rhw;
        c[4] = ((o.diffuse >> 16) & 255) / 255.0f; c[5] = ((o.diffuse >> 8) & 255) / 255.0f;
        c[6] = (o.diffuse & 255) / 255.0f;         c[7] = (o.diffuse >> 24) / 255.0f;
        c[8] = ((o.specular >> 16) & 255) / 255.0f; c[9] = ((o.specular >> 8) & 255) / 255.0f;
        c[10] = (o.specular & 255) / 255.0f;        c[11] = (o.specular >> 24) / 255.0f;
        for (int s = 0; s < 4; ++s) for (int k = 0; k < 4; ++k) c[12 + s * 4 + k] = o.tex[s][k];
        c[28] = o.fogv[0];
        for (int k = 0; k < KVP_VERIFY_FLOATS; ++k) {
            float atol = k < 4 ? 2e-3f : k < 12 ? 1.5f / 255.0f : k == 28 ? 6e-3f : 2e-4f;
            float rtol = k < 4 ? 2e-3f : k < 12 ? 0.0f : k == 28 ? 0.0f : 2e-3f;
            if (!kvp_close(g[k], c[k], atol, rtol)) {
                if (e->vmism < 12)
                    fprintf(stderr, "[KVP-VERIFY] MISMATCH prog=%016llX draw#%u vtx %u %s gpu=%.7g cpu=%.7g\n",
                            (unsigned long long)e->hash, draw_no, i, nm[k], g[k], c[k]);
                e->vmism++;
                vbad = 1;
            }
        }
        bad += (uint32_t)vbad;
    }
    e->vverts += n;
    fprintf(stderr, "[KVP-VERIFY] draw#%u prog=%016llX verts=%u bad=%u (program total %llu/%llu)\n", draw_no,
            (unsigned long long)e->hash, n, bad, (unsigned long long)e->vmism, (unsigned long long)e->vverts);
    free(gout);
    return bad;
}

static int gpu_vp_draw(IDirect3DDevice8 *dev, uint32_t count, int (*fetch)(uint32_t, Vec4 *), uint32_t prim,
                       int textured[4], IDirect3DTexture8 *tex[4], float tex_scale[4][2])
{
    long long t0 = kperf_now(), t1;
    KvpEntry *e = kvp_get();
    D3D8KvpDraw dd;
    uint32_t n, nidx = 0, na = 0, unique = 0, i;
    int kind, topo, need_idx = 0;
    if (!e || !e->prog) return 0;
    {   int st = d3d8_kvp_status(e->prog);
        if (st == 0) return 0;                      /* still compiling on the shader worker: this draw takes the CPU path */
        if (st < 0) { e->prog = NULL; return 0; }   /* failed: CPU path for good */
    }
    for (int a = 0; a < 16; ++a) na += (e->mask >> a) & 1u;
    memset(&dd, 0, sizeof(dd));

    if (fetch == fetch_indexed_cb) {
        unique = (count >= 16) ? gpu_build_unique(count) : 0;
        if (unique) { kind = 2; n = unique; need_idx = 1; }
        else { kind = 3; n = count; }
    } else if (fetch == fetch_arrays_cb) { kind = 0; n = count; }
    else if (fetch == fetch_inline_cb) { kind = 1; n = count; }
    else return 0;

    switch (prim) {
    case 1: topo = KVP_TOPO_POINTS; break;
    case 2: topo = KVP_TOPO_LINES; break;
    case 3: case 4: topo = KVP_TOPO_LINESTRIP; break;
    case 5: topo = KVP_TOPO_TRIS; break;
    case 6: topo = KVP_TOPO_TRISTRIP; break;
    case 7: case 10: topo = KVP_TOPO_TRIS; need_idx = 1; nidx = count >= 3 ? (count - 2) * 3 : 0; break;
    case 8: topo = KVP_TOPO_TRIS; need_idx = 1; nidx = count / 4 * 6; break;
    case 9: topo = KVP_TOPO_TRIS; need_idx = 1; nidx = count >= 4 ? (count - 2) / 2 * 6 : 0; break;
    default: return 0;
    }
    if (prim <= 6) nidx = count;   /* indexed list/strip: identity over remapped ids */
    if (need_idx) {
        size_t cap = nidx;
        if (cap > g_gidx_cap) {
            uint32_t *q = (uint32_t *)realloc(g_gidx, cap * sizeof(uint32_t));
            if (!q) return 0;
            g_gidx = q; g_gidx_cap = cap;
        }
#define VID(j) (kind == 2 ? (uint32_t)g_umap[K.indices[(j)] - g_gu_lo] : (uint32_t)(j))
        if (prim == 7 || prim == 10) {
            uint32_t o = 0;
            for (i = 0; i + 2 < count; ++i) { g_gidx[o++] = VID(0); g_gidx[o++] = VID(i + 1); g_gidx[o++] = VID(i + 2); }
        } else if (prim == 8) {
            uint32_t o = 0;
            for (i = 0; i + 3 < count; i += 4) {
                g_gidx[o++] = VID(i); g_gidx[o++] = VID(i + 1); g_gidx[o++] = VID(i + 2);
                g_gidx[o++] = VID(i); g_gidx[o++] = VID(i + 2); g_gidx[o++] = VID(i + 3);
            }
        } else if (prim == 9) {
            uint32_t o = 0;
            for (i = 0; i + 3 < count; i += 2) {
                g_gidx[o++] = VID(i); g_gidx[o++] = VID(i + 1); g_gidx[o++] = VID(i + 3);
                g_gidx[o++] = VID(i); g_gidx[o++] = VID(i + 3); g_gidx[o++] = VID(i + 2);
            }
        } else {
            for (i = 0; i < count; ++i) g_gidx[i] = VID(i);
        }
#undef VID
    }
    if (need_idx && !nidx) return 0;

    {   size_t need = (size_t)n * na * 4;
        if (need > g_pk_cap) {
            float *q = (float *)realloc(g_pk, need * sizeof(float));
            if (!q) return 0;
            g_pk = q; g_pk_cap = need;
        }
    }
    XF.pk = g_pk; XF.pkmask = e->mask; XF.pkkind = kind;
    if (!(n >= xf_par_min() && xf_parallel(n, NULL, NULL, NULL)))
        for (i = 0; i < n; ++i) pack_vertex(kind, i, e->mask, g_pk + (size_t)i * na * 4);
    XF.pk = NULL;
    t1 = kperf_now(); g_kelvin_perf[1] += t1 - t0; t0 = t1;

    {   /* STE c77 probe (KELVIN_ACTOR_LOG=<file>): window-space bbox of large skinned-mesh draws (actors), every 8th vertex through the CPU
         * interpreter; used for the fps_cap physics comparison, off by default */
        static FILE *alog; static int ainit; static LARGE_INTEGER aq0, aqf;
        if (!ainit) { const char *p = kenv("KELVIN_ACTOR_LOG"); ainit = 1; if (p) alog = fopen(p, "w"); QueryPerformanceFrequency(&aqf); QueryPerformanceCounter(&aq0); }
        if (alog && (e->mask & 0xC00u) == 0xC00u && n >= 1000) {
            float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f; uint32_t cnt = 0; LARGE_INTEGER q;
            for (i = 0; i < n; i += 8) {
                Vec4 v[16]; OutVertex o;
                if (kind == 1) fetch_inline(i, v); else fetch_array(kind == 0 ? K.array_start + i : kind == 2 ? g_uniq[i] : K.indices[i], v);
                transform_vertex(v, &o, (const float (*)[2])tex_scale);
                if (!(o.rhw > 0.0f) || o.x != o.x || o.y != o.y || fabsf(o.x) > 4000.0f || fabsf(o.y) > 4000.0f) continue;
                if (o.x < x0) x0 = o.x; if (o.x > x1) x1 = o.x; if (o.y < y0) y0 = o.y; if (o.y > y1) y1 = o.y; ++cnt;
            }
            QueryPerformanceCounter(&q);
            if (cnt) { fprintf(alog, "%.4f %u %llX %u %.1f %.1f %.1f %.1f\n", (double)(q.QuadPart - aq0.QuadPart) / (double)aqf.QuadPart, K.frame, (unsigned long long)e->hash, n, x0, x1, y0, y1); fflush(alog); }
        }
    }    dd.verts = g_pk; dd.nverts = n;
    dd.indices = need_idx ? g_gidx : NULL; dd.nidx = need_idx ? nidx : 0;
    dd.topo = topo;
    dd.constants = &K.constants[0][0];
    dd.clip_max = K.clip_max;
    dd.fog_en = (g_fog_en && !g_no_fog) ? 1.0f : 0.0f;
    dd.fog_mode = g_fog_mode;
    dd.fog_p[0] = g_fog_p[0]; dd.fog_p[1] = g_fog_p[1]; dd.fog_p[2] = g_fog_p[2];
    for (int s = 0; s < 4; ++s) {
        uint32_t m = (K.psh.shader_stage_program >> (5 * s)) & 0x1F;
        dd.tscale[s * 2] = tex_scale[s][0]; dd.tscale[s * 2 + 1] = tex_scale[s][1];
        if (m >= 0x09 && m != 0x0F && m != 0x10 && m <= 0x12) dd.dotraw |= 1u << s;
    }

    {   static int vlim = -1;
        if (vlim < 0) { const char *ev = kenv("KELVIN_VP_VERIFY"); vlim = ev ? atoi(ev) : 0; }
        if (vlim > 0 && e->vdraws < (uint32_t)vlim && n <= 200000u) {
            e->vdraws++;
            if (gpu_vp_verify(e, &dd, kind, n, (const float (*)[2])tex_scale, K.stats.draws + 1) && kenv("KELVIN_VP_VERIFY_FATAL")) {
                fprintf(stderr, "[KVP-VERIFY] FATAL: GPU vertex program disagrees with the CPU interpreter\n");
                fflush(stderr);
                abort();
            }
        }
    }

    setup_draw_state(dev, textured, tex);
    if ((g_fsq_lim() > 0 || g_rtp_on()) && n <= 64) {   /* c263 probe: bbox of a small GPU-path draw through the CPU interpreter (see fsquad_probe) */
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f; uint32_t cd = 0; int ok = 1;
        for (i = 0; i < n; ++i) {
            Vec4 v[16]; OutVertex o;
            if (kind == 1) fetch_inline(i, v); else fetch_array(kind == 0 ? K.array_start + i : kind == 2 ? g_uniq[i] : K.indices[i], v);
            transform_vertex(v, &o, (const float (*)[2])tex_scale);
            if (!(o.rhw > 0.0f) || o.x != o.x || o.y != o.y) { ok = 0; break; }
            if (o.x < x0) x0 = o.x; if (o.x > x1) x1 = o.x; if (o.y < y0) y0 = o.y; if (o.y > y1) y1 = o.y;
            if (!i) cd = o.diffuse;
        }
        if (ok) fsquad_probe("gpu", n, prim, x0, x1, y0, y1, cd, textured[0] ? K.tex[0].offset : 0);
    }
    kelvin_apply_xmap(0, 0);
    {   /* c108 debug: KELVIN_ZDBG=<hex tex offset>:<bits> (1 = depth write off, 2 = depth test off, 4 = alpha blend off) for draws whose stage-0 texture sits at that offset */
        static int zi = -1; static uint32_t zoff, zbits; static int zhit;
        if (zi < 0) { const char *z = kenv("KELVIN_ZDBG"); zi = 0; if (z) { char *c = NULL; zoff = (uint32_t)strtoul(z, &c, 16); zbits = (c && *c == 0x3A) ? (uint32_t)strtoul(c + 1, NULL, 0) : 1u; zi = 1; } }
        static int fefix = -1;
        if (fefix < 0) { const char *z = getenv("STE_FRONTEND_ZFIX"); fefix = !(z && *z == 0x30); }
        static int gbl = -1; if (gbl < 0) { const char *g = getenv("STE_DBG_GLASS"); gbl = g ? atoi(g) : 0; }
        if (gbl && K.psh.shader_stage_program == 0x61u) {   /* c127 debug: 1 = alpha blend ON, 2 = blend on + depth write off, 3 = depth write off only */
            if (gbl & 1) { dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE); dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA); }
            if (gbl & 2) dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
            zhit = 1; g_as_valid = 0;
        } else
        if (fefix && K.blend_enable && K.depth_mask && d3d8_front_end_active()) {
            /* c110: in the front-end a translucent draw that writes depth (the globe halo GFC_Globe_Halo: a self-overlapping 1532-vertex strip, depth LEQUAL + write on) rejects its own
             * overlapping fragments after rasterisation differences and leaves dark hatch dashes on the glow ring; with the depth write off it is the intended soft band
             * (tmp\q1b\zdbg_crop.png).  STE_FRONTEND_ZFIX=0 restores the old behaviour. */
            dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE); zhit = 1; g_as_valid = 0;
        } else
        if (zi && textured[0] && K.tex[0].offset == zoff) {
            if (zbits & 1) dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
            if (zbits & 2) dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
            if (zbits & 4) dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
            zhit = 1; g_as_valid = 0;
        } else if (zhit) { zhit = 0; g_as_valid = 0; }   /* state cache must re-apply for the next draw */
    }
    dev->lpVtbl->BeginScene(dev);
    {   /* c145 probe: same trigger as before the CPU-vertex draw (most world draws take this GPU vertex-program path) */
        static int on = -1; static unsigned first, nlines, last_frame, in_frame; extern int g_d3d8_cullprobe_req; extern unsigned g_d3d8_cullprobe_tag;
        if (on < 0) { const char *e2 = getenv("STE_CULLDRAW"); on = (e2 && *e2 && *e2 != '0'); first = on ? (unsigned)atoi(e2) : 0; }
        if (on && K.frame >= first && (K.frame % 120) == 0 && nlines < 800 && K.cull_enable && count >= 60) {
            if (K.frame != last_frame) { last_frame = K.frame; in_frame = 0; }
            if (in_frame < 100) { ++in_frame; ++nlines; g_d3d8_cullprobe_tag = (K.frame << 8) | in_frame | 0x80u; g_d3d8_cullprobe_req = 1; }
        }
    }
    {   int dr = d3d8_kvp_draw(e->prog, &dd);
        if (!dr) return 0;
        if (dr == 2) { K.stats.draws++; K.stats.vertices += count; K.stats.program_draws++; return 1; }   /* pixel shader still compiling: skipped on purpose */
    }
    g_kelvin_perf[2] += kperf_now() - t0; g_kelvin_perf[3]++;
    K.stats.draws++;
    K.stats.vertices += count;
    K.stats.program_draws++;
    return 1;
}
/* c97: where do draws go? STE_KPATH=1 prints every 600 frames: draws/vertices that took the GPU vertex-program path vs each CPU-interpreter reason. Logging only. */
static unsigned long long g_kp_draws[8], g_kp_verts[8]; static unsigned g_kp_frames;
static void kpath_note(int why, uint32_t count) { g_kp_draws[why]++; g_kp_verts[why] += count; }
static void kpath_report(void)
{
    static int on = -1, every = 600; static const char *nm[8] = { "gpu", "fixedfn", "cpu_vp_env", "small", "debug", "tiny_untex", "gpu_declined", "?" };
    if (on < 0) { const char *e = getenv("STE_KPATH"); on = (e && *e && *e != '0'); if (on && atoi(e) > 1) every = atoi(e); }   /* STE_KPATH=N: report every N frames (default 600) */
    if (!on || ++g_kp_frames < (unsigned)every) return;
    g_kp_frames = 0;
    fprintf(stderr, "[KPATH] tick=%lu frames=%d draws/verts:", (unsigned long)GetTickCount(), every); for (int i = 0; i < 7; ++i) fprintf(stderr, " %s=%llu/%llu", nm[i], g_kp_draws[i], g_kp_verts[i]);
    fprintf(stderr, "\n"); fflush(stderr); memset(g_kp_draws, 0, sizeof g_kp_draws); memset(g_kp_verts, 0, sizeof g_kp_verts);
}
static int g_cullstat_dump_request;   /* c139: set by kelvin_end_frame every STE_CULLSTAT frames */
static void submit(uint32_t count, int (*fetch)(uint32_t i, Vec4 v[16]))
{
    long long _t0, _t1;
    IDirect3DDevice8 *dev = xbox_GetD3DDevice();
    if (!dev || count == 0) return;
    if ((K.color_mask & 0x01010101u) == 0 && !K.depth_mask) return;
    apply_target();   /* bind the (offscreen) surface the title selected, now that its clip is final */
    {   /* c139 probe: STE_CULLSTAT=N prints every N frames (default 600) how many draws/vertices used each NV2A cull setup (enable, CULL_FACE, FRONT_FACE) and how often it changed between draws */
        static int on = -1, every = 600; static unsigned long long nd[2][4][3], nv[2][4][3], toggles, draws; static uint32_t last = 0xFFFFFFFFu;
        if (on < 0) { const char *e = getenv("STE_CULLSTAT"); on = (e && *e && *e != '0'); if (on && atoi(e) > 1) every = atoi(e); }
        if (on) {
            int fi = (K.cull_face == 0x404u) ? 1 : (K.cull_face == 0x405u) ? 2 : (K.cull_face == 0x408u) ? 3 : 0; int ff = (K.front_face == 0x900u) ? 1 : (K.front_face == 0x901u) ? 2 : 0;
            uint32_t sig = (K.cull_enable ? 1u : 0u) | ((uint32_t)fi << 1) | ((uint32_t)ff << 4);
            nd[K.cull_enable ? 1 : 0][fi][ff]++; nv[K.cull_enable ? 1 : 0][fi][ff] += count; draws++; if (sig != last) toggles++; last = sig;
            if (g_cullstat_dump_request) {
                static const char *fn[4] = { "unset", "FRONT", "BACK", "BOTH" }, *fo[3] = { "unset", "CW", "CCW" }; int a, b, c;
                fprintf(stderr, "[CULLSTAT] frames=%d draws=%llu setup changes=%llu\n", every, draws, toggles);
                for (a = 0; a < 2; ++a) for (b = 0; b < 4; ++b) for (c = 0; c < 3; ++c) if (nd[a][b][c])
                    fprintf(stderr, "[CULLSTAT]   cull_enable=%d face=%s front=%s : draws=%llu verts=%llu\n", a, fn[b], fo[c], nd[a][b][c], nv[a][b][c]);
                fflush(stderr); memset(nd, 0, sizeof nd); memset(nv, 0, sizeof nv); toggles = draws = 0; g_cullstat_dump_request = 0;
            }
        }
    }
    d3d8_set_kelvin_vertex_fog(1);
    {   /* debug: KELVIN_SKIP_TEX=<hex offset>[,...] drops draws sampling those textures */
        const char *sk = kenv("KELVIN_SKIP_TEX");
        if (sk) {
            char buf[256]; char *tok, *ctx = NULL;
            strncpy(buf, sk, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
            for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx))
                if ((uint32_t)strtoul(tok, NULL, 16) == K.tex[0].offset) return;
        }
    }
    if (kenv("KELVIN_ONLY_VP") && (!K.program_mode ||
        (uint32_t)atoi(kenv("KELVIN_ONLY_VP")) != K.program_start))
        return;   /* debug: isolate one vertex program's draws */

    /* Expand quads, quad strips and polygons to triangle lists here; the
     * rest map directly onto D3D primitive types. */
    uint32_t prim = K.prim, out_count = count;
    D3DPRIMITIVETYPE d3dprim;
    switch (prim) {
    case 1: d3dprim = D3DPT_POINTLIST; break;
    case 2: d3dprim = D3DPT_LINELIST; break;
    case 3: case 4: d3dprim = D3DPT_LINESTRIP; break;
    case 5: d3dprim = D3DPT_TRIANGLELIST; break;
    case 6: d3dprim = D3DPT_TRIANGLESTRIP; break;
    case 7: case 10: d3dprim = D3DPT_TRIANGLEFAN; break;
    case 8: d3dprim = D3DPT_TRIANGLELIST; out_count = count / 4 * 6; break;
    case 9: d3dprim = D3DPT_TRIANGLELIST; out_count = count >= 4 ? (count - 2) / 2 * 6 : 0; break;
    default: return;
    }
    if (!out_count) return;

    /* D3D's fixed-function/state vertex shader normally materializes the
     * screen-space transform in c96-c99 before these passthrough programs
     * run.  Some titles submit that state immediately before the retained
     * push-buffer range we first observe, leaving a completely empty matrix.
     * Recover the canonical 640x480 pre-transformed-vertex mapping only for
     * that unmistakable all-zero state; any guest-provided matrix wins. */
    if (K.program_mode && (K.program_start == 0 || K.program_start == 17)) {
        int empty = 1;
        for (int r = 96; r <= 99 && empty; ++r)
            for (int c = 0; c < 4; ++c)
                if (K.constants[r][c] != 0.0f) { empty = 0; break; }
        if (empty) {
            static int reported;
            memset(&K.constants[96][0], 0, 4 * 4 * sizeof(float));
            K.constants[96][0] = 1.0f / 320.0f;
            K.constants[96][3] = -1.0f;
            K.constants[97][1] = -1.0f / 240.0f;
            K.constants[97][3] = 1.0f;
            K.constants[99][3] = 1.0f;
            if (!reported++)
                fprintf(stderr, "[KELVIN] recovered missing 640x480 screen transform in c96-c99\n");
        }
    }

    int textured[4] = {0, 0, 0, 0};
    float tex_scale[4][2];
    IDirect3DTexture8 *tex[4] = {NULL, NULL, NULL, NULL};
    _t0 = kperf_now();
    for (int s = 0; s < 4; ++s) {
        tex_scale[s][0] = tex_scale[s][1] = 1.0f;
        tex[s] = get_texture(s);
        if (tex[s]) {
            uint32_t color = (K.tex[s].format >> 8) & 0xFF;
            textured[s] = 1;
            if (tex_is_linear(color)) {
                /* linear textures are addressed in texels */
                uint32_t w, h, pitch; size_t bytes;
                tex_geometry(&K.tex[s], &w, &h, &pitch, &bytes);
                tex_scale[s][0] = 1.0f / (float)w;
                tex_scale[s][1] = 1.0f / (float)h;
            }
        }
    }

    {   /* c127: STE_DRAWLOG_FILE=<path>: when that file exists, log EVERY draw of the next STE_DRAWLOG_FRAMES (default 2) Kelvin frames once (stage textures incl. cube flag, combiner stage
         * programs, blend/depth/alpha state, vertex count); the GPU/CPU vertex path is not changed. Logging only. */
        static int dl_init, dl_done; static const char *dl_file; static uint32_t dl_start, dl_last = ~0u, dl_frames = 2; static int dl_active;
        if (!dl_init) { const char *e = getenv("STE_DRAWLOG_FILE"); const char *n = getenv("STE_DRAWLOG_FRAMES"); dl_init = 1; dl_file = (e && *e) ? e : NULL; if (n && atoi(n) > 0) dl_frames = (uint32_t)atoi(n); }
        if (dl_file && !dl_done) {
            if (!dl_active && dl_last != K.frame) { dl_last = K.frame; if (GetFileAttributesA(dl_file) != INVALID_FILE_ATTRIBUTES) { dl_active = 1; dl_start = K.frame; fprintf(stderr, "[DRAWLOG] start frame=%u\n", K.frame); } }
            if (dl_active && K.frame - dl_start >= dl_frames) { dl_active = 0; dl_done = 1; fprintf(stderr, "[DRAWLOG] end frame=%u\n", K.frame); }
            if (dl_active) {
                fprintf(stderr, "[DRAWLOG] f=%u n=%u prim=%u prog=%d/%u blend=%u %X/%X depth=%u/%X zmask=%u cmask=%08X atest=%u/%X/%02X psh=%08X/%08X cube=%X",
                        K.frame, count, prim, (int)K.program_mode, K.program_start, K.blend_enable, K.sfactor, K.dfactor, K.depth_test, K.depth_func, K.depth_mask, K.color_mask,
                        K.alpha_test, K.alpha_func, K.alpha_ref, K.psh.shader_stage_program, K.psh.other_stage_input, K.psh.cube_mask);
                for (int s = 0; s < 4; ++s) {
                    uint32_t tw = 0, th = 0, tp; size_t tb;
                    if (K.tex[s].offset || (K.tex[s].control0 & (1u << 30))) tex_geometry(&K.tex[s], &tw, &th, &tp, &tb);
                    fprintf(stderr, " t%d[en=%u got=%u off=%08X fmt=%08X %ux%u cube=%u addr=%08X filt=%08X c0=%08X c1=%08X]", s, (K.tex[s].control0 >> 30) & 1u, tex[s] ? 1u : 0u, K.tex[s].offset, K.tex[s].format, tw, th, (K.tex[s].format >> 2) & 1u, K.tex[s].address, K.tex[s].filter, K.tex[s].control0, K.tex[s].control1);
                }
                if (K.psh.shader_stage_program != 1 && K.psh.shader_stage_program != 0 || getenv("STE_DRAWLOG_ALL")) {   /* non-2D pixel shader (cube/dot/...): screen bbox of the vertices + register combiner state */
                    Vec4 v[16]; OutVertex o; float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f; uint32_t nv = count < 4000 ? count : 4000;
                    for (uint32_t i = 0; i < nv; ++i) { if (!fetch(i, v)) continue; transform_vertex(v, &o, tex_scale);
                        if (o.x < x0) x0 = o.x; if (o.x > x1) x1 = o.x; if (o.y < y0) y0 = o.y; if (o.y > y1) y1 = o.y; if (o.z < z0) z0 = o.z; if (o.z > z1) z1 = o.z; }
                    fprintf(stderr, " bbox=[%.1f..%.1f]x[%.1f..%.1f] z=[%.4f..%.4f] ctl=%08X final=%08X/%08X rgb0=%08X/%08X a0=%08X/%08X rgb1=%08X/%08X a1=%08X/%08X f0=%08X f1=%08X",
                            x0, x1, y0, y1, z0, z1, K.psh.combiner_control, K.psh.final0, K.psh.final1, K.psh.rgb_in[0], K.psh.rgb_out[0], K.psh.alpha_in[0], K.psh.alpha_out[0],
                            K.psh.rgb_in[1], K.psh.rgb_out[1], K.psh.alpha_in[1], K.psh.alpha_out[1], K.psh.factor0[0], K.psh.factor1[0]);
                }
                if (getenv("STE_DRAWLOG_VERTS") && K.tex[0].offset == (uint32_t)strtoul(getenv("STE_DRAWLOG_VERTS"), NULL, 16)) {   /* c137 probe: every vertex (post-VP screen x/y/z, tex0 s/t, diffuse) of the draws that sample that texture, tab separated */
                    Vec4 v[16]; OutVertex o; uint32_t nv = count < 6000 ? count : 6000;
                    for (uint32_t i = 0; i < nv; ++i) { if (!fetch(i, v)) continue; transform_vertex(v, &o, tex_scale);
                        fprintf(stderr, "\n[DRAWV] %u\t%.2f\t%.2f\t%.5f\t%.4f\t%.4f\t%08X\t%.3f", i, o.x, o.y, o.z, o.tex[0][0], o.tex[0][1], o.diffuse, o.fogv[0]); }
                }
                if (count >= 1000 && getenv("STE_DRAWLOG_ALL")) {   /* c129: diffuse/texcoord ranges over the whole strip + 6 sample vertices */
                    Vec4 v[16]; OutVertex o; uint32_t nv = count < 4000 ? count : 4000; float s0 = 1e30f, s1 = -1e30f, t0 = 1e30f, t1 = -1e30f; uint32_t a0 = 255, a1 = 0, c0 = 255, c1 = 0;
                    for (uint32_t i = 0; i < nv; ++i) { if (!fetch(i, v)) continue; transform_vertex(v, &o, tex_scale);
                        if (o.tex[0][0] < s0) s0 = o.tex[0][0]; if (o.tex[0][0] > s1) s1 = o.tex[0][0]; if (o.tex[0][1] < t0) t0 = o.tex[0][1]; if (o.tex[0][1] > t1) t1 = o.tex[0][1];
                        uint32_t a = o.diffuse >> 24, c = o.diffuse & 255; if (a < a0) a0 = a; if (a > a1) a1 = a; if (c < c0) c0 = c; if (c > c1) c1 = c;
                        if (i < 6 || (i >= 700 && i < 704)) fprintf(stderr, "\n[DRAWLOG]   v%u screen=(%.1f,%.1f) z=%.4f diff=%08X spec=%08X tex0=(%.4f,%.4f)", i, o.x, o.y, o.z, o.diffuse, o.specular, o.tex[0][0], o.tex[0][1]); }
                    fprintf(stderr, "\n[DRAWLOG]   ranges: tex0 s=[%.3f..%.3f] t=[%.3f..%.3f] diffA=[%u..%u] diffB=[%u..%u]", s0, s1, t0, t1, a0, a1, c0, c1);
                }
                fprintf(stderr, "\n");
            }
        }
    }
    {   /* c207: STE_POPLOG=<file> (binary, 48-byte records, one per depth-tested draw): frame, mesh key (FNV of count/prim/tex0/program + object-space position of 3 vertices),
         * min/mean clip-W (view depth) of up to 24 sampled vertices, screen bbox, blend/alpha flags. STE_POPLOG_FROM / STE_POPLOG_FRAMES bound the window (default 0 / 3000 Kelvin frames).
         * Logging only; tools\poplog_analyze.py pairs instances across frames. */
        static int pl_init; static FILE *pl_f; static uint32_t pl_from, pl_frames = 3000;
        if (!pl_init) { const char *e = getenv("STE_POPLOG"); const char *a = getenv("STE_POPLOG_FROM"); const char *n = getenv("STE_POPLOG_FRAMES"); pl_init = 1;
            if (a) pl_from = (uint32_t)atoi(a); if (n && atoi(n) > 0) pl_frames = (uint32_t)atoi(n);
            if (e && *e) { pl_f = fopen(e, "wb"); fprintf(stderr, "[POPLOG] %s from=%u frames=%u f=%p\n", e, pl_from, pl_frames, (void *)pl_f); } }
        if (pl_f && K.frame >= pl_from && K.frame - pl_from < pl_frames && K.depth_test && count >= 3) {
            {   /* c212: VP constant rows 96..99 (the 640x480 view-projection used by world-space scenery) whenever they change: two pseudo records (key 0xFFFFFF01/02, 8 floats each in count..y1) */
                static float pl_last[4][4]; static int pl_have;
                if (!pl_have || memcmp(K.constants[96], pl_last, sizeof pl_last)) {
                    struct { uint32_t frame, key; float m[8]; uint32_t flags, na; } r2; memcpy(pl_last, K.constants[96], sizeof pl_last); pl_have = 1;
                    for (int part = 0; part < 2; ++part) { r2.frame = K.frame; r2.key = 0xFFFFFF01u + (uint32_t)part; memcpy(r2.m, K.constants[96 + part * 2], 8 * sizeof(float)); r2.flags = 0; r2.na = 0; fwrite(&r2, sizeof r2, 1, pl_f); }
                }
            }
            {   /* c214: tag record (key 0xFFFFFF03, 8 floats) whenever the game side (debug free cam in recomp_manual.c) bumps g_ste_poplog_tag_ver: eye xyz, yaw, pitch, phase, frozen, spare */
                static uint32_t pl_tagver;
                if (g_ste_poplog_tag_ver != pl_tagver) {
                    struct { uint32_t frame, key; float m[8]; uint32_t flags, na; } r3; pl_tagver = g_ste_poplog_tag_ver;
                    r3.frame = K.frame; r3.key = 0xFFFFFF03u; memcpy(r3.m, g_ste_poplog_tag, sizeof r3.m); r3.flags = 0; r3.na = 0; fwrite(&r3, sizeof r3, 1, pl_f);
                }
            }
            Vec4 v[16]; OutVertex o; uint32_t h = 2166136261u; float wmin = 1e30f, wsum = 0.0f, x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f; uint32_t nw = 0, na = 0, asum = 0;
            uint32_t step = count / 24; if (step < 1) step = 1;
            #define PL_MIX(x) do { h ^= (uint32_t)(x); h *= 16777619u; } while (0)
            PL_MIX(count); PL_MIX(prim); PL_MIX(K.tex[0].offset); PL_MIX(K.program_start);
            for (uint32_t k = 0; k < 3; ++k) { uint32_t i = k == 0 ? 0 : (k == 1 ? count / 2 : count - 1); if (fetch(i, v)) { uint32_t b[3]; memcpy(b, v[0].v, sizeof b); PL_MIX(b[0]); PL_MIX(b[1]); PL_MIX(b[2]); } }
            for (uint32_t i = 0; i < count; i += step) { if (!fetch(i, v)) continue; transform_vertex(v, &o, tex_scale);
                if (o.rhw > 0.0f) { float w = 1.0f / o.rhw; if (w < wmin) wmin = w; wsum += w; ++nw;
                    if (o.x < x0) x0 = o.x; if (o.x > x1) x1 = o.x; if (o.y < y0) y0 = o.y; if (o.y > y1) y1 = o.y; }
                asum += o.diffuse >> 24; ++na; }
            #undef PL_MIX
            uint32_t pl_raw = h;   /* c210: h (raw object-space positions) is kept as the secondary id; the primary key is the guest address of the position stream's first/last vertex (stable for static meshes, 0 for inline data) */
            {   uint32_t i0 = 0, i1 = 0, ad0 = 0, ad1 = 0; int ok = 1;
                if (fetch == fetch_indexed_cb) { i0 = K.indices[0]; i1 = K.indices[count - 1]; } else if (fetch == fetch_arrays_cb) { i0 = K.array_start; i1 = K.array_start + count - 1; } else ok = 0;
                if (ok) { ad0 = K.attr_offset[0] + i0 * attr_stride(0); ad1 = K.attr_offset[0] + i1 * attr_stride(0); }
                h = 2166136261u; h ^= count; h *= 16777619u; h ^= (uint32_t)prim; h *= 16777619u; h ^= K.tex[0].offset; h *= 16777619u; h ^= ad0; h *= 16777619u; h ^= ad1; h *= 16777619u; h ^= (uint32_t)ok; h *= 16777619u; }
            {   /* c207b: STE_POPLOG_CONST=<frame>: print the VP constant rows of the first 6 draws from that Kelvin frame (find the per-object world matrix rows) */
                static int pc_init; static uint32_t pc_frame; static int pc_n; if (!pc_init) { const char *e = getenv("STE_POPLOG_CONST"); pc_init = 1; pc_frame = e ? (uint32_t)atoi(e) : 0; }
                static float pc_prev[192][4];
                if (pc_frame && K.frame >= pc_frame && pc_n < 700 && nw && K.frame < pc_frame + 2) { ++pc_n; fprintf(stderr, "[POPCONST] f=%u draw=%d key=%08X count=%u prog=%u W=%.0f changed rows:", K.frame, pc_n, h, count, K.program_start, wmin);
                    for (int row = 96; row < 192; ++row) { const float *c = K.constants[row]; if (memcmp(c, pc_prev[row], 16)) { fprintf(stderr, " c%d(%.2f %.2f %.2f %.2f)", row, c[0], c[1], c[2], c[3]); memcpy(pc_prev[row], c, 16); } }
                    fprintf(stderr, "\n"); }
            }
            if (nw) { struct { uint32_t frame, key, count, tex0; float wmin, wmean, x0, y0, x1, y1; uint32_t flags, na; } r;
                r.frame = K.frame; r.key = h; r.count = count; r.tex0 = K.tex[0].offset; r.wmin = wmin; r.wmean = wsum / (float)nw; r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = y1;
                r.flags = (K.blend_enable ? 1u : 0u) | (K.depth_mask ? 2u : 0u) | (K.alpha_test ? 4u : 0u) | (K.program_mode ? 8u : 0u) | ((na ? asum / na : 0u) << 8); r.na = pl_raw;
                fwrite(&r, sizeof r, 1, pl_f); }
            if ((K.frame % 120) == 0) fflush(pl_f);
        }
    }
    {   /* c100: STE_TEXTRACE="WxH[,WxH...]" logs the draws that sample a texture of that size (any format): first 6 per texture in full, then a count line every 600 frames.
         * Used to find which draws render the front-end glow (GFC_Globe_Halo 16x16 DXT3), bubbles (32x32) and static (256x64). Logging only. */
        static int on = -1; static unsigned want[8][2]; static int nwant;
        if (on < 0) { const char *e = getenv("STE_TEXTRACE"); on = (e && *e && *e != '0');
            if (on) { const char *p = e; while (*p && nwant < 8) { unsigned a = 0, b = 0; if (sscanf(p, "%ux%u", &a, &b) == 2) { want[nwant][0] = a; want[nwant][1] = b; nwant++; } while (*p && *p != ',') p++; if (*p == ',') p++; }
                fprintf(stderr, "[TEXTRACE] watching %d sizes\n", nwant); } }
        if (on) for (int s = 0; s < 4; ++s) if (tex[s] && !d3d8_rt_texture(K.tex[s].offset & 0x03FFFFFFu)) {
            uint32_t tw, th, tp; size_t tb; int hit = 0;
            tex_geometry(&K.tex[s], &tw, &th, &tp, &tb);
            for (int i = 0; i < nwant; ++i) if (tw == want[i][0] && th == want[i][1]) hit = 1;
            if (nwant == 0) hit = 1;   /* c103: STE_TEXTRACE=all traces every sampled texture */
            if (hit) {
                int k; static struct { uint32_t off, fmt, w, h; unsigned long n; } seen[256]; static int ns;
                for (k = 0; k < ns; ++k) if (seen[k].off == K.tex[s].offset && seen[k].fmt == K.tex[s].format) break;
                if (k == ns && ns < 256) { seen[ns].off = K.tex[s].offset; seen[ns].fmt = K.tex[s].format; seen[ns].w = tw; seen[ns].h = th; seen[ns].n = 0; ns++; }
                if (k < ns) {
                    seen[k].n++;
                    if (seen[k].n == 1 && getenv("STE_TEXDUMP_DIR")) {   /* c105: BMP of the decoded texture (first sight) */
                        D3DLOCKED_RECT lr; char path[512];
                        if (tex[s]->lpVtbl->LockRect(tex[s], 0, &lr, NULL, 0) == 0 && lr.pBits) {
                            FILE *bf; snprintf(path, sizeof path, "%s\\tex_%08X_%ux%u_fmt%08X.bmp", getenv("STE_TEXDUMP_DIR"), K.tex[s].offset, tw, th, K.tex[s].format);
                            bf = fopen(path, "wb");
                            if (bf) { BITMAPFILEHEADER fh = {0}; BITMAPINFOHEADER ih = {0}; fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + tw * th * 4;
                                ih.biSize = sizeof ih; ih.biWidth = (LONG)tw; ih.biHeight = -(LONG)th; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
                                fwrite(&fh, sizeof fh, 1, bf); fwrite(&ih, sizeof ih, 1, bf);
                                for (uint32_t yy = 0; yy < th; ++yy) fwrite((uint8_t *)lr.pBits + (size_t)yy * lr.Pitch, tw * 4, 1, bf);
                                fclose(bf); }
                            tex[s]->lpVtbl->UnlockRect(tex[s], 0);
                        }
                    }
                    if (seen[k].n <= 6 || (seen[k].n % 2000) == 0) {
                        Vec4 v[16]; OutVertex o; float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f; uint32_t nv = count < 16 ? count : 16;
                        for (uint32_t i = 0; i < nv; ++i) { if (!fetch(i, v)) continue; transform_vertex(v, &o, tex_scale); if (i < (seen[k].n == 1 ? 16u : 4u) && seen[k].n <= 6) fprintf(stderr, "[TEXTRACE]   v%u screen=(%.1f,%.1f) z=%.4f w=%.4f rhw=%g diff=%08X spec=%08X tex0=(%g,%g)\n", i, o.x, o.y, o.z, o.rhw != 0.0f ? 1.0f / o.rhw : 0.0f, o.rhw, o.diffuse, o.specular, o.tex[0][0], o.tex[0][1]);
                            if (o.x < x0) x0 = o.x; if (o.x > x1) x1 = o.x; if (o.y < y0) y0 = o.y; if (o.y > y1) y1 = o.y; }
                        fprintf(stderr, "[TEXTRACE] frame=%u n=%lu stage=%d tex off=%08X fmt=%08X %ux%u verts=%u prim=%u prog=%d start=%u blend=%u %X/%X depth=%u/%X zmask=%u "
                                "c58=(%g,%g) c96=(%g,%g) c97=(%g,%g) c99w=%g bbox(first %u verts)=[%.1f..%.1f]x[%.1f..%.1f] psh_stages=%08X\n",
                                K.frame, seen[k].n, s, K.tex[s].offset, K.tex[s].format, tw, th, count, prim, (int)K.program_mode, K.program_start, K.blend_enable, K.sfactor, K.dfactor,
                                K.depth_test, K.depth_func, K.depth_mask, K.constants[58][0], K.constants[58][1], K.constants[96][0], K.constants[96][3], K.constants[97][0], K.constants[97][1],
                                K.constants[99][3], nv, x0, x1, y0, y1, K.psh.shader_stage_program);
                    }
                }
            }
        }
    }    _t1 = kperf_now(); g_kelvin_perf[0] += _t1 - _t0; _t0 = _t1;
    {   static uint32_t gmin = 0; int why = 0;
        if (!gmin) { const char *e = kenv("KELVIN_GPU_VP_MIN"); gmin = e ? (uint32_t)atoi(e) : 1u; if (!gmin) gmin = 1; }   /* c104: default 1 (was 48): draws under 48 vertices ran the CPU vertex-program interpreter on the render thread (~400 draws/frame in a Tokyo fight, ~4 ms); KELVIN_GPU_VP_MIN=48 restores the old threshold */
        if (!K.program_mode) why = 1;                                     /* fixed-function / no vertex program */
        else if (!gpu_vp_mode()) why = 2;                                 /* KELVIN_CPU_VP */
        else if (count < gmin) why = 3;                                   /* below KELVIN_GPU_VP_MIN (default 48 vertices) */
        else if (g_rhw_legacy || g_kelvin_log_draws || kenv("KELVIN_DUMP_VP") || kenv("KELVIN_LOG_FRAME")) why = 4;   /* debug / legacy */
        else if (!textured[0] && count <= 16) why = 5;                    /* tiny untextured draw */
        if (!why) {
            if (gpu_vp_draw(dev, count, fetch, prim, textured, tex, tex_scale)) { kpath_note(0, count); return; }
            why = 6;                                                      /* gpu_vp_draw declined (program still compiling / failed / unsupported prim or fetch) */
        }
        kpath_note(why, count);
    }
    OutVertex *src = reserve_verts(count + out_count);
    if (!src) return;
    OutVertex *dst = src + count;
    Vec4 v[16];
    if (fetch == fetch_indexed_cb && count >= 16 && !g_kelvin_log_draws && !kenv("KELVIN_DUMP_VP") &&
        xf_indexed_unique(count, src, tex_scale)) {
        /* indexed: each distinct vertex transformed once, then copied per index */
    } else
    if (count >= xf_par_min() && !g_kelvin_log_draws && !kenv("KELVIN_DUMP_VP") &&
        xf_parallel(count, fetch, src, (const float (*)[2])tex_scale)) {
        /* transformed by the worker pool */
    } else
    for (uint32_t i = 0; i < count; ++i) {
        if (!fetch(i, v)) memset(v, 0, sizeof(v));
        transform_vertex(v, &src[i], tex_scale);
        if (g_kelvin_log_draws && (src[i].x > 1e4f || src[i].x < -1e4f ||
                                   src[i].y > 1e4f || src[i].y < -1e4f)) {
            /* exploding vertex: show what the program was fed */
            static int shown;
            if (shown++ < 24)
                fprintf(stderr, "[HUGE] draw#%u vtx %u v0=(%g,%g,%g,%g) v3=(%g,%g,%g,%g) fmt0=%08X off0=%08X "
                        "out=(%g,%g,%g rhw=%g) c96=(%g,%g,%g,%g) c99=(%g,%g,%g,%g)\n",
                        K.stats.draws + 1, i, v[0].v[0], v[0].v[1], v[0].v[2], v[0].v[3],
                        v[3].v[0], v[3].v[1], v[3].v[2], v[3].v[3], K.attr_format[0], K.attr_offset[0],
                        src[i].x, src[i].y, src[i].z, src[i].rhw,
                        K.constants[96][0], K.constants[96][1], K.constants[96][2], K.constants[96][3],
                        K.constants[99][0], K.constants[99][1], K.constants[99][2], K.constants[99][3]);
        }
        if (i < 96 && K.program_mode && kenv("KELVIN_DUMP_VP") &&
            (uint32_t)atoi(kenv("KELVIN_DUMP_VP")) == K.program_start) {
            /* one-shot dump of a vertex program, its inputs and outputs */
            static int dumped;
            static uint32_t dump_draw;
            if (dumped && dump_draw == K.stats.draws && i < 96) {
                VshState s;
                vsh_run(v, &s);
                fprintf(stderr, "[VP-VTX] %u v0=(%.3f,%.3f,%.3f) v1=(%g,%g,%g,%g) v2=(%g,%g) o0=(%.1f,%.1f,%.4g,%.4g)\n", i,
                        v[0].v[0], v[0].v[1], v[0].v[2], v[1].v[0], v[1].v[1], v[1].v[2], v[1].v[3],
                        v[2].v[0], v[2].v[1], s.o[0].v[0], s.o[0].v[1], s.o[0].v[2], s.o[0].v[3]);
            }
            if (!dumped++) {
                VshState s;
                dump_draw = K.stats.draws;
                vsh_run(v, &s);
                for (int r = 0; r < CONST_ROWS; ++r)
                    if (K.constants[r][0] != 0.0f || K.constants[r][1] != 0.0f || K.constants[r][2] != 0.0f || K.constants[r][3] != 0.0f)
                        fprintf(stderr, "[VP-CONST] c%d = (%g, %g, %g, %g)\n", r,
                                K.constants[r][0], K.constants[r][1], K.constants[r][2], K.constants[r][3]);
                fprintf(stderr, "[VP-DUMP] start=%u count=%u prim=%u\n", K.program_start, count, K.prim);
                for (int a = 0; a < 16; ++a)
                    fprintf(stderr, "[VP-DUMP] v%d fmt=%08X off=%08X = (%g, %g, %g, %g)\n", a,
                            K.attr_format[a], K.attr_offset[a], v[a].v[0], v[a].v[1], v[a].v[2], v[a].v[3]);
                for (uint32_t sl = K.program_start; sl < PROGRAM_SLOTS; ++sl) {
                    fprintf(stderr, "[VP-DUMP] slot %u: %08X %08X %08X %08X\n", sl, K.program[sl][0],
                            K.program[sl][1], K.program[sl][2], K.program[sl][3]);
                    if (K.program[sl][3] & 1) break;
                }
                for (int o = 0; o < 13; ++o)
                    fprintf(stderr, "[VP-DUMP] o%d = (%g, %g, %g, %g)\n", o,
                            s.o[o].v[0], s.o[o].v[1], s.o[o].v[2], s.o[o].v[3]);
                for (int r = 0; r < 12; ++r)
                    fprintf(stderr, "[VP-DUMP] r%d = (%g, %g, %g, %g)\n", r,
                            s.r[r].v[0], s.r[r].v[1], s.r[r].v[2], s.r[r].v[3]);
            }
        }
        if (i == 0 && K.stats.draws < 3) {
            uint32_t ps = K.program_start;
            fprintf(stderr, "[KELVIN] dbg start=%u load=%u slot=%08X %08X %08X %08X "
                    "v0=(%g,%g,%g,%g) v9=(%g,%g) c96=(%g,%g,%g,%g) fmt0=%08X\n",
                    ps, K.program_load, K.program[ps][0], K.program[ps][1],
                    K.program[ps][2], K.program[ps][3], v[0].v[0], v[0].v[1],
                    v[0].v[2], v[0].v[3], v[9].v[0], v[9].v[1], K.constants[96][0],
                    K.constants[96][1], K.constants[96][2], K.constants[96][3],
                    K.attr_format[0]);
        }
    }
    const OutVertex *draw = src;
    if (prim == 8) {
        for (uint32_t q = 0; q < count / 4; ++q) {
            const OutVertex *a = &src[q * 4];
            dst[q * 6 + 0] = a[0]; dst[q * 6 + 1] = a[1]; dst[q * 6 + 2] = a[2];
            dst[q * 6 + 3] = a[0]; dst[q * 6 + 4] = a[2]; dst[q * 6 + 5] = a[3];
        }
        draw = dst;
    } else if (prim == 9) {
        uint32_t n = 0;
        for (uint32_t i = 0; i + 3 < count; i += 2) {
            dst[n++] = src[i]; dst[n++] = src[i + 1]; dst[n++] = src[i + 3];
            dst[n++] = src[i]; dst[n++] = src[i + 3]; dst[n++] = src[i + 2];
        }
        draw = dst;
    }

    UINT prims;
    switch (d3dprim) {
    case D3DPT_POINTLIST: prims = out_count; break;
    case D3DPT_LINELIST: prims = out_count / 2; break;
    case D3DPT_LINESTRIP: prims = out_count - 1; break;
    case D3DPT_TRIANGLELIST: prims = out_count / 3; break;
    default: prims = out_count >= 3 ? out_count - 2 : 0; break;
    }
    if (!prims) return;

    _t1 = kperf_now(); g_kelvin_perf[1] += _t1 - _t0; _t0 = _t1;
    setup_draw_state(dev, textured, tex);
    {
        int fs = 0;
        if ((!textured[0] || g_fsq_lim() > 0 || g_rtp_on()) && count <= 64) {
            float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
            for (uint32_t i = 0; i < count; ++i) {
                if (src[i].x < x0) x0 = src[i].x; if (src[i].x > x1) x1 = src[i].x;
                if (src[i].y < y0) y0 = src[i].y; if (src[i].y > y1) y1 = src[i].y;
            }
            if (!textured[0] && count <= 16) fs = x0 <= 4.0f && x1 >= 636.0f && y0 <= 4.0f && y1 >= 476.0f;
            if (g_fsq_lim() > 0 || g_rtp_on()) fsquad_probe("cpu", count, prim, x0, x1, y0, y1, src[0].diffuse, textured[0] ? K.tex[0].offset : 0);
        }
        kelvin_apply_xmap(0, fs);
    }
    dev->lpVtbl->BeginScene(dev);
    {   /* c145 probe: STE_CULLDRAW=<first frame> counts, for every culled draw (>= 60 vertices) of every 120th frame, 800 draws in total, what the cull mode removes (see cullprobe_draw in d3d8_device.c) */
        static int on = -1; static unsigned first, nlines, last_frame, in_frame; extern int g_d3d8_cullprobe_req; extern unsigned g_d3d8_cullprobe_tag;
        if (on < 0) { const char *e = getenv("STE_CULLDRAW"); on = (e && *e && *e != '0'); first = on ? (unsigned)atoi(e) : 0; }
        if (on && K.frame >= first && (K.frame % 120) == 0 && nlines < 800 && K.cull_enable && count >= 60 && (d3dprim == D3DPT_TRIANGLELIST || d3dprim == D3DPT_TRIANGLESTRIP)) {
            if (K.frame != last_frame) { last_frame = K.frame; in_frame = 0; }
            if (in_frame < 100) { ++in_frame; ++nlines; g_d3d8_cullprobe_tag = (K.frame << 8) | in_frame; g_d3d8_cullprobe_req = 1; }
        }
    }
    dev->lpVtbl->DrawPrimitiveUP(dev, d3dprim, prims, draw, sizeof(OutVertex));
    g_kelvin_perf[2] += kperf_now() - _t0; g_kelvin_perf[3]++;

    K.stats.draws++;
    K.stats.vertices += count;
    if (K.program_mode) K.stats.program_draws++; else K.stats.ff_draws++;

    if (g_kelvin_log_draws) {
        /* per-draw log for scene analysis: screen bbox, depth range, state */
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
        for (uint32_t i = 0; i < count; ++i) {
            if (src[i].x < x0) x0 = src[i].x; if (src[i].x > x1) x1 = src[i].x;
            if (src[i].y < y0) y0 = src[i].y; if (src[i].y > y1) y1 = src[i].y;
            if (src[i].z < z0) z0 = src[i].z; if (src[i].z > z1) z1 = src[i].z;
        }
        fprintf(stderr, "[DRAW] #%u prim=%u n=%u %s vp=%u box=(%.0f..%.0f,%.0f..%.0f) z=%.3f..%.3f "
                "c0=%08X tex=%08X/%08X,%08X/%08X blend=%u %X/%X atest=%u depth=%u/%u cmask=%08X src=%s a0=%08X/%08X\n",
                K.stats.draws, prim, count, K.program_mode ? "P" : "F", K.program_start,
                x0, x1, y0, y1, z0, z1, src[0].diffuse,
                textured[0] ? K.tex[0].offset : 0, textured[0] ? K.tex[0].format : 0,
                textured[1] ? K.tex[1].offset : 0, textured[1] ? K.tex[1].format : 0,
                K.blend_enable, K.sfactor, K.dfactor, K.alpha_test, K.depth_test,
                K.depth_mask, K.color_mask,
                K.inline_count ? (g_inline_in_call ? "inline-call" : "inline") : K.index_count ? "indexed" : "array",
                K.inline_count ? g_inline_src : K.attr_offset[0], K.attr_format[0]);
        fprintf(stderr, "[PSH] vp=%u ctl=%08X stage=%08X other=%08X cube=%X final=%08X/%08X "
                "tex2=%08X/%08X tex3=%08X/%08X "
                "rgb0=%08X/%08X alpha0=%08X/%08X rgb1=%08X/%08X alpha1=%08X/%08X\n",
                K.program_mode ? K.program_start : 0, K.psh.combiner_control,
                K.psh.shader_stage_program, K.psh.other_stage_input, K.psh.cube_mask,
                K.psh.final0, K.psh.final1,
                textured[2] ? K.tex[2].offset : 0, textured[2] ? K.tex[2].format : 0,
                textured[3] ? K.tex[3].offset : 0, textured[3] ? K.tex[3].format : 0,
                K.psh.rgb_in[0], K.psh.rgb_out[0], K.psh.alpha_in[0], K.psh.alpha_out[0],
                K.psh.rgb_in[1], K.psh.rgb_out[1], K.psh.alpha_in[1], K.psh.alpha_out[1]);
        fprintf(stderr, "[PSH-FACTORS] f0=%08X,%08X,%08X,%08X,%08X "
                "f1=%08X,%08X,%08X,%08X,%08X final=%08X/%08X\n",
                K.psh.factor0[0], K.psh.factor0[1], K.psh.factor0[2],
                K.psh.factor0[3], K.psh.factor0[4], K.psh.factor1[0],
                K.psh.factor1[1], K.psh.factor1[2], K.psh.factor1[3],
                K.psh.factor1[4], K.psh.final_factor0, K.psh.final_factor1);
    }

    {   /* STE diagnostic: KELVIN_LOG_FRAME=<n> logs every draw of kelvin frames n..n+4 with a content hash of the stage-0 texture bytes */
        const char *lf = kenv("KELVIN_LOG_FRAME");
        if (lf && K.frame >= (uint32_t)atoi(lf) && K.frame < (uint32_t)atoi(lf) + 5) {
            uint32_t tw, th, tp; size_t tb; const uint8_t *tp8 = guest_ptr(K.tex[0].offset);
            tex_geometry(&K.tex[0], &tw, &th, &tp, &tb);
            fprintf(stderr, "[KELVIN-FRAME] kf=%u draw prim=%u verts=%u tex0=%08X fmt=%08X %ux%u pitch=%u bytes=%zu nzwords=%u/%zu hash=%016llX v0=(%.1f,%.1f) uv0=(%.2f,%.2f)\n",
                    K.frame, prim, count, K.tex[0].offset, K.tex[0].format, tw, th, tp, tb, ste_nz_words(tp8, tb), tb / 4,
                    (unsigned long long)((tb && tb < (64u << 20)) ? hash_bytes(tp8, tb) : 0), src[0].x, src[0].y, src[0].tex[0][0], src[0].tex[0][1]);
        }
    }
    if (K.stats.draws <= 12) {
        fprintf(stderr, "[KELVIN] draw #%u prim=%u verts=%u %s tex0=%08X fmt=%08X "
                "v0=(%.1f,%.1f,%.3f,%.4f) c=%08X uv=(%.3f,%.3f)\n",
                K.stats.draws, prim, count, K.program_mode ? "program" : "fixed",
                K.tex[0].offset, K.tex[0].format, src[0].x, src[0].y, src[0].z, src[0].rhw,
                src[0].diffuse, src[0].tex[0][0], src[0].tex[0][1]);
        fprintf(stderr, "[KELVIN]   blend=%u %X/%X eq=%X atest=%u func=%X ref=%X "
                "depth=%u func=%X mask=%u cmask=%08X clip=%g\n",
                K.blend_enable, K.sfactor, K.dfactor, K.blend_eq, K.alpha_test,
                K.alpha_func, K.alpha_ref, K.depth_test, K.depth_func, K.depth_mask,
                K.color_mask, K.clip_max);
    }
}

static int fetch_inline_cb(uint32_t i, Vec4 v[16]) { fetch_inline(i, v); return 1; }
static int fetch_indexed_cb(uint32_t i, Vec4 v[16]) { fetch_array(K.indices[i], v); return 1; }
static int fetch_arrays_cb(uint32_t i, Vec4 v[16]) { fetch_array(K.array_start + i, v); return 1; }

static void end_primitive(void)
{
    if (K.inline_count) {
        uint32_t stride = inline_vertex_dwords();
        if (stride) submit(K.inline_count / stride, fetch_inline_cb);
    } else if (K.index_count) {
        submit(K.index_count, fetch_indexed_cb);
    } else if (K.array_count) {
        submit(K.array_count, fetch_arrays_cb);
    }
    K.inline_count = 0;
    K.index_count = 0;
    K.array_count = 0;
    K.have_arrays = 0;
}

static void push_index(uint32_t idx)
{
    if (K.index_count >= K.index_cap) {
        uint32_t cap = K.index_cap ? K.index_cap * 2 : 4096;
        uint32_t *p = (uint32_t *)realloc(K.indices, cap * sizeof(uint32_t));
        if (!p) return;
        K.indices = p; K.index_cap = cap;
    }
    K.indices[K.index_count++] = idx;
}

/* ------------------------------------------------------------------ */
/* Method dispatch                                                     */
/* ------------------------------------------------------------------ */
/* pushbuffer address of the word being executed (set by the host walker) */
uint32_t g_kelvin_pb_addr, g_kelvin_pb_in_call;

int kelvin_method(uint32_t subchannel, uint32_t method, uint32_t param)
{
    (void)subchannel;
    if (!K.initialized) return 0;

    /* c80: the two per-index methods of every indexed draw come first (they used to sit behind ~25 range checks) */
    if (method == M_ARRAY_ELEMENT16) { push_index(param & 0xFFFF); push_index(param >> 16); return 1; }
    if (method == M_ARRAY_ELEMENT32) { push_index(param); return 1; }

    if (method == M_CONTEXT_DMA_A || method == M_CONTEXT_DMA_B) {
        uint32_t *slot = method == M_CONTEXT_DMA_A ? &K.dma_a : &K.dma_b;
        if (*slot != param)
            fprintf(stderr, "[KELVIN-DMA] %c handle=%08X pb=%08X\n",
                    method == M_CONTEXT_DMA_A ? 'A' : 'B', param, g_kelvin_pb_addr);
        *slot = param;
        return 1;
    }

    if (method >= M_TRANSFORM_DATA && method < M_TRANSFORM_DATA + 0x10) {
        K.transform_data[(method - M_TRANSFORM_DATA) / 4] = u2f(param);
        return 1;
    }
    if (method >= M_TRANSFORM_PROGRAM && method < M_TRANSFORM_PROGRAM + 0x80) {
        uint32_t slot = (method - M_TRANSFORM_PROGRAM) / 4;
        if (K.program_load < PROGRAM_SLOTS) {
            K.program[K.program_load][slot % 4] = param;
            g_vdec_ok[K.program_load] = 0;
            g_prog_gen++;
        }
        if (slot % 4 == 3) K.program_load++;
        return 1;
    }
    if (method >= M_TRANSFORM_CONSTANT && method < M_TRANSFORM_CONSTANT + 0x80) {
        uint32_t slot = (method - M_TRANSFORM_CONSTANT) / 4;
        if (K.const_load < CONST_ROWS) K.constants[K.const_load][slot % 4] = u2f(param);
        if (slot % 4 == 3) K.const_load++;
        return 1;
    }
    if (method >= M_COMPOSITE_MATRIX && method < M_COMPOSITE_MATRIX + 0x40) {
        uint32_t slot = (method - M_COMPOSITE_MATRIX) / 4;
        K.composite[slot / 4][slot % 4] = u2f(param); /* row n/4, column n%4 */
        K.constants[0 + slot / 4][slot % 4] = u2f(param); /* XFCTX_CMAT0 */
        return 1;
    }
    if (method >= M_VIEWPORT_OFFSET && method < M_VIEWPORT_OFFSET + 0x10) {
        K.viewport_offset[(method - M_VIEWPORT_OFFSET) / 4] = u2f(param);
        K.constants[0x3B][(method - M_VIEWPORT_OFFSET) / 4] = u2f(param); /* XFCTX_VPOFF */
        return 1;
    }
    if (method >= M_VIEWPORT_SCALE && method < M_VIEWPORT_SCALE + 0x10) {
        K.viewport_scale[(method - M_VIEWPORT_SCALE) / 4] = u2f(param);
        K.constants[0x3A][(method - M_VIEWPORT_SCALE) / 4] = u2f(param); /* XFCTX_VPSCL */
        return 1;
    }
    if (method >= M_VERTEX_ARRAY_OFFSET && method < M_VERTEX_ARRAY_OFFSET + 0x40) {
        K.attr_offset[(method - M_VERTEX_ARRAY_OFFSET) / 4] = param & 0x7FFFFFFFu;
        return 1;
    }
    if (method >= M_VERTEX_ARRAY_FORMAT && method < M_VERTEX_ARRAY_FORMAT + 0x40) {
        K.attr_format[(method - M_VERTEX_ARRAY_FORMAT) / 4] = param;
        return 1;
    }
    if (method >= M_VERTEX_DATA4UB && method < M_VERTEX_DATA4UB + 0x40) {
        int a = (method - M_VERTEX_DATA4UB) / 4;
        K.attr_const[a][0] = (param & 0xFF) / 255.0f;
        K.attr_const[a][1] = ((param >> 8) & 0xFF) / 255.0f;
        K.attr_const[a][2] = ((param >> 16) & 0xFF) / 255.0f;
        K.attr_const[a][3] = ((param >> 24) & 0xFF) / 255.0f;
        return 1;
    }
    if (method >= M_VERTEX_DATA4F && method < M_VERTEX_DATA4F + 0x100) {
        uint32_t i = (method - M_VERTEX_DATA4F) / 4;
        K.attr_const[i / 4][i % 4] = u2f(param);
        return 1;
    }
    if (method >= M_VERTEX_DATA2F && method < M_VERTEX_DATA2F + 0x80) {
        uint32_t i = (method - M_VERTEX_DATA2F) / 4;
        K.attr_const[i / 2][i % 2] = u2f(param);
        K.attr_const[i / 2][2] = 0.0f; K.attr_const[i / 2][3] = 1.0f;
        return 1;
    }
    if (method >= M_TEXTURE_BASE && method < M_TEXTURE_BASE + 0x100) {
        uint32_t stage = (method - M_TEXTURE_BASE) / 0x40, reg = (method - M_TEXTURE_BASE) % 0x40;
        TexStage *t = &K.tex[stage];
        switch (reg) {
        case 0x00: t->offset = param; break;
        case 0x04: t->format = param; break;
        case 0x08: t->address = param; break;
        case 0x0C: t->control0 = param; break;
        case 0x10: t->control1 = param; break;
        case 0x14: t->filter = param; break;
        case 0x1C: t->image_rect = param; break;
        case 0x20: t->palette = param; break;
        case 0x28: case 0x2C: case 0x30: case 0x34: {
            /* BUMP_ENV_MAT: methods are 00,01,11,10 (xemu swizzle) */
            static const int col[4] = { 0, 1, 3, 2 };
            K.psh.bump_mat[stage][col[(reg - 0x28) / 4]] = u2f(param);
            break;
        }
        case 0x38: K.psh.bump_scale[stage] = u2f(param); break;
        case 0x3C: K.psh.bump_offset[stage] = u2f(param); break;
        default: break;
        }
        return 1;
    }

    /* register combiners (xemu pgraph: COMBINE* registers) */
    if (method >= 0x0260 && method < 0x0280) { K.psh.alpha_in[(method - 0x0260) / 4] = param; return 1; }
    if (method >= 0x0A60 && method < 0x0A80) { K.psh.factor0[(method - 0x0A60) / 4] = param; return 1; }
    if (method >= 0x0A80 && method < 0x0AA0) { K.psh.factor1[(method - 0x0A80) / 4] = param; return 1; }
    if (method >= 0x0AA0 && method < 0x0AC0) { K.psh.alpha_out[(method - 0x0AA0) / 4] = param; return 1; }
    if (method >= 0x0AC0 && method < 0x0AE0) { K.psh.rgb_in[(method - 0x0AC0) / 4] = param; return 1; }
    if (method >= 0x1E40 && method < 0x1E60) { K.psh.rgb_out[(method - 0x1E40) / 4] = param; return 1; }
    if (method == 0x029C) g_fog_mode = param; else if (method == 0x02A4) g_fog_en = param;
    else if (method >= 0x09C0 && method <= 0x09C8) { float fv; memcpy(&fv, &param, 4); if (method == 0x09C4) { static int nlog; if (nlog++ < 8) fprintf(stderr, "[FOG] mode=0x%X en=%u p1(raw)=%g\n", g_fog_mode, g_fog_en, (double)fv); g_fog_p1_raw = fv; if (g_fog_scale != 1.0f) fv /= g_fog_scale; } g_fog_p[(method - 0x09C0) / 4] = fv; }
    switch (method) {
    case 0x0288: K.psh.final0 = param; return 1;               /* SPECULAR_FOG_CW0 */
    case 0x028C: K.psh.final1 = param; return 1;               /* SPECULAR_FOG_CW1 */
    case 0x02A8: /* FOG_COLOR: parameter is ABGR, PGRAPH keeps ARGB */
        K.psh.fog_color = (param & 0xFF00FF00u) | ((param & 0xFFu) << 16) | ((param >> 16) & 0xFFu);
        return 1;
    case 0x1E20: K.psh.final_factor0 = param; return 1;        /* SPECULAR_FOG_FACTOR */
    case 0x1E24: K.psh.final_factor1 = param; return 1;
    case 0x1E60: K.psh.combiner_control = param; return 1;     /* COMBINER_CONTROL */
    case 0x1E70: K.psh.shader_stage_program = param; return 1; /* SHADER_STAGE_PROGRAM */
    case 0x1E78: K.psh.other_stage_input = param; return 1;    /* SHADER_OTHER_STAGE_INPUT */
    case 0x1E74: K.psh.dot_rgbmapping = param; return 1;       /* DOT_RGBMAPPING */
    default: break;
    }

    switch (method) {
    case M_BEGIN_END:
        if (param == 0) {
            if (K.in_begin) end_primitive();
            K.in_begin = 0;
        } else {
            K.in_begin = 1;
            K.prim = param;
            K.inline_count = K.index_count = K.array_count = 0;
        }
        return 1;
    case M_INLINE_ARRAY:
        if (!K.inline_count) { g_inline_src = g_kelvin_pb_addr; g_inline_in_call = g_kelvin_pb_in_call; }
        if (K.inline_count < MAX_VERTS * 4) K.inline_data[K.inline_count++] = param;
        return 1;
    case M_ARRAY_ELEMENT16:
        push_index(param & 0xFFFF);
        push_index(param >> 16);
        return 1;
    case M_ARRAY_ELEMENT32:
        push_index(param);
        return 1;
    case M_DRAW_ARRAYS: {
        uint32_t start = param & 0xFFFFFF, count = (param >> 24) + 1;
        /* consecutive DRAW_ARRAYS extend one run; otherwise emit indices */
        if (K.array_count && K.array_start + K.array_count == start) {
            K.array_count += count;
        } else if (!K.array_count && !K.index_count) {
            K.array_start = start; K.array_count = count;
        } else {
            if (K.array_count) {
                for (uint32_t i = 0; i < K.array_count; ++i) push_index(K.array_start + i);
                K.array_count = 0;
            }
            for (uint32_t i = 0; i < count; ++i) push_index(start + i);
        }
        return 1;
    }
    case M_TRANSFORM_EXEC_MODE: K.program_mode = (param & 3) == 2; return 1;
    case M_LAUNCH_TRANSFORM: {
        Vec4 input[16] = {{{0}}};
        VshState state;
        memcpy(input[0].v, K.transform_data, sizeof(K.transform_data));
        vsh_run_from(param, input, &state, 1);
        return 1;
    }
    case M_TRANSFORM_PROG_LOAD: K.program_load = param; return 1;
    case M_TRANSFORM_PROG_START: K.program_start = param; return 1;
    case M_TRANSFORM_CONST_LOAD: K.const_load = param; return 1;
    case M_CLIP_MIN: K.clip_min = u2f(param); return 1;
    case M_CLIP_MAX: K.clip_max = u2f(param); return 1;
    case M_CULL_FACE_ENABLE: K.cull_enable = param; return 1;
    case M_CULL_FACE: K.cull_face = param; return 1;
    case M_FRONT_FACE: K.front_face = param; return 1;
    case M_BLEND_ENABLE: K.blend_enable = param; return 1;
    case M_BLEND_SFACTOR: K.sfactor = param; return 1;
    case M_BLEND_DFACTOR: K.dfactor = param; return 1;
    case M_BLEND_EQUATION: K.blend_eq = param; return 1;
    case M_ALPHA_TEST_ENABLE: K.alpha_test = param; return 1;
    case M_ALPHA_FUNC: K.alpha_func = param; return 1;
    case M_ALPHA_REF: K.alpha_ref = param; return 1;
    case M_DEPTH_TEST_ENABLE: K.depth_test = param; return 1;
    case M_DEPTH_FUNC: K.depth_func = param; return 1;
    case M_DEPTH_MASK: K.depth_mask = param; return 1;
    case M_COLOR_MASK: K.color_mask = param; return 1;
    case M_SURFACE_COLOR_OFFSET:
        g_surf_color = param & 0x03FFFFFFu;
        select_target();
        return 1;
    case M_SURFACE_CLIP_H:
        K.clip_h = param;
        if (g_target_key) g_target_dirty = 1;
        return 1;
    case M_SURFACE_CLIP_V:
        K.clip_v = param;
        if (g_target_key) g_target_dirty = 1;
        return 1;
    case M_COLOR_CLEAR_VALUE: K.clear_color = param; return 1;
    case M_ZSTENCIL_CLEAR_VALUE: K.clear_zstencil = param; return 1;
    case M_CLEAR_RECT_H: K.clear_rect_h = param; return 1;
    case M_CLEAR_RECT_V: K.clear_rect_v = param; return 1;
    case M_CLEAR_SURFACE: {
        IDirect3DDevice8 *dev = xbox_GetD3DDevice();
        if (kenv("KELVIN_LOG_CLEARS")) {
            fprintf(stderr,
                    "[CLEAR] frame=%u flags=%08X rect=(%u,%u)-(%u,%u) color=%08X zs=%08X surf=%08X clip=%08X/%08X\n",
                    K.frame, param,
                    K.clear_rect_h & 0xFFFF, K.clear_rect_v & 0xFFFF,
                    (K.clear_rect_h >> 16) + 1, (K.clear_rect_v >> 16) + 1,
                    K.clear_color, K.clear_zstencil, g_surf_color, K.clip_h, K.clip_v);
        }
        apply_target();
        if (dev) {
            DWORD flags = 0;
            D3DRECT rect;
            rect.x1 = K.clear_rect_h & 0xFFFF; rect.x2 = (K.clear_rect_h >> 16) + 1;
            rect.y1 = K.clear_rect_v & 0xFFFF; rect.y2 = (K.clear_rect_v >> 16) + 1;
            /* CLEAR_SURFACE bits 4..7 are the R,G,B,A channel enables.  An alpha-only clear (0x80, issued before the
             * post-processing passes) must not touch RGB; D3D Clear has no channel mask, so wiping the target to the
             * clear colour erased the whole rendered scene.  KELVIN_CLEAR_LEGACY=1 restores the old behaviour. */
            if ((param & 0x70) || ((param & 0xF0) && kenv("KELVIN_CLEAR_LEGACY"))) flags |= D3DCLEAR_TARGET;
            if (param & 0x01) flags |= D3DCLEAR_ZBUFFER;
            if (param & 0x02) flags |= D3DCLEAR_STENCIL;
            if (flags || !(param & 0x80))
                dev->lpVtbl->Clear(dev, 1, &rect, flags, K.clear_color,
                                   (float)(K.clear_zstencil >> 8) / 16777215.0f,
                                   K.clear_zstencil & 0xFF);
            if ((param & 0x80) && !(param & 0x70) && !kenv("KELVIN_CLEAR_LEGACY")) {
                /* alpha-only colour clear (c56): D3D Clear has no channel mask, so draw a rectangle that writes only alpha */
                OutVertex q[4]; int vi;
                float xs[4] = { (float)rect.x1, (float)rect.x2, (float)rect.x1, (float)rect.x2 };
                float ys[4] = { (float)rect.y1, (float)rect.y1, (float)rect.y2, (float)rect.y2 };
                memset(q, 0, sizeof(q));
                for (vi = 0; vi < 4; ++vi) {
                    q[vi].x = xs[vi]; q[vi].y = ys[vi]; q[vi].z = 0.0f; q[vi].rhw = 1.0f;
                    q[vi].diffuse = K.clear_color; q[vi].tex[3][3] = 1.0f; q[vi].fogv[0] = 1.0f;
                }
                d3d8_nv2a_psh_set(NULL);
                kelvin_state_cache_invalidate();
                for (vi = 0; vi < 4; ++vi) dev->lpVtbl->SetTexture(dev, vi, NULL);
                dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
                dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
                dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
                dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
                dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE, 8);
                dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | 0x500u | 0x02AA0000u);
                kelvin_apply_xmap(1, 0);
                dev->lpVtbl->BeginScene(dev);
                dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(OutVertex));
            }
        }
        K.stats.clears++;
        return 1;
    }
    default:
        return 0;
    }
}

/* c165: contiguous blocks returned to the pool are queued by the kernel (any thread, lock held) and applied at the next frame boundary of this
 * thread, so a freed offscreen render target stops shadowing the guest memory that is reused at its address (STE_RT_INVALIDATE=0 turns it off). */
extern int d3d8_rt_invalidate_range(uint32_t phys, uint32_t bytes);
extern void xbox_SetContigFreeHook(void (*fn)(uint32_t phys, uint32_t bytes));
static SRWLOCK g_rtinv_lock = SRWLOCK_INIT;
static struct { uint32_t phys, bytes; } g_rtinv_q[64];
static int g_rtinv_n, g_rtinv_overflow;
static void kelvin_contig_freed(uint32_t phys, uint32_t bytes)
{
    AcquireSRWLockExclusive(&g_rtinv_lock);
    if (g_rtinv_n < 64) { g_rtinv_q[g_rtinv_n].phys = phys; g_rtinv_q[g_rtinv_n].bytes = bytes; g_rtinv_n++; } else g_rtinv_overflow = 1;
    ReleaseSRWLockExclusive(&g_rtinv_lock);
}
static void kelvin_apply_rt_invalidations(void)
{
    static int hooked, off, logged;
    uint32_t p[64], b[64]; int n, i, ov;
    if (!hooked) { const char *e = getenv("STE_RT_INVALIDATE"); hooked = 1; off = (e && e[0] == '0'); if (!off) xbox_SetContigFreeHook(kelvin_contig_freed); }
    if (off || (!g_rtinv_n && !g_rtinv_overflow)) return;
    AcquireSRWLockExclusive(&g_rtinv_lock);
    n = g_rtinv_n; ov = g_rtinv_overflow;
    for (i = 0; i < n; ++i) { p[i] = g_rtinv_q[i].phys; b[i] = g_rtinv_q[i].bytes; }
    g_rtinv_n = 0; g_rtinv_overflow = 0;
    ReleaseSRWLockExclusive(&g_rtinv_lock);
    if (ov) { int d = d3d8_rt_invalidate_range(0, 0x04000000u); if (logged++ < 40) fprintf(stderr, "[RTINV] frame=%u queue overflow: dropped %d render targets\n", K.frame, d); }
    for (i = 0; i < n; ++i) {
        int d = d3d8_rt_invalidate_range(p[i], b[i]);
        if (d && logged++ < 40) fprintf(stderr, "[RTINV] frame=%u freed phys=%08X bytes=%u: dropped %d render target(s)\n", K.frame, p[i], b[i], d);
    }
}

void kelvin_end_frame(void)
{
    if (K.in_begin) { end_primitive(); K.in_begin = 0; }
    if (g_surf_color && g_surf_color != g_display[0]) {
        g_display[1] = g_display[0];
        g_display[0] = g_surf_color;
    }
    if (g_target_key) {
        uint32_t completed_key = g_target_key;
        /* Initial history replay can classify the first completed display
         * surface as offscreen.  Present only this frame-ending surface; never
         * expose intermediate render-to-texture passes, which visibly flash. */
        g_target_key = 0;
        d3d8_rt_present(completed_key);
        d3d8_rt_select(0, 0, 0);
        g_target_dirty = 0;
    }
    K.frame++;
    kelvin_apply_rt_invalidations();
    {   static int every = -1; if (every < 0) { const char *e = getenv("STE_CULLSTAT"); every = (e && *e && *e != '0') ? (atoi(e) > 1 ? atoi(e) : 600) : 0; }
        if (every && (K.frame % (uint32_t)every) == 0) g_cullstat_dump_request = 1; }
    kpath_report();
}
