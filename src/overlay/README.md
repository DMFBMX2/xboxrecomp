# xbox_overlay: performance overlay

A panel drawn over the picture just before it is shown, with numbers and
graphs that say how the frame rate is holding up. It is for catching the
moments a game drops frames while it is being played.

```
set RECOMP_OVERLAY=1
```

With the variable set the panel is showing from the start and **F10** hides it
and shows it again (the game's window must be in front). Without the variable
the overlay does not exist: nothing is sampled or drawn, and F10 is the
game's.

What it shows:

| Row       | Meaning |
|-----------|---------|
| `FPS`     | Frames shown per second. |
| `FRAME`   | Time between frames shown, with a graph of the last 240 and a line a little over one refresh (16.7 ms). A bar over the line is amber; over 1.5 times it, red: a missed refresh is 33 ms. |
| `BUSY`    | How long the pushbuffer executor, which decodes the game's commands and draws them, worked for the frame. Close to `FRAME` means the renderer is the limit. |
| `OVERLAY` | What drawing the panel costs on the processor. |
| `VOICES`  | Voices the audio processor played in its last frame. |
| `QUEUE`   | Buffers waiting in the audio output queue. At 0 it is about to click. |
| `UNDERRUNS`, `DROPPED` | Times the queue ran dry, and submissions thrown away for it being full. Red once above zero. |

The panel is drawn after a screenshot (F9 in the game) is taken, so a
screenshot is of the game.

## Adding a metric

Any library, or the game, adds a row from any thread, at any time:

```c
#include "overlay.h"

static double my_value(void *user) { return queue_depth; }

OverlayMetric m = { "MYTHING", "ms", my_value, NULL,
                    1,          /* digits after the point */
                    50.0,       /* graph up to this; 0 for a row without one */
                    16.7,       /* budget: the line, and red above it; 0 none */
                    0 };        /* warn, for a row without a budget (-1: red above 0) */
overlay_add_metric(&m);
```

`read` is called once for each frame shown, on the thread that shows it, so it
must be quick. Rows with graphs come first, then the others, each in the order
they were added. Nothing else changes: the layout and the drawing take the
list as it is.

## For the renderer

Call `overlay_present(d3d11_device, d3d11_context)` once for each frame, after
the frame is drawn and before `Present`. It draws on the render target that is
bound, and puts back the Direct3D 11 state it changes. The NV2A sink
(`src/nv2a/nv2a_d3d11_sink.c`) does this, and calls `overlay_toggle` for F10
in its window procedure. Its text is a glyph atlas made with GDI from
Consolas, or Courier New where that is missing, sized to the height of the
render target.

Only Windows draws. Elsewhere `overlay.h` has empty functions, so a caller
needs no `#ifdef`.
