/*
 * NV2A pushbuffer -> D3D11, for any title.
 *
 * nv2a_pgraph_d3d11.c translates one title's menu stream: five-dword inline
 * vertices and textures looked up by the addresses that title happened to
 * load them at. This is the same idea with the title-specific parts removed.
 *
 * The decode is not repeated here. src/kernel/nv2a_pb_exec.c already walks the
 * command stream and knows which vertex streams and which texture a batch
 * uses; it hands each screen-space batch to a registered sink (see
 * nv2a_pb_sink.h), and this file is that sink. What it adds is the part the
 * software rasteriser could not do at any useful speed: a window, textures
 * built from guest memory, blending, and a present.
 *
 * Drawing goes through the D3D8-on-D3D11 layer in src/d3d rather than D3D11
 * directly, so the state objects, shaders and vertex formats are the ones that
 * layer already has.
 *
 * Threads: the window belongs to its own thread, which does nothing but pump
 * messages -- the title's thread never returns to a message loop, and a window
 * nobody pumps is reported as not responding. Every D3D call is made from the
 * NV2A poll thread, which is where the sink callbacks arrive.
 *
 * ponytail: screen-space batches only, texture stage 0 only, and the stage's
 * combiner is assumed to be texture x diffuse. Batches that need a vertex
 * program are still skipped upstream and counted there.
 */
#ifdef _WIN32

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define COBJMACROS
#include <d3d11.h>

#include "../d3d/d3d8_xbox.h"
#include "../kernel/nv2a_pb_sink.h"

/* From the D3D8 layer (d3d8_internal.h), for reading the frame back. */
ID3D11Device        *d3d8_GetD3D11Device(void);
ID3D11DeviceContext *d3d8_GetD3D11Context(void);

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* A menu binds a dozen textures; a level binds several hundred a frame. With
 * fewer slots than that every frame evicts what the next one needs, and each
 * miss is a decode and an upload -- four hundred a second, at 96. */
#define SINK_MAX_TEXTURES 1024
#define SINK_TEX_BUCKETS  4096

/* A texture built from guest memory. Identity is where it lives and what
 * shape it is; `hash` says whether the guest has rewritten it since. */
typedef struct {
    uint32_t offset, width, height, format, palette, levels;
    uint64_t hash;
    uint32_t checked_frame;         /* hashed at most once per frame */
    uint32_t used_frame;
    /* Set once a batch has drawn this texture any way but one texel to a
     * title pixel. See sink_unit_sprites. */
    int      scaled;
    IDirect3DTexture8 *tex;
} SinkTexture;

static struct {
    char   title[128];
    HANDLE window_ready;
    HWND   hwnd;
    int    failed;
    IDirect3DDevice8 *dev;
    uint32_t width, height;         /* the title's surface, e.g. 640x480 */
    /* The picture on the host: the back buffer, and where the title's
     * surface sits in it. See sink_configure_output. */
    uint32_t out_w, out_h;
    float    sx, sy, ox, oy;        /* host = o + title * s */
    float    vis_x0, vis_y0, vis_x1, vis_y1;    /* inside this, picture   */
    int      borderless;
    uint32_t frame;
    int      drew_this_frame;
    DWORD    last_present_ms;
    SinkTexture tex[SINK_MAX_TEXTURES];
    /* Where each address was last found: slot + 1, or 0. A hint, checked
     * before the scan, so the common case is one comparison rather than a
     * walk of the table per batch. */
    uint16_t  tex_hint[SINK_TEX_BUCKETS];
    SinkTexture *bound;             /* what sink_texture last returned */
    uint32_t *scratch;
    size_t    scratch_texels;
    uint32_t  uploads, draws;
} g_sink;

/* ── Output size and shape ─────────────────────────────────── */

/* The visibility test in progress; see sink_count_begin. */
#define SINK_COUNT_QUERIES 512
static struct {
    /* A query per test whose answer is still to come; see
     * sink_count_end_later. */
    struct {
        ID3D11Query *query;
        int          pending;       /* closed, and its answer not read */
        int          past_edge;
        DWORD        closed_ms;
    } q[SINK_COUNT_QUERIES];
    int          open;              /* slot + 1 of the one counting, or 0 */
    unsigned     next;              /* where to look for a free slot */
    /* Something drawn during the test reached an edge of the title's
     * surface that the picture carries on past. */
    int          past_edge;
    uint32_t     tests, timeouts;
} g_count;

/*
 * RECOMP_RES=<width>x<height>   back buffer and window, default 1920x1080.
 * RECOMP_ASPECT=<a>:<b>         shape of the picture, default 16:9.
 *              =stretch         fill the window whatever its shape.
 *
 * The title draws a 4:3 frame in its own pixels. That frame is scaled to the
 * height of the picture and centred, with square pixels, so nothing is made
 * fatter. A picture wider than 4:3 is filled by what the title drew beyond
 * the edges of its frame: a 3D scene is submitted wider than the screen it
 * expected, since it is only culled object by object, and that is what
 * shows. Menus and the HUD are authored for the 4:3 frame and stay there.
 * RECOMP_ASPECT=4:3 gives the console's picture, with bars at the sides;
 * stretch scales each axis separately and is the only mode that distorts.
 *
 * Where the picture is narrower or shorter than the window, the rest is
 * blacked out at present time (sink_draw_bars).
 */
static void sink_configure_output(uint32_t title_w, uint32_t title_h)
{
    const char *res = getenv("RECOMP_RES");
    const char *asp = getenv("RECOMP_ASPECT");
    unsigned w = 1920, h = 1080, an = 16, ad = 9;
    int stretch = 0;
    float aspect, vis_w, vis_h;

    if (res && sscanf(res, "%ux%u", &w, &h) == 2 && w >= 160 && h >= 120
            && w <= 16384 && h <= 16384) {
        /* as given */
    } else {
        w = 1920;
        h = 1080;
    }
    if (asp && (asp[0] == 's' || asp[0] == 'S'))
        stretch = 1;
    else if (!(asp && sscanf(asp, "%u:%u", &an, &ad) == 2 && an && ad)) {
        an = 16;
        ad = 9;
    }

    g_sink.out_w = w;
    g_sink.out_h = h;
    if (stretch) {
        g_sink.sx = (float)w / (float)title_w;
        g_sink.sy = (float)h / (float)title_h;
        g_sink.ox = g_sink.oy = 0.0f;
        g_sink.vis_x0 = g_sink.vis_y0 = 0.0f;
        g_sink.vis_x1 = (float)w;
        g_sink.vis_y1 = (float)h;
    } else {
        aspect = (float)an / (float)ad;
        vis_w = (float)h * aspect;
        if (vis_w > (float)w)
            vis_w = (float)w;
        vis_h = vis_w / aspect;
        g_sink.sy = g_sink.sx = vis_h / (float)title_h;
        g_sink.ox = ((float)w - (float)title_w * g_sink.sx) * 0.5f;
        g_sink.oy = ((float)h - vis_h) * 0.5f;
        g_sink.vis_x0 = ((float)w - vis_w) * 0.5f;
        g_sink.vis_x1 = g_sink.vis_x0 + vis_w;
        g_sink.vis_y0 = g_sink.oy;
        g_sink.vis_y1 = g_sink.oy + vis_h;
    }
    fprintf(stderr, "[NV2A-D3D11] output %ux%u, picture %s (%.0fx%.0f at %.0f,%.0f),"
                    " title surface %ux%u scaled %.3fx%.3f\n",
            w, h, stretch ? "stretched" : (asp ? asp : "16:9"),
            g_sink.vis_x1 - g_sink.vis_x0, g_sink.vis_y1 - g_sink.vis_y0,
            g_sink.vis_x0, g_sink.vis_y0, title_w, title_h,
            g_sink.sx, g_sink.sy);
}

/* ── Window ────────────────────────────────────────────────── */

extern void xbox_FramebufferKeyEvent(int vk, int down);

