#pragma once
// The link between the plugin (rfgrt.asi, inside rfg.exe) and the ray tracing core (rtcore): one region of plain data
// both sides read and write, plus GPU resources shared by handle. With today's 32-bit rfg.exe the core runs in
// rfgrt_host.exe (64-bit; 32-bit processes get no hardware ray tracing) and the region is named shared memory; with a
// 64-bit rfg.exe the core runs on a thread inside the plugin and the region is ordinary memory. Fixed-size types only,
// the same layout in 32-bit and 64-bit code.
//
// The region: Block at offset 0, the scene ring (scene_ring.h) at Block::ring_offset, then the frame scenes
// (frame_scene.h) at Block::frames_offset.
//
// The core lives as long as the game: it starts at the first mesh the game creates (the mirror needs every one) and
// is not restarted when the scene's size changes; the per-frame textures are remade instead (frame_generation).
//
// Per frame n (n = 1, 2, ...), research/docs/03 "Architecture":
//   plugin: at rl_scene_renderer::render's start, write frame n's scene (instances, camera) and set the scene event
//   core:   build the frame's TLAS; GPU-wait fence A >= n; trace into `result`; Signal(fence B, n); publish
//           host_submitted = n (before the GPU has run it)
//   plugin: at the seam, copy the depth into `depth`; Signal(fence A, n); if host_submitted >= n, GPU-wait fence
//           B >= n and apply `result`; otherwise keep the game's own result.
// Without frame scenes (snapshots = 0) the core queues frames ahead of the plugin instead.
// Fence A is signalled only by the plugin, fence B only by the core: one fence signalled by both sides failed in the
// probes once frames were in flight (research/docs/01 section 8).
#include <intrin.h>

#include <cstdint>

namespace rfgrt_ipc {

constexpr uint32_t kMagic = 0x54524652;  // "RFRT"
constexpr uint32_t kVersion = 28;
constexpr uint32_t kMaxLightMasks = 16;  // shadow masks of dynamic lights a frame

enum HostState : int32_t {
    kHostStarting = 0,
    kHostReady = 1,   // fence B published, the scene ring is being read
    kHostFailed = 2,  // see host_message
    kHostExited = 3,
};

enum Mode : int32_t {
    kModeDepthPattern = 1,  // phase 1: a pattern computed from the game's depth (proves the round trip)
    kModeDepthCompare = 2,  // phase 2: the mirror's depth (rays from the game's camera) against the game's
    kModeSunShadows = 3,    // phase 3: ray-traced sun shadows into ClbTexture's G, compared with the game's cascades
};

struct Block {
    uint32_t magic;
    uint32_t version;
    uint32_t block_bytes;  // sizeof(Block), checked by both sides
    uint32_t plugin_pid;
    uint32_t host_pid;
    uint32_t adapter_luid_low;
    int32_t adapter_luid_high;
    int32_t mode;
    uint64_t region_bytes;  // the whole region: this block and the ring
    uint64_t ring_offset;   // from the start of the region
    uint64_t ring_bytes;    // a power of two

    // Fence A: created and signalled by the plugin; its handle is valid in the core's process before the core starts.
    uint64_t fence_a_handle;
    // Fence B: created by the core, its handle duplicated into the plugin's process (valid there).
    uint64_t fence_b_handle;

