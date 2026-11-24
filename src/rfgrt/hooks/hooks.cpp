#include "hooks/hooks.h"

#include <windows.h>

#include <MinHook.h>

#include "core/log.h"
#include "game/rfg.h"

namespace hooks {

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
    MH_STATUS s = MH_CreateHook(rfg::addr(rva), detour, original);
    if (s != MH_OK) {
        LOG("hook %s (RVA %08X) failed: %s", name, rva, MH_StatusToString(s));
        return false;
    }
    return true;
}

bool enable_all() {
    MH_STATUS s = MH_EnableHook(MH_ALL_HOOKS);
    if (s != MH_OK) {
        LOG("enabling hooks failed: %s", MH_StatusToString(s));
        return false;
    }
    return true;
}

void shutdown() { MH_Uninitialize(); }

bool create_now(void* target, void* detour, void** original, const char* name) {
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) {
        LOG("hook %s (%p) failed: %s", name, target, MH_StatusToString(s));
        return false;
    }
    return true;
}

}  // namespace hooks
