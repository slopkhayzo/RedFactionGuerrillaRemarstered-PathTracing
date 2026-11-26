// The 32-bit plugin's presenter (research plan 03, phase 3): presenter.h's functions for the 32-bit build. DLSS Frame
// Generation needs Streamline, which is 64-bit only, so the presenting is done by rfgrt_present.exe (64-bit,
// src/present_host; present/present_ipc.h), next to rfgrt_host.exe. This side captures, as the 64-bit presenter does in
// the game's process (presenter.cpp): at the scene's end the frame before the HUD, the game's depth (R32F) and the
// camera; at the game's present the finished frame; each into a slot of shared Direct3D 11 textures the helper opens.
// The game's own present is then skipped while the helper presents the frame through Streamline over the game's window
// (the game waits for it: DLSS-G's pacing is the game's). Until the helper is ready, in exclusive full screen and when it
// fails, the game presents as it ships.
#include "present/presenter.h"

#include "core/config.h"
#include "core/log.h"

#if !defined(_WIN64)

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>

#include "copy_depth_ps.h"  // fxc: g_copy_depth_ps (the game's depth into R32F)
#include "fullscreen_vs.h"  // fxc: g_fullscreen_vs
#include "core/session.h"
#include "game/rfg.h"
#include "harness/screenshot.h"
#include "hooks/hooks.h"
#include "present/present_ipc.h"
#include "render/d3d_state.h"

using Microsoft::WRL::ComPtr;

namespace presenter {
namespace {

constexpr UINT kSlots = present_ipc::kSlots;
constexpr int kPresentSlot = 8;          // IDXGISwapChain::Present in the vtable
constexpr DWORD kDoneWaitMs = 2000;      // the helper's present (DLSS-G's pacing included): past this the game presents
constexpr double kReportSeconds = 10.0;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain* self, UINT sync_interval, UINT flags);

struct State {
    bool presenter_only = false;      // FrameGeneration.Presenter: the presenter without generation (a test)
    std::atomic<bool> wanted{false};  // frame generation (rfgrt_set_frame_generation, FrameGeneration.Enabled)
    bool test_focus = false;          // FrameGeneration.TestFocus
    std::wstring helper_exe;
    PresentFn original = nullptr;
    std::atomic<bool> detour_placed{false};
    IDXGISwapChain* game = nullptr;  // the game's chain, as last seen (render thread)
    // the game's device and window
    ComPtr<ID3D11Device> d3d11;
    ComPtr<ID3D11Device5> d3d11_5;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11DeviceContext4> immediate4;
    HWND parent = nullptr;
    // the helper
    bool setup_tried = false, started = false, broken = false, ready_logged = false;
    HANDLE process = nullptr, job = nullptr, mapping = nullptr, frame_event = nullptr, done_event = nullptr;
    present_ipc::Block* block = nullptr;
    ComPtr<ID3D11Fence> ready;  // fence A: our copies (our values)
    ComPtr<ID3D11Fence> done;   // fence B: the helper's frames (its values)
    UINT64 ready_value = 0;
    bool helper_wanted = false;  // what the helper was last told
    // the slots' shared textures (the frame, the frame before the HUD, the depth)
    ComPtr<ID3D11Texture2D> frame[kSlots], hudless[kSlots], depth[kSlots];
    ComPtr<ID3D11RenderTargetView> depth_rtv[kSlots];
    UINT64 frames = 0;
    UINT64 scene_for = UINT64_MAX;  // the frame whose scene was captured
    // the camera, and the previous captured frame's
    present_ipc::Camera camera{};
    float depth_max = 1.0f;
    float prev_vp[16] = {}, prev_inv_vp[16] = {};
    double prev_position[3] = {};
    UINT prev_width = 0, prev_height = 0;
    bool prev_valid = false;
    // the depth copy (the plugin's full-screen triangle and copy_depth_ps.hlsl)
    ComPtr<ID3D11VertexShader> copy_vs;
    ComPtr<ID3D11PixelShader> copy_ps;
    ComPtr<ID3D11RasterizerState> no_cull;
    ComPtr<ID3D11DepthStencilState> no_depth;
    ComPtr<ID3D11BlendState> write_all;
    bool copy_broken = false;
    // the report
    uint64_t ours = 0, game_presents = 0, fullscreen = 0, scenes = 0;
    // what the hooks saw while nothing went through the helper (a diagnostic)
    uint64_t ended = 0, calls = 0, calls_game = 0, ours_total = 0;
    int64_t diag_from = 0;
    bool other_logged = false;
    void* hooked = nullptr;  // the vtable slot our Present is in
    double wait_ms_sum = 0.0, wait_ms_max = 0.0;
    int64_t report_from = 0;
} s;

bool active() { return s.wanted.load(std::memory_order_relaxed) || s.presenter_only; }

double ms_since(int64_t from) { return session::ticks_to_ms(session::qpc() - from); }

DXGI_FORMAT chain_format(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return format;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

// --- 4x4 matrices, row-major for row vectors (as presenter.cpp) -----------------------------------------------------

void multiply(const float a[16], const float b[16], float c[16]) {
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a[r * 4 + k] * b[k * 4 + col];
            c[r * 4 + col] = sum;
        }
}

bool invert(const float m[16], float out[16]) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? double(m[r * 4 + c]) : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-20) return false;
        if (pivot != c)
            for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] *= inv;
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                const double f = a[r][c];
                for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = float(a[r][c + 4]);
    return true;
}

