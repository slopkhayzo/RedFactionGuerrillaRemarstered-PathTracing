#pragma once
#include <d3d11_4.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "frame_scene.h"
#include "rfgrt_ipc.h"
#include "scene_ring.h"

// The plugin's side of the ray tracing core (src/rtcore): the shared region, the GPU resources shared with the core,
// and the core's lifetime. Today's 32-bit rfg.exe gets no hardware ray tracing in its own process, so the core runs in
// rfgrt_host.exe (64-bit), linked by named shared memory; a 64-bit rfg.exe runs the core on a thread of this process
// instead (RFGRT_LOCAL_CORE builds). Either way the protocol is rfgrt_ipc.h's.
//
// The core starts at the first mesh the game creates (or the first frame, without the mirror), and lives until the game
// ends or the core dies. A game that switches path tracing off stops it (stop(): core/active.h) and starts another
// later, which the scene mirror then gives every live mesh again (mirror/geometry.h).
namespace rtlink {

struct Options {
    int mode = rfgrt_ipc::kModeDepthPattern;
    std::wstring host_exe;   // rfgrt_host.exe, when the core runs in its own process
    std::wstring log_path;   // the core's log
    uint32_t ring_mb = 32;   // the scene ring (a power of two)
    bool snapshots = false;  // frame scenes (instances and camera) every frame
    uint32_t max_instances = 32768;
    uint32_t max_skins = 512;     // skinned instances' palettes a frame scene holds (0: none)
    uint32_t max_bones = 16384;
    int shadow_rays = 4;
    float shadow_sun_angle = 0.5f;
    int shadow_filter_radius = 2;
    int shadow_history = 12;
    int ao_rays = 0;          // 0: no ambient occlusion
    float ao_radius = 2.0f;
    int debug_view = 0;
    int gi_rays = 0;          // 0: no global illumination (no irradiance texture)
    float gi_length = 60.0f;
    float gi_albedo = 0.35f;
    int gi_history = 24;
    int gi_filter_radius = 3;
    float gi_bounce = 1.0f;
    float gi_sun_bounce_albedo = 0.0f;  // GI.SunBounceAlbedo (rfgrt_ipc::Block::gi_sun_bounce_albedo)
    float gi_shade_bounce_albedo = 0.0f;  // GI.ShadeBounceAlbedo (rfgrt_ipc::Block::gi_shade_bounce_albedo)
    float gi_sky = 1.0f;
    bool gi_cache = true;  // the world radiance cache (GI.Cache)
    bool gi_glass = true;  // the glass's light (GI.Glass): a texture the core writes, the patched glass shader reads at t17
    bool refl_glass = true;       // what the clear glass reflects (Reflections.Glass, with GI.Glass): another, read at t19
    float refl_glass_f0 = 0.2f;   // the clear glass's reflectance at normal incidence (Reflections.GlassReflectance)
    bool frosted_glass = true;    // the translucent glass diffuses the light through it (GI.FrostedGlass)
    bool window_tint = true;      // the clear glass tints the sun's light through it (GI.WindowTint): a texture at t20
    bool tlas_fast_trace = true;  // the TLAS built for fast tracing (Mirror.TlasFastTrace)
    bool dred = true;             // the core's device keeps its removed-device data (Host.Dred)
    float terrain_albedo[3] = {0.032f, 0.010f, 0.002f};  // what the rays take for the terrain's albedo (GI.TerrainAlbedo)
    int gi_denoiser = 2;  // the GI's denoiser (GI.Denoiser: 0 one pass, 1 wavelet, 2 Ray Reconstruction)
    int dlss_mode = 1;    // Ray Reconstruction's mode (DLSS.Mode; rfgrt_ipc::Block::dlss_mode)
    bool fog = false;     // the volumetric fog (Fog.Enabled, with GI): a texture the core writes, the plugin blends
    float fog_sun_share = 0.65f;
    float fog_anisotropy = 0.25f;
    float fog_density = 1.0f;  // Test.FogDensity
    float fog_near = 0.5f;     // Fog.Near
    int fog_history = 20;      // Fog.History
    int fog_rays = 1;          // Fog.Rays
    bool fog_filter = true;    // Fog.Filter
    bool fog_glass = true;     // Fog.Glass: the fog in front of the glass, a texture the core writes, the glass shader reads at t21
    float gi_sky_drawn = 1.0f;  // the drawn sky for rays that get out (GI.SkyDrawn)
    int light_masks = 0;      // shadow masks of dynamic lights (0: none)
    bool refl_enabled = false;  // reflections (a texture the core writes, the patched lit shaders read at t16)
    float refl_length = 100.0f;
    float refl_gloss = 32.0f;
    int refl_history = 6;
    int refl_filter_radius = 1;
    bool in_process = false; // 64-bit builds: the core on a thread of this process
};

class Link {
public:
    void configure(const Options& o) { opt_ = o; }

