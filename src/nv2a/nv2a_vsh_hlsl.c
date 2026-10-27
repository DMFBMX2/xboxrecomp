/*
 * An NV2A vertex program as HLSL. See nv2a_vsh_hlsl.h.
 *
 * The instruction layout is the table at the top of
 * src/kernel/nv2a_vsh_interp.c; the meaning of each operation is
 * nv2a_vsh_run_scalar's. A program has no branches or loops, so the whole
 * of it is straight-line code, one block a slot, and which outputs it writes
 * is known when it is translated.
 */
#include "nv2a_vsh_hlsl.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char   *buf;
    size_t  size, used;
    int     overflow;
} Out;

static void put(Out *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (o->overflow)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->buf + o->used, o->size - o->used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->size - o->used)
        o->overflow = 1;
    else
        o->used += (size_t)n;
}

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

/* One source operand, as an expression: the register, input or constant it
 * names, swizzled and negated. */
static void source(Out *o, const uint32_t *ins, int which, Nv2aVshHlslInfo *info)
{
    static const char xyzw[] = "xyzw";
    uint32_t neg, swz, mux, reg;
    char sw[8];
    int k;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    sw[0] = '.';
    for (k = 0; k < 4; k++)
        sw[1 + k] = xyzw[(swz >> (6 - 2 * k)) & 3u];
    sw[5] = 0;
    if (swz == 0x1Bu)
        sw[0] = 0;              /* x y z w: as it is */

    put(o, "%s(", neg ? "-" : "");
    if (mux == 1) {
        /* R0..R11; 12 is oPos; beyond that the interpreter reads R0. */
        if (reg == 12)
            put(o, "P");
        else
            put(o, "R%u", reg < 12 ? reg : 0u);
    } else if (mux == 2) {
        uint32_t n = field(ins, 1, 9, 4);

        info->inputs |= 1u << n;
        put(o, "v[%u]", n);
    } else {
        uint32_t ci = field(ins, 1, 13, 8);

        if (field(ins, 3, 1, 1)) {
            info->relative = 1;
            put(o, "vsh_c(%u + a0)", ci);
        } else {
            put(o, "vc[%u]", ci < NV2A_VSH_HLSL_CONSTANTS ? ci : 0u);
        }
    }
    put(o, ")%s", sw);
}

static const char *mask_letters(uint32_t mask, char out[5])
{
    int n = 0;

    if (mask & 8) out[n++] = 'x';
    if (mask & 4) out[n++] = 'y';
    if (mask & 2) out[n++] = 'z';
    if (mask & 1) out[n++] = 'w';
    out[n] = 0;
    return out;
}

static void mac_op(Out *o, uint32_t op)
{
    switch (op) {
    case 1: put(o, "sa"); break;                                  /* mov */
    case 2: put(o, "sa * sb"); break;
    case 3: put(o, "sa + sc"); break;
    case 4: put(o, "sa * sb + sc"); break;
    case 5: put(o, "(sa.x * sb.x + sa.y * sb.y + sa.z * sb.z).xxxx"); break;
    case 6: put(o, "(sa.x * sb.x + sa.y * sb.y + sa.z * sb.z + sb.w).xxxx");
            break;                                                /* dph */
    case 7: put(o, "(sa.x * sb.x + sa.y * sb.y + sa.z * sb.z"
                   " + sa.w * sb.w).xxxx"); break;
    case 8: put(o, "float4(1.0, sa.y * sb.y, sa.z, sb.w)"); break;  /* dst */
    case 9: put(o, "(sa < sb) ? sa : sb"); break;
    case 10: put(o, "(sa > sb) ? sa : sb"); break;
    case 11: put(o, "(sa < sb) ? (float4)1 : (float4)0"); break;
    case 12: put(o, "(sa >= sb) ? (float4)1 : (float4)0"); break;
    default: put(o, "sa"); break;       /* arl, and the two unused: A */
    }
}

