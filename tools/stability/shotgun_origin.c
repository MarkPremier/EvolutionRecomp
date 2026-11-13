/* Drive the real actor effect-spawn path; observe its transform at allocation. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
static unsigned char memory[8u << 20];
ptrdiff_t g_xbox_mem_offset;
__declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp;
__declspec(thread) uint32_t g_fs_base = 0x780000;
__declspec(thread) double g_fp_stack[8];
__declspec(thread) int g_fp_top, g_fp_cmp, g_df;
__declspec(thread) uint16_t g_fp_control_word = 0x37f, g_fp_cc;
#define TUROK_MEM32(va) (*(uint32_t *)(memory + (va)))
static const uint32_t weapon=0x10000, player=0x20000, camera=0x30000, world=0x40000, manager=0x50000;
static uint32_t captured[16];
static unsigned allocations;
#include "shotgun_origin_under_test.inc"
void sub_000966F0_gen(void)
{
    uint32_t transform=TUROK_MEM32(g_esp+12), info=TUROK_MEM32(g_esp+16);
    assert(g_ecx==manager && TUROK_MEM32(info+0x10)==weapon);
    memcpy(captured,memory+transform,sizeof captured); allocations++;
    g_eax=0; g_esp+=20;
}
#define UNREACHED(name) void name(void) { assert(!#name); }
UNREACHED(sub_00022D00) UNREACHED(sub_00035100) UNREACHED(sub_00038480)
UNREACHED(sub_00039220) UNREACHED(sub_0003A660) UNREACHED(sub_00040460) UNREACHED(sub_0008A5D0)
#include "shotgun_origin_generated.inc"
recomp_func_t recomp_lookup(uint32_t va) { (void)va; assert(0); return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; assert(0); return NULL; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; assert(0); return NULL; }
void recomp_icall_fail_log(uint32_t va) { (void)va; assert(0); }
static void put(uint32_t at,float value) { memcpy(memory+at,&value,4); }
static void setup(void)
{
    memset(memory,0,sizeof memory); allocations=0;
    TUROK_MEM32(weapon)=0x2143C8;
    TUROK_MEM32(weapon+4)=0x60000; TUROK_MEM32(0x60004)=weapon+4;
    TUROK_MEM32(weapon+0x48)=world; TUROK_MEM32(world+0x17C)=manager;
    TUROK_MEM32(weapon+0x234)=player; TUROK_MEM32(player+0x1A54)=weapon;
    TUROK_MEM32(player+0x3CC)=camera;
    for(unsigned i=0;i<16;i++) put(weapon+0x130+i*4,0);
    put(weapon+0x130,-1); put(weapon+0x144,1); put(weapon+0x158,-1); put(weapon+0x16C,1);
    /* Measured idle offset, translated away from the test's world origin. */
    put(camera+0x2FC,10); put(camera+0x300,20); put(camera+0x304,30);
    put(weapon+0x160,10.214f); put(weapon+0x164,19.782f); put(weapon+0x168,29.801f);
}
static void spawn(const char *name,int corrected)
{
    uint32_t before[16], expected[16];
    memcpy(before,memory+weapon+0x130,sizeof before); memcpy(expected,before,sizeof expected);
    if(corrected) memcpy(expected+12,memory+camera+0x2FC,12);
    strcpy((char *)memory+0x70000,name);
    g_ecx=weapon;g_esp=0x700000;
    TUROK_MEM32(g_esp)=0x123456;
    TUROK_MEM32(g_esp+4)=0x70000; /* resource */
    TUROK_MEM32(g_esp+8)=0; /* alternate */
    TUROK_MEM32(g_esp+12)=0; /* follow */
    TUROK_MEM32(g_esp+16)=0; /* flags */
    TUROK_MEM32(g_esp+20)=0; /* offset enabled */
    TUROK_MEM32(g_esp+24)=1; /* relative */
    TUROK_MEM32(g_esp+28)=0x71000; /* offset */
    TUROK_MEM32(g_esp+32)=0x71010; /* rotation */
    TUROK_MEM32(g_esp+36)=0xFFFFFFFF; /* no hotpoint */
    TUROK_MEM32(g_esp+40)=player;
    g_ebx=11;g_esi=22;g_edi=33;g_ebp=g_seh_ebp=44;g_fp_top=0;
    sub_000437E0();
    assert(allocations==1 && !memcmp(captured,expected,sizeof expected));
    assert(!memcmp(before,memory+weapon+0x130,sizeof before));
    assert(g_esp==0x70002C && g_ebx==11 && g_esi==22 && g_edi==33 && g_ebp==44 && g_fp_top==0);
    assert(TUROK_MEM32(weapon+4)==0x60000 && TUROK_MEM32(0x60004)==weapon+4);
}
int main(void)
{
    const char *shots[]={"shotgun_01.eff","shotgun_2.eff","shotgun_3.eff","supershotgun.eff"};
    const char *other[]={"shotgun_shells/shotgun_shell.eff","muzz_flash/shotgun_mflsh01.eff","shotgun__impact.eff"};
    char name[192];g_xbox_mem_offset=(ptrdiff_t)memory;
    for(unsigned i=0;i<4;i++) {
        snprintf(name,sizeof name,"$\\Data\\Particle\\marc\\weapons\\shotgun\\%s",shots[i]);
        setup();spawn(name,1);
        setup();TUROK_MEM32(weapon)=0x123456;spawn(name,0); /* enemy/unrelated actor */
        setup();TUROK_MEM32(player+0x1A54)=0;spawn(name,0); /* no longer held */
        setup();TUROK_MEM32(player+0x3CC)=0;spawn(name,0); /* no aim camera */
    }
    for(unsigned i=0;i<3;i++) {
        snprintf(name,sizeof name,"marc/weapons/shotgun/%s",other[i]);setup();spawn(name,0);
    }
    setup();spawn("$/DATA/PARTICLE/MARC/WEAPONS/SHOTGUN/SHOTGUN_01.EFF",1);
    setup();spawn("enemy/marc/weapons/shotgun/other.eff",0);
    puts("PASS: real actor spawn centers shotgun pellet origins on the player's camera, preserves orientation/caller state, and leaves shells/flashes/impacts/other actors unchanged");
    return 0;
}
