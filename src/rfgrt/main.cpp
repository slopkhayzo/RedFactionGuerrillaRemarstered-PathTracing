// rfgrt: path tracing for Red Faction Guerrilla Re-Mars-tered, loaded by Ultimate ASI Loader. Today's rfg.exe is
// 32-bit and the ray tracing core runs in rfgrt_host.exe (64-bit); built for x64, the plugin runs the core in-process
// for a 64-bit rfg.exe. See research/docs.
#include <windows.h>

#include <string>
#include <vector>

#include "core/build_check.h"
#include "core/config.h"
#include "core/gameloop.h"
#include "core/log.h"
#include "core/session.h"
#include "game/openrfg64.h"
#include "game/rfg.h"
#include "harness/shader_dump.h"
#include "hooks/binding.h"
#include "hooks/hooks.h"
#include "mirror/geometry.h"
#include "mirror/instances.h"
#include "mirror/textures.h"
#include "render/gi.h"
#include "render/lights.h"
#include "render/seam.h"
#include "rt/link.h"
#include "test/automation.h"

namespace {

HMODULE g_module = nullptr;
// rfgrt_state(): 0 not started, 1 inactive (the log says why), 2 running.
volatile LONG g_state = 0;

std::wstring module_dir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    std::wstring s(path, n);
    return s.substr(0, s.find_last_of(L"\\/"));
}

