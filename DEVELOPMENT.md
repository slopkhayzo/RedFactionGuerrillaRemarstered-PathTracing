# Development

How `rfgrt` is built, tested and laid out on the author's machine. The plan and the reasoning behind it are in
[research/docs](research/docs/README.md): the path-tracing target in [04](research/docs/04-path-tracing.md), the
architecture and phases in [03](research/docs/03-implementation-plan.md).

## One plugin, two architectures

Today's `rfg.exe` (GOG and Steam) is 32-bit, and the driver gives 32-bit processes no hardware ray tracing
([01 §7](research/docs/01-findings.md)). So the plugin is split:

| Piece | Where it runs | What it does |
| --- | --- | --- |
| `rfgrt.asi` (Win32) | inside `rfg.exe` | hooks the game (PDB symbols), mirrors the scene, copies the depth, synchronizes with the core, puts the results into the frame |
| `rtcore` (x64 library) | in `rfgrt_host.exe` | the D3D12 ray tracing core: opens the shared textures and fences, does the GPU work each frame |
| `rfgrt_host.exe` (x64) | its own process, started by the plugin | runs `rtcore`; linked to the plugin by named shared memory ([src/shared/rfgrt_ipc.h](src/shared/rfgrt_ipc.h)) |
| `rfgrt.asi` (x64) | inside a 64-bit `rfg.exe` (the 64-bit port) | the same plugin source with `RFGRT_LOCAL_CORE`: `rtcore` on a thread of the game's own process, no host |

