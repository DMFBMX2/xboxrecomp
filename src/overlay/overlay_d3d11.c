/*
 * The overlay's drawing, in Direct3D 11.
 *
 * Text comes from a glyph atlas made with GDI from a monospace system font:
 * Consolas, or Courier New where that is missing. It is made again when the
 * render target's height wants another size, so the panel is as readable at
 * 4K as it is at 480p. A cell of the atlas is solid white, which is what
 * rectangles are drawn from, so a whole panel is one draw call with one
 * texture.
 *
 * It draws between the end of a frame and its Present, on a context that is
 * in the middle of someone else's rendering, so everything it changes is
 * read first and put back in ovl_gfx_end.
 */
#ifdef _WIN32

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COBJMACROS
#include <d3d11.h>
#include <d3dcompiler.h>

#include "overlay_gfx.h"

#define OVL_MAX_QUADS   4096
#define OVL_FIRST_CHAR  32
#define OVL_LAST_CHAR   126
#define OVL_ATLAS_COLS  16
#define OVL_ATLAS_ROWS  7           /* six of glyphs, then the solid cell */
#define OVL_FONT_PX_1080 16         /* the font's height at 1080 lines */
#define OVL_FONT_PX_MIN  12

typedef struct {
    float    x, y, u, v;
    uint32_t colour;
} Vertex;

typedef struct {
    ID3D11RenderTargetView  *rtv;
    ID3D11DepthStencilView  *dsv;
    ID3D11BlendState        *blend;
    FLOAT                    blend_factor[4];
    UINT                     blend_mask;
    ID3D11DepthStencilState *depth;
    UINT                     stencil_ref;
    ID3D11RasterizerState   *raster;
    D3D11_VIEWPORT           viewport;
    UINT                     viewports;
    ID3D11InputLayout       *layout;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11Buffer            *vb;
    UINT                     vb_stride, vb_offset;
    ID3D11VertexShader      *vs;
    ID3D11PixelShader       *ps;
    ID3D11GeometryShader    *gs;
    ID3D11ShaderResourceView *srv;
    ID3D11SamplerState      *sampler;
} Saved;

static struct {
    ID3D11Device            *device;
    ID3D11DeviceContext     *ctx;
    int                      failed;
    ID3D11VertexShader      *vs;
    ID3D11PixelShader       *ps;
    ID3D11InputLayout       *layout;
    ID3D11Buffer            *vb;
    ID3D11BlendState        *blend;
    ID3D11DepthStencilState *depth;
    ID3D11RasterizerState   *raster;
    ID3D11SamplerState      *sampler;
    /* The atlas, for the font size it was made at. */
    ID3D11Texture2D         *atlas;
    ID3D11ShaderResourceView *atlas_srv;
    unsigned                 font_px, cw, ch, atlas_w, atlas_h;
    /* This frame. */
    Saved                    saved;
    unsigned                 target_w, target_h;
    Vertex                   quads[OVL_MAX_QUADS * 6];
    unsigned                 count;
} g;

static const char shader_source[] =
    "Texture2D atlas : register(t0);\n"
    "SamplerState point_clamp : register(s0);\n"
    "struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0;\n"
    "              float4 colour : COLOR0; };\n"
    "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0;\n"
    "              float4 colour : COLOR0; };\n"
    "PSIn vs(VSIn i) {\n"
    "  PSIn o; o.pos = float4(i.pos, 0, 1); o.uv = i.uv; o.colour = i.colour;\n"
    "  return o; }\n"
    "float4 ps(PSIn i) : SV_Target {\n"
    "  return atlas.Sample(point_clamp, i.uv) * i.colour; }\n";

static void release_atlas(void)
{
    if (g.atlas_srv) { ID3D11ShaderResourceView_Release(g.atlas_srv); g.atlas_srv = NULL; }
    if (g.atlas) { ID3D11Texture2D_Release(g.atlas); g.atlas = NULL; }
}

