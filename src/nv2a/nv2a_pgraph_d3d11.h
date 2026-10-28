/*
 * NV2A PGRAPH → D3D11 Translator
 *
 * Intercepts NV2A push buffer method calls and translates them into
 * D3D8→D3D11 rendering commands. This is the core of the GPU translation
 * layer for Xbox static recompilation.
 *
 * The push buffer contains NV2A Kelvin (NV097) methods:
 *   - Surface/viewport setup → D3D11 render target + viewport
 *   - Render state (blend, depth, cull) → D3D11 state objects
 *   - Begin/End draw + Inline vertex data → D3D11 DrawPrimitiveUP
 *   - Texture binding → D3D11 shader resource views
 *   - Clear commands → D3D11 ClearRenderTargetView
 *
 * Vertex formats observed in menus:
 *   5 dwords per vertex: float X, float Y, float U, float V, D3DCOLOR
 *   Drawn as TRIANGLE_STRIP (mode 6)
 *
 * This module is designed to be reusable across Xbox recompilation projects.
 * See: https://github.com/sp00nznet/xboxrecomp
 */

#ifndef NV2A_PGRAPH_D3D11_H
#define NV2A_PGRAPH_D3D11_H

#include <stdint.h>

/* Initialize the PGRAPH→D3D11 translator. Call after D3D11 device is created. */
void pgraph_d3d11_init(void);

/* Shut down and release resources. */
void pgraph_d3d11_shutdown(void);

/* Process an NV2A PGRAPH method call. Called from push buffer parser.
 * Returns 1 if handled, 0 if unhandled (caller should log/ignore). */
int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param);

/* Flush any pending draw commands (call at end of frame). */
void pgraph_d3d11_flush(void);

/* Set chyron scroll: pass frame counter to animate, 0 to disable.
 * Applies horizontal scroll offset to vertices in the chyron Y band. */
void pgraph_d3d11_set_chyron_scroll(uint32_t frame);

/* Statistics */
typedef struct {
    uint32_t frames;
    uint32_t draw_calls;
    uint32_t vertices_submitted;
    uint32_t methods_handled;
    uint32_t methods_ignored;
    uint32_t clears;
} PgraphD3D11Stats;

void pgraph_d3d11_get_stats(PgraphD3D11Stats *out);

/* The title-independent path: open a window and draw every screen-space batch
 * the pushbuffer executor decodes (src/kernel/nv2a_pb_exec.c) through D3D11.
 * Call once, after the memory layout is up and before the title starts. See
 * nv2a_d3d11_sink.c. */
void nv2a_d3d11_sink_start(const char *window_title);

/* RECOMP_SPIKES=<ms>: a frame over that long is written up as a [SPIKE] line
 * with the slow things that happened in it (see nv2a_d3d11_sink.c). A host
 * that wants to say what else was going on installs a hook, which is called,
 * on the executor's thread, as each one is written: the thread's id, the
 * frame's length in milliseconds, GetTickCount at its end and
 * QueryPerformanceCounter at its start and end. */
typedef void (*Nv2aSinkSpikeHook)(unsigned long executor_thread, double ms,
                                  unsigned long tick, long long qpc_start,
                                  long long qpc_end);
void nv2a_d3d11_sink_set_spike_hook(Nv2aSinkSpikeHook hook);

/* A slow thing in the frame in progress, for the sink to list if the frame
 * turns out a spike. `kind` is one of NV2A_NOTE_*; `ms` how long it took;
 * a, b, c, d say what (a texture: address, width << 16 | height, format,
 * stage; a shader: 1 for a vertex shader, a hash). Nothing is recorded
 * unless RECOMP_SPIKES is set. */
#define NV2A_NOTE_TEXTURE 1
#define NV2A_NOTE_COMPILE 2
#define NV2A_NOTE_WAIT    3
#define NV2A_NOTE_VOICE   4     /* the title waited for DirectSound to stop a voice */
void nv2a_d3d11_sink_note(int kind, double ms, unsigned a, unsigned b,
                          unsigned c, unsigned d);

#endif /* NV2A_PGRAPH_D3D11_H */
