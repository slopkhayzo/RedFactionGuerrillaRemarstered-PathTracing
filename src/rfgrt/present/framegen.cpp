#include "present/framegen.h"

#include "core/log.h"

#if defined(RFGRT_STREAMLINE)

#pragma warning(push, 0)
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>  // (not inline: included here only)
#pragma warning(pop)

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>

namespace framegen {
namespace {

// The NGX project id (as Ray Reconstruction's, rtcore/dlss.cpp).
constexpr char kProjectId[] = "8d2f5b61-3c47-4e9a-b0d4-6a91c2e7f3b8";
// Streamline's lines logged: its first informational ones, its first warnings and errors, then every 1000th of those
// (it can repeat one every frame); DLSS-G's own (its present path) with a budget of their own.
constexpr int kInfoLines = 200, kProblemLines = 100, kProblemEvery = 1000, kFrameGenLines = 300, kFrameGenEvery = 500;
// Without inputs for this many presents (loading screens, the main menu): DLSS-G off until they come again.
constexpr unsigned kIdlePresents = 30;

struct Api {
    HMODULE module = nullptr;
    PFun_slInit* init{};
    PFun_slSetD3DDevice* set_device{};
    PFun_slUpgradeInterface* upgrade{};
    PFun_slIsFeatureSupported* is_supported{};
    PFun_slGetFeatureFunction* get_function{};
    PFun_slGetNewFrameToken* new_frame{};
    PFun_slSetConstants* set_constants{};
    PFun_slSetTagForFrame* set_tag{};
    PFun_slDLSSGSetOptions* dlssg_set_options{};
    PFun_slDLSSGGetState* dlssg_get_state{};
    PFun_slReflexSetOptions* reflex_set_options{};
    PFun_slReflexSleep* reflex_sleep{};
    PFun_slPCLSetMarker* pcl_set_marker{};
};

struct State {
    Api api;
    bool tried = false;
    bool loaded = false;
    bool supported = false;           // DLSS-G on this adapter
    std::atomic<bool> wanted{false};
    bool applied = false;             // the mode last set
    bool applied_once = false;
    std::string unavailable;          // why it cannot run
    unsigned presents_without_inputs = 1000;
    bool rtss_warned = false;
    sl::FrameToken* frame = nullptr;  // the frame the game is on (from just after the previous present)
    uint64_t presents = 0, inputs = 0, input_failures = 0;
    double sleep_ms_sum = 0.0, sleep_ms_max = 0.0;
    uint64_t sleeps = 0;
    // DLSS-G allocates for the size of the frame and of the depth and motion it is turned on with (no dynamic resolution
    // here): those (0: not known then, the first tagged frame's taken), and the latest inputs'
    unsigned on_w = 0, on_h = 0, on_depth_w = 0, on_depth_h = 0;
    unsigned seen_w = 0, seen_h = 0, seen_depth_w = 0, seen_depth_h = 0;
    bool reset_next = false;  // DLSS-G back on for a new size: its first frame without history
    uint64_t resizes = 0;     // times it went off and on again for a new size
    std::atomic<int> info_lines{0}, problem_lines{0}, framegen_lines{0};
} s;

void on_log(sl::LogType type, const char* message) {
    const std::string_view text = message ? message : "";
    std::string line(text);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (type == sl::LogType::eInfo) {
        if (text.find("dlfg") != std::string_view::npos || text.find("dlssg") != std::string_view::npos ||
            text.find("dlss_g") != std::string_view::npos || text.find("DLSSG") != std::string_view::npos ||
            text.find("pacer") != std::string_view::npos) {
            const int n = s.framegen_lines.fetch_add(1);
            if (n < kFrameGenLines || n % kFrameGenEvery == 0) LOG("Streamline (DLSS-G line %d): %s", n + 1, line.c_str());
            return;
        }
        if (s.info_lines.fetch_add(1) < kInfoLines) LOG("Streamline: %s", line.c_str());
        return;
    }
    const int n = s.problem_lines.fetch_add(1);
    if (n >= kProblemLines && n % kProblemEvery != 0) return;
    LOG("Streamline %s%s: %s", type == sl::LogType::eError ? "error" : "warning",
        n >= kProblemLines ? " (repeated)" : "", line.c_str());
}

template <typename T>
bool exported(const char* name, T*& function) {
    function = reinterpret_cast<T*>(GetProcAddress(s.api.module, name));
    if (function) return true;
    s.unavailable = std::string("Streamline's sl.interposer.dll has no ") + name + " (an unexpected version)";
    LOG("frame generation: %s: unavailable", s.unavailable.c_str());
    return false;
}

template <typename T>
bool feature_function(sl::Feature feature, const char* name, T*& function) {
    void* address = nullptr;
    const sl::Result r = s.api.get_function(feature, name, address);
    function = reinterpret_cast<T*>(address);
    if (r == sl::Result::eOk && function) return true;
    s.unavailable = std::string("Streamline has no ") + name;
    LOG("frame generation: %s (result %d): unavailable", s.unavailable.c_str(), int(r));
    return false;
}

std::string unsupported_reason(sl::Result r) {
    switch (r) {
        case sl::Result::eErrorOSDisabledHWS:
            return "hardware-accelerated GPU scheduling is off (Windows Settings, System, Display, Graphics); DLSS Frame "
                   "Generation needs it";
        case sl::Result::eErrorNoSupportedAdapterFound:
        case sl::Result::eErrorAdapterNotSupported: return "DLSS Frame Generation needs an NVIDIA GeForce RTX 40 or 50 series GPU";
        case sl::Result::eErrorDriverOutOfDate: return "the NVIDIA driver is too old for DLSS Frame Generation";
        case sl::Result::eErrorOSOutOfDate: return "Windows is too old for DLSS Frame Generation (Windows 10 20H1 or later)";
        default: {
            char text[96];
            snprintf(text, sizeof(text), "DLSS Frame Generation is not supported here (Streamline result %d)", int(r));
            return text;
        }
    }
}

std::wstring plugin_folder() {
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH]{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&plugin_folder), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring folder = path;
    const size_t slash = folder.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : folder.substr(0, slash);
}

void to_matrix(const float m[16], sl::float4x4& out) {
    for (uint32_t row = 0; row < 4; ++row) out.setRow(row, sl::float4(m[row * 4], m[row * 4 + 1], m[row * 4 + 2], m[row * 4 + 3]));
}

// c = a * b (row-major 4x4).
void multiply(const float a[16], const float b[16], float c[16]) {
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a[r * 4 + k] * b[k * 4 + col];
            c[r * 4 + col] = sum;
        }
}