    // Frame resources, made by the plugin at the scene's size and remade when that changes. The plugin fills width,
    // height, the handles (valid in the core's process) and generation_first_frame, then raises frame_generation; the
    // core switches when it queues a frame >= generation_first_frame. Frames it had queued with the previous set
    // before that are not applied (host_generation_first_frame tells the plugin which ones).
    volatile int32_t frame_generation;  // 0: none yet
    uint32_t width, height;             // of `depth` and `result`
    uint32_t pad0;
    uint64_t depth_handle;   // Texture2D R32_FLOAT: the game's depth, copied by the plugin each frame
    uint64_t result_handle;  // Texture2D R8G8_UNORM: R = ambient occlusion, G = sun shadow (ClbTexture's layout)
    uint64_t irradiance_handle;  // Texture2D R16G16B16A16_FLOAT: the global illumination (irradiance, the game's units)
                                 // the core writes and the game's patched lit shaders read at t14 (0: no GI)
    uint64_t reflection_handle;  // Texture2D R16G16B16A16_FLOAT: the reflections (what the specular ambient becomes, the
                                 // game's units), the core writes, the patched lit shaders read at t16 (0: none)
    uint64_t game_clb_handle;  // Texture2D R8G8_UNORM: the game's own ClbTexture at the seam (its cascades), copied
    uint64_t glass_handle;  // Texture2D R16G16B16A16_FLOAT: the light of the nearest glass in front of each pixel's surface
                            // (glass_light.hlsl), the core writes, the patched glass shader reads at t17 (0: none)
    uint64_t fog_handle;    // Texture2D R16G16B16A16_FLOAT: the volumetric fog in front of each pixel's surface (fog_apply.hlsl:
                            // rgb what it scatters toward the camera, a what of the surface's light gets through), the core
                            // writes, the plugin blends over the lit scene (0: none)
    uint64_t glass_refl_handle;  // Texture2D R16G16B16A16_FLOAT: what the clear glass in front of each pixel reflects
                                 // (glass_light.hlsl: rgb the radiance its Fresnel term lets show, a that term), the core
                                 // writes, the patched glass shader reads at t19 (0: none)
    uint64_t sun_tint_handle;  // Texture2D R8G8B8A8_UNORM: what of the sun's light the clear glass between each pixel's surface
                               // and the sun lets through (sun_tint.hlsl: rgb, a 1), the core writes, the patched lit shaders
                               // read at t20 (0: none)
    uint64_t glass_fog_handle;  // Texture2D R16G16B16A16_FLOAT: the volumetric fog between the camera and the nearest glass in
                                // front of each pixel (fog_apply.hlsl: rgb what it scatters toward the camera, a what of the
                                // glass's light gets through; 0 where no glass), the core writes with the fog, the patched
                                // glass shader reads at t21 (0: none)
    // Texture2D R8G8_UNORM each: the shadow masks of the dynamic lights a frame scene lists (frame_scene.h Light::mask),
    // G the light's visibility, R 1; the plugin binds each at t15 for its light's pass (0: no such mask).
    uint64_t light_mask_handles[kMaxLightMasks];
    // The game's lit colour (the opaque scene after its lighting passes), copied by the plugin each frame into a shared
    // texture of the game's own format, for the GI rays' hits: made at the first copy and remade when that target
    // changes (radiance_generation raised after radiance_handle, a handle valid in the core's process, closed by it);
    // radiance_frame: the frame whose colour it holds.
    uint64_t radiance_handle;
    volatile int32_t radiance_generation;
    int32_t gi_denoiser;  // GI.Denoiser: 0 one filter pass, 1 the a-trous wavelet (gi_atrous.hlsl), 2 DLSS Ray Reconstruction
                          // (dlss.h; where unavailable, the wavelet)
    volatile int64_t radiance_frame;
    volatile int64_t generation_first_frame;

    // Sun shadows (kModeSunShadows): rays per pixel spread over the sun's disk, its angular radius, the filter's radius
    int32_t shadow_rays;
    float shadow_sun_angle_deg;
    int32_t shadow_filter_radius;
    int32_t shadow_history;  // frames the temporal accumulation averages at most (1: none)
    int32_t ao_rays;         // ambient occlusion rays a pixel and frame (0: no ambient occlusion)
    float ao_radius;         // their length, metres
    int32_t debug_view;      // Render.DebugView: 1 the shadow comparison in the result's R (not applied), 2 the occlusion
    float gi_sky_drawn;      // GI.SkyDrawn: rays that get out see the sky the game draws (1), its ambient colour (0), between
    // Global illumination (GI.*): rays a pixel and frame (0: none), their length (m), the albedo of what they hit, the
    // most frames averaged, the filter's radius.
    int32_t gi_rays;
    float gi_length;
    float gi_albedo;
    int32_t gi_history;
    int32_t gi_filter_radius;
    float gi_bounce;  // the bounced light's scale (1: as traced)
    // Shadows of the dynamic lights (Lights.*): masks the plugin made (0: none).
    int32_t light_masks;
    float gi_sky;  // the sky's light scale (GI.Sky; 0: none, the sun's, the lights' and the glows' alone)
    // GI.SunBounceAlbedo: the least albedo a sunlit surface bounces the sun with (the game's textures are authored dark
    // under a very bright sun); 0: as the game draws it
    float gi_sun_bounce_albedo;
    // GI.ShadeBounceAlbedo: the least albedo a surface in shade passes on its light with (the second and later bounces);
    // 0: as the game draws it
    float gi_shade_bounce_albedo;
    // Reflections (Reflections.*): on, the rays' length (m), the lobe's exponent, the most frames averaged, the filter.
    int32_t refl_enabled;
    float refl_length;
    float refl_gloss;
    int32_t refl_history;
    int32_t refl_filter_radius;
    int32_t gi_cache;  // GI.Cache: the world radiance cache for what rays hit off screen (0: none)
    // The volumetric fog (Fog.*): on, the share of the fog's colour the sun gives (the rest the sky), the scattering's
    // anisotropy (Henyey-Greenstein g).
    int32_t fog_enabled;
    float fog_sun_share;
    float fog_anisotropy;
    float fog_density;  // Test.FogDensity: the volumetric fog's density over the game's fog's (1)
    float fog_near;       // Fog.Near: the share kept of the fog the game leaves out within its start (0 to 1)
    int32_t fog_history;  // Fog.History: the most frames each froxel's light is averaged over
    int32_t fog_rays;     // Fog.Rays: rays from each froxel a frame (1 to 4)
    int32_t fog_filter;   // Fog.Filter: 1 the froxels' light smoothed across the screen before it is summed
    int32_t host_fog;  // the core: 1 when it writes the fog texture of the frame resources it queues with (host_generation)
    // The clear glass's reflections (Reflections.Glass): on, its reflectance at normal incidence (Reflections.GlassReflectance)
    int32_t refl_glass;
    float refl_glass_f0;
    float probe_x, probe_y;  // where the probe aims (Test.ProbePixel): fractions of the screen; 0.5, 0.5 the crosshair
    // GI.FrostedGlass: the translucent glass (rfg-eosglass) diffuses the light through it: no direct light through it, its
    // panes glowing with the light of their far side instead (rt_query.hlsli, trace_common.hlsli pane_radiance)
    int32_t frosted_glass;
    int32_t tlas_fast_trace;  // Mirror.TlasFastTrace: the frame's TLAS built PREFER_FAST_TRACE (1) or PREFER_FAST_BUILD (0)
    // GI.TerrainAlbedo: the albedo the rays give a surface whose material is not known: the terrain (its own blending
    // shader, no material map), where neither the game's colour nor the radiance cache has it
    float terrain_albedo[3];
    int32_t window_tint;  // GI.WindowTint: the clear glass tints the sun's light through it (sun_tint.hlsl)
    // Host.Dred (OpenRFG64-memtest): the core's device keeps Direct3D 12's removed-device data (the command lists'
    // progress, the page fault's address), logged if the device is lost
    int32_t dred;
    // Test.KillHostAtSeconds with the core in the game's process (OpenRFG64-memtest): 1 asks the core to remove its
    // device (ID3D12Device5::RemoveDevice) as a lost device would be; the core sets 2 once it has
    volatile int32_t test_remove_device;
    // DLSS (DLSS.Mode; OpenRFG64 VOM's DLSS row, also while playing), with GI.Denoiser=2: 0 off (the wavelet filter),
    // 1 DLAA (the GI's rays at the frame's size), 2 quality, 3 balanced, 4 performance, 5 ultra performance (the rays at
    // NVIDIA's render size for the mode, jittered, upscaled by Ray Reconstruction). The core switches at its next frame,
    // once the frames it has queued are done.
    volatile int32_t dlss_mode;
    volatile int32_t host_dlss_mode;  // the core: the mode its frames use (-1 before its first)

