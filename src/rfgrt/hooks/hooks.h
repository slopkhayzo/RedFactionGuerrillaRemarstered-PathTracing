#pragma once
#include <cstdint>

namespace hooks {

bool init();
// Creates a MinHook detour on the function at `rva`; `original` receives the trampoline. Enabled by enable_all().
bool create(uint32_t rva, void* detour, void** original, const char* name);
bool enable_all();
void shutdown();
// A detour on a function outside rfg.exe (a COM method, say), created and enabled at once: for hooks that can only be
// placed once the game has made the object.
bool create_now(void* target, void* detour, void** original, const char* name);

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
