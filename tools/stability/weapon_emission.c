/* Test child creation through the original emitter scheduler and frustum test. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
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
static const uint32_t manager=0x10000, parent=0x20000, def=0x30000, emitter=0x40000;
static unsigned born, expected_count, original_no_cull;
static void test_indirect(uint32_t va, uint32_t saved)
{
    assert(va==0x123456 && g_ecx==parent);
    g_eax=born; g_esp=saved;
}
void sub_0008D120(void)
{
    assert(g_ecx==parent && TUROK_MEM32(g_esp+4)==expected_count);
    born+=TUROK_MEM32(g_esp+4);
    g_esp+=16;
}
void sub_0008A0C0(void)
{
    g_fp_top=(g_fp_top+7u)&7u; g_fp_stack[g_fp_top]=0; g_esp+=4;
}
void sub_000F4F34(void) { assert(!"periodic-emitter conversion is outside this fixture"); }
#include "weapon_emission_under_test.inc"
void sub_000B4480_gen(void)
{
    /* Simulate the parser's final flag setter; scheduling is real below. */
    uint32_t stack=g_esp, self=g_ecx;
    g_esp=0x710000; g_ecx=emitter; TUROK_MEM32(g_esp+4)=original_no_cull;
    sub_000AFF50();
    g_ecx=self; g_esp=stack+20; g_eax=1;
}
#include "weapon_emission_generated.inc"
recomp_func_t recomp_lookup(uint32_t va) { (void)va; assert(0); return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; assert(0); return NULL; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; assert(0); return NULL; }
void recomp_icall_fail_log(uint32_t va) { (void)va; assert(0); }
static void put(uint32_t at,float f) { memcpy(memory+at,&f,4); }
static void plane(unsigned i,float x,float y,float z,float d)
{
    uint32_t at=manager+0x4104+i*16;
    put(at,x); put(at+4,y); put(at+8,z); put(at+12,d);
}
static void setup(unsigned count)
{
    memset(memory,0,sizeof memory); born=0; expected_count=count;
    TUROK_MEM32(parent)=0x60000; TUROK_MEM32(0x60030)=0x123456;
    TUROK_MEM32(parent+0xC)=0x40008010;
    TUROK_MEM32(parent+0x48)=def; TUROK_MEM32(parent+0x4C)=manager;
    TUROK_MEM32(parent+0x1DC)=emitter;
    TUROK_MEM32(def)=0x10001004; /* Parent already bypasses simulation culling. */
    put(parent+0x3C,1); put(parent+0xB0,0.1f); put(def+0x268,0.01f);
    TUROK_MEM32(manager+0x18)=4000; TUROK_MEM32(manager+0x4C)=1;
    TUROK_MEM32(manager+0x4164)=5;
    plane(0,0,0,1,-0.25f); plane(1,1,0,1,0); plane(2,-1,0,1,0);
    plane(3,0,1,1,0); plane(4,0,-1,1,0);
    TUROK_MEM32(emitter+8)=count==1 ? 0x2 : 0xA; /* Shell / shotgun EMI: no NoCull bit. */
    TUROK_MEM32(emitter+0xC)=count; TUROK_MEM32(emitter+0x10)=count;
    put(emitter+0x18,-1);
    put(0x1FB7D8,0.0001f);
}
static void emit(float now)
{
    g_ecx=parent; g_esp=0x700000; put(g_esp+4,now);
    g_ebx=11; g_esi=22; g_edi=33; g_ebp=44; g_seh_ebp=44; g_fp_top=0;
    sub_0008D4C0();
    if (!(g_esp==0x700008 && g_ebx==11 && g_esi==22 && g_edi==33 && g_ebp==44 && g_fp_top==0))
        fprintf(stderr,"ABI: born=%u esp=%X ebx=%X esi=%X edi=%X ebp=%X fp=%d\n",born,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_fp_top);
    assert(g_esp==0x700008 && g_ebx==11 && g_esi==22 && g_edi==33 && g_ebp==44 && g_fp_top==0);
}
static void parse(const char *path,unsigned no_cull)
{
    char text[256]; snprintf(text,sizeof text,"*EMITDEF = { \"POINT\" \"%s\" }",path);
    strcpy((char *)memory+0x70000,text);
    g_ecx=0x80000; g_esp=0x700000;
    TUROK_MEM32(g_esp+8)=0x70000; TUROK_MEM32(g_esp+12)=(uint32_t)strlen(text);
    original_no_cull=no_cull; sub_000B4480();
    assert(g_esp==0x700014 && g_eax==1 && !s_weapon_emitter);
}
int main(void)
{
    g_xbox_mem_offset=(ptrdiff_t)memory;
    for(unsigned count=1;count<=10;count+=9) {
        unsigned original_flags=count==1 ? 0x2 : 0xA;
        setup(count); emit(1); assert(born==0); /* Parent updates, but no child is born. */
        put(parent+0xB0,1); emit(2); assert(born==count); /* Backing away releases it. */
        setup(count); parse("marc\\weapons\\shotgun\\shotgun_part",0); emit(1);
        assert(born==count); /* The same near-camera shot must now be born immediately. */
        assert(TUROK_MEM32(emitter+8)==(original_flags|0x20));
        emit(1.1f); assert(born==count); /* No duplicate shot. */
        setup(count); plane(0,0,0,1,0); put(parent+0xB0,-0.1f);
        emit(1); assert(born==0); /* Actual camera-eye plane, source behind the eye. */
        parse("marc\\weapons\\shotgun\\shotgun_part",0); emit(2); assert(born==count);
        g_ecx=manager; g_esp=0x700000;
        TUROK_MEM32(g_esp+4)=parent+0xA8; put(g_esp+8,0.01f);
        sub_0008A0E0(); assert(g_eax==1); /* Render visibility remains unchanged. */
        setup(count); parse("MARC/WEAPONS/SHOTGUN/shotgun_shells/SHOTGUN_SHELL",0); emit(1);
        assert(born==count);
        setup(count); parse("props\\sm_explosion\\sm_steam\\sm_steam",0); emit(1);
        assert(born==0 && TUROK_MEM32(emitter+8)==original_flags);
        setup(count); parse("weapon_enemy\\shotgun\\shotgun01",0); emit(1);
        assert(born==0 && TUROK_MEM32(emitter+8)==original_flags);
        setup(count); parse("props\\custom",1); emit(1); assert(born==count);
        setup(count); parse("marc\\weapons\\shotgun\\shotgun_part",0);
        put(emitter+0x18,0.005f); emit(1); assert(born==0); /* Distance limits remain. */
        setup(count); parse("marc\\weapons\\shotgun\\shotgun_part",0);
        TUROK_MEM32(manager+0x24)=4000; emit(1); assert(born==0); /* Pool budget remains. */
    }
    puts("PASS: real emitter reproduces delayed births; player shots/casings emit immediately with original rates, distance limits, budgets and guest ABI");
    return 0;
}
