/**
 * D3D8 Render State → D3D11 State Object Translation
 *
 * Converts D3D8 render state values into D3D11 state objects:
 *   - Blend state (alpha blending, color write mask)
 *   - Depth-stencil state (z-test, z-write, stencil)
 *   - Rasterizer state (cull mode, fill mode)
 *   - Sampler state (texture filtering, addressing)
 *
 * State objects are cached and recreated only when dirty.
 */

#include "d3d8_internal.h"
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Cached D3D11 state objects
 * ================================================================ */

static ID3D11BlendState        *g_blend_state = NULL;
static ID3D11DepthStencilState *g_ds_state = NULL;
static ID3D11RasterizerState   *g_raster_state = NULL;
static ID3D11SamplerState      *g_sampler_states[4] = { NULL, NULL, NULL, NULL };

/* c79: state objects are interned by descriptor (a draw alternating between a few states used to Create/Release one per
 * draw, and every draw re-created 4 sampler states); bindings are skipped when the object is already bound. */
#define STATE_CACHE 96
typedef struct { D3D11_BLEND_DESC d; ID3D11BlendState *o; } BlendEnt;
typedef struct { D3D11_DEPTH_STENCIL_DESC d; ID3D11DepthStencilState *o; } DsEnt;
typedef struct { D3D11_RASTERIZER_DESC d; ID3D11RasterizerState *o; } RastEnt;
typedef struct { D3D11_SAMPLER_DESC d; ID3D11SamplerState *o; } SampEnt;
static BlendEnt g_blend_cache[STATE_CACHE]; static int g_blend_n;
static DsEnt    g_ds_cache[STATE_CACHE];    static int g_ds_n;
static RastEnt  g_rast_cache[STATE_CACHE];  static int g_rast_n;
static SampEnt  g_samp_cache[STATE_CACHE];  static int g_samp_n;
static ID3D11BlendState *g_bound_blend; static ID3D11DepthStencilState *g_bound_ds; static UINT g_bound_ref = 0xFFFFFFFFu;
static ID3D11RasterizerState *g_bound_rast; static ID3D11SamplerState *g_bound_samp[4];

/* Last known render state identity for dirty detection */
static DWORD g_last_blend_hash = 0;
static DWORD g_last_raster_hash = 0;
static D3D11_DEPTH_STENCIL_DESC g_last_ds_desc;

/* ================================================================
 * D3D8 → D3D11 enum translation
 * ================================================================ */

static D3D11_BLEND d3d8_to_d3d11_blend(DWORD d3d8blend)
{
    switch (d3d8blend) {
    case D3DBLEND_ZERO:         return D3D11_BLEND_ZERO;
    case D3DBLEND_ONE:          return D3D11_BLEND_ONE;
    case D3DBLEND_SRCCOLOR:     return D3D11_BLEND_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR:  return D3D11_BLEND_INV_SRC_COLOR;
    case D3DBLEND_SRCALPHA:     return D3D11_BLEND_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA:  return D3D11_BLEND_INV_SRC_ALPHA;
    case D3DBLEND_DESTALPHA:    return D3D11_BLEND_DEST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
    case D3DBLEND_DESTCOLOR:    return D3D11_BLEND_DEST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return D3D11_BLEND_INV_DEST_COLOR;
    case D3DBLEND_SRCALPHASAT:  return D3D11_BLEND_SRC_ALPHA_SAT;
    default:                    return D3D11_BLEND_ONE;
    }
}

/* D3D11 forbids color factors in the alpha channel. Use their alpha components. */
static D3D11_BLEND blend_alpha_factor(D3D11_BLEND blend)
{
    switch (blend) {
    case D3D11_BLEND_SRC_COLOR:      return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR:  return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR:     return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    default:                       return blend;
    }
}

static D3D11_COMPARISON_FUNC d3d8_to_d3d11_cmp(DWORD d3d8cmp)
{
    switch (d3d8cmp) {
    case D3DCMP_NEVER:        return D3D11_COMPARISON_NEVER;
    case D3DCMP_LESS:         return D3D11_COMPARISON_LESS;
    case D3DCMP_EQUAL:        return D3D11_COMPARISON_EQUAL;
    case D3DCMP_LESSEQUAL:    return D3D11_COMPARISON_LESS_EQUAL;
    case D3DCMP_GREATER:      return D3D11_COMPARISON_GREATER;
    case D3DCMP_NOTEQUAL:     return D3D11_COMPARISON_NOT_EQUAL;
    case D3DCMP_GREATEREQUAL: return D3D11_COMPARISON_GREATER_EQUAL;
    case D3DCMP_ALWAYS:       return D3D11_COMPARISON_ALWAYS;
    default:                  return D3D11_COMPARISON_LESS_EQUAL;
    }
}