static LRESULT CALLBACK sink_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        /* The title has no idea a window exists, so closing it cannot ask the
         * title to quit; it ends the process, which is what the user meant. */
        ExitProcess(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    /* The keyboard stands in for a pad through the framebuffer window's key
     * table (RECOMP_KEYBOARD, src/input). This is the window with the focus
     * when the sink is drawing, so it is the one that has to fill it in. */
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        xbox_FramebufferKeyEvent((int)wp, 1);
        return msg == WM_SYSKEYDOWN ? DefWindowProcA(hwnd, msg, wp, lp) : 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        xbox_FramebufferKeyEvent((int)wp, 0);
        return msg == WM_SYSKEYUP ? DefWindowProcA(hwnd, msg, wp, lp) : 0;
    case WM_KILLFOCUS:
        xbox_FramebufferKeyEvent(-1, 0);
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

static DWORD WINAPI sink_window_thread(LPVOID unused)
{
    WNDCLASSA wc;
    RECT rc = { 0, 0, 960, 720 };
    DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    MSG msg;

    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = sink_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    /* The executable's own icon, for the title bar and the taskbar: icon
     * resource 1, if the game's build put one there. Without it this is
     * NULL and the window has the system's default icon, as before. */
    wc.hIcon         = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    wc.lpszClassName = "xboxrecomp_nv2a_d3d11";
    RegisterClassA(&wc);

    /* A window whose client area is the back buffer, so nothing is scaled a
     * second time on the way to the screen. If that does not fit on the
     * desktop with a frame round it -- 1920x1080 on a 1920x1080 monitor --
     * it is a borderless window at the corner of the screen instead. */
    rc.right = (LONG)g_sink.out_w;
    rc.bottom = (LONG)g_sink.out_h;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    if (rc.right - rc.left > GetSystemMetrics(SM_CXSCREEN)
            || rc.bottom - rc.top > GetSystemMetrics(SM_CYSCREEN)
            || getenv("RECOMP_BORDERLESS")) {
        style = WS_POPUP | WS_VISIBLE;
        rc.left = rc.top = 0;
        rc.right = (LONG)g_sink.out_w;
        rc.bottom = (LONG)g_sink.out_h;
        x = y = 0;
        g_sink.borderless = 1;
    }
    /* RECOMP_WINDOW_POS=x,y: where the window's corner goes. An instance
     * that reboots into itself sets it for its successor, so the picture
     * stays where the user put it. */
    if (!g_sink.borderless) {
        const char *pos = getenv("RECOMP_WINDOW_POS");
        int px, py;

        if (pos && sscanf(pos, "%d,%d", &px, &py) == 2) {
            x = px;
            y = py;
        }
    }
    g_sink.hwnd = CreateWindowExA(0, wc.lpszClassName, g_sink.title, style,
                                  x, y,
                                  rc.right - rc.left, rc.bottom - rc.top,
                                  NULL, NULL, wc.hInstance, NULL);
    SetEvent(g_sink.window_ready);
    if (!g_sink.hwnd)
        return 0;

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

/* ── Device ────────────────────────────────────────────────── */

/* Created on first use, from the thread that will draw, at the size of the
 * surface the title is actually rendering to. */
static int sink_ensure_device(uint32_t width, uint32_t height)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3D8 *d3d;
    HRESULT hr;

    if (g_sink.dev)
        return 1;
    if (g_sink.failed || !width || !height)
        return 0;

    sink_configure_output(width, height);
    g_sink.window_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, sink_window_thread, NULL, 0, NULL));
    WaitForSingleObject(g_sink.window_ready, 5000);
    if (!g_sink.hwnd) {
        fprintf(stderr, "[NV2A-D3D11] could not create a window\n");
        g_sink.failed = 1;
        return 0;
    }

    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth  = g_sink.out_w;
    pp.BackBufferHeight = g_sink.out_h;
    pp.BackBufferFormat = D3DFMT_LIN_A8R8G8B8;
    pp.BackBufferCount  = 1;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow    = g_sink.hwnd;
    pp.Windowed         = TRUE;

    d3d = xbox_Direct3DCreate8(0);
    hr = d3d ? d3d->lpVtbl->CreateDevice(d3d, 0, 0, g_sink.hwnd, 0, &pp,
                                         &g_sink.dev)
             : E_FAIL;
    if (FAILED(hr) || !g_sink.dev) {
        fprintf(stderr, "[NV2A-D3D11] device creation failed: 0x%08lX\n",
                (unsigned long)hr);
        g_sink.dev = NULL;
        g_sink.failed = 1;
        return 0;
    }
    g_sink.width  = width;
    g_sink.height = height;
    fprintf(stderr, "[NV2A-D3D11] rendering %ux%u through D3D11 at %ux%u\n",
            width, height, g_sink.out_w, g_sink.out_h);
    return 1;
}

/* ── Frame times ───────────────────────────────────────────── */

/* RECOMP_FRAME_TIMES=<ms>: where a slow frame's time went.
 *
 * A frame here is the time from one flip to the next on the thread that
 * executes the title's commands. Each one longer than <ms> (20 when the value
 * is not a number) gets a line saying how much of it this thread spent
 * drawing batches, checking and uploading textures, waiting for the answers
 * to visibility tests and inside Present. "decode" is the rest of the time
 * the executor was busy: reading the title's commands, running its vertex
 * programs and assembling the batches. "idle" is the time it had nothing
 * submitted to execute, which is the title's own share of the frame. Once a
 * second there is a summary line, slow frames or not.
 *
 * The tick on each line is GetTickCount's, the clock the [DUMP] line of an F9
 * screenshot carries, so F9 marks a place in the log. */
static struct {
    int      on;                    /* 0 not asked yet, 1 on, -1 off */
    double   slow_ms, ms_per_count;
    LONGLONG last_flip, second, last_busy;
    /* Since the last flip. */
    LONGLONG draw, textures, count, present;
    uint32_t batches, uploads, tests, unanswered;
    /* Since the last summary. */
    uint32_t frames, slow, presented;
    double   sum_ms, worst_ms;
} g_ft;

/* The lines are written by a thread of their own. A write to the log takes
 * a millisecond or two, and made on the executor's thread a line about a slow
 * frame was itself a good part of the next one. */
static struct {
    CRITICAL_SECTION lock;
    char   text[1 << 16];
    size_t used;
} g_ft_log;

static void ft_log(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    EnterCriticalSection(&g_ft_log.lock);
    n = vsnprintf(g_ft_log.text + g_ft_log.used,
                  sizeof g_ft_log.text - g_ft_log.used, fmt, ap);
    /* A line that does not fit is dropped whole. */
    if (n > 0 && (size_t)n < sizeof g_ft_log.text - g_ft_log.used)
        g_ft_log.used += (size_t)n;
    LeaveCriticalSection(&g_ft_log.lock);
    va_end(ap);
}

static DWORD WINAPI ft_log_thread(LPVOID unused)
{
    static char out[sizeof g_ft_log.text];
    size_t n;

    (void)unused;
    for (;;) {
        Sleep(250);
        EnterCriticalSection(&g_ft_log.lock);
        n = g_ft_log.used;
        memcpy(out, g_ft_log.text, n);
        g_ft_log.used = 0;
        LeaveCriticalSection(&g_ft_log.lock);
        if (n) {
            fwrite(out, 1, n, stderr);
            fflush(stderr);
        }
    }
}

