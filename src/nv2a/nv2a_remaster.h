/*
 * Replacement ("HD") textures.
 *
 * A texture the title has in guest memory is known by a hash of the bytes of
 * its base level. If a file named for that hash, `<dir>/textures/<hash>.dds`,
 * is a BC7 image with its mip chain, it is used in place of the title's
 * picture, at its own size: the coordinates a title draws with are fractions
 * of the picture, so a bigger picture needs nothing else changed.
 *
 * A palettised texture (8 bits an index, with a 256-entry palette) is known
 * by the hash of its indices combined with the hash of its palette: the
 * title recolours such art by rewriting the palette and leaving the indices
 * alone, so one set of indices can be several pictures, and each has its own
 * file. The file is the picture in full colour, with its alpha: BC7 has no
 * palette.
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

/* The title's format for 8-bit indices into a palette of 256 32-bit entries
 * (SZ_I8_A8R8G8B8), and the size of that palette. */
#define NV2A_REMASTER_FORMAT_P8   0x0Bu
#define NV2A_REMASTER_PALETTE_BYTES 1024u

/* The hash a palettised texture is known by: the hash of its indices, xor
 * the hash of its palette times a constant (the same as sink_texture in
 * nv2a_d3d11_sink.c, and paletted_key in tools/uilib.py). */
uint64_t nv2a_remaster_key_paletted(const uint8_t *base, size_t bytes,
                                    const uint8_t *palette);

/* The replacement for the texture of `bytes` bytes at `offset` in guest
 * memory in the title's `format`, if there is one: 1 and the image, which the
 * caller frees with nv2a_remaster_free; 0 if not (the format is not one that
 * has replacements, there is no file, or it is not a usable image). For a
 * palettised texture `palette` is where its palette is in guest memory (0 if
 * it has none, and then there is no replacement); it is ignored for the
 * others. */
int nv2a_remaster_for(const uint8_t *mem, uint32_t offset, size_t bytes,
                      uint32_t format, uint32_t palette,
                      Nv2aRemasterImage *out);

/* Where level `level` is in the image, and its row length and row count. */
void nv2a_remaster_level(const Nv2aRemasterImage *img, uint32_t level,
                         const uint8_t **blocks, uint32_t *row_bytes,
                         uint32_t *rows);

void nv2a_remaster_free(Nv2aRemasterImage *img);

#endif /* NV2A_REMASTER_H */
