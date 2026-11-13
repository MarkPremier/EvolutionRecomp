/* Execute the original particle update through its real sphere/frustum test. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
static unsigned char memory[8u << 20];
ptrdiff_t g_xbox_mem_offset;
__declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp;
__declspec(thread) double g_fp_stack[8];
__declspec(thread) int g_fp_top, g_fp_cmp, g_df;
__declspec(thread) uint16_t g_fp_control_word = 0x37f, g_fp_cc;
#define TUROK_MEM32(va) (*(uint32_t *)(memory + (va)))

static const uint32_t manager = 0x10000, def = 0x20000, particle = 0x30000;
static unsigned lifetime_calls;
void sub_000948F0(void)
{
    /* The update reached lifetime/physics rather than returning at visibility.
     * Stop here so this fixture does not emulate collisions or animation. */
    assert(g_ecx == particle && TUROK_MEM32(g_esp + 4) == def);
    lifetime_calls++; g_eax = 1; g_esp += 8;
}
#define UNREACHED(name) void name(void) { assert(!#name); }
UNREACHED(sub_000C9DB0) UNREACHED(sub_00091F50) UNREACHED(sub_00091400)
UNREACHED(sub_00093DF0) UNREACHED(sub_00092150) UNREACHED(sub_000B7BA0)
UNREACHED(sub_00092240) UNREACHED(sub_00092010) UNREACHED(sub_00090AE0)
UNREACHED(sub_00091910) UNREACHED(sub_00093120) UNREACHED(sub_00094C30)
UNREACHED(sub_000939F0) UNREACHED(sub_00090300) UNREACHED(sub_000BAF10)
UNREACHED(sub_00090DF0) UNREACHED(sub_00083DA0) UNREACHED(sub_00052050)
UNREACHED(sub_00086830)
#include "particle_simulation_generated.inc"
recomp_func_t recomp_lookup(uint32_t va) { (void)va; assert(0); return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; assert(0); return NULL; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; assert(0); return NULL; }
void recomp_icall_fail_log(uint32_t va) { (void)va; assert(0); }
static void put(uint32_t at, float f) { memcpy(memory + at, &f, 4); }
static float get(uint32_t at) { float f; memcpy(&f, memory + at, 4); return f; }
static void plane(unsigned index, float x, float y, float z, float d)
{
    uint32_t at = manager + 0x4104 + index * 16;
    put(at,x); put(at+4,y); put(at+8,z); put(at+12,d);
}
static void setup(unsigned mode)
{
    memset(memory,0,sizeof memory);
    TUROK_MEM32(manager + 0x4168) = mode;
    TUROK_MEM32(manager + 0x4164) = mode == 8 ? 6 : 5;
    TUROK_MEM32(manager + 0x48) = 1;
    TUROK_MEM32(manager + 0xA88) = 0x40000;
    TUROK_MEM32(0x40110) = 0x50000;
    plane(0,0,0,1,-0.25f); /* Camera near plane. */
    plane(1,1,0,1,0); plane(2,-1,0,1,0);
    plane(3,0,1,1,0); plane(4,0,-1,1,0); plane(5,0,0,-1,10);
    TUROK_MEM32(particle + 0x48) = def;
    TUROK_MEM32(particle + 0x4C) = manager;
    TUROK_MEM32(particle + 0xC) = 0x40000010; /* Active, first update ready. */
    put(particle+0x34,0.01f); put(particle+0x38,0.01f); put(particle+0x3C,1);
    put(particle+0x10,0.25f);
    TUROK_MEM32(0x201F4C) = 0x3FB50481; /* Original sqrt(2) radius multiplier. */
    TUROK_MEM32(0x952D0) = 0x94D1B; /* Priority-zero counter jump-table entry. */
    lifetime_calls = 0;
}
static unsigned update(float x, float z, float now)
{
    unsigned before = lifetime_calls;
    uint32_t planes[24]; memcpy(planes,memory+manager+0x4104,sizeof planes);
    put(particle+0xA8,x); put(particle+0xAC,0); put(particle+0xB0,z);
    g_ecx=particle; g_esp=0x700000; put(g_esp+4,now);
    g_ebx=11; g_esi=22; g_edi=33; g_ebp=44; g_seh_ebp=44; g_fp_top=0;
    sub_00094D00();
    assert(g_esp==0x700008 && g_ebx==11 && g_esi==22 && g_edi==33 && g_ebp==44 && g_fp_top==0);
    assert(!memcmp(planes,memory+manager+0x4104,sizeof planes));
    return lifetime_calls-before;
}
static unsigned sphere(uint32_t caller)
{
    g_ecx=manager; g_esp=0x700000; TUROK_MEM32(g_esp)=caller;
    put(g_esp+4,0); put(g_esp+8,0); put(g_esp+12,0.1f); put(g_esp+16,0.01f);
    g_fp_top=0; sub_0008A0E0();
    assert(g_esp==0x700014 && g_fp_top==0);
    return g_eax;
}
int main(void)
{
    g_xbox_mem_offset=(ptrdiff_t)memory;
    for(unsigned mode=7;mode<=8;mode++) {
        setup(mode);
        /* Ordinary culled effects pause before lifetime/physics, then resume. */
        assert(update(0,0.1f,0.5f)==0);
        assert(get(particle+0x104)==0);
        assert(update(0,1,0.7f)==1);
        assert(fabsf(get(particle+0x104)-0.45f)<1e-6f);
        assert(update(2,0.1f,0.8f)==0); /* Side rejection still works. */
        if(mode==8) assert(update(0,20,0.9f)==0); /* Far rejection remains. */
        assert(sphere(0x95A26)==1); /* Render culling retains its near plane. */
        assert(sphere(0x123456)==1); /* Other callers retain original results. */
        assert(sphere(0x94EBA)==1);
        TUROK_MEM32(manager+0x4168)=3; /* Custom/side-only plane configurations. */
        assert(sphere(0x94EBA)==1);
        assert(update(0,0.1f,1)==0);

        /* Live shotgun definitions use this flag, bypassing update culling
         * already. Removing the near plane cannot fix their reported hangs. */
        setup(mode); TUROK_MEM32(def)=0x10001004u;
        assert(update(0,0.1f,0.5f)==1);
        assert(fabsf(get(particle+0x104)-0.25f)<1e-6f);
        assert(update(2,0.1f,0.6f)==1);
        assert(update(0,20,0.7f)==1);
        assert(sphere(0x95A26)==1); /* This flag does not disable render clipping. */
    }
    puts("PASS: original particle culling pauses ordinary effects; AlwaysUpdate weapon effects advance inside/outside the frustum; guest ABI is preserved");
    return 0;
}
