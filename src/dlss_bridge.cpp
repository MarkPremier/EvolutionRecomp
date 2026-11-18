/* dlss_bridge.dll: NVIDIA DLSS (as DLAA, at native resolution) for the
 * Temporal AA setting's "dlaa" value.
 *
 * Kept out of turok_evolution.exe: the renderer (the toolkit's
 * nv2a_gpu_d3d11.cpp) loads this DLL only when DLSS is chosen, and carries
 * on with its own TAA when it is missing or the GPU cannot run DLSS. It
 * links NVIDIA's NGX SDK (nvsdk_ngx_d.lib) and needs NVIDIA's runtime,
 * nvngx_dlss.dll, beside the program. Neither is part of this repository:
 * the build takes them from DLSS_SDK_DIR (see CMakeLists.txt), and the DLSS
 * SDK is NVIDIA's, under NVIDIA's license.
 *
 * Inputs, all at the scene's own size (DLAA: input and output the same):
 * the scene's colour before the HUD, its depth as R32_FLOAT, motion vectors
 * (R16G16_FLOAT, scaled to pixels by the caller's MV scale), the frame's
 * sub-pixel jitter, and an output texture with unordered access. */
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>

#include "nvsdk_ngx_helpers_d3d.h"

/* This application's NGX project id (a random GUID; no NVIDIA application id). */
static const char k_project_id[] = "ff9d3085-451b-4037-bc7e-69b1a7866748";

static ID3D11Device *s_device;
static NVSDK_NGX_Parameter *s_parameters;
static NVSDK_NGX_Handle *s_feature;
static unsigned s_width, s_height;
static int s_ready;

static void report(const char *what, NVSDK_NGX_Result result)
{
    fprintf(stderr, "[DLSS] %s: 0x%08X\n", what, (unsigned)result);
    fflush(stderr);
}

/* 1 when DLSS can run on this device. log_dir: where NGX may write its logs. */
extern "C" __declspec(dllexport) int dlss_bridge_init(ID3D11Device *device, const wchar_t *log_dir)
{
    NVSDK_NGX_Result result;
    int available = 0, needs_driver = 0;
    if (s_ready) return 1;
    if (!device) return 0;
    result = NVSDK_NGX_D3D11_Init_with_ProjectID(k_project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", log_dir, device);
    if (NVSDK_NGX_FAILED(result)) {
        report("NGX initialisation failed (no NVIDIA RTX GPU or driver?)", result);
        return 0;
    }
    result = NVSDK_NGX_D3D11_GetCapabilityParameters(&s_parameters);
    if (NVSDK_NGX_FAILED(result) || !s_parameters) {
        report("NGX capability query failed", result);
        NVSDK_NGX_D3D11_Shutdown1(device);
        return 0;
    }
    NVSDK_NGX_Parameter_GetI(s_parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    NVSDK_NGX_Parameter_GetI(s_parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (!available) {
        fprintf(stderr, "[DLSS] DLSS is not available on this GPU%s\n", needs_driver ? " (it needs a newer driver)" : "");
        NVSDK_NGX_D3D11_DestroyParameters(s_parameters);
        s_parameters = nullptr;
        NVSDK_NGX_D3D11_Shutdown1(device);
        return 0;
    }
    s_device = device;
    s_ready = 1;
    fprintf(stderr, "[DLSS] ready (DLAA)\n");
    return 1;
}

/* One frame. Returns 1 when `output` holds the result. */
extern "C" __declspec(dllexport) int dlss_bridge_evaluate(ID3D11DeviceContext *context, ID3D11Resource *color,
                                                          ID3D11Resource *depth, ID3D11Resource *motion,
                                                          ID3D11Resource *output, unsigned width, unsigned height,
                                                          float jitter_x, float jitter_y, float motion_scale_x,
                                                          float motion_scale_y, int reset, float frame_ms)
{
    NVSDK_NGX_Result result;
    if (!s_ready || !context || !color || !depth || !motion || !output || !width || !height) return 0;
    if (!s_feature || width != s_width || height != s_height) {
        NVSDK_NGX_DLSS_Create_Params create = {};
        if (s_feature) NVSDK_NGX_D3D11_ReleaseFeature(s_feature);
        s_feature = nullptr;
        create.Feature.InWidth = width;
        create.Feature.InHeight = height;
        create.Feature.InTargetWidth = width;
        create.Feature.InTargetHeight = height;
        create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
        /* The title's colour is already tone-mapped (LDR). */
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        /* RECOMP_DLSS_PRESET=<letter>: NVIDIA's model for DLAA (default K,
         * the transformer; M a lighter one; E or F the older CNN). */
        {
            const char *preset = getenv("RECOMP_DLSS_PRESET");
            if (preset && *preset >= 'A' && *preset <= 'Z') {
                unsigned value = (unsigned)(*preset - 'A') + 1u;
                NVSDK_NGX_Parameter_SetUI(s_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, value);
                fprintf(stderr, "[DLSS] preset %c asked for\n", *preset);
            }
        }
        result = NGX_D3D11_CREATE_DLSS_EXT(context, &s_feature, s_parameters, &create);
        if (NVSDK_NGX_FAILED(result) || !s_feature) {
            report("creating the DLSS feature failed", result);
            s_feature = nullptr;
            return 0;
        }
        fprintf(stderr, "[DLSS] DLAA at %ux%u\n", width, height);
        s_width = width;
        s_height = height;
        reset = 1;
    }
    NVSDK_NGX_D3D11_DLSS_Eval_Params evaluate = {};
    evaluate.Feature.pInColor = color;
    evaluate.Feature.pInOutput = output;
    evaluate.pInDepth = depth;
    evaluate.pInMotionVectors = motion;
    evaluate.InJitterOffsetX = jitter_x;
    evaluate.InJitterOffsetY = jitter_y;
    evaluate.InRenderSubrectDimensions.Width = width;
    evaluate.InRenderSubrectDimensions.Height = height;
    evaluate.InReset = reset;
    evaluate.InMVScaleX = motion_scale_x;
    evaluate.InMVScaleY = motion_scale_y;
    evaluate.InFrameTimeDeltaInMsec = frame_ms;
    result = NGX_D3D11_EVALUATE_DLSS_EXT(context, s_feature, s_parameters, &evaluate);
    if (NVSDK_NGX_FAILED(result)) {
        static int reported;
        if (reported++ < 5) report("DLSS evaluation failed", result);
        return 0;
    }
    return 1;
}

extern "C" __declspec(dllexport) void dlss_bridge_shutdown(void)
{
    if (s_feature) NVSDK_NGX_D3D11_ReleaseFeature(s_feature);
    s_feature = nullptr;
    if (s_parameters) NVSDK_NGX_D3D11_DestroyParameters(s_parameters);
    s_parameters = nullptr;
    if (s_device) NVSDK_NGX_D3D11_Shutdown1(s_device);
    s_device = nullptr;
    s_ready = 0;
}
