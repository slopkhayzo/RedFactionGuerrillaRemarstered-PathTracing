// rfgrt_host.exe: the 64-bit process that runs the ray tracing core (src/rtcore) next to today's 32-bit rfg.exe.
// Started by the plugin, suspended, in a job object that ends it with the game; its only argument is the name of the
// shared block (src/shared/rfgrt_ipc.h), which carries everything else.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#include "rfgrt_ipc.h"
#include "rtcore.h"

namespace {

FILE* g_log = nullptr;
std::mutex g_log_mutex;
LARGE_INTEGER g_t0, g_freq;

void log_line(const char* line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double s = double(now.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart);
    if (g_log) {
        fprintf(g_log, "[%10.3f] %s\n", s, line);
        fflush(g_log);
    }
}

bool publish_fence_b(HANDLE fence_b, rfgrt_ipc::Block* block, void* ctx) {
    HANDLE plugin = static_cast<HANDLE>(ctx);
    HANDLE in_plugin = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), fence_b, plugin, &in_plugin, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
    block->fence_b_handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(in_plugin));
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    if (argc < 2) return 2;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, argv[1]);
    if (!mapping) return 3;
    // The whole region: the block, then the scene ring.
    auto* block = static_cast<rfgrt_ipc::Block*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!block || block->magic != rfgrt_ipc::kMagic || block->version != rfgrt_ipc::kVersion ||
        block->block_bytes != sizeof(rfgrt_ipc::Block))
        return 4;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(block, &mbi, sizeof(mbi)) || mbi.RegionSize < block->region_bytes ||
        block->ring_offset + block->ring_bytes > block->region_bytes)
        return 4;
    block->host_pid = GetCurrentProcessId();
    if (block->log_path[0]) g_log = _wfsopen(block->log_path, L"ab", _SH_DENYWR);

    char line[300];
    snprintf(line, sizeof(line), "rfgrt_host started (pid %lu) for rfg.exe pid %u", GetCurrentProcessId(), block->plugin_pid);
    log_line(line);
    HANDLE plugin = OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, block->plugin_pid);
    if (!plugin) {
        snprintf(block->host_message, sizeof(block->host_message), "cannot open rfg.exe (error %lu)", GetLastError());
        block->host_state = rfgrt_ipc::kHostFailed;
        log_line(block->host_message);
        return 5;
    }
    rtcore::RunParams p;
    p.block = block;
    p.peer_process = plugin;
    p.publish_fence_b = &publish_fence_b;
    p.publish_ctx = plugin;
    p.log = &log_line;
    const int code = rtcore::run(p);
    snprintf(line, sizeof(line), "rfgrt_host exits with code %d", code);
    log_line(line);
    if (g_log) fclose(g_log);
    CloseHandle(plugin);
    UnmapViewOfFile(block);
    CloseHandle(mapping);
    return code;
}
