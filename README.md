# rfgrt: path tracing for Red Faction Guerrilla Re-Mars-tered

<div align="center">
  <a href="https://discord.gg/EmmwryzskU">
        <img src="https://img.shields.io/discord/1324804381610213407?color=blue&labelColor=555555&label=&logo=discord&style=for-the-badge"
            alt="Chat on Discord"></a>
</div>

> [!WARNING]
> This patch has been pretty much entirely been generated using AI; 
> I won't and will never claim to have enough knowledge or expertise to do this 
> kind of reverse-engineering by myself; I've tested a few wrecking crew levels and a couple single player saves
> and so far spotted no major issues (on my machine ofc, if you have issues feel free to open an Issue) keep in mind the game will probably very often run out of memory.
> This patch currently probably only works for the latest GOG and Steam versions of the game.
> If you're interested and want more (human generated) info, I have a blog post [here](https://slop-blog.enkhayzomachines.net/posts/red-faction-guerrilla-physx-integration) :)

A mod for *Red Faction Guerrilla Re-Mars-tered* (GOG and Steam) that lights the game by path tracing, the way
Cyberpunk 2077's RT Overdrive does: the game keeps drawing its own image (geometry, materials, decals, particles,
post-processing), and the light falling on it is path-traced on the graphics card, denoised, and fed into the game's
own shaders.

- **Global illumination:** the light bounced off everything around, in place of the game's flat ambient colour. Rooms
  and alleys darken where the sky can't reach, shaded walls take on the colour of the sunlit ground in front of them,
  and lights and glowing materials light their surroundings.
