/*
 * The NV2A's pixel pipeline as an HLSL pixel shader. See nv2a_psh_hlsl.h.
 *
 * The shader is written out in the order the hardware works: the four
 * texture stages, then the combiners, the final combiner and the alpha
 * test. Names in it are the hardware's: pT0..pT3 are the interpolated
 * coordinates, t0..t3 what the stages produce, v0 and v1 the vertex colours,
 * r0 and r1 the combiners' two registers, c0 and c1 their constants.
 */
#include "nv2a_psh_hlsl.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char  *buf;
    size_t size, len;
    int    overflow;
} Out;

static void emit(Out *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (o->overflow)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->buf + o->len, o->size - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->size - o->len)
        o->overflow = 1;
    else
        o->len += (size_t)n;
}

/* ── Texture stages ────────────────────────────────────────── */

/* SHADER_STAGE_PROGRAM modes (xemu psh.h PS_TEXTUREMODES_*). */
enum {
    M_NONE = 0x00, M_PROJECT2D = 0x01, M_PROJECT3D = 0x02, M_CUBEMAP = 0x03,
    M_PASSTHRU = 0x04, M_CLIPPLANE = 0x05, M_BUMPENVMAP = 0x06,
    M_BUMPENVMAP_LUM = 0x07, M_BRDF = 0x08, M_DOT_ST = 0x09, M_DOT_ZW = 0x0A,
    M_DOT_RFLCT_DIFF = 0x0B, M_DOT_RFLCT_SPEC = 0x0C, M_DOT_STR_3D = 0x0D,
    M_DOT_STR_CUBE = 0x0E, M_DPNDNT_AR = 0x0F, M_DPNDNT_GB = 0x10,
    M_DOTPRODUCT = 0x11, M_DOT_RFLCT_SPEC_CONST = 0x12
};

static uint32_t stage_mode(const Nv2aPshKey *key, int st)
{
    return (key->rc.stage_program >> (st * 5)) & 0x1Fu;
}

/* Whether a stage's mode takes a dot product of its coordinates with the
 * colour of an earlier stage. */
static int mode_has_dot(uint32_t mode)
{
    return mode == M_DOT_ST || mode == M_DOT_RFLCT_DIFF
        || mode == M_DOT_RFLCT_SPEC || mode == M_DOT_STR_3D
        || mode == M_DOT_STR_CUBE || mode == M_DOTPRODUCT;
}

/* The earlier stage whose colour stage `st` reads: always 0 for stage 1,
 * and the title's choice among the ones before it for stages 2 and 3. */
static int input_stage(const Nv2aPshKey *key, int st)
{
    int in = 0;

    if (st == 2)
        in = (int)((key->other_stage_input >> 16) & 0xFu);
    else if (st == 3)
        in = (int)((key->other_stage_input >> 20) & 0xFu);
    return in < st ? in : 0;
}

/* The lookup of stage `st` at the float3 `coord`. A stage with nothing
 * bound reads white, as the software path's does, so that a missing
 * texture multiplies through. */
static void emit_lookup(Out *o, const Nv2aPshKey *key, int st,
                        const char *coord)
{
    switch (key->tex_kind[st]) {
    case NV2A_PSH_TEX_2D:
        emit(o, "    t%d = tex%d.Sample(samp%d, (%s).xy * TexScale[%d].xy);\n",
             st, st, st, coord, st);
        break;
    case NV2A_PSH_TEX_3D:
        emit(o, "    t%d = tex%d.Sample(samp%d, (%s).xyz * TexScale[%d].xyz);\n",
             st, st, st, coord, st);
        break;
    case NV2A_PSH_TEX_CUBE:
        emit(o, "    t%d = tex%d.Sample(samp%d, (%s).xyz);\n", st, st, st,
             coord);
        break;
    default:
        emit(o, "    t%d = float4(1.0, 1.0, 1.0, 1.0);\n", st);
        return;
    }
    if (key->alpha_kill[st])
        emit(o, "    if (t%d.a == 0.0) discard;\n", st);
}

/* How a dot-product stage reads a colour as a vector (PS_DOTMAPPING).
 * Only 0 has been seen in a title here; the others are after xemu's
 * dotmap_* functions. */
