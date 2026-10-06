/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>
#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
extern int nv2a_pb_exec_has_sink(void);
extern uint32_t nv2a_pb_exec_report_pixels(int *counted);
static int s_exec_enabled = -1;

/* Where each (subchannel, method) lives in s_seen: slot + 1, or 0 for "not
 * seen yet". This runs once per command word, and a level sends half a
 * million of those a frame; searching the table for each one was a
 * measurable part of every frame for the sake of a count nobody reads until
 * exit. */
static uint16_t s_seen_slot[8][0x800];

static void note(uint32_t subch, uint32_t method)
{
    uint16_t *slot = &s_seen_slot[subch & 7u][(method >> 2) & 0x7FFu];

    if (*slot) {
        s_seen[*slot - 1].count++;
        return;
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        *slot = (uint16_t)(s_seen_count + 1);
        s_seen_count++;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0)
        nv2a_pb_exec_report();
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u unrecognised"
                    " -- %d distinct (subchannel, method) pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_unknown,
            s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* D3DDevice_InsertCallback.
 *
 * A title can ask to be called back when the GPU reaches a point in the
 * command stream, and D3D builds that out of ordinary methods: it parks the
 * routine and its context in the two clear-value registers, then issues
 * NV097_NO_OPERATION with a non-zero argument. On hardware a non-zero no-op
 * raises a PGRAPH software interrupt, and D3D's handler reads the argument to
 * see what was asked for -- 0x314 a read callback, 0x318 a write callback
 * (issued after a WAIT_FOR_IDLE) -- picks the routine and context back out of
 * those registers, and calls it.
 *
 *     0x00081D8C  routine  context     SET_ZSTENCIL_CLEAR_VALUE, COLOR_CLEAR
 *     0x00040100  0x314                NO_OPERATION, read callback
 *
 * Nothing here raises that interrupt, so the routine was never called. Dave
 * Mirra Freestyle BMX 2 returns each dynamic vertex batch to its free list
 * from exactly such a callback; without it every batch was built fresh --
 * a 56 KB contiguous vertex buffer apiece -- until the 64 MB arena was gone,
 * 868 batches and a few seconds in.
 *
 * The commands are complete by the time they are seen here, so the callback is
 * simply due. It is queued rather than called: this runs on the NV2A poll
 * thread, which has no guest stack, and the kernel bridge delivers it on the
 * title's own thread the way the interrupt would have arrived there.
 */
#define NV097_NO_OPERATION            0x0100u
#define NV097_SET_ZSTENCIL_CLEAR      0x1D8Cu
#define NV097_SET_COLOR_CLEAR         0x1D90u
#define D3D_NOP_READ_CALLBACK         0x314u
#define D3D_NOP_WRITE_CALLBACK        0x318u

extern void xbox_QueueGuestCall(uint32_t routine_va, uint32_t argument);

/* Where a DMA context object points.
 *
 * A method like GET_REPORT names its destination as an offset inside a DMA
 * object, and the object is named by a handle. The handle is looked up in
 * RAMHT, the hash table PFIFO keeps in instance memory; the entry gives the
 * object's own address in instance memory; and the object holds the physical
 * base it describes. D3D writes all three itself, into the register aperture,
 * which is ordinary memory here -- so they can simply be read back.
 *
 * The table is scanned rather than hashed. It is at most 32 KB, this runs once
 * per report, and a scan cannot disagree with the title about the hash.
 *
 * Returns a guest address in the contiguous window, or 0 if the handle is not
 * there or does not describe RAM.
 */
#define NV2A_APERTURE          0xFD000000u
#define NV2A_PFIFO_RAMHT       0x00002210u
#define NV2A_RAMIN             0x00700000u
#define NV2A_RAMHT_VALID       0x80000000u

static uint32_t dma_object_base(uint32_t handle)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *regs = mem + NV2A_APERTURE;
    uint32_t ramht = *(const uint32_t *)(regs + NV2A_PFIFO_RAMHT);
    uint32_t table = ((ramht >> 4) & 0x1Fu) << 12;
    uint32_t bytes = 1u << (((ramht >> 16) & 3u) + 12);
    uint32_t i;

    if (!handle || table + bytes > 0x00100000u)
        return 0;
    for (i = 0; i + 8 <= bytes; i += 8) {
        const uint32_t *e = (const uint32_t *)(regs + NV2A_RAMIN + table + i);
        uint32_t inst, flags, frame, base;

        if (e[0] != handle || !(e[1] & NV2A_RAMHT_VALID))
            continue;
        inst = (e[1] & 0xFFFFu) << 4;
        if (inst + 16 > 0x00100000u)
            return 0;
        flags = *(const uint32_t *)(regs + NV2A_RAMIN + inst);
        frame = *(const uint32_t *)(regs + NV2A_RAMIN + inst + 8);
        /* The page-aligned frame, plus the sub-page adjust kept in the top
         * twelve bits of the first word. */
        base = (frame & 0xFFFFF000u) | (flags >> 20);
        if (base >= XBOX_CONTIG_SIZE)
            return 0;
        return XBOX_CONTIG_BASE | base;
    }
    return 0;
}

