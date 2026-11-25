#pragma once
#include <string>
#include <vector>

// Settings from scripts\rfgrt.ini; an environment variable RFGRT_<SECTION>_<KEY> overrides any of them (the test
// scripts configure runs that way).
struct Config {
    std::wstring ini_path;

    // [General]
    bool enabled = true;
    std::wstring output_dir;  // session folders; empty: <plugin folder>\rfgrt_runs
    int keep_sessions = 10;   // older session folders are deleted at start (0 keeps all)

    // [Render]
    int clb_pattern = 0;          // test pattern into ClbTexture: 1 the sun-shadow channel (G), 2 AO (R), 3 both
    double pattern_from_s = 0;    // gameplay seconds before the pattern (or the host's result) is applied
    bool skip_cascades = false;   // the shadow cascades get no casters (needs something to fill ClbTexture; cloud shadows stay)
    int debug_view = 0;           // the core's result over the presented frame: 1 the comparison with the game's (depth or
                                  // shadows), 2 the ambient occlusion (shadowed parts red); 3 the game's lit colour the
                                  // rays read for their hits (the capture at the end of its lighting passes); 4 the path-traced irradiance (GI) the
                                  // patched shaders read, the light alone

    // [Host]
    bool host_enabled = false;    // run rfgrt_host.exe (64-bit) and apply its result to ClbTexture
    int host_mode = 1;            // what the core computes: 1 a pattern from the game's depth, 2 the depth compare, 3 sun shadows
    std::wstring host_path;       // empty: <plugin folder>\rfgrt\rfgrt_host.exe
    int ring_mb = 32;             // the scene ring between the plugin and the core (rounded up to a power of two)
    double host_wait_ms = 50.0;   // the most a frame waits for the core to queue it (a miss: the core's last result)
    bool host_dred = true;        // the core's device keeps Direct3D 12's removed-device data, logged if it is lost

    // [Mirror]
    bool mirror_enabled = true;   // with the core: every mesh the game creates goes to the core (acceleration structures)
    int max_instances = 32768;    // render instances per frame scene
    bool skinning = true;         // skinned instances' bone palettes go to the core, which poses them (else: bind pose)
    bool glass = true;            // rays go through glass and other transparent materials (0: everything stops them)
    bool terrain_chunks = true;   // terrain chunk by chunk, only the chunks the game draws (0: each subzone's height mesh whole)
    bool tlas_fast_trace = true;  // the frame's TLAS built for fast tracing (0: for a fast build)

    // [Shadows] (Host.Mode=3)
    int shadow_rays = 2;          // rays per pixel and frame toward the sun, spread over its disk
    double shadow_sun_angle = 0.5;  // the sun's angular radius in degrees: the softness of penumbrae
    int shadow_filter_radius = 2;   // the spatial filter's radius in pixels (2: 5x5)
    int shadow_history = 12;        // frames the temporal accumulation averages at most (1: none)
    double shadow_specular = 0.0;   // the sun's specular a surface in full shadow keeps where the core traced it (the game's
                                    // shaders keep 0.05: its highlights then shine in the dark of path-traced interiors;
                                    // patch/shadow_floor.h)

    // [AO] (Host.Mode=3): ray-traced ambient occlusion into ClbTexture's R, in place of the game's SSAO
    bool ao_enabled = true;
    int ao_rays = 2;                // rays per pixel and frame over the hemisphere
    double ao_radius = 2.0;         // their length in metres: how far away a surface still occludes
    bool ao_with_gi = false;        // also with GI.Enabled=1 (whose irradiance already holds the occlusion: R is 1 without)

    // [Telemetry]
    bool geometry_census = false;  // log every mesh the renderer creates, by creator and vertex layout
    bool dump_shaders = false;     // save the bytecode of the shaders KAIKO's layer loads by name, into the run folder

