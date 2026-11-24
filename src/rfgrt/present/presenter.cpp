#include "present/presenter.h"

#include "core/config.h"
#include "core/log.h"

#if defined(_WIN64)

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "copy_depth_ps.h"   // fxc: g_copy_depth_ps (the game's depth into R32F)
#include "fullscreen_vs.h"   // fxc: g_fullscreen_vs
#include "core/session.h"
#include "game/rfg.h"
#include "harness/screenshot.h"
#include "hooks/hooks.h"
#include "present/framegen.h"
#include "render/d3d_state.h"
#if defined(RFGRT_STREAMLINE)
#include "fg_motion_cs.h"  // dxc: g_fg_motion_cs
#include "fg_ui_cs.h"      // dxc: g_fg_ui_cs
#endif

using Microsoft::WRL::ComPtr;

namespace presenter {
namespace {

constexpr UINT kBuffers = 3;          // our swap chain's buffers (flip model), and the game's frame copies
constexpr int kPresentSlot = 8;       // IDXGISwapChain::Present in the vtable
constexpr DWORD kWaitLimitMs = 5000;  // our queue that far behind: that frame goes to the game's chain
constexpr double kReportSeconds = 10.0;
// A present through Streamline this long turns frame generation off (one as its inputs start excepted: DLSS-G makes its
// feature then, 440-500 ms in Space Engineers).
constexpr double kStalledPresentMs = 400.0;
constexpr wchar_t kChildClass[] = L"rfgrt.presenter";
// Posted by the render thread: to the game's window (our subclass of it makes ours, on that window's thread), to ours.
constexpr UINT kCreateMessage = WM_APP + 0x51;
constexpr UINT kFitMessage = WM_APP + 0x52;   // cover the game's client area, shown
constexpr UINT kHideMessage = WM_APP + 0x53;  // hidden
// The passes' root constants (fg_motion.hlsl: the clip-to-previous-clip rows, the size, the depth range; fg_ui.hlsl the
// size) and their descriptor sets: per slot, the motion's (t0 the depth, t1 none, u0 the motion) then the UI layer's (t0
// the frame, t1 the frame before the HUD, u0 the layer).
constexpr UINT kPassConstants = 20;
constexpr UINT kSetDescriptors = 3;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain* self, UINT sync_interval, UINT flags);
using SceneDrawnFn = void(__cdecl*)(void* scene_renderer, void* camera);

struct Stats {
    uint64_t ours = 0;        // frames presented through our chain
    uint64_t game = 0;        // through the game's (not wanted, full screen, or a failure of ours)
    uint64_t fullscreen = 0;  // of those: the game in exclusive full screen
    uint64_t failed = 0;      // of those: ours could not present
    uint64_t occluded = 0;    // our presents that returned DXGI_STATUS_OCCLUDED
    uint64_t scenes = 0;      // frames whose scene was captured for frame generation (the frame before the HUD, depth)
    double cpu_ms_sum = 0.0, cpu_ms_max = 0.0;          // the render thread's time in our present path
    double present_ms_sum = 0.0, present_ms_max = 0.0;  // in our chain's Present alone (Streamline's pacing included)
};

struct State {
    bool presenter_only = false;      // FrameGeneration.Presenter: the presenter without generation (a test)
    std::atomic<bool> wanted{false};  // frame generation (rfgrt_set_frame_generation, FrameGeneration.Enabled)
    PresentFn original = nullptr;
    SceneDrawnFn original_scene_drawn = nullptr;
    bool on_seam = false;                 // the hook is on OpenRFG64 VOM's seam: every present it sees is the game's
    std::atomic<bool> detour_placed{false};
    IDXGISwapChain* game = nullptr;       // the game's chain, as last seen (render thread)
    // the game's device and window
    ComPtr<ID3D11Device> d3d11;
    ComPtr<ID3D11Device5> d3d11_5;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11DeviceContext4> immediate4;
    HWND parent = nullptr;
    WNDPROC parent_proc = nullptr;      // the game window's procedure under our subclass
    std::atomic<HWND> child{nullptr};   // ours: made on the game window's thread
    bool child_asked = false;
    bool shown = false;                 // ours shown over the game's client area
    // ours
    bool set_up = false;
    bool setup_tried = false;
    bool broken = false;  // ours could not present: the game's chain from now on
    ComPtr<IDXGIFactory4> factory;
    IDXGIFactory2* proxy_factory = nullptr;  // Streamline's proxy of it (kept for good)
    bool streamline = false;                 // our queue and chain made through Streamline's proxies
    ComPtr<ID3D12Device> d3d12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> chain;
    HWND window = nullptr;  // the window ours is on
    uint64_t chain_presents = 0;  // presents through our chain since it was made (Streamline sets up at its first ones)
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // the game's back buffer's (ours: its flip-model form)
    bool tearing = false;                      // DXGI_FEATURE_PRESENT_ALLOW_TEARING
    // The game's back buffer copied for Direct3D 12, one copy per slot (frames % kBuffers): Direct3D 11 writes a slot
    // once Direct3D 12 finished that slot's last frame, three presents back.
    ComPtr<ID3D11Texture2D> frame11[kBuffers];
    ComPtr<ID3D12Resource> frame12[kBuffers];
    ComPtr<ID3D12Fence> fence12;
    ComPtr<ID3D11Fence> fence11;
    UINT64 fence_value = 0;
    ComPtr<ID3D12CommandAllocator> allocators[kBuffers];
    UINT64 allocator_done[kBuffers] = {};  // the fence value once Direct3D 12 finished the slot's list
    ComPtr<ID3D12GraphicsCommandList> list;
    HANDLE event = nullptr;
    UINT64 frames = 0;
    Stats stats;
    int64_t report_from = 0;
    bool removed_logged = false;
    // Frame generation's inputs, per slot as the frame: the frame before the HUD and the game's depth (R32F), made by
    // Direct3D 11 at the seam after the scene (capture_scene) for the present whose frames is scene_for, and the camera.
    ComPtr<ID3D11Texture2D> hudless11[kBuffers];
    ComPtr<ID3D12Resource> hudless12[kBuffers];
    ComPtr<ID3D11Texture2D> depth11[kBuffers];
    ComPtr<ID3D12Resource> depth12[kBuffers];
    ComPtr<ID3D11RenderTargetView> depth_rtv[kBuffers];
    UINT64 scene_for = UINT64_MAX;
    framegen::Camera camera{};
    float depth_max = 1.0f;
    // the previous captured frame's camera
    float prev_vp[16] = {}, prev_inv_vp[16] = {};
    double prev_position[3] = {};
    UINT prev_width = 0, prev_height = 0;
    bool prev_valid = false;
    // Direct3D 11's depth copy (the plugin's full-screen triangle and copy_depth_ps.hlsl)
    ComPtr<ID3D11VertexShader> copy_vs;
    ComPtr<ID3D11PixelShader> copy_ps;
    ComPtr<ID3D11RasterizerState> no_cull;
    ComPtr<ID3D11DepthStencilState> no_depth;
    ComPtr<ID3D11BlendState> write_all;
    bool copy_broken = false;
    // Direct3D 12's passes (fg_motion.hlsl, fg_ui.hlsl) and their outputs at the frame's size, kept in unordered access
    ComPtr<ID3D12RootSignature> pass_signature;
    ComPtr<ID3D12PipelineState> motion_pso, ui_pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT descriptor_size = 0;
    ComPtr<ID3D12Resource> motion, ui;
    bool passes_broken = false;
    int64_t last_inputs = 0;           // QPC of the last present with frame generation's inputs
    bool long_present_allowed = false;  // a run of inputs starting (after a second or more without) allows one
} s;

bool active() { return s.wanted.load(std::memory_order_relaxed) || s.presenter_only; }

double ms_since(int64_t from) { return session::ticks_to_ms(session::qpc() - from); }

// Ours takes the non-sRGB form of the game's format (flip-model chains have no sRGB formats; the copy keeps the bytes).
// UNKNOWN: a format ours cannot have.
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

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

// --- 4x4 matrices, row-major for row vectors ------------------------------------------------------------------------

void multiply(const float a[16], const float b[16], float c[16]) {
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a[r * 4 + k] * b[k * 4 + col];
            c[r * 4 + col] = sum;
        }
}

