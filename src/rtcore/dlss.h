#pragma once
// DLSS Ray Reconstruction (NVIDIA NGX) as the denoiser of the path-traced irradiance (GI.Denoiser=2), in place of the
// GI's temporal and wavelet passes. Built when the DLSS SDK is there (third_party/dlss, tools/fetch_dlss.ps1:
// RFGRT_DLSS); otherwise every call says it is unavailable. The core's thread only.
//
// What it denoises is the light alone (the irradiance the game's patched shaders multiply by their albedo), so its
// albedo guide is white: no texture detail can be lost. Adapted from the SpaceEngineers-RTHWAccel project's wrapper.
#include <d3d12.h>

namespace rtcore::dlss {

using LogFn = void (*)(const char* line);

// One frame's inputs, at the render size (the output at the same size: DLAA).
struct Inputs {
    ID3D12Resource* color = nullptr;            // RGBA16F: this frame's traced irradiance, unfiltered
    ID3D12Resource* diffuse_albedo = nullptr;   // RGBA8: white on surfaces, black on the sky
    ID3D12Resource* specular_albedo = nullptr;  // RGBA8: black (a diffuse signal)
    ID3D12Resource* normal_roughness = nullptr; // RGBA16F: world normal, roughness in w (1)
    ID3D12Resource* depth = nullptr;            // R32F: linear view depth (1e6 for the sky)
    ID3D12Resource* motion = nullptr;           // RG16F: pixels, previous = current + motion
    ID3D12Resource* output = nullptr;           // RGBA16F, unordered access
    unsigned width = 0, height = 0;
    const float* world_to_view = nullptr;  // row-major, row vectors (camera-relative world)
    const float* view_to_clip = nullptr;   // the game's projection, row vectors
    bool reset = false;                    // no usable history (first frame, new textures, a cut)
    float frame_ms = 16.7f;
};

bool built();

// Initializes NGX on `device` once: the runtime (nvngx_dlssd.dll) searched in `runtime_dir`, NGX's own logs under
// `data_dir`. True when Ray Reconstruction is available; later calls give the first answer.
bool initialize(ID3D12Device* device, const wchar_t* runtime_dir, const wchar_t* data_dir, LogFn log);

// Records Ray Reconstruction on `list`: the inputs in the non-pixel shader resource state, the output in unordered
// access. A new size creates the feature first (that frame evaluates nothing: false), as does unavailability. NGX
// changes the list's pipeline, root signature and descriptor heaps: set them again after.
bool evaluate(ID3D12GraphicsCommandList* list, const Inputs& in);

void shutdown();

}  // namespace rtcore::dlss
