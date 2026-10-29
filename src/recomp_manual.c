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
#include <intrin.h>   /* _mm_mfence */

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

extern __declspec(thread) uint32_t g_eax;   /* thread-local, like the whole register file */
extern ptrdiff_t g_xbox_mem_offset;

/* The D3D fence-wait wrapper and fence-rescue thread from main are not
 * here: on the aknavj toolkit, native fences (RECOMP_NV2A_NATIVE_FENCES)
 * write the semaphore through the GPU's own DMA context instead. */

#define TUROK_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define TUROK_D3D_DEVICE_PTR   0x001A1AD0u

/* Thread-local like the rest of the register file; see main.c. */
extern __declspec(thread) uint32_t g_esp;

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
 * visible and the original function reads it. When the executor reaches the
 * GET_REPORT it writes the same verdict.
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
extern __declspec(thread) uint32_t g_ecx;
extern void sub_001B23DD_gen(void);

void sub_001B23DD(void)
{
    uint32_t handle = TUROK_MEM32(g_esp + 4);

    if (handle < 0x100u && TUROK_MEM32(g_ecx + 0x330u + handle * 4u) == 0) {
        g_esp += 8;   /* ret 4 */
        return;
    }
    sub_001B23DD_gen();
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
 * RECOMP_SHADOW_DISTANCE (default 64) and RECOMP_SHADOW_MAP (default 2048,
 * a power of two from 512 to 4096) replace them; at the defaults shadows
 * reach four times as far (see sub_0017DAF0 below for the area around the
 * player). Set 16 and 512 (and RECOMP_SHADOW_AROUND=0) for the original.
 */
extern void sub_0017D5C0_gen(void);

void sub_0017D5C0(void)
{
    uint32_t self = g_ecx;
    const char *distance_text = getenv("RECOMP_SHADOW_DISTANCE");
    const char *map_text = getenv("RECOMP_SHADOW_MAP");
    float distance = distance_text ? (float)atof(distance_text) : 64.0f;
    uint32_t map = map_text ? (uint32_t)strtoul(map_text, NULL, 0) : 2048u;

    sub_0017D5C0_gen();
    if (distance > 0.0f && distance < 10000.0f)
        memcpy((void *)((uintptr_t)(self + 0x40Cu) + g_xbox_mem_offset), &distance, 4);
    if (map >= 512u && map <= 4096u && !(map & (map - 1u))) {
        TUROK_MEM32(self + 0x400u) = map;
        TUROK_MEM32(self + 0x404u) = map;
    }
}

/* ── Shadow area around the player (sub_0017DAF0) ──────────────
 *
 * The camera-fitted light camera (light camera at shadow manager +0x10, eye
 * +0x204, target +0x210) looks at a point half the shadow distance ahead
 * of the view and is given just the angle that reaches the player
 * (sub_0001FD20 sets it, radians at camera +0xEC): the shadowed area is a
 * disc with the player on its rim. Anything beside or behind the player --
 * and, in widescreen, near the sides of the view -- is off the map and
 * casts nothing. The angle is widened so the disc also reaches
 * RECOMP_SHADOW_AROUND units (default 12) past the player. */
extern void sub_0017DAF0_gen(void);
extern void sub_0001FD20_gen(void);

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
    sub_0017DAF0_gen();
    s_shadow_fit = 0;
}

void sub_0001FD20(void)
{
    static float around = -1.0f;
    uint32_t self = s_shadow_fit;

    if (around < 0) {
        const char *text = getenv("RECOMP_SHADOW_AROUND");
        around = text ? (float)atof(text) : 12.0f;
        if (!(around >= 0 && around < 1000.0f)) around = 0;
    }
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
 * RECOMP_SHADOW_OFFSCREEN=0 restores that; otherwise every model the
 * manager offers casts its shadow, and the light camera clips the rest. */
extern void sub_000F21B0_gen(void);

void sub_000F21B0(void)
{
    static int offscreen = -1;
    if (offscreen < 0) {
        const char *text = getenv("RECOMP_SHADOW_OFFSCREEN");
        offscreen = !text || atoi(text) != 0;
    }
    sub_000F21B0_gen();
    if (offscreen) g_eax = 1;
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
extern __declspec(thread) uint16_t g_fp_control_word;

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
            double h = FMUL(across, guest_float(camera + 0x108u));
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
    }
    return -1;
}

/* RECOMP_CULL_STATS=1 reports how many tests were settled here;
 * RECOMP_CULL_VERIFY=1 also runs the original for every one and counts the
 * answers that differ. */
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
        sub_00021A90_gen();
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

void sub_00054A60(void)
{
    uint32_t self = g_ecx;
    uint32_t name = self ? TUROK_MEM32(self + 0x20u) : 0;
    if (name) {
        const char *text = (const char *)((uintptr_t)name + g_xbox_mem_offset);
        int kind = is_front_end_screen(text);
        fprintf(stderr, "[TUROK] screen %.96s: %s\n", text, kind == 1 ? "front end" : kind ? "pause menu" : "game");
        video_settings_screen(kind == 1, kind == 2);
        video_settings_menu_screen(self, text);
    }
    sub_00054A60_gen();
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
extern __declspec(thread) uint32_t g_esi;
extern void sub_0003C9E0_gen(void);

void sub_0003C9E0(void)
{
    float scale = video_settings_draw_distance(), fade_near, fade_far;
    uint32_t block = g_esi ? TUROK_MEM32(g_esi + 0x1CCu) : 0, near_bits, far_bits;

    if (scale == 1.0f || !block) { sub_0003C9E0_gen(); return; }
    near_bits = TUROK_MEM32(block);
    far_bits = TUROK_MEM32(block + 4u);
    memcpy(&fade_near, &near_bits, 4);
    memcpy(&fade_far, &far_bits, 4);
    if (!(fade_near > 0.0f)) { sub_0003C9E0_gen(); return; }
    fade_near *= scale;
    fade_far *= scale;
    memcpy((void *)((uintptr_t)block + g_xbox_mem_offset), &fade_near, 4);
    memcpy((void *)((uintptr_t)(block + 4u) + g_xbox_mem_offset), &fade_far, 4);
    sub_0003C9E0_gen();
    TUROK_MEM32(block) = near_bits;
    TUROK_MEM32(block + 4u) = far_bits;
}

/* ── Stick response curve (sub_00142280) ───────────────────────
 *
 * this-call (the controller), args: float *x, float *y, and a third. For
 * the stick vector's length m: under 0.5 both are scaled by 0.25, under 0.9
 * by 0.3, and above it each is squared (keeping its sign). */
extern void sub_00142280_gen(void);

void sub_00142280(void)
{
    static int trace = -1;
    uint32_t ret = TUROK_MEM32(g_esp), xp = TUROK_MEM32(g_esp + 4u), yp = TUROK_MEM32(g_esp + 8u), self = g_ecx;
    float x0, y0, x1, y1;
    if (trace < 0) trace = getenv("RECOMP_CURVE_TRACE") != NULL;
    memcpy(&x0, (void *)((uintptr_t)xp + g_xbox_mem_offset), 4);
    memcpy(&y0, (void *)((uintptr_t)yp + g_xbox_mem_offset), 4);
    sub_00142280_gen();
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

    (void)xbox_va;
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
