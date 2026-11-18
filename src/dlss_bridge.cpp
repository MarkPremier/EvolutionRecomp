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
static int s_ready;

/* One DLAA feature per scene size, kept: the game switches between its 4:3
 * menus and cutscenes and its widescreen play, and making a feature takes
 * seconds -- each switch used to throw one away and make another. */
#define DLSS_FEATURES 4
static struct { NVSDK_NGX_Handle *handle; unsigned width, height; unsigned long long used; } s_features[DLSS_FEATURES];
static unsigned long long s_calls;
static int s_trace = -1;   /* RECOMP_DLSS_TRACE: each feature made, and the first calls after */

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
    NVSDK_NGX_Handle *feature = nullptr;
    int slot = -1, i;
    if (!s_ready || !context || !color || !depth || !motion || !output || !width || !height) return 0;
    if (s_trace < 0) {
        const char *trace = getenv("RECOMP_DLSS_TRACE");
        s_trace = trace && *trace && *trace != '0';
    }
    s_calls++;
    for (i = 0; i < DLSS_FEATURES; i++)
        if (s_features[i].handle && s_features[i].width == width && s_features[i].height == height) slot = i;
    if (slot >= 0) {
        feature = s_features[slot].handle;
        if (s_features[slot].used + 1 != s_calls) reset = 1;   /* back to this size: its history is old */
    } else {
        NVSDK_NGX_DLSS_Create_Params create = {};
        /* A free slot, or the one used longest ago. */
        for (i = 0, slot = 0; i < DLSS_FEATURES; i++) {
            if (!s_features[i].handle) { slot = i; break; }
            if (s_features[i].used < s_features[slot].used) slot = i;
        }
        if (s_features[slot].handle) NVSDK_NGX_D3D11_ReleaseFeature(s_features[slot].handle);
        s_features[slot].handle = nullptr;
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
        result = NGX_D3D11_CREATE_DLSS_EXT(context, &feature, s_parameters, &create);
        if (NVSDK_NGX_FAILED(result) || !feature) {
            report("creating the DLSS feature failed", result);
            return 0;
        }
        fprintf(stderr, "[DLSS] DLAA at %ux%u\n", width, height);
        fflush(stderr);
        s_features[slot].handle = feature;
        s_features[slot].width = width;
        s_features[slot].height = height;
        reset = 1;
    }
    s_features[slot].used = s_calls;
    if (s_trace > 0) {
        static unsigned shown;
        if (reset && shown > 20) shown = 0;   /* again after each change */
        if (shown++ < 20) {
            fprintf(stderr, "[DLSS] call %llu: %ux%u reset %d jitter %.3f %.3f\n", s_calls, width, height, reset,
                    jitter_x, jitter_y);
            fflush(stderr);
        }
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
    result = NGX_D3D11_EVALUATE_DLSS_EXT(context, feature, s_parameters, &evaluate);
    if (NVSDK_NGX_FAILED(result)) {
        static int reported;
        if (reported++ < 5) report("DLSS evaluation failed", result);
        return 0;
    }
    return 1;
}

extern "C" __declspec(dllexport) void dlss_bridge_shutdown(void)
{
    for (int i = 0; i < DLSS_FEATURES; i++) {
        if (s_features[i].handle) NVSDK_NGX_D3D11_ReleaseFeature(s_features[i].handle);
        s_features[i].handle = nullptr;
    }
    if (s_parameters) NVSDK_NGX_D3D11_DestroyParameters(s_parameters);
    s_parameters = nullptr;
    if (s_device) NVSDK_NGX_D3D11_Shutdown1(s_device);
    s_device = nullptr;
    s_ready = 0;
}