    // [GI] global illumination through the game's shading (render/gi.h)
    bool gi_enabled = false;       // lit base-pass shaders patched to read their ambient from the irradiance texture
    int gi_debug = 0;              // 1: that texture magenta (coverage), 2: nothing traced, the game's own ambient (identity check),
                                   // 3: no ambient at all (black, alpha 1): only the direct light, glows and fog stay
    int gi_rays = 2;               // path-traced rays per pixel and frame (with the core, Host.Mode=3)
    double gi_length = 60.0;       // their length in metres
    double gi_albedo = 0.35;       // the albedo of the surfaces they hit where their material is not known (no texture)
    bool gi_textures = true;       // the materials' diffuse maps for the rays' hits (reflections, GI off screen): copied to
                                   // the core at 256 texels (0: every hit the plain grey of GI.Albedo)
    int gi_history = 24;           // the most frames averaged
    int gi_filter_radius = 3;      // the spatial filter's radius in pixels
    double gi_bounce = 1.0;        // the bounced light's scale: taste, 1 is as traced
    // The least albedo a sunlit surface bounces the sun with (GI.SunBounceAlbedo): the game's textures are authored dark
    // under a very bright sun (its terrain sends on about 0.03 of the sun it gets, real ground 0.15 to 0.3), so the light
    // sunlit surfaces bounce is scaled as if no material were darker than this (at most 32 times); 0: as the game draws
    // them. Only the sunlit surfaces' light: the shaded ones' stays as traced.
    double gi_sun_bounce_albedo = 0.2;
    double gi_sky_drawn = 1.0;    // the sky rays that get out see: 0 the game's ambient colour, 1 the drawn sky's colour at
                                  // the ambient's brightness, 2 the drawn sky as it is (about 4x darker); between: blends
    int gi_denoiser = 2;          // the GI's denoiser: 0 one 7x7 pass, 1 an edge-avoiding wavelet over ~61 x 61 pixels, 2 DLSS
                                  // Ray Reconstruction (NVIDIA RTX; elsewhere, and in builds without the SDK, the wavelet)
    int dlss_mode = 1;            // DLSS.Mode, with Denoiser=2: 0 off (the wavelet), 1 DLAA, 2 quality, 3 balanced,
                                  // 4 performance, 5 ultra performance (the GI traced at a share of the frame, upscaled)
    bool fg_enabled = false;      // FrameGeneration.Enabled: DLSS Frame Generation (64-bit; present/presenter.h)
    bool fg_presenter = false;    // FrameGeneration.Presenter: the presenter without generation (a test of its presents)
    bool gi_cache = true;         // the world radiance cache: the light of surfaces seen on screen, for rays hitting them later
    bool gi_glass = true;         // the game's plain glass (windows, shards) lit by the path tracer: its shadow, the light
                                  // around it (patch/glass.h; 0: the glass's own light, no shadow)
    std::vector<double> gi_terrain_albedo;  // GI.TerrainAlbedo: r, g, b the rays give the terrain where nothing better is
                                            // known (default 0.032, 0.010, 0.002: the soil as the game draws it)
    bool gi_window_tint = true;   // the clear glass tints the sun's light through it, texel by texel (GI.WindowTint)
    bool gi_frosted_glass = true; // the patterned glass (rfg-eosglass) diffuses the light through it: no sunlight through
                                  // it, its panes glowing on their other side with it (0: a share of the sun through each)
    double gi_sky = 1.0;           // the sky's light (the game's ambient colour as the sky's radiance): 0 none, 1 the game's

    // [Fog] the game's fog made volumetric (with GI): its density, height falloff, start and colour, lit where the sun
    // and the sky reach it (light shafts, shadowed fog, darker indoors), in place of the game's over the lit opaque scene
    // (patch/fog.h); the transparents keep the game's
    bool fog_enabled = true;
    double fog_sun_share = 0.65;   // the share of the fog's colour the sun gives (the rest the sky): fog where both reach
                                   // is the game's colour
    double fog_anisotropy = 0.25;  // how much more of the sun's share the fog scatters toward the sun than away from it
                                   // (Henyey-Greenstein g: 0 even; the shafts stand out more looking toward the sun)

