// rfgrt_present.exe: the 32-bit plugin's presenter (research plan 03, phase 3; present/present_ipc.h). DLSS Frame
// Generation needs NVIDIA Streamline, which is 64-bit only: the 32-bit plugin starts this process (in a job: it ends with
// the game), captures the game's frames into shared Direct3D 11 textures (the frame, the frame before the HUD, the depth)
// and the camera, and in place of the game's present hands each frame here. This process presents it through Streamline
// on a Direct3D 12 flip-model swap chain of its own, on a child window over the game's client area (the window of another
// process: their threads' input joined, hence the window's own thread here, which never waits), with the motion made from
// the depth and the camera and the UI layer from the two frames, as the 64-bit plugin's presenter does in the game's
// process (present/presenter.cpp). DLSS-G generates only for the process whose window has the focus: the foreground
// window is answered as ours while the game's is in front (user32's GetForegroundWindow, which Streamline asks; the
// research's fg_probe showed it). Usage: rfgrt_present.exe <the block's file mapping name>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "fg_motion_cs.h"  // dxc: g_fg_motion_cs
#include "fg_ui_cs.h"      // dxc: g_fg_ui_cs
#include "present/framegen.h"
#include "present/present_ipc.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kSlots = present_ipc::kSlots;
constexpr UINT kBuffers = 3;  // our swap chain's
constexpr DWORD kWaitLimitMs = 5000;
constexpr double kReportSeconds = 10.0;
constexpr double kStalledPresentMs = 400.0;
constexpr wchar_t kChildClass[] = L"rfgrt.present";
constexpr UINT kFitMessage = WM_APP + 0x52;   // cover the game's client area, shown
constexpr UINT kHideMessage = WM_APP + 0x53;  // hidden
constexpr UINT kPassConstants = 20;
constexpr UINT kSetDescriptors = 3;

present_ipc::Block* g_block = nullptr;
HWND g_parent = nullptr;
std::atomic<HWND> g_child{nullptr};
HANDLE g_child_made = nullptr;
LARGE_INTEGER g_frequency{};

double ms_since(const LARGE_INTEGER& from) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - from.QuadPart) * 1000.0 / double(g_frequency.QuadPart);
}

// --- the focus, as Streamline asks it ---------------------------------------------------------------------------------

using WindowFn = HWND(WINAPI*)();
WindowFn o_foreground = nullptr;
std::atomic<uint64_t> g_asked{0}, g_answered{0};

// The game's window in front (or FrameGeneration.TestFocus): ours answers, so that DLSS-G, which generates only for the
// process whose window has the focus, takes it as this process's.
HWND WINAPI d_foreground() {
    ++g_asked;
    const HWND real = o_foreground();
    const HWND child = g_child.load();
    if (child && (real == g_parent || (g_block && g_block->focus_always))) {
        ++g_answered;
        return child;
    }
    return real;
}

bool hook_focus() {
    return MH_Initialize() == MH_OK &&
           MH_CreateHookApi(L"user32", "GetForegroundWindow", reinterpret_cast<LPVOID>(&d_foreground),
                            reinterpret_cast<LPVOID*>(&o_foreground)) == MH_OK &&
           MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
}

// --- our window, on a thread of its own -------------------------------------------------------------------------------

LRESULT CALLBACK child_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_NCHITTEST: return HTTRANSPARENT;  // the mouse goes to the game's window
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: ValidateRect(window, nullptr); return 0;
        case kFitMessage: {
            RECT client{};
            GetClientRect(g_parent, &client);
            SetWindowPos(window, HWND_TOP, 0, 0, client.right - client.left, client.bottom - client.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            return 0;
        }
        case kHideMessage: ShowWindow(window, SW_HIDE); return 0;
        default: return DefWindowProcW(window, message, wparam, lparam);
    }
}