void init() {
    InterlockedExchange(&g_state, 1);  // until the end
    const std::wstring dir = module_dir();
    Config cfg = load_config(dir + L"\\rfgrt.ini");
    session::init(cfg, dir);
    LOG("rfgrt %s (%s) starting (plugin folder %ls)", RFGRT_VERSION, sizeof(void*) == 8 ? "64-bit" : "32-bit", dir.c_str());
    LOG("config: %s", describe(cfg).c_str());
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    LOG("exe %ls loaded at %p", exe, reinterpret_cast<void*>(rfg::base()));

    std::string detail;
    const int build = identify_build(detail);
    LOG("build check: %s", detail.c_str());
    if (build < 0) {
        LOG("unknown rfg.exe build: plugin stays inactive");
        return;
    }
    if (build == rfg::openrfg64::build_index()) {
        // OpenRFG64-memtest: the values come from the program's own PDB, by name (game/openrfg64.cpp).
        std::string report;
        const int missing = rfg::openrfg64::load(report);
        size_t start = 0;
        for (size_t end = report.find('\n'); end != std::string::npos; start = end + 1, end = report.find('\n', start))
            LOG("%s", report.substr(start, end - start).c_str());
        if (missing > 0) LOG("OpenRFG64: %d names missing: the hooks on them stay off", missing);
    }
    rfg::select_build(build);  // before anything uses a game address or offset
    if (!cfg.enabled) {
        LOG("disabled in config");
        return;
    }
    if (!hooks::init()) return;
    if (!gameloop::install()) {
        LOG("main-loop hook failed: plugin stays inactive");
        return;
    }
    gameloop::add_end(&session::on_frame);
    automation::install(cfg);
    // The ray tracing core: started by the first mesh (scene mirror) or the first frame (seam) that needs it.
    rtlink::Options link;
    link.mode = cfg.host_mode;
    link.host_exe = cfg.host_path.empty() ? dir + L"\\rfgrt\\rfgrt_host.exe" : cfg.host_path;
    link.log_path = session::run_dir() + L"\\rfgrt_host.log";
    link.ring_mb = uint32_t(cfg.ring_mb > 0 ? cfg.ring_mb : 32);
    link.snapshots = cfg.host_enabled && cfg.mirror_enabled;
    link.max_instances = uint32_t(cfg.max_instances > 0 ? cfg.max_instances : 65536);
    link.max_skins = cfg.skinning ? 512 : 0;
    link.max_bones = cfg.skinning ? 16384 : 0;  // (a frame poses a few hundred; each slot holds them: the 32-bit game's address space)
    link.shadow_rays = cfg.shadow_rays;
    link.shadow_sun_angle = float(cfg.shadow_sun_angle);
    link.shadow_filter_radius = cfg.shadow_filter_radius;
    link.shadow_history = cfg.shadow_history;
    // The path-traced irradiance holds the occlusion already: no ambient occlusion rays on top of it (R stays 1), unless
    // AO.WithGI asks for them.
    const bool gi_traced = cfg.gi_enabled && cfg.gi_debug == 0 && cfg.host_mode == rfgrt_ipc::kModeSunShadows;
    link.ao_rays = cfg.ao_enabled && cfg.host_mode == rfgrt_ipc::kModeSunShadows && (!gi_traced || cfg.ao_with_gi)
                       ? (cfg.ao_rays > 0 ? cfg.ao_rays : 1)
                       : 0;
    link.ao_radius = float(cfg.ao_radius > 0.05 ? cfg.ao_radius : 0.05);
    link.debug_view = cfg.debug_view;
    link.gi_rays = gi_traced ? (cfg.gi_rays > 0 ? cfg.gi_rays : 1) : 0;
    link.gi_length = float(cfg.gi_length > 1.0 ? cfg.gi_length : 1.0);
    link.gi_albedo = float(cfg.gi_albedo);
    link.gi_history = cfg.gi_history > 1 ? cfg.gi_history : 1;
    link.gi_filter_radius = cfg.gi_filter_radius >= 0 ? cfg.gi_filter_radius : 0;
    link.gi_bounce = float(cfg.gi_bounce >= 0.0 ? cfg.gi_bounce : 0.0);
    link.gi_sky = float(cfg.gi_sky >= 0.0 ? cfg.gi_sky : 0.0);
    link.gi_cache = cfg.gi_cache;
    link.gi_glass = cfg.gi_glass;
    link.gi_denoiser = cfg.gi_denoiser < 0 ? 0 : cfg.gi_denoiser > 2 ? 2 : cfg.gi_denoiser;
    link.fog = cfg.fog_enabled && cfg.gi_enabled;
    link.fog_sun_share = float(cfg.fog_sun_share < 0.0 ? 0.0 : cfg.fog_sun_share > 1.0 ? 1.0 : cfg.fog_sun_share);
    link.fog_anisotropy = float(cfg.fog_anisotropy < -0.9 ? -0.9 : cfg.fog_anisotropy > 0.9 ? 0.9 : cfg.fog_anisotropy);
    link.fog_density = float(cfg.test_fog_density < 0.0 ? 0.0 : cfg.test_fog_density > 100.0 ? 100.0 : cfg.test_fog_density);
    link.gi_sky_drawn = float(cfg.gi_sky_drawn < 0.0 ? 0.0 : cfg.gi_sky_drawn > 2.0 ? 2.0 : cfg.gi_sky_drawn);
    link.refl_enabled = cfg.refl_enabled && cfg.host_enabled && cfg.host_mode == rfgrt_ipc::kModeSunShadows;
    link.refl_length = float(cfg.refl_length > 1.0 ? cfg.refl_length : 1.0);
    link.refl_gloss = float(cfg.refl_gloss > 1.0 ? cfg.refl_gloss : 1.0);
    link.refl_history = cfg.refl_history > 1 ? cfg.refl_history : 1;
    link.refl_filter_radius = cfg.refl_filter_radius >= 0 ? cfg.refl_filter_radius : 0;
    link.refl_glass = cfg.refl_glass && cfg.gi_glass;
    link.refl_glass_f0 = float(cfg.refl_glass_f0 < 0.0 ? 0.0 : cfg.refl_glass_f0 > 1.0 ? 1.0 : cfg.refl_glass_f0);
    link.frosted_glass = cfg.gi_frosted_glass;
    link.window_tint = cfg.gi_window_tint && cfg.host_mode == rfgrt_ipc::kModeSunShadows;
    link.tlas_fast_trace = cfg.tlas_fast_trace;
    link.dred = cfg.host_dred;
    if (cfg.gi_terrain_albedo.size() >= 3)
        for (int i = 0; i < 3; ++i)
            link.terrain_albedo[i] = float(cfg.gi_terrain_albedo[size_t(i)] < 0.0 ? 0.0 : cfg.gi_terrain_albedo[size_t(i)] > 1.0 ? 1.0
                                                                                                                         : cfg.gi_terrain_albedo[size_t(i)]);
    link.light_masks = cfg.lights_enabled && cfg.host_enabled && cfg.host_mode == rfgrt_ipc::kModeSunShadows
                           ? (cfg.lights_max < 1 ? 1 : cfg.lights_max > int(rfgrt_ipc::kMaxLightMasks) ? int(rfgrt_ipc::kMaxLightMasks) : cfg.lights_max)
                           : 0;
#if defined(RFGRT_LOCAL_CORE)
    link.in_process = true;
#endif
    rtlink::core_link().configure(link);
    // the materials' diffuse maps for the rays' hits: only where rays look at surfaces (GI, reflections), with the mirror
    textures::set_enabled(cfg.gi_textures && link.snapshots && (link.gi_rays > 0 || link.refl_enabled));
    LOG("textures: the materials' diffuse maps %s", textures::enabled() ? "go to the core for the rays' hits (GI.Textures)"
                                                                        : "stay the game's (every hit a plain grey)");
    if (!lights::install(cfg)) LOG("lights hooks failed: no shadows for the dynamic lights");
    if (!gi::install(cfg)) LOG("gi hooks failed: no global illumination");
    if (!seam::install(cfg)) {
        LOG("frame seam hooks failed: plugin stays inactive");
        return;
    }
    session::add_flush_handler(&seam::shutdown);
    if (!geometry::install(cfg)) {
        LOG("geometry hooks failed: plugin stays inactive");
        return;
    }
    session::add_flush_handler(&geometry::report);
    if (!instances::install(cfg)) {
        LOG("instance hooks failed: plugin stays inactive");
        return;
    }
    session::add_flush_handler(&instances::report);
    if (!shader_dump::install(cfg)) LOG("shader dump hooks failed: no shaders saved");
    if (!hooks::enable_all()) return;
    if (hooks::seamed() > 0) LOG("hooks: %d on the game's seams", hooks::seamed());
    session::start_watchdog();
    InterlockedExchange(&g_state, 2);
    LOG("initialization complete");
}

}  // namespace

// Called by Ultimate ASI Loader after LoadLibrary, outside the loader lock, when the game's own code starts.
extern "C" __declspec(dllexport) void InitializeASI() {
    static LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0) init();
}

// For a game that loads the plugin itself (OpenRFG64 VOM's renderer backend), after InitializeASI: 0 not started,
// 1 inactive (the log says why), 2 running.
extern "C" __declspec(dllexport) int rfgrt_state() { return int(g_state); }

// For a game with seams for the plugin's hooks (OpenRFG64 VOM's renderer backend; hooks/binding.h): before
// InitializeASI.
extern "C" __declspec(dllexport) void rfgrt_set_binding(const rfgrt_binding* binding) {
    if (binding) hooks::set_binding(*binding);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        session::on_process_exit();  // process is terminating: flush what is buffered
    }
    return TRUE;
}