static int create_pipeline(void)
{
    static const D3D11_INPUT_ELEMENT_DESC elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3DBlob *vs = NULL, *ps = NULL;
    D3D11_BUFFER_DESC bd = { 0 };
    D3D11_BLEND_DESC blend = { 0 };
    D3D11_DEPTH_STENCIL_DESC depth = { 0 };
    D3D11_RASTERIZER_DESC raster = { 0 };
    D3D11_SAMPLER_DESC sampler = { 0 };
    HRESULT hr;

    hr = D3DCompile(shader_source, sizeof shader_source - 1, NULL, NULL, NULL,
                    "vs", "vs_4_0", 0, 0, &vs, NULL);
    if (SUCCEEDED(hr))
        hr = D3DCompile(shader_source, sizeof shader_source - 1, NULL, NULL,
                        NULL, "ps", "ps_4_0", 0, 0, &ps, NULL);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateVertexShader(g.device,
                ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs),
                NULL, &g.vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreatePixelShader(g.device,
                ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps),
                NULL, &g.ps);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(g.device, elements, 3,
                ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs),
                &g.layout);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
    if (FAILED(hr))
        return 0;

    bd.ByteWidth = sizeof g.quads;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(g.device, &bd, NULL, &g.vb);
    if (FAILED(hr))
        return 0;

    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW =
        D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    return SUCCEEDED(ID3D11Device_CreateBlendState(g.device, &blend, &g.blend))
        && SUCCEEDED(ID3D11Device_CreateDepthStencilState(g.device, &depth,
                                                          &g.depth))
        && SUCCEEDED(ID3D11Device_CreateRasterizerState(g.device, &raster,
                                                        &g.raster))
        && SUCCEEDED(ID3D11Device_CreateSamplerState(g.device, &sampler,
                                                     &g.sampler));
}

/* The atlas for a font `px` high. */
static int create_atlas(unsigned px)
{
    BITMAPINFO bmi = { 0 };
    D3D11_TEXTURE2D_DESC td = { 0 };
    D3D11_SUBRESOURCE_DATA init;
    HDC dc;
    HFONT font, old_font = NULL;
    HBITMAP dib, old_bitmap = NULL;
    TEXTMETRICA tm = { 0 };
    uint32_t *pixels = NULL, *out = NULL;
    unsigned x, y, c, cw, ch;
    int ok = 0;
    static const char *const faces[] = { "Consolas", "Courier New" };
    unsigned face;

    dc = CreateCompatibleDC(NULL);
    if (!dc)
        return 0;
    for (face = 0; face < 2; face++) {
        font = CreateFontA(-(int)px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN,
                           faces[face]);
        if (!font)
            continue;
        old_font = (HFONT)SelectObject(dc, font);
        GetTextMetricsA(dc, &tm);
        break;
    }
    if (!font) {
        DeleteDC(dc);
        return 0;
    }
    cw = (unsigned)tm.tmAveCharWidth;
    ch = (unsigned)tm.tmHeight;
    g.atlas_w = cw * OVL_ATLAS_COLS;
    g.atlas_h = ch * OVL_ATLAS_ROWS;

    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = (LONG)g.atlas_w;
    bmi.bmiHeader.biHeight = -(LONG)g.atlas_h;      /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    if (!dib)
        goto done;
    old_bitmap = (HBITMAP)SelectObject(dc, dib);
    memset(pixels, 0, (size_t)g.atlas_w * g.atlas_h * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (c = OVL_FIRST_CHAR; c <= OVL_LAST_CHAR; c++) {
        char s = (char)c;
        unsigned cell = c - OVL_FIRST_CHAR;

        TextOutA(dc, (int)(cell % OVL_ATLAS_COLS * cw),
                 (int)(cell / OVL_ATLAS_COLS * ch), &s, 1);
    }
    GdiFlush();

    /* White, with the glyph's coverage as alpha; and a solid cell. */
    out = (uint32_t *)malloc((size_t)g.atlas_w * g.atlas_h * 4);
    if (!out)
        goto done;
    for (y = 0; y < g.atlas_h; y++)
        for (x = 0; x < g.atlas_w; x++) {
            uint32_t a = (pixels[y * g.atlas_w + x] >> 8) & 0xFF;

            if (y >= ch * (OVL_ATLAS_ROWS - 1) && x < cw)
                a = 255;
            out[y * g.atlas_w + x] = 0x00FFFFFFu | (a << 24);
        }

    td.Width = g.atlas_w;
    td.Height = g.atlas_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    init.pSysMem = out;
    init.SysMemPitch = g.atlas_w * 4;
    init.SysMemSlicePitch = 0;
    release_atlas();
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g.device, &td, &init, &g.atlas))
            && SUCCEEDED(ID3D11Device_CreateShaderResourceView(g.device,
                    (ID3D11Resource *)g.atlas, NULL, &g.atlas_srv))) {
        g.font_px = px;
        g.cw = cw;
        g.ch = ch;
        ok = 1;
    }