// The frame's camera with the previous captured frame's: the constants for DLSS-G, the motion's matrix (presenter.cpp's
// read_camera, into the block's form).
void read_camera(const uint8_t* camera, UINT width, UINT height) {
    float world[12];
    memcpy(world, camera + rfg::off::Entity_worldMatrix, sizeof(world));
    present_ipc::Camera& c = s.camera;
    memcpy(c.projection, camera + rfg::off::Camera_projection, sizeof(c.projection));
    const float view[16] = {world[0], world[3], world[6], 0.f, world[1], world[4], world[7], 0.f,
                            world[2], world[5], world[8], 0.f, 0.f,      0.f,      0.f,      1.f};
    multiply(view, c.projection, c.view_projection);
    if (!invert(c.projection, c.inv_projection) || !invert(c.view_projection, c.inv_view_projection)) {
        s.prev_valid = false;
        return;
    }
    for (int k = 0; k < 3; ++k) {
        c.right[k] = world[k];
        c.up[k] = world[3 + k];
        c.forward[k] = world[6 + k];
    }
    const double position[3] = {world[9], world[10], world[11]};
    const bool same_size = s.prev_width == width && s.prev_height == height;
    double moved = 0.0;
    for (int k = 0; k < 3; ++k) {
        c.delta[k] = s.prev_valid ? position[k] - s.prev_position[k] : 0.0;
        moved += c.delta[k] * c.delta[k];
    }
    const bool reset = !s.prev_valid || !same_size || moved > 50.0 * 50.0;  // a cut: none before, a new size, 50 m away
    c.reset = reset ? 1u : 0u;
    if (reset) {
        memcpy(c.prev_view_projection, c.view_projection, sizeof(c.prev_view_projection));
        memcpy(c.prev_inv_view_projection, c.inv_view_projection, sizeof(c.prev_inv_view_projection));
        for (double& d : c.delta) d = 0.0;
    } else {
        memcpy(c.prev_view_projection, s.prev_vp, sizeof(s.prev_vp));
        memcpy(c.prev_inv_view_projection, s.prev_inv_vp, sizeof(s.prev_inv_vp));
    }
    const float p11 = c.projection[5], p00 = c.projection[0], p22 = c.projection[10], p32 = c.projection[14];
    c.near_clip = rfg::field<float>(const_cast<uint8_t*>(camera), rfg::off::Camera_nearClip);
    c.far_clip = rfg::field<float>(const_cast<uint8_t*>(camera), rfg::off::Camera_farClip);
    c.fov_y = p11 != 0.f ? 2.0f * std::atan(1.0f / p11) : 1.0f;
    c.aspect = p00 != 0.f ? p11 / p00 : 1.0f;
    c.depth_inverted = c.near_clip > 0.f && (c.near_clip * p22 + p32) / c.near_clip > 0.5f ? 1u : 0u;
    memcpy(s.prev_vp, c.view_projection, sizeof(s.prev_vp));
    memcpy(s.prev_inv_vp, c.inv_view_projection, sizeof(s.prev_inv_vp));
    memcpy(s.prev_position, position, sizeof(position));
    s.prev_width = width;
    s.prev_height = height;
    s.prev_valid = true;
}

