#pragma once
// DLSS Frame Generation through NVIDIA Streamline (third_party/streamline, RFGRT_STREAMLINE; 64-bit only), on the
// presenter's Direct3D 12 swap chain (presenter.h): manual hooking, only the presenter's queue and chain go through
// Streamline's proxies. Its inputs are the presenter's own (the game's depth, the motion made from it and the camera, the
// frame without the HUD, the UI layer), tagged on the presenter's list at each present. Reflex low latency and the PCL
// markers, which DLSS-G requires. Ported from the SpaceEngineers-RTHWAccel project's framegen. Built without the SDK,
// it is never available.
#include <d3d12.h>
#include <dxgi.h>

namespace framegen {

// The frame's camera: row-major 4x4 matrices for row vectors (the game's convention), camera-relative world.
struct Camera {
    float projection[16];            // view -> clip (rl_camera::m_projection_transform)
    float inv_projection[16];
    float view_projection[16];       // camera-relative world -> clip: the view's rotation, then the projection
    float inv_view_projection[16];
    float prev_view_projection[16];  // the previous frame's
    float prev_inv_view_projection[16];
    float right[3], up[3], forward[3];  // the camera's axes (world)
    double delta[3];                    // this frame's camera position minus the previous frame's (world)
    float near_clip = 0.1f, far_clip = 1000.0f, fov_y = 1.0f, aspect = 1.0f;
    bool depth_inverted = false;  // 1 at the near plane, 0 at the far one
    bool reset = false;           // no usable previous frame (the first, a cut, a new size)
};

// What a frame gives DLSS-G, all at the frame's size, on the presenter's device.
struct Inputs {
    ID3D12Resource* depth = nullptr;    // R32F: the game's hardware depth (non-pixel shader resource state)
    ID3D12Resource* motion = nullptr;   // RG16F: pixels, previous = current + motion (unordered access state)
    ID3D12Resource* hudless = nullptr;  // the frame before the HUD (non-pixel shader resource state)
    ID3D12Resource* ui = nullptr;       // the UI layer, colour and alpha (unordered access state)
    unsigned width = 0, height = 0;
};

// Once, as the presenter sets up: Streamline (sl.interposer.dll from the plugin's folder, checked for NVIDIA's signature)
// started on `device`, DLSS-G and Reflex looked up. False when unavailable (the reason logged and in unavailable()).
bool load(ID3D12Device* device);
bool loaded();
// Streamline's proxy of a device or DXGI factory (slUpgradeInterface): the presenter's queue and swap chain are made
// through them. The proxy holds the interface; the caller keeps the returned pointer for good.
bool upgrade(void** interface_pointer);
// Frame generation wanted (the game's setting, FrameGeneration.Enabled) and supported here.
void set_wanted(bool on);
bool enabled();
// DLSS-G on right now (frames are being generated; not while paused without inputs).
bool generating();
// Why it cannot run (empty: it can, or Streamline was not tried yet).
const char* unavailable();

// At each present through the presenter's chain (render thread), in order: the frame's start (whether it has inputs:
// without them for a while, in menus and loading screens, DLSS-G pauses), its inputs (on the presenter's list, copied by
// Streamline there), the markers around the present.
void begin_frame(bool has_inputs);
bool set_inputs(ID3D12GraphicsCommandList* list, const Camera& camera, const Inputs& inputs);
void before_present();
void after_present();
// The statistics and DLSS-G's state logged (with the presenter's report).
void report(double seconds);
// The presenter gives the game's presents back (frame generation switched off): DLSS-G set off now, so that when it is on
// again it waits for a frame with inputs.
void suspend();

}  // namespace framegen
