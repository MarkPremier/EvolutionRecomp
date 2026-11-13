#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char s_ini_path[MAX_PATH];
static unsigned calls;
static uint32_t images[2][64*64], widths[2], heights[2];
void nv2a_gpu_replace_texture(uint64_t hash,const uint32_t *pixels,uint32_t width,uint32_t height)
{
    unsigned at=hash==0x89FA67573E0F3F67ull ? 0 : 1;
    assert(hash==0x89FA67573E0F3F67ull || hash==0x256B2E3DFDD6EA16ull);
    assert(width<=64 && height<=64);widths[at]=width;heights[at]=height;
    memcpy(images[at],pixels,(size_t)width*height*4);calls++;
}
#include "pc_icons_under_test.inc"
static void preview(void)
{
    BITMAPFILEHEADER file={0};BITMAPINFOHEADER info={0};
    uint32_t canvas[128*64];FILE *out;
    for(unsigned y=0;y<64;y++)for(unsigned x=0;x<128;x++)
        canvas[y*128+x]=images[x/64][y*64+x%64];
    file.bfType=0x4D42;file.bfOffBits=sizeof file+sizeof info;file.bfSize=file.bfOffBits+sizeof canvas;
    info.biSize=sizeof info;info.biWidth=128;info.biHeight=-64;info.biPlanes=1;info.biBitCount=32;
    out=fopen("pc_icons_preview.bmp","wb");assert(out);
    fwrite(&file,sizeof file,1,out);fwrite(&info,sizeof info,1,out);fwrite(canvas,sizeof canvas,1,out);fclose(out);
}
int main(void)
{
    char directory[MAX_PATH],tmp[MAX_PATH],path[MAX_PATH];
    unsigned char dds[136]={0};uint32_t word;
    _putenv_s("RECOMP_PC_GAME_DIR","");strcpy(s_ini_path,"Z:\\missing-test-settings.ini");
    assert(load_pc_icons()==2 && calls==2 && widths[0]==64 && widths[1]==64);
    assert(memcmp(images[0],images[1],sizeof images[0]));
    assert(!images[0][0] && !images[1][0]);
    assert((images[0][31*64+20]>>24)==255 && (images[1][22*64+6]>>24)==255);
    preview();
    _putenv_s("RECOMP_PC_GAME_DIR","Z:\\missing-pc-game");calls=0;assert(load_pc_icons()==2 && calls==2);
    /* A provided PC DDS still wins; a missing companion falls back separately. */
    GetTempPathA(sizeof tmp,tmp);GetTempFileNameA(tmp,"ico",0,directory);DeleteFileA(directory);assert(CreateDirectoryA(directory,NULL));
    const char *dirs[]={"data","data\\Textures","data\\Textures\\OnScreenGraphics","data\\Textures\\OnScreenGraphics\\FrontEnd","data\\Textures\\OnScreenGraphics\\FrontEnd\\Controls"};
    for(unsigned i=0;i<5;i++){snprintf(path,sizeof path,"%s\\%s",directory,dirs[i]);assert(CreateDirectoryA(path,NULL));}
    memcpy(dds,"DDS ",4);word=4;memcpy(dds+12,&word,4);memcpy(dds+16,&word,4);word=0x31545844;memcpy(dds+84,&word,4);
    dds[128]=0;dds[129]=0xF8; /* opaque red DXT1 block */
    snprintf(path,sizeof path,"%s\\%s\\OSG_bf_a.dds",directory,dirs[4]);FILE *f=fopen(path,"wb");assert(f);fwrite(dds,sizeof dds,1,f);fclose(f);
    _putenv_s("RECOMP_PC_GAME_DIR",directory);calls=0;assert(load_pc_icons()==2 && calls==2);
    assert(widths[0]==4 && heights[0]==4 && images[0][0]==0xFFFF0000 && widths[1]==64);
    DeleteFileA(path);
    for(int i=4;i>=0;i--){snprintf(path,sizeof path,"%s\\%s",directory,dirs[i]);RemoveDirectoryA(path);}RemoveDirectoryA(directory);
    puts("PASS: PC menu icons work without PC assets, recover from missing paths, and preserve optional per-icon PC DDS replacements");
    return 0;
}
