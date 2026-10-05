/**
 * nv2a_vp.c -- see nv2a_vp.h.
 *
 * Written from the instruction layout the hardware uses, which is public in
 * every open NV2A implementation; the field positions below are that layout
 * and nothing about them is a choice.
 *
 * One instruction can do two things at once. It has a "MAC" half (move,
 * multiply, add, the dot products) and an "ILU" half (the reciprocal family,
 * exp, log, lit), each with its own opcode and write mask. Both read their
 * operands before either writes, the MAC result goes to a temporary register
 * of the instruction's choosing, the ILU result always goes to r1, and one of
 * the two can also be written to an output or a constant. D3D uses the pairing
 * for exactly one thing -- the perspective divide it appends -- and that one
 * thing is on every program, so it is not optional.
 */
#include "nv2a_vp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_prog[NV2A_VP_MAX_INSNS][4];
static float    s_const[NV2A_VP_MAX_CONSTS][4];
static uint32_t s_prog_load, s_prog_start, s_const_load;

/* Which dword of the instruction or constant at the load pointer comes next.
 *
 * The method offset says so too -- for a command that increments. One that
 * does not sends every dword to the first method of the range, the hardware
 * still files them in order, and D3D uploads a run of constants that way
 * when it has more than one command's worth: a skinned model's bone matrices,
 * for one. Trusting the offset put all of those in component 0 of a single
 * constant and never moved the load pointer; the bones stayed whatever they
 * were and every skinned and moving object in the level was drawn at the
 * origin. Counting is right for both kinds of command.
 */
static uint32_t s_prog_word, s_const_word;

void nv2a_vp_set_program_load(uint32_t index)
{
    s_prog_load = index;
    s_prog_word = 0;
}
void nv2a_vp_set_program_start(uint32_t index) { s_prog_start = index; }
void nv2a_vp_set_constant_load(uint32_t index)
{
    s_const_load = index;
    s_const_word = 0;
}

/* An instruction, taken apart once.
 *
 * A level is a hundred thousand vertices a frame through programs of twenty
 * or thirty instructions, and pulling some thirty bit fields out of four
 * dwords for every one of those was most of the time it took to draw. The
 * fields only change when the title uploads a program, which is rare; they are
 * decoded when first run after that and kept.
 */
typedef struct {
    uint8_t valid;
    uint8_t mac, ilu;
    uint8_t mux[3], reg[3], neg[3], swz[3][4];
    uint8_t v_index, c_index, a0x;
    uint8_t mac_reg, mac_mask, ilu_mask;
    uint8_t out_mask, out_is_output, out_addr, out_from_ilu;
    uint8_t final;
} VpInsn;

static VpInsn s_dec[NV2A_VP_MAX_INSNS];

void nv2a_vp_program_word(uint32_t slot, uint32_t value)
{
    (void)slot;
    if (s_prog_load < NV2A_VP_MAX_INSNS) {
        s_prog[s_prog_load][s_prog_word] = value;
        s_dec[s_prog_load].valid = 0;
    }
    if (++s_prog_word == 4u) {
        s_prog_word = 0;
        s_prog_load++;
    }
}

void nv2a_vp_constant_word(uint32_t slot, uint32_t value)
{
    (void)slot;
    if (s_const_load < NV2A_VP_MAX_CONSTS)
        memcpy(&s_const[s_const_load][s_const_word], &value, 4);
    if (++s_const_word == 4u) {
        s_const_word = 0;
        s_const_load++;
    }
}

void nv2a_vp_set_constant(uint32_t index, uint32_t component, uint32_t value)
{
    if (index < NV2A_VP_MAX_CONSTS)
        memcpy(&s_const[index][component & 3u], &value, 4);
}

/* ---- instruction fields ------------------------------------------------ */

#define FLD(tok, word, start, bits) \
    (((tok)[word] >> (start)) & ((1u << (bits)) - 1u))

enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
       MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum { MUX_R = 1, MUX_V = 2, MUX_C = 3 };

typedef struct {
    float r[13][4];                     /* r0..r11, and r12 which is oPos   */
    const float (*v)[4];
    int   a0;
} VpState;

