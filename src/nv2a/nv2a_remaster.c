/*
 * Replacement ("HD") textures: finding and reading them. See the header.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "nv2a_remaster.h"

/* The title's texture formats that have replacements: DXT1, DXT3 and DXT5,
 * and 8-bit indices into a palette. */
#define FORMAT_DXT1 0x0Cu
#define FORMAT_DXT3 0x0Eu
#define FORMAT_DXT5 0x0Fu
#define FORMAT_P8   NV2A_REMASTER_FORMAT_P8

/* What a palette's hash is multiplied by before it is combined with the
 * indices'. */
#define PALETTE_KEY_MUL 0x9E3779B97F4A7C15ull

/* A DDS file with a DX10 header: "DDS ", the 124-byte header, then 20 bytes
 * naming the DXGI format, then the data. */
#define DDS_MAGIC          0x20534444u      /* "DDS " */
#define DDS_FOURCC_DX10    0x30315844u      /* "DX10" */
#define DDS_HEADER_SIZE    124u
#define DDS_DATA_AT        148u
#define DDS_HEIGHT_AT      12u
#define DDS_WIDTH_AT       16u
#define DDS_MIPS_AT        28u
#define DDS_FOURCC_AT      84u
#define DDS_DXGI_AT        128u

#define BC7_BLOCK_BYTES    16u
#define MAX_LEVELS         14u

static const char *remaster_dir(void)
{
    static int looked;
    static const char *dir;

    if (!looked) {
        const char *d = getenv("RECOMP_REMASTER_DIR");
        const char *off = getenv("RECOMP_REMASTER");

        looked = 1;
        dir = (d && d[0] && !(off && off[0] == '0')) ? d : NULL;
    }
    return dir;
}

int nv2a_remaster_enabled(void)
{
    return remaster_dir() != NULL;
}

static int remaster_log(void)
{
    static int log = -1;

    if (log < 0)
        log = getenv("RECOMP_REMASTER_LOG") != NULL;
    return log;
}

/* FNV-1a over 64-bit words, four lanes while there are four words left: sink_hash
 * in nv2a_d3d11_sink.c, whose offset basis is the one here (one digit short of
 * FNV's own). The files are named by it, so it must not change. */
