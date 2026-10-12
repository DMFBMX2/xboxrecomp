/*
 * Drawing a shaded batch with D3D11: the part of the sink that does not go
 * through the D3D8 layer's fixed pipeline. See nv2a_d3d11_shade.c.
 */
#ifndef XBOXRECOMP_NV2A_D3D11_SHADE_H
#define XBOXRECOMP_NV2A_D3D11_SHADE_H

#include <stdint.h>

#include "../kernel/nv2a_pb_sink.h"

/* A vertex as the shaded path's vertex shader takes it: the position in the
 * back buffer's pixels with depth and 1/w, as the sink's other vertices
 * are, the two colours as D3DCOLORs, and the four sets of coordinates. */
typedef struct {
    float    x, y, z, rhw;
    uint32_t diffuse, specular;
    float    tex[4][4];
    float    fog;
} Nv2aShadeVertex;

/* Draws `count` vertices as triangles with the pixel pipeline `shade`
 * describes. The blend, depth, stencil and cull state must already have
 * been given to the D3D8 device; the alpha test is taken from `state`.
 * `frame` is the sink's frame number, by which a texture is checked for
 * changes once a frame. Returns 0 if nothing could be drawn. */
int nv2a_d3d11_shade_draw(const Nv2aShadeVertex *v, uint32_t count,
                          const Nv2aSinkShade *shade,
                          const Nv2aSinkState *state, uint32_t frame);

#endif
