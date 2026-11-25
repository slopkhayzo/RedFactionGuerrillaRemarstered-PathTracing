#include "rtcore.h"

#include <dxgi1_6.h>
#include <d3d12.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "depth_compare_cs.h"  // g_depth_compare_cs (dxc, cs_6_5)
#include "depth_pattern_cs.h"  // g_depth_pattern_cs (dxc, cs_6_0)
#include "sun_shadow_cs.h"     // g_sun_shadow_cs (dxc, cs_6_5)
#include "sun_shadow_filter_cs.h"  // g_sun_shadow_filter_cs (dxc, cs_6_5)
#include "sun_shadow_temporal_cs.h"  // g_sun_shadow_temporal_cs (dxc, cs_6_5)
#include "gi_filter_cs.h"    // g_gi_filter_cs (dxc, cs_6_0)
#include "gi_atrous_cs.h"    // g_gi_atrous_cs (dxc, cs_6_0)
#include "gi_temporal_cs.h"  // g_gi_temporal_cs (dxc, cs_6_0)
#include "gi_trace_cs.h"     // g_gi_trace_cs (dxc, cs_6_5)
#include "light_shadow_cs.h"  // g_light_shadow_cs (dxc, cs_6_5)
#include "reflect_trace_cs.h"  // g_reflect_trace_cs (dxc, cs_6_5)
#include "cache_update_cs.h"   // g_cache_update_cs (dxc, cs_6_5)
#include "cache_resolve_cs.h"  // g_cache_resolve_cs (dxc, cs_6_5)
#include "sky_update_cs.h"     // g_sky_update_cs (dxc, cs_6_5)
#include "sky_resolve_cs.h"    // g_sky_resolve_cs (dxc, cs_6_5)
#include "probe_cs.h"          // g_probe_cs (dxc, cs_6_5)
#include "rr_guides_cs.h"      // g_rr_guides_cs (dxc, cs_6_0)
#include "rr_finalize_cs.h"    // g_rr_finalize_cs (dxc, cs_6_0)
#include "glass_light_cs.h"    // g_glass_light_cs (dxc, cs_6_5)
#include "glass_filter_cs.h"   // g_glass_filter_cs (dxc, cs_6_5)
#include "sun_tint_cs.h"       // g_sun_tint_cs (dxc, cs_6_5)
#include "fog_inject_cs.h"     // g_fog_inject_cs (dxc, cs_6_5)
#include "fog_integrate_cs.h"  // g_fog_integrate_cs (dxc, cs_6_5)
#include "fog_apply_cs.h"      // g_fog_apply_cs (dxc, cs_6_5)
#include "dlss.h"
#include "frame_scene.h"
#include "scene.h"
#include "skinning.h"
#include "tlas.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")

namespace rtcore {
namespace {

constexpr int kRing = 3;            // frames in flight: command allocators, readback slots
constexpr uint32_t kCounts = 512;   // per slot: 5 pixel classes (of 8), 16 depth-ratio buckets, 8 spare, 32 samples x 3;
                                    // with GI, the GI census from uint 100 (gi_trace.hlsl, Telemetry.GiCensus)
constexpr uint32_t kStamps = 12;    // timestamps per slot: TLAS start and end, pass start and end, GI, lights, reflections,
                                    // the fog's start and end; in the GI's: the rays' start, the glass's, the sun tint's
// The volumetric fog's grid of froxels (shaders/fog_common.hlsli): columns across the screen (rows by its aspect), slices
// in depth from kFogNear metres to the camera's far plane (50 to 2000 m), the most frames its light is averaged over.
constexpr uint32_t kFogColumns = 160, kFogSlices = 64;
constexpr float kFogNear = 0.5f, kFogFrames = 20.0f;
constexpr uint32_t kReadback = kStamps * 8 + kCounts;  // per slot: the timestamps, then the counts

const RunParams* g_p = nullptr;

void logf(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (g_p && g_p->log) g_p->log(line);
}

int fail(const char* what, HRESULT hr) {
    logf("%s failed 0x%08lX", what, static_cast<unsigned long>(hr));
    if (g_p && g_p->block) {
        snprintf(g_p->block->host_message, sizeof(g_p->block->host_message), "%s failed 0x%08lX", what,
                 static_cast<unsigned long>(hr));
        g_p->block->host_state = rfgrt_ipc::kHostFailed;
    }
    return 1;
}

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// OpenRFG64-memtest: a lost device. A fence of a removed device reads UINT64_MAX, and every call on it fails or does
// nothing. The core used to go on regardless: in a campaign session (2026-11-22) the device was lost after a 4 s hang,
// and the core kept recording a frame every 5 ms into it, its allocations all failing, no frame answered, while the
// game's process grew by 11 GB in two minutes, to unplayable paging. Now it logs why and stops; the plugin then takes
// the game back to its own lighting (Link::monitor_main).
const char* removed_reason_text(HRESULT hr) {
    switch (hr) {
        case DXGI_ERROR_DEVICE_HUNG: return "the GPU hung on one of its commands (the driver reset it)";
        case DXGI_ERROR_DEVICE_RESET: return "the device was reset (a badly formed command)";
        case DXGI_ERROR_DEVICE_REMOVED: return "the device was removed (a page fault, or the driver went away)";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "the driver failed";
        case DXGI_ERROR_INVALID_CALL: return "an invalid call";
        default: return "no reason given";
    }
}

const char* breadcrumb_op_name(D3D12_AUTO_BREADCRUMB_OP op) {
    switch (op) {
        case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
        case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
        case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
        case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
        case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE: return "WriteBufferImmediate";
        case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BuildRaytracingAccelerationStructure";
        case D3D12_AUTO_BREADCRUMB_OP_EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO:
            return "EmitRaytracingAccelerationStructurePostbuildInfo";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE: return "CopyRaytracingAccelerationStructure";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DispatchRays";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEMETACOMMAND: return "ExecuteMetaCommand";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEEXTENSIONCOMMAND: return "ExecuteExtensionCommand";
        default: return nullptr;
    }
}

std::string narrow_name(const char* a, const wchar_t* w) {
    if (a && *a) return a;
    if (w && *w) {
        char s[128];
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s, sizeof(s), nullptr, nullptr);
        s[sizeof(s) - 1] = 0;
        return s;
    }
    return "(unnamed)";
}

// What Direct3D 12 kept of the lost device (Host.Dred): each command list the GPU had not finished, with the
// operations around the last one it completed, and the allocations at the page fault's address.
void log_removed_device_data(ID3D12Device* dev) {
    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dred)))) {
        logf("lost device: no removed-device data (Host.Dred off, or this Windows has none)");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs))) {
        int lists = 0, unfinished = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* node = crumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
            ++lists;
            const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            if (done >= node->BreadcrumbCount) continue;  // finished
            if (++unfinished > 8) continue;
            const std::string queue_name = narrow_name(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW);
            const std::string list_name = narrow_name(node->pCommandListDebugNameA, node->pCommandListDebugNameW);
            std::string ops;
            const UINT from = done > 6 ? done - 6 : 0;
            const UINT to = done + 4 < node->BreadcrumbCount ? done + 4 : node->BreadcrumbCount;
            for (UINT i = from; i < to; ++i) {
                const char* op = breadcrumb_op_name(node->pCommandHistory[i]);
                char item[96];
                if (op)
                    snprintf(item, sizeof(item), "%s%s%s", i == from ? "" : ", ", i == done ? ">>" : "", op);
                else
                    snprintf(item, sizeof(item), "%s%sop %d", i == from ? "" : ", ", i == done ? ">>" : "",
                             static_cast<int>(node->pCommandHistory[i]));
                ops += item;
            }
            logf("lost device: queue \"%s\", command list \"%s\": %u of its %u operations done; around the first not done "
                 "(>>): %s",
                 queue_name.c_str(), list_name.c_str(), done, node->BreadcrumbCount, ops.c_str());
        }
        logf("lost device: %d command lists in the breadcrumbs, %d of them not finished", lists, unfinished);
    } else {
        logf("lost device: no breadcrumbs");
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault))) {
        if (fault.PageFaultVA == 0) {
            logf("lost device: no page fault");
        } else {
            logf("lost device: page fault at GPU address 0x%llX", static_cast<unsigned long long>(fault.PageFaultVA));
            int n = 0;
            for (const D3D12_DRED_ALLOCATION_NODE* a = fault.pHeadExistingAllocationNode; a && n < 8; a = a->pNext, ++n)
                logf("lost device:   there now: \"%s\" (allocation type %d)", narrow_name(a->ObjectNameA, a->ObjectNameW).c_str(),
                     static_cast<int>(a->AllocationType));
            n = 0;
            for (const D3D12_DRED_ALLOCATION_NODE* a = fault.pHeadRecentFreedAllocationNode; a && n < 8; a = a->pNext, ++n)
                logf("lost device:   freed there recently: \"%s\" (allocation type %d)",
                     narrow_name(a->ObjectNameA, a->ObjectNameW).c_str(), static_cast<int>(a->AllocationType));
        }
    }
    dred->Release();
}

// True once the device is gone (logged once, with the reason and the removed-device data, and the core marked failed).
bool device_lost(ID3D12Device* dev, UINT64 fence_value) {
    if (fence_value != UINT64_MAX) return false;
    const HRESULT reason = dev->GetDeviceRemovedReason();
    if (SUCCEEDED(reason)) return false;
    logf("lost device: 0x%08lX, %s", static_cast<unsigned long>(reason), removed_reason_text(reason));
    log_removed_device_data(dev);
    if (g_p && g_p->block) {
        snprintf(g_p->block->host_message, sizeof(g_p->block->host_message),
                 "the GPU device was lost (0x%08lX, %s): the game's own lighting from now on",
                 static_cast<unsigned long>(reason), removed_reason_text(reason));
        g_p->block->host_state = rfgrt_ipc::kHostFailed;
    }
    return true;
}

HANDLE to_handle(uint64_t v) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(v)); }

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    return br;
}

D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    br.UAV.pResource = r;
    return br;
}

bool peer_alive(const RunParams& p) { return !p.peer_process || WaitForSingleObject(p.peer_process, 0) != WAIT_OBJECT_0; }

ID3D12Resource* make_buffer(ID3D12Device* dev, D3D12_HEAP_TYPE heap, uint64_t size, D3D12_RESOURCE_FLAGS flags,
                            D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{heap};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

ID3D12RootSignature* make_root_signature(ID3D12Device* dev, const D3D12_ROOT_SIGNATURE_DESC& d) {
    ID3DBlob *blob = nullptr, *err = nullptr;
    ID3D12RootSignature* rs = nullptr;
    if (SUCCEEDED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
        dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
    release(blob);
    release(err);
    return rs;
}

// The plugin's per-frame textures of one generation (rfgrt_ipc.h).
struct FrameSet {
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* result = nullptr;
    ID3D12Resource* game_clb = nullptr;  // the game's ClbTexture at the seam
    ID3D12Resource* vis = nullptr;       // ours: this frame's traced visibility, sun and ambient
    ID3D12Resource* history[2] = {};     // ours: its accumulation over frames (sun, ambient, frames), ping-pong
    ID3D12Resource* prev_depth = nullptr;  // ours: the previous frame's depth, for the reprojection
    ID3D12Resource* irradiance = nullptr;  // the plugin's: the GI the game's patched lit shaders read (null: no GI)
    ID3D12Resource* gi_traced = nullptr;   // ours: this frame's traced irradiance
    ID3D12Resource* gi_history[2] = {};    // ours: its accumulation over frames, ping-pong
    ID3D12Resource* gi_scratch = nullptr;  // ours: the wavelet filter's passes, with gi_traced
    // ours, with GI.Denoiser=2 (dlss.h): Ray Reconstruction's guide images (linear depth, motion, normal + roughness,
    // albedo, a black specular albedo) and its output
    ID3D12Resource *rr_depth = nullptr, *rr_motion = nullptr, *rr_normal = nullptr, *rr_albedo = nullptr, *rr_spec = nullptr,
                   *rr_out = nullptr;
    ID3D12Resource* light_masks[rfgrt_ipc::kMaxLightMasks] = {};  // the plugin's: the dynamic lights' shadow masks
    ID3D12Resource* reflection = nullptr;  // the plugin's: the reflections the patched lit shaders read (null: none)
    ID3D12Resource* refl_traced = nullptr;
    ID3D12Resource* refl_history[2] = {};
    ID3D12Resource* glass = nullptr;  // the plugin's: the light of the nearest glass in front of each pixel (null: none)
    ID3D12Resource* glass_raw = nullptr;  // ours: its traced light before the filter (glass_light.hlsl, RGBA32F)
    ID3D12Resource* glass_prev = nullptr;  // ours: glass_raw as the previous frame left it (the history it reprojects)
    ID3D12Resource* glass_refl = nullptr;  // the plugin's: what the clear glass reflects (glass_light.hlsl; null: none)
    ID3D12Resource* sun_tint = nullptr;    // the plugin's: the sun through clear glass (sun_tint.hlsl; null: none)
    ID3D12Resource* fog = nullptr;        // the plugin's: the volumetric fog in front of each pixel (null: none)
    ID3D12Resource* fog_vis[2] = {};      // ours: the froxels' sun and sky visibility over frames (fog_inject.hlsl), ping-pong
    ID3D12Resource* fog_ratio = nullptr;  // ours: the fog's light along each column of froxels (fog_integrate.hlsl)
    uint32_t fog_x = 0, fog_y = 0;        // froxels across the screen
    int32_t generation = 0;
    uint32_t width = 0, height = 0;
};

// The depth-compare pass's root constants (shaders/depth_compare.hlsl, cbuffer Frame).
struct CompareConstants {
    float right[4], up[4], forward[4], origin[4];
    float proj[4];  // P00, P11, P22, P32
    float range[4]; // x: the main scene's depth range end
    float sun[4];   // xyz: rl_light::m_light_direction, w: 1 when there is a sun
    uint32_t width, height;
    float tolerance;
    uint32_t flags;
    uint32_t shadow[4];  // rays per pixel, tan of the sun's angular radius (float bits), filter radius, frame
    float reproject[16];  // render-offset space now -> the previous frame's clip space (row-vector)
    float prev[4];        // the previous frame's P22, P32, depth range end; the history's length in frames
};

// The GI passes' constant buffer (shaders/gi_*.hlsl, cbuffer Frame): the frame's constants, then the game's lighting.
struct GiConstants {
    CompareConstants base;
    float ambient[4], back_ambient[4], sun_color[4];  // the game's, in its shaders' units (frame_scene.h)
    float gi[4];                                        // rays a pixel, their length, the albedo they meet, max frames
    float gi2[4];                                       // x: the bounced light's scale, y: the sky's, z: GI.SunBounceAlbedo
    float world[4];                                     // xyz: the render offset; w: 1 with the radiance cache
    float sky_probe[4];  // x: 1 with the drawn sky; y: the cosine of the angle about the sun it leaves out; z: GI.SkyDrawn
    float fog[4];        // the game's fog: its density a, height falloff b, start c (fogDistance's y, x, w); w: 1
    float fog_color[4];  // rgb: the game's fogColor; w: the sun's share of the fog's light (Fog.SunShare)
    float fog2[4];       // x: the scattering's anisotropy (Fog.Anisotropy); y, z: the grid's near and far depth; w: slices
    float fog3[4];       // x, y: froxels across the screen; z: 1 when the previous frame's grid is there; w: frames averaged
    float glass[4];      // the clear glass's reflections: x its reflectance at normal incidence; y 1 when they are on
    float probe[4];      // xy: where the probe aims, fractions of the screen (Test.ProbePixel)
    float terrain[4];    // rgb: the albedo of a surface whose material is not known, the terrain (GI.TerrainAlbedo)
    // DLSS (dlss.h) upscaling: xy the render size the GI's rays and Ray Reconstruction's guides run at (0: the frame's),
    // the top-left part of their textures; zw the sample's offset from each render pixel's centre (render pixels, y down):
    // each traces from the frame's pixel under that point
    float upscale[4];
};
static_assert(sizeof(GiConstants) <= 512, "a 512-byte slot a ring slot");

// The dynamic lights' shadow pass's constant buffer (shaders/light_shadow.hlsl): the frame's, then the lights.
struct LightConstants {
    CompareConstants base;
    uint32_t light_info[4];  // x: lights
    rfgrt_frame::Light lights[rfgrt_frame::kMaxLights];
};
static_assert(sizeof(LightConstants) <= 1024, "a 1 KB slot a ring slot");

// a * b, row-major 4x4 (row-vector convention: a applies first)
void mul44(const float* a, const float* b, float* out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c] +
                             a[r * 4 + 3] * b[3 * 4 + c];
}

// The Halton sequence's i-th point in base `base` (i >= 1), in [0, 1): DLSS's sub-pixel jitter (bases 2 and 3).
float halton(uint32_t i, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    for (; i > 0; i /= base) {
        f /= float(base);
        r += f * float(i % base);
    }
    return r;
}

// The descriptor heap: groups of the frame textures, one table each (w: the history written this frame, 0 or 1).
//   0..2          pattern and depth compare: depth SRV, result UAV, the game's ClbTexture SRV
//   3..5          sun shadow trace:          depth SRV, visibility UAV, the game's ClbTexture SRV
//   6+5w..10+5w   temporal:                  visibility SRV, depth SRV, previous depth SRV, history[1-w] SRV, history[w] UAV
//   16+4w..19+4w  filter:                    history[w] SRV, depth SRV, the game's ClbTexture SRV, result UAV
//   24..87        the geometry pool's pages: raw buffer SRVs (null where there is no page yet)
//   88..93        GI trace:                  depth SRV (and 4 more, unused), traced UAV
//   94+6w..99+6w  GI temporal:               traced SRV, depth SRV, previous depth SRV, gi_history[1-w] SRV, (unused), gi_history[w] UAV
//   106+6w..      GI filter:                 gi_history[w] SRV, depth SRV (and 3 unused), irradiance UAV
//   118..134      the dynamic lights' shadows: depth SRV, the 16 masks' UAVs (null where there is none)
//   135..140      reflections' trace:        as GI's (depth, the lit colour, previous depth, depth x2; traced UAV)
//   141+6w..      reflections' temporal:     as GI's, with their own textures
//   153+6w..      reflections' filter:       as GI's, into the reflection texture
//   165..194      GI's wavelet passes:       input SRV, depth SRV x4, output UAV: [w] gi_history[w] -> gi_traced,
//                                            [2] gi_traced -> gi_scratch, [3] gi_scratch -> gi_traced, [4] gi_traced -> irradiance
//   195..199      Ray Reconstruction guides: depth SRV, rr_depth, rr_motion, rr_normal, rr_albedo UAVs
//   200..205      its finalize pass:         rr_out SRV, depth SRV x4, irradiance UAV
//   206..211      the glass's light:         as GI's trace (depth, the lit colour, previous depth), glass_prev (t3), depth,
//                                            glass_raw UAV
//   212..217      its filter:                depth SRV x3, glass_raw SRV (t3), depth SRV, the glass UAV
//   218+6w..      the fog's froxels:         depth, depth, previous depth, fog_vis[1-w] SRV (t3), depth; fog_vis[w] UAV
//   230+6w..      their integration:         depth, depth, previous depth, fog_vis[w] SRV (t3), depth; fog_ratio UAV
//   242..247      the fog per pixel:         depth, depth, previous depth, fog_ratio SRV (t3), depth; the fog UAV
//   248           what the clear glass reflects: its UAV (u7)
//   249           the sun through clear glass: its UAV (u7, sun_tint.hlsl)
//   250..         the materials' textures (scene.h, kMaxTextures: t0.. in space2), null until each arrives
constexpr UINT kTableCompare = 0, kTableTrace = 3, kTableTemporal = 6, kTableFilter = 16, kGeoPages = 24, kMaxGeoPages = 64,
               kTableGiTrace = 88, kTableGiTemporal = 94, kTableGiFilter = 106, kTableLights = 118, kTableReflTrace = 135,
               kTableReflTemporal = 141, kTableReflFilter = 153, kTableGiAtrous = 165, kTableRrGuides = 195,
               kTableRrFinalize = 200, kTableGlass = 206, kTableGlassFilter = 212, kTableFogInject = 218,
               kTableFogIntegrate = 230, kTableFogApply = 242, kTableGlassRefl = 248, kTableSunTint = 249, kTableTextures = 250,
               kDescriptors = kTableTextures + rfgrt_scene::kMaxTextures;

// The game's projection (rl_camera::m_projection_transform): row-vector (P23 = 1, P32 at [14]) or column-vector
// (P32 = 1 at [14], the depth term at [11]); either way P00, P11 and P22 are on the diagonal.
void projection_terms(const float* m, float out[4], bool* reversed) {
    out[0] = m[0];
    out[1] = m[5];
    out[2] = m[10];
    out[3] = std::fabs(m[11]) == 1.0f && m[15] == 0.0f ? m[14] : m[11];
    *reversed = out[3] > 0.0f;  // depth falls with distance (near 1, far 0)
}

struct Stats {
    double tlas_us = 0, pass_us = 0, gi_us = 0, light_us = 0, refl_us = 0, fog_us = 0;
    double prep_us = 0, trace_us = 0, glass_us = 0, tint_us = 0, denoise_us = 0;  // the GI's parts (timestamps 9-11)
    uint64_t frames = 0, instances = 0, in_tlas = 0;
    uint64_t classes[5] = {};
    uint64_t ratio[16] = {};
    uint64_t shadow[9] = {};  // sun shadows: 0-3, 8 against the game's cascades (sun_shadow_filter.hlsl), 4-5 the history
    uint64_t glass_px = 0, glass_lit = 0;  // the glass's light (glass_light.hlsl): pixels with glass in front, in the sun
    float samples[96] = {};  // the last frame's 32 sample pixels: game depth value, mirror view depth, game view depth
    void clear() { *this = Stats{}; }
};

}  // namespace