    // Starts the core: the shared region, fence A, then the process or thread. Thread-safe; returns at once (the
    // core gets ready on its own). False when it cannot start, now or at an earlier attempt.
    bool start(ID3D11Device* device);
    // Asks the core to stop and waits up to `wait_ms` for it to exit (its log complete). Nothing is released: the
    // session is ending, and the render thread may still be using the link (ring writes fail from now on). The host
    // process ends with the game in any case (job object).
    void request_stop(uint32_t wait_ms);
    // Stops the core and releases all the link holds (the shared region, fences, frame textures, events, threads), so
    // that start() starts a new core: path tracing switched off (core/active.h). Call it once no thread but the scene
    // ring's writers uses the link (path tracing off, its frame work finished); those are waited out. True when the
    // core ended within `wait_ms`; otherwise it keeps what it may still read and no core starts again.
    bool stop(uint32_t wait_ms);

    // DLSS's mode (rfgrt_ipc::Block::dlss_mode), for the cores that start from now on and the one running, which switches
    // at its next frame. Any thread.
    void set_dlss_mode(int mode);

    bool started() const { return started_.load(); }
    // The core has published fence B and reads the ring. Render thread (opens fence B the first time).
    bool ready();
    // The core ended without being asked (its process died, its thread returned, or it failed to start); the monitor
    // has released any GPU wait on fence B by then.
    bool dead() const { return dead_.load(); }
    // Ends the core's process as a crash would (tests the fallback).
    void kill_for_test();

    // --- frames (render thread) ---
    // Makes the per-frame textures at this size if they are not, for frames from `first_frame` on. False on failure.
    bool ensure_frame_resources(uint32_t width, uint32_t height, int64_t first_frame);
    int32_t generation() const { return generation_; }
    rfgrt_ipc::Block* block() { return block_; }
    // Asks the core to log what the rays under the crosshair meet, at its next frame (Test.ProbeKey).
    void request_probe() {
        if (block_) block_->probe_request = block_->probe_request + 1;
    }
    ID3D11RenderTargetView* depth_rtv() const { return depth_rtv_; }
    ID3D11ShaderResourceView* result_srv() const { return result_srv_; }
    // The core's irradiance (GI), for the patched lit shaders' t14; null without GI.
    ID3D11ShaderResourceView* irradiance_srv() const { return irradiance_srv_; }
    ID3D11ShaderResourceView* radiance_srv() const { return radiance_srv_; }  // the game's lit colour (DebugView 3)
    // The core's reflections, for the patched lit shaders' t16; null without them.
    ID3D11ShaderResourceView* reflection_srv() const { return reflection_srv_; }
    // The light of the nearest glass in front of each pixel (shaders/glass_light.hlsl), for the patched glass shader's
    // t17; null without GI.
    ID3D11ShaderResourceView* glass_srv() const { return glass_srv_; }
    // What the clear glass in front of each pixel reflects (Reflections.Glass; glass_light.hlsl), for the patched glass
    // shader's t19 (null: none)
    ID3D11ShaderResourceView* glass_refl_srv() const { return glass_refl_srv_; }
    // The volumetric fog between the camera and the clear glass in front of each pixel (fog_apply.hlsl), for the patched
    // glass shader's t21 (null: none, the game's fog on the glass)
    ID3D11ShaderResourceView* glass_fog_srv() const { return glass_fog_srv_; }
    // What of the sun's light the clear glass lets through to each pixel's surface (GI.WindowTint; shaders/sun_tint.hlsl),
    // for the patched lit shaders' t20 (null: none)
    ID3D11ShaderResourceView* sun_tint_srv() const { return sun_tint_srv_; }
    // The volumetric fog in front of each pixel's surface (shaders/fog_apply.hlsl), blended over the lit scene; null
    // without it.
    ID3D11ShaderResourceView* fog_srv() const { return fog_srv_; }
    // The dynamic lights' shadow masks (render/lights.h), and a white one (lit everywhere) at the same size.
    int light_masks() const { return light_mask_count_; }
    ID3D11ShaderResourceView* light_mask_srv(int i) const { return i >= 0 && i < light_mask_count_ ? light_mask_srv_[i] : nullptr; }
    ID3D11ShaderResourceView* white_srv() const { return white_srv_; }
    // The game's lit colour of this frame (its scene target after the lighting passes) into the texture the core's GI
    // reads, recorded on `ctx` (the scene's context); the texture is made, or remade, at the target's size and format.
    // `frame`: the frame whose colour it is (the render() under way while its passes are recorded).
    bool capture_radiance(ID3D11DeviceContext* ctx, ID3D11Texture2D* target, int64_t frame);
    ID3D11Texture2D* game_clb() const { return game_clb_; }  // receives the game's ClbTexture each frame
    ID3D11Fence* fence_a() const { return fence_a_; }
    ID3D11Fence* fence_b() const { return fence_b_; }

