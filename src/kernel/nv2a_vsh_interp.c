/**
 * NV2A vertex program interpreter. See nv2a_vsh_interp.h for why it exists.
 *
 * Encoding. An instruction is four dwords; dword 0 is unused. Fields, as
 * (dword, lowest bit, width):
 *
 *   ILU op        1 25 3      MAC op        1 21 4
 *   const index   1 13 8      input index   1  9 4   (shared by A, B, C)
 *   A negate      1  8 1      A swizzle     1  0 8   (x y z w, 2 bits each,
 *   A temp        2 28 4      A mux         2 26 2    x highest)
 *   B negate      2 25 1      B swizzle     2 17 8
 *   B temp        2 13 4      B mux         2 11 2
 *   C negate      2 10 1      C swizzle     2  2 8
 *   C temp        2 0 2 (high) : 3 30 2 (low)          C mux 3 28 2
 *   MAC temp mask 3 24 4      temp dest     3 20 4
 *   ILU temp mask 3 16 4      out mask      3 12 4
 *   out to o[]    3 11 1      out address   3  3 8   (0: to c[])
 *   out from ILU  3  2 1      a0.x relative 3  1 1   final 3 0 1
 *
 * Masks are x=8 y=4 z=2 w=1. A source mux of 1 reads a temp, 2 an input,
 * 3 a constant. Temp 12 is oPos. When an instruction carries both a MAC and
 * an ILU op, the ILU's temp write goes to R1 whatever the temp field says,
 * and both units read their sources before either writes.
 *
 * The layout is checked against real programs rather than taken on trust:
 * RECOMP_VSH_DUMP disassembles each program the title uploads, and an Xbox
 * D3D program is recognisable -- DP4s against the matrix constants, ending in
 * the runtime's screen-space epilogue (RCC of R12.w, then a MAD into oPos).
 */
#include "nv2a_vsh_interp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Four floats at a time where the processor has them: see "execution, four
 * at a time" below. Every x86-64 has SSE2. */
#if defined(__SSE2__) || defined(_M_X64) \
        || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define VSH_SSE2 1
#endif

static uint32_t s_program[NV2A_VSH_SLOTS][4];
static float    s_const[NV2A_VSH_CONSTANTS][4];
static uint32_t s_load_slot, s_load_word, s_start_slot;
static uint32_t s_const_load, s_const_word;
static uint32_t s_cxt_write;

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

/* ---- decode -------------------------------------------------------------
 *
 * An instruction is four words of bit fields, and running one meant pulling
 * some thirty of them apart -- for every instruction of every vertex. A title
 * with a 3D scene runs its programs a couple of million times a second on one
 * thread, and that thread spent most of each second in field(): Dave Mirra
 * Freestyle BMX 2 dropped to 54 frames a second in a level, stuttering.
 *
 * So each slot is taken apart when it is written, and the run reads the
 * result. Sources an operation does not use are not read at all.
 */
typedef struct {
    uint8_t neg, mux, reg;      /* mux: 1 temp, 2 input, else constant */
    uint8_t sw[4];              /* source component for x, y, z, w */
    uint8_t pick;               /* 0: x,y,z,w as they are; 1: one component
                                 * four times; 2: anything else */
    uint8_t temp;               /* for mux 1, the register read: 12 is oPos,
                                 * and one there is not reads R0 */
} VshSrc;

typedef struct {
    VshSrc  src[3];
    uint8_t mac, ilu;
    uint8_t mac_mask, ilu_mask, tdst;
    uint8_t omask, oaddr;
    uint8_t out_ilu;            /* the output takes the ILU's result */
    uint8_t out_reg;            /* ...and goes to o[], not c[] */
    uint8_t input;              /* v[] index, for mux 2 */
    uint8_t ci;                 /* c[] index, for a constant */
    uint8_t ci_a0;              /* ...plus A0 */
    uint8_t need;               /* bit n: source n is read */
    uint8_t last;
} VshIns;

static VshIns s_decoded[NV2A_VSH_SLOTS];

