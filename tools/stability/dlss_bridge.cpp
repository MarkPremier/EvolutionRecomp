/* Execute the production bridge with a small mock of the NGX API contract.
 * This checks cache/lifetime logic, not NVIDIA's GPU implementation. */
#include <windows.h>
#include <d3d11.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
using NVSDK_NGX_Result=int;
struct NVSDK_NGX_Parameter {};
struct NVSDK_NGX_Handle { unsigned serial; };
struct NVSDK_NGX_DLSS_Create_Params {
    struct { unsigned InWidth,InHeight,InTargetWidth,InTargetHeight;int InPerfQualityValue; } Feature;
    unsigned InFeatureCreateFlags;
};
struct NVSDK_NGX_D3D11_DLSS_Eval_Params {
    struct { ID3D11Resource *pInColor,*pInOutput; } Feature;
    ID3D11Resource *pInDepth,*pInMotionVectors;
    float InJitterOffsetX,InJitterOffsetY;
    struct { unsigned Width,Height; } InRenderSubrectDimensions;
    int InReset;
    float InMVScaleX,InMVScaleY,InFrameTimeDeltaInMsec;
};
enum { NVSDK_NGX_ENGINE_TYPE_CUSTOM, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
       NVSDK_NGX_Parameter_SuperSampling_Available,NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
       NVSDK_NGX_PerfQuality_Value_DLAA,NVSDK_NGX_DLSS_Feature_Flags_AutoExposure };
#define NVSDK_NGX_FAILED(result) (!(result))
static NVSDK_NGX_Parameter parameters;
static std::unordered_map<NVSDK_NGX_Handle *,std::pair<unsigned,unsigned>> live;
static unsigned creates,releases,evaluates,destroys,shutdowns;
static bool reset_seen,fail_create,fail_evaluate;
static int NVSDK_NGX_D3D11_Init_with_ProjectID(const char *,int,const char *,const wchar_t *,ID3D11Device *) { return 1; }
static int NVSDK_NGX_D3D11_GetCapabilityParameters(NVSDK_NGX_Parameter **p) { *p=&parameters;return 1; }
static int NVSDK_NGX_Parameter_GetI(NVSDK_NGX_Parameter *,int key,int *out) { *out=key==NVSDK_NGX_Parameter_SuperSampling_Available;return 1; }
static void NVSDK_NGX_Parameter_SetUI(NVSDK_NGX_Parameter *,int,unsigned) {}
static int NGX_D3D11_CREATE_DLSS_EXT(ID3D11DeviceContext *,NVSDK_NGX_Handle **handle,NVSDK_NGX_Parameter *,NVSDK_NGX_DLSS_Create_Params *p)
{
    creates++;if(fail_create)return 0;
    assert(p->Feature.InWidth==p->Feature.InTargetWidth && p->Feature.InHeight==p->Feature.InTargetHeight);
    *handle=new NVSDK_NGX_Handle{creates};live[*handle]={p->Feature.InWidth,p->Feature.InHeight};return 1;
}
static int NGX_D3D11_EVALUATE_DLSS_EXT(ID3D11DeviceContext *,NVSDK_NGX_Handle *handle,NVSDK_NGX_Parameter *,NVSDK_NGX_D3D11_DLSS_Eval_Params *p)
{
    assert(live.count(handle));auto size=live.at(handle);
    assert(size.first==p->InRenderSubrectDimensions.Width && size.second==p->InRenderSubrectDimensions.Height);
    reset_seen=p->InReset!=0;evaluates++;return !fail_evaluate;
}
static int NVSDK_NGX_D3D11_ReleaseFeature(NVSDK_NGX_Handle *p) { assert(live.erase(p)==1);delete p;releases++;return 1; }
static int NVSDK_NGX_D3D11_DestroyParameters(NVSDK_NGX_Parameter *p) { assert(p==&parameters);destroys++;return 1; }
static int NVSDK_NGX_D3D11_Shutdown1(ID3D11Device *) { shutdowns++;return 1; }
#include "dlss_bridge_under_test.inc"
static int frame(unsigned width,unsigned height,int reset=0)
{
    return dlss_bridge_evaluate(reinterpret_cast<ID3D11DeviceContext *>(1),reinterpret_cast<ID3D11Resource *>(2),
        reinterpret_cast<ID3D11Resource *>(3),reinterpret_cast<ID3D11Resource *>(4),reinterpret_cast<ID3D11Resource *>(5),
        width,height,0.25f,-0.125f,-(float)width,-(float)height,reset,16.7f);
}
int main()
{
    _putenv_s("RECOMP_DLSS_TRACE","0");
    assert(!frame(64,48));assert(dlss_bridge_init(reinterpret_cast<ID3D11Device *>(1),L"."));
    assert(frame(64,48) && reset_seen && creates==1 && !s_trace);
    for(int i=0;i<100;i++)assert(frame(64,48) && !reset_seen);
    assert(creates==1);
    assert(frame(86,48) && reset_seen && creates==2);
    assert(frame(64,48) && reset_seen && creates==2); /* returning from widescreen */
    assert(frame(64,48,1) && reset_seen); /* level change */
    for(unsigned i=0;i<10;i++)assert(frame(100+i,48));
    assert(live.size()==DLSS_FEATURES && releases==creates-DLSS_FEATURES);
    assert(!frame(0,48));
    fail_create=true;assert(!frame(250,48));fail_create=false;assert(frame(250,48));
    fail_evaluate=true;assert(!frame(250,48));fail_evaluate=false;
    dlss_bridge_shutdown();assert(live.empty() && destroys==1 && shutdowns==1);
    dlss_bridge_shutdown();assert(destroys==1 && shutdowns==1);
    assert(dlss_bridge_init(reinterpret_cast<ID3D11Device *>(1),L"."));assert(frame(64,48) && reset_seen);
    dlss_bridge_shutdown();assert(live.empty() && destroys==2 && shutdowns==2);
    std::puts("PASS: real DLAA bridge reuses sizes, resets returning histories, bounds cached features, handles NGX failures, and releases once");
}
