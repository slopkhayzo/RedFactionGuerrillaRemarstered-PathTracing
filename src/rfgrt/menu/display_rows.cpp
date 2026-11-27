#include "menu/display_rows.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>

#include "core/config.h"
#include "core/log.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace display_rows {
namespace {

// The rows' captions and values: keys of the game's localization table (hashed as keen::getCrc32LwrValue does) and
// our texts, which the table points to (hence writable arrays that live as long as the game).
struct Label {
    const char* key;
    wchar_t text[24];
};
Label g_labels[] = {
    {"RFGRT_RENDERER_NEXT_START", L"RENDERER (NEXT START)"},
    {"RFGRT_RENDERER_STANDARD", L"STANDARD"},
    {"RFGRT_RENDERER_PATH_TRACED", L"PATH TRACED"},
    {"RFGRT_DLSS", L"DLSS"},
    {"RFGRT_DLSS_OFF", L"OFF"},
    {"RFGRT_DLSS_DLAA", L"DLAA"},
    {"RFGRT_DLSS_QUALITY", L"QUALITY"},
    {"RFGRT_DLSS_BALANCED", L"BALANCED"},
    {"RFGRT_DLSS_PERFORMANCE", L"PERFORMANCE"},
    {"RFGRT_DLSS_ULTRA_PERFORMANCE", L"ULTRA PERFORMANCE"},
    {"RFGRT_FRAME_GENERATION", L"FRAME GENERATION"},
};
const char* kRendererNames[2] = {"RFGRT_RENDERER_STANDARD", "RFGRT_RENDERER_PATH_TRACED"};
constexpr int kDlssModes = 6;  // rfgrt_ipc::Block::dlss_mode: 0 off, 1 DLAA, 2 quality, 3 balanced, 4 performance, 5 ultra performance
const char* kDlssNames[kDlssModes] = {"RFGRT_DLSS_OFF",         "RFGRT_DLSS_DLAA",        "RFGRT_DLSS_QUALITY",
                                      "RFGRT_DLSS_BALANCED",    "RFGRT_DLSS_PERFORMANCE", "RFGRT_DLSS_ULTRA_PERFORMANCE"};

using ItemCallback = void(__cdecl*)(int, void*);
using PageFn = void(__cdecl*)(int);
using EnterFn = void(__cdecl*)(int);
using HintFn = void(__cdecl*)(int);
using ChangesMadeFn = bool(__cdecl*)();
using AddListFn = int(__cdecl*)(int, const char*, const char**, int, ItemCallback, int, bool);
using AddBoolFn = int(__cdecl*)(int, const char*, bool*, ItemCallback, bool);
using EnableFn = void(__cdecl*)(int, int, bool);
using CountFn = int(__cdecl*)(int);
using HashGetFn = bool(__thiscall*)(void*, uint32_t, wchar_t**);
using HashInsertFn = bool(__thiscall*)(void*, uint32_t, wchar_t* const*);

PageFn o_set_detail_page = nullptr;
EnterFn o_enter = nullptr;
HintFn o_hint = nullptr;
ChangesMadeFn o_changes_made = nullptr;

// The settings as they are, and as the rows have them since the display menu opened.
struct Values {
    int renderer = 1;  // what the next start runs: 1 PATH TRACED, 0 STANDARD
    int dlss = 1;
    bool frame_generation = false;
};
Values g_current, g_pending;
bool g_pending_fg = false;  // the FRAME GENERATION row's variable: the menu writes it itself

std::wstring g_ini;
Actions g_actions{};
bool g_path_traced_now = false;  // path tracing runs in this session (the DLSS row's condition)
bool g_dlss = false;             // Ray Reconstruction (GI.Denoiser=2): DLSS's modes mean something
bool g_fg = false;               // FRAME GENERATION switches (the 64-bit presenter's detour placed)
int g_state_idle = -1;           // GS_MAINMENU_IDLE: with it (or no main menu document) the menu was opened in game
int g_hint_selection = 0;        // HT_SELECTION: the menu's accept
int g_page_general = 0;          // DETAIL_OPTIONS_PAGE_GENERAL
int g_row_renderer = -1, g_row_dlss = -1, g_row_fg = -1;
bool g_shown = false;
bool g_told = false;

int enum_value(const char* name, int fallback) {
    for (const auto& e : rfg::game_state_names)
        if (!strcmp(e.name, name)) return e.value;
    for (const auto& e : rfg::hint_type_names)
        if (!strcmp(e.name, name)) return e.value;
    for (const auto& e : rfg::detail_option_pages_names)
        if (!strcmp(e.name, name)) return e.value;
    return fallback;
}

// keen::getCrc32LwrValue: CRC-32 (reflected 0xEDB88320, initial value and final xor all ones), 'A'..'Z' folded to lower
// case, characters sign-extended.
uint32_t crc32_lower(const char* s) {
    static const struct Table {
        uint32_t v[256];
        Table() {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int bit = 0; bit < 8; ++bit) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
                v[i] = c;
            }
        }
    } table;
    uint32_t crc = 0xFFFFFFFFu;
    for (; *s; ++s) {
        char ch = *s;
        if (uint8_t(ch - 'A') < 26) ch = char(ch + ' ');
        crc = (crc >> 8) ^ table.v[(uint32_t(int32_t(ch)) ^ crc) & 0xFF];
    }
    return ~crc;
}

