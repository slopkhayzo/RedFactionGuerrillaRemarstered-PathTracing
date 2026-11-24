#include "hooks/hooks.h"

#include <windows.h>

#include <MinHook.h>

#include "core/log.h"
#include "game/openrfg64.h"
#include "game/rfg.h"
#include "hooks/binding.h"
#include "openrfg64_column.h"

namespace hooks {
namespace {

rfgrt_binding g_binding{};
int g_seamed = 0;

}  // namespace

bool init() {
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        LOG("MinHook init failed: %s", MH_StatusToString(s));
        return false;
    }
    return true;
}

bool create(uint32_t rva, void* detour, void** original, const char* name) {
    if (rva == 0) {  // OpenRFG64-memtest: a name the running build does not have (game/openrfg64.cpp)
        LOG("hook %s: not in this build (no address): not hooked", name);
        return false;
    }
    if (g_binding.attach) {
        const ::openrfg64::Name* symbol = rfg::openrfg64::symbol_of(rva);
        if (symbol && g_binding.attach(symbol->name, symbol->signature, detour, original, false)) {
            ++g_seamed;
            return true;
        }
        LOG("hook %s: not on a seam; a MinHook detour", name);
    }
    MH_STATUS s = MH_CreateHook(rfg::addr(rva), detour, original);
    if (s != MH_OK) {
        LOG("hook %s (RVA %08X) failed: %s", name, rva, MH_StatusToString(s));
        return false;
    }
    return true;
}

bool enable_all() {
    if (g_binding.enable_all) g_binding.enable_all();
    MH_STATUS s = MH_EnableHook(MH_ALL_HOOKS);
    if (s != MH_OK) {
        LOG("enabling hooks failed: %s", MH_StatusToString(s));
        return false;
    }
    return true;
}

void shutdown() { MH_Uninitialize(); }

bool create_now(void* target, void* detour, void** original, const char* name) {
    if (g_binding.attach) {
        if (g_binding.attach(name, nullptr, detour, original, true)) {
            ++g_seamed;
            return true;
        }
        LOG("hook %s: not on a seam; a MinHook detour", name);
    }
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) {
        LOG("hook %s (%p) failed: %s", name, target, MH_StatusToString(s));
        return false;
    }
    return true;
}

void set_binding(const rfgrt_binding& binding) { g_binding = binding; }

bool seam(const char* name, void* detour, void** original) {
    if (!g_binding.attach || !g_binding.attach(name, nullptr, detour, original, false)) {
        LOG("hook %s: the game has no such seam", name);
        return false;
    }
    ++g_seamed;
    return true;
}

const void* caller(const void* return_address) {
    return g_binding.caller ? g_binding.caller(return_address) : return_address;
}

int seamed() { return g_seamed; }

}  // namespace hooks
