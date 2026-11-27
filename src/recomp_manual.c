/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>   /* getenv */
#include <string.h>
#include <math.h>     /* shadow light camera angle */
#ifdef _WIN32
#include <intrin.h>
#else
#include <emmintrin.h>
#include "platform/xbox_winnt.h"
#endif

#if defined(_MSC_VER)
#define RECOMP_TLS __declspec(thread)
#else
#define RECOMP_TLS _Thread_local
#endif

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern RECOMP_TLS uint32_t g_eax;   /* thread-local, like the whole register file */
extern ptrdiff_t g_xbox_mem_offset;

/* The D3D fence-wait wrapper and fence-rescue thread from main are not
 * here: on the aknavj toolkit, native fences (RECOMP_NV2A_NATIVE_FENCES)
 * write the semaphore through the GPU's own DMA context instead. */

#define TUROK_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define TUROK_D3D_DEVICE_PTR   0x001A1AD0u

/* Thread-local like the rest of the register file; see main.c. */
extern RECOMP_TLS uint32_t g_esp;

/* ── D3D GetVisibilityTestResult (sub_00194B40) ────────────── */

/*
 * GetVisibilityTestResult(index, *result, *timestamp) finds the test's 16-byte
 * report slot (table at device+0x39C, 256 slots per page) and returns
 * D3DERR_TESTINCOMPLETE (0x88760828) while the slot's status word is still
 * 0xFFFFFFFF, i.e. until the GPU has executed the GET_REPORT.
 *
 * Turok tests its foliage clusters (palm crowns, bushes) this way and leaves
 * out any whose result is not in. The pushbuffer executor runs behind the
 * title, so some results were not in yet, and the foliage came and went from
 * frame to frame. The toolkit answers every test "fully visible" anyway (it
 * has no real occlusion counter on the D3D11 path), so an unfinished test is
 * given that same answer now rather than "not yet": the slot is completed as
 * visible and the original function reads it. The executor never writes the
 * report itself: here D3D's report page sits above the 16 MB its 24-bit
 * offset reaches (0x01A89000, reported as 0xA89000), so a write would land
 * 16 MB too low, on other data.
 */
extern void sub_00194B40_gen(void);

void sub_00194B40(void)
{
    uint32_t index = TUROK_MEM32(g_esp + 4);
    uint32_t dev = TUROK_MEM32(TUROK_D3D_DEVICE_PTR);
    uint32_t page = dev ? TUROK_MEM32(dev + 0x39C + (index >> 8) * 4) : 0;

    if (page) {
        uint32_t slot = page + (index & 0xFFu) * 16u;
        if (TUROK_MEM32(slot + 0xC) == 0xFFFFFFFFu) {
            TUROK_MEM32(slot + 0x0) = 0;           /* timestamp */
            TUROK_MEM32(slot + 0x4) = 0;
            TUROK_MEM32(slot + 0x8) = 640u * 480u; /* pixels passed: all */
            TUROK_MEM32(slot + 0xC) = 0;           /* done */
        }
    }
    sub_00194B40_gen();
}

/* ── DirectSound idle-voice notification (sub_001B23DD) ────── */

/*
 * The APU interrupt handler calls this with the handle of a voice the voice
 * processor found inactive in its lists (SE2FE_IDLE_VOICE). It looks up the
 * voice's owner, this->voices[handle] at +0x330, and unlinks it
 * (sub_001B34BF). The emulated APU walks the lists on its own thread while
 * the title edits them, so it can report a handle nobody owns -- voice 0 with
 * a NULL owner -- which hardware never does; the unlink then dereferences
 * NULL and the process dies (most often when a level is quit and every voice
 * is released at once). An unowned handle has nothing to unlink: ignore it.
 */
extern RECOMP_TLS uint32_t g_ecx;
extern void sub_001B23DD_gen(void);
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *g_apu_state;
extern int mcpx_apu_voice_idle_begin(MCPXAPUState *d, uint32_t voice);
extern void mcpx_apu_voice_idle_end(MCPXAPUState *d, uint32_t voice);

void sub_001B23DD(void)
{
    uint32_t handle = TUROK_MEM32(g_esp + 4);

    if (handle < 0x100u && TUROK_MEM32(g_ecx + 0x330u + handle * 4u) == 0) {
        g_esp += 8;   /* ret 4 */
        return;
    }
    /* The trap can be consumed here from either the ISR or synchronous
     * DirectSound work, after a stop/restart has made its idle report stale.
     * Retiring it then unlinks the new shot, leaving an active silent voice. */
    if (!mcpx_apu_voice_idle_begin(g_apu_state, handle)) {
        g_esp += 8;
        return;
    }
    sub_001B23DD_gen();
    mcpx_apu_voice_idle_end(g_apu_state, handle);
}

/*
 * The same handler's pass over the voices to service (sub_001B22DA, this =
 * DirectSound) walks three lists at +0x730 (8 bytes a head) and empties the
 * one at +0x748, calling each entry's vfunc +0x14. With the title relinking
 * them on its own thread, a head was once left pointing at 0xFE0001F0 --
 * not memory -- and the call through it took the process down. Each list is
 * checked first: a link outside RAM, or one that does not come back to its
 * head, empties that list (its voices miss this notification) instead.
 */
extern void sub_001B22DA_gen(void);
extern size_t g_xbox_total_ram;

/* In RAM, or in its kernel mirror at 0x80000000 (where DirectSound's own
 * allocations sit). */
static int dsound_link_ok(uint32_t link)
{
    uint32_t ram = (uint32_t)g_xbox_total_ram, low = link & 0x7FFFFFFFu;
    return !(link & 3u) && low >= 0x10000u && low < ram && (link < ram || link >= 0x80000000u);
}

void sub_001B22DA(void)
{
    static const uint32_t heads[] = {0x730u, 0x738u, 0x740u, 0x748u};
    uint32_t self = g_ecx;
    unsigned i;
    for (i = 0; i < sizeof heads / sizeof *heads; i++) {
        uint32_t head = self + heads[i], link = TUROK_MEM32(head), steps = 0;
        while (link != head && dsound_link_ok(link) && steps++ < 4096u) link = TUROK_MEM32(link);
        if (link != head) {
            fprintf(stderr, "[DSOUND] list +0x%X links to %08X; emptied\n", heads[i], link);
            TUROK_MEM32(head) = head;
            TUROK_MEM32(head + 4u) = head;
        }
    }
    g_ecx = self;
    sub_001B22DA_gen();
}

/* The drain of +0x748 itself pops each entry with sub_001AE520 (stdcall,
 * the node; unlinks it and returns it, from 0x1B2334) and calls its object's
 * vfunc +0x14 (the object is the node - 0x54). Crashes have still come from
 * an entry whose object was freed and reused by then: a valid link to an
 * object that is no longer a voice, whose slot 5 is AddRef (sub_001AEB48),
 * which reads the kernel thunk at 0xFE00019C and takes the process down. So
 * each entry is checked as it is popped: its object must be one of
 * DirectSound's voices (the handle table at +0x330, 256 entries) or have a
 * vtable a voice there has -- a voice whose handle is already released
 * still gets its notification. Anything else is unlinked (without writing
 * into the object) and skipped. */
extern void sub_001AE520_gen(void);
extern RECOMP_TLS uint32_t g_edi, g_esi;

static int dsound_voice_ok(uint32_t ds, uint32_t obj)
{
    static uint32_t tables[16];
    static int count;
    uint32_t vt, h, i;

    if (!dsound_link_ok(obj)) return 0;
    vt = TUROK_MEM32(obj);
    if (!dsound_link_ok(vt & 0x7FFFFFFFu) || TUROK_MEM32(vt + 0x14u) == 0x001AEB48u) return 0;
    for (h = 0; h < 0x100u; h++) {
        uint32_t voice = TUROK_MEM32(ds + 0x330u + h * 4u), t;
        if (voice == obj) return 1;
        if (!voice || !dsound_link_ok(voice)) continue;
        t = TUROK_MEM32(voice);
        for (i = 0; i < (uint32_t)count && tables[i] != t; i++) {}
        if (i == (uint32_t)count && count < 16 && TUROK_MEM32(t + 0x14u) != 0x001AEB48u) tables[count++] = t;
    }
    for (i = 0; i < (uint32_t)count; i++)
        if (tables[i] == vt) return 1;
    return 0;
}

void sub_001AE520(void)
{
    if (TUROK_MEM32(g_esp) == 0x001B2339u && g_edi == g_esi + 0x748u) {
        uint32_t ds = g_esi, head = g_edi, node = TUROK_MEM32(g_esp + 4u), guard = 0;
        while (node != head && !dsound_voice_ok(ds, node - 0x54u) && guard++ < 256u) {
            uint32_t next = TUROK_MEM32(node), prev = TUROK_MEM32(node + 4u);
            fprintf(stderr, "[DSOUND] skipped a finished-voice entry that is not a voice (object %08X)\n",
                    node - 0x54u);
            if (dsound_link_ok(next) && dsound_link_ok(prev) &&
                TUROK_MEM32(prev) == node && TUROK_MEM32(next + 4u) == node) {
                TUROK_MEM32(prev) = next;
                TUROK_MEM32(next + 4u) = prev;
            } else {
                TUROK_MEM32(head) = head;
                TUROK_MEM32(head + 4u) = head;
            }
            node = TUROK_MEM32(head);
        }
        TUROK_MEM32(g_esp + 4u) = node;
    }
    sub_001AE520_gen();
}

/* ── D3D KickOff without the write-combine handshake ───────────
 *
 * Before publishing DMA_PUT, KickOff (sub_001984B0) and its flush helper
 * (sub_00198480) set PFB 0x100410 bit 16 and spin until the GPU clears it:
 * the write-combining buffers have drained into memory. Here memory is
 * coherent and the bit is cleared by the toolkit's GPU thread only between
 * pushbuffer packets, so the game thread sat out whatever that thread was
 * doing -- a seventh of the frame in busy levels. The handshake is skipped:
 * a store barrier orders the pushbuffer before PUT, which is all the flush
 * guaranteed. KickOff's other cases (the device flag 0x2000 path and the
 * synchronous-debug wait on [0x1A1AD4]) run the original. */
extern void sub_001984B0_gen(void);
extern void sub_00198480_gen(void);
extern void sub_0019D580_gen(void);

/* D3D push-buffer fixups, natively (types 3 and 4 of CMiniport::
 * SoftwareMethod, sub_0019D840 -> sub_0019D7C0). The parameter's upper bits
 * (>> 5, | 0x80000000) address a descriptor {fixup list, buffer base, end
 * offset, resume address}; each fixup {size, offset, bytes} is copied into
 * the buffer (until a size of -1), then the buffer's end gets an old-style
 * JUMP to the resume address. Type 3 also loads the resume address into
 * PFIFO DMA_SUBROUTINE (registers +0x324C) and adds the end offset to the
 * miniport's counter (+0x818); its trailing write-combine flush
 * (sub_00198480) is a no-op here. Type 10 (the return from a pushbuffer
 * call) is done here too. Together they are nearly every software method
 * D3D issues -- hundreds a frame, each otherwise queued to the timer thread
 * and waited for. Run here, on the executor, the stream waits for them as
 * the GPU does. RECOMP_SWM_NATIVE=0 sends them back. */
#define TUROK_MINIPORT 0x001A4350u

int turok_software_method_native(uint32_t parameter)
{
    uint32_t type = parameter & 0x1Fu;
    uint32_t descriptor, list, base, end, target;
    if (type == 10) {
        /* The return from a pushbuffer call: the counter takes the call's
         * size, DMA_SUBROUTINE the address register 0x401A88 holds. */
        uint32_t registers = TUROK_MEM32(TUROK_MINIPORT);
        TUROK_MEM32(TUROK_MINIPORT + 0x818u) += parameter >> 5;
        TUROK_MEM32(registers + 0x324Cu) = TUROK_MEM32(registers + 0x401A88u);
        return 1;
    }
    if (type == 5) {
        /* RECOMP_BLOCK_WATCH_TEST=<n>: lose every n-th BlockOnTime wake-up,
         * as the GET race does (see d3d_wait.c), to exercise the watcher. */
        static int every = -1;
        static unsigned count;
        if (every < 0) {
            const char *value = getenv("RECOMP_BLOCK_WATCH_TEST");
            every = value ? atoi(value) : 0;
        }
        return every > 0 && ++count % (unsigned)every == 0;
    }
    if (type != 3 && type != 4) return 0;
    descriptor = (parameter >> 5) | 0x80000000u;
    list = TUROK_MEM32(descriptor);
    base = TUROK_MEM32(descriptor + 4u);
    if (list) {
        uint32_t size;
        while ((size = TUROK_MEM32(list)) != 0xFFFFFFFFu) {
            uint32_t offset = TUROK_MEM32(list + 4u);
            memmove((void *)((uintptr_t)(base + offset) + g_xbox_mem_offset),
                    (const void *)((uintptr_t)(list + 8u) + g_xbox_mem_offset), size);
            list += 8u + size;
        }
    }
    end = TUROK_MEM32(descriptor + 8u);
    target = TUROK_MEM32(descriptor + 12u) & 0x0FFFFFFFu;
    TUROK_MEM32(base + end) = target + 1u;
    if (type == 3) {
        TUROK_MEM32(TUROK_MEM32(TUROK_MINIPORT) + 0x324Cu) = target;
        TUROK_MEM32(TUROK_MINIPORT + 0x818u) += end;
    }
    _mm_mfence();
    return 1;
}

/* The miniport's vblank handler (sub_0019D580, this = miniport, [this] = the
 * NV2A register base) acknowledges PCRTC_INTR and then spins until PMC_INTR
 * drops its PCRTC bit (0x1000000), which hardware does at once. Here only the
 * toolkit's ack thread clears it, between pushbuffer walks, so the DPC -- on
 * the timer thread, which runs every DPC and timer -- spun for most of a
 * core while the game waited on events it would have signalled. Its caller
 * (DPC sub_0019DD40) has already read the bit and chosen to handle the blank,
 * and the handler does not read it before the spin: drop it first. */
void sub_0019D580(void)
{
    uint32_t regs = TUROK_MEM32(g_ecx);
    if (regs)
        TUROK_MEM32(regs + 0x100u) &= ~0x01000000u;
    sub_0019D580_gen();
}

/* BlockOnTime (sub_00198650, stdcall: GPU time, flags) with a watcher that
 * releases a wait whose wake-up was lost: d3d_wait.c. */
extern void sub_00198650_gen(void);
extern int d3d_wait_begin(uint32_t time, uint32_t outer[3]);
extern void d3d_wait_end(const uint32_t outer[3]);

#ifndef _WIN32
extern void d3d_wait_paced(uint32_t time);
#endif

void sub_00198650(void)
{
    uint32_t outer[3];
    int watched;
#ifndef _WIN32
    d3d_wait_paced(TUROK_MEM32(g_esp + 4u));
#endif
    watched = d3d_wait_begin(TUROK_MEM32(g_esp + 4u), outer);
    sub_00198650_gen();
    if (watched) d3d_wait_end(outer);
}

void sub_00198480(void)
{
    _mm_mfence();
    g_esp += 4;   /* ret */
}

void sub_001984B0(void)
{
    uint32_t dev = g_ecx;
    uint32_t flags = TUROK_MEM32(dev + 8u);

    if ((flags & 0x2000u) || TUROK_MEM32(0x001A1AD4u)) {
        sub_001984B0_gen();
        return;
    }
    {
        uint32_t put = (flags & 4u) ? TUROK_MEM32(dev + 0x35Cu) : TUROK_MEM32(dev);
        TUROK_MEM32(dev + 0x2Cu) = put;
        _mm_mfence();
        TUROK_MEM32(TUROK_MEM32(dev + 0x23BCu) + 0x40u) = put & 0x0FFFFFFFu;
        g_eax = 0;
        g_esp += 4;   /* ret */
    }
}

/* ── Shadow manager defaults (sub_0017D5C0) ────────────────── */

/*
 * The shadow manager's base constructor sets the shadow map to 512x512
 * (+0x400/+0x404) and the shadow distance to 16 units (+0x40C): the map
 * covers a region that far ahead of the camera (sub_0017DAF0 centres the
 * light camera half that distance along the view direction and sizes it by
 * the distance), so a creature further away casts nothing. The XBOX
 * subclass then creates its render target from the size fields.
 *
 * With Shadows set to enhanced (video_settings_enhanced_shadows) the map is
 * 2048 and the distance 64 (RECOMP_SHADOW_MAP, a power of two from 512 to
 * 4096, and RECOMP_SHADOW_DISTANCE replace them): shadows reach four times
 * as far (see sub_0017DAF0 below for the area around the player). Xbox keeps
 * the game's own. The distance follows the setting at once (written at each
 * fit, sub_0017DAF0); the map size when the manager is next made.
 */
extern void sub_0017D5C0_gen(void);
extern int video_settings_enhanced_shadows(void);

static float s_shadow_distance;        /* enhanced shadows' distance, or 0 */
static float s_game_shadow_distance;   /* the game's own (16) */

void sub_0017D5C0(void)
{
    uint32_t self = g_ecx;
    const char *distance_text = getenv("RECOMP_SHADOW_DISTANCE");
    const char *map_text = getenv("RECOMP_SHADOW_MAP");
    float distance = distance_text ? (float)atof(distance_text) : 64.0f;
    uint32_t map = map_text ? (uint32_t)strtoul(map_text, NULL, 0) : 2048u;

    sub_0017D5C0_gen();
    memcpy(&s_game_shadow_distance, (void *)((uintptr_t)(self + 0x40Cu) + g_xbox_mem_offset), 4);
    s_shadow_distance = distance > 0.0f && distance < 10000.0f ? distance : s_game_shadow_distance;
    if (video_settings_enhanced_shadows() && map >= 512u && map <= 4096u && !(map & (map - 1u))) {
        TUROK_MEM32(self + 0x400u) = map;
        TUROK_MEM32(self + 0x404u) = map;
    }
    fprintf(stderr, "[SHADOW] shadow manager: %ux%u map, %s shadows\n", TUROK_MEM32(self + 0x400u),
            TUROK_MEM32(self + 0x404u), video_settings_enhanced_shadows() ? "enhanced" : "xbox");
}

/* ── Shadow area around the player (sub_0017DAF0) ──────────────
 *
 * The camera-fitted light camera (light camera at shadow manager +0x10, eye
 * +0x204, target +0x210) looks at a point half the shadow distance ahead
 * of the view and is given just the angle that reaches the player
 * (sub_0001FD20 sets it, radians at camera +0xEC): the shadowed area is a
 * disc with the player on its rim. Anything beside or behind the player --
 * and, in widescreen, near the sides of the view -- is off the map and
 * casts nothing. With enhanced shadows the angle is widened so the disc also
 * reaches RECOMP_SHADOW_AROUND units past the player: 12 outdoors, 32 in the
 * indoor levels (video_settings_indoor_level) -- under a roof, where the
 * whole room is in shadow, the disc's rim shows as a straight lit edge, and
 * at 21:9 one 12 units out was in view at the sides. Outdoors the smaller
 * disc keeps the map's detail. RECOMP_SHADOW_AROUND sets both. */
extern void sub_0017DAF0_gen(void);
extern void sub_0001FD20_gen(void);
extern int video_settings_indoor_level(void);

static uint32_t s_shadow_fit;   /* the shadow manager while it fits its camera */

static float guest_float(uint32_t va)
{
    float value;
    uint32_t bits = TUROK_MEM32(va);
    memcpy(&value, &bits, 4);
    return value;
}

void sub_0017DAF0(void)
{
    s_shadow_fit = g_ecx;
    if (s_game_shadow_distance > 0.0f) {
        float distance = video_settings_enhanced_shadows() ? s_shadow_distance : s_game_shadow_distance;
        memcpy((void *)((uintptr_t)(g_ecx + 0x40Cu) + g_xbox_mem_offset), &distance, 4);
    }
    sub_0017DAF0_gen();
    s_shadow_fit = 0;
}

void sub_0001FD20(void)
{
    static float set = -2.0f;   /* RECOMP_SHADOW_AROUND, or -1 for the defaults */
    uint32_t self = s_shadow_fit;
    float around;

    if (set < -1.5f) {
        const char *text = getenv("RECOMP_SHADOW_AROUND");
        set = text ? (float)atof(text) : -1.0f;
        if (text && !(set >= 0 && set < 1000.0f)) set = 0;
    }
    around = !video_settings_enhanced_shadows() ? 0.0f : set >= 0 ? set : video_settings_indoor_level() ? 32.0f : 12.0f;
    if (self && g_ecx == self + 0x10u && around > 0) {
        float dx = guest_float(self + 0x210u) - guest_float(self + 0x204u);
        float dy = guest_float(self + 0x214u) - guest_float(self + 0x208u);
        float dz = guest_float(self + 0x218u) - guest_float(self + 0x20Cu);
        float range = sqrtf(dx * dx + dy * dy + dz * dz);
        float angle = guest_float(g_esp + 4);
        if (range > 0.001f && angle > 0 && angle < 3.0f) {
            float radius = range * tanf(angle * 0.5f) + around;
            uint32_t bits;
            angle = 2.0f * atanf(radius / range);
            memcpy(&bits, &angle, 4);
            TUROK_MEM32(g_esp + 4) = bits;
        }
    }
    sub_0001FD20_gen();
}

/* ── Shadows of models out of view (sub_000F21B0) ──────────────
 *
 * Before a model's shadow is drawn the manager asks this whether the model
 * passed one of its visibility tests (occlusion queries, polled every
 * fourth frame, sub_00194B40). Only a model drawn on screen issues them, so
 * one just out of view -- whose shadow may well be in view -- casts none.
 * Xbox shadows keep that; enhanced ones cast every model the manager offers,
 * and the light camera clips the rest (RECOMP_SHADOW_OFFSCREEN=0: not). */
extern void sub_000F21B0_gen(void);

/* Its caller, the shadow pass's actor loop in sub_0017E2D0, holds the actor
 * in esi (position +0x38/+0x3C/+0x40, y up). With enhanced shadows the
 * ambient birds -- FlyingIndigenousAI, vtable 0x21A240 (made by
 * sub_0012D370) -- cast none once they are more than RECOMP_SHADOW_BIRD_HEIGHT
 * units (default 10) above the shadowed area's centre (the manager's target,
 * +0x210, at the player's height): circling high over the levels as set
 * dressing, their shadows landed far below them as stray blobs. Birds
 * flushed from the brush (the birds that burst up at the start of the first
 * level) always cast theirs, however high they climb. Flying enemies
 * (FlyingEnemyAI) always cast. RECOMP_SHADOW_BIRDS=1: every bird does. */
extern RECOMP_TLS uint32_t g_esi;
#define FLYING_INDIGENOUS_AI_VTABLE 0x0021A240u

/* A bird sitting in the brush is a "Ground" type (BirdForm +0x7F0 0, its
 * flying type in AlternateType +0x7EC). Flushed, it is replaced
 * (sub_0012C9D0) by a new actor of the flying type, made by sub_0003DC30 --
 * the same type as the set-dressing birds overhead. The actors made there
 * are remembered as flushed for the rest of the level. */
extern void sub_0012C9D0_gen(void);
extern void sub_0003DC30_gen(void);
#define FLUSHED_BIRDS 64
static uint32_t s_flushed[FLUSHED_BIRDS];
static unsigned s_flushed_next;
static int s_flushing;

void flushed_birds_reset(void)
{
    memset(s_flushed, 0, sizeof s_flushed);
    s_flushed_next = 0;
}

static int bird_was_flushed(uint32_t actor)
{
    unsigned i;
    for (i = 0; i < FLUSHED_BIRDS; i++)
        if (s_flushed[i] == actor) return 1;
    return 0;
}

void sub_0012C9D0(void)
{
    s_flushing++;
    sub_0012C9D0_gen();
    s_flushing--;
}

void sub_0003DC30(void)
{
    sub_0003DC30_gen();
    if (s_flushing && g_eax) {
        static unsigned reported;
        if (reported < 16) {
            reported++;
            fprintf(stderr, "[BIRD] flushed from the brush: %08X keeps its shadow\n", g_eax);
        }
        s_flushed[s_flushed_next] = g_eax;
        s_flushed_next = (s_flushed_next + 1) % FLUSHED_BIRDS;
    }
}

void sub_000F21B0(void)
{
    static int offscreen = -1, birds = -1;
    static float bird_height = 10.0f;
    uint32_t actor = g_esi, manager = g_ecx;
    if (offscreen < 0) {
        const char *text = getenv("RECOMP_SHADOW_OFFSCREEN");
        offscreen = !text || atoi(text) != 0;
        birds = getenv("RECOMP_SHADOW_BIRDS") && atoi(getenv("RECOMP_SHADOW_BIRDS")) != 0;
        if (getenv("RECOMP_SHADOW_BIRD_HEIGHT")) bird_height = (float)atof(getenv("RECOMP_SHADOW_BIRD_HEIGHT"));
    }
    sub_000F21B0_gen();
    if (!video_settings_enhanced_shadows()) return;   /* Xbox shadows: the game's verdict */
    if (offscreen) g_eax = 1;
    if (!birds && actor && TUROK_MEM32(actor) == FLYING_INDIGENOUS_AI_VTABLE &&
            guest_float(actor + 0x3Cu) - guest_float(manager + 0x214u) > bird_height &&
            !bird_was_flushed(actor))
        g_eax = 0;
}

/* ── Club hold (the war club): straight to the charged strike (PC Settings) ──
 *
 * The war club (WarClubObject, vtable 0x215F10; its states by index: 6
 * Idle, 7 Fire, 9/11/13 the combos, 15 Charge) takes the attack button
 * through three methods: pressed (vfunc +0x18C, sub_00123D10) -- from Idle
 * the swing (Fire) at once, during a swing the next combo queued; held
 * (+0x194, sub_00123C00) -- a timer started; released (+0x190, sub_00123DC0)
 * -- the charged strike's return if it was held long enough, else, from
 * Idle, the swing. Its update (+0x8, sub_00123B50) turns a hold of a second
 * into the charged strike (Charge). So a hold always swung first, the charge
 * coming after (issue #22). With Club hold "charge", a press from Idle
 * does nothing: a tap swings when released (the release's own path), and a
 * hold goes straight to the charged strike. Combos, pressed during a swing,
 * are as before. Off by default (the Xbox's). */
extern void sub_00123D10_gen(void);
extern int video_settings_club_charge(void);

void sub_00123D10(void)
{
    uint32_t club = g_ecx, state = club ? TUROK_MEM32(club + 0x12Cu) : 0u;
    if (video_settings_club_charge() && state && TUROK_MEM32(state) == 6u) {
        g_esp += 4u;   /* ret */
        return;
    }
    sub_00123D10_gen();
}

/* ── Rumble from the player's own gun, measured from the eyes ───
 *
 * The scripted Rumble event (vtable 0x204CAC, execute sub_000A3770) shakes
 * the pads near an actor through sub_000549D0 -> sub_00054120 (this-call:
 * strength, ?, radius, ?, position; ret 0x14): each player in the world's
 * list (+0x118..+0x11C) that is locally controlled (sub_00038C50) and
 * within the radius gets sub_0002C600 (cdecl: pad, motor, strength,
 * duration), unless its pad's vibration is off ([[0x250A84]+0xA4]+0xC).
 *
 * The pistol fires one at its muzzle with a radius of 2. A multiplayer
 * character's position is high on its body, 0.7 to 1.0 from the muzzle, so
 * every shot rumbles; the single-player character's (vtable 0x222CE8) is its
 * feet, 2.13 below the muzzle standing -- outside the radius, so its pistol
 * rumbled only when the gun dipped (issue: no vibration from the pistol in
 * single player). A rumble from right at a single-player character -- within
 * 1 unit across and no higher than half a unit over its eyes (EYEHEIGHT,
 * +0x860): its own held weapon -- is measured from the eyes instead: the
 * height is raised for the call and put back. RECOMP_GUN_RUMBLE_FIX=0 keeps
 * the feet. RECOMP_RUMBLE_TRACE=1 logs each positional rumble with every
 * player's distance, and every pad rumble with its caller. */
extern void sub_00054120_gen(void);
extern void sub_0002C600_gen(void);
#define RUMBLE_MEM8(va) (*(volatile uint8_t *)((uintptr_t)(va) + g_xbox_mem_offset))
static int rumble_trace(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_RUMBLE_TRACE") != NULL;
    return on;
}

void sub_00054120(void)
{
    if (rumble_trace()) {
        uint32_t world = g_ecx, position = TUROK_MEM32(g_esp + 0x14u), list = TUROK_MEM32(world + 0x118u),
                 end = TUROK_MEM32(world + 0x11Cu), at;
        float radius = guest_float(g_esp + 0xCu);
        fprintf(stderr, "[RUMBLE] at %.2f %.2f %.2f: strength %.3f, %.3f, radius %.2f, %.3f; from %06X\n",
                position ? guest_float(position) : 0.0f, position ? guest_float(position + 4u) : 0.0f,
                position ? guest_float(position + 8u) : 0.0f, guest_float(g_esp + 4u), guest_float(g_esp + 8u), radius,
                guest_float(g_esp + 0x10u), TUROK_MEM32(g_esp));
        for (at = list; list && at < end && at < list + 64u; at += 4u) {
            uint32_t player = TUROK_MEM32(at), control = player ? TUROK_MEM32(player + 0x210u) : 0u;
            float dx = 0, dy = 0, dz = 0;
            if (player && position) {
                dx = guest_float(position) - guest_float(player + 0x38u);
                dy = guest_float(position + 4u) - guest_float(player + 0x3Cu);
                dz = guest_float(position + 8u) - guest_float(player + 0x40u);
            }
            fprintf(stderr, "[RUMBLE]   player %08X (vtable %08X%s) at %.2f %.2f %.2f, distance %.2f; control %08X: "
                            "%02X %02X %02X pad %d\n",
                    player, player ? TUROK_MEM32(player) : 0u, player == TUROK_MEM32(0x257E90u) ? ", you" : "",
                    player ? guest_float(player + 0x38u) : 0.0f, player ? guest_float(player + 0x3Cu) : 0.0f,
                    player ? guest_float(player + 0x40u) : 0.0f, sqrtf(dx * dx + dy * dy + dz * dz), control,
                    control ? RUMBLE_MEM8(control) : 0u, control ? RUMBLE_MEM8(control + 1u) : 0u,
                    control ? RUMBLE_MEM8(control + 2u) : 0u, control ? (int)TUROK_MEM32(control + 4u) : -1);
        }
    }
    {
        static int enabled = -1;
        uint32_t world = g_ecx, position = TUROK_MEM32(g_esp + 0x14u), list = TUROK_MEM32(world + 0x118u),
                 end = TUROK_MEM32(world + 0x11Cu), at, raised[16], saved[16];
        unsigned count = 0, i;
        if (enabled < 0) enabled = !(getenv("RECOMP_GUN_RUMBLE_FIX") && *getenv("RECOMP_GUN_RUMBLE_FIX") == '0');
        for (at = list; enabled && position && list && at < end && count < 16u; at += 4u) {
            uint32_t player = TUROK_MEM32(at);
            float feet, eye, dx, dz, up, height;
            uint32_t bits;
            if (!player || TUROK_MEM32(player) != 0x00222CE8u) continue;
            feet = guest_float(player + 0x3Cu);
            eye = guest_float(player + 0x860u);
            dx = guest_float(position) - guest_float(player + 0x38u);
            dz = guest_float(position + 8u) - guest_float(player + 0x40u);
            up = guest_float(position + 4u) - feet;
            if (!(eye > 0.0f && eye < 10.0f) || dx * dx + dz * dz > 1.0f || up < 0.0f || up > eye + 0.5f) continue;
            height = feet + eye;
            memcpy(&bits, &height, 4);
            raised[count] = player;
            saved[count] = TUROK_MEM32(player + 0x3Cu);
            TUROK_MEM32(player + 0x3Cu) = bits;
            count++;
        }
        sub_00054120_gen();
        for (i = 0; i < count; i++) TUROK_MEM32(raised[i] + 0x3Cu) = saved[i];
    }
}

void sub_0002C600(void)
{
    if (rumble_trace())
        fprintf(stderr, "[RUMBLE] pad %d motor %u strength %.3f duration %.3f from %06X\n",
                (int)TUROK_MEM32(g_esp + 4u), TUROK_MEM32(g_esp + 8u), guest_float(g_esp + 0xCu),
                guest_float(g_esp + 0x10u), TUROK_MEM32(g_esp));
    sub_0002C600_gen();
}

/* ── Minigun spin-up pitch at any frame rate ────────────────────
 *
 * The minigun's update (sub_0011F020, this-call (seconds), ret 4) moves its
 * spin sound's pitch (+0x2F8, from -25 when equipped) a fixed step a frame
 * while the barrels turn fast: +5 in the spin state (0xB), -12 in the
 * others, and hands it to the sound (sub_00085370, returning to 0x11F17D;
 * the mixer clamps it to +-4800). Every other part of its spin follows the
 * frame time; this step does not, so above 60 fps the spin-up rose faster
 * and higher the higher the frame rate (issue: the minigun pitches up with
 * the frame rate). Above 60 fps the step is scaled to the frame time, as
 * many a second as at 60 (the Xbox ran at 60 or less): the change the update
 * made is rescaled just before it reaches the sound. At 60 fps and below
 * nothing changes. RECOMP_MINIGUN_PITCH_FIX=0 keeps the per-frame step. */
extern void sub_0011F020_gen(void);
extern void sub_00085370_gen(void);
static uint32_t s_minigun;
static float s_minigun_pitch, s_minigun_scale;

void sub_0011F020(void)
{
    static int enabled = -1;
    float seconds = guest_float(g_esp + 4u);
    if (enabled < 0) enabled = !(getenv("RECOMP_MINIGUN_PITCH_FIX") && *getenv("RECOMP_MINIGUN_PITCH_FIX") == '0');
    s_minigun = g_ecx;
    s_minigun_pitch = guest_float(g_ecx + 0x2F8u);
    s_minigun_scale = enabled && seconds > 0.0f && seconds < 1.0f / 60.0f ? seconds * 60.0f : 1.0f;
    sub_0011F020_gen();
    s_minigun = 0;
}

void sub_00085370(void)
{
    if (s_minigun && s_minigun_scale != 1.0f && TUROK_MEM32(g_esp) == 0x0011F17Du) {
        float pitch = s_minigun_pitch + (guest_float(s_minigun + 0x2F8u) - s_minigun_pitch) * s_minigun_scale;
        uint32_t bits;
        memcpy(&bits, &pitch, 4);
        TUROK_MEM32(s_minigun + 0x2F8u) = bits;
        TUROK_MEM32(g_esp + 8u) = bits;   /* the pitch argument */
    }
    sub_00085370_gen();
}

