#include "test/input_script.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace input_script {
namespace {

struct Event {
    double t = 0;
    enum Kind { kLook, kKey, kMouse } kind = kLook;
    float dx = 0, dy = 0, duration = 0;
    int index = 0;
    bool down = false;
};
struct Look {
    double until;
    float dx, dy;
};

std::vector<Event> g_events;
size_t g_next = 0;
std::vector<Look> g_looks;
uint8_t g_keys[256] = {};  // the script's keys: 0 never used, 1 up, 2 down
uint8_t g_buttons[5] = {};
float g_carry_x = 0, g_carry_y = 0;  // the fraction of a count not sent yet
int64_t g_last_poll = 0;
uint64_t g_frames = 0;
thread_local bool t_in_controls = false;

using VoidFn = void(__cdecl*)();
VoidFn o_controls_poll = nullptr, o_input_poll = nullptr;

// controls_poll: the gameplay frame's input; its input_poll is the one that takes the frame's state.
void __cdecl d_controls_poll() {
    t_in_controls = true;
    o_controls_poll();
    t_in_controls = false;
}

// input_poll makes the "next" cached input state current, and the frame reads that: the script writes it first.
void inject() {
    const double t = session::gameplay_seconds();
    const int64_t now = session::qpc();
    const double dt = g_last_poll ? session::ticks_to_ms(now - g_last_poll) / 1000.0 : 0.0;
    g_last_poll = now;
    if (t < 0) return;  // not in gameplay yet
    while (g_next < g_events.size() && g_events[g_next].t <= t) {
        const Event& e = g_events[g_next++];
        switch (e.kind) {
            case Event::kLook:
                g_looks.push_back({e.t + e.duration, e.dx, e.dy});
                LOG("input script at %.2f s: look %g %g a second for %g s", t, e.dx, e.dy, e.duration);
                break;
            case Event::kKey:
                g_keys[e.index] = e.down ? 2 : 1;
                LOG("input script at %.2f s: key %d %s", t, e.index, e.down ? "down" : "up");
                break;
            case Event::kMouse:
                g_buttons[e.index] = e.down ? 2 : 1;
                LOG("input script at %.2f s: mouse button %d %s", t, e.index, e.down ? "down" : "up");
                break;
        }
    }
    float x = g_carry_x, y = g_carry_y;
    for (const Look& l : g_looks) {
        if (t < l.until) {
            x += float(l.dx * dt);
            y += float(l.dy * dt);
        }
    }
    const float sx = std::trunc(x), sy = std::trunc(y);
    g_carry_x = x - sx;
    g_carry_y = y - sy;
    uint8_t* state = static_cast<uint8_t*>(rfg::addr(rfg::rva::Cached_input_states)) +
                     size_t(rfg::global<uint8_t>(rfg::rva::Next_cached_input_state)) * rfg::off::CachedInput_size;
    uint8_t* mouse = state + rfg::off::CachedInput_mouse;
    rfg::field<float>(mouse, rfg::off::MouseState_moveX) = sx;
    rfg::field<float>(mouse, rfg::off::MouseState_moveY) = sy;
    for (int i = 0; i < 5; ++i)
        if (g_buttons[i]) mouse[rfg::off::MouseState_buttons + i] = g_buttons[i] == 2 ? 1 : 0;
    uint8_t* keys = state + rfg::off::CachedInput_keyboard;  // cached_keyboard_state: a byte a key
    for (int i = 0; i < 256; ++i)
        if (g_keys[i]) keys[i] = g_keys[i] == 2 ? 1 : 0;
    ++g_frames;
}

void __cdecl d_input_poll() {
    if (t_in_controls) inject();
    o_input_poll();
}

bool load(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        LOG("input script: cannot read %ls", path.c_str());
        return false;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (char* hash = strchr(line, '#')) *hash = 0;
        Event e{};
        char cmd[16] = {}, arg[16] = {};
        if (sscanf_s(line, "%lf %15s", &e.t, cmd, unsigned(sizeof(cmd))) != 2) continue;
        if (strcmp(cmd, "look") == 0 &&
            sscanf_s(line, "%lf %15s %f %f %f", &e.t, cmd, unsigned(sizeof(cmd)), &e.dx, &e.dy, &e.duration) == 5) {
            e.kind = Event::kLook;
        } else if ((strcmp(cmd, "key") == 0 || strcmp(cmd, "mouse") == 0) &&
                   sscanf_s(line, "%lf %15s %d %15s", &e.t, cmd, unsigned(sizeof(cmd)), &e.index, arg, unsigned(sizeof(arg))) == 4) {
            e.kind = cmd[0] == 'k' ? Event::kKey : Event::kMouse;
            e.down = strcmp(arg, "down") == 0;
            if (e.index < 0 || e.index > (e.kind == Event::kKey ? 255 : 4)) {
                LOG("input script: line ignored (no such button): %s", line);
                continue;
            }
        } else {
            LOG("input script: line ignored: %s", line);
            continue;
        }
        // in time order (a stable insertion: lines with the same time keep theirs)
        size_t at = g_events.size();
        while (at > 0 && g_events[at - 1].t > e.t) --at;
        g_events.insert(g_events.begin() + ptrdiff_t(at), e);
    }
    fclose(f);
    LOG("input script %ls: %zu commands", path.c_str(), g_events.size());
    return !g_events.empty();
}

}  // namespace

bool install(const std::wstring& path) {
    if (rfg::off::CachedInput_size < rfg::off::CachedInput_keyboard + 256) {
        LOG("input script: this build's cached_input_state has no room for 256 keys: not installed");
        return false;
    }
    if (!load(path)) return false;
    bool ok = hooks::create(rfg::rva::controls_poll, &d_controls_poll, &o_controls_poll, "controls_poll");
    ok &= hooks::create(rfg::rva::input_poll, &d_input_poll, &o_input_poll, "input_poll");
    return ok;
}

}  // namespace input_script