/* What the scan writes into a completed report record. */
#define REPORT_STAMP_LO   0x44556677u
#define REPORT_STAMP_HI   0x00112233u
#define REPORT_PIXELS     0x00000400u

/*
 * Is this a report record waiting for its answer?
 *
 * The 0xFFFFFFFF in the last dword is what D3D polls, but on its own it is not
 * a description of anything: it is the most common non-zero dword in a title's
 * memory. The three dwords before it narrow that down to a record -- they are
 * either still zero from the allocation or hold what was written here the last
 * time the record was used.
 */
static int report_pending(const uint32_t *rec)
{
    if (rec[3] != 0xFFFFFFFFu)
        return 0;
    if (!rec[0] && !rec[1] && !rec[2])
        return 1;
    /* The count is whatever was counted, so it is not part of the mark. */
    return rec[0] == REPORT_STAMP_LO && rec[1] == REPORT_STAMP_HI;
}

static void callbacks(uint32_t method, uint32_t param)
{
    static uint32_t routine, context, report_dma;

    switch (method) {
    case NV097_SET_ZSTENCIL_CLEAR: routine = param; break;
    case NV097_SET_COLOR_CLEAR:    context = param; break;
    case NV097_NO_OPERATION:
        if ((param == D3D_NOP_READ_CALLBACK || param == D3D_NOP_WRITE_CALLBACK)
                && routine)
            xbox_QueueGuestCall(routine, context);
        break;

    /* Visibility tests (occlusion queries).
     *
     * D3DDevice_EndVisibilityTest marks a 16-byte record "pending" by setting
     * its last dword to 0xFFFFFFFF and submits GET_REPORT, which has the GPU
     * overwrite the record: a timestamp, the count of pixels that passed the
     * depth test, and zero in that last dword. GetVisibilityTestResult
     * returns D3DERR_TESTINCOMPLETE until the zero appears.
     *
     * Nothing wrote it, and Dave Mirra Freestyle BMX 2 polls for the result
     * without a timeout -- it loaded its first level and then sat in that
     * loop for good, submitting no more frames.
     *
     * ponytail: the count is a constant. Nothing here knows how many pixels
     * passed, because nothing here runs the depth test for 3D geometry; a
     * non-zero count says "visible", which is the answer that keeps whatever
     * was being tested (a lens flare, typically) on screen rather than
     * silently removing it.
     */
    case 0x01A8:                        /* NV097_SET_CONTEXT_DMA_REPORT */
        report_dma = param;
        break;
    case 0x17D0: {                      /* NV097_GET_REPORT */
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        uint32_t offset = param & 0x00FFFFFFu;
        uint32_t base = dma_object_base(report_dma);
        uint32_t rec_va = 0, k;
        static int warned;

        /* The offset carries only the low 24 bits of the record's address,
         * and the DMA object read back from RAMHT says base zero for every
         * report this D3D submits -- including the ones whose records live
         * 16 or 32 MB up. So the object's answer is checked, not trusted, and
         * when it is wrong the record is looked for: sixty-four megabytes is
         * four 16 MB slices, so there are four places it can be.
         *
         * The test for "is this it" has to be a strict one. Matching on the
         * 0xFFFFFFFF alone took the first slice that had one at that offset,
         * and the low slice is where the title's own image is mapped: a
         * record at 0x820FE4A0 lost to four bytes of the game's data at
         * 0x800FE4AC, which were then overwritten with a pixel count while
         * the real record stayed pending and the title waited on it. */
        if (base && offset + 16 <= XBOX_CONTIG_SIZE - (base - XBOX_CONTIG_BASE)
                && report_pending((const uint32_t *)(mem + base + offset)))
            rec_va = base + offset;
        for (k = 0; !rec_va && k < XBOX_CONTIG_SIZE; k += 0x01000000u) {
            uint32_t va = XBOX_CONTIG_BASE + k + offset;
            if (k + offset + 16 <= XBOX_CONTIG_SIZE
                    && report_pending((const uint32_t *)(mem + va)))
                rec_va = va;
        }

        if (rec_va) {
            uint32_t *rec = (uint32_t *)(mem + rec_va);
            rec[0] = REPORT_STAMP_LO;   /* timestamp, low then high */
            rec[1] = REPORT_STAMP_HI;
            {
                /* The real count where something can count it. A renderer
                 * that cannot still answers "visible", which keeps whatever
                 * was being tested on screen rather than removing it. */
                int counted = 0;
                uint32_t pixels = nv2a_pb_exec_report_pixels(&counted);
                rec[2] = counted ? pixels : REPORT_PIXELS;
            }
            rec[3] = 0;                 /* complete */
        } else if (!warned++) {
            fprintf(stderr, "[GPU] GET_REPORT 0x%08X: no pending record found"
                            " (DMA handle 0x%08X); visibility tests will not"
                            " complete\n", param, report_dma);
        }
        break;
    }
    }
}