DWORD WINAPI window_thread(LPVOID) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &child_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kChildClass;
    RegisterClassExW(&wc);
    RECT client{};
    GetClientRect(g_parent, &client);
    // disabled and never activated: the game's window keeps the keyboard and the mouse
    const HWND child = CreateWindowExW(WS_EX_NOACTIVATE, kChildClass, L"", WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS, 0, 0,
                                       client.right - client.left, client.bottom - client.top, g_parent, nullptr, wc.hInstance,
                                       nullptr);
    if (!child) {
        LOG("present: our window over the game's could not be made (error %lu)", GetLastError());
        SetEvent(g_child_made);
        return 1;
    }
    g_child.store(child);
    LOG("present: our window %p over the game's %p (%ldx%ld client area; its own thread %lu)", static_cast<void*>(child),
        static_cast<void*>(g_parent), client.right - client.left, client.bottom - client.top, GetCurrentThreadId());
    SetEvent(g_child_made);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// --- Direct3D 12 ------------------------------------------------------------------------------------------------------

struct State {
    ComPtr<IDXGIFactory4> factory;
    IDXGIFactory2* proxy_factory = nullptr;
    bool streamline = false;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> chain;
    bool tearing = false;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // the game's (ours: its flip-model form)
    uint64_t chain_presents = 0;
    ComPtr<ID3D12Fence> ready;  // fence A: the plugin's copies
    ComPtr<ID3D12Fence> done;   // fence B: our frames
    UINT64 done_value = 0;
    HANDLE event = nullptr;
    ComPtr<ID3D12CommandAllocator> allocators[kSlots];
    UINT64 allocator_done[kSlots] = {};
    ComPtr<ID3D12GraphicsCommandList> list;
    // the slots' shared textures, as opened, and their handles, held open: a handle the block still names is one we
    // opened (a value Windows cannot give another while we hold it), a different one a texture the plugin made again
    ComPtr<ID3D12Resource> frame[kSlots], hudless[kSlots], depth[kSlots];
    uint64_t held[kSlots][3] = {};
    uint32_t generation[kSlots] = {};
    // the passes (fg_motion.hlsl, fg_ui.hlsl) and their outputs
    ComPtr<ID3D12RootSignature> pass_signature;
    ComPtr<ID3D12PipelineState> motion_pso, ui_pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT descriptor_size = 0;
    ComPtr<ID3D12Resource> motion, ui;
    bool passes_broken = false;
    bool shown = false;
    bool generation_wanted = false;
    bool long_present_allowed = true;
    // the report
    LARGE_INTEGER report_from{};
    uint64_t presents = 0, with_inputs = 0, occluded = 0;
    double present_ms_sum = 0.0, present_ms_max = 0.0;
} s;

void fail(const char* reason) {
    LOG("present: %s: the game presents from now on", reason);
    strncpy_s(g_block->reason, reason, _TRUNCATE);
    g_block->helper_state = 2;
}

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

void multiply(const float a[16], const float b[16], float c[16]) {
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a[r * 4 + k] * b[k * 4 + col];
            c[r * 4 + col] = sum;
        }
}

// Until Direct3D 12 reached `value` on fence B. False when not within kWaitLimitMs or the device was removed.
bool wait_for(UINT64 value) {
    if (value == 0 || s.done->GetCompletedValue() >= value) return true;
    if (FAILED(s.done->SetEventOnCompletion(value, s.event))) return false;
    WaitForSingleObject(s.event, kWaitLimitMs);
    const UINT64 completed = s.done->GetCompletedValue();
    if (completed == UINT64_MAX) {
        LOG("present: the device was removed (reason 0x%08lX)", static_cast<unsigned long>(s.device->GetDeviceRemovedReason()));
        return false;
    }
    return completed >= value;
}

bool wait_idle() { return wait_for(s.done_value); }

