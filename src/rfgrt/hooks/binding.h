#pragma once
// A game with seams for the plugin's hooks passes this to rfgrt_set_binding (main.cpp) before InitializeASI: OpenRFG64
// VOM, which builds the plugin as its renderer backend and has a line at the top of each hooked function where a
// handler can be attached. Every hook is then offered to the game first, by the function's name in the game's PDB, and
// only a hook it declines becomes a MinHook detour. Plain C at global scope: the game includes it next to code of its
// own with the plugin's namespace names (hooks, rfg).
struct rfgrt_binding {
    // Attaches `detour` to the game's seam of the function `name`, with `params` its parameters as the names table
    // writes them (game/rfg_symbols_names.generated.h), or null for a D3D11 interface's method by
    // "Interface::Method" (hooks::create_now). `original` receives what the detour calls on, as MinHook's trampoline
    // would. `now`: on at once (create_now); otherwise at enable_all. False: the game has no seam there.
    bool (*attach)(const char* name, const char* params, void* detour, void** original, bool now);
    // Turns on what attach put on the seams without `now` (hooks::enable_all).
    void (*enable_all)();
    // The game's call site of the call a seam passed to the running handler; `return_address` outside of one
    // (hooks::caller).
    const void* (*caller)(const void* return_address);
};
