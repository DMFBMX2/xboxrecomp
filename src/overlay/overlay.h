/*
 * Performance overlay.
 *
 * A panel drawn over the picture, just before it is shown, with numbers and
 * graphs that say how the frame rate is holding up. It is for finding the
 * moments a game drops frames while it is being played, and it is built so a
 * project on this toolkit gets it without writing anything: the renderer
 * calls overlay_present once per frame it shows, and the libraries add what
 * they know (the audio's queue, the pushbuffer executor's busy time).
 *
 * RECOMP_OVERLAY=1 makes the overlay exist, and shows it. F10 hides it and
 * shows it again while the game is in front. Without the variable nothing is
 * sampled, nothing is drawn and F10 is the game's.
 *
 * What it shows is a list of metrics. FPS and frame time are the overlay's
 * own; anything else is added with overlay_add_metric, from any thread, at
 * any time. Adding a metric is all it takes: the layout and the drawing do
 * not change.
 *
 * Only Windows draws (Direct3D 11). Elsewhere the calls are empty.
 */
#ifndef XBOXRECOMP_OVERLAY_H
#define XBOXRECOMP_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* One row of the panel. */
typedef struct {
    const char *name;               /* the row's label; at most 11 characters */
    const char *unit;               /* after the value: "ms", or "" */
    /* The value now. Called once for each frame shown, from the thread that
     * shows it, and must be quick. */
    double    (*read)(void *user);
    void       *user;
    int         decimals;           /* digits after the point */

    /* A graph of the last frames' values under the row, when graph_max is
     * above zero: bars rising to graph_max, and as far as they go. */
    double      graph_max;
    /* Where a value starts to be a problem, or 0: a graph draws a line there
     * and colours the bars over it, and the row's number goes red when its
     * highest value in the last moment is over it. */
    double      budget;
    /* For a row with no budget: a value over `warn` turns it red. A warn
     * below zero is red whenever the value is above zero, for a count of
     * things going wrong. 0 is never red. */
    double      warn;
} OverlayMetric;

#ifdef _WIN32

/* Whether RECOMP_OVERLAY is set. The first call reads it. */
int  overlay_enabled(void);

/* Adds a row, last in the panel. Returns 0 when there is no room or the
 * overlay is not enabled; the metric is copied, the strings are not. */
int  overlay_add_metric(const OverlayMetric *metric);

/* F10: shows the panel if it is hidden and hides it if it is shown. */
void overlay_toggle(void);

/* Called as a frame is about to be shown: counts it, reads every metric,
 * and, when the panel is showing, draws it on the render target that is
 * bound to `d3d11_context` (an ID3D11Device * and an ID3D11DeviceContext *).
 * The state it changes is put back. */
void overlay_present(void *d3d11_device, void *d3d11_context);

#else

static inline int  overlay_enabled(void) { return 0; }
static inline int  overlay_add_metric(const OverlayMetric *metric)
{
    (void)metric;
    return 0;
}
static inline void overlay_toggle(void) {}
static inline void overlay_present(void *d, void *c) { (void)d; (void)c; }

#endif

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_OVERLAY_H */