static void emit_dotmap(Out *o, const Nv2aPshKey *key, int st,
                        uint32_t *unsupported)
{
    uint32_t map = (key->dot_mapping >> ((st - 1) * 4)) & 0xFu;

    emit(o, "float3 dotmap%d(float3 c) {\n", st);
    switch (map) {
    case 1:                                 /* -1..1, D3D's rounding */
        emit(o, "    return (c * 255.0 - 128.0) / 127.0;\n");
        break;
    case 2:                                 /* -1..1, OpenGL's */
        emit(o, "    return (c * 510.0 - 255.0) / 255.0;\n");
        break;
    case 3:                                 /* the bytes as signed */
        emit(o, "    float3 b = c * 255.0;\n"
                "    return (b - 256.0 * step(128.0, b)) / 127.0;\n");
        break;
    default:
        /* ponytail: 4..7 read two 16-bit channels (HILO), which an 8-bit
         * upload does not carry. Read as 0..1 and named. */
        if (map != 0)
            *unsupported |= 1u << (st + 4);
        emit(o, "    return c;\n");
        break;
    }
    emit(o, "}\n");
}

static void emit_stage(Out *o, const Nv2aPshKey *key, int st,
                       uint32_t *unsupported)
{
    uint32_t mode = stage_mode(key, st);
    int in = input_stage(key, st), j;
    char coord[160];

    if (st >= 1 && mode_has_dot(mode))
        emit(o, "    dot%d = dot(pT%d.xyz, dotmap%d(t%d.rgb));\n", st, st, st,
             in);

    switch (mode) {
    case M_NONE:
        emit(o, "    t%d = float4(0.0, 0.0, 0.0, 1.0);\n", st);
        break;
    case M_PROJECT2D:
    case M_PROJECT3D:
        /* The coordinates over their fourth; which of them are used is the
         * texture's business. */
        snprintf(coord, sizeof coord,
                 "pT%d.xyz / (pT%d.w != 0.0 ? pT%d.w : 1.0)", st, st, st);
        emit_lookup(o, key, st, coord);
        break;
    case M_CUBEMAP:
        snprintf(coord, sizeof coord, "pT%d.xyz", st);
        emit_lookup(o, key, st, coord);
        break;
    case M_PASSTHRU:
        emit(o, "    t%d = saturate(pT%d);\n", st, st);
        break;
    case M_CLIPPLANE:
        for (j = 0; j < 4; j++) {
            int ge = (key->clip_plane_mode >> (st * 4 + j)) & 1;
            emit(o, "    if (pT%d.%c %s 0.0) discard;\n", st, "xyzw"[j],
                 ge ? ">=" : "<");
        }
        break;
    case M_DOTPRODUCT:
        break;                              /* the dot product is the point */
    case M_DOT_ST:
        snprintf(coord, sizeof coord, "float3(dot%d, dot%d, 0.0)", st - 1, st);
        emit_lookup(o, key, st, coord);
        break;
    case M_DOT_RFLCT_DIFF:
        /* The normal, moved by the three rows of the title's matrix; the
         * third row is the next stage's. */
        if (st != 2)
            goto unknown;
        snprintf(coord, sizeof coord,
                 "float3(dot1, dot2, dot(pT3.xyz, dotmap3(t%d.rgb)))",
                 input_stage(key, 3));
        emit_lookup(o, key, st, coord);
        break;
    case M_DOT_RFLCT_SPEC:
        /* The same normal, and the eye vector carried in the fourth
         * coordinate of the three rows, reflected about it. */
        if (st != 3)
            goto unknown;
        emit(o, "    {\n"
                "        float3 n = float3(dot1, dot2, dot3);\n"
                "        float3 e = float3(pT1.w, pT2.w, pT3.w);\n"
                "        float3 rv = 2.0 * n * dot(n, e)"
                " / max(dot(n, n), 1.0e-20) - e;\n    ");
        emit_lookup(o, key, st, "rv");
        emit(o, "    }\n");
        break;
    case M_DOT_STR_3D:
    case M_DOT_STR_CUBE:
        if (st != 3)
            goto unknown;
        emit_lookup(o, key, st, "float3(dot1, dot2, dot3)");
        break;
    case M_DPNDNT_AR:
        if (st < 1)
            goto unknown;
        snprintf(coord, sizeof coord, "float3(t%d.a, t%d.r, 0.0)", in, in);
        emit_lookup(o, key, st, coord);
        break;
    case M_DPNDNT_GB:
        if (st < 1)
            goto unknown;
        snprintf(coord, sizeof coord, "float3(t%d.g, t%d.b, 0.0)", in, in);
        emit_lookup(o, key, st, coord);
        break;
    default:
    unknown:
        /* ponytail: the bump environment maps (6, 7), BRDF (8), depth
         * replace (10) and the reflection with a constant eye (18) read
         * zero. They need state the executor does not record yet. */
        *unsupported |= 1u << st;
        break;
    }
}