static void decode_src(VshSrc *d, const uint32_t *ins, int which)
{
    uint32_t swz;

    if (which == 0) {
        d->neg = (uint8_t)field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        d->mux = (uint8_t)field(ins, 2, 26, 2);
        d->reg = (uint8_t)field(ins, 2, 28, 4);
    } else if (which == 1) {
        d->neg = (uint8_t)field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        d->mux = (uint8_t)field(ins, 2, 11, 2);
        d->reg = (uint8_t)field(ins, 2, 13, 4);
    } else {
        d->neg = (uint8_t)field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        d->mux = (uint8_t)field(ins, 3, 28, 2);
        d->reg = (uint8_t)((field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2));
    }
    d->sw[0] = (uint8_t)((swz >> 6) & 3);
    d->sw[1] = (uint8_t)((swz >> 4) & 3);
    d->sw[2] = (uint8_t)((swz >> 2) & 3);
    d->sw[3] = (uint8_t)(swz & 3);
    d->pick = swz == 0x1Bu ? 0
            : (d->sw[0] == d->sw[1] && d->sw[1] == d->sw[2]
               && d->sw[2] == d->sw[3]) ? 1 : 2;
    d->temp = d->reg <= 12 ? d->reg : 0;
}

static void decode_slot(uint32_t slot)
{
    const uint32_t *ins = s_program[slot];
    VshIns *d = &s_decoded[slot];

    decode_src(&d->src[0], ins, 0);
    decode_src(&d->src[1], ins, 1);
    decode_src(&d->src[2], ins, 2);
    d->mac      = (uint8_t)field(ins, 1, 21, 4);
    d->ilu      = (uint8_t)field(ins, 1, 25, 3);
    d->mac_mask = (uint8_t)field(ins, 3, 24, 4);
    d->ilu_mask = (uint8_t)field(ins, 3, 16, 4);
    d->tdst     = (uint8_t)field(ins, 3, 20, 4);
    d->omask    = (uint8_t)field(ins, 3, 12, 4);
    d->oaddr    = (uint8_t)field(ins, 3, 3, 8);
    d->out_ilu  = (uint8_t)field(ins, 3, 2, 1);
    d->out_reg  = (uint8_t)field(ins, 3, 11, 1);
    d->input    = (uint8_t)field(ins, 1, 9, 4);
    d->ci       = (uint8_t)field(ins, 1, 13, 8);
    d->ci_a0    = (uint8_t)field(ins, 3, 1, 1);
    d->last     = (uint8_t)field(ins, 3, 0, 1);
    /* Which sources the operations read: every MAC op reads A, the two-
     * and three-operand ones B, add and mad C; every ILU op reads C. */
    d->need = 0;
    if (d->mac) {
        d->need |= 1;
        if (d->mac == 2 || (d->mac >= 4 && d->mac <= 12))
            d->need |= 2;
        if (d->mac == 3 || d->mac == 4)
            d->need |= 4;
    }
    if (d->ilu)
        d->need |= 4;
}

/* ---- uploads ------------------------------------------------------------ */

void nv2a_vsh_set_load_slot(uint32_t slot)
{
    s_load_slot = slot;
    s_load_word = 0;
}

void nv2a_vsh_program_word(uint32_t word)
{
    if (s_load_slot < NV2A_VSH_SLOTS) {
        s_program[s_load_slot][s_load_word] = word;
        decode_slot(s_load_slot);
    }
    if (++s_load_word == 4) {
        s_load_word = 0;
        s_load_slot++;
    }
}

void nv2a_vsh_set_start_slot(uint32_t slot) { s_start_slot = slot; }
void nv2a_vsh_set_cxt_write(uint32_t enable) { s_cxt_write = enable; }

void nv2a_vsh_set_constant_load(uint32_t index)
{
    s_const_load = index;
    s_const_word = 0;
}

void nv2a_vsh_constant_word(uint32_t word)
{
    if (s_const_load < NV2A_VSH_CONSTANTS)
        memcpy(&s_const[s_const_load][s_const_word], &word, 4);
    if (++s_const_word == 4) {
        s_const_word = 0;
        s_const_load++;
    }
}

void nv2a_vsh_set_constant(uint32_t index, const float v[4])
{
    if (index < NV2A_VSH_CONSTANTS)
        memcpy(s_const[index], v, sizeof s_const[index]);
}