/* The time now, or 0 when nobody asked for frame times. */
static LONGLONG ft_now(void)
{
    LARGE_INTEGER t;

    if (g_ft.on <= 0) {
        const char *e = getenv("RECOMP_FRAME_TIMES");

        if (g_ft.on < 0 || !e) {
            g_ft.on = -1;
            return 0;
        }
        g_ft.on = 1;
        InitializeCriticalSection(&g_ft_log.lock);
        CloseHandle(CreateThread(NULL, 0, ft_log_thread, NULL, 0, NULL));
        g_ft.slow_ms = atof(e) > 0.0 ? atof(e) : 20.0;
        QueryPerformanceFrequency(&t);
        g_ft.ms_per_count = 1000.0 / (double)t.QuadPart;
    }
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void ft_add(LONGLONG *sum, LONGLONG since)
{
    if (since)
        *sum += ft_now() - since;
}

static void ft_flip(void)
{
    LONGLONG now = ft_now(), busy;
    unsigned long tick;

    if (!now)
        return;
    tick = (unsigned long)GetTickCount();
    busy = nv2a_pb_exec_busy_counts();
    if (g_ft.last_flip) {
        double k = g_ft.ms_per_count;
        double ms = (double)(now - g_ft.last_flip) * k;
        double here = (double)(g_ft.draw + g_ft.count + g_ft.present) * k;
        double exec = (double)(busy - g_ft.last_busy) * k;

        g_ft.frames++;
        g_ft.sum_ms += ms;
        if (ms > g_ft.worst_ms)
            g_ft.worst_ms = ms;
        if (ms > g_ft.slow_ms) {
            g_ft.slow++;
            ft_log("[FRAME] tick=%lu %.1f ms: draw %.1f (%u batches),"
                            " textures %.1f (%u uploaded), tests %.1f (%u, %u"
                            " unanswered), present %.1f, decode %.1f, idle %.1f\n",
                    tick, ms, (double)(g_ft.draw - g_ft.textures) * k,
                    g_ft.batches, (double)g_ft.textures * k, g_ft.uploads,
                    (double)g_ft.count * k, g_ft.tests, g_ft.unanswered,
                    (double)g_ft.present * k, exec > here ? exec - here : 0.0,
                    ms > exec ? ms - exec : 0.0);
        }
    }
    g_ft.last_flip = now;
    g_ft.last_busy = busy;
    g_ft.draw = g_ft.textures = g_ft.count = g_ft.present = 0;
    g_ft.batches = g_ft.uploads = g_ft.tests = g_ft.unanswered = 0;

    if (!g_ft.second)
        g_ft.second = now;
    if ((double)(now - g_ft.second) * g_ft.ms_per_count >= 1000.0) {
        ft_log("[FRAME] tick=%lu 1s: %u frames, %u presented, avg"
                        " %.1f ms, worst %.1f ms, %u slow\n",
                tick, g_ft.frames, g_ft.presented,
                g_ft.frames ? g_ft.sum_ms / g_ft.frames : 0.0, g_ft.worst_ms,
                g_ft.slow);
        g_ft.second = now;
        g_ft.frames = g_ft.slow = g_ft.presented = 0;
        g_ft.sum_ms = g_ft.worst_ms = 0.0;
    }
}

/* ── Textures ──────────────────────────────────────────────── */

/* FNV-1a over 64-bit words: fast enough to run over a video frame every
 * frame, and all it has to do is notice that the bytes changed. */
static uint64_t sink_hash(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i, words = n / 8;

    for (i = 0; i < words; i++) {
        uint64_t w;
        memcpy(&w, p + i * 8, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    for (i = words * 8; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static void sink_upload(SinkTexture *t)
{
    size_t texels = (size_t)t->width * t->height;
    uint32_t level;

    if (texels > g_sink.scratch_texels) {
        free(g_sink.scratch);
        g_sink.scratch = (uint32_t *)malloc(texels * 4);
        g_sink.scratch_texels = g_sink.scratch ? texels : 0;
    }
    if (!g_sink.scratch)
        return;

    /* Every level the title supplied, not ones made here by averaging: a
     * mip chain is art, and a chain-link fence or a sign is drawn to stay
     * readable in it. */
    for (level = 0; level < t->levels; level++) {
        uint32_t w = (t->width >> level) ? (t->width >> level) : 1;
        uint32_t h = (t->height >> level) ? (t->height >> level) : 1;
        D3DLOCKED_RECT lr;
        uint32_t y;

        if (!nv2a_pb_exec_decode_texture_level(level, g_sink.scratch))
            return;
        memset(&lr, 0, sizeof lr);
        if (FAILED(t->tex->lpVtbl->LockRect(t->tex, level, &lr, NULL, 0))
                || !lr.pBits)
            return;
        for (y = 0; y < h; y++)
            memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch,
                   g_sink.scratch + (size_t)y * w, (size_t)w * 4);
        t->tex->lpVtbl->UnlockRect(t->tex, level);
    }
    g_sink.uploads++;
    g_ft.uploads++;
}

/* The D3D texture for what the title has bound, decoded and uploaded if it is
 * new or the guest has rewritten it since the last frame it was used in. */
static IDirect3DTexture8 *sink_texture(const Nv2aSinkTexture *src)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    SinkTexture *t = NULL, *spare = NULL, *oldest = &g_sink.tex[0];
    int i, fresh = 0;
    uint32_t bucket, levels = src->levels ? src->levels : 1;

    if (!src->width || !src->height || src->width > 4096 || src->height > 4096)
        return NULL;

    bucket = ((src->offset >> 8) ^ (src->offset >> 20) ^ src->palette)
           & (SINK_TEX_BUCKETS - 1u);
    if (g_sink.tex_hint[bucket]) {
        SinkTexture *c = &g_sink.tex[g_sink.tex_hint[bucket] - 1];
        if (c->tex && c->offset == src->offset && c->width == src->width
                && c->height == src->height && c->format == src->format
                && c->palette == src->palette && c->levels == levels)
            t = c;
    }

    for (i = 0; !t && i < SINK_MAX_TEXTURES; i++) {
        SinkTexture *c = &g_sink.tex[i];
        if (!c->tex) {
            if (!spare)
                spare = c;
            continue;
        }
        if (c->offset == src->offset && c->width == src->width
                && c->height == src->height && c->format == src->format
                && c->palette == src->palette && c->levels == levels) {
            t = c;
            break;
        }
        if (c->used_frame < oldest->used_frame)
            oldest = c;
    }

    if (!t) {
        /* A free slot if there is one, otherwise the least recently drawn. */
        t = spare ? spare : oldest;
        if (t->tex)
            t->tex->lpVtbl->Release(t->tex);
        memset(t, 0, sizeof *t);
        fresh = 1;
        if (FAILED(g_sink.dev->lpVtbl->CreateTexture(
                g_sink.dev, src->width, src->height, levels, 0,
                D3DFMT_LIN_A8R8G8B8, 0, &t->tex)) || !t->tex) {
            t->tex = NULL;
            return NULL;
        }
        t->offset = src->offset;
        t->width  = src->width;
        t->height = src->height;
        t->format = src->format;
        t->palette = src->palette;
        t->levels = levels;
        t->checked_frame = (uint32_t)-1;
    }

    g_sink.tex_hint[bucket] = (uint16_t)(t - g_sink.tex + 1);
    g_sink.bound = t;
    t->used_frame = g_sink.frame;
    if (t->checked_frame != g_sink.frame) {
        LONGLONG ft = ft_now();
        uint64_t h = src->bytes ? sink_hash(mem + src->offset, src->bytes)
                                : (uint64_t)g_sink.frame + 1;
        /* A title recolours palettised art by rewriting the palette and
         * leaving the indices alone. */
        if (src->palette)
            h ^= sink_hash(mem + src->palette, 1024) * 0x9E3779B97F4A7C15ull;
        t->checked_frame = g_sink.frame;
        if (fresh || h != t->hash) {
            t->hash = h;
            sink_upload(t);
        }
        ft_add(&g_ft.textures, ft);
    }
    return t->tex;
}

/* ── State ─────────────────────────────────────────────────── */

/* NV097 texture address modes to D3D's. */
static DWORD sink_address(uint32_t nv)
{
    switch (nv) {
    case 1:  return D3DTADDRESS_WRAP;
    case 2:  return D3DTADDRESS_MIRROR;
    case 4:  return D3DTADDRESS_BORDER;
    default: return D3DTADDRESS_CLAMP;      /* 3 clamp to edge, 5 clamp */
    }
}

/* The sampler a stage asks for: SET_TEXTURE_FILTER, as the title wrote it.
 *
 * None of this used to be carried over, and a stage nobody configures
 * samples the nearest texel of the top level -- every surface in a level
 * shimmered with distance and broke into squares up close, where the title
 * had asked for trilinear filtering of the mip chain it supplied.
 *
 * The minification filter names both how texels are combined and how levels
 * are chosen. The convolution kernels (quincunx, gaussian) have no D3D11
 * counterpart and are drawn as linear. */
static void sink_sampler(IDirect3DDevice8 *dev, const Nv2aSinkTexture *src,
                         int clamp_u, int clamp_v, int nearest)
{
    uint32_t mag = (src->filter >> 24) & 0xFu, min = (src->filter >> 16) & 0xFFu;
    int32_t bias_fixed = (int32_t)(src->filter & 0x1FFFu);
    DWORD d3d_mag = D3DTEXF_POINT, d3d_min = D3DTEXF_POINT, d3d_mip = D3DTEXF_NONE;
    DWORD bias_bits;
    float bias;

    if (mag >= 2)
        d3d_mag = D3DTEXF_LINEAR;
    switch (min) {
    case 2: d3d_min = D3DTEXF_LINEAR;                           break;
    case 3:                           d3d_mip = D3DTEXF_POINT;  break;
    case 4: d3d_min = D3DTEXF_LINEAR; d3d_mip = D3DTEXF_POINT;  break;
    case 5:                           d3d_mip = D3DTEXF_LINEAR; break;
    case 6: d3d_min = D3DTEXF_LINEAR; d3d_mip = D3DTEXF_LINEAR; break;
    case 7: d3d_min = D3DTEXF_LINEAR;                           break;
    default: break;                             /* 1 nearest, 0 never set */
    }
    if (src->levels <= 1)
        d3d_mip = D3DTEXF_NONE;
    if (nearest) {
        d3d_mag = d3d_min = D3DTEXF_POINT;
        d3d_mip = D3DTEXF_NONE;
    }
    /* Thirteen bits, signed, in 1/256ths of a level. */
    if (bias_fixed & 0x1000)
        bias_fixed -= 0x2000;
    bias = d3d_mip == D3DTEXF_NONE ? 0.0f : (float)bias_fixed / 256.0f;
    memcpy(&bias_bits, &bias, sizeof bias_bits);

    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, d3d_mag);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_MINFILTER, d3d_min);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_MIPFILTER, d3d_mip);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_MIPMAPLODBIAS, bias_bits);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ADDRESSU,
        clamp_u ? D3DTADDRESS_CLAMP : sink_address(src->addr_u));
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ADDRESSV,
        clamp_v ? D3DTADDRESS_CLAMP : sink_address(src->addr_v));
}