// The inverse (Gauss-Jordan with partial pivoting, in double); false when singular.
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

// Waits until Direct3D 12 reached `value` on our fence. False when it did not within kWaitLimitMs (logged: the work may
// still run, so nothing it uses may be reused or released) or the device was removed.
bool wait_for(UINT64 value) {
    if (value == 0 || s.fence12->GetCompletedValue() >= value) return true;
    if (FAILED(s.fence12->SetEventOnCompletion(value, s.event))) return false;
    const int64_t start = session::qpc();
    const DWORD waited = WaitForSingleObject(s.event, kWaitLimitMs);
    const UINT64 completed = s.fence12->GetCompletedValue();
    if (completed == UINT64_MAX) {
        if (!s.removed_logged) {
            s.removed_logged = true;
            LOG("presenter: the device was removed (reason 0x%08lX)", static_cast<unsigned long>(s.d3d12->GetDeviceRemovedReason()));
        }
        return false;
    }
    if (waited != WAIT_OBJECT_0 || completed < value) {
        LOG("presenter: our queue has not reached %llu after %.0f ms (at %llu): the frame goes to the game's chain",
            static_cast<unsigned long long>(value), ms_since(start), static_cast<unsigned long long>(completed));
        return false;
    }
    return true;
}

bool wait_idle() { return wait_for(s.fence_value); }

// Frame generation switched off: our window hidden and DLSS-G off, the game's chain presents; our chain is kept for the
// next switch on (DLSS-G does not generate again on a new chain once it has on one: Test.FrameGenerationCycle, take 3).
void pause_ours() {
    if (!s.shown) return;
    PostMessageW(s.child.load(), kHideMessage, 0, 0);
    s.shown = false;
    if (s.streamline) framegen::suspend();
    LOG("presenter: the game's presents again (our swap chain kept)");
}

void release_ours() {
    if (s.shown) {
        PostMessageW(s.child.load(), kHideMessage, 0, 0);
        s.shown = false;
    }
    if (!s.chain) return;
    if (s.streamline) framegen::suspend();
    if (!wait_idle()) {
        LOG("presenter: our swap chain is kept (its frames are still on the GPU)");
        return;
    }
    s.chain.Reset();
    s.window = nullptr;
    LOG("presenter: our swap chain released; the game's presents again");
}

