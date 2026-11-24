#pragma once
#include <cstdint>

struct rfgrt_binding;  // hooks/binding.h

namespace hooks {

bool init();
// Creates a MinHook detour on the function at `rva`; `original` receives the trampoline. Enabled by enable_all().
// With a binding (a game with seams), the hook goes on the function's seam instead, when the game has one.
bool create(uint32_t rva, void* detour, void** original, const char* name);
bool enable_all();
void shutdown();
// A detour on a function outside rfg.exe (a COM method, say), created and enabled at once: for hooks that can only be
// placed once the game has made the object. With a binding, on the game's seam of that call by `name` when it has one.
bool create_now(void* target, void* detour, void** original, const char* name);

// The game's seams (hooks/binding.h): set before the first hook is created.
void set_binding(const rfgrt_binding& binding);
// A hook on a seam the game names, for what only a game with seams has (no function of rfg.exe's behind it): on at
// enable_all. False without a binding, or when the game has no seam of that name.
bool seam(const char* name, void* detour, void** original);
// Where the game called the running handler from: `return_address` (the handler's _ReturnAddress()) for a detour; for a
// handler on a seam, which the seam itself calls, the seam's caller.
const void* caller(const void* return_address);
// How many hooks are on the game's seams.
int seamed();

template <typename Fn>
bool create(uint32_t rva, Fn detour, Fn* original, const char* name) {
    return create(rva, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), name);
}
// Detour and trampoline types differ for __thiscall functions on x86 (see RFG_THIS in game/rfg.h).
template <typename Detour, typename Original>
bool create_this(uint32_t rva, Detour detour, Original* original, const char* name) {
    return create(rva, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), name);
}

}  // namespace hooks