static D3D11_STENCIL_OP d3d8_to_d3d11_stencilop(DWORD op)
{
    switch (op) {
    case 1: return D3D11_STENCIL_OP_KEEP;
    case 2: return D3D11_STENCIL_OP_ZERO;
    case 3: return D3D11_STENCIL_OP_REPLACE;
    case 4: return D3D11_STENCIL_OP_INCR_SAT;
    case 5: return D3D11_STENCIL_OP_DECR_SAT;
    case 6: return D3D11_STENCIL_OP_INVERT;
    case 7: return D3D11_STENCIL_OP_INCR;
    case 8: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
    }
}

static D3D11_BLEND_OP d3d8_to_d3d11_blendop(DWORD op)
{
    switch (op) {
    case 1: return D3D11_BLEND_OP_ADD;
    case 2: return D3D11_BLEND_OP_SUBTRACT;
    case 3: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 4: return D3D11_BLEND_OP_MIN;
    case 5: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
    }
}

/* Simple hash of relevant render state values for dirty detection */
static DWORD hash_blend_states(const DWORD *rs)
{
    return rs[D3DRS_ALPHABLENDENABLE] ^
           (rs[D3DRS_SRCBLEND] << 4) ^
           (rs[D3DRS_DESTBLEND] << 8) ^
           (rs[D3DRS_BLENDOP] << 12) ^
           (rs[D3DRS_COLORWRITEENABLE] << 16);
}

/* c136: optional scissor rectangle (render-target pixels); d3d8_set_scissor() is called by the Kelvin translator per draw (front-end 4:3 clip of the CRT static overlay). */
static int g_scissor_on; static D3D11_RECT g_scissor_rc;
void d3d8_set_scissor(int on, int l, int t, int r, int b)
{
    g_scissor_on = on ? 1 : 0;
    if (on) { g_scissor_rc.left = l; g_scissor_rc.top = t; g_scissor_rc.right = r; g_scissor_rc.bottom = b; }
}

static DWORD hash_raster_states(const DWORD *rs)
{
    return rs[D3DRS_CULLMODE] ^
           (rs[D3DRS_FILLMODE] << 4) ^ (g_scissor_on ? 0x5C155081u : 0u);
}

/* ================================================================
 * State object creation
 * ================================================================ */

static void update_blend_state(const DWORD *rs)
{
    DWORD hash = hash_blend_states(rs);
    D3D11_BLEND_DESC bd;
    HRESULT hr;

    if (hash == g_last_blend_hash && g_blend_state) return;
    g_last_blend_hash = hash;

    g_blend_state = NULL;

    memset(&bd, 0, sizeof(bd));
    bd.RenderTarget[0].BlendEnable = rs[D3DRS_ALPHABLENDENABLE] ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = d3d8_to_d3d11_blend(rs[D3DRS_SRCBLEND]);
    bd.RenderTarget[0].DestBlend = d3d8_to_d3d11_blend(rs[D3DRS_DESTBLEND]);
    bd.RenderTarget[0].BlendOp = d3d8_to_d3d11_blendop(rs[D3DRS_BLENDOP] ? rs[D3DRS_BLENDOP] : 1);
    bd.RenderTarget[0].SrcBlendAlpha = blend_alpha_factor(bd.RenderTarget[0].SrcBlend);
    bd.RenderTarget[0].DestBlendAlpha = blend_alpha_factor(bd.RenderTarget[0].DestBlend);
    bd.RenderTarget[0].BlendOpAlpha = bd.RenderTarget[0].BlendOp;
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)(rs[D3DRS_COLORWRITEENABLE] & 0x0F);

    for (int i = 0; i < g_blend_n; ++i)
        if (!memcmp(&g_blend_cache[i].d, &bd, sizeof(bd))) { g_blend_state = g_blend_cache[i].o; return; }
    hr = ID3D11Device_CreateBlendState(d3d8_GetD3D11Device(), &bd, &g_blend_state);
    if (FAILED(hr))
        fprintf(stderr, "D3D8: CreateBlendState failed: 0x%08lX\n", hr);
    else if (g_blend_n < STATE_CACHE) { g_blend_cache[g_blend_n].d = bd; g_blend_cache[g_blend_n].o = g_blend_state; g_blend_n++; }
}

