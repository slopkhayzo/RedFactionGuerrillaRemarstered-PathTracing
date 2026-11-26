#pragma once
#include <string>

// The display menu's rows for the plugin's settings, hooked into the game's unmodified menu (OpenRFG64 VOM has rows of
// its own in its source: there the plugin adds none). After the game's rows on the general page:
//   RENDERER          PATH TRACED or STANDARD (the game as it ships): rfgrt.ini's Host.Enabled, from the next start,
//                     which its caption says; changed from the main menu only (greyed out in game). VOM's row switches
//                     at once (core/active.h), which takes its seams: the scene mirror keeping a copy of every mesh (too
//                     much for the 32-bit game's memory), and the patched pixel shaders made beside the originals, the
//                     other version released with the game's, through VOM's seam in FragmentShader::destroy, which
//                     rfg.exe inlines into each of its callers.
//   DLSS              the path tracer's DLSS mode (off, DLAA, quality, balanced, performance, ultra performance): DLSS.Mode.
//                     With path tracing running and Ray Reconstruction (GI.Denoiser=2) only; in game too, at once.
//   FRAME GENERATION  DLSS Frame Generation: FrameGeneration.Enabled; in game too, at once, with either renderer. 64-bit:
//                     the presenter's detour is then placed even when it starts off. The 32-bit plugin has no presenter:
//                     greyed out.
// Accept applies them and writes them to rfgrt.ini; back asks before dropping changes, as for the game's own rows (the
// game's changes_made). The captions and values go into the game's localization table, whose data has none of them.
struct Config;

namespace display_rows {

// What the rows change at once (main.cpp's exports).
struct Actions {
    void (*set_dlss_mode)(int mode);
    void (*set_frame_generation)(int on);
};

// The hooks, for a game without rows of its own (Menu.Rows; main.cpp leaves VOM out). `ini`: the rfgrt.ini Accept
// writes to. `frame_generation`: the FRAME GENERATION row can switch (the 64-bit presenter's detour placed). False when
// the game lacks a name the rows need.
bool install(const Config& cfg, const std::wstring& ini, bool frame_generation, const Actions& actions);

// Test.DisplayRows (test/automation.cpp): the general page has the rows (made since the display menu opened).
bool rows_shown();
// Test.DisplayRows: the rows set as a player would (-1: unchanged): RENDERER (0 STANDARD, 1 PATH TRACED), DLSS's mode,
// FRAME GENERATION (0 off, 1 on). Accept (the game's display_hint_callback) then applies them.
void test_choose(int renderer, int dlss, int frame_generation);

}  // namespace display_rows
