#pragma once
// Path tracing on or off while the game runs, for a game that switches its renderer (OpenRFG64 VOM's RENDERER row,
// through rfgrt_set_active: main.cpp). Off, the game renders as it ships: its original shaders bound, its own shadows
// and ambient occlusion, the render hooks straight through to the game's functions; the core is stopped and its memory
// released. The scene mirror keeps a copy of every live mesh meanwhile, so that a core started later gets them all
// (mirror/geometry.h), and the patched shaders are made beside the originals (harness/shader_dump.h). The plugin under
// an ASI loader is on from its start and stays on.
namespace active {

// The game switches the renderer: it said so before the plugin started.
bool switchable();
// Path tracing draws: the render hooks do their work (else they call the game's function and nothing else).
bool on();

// main.cpp: before the plugin starts, the game switches and path tracing starts on or off; afterwards, a switch.
void set_switchable(bool initially_on);
void set_on(bool on);

// A render hook's work with the core's frames or textures, counted while it runs so that a switch off waits for it
// before the core and its textures go (wait_idle). The count goes up before on() is read: work that saw path tracing on
// is always waited for.
struct Work {
    bool on;
    Work();
    ~Work();
    Work(const Work&) = delete;
    Work& operator=(const Work&) = delete;
};
// After set_on(false): waits up to `ms` for the work begun while path tracing was on. False when some is still under way.
bool wait_idle(unsigned ms);

}  // namespace active