void nv2a_vsh_set_instruction(uint32_t slot, const uint32_t words[4])
{
    if (slot < NV2A_VSH_SLOTS) {
        memcpy(s_program[slot], words, sizeof s_program[slot]);
        decode_slot(slot);
    }
}

/* ---- disassembly (RECOMP_VSH_DUMP) --------------------------------------- */

static const char *const k_mac[16] = {
    "nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4",
    "dst", "min", "max", "slt", "sge", "arl", "mac14", "mac15"
};
static const char *const k_ilu[8] = {
    "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit"
};

static void dis_src(char *b, size_t n, const uint32_t *ins, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;

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
    if (mux == 1)
        snprintf(b, n, "%sR%u", neg ? "-" : "", reg);
    else if (mux == 2)
        snprintf(b, n, "%sv%u", neg ? "-" : "", field(ins, 1, 9, 4));
    else
        snprintf(b, n, "%sc[%s%u]", neg ? "-" : "",
                 field(ins, 3, 1, 1) ? "a0.x+" : "", field(ins, 1, 13, 8));
    {
        size_t l = strlen(b);
        if (l + 6 < n)
            snprintf(b + l, n - l, ".%c%c%c%c", sw[(swz >> 6) & 3],
                     sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
    }
}

static void dump_program(uint32_t start)
{
    uint32_t s;
    fprintf(stderr, "  [VSH] program at slot %u:\n", start);
    for (s = start; s < NV2A_VSH_SLOTS; s++) {
        const uint32_t *ins = s_program[s];
        char a[40], bb[40], c[40];
        dis_src(a, sizeof a, ins, 0);
        dis_src(bb, sizeof bb, ins, 1);
        dis_src(c, sizeof c, ins, 2);
        fprintf(stderr, "  [VSH] %3u %-4s %-4s A=%s B=%s C=%s  R%u mac%X ilu%X"
                " out%s%u m%X%s%s\n", s,
                k_mac[field(ins, 1, 21, 4)], k_ilu[field(ins, 1, 25, 3)],
                a, bb, c, field(ins, 3, 20, 4), field(ins, 3, 24, 4),
                field(ins, 3, 16, 4), field(ins, 3, 11, 1) ? "o" : "c",
                field(ins, 3, 3, 8), field(ins, 3, 12, 4),
                field(ins, 3, 2, 1) ? " ilu" : " mac",
                field(ins, 3, 0, 1) ? " FINAL" : "");
        if (field(ins, 3, 0, 1))
            break;
    }
    fflush(stderr);
}

const float *nv2a_vsh_constant(uint32_t index)
{
    return s_const[index < NV2A_VSH_CONSTANTS ? index : 0];
}

void nv2a_vsh_constant_component(uint32_t index, uint32_t comp, uint32_t word)
{
    if (index < NV2A_VSH_CONSTANTS && comp < 4)
        memcpy(&s_const[index][comp], &word, 4);
}

/* ---- execution ----------------------------------------------------------- */

typedef struct { float v[4]; } vec4;

static vec4 read_src(const VshIns *d, int which,
                     const float in[NV2A_VSH_INPUTS][4],
                     const vec4 *temp, const vec4 *opos, int a0)
{
    const VshSrc *src = &d->src[which];
    const float *s;
    vec4 r;

    if (src->mux == 1)
        s = (src->reg == 12) ? opos->v
          : (src->reg < 12 ? temp[src->reg].v : temp[0].v);
    else if (src->mux == 2)
        s = in[d->input];
    else {
        int ci = (int)d->ci;
        if (d->ci_a0)
            ci += a0;
        if (ci < 0 || ci >= NV2A_VSH_CONSTANTS)
            ci = 0;
        s = s_const[ci];
    }
    r.v[0] = s[src->sw[0]];
    r.v[1] = s[src->sw[1]];
    r.v[2] = s[src->sw[2]];
    r.v[3] = s[src->sw[3]];
    if (src->neg) {
        r.v[0] = -r.v[0]; r.v[1] = -r.v[1];
        r.v[2] = -r.v[2]; r.v[3] = -r.v[3];
    }
    return r;
}

static void write_masked(float *dst, const vec4 *src, uint32_t mask)
{
    if (mask & 8) dst[0] = src->v[0];
    if (mask & 4) dst[1] = src->v[1];
    if (mask & 2) dst[2] = src->v[2];
    if (mask & 1) dst[3] = src->v[3];
}

static vec4 splat(float f)
{
    vec4 r;
    r.v[0] = r.v[1] = r.v[2] = r.v[3] = f;
    return r;
}

static vec4 run_mac(uint32_t op, vec4 a, vec4 b, vec4 c, int *a0)
{
    vec4 r = a;
    int i;

    switch (op) {
    case 1: break;                                            /* mov */
    case 2: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i]; break;
    case 3: for (i = 0; i < 4; i++) r.v[i] = a.v[i] + c.v[i]; break;
    case 4: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i] + c.v[i]; break;
    case 5: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]); break;
    case 6: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + b.v[3]); break;                        /* dph */
    case 7: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + a.v[3]*b.v[3]); break;
    case 8: r.v[0] = 1.0f; r.v[1] = a.v[1] * b.v[1];            /* dst */
            r.v[2] = a.v[2]; r.v[3] = b.v[3]; break;
    case 9: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; break;
    /* max: of two that are equal, the second, as min has it -- which only
     * tells between a zero and a minus zero, and is what the processor's own
     * max does, so that a compiler using it changes nothing. */
    case 10: for (i = 0; i < 4; i++) r.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; break;
    case 11: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? 1.0f : 0.0f; break;
    case 12: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? 1.0f : 0.0f; break;
    case 13: *a0 = (int)floorf(a.v[0] + 0.001f); break;         /* arl */
    default: break;
    }
    return r;
}

