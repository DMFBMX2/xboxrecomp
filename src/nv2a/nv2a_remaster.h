/*
 * Replacement ("HD") textures.
 *
 * A texture the title has in guest memory is known by a hash of the bytes of
 * its base level. If a file named for that hash, `<dir>/textures/<hash>.dds`,
 * is a BC7 image with its mip chain, it is used in place of the title's
 * picture, at its own size: the coordinates a title draws with are fractions
 * of the picture, so a bigger picture needs nothing else changed.
 *
 * The files are made by the game's own tool from the player's own copy of
 * the game (tools/remaster.py in the game repository), which keys them with
 * the same hash (nv2a_remaster_key must equal sink_hash in
 * nv2a_d3d11_sink.c and texture_hash in tools/txr.py).
 *
 * Off unless RECOMP_REMASTER_DIR names a folder, and off whatever it names
 * if RECOMP_REMASTER is 0. RECOMP_REMASTER_LOG=1 says what is replaced.
 */
#ifndef NV2A_REMASTER_H
#define NV2A_REMASTER_H

#include <stddef.h>
#include <stdint.h>

/* DXGI_FORMAT_BC7_UNORM. */
#define NV2A_REMASTER_DXGI_BC7 98u

typedef struct {
    uint32_t width, height, levels;
    uint8_t *data;              /* every level, one after another */
    size_t   bytes;
} Nv2aRemasterImage;

/* Whether replacement is on. */
int nv2a_remaster_enabled(void);

/* The hash a texture is known by: its base level's bytes. */
uint64_t nv2a_remaster_key(const uint8_t *base, size_t bytes);

/* The replacement for the texture of `bytes` bytes at `offset` in guest
 * memory in the title's `format`, if there is one: 1 and the image, which the
 * caller frees with nv2a_remaster_free; 0 if not (the format is not one that
 * has replacements, there is no file, or it is not a usable image). */
int nv2a_remaster_for(const uint8_t *mem, uint32_t offset, size_t bytes,
                      uint32_t format, Nv2aRemasterImage *out);

/* Where level `level` is in the image, and its row length and row count. */
void nv2a_remaster_level(const Nv2aRemasterImage *img, uint32_t level,
                         const uint8_t **blocks, uint32_t *row_bytes,
                         uint32_t *rows);

void nv2a_remaster_free(Nv2aRemasterImage *img);

#endif /* NV2A_REMASTER_H */
