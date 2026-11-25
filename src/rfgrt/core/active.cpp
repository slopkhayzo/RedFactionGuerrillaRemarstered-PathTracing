#include "core/active.h"

#include <windows.h>

#include <atomic>

namespace active {
namespace {

std::atomic<bool> g_switchable{false};
std::atomic<bool> g_on{true};
std::atomic<int> g_work{0};

}  // namespace

bool switchable() { return g_switchable.load(std::memory_order_acquire); }

bool on() { return g_on.load(std::memory_order_acquire); }

void set_switchable(bool initially_on) {
    g_on.store(initially_on, std::memory_order_release);
    g_switchable.store(true, std::memory_order_release);
}

void set_on(bool on) { g_on.store(on); }

Work::Work() {
    g_work.fetch_add(1);
    on = g_on.load();
    if (!on) g_work.fetch_sub(1);
}

Work::~Work() {
    if (on) g_work.fetch_sub(1);
}

bool wait_idle(unsigned ms) {
    const ULONGLONG t0 = GetTickCount64();
    while (g_work.load() != 0) {
        if (GetTickCount64() - t0 >= ms) return false;
        Sleep(1);
    }
    return true;
}

}  // namespace active
