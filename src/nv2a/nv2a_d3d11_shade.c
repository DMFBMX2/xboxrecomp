/*
 * Drawing a shaded batch with D3D11.
 *
 * Nearly everything a title draws is one texture times the vertex colour,
 * and the sink draws that through the D3D8 layer's fixed pipeline. A batch
 * that asks for more -- several texture stages, a cube map or a volume, a
 * stage that looks up where an earlier one points, combiners that do
 * anything else -- comes here instead, and is drawn with a pixel shader
 * written for exactly what the title programmed (nv2a_psh_hlsl.c).
 *
 * The blend, depth, stencil and cull state are still the D3D8 layer's: the
 * sink sets them there as for any batch and d3d8_states_apply() puts them on
 * the device. What is this file's own is the rest of the pipeline: the
 * vertex format with four sets of four coordinates, the textures of all four
 * stages built from guest memory as flat pictures, cube maps or volumes,
 * their samplers, and the shaders. The layer binds its own again for every
 * batch it draws, so nothing has to be put back afterwards.
 *
 * Runs on the NV2A poll thread, like the rest of the sink.
 */
#ifdef _WIN32

#include <windows.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define COBJMACROS
#include <d3d11.h>
#include <d3dcompiler.h>

#include "nv2a_d3d11_shade.h"
#include "nv2a_psh_hlsl.h"
#include "nv2a_vsh_hlsl.h"
#include "../kernel/nv2a_vsh_interp.h"

#pragma comment(lib, "d3dcompiler.lib")

/* From the D3D8 layer (d3d8_internal.h). */
ID3D11Device        *d3d8_GetD3D11Device(void);
ID3D11DeviceContext *d3d8_GetD3D11Context(void);
UINT                 d3d8_GetBackbufferWidth(void);
UINT                 d3d8_GetBackbufferHeight(void);
void                 d3d8_states_apply(void);

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define SHADE_MAX_TEXTURES 1024
#define SHADE_TEX_BUCKETS  4096
#define SHADE_MAX_SAMPLERS 32
#define SHADE_MAX_SHADERS  64

/* A stage's texture, built from guest memory. As in the sink, identity is
 * where it lives and what shape it is, and `hash` says whether the guest
 * has rewritten it since. */
typedef struct {
    uint32_t offset, width, height, depth, format, palette, levels;
    int      cube, dims;
    uint64_t hash;
    uint32_t checked_frame, used_frame;
    ID3D11Resource           *res;
    ID3D11ShaderResourceView *srv;
} ShadeTexture;

typedef struct {
    uint32_t filter, addr_u, addr_v, addr_p;
    int      mips, cube;
    ID3D11SamplerState *state;
} ShadeSampler;

typedef struct {
    Nv2aPshKey         key;
    ID3D11PixelShader *ps;              /* NULL: it did not compile */
    int                used;
} ShadeShader;

static struct {
    int inited, failed;
    ID3D11VertexShader *vs;
    ID3D11InputLayout  *layout;
    ID3D11Buffer       *vs_cb, *ps_cb, *vb;
    UINT                vb_bytes;
    ShadeTexture textures[SHADE_MAX_TEXTURES];
    /* Where each address was last found: slot + 1, or 0. A hint, checked
     * before the scan, so that the common case is one comparison. */
    uint16_t     tex_hint[SHADE_TEX_BUCKETS];
    ShadeSampler samplers[SHADE_MAX_SAMPLERS];
    int          sampler_count;
    ShadeShader  shaders[SHADE_MAX_SHADERS];
    int          shader_next;
    uint32_t    *scratch;
    size_t       scratch_texels;
} g_shade;

/* ── The vertex shader ─────────────────────────────────────── */

/* The position arrives in the back buffer's pixels with the depth value and
 * 1/w, and is turned back into clip space exactly as the D3D8 layer's
 * shader does for a pre-transformed vertex, so that a shaded batch lands on
 * the same pixels and depths as the batches around it. */
static const char g_vs_source[] =
    "cbuffer ShadeVS : register(b0) { float4 Screen; };\n"
    "struct VS_IN {\n"
    "    float4 pos : POSITION;\n"
    "    float4 d0  : COLOR0;\n"
    "    float4 d1  : COLOR1;\n"
    "    float4 t0  : TEXCOORD0;\n"
    "    float4 t1  : TEXCOORD1;\n"
    "    float4 t2  : TEXCOORD2;\n"
    "    float4 t3  : TEXCOORD3;\n"
    "    float  fog : TEXCOORD4;\n"
    "};\n"
    "struct VS_OUT {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 d0  : COLOR0;\n"
    "    float4 d1  : COLOR1;\n"
    "    float4 t0  : TEXCOORD0;\n"
    "    float4 t1  : TEXCOORD1;\n"
    "    float4 t2  : TEXCOORD2;\n"
    "    float4 t3  : TEXCOORD3;\n"
    "    float  fog : TEXCOORD4;\n"
    "};\n"
    "VS_OUT main(VS_IN i) {\n"
    "    VS_OUT o;\n"
    "    o.pos.x = (i.pos.x / Screen.x) * 2.0 - 1.0;\n"
    "    o.pos.y = 1.0 - (i.pos.y / Screen.y) * 2.0;\n"
    "    o.pos.z = i.pos.z;\n"
    "    o.pos.w = 1.0;\n"
    "    if (i.pos.w != 0.0) o.pos /= i.pos.w;\n"
    /* A D3DCOLOR's bytes are b g r a. */
    "    o.d0 = i.d0.bgra;\n"
    "    o.d1 = i.d1.bgra;\n"
    "    o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
    "    o.fog = i.fog;\n"
    "    return o;\n"
    "}\n";