// Our window (the game window's thread): transparent to the mouse (disabled too: the game's window gets its input),
// nothing painted but the swap chain's content.
LRESULT CALLBACK child_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: ValidateRect(window, nullptr); return 0;
        case kFitMessage: {
            RECT client{};
            GetClientRect(GetParent(window), &client);
            SetWindowPos(window, HWND_TOP, 0, 0, client.right - client.left, client.bottom - client.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            return 0;
        }
        case kHideMessage: ShowWindow(window, SW_HIDE); return 0;
        default: return DefWindowProcW(window, message, wparam, lparam);
    }
}

// On the game window's thread (our subclass's kCreateMessage): our window over its client area.
void make_child() {
    if (s.child.load()) return;
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&child_proc), &module);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &child_proc;
    window_class.hInstance = module;
    window_class.lpszClassName = kChildClass;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LOG("presenter: our window class could not be registered (error %lu): the game's chain presents", GetLastError());
        s.broken = true;
        return;
    }
    RECT client{};
    GetClientRect(s.parent, &client);
    const HWND child = CreateWindowExW(WS_EX_NOACTIVATE, kChildClass, L"", WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS, 0, 0,
                                       client.right - client.left, client.bottom - client.top, s.parent, nullptr, module,
                                       nullptr);
    if (!child) {
        LOG("presenter: our window could not be made (error %lu): the game's chain presents", GetLastError());
        s.broken = true;
        return;
    }
    s.child.store(child);
    LOG("presenter: our window %p made over the game's %p (%ldx%ld client area, thread %lu)", static_cast<void*>(child),
        static_cast<void*>(s.parent), client.right - client.left, client.bottom - client.top, GetCurrentThreadId());
}

// The game's window procedure under ours: our window made when the render thread asks, everything else passed on.
LRESULT CALLBACK parent_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kCreateMessage) {
        make_child();
        return 0;
    }
    return CallWindowProcW(s.parent_proc, window, message, wparam, lparam);
}

// Our window is made on the thread of the game's window (a child of another thread's window would join the two threads'
// input): the game's window subclassed, then asked.
void ask_child() {
    if (s.child_asked) return;
    s.child_asked = true;
    s.parent_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(s.parent, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&parent_proc)));
    if (!s.parent_proc) {
        LOG("presenter: the game's window could not be subclassed (error %lu): the game's chain presents", GetLastError());
        s.broken = true;
        return;
    }
    PostMessageW(s.parent, kCreateMessage, 0, 0);
}

// Our device, queue, fence and lists, on the game's adapter, at the first present wanted; with Streamline when this
// build has it (its proxies: our queue and chain go through them, frame generation on or off).
bool set_up(IDXGISwapChain* game) {
    if (s.set_up) return true;
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
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(hr)) {
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> game_adapter;
        hr = s.d3d11.As(&dxgi_device);
        if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&game_adapter);
        if (SUCCEEDED(hr)) hr = game_adapter->GetDesc(&adapter_desc);
    }
    if (SUCCEEDED(hr)) hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&s.factory));
    if (SUCCEEDED(hr)) hr = s.factory->EnumAdapterByLuid(adapter_desc.AdapterLuid, IID_PPV_ARGS(&adapter));
    if (SUCCEEDED(hr)) hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s.d3d12));
    if (SUCCEEDED(hr)) {
        ComPtr<IDXGIFactory5> factory5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(s.factory.As(&factory5)) &&
            SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
            s.tearing = tearing != FALSE;
    }
    HANDLE fence_handle = nullptr;
    if (SUCCEEDED(hr)) hr = s.d3d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.fence12));
    if (SUCCEEDED(hr)) hr = s.d3d12->CreateSharedHandle(s.fence12.Get(), nullptr, GENERIC_ALL, nullptr, &fence_handle);
    if (SUCCEEDED(hr)) {
        hr = s.d3d11_5->OpenSharedFence(fence_handle, IID_PPV_ARGS(&s.fence11));
        CloseHandle(fence_handle);
    }
    // Our queue made through Streamline's device proxy (it sees the queue the chain presents on), the chain through its
    // factory proxy (ensure_ours).
    ID3D12Device* queue_device = s.d3d12.Get();
    if (SUCCEEDED(hr) && framegen::load(s.d3d12.Get())) {
        ID3D12Device* proxy_device = s.d3d12.Get();
        IDXGIFactory2* proxy_factory = s.factory.Get();
        proxy_device->AddRef();  // the proxies keep these references for good
        proxy_factory->AddRef();
        if (framegen::upgrade(reinterpret_cast<void**>(&proxy_device)) && framegen::upgrade(reinterpret_cast<void**>(&proxy_factory))) {
            queue_device = proxy_device;
            s.proxy_factory = proxy_factory;
            s.streamline = true;
        } else {
            LOG("presenter: Streamline's proxies could not be made: ours presents without frame generation");
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = queue_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&s.queue));
    }
    for (UINT i = 0; i < kBuffers && SUCCEEDED(hr); ++i)
        hr = s.d3d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocators[i]));
    if (SUCCEEDED(hr))
        hr = s.d3d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocators[0].Get(), nullptr, IID_PPV_ARGS(&s.list));
    if (SUCCEEDED(hr)) hr = s.list->Close();
    if (FAILED(hr)) {
        LOG("presenter: setting up failed (0x%08lX): the game's swap chain presents", static_cast<unsigned long>(hr));
        return false;
    }
    s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.set_up = true;
    LOG("presenter: Direct3D 12 on the game's adapter (%ls), a queue of its own; tearing %s; frame generation %s",
        adapter_desc.Description, s.tearing ? "allowed" : "not supported",
        s.streamline ? (framegen::enabled() ? "on" : *framegen::unavailable() ? framegen::unavailable() : "available (off)")
                     : framegen::unavailable());
    return true;
}

