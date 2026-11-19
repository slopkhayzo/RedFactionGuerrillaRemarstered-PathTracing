#pragma once
#include <cstdint>

#include "game/rfg_symbols.generated.h"

// Access to rfg.exe code and data. The exe is ASLR-enabled, so code and globals are relative to its load address
// (rfg::rva, by name), and struct members are read at their offsets in the running build (rfg::off, by name). Both
// tables hold the running build's values once select_build() has run, at start-up before anything uses them; code
// reads them when it runs, never into constants. Nothing here assumes a 32-bit build: the 64-bit port's rfg.exe
// will come with its own table.
namespace rfg {

uintptr_t base();

// The build running (index into kBuilds), -1 until select_build().
int build();
const char* build_name();
// Switches every value in rfg::rva and rfg::off to that build's.
void select_build(int index);

inline void* addr(uint32_t rva) { return reinterpret_cast<void*>(base() + rva); }

template <typename T>
inline T& global(uint32_t rva) {
    return *reinterpret_cast<T*>(base() + rva);
}

template <typename Fn>
inline Fn fn(uint32_t rva) {
    return reinterpret_cast<Fn>(base() + rva);
}

// A struct member at its offset in the running build: field<T>(object, rfg::off::Type_member).
template <typename T>
inline T& field(void* object, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset);
}

const char* game_state_name(int value);

inline int gameseq_get_state() { return fn<int(__cdecl*)()>(rva::gameseq_get_state)(); }

}  // namespace rfg

// Hooking __thiscall functions: on x86 a detour is declared __fastcall with an unused second register parameter
// (EDX), which lines the rest up with the stack arguments; on x64 there is one calling convention and no such
// parameter. Trampolines (the originals) are called through __thiscall pointer types, which x64 ignores too.
//   int __fastcall d_fn(RFG_THIS(void*, self), int a) { return o_fn(self, a); }   o_fn: int(__thiscall*)(void*, int)
#if defined(_WIN64)
#define RFG_THIS(type, name) type name
#else
#define RFG_THIS(type, name) type name, void* /*edx*/
#endif