/* NV097 blend factors carry OpenGL's numbering. */
static DWORD sink_blend(uint32_t nv, DWORD fallback)
{
    switch (nv) {
    case 0x0000: return D3DBLEND_ZERO;
    case 0x0001: return D3DBLEND_ONE;
    case 0x0300: return D3DBLEND_SRCCOLOR;
    case 0x0301: return D3DBLEND_INVSRCCOLOR;
    case 0x0302: return D3DBLEND_SRCALPHA;
    case 0x0303: return D3DBLEND_INVSRCALPHA;
    case 0x0304: return D3DBLEND_DESTALPHA;
    case 0x0305: return D3DBLEND_INVDESTALPHA;
    case 0x0306: return D3DBLEND_DESTCOLOR;
    case 0x0307: return D3DBLEND_INVDESTCOLOR;
    case 0x0308: return D3DBLEND_SRCALPHASAT;
    default:     return fallback;
    }
}

/* ── Frame capture ─────────────────────────────────────────── */

/* RECOMP_D3D11_DUMP=<prefix> writes what is about to be presented to
 * <prefix>NNN.bmp: sixty frames, one a second unless RECOMP_D3D11_DUMP_MS
 * gives another interval. It reads the
 * render target back rather than photographing the window, so it shows what
 * was drawn whether or not the window is visible, covered or on another
 * monitor -- and nothing else that happens to be on the screen. */
static void sink_dump_frame(void)
{
    static const char *prefix;
    static int asked, seq;
    static DWORD last_ms;
    ID3D11DeviceContext *ctx;
    ID3D11Device *d3d;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Resource *res = NULL;
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE map;
    DWORD now = GetTickCount();

    static DWORD interval_ms = 1000;
    static DWORD start_ms;
    static char shot_path[MAX_PATH];
    int want_shot = 0;

    if (!asked) {
        const char *ms = getenv("RECOMP_D3D11_DUMP_MS");
        asked = 1;
        prefix = getenv("RECOMP_D3D11_DUMP");
        if (ms && atoi(ms) > 0)
            interval_ms = (DWORD)atoi(ms);
        /* RECOMP_D3D11_DUMP_AFTER=<seconds>: start that long after the first
         * frame, so the sixty can be spent densely on one moment. */
        ms = getenv("RECOMP_D3D11_DUMP_AFTER");
        start_ms = now + (ms ? (DWORD)atoi(ms) * 1000u : 0u);
    }
    /* F9, with the game window in front, writes the frame being shown to
     * %TEMP%\\dmf_shot_NNN.bmp whatever the variables above say: the way to
     * hand over a picture of something that only happens while playing. */
    {
        static int shot_seq;
        static SHORT was;
        SHORT key = GetAsyncKeyState(VK_F9);
        int pressed = (key & 0x8000) && !(was & 0x8000)
                   && GetForegroundWindow() == g_sink.hwnd;
        was = key;
        if (pressed) {
            const char *tmp = getenv("TEMP");
            snprintf(shot_path, sizeof shot_path, "%s/dmf_shot_%03d.bmp",
                     tmp ? tmp : ".", shot_seq++);
            want_shot = 1;
        }
    }
    if (!want_shot) {
        if (!prefix || seq >= 60 || (int)(now - start_ms) < 0
                || now - last_ms < interval_ms)
            return;
        last_ms = now;
    }

    d3d = d3d8_GetD3D11Device();
    ctx = d3d8_GetD3D11Context();
    if (!d3d || !ctx)
        return;
    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &rtv, NULL);
    if (!rtv)
        return;
    ID3D11RenderTargetView_GetResource(rtv, &res);
    ID3D11RenderTargetView_Release(rtv);
    if (!res)
        return;

    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(d3d, &desc, NULL, &staging))) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, res);
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging,
                                              0, D3D11_MAP_READ, 0, &map))) {
            char path[512];
            FILE *f;

            /* The tick goes in the log so a capture can be matched to
             * anything else that is keyed on time (RECOMP_Z_CLAMP=alt). */
            fprintf(stderr, "[DUMP] %03d tick=%lu\n", seq,
                    (unsigned long)now);
            if (want_shot)
                snprintf(path, sizeof path, "%s", shot_path);
            else
                snprintf(path, sizeof path, "%s%03d.bmp", prefix, seq++);
            f = fopen(path, "wb");
            if (f) {
                uint32_t w = desc.Width, h = desc.Height, y, x;
                uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
                uint8_t hdr[54], *line = (uint8_t *)calloc(1, row);
                int rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM
                        || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

                memset(hdr, 0, sizeof hdr);
                hdr[0] = 'B'; hdr[1] = 'M';
                memcpy(hdr + 2, &size, 4);
                hdr[10] = 54; hdr[14] = 40;
                memcpy(hdr + 18, &w, 4);
                memcpy(hdr + 22, &h, 4);
                hdr[26] = 1; hdr[28] = 24;
                fwrite(hdr, 1, sizeof hdr, f);
                for (y = 0; line && y < h; y++) {       /* BMP is bottom-up */
                    const uint8_t *src = (const uint8_t *)map.pData
                                       + (size_t)(h - 1 - y) * map.RowPitch;
                    for (x = 0; x < w; x++) {
                        line[x * 3 + 0] = src[x * 4 + (rgba ? 2 : 0)];
                        line[x * 3 + 1] = src[x * 4 + 1];
                        line[x * 3 + 2] = src[x * 4 + (rgba ? 0 : 2)];
                    }
                    fwrite(line, 1, row, f);
                }
                free(line);
                fclose(f);
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
        }
        ID3D11Texture2D_Release(staging);
    }
    ID3D11Resource_Release(res);
}