void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t va = start_va;
    uint32_t words = 0, jumps = 0, unknown = 0;
    uint32_t ret_va = 0;                /* where an open call returns to */

    static int s_survey = -1;

    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    /* A registered renderer is a request to execute, and it can arrive after
     * the first segment has been scanned. */
    if (s_exec_enabled == 0 && nv2a_pb_exec_has_sink())
        s_exec_enabled = 1;
    if (s_survey <= 0)
        s_survey = getenv("RECOMP_PB_SCAN") != NULL || s_exec_enabled;
    /* The walk itself is no longer optional: callbacks() is something the
     * title depends on, not a diagnostic. Only the survey and the executor
     * stay behind their switches. */
    if (end_va == start_va)
        return;

    /* Walk from where the last walk stopped to where the title's writer is
     * now, following the stream rather than the addresses.
     *
     * The pushbuffer is a ring. When the writer nears the end it emits a jump
     * back to the start and carries on from there, so the new PUT is *below*
     * the old one. This used to be handled by not handling it: a range that
     * ran backwards was skipped, and a jump ended the walk. Everything between
     * the old PUT and the jump, and everything after the jump, was never
     * looked at -- once per trip round the buffer, a slice of a frame's
     * commands simply vanished.
     *
     * For a survey that is a rounding error. For commands the title waits on
     * it is a hang: Dave Mirra Freestyle BMX 2 polls each visibility test
     * until its GET_REPORT has been answered, and the first one to fall in a
     * skipped slice stopped the game for good a few seconds into its menu.
     *
     * So: a jump is followed, and the walk ends when it reaches PUT. The
     * bounds below are for a stream that does not behave -- a range that
     * should wrap but shows no jump, or a target outside RAM. */
    while (va != end_va && words < 0x100000u) {
        uint32_t w;

        if (va - XBOX_CONTIG_BASE >= XBOX_CONTIG_SIZE)
            break;                            /* walked out of RAM */
        if (!jumps && end_va > start_va && va > end_va)
            break;                            /* overshot a forward range */
        if (!jumps && end_va < start_va && va - start_va > 0x00400000u)
            break;                            /* should have wrapped by now */

        w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if ((w & 0xE0000003u) == 0x20000000u) {         /* old-style jump */
            jumps++;
            va = XBOX_CONTIG_BASE | (w & 0x0FFFFFFCu);
            continue;
        }
        if ((w & 3u) == 1u) {                            /* jump */
            jumps++;
            va = XBOX_CONTIG_BASE | (w & 0x0FFFFFFCu);
            continue;
        }
        /* Call and return. A title can keep a run of commands in a buffer
         * of its own and have the main stream call it -- D3D's RunPushBuffer
         * is exactly that, and an engine that records each model's draw once
         * and replays it per frame submits most of a scene this way.
         *
         * Both used to be stepped over: the call's target was never walked,
         * so whatever it held was never seen. Dave Mirra Freestyle BMX 2's
         * world geometry is drawn from the main stream and was fine; its
         * rider, bike and every moving object upload their bone matrices
         * from called buffers, never got new ones, and were drawn where the
         * last menu had left them.
         *
         * One level of call, which is all the hardware has: a second call
         * before the return is an error there and is ignored here. */
        if ((w & 0xFFFF0003u) == 0x00020000u) {          /* return */
            if (ret_va) {
                va = ret_va;
                ret_va = 0;
            }
            continue;
        }
        if ((w & 3u) == 2u) {                            /* call */
            if (!ret_va) {
                ret_va = va;
                va = XBOX_CONTIG_BASE | (w & 0x0FFFFFFCu);
                jumps++;
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            for (uint32_t i = 0; i < count && va != end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                callbacks(m, *(const uint32_t *)(mem + va));
                if (!s_survey) {
                    va += 4;
                    words++;
                    continue;
                }
                note(subch, m);
                /* Same walk, two consumers: the survey counts, the executor
                 * acts. Keeping them on one decode means they can never
                 * disagree about what the stream said. */
                if (s_exec_enabled)
                    nv2a_pb_exec_method(subch, m,
                                        *(const uint32_t *)(mem + va));
                va += 4;
                words++;
            }
            continue;
        }
        unknown++;
    }

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}