// A texture of the game's frame Direct3D 12 can read: made by Direct3D 11 and shared, opened by Direct3D 12 (`rtv`: a
// render target too, with its view).
bool ensure_shared(ComPtr<ID3D11Texture2D>& texture11, ComPtr<ID3D12Resource>& texture12, UINT width, UINT height,
                   DXGI_FORMAT format, const char* name, ComPtr<ID3D11RenderTargetView>* rtv = nullptr) {
    if (texture12) {
        D3D11_TEXTURE2D_DESC desc{};
        texture11->GetDesc(&desc);
        if (desc.Width == width && desc.Height == height && desc.Format == format) return true;
        if (!wait_idle()) return false;
        if (rtv) rtv->Reset();
        texture11.Reset();
        texture12.Reset();
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
    HRESULT hr = s.d3d11->CreateTexture2D(&desc, nullptr, &texture11);
    ComPtr<IDXGIResource1> dxgi;
    HANDLE handle = nullptr;
    if (SUCCEEDED(hr) && rtv) hr = s.d3d11->CreateRenderTargetView(texture11.Get(), nullptr, rtv->ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) hr = texture11.As(&dxgi);
    if (SUCCEEDED(hr)) hr = dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
    if (SUCCEEDED(hr)) {
        hr = s.d3d12->OpenSharedHandle(handle, IID_PPV_ARGS(&texture12));
        CloseHandle(handle);
    }
    if (FAILED(hr)) {
        LOG("presenter: the shared %s %ux%u (format %d) could not be made (0x%08lX)", name, width, height, int(format),
            static_cast<unsigned long>(hr));
        if (rtv) rtv->Reset();
        texture11.Reset();
        texture12.Reset();
        return false;
    }
    return true;
}

// Ours on `window` at the game's back buffer's size and format: made, or resized.
bool ensure_ours(HWND window, UINT width, UINT height, DXGI_FORMAT format) {
    const DXGI_FORMAT ours = chain_format(format);
    if (ours == DXGI_FORMAT_UNKNOWN) {
        LOG("presenter: the game's back buffer format %d has no flip-model form: the game's chain presents", int(format));
        s.broken = true;
        return false;
    }
    if (s.chain && window != s.window) release_ours();
    const UINT flags = s.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (!s.chain) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = ours;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kBuffers;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = flags;
        ComPtr<IDXGISwapChain1> chain;
        IDXGIFactory2* factory = s.proxy_factory ? s.proxy_factory : s.factory.Get();
        HRESULT hr = factory->CreateSwapChainForHwnd(s.queue.Get(), window, &desc, nullptr, nullptr, &chain);
        if (SUCCEEDED(hr)) hr = chain.As(&s.chain);
        if (FAILED(hr)) {
            LOG("presenter: no Direct3D 12 swap chain on our window (0x%08lX): the game's chain presents",
                static_cast<unsigned long>(hr));
            s.chain.Reset();
            s.broken = true;
            return false;
        }
        s.factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        s.window = window;
        s.chain_presents = 0;
        s.width = width;
        s.height = height;
        s.format = format;
        LOG("presenter: Direct3D 12 flip-model swap chain on our window: %ux%u, format %d (the game's %d), %u buffers%s", width,
            height, int(ours), int(format), kBuffers, s.proxy_factory ? ", through Streamline" : "");
    } else if (width != s.width || height != s.height || format != s.format) {
        if (!wait_idle()) return false;  // resized once the GPU is done with its buffers
        const HRESULT hr = s.chain->ResizeBuffers(kBuffers, width, height, ours, flags);
        if (FAILED(hr)) {
            LOG("presenter: resizing our swap chain to %ux%u failed (0x%08lX): the game's chain presents", width, height,
                static_cast<unsigned long>(hr));
            release_ours();
            s.broken = true;
            return false;
        }
        s.width = width;
        s.height = height;
        s.format = format;
        LOG("presenter: our swap chain resized to %ux%u", width, height);
    }
    for (UINT i = 0; i < kBuffers; ++i)
        if (!ensure_shared(s.frame11[i], s.frame12[i], width, height, ours, "frame")) return false;
    return true;
}

// --- frame generation's inputs ---------------------------------------------------------------------------------------

// Direct3D 11's depth copy: made at its first use.
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

// The game's depth (its scene renderer's main depth texture) into `rtv`, the context's state put back after.
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

