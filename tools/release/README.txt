rfgrt @VERSION@ - path tracing for Red Faction Guerrilla Re-Mars-tered
=====================================================================

What it does
------------
The game's lighting is path traced on the graphics card, the way Cyberpunk 2077's RT Overdrive does it: the game draws
its world, materials and effects as it always has, and the light falling on them is traced through the scene.

- Global illumination: the light bounced off everything around a surface, in place of the game's flat ambient
  colour. Shaded walls take on the colour of the sunlit ground in front of them; rooms, alleys and the undersides of
  things darken where the sky cannot reach; and what is lit, the game's lights and glowing materials included, lights
  its surroundings, also once it is out of view (the light of what was seen is kept in the world for half a minute)
  and bounce after bounce.
- Light from the sky comes in through doors and windows in the colour of the sky the game draws, and goes through
  glass and every other see-through material. Windows and glass shards are lit by it too: they take soft shadows
  and the light around them instead of the game's flat lighting. The patterned glass of the multiplayer maps scatters
  the light through it, as frosted glass does: no sunlight passes straight through, the panes glow on the other side
  with the light falling on them, tinted by their colour ([GI] FrostedGlass). Sunlight through the clear windows takes
  the colour of the glass where it passes it, pane by pane and texel by texel, and so does the light bounced in
  through them ([GI] WindowTint).
- Windows reflect: clear glass mirrors the path-traced scene and the sky, some of it straight on and more and more at
  grazing angles, as glass does ([Reflections] GlassReflectance: 0.04 is plain glass, 1 a mirror; 0.2 by default).
- What the rays hit out of view has its own textures and colours: reflections in windows show the buildings, rubble
  and props as they look, and the light they bounce takes their colour ([GI] Textures). The terrain, drawn by a shader
  of its own, shows in reflections as the game last drew it, or in its soil's dark rust where it was not seen
  ([GI] TerrainAlbedo).
- No sun glints in the shade: the game's materials keep a little of the sun's highlight in full shadow, which showed
  as bright edges inside buildings and vehicles once their shade was path-traced ([Shadows] Specular). The patterned
  glass of the multiplayer maps no longer glows in the shade either, and the terrain no longer reflects the open sky
  in caves and under buildings.
- Shadows for every light. The sun's are ray traced instead of drawn from the game's shadow maps: soft, with edges that
  widen with distance from what casts them, sharp where objects touch the ground, and none of the shadow maps' limits
  (no blocky or shimmering edges, no shadows fading out at a distance). The dynamic lights (explosions, fires, flares,
  lamps, the arc welder's arc) cast ray-traced shadows too; in the original most of them shone through walls.
- Volumetric fog: the game's fog, in its colour and density, lit by what reaches it. The sun breaking through makes
  light shafts, the fog in shadows and indoors is darker, and fog both the sun and the sky reach looks as the game's
  ([Fog]). Glass, particles and effects keep the game's own fog, as do the sky and the distant landmarks.
- Reflections: the materials that reflect their surroundings in the game (most buildings, the destructible ones too,
  and the terrain) reflect the path-traced scene instead of a plain sky. As in the game, they show mostly at grazing
  angles.
- Everything the game draws takes part: buildings and every piece Geo-Mod breaks them into, rubble, props, vehicles,
  the terrain, and characters in their animated poses.
- The game's moving cloud shadows stay, on top of the ray-traced ones.
- On NVIDIA RTX graphics cards the bounced light is denoised by NVIDIA DLSS Ray Reconstruction; elsewhere by the
  mod's own filter ([GI] Denoiser).
- The game no longer renders its shadow maps or its screen-space ambient occlusion. The path tracing costs about 7 ms
  of graphics card time a frame at 1920x1080 on an RTX 4090 with Ray Reconstruction, about 5 ms with the mod's own
  filter; around 9 ms in a Wrecking Crew match among glass buildings and debris. Every frame waits for its own path
  tracing ([Host] WaitMs): where it takes most of the graphics card's time, the game runs at its pace rather than show
  a frame with the lighting of the one before.
- The ray tracing runs in a separate 64-bit process, so the 32-bit game keeps its memory.

Requirements
------------
- Red Faction Guerrilla Re-Mars-tered, GOG or Steam version. The mod checks the game executable and stays inactive
  with any other build.
- 64-bit Windows 10 (version 2004 or newer) or 11.
- A graphics card with DirectX ray tracing (DXR 1.1): NVIDIA RTX 20 series or newer, AMD Radeon RX 6000 series or
  newer, Intel Arc. Without one the game runs as it ships.
- With rfgphys (the PhysX mod): version 0.5.0 or newer. Older versions take 128 MB more of the game's memory (see
  Known limitations), and with the Steam version the game then crashed already in the main menu in testing.

Install
-------
The mod comes in two archives:
    rfgrt-@VERSION@-with-asi-loader.zip   everything, including Ultimate ASI Loader 9.7.4 (as dinput8.dll)
    rfgrt-@VERSION@-plugin-only.zip       the same without the loader, for a game that already has one
The mod needs Ultimate ASI Loader, any version from 2018 on, loading .asi files from the scripts folder.