// Our captions and values into the localization table, those it lacks (its data has none; a language reload clears
// the table, hence at every general page).
void add_labels() {
    void* table = rfg::addr(rfg::rva::Localization_hash);
    auto get = rfg::fn<HashGetFn>(rfg::rva::LocalizationHash_get);
    auto insert = rfg::fn<HashInsertFn>(rfg::rva::LocalizationHash_insert);
    int added = 0, failed = 0;
    for (Label& label : g_labels) {
        const uint32_t key = crc32_lower(label.key);
        wchar_t* existing = nullptr;
        get(table, key, &existing);
        if (existing) continue;
        wchar_t* text = label.text;
        if (insert(table, key, &text))
            ++added;
        else
            ++failed;
    }
    if (failed) LOG("display rows: %d of our captions could not go into the localization table (their rows show no text)", failed);
    if (added && !g_told) LOG("display rows: %d captions put into the localization table", added);
}

// The display menu opened from the main menu (no level loaded), as the game's own screen rows decide.
bool at_main_menu() {
    return rfg::gameseq_get_state() != g_state_idle && rfg::global<int>(rfg::rva::Doc_handle_main) != -1;
}

bool dlss_row_on() { return g_dlss && g_path_traced_now; }

// All three from the main menu only (greyed out in game).
void update_rows() {
    const int menu = rfg::global<int>(rfg::rva::Menu_handle_doptions);
    auto enable = rfg::fn<EnableFn>(rfg::rva::ui_menu_enable_menu_item);
    const bool main_menu = at_main_menu();
    if (g_row_renderer >= 0) enable(menu, g_row_renderer, main_menu);
    if (g_row_dlss >= 0) enable(menu, g_row_dlss, main_menu && dlss_row_on());
    if (g_row_fg >= 0) enable(menu, g_row_fg, main_menu && g_fg);
}

void __cdecl on_renderer(int, void* value) { g_pending.renderer = int(intptr_t(value)) == 1 ? 1 : 0; }

void __cdecl on_dlss(int, void* value) {
    const int mode = int(intptr_t(value));
    g_pending.dlss = mode < 0 ? 0 : mode >= kDlssModes ? kDlssModes - 1 : mode;
}

void __cdecl on_frame_generation(int, void*) { g_pending.frame_generation = g_pending_fg; }

bool changed() {
    return g_pending.renderer != g_current.renderer || g_pending.dlss != g_current.dlss ||
           g_pending.frame_generation != g_current.frame_generation;
}