int run(const RunParams& p) {
    g_p = &p;
    rfgrt_ipc::Block* b = p.block;
    if (!b || b->magic != rfgrt_ipc::kMagic || b->version != rfgrt_ipc::kVersion || b->block_bytes != sizeof(rfgrt_ipc::Block))
        return fail("protocol check", E_INVALIDARG);
    uint8_t* region = reinterpret_cast<uint8_t*>(b);
    const uint8_t* ring = region + b->ring_offset;

    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return fail("CreateDXGIFactory2", hr);
    LUID luid{};
    luid.LowPart = b->adapter_luid_low;
    luid.HighPart = b->adapter_luid_high;
    IDXGIAdapter1* adapter = nullptr;
    if (FAILED(hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) return fail("EnumAdapterByLuid", hr);
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    // Host.Dred: set before the device exists (it applies to the devices this process creates from now on; the game's
    // own device is Direct3D 11's).
    bool dredOn = false;
    if (b->dred) {
        ID3D12DeviceRemovedExtendedDataSettings* dredSettings = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings)))) {
            dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dredSettings->Release();
            dredOn = true;
        }
    }
    ID3D12Device* dev = nullptr;
    if (FAILED(hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) return fail("D3D12CreateDevice", hr);
    logf("removed-device data (Host.Dred): %s", dredOn ? "on (breadcrumbs, page faults)" : b->dred ? "not available" : "off");
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
    logf("D3D12 device on %s (%llu MB), ray tracing tier %d (%s process)", name,
         static_cast<unsigned long long>(ad.DedicatedVideoMemory >> 20), static_cast<int>(o5.RaytracingTier),
         sizeof(void*) == 8 ? "64-bit" : "32-bit");
    const bool snapshots = b->snapshots != 0;
    int mode = b->mode;
    const bool traced = mode == rfgrt_ipc::kModeDepthCompare || mode == rfgrt_ipc::kModeSunShadows;
    if (traced && (!snapshots || o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)) {
        logf("mode %d needs frame scenes and ray tracing tier 1.1: depth pattern instead", mode);
        mode = rfgrt_ipc::kModeDepthPattern;
    }

    // Fences and the scene event. The handles the plugin gave us are ours to close once opened.
    ID3D12Fence *fenceA = nullptr, *fenceB = nullptr;
    hr = dev->OpenSharedHandle(to_handle(b->fence_a_handle), IID_PPV_ARGS(&fenceA));
    CloseHandle(to_handle(b->fence_a_handle));
    if (FAILED(hr)) return fail("open fence A", hr);
    if (FAILED(hr = dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fenceB)))) return fail("create fence B", hr);
    HANDLE fenceBHandle = nullptr;
    if (FAILED(hr = dev->CreateSharedHandle(fenceB, nullptr, GENERIC_ALL, nullptr, &fenceBHandle))) return fail("fence B handle", hr);
    if (!p.publish_fence_b || !p.publish_fence_b(fenceBHandle, b, p.publish_ctx))
        return fail("handing fence B to the plugin", HRESULT_FROM_WIN32(GetLastError()));
    HANDLE sceneEvent = to_handle(b->scene_event_handle);

    // Pass 1, the depth pattern: depth (t0) -> result (u0); frame, mode, width, height as root constants.
    D3D12_DESCRIPTOR_RANGE patternRanges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                               {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER patternParams[2]{};
    patternParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    patternParams[0].DescriptorTable = {2, patternRanges};
    patternParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    patternParams[1].Constants.Num32BitValues = 4;
    ID3D12RootSignature* patternRs = make_root_signature(dev, {2, patternParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    // The traced passes (depth compare, sun shadows): TLAS (root SRV t0); depth (t1), result (u0) and the game's
    // ClbTexture (t2) in the table; counters (root UAV u1); constants (b0); the TLAS's instance table (root SRV t3: which
    // glass is translucent, rt_query.hlsli).
    D3D12_DESCRIPTOR_RANGE compareRanges[3] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0},
                                               {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1},
                                               {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, 2}};
    D3D12_ROOT_PARAMETER compareParams[5]{};
    compareParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    compareParams[0].DescriptorTable = {3, compareRanges};
    compareParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    compareParams[1].Descriptor = {0, 0};
    compareParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    compareParams[2].Descriptor = {1, 0};
    compareParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    compareParams[3].Constants = {0, 0, sizeof(CompareConstants) / 4};
    compareParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    compareParams[4].Descriptor = {3, 0};
    ID3D12RootSignature* compareRs = make_root_signature(dev, {5, compareParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    // The sun shadow's filter: visibility, depth and the game's ClbTexture (t0-t2) and result (u0) in the table,
    // counters (root UAV u1), constants (b0).
    D3D12_DESCRIPTOR_RANGE filterRanges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0},
                                              {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3}};
    D3D12_ROOT_PARAMETER filterParams[3]{};
    filterParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    filterParams[0].DescriptorTable = {2, filterRanges};
    filterParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    filterParams[1].Descriptor = {1, 0};
    filterParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    filterParams[2].Constants = {0, 0, sizeof(CompareConstants) / 4};
    ID3D12RootSignature* filterRs = make_root_signature(dev, {3, filterParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    // The sun shadow's accumulation over frames: visibility, depth, previous depth and previous history (t0-t3) and
    // history (u0) in the table, counters (root UAV u1), constants (b0).
    D3D12_DESCRIPTOR_RANGE temporalRanges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0},
                                                {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 4}};
    D3D12_ROOT_PARAMETER temporalParams[3]{};
    temporalParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    temporalParams[0].DescriptorTable = {2, temporalRanges};
    temporalParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    temporalParams[1].Descriptor = {1, 0};
    temporalParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    temporalParams[2].Constants = {0, 0, sizeof(CompareConstants) / 4};
    ID3D12RootSignature* temporalRs = make_root_signature(dev, {3, temporalParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    if (!patternRs || !compareRs || !filterRs || !temporalRs) return fail("root signatures", E_FAIL);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = patternRs;
    pd.CS = {g_depth_pattern_cs, sizeof(g_depth_pattern_cs)};
    ID3D12PipelineState *patternPso = nullptr, *comparePso = nullptr, *filterPso = nullptr, *temporalPso = nullptr;
    ID3D12RootSignature* giRs = nullptr;
    ID3D12PipelineState *giTracePso = nullptr, *giTemporalPso = nullptr, *giFilterPso = nullptr, *giAtrousPso = nullptr;
    ID3D12PipelineState* glassPso = nullptr;  // the glass's light (glass_light.hlsl), with the GI root signature
    ID3D12PipelineState* glassFilterPso = nullptr;  // and its filter (glass_filter.hlsl)
    ID3D12PipelineState* sunTintPso = nullptr;  // the sun through clear glass (sun_tint.hlsl, GI.WindowTint), the GI root signature
    // the volumetric fog (fog_inject.hlsl, fog_integrate.hlsl, fog_apply.hlsl), with the GI root signature
    ID3D12PipelineState *fogInjectPso = nullptr, *fogIntegratePso = nullptr, *fogApplyPso = nullptr;
    // The world radiance cache (shaders/radiance_cache.hlsli): keys, this frame's sums, values; kCacheSlots each.
    constexpr uint32_t kCacheSlots = 1u << 20;
    ID3D12PipelineState *cacheUpdatePso = nullptr, *cacheResolvePso = nullptr;
    ID3D12Resource *cacheKeys = nullptr, *cacheAccum = nullptr, *cacheValue = nullptr;
    bool cacheOn = false;  // GI.Cache
    // The drawn sky (shaders/sky_probe.hlsli): sums and values of a 32 x 32 octahedral map, the mean after them.
    constexpr uint32_t kSkyTexels = 32 * 32;
    ID3D12PipelineState *skyUpdatePso = nullptr, *skyResolvePso = nullptr;
    ID3D12Resource *skyAccum = nullptr, *skyValue = nullptr;
    bool skyOn = false;  // GI.SkyDrawn > 0
    // The crosshair probe (Test.ProbeKey, shaders/probe.hlsl): the frame each ring slot probes (0: none), with the frame's
    // instances (mesh id, submesh, kind each) to name what the rays met.
    ID3D12PipelineState* probePso = nullptr;
    // DLSS Ray Reconstruction (GI.Denoiser=2, dlss.h): on when NGX has it
    ID3D12RootSignature* rrGuidesRs = nullptr;
    ID3D12PipelineState *rrGuidesPso = nullptr, *rrFinalizePso = nullptr;
    bool rrOn = false;
    int32_t rrGeneration = -1;  // the frame textures Ray Reconstruction last ran with (new ones: no history)
    LARGE_INTEGER rrLast{}, rrFreq{};
    uint64_t rrFrames = 0;
    // DLSS (dlss.h, Block::dlss_mode): the mode the frames are queued with (-1 before the first), what the log last said
    int dlssMode = -1;
    int dlssLoggedMode = -2;
    unsigned dlssLoggedWidth = 0, dlssLoggedHeight = 0;
    bool dlssReset = false;  // Ray Reconstruction's history starts over (a new mode)
    int64_t probeFrame[kRing]{};
    std::vector<uint32_t> probeInstances[kRing];
    int32_t probeSeen = b->probe_request;
    ID3D12Resource* giCb = nullptr;  // upload: kRing x 512 bytes of GiConstants
    uint8_t* giCbCpu = nullptr;
    ID3D12PipelineState* reflTracePso = nullptr;
    ID3D12Resource* reflCb = nullptr;  // upload: kRing x 512 bytes of GiConstants (the reflections')
    uint8_t* reflCbCpu = nullptr;
    ID3D12RootSignature* lightRs = nullptr;
    ID3D12PipelineState* lightPso = nullptr;
    ID3D12Resource* lightCb = nullptr;  // upload: kRing x 1 KB of LightConstants
    uint8_t* lightCbCpu = nullptr;
    if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&patternPso)))) return fail("pattern pipeline", hr);
    if (mode == rfgrt_ipc::kModeDepthCompare) {
        pd.pRootSignature = compareRs;
        pd.CS = {g_depth_compare_cs, sizeof(g_depth_compare_cs)};
        if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&comparePso)))) return fail("depth compare pipeline", hr);
    } else if (mode == rfgrt_ipc::kModeSunShadows) {
        pd.pRootSignature = compareRs;
        pd.CS = {g_sun_shadow_cs, sizeof(g_sun_shadow_cs)};
        if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&comparePso)))) return fail("sun shadow pipeline", hr);
        pd.pRootSignature = filterRs;
        pd.CS = {g_sun_shadow_filter_cs, sizeof(g_sun_shadow_filter_cs)};
        if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&filterPso)))) return fail("sun shadow filter pipeline", hr);
        pd.pRootSignature = temporalRs;
        pd.CS = {g_sun_shadow_temporal_cs, sizeof(g_sun_shadow_temporal_cs)};
        if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&temporalPso)))) return fail("sun shadow temporal pipeline", hr);
        logf("sun shadows: %d rays a pixel and frame over a sun of %.2f degrees, averaged over up to %d frames, filter radius %d; "
             "ambient occlusion: %d rays a pixel and frame out to %.2f m",
             b->shadow_rays, b->shadow_sun_angle_deg, b->shadow_history, b->shadow_filter_radius, b->ao_rays, b->ao_radius);
        if (b->gi_rays > 0 || b->refl_enabled > 0) {
            // GI (and reflections, its passes reused): five SRVs and a UAV in a table, the TLAS (t5) and the instance table (t6) as root SRVs, the constants
            // as a root CBV (b0), and the geometry pool's pages (t0-t63, space1) in a table.
            D3D12_DESCRIPTOR_RANGE giRanges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 5}};
            D3D12_DESCRIPTOR_RANGE pageRange = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaxGeoPages, 0, 1, 0};
            D3D12_ROOT_PARAMETER gp[15]{};
            gp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            gp[0].DescriptorTable = {2, giRanges};
            gp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            gp[1].Descriptor = {5, 0};
            gp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            gp[2].Descriptor = {6, 0};
            gp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            gp[3].Descriptor = {0, 0};
            gp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            gp[4].DescriptorTable = {1, &pageRange};
            // root UAVs: the world radiance cache (shaders/radiance_cache.hlsli) u1-u3, the drawn sky (sky_probe.hlsli) u4-u5,
            // the frame's counters (u6: the crosshair probe's answer, shaders/probe.hlsl)
            for (UINT u = 0; u < 6; ++u) {
                gp[5 + u].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
                gp[5 + u].Descriptor = {1 + u, 0};
            }
            gp[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b1: the wavelet pass's step, and last
            gp[11].Constants = {1, 0, 2};
            // u7: what the clear glass reflects (glass_light.hlsl), in a table of its own (the other passes leave it unset)
            D3D12_DESCRIPTOR_RANGE glassReflRange = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 7, 0, 0};
            gp[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            gp[12].DescriptorTable = {1, &glassReflRange};
            // the hits' materials (trace_common.hlsli hit_surface): the meshes' material slots (root SRV t7), their
            // textures (t0.. in space2, a table)
            gp[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            gp[13].Descriptor = {7, 0};
            D3D12_DESCRIPTOR_RANGE textureRange = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, rfgrt_scene::kMaxTextures, 0, 2, 0};
            gp[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            gp[14].DescriptorTable = {1, &textureRange};
            // s0: linear, clamped (the fog reads its froxels between their centres); s1: linear, wrapping (the materials'
            // textures)
            D3D12_STATIC_SAMPLER_DESC samplers[2]{};
            samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[0].ShaderRegister = 0;
            samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            samplers[1] = samplers[0];
            samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samplers[1].ShaderRegister = 1;
            giRs = make_root_signature(dev, {15, gp, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE});
            if (!giRs) return fail("GI root signature", E_FAIL);
            pd.pRootSignature = giRs;
            pd.CS = {g_gi_trace_cs, sizeof(g_gi_trace_cs)};
            if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&giTracePso)))) return fail("GI trace pipeline", hr);
            pd.CS = {g_gi_temporal_cs, sizeof(g_gi_temporal_cs)};
            if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&giTemporalPso)))) return fail("GI temporal pipeline", hr);
            pd.CS = {g_gi_filter_cs, sizeof(g_gi_filter_cs)};
            if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&giFilterPso)))) return fail("GI filter pipeline", hr);
            pd.CS = {g_gi_atrous_cs, sizeof(g_gi_atrous_cs)};
            if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&giAtrousPso)))) return fail("GI wavelet pipeline", hr);
            pd.CS = {g_glass_light_cs, sizeof(g_glass_light_cs)};
            if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&glassPso)))) glassPso = nullptr;
            pd.CS = {g_glass_filter_cs, sizeof(g_glass_filter_cs)};
            if (glassPso && FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&glassFilterPso)))) {
                glassFilterPso = nullptr;
                release(glassPso);
            }
            logf("glass: %s", glassPso ? "lit by the path tracer at the nearest glass in front of each pixel (the patched glass shader's t17)"
                                       : "its own light (no pipeline)");
            logf("glass: the translucent (patterned) glass %s", b->frosted_glass
                     ? "diffuses the light through it (GI.FrostedGlass): no direct light through it; its panes glow toward the "
                       "rays with the light of their far side, through their texture"
                     : "lets a share of the sun through each pane (GI.FrostedGlass=0)");
            if (b->window_tint) {
                pd.CS = {g_sun_tint_cs, sizeof(g_sun_tint_cs)};
                if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&sunTintPso)))) sunTintPso = nullptr;
                logf("sun through clear glass: %s", sunTintPso ? "tinted by each pane's texel the ray crosses (GI.WindowTint; the lit "
                                                                 "shaders' t20)"
                                                               : "untinted (no pipeline)");
            }
            if (glassPso && b->refl_glass)
                logf("glass: the clear glass reflects (Reflections.Glass): a mirror ray off it, weighted by Schlick's Fresnel term "
                     "from %.3f at normal incidence (Reflections.GlassReflectance), faded by the game's fog (the patched glass "
                     "shader's t19)", b->refl_glass_f0);
            if (b->fog_enabled && b->gi_rays > 0) {
                pd.CS = {g_fog_inject_cs, sizeof(g_fog_inject_cs)};
                if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&fogInjectPso)))) fogInjectPso = nullptr;
                pd.CS = {g_fog_integrate_cs, sizeof(g_fog_integrate_cs)};
                if (fogInjectPso && FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&fogIntegratePso)))) fogIntegratePso = nullptr;
                pd.CS = {g_fog_apply_cs, sizeof(g_fog_apply_cs)};
                if (fogIntegratePso && FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&fogApplyPso)))) fogApplyPso = nullptr;
                if (!fogApplyPso) {
                    release(fogInjectPso);
                    release(fogIntegratePso);
                    logf("fog: the game's own (no pipeline for the volumetric fog)");
                } else {
                    logf("fog: volumetric (Fog.Enabled): the game's fog lit where the sun and the sky reach it, %u froxels across "
                         "the screen, %u slices from %.1f m to the far plane, averaged over up to %.0f frames; the sun's share %.2f, "
                         "anisotropy %.2f",
                         kFogColumns, kFogSlices, kFogNear, kFogFrames, b->fog_sun_share, b->fog_anisotropy);
                if (b->fog_density != 1.0f) logf("fog: its density %g times the game's (Test.FogDensity)", b->fog_density);
                }
            }
            // The world radiance cache: made with the GI root signature whatever GI.Cache says (its root UAVs are then
            // always bound), filled and read only with it (GiConstants::world[3]).
            pd.CS = {g_cache_update_cs, sizeof(g_cache_update_cs)};
            if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&cacheUpdatePso)))) return fail("cache update pipeline", hr);
            pd.CS = {g_cache_resolve_cs, sizeof(g_cache_resolve_cs)};
            if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&cacheResolvePso)))) return fail("cache resolve pipeline", hr);
            // zeroed as committed resources are: every slot free
            cacheKeys = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, uint64_t(kCacheSlots) * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cacheAccum = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, uint64_t(kCacheSlots) * 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cacheValue = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, uint64_t(kCacheSlots) * 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!cacheKeys || !cacheAccum || !cacheValue) return fail("the radiance cache", E_OUTOFMEMORY);
            cacheOn = b->gi_cache != 0;
            pd.CS = {g_sky_update_cs, sizeof(g_sky_update_cs)};
            if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&skyUpdatePso)))) return fail("sky update pipeline", hr);
            pd.CS = {g_sky_resolve_cs, sizeof(g_sky_resolve_cs)};
            if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&skyResolvePso)))) return fail("sky resolve pipeline", hr);
            skyAccum = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, uint64_t(kSkyTexels) * 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            skyValue = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, uint64_t(kSkyTexels + 1) * 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!skyAccum || !skyValue) return fail("the sky map", E_OUTOFMEMORY);
            skyOn = b->gi_sky_drawn > 0.0f;
            pd.CS = {g_probe_cs, sizeof(g_probe_cs)};
            if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&probePso)))) return fail("probe pipeline", hr);
            if (b->gi_rays > 0 && b->gi_denoiser == 2) {
                // DLSS Ray Reconstruction (dlss.h): its guide pass's root signature (depth, the four guide images, the
                // constants), its finalize pass (on the GI's), NGX with its runtime next to this executable
                D3D12_DESCRIPTOR_RANGE rrRanges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, 1}};
                D3D12_ROOT_PARAMETER rp[2]{};
                rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                rp[0].DescriptorTable = {2, rrRanges};
                rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                rp[1].Descriptor = {0, 0};
                rrGuidesRs = make_root_signature(dev, {2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
                if (rrGuidesRs) {
                    pd.pRootSignature = rrGuidesRs;
                    pd.CS = {g_rr_guides_cs, sizeof(g_rr_guides_cs)};
                    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&rrGuidesPso)))) rrGuidesPso = nullptr;
                    pd.pRootSignature = giRs;
                    pd.CS = {g_rr_finalize_cs, sizeof(g_rr_finalize_cs)};
                    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&rrFinalizePso)))) rrFinalizePso = nullptr;
                }
                wchar_t exeDir[MAX_PATH] = {}, dataDir[MAX_PATH] = {};
                GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
                if (wchar_t* slash = wcsrchr(exeDir, L'\\')) *slash = 0;
                wcsncpy_s(dataDir, b->log_path[0] ? b->log_path : exeDir, _TRUNCATE);  // NGX's logs beside ours
                if (b->log_path[0])
                    if (wchar_t* slash = wcsrchr(dataDir, L'\\')) *slash = 0;
                rrOn = rrGuidesPso && rrFinalizePso && dlss::initialize(dev, exeDir, dataDir, p.log);
                logf("GI denoiser: %s", rrOn ? "DLSS Ray Reconstruction (GI.Denoiser=2)"
                                        : dlss::built() ? "the wavelet filter (Ray Reconstruction unavailable here)"
                                                        : "the wavelet filter (this build has no DLSS SDK)");
            }
            pd.pRootSignature = giRs;
            logf("sky: rays that get out see %s", skyOn ? "the sky the game draws, learnt from the pixels showing it (GI.SkyDrawn)"
                                                       : "the game's ambient colour (GI.SkyDrawn=0)");
            logf("radiance cache: %s, %u slots (%.0f MB) for the light of surfaces seen on screen, by place and facing",
                 cacheOn ? "on" : "off (GI.Cache=0)", kCacheSlots, kCacheSlots * 36.0 / 1048576.0);
            giCb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, uint64_t(kRing) * 512, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
            D3D12_RANGE none{0, 0};
            if (!giCb || FAILED(giCb->Map(0, &none, reinterpret_cast<void**>(&giCbCpu)))) return fail("GI constants", E_OUTOFMEMORY);
            if (b->refl_enabled > 0) {
                pd.CS = {g_reflect_trace_cs, sizeof(g_reflect_trace_cs)};
                if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&reflTracePso)))) return fail("reflection pipeline", hr);
                reflCb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, uint64_t(kRing) * 512, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
                D3D12_RANGE rnone{0, 0};
                if (!reflCb || FAILED(reflCb->Map(0, &rnone, reinterpret_cast<void**>(&reflCbCpu))))
                    return fail("reflection constants", E_OUTOFMEMORY);
                logf("reflections: one ray a pixel and frame out to %.0f m over a lobe of exponent %.0f, averaged over up to %d "
                     "frames, filter radius %d",
                     b->refl_length, b->refl_gloss, b->refl_history, b->refl_filter_radius);
            }
            logf("global illumination: %d rays a pixel and frame out to %.0f m, albedo %.2f where they hit, averaged over up to %d "
                 "frames, filter radius %d",
                 b->gi_rays, b->gi_length, b->gi_albedo, b->gi_history, b->gi_filter_radius);
        }
        if (b->light_masks > 0) {
            // The dynamic lights' shadows: depth (t0) and the masks (u0-u15) in a table, the TLAS (t5) as a root SRV,
            // the constants and the lights as a root CBV (b0).
            D3D12_DESCRIPTOR_RANGE lightRanges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                                     {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, rfgrt_ipc::kMaxLightMasks, 0, 0, 1}};
            D3D12_ROOT_PARAMETER lp[3]{};
            lp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            lp[0].DescriptorTable = {2, lightRanges};
            lp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            lp[1].Descriptor = {5, 0};
            lp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            lp[2].Descriptor = {0, 0};
            lightRs = make_root_signature(dev, {3, lp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
            if (!lightRs) return fail("light shadow root signature", E_FAIL);
            pd.pRootSignature = lightRs;
            pd.CS = {g_light_shadow_cs, sizeof(g_light_shadow_cs)};
            if (FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&lightPso)))) return fail("light shadow pipeline", hr);
            lightCb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, uint64_t(kRing) * 1024, D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
            D3D12_RANGE none{0, 0};
            if (!lightCb || FAILED(lightCb->Map(0, &none, reinterpret_cast<void**>(&lightCbCpu))))
                return fail("light shadow constants", E_OUTOFMEMORY);
            logf("dynamic lights: shadow masks for up to %d lights a frame, one ray a pixel and light", b->light_masks);
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kDescriptors, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ID3D12DescriptorHeap* heap = nullptr;
    if (FAILED(hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return fail("descriptor heap", hr);
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // The geometry pool's pages: raw views, null until a page exists (rays read the triangles they hit from them).
    D3D12_SHADER_RESOURCE_VIEW_DESC rawSrv{};
    rawSrv.Format = DXGI_FORMAT_R32_TYPELESS;
    rawSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    rawSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    rawSrv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    for (UINT i = 0; i < kMaxGeoPages; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE hnd = heap->GetCPUDescriptorHandleForHeapStart();
        hnd.ptr += SIZE_T(kGeoPages + i) * inc;
        dev->CreateShaderResourceView(nullptr, &rawSrv, hnd);
    }
    size_t geoPagesViewed = 0;
    {  // the materials' textures: null views until each arrives (scene.cpp add_texture)
        D3D12_SHADER_RESOURCE_VIEW_DESC nullTex{};
        nullTex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullTex.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullTex.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullTex.Texture2D.MipLevels = 1;
        for (UINT i = 0; i < rfgrt_scene::kMaxTextures; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE hnd = heap->GetCPUDescriptorHandleForHeapStart();
            hnd.ptr += SIZE_T(kTableTextures + i) * inc;
            dev->CreateShaderResourceView(nullptr, &nullTex, hnd);
        }
    }
    {  // the light masks' UAVs: null until a set of frame textures has them
        D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav{};
        nullUav.Format = DXGI_FORMAT_R8G8_UNORM;
        nullUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        for (UINT i = 0; i <= rfgrt_ipc::kMaxLightMasks; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE hnd = heap->GetCPUDescriptorHandleForHeapStart();
            hnd.ptr += SIZE_T(kTableLights + i) * inc;
            if (i == 0) {
                D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
                nullSrv.Format = DXGI_FORMAT_R32_FLOAT;
                nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                nullSrv.Texture2D.MipLevels = 1;
                dev->CreateShaderResourceView(nullptr, &nullSrv, hnd);
            } else {
                dev->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, hnd);
            }
        }
    }

    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_COMPUTE};
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return fail("compute queue", hr);
    queue->SetName(L"rtcore frames");
    ID3D12CommandAllocator* alloc[kRing]{};
    for (auto& a : alloc) dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&a));
    ID3D12GraphicsCommandList4* cl = nullptr;
    if (FAILED(hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, alloc[0], nullptr, IID_PPV_ARGS(&cl))))
        return fail("command list", hr);
    cl->SetName(L"rtcore frame");  // named in the removed-device data
    cl->Close();
    D3D12_QUERY_HEAP_DESC qhd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kRing * kStamps, 0};
    ID3D12QueryHeap* qheap = nullptr;
    dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qheap));
    ID3D12Resource* readback = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, kRing * kReadback, D3D12_RESOURCE_FLAG_NONE,
                                           D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* counts = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, kRing * kCounts, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!readback || !counts) return fail("readback buffers", E_OUTOFMEMORY);
    UINT64 tsFreq = 1;
    queue->GetTimestampFrequency(&tsFreq);
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    Scene scene;
    {
        D3D12_CPU_DESCRIPTOR_HANDLE textures = heap->GetCPUDescriptorHandleForHeapStart();
        textures.ptr += SIZE_T(kTableTextures) * inc;
        scene.set_texture_table(textures, inc);
    }
    scene.init(dev, b, ring, o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0, p.log);
    ID3D12Device5* dev5 = nullptr;
    Tlas tlas;
    const bool use_tlas = snapshots && o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0 &&
                          SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev5))) &&
                          tlas.init(dev5, b->max_instances, kRing, p.log, b->tlas_fast_trace != 0);
    if (snapshots && !use_tlas) logf("no TLAS this session (no ray tracing, or its buffers failed)");
    if (use_tlas) logf("TLAS: rebuilt every frame, for %s (Mirror.TlasFastTrace)", b->tlas_fast_trace ? "fast tracing" : "a fast build");
    Skinning skinning;
    if (use_tlas) skinning.init(dev5, kRing, b->max_skins, b->max_bones, b->max_instances, p.log);

    b->host_state = rfgrt_ipc::kHostReady;
    logf("ready: mode %d (%s), scene ring %llu MB, %s", mode,
         mode == rfgrt_ipc::kModeDepthCompare ? "depth compare" : mode == rfgrt_ipc::kModeSunShadows ? "sun shadows" : "depth pattern",
         static_cast<unsigned long long>(b->ring_bytes >> 20),
         snapshots ? "frames follow the plugin's frame scenes" : "frames are queued ahead of the plugin");

    FrameSet set;
    // The sun shadow's history: which of the two this frame writes, whether the other holds the previous frame's,
    // and the camera that frame was seen with.
    int histOut = 0;
    bool historyValid = false;
    // The fog's froxels: which of the two this frame writes, whether the other holds the previous frame's grid
    int fogOut = 0;
    bool fogHistory = false;
    uint64_t fogFrames = 0;
    float prevView[16] = {}, prevProj[16] = {}, prevOffset[3] = {}, prevRange = 1.0f;
    int64_t n = 0;               // the last frame taken (queued, or its scene skipped)
    int64_t sig = 0;             // the last frame queued: fence B reaches it when the GPU has run it
    int64_t slotFrame[kRing]{};  // the frame each ring slot was last used for
    uint64_t queued = 0;         // frames queued, for the ring slot
    Stats st;
    bool loggedProjection = false;
    CompareConstants lastCamera{};
    GiConstants lastGi{};
    uint64_t giFrames = 0;
    // Telemetry.GiCensus (RFGRT_TELEMETRY_GICENSUS=1, read here from the environment: a diagnostic): gi_trace.hlsl's census
    // of the traced light by the kind of surface, five classes of five sums from the frame's counter 100, summed over the
    // log's period.
    const bool giCensus = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("RFGRT_TELEMETRY_GICENSUS", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    uint64_t census[25] = {};
    uint64_t lightFrames = 0, lightsTraced = 0;
    // The game's lit colour (GI's hits read it where the game drew them in the previous frame)
    ID3D12Resource* radiance = nullptr;
    int32_t radianceGen = 0;
    uint64_t radianceFrames = 0;
    uint64_t reflFrames = 0;
    float lastOffset[3] = {};
    float worldBasis[4][3] = {};  // right, up, forward, position from rl_camera's world matrix, for the log
    float lastView[16] = {};
    float lastStateView[16] = {}, lastStateProj[16] = {}, lastProj[16] = {};
    ULONGLONG lastLog = GetTickCount64();
    uint64_t torn = 0, skippedNoSet = 0;
    bool lost = false;
    while (!b->stop && peer_alive(p)) {
        if (b->test_remove_device == 1 && dev5) {
            b->test_remove_device = 2;
            logf("test: the device removed (Test.KillHostAtSeconds)");
            dev5->RemoveDevice();
        }
        const UINT64 done = fenceB->GetCompletedValue();
        if (device_lost(dev, done)) {
            lost = true;
            break;
        }
        scene.set_frames(sig, done);
        skinning.collect(n, done);
        bool did = scene.pump();
        UINT64 waitFor = 0;
        const int k = int(queued % kRing);

        // The frame to queue: the newest published scene (snapshots), or the next number (queued ahead).
        int64_t next = 0;
        if (snapshots) {
            const int64_t pub = rfgrt_ipc::load64(&b->frame_published);
            if (pub > n) next = pub;
        } else if (b->frame_generation != 0) {
            next = n + 1;
        }
        // A new set of frame textures: every frame queued with the current one finishes first.
        const int32_t gen = b->frame_generation;
        if (next && gen != 0 && gen != set.generation && next >= rfgrt_ipc::load64(&b->generation_first_frame)) {
            if (sig > 0 && done < UINT64(sig)) {
                waitFor = UINT64(sig);
                next = 0;
            } else {
                release(set.depth);
                release(set.result);
                release(set.game_clb);
                release(set.vis);
                release(set.history[0]);
                release(set.history[1]);
                release(set.prev_depth);
                release(set.irradiance);
                release(set.gi_traced);
                release(set.gi_scratch);
                for (ID3D12Resource** r : {&set.rr_depth, &set.rr_motion, &set.rr_normal, &set.rr_albedo, &set.rr_spec, &set.rr_out})
                    release(*r);
                release(set.gi_history[0]);
                release(set.gi_history[1]);
                for (auto& m : set.light_masks) release(m);
                release(set.reflection);
                release(set.refl_traced);
                release(set.refl_history[0]);
                release(set.refl_history[1]);
                release(set.glass);
                release(set.glass_raw);
                release(set.glass_prev);
                release(set.glass_refl);
                release(set.sun_tint);
                release(set.fog);
                release(set.fog_vis[0]);
                release(set.fog_vis[1]);
                release(set.fog_ratio);
                historyValid = false;
                fogHistory = false;
                hr = dev->OpenSharedHandle(to_handle(b->depth_handle), IID_PPV_ARGS(&set.depth));
                if (SUCCEEDED(hr)) hr = dev->OpenSharedHandle(to_handle(b->result_handle), IID_PPV_ARGS(&set.result));
                if (SUCCEEDED(hr)) hr = dev->OpenSharedHandle(to_handle(b->game_clb_handle), IID_PPV_ARGS(&set.game_clb));
                CloseHandle(to_handle(b->depth_handle));
                CloseHandle(to_handle(b->result_handle));
                CloseHandle(to_handle(b->game_clb_handle));
                if (b->irradiance_handle) {
                    if (SUCCEEDED(hr) && giRs) hr = dev->OpenSharedHandle(to_handle(b->irradiance_handle), IID_PPV_ARGS(&set.irradiance));
                    CloseHandle(to_handle(b->irradiance_handle));
                }
                if (b->reflection_handle) {
                    if (SUCCEEDED(hr) && reflTracePso) hr = dev->OpenSharedHandle(to_handle(b->reflection_handle), IID_PPV_ARGS(&set.reflection));
                    CloseHandle(to_handle(b->reflection_handle));
                }
                if (b->glass_handle) {
                    if (SUCCEEDED(hr) && glassPso) hr = dev->OpenSharedHandle(to_handle(b->glass_handle), IID_PPV_ARGS(&set.glass));
                    CloseHandle(to_handle(b->glass_handle));
                }
                if (b->glass_refl_handle) {
                    if (SUCCEEDED(hr) && glassPso)
                        hr = dev->OpenSharedHandle(to_handle(b->glass_refl_handle), IID_PPV_ARGS(&set.glass_refl));
                    CloseHandle(to_handle(b->glass_refl_handle));
                }
                if (b->sun_tint_handle) {
                    if (SUCCEEDED(hr) && sunTintPso)
                        hr = dev->OpenSharedHandle(to_handle(b->sun_tint_handle), IID_PPV_ARGS(&set.sun_tint));
                    CloseHandle(to_handle(b->sun_tint_handle));
                }
                if (b->fog_handle) {
                    if (SUCCEEDED(hr) && fogApplyPso) hr = dev->OpenSharedHandle(to_handle(b->fog_handle), IID_PPV_ARGS(&set.fog));
                    CloseHandle(to_handle(b->fog_handle));
                }
                for (uint32_t m = 0; m < rfgrt_ipc::kMaxLightMasks; ++m) {
                    if (!b->light_mask_handles[m]) continue;
                    if (SUCCEEDED(hr) && lightRs)
                        hr = dev->OpenSharedHandle(to_handle(b->light_mask_handles[m]), IID_PPV_ARGS(&set.light_masks[m]));
                    CloseHandle(to_handle(b->light_mask_handles[m]));
                }
                if (FAILED(hr)) return fail("open the frame textures", hr);
                set.generation = gen;
                set.width = b->width;
                set.height = b->height;
                // Ours: the traced visibility (R8G8: sun, ambient), its history (R16G16B16A16: sun, ambient, frames) and the
                // previous depth (R32), at the frame's size.
                {
                    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
                    D3D12_RESOURCE_DESC td{};
                    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    td.Width = set.width;
                    td.Height = set.height;
                    td.DepthOrArraySize = 1;
                    td.MipLevels = 1;
                    td.Format = DXGI_FORMAT_R8G8_UNORM;
                    td.SampleDesc.Count = 1;
                    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                    if (FAILED(hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                 nullptr, IID_PPV_ARGS(&set.vis))))
                        return fail("the visibility texture", hr);
                    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    for (auto& t : set.history)
                        if (FAILED(hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                     nullptr, IID_PPV_ARGS(&t))))
                            return fail("the shadow history", hr);
                    td.Format = DXGI_FORMAT_R32_FLOAT;
                    td.Flags = D3D12_RESOURCE_FLAG_NONE;
                    if (FAILED(hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                                 IID_PPV_ARGS(&set.prev_depth))))
                        return fail("the previous depth", hr);
                    if (set.reflection) {  // reflections: their traced value and its history (RGBA16F)
                        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                        if (FAILED(hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                     nullptr, IID_PPV_ARGS(&set.refl_traced))))
                            return fail("the reflection texture", hr);
                        for (auto& t : set.refl_history)
                            if (FAILED(hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                         nullptr, IID_PPV_ARGS(&t))))
                                return fail("the reflection history", hr);
                    }
                    if (set.irradiance) {  // GI: the traced irradiance and its history (RGBA16F)
                        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                        if (FAILED(hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                     nullptr, IID_PPV_ARGS(&set.gi_traced))))
                            return fail("the GI texture", hr);
                        if (FAILED(hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                     nullptr, IID_PPV_ARGS(&set.gi_scratch))))
                            return fail("the GI filter's texture", hr);
                        if (set.glass) {  // the glass's traced light before its filter (zeroed: no glass seen yet)
                            td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                            if (FAILED(hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                         nullptr, IID_PPV_ARGS(&set.glass_raw))))
                                return fail("the glass's texture", hr);
                            if (FAILED(hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                                         IID_PPV_ARGS(&set.glass_prev))))
                                return fail("the glass's history texture", hr);
                            td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        }
                        if (rrOn) {  // Ray Reconstruction's guides and output
                            auto make = [&](DXGI_FORMAT f, D3D12_RESOURCE_STATES st, ID3D12Resource** out) {
                                td.Format = f;
                                return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(out));
                            };
                            const D3D12_RESOURCE_STATES uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                            if (FAILED(hr = make(DXGI_FORMAT_R32_FLOAT, uav, &set.rr_depth)) ||
                                FAILED(hr = make(DXGI_FORMAT_R16G16_FLOAT, uav, &set.rr_motion)) ||
                                FAILED(hr = make(DXGI_FORMAT_R16G16B16A16_FLOAT, uav, &set.rr_normal)) ||
                                FAILED(hr = make(DXGI_FORMAT_R8G8B8A8_UNORM, uav, &set.rr_albedo)) ||
                                FAILED(hr = make(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &set.rr_spec)) ||
                                FAILED(hr = make(DXGI_FORMAT_R16G16B16A16_FLOAT, uav, &set.rr_out)))
                                return fail("Ray Reconstruction's textures", hr);
                            td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        }
                        for (auto& t : set.gi_history)
                            if (FAILED(hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                         nullptr, IID_PPV_ARGS(&t))))
                                return fail("the GI history", hr);
                        if (set.fog) {  // the fog's froxels: their visibility (ping-pong), the light along their columns
                            set.fog_x = kFogColumns;
                            const uint32_t rows = set.width ? uint32_t((uint64_t(set.height) * kFogColumns + set.width / 2) / set.width) : 1u;
                            set.fog_y = rows > 0 ? rows : 1u;
                            D3D12_RESOURCE_DESC vd{};
                            vd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
                            vd.Width = set.fog_x;
                            vd.Height = set.fog_y;
                            vd.DepthOrArraySize = UINT16(kFogSlices);
                            vd.MipLevels = 1;
                            vd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                            vd.SampleDesc.Count = 1;
                            vd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                            for (auto& t : set.fog_vis)
                                if (FAILED(hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &vd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                             nullptr, IID_PPV_ARGS(&t))))
                                    return fail("the fog's froxels", hr);
                            vd.Format = DXGI_FORMAT_R16_FLOAT;
                            if (FAILED(hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &vd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                         nullptr, IID_PPV_ARGS(&set.fog_ratio))))
                                return fail("the fog's light", hr);
                        }
                    }
                }
                auto srv_of = [](DXGI_FORMAT f) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
                    d.Format = f;
                    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    d.Texture2D.MipLevels = 1;
                    return d;
                };
                auto uav_of = [](DXGI_FORMAT f) {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
                    d.Format = f;
                    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    return d;
                };
                const D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv = srv_of(DXGI_FORMAT_R32_FLOAT), clbSrv = srv_of(DXGI_FORMAT_R8G8_UNORM),
                                                      visSrv = srv_of(DXGI_FORMAT_R8G8_UNORM), histSrv = srv_of(DXGI_FORMAT_R16G16B16A16_FLOAT);
                const D3D12_UNORDERED_ACCESS_VIEW_DESC resultUav = uav_of(DXGI_FORMAT_R8G8_UNORM), visUav = uav_of(DXGI_FORMAT_R8G8_UNORM),
                                                       histUav = uav_of(DXGI_FORMAT_R16G16B16A16_FLOAT);
                auto at = [&](UINT i) {
                    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
                    h.ptr += SIZE_T(i) * inc;
                    return h;
                };
                dev->CreateShaderResourceView(set.depth, &depthSrv, at(kTableCompare + 0));
                dev->CreateUnorderedAccessView(set.result, nullptr, &resultUav, at(kTableCompare + 1));
                dev->CreateShaderResourceView(set.game_clb, &clbSrv, at(kTableCompare + 2));
                dev->CreateShaderResourceView(set.depth, &depthSrv, at(kTableTrace + 0));
                dev->CreateUnorderedAccessView(set.vis, nullptr, &visUav, at(kTableTrace + 1));
                dev->CreateShaderResourceView(set.game_clb, &clbSrv, at(kTableTrace + 2));
                for (UINT w = 0; w < 2; ++w) {
                    const UINT t = kTableTemporal + 5 * w, f = kTableFilter + 4 * w;
                    dev->CreateShaderResourceView(set.vis, &visSrv, at(t + 0));
                    dev->CreateShaderResourceView(set.depth, &depthSrv, at(t + 1));
                    dev->CreateShaderResourceView(set.prev_depth, &depthSrv, at(t + 2));
                    dev->CreateShaderResourceView(set.history[1 - w], &histSrv, at(t + 3));
                    dev->CreateUnorderedAccessView(set.history[w], nullptr, &histUav, at(t + 4));
                    dev->CreateShaderResourceView(set.history[w], &histSrv, at(f + 0));
                    dev->CreateShaderResourceView(set.depth, &depthSrv, at(f + 1));
                    dev->CreateShaderResourceView(set.game_clb, &clbSrv, at(f + 2));
                    dev->CreateUnorderedAccessView(set.result, nullptr, &resultUav, at(f + 3));
                }
                if (set.irradiance) {
                    const D3D12_SHADER_RESOURCE_VIEW_DESC giSrv = srv_of(DXGI_FORMAT_R16G16B16A16_FLOAT);
                    const D3D12_UNORDERED_ACCESS_VIEW_DESC giUav = uav_of(DXGI_FORMAT_R16G16B16A16_FLOAT);
                    // t0 depth, t1 the game's lit colour (set when it comes; null until then), t2 the previous depth
                    for (UINT s = 0; s < 5; ++s)
                        if (s != 1) dev->CreateShaderResourceView(s == 2 ? set.prev_depth : set.depth, &depthSrv, at(kTableGiTrace + s));
                    if (!radiance) {
                        D3D12_SHADER_RESOURCE_VIEW_DESC none = srv_of(DXGI_FORMAT_R16G16B16A16_FLOAT);
                        dev->CreateShaderResourceView(nullptr, &none, at(kTableGiTrace + 1));
                    }
                    dev->CreateUnorderedAccessView(set.gi_traced, nullptr, &giUav, at(kTableGiTrace + 5));
                    if (set.glass && set.glass_raw) {  // the glass's light: the trace's inputs; its filter
                        for (UINT s = 0; s < 5; ++s)
                            if (s != 1 && s != 3)
                                dev->CreateShaderResourceView(s == 2 ? set.prev_depth : set.depth, &depthSrv, at(kTableGlass + s));
                        if (!radiance) {
                            D3D12_SHADER_RESOURCE_VIEW_DESC none = srv_of(DXGI_FORMAT_R16G16B16A16_FLOAT);
                            dev->CreateShaderResourceView(nullptr, &none, at(kTableGlass + 1));
                        }
                        const D3D12_UNORDERED_ACCESS_VIEW_DESC rawUav = uav_of(DXGI_FORMAT_R32G32B32A32_FLOAT);
                        const D3D12_SHADER_RESOURCE_VIEW_DESC rawSrv = srv_of(DXGI_FORMAT_R32G32B32A32_FLOAT);
                        dev->CreateShaderResourceView(set.glass_prev, &rawSrv, at(kTableGlass + 3));  // the history, t3
                        dev->CreateUnorderedAccessView(set.glass_raw, nullptr, &rawUav, at(kTableGlass + 5));
                        for (UINT s = 0; s < 5; ++s)
                            if (s != 3) dev->CreateShaderResourceView(set.depth, &depthSrv, at(kTableGlassFilter + s));
                        dev->CreateShaderResourceView(set.glass_raw, &rawSrv, at(kTableGlassFilter + 3));
                        dev->CreateUnorderedAccessView(set.glass, nullptr, &giUav, at(kTableGlassFilter + 5));
                        // what the clear glass reflects (a null view without it: the pass's writes go nowhere)
                        dev->CreateUnorderedAccessView(set.glass_refl, nullptr, &giUav, at(kTableGlassRefl));
                    }
                    if (set.sun_tint) {  // the sun through clear glass
                        const D3D12_UNORDERED_ACCESS_VIEW_DESC tintUav = uav_of(DXGI_FORMAT_R8G8B8A8_UNORM);
                        dev->CreateUnorderedAccessView(set.sun_tint, nullptr, &tintUav, at(kTableSunTint));
                    }
                    if (set.fog && set.fog_ratio) {  // the fog's passes: their 3D textures at t3 and u0, the depths around them
                        D3D12_SHADER_RESOURCE_VIEW_DESC visSrv3{};
                        visSrv3.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        visSrv3.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                        visSrv3.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        visSrv3.Texture3D.MipLevels = 1;
                        D3D12_UNORDERED_ACCESS_VIEW_DESC visUav3{};
                        visUav3.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        visUav3.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                        visUav3.Texture3D.WSize = kFogSlices;
                        D3D12_SHADER_RESOURCE_VIEW_DESC ratioSrv3 = visSrv3;
                        ratioSrv3.Format = DXGI_FORMAT_R16_FLOAT;
                        D3D12_UNORDERED_ACCESS_VIEW_DESC ratioUav3 = visUav3;
                        ratioUav3.Format = DXGI_FORMAT_R16_FLOAT;
                        auto depths = [&](UINT t) {  // t0, t1, t4: depth; t2: the previous depth
                            for (UINT s = 0; s < 5; ++s)
                                if (s != 3) dev->CreateShaderResourceView(s == 2 ? set.prev_depth : set.depth, &depthSrv, at(t + s));
                        };
                        for (UINT w = 0; w < 2; ++w) {
                            depths(kTableFogInject + 6 * w);
                            dev->CreateShaderResourceView(set.fog_vis[1 - w], &visSrv3, at(kTableFogInject + 6 * w + 3));
                            dev->CreateUnorderedAccessView(set.fog_vis[w], nullptr, &visUav3, at(kTableFogInject + 6 * w + 5));
                            depths(kTableFogIntegrate + 6 * w);
                            dev->CreateShaderResourceView(set.fog_vis[w], &visSrv3, at(kTableFogIntegrate + 6 * w + 3));
                            dev->CreateUnorderedAccessView(set.fog_ratio, nullptr, &ratioUav3, at(kTableFogIntegrate + 6 * w + 5));
                        }
                        depths(kTableFogApply);
                        dev->CreateShaderResourceView(set.fog_ratio, &ratioSrv3, at(kTableFogApply + 3));
                        dev->CreateUnorderedAccessView(set.fog, nullptr, &giUav, at(kTableFogApply + 5));
                    }
                    for (UINT w = 0; w < 2; ++w) {
                        const UINT t = kTableGiTemporal + 6 * w, f = kTableGiFilter + 6 * w;
                        dev->CreateShaderResourceView(set.gi_traced, &giSrv, at(t + 0));
                        dev->CreateShaderResourceView(set.depth, &depthSrv, at(t + 1));
                        dev->CreateShaderResourceView(set.prev_depth, &depthSrv, at(t + 2));
                        dev->CreateShaderResourceView(set.gi_history[1 - w], &giSrv, at(t + 3));
                        dev->CreateShaderResourceView(set.depth, &depthSrv, at(t + 4));
                        dev->CreateUnorderedAccessView(set.gi_history[w], nullptr, &giUav, at(t + 5));
                        dev->CreateShaderResourceView(set.gi_history[w], &giSrv, at(f + 0));
                        for (UINT s = 1; s < 5; ++s) dev->CreateShaderResourceView(set.depth, &depthSrv, at(f + s));
                        dev->CreateUnorderedAccessView(set.irradiance, nullptr, &giUav, at(f + 5));
                    }
                    // the wavelet passes (gi_atrous.hlsl): input, depth (x4), output
                    auto atrous = [&](UINT t, ID3D12Resource* in, ID3D12Resource* out) {
                        dev->CreateShaderResourceView(in, &giSrv, at(t + 0));
                        for (UINT s = 1; s < 5; ++s) dev->CreateShaderResourceView(set.depth, &depthSrv, at(t + s));
                        dev->CreateUnorderedAccessView(out, nullptr, &giUav, at(t + 5));
                    };
                    atrous(kTableGiAtrous + 0, set.gi_history[0], set.gi_traced);
                    atrous(kTableGiAtrous + 6, set.gi_history[1], set.gi_traced);
                    atrous(kTableGiAtrous + 12, set.gi_traced, set.gi_scratch);
                    atrous(kTableGiAtrous + 18, set.gi_scratch, set.gi_traced);
                    atrous(kTableGiAtrous + 24, set.gi_traced, set.irradiance);
                    if (set.rr_out) {  // Ray Reconstruction's guide pass and finalize pass
                        dev->CreateShaderResourceView(set.depth, &depthSrv, at(kTableRrGuides + 0));
                        const D3D12_UNORDERED_ACCESS_VIEW_DESC dUav = uav_of(DXGI_FORMAT_R32_FLOAT), mUav = uav_of(DXGI_FORMAT_R16G16_FLOAT),
                                                               aUav = uav_of(DXGI_FORMAT_R8G8B8A8_UNORM);
                        dev->CreateUnorderedAccessView(set.rr_depth, nullptr, &dUav, at(kTableRrGuides + 1));
                        dev->CreateUnorderedAccessView(set.rr_motion, nullptr, &mUav, at(kTableRrGuides + 2));
                        dev->CreateUnorderedAccessView(set.rr_normal, nullptr, &giUav, at(kTableRrGuides + 3));
                        dev->CreateUnorderedAccessView(set.rr_albedo, nullptr, &aUav, at(kTableRrGuides + 4));
                        dev->CreateShaderResourceView(set.rr_out, &giSrv, at(kTableRrFinalize + 0));
                        for (UINT s = 1; s < 5; ++s) dev->CreateShaderResourceView(set.depth, &depthSrv, at(kTableRrFinalize + s));
                        dev->CreateUnorderedAccessView(set.irradiance, nullptr, &giUav, at(kTableRrFinalize + 5));
                    }
                }
                if (set.reflection) {
                    const D3D12_SHADER_RESOURCE_VIEW_DESC rSrv = srv_of(DXGI_FORMAT_R16G16B16A16_FLOAT);
                    const D3D12_UNORDERED_ACCESS_VIEW_DESC rUav = uav_of(DXGI_FORMAT_R16G16B16A16_FLOAT);
                    for (UINT s = 0; s < 5; ++s)
                        if (s != 1) dev->CreateShaderResourceView(s == 2 ? set.prev_depth : set.depth, &depthSrv, at(kTableReflTrace + s));
                    if (!radiance) {
                        D3D12_SHADER_RESOURCE_VIEW_DESC none = srv_of(DXGI_FORMAT_R16G16B16A16_FLOAT);
                        dev->CreateShaderResourceView(nullptr, &none, at(kTableReflTrace + 1));
                    }
                    dev->CreateUnorderedAccessView(set.refl_traced, nullptr, &rUav, at(kTableReflTrace + 5));
                    for (UINT w = 0; w < 2; ++w) {
                        const UINT t = kTableReflTemporal + 6 * w, f = kTableReflFilter + 6 * w;
                        dev->CreateShaderResourceView(set.refl_traced, &rSrv, at(t + 0));
                        dev->CreateShaderResourceView(set.depth, &depthSrv, at(t + 1));
                        dev->CreateShaderResourceView(set.prev_depth, &depthSrv, at(t + 2));
                        dev->CreateShaderResourceView(set.refl_history[1 - w], &rSrv, at(t + 3));
                        dev->CreateShaderResourceView(set.depth, &depthSrv, at(t + 4));
                        dev->CreateUnorderedAccessView(set.refl_history[w], nullptr, &rUav, at(t + 5));
                        dev->CreateShaderResourceView(set.refl_history[w], &rSrv, at(f + 0));
                        for (UINT s = 1; s < 5; ++s) dev->CreateShaderResourceView(set.depth, &depthSrv, at(f + s));
                        dev->CreateUnorderedAccessView(set.reflection, nullptr, &rUav, at(f + 5));
                    }
                }
                if (lightRs) {
                    dev->CreateShaderResourceView(set.depth, &depthSrv, at(kTableLights));
                    const D3D12_UNORDERED_ACCESS_VIEW_DESC maskUav = uav_of(DXGI_FORMAT_R8G8_UNORM);
                    for (UINT m = 0; m < rfgrt_ipc::kMaxLightMasks; ++m)
                        dev->CreateUnorderedAccessView(set.light_masks[m], nullptr, &maskUav, at(kTableLights + 1 + m));
                }
                b->host_fog = set.fog && set.fog_ratio ? 1 : 0;  // the plugin blends the fog only then
                rfgrt_ipc::store64(&b->host_generation_first_frame, next);
                InterlockedExchange(reinterpret_cast<volatile LONG*>(&b->host_generation), gen);
                logf("frame textures %ux%u (set %d) from frame %lld", set.width, set.height, gen, static_cast<long long>(next));
            }
        }
        // The game's lit colour in a new shared texture (its first copy, or its target changed): once every frame
        // queued with the previous one is done, as the descriptor changes.
        const int32_t rgen = b->radiance_generation;
        if (next && giRs && set.generation != 0 && rgen != radianceGen && b->radiance_handle) {
            if (sig > 0 && done < UINT64(sig)) {
                waitFor = UINT64(sig);
                next = 0;
            } else {
                release(radiance);
                const HRESULT rhr = dev->OpenSharedHandle(to_handle(b->radiance_handle), IID_PPV_ARGS(&radiance));
                CloseHandle(to_handle(b->radiance_handle));
                radianceGen = rgen;
                if (FAILED(rhr)) {
                    release(radiance);
                    logf("GI: the game's lit colour could not be opened (0x%08lX): the rays' hits are shaded without it",
                         static_cast<unsigned long>(rhr));
                } else {
                    const D3D12_RESOURCE_DESC rd = radiance->GetDesc();
                    DXGI_FORMAT f = rd.Format;
                    if (f == DXGI_FORMAT_R16G16B16A16_TYPELESS) f = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS) f = DXGI_FORMAT_R8G8B8A8_UNORM;
                    if (f == DXGI_FORMAT_R10G10B10A2_TYPELESS) f = DXGI_FORMAT_R10G10B10A2_UNORM;
                    if (f == DXGI_FORMAT_R32G32B32A32_TYPELESS) f = DXGI_FORMAT_R32G32B32A32_FLOAT;
                    D3D12_SHADER_RESOURCE_VIEW_DESC rs{};
                    rs.Format = f;
                    rs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    rs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    rs.Texture2D.MipLevels = 1;
                    for (UINT slot : {kTableGiTrace + 1, kTableReflTrace + 1, kTableGlass + 1}) {  // GI's, the reflections', the glass's t1
                        D3D12_CPU_DESCRIPTOR_HANDLE hnd = heap->GetCPUDescriptorHandleForHeapStart();
                        hnd.ptr += SIZE_T(slot) * inc;
                        dev->CreateShaderResourceView(radiance, &rs, hnd);
                    }
                    logf("GI: the game's lit colour %llux%u (format %d, viewed as %d) for the rays' hits",
                         static_cast<unsigned long long>(rd.Width), rd.Height, int(rd.Format), int(f));
                }
            }
        }
        // A new DLSS mode (Block::dlss_mode: DLSS.Mode, or the game's DLSS row while it runs): once every frame queued
        // with the previous one is done, as Ray Reconstruction is made again for it (the one they used is released).
        {
            const int32_t want = b->dlss_mode < dlss::kModeOff ? dlss::kModeOff : b->dlss_mode >= dlss::kModeCount ? dlss::kModeCount - 1 : b->dlss_mode;
            if (next && want != dlssMode) {
                if (dlssMode >= 0 && sig > 0 && done < UINT64(sig)) {
                    waitFor = UINT64(sig);
                    next = 0;
                } else {
                    dlssReset = dlssMode >= 0;
                    dlssMode = want;
                    InterlockedExchange(reinterpret_cast<volatile LONG*>(&b->host_dlss_mode), dlssMode);
                }
            }
        }
        if (next && set.generation == 0) {  // no textures yet: nothing to answer this frame with
            if (snapshots) n = next;  // its scene is skipped
            ++skippedNoSet;
            next = 0;
        }
        // The ring slot must be free: its last frame done.
        if (next && slotFrame[k] && done < UINT64(slotFrame[k])) {
            waitFor = UINT64(slotFrame[k]);
            next = 0;
        }
        if (next) {
            // The finished frame's numbers, from the slot's readback.
            if (slotFrame[k]) {
                D3D12_RANGE rr{SIZE_T(k) * kReadback, SIZE_T(k + 1) * kReadback};
                uint8_t* r = nullptr;
                if (SUCCEEDED(readback->Map(0, &rr, reinterpret_cast<void**>(&r)))) {
                    const uint64_t* t = reinterpret_cast<const uint64_t*>(r + k * kReadback);
                    const uint32_t* c = reinterpret_cast<const uint32_t*>(r + k * kReadback + kStamps * 8);
                    const double tlasUs = t[1] > t[0] ? double(t[1] - t[0]) * 1e6 / double(tsFreq) : 0.0;
                    const double passUs = t[3] > t[2] ? double(t[3] - t[2]) * 1e6 / double(tsFreq) : 0.0;
                    const double giUs = t[6] > t[4] ? double(t[6] - t[4]) * 1e6 / double(tsFreq) : 0.0;  // the pass's GI part
                    const double reflUs = t[3] > t[6] ? double(t[3] - t[6]) * 1e6 / double(tsFreq) : 0.0;  // and reflections
                    const double lightUs = t[4] > t[5] ? double(t[4] - t[5]) * 1e6 / double(tsFreq) : 0.0;  // its lights' part
                    const double fogUs = t[8] > t[7] ? double(t[8] - t[7]) * 1e6 / double(tsFreq) : 0.0;  // the fog (in GI's)
                    st.fog_us += fogUs;
                    auto span = [&](int a, int z) { return t[z] > t[a] ? double(t[z] - t[a]) * 1e6 / double(tsFreq) : 0.0; };
                    st.prep_us += span(4, 9);      // the probe, the drawn sky, the radiance cache (and the lights' masks' end)
                    st.trace_us += span(9, 10);    // the GI's rays (gi_trace.hlsl)
                    st.glass_us += span(10, 11);   // the glass's light and reflections (glass_light, glass_filter)
                    st.tint_us += span(11, 7);     // the sun through clear glass (sun_tint.hlsl)
                    st.denoise_us += span(8, 6);   // Ray Reconstruction (or the temporal and wavelet passes)
                    st.tlas_us += tlasUs;
                    st.pass_us += passUs;
                    st.gi_us += giUs;
                    st.light_us += lightUs;
                    st.refl_us += reflUs;
                    ++st.frames;
                    if (mode == rfgrt_ipc::kModeDepthCompare) {
                        for (int i = 0; i < 5; ++i) st.classes[i] += c[i];
                        for (int i = 0; i < 16; ++i) st.ratio[i] += c[8 + i];
                        memcpy(st.samples, c + 32, sizeof(st.samples));
                    } else if (mode == rfgrt_ipc::kModeSunShadows) {
                        for (int i = 0; i < 9; ++i) st.shadow[i] += c[i];
                        if (giCensus)
                            for (int i = 0; i < 25; ++i) census[i] += c[100 + i];
                        st.glass_px += c[60];
                        st.glass_lit += c[61];
                        static uint64_t skyLog = 0;
                        if (skyOn && ++skyLog % 900 == 0) {
                            auto fl = [](uint32_t u) {
                                float v;
                                memcpy(&v, &u, 4);
                                return v;
                            };
                            logf("sky: the drawn sky's mean radiance (%.4f %.4f %.4f) over %u of 1024 directions; at the top centre "
                                 "the pass reads the lit colour (%.4f %.4f %.4f), depth %.6f, previous depth %.6f, flags 0x%X",
                                 fl(c[48]), fl(c[49]), fl(c[50]), c[51], fl(c[52]), fl(c[53]), fl(c[54]), fl(c[55]), fl(c[56]), c[57]);
                        }
                        if (probeFrame[k] && probeFrame[k] == slotFrame[k] && c[32] == 0x50524F42u) {
                            auto f32 = [](uint32_t u) {
                                float v;
                                memcpy(&v, &u, 4);
                                return v;
                            };
                            const std::vector<uint32_t>& pi = probeInstances[k];
                            auto hit_name = [&](uint32_t id, uint32_t geometry, float t, char* out, size_t n) {
                                static const char* kinds[] = {"?", "mesh", "skinned", "Geo-Mod piece", "other", "terrain"};
                                if (id == 0xFFFFFFFFu)
                                    snprintf(out, n, "nothing");
                                else if (size_t(id) * 4 + 3 >= pi.size())
                                    snprintf(out, n, "instance %u (beyond the frame's list)", id);
                                else {
                                    // the draw range's material map entry, and what the instance's glass word makes of it
                                    const uint32_t entry = scene.geometry_material(pi[id * 4], pi[id * 4 + 1], geometry);
                                    const uint32_t gw = pi[id * 4 + 3];
                                    const char* what = "not glass";
                                    uint32_t mclear = 0, mtrans = 0;
                                    if (entry < 32 && gw && scene.mesh_glass(pi[id * 4], &mclear, &mtrans)) {
                                        what = (mclear >> entry) & 1u ? "clear glass (by its mesh's materials)"
                                               : (mtrans >> entry) & 1u ? "translucent glass (by its mesh's materials)" : "not glass";
                                    } else if (entry < 31) {
                                        if (gw & 0x40000000u)
                                            what = (gw >> entry) & 1u ? "clear glass" : (entry < 15 && ((gw >> (15 + entry)) & 1u)) ? "translucent glass" : "not glass";
                                        else if ((gw >> entry) & 1u)
                                            what = gw & 0x80000000u ? "translucent glass" : "clear glass";
                                    }
                                    snprintf(out, n, "\"%s\" (mesh %u, submesh %u, %s, draw range %u: material entry %d, %s; glass word %08X) at %.2f m",
                                             scene.mesh_name(pi[id * 4]), pi[id * 4], pi[id * 4 + 1], pi[id * 4 + 2] < 6 ? kinds[pi[id * 4 + 2]] : "?",
                                             geometry, entry == 0xFFFFFFFFu ? -1 : int(entry), what, gw, t);
                                }
                            };
                            char sunHit[320], camHit[320], anyHit[320];
                            hit_name(c[35], c[36], f32(c[37]), sunHit, sizeof(sunHit));
                            hit_name(c[38], c[39], f32(c[40]), camHit, sizeof(camHit));
                            hit_name(c[45], c[46], f32(c[47]), anyHit, sizeof(anyHit));
                            logf("probe (frame %lld): the first surface of any kind there (glass included) is %s",
                                 static_cast<long long>(probeFrame[k]), anyHit);
                            if (c[80] == 0x4D495252u) {  // "MIRR": what a mirror at the first surface would show (probe.hlsl)
                                static const char* kHow[] = {"(nothing there: the sky)", "the game's colour where it saw it",
                                                             "its material's texture", "its material, which has no texture",
                                                             "nothing known of its material: the plain grey"};
                                char mirrorHit[320];
                                hit_name(c[76], c[77], f32(c[78]), mirrorHit, sizeof(mirrorHit));
                                logf("probe (frame %lld): a mirror at that first surface shows %s, shaded from %s",
                                     static_cast<long long>(probeFrame[k]), mirrorHit, c[79] <= 4 ? kHow[c[79]] : "?");
                                if (c[76] != 0xFFFFFFFFu) {
                                    auto rgb = [&](uint32_t at, char* out, size_t n) {
                                        if (f32(c[at]) < 0.0f)
                                            snprintf(out, n, "none");
                                        else
                                            snprintf(out, n, "(%.3f %.3f %.3f)", f32(c[at]), f32(c[at + 1]), f32(c[at + 2]));
                                    };
                                    char seenS[64], cacheS[64], madeS[64];
                                    rgb(87, seenS, sizeof(seenS));
                                    rgb(90, cacheS, sizeof(cacheS));
                                    rgb(93, madeS, sizeof(madeS));
                                    logf("probe (frame %lld): that surface: the game's colour %s, the radiance cache's %s, its material %s "
                                         "makes %s under the sun and the ambient (albedo %.3f %.3f %.3f)",
                                         static_cast<long long>(probeFrame[k]), seenS, cacheS, c[99] ? "(known)" : "(not known)",
                                         madeS, f32(c[96]), f32(c[97]), f32(c[98]));
                                }
                            }
                            if (c[75] == 0x4D41544Cu) {  // "MATL": what the rays make of the camera ray's surface (probe.hlsl)
                                auto half = [](uint32_t h) {  // IEEE half to float
                                    const uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
                                    uint32_t bits;
                                    if (e == 0)
                                        bits = s;  // (subnormals as zero: none of ours)
                                    else if (e == 31)
                                        bits = s | 0x7F800000u | (m << 13);
                                    else
                                        bits = s | ((e + 112u) << 23) | (m << 13);
                                    float v;
                                    memcpy(&v, &bits, 4);
                                    return v;
                                };
                                const uint32_t mx = c[71];
                                if (mx == 0xFFFFFFFFu || !(mx & 0x80000000u))
                                    logf("probe (frame %lld): its surface for the rays: albedo (%.3f %.3f %.3f), glow %.2f; texture "
                                         "coordinates at %08X, material slot %08X entry %d: %s",
                                         static_cast<long long>(probeFrame[k]), f32(c[64]), f32(c[65]), f32(c[66]), f32(c[67]), c[68],
                                         c[69], int(c[70]), mx == 0xFFFFFFFFu ? "no slot or entry" : "not known (a plain grey)");
                                else
                                    logf("probe (frame %lld): its surface for the rays: albedo (%.3f %.3f %.3f), glow %.2f; texture "
                                         "coordinates at %08X, material slot %u entry %u: texture %d, tiling (%.3f %.3f), colour "
                                         "(%.3f %.3f %.3f), self-illumination %.2f",
                                         static_cast<long long>(probeFrame[k]), f32(c[64]), f32(c[65]), f32(c[66]), f32(c[67]), c[68],
                                         c[69], c[70], int(mx & 0xFFFFu) - 1, half(c[72] & 0xFFFFu), half(c[72] >> 16),
                                         half(c[73] & 0xFFFFu), half(c[73] >> 16), half(c[74] & 0xFFFFu), half(c[74] >> 16));
                                if (f32(c[81]) >= 0.0f)
                                    logf("probe (frame %lld): there the game drew (%.3f %.3f %.3f), the rays make (%.3f %.3f %.3f) of its "
                                         "material under the sun and the ambient",
                                         static_cast<long long>(probeFrame[k]), f32(c[81]), f32(c[82]), f32(c[83]), f32(c[84]),
                                         f32(c[85]), f32(c[86]));
                                else
                                    logf("probe (frame %lld): the game did not draw it in the previous frame; the rays make (%.3f %.3f "
                                         "%.3f) of its material under the sun and the ambient",
                                         static_cast<long long>(probeFrame[k]), f32(c[84]), f32(c[85]), f32(c[86]));
                            }
                            if (c[33])
                                logf("probe (frame %lld): the crosshair's surface is %.2f m away at (%.1f %.1f %.1f); toward the sun "
                                     "its ray meets %s; from the camera the rays first meet %s",
                                     static_cast<long long>(probeFrame[k]), f32(c[34]), f32(c[42]), f32(c[43]), f32(c[44]),
                                     c[41] ? sunHit : "(no sun)", camHit);
                            else
                                logf("probe (frame %lld): no surface under the crosshair (sky); from the camera the rays first meet %s",
                                     static_cast<long long>(probeFrame[k]), camHit);
                            probeFrame[k] = 0;
                        }
                    }
                    D3D12_RANGE none{0, 0};
                    readback->Unmap(0, &none);
                    rfgrt_ipc::store64(&b->host_gpu_us, int64_t(passUs));
                    rfgrt_ipc::store64(&b->tlas_build_us, int64_t(tlasUs + passUs));
                }
                slotFrame[k] = 0;  // read once
            }
            alloc[k]->Reset();
            // List 1, no wait: the TLAS from the frame's scene, built while the game still renders the frame.
            const rfgrt_frame::SlotHeader* h = nullptr;
            uint32_t inTlas = 0;
            bool scene_ok = true;
            cl->Reset(alloc[k], nullptr);
            cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps));
            scene.record_material_uploads(cl, k);  // the meshes' materials changed since, before this frame's rays
            if (use_tlas) {
                h = reinterpret_cast<const rfgrt_frame::SlotHeader*>(region + b->frames_offset +
                                                                     uint64_t(next % rfgrt_frame::kSlots) * b->frame_slot_bytes);
                const uint32_t count = h->instance_count < b->max_instances ? h->instance_count : b->max_instances;
                const auto& cm = h->camera.camera_to_world.m;
                const float camera[3] = {cm[0][3], cm[1][3], cm[2][3]};
                tlas.set_camera(camera);
                // skinned instances posed first: their BLAS replace their meshes' bind pose in this TLAS
                skinning.record(cl, k, h, b->max_instances, b->max_skins, scene, next);
                inTlas = tlas.record(cl, k, rfgrt_frame::slot_instances(h), count, scene, skinning.overrides());
                probeFrame[k] = 0;
                if (b->probe_request != probeSeen && probePso) {
                    probeSeen = b->probe_request;
                    probeFrame[k] = next;
                    const rfgrt_frame::Instance* in = rfgrt_frame::slot_instances(h);
                    probeInstances[k].resize(size_t(count) * 4);
                    for (uint32_t i = 0; i < count; ++i) {
                        probeInstances[k][i * 4] = in[i].mesh_id;
                        probeInstances[k][i * 4 + 1] = in[i].submesh;
                        probeInstances[k][i * 4 + 2] = in[i].kind;
                        probeInstances[k][i * 4 + 3] = in[i].glass;
                    }
                }
                st.instances += count;
                st.in_tlas += inTlas;
                rfgrt_ipc::store64(&b->tlas_instances, inTlas);
                rfgrt_ipc::store64(&b->tlas_missing, int64_t(count) - int64_t(inTlas));
                scene_ok = rfgrt_ipc::load64(&h->frame) == next;  // the plugin did not overwrite it meanwhile
            }
            cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 1));
            cl->Close();
            if (!scene_ok) {
                ++torn;  // a newer scene replaced it while we read: take that one next time
                skinning.abandon();
                continue;
            }
            ID3D12CommandList* lists[] = {cl};
            queue->ExecuteCommandLists(1, lists);
            skinning.commit();
            scene.commit_material_uploads();
            // List 2, after the plugin's depth copy (fence A): the pass.
            cl->Reset(alloc[k], nullptr);
            // the geometry pool's new pages, viewed for the rays (only ever added)
            for (; geoPagesViewed < scene.geo_pool().page_count() && geoPagesViewed < kMaxGeoPages; ++geoPagesViewed) {
                D3D12_SHADER_RESOURCE_VIEW_DESC d = rawSrv;
                d.Buffer.NumElements = UINT(scene.geo_pool().page_size(geoPagesViewed) / 4);
                D3D12_CPU_DESCRIPTOR_HANDLE hnd = heap->GetCPUDescriptorHandleForHeapStart();
                hnd.ptr += SIZE_T(kGeoPages + geoPagesViewed) * inc;
                dev->CreateShaderResourceView(scene.geo_pool().page(geoPagesViewed), &d, hnd);
            }
            D3D12_RESOURCE_BARRIER in[16 + rfgrt_ipc::kMaxLightMasks] = {
                transition(set.depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                transition(set.result, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                transition(set.game_clb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                transition(set.irradiance, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
            UINT inCount = set.irradiance ? 4 : 3;
            for (ID3D12Resource* m : set.light_masks)
                if (m) in[inCount++] = transition(m, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (radiance) in[inCount++] = transition(radiance, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (set.reflection) in[inCount++] = transition(set.reflection, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (set.glass) in[inCount++] = transition(set.glass, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (set.glass_refl)
                in[inCount++] = transition(set.glass_refl, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (set.sun_tint)
                in[inCount++] = transition(set.sun_tint, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (set.fog) in[inCount++] = transition(set.fog, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl->ResourceBarrier(inCount, in);
            cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 2));
            bool giStamped = false;  // GI's and the lights' starts stamped (else just before the pass's end: no time)
            bool fogStamped = false;  // and the fog's
            bool partsStamped = false;  // and the GI's parts' (9-11)
            cl->SetDescriptorHeaps(1, &heap);
            if ((mode == rfgrt_ipc::kModeDepthCompare || mode == rfgrt_ipc::kModeSunShadows) && h) {
                CompareConstants c{};
                // The camera as the GPU sees it: rl_camera's view matrix (row-vector, p_view = p V + t). Its columns are
                // the camera's axes in render space, and the camera sits where p V + t = 0.
                const float* V = h->camera.view;
                for (int j = 0; j < 3; ++j) {
                    c.right[j] = V[j * 4 + 0];
                    c.up[j] = V[j * 4 + 1];
                    c.forward[j] = V[j * 4 + 2];
                    c.origin[j] = -(V[12] * V[j * 4 + 0] + V[13] * V[j * 4 + 1] + V[14] * V[j * 4 + 2]);
                }
                const auto& m = h->camera.camera_to_world.m;
                for (int j = 0; j < 3; ++j) {
                    worldBasis[0][j] = m[j][0];
                    worldBasis[1][j] = m[j][1];
                    worldBasis[2][j] = m[j][2];
                    worldBasis[3][j] = m[j][3];
                }
                bool reversed = false;
                projection_terms(h->camera.projection, c.proj, &reversed);
                if (!loggedProjection) {
                    loggedProjection = true;
                    const float* P = h->camera.projection;
                    logf("camera: projection rows [%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g], near %g, far %g, fov %g; "
                         "depth %s",
                         P[0], P[1], P[2], P[3], P[4], P[5], P[6], P[7], P[8], P[9], P[10], P[11], P[12], P[13], P[14], P[15],
                         h->camera.near_clip, h->camera.far_clip, h->camera.fov, reversed ? "reversed (near 1)" : "standard (near 0)");
                    logf("camera: right (%g %g %g) up (%g %g %g) forward (%g %g %g) at (%g %g %g); render offset (%g %g %g)", c.right[0],
                         c.right[1], c.right[2], c.up[0], c.up[1], c.up[2], c.forward[0], c.forward[1], c.forward[2], c.origin[0],
                         c.origin[1], c.origin[2], h->render_offset[0], h->render_offset[1], h->render_offset[2]);
                }
                c.range[0] = h->camera.depth_max > 0.5f && h->camera.depth_max <= 1.0f ? h->camera.depth_max : 1.0f;
                memcpy(c.sun, h->sun.direction, sizeof(float) * 3);
                c.sun[3] = h->sun.present;
                c.width = set.width;
                c.height = set.height;
                c.tolerance = 0.02f;
                lastCamera = c;
                memcpy(lastOffset, h->render_offset, sizeof(lastOffset));
                memcpy(lastView, h->camera.view, sizeof(lastView));
                memcpy(lastStateView, h->camera.state_view, sizeof(lastStateView));
                memcpy(lastStateProj, h->camera.state_projection, sizeof(lastStateProj));
                memcpy(lastProj, h->camera.projection, sizeof(lastProj));
                auto table = [&](UINT i) {
                    D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
                    h.ptr += UINT64(i) * inc;
                    return h;
                };
                const D3D12_GPU_VIRTUAL_ADDRESS countsVa = counts->GetGPUVirtualAddress() + uint64_t(k) * kCounts;
                const D3D12_RESOURCE_BARRIER cb = uav_barrier(counts);
                if (mode == rfgrt_ipc::kModeSunShadows) {
                    const float tanSun = std::tan(b->shadow_sun_angle_deg * 3.14159265f / 180.0f);
                    c.shadow[0] = uint32_t(b->shadow_rays > 0 ? b->shadow_rays : 1);
                    memcpy(&c.shadow[1], &tanSun, 4);
                    c.shadow[2] = uint32_t(b->shadow_filter_radius >= 0 ? b->shadow_filter_radius : 0);
                    c.shadow[3] = uint32_t(next);
                    c.range[1] = b->ao_radius;                                   // ambient occlusion: the rays' length
                    c.range[2] = float(b->ao_rays > 0 ? b->ao_rays : 0);         // and how many a pixel (0: none)
                    // The reprojection into the previous frame answered: its camera, and the render offset's change since
                    // (a point p of this frame's render space is p + offset - previous offset in that frame's).
                    {
                        float shift[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                        for (int i = 0; i < 3; ++i) shift[12 + i] = h->render_offset[i] - prevOffset[i];
                        float vp[16];
                        mul44(prevView, prevProj, vp);
                        mul44(shift, vp, c.reproject);
                        float prevTerms[4];
                        bool prevReversed = false;
                        projection_terms(prevProj, prevTerms, &prevReversed);
                        c.prev[0] = prevTerms[2];
                        c.prev[1] = prevTerms[3];
                        c.prev[2] = prevRange;
                        c.prev[3] = float(b->shadow_history > 1 ? b->shadow_history : 1);
                    }
                    const uint32_t depthFlag = reversed ? 2u : 0u;
                    const uint32_t historyFlag = historyValid && b->shadow_history > 1 ? 0u : 8u;
                    // 32: the translucent glass diffuses the light through it (GI.FrostedGlass, rt_query.hlsli): for the
                    // passes that trace
                    const uint32_t frostedFlag = b->frosted_glass ? 32u : 0u;
                    // 64: the clear glass tints the GI's and the reflections' rays (GI.WindowTint, trace_common.hlsli)
                    const uint32_t tintFlag = b->window_tint && sunTintPso ? 64u : 0u;
                    const UINT filterTable = kTableFilter + 4 * UINT(histOut), temporalTable = kTableTemporal + 5 * UINT(histOut);
                    // the counters first: the passes add to them
                    cl->SetPipelineState(filterPso);
                    cl->SetComputeRootSignature(filterRs);
                    cl->SetComputeRootDescriptorTable(0, table(filterTable));
                    cl->SetComputeRootUnorderedAccessView(1, countsVa);
                    c.flags = 1;
                    cl->SetComputeRoot32BitConstants(2, UINT(sizeof(c) / 4), &c, 0);
                    cl->Dispatch(1, 1, 1);
                    cl->ResourceBarrier(1, &cb);
                    // pass 1: this frame's visibility, a few rays a pixel
                    c.flags = depthFlag | frostedFlag;
                    cl->SetPipelineState(comparePso);
                    cl->SetComputeRootSignature(compareRs);
                    cl->SetComputeRootDescriptorTable(0, table(kTableTrace));
                    cl->SetComputeRootShaderResourceView(1, tlas.address(k));
                    cl->SetComputeRootUnorderedAccessView(2, countsVa);
                    cl->SetComputeRoot32BitConstants(3, UINT(sizeof(c) / 4), &c, 0);
                    cl->SetComputeRootShaderResourceView(4, tlas.instance_table(k));
                    cl->Dispatch((set.width + 7) / 8, (set.height + 7) / 8, 1);
                    // pass 2: into the accumulation over frames (history[histOut], from history[1 - histOut])
                    const D3D12_RESOURCE_BARRIER toTemporal[2] = {
                        transition(set.vis, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                        transition(set.history[1 - histOut], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
                    cl->ResourceBarrier(2, toTemporal);
                    cl->SetPipelineState(temporalPso);
                    cl->SetComputeRootSignature(temporalRs);
                    cl->SetComputeRootDescriptorTable(0, table(temporalTable));
                    cl->SetComputeRootUnorderedAccessView(1, countsVa);
                    c.flags = depthFlag | historyFlag;
                    cl->SetComputeRoot32BitConstants(2, UINT(sizeof(c) / 4), &c, 0);
                    cl->Dispatch((set.width + 7) / 8, (set.height + 7) / 8, 1);
                    // pass 3: the filter into the result, and the counters against the game's cascades
                    const D3D12_RESOURCE_BARRIER toFilter =
                        transition(set.history[histOut], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    cl->ResourceBarrier(1, &toFilter);
                    cl->SetPipelineState(filterPso);
                    cl->SetComputeRootSignature(filterRs);
                    cl->SetComputeRootDescriptorTable(0, table(filterTable));
                    cl->SetComputeRootUnorderedAccessView(1, countsVa);
                    c.flags = depthFlag | (b->debug_view == 1 ? 4u : 0u);  // 4: the comparison display in R
                    cl->SetComputeRoot32BitConstants(2, UINT(sizeof(c) / 4), &c, 0);
                    cl->Dispatch((set.width + 7) / 8, (set.height + 7) / 8, 1);
                    // The dynamic lights' shadows: a mask each, for the plugin to bind at t15 for that light's pass.
                    cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 5));
                    if (lightRs && h->light_count > 0) {
                        LightConstants lc{};
                        lc.base = c;
                        lc.base.flags = depthFlag | frostedFlag;
                        const uint32_t count = h->light_count < rfgrt_frame::kMaxLights ? h->light_count : rfgrt_frame::kMaxLights;
                        lc.light_info[0] = count;
                        memcpy(lc.lights, h->lights, sizeof(rfgrt_frame::Light) * count);
                        for (uint32_t i = 0; i < count; ++i)
                            if (lc.lights[i].mask >= rfgrt_ipc::kMaxLightMasks) lc.lights[i].mask = rfgrt_ipc::kMaxLightMasks - 1;
                        memcpy(lightCbCpu + uint64_t(k) * 1024, &lc, sizeof(lc));
                        cl->SetComputeRootSignature(lightRs);
                        cl->SetPipelineState(lightPso);
                        cl->SetComputeRootDescriptorTable(0, table(kTableLights));
                        cl->SetComputeRootShaderResourceView(1, tlas.address(k));
                        cl->SetComputeRootConstantBufferView(2, lightCb->GetGPUVirtualAddress() + uint64_t(k) * 1024);
                        cl->Dispatch((set.width + 7) / 8, (set.height + 7) / 8, 1);
                        ++lightFrames;
                        lightsTraced += count;
                    }
                    // Global illumination: path-traced irradiance, accumulated over frames (the same reprojection as the
                    // shadows'), filtered into the texture the game's patched lit shaders read as their ambient.
                    cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 4));
                    giStamped = true;
                    if (giRs && set.irradiance && set.gi_traced) {
                        GiConstants g{};
                        g.base = c;
                        // 16: the previous frame's lit colour and depth are there for the hits. When this frame runs on the
                        // GPU (after fence A at its seam, before the game's lighting passes copy its own colour) the
                        // texture holds the previous frame's, if that frame copied one; the copy of this frame may
                        // already be recorded by now (this list is recorded at the frame scene, often later), hence >=.
                        const bool radianceOk = radiance && historyValid && rfgrt_ipc::load64(&b->radiance_frame) >= next - 1;
                        if (radianceOk) ++radianceFrames;
                        g.base.flags = depthFlag | historyFlag | (radianceOk ? 16u : 0u) | frostedFlag | tintFlag | (giCensus ? 128u : 0u);
                        g.base.shadow[1] = uint32_t(b->gi_filter_radius >= 0 ? b->gi_filter_radius : 0);
                        memcpy(g.ambient, h->ambient, sizeof(g.ambient));
                        memcpy(g.back_ambient, h->back_ambient, sizeof(g.back_ambient));
                        memcpy(g.sun_color, h->light_color, sizeof(g.sun_color));
                        g.gi[0] = float(b->gi_rays);
                        g.gi[1] = b->gi_length;
                        g.gi[2] = b->gi_albedo;
                        g.gi[3] = float(b->gi_history > 1 ? b->gi_history : 1);
                        g.gi2[0] = b->gi_bounce;
                        g.gi2[1] = b->gi_sky;
                        g.gi2[2] = b->gi_sun_bounce_albedo;  // GI.SunBounceAlbedo (trace_common.hlsli sun_bounce)
                        memcpy(g.world, h->render_offset, sizeof(float) * 3);
                        g.world[3] = cacheOn ? 1.0f : 0.0f;
                        g.sky_probe[0] = skyOn ? 1.0f : 0.0f;
                        g.sky_probe[1] = 0.990268f;  // cos 8 degrees: the sun's disk and glow
                        g.sky_probe[2] = b->gi_sky_drawn;
                        g.sky_probe[3] = std::tan(b->shadow_sun_angle_deg * 3.14159265f / 180.0f);  // the glass's sun
                        memcpy(g.terrain, b->terrain_albedo, sizeof(float) * 3);  // the terrain's albedo (GI.TerrainAlbedo)
                        const bool fogSet = fogApplyPso && set.fog && set.fog_ratio;
                        // the game's fog (frame_scene.h: fogDistance = (b, a, -, c)): the volumetric fog's, and what fades
                        // the glass's reflections
                        g.fog[0] = h->fog_distance[1] * (b->fog_density >= 0.0f ? b->fog_density : 1.0f);
                        g.fog[1] = h->fog_distance[0];
                        g.fog[2] = h->fog_distance[3];
                        g.fog[3] = 1.0f;
                        g.glass[0] = b->refl_glass_f0;
                        g.probe[0] = b->probe_x > 0.0f && b->probe_x < 1.0f ? b->probe_x : 0.5f;
                        g.probe[1] = b->probe_y > 0.0f && b->probe_y < 1.0f ? b->probe_y : 0.5f;
                        g.glass[1] = set.glass_refl && b->refl_glass ? 1.0f : 0.0f;
                        if (fogSet) {  // its grid
                            memcpy(g.fog_color, h->fog_color, sizeof(float) * 3);
                            g.fog_color[3] = b->fog_sun_share;
                            g.fog2[0] = b->fog_anisotropy;
                            g.fog2[1] = kFogNear;
                            const float farPlane = h->camera.far_clip;
                            g.fog2[2] = farPlane > 50.0f ? (farPlane < 2000.0f ? farPlane : 2000.0f) : 50.0f;
                            g.fog2[3] = float(kFogSlices);
                            g.fog3[0] = float(set.fog_x);
                            g.fog3[1] = float(set.fog_y);
                            g.fog3[2] = fogHistory && historyValid ? 1.0f : 0.0f;
                            g.fog3[3] = kFogFrames;
                        }
                        // DLSS (dlss.h): Ray Reconstruction denoises the GI unless the mode is off; below DLAA the rays
                        // and its guides run at the mode's render size (the top-left part of their textures), each of
                        // its pixels tracing from the frame's pixel under a jittered point of it (Halton 2, 3: about 8
                        // points per frame pixel it covers), and Ray Reconstruction upscales the light to the frame's size.
                        const bool rrUse = rrOn && set.rr_out && b->gi_denoiser == 2 && dlssMode >= dlss::kModeDlaa;
                        unsigned rw = set.width, rh = set.height;
                        if (rrUse) dlss::render_size(set.width, set.height, dlssMode, rw, rh);
                        const bool upscaling = rw < set.width || rh < set.height;
                        float jx = 0.0f, jy = 0.0f;
                        if (upscaling) {
                            const float sx = float(set.width) / float(rw), sy = float(set.height) / float(rh);
                            uint32_t phases = uint32_t(std::ceil(8.0f * sx * sy));
                            if (phases < 8) phases = 8;
                            const uint32_t i = 1 + uint32_t(uint64_t(next) % phases);
                            // the point (Halton, in [0, 1) of the render pixel) moved to the centre of the frame's pixel
                            // under it: exact for every render pixel at a whole scale (performance, ultra performance)
                            jx = (std::floor(halton(i, 2) * sx) + 0.5f) / sx - 0.5f;
                            jy = (std::floor(halton(i, 3) * sy) + 0.5f) / sy - 0.5f;
                            g.upscale[0] = float(rw);
                            g.upscale[1] = float(rh);
                            g.upscale[2] = jx;
                            g.upscale[3] = jy;
                        }
                        if (rrUse && (dlssMode != dlssLoggedMode || rw != dlssLoggedWidth || rh != dlssLoggedHeight)) {
                            dlssLoggedMode = dlssMode;
                            dlssLoggedWidth = rw;
                            dlssLoggedHeight = rh;
                            logf("DLSS %s: the GI's rays at %ux%u of %ux%u (%.0f %%)%s", dlss::mode_name(dlssMode), rw, rh, set.width,
                                 set.height, 100.0 * double(rw) / double(set.width),
                                 upscaling ? ", jittered, Ray Reconstruction upscaling them" : "");
                        } else if (!rrUse && rrOn && set.rr_out && b->gi_denoiser == 2 && dlssLoggedMode != dlss::kModeOff) {
                            dlssLoggedMode = dlss::kModeOff;
                            logf("DLSS off: the GI's own filter at %ux%u", set.width, set.height);
                        }
                        memcpy(giCbCpu + uint64_t(k) * 512, &g, sizeof(g));
                        lastGi = g;
                        const UINT gx = (set.width + 7) / 8, gy = (set.height + 7) / 8;
                        const UINT rgx = (rw + 7) / 8, rgy = (rh + 7) / 8;  // the GI's rays and the guides
                        cl->SetComputeRootSignature(giRs);
                        cl->SetComputeRootShaderResourceView(1, tlas.address(k));
                        cl->SetComputeRootShaderResourceView(2, tlas.instance_table(k));
                        cl->SetComputeRootConstantBufferView(3, giCb->GetGPUVirtualAddress() + uint64_t(k) * 512);
                        cl->SetComputeRootDescriptorTable(4, table(kGeoPages));
                        if (cacheValue) {
                            cl->SetComputeRootUnorderedAccessView(5, cacheKeys->GetGPUVirtualAddress());
                            cl->SetComputeRootUnorderedAccessView(6, cacheAccum->GetGPUVirtualAddress());
                            cl->SetComputeRootUnorderedAccessView(7, cacheValue->GetGPUVirtualAddress());
                            cl->SetComputeRootUnorderedAccessView(8, skyAccum->GetGPUVirtualAddress());
                            cl->SetComputeRootUnorderedAccessView(9, skyValue->GetGPUVirtualAddress());
                        }
                        cl->SetComputeRootUnorderedAccessView(10, countsVa);
                        if (const D3D12_GPU_VIRTUAL_ADDRESS mva = scene.materials_va()) cl->SetComputeRootShaderResourceView(13, mva);
                        cl->SetComputeRootDescriptorTable(14, table(kTableTextures));
                        if (probeFrame[k] == next && probePso) {  // the crosshair probe (Test.ProbeKey)
                            cl->SetPipelineState(probePso);
                            cl->SetComputeRootDescriptorTable(0, table(kTableGiTrace));
                            cl->Dispatch(1, 1, 1);
                            cl->ResourceBarrier(1, &cb);
                        }
                        if (skyOn) {  // the drawn sky: this frame's view of it, then the map and its mean
                            D3D12_RESOURCE_BARRIER all{};
                            all.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                            cl->SetComputeRootDescriptorTable(0, table(kTableGiTrace));
                            if (radianceOk) {
                                cl->SetPipelineState(skyUpdatePso);
                                cl->Dispatch((set.width / 4 + 8) / 8, (set.height / 4 + 8) / 8, 1);
                                cl->ResourceBarrier(1, &all);
                            }
                            cl->SetPipelineState(skyResolvePso);
                            cl->Dispatch(1, 1, 1);
                            cl->ResourceBarrier(1, &all);
                        }
                        if (cacheOn) {
                            // the world radiance cache: this frame's view of the previous frame's colour into it, then
                            // each cell's mean, before the rays read it
                            D3D12_RESOURCE_BARRIER all{};
                            all.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;  // every UAV access before, after
                            cl->SetComputeRootDescriptorTable(0, table(kTableGiTrace));
                            if (radianceOk) {
                                cl->SetPipelineState(cacheUpdatePso);
                                cl->Dispatch((set.width / 2 + 8) / 8, (set.height / 2 + 8) / 8, 1);
                                cl->ResourceBarrier(1, &all);
                            }
                            cl->SetPipelineState(cacheResolvePso);
                            cl->Dispatch(kCacheSlots / 64, 1, 1);
                            cl->ResourceBarrier(1, &all);
                        }
                        cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 9));
                        cl->SetPipelineState(giTracePso);
                        cl->SetComputeRootDescriptorTable(0, table(kTableGiTrace));
                        cl->Dispatch(rgx, rgy, 1);
                        cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 10));
                        partsStamped = true;
                        if (glassPso && glassFilterPso && set.glass && set.glass_raw) {
                            // the glass's light (glass_light.hlsl, then glass_filter.hlsl), on the trace's bindings; first the
                            // previous frame's raw light kept aside: the history it reprojects (read in place, it trailed
                            // behind the view)
                            const D3D12_RESOURCE_BARRIER h1[2] = {
                                transition(set.glass_raw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                transition(set.glass_prev, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
                            cl->ResourceBarrier(2, h1);
                            cl->CopyResource(set.glass_prev, set.glass_raw);
                            const D3D12_RESOURCE_BARRIER h2[2] = {
                                transition(set.glass_raw, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                                transition(set.glass_prev, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
                            cl->ResourceBarrier(2, h2);
                            cl->SetPipelineState(glassPso);
                            cl->SetComputeRootDescriptorTable(0, table(kTableGlass));
                            cl->SetComputeRootDescriptorTable(12, table(kTableGlassRefl));
                            cl->Dispatch(gx, gy, 1);
                            const D3D12_RESOURCE_BARRIER g1 = transition(set.glass_raw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            cl->ResourceBarrier(1, &g1);
                            cl->SetPipelineState(glassFilterPso);
                            cl->SetComputeRootDescriptorTable(0, table(kTableGlassFilter));
                            cl->Dispatch(gx, gy, 1);
                            const D3D12_RESOURCE_BARRIER g2 = transition(set.glass_raw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            cl->ResourceBarrier(1, &g2);
                            cl->SetComputeRootDescriptorTable(0, table(kTableGiTrace));
                        }
                        cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 11));
                        if (sunTintPso && set.sun_tint) {  // the sun through clear glass, on the trace's bindings
                            cl->SetPipelineState(sunTintPso);
                            cl->SetComputeRootDescriptorTable(0, table(kTableGiTrace));
                            cl->SetComputeRootDescriptorTable(12, table(kTableSunTint));
                            cl->Dispatch(gx, gy, 1);
                        }
                        cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 7));
                        if (fogSet) {
                            // The volumetric fog: what of the sun and the sky reaches each froxel (fog_inject.hlsl, into
                            // fog_vis[fogOut] from the other), the light along their columns (fog_integrate.hlsl), per pixel
                            // into the plugin's texture (fog_apply.hlsl). No fog (density 0): only the last, which writes
                            // none, so the texture never holds an older frame's.
                            const D3D12_RESOURCE_STATES uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                        srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                            const bool lit = g.fog[0] > 0.0f;
                            if (lit) {
                                const D3D12_RESOURCE_BARRIER f0 = transition(set.fog_vis[1 - fogOut], uav, srv);
                                cl->ResourceBarrier(1, &f0);
                                cl->SetPipelineState(fogInjectPso);
                                cl->SetComputeRootDescriptorTable(0, table(kTableFogInject + 6 * UINT(fogOut)));
                                cl->Dispatch((set.fog_x + 3) / 4, (set.fog_y + 3) / 4, kFogSlices / 4);
                                const D3D12_RESOURCE_BARRIER f1 = transition(set.fog_vis[fogOut], uav, srv);
                                cl->ResourceBarrier(1, &f1);
                                cl->SetPipelineState(fogIntegratePso);
                                cl->SetComputeRootDescriptorTable(0, table(kTableFogIntegrate + 6 * UINT(fogOut)));
                                cl->Dispatch((set.fog_x + 7) / 8, (set.fog_y + 7) / 8, 1);
                            }
                            const D3D12_RESOURCE_BARRIER f2 = transition(set.fog_ratio, uav, srv);
                            cl->ResourceBarrier(1, &f2);
                            cl->SetPipelineState(fogApplyPso);
                            cl->SetComputeRootDescriptorTable(0, table(kTableFogApply));
                            cl->Dispatch(gx, gy, 1);
                            const D3D12_RESOURCE_BARRIER f3[3] = {transition(set.fog_ratio, srv, uav), transition(set.fog_vis[0], srv, uav),
                                                                  transition(set.fog_vis[1], srv, uav)};
                            cl->ResourceBarrier(lit ? 3u : 1u, f3);
                            cl->SetComputeRootDescriptorTable(0, table(kTableGiTrace));
                            if (lit) {
                                fogOut = 1 - fogOut;
                                fogHistory = true;
                                ++fogFrames;
                            } else {
                                fogHistory = false;
                            }
                        }
                        cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 8));
                        fogStamped = true;
                        bool rrDone = false;
                        if (rrUse) {
                            // DLSS Ray Reconstruction (dlss.h) denoises this frame's traced irradiance with its guide
                            // images, in place of the temporal and wavelet passes. The frame it is (re)created at, or a
                            // failure, runs those instead.
                            const D3D12_RESOURCE_STATES uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                        srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                            const D3D12_RESOURCE_BARRIER r1 = transition(set.gi_traced, uav, srv);
                            cl->ResourceBarrier(1, &r1);
                            cl->SetComputeRootSignature(rrGuidesRs);
                            cl->SetComputeRootDescriptorTable(0, table(kTableRrGuides));
                            cl->SetComputeRootConstantBufferView(1, giCb->GetGPUVirtualAddress() + uint64_t(k) * 512);
                            cl->SetPipelineState(rrGuidesPso);
                            cl->Dispatch(rgx, rgy, 1);
                            const D3D12_RESOURCE_BARRIER r2[4] = {transition(set.rr_depth, uav, srv), transition(set.rr_motion, uav, srv),
                                                                  transition(set.rr_normal, uav, srv), transition(set.rr_albedo, uav, srv)};
                            cl->ResourceBarrier(4, r2);
                            // the view without its translation (camera-relative world) and the game's projection, row vectors
                            const float worldToView[16] = {c.right[0], c.up[0], c.forward[0], 0.0f, c.right[1], c.up[1], c.forward[1], 0.0f,
                                                           c.right[2], c.up[2], c.forward[2], 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
                            LARGE_INTEGER now{};
                            QueryPerformanceCounter(&now);
                            if (!rrFreq.QuadPart) QueryPerformanceFrequency(&rrFreq);
                            const double ms = rrLast.QuadPart ? double(now.QuadPart - rrLast.QuadPart) * 1000.0 / double(rrFreq.QuadPart) : 16.7;
                            rrLast = now;
                            dlss::Inputs rri;
                            rri.color = set.gi_traced;
                            rri.diffuse_albedo = set.rr_albedo;
                            rri.specular_albedo = set.rr_spec;
                            rri.normal_roughness = set.rr_normal;
                            rri.depth = set.rr_depth;
                            rri.motion = set.rr_motion;
                            rri.output = set.rr_out;
                            rri.width = rw;
                            rri.height = rh;
                            rri.out_width = set.width;
                            rri.out_height = set.height;
                            rri.mode = dlssMode;
                            rri.jitter_x = -jx;  // where the geometry moved (dlss.h)
                            rri.jitter_y = -jy;
                            rri.world_to_view = worldToView;
                            rri.view_to_clip = h->camera.projection;
                            rri.reset = !historyValid || set.generation != rrGeneration || dlssReset;
                            dlssReset = false;
                            rri.frame_ms = float(ms < 1.0 ? 1.0 : ms > 100.0 ? 100.0 : ms);
                            rrGeneration = set.generation;
                            const bool evaluated = dlss::evaluate(cl, rri);
                            cl->SetDescriptorHeaps(1, &heap);  // NGX sets its own
                            const D3D12_RESOURCE_BARRIER r3[4] = {transition(set.rr_depth, srv, uav), transition(set.rr_motion, srv, uav),
                                                                  transition(set.rr_normal, srv, uav), transition(set.rr_albedo, srv, uav)};
                            cl->ResourceBarrier(4, r3);
                            // the GI's root signature and its parameters again (NGX changed them)
                            cl->SetComputeRootSignature(giRs);
                            cl->SetComputeRootShaderResourceView(1, tlas.address(k));
                            cl->SetComputeRootShaderResourceView(2, tlas.instance_table(k));
                            cl->SetComputeRootConstantBufferView(3, giCb->GetGPUVirtualAddress() + uint64_t(k) * 512);
                            cl->SetComputeRootDescriptorTable(4, table(kGeoPages));
                            if (cacheValue) {
                                cl->SetComputeRootUnorderedAccessView(5, cacheKeys->GetGPUVirtualAddress());
                                cl->SetComputeRootUnorderedAccessView(6, cacheAccum->GetGPUVirtualAddress());
                                cl->SetComputeRootUnorderedAccessView(7, cacheValue->GetGPUVirtualAddress());
                                cl->SetComputeRootUnorderedAccessView(8, skyAccum->GetGPUVirtualAddress());
                                cl->SetComputeRootUnorderedAccessView(9, skyValue->GetGPUVirtualAddress());
                            }
                            cl->SetComputeRootUnorderedAccessView(10, countsVa);
                            if (const D3D12_GPU_VIRTUAL_ADDRESS mva = scene.materials_va()) cl->SetComputeRootShaderResourceView(13, mva);
                            cl->SetComputeRootDescriptorTable(14, table(kTableTextures));
                            if (evaluated) {  // its output into the irradiance texture, the sky left to the game's ambient
                                const D3D12_RESOURCE_BARRIER r4 = transition(set.rr_out, uav, srv);
                                cl->ResourceBarrier(1, &r4);
                                cl->SetPipelineState(rrFinalizePso);
                                cl->SetComputeRootDescriptorTable(0, table(kTableRrFinalize));
                                cl->Dispatch(gx, gy, 1);
                                const D3D12_RESOURCE_BARRIER r5[2] = {transition(set.rr_out, srv, uav), transition(set.gi_traced, srv, uav)};
                                cl->ResourceBarrier(2, r5);
                                rrDone = true;
                                ++rrFrames;
                            } else {
                                const D3D12_RESOURCE_BARRIER r6 = transition(set.gi_traced, srv, uav);
                                cl->ResourceBarrier(1, &r6);
                            }
                        }
                        // Without Ray Reconstruction's answer the GI's own passes run, at the frame's size; a frame traced
                        // at a smaller one (its feature made at this frame, for a new mode) keeps the last irradiance.
                        if (!rrDone && !upscaling) {
                            const D3D12_RESOURCE_BARRIER giToTemporal[2] = {
                                transition(set.gi_traced, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                transition(set.gi_history[1 - histOut], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
                            cl->ResourceBarrier(2, giToTemporal);
                            cl->SetPipelineState(giTemporalPso);
                            cl->SetComputeRootDescriptorTable(0, table(kTableGiTemporal + 6 * UINT(histOut)));
                            cl->Dispatch(gx, gy, 1);
                            const D3D12_RESOURCE_BARRIER giToFilter =
                                transition(set.gi_history[histOut], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            cl->ResourceBarrier(1, &giToFilter);
                            if (giAtrousPso && set.gi_scratch && b->gi_denoiser >= 1) {
                                // the wavelet filter: history -> traced -> scratch -> traced -> irradiance, steps 1, 2, 4, 8
                                cl->SetPipelineState(giAtrousPso);
                                auto pass = [&](UINT tableAt, UINT step, UINT last) {
                                    const UINT pc[2] = {step, last};
                                    cl->SetComputeRoot32BitConstants(11, 2, pc, 0);
                                    cl->SetComputeRootDescriptorTable(0, table(tableAt));
                                    cl->Dispatch(gx, gy, 1);
                                };
                                const D3D12_RESOURCE_BARRIER a1 =
                                    transition(set.gi_traced, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                                cl->ResourceBarrier(1, &a1);
                                pass(kTableGiAtrous + 6 * UINT(histOut), 1, 0);
                                const D3D12_RESOURCE_BARRIER a2 =
                                    transition(set.gi_traced, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                cl->ResourceBarrier(1, &a2);
                                pass(kTableGiAtrous + 12, 2, 0);
                                const D3D12_RESOURCE_BARRIER a3[2] = {
                                    transition(set.gi_scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                    transition(set.gi_traced, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
                                cl->ResourceBarrier(2, a3);
                                pass(kTableGiAtrous + 18, 4, 0);
                                const D3D12_RESOURCE_BARRIER a4 =
                                    transition(set.gi_traced, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                cl->ResourceBarrier(1, &a4);
                                pass(kTableGiAtrous + 24, 8, 1);
                                const D3D12_RESOURCE_BARRIER a5 =
                                    transition(set.gi_scratch, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                                cl->ResourceBarrier(1, &a5);  // gi_traced ends a shader resource, as before
                            } else {
                                cl->SetPipelineState(giFilterPso);
                                cl->SetComputeRootDescriptorTable(0, table(kTableGiFilter + 6 * UINT(histOut)));
                                cl->Dispatch(gx, gy, 1);
                            }
                            const D3D12_RESOURCE_BARRIER giBack[3] = {
                                transition(set.gi_traced, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                                transition(set.gi_history[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                                transition(set.gi_history[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
                            cl->ResourceBarrier(3, giBack);
                        }
                        ++giFrames;
                    }
                    // Reflections: one ray a pixel along the reflection vector, through GI's temporal and filter passes,
                    // into the texture the patched lit shaders read as their specular ambient.
                    cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 6));
                    if (reflTracePso && set.reflection && set.refl_traced) {
                        GiConstants g{};
                        g.base = c;
                        const bool seenOk = radiance && historyValid && rfgrt_ipc::load64(&b->radiance_frame) >= next - 1;
                        g.base.flags = depthFlag | historyFlag | (seenOk ? 16u : 0u) | frostedFlag | tintFlag;
                        g.base.shadow[1] = uint32_t(b->refl_filter_radius >= 0 ? b->refl_filter_radius : 0);
                        memcpy(g.ambient, h->ambient, sizeof(g.ambient));
                        memcpy(g.back_ambient, h->back_ambient, sizeof(g.back_ambient));
                        memcpy(g.sun_color, h->light_color, sizeof(g.sun_color));
                        g.gi[0] = b->refl_gloss;
                        g.gi[1] = b->refl_length;
                        g.gi[2] = b->gi_albedo;
                        g.gi[3] = float(b->refl_history > 1 ? b->refl_history : 1);
                        g.gi2[0] = 1.0f;
                        g.gi2[1] = b->gi_sky;
                        memcpy(g.world, h->render_offset, sizeof(float) * 3);
                        g.world[3] = cacheOn ? 1.0f : 0.0f;
                        g.sky_probe[0] = skyOn ? 1.0f : 0.0f;
                        g.sky_probe[1] = 0.990268f;
                        g.sky_probe[2] = b->gi_sky_drawn;
                        g.sky_probe[3] = std::tan(b->shadow_sun_angle_deg * 3.14159265f / 180.0f);  // the glass's sun
                        memcpy(g.terrain, b->terrain_albedo, sizeof(float) * 3);  // the terrain's albedo (GI.TerrainAlbedo)
                        memcpy(reflCbCpu + uint64_t(k) * 512, &g, sizeof(g));
                        const UINT gx = (set.width + 7) / 8, gy = (set.height + 7) / 8;
                        cl->SetComputeRootSignature(giRs);
                        cl->SetComputeRootShaderResourceView(1, tlas.address(k));
                        cl->SetComputeRootShaderResourceView(2, tlas.instance_table(k));
                        cl->SetComputeRootConstantBufferView(3, reflCb->GetGPUVirtualAddress() + uint64_t(k) * 512);
                        cl->SetComputeRootDescriptorTable(4, table(kGeoPages));
                        if (cacheValue) {
                            cl->SetComputeRootUnorderedAccessView(5, cacheKeys->GetGPUVirtualAddress());
                            cl->SetComputeRootUnorderedAccessView(6, cacheAccum->GetGPUVirtualAddress());
                            cl->SetComputeRootUnorderedAccessView(7, cacheValue->GetGPUVirtualAddress());
                            cl->SetComputeRootUnorderedAccessView(8, skyAccum->GetGPUVirtualAddress());
                            cl->SetComputeRootUnorderedAccessView(9, skyValue->GetGPUVirtualAddress());
                        }
                        cl->SetComputeRootUnorderedAccessView(10, countsVa);
                        if (const D3D12_GPU_VIRTUAL_ADDRESS mva = scene.materials_va()) cl->SetComputeRootShaderResourceView(13, mva);
                        cl->SetComputeRootDescriptorTable(14, table(kTableTextures));
                        cl->SetPipelineState(reflTracePso);
                        cl->SetComputeRootDescriptorTable(0, table(kTableReflTrace));
                        cl->Dispatch(gx, gy, 1);
                        const D3D12_RESOURCE_BARRIER rToTemporal[2] = {
                            transition(set.refl_traced, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                            transition(set.refl_history[1 - histOut], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
                        cl->ResourceBarrier(2, rToTemporal);
                        cl->SetPipelineState(giTemporalPso);
                        cl->SetComputeRootDescriptorTable(0, table(kTableReflTemporal + 6 * UINT(histOut)));
                        cl->Dispatch(gx, gy, 1);
                        const D3D12_RESOURCE_BARRIER rToFilter =
                            transition(set.refl_history[histOut], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        cl->ResourceBarrier(1, &rToFilter);
                        cl->SetPipelineState(giFilterPso);
                        cl->SetComputeRootDescriptorTable(0, table(kTableReflFilter + 6 * UINT(histOut)));
                        cl->Dispatch(gx, gy, 1);
                        const D3D12_RESOURCE_BARRIER rBack[3] = {
                            transition(set.refl_traced, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                            transition(set.refl_history[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                            transition(set.refl_history[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
                        cl->ResourceBarrier(3, rBack);
                        ++reflFrames;
                    }
                    // back to where the next frame starts, and this frame's depth becomes its previous one
                    const D3D12_RESOURCE_BARRIER afterPasses[5] = {
                        transition(set.vis, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                        transition(set.history[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                        transition(set.history[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                        transition(set.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
                        transition(set.prev_depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
                    cl->ResourceBarrier(5, afterPasses);
                    cl->CopyResource(set.prev_depth, set.depth);
                    const D3D12_RESOURCE_BARRIER afterCopy[2] = {
                        transition(set.depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                        transition(set.prev_depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
                    cl->ResourceBarrier(2, afterCopy);
                    memcpy(prevView, h->camera.view, sizeof(prevView));
                    memcpy(prevProj, h->camera.projection, sizeof(prevProj));
                    memcpy(prevOffset, h->render_offset, sizeof(prevOffset));
                    prevRange = c.range[0];
                    historyValid = true;
                    histOut = 1 - histOut;
                } else {
                    cl->SetPipelineState(comparePso);
                    cl->SetComputeRootSignature(compareRs);
                    cl->SetComputeRootDescriptorTable(0, table(kTableCompare));
                    cl->SetComputeRootShaderResourceView(1, tlas.address(k));
                    cl->SetComputeRootUnorderedAccessView(2, countsVa);
                    cl->SetComputeRootShaderResourceView(4, tlas.instance_table(k));
                    c.flags = 1;  // clear the counters
                    cl->SetComputeRoot32BitConstants(3, UINT(sizeof(c) / 4), &c, 0);
                    cl->Dispatch(1, 1, 1);
                    cl->ResourceBarrier(1, &cb);
                    c.flags = reversed ? 2u : 0u;
                    cl->SetComputeRoot32BitConstants(3, UINT(sizeof(c) / 4), &c, 0);
                    cl->Dispatch((set.width + 7) / 8, (set.height + 7) / 8, 1);
                }
            } else {
                cl->SetPipelineState(patternPso);
                cl->SetComputeRootSignature(patternRs);
                cl->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
                const UINT constants[4] = {UINT(next), UINT(rfgrt_ipc::kModeDepthPattern), set.width, set.height};
                cl->SetComputeRoot32BitConstants(1, 4, constants, 0);
                cl->Dispatch((set.width + 7) / 8, (set.height + 7) / 8, 1);
            }
            if (!giStamped) {
                cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 5));
                cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 4));
                cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 6));
            }
            if (!fogStamped) {
                cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 7));
                cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 8));
            }
            if (!partsStamped)
                for (UINT s = 9; s <= 11; ++s) cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + s));
            cl->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps + 3));
            D3D12_RESOURCE_BARRIER out[16 + rfgrt_ipc::kMaxLightMasks] = {
                transition(set.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                transition(set.result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
                transition(set.game_clb, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                transition(counts, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition(set.irradiance, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)};
            UINT outCount = set.irradiance ? 5 : 4;
            for (ID3D12Resource* m : set.light_masks)
                if (m) out[outCount++] = transition(m, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            if (radiance) out[outCount++] = transition(radiance, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            if (set.reflection) out[outCount++] = transition(set.reflection, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            if (set.glass) out[outCount++] = transition(set.glass, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            if (set.glass_refl)
                out[outCount++] = transition(set.glass_refl, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            if (set.sun_tint)
                out[outCount++] = transition(set.sun_tint, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            if (set.fog) out[outCount++] = transition(set.fog, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            cl->ResourceBarrier(outCount, out);
            cl->ResolveQueryData(qheap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(k * kStamps), kStamps, readback, UINT64(k) * kReadback);
            cl->CopyBufferRegion(readback, UINT64(k) * kReadback + kStamps * 8, counts, UINT64(k) * kCounts, kCounts);
            const D3D12_RESOURCE_BARRIER back = transition(counts, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl->ResourceBarrier(1, &back);
            cl->Close();
            queue->Wait(fenceA, UINT64(next));
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(fenceB, UINT64(next));
            n = sig = next;
            slotFrame[k] = next;
            ++queued;
            rfgrt_ipc::store64(&b->host_submitted, sig);  // queued; fence B reaches it when the GPU has run it
            did = true;
        }
        if (GetTickCount64() - lastLog >= 10000) {
            lastLog = GetTickCount64();
            scene.log_stats();
            skinning.log_stats();
            if (st.frames) {
                const double f = double(st.frames);
                uint64_t px = 0;
                for (uint64_t c : st.classes) px += c;
                logf("frames: %llu answered; GPU per frame: TLAS %.0f us, pass %.0f us (GI %.0f us, lights' shadows %.0f us, "
                     "reflections %.0f us of it); TLAS %.0f of %.0f scene instances (left out: %.0f with no mesh here, %.0f with "
                     "their submesh's BLAS not built, %.0f with a submesh beyond their mesh as sent), the nearest %.1f m from the "
                     "camera; scenes skipped: %llu read while overwritten, %llu before the frame textures",
                     static_cast<unsigned long long>(st.frames), st.tlas_us / f, st.pass_us / f, st.gi_us / f, st.light_us / f,
                     st.refl_us / f,
                     double(st.in_tlas) / f,
                     double(st.instances) / f,
                     tlas.stats.frames ? double(tlas.stats.missing_mesh) / double(tlas.stats.frames) : 0.0,
                     tlas.stats.frames ? double(tlas.stats.missing_blas) / double(tlas.stats.frames) : 0.0,
                     tlas.stats.frames ? double(tlas.stats.submesh_beyond) / double(tlas.stats.frames) : 0.0,
                     tlas.stats.frames ? tlas.stats.nearest_sum / double(tlas.stats.frames) : -1.0,
                     static_cast<unsigned long long>(torn), static_cast<unsigned long long>(skippedNoSet));
                if (rrOn) logf("GI denoiser: DLSS Ray Reconstruction denoised %llu frames so far", static_cast<unsigned long long>(rrFrames));
                logf("GI's parts per frame: before the rays (probe, drawn sky, radiance cache) %.0f us, the rays %.0f us, the "
                     "glass's light %.0f us, the sun through clear glass %.0f us, the fog %.0f us, the denoiser %.0f us",
                     st.prep_us / f, st.trace_us / f, st.glass_us / f, st.tint_us / f, st.fog_us / f, st.denoise_us / f);
                if (glassPso && set.glass_raw)
                    logf("glass: per frame %.0f pixels see glass in front of their surface, %.0f of them with the sun on it",
                         double(st.glass_px) / f, double(st.glass_lit) / f);
                if (fogApplyPso && set.fog_ratio) {
                    logf("fog: volumetric in %llu frames, GPU %.0f us a frame (of GI's); the game's fog: density %.6f, height falloff "
                         "%.6f, start %.4f, colour (%.3f %.3f %.3f); grid %ux%ux%u out to %.0f m",
                         static_cast<unsigned long long>(fogFrames), st.fog_us / f, lastGi.fog[0], lastGi.fog[1], lastGi.fog[2],
                         lastGi.fog_color[0], lastGi.fog_color[1], lastGi.fog_color[2], set.fog_x, set.fog_y, kFogSlices, lastGi.fog2[2]);
                    fogFrames = 0;
                }
                if (tlas.stats.frames)
                    logf("TLAS glass (rays go through transparent materials): per frame %.1f instances all of it (non-opaque "
                         "panes; %.1f of them out of the rays' mask: more than 23 geometries), %.1f with some (non-opaque), %.1f "
                         "with translucent glass (a share of the rays through); %.1f of the glassy ones classed by their mesh's "
                         "materials, every entry",
                         double(tlas.stats.glass_whole) / double(tlas.stats.frames),
                         double(tlas.stats.glass_whole_out) / double(tlas.stats.frames),
                         double(tlas.stats.glass_partial) / double(tlas.stats.frames),
                         double(tlas.stats.glass_translucent) / double(tlas.stats.frames),
                         double(tlas.stats.glass_by_mesh) / double(tlas.stats.frames));
                if (tlas.stats.frames) {
                    const double f = double(tlas.stats.frames);
                    logf("TLAS materials: per frame instances whose mesh has none (a plain grey for the rays): meshes %.1f, "
                         "skinned %.1f, Geo-Mod pieces %.1f, other %.1f, terrain %.1f (its own shader: the radiance cache "
                         "in mirrors)",
                         double(tlas.stats.no_materials[1]) / f, double(tlas.stats.no_materials[2]) / f,
                         double(tlas.stats.no_materials[3]) / f, double(tlas.stats.no_materials[4] + tlas.stats.no_materials[0]) / f,
                         double(tlas.stats.no_materials[5]) / f);
                }
                if (tlas.stats.glass_mesh_count) {
                    char names[6 * 80] = {};
                    size_t at = 0;
                    for (uint32_t m = 0; m < tlas.stats.glass_mesh_count && at < sizeof(names) - 80; ++m)
                        at += size_t(snprintf(names + at, sizeof(names) - at, "%s\"%s\" (%s)", m ? ", " : "",
                                              scene.mesh_name(tlas.stats.glass_meshes[m]),
                                              (tlas.stats.glass_whole_bits >> m) & 1u ? "all of it" : "some ranges"));
                    logf("TLAS glass, meshes: %s", names);
                }
                if (tlas.stats.missing_blas || tlas.stats.submesh_beyond)
                    logf("TLAS left out, a sample of each: BLAS not built: mesh %u submesh %u (kind %u); beyond: mesh %u submesh %u "
                         "of %u", tlas.stats.sample_blas[0], tlas.stats.sample_blas[1], tlas.stats.sample_blas[2],
                         tlas.stats.sample_beyond[0], tlas.stats.sample_beyond[1], tlas.stats.sample_beyond[2]);
                tlas.stats = Tlas::Stats{};
                if (mode == rfgrt_ipc::kModeDepthCompare) {
                    logf("camera (view matrix): right (%.3f %.3f %.3f) up (%.3f %.3f %.3f) forward (%.3f %.3f %.3f) at (%.2f %.2f %.2f), "
                         "render offset (%.1f %.1f %.1f)",
                         lastCamera.right[0], lastCamera.right[1], lastCamera.right[2], lastCamera.up[0], lastCamera.up[1],
                         lastCamera.up[2], lastCamera.forward[0], lastCamera.forward[1], lastCamera.forward[2], lastCamera.origin[0],
                         lastCamera.origin[1], lastCamera.origin[2], lastOffset[0], lastOffset[1], lastOffset[2]);
                    logf("camera (world matrix, render offset taken off): right (%.3f %.3f %.3f) up (%.3f %.3f %.3f) forward (%.3f %.3f %.3f) "
                         "at (%.2f %.2f %.2f)",
                         worldBasis[0][0], worldBasis[0][1], worldBasis[0][2], worldBasis[1][0], worldBasis[1][1], worldBasis[1][2],
                         worldBasis[2][0], worldBasis[2][1], worldBasis[2][2], worldBasis[3][0], worldBasis[3][1], worldBasis[3][2]);
                    auto rows = [](const char* what, const float* m) {
                        logf("%s rows [%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g]", what, m[0], m[1], m[2], m[3], m[4], m[5],
                             m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
                    };
                    rows("camera view", lastView);
                    rows("camera projection", lastProj);
                    rows("renderer view (at render's start)", lastStateView);
                    rows("renderer projection (at render's start)", lastStateProj);
                }
                if (reflFrames) {
                    logf("reflections: %llu frames", static_cast<unsigned long long>(reflFrames));
                    reflFrames = 0;
                }
                if (lightFrames) {
                    logf("dynamic lights: shadow masks traced in %llu frames, %.1f lights a frame",
                         static_cast<unsigned long long>(lightFrames), double(lightsTraced) / double(lightFrames));
                    lightFrames = 0;
                    lightsTraced = 0;
                }
                if (giFrames) {
                    logf("global illumination: %llu frames (%llu with the previous frame's lit colour for the hits); the game's "
                         "lighting (its shaders' units): ambient (%.3f %.3f %.3f), back ambient (%.3f %.3f %.3f), sun (%.3f %.3f %.3f, "
                         "scale %.3f)",
                         static_cast<unsigned long long>(giFrames), static_cast<unsigned long long>(radianceFrames), lastGi.ambient[0],
                         lastGi.ambient[1], lastGi.ambient[2],
                         lastGi.back_ambient[0], lastGi.back_ambient[1], lastGi.back_ambient[2], lastGi.sun_color[0],
                         lastGi.sun_color[1], lastGi.sun_color[2], lastGi.sun_color[3]);
                    giFrames = 0;
                    radianceFrames = 0;
                }
                if (giCensus) {
                    static const char* const kCensusClass[5] = {"up, in the sun", "up, in shade", "sideways, in the sun",
                                                                "sideways, in shade", "down (ceilings, undersides)"};
                    for (int cc = 0; cc < 5; ++cc) {
                        const uint64_t* v = census + cc * 5;
                        if (!v[0]) continue;
                        const double nn = double(v[0]) * 256.0;
                        logf("GI census, %s: %llu pixels; mean luminance in the game's units: the traced irradiance %.3f, the "
                             "game's lit colour %.3f, the sun's irradiance %.3f, the game's own ambient for that normal %.3f",
                             kCensusClass[cc], static_cast<unsigned long long>(v[0]), double(v[1]) / nn, double(v[2]) / nn,
                             double(v[3]) / nn, double(v[4]) / nn);
                    }
                    memset(census, 0, sizeof(census));
                }
                uint64_t both = 0;
                for (uint64_t r : st.ratio) both += r;
                if (both) {
                    char hist[512];
                    int at = 0;
                    for (int i = 0; i < 16 && at < int(sizeof(hist)) - 16; ++i)
                        at += snprintf(hist + at, sizeof(hist) - size_t(at), " %.1f", 100.0 * double(st.ratio[i]) / double(both));
                    logf("depth compare: mirror/game depth where both have a surface, %% per bucket of 2^(1/4) from 1/4 to 4:%s", hist);
                }
                if (mode == rfgrt_ipc::kModeDepthCompare) {
                    std::string line;
                    for (int i = 0; i < 32; ++i) {
                        char item[64];
                        snprintf(item, sizeof(item), " [%d: d %.6f, mirror %.2f, game %.2f]", i, st.samples[i * 3], st.samples[i * 3 + 1],
                                 st.samples[i * 3 + 2]);
                        line += item;
                        if (i % 8 == 7) {
                            logf("depth samples%s", line.c_str());
                            line.clear();
                        }
                    }
                }
                const uint64_t surfaces = st.shadow[0] + st.shadow[1] + st.shadow[2] + st.shadow[3];
                if (surfaces) {
                    auto pct = [&](uint64_t v) { return 100.0 * double(v) / double(surfaces); };
                    logf("sun shadows against the game's cascades (of the pixels with a surface): agree %.2f%% (both lit %.2f%%, "
                         "both shadowed %.2f%%), only ours shadowed %.2f%%, only the game's %.2f%%; sky %.2f%% of all; sun (%.3f %.3f %.3f)",
                         pct(st.shadow[0] + st.shadow[1]), pct(st.shadow[0]), pct(st.shadow[1]), pct(st.shadow[2]), pct(st.shadow[3]),
                         100.0 * double(st.shadow[8]) / double(surfaces + st.shadow[8]), lastCamera.sun[0], lastCamera.sun[1], lastCamera.sun[2]);
                    if (b->shadow_history > 1)
                        logf("sun shadow history (of the pixels with a surface): kept %.2f%%, of which bounded by this frame's "
                             "neighbourhood %.2f%%", pct(st.shadow[4]), pct(st.shadow[5]));
                }
                if (px)
                    logf("depth compare: match %.2f%%, sky %.2f%%, missing %.2f%%, mirror in front %.2f%%, mirror behind %.2f%% (pixels)",
                         100.0 * double(st.classes[0]) / double(px), 100.0 * double(st.classes[1]) / double(px),
                         100.0 * double(st.classes[2]) / double(px), 100.0 * double(st.classes[3]) / double(px),
                         100.0 * double(st.classes[4]) / double(px));
                st.clear();
            }
        }
        if (!did) {
            // Nothing to do: wake on a published scene, on the GPU finishing what we wait for, or soon anyway.
            HANDLE waits[2];
            DWORD count = 0;
            if (sceneEvent) waits[count++] = sceneEvent;
            if (waitFor) {
                fenceB->SetEventOnCompletion(waitFor, fenceEvent);
                waits[count++] = fenceEvent;
            }
            if (count)
                WaitForMultipleObjects(count, waits, FALSE, 2);
            else
                Sleep(1);
        }
    }
    logf("stopping at frame %lld (%s)", static_cast<long long>(sig),
         lost ? "the GPU device was lost" : b->stop ? "the plugin asked" : "the plugin's process ended");
    scene.log_stats();
    // Frames queued past the plugin's last signal wait on fence A for good, and a device with work that can never
    // finish cannot be released (it hung the host at the end of every session). The plugin is stopping or gone, and
    // nothing it does from here depends on those frames: fence A is raised from here, the queue drains, and fence B
    // reaches every value the plugin may still wait for. A lost device has nothing left to drain (its fences read
    // UINT64_MAX), and the plugin releases its own waits on fence B once this thread has ended.
    if (!lost) {
        fenceA->Signal(1ull << 62);
        if (sig > 0) {
            fenceB->SetEventOnCompletion(UINT64(sig), fenceEvent);
            if (WaitForSingleObject(fenceEvent, 1000) != WAIT_OBJECT_0) logf("the frame queue did not drain within 1 s");
        }
    }
    scene.set_frames(0, UINT64(sig));
    scene.shutdown(500);
    skinning.release();
    tlas.release();
    if (!lost) b->host_state = rfgrt_ipc::kHostExited;  // a lost device keeps kHostFailed and its message
    CloseHandle(fenceEvent);
    if (sceneEvent) CloseHandle(sceneEvent);
    if (fenceBHandle) CloseHandle(fenceBHandle);
    release(dev5);
    release(set.depth);
    release(set.result);
    release(set.game_clb);
    release(set.vis);
    release(set.history[0]);
    release(set.history[1]);
    release(set.prev_depth);
    release(set.irradiance);
    release(set.gi_traced);
    release(set.gi_scratch);
    for (ID3D12Resource** r : {&set.rr_depth, &set.rr_motion, &set.rr_normal, &set.rr_albedo, &set.rr_spec, &set.rr_out})
        release(*r);
    dlss::shutdown();
    release(set.gi_history[0]);
    release(set.gi_history[1]);
    for (auto& m : set.light_masks) release(m);
    release(radiance);
    release(set.reflection);
    release(set.refl_traced);
    release(set.refl_history[0]);
    release(set.refl_history[1]);
    release(set.glass);
    release(set.glass_raw);
    release(set.glass_prev);
    release(set.glass_refl);
    release(set.sun_tint);
    release(sunTintPso);
    release(glassPso);
    release(glassFilterPso);
    release(set.fog);
    release(set.fog_vis[0]);
    release(set.fog_vis[1]);
    release(set.fog_ratio);
    release(fogInjectPso);
    release(fogIntegratePso);
    release(fogApplyPso);
    if (reflCb && reflCbCpu) reflCb->Unmap(0, nullptr);
    release(reflCb);
    release(reflTracePso);
    if (lightCb && lightCbCpu) lightCb->Unmap(0, nullptr);
    release(lightCb);
    release(lightPso);
    release(lightRs);
    if (giCb && giCbCpu) giCb->Unmap(0, nullptr);
    release(giCb);
    release(giTracePso);
    release(cacheUpdatePso);
    release(cacheResolvePso);
    release(cacheKeys);
    release(cacheAccum);
    release(cacheValue);
    release(skyUpdatePso);
    release(skyResolvePso);
    release(skyAccum);
    release(skyValue);
    release(probePso);
    release(rrGuidesPso);
    release(rrFinalizePso);
    release(rrGuidesRs);
    release(giTemporalPso);
    release(giFilterPso);
    release(giAtrousPso);
    release(giRs);
    release(counts);
    release(readback);
    release(qheap);
    release(cl);
    for (auto& a : alloc) release(a);
    release(queue);
    release(heap);
    release(comparePso);
    release(filterPso);
    release(filterRs);
    release(temporalPso);
    release(temporalRs);
    release(patternPso);
    release(compareRs);
    release(patternRs);
    release(fenceB);
    release(fenceA);
    release(dev);
    release(adapter);
    release(factory);
    return lost ? 1 : 0;
}

}  // namespace rtcore
