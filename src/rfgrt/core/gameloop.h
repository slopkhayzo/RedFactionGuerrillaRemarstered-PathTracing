#pragma once

// The game's main-loop frame on the simulation thread (rfg_do_frame): one hook shared by every module that needs
// per-frame work.
namespace gameloop {

bool install();

// Callbacks on the sim thread after each frame, with the game state it ended in. The end of a frame is a safe point to
// call game functions.
void add_end(void (*fn)(int game_state));

unsigned long sim_thread_id();
unsigned long long frames_ended();

}  // namespace gameloop
