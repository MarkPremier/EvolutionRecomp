#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <process.h>
#include "input/xinput_xbox.h"
#include "../../src/video_settings.h"
static int keys[256], mouse[5], pointer, wheel;
static char test_directory[MAX_PATH];
ptrdiff_t g_xbox_mem_offset;
ptrdiff_t xbox_GetMemoryOffset(void) { assert(!"direct-camera probe is disabled");return 0; }
static BOOL key_down(int vk) { return vk>0 && vk<256 && keys[vk]; }
static BOOL keyboard_enabled(void) { return FALSE; }
int xbox_FramebufferMouseDelta(int *x,int *y) { *x=*y=0;return 0; }
int xbox_FramebufferMouseButton(int n) { return mouse[n]; }
int xbox_FramebufferMenuPointer(void) { return pointer; }
int xbox_FramebufferMouseWheel(void) { int n=wheel;wheel=0;return n; }
#include "controls_input_under_test.inc"
static DWORD test_module_name(HMODULE module,char *path,DWORD count)
{
    (void)module;return (DWORD)snprintf(path,count,"%s\\controls_startup.exe",test_directory);
}
#define GetModuleFileNameA test_module_name
static void controls_show(int bind, int key) { (void)bind; (void)key; }
#include "controls_settings_under_test.inc"
#undef GetModuleFileNameA
static const int custom[BIND_COUNT] = {
    'I', 'K', 'J', 'L', 'U', 'O', VK_RETURN, VK_SHIFT, 'N', 'M', 'P', VK_TAB,
    'F', 'R', 'T', VK_F2, KEY_MOUSE_5, KEY_WHEEL_UP, 'H', 'B', VK_F4, 0,
};
static XBOX_INPUT_STATE poll(void) { XBOX_INPUT_STATE s;kbm_state(&s);return s; }
static void check_saved(void)
{
    XBOX_INPUT_STATE s;
    for(int i=0;i<BIND_COUNT;i++) assert(k_keys[s_binds[i]].code==custom[i]);
    apply_bindings();
    /* First input poll used to replace saved action keys with RECOMP_KBM_MAP. */
    keys['U']=1;s=poll();assert(s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER]==255);keys['U']=0;
    for(int i=0;i<BIND_WEAPON_FIRST;i++) {
        int target=-1;
        for(int t=0;t<KB_COUNT;t++) if(!strcmp(kbm_binds[t].name,k_binds[i].target)) target=t;
        assert(target>=0 && kbm_binds[target].vk[0]==custom[i]);
        assert(!kbm_binds[target].vk[1] && !kbm_binds[target].vk[2]);
        keys[custom[i]]=1;s=poll();assert(kbm_down(target));keys[custom[i]]=0;
    }
    keys[VK_LEFT]=keys[VK_RIGHT]=1;s=poll();assert(!(s.Gamepad.wButtons & (XBOX_GAMEPAD_DPAD_LEFT|XBOX_GAMEPAD_DPAD_RIGHT)));
    assert(!s.Gamepad.bAnalogButtons[XBOX_BUTTON_X] && !s.Gamepad.bAnalogButtons[XBOX_BUTTON_Y]);
    keys[VK_LEFT]=keys[VK_RIGHT]=0;poll();
    keys['2']=1;assert(!video_settings_weapon_input());keys['2']=0;
    keys['R']=1;assert(video_settings_weapon_input()==(1u<<1));keys['R']=0;
    mouse[4]=1;assert(video_settings_weapon_input()==(1u<<4));mouse[4]=0;
    wheel=1;poll();assert(video_settings_weapon_input()==(1u<<5));
}
int main(int argc,char **argv)
{
    XBOX_INPUT_STATE s;
    _putenv_s("RECOMP_KBM_LOOK_DIRECT","0");_putenv_s("RECOMP_KBM_LOOK_TRACE","0");
    _putenv_s("RECOMP_KBM_MAP","A=Space|Return,B=Back,X=WheelDown|E,Y=WheelUp|Q,LT=MouseR,RT=MouseL,WHITE=Control|C,BLACK=None,START=Escape|Tab,BACK=None,LTHUMB=None,RTHUMB=MouseM|Z,UP=Up,DOWN=Down,LEFT=Left,RIGHT=Right");
    _putenv_s("RECOMP_KBM_MENU_MAP","A=Return|Space,B=Back|Escape,START=Tab,UP=Up,DOWN=Down,LEFT=Left,RIGHT=Right");
    if(argc==3) {
        snprintf(test_directory,sizeof test_directory,"%s",argv[2]);video_settings_load();check_saved();
        puts("PASS: fresh process applies every saved binding before its first gameplay input poll");return 0;
    }
    { char tmp[MAX_PATH];GetTempPathA(sizeof tmp,tmp);GetTempFileNameA(tmp,"kbm",0,test_directory);DeleteFileA(test_directory);assert(CreateDirectoryA(test_directory,NULL)); }
    video_settings_load();
    for (int slot = 0; slot < VIDEO_WEAPON_SLOTS; slot++)
        assert(k_keys[s_binds[BIND_WEAPON_FIRST + slot]].code == k_binds[BIND_WEAPON_FIRST + slot].code);
    assert(WritePrivateProfileStringA("controls", "zoom", "3", s_ini_path));
    video_settings_load();
    assert(k_keys[s_binds[BIND_ZOOM]].code == '3');
    assert(s_binds[BIND_WEAPON_FIRST + 2] == 0);
    for(int i=0;i<BIND_COUNT;i++) s_binds[i]=key_index(custom[i]);
    controls_save();check_saved();
    assert(_spawnl(_P_WAIT,argv[0],argv[0],"reload",test_directory,NULL)==0);
    controls_set(BIND_WEAPON_FIRST + 1, key_index('U'));
    assert(k_keys[s_binds[BIND_FIRE]].code == 'R');
    keys['U']=1;assert(video_settings_weapon_input() & (1u<<1));keys['U']=0;
    controls_set(BIND_WEAPON_FIRST + 1, 0);
    keys['U']=1;assert(!(video_settings_weapon_input() & (1u<<1)));keys['U']=0;
    /* Arrow keys belong to menus, or to the one gameplay action selected. */
    s_binds[BIND_LEFT]=key_index(VK_LEFT);apply_bindings();keys[VK_LEFT]=1;s=poll();
    assert(s.Gamepad.sThumbLX==-32767 && !(s.Gamepad.wButtons & XBOX_GAMEPAD_DPAD_LEFT));
    keys[VK_LEFT]=0;poll();xbox_KbmSetMenuMode(1);poll();
    keys['F']=1;assert(!video_settings_weapon_input());keys['F']=0;
    keys[VK_LEFT]=1;s=poll();assert(s.Gamepad.wButtons & XBOX_GAMEPAD_DPAD_LEFT);keys[VK_LEFT]=0;
    keys[VK_RETURN]=1;s=poll();assert(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A]==255);keys[VK_RETURN]=0;
    keys[VK_ESCAPE]=1;s=poll();assert(s.Gamepad.bAnalogButtons[XBOX_BUTTON_B]==255);keys[VK_ESCAPE]=0;
    poll();xbox_KbmSetMenuMode(0);poll();
    s_binds[BIND_FIRE]=0;apply_bindings();keys['U']=1;s=poll();assert(!s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER]);keys['U']=0;
    s_binds[BIND_FIRE]=key_index(KEY_MOUSE_4);apply_bindings();mouse[3]=1;s=poll();assert(s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER]==255);mouse[3]=0;
    s_binds[BIND_NEXT]=key_index(KEY_WHEEL_DOWN);apply_bindings();wheel=-1;s=poll();assert(s.Gamepad.bAnalogButtons[XBOX_BUTTON_X]==255);
    DeleteFileA(s_ini_path);RemoveDirectoryA(test_directory);
    puts("PASS: weapon defaults, saved key conflicts, live swaps and unbinding, mouse buttons, wheel and independent menu controls");
    return 0;
}