The protocol is the same either way ([src/shared/rfgrt_ipc.h](src/shared/rfgrt_ipc.h)): a shared region (the control
block, the scene ring, the frame scenes), one fence per direction (A: the plugin's "depth ready", B: the core's "result
ready"), per-frame textures by NT handle. The core starts at the first mesh the game creates and lives as long as the
game: it holds the mirror, which cannot be sent again, so a resolution change remakes the frame textures instead of
restarting it. If it dies, the plugin releases any GPU wait on fence B from the CPU and the game keeps its own shadows.

Nothing in the plugin assumes 32 bits:

- **Addresses and struct layouts are per build and by name.** `tools/gen_symbols.py` turns `tools/symbols.txt` into
  `src/rfgrt/game/rfg_symbols.generated.h`: function and global addresses (`rfg::rva`) and struct member offsets and
  sizes (`rfg::off`) for every known build. Code reads `rfg::field<T>(object, rfg::off::Type_member)`, never a constant
  offset. A 64-bit build of the game (the 64-bit port generates its C++ from the same PDB, so names stay) adds a column
  read from its own PDB; the build check only accepts builds of the plugin's own architecture (PE machine type).
- **`__thiscall` hooks** use `RFG_THIS` ([src/rfgrt/game/rfg.h](src/rfgrt/game/rfg.h)): the x86 `__fastcall` + EDX
  idiom, a plain parameter on x64.
- **PE headers** are read natively (`IMAGE_NT_HEADERS`), pointers as `uintptr_t`.
- `tools\build.ps1` builds the x64 plugin every time, so it keeps compiling.

## Build

Requirements as for the PhysX project: Visual Studio 2026 with the C++ workload (its CMake), the Windows SDK (`fxc` and
`dxc` compile the shaders at build time), Python 3 for the symbol generator.

```powershell
$q = 'D:\Githubs\_agents\queue_executor\queue.ps1'
& $q submit -Name 'rfgrt build' -Owner 'RedFactionGuerrillaRemarstered-RTHWAccel' -File tools\build.ps1 -Wait
#   build-x86\RelWithDebInfo\rfgrt.asi, build-x64\RelWithDebInfo\rfgrt_host.exe and rfgrt.asi (x64)
python tools\gen_symbols.py   # after editing tools\symbols.txt
```

The generator reads `rfg.pdb` through the PhysX project's analysis scripts (`..\RedFactionGuerrillaRemarstered-PhysX
\research\scripts`, or `RFGRE_SCRIPTS`); `tools/steam_map.json` (from the PhysX project's `match_builds.py`) gives
the Steam build's addresses.

The shader patches (`src/rfgrt/patch`: the GI's ambient, the light passes' shadow) are checked offline on the game's
own shaders before any game run. `research\scripts\shaders.py extract` takes them out of `misc.vpp_pc` into
`research\out\shaders` (the game's data: never in the repository); then `build-x64\RelWithDebInfo\shaderpatch.exe
research\out\shaders` (a queue job after the build) decodes all 1873, patches every pixel shader, creates each patched
one on WARP and runs it against its original, one pixel with made-up inputs, in settings where the two must agree
(the ambient patch: E against the ambient it replaces, the bounce inside it, the specular ambient untouched; the
light-pass patch: the colour times `t15.y`). The report, `research\out\shaders\patch_report.txt`, lists every
disagreement; the patched disassembly is written next to each shader (`.patched.asm`, `.lightpass.asm`).

## Test runs: a private copy of the game

Other agents test their patches in the installed game folder at the same time (the 64-bit port deploys `rfgphys`
there; Sledge is installed). This project never touches it:

- `tools\sandbox.ps1` makes `game\gog\`: `rfg.exe` and the game's own DLLs copied, every file of `data\` hard-linked
  (no extra space; the game only reads them), no mod of any kind. Deleting `game\` removes only the links.
- `tools\run_game.ps1` (an **atomic** queue job) installs Ultimate ASI Loader (`dinput8.dll`), `scripts\rfgrt.asi`
  and `scripts\rfgrt\rfgrt_host.exe` into the copy, runs the game unattended, collects the run folder, and removes
  everything it installed, whatever happened. It refuses to run binaries older than the sources.
- Save writes are blocked (the plugin returns success from the save writer without writing), the real save folders
  are hashed before and after (exit code 5 if anything changed) and backed up into `runs\save_backups`.
- The game keeps running without the foreground (`Test.IgnoreFocus`): started from a hidden job it would quick-pause.

```powershell
& $q submit -Atomic -Name 'rfgrt: ...' -Owner 'RedFactionGuerrillaRemarstered-RTHWAccel' -TimeoutMinutes 8 `
    -File tools\run_game.ps1 -Arguments '-Name','smoke','-AutoLoadSave','-ExitAfterGameplaySeconds','30',`
    '-Set','Render.ClbPattern=1;Render.PatternFromSeconds=15;Test.ShotsAt=10,20'
```

Each run gets `runs\<date>-<name>\`: `rfgrt.log`, `rfgrt_host.log`, screenshots (`shot_<s>.png`), the game's own
logs, crash dumps. Several runs that belong together go into one atomic job file (`tools\jobs\*.json`).

The Steam copy is `game\steam`: pass `-GameDir '<the Steam install>' -Sandbox game\steam` to `run_game.ps1` (it
refreshes its copy from `-GameDir` before every run, GOG's by default). `-WithPhysX` takes rfgphys from `-GameDir`'s
`scripts` folder; `-PhysXFrom <folder>` from any folder laid out the same way, such as the PhysX project's package
`dist\rfgphys-<version>\scripts` (`-Set General.Enabled=0` leaves rfgrt installed but inert, for rfgphys alone).

On Steam the game loads Steam's own libraries (steamclient, the overlay, which also injects itself into
`rfgrt_host.exe`, and Steam tracks that process as part of the game): about 40 MB less address space than on GOG (at
the end of the campaign save's load: 661 MB free on GOG, 623 MB on Steam with rfgrt; 194 MB with rfgphys 0.5.0 as
well). Live 85-89 (2026-11-19): rfgrt 0.1.18 on Steam gives what it does on GOG (the same shader patches, glass,
materials, TLAS, timings); with the Steam folder's rfgphys 0.5.0-pre (Havok heap 384 MB) the game died in the main
menu, 1-2 s after rfgphys's PhysX host came up, in Steam's own library: a fatal error with no exception record (Steam's
crash handler writes `Steam\dumps\crash_rfg.exe_*.dmp` and starts `steamerrorreporter.exe`, the game exits with code
1), after which the Steam client itself failed a few minutes later (`fatal stalled cross-thread pipe`) and the next
launches showed "Steamworks: Initialization failed". rfgphys 0.5.0 (Havok heap 256 MB) alone and with rfgrt ran
cleanly on Steam (campaign and Wrecking Crew). Unattended runs' abrupt exit (`session::exit_now`) leaves the core's
queue waiting on a frame that never comes: the driver resets the engine after 2 s (System log `nvlddmkm` 153, WER
`LiveKernelEvent` 141); the user's sessions, quit or crashed, never show it.

**Recorded sessions:** the PhysX project's harness (rfgphys, as installed in the game folder: never its repository)
records a session's inputs and replays them frame for frame with the camera locked to the recording. `play_session.ps1
-Record` (the profile's *Record the session for the agent*) records into `recordings\<time>-<mode>-<map>\` (F11 marks a
frame to look at, F10 ends; the path tracer's on/off key moves to F9 meanwhile); `run_game.ps1 -Replay <folder, its
name, or latest>` replays one (the harness quits the game at its end: exit code 2, no "session end" in the log),
`-ShotFrames 600,1500` taking screenshots at those frames (the recording's marks by default). Frames are counted as
`session::gameplay_frames()` counts them, so `Test.ProbeAtFrame` lands on the same frame. A/B comparisons are two runs of
one replay in one atomic job (`tools\jobs\pt49_textures.json`).

**Playing it yourself:** the queue app's profile *Red Faction: Guerrilla Re-Mars-tered: path tracing / Play with the
path tracer* runs `tools\play_session.ps1`: the newest package in `dist\` (or the development build) in the same
private copy, with the release settings, optionally rfgphys as installed in the game folder, the game started for you
and everything installed removed when you quit. Saves are written normally (backed up first) unless blocked; the logs
go to `runs\<time>-play-<name>\`.

## Settings

`scripts\rfgrt.ini` in the game folder; an environment variable `RFGRT_<SECTION>_<KEY>` overrides any of them (the
test scripts configure runs that way; `-Set 'Section.Key=Value;...'`).

| Setting | Default | Meaning |
| --- | --- | --- |
| `General.Enabled` | 1 | 0: the plugin loads but does nothing |
| `General.OutputDir`, `KeepSessions` | `scripts\rfgrt_runs`, 10 | session folders (log, screenshots) |
| `Render.ClbPattern` | 0 | test pattern into `ClbTexture`: 1 the sun-shadow channel, 2 AO, 3 both |
| `Render.PatternFromSeconds` | 0 | gameplay seconds before the pattern or the core's result is applied (A/B in one run) |
| `Render.SkipCascades` | 0 | the shadow cascades draw no casters while the core fills the shadow channel; the game's cloud shadows (projected in the same passes) stay, and the core's shadow is multiplied into them |
| `Render.DebugView` | 0 | the core's result over the presented frame. 1: the comparison with the game's: with `Host.Mode=2` the game's depth as bands on the left half and the mirror's on the right (gray where they agree, red where not, blue where the mirror has nothing), with `Host.Mode=3` the game's shadow on the left half and ours on the right; 2: the ray-traced ambient occlusion (shadowed parts red); 3: the game's lit colour that the rays read for what they hit (the capture at the end of its lighting passes, `x / (1 + x)`); 4: the path-traced irradiance the patched shaders read (the light alone, after the denoiser), the same curve |
| `Host.Enabled` | 0 | run the ray tracing core and apply its result |
| `Host.Mode` | 1 | what the core computes: 1 depth bands (the phase 1 round trip), 2 depth compare (the mirror's depth against the game's), 3 ray-traced soft sun shadows and ambient occlusion into `ClbTexture` (the shadows compared with the game's cascades in the log) |
| `GI.Enabled`, `Debug` | 0, 0 | global illumination through the game's shading: the lit base-pass pixel shaders are patched as they are created (`src/rfgrt/patch`) to read their ambient colour from `t14`, bound next to `ClbTexture`; with the core (`Host.Mode=3`) it holds the path-traced irradiance. Where its alpha is 0 (the sky, the landmark layer, and every pixel while the core does not answer) the patched shaders keep the game's own ambient. `Debug` 1 fills it magenta (which surfaces read it), 2 with alpha 0 (the patched shaders must then draw the game's own image: an identity check) |
| `GI.Rays`, `Length`, `Albedo`, `History`, `FilterRadius` | 2, 500, 0.35, 24, 3 | rays a pixel and frame, their length in metres, the albedo of what they hit where the game did not draw it in the previous frame (elsewhere its own lit colour) and its material is not known (`GI.Textures`), the most frames averaged, the spatial filter's radius |
| `GI.Textures` | 1 | with GI or reflections: the materials for the rays' hits (world-space shading, `trace_common.hlsli` `hit_surface`). Each mesh's material map entries (from the first instance of it seen) go to the core once (`kMeshMaterials`): the diffuse map (`rl_material::get_texture_data(int*, "Diffuse_Map")`) and the constants `Diffuse_Color`, `Diffuse_Map_TilingU/V`, `Self_Illumination` (`get_constant_data`, asked only where `get_constant_loc` finds the name: for one its shader lacks, the game's copy reads from before the constants). The diffuse maps: `src/rfgrt/mirror/textures.cpp` copies each (`get_dev_texture`'s object, its D3D11 view at +0x18) at the frame seam, 8 a frame, its mips from 256 texels down, into a texture shared with the core (`kTextureAdd`), sent once the core has queued the frame whose fence A follows the copy (a frame it queues later waits on a later fence value); a mesh's materials wait for their textures (5 s at most, then again once they are all there). The core (`scene.cpp`) views each texture in its bindless range (t0.. in space2, 4096) and keeps a slot of 32 packed entries per mesh in a buffer (t7), its index in the geometry tables' header next to the texture coordinates' offset (TexCoord0, `kMeshUV`); `hit_surface` interpolates the texture coordinates at the hit and samples the diffuse map at a level of detail from ray cones (the triangle's texel area against its world area; the reflections' footprint a pixel's, the GI's a 0.15 rad cone), times the colour; a self-illuminated material is lit toward albedo x 4, as `rfg-standard` does. Hits off screen (the GI's after the radiance cache, the glass's reflections always, no longer the cache's blocks) take the surface's own colour; where a mesh has no texture coordinates, its material is unknown or has no diffuse map: `GI.Albedo`'s grey. The probe (`Test.ProbeKey`) logs the camera ray's surface: its albedo, material slot, entry and texture. 0: every hit grey, as before |
| `GI.Bounce` | 1.0 | the bounced light's scale (the sky's light stays): the game was lit without real bounce, so shaded faces toward sunlit ground get 1.5-3x brighter than vanilla at 1; lower for a look closer to the original |
| `GI.Denoiser` | 2 | the GI's denoiser. 2: DLSS Ray Reconstruction (`src/rtcore/dlss.cpp`, NGX; the SDK fetched by `tools/fetch_dlss.ps1` into `third_party/dlss`, the runtime `nvngx_dlssd.dll` beside `rfgrt_host.exe`): the traced irradiance with guide images (`shaders/rr_guides.hlsl`: linear depth, the camera's motion in pixels, the normal from the depth with roughness 1, a white albedo: the irradiance has no texture in it), DLAA, in place of the temporal and wavelet passes; `rr_finalize.hlsl` writes it with the sky's alpha 0. Where NGX lacks it (other GPUs, builds without the SDK) and the frame it is created at: 1. 1: the wavelet below. 0: `gi_filter.hlsl`'s single 7x7 pass (`GI.FilterRadius`) |
| `GI.Wavelet` (1, replaced by `GI.Denoiser`) | | the GI's spatial filter: an edge-avoiding a-trous wavelet (`shaders/gi_atrous.hlsl`, as SVGF's): four passes of a 5x5 B3 kernel at steps 1, 2, 4, 8 (about 61 x 61 pixels), weighed by the distance to the pixel's surface plane and by the irradiance's difference against the noise left there (the 3x3 spread of the accumulated value, measured in the first pass). 0: `gi_filter.hlsl`'s single 7x7 pass (`GI.FilterRadius`). The history is cut short (3-6 frames) only where the light changed by 30-50 % and beyond three standard errors of the frame's 5x5 mean |
| `Test.VanillaAt` | (none) | `Test.VanillaKey` pressed at these gameplay seconds (tests) |
| (no setting) | | the sky's reflection where the reflections' patch could not go (the terrain's shaders: "no single specular ambient value"): `src/rfgrt/patch/spec_occlusion.cpp` scales indirectLightParams.z (cb0[7].z, the specular ambient's scale) by saturate(luminance(t14 irradiance) / luminance(cb0[2] ambient)) where t14's alpha != 0. With GI the AO in ClbTexture.R is 1 (the traced light holds it), so without this those shaders reflected the open sky in caves and corridors. WARP checks SA/SB/SC |
| `GI.Glass` | 1 | the plain glass (`rfg-glass`'s main pass: windows, shards; and `rfg-glass-etched`, which reads ClbTexture's occlusion only) lit by the path tracer, its sun's visibility soft: two rays a frame over the sun's disk (`Shadows.SunAngle`), averaged over frames and filtered with the light; t17's alpha is 1 + that visibility, which scales the glass's sun highlight. The core (`shaders/glass_light.hlsl`, after the GI trace, with its bindings) finds the nearest clear glass in front of each pixel's surface (one query, `RAY_FLAG_CULL_OPAQUE`: the non-opaque instances' clear glass geometries, every geometry of the glass-only ones) and traces its light: the sun through a shadow ray (from the side facing it), the sky and surroundings through `GI.Rays` cosine rays (as the GI's) into the core's RGBA32F `glass_raw` (the sky and bounce only, averaged at 10 % a frame with the previous frame's at the same point of the glass: reprojected into `glass_prev`, the core's copy of the previous frame's, where that frame saw glass at about its view depth; read in place until 0.1.17, the averages trailed behind the view as it moved; the alpha packs the view depth, the sun's visibility and cosine), then `shaders/glass_filter.hlsl` averages that over 25 pixels of a 9 x 9 window of glass at the same distance and adds the sun back per pixel, into a texture the plugin shares (`rfgrt_ipc::Block::glass_handle`): rgb the irradiance (the sun's included), alpha the distance, negative in shadow, 0 without glass. `src/rfgrt/patch/glass.cpp` makes the glass shader take it at t17 in place of its own light (the game's ambient with a fake bounce) and drop its sun highlight in shadow; checked on WARP (GA/GB/GC). The log counts the pixels with glass a frame |
| `GI.FrostedGlass` | 1 | the translucent (patterned, `rfg-eosglass`) glass diffuses the light through it, as frosted glass does: no direct light through it, its panes glowing on the other side instead. The passes that trace set bit 32 of their constants' `flags`; with it `RT_THROUGH` and `RT_PROCEED` (`rt_query.hlsli`) commit a translucent candidate like an opaque one, unless it lies within `kPaneThickness` (25 cm) of the ray's origin (the pane the ray starts on: so a pane's own sun visibility stays its outer face's, and the game's glow on it, which follows `ClbTexture`'s sun, keeps working). A hit on such a pane (`hit_translucent`: its instance table entry's bits 8-31) is shaded by `pane_radiance` in place of the game's colour and the radiance cache (its drawn look): 0.6 of the light on its far side (the sun through a shadow ray from 30 cm beyond it, the sky's ambient for its far face), tinted half way by its pattern's hue (its texture over its brightest channel: the pattern is a dark teal, its look, not its transmittance), plus 0.1 of its near side's light times its texture. The pane's own look stays the game's (`rfg-eosglass`'s glow: its pattern times its cubemap times the sun's colour times `ClbTexture`'s sun visibility there, with no floor where traced): so that a pane out of the sun still glows as frosted glass under the sky does, the sun shadow pass (`sun_shadow.hlsl`) finds the pixels whose surface is a pane (the camera's ray stops on a translucent geometry at the depth's distance) and adds 0.3 (the game's own floor for that glow) times the sky its far side sees (one cosine ray a frame from 30 cm beyond it, averaged and filtered with the shadow); a pane between closed rooms stays dark. 0: 0.1.13's model, a share (30 %) of the sun through each pane, a hashed 70 % of the other rays stopped |
| `GI.WindowTint` | 1 | the sun's light through clear glass takes the glass's colour where it passes it, texel by texel (stained, tinted or dirty glass casts its pattern). The core (`shaders/sun_tint.hlsl`, after the glass's passes, the GI's root signature) traces from each pixel's surface toward the sun's centre and multiplies, for each clear pane the ray crosses (one query, `RAY_FLAG_CULL_OPAQUE`: the clear glass geometries of the non-opaque instances, every geometry of the glass-only ones; contribution bit set, not translucent; a pane's faces within 25 cm counted once), what the game's own blend of it shows through: `lerp(1, colour, alpha)` of its material's texel there (texture times `Diffuse_Color`, the texture's alpha; the LOD of the receiving pixel's footprint), into a texture the plugin shares (`sun_tint_handle`, RGBA8: rgb that, a 1). `src/rfgrt/patch/sun_tint.cpp` (on the GI-patched base lit passes, after the shadow floor's patch, before the fog's) loads t20 at the pixel and has every read of the sun's colour (cb1[0], `g_lightingFragmentConstants.lightColor`) read it times that where alpha != 0; the light passes keep their own colour. Checked on WARP by `shaderpatch` (TA alpha 0 and TB white leave the output as it was; TC a colour equals the shader run with the sun's colour times it). The shadow's softness stays `ClbTexture`'s; the tint follows the sun's centre. The bounced light comes through the windows tinted too: the GI's, the glass's and the reflections' rays (the passes' flags & 64) gather the clear panes they pass in their own traversal (`trace_common.hlsli` `RT_PROCEED_PANES`: the clear glass candidates, up to 4, a pane's faces once, each with `pane_pass` there; the texel's LOD from the GI's wide cone) and multiply what they bring back by those before their hit (`panes_through`). 0.1.17 traced two more queries along each ray for them (`glass_through`): 1.5 ms more GI in window-heavy rooms, enough to make frames miss the core's result. Live 83 (the user's second wc4 recording): the tint now costs the GI's rays 0.3 ms and the reflections 0.1 ms (0.1.17: 1.7 ms of GI), with the same colours as 0.1.17's at the recording's marks (linear means of the floor and walls equal to the fourth decimal). The sun's tint pass itself: 0.47 ms. 0: white sunlight and white bounce through every window |
| `GI.TerrainAlbedo` | 0.032,0.010,0.002 | the albedo the rays give a surface whose material is not known: the terrain (drawn by its own shader, blending four layers by a blend map), where neither the game's colour of the previous frame nor the radiance cache has it. Calibrated on a sunlit point of wc4's soil (`Test.Probes`: the game drew 0.042 0.010 0.001 where the rays made 0.336 0.143 0.044 of 0.25 0.14 0.08): the game's soil is a dark rust in its units, ten times darker than `GI.Albedo`'s grey, which showed white in the windows' reflections |
| `Fog.Enabled`, `SunShare`, `Anisotropy` | 1, 0.65, 0.25 | with GI: the game's fog made volumetric. The game's vertex shaders compute its transmittance per vertex, `T = 2^-max(tau - c, 0)`, `tau = a d (1 - e^(-b dy)) / (b dy)` (`g_commonVertexConstants.fogDistance = (b, a, -, c)`, `dy` the height above the camera), and its lit pixel shaders blend `colour T + fogColor (1 - T)`. The plugin sends `fogDistance` (`rl_renderer::m_commonVertexData` + 208) and `fogColor` (`m_commonFragmentData` + 64) with each frame's scene (`frame_scene.h`). The core (`shaders/fog_*.hlsl`, after the glass's passes, with GI's root signature and a linear static sampler) traces, for a camera-aligned grid of 160 froxels across by 64 exponential slices (0.5 m to the camera's far plane), a shadow ray toward the sun's disk and an upward cosine ray (250 m: the sky) from a random point of each froxel, averaged over up to 20 frames with the reprojected previous grid (`fog_inject`); integrates each column front to back into R, the fog's light over the game's fog's up to each slice (`fog_integrate`: each froxel's `(s p V_sun + (1 - s) V_sky) / (s p + 1 - s)`, `s` = `SunShare`, `p` the Henyey-Greenstein phase of `Anisotropy` toward the sun, weighted by the fog the slice holds); and writes per pixel `(fogColor R (1 - T), T)` with the game's T to the surface (`fog_apply`; beyond the grid the fog is taken as lit in the open; 0 without a main-scene surface: the sky, the landmark layer) into a texture the plugin shares (`fog_handle`; `host_fog` says the core writes it). Fog both the sun and the sky reach is the game's exactly. `src/rfgrt/patch/fog.cpp` (on every pixel shader, last) finds the fog's interpolator component (read only by `add r, -v.c, l(1)` and a `mad` with cb0[4]) and reads it as 0 where t18's alpha > 0; the plugin binds the core's fog at t18 from the frame's first lit pass (`set_shadow_states`, armed at `render()`'s start) to the end of the lighting passes (`on_lit`), where it unbinds it and blends the fog over the lit scene (`ONE, SRC_ALPHA`, after the GI's capture of the lit colour, which so holds no fog): lit passes and lights' passes alike get the volumetric fog, what is drawn after (glass, particles, effects) the game's. Checked on WARP (XA/XB). F11 and the fallback keep the game's fog. The fog's and the ambient's constants are the lit passes' (`gi::lit_constants`: the scene thread's renderer at `set_shadow_states`, the frame before), not render()'s renderer's, which holds its depth pre-pass's and cascades' part of the time (the fog off: density 0, falloff 1; read beside them, a frame now and then went without fog). `Test.FogDensity` (tests: 1) scales the volumetric fog's density, to see its structure in thick fog (the sky, the landmarks and see-through things keep the game's) |
| `GI.Cache` | 1 | the world radiance cache (`shaders/radiance_cache.hlsli`): the light the game drew for each surface it showed, kept by place and facing (cells of about 1.5 % of the distance from the camera, from 12.5 cm) for half a minute, so that rays hitting it later, out of view, still get it (glows, lit patches); filled from a quarter of the pixels a frame (`cache_update`), averaged per cell (`cache_resolve`). 0: hits out of view get the sun and one more ray only |
| `GI.SkyDrawn` | 1.0 | what a ray that gets out sees (`shaders/sky_probe.hlsli`: the drawn sky learnt from the pixels showing it into a 32 x 32 octahedral map, not within 8 degrees of the sun, averaged over about 10 frames, forgotten after half a minute unseen; the mean of what was seen for directions not seen; the game's clear colour, pure green in its first seconds, left out). 0: the game's ambient colour (bluish by day: what lit doorways blue); 1: the drawn sky's colour, each direction's relative to the mean, at the brightness of the game's ambient; 2: the drawn sky as it is (by day about 0.1 against the ambient's 0.4: shade about 4x darker); between: blends. Below the horizon the game's back ambient stays; reflections that get out see it too |
| `GI.Sky` | 1.0 | the sky as a light: the game's ambient colour as its radiance, reaching what can see it (a GI ray that gets out; at a hit out of view, one more ray from it). 0: none, only the sun, the lights and glows, bounced. Ray hits out of view get the sun, then one cosine-weighted ray: the sky where it gets out, a second bounce where it meets something |
| `AO.Enabled`, `Rays`, `Radius` | 1, 2, 2.0 | `Host.Mode=3`: ray-traced ambient occlusion into `ClbTexture`'s R in place of the game's SSAO (skipped while ours is there); rays a pixel and frame over the hemisphere, their length in metres (accumulated and filtered with the shadows) |
| `Reflections.Enabled`, `Length`, `Gloss`, `History`, `FilterRadius` | 0, 500, 32, 6, 1 | with `GI.Enabled=1`: path-traced reflections in place of the lit shaders' specular ambient (the sky looked up along the reflection vector): one ray a pixel about the mirror direction off the depth's normal, over a lobe of exponent `Gloss`, `Length` metres; the hit's colour is what the game drew there in the previous frame, else a diffuse surface under the sky and the sun; averaged over `History` frames and filtered (`FilterRadius`) |
| `Lights.Enabled`, `Max`, `Fixture` | 0, 16, 0.3 | `Host.Mode=3`: shadows for the dynamic lights. The scene renderer's visible point and spot lights, nearest first, get a ray-traced shadow mask each (one ray a pixel and light toward its centre), bound at `t15` for that light's own pass; the light-pass shaders that did not read `t15` are patched to multiply by it. A light beyond `Max` gets a white mask (no shadow, but no longer the sun's either). `Fixture`: metres around a light within which nothing shadows it (the lamp around it) |
| `Test.TimeOfDay` | -1 | the game clock set to this hour (0-23) once gameplay runs, and held there: daylight tests from a night save (`game_clock_set_time`, `game_clock_freeze_ingame_time`) |
| `AO.WithGI` | 0 | with the path-traced GI on, ambient occlusion rays only if 1: the irradiance already holds the occlusion (rays that hit something nearby bring back its bounce, not the sky), so by default R is 1 and the game's SSAO stays off |
| `Shadows.Rays`, `SunAngle`, `FilterRadius`, `History` | 2, 0.5, 2, 12 | `Host.Mode=3`: rays a pixel and frame, the sun's angular radius in degrees (penumbra softness), the edge-aware filter's radius in pixels, the most frames the temporal accumulation averages (1: none) |
| `Mirror.MaxInstances` over its cap | | each frame scene counts its instances by distance (log2 of the squared distance in quarter steps, `src/rfgrt/mirror/instances.cpp`); after one over the cap the next leaves out those beyond the distance that keeps 95 % of it (terrain and characters always kept), instead of whichever came last. The log: "over the cap (N): ... left out for being farthest, beyond X m" |
| `Shadows.Specular` | 0 | (with it, in `shadow_floor.cpp`: rfg-eosglass's glow `mad <shadow>, 0.7, 0.3` gets weight 1 and floor 0 where traced, and every `mul` by the sun's colour (cb1[0]) whose other factor is not shadowed and whose result is not multiplied by a shadowed value later, the game's own eosglass variants' glow and foliage among them, is multiplied by ClbTexture's G read at the pixel where traced) the sun's highlight a surface keeps in full shadow where the core traced it. The game's lit shaders scale it by `max(shadow, 0.05)`; with path-traced shade (dark indoors) those 5 % shone as sun glints in buildings, containers and vehicles. `src/rfgrt/patch/shadow_floor.cpp` turns the floor into this value where the irradiance at t14 has alpha != 0 (the game's 0.05 elsewhere: F11, the core not answering); checked on WARP by `shaderpatch` (FA/FB/FC). 0.05 or more: not patched |
| `Host.Path` | `scripts\rfgrt\rfgrt_host.exe` | |
| `Host.RingMB` | 32 | the scene ring between the plugin and the core |
| `Host.WaitMs` | 50 | the most a frame waits at its seam for the core to queue it (its GPU then waits on fence B for the result, `seam.cpp` `host_frame`). Where the GPU is the limit, the core queues frame n once its frame n-3 is done (`kRing`), so every frame waits nearly a frame's GPU time: the cap has to stay well above the slowest frames. A frame that gives up gets the core's last sun shadow and occlusion (`g_last`), and its patched shaders read the other textures unsynchronized: the previous frame's, or torn while the core writes them. At 10 ms (until 0.1.17), with GPU passes of 10-15 ms in a debris-heavy Wrecking Crew match, up to half the frames gave up: the lighting lagged behind the picture in layers and the glass flickered (the user's second wc4 recording replayed, live 83: 18.6 % of the frames missed with 0.1.17 (22.6 % with its glass search capped at 25 m, 1.4 % without the window tint), 0.3 % with 50 ms, all while loading). 0: never wait |
| `Mirror.Enabled` | 1 | with the core: every mesh and, each frame, every render instance go to the core |
| `Mirror.MaxInstances` | 65536 | render instances per frame scene |
| `Test.ProbeKey` | 121 (F10) | the key (a virtual-key code, in decimal) that has the core log what the rays under the crosshair meet (`shaders/probe.hlsl`): from the surface there toward the sun (what shadows that spot: a mesh's name, kind, submesh and draw range), and from the camera through it (nearer than the game's surface: something the rays see that the game does not show; farther or nothing: what the game drew is not in the rays' scene). 0: none |
| `Test.ProbeAt` | 0 | one probe at this gameplay second (tests) |
| `Test.ProbeAtFrame` | -1 | one probe at this gameplay frame (`session::gameplay_frames()`: the PhysX harness's replay frame numbers, so a probe lands on a recording's marked frame) |
| `Test.ProbePixel` | 0.5,0.5 | where the probe aims, as fractions of the screen (x, y from the top left) |
| `Test.Probes` | (none) | several probes in one run: `frame@x,y/frame@x,y/...` (gameplay frames as `Test.ProbeAtFrame` counts them, points as `Test.ProbePixel`), one a frame from its frame on (several at one frame go on the frames after it). Each logs, besides the usual: what a mirror at the first surface there (glass included) would show and from what it is shaded, that surface's colour three ways (the game's where it drew it, the radiance cache's, its material's under the sun and the ambient) and its albedo; and for the camera ray's own surface the game's colour against what the rays make of it (the albedo's calibration) |
| `Test.VanillaKey` | 122 (F11) | the key that switches between the path tracing and the game's own lighting and shadows, live, for comparing: the core keeps running and in step (fence A every frame), its results unused; the patched shaders get no irradiance or reflections (their alpha-0 fallback, the game's own terms), the lights white masks, and the game's cascades and SSAO run again. 0: none |
| `Mirror.TerrainChunks` | 1 | terrain chunk by chunk: a subzone's height mesh gets its chunk table (25 chunks) only after it is made, so the mirror keeps its data until then and sends it a submesh a chunk; the snapshot puts in only the chunks the game draws. Neighbouring subzones overlap (their tiles span 160-207 m, 154 m apart) and the game draws one's chunks there: sent whole, the other's parts shadowed the drawn ground in bands along the slopes. 0: each height mesh whole, as made, once a frame |
| `Mirror.TlasFastTrace` | 1 | the frame's TLAS built `PREFER_FAST_TRACE` (0: `PREFER_FAST_BUILD`, as until 0.1.17). It is rebuilt every frame (its instances move), but the rays take far more of the frame than its build, and thousands of Geo-Mod pieces and debris overlap (live 83, the user's second wc4 recording: pass 9.4 against 9.7 ms, the GI's rays and the glass's light 0.2 ms faster, the build no slower) |
| `Mirror.Glass` | 1 | rays go through glass and every other transparent material (`rl_material_data::mat_flags` bit 0): an instance drawing some is non-opaque and its transparent geometries are skipped (`rt_query.hlsli`); one drawing only such ranges too, every geometry flagged (more than 23 geometries: out of the rays' mask, as every such instance was until 0.1.17); 0: everything stops rays. Translucent glass (`rfg-eosglass`'s materials, found by their shader handle: Instance.glass bit 31) stops 70 % of the GI's, reflections' and lights' rays, chosen by a hash of the ray and the pane (`RT_PROCEED`); the sun's shadow rays and the fog's carry its share through instead, 30 % a pane crossed (`RT_THROUGH`), so the light through it comes smooth. A pane: translucent candidates within 25 cm of each other along a ray (its two faces count once). The clear glass in the same instance stays clear: Instance.glass bit 30 gives each kind its own entries (bits 0-14 clear, 15-29 translucent; all among the first 15, else all of it translucent), the TLAS marks the instance with contribution bit 23 and its translucent geometries in its instance table entry's x (bits 8-31), which the passes with the table read (the sun shadows, GI, reflections, the glass, the fog; the lights' shadows take all of such an instance's glass as translucent) |
| `Reflections.Glass`, `GlassReflectance` | 1, 0.2 | with `GI.Glass`: the clear glass (the patched `rfg-glass` shaders) reflects. The glass pass (`glass_light.hlsl`) finds the nearest clear glass in front of each pixel's surface (the clear glass geometries of the non-opaque instances, the glass-only ones among them: `RAY_FLAG_CULL_OPAQUE`), traces the mirror direction off it (the game's lit colour where it saw the hit; else the surface's own material, `GI.Textures`, lit by `mirror_shade`: where the radiance cache has the surface, the larger of the sun on it and the cell's light over the material's mean albedo (the bounce and sky the game drew it with), elsewhere the sun and the sky as four fixed rays about its normal see it (unoccluded, the walls of a room behind the camera showed flat blue); no random ray: the reflection stays still. The terrain: the cache's colour, else `GI.TerrainAlbedo`), weights it by Schlick's Fresnel term from `GlassReflectance` (0.04 is plain glass, physically: hard to see head-on; 0.2 the default, the user wanted near-mirror windows; 1 a mirror) and the game's fog to the glass, into the plugin's texture at t19 (rgb F L, a F). The glass shader's patch (`patch/glass.cpp`) sends its output through a temp and before each `ret` gives it alpha a + F (1 - a) and colour (c a + F L) / that, so that blended (SRC_ALPHA, INV_SRC_ALPHA) the reflection sits on the glass and the scene behind it dims by 1 - F; checked on WARP by `shaderpatch` (GE) |
| `Test.WreckingCrew` | (none) | unattended runs: main menu -> Wrecking Crew -> this map (its file name, `wcdlc4`, or its index; `current`: the menu's) -> the match, as the PhysX project's replays start one (`main_menu_handle_selection(2)`, the menu's data loaded, `wrecking_crew_options::map_index`, `wrecking_crew_match_begin()`); the match's "how to play" screen waits for Return (`tools/input/wc_look.txt`, `wc_survey.txt` press it). The log lists the maps |
| `Mirror.Skinning` | 1 | skinned instances' bone palettes go to the core each frame, which poses their meshes and refits a BLAS per instance (0: characters in their bind pose) |
| `Telemetry.GeometryCensus` | 0 | log every mesh the renderer creates, by creator and vertex layout |
| `Telemetry.DumpShaders` | 0 | save the bytecode of every shader KAIKO's layer loads by name (packed into `rfg.exe`; the terrain's among them) into the run folder's `shaders\`, for `fxc /dumpbin` |
| `Test.AutoLoadSave` | 0 | main menu, Load Game, the most recent save, without input |
| `Test.BlockSaveWrites`, `GodMode`, `IgnoreFocus` | 0 | unattended-run safety |
| `Test.ExitAfterGameplaySeconds`, `ExitAfterMainMenuSeconds`, `ExitAfterSeconds` | 0 | timed exits and the watchdog |
| `Test.ShotsAt` | | screenshots at these gameplay seconds |
| `Test.KillHostAtSeconds` | 0 | ends the host as a crash would (tests the fallback) |
| `Test.MissEvery` | 0 | every Nth frame's core result left unused, as if the core had not got to it. Tests the misses (1-5 % of frames in play, more while an area streams in): the cascades stay without casters while the core answered within the last 4 frames, and a missed frame gets the core's previous result, so no frame goes without the sun's shadow or switches to the game's |
| `Test.InputScript` | | a file of scripted mouse and keyboard input at gameplay seconds (camera turns, walking; format in `src/rfgrt/test/input_script.h`, an example in `tools/input/pan_walk.txt`) |

## Layout

| Path | Contents |
| --- | --- |
| `src/rfgrt/` | the plugin: `core/` (settings, log, session, build check, main-loop hook), `game/` (symbols, access), `hooks/`, `render/` (the frame seam, D3D11 state), `rt/` (the link to the core), `mirror/` (the scene mirror: meshes, render instances, the geometry census), `harness/` (screenshots), `test/` (unattended runs), `shaders/` |
| `src/rtcore/` | the ray tracing core (x64, D3D12) and its shaders |
| `src/shared/` | the plugin-core protocol, the version resource |
| `host/` | `rfgrt_host.exe` |
| `tools/` | build, symbols, sandbox, run scripts; `jobs/` (atomic job files) |
| `third_party/` | MinHook 1.3.4 (BSD-2), Ultimate ASI Loader 9.7.4 (MIT), from the PhysX project |
| `research/` | the feasibility study (`docs/`), its probes (`probes/`) |

## Where the frame is entered

[src/rfgrt/render/seam.cpp](src/rfgrt/render/seam.cpp) hooks `rl_scene_renderer::render` (render thread):

- **At its start** the frame gets its number and the plugin writes the frame's scene (every render instance's mesh,
  submesh and transform, and the camera) for the core. The game holds its render data still here: the sim thread
  changes render instances only inside the frame handshake (`rl_unlock` .. `rl_lock` in
  `game_handshake_do_middle_of_frame`) and defers adds and removes otherwise.
- **The seam:** `keen::graphics::endCommandBuffer` is called only by `render`, after the render thread has recorded
  the depth pre-pass, the occlusion queries and the shadow cascades with their projection into `ClbTexture`, and just
  before it executes the scene thread's command list (SSAO, the base pass that samples `ClbTexture`, everything after).
  The plugin copies the depth, signals fence A, and applies the core's result if it is ready. (`render` clears the
  game's own `Current_scene_renderer` before that point, hence the hook at its start.)
  ([research/docs/01 §3-4](research/docs/01-findings.md))

## The scene mirror

What the core knows of the game's scene ([research/docs/03 phase 2](research/docs/03-implementation-plan.md)):

- **Meshes** ([src/rfgrt/mirror/geometry.cpp](src/rfgrt/mirror/geometry.cpp)): `keen::graphics::initializeRenderGeometry`
  creates every mesh and `freeRenderGeometry` releases it. While the creation call runs, the game's CPU copy of the
  vertices and indices still exists: the plugin decodes the positions to float3, converts strips to triangle lists,
  reads `rl_mesh`'s submesh table (an instance draws one submesh; a submesh is a list of draw ranges and an offset),
  and writes one record into the scene ring ([src/shared/scene_ring.h](src/shared/scene_ring.h)). The core copies it
  into an upload ring and builds one BLAS per submesh (one geometry per draw range, fast trace, compacted) on a queue
  of its own. Copying the CPU data replaced the plan's legacy-shared vertex buffers: the game's D3D11 resources stay
  untouched, nothing depends on a buffer's lifetime, and the in-process core of a 64-bit game reads the same records.
- **Instances, per frame** ([src/rfgrt/mirror/instances.cpp](src/rfgrt/mirror/instances.cpp)):
  `rl_scene::add_renderable_instance` / `remove_renderable_instance` keep a registry (the game replays its deferred
  adds and removes through the same functions). At `render`'s start the plugin writes each instance of the scene
  ([src/shared/frame_scene.h](src/shared/frame_scene.h)): mesh id, submesh, and the transform the game draws it with
  (the world matrix minus the scene's render offset; for Geo-Mod buildings each active piece, rotated by the game's
  own `matrix::set`). The core builds the frame's TLAS from it while the game is still rendering, and traces once
  fence A says the depth is there.
- **Skinned characters** ([src/rtcore/skinning.h](src/rtcore/skinning.h)): meshes with bone indices and weights in
  their vertices (the `_c` / `_mc` materials) carry them in their record (`kMeshSkinned`), and the core keeps their bind
  pose on the GPU. Each frame scene adds, for every visible `rl_skin_instance`, its bone palette
  (`m_bone_matrices_p`, `matrix43` a bone); the core skins the bind pose with it (`shaders/skin.hlsl`) into a vertex
  buffer of the instance's own, builds a BLAS once and refits it every frame after, and the TLAS takes that BLAS
  instead of the mesh's bind-pose one.
- Left out so far: clone meshes (foliage), low-LOD and landmark meshes, particles, decals; alpha-tested surfaces are
  opaque.