static void update_depth_stencil_state(const DWORD *rs)
{
    D3D11_DEPTH_STENCIL_DESC dsd;
    HRESULT hr;

    memset(&dsd, 0, sizeof(dsd));
    dsd.DepthEnable = rs[D3DRS_ZENABLE] ? TRUE : FALSE;
    dsd.DepthWriteMask = rs[D3DRS_ZWRITEENABLE] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = d3d8_to_d3d11_cmp(rs[D3DRS_ZFUNC]);

    dsd.StencilEnable = rs[D3DRS_STENCILENABLE] ? TRUE : FALSE;
    dsd.StencilReadMask = (UINT8)(rs[D3DRS_STENCILMASK] & 0xFF);
    dsd.StencilWriteMask = (UINT8)(rs[D3DRS_STENCILWRITEMASK] & 0xFF);

    dsd.FrontFace.StencilFunc = d3d8_to_d3d11_cmp(rs[D3DRS_STENCILFUNC]);
    dsd.FrontFace.StencilFailOp = d3d8_to_d3d11_stencilop(rs[D3DRS_STENCILFAIL]);
    dsd.FrontFace.StencilDepthFailOp = d3d8_to_d3d11_stencilop(rs[D3DRS_STENCILZFAIL]);
    dsd.FrontFace.StencilPassOp = d3d8_to_d3d11_stencilop(rs[D3DRS_STENCILPASS]);
    dsd.BackFace = dsd.FrontFace;

    /* Zeroed padding makes the complete descriptor comparable; the reference
     * is bound separately. */
    if (g_ds_state && memcmp(&dsd, &g_last_ds_desc, sizeof(dsd)) == 0) return;
    g_ds_state = NULL;
    for (int i = 0; i < g_ds_n; ++i)
        if (!memcmp(&g_ds_cache[i].d, &dsd, sizeof(dsd))) { g_ds_state = g_ds_cache[i].o; memcpy(&g_last_ds_desc, &dsd, sizeof(dsd)); return; }

    hr = ID3D11Device_CreateDepthStencilState(d3d8_GetD3D11Device(), &dsd, &g_ds_state);
    if (FAILED(hr))
        fprintf(stderr, "D3D8: CreateDepthStencilState failed: 0x%08lX\n", hr);
    else {
        memcpy(&g_last_ds_desc, &dsd, sizeof(dsd));
        if (g_ds_n < STATE_CACHE) { g_ds_cache[g_ds_n].d = dsd; g_ds_cache[g_ds_n].o = g_ds_state; g_ds_n++; }
    }
}

static void update_rasterizer_state(const DWORD *rs)
{
    DWORD hash = hash_raster_states(rs);
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;

    if (hash == g_last_raster_hash && g_raster_state) return;
    g_last_raster_hash = hash;

    g_raster_state = NULL;

    memset(&rd, 0, sizeof(rd));

    switch (rs[D3DRS_FILLMODE]) {
    case D3DFILL_POINT:     rd.FillMode = D3D11_FILL_WIREFRAME; break;  /* D3D11 has no point fill */
    case D3DFILL_WIREFRAME: rd.FillMode = D3D11_FILL_WIREFRAME; break;
    default:                rd.FillMode = D3D11_FILL_SOLID; break;
    }

    switch (rs[D3DRS_CULLMODE]) {
    case D3DCULL_NONE: rd.CullMode = D3D11_CULL_NONE; break;
    case D3DCULL_CW:   rd.CullMode = D3D11_CULL_FRONT; break;  /* D3D8 CW = cull front in D3D11 convention */
    case D3DCULL_CCW:  rd.CullMode = D3D11_CULL_BACK; break;
    default:           rd.CullMode = D3D11_CULL_BACK; break;
    }

    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = g_scissor_on ? TRUE : FALSE;
    rd.MultisampleEnable = FALSE;
    rd.AntialiasedLineEnable = FALSE;

    for (int i = 0; i < g_rast_n; ++i)
        if (!memcmp(&g_rast_cache[i].d, &rd, sizeof(rd))) { g_raster_state = g_rast_cache[i].o; return; }
    hr = ID3D11Device_CreateRasterizerState(d3d8_GetD3D11Device(), &rd, &g_raster_state);
    if (FAILED(hr))
        fprintf(stderr, "D3D8: CreateRasterizerState failed: 0x%08lX\n", hr);
    else if (g_rast_n < STATE_CACHE) { g_rast_cache[g_rast_n].d = rd; g_rast_cache[g_rast_n].o = g_raster_state; g_rast_n++; }
}

