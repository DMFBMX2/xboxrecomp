/**
 * Where the pushbuffer executor's output goes.
 *
 * nv2a_pb_exec.c decodes the title's NV2A command stream -- which surface,
 * which vertex streams, which texture -- and then has to put the result
 * somewhere. Left alone it rasterises into the guest framebuffer on the CPU,
 * which is enough to see that a title draws and not enough to watch it.
 *
 * A sink takes the same decoded batches and draws them with a real GPU. The
 * decode stays in one place, so the software path and the hardware path can
 * never disagree about what the stream said; only the last step differs.
 *
 * The kernel library knows nothing about Direct3D. The sink is a table of
 * callbacks so that whichever renderer a title links registers itself -- see
 * src/nv2a/nv2a_d3d11_sink.c for the D3D11 one.
 *
 * Every callback runs on the NV2A poll thread, in stream order.
 */
#ifndef NV2A_PB_SINK_H
#define NV2A_PB_SINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One vertex of a screen-space batch. Position is in surface pixels -- as the
 * title submitted it, or as its vertex program computed it -- with z scaled
 * to 0..1 and rhw the reciprocal of clip w. Texture coordinates are in texels
 * of the bound texture, whichever convention the title used. */
typedef struct {
    float    x, y, z, rhw;
    uint32_t argb;
    float    u, v;
} Nv2aSinkVertex;

/* The texture bound to stage 0 when the batch was drawn. `offset` is a guest
 * address; together with the size and format it identifies the texture, and
 * nv2a_pb_exec_decode_texture() turns it into pixels. addr_u and addr_v are
 * 1 wrap, 2 mirror, 3 clamp to edge, 4 border, 5 clamp. */
typedef struct {
    int      valid;
    uint32_t offset;
    uint32_t width, height;
    uint32_t format;                /* NV097 colour-format code */
    uint32_t addr_u, addr_v;        /* NV097 wrap modes */
    uint32_t bytes;                 /* size of the guest image, for hashing */
    /* Guest address of the 256-entry A8R8G8B8 palette for a palettised
     * format, else 0. Part of what the texture looks like: the same indices
     * under a different palette are a different picture. */
    uint32_t palette;
    /* How many mip levels the title supplied (1 = just the image), and the
     * NV097_SET_TEXTURE_FILTER word as written: bits 0..12 a signed LOD bias
     * in 1/256ths of a level, 16..23 the minification filter (1 nearest,
     * 2 linear, 3/4 the same picking the nearest mip level, 5/6 the same
     * blending two levels, 7 a convolution of level 0), 24..27 the
     * magnification filter (1 nearest, 2 linear, 4 convolution). Zero is a
     * title that has not set one. */
    uint32_t levels;
    uint32_t filter;
} Nv2aSinkTexture;

/* Output-merger and rasteriser state. Factors and functions are the NV097
 * (OpenGL-numbered) values. */
typedef struct {
    int      blend_enable;
    uint32_t blend_src, blend_dst;
    /* NV097_SET_BLEND_EQUATION (0x8006 add, 0x800A subtract, 0x800B reverse
     * subtract, 0x8007 min, 0x8008 max; 0 = never set, add) and
     * NV097_SET_COLOR_MASK as the sink wants it: bit 0 red, 1 green, 2 blue,
     * 3 alpha, with color_mask_set saying the title has written one. */
    uint32_t blend_equation;
    uint32_t color_mask;
    int      color_mask_set;
    int      alpha_test_enable;
    uint32_t alpha_func, alpha_ref;
    /* Depth. `z` in a vertex is the depth-buffer value scaled to 0..1. */
    int      depth_test_enable;
    uint32_t depth_func;            /* 0x200 NEVER .. 0x207 ALWAYS */
    int      depth_write;
    /* Stencil. The function is 0x200 NEVER .. 0x207 ALWAYS like the depth
     * one (0 = never set, ALWAYS); the three operations are the GL names the
     * NV2A takes -- 0x1E00 keep, 0 zero, 0x1E01 replace, 0x1E02/0x1E03
     * saturating increment/decrement, 0x150A invert, 0x8507/0x8508 wrapping
     * increment/decrement -- with stencil_ops_set saying which of fail (bit
     * 0), zfail (1) and zpass (2) the title has written; an unwritten one is
     * keep. */
    int      stencil_enable;
    uint32_t stencil_func, stencil_ref, stencil_mask, stencil_write_mask;
    uint32_t stencil_fail, stencil_zfail, stencil_zpass;
    uint32_t stencil_ops_set;
    int      stencil_write_mask_set, stencil_mask_set;
    /* Culling, in window space: front_face is 0x900 clockwise or 0x901
     * counter-clockwise, cull_face 0x404 front, 0x405 back, 0x408 both. */
    int      cull_enable;
    uint32_t cull_face, front_face;
    /* What the register combiners multiply their result by: 1, 2 or 4. The
     * combiners themselves are not translated -- texture times diffuse is
     * assumed -- but the scale is the difference between a level lit as its
     * artists lit it and one at half brightness, since a title that uses 2x
     * bakes its lighting with 0.5 as full. */
    int      color_scale;
} Nv2aSinkState;