/* The hardware's multiply: zero times anything is zero, infinity included.
 * A title that relies on it is rare; one that is broken by IEEE's NaN here
 * draws nothing at all, with nothing to say why. */
static float vp_mul(float a, float b)
{
    return (a == 0.0f || b == 0.0f) ? 0.0f : a * b;
}

static void vp_decode(VpInsn *d, const uint32_t *tok)
{
    d->mac = (uint8_t)FLD(tok, 1, 21, 4);
    d->ilu = (uint8_t)FLD(tok, 1, 25, 3);
    d->c_index = (uint8_t)FLD(tok, 1, 13, 8);
    d->v_index = (uint8_t)FLD(tok, 1, 9, 4);
    d->a0x = (uint8_t)FLD(tok, 3, 1, 1);

    d->mux[0] = (uint8_t)FLD(tok, 2, 26, 2);
    d->reg[0] = (uint8_t)FLD(tok, 2, 28, 4);
    d->neg[0] = (uint8_t)FLD(tok, 1, 8, 1);
    d->swz[0][0] = (uint8_t)FLD(tok, 1, 6, 2);
    d->swz[0][1] = (uint8_t)FLD(tok, 1, 4, 2);
    d->swz[0][2] = (uint8_t)FLD(tok, 1, 2, 2);
    d->swz[0][3] = (uint8_t)FLD(tok, 1, 0, 2);

    d->mux[1] = (uint8_t)FLD(tok, 2, 11, 2);
    d->reg[1] = (uint8_t)FLD(tok, 2, 13, 4);
    d->neg[1] = (uint8_t)FLD(tok, 2, 25, 1);
    d->swz[1][0] = (uint8_t)FLD(tok, 2, 23, 2);
    d->swz[1][1] = (uint8_t)FLD(tok, 2, 21, 2);
    d->swz[1][2] = (uint8_t)FLD(tok, 2, 19, 2);
    d->swz[1][3] = (uint8_t)FLD(tok, 2, 17, 2);

    d->mux[2] = (uint8_t)FLD(tok, 3, 28, 2);
    d->reg[2] = (uint8_t)((FLD(tok, 2, 0, 2) << 2) | FLD(tok, 3, 30, 2));
    d->neg[2] = (uint8_t)FLD(tok, 2, 10, 1);
    d->swz[2][0] = (uint8_t)FLD(tok, 2, 8, 2);
    d->swz[2][1] = (uint8_t)FLD(tok, 2, 6, 2);
    d->swz[2][2] = (uint8_t)FLD(tok, 2, 4, 2);
    d->swz[2][3] = (uint8_t)FLD(tok, 2, 2, 2);

    d->mac_reg  = (uint8_t)FLD(tok, 3, 20, 4);
    d->mac_mask = (uint8_t)FLD(tok, 3, 24, 4);
    d->ilu_mask = (uint8_t)FLD(tok, 3, 16, 4);
    d->out_mask = (uint8_t)FLD(tok, 3, 12, 4);
    d->out_is_output = (uint8_t)FLD(tok, 3, 11, 1);
    d->out_addr = (uint8_t)FLD(tok, 3, 3, 8);
    d->out_from_ilu = (uint8_t)FLD(tok, 3, 2, 1);
    d->final = (uint8_t)FLD(tok, 3, 0, 1);
    d->valid = 1;
}

static void vp_operand(const VpState *st, const VpInsn *d, int which,
                       float out[4])
{
    const float *src;
    static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const uint8_t *swz = d->swz[which];

    switch (d->mux[which]) {
    case MUX_R:
        src = st->r[d->reg[which] < 13 ? d->reg[which] : 12];
        break;
    case MUX_V:
        src = st->v[d->v_index];
        break;
    case MUX_C: {
        int index = d->c_index;
        if (d->a0x)                     /* indexed by the address register */
            index += st->a0;
        src = (index >= 0 && index < NV2A_VP_MAX_CONSTS) ? s_const[index]
                                                         : zero;
        break;
    }
    default:
        src = zero;
        break;
    }
    if (d->neg[which]) {
        out[0] = -src[swz[0]]; out[1] = -src[swz[1]];
        out[2] = -src[swz[2]]; out[3] = -src[swz[3]];
    } else {
        out[0] = src[swz[0]]; out[1] = src[swz[1]];
        out[2] = src[swz[2]]; out[3] = src[swz[3]];
    }
}