// --- the depth copy (as presenter.cpp) --------------------------------------------------------------------------------

bool ensure_copy() {
    if (s.copy_vs) return true;
    if (s.copy_broken) return false;
    s.copy_broken = true;
    HRESULT hr = s.d3d11->CreateVertexShader(g_fullscreen_vs, sizeof(g_fullscreen_vs), nullptr, &s.copy_vs);
    if (SUCCEEDED(hr)) hr = s.d3d11->CreatePixelShader(g_copy_depth_ps, sizeof(g_copy_depth_ps), nullptr, &s.copy_ps);
    if (SUCCEEDED(hr)) {
        D3D11_RASTERIZER_DESC r{};
        r.FillMode = D3D11_FILL_SOLID;
        r.CullMode = D3D11_CULL_NONE;
        r.DepthClipEnable = TRUE;
        hr = s.d3d11->CreateRasterizerState(&r, &s.no_cull);
    }
    if (SUCCEEDED(hr)) {
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = FALSE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        hr = s.d3d11->CreateDepthStencilState(&d, &s.no_depth);
    }
    if (SUCCEEDED(hr)) {
        D3D11_BLEND_DESC b{};
        b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = s.d3d11->CreateBlendState(&b, &s.write_all);
    }
    if (FAILED(hr)) {
        LOG("presenter: the depth copy could not be made (0x%08lX): no frame generation", static_cast<unsigned long>(hr));
        s.copy_vs.Reset();
        return false;
    }
    s.copy_broken = false;
    return true;
}

void copy_depth(ID3D11ShaderResourceView* depth, ID3D11RenderTargetView* rtv, UINT width, UINT height) {
    ID3D11DeviceContext* c = s.immediate.Get();
    D3D11StateBlock saved;
    saved.capture(c);
    c->OMSetRenderTargets(1, &rtv, nullptr);
    c->OMSetBlendState(s.write_all.Get(), nullptr, 0xFFFFFFFF);
    c->OMSetDepthStencilState(s.no_depth.Get(), 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* no_vb = nullptr;
    UINT zero = 0;
    c->IASetVertexBuffers(0, 1, &no_vb, &zero, &zero);
    c->VSSetShader(s.copy_vs.Get(), nullptr, 0);
    c->PSSetShader(s.copy_ps.Get(), nullptr, 0);
    c->PSSetShaderResources(0, 1, &depth);
    c->RSSetState(s.no_cull.Get());
    const D3D11_VIEWPORT vp{0.f, 0.f, float(width), float(height), 0.f, 1.f};
    c->RSSetViewports(1, &vp);
    c->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    c->PSSetShaderResources(0, 1, &none);
    saved.restore(c);
}

// --- the helper -------------------------------------------------------------------------------------------------------

// A handle of ours duplicated into the helper's process (0 on failure).
uint64_t into_helper(HANDLE handle) {
    HANDLE theirs = nullptr;
    if (!handle || !DuplicateHandle(GetCurrentProcess(), handle, s.process, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS)) return 0;
    return uint64_t(reinterpret_cast<uintptr_t>(theirs));
}

bool make_fence(ComPtr<ID3D11Fence>& fence, uint64_t& helper_handle, const char* name) {
    HANDLE shared = nullptr;
    HRESULT hr = s.d3d11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
    if (SUCCEEDED(hr)) hr = fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared);
    if (FAILED(hr)) {
        LOG("presenter: the shared fence %s could not be made (0x%08lX): the game presents", name, static_cast<unsigned long>(hr));
        return false;
    }
    helper_handle = into_helper(shared);
    CloseHandle(shared);
    return helper_handle != 0;
}