static vec4 run_ilu(uint32_t op, vec4 c)
{
    float x = c.v[0];
    vec4 r = c;

    switch (op) {
    case 1: break;                                            /* mov */
    case 2: r = splat(x == 0.0f ? INFINITY : 1.0f / x); break;  /* rcp */
    case 3: {                                                  /* rcc */
        float f = 1.0f / x;
        if (fabsf(f) < 5.42101e-20f) f = f < 0 ? -5.42101e-20f : 5.42101e-20f;
        if (fabsf(f) > 1.884467e19f) f = f < 0 ? -1.884467e19f : 1.884467e19f;
        r = splat(f);
        break;
    }
    case 4: r = splat(1.0f / sqrtf(fabsf(x))); break;           /* rsq */
    case 5: {                                                  /* exp */
        float fl = floorf(x);
        r.v[0] = exp2f(fl); r.v[1] = x - fl; r.v[2] = exp2f(x); r.v[3] = 1.0f;
        break;
    }
    case 6: {                                                  /* log */
        float ax = fabsf(x);
        if (ax == 0.0f) {
            r.v[0] = r.v[2] = -INFINITY; r.v[1] = 1.0f;
        } else {
            float e = floorf(log2f(ax));
            r.v[0] = e; r.v[1] = ax / exp2f(e); r.v[2] = log2f(ax);
        }
        r.v[3] = 1.0f;
        break;
    }
    case 7: {                                                  /* lit */
        float nl = c.v[0] > 0.0f ? c.v[0] : 0.0f;
        float nh = c.v[1] > 0.0f ? c.v[1] : 0.0f;
        float p = c.v[3] < -127.9961f ? -127.9961f
                : (c.v[3] > 127.9961f ? 127.9961f : c.v[3]);
        r.v[0] = 1.0f; r.v[1] = nl;
        r.v[2] = c.v[0] > 0.0f ? powf(nh, p) : 0.0f; r.v[3] = 1.0f;
        break;
    }
    default: break;
    }
    return r;
}

/* RECOMP_VSH_DUMP: disassemble each program the first time it is run. */
static void dump_once(void)
{
    static int dump = -1;
    static uint32_t dumped[16];
    static int ndumped;

    if (dump < 0)
        dump = getenv("RECOMP_VSH_DUMP") != NULL;
    if (dump && ndumped < 16) {
        /* By content: titles reload different programs into the same slot. */
        uint32_t h = 2166136261u, t;
        int i, seen = 0;
        for (t = s_start_slot; t < NV2A_VSH_SLOTS; t++) {
            for (i = 0; i < 4; i++)
                h = (h ^ s_program[t][i]) * 16777619u;
            if (field(s_program[t], 3, 0, 1))
                break;
        }
        for (i = 0; i < ndumped; i++)
            if (dumped[i] == h) seen = 1;
        if (!seen) {
            dumped[ndumped++] = h;
            dump_program(s_start_slot);
        }
    }
}