    // [Reflections] path-traced reflections in place of the lit shaders' specular ambient (with the core, Host.Mode=3)
    bool refl_enabled = false;
    double refl_length = 100.0;    // the rays' length in metres
    double refl_gloss = 32.0;      // the lobe's exponent around the mirror direction (higher: sharper)
    bool refl_glass = true;        // the clear glass reflects (a mirror, Fresnel-weighted; with GI.Glass)
    double refl_glass_f0 = 0.2;    // its reflectance at normal incidence (Schlick's F0: plain glass 0.04, a mirror 1)
    int refl_history = 6;          // the most frames averaged
    int refl_filter_radius = 1;    // the spatial filter's radius in pixels

    // [Lights] shadows for the dynamic lights (render/lights.h), with the core (Host.Mode=3)
    bool lights_enabled = false;   // the visible point and spot lights get ray-traced shadow masks, at t15 for their passes
    int lights_max = 16;           // masks: the nearest lights get one, the others go unshadowed (no longer by the sun's)
    double lights_fixture = 0.3;   // metres around a light within which nothing shadows it (its lamp)

    // [Test]
    bool auto_load_save = false;  // main menu -> Load Game -> most recent save, without input
    bool block_save_writes = false;
    bool god_mode = false;
    bool ignore_focus = false;    // the game keeps running when its window is not in front (no quick pause)
    int exit_after_main_menu_s = 0;
    int exit_after_gameplay_s = 0;
    int exit_after_s = 0;         // watchdog: the session ends after this long whatever happens
    std::vector<double> shots_at; // screenshots at these gameplay seconds (shot_<s>.png in the session folder)
    double kill_host_at_s = 0;    // ends the host at this gameplay second (tests the fallback)
    int test_miss_every = 0;      // every Nth frame's core result left unused, as if missed (tests the misses)
    double test_fog_density = 1.0;  // Test.FogDensity: the volumetric fog's density over the game's fog's (tests: the
                                    // fog's structure seen in thick fog; the sky, the landmarks and see-through things
                                    // keep the game's)
    int probe_key = 0x79;         // Test.ProbeKey: the key (virtual-key code; F10) that logs what the rays under the
                                  // crosshair meet: what shadows that spot, what the rays see there (0: none)
    double probe_at_s = 0;        // Test.ProbeAt: one probe at this gameplay second (tests)
    int probe_at_frame = -1;      // Test.ProbeAtFrame: one probe at this gameplay frame (a replay's frame number)
    std::vector<double> probe_pixel;  // Test.ProbePixel: where the probe aims, fractions of the screen (x, y; default
                                      // its centre, the crosshair)
    std::string probes;           // Test.Probes: several probes, `frame@x,y/frame@x,y/...` (replay frames, fractions of the screen)
    std::vector<double> vanilla_at;  // Test.VanillaAt: Test.VanillaKey pressed at these gameplay seconds (tests)
    std::vector<double> glass_refl_at;  // Test.GlassReflectionsAt: the clear glass's reflections hidden, shown again, ...
                                        // at these gameplay seconds (A/B shots of one view)
    int vanilla_key = 0x7A;       // Test.VanillaKey: the key (F11) that switches between the path tracing and the game's
                                  // own lighting and shadows, for comparing (the core keeps running; 0: none)
    std::wstring input_script;    // scripted mouse and keyboard input (test/input_script.h)
    int time_of_day = -1;         // the game clock set to this hour once gameplay starts, and held there (-1: the save's)
    int test_dlss_cycle_s = 0;    // Test.DlssCycle: every this many gameplay seconds the next DLSS mode (1 to 5, then 0), as
                                  // a game's DLSS setting changes it while playing (0: never)
    int test_fg_cycle_s = 0;      // Test.FrameGenerationCycle: every this many gameplay seconds frame generation switched
                                  // (on, off, ...), as a game's setting does while playing (0: never)
    std::string wrecking_crew;    // Test.WreckingCrew: main menu -> Wrecking Crew -> this map (its file name, as
                                  // wcdlc4, or its index; "current": the menu's) -> the match, instead of a save
};

Config load_config(const std::wstring& ini_path);
std::string describe(const Config& c);