// rfgrt.ini's key, for the next start. The environment's RFGRT_<SECTION>_<KEY> wins over it then (the log says so).
void save(const wchar_t* section, const wchar_t* key, int value) {
    wchar_t text[16], env[96];
    swprintf(text, 16, L"%d", value);
    swprintf(env, 96, L"RFGRT_%ls_%ls", section, key);
    for (wchar_t* c = env; *c; ++c) *c = wchar_t(towupper(*c));
    wchar_t set[8];
    if (!WritePrivateProfileStringW(section, key, text, g_ini.c_str()))
        LOG("display rows: rfgrt.ini could not be written (%ls.%ls=%ls, error %lu)", section, key, text, GetLastError());
    else if (GetEnvironmentVariableW(env, set, 8) > 0)
        LOG("display rows: %ls.%ls=%ls written to rfgrt.ini (the environment's %ls wins over it at the next start)", section, key, text, env);
}

// Accept: the rows' changes applied and written to rfgrt.ini (at the main menu; in game the rows are greyed out).
void apply() {
    if (!changed()) return;
    const Values before = g_current;
    if (!at_main_menu()) {
        LOG("display rows: RENDERER, DLSS and FRAME GENERATION change from the main menu only: kept as they were");
        g_pending = before;
        return;
    }
    if (g_pending.renderer != before.renderer) {
        LOG("display rows: RENDERER %s from the next start (Host.Enabled=%d in rfgrt.ini)", g_pending.renderer ? "PATH TRACED" : "STANDARD",
            g_pending.renderer);
        save(L"Host", L"Enabled", g_pending.renderer);
    }
    if (g_pending.dlss != before.dlss) {
        g_actions.set_dlss_mode(g_pending.dlss);
        LOG("display rows: DLSS mode %d (DLSS.Mode in rfgrt.ini)", g_pending.dlss);
        save(L"DLSS", L"Mode", g_pending.dlss);
    }
    if (g_pending.frame_generation != before.frame_generation) {
        g_actions.set_frame_generation(g_pending.frame_generation ? 1 : 0);
        LOG("display rows: FRAME GENERATION %s (FrameGeneration.Enabled in rfgrt.ini)", g_pending.frame_generation ? "on" : "off");
        save(L"FrameGeneration", L"Enabled", g_pending.frame_generation ? 1 : 0);
    }
    g_current = g_pending;
}

// The general page: our rows after the game's.
void add_rows() {
    const int menu = rfg::global<int>(rfg::rva::Menu_handle_doptions);
    if (menu < 0) return;
    add_labels();
    auto count = rfg::fn<CountFn>(rfg::rva::ui_menu_get_total_item_count);
    auto add_list = rfg::fn<AddListFn>(rfg::rva::ui_menu_add_char_list_item);
    auto add_bool = rfg::fn<AddBoolFn>(rfg::rva::ui_menu_add_bool_item);
    g_row_renderer = count(menu);
    if (add_list(menu, "RFGRT_RENDERER_NEXT_START", kRendererNames, 2, &on_renderer, g_pending.renderer, true) < 0) g_row_renderer = -1;
    g_row_dlss = count(menu);
    if (add_list(menu, "RFGRT_DLSS", kDlssNames, kDlssModes, &on_dlss, g_pending.dlss, true) < 0) g_row_dlss = -1;
    g_pending_fg = g_pending.frame_generation;
    g_row_fg = count(menu);
    if (add_bool(menu, "RFGRT_FRAME_GENERATION", &g_pending_fg, &on_frame_generation, true) < 0) g_row_fg = -1;
    update_rows();
    g_shown = true;
    if (!g_told) {
        g_told = true;
        LOG("display rows: the general page has RENDERER (row %d), DLSS (row %d) and FRAME GENERATION (row %d)%s", g_row_renderer,
            g_row_dlss, g_row_fg, at_main_menu() ? "" : " (in game: greyed out)");
    }
}

void __cdecl d_set_detail_page(int page) {
    o_set_detail_page(page);
    if (page == g_page_general) add_rows();
}

void __cdecl d_enter(int previous_state) {
    g_pending = g_current;
    g_row_renderer = g_row_dlss = g_row_fg = -1;
    g_shown = false;
    o_enter(previous_state);
}

