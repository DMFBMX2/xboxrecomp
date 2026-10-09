/* nv2a_vsh_run against nv2a_vsh_run_scalar: the same answers, to the bit.
 *
 * nv2a_vsh_run does four floats at a time where the processor can, and the
 * scalar one is the program's meaning written out plainly. Programs are made
 * up at random -- every operation, every swizzle, mask and destination, with
 * awkward floats among the inputs -- and both are run on each. A NaN is
 * allowed to differ in its bits from another NaN: which operand's a compiler
 * keeps is its own business.
 *
 * With a number as argument it also times the two over that many runs of a
 * program shaped like a title's: a matrix of DP4s, a lit colour, texture
 * coordinates and the screen-space epilogue.
 */
#include "nv2a_vsh_interp.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t s_seed = 0x2545F491u;

static uint32_t rnd(void)
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    return s_seed;
}

static float rnd_float(void)
{
    static const float odd[] = {
        0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 2.0f, 127.5f, -130.0f, 1e-30f,
        1e30f, -1e30f, 3.4e38f, 1e-42f
    };
    uint32_t r = rnd();

    switch (r % 8u) {
    case 0: return odd[(r >> 8) % (sizeof odd / sizeof odd[0])];
    case 1: return (r >> 8) % 2u ? INFINITY : -INFINITY;
    case 2: return NAN;
    case 3: return (float)((int)((r >> 8) % 2001u) - 1000);
    default: return ((float)(r >> 8) / 8388608.0f - 1.0f) * 16.0f;
    }
}

static int same(const float *a, const float *b, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (isnan(a[i]) && isnan(b[i]))
            continue;
        if (memcmp(&a[i], &b[i], sizeof a[i]))
            return 0;
    }
    return 1;
}

/* One made-up program of `len` instructions, the last one FINAL. */
static void random_program(uint32_t len)
{
    uint32_t s, w[4];

    for (s = 0; s < len; s++) {
        w[0] = 0;
        w[1] = rnd() & 0x0FFFFFFFu;         /* ops, constant, input, A */
        w[2] = rnd();
        w[3] = rnd() & ~1u;
        /* A constant write lands somewhere in c[]; keep them few. */
        if (rnd() % 4u)
            w[3] |= 1u << 11;
        if (s + 1 == len)
            w[3] |= 1u;
        nv2a_vsh_set_instruction(s, w);
    }
}

static void random_constants(float keep[NV2A_VSH_CONSTANTS][4])
{
    uint32_t i;
    int k;

    for (i = 0; i < NV2A_VSH_CONSTANTS; i++) {
        for (k = 0; k < 4; k++)
            keep[i][k] = rnd_float();
        nv2a_vsh_set_constant(i, keep[i]);
    }
}

static int compare(int rounds)
{
    static float consts[NV2A_VSH_CONSTANTS][4], after[2][NV2A_VSH_CONSTANTS][4];
    float in[NV2A_VSH_INPUTS][4];
    Nv2aVshOutput o[2];
    int r, i, k, which;

    for (r = 0; r < rounds; r++) {
        int ran[2];

        random_program(1u + rnd() % 24u);
        random_constants(consts);
        nv2a_vsh_set_cxt_write(rnd() % 2u);
        for (i = 0; i < NV2A_VSH_INPUTS; i++)
            for (k = 0; k < 4; k++)
                in[i][k] = rnd_float();

        for (which = 0; which < 2; which++) {
            uint32_t c;

            for (c = 0; c < NV2A_VSH_CONSTANTS; c++)
                nv2a_vsh_set_constant(c, consts[c]);
            memset(&o[which], 0, sizeof o[which]);
            ran[which] = which
                ? nv2a_vsh_run((const float (*)[4])in, &o[which])
                : nv2a_vsh_run_scalar((const float (*)[4])in, &o[which]);
            for (c = 0; c < NV2A_VSH_CONSTANTS; c++)
                memcpy(after[which][c], nv2a_vsh_constant(c),
                       sizeof after[which][c]);
        }
        if (ran[0] != ran[1] || o[0].written != o[1].written
                || !same(o[0].pos, o[1].pos,
                         (int)(offsetof(Nv2aVshOutput, written)
                               / sizeof(float)))
                || !same(&after[0][0][0], &after[1][0][0],
                         NV2A_VSH_CONSTANTS * 4)) {
            printf("FAIL: program %d differs (ran %d/%d, written %X/%X,"
                   " pos %g %g %g %g / %g %g %g %g)\n", r, ran[0], ran[1],
                   o[0].written, o[1].written,
                   o[0].pos[0], o[0].pos[1], o[0].pos[2], o[0].pos[3],
                   o[1].pos[0], o[1].pos[1], o[1].pos[2], o[1].pos[3]);
            puts("  RECOMP_VSH_DUMP=1 prints the program");
            return 1;
        }
    }
    return 0;
}

/* ---- a program like a title's, for timing -------------------------------- */

enum { T = 1, V = 2, C = 3 };
#define XYZW 0x1B
#define WWWW 0xFF
#define XXXX 0x00