/* One float at a time: what the program means, written out plainly. It runs
 * where there is no SSE2, and the test holds the other one to it. */
int nv2a_vsh_run_scalar(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out)
{
    vec4 temp[12], opos;
    float outregs[13][4];
    int a0 = 0;
    uint32_t s, written = 0;

    dump_once();
    memset(temp, 0, sizeof temp);
    memset(&opos, 0, sizeof opos);
    memset(outregs, 0, sizeof outregs);
    outregs[3][3] = outregs[4][3] = 1.0f;      /* colours default opaque */
    /* Unwritten texture coordinates are (0,0,0,1), as xemu initialises them:
     * a projective stage divides by q, and q = 0 would put every texel of an
     * unwritten stage at infinity. */
    outregs[9][3] = outregs[10][3] = outregs[11][3] = outregs[12][3] = 1.0f;

    for (s = s_start_slot; s < NV2A_VSH_SLOTS; s++) {
        const VshIns *ins = &s_decoded[s];
        uint32_t mac = ins->mac, ilu = ins->ilu;
        uint32_t mac_mask = ins->mac_mask, ilu_mask = ins->ilu_mask;
        uint32_t tdst = ins->tdst;
        uint32_t omask = ins->omask, oaddr = ins->oaddr;
        vec4 a = {{0, 0, 0, 0}}, b = {{0, 0, 0, 0}}, c = {{0, 0, 0, 0}};
        vec4 mres = {{0, 0, 0, 0}}, ires = {{0, 0, 0, 0}};

        if (ins->need & 1) a = read_src(ins, 0, in, temp, &opos, a0);
        if (ins->need & 2) b = read_src(ins, 1, in, temp, &opos, a0);
        if (ins->need & 4) c = read_src(ins, 2, in, temp, &opos, a0);

        if (mac)
            mres = run_mac(mac, a, b, c, &a0);
        if (ilu)
            ires = run_ilu(ilu, c);

        if (mac && mac != 13 && mac_mask) {
            float *d = tdst == 12 ? opos.v : (tdst < 12 ? temp[tdst].v : NULL);
            if (d) write_masked(d, &mres, mac_mask);
        }
        if (ilu && ilu_mask) {
            /* Paired with any MAC op, the ILU can only write R1 -- even when
             * the MAC half writes no temp at all. The D3D epilogue is exactly
             * that: `mul o[0].xyz` beside `rcc R1.x`, then `mad` with R1.x.
             * Sending the rcc to the temp field (R7 there) left R1.x at 0 and
             * put every vertex of every 3D batch at c[59]. */
            uint32_t it = mac ? 1u : tdst;
            float *d = it == 12 ? opos.v : (it < 12 ? temp[it].v : NULL);
            if (d) write_masked(d, &ires, ilu_mask);
        }
        if (omask) {
            const vec4 *src = ins->out_ilu ? &ires : &mres;
            if (!ins->out_reg) {                          /* to c[] */
                if (s_cxt_write && oaddr < NV2A_VSH_CONSTANTS)
                    write_masked(s_const[oaddr], src, omask);
            } else if (oaddr == 0) {
                write_masked(opos.v, src, omask);
            } else if (oaddr < 13) {
                write_masked(outregs[oaddr], src, omask);
                written |= 1u << oaddr;
            }
        }
        if (ins->last) {
            memcpy(out->pos, opos.v, sizeof out->pos);
            memcpy(out->d0, outregs[3], sizeof out->d0);
            memcpy(out->d1, outregs[4], sizeof out->d1);
            memcpy(out->fog, outregs[5], sizeof out->fog);
            memcpy(out->tex[0], outregs[9], sizeof out->tex[0]);
            memcpy(out->tex[1], outregs[10], sizeof out->tex[1]);
            memcpy(out->tex[2], outregs[11], sizeof out->tex[2]);
            memcpy(out->tex[3], outregs[12], sizeof out->tex[3]);
            out->written = written;
            return 1;
        }
    }
    return 0;
}

