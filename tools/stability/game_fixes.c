/* The runner extracts these functions verbatim from the real overrides. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static unsigned char memory[8u << 20];
ptrdiff_t g_xbox_mem_offset;
__declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp, g_esi, g_edi;
__declspec(thread) int g_df;
static int keep;
static uint32_t manager = 0x10000u;
static unsigned killed;

#include "game_overrides_under_test.inc"
#include "effect_part_generated.inc"

MCPXAPUState *g_apu_state;
static int idle_valid, idle_held, idle_retired;
int mcpx_apu_voice_idle_begin(MCPXAPUState *d, uint32_t voice)
{
    (void)d; (void)voice;
    idle_held = idle_valid;
    return idle_valid;
}
void mcpx_apu_voice_idle_end(MCPXAPUState *d, uint32_t voice)
{
    (void)d; (void)voice;
    assert(idle_held); idle_held = 0;
}
void sub_001B23DD_gen(void)
{
    assert(idle_held); idle_retired++; g_esp += 8;
}

static void test_idle_handler(void)
{
    const uint32_t device = 0x50000, stack = 0x30000, voice = 3;
    g_ecx = device; g_esp = stack; TUROK_MEM32(stack + 4) = voice;
    idle_valid = 1;
    sub_001B23DD();
    assert(g_esp == stack + 8 && !idle_retired && !idle_held);
    TUROK_MEM32(device + 0x330 + voice * 4) = 1;
    idle_valid = 0; g_esp = stack;
    sub_001B23DD();
    assert(g_esp == stack + 8 && !idle_retired && !idle_held);
    idle_valid = 1; g_esp = stack;
    sub_001B23DD();
    assert(g_esp == stack + 8 && idle_retired == 1 && !idle_held);
    puts("PASS: shared DirectSound handler skips stale/unowned voices and guards valid retirement");
}

int video_settings_keep_decals(void) { return keep; }
void sub_000AF2A0_gen(void)
{
    float life = fabsf(guest_float(g_esp + 4));
    memcpy(memory + g_ecx + 0x7C, &life, 4);
    g_esp += 8;
}
void sub_000B98D0_gen(void) {}
#ifndef HAVE_GENERATED_EFFECT_PART
void sub_000AF9A0_gen(void)
{
    g_eax = g_ecx;
    TUROK_MEM32(g_eax) = TUROK_MEM32(g_esp + 4);
    TUROK_MEM32(g_eax + 4) = TUROK_MEM32(g_esp + 8);
    g_ecx = 99;
    g_esp += 56;
}
#endif

static void kill_particle(void)
{
    uint32_t at = g_ecx, before = TUROK_MEM32(at + 0x1A8), after = TUROK_MEM32(at + 0x1A4);
    TUROK_MEM32(before + 0x1A4) = after;
    TUROK_MEM32(after + 0x1A8) = before;
    TUROK_MEM32(at + 0x1A8) = 0;
    TUROK_MEM32(manager + 0x24)--;
    g_esp += 4;
    g_eax = g_edx = 99;
    killed++;
}

recomp_func_t recomp_lookup(uint32_t address)
{
    return address == 0x123456u ? kill_particle : NULL;
}

static void put_float(uint32_t at, float value) { memcpy(memory + at, &value, 4); }

static void test_decals(void)
{
    keep_decals_reset();
    keep = 1;
    s_partdef_keepsake = 1;
    g_ecx = 0x20000; g_esp = 0x30000;
    put_float(g_esp + 4, 3);
    sub_000AF2A0();
    assert(s_keep_count == 1 && guest_float(0x2007C) == 600);
    keep_decals_frame();
    keep = 0; keep_decals_frame();
    assert(guest_float(0x2007C) == 3);
    keep = 1; keep_decals_frame();
    assert(guest_float(0x2007C) == 600);

    /* World teardown followed by another object at exactly the same address. */
    keep_decals_reset();
    keep = 0; put_float(0x2007C, 600); keep_decals_frame();
    assert(s_keep_count == 0 && guest_float(0x2007C) == 600);

    /* Reparsing a recorded definition as a non-decal also invalidates it. */
    keep = 1; s_partdef_keepsake = 1; g_ecx = 0x20000; g_esp = 0x30000;
    put_float(g_esp + 4, 3); sub_000AF2A0();
    s_partdef_keepsake = 0; g_ecx = 0x20000; g_esp = 0x30000;
    put_float(g_esp + 4, 600); sub_000AF2A0();
    keep = 0; keep_decals_frame();
    assert(s_keep_count == 0 && guest_float(0x2007C) == 600);
    puts("PASS: decal lifetime reset, live toggles, and reparsed definitions");
}

