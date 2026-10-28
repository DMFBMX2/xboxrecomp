/* nv2a_vsh_source against nv2a_vsh_run_scalar: the same answers, on a GPU.
 *
 * Programs are made up at random -- every operation, swizzle, mask and
 * destination -- translated to HLSL, compiled, and run by a D3D11 compute
 * shader on the WARP rasteriser over a few dozen vertices each. The
 * interpreter runs the same program on the same vertices, and the two have
 * to agree to within the little a GPU's arithmetic differs by. A lane where
 * the interpreter's answer is not a sensible number is not compared: what a
 * GPU does with a NaN or an infinity is its own business.
 *
 * With a number as argument it runs that many programs (default 300). With a
 * second, a seed.
 */
#define COBJMACROS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_vsh_hlsl.h"
#include "nv2a_vsh_interp.h"

#define VERTS 64u

static uint32_t s_seed = 0x2545F491u;

static uint32_t rnd(void)
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    return s_seed;
}

/* Sensible numbers: -4..4, with some whole ones and zeros. */
static float rnd_float(void)
{
    uint32_t r = rnd();

    switch (r % 6u) {
    case 0: return 0.0f;
    case 1: return (float)((int)((r >> 8) % 9u) - 4);
    default: return ((float)(r >> 8) / 8388608.0f - 1.0f) * 4.0f;
    }
}

static uint32_t s_prog[NV2A_VSH_HLSL_SLOTS][4];

static void random_program(uint32_t len)
{
    uint32_t s;

    for (s = 0; s < len; s++) {
        s_prog[s][0] = 0;
        s_prog[s][1] = rnd() & 0x0FFFFFFFu;
        /* rcc of a zero is a signed infinity, and which one the compiler
         * keeps of a -0 it has folded is its own business: a random program
         * reads registers it never wrote. The title's use of rcc is the
         * epilogue's, tried below with the numbers it sees. */
        if (((s_prog[s][1] >> 25) & 7u) == 3u)
            s_prog[s][1] ^= 1u << 25;
        s_prog[s][2] = rnd();
        s_prog[s][3] = rnd() & ~1u;
        if (rnd() % 3u)
            s_prog[s][3] |= 1u << 11;           /* to o[], not c[] */
        if (s + 1 == len)
            s_prog[s][3] |= 1u;
        nv2a_vsh_set_instruction(s, s_prog[s]);
    }
}

/* A register that nothing has written holds zero, and a compiler that knows
 * it folds -(0) into 0, which the interpreter's -0 is not. Real programs do
 * not read what they have not written; random ones do. */
static const char k_zero[] = "#define VSH_ZERO (vc[0] - vc[0])\n";

static const char k_main[] =
    "StructuredBuffer<float4> vin : register(t0);\n"
    "RWStructuredBuffer<float4> vout : register(u0);\n"
    "[numthreads(64, 1, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    float4 v[16];\n"
    "    for (int i = 0; i < 16; i++) v[i] = vin[id.x * 16 + i];\n"
    "    VshOut o = vsh_run(v);\n"
    "    uint b = id.x * 8;\n"
    "    vout[b] = o.pos; vout[b + 1] = o.d0; vout[b + 2] = o.d1;\n"
    "    vout[b + 3] = o.fog; vout[b + 4] = o.t0; vout[b + 5] = o.t1;\n"
    "    vout[b + 6] = o.t2; vout[b + 7] = o.t3;\n"
    "}\n";

static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;

static ID3D11Buffer *make_buffer(UINT bytes, UINT bind, UINT stride, UINT misc,
                                 D3D11_USAGE usage, UINT cpu,
                                 const void *init)
{
    D3D11_BUFFER_DESC d;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Buffer *b = NULL;

    memset(&d, 0, sizeof d);
    d.ByteWidth = bytes;
    d.Usage = usage;
    d.BindFlags = bind;
    d.CPUAccessFlags = cpu;
    d.MiscFlags = misc;
    d.StructureByteStride = stride;
    memset(&sd, 0, sizeof sd);
    sd.pSysMem = init;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &d, init ? &sd : NULL, &b)))
        return NULL;
    return b;
}