- **Ray-traced shadows** for the sun (soft, sharp where objects touch the ground, without the shadow maps' limits) and
  for the dynamic lights: explosions, fires, flares, lamps.
- **Reflections:** the game's reflective materials and its windows reflect the path-traced scene instead of a plain
  sky.
- **Glass:** windows are lit by the path tracing, sunlight through clear glass takes its colour, and the multiplayer
  maps' patterned glass scatters the light through it.
- **Volumetric fog** in the game's colour and density, lit by the sun and the sky, with light shafts.
- Everything the game draws takes part: buildings and every piece Geo-Mod breaks them into, debris, vehicles, the
  terrain and animated characters.
- Denoised by NVIDIA DLSS Ray Reconstruction on RTX graphics cards, by the mod's own filter elsewhere.

The path tracing costs about 7 ms of GPU time a frame at 1920x1080 on an RTX 4090 with Ray Reconstruction, around
9 ms in a Wrecking Crew match among glass buildings and debris.

## Install

Download a release and extract it into the game folder, the one with `rfg.exe`. The bundled `rfgrt_README.txt`
([tools/release/README.txt](tools/release/README.txt)) covers requirements, settings, uninstalling and known
limitations.

- **GOG or Steam version.** With any other `rfg.exe` the mod stays inactive and the game runs unmodded.
- **64-bit Windows 10 (version 2004 or newer) or 11, and a graphics card with DirectX ray tracing (DXR 1.1):** NVIDIA
  RTX 20 series or newer, AMD Radeon RX 6000 series or newer, Intel Arc. Without one the game runs as it ships.
- **Two archives:** `rfgrt-<version>-with-asi-loader.zip` includes Ultimate ASI Loader as `dinput8.dll`;
  `rfgrt-<version>-plugin-only.zip` is for a game that already has the loader. Sledge, the game's mod manager, uses
  `dinput8.dll` too: with Sledge, take the plugin-only archive, with Ultimate ASI Loader installed as `winmm.dll`.
- **Memory:** the game is 32-bit, so it and every mod in it share 4 GB of address space. With rfgphys, a long Wrecking
  Crew match full of destruction can use it up, and the game then stops. Objects flickering in and out are the sign
  that it is nearly used up: restart the game then.
- **With rfgphys, use version 0.5.0 or newer.** Older versions take 128 MB more of that memory, and with the Steam
  version (whose own libraries take a little more of it than GOG's) the game then crashed already in the main menu in
  testing.

## How it works

- `scripts\rfgrt.asi` (32-bit, loaded by [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader))
  hooks the game's functions with [MinHook](https://github.com/TsudaKageyu/minhook), at addresses and struct layouts
  taken from the debug symbols (`rfg.pdb`) that ship with the GOG version. The Steam version's addresses come from the
  map rfgphys's `match_builds.py` made of its `rfg.exe` (`tools/steam_map.json`). With any other build the plugin
  stays inactive.
- The game is 32-bit, and graphics drivers give 32-bit programs no hardware ray tracing. The path tracing runs in
  `scripts\rfgrt\rfgrt_host.exe`, a 64-bit Direct3D 12 process that the plugin starts and that ends with the game. The
  two share the GPU work through shared textures and fences, and each frame waits for its own path tracing.
- For a 64-bit build of the game (being ported by another project), the same plugin built for x64 runs the path
  tracing inside the game's own process instead.
- Every mesh, Geo-Mod piece and terrain tile the game draws is mirrored into the 64-bit process, with an acceleration
  structure, its texture coordinates and its materials' textures. The top-level structure is rebuilt every frame, and
  animated characters are posed every frame.
- Where a ray hits something on screen, it takes the colour the game drew there the frame before. Out of view it takes
  the light kept in a world-space radiance cache, or lights the surface's own texture with the sun and the sky it sees.
  Rays that get out see the sky as the game draws it, and rays go through glass, tinted by it.
- The game's lit pixel shaders are rewritten as the game creates them: their ambient colour reads the path-traced
  light, their reflection of the sky the path-traced reflections, the sun's and the dynamic lights' passes the
  ray-traced shadows, and their fog the volumetric fog. Every rewrite is checked offline against the original shader on
  Direct3D's software rasterizer (WARP), by what the two compute (`tools/shaderpatch`).

## Building from source

Requirements:

- Windows 10 or 11, 64-bit
- Visual Studio 2026 (18) Community with the *Desktop development with C++* workload and the Windows SDK. The scripts
  use the CMake that comes with Visual Studio, at its default install path.
- Python 3 and Git

```powershell
git clone https://github.com/slopkhayzo/RedFactionGuerrillaRemarstered-PathTracing rfgrt
cd rfgrt
powershell -ExecutionPolicy Bypass -File tools\fetch_dlss.ps1   # optional: NVIDIA's DLSS SDK into third_party\dlss
powershell -ExecutionPolicy Bypass -File tools\build.ps1        # build-x86\RelWithDebInfo\rfgrt.asi, build-x64\RelWithDebInfo\rfgrt_host.exe
python tools\package.py                                         # dist\rfgrt-<version>-*.zip
```

- **DLSS Ray Reconstruction** is optional: without NVIDIA's SDK the path tracer is built with the mod's own denoiser
  only. `tools\fetch_dlss.ps1` downloads the SDK's headers, library and runtime from NVIDIA's repository, pinned to one
  tag; they stay out of git.
- **Ultimate ASI Loader 9.7.4:** `package.py` needs `dinput8.dll` from the official
  [`Ultimate-ASI-Loader.zip`](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/tag/v9.7.4) in
  `third_party\ultimate-asi-loader\`; it checks the file's SHA-256. Each release's source is tagged with its version,
  for example `v0.1.18`.
- **The game's addresses** are in `src/rfgrt/game/rfg_symbols.generated.h`, for both builds. `tools/gen_symbols.py`
  generates it from `tools/symbols.txt`, the GOG game's `rfg.pdb` (read with analysis scripts that aren't part of this
  repository) and `tools/steam_map.json`.
- **The shader checks** (`tools/shaderpatch`) run on the game's own shaders, extracted with the same analysis scripts.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/rfgrt/` | the plugin: `core/` (settings, log, build check, main-loop hook), `hooks/`, `game/` (the game's addresses and helpers), `mirror/` (the meshes, instances and textures sent to the path tracer), `render/` (where the path tracing joins the game's frame: shadows, lights, global illumination, fog), `patch/` (the shader rewrites), `rt/` (the link to the path tracer), `shaders/` (the plugin's own), `harness/` and `test/` (screenshots, shader dumps, unattended-run automation) |
| `src/rtcore/` | the path tracer (Direct3D 12): the scene's acceleration structures, skinning, the frame's passes, DLSS Ray Reconstruction, and its HLSL shaders (`shaders/`) |
| `src/shared/` | the shared-memory protocol between the plugin and the path tracer, the version resource |
| `host/` | `rfgrt_host.exe`, the 64-bit process that runs the path tracer for the 32-bit game |
| `tools/` | build, package and test scripts; `shaderpatch/` checks the shader rewrites; `release/` holds the release's settings and README |
| `third_party/` | MinHook and Ultimate ASI Loader's license, and the DLSS SDK once downloaded ([third_party/README.md](third_party/README.md)) |

Testing and development workflow: [DEVELOPMENT.md](DEVELOPMENT.md).

## License

MIT ([LICENSE](LICENSE)). Third-party components keep their own licenses: MinHook (BSD 2-Clause), Ultimate ASI Loader
(MIT) and NVIDIA DLSS Ray Reconstruction (NVIDIA RTX SDKs License: the SDK isn't kept in this repository, and the
release archives include its runtime, `nvngx_dlssd.dll`); see [third_party/README.md](third_party/README.md). NVIDIA,
RTX and DLSS are trademarks of NVIDIA Corporation.
