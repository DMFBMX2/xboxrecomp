/*
 * The NV2A's pixel pipeline as an HLSL pixel shader.
 *
 * A pixel on the NV2A is made in two steps. Four texture stages each produce
 * a colour, by a mode of their own: a plain lookup, a cube map, or one of the
 * "texture shader" modes in which a stage's coordinates are combined with the
 * colour an earlier stage produced before anything is looked up. Then up to
 * eight register combiners and a final one mix those four colours with the
 * vertex colours, constants and fog.
 *
 * nv2a_psh_source() writes the shader that does both for one setup, from the
 * words the title wrote to the GPU. It follows xemu's generator
 * (hw/xbox/nv2a/pgraph/glsl/psh.c) for the texture stages and this toolkit's
 * own software combiners (src/kernel/nv2a_combiner.c) for the rest, so that
 * the software path and this one do the same sums.
 *
 * No graphics API in here: text in, text out.
 */
#ifndef XBOXRECOMP_NV2A_PSH_HLSL_H
#define XBOXRECOMP_NV2A_PSH_HLSL_H

#include <stddef.h>
#include <stdint.h>

#include "../kernel/nv2a_combiner.h"

/* What is bound to a stage, which decides how the shader declares it. */
enum {
    NV2A_PSH_TEX_NONE = 0,
    NV2A_PSH_TEX_2D,
    NV2A_PSH_TEX_3D,
    NV2A_PSH_TEX_CUBE
};

/* Everything that changes the shader's text. Two setups with the same key
 * are drawn with the same shader; what differs between them (the combiners'
 * constants, the fog colour, the alpha reference) is in Nv2aPshConstants.
 * Zero the whole key before filling it in: it is compared as bytes. */
typedef struct {
    /* The combiner words and the stage program. factor0/1 and final_c0/c1
     * are constants and must be left zero here. */
    Nv2aCombiner rc;
    uint32_t dot_mapping;           /* SET_DOT_RGBMAPPING */
    uint32_t other_stage_input;     /* SET_SHADER_OTHER_STAGE_INPUT */
    uint32_t clip_plane_mode;       /* SET_SHADER_CLIP_PLANE_MODE */
    uint32_t alpha_func;            /* 0x200..0x207, or 0: no alpha test */
    uint8_t  tex_kind[4];           /* NV2A_PSH_TEX_* */
    uint8_t  alpha_kill[4];         /* a texel with no alpha kills the pixel */
} Nv2aPshKey;

/* The shader's constant buffer (register b0), as HLSL lays it out. */
typedef struct {
    float factor0[8][4];            /* the combiners' c0, r g b a */
    float factor1[8][4];
    float final_c0[4], final_c1[4];
    float fog_color[4];
    /* What a stage's coordinates are multiplied by before the lookup: 1, or
     * one over the size for a texture addressed in texels. */
    float tex_scale[4][4];
    float alpha_ref;                /* 0..255 */
    float pad[3];
} Nv2aPshConstants;

/* The shader's input, which a vertex shader has to produce:
 *
 *   SV_POSITION, COLOR0 (diffuse), COLOR1 (specular), both r g b a,
 *   TEXCOORD0..3 (float4: s t r q of each stage), TEXCOORD4 (float: fog,
 *   1 = none).
 *
 * Textures are t0..t3 and samplers s0..s3, by stage.
 *
 * Writes the source into buf and returns its length, or -1 if it does not
 * fit. *unsupported gets a bit set for each stage whose mode is not
 * translated; such a stage reads zero. */
int nv2a_psh_source(const Nv2aPshKey *key, char *buf, size_t size,
                    uint32_t *unsupported);

#endif
