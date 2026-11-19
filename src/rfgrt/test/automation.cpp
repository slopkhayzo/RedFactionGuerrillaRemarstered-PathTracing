#include "test/automation.h"

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "harness/screenshot.h"
#include "hooks/hooks.h"
#include "test/input_script.h"

namespace automation {
namespace {

constexpr int GS_MAINMENU = 0, GS_GAMEPLAY = 1, GS_PLAYER_DEATH_OPTIONS = 16, GS_WRECKING_CREW_MAIN_MENU = 19,
              GS_INGAME_HANDBOOK_HUD = 43, GS_SAVE_LOAD_SCREEN = 50;
constexpr int kMainMenuLoadGame = 1;               // main_menu_handle_selection's argument
constexpr int kMainMenuWreckingCrew = 2;           // (the PhysX project's replays start their matches so)
constexpr int kWcMenuLoaded = 1;                   // wc_shell_info::state MMS_WC_LOADED: the menu's data is in
constexpr int kClassKeyboard = 1, kClassMouse = 2;  // keen::ControllerClass
constexpr uint32_t kMaxControllers = 16;
constexpr uint32_t kHumanInvulnerable = 0x20;  // human_flags: the bit human::is_invulnerable reads

Config g_cfg;
int g_last_state = -1000;
int64_t g_state_since = 0;
bool g_pressed_load = false, g_selected_save = false, g_refreshed_list = false, g_caught_input = false;
size_t g_next_shot = 0;

// The main menu waits for "any button" to pick player 1's input device. Assigning the keyboard and mouse to player 0
// is what a caught key press does (keen::input::setControllerPlayerId).
void catch_keyboard_and_mouse() {
    auto* system = rfg::global<uint8_t*>(rfg::rva::Keen_input_system);
    if (!system) return;
    auto set_player = rfg::fn<void(__cdecl*)(void*, uint32_t, void*)>(rfg::rva::keen__input__setControllerPlayerId);
    for (uint32_t i = 0; i < kMaxControllers; ++i) {
        uint8_t* info = system + rfg::off::InputSystem_controllerInfos + i * rfg::off::ControllerInfo_size;
        const int cls = rfg::field<int32_t>(info, rfg::off::ControllerInfo_controllerClass);
        if (cls == kClassKeyboard || cls == kClassMouse) {
            set_player(system, i, nullptr /* player 0 */);
            LOG("automation: input device %u (%s) assigned to player 1", i, cls == kClassKeyboard ? "keyboard" : "mouse");
        }
    }
}

// Profile, options and the save list are loaded (only then does Load Game list anything).
bool main_menu_ready() {
    return rfg::global<uint8_t>(rfg::rva::Main_menu_auto_catch_done) &&
           !rfg::global<uint8_t>(rfg::rva::Main_menu_loading_options) &&
           !rfg::global<uint8_t>(rfg::rva::Main_menu_waiting_for_options_load);
}

// Unattended runs start the game from a hidden job, often without the foreground: the game would quick-pause
// (GS_QUICK_PAUSE) at once. os_foreground() says it is in front, and Os_app_active stays set, as the PhysX project's
// replay harness does.
bool(__cdecl* o_os_foreground)();
bool __cdecl d_os_foreground() {
    rfg::global<uint8_t>(rfg::rva::Os_app_active) = 1;
    return true;
}

// __thiscall SaveDataProviderWin32Steam::writeFileToSteamStorage(const char* name, void* data, uint size): every save
// write and erase goes through it; returning OperationResult_Ok (0) without writing keeps the game happy.
int(__thiscall* o_write_file)(void*, const char*, void*, uint32_t);
int __fastcall d_write_file(RFG_THIS(void*, self), const char* name, void*, uint32_t size) {
    (void)self;
    LOG("save write BLOCKED (test mode): %s, %u bytes", name ? name : "?", size);
    return 0;
}

// Test.WreckingCrew: the map's file name (wrecking_crew_get_map_file_name; null past the list).
const char* wc_map_file(int map) {
    __try {
        return rfg::fn<const char*(__cdecl*)(int)>(rfg::rva::wrecking_crew_get_map_file_name)(map);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::string file_stem(const char* path) {
    std::string s = path ? path : "";
    if (const size_t slash = s.find_last_of("\\/"); slash != std::string::npos) s = s.substr(slash + 1);
    if (const size_t dot = s.find_last_of('.'); dot != std::string::npos) s.resize(dot);
    return s;
}

// Main menu -> Wrecking Crew; once its menu has its data, the asked map in its options and the match begun (the
// players' setup, mode and weapons as the menu has them).
bool g_pressed_wc = false, g_wc_started = false;
int64_t g_wc_loaded_since = 0;

void drive_wrecking_crew(int state, double in_state_ms) {
    if (state == GS_MAINMENU && !g_pressed_wc && in_state_ms > 2000) {
        if (!g_caught_input && !rfg::global<uint8_t>(rfg::rva::Main_menu_auto_catch_done)) {
            g_caught_input = true;
            catch_keyboard_and_mouse();
        }
        static int64_t ready_since = 0;
        if (main_menu_ready()) {
            if (!ready_since) ready_since = session::qpc();
        } else {
            ready_since = 0;
        }
        const bool settled = ready_since && session::ticks_to_ms(session::qpc() - ready_since) > 1500;
        if (settled || in_state_ms > 45000) {
            g_pressed_wc = true;
            LOG("automation: main menu %s after %.1f s -> Wrecking Crew", settled ? "ready" : "NOT ready (timeout)",
                in_state_ms / 1000.0);
            rfg::fn<void(__cdecl*)(int)>(rfg::rva::main_menu_handle_selection)(kMainMenuWreckingCrew);
        }
        return;
    }
    if (state != GS_WRECKING_CREW_MAIN_MENU || g_wc_started) return;
    const int menu = rfg::field<int32_t>(rfg::addr(rfg::rva::Menu_wrecking_crew_info), rfg::off::WcShellInfo_state);
    if (menu != kWcMenuLoaded) {
        g_wc_loaded_since = 0;
        if (in_state_ms > 60000) session::exit_now("automation: the Wrecking Crew menu never loaded");
        return;
    }
    if (!g_wc_loaded_since) g_wc_loaded_since = session::qpc();
    if (session::ticks_to_ms(session::qpc() - g_wc_loaded_since) < 1000) return;
    g_wc_started = true;
    void* options = rfg::addr(rfg::rva::Wrecking_crew_options);
    int32_t& map = rfg::field<int32_t>(options, rfg::off::WcOptions_map_index);
    const int maps = rfg::global<int32_t>(rfg::rva::Wrecking_crew_num_maps);
    std::string list;
    int wanted = -1;
    for (int i = 0; i < maps && i < 64; ++i) {
        const std::string name = file_stem(wc_map_file(i));
        list += (i ? ", " : "") + std::to_string(i) + " " + name;
        if (_stricmp(name.c_str(), g_cfg.wrecking_crew.c_str()) == 0) wanted = i;
    }
    LOG("automation: Wrecking Crew's maps: %s", list.c_str());
    if (wanted < 0 && !g_cfg.wrecking_crew.empty() && isdigit(uint8_t(g_cfg.wrecking_crew[0])))
        wanted = atoi(g_cfg.wrecking_crew.c_str());
    if (wanted >= 0 && wanted < maps) {
        map = wanted;
    } else if (_stricmp(g_cfg.wrecking_crew.c_str(), "current") != 0 || map < 0 || map >= maps) {
        LOG("automation: no Wrecking Crew map \"%s\" (the menu's is %d): map 0", g_cfg.wrecking_crew.c_str(), map);
        map = 0;
    }
    LOG("automation: the Wrecking Crew match begins: mode %d, map %d (%s)",
        rfg::field<int32_t>(options, rfg::off::WcOptions_mode), map, file_stem(wc_map_file(map)).c_str());
    rfg::fn<void(__cdecl*)()>(rfg::rva::wrecking_crew_match_begin)();
}

void on_frame(int state) {
    if (state != g_last_state) {
        // a Wrecking Crew match loading (started by anyone: the player, Test.WreckingCrew, a replay): its map, for the log
        if (g_last_state == GS_WRECKING_CREW_MAIN_MENU && state == 2 /* GS_LOAD */) {
            void* options = rfg::addr(rfg::rva::Wrecking_crew_options);
            const int map = rfg::field<int32_t>(options, rfg::off::WcOptions_map_index);
            LOG("automation: a Wrecking Crew match loads: mode %d, map %d (%s)", rfg::field<int32_t>(options, rfg::off::WcOptions_mode),
                map, map >= 0 && map < rfg::global<int32_t>(rfg::rva::Wrecking_crew_num_maps) ? file_stem(wc_map_file(map)).c_str() : "?");
        }
        g_last_state = state;
        g_state_since = session::qpc();
    }
    const double in_state_ms = session::ticks_to_ms(session::qpc() - g_state_since);

    if (!g_cfg.wrecking_crew.empty()) {
        drive_wrecking_crew(state, in_state_ms);
    } else if (g_cfg.auto_load_save) {
        if (state == GS_MAINMENU && !g_caught_input && in_state_ms > 2000 &&
            !rfg::global<uint8_t>(rfg::rva::Main_menu_auto_catch_done)) {
            g_caught_input = true;
            catch_keyboard_and_mouse();
        }
        static int64_t ready_since = 0;
        if (state == GS_MAINMENU && !g_pressed_load && in_state_ms > 2000) {
            if (main_menu_ready()) {
                if (!ready_since) ready_since = session::qpc();
            } else {
                ready_since = 0;
            }
            const bool settled = ready_since && session::ticks_to_ms(session::qpc() - ready_since) > 1500;
            if (settled || in_state_ms > 45000) {
                g_pressed_load = true;
                LOG("automation: main menu %s after %.1f s (cache loaded %d) -> Load Game",
                    settled ? "ready" : "NOT ready (timeout)", in_state_ms / 1000.0,
                    rfg::global<uint8_t>(rfg::rva::Directory_cache_loaded));
                rfg::fn<void(__cdecl*)(int)>(rfg::rva::main_menu_handle_selection)(kMainMenuLoadGame);
            }
        }
        if (state == GS_SAVE_LOAD_SCREEN && !g_selected_save && in_state_ms > 1500) {
            const int saves = rfg::global<int32_t>(rfg::rva::Num_saved_games);
            if (saves > 0) {
                g_selected_save = true;
                LOG("automation: load screen lists %d saves -> loading the selected (most recent) one", saves);
                rfg::fn<void(__cdecl*)(bool)>(rfg::rva::hud_ui_save_load_selection_made)(false);
            } else if (!g_refreshed_list && in_state_ms > 5000) {
                g_refreshed_list = true;
                LOG("automation: load screen still empty -> refreshing the save directory cache");
                rfg::fn<void(__cdecl*)()>(rfg::rva::game_save_clear_load_directory_cache)();
                rfg::global<uint8_t>(rfg::rva::Update_list_uisave) = 1;
            } else if (in_state_ms > 20000) {
                session::exit_now("automation: no saves listed on the load screen");
            }
        }
        // A save can open a handbook (tutorial) pane a little after loading, which waits for a key: closed as its back
        // button does.
        static bool closing = false;
        if (state != GS_INGAME_HANDBOOK_HUD) {
            closing = false;
        } else if (!closing && in_state_ms > 3000) {
            closing = true;
            rfg::global<uint8_t>(rfg::rva::ExitHandbook) = 1;
            LOG("automation: a handbook pane waits for a key -> closed");
        }
    }
    if (g_cfg.god_mode && state == GS_GAMEPLAY) {
        if (uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(rfg::rva::player_get_local_player)()) {
            uint32_t& flags = rfg::field<uint32_t>(player, rfg::off::human_hflags);
            if (!(flags & kHumanInvulnerable)) {
                flags |= kHumanInvulnerable;
                static bool logged = false;
                if (!logged) LOG("automation: god mode, the player takes no damage");
                logged = true;
            }
        }
    }
    const double gameplay_s = session::gameplay_seconds();
    // Test.TimeOfDay: the clock set once gameplay runs, then held (the time of day's lighting follows it each frame)
    static bool clock_set = false;
    if (g_cfg.time_of_day >= 0 && !clock_set && state == GS_GAMEPLAY && gameplay_s >= 0.2) {
        clock_set = true;
        const uint8_t hour = uint8_t(g_cfg.time_of_day % 24);
        rfg::fn<void(__cdecl*)(uint32_t, uint32_t, uint32_t)>(rfg::rva::game_clock_set_time)(hour, 0, 0);
        rfg::fn<void(__cdecl*)(uint32_t)>(rfg::rva::game_clock_freeze_ingame_time)(1);
        LOG("automation: the game clock set to %02u:00 and held there", static_cast<unsigned>(hour));
    }
    if (g_next_shot < g_cfg.shots_at.size() && state == GS_GAMEPLAY && gameplay_s >= g_cfg.shots_at[g_next_shot]) {
        wchar_t name[64];
        swprintf(name, 64, L"\\shot_%03d.png", int(g_cfg.shots_at[g_next_shot] + 0.5));
        screenshot::request(session::run_dir() + name);
        ++g_next_shot;
    }
    if (g_cfg.exit_after_gameplay_s > 0 && state == GS_PLAYER_DEATH_OPTIONS && in_state_ms > 5000)
        session::exit_now("automation: the player died (unattended run)");
    if (g_cfg.exit_after_gameplay_s > 0 && state == GS_GAMEPLAY && gameplay_s >= g_cfg.exit_after_gameplay_s)
        session::exit_now("ExitAfterGameplaySeconds elapsed");
}

}  // namespace

bool install(const Config& cfg) {
    g_cfg = cfg;
    bool ok = true;
    if (cfg.block_save_writes) {
        ok &= hooks::create_this(rfg::rva::keen__SaveData__SaveDataProviderWin32Steam__writeFileToSteamStorage,
                                 &d_write_file, &o_write_file, "SaveDataProviderWin32Steam::writeFileToSteamStorage");
        LOG("automation: save writes and erases are blocked for this session");
    }
    if (cfg.ignore_focus) {
        ok &= hooks::create(rfg::rva::os_foreground, &d_os_foreground, &o_os_foreground, "os_foreground");
        LOG("automation: the game keeps running without the foreground");
    }
    if (!cfg.shots_at.empty()) ok &= screenshot::install();
    if (!cfg.input_script.empty()) ok &= input_script::install(cfg.input_script);
    session::add_frame_callback(&on_frame);
    return ok;
}

}  // namespace automation