void __cdecl d_hint(int hint) {
    if (hint == g_hint_selection) apply();
    o_hint(hint);
}

bool __cdecl d_changes_made() { return o_changes_made() || changed(); }

}  // namespace

bool install(const Config& cfg, const std::wstring& ini, bool frame_generation, const Actions& actions) {
    namespace rva = rfg::rva;
    if (!rva::set_detail_page || !rva::display_menu_enter || !rva::display_hint_callback || !rva::ui_menu_add_char_list_item ||
        !rva::ui_menu_add_bool_item || !rva::ui_menu_enable_menu_item || !rva::ui_menu_get_total_item_count ||
        !rva::Menu_handle_doptions || !rva::Doc_handle_main || !rva::Localization_hash || !rva::LocalizationHash_get ||
        !rva::LocalizationHash_insert) {
        LOG("display rows: this build lacks the display menu's names: no rows");
        return false;
    }
    g_ini = ini;
    g_actions = actions;
    g_path_traced_now = cfg.host_enabled;
    g_dlss = cfg.gi_enabled && cfg.gi_denoiser == 2;
    g_fg = frame_generation;
    g_state_idle = enum_value("GS_MAINMENU_IDLE", 48);
    g_hint_selection = enum_value("HT_SELECTION", 0);
    g_page_general = enum_value("DETAIL_OPTIONS_PAGE_GENERAL", 0);
    g_current.renderer = cfg.host_enabled ? 1 : 0;
    g_current.dlss = cfg.dlss_mode < 0 ? 0 : cfg.dlss_mode >= kDlssModes ? kDlssModes - 1 : cfg.dlss_mode;
    g_current.frame_generation = frame_generation && cfg.fg_enabled;
    g_pending = g_current;
    if (!hooks::create(rva::set_detail_page, &d_set_detail_page, &o_set_detail_page, "set_detail_page") ||
        !hooks::create(rva::display_menu_enter, &d_enter, &o_enter, "display_menu_enter") ||
        !hooks::create(rva::display_hint_callback, &d_hint, &o_hint, "display_hint_callback")) {
        LOG("display rows: the display menu's hooks failed: no rows");
        return false;
    }
    // back's question (lose the changes?) for ours too; without it back drops them unasked
    const bool asks = rva::changes_made && hooks::create(rva::changes_made, &d_changes_made, &o_changes_made, "changes_made");
    LOG("display rows: RENDERER (from the next start), DLSS (%s) and FRAME GENERATION (%s) on the display menu's general page, "
        "changed from the main menu only%s",
        !g_dlss ? "greyed out: GI.Denoiser is not 2" : g_path_traced_now ? "Ray Reconstruction's modes" : "greyed out: no path tracing",
        frame_generation ? "at once" : "greyed out: no presenter in this plugin", asks ? "" : "; back drops their changes unasked");
    return true;
}

bool rows_shown() { return g_shown; }

void test_choose(int renderer, int dlss, int frame_generation) {
    // as a player would: only the rows that can change now (at the main menu only)
    const bool main_menu = at_main_menu();
    if (renderer >= 0 && g_row_renderer >= 0 && main_menu) on_renderer(g_row_renderer, reinterpret_cast<void*>(intptr_t(renderer)));
    if (dlss >= 0 && g_row_dlss >= 0 && main_menu && dlss_row_on()) on_dlss(g_row_dlss, reinterpret_cast<void*>(intptr_t(dlss)));
    if (frame_generation >= 0 && g_row_fg >= 0 && main_menu && g_fg) {
        g_pending_fg = frame_generation != 0;
        on_frame_generation(g_row_fg, nullptr);
    }
    LOG("display rows (Test.DisplayRows): the rows now RENDERER %d, DLSS %d, FRAME GENERATION %d (asked %d, %d, %d)", g_pending.renderer,
        g_pending.dlss, int(g_pending.frame_generation), renderer, dlss, frame_generation);
}

}  // namespace display_rows