static void ilu_op(Out *o, uint32_t op)
{
    switch (op) {
    case 1: put(o, "sc"); break;                                  /* mov */
    case 2: put(o, "(sc.x == 0.0 ? vsh_inf : 1.0 / sc.x).xxxx"); break;
    case 3: put(o, "vsh_rcc(sc.x).xxxx"); break;
    case 4: put(o, "vsh_rsq(sc.x).xxxx"); break;
    case 5: put(o, "float4(exp2(floor(sc.x)), sc.x - floor(sc.x),"
                   " exp2(sc.x), 1.0)"); break;
    case 6: put(o, "vsh_log(sc.x)"); break;
    case 7: put(o, "vsh_lit(sc)"); break;
    default: put(o, "sc"); break;
    }
}

/* Where a write to temp `t` goes, or nothing for a register that is not. */
static int temp_name(char *dst, uint32_t t)
{
    if (t == 12)
        strcpy(dst, "P");
    else if (t < 12)
        sprintf(dst, "R%u", t);
    else
        return 0;
    return 1;
}

int nv2a_vsh_source(const uint32_t program[][4], uint32_t start,
                    int cxt_write, char *buf, size_t size,
                    Nv2aVshHlslInfo *info)
{
    Out o = { buf, size, 0, 0 };
    Nv2aVshHlslInfo local;
    uint32_t s, last = NV2A_VSH_HLSL_SLOTS, k;

    if (!info)
        info = &local;
    memset(info, 0, sizeof *info);
    for (s = start; s < NV2A_VSH_HLSL_SLOTS; s++)
        if (field(program[s], 3, 0, 1)) {
            last = s;
            break;
        }
    if (last == NV2A_VSH_HLSL_SLOTS)
        return -1;

    put(&o,
        "#ifndef VSH_ZERO\n"
        "#define VSH_ZERO 0\n"
        "#endif\n"
        "#ifdef VSH_CONSTANTS_T1\n"
        "StructuredBuffer<float4> vc : register(t1);\n"
        "#else\n"
        "cbuffer VshConstants : register(b0) { float4 vc[%u]; };\n"
        "#endif\n"
        "struct VshOut { float4 pos, d0, d1, fog, t0, t1, t2, t3; };\n"
        "static const float vsh_inf = asfloat(0x7F800000);\n"
        "float4 vsh_c(int i)\n"
        "{ return ((uint)i >= %uu) ? vc[0] : vc[(uint)i %% %uu]; }\n"
        "float vsh_rcc(float x)\n"
        "{\n"
        "    float f = x == 0.0 ? ((asuint(x) >> 31) ? -vsh_inf : vsh_inf)\n"
        "                       : 1.0 / x;\n"
        "    if (abs(f) < 5.42101e-20) f = f < 0 ? -5.42101e-20 : 5.42101e-20;\n"
        "    if (abs(f) > 1.884467e19) f = f < 0 ? -1.884467e19 : 1.884467e19;\n"
        "    return f;\n"
        "}\n"
        "float vsh_rsq(float x)\n"
        "{ float a = abs(x); return a == 0.0 ? vsh_inf : 1.0 / sqrt(a); }\n"
        "float4 vsh_log(float x)\n"
        "{\n"
        "    float ax = abs(x);\n"
        "    if (ax == 0.0) return float4(-vsh_inf, 1.0, -vsh_inf, 1.0);\n"
        "    float e = floor(log2(ax));\n"
        "    return float4(e, ax / exp2(e), log2(ax), 1.0);\n"
        "}\n"
        "float4 vsh_lit(float4 c)\n"
        "{\n"
        "    float nl = max(c.x, 0.0), nh = max(c.y, 0.0);\n"
        "    float p = clamp(c.w, -127.9961, 127.9961);\n"
        "    return float4(1.0, nl, c.x > 0.0 ? (p == 0.0 ? 1.0 : pow(nh, p)) : 0.0,\n"
        "                  1.0);\n"
        "}\n"
        "VshOut vsh_run(float4 v[16])\n"
        "{\n"
        "    float4 R0 = VSH_ZERO, R1 = VSH_ZERO, R2 = VSH_ZERO, R3 = VSH_ZERO,\n"
        "           R4 = VSH_ZERO, R5 = VSH_ZERO, R6 = VSH_ZERO, R7 = VSH_ZERO,\n"
        "           R8 = VSH_ZERO, R9 = VSH_ZERO, R10 = VSH_ZERO, R11 = VSH_ZERO,\n"
        "           P = VSH_ZERO;\n"
        "    float4 O3 = float4(0, 0, 0, 1), O4 = float4(0, 0, 0, 1);\n"
        "    float4 O5 = 0, O9 = float4(0, 0, 0, 1), O10 = O9, O11 = O9,\n"
        "           O12 = O9;\n"
        "    int a0 = 0;\n",
        NV2A_VSH_HLSL_CONSTANTS, NV2A_VSH_HLSL_CONSTANTS,
        NV2A_VSH_HLSL_CONSTANTS);

    for (s = start; s <= last; s++) {
        const uint32_t *ins = program[s];
        uint32_t mac = field(ins, 1, 21, 4), ilu = field(ins, 1, 25, 3);
        uint32_t mac_mask = field(ins, 3, 24, 4), ilu_mask = field(ins, 3, 16, 4);
        uint32_t tdst = field(ins, 3, 20, 4);
        uint32_t omask = field(ins, 3, 12, 4), oaddr = field(ins, 3, 3, 8);
        int out_ilu = (int)field(ins, 3, 2, 1), out_reg = (int)field(ins, 3, 11, 1);
        uint32_t need = 0;
        char name[8], letters[5];

        /* Which sources the operations read, as the interpreter has it. */
        if (mac) {
            need |= 1;
            if (mac == 2 || (mac >= 4 && mac <= 12))
                need |= 2;
            if (mac == 3 || mac == 4)
                need |= 4;
        }
        if (ilu)
            need |= 4;

        put(&o, "    { // slot %u\n", s);
        put(&o, "      float4 sa = 0, sb = 0, sc = 0, mres = 0, ires = 0;\n");
        for (k = 0; k < 3; k++)
            if (need & (1u << k)) {
                put(&o, "      s%c = ", 'a' + (int)k);
                source(&o, ins, (int)k, info);
                put(&o, ";\n");
            }
        if (mac) {
            put(&o, "      mres = ");
            mac_op(&o, mac);
            put(&o, ";\n");
            if (mac == 13)
                put(&o, "      a0 = (int)floor(sa.x + 0.001);\n");
        }
        if (ilu) {
            put(&o, "      ires = ");
            ilu_op(&o, ilu);
            put(&o, ";\n");
        }
        if (mac && mac != 13 && mac_mask && temp_name(name, tdst))
            put(&o, "      %s.%s = mres.%s;\n", name,
                mask_letters(mac_mask, letters), letters);
        if (ilu && ilu_mask) {
            /* Beside a MAC op the ILU can only write R1. */
            if (temp_name(name, mac ? 1u : tdst))
                put(&o, "      %s.%s = ires.%s;\n", name,
                    mask_letters(ilu_mask, letters), letters);
        }
        if (omask) {
            const char *v = out_ilu ? "ires" : "mres";

            if (!out_reg) {                         /* to c[] */
                if (cxt_write && oaddr < NV2A_VSH_HLSL_CONSTANTS)
                    info->writes_constants = 1;
            } else if (oaddr == 0) {
                put(&o, "      P.%s = %s.%s;\n", mask_letters(omask, letters),
                    v, letters);
            } else if (oaddr < 13) {
                info->written |= 1u << oaddr;
                /* Only the outputs a consumer reads are kept. */
                if (oaddr == 3 || oaddr == 4 || oaddr == 5
                        || (oaddr >= 9 && oaddr <= 12))
                    put(&o, "      O%u.%s = %s.%s;\n", oaddr,
                        mask_letters(omask, letters), v, letters);
            }
        }
        put(&o, "    }\n");
    }
    info->slots = last - start + 1;

    /* The colours are clamped to 0..1 before they are interpolated. */
    put(&o,
        "    VshOut o;\n"
        "    o.pos = P; o.d0 = saturate(O3); o.d1 = saturate(O4);\n"
        "    o.fog = O5; o.t0 = O9; o.t1 = O10; o.t2 = O11; o.t3 = O12;\n"
        "    return o;\n"
        "}\n");
    return o.overflow ? -1 : (int)o.used;
}