static ID3DBlob *shade_compile(const char *src, size_t len, const char *target)
{
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, len, "nv2a_shade", NULL, NULL, "main",
                            target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &blob,
                            &err);

    if (FAILED(hr)) {
        fprintf(stderr, "[NV2A-D3D11] shaded: %s did not compile (0x%08lX):"
                        " %s\n%s\n", target, (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "",
                src);
        fflush(stderr);
        blob = NULL;
    }
    if (err)
        ID3D10Blob_Release(err);
    return blob;
}

static int shade_init(void)
{
    static const D3D11_INPUT_ELEMENT_DESC elems[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,     0, 16,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R8G8B8A8_UNORM,     0, 20,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 72,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32_FLOAT,          0, 88,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11Device *d3d = d3d8_GetD3D11Device();
    D3D11_BUFFER_DESC bd;
    ID3DBlob *blob;

    if (g_shade.inited)
        return !g_shade.failed;
    g_shade.inited = 1;
    g_shade.failed = 1;
    if (!d3d)
        return 0;

    blob = shade_compile(g_vs_source, sizeof g_vs_source - 1, "vs_4_0");
    if (!blob)
        return 0;
    if (FAILED(ID3D11Device_CreateVertexShader(d3d,
                ID3D10Blob_GetBufferPointer(blob),
                ID3D10Blob_GetBufferSize(blob), NULL, &g_shade.vs))
            || FAILED(ID3D11Device_CreateInputLayout(d3d, elems,
                sizeof elems / sizeof elems[0],
                ID3D10Blob_GetBufferPointer(blob),
                ID3D10Blob_GetBufferSize(blob), &g_shade.layout))) {
        ID3D10Blob_Release(blob);
        return 0;
    }
    ID3D10Blob_Release(blob);

    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = 16;
    if (FAILED(ID3D11Device_CreateBuffer(d3d, &bd, NULL, &g_shade.vs_cb)))
        return 0;
    bd.ByteWidth = sizeof(Nv2aPshConstants);
    if (FAILED(ID3D11Device_CreateBuffer(d3d, &bd, NULL, &g_shade.ps_cb)))
        return 0;

    g_shade.failed = 0;
    return 1;
}

/* ── Textures ──────────────────────────────────────────────── */

/* FNV-1a over 64-bit words, as the sink's. */
static uint64_t shade_hash(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i, words = n / 8;

    for (i = 0; i < words; i++) {
        uint64_t w;
        memcpy(&w, p + i * 8, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    for (i = words * 8; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static uint32_t level_size(uint32_t size, uint32_t level)
{
    return (size >> level) ? (size >> level) : 1;
}

static void shade_texture_release(ShadeTexture *t)
{
    if (t->srv)
        ID3D11ShaderResourceView_Release(t->srv);
    if (t->res)
        ID3D11Resource_Release(t->res);
    memset(t, 0, sizeof *t);
}

static int shade_texture_create(ShadeTexture *t)
{
    ID3D11Device *d3d = d3d8_GetD3D11Device();
    HRESULT hr;

    if (t->dims == 3 && !t->cube) {
        D3D11_TEXTURE3D_DESC td;
        ID3D11Texture3D *tex = NULL;

        memset(&td, 0, sizeof td);
        td.Width = t->width;
        td.Height = t->height;
        td.Depth = t->depth;
        td.MipLevels = t->levels;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = ID3D11Device_CreateTexture3D(d3d, &td, NULL, &tex);
        t->res = (ID3D11Resource *)tex;
    } else {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D *tex = NULL;

        memset(&td, 0, sizeof td);
        td.Width = t->width;
        td.Height = t->height;
        td.MipLevels = t->levels;
        td.ArraySize = t->cube ? 6 : 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = t->cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        hr = ID3D11Device_CreateTexture2D(d3d, &td, NULL, &tex);
        t->res = (ID3D11Resource *)tex;
    }
    if (FAILED(hr) || !t->res) {
        t->res = NULL;
        return 0;
    }
    /* A view of the whole resource, of the kind the resource is. */
    hr = ID3D11Device_CreateShaderResourceView(d3d, t->res, NULL, &t->srv);
    if (FAILED(hr) || !t->srv) {
        shade_texture_release(t);
        return 0;
    }
    return 1;
}

/* Every level the title supplied, of every face or slice. */
static int shade_texture_upload(ShadeTexture *t, uint32_t stage)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    size_t texels = (size_t)t->width * t->height * t->depth;
    uint32_t parts = t->cube ? 6 : 1, part, level, z;

    if (texels > g_shade.scratch_texels) {
        free(g_shade.scratch);
        g_shade.scratch = (uint32_t *)malloc(texels * 4);
        g_shade.scratch_texels = g_shade.scratch ? texels : 0;
    }
    if (!g_shade.scratch)
        return 0;

    for (part = 0; part < parts; part++) {
        for (level = 0; level < t->levels; level++) {
            uint32_t w = level_size(t->width, level);
            uint32_t h = level_size(t->height, level);
            uint32_t d = level_size(t->depth, level);

            for (z = 0; z < d; z++)
                if (!nv2a_pb_exec_decode_stage(stage, level,
                        t->cube ? part : z,
                        g_shade.scratch + (size_t)z * w * h))
                    return 0;
            ID3D11DeviceContext_UpdateSubresource(ctx, t->res,
                level + part * t->levels, NULL, g_shade.scratch, w * 4,
                w * h * 4);
        }
    }
    return 1;
}

/* The view of what the title has bound to a stage, decoded and uploaded if
 * it is new or the guest has rewritten it since the last frame it was used
 * in. NULL if it cannot be had. */
static ID3D11ShaderResourceView *shade_texture(const Nv2aSinkTexture *src,
                                               uint32_t frame)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    ShadeTexture *t = NULL, *spare = NULL, *oldest = &g_shade.textures[0];
    uint32_t levels = src->levels ? src->levels : 1;
    uint32_t depth = (src->dims == 3 && src->depth) ? src->depth : 1;
    int i, fresh = 0;
    uint32_t bucket;

    if (!src->valid || !src->width || !src->height || src->width > 4096
            || src->height > 4096 || depth > 2048)
        return NULL;

    bucket = ((src->offset >> 8) ^ (src->offset >> 20) ^ src->palette)
           & (SHADE_TEX_BUCKETS - 1u);
    if (g_shade.tex_hint[bucket]) {
        ShadeTexture *c = &g_shade.textures[g_shade.tex_hint[bucket] - 1];

        if (c->res && c->offset == src->offset && c->width == src->width
                && c->height == src->height && c->depth == depth
                && c->format == src->format && c->palette == src->palette
                && c->levels == levels && c->cube == src->cube
                && c->dims == (int)src->dims)
            t = c;
    }

    for (i = 0; !t && i < SHADE_MAX_TEXTURES; i++) {
        ShadeTexture *c = &g_shade.textures[i];

        if (!c->res) {
            if (!spare)
                spare = c;
            continue;
        }
        if (c->offset == src->offset && c->width == src->width
                && c->height == src->height && c->depth == depth
                && c->format == src->format && c->palette == src->palette
                && c->levels == levels && c->cube == src->cube
                && c->dims == (int)src->dims) {
            t = c;
            break;
        }
        if (c->used_frame < oldest->used_frame)
            oldest = c;
    }

    if (!t) {
        t = spare ? spare : oldest;
        shade_texture_release(t);
        t->offset = src->offset;
        t->width = src->width;
        t->height = src->height;
        t->depth = depth;
        t->format = src->format;
        t->palette = src->palette;
        t->levels = levels;
        t->cube = src->cube;
        t->dims = (int)src->dims;
        t->checked_frame = (uint32_t)-1;
        if (!shade_texture_create(t)) {
            memset(t, 0, sizeof *t);
            return NULL;
        }
        fresh = 1;
    }

    g_shade.tex_hint[bucket] = (uint16_t)(t - g_shade.textures + 1);
    t->used_frame = frame;
    if (t->checked_frame != frame) {
        uint64_t h = src->bytes ? shade_hash(mem + src->offset, src->bytes)
                                : (uint64_t)frame + 1;

        if (src->palette)
            h ^= shade_hash(mem + src->palette, 1024) * 0x9E3779B97F4A7C15ull;
        t->checked_frame = frame;
        if (fresh || h != t->hash) {
            t->hash = h;
            if (!shade_texture_upload(t, src->stage)) {
                /* A format the executor cannot read: leave the stage
                 * unbound, and say which. */
                fprintf(stderr, "[NV2A-D3D11] shaded: stage %u's texture"
                                " 0x%08X (format 0x%02X) cannot be decoded\n",
                        src->stage, src->offset, src->format);
                fflush(stderr);
                shade_texture_release(t);
                return NULL;
            }
        }
    }
    return t->srv;
}

/* ── Samplers ──────────────────────────────────────────────── */

static D3D11_TEXTURE_ADDRESS_MODE shade_address(uint32_t nv)
{
    switch (nv) {
    case 1:  return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4:  return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;    /* 3 and 5 */
    }
}

/* The sampler a stage asks for: SET_TEXTURE_FILTER and _ADDRESS as the
 * title wrote them, read the way the sink reads stage 0's. */
static ID3D11SamplerState *shade_sampler(const Nv2aSinkTexture *src)
{
    uint32_t mag = (src->filter >> 24) & 0xFu, min = (src->filter >> 16) & 0xFFu;
    int32_t bias_fixed = (int32_t)(src->filter & 0x1FFFu);
    int min_linear = 0, mip = 0, mip_linear = 0, mips = src->levels > 1, i;
    D3D11_SAMPLER_DESC sd;
    ShadeSampler *s;

    for (i = 0; i < g_shade.sampler_count; i++) {
        s = &g_shade.samplers[i];
        if (s->filter == src->filter && s->addr_u == src->addr_u
                && s->addr_v == src->addr_v && s->addr_p == src->addr_p
                && s->mips == mips && s->cube == src->cube)
            return s->state;
    }
    if (g_shade.sampler_count == SHADE_MAX_SAMPLERS) {
        /* ponytail: start over when the table is full; it never has been. */
        for (i = 0; i < SHADE_MAX_SAMPLERS; i++)
            if (g_shade.samplers[i].state)
                ID3D11SamplerState_Release(g_shade.samplers[i].state);
        g_shade.sampler_count = 0;
    }

    switch (min) {
    case 2: min_linear = 1;                              break;
    case 3:                 mip = 1;                     break;
    case 4: min_linear = 1; mip = 1;                     break;
    case 5:                 mip = 1; mip_linear = 1;     break;
    case 6: min_linear = 1; mip = 1; mip_linear = 1;     break;
    case 7: min_linear = 1;                              break;
    default: break;                         /* 1 nearest, 0 never set */
    }
    if (!mips)
        mip = mip_linear = 0;
    if (bias_fixed & 0x1000)
        bias_fixed -= 0x2000;

    memset(&sd, 0, sizeof sd);
    /* D3D11_FILTER is three bits: linear minification 0x10, linear
     * magnification 0x4, linear between levels 0x1. */
    sd.Filter = (D3D11_FILTER)((min_linear ? 0x10 : 0) | (mag >= 2 ? 0x4 : 0)
                               | (mip_linear ? 0x1 : 0));
    sd.AddressU = src->cube ? D3D11_TEXTURE_ADDRESS_CLAMP
                            : shade_address(src->addr_u);
    sd.AddressV = src->cube ? D3D11_TEXTURE_ADDRESS_CLAMP
                            : shade_address(src->addr_v);
    sd.AddressW = src->cube ? D3D11_TEXTURE_ADDRESS_CLAMP
                            : shade_address(src->addr_p);
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = mip ? D3D11_FLOAT32_MAX : 0.0f;
    sd.MipLODBias = mip ? (float)bias_fixed / 256.0f : 0.0f;

    s = &g_shade.samplers[g_shade.sampler_count];
    memset(s, 0, sizeof *s);
    if (FAILED(ID3D11Device_CreateSamplerState(d3d8_GetD3D11Device(), &sd,
                                               &s->state)))
        return NULL;
    s->filter = src->filter;
    s->addr_u = src->addr_u;
    s->addr_v = src->addr_v;
    s->addr_p = src->addr_p;
    s->mips = mips;
    s->cube = src->cube;
    g_shade.sampler_count++;
    return s->state;
}

/* ── Shaders ───────────────────────────────────────────────── */

/* Whether a stage's mode looks anything up in the stage's texture. */
static int mode_samples(uint32_t mode)
{
    switch (mode) {
    case 0x01: case 0x02: case 0x03:        /* projective, cube map */
    case 0x09: case 0x0B: case 0x0C:        /* the dot-product lookups */
    case 0x0D: case 0x0E:
    case 0x0F: case 0x10:                   /* dependent on another stage */
        return 1;
    default:
        return 0;
    }
}

/* The pixel shader for a setup, written and compiled the first time the
 * setup is seen. Each new one is named in the log, which is how a
 * play-through says what a title asks for. */
static ID3D11PixelShader *shade_shader(const Nv2aPshKey *key)
{
    static char src[32768];
    uint32_t unsupported = 0;
    ShadeShader *s;
    ID3DBlob *blob;
    int i, len;

    for (i = 0; i < SHADE_MAX_SHADERS; i++) {
        s = &g_shade.shaders[i];
        if (s->used && memcmp(&s->key, key, sizeof *key) == 0)
            return s->ps;
    }
    s = &g_shade.shaders[g_shade.shader_next];
    g_shade.shader_next = (g_shade.shader_next + 1) % SHADE_MAX_SHADERS;
    if (s->ps)
        ID3D11PixelShader_Release(s->ps);
    memset(s, 0, sizeof *s);
    s->key = *key;
    s->used = 1;

    fprintf(stderr, "[NV2A-D3D11] shaded: stage program %05X, textures"
                    " %u%u%u%u, %u combiner stage(s), s0 icw %08X %08X ocw"
                    " %08X %08X, final %08X %08X, alpha test %X\n",
            key->rc.stage_program, key->tex_kind[0], key->tex_kind[1],
            key->tex_kind[2], key->tex_kind[3], key->rc.control & 0xFFu,
            key->rc.color_icw[0], key->rc.alpha_icw[0], key->rc.color_ocw[0],
            key->rc.alpha_ocw[0], key->rc.final0, key->rc.final1,
            key->alpha_func);

    len = nv2a_psh_source(key, src, sizeof src, &unsupported);
    if (len < 0) {
        fprintf(stderr, "[NV2A-D3D11] shaded: the shader is too long\n");
        fflush(stderr);
        return NULL;
    }
    if (unsupported & 0xFu)
        fprintf(stderr, "[NV2A-D3D11] shaded: stage mode(s) not translated"
                        " and read as zero (stages, by bit: %X)\n",
                unsupported & 0xFu);
    if (unsupported & 0xF0u)
        fprintf(stderr, "[NV2A-D3D11] shaded: dot mapping %03X is not"
                        " translated and read as 0..1\n", key->dot_mapping);
    /* RECOMP_D3D11_SHADE_DUMP: the source of each shader as it is made. */
    if (getenv("RECOMP_D3D11_SHADE_DUMP"))
        fprintf(stderr, "%s", src);
    fflush(stderr);

    blob = shade_compile(src, (size_t)len, "ps_4_0");
    if (!blob)
        return NULL;
    if (FAILED(ID3D11Device_CreatePixelShader(d3d8_GetD3D11Device(),
                ID3D10Blob_GetBufferPointer(blob),
                ID3D10Blob_GetBufferSize(blob), NULL, &s->ps)))
        s->ps = NULL;
    ID3D10Blob_Release(blob);
    return s->ps;
}

/* ── Drawing ───────────────────────────────────────────────── */

static void unpack_argb(uint32_t c, float out[4])
{
    out[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    out[1] = (float)((c >>  8) & 0xFF) / 255.0f;
    out[2] = (float)( c        & 0xFF) / 255.0f;
    out[3] = (float)( c >> 24        ) / 255.0f;
}

static int shade_vertices(const Nv2aShadeVertex *v, uint32_t count)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    UINT bytes = count * (UINT)sizeof *v;
    D3D11_MAPPED_SUBRESOURCE map;

    if (bytes > g_shade.vb_bytes) {
        D3D11_BUFFER_DESC bd;

        if (g_shade.vb)
            ID3D11Buffer_Release(g_shade.vb);
        g_shade.vb = NULL;
        g_shade.vb_bytes = 0;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = bytes < (1u << 20) ? (1u << 20) : bytes;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &bd,
                                             NULL, &g_shade.vb)))
            return 0;
        g_shade.vb_bytes = bd.ByteWidth;
    }
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_shade.vb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &map)))
        return 0;
    memcpy(map.pData, v, bytes);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_shade.vb, 0);
    return 1;
}