static void define_keepsake(uint32_t def, float life)
{
    s_partdef_keepsake = 1; g_ecx = def; g_esp = 0x30000;
    put_float(g_esp + 4, life); sub_000AF2A0();
}

static void bind_effect(uint32_t def, uint32_t emitter)
{
    unsigned i;
    g_ecx = 0x40000; g_esp = 0x30000;
    TUROK_MEM32(g_esp + 4) = def;
    TUROK_MEM32(g_esp + 8) = emitter;
    TUROK_MEM32(g_esp + 12) = 1;
    TUROK_MEM32(g_esp + 16) = 0x60000;
    for (i = 0; i < 16; i++) TUROK_MEM32(0x60000 + i * 4) = 100 + i;
    for (i = 0; i < 9; i++) TUROK_MEM32(g_esp + 20 + i * 4) = 200 + i;
    g_esi = 22; g_edi = 33;
    sub_000AF9A0();
    assert(g_eax == 0x40000 && g_esp == 0x30038 && g_esi == 22 && g_edi == 33);
    assert(TUROK_MEM32(0x40000) == def && TUROK_MEM32(0x40004) == emitter);
#ifdef HAVE_GENERATED_EFFECT_PART
    assert(TUROK_MEM32(0x40008) == 1);
    for (i = 0; i < 16; i++) assert(TUROK_MEM32(0x4000C + i * 4) == 100 + i);
    for (i = 0; i < 9; i++) assert(TUROK_MEM32(0x4004C + i * 4) == 200 + i);
#endif
}

static void test_emitter_lifetimes(void)
{
    unsigned i;
    const uint32_t parent = 0x20000, decal = 0x20200;
    keep_decals_reset(); keep = 1;
    /* Steam parents reuse a bullet-hole texture; real bullet holes have no emitter. */
    define_keepsake(parent, 5); define_keepsake(decal, 10);
    assert(guest_float(parent + 0x7C) == 600);
    bind_effect(parent, 0x50000); bind_effect(decal, 0);
    assert(guest_float(parent + 0x7C) == 5);
    assert(guest_float(decal + 0x7C) == 600);
    keep_decals_frame();
    keep = 0; keep_decals_frame();
    assert(guest_float(parent + 0x7C) == 5 && guest_float(decal + 0x7C) == 10);
    keep = 1; keep_decals_frame();
    assert(guest_float(parent + 0x7C) == 5 && guest_float(decal + 0x7C) == 600);

    /* Binding while disabled must still prevent a later toggle extending steam. */
    keep_decals_reset(); keep = 0;
    define_keepsake(parent, 1); bind_effect(parent, 0x50000);
    keep_decals_frame(); keep = 1; keep_decals_frame();
    assert(guest_float(parent + 0x7C) == 1);

    /* Respect game-side edits, and forget the role if the address is reparsed. */
    define_keepsake(parent, 5); put_float(parent + 0x7C, 7);
    bind_effect(parent, 0x50000);
    keep = 0; keep_decals_frame(); keep = 1; keep_decals_frame();
    assert(guest_float(parent + 0x7C) == 7);
    define_keepsake(parent, 3); bind_effect(parent, 0);
    assert(guest_float(parent + 0x7C) == 600);
    keep = 0; keep_decals_frame();
    assert(guest_float(parent + 0x7C) == 3);

    /* Never extend an untracked definition: it could be an emitter parent. */
    keep_decals_reset(); keep = 1;
    for (i = 0; i < KEEP_DECAL_DEFS; i++) define_keepsake(0x100000 + i * 0x100, 10);
    define_keepsake(parent, 5); bind_effect(parent, 0x50000);
    assert(s_keep_count == KEEP_DECAL_DEFS && guest_float(parent + 0x7C) == 5);
    keep_decals_reset();
    puts("PASS: emitter parents retain original lifetimes, live toggles, address reuse, and full tracking table");
}