// The frame's camera (rl_camera: its world matrix, rows right, up, forward then the position; its projection, row vectors)
// with the previous captured frame's: the constants for DLSS-G, the motion's matrix.
void read_camera(const uint8_t* camera, UINT width, UINT height) {
    float world[12];
    memcpy(world, camera + rfg::off::Entity_worldMatrix, sizeof(world));
    framegen::Camera& c = s.camera;
    memcpy(c.projection, camera + rfg::off::Camera_projection, sizeof(c.projection));
    // the view's rotation: the camera's axes in its columns (rows of the world matrix transposed)
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
    // a cut: no previous frame, a new size, or the camera 50 m away from where it was (a teleport, a cutscene's cut)
    c.reset = !s.prev_valid || !same_size || moved > 50.0 * 50.0;
    if (c.reset) {
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
    c.depth_inverted = c.near_clip > 0.f && (c.near_clip * p22 + p32) / c.near_clip > 0.5f;
    // the previous frame's, for the next
    memcpy(s.prev_vp, c.view_projection, sizeof(s.prev_vp));
    memcpy(s.prev_inv_vp, c.inv_view_projection, sizeof(s.prev_inv_vp));
    memcpy(s.prev_position, position, sizeof(position));
    s.prev_width = width;
    s.prev_height = height;
    s.prev_valid = true;
}

// At the seam after the game's scene (render thread, mid-frame): the frame before the HUD, the game's depth and the
// camera, for the upcoming present's slot, once Direct3D 12 finished that slot's last frame.
void capture_scene(uint8_t* scene_renderer, uint8_t* camera) {
    // (our chain not needed yet: frame generation just switched on makes it at this frame's present, which then has its
    // inputs)
    if (!s.streamline || s.broken || !framegen::enabled() || !s.set_up || !s.game || !scene_renderer || !camera) return;
    auto* depth_texture = rfg::field<uint8_t*>(scene_renderer, rfg::off::SceneRenderer_mainDepthTexture);
    auto* depth_srv = depth_texture ? rfg::field<ID3D11ShaderResourceView*>(depth_texture, rfg::off::TextureData_pShaderView) : nullptr;
    if (!depth_srv || !ensure_copy()) return;
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(s.game->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return;
    D3D11_TEXTURE2D_DESC desc{};
    back_buffer->GetDesc(&desc);
    const DXGI_FORMAT format = chain_format(desc.Format);
    const UINT slot = UINT(s.frames % kBuffers);
    if (format == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count != 1 ||
        !ensure_shared(s.hudless11[slot], s.hudless12[slot], desc.Width, desc.Height, format, "frame before the HUD") ||
        !ensure_shared(s.depth11[slot], s.depth12[slot], desc.Width, desc.Height, DXGI_FORMAT_R32_FLOAT, "depth", &s.depth_rtv[slot]))
        return;
    if (s.allocator_done[slot]) s.immediate4->Wait(s.fence11.Get(), s.allocator_done[slot]);
    s.immediate->CopyResource(s.hudless11[slot].Get(), back_buffer.Get());
    copy_depth(depth_srv, s.depth_rtv[slot].Get(), desc.Width, desc.Height);
    read_camera(camera, desc.Width, desc.Height);
    if (!s.prev_valid) return;  // (a singular projection)
    s.depth_max = 1.0f - rfg::global<float>(rfg::rva::RL_reserved_landmark_depth_range);
    if (!(s.depth_max > 0.5f && s.depth_max <= 1.0f)) s.depth_max = 1.0f;
    s.scene_for = s.frames;
    ++s.stats.scenes;
}

void __cdecl d_scene_drawn(void* scene_renderer, void* camera) {
    capture_scene(static_cast<uint8_t*>(scene_renderer), static_cast<uint8_t*>(camera));
    if (s.original_scene_drawn) s.original_scene_drawn(scene_renderer, camera);
}

// Direct3D 12's passes and their outputs, at the frame's size: made, or remade.
bool ensure_passes(UINT width, UINT height, DXGI_FORMAT format) {
#if defined(RFGRT_STREAMLINE)
    if (s.passes_broken) return false;
    if (!s.pass_signature) {
        s.passes_broken = true;
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, kPassConstants};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {2, ranges};
        const D3D12_ROOT_SIGNATURE_DESC rs{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, error;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (SUCCEEDED(hr))
            hr = s.d3d12->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s.pass_signature));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = s.pass_signature.Get();
        if (SUCCEEDED(hr)) {
            pd.CS = {g_fg_motion_cs, sizeof(g_fg_motion_cs)};
            hr = s.d3d12->CreateComputePipelineState(&pd, IID_PPV_ARGS(&s.motion_pso));
        }
        if (SUCCEEDED(hr)) {
            pd.CS = {g_fg_ui_cs, sizeof(g_fg_ui_cs)};
            hr = s.d3d12->CreateComputePipelineState(&pd, IID_PPV_ARGS(&s.ui_pso));
        }
        if (SUCCEEDED(hr)) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = kBuffers * 2 * kSetDescriptors;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = s.d3d12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.heap));
        }
        if (FAILED(hr)) {
            LOG("presenter: frame generation's passes could not be made (0x%08lX): no frame generation", static_cast<unsigned long>(hr));
            return false;
        }
        s.descriptor_size = s.d3d12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        s.passes_broken = false;
    }
    auto make = [&](ComPtr<ID3D12Resource>& texture, DXGI_FORMAT f, const char* name) {
        if (texture) {
            const D3D12_RESOURCE_DESC d = texture->GetDesc();
            if (d.Width == width && d.Height == height && d.Format == f) return true;
            if (!wait_idle()) return false;
            texture.Reset();
        }
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width;
        d.Height = height;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = f;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const HRESULT hr = s.d3d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                           nullptr, IID_PPV_ARGS(&texture));
        if (FAILED(hr)) {
            LOG("presenter: the %s texture %ux%u could not be made (0x%08lX)", name, width, height, static_cast<unsigned long>(hr));
            return false;
        }
        return true;
    };
    return make(s.motion, DXGI_FORMAT_R16G16_FLOAT, "motion") && make(s.ui, format, "UI layer");