/* What a batch's pixels need: the pixel shader for the setup, its constants
 * written, and the views and samplers of the stages that look something up.
 * Returns 0 if there is nothing to draw with. */
static int shade_pixel(const Nv2aSinkShade *shade, const Nv2aSinkState *state,
                       uint32_t frame, ID3D11PixelShader **ps_out,
                       ID3D11ShaderResourceView *srv[4],
                       ID3D11SamplerState *samp[4])
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11PixelShader *ps;
    Nv2aPshConstants pc;
    Nv2aPshKey key;
    uint32_t stages = shade->rc.control & 0xFFu, k;
    int st;

    /* What the shader's text depends on, and what it reads as constants. */
    memset(&key, 0, sizeof key);
    memset(&pc, 0, sizeof pc);
    key.rc = shade->rc;
    if (stages > 8)
        stages = 8;
    for (k = 0; k < 8; k++) {
        unpack_argb(shade->rc.factor0[k], pc.factor0[k]);
        unpack_argb(shade->rc.factor1[k], pc.factor1[k]);
        key.rc.factor0[k] = key.rc.factor1[k] = 0;
        /* A stage past the count is not run, whatever is left in it. */
        if (k >= stages)
            key.rc.color_icw[k] = key.rc.alpha_icw[k] =
                key.rc.color_ocw[k] = key.rc.alpha_ocw[k] = 0;
    }
    unpack_argb(shade->rc.final_c0, pc.final_c0);
    unpack_argb(shade->rc.final_c1, pc.final_c1);
    key.rc.final_c0 = key.rc.final_c1 = 0;
    unpack_argb(shade->fog_color, pc.fog_color);
    /* Only what a stage's mode reads of these three, so that a word left
     * over from other work does not make a second shader of the same text. */
    for (st = 0; st < 4; st++) {
        uint32_t mode = (shade->rc.stage_program >> (st * 5)) & 0x1Fu;
        int dot = mode == 0x09 || (mode >= 0x0B && mode <= 0x0E)
               || mode == 0x11;

        if (mode == 0x05)
            key.clip_plane_mode |= shade->clip_plane_mode & (0xFu << (st * 4));
        if (st >= 1 && dot)
            key.dot_mapping |= shade->dot_mapping & (0xFu << ((st - 1) * 4));
        /* A reflection in stage 2 reads stage 3's row as well. */
        if (st == 3 && (dot || ((shade->rc.stage_program >> 10) & 0x1Fu) == 0x0B))
            key.dot_mapping |= shade->dot_mapping & 0xF00u;
        if (st >= 2 && (dot || mode == 0x0F || mode == 0x10))
            key.other_stage_input |= shade->other_stage_input
                                   & (0xFu << (st == 2 ? 16 : 20));
    }
    if (((shade->rc.stage_program >> 10) & 0x1Fu) == 0x0B)
        key.other_stage_input |= shade->other_stage_input & 0x00F00000u;
    /* A test that always passes is no test. */
    if (state->alpha_test_enable && state->alpha_func
            && (state->alpha_func & 7u) != 7u) {
        key.alpha_func = state->alpha_func;
        pc.alpha_ref = (float)(state->alpha_ref & 0xFFu);
    }

    for (st = 0; st < 4; st++) {
        const Nv2aSinkTexture *t = &shade->tex[st];
        uint32_t mode = (shade->rc.stage_program >> (st * 5)) & 0x1Fu;

        pc.tex_scale[st][0] = pc.tex_scale[st][1] = pc.tex_scale[st][2] = 1.0f;
        if (!mode_samples(mode))
            continue;
        srv[st] = shade_texture(t, frame);
        if (!srv[st])
            continue;
        samp[st] = shade_sampler(t);
        key.tex_kind[st] = t->cube ? NV2A_PSH_TEX_CUBE
                         : t->dims == 3 ? NV2A_PSH_TEX_3D : NV2A_PSH_TEX_2D;
        key.alpha_kill[st] = (t->control0 >> 2) & 1u;
        if (t->rect) {
            pc.tex_scale[st][0] = 1.0f / (float)t->width;
            pc.tex_scale[st][1] = 1.0f / (float)t->height;
            pc.tex_scale[st][2] = 1.0f / (float)(t->depth ? t->depth : 1);
        }
    }

    ps = shade_shader(&key);
    if (!ps)
        return 0;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_shade.ps_cb,
                                          0, D3D11_MAP_WRITE_DISCARD, 0,
                                          &map))) {
        memcpy(map.pData, &pc, sizeof pc);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_shade.ps_cb, 0);
    }
    *ps_out = ps;
    return 1;
}