/* The NV2A's stencil operations are GL's; D3DSTENCILOP_* is 1 keep, 2 zero,
 * 3 replace, 4/5 saturating increment/decrement, 6 invert, 7/8 wrapping. */
static DWORD sink_stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000u: return 2;
    case 0x1E01u: return 3;
    case 0x1E02u: return 4;
    case 0x1E03u: return 5;
    case 0x150Au: return 6;
    case 0x8507u: return 7;
    case 0x8508u: return 8;
    default:      return 1;
    }
}

/* ── Sink callbacks ────────────────────────────────────────── */

static void sink_clear(uint32_t argb, uint32_t width, uint32_t height,
                       uint32_t what)
{
    DWORD flags = 0;

    if (!sink_ensure_device(width, height))
        return;
    if (what & NV2A_SINK_CLEAR_COLOR) flags |= D3DCLEAR_TARGET;
    if (what & NV2A_SINK_CLEAR_DEPTH) flags |= D3DCLEAR_ZBUFFER;
    if (what & NV2A_SINK_CLEAR_STENCIL) flags |= D3DCLEAR_STENCIL;
    g_sink.dev->lpVtbl->Clear(g_sink.dev, 0, NULL, flags, argb, 1.0f,
                              (what >> 8) & 0xFFu);
    if (what & NV2A_SINK_CLEAR_COLOR)
        g_sink.drew_this_frame = 1;
}

typedef struct { float x, y, z, rhw; uint32_t color; float u, v; } SinkVertex;

/* Is this batch nothing but sprites drawn one texel to a pixel?
 *
 * That is how a title draws its interface, and at its own resolution such a
 * sprite is not filtered whatever the stage says: every pixel reads the
 * middle of one texel. The art is made for that. A glyph or an icon sits in
 * a cell whose unused texels are transparent black, cut away by the alpha
 * test, and nothing ever reads between a texel that is kept and one that is
 * not.
 *
 * Scaled up with the filter the stage asks for, something does: the kept
 * colour is blended towards that black before the test, and every icon gets
 * a dark fringe, every cell a faint box. So these are drawn the way the
 * console showed them -- each texel one flat block -- and the filter is
 * left for sprites the title itself scales, and for everything in
 * perspective.
 */
static int sink_unit_sprites(const Nv2aSinkVertex *v, uint32_t count)
{
    uint32_t i;
    int k;

    for (i = 0; i + 2 < count; i += 3) {
        float x0 = v[i].x, x1 = v[i].x, y0 = v[i].y, y1 = v[i].y;
        float u0 = v[i].u, u1 = v[i].u, w0 = v[i].v, w1 = v[i].v;
        float ru, rv;

        if (v[i].rhw != v[i + 1].rhw || v[i].rhw != v[i + 2].rhw)
            return 0;
        for (k = 1; k < 3; k++) {
            const Nv2aSinkVertex *q = &v[i + k];
            if (q->x < x0) x0 = q->x;
            if (q->x > x1) x1 = q->x;
            if (q->y < y0) y0 = q->y;
            if (q->y > y1) y1 = q->y;
            if (q->u < u0) u0 = q->u;
            if (q->u > u1) u1 = q->u;
            if (q->v < w0) w0 = q->v;
            if (q->v > w1) w1 = q->v;
        }
        for (k = 0; k < 3; k++) {
            const Nv2aSinkVertex *q = &v[i + k];
            if ((q->x != x0 && q->x != x1) || (q->y != y0 && q->y != y1)
                    || (q->u != u0 && q->u != u1) || (q->v != w0 && q->v != w1))
                return 0;
        }
        if (!(x1 > x0) || !(y1 > y0))
            return 0;
        ru = (u1 - u0) / (x1 - x0);
        rv = (w1 - w0) / (y1 - y0);
        if (ru < 0.98f || ru > 1.02f || rv < 0.98f || rv > 1.02f)
            return 0;
    }
    return count >= 3;
}

