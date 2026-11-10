#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static unsigned char memory[4u << 20];
ptrdiff_t g_xbox_mem_offset;
__declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp;
__declspec(thread) double g_fp_stack[8];
__declspec(thread) int g_fp_top, g_fp_cmp;
__declspec(thread) uint16_t g_fp_control_word = 0x37f, g_fp_cc;
static int modern;
static float yaw, pitch, yaw_dt, pitch_dt;
#define TUROK_MEM32(va) (*(uint32_t *)(memory + (va)))
int video_settings_modern_aim(void) { return modern; }
void sub_000CF0C0(void);
void sub_00144430(void);
void sub_00144560(void);
static void capture_yaw(void);
#include "controller_aim_under_test.inc"
#include "controller_aim_generated.inc"

static void put(uint32_t at, float value) { memcpy(memory + at, &value, 4); }
static int near(float a, float b) { return fabsf(a - b) < 0.00002f; }

static void vector_checks(void)
{
    /* Equal radii must produce equal speeds at every angle, with no axis
     * snapping or discontinuities at the original curve's thresholds. */
    for (int angle = 0; angle < 360; angle++) {
        float a = angle * 0.01745329252f, last = -1;
        for (int step = 0; step <= 1000; step++) {
            float r = step / 1000.0f, x = cosf(a) * r, y = sinf(a) * r;
            modern_look_vector(&x, &y);
            float speed = sqrtf(x*x + y*y), want = fmaxf(0, (r - 0.15f) / 0.85f);
            assert(near(speed, want) && speed + 0.000002f >= last);
            assert(fabsf(x * sinf(a) - y * cosf(a)) < 0.00002f);
            last = speed;
        }
    }
    float x = 1, y = 1;
    modern_look_vector(&x, &y);
    assert(near(x, 0.70710678f) && near(y, x));
    x = NAN; y = 1; modern_look_vector(&x, &y); assert(x == 0 && y == 0);
    x = 1; y = INFINITY; modern_look_vector(&x, &y); assert(x == 0 && y == 0);
    puts("PASS: circular deadzone, continuous linear response, preserved direction and bounded diagonals");
}

static void guard_checks(void)
{
    uint32_t self = 0x10000, xp = self + 0x438, yp = self + 0x43c;
    put(xp, 0.25f); put(yp, -0.5f); put(self + 0x440, 0.2f);
    modern = 0;
    assert(!modern_look_curve(0x144848, self, xp, yp));
    modern = 1;
    for (uint32_t ret = 0x14556e; ret <= 0x14a963; ret++)
        assert(!modern_look_curve(ret, self, xp, yp));
    assert(guest_float(xp) == 0.25f && guest_float(yp) == -0.5f && guest_float(self + 0x440) == 0.2f);
    puts("PASS: Original mode and non-look callers leave their inputs and smoothing state untouched");
}

#ifdef HAVE_GENERATED_AIM
/* Run the actual lifted on-foot update around the native override. Observe
 * its final camera arguments without needing a loaded level or rendering. */
static void capture_yaw(void)
{
    yaw = guest_float(g_esp + 0x18); yaw_dt = guest_float(g_esp + 0x14);
    g_esp += 32;
}
void sub_00144430(void) { capture_yaw(); }
void sub_00144560(void)
{
    pitch = guest_float(g_esp + 0x28); pitch_dt = guest_float(g_esp + 0x20);
    g_esp += 44;
}
void sub_000CF0C0(void)
{
    double value = sqrt(guest_float(g_esp + 4));
    g_fp_top = (g_fp_top + 7) & 7; g_fp_stack[g_fp_top] = value;
    g_esp += 4;
}
static void pipeline_checks(void)
{
    put(0x1fdf14, 0.3f); put(0x2116ec, -0.3f); put(0x1fab0c, 1 / 0.7f);
    put(0x1faad4, 0.5f); put(0x1fab14, 0.9f); put(0x1fab18, 0);
    put(0x1fab1c, 0.2f); put(0x1fab30, 0.25f); put(0x1fbea8, 0.25f);
    for (int fps = 30; fps <= 240; fps += 30) for (int player = 0; player < 4; player++) {
        uint32_t self = 0x10000 + player * 0x3000;
        TUROK_MEM32(self) = 0x20000;
        for (int mode = 0; mode <= 1; mode++) for (int sign = -1; sign <= 1; sign += 2) {
            float x = sign * 0.5f, y = sign * 0.1f;
            put(self + 0x192c, x); put(self + 0x1928, 0);
            put(self + 0x1920, y); put(self + 0x1924, 0);
            put(self + 0x440, 0); put(self + 0x444, 0); put(self + 0x884, 1);
            g_ecx = self; g_esp = 0x8000; TUROK_MEM32(g_esp) = 0;
            put(g_esp + 4, 1.0f / fps); g_fp_top = 0;
            g_ebx = 11; g_esi = 22; g_edi = 33;
            modern = mode; sub_00144780();
            assert(g_esp == 0x8008 && g_fp_top == 0);
            assert(g_ebx == 11 && g_esi == 22 && g_edi == 33);
            assert(near(yaw_dt, 1.0f / fps) && near(pitch_dt, yaw_dt));
            if (mode) {
                modern_look_vector(&x, &y);
                assert(near(yaw, x) && near(pitch, y));
            } else {
                assert(near(yaw, sign * (0.2f / 0.7f) * 0.25f * 0.2f));
                assert(pitch == 0);  /* original axial deadzone */
            }
        }
    }
    puts("PASS: real on-foot pipeline preserves diagonals, Original response, dt, stack and registers for four players at 30-240 fps");
}
#else
void sub_00142280_gen(void) { g_esp += 16; }
#endif

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)memory;
    vector_checks(); guard_checks();
#ifdef HAVE_GENERATED_AIM
    pipeline_checks();
#endif
    return 0;
}
