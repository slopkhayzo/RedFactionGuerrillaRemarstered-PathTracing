#include "core/config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwctype>

namespace {

std::wstring env_name(const wchar_t* section, const wchar_t* key) {
    std::wstring n = L"RFGRT_";
    for (const wchar_t* p = section; *p; ++p) n += wchar_t(towupper(*p));
    n += L'_';
    for (const wchar_t* p = key; *p; ++p) n += wchar_t(towupper(*p));
    return n;
}

bool env_value(const wchar_t* section, const wchar_t* key, std::wstring& out) {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(env_name(section, key).c_str(), buf, 1024);
    if (n == 0 || n >= 1024) return false;
    out.assign(buf, n);
    return true;
}

std::wstring get_string(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    std::wstring v;
    if (env_value(section, key, v)) return v;
    wchar_t buf[1024];
    GetPrivateProfileStringW(section, key, def, buf, 1024, ini.c_str());
    return buf;
}

int get_int(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int def) {
    std::wstring v = get_string(ini, section, key, L"");
    return v.empty() ? def : _wtoi(v.c_str());
}

double get_double(const std::wstring& ini, const wchar_t* section, const wchar_t* key, double def) {
    std::wstring v = get_string(ini, section, key, L"");
    return v.empty() ? def : _wtof(v.c_str());
}

std::vector<double> get_list(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    std::vector<double> out;
    std::wstring v = get_string(ini, section, key, L"");
    for (size_t pos = 0; pos < v.size();) {
        size_t end = v.find_first_of(L",; ", pos);
        if (end == std::wstring::npos) end = v.size();
        if (end > pos) out.push_back(_wtof(v.substr(pos, end - pos).c_str()));
        pos = end + 1;
    }
    return out;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

Config load_config(const std::wstring& ini) {
    Config c;
    c.ini_path = ini;
    c.enabled = get_int(ini, L"General", L"Enabled", 1) != 0;
    c.output_dir = get_string(ini, L"General", L"OutputDir", L"");
    c.keep_sessions = get_int(ini, L"General", L"KeepSessions", 10);
    c.clb_pattern = get_int(ini, L"Render", L"ClbPattern", 0);
    c.pattern_from_s = get_double(ini, L"Render", L"PatternFromSeconds", 0);
    c.skip_cascades = get_int(ini, L"Render", L"SkipCascades", 0) != 0;
    c.debug_view = get_int(ini, L"Render", L"DebugView", 0);
    c.host_enabled = get_int(ini, L"Host", L"Enabled", 0) != 0;
    c.host_mode = get_int(ini, L"Host", L"Mode", 1);
    c.host_path = get_string(ini, L"Host", L"Path", L"");
    c.ring_mb = get_int(ini, L"Host", L"RingMB", 32);
    c.host_wait_ms = get_double(ini, L"Host", L"WaitMs", 50.0);
    c.host_dred = get_int(ini, L"Host", L"Dred", 1) != 0;
    c.mirror_enabled = get_int(ini, L"Mirror", L"Enabled", 1) != 0;
    c.max_instances = get_int(ini, L"Mirror", L"MaxInstances", 65536);
    c.skinning = get_int(ini, L"Mirror", L"Skinning", 1) != 0;
    c.glass = get_int(ini, L"Mirror", L"Glass", 1) != 0;
    c.terrain_chunks = get_int(ini, L"Mirror", L"TerrainChunks", 1) != 0;
    c.tlas_fast_trace = get_int(ini, L"Mirror", L"TlasFastTrace", 1) != 0;
    c.shadow_rays = get_int(ini, L"Shadows", L"Rays", 2);
    c.shadow_sun_angle = get_double(ini, L"Shadows", L"SunAngle", 0.5);
    c.shadow_filter_radius = get_int(ini, L"Shadows", L"FilterRadius", 2);
    c.shadow_history = get_int(ini, L"Shadows", L"History", 12);
    c.shadow_specular = get_double(ini, L"Shadows", L"Specular", 0.0);
    c.ao_enabled = get_int(ini, L"AO", L"Enabled", 1) != 0;
    c.ao_rays = get_int(ini, L"AO", L"Rays", 2);
    c.ao_radius = get_double(ini, L"AO", L"Radius", 2.0);
    c.ao_with_gi = get_int(ini, L"AO", L"WithGI", 0) != 0;
    c.geometry_census = get_int(ini, L"Telemetry", L"GeometryCensus", 0) != 0;
    c.dump_shaders = get_int(ini, L"Telemetry", L"DumpShaders", 0) != 0;
    c.gi_enabled = get_int(ini, L"GI", L"Enabled", 0) != 0;
    c.gi_debug = get_int(ini, L"GI", L"Debug", 0);
    c.gi_rays = get_int(ini, L"GI", L"Rays", 2);
    c.gi_length = get_double(ini, L"GI", L"Length", 500.0);
    c.gi_albedo = get_double(ini, L"GI", L"Albedo", 0.35);
    c.gi_textures = get_int(ini, L"GI", L"Textures", 1) != 0;
    c.gi_history = get_int(ini, L"GI", L"History", 24);
    c.gi_filter_radius = get_int(ini, L"GI", L"FilterRadius", 3);
    c.gi_bounce = get_double(ini, L"GI", L"Bounce", 1.0);
    c.gi_cache = get_int(ini, L"GI", L"Cache", 1) != 0;
    c.gi_glass = get_int(ini, L"GI", L"Glass", 1) != 0;
    c.gi_frosted_glass = get_int(ini, L"GI", L"FrostedGlass", 1) != 0;
    c.gi_window_tint = get_int(ini, L"GI", L"WindowTint", 1) != 0;
    c.gi_terrain_albedo = get_list(ini, L"GI", L"TerrainAlbedo");
    c.fog_enabled = get_int(ini, L"Fog", L"Enabled", 1) != 0;
    c.fog_sun_share = get_double(ini, L"Fog", L"SunShare", 0.65);
    c.fog_anisotropy = get_double(ini, L"Fog", L"Anisotropy", 0.25);
    c.gi_denoiser = get_int(ini, L"GI", L"Denoiser", 2);
    c.dlss_mode = get_int(ini, L"DLSS", L"Mode", 1);
    c.fg_enabled = get_int(ini, L"FrameGeneration", L"Enabled", 0) != 0;
    c.fg_presenter = get_int(ini, L"FrameGeneration", L"Presenter", 0) != 0;
    c.gi_sky_drawn = get_double(ini, L"GI", L"SkyDrawn", 1.0);
    c.gi_sky = get_double(ini, L"GI", L"Sky", 1.0);
    c.refl_enabled = get_int(ini, L"Reflections", L"Enabled", 0) != 0;
    c.refl_length = get_double(ini, L"Reflections", L"Length", 500.0);
    c.refl_gloss = get_double(ini, L"Reflections", L"Gloss", 32.0);
    c.refl_glass = get_int(ini, L"Reflections", L"Glass", 1) != 0;
    c.refl_glass_f0 = get_double(ini, L"Reflections", L"GlassReflectance", 0.2);
    c.refl_history = get_int(ini, L"Reflections", L"History", 6);
    c.refl_filter_radius = get_int(ini, L"Reflections", L"FilterRadius", 1);
    c.lights_enabled = get_int(ini, L"Lights", L"Enabled", 0) != 0;
    c.lights_max = get_int(ini, L"Lights", L"Max", 16);
    c.lights_fixture = get_double(ini, L"Lights", L"Fixture", 0.3);
    c.auto_load_save = get_int(ini, L"Test", L"AutoLoadSave", 0) != 0;
    c.block_save_writes = get_int(ini, L"Test", L"BlockSaveWrites", 0) != 0;
    c.god_mode = get_int(ini, L"Test", L"GodMode", 0) != 0;
    c.ignore_focus = get_int(ini, L"Test", L"IgnoreFocus", 0) != 0;
    c.exit_after_main_menu_s = get_int(ini, L"Test", L"ExitAfterMainMenuSeconds", 0);
    c.exit_after_gameplay_s = get_int(ini, L"Test", L"ExitAfterGameplaySeconds", 0);
    c.exit_after_s = get_int(ini, L"Test", L"ExitAfterSeconds", 0);
    c.shots_at = get_list(ini, L"Test", L"ShotsAt");
    c.kill_host_at_s = get_double(ini, L"Test", L"KillHostAtSeconds", 0);
    c.input_script = get_string(ini, L"Test", L"InputScript", L"");
    c.time_of_day = get_int(ini, L"Test", L"TimeOfDay", -1);
    c.test_dlss_cycle_s = get_int(ini, L"Test", L"DlssCycle", 0);
    c.test_fg_cycle_s = get_int(ini, L"Test", L"FrameGenerationCycle", 0);
    c.test_miss_every = get_int(ini, L"Test", L"MissEvery", 0);
    c.test_fog_density = get_double(ini, L"Test", L"FogDensity", 1.0);
    c.probe_key = get_int(ini, L"Test", L"ProbeKey", 0x79);
    c.probe_at_s = get_double(ini, L"Test", L"ProbeAt", 0);
    c.probe_at_frame = get_int(ini, L"Test", L"ProbeAtFrame", -1);
    c.probe_pixel = get_list(ini, L"Test", L"ProbePixel");
    c.probes = narrow(get_string(ini, L"Test", L"Probes", L""));
    c.vanilla_key = get_int(ini, L"Test", L"VanillaKey", 0x7A);
    c.vanilla_at = get_list(ini, L"Test", L"VanillaAt");
    c.glass_refl_at = get_list(ini, L"Test", L"GlassReflectionsAt");
    c.wrecking_crew = narrow(get_string(ini, L"Test", L"WreckingCrew", L""));
    return c;
}

std::string describe(const Config& c) {
    char buf[2048];
    std::string shots;
    for (double s : c.shots_at) {
        char t[32];
        snprintf(t, sizeof(t), "%s%g", shots.empty() ? "" : ",", s);
        shots += t;
    }
    snprintf(buf, sizeof(buf),
             "Enabled=%d KeepSessions=%d | Render: ClbPattern=%d PatternFromSeconds=%g SkipCascades=%d DebugView=%d | Host: Enabled=%d "
             "Mode=%d Path=%s RingMB=%d WaitMs=%g | Mirror: Enabled=%d MaxInstances=%d Skinning=%d Glass=%d TerrainChunks=%d TlasFastTrace=%d | Shadows: Rays=%d SunAngle=%g FilterRadius=%d History=%d Specular=%g | AO: Enabled=%d Rays=%d Radius=%g WithGI=%d | GI: Enabled=%d Debug=%d Rays=%d Length=%g Albedo=%g Textures=%d History=%d FilterRadius=%d Bounce=%g Sky=%g SkyDrawn=%g Cache=%d Glass=%d FrostedGlass=%d WindowTint=%d Denoiser=%d | DLSS: Mode=%d | FrameGeneration: Enabled=%d Presenter=%d | Fog: Enabled=%d SunShare=%g Anisotropy=%g (Test.FogDensity %g) | Reflections: Enabled=%d Length=%g Gloss=%g History=%d FilterRadius=%d Glass=%d GlassReflectance=%g | Lights: Enabled=%d Max=%d Fixture=%g | Telemetry: GeometryCensus=%d | Test: AutoLoadSave=%d BlockSaveWrites=%d GodMode=%d IgnoreFocus=%d "
             "ExitAfterMainMenuSeconds=%d ExitAfterGameplaySeconds=%d ExitAfterSeconds=%d ShotsAt=%s KillHostAtSeconds=%g InputScript=%s TimeOfDay=%d DlssCycle=%d FrameGenerationCycle=%d MissEvery=%d ProbeKey=0x%X ProbeAt=%g VanillaKey=0x%X WreckingCrew=%s",
             c.enabled, c.keep_sessions, c.clb_pattern, c.pattern_from_s, c.skip_cascades, c.debug_view, c.host_enabled, c.host_mode,
             c.host_path.empty() ? "(default)" : narrow(c.host_path).c_str(), c.ring_mb, c.host_wait_ms, c.mirror_enabled, c.max_instances, c.skinning, c.glass, c.terrain_chunks, c.tlas_fast_trace, c.shadow_rays, c.shadow_sun_angle, c.shadow_filter_radius, c.shadow_history, c.shadow_specular, c.ao_enabled, c.ao_rays, c.ao_radius, c.ao_with_gi, c.gi_enabled, c.gi_debug, c.gi_rays, c.gi_length, c.gi_albedo, c.gi_textures, c.gi_history, c.gi_filter_radius, c.gi_bounce, c.gi_sky, c.gi_sky_drawn, c.gi_cache, c.gi_glass, c.gi_frosted_glass, c.gi_window_tint, c.gi_denoiser, c.dlss_mode, int(c.fg_enabled), int(c.fg_presenter), c.fog_enabled, c.fog_sun_share, c.fog_anisotropy, c.test_fog_density, c.refl_enabled, c.refl_length, c.refl_gloss, c.refl_history, c.refl_filter_radius, c.refl_glass, c.refl_glass_f0, c.lights_enabled, c.lights_max, c.lights_fixture, c.geometry_census, c.auto_load_save, c.block_save_writes,
             c.god_mode, c.ignore_focus, c.exit_after_main_menu_s, c.exit_after_gameplay_s, c.exit_after_s, shots.c_str(),
             c.kill_host_at_s, c.input_script.empty() ? "(none)" : narrow(c.input_script).c_str(), c.time_of_day, c.test_dlss_cycle_s, c.test_fg_cycle_s,
             c.test_miss_every, c.probe_key, c.probe_at_s, c.vanilla_key, c.wrecking_crew.empty() ? "(none)" : c.wrecking_crew.c_str());
    return buf;
}