/* Run the compute shader; fill out[VERTS][8][4] from the GPU. */
static int run_gpu(const char *hlsl, const float consts[][4],
                   const float in[][16][4], float out[][8][4])
{
    ID3DBlob *code = NULL, *err = NULL;
    ID3D11ComputeShader *cs = NULL;
    ID3D11Buffer *cb, *vin, *vout, *stage;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11UnorderedAccessView *uav = NULL;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr;

    hr = D3DCompile(hlsl, strlen(hlsl), "vsh", NULL, NULL, "main", "cs_5_0",
                    0, 0, &code, &err);
    if (FAILED(hr)) {
        printf("FAIL: the translation does not compile:\n%s\n",
               err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return 0;
    }
    ID3D11Device_CreateComputeShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                     ID3D10Blob_GetBufferSize(code), NULL, &cs);
    ID3D10Blob_Release(code);
    cb = make_buffer(NV2A_VSH_HLSL_CBUFFER_BYTES, D3D11_BIND_CONSTANT_BUFFER,
                     0, 0, D3D11_USAGE_DEFAULT, 0, consts);
    vin = make_buffer(VERTS * 16 * 16, D3D11_BIND_SHADER_RESOURCE, 16,
                      D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                      D3D11_USAGE_DEFAULT, 0, in);
    vout = make_buffer(VERTS * 8 * 16, D3D11_BIND_UNORDERED_ACCESS, 16,
                       D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                       D3D11_USAGE_DEFAULT, 0, NULL);
    stage = make_buffer(VERTS * 8 * 16, 0, 16,
                        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                        D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, NULL);
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.NumElements = VERTS * 16;
    memset(&ud, 0, sizeof ud);
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = VERTS * 8;
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)vin, &sd, &srv);
    ID3D11Device_CreateUnorderedAccessView(s_dev, (ID3D11Resource *)vout, &ud, &uav);

    ID3D11DeviceContext_CSSetShader(s_ctx, cs, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(s_ctx, 0, 1, &cb);
    ID3D11DeviceContext_CSSetShaderResources(s_ctx, 0, 1, &srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(s_ctx, 0, 1, &uav, NULL);
    ID3D11DeviceContext_Dispatch(s_ctx, VERTS / 64, 1, 1);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stage,
                                     (ID3D11Resource *)vout);
    hr = ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stage, 0,
                                 D3D11_MAP_READ, 0, &m);
    if (SUCCEEDED(hr)) {
        memcpy(out, m.pData, VERTS * 8 * 16);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stage, 0);
    }
    ID3D11UnorderedAccessView_Release(uav);
    ID3D11ShaderResourceView_Release(srv);
    ID3D11Buffer_Release(stage);
    ID3D11Buffer_Release(vout);
    ID3D11Buffer_Release(vin);
    ID3D11Buffer_Release(cb);
    ID3D11ComputeShader_Release(cs);
    return SUCCEEDED(hr);
}

static int close_enough(float cpu, float gpu)
{
    if (!(fabsf(cpu) < 1.0e6f))
        return 1;                   /* NaN, infinity, or beyond a float's care */
    return fabsf(cpu - gpu) <= 1.0e-3f * fabsf(cpu) + 2.0e-4f;
}

static float s_consts[NV2A_VSH_CONSTANTS][4];
static float s_in[VERTS][16][4], s_out[VERTS][8][4];
static char  s_hlsl[1 << 17];
static int   s_compared;

/* Run s_prog (len slots) over s_consts and s_in, here and on the GPU, and
 * compare. Returns 0 if they agree. */
static int check(const char *what, int p, uint32_t len)
{
    Nv2aVshHlslInfo info;
    uint32_t c, v, q;
    int n, k;

    for (c = 0; c < NV2A_VSH_CONSTANTS; c++)
        nv2a_vsh_set_constant(c, s_consts[c]);
    memcpy(s_hlsl, k_zero, sizeof k_zero - 1);
    n = nv2a_vsh_source((const uint32_t (*)[4])s_prog, 0, 0,
                        s_hlsl + sizeof k_zero - 1,
                        sizeof s_hlsl - sizeof k_zero - sizeof k_main, &info);
    if (n < 0) {
        printf("FAIL: %s %d: no translation\n", what, p);
        return 1;
    }
    n += (int)sizeof k_zero - 1;
    memcpy(s_hlsl + n, k_main, sizeof k_main);
    if (!run_gpu(s_hlsl, (const float (*)[4])s_consts,
                 (const float (*)[16][4])s_in, s_out)) {
        printf("  (%s %d, %u slots)\n", what, p, len);
        return 1;
    }
    for (v = 0; v < VERTS; v++) {
        Nv2aVshOutput o;
        const float *cpu[8];

        memset(&o, 0, sizeof o);
        nv2a_vsh_run_scalar((const float (*)[4])s_in[v], &o);
        cpu[0] = o.pos; cpu[1] = o.d0; cpu[2] = o.d1; cpu[3] = o.fog;
        cpu[4] = o.tex[0]; cpu[5] = o.tex[1]; cpu[6] = o.tex[2];
        cpu[7] = o.tex[3];
        if (o.written != info.written) {
            printf("FAIL: %s %d: written %X, translation says %X\n", what, p,
                   o.written, info.written);
            return 1;
        }
        for (q = 0; q < 8; q++)
            for (k = 0; k < 4; k++) {
                s_compared++;
                if (!close_enough(cpu[q][k], s_out[v][q][k])) {
                    printf("FAIL: %s %d (%u slots), vertex %u, output %u.%c:"
                           " interpreter %g, GPU %g\n", what, p, len, v, q,
                           "xyzw"[k], cpu[q][k], s_out[v][q][k]);
                    printf("--- source ---\n%.*s\n", n, s_hlsl);
                    return 1;
                }
            }
    }
    return 0;
}