/* Bind the pixel side of a batch. The D3D8 layer's own sampler binding comes
 * with d3d8_states_apply(), so ours go on after it. */
static void shade_bind_pixel(ID3D11DeviceContext *ctx, ID3D11PixelShader *ps,
                             ID3D11ShaderResourceView *srv[4],
                             ID3D11SamplerState *samp[4])
{
    int st;

    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_shade.ps_cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 4, srv);
    for (st = 0; st < 4; st++)
        if (samp[st])
            ID3D11DeviceContext_PSSetSamplers(ctx, (UINT)st, 1, &samp[st]);
}

/* The D3D8 layer binds its own shaders, layout, buffers and samplers for
 * every batch, and stage 0's texture when the sink next draws; the other
 * three stages it never touches, so they are cleared here. */
static void shade_unbind_pixel(ID3D11DeviceContext *ctx)
{
    ID3D11ShaderResourceView *none[4] = { NULL, NULL, NULL, NULL };

    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 4, none);
}

int nv2a_d3d11_shade_draw(const Nv2aShadeVertex *v, uint32_t count,
                          const Nv2aSinkShade *shade,
                          const Nv2aSinkState *state, uint32_t frame)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    ID3D11ShaderResourceView *srv[4] = { NULL, NULL, NULL, NULL };
    ID3D11SamplerState *samp[4] = { NULL, NULL, NULL, NULL };
    ID3D11PixelShader *ps;
    D3D11_MAPPED_SUBRESOURCE map;
    UINT stride = sizeof *v, offset = 0;

    if (!ctx || count < 3 || !shade_init())
        return 0;
    if (!shade_pixel(shade, state, frame, &ps, srv, samp)
            || !shade_vertices(v, count))
        return 0;

    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_shade.vs_cb,
                                          0, D3D11_MAP_WRITE_DISCARD, 0,
                                          &map))) {
        float screen[4] = { (float)d3d8_GetBackbufferWidth(),
                            (float)d3d8_GetBackbufferHeight(), 0.0f, 0.0f };
        memcpy(map.pData, screen, sizeof screen);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_shade.vs_cb, 0);
    }

    /* The output merger and rasteriser as the sink has just set them on the
     * D3D8 device. It also binds that device's samplers, so ours go on
     * after it. */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetInputLayout(ctx, g_shade.layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_shade.vb, &stride,
                                           &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, g_shade.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_shade.vs_cb);
    shade_bind_pixel(ctx, ps, srv, samp);
    ID3D11DeviceContext_Draw(ctx, count, 0);
    shade_unbind_pixel(ctx);
    return 1;
}