    // --- frame scenes (render thread) ---
    // Frame n's slot, to fill and then publish; nullptr without snapshots or before the core is ready.
    rfgrt_frame::SlotHeader* frame_slot(int64_t frame);
    uint32_t max_instances() const { return block_ ? block_->max_instances : 0; }
    uint32_t max_skins() const { return block_ ? block_->max_skins : 0; }
    uint32_t max_bones() const { return block_ ? block_->max_bones : 0; }
    void frame_published(int64_t frame);

    // --- the scene ring (any thread) ---
    // Writes one record of `bytes` (a multiple of 8): `fill` gets the contiguous space and writes all of it. Waits
    // for the core to make room for up to `wait_ms`. False when the record is too large, the core is gone, or the
    // ring stayed full.
    bool ring_write(uint64_t bytes, void (*fill)(uint8_t* dst, void* ctx), void* ctx, uint32_t wait_ms = 3000);
    uint64_t ring_max_record() const { return block_ ? block_->ring_bytes / 2 : 0; }
    // The largest record a core's ring takes, also while none runs (a mesh kept for a later core: mirror/geometry.h).
    uint64_t ring_max_record_planned() const;
    // A record was dropped because the core stopped reading: the mirror is incomplete from then on.
    bool ring_stalled() const { return ring_stalled_.load(); }
    struct RingStats {
        uint64_t records = 0, bytes = 0, waits_ms = 0, dropped = 0;
    };
    RingStats ring_stats() const;  // without the ring's lock: a writer may be waiting for space

    // A resource made with D3D11_RESOURCE_MISC_SHARED_NTHANDLE, shared with the core: an NT handle valid in the core's
    // process (the core closes it), as a record carries it; 0 on failure (the HRESULT in *hr). Any thread.
    uint64_t share_with_core(ID3D11Resource* resource, HRESULT* hr = nullptr);

private:
    bool start_locked(ID3D11Device* device);
    bool create_region();
    bool start_process();
    bool start_thread();
    void release_frame_resources();
    static unsigned long __stdcall monitor_main(void* self);
    void rescue_fence_b();

