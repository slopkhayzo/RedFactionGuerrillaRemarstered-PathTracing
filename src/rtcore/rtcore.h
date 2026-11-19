#pragma once
// The ray tracing core (64-bit, D3D12): opens the plugin's shared resources and answers each frame on the GPU. It runs
// in rfgrt_host.exe next to today's 32-bit rfg.exe, and on a thread inside the plugin with a 64-bit rfg.exe; the same
// run() serves both. Phase 1: a test pattern from the game's depth (research/docs/03, phase 1).
#include <windows.h>

#include "rfgrt_ipc.h"

namespace rtcore {

using LogFn = void (*)(const char* line);
// Hands fence B's NT handle to the plugin: into another process (DuplicateHandle) or within this one. Stores the
// plugin-side value in block->fence_b_handle and returns true on success.
using PublishFenceFn = bool (*)(HANDLE fence_b, rfgrt_ipc::Block* block, void* ctx);

struct RunParams {
    rfgrt_ipc::Block* block = nullptr;
    HANDLE peer_process = nullptr;  // the plugin's process: run() returns when it exits (null in-process)
    PublishFenceFn publish_fence_b = nullptr;
    void* publish_ctx = nullptr;
    LogFn log = nullptr;
};

// Runs until block->stop is set, the peer process exits or something fails. Returns 0 on a normal stop.
int run(const RunParams& p);

}  // namespace rtcore