/* ── Aim wobble at any frame rate ───────────────────────────────
 *
 * A scoped gun (sub_0011B220, called each frame while scoped) and the bows
 * at full draw (sub_0011CD30, sub_0011E9E0, and the update sub_00122CD0,
 * whose wobble is inline) wobble the aim one step per frame: each step eases
 * a yaw and a pitch rate toward a random target (the bows' BaseWobble and
 * RandWobble), turns the view by the yaw rate (sub_001421B0) and moves the
 * aim point by the pitch rate (sub_00038E10). No step looks at the frame
 * time, so at 120 fps the aim wandered twice as fast as at 60 and twice as
 * far a second, at 240 four times: past 60, scopes and drawn bows were
 * impossible to hold on a target (the pistol scope worst).
 *
 * The steps now come at most 60 a second, by the game's frame time (its
 * timer, [0x250A84] + 0x5C): at 60 fps and below, one a frame, as before
 * (the Xbox ran at 60 or less); above, only on frames where a 60th of a
 * second has gone by. The three wobble-only functions are skipped on the
 * other frames. The bow update still runs then -- its draw timer and the
 * rest -- with its wobble's state (+0x2BC..+0x2C8) put back and its turn and
 * aim moves dropped. */
#define AIM_WOBBLE_RATE   60.0f
#define GAME_TIMER_PTR    0x00250A84u

static RECOMP_TLS int s_wobble_held;   /* sub_00122CD0's wobble is not due */

#define AIM_WOBBLE_CLOCKS 64u
static struct { uint32_t self; float owed[4]; } s_wobble_clocks[AIM_WOBBLE_CLOCKS];

static void aim_wobble_reset(void)
{
    memset(s_wobble_clocks, 0, sizeof s_wobble_clocks);
}

/* Each weapon instance has its own clock for each update method. Sharing
 * one across players let one player's update consume another's next step. */
static int aim_wobble_due(uint32_t self, unsigned method)
{
    unsigned first = (self >> 4) & (AIM_WOBBLE_CLOCKS - 1u), at = first, tries;
    uint32_t timer = TUROK_MEM32(GAME_TIMER_PTR);
    float seconds = timer ? guest_float(timer + 0x5Cu) : 0.0f;
    float *owed;
    for (tries = 0; tries < AIM_WOBBLE_CLOCKS; tries++, at = (at + 1u) & (AIM_WOBBLE_CLOCKS - 1u))
        if (!s_wobble_clocks[at].self || s_wobble_clocks[at].self == self) break;
    if (tries == AIM_WOBBLE_CLOCKS) {
        at = first;   /* bounded storage; a new weapon starts with a fresh clock */
        memset(&s_wobble_clocks[at], 0, sizeof s_wobble_clocks[at]);
    }
    s_wobble_clocks[at].self = self;
    owed = &s_wobble_clocks[at].owed[method];
    if (!(seconds > 0.0f) || seconds > 0.25f) seconds = 1.0f / AIM_WOBBLE_RATE;
    *owed += seconds * AIM_WOBBLE_RATE;
    if (*owed < 0.999f) return 0;
    *owed -= 1.0f;
    if (*owed > 1.0f) *owed = 0.0f;    /* below 60 fps: no catching up */
    return 1;
}

extern void sub_0011B220_gen(void);
extern void sub_0011CD30_gen(void);
extern void sub_0011E9E0_gen(void);
extern void sub_00122CD0_gen(void);
extern void sub_001421B0_gen(void);
extern void sub_00038E10_gen(void);

void sub_0011B220(void)   /* scoped gun (this-call, ret) */
{
    if (!aim_wobble_due(g_ecx, 0)) { g_esp += 4; return; }
    sub_0011B220_gen();
}

void sub_0011CD30(void)   /* bow (this-call, ret) */
{
    if (!aim_wobble_due(g_ecx, 1)) { g_esp += 4; return; }
    sub_0011CD30_gen();
}

void sub_0011E9E0(void)   /* bow (this-call, ret) */
{
    if (!aim_wobble_due(g_ecx, 2)) { g_esp += 4; return; }
    sub_0011E9E0_gen();
}

void sub_00122CD0(void)   /* bow update (this-call, ret 4: frame seconds) */
{
    uint32_t self = g_ecx, state[4];
    int i, held = s_wobble_held;
    if (aim_wobble_due(self, 3)) { sub_00122CD0_gen(); return; }
    for (i = 0; i < 4; i++) state[i] = TUROK_MEM32(self + 0x2BCu + 4u * (uint32_t)i);
    s_wobble_held = 1;
    sub_00122CD0_gen();
    s_wobble_held = held;
    for (i = 0; i < 4; i++) TUROK_MEM32(self + 0x2BCu + 4u * (uint32_t)i) = state[i];
}

void sub_001421B0(void)   /* turn the view (this-call, ret 12) */
{
    if (s_wobble_held && TUROK_MEM32(g_esp) == 0x00122EE3u) { g_esp += 16; return; }
    sub_001421B0_gen();
}

void sub_00038E10(void)   /* set the aim point (this-call, ret 4) */
{
    if (s_wobble_held && TUROK_MEM32(g_esp) == 0x00122F58u) { g_esp += 8; return; }
    sub_00038E10_gen();
}

/* ── Enemy fire chance at high frame rates (issue #20) ───────────
 *
 * Two enemy kinds decide to shoot with a coin toss EVERY FRAME, with no
 * reload or cool-down after it:
 *   - FlyingEnemyAI (vtable 0x2116F8; the flying levels' fighters with red
 *     lasers): its attack update sub_0011A3B0 (this-call, ret 4) draws
 *     rand(0, 0x8000) / 32767 (sub_000D04D0, [0x1FAB4C]) and, below SHOOTFR
 *     (+0x834), fires (sub_00119750, called only from there, 0x0011A67F).
 *   - turrets (props TurretType/EnemyTurretType...; SFre at +0x494):
 *     sub_001267D0 (this-call, ret) draws the same and, below SFre, enters
 *     the "Fire" state and returns 1. Called from the turret update
 *     sub_00126940 (0x126976, 0x1269A1) and the player's turret state
 *     (0x149161).
 * So shots a second grew with the frame rate: four times the fire at 240 fps.
 * Now each enemy gets at most 60 tosses a second, by the game's frame time:
 * at 60 fps and below one a frame, as before (the Xbox ran at 60 or less);
 * above, the frames in between skip the toss. Every toss is independent, so
 * the shots a second are those of 60 fps. RECOMP_ENEMY_FIRE_RATE=0: as the
 * original (one toss a frame). */
static float frame_seconds(void);   /* the game's frame time, below */

#define ENEMY_FIRE_RATE   60.0f
#define ENEMY_FIRE_CLOCKS 256u

static struct { uint32_t self; float owed; } s_fire_clocks[ENEMY_FIRE_CLOCKS];
static RECOMP_TLS int s_fly_fire_held;   /* this frame's toss is not due */

static int enemy_fire_due(uint32_t self)
{
    static int enabled = -1;
    unsigned first = (self >> 4) & (ENEMY_FIRE_CLOCKS - 1u), at = first, tries;
    float seconds;
    if (enabled < 0) {
        const char *value = getenv("RECOMP_ENEMY_FIRE_RATE");
        enabled = !(value && *value == '0');
    }
    if (!enabled) return 1;
    seconds = frame_seconds();
    if (!(seconds > 0.0f) || seconds >= 1.0f / ENEMY_FIRE_RATE) return 1;
    for (tries = 0; tries < ENEMY_FIRE_CLOCKS; tries++, at = (at + 1u) & (ENEMY_FIRE_CLOCKS - 1u))
        if (!s_fire_clocks[at].self || s_fire_clocks[at].self == self) break;
    if (tries == ENEMY_FIRE_CLOCKS) {
        at = first;   /* bounded storage; a new enemy starts with a fresh clock */
        s_fire_clocks[at].owed = 0.0f;
    } else if (s_fire_clocks[at].self != self) {
        s_fire_clocks[at].owed = 0.0f;
    }
    s_fire_clocks[at].self = self;
    s_fire_clocks[at].owed += seconds * ENEMY_FIRE_RATE;
    if (s_fire_clocks[at].owed < 0.999f) return 0;
    s_fire_clocks[at].owed -= 1.0f;
    if (s_fire_clocks[at].owed > 1.0f) s_fire_clocks[at].owed = 0.0f;
    return 1;
}

extern void sub_0011A3B0_gen(void);
extern void sub_00119750_gen(void);
extern void sub_001267D0_gen(void);

void sub_0011A3B0(void)   /* FlyingEnemyAI attack update (this-call, ret 4: frame seconds) */
{
    int held = s_fly_fire_held;
    s_fly_fire_held = !enemy_fire_due(g_ecx);
    sub_0011A3B0_gen();
    s_fly_fire_held = held;
}

void sub_00119750(void)   /* FlyingEnemyAI fire (this-call, ret) */
{
    if (s_fly_fire_held && TUROK_MEM32(g_esp) == 0x0011A67Fu) { g_esp += 4; return; }
    sub_00119750_gen();
}

void sub_001267D0(void)   /* turret fire toss (this-call, ret; 1 = fired) */
{
    if (!enemy_fire_due(g_ecx)) { g_eax = 0; g_esp += 4; return; }
    sub_001267D0_gen();
}

/* ── Longer-lasting decals ──────────────────────────────────────
 *
 * Blood splats, bullet holes, scorch marks and gibs are particles: each kind
 * a PARTDEF text (data\particle\...\*.parx) that the particle system
 * parses (sub_000B0110, this-call: ?, the text, its length, ?) as a level
 * loads. Its eighth value is the particle's life in seconds -- 3 to 20 for
 * these -- which the parser stores through sub_000AF2A0 (+0x7C). The
 * definitions whose texture or mesh is a splat, a bullet hole, a scorch or
 * a gore piece get a minute instead, except in the flying levels
 * (video_settings_keep_decals). Effect parts with attached emitters retain
 * their own life, even if their parent definition uses a decal texture (as
 * several steam effects do). Every such definition is remembered with its
 * own life, so the rule re-applies when it changes (keep_decals_frame);
 * particles already out keep the life they were given.
 * RECOMP_PARTICLE_TRACE=1 lists the definitions. */
extern void sub_000B0110_gen(void);
extern void sub_000AF2A0_gen(void);
extern void sub_000AF9A0_gen(void);
extern int video_settings_keep_decals(void);

#define KEEP_DECAL_SECONDS 60.0f
#define KEEP_DECAL_DEFS 1024
static int s_partdef_keepsake;   /* the PARTDEF being parsed is a decal or gib */
static struct { uint32_t def; float own, set; int emitter_parent; } s_keep_defs[KEEP_DECAL_DEFS];
static unsigned s_keep_count;
static int s_keep_applied = -1;  /* the setting the remembered lives reflect */

/* Definitions belong to the world being left. Forget their addresses before
 * the screen loader frees that world or reuses its memory for the next one.
 * Pause-menu screens keep the same world and must keep its records too. */
static void keep_decals_reset(void)
{
    s_keep_count = 0;
    s_keep_applied = -1;
    s_partdef_keepsake = 0;
}