/* What a clear() touches. */
#define NV2A_SINK_CLEAR_COLOR 1u
#define NV2A_SINK_CLEAR_DEPTH 2u
/* Stencil too, to the value in bits 8..15 of `what`. */
#define NV2A_SINK_CLEAR_STENCIL 4u

typedef struct {
    /* NV097_CLEAR_SURFACE. `what` is NV2A_SINK_CLEAR_* bits; depth clears to
     * the far plane. */
    void (*clear)(uint32_t argb, uint32_t width, uint32_t height,
                  uint32_t what);
    /* `count` vertices forming count/3 triangles. `tex` is NULL when the
     * batch carries no usable texture coordinates. */
    void (*triangles)(const Nv2aSinkVertex *v, uint32_t count,
                      const Nv2aSinkTexture *tex, const Nv2aSinkState *state);
    /* The title asked for the frame to be shown. */
    void (*flip)(void);
    /* Visibility tests. Between count_begin() and count_end() the sink counts
     * the pixels of every batch that pass the depth test; count_end() returns
     * that count in the title's own pixels, however large the sink draws,
     * waiting for the answer if it has to. A title brackets an object with
     * these and decides from the answer whether to go on drawing it, so a
     * count of zero has to mean hidden. Either may be NULL: every test is
     * then answered "visible". */
    void     (*count_begin)(void);
    uint32_t (*count_end)(void);
    /* The same without the wait, which is how the GPU answers: the count is
     * written into the title's record when it is known, and until then the
     * title sees the test as incomplete. count_end_later() closes the count
     * and returns a ticket for it, or 0 when nothing was counted and the
     * answer is "visible". count_result() says whether a ticket's answer has
     * arrived; when it has, it stores the count and the ticket is spent.
     * `submit` asks the sink to hand the GPU whatever it is still holding
     * back, without which an answer may never come. Either may be NULL, and
     * count_end() is then used. */
    uint32_t (*count_end_later)(void);
    int      (*count_result)(uint32_t ticket, int submit, uint32_t *pixels);
} Nv2aPbSink;

/* Install a sink (or NULL to go back to the software rasteriser). Installing
 * one also turns the executor on; RECOMP_PB_EXEC is not needed. */
void nv2a_pb_exec_set_sink(const Nv2aPbSink *sink);
int  nv2a_pb_exec_has_sink(void);

/* Decode the texture currently bound to stage 0 into width*height ARGB8888
 * texels, top row first. Only valid inside the triangles() callback. Returns
 * 0 if the format cannot be decoded. */
int nv2a_pb_exec_decode_texture(uint32_t *argb_out);

/* What NV097_GET_REPORT should answer just now: the pixels counted since the
 * report value was last cleared. Sets *counted to 0 when nothing can count --
 * there is no sink, or it has no count_begin/count_end -- and the caller
 * should answer "visible" on its own account. */
uint32_t nv2a_pb_exec_report_pixels(int *counted);

/* The same when the sink can answer later. Closes the report: stores the
 * tickets of the counts that make it up, at most `max`, and in *pixels what
 * is already known of the total. Returns how many tickets, or -1 when the
 * sink cannot answer later and nv2a_pb_exec_report_pixels is the one to
 * call. nv2a_pb_exec_ticket_result is the sink's count_result. */
int nv2a_pb_exec_report_tickets(uint32_t *tickets, int max, uint32_t *pixels);
int nv2a_pb_exec_ticket_result(uint32_t ticket, int submit, uint32_t *pixels);
/* The same for mip level `level` of it (0 is the image itself): each level
 * is half the one before in each direction, never less than one texel.
 * Returns 0 for a level the texture does not have. */
int nv2a_pb_exec_decode_texture_level(uint32_t level, uint32_t *argb_out);

/* How long the executor has spent executing the title's commands so far, in
 * QueryPerformanceCounter counts, sink callbacks included; the rest of its
 * time it had nothing submitted to execute. Only meaningful on the executor's
 * own thread, which is where a sink's callbacks run. */
int64_t nv2a_pb_exec_busy_counts(void);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_PB_SINK_H */