static void test_wobble(void)
{
    const int rates[] = {30, 60, 120, 144, 240};
    unsigned rate, method, player;
    TUROK_MEM32(GAME_TIMER_PTR) = 0x40000;
    for (rate = 0; rate < sizeof rates / sizeof *rates; rate++) {
        int steps[4][4] = {{0}}, frame;
        aim_wobble_reset();
        put_float(0x4005C, 1.0f / rates[rate]);
        for (frame = 0; frame < rates[rate]; frame++)
            for (player = 0; player < 4; player++)
                for (method = 0; method < 4; method++)
                    /* Deliberate hash collisions between player instances. */
                    steps[player][method] += aim_wobble_due(0x50000 + player * 0x400, method);
        for (player = 0; player < 4; player++)
            for (method = 0; method < 4; method++)
                assert(steps[player][method] == (rates[rate] < 60 ? rates[rate] : 60));
    }
    aim_wobble_reset();
    for (player = 0; player < AIM_WOBBLE_CLOCKS; player++) assert(!s_wobble_clocks[player].self);
    puts("PASS: independent wobble clocks for four players at 30/60/120/144/240 fps");
}

static void make_particles(unsigned count, int kept)
{
    unsigned n, priority, kind;
    uint32_t head = manager + 0x1E1C, tail = manager + 0xCC4, previous = head;
    particle_tracking_reset();
    s_particle_manager = manager;
    for (priority = 0; priority < 5; priority++)
        for (kind = 0; kind < 2; kind++)
            TUROK_MEM32(manager + (kind ? 0x26C8 : 0x1E1C) + priority * 0x1BC + 0x1A4) = 0;
    put_float(0x6007C, 600);
    TUROK_MEM32(0x7001C) = 0x123456;
    for (n = 0; n < count; n++) {
        uint32_t at = 0x100000 + n * 0x200;
        TUROK_MEM32(at) = 0x70000;
        TUROK_MEM32(at + 0xC) = 0;
        TUROK_MEM32(at + 0x48) = kept ? 0x60000 : 0;
        TUROK_MEM32(at + 0x1A8) = previous;
        TUROK_MEM32(previous + 0x1A4) = at;
        *particle_stamp(at) = ++s_particle_stamp;
        previous = at;
    }
    TUROK_MEM32(previous + 0x1A4) = tail;
    TUROK_MEM32(tail + 0x1A8) = previous;
    TUROK_MEM32(manager + 0x24) = count;
    killed = 0;
}

static void test_particles(void)
{
    unsigned i;
    make_particles(4000, 0);
    particle_trim(manager, 10);
    assert(s_particle_trim.valid && s_particle_trim.count == 0);
    /* Poison the head: another walk would fault. An empty result is reused. */
    TUROK_MEM32(manager + 0x1E1C + 0x1A4) = 0xFFFFFFFF;
    for (i = 0; i < 1000; i++) particle_trim(manager, 10);
    assert(killed == 0);

    make_particles(4, 1);
    g_eax = 1; g_ecx = 2; g_edx = 3; g_esp = 0x80000;
    particle_trim(manager, 1);
    assert(killed == 1 && TUROK_MEM32(0x100000 + 0x1A8) == 0);
    assert(g_eax == 1 && g_ecx == 2 && g_edx == 3 && g_esp == 0x80000);
    /* A new particle reuses the next candidate's address. Keep the new one. */
    *particle_stamp(0x100200) = ++s_particle_stamp;
    particle_trim(manager, 1);
    assert(killed == 2 && TUROK_MEM32(0x100200 + 0x1A8) != 0);
    assert(TUROK_MEM32(0x100400 + 0x1A8) == 0);
    particle_trim(manager, 20);
    assert(killed == 3);
    s_particle_frame++;
    particle_trim(manager, 1);
    assert(killed == 4);
    puts("PASS: particle batching, oldest-first removal, recycled addresses, frame refresh, and registers");
    free(s_particle_trim.kept);
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)memory;
    test_idle_handler(); test_decals(); test_emitter_lifetimes(); test_wobble(); test_particles();
    return 0;
}