// A shared object by its handle (duplicated into this process by the plugin); `close`: the handle closed after it.
bool open_shared(uint64_t handle, REFIID iid, void** out, const char* name, bool close = true) {
    const HRESULT hr = s.device->OpenSharedHandle(reinterpret_cast<HANDLE>(handle), iid, out);
    if (close) CloseHandle(reinterpret_cast<HANDLE>(handle));
    if (FAILED(hr)) LOG("present: the shared %s could not be opened (0x%08lX)", name, static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

// Our device on the game's adapter, Streamline on it (its proxies: our queue and chain go through them), the plugin's
// fences opened, our lists.
bool set_up() {
    const LUID luid{g_block->adapter_luid_low, g_block->adapter_luid_high};
    ComPtr<IDXGIAdapter1> adapter;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&s.factory));
    if (SUCCEEDED(hr)) hr = s.factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    DXGI_ADAPTER_DESC1 ad{};
    if (SUCCEEDED(hr)) adapter->GetDesc1(&ad);
    if (SUCCEEDED(hr)) hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s.device));
    if (FAILED(hr)) {
        LOG("present: Direct3D 12 on the game's adapter failed (0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }
    {
        ComPtr<IDXGIFactory5> factory5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(s.factory.As(&factory5)) &&
            SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
            s.tearing = tearing != FALSE;
    }
    if (!open_shared(g_block->fence_ready, IID_PPV_ARGS(&s.ready), "fence A") ||
        !open_shared(g_block->fence_done, IID_PPV_ARGS(&s.done), "fence B"))
        return false;
    s.done_value = s.done->GetCompletedValue();
    ID3D12Device* queue_device = s.device.Get();
    framegen::set_wanted(g_block->wanted != 0);
    s.generation_wanted = g_block->wanted != 0;
    if (framegen::load(s.device.Get())) {
        ID3D12Device* proxy_device = s.device.Get();
        IDXGIFactory2* proxy_factory = s.factory.Get();
        proxy_device->AddRef();  // the proxies keep these references for good
        proxy_factory->AddRef();
        if (framegen::upgrade(reinterpret_cast<void**>(&proxy_device)) && framegen::upgrade(reinterpret_cast<void**>(&proxy_factory))) {
            queue_device = proxy_device;
            s.proxy_factory = proxy_factory;
            s.streamline = true;
        } else {
            LOG("present: Streamline's proxies could not be made: ours presents without frame generation");
        }
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = queue_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&s.queue));
    for (UINT i = 0; i < kSlots && SUCCEEDED(hr); ++i)
        hr = s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocators[i]));
    if (SUCCEEDED(hr))
        hr = s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocators[0].Get(), nullptr, IID_PPV_ARGS(&s.list));
    if (SUCCEEDED(hr)) hr = s.list->Close();
    if (FAILED(hr)) {
        LOG("present: our queue and lists could not be made (0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }
    s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    LOG("present: Direct3D 12 on the game's adapter (%ls), a queue of its own; tearing %s; frame generation %s", ad.Description,
        s.tearing ? "allowed" : "not supported",
        s.streamline ? (framegen::enabled() ? "on" : *framegen::unavailable() ? framegen::unavailable() : "available (off)")
                     : framegen::unavailable());
    return true;
}

// Ours on our window at the game's size and format: made, or resized.
bool ensure_chain(UINT width, UINT height, DXGI_FORMAT format) {
    const DXGI_FORMAT ours = chain_format(format);
    if (ours == DXGI_FORMAT_UNKNOWN) {
        char reason[96];
        snprintf(reason, sizeof(reason), "the game's back buffer format %d has no flip-model form", int(format));
        fail(reason);
        return false;
    }
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
        HRESULT hr = factory->CreateSwapChainForHwnd(s.queue.Get(), g_child.load(), &desc, nullptr, nullptr, &chain);
        if (SUCCEEDED(hr)) hr = chain.As(&s.chain);
        if (FAILED(hr)) {
            char reason[96];
            snprintf(reason, sizeof(reason), "no Direct3D 12 swap chain on our window (0x%08lX)", static_cast<unsigned long>(hr));
            fail(reason);
            s.chain.Reset();
            return false;
        }
        s.factory->MakeWindowAssociation(g_child.load(), DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        s.chain_presents = 0;
        LOG("present: Direct3D 12 flip-model swap chain on our window: %ux%u, format %d (the game's %d), %u buffers%s", width, height,
            int(ours), int(format), kBuffers, s.proxy_factory ? ", through Streamline" : "");
    } else if (width != s.width || height != s.height || format != s.format) {
        if (!wait_idle()) return false;
        const HRESULT hr = s.chain->ResizeBuffers(kBuffers, width, height, ours, flags);
        if (FAILED(hr)) {
            char reason[96];
            snprintf(reason, sizeof(reason), "resizing our swap chain to %ux%u failed (0x%08lX)", width, height,
                     static_cast<unsigned long>(hr));
            fail(reason);
            return false;
        }
        LOG("present: our swap chain resized to %ux%u", width, height);
    }
    s.width = width;
    s.height = height;
    s.format = format;
    return true;
}

// The slot's shared textures, opened again when the plugin made them again (a new size).
// One of a slot's shared textures as the block names it: kept while its handle is the one we hold and it is open,
// else (made again by the plugin, or not open) opened from the block's handle, which we then hold. 'what' for the log.
bool open_one(ComPtr<ID3D12Resource>& texture, uint64_t& held, uint64_t now, const char* name, bool& idle, const char*& what) {
    if (now == held && (texture || !now)) {
        what = now ? "kept" : "none";
        return true;
    }
    if (texture && !idle) {
        if (!wait_idle()) return false;  // (our frames still reading the old one)
        idle = true;
    }
    texture.Reset();
    if (held && held != now) CloseHandle(reinterpret_cast<HANDLE>(held));
    held = now;
    if (!now) {
        what = "none";
        return true;
    }
    ID3D12Resource* raw = nullptr;
    const HRESULT hr = s.device->OpenSharedHandle(reinterpret_cast<HANDLE>(now), IID_PPV_ARGS(&raw));
    if (FAILED(hr) || !raw) {
        LOG("present: the shared %s (handle %llx) could not be opened (0x%08lX)", name, static_cast<unsigned long long>(now),
            static_cast<unsigned long>(hr));
        what = "FAILED";
        return false;
    }
    texture.Attach(raw);
    what = "opened";
    return true;
}

// The slot's shared textures, those the plugin made again (a new handle) opened again.
bool open_slot(UINT slot) {
    const present_ipc::Slot& in = g_block->slots[slot];
    if (s.frame[slot] && s.generation[slot] == in.generation) return true;
    bool idle = false;
    const char* what[3] = {"", "", ""};
    const bool frame_ok = open_one(s.frame[slot], s.held[slot][0], in.frame, "frame", idle, what[0]);
    open_one(s.hudless[slot], s.held[slot][1], in.hudless, "frame before the HUD", idle, what[1]);
    open_one(s.depth[slot], s.held[slot][2], in.depth, "depth", idle, what[2]);
    static int told = 0;
    if (told++ < 24)
        LOG("present: slot %u generation %u: the frame %s (%llx), before the HUD %s (%llx), the depth %s (%llx)", slot, in.generation,
            what[0], static_cast<unsigned long long>(in.frame), what[1], static_cast<unsigned long long>(in.hudless), what[2],
            static_cast<unsigned long long>(in.depth));
    s.generation[slot] = in.generation;
    if (!frame_ok || !s.frame[slot]) {
        fail("the slot's frame could not be opened");
        return false;
    }
    return true;
}

// --- frame generation's inputs (as present/presenter.cpp makes them) ------------------------------------------------

bool ensure_passes(UINT width, UINT height, DXGI_FORMAT format, UINT depth_width, UINT depth_height) {
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
            hr = s.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s.pass_signature));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = s.pass_signature.Get();
        if (SUCCEEDED(hr)) {
            pd.CS = {g_fg_motion_cs, sizeof(g_fg_motion_cs)};
            hr = s.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&s.motion_pso));
        }
        if (SUCCEEDED(hr)) {
            pd.CS = {g_fg_ui_cs, sizeof(g_fg_ui_cs)};
            hr = s.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&s.ui_pso));
        }
        if (SUCCEEDED(hr)) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = kSlots * 2 * kSetDescriptors;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = s.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.heap));
        }
        if (FAILED(hr)) {
            LOG("present: frame generation's passes could not be made (0x%08lX): no frame generation", static_cast<unsigned long>(hr));
            return false;
        }
        s.descriptor_size = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        s.passes_broken = false;
    }
    auto make = [&](ComPtr<ID3D12Resource>& texture, DXGI_FORMAT f, const char* name, UINT w, UINT h) {
        if (texture) {
            const D3D12_RESOURCE_DESC d = texture->GetDesc();
            if (d.Width == w && d.Height == h && d.Format == f) return true;
            if (!wait_idle()) return false;
            texture.Reset();
        }
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = f;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const HRESULT hr = s.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                             nullptr, IID_PPV_ARGS(&texture));
        if (FAILED(hr)) {
            LOG("present: the %s texture %ux%u could not be made (0x%08lX)", name, w, h, static_cast<unsigned long>(hr));
            return false;
        }
        return true;
    };
    return make(s.motion, DXGI_FORMAT_R16G16_FLOAT, "motion", depth_width, depth_height) && make(s.ui, format, "UI layer", width, height);
}

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
        s.device->CreateShaderResourceView(r, &d, h);
    };
    srv(t0, t0_format, 0);
    srv(t1, t1_format, 1);
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = u0_format;
    u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
    h.ptr += SIZE_T(2) * s.descriptor_size;
    s.device->CreateUnorderedAccessView(u0, nullptr, &u, h);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = s.heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(set) * kSetDescriptors * s.descriptor_size;
    return gpu;
}

