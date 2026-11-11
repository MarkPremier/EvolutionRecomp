/* Exercise the real Xbox distance/visibility gates around the native wrapper. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
static unsigned char memory[4u << 20];
ptrdiff_t g_xbox_mem_offset;
__declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp;
__declspec(thread) double g_fp_stack[8];
__declspec(thread) int g_fp_top, g_fp_cmp;
__declspec(thread) uint16_t g_fp_control_word = 0x37f, g_fp_cc;
#define TUROK_MEM32(va) (*(uint32_t *)(memory + (va)))
static float scale;
float video_settings_draw_distance(void) { return scale; }
void sub_00052000(void);
void sub_00052010(void);
#include "pickup_distance_under_test.inc"
#include "pickup_distance_generated.inc"
void sub_00052000(void) { g_eax = 1; g_esp += 4; }
void sub_00052010(void) { g_eax = 0x20000; g_esp += 8; }
static void put(uint32_t at, float value) { memcpy(memory + at, &value, 4); }
#ifdef HAVE_GENERATED_PICKUP
static int updates(uint32_t table, int visible, float distance, float reach)
{
    const uint32_t actor = 0x10000;
    TUROK_MEM32(actor) = table;
    memory[actor + 0x53] = (unsigned char)visible;
    put(actor + 0x1D0, reach); put(actor + 0x38, distance);
    g_ecx = actor; g_esp = 0x30000;
    g_ebx = 11; g_esi = 22; g_edi = 33; g_seh_ebp = 44;
    g_fp_top = 0;
    sub_0003B1B0();
    assert(g_esp == 0x30004 && g_ebx == 11 && g_esi == 22 && g_edi == 33 && g_fp_top == 0);
    assert(*(float *)(memory + actor + 0x1D0) == reach);
    return g_eax != 0;
}
int main(void)
{
    const uint32_t pickups[] = {0x1FCFB8, 0x210450, 0x2255F0};
    g_xbox_mem_offset = (ptrdiff_t)memory;
    memory[0x20000 + 0x1E] = 1; /* A live player at the origin. */
    for (int s = 1; s <= 8; s *= 2) {
        scale = (float)s;
        for (unsigned i = 0; i < sizeof pickups / sizeof pickups[0]; i++) {
            assert(updates(pickups[i], 1, 2, 5));
            assert(updates(pickups[i], 1, 20, 5));
            assert(updates(pickups[i], 1, 1000, 5));
            assert(!updates(pickups[i], 0, 20, 5));
            TUROK_MEM32(0x10000 + 0x1D4) = 1; /* Original hidden-update prohibition. */
            assert(!updates(pickups[i], 0, 2, 5));
            TUROK_MEM32(0x10000 + 0x1D4) = 0;
        }
        assert(!updates(0x123456, 1, 20, 5)); /* AI/scenery cutoff is untouched. */
        assert(updates(0x123456, 1, 20, 0)); /* Original unlimited actors. */
        TUROK_MEM32(0x10000 + 0x7F8) = 3;
        assert(updates(FLYING_INDIGENOUS_AI_VTABLE, 1, 6, 5) == (s > 1));
        TUROK_MEM32(0x10000 + 0x7F8) = 2;
        assert(!updates(FLYING_INDIGENOUS_AI_VTABLE, 1, 6, 5));
    }
    puts("PASS: visible pickups update at all draw distances; hidden actors, AI, butterflies, guest ABI and AU_D are preserved");
    return 0;
}
#else
int main(void) { puts("SKIP: regenerate the Xbox functions for pickup distance integration checks"); return 0; }
#endif
