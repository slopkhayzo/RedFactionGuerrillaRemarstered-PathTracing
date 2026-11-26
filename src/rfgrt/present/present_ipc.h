#pragma once
// The 32-bit plugin's frames presented by rfgrt_present.exe (64-bit), research plan 03 phase 3: DLSS Frame Generation
// needs Streamline, which is 64-bit only. The plugin captures the game's frames with Direct3D 11 into shared textures
// (the frame, the frame before the HUD, the depth) and writes the camera here; the helper presents them through
// Streamline on a Direct3D 12 swap chain of its own, on a child window over the game's, in place of the game's present.
// One block in a named file mapping, written by both processes; the same layout in both (fixed-size fields only).
#include <cstdint>

namespace present_ipc {

constexpr uint32_t kVersion = 1;
constexpr uint32_t kSlots = 3;  // the frames in flight: a slot is written again once the helper finished it

// framegen::Camera's values (present/framegen.h), fixed-size.
struct Camera {
    float projection[16];
    float inv_projection[16];
    float view_projection[16];
    float inv_view_projection[16];
    float prev_view_projection[16];
    float prev_inv_view_projection[16];
    float right[3], up[3], forward[3];
    float pad0;
    double delta[3];
    float near_clip, far_clip, fov_y, aspect;
    uint32_t depth_inverted, reset;
};

// A slot's shared textures, made by the plugin (Direct3D 11) and duplicated into the helper's process.
struct Slot {
    uint64_t frame;    // NT handles in the helper's process (0: none yet)
    uint64_t hudless;
    uint64_t depth;    // R32F
    uint32_t generation;  // bumped when the slot's textures are made again (the helper opens them again)
    uint32_t width, height, format;   // the frame's and the frame before the HUD's
    uint32_t depth_width, depth_height;
};

struct Block {
    uint32_t version, size;
    uint32_t game_pid, helper_pid;
    uint64_t game_window;  // the game's top-level window (HWND)
    uint32_t adapter_luid_low;
    int32_t adapter_luid_high;
    // handles in the helper's process (each fence's values have one writer)
    uint64_t fence_ready;  // shared fence A: the plugin signals its copies into a slot (ready_value)
    uint64_t fence_done;   // shared fence B: the helper signals its frames (slot_done)
    uint64_t frame_event;  // auto-reset: a frame is ready (plugin -> helper)
    uint64_t done_event;   // auto-reset: the frame was presented (helper -> plugin)
    // control (the plugin)
    volatile int32_t wanted;        // frame generation wanted: else the helper hides its window and DLSS-G is off
    volatile int32_t quit;
    volatile int32_t focus_always;  // FrameGeneration.TestFocus: the focus answered always (unattended tests)
    int32_t pad1;
    Slot slots[kSlots];
    // the frame (the plugin, before frame_event)
    uint64_t frame;        // its number
    uint64_t ready_value;  // the fence's value once the plugin's copies into the slot are done
    uint32_t slot, sync_interval, has_inputs, width, height, format;  // (the frame's size and format, as its slot's)
    float depth_max;
    uint32_t pad2;
    Camera camera;
    // back (the helper)
    volatile int32_t helper_state;  // 0 starting, 1 ready (its window, device and chain), 2 broken (the game presents)
    volatile int32_t generating;    // DLSS-G generating
    volatile uint64_t presented;    // the last frame presented
    volatile uint64_t slot_done[kSlots];  // the fence's value once the helper finished the slot's frame
    volatile uint64_t presents, with_inputs;  // counts, for the plugin's log
    wchar_t log_path[260];
    char reason[256];  // why the helper broke, or why frame generation is unavailable
};

// The same layout in the 32-bit plugin and the 64-bit helper (natural alignment, no pointers).
static_assert(sizeof(Camera) == 472, "Camera's layout");
static_assert(sizeof(Slot) == 48, "Slot's layout");
static_assert(sizeof(Block) == 1576, "Block's layout");

}  // namespace present_ipc