void record_passes(UINT slot, const framegen::Camera& c, float depth_max, UINT width, UINT height, DXGI_FORMAT format,
                   UINT depth_width, UINT depth_height) {
    ID3D12GraphicsCommandList* list = s.list.Get();
    ID3D12DescriptorHeap* heaps[] = {s.heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(s.pass_signature.Get());
    float move[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    move[12] = float(c.delta[0]);
    move[13] = float(c.delta[1]);
    move[14] = float(c.delta[2]);
    float world[16], clip_to_prev[16];
    multiply(c.inv_view_projection, move, world);
    multiply(world, c.prev_view_projection, clip_to_prev);
    uint32_t constants[kPassConstants] = {};
    memcpy(constants, clip_to_prev, sizeof(clip_to_prev));
    constants[16] = depth_width;
    constants[17] = depth_height;
    memcpy(&constants[18], &depth_max, sizeof(float));
    list->SetPipelineState(s.motion_pso.Get());
    list->SetComputeRoot32BitConstants(0, kPassConstants, constants, 0);
    list->SetComputeRootDescriptorTable(1, write_set(slot * 2, s.depth[slot].Get(), DXGI_FORMAT_R32_FLOAT, nullptr,
                                                     DXGI_FORMAT_R32_FLOAT, s.motion.Get(), DXGI_FORMAT_R16G16_FLOAT));
    list->Dispatch((depth_width + 7) / 8, (depth_height + 7) / 8, 1);
    const uint32_t size[2] = {width, height};
    list->SetPipelineState(s.ui_pso.Get());
    list->SetComputeRoot32BitConstants(0, 2, size, 0);
    list->SetComputeRootDescriptorTable(1, write_set(slot * 2 + 1, s.frame[slot].Get(), format, s.hudless[slot].Get(), format,
                                                     s.ui.Get(), format));
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &uav);
}

framegen::Camera camera_of(const present_ipc::Camera& in) {
    framegen::Camera c{};
    memcpy(c.projection, in.projection, sizeof(c.projection));
    memcpy(c.inv_projection, in.inv_projection, sizeof(c.inv_projection));
    memcpy(c.view_projection, in.view_projection, sizeof(c.view_projection));
    memcpy(c.inv_view_projection, in.inv_view_projection, sizeof(c.inv_view_projection));
    memcpy(c.prev_view_projection, in.prev_view_projection, sizeof(c.prev_view_projection));
    memcpy(c.prev_inv_view_projection, in.prev_inv_view_projection, sizeof(c.prev_inv_view_projection));
    memcpy(c.right, in.right, sizeof(c.right));
    memcpy(c.up, in.up, sizeof(c.up));
    memcpy(c.forward, in.forward, sizeof(c.forward));
    memcpy(c.delta, in.delta, sizeof(c.delta));
    c.near_clip = in.near_clip;
    c.far_clip = in.far_clip;
    c.fov_y = in.fov_y;
    c.aspect = in.aspect;
    c.depth_inverted = in.depth_inverted != 0;
    c.reset = in.reset != 0;
    return c;
}

void report() {
    if (ms_since(s.report_from) < kReportSeconds * 1000.0) return;
    const double seconds = ms_since(s.report_from) / 1000.0;
    if (s.presents)
        LOG("present over %.1f s: %llu frames presented (%llu with frame generation's inputs, %llu occluded), our Present "
            "%.2f ms avg / %.1f max; the focus asked %llu times, answered as ours %llu",
            seconds, static_cast<unsigned long long>(s.presents), static_cast<unsigned long long>(s.with_inputs),
            static_cast<unsigned long long>(s.occluded), s.present_ms_sum / double(s.presents), s.present_ms_max,
            static_cast<unsigned long long>(g_asked.load()), static_cast<unsigned long long>(g_answered.load()));
    if (s.streamline && s.presents) framegen::report(seconds);
    s.presents = s.with_inputs = s.occluded = 0;
    s.present_ms_sum = s.present_ms_max = 0.0;
    QueryPerformanceCounter(&s.report_from);
}

// The frame the plugin handed over: through our chain, with frame generation's inputs when it has them.
bool present_frame() {
    present_ipc::Block& b = *g_block;
    const UINT slot = b.slot % kSlots;
    const UINT width = b.width, height = b.height;
    const DXGI_FORMAT format = DXGI_FORMAT(b.format);
    if (!ensure_chain(width, height, format) || !open_slot(slot)) return false;
    if (!wait_for(s.allocator_done[slot])) {
        fail("our queue is too far behind");
        return false;
    }
    ComPtr<ID3D12Resource> target;
    HRESULT hr = s.chain->GetBuffer(s.chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&target));
    if (SUCCEEDED(hr)) hr = s.allocators[slot]->Reset();
    if (SUCCEEDED(hr)) hr = s.list->Reset(s.allocators[slot].Get(), nullptr);
    if (FAILED(hr)) {
        fail("our list could not be recorded");
        return false;
    }
    ID3D12GraphicsCommandList* list = s.list.Get();
    ID3D12Resource* frame = s.frame[slot].Get();
    const DXGI_FORMAT ours = chain_format(format);
    transition(list, frame, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list, target.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(target.Get(), frame);
    transition(list, target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    bool inputs = false;
    if (s.streamline) {
        const bool have = b.has_inputs != 0 && s.hudless[slot] && s.depth[slot];
        const UINT depth_width = b.slots[slot].depth_width ? b.slots[slot].depth_width : width;
        const UINT depth_height = b.slots[slot].depth_height ? b.slots[slot].depth_height : height;
        framegen::begin_frame(have, width, height, depth_width, depth_height);
        if (have && framegen::enabled() && ensure_passes(width, height, ours, depth_width, depth_height)) {
            ID3D12Resource* hudless = s.hudless[slot].Get();
            ID3D12Resource* depth = s.depth[slot].Get();
            const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            transition(list, frame, D3D12_RESOURCE_STATE_COPY_SOURCE, srv);
            transition(list, hudless, D3D12_RESOURCE_STATE_COMMON, srv);
            transition(list, depth, D3D12_RESOURCE_STATE_COMMON, srv);
            const framegen::Camera camera = camera_of(b.camera);
            record_passes(slot, camera, b.depth_max, width, height, ours, depth_width, depth_height);
            framegen::Inputs in;
            in.depth = depth;
            in.motion = s.motion.Get();
            in.hudless = hudless;
            in.ui = s.ui.Get();
            in.width = width;
            in.height = height;
            in.depth_width = depth_width;
            in.depth_height = depth_height;
            inputs = framegen::set_inputs(list, camera, in);
            transition(list, depth, srv, D3D12_RESOURCE_STATE_COMMON);
            transition(list, hudless, srv, D3D12_RESOURCE_STATE_COMMON);
            transition(list, frame, srv, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
    }
    transition(list, frame, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (FAILED(list->Close())) {
        fail("our list could not be closed");
        return false;
    }
    s.queue->Wait(s.ready.Get(), b.ready_value);  // the plugin's copies into the slot
    ID3D12CommandList* lists[] = {list};
    s.queue->ExecuteCommandLists(1, lists);
    const UINT64 done = ++s.done_value;
    s.queue->Signal(s.done.Get(), done);
    s.allocator_done[slot] = done;
    b.slot_done[slot] = done;
    target.Reset();
    if (s.streamline) framegen::before_present();
    LARGE_INTEGER present_start;
    QueryPerformanceCounter(&present_start);
    const UINT interval = b.sync_interval;
    hr = s.chain->Present(interval, interval == 0 && s.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    const double present_ms = ms_since(present_start);
    ++s.chain_presents;
    s.present_ms_sum += present_ms;
    if (present_ms > s.present_ms_max) s.present_ms_max = present_ms;
    if (s.streamline) {
        framegen::after_present();
        // a present held this long while DLSS-G generates is its pacing stalled: off before it turns into a GPU timeout
        // (not a new chain's first presents, nor once as inputs start)
        if (present_ms > kStalledPresentMs && framegen::generating() && s.chain_presents > 3) {
            if (s.long_present_allowed) {
                s.long_present_allowed = false;
                LOG("present: a present through Streamline took %.0f ms as frame generation started", present_ms);
            } else {
                framegen::set_wanted(false);
                LOG("present: a present through Streamline took %.0f ms with frame generation on: frame generation is turned off",
                    present_ms);
            }
        }
    }
    if (FAILED(hr) && hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) {
        char reason[96];
        snprintf(reason, sizeof(reason), "our Present failed (0x%08lX)", static_cast<unsigned long>(hr));
        fail(reason);
        return false;
    }
    if (hr == DXGI_STATUS_OCCLUDED) ++s.occluded;
    ++s.presents;
    if (inputs) ++s.with_inputs;
    b.presents = b.presents + 1;
    if (inputs) b.with_inputs = b.with_inputs + 1;
    b.generating = s.streamline && framegen::generating() ? 1 : 0;
    // ours shown over the game's client area once it presented, fitted again when that area changed
    RECT client{}, mine{};
    GetClientRect(g_parent, &client);
    GetClientRect(g_child.load(), &mine);
    if (!s.shown || client.right != mine.right || client.bottom != mine.bottom) {
        PostMessageW(g_child.load(), kFitMessage, 0, 0);
        s.shown = true;
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    QueryPerformanceFrequency(&g_frequency);
    QueryPerformanceCounter(&s.report_from);
    if (argc < 2) return 2;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, argv[1]);
    if (!mapping) return 3;
    g_block = static_cast<present_ipc::Block*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(present_ipc::Block)));
    if (!g_block || g_block->version != present_ipc::kVersion || g_block->size != sizeof(present_ipc::Block)) return 4;
    present_ipc::Block& b = *g_block;
    b.helper_pid = GetCurrentProcessId();
    logx::open(b.log_path);
    g_parent = reinterpret_cast<HWND>(b.game_window);
    LOG("rfgrt_present %s: the 32-bit plugin's presenter for process %lu (its window %p)", RFGRT_VERSION, b.game_pid,
        static_cast<void*>(g_parent));
    const HANDLE frame_event = reinterpret_cast<HANDLE>(b.frame_event);
    const HANDLE done_event = reinterpret_cast<HANDLE>(b.done_event);
    if (!hook_focus()) LOG("present: GetForegroundWindow could not be hooked: DLSS-G will see the game's window as another process's");
    g_child_made = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 0, &window_thread, nullptr, 0, nullptr)) CloseHandle(t);
    WaitForSingleObject(g_child_made, 10000);
    if (!g_child.load() || !set_up()) {
        fail("our window or device could not be made");
        SetEvent(done_event);
        return 5;
    }
    b.helper_state = 1;
    LOG("present: ready");
    for (;;) {
        const DWORD w = WaitForSingleObject(frame_event, 500);
        if (b.quit || !IsWindow(g_parent)) break;
        const bool wanted = b.wanted != 0;
        if (wanted != s.generation_wanted) {
            s.generation_wanted = wanted;
            framegen::set_wanted(wanted);
            if (!wanted && s.shown) {
                PostMessageW(g_child.load(), kHideMessage, 0, 0);
                s.shown = false;
                if (s.streamline) framegen::suspend();
                LOG("present: frame generation off: the game presents, our swap chain kept");
            }
        }
        if (w == WAIT_OBJECT_0) {
            // a frame, or (frame generation just turned off) only the wake-up for the change above: acknowledged either way
            if (wanted) {
                if (b.helper_state == 1 && !present_frame()) b.helper_state = 2;
                b.presented = b.frame;
            }
            SetEvent(done_event);
        }
        report();
        if (b.helper_state == 2) break;
    }
    LOG("present: done (%s)", b.quit ? "the plugin asked" : b.helper_state == 2 ? "broken" : "the game's window is gone");
    if (s.queue && s.done) wait_idle();
    if (const HWND child = g_child.load()) PostMessageW(child, WM_CLOSE, 0, 0);
    return 0;
}