// A clip-space position of one frame into the other's: back to the first frame's camera-relative world, moved by the
// cameras' offset (sign 1: into the previous frame's, -1: from it), through the other's view and projection.
void clip_to_clip(const float inv_view_projection[16], const double delta[3], double sign, const float view_projection[16],
                  sl::float4x4& out) {
    float move[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    move[12] = float(sign * delta[0]);
    move[13] = float(sign * delta[1]);
    move[14] = float(sign * delta[2]);
    float world[16], result[16];
    multiply(inv_view_projection, move, world);
    multiply(world, view_projection, result);
    to_matrix(result, out);
}

void apply_reflex() {
    sl::ReflexOptions reflex{};
    reflex.mode = sl::ReflexMode::eLowLatency;
    const sl::Result r = s.api.reflex_set_options(reflex);
    if (r != sl::Result::eOk) LOG("frame generation: slReflexSetOptions failed (result %d)", int(r));
}

void apply_mode() {
    const bool on = s.wanted.load() && s.supported && s.presents_without_inputs < kIdlePresents;
    if (s.applied_once && on == s.applied) return;
    sl::DLSSGOptions options{};
    options.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    options.numFramesToGenerate = 1;
    // full-screen menus over the frame (the UI layer covers the screen): DLSS-G pauses itself
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff | sl::DLSSGFlags::eEnableFullscreenMenuDetection;
    // the sizes its inputs have had (it allocates for them now; unknown yet: for the first tagged frame's)
    if (on && s.seen_w && s.seen_depth_w) {
        options.colorWidth = s.seen_w;
        options.colorHeight = s.seen_h;
        options.mvecDepthWidth = s.seen_depth_w;
        options.mvecDepthHeight = s.seen_depth_h;
    }
    const sl::Result r = s.api.dlssg_set_options(sl::ViewportHandle(0u), options);
    if (r != sl::Result::eOk) LOG("frame generation: slDLSSGSetOptions(%s) failed (result %d)", on ? "on" : "off", int(r));
    if (on) {
        s.on_w = s.seen_w;
        s.on_h = s.seen_h;
        s.on_depth_w = s.seen_depth_w;
        s.on_depth_h = s.seen_depth_h;
    }
    s.applied = on;
    s.applied_once = true;
    LOG("frame generation: DLSS-G %s", on ? "on (one generated frame between each two)"
                                       : s.wanted.load() ? "paused (no inputs: a loading screen or a menu)" : "off");
    if (on && !s.rtss_warned && GetModuleHandleW(L"RTSSHooks64.dll")) {
        s.rtss_warned = true;
        LOG("frame generation: RivaTuner Statistics Server is hooking the game: its on-screen display, drawn into every "
            "frame DLSS-G presents, costs frame generation most of its frame rate; set its application detection level to "
            "None for the game");
    }
}

// The next frame's token as the game starts it (after the previous present): Reflex's pacing sleep, then the
// simulation's start marker (the game's whole frame up to its present, unmarked within).
void start_frame() {
    if (s.api.new_frame(s.frame, nullptr) != sl::Result::eOk) {
        s.frame = nullptr;
        return;
    }
    LARGE_INTEGER a{}, b{}, f{};
    QueryPerformanceCounter(&a);
    s.api.reflex_sleep(*s.frame);
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    const double ms = double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart);
    s.sleep_ms_sum += ms;
    if (ms > s.sleep_ms_max) s.sleep_ms_max = ms;
    ++s.sleeps;
    s.api.pcl_set_marker(sl::PCLMarker::eSimulationStart, *s.frame);
}

}  // namespace