/* ── Vertex programs on the GPU ────────────────────────────── */

/* A program's vertex shader is the translation of the title's program
 * (nv2a_vsh_hlsl.c) followed by what sink_vertex and the shaded path's
 * vertex shader do to the interpreter's answer, so that a batch lands on the
 * same pixels and depths as one the executor transformed itself:
 *
 *   - w of zero (not minus zero) is one, and x and y are scaled for an
 *     anti-aliased surface;
 *   - depth is the depth buffer's value over its largest; what is a hair
 *     beyond the far plane, or on it, is put just short of it, and what is
 *     well beyond it is kept when the title did not ask for the cut (the
 *     rasteriser clips at the plane);
 *   - the title's pixels become the host's (Map), and those pixels clip space
 *     (Screen is the back buffer's size), with w kept;
 *   - the diffuse colour is white when the program never wrote one, fog is
 *     worked out from oFog.x as the executor does.
 *
 * Vertices are read from VData by SV_VertexID, so the draw is indexed and
 * needs no vertex buffer or input layout. */
static const char g_prog_wrapper[] =
    "cbuffer ProgParams : register(b1) {\n"
    "    float4 Screen;\n"                  /* back buffer w, h */
    "    float4 Map;\n"                     /* ox, sx, oy, sy */
    "    float4 Aa;\n"                      /* aa x, aa y, zeta max, depth add */
    "    float4 Far;\n"                     /* far cull, slack, far depth */
    "    float4 Fog;\n"                     /* mode, p0, p1 */
    "    uint4  Attr;\n"                    /* array mask, arrays */
    "    float4 Imm[16];\n"
    "};\n"
    "StructuredBuffer<float4> VData : register(t0);\n"
    "struct VS_OUT {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 d0  : COLOR0;\n"
    "    float4 d1  : COLOR1;\n"
    "    float4 t0  : TEXCOORD0;\n"
    "    float4 t1  : TEXCOORD1;\n"
    "    float4 t2  : TEXCOORD2;\n"
    "    float4 t3  : TEXCOORD3;\n"
    "    float  fog : TEXCOORD4;\n"
    "};\n"
    "float fog_factor(float d)\n"
    "{\n"
    "    uint mode = (uint)Fog.x;\n"
    "    float f;\n"
    "    if (mode == 0) return 1.0;\n"
    "    if (mode == 0x800 || mode == 0x802) f = Fog.y + exp2(d * Fog.z * 16.0) - 1.5;\n"
    "    else if (mode == 0x801 || mode == 0x803)\n"
    "        f = Fog.y + exp2(-d * d * Fog.z * Fog.z * 32.0) - 1.5;\n"
    "    else f = Fog.y + d * Fog.z - 1.0;\n"
    "    if (mode == 0x802 || mode == 0x803 || mode == 0x804) f = abs(f);\n"
    "    if (f != f) f = 1.0;\n"
    "    return saturate(f);\n"
    "}\n"
    "VS_OUT main(uint vid : SV_VertexID)\n"
    "{\n"
    "    float4 v[16];\n"
    "    uint n = 0;\n"
    "    [unroll] for (uint i = 0; i < 16; i++) {\n"
    "        if (Attr.x & (1u << i)) { v[i] = VData[vid * Attr.y + n]; n++; }\n"
    "        else v[i] = Imm[i];\n"
    "    }\n"
    "    VshOut r = vsh_run(v);\n"
    "    float4 p = r.pos;\n"
    "    if (p.w == 0.0 && asuint(p.w) == 0u) p.w = 1.0;\n"
    "    p.x *= Aa.x; p.y *= Aa.y;\n"
    "    float z = p.z / Aa.z;\n"
    "    if (!(z > -1.0e30 && z < 1.0e30)) z = 1.0;\n"
    "    if (p.w > 0.0 && z > 1.0 && !(Far.x != 0.0 && z > 1.0 + Far.y)) z = Far.z;\n"
    "    else if (z >= 1.0 - Far.y && z <= 1.0 + Far.y) z = Far.z;\n"
    "    z += Aa.w;\n"
    "    float nx = (Map.x + p.x * Map.y) / Screen.x * 2.0 - 1.0;\n"
    "    float ny = 1.0 - (Map.z + p.y * Map.w) / Screen.y * 2.0;\n"
    "    VS_OUT o;\n"
    "    o.pos = float4(nx * p.w, ny * p.w, z * p.w, p.w);\n"
    "    o.d0 = VSH_D0_WRITTEN ? r.d0 : float4(1, 1, 1, 1);\n"
    "    o.d1 = r.d1;\n"
    "    o.t0 = r.t0; o.t1 = r.t1; o.t2 = r.t2; o.t3 = r.t3;\n"
    "    o.fog = fog_factor(r.fog.x);\n"
    "    return o;\n"
    "}\n";