If the game folder already has a dinput8.dll, do not overwrite it: Sledge, the game's mod manager, uses that name
too. Use the plugin-only archive then, with Ultimate ASI Loader installed under another of its names (winmm.dll, for
one), unless another mod already put it there that way.

Extract the archive into the game folder, the one that contains rfg.exe, and let it merge the folders. In Steam:
right-click the game, Manage, Browse local files; in GOG Galaxy: the game's settings, Manage installation, Show
folder.
It adds:
    dinput8.dll                      Ultimate ASI Loader 9.7.4 (only in the archive with the loader)
    scripts\rfgrt.asi                the mod
    scripts\rfgrt.ini                its settings
    scripts\rfgrt\rfgrt_host.exe     the ray tracing process; keep it next to rfgrt.asi
    scripts\rfgrt\nvngx_dlssd.dll    NVIDIA DLSS Ray Reconstruction, used on NVIDIA RTX graphics cards
    CrashDumps\                      crash reports (see below)

Settings
--------
scripts\rfgrt.ini, explained in the file. Enabled=0 turns the mod off without uninstalling it. Each effect has a
section of its own and can be turned off there. [GI] Sky scales the sky's light (0 leaves only the sun, the lights and
glowing surfaces, bounced around), [GI] Bounce the bounced light, [GI] SkyDrawn the sky's light (1 the colour of the
sky as drawn at the brightness the game gives its shade, 2 the drawn sky as it is, about four times darker, 0 the
game's ambient colour), [GI] Denoiser the denoiser of the bounced light (2 NVIDIA DLSS Ray Reconstruction, 1 the
mod's own filter, faster).

Uninstall
---------
Delete scripts\rfgrt.asi, scripts\rfgrt.ini, scripts\rfgrt\, scripts\rfgrt_runs\ and CrashDumps\, and dinput8.dll
unless another mod uses it. The mod writes nothing into your saves.

Logs and crash reports
----------------------
Every game session writes scripts\rfgrt_runs\<date-time>-session\rfgrt.log and rfgrt_host.log; the newest 10
sessions are kept. While the CrashDumps folder exists, Ultimate ASI Loader writes a crash dump there (about 60 MB
each) if the game crashes; delete the folder to turn that off. When reporting a problem, include the session folder
and, after a crash, the files from CrashDumps.

A shadow that should not be there, or one that is missing: put the crosshair on the spot and press F10. The session's
rfgrt_host.log then names what the ray tracing meets there: the object that shadows that spot, and whether it sees
what the game drew there (the key is [Test] ProbeKey in rfgrt.ini). F11 switches between the path tracing and the
game's own lighting and shadows on the spot, to compare ([Test] VanillaKey).

Known limitations
-----------------
- The bounced light takes the colours the game showed. A surface not seen yet lights its surroundings as a plain grey
  surface would, under the sun, the sky it sees and one more bounce; a light or a glow not seen yet adds none.
- The sky's colour is learnt from the parts of it on screen: until some of it has been seen, the game's own ambient
  colour stands in ([GI] SkyDrawn=0 keeps that colour).
- The game is 32-bit: it and every mod in it share 4 GB of address space. With rfgphys (the PhysX mod, which takes
  about 430 MB of it with its default settings; the Steam version itself takes about 40 MB more than the GOG version)
  a long Wrecking Crew match full of destruction can use it up; the game then stops with a fatal error (possibly
  hidden behind its window). Objects flickering in and out are the sign that it is nearly used up: restart the game
  then. This mod logs what is left every minute.
- Metals and the EDF's vehicles have no reflections in the game's materials, and get none.
- The game adapts its exposure to the frame's brightness. With the bounced light in, the sunlit parts of daylight
  scenes come out a little darker than in the original ([GI] Bounce lowers it).
- Up to 16 dynamic lights at a time get shadows, the nearest first.
- The ray tracing holds up to 65,536 objects. A Wrecking Crew map levelled with rfgphys keeping all its debris can
  have more: the farthest then cast no shadow and get no bounced light from the ray tracing.
- With the sun low, the terrain's ledges and ridges cast sharp shadows on the slopes below them, as dark bands (F11
  compares with the game's own shadows on the spot).
- Surfaces the game cuts out with a texture (chain-link fences, gratings, foliage) cast solid shadows.
- Some meshes are not ray traced yet and cast no shadow: the game's clone meshes (much of the vegetation and some
  props), the low-detail far terrain and landmarks, particles and decals.
- If the ray tracing process stops, the game goes back to its own lighting and shadows for the rest of the session.

License
-------
rfgrt is released under the MIT License (rfgrt_licenses\rfgrt-MIT.txt).

Third-party components
----------------------
- Ultimate ASI Loader (https://github.com/ThirteenAG/Ultimate-ASI-Loader), MIT License (in the archive with the
  loader)
- MinHook (https://github.com/TsudaKageyu/minhook), BSD 2-Clause License
- NVIDIA DLSS Ray Reconstruction (nvngx_dlssd.dll, from https://github.com/NVIDIA/DLSS), NVIDIA RTX SDKs License.
  NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation.
The license texts are in rfgrt_licenses\.

Misc
----
For more game fixes, visit: https://slop-blog.enkhayzomachines.net/fixes :)