/* ---- execution, four at a time -------------------------------------------
 *
 * A register is four floats and nearly every operation does the same thing
 * to each of them, which is what SSE is for. Run a float at a time, a level
 * of Dave Mirra Freestyle BMX 2 with a long view in it -- thirteen hundred
 * batches -- kept the thread that executes the title's commands busy for
 * sixteen of a frame's 16.7 milliseconds, two fifths of that here, and the
 * frames that went over made the picture judder.
 *
 * The answers are the same to the bit: the operations are IEEE's either
 * way, a multiply-add is still a multiply and then an add, and a dot product
 * is summed in the same order. The few operations that are not four of one
 * thing (dst, and the whole ILU) go through the code above.
 */
#ifdef VSH_SSE2

/* Write masks, x=8 y=4 z=2 w=1, as the lanes they keep. */
#define M(x, y, z, w) { x ? ~0u : 0u, y ? ~0u : 0u, z ? ~0u : 0u, w ? ~0u : 0u }
static const uint32_t k_lanes[16][4] = {
    M(0,0,0,0), M(0,0,0,1), M(0,0,1,0), M(0,0,1,1),
    M(0,1,0,0), M(0,1,0,1), M(0,1,1,0), M(0,1,1,1),
    M(1,0,0,0), M(1,0,0,1), M(1,0,1,0), M(1,0,1,1),
    M(1,1,0,0), M(1,1,0,1), M(1,1,1,0), M(1,1,1,1)
};
#undef M

static void sse_write(float *dst, __m128 v, uint32_t mask)
{
    __m128 m = _mm_loadu_ps((const float *)k_lanes[mask & 15u]);

    _mm_storeu_ps(dst, _mm_or_ps(_mm_and_ps(m, v),
                                 _mm_andnot_ps(m, _mm_loadu_ps(dst))));
}

static __m128 sse_src(const VshIns *d, int which,
                      const float in[NV2A_VSH_INPUTS][4],
                      float reg[13][4], int a0)
{
    const VshSrc *src = &d->src[which];
    const float *s;
    __m128 r;

    if (src->mux == 1) {
        s = reg[src->temp];
    } else if (src->mux == 2) {
        s = in[d->input];
    } else {
        int ci = (int)d->ci;
        if (d->ci_a0)
            ci += a0;
        if (ci < 0 || ci >= NV2A_VSH_CONSTANTS)
            ci = 0;
        s = s_const[ci];
    }
    if (src->pick == 0)
        r = _mm_loadu_ps(s);
    else if (src->pick == 1)
        r = _mm_set1_ps(s[src->sw[0]]);
    else
        r = _mm_set_ps(s[src->sw[3]], s[src->sw[2]], s[src->sw[1]],
                       s[src->sw[0]]);
    if (src->neg)
        r = _mm_xor_ps(r, _mm_set1_ps(-0.0f));
    return r;
}

/* x*x' + y*y' + z*z', summed in that order, in the low float. */
static __m128 sse_dot3(__m128 p)
{
    __m128 s = _mm_add_ss(p, _mm_shuffle_ps(p, p, _MM_SHUFFLE(1, 1, 1, 1)));

    return _mm_add_ss(s, _mm_shuffle_ps(p, p, _MM_SHUFFLE(2, 2, 2, 2)));
}

static __m128 sse_splat(__m128 low)
{
    return _mm_shuffle_ps(low, low, _MM_SHUFFLE(0, 0, 0, 0));
}