    volatile int32_t host_state;  // HostState
    volatile int32_t stop;        // set by the plugin: the core stops submitting and exits
    volatile int64_t plugin_frame;    // last frame the plugin signalled on fence A
    volatile int64_t host_submitted;  // last frame the core queued (fence B reaches it when the GPU is done)
    volatile int64_t host_gpu_us;     // GPU time of the core's last finished frame, microseconds
    volatile int32_t host_generation;              // the frame resources the core queues frames with
    volatile int32_t probe_request;  // raised by the plugin for each probe (Test.ProbeKey): the core logs the crosshair's rays
    volatile int64_t host_generation_first_frame;  // the first frame it queued with them

    // The scene ring: bytes written by the plugin and bytes consumed by the core, both counted from the start (the
    // offset in the ring is the count modulo ring_bytes). The plugin raises ring_write once a record is complete; the
    // core raises ring_read once it has copied a record out.
    volatile int64_t ring_write;
    volatile int64_t ring_read;

    // Frame scenes (frame_scene.h): rfgrt_frame::kSlots slots of frame_slot_bytes at frames_offset, frame n in slot
    // n % kSlots. With snapshots = 1 the core queues a frame once its scene is published, and waits on the scene
    // event (auto-reset, set by the plugin after each one; a handle valid in the core's process, closed by it).
    uint64_t frames_offset;
    uint64_t frame_slot_bytes;
    uint32_t max_instances;
    uint32_t snapshots;
    uint32_t max_skins;  // Skin records a frame scene holds (0: no palettes, skinned instances in their bind pose)
    uint32_t max_bones;  // Bone matrices a frame scene holds
    volatile int64_t frame_published;  // the last frame whose scene is complete
    uint64_t scene_event_handle;

    // The core's counters, for the plugin's log.
    volatile int64_t tlas_instances;  // in the last frame's TLAS
    volatile int64_t tlas_missing;    // in the last frame's scene, without a mesh or a BLAS in the core
    volatile int64_t tlas_build_us;   // GPU time of the last frame's work (TLAS and pass)
    volatile int64_t scene_meshes;       // live meshes
    volatile int64_t scene_blas;         // live bottom-level acceleration structures
    volatile int64_t scene_triangles;    // in them
    volatile int64_t scene_gpu_bytes;    // GPU memory of the mirror: vertices, indices and acceleration structures
    volatile int64_t scene_build_us;     // GPU time of the BLAS builds so far, microseconds

    wchar_t log_path[260];  // the core's log file
    char host_message[256];
};

static_assert(sizeof(Block) % 8 == 0, "fixed layout");

// The 64-bit counters are read and written whole, from 32-bit code too, where a plain access is two 32-bit moves
// that can tear. Both are full barriers: what was written before a store64 is visible to whoever load64s the value.
inline int64_t load64(const volatile int64_t* p) {
    return _InterlockedCompareExchange64(const_cast<volatile int64_t*>(p), 0, 0);
}
inline void store64(volatile int64_t* p, int64_t v) {
    int64_t old = *p;
    for (;;) {
        const int64_t seen = _InterlockedCompareExchange64(p, v, old);
        if (seen == old) return;
        old = seen;
    }
}

}  // namespace rfgrt_ipc
