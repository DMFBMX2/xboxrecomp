/**
 * nv2a_vp.h -- the NV2A's vertex program unit, as an interpreter.
 *
 * A title that draws 3D on Xbox does not hand the GPU a matrix. It hands it a
 * small program, run once per vertex, that turns the title's own attributes
 * into a position, colours and texture coordinates. Direct3D compiles the
 * title's vertex shader into one, and then -- this is the part that makes an
 * interpreter enough -- appends the viewport transform and the perspective
 * divide to it. What comes out is not clip space. It is the pixel the vertex
 * lands on, its depth-buffer value, and w: exactly the pre-transformed vertex
 * a 2D quad is submitted as.
 *
 * So the executor does not need a second renderer for 3D. It needs to run the
 * program, and then every batch is a screen-space batch.
 *
 * The program store, the constants and the instruction encoding are the
 * hardware's (NV_PGRAPH's "cheops" unit): 136 instructions of four dwords,
 * 192 constants of four floats.
 */
#ifndef NV2A_VP_H
#define NV2A_VP_H

#include <stdint.h>

#define NV2A_VP_MAX_INSNS    136
#define NV2A_VP_MAX_CONSTS   192
#define NV2A_VP_INPUTS       16

/* Output registers, by the index the instruction encodes. */
enum {
    NV2A_VP_OUT_POS = 0,
    NV2A_VP_OUT_D0  = 3,
    NV2A_VP_OUT_D1  = 4,
    NV2A_VP_OUT_FOG = 5,
    NV2A_VP_OUT_PTS = 6,
    NV2A_VP_OUT_B0  = 7,
    NV2A_VP_OUT_B1  = 8,
    NV2A_VP_OUT_T0  = 9,
    NV2A_VP_OUT_T1  = 10,
    NV2A_VP_OUT_T2  = 11,
    NV2A_VP_OUT_T3  = 12,
    NV2A_VP_OUTPUTS = 13
};

typedef struct {
    float    out[NV2A_VP_OUTPUTS][4];
    /* Bit n set if output n was written at all. A program that never writes
     * a colour is not asking for black. */
    uint32_t written;
} Nv2aVpResult;

/* The registers a title programs the unit through. `slot` is the dword within
 * the method's 32-dword window; four of them make one instruction or one
 * constant, and completing one advances the load pointer. */
void nv2a_vp_set_program_load(uint32_t index);
void nv2a_vp_set_program_start(uint32_t index);
void nv2a_vp_set_constant_load(uint32_t index);
void nv2a_vp_program_word(uint32_t slot, uint32_t value);
void nv2a_vp_constant_word(uint32_t slot, uint32_t value);

/* The viewport is two of the constants. SET_VIEWPORT_SCALE and
 * SET_VIEWPORT_OFFSET are not state the rasteriser applies afterwards: they
 * write c[58] and c[59], and the instructions D3D appends to each program
 * read them from there. Leave them unset and every vertex of every 3D batch
 * lands on pixel (0,0) at depth zero, with a perfectly good w. */
#define NV2A_VP_CONST_VIEWPORT_SCALE   58
#define NV2A_VP_CONST_VIEWPORT_OFFSET  59
void nv2a_vp_set_constant(uint32_t index, uint32_t component, uint32_t value);

/* RECOMP_VP_DUMP: print the program at the start address, once per distinct
 * program, as a listing. Returns 1 if this call printed one -- the caller can
 * then add what it knows, such as the vertex layout the program is fed. */
int nv2a_vp_dump_if_new(void);
/* The hash RECOMP_VP_DUMP names a program by. */
uint32_t nv2a_vp_program_hash(void);
/* A constant's current value, for the same diagnostic. */
const float *nv2a_vp_constant(uint32_t index);

/* Run the program from its start address over one vertex. Returns 0 if there
 * is no program to run or it ran off the end of the store. */
int nv2a_vp_run(const float in[NV2A_VP_INPUTS][4], Nv2aVpResult *res);

#endif /* NV2A_VP_H */
