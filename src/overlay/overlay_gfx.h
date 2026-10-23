/*
 * What the overlay draws with. overlay.c decides what is on the panel and
 * where; this is the part that puts rectangles and text on the render target.
 * Colours are 0xAABBGGRR, which is R8G8B8A8_UNORM in memory.
 */
#ifndef XBOXRECOMP_OVERLAY_GFX_H
#define XBOXRECOMP_OVERLAY_GFX_H

#include <stdint.h>

#define OVL_RGBA(r, g, b, a) \
    ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(a) << 24))

/* Starts a frame's drawing on the render target now bound to the context.
 * Gives its size in pixels and the size of one character at the size this
 * target wants text drawn (the font is a monospace one). Returns 0 when
 * nothing can be drawn. Saves what it is about to change. */
int  ovl_gfx_begin(void *d3d11_device, void *d3d11_context,
                   unsigned *width, unsigned *height,
                   unsigned *char_w, unsigned *char_h);

/* A filled rectangle, in pixels from the top left. */
void ovl_gfx_rect(float x, float y, float w, float h, uint32_t rgba);

/* Text, ASCII, its top left corner at (x, y). */
void ovl_gfx_text(float x, float y, const char *text, uint32_t rgba);

/* Draws what was asked for and puts back what begin saved. */
void ovl_gfx_end(void);

#endif /* XBOXRECOMP_OVERLAY_GFX_H */