/* What the wrapper reads in b1, as HLSL lays it out. */
typedef struct {
    float    screen[4], map[4], aa[4], far_[4], fog[4];
    uint32_t attr[4];
    float    imm[16][4];
} ProgParams;

#define SHADE_MAX_PROGRAMS 128

typedef struct {
    uint64_t           hash;
    int                used, usable;
    ID3D11VertexShader *vs;
} ShadeProgram;

static struct {
    ShadeProgram       programs[SHADE_MAX_PROGRAMS];
    int                next;
    ShadeProgram      *last;            /* what `version` was made into */
    uint32_t           last_version;
    ID3D11Buffer      *vc_cb, *pp_cb;
    ID3D11Buffer      *vdata, *ibuf;
    ID3D11ShaderResourceView *vdata_srv;
    UINT               vdata_bytes, ibuf_bytes;
} g_prog;

static uint64_t program_hash(const Nv2aSinkProgram *b)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t s, i;

    h = (h ^ b->start) * 1099511628211ull;
    for (s = b->start; s < NV2A_VSH_HLSL_SLOTS; s++) {
        for (i = 0; i < 4; i++)
            h = (h ^ b->words[s][i]) * 1099511628211ull;
        if (b->words[s][3] & 1u)
            break;
    }
    return h;
}

