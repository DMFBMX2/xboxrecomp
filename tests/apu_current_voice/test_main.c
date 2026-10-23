/* Which voice a thread's writes reach.
 *
 * The front end's current voice (SET_CURRENT_VOICE) was one register for the
 * whole device. The title addresses a voice in two steps, select it and
 * write to it, and DirectSound's own code on another thread selects the
 * music's voice in between, so a sound effect's volumes (and format and
 * pitch) landed on the music: it went louder, muffled, or slow.
 *
 * apu_vp.c is compiled into this file, with stand-ins for the little it
 * uses from the rest of the APU. Run with: ctest --test-dir <build> */
#include "../../src/apu/apu_vp.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Symbols the VP takes from the rest of the APU. */
MCPXAPUState *g_state;
struct McpxApuDebug g_dbg, g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4];
uint8_t *g_apu_ram_ptr;

#define RAM_SIZE   0x100000u
#define VOICE_BASE 0x10000u   /* NV_PAPU_VPVADDR */
#define SGE_BASE   0x20000u   /* NV_PAPU_VPSGEADDR */
#define DATA_BASE  0x30000u   /* what SGE entry 0 names */

static uint8_t *s_ram;
static int s_failures;

uint8_t *mcpx_apu_phys(uint64_t addr)
{
    if (addr >= RAM_SIZE) {
        fprintf(stderr, "FAIL: guest access at 0x%llX is outside the fake RAM\n",
                (unsigned long long)addr);
        exit(1);
    }
    return s_ram + addr;
}

const char *mcpx_apu_phys_class(uint64_t addr) { (void)addr; return "fake"; }
int apu_env_on(const char *name) { (void)name; return 0; }
void mcpx_apu_publish_irq(MCPXAPUState *d) { (void)d; }
__declspec(thread) uint32_t g_esp;
uint32_t g_xbox_code_lo, g_xbox_code_hi;
ptrdiff_t g_xbox_mem_offset;

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); s_failures++; } } while (0)

/* The title's thread selects voice 3; another thread selects voice 5 before
 * the first writes its volumes. They must reach voice 3. */
static MCPXAPUState s_d;
static HANDLE s_go, s_done;

static DWORD WINAPI select_voice_5(LPVOID arg)
{
    (void)arg;
    WaitForSingleObject(s_go, INFINITE);
    mcpx_apu_vp_write(&s_d, NV1BA0_PIO_SET_CURRENT_VOICE, 5, 4);
    SetEvent(s_done);
    return 0;
}

static void current_voice_is_per_thread(void)
{
    MCPXAPUState *d = &s_d;
    const uint32_t vol = 0x12345678u;
    HANDLE t;

    memset(s_ram, 0, RAM_SIZE);
    memset(d, 0, sizeof(*d));
    d->regs[NV_PAPU_VPVADDR] = VOICE_BASE;
    s_go = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    t = CreateThread(NULL, 0, select_voice_5, NULL, 0, NULL);

    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_CURRENT_VOICE, 3, 4);
    SetEvent(s_go);
    WaitForSingleObject(s_done, 1000);
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_VOICE_TAR_VOLA, vol, 4);

    CHECK(voice_get_mask(d, 3, NV_PAVS_VOICE_TAR_VOLA, 0xFFFFFFFF) == vol,
          "a write after SET_CURRENT_VOICE 3 did not reach voice 3");
    CHECK(voice_get_mask(d, 5, NV_PAVS_VOICE_TAR_VOLA, 0xFFFFFFFF) == 0,
          "another thread's SET_CURRENT_VOICE took this thread's write");
    WaitForSingleObject(t, 1000);
    CloseHandle(t);
    CloseHandle(s_go);
    CloseHandle(s_done);
}

/* The same for the table a streaming voice reads its data through: one
 * thread selects an SSL page and writes its segments, another selects its
 * own page in between. The first's segment must be written to its page. */
static DWORD WINAPI select_ssl_page_80(LPVOID arg)
{
    (void)arg;
    WaitForSingleObject(s_go, INFINITE);
    mcpx_apu_vp_write(&s_d, NV1BA0_PIO_SET_CURRENT_SSL, 0x80, 4);
    SetEvent(s_done);
    return 0;
}

static void ssl_page_is_per_thread(void)
{
    MCPXAPUState *d = &s_d;
    const uint32_t seg = 0xCAFEF00Du;
    const hwaddr ssl_base = 0x40000u;
    uint32_t at_40, at_80;
    HANDLE t;

    memset(s_ram, 0, RAM_SIZE);
    memset(d, 0, sizeof(*d));
    d->regs[NV_PAPU_VPSSLADDR] = ssl_base;
    s_go = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    t = CreateThread(NULL, 0, select_ssl_page_80, NULL, 0, NULL);

    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_CURRENT_SSL, 0x40, 4);
    SetEvent(s_go);
    WaitForSingleObject(s_done, 1000);
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET, seg, 4);

    memcpy(&at_40, s_ram + ssl_base + 0x40 * 8, 4);
    memcpy(&at_80, s_ram + ssl_base + 0x80 * 8, 4);
    CHECK(at_40 == seg, "a segment written after SET_CURRENT_SSL 0x40 did "
                        "not reach page 0x40");
    CHECK(at_80 == 0, "another thread's SET_CURRENT_SSL took this thread's "
                      "segment");
    WaitForSingleObject(t, 1000);
    CloseHandle(t);
    CloseHandle(s_go);
    CloseHandle(s_done);
}

int main(void)
{
    s_ram = (uint8_t *)calloc(1, RAM_SIZE);
    if (!s_ram) return 2;

    current_voice_is_per_thread();
    ssl_page_is_per_thread();

    free(s_ram);
    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    puts("PASS: each thread's writes reach the voice and the SSL page it "
         "selected.");
    return 0;
}