// The game's device, the block, the helper started (in a job: it ends with the game), the fences and events handed to it.
bool set_up(IDXGISwapChain* game) {
    if (s.started) return true;
    if (s.setup_tried) return false;
    s.setup_tried = true;
    DXGI_SWAP_CHAIN_DESC game_desc{};
    if (FAILED(game->GetDesc(&game_desc)) || !game_desc.OutputWindow) {
        LOG("presenter: the game's swap chain has no window: it presents");
        return false;
    }
    s.parent = game_desc.OutputWindow;
    HRESULT hr = game->GetDevice(IID_PPV_ARGS(&s.d3d11));
    if (SUCCEEDED(hr)) hr = s.d3d11.As(&s.d3d11_5);
    if (SUCCEEDED(hr)) {
        s.d3d11->GetImmediateContext(&s.immediate);
        hr = s.immediate.As(&s.immediate4);
    }
    DXGI_ADAPTER_DESC adapter_desc{};
    if (SUCCEEDED(hr)) {
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        hr = s.d3d11.As(&dxgi_device);
        if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->GetDesc(&adapter_desc);
    }
    if (FAILED(hr)) {
        LOG("presenter: the game's device has no Direct3D 11.4 fences (0x%08lX): the game presents", static_cast<unsigned long>(hr));
        return false;
    }
    // the block
    wchar_t name[96];
    swprintf(name, 96, L"Local\\rfgrt_present_%lu_%llu", GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()));
    s.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(present_ipc::Block), name);
    if (s.mapping) s.block = static_cast<present_ipc::Block*>(MapViewOfFile(s.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(present_ipc::Block)));
    if (!s.block) {
        LOG("presenter: the helper's block could not be made (%lu): the game presents", GetLastError());
        return false;
    }
    present_ipc::Block& b = *s.block;
    memset(&b, 0, sizeof(b));
    b.version = present_ipc::kVersion;
    b.size = sizeof(b);
    b.game_pid = GetCurrentProcessId();
    b.game_window = uint64_t(reinterpret_cast<uintptr_t>(s.parent));
    b.adapter_luid_low = adapter_desc.AdapterLuid.LowPart;
    b.adapter_luid_high = adapter_desc.AdapterLuid.HighPart;
    b.wanted = active() ? 1 : 0;
    b.focus_always = s.test_focus ? 1 : 0;
    wcsncpy_s(b.log_path, (session::run_dir() + L"\\rfgrt_present.log").c_str(), _TRUNCATE);
    s.helper_wanted = b.wanted != 0;
    s.frame_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.done_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // the helper, suspended until it has its handles
    s.job = CreateJobObjectW(nullptr, nullptr);
    if (s.job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(s.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    std::wstring cmd = L"\"" + s.helper_exe + L"\" " + name;
    const std::wstring dir = s.helper_exe.substr(0, s.helper_exe.find_last_of(L"\\/"));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(s.helper_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                        dir.c_str(), &si, &pi)) {
        LOG("presenter: starting %ls failed (%lu): the game presents", s.helper_exe.c_str(), GetLastError());
        return false;
    }
    s.process = pi.hProcess;
    if (s.job && !AssignProcessToJobObject(s.job, pi.hProcess)) LOG("presenter: the helper's job could not be set (%lu)", GetLastError());
    b.frame_event = into_helper(s.frame_event);
    b.done_event = into_helper(s.done_event);
    if (!b.frame_event || !b.done_event || !make_fence(s.ready, b.fence_ready, "A") || !make_fence(s.done, b.fence_done, "B")) {
        LOG("presenter: the helper's handles could not be made (%lu): the game presents", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        return false;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    s.started = true;
    LOG("presenter: rfgrt_present.exe started (pid %lu) for the game's window %p on %ls: the game presents until it is ready",
        pi.dwProcessId, static_cast<void*>(s.parent), adapter_desc.Description);
    return true;
}

// The helper ready to take frames (it says so in the block); broken: the game presents from now on.
bool helper_ready() {
    if (!s.started || s.broken) return false;
    const int state = s.block->helper_state;
    if (state == 2 || WaitForSingleObject(s.process, 0) == WAIT_OBJECT_0) {
        s.broken = true;
        LOG("presenter: the helper %s (%s): the game presents from now on", state == 2 ? "broke" : "ended",
            s.block->reason[0] ? s.block->reason : "its log says why");
        return false;
    }
    if (state != 1) return false;
    if (!s.ready_logged) {
        s.ready_logged = true;
        LOG("presenter: the helper is ready: its swap chain presents the game's frames");
    }
    return true;
}

// The helper told whether frame generation is wanted (off: its window hidden), and the change acknowledged.
void tell_helper(bool wanted) {
    if (!s.started || s.broken || s.helper_wanted == wanted) return;
    s.helper_wanted = wanted;
    s.block->wanted = wanted ? 1 : 0;
    if (!wanted) {
        SetEvent(s.frame_event);  // (a wake-up without a frame: it hides its window and acknowledges)
        WaitForSingleObject(s.done_event, 500);
    }
}

// A slot's shared texture, made by Direct3D 11 and duplicated into the helper's process; true when it was made again
// (the helper opens it again).
bool ensure_slot(ComPtr<ID3D11Texture2D>& texture, uint64_t& helper_handle, UINT width, UINT height, DXGI_FORMAT format,
                 const char* name, ComPtr<ID3D11RenderTargetView>* rtv, bool& remade) {
    if (texture) {
        D3D11_TEXTURE2D_DESC d{};
        texture->GetDesc(&d);
        if (d.Width == width && d.Height == height && d.Format == format) return true;
        if (rtv) rtv->Reset();
        texture.Reset();
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rtv ? D3D11_BIND_RENDER_TARGET : 0);
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = s.d3d11->CreateTexture2D(&desc, nullptr, &texture);
    if (SUCCEEDED(hr) && rtv) hr = s.d3d11->CreateRenderTargetView(texture.Get(), nullptr, rtv->ReleaseAndGetAddressOf());
    ComPtr<IDXGIResource1> dxgi;
    HANDLE shared = nullptr;
    if (SUCCEEDED(hr)) hr = texture.As(&dxgi);
    if (SUCCEEDED(hr)) hr = dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &shared);
    if (SUCCEEDED(hr)) {
        helper_handle = into_helper(shared);
        CloseHandle(shared);
        if (!helper_handle) hr = E_FAIL;
    }
    if (FAILED(hr)) {
        LOG("presenter: the shared %s %ux%u (format %d) could not be made (0x%08lX)", name, width, height, int(format),
            static_cast<unsigned long>(hr));
        if (rtv) rtv->Reset();
        texture.Reset();
        helper_handle = 0;
        return false;
    }
    remade = true;
    static int told = 0;
    if (told++ < 24)
        LOG("presenter: the shared %s %ux%u (format %d) made, its handle in the helper %llx", name, width, height, int(format),
            static_cast<unsigned long long>(helper_handle));
    return true;
}

// The slot's textures at the frame's size (the frame; with `scene` the frame before the HUD and the depth too).
bool ensure_slot_textures(UINT slot, UINT width, UINT height, DXGI_FORMAT format, bool scene, UINT depth_width, UINT depth_height) {
    present_ipc::Slot& out = s.block->slots[slot];
    bool remade = false;
    if (!ensure_slot(s.frame[slot], out.frame, width, height, format, "frame", nullptr, remade)) return false;
    if (scene && (!ensure_slot(s.hudless[slot], out.hudless, width, height, format, "frame before the HUD", nullptr, remade) ||
                  !ensure_slot(s.depth[slot], out.depth, depth_width, depth_height, DXGI_FORMAT_R32_FLOAT, "depth", &s.depth_rtv[slot],
                               remade)))
        return false;
    if (remade) {
        static int told = 0;
        if (told++ < 24)
            LOG("presenter: slot %u generation %u: handles frame %llx, before the HUD %llx, depth %llx", slot, out.generation + 1,
                static_cast<unsigned long long>(out.frame), static_cast<unsigned long long>(out.hudless),
                static_cast<unsigned long long>(out.depth));
        out.width = width;
        out.height = height;
        out.format = uint32_t(format);
        if (scene) {
            out.depth_width = depth_width;
            out.depth_height = depth_height;
        }
        ++out.generation;
    }
    return true;
}

// --- capture and present ----------------------------------------------------------------------------------------------

// At the scene's end (render thread): the frame before the HUD, the game's depth and the camera, into the upcoming
// present's slot once the helper finished that slot's last frame.
void capture_scene(uint8_t* scene_renderer, uint8_t* camera) {
    if (!active() || !helper_ready() || !s.game || !scene_renderer || !camera) return;
    auto* depth_texture = rfg::field<uint8_t*>(scene_renderer, rfg::off::SceneRenderer_mainDepthTexture);
    auto* depth_srv = depth_texture ? rfg::field<ID3D11ShaderResourceView*>(depth_texture, rfg::off::TextureData_pShaderView) : nullptr;
    if (!depth_srv || !ensure_copy()) return;
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(s.game->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return;
    D3D11_TEXTURE2D_DESC desc{};
    back_buffer->GetDesc(&desc);
    const DXGI_FORMAT format = chain_format(desc.Format);
    const UINT slot = UINT(s.frames % kSlots);
    UINT depth_width = desc.Width, depth_height = desc.Height;
    {
        ComPtr<ID3D11Resource> resource;
        ComPtr<ID3D11Texture2D> texture;
        depth_srv->GetResource(&resource);
        if (resource && SUCCEEDED(resource.As(&texture))) {
            D3D11_TEXTURE2D_DESC d{};
            texture->GetDesc(&d);
            if (d.Width && d.Height && d.Width <= desc.Width && d.Height <= desc.Height) {
                depth_width = d.Width;
                depth_height = d.Height;
            }
        }
    }
    if (format == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count != 1 ||
        !ensure_slot_textures(slot, desc.Width, desc.Height, format, true, depth_width, depth_height))
        return;
    const UINT64 slot_done = s.block->slot_done[slot];
    if (slot_done) s.immediate4->Wait(s.done.Get(), slot_done);  // the helper's last frame of this slot
    s.immediate->CopyResource(s.hudless[slot].Get(), back_buffer.Get());
    copy_depth(depth_srv, s.depth_rtv[slot].Get(), depth_width, depth_height);
    read_camera(camera, desc.Width, desc.Height);
    if (!s.prev_valid) return;  // (a singular projection)
    s.depth_max = 1.0f - rfg::global<float>(rfg::rva::RL_reserved_landmark_depth_range);
    if (!(s.depth_max > 0.5f && s.depth_max <= 1.0f)) s.depth_max = 1.0f;
    s.scene_for = s.frames;
    ++s.scenes;
}

void report() {
    if (!s.report_from) s.report_from = session::qpc();
    const double seconds = ms_since(s.report_from) / 1000.0;
    if (seconds < kReportSeconds) return;
    if (s.ours || s.game_presents)
        LOG("presenter over %.1f s: %llu frames through the helper (waited %.2f ms avg / %.1f max for its present; %llu "
            "scenes captured for frame generation; it presented %llu, %llu with the inputs; DLSS-G %s), %llu through the "
            "game's (%llu of them in exclusive full screen)",
            seconds, static_cast<unsigned long long>(s.ours), s.ours ? s.wait_ms_sum / double(s.ours) : 0.0, s.wait_ms_max,
            static_cast<unsigned long long>(s.scenes), static_cast<unsigned long long>(s.block ? s.block->presents : 0),
            static_cast<unsigned long long>(s.block ? s.block->with_inputs : 0),
            s.block && s.block->generating ? "generating" : "not generating", static_cast<unsigned long long>(s.game_presents),
            static_cast<unsigned long long>(s.fullscreen));
    s.ours = s.game_presents = s.fullscreen = s.scenes = 0;
    s.wait_ms_sum = s.wait_ms_max = 0.0;
    s.report_from = session::qpc();
}

// The game's frame through the helper. False: the game's chain presents it.
bool present_remote(IDXGISwapChain* game, UINT sync_interval) {
    if (!set_up(game) || !helper_ready()) return false;
    BOOL fullscreen = FALSE;
    if (SUCCEEDED(game->GetFullscreenState(&fullscreen, nullptr)) && fullscreen) {  // the game's chain owns the output
        ++s.fullscreen;
        tell_helper(false);
        return false;
    }
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(game->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    back_buffer->GetDesc(&desc);
    const DXGI_FORMAT format = chain_format(desc.Format);
    const UINT slot = UINT(s.frames % kSlots);
    if (format == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count != 1 ||
        !ensure_slot_textures(slot, desc.Width, desc.Height, format, false, 0, 0))
        return false;
    tell_helper(true);
    // Direct3D 11: once the helper finished this slot's last frame, this frame into the slot, signalled, submitted now
    const UINT64 slot_done = s.block->slot_done[slot];
    if (slot_done) s.immediate4->Wait(s.done.Get(), slot_done);
    s.immediate->CopyResource(s.frame[slot].Get(), back_buffer.Get());
    back_buffer.Reset();  // the game resizes its chain only without references to its buffers
    const UINT64 ready = ++s.ready_value;
    s.immediate4->Signal(s.ready.Get(), ready);
    s.immediate->Flush();
    // the frame for the helper, then its present waited for
    present_ipc::Block& b = *s.block;
    b.slot = slot;
    b.frame = s.frames;
    b.ready_value = ready;
    b.sync_interval = sync_interval;
    b.width = desc.Width;
    b.height = desc.Height;
    b.format = uint32_t(desc.Format);
    const bool inputs = s.scene_for == s.frames;
    b.has_inputs = inputs ? 1 : 0;
    if (inputs) {
        b.camera = s.camera;
        b.depth_max = s.depth_max;
    }
    const int64_t start = session::qpc();
    SetEvent(s.frame_event);
    const DWORD waited = WaitForSingleObject(s.done_event, kDoneWaitMs);
    const double ms = ms_since(start);
    ++s.frames;
    if (waited != WAIT_OBJECT_0 || b.helper_state == 2) {
        s.broken = true;
        b.quit = 1;
        LOG("presenter: the helper %s (%s): the game presents from now on",
            waited != WAIT_OBJECT_0 ? "did not present within 2 s" : "broke", b.reason[0] ? b.reason : "its log says why");
        return false;
    }
    ++s.ours;
    ++s.ours_total;
    s.wait_ms_sum += ms;
    if (ms > s.wait_ms_max) s.wait_ms_max = ms;
    return true;
}

HRESULT STDMETHODCALLTYPE d_present(IDXGISwapChain* self, UINT sync_interval, UINT flags) {
    ++s.calls;
    if (self == s.game) {
        ++s.calls_game;
    } else if (!s.other_logged) {
        s.other_logged = true;
        LOG("presenter: a present of another swap chain %p (the game's %p)", static_cast<void*>(self), static_cast<void*>(s.game));
    }
    if (self == s.game && (flags & DXGI_PRESENT_TEST) == 0) {
        if (active() && !s.broken) {
            if (present_remote(self, sync_interval)) {
                report();
                return S_OK;
            }
        } else {
            tell_helper(false);
        }
        ++s.game_presents;
        report();
    }
    return s.original(self, sync_interval, flags);
}

// The game's swap chain at each frame's end (render thread); its Present detoured at the first.
// The module a code address belongs to (the log), "?" when none.
std::string module_of(const void* address) {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH];
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        static_cast<LPCWSTR>(address), &module) ||
        !GetModuleFileNameW(module, path, MAX_PATH))
        return "?";
    const wchar_t* name = wcsrchr(path, L'\\');
    std::string out;
    for (const wchar_t* c = name ? name + 1 : path; *c; ++c) out += char(*c < 128 ? *c : '?');
    return out;
}

