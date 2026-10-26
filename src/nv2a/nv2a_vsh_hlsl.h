/*
 * An NV2A vertex program as HLSL.
 *
 * The executor runs a title's vertex programs on the CPU, a vertex at a time
 * (src/kernel/nv2a_vsh_interp.c), which on a busy view is most of a frame.
 * nv2a_vsh_source() writes the same program as HLSL, so that a sink can have
 * the GPU do it. The encoding and what each operation means are the
 * interpreter's; this follows its scalar version instruction for
 * instruction, and a test runs the two side by side on made-up programs.
 *
 * No graphics API in here: text in, text out.
 */
#ifndef XBOXRECOMP_NV2A_VSH_HLSL_H
#define XBOXRECOMP_NV2A_VSH_HLSL_H

#include <stddef.h>
#include <stdint.h>

#define NV2A_VSH_HLSL_SLOTS      136    /* program memory, as the interpreter */
#define NV2A_VSH_HLSL_CONSTANTS  192

/* What a program turned out to need. */
typedef struct {
    uint32_t slots;             /* instructions, from the start slot to FINAL */
    uint32_t inputs;            /* bit n: the program reads v[n] */
    /* Bit n: the program writes output register n (3 oD0, 4 oD1, 5 oFog,
     * 9..12 oT0..oT3), whatever its mask, as Nv2aVshOutput.written has it. */
    uint32_t written;
    int      writes_constants;  /* it writes c[], which a GPU cannot keep */
    int      relative;          /* it addresses c[] through a0 */
} Nv2aVshHlslInfo;

/* The shader's constants: float4 vc[192], register b0, the title's c[]. */
#define NV2A_VSH_HLSL_CBUFFER_BYTES (NV2A_VSH_HLSL_CONSTANTS * 16)

/* Writes, into buf, HLSL that declares
 *
 *   struct VshOut { float4 pos, d0, d1, fog, t0, t1, t2, t3; };
 *   VshOut vsh_run(float4 v[16]);
 *
 * and returns its length, or -1 if there is no program to run from `start`
 * (no FINAL within memory) or it does not fit in `size`. The outputs are the
 * interpreter's: pos as the program wrote it (screen space, w kept), d0 and
 * d1 clamped to 0..1, an output the program never wrote left at its default
 * (colours opaque, texture q 1). `program` is the title's program memory,
 * four words an instruction. `cxt_write` is whether writes to c[] are
 * enabled; if they are, and the program makes any, *info says so. */
int nv2a_vsh_source(const uint32_t program[][4], uint32_t start,
                    int cxt_write, char *buf, size_t size,
                    Nv2aVshHlslInfo *info);

#endif