/* ================================================================
 * Sampler state
 * ================================================================ */

static D3D11_TEXTURE_ADDRESS_MODE d3d8_to_d3d11_address(DWORD mode)
{
    switch (mode) {
    case D3DTADDRESS_WRAP:       return D3D11_TEXTURE_ADDRESS_WRAP;
    case D3DTADDRESS_MIRROR:     return D3D11_TEXTURE_ADDRESS_MIRROR;
    case D3DTADDRESS_CLAMP:      return D3D11_TEXTURE_ADDRESS_CLAMP;
    case D3DTADDRESS_BORDER:     return D3D11_TEXTURE_ADDRESS_BORDER;
    case D3DTADDRESS_MIRRORONCE: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default:                     return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

static D3D11_FILTER d3d8_to_d3d11_filter(DWORD mag, DWORD min, DWORD mip)
{
    /* Simplified filter mapping */
    BOOL mag_linear = (mag == D3DTEXF_LINEAR || mag == D3DTEXF_ANISOTROPIC);
    BOOL min_linear = (min == D3DTEXF_LINEAR || min == D3DTEXF_ANISOTROPIC);
    BOOL mip_linear = (mip == D3DTEXF_LINEAR);

    if (mag == D3DTEXF_ANISOTROPIC || min == D3DTEXF_ANISOTROPIC)
        return D3D11_FILTER_ANISOTROPIC;

    if (min_linear && mag_linear && mip_linear)
        return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (min_linear && mag_linear)
        return D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    if (min_linear)
        return D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    if (mag_linear)
        return D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;

    return D3D11_FILTER_MIN_MAG_MIP_POINT;
}

/* c128 texture quality (ste_video.ini anisotropy / mip_bias): set once by the Kelvin texture-pack module; mips = textures may carry a mip chain
 * (trilinear instead of point mip selection); the aniso/bias part only applies to draws Kelvin marks as world draws (d3d8_sampler_quality_world). */
static unsigned g_sq_aniso; static float g_sq_bias; static int g_sq_mips, g_sq_world;
void d3d8_set_sampler_quality(unsigned aniso, float bias, int mips) { g_sq_aniso = aniso; g_sq_bias = bias; g_sq_mips = mips; }
void d3d8_sampler_quality_world(int on) { g_sq_world = on; }

void d3d8_states_apply_sampler(DWORD stage)
{
    const DWORD *tss;
    D3D11_SAMPLER_DESC sd;
    HRESULT hr;
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();

    if (stage >= 4) return;
    tss = d3d8_GetTSS(stage);
    if (!tss) return;

    memset(&sd, 0, sizeof(sd));
    sd.Filter = d3d8_to_d3d11_filter(
        tss[D3DTSS_MAGFILTER],
        tss[D3DTSS_MINFILTER],
        tss[D3DTSS_MIPFILTER]);
    sd.AddressU = d3d8_to_d3d11_address(tss[D3DTSS_ADDRESSU] ? tss[D3DTSS_ADDRESSU] : D3DTADDRESS_WRAP);
    sd.AddressV = d3d8_to_d3d11_address(tss[D3DTSS_ADDRESSV] ? tss[D3DTSS_ADDRESSV] : D3DTADDRESS_WRAP);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxAnisotropy = tss[D3DTSS_MAXANISOTROPY] ? tss[D3DTSS_MAXANISOTROPY] : 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (g_sq_mips && sd.Filter == D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT) sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (g_sq_world) {
        if (g_sq_aniso > 1 && (sd.Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR || sd.Filter == D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT)) { sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = g_sq_aniso; }
        sd.MipLODBias = g_sq_bias;
    }

    {
        ID3D11SamplerState *s = NULL;
        for (int i = 0; i < g_samp_n; ++i)
            if (!memcmp(&g_samp_cache[i].d, &sd, sizeof(sd))) { s = g_samp_cache[i].o; break; }
        if (!s) {
            hr = ID3D11Device_CreateSamplerState(d3d8_GetD3D11Device(), &sd, &s);
            if (FAILED(hr)) return;
            if (g_samp_n < STATE_CACHE) { g_samp_cache[g_samp_n].d = sd; g_samp_cache[g_samp_n].o = s; g_samp_n++; }
        }
        g_sampler_states[stage] = s;
        if (g_bound_samp[stage] != s) { ID3D11DeviceContext_PSSetSamplers(ctx, stage, 1, &s); g_bound_samp[stage] = s; }
    }
}

/* ================================================================
 * Apply all states before draw call
 * ================================================================ */

HRESULT d3d8_states_init(void)
{
    /* States are created on first apply */
    return S_OK;
}

void d3d8_states_shutdown(void)
{
    int i;
    for (i = 0; i < g_blend_n; i++) ID3D11BlendState_Release(g_blend_cache[i].o);
    for (i = 0; i < g_ds_n; i++) ID3D11DepthStencilState_Release(g_ds_cache[i].o);
    for (i = 0; i < g_rast_n; i++) ID3D11RasterizerState_Release(g_rast_cache[i].o);
    for (i = 0; i < g_samp_n; i++) ID3D11SamplerState_Release(g_samp_cache[i].o);
    g_blend_n = g_ds_n = g_rast_n = g_samp_n = 0;
    g_blend_state = NULL; g_ds_state = NULL; g_raster_state = NULL;
    g_bound_blend = NULL; g_bound_ds = NULL; g_bound_ref = 0xFFFFFFFFu; g_bound_rast = NULL;
    for (i = 0; i < 4; i++) { g_sampler_states[i] = NULL; g_bound_samp[i] = NULL; }
    g_last_blend_hash = 0;
    g_last_raster_hash = 0;
}

/* Forget what is believed to be bound (after command lists / anything else that may have changed the immediate context). */
extern int g_kvp_ff_dirty, g_psh_ps_dirty;
void d3d8_states_invalidate_bindings(void)
{
    int i;
    g_bound_blend = NULL; g_bound_ds = NULL; g_bound_ref = 0xFFFFFFFFu; g_bound_rast = NULL;
    for (i = 0; i < 4; i++) g_bound_samp[i] = NULL;
    g_kvp_ff_dirty = 1; g_psh_ps_dirty = 1;
}

void d3d8_states_apply(void)
{
    const DWORD *rs = d3d8_GetRenderStates();
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    float blend_factor[4] = { 1, 1, 1, 1 };

    if (!rs || !ctx) return;

    update_blend_state(rs);
    update_depth_stencil_state(rs);
    update_rasterizer_state(rs);

    if (g_blend_state && g_blend_state != g_bound_blend) {
        ID3D11DeviceContext_OMSetBlendState(ctx, g_blend_state, blend_factor, 0xFFFFFFFF);
        g_bound_blend = g_blend_state;
    }
    if (g_ds_state && (g_ds_state != g_bound_ds || rs[D3DRS_STENCILREF] != g_bound_ref)) {
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, g_ds_state, rs[D3DRS_STENCILREF]);
        g_bound_ds = g_ds_state; g_bound_ref = rs[D3DRS_STENCILREF];
    }
    if (g_raster_state && g_raster_state != g_bound_rast) {
        ID3D11DeviceContext_RSSetState(ctx, g_raster_state);
        g_bound_rast = g_raster_state;
    }
    if (g_scissor_on) ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &g_scissor_rc);

    /* Apply samplers for all 4 texture stages */
    {
        DWORD s;
        for (s = 0; s < 4; s++)
            d3d8_states_apply_sampler(s);
    }
}

/* c145 probe: D3D11 CullMode (1 NONE, 2 FRONT, 3 BACK) of the rasterizer state bound by the last d3d8_states_apply, -1 if unknown */
int d3d8_states_bound_cullmode(void)
{
    int i; for (i = 0; i < g_rast_n; ++i) if (g_rast_cache[i].o == g_bound_rast) return (int)g_rast_cache[i].d.CullMode;
    return -1;
}