/* The epilogue Xbox D3D ends every program with (see nv2a_vsh_test.c), over
 * what a title's vertices give it: w in front of the eye and behind it, near
 * and far, and the odd zero. */
static int epilogue(void)
{
    enum { T = 1, V = 2, C = 3 };
    uint32_t i0[4] = { 0 }, i1[4] = { 0 }, i2[4] = { 0 };
    uint32_t v;
    int k;

#define SRC(w, which, mux, reg, swz) do { \
        if ((which) == 0) { (w)[1] |= (swz); (w)[2] |= ((mux) << 26) | ((reg) << 28); } \
        else if ((which) == 1) { (w)[2] |= ((swz) << 17) | ((mux) << 11) | ((reg) << 13); } \
        else { (w)[2] |= ((swz) << 2) | ((reg) >> 2); \
               (w)[3] |= ((mux) << 28) | (((reg) & 3) << 30); } } while (0)
    i0[1] = 1u << 21;                               /* mov o[0], v0 */
    SRC(i0, 0, V, 0, 0x1B);
    i0[3] |= (0xFu << 12) | (1u << 11);
    i1[1] = (3u << 25) | (2u << 21) | (58u << 13);  /* mul .. ; rcc R1.x, R12.w */
    SRC(i1, 0, T, 12, 0x1B); SRC(i1, 1, C, 0, 0x1B); SRC(i1, 2, T, 12, 0xFF);
    i1[3] |= (7u << 20) | (8u << 16) | (0xEu << 12) | (1u << 11);
    i2[1] = (4u << 21) | (59u << 13);               /* mad o[0].xyz, R12, R1.x, c[59] */
    SRC(i2, 0, T, 12, 0x1B); SRC(i2, 1, T, 1, 0x00); SRC(i2, 2, C, 0, 0x1B);
    i2[3] |= (0xEu << 12) | (1u << 11) | 1u;
#undef SRC
    memcpy(s_prog[0], i0, sizeof i0);
    memcpy(s_prog[1], i1, sizeof i1);
    memcpy(s_prog[2], i2, sizeof i2);
    for (v = 0; v < 3; v++)
        nv2a_vsh_set_instruction(v, s_prog[v]);
    memset(s_consts, 0, sizeof s_consts);
    s_consts[58][0] = 320.0f; s_consts[58][1] = -240.0f; s_consts[58][2] = 1.0e6f;
    s_consts[59][0] = 320.0f; s_consts[59][1] = 240.0f;
    for (v = 0; v < VERTS; v++) {
        static const float ws[] = { 0.001f, 0.5f, 1.0f, 7.0f, 300.0f, 5.0e4f,
                                    -0.5f, -20.0f, 0.0f };

        memset(s_in[v], 0, sizeof s_in[v]);
        for (k = 0; k < 3; k++)
            s_in[v][0][k] = rnd_float() * 50.0f;
        s_in[v][0][3] = ws[v % (sizeof ws / sizeof ws[0])];
    }
    return check("epilogue", 0, 3);
}

int main(int argc, char **argv)
{
    int programs = argc > 1 ? atoi(argv[1]) : 300, p;
    D3D_FEATURE_LEVEL got;

    if (argc > 2)
        s_seed = (uint32_t)strtoul(argv[2], NULL, 0) | 1u;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
                                 D3D11_SDK_VERSION, &s_dev, &got, &s_ctx))) {
        puts("SKIP: no D3D11 device");
        return 0;
    }
    nv2a_vsh_set_cxt_write(0);
    if (epilogue())
        return 1;
    for (p = 0; p < programs; p++) {
        uint32_t len = 1u + rnd() % 24u, c, v;
        int k;

        random_program(len);
        for (c = 0; c < NV2A_VSH_CONSTANTS; c++)
            for (k = 0; k < 4; k++)
                s_consts[c][k] = rnd_float();
        for (v = 0; v < VERTS; v++)
            for (c = 0; c < 16; c++)
                for (k = 0; k < 4; k++)
                    s_in[v][c][k] = rnd_float();
        if (check("program", p, len))
            return 1;
    }
    printf("ok: the epilogue and %d programs, %d values compared\n", p,
           s_compared);
    return 0;
}