static ShadeProgram *shade_program(const Nv2aSinkProgram *b)
{
    static char src[1 << 18];
    Nv2aVshHlslInfo info;
    ShadeProgram *p;
    ID3DBlob *blob;
    uint64_t h;
    int i, n;

    if (g_prog.last && g_prog.last_version == b->version)
        return g_prog.last;
    h = program_hash(b);
    for (i = 0; i < SHADE_MAX_PROGRAMS; i++)
        if (g_prog.programs[i].used && g_prog.programs[i].hash == h) {
            g_prog.last = &g_prog.programs[i];
            g_prog.last_version = b->version;
            return g_prog.last;
        }
    p = &g_prog.programs[g_prog.next];
    g_prog.next = (g_prog.next + 1) % SHADE_MAX_PROGRAMS;
    if (p->vs)
        ID3D11VertexShader_Release(p->vs);
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->hash = h;
    g_prog.last = p;
    g_prog.last_version = b->version;

    n = nv2a_vsh_source(b->words, b->start, nv2a_vsh_cxt_write(), src,
                        sizeof src - sizeof g_prog_wrapper - 128, &info);
    if (n < 0 || info.writes_constants) {
        fprintf(stderr, "[NV2A-D3D11] vertex program at slot %u stays on the"
                        " CPU (%s)\n", b->start,
                n < 0 ? "no translation" : "it writes constants");
        fflush(stderr);
        return p;
    }
    n += snprintf(src + n, 128, "#define VSH_D0_WRITTEN %d\n",
                  (info.written >> 3) & 1);
    /* The wrapper needs the definition above before main, and the program
     * before the wrapper: the macro goes in the translation's place. */
    memcpy(src + n, g_prog_wrapper, sizeof g_prog_wrapper);
    blob = shade_compile(src, (size_t)n + sizeof g_prog_wrapper - 1, "vs_5_0");
    if (!blob)
        return p;
    if (SUCCEEDED(ID3D11Device_CreateVertexShader(d3d8_GetD3D11Device(),
                ID3D10Blob_GetBufferPointer(blob),
                ID3D10Blob_GetBufferSize(blob), NULL, &p->vs)))
        p->usable = 1;
    ID3D10Blob_Release(blob);
    if (getenv("RECOMP_D3D11_SHADE_DUMP"))
        fprintf(stderr, "[NV2A-D3D11] vertex program %016llX, %u slots\n%s",
                (unsigned long long)h, info.slots, src);
    return p;
}