/* ── Combiners ─────────────────────────────────────────────── */

/* A combiner's registers by number; see nv2a_combiner.c. */
static const char *const REG[16] = {
    "zero", "c0", "c1", "fog", "v0", "v1", "zero", "zero",
    "t0", "t1", "t2", "t3", "r0", "r1", "vsum", "ef"
};

/* An input byte applied to `x`: its mapping (PS_INPUTMAPPING). */
static void map_in(char *out, size_t n, const char *x, uint32_t byte)
{
    switch ((byte >> 5) & 7u) {
    case 0:  snprintf(out, n, "max(%s, 0.0)", x);                 break;
    case 1:  snprintf(out, n, "(1.0 - saturate(%s))", x);         break;
    case 2:  snprintf(out, n, "(2.0 * max(%s, 0.0) - 1.0)", x);   break;
    case 3:  snprintf(out, n, "(1.0 - 2.0 * max(%s, 0.0))", x);   break;
    case 4:  snprintf(out, n, "(max(%s, 0.0) - 0.5)", x);         break;
    case 5:  snprintf(out, n, "(0.5 - max(%s, 0.0))", x);         break;
    case 6:  snprintf(out, n, "(%s)", x);                         break;
    default: snprintf(out, n, "(-(%s))", x);                      break;
    }
}

/* In the colour half the alpha bit repeats .a; in the alpha half it picks
 * .a over .b. */
static const char *rgb_in(char *out, size_t n, uint32_t byte)
{
    char x[32];

    byte &= 0xFFu;
    snprintf(x, sizeof x, "%s.%s", REG[byte & 0xF],
             (byte & 0x10) ? "aaa" : "rgb");
    map_in(out, n, x, byte);
    return out;
}

static const char *alpha_in(char *out, size_t n, uint32_t byte)
{
    char x[32];

    byte &= 0xFFu;
    snprintf(x, sizeof x, "%s.%s", REG[byte & 0xF], (byte & 0x10) ? "a" : "b");
    map_in(out, n, x, byte);
    return out;
}

/* A combiner's output scale and bias (PS_COMBINEROUTPUT), then its clamp. */
static const char *map_out(char *out, size_t n, const char *x, uint32_t fl)
{
    switch (fl & 0x38u) {
    case 0x08: snprintf(out, n, "clamp(%s - 0.5, -1.0, 1.0)", x);         break;
    case 0x10: snprintf(out, n, "clamp(%s * 2.0, -1.0, 1.0)", x);         break;
    case 0x18: snprintf(out, n, "clamp((%s - 0.5) * 2.0, -1.0, 1.0)", x); break;
    case 0x20: snprintf(out, n, "clamp(%s * 4.0, -1.0, 1.0)", x);         break;
    case 0x30: snprintf(out, n, "clamp(%s * 0.5, -1.0, 1.0)", x);         break;
    default:   snprintf(out, n, "clamp(%s, -1.0, 1.0)", x);               break;
    }
    return out;
}

static int writable(uint32_t reg)
{
    return strcmp(REG[reg & 0xF], "zero") != 0;
}