// The module an inline hook at `code` jumps to (E9 rel32, or FF 25 abs32 on x86), else the code's own.
std::string jump_owner(const void* code) {
    const auto* p = static_cast<const uint8_t*>(code);
    if (!p) return "?";
    if (p[0] == 0xE9) return module_of(p + 5 + *reinterpret_cast<const int32_t*>(p + 1)) + " (an inline hook)";
    if (p[0] == 0xFF && p[1] == 0x25) return module_of(**reinterpret_cast<void* const* const*>(p + 2)) + " (an inline hook)";
    return module_of(code);
}

// The game's Present taken over in its swap chain's vtable (every DXGI swap chain of the process shares it): an overlay
// that keeps its own inline hook on the method's code (RivaTuner's, here) drops an inline hook placed after it, so the
// first one lasted a single frame. The original we call is the method's entry, through that overlay's hook.
bool patch_vtable(void** slot) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (*slot != reinterpret_cast<void*>(&d_present)) {
        s.original = reinterpret_cast<PresentFn>(*slot);
        *slot = reinterpret_cast<void*>(&d_present);
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// The game's swap chain at each frame's end (render thread); its Present taken over at the first.
void on_end_frame(IDXGISwapChain* chain) {
    ++s.ended;
    void** vtable = *reinterpret_cast<void***>(chain);
    if (chain != s.game)
        LOG("presenter: the game's swap chain is %p (its vtable's Present %p, in %s)", static_cast<void*>(chain), vtable[kPresentSlot],
            vtable[kPresentSlot] == reinterpret_cast<void*>(&d_present) ? "ours" : module_of(vtable[kPresentSlot]).c_str());
    s.game = chain;
    if (!s.detour_placed.exchange(true)) {
        s.hooked = &vtable[kPresentSlot];
        const void* code = vtable[kPresentSlot];
        if (patch_vtable(&vtable[kPresentSlot]))
            LOG("presenter: the game's IDXGISwapChain::Present taken over in its vtable (the method at %p, its entry going to %s)", code,
                jump_owner(code).c_str());
        else
            LOG("presenter: the swap chain's vtable could not be written (%lu): the game's chain presents", GetLastError());
    } else if (static_cast<void**>(s.hooked) == &vtable[kPresentSlot] && vtable[kPresentSlot] != reinterpret_cast<void*>(&d_present)) {
        static int told = 0;
        if (told++ < 4) LOG("presenter: our vtable entry was replaced (by %s): taken over again", module_of(vtable[kPresentSlot]).c_str());
        patch_vtable(&vtable[kPresentSlot]);
    }
    // (a diagnostic) every 10 s while nothing went through the helper: what the hooks saw
    if (!s.diag_from) s.diag_from = session::qpc();
    if (ms_since(s.diag_from) > 10000.0) {
        if (!s.ours_total && active())
            LOG("presenter (diagnostic): %llu frames ended, our Present called %llu times (%llu with the game's chain)",
                static_cast<unsigned long long>(s.ended), static_cast<unsigned long long>(s.calls),
                static_cast<unsigned long long>(s.calls_game));
        s.ended = s.calls = s.calls_game = 0;
        s.diag_from = session::qpc();
    }
}

std::wstring helper_path() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&helper_path), &module);
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring dir(path, n);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    return dir + L"\\rfgrt\\rfgrt_present.exe";
}

}  // namespace