static void vp_write(float *dst, const float val[4], uint32_t mask)
{
    if (mask & 8u) dst[0] = val[0];
    if (mask & 4u) dst[1] = val[1];
    if (mask & 2u) dst[2] = val[2];
    if (mask & 1u) dst[3] = val[3];
}

uint32_t nv2a_vp_program_hash(void)
{
    uint32_t pc, hash = 2166136261u;
    int i;

    for (pc = s_prog_start; pc < NV2A_VP_MAX_INSNS; pc++) {
        for (i = 0; i < 4; i++)
            hash = (hash ^ s_prog[pc][i]) * 16777619u;
        if (s_prog[pc][3] & 1u)
            break;
    }
    return hash;
}

const float *nv2a_vp_constant(uint32_t index)
{
    static const float zero[4];
    return index < NV2A_VP_MAX_CONSTS ? s_const[index] : zero;
}

int nv2a_vp_dump_if_new(void)
{
    static int enabled = -1;
    static uint32_t seen[256];
    static int seen_count;
    static const char *const mac_name[16] = {
        "nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4",
        "dst", "min", "max", "slt", "sge", "arl", "?14", "?15" };
    static const char *const ilu_name[8] = {
        "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit" };
    static const char *const out_name[16] = {
        "oPos", "o1", "o2", "oD0", "oD1", "oFog", "oPts", "oB0",
        "oB1", "oT0", "oT1", "oT2", "oT3", "o13", "o14", "o15" };
    uint32_t pc, hash = 2166136261u, len = 0;
    int i, which;

    if (enabled < 0)
        enabled = getenv("RECOMP_VP_DUMP") != NULL;
    if (!enabled)
        return 0;

    for (pc = s_prog_start; pc < NV2A_VP_MAX_INSNS; pc++) {
        for (i = 0; i < 4; i++)
            hash = (hash ^ s_prog[pc][i]) * 16777619u;
        len++;
        if (s_prog[pc][3] & 1u)
            break;
    }
    for (i = 0; i < seen_count; i++)
        if (seen[i] == hash)
            return 0;
    if (seen_count >= 256)
        return 0;
    seen[seen_count++] = hash;

    fprintf(stderr, "[VP] program %08X: start %u, %u instructions\n",
            hash, s_prog_start, len);
    for (pc = s_prog_start; pc < s_prog_start + len; pc++) {
        VpInsn d;
        char line[256];
        int n = 0;

        vp_decode(&d, s_prog[pc]);
        n += snprintf(line + n, sizeof line - n, "  %3u: %s/%s", pc,
                      mac_name[d.mac & 15], ilu_name[d.ilu & 7]);
        for (which = 0; which < 3; which++) {
            static const char xyzw[] = "xyzw";
            char src[16];
            if (d.mux[which] == MUX_R)
                snprintf(src, sizeof src, "r%u", d.reg[which]);
            else if (d.mux[which] == MUX_V)
                snprintf(src, sizeof src, "v%u", d.v_index);
            else if (d.mux[which] == MUX_C)
                snprintf(src, sizeof src, "c[%s%u]", d.a0x ? "a0+" : "",
                         d.c_index);
            else
                snprintf(src, sizeof src, "-");
            n += snprintf(line + n, sizeof line - n, " %s%s.%c%c%c%c",
                          d.neg[which] ? "-" : "", src,
                          xyzw[d.swz[which][0]], xyzw[d.swz[which][1]],
                          xyzw[d.swz[which][2]], xyzw[d.swz[which][3]]);
        }
        n += snprintf(line + n, sizeof line - n, " -> r%u/%X r1/%X",
                      d.mac_reg, d.mac_mask, d.ilu_mask);
        if (d.out_mask)
            n += snprintf(line + n, sizeof line - n, " %s%s%u/%X(%s)",
                          d.out_is_output ? out_name[d.out_addr & 15] : "c",
                          d.out_is_output ? "#" : "", d.out_addr, d.out_mask,
                          d.out_from_ilu ? "ilu" : "mac");
        fprintf(stderr, "%s%s\n", line, d.final ? " END" : "");
    }
    fprintf(stderr, "  c58 (scale) %g %g %g %g  c59 (offset) %g %g %g %g\n",
            s_const[58][0], s_const[58][1], s_const[58][2], s_const[58][3],
            s_const[59][0], s_const[59][1], s_const[59][2], s_const[59][3]);
    fflush(stderr);
    return 1;
}