/* A dynamic buffer of at least `bytes`, made again if it is too small. */
static ID3D11Buffer *prog_buffer(ID3D11Buffer **buf, UINT *have, UINT bytes,
                                 UINT bind, UINT misc, UINT stride)
{
    D3D11_BUFFER_DESC bd;

    if (*buf && bytes <= *have)
        return *buf;
    if (*buf)
        ID3D11Buffer_Release(*buf);
    *buf = NULL;
    *have = 0;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = bytes < (1u << 18) ? (1u << 18) : bytes;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = bind;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = misc;
    bd.StructureByteStride = stride;
    if (FAILED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &bd, NULL, buf)))
        return NULL;
    *have = bd.ByteWidth;
    return *buf;
}

static int prog_upload(ID3D11DeviceContext *ctx, ID3D11Buffer *buf,
                       const void *data, UINT bytes)
{
    D3D11_MAPPED_SUBRESOURCE map;

    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)buf, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &map)))
        return 0;
    memcpy(map.pData, data, bytes);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)buf, 0);
    return 1;
}

int nv2a_d3d11_shade_draw_program(const Nv2aSinkProgram *b,
                                  const float map[4],
                                  const Nv2aSinkShade *shade,
                                  const Nv2aSinkState *state, uint32_t frame)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    ID3D11ShaderResourceView *srv[4] = { NULL, NULL, NULL, NULL };
    ID3D11SamplerState *samp[4] = { NULL, NULL, NULL, NULL };
    ID3D11ShaderResourceView *none = NULL;
    ID3D11Buffer *cbs[2];
    ID3D11PixelShader *ps;
    ShadeProgram *prog;
    ProgParams pp;
    UINT vbytes = b->nverts * b->narrays * 16u, ibytes = b->nindex * 4u, i;

    if (!ctx || b->nindex < 3 || !shade_init())
        return 0;
    prog = shade_program(b);
    if (!prog || !prog->usable)
        return 0;
    if (!g_prog.vc_cb) {
        D3D11_BUFFER_DESC bd;

        memset(&bd, 0, sizeof bd);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = NV2A_VSH_HLSL_CBUFFER_BYTES;
        if (FAILED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &bd, NULL,
                                             &g_prog.vc_cb)))
            return 0;
        bd.ByteWidth = (sizeof(ProgParams) + 15u) & ~15u;
        if (FAILED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &bd, NULL,
                                             &g_prog.pp_cb)))
            return 0;
    }
    /* A batch whose arrays are all constant has nothing to read; one float4
     * keeps the buffer a valid size. */
    if (!vbytes)
        vbytes = 16;
    {
        ID3D11Buffer *before = g_prog.vdata;

        if (!prog_buffer(&g_prog.vdata, &g_prog.vdata_bytes, vbytes,
                         D3D11_BIND_SHADER_RESOURCE,
                         D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16))
            return 0;
        if (g_prog.vdata != before || !g_prog.vdata_srv) {
            if (g_prog.vdata_srv)
                ID3D11ShaderResourceView_Release(g_prog.vdata_srv);
            g_prog.vdata_srv = NULL;
            if (FAILED(ID3D11Device_CreateShaderResourceView(
                    d3d8_GetD3D11Device(), (ID3D11Resource *)g_prog.vdata,
                    NULL, &g_prog.vdata_srv)))
                return 0;
        }
    }
    if (!prog_buffer(&g_prog.ibuf, &g_prog.ibuf_bytes, ibytes,
                     D3D11_BIND_INDEX_BUFFER, 0, 0))
        return 0;
    if (!shade_pixel(shade, state, frame, &ps, srv, samp))
        return 0;

    memset(&pp, 0, sizeof pp);
    pp.screen[0] = (float)d3d8_GetBackbufferWidth();
    pp.screen[1] = (float)d3d8_GetBackbufferHeight();
    memcpy(pp.map, map, sizeof pp.map);
    pp.aa[0] = b->aa_scale[0];
    pp.aa[1] = b->aa_scale[1];
    pp.aa[2] = b->zeta_max;
    pp.aa[3] = b->depth_add;
    pp.far_[0] = b->far_cull ? 1.0f : 0.0f;
    pp.far_[1] = b->far_slack;
    pp.far_[2] = b->far_depth;
    pp.fog[0] = (float)b->fog_mode;
    pp.fog[1] = b->fog_param[0];
    pp.fog[2] = b->fog_param[1];
    pp.attr[0] = b->array_mask;
    pp.attr[1] = b->narrays;
    for (i = 0; i < 16; i++)
        memcpy(pp.imm[i], b->constant[i], sizeof pp.imm[i]);

    if (b->narrays && !prog_upload(ctx, g_prog.vdata, b->vertex, vbytes))
        return 0;
    if (!prog_upload(ctx, g_prog.ibuf, b->index, ibytes)
            || !prog_upload(ctx, g_prog.vc_cb, b->constants,
                            NV2A_VSH_HLSL_CBUFFER_BYTES)
            || !prog_upload(ctx, g_prog.pp_cb, &pp,
                            (sizeof pp + 15u) & ~15u))
        return 0;

    d3d8_states_apply();

    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, g_prog.ibuf,
                                         DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, prog->vs, NULL, 0);
    cbs[0] = g_prog.vc_cb;
    cbs[1] = g_prog.pp_cb;
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 2, cbs);
    ID3D11DeviceContext_VSSetShaderResources(ctx, 0, 1, &g_prog.vdata_srv);
    shade_bind_pixel(ctx, ps, srv, samp);
    ID3D11DeviceContext_DrawIndexed(ctx, b->nindex, 0, 0);
    shade_unbind_pixel(ctx);
    ID3D11DeviceContext_VSSetShaderResources(ctx, 0, 1, &none);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, NULL, DXGI_FORMAT_UNKNOWN, 0);
    return 1;
}

#endif /* _WIN32 */
