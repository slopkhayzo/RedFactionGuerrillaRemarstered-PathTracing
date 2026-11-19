#include "core/gameloop.h"

#include <windows.h>

#include <atomic>
#include <vector>

#include "core/log.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace gameloop {
namespace {

std::vector<void (*)(int)> g_end;
std::atomic<unsigned long> g_sim_tid{0};
std::atomic<unsigned long long> g_frames{0};

bool(__cdecl* o_do_frame)();

bool __cdecl d_do_frame() {
    if (!g_sim_tid.load(std::memory_order_relaxed)) {
        g_sim_tid = GetCurrentThreadId();
        LOG("main loop runs on thread %lu", GetCurrentThreadId());
    }
    const bool result = o_do_frame();
    const int state = rfg::gameseq_get_state();
    g_frames.fetch_add(1, std::memory_order_release);
    for (auto fn : g_end) fn(state);
    return result;
}

}  // namespace

bool install() { return hooks::create(rfg::rva::rfg_do_frame, &d_do_frame, &o_do_frame, "rfg_do_frame"); }

void add_end(void (*fn)(int)) { g_end.push_back(fn); }

unsigned long sim_thread_id() { return g_sim_tid.load(std::memory_order_relaxed); }
unsigned long long frames_ended() { return g_frames.load(std::memory_order_acquire); }

}  // namespace gameloop