static __m128 sse_mac(uint32_t op, __m128 a, __m128 b, __m128 c, int *a0)
{
    switch (op) {
    case 2: return _mm_mul_ps(a, b);
    case 3: return _mm_add_ps(a, c);
    case 4: return _mm_add_ps(_mm_mul_ps(a, b), c);
    case 5: return sse_splat(sse_dot3(_mm_mul_ps(a, b)));
    case 6:                                                    /* dph */
        return sse_splat(_mm_add_ss(sse_dot3(_mm_mul_ps(a, b)),
                    _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 3, 3, 3))));
    case 7: {
        __m128 p = _mm_mul_ps(a, b);
        return sse_splat(_mm_add_ss(sse_dot3(p),
                    _mm_shuffle_ps(p, p, _MM_SHUFFLE(3, 3, 3, 3))));
    }
    case 9: return _mm_min_ps(a, b);          /* a < b ? a : b, as above */
    case 10: return _mm_max_ps(a, b);         /* a > b ? a : b */
    case 11: return _mm_and_ps(_mm_cmplt_ps(a, b), _mm_set1_ps(1.0f));
    case 12: return _mm_and_ps(_mm_cmpge_ps(a, b), _mm_set1_ps(1.0f));
    case 8: case 13: {                        /* dst, arl */
        vec4 va, vb, vc, r;
        _mm_storeu_ps(va.v, a); _mm_storeu_ps(vb.v, b); _mm_storeu_ps(vc.v, c);
        r = run_mac(op, va, vb, vc, a0);
        return _mm_loadu_ps(r.v);
    }
    default: return a;                        /* mov, and the two unused */
    }
}

static int run_sse(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out)
{
    float reg[13][4];                         /* R0..R11, and oPos at 12 */
    float outregs[13][4];
    const __m128 zero = _mm_setzero_ps();
    int a0 = 0;
    uint32_t s, written = 0;

    memset(reg, 0, sizeof reg);
    memset(outregs, 0, sizeof outregs);
    /* The same defaults as above: colours opaque, texture q of 1. */
    outregs[3][3] = outregs[4][3] = 1.0f;
    outregs[9][3] = outregs[10][3] = outregs[11][3] = outregs[12][3] = 1.0f;

    for (s = s_start_slot; s < NV2A_VSH_SLOTS; s++) {
        const VshIns *ins = &s_decoded[s];
        uint32_t mac = ins->mac, ilu = ins->ilu;
        uint32_t tdst = ins->tdst;
        __m128 a = zero, b = zero, c = zero, mres = zero, ires = zero;

        if (ins->need & 1) a = sse_src(ins, 0, in, reg, a0);
        if (ins->need & 2) b = sse_src(ins, 1, in, reg, a0);
        if (ins->need & 4) c = sse_src(ins, 2, in, reg, a0);

        if (mac)
            mres = sse_mac(mac, a, b, c, &a0);
        if (ilu) {
            vec4 vc;
            _mm_storeu_ps(vc.v, c);
            vc = run_ilu(ilu, vc);
            ires = _mm_loadu_ps(vc.v);
        }

        if (mac && mac != 13 && ins->mac_mask && tdst <= 12)
            sse_write(reg[tdst], mres, ins->mac_mask);
        if (ilu && ins->ilu_mask) {
            uint32_t it = mac ? 1u : tdst;    /* beside a MAC op, always R1 */
            if (it <= 12)
                sse_write(reg[it], ires, ins->ilu_mask);
        }
        if (ins->omask) {
            __m128 v = ins->out_ilu ? ires : mres;
            uint32_t oaddr = ins->oaddr;

            if (!ins->out_reg) {                          /* to c[] */
                if (s_cxt_write && oaddr < NV2A_VSH_CONSTANTS)
                    sse_write(s_const[oaddr], v, ins->omask);
            } else if (oaddr == 0) {
                sse_write(reg[12], v, ins->omask);
            } else if (oaddr < 13) {
                sse_write(outregs[oaddr], v, ins->omask);
                written |= 1u << oaddr;
            }
        }
        if (ins->last) {
            memcpy(out->pos, reg[12], sizeof out->pos);
            memcpy(out->d0, outregs[3], sizeof out->d0);
            memcpy(out->d1, outregs[4], sizeof out->d1);
            memcpy(out->fog, outregs[5], sizeof out->fog);
            memcpy(out->tex, outregs[9], sizeof out->tex);
            out->written = written;
            return 1;
        }
    }
    return 0;
}

#endif /* VSH_SSE2 */

int nv2a_vsh_run(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out)
{
#ifdef VSH_SSE2
    dump_once();
    return run_sse(in, out);
#else
    return nv2a_vsh_run_scalar(in, out);
#endif
}