done:
    free(out);
    if (old_bitmap) SelectObject(dc, old_bitmap);
    if (dib) DeleteObject(dib);
    if (old_font) SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    return ok;
}

static void save_state(void)
{
    Saved *s = &g.saved;
    ID3D11DeviceContext *c = g.ctx;

    memset(s, 0, sizeof *s);
    ID3D11DeviceContext_OMGetRenderTargets(c, 1, &s->rtv, &s->dsv);
    ID3D11DeviceContext_OMGetBlendState(c, &s->blend, s->blend_factor,
                                        &s->blend_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &s->depth, &s->stencil_ref);
    ID3D11DeviceContext_RSGetState(c, &s->raster);
    s->viewports = 1;
    ID3D11DeviceContext_RSGetViewports(c, &s->viewports, &s->viewport);
    ID3D11DeviceContext_IAGetInputLayout(c, &s->layout);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &s->topology);
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 1, &s->vb, &s->vb_stride,
                                           &s->vb_offset);
    ID3D11DeviceContext_VSGetShader(c, &s->vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(c, &s->ps, NULL, NULL);
    ID3D11DeviceContext_GSGetShader(c, &s->gs, NULL, NULL);
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 1, &s->srv);
    ID3D11DeviceContext_PSGetSamplers(c, 0, 1, &s->sampler);
}

#define RELEASE(p) do { if (p) IUnknown_Release((IUnknown *)(p)); } while (0)

static void restore_state(void)
{
    Saved *s = &g.saved;
    ID3D11DeviceContext *c = g.ctx;

    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &s->rtv, s->dsv);
    ID3D11DeviceContext_OMSetBlendState(c, s->blend, s->blend_factor,
                                        s->blend_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(c, s->depth, s->stencil_ref);
    ID3D11DeviceContext_RSSetState(c, s->raster);
    ID3D11DeviceContext_RSSetViewports(c, s->viewports, &s->viewport);
    ID3D11DeviceContext_IASetInputLayout(c, s->layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, s->topology);
    ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &s->vb, &s->vb_stride,
                                           &s->vb_offset);
    ID3D11DeviceContext_VSSetShader(c, s->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, s->ps, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, s->gs, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &s->srv);
    ID3D11DeviceContext_PSSetSamplers(c, 0, 1, &s->sampler);

    RELEASE(s->rtv); RELEASE(s->dsv); RELEASE(s->blend); RELEASE(s->depth);
    RELEASE(s->raster); RELEASE(s->layout); RELEASE(s->vb); RELEASE(s->vs);
    RELEASE(s->ps); RELEASE(s->gs); RELEASE(s->srv); RELEASE(s->sampler);
}

int ovl_gfx_begin(void *d3d11_device, void *d3d11_context,
                  unsigned *width, unsigned *height,
                  unsigned *char_w, unsigned *char_h)
{
    ID3D11Resource *res = NULL;
    D3D11_TEXTURE2D_DESC td;
    unsigned px;

    if (g.failed || !d3d11_device || !d3d11_context)
        return 0;
    if (g.device != d3d11_device) {
        /* A different device than the one the resources were made for. */
        g.device = (ID3D11Device *)d3d11_device;
        g.ctx = (ID3D11DeviceContext *)d3d11_context;
        if (!create_pipeline()) {
            g.failed = 1;
            fprintf(stderr, "[OVERLAY] could not create its Direct3D 11"
                            " resources; the overlay is off\n");
            return 0;
        }
    }
    g.ctx = (ID3D11DeviceContext *)d3d11_context;

    save_state();
    if (!g.saved.rtv) {
        restore_state();
        return 0;
    }
    ID3D11RenderTargetView_GetResource(g.saved.rtv, &res);
    if (!res) {
        restore_state();
        return 0;
    }
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    ID3D11Resource_Release(res);
    g.target_w = td.Width;
    g.target_h = td.Height;

    px = (td.Height * OVL_FONT_PX_1080 + 540) / 1080;
    if (px < OVL_FONT_PX_MIN)
        px = OVL_FONT_PX_MIN;
    if (px != g.font_px && !create_atlas(px)) {
        g.failed = 1;
        fprintf(stderr, "[OVERLAY] could not make its font; the overlay is"
                        " off\n");
        restore_state();
        return 0;
    }
    g.count = 0;
    *width = td.Width;
    *height = td.Height;
    *char_w = g.cw;
    *char_h = g.ch;
    return 1;
}

