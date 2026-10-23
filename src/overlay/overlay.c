/*
 * Performance overlay: the metrics, the numbers kept about them, and the
 * panel's layout. See overlay.h. The drawing is in overlay_d3d11.c.
 */
#ifdef _WIN32

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "overlay.h"
#include "overlay_gfx.h"

#define OVL_MAX_METRICS 16
#define OVL_HISTORY     240     /* frames a graph holds: four seconds at 60 */
#define OVL_WINDOW_MS   250     /* how often the numbers are brought up to date */
#define OVL_LABEL_CHARS 9
#define OVL_TEXT_CHARS  18      /* "16.7 ms  max 21.4" */

typedef struct {
    OverlayMetric m;
    float         history[OVL_HISTORY];
    double        last;                 /* the latest value */
    /* Over the current window, and what the last one came to. */
    double        sum, max;
    unsigned      n;
    double        shown, shown_max;
} Row;

static struct {
    INIT_ONCE        once;
    CRITICAL_SECTION lock;
    int              enabled;
    volatile LONG    visible;
    Row              rows[OVL_MAX_METRICS];
    int              count;
    unsigned         head, filled;      /* the history's ring */
    LONGLONG         last_qpc, window_qpc, freq;
    unsigned         window_frames;
    double           frame_ms, fps;     /* now, for the first two rows */
    double           cost_ms;           /* drawing the panel, last frame */
} g;

static BOOL CALLBACK overlay_once(PINIT_ONCE once, PVOID param, PVOID *ctx);
static double read_fps(void *user)   { (void)user; return g.fps; }
static double read_frame(void *user) { (void)user; return g.frame_ms; }
static double read_cost(void *user)  { (void)user; return g.cost_ms; }

/* RECOMP_OVERLAY=1 (any value but 0) turns it on. The panel is showing
 * from the start; F10 hides it. */
static BOOL CALLBACK overlay_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    const char *e = getenv("RECOMP_OVERLAY");
    LARGE_INTEGER f;
    OverlayMetric fps = { "FPS", "", read_fps, NULL, 0, 0, 0, 0 };
    /* A frame is late when it misses a refresh: 33 ms on a 60 Hz display.
     * The line is a little over one refresh, not on it, because the frames
     * that make it still differ by a millisecond either way. */
    OverlayMetric frame = { "FRAME", "ms", read_frame, NULL, 1, 50.0,
                            1000.0 / 60.0 * 1.1, 0 };

    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g.lock);
    g.enabled = e && e[0] && strcmp(e, "0") != 0;
    if (!g.enabled)
        return TRUE;
    g.visible = 1;
    QueryPerformanceFrequency(&f);
    g.freq = f.QuadPart;
    g.rows[g.count++].m = fps;
    g.rows[g.count++].m = frame;
    /* What the panel itself costs, on the processor, so that watching a
     * frame rate does not need to be taken on trust. */
    g.rows[g.count++].m = (OverlayMetric){ "OVERLAY", "ms", read_cost, NULL,
                                           2, 0, 0, 0 };
    return TRUE;
}

int overlay_enabled(void)
{
    InitOnceExecuteOnce(&g.once, overlay_once, NULL, NULL);
    return g.enabled;
}

int overlay_add_metric(const OverlayMetric *metric)
{
    int ok = 0;

    if (!overlay_enabled() || !metric || !metric->read || !metric->name)
        return 0;
    EnterCriticalSection(&g.lock);
    if (g.count < OVL_MAX_METRICS) {
        Row *r = &g.rows[g.count];

        memset(r, 0, sizeof *r);
        r->m = *metric;
        g.count++;
        ok = 1;
    }
    LeaveCriticalSection(&g.lock);
    return ok;
}

void overlay_toggle(void)
{
    if (overlay_enabled())
        InterlockedExchange(&g.visible, !g.visible);
}

/* A frame has been shown: its time, and every metric's value for it. */
static void overlay_sample(LONGLONG now)
{
    int i;

    if (g.last_qpc) {
        g.frame_ms = (double)(now - g.last_qpc) * 1000.0 / (double)g.freq;
        g.fps = g.frame_ms > 0.0 ? 1000.0 / g.frame_ms : 0.0;
        g.window_frames++;
        for (i = 0; i < g.count; i++) {
            Row *r = &g.rows[i];
            double v = r->m.read(r->m.user);

            r->last = v;
            r->sum += v;
            r->n++;
            if (v > r->max)
                r->max = v;
            r->history[g.head] = (float)v;
        }
        g.head = (g.head + 1) % OVL_HISTORY;
        if (g.filled < OVL_HISTORY)
            g.filled++;
    }
    g.last_qpc = now;

    if (!g.window_qpc)
        g.window_qpc = now;
    if ((double)(now - g.window_qpc) * 1000.0 / (double)g.freq
            >= OVL_WINDOW_MS) {
        double secs = (double)(now - g.window_qpc) / (double)g.freq;

        for (i = 0; i < g.count; i++) {
            Row *r = &g.rows[i];

            r->shown = r->n ? r->sum / r->n : 0.0;
            r->shown_max = r->max;
            r->sum = r->max = 0.0;
            r->n = 0;
        }
        /* Frames over time rather than the mean of each one's rate. */
        g.rows[0].shown = secs > 0.0 ? g.window_frames / secs : 0.0;
        g.window_frames = 0;
        g.window_qpc = now;
    }
}