static int partdef_is_keepsake(uint32_t text, uint32_t size)
{
    /* Not explosions\1txt_fireb3_.tga: those are explosion fireballs, which
     * play their texture over their life -- ten minutes of it left the
     * explosions barely there, and their lingering took the room of new
     * effects such as the rockets' smoke trails.
     * The value before the texture is how a particle is turned: COLLIDE
     * (laid on the surface hit), PARENT (as what made it), SCREEN (facing
     * the camera). A SCREEN one is a sprite, never a decal. Blood turned as
     * its PARENT stays: most of it lies on the ground an impact effect was
     * turned to (the pools); only some, from a wound, stand up where they
     * land, and the definitions are the same, so they cannot be told apart
     * here. (0.7.28 left out all PARENT blood: most blood went in seconds.) */
    static const char *const kinds[] = {"splat", "_bh_", "scortch", "biggib\\", "bird_pieces\\",
                                        "brain_chunks\\"};
    const char *body = (const char *)((uintptr_t)text + g_xbox_mem_offset);
    char name[128], before[128] = "";
    uint32_t i = 0, k;
    if (!text || size > 65536u) return 0;
    while (i < size) {
        uint32_t n = 0;
        while (i < size && body[i] != '"') i++;
        i++;
        while (i < size && body[i] != '"' && n + 1 < sizeof name) {
            char c = body[i++];
            name[n++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        name[n] = 0;
        i++;
        if (!strstr(name, ".tga") && !strstr(name, ".mtf")) {
            memcpy(before, name, n + 1);
            continue;
        }
        if (!strcmp(before, "screen")) return 0;
        for (k = 0; k < sizeof kinds / sizeof *kinds; k++)
            if (strstr(name, kinds[k])) return 1;
        return 0;   /* the first texture or mesh decides */
    }
    return 0;
}

void sub_000B0110(void)
{
    static int trace = -1;
    uint32_t text = TUROK_MEM32(g_esp + 8), size = TUROK_MEM32(g_esp + 12);
    if (trace < 0) trace = getenv("RECOMP_PARTICLE_TRACE") != NULL;
    s_partdef_keepsake = partdef_is_keepsake(text, size);
    if (trace)
        fprintf(stderr, "[PARTDEF] %u bytes%s\n", size, s_partdef_keepsake ? " (decal or gib)" : "");
    sub_000B0110_gen();
    s_partdef_keepsake = 0;
}

/* The life sub_000AF2A0 stores at +0x7C is the value's magnitude. */
void sub_000AF2A0(void)
{
    uint32_t def = g_ecx;
    unsigned i;
    /* A definition reparsed at the same address may no longer be a decal. */
    for (i = 0; i < s_keep_count; i++)
        if (s_keep_defs[i].def == def) {
            s_keep_defs[i] = s_keep_defs[--s_keep_count];
            break;
        }
    if (s_partdef_keepsake) {
        float own = fabsf(guest_float(g_esp + 4)), set = own;
        i = s_keep_count;
        if (i < KEEP_DECAL_DEFS && video_settings_keep_decals() && own < KEEP_DECAL_SECONDS) {
            uint32_t bits;
            set = KEEP_DECAL_SECONDS;
            memcpy(&bits, &set, 4);
            TUROK_MEM32(g_esp + 4) = bits;
        }
        if (s_keep_count < KEEP_DECAL_DEFS) s_keep_count++;
        if (i < KEEP_DECAL_DEFS) {
            s_keep_defs[i].def = def;
            s_keep_defs[i].own = own;
            s_keep_defs[i].set = set;
            s_keep_defs[i].emitter_parent = 0;
        }
    }
    sub_000AF2A0_gen();
}

static void keep_decals_emitter_parent(uint32_t def)
{
    unsigned i;
    for (i = 0; i < s_keep_count; i++) {
        uint32_t bits;
        if (s_keep_defs[i].def != def) continue;
        s_keep_defs[i].emitter_parent = 1;
        /* Preserve independent edits made by the game. */
        if (guest_float(def + 0x7Cu) == s_keep_defs[i].set) {
            memcpy(&bits, &s_keep_defs[i].own, 4);
            TUROK_MEM32(def + 0x7Cu) = bits;
            s_keep_defs[i].set = s_keep_defs[i].own;
        }
        break;
    }
}

void sub_000AF9A0(void)
{
    /* Effect-part constructor: (PARTDEF *, emitter *, ...). This binding is
     * available after parsing and before the first particle is spawned.
     * Extending a parent keeps its child emitter running for the extended life. */
    if (TUROK_MEM32(g_esp + 8))
        keep_decals_emitter_parent(TUROK_MEM32(g_esp + 4));
    sub_000AF9A0_gen();
}

/* Once a frame: apply the setting to this world's definitions. The value
 * check also leaves a definition alone if the game has edited its life. */
static void keep_decals_frame(void)
{
    int keep = video_settings_keep_decals();
    unsigned i;
    if (keep == s_keep_applied) return;
    s_keep_applied = keep;
    for (i = 0; i < s_keep_count; i++) {
        float now = guest_float(s_keep_defs[i].def + 0x7Cu), want;
        uint32_t bits;
        if (now != s_keep_defs[i].set) continue;
        want = keep && !s_keep_defs[i].emitter_parent && s_keep_defs[i].own < KEEP_DECAL_SECONDS
            ? KEEP_DECAL_SECONDS : s_keep_defs[i].own;
        memcpy(&bits, &want, 4);
        TUROK_MEM32(s_keep_defs[i].def + 0x7Cu) = bits;
        s_keep_defs[i].set = want;
    }
}

/* ── Player-weapon emitter visibility ──────────────────────── */
extern void sub_000B4480_gen(void);
extern void sub_000AFF50_gen(void);
static int s_weapon_emitter;

static int emitter_is_player_weapon(uint32_t text, uint32_t size)
{
    const char *body = (const char *)((uintptr_t)text + g_xbox_mem_offset);
    uint32_t i = 0;
    char name[128];
    if (!text || size > 65536u) return 0;
    while (i < size) {
        uint32_t n = 0;
        while (i < size && body[i] != '"') i++;
        i++;
        while (i < size && body[i] != '"' && n + 1 < sizeof name) {
            char c = body[i++];
            if (c == '/') c = '\\';
            name[n++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        name[n] = 0;
        i++;
        if (!strncmp(name, "marc\\weapons\\", 13)) return 1;
    }
    return 0;
}

void sub_000B4480(void)
{
    int previous = s_weapon_emitter;
    s_weapon_emitter = emitter_is_player_weapon(TUROK_MEM32(g_esp + 8), TUROK_MEM32(g_esp + 12));
    sub_000B4480_gen();
    s_weapon_emitter = previous;
}

void sub_000AFF50(void)
{
    static int logged;
    /* This is the emitter's NoCull setter, independent of PARTDEF's
     * AlwaysUpdate flag. The original scheduler (0x8D4C0) can discard a
     * weapon's pending birth and reset its emission clock when the muzzle
     * is outside the camera frustum. Player-weapon particles must be born
     * regardless of view; render clipping, rates and pool limits still apply.
     * World and enemy emitters retain their own settings. */
    if (s_weapon_emitter) {
        TUROK_MEM32(g_esp + 4) = 1;
        if (!logged) {
            fprintf(stderr, "[PARTICLES] player-weapon emitters: camera-independent spawning enabled\n");
            logged = 1;
        }
    }
    sub_000AFF50_gen();
}

/* ── Shotgun ballistic origin ──────────────────────────────── */
extern void sub_000966F0_gen(void);

static int shotgun_ballistic_effect(uint32_t address)
{
    static const char prefix[] = "marc\\weapons\\shotgun\\";
    const char *text = (const char *)((uintptr_t)address + g_xbox_mem_offset);
    char name[192];
    const char *tail;
    unsigned i;
    if (!address) return 0;
    for (i = 0; i + 1 < sizeof name && text[i]; i++) {
        char c = text[i];
        if (c == '/') c = '\\';
        name[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    if (i + 1 == sizeof name) return 0;
    name[i] = 0;
    tail = strstr(name, prefix);
    if (!tail || (tail != name && tail[-1] != '\\')) return 0;
    tail += sizeof prefix - 1;
    return !strcmp(tail, "shotgun_01.eff") || !strcmp(tail, "shotgun_2.eff") ||
           !strcmp(tail, "shotgun_3.eff") || !strcmp(tail, "supershotgun.eff");
}

void sub_000966F0(void)
{
    static int logged;
    uint32_t matrix = 0, saved[3];
    unsigned i;
    /* Actor::SpawnParticle builds the effect transform at 0x43B6D. For a
     * held shotgun its translation includes the view model's lateral/bob
     * offset (~21 cm left, 22 cm down in a measured idle frame). Pellets
     * inherit that offset, so point-blank impacts miss the crosshair.
     * Recenter only the player's ballistic effect; the existing orientation
     * and EMI velocity spread remain intact. Shells, flashes, impacts and
     * enemy weapons keep their original origins. */
    if (TUROK_MEM32(g_esp) == 0x00043B72u && shotgun_ballistic_effect(TUROK_MEM32(g_esp + 4))) {
        uint32_t info = TUROK_MEM32(g_esp + 16);
        uint32_t weapon = info ? TUROK_MEM32(info + 0x10) : 0;
        if (weapon && TUROK_MEM32(weapon) == 0x002143C8u) {
            uint32_t player = TUROK_MEM32(weapon + 0x234);
            uint32_t camera = player ? TUROK_MEM32(player + 0x3CC) : 0;
            uint32_t transform = TUROK_MEM32(g_esp + 12);
            /* Weapon::Update has just refreshed this camera's world matrix
             * via 0x1FFB0. Each local player has its own camera/weapon. */
            if (camera && transform && TUROK_MEM32(player + 0x1A54) == weapon) {
                matrix = transform + 0x30;
                for (i = 0; i < 3; i++) {
                    saved[i] = TUROK_MEM32(matrix + i * 4u);
                    TUROK_MEM32(matrix + i * 4u) = TUROK_MEM32(camera + 0x2FC + i * 4u);
                }
                if (!logged) {
                    fprintf(stderr, "[PARTICLES] shotgun ballistic origin follows the aim camera\n");
                    logged = 1;
                }
            }
        }
    }
    sub_000966F0_gen();
    /* The input transform belongs to the caller, not the particle manager. */
    if (matrix) for (i = 0; i < 3; i++) TUROK_MEM32(matrix + i * 4u) = saved[i];
}

/* ── Particle limit (sub_00096EE0, sub_000B98D0, sub_000B9700) ──
 *
 * A level makes one particle manager (sub_000B9D40) for at most the larger
 * of 1000 particles and its own setting (level +0x90), about 540 bytes of
 * budget each (sub_000B7010). The particles out (+0x24) sit in a list per
 * priority, 0 to 4, oldest first: sub_000B98D0 adds each before the tail
 * node (+0xCC4 + priority * 0x1BC, or +0x1570 for the other kind), after
 * the head node (+0x1E1C + ..., or +0x26C8), linked through +0x1A4 and
 * back through +0x1A8. Before an effect is emitted, sub_000B9700
 * (this-call: priority, count) refuses it -- returns 1 -- by chance once the
 * particles out pass that priority's share of the capacity (+0x2F74 +
 * priority * 4: 70, 60, 50 and 40% for priorities 1 to 4, less under
 * load), and always about 5% past it.
 *
 * Blood, bullet holes and gibs kept out for a minute (ten minutes, when
 * this was written), so a long fight filled those shares and new effects -- the blood
 * from a severed neck among them -- stopped appearing. Now, as an effect
 * comes within 1% of its share, the oldest kept particles are removed (by
 * their own kill, vfunc +0x1C, as the game's make-room sub_000B97D0 does)
 * to bring the count 3% under it; each particle is stamped as it is added
 * to tell the oldest across the lists. The capacity is also at least
 * RECOMP_PARTICLES (default 4000; this build maps 128 MB, so the extra
 * 1.6 MB is room the console never had to give), so more stay before any
 * go. Removals and refusals are logged, at most once every ten seconds. */
#include <time.h>
extern void sub_00096EE0_gen(void);
extern void sub_000B98D0_gen(void);
extern void sub_000B9700_gen(void);
extern RECOMP_TLS uint32_t g_edx;
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

#define PARTICLE_STAMPS 65536u
static struct { uint32_t particle, stamp; } s_particle_stamps[PARTICLE_STAMPS];
static uint32_t s_particle_stamp, s_particle_manager;
static unsigned s_particles_removed, s_particles_refused;
static uint64_t s_particle_frame;

typedef struct { uint32_t stamp, particle; } KeptParticle;
static struct {
    KeptParticle *kept;
    unsigned room, count, next;
    uint32_t manager;
    uint64_t frame;
    int valid;
} s_particle_trim;

static void particle_tracking_reset(void)
{
    memset(s_particle_stamps, 0, sizeof s_particle_stamps);
    s_particle_stamp = s_particle_manager = 0;
    s_particle_trim.valid = 0;
}

static int32_t particle_capacity(void)
{
    static int32_t capacity;
    if (!capacity) {
        const char *value = getenv("RECOMP_PARTICLES");
        capacity = value && *value ? atoi(value) : 4000;
        if (capacity < 1000) capacity = 1000;
        if (capacity > 20000) capacity = 20000;
    }
    return capacity;
}

void sub_00096EE0(void)
{
    static int32_t logged;
    uint32_t level = TUROK_MEM32(g_ecx), asked = TUROK_MEM32(level + 0x90u);
    int32_t capacity = particle_capacity();
    if ((int32_t)asked < capacity) TUROK_MEM32(level + 0x90u) = (uint32_t)capacity;
    else capacity = (int32_t)asked;
    if (capacity != logged) {
        logged = capacity;
        fprintf(stderr, "[PARTICLES] capacity %d (the level asks for %d)\n", capacity, (int32_t)asked);
    }
    sub_00096EE0_gen();
    TUROK_MEM32(level + 0x90u) = asked;
}

/* The stamp a particle got when it was added, or a new slot for it. */
static uint32_t *particle_stamp(uint32_t particle)
{
    uint32_t at = (particle >> 2) * 2654435761u >> 16, tries;
    for (tries = 0; tries < 64u; tries++, at = (at + 1u) & (PARTICLE_STAMPS - 1u)) {
        if (s_particle_stamps[at].particle == particle) return &s_particle_stamps[at].stamp;
        if (!s_particle_stamps[at].particle) {
            s_particle_stamps[at].particle = particle;
            s_particle_stamps[at].stamp = 0;
            return &s_particle_stamps[at].stamp;
        }
    }
    return NULL;
}

void sub_000B98D0(void)
{
    uint32_t *stamp;
    if (g_ecx != s_particle_manager) {
        particle_tracking_reset();
        s_particle_manager = g_ecx;
    }
    stamp = particle_stamp(TUROK_MEM32(g_esp + 4u));
    if (stamp) *stamp = ++s_particle_stamp;
    sub_000B98D0_gen();
}

/* A particle Keep decals made long-lived: its definition (+0x48) has the
 * long life, and the game has not marked it never to be culled. */
static int particle_kept(uint32_t particle)
{
    uint32_t def = TUROK_MEM32(particle + 0x48u);
    return def && !(TUROK_MEM32(particle + 0xCu) & 0x400000u) &&
           guest_float(def + 0x7Cu) >= KEEP_DECAL_SECONDS;
}

static int particle_kill(uint32_t particle)
{
    uint32_t va = TUROK_MEM32(TUROK_MEM32(particle) + 0x1Cu);
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, esp = g_esp;
    recomp_func_t fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup(va);
    if (!fn) return 0;
    g_esp -= 4;
    TUROK_MEM32(g_esp) = 0;   /* return address */
    g_ecx = particle;
    fn();
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_esp = esp;
    return 1;
}

static int kept_older(const void *a, const void *b)
{
    uint32_t x = ((const KeptParticle *)a)->stamp, y = ((const KeptParticle *)b)->stamp;
    return x < y ? -1 : x > y;
}

/* Scan and sort at most once per frame, then consume the list across this
 * frame's emission requests. Even an empty result is cached: a full manager
 * with nothing removable must not be walked again for every effect. Newly
 * added particles become candidates next frame; their stamps also protect
 * against removing a new particle at an address a cached one used. */
static void particle_trim(uint32_t self, int32_t count)
{
    unsigned n = 0, i, priority, kind;
    if (s_particle_trim.valid && s_particle_trim.manager == self && s_particle_trim.frame == s_particle_frame)
        goto remove_kept;
    s_particle_trim.valid = 1;
    s_particle_trim.manager = self;
    s_particle_trim.frame = s_particle_frame;
    s_particle_trim.next = s_particle_trim.count = 0;
    for (priority = 0; priority < 5u; priority++)
        for (kind = 0; kind < 2u; kind++) {
            uint32_t head = self + (kind ? 0x26C8u : 0x1E1Cu) + priority * 0x1BCu;
            uint32_t tail = self + (kind ? 0x1570u : 0xCC4u) + priority * 0x1BCu;
            uint32_t at = TUROK_MEM32(head + 0x1A4u), walked = 0;
            for (; at && at != tail && walked < 65536u; at = TUROK_MEM32(at + 0x1A4u), walked++) {
                uint32_t *stamp;
                if (!particle_kept(at)) continue;
                if (n == s_particle_trim.room) {
                    KeptParticle *more = realloc(s_particle_trim.kept, (s_particle_trim.room + 1024u) * sizeof *more);
                    if (!more) break;
                    s_particle_trim.kept = more;
                    s_particle_trim.room += 1024u;
                }
                stamp = particle_stamp(at);
                s_particle_trim.kept[n].stamp = stamp ? *stamp : 0;
                s_particle_trim.kept[n].particle = at;
                n++;
            }
        }
    s_particle_trim.count = n;
    if (n > 1) qsort(s_particle_trim.kept, n, sizeof *s_particle_trim.kept, kept_older);
remove_kept:
    while (s_particle_trim.next < s_particle_trim.count && count > 0) {
        uint32_t at, previous, *stamp;
        i = s_particle_trim.next++;
        at = s_particle_trim.kept[i].particle;
        stamp = particle_stamp(at);
        if (!stamp || *stamp != s_particle_trim.kept[i].stamp) continue;
        /* still out (removing one may have taken others with it) */
        previous = TUROK_MEM32(at + 0x1A8u);
        if (!previous || TUROK_MEM32(previous + 0x1A4u) != at || !particle_kept(at)) continue;
        if (!particle_kill(at)) continue;
        count--;
        s_particles_removed++;
    }
}

void sub_000B9700(void)
{
    static clock_t reported;
    uint32_t self = g_ecx, priority = TUROK_MEM32(g_esp + 4u);
    int32_t count = (int32_t)TUROK_MEM32(g_esp + 8u);
    if (priority < 5u && video_settings_keep_decals()) {
        int32_t capacity = (int32_t)TUROK_MEM32(self + 0x18u), out = (int32_t)TUROK_MEM32(self + 0x24u);
        int32_t share = (int32_t)TUROK_MEM32(self + 0x2F74u + priority * 4u), margin = capacity / 100;
        if (share < capacity && out + count > share - margin)
            particle_trim(self, out + count - (share - 3 * margin));
    }
    g_ecx = self;
    sub_000B9700_gen();
    if (g_eax) s_particles_refused++;
    if ((s_particles_removed || s_particles_refused) &&
            (!reported || clock() - reported > 10 * CLOCKS_PER_SEC)) {
        fprintf(stderr, "[PARTICLES] %d of %d out; %u oldest kept removed, %u effects refused\n",
                (int32_t)TUROK_MEM32(self + 0x24u), (int32_t)TUROK_MEM32(self + 0x18u),
                s_particles_removed, s_particles_refused);
        s_particles_removed = s_particles_refused = 0;
        reported = clock();
    }
}

/* ── Corpse trace (RECOMP_CORPSE_TRACE=1) ──────────────────────
 *
 * Finding what makes bodies vanish: when each actor first runs the Dead
 * physics state (sub_00118AE0, cdecl (actor, ...), the state table's entry
 * for "Dead"), and every deactivate (sub_00092B60) and activate
 * (sub_00093330) of an actor (this-call), with the guest caller and, for
 * an actor seen dead, how long after. */
#include <time.h>
extern void sub_00118AE0_gen(void);
extern void sub_00092B60_gen(void);
extern void sub_00093330_gen(void);

static int corpse_trace(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_CORPSE_TRACE") != NULL;
    return on;
}

static struct { uint32_t actor; clock_t at; } s_dead[512];
static unsigned s_dead_count;

static long dead_for_ms(uint32_t actor)
{
    unsigned i;
    for (i = 0; i < s_dead_count; i++)
        if (s_dead[i].actor == actor)
            return (long)((clock() - s_dead[i].at) * 1000 / CLOCKS_PER_SEC);
    return -1;
}

void sub_00118AE0(void)
{
    if (corpse_trace()) {
        uint32_t actor = TUROK_MEM32(g_esp + 4);
        if (dead_for_ms(actor) < 0 && s_dead_count < 512) {
            s_dead[s_dead_count].actor = actor;
            s_dead[s_dead_count].at = clock();
            s_dead_count++;
            fprintf(stderr, "[CORPSE] actor %08X (vtable %08X) dead at %.1f s\n",
                    actor, TUROK_MEM32(actor), (double)clock() / CLOCKS_PER_SEC);
        }
    }
    sub_00118AE0_gen();
}

void sub_00092B60(void)
{
    if (corpse_trace()) {
        long dead = dead_for_ms(g_ecx);
        fprintf(stderr, "[CORPSE] deactivate %08X (vtable %08X) arg %u from %08X%s",
                g_ecx, TUROK_MEM32(g_ecx), TUROK_MEM32(g_esp + 4), TUROK_MEM32(g_esp),
                dead >= 0 ? "" : "\n");
        if (dead >= 0) fprintf(stderr, ", dead %.1f s\n", dead / 1000.0);
    }
    sub_00092B60_gen();
}

/* The guest return addresses near the top of the stack (values in .text):
 * enough to name who is destroying an actor. */
static void corpse_callers(void)
{
    uint32_t i, shown = 0;
    for (i = 0; i < 48 && shown < 8; i++) {
        uint32_t v = TUROK_MEM32(g_esp + i * 4u);
        if (v >= 0x00011000u && v < 0x001F0000u) {
            fprintf(stderr, " %08X", v);
            shown++;
        }
    }
    fprintf(stderr, "\n");
}

/* AnimalAI's and HumanAI's deleting destructors (vtable slot 0x154). */
extern void sub_0012F110_gen(void);
extern void sub_00136030_gen(void);

static void unwatch_actor(uint32_t actor);

void sub_0012F110(void)
{
    unwatch_actor(g_ecx);
    if (corpse_trace()) {
        fprintf(stderr, "[CORPSE] delete AnimalAI %08X at %.1f s:", g_ecx, (double)clock() / CLOCKS_PER_SEC);
        corpse_callers();
    }
    sub_0012F110_gen();
}

void sub_00136030(void)
{
    unwatch_actor(g_ecx);
    if (corpse_trace()) {
        fprintf(stderr, "[CORPSE] delete HumanAI %08X at %.1f s:", g_ecx, (double)clock() / CLOCKS_PER_SEC);
        corpse_callers();
    }
    sub_00136030_gen();
}

/* Watching AnimalAI/HumanAI actors for what changes when one dies and when
 * its body goes: their constructors (sub_0012F130, 0x7CC bytes;
 * sub_001354C0, 0x8B0) record them, and once a frame (sub_00104600) every
 * word that has changed at most four times is reported -- flags and states,
 * not positions or timers. */
#define WATCH_ACTORS 96
#define WATCH_WORDS  (0x8B0 / 4)
static struct {
    uint32_t actor, words;
    clock_t born;
    uint32_t last[WATCH_WORDS];
    uint8_t changes[WATCH_WORDS];
} s_watch[WATCH_ACTORS];

extern void sub_0012F130_gen(void);
extern void sub_001354C0_gen(void);
extern void sub_00104600_gen(void);

static void watch_actor(uint32_t actor, uint32_t bytes)
{
    unsigned i, w;
    for (i = 0; i < WATCH_ACTORS; i++)
        if (!s_watch[i].actor || s_watch[i].actor == actor) break;
    if (i == WATCH_ACTORS) return;
    s_watch[i].actor = actor;
    s_watch[i].words = bytes / 4;
    s_watch[i].born = clock();
    for (w = 0; w < s_watch[i].words; w++) {
        s_watch[i].last[w] = TUROK_MEM32(actor + w * 4u);
        s_watch[i].changes[w] = 0;
    }
}

static clock_t s_level_start;   /* RECOMP_DEBUG_KILL's clock: the last level load */
static int s_level_killed;

void corpse_watch_reset(void)
{
    memset(s_watch, 0, sizeof s_watch);
    s_level_start = clock();
    s_level_killed = 0;
}

static void unwatch_actor(uint32_t actor)
{
    unsigned i;
    for (i = 0; i < WATCH_ACTORS; i++)
        if (s_watch[i].actor == actor) s_watch[i].actor = 0;
}

void sub_0012F130(void)
{
    uint32_t self = g_ecx;
    sub_0012F130_gen();
    if (corpse_trace()) watch_actor(self, 0x7CC);
}

void sub_001354C0(void)
{
    uint32_t self = g_ecx;
    sub_001354C0_gen();
    if (corpse_trace()) watch_actor(self, 0x8B0);
}

static int watched(uint32_t actor)
{
    unsigned i;
    for (i = 0; i < WATCH_ACTORS; i++)
        if (s_watch[i].actor == actor) return 1;
    return 0;
}

/* SetAlpha (sub_00040DB0, this-call, float alpha, ret 4): traced for the
 * watched actors. */
extern void sub_00040DB0_gen(void);

void sub_00040DB0(void)
{
    if (corpse_trace() && watched(g_ecx)) {
        fprintf(stderr, "[CORPSE] %.2f s SetAlpha %08X %.3f:", (double)clock() / CLOCKS_PER_SEC,
                g_ecx, guest_float(g_esp + 4));
        corpse_callers();
    }
    sub_00040DB0_gen();
}

static void hurt_frame(void);   /* RECOMP_FALL_TRACE, below */
static void put_guest_float(uint32_t va, float value);   /* below */
extern float video_settings_look_limit(void);

void sub_00104600(void)
{
    s_particle_frame++;
    sub_00104600_gen();
    keep_decals_frame();
    hurt_frame();
    /* Look limit (PC Settings): the game's pitch limit, [0x221420] (70
     * degrees), read by the on-foot look (sub_00144560 through the call at
     * 0x144970), the look of the other aiming states (0x1458FD) and Ladder
     * freelook. Extended: 85. */
    {
        float limit = video_settings_look_limit();
        if (guest_float(0x00221420u) != limit) put_guest_float(0x00221420u, limit);
    }
    /* RECOMP_DEBUG_KILL=<s>: that long after a level's actors were made,
     * every watched AnimalAI's health (+0x1D0, a float) is set to 0 once --
     * a dinosaur to watch die without a fight. */
    {
        static int kill = -1;
        if (kill < 0) kill = getenv("RECOMP_DEBUG_KILL") ? atoi(getenv("RECOMP_DEBUG_KILL")) : 0;
        if (kill > 0) {
            unsigned i;
            uint32_t zero = 0;
            if (s_level_start && !s_level_killed && clock() - s_level_start > kill * CLOCKS_PER_SEC) {
                s_level_killed = 1;
                for (i = 0; i < WATCH_ACTORS; i++)
                    if (s_watch[i].actor && s_watch[i].words == 0x7CC / 4) {
                        fprintf(stderr, "[CORPSE] %.2f s debug kill %08X (health %.1f)\n",
                                (double)clock() / CLOCKS_PER_SEC, s_watch[i].actor,
                                guest_float(s_watch[i].actor + 0x1D0u));
                        TUROK_MEM32(s_watch[i].actor + 0x1D0u) = zero;
                    }
            }
        }
    }
    if (corpse_trace()) {
        unsigned i, w;
        clock_t now = clock();
        for (i = 0; i < WATCH_ACTORS; i++) {
            if (!s_watch[i].actor) continue;
            for (w = 0; w < s_watch[i].words; w++) {
                uint32_t value = TUROK_MEM32(s_watch[i].actor + w * 4u);
                if (value == s_watch[i].last[w]) continue;
                if (s_watch[i].changes[w] < 255) s_watch[i].changes[w]++;
                if (s_watch[i].changes[w] <= 4 && now - s_watch[i].born > 3 * CLOCKS_PER_SEC)
                    fprintf(stderr, "[WATCH] %.2f s actor %08X +%03X: %08X -> %08X\n",
                            (double)now / CLOCKS_PER_SEC, s_watch[i].actor, w * 4u,
                            s_watch[i].last[w], value);
                s_watch[i].last[w] = value;
            }
        }
    }
}

void sub_00093330(void)
{
    if (corpse_trace() && dead_for_ms(g_ecx) >= 0)
        fprintf(stderr, "[CORPSE] activate %08X from %08X, dead %.1f s\n",
                g_ecx, TUROK_MEM32(g_esp), dead_for_ms(g_ecx) / 1000.0);
    sub_00093330_gen();
}

/* ── Box-in-view test (sub_00021A90) ───────────────────────────
 *
 * camera->InView(centre, axes) (this-call, ret 8): 1 if a box is in view,
 * 0 if not. It first transforms the centre by the camera's view matrix
 * (rows at +0x58/+0x68/+0x78, translation +0x88) and accepts it if it lies
 * between the near and far planes (+0xF0/+0xF4) and within the cotangents
 * of the half fields of view (+0x108/+0x10C, kept by sub_00020030 while
 * +0x1A8 bit 1 is clear); only otherwise does it test the box's corners
 * against the six planes. The scene's culling calls it for every object, a
 * fifth of the game thread in busy views, so both tests are done here
 * natively with the original's roundings (x87 below); what they do not
 * settle -- a stale cache, a box across a plane, a NaN -- is left to it. */
extern void sub_00021A90_gen(void);

/* Widescreen culling. The game culls against its own camera, which stays
 * 4:3 -- widescreen only widens the projection on the GPU -- so anything past
 * the 4:3 edges (blood, bullet holes, effects, objects) was culled while in
 * view. The camera's view planes (sub_00020EE0) and the in-view test take
 * the horizontal half-angle's cotangent from sub_00020030; while they run it
 * is divided by the widescreen factor, which widens what they keep to the
 * screen's width. The projection, which reads the camera's own copy, is left
 * as it is. */
extern void sub_00020030_gen(void);
extern void sub_00020EE0_gen(void);
extern float video_settings_wide_factor(void);
static int s_cull_widen;

void sub_00020030(void)
{
    uint32_t across = TUROK_MEM32(g_esp + 4);
    float wide = video_settings_wide_factor();
    sub_00020030_gen();
    if (s_cull_widen && wide > 1.001f && across) {
        float cotangent = guest_float(across) / wide;
        uint32_t bits;
        memcpy(&bits, &cotangent, 4);
        TUROK_MEM32(across) = bits;
    }
}

void sub_00020EE0(void)
{
    s_cull_widen++;
    sub_00020EE0_gen();
    s_cull_widen--;
}

/* Particles -- blood, bullet holes, scorches, sparks -- are culled by the
 * particle manager against planes of its own (sub_000BA820: the near plane,
 * then the sides and the top and bottom; sub_0008A0E0 tests each particle as
 * a sphere). The side planes (sub_000BA200) take the camera's horizontal
 * field of view from sub_0001FE00, 4:3's, so a splat whose centre passed the
 * 4:3 edge vanished while on screen: at 21:9 only the middle 56% of the
 * width kept them. While they are built the angle is the widened view's,
 * 2 atan(wide tan(fov / 2)). Its other callers -- the scene walk's and the
 * meshes' level of detail -- keep 4:3's, as Hor+ leaves sizes on screen as
 * they were.
 *
 * The sphere a particle is tested as is far smaller than a blood pool or a
 * splat lying on the ground, so one went as soon as its centre passed an
 * edge, half of it still on screen (look up from a pool and it vanishes; the
 * original does it too). The side planes (sub_000BA200) and the top and
 * bottom ones (sub_000BA3B0) take an angle added to the field of view as
 * their first argument; each gets RECOMP_PARTICLE_MARGIN degrees more
 * (default 40), so a particle is dropped only well off the screen. The
 * widened horizontal angle and its margin stay under 170 degrees. */
extern void sub_0001FE00_gen(void);
extern void sub_000BA200_gen(void);
extern void sub_000BA3B0_gen(void);
static int s_fov_widen;
static float s_fov_margin;   /* the angle sub_000BA200 adds to the field of view */

static float particle_margin(void)
{
    static float margin = -1.0f;
    if (margin < 0.0f) {
        const char *value = getenv("RECOMP_PARTICLE_MARGIN");
        margin = (value && *value ? (float)atof(value) : 40.0f) * 3.14159265f / 180.0f;
        if (!(margin >= 0.0f) || margin > 1.4f) margin = 0.0f;
    }
    return margin;
}

static void add_to_float_arg(uint32_t address, float amount)
{
    float value = guest_float(address) + amount;
    uint32_t bits;
    memcpy(&bits, &value, 4);
    TUROK_MEM32(address) = bits;
}

/* Level of detail (issue #34): characters and meshes pick a model by
 * tan(fov x [0x1FAAD4]) x (distance - an offset), against each model's
 * switch distances -- the actor draw from 0x000EECA0, the mesh draw from
 * 0x000F04DE, both right after this call. Far off, characters drop to models
 * with few vertices, whose per-vertex lighting jumps visibly as they move;
 * the sniper scope magnifies it. LOD range (Video Options) divides that
 * measure by 2 or 4 for those two calls alone: detailed models twice or four
 * times as far, or near nothing ("max"): the detailed models at any distance. RECOMP_LOD_TRACE=1 logs what the two sites see. */
float video_settings_lod_factor(void);   /* video_settings.h, included further down */
static void lod_adjust(uint32_t out, uint32_t site)
{
    static int trace = -1;
    static unsigned long last;
    float factor = video_settings_lod_factor(), k = guest_float(0x1FAAD4u), fov = guest_float(out);
    if (trace < 0) trace = getenv("RECOMP_LOD_TRACE") != NULL;
    if (trace && (unsigned long)clock() - last >= 1000) {
        last = (unsigned long)clock();
        fprintf(stderr, "[LOD] site %08X fov %.4f k %.6f tan %.4f factor %.0f\n", site, fov, k, tanf(fov * k), factor);
    }
    if (factor > 1.0f && k > 0.0f && fov > 0.0f && fov * k < 1.55f) {
        uint32_t bits;
        fov = atanf(tanf(fov * k) / factor) / k;
        memcpy(&bits, &fov, 4);
        TUROK_MEM32(out) = bits;
    }
}

void sub_0001FE00(void)
{
    uint32_t out = TUROK_MEM32(g_esp + 4), site = TUROK_MEM32(g_esp);
    float wide = video_settings_wide_factor();
    sub_0001FE00_gen();
    if (out && (site == 0x000EECA0u || site == 0x000F04DEu)) lod_adjust(out, site);
    if (s_fov_widen && out) {
        float fov = guest_float(out), limit = 2.967f - s_fov_margin;   /* 170 degrees */
        uint32_t bits;
        if (wide > 1.001f) fov = 2.0f * atanf(wide * tanf(fov * 0.5f));
        if (fov > limit) fov = limit;
        memcpy(&bits, &fov, 4);
        TUROK_MEM32(out) = bits;
    }
}

void sub_000BA200(void)
{
    add_to_float_arg(g_esp + 4, particle_margin());
    s_fov_margin = guest_float(g_esp + 4);
    s_fov_widen++;
    sub_000BA200_gen();
    s_fov_widen--;
}

void sub_000BA3B0(void)
{
    add_to_float_arg(g_esp + 4, particle_margin());
    sub_000BA3B0_gen();
}

/* The camera's other view tests take the cotangent from sub_00020030 too:
 * a point (sub_00021740) and a sphere's two tests
 * (sub_00021860, sub_00022590: the models' bounding spheres, sub_00180720). */
extern void sub_00021740_gen(void);
extern void sub_00021860_gen(void);
extern void sub_00022590_gen(void);
void sub_00021740(void) { s_cull_widen++; sub_00021740_gen(); s_cull_widen--; }
void sub_00021860(void) { s_cull_widen++; sub_00021860_gen(); s_cull_widen--; }
void sub_00022590(void) { s_cull_widen++; sub_00022590_gen(); s_cull_widen--; }
extern RECOMP_TLS uint16_t g_fp_control_word;

/* One x87 operation's result as the generated code rounds it: to float
 * while the control word asks for single precision (as D3D leaves it), else
 * kept. Out-of-range results (where it rounds more carefully) do not occur
 * in these tests' inputs; they would at worst send a box the other way at a
 * plane's exact edge. */
static double x87(double value)
{
    return (g_fp_control_word & 0x300u) ? value : (double)(float)value;
}
#define FMUL(a, b) x87((double)(a) * (double)(b))
#define FADD(a, b) x87((double)(a) + (double)(b))

/* Camera row (x, y, z multipliers at +m, +m+0x10, +m+0x20) applied to a
 * point as the original orders it: z, plus y, plus x, plus translation. */
static double view_row(uint32_t camera, uint32_t m, double x, double y, double z)
{
    double v = FADD(FMUL(z, guest_float(camera + m + 0x20u)), FMUL(y, guest_float(camera + m + 0x10u)));
    v = FADD(v, FMUL(x, guest_float(camera + m)));
    return FADD(v, guest_float(camera + m + 0x30u));
}

static int in_view(uint32_t camera, uint32_t point, uint32_t axes)
{
    double x = guest_float(point), y = guest_float(point + 4u), z = guest_float(point + 8u);

    if (!(TUROK_MEM32(camera + 0x1A8u) & 2u)) {
        double depth = view_row(camera, 0x60u, x, y, z);
        if (depth >= guest_float(camera + 0xF0u) && depth <= guest_float(camera + 0xF4u)) {
            float across = (float)view_row(camera, 0x58u, x, y, z);
            float up = (float)view_row(camera, 0x5Cu, x, y, z);
            double limit = (float)depth;
            double h = FMUL(across, (float)(guest_float(camera + 0x108u) / video_settings_wide_factor()));
            double v = FMUL(up, guest_float(camera + 0x10Cu));
            if (h <= limit && -h <= limit && v <= limit && -v <= limit)
                return 1;
        }
    }
    /* The plane test: the box is the centre argument as a corner plus three
     * edges (second argument). Against each of the six planes the camera
     * keeps (normals +0x144, distances +0x18C; valid while +0x1A8 bit 4 is
     * clear and the cache is for argument 0, +0x1A4), the corner furthest
     * along the normal behind it means out of view (0), and the nearest in
     * front of every plane means wholly in view (1). The original keeps the
     * far corner in x87 registers and stores the near one as float. A box
     * across a plane goes on to the original's finer test. */
    if (!(TUROK_MEM32(camera + 0x1A8u) & 0x10u) && TUROK_MEM32(camera + 0x1A4u) == 0) {
        uint32_t plane, k;
        int inside = 0;
        for (plane = 0; plane < 6; plane++) {
            uint32_t normal = camera + 0x144u + plane * 12u;
            double nx = guest_float(normal), ny = guest_float(normal + 4u), nz = guest_float(normal + 8u);
            double far_x = x, far_y = y, far_z = z, distance = guest_float(camera + 0x18Cu + plane * 4u), d;
            float near_x = (float)x, near_y = (float)y, near_z = (float)z;
            for (k = 0; k < 3; k++) {
                uint32_t edge = axes + k * 12u;
                double ex = guest_float(edge), ey = guest_float(edge + 4u), ez = guest_float(edge + 8u);
                if (FADD(FADD(FMUL(ez, nz), FMUL(ey, ny)), FMUL(nx, ex)) > 0.0) {
                    far_x = FADD(far_x, ex); far_y = FADD(far_y, ey); far_z = FADD(far_z, ez);
                } else {
                    near_x = (float)FADD(near_x, ex); near_y = (float)FADD(near_y, ey); near_z = (float)FADD(near_z, ez);
                }
            }
            d = FADD(FADD(FADD(FMUL(far_x, nx), FMUL(far_z, nz)), FMUL(far_y, ny)), distance);
            if (d < 0.0)
                return 0;
            d = FADD(FADD(FADD(FMUL(near_z, nz), FMUL(near_y, ny)), FMUL(near_x, nx)), distance);
            if (d >= 0.0)
                inside++;
        }
        if (inside == 6)
            return 1;

        /* A box across a plane: the original takes its eight corners, and is
         * out of view if all eight are behind one plane, in view if any is in
         * front of all six; only then does it test the twelve edges
         * (sub_000201B0). It orders each corner's sums differently, so here a
         * corner whose distance is within rounding of a plane leaves the box
         * to it; every other sign is the original's. */
        {
            double corner[8][3];
            int behind[6] = {0}, in_front[8] = {0}, c, axis;
            for (c = 0; c < 8; c++) {
                double p[3] = {x, y, z};
                for (k = 0; k < 3; k++)
                    if (c & (1 << k))
                        for (axis = 0; axis < 3; axis++)
                            p[axis] = (float)FADD(p[axis], guest_float(axes + k * 12u + axis * 4u));
                memcpy(corner[c], p, sizeof p);
            }
            for (plane = 0; plane < 6; plane++) {
                uint32_t normal = camera + 0x144u + plane * 12u;
                double nx = guest_float(normal), ny = guest_float(normal + 4u), nz = guest_float(normal + 8u);
                double distance = guest_float(camera + 0x18Cu + plane * 4u);
                for (c = 0; c < 8; c++) {
                    double tx = corner[c][0] * nx, ty = corner[c][1] * ny, tz = corner[c][2] * nz;
                    double d = tx + ty + tz + distance;
                    double margin = 1e-4 * (fabs(tx) + fabs(ty) + fabs(tz) + fabs(distance)) + 1e-6;
                    if (d != d || fabs(d) <= margin)
                        return -1;
                    if (d < 0.0) behind[plane]++;
                    else in_front[c]++;
                }
            }
            for (plane = 0; plane < 6; plane++)
                if (behind[plane] == 8)
                    return 0;
            for (c = 0; c < 8; c++)
                if (in_front[c] == 6)
                    return 1;
        }
    }
    return -1;
}

/* RECOMP_CULL_STATS=1 reports how many tests were settled here;
 * RECOMP_CULL_VERIFY=1 also runs the original for every one and counts the
 * answers that differ. */
int sub_00021A90_lifted(void);

void sub_00021A90(void)
{
    static int stats = -1, verify;
    static unsigned long long settled, passed, differ;
    int answer = in_view(g_ecx, TUROK_MEM32(g_esp + 4), TUROK_MEM32(g_esp + 8));

    if (stats < 0) {
        verify = getenv("RECOMP_CULL_VERIFY") != NULL;
        stats = verify || getenv("RECOMP_CULL_STATS") != NULL;
    }
    if (stats && settled + passed >= 4000000u) {
        fprintf(stderr, "[CULL] in-view tests: %llu settled natively, %llu by the original, %llu differ\n",
                settled, passed, differ);
        settled = passed = differ = 0;
    }
    if (answer < 0 || verify) {
        s_cull_widen++;
        if (verify || !sub_00021A90_lifted()) sub_00021A90_gen();
        s_cull_widen--;
        if (answer < 0) passed++;
        else { settled++; if ((int)g_eax != answer) differ++; }
        return;
    }
    settled++;
    g_eax = (uint32_t)answer;
    g_esp += 12;   /* ret 8 */
}

/* ── Which screen the game is on (sub_00054A60) ────────────────
 *
 * The screen loader (this-call) builds y:\data\precachelists\autolists\
 * <name>.pcl from the name at this+0x20 as it enters a screen or a level. The
 * front end -- its screens, d:\data\levels\u_jcowlishaw\screens\<name>\ --
 * stays 4:3 in widescreen; a level (pod-...) is the game. The pause menu's
 * screens (ingame...) are the game's; its first (ingamepause) is told apart,
 * as Escape resumes from it. Returns 1 for the front end, 2 for that screen.
 * The loader's this is the screen object the menu pointer works on
 * (video_settings_menu_screen). (The PAL release has the loader at
 * sub_00054A80: found by phobos665's turok-evolution-recomp.) */
#include "video_settings.h"
extern void sub_00054A60_gen(void);

static int is_front_end_screen(const char *name)
{
    char lower[160];
    const char *screens;
    size_t n;
    for (n = 0; n + 1 < sizeof lower && name[n]; n++)
        lower[n] = (char)(name[n] >= 'A' && name[n] <= 'Z' ? name[n] + 32 : name[n]);
    lower[n] = 0;
    screens = strstr(lower, "\\screens\\");
    if (!screens) return 0;
    if (!strncmp(screens + 9, "ingamepause\\", 12)) return 2;
    return strncmp(screens + 9, "ingame", 6) != 0;
}

static int s_level_loading;   /* a level, not a front end screen, is loading */

/* The language Game Options' Language row asks for, made the game's with its
 * own switch, sub_00027EF0 (game object this-call, the language as its
 * argument: it unloads the text, sets [game+0xA8] and loads the text again;
 * the PAL release's language event, sub_000A5820, calls it). The USA game
 * sets American at start-up and never calls it. Made as a screen loads, so
 * leaving Game Options brings the next screen up in the new language. */
extern void sub_00027EF0(void);

static void follow_language(void)
{
    uint32_t game = TUROK_MEM32(0x250A84u), eax = g_eax, ecx = g_ecx, edx = g_edx, esp = g_esp;
    int language = video_settings_language();
    if (!language || !game || !TUROK_MEM32(game + 0x94u) || TUROK_MEM32(game + 0xA8u) == (uint32_t)language) return;
    fprintf(stderr, "[TUROK] language %u -> %d\n", TUROK_MEM32(game + 0xA8u), language);
    g_esp -= 4;
    TUROK_MEM32(g_esp) = (uint32_t)language;
    g_esp -= 4;
    TUROK_MEM32(g_esp) = 0;   /* return address */
    g_ecx = game;
    sub_00027EF0();
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_esp = esp;
}

void sub_00054A60(void)
{
    uint32_t self = g_ecx;
    uint32_t name = self ? TUROK_MEM32(self + 0x20u) : 0;
    follow_language();
    g_ecx = self;
    if (name) {
        const char *text = (const char *)((uintptr_t)name + g_xbox_mem_offset);
        int kind = is_front_end_screen(text);
        if (kind == 1 || !strstr(text, "creens\\")) {
            keep_decals_reset();
            aim_wobble_reset();
            particle_tracking_reset();
        }
        fprintf(stderr, "[TUROK] screen %.96s: %s\n", text, kind == 1 ? "front end" : kind ? "pause menu" : "game");
        if (!strstr(text, "creens\\")) {   /* a level: 4:3 and held until sub_000FFF20 */
            s_level_loading = 1;
            video_settings_loading(1);
            video_settings_level(text);
        }
        video_settings_screen(kind == 1, kind == 2);
        video_settings_menu_screen(self, text);
        if (!strstr(text, "creens\\")) {   /* a level: the actors watched before are gone */
            extern void corpse_watch_reset(void);
            corpse_watch_reset();
            flushed_birds_reset();
        }
    }
    sub_00054A60_gen();
}

/* ── The loading screen (game object vtable 0x209D60) ──────────
 *
 * The game object's this-calls at +0x2C (sub_000FFA00) put the loading
 * screen up -- the picture, title and text the level's LoadingScreen event
 * left at [0x257E14]+0x1CC.., and the spinner, in 0x257E28..0x257E40 -- +0x30
 * (sub_000FFDE0) is the loading tick, which draws and presents a frame each
 * time its timer comes round, and +0x38 (sub_000FFF20) takes it down as the
 * level begins; +0xB0 says it is up. Loads here take a fraction of the Xbox's
 * time, so a level's flashed past: it now stays at least
 * RECOMP_LOADING_HOLD_MS (default 3000) by ticking on before it comes down
 * (not the front end's own loads, such as its backdrop at boot). While up,
 * it is redrawn at the display's rate (RECOMP_LOADING_REDRAW).
 * The level stays 4:3 from its load (sub_00054A60) until then. */
extern void sub_000FFA00_gen(void);
extern void sub_000FFF20_gen(void);
extern void sub_000FFDE0_gen(void);
void sub_000FFDE0(void);
extern RECOMP_TLS uint32_t g_edx;
#ifdef _WIN32
__declspec(dllimport) void __stdcall Sleep(unsigned long milliseconds);
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#endif
static clock_t s_loading_since;     /* when the screen went up; 0 when down */

static long loading_hold_ms(void)
{
    static long ms = -1;
    if (ms < 0) {
        const char *value = getenv("RECOMP_LOADING_HOLD_MS");
        ms = value && *value ? atol(value) : 3000;
        if (ms < 0) ms = 0;
    }
    return ms;
}

/* RECOMP_LOADING_REDRAW=0: the loading screen redrawn every 0.2 s, as the
 * game's loading tick does; by default it is redrawn at the display's rate,
 * its spinner still turning at the Xbox's 5 frames a second. */
static int loading_redraw(void)
{
    static int on = -1;
    if (on < 0) {
        const char *value = getenv("RECOMP_LOADING_REDRAW");
        on = !(value && *value == '0');
    }
    return on;
}

static long loading_shown_ms(void)
{
    return (long)((clock() - s_loading_since) * 1000 / CLOCKS_PER_SEC);
}

static clock_t s_loading_drawn, s_loading_spun;   /* when the last redraw ended, the last spinner step */
static clock_t s_loading_cost;                    /* how long that redraw took */
static int s_loading_holding;                     /* in the hold (sub_000FFF20): nothing else to do */

void sub_000FFA00(void)
{
    if (!*(volatile uint8_t *)((uintptr_t)g_ecx + 0xB0u + g_xbox_mem_offset)) {
        s_loading_since = clock() | 1;
        s_loading_drawn = s_loading_spun = s_loading_cost = 0;
    }
    sub_000FFA00_gen();
}

/* The tick, from the loader between its reads and from the hold below. It
 * draws once its time passes 0.2 s ([0x24E1B0] against [0x209400]): given
 * the time whenever a redraw is due, it draws at up to 125 frames a second,
 * with the spinner -- the tick stores its next frame in [0x257E74] -- kept on
 * its frame until the Xbox's 0.2 s have gone by.
 *
 * Only a level's loading screen, which is held to RECOMP_LOADING_HOLD_MS
 * anyway, and with the redraws given at most half of the loader's time: the
 * next one is due no sooner after the last one ended than that one took
 * (8 ms at least). A draw waits for the display's next refresh, and the
 * loader ticks after every file: a front end screen ticks ~600 times in its
 * half second, and drawing each of those made it 4-10 s. Front end screens
 * draw every 0.2 s, as on the Xbox. */
static uint32_t s_tick_this;      /* the loading screen the loader last ticked */
static clock_t s_tick_seen;       /* when it did */
static unsigned long s_tick_thread; /* and on which thread */
static int s_tick_between;        /* inside loading_tick_between's tick */

static int loading_redraw_due(clock_t now)
{
    clock_t gap = CLOCKS_PER_SEC / 125;
    if (!s_level_loading || !loading_redraw()) return 0;
    if (!s_loading_holding && s_loading_cost > gap) gap = s_loading_cost;
    return !s_loading_drawn || now - s_loading_drawn >= gap;
}

void sub_000FFDE0(void)
{
    clock_t forced = 0;
    if (s_loading_since && !s_tick_between) {
        s_tick_this = g_ecx;
        s_tick_seen = clock() | 1;
        s_tick_thread = GetCurrentThreadId();
    }
    if (s_loading_since && *(volatile uint8_t *)((uintptr_t)g_ecx + 0xB0u + g_xbox_mem_offset)) {
        clock_t now = clock();
        if (loading_redraw_due(now)) {
            static const double due = 1.0;
            forced = now | 1;
            if (!s_loading_spun || now - s_loading_spun >= CLOCKS_PER_SEC / 5)
                s_loading_spun = now | 1;
            else if (TUROK_MEM32(0x257E2Cu) && TUROK_MEM32(0x257E74u))
                TUROK_MEM32(0x257E74u) -= 1;
            memcpy((void *)((uintptr_t)0x24E1B0u + g_xbox_mem_offset), &due, sizeof due);
        }
    }
    sub_000FFDE0_gen();
    if (forced) {
        clock_t end = clock();
        s_loading_cost = end - forced;
        s_loading_drawn = end | 1;
    }
}

/* The generated code's XMM register type (recomp_types.h, same guard). */
#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
typedef union RecompXmm {
    float    f[4];
    double   d[2];
    uint32_t u[4];
    int32_t  i[4];
    uint64_t q[2];
} RecompXmm;
#endif
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp;
extern RECOMP_TLS int g_fp_top, g_df;
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS uint16_t g_fp_cc;

/* The loader's longest stretch without a tick builds the level's shaders
 * (XGraphics, sub_001ABF4D, 5-17 ms each), a dozen at a time: 130-150 ms
 * with the loading screen standing still. After each one a tick is made
 * here when a frame is due, as the loader would between its steps -- only
 * while the loader itself is ticking (within the last half second), on the
 * screen it ticks, and with everything of the guest's put back afterwards:
 * the registers, the published frame, the x87 stack and its control word,
 * the XMM registers and the direction flag. Only when a redraw is due (above);
 * RECOMP_LOADING_REDRAW=0: not at all. */
static void loading_tick_between(void)
{
    clock_t now;
    if (!s_loading_since || !s_tick_this || s_tick_between) return;
    now = clock();
    if (now - s_tick_seen > CLOCKS_PER_SEC / 2 || GetCurrentThreadId() != s_tick_thread) return;
    if (!loading_redraw_due(now)) return;
    if (!*(volatile uint8_t *)((uintptr_t)s_tick_this + 0xB0u + g_xbox_mem_offset)) return;
    {
        uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, ebx = g_ebx, esi = g_esi, edi = g_edi, esp = g_esp;
        uint32_t ebp = g_ebp, seh_ebp = g_seh_ebp;
        int top = g_fp_top, df = g_df;
        double stack[8];
        uint16_t cc = g_fp_cc, control = g_fp_control_word;
        RecompXmm xmm[8] = {g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7};
        memcpy(stack, g_fp_stack, sizeof stack);
        s_tick_between = 1;
        g_esp -= 4;
        TUROK_MEM32(g_esp) = 0;   /* return address */
        g_ecx = s_tick_this;
        sub_000FFDE0();
        s_tick_between = 0;
        g_eax = eax; g_ecx = ecx; g_edx = edx; g_ebx = ebx; g_esi = esi; g_edi = edi; g_esp = esp;
        g_ebp = ebp; g_seh_ebp = seh_ebp;
        g_fp_top = top; g_df = df; g_fp_cc = cc; g_fp_control_word = control;
        memcpy(g_fp_stack, stack, sizeof stack);
        g_xmm0 = xmm[0]; g_xmm1 = xmm[1]; g_xmm2 = xmm[2]; g_xmm3 = xmm[3];
        g_xmm4 = xmm[4]; g_xmm5 = xmm[5]; g_xmm6 = xmm[6]; g_xmm7 = xmm[7];
    }
}

extern void sub_001ABF4D_gen(void);
void sub_001ABF4D(void)
{
    sub_001ABF4D_gen();
    loading_tick_between();
}

void sub_000FFF20(void)
{
    uint32_t self = g_ecx;
    if (s_level_loading && s_loading_since &&
            *(volatile uint8_t *)((uintptr_t)self + 0xB0u + g_xbox_mem_offset)) {
        fprintf(stderr, "[TUROK] level loaded in %ld ms; loading screen held to %ld ms%s\n",
                loading_shown_ms(), loading_hold_ms(), loading_redraw() ? ", redrawn each frame" : "");
        s_loading_holding = 1;
        while (loading_shown_ms() < loading_hold_ms()) {
            uint32_t eax = g_eax, edx = g_edx, esp = g_esp;
            g_esp -= 4;
            TUROK_MEM32(g_esp) = 0;   /* return address */
            g_ecx = self;
            sub_000FFDE0();
            g_eax = eax; g_edx = edx; g_esp = esp;
            Sleep(1);
        }
        s_loading_holding = 0;
    }
    if (s_loading_since && getenv("RECOMP_LOADING_LOG"))
        fprintf(stderr, "[TUROK] loading screen up %ld ms\n", loading_shown_ms());
    s_loading_since = 0;
    s_level_loading = 0;
    g_ecx = self;
    sub_000FFF20_gen();
    video_settings_loading(0);
}

/* ── In-engine cutscenes (StartCinema / EndCinema) ─────────────
 *
 * The level events of those names (built by sub_00105xxx's factory,
 * vtables 0x226AC8 and 0x226AD8) set and clear [[0x257E14]+0x1AC] in their
 * execute methods. A cutscene is framed for 4:3, with its own letterbox
 * bars, so widescreen shows it as it was made. */
extern void sub_00164230_gen(void);
extern void sub_001642A0_gen(void);

void sub_00164230(void)
{
    sub_00164230_gen();
    video_settings_cinema(1);
}

void sub_001642A0(void)
{
    sub_001642A0_gen();
    video_settings_cinema(0);
}

/* ── Raw mouse look (sub_0002F580) ─────────────────────────────
 *
 * The input mapper fills each player's axes (0x250BC0 + i*0x54, +0x08..+0x14:
 * move x, move y, look x, look y) from the pads once a frame; the player turns
 * by the look axes, unclamped (sub_0014E620 reads them). After it, each
 * frame's mouse counts are taken (video_settings_mouse_look) for the turn
 * functions below, and the menu pointer runs. */
extern void sub_0002F580_gen(void);
void sub_0002F580(void)
{
    sub_0002F580_gen();
    video_settings_mouse_look();
}

/* ── Draw distance (sub_0003C9E0) ──────────────────────────────
 *
 * An actor's distance fade (esi the actor, edi the camera; the factor on the
 * FPU stack): its block at +0x1CC holds A_NDD, where it starts to fade, and
 * A_MDD, where it is gone (grass and plants: 15 to 25 units), 0 for no limit.
 * For the draw distance setting the two are scaled while the fade is worked
 * out, then put back. */
extern RECOMP_TLS uint32_t g_esi;
/* the body: lifted or generated (the "Lifted leaves" section below) */
void sub_0003C9E0_lifted(void);

void sub_0003C9E0(void)
{
    float scale = video_settings_draw_distance(), fade_near, fade_far;
    uint32_t block = g_esi ? TUROK_MEM32(g_esi + 0x1CCu) : 0, near_bits, far_bits;

    if (scale == 1.0f || !block) { sub_0003C9E0_lifted(); return; }
    near_bits = TUROK_MEM32(block);
    far_bits = TUROK_MEM32(block + 4u);
    memcpy(&fade_near, &near_bits, 4);
    memcpy(&fade_far, &far_bits, 4);
    if (!(fade_near > 0.0f)) { sub_0003C9E0_lifted(); return; }
    fade_near *= scale;
    fade_far *= scale;
    memcpy((void *)((uintptr_t)block + g_xbox_mem_offset), &fade_near, 4);
    memcpy((void *)((uintptr_t)(block + 4u) + g_xbox_mem_offset), &fade_far, 4);
    sub_0003C9E0_lifted();
    TUROK_MEM32(block) = near_bits;
    TUROK_MEM32(block + 4u) = far_bits;
}

/* ── Update distance (sub_0003B110) ────────────────────────────
 *
 * cdecl (the actor): whether a player is within the actor's AU_D (+0x1D0,
 * 0 for always), the distance out to which it updates -- its animation, its
 * flight. Levels set it to match A_MDD, where the actor stops being drawn,
 * so with the draw distance setting butterflies were drawn hanging frozen
 * until close. Theirs is scaled with the draw distance. Visible pickups
 * must also keep updating regardless of the setting, or their rotation and
 * bobbing freeze a few metres away. So must visible wildlife -- the frogs,
 * fish, birds and other critters, whose AU_D is often shorter than the
 * distance they are drawn at even with the original draw distance (the
 * Xbox froze them in mid-hop too). +0x53 is set by the actor's draw
 * (sub_000448D0) when it is drawn this frame, so only what is on screen
 * gains updates. Off-screen pickups and other actors keep the original
 * cutoff, as more actors updating costs frame rate, and enemies keep it so
 * they do not wake from further away.
 * Butterflies are the FlyingIndigenousAI (vtable 0x21A240) with
 * FlyingPerturb (+0x7F8) 3: no bird, dragonfly or pteranodon has it. */
extern void sub_0003B110_gen(void);
int sub_0003B110_lifted(void);

static int wildlife_vtable(uint32_t table)
{
    return table == 0x0021B0D0u ||  /* IndigenousAI */
           table == 0x0021A7B0u ||  /* AquaticIndigenousAI */
           table == FLYING_INDIGENOUS_AI_VTABLE ||
           table == 0x0021C0C0u ||  /* AnimalAI */
           table == 0x0021BAF0u;    /* GroupAnimalAI */
}

void sub_0003B110(void)
{
    float scale = video_settings_draw_distance(), reach;
    uint32_t actor = TUROK_MEM32(g_esp + 4u), bits, table = actor ? TUROK_MEM32(actor) : 0;
    int drawn = actor && *(uint8_t *)((uintptr_t)(actor + 0x53u) + g_xbox_mem_offset);
    int visible_always = drawn &&
        (table == 0x001FCFB8u || table == 0x00210450u || table == 0x002255F0u || /* Pickup, TurokPickup, MPPickup */
         wildlife_vtable(table));

    if (!visible_always && (scale == 1.0f || table != FLYING_INDIGENOUS_AI_VTABLE ||
        TUROK_MEM32(actor + 0x7F8u) != 3u)) {
        if (!sub_0003B110_lifted()) sub_0003B110_gen();
        /* Name any other class seen frozen on screen, once each, so the
         * next report of a frozen creature says what it is. */
        if (drawn && !g_eax) {
            static uint32_t seen[32];
            static int count;
            int i;
            for (i = 0; i < count && seen[i] != table; i++) {}
            if (i == count && count < 32) {
                seen[count++] = table;
                bits = TUROK_MEM32(actor + 0x1D0u);
                memcpy(&reach, &bits, 4);
                fprintf(stderr, "[UPDATE] drawn but beyond its update distance: vtable 0x%06X, AU_D %.1f\n",
                        table, reach);
            }
        }
        return;
    }
    bits = TUROK_MEM32(actor + 0x1D0u);
    memcpy(&reach, &bits, 4);
    if (!(reach > 0.0f)) { sub_0003B110_gen(); return; }
    reach = visible_always ? 0.0f : reach * scale; /* AU_D zero means unlimited. */
    memcpy((void *)((uintptr_t)(actor + 0x1D0u) + g_xbox_mem_offset), &reach, 4);
    sub_0003B110_gen();
    TUROK_MEM32(actor + 0x1D0u) = bits;
}

/* ── Stick response curve (sub_00142280) ───────────────────────
 *
 * this-call (the controller), args: float *x, float *y, and a third. For
 * the stick vector's length m: under 0.5 both are scaled by 0.25, under 0.9
 * by 0.3, and above it each is squared (keeping its sign). */
extern void sub_00142280_gen(void);
extern int video_settings_modern_aim(void);

/* One radial deadzone, then a continuous linear magnitude. Clamp the radius
 * at full deflection without changing direction, including square-gate pads. */
static void modern_look_vector(float *x, float *y)
{
    const float deadzone = 0.15f;
    float radius, gain;
    if (!isfinite(*x) || !isfinite(*y)) { *x = *y = 0; return; }
    *x = fmaxf(-1.0f, fminf(1.0f, *x));
    *y = fmaxf(-1.0f, fminf(1.0f, *y));
    radius = sqrtf(*x * *x + *y * *y);
    if (radius <= deadzone) { *x = *y = 0; return; }
    gain = (fminf(radius, 1.0f) - deadzone) / ((1.0f - deadzone) * radius);
    *x *= gain;
    *y *= gain;
}

static int modern_look_curve(uint32_t ret, uint32_t self, uint32_t xp, uint32_t yp)
{
    float x, y;
    uint32_t xb, yb;
    /* This curve is also used for movement and vehicles. Only the on-foot
     * look caller has the axial deadzone and easing being replaced here. */
    if (ret != 0x00144848u || !video_settings_modern_aim()) return 0;
    x = guest_float(self + 0x192Cu) - guest_float(self + 0x1928u);
    y = guest_float(self + 0x1920u) - guest_float(self + 0x1924u);
    modern_look_vector(&x, &y);
    memcpy(&xb, &x, 4); memcpy(&yb, &y, 4);
    TUROK_MEM32(xp) = xb; TUROK_MEM32(yp) = yb;
    /* The caller eases towards its old per-axis values. Seed those with
     * this vector to preserve its direction and avoid frame-based delay.
     * The original yaw/pitch functions still handle dt, sensitivity, zoom,
     * invert look and pitch limits, and add raw mouse input afterwards. */
    TUROK_MEM32(self + 0x440u) = xb;
    TUROK_MEM32(self + 0x444u) = yb;
    return 1;
}

void sub_00142280(void)
{
    static int trace = -1;
    uint32_t ret = TUROK_MEM32(g_esp), xp = TUROK_MEM32(g_esp + 4u), yp = TUROK_MEM32(g_esp + 8u), self = g_ecx;
    float x0, y0, x1, y1;
    if (trace < 0) trace = getenv("RECOMP_CURVE_TRACE") != NULL;
    memcpy(&x0, (void *)((uintptr_t)xp + g_xbox_mem_offset), 4);
    memcpy(&y0, (void *)((uintptr_t)yp + g_xbox_mem_offset), 4);
    if (modern_look_curve(ret, self, xp, yp)) g_esp += 16;  /* ret 12 */
    else sub_00142280_gen();
    if (trace && (x0 != 0.0f || y0 != 0.0f)) {
        static unsigned n;
        memcpy(&x1, (void *)((uintptr_t)xp + g_xbox_mem_offset), 4);
        memcpy(&y1, (void *)((uintptr_t)yp + g_xbox_mem_offset), 4);
        if (n++ % 15 == 0)
            fprintf(stderr, "[CURVE] from %06X this %08X x %.3f y %.3f -> %.3f %.3f\n", ret, self, x0, y0, x1, y1);
    }
}

/* ── Native mouse look (sub_00144430 yaw, sub_00144560 pitch) ──
 *
 * The player controller's turn functions, after the stick's deadzone,
 * response curve and easing (sub_00144780): yaw takes (.., speed, frame
 * time, x) as arguments 4-6, pitch (.., frame time, x, y) as 8-10, and turn
 * by value x speed x frame time x a zoom factor. The mouse's turn
 * (video_settings_take_mouse_turn) is added to the value over the frame
 * time, so the view turns by the mouse's counts alone. */
extern void sub_00144430_gen(void);
extern void sub_00144560_gen(void);

static void add_mouse_turn(uint32_t time_arg, uint32_t value_arg, int axis)
{
    float frame_time, value, turn;
    uint32_t bits = TUROK_MEM32(time_arg);
    memcpy(&frame_time, &bits, 4);
    if (!(frame_time > 0.0f) || frame_time > 1.0f) return;
    turn = video_settings_take_mouse_turn(axis);
    if (turn == 0.0f) return;
    bits = TUROK_MEM32(value_arg);
    memcpy(&value, &bits, 4);
    value += turn / frame_time;
    memcpy(&bits, &value, 4);
    TUROK_MEM32(value_arg) = bits;
}

void sub_00144430(void)
{
    add_mouse_turn(g_esp + 0x14u, g_esp + 0x18u, 0);
    sub_00144430_gen();
}

void sub_00144560(void)
{
    add_mouse_turn(g_esp + 0x20u, g_esp + 0x28u, 1);
    sub_00144560_gen();
}

/* ── Mouse steering on the pteranodon (sub_001424B0) ──────────
 *
 * The flight update (sub_00150590) has this make its two stick axes from
 * the player's analog actions: +0x434 up (+0x1920 less +0x1924), +0x430
 * right (+0x192C less +0x1928), the latter halved under 0.526 and squared
 * above. sub_001460B0 speeds the pteranodon across its flight path by them.
 * The mouse (video_settings_take_mouse_steer: right and up, by Flight mouse
 * and Flight speed) moves a stick of its own that springs back to the centre
 * in about a sixth of a second, added to the pad's: the mouse up the screen
 * climbs, as it looks up on foot. */
extern void sub_001424B0_gen(void);

void sub_001424B0(void)
{
    static double last = -1;
    static float stick[2];
    uint32_t self = g_ecx;
    struct timespec now;
    double seconds;
    float frame_time, keep;
    int axis;

    sub_001424B0_gen();
    timespec_get(&now, TIME_UTC);
    seconds = (double)now.tv_sec + now.tv_nsec * 1e-9;
    frame_time = last < 0 ? 1.0f : (float)(seconds - last);
    last = seconds;
    /* A pause or a fresh flight starts the mouse's stick centred. */
    keep = frame_time > 0.0f && frame_time < 0.25f ? expf(-frame_time / 0.15f) : 0.0f;
    for (axis = 0; axis < 2; axis++) {
        float steer = video_settings_take_mouse_steer(axis), value;
        uint32_t va = self + (axis ? 0x434u : 0x430u), bits;
        stick[axis] = stick[axis] * keep + steer;
        if (stick[axis] > 1.0f) stick[axis] = 1.0f;
        if (stick[axis] < -1.0f) stick[axis] = -1.0f;
        if (stick[axis] == 0.0f) continue;
        value = guest_float(va) + stick[axis];
        if (value > 1.0f) value = 1.0f;
        if (value < -1.0f) value = -1.0f;
        memcpy(&bits, &value, 4);
        TUROK_MEM32(va) = bits;
    }
}

/* ── Sound volume (sub_000DD7E0) ── DIAG RECOMP_SNDVOL_TRACE ──
 * A sound's volume: its level (+0x1C of the first argument) x a group
 * factor (sub_000D5A20) x the fourth argument, as 20 log10 dB. */
extern void sub_000DD7E0_gen(void);

void sub_000DD7E0(void)
{
    static int trace = -1;
    if (trace < 0) trace = getenv("RECOMP_SNDVOL_TRACE") != NULL;
    if (trace) {
        static unsigned n;
        uint32_t a1 = TUROK_MEM32(g_esp + 4u), level_bits = a1 ? TUROK_MEM32(a1 + 0x1Cu) : 0, a4 = TUROK_MEM32(g_esp + 0x10u);
        float level, scale;
        memcpy(&level, &level_bits, 4);
        memcpy(&scale, &a4, 4);
        if (n++ < 200)
            fprintf(stderr, "[SNDVOL] from %06X sound %08X level %.4f x %.4f (a2 %08X a3 %08X)\n", TUROK_MEM32(g_esp),
                    a1, level, scale, TUROK_MEM32(g_esp + 8u), TUROK_MEM32(g_esp + 0xCu));
    }
    sub_000DD7E0_gen();
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    if (xbox_va == 0x0004E1A0u) return video_settings_draw_menu_widget;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ── Flight-path turns skip rotation collision (sub_00042450) ──
 * The movement integrator (thiscall; arg 1 an absolute orientation
 * quaternion, arg 2 a rotation delta, then position arguments) turns an
 * actor to arg 1 in collision-checked steps unless its flags (+0x34 bit 2)
 * say rotation ignores collision, when it simply takes the quaternion. The
 * scripted flight path's mover (sub_00080CD0, returning to 0x80EB1) calls
 * it with the path's orientation and nothing else. Escape Flight's VTOLs
 * climb to the play area's ceiling on such a path; at 120 fps their smaller
 * steps bring them close enough that turning the box touches the ceiling,
 * and the blocked turn left the ship tumbling in every direction. A path's
 * own orientation is not something collision should bend: for that caller
 * the turn is taken as given. RECOMP_PATH_TURN_DIRECT=0 turns this off. */
extern void sub_00042450_gen(void);
void sub_00042450(void)
{
    static int direct = -1;
    uint32_t actor = g_ecx, flags;
    int lift;
    if (direct < 0) {
        const char *value = getenv("RECOMP_PATH_TURN_DIRECT");
        direct = !(value && *value == '0');
    }
    flags = TUROK_MEM32(actor + 0x34u);
    lift = direct && TUROK_MEM32(g_esp) == 0x00080EB1u && TUROK_MEM32(g_esp + 4) && !(flags & 4u);
    if (lift) TUROK_MEM32(actor + 0x34u) = flags | 4u;
    sub_00042450_gen();
    if (lift) TUROK_MEM32(actor + 0x34u) &= ~4u;
}

/* ── Path component trace (RECOMP_PATH_TRACE=1) ──
 * A path component (vtable 0x1FBE48) moves an actor along a path: lifts,
 * doors, flight paths. +4 the path, +8 the position on it (points 0..n-1),
 * +0x24 the direction (1 forward), +0x25 paused. Its update (sub_00080EE0,
 * thiscall: actor, dt, ...) runs the follower; SetDirection
 * (sub_0007F820, vtable +0x20) and the command handler (sub_00080240,
 * vtable +4: a bit mask, 0x40 forward, 0x80 back, 0x100 reverse) turn it.
 * Logs each direction change and command with the return addresses on the
 * stack, and moving components twice a second. */
extern void sub_0007F820_gen(void);
extern void sub_00080240_gen(void);
extern void sub_00080EE0_gen(void);
static int path_trace(void)
{
    static int trace = -1;
    if (trace < 0) {
        const char *value = getenv("RECOMP_PATH_TRACE");
        trace = value && *value && *value != '0';
    }
    return trace;
}
static unsigned s_path_lines;
static uint32_t path_ms(void) { return (uint32_t)((double)clock() * 1000.0 / CLOCKS_PER_SEC); }
static void path_callers(char *out, size_t size)
{
    size_t used = 0;
    int found = 0;
    out[0] = 0;
    for (uint32_t at = g_esp; at < g_esp + 0x400u && found < 14; at += 4) {
        uint32_t value = TUROK_MEM32(at);
        if (value >= 0x11000u && value < 0x1F0000u) {
            int n = snprintf(out + used, size - used, " %08X", value);
            if (n < 0 || (size_t)n >= size - used) break;
            used += (size_t)n;
            found++;
        }
    }
}
/* A trigger action (sub_00163180, thiscall: ..., ..., target) opens
 * (+0x28 == 0) or closes a door; the trace logs it with the stack. */
extern void sub_00163180_gen(void);
void sub_00163180(void)
{
    uint32_t action = g_ecx, target = TUROK_MEM32(g_esp + 0xCu);
    if (path_trace() && s_path_lines < 20000u) {
        char callers[160];
        path_callers(callers, sizeof callers);
        s_path_lines++;
        fprintf(stderr, "[PATH] door action %08X %s target %08X (vtable %08X), tick %lu; stack%s\n",
                action, TUROK_MEM32(action + 0x28u) ? "close" : "open", target, target ? TUROK_MEM32(target) : 0u,
                (unsigned long)path_ms(), callers);
    }
    sub_00163180_gen();
}

/* ── Trigger zones: a point on a box zone's face counts as inside ──
 * An actor's zone membership (sub_00046480, every frame) tests its position
 * in each nearby zone's space against the zone's shapes (sub_000832A0); a
 * box shape (sub_000B6490, this +0x34.. the half sizes, arg the point)
 * holds it when -half <= p <= half on every axis. The Senate Chambers' end
 * lift is started by a zone whose box ends exactly at the shaft wall, and a
 * player riding against that wall sits on its face: at 144 fps the
 * collision rests the player 0.0001 inside going up and 0.0004 outside going down,
 * so the zone's leave and enter events (close and open the lift) took turns
 * and the lift bounced in the shaft for ever, short of the top. For zone
 * tests only (the call from 0x832F0), a box holds points within a
 * hundredth of a unit of its faces. RECOMP_ZONE_TOLERANCE=0 turns it off. */
extern void sub_000B6490_gen(void);
void sub_000B6490(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("RECOMP_ZONE_TOLERANCE");
        enabled = !(value && *value == '0');
    }
    if (enabled && TUROK_MEM32(g_esp) == 0x000832F0u) {
        uint32_t box = g_ecx, point = TUROK_MEM32(g_esp + 4u);
        int axis, inside = 1;
        for (axis = 0; axis < 3; axis++) {
            float p = guest_float(point + 4u * (uint32_t)axis);
            float half = guest_float(box + 0x34u + 4u * (uint32_t)axis) + 0.01f;
            if (!(p >= -half && p <= half)) inside = 0;
        }
        g_eax = (uint32_t)inside;
        g_esp += 8;   /* ret 4 */
        return;
    }
    sub_000B6490_gen();
}

void sub_0007F820(void)
{
    uint32_t component = g_ecx;
    uint32_t direction = TUROK_MEM32(g_esp + 4u) & 0xFFu;
    uint32_t before = component ? TUROK_MEM32(component + 0x24u) & 0xFFu : direction;
    if (path_trace() && component && before != direction && s_path_lines < 20000u) {
        char callers[160];
        path_callers(callers, sizeof callers);
        s_path_lines++;
        fprintf(stderr, "[PATH] %08X direction %u -> %u at %.4f, paused %u, tick %lu; stack%s\n",
                component, before, direction, guest_float(component + 8u),
                (TUROK_MEM32(component + 0x24u) >> 8) & 0xFFu, (unsigned long)path_ms(), callers);
    }
    sub_0007F820_gen();
}
void sub_00080240(void)
{
    uint32_t component = g_ecx;
    uint32_t command = TUROK_MEM32(g_esp + 4u), value = TUROK_MEM32(g_esp + 8u);
    if (path_trace() && component && command && s_path_lines < 20000u) {
        char callers[160];
        path_callers(callers, sizeof callers);
        s_path_lines++;
        fprintf(stderr, "[PATH] %08X command %X value %X at %.4f, direction %u, paused %u, tick %lu; stack%s\n",
                component, command, value, guest_float(component + 8u),
                TUROK_MEM32(component + 0x24u) & 0xFFu, (TUROK_MEM32(component + 0x24u) >> 8) & 0xFFu,
                (unsigned long)path_ms(), callers);
    }
    sub_00080240_gen();
}
static int fall_trace(void);   /* RECOMP_FALL_TRACE, below */
static uint32_t s_player_path;  /* the local player's path component, last seen */

/* ── A path event re-fired on contact at high frame rates (issue #25) ──
 *
 * The "put on a path" event (sub_000A7F50, this-call (?, ?, actor), ret 0xC;
 * +0x28 bit 1, the path at +0x30) gives the actor's path component
 * ([actor+0x1F4]+0x20) the path and starts it from 0, then commands it.
 * Withering Heights' trampolines (TreehouseJumpers) fire it on contact: the
 * player is carried up an arc to the next level. At 60 fps the first step
 * of the arc lifts the player clear of the pad; at 400+ fps the first steps
 * are so small that the player still touches it, the contact fires the event
 * again, and the arc restarts from 0 -- dozens of times, the player held on
 * the pad (and, snapped down each restart, killed by "fall" damage). Above
 * 60 fps, the same event firing again for the same actor while its path has
 * run under a quarter of a second is ignored. At 60 fps and below nothing
 * changes. RECOMP_PATH_RESTART_FIX=0 runs every firing. */
extern void sub_000A7F50_gen(void);
void sub_000A7F50(void)
{
    static int enabled = -1;
    static uint32_t last_event, last_actor;
    uint32_t event = g_ecx, actor = TUROK_MEM32(g_esp + 0xCu);
    if (enabled < 0) enabled = !(getenv("RECOMP_PATH_RESTART_FIX") && *getenv("RECOMP_PATH_RESTART_FIX") == '0');
    if (enabled && event && actor && (TUROK_MEM32(event + 0x28u) & 2u)) {
        uint32_t holder = TUROK_MEM32(actor + 0x1F4u), component = holder ? TUROK_MEM32(holder + 0x20u) : 0u;
        float seconds = frame_seconds(), at = component ? guest_float(component + 8u) : 0.0f;
        if (event == last_event && actor == last_actor && component && at > 0.0f && at < 0.25f &&
            seconds > 0.0f && seconds < 1.0f / 60.0f) {
            if (fall_trace()) {
                static unsigned shown;
                if (shown++ < 50u)
                    fprintf(stderr, "[PATHSTART] repeat of event %08X ignored at %.5f\n", event, at);
            }
            g_esp += 16u;   /* ret 0xC */
            return;
        }
        last_event = event;
        last_actor = actor;
    }
    sub_000A7F50_gen();
}

/* The path position setter (sub_0007FA50, this-call (position), ret 4) on the
 * local player's path under RECOMP_FALL_TRACE: each set to the start (0)
 * with the code addresses on the guest stack -- the first 200. */
extern void sub_0007FA50_gen(void);
void sub_0007FA50(void)
{
    static unsigned shown;
    uint32_t component = g_ecx;
    if (fall_trace() && component && component == s_player_path && guest_float(g_esp + 4u) == 0.0f &&
        shown < 200u) {
        char callers[160];
        shown++;
        path_callers(callers, sizeof callers);
        fprintf(stderr, "[PATHSTART] %08X from %.5f; frame %.5f s; stack%s\n", component,
                guest_float(component + 8u), frame_seconds(), callers);
    }
    sub_0007FA50_gen();
}

void sub_00080EE0(void)
{
    static uint32_t seen[16], last_tick[16];
    uint32_t component = g_ecx, actor = TUROK_MEM32(g_esp + 4u);
    float dt = guest_float(g_esp + 8u), before = component ? guest_float(component + 8u) : 0.0f;
    sub_00080EE0_gen();
    /* The local player on a path (a trampoline's arc): every update, moving
     * or not, the first 1500 -- under RECOMP_FALL_TRACE as well. */
    if ((path_trace() || fall_trace()) && component && actor && actor == TUROK_MEM32(0x257E90u)) {
        static unsigned shown;
        s_player_path = component;
        if (shown++ < 1500u)
            fprintf(stderr, "[PATHP] %08X %.5f -> %.5f of %u points, direction %u paused %u, dt %.5f, "
                            "at %.3f %.3f %.3f; words %08X %08X %08X %08X %08X %08X\n",
                    component, before, guest_float(component + 8u),
                    TUROK_MEM32(TUROK_MEM32(component + 4u) + 0x14u), TUROK_MEM32(component + 0x24u) & 0xFFu,
                    (TUROK_MEM32(component + 0x24u) >> 8) & 0xFFu, dt, guest_float(actor + 0x38u),
                    guest_float(actor + 0x3Cu), guest_float(actor + 0x40u), TUROK_MEM32(component + 0xCu),
                    TUROK_MEM32(component + 0x10u), TUROK_MEM32(component + 0x14u), TUROK_MEM32(component + 0x18u),
                    TUROK_MEM32(component + 0x1Cu), TUROK_MEM32(component + 0x20u));
    }
    if (path_trace() && component && s_path_lines < 20000u) {
        float after = guest_float(component + 8u);
        uint32_t now = path_ms();
        unsigned slot;
        if (after == before) return;
        for (slot = 0; slot < 16 && seen[slot] && seen[slot] != component; slot++) {}
        if (slot == 16) {                       /* all taken: reuse the one quiet longest */
            unsigned oldest = 0;
            for (slot = 1; slot < 16; slot++)
                if (now - last_tick[slot] > now - last_tick[oldest]) oldest = slot;
            slot = oldest;
            seen[slot] = 0;
        }
        if (seen[slot] && now - last_tick[slot] < 500u) return;
        seen[slot] = component;
        last_tick[slot] = now;
        s_path_lines++;
        fprintf(stderr, "[PATH] %08X moving %.4f -> %.4f of %u points, direction %u, actor %08X (vtable %08X) at %.3f %.3f %.3f, dt %.5f, tick %lu\n",
                component, before, after, TUROK_MEM32(TUROK_MEM32(component + 4u) + 0x14u),
                TUROK_MEM32(component + 0x24u) & 0xFFu, actor, actor ? TUROK_MEM32(actor) : 0u,
                actor ? guest_float(actor + 0x38u) : 0.0f, actor ? guest_float(actor + 0x3Cu) : 0.0f,
                actor ? guest_float(actor + 0x40u) : 0.0f, dt, (unsigned long)now);
    }
}


/* ── Weapons on the number keys (keyboard) ──────────────────────
 *
 * A PC addition: 1-9 and 0 pick a weapon directly, a second press of the
 * same key the next weapon sharing it. The pad's next/previous weapon
 * only queue steps (player vfunc +0x250/+0x254 add to the inventory's +0x2C
 * and arm its 0.2 s timer at +0x30); the inventory's update (sub_00160760,
 * this = player + 0x19FC) walks them and switches with sub_001605A0(index).
 * A key calls that same switch, from the same update, so the game changes
 * weapon as it does for a step: its own checks (sub_0015FDA0: owned, with
 * ammunition), animation and timing. Entries (0x28 bytes, from [+4] to [+8])
 * hold the weapon's ID first; [+0x5C] is the one being switched to. Not
 * while the inventory is locked ([+0x1C], [+0x34], as the steps) or has a
 * weapon to return to ([+0x38], sub_0015FFC0/sub_00160680). Keyboard and
 * mouse drive controller 1, so the keys act only while there is one player
 * (the list at 0x257E90, linked through +0x39C), not in split-screen. */
extern void sub_00160760_gen(void);
extern void sub_0015FDA0(void);
extern void sub_001605A0(void);
extern RECOMP_TLS uint32_t g_eax, g_edx, g_esp;

/* Weapon IDs per key, '1' to '9' then '0' (the table at 0x24B790): 1 War
 * Club, 2 Bow, 3 Tek Bow, 4 Spiked Mine, 5 Pistol, 6 Shotgun, 7 Minigun,
 * 8 Rocket Launcher, 9 Tek Weapon (Plasma Cannon), 10 Flamethrower,
 * 11 Guided Device (Spider Mine), 12 Gravity Disruptor, 13 Dark Matter
 * Cube, 14 Crossbow. Spiked Mine is the grenade weapon. Crossbow shares
 * the bow slot if available. */
static const signed char k_weapon_keys[VIDEO_WEAPON_SLOTS][4] = {
    {1, -1}, {2, 3, 14, -1}, {5, -1}, {6, -1}, {8, -1},
    {9, -1}, {7, -1}, {10, -1}, {12, -1}, {4, 11, 13, -1},
};

/* An inventory method taking an entry index (thiscall, ret 4); its eax. */
static uint32_t inventory_call(void (*method)(void), uint32_t inventory, uint32_t index)
{
    uint32_t ecx_saved = g_ecx, eax_saved = g_eax, edx_saved = g_edx, result;
    g_esp -= 4; TUROK_MEM32(g_esp) = index;
    g_esp -= 4; TUROK_MEM32(g_esp) = 0;      /* return address; its ret 4 pops both */
    g_ecx = inventory;
    method();
    result = g_eax;
    g_ecx = ecx_saved; g_eax = eax_saved; g_edx = edx_saved;
    return result;
}

static void weapon_key(uint32_t inventory, int key)
{
    uint32_t begin = TUROK_MEM32(inventory + 4u), end = TUROK_MEM32(inventory + 8u);
    uint32_t count = end > begin ? (end - begin) / 0x28u : 0;
    uint32_t current = TUROK_MEM32(inventory + 0x5Cu), current_id, i;
    int length = 0, position = -1, step;
    const signed char *group = k_weapon_keys[key];

    if (!count || count > 32) return;
    current_id = current < count ? TUROK_MEM32(begin + current * 0x28u) : 0xFFFFFFFFu;
    while (length < 4 && group[length] >= 0) {
        if ((uint32_t)group[length] == current_id) position = length;
        length++;
    }
    /* The next of the key's weapons after the one in hand (the first if
     * the one in hand is not the key's), skipping any that cannot be used. */
    for (step = 1; step <= length; step++) {
        uint32_t id = (uint32_t)group[(position + step) % length];
        for (i = 0; i < count; i++)
            if (TUROK_MEM32(begin + i * 0x28u) == id) break;
        if (i == count || i == current) continue;
        if (inventory_call(sub_0015FDA0, inventory, i) && inventory_call(sub_001605A0, inventory, i)) {
            TUROK_MEM32(inventory + 0x2Cu) = 0;     /* a wheel step queued before does not undo it */
            return;
        }
    }
}

void sub_00160760(void)
{
    static unsigned held;                 /* keys down at the last update */
    static int keyboard = -1;
    uint32_t inventory = g_ecx, player = TUROK_MEM32(inventory + 0x4Cu);
    uint32_t first = TUROK_MEM32(0x257E90u);
    unsigned down, pressed;
    int key;

    if (keyboard < 0) {
        const char *value = getenv("RECOMP_KBM");
        keyboard = value && *value && *value != '0';
    }
    if (!keyboard) {
        sub_00160760_gen();
        return;
    }

    down = video_settings_weapon_input();
    pressed = down & ~held;
    held = down;
    if (pressed && player && player == first && !TUROK_MEM32(first + 0x39Cu) &&
            !TUROK_MEM32(inventory + 0x1Cu) && !TUROK_MEM32(inventory + 0x34u) &&
            TUROK_MEM32(inventory + 0x38u) == 0xFFFFFFFFu)
        for (key = 0; key < VIDEO_WEAPON_SLOTS; key++)
            if (pressed & (1u << key)) { weapon_key(inventory, key); break; }
    g_ecx = inventory;
    sub_00160760_gen();
}

/* ── Keep weapons (PC Settings, off by default) ─────────────────
 *
 * Each level rebuilds the player's inventory at its start (sub_00160BC0,
 * this = player + 0x19FC) from the level's weapon set -- an actor whose
 * properties go to [0x257E14] + 0x198, flag +0x19C: eight WeaponSlotN (the
 * weapon's ID - 1, +0x234), StartsOffWithWeaponN (+0x294), upgrades and
 * ammunition -- and the saved weapon state, SaveFile.hst: six items a
 * weapon from item (ID - 1) * 6 + 1 (owned, upgrade 1, upgrade 2, three
 * ammunition counts), written from the inventory at each level's end and
 * save (sub_00101060 -> sub_00160340) and kept in the save. A weapon is
 * owned (entry +0x20) only if the level starts you with it AND the save
 * says you own it, so a level can take weapons away but never give them
 * back; and a weapon outside its eight slots is not in the inventory at
 * all (its saved state waits for a later level). That is the cut content
 * showing: weapons vanish between chapters, ammunition for weapons you
 * cannot have, the All Weapons cheat limited to the level's set.
 *
 * With the option on, every weapon the save says you own is yours: owned
 * if the level has it, otherwise added with its saved ammunition and
 * upgrades (as the build does for a level without a weapon set), in ID
 * order. With the All Weapons cheat on, all fourteen are in. This happens
 * just before the build's last step (sub_0015F400, which pools the bows'
 * arrows), so that sees them too. Not in the flying levels. Nothing is
 * written to the save that the game would not write: at the level's end
 * it saves the inventory as always, now with the weapons kept. */
extern void sub_00160BC0_gen(void);
extern void sub_0015F400_gen(void);
extern void sub_00160A50(void);

static uint32_t s_keep_inventory;     /* the inventory being built, while sub_00160BC0 runs */

/* A history group's items (8 bytes each, the value first; item k at k - 1)
 * if it has at least `least`, or 0. */
static uint32_t history_items(const char *name, size_t name_size, uint32_t least)
{
    uint32_t game = TUROK_MEM32(0x250A84u), manager, first, last, at;
    if (!game || !(manager = TUROK_MEM32(game + 0x80u))) return 0;
    first = TUROK_MEM32(manager); last = TUROK_MEM32(manager + 4u);
    if (last < first || last - first > 1024u) return 0;
    for (at = first; at < last; at += 4) {
        uint32_t group = TUROK_MEM32(at), text = group ? TUROK_MEM32(group + 0x14u) : 0, i;
        if (!text) continue;
        for (i = 0; i < name_size; i++) {
            char c = *(volatile char *)((uintptr_t)(text + i) + g_xbox_mem_offset), n = name[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (n >= 'A' && n <= 'Z') n += 32;
            if (c != n) break;
        }
        if (i == name_size) {
            uint32_t begin = TUROK_MEM32(group + 4u), end = TUROK_MEM32(group + 8u);
            return end >= begin + least * 8u ? begin : 0;
        }
    }
    return 0;
}

/* SaveFile.hst: six items a weapon, 85 in all. */
static uint32_t save_history_items(void)
{
    static const char name[] = "$\\Data\\History\\SaveFile.hst";
    return history_items(name, sizeof name, 85u);
}

/* The All Weapons cheat, Cheats.hst key 1 (its first item; key 2 is
 * Unlimited Ammo -- the PC release's data\history\cheats.hst names them).
 * The level start (sub_00153A00) copies them to the player, All Weapons to
 * +0x3BC and Unlimited Ammo to +0x3B8, only AFTER it has built the
 * inventory (0x153B59 vs 0x153EE3), so during the build the player's copies
 * still hold the old values (none on a fresh player) -- read the history
 * itself. Attachments need nothing more: with +0x3BC set the weapon-mode
 * lookup (sub_0015F1E0) takes every mode of a weapon in the inventory as
 * available. */
static int all_weapons_cheat(uint32_t player)
{
    static const char name[] = "$\\Data\\History\\Cheats.hst";
    uint32_t items = history_items(name, sizeof name, 1u);
    if (items) return TUROK_MEM32(items) != 0;
    return player && TUROK_MEM32(player + 0x3BCu) != 0;
}

static int32_t saved_weapon(uint32_t items, uint32_t id, int field)
{
    return (int32_t)TUROK_MEM32(items + ((id - 1u) * 6u + (uint32_t)field) * 8u);
}

static int find_weapon(uint32_t inventory, uint32_t id)
{
    uint32_t begin = TUROK_MEM32(inventory + 4u), end = TUROK_MEM32(inventory + 8u), i;
    for (i = 0; begin + (i + 1u) * 0x28u <= end && i < 32; i++)
        if (TUROK_MEM32(begin + i * 0x28u) == id) return (int)i;
    return -1;
}

static void keep_weapons(uint32_t inventory)
{
    uint32_t items = save_history_items(), player = TUROK_MEM32(inventory + 0x4Cu), id;
    int cheat = all_weapons_cheat(player);
    char kept[64] = "", added[64] = "";
    if (!items) return;
    for (id = 2; id <= 14; id++) {
        uint32_t begin, entry, count, max = 0x002259A0u + id * 16u;
        int at, position, field;
        if (saved_weapon(items, id, 0) == 0 && !cheat) continue;
        /* The bow stays beside the Tek Bow: the game holds both (level sets
         * such as BrandWheel/bWheel1 list both; sub_0015F400 pools the
         * arrows of the bow, the Tek Bow and the crossbow). */
        if ((at = find_weapon(inventory, id)) >= 0) {
            entry = TUROK_MEM32(inventory + 4u) + (uint32_t)at * 0x28u;
            if (!TUROK_MEM32(entry + 0x20u)) {
                TUROK_MEM32(entry + 0x20u) = 1;
                snprintf(kept + strlen(kept), sizeof kept - strlen(kept), " %u", id);
            }
            continue;
        }
        /* Append it (sub_00160A50(id, 0): this-call, ret 8), then fill it in
         * as the build does without a weapon set (0x160EE0..0x160FDF). */
        {
            uint32_t ecx_saved = g_ecx, eax_saved = g_eax, edx_saved = g_edx;
            g_esp -= 4; TUROK_MEM32(g_esp) = 0;
            g_esp -= 4; TUROK_MEM32(g_esp) = id;
            g_esp -= 4; TUROK_MEM32(g_esp) = 0;      /* return address; its ret 8 pops all three */
            g_ecx = inventory;
            sub_00160A50();
            g_ecx = ecx_saved; g_eax = eax_saved; g_edx = edx_saved;
        }
        if ((at = find_weapon(inventory, id)) < 0) continue;
        begin = TUROK_MEM32(inventory + 4u);
        entry = begin + (uint32_t)at * 0x28u;
        if (id == 2 || id == 3 || id == 14) {         /* bows: both arrow kinds */
            TUROK_MEM32(entry + 0x14u) = 1; TUROK_MEM32(entry + 0x18u) = 1; TUROK_MEM32(entry + 0x24u) = 0;
        } else {
            TUROK_MEM32(entry + 0x24u) = (id == 7 || id == 8) ? 0u : 1u;
        }
        if (saved_weapon(items, id, 1)) TUROK_MEM32(entry + 0x14u) = 1;
        if (saved_weapon(items, id, 2)) TUROK_MEM32(entry + 0x18u) = 1;
        for (field = 0; field < 3; field++) {
            int32_t ammo = (int32_t)TUROK_MEM32(entry + 4u + 4u * (uint32_t)field) + saved_weapon(items, id, 3 + field);
            int32_t most = (int32_t)TUROK_MEM32(max + 4u * (uint32_t)field);
            if (ammo > most) ammo = most;
            if (ammo < 0) ammo = 0;
            TUROK_MEM32(entry + 4u + 4u * (uint32_t)field) = (uint32_t)ammo;
        }
        if (id == 8) TUROK_MEM32(entry + 0xCu) = 0;
        TUROK_MEM32(entry + 0x1Cu) = 0;          /* a byte the game uses, and padding it leaves unset */
        TUROK_MEM32(entry + 0x20u) = 1;
        snprintf(added + strlen(added), sizeof added - strlen(added), " %u", id);

        /* Move it among the others by ID, so next and previous weapon meet
         * it in order. The build has chosen nothing yet; indices the
         * inventory holds (in hand, switching to, to return to) follow. */
        count = (TUROK_MEM32(inventory + 8u) - begin) / 0x28u;
        for (position = 0; position < at; position++)
            if (TUROK_MEM32(begin + (uint32_t)position * 0x28u) > id) break;
        if (position < at) {
            uint32_t words[10], k;
            static const uint32_t index_fields[3] = {0x10u, 0x5Cu, 0x38u};
            for (k = 0; k < 10; k++) words[k] = TUROK_MEM32(entry + 4u * k);
            memmove((void *)((uintptr_t)(begin + (uint32_t)(position + 1) * 0x28u) + g_xbox_mem_offset),
                    (void *)((uintptr_t)(begin + (uint32_t)position * 0x28u) + g_xbox_mem_offset),
                    (size_t)(at - position) * 0x28u);
            for (k = 0; k < 10; k++) TUROK_MEM32(begin + (uint32_t)position * 0x28u + 4u * k) = words[k];
            for (k = 0; k < 3; k++) {
                uint32_t index = TUROK_MEM32(inventory + index_fields[k]);
                if (index < count && index >= (uint32_t)position && index < (uint32_t)at)
                    TUROK_MEM32(inventory + index_fields[k]) = index + 1u;
            }
        }
    }
    /* All Weapons: every weapon also full. One never picked up came in with
     * no ammunition, and the game passes over an empty weapon when you
     * change weapons, so the cheat looked as if it gave nothing (unless
     * Unlimited Ammo was on too). Each ammunition kind goes to its maximum
     * (the table at 0x2259A0, 16 bytes a weapon: three maximums and the
     * number of modes); the bows' shared arrows are pooled after this
     * (sub_0015F400). */
    if (cheat) {
        uint32_t begin = TUROK_MEM32(inventory + 4u), end = TUROK_MEM32(inventory + 8u), i;
        for (i = 0; begin + (i + 1u) * 0x28u <= end && i < 32; i++) {
            uint32_t entry = begin + i * 0x28u, weapon = TUROK_MEM32(entry), field;
            if (weapon < 2 || weapon > 14 || !TUROK_MEM32(entry + 0x20u)) continue;
            for (field = 0; field < 3; field++)
                TUROK_MEM32(entry + 4u + 4u * field) = TUROK_MEM32(0x002259A0u + weapon * 16u + 4u * field);
        }
    }
    if (*kept || *added || cheat)
        fprintf(stderr, "[WEAPONS] keep weapons: owned again%s; added%s%s\n", *kept ? kept : " none",
                *added ? added : " none", cheat ? " (All Weapons cheat: all full)" : "");
}

void sub_00160BC0(void)
{
    uint32_t inventory = g_ecx, previous = s_keep_inventory;
    s_keep_inventory = video_settings_keep_weapons() ? inventory : 0;
    sub_00160BC0_gen();          /* ends in sub_0015F400 */
    s_keep_inventory = previous;
}

void sub_0015F400(void)
{
    if (s_keep_inventory && g_ecx == s_keep_inventory) {
        uint32_t inventory = s_keep_inventory;
        s_keep_inventory = 0;
        keep_weapons(inventory);
        g_ecx = inventory;
    }
    sub_0015F400_gen();
}

/* ── Surfacing at high frame rates ─────────────────────────────
 *
 * Each frame the swim update asks sub_00144B90 whether a point 0.15 above
 * the head object ([player + 0x3CC] + 0x38) is still in a water volume; under
 * water (callers 0x14DA48, 0x151D9D) a "no" surfaces the player (state 0x11),
 * and the surface state then holds them exactly where they are -- it never
 * moves them up or down. So the head floats above the waterline by however
 * far the last frame of the climb carried it: at 60 fps up to 0.14 rising with
 * jump (8-10 units a second), at the console's 30 up to 0.29; uncapped 0.05
 * or less, and swimming up at an angle with forward (about 4.5 units a second)
 * even less. Then the head's bob dips the test point back under, the player
 * drops to state 0x13, sinks 1.6 units, climbs, resurfaces for a moment: the
 * camera "trapped beneath" the waterline (Chapter 1, pod01_03's lake; deeper
 * lakes leave more room). Above 60 fps, the frame that surfaces lifts the
 * player so the head sits 0.15 above the waterline -- taken as half way
 * through that frame's climb -- through the game's own set-position
 * (sub_0003F380). At 60 fps and below nothing changes. */
extern void sub_00144B90_gen(void);
extern void sub_0003F380(void);
extern RECOMP_TLS uint32_t g_eax, g_edx, g_esp;

static float frame_seconds(void)
{
    uint32_t timing = TUROK_MEM32(0x250A84u);
    return timing ? guest_float(timing + 0x5Cu) : 0.0f;
}

void sub_00144B90(void)
{
    static uint32_t last_player;
    static float last_head;               /* the head at the last climbing test, still under */
    static clock_t last_time;
    uint32_t player = g_ecx, caller = TUROK_MEM32(g_esp), head = TUROK_MEM32(player + 0x3CCu);
    float dt = frame_seconds(), head_y = head ? guest_float(head + 0x3Cu) : 0.0f, step, lift;
    clock_t now = clock();
    int climbing;

    sub_00144B90_gen();
    /* Only the underwater update's test (0x151D9D) counts: the state-entry
     * test (0x14DA48) also runs when the player falls in or dives, and a
     * lift there kept them on top of the water. */
    if (player != TUROK_MEM32(0x257E90u) || !head || caller != 0x00151D9Du) return;
    climbing = last_player == player && now - last_time < CLOCKS_PER_SEC / 5 && head_y > last_head;
    step = head_y - last_head;
    if (g_eax) {                          /* still under: remember where */
        last_player = player; last_head = head_y; last_time = now;
        return;
    }
    last_player = 0;
    if (!climbing || dt <= 0.0f || dt >= 1.0f / 61.0f || step > 0.3f) return;
    lift = 0.15f - step * 0.5f;
    if (lift <= 0.0f) return;
    {
        /* sub_0003F380 (thiscall, ret 4) with the raised position, from a
         * vector on the guest stack. */
        uint32_t saved_eax = g_eax, saved_ecx = g_ecx, saved_edx = g_edx, vec, y_bits;
        float y = guest_float(player + 0x3Cu) + lift;
        g_esp -= 12;
        vec = g_esp;
        TUROK_MEM32(vec) = TUROK_MEM32(player + 0x38u);
        memcpy(&y_bits, &y, 4);
        TUROK_MEM32(vec + 4u) = y_bits;
        TUROK_MEM32(vec + 8u) = TUROK_MEM32(player + 0x40u);
        g_esp -= 4; TUROK_MEM32(g_esp) = vec;
        g_esp -= 4; TUROK_MEM32(g_esp) = 0;
        g_ecx = player;
        sub_0003F380();
        g_esp += 12;
        g_eax = saved_eax; g_ecx = saved_ecx; g_edx = saved_edx;
    }
}

/* ── Water drag at high frame rates ────────────────────────────
 *
 * In the swim states (sub_0014FB40, sub_00151D30, sub_00153520) sub_00148950
 * steers the player's body velocity ([player + 0xFC] + 0x18) along each
 * swim axis toward its target speed (+0x3D8 strafe, +0x3DC vertical, +0x3E0
 * forward) by a fixed fraction every frame: 0.03 horizontally, 0.1
 * vertically -- not scaled by the frame time. A fall into water keeps 90% of
 * its speed per frame: at 60 fps the plunge carries the player under, but at
 * 200+ fps it is gone almost the instant they touch the water and they settle
 * in slowly. Above 60 fps the change the function makes to the velocity is
 * scaled to what that many frames at 60 fps would make,
 * (1 - 0.9^(60 dt)) / 0.1 (the same within a few percent for 0.03), so the
 * water slows the player per second as it does at 60. At 60 fps and below
 * nothing changes. */
extern void sub_00148950_gen(void);

void sub_00148950(void)
{
    uint32_t player = g_ecx, body = player ? TUROK_MEM32(player + 0xFCu) : 0;
    float dt = frame_seconds(), before[3], after[3], scale;
    int i;

    if (!body || dt <= 0.0f || dt >= 1.0f / 61.0f) {
        sub_00148950_gen();
        return;
    }
    for (i = 0; i < 3; i++) before[i] = guest_float(body + 0x18u + 4u * i);
    sub_00148950_gen();
    body = TUROK_MEM32(player + 0xFCu);
    if (!body) return;
    scale = (1.0f - powf(0.9f, 60.0f * dt)) / 0.1f;
    for (i = 0; i < 3; i++) {
        uint32_t bits;
        after[i] = guest_float(body + 0x18u + 4u * i);
        after[i] = before[i] + (after[i] - before[i]) * scale;
        memcpy(&bits, &after[i], 4);
        TUROK_MEM32(body + 0x18u + 4u * i) = bits;
    }
}

/* ── Ladders ──────────────────────────────────────────────────────
 *
 * Climbing (movement state 0x0F, update sub_0014D6A0 -> sub_0014A920) aims
 * the view rather than turning it: sub_0014A920 passes sub_001435C0 a pitch
 * TARGET, 75 degrees ([0x24E5D4], the view up the ladder) plus, for the look
 * stick one way, up to 2 x 75 x stick x (pitch sensitivity + 1) / 6, and
 * sub_001435C0 eases the pitch (player +0x3FC, degrees) towards it at 5 per
 * second. Nothing limits it: on foot sub_00144560 holds the pitch within
 * 70 degrees, but here the target reaches 225, past straight up, and the
 * view goes over the top and turns upside down -- on the Xbox too. Its yaw
 * (+0x400, the head's, relative to the body) eases towards CLIMB_HORIZ-
 * VIEWANGLE (+0x894) x the stick and back to 0 when it is let go.
 *
 * Always: the target stops at 85 degrees either way, short of vertical, so
 * the view cannot flip; otherwise the ladder looks as on the Xbox.
 *
 * Ladder freelook (PC Settings): the look stick turns the view instead, as
 * it does on foot, and it stays where it is left: the pitch within the
 * on-foot limit ([0x221420], 70), the head's yaw within CLIMB_HORIZVIEW-
 * ANGLE. The stick goes through what the on-foot look does (sub_00144780):
 * a 0.3 deadzone per axis, the response curve (sub_00142280) and easing
 * up by a fifth a frame -- or Modern Pad Aim's vector -- and Mouse look
 * raw adds the mouse's turn. Then the on-foot speeds: yaw TURNSPEED (+0x884)
 * x 0.75 x min(2 x FOV/90, 1) (sub_00144430), pitch TURNSPEED x 0.5 x
 * FOV/90 (sub_00144560), each x (sensitivity + 1) / 6, with Invert Look.
 * Done through the target sub_001435C0 eases to, chosen so that its step
 * lands exactly on the turned pitch, and its yaw step. */
extern void sub_001435C0_gen(void);
extern int video_settings_ladder_freelook(void);

#define LADDER_CALL       0x0014ABBAu   /* sub_0014A920's call */
static float s_ladder_yaw;              /* the head's yaw after the last ladder turn */
static int s_ladder_turned;             /* ladder_turn_body has turned the body */
#define LADDER_TARGET_MAX 85.0f
static void put_guest_float(uint32_t va, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, 4);
    TUROK_MEM32(va) = bits;
}

static float clampf(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

extern float video_settings_take_mouse_turn(int axis);

/* The on-foot look's stick (sub_00144780): right x, up y. */
static void ladder_look_stick(uint32_t player, float *x, float *y, float ease[2], float dt)
{
    float v[2], m;
    int i;
    v[0] = guest_float(player + 0x192Cu) - guest_float(player + 0x1928u);
    v[1] = guest_float(player + 0x1920u) - guest_float(player + 0x1924u);
    if (video_settings_modern_aim()) {
        modern_look_vector(&v[0], &v[1]);
        ease[0] = v[0]; ease[1] = v[1];
    } else {
        for (i = 0; i < 2; i++)
            v[i] = v[i] > 0.3f ? (v[i] - 0.3f) * (1.0f / 0.7f) : v[i] < -0.3f ? (v[i] + 0.3f) * (1.0f / 0.7f) : 0.0f;
        m = sqrtf(v[0] * v[0] + v[1] * v[1]);
        for (i = 0; i < 2; i++) {
            if (m < 0.5f) v[i] *= 0.25f;
            else if (m < 0.9f) v[i] *= 0.3f;
            else v[i] = v[i] < 0.0f ? -v[i] * v[i] : v[i] * v[i];
            if ((v[i] > 0.0f && v[i] > ease[i]) || (v[i] < 0.0f && v[i] < ease[i]))
                v[i] = ease[i] + (v[i] - ease[i]) * 0.2f;
            ease[i] = v[i];
        }
    }
    /* Mouse look raw: the mouse's turn, as the on-foot turn functions add it. */
    for (i = 0; i < 2; i++) {
        float turn = video_settings_take_mouse_turn(i);
        if (dt > 0.0f && dt <= 1.0f) v[i] += turn / dt;
    }
    *x = v[0];
    *y = v[1];
}

void sub_001435C0(void)
{
    uint32_t player = g_ecx, args = g_esp + 4u;
    static clock_t s_last;
    static float s_ease[2];
    float target, yaw_step, dt, pitch, yaw, limit, look_x, look_y, pitch_k, yaw_k;
    int freelook;
    clock_t now;

    if (TUROK_MEM32(g_esp) != LADDER_CALL || !player) {
        sub_001435C0_gen();
        return;
    }
    target = guest_float(args);
    yaw_step = guest_float(args + 4u);
    dt = guest_float(args + 8u);
    pitch = guest_float(player + 0x3FCu);
    yaw = guest_float(player + 0x400u);
    freelook = video_settings_ladder_freelook();
    now = clock();
    if (now - s_last > CLOCKS_PER_SEC / 2) {
        s_ease[0] = s_ease[1] = 0.0f;
        fprintf(stderr, "[LADDER] climbing: pitch %.1f yaw %.1f target %.1f side limit %.1f turn speed %.1f "
                "fov %.1f flags 0x%X%s\n", pitch, yaw, target, guest_float(player + 0x894u),
                guest_float(player + 0x884u), guest_float(player + 0x830u), TUROK_MEM32(player + 0x45Cu),
                freelook ? " (freelook)" : "");
    }
    s_last = now;

    if (freelook && dt > 0.0f) {
        float turn_speed = guest_float(player + 0x884u), fov = guest_float(player + 0x830u) / 90.0f;
        float pitch_limit = guest_float(0x00221420u), yaw_zoom, turned;
        if (!(turn_speed > 0.0f && turn_speed < 10000.0f)) turn_speed = 180.0f;
        if (!(fov > 0.0f && fov < 4.0f)) fov = 1.0f;
        if (!(pitch_limit > 0.0f && pitch_limit < 90.0f)) pitch_limit = 70.0f;
        yaw_zoom = fminf(2.0f * fov, 1.0f);
        ladder_look_stick(player, &look_x, &look_y, s_ease, dt);
        pitch_k = (float)((int32_t)TUROK_MEM32(player + 0x3C8u) + 1) / 6.0f;
        yaw_k = (float)((int32_t)TUROK_MEM32(player + 0x3C4u) + 1) / 6.0f;
        /* sub_00144560: the step is negated without Invert Look (+0x3C0). */
        {
            float step = fov * 0.5f * turn_speed * look_y * dt * pitch_k;
            turned = clampf(pitch + (TUROK_MEM32(player + 0x3C0u) ? step : -step), -pitch_limit, pitch_limit);
        }
        /* sub_001435C0 sets pitch += (target - pitch) x dt x 5. */
        target = pitch + (turned - pitch) / (dt * 5.0f);
        limit = guest_float(player + 0x894u);
        if (!(limit > 0.0f && limit < 180.0f)) limit = 90.0f;
        yaw_step = clampf(yaw - yaw_zoom * 0.75f * turn_speed * look_x * dt * yaw_k, -limit, limit) - yaw;
    } else {
        target = clampf(target, -LADDER_TARGET_MAX, LADDER_TARGET_MAX);
    }
    put_guest_float(args, target);
    put_guest_float(args + 4u, yaw_step);
    sub_001435C0_gen();
    s_ladder_yaw = guest_float(player + 0x400u);
}

/* Leaving the ladder: off it the head's yaw (+0x400) goes back to the
 * front, so with Ladder freelook the view swung back to the ladder. Instead
 * the body turns by the head's yaw and the head faces forward: the view
 * stays where it was. The body's yaw is what the on-foot turn
 * (sub_00144430) turns -- radians at +4 of what sub_00038870 returns, which
 * also marks the angles changed -- and turns the same way for the same
 * stick as the head's degrees do. The pitch is the on-foot one already.
 *
 * Two ways off: the state's exit (sub_0014D7D0: at the bottom, letting go)
 * still has the head's yaw; at the top the update (sub_0014D6A0) zeroes it
 * itself (0x14D7A8) before it hops the player onto the ledge, so there the
 * yaw is the last the turn left (s_ladder_yaw, sub_001435C0). The hop's
 * velocity is already in the world by then: turning the body after it does
 * not change where the player lands. */
extern void sub_0014D7D0_gen(void);
extern void sub_0014D6A0_gen(void);
extern void sub_00038870(void);

static void ladder_turn_body(uint32_t player, float head_yaw, const char *where)
{
    uint32_t saved_eax = g_eax, saved_ecx = g_ecx, saved_edx = g_edx, angles;
    if (!player || !video_settings_ladder_freelook() || head_yaw == 0.0f || !(fabsf(head_yaw) < 180.0f)) return;
    g_esp -= 4; TUROK_MEM32(g_esp) = 0;   /* return address */
    g_ecx = player;
    sub_00038870();                       /* thiscall, ret */
    angles = g_eax;
    g_eax = saved_eax; g_ecx = saved_ecx; g_edx = saved_edx;
    if (!angles) return;
    put_guest_float(angles + 4u, guest_float(angles + 4u) + head_yaw * 0.017453292f);
    put_guest_float(player + 0x400u, 0.0f);
    s_ladder_turned = 1;
    fprintf(stderr, "[LADDER] off the ladder %s: body turned %.1f degrees with the view\n", where, head_yaw);
}

void sub_0014D7D0(void)
{
    uint32_t player = g_ecx;
    if (player) ladder_turn_body(player, guest_float(player + 0x400u), "(let go)");
    sub_0014D7D0_gen();
}

void sub_0014D6A0(void)
{
    uint32_t player = g_ecx;
    float before = player ? guest_float(player + 0x400u) : 0.0f;
    s_ladder_yaw = before;
    s_ladder_turned = 0;
    sub_0014D6A0_gen();
    /* Not again if the exit (inside the update) already turned it. */
    if (player && !s_ladder_turned && before != 0.0f && guest_float(player + 0x400u) == 0.0f)
        ladder_turn_body(player, s_ladder_yaw, "at the top");
}

/* ── Walk bob (sub_00149BE0) ───────────────────────────────────
 *
 * this-call (the player), arg dt, ret 4. Sets the view model's bob: +0x40C
 * (vertical) straight from |trig(step phase)| x amplitude x a speed factor,
 * +0x408 (lateral) eased towards its target. The factor is this frame's
 * movement (+0x38 minus last frame's position, +0xAC) over the state's
 * speed x dt, clamped to 0..1. On some ground (Chapter 6's paths, slopes,
 * seams) collision lets the player move only every other frame or so --
 * full steps between frames of nothing or a tenth of one -- so the factor
 * flicked between 1 and 0 and the gun shook up and down by its whole bob
 * (the sideways bob is eased, so only the vertical shook). The bob now sees
 * the player's speed over about a tenth of a second: the last position is
 * moved, for the length of the call, to where that speed would have come
 * from. Steady walking gives the same bob as before. RECOMP_BOB_TRACE logs
 * the raw and smoothed movement per frame. */
extern void sub_00149BE0_gen(void);

#define BOB_SMOOTH_SECONDS 0.1f
#define BOB_MAX_SPEED      40.0f   /* a teleport or respawn is not walking */

/* The ease (sub_0017F8C0, cdecl: value *, target, rate, seconds: value +=
 * (target - value) x rate x seconds, not past the target) as the walk bob
 * calls it for the sideways offset (returning to 0x149FB5): under
 * RECOMP_BOB_TRACE, its arguments. */
extern void sub_0017F8C0_gen(void);
void sub_0017F8C0(void)
{
    static int trace = -1;
    if (trace < 0) trace = getenv("RECOMP_BOB_TRACE") != NULL;
    if (trace && TUROK_MEM32(g_esp) == 0x00149FB5u) {
        uint32_t value = TUROK_MEM32(g_esp + 4u);
        fprintf(stderr, "[BOBX] value %.4f target %.4f rate %.4f seconds %.5f\n", value ? guest_float(value) : 0.0f,
                guest_float(g_esp + 8u), guest_float(g_esp + 0xCu), guest_float(g_esp + 0x10u));
    }
    sub_0017F8C0_gen();
}

void sub_00149BE0(void)
{
    static int trace = -1;
    static struct { uint32_t player; float vx, vz; } smooth[4];
    uint32_t player = g_ecx, old_x = 0, old_z = 0, new_x = 0, new_z = 0;
    float dt = guest_float(g_esp + 4u), dx = 0, dz = 0, vx = 0, vz = 0;
    int slot = -1, i;

    if (trace < 0) trace = getenv("RECOMP_BOB_TRACE") != NULL;
    if (player && dt > 0.001f && dt < 0.5f) {
        float x = guest_float(player + 0x38u), z = guest_float(player + 0x40u), speed, k;
        for (i = 0; i < 4 && smooth[i].player != player; i++) {}
        if (i == 4) {   /* a new player: take a free or the first slot, starting from rest */
            for (i = 0; i < 4 && smooth[i].player; i++) {}
            if (i == 4) i = 0;
            smooth[i].player = player;
            smooth[i].vx = smooth[i].vz = 0;
        }
        slot = i;
        dx = x - guest_float(player + 0xACu);
        dz = z - guest_float(player + 0xB4u);
        vx = dx / dt;
        vz = dz / dt;
        speed = sqrtf(vx * vx + vz * vz);
        if (!isfinite(speed) || speed > BOB_MAX_SPEED) vx = vz = 0;
        k = 1.0f - expf(-dt / BOB_SMOOTH_SECONDS);
        smooth[slot].vx += (vx - smooth[slot].vx) * k;
        smooth[slot].vz += (vz - smooth[slot].vz) * k;
        old_x = TUROK_MEM32(player + 0xACu);
        old_z = TUROK_MEM32(player + 0xB4u);
        put_guest_float(player + 0xACu, x - smooth[slot].vx * dt);
        put_guest_float(player + 0xB4u, z - smooth[slot].vz * dt);
        new_x = TUROK_MEM32(player + 0xACu);
        new_z = TUROK_MEM32(player + 0xB4u);
    }
    sub_00149BE0_gen();
    if (slot >= 0) {
        /* Put the real last position back, unless something in the call set
         * a new one. */
        if (TUROK_MEM32(player + 0xACu) == new_x) TUROK_MEM32(player + 0xACu) = old_x;
        if (TUROK_MEM32(player + 0xB4u) == new_z) TUROK_MEM32(player + 0xB4u) = old_z;
    }
    if (trace && player) {
        uint32_t state = TUROK_MEM32(player + 0x12Cu);
        fprintf(stderr, "[BOB] dt %.4f move %.4f %.4f (%.2f/s, smoothed %.2f/s) y %.4f x %.4f z %.4f "
                        "second %.4f %.4f state %u t %.3f\n",
                dt, dx, dz, sqrtf(vx * vx + vz * vz),
                slot >= 0 ? sqrtf(smooth[slot].vx * smooth[slot].vx + smooth[slot].vz * smooth[slot].vz) : 0.0f,
                guest_float(player + 0x40Cu), guest_float(player + 0x408u), guest_float(player + 0x410u),
                guest_float(player + 0x1A70u), guest_float(player + 0x1A74u),
                state ? TUROK_MEM32(state) : 0u, state ? guest_float(state + 0x10u) : 0.0f);
    }
}

/* ── 4x3 matrix product (sub_000E7570) ─────────────────────────
 *
 * cdecl (out, a, b): out = a x b for the game's 4x3 matrices (rows of four
 * floats, translation in the fourth column). The scene's transform walk
 * (sub_000E80F0, its only caller) concatenates one for every node it
 * visits; in dense, far-drawn scenes that was a fifth of the game thread,
 * each x87 instruction going through the emulated register stack. Done
 * here with the same products and sums in the same order and the generated
 * code's rounding of each (RECOMP_FP_PC), reading and writing guest memory
 * in the original order so an `out` that is also an input behaves the same.
 * RECOMP_MATRIX_VERIFY=1 runs the original after it and reports any
 * difference. */
extern void sub_000E7570_gen(void);
extern RECOMP_TLS uint32_t g_eax, g_edx;

static double mx_round(double x)
{
    double ax;
    int e;
    if (g_fp_control_word & 0x300u) return x;
    ax = fabs(x);
    if ((ax <= 3.4028234663852886e38 && ax >= 1.1754943508222875e-38) || ax == 0.0 || x != x || ax == INFINITY)
        return (double)(float)x;
    x = frexp(x, &e);
    return ldexp((double)(float)x, e);
}

#define MX(p, i) ((double)guest_float((p) + (uint32_t)(i) * 4u))
#define MXMUL(x, y) mx_round((x) * (y))
#define MXADD(x, y) mx_round((x) + (y))

static void mx_store(uint32_t va, double value)
{
    float f = (float)value;
    uint32_t bits;
    memcpy(&bits, &f, 4);
    TUROK_MEM32(va) = bits;
}

/* (p1 + p2) + p3, each term rounded, in the original's order. */
#define MXSUM3(p1, p2, p3) MXADD(MXADD(p1, p2), p3)

static void matrix_product_native(uint32_t out, uint32_t a, uint32_t b)
{
    double v;
    v = MXSUM3(MXMUL(MX(a, 0), MX(b, 0)), MXMUL(MX(b, 2), MX(a, 8)), MXMUL(MX(b, 1), MX(a, 4)));
    mx_store(out + 0x00u, v);
    v = MXSUM3(MXMUL(MX(b, 5), MX(a, 4)), MXMUL(MX(b, 6), MX(a, 8)), MXMUL(MX(a, 0), MX(b, 4)));
    mx_store(out + 0x04u, v);
    v = MXSUM3(MXMUL(MX(b, 9), MX(a, 4)), MXMUL(MX(b, 8), MX(a, 0)), MXMUL(MX(a, 8), MX(b, 10)));
    mx_store(out + 0x08u, v);
    v = MXSUM3(MXMUL(MX(b, 14), MX(a, 8)), MXMUL(MX(b, 13), MX(a, 4)), MXMUL(MX(b, 12), MX(a, 0)));
    v = MXADD(v, MX(a, 12));
    mx_store(out + 0x0Cu, v);
    v = MXSUM3(MXMUL(MX(a, 1), MX(b, 0)), MXMUL(MX(a, 5), MX(b, 1)), MXMUL(MX(a, 9), MX(b, 2)));
    mx_store(out + 0x10u, v);
    v = MXSUM3(MXMUL(MX(a, 5), MX(b, 5)), MXMUL(MX(a, 1), MX(b, 4)), MXMUL(MX(a, 9), MX(b, 6)));
    mx_store(out + 0x14u, v);
    v = MXSUM3(MXMUL(MX(a, 9), MX(b, 10)), MXMUL(MX(a, 5), MX(b, 9)), MXMUL(MX(b, 8), MX(a, 1)));
    mx_store(out + 0x18u, v);
    v = MXSUM3(MXMUL(MX(b, 14), MX(a, 9)), MXMUL(MX(a, 5), MX(b, 13)), MXMUL(MX(b, 12), MX(a, 1)));
    v = MXADD(v, MX(a, 13));
    mx_store(out + 0x1Cu, v);
    v = MXSUM3(MXMUL(MX(a, 2), MX(b, 0)), MXMUL(MX(a, 10), MX(b, 2)), MXMUL(MX(b, 1), MX(a, 6)));
    mx_store(out + 0x20u, v);
    v = MXSUM3(MXMUL(MX(b, 5), MX(a, 6)), MXMUL(MX(a, 2), MX(b, 4)), MXMUL(MX(a, 10), MX(b, 6)));
    mx_store(out + 0x24u, v);
    v = MXSUM3(MXMUL(MX(a, 10), MX(b, 10)), MXMUL(MX(b, 8), MX(a, 2)), MXMUL(MX(b, 9), MX(a, 6)));
    mx_store(out + 0x28u, v);
    v = MXSUM3(MXMUL(MX(b, 12), MX(a, 2)), MXMUL(MX(a, 10), MX(b, 14)), MXMUL(MX(b, 13), MX(a, 6)));
    v = MXADD(v, MX(a, 14));
    mx_store(out + 0x2Cu, v);
}

void sub_000E7570(void)
{
    static int verify = -1;
    uint32_t out = TUROK_MEM32(g_esp + 4u), a = TUROK_MEM32(g_esp + 8u), b = TUROK_MEM32(g_esp + 12u);
    if (verify < 0) verify = getenv("RECOMP_MATRIX_VERIFY") != NULL;
    if (verify) {
        /* Run both on the same inputs and compare what each wrote. */
        uint32_t saved_out[12], saved_a[16], saved_b[16], native[12], original[12], i;
        static unsigned checks, mismatches;
        for (i = 0; i < 12; i++) saved_out[i] = TUROK_MEM32(out + i * 4u);
        for (i = 0; i < 16; i++) { saved_a[i] = TUROK_MEM32(a + i * 4u); saved_b[i] = TUROK_MEM32(b + i * 4u); }
        matrix_product_native(out, a, b);
        for (i = 0; i < 12; i++) native[i] = TUROK_MEM32(out + i * 4u);
        for (i = 0; i < 12; i++) TUROK_MEM32(out + i * 4u) = saved_out[i];
        for (i = 0; i < 16; i++) { TUROK_MEM32(a + i * 4u) = saved_a[i]; TUROK_MEM32(b + i * 4u) = saved_b[i]; }
        sub_000E7570_gen();   /* pops its return address */
        for (i = 0; i < 12; i++) original[i] = TUROK_MEM32(out + i * 4u);
        checks++;
        if (memcmp(native, original, sizeof native) && mismatches++ < 16)
            fprintf(stderr, "[MATRIX] native product differs (check %u, control word %04X)\n",
                    checks, (unsigned)g_fp_control_word);
        if ((checks & 0xFFFFFu) == 0)
            fprintf(stderr, "[MATRIX] %u checks, %u differ\n", checks, mismatches);
        return;
    }
    matrix_product_native(out, a, b);
    /* As the original leaves them. */
    g_eax = a; g_ecx = b; g_edx = out;
    g_esp += 4u;   /* ret */
}

/* ── Fall trace (RECOMP_FALL_TRACE=1; issue #25) ───────────────
 *
 * Players dying at random above 150 fps (both Flight maps; the start of
 * Chapter 2 Level 3 uncapped). The on-foot fall checks are speed-based:
 * sub_0014CA80 (after the on-foot update) starts the falling scream
 * (+0x1838) once the downward speed ([+0xFC]+0x1C) passes FALLDAMAGEMAXSPEED
 * (+0x8B4); sub_0014B360 (thiscall (landing), ret 4) takes damage on
 * landing from the impact speed (landing+0xC) between FALLDAMAGEMINSPEED
 * (+0x8B0) and the maximum. This logs both with the frame time, to see
 * whether a high frame rate hands them a bogus speed. */
extern void sub_0014B360_gen(void);
extern void sub_0014CA80_gen(void);

static int fall_trace(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_FALL_TRACE") != NULL;
    return on;
}

void sub_0014B360(void)
{
    if (fall_trace()) {
        uint32_t self = g_ecx, landing = TUROK_MEM32(g_esp + 4u);
        float speed = landing ? guest_float(landing + 0xCu) : 0.0f;
        float least = guest_float(self + 0x8B0u), most = guest_float(self + 0x8B4u);
        if (speed < -least)
            fprintf(stderr, "[FALL] landing at %.2f (damage from %.2f, all at %.2f); frame %.5f s; at %.1f %.1f %.1f\n",
                    speed, -least, -most, frame_seconds(), guest_float(self + 0x38u), guest_float(self + 0x3Cu),
                    guest_float(self + 0x40u));
    }
    sub_0014B360_gen();
}

void sub_0014CA80(void)
{
    uint32_t self = g_ecx, falling = fall_trace() ? TUROK_MEM32(self + 0x1838u) : 1u;
    sub_0014CA80_gen();
    if (!falling && TUROK_MEM32(self + 0x1838u)) {
        uint32_t body = TUROK_MEM32(self + 0xFCu);
        float speed = body ? guest_float(body + 0x1Cu) : 0.0f;
        fprintf(stderr, "[FALL] falling scream at %.2f (max %.2f); frame %.5f s; at %.1f %.1f %.1f\n",
                speed, -guest_float(self + 0x8B4u), frame_seconds(), guest_float(self + 0x38u),
                guest_float(self + 0x3Cu), guest_float(self + 0x40u));
    }
}

/* ── Trampoline trace (RECOMP_FALL_TRACE=1; issue #25) ─────────
 *
 * A Bouncer (Withering Heights' TreehouseJumpers) bounces what touches it
 * through sub_00088BC0 (this-call (?, bouncee, ?), ret 0xC; this = the
 * bounce settings: +0 BounceKind, +8 BounceAdjust, +0x14 Bounce, +0x64/+0x68
 * BounceNK/NP, +0x6C Recharge). The trampolines are kind 2: the touching
 * actor keeps its horizontal velocity (body +0x18/+0x20) and gets 25 up
 * (+0x1C). Players stuck inside them at high frame rates: this logs each
 * bounce of the local player with the frame time and the velocity before
 * and after -- every one of the first 300, then one in 500. */
extern void sub_00088BC0_gen(void);
extern void sub_00088C60_gen(void);

static void bounce_call(void (*gen)(void), int which)
{
    static unsigned count;
    uint32_t self = g_ecx, bouncee = TUROK_MEM32(g_esp + 8u), body;
    float before[3] = { 0 };
    int i;
    if (!fall_trace() || !bouncee) {
        gen();
        return;
    }
    body = TUROK_MEM32(bouncee + 0xFCu);
    for (i = 0; i < 3 && body; i++) before[i] = guest_float(body + 0x18u + 4u * i);
    gen();
    if (++count <= 300u || count % 500u == 0u)
        fprintf(stderr, "[BOUNCE] %c #%u kind %u: actor %08X (vtable %08X%s, type flags %08X, wants %08X) velocity "
                        "%.2f %.2f %.2f -> %.2f %.2f %.2f (bounced %u); at %.2f %.2f %.2f; frame %.5f s; recharge %.3f\n",
                which ? 'T' : 'C', count, TUROK_MEM32(self), bouncee, TUROK_MEM32(bouncee),
                bouncee == TUROK_MEM32(0x257E90u) ? ", you" : "", TUROK_MEM32(bouncee + 0x24u),
                TUROK_MEM32(self + 0x60u), before[0], before[1], before[2],
                body ? guest_float(body + 0x18u) : 0.0f, body ? guest_float(body + 0x1Cu) : 0.0f,
                body ? guest_float(body + 0x20u) : 0.0f, g_eax, guest_float(bouncee + 0x38u),
                guest_float(bouncee + 0x3Cu), guest_float(bouncee + 0x40u), frame_seconds(),
                guest_float(self + 0x6Cu));
}

/* The set-position (sub_0003F380, this-call (position *), ret 4: +0x38..+0x40)
 * for the local player under RECOMP_FALL_TRACE: each time it puts the player
 * more than 0.02 lower, the height before and after and the code addresses
 * on the guest stack (callers, innermost first) -- the first 400. */
extern void sub_0003F380_gen(void);

void sub_0003F380(void)
{
    static unsigned shown;
    uint32_t self = g_ecx, position = TUROK_MEM32(g_esp + 4u), esp = g_esp;
    if (fall_trace() && self && self == TUROK_MEM32(0x257E90u) && position && shown < 400u) {
        float before = guest_float(self + 0x3Cu), after = guest_float(position + 4u);
        if (after < before - 0.02f) {
            unsigned i, found = 0;
            shown++;
            fprintf(stderr, "[SETPOS] y %.4f -> %.4f; frame %.5f s; callers", before, after, frame_seconds());
            for (i = 0; i < 96u && found < 8u; i++) {
                uint32_t v = TUROK_MEM32(esp + 4u * i);
                if (v >= 0x00011000u && v < 0x00190000u) {
                    fprintf(stderr, " %06X", v);
                    found++;
                }
            }
            fprintf(stderr, "\n");
        }
    }
    sub_0003F380_gen();
}

void sub_00088BC0(void) { bounce_call(sub_00088BC0_gen, 0); }
void sub_00088C60(void) { bounce_call(sub_00088C60_gen, 1); }

/* ── The stuck-flier kill at high frame rates (sub_00148E30; issue #25) ──
 *
 * sub_00148E30 (this-call (seconds), ret 4), called by the player update
 * (from 0x14EB35) while the player flies (+0x4D0), kills a player who has
 * been "stuck" for half a second: when the squared distance from the last
 * frame's position (+0xAC) to this one (+0x38) is under 0.005, it adds the
 * frame time to +0x4CC, and at 0.5 deals the health plus 1000 (type 8);
 * any larger step resets the count. The test is per frame: under 0.071
 * units a frame is 4.2 units a second at 60 fps, but 10 at 143 fps and 14
 * at 200 -- above ~150 fps a pteranodon turning or slowing, or just
 * respawned, was killed in mid-air. Above 60 fps the step is measured as
 * it would be over a 60th of a second (the last position is moved back
 * along the step for the call, then restored), so the kill takes the same
 * speed as at 60 fps. At 60 fps and below nothing changes.
 * RECOMP_STUCK_FIX=0 runs the original test. */
extern void sub_00148E30_gen(void);

void sub_00148E30(void)
{
    static int enabled = -1;
    uint32_t self = g_ecx, saved[3];
    float seconds = guest_float(g_esp + 4u), scale;
    int i;
    if (enabled < 0) enabled = !(getenv("RECOMP_STUCK_FIX") && *getenv("RECOMP_STUCK_FIX") == '0');
    if (!enabled || !(seconds > 0.0f && seconds < 1.0f / 60.0f)) {
        sub_00148E30_gen();
        return;
    }
    scale = (1.0f / 60.0f) / seconds;
    for (i = 0; i < 3; i++) {
        float now = guest_float(self + 0x38u + 4u * i), last = guest_float(self + 0xACu + 4u * i);
        float stretched = now - (now - last) * scale;
        uint32_t bits;
        memcpy(&bits, &stretched, 4);
        saved[i] = TUROK_MEM32(self + 0xACu + 4u * i);
        TUROK_MEM32(self + 0xACu + 4u * i) = bits;
    }
    sub_00148E30_gen();
    for (i = 0; i < 3; i++) TUROK_MEM32(self + 0xACu + 4u * i) = saved[i];
}

/* ── Damage and flight-collision trace (RECOMP_FALL_TRACE=1; issue #25) ──
 *
 * Flight multiplayer: sudden deaths in the air above 150 fps, sometimes
 * right after respawning. The player's TakeDamage is vfunc +0xE0,
 * sub_00143C70 (thiscall (damage, flag), ret 8; damage +0 amount, +4 type,
 * health float at [+0x1E8]). The pteranodon's world-collision state
 * (sub_0014CC30) deals 200 -- death -- for a head-on hit (|velocity .
 * normal| from +0xA78/+0xA84 above 0.45), else +0xA68 x 0.5 on the cooldown
 * +0xA9C; its actor-collision state (sub_0014CDE0) +0xA6C x 0.5. The flight
 * update is sub_00150590 (thiscall (seconds), ret 4). This logs every hit
 * on a player with its caller, both collisions with their inputs, and any
 * health lost elsewhere in the flight update. */
#define PLAYER_VTABLE 0x00222E3Cu
#define MP_PLAYER_VTABLE 0x00225020u   /* multiplayer; TakeDamage sub_00155920 */
extern void sub_00143C70_gen(void);
extern void sub_00155920_gen(void);
extern void xbox_WatchArmVa(uint32_t va);
extern void sub_0014CC30_gen(void);
extern void sub_0014CDE0_gen(void);
extern void sub_00150590_gen(void);

static float player_health(uint32_t self)
{
    uint32_t health = TUROK_MEM32(self + 0x1E8u);
    return health ? guest_float(health) : -1.0f;
}

static void hurt_where(uint32_t self)
{
    fprintf(stderr, " at %.1f %.1f %.1f, velocity %.2f %.2f %.2f, speed %.2f %.2f; player %08X%s\n",
            guest_float(self + 0x38u), guest_float(self + 0x3Cu), guest_float(self + 0x40u),
            guest_float(self + 0x3E4u), guest_float(self + 0x3E8u), guest_float(self + 0x3ECu),
            guest_float(self + 0xAA0u), guest_float(self + 0xAA4u), self,
            self == TUROK_MEM32(0x257E90u) ? " (you)" : "");
}

static void hurt_call(void (*gen)(void))
{
    uint32_t self = g_ecx, from, damage, flag;
    float before;
    if (!fall_trace() || (TUROK_MEM32(self) != PLAYER_VTABLE && TUROK_MEM32(self) != MP_PLAYER_VTABLE)) {
        gen();
        return;
    }
    from = TUROK_MEM32(g_esp), damage = TUROK_MEM32(g_esp + 4u), flag = TUROK_MEM32(g_esp + 8u);
    before = player_health(self);
    gen();
    fprintf(stderr, "[HURT] %.2f type %u (%08X %08X %08X) flag %u from %06X; health %.1f -> %.1f; frame %.5f s;",
            damage ? guest_float(damage) : 0.0f, damage ? TUROK_MEM32(damage + 4u) : 0u,
            damage ? TUROK_MEM32(damage + 8u) : 0u, damage ? TUROK_MEM32(damage + 0xCu) : 0u,
            damage ? TUROK_MEM32(damage + 0x10u) : 0u, flag, from, before, player_health(self), frame_seconds());
    hurt_where(self);
}

void sub_00143C70(void) { hurt_call(sub_00143C70_gen); }
void sub_00155920(void) { hurt_call(sub_00155920_gen); }

/* Once a frame (sub_00104600): the local player's health, every change, and
 * a write trap on it (the toolkit's RECOMP_WATCH, printing the guest return
 * addresses of whatever changes it). */
extern void vy_watch_arm(uint32_t guest);
extern void vy_watch_report(const char *label);

static void hurt_frame(void)
{
    static uint32_t last_player, last_health_va, frames;
    static float last_health;
    static clock_t start;
    uint32_t self, health_va;
    float health;
    if (!fall_trace()) return;
    if (!start) start = clock();
    frames++;
    self = TUROK_MEM32(0x257E90u);
    health_va = self ? TUROK_MEM32(self + 0x1E8u) : 0u;
    health = health_va ? guest_float(health_va) : -1.0f;
    if (self != last_player || health_va != last_health_va) {
        fprintf(stderr, "[LIFE] %.2f s frame %u: player %08X (vtable %08X) health at %08X = %.1f\n",
                (double)(clock() - start) / CLOCKS_PER_SEC, frames, self, self ? TUROK_MEM32(self) : 0u,
                health_va, health);
        if (health_va && self && TUROK_MEM32(self) == MP_PLAYER_VTABLE && !getenv("RECOMP_VY_WATCH"))
            xbox_WatchArmVa(health_va);
    } else if (health != last_health) {
        fprintf(stderr, "[LIFE] %.2f s frame %u: health %.1f -> %.1f; frame %.5f s;",
                (double)(clock() - start) / CLOCKS_PER_SEC, frames, last_health, health, frame_seconds());
        hurt_where(self);
    }
    /* Falling (state 6) for over 0.3 s: every frame of it, up to 4000. */
    if (self) {
        static float falling;
        static unsigned shown;
        uint32_t state = TUROK_MEM32(self + 0x12Cu), body = TUROK_MEM32(self + 0xFCu);
        float seconds = frame_seconds();
        falling = state && TUROK_MEM32(state) == 6u ? falling + seconds : 0.0f;
        if (falling > 0.3f && shown < 4000u) {
            shown++;
            fprintf(stderr, "[AIR] %.3f s falling: y %.4f vy %.3f flags %X at %.2f %.2f; frame %.5f s\n", falling,
                    guest_float(self + 0x3Cu), body ? guest_float(body + 0x1Cu) : 0.0f, TUROK_MEM32(self + 0x45Cu),
                    guest_float(self + 0x38u), guest_float(self + 0x40u), seconds);
        }
    }
    /* RECOMP_VY_WATCH=1: a data breakpoint on the player's vertical speed
     * (body +0x1C); =y on the player's height (+0x3C). Reported every
     * quarter second (src/vy_watch.c). */
    {
        static int on = -1;
        static uint32_t armed;
        static float since;
        uint32_t body = self ? TUROK_MEM32(self + 0xFCu) : 0u;
        if (on < 0) {
            const char *mode = getenv("RECOMP_VY_WATCH");
            on = !mode ? 0 : *mode == 'y' ? 2 : *mode == 'p' ? 3 : *mode == 'g' ? 4 : 1;
        }
        if (on) {
            uint32_t target = on == 4 ? (self ? self + 0x408u : 0u)   /* the gun's sideways offset */
                            : on == 3 ? (s_player_path ? s_player_path + 8u : 0u)
                            : on == 2 ? (self ? self + 0x3Cu : 0u) : (body ? body + 0x1Cu : 0u);
            if (target != armed) {
                vy_watch_arm(target);
                armed = target;
            }
            since += frame_seconds();
            if (since >= 0.25f && self) {
                char label[160];
                uint32_t state = TUROK_MEM32(self + 0x12Cu);
                since = 0.0f;
                snprintf(label, sizeof label, "%.2f s state %u y %.3f at %.2f %.2f frame %.5f s",
                         (double)(clock() - start) / CLOCKS_PER_SEC, state ? TUROK_MEM32(state) : 0u,
                         guest_float(self + 0x3Cu), guest_float(self + 0x38u), guest_float(self + 0x40u),
                         frame_seconds());
                vy_watch_report(label);
            }
        }
    }
    last_player = self, last_health_va = health_va, last_health = health;
}

static void collision_trace(const char *what, uint32_t self)
{
    float a[3], b[3], dot;
    int i;
    for (i = 0; i < 3; i++) a[i] = guest_float(self + 0xA78u + 4u * i), b[i] = guest_float(self + 0xA84u + 4u * i);
    dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    fprintf(stderr, "[CRASH] %s: %.3f %.3f %.3f . %.3f %.3f %.3f = %.3f; cooldown %.3f; health %.1f; frame %.5f s;",
            what, a[0], a[1], a[2], b[0], b[1], b[2], dot, guest_float(self + 0xA9Cu), player_health(self),
            frame_seconds());
    hurt_where(self);
}

void sub_0014CC30(void)
{
    if (fall_trace()) collision_trace("world", g_ecx);
    sub_0014CC30_gen();
}

void sub_0014CDE0(void)
{
    if (fall_trace()) collision_trace("actor", g_ecx);
    sub_0014CDE0_gen();
}

void sub_00150590(void)
{
    uint32_t self = g_ecx;
    float before, after, seconds;
    if (!fall_trace()) {
        sub_00150590_gen();
        return;
    }
    seconds = guest_float(g_esp + 4u);
    before = player_health(self);
    sub_00150590_gen();
    after = player_health(self);
    if (after != before) {
        fprintf(stderr, "[FLY] health %.1f -> %.1f in the flight update (%.5f s);", before, after, seconds);
        hurt_where(self);
    }
}

/* ── Native rewrites: the switch back ───────────────────────────
 *
 * RECOMP_NATIVE=0 runs the generated code again for every native rewrite
 * below that names it (animation decoding, the 4x4 product, object
 * lighting); each also has its own switch. For telling whether one of them
 * is behind a new problem. */
static int native_enabled(const char *own)
{
    const char *all = getenv("RECOMP_NATIVE"), *one = getenv(own);
    int on = !(all && *all == '0') && !(one && *one == '0');
    if (!on) fprintf(stderr, "[NATIVE] %s off: the generated code runs\n", own);
    return on;
}

/* ── Object lighting (sub_00036D00) ────────────────────────────
 *
 * thiscall (ecx = the light set; five arguments, ret 0x14): gathers the
 * lights reaching an object -- each light's position, colour and range
 * against the object's -- sets the ambient, and folds them into the three
 * 4x4 light matrices the object is drawn with (merging the nearest pairs
 * while there are more than four). Run for every lit object every frame: a
 * tenth of the game thread in the Chapter 1 jungle, almost all of it the
 * generated code's register and x87-stack traffic.
 *
 * src/native/sub_00036D00.c is the same instructions lifted again by
 * tools/x86_lift.py, registers and x87 slots in locals (see there).
 * RECOMP_NATIVE_LIGHTING=0 (or RECOMP_NATIVE=0) runs the generated code.
 * RECOMP_LIGHTING_VERIFY=<n>: every nth call runs both from the same guest
 * RAM and registers and reports any byte or register that differs (another
 * thread writing RAM in that moment shows up too: look at the addresses). */
extern void sub_00036D00_gen(void);
#ifdef RECOMP_HAVE_LIFTED_LIGHTING
extern void sub_00036D00_native(void);
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_seh_ebp;
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS uint16_t g_fp_cc;

#define LIGHTING_RAM (64u << 20)

typedef struct LightingRegs {
    uint32_t r[8];
    int top;
    double stack[8];
    uint16_t cc;
} LightingRegs;

static void lighting_regs_save(LightingRegs *s)
{
    s->r[0] = g_eax; s->r[1] = g_ecx; s->r[2] = g_edx; s->r[3] = g_ebx;
    s->r[4] = g_esi; s->r[5] = g_edi; s->r[6] = g_esp; s->r[7] = g_seh_ebp;
    s->top = g_fp_top; memcpy(s->stack, g_fp_stack, sizeof s->stack); s->cc = g_fp_cc;
}

static void lighting_regs_load(const LightingRegs *s)
{
    g_eax = s->r[0]; g_ecx = s->r[1]; g_edx = s->r[2]; g_ebx = s->r[3];
    g_esi = s->r[4]; g_edi = s->r[5]; g_esp = s->r[6]; g_seh_ebp = s->r[7];
    g_fp_top = s->top; memcpy(g_fp_stack, s->stack, sizeof s->stack); g_fp_cc = s->cc;
}

/* RECOMP_LIGHTING_TRACE=1 (with the verify): the traced copies of both
 * (tools/lift_trace.py) record registers at each label; the first record
 * that differs names the block the lifted code got wrong. */
typedef struct LiftRecord { uint32_t at, r[7]; double top; int fp_top; uint32_t frame; } LiftRecord;
#define LIFT_RECORDS 200000
static LiftRecord *s_lift_records[2];
static int s_lift_side = -1, s_lift_count[2];
void lift_trace(uint32_t at, uint32_t a, uint32_t c, uint32_t d, uint32_t b, uint32_t si, uint32_t di, uint32_t sp,
                double top, int fp_top)
{
    LiftRecord *r;
    if (s_lift_side < 0 || s_lift_count[s_lift_side] >= LIFT_RECORDS) return;
    r = &s_lift_records[s_lift_side][s_lift_count[s_lift_side]++];
    r->at = at; r->r[0] = a; r->r[1] = c; r->r[2] = d; r->r[3] = b; r->r[4] = si; r->r[5] = di; r->r[6] = sp;
    r->top = top; r->fp_top = fp_top & 7;
    {   /* the frame: 0x200 bytes up from esp */
        uint32_t h = 2166136261u, k;
        for (k = 0; k < 0x200u; k += 4) { h ^= TUROK_MEM32(sp + k); h *= 16777619u; }
        r->frame = h;
    }
}
typedef struct LiftStore { uint32_t address, value, after; } LiftStore;
static LiftStore *s_lift_stores[2];
static int s_lift_store_count[2];
void lift_store(uint32_t address, uint32_t value)
{
    LiftStore *r;
    int side = s_lift_side;
    if (side < 0 || !s_lift_stores[side] || s_lift_store_count[side] >= LIFT_RECORDS) return;
    r = &s_lift_stores[side][s_lift_store_count[side]++];
    r->address = address; r->value = value;
    r->after = s_lift_count[side] ? s_lift_records[side][s_lift_count[side] - 1].at : 0;
}
#ifdef RECOMP_HAVE_LIFT_TRACE
extern void sub_00036D00_tracelift(void);
extern void sub_00036D00_tracegen(void);
#else
#define sub_00036D00_tracelift sub_00036D00_native
#define sub_00036D00_tracegen sub_00036D00_gen
#endif

static void lift_trace_compare(int entry_top)
{
    static unsigned reported;
    int n = s_lift_count[0] < s_lift_count[1] ? s_lift_count[0] : s_lift_count[1], k;
    for (k = 0; k < n; k++) {
        LiftRecord *x = &s_lift_records[0][k], *y = &s_lift_records[1][k];
        int same = x->at == y->at && !memcmp(x->r, y->r, sizeof x->r) && x->fp_top == y->fp_top && x->frame == y->frame &&
                   (x->fp_top == (entry_top & 7) || !memcmp(&x->top, &y->top, sizeof x->top));
        if (same) continue;
        if (reported++ < 12) {
            fprintf(stderr, "[LIFT] record %d differs (after %08X): lifted at %08X, generated at %08X; frame %08X/%08X\n", k,
                    k ? s_lift_records[0][k - 1].at : 0, x->at, y->at, x->frame, y->frame);
            fprintf(stderr, "[LIFT]   lifted    eax %08X ecx %08X edx %08X ebx %08X esi %08X edi %08X esp %08X top %d %.9g\n",
                    x->r[0], x->r[1], x->r[2], x->r[3], x->r[4], x->r[5], x->r[6], x->fp_top, x->top);
            fprintf(stderr, "[LIFT]   generated eax %08X ecx %08X edx %08X ebx %08X esi %08X edi %08X esp %08X top %d %.9g\n",
                    y->r[0], y->r[1], y->r[2], y->r[3], y->r[4], y->r[5], y->r[6], y->fp_top, y->top);
        }
        return;
    }
    {
        int m = s_lift_store_count[0] < s_lift_store_count[1] ? s_lift_store_count[0] : s_lift_store_count[1];
        for (k = 0; k < m; k++) {
            LiftStore *x = &s_lift_stores[0][k], *y = &s_lift_stores[1][k];
            if (x->address == y->address && x->value == y->value) continue;
            if (reported++ < 12)
                fprintf(stderr, "[LIFT] store %d differs: lifted [%08X] = %08X (after label %08X), generated [%08X] = %08X (after %08X)\n",
                        k, x->address, x->value, x->after, y->address, y->value, y->after);
            return;
        }
        if (s_lift_store_count[0] != s_lift_store_count[1] && reported++ < 12)
            fprintf(stderr, "[LIFT] stores: %d lifted, %d generated\n", s_lift_store_count[0], s_lift_store_count[1]);
    }
    if (s_lift_count[0] != s_lift_count[1] && reported++ < 12)
        fprintf(stderr, "[LIFT] same for %d records, then one ends (%d / %d)\n", n, s_lift_count[0], s_lift_count[1]);
}

static struct { uint32_t address, times; } s_lighting_seen[256];
static uint32_t s_lighting_seen_count, s_lighting_reported;

static void lighting_verify(void)
{
    static uint8_t *before, *native;
    static unsigned checks, mismatches;
    uint8_t *ram = (uint8_t *)(uintptr_t)g_xbox_mem_offset;
    LightingRegs entry, after_native, after_original;
    uint32_t page, differ = 0, shown = 0;
    if (!before) {
        before = (uint8_t *)malloc(LIGHTING_RAM);
        native = (uint8_t *)malloc(LIGHTING_RAM);
        if (!before || !native) { sub_00036D00_native(); return; }
    }
    lighting_regs_save(&entry);
    memcpy(before, ram, LIGHTING_RAM);
    {
        static int trace = -1;
        if (trace < 0) {
            trace = getenv("RECOMP_LIGHTING_TRACE") != NULL;
            if (trace) {
                s_lift_records[0] = (LiftRecord *)malloc(sizeof(LiftRecord) * LIFT_RECORDS);
                s_lift_records[1] = (LiftRecord *)malloc(sizeof(LiftRecord) * LIFT_RECORDS);
                s_lift_stores[0] = (LiftStore *)malloc(sizeof(LiftStore) * LIFT_RECORDS);
                s_lift_stores[1] = (LiftStore *)malloc(sizeof(LiftStore) * LIFT_RECORDS);
                if (!s_lift_records[0] || !s_lift_records[1] || !s_lift_stores[0] || !s_lift_stores[1]) trace = 0;
            }
        }
        s_lift_side = -1;
        if (trace) { s_lift_count[0] = s_lift_count[1] = 0; s_lift_store_count[0] = s_lift_store_count[1] = 0; s_lift_side = 0; sub_00036D00_tracelift(); s_lift_side = -1; }
        else if (getenv("RECOMP_LIGHTING_VERIFY_SELF")) sub_00036D00_gen();
        else sub_00036D00_native();
    }
    lighting_regs_save(&after_native);
    memcpy(native, ram, LIGHTING_RAM);
    for (page = 0; page < LIGHTING_RAM; page += 4096u)   /* back as it was */
        if (memcmp(ram + page, before + page, 4096u)) memcpy(ram + page, before + page, 4096u);
    lighting_regs_load(&entry);
    if (s_lift_records[0]) {
        s_lift_side = 1; sub_00036D00_tracegen(); s_lift_side = -1;
        lift_trace_compare(entry.top);
    } else sub_00036D00_gen();
    lighting_regs_save(&after_original);
    for (page = 0; page < LIGHTING_RAM; page += 4096u) {
        uint32_t i;
        if (!memcmp(ram + page, native + page, 4096u)) continue;
        for (i = 0; i < 4096u; i += 4) {
            uint32_t a, b;
            memcpy(&a, native + page + i, 4); memcpy(&b, ram + page + i, 4);
            if (a == b) continue;
            differ++;
            {   /* every address that ever differed, with how often */
                uint32_t k;
                for (k = 0; k < s_lighting_seen_count && s_lighting_seen[k].address != page + i; k++) {}
                if (k < s_lighting_seen_count) s_lighting_seen[k].times++;
                else if (k < 256) { s_lighting_seen[k].address = page + i; s_lighting_seen[k].times = 1; s_lighting_seen_count++; }
            }
            /* other threads' stacks, the D3D device's counters, the frame clocks */
            if ((page + i >= 0x007B0000u && page + i < 0x00800000u) ||
                (page + i >= 0x001A4400u && page + i < 0x001A4C00u) || page + i == 0x00257DF0u ||
                (page + i >= 0x01081E00u && page + i < 0x01082900u) || page + i < 0x4000u) continue;
            if (shown < 8 && s_lighting_reported < 24) {
                fprintf(stderr, "[LIGHTING]   %08X: native %08X, original %08X (before %08X)\n",
                        page + i, a, b, *(uint32_t *)(before + page + i));
                shown++;
            }
        }
    }
    if (shown) s_lighting_reported++;
    if (shown) fprintf(stderr, "[LIGHTING]   (check %u; ecx %08X, esp at entry %08X)\n", checks + 1, entry.r[1], entry.r[6]);
    after_native.top &= 7; after_original.top &= 7;
    checks++;
    if (differ || memcmp(after_native.r, after_original.r, sizeof after_native.r) ||
        after_native.top != after_original.top || after_native.cc != after_original.cc) {
        if (mismatches++ < 16)
            fprintf(stderr, "[LIGHTING] check %u differs: %u words; eax %08X/%08X esp %08X/%08X ebx %08X/%08X "
                    "esi %08X/%08X edi %08X/%08X ecx %08X/%08X edx %08X/%08X x87 top %d/%d cc %04X/%04X\n",
                    checks, differ, after_native.r[0], after_original.r[0], after_native.r[6], after_original.r[6],
                    after_native.r[3], after_original.r[3], after_native.r[4], after_original.r[4],
                    after_native.r[5], after_original.r[5], after_native.r[1], after_original.r[1],
                    after_native.r[2], after_original.r[2], after_native.top, after_original.top,
                    after_native.cc, after_original.cc);
    }
    if ((checks & 63u) == 0) {
        uint32_t k;
        fprintf(stderr, "[LIGHTING] %u checks, %u differ; addresses that ever differed:", checks, mismatches);
        for (k = 0; k < s_lighting_seen_count; k++)
            fprintf(stderr, " %08X x%u", s_lighting_seen[k].address, s_lighting_seen[k].times);
        fprintf(stderr, "\n");
    }
}

#endif   /* RECOMP_HAVE_LIFTED_LIGHTING */

void sub_00036D00(void)
{
#ifndef RECOMP_HAVE_LIFTED_LIGHTING
    sub_00036D00_gen();   /* src/native/sub_00036D00.c not made: tools/x86_lift.py */
#else
    static int mode = -1, every = 0;
    static unsigned calls;
    if (mode < 0) {
        const char *verify = getenv("RECOMP_LIGHTING_VERIFY");
        mode = native_enabled("RECOMP_NATIVE_LIGHTING");
        if (mode && verify && *verify) {
            every = atoi(verify);
            if (every < 1) every = 256;
            mode = 2;
            fprintf(stderr, "[LIGHTING] verifying every %d calls\n", every);
        }
    }
    if (mode == 0) { sub_00036D00_gen(); return; }
    if (mode == 2 && ++calls % (unsigned)every == 0) {
        /* RECOMP_LIGHTING_VERIFY_AFTER=<s>: from that many seconds after the first call */
        static time_t from = 0;
        if (!from) {
            const char *after = getenv("RECOMP_LIGHTING_VERIFY_AFTER");
            from = time(NULL) + (after ? atoi(after) : 0) + (time_t)(after ? 0 : -1);
        }
        if (time(NULL) >= from) { lighting_verify(); return; }
    }
    sub_00036D00_native();
#endif
}

/* ── 4x4 matrix product (sub_000230E0) ─────────────────────────
 *
 * cdecl (out, a, b): out = a x b for 4x4 matrices whose last row is
 * (0, 0, 0, 1), rows of four floats; out's last row is written as that.
 * The culling and transform code (sub_000205xx-00020Exx) calls it for every
 * object it places: at the waterfall of a later level, 7% of the game
 * thread. Same products and sums in the same order with the generated
 * code's rounding (mx_round), all reads before any write as the original's.
 * It also leaves two of its sums in its own argument slots, as the original
 * does. RECOMP_MATRIX_VERIFY=1 compares it with the original too;
 * RECOMP_NATIVE_MATRIX4=0 (or RECOMP_NATIVE=0) runs the original instead. */
extern void sub_000230E0_gen(void);

static void matrix4_product_native(uint32_t out, uint32_t a, uint32_t b, uint32_t slots)
{
    double v1 = MXADD(MXSUM3(MXMUL(MX(b, 14), MX(a, 10)), MXMUL(MX(a, 6), MX(b, 13)), MXMUL(MX(a, 2), MX(b, 12))), MX(a, 14));
    double v2 = MXSUM3(MXMUL(MX(b, 10), MX(a, 10)), MXMUL(MX(b, 9), MX(a, 6)), MXMUL(MX(a, 2), MX(b, 8)));
    double v3 = MXSUM3(MXMUL(MX(b, 6), MX(a, 10)), MXMUL(MX(a, 2), MX(b, 4)), MXMUL(MX(a, 6), MX(b, 5)));
    double v4 = MXSUM3(MXMUL(MX(a, 6), MX(b, 1)), MXMUL(MX(a, 2), MX(b, 0)), MXMUL(MX(a, 10), MX(b, 2)));
    double t8 = MXADD(MXSUM3(MXMUL(MX(b, 14), MX(a, 9)), MXMUL(MX(a, 5), MX(b, 13)), MXMUL(MX(a, 1), MX(b, 12))), MX(a, 13));
    double t0 = MXSUM3(MXMUL(MX(b, 10), MX(a, 9)), MXMUL(MX(b, 9), MX(a, 5)), MXMUL(MX(a, 1), MX(b, 8)));
    double v5 = MXSUM3(MXMUL(MX(b, 6), MX(a, 9)), MXMUL(MX(a, 1), MX(b, 4)), MXMUL(MX(a, 5), MX(b, 5)));
    double v6 = MXSUM3(MXMUL(MX(a, 5), MX(b, 1)), MXMUL(MX(a, 9), MX(b, 2)), MXMUL(MX(a, 1), MX(b, 0)));
    double t4 = MXADD(MXSUM3(MXMUL(MX(b, 14), MX(a, 8)), MXMUL(MX(b, 12), MX(a, 0)), MXMUL(MX(a, 4), MX(b, 13))), MX(a, 12));
    double t18 = MXSUM3(MXMUL(MX(b, 9), MX(a, 4)), MXMUL(MX(b, 8), MX(a, 0)), MXMUL(MX(b, 10), MX(a, 8)));
    double t14 = MXSUM3(MXMUL(MX(a, 4), MX(b, 5)), MXMUL(MX(b, 6), MX(a, 8)), MXMUL(MX(b, 4), MX(a, 0)));
    double v7 = MXSUM3(MXMUL(MX(a, 0), MX(b, 0)), MXMUL(MX(a, 8), MX(b, 2)), MXMUL(MX(a, 4), MX(b, 1)));
    /* its locals: arg slots 2 and 3 (esp+0x14/0x18 inside it) */
    mx_store(slots + 4u, t14);
    mx_store(slots + 8u, t18);
    mx_store(out + 0x10u, t14);
    TUROK_MEM32(out + 0x0Cu) = 0;
    TUROK_MEM32(out + 0x1Cu) = 0;
    mx_store(out + 0x00u, v7);
    mx_store(out + 0x20u, t18);
    mx_store(out + 0x04u, v6);
    TUROK_MEM32(out + 0x2Cu) = 0;
    mx_store(out + 0x08u, v4);
    mx_store(out + 0x24u, t0);
    mx_store(out + 0x30u, t4);
    mx_store(out + 0x14u, v5);
    mx_store(out + 0x18u, v3);
    mx_store(out + 0x28u, v2);
    mx_store(out + 0x38u, v1);
    mx_store(out + 0x34u, t8);
    TUROK_MEM32(out + 0x3Cu) = 0x3F800000u;
    g_eax = out;
    g_ecx = TUROK_MEM32(out + 0x30u);
    g_edx = TUROK_MEM32(out + 0x34u);
}

void sub_000230E0(void)
{
    static int verify = -1;
    uint32_t out = TUROK_MEM32(g_esp + 4u), a = TUROK_MEM32(g_esp + 8u), b = TUROK_MEM32(g_esp + 12u);
    static int native = -1;
    if (native < 0) native = native_enabled("RECOMP_NATIVE_MATRIX4");
    if (!native) { sub_000230E0_gen(); return; }
    if (verify < 0) verify = getenv("RECOMP_MATRIX_VERIFY") != NULL;
    if (verify) {
        uint32_t saved_out[16], saved_a[16], saved_b[16], saved_slots[2], native[18], original[18], i;
        static unsigned checks, mismatches;
        for (i = 0; i < 16; i++) {
            saved_out[i] = TUROK_MEM32(out + i * 4u);
            saved_a[i] = TUROK_MEM32(a + i * 4u);
            saved_b[i] = TUROK_MEM32(b + i * 4u);
        }
        saved_slots[0] = TUROK_MEM32(g_esp + 8u); saved_slots[1] = TUROK_MEM32(g_esp + 12u);
        matrix4_product_native(out, a, b, g_esp + 4u);
        for (i = 0; i < 16; i++) native[i] = TUROK_MEM32(out + i * 4u);
        native[16] = TUROK_MEM32(g_esp + 8u); native[17] = TUROK_MEM32(g_esp + 12u);
        for (i = 0; i < 16; i++) TUROK_MEM32(out + i * 4u) = saved_out[i];
        for (i = 0; i < 16; i++) { TUROK_MEM32(a + i * 4u) = saved_a[i]; TUROK_MEM32(b + i * 4u) = saved_b[i]; }
        TUROK_MEM32(g_esp + 8u) = saved_slots[0]; TUROK_MEM32(g_esp + 12u) = saved_slots[1];
        {
            uint32_t slots = g_esp + 4u;
            sub_000230E0_gen();   /* pops its return address */
            for (i = 0; i < 16; i++) original[i] = TUROK_MEM32(out + i * 4u);
            original[16] = TUROK_MEM32(slots + 4u); original[17] = TUROK_MEM32(slots + 8u);
        }
        checks++;
        if (memcmp(native, original, sizeof native) && mismatches++ < 16)
            fprintf(stderr, "[MATRIX4] native product differs (check %u, control word %04X)\n",
                    checks, (unsigned)g_fp_control_word);
        if ((checks & 0xFFFFFu) == 0)
            fprintf(stderr, "[MATRIX4] %u checks, %u differ\n", checks, mismatches);
        return;
    }
    matrix4_product_native(out, a, b, g_esp + 4u);
    g_esp += 4u;   /* ret */
}

/* ── Animation channel decoding (sub_000765B0) ─────────────────
 *
 * cdecl (stream, count, out): unpacks one channel of a compressed
 * animation into `out`, 16-bit values. The stream is {bit offset 0-7,
 * bits left, ?, byte pointer}; a field of n bits is the dword at the
 * pointer shifted right (arithmetically) by the bit offset. A 16-bit start
 * value comes first, then a 4-bit width: 0 means `count` raw 16-bit deltas;
 * otherwise groups of deltas that many bits wide, sign and magnitude, each
 * group ended by the escape code (the sign bit alone) and followed, while
 * fewer than `count` values are out, by the next group's width. Every delta
 * is added to a velocity and the velocity to the value (second-order
 * delta coding), and each value stored.
 *
 * Its only caller (sub_00076760, three channels a key) runs it for every
 * animated thing each frame: at the waterfall of a later level, a tenth of
 * the game thread, every step through the generated code's flags and
 * memory. Done here with the same arithmetic. The caller reads nothing it
 * leaves but the outputs and the stream; its own first argument slot ends
 * as the original leaves it (its loop counter). RECOMP_ANIM_VERIFY=1 runs the
 * original after it and reports any difference; RECOMP_NATIVE_ANIM=0 (or
 * RECOMP_NATIVE=0) runs the original instead. */
extern void sub_000765B0_gen(void);
#define TUROK_MEM16(va) (*(volatile uint16_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define TUROK_MEM8(va) (*(volatile uint8_t *)((uintptr_t)(va) + g_xbox_mem_offset))

typedef struct AnimDecode {
    uint32_t offset, remaining, pointer;   /* the stream's +0, +4, +0xC after */
    uint32_t slot;                          /* the first argument's slot after */
    uint32_t words;                         /* values written */
} AnimDecode;

#define ANIM_MAX_WORDS 65536u
static uint16_t s_anim_words[ANIM_MAX_WORDS];

/* 0 when the stream runs past ANIM_MAX_WORDS values (left to the original). */
static int anim_decode_native(uint32_t stream, int32_t count, AnimDecode *result)
{
    uint32_t offset = TUROK_MEM32(stream), remaining = TUROK_MEM32(stream + 4u), pointer = TUROK_MEM32(stream + 0xCu);
    uint32_t value, velocity = 0, words = 0, peek;
#define ANIM_FIELD() ((uint32_t)((int32_t)TUROK_MEM32(pointer) >> (offset & 31u)))
#define ANIM_ADVANCE(n) do { uint32_t next_ = offset + (n); pointer += (uint32_t)((int32_t)next_ >> 3); \
                             offset = next_ & 7u; remaining -= (n); } while (0)
    value = ANIM_FIELD();
    ANIM_ADVANCE(16u);
    peek = ((uint32_t)TUROK_MEM8(pointer) | ((uint32_t)TUROK_MEM8(pointer + 1u) << 8)) >> (offset & 31u);
    if ((peek & 0xFu) == 0) {
        ANIM_ADVANCE(4u);
        result->slot = TUROK_MEM32(g_esp + 4u);
        if (count > 0) {
            int32_t left;
            if ((uint32_t)count > ANIM_MAX_WORDS) return 0;
            for (left = count; left; left--) {
                uint32_t delta = ANIM_FIELD();
                ANIM_ADVANCE(16u);
                velocity += delta;
                value += velocity;
                s_anim_words[words++] = (uint16_t)value;
            }
            result->slot = 0;
        }
    } else {
        uint32_t done = 0;
        result->slot = 0;
        if (count > 0) for (;;) {
            uint32_t width = ANIM_FIELD() & 0xFu, magnitude, sign, mask;
            ANIM_ADVANCE(4u);
            magnitude = ~(0xFFFFu << ((width - 1u) & 31u));
            sign = (1u << ((width - 1u) & 31u)) & 0xFFFFu;
            mask = ~(0xFFFFFFFFu << (width & 31u));
            for (;;) {
                uint32_t field = ANIM_FIELD() & mask, extended, delta;
                ANIM_ADVANCE(width);
                extended = (uint32_t)(int32_t)(int16_t)field;
                if (!(sign & extended)) delta = field;
                else if (extended == sign) break;   /* the group's end */
                else delta = 0u - (field & magnitude);
                if (words >= ANIM_MAX_WORDS) return 0;
                velocity += delta;
                value += velocity;
                s_anim_words[words++] = (uint16_t)value;
                done++;
            }
            if ((int32_t)done >= count) break;
        }
        result->slot = done;
    }
#undef ANIM_FIELD
#undef ANIM_ADVANCE
    result->offset = offset;
    result->remaining = remaining;
    result->pointer = pointer;
    result->words = words;
    return 1;
}

void sub_000765B0(void)
{
    static int verify = -1;
    uint32_t stream = TUROK_MEM32(g_esp + 4u), out = TUROK_MEM32(g_esp + 12u), i;
    int32_t count = (int32_t)TUROK_MEM32(g_esp + 8u);
    AnimDecode result;
    static int native = -1;
    if (native < 0) native = native_enabled("RECOMP_NATIVE_ANIM");
    if (!native) { sub_000765B0_gen(); return; }
    if (verify < 0) verify = getenv("RECOMP_ANIM_VERIFY") != NULL;
    if (!anim_decode_native(stream, count, &result)) {
        sub_000765B0_gen();
        return;
    }
    if (verify) {
        static unsigned checks, mismatches;
        uint32_t slot_va = g_esp + 4u;
        int same;
        sub_000765B0_gen();   /* pops its return address */
        same = TUROK_MEM32(stream) == result.offset && TUROK_MEM32(stream + 4u) == result.remaining &&
               TUROK_MEM32(stream + 0xCu) == result.pointer && TUROK_MEM32(slot_va) == result.slot;
        for (i = 0; same && i < result.words; i++) same = TUROK_MEM16(out + i * 2u) == s_anim_words[i];
        checks++;
        if (!same && mismatches++ < 16)
            fprintf(stderr, "[ANIM] native decode differs (check %u: %u values, stream %08X)\n", checks,
                    result.words, stream);
        if ((checks & 0xFFFFFu) == 0)
            fprintf(stderr, "[ANIM] %u checks, %u differ\n", checks, mismatches);
        return;
    }
    for (i = 0; i < result.words; i++) TUROK_MEM16(out + i * 2u) = s_anim_words[i];
    TUROK_MEM32(stream) = result.offset;
    TUROK_MEM32(stream + 4u) = result.remaining;
    TUROK_MEM32(stream + 0xCu) = result.pointer;
    TUROK_MEM32(g_esp + 4u) = result.slot;
    g_eax = 1;
    g_ecx = out + result.words * 2u;
    g_edx = result.offset;
    g_esp += 4u;   /* ret */
}

/* ── Mid-air landings at high frame rates ──────────────────────
 *
 * The on-foot movement (sub_0014A020, this-call, ret 4: the frame's
 * seconds; run by the run and jump states, among others) ends a fall when
 * the player has moved less than 0.0005 units up or down this frame (the
 * height +0x3C against the last frame's +0xB0): a stall check, for a fall
 * caught on something that never reports ground. That is per frame. At 30
 * fps the frames at the top of a jump move ~0.02, but above ~150 fps they
 * move less than 0.0005, so the jump "landed" in mid-air: idle, then run,
 * hanging in the air for seconds while the run state's own fall test (also
 * per frame) slowly let it sink. Above 30 fps the test is made over a 30th
 * of a second instead of a frame: a frame that moved under 0.0005 lets the
 * fall end only once the player has moved under 0.0005 over the last 1/30 s
 * -- what a 30 fps frame would have measured. Until then the check is
 * passed by nudging the remembered height for it alone: the method the
 * movement calls just before it (vfunc +0x32C, sub_001425C0, returning to
 * 0x14A4CA) is wrapped; the real height is put back when the movement
 * returns. (It used to hold the check to 0.015 units a second, a 30 fps
 * frame's 0.0005 as a speed. A fall that starts from rest -- the top of a
 * jump, the step after a landing -- is slower than that for its first
 * frames above ~500 fps, so jumps hovered and Withering Heights' trampolines
 * buzzed without lifting the player: issue #25.) At 30 fps and below nothing
 * changes. */
extern void sub_0014A020_gen(void);
extern void sub_001425C0_gen(void);
static float s_move_seconds;
static uint32_t s_move_frames;        /* on-foot movement updates so far */
static float s_last_move_seconds;
static uint32_t s_stall_player, s_stall_saved, s_stall_set;

void sub_0014A020(void)
{
    uint32_t player = g_ecx;
    float seconds = s_move_seconds;
    s_move_seconds = guest_float(g_esp + 4u);
    s_move_frames++;
    s_last_move_seconds = s_move_seconds;
    s_stall_player = 0;
    sub_0014A020_gen();
    if (s_stall_player == player && TUROK_MEM32(player + 0xB0u) == s_stall_set)
        TUROK_MEM32(player + 0xB0u) = s_stall_saved;
    s_stall_player = 0;
    s_move_seconds = seconds;
}

void sub_001425C0(void)
{
    static int enabled = -1;   /* RECOMP_FALL_STALL_FIX=0: the per-frame test as the game has it */
    uint32_t player = g_ecx, state = player ? TUROK_MEM32(player + 0x12Cu) : 0;
    /* From the movement directly, or (multiplayer players, vtable 0x225020)
     * through their own vfunc +0x32C, sub_001573D0, which calls this first
     * (returning to 0x1573D8, with the movement's 0x14A4CA two slots up). */
    uint32_t caller = TUROK_MEM32(g_esp);
    int from_move = caller == 0x0014A4CAu || (caller == 0x001573D8u && TUROK_MEM32(g_esp + 8u) == 0x0014A4CAu);
    static uint32_t window_player, window_frame;
    static float window_seconds, window_y;   /* the stalled frames so far, and the height before them */
    if (enabled < 0) enabled = !(getenv("RECOMP_FALL_STALL_FIX") && *getenv("RECOMP_FALL_STALL_FIX") == '0');
    if (enabled && from_move && state && TUROK_MEM32(state) == 6u &&
        s_move_seconds > 0.0f && s_move_seconds < 1.0f / 30.0f) {
        float y = guest_float(player + 0x3Cu), last = guest_float(player + 0xB0u);
        if (fabsf(y - last) < 0.0005f) {
            if (window_player != player || window_frame + 1u != s_move_frames) {
                window_player = player;
                window_seconds = 0.0f;
                window_y = last;
            }
            window_frame = s_move_frames;
            window_seconds += s_move_seconds;
            if (window_seconds < 1.0f / 30.0f || fabsf(y - window_y) >= 0.0005f) {
                float nudged = y - 0.001f;   /* reads as falling */
                if (window_seconds >= 1.0f / 30.0f) {   /* moved enough over a 30 fps frame: start another */
                    window_seconds = 0.0f;
                    window_y = y;
                }
                s_stall_player = player;
                s_stall_saved = TUROK_MEM32(player + 0xB0u);
                memcpy(&s_stall_set, &nudged, 4);
                TUROK_MEM32(player + 0xB0u) = s_stall_set;
            }
        } else {
            window_player = 0;
        }
    }
    sub_001425C0_gen();
}

/* ── Ground lost on stair edges at high frame rates ────────────
 *
 * After its collision the player's update (sub_0014E960) asks whether the
 * surface it touched counts as ground: its normal within 45 degrees of
 * straight up (sub_00142170, stdcall: the normal; 1 = ground). If not, it
 * clears the on-ground flag (+0x45C bit 0, setting 0x40), gravity starts to
 * build, and a frame later the player is falling (state 6, from 0x14EE46 or
 * the movement's own test). On a step's edge the normal leans further than
 * 45 degrees. At 30 fps a frame carries the player onto the step's top
 * before the next test; at 300+ the player touches the edge for many tiny
 * frames, so every step of a staircase (the Senate's slatted steps) dropped
 * it into the fall state, where it has no forward control and the gun's
 * bob is left where the fall found it: it stalled at each step, the gun
 * warped. A surface too steep to stand on now counts as ground until it has
 * been so for 1/30 s of on-foot movement -- one Xbox frame. A real slope or
 * ledge is let go no later than at 30 fps. RECOMP_GROUND_GRACE=0: as the
 * game has it.
 *
 * RECOMP_STATE_TRACE=1 logs the player's state changes, through the generic
 * set-state (sub_00039FF0, this-call, ret 4: the state): who asked, height,
 * flags and the gun's offsets. */
extern void sub_00039FF0_gen(void);
extern void sub_00142170_gen(void);

void sub_00142170(void)
{
    static int enabled = -1;
    static uint32_t last_frame;
    static float steep;
    uint32_t caller = TUROK_MEM32(g_esp);
    if (enabled < 0) enabled = !(getenv("RECOMP_GROUND_GRACE") && *getenv("RECOMP_GROUND_GRACE") == '0');
    sub_00142170_gen();
    if (!enabled || caller != 0x0014ED9Du || !(s_last_move_seconds > 0.0f && s_last_move_seconds < 1.0f / 30.0f))
        return;
    if (g_eax) { steep = 0.0f; return; }                       /* ground */
    if (last_frame + 1u != s_move_frames && last_frame != s_move_frames) steep = 0.0f;   /* a new edge */
    if (last_frame != s_move_frames) steep += s_last_move_seconds;
    last_frame = s_move_frames;
    if (steep < 1.0f / 30.0f) g_eax = 1;                         /* still ground, for now */
}

void sub_00039FF0(void)
{
    static int trace = -1;
    uint32_t actor = g_ecx;
    /* The armoured Sleg's recovery callback can request Idle on the
     * first frame of an Expel launch. A wall hit only deals impact
     * damage while the actor is in Knockback or Knockfore. Ignore that
     * early animation recovery while the impulse is still lifting it.
     * The original contact handler still ends the launch on impact. */
    if (actor && TUROK_MEM32(g_esp) == 0x0013D646u &&
        TUROK_MEM32(g_esp + 4u) == 1u) {
        uint32_t state = TUROK_MEM32(actor + 0x12Cu);
        uint32_t id = state ? TUROK_MEM32(state) : 0u;
        if (id == 63u || id == 64u) {
            uint32_t physics = TUROK_MEM32(actor + 0xFCu);
            if (physics && guest_float(physics + 0x1Cu) > 0.0f) {
                g_eax = 0;   /* state change refused, like sub_00046940 */
                g_esp += 8;  /* ret 4 */
                return;
            }
        }
    }
    if (trace < 0) trace = getenv("RECOMP_STATE_TRACE") != NULL;
    if (trace && actor && (TUROK_MEM32(actor) == 0x00222CE8u || TUROK_MEM32(actor) == 0x00225020u)) {
        uint32_t state = TUROK_MEM32(actor + 0x12Cu);
        fprintf(stderr, "[STATE] %u -> %u from %08X y %.4f flags %X gun %.4f %.4f %.4f move %u dt %.4f\n",
                state ? TUROK_MEM32(state) : 0u, TUROK_MEM32(g_esp + 4u), TUROK_MEM32(g_esp),
                guest_float(actor + 0x3Cu), TUROK_MEM32(actor + 0x45Cu),
                guest_float(actor + 0x408u), guest_float(actor + 0x40Cu), guest_float(actor + 0x410u),
                s_move_frames, s_last_move_seconds);
    }
    /* Multiplayer gun drift. The walk bob eases the gun's sideways offset
     * (+0x408) towards trig(the state's clock) x amplitude, and each state's
     * clock starts at 0 -- the right-hand half of the swing. Switching between
     * forward, strafe and attack faster than half a step keeps the target to
     * the right, so the gun crept right until nearly off screen, returning
     * only when the player stopped (any frame rate; the Xbox's own
     * behaviour). Single player's movement states shrink the gun offsets to
     * 0.4 when entered (sub_00145FE0); the multiplayer character's states
     * (vtable 0x225020) do not. They get the same shrink of the gun offsets
     * (+0x408, +0x40C, +0x1A70, +0x1A74) on every change of state. */
    if (actor && TUROK_MEM32(actor) == 0x00225020u) {
        uint32_t state = TUROK_MEM32(actor + 0x12Cu);
        if (state && TUROK_MEM32(state) != TUROK_MEM32(g_esp + 4u)) {
            static const uint32_t offsets[4] = { 0x408u, 0x40Cu, 0x1A70u, 0x1A74u };
            int i;
            for (i = 0; i < 4; i++) put_guest_float(actor + offsets[i], guest_float(actor + offsets[i]) * 0.4f);
        }
    }
    sub_00039FF0_gen();
}
extern void sub_001686C0_gen(void);
extern void sub_00139210_gen(void);

void sub_001686C0(void)
{
    uint32_t axis = TUROK_MEM32(g_esp + 4);
    /* The movement controller steers forward and sideways toward its own
     * speed every update. Some Slegs keep running it during Expel and
     * brake their launch before they reach a wall. Suspend those axes
     * for the HumanAI knockback states. World contact and gravity run
     * separately, as does any vertical steering through this helper. */
    if (axis == 0x242720u || axis == 0x242738u) {
        uint32_t state = TUROK_MEM32(g_ecx + 0x12C);
        uint32_t definition = state ? TUROK_MEM32(state + 8) : 0;
        if (definition == 0x21F220u || definition == 0x21F240u) {
            g_esp += 16; /* ret 12 */
            return;
        }
    }
    sub_001686C0_gen();
}

void sub_00139210(void)
{
    uint32_t actor = g_ecx;
    uint32_t state = TUROK_MEM32(actor + 0x12C);
    uint32_t id = state ? TUROK_MEM32(state) : 0;
    /* Each overlapping Expel particle can relaunch the same target.
     * The launch sets vertical speed and lifts its position, so repeated
     * hits stack lift while the first push is still in flight. Leave the
     * launch alone until contact or recovery ends the knockback state. */
    if (id == 63u || id == 64u) {
        g_eax = 0;
        g_esp += 8; /* ret 4 */
        return;
    }
    sub_00139210_gen();
}

/* ── Spider mine (guided device) jump: diagnostic ───────────────
 *
 * Jump while steering the spider (the player's state 0x19,
 * UsingGuidedDevice) goes to the player's jump binding sub_0014E800
 * (this-call, ret 8): with the spider (player +0x1938) not already in its
 * "Jump" state, it sets the spider's vertical speed (physics +0x1C, physics
 * = actor +0xFC) to its jump speed (+0x3B0) and the state to "Jump". There
 * is no test for the ground, on the Xbox or the GameCube (0x801B5340), so
 * presses after the jump animation ends keep it aloft. RECOMP_SPIDER_TRACE=1
 * logs the spider's physics at each press and every 50 ms while it is
 * steered (sub_00145BD0, this-call, ret 4), to find what marks it grounded. */
extern void sub_0014E800_gen(void);
extern void sub_00145BD0_gen(void);
static int spider_trace(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_SPIDER_TRACE") != NULL;
    return on;
}

static void spider_dump(const char *what, uint32_t device)
{
    uint32_t physics = TUROK_MEM32(device + 0xFCu), i;
    char line[1024];
    uint32_t state = TUROK_MEM32(device + 0x12Cu);
    int n = snprintf(line, sizeof line, "[SPIDER] %s t=%lu dev=%08X state %u anim %08X h %.3f jumpv %.3f phys", what,
                     (unsigned long)clock(), device, state ? TUROK_MEM32(state) : 0u, TUROK_MEM32(device + 0x128u),
                     guest_float(device + 0x3Cu), guest_float(device + 0x3B0u));
    for (i = 0; physics && i < 0x80u && n < (int)sizeof line - 12; i += 4)
        n += snprintf(line + n, sizeof line - n, " %08X", TUROK_MEM32(physics + i));
    n += snprintf(line + n, sizeof line - n, " | act");
    for (i = 0; i < 0x30u && n < (int)sizeof line - 12; i += 4)
        n += snprintf(line + n, sizeof line - n, " %08X", TUROK_MEM32(device + 0x10u + i));
    fprintf(stderr, "%s\n", line);
}

void sub_0014E800(void)
{
    uint32_t player = g_ecx, state = TUROK_MEM32(player + 0x12Cu);
    uint32_t device = state && TUROK_MEM32(state) == 0x19u ? TUROK_MEM32(player + 0x1938u) : 0;
    if (device && spider_trace()) spider_dump("press", device);
    sub_0014E800_gen();
    if (device && spider_trace()) {
        uint32_t physics = TUROK_MEM32(device + 0xFCu);
        fprintf(stderr, "[SPIDER] after press: vy %.3f (%s)\n", physics ? guest_float(physics + 0x1Cu) : 0.0f,
                physics && guest_float(physics + 0x1Cu) == guest_float(device + 0x3B0u) ? "jumped" : "refused");
    }
}

void sub_00145BD0(void)
{
    static long last;
    uint32_t device = TUROK_MEM32(g_ecx + 0x1938u);
    if (device && spider_trace() && (long)clock() - last >= 50) {
        last = (long)clock();
        spider_dump("steer", device);
    }
    sub_00145BD0_gen();
}

/* ── Three hot leaf functions, natively ─────────────────────────
 *
 * The game thread is the limit at high frame rates (a 2026-11-23 profile at
 * 480p and 1440p: ~97% busy, its time spread over many functions). These
 * three are leaves -- floating point only, no calls -- and among the
 * busiest: a 4x4 matrix product (sub_00015DE0, 3.6%), a bounding box moved
 * by a matrix (sub_0017F940, 3.1%) and a clamped running total with its
 * extremes (sub_0001A980, 2.3%). As the earlier rewrites: the same
 * operations in the same order with the generated code's rounding of each
 * (mx_round), guest memory read and written in the original order (so an
 * output that is also an input behaves the same), the registers, the x87
 * compare state and any stack slot the original writes left as it leaves
 * them. RECOMP_MATRIX_VERIFY=1 runs the original after each and reports any
 * difference ([LEAF] lines); RECOMP_NATIVE_LEAF=0 (or RECOMP_NATIVE=0) runs
 * the originals. */
extern void sub_00015DE0_gen(void);
extern void sub_0017F940_gen(void);
extern void sub_0001A980_gen(void);
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS uint16_t g_fp_cc;
#define MXV(va) ((double)guest_float(va))
/* The generated code's compare and its status word (recomp_types.h). */
#ifndef RECOMP_FCMP
#define RECOMP_FCMP(a, b)     (((a) != (a) || (b) != (b)) ? 2 : (a) < (b) ? -1 : (a) > (b) ? 1 : 0)
#define RECOMP_FCMP_CC(c) ((uint16_t)((c)==2 ? 0x4500u : (c)<0 ? 0x0100u : (c)>0 ? 0u : 0x4000u))
#endif

static int leaf_native(void)
{
    static int on = -1;
    if (on < 0) on = native_enabled("RECOMP_NATIVE_LEAF");
    return on;
}

static int leaf_verify(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_MATRIX_VERIFY") != NULL;
    return on;
}

/* An x87 compare as the generated code does it: g_fp_cmp, g_fp_cc. */
static uint16_t leaf_compare(double a, double b)
{
    g_fp_cmp = RECOMP_FCMP(a, b);
    g_fp_cc = RECOMP_FCMP_CC(g_fp_cmp);
    return g_fp_cc;
}

/* jp after "test ah, mask": taken when the masked bits are even. */
static int leaf_jp(uint16_t cc, unsigned mask)
{
    unsigned bits = (cc >> 8) & mask, n = 0;
    while (bits) { n += bits & 1u; bits >>= 1; }
    return (n & 1u) == 0;
}

/* sub_00015DE0, cdecl (out, a, b): out = a x b, 4x4, rows of four floats. */
static void matrix44_native(uint32_t out, uint32_t a, uint32_t b)
{
    double t0 = MXV(a + 0x20u);
    double t1 = MXMUL(t0, MXV(b + 0x8u));
    double t2 = MXV(b + 0x4u);
    double t3 = MXMUL(t2, MXV(a + 0x10u));
    double t4 = MXADD(t1, t3);
    double t5 = MXV(a);
    double t6 = MXMUL(t5, MXV(b));
    double t7 = MXADD(t4, t6);
    double t8 = MXV(a + 0x30u);
    double t9 = MXMUL(t8, MXV(b + 0xCu));
    double t10 = MXADD(t7, t9);
    mx_store(out, t10);
    double t11 = MXV(a + 0x34u);
    double t12 = MXMUL(t11, MXV(b + 0xCu));
    double t13 = MXV(a + 0x14u);
    double t14 = MXMUL(t13, MXV(b + 0x4u));
    double t15 = MXADD(t12, t14);
    double t16 = MXV(a + 0x4u);
    double t17 = MXMUL(t16, MXV(b));
    double t18 = MXADD(t15, t17);
    double t19 = MXV(a + 0x24u);
    double t20 = MXMUL(t19, MXV(b + 0x8u));
    double t21 = MXADD(t18, t20);
    mx_store(out + 0x4u, t21);
    double t22 = MXV(a + 0x28u);
    double t23 = MXMUL(t22, MXV(b + 0x8u));
    double t24 = MXV(a + 0x38u);
    double t25 = MXMUL(t24, MXV(b + 0xCu));
    double t26 = MXADD(t23, t25);
    double t27 = MXV(a + 0x18u);
    double t28 = MXMUL(t27, MXV(b + 0x4u));
    double t29 = MXADD(t26, t28);
    double t30 = MXV(a + 0x8u);
    double t31 = MXMUL(t30, MXV(b));
    double t32 = MXADD(t29, t31);
    mx_store(out + 0x8u, t32);
    double t33 = MXV(a + 0x2Cu);
    double t34 = MXMUL(t33, MXV(b + 0x8u));
    double t35 = MXV(a + 0x3Cu);
    double t36 = MXMUL(t35, MXV(b + 0xCu));
    double t37 = MXADD(t34, t36);
    double t38 = MXV(b);
    double t39 = MXMUL(t38, MXV(a + 0xCu));
    double t40 = MXADD(t37, t39);
    double t41 = MXV(b + 0x4u);
    double t42 = MXMUL(t41, MXV(a + 0x1Cu));
    double t43 = MXADD(t40, t42);
    mx_store(out + 0xCu, t43);
    double t44 = MXV(b + 0x1Cu);
    double t45 = MXMUL(t44, MXV(a + 0x30u));
    double t46 = MXV(b + 0x18u);
    double t47 = MXMUL(t46, MXV(a + 0x20u));
    double t48 = MXADD(t45, t47);
    double t49 = MXV(b + 0x10u);
    double t50 = MXMUL(t49, MXV(a));
    double t51 = MXADD(t48, t50);
    double t52 = MXV(a + 0x10u);
    double t53 = MXMUL(t52, MXV(b + 0x14u));
    double t54 = MXADD(t51, t53);
    mx_store(out + 0x10u, t54);
    double t55 = MXV(a + 0x34u);
    double t56 = MXMUL(t55, MXV(b + 0x1Cu));
    double t57 = MXV(b + 0x18u);
    double t58 = MXMUL(t57, MXV(a + 0x24u));
    double t59 = MXADD(t56, t58);
    double t60 = MXV(b + 0x10u);
    double t61 = MXMUL(t60, MXV(a + 0x4u));
    double t62 = MXADD(t59, t61);
    double t63 = MXV(a + 0x14u);
    double t64 = MXMUL(t63, MXV(b + 0x14u));
    double t65 = MXADD(t62, t64);
    mx_store(out + 0x14u, t65);
    double t66 = MXV(a + 0x38u);
    double t67 = MXMUL(t66, MXV(b + 0x1Cu));
    double t68 = MXV(a + 0x28u);
    double t69 = MXMUL(t68, MXV(b + 0x18u));
    double t70 = MXADD(t67, t69);
    double t71 = MXV(b + 0x10u);
    double t72 = MXMUL(t71, MXV(a + 0x8u));
    double t73 = MXADD(t70, t72);
    double t74 = MXV(a + 0x18u);
    double t75 = MXMUL(t74, MXV(b + 0x14u));
    double t76 = MXADD(t73, t75);
    mx_store(out + 0x18u, t76);
    double t77 = MXV(b + 0x18u);
    double t78 = MXMUL(t77, MXV(a + 0x2Cu));
    double t79 = MXV(b + 0x14u);
    double t80 = MXMUL(t79, MXV(a + 0x1Cu));
    double t81 = MXADD(t78, t80);
    double t82 = MXV(b + 0x10u);
    double t83 = MXMUL(t82, MXV(a + 0xCu));
    double t84 = MXADD(t81, t83);
    double t85 = MXV(b + 0x1Cu);
    double t86 = MXMUL(t85, MXV(a + 0x3Cu));
    double t87 = MXADD(t84, t86);
    mx_store(out + 0x1Cu, t87);
    double t88 = MXV(b + 0x2Cu);
    double t89 = MXMUL(t88, MXV(a + 0x30u));
    double t90 = MXV(b + 0x28u);
    double t91 = MXMUL(t90, MXV(a + 0x20u));
    double t92 = MXADD(t89, t91);
    double t93 = MXV(b + 0x20u);
    double t94 = MXMUL(t93, MXV(a));
    double t95 = MXADD(t92, t94);
    double t96 = MXV(a + 0x10u);
    double t97 = MXMUL(t96, MXV(b + 0x24u));
    double t98 = MXADD(t95, t97);
    mx_store(out + 0x20u, t98);
    double t99 = MXV(a + 0x34u);
    double t100 = MXMUL(t99, MXV(b + 0x2Cu));
    double t101 = MXV(b + 0x28u);
    double t102 = MXMUL(t101, MXV(a + 0x24u));
    double t103 = MXADD(t100, t102);
    double t104 = MXV(b + 0x20u);
    double t105 = MXMUL(t104, MXV(a + 0x4u));
    double t106 = MXADD(t103, t105);
    double t107 = MXV(a + 0x14u);
    double t108 = MXMUL(t107, MXV(b + 0x24u));
    double t109 = MXADD(t106, t108);
    mx_store(out + 0x24u, t109);
    double t110 = MXV(b + 0x2Cu);
    double t111 = MXMUL(t110, MXV(a + 0x38u));
    double t112 = MXV(b + 0x28u);
    double t113 = MXMUL(t112, MXV(a + 0x28u));
    double t114 = MXADD(t111, t113);
    double t115 = MXV(b + 0x20u);
    double t116 = MXMUL(t115, MXV(a + 0x8u));
    double t117 = MXADD(t114, t116);
    double t118 = MXV(a + 0x18u);
    double t119 = MXMUL(t118, MXV(b + 0x24u));
    double t120 = MXADD(t117, t119);
    mx_store(out + 0x28u, t120);
    double t121 = MXV(b + 0x28u);
    double t122 = MXMUL(t121, MXV(a + 0x2Cu));
    double t123 = MXV(b + 0x24u);
    double t124 = MXMUL(t123, MXV(a + 0x1Cu));
    double t125 = MXADD(t122, t124);
    double t126 = MXV(b + 0x20u);
    double t127 = MXMUL(t126, MXV(a + 0xCu));
    double t128 = MXADD(t125, t127);
    double t129 = MXV(b + 0x2Cu);
    double t130 = MXMUL(t129, MXV(a + 0x3Cu));
    double t131 = MXADD(t128, t130);
    mx_store(out + 0x2Cu, t131);
    double t132 = MXV(b + 0x3Cu);
    double t133 = MXMUL(t132, MXV(a + 0x30u));
    double t134 = MXV(b + 0x38u);
    double t135 = MXMUL(t134, MXV(a + 0x20u));
    double t136 = MXADD(t133, t135);
    double t137 = MXV(b + 0x30u);
    double t138 = MXMUL(t137, MXV(a));
    double t139 = MXADD(t136, t138);
    double t140 = MXV(a + 0x10u);
    double t141 = MXMUL(t140, MXV(b + 0x34u));
    double t142 = MXADD(t139, t141);
    mx_store(out + 0x30u, t142);
    double t143 = MXV(a + 0x34u);
    double t144 = MXMUL(t143, MXV(b + 0x3Cu));
    double t145 = MXV(b + 0x38u);
    double t146 = MXMUL(t145, MXV(a + 0x24u));
    double t147 = MXADD(t144, t146);
    double t148 = MXV(b + 0x30u);
    double t149 = MXMUL(t148, MXV(a + 0x4u));
    double t150 = MXADD(t147, t149);
    double t151 = MXV(a + 0x14u);
    double t152 = MXMUL(t151, MXV(b + 0x34u));
    double t153 = MXADD(t150, t152);
    mx_store(out + 0x34u, t153);
    double t154 = MXV(b + 0x3Cu);
    double t155 = MXMUL(t154, MXV(a + 0x38u));
    double t156 = MXV(b + 0x38u);
    double t157 = MXMUL(t156, MXV(a + 0x28u));
    double t158 = MXADD(t155, t157);
    double t159 = MXV(b + 0x30u);
    double t160 = MXMUL(t159, MXV(a + 0x8u));
    double t161 = MXADD(t158, t160);
    double t162 = MXV(a + 0x18u);
    double t163 = MXMUL(t162, MXV(b + 0x34u));
    double t164 = MXADD(t161, t163);
    mx_store(out + 0x38u, t164);
    double t165 = MXV(b + 0x38u);
    double t166 = MXMUL(t165, MXV(a + 0x2Cu));
    double t167 = MXV(b + 0x34u);
    double t168 = MXMUL(t167, MXV(a + 0x1Cu));
    double t169 = MXADD(t166, t168);
    double t170 = MXV(b + 0x30u);
    double t171 = MXMUL(t170, MXV(a + 0xCu));
    double t172 = MXADD(t169, t171);
    double t173 = MXV(b + 0x3Cu);
    double t174 = MXMUL(t173, MXV(a + 0x3Cu));
    double t175 = MXADD(t172, t174);
    mx_store(out + 0x3Cu, t175);

}

/* sub_0017F940, cdecl (matrix, low, high): the box low..high (three floats
 * each) moved by the matrix (4x4 rows, translation in the fourth): each
 * axis starts at the translation and gains, per row, the smaller and the
 * larger of the row's term times the old low and high. Its second argument
 * slot holds each low product as a float, as the original's does. */
static void box_transform_native(uint32_t matrix, uint32_t low, uint32_t high, uint32_t slot)
{
    uint32_t old_low[3], old_high[3], shift[3], i, j;
    uint16_t cc = 0;
    for (i = 0; i < 3; i++) old_low[i] = TUROK_MEM32(low + i * 4u);
    for (i = 0; i < 3; i++) old_high[i] = TUROK_MEM32(high + i * 4u);
    for (i = 0; i < 3; i++) shift[i] = TUROK_MEM32(matrix + 0x30u + i * 4u);
    for (i = 0; i < 3; i++) TUROK_MEM32(low + i * 4u) = shift[i];
    for (i = 0; i < 3; i++) shift[i] = TUROK_MEM32(matrix + 0x30u + i * 4u);
    for (i = 0; i < 3; i++) TUROK_MEM32(high + i * 4u) = shift[i];
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            float lo_f, hi_f;
            double term, product_low, product_high;
            memcpy(&lo_f, &old_low[j], 4);
            memcpy(&hi_f, &old_high[j], 4);
            term = MXV(matrix + j * 16u + i * 4u);
            product_low = MXMUL((double)lo_f, term);
            mx_store(slot, product_low);
            product_high = MXMUL(term, (double)hi_f);
            cc = leaf_compare(MXV(slot), product_high);
            if (!leaf_jp(cc, 5)) {                 /* low product the smaller */
                mx_store(low + i * 4u, MXADD(MXV(slot), MXV(low + i * 4u)));
                mx_store(high + i * 4u, MXADD(product_high, MXV(high + i * 4u)));
            } else {
                mx_store(low + i * 4u, MXADD(product_high, MXV(low + i * 4u)));
                mx_store(high + i * 4u, MXADD(MXV(slot), MXV(high + i * 4u)));
            }
        }
    /* As it leaves them: fnstsw's word in ax (one value on the stack then). */
    g_eax = (matrix & 0xFFFF0000u) | ((uint32_t)((g_fp_top + 7) & 7) << 11) | cc;
    g_ecx = high + 12u;
    g_edx = low - high;
}

/* sub_0001A980, cdecl (values, amount): values[0] += amount, held within
 * the two limits at 0x1FAAF8 and 0x1FAAF4 (set to -1e8 or 1e8 past them);
 * values[1] keeps the least and values[2] the greatest. */
static void running_total_native(uint32_t values, uint32_t amount_slot)
{
    double total = MXADD(MXV(amount_slot), MXV(values));
    uint16_t cc = leaf_compare(total, MXV(0x1FAAF8u));
    uint32_t eax = g_eax, edx = g_edx;
    mx_store(values, total);
    if (!leaf_jp(cc, 5)) {
        TUROK_MEM32(values) = 0xCCBEBC20u;
    } else {
        cc = leaf_compare(total, MXV(0x1FAAF4u));
        if (!((cc >> 8) & 0x41u)) TUROK_MEM32(values) = 0x4CBEBC20u;
    }
    cc = leaf_compare(MXV(values), MXV(values + 4u));
    if (!leaf_jp(cc, 5)) {
        eax = TUROK_MEM32(values);
        TUROK_MEM32(values + 4u) = eax;
    }
    cc = leaf_compare(MXV(values), MXV(values + 8u));
    if (!((cc >> 8) & 0x41u)) {
        edx = TUROK_MEM32(values);
        TUROK_MEM32(values + 8u) = edx;
    }
    g_eax = (eax & 0xFFFF0000u) | ((uint32_t)(g_fp_top & 7) << 11) | cc;
    g_ecx = values;
    g_edx = edx;
}

/* Runs `native`, then the original on the same memory, and reports any
 * difference in the given ranges or in eax/ecx/edx. */
typedef struct LeafRange { uint32_t va, words; } LeafRange;
static void leaf_check(const char *name, void (*native)(void *), void *context, void (*original)(void),
                       const LeafRange *ranges, int count)
{
    static unsigned checks, mismatches;
    uint32_t saved[64], after_native[64], after_original[64], regs_native[3], regs_original[3];
    uint32_t entry[3] = {g_eax, g_ecx, g_edx}, n = 0, i;
    int r, top = g_fp_top;
    for (r = 0; r < count; r++)
        for (i = 0; i < ranges[r].words && n < 64; i++) saved[n++] = TUROK_MEM32(ranges[r].va + i * 4u);
    native(context);
    regs_native[0] = g_eax; regs_native[1] = g_ecx; regs_native[2] = g_edx;
    for (n = 0, r = 0; r < count; r++)
        for (i = 0; i < ranges[r].words && n < 64; i++) after_native[n++] = TUROK_MEM32(ranges[r].va + i * 4u);
    for (n = 0, r = 0; r < count; r++)
        for (i = 0; i < ranges[r].words && n < 64; i++) TUROK_MEM32(ranges[r].va + i * 4u) = saved[n++];
    g_eax = entry[0]; g_ecx = entry[1]; g_edx = entry[2]; g_fp_top = top;
    original();   /* pops its return address */
    regs_original[0] = g_eax; regs_original[1] = g_ecx; regs_original[2] = g_edx;
    for (n = 0, r = 0; r < count; r++)
        for (i = 0; i < ranges[r].words && n < 64; i++) after_original[n++] = TUROK_MEM32(ranges[r].va + i * 4u);
    checks++;
    if ((memcmp(after_native, after_original, n * 4u) || memcmp(regs_native, regs_original, sizeof regs_native)) &&
            mismatches++ < 24) {
        for (i = 0; i < n && after_native[i] == after_original[i]; i++) {}
        fprintf(stderr, "[LEAF] %s differs (check %u, cw %04X): word %u %08X vs %08X; eax %08X/%08X ecx %08X/%08X "
                "edx %08X/%08X\n", name, checks, (unsigned)g_fp_control_word, i, i < n ? after_native[i] : 0,
                i < n ? after_original[i] : 0, regs_native[0], regs_original[0], regs_native[1], regs_original[1],
                regs_native[2], regs_original[2]);
    }
    if ((checks & 0x3FFFFu) == 0) fprintf(stderr, "[LEAF] %u checks, %u differ\n", checks, mismatches);
}

static void matrix44_context(void *c) { uint32_t *a = c; matrix44_native(a[0], a[1], a[2]); g_eax = a[1]; g_ecx = a[2]; g_edx = a[0]; }
static void box_context(void *c) { uint32_t *a = c; box_transform_native(a[0], a[1], a[2], a[3]); }
static void total_context(void *c) { uint32_t *a = c; running_total_native(a[0], a[1]); }

int sub_00015DE0_lifted(void);
void sub_00015DE0(void)
{
    if (sub_00015DE0_lifted()) return;   /* the lifted body, when there is one */
    uint32_t args[3] = {TUROK_MEM32(g_esp + 4u), TUROK_MEM32(g_esp + 8u), TUROK_MEM32(g_esp + 12u)};
    if (!leaf_native()) { sub_00015DE0_gen(); return; }
    if (leaf_verify()) {
        LeafRange ranges[3] = {{args[0], 16}, {args[1], 16}, {args[2], 16}};
        leaf_check("matrix44", matrix44_context, args, sub_00015DE0_gen, ranges, 3);
        return;
    }
    matrix44_context(args);
    g_esp += 4u;   /* ret */
}

int sub_0017F940_lifted(void);
void sub_0017F940(void)
{
    if (sub_0017F940_lifted()) return;   /* the lifted body, when there is one */
    uint32_t args[4] = {TUROK_MEM32(g_esp + 4u), TUROK_MEM32(g_esp + 8u), TUROK_MEM32(g_esp + 12u), g_esp + 8u};
    if (!leaf_native()) { sub_0017F940_gen(); return; }
    if (leaf_verify()) {
        LeafRange ranges[4] = {{args[1], 3}, {args[2], 3}, {args[3], 1}, {args[0], 16}};
        leaf_check("box", box_context, args, sub_0017F940_gen, ranges, 4);
        return;
    }
    box_context(args);
    g_esp += 4u;   /* ret */
}

int sub_0001A980_lifted(void);
void sub_0001A980(void)
{
    if (sub_0001A980_lifted()) return;   /* the lifted body, when there is one */
    uint32_t args[2] = {TUROK_MEM32(g_esp + 4u), g_esp + 8u};
    if (!leaf_native()) { sub_0001A980_gen(); return; }
    if (leaf_verify()) {
        LeafRange ranges[1] = {{args[0], 3}};
        leaf_check("total", total_context, args, sub_0001A980_gen, ranges, 1);
        return;
    }
    total_context(args);
    g_esp += 4u;   /* ret */
}

/* ── Lifted leaves ───────────────────────────────────────────────
 *
 * Four more of the game thread's busiest functions, leaves with branches
 * and loops: sub_00070930 (1.6%), sub_00022ED0 (1.6%), sub_0002FD10 (1.5%)
 * and sub_00070DF0 (0.9%). Each is lifted again from the game's own
 * instructions by tools/x86_lift.py into src/native/sub_XXXXXXXX.c, with
 * registers and x87 slots in locals (as object lighting, sub_00036D00):
 *
 *   py -3 tools/x86_lift.py <default.xbe> 0x00070930 0x00070AAD sub_00070930_native src/native/sub_00070930.c
 *   py -3 tools/x86_lift.py <default.xbe> 0x00022ED0 0x000230D3 sub_00022ED0_native src/native/sub_00022ED0.c
 *   py -3 tools/x86_lift.py <default.xbe> 0x0002FD10 0x0002FDF4 sub_0002FD10_native src/native/sub_0002FD10.c
 *   py -3 tools/x86_lift.py <default.xbe> 0x00070DF0 0x000712A7 sub_00070DF0_native src/native/sub_00070DF0.c
 *
 * and thirteen more, about a third of the game thread at the jungle start
 * (culling sub_000200C0..sub_000223F0, animation and skinning sub_000725F0 /
 * sub_0008DD10 / sub_0008E1D0, actor and mesh work), each the same way; the
 * last arguments name callees that return a float (one more on the x87
 * stack):
 *
 *   0x0008E1D0 0x0008E85E 355f0=1 cf0c0=1   0x00020B50 0x00020D2B
 *   0x000223F0 0x00022586                   0x000725F0 0x00072AB1
 *   0x000213E0 0x000214A3                   0x000ED210 0x000ED628
 *   0x000200C0 0x000201AC                   0x00022AE0 0x00022BBC cf3b0=1
 *   0x00039220 0x000392DD                   0x000E7F70 0x000E807B
 *   0x00072FA0 0x000730A8                   0x00030280 0x0003038A
 *   0x0008DD10 0x0008DFAA cf0c0=1
 *
 * and seven with switch tables and indirect calls (made as the generated
 * code makes them): 0x00032230 0x00032EE6, 0x000EEB10 0x000EEE31,
 * 0x0003CFB0 0x0003D2A9, 0x000E8260 0x000E85ED, 0x000EE850 0x000EEB05,
 * 0x000E80F0 0x000E8251, 0x00082F70 0x00083299.
 *
 * and three more: 0x00076760 0x00076D5A (scalar SSE: movss, rsqrtps, rcpps,
 * on the shared XMM registers with the generated code's exact helpers),
 * 0x0003C9E0 0x0003CB05 3ca0a=1 3ca97=1 88f00=1 cf0c0=1 (returns a float in
 * st(0); its indirect calls at 0x3CA0A and 0x3CA97 return one too), and
 * 0x000448D0 0x00044B99 387b0=1 3c9e0=1. sub_0003C9E0's lifted body runs
 * under the draw distance override (sub_0003C9E0_lifted).
 *
 * Like src/recomp/gen those files are made from the XBE and not kept in the
 * repository; without one, that function's generated code runs.
 * RECOMP_NATIVE_LIFTED=0 (or RECOMP_NATIVE=0) runs the generated code for
 * all of them. RECOMP_LIFT_VERIFY=<n>: every nth call of each runs both from
 * the same guest RAM and registers and reports any word or register that
 * differs ([LIFTED] lines; another thread writing RAM in that moment shows
 * up too). */
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp;
extern RECOMP_TLS double g_fp_stack[8];
#define LIFTED_RAM (64u << 20)

typedef struct LiftedRegs {
    uint32_t r[8];
    uint32_t ebp, seh_ebp;   /* the published frame: not compared, but each run starts from the same one */
    int top;
    double stack[8];
    uint16_t cc;
    RecompXmm xmm0;          /* the one XMM register a lifted function uses (sub_00076760) */
} LiftedRegs;

static void lifted_save(LiftedRegs *s)
{
    s->r[0] = g_eax; s->r[1] = g_ecx; s->r[2] = g_edx; s->r[3] = g_ebx;
    s->r[4] = g_esi; s->r[5] = g_edi; s->r[6] = g_esp; s->r[7] = 0;
    s->ebp = g_ebp; s->seh_ebp = g_seh_ebp;
    s->top = g_fp_top & 7; memcpy(s->stack, g_fp_stack, sizeof s->stack); s->cc = g_fp_cc;
    s->xmm0 = g_xmm0;
}

static void lifted_load(const LiftedRegs *s)
{
    g_eax = s->r[0]; g_ecx = s->r[1]; g_edx = s->r[2]; g_ebx = s->r[3];
    g_esi = s->r[4]; g_edi = s->r[5]; g_esp = s->r[6];
    g_ebp = s->ebp; g_seh_ebp = s->seh_ebp;
    g_fp_top = s->top; memcpy(g_fp_stack, s->stack, sizeof s->stack); g_fp_cc = s->cc;
    g_xmm0 = s->xmm0;
}

static void lifted_run(const char *name, void (*native)(void), void (*original)(void), unsigned *calls)
{
    static int on = -1, every = -1;
    static uint8_t *before, *after;
    uint8_t *ram = (uint8_t *)(uintptr_t)g_xbox_mem_offset;
    LiftedRegs entry, native_regs, original_regs;
    uint32_t page, differ = 0;
    if (on < 0) on = native_enabled("RECOMP_NATIVE_LIFTED");
    if (!on || !native) { original(); return; }
    if (every < 0) { const char *v = getenv("RECOMP_LIFT_VERIFY"); every = v ? atoi(v) : 0; }
    if (every <= 0 || ++*calls % (unsigned)every) { native(); return; }
    {   /* RECOMP_LIFT_VERIFY_SELF=1: the generated code against itself, the background's differences */
        static int self = -1;
        if (self < 0) self = getenv("RECOMP_LIFT_VERIFY_SELF") && *getenv("RECOMP_LIFT_VERIFY_SELF") == '1';
        if (self) native = original;
    }
    if (!before) {
        before = (uint8_t *)malloc(LIFTED_RAM);
        after = (uint8_t *)malloc(LIFTED_RAM);
        if (!before || !after) { every = 0; native(); return; }
    }
    lifted_save(&entry);
    memcpy(before, ram, LIFTED_RAM);
    native();
    lifted_save(&native_regs);
    memcpy(after, ram, LIFTED_RAM);
    for (page = 0; page < LIFTED_RAM; page += 4096u)   /* back as it was */
        if (memcmp(ram + page, before + page, 4096u)) memcpy(ram + page, before + page, 4096u);
    lifted_load(&entry);
    original();
    lifted_save(&original_regs);
    for (page = 0; page < LIFTED_RAM; page += 4096u) {
        uint32_t i;
        if (!memcmp(ram + page, after + page, 4096u)) continue;
        for (i = 0; i < 4096u; i += 4) {
            uint32_t a, b;
            memcpy(&a, after + page + i, 4); memcpy(&b, ram + page + i, 4);
            if (a == b) continue;
            /* other threads' stacks, the D3D device's counters, the frame clocks (as lighting_verify) */
            if ((page + i >= 0x007B0000u && page + i < 0x00800000u) ||
                (page + i >= 0x001A4400u && page + i < 0x001A4C00u) || page + i == 0x00257DF0u || page + i == 0x00740020u ||
                (page + i >= 0x01081E00u && page + i < 0x01082900u) || page + i < 0x4000u) continue;
            if (differ++ < 4)
                fprintf(stderr, "[LIFTED] %s: %08X lifted %08X, generated %08X\n", name, page + i, a, b);
        }
    }
    native_regs.top &= 7; original_regs.top &= 7;
    if (memcmp(native_regs.r, original_regs.r, sizeof native_regs.r) || native_regs.top != original_regs.top ||
            native_regs.cc != original_regs.cc || memcmp(&native_regs.xmm0, &original_regs.xmm0, sizeof native_regs.xmm0) ||
            (native_regs.top != entry.top && memcmp(&native_regs.stack[native_regs.top],
                                                    &original_regs.stack[original_regs.top], sizeof(double)))) {
        differ++;
        fprintf(stderr, "[LIFTED] %s registers: eax %08X/%08X ecx %08X/%08X edx %08X/%08X esp %08X/%08X top %d/%d\n",
                name, native_regs.r[0], original_regs.r[0], native_regs.r[1], original_regs.r[1], native_regs.r[2],
                original_regs.r[2], native_regs.r[6], original_regs.r[6], native_regs.top, original_regs.top);
    }
    {
        static unsigned checks, mismatches;
        checks++;
        if (differ) mismatches++;
        if ((checks & 0xFFu) == 0)
            fprintf(stderr, "[LIFTED] %u checks, %u with a difference\n", checks, mismatches);
    }
}

#define LIFTED_LEAF(addr)                                                                    \
    extern void sub_##addr##_gen(void);                                                      \
    void sub_##addr(void)                                                                    \
    {                                                                                        \
        static unsigned calls;                                                               \
        lifted_run("sub_" #addr, LIFTED_NATIVE_##addr, sub_##addr##_gen, &calls);            \
    }

#ifdef RECOMP_HAVE_LIFTED_00070930
extern void sub_00070930_native(void);
#define LIFTED_NATIVE_00070930 sub_00070930_native
#else
#define LIFTED_NATIVE_00070930 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00022ED0
extern void sub_00022ED0_native(void);
#define LIFTED_NATIVE_00022ED0 sub_00022ED0_native
#else
#define LIFTED_NATIVE_00022ED0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_0002FD10
extern void sub_0002FD10_native(void);
#define LIFTED_NATIVE_0002FD10 sub_0002FD10_native
#else
#define LIFTED_NATIVE_0002FD10 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00070DF0
extern void sub_00070DF0_native(void);
#define LIFTED_NATIVE_00070DF0 sub_00070DF0_native
#else
#define LIFTED_NATIVE_00070DF0 NULL
#endif
LIFTED_LEAF(00070930)
LIFTED_LEAF(00022ED0)
LIFTED_LEAF(0002FD10)
LIFTED_LEAF(00070DF0)
#ifdef RECOMP_HAVE_LIFTED_0008E1D0
extern void sub_0008E1D0_native(void);
#define LIFTED_NATIVE_0008E1D0 sub_0008E1D0_native
#else
#define LIFTED_NATIVE_0008E1D0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00020B50
extern void sub_00020B50_native(void);
#define LIFTED_NATIVE_00020B50 sub_00020B50_native
#else
#define LIFTED_NATIVE_00020B50 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000223F0
extern void sub_000223F0_native(void);
#define LIFTED_NATIVE_000223F0 sub_000223F0_native
#else
#define LIFTED_NATIVE_000223F0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000725F0
extern void sub_000725F0_native(void);
#define LIFTED_NATIVE_000725F0 sub_000725F0_native
#else
#define LIFTED_NATIVE_000725F0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000213E0
extern void sub_000213E0_native(void);
#define LIFTED_NATIVE_000213E0 sub_000213E0_native
#else
#define LIFTED_NATIVE_000213E0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000ED210
extern void sub_000ED210_native(void);
#define LIFTED_NATIVE_000ED210 sub_000ED210_native
#else
#define LIFTED_NATIVE_000ED210 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000200C0
extern void sub_000200C0_native(void);
#define LIFTED_NATIVE_000200C0 sub_000200C0_native
#else
#define LIFTED_NATIVE_000200C0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00022AE0
extern void sub_00022AE0_native(void);
#define LIFTED_NATIVE_00022AE0 sub_00022AE0_native
#else
#define LIFTED_NATIVE_00022AE0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00039220
extern void sub_00039220_native(void);
#define LIFTED_NATIVE_00039220 sub_00039220_native
#else
#define LIFTED_NATIVE_00039220 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000E7F70
extern void sub_000E7F70_native(void);
#define LIFTED_NATIVE_000E7F70 sub_000E7F70_native
#else
#define LIFTED_NATIVE_000E7F70 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00072FA0
extern void sub_00072FA0_native(void);
#define LIFTED_NATIVE_00072FA0 sub_00072FA0_native
#else
#define LIFTED_NATIVE_00072FA0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00030280
extern void sub_00030280_native(void);
#define LIFTED_NATIVE_00030280 sub_00030280_native
#else
#define LIFTED_NATIVE_00030280 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_0008DD10
extern void sub_0008DD10_native(void);
#define LIFTED_NATIVE_0008DD10 sub_0008DD10_native
#else
#define LIFTED_NATIVE_0008DD10 NULL
#endif
LIFTED_LEAF(0008E1D0)
LIFTED_LEAF(00020B50)
LIFTED_LEAF(000223F0)
LIFTED_LEAF(000725F0)
LIFTED_LEAF(000213E0)
LIFTED_LEAF(000ED210)
LIFTED_LEAF(000200C0)
LIFTED_LEAF(00022AE0)
LIFTED_LEAF(00039220)
LIFTED_LEAF(000E7F70)
LIFTED_LEAF(00072FA0)
LIFTED_LEAF(00030280)
LIFTED_LEAF(0008DD10)
#ifdef RECOMP_HAVE_LIFTED_00032230
extern void sub_00032230_native(void);
#define LIFTED_NATIVE_00032230 sub_00032230_native
#else
#define LIFTED_NATIVE_00032230 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000EEB10
extern void sub_000EEB10_native(void);
#define LIFTED_NATIVE_000EEB10 sub_000EEB10_native
#else
#define LIFTED_NATIVE_000EEB10 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_0003CFB0
extern void sub_0003CFB0_native(void);
#define LIFTED_NATIVE_0003CFB0 sub_0003CFB0_native
#else
#define LIFTED_NATIVE_0003CFB0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000E8260
extern void sub_000E8260_native(void);
#define LIFTED_NATIVE_000E8260 sub_000E8260_native
#else
#define LIFTED_NATIVE_000E8260 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000EE850
extern void sub_000EE850_native(void);
#define LIFTED_NATIVE_000EE850 sub_000EE850_native
#else
#define LIFTED_NATIVE_000EE850 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000E80F0
extern void sub_000E80F0_native(void);
#define LIFTED_NATIVE_000E80F0 sub_000E80F0_native
#else
#define LIFTED_NATIVE_000E80F0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00076760
extern void sub_00076760_native(void);
#define LIFTED_NATIVE_00076760 sub_00076760_native
#else
#define LIFTED_NATIVE_00076760 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000448D0
extern void sub_000448D0_native(void);
#define LIFTED_NATIVE_000448D0 sub_000448D0_native
#else
#define LIFTED_NATIVE_000448D0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_0003C9E0
extern void sub_0003C9E0_native(void);
#define LIFTED_NATIVE_0003C9E0 sub_0003C9E0_native
#else
#define LIFTED_NATIVE_0003C9E0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00082F70
extern void sub_00082F70_native(void);
#define LIFTED_NATIVE_00082F70 sub_00082F70_native
#else
#define LIFTED_NATIVE_00082F70 NULL
#endif
LIFTED_LEAF(00032230)
LIFTED_LEAF(000EEB10)
LIFTED_LEAF(0003CFB0)
LIFTED_LEAF(000E8260)
LIFTED_LEAF(000EE850)
LIFTED_LEAF(000E80F0)
LIFTED_LEAF(00082F70)
LIFTED_LEAF(00076760)
LIFTED_LEAF(000448D0)

/* A fourth round, the next thirty by time on the game thread (see "Lifted
 * leaves" above; start, end and the float-returning callees as before):
 *   0x00047470 0x000485D7 cf0c0=1   0x0001FFB0 0x0002002A
 *   0x000ED070 0x000ED15D            0x000B5EE0 0x000B62B9
 *   0x00044460 0x00044577            0x0001A7C0 0x0001A812
 *   0x000382B0 0x0003847C            0x00016000 0x00016030
 *   0x00070D90 0x00070DED            0x000D0100 0x000D0235 cf6b0=1
 *   0x00038580 0x000385D1            0x000EEE60 0x000EEED2
 *   0x0001BE80 0x0001BFAA            0x0002FCB0 0x0002FD0A
 *   0x00069940 0x00069A43            0x00035330 0x00035375
 *   0x00022D30 0x00022D55            0x000671F0 0x00067354
 *   0x000712B0 0x00071759            0x0003D2C0 0x0003D3EF
 *   0x0003CF30 0x0003CFA7            0x00051940 0x000519AE
 *   0x000387B0 0x000387EB cf0c0=1   0x00088F00 0x00088F32
 *   0x000F3410 0x000F356D
 * and five that have a wrapper here, which calls sub_X_lifted:
 *   0x00021A90 0x000223E8 (the in-view test's fallback), 0x0003B110
 *   0x0003B1A7 (update distance), and the three hand-written leaves above,
 *   0x00015DE0 0x00015FF4, 0x0017F940 0x0017FA9B, 0x0001A980 0x0001A9D9,
 *   whose lifted bodies are faster (the hand-written ones still run with
 *   RECOMP_NATIVE_LIFTED=0). sub_00089580 stays generated: it returns with
 *   four values on the x87 stack. */
#ifdef RECOMP_HAVE_LIFTED_00047470
extern void sub_00047470_native(void);
#define LIFTED_NATIVE_00047470 sub_00047470_native
#else
#define LIFTED_NATIVE_00047470 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_0001FFB0
extern void sub_0001FFB0_native(void);
#define LIFTED_NATIVE_0001FFB0 sub_0001FFB0_native
#else
#define LIFTED_NATIVE_0001FFB0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000ED070
extern void sub_000ED070_native(void);
#define LIFTED_NATIVE_000ED070 sub_000ED070_native
#else
#define LIFTED_NATIVE_000ED070 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000B5EE0
extern void sub_000B5EE0_native(void);
#define LIFTED_NATIVE_000B5EE0 sub_000B5EE0_native
#else
#define LIFTED_NATIVE_000B5EE0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00044460
extern void sub_00044460_native(void);
#define LIFTED_NATIVE_00044460 sub_00044460_native
#else
#define LIFTED_NATIVE_00044460 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_0001A7C0
extern void sub_0001A7C0_native(void);
#define LIFTED_NATIVE_0001A7C0 sub_0001A7C0_native
#else
#define LIFTED_NATIVE_0001A7C0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000382B0
extern void sub_000382B0_native(void);
#define LIFTED_NATIVE_000382B0 sub_000382B0_native
#else
#define LIFTED_NATIVE_000382B0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00016000
extern void sub_00016000_native(void);
#define LIFTED_NATIVE_00016000 sub_00016000_native
#else
#define LIFTED_NATIVE_00016000 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00070D90
extern void sub_00070D90_native(void);
#define LIFTED_NATIVE_00070D90 sub_00070D90_native
#else
#define LIFTED_NATIVE_00070D90 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000D0100
extern void sub_000D0100_native(void);
#define LIFTED_NATIVE_000D0100 sub_000D0100_native
#else
#define LIFTED_NATIVE_000D0100 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00038580
extern void sub_00038580_native(void);
#define LIFTED_NATIVE_00038580 sub_00038580_native
#else
#define LIFTED_NATIVE_00038580 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000EEE60
extern void sub_000EEE60_native(void);
#define LIFTED_NATIVE_000EEE60 sub_000EEE60_native
#else
#define LIFTED_NATIVE_000EEE60 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_0001BE80
extern void sub_0001BE80_native(void);
#define LIFTED_NATIVE_0001BE80 sub_0001BE80_native
#else
#define LIFTED_NATIVE_0001BE80 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_0002FCB0
extern void sub_0002FCB0_native(void);
#define LIFTED_NATIVE_0002FCB0 sub_0002FCB0_native
#else
#define LIFTED_NATIVE_0002FCB0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00069940
extern void sub_00069940_native(void);
#define LIFTED_NATIVE_00069940 sub_00069940_native
#else
#define LIFTED_NATIVE_00069940 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00035330
extern void sub_00035330_native(void);
#define LIFTED_NATIVE_00035330 sub_00035330_native
#else
#define LIFTED_NATIVE_00035330 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00022D30
extern void sub_00022D30_native(void);
#define LIFTED_NATIVE_00022D30 sub_00022D30_native
#else
#define LIFTED_NATIVE_00022D30 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000671F0
extern void sub_000671F0_native(void);
#define LIFTED_NATIVE_000671F0 sub_000671F0_native
#else
#define LIFTED_NATIVE_000671F0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000712B0
extern void sub_000712B0_native(void);
#define LIFTED_NATIVE_000712B0 sub_000712B0_native
#else
#define LIFTED_NATIVE_000712B0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_0003D2C0
extern void sub_0003D2C0_native(void);
#define LIFTED_NATIVE_0003D2C0 sub_0003D2C0_native
#else
#define LIFTED_NATIVE_0003D2C0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_0003CF30
extern void sub_0003CF30_native(void);
#define LIFTED_NATIVE_0003CF30 sub_0003CF30_native
#else
#define LIFTED_NATIVE_0003CF30 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00051940
extern void sub_00051940_native(void);
#define LIFTED_NATIVE_00051940 sub_00051940_native
#else
#define LIFTED_NATIVE_00051940 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000387B0
extern void sub_000387B0_native(void);
#define LIFTED_NATIVE_000387B0 sub_000387B0_native
#else
#define LIFTED_NATIVE_000387B0 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00088F00
extern void sub_00088F00_native(void);
#define LIFTED_NATIVE_00088F00 sub_00088F00_native
#else
#define LIFTED_NATIVE_00088F00 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_000F3410
extern void sub_000F3410_native(void);
#define LIFTED_NATIVE_000F3410 sub_000F3410_native
#else
#define LIFTED_NATIVE_000F3410 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00021A90
extern void sub_00021A90_native(void);
#define LIFTED_NATIVE_00021A90 sub_00021A90_native
#else
#define LIFTED_NATIVE_00021A90 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_0003B110
extern void sub_0003B110_native(void);
#define LIFTED_NATIVE_0003B110 sub_0003B110_native
#else
#define LIFTED_NATIVE_0003B110 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_0017F940
extern void sub_0017F940_native(void);
#define LIFTED_NATIVE_0017F940 sub_0017F940_native
#else
#define LIFTED_NATIVE_0017F940 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_0001A980
extern void sub_0001A980_native(void);
#define LIFTED_NATIVE_0001A980 sub_0001A980_native
#else
#define LIFTED_NATIVE_0001A980 NULL
#endif
#ifdef RECOMP_HAVE_LIFTED_00015DE0
extern void sub_00015DE0_native(void);
#define LIFTED_NATIVE_00015DE0 sub_00015DE0_native
#else
#define LIFTED_NATIVE_00015DE0 NULL
#endif
LIFTED_LEAF(00047470)
LIFTED_LEAF(0001FFB0)
LIFTED_LEAF(000ED070)
LIFTED_LEAF(000B5EE0)
LIFTED_LEAF(00044460)
LIFTED_LEAF(0001A7C0)
LIFTED_LEAF(000382B0)
LIFTED_LEAF(00016000)
LIFTED_LEAF(00070D90)
LIFTED_LEAF(000D0100)
LIFTED_LEAF(00038580)
LIFTED_LEAF(000EEE60)
LIFTED_LEAF(0001BE80)
LIFTED_LEAF(0002FCB0)
LIFTED_LEAF(00069940)
LIFTED_LEAF(00035330)
LIFTED_LEAF(00022D30)
LIFTED_LEAF(000671F0)
LIFTED_LEAF(000712B0)
LIFTED_LEAF(0003D2C0)
LIFTED_LEAF(0003CF30)
LIFTED_LEAF(00051940)
LIFTED_LEAF(000387B0)
LIFTED_LEAF(00088F00)
LIFTED_LEAF(000F3410)

/* Functions with a hand-written wrapper: the wrapper calls sub_X_lifted,
 * which runs the lifted body and returns 1, or returns 0 when there is
 * none or RECOMP_NATIVE_LIFTED=0 (the wrapper then does as before). */
static int lifted_enabled(void)
{
    static int on = -1;
    if (on < 0) on = native_enabled("RECOMP_NATIVE_LIFTED");
    return on;
}

#define LIFTED_WRAPPED(addr)                                                                 \
    int sub_##addr##_lifted(void)                                                            \
    {                                                                                        \
        static unsigned calls;                                                               \
        void (*native)(void) = LIFTED_NATIVE_##addr;                                         \
        if (!native || !lifted_enabled()) return 0;                                          \
        lifted_run("sub_" #addr, native, sub_##addr##_gen, &calls);                          \
        return 1;                                                                            \
    }
LIFTED_WRAPPED(00021A90)
LIFTED_WRAPPED(0003B110)
LIFTED_WRAPPED(0017F940)
LIFTED_WRAPPED(0001A980)
LIFTED_WRAPPED(00015DE0)

/* Not a LIFTED_LEAF: sub_0003C9E0 itself is the draw distance override. */
extern void sub_0003C9E0_gen(void);
void sub_0003C9E0_lifted(void)
{
    static unsigned calls;
    lifted_run("sub_0003C9E0", LIFTED_NATIVE_0003C9E0, sub_0003C9E0_gen, &calls);
}