bool available() { return GetFileAttributesW(helper_path().c_str()) != INVALID_FILE_ATTRIBUTES; }

bool install(const Config& cfg, bool switchable) {
    s.presenter_only = cfg.fg_presenter;
    s.wanted.store(cfg.fg_enabled);
    s.test_focus = cfg.fg_test_focus;
    s.helper_exe = helper_path();
    if (!available()) {
        if (cfg.fg_enabled || cfg.fg_presenter || switchable)
            LOG("presenter: no %ls (frame generation in the 32-bit plugin is presented by it): no frame generation", s.helper_exe.c_str());
        return true;
    }
    if (!cfg.fg_enabled && !cfg.fg_presenter && !switchable) return true;
    screenshot::set_end_frame(&on_end_frame);
    const bool ok = screenshot::install();
    LOG("presenter: %s (frame generation %s, presented by rfgrt_present.exe%s)",
        ok ? "the game's present is hooked at its first frame" : "keen::graphics::endFrame could not be hooked",
        cfg.fg_enabled ? "wanted" : "off", s.test_focus ? "; FrameGeneration.TestFocus: the focus answered always" : "");
    return ok;
}

void set_wanted(bool on) {
    if (s.wanted.exchange(on) != on) LOG("presenter: frame generation %s", on ? "wanted" : "off");
}

void scene_rendered(void* scene_renderer, void* camera) {
    capture_scene(static_cast<uint8_t*>(scene_renderer), static_cast<uint8_t*>(camera));
}

}  // namespace presenter

#endif  // !_WIN64