static int row_is_red(const Row *r, double v)
{
    if (r->m.budget > 0.0 && v > r->m.budget)
        return 1;
    if (r->m.warn > 0.0 && v > r->m.warn)
        return 1;
    return r->m.warn < 0.0 && v > 0.0;
}

static uint32_t bar_colour(const Row *r, double v)
{
    if (r->m.budget <= 0.0)
        return OVL_RGBA(100, 180, 240, 255);
    if (v <= r->m.budget)
        return OVL_RGBA(60, 210, 110, 255);
    if (v <= r->m.budget * 1.5)
        return OVL_RGBA(240, 190, 50, 255);
    return OVL_RGBA(240, 75, 65, 255);
}

static void draw_graph(const Row *r, float x, float y, float w, float h,
                       float bar_w)
{
    unsigned i;
    double max = r->m.graph_max;

    ovl_gfx_rect(x, y, w, h, OVL_RGBA(255, 255, 255, 28));
    for (i = 0; i < g.filled; i++) {
        double v = r->history[(g.head + OVL_HISTORY - g.filled + i)
                              % OVL_HISTORY];
        float bh = (float)(v >= max ? h : v / max * h);

        if (v > 0.0 && bh < 1.0f)
            bh = 1.0f;
        if (bh > 0.0f)
            ovl_gfx_rect(x + (float)(OVL_HISTORY - g.filled + i) * bar_w,
                         y + h - bh, bar_w, bh, bar_colour(r, v));
    }
    if (r->m.budget > 0.0 && r->m.budget < max)
        ovl_gfx_rect(x, y + h - (float)(r->m.budget / max * h), w, 1.0f,
                     OVL_RGBA(255, 255, 255, 210));
}

static void draw_panel(unsigned cw, unsigned ch)
{
    float bar_w = (float)((OVL_LABEL_CHARS + 1 + OVL_TEXT_CHARS) * cw
                          / OVL_HISTORY);
    float graph_w, graph_h = (float)ch * 2.5f, pitch = (float)ch * 1.25f;
    float pad = (float)cw, x0 = (float)ch, y0 = (float)ch, w, h, y;
    int i, k, order[OVL_MAX_METRICS], rows = 0;

    if (bar_w < 1.0f)
        bar_w = 1.0f;
    graph_w = bar_w * OVL_HISTORY;
    w = graph_w;
    if (w < (float)((OVL_LABEL_CHARS + 1 + OVL_TEXT_CHARS) * cw))
        w = (float)((OVL_LABEL_CHARS + 1 + OVL_TEXT_CHARS) * cw);
    h = 0.0f;
    for (i = 0; i < g.count; i++)
        h += pitch + (g.rows[i].m.graph_max > 0.0 ? graph_h + pitch * 0.25f
                                                   : 0.0f);
    ovl_gfx_rect(x0, y0, w + 2 * pad, h + 2 * pad - pitch * 0.25f,
                 OVL_RGBA(10, 12, 16, 190));

    /* FPS first, then the rows with graphs, then the rest. */
    order[rows++] = 0;
    for (i = 1; i < g.count; i++)
        if (g.rows[i].m.graph_max > 0.0)
            order[rows++] = i;
    for (i = 1; i < g.count; i++)
        if (g.rows[i].m.graph_max <= 0.0)
            order[rows++] = i;

    y = y0 + pad;
    for (k = 0; k < rows; k++) {
        const Row *r = &g.rows[i = order[k]];
        int graph = r->m.graph_max > 0.0;
        double v = graph || i == 0 ? r->shown : r->last;
        char text[64];
        int n;

        ovl_gfx_text(x0 + pad, y, r->m.name, OVL_RGBA(170, 180, 195, 255));
        n = snprintf(text, sizeof text, "%.*f%s%s", r->m.decimals, v,
                     r->m.unit[0] ? " " : "", r->m.unit);
        if (graph && n > 0 && (size_t)n < sizeof text)
            snprintf(text + n, sizeof text - n, "  max %.*f", r->m.decimals,
                     r->shown_max);
        ovl_gfx_text(x0 + pad + (float)((OVL_LABEL_CHARS + 1) * cw), y, text,
                     row_is_red(r, graph ? r->shown_max : v)
                         ? OVL_RGBA(255, 110, 100, 255)
                         : OVL_RGBA(240, 244, 250, 255));
        y += pitch;
        if (graph) {
            draw_graph(r, x0 + pad, y, graph_w, graph_h, bar_w);
            y += graph_h + pitch * 0.25f;
        }
    }
}

void overlay_present(void *d3d11_device, void *d3d11_context)
{
    LARGE_INTEGER now, done;
    unsigned w, h, cw, ch;

    if (!overlay_enabled())
        return;
    QueryPerformanceCounter(&now);
    EnterCriticalSection(&g.lock);
    overlay_sample(now.QuadPart);
    if (g.visible && ovl_gfx_begin(d3d11_device, d3d11_context, &w, &h, &cw,
                                   &ch)) {
        draw_panel(cw, ch);
        ovl_gfx_end();
        QueryPerformanceCounter(&done);
        g.cost_ms = (double)(done.QuadPart - now.QuadPart) * 1000.0
                    / (double)g.freq;
    }
    LeaveCriticalSection(&g.lock);
}

#endif /* _WIN32 */