    Options opt_;
    std::mutex start_mutex_;
    bool start_failed_ = false;
    std::atomic<bool> started_{false};
    std::atomic<bool> dead_{false};
    std::atomic<bool> stopping_{false};
    ID3D11Device* device_ = nullptr;  // the game's (not owned)
    rfgrt_ipc::Block* block_ = nullptr;
    uint8_t* region_ = nullptr;
    wchar_t region_name_[96] = {};
    void* mapping_ = nullptr;     // HANDLE: the shared region (process mode)
    void* process_ = nullptr;     // HANDLE: rfgrt_host.exe
    void* job_ = nullptr;         // HANDLE: ends the host with the game
    void* core_thread_ = nullptr; // HANDLE: the in-process core
    void* monitor_ = nullptr;     // HANDLE: the monitor thread
    void* stop_event_ = nullptr;  // HANDLE: wakes the monitor on stop()
    void* scene_event_ = nullptr; // HANDLE: wakes the core when a frame scene is published

    // Fences: A is ours, B the core's (opened once the core is ready).
    ID3D11Fence* fence_a_ = nullptr;
    ID3D11Fence* fence_b_ = nullptr;
    void* fence_a_nt_ = nullptr;  // HANDLEs in this process
    void* fence_b_nt_ = nullptr;
    bool fence_b_failed_ = false;

    // Frame resources of the current generation.
    int32_t generation_ = 0;
    uint32_t width_ = 0, height_ = 0;
    ID3D11Texture2D* depth_ = nullptr;
    ID3D11RenderTargetView* depth_rtv_ = nullptr;
    ID3D11Texture2D* result_ = nullptr;
    ID3D11ShaderResourceView* result_srv_ = nullptr;
    ID3D11Texture2D* game_clb_ = nullptr;
    ID3D11Texture2D* irradiance_ = nullptr;
    ID3D11ShaderResourceView* irradiance_srv_ = nullptr;
    void* irradiance_nt_ = nullptr;
    ID3D11Texture2D* reflection_ = nullptr;
    ID3D11ShaderResourceView* reflection_srv_ = nullptr;
    void* reflection_nt_ = nullptr;
    ID3D11Texture2D* glass_ = nullptr;
    ID3D11ShaderResourceView* glass_srv_ = nullptr;
    void* glass_nt_ = nullptr;
    ID3D11Texture2D* glass_refl_ = nullptr;
    ID3D11ShaderResourceView* glass_refl_srv_ = nullptr;
    void* glass_refl_nt_ = nullptr;
    ID3D11Texture2D* glass_fog_ = nullptr;
    ID3D11ShaderResourceView* glass_fog_srv_ = nullptr;
    void* glass_fog_nt_ = nullptr;
    ID3D11Texture2D* sun_tint_ = nullptr;
    ID3D11ShaderResourceView* sun_tint_srv_ = nullptr;
    void* sun_tint_nt_ = nullptr;
    ID3D11Texture2D* fog_ = nullptr;
    ID3D11ShaderResourceView* fog_srv_ = nullptr;
    void* fog_nt_ = nullptr;
    int light_mask_count_ = 0;
    ID3D11Texture2D* light_mask_[rfgrt_ipc::kMaxLightMasks] = {};
    ID3D11ShaderResourceView* light_mask_srv_[rfgrt_ipc::kMaxLightMasks] = {};
    void* light_mask_nt_[rfgrt_ipc::kMaxLightMasks] = {};
    ID3D11Texture2D* white_ = nullptr;
    ID3D11ShaderResourceView* white_srv_ = nullptr;
    ID3D11Texture2D* radiance_ = nullptr;  // replaced (and the old one released) when the game's size changes
    ID3D11ShaderResourceView* radiance_srv_ = nullptr;
    uint32_t radiance_w_ = 0, radiance_h_ = 0, radiance_format_ = 0;
    bool radiance_failed_ = false;
    void* depth_nt_ = nullptr;
    void* result_nt_ = nullptr;
    void* game_clb_nt_ = nullptr;
    uint64_t depth_remote_ = 0, result_remote_ = 0;  // the handles duplicated into the host (closed with it)

    // The scene ring's writer, serialized.
    std::mutex ring_mutex_;
    rfgrt_scene::RingWriter writer_;
    std::atomic<uint64_t> ring_records_{0}, ring_bytes_{0}, ring_waits_ms_{0}, ring_dropped_{0};
    std::atomic<bool> ring_stalled_{false};
};

// The session's link, shared by the frame seam and the scene mirror.
Link& core_link();

}  // namespace rtlink
