#include "dlss.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

#if defined(RFGRT_DLSS)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_dlssd_d3d.h>
#include <nvsdk_ngx_params.h>
#endif

namespace rtcore::dlss {

#if defined(RFGRT_DLSS)

namespace {

// NGX asks custom engines for a project id of their own (GUID-style).
constexpr char kProjectId[] = "8d2f5b61-3c47-4e9a-b0d4-6a91c2e7f3b8";

struct State {
    bool initialized = false, available = false;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    unsigned width = 0, height = 0, out_width = 0, out_height = 0;  // the feature's render and output sizes, its mode
    int mode = 0;
    bool told_eval_failure = false;
    LogFn log = nullptr;
    // render_size's last answer
    unsigned size_out_width = 0, size_out_height = 0, size_width = 0, size_height = 0;
    int size_mode = -1;
} s;

NVSDK_NGX_PerfQuality_Value quality_value(int mode) {
    switch (mode) {
        case 2: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case 3: return NVSDK_NGX_PerfQuality_Value_Balanced;
        case 4: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case 5: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        default: return NVSDK_NGX_PerfQuality_Value_DLAA;
    }
}

void logf(const char* fmt, ...) {
    if (!s.log) return;
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    s.log(line);
}

void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    if (!message || !s.log) return;
    char line[512];
    size_t n = 0;
    for (const char* c = message; *c && n + 1 < sizeof(line); ++c)
        if (*c != '\n' && *c != '\r') line[n++] = *c;
    line[n] = 0;
    logf("NGX: %s", line);
}

}  // namespace

bool built() { return true; }

bool initialize(ID3D12Device* device, const wchar_t* runtime_dir, const wchar_t* data_dir, LogFn log) {
    if (s.initialized) return s.available;
    s.initialized = true;
    s.log = log;
    const wchar_t* paths[] = {runtime_dir};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = &ngx_log;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;  // its errors still come back as results
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.1", data_dir, device,
                                                             &info, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) {
        logf("DLSS: NGX could not be initialized (0x%08X): Ray Reconstruction stays off, the GI's own denoiser runs",
             static_cast<unsigned>(r));
        return false;
    }
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&s.parameters);
    if (NVSDK_NGX_FAILED(r) || !s.parameters) {
        logf("DLSS: NGX's capabilities unavailable (0x%08X): Ray Reconstruction stays off", static_cast<unsigned>(r));
        return false;
    }
    int available = 0, needs_driver = 0;
    unsigned major = 0, minor = 0;
    NVSDK_NGX_Parameter_GetI(s.parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
    NVSDK_NGX_Parameter_GetI(s.parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needs_driver);
    NVSDK_NGX_Parameter_GetUI(s.parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, &major);
    NVSDK_NGX_Parameter_GetUI(s.parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, &minor);
    if (!available) {
        int init_result = 0;
        NVSDK_NGX_Parameter_GetI(s.parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, &init_result);
        if (needs_driver)
            logf("DLSS: Ray Reconstruction is not available (0x%08X, needs driver %u.%u): the GI's own denoiser runs",
                 static_cast<unsigned>(init_result), major, minor);
        else
            logf("DLSS: Ray Reconstruction is not available (0x%08X): the GI's own denoiser runs", static_cast<unsigned>(init_result));
        return false;
    }
    s.available = true;
    logf("DLSS: Ray Reconstruction available (its runtime next to rfgrt_host.exe)");
    return true;
}

const char* mode_name(int mode) {
    static const char* const kNames[kModeCount] = {"off", "DLAA", "quality", "balanced", "performance", "ultra performance"};
    return mode >= 0 && mode < kModeCount ? kNames[mode] : "?";
}

void render_size(unsigned out_width, unsigned out_height, int mode, unsigned& width, unsigned& height) {
    width = out_width;
    height = out_height;
    if (!s.available || mode <= kModeDlaa || mode >= kModeCount || !out_width || !out_height) return;
    if (s.size_out_width == out_width && s.size_out_height == out_height && s.size_mode == mode) {
        width = s.size_width;
        height = s.size_height;
        return;
    }
    unsigned w = 0, h = 0, max_w = 0, max_h = 0, min_w = 0, min_h = 0;
    float sharpness = 0.0f;
    const NVSDK_NGX_Result r = NGX_DLSSD_GET_OPTIMAL_SETTINGS(s.parameters, out_width, out_height, quality_value(mode), &w, &h,
                                                             &max_w, &max_h, &min_w, &min_h, &sharpness);
    if (NVSDK_NGX_FAILED(r) || !w || !h || w > out_width || h > out_height) {
        static const double kScale[kModeCount] = {1.0, 1.0, 2.0 / 3.0, 0.58, 0.5, 1.0 / 3.0};  // NVIDIA's, per axis
        w = unsigned(std::lround(out_width * kScale[mode]));
        h = unsigned(std::lround(out_height * kScale[mode]));
        if (!w) w = 1;
        if (!h) h = 1;
        logf("DLSS: no render size from NGX for %s at %ux%u (0x%08X): %ux%u by NVIDIA's scale", mode_name(mode), out_width,
             out_height, static_cast<unsigned>(r), w, h);
    }
    s.size_out_width = out_width;
    s.size_out_height = out_height;
    s.size_mode = mode;
    s.size_width = width = w;
    s.size_height = height = h;
}