#else
    (void)width;
    (void)height;
    (void)format;
    return false;
#endif
}

// A descriptor set of the passes' heap: t0, t1 (null where none), u0.
D3D12_GPU_DESCRIPTOR_HANDLE write_set(UINT set, ID3D12Resource* t0, DXGI_FORMAT t0_format, ID3D12Resource* t1,
                                      DXGI_FORMAT t1_format, ID3D12Resource* u0, DXGI_FORMAT u0_format) {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = s.heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(set) * kSetDescriptors * s.descriptor_size;
    auto srv = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT at) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = f;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += SIZE_T(at) * s.descriptor_size;
        s.d3d12->CreateShaderResourceView(r, &d, h);
    };
    srv(t0, t0_format, 0);
    srv(t1, t1_format, 1);
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = u0_format;
    u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
    h.ptr += SIZE_T(2) * s.descriptor_size;
    s.d3d12->CreateUnorderedAccessView(u0, nullptr, &u, h);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = s.heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(set) * kSetDescriptors * s.descriptor_size;
    return gpu;
}

// The motion and the UI layer for the slot's frame (its copy, the frame before the HUD and the depth in the non-pixel
// shader resource state), on our list.
void record_passes(UINT slot, UINT width, UINT height, DXGI_FORMAT format) {
    ID3D12GraphicsCommandList* list = s.list.Get();
    ID3D12DescriptorHeap* heaps[] = {s.heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(s.pass_signature.Get());
    // the motion: the clip-to-previous-clip matrix as framegen gives it DLSS-G (inverse view-projection, the offset, the
    // previous view-projection)
    const framegen::Camera& c = s.camera;
    float move[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    move[12] = float(c.delta[0]);
    move[13] = float(c.delta[1]);
    move[14] = float(c.delta[2]);
    float world[16], clip_to_prev[16];
    multiply(c.inv_view_projection, move, world);
    multiply(world, c.prev_view_projection, clip_to_prev);
    uint32_t constants[kPassConstants] = {};
    memcpy(constants, clip_to_prev, sizeof(clip_to_prev));
    constants[16] = width;
    constants[17] = height;
    memcpy(&constants[18], &s.depth_max, sizeof(float));
    list->SetPipelineState(s.motion_pso.Get());
    list->SetComputeRoot32BitConstants(0, kPassConstants, constants, 0);
    list->SetComputeRootDescriptorTable(1, write_set(slot * 2, s.depth12[slot].Get(), DXGI_FORMAT_R32_FLOAT, nullptr,
                                                     DXGI_FORMAT_R32_FLOAT, s.motion.Get(), DXGI_FORMAT_R16G16_FLOAT));
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    // the UI layer
    const uint32_t size[2] = {width, height};
    list->SetPipelineState(s.ui_pso.Get());
    list->SetComputeRoot32BitConstants(0, 2, size, 0);
    list->SetComputeRootDescriptorTable(1, write_set(slot * 2 + 1, s.frame12[slot].Get(), format, s.hudless12[slot].Get(),
                                                     format, s.ui.Get(), format));
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &uav);
}

void report() {
    if (!s.report_from) s.report_from = session::qpc();
    const double seconds = ms_since(s.report_from) / 1000.0;
    if (seconds < kReportSeconds) return;
    const Stats& t = s.stats;
    if (t.ours || t.game)
        LOG("presenter over %.1f s: %llu frames through ours (our path %.2f ms avg / %.1f max on the render thread, "
            "its Present %.2f / %.1f; %llu occluded; %llu scenes captured for frame generation), %llu through the game's "
            "(%llu of them in exclusive full screen, %llu failures of ours)",
            seconds, static_cast<unsigned long long>(t.ours), t.ours ? t.cpu_ms_sum / double(t.ours) : 0.0, t.cpu_ms_max,
            t.ours ? t.present_ms_sum / double(t.ours) : 0.0, t.present_ms_max, static_cast<unsigned long long>(t.occluded),
            static_cast<unsigned long long>(t.scenes), static_cast<unsigned long long>(t.game),
            static_cast<unsigned long long>(t.fullscreen), static_cast<unsigned long long>(t.failed));
    if (s.streamline) framegen::report(seconds);
    s.stats = Stats{};
    s.report_from = session::qpc();
}

// The game's frame through ours. False: the game's chain presents it (`result` unused).
bool present_ours(IDXGISwapChain* game, UINT sync_interval, HRESULT& result) {
    const int64_t start = session::qpc();
    if (!set_up(game)) {
        s.broken = true;
        return false;
    }
    ask_child();
    const HWND child = s.child.load();
    if (!child) return false;  // until the game window's thread made it
    // Exclusive full screen: the game's chain owns the output.
    BOOL fullscreen = FALSE;
    if (SUCCEEDED(game->GetFullscreenState(&fullscreen, nullptr)) && fullscreen) {
        ++s.stats.fullscreen;
        release_ours();
        return false;
    }
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(game->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        ++s.stats.failed;
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    back_buffer->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1 || !ensure_ours(child, desc.Width, desc.Height, desc.Format)) {
        ++s.stats.failed;
        return false;
    }
    // Direct3D 11: once Direct3D 12 finished this slot's last frame (three presents back), this frame into the slot's
    // copy, signalled, and submitted now: else the copy and its signal wait in Direct3D 11's buffer until the game's next
    // frame flushes it, and our queue with them (and with frame generation Streamline's present waits on that queue).
    const UINT slot = UINT(s.frames % kBuffers);
    if (s.allocator_done[slot]) s.immediate4->Wait(s.fence11.Get(), s.allocator_done[slot]);
    s.immediate->CopyResource(s.frame11[slot].Get(), back_buffer.Get());
    back_buffer.Reset();  // the game resizes its chain only without references to its buffers
    const UINT64 ready = ++s.fence_value;
    s.immediate4->Signal(s.fence11.Get(), ready);
    s.immediate->Flush();
    // Direct3D 12: into our current back buffer, after the copy.
    if (!wait_for(s.allocator_done[slot])) {
        ++s.stats.failed;
        return false;  // its list may still run: neither it nor its allocator is touched
    }
    ComPtr<ID3D12Resource> target;
    HRESULT hr = s.chain->GetBuffer(s.chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&target));
    if (SUCCEEDED(hr)) hr = s.allocators[slot]->Reset();
    if (SUCCEEDED(hr)) hr = s.list->Reset(s.allocators[slot].Get(), nullptr);
    if (FAILED(hr)) {
        ++s.stats.failed;
        return false;  // the copy and its signal are harmless without a reader
    }
    ID3D12GraphicsCommandList* list = s.list.Get();
    ID3D12Resource* frame12 = s.frame12[slot].Get();
    const DXGI_FORMAT format = chain_format(desc.Format);
    transition(list, frame12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list, target.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(target.Get(), frame12);
    transition(list, target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    // Frame generation: the frame's inputs when its scene was captured (the frame before the HUD, the depth, the camera);
    // the motion and the UI layer made here, then all tagged: Streamline copies them on this list.
    bool inputs = false;
    if (s.streamline) {
        const bool have = s.scene_for == s.frames;
        framegen::begin_frame(have);
        if (have && framegen::enabled() && ensure_passes(desc.Width, desc.Height, format)) {
            ID3D12Resource* hudless12 = s.hudless12[slot].Get();
            ID3D12Resource* depth12 = s.depth12[slot].Get();
            const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            transition(list, frame12, D3D12_RESOURCE_STATE_COPY_SOURCE, srv);
            transition(list, hudless12, D3D12_RESOURCE_STATE_COMMON, srv);
            transition(list, depth12, D3D12_RESOURCE_STATE_COMMON, srv);
            record_passes(slot, desc.Width, desc.Height, format);
            framegen::Inputs in;
            in.depth = depth12;
            in.motion = s.motion.Get();
            in.hudless = hudless12;
            in.ui = s.ui.Get();
            in.width = desc.Width;
            in.height = desc.Height;
            inputs = framegen::set_inputs(list, s.camera, in);
            transition(list, depth12, srv, D3D12_RESOURCE_STATE_COMMON);
            transition(list, hudless12, srv, D3D12_RESOURCE_STATE_COMMON);
            transition(list, frame12, srv, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
    }
    transition(list, frame12, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (FAILED(list->Close())) {
        ++s.stats.failed;
        return false;
    }
    s.queue->Wait(s.fence12.Get(), ready);
    ID3D12CommandList* lists[] = {list};
    s.queue->ExecuteCommandLists(1, lists);
    const UINT64 done = ++s.fence_value;
    s.queue->Signal(s.fence12.Get(), done);
    s.allocator_done[slot] = done;
    if (inputs) {
        if (!s.last_inputs || session::ticks_to_ms(start - s.last_inputs) > 1000.0) s.long_present_allowed = true;
        s.last_inputs = start;
    }
    target.Reset();
    if (s.streamline) framegen::before_present();
    const int64_t present_start = session::qpc();
    result = s.chain->Present(sync_interval, sync_interval == 0 && s.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    const double present_ms = ms_since(present_start);
    s.stats.present_ms_sum += present_ms;
    if (present_ms > s.stats.present_ms_max) s.stats.present_ms_max = present_ms;
    ++s.chain_presents;
    if (s.streamline) {
        framegen::after_present();
        // A present held this long while DLSS-G generates is its pacing stalled (it waits on the GPU): off before a stall
        // turns into a GPU timeout. Not a new chain's first presents (Streamline sets up at them: 680 ms seen) nor while
        // DLSS-G is paused.
        if (present_ms > kStalledPresentMs && framegen::generating() && s.chain_presents > 3) {
            if (s.long_present_allowed) {
                s.long_present_allowed = false;
                LOG("presenter: a present through Streamline took %.0f ms as frame generation started", present_ms);
            } else {
                framegen::set_wanted(false);
                s.wanted.store(false);
                LOG("presenter: a present through Streamline took %.0f ms with frame generation on: frame generation is "
                    "turned off",
                    present_ms);
            }
        }
    }
    ++s.frames;
    if (FAILED(result) && result != DXGI_ERROR_DEVICE_REMOVED && result != DXGI_ERROR_DEVICE_RESET) {
        LOG("presenter: our Present failed (0x%08lX): the game's swap chain presents from now on",
            static_cast<unsigned long>(result));
        ++s.stats.failed;
        release_ours();
        s.broken = true;
        return false;
    }
    ++s.stats.ours;
    if (result == DXGI_STATUS_OCCLUDED) ++s.stats.occluded;
    // Our window shown over the game's client area once ours presented, fitted again when that area changed.
    RECT client{}, mine{};
    GetClientRect(s.parent, &client);
    GetClientRect(child, &mine);
    if (!s.shown || client.right != mine.right || client.bottom != mine.bottom) {
        PostMessageW(child, kFitMessage, 0, 0);
        s.shown = true;
    }
    const double ms = ms_since(start);
    s.stats.cpu_ms_sum += ms;
    if (ms > s.stats.cpu_ms_max) s.stats.cpu_ms_max = ms;
    return true;
}

HRESULT STDMETHODCALLTYPE d_present(IDXGISwapChain* self, UINT sync_interval, UINT flags) {
    // On VOM's seam every present is the game's; the plugins' detour sees every swap chain's (one implementation): only
    // the game's chain (the one its frames end with) is taken over, ours and test presents go through as they are.
    if (s.on_seam) s.game = self;
    if (self == s.game && (flags & DXGI_PRESENT_TEST) == 0) {
        if (active() && !s.broken) {
            HRESULT result = S_OK;
            if (present_ours(self, sync_interval, result)) {
                report();
                return result;
            }
        } else if (s.broken) {
            release_ours();
        } else {
            pause_ours();
        }
        ++s.stats.game;
        report();
    }
    return s.original(self, sync_interval, flags);
}

// The plugins: the game's swap chain at each frame's end (render thread); its Present detoured at the first.
void on_end_frame(IDXGISwapChain* chain) {
    s.game = chain;
    if (s.detour_placed.exchange(true)) return;
    void** vtable = *reinterpret_cast<void***>(chain);
    if (!hooks::create_now(vtable[kPresentSlot], reinterpret_cast<void*>(&d_present), reinterpret_cast<void**>(&s.original),
                           "IDXGISwapChain::Present"))
        LOG("presenter: IDXGISwapChain::Present could not be hooked: the game's chain presents");
}

}  // namespace

bool install(const Config& cfg) {
    s.presenter_only = cfg.fg_presenter;
    s.wanted.store(cfg.fg_enabled);
    framegen::set_wanted(cfg.fg_enabled);
    // OpenRFG64 VOM: the seams of its present and of its scene's end, always (its row switches while playing).
    if (hooks::seam("IDXGISwapChain::Present", reinterpret_cast<void*>(&d_present), reinterpret_cast<void**>(&s.original))) {
        s.on_seam = true;
        const bool scene = hooks::seam("openrfg_scene_drawn", reinterpret_cast<void*>(&d_scene_drawn),
                                       reinterpret_cast<void**>(&s.original_scene_drawn));
        LOG("presenter: on the game's present%s (frame generation %s%s)", scene ? " and its scene's end" : " (no scene seam: no inputs)",
            cfg.fg_enabled ? "wanted" : "off", cfg.fg_presenter ? "; FrameGeneration.Presenter: ours presents without it" : "");
        return true;
    }
    // The plugins: only when wanted at start, a detour placed at the first frame's end (their scene's end: research plan
    // 03 phase 3).
    if (!cfg.fg_enabled && !cfg.fg_presenter) return true;
    screenshot::set_end_frame(&on_end_frame);
    const bool ok = screenshot::install();
    LOG("presenter: %s (frame generation %s%s)", ok ? "the game's present is hooked at its first frame" : "keen::graphics::endFrame could not be hooked",
        cfg.fg_enabled ? "wanted" : "off", cfg.fg_presenter ? "; FrameGeneration.Presenter: ours presents without it" : "");
    return ok;
}

void set_wanted(bool on) {
    framegen::set_wanted(on);
    if (s.wanted.exchange(on) != on) LOG("presenter: frame generation %s", on ? "wanted" : "off");
}

}  // namespace presenter

#else  // the 32-bit plugin: no presenter (Streamline is 64-bit)

namespace presenter {

bool install(const Config& cfg) {
    if (cfg.fg_enabled || cfg.fg_presenter) LOG("presenter: none in the 32-bit plugin (frame generation needs the 64-bit one)");
    return true;
}
void set_wanted(bool) {}

}  // namespace presenter

#endif