static void sink_triangles(const Nv2aSinkVertex *v, uint32_t count,
                           const Nv2aSinkTexture *src,
                           const Nv2aSinkState *state)
{
    static SinkVertex *out;
    float map_ox = g_sink.ox, map_sx = g_sink.sx;
    static uint32_t out_cap;
    IDirect3DDevice8 *dev;
    IDirect3DTexture8 *tex = NULL;
    float inv_w = 1.0f, inv_h = 1.0f;
    float u_lo = 1.0e30f, u_hi = -1.0e30f, v_lo = 1.0e30f, v_hi = -1.0e30f;
    /* The current triangle, when it is one half of a sprite: its texel
     * rectangle, and how far in from each edge of it to sample. */
    float su0 = 0, su1 = 0, sv0 = 0, sv1 = 0, inset_u = 0, inset_v = 0;
    int unit = 0;
    uint32_t i;

    if (!g_sink.dev || count < 3)
        return;
    dev = g_sink.dev;

    /* RECOMP_D3D11_VERBOSE=<n>: describe the first n batches. What a batch
     * looks like on screen and what it was asked to be are different
     * questions, and only the second one can be answered from a log. */
    {
        static int budget = -1;
        if (budget < 0) {
            const char *e = getenv("RECOMP_D3D11_VERBOSE");
            budget = e ? atoi(e) : 0;
        }
        if (budget > 0) {
            uint32_t k;
            budget--;
            fprintf(stderr, "[NV2A-D3D11] frame %u batch: %u verts, tex %s"
                            " 0x%08X %ux%u fmt 0x%02X, blend %d (0x%X,0x%X),"
                            " alphatest %d (0x%X ref %u)\n",
                    g_sink.frame, count, (src && src->valid) ? "yes" : "no",
                    src ? src->offset : 0, src ? src->width : 0,
                    src ? src->height : 0, src ? src->format : 0,
                    state->blend_enable, state->blend_src, state->blend_dst,
                    state->alpha_test_enable, state->alpha_func,
                    state->alpha_ref);
            for (k = 0; k < count && k < 6; k++)
                fprintf(stderr, "    [%u] (%.1f, %.1f, %.3f, %.3f) %08X"
                                " uv (%.1f, %.1f)\n", k, v[k].x, v[k].y,
                        v[k].z, v[k].rhw, v[k].argb, v[k].u, v[k].v);
        }
    }

    if (count > out_cap) {
        free(out);
        out = (SinkVertex *)malloc((size_t)count * sizeof *out);
        out_cap = out ? count : 0;
        if (!out)
            return;
    }

    if (src && src->valid) {
        tex = sink_texture(src);
        inv_w = 1.0f / (float)src->width;
        inv_h = 1.0f / (float)src->height;
        /* Whether a texture's sprites are filtered is a property of the
         * texture, not of the frame. A menu's selected line pulses in size,
         * and as it passed through one texel to a pixel its text went from
         * smooth to blocks and back, every beat. So a texture that has ever
         * been drawn at another scale -- a font, by then -- is filtered from
         * there on, and only one that never has is left as texels. */
        unit = sink_unit_sprites(v, count);
        if (tex && g_sink.bound) {
            if (!unit)
                g_sink.bound->scaled = 1;
            else if (g_sink.bound->scaled)
                unit = 0;
        }
    }

    for (i = 0; i < count; i++) {
        /* The title's pixels to the host's: see sink_configure_output.
         *
         * A flat triangle that spans the title's whole width belongs to
         * something meant to cover the screen -- a menu backdrop, a fade, a
         * tint over the scene -- and in a picture wider than 4:3 it has to
         * cover that, or it shows as a band down the middle. Those are
         * stretched to the picture's edges; everything else, including the
         * text and panels drawn in the same batch, keeps its proportions.
         * "Flat" is all three vertices at the same 1/w, which a triangle in
         * perspective is not. Decided once per triangle. */
        if (i % 3 == 0) {
            map_ox = g_sink.ox;
            map_sx = g_sink.sx;
            if (i + 2 < count && v[i].rhw == v[i + 1].rhw
                    && v[i].rhw == v[i + 2].rhw) {
                float lo = v[i].x, hi = v[i].x;
                if (v[i + 1].x < lo) lo = v[i + 1].x;
                if (v[i + 2].x < lo) lo = v[i + 2].x;
                if (v[i + 1].x > hi) hi = v[i + 1].x;
                if (v[i + 2].x > hi) hi = v[i + 2].x;
                if (lo <= 1.0f && hi >= (float)g_sink.width - 1.0f) {
                    map_ox = g_sink.vis_x0;
                    map_sx = (g_sink.vis_x1 - g_sink.vis_x0)
                           / (float)g_sink.width;
                }
            }
        }
        /* A sprite is a flat, axis-aligned rectangle showing a rectangle of
         * its texture -- often a cell of a sheet with other art beside it.
         * At the title's resolution a texel lands on a pixel and each pixel
         * reads the middle of its texel. Scaled up, the pixels along the
         * rectangle's edge read from within half a texel of the cell's
         * border, and a filter blends in whatever lies across it: a dark or
         * light hairline down every seam of a tiled menu panel.
         *
         * So the corners are moved in to where the middle of the outermost
         * host pixel reads the middle of the outermost texel, which is what
         * the title's own pixel read there. Nothing at one host pixel per
         * texel or fewer; under half a texel at any scale. */
        if (i % 3 == 0) {
            inset_u = inset_v = 0.0f;
            if (src && src->valid && !unit && i + 2 < count
                    && v[i].rhw == v[i + 1].rhw && v[i].rhw == v[i + 2].rhw) {
                float x0 = v[i].x, x1 = v[i].x, y0 = v[i].y, y1 = v[i].y;
                int k, aligned = 1;

                su0 = su1 = v[i].u;
                sv0 = sv1 = v[i].v;
                for (k = 1; k < 3; k++) {
                    const Nv2aSinkVertex *q = &v[i + k];
                    if (q->x < x0) x0 = q->x;
                    if (q->x > x1) x1 = q->x;
                    if (q->y < y0) y0 = q->y;
                    if (q->y > y1) y1 = q->y;
                    if (q->u < su0) su0 = q->u;
                    if (q->u > su1) su1 = q->u;
                    if (q->v < sv0) sv0 = q->v;
                    if (q->v > sv1) sv1 = q->v;
                }
                for (k = 0; k < 3; k++) {
                    const Nv2aSinkVertex *q = &v[i + k];
                    if ((q->x != x0 && q->x != x1) || (q->y != y0 && q->y != y1)
                            || (q->u != su0 && q->u != su1)
                            || (q->v != sv0 && q->v != sv1))
                        aligned = 0;
                }
                if (aligned && x1 > x0 && y1 > y0) {
                    /* Texels per host pixel along each axis. */
                    float ru = (su1 - su0) / ((x1 - x0) * map_sx);
                    float rv = (sv1 - sv0) / ((y1 - y0) * g_sink.sy);
                    if (ru > 0.0f && ru < 1.0f && su1 - su0 >= 1.0f)
                        inset_u = 0.5f - 0.5f * ru;
                    if (rv > 0.0f && rv < 1.0f && sv1 - sv0 >= 1.0f)
                        inset_v = 0.5f - 0.5f * rv;
                }
            }
        }
        out[i].x = map_ox + v[i].x * map_sx;
        out[i].y = g_sink.oy + v[i].y * g_sink.sy;
        /* Depth and 1/w as the batch carried them. Both used to be flattened
         * to constants, which is right for a menu and is why a level could
         * not have been drawn through here even with its vertices in hand:
         * no depth to test, and textures interpolated as if every triangle
         * faced the screen. */
        out[i].z = v[i].z;
        out[i].rhw = v[i].rhw;
        out[i].color = v[i].argb;
        /* The executor works in texels; D3D wants the unit square. */
        if (g_count.open && !g_count.past_edge) {
            /* Is the picture carrying on past the surface on this side? */
            int wide = g_sink.vis_x0 < g_sink.ox - 0.5f;
            int tall = g_sink.vis_y0 < g_sink.oy - 0.5f;
            if ((wide && (v[i].x <= 1.0f
                          || v[i].x >= (float)g_sink.width - 1.0f))
                    || (tall && (v[i].y <= 1.0f
                                 || v[i].y >= (float)g_sink.height - 1.0f)))
                g_count.past_edge = 1;
        }
        out[i].u = v[i].u;
        out[i].v = v[i].v;
        if (inset_u > 0.0f)
            out[i].u += (v[i].u == su0) ? inset_u : -inset_u;
        if (inset_v > 0.0f)
            out[i].v += (v[i].v == sv0) ? inset_v : -inset_v;
        out[i].u *= inv_w;
        out[i].v *= inv_h;
        if (out[i].u < u_lo) u_lo = out[i].u;
        if (out[i].u > u_hi) u_hi = out[i].u;
        if (out[i].v < v_lo) v_lo = out[i].v;
        if (out[i].v > v_hi) v_hi = out[i].v;
    }

    /* NV097 comparison functions are GL_NEVER (0x200) .. GL_ALWAYS (0x207);
     * D3DCMP_* is the same order starting at 1. A function of zero is a
     * title that has not set one yet, and LEQUAL is the hardware default. */
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE,
                                state->depth_test_enable ? TRUE : FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE,
                                state->depth_write ? TRUE : FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZFUNC,
                                state->depth_func ? (state->depth_func & 7u) + 1u
                                                  : D3DCMP_LESSEQUAL);
    /* Stencil. Without it a title's shadow pass -- volumes counted into the
     * stencil buffer, then one translucent black quad over the screen that
     * passes only where the count is non-zero -- darkens the whole picture
     * instead of the shadows. */
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILENABLE,
                                state->stencil_enable ? TRUE : FALSE);
    if (state->stencil_enable) {
        dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILFUNC,
                                    state->stencil_func
                                        ? (state->stencil_func & 7u) + 1u
                                        : D3DCMP_ALWAYS);
        dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILREF,
                                    state->stencil_ref & 0xFFu);
        dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILMASK,
                                    state->stencil_mask_set
                                        ? state->stencil_mask & 0xFFu : 0xFFu);
        dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILWRITEMASK,
                                    state->stencil_write_mask_set
                                        ? state->stencil_write_mask & 0xFFu
                                        : 0xFFu);
        dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILFAIL,
            sink_stencil_op((state->stencil_ops_set & 1u) ? state->stencil_fail
                                                          : 0x1E00u));
        dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILZFAIL,
            sink_stencil_op((state->stencil_ops_set & 2u) ? state->stencil_zfail
                                                          : 0x1E00u));
        dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILPASS,
            sink_stencil_op((state->stencil_ops_set & 4u) ? state->stencil_zpass
                                                          : 0x1E00u));
    }
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    {
        /* The NV2A names the front face and which face to drop; D3D8 names
         * the winding to drop. Window space on both sides, y down. */
        DWORD cull = D3DCULL_NONE;
        static int no_cull = -1;

        if (no_cull < 0)
            no_cull = getenv("RECOMP_D3D11_NOCULL") != NULL;
        if (state->cull_enable && !no_cull && state->cull_face != 0x0408u) {
            int front_cw = state->front_face == 0x0900u;
            int drop_front = state->cull_face == 0x0404u;
            /* Dropping the back face of a clockwise-fronted mesh drops the
             * counter-clockwise triangles, and so on round. */
            cull = (front_cw != drop_front) ? D3DCULL_CCW : D3DCULL_CW;
        }
        dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, cull);
    }
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,
                                state->blend_enable ? TRUE : FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND,
                                sink_blend(state->blend_src, D3DBLEND_ONE));
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND,
                                sink_blend(state->blend_dst, D3DBLEND_ZERO));
    /* Which channels are written, and how source and destination combine.
     * Neither used to be carried over. A title that draws a full-screen quad
     * with only alpha enabled -- to stamp a value into destination alpha for
     * a later pass -- got that quad blended into the picture instead. */
    dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE,
                                state->color_mask_set ? state->color_mask : 0xFu);
    {
        DWORD op = 1;                                   /* D3DBLENDOP_ADD */
        switch (state->blend_equation) {
        case 0x800Au: op = 2; break;                    /* SUBTRACT       */
        case 0x800Bu: op = 3; break;                    /* REVSUBTRACT    */
        case 0x8007u: op = 4; break;                    /* MIN            */
        case 0x8008u: op = 5; break;                    /* MAX            */
        default: break;
        }
        dev->lpVtbl->SetRenderState(dev, D3DRS_BLENDOP, op);
    }
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE,
                                state->alpha_test_enable ? TRUE : FALSE);
    if (state->alpha_test_enable) {
        /* NV097 alpha functions are GL_NEVER (0x200) .. GL_ALWAYS (0x207);
         * D3DCMP_* is the same order starting at 1. */
        dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHAFUNC,
                                    (state->alpha_func & 7u) + 1u);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHAREF,
                                    state->alpha_ref & 0xFFu);
    }

    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE
                                      | D3DFVF_TEX1);
    if (tex) {
        dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8 *)tex);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP,
            state->color_scale >= 4 ? D3DTOP_MODULATE4X :
            state->color_scale == 2 ? D3DTOP_MODULATE2X : D3DTOP_MODULATE);
        dev->lpVtbl->SetTextureStageState(dev, 0, 2 /*COLORARG1*/, D3DTA_TEXTURE);
        dev->lpVtbl->SetTextureStageState(dev, 0, 3 /*COLORARG2*/, 0 /*DIFFUSE*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 4 /*ALPHAOP*/, D3DTOP_MODULATE);
        dev->lpVtbl->SetTextureStageState(dev, 0, 5 /*ALPHAARG1*/, D3DTA_TEXTURE);
        dev->lpVtbl->SetTextureStageState(dev, 0, 6 /*ALPHAARG2*/, 0 /*DIFFUSE*/);
        /* A batch that shows its texture once -- every coordinate between 0
         * and 1 -- repeats nothing, so wrapping does nothing for it except at
         * the very edge, where a filter reads half a texel past it and wrap
         * hands back the opposite side of the texture.
         *
         * At a title's own resolution that is a sliver nobody sees. Larger,
         * it is a line: a faint frame round every tile of a menu, and round
         * every panel of a sky. Dave Mirra Freestyle BMX 2's sky is eight
         * panels and a cap, each on wrap and each drawn from 0 to 1; the top
         * of every panel picked up its own bottom row, the haze at the
         * horizon, and drew a dark line across the sky all the way round.
         *
         * So such a batch is clamped, whatever the stage says. A surface
         * that tiles one texture across many faces, each from 0 to 1, loses
         * the blend across each seam and keeps two neighbouring texels side
         * by side there, which is what an unfiltered surface shows. */
        {
            int once_u = u_lo >= -0.001f && u_hi <= 1.001f;
            int once_v = v_lo >= -0.001f && v_hi <= 1.001f;
            sink_sampler(dev, src, once_u, once_v, unit);
        }
    } else {
        /* The vertex colour and alpha, untouched. Asked for as "select
         * argument 2 = CURRENT", which at stage 0 is the diffuse colour.
         *
         * It used to be "select argument 1 = DIFFUSE", and D3DTA_DIFFUSE is
         * zero -- which the D3D8 layer's stage setup reads as "not set" and
         * replaces with TEXTURE. With no texture bound that is transparent
         * black: an untextured batch lost its colour, and with blending on
         * it lost everything. The ruler frame and tick marks of Dave Mirra
         * Freestyle BMX 2's height meter are exactly that -- flat grey,
         * half-transparent quads -- and so is the rider's shadow. */
        dev->lpVtbl->SetTexture(dev, 0, NULL);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        dev->lpVtbl->SetTextureStageState(dev, 0, 3 /*COLORARG2*/, D3DTA_CURRENT);
        dev->lpVtbl->SetTextureStageState(dev, 0, 4 /*ALPHAOP*/, D3DTOP_SELECTARG2);
        dev->lpVtbl->SetTextureStageState(dev, 0, 6 /*ALPHAARG2*/, D3DTA_CURRENT);
    }

    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, count / 3, out,
                                 sizeof *out);
    g_sink.draws++;
    g_sink.drew_this_frame = 1;
}