static void add_quad(float x0, float y0, float x1, float y1,
                     float u0, float v0, float u1, float v1, uint32_t colour)
{
    Vertex *v;
    float ax = x0 / (float)g.target_w * 2.0f - 1.0f;
    float bx = x1 / (float)g.target_w * 2.0f - 1.0f;
    float ay = 1.0f - y0 / (float)g.target_h * 2.0f;
    float by = 1.0f - y1 / (float)g.target_h * 2.0f;

    if (g.count >= OVL_MAX_QUADS)
        return;
    v = &g.quads[g.count++ * 6];
    v[0] = (Vertex){ ax, ay, u0, v0, colour };
    v[1] = (Vertex){ bx, ay, u1, v0, colour };
    v[2] = (Vertex){ ax, by, u0, v1, colour };
    v[3] = v[2];
    v[4] = v[1];
    v[5] = (Vertex){ bx, by, u1, v1, colour };
}

void ovl_gfx_rect(float x, float y, float w, float h, uint32_t rgba)
{
    /* The middle of the solid cell, so no edge of it is ever sampled. */
    float u = ((float)g.cw * 0.5f) / (float)g.atlas_w;
    float v = ((float)g.ch * (OVL_ATLAS_ROWS - 1) + (float)g.ch * 0.5f)
              / (float)g.atlas_h;

    add_quad(x, y, x + w, y + h, u, v, u, v, rgba);
}

void ovl_gfx_text(float x, float y, const char *text, uint32_t rgba)
{
    for (; *text; text++, x += (float)g.cw) {
        unsigned c = (unsigned char)*text, cell;
        float u0, v0;

        if (c < OVL_FIRST_CHAR || c > OVL_LAST_CHAR)
            c = '?';
        if (c == ' ')
            continue;
        cell = c - OVL_FIRST_CHAR;
        u0 = (float)(cell % OVL_ATLAS_COLS * g.cw) / (float)g.atlas_w;
        v0 = (float)(cell / OVL_ATLAS_COLS * g.ch) / (float)g.atlas_h;
        add_quad(x, y, x + (float)g.cw, y + (float)g.ch, u0, v0,
                 u0 + (float)g.cw / (float)g.atlas_w,
                 v0 + (float)g.ch / (float)g.atlas_h, rgba);
    }
}

void ovl_gfx_end(void)
{
    ID3D11DeviceContext *c = g.ctx;
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_VIEWPORT vp = { 0 };
    UINT stride = sizeof(Vertex), offset = 0;
    static const FLOAT no_factor[4] = { 0, 0, 0, 0 };

    if (g.count && SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)g.vb,
                                0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, g.quads, (size_t)g.count * 6 * sizeof(Vertex));
        ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)g.vb, 0);

        vp.Width = (FLOAT)g.target_w;
        vp.Height = (FLOAT)g.target_h;
        vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_OMSetRenderTargets(c, 1, &g.saved.rtv, NULL);
        ID3D11DeviceContext_OMSetBlendState(c, g.blend, no_factor, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetDepthStencilState(c, g.depth, 0);
        ID3D11DeviceContext_RSSetState(c, g.raster);
        ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
        ID3D11DeviceContext_IASetInputLayout(c, g.layout);
        ID3D11DeviceContext_IASetPrimitiveTopology(c,
                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &g.vb, &stride,
                                               &offset);
        ID3D11DeviceContext_VSSetShader(c, g.vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(c, g.ps, NULL, 0);
        ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &g.atlas_srv);
        ID3D11DeviceContext_PSSetSamplers(c, 0, 1, &g.sampler);
        ID3D11DeviceContext_Draw(c, g.count * 6, 0);
    }
    g.count = 0;
    restore_state();
}

#endif /* _WIN32 */