static void src(uint32_t w[4], int which, int mux, int reg, int swz)
{
    if (which == 0) { w[1] |= swz; w[2] |= (mux << 26) | (reg << 28); }
    else if (which == 1) { w[2] |= (swz << 17) | (mux << 11) | (reg << 13); }
    else { w[2] |= (swz << 2) | (reg >> 2); w[3] |= (mux << 28) | ((reg & 3) << 30); }
}

static uint32_t s_slot;

static void emit(uint32_t mac, uint32_t ilu, uint32_t ci, uint32_t input,
                 int am, int ar, int as, int bm, int br, int bs,
                 int cm, int cr, int cs, uint32_t w3)
{
    uint32_t w[4] = {0};

    w[1] = (ilu << 25) | (mac << 21) | (ci << 13) | (input << 9);
    src(w, 0, am, ar, as); src(w, 1, bm, br, bs); src(w, 2, cm, cr, cs);
    w[3] |= w3;
    nv2a_vsh_set_instruction(s_slot++, w);
}

#define TO_TEMP(reg, mask)  (((uint32_t)(mask) << 24) | ((uint32_t)(reg) << 20))
#define TO_OUT(reg, mask)   (((uint32_t)(mask) << 12) | (1u << 11) \
                             | ((uint32_t)(reg) << 3))

static void title_program(void)
{
    int i;

    s_slot = 0;
    for (i = 0; i < 4; i++)             /* dp4 oPos.{xyzw}, v0, c[96+i] */
        emit(7, 0, 96 + i, 0, V, 0, XYZW, C, 0, XYZW, T, 0, XYZW,
             TO_OUT(0, 8 >> i));
    for (i = 0; i < 3; i++)             /* dp3 R0.{xyz}, v2, c[100+i] */
        emit(5, 0, 100 + i, 2, V, 0, XYZW, C, 0, XYZW, T, 0, XYZW,
             TO_TEMP(0, 8 >> i));
    emit(5, 0, 104, 0, T, 0, XYZW, C, 0, XYZW, T, 0, XYZW, TO_TEMP(1, 8));
    emit(10, 0, 105, 0, T, 1, XXXX, C, 0, XXXX, T, 0, XYZW, TO_TEMP(1, 8));
    emit(4, 0, 106, 0, T, 1, XXXX, C, 0, XYZW, C, 0, XYZW, TO_OUT(3, 15));
    emit(1, 0, 0, 9, V, 0, XYZW, T, 0, XYZW, T, 0, XYZW, TO_OUT(9, 15));
    emit(1, 0, 0, 10, V, 0, XYZW, T, 0, XYZW, T, 0, XYZW, TO_OUT(10, 15));
    /* The epilogue: mul oPos.xyz, R12, c[58] with rcc R1.x, R12.w; then
     * mad oPos.xyz, R12, R1.x, c[59]. */
    emit(2, 3, 58, 0, T, 12, XYZW, C, 0, XYZW, T, 12, WWWW,
         (8u << 16) | TO_OUT(0, 14));
    emit(4, 0, 59, 0, T, 12, XYZW, T, 1, XXXX, C, 0, XYZW, TO_OUT(0, 14) | 1u);
}

static int time_them(long runs)
{
    float in[NV2A_VSH_INPUTS][4];
    Nv2aVshOutput o;
    double ms[2], sum = 0;
    uint32_t c;
    int i, k, which;
    long n;

    title_program();
    nv2a_vsh_set_cxt_write(0);
    for (c = 0; c < NV2A_VSH_CONSTANTS; c++) {
        float v[4];
        for (k = 0; k < 4; k++)
            v[k] = (float)(rnd() % 2000u) / 1000.0f - 1.0f;
        nv2a_vsh_set_constant(c, v);
    }
    for (i = 0; i < NV2A_VSH_INPUTS; i++)
        for (k = 0; k < 4; k++)
            in[i][k] = (float)(rnd() % 2000u) / 100.0f - 10.0f;

    for (which = 0; which < 2; which++) {
        clock_t began = clock();

        for (n = 0; n < runs; n++) {
            in[0][0] = (float)(n & 1023);
            if (which)
                nv2a_vsh_run((const float (*)[4])in, &o);
            else
                nv2a_vsh_run_scalar((const float (*)[4])in, &o);
            sum += o.pos[0];
        }
        ms[which] = (double)(clock() - began) * 1000.0 / CLOCKS_PER_SEC;
    }
    printf("%ld runs of %u instructions: scalar %.0f ms (%.0f ns each),"
           " nv2a_vsh_run %.0f ms (%.0f ns each), %.2f times as fast"
           " [%g]\n", runs, s_slot, ms[0], ms[0] * 1e6 / (double)runs,
           ms[1], ms[1] * 1e6 / (double)runs,
           ms[1] > 0 ? ms[0] / ms[1] : 0.0, sum);
    return 0;
}

int main(int argc, char **argv)
{
    if (compare(200000))
        return 1;
    puts("ok");
    if (argc > 1)
        return time_them(atol(argv[1]));
    return 0;
}