static void emit_combiner(Out *o, const Nv2aCombiner *rc, uint32_t s)
{
    uint32_t flags = rc->control >> 8;
    uint32_t icw = rc->color_icw[s], ocw = rc->color_ocw[s];
    uint32_t aicw = rc->alpha_icw[s], aocw = rc->alpha_ocw[s];
    uint32_t fl = ocw >> 12, afl = aocw >> 12;
    char a[96], b[96], c[96], d[96];

    /* One c0 and c1 for every stage, or one each. Everything is read
     * before anything is written. */
    emit(o, "    {\n        c0 = F0[%u]; c1 = F1[%u];\n",
         (flags & 0x010) ? s : 0u, (flags & 0x100) ? s : 0u);
    emit(o, "        bool mux = %s;\n",
         (flags & 1) ? "r0.a >= 0.5" : "(((int)(r0.a * 255.0)) & 1) != 0");
    emit(o, "        float3 A = %s, B = %s, C = %s, D = %s;\n",
         rgb_in(a, sizeof a, icw >> 24), rgb_in(b, sizeof b, icw >> 16),
         rgb_in(c, sizeof c, icw >> 8), rgb_in(d, sizeof d, icw));
    emit(o, "        float aA = %s, aB = %s, aC = %s, aD = %s;\n",
         alpha_in(a, sizeof a, aicw >> 24), alpha_in(b, sizeof b, aicw >> 16),
         alpha_in(c, sizeof c, aicw >> 8), alpha_in(d, sizeof d, aicw));
    emit(o, "        float3 pab = %s, pcd = %s;\n",
         (fl & 2) ? "(float3)dot(A, B)" : "A * B",
         (fl & 1) ? "(float3)dot(C, D)" : "C * D");
    emit(o, "        float3 psum = %s;\n",
         (fl & 4) ? "(mux ? pcd : pab)" : "pab + pcd");
    emit(o, "        float qab = aA * aB, qcd = aC * aD;\n");
    emit(o, "        float qsum = %s;\n",
         (afl & 4) ? "(mux ? qcd : qab)" : "qab + qcd");
    emit(o, "        float3 oab = %s;\n", map_out(a, sizeof a, "pab", fl));
    emit(o, "        float3 ocd = %s;\n", map_out(a, sizeof a, "pcd", fl));
    emit(o, "        float3 osum = %s;\n", map_out(a, sizeof a, "psum", fl));

    /* The colour half's three places; blue-to-alpha fills in .a too. */
    if (writable(ocw >> 4)) {
        emit(o, "        %s.rgb = oab;\n", REG[(ocw >> 4) & 0xF]);
        if (fl & 0x80)
            emit(o, "        %s.a = oab.b;\n", REG[(ocw >> 4) & 0xF]);
    }
    if (writable(ocw)) {
        emit(o, "        %s.rgb = ocd;\n", REG[ocw & 0xF]);
        if (fl & 0x40)
            emit(o, "        %s.a = ocd.b;\n", REG[ocw & 0xF]);
    }
    if (writable(ocw >> 8))
        emit(o, "        %s.rgb = osum;\n", REG[(ocw >> 8) & 0xF]);
    if (writable(aocw >> 4))
        emit(o, "        %s.a = %s;\n", REG[(aocw >> 4) & 0xF],
             map_out(a, sizeof a, "qab", afl));
    if (writable(aocw))
        emit(o, "        %s.a = %s;\n", REG[aocw & 0xF],
             map_out(a, sizeof a, "qcd", afl));
    if (writable(aocw >> 8))
        emit(o, "        %s.a = %s;\n", REG[(aocw >> 8) & 0xF],
             map_out(a, sizeof a, "qsum", afl));
    emit(o, "    }\n");
}

static void emit_final(Out *o, const Nv2aCombiner *rc)
{
    uint32_t f0 = rc->final0, f1 = rc->final1, fflags = f1 & 0xFFu;
    char a[96], b[96], c[96], d[96];

    if (!f0 && !f1) {
        emit(o, "    float4 o = r0;\n");
        return;
    }
    /* v1 + r0, either of them complemented first, clamped if asked; then
     * E x F; then D + mix(C, B, A), with alpha from G. */
    emit(o, "    c0 = FC0; c1 = FC1;\n");
    emit(o, "    vsum = float4(%s(%s + %s), 0.0);\n",
         (fflags & 0x80) ? "saturate" : "",
         (fflags & 0x40) ? "(1.0 - v1.rgb)" : "v1.rgb",
         (fflags & 0x20) ? "(1.0 - r0.rgb)" : "r0.rgb");
    emit(o, "    ef = float4(%s * %s, 0.0);\n",
         rgb_in(a, sizeof a, f1 >> 24), rgb_in(b, sizeof b, f1 >> 16));
    emit(o, "    float3 fA = %s, fB = %s, fC = %s, fD = %s;\n",
         rgb_in(a, sizeof a, f0 >> 24), rgb_in(b, sizeof b, f0 >> 16),
         rgb_in(c, sizeof c, f0 >> 8), rgb_in(d, sizeof d, f0));
    emit(o, "    float4 o = float4(fD + fC * (1.0 - fA) + fB * fA, %s);\n",
         alpha_in(a, sizeof a, f1 >> 8));
}

