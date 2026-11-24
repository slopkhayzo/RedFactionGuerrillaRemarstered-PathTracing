#include "game/rfg.h"

#include <windows.h>

namespace rfg {
namespace {

int g_build = -1;

}  // namespace

uintptr_t base() {
    static const uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return b;
}

int build() { return g_build; }

const char* build_name() {
    return g_build >= kBuildCount ? "OpenRFG64" : g_build >= 0 ? kBuilds[g_build].name : "unknown";
}

void select_build(int index) {
    // OpenRFG64-memtest: index kBuildCount is OpenRFG64, whose values game/openrfg64.cpp has filled from its PDB.
    if (index < kBuildCount)
        for (const RvaEntry& e : kRvaTable) *e.value = e.in_build[index];
    g_build = index;
}

const char* game_state_name(int value) {
    for (const auto& s : game_state_names)
        if (s.value == value) return s.name;
    return "?";
}

}  // namespace rfg