/* Black over whatever lies outside the picture: the side bands of a 4:3
 * picture in a wide window, or the top and bottom of a wide one in a tall
 * window. Drawn last, in the host's pixels, with every test off. */
static void sink_draw_bars(void)
{
    IDirect3DDevice8 *dev = g_sink.dev;
    float w = (float)g_sink.out_w, h = (float)g_sink.out_h;
    float rect[4][4];
    SinkVertex v[24];
    int n = 0, r, k;

    if (g_sink.vis_x0 > 0.5f) {
        rect[n][0] = 0.0f; rect[n][1] = 0.0f;
        rect[n][2] = g_sink.vis_x0; rect[n][3] = h; n++;
        rect[n][0] = g_sink.vis_x1; rect[n][1] = 0.0f;
        rect[n][2] = w; rect[n][3] = h; n++;
    }
    if (g_sink.vis_y0 > 0.5f) {
        rect[n][0] = 0.0f; rect[n][1] = 0.0f;
        rect[n][2] = w; rect[n][3] = g_sink.vis_y0; n++;
        rect[n][0] = 0.0f; rect[n][1] = g_sink.vis_y1;
        rect[n][2] = w; rect[n][3] = h; n++;
    }
    if (!n)
        return;
    for (r = 0, k = 0; r < n; r++) {
        static const int corner[6][2] = { {0,1}, {2,1}, {2,3}, {0,1}, {2,3}, {0,3} };
        int c;
        for (c = 0; c < 6; c++, k++) {
            v[k].x = rect[r][corner[c][0]];
            v[k].y = rect[r][corner[c][1]];
            v[k].z = 0.0f;
            v[k].rhw = 1.0f;
            v[k].color = 0xFF000000u;
            v[k].u = v[k].v = 0.0f;
        }
    }
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xFu);
    dev->lpVtbl->SetTexture(dev, 0, NULL);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    dev->lpVtbl->SetTextureStageState(dev, 0, 3 /*COLORARG2*/, D3DTA_CURRENT);
    dev->lpVtbl->SetTextureStageState(dev, 0, 4 /*ALPHAOP*/, D3DTOP_SELECTARG2);
    dev->lpVtbl->SetTextureStageState(dev, 0, 6 /*ALPHAARG2*/, D3DTA_CURRENT);
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE
                                      | D3DFVF_TEX1);
    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, (UINT)(n * 2), v,
                                 sizeof v[0]);
}

static void sink_flip(void)
{
    DWORD now;
    LONGLONG ft;

    ft_flip();

    /* RECOMP_D3D11_STATS: one line every five seconds saying how many flips
     * the title asked for and how many of them had anything in them. A window
     * that has stopped changing looks the same whether the title has hung,
     * is running but drawing nothing this renderer understands, or is being
     * shown the same frame; these counts tell those apart. */
    {
        static int on = -1;
        static DWORD last;
        static uint32_t flips, drawn, last_draws, last_tests;

        if (on < 0)
            on = getenv("RECOMP_D3D11_STATS") != NULL;
        flips++;
        if (g_sink.drew_this_frame)
            drawn++;
        now = GetTickCount();
        if (on && now - last >= 5000) {
            fprintf(stderr, "[NV2A-D3D11] %u flips, %u with content, %u"
                            " batches, %u texture uploads, %u visibility"
                            " tests (%u unanswered) in the last %.1fs\n",
                    flips, drawn, g_sink.draws - last_draws, g_sink.uploads,
                    g_count.tests - last_tests, g_count.timeouts,
                    last ? (now - last) / 1000.0 : 0.0);
            last_tests = g_count.tests;
            last = now;
            flips = drawn = 0;
            last_draws = g_sink.draws;
            g_sink.uploads = 0;
        }
    }

    if (!g_sink.dev || !g_sink.drew_this_frame)
        return;
    g_sink.drew_this_frame = 0;
    g_sink.frame++;

    /* Present waits for the display's vertical blank. A title is not held to
     * that here -- its swap counter is mirrored, so it submits frames as fast
     * as it can build them -- and waiting once per submitted frame would put
     * this thread further behind the command stream with every frame. So show
     * the newest frame at most once per refresh and let the rest go. */
    now = GetTickCount();
    if (now - g_sink.last_present_ms < 15)
        return;
    g_sink.last_present_ms = now;

    sink_draw_bars();
    g_sink.dev->lpVtbl->EndScene(g_sink.dev);
    sink_dump_frame();
    ft = ft_now();
    g_sink.dev->lpVtbl->Present(g_sink.dev, NULL, NULL, NULL, NULL);
    ft_add(&g_ft.present, ft);
    g_ft.presented++;
    /* The first few frames on screen: tell the instance that rebooted into
     * this one, which has kept its window up meanwhile, that it can go. */
    {
        static int presented;

        if (presented < 4 && ++presented == 4) {
            const char *name = getenv("RECOMP_HANDOVER_EVENT");

            if (name && *name) {
                HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);

                if (ev) {
                    SetForegroundWindow(g_sink.hwnd);
                    SetEvent(ev);
                    CloseHandle(ev);
                }
            }
        }
    }
}