bool load(ID3D12Device* device) {
    if (s.tried) return s.loaded;
    s.tried = true;
    const std::wstring folder = plugin_folder();
    const std::wstring interposer = folder + L"\\sl.interposer.dll";
    if (GetFileAttributesW(interposer.c_str()) == INVALID_FILE_ATTRIBUTES) {
        s.unavailable = "NVIDIA Streamline's files are not in the plugin's folder (sl.interposer.dll and the rest)";
        LOG("frame generation: %s: unavailable", s.unavailable.c_str());
        return false;
    }
    if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
        s.unavailable = "sl.interposer.dll is not signed by NVIDIA";
        LOG("frame generation: %s: unavailable", s.unavailable.c_str());
        return false;
    }
    s.api.module = LoadLibraryExW(interposer.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!s.api.module) {
        s.unavailable = "sl.interposer.dll could not be loaded";
        LOG("frame generation: %s (error %lu): unavailable", s.unavailable.c_str(), GetLastError());
        return false;
    }
    if (!exported("slInit", s.api.init) || !exported("slSetD3DDevice", s.api.set_device) ||
        !exported("slUpgradeInterface", s.api.upgrade) || !exported("slIsFeatureSupported", s.api.is_supported) ||
        !exported("slGetFeatureFunction", s.api.get_function) || !exported("slGetNewFrameToken", s.api.new_frame) ||
        !exported("slSetConstants", s.api.set_constants) || !exported("slSetTagForFrame", s.api.set_tag))
        return false;
    const wchar_t* paths[] = {folder.c_str()};
    const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences preferences{};
    preferences.logLevel = sl::LogLevel::eDefault;
    preferences.pathsToPlugins = paths;
    preferences.numPathsToPlugins = 1;
    preferences.logMessageCallback = &on_log;
    // no downloads: the pinned, signed plugins only
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                        sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = uint32_t(std::size(features));
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "rfgrt";
    preferences.projectId = kProjectId;
    preferences.renderAPI = sl::RenderAPI::eD3D12;
    sl::Result r = s.api.init(preferences, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        s.unavailable = "NVIDIA Streamline could not start (slInit)";
        LOG("frame generation: %s (result %d): unavailable", s.unavailable.c_str(), int(r));
        return false;
    }
    r = s.api.set_device(device);
    if (r != sl::Result::eOk) {
        s.unavailable = "NVIDIA Streamline refused the device (slSetD3DDevice)";
        LOG("frame generation: %s (result %d): unavailable", s.unavailable.c_str(), int(r));
        return false;
    }
    if (!feature_function(sl::kFeatureReflex, "slReflexSetOptions", s.api.reflex_set_options) ||
        !feature_function(sl::kFeatureReflex, "slReflexSleep", s.api.reflex_sleep) ||
        !feature_function(sl::kFeaturePCL, "slPCLSetMarker", s.api.pcl_set_marker))
        return false;
    apply_reflex();
    LUID luid = device->GetAdapterLuid();
    sl::AdapterInfo adapter{};
    adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
    adapter.deviceLUIDSizeInBytes = sizeof(luid);
    r = s.api.is_supported(sl::kFeatureDLSS_G, adapter);
    s.supported = r == sl::Result::eOk && feature_function(sl::kFeatureDLSS_G, "slDLSSGSetOptions", s.api.dlssg_set_options) &&
                  feature_function(sl::kFeatureDLSS_G, "slDLSSGGetState", s.api.dlssg_get_state);
    if (r != sl::Result::eOk) s.unavailable = unsupported_reason(r);
    s.loaded = true;
    LOG("frame generation: Streamline loaded from %ls (manual hooking, Reflex low latency); DLSS-G %s%s", folder.c_str(),
        s.supported ? "supported on this adapter" : "not supported: ", s.supported ? "" : s.unavailable.c_str());
    return true;
}