int nv2a_vp_run(const float in[NV2A_VP_INPUTS][4], Nv2aVpResult *res)
{
    VpState st;
    uint32_t pc, steps;
    int i;

    memset(&st, 0, sizeof st);
    st.v = in;
    memset(res, 0, sizeof *res);
    for (i = 0; i < NV2A_VP_OUTPUTS; i++)
        res->out[i][3] = 1.0f;
    st.r[12][3] = 1.0f;

    pc = s_prog_start;
    for (steps = 0; steps < NV2A_VP_MAX_INSNS; steps++, pc++) {
        VpInsn *d;
        uint32_t mac, ilu;
        float a[4], b[4], c[4], mres[4], ires[4];
        int have_m = 0, have_i = 0;

        if (pc >= NV2A_VP_MAX_INSNS)
            return 0;
        d = &s_dec[pc];
        if (!d->valid)
            vp_decode(d, s_prog[pc]);
        mac = d->mac;
        ilu = d->ilu;

        /* Operands nothing reads are not fetched: A feeds only the MAC half,
         * B only its two-operand forms, C the adds and the ILU half. */
        if (mac != MAC_NOP)
            vp_operand(&st, d, 0, a);
        if (mac == MAC_MUL || mac == MAC_MAD || (mac >= MAC_DP3 && mac <= MAC_SGE))
            vp_operand(&st, d, 1, b);
        if (mac == MAC_ADD || mac == MAC_MAD || ilu != ILU_NOP)
            vp_operand(&st, d, 2, c);

        switch (mac) {
        case MAC_MOV:
            memcpy(mres, a, sizeof mres); have_m = 1; break;
        case MAC_MUL:
            for (i = 0; i < 4; i++) mres[i] = vp_mul(a[i], b[i]);
            have_m = 1; break;
        case MAC_ADD:
            for (i = 0; i < 4; i++) mres[i] = a[i] + c[i];
            have_m = 1; break;
        case MAC_MAD:
            for (i = 0; i < 4; i++) mres[i] = vp_mul(a[i], b[i]) + c[i];
            have_m = 1; break;
        case MAC_DP3:
            mres[0] = vp_mul(a[0], b[0]) + vp_mul(a[1], b[1])
                    + vp_mul(a[2], b[2]);
            mres[1] = mres[2] = mres[3] = mres[0];
            have_m = 1; break;
        case MAC_DPH:
            mres[0] = vp_mul(a[0], b[0]) + vp_mul(a[1], b[1])
                    + vp_mul(a[2], b[2]) + b[3];
            mres[1] = mres[2] = mres[3] = mres[0];
            have_m = 1; break;
        case MAC_DP4:
            mres[0] = vp_mul(a[0], b[0]) + vp_mul(a[1], b[1])
                    + vp_mul(a[2], b[2]) + vp_mul(a[3], b[3]);
            mres[1] = mres[2] = mres[3] = mres[0];
            have_m = 1; break;
        case MAC_DST:
            mres[0] = 1.0f;
            mres[1] = vp_mul(a[1], b[1]);
            mres[2] = a[2];
            mres[3] = b[3];
            have_m = 1; break;
        case MAC_MIN:
            for (i = 0; i < 4; i++) mres[i] = a[i] < b[i] ? a[i] : b[i];
            have_m = 1; break;
        case MAC_MAX:
            for (i = 0; i < 4; i++) mres[i] = a[i] > b[i] ? a[i] : b[i];
            have_m = 1; break;
        case MAC_SLT:
            for (i = 0; i < 4; i++) mres[i] = a[i] < b[i] ? 1.0f : 0.0f;
            have_m = 1; break;
        case MAC_SGE:
            for (i = 0; i < 4; i++) mres[i] = a[i] >= b[i] ? 1.0f : 0.0f;
            have_m = 1; break;
        case MAC_ARL:
            st.a0 = (int)floorf(a[0]);
            break;
        default:
            break;
        }

        switch (ilu) {
        case ILU_MOV:
            memcpy(ires, c, sizeof ires); have_i = 1; break;
        case ILU_RCP:
            ires[0] = (c[0] == 1.0f) ? 1.0f : 1.0f / c[0];
            ires[1] = ires[2] = ires[3] = ires[0];
            have_i = 1; break;
        case ILU_RCC: {
            /* The clamped reciprocal the appended divide uses: a vertex at
             * w = 0 gets a very large coordinate rather than an infinite
             * one, which is what lets the rasteriser clip it. */
            float t = (c[0] == 1.0f) ? 1.0f : 1.0f / c[0];
            if (t > 0.0f) {
                if (t < 5.42101e-020f) t = 5.42101e-020f;
                if (t > 1.884467e+019f) t = 1.884467e+019f;
            } else {
                if (t < -1.884467e+019f) t = -1.884467e+019f;
                if (t > -5.42101e-020f) t = -5.42101e-020f;
            }
            ires[0] = ires[1] = ires[2] = ires[3] = t;
            have_i = 1; break;
        }
        case ILU_RSQ: {
            float t = fabsf(c[0]);
            ires[0] = (t == 1.0f) ? 1.0f : 1.0f / sqrtf(t);
            ires[1] = ires[2] = ires[3] = ires[0];
            have_i = 1; break;
        }
        case ILU_EXP: {
            float fl = floorf(c[0]);
            ires[0] = powf(2.0f, fl);
            ires[1] = c[0] - fl;
            ires[2] = powf(2.0f, c[0]);
            ires[3] = 1.0f;
            have_i = 1; break;
        }
        case ILU_LOG: {
            float t = fabsf(c[0]);
            if (t == 0.0f) {
                ires[0] = ires[2] = -INFINITY;
                ires[1] = 1.0f;
            } else {
                float l = log2f(t), fl = floorf(l);
                ires[0] = fl;
                ires[1] = t / powf(2.0f, fl);
                ires[2] = l;
            }
            ires[3] = 1.0f;
            have_i = 1; break;
        }
        case ILU_LIT: {
            float x = c[0] > 0.0f ? c[0] : 0.0f;
            float y = c[1] > 0.0f ? c[1] : 0.0f;
            float w = c[3];
            if (w < -127.9961f) w = -127.9961f;
            if (w >  127.9961f) w =  127.9961f;
            ires[0] = 1.0f;
            ires[1] = x;
            ires[2] = (x > 0.0f) ? powf(y, w) : 0.0f;
            ires[3] = 1.0f;
            have_i = 1; break;
        }
        default:
            break;
        }

        /* All reads are done; now the writes. */
        if (have_m)
            vp_write(st.r[d->mac_reg < 13 ? d->mac_reg : 12], mres, d->mac_mask);
        if (have_i)
            vp_write(st.r[1], ires, d->ilu_mask);

        if (d->out_mask && (d->out_from_ilu ? have_i : have_m)) {
            const float *val = d->out_from_ilu ? ires : mres;
            uint32_t addr = d->out_addr;

            if (d->out_is_output) {                     /* an output        */
                addr &= 0xFu;
                if (addr < NV2A_VP_OUTPUTS) {
                    vp_write(res->out[addr], val, d->out_mask);
                    res->written |= 1u << addr;
                    if (addr == NV2A_VP_OUT_POS)
                        vp_write(st.r[12], val, d->out_mask);
                }
            } else if (addr < NV2A_VP_MAX_CONSTS) {      /* a constant       */
                vp_write(s_const[addr], val, d->out_mask);
            }
        }

        if (d->final)
            return 1;
    }
    return 0;
}