/* ── Visibility tests ──────────────────────────────────────── */

/* One occlusion query counting at a time, which is how the NV2A counts, and
 * as many waiting for their answers as a title has tests outstanding.
 *
 * The answer is not waited for. The GPU has it a moment after the draws are
 * handed over, but getting it straight away means handing them over there
 * and then and stopping until they are done, and a title asks a hundred
 * times a frame: Dave Mirra Freestyle BMX 2 lost three to five milliseconds
 * of every frame to that, and now and then twenty-five to a single test. A
 * test is closed with sink_count_end_later and its answer collected with
 * sink_count_result when it has arrived, which is what the console does too.
 *
 * If the answer does not come the test is answered "visible": a title that
 * is told an object is hidden stops drawing it, and one told it is visible
 * merely draws something it did not need to.
 */
#define SINK_COUNT_VISIBLE   0x400u     /* the answer when there is none */
#define SINK_COUNT_WAIT_MS   250u

/* RECOMP_D3D11_COUNT=0 answers every test "visible" without counting, which
 * is what this sink did before it could count: for telling something a test
 * hid from something that was never drawn. */
static int sink_count_off(void)
{
    static int off = -1;

    if (off < 0) {
        const char *e = getenv("RECOMP_D3D11_COUNT");
        off = e && e[0] == '0';
    }
    return off;
}

static void sink_count_begin(void)
{
    ID3D11Device *d3d = d3d8_GetD3D11Device();
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    unsigned i, slot = 0;

    if (!d3d || !ctx || g_count.open || sink_count_off())
        return;
    /* A free query. With none to be had the test goes uncounted, and is
     * answered "visible". */
    for (i = 0; i < SINK_COUNT_QUERIES; i++) {
        slot = (g_count.next + i) % SINK_COUNT_QUERIES;
        if (!g_count.q[slot].pending)
            break;
    }
    if (i == SINK_COUNT_QUERIES)
        return;
    if (!g_count.q[slot].query) {
        D3D11_QUERY_DESC qd;
        qd.Query = D3D11_QUERY_OCCLUSION;
        qd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateQuery(d3d, &qd, &g_count.q[slot].query)))
            g_count.q[slot].query = NULL;
    }
    if (!g_count.q[slot].query)
        return;
    ID3D11DeviceContext_Begin(ctx,
                              (ID3D11Asynchronous *)g_count.q[slot].query);
    g_count.next = (slot + 1) % SINK_COUNT_QUERIES;
    g_count.open = (int)slot + 1;
    g_count.past_edge = 0;
}

/* Close the count and return a ticket for its answer: slot + 1, or 0 when
 * nothing was counting. */
static uint32_t sink_count_end_later(void)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    unsigned slot;

    if (!ctx || !g_count.open)
        return 0;
    slot = (unsigned)g_count.open - 1;
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)g_count.q[slot].query);
    g_count.q[slot].pending = 1;
    g_count.q[slot].past_edge = g_count.past_edge;
    g_count.q[slot].closed_ms = GetTickCount();
    g_count.open = 0;
    g_count.tests++;
    g_ft.tests++;
    return slot + 1;
}

/* Has the ticket's answer arrived? If so store it, in the title's pixels,
 * and the ticket is spent. */
static int sink_count_result(uint32_t ticket, int submit, uint32_t *out)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    unsigned slot = ticket - 1;
    UINT64 samples = 0;
    LONGLONG ft;
    HRESULT hr;
    double pixels;

    *out = SINK_COUNT_VISIBLE;
    if (!ticket || ticket > SINK_COUNT_QUERIES || !g_count.q[slot].pending)
        return 1;
    if (!ctx) {
        g_count.q[slot].pending = 0;
        return 1;
    }
    /* Without the do-not-flush flag the first call sends what is queued to
     * the GPU; after that it is only asked. */
    ft = ft_now();
    hr = ID3D11DeviceContext_GetData(ctx,
            (ID3D11Asynchronous *)g_count.q[slot].query, &samples,
            sizeof samples,
            submit ? 0 : (UINT)D3D11_ASYNC_GETDATA_DONOTFLUSH);
    ft_add(&g_ft.count, ft);
    if (hr != S_OK) {
        if (SUCCEEDED(hr)
                && GetTickCount() - g_count.q[slot].closed_ms
                       <= SINK_COUNT_WAIT_MS)
            return 0;
        if (g_count.timeouts++ == 0) {
            fprintf(stderr, "[NV2A-D3D11] a visibility test got no answer"
                            " in %u ms; answering visible\n",
                    SINK_COUNT_WAIT_MS);
            fflush(stderr);
        }
        g_ft.unanswered++;
        g_count.q[slot].pending = 0;
        return 1;
    }
    g_count.q[slot].pending = 0;

    /* A picture wider than the title's surface shows what lies beside it,
     * and a title knows nothing of that: it tests against its own screen,
     * and what it tests with stops at that screen's edge. Something that
     * reaches the edge may be in plain view just past it, so its test is
     * answered "visible" unless more than that was counted. Without this,
     * windows and signs came and went at the sides of a 16:9 picture as the
     * camera turned. A picture the size of the surface has no "past". */
    if (g_count.q[slot].past_edge && samples < SINK_COUNT_VISIBLE)
        return 1;

    /* Host pixels to the title's: a 640x480 title drawn at 1920x1080 covers
     * about five times as many, and a title compares the count with numbers
     * it chose for its own surface. Something that passed at all still
     * counts as one. */
    pixels = (double)samples;
    if (g_sink.sx > 0.0f && g_sink.sy > 0.0f)
        pixels /= (double)g_sink.sx * (double)g_sink.sy;
    if (samples && pixels < 1.0)
        pixels = 1.0;
    *out = pixels > 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)pixels;
    return 1;
}

/* The same with the wait, for an executor that wants the answer at once. */
static uint32_t sink_count_end(void)
{
    uint32_t ticket = sink_count_end_later(), pixels;

    while (!sink_count_result(ticket, 1, &pixels))
        SwitchToThread();
    return pixels;
}

/* Timed for RECOMP_FRAME_TIMES. */
static void sink_triangles_timed(const Nv2aSinkVertex *v, uint32_t count,
                                 const Nv2aSinkTexture *src,
                                 const Nv2aSinkState *state)
{
    LONGLONG ft = ft_now();

    sink_triangles(v, count, src, state);
    ft_add(&g_ft.draw, ft);
    g_ft.batches++;
}

static const Nv2aPbSink g_sink_vtbl = {
    sink_clear,
    sink_triangles_timed,
    sink_flip,
    sink_count_begin,
    sink_count_end,
    sink_count_end_later,
    sink_count_result,
};

void nv2a_d3d11_sink_start(const char *window_title)
{
    strncpy(g_sink.title, window_title ? window_title : "xboxrecomp",
            sizeof g_sink.title - 1);
    nv2a_pb_exec_set_sink(&g_sink_vtbl);
}

#else  /* !_WIN32 */

void nv2a_d3d11_sink_start(const char *window_title) { (void)window_title; }

#endif