bool loaded() { return s.loaded; }

bool upgrade(void** interface_pointer) {
    if (!s.loaded || !interface_pointer || !*interface_pointer) return false;
    const sl::Result r = s.api.upgrade(interface_pointer);
    if (r != sl::Result::eOk) {
        LOG("frame generation: slUpgradeInterface failed (result %d)", int(r));
        return false;
    }
    return true;
}

void set_wanted(bool on) { s.wanted.store(on); }
bool enabled() { return s.loaded && s.supported && s.wanted.load(); }
bool generating() { return s.loaded && s.applied; }
const char* unavailable() { return s.unavailable.c_str(); }

void begin_frame(bool has_inputs) {
    if (!s.loaded) return;
    s.presents_without_inputs = has_inputs ? 0 : (s.presents_without_inputs < 1000 ? s.presents_without_inputs + 1 : 1000);
    if (s.supported) apply_mode();
    if (!s.frame) start_frame();  // the first frame through the presenter
    if (s.frame) {
        s.api.pcl_set_marker(sl::PCLMarker::eSimulationEnd, *s.frame);
        s.api.pcl_set_marker(sl::PCLMarker::eRenderSubmitStart, *s.frame);
    }
}

bool set_inputs(ID3D12GraphicsCommandList* list, const Camera& c, const Inputs& in) {
    if (!enabled() || !s.frame || !list || !in.depth || !in.motion || !in.hudless || !in.ui || !in.width || !in.height)
        return false;
    sl::Constants k{};
    to_matrix(c.projection, k.cameraViewToClip);
    to_matrix(c.inv_projection, k.clipToCameraView);
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    to_matrix(identity, k.clipToLensClip);
    clip_to_clip(c.inv_view_projection, c.delta, 1.0, c.prev_view_projection, k.clipToPrevClip);
    clip_to_clip(c.prev_inv_view_projection, c.delta, -1.0, c.view_projection, k.prevClipToClip);
    // the presented frame is the game's (not jittered); the motion in pixels of its own size, the camera's included
    const unsigned depth_width = in.depth_width ? in.depth_width : in.width;
    const unsigned depth_height = in.depth_height ? in.depth_height : in.height;
    // A new size of the frame, or of the depth and motion (the game's DLSS mode changed while playing: its scene drawn at
    // another size), while DLSS-G runs: it would go on with what it allocated for the old size, its frames made with
    // the motion read wrongly until it was turned off and on again (a level's load). As NVIDIA's guide asks (DLSS-G off
    // when the resolution changes): off now with its resources freed, on again at the next present for this size.
    s.seen_w = in.width;
    s.seen_h = in.height;
    s.seen_depth_w = depth_width;
    s.seen_depth_h = depth_height;
    if (s.applied && !s.on_w) {  // turned on before any inputs: it takes these
        s.on_w = in.width;
        s.on_h = in.height;
        s.on_depth_w = depth_width;
        s.on_depth_h = depth_height;
    } else if (s.applied && (in.width != s.on_w || in.height != s.on_h || depth_width != s.on_depth_w || depth_height != s.on_depth_h)) {
        sl::DLSSGOptions off{};
        off.mode = sl::DLSSGMode::eOff;
        off.flags = sl::DLSSGFlags::eEnableFullscreenMenuDetection;  // (not retained: freed)
        const sl::Result r = s.api.dlssg_set_options(sl::ViewportHandle(0u), off);
        LOG("frame generation: its inputs' size changed (the frame %ux%u, the depth and motion %ux%u; they were %ux%u and "
            "%ux%u): DLSS-G off, its resources freed (result %d), on again at the next present for the new size",
            in.width, in.height, depth_width, depth_height, s.on_w, s.on_h, s.on_depth_w, s.on_depth_h, int(r));
        s.applied = false;
        s.reset_next = true;
        ++s.resizes;
        return false;
    }
    k.jitterOffset = sl::float2(0.0f, 0.0f);
    k.mvecScale = sl::float2(1.0f / float(depth_width), 1.0f / float(depth_height));
    k.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    k.cameraPos = sl::float3(0.0f, 0.0f, 0.0f);  // camera-relative
    k.cameraRight = sl::float3(c.right[0], c.right[1], c.right[2]);
    k.cameraUp = sl::float3(c.up[0], c.up[1], c.up[2]);
    k.cameraFwd = sl::float3(c.forward[0], c.forward[1], c.forward[2]);
    k.cameraNear = c.near_clip;
    k.cameraFar = c.far_clip;
    k.cameraFOV = c.fov_y;
    k.cameraAspectRatio = c.aspect;
    k.depthInverted = c.depth_inverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    k.cameraMotionIncluded = sl::Boolean::eTrue;
    k.motionVectors3D = sl::Boolean::eFalse;
    k.reset = c.reset || s.reset_next ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    s.reset_next = false;
    k.orthographicProjection = sl::Boolean::eFalse;
    k.motionVectorsDilated = sl::Boolean::eFalse;
    k.motionVectorsJittered = sl::Boolean::eFalse;
    if (s.api.set_constants(k, *s.frame, sl::ViewportHandle(0u)) != sl::Result::eOk) {
        ++s.input_failures;
        return false;
    }
    sl::Resource depth(sl::ResourceType::eTex2d, in.depth, nullptr, nullptr, uint32_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    sl::Resource motion(sl::ResourceType::eTex2d, in.motion, nullptr, nullptr, uint32_t(D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    sl::Resource hudless(sl::ResourceType::eTex2d, in.hudless, nullptr, nullptr, uint32_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    sl::Resource ui(sl::ResourceType::eTex2d, in.ui, nullptr, nullptr, uint32_t(D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    const sl::Extent extent{0, 0, in.width, in.height};
    const sl::Extent depth_extent{0, 0, depth_width, depth_height};
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &depth_extent),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &depth_extent),
        sl::ResourceTag(&hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &extent),
        sl::ResourceTag(&ui, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eOnlyValidNow, &extent),
    };
    const sl::Result r = s.api.set_tag(*s.frame, sl::ViewportHandle(0u), tags, uint32_t(std::size(tags)),
                                       reinterpret_cast<sl::CommandBuffer*>(list));
    if (r != sl::Result::eOk) {
        if (++s.input_failures <= 5) LOG("frame generation: slSetTagForFrame failed (result %d)", int(r));
        return false;
    }
    ++s.inputs;
    return true;
}

void before_present() {
    if (!s.loaded || !s.frame) return;
    s.api.pcl_set_marker(sl::PCLMarker::eRenderSubmitEnd, *s.frame);
    s.api.pcl_set_marker(sl::PCLMarker::ePresentStart, *s.frame);
}

void after_present() {
    if (!s.loaded) return;
    if (s.frame) {
        s.api.pcl_set_marker(sl::PCLMarker::ePresentEnd, *s.frame);
        ++s.presents;
    }
    start_frame();
}

void report(double seconds) {
    if (!s.loaded) return;
    char state[256] = "not supported";
    if (s.supported) {
        sl::DLSSGState g{};
        const sl::Result r = s.api.dlssg_get_state(sl::ViewportHandle(0u), g, nullptr);
        if (r != sl::Result::eOk)
            snprintf(state, sizeof(state), "state unknown (result %d)", int(r));
        else
            snprintf(state, sizeof(state),
                     "status %u, %u frames shown per game frame lately, up to %u generated a frame, about %llu MB",
                     unsigned(g.status), g.numFramesActuallyPresented, g.numFramesToGenerateMax,
                     static_cast<unsigned long long>(g.estimatedVRAMUsageInBytes >> 20));
    }
    LOG("frame generation over %.1f s: %llu presents through Streamline, %llu with the inputs (%llu refused); Reflex slept "
        "%.2f ms avg / %.1f max a frame; DLSS-G %s (%s); the depth and motion %ux%u, off and on again for a new size %llu "
        "times so far",
        seconds, static_cast<unsigned long long>(s.presents), static_cast<unsigned long long>(s.inputs),
        static_cast<unsigned long long>(s.input_failures), s.sleeps ? s.sleep_ms_sum / double(s.sleeps) : 0.0,
        s.sleep_ms_max, s.applied ? "on" : "off", state, s.seen_depth_w, s.seen_depth_h,
        static_cast<unsigned long long>(s.resizes));
    s.presents = s.inputs = s.input_failures = s.sleeps = 0;
    s.sleep_ms_sum = s.sleep_ms_max = 0.0;
}

void suspend() {
    s.presents_without_inputs = 1000;
    if (!s.loaded || !s.supported || !s.applied) return;
    sl::DLSSGOptions options{};
    options.mode = sl::DLSSGMode::eOff;
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    const sl::Result r = s.api.dlssg_set_options(sl::ViewportHandle(0u), options);
    if (r != sl::Result::eOk) LOG("frame generation: slDLSSGSetOptions(off) failed (result %d)", int(r));
    s.applied = false;
    LOG("frame generation: DLSS-G off (the presenter gave the game's presents back)");
}

}  // namespace framegen

#else  // built without the Streamline SDK

namespace framegen {

bool load(ID3D12Device*) { return false; }
bool loaded() { return false; }
bool upgrade(void**) { return false; }
void set_wanted(bool) {}
bool enabled() { return false; }
bool generating() { return false; }
const char* unavailable() { return "this build has no Streamline SDK (tools/fetch_streamline.ps1)"; }
void begin_frame(bool) {}
bool set_inputs(ID3D12GraphicsCommandList*, const Camera&, const Inputs&) { return false; }
void before_present() {}
void after_present() {}
void report(double) {}
void suspend() {}

}  // namespace framegen

#endif