uint64_t nv2a_remaster_key(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i = 0, words = n / 8;

    if (words >= 4) {
        uint64_t a = h, b = h + 1, c = h + 2, d = h + 3;

        for (; i + 4 <= words; i += 4) {
            uint64_t w[4];

            memcpy(w, p + i * 8, 32);
            a = (a ^ w[0]) * 1099511628211ull;
            b = (b ^ w[1]) * 1099511628211ull;
            c = (c ^ w[2]) * 1099511628211ull;
            d = (d ^ w[3]) * 1099511628211ull;
        }
        h = (a ^ b) * 1099511628211ull;
        h = (h ^ c) * 1099511628211ull;
        h = (h ^ d) * 1099511628211ull;
    }
    for (; i < words; i++) {
        uint64_t w;

        memcpy(&w, p + i * 8, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    for (i = words * 8; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

uint64_t nv2a_remaster_key_paletted(const uint8_t *base, size_t bytes,
                                    const uint8_t *palette)
{
    return nv2a_remaster_key(base, bytes)
         ^ nv2a_remaster_key(palette, NV2A_REMASTER_PALETTE_BYTES)
           * PALETTE_KEY_MUL;
}

static uint32_t level_size(uint32_t size, uint32_t level)
{
    return (size >> level) ? (size >> level) : 1;
}

static uint32_t level_row_bytes(uint32_t width, uint32_t level)
{
    return ((level_size(width, level) + 3) / 4) * BC7_BLOCK_BYTES;
}

static uint32_t level_rows(uint32_t height, uint32_t level)
{
    return (level_size(height, level) + 3) / 4;
}

void nv2a_remaster_level(const Nv2aRemasterImage *img, uint32_t level,
                         const uint8_t **blocks, uint32_t *row_bytes,
                         uint32_t *rows)
{
    size_t at = 0;
    uint32_t l;

    for (l = 0; l < level; l++)
        at += (size_t)level_row_bytes(img->width, l)
            * level_rows(img->height, l);
    *blocks = img->data + at;
    *row_bytes = level_row_bytes(img->width, level);
    *rows = level_rows(img->height, level);
}

void nv2a_remaster_free(Nv2aRemasterImage *img)
{
    free(img->data);
    memset(img, 0, sizeof *img);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
         | (uint32_t)p[3] << 24;
}

/* Reads `path` as a BC7 DDS. 0 if it is not there; 0 and a message if it is
 * there and unusable. */
static int read_image(const char *path, Nv2aRemasterImage *out)
{
    FILE *f = fopen(path, "rb");
    uint8_t head[DDS_DATA_AT];
    long file_bytes;
    size_t want = 0;
    uint32_t l;

    if (!f)
        return 0;
    if (fread(head, 1, sizeof head, f) != sizeof head
            || le32(head) != DDS_MAGIC || le32(head + 4) != DDS_HEADER_SIZE
            || le32(head + DDS_FOURCC_AT) != DDS_FOURCC_DX10
            || le32(head + DDS_DXGI_AT) != NV2A_REMASTER_DXGI_BC7) {
        fprintf(stderr, "[REMASTER] %s is not a BC7 DDS file\n", path);
        fclose(f);
        return 0;
    }
    out->height = le32(head + DDS_HEIGHT_AT);
    out->width = le32(head + DDS_WIDTH_AT);
    out->levels = le32(head + DDS_MIPS_AT);
    if (!out->levels)
        out->levels = 1;
    if (!out->width || !out->height || out->width > 8192 || out->height > 8192
            || out->levels > MAX_LEVELS) {
        fprintf(stderr, "[REMASTER] %s has a size that is not usable\n", path);
        fclose(f);
        return 0;
    }
    for (l = 0; l < out->levels; l++)
        want += (size_t)level_row_bytes(out->width, l)
              * level_rows(out->height, l);
    fseek(f, 0, SEEK_END);
    file_bytes = ftell(f);
    if (file_bytes < 0 || (size_t)file_bytes < DDS_DATA_AT + want) {
        fprintf(stderr, "[REMASTER] %s is shorter than its header says\n",
                path);
        fclose(f);
        return 0;
    }
    out->data = (uint8_t *)malloc(want);
    if (!out->data) {
        fclose(f);
        return 0;
    }
    fseek(f, DDS_DATA_AT, SEEK_SET);
    if (fread(out->data, 1, want, f) != want) {
        free(out->data);
        memset(out, 0, sizeof *out);
        fclose(f);
        return 0;
    }
    out->bytes = want;
    fclose(f);
    return 1;
}

int nv2a_remaster_for(const uint8_t *mem, uint32_t offset, size_t bytes,
                      uint32_t format, uint32_t palette,
                      Nv2aRemasterImage *out)
{
    static unsigned loaded;
    static double load_ms, loaded_mb;
    const char *dir = remaster_dir();
    char path[600];
    uint64_t key;
    double began = 0.0;

    memset(out, 0, sizeof *out);
    if (!dir || !bytes
            || (format != FORMAT_DXT1 && format != FORMAT_DXT3
                && format != FORMAT_DXT5
                && !(format == FORMAT_P8 && palette)))
        return 0;
    key = format == FORMAT_P8
        ? nv2a_remaster_key_paletted(mem + offset, bytes, mem + palette)
        : nv2a_remaster_key(mem + offset, bytes);
    snprintf(path, sizeof path, "%s/textures/%016llx.dds", dir,
             (unsigned long long)key);
#ifdef _WIN32
    if (remaster_log()) {
        LARGE_INTEGER t, fq;

        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&fq);
        began = (double)t.QuadPart * 1000.0 / (double)fq.QuadPart;
    }
#endif
    if (!read_image(path, out)) {
        if (remaster_log())
            fprintf(stderr, "[REMASTER] %016llx not replaced: %u bytes, format 0x%x\n",
                    (unsigned long long)key, (unsigned)bytes, (unsigned)format);
        return 0;
    }
    if (remaster_log()) {
#ifdef _WIN32
        LARGE_INTEGER t, fq;

        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&fq);
        load_ms += (double)t.QuadPart * 1000.0 / (double)fq.QuadPart - began;
#endif
        loaded++;
        loaded_mb += (double)out->bytes / 1e6;
        if (loaded <= 5 || loaded % 100 == 0)
            fprintf(stderr, "[REMASTER] %016llx replaced: %ux%u levels %u"
                            " (%u so far, %.1f MB read, %.2f ms a file)\n",
                    (unsigned long long)key, out->width, out->height,
                    out->levels, loaded, loaded_mb, load_ms / loaded);
    }
    return 1;
}
