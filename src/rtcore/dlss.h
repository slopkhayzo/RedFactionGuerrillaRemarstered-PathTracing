#pragma once
// DLSS Ray Reconstruction (NVIDIA NGX) as the denoiser of the path-traced irradiance (GI.Denoiser=2), in place of the
// GI's temporal and wavelet passes. Built when the DLSS SDK is there (third_party/dlss, tools/fetch_dlss.ps1:
// RFGRT_DLSS); otherwise every call says it is unavailable. The core's thread only.
//
// What it denoises is the light alone (the irradiance the game's patched shaders multiply by their albedo), so its
// albedo guide is white: no texture detail can be lost. Its modes (DLSS.Mode) trace the light at a share of the frame's
// size and upscale it to the frame's: DLAA at the frame's size, the others at NVIDIA's render size for them. Adapted
// from the SpaceEngineers-RTHWAccel project's wrapper.
#include <d3d12.h>

namespace rtcore::dlss {

using LogFn = void (*)(const char* line);

// DLSS.Mode (rfgrt_ipc::Block::dlss_mode): 0 off (no Ray Reconstruction), 1 DLAA, 2 quality, 3 balanced, 4 performance,
// 5 ultra performance.
constexpr int kModeOff = 0, kModeDlaa = 1, kModeCount = 6;

// One frame's inputs, in the top-left `width` x `height` of their textures (the render size), and its output, at the
// output size (the same for DLAA, larger when Ray Reconstruction upscales).
struct Inputs {
    ID3D12Resource* color = nullptr;            // RGBA16F: this frame's traced irradiance, unfiltered
    ID3D12Resource* diffuse_albedo = nullptr;   // RGBA8: white on surfaces, black on the sky
    ID3D12Resource* specular_albedo = nullptr;  // RGBA8: black (a diffuse signal)
    ID3D12Resource* normal_roughness = nullptr; // RGBA16F: world normal, roughness in w (1)
    ID3D12Resource* depth = nullptr;            // R32F: linear view depth (1e6 for the sky)
    ID3D12Resource* motion = nullptr;           // RG16F: render pixels, previous = current + motion
    ID3D12Resource* output = nullptr;           // RGBA16F, unordered access
    unsigned width = 0, height = 0;             // the render size
    unsigned out_width = 0, out_height = 0;     // the output's size
    int mode = kModeDlaa;                       // DLSS.Mode, 1 to 5
    // The jitter as DLSS takes it (render pixels, y down): where the geometry moved, the opposite of the sample's
    // offset from its pixel's centre (DLSS Programming Guide 3.7.3).
    float jitter_x = 0.0f, jitter_y = 0.0f;
    const float* world_to_view = nullptr;  // row-major, row vectors (camera-relative world)
    const float* view_to_clip = nullptr;   // the game's projection, row vectors
    bool reset = false;                    // no usable history (first frame, new textures, a cut)
    float frame_ms = 16.7f;
};

bool built();

// Initializes NGX on `device` once: the runtime (nvngx_dlssd.dll) searched in `runtime_dir`, NGX's own logs under
// `data_dir`. True when Ray Reconstruction is available; later calls give the first answer.
bool initialize(ID3D12Device* device, const wchar_t* runtime_dir, const wchar_t* data_dir, LogFn log);

// The render size for an output of `out_width` x `out_height` in `mode`: NVIDIA's optimal size for that mode (by its
// published scale where NGX gives none), the output's own for DLAA and while Ray Reconstruction is unavailable.
void render_size(unsigned out_width, unsigned out_height, int mode, unsigned& width, unsigned& height);
// A mode's name for the log ("DLAA", "quality", ...).
const char* mode_name(int mode);

// Records Ray Reconstruction on `list`: the inputs in the non-pixel shader resource state, the output in unordered
// access. A new size or mode creates the feature first (that frame evaluates nothing: false), as does unavailability;
// the frames that used the previous one must be complete. NGX changes the list's pipeline, root signature and
// descriptor heaps: set them again after.
bool evaluate(ID3D12GraphicsCommandList* list, const Inputs& in);

void shutdown();

}  // namespace rtcore::dlss