static void emit_alpha_test(Out *o, uint32_t func)
{
    static const char *const cmp[8] = {
        NULL, "<", "==", "<=", ">", "!=", ">=", NULL
    };

    if (!func || (func & 7u) == 7u)
        return;                             /* off, or always */
    if ((func & 7u) == 0u) {
        emit(o, "    discard;\n");          /* never */
        return;
    }
    emit(o, "    if (!((int)(o.a * 255.0 + 0.5) %s (int)Misc.x)) discard;\n",
         cmp[func & 7u]);
}

/* ── The shader ────────────────────────────────────────────── */

int nv2a_psh_source(const Nv2aPshKey *key, char *buf, size_t size,
                    uint32_t *unsupported)
{
    static const char *const decl[4] = {
        NULL, "Texture2D", "Texture3D", "TextureCube"
    };
    uint32_t stages = key->rc.control & 0xFFu, s, none = 0;
    Out o;
    int st;

    if (!unsupported)
        unsupported = &none;
    *unsupported = 0;
    o.buf = buf;
    o.size = size;
    o.len = 0;
    o.overflow = 0;
    if (stages > 8)
        stages = 8;

    emit(&o,
         "cbuffer PshConstants : register(b0) {\n"
         "    float4 F0[8];\n"
         "    float4 F1[8];\n"
         "    float4 FC0;\n"
         "    float4 FC1;\n"
         "    float4 FogColor;\n"
         "    float4 TexScale[4];\n"
         "    float4 Misc;\n"
         "};\n");
    for (st = 0; st < 4; st++) {
        if (key->tex_kind[st] != NV2A_PSH_TEX_NONE
                && key->tex_kind[st] <= NV2A_PSH_TEX_CUBE)
            emit(&o, "%s tex%d : register(t%d);\n"
                     "SamplerState samp%d : register(s%d);\n",
                 decl[key->tex_kind[st]], st, st, st, st);
    }
    for (st = 1; st < 4; st++)
        emit_dotmap(&o, key, st, unsupported);
    emit(&o,
         "struct PS_IN {\n"
         "    float4 pos : SV_POSITION;\n"
         "    float4 d0  : COLOR0;\n"
         "    float4 d1  : COLOR1;\n"
         "    float4 t0  : TEXCOORD0;\n"
         "    float4 t1  : TEXCOORD1;\n"
         "    float4 t2  : TEXCOORD2;\n"
         "    float4 t3  : TEXCOORD3;\n"
         "    float  fog : TEXCOORD4;\n"
         "};\n"
         "float4 main(PS_IN i) : SV_Target {\n"
         "    float4 pT0 = i.t0, pT1 = i.t1, pT2 = i.t2, pT3 = i.t3;\n"
         "    float4 t0 = 0.0, t1 = 0.0, t2 = 0.0, t3 = 0.0;\n"
         "    float dot0 = 0.0, dot1 = 0.0, dot2 = 0.0, dot3 = 0.0;\n");
    for (st = 0; st < 4; st++)
        emit_stage(&o, key, st, unsupported);

    /* r0's alpha starts as texture 0's, or 1 with stage 0 off. */
    emit(&o,
         "    float4 zero = 0.0, c0 = 0.0, c1 = 0.0, r1 = 0.0;\n"
         "    float4 vsum = 0.0, ef = 0.0;\n"
         "    float4 v0 = i.d0, v1 = i.d1;\n"
         "    float4 fog = float4(FogColor.rgb, i.fog);\n"
         "    float4 r0 = float4(0.0, 0.0, 0.0, %s);\n",
         stage_mode(key, 0) ? "t0.a" : "1.0");
    for (s = 0; s < stages; s++)
        emit_combiner(&o, &key->rc, s);
    emit_final(&o, &key->rc);
    emit(&o, "    o = saturate(o);\n");
    emit_alpha_test(&o, key->alpha_func);
    emit(&o, "    return o;\n}\n");

    return o.overflow ? -1 : (int)o.len;
}