bool evaluate(ID3D12GraphicsCommandList* list, const Inputs& in) {
    if (!s.available || !list || !in.width || !in.height || in.out_width < in.width || in.out_height < in.height) return false;
    if (!s.feature || s.width != in.width || s.height != in.height || s.out_width != in.out_width ||
        s.out_height != in.out_height || s.mode != in.mode) {
        if (s.feature) {
            // the frames that used it are complete: new frame textures, or a new mode (the core waits for them)
            NVSDK_NGX_D3D12_ReleaseFeature(s.feature);
            s.feature = nullptr;
        }
        NVSDK_NGX_DLSSD_Create_Params create{};
        create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
        create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_Linear;
        create.InWidth = in.width;
        create.InHeight = in.height;
        create.InTargetWidth = in.out_width;
        create.InTargetHeight = in.out_height;
        create.InPerfQualityValue = quality_value(in.mode);
        create.InFeatureCreateFlags =
            NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        create.InEnableOutputSubrects = false;
        const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSSD_EXT(list, 1, 1, &s.feature, s.parameters, &create);
        if (NVSDK_NGX_FAILED(r) || !s.feature) {
            logf("DLSS: Ray Reconstruction could not be created at %ux%u to %ux%u (%s, 0x%08X): the GI's own denoiser runs",
                 in.width, in.height, in.out_width, in.out_height, mode_name(in.mode), static_cast<unsigned>(r));
            s.feature = nullptr;
            s.available = false;
            return false;
        }
        s.width = in.width;
        s.height = in.height;
        s.out_width = in.out_width;
        s.out_height = in.out_height;
        s.mode = in.mode;
        logf("DLSS: Ray Reconstruction created: the light traced at %ux%u, out at %ux%u (%s; the irradiance alone, white "
             "albedo)",
             in.width, in.height, in.out_width, in.out_height, mode_name(in.mode));
        return false;  // evaluated from the next frame on
    }
    NVSDK_NGX_D3D12_DLSSD_Eval_Params eval{};
    eval.pInColor = in.color;
    eval.pInOutput = in.output;
    eval.pInDepth = in.depth;
    eval.pInMotionVectors = in.motion;
    eval.pInDiffuseAlbedo = in.diffuse_albedo;
    eval.pInSpecularAlbedo = in.specular_albedo;
    eval.pInNormals = in.normal_roughness;
    eval.pInRoughness = in.normal_roughness;
    eval.InRenderSubrectDimensions.Width = in.width;  // the inputs' top-left part
    eval.InRenderSubrectDimensions.Height = in.height;
    eval.InReset = in.reset ? 1 : 0;
    eval.InJitterOffsetX = in.jitter_x;
    eval.InJitterOffsetY = in.jitter_y;
    eval.InMVScaleX = 1.0f;
    eval.InMVScaleY = 1.0f;
    eval.pInWorldToViewMatrix = const_cast<float*>(in.world_to_view);
    eval.pInViewToClipMatrix = const_cast<float*>(in.view_to_clip);
    eval.InFrameTimeDeltaInMsec = in.frame_ms;
    const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSSD_EXT(list, s.feature, s.parameters, &eval);
    if (NVSDK_NGX_FAILED(r)) {
        if (!s.told_eval_failure) {
            s.told_eval_failure = true;
            logf("DLSS: Ray Reconstruction's evaluation failed (0x%08X)", static_cast<unsigned>(r));
        }
        return false;
    }
    return true;
}

void shutdown() {
    if (s.feature) NVSDK_NGX_D3D12_ReleaseFeature(s.feature);
    s.feature = nullptr;
    if (s.parameters) NVSDK_NGX_D3D12_DestroyParameters(s.parameters);
    s.parameters = nullptr;
    if (s.initialized && s.available) NVSDK_NGX_D3D12_Shutdown1(nullptr);
    s = State{};  // a core started later (path tracing switched on again) initializes NGX again
}

#else

bool built() { return false; }
bool initialize(ID3D12Device*, const wchar_t*, const wchar_t*, LogFn) { return false; }
void render_size(unsigned out_width, unsigned out_height, int, unsigned& width, unsigned& height) {
    width = out_width;
    height = out_height;
}
const char* mode_name(int) { return "unavailable"; }
bool evaluate(ID3D12GraphicsCommandList*, const Inputs&) { return false; }
void shutdown() {}

#endif

}  // namespace rtcore::dlss
