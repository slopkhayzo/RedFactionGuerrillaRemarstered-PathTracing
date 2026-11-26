#include "rt/link.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/log.h"

#if defined(RFGRT_LOCAL_CORE)
#include "rtcore.h"
#endif

namespace rtlink {
namespace {

template <typename T>
void rel(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void close_handle(void*& h) {
    if (h) CloseHandle(static_cast<HANDLE>(h));
    h = nullptr;
}

uint64_t to_u64(HANDLE h) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h)); }
HANDLE to_handle(uint64_t v) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(v)); }

constexpr uint64_t kBlockBytes = (sizeof(rfgrt_ipc::Block) + 4095) & ~uint64_t(4095);

#if defined(RFGRT_LOCAL_CORE)
void core_log(const char* line) { LOG("core: %s", line); }
// In-process: the core's fence B handle, duplicated so that the plugin's copy is independent of the core's.
bool publish_in_process(HANDLE fence_b, rfgrt_ipc::Block* block, void*) {
    HANDLE dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), fence_b, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return false;
    block->fence_b_handle = to_u64(dup);
    return true;
}
DWORD WINAPI core_thread_main(LPVOID param) {
    rtcore::RunParams p;
    p.block = static_cast<rfgrt_ipc::Block*>(param);
    p.publish_fence_b = &publish_in_process;
    p.log = &core_log;
    return DWORD(rtcore::run(p));
}
#endif

}  // namespace

Link& core_link() {
    static Link link;
    return link;
}

void Link::set_dlss_mode(int mode) {
    std::lock_guard<std::mutex> lock(start_mutex_);  // stop() frees the block under it
    opt_.dlss_mode = mode;
    if (block_) InterlockedExchange(reinterpret_cast<volatile LONG*>(&block_->dlss_mode), mode);
}

bool Link::start(ID3D11Device* device) {
    if (started_.load()) return true;
    std::lock_guard<std::mutex> lock(start_mutex_);
    if (started_.load()) return true;
    if (start_failed_ || !device) return false;
    if (!start_locked(device)) {
        start_failed_ = true;
        dead_ = true;
        return false;
    }
    started_ = true;
    return true;
}

bool Link::start_locked(ID3D11Device* device) {
    device_ = device;
    ID3D11Device5* dev5 = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dev5)))) {
        LOG("link: the game's device has no ID3D11Device5 (fences): the core cannot be used");
        return false;
    }
    HRESULT hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_a_));
    rel(dev5);
    HANDLE fa = nullptr;
    if (SUCCEEDED(hr)) hr = fence_a_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fa);
    fence_a_nt_ = fa;
    if (FAILED(hr)) {
        LOG("link: creating fence A failed 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }
    if (!create_region()) return false;
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    scene_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!(opt_.in_process ? start_thread() : start_process())) return false;
    monitor_ = CreateThread(nullptr, 64 * 1024, &Link::monitor_main, this, 0, nullptr);
    return true;
}

namespace {
uint64_t ring_bytes_for(uint32_t ring_mb) {
    uint64_t ring = 1ull << 20;
    while (ring < uint64_t(ring_mb) << 20 && ring < (1ull << 30)) ring <<= 1;
    return ring;
}
}  // namespace

uint64_t Link::ring_max_record_planned() const { return ring_bytes_for(opt_.ring_mb) / 2; }

bool Link::create_region() {
    const uint64_t ring = ring_bytes_for(opt_.ring_mb);
    const uint32_t max_instances = opt_.snapshots ? opt_.max_instances : 0;
    const uint32_t max_skins = opt_.snapshots && opt_.max_bones ? opt_.max_skins : 0;
    const uint32_t max_bones = max_skins ? opt_.max_bones : 0;
    const uint64_t slot_bytes =
        opt_.snapshots ? (rfgrt_frame::slot_bytes(max_instances, max_skins, max_bones) + 4095) & ~uint64_t(4095) : 0;
    const uint64_t frames_offset = kBlockBytes + ring;
    const uint64_t bytes = frames_offset + slot_bytes * rfgrt_frame::kSlots;
    if (opt_.in_process) {
        region_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, SIZE_T(bytes), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!region_) {
            LOG("link: allocating the shared region (%llu MB) failed", static_cast<unsigned long long>(bytes >> 20));
            return false;
        }
    } else {
        wchar_t name[96];
        swprintf(name, 96, L"Local\\rfgrt_%lu_%llu", GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()));
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(bytes >> 32), DWORD(bytes), name);
        if (!mapping) {
            LOG("link: CreateFileMapping (%llu MB) failed (%lu)", static_cast<unsigned long long>(bytes >> 20), GetLastError());
            return false;
        }
        mapping_ = mapping;
        region_ = static_cast<uint8_t*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, SIZE_T(bytes)));
        if (!region_) {
            LOG("link: mapping the shared region (%llu MB) failed (%lu)", static_cast<unsigned long long>(bytes >> 20), GetLastError());
            return false;
        }
        wcsncpy_s(region_name_, name, _TRUNCATE);
    }
    block_ = reinterpret_cast<rfgrt_ipc::Block*>(region_);
    memset(block_, 0, sizeof(*block_));
    block_->magic = rfgrt_ipc::kMagic;
    block_->version = rfgrt_ipc::kVersion;
    block_->block_bytes = sizeof(rfgrt_ipc::Block);
    block_->plugin_pid = GetCurrentProcessId();
    block_->mode = opt_.mode;
    block_->region_bytes = bytes;
    block_->ring_offset = kBlockBytes;
    block_->ring_bytes = ring;
    block_->frames_offset = frames_offset;
    block_->frame_slot_bytes = slot_bytes;
    block_->max_instances = max_instances;
    block_->max_skins = max_skins;
    block_->max_bones = max_bones;
    block_->snapshots = opt_.snapshots ? 1 : 0;
    block_->shadow_rays = opt_.shadow_rays;
    block_->shadow_sun_angle_deg = opt_.shadow_sun_angle;
    block_->shadow_filter_radius = opt_.shadow_filter_radius;
    block_->shadow_history = opt_.shadow_history;
    block_->ao_rays = opt_.ao_rays;
    block_->light_masks = opt_.light_masks;
    block_->gi_bounce = opt_.gi_bounce;
    block_->gi_sun_bounce_albedo = opt_.gi_sun_bounce_albedo;
    block_->gi_shade_bounce_albedo = opt_.gi_shade_bounce_albedo;
    block_->gi_sky = opt_.gi_sky;
    block_->gi_cache = opt_.gi_cache ? 1 : 0;
    block_->gi_denoiser = opt_.gi_denoiser;
    block_->dlss_mode = opt_.dlss_mode;
    block_->host_dlss_mode = -1;
    block_->fog_enabled = opt_.fog && opt_.gi_rays > 0 ? 1 : 0;
    block_->fog_sun_share = opt_.fog_sun_share;
    block_->fog_anisotropy = opt_.fog_anisotropy;
    block_->fog_density = opt_.fog_density;
    block_->fog_near = opt_.fog_near;
    block_->fog_history = opt_.fog_history;
    block_->fog_rays = opt_.fog_rays;
    block_->fog_filter = opt_.fog_filter ? 1 : 0;
    block_->gi_sky_drawn = opt_.gi_sky_drawn;
    block_->refl_enabled = opt_.refl_enabled ? 1 : 0;
    block_->refl_length = opt_.refl_length;
    block_->refl_gloss = opt_.refl_gloss;
    block_->refl_history = opt_.refl_history;
    block_->refl_filter_radius = opt_.refl_filter_radius;
    block_->refl_glass = opt_.refl_glass && opt_.gi_glass ? 1 : 0;
    block_->refl_glass_f0 = opt_.refl_glass_f0;
    block_->frosted_glass = opt_.frosted_glass ? 1 : 0;
    block_->window_tint = opt_.window_tint ? 1 : 0;
    block_->tlas_fast_trace = opt_.tlas_fast_trace ? 1 : 0;
    block_->dred = opt_.dred ? 1 : 0;
    for (int i = 0; i < 3; ++i) block_->terrain_albedo[i] = opt_.terrain_albedo[i];
    block_->ao_radius = opt_.ao_radius;
    block_->debug_view = opt_.debug_view;
    block_->gi_rays = opt_.gi_rays;
    block_->gi_length = opt_.gi_length;
    block_->gi_albedo = opt_.gi_albedo;
    block_->gi_history = opt_.gi_history;
    block_->gi_filter_radius = opt_.gi_filter_radius;
    wcsncpy_s(block_->log_path, opt_.log_path.c_str(), _TRUNCATE);
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC ad{};
    if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter))) adapter->GetDesc(&ad);
    rel(adapter);
    rel(dxgi);
    block_->adapter_luid_low = ad.AdapterLuid.LowPart;
    block_->adapter_luid_high = ad.AdapterLuid.HighPart;
    writer_.attach(block_, region_ + kBlockBytes);
    return true;
}

bool Link::start_process() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    job_ = job;
    std::wstring cmd = L"\"" + opt_.host_exe + L"\" " + region_name_;
    std::wstring dir = opt_.host_exe.substr(0, opt_.host_exe.find_last_of(L"\\/"));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(opt_.host_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                        dir.c_str(), &si, &pi)) {
        LOG("link: starting %ls failed (%lu)", opt_.host_exe.c_str(), GetLastError());
        return false;
    }
    process_ = pi.hProcess;
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) LOG("link: AssignProcessToJobObject failed (%lu)", GetLastError());
    HANDLE fa = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(fence_a_nt_), pi.hProcess, &fa, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        LOG("link: DuplicateHandle (fence A) into the host failed (%lu)", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        return false;
    }
    block_->fence_a_handle = to_u64(fa);
    HANDLE se = nullptr;
    if (scene_event_ && DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(scene_event_), pi.hProcess, &se, 0, FALSE,
                                        DUPLICATE_SAME_ACCESS))
        block_->scene_event_handle = to_u64(se);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    LOG("link: rfgrt_host.exe started (pid %lu), scene ring %llu MB", pi.dwProcessId,
        static_cast<unsigned long long>(block_->ring_bytes >> 20));
    return true;
}

bool Link::start_thread() {
#if defined(RFGRT_LOCAL_CORE)
    HANDLE fa = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(fence_a_nt_), GetCurrentProcess(), &fa, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return false;
    block_->fence_a_handle = to_u64(fa);
    HANDLE se = nullptr;
    if (scene_event_ && DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(scene_event_), GetCurrentProcess(), &se, 0,
                                        FALSE, DUPLICATE_SAME_ACCESS))
        block_->scene_event_handle = to_u64(se);
    core_thread_ = CreateThread(nullptr, 0, &core_thread_main, block_, 0, nullptr);
    if (!core_thread_) return false;
    LOG("link: the core runs on a thread of this process, scene ring %llu MB", static_cast<unsigned long long>(block_->ring_bytes >> 20));
    return true;
#else
    LOG("link: this build has no in-process core (32-bit processes get no hardware ray tracing)");
    return false;
#endif
}

bool Link::ready() {
    if (!started_.load() || dead_.load() || fence_b_failed_) return false;
    if (fence_b_) return true;
    if (block_->host_state != rfgrt_ipc::kHostReady) return false;
    ID3D11Device5* dev5 = nullptr;
    HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(&dev5));
    if (SUCCEEDED(hr)) hr = dev5->OpenSharedFence(to_handle(block_->fence_b_handle), IID_PPV_ARGS(&fence_b_));
    rel(dev5);
    if (FAILED(hr)) {
        LOG("link: opening fence B failed 0x%08lX: the game's own shadows", static_cast<unsigned long>(hr));
        fence_b_failed_ = true;
        return false;
    }
    LOG("link: the core is ready (%s)", opt_.in_process ? "in this process" : "rfgrt_host.exe");
    return true;
}

rfgrt_frame::SlotHeader* Link::frame_slot(int64_t frame) {
    if (!block_ || !block_->snapshots || frame <= 0) return nullptr;
    return reinterpret_cast<rfgrt_frame::SlotHeader*>(region_ + block_->frames_offset +
                                                      uint64_t(frame % rfgrt_frame::kSlots) * block_->frame_slot_bytes);
}

void Link::frame_published(int64_t frame) {
    rfgrt_ipc::store64(&block_->frame_published, frame);
    if (scene_event_) SetEvent(static_cast<HANDLE>(scene_event_));
}

bool Link::ensure_frame_resources(uint32_t w, uint32_t h, int64_t first_frame) {
    if (depth_ && w == width_ && h == height_) return true;
    // One switch at a time: the core takes the set it was given before it gets another (its handles would leak, and
    // it could read a half-written set).
    if (generation_ && block_->host_generation != generation_) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *depth = nullptr, *result = nullptr, *game_clb = nullptr, *irradiance = nullptr;
    ID3D11RenderTargetView* depth_rtv = nullptr;
    ID3D11ShaderResourceView *result_srv = nullptr, *irradiance_srv = nullptr;
    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &depth);
    if (SUCCEEDED(hr)) {
        td.Format = DXGI_FORMAT_R8G8_UNORM;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        hr = device_->CreateTexture2D(&td, nullptr, &result);
    }
    if (SUCCEEDED(hr)) {
        td.Format = DXGI_FORMAT_R8G8_UNORM;  // ClbTexture's own format: CopyResource needs it the same
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = device_->CreateTexture2D(&td, nullptr, &game_clb);
    }
    ID3D11Texture2D* reflection = nullptr;
    ID3D11ShaderResourceView* reflection_srv = nullptr;
    if (SUCCEEDED(hr) && opt_.refl_enabled) {  // the reflections: the core writes them, the patched lit shaders read them
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        hr = device_->CreateTexture2D(&td, nullptr, &reflection);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(reflection, nullptr, &reflection_srv);
    }
    if (SUCCEEDED(hr) && opt_.gi_rays > 0) {  // the irradiance: the core writes it, the patched lit shaders read it
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        hr = device_->CreateTexture2D(&td, nullptr, &irradiance);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(irradiance, nullptr, &irradiance_srv);
    }
    ID3D11Texture2D* glass = nullptr;
    ID3D11ShaderResourceView* glass_srv = nullptr;
    ID3D11Texture2D* fog = nullptr;
    ID3D11ShaderResourceView* fog_srv = nullptr;
    if (SUCCEEDED(hr) && opt_.gi_rays > 0 && opt_.fog) {  // the fog: the core writes it, the plugin blends it over the lit scene
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        hr = device_->CreateTexture2D(&td, nullptr, &fog);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(fog, nullptr, &fog_srv);
    }
    if (SUCCEEDED(hr) && opt_.gi_rays > 0 && opt_.gi_glass) {  // the glass's light: the core writes it, the glass shader reads it
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        hr = device_->CreateTexture2D(&td, nullptr, &glass);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(glass, nullptr, &glass_srv);
    }
    ID3D11Texture2D* glass_refl = nullptr;
    ID3D11ShaderResourceView* glass_refl_srv = nullptr;
    if (SUCCEEDED(hr) && glass && opt_.refl_glass) {  // what the clear glass reflects: the core writes it, the glass shader reads it
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        hr = device_->CreateTexture2D(&td, nullptr, &glass_refl);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(glass_refl, nullptr, &glass_refl_srv);
    }
    ID3D11Texture2D* sun_tint = nullptr;
    ID3D11ShaderResourceView* sun_tint_srv = nullptr;
    if (SUCCEEDED(hr) && opt_.gi_rays > 0 && opt_.window_tint) {  // the sun through clear glass: the core writes it, the lit shaders read it
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        hr = device_->CreateTexture2D(&td, nullptr, &sun_tint);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(sun_tint, nullptr, &sun_tint_srv);
    }
    // The dynamic lights' shadow masks (the core writes them, the light passes read them at t15) and a white one for
    // the lights without (the patched light passes ld it at the pixel: full size).
    const int masks = opt_.light_masks < 0 ? 0 : opt_.light_masks > int(rfgrt_ipc::kMaxLightMasks) ? int(rfgrt_ipc::kMaxLightMasks) : opt_.light_masks;
    ID3D11Texture2D* mask[rfgrt_ipc::kMaxLightMasks] = {};
    ID3D11ShaderResourceView* mask_srv[rfgrt_ipc::kMaxLightMasks] = {};
    HANDLE mask_nt[rfgrt_ipc::kMaxLightMasks] = {}, mask_remote[rfgrt_ipc::kMaxLightMasks] = {};
    ID3D11Texture2D* white = nullptr;
    ID3D11ShaderResourceView* white_srv = nullptr;
    for (int i = 0; i < masks && SUCCEEDED(hr); ++i) {
        td.Format = DXGI_FORMAT_R8G8_UNORM;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        hr = device_->CreateTexture2D(&td, nullptr, &mask[i]);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(mask[i], nullptr, &mask_srv[i]);
    }
    if (SUCCEEDED(hr) && masks > 0) {
        D3D11_TEXTURE2D_DESC wd = td;
        wd.Format = DXGI_FORMAT_R8G8_UNORM;
        wd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        wd.MiscFlags = 0;
        wd.Usage = D3D11_USAGE_IMMUTABLE;
        std::vector<uint8_t> ones(size_t(w) * h * 2, 0xFF);
        D3D11_SUBRESOURCE_DATA init{ones.data(), w * 2, 0};
        hr = device_->CreateTexture2D(&wd, &init, &white);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(white, nullptr, &white_srv);
    }
    if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(depth, nullptr, &depth_rtv);
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(result, nullptr, &result_srv);
    auto share = [](ID3D11Resource* r, HANDLE* out) {
        IDXGIResource1* r1 = nullptr;
        if (FAILED(r->QueryInterface(IID_PPV_ARGS(&r1)))) return E_NOINTERFACE;
        HRESULT res = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, out);
        r1->Release();
        return res;
    };
    HANDLE depth_nt = nullptr, result_nt = nullptr, depth_remote = nullptr, result_remote = nullptr;
    HANDLE game_clb_nt = nullptr, game_clb_remote = nullptr, irradiance_nt = nullptr, irradiance_remote = nullptr;
    HANDLE reflection_nt = nullptr, reflection_remote = nullptr, glass_nt = nullptr, glass_remote = nullptr;
    HANDLE fog_nt = nullptr, fog_remote = nullptr, glass_refl_nt = nullptr, glass_refl_remote = nullptr;
    HANDLE sun_tint_nt = nullptr, sun_tint_remote = nullptr;
    if (SUCCEEDED(hr) && sun_tint) hr = share(sun_tint, &sun_tint_nt);
    if (SUCCEEDED(hr) && sun_tint &&
        !DuplicateHandle(GetCurrentProcess(), sun_tint_nt, process_ ? static_cast<HANDLE>(process_) : GetCurrentProcess(),
                         &sun_tint_remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
        hr = HRESULT_FROM_WIN32(GetLastError());
    if (SUCCEEDED(hr) && glass_refl) hr = share(glass_refl, &glass_refl_nt);
    if (SUCCEEDED(hr) && glass_refl &&
        !DuplicateHandle(GetCurrentProcess(), glass_refl_nt, process_ ? static_cast<HANDLE>(process_) : GetCurrentProcess(),
                         &glass_refl_remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
        hr = HRESULT_FROM_WIN32(GetLastError());
    if (SUCCEEDED(hr) && fog) hr = share(fog, &fog_nt);
    if (SUCCEEDED(hr) && fog &&
        !DuplicateHandle(GetCurrentProcess(), fog_nt, process_ ? static_cast<HANDLE>(process_) : GetCurrentProcess(),
                         &fog_remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
        hr = HRESULT_FROM_WIN32(GetLastError());
    if (SUCCEEDED(hr) && reflection) hr = share(reflection, &reflection_nt);
    if (SUCCEEDED(hr) && reflection &&
        !DuplicateHandle(GetCurrentProcess(), reflection_nt, process_ ? static_cast<HANDLE>(process_) : GetCurrentProcess(),
                         &reflection_remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
        hr = HRESULT_FROM_WIN32(GetLastError());
    if (SUCCEEDED(hr) && glass) hr = share(glass, &glass_nt);
    if (SUCCEEDED(hr) && glass &&
        !DuplicateHandle(GetCurrentProcess(), glass_nt, process_ ? static_cast<HANDLE>(process_) : GetCurrentProcess(),
                         &glass_remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
        hr = HRESULT_FROM_WIN32(GetLastError());
    if (SUCCEEDED(hr) && irradiance) hr = share(irradiance, &irradiance_nt);
    if (SUCCEEDED(hr) && irradiance &&
        !DuplicateHandle(GetCurrentProcess(), irradiance_nt, process_ ? static_cast<HANDLE>(process_) : GetCurrentProcess(),
                         &irradiance_remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
        hr = HRESULT_FROM_WIN32(GetLastError());
    for (int i = 0; i < masks && SUCCEEDED(hr); ++i) {
        hr = share(mask[i], &mask_nt[i]);
        if (SUCCEEDED(hr) && !DuplicateHandle(GetCurrentProcess(), mask_nt[i], process_ ? static_cast<HANDLE>(process_) : GetCurrentProcess(),
                                              &mask_remote[i], 0, FALSE, DUPLICATE_SAME_ACCESS))
            hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr)) hr = share(depth, &depth_nt);
    if (SUCCEEDED(hr)) hr = share(result, &result_nt);
    if (SUCCEEDED(hr)) hr = share(game_clb, &game_clb_nt);
    // The core gets its own handles and closes them once it has opened the textures.
    HANDLE target = process_ ? static_cast<HANDLE>(process_) : GetCurrentProcess();
    if (SUCCEEDED(hr) &&
        (!DuplicateHandle(GetCurrentProcess(), depth_nt, target, &depth_remote, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
         !DuplicateHandle(GetCurrentProcess(), result_nt, target, &result_remote, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
         !DuplicateHandle(GetCurrentProcess(), game_clb_nt, target, &game_clb_remote, 0, FALSE, DUPLICATE_SAME_ACCESS)))
        hr = HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr)) {
        LOG("link: making the frame textures (%ux%u) failed 0x%08lX", w, h, static_cast<unsigned long>(hr));
        rel(result_srv);
        rel(irradiance_srv);
        rel(irradiance);
        if (irradiance_nt) CloseHandle(irradiance_nt);
        rel(reflection_srv);
        rel(reflection);
        if (reflection_nt) CloseHandle(reflection_nt);
        rel(glass_srv);
        rel(glass);
        if (glass_nt) CloseHandle(glass_nt);
        rel(glass_refl_srv);
        rel(glass_refl);
        if (glass_refl_nt) CloseHandle(glass_refl_nt);
        rel(sun_tint_srv);
        rel(sun_tint);
        if (sun_tint_nt) CloseHandle(sun_tint_nt);
        rel(fog_srv);
        rel(fog);
        if (fog_nt) CloseHandle(fog_nt);
        for (int i = 0; i < masks; ++i) {
            rel(mask_srv[i]);
            rel(mask[i]);
            if (mask_nt[i]) CloseHandle(mask_nt[i]);
        }
        rel(white_srv);
        rel(white);
        rel(depth_rtv);
        rel(game_clb);
        rel(result);
        rel(depth);
        if (depth_nt) CloseHandle(depth_nt);
        if (result_nt) CloseHandle(result_nt);
        if (game_clb_nt) CloseHandle(game_clb_nt);
        return false;
    }
    release_frame_resources();
    depth_ = depth;
    result_ = result;
    depth_rtv_ = depth_rtv;
    result_srv_ = result_srv;
    depth_nt_ = depth_nt;
    result_nt_ = result_nt;
    game_clb_ = game_clb;
    game_clb_nt_ = game_clb_nt;
    irradiance_ = irradiance;
    irradiance_srv_ = irradiance_srv;
    irradiance_nt_ = irradiance_nt;
    reflection_ = reflection;
    reflection_srv_ = reflection_srv;
    reflection_nt_ = reflection_nt;
    glass_ = glass;
    glass_srv_ = glass_srv;
    glass_nt_ = glass_nt;
    glass_refl_ = glass_refl;
    glass_refl_srv_ = glass_refl_srv;
    glass_refl_nt_ = glass_refl_nt;
    sun_tint_ = sun_tint;
    sun_tint_srv_ = sun_tint_srv;
    sun_tint_nt_ = sun_tint_nt;
    fog_ = fog;
    fog_srv_ = fog_srv;
    fog_nt_ = fog_nt;
    light_mask_count_ = masks;
    for (int i = 0; i < masks; ++i) {
        light_mask_[i] = mask[i];
        light_mask_srv_[i] = mask_srv[i];
        light_mask_nt_[i] = mask_nt[i];
    }
    white_ = white;
    white_srv_ = white_srv;
    width_ = w;
    height_ = h;
    block_->width = w;
    block_->height = h;
    block_->depth_handle = to_u64(depth_remote);
    block_->result_handle = to_u64(result_remote);
    block_->game_clb_handle = to_u64(game_clb_remote);
    block_->irradiance_handle = to_u64(irradiance_remote);
    block_->reflection_handle = to_u64(reflection_remote);
    block_->glass_handle = to_u64(glass_remote);
    block_->glass_refl_handle = to_u64(glass_refl_remote);
    block_->sun_tint_handle = to_u64(sun_tint_remote);
    block_->fog_handle = to_u64(fog_remote);
    for (uint32_t i = 0; i < rfgrt_ipc::kMaxLightMasks; ++i) block_->light_mask_handles[i] = to_u64(mask_remote[i]);
    rfgrt_ipc::store64(&block_->generation_first_frame, first_frame);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block_->frame_generation), ++generation_);
    LOG("link: frame textures %ux%u (set %d, from frame %lld)", w, h, generation_, static_cast<long long>(first_frame));
    return true;
}

uint64_t Link::share_with_core(ID3D11Resource* resource, HRESULT* hr_out) {
    HRESULT hr = E_FAIL;
    HANDLE nt = nullptr, remote = nullptr;
    if (resource && started_.load() && !dead_.load()) {
        IDXGIResource1* r1 = nullptr;
        hr = resource->QueryInterface(IID_PPV_ARGS(&r1));
        if (SUCCEEDED(hr)) hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &nt);
        if (r1) r1->Release();
        if (SUCCEEDED(hr) && !DuplicateHandle(GetCurrentProcess(), nt, process_ ? static_cast<HANDLE>(process_) : GetCurrentProcess(),
                                              &remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
            hr = HRESULT_FROM_WIN32(GetLastError());
        if (nt) CloseHandle(nt);
    }
    if (hr_out) *hr_out = hr;
    return SUCCEEDED(hr) ? to_u64(remote) : 0;
}

bool Link::capture_radiance(ID3D11DeviceContext* ctx, ID3D11Texture2D* target, int64_t frame) {
    if (!ctx || !target || !block_ || !started_.load() || dead_.load() || radiance_failed_) return false;
    D3D11_TEXTURE2D_DESC sd{};
    target->GetDesc(&sd);
    if (!radiance_ || sd.Width != radiance_w_ || sd.Height != radiance_h_ || uint32_t(sd.Format) != radiance_format_) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = sd.Width;
        d.Height = sd.Height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = sd.Format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        ID3D11Texture2D* tex = nullptr;
        HANDLE nt = nullptr, remote = nullptr;
        IDXGIResource1* r1 = nullptr;
        HRESULT hr = device_->CreateTexture2D(&d, nullptr, &tex);
        if (SUCCEEDED(hr)) hr = tex->QueryInterface(IID_PPV_ARGS(&r1));
        if (SUCCEEDED(hr)) hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &nt);
        if (r1) r1->Release();
        if (SUCCEEDED(hr) && !DuplicateHandle(GetCurrentProcess(), nt, process_ ? static_cast<HANDLE>(process_) : GetCurrentProcess(),
                                              &remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
            hr = HRESULT_FROM_WIN32(GetLastError());
        if (nt) CloseHandle(nt);
        LOG("link: the game's lit colour %ux%u, format %u, %u samples, bind flags 0x%X: %s (0x%08lX)", sd.Width, sd.Height,
            static_cast<unsigned>(sd.Format), sd.SampleDesc.Count, sd.BindFlags,
            SUCCEEDED(hr) ? "shared with the core for the GI rays' hits" : "could not be shared", static_cast<unsigned long>(hr));
        if (FAILED(hr)) {
            rel(tex);
            radiance_failed_ = true;
            return false;
        }
        // The previous one goes: the core reads it through its own reference (the NT handle it opened, or the one
        // duplicated into its process until it does), so it lives as long as the core needs it. Kept, every change of
        // the game's size (a switch between windowed and full screen) left a full-screen texture behind.
        rel(radiance_srv_);
        rel(radiance_);
        radiance_ = tex;
        ID3D11ShaderResourceView* srv = nullptr;
        if (SUCCEEDED(device_->CreateShaderResourceView(tex, nullptr, &srv))) radiance_srv_ = srv;
        radiance_w_ = sd.Width;
        radiance_h_ = sd.Height;
        radiance_format_ = uint32_t(sd.Format);
        block_->radiance_handle = to_u64(remote);
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&block_->radiance_generation));
    }
    if (sd.SampleDesc.Count > 1)
        ctx->ResolveSubresource(radiance_, 0, target, 0, sd.Format);
    else
        ctx->CopyResource(radiance_, target);
    rfgrt_ipc::store64(&block_->radiance_frame, frame);
    return true;
}

void Link::release_frame_resources() {
    rel(result_srv_);
    rel(result_);
    rel(depth_rtv_);
    rel(depth_);
    rel(game_clb_);
    rel(irradiance_srv_);
    rel(irradiance_);
    close_handle(irradiance_nt_);
    rel(reflection_srv_);
    rel(reflection_);
    close_handle(reflection_nt_);
    rel(glass_srv_);
    rel(glass_);
    close_handle(glass_nt_);
    rel(glass_refl_srv_);
    rel(glass_refl_);
    close_handle(glass_refl_nt_);
    rel(sun_tint_srv_);
    rel(sun_tint_);
    close_handle(sun_tint_nt_);
    rel(fog_srv_);
    rel(fog_);
    close_handle(fog_nt_);
    for (int i = 0; i < light_mask_count_; ++i) {
        rel(light_mask_srv_[i]);
        rel(light_mask_[i]);
        close_handle(light_mask_nt_[i]);
    }
    light_mask_count_ = 0;
    rel(white_srv_);
    rel(white_);
    close_handle(depth_nt_);
    close_handle(result_nt_);
    close_handle(game_clb_nt_);
}

bool Link::ring_write(uint64_t bytes, void (*fill)(uint8_t*, void*), void* ctx, uint32_t wait_ms) {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (!started_.load() || dead_.load() || stopping_.load() || ring_stalled_.load() || bytes > ring_max_record()) {
        ++ring_dropped_;
        return false;
    }
    uint8_t* dst = writer_.try_reserve(bytes);
    if (!dst) {
        const ULONGLONG t0 = GetTickCount64();
        while (!(dst = writer_.try_reserve(bytes))) {
            if (dead_.load() || stopping_.load() || GetTickCount64() - t0 >= wait_ms) {
                ++ring_dropped_;
                ring_waits_ms_ += GetTickCount64() - t0;
                // A core that stopped reading would otherwise cost every later record the whole wait.
                if (wait_ms && !dead_.load() && !stopping_.load()) {
                    ring_stalled_ = true;
                    LOG("link: the core has not read the scene ring for %lu ms: no more scene records this session",
                        static_cast<unsigned long>(wait_ms));
                }
                return false;
            }
            Sleep(1);
        }
        ring_waits_ms_ += GetTickCount64() - t0;
    }
    fill(dst, ctx);
    writer_.commit();
    ++ring_records_;
    ring_bytes_ += bytes;
    return true;
}

Link::RingStats Link::ring_stats() const {
    RingStats r;
    r.records = ring_records_.load();
    r.bytes = ring_bytes_.load();
    r.waits_ms = ring_waits_ms_.load();
    r.dropped = ring_dropped_.load();
    return r;
}

unsigned long __stdcall Link::monitor_main(void* param) {
    auto* self = static_cast<Link*>(param);
    HANDLE core = static_cast<HANDLE>(self->process_ ? self->process_ : self->core_thread_);
    HANDLE waits[2] = {core, static_cast<HANDLE>(self->stop_event_)};
    const DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (r == WAIT_OBJECT_0 && !self->stopping_.load()) {
        DWORD code = 0;
        if (self->process_) GetExitCodeProcess(core, &code);
        if (self->block_->host_state == rfgrt_ipc::kHostFailed)
            LOG("link: the core failed (exit code %lu): %s", code, self->block_->host_message);
        else
            LOG("link: the core ended unexpectedly (exit code %lu): releasing fence B, back to the game's own shadows", code);
        self->rescue_fence_b();
        self->dead_ = true;
    }
    return 0;
}

// The core can die with a GPU wait on fence B already queued in the game's queue, after the work that would have
// signalled it was lost. D3D11 cannot signal a fence from the CPU; D3D12 can, in a 32-bit process too (only ray
// tracing is missing there). The device lives for this call only.
void Link::rescue_fence_b() {
    const HANDLE fence_b = block_ ? to_handle(block_->fence_b_handle) : nullptr;
    if (!fence_b) return;
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto create_device = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    using CreateFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto create_factory = dxgi ? reinterpret_cast<CreateFactory2>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* dev = nullptr;
    ID3D12Fence* fence = nullptr;
    HRESULT hr = E_FAIL;
    if (create_device && create_factory) {
        LUID luid{};
        luid.LowPart = block_->adapter_luid_low;
        luid.HighPart = block_->adapter_luid_high;
        hr = create_factory(0, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
        if (SUCCEEDED(hr)) hr = create_device(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        if (SUCCEEDED(hr)) hr = dev->OpenSharedHandle(fence_b, IID_PPV_ARGS(&fence));
        if (SUCCEEDED(hr)) hr = fence->Signal(1ull << 62);
    }
    LOG("link: fence B released from the CPU: 0x%08lX", static_cast<unsigned long>(hr));
    rel(fence);
    rel(dev);
    rel(adapter);
    rel(factory);
}

void Link::kill_for_test() {
    if (process_) {
        LOG("link: ending rfgrt_host.exe as a crash would (test)");
        TerminateProcess(static_cast<HANDLE>(process_), 9);
    } else if (core_thread_ && block_) {
        // OpenRFG64-memtest: the core in this process loses its device instead (what a GPU hang does to it).
        LOG("link: the core's device removed as a lost device would be (test)");
        block_->test_remove_device = 1;
    }
}

bool Link::stop(uint32_t wait_ms) {
    std::lock_guard<std::mutex> lock(start_mutex_);
    if (started_.load()) {
        stopping_ = true;
        block_->stop = 1;
        if (stop_event_) SetEvent(static_cast<HANDLE>(stop_event_));
        HANDLE core = static_cast<HANDLE>(process_ ? process_ : core_thread_);
        if (core && WaitForSingleObject(core, wait_ms) != WAIT_OBJECT_0) {
            LOG("link: the core did not stop within %lu ms: it keeps what it has, and no core starts again",
                static_cast<unsigned long>(wait_ms));
            start_failed_ = true;
            return false;
        }
        if (monitor_) WaitForSingleObject(static_cast<HANDLE>(monitor_), 2000);  // it ends on stop_event_
    }
    std::lock_guard<std::mutex> ring_lock(ring_mutex_);  // a scene record being written is finished first
    started_ = false;
    release_frame_resources();
    rel(radiance_srv_);
    rel(radiance_);
    radiance_w_ = radiance_h_ = radiance_format_ = 0;
    radiance_failed_ = false;
    rel(fence_b_);
    rel(fence_a_);
    close_handle(fence_a_nt_);
    close_handle(fence_b_nt_);
    if (block_ && block_->fence_b_handle) CloseHandle(to_handle(block_->fence_b_handle));  // the core's, duplicated for us
    close_handle(monitor_);
    close_handle(core_thread_);
    close_handle(process_);
    close_handle(job_);
    close_handle(stop_event_);
    close_handle(scene_event_);
    if (region_) {
        if (opt_.in_process)
            VirtualFree(region_, 0, MEM_RELEASE);
        else
            UnmapViewOfFile(region_);
    }
    close_handle(mapping_);
    region_ = nullptr;
    block_ = nullptr;
    region_name_[0] = 0;
    writer_ = rfgrt_scene::RingWriter{};
    generation_ = 0;
    width_ = height_ = 0;
    depth_remote_ = result_remote_ = 0;
    fence_b_failed_ = false;
    ring_stalled_ = false;
    dead_ = false;
    stopping_ = false;
    start_failed_ = false;
    LOG("link: the core stopped; the region, fences and frame textures released");
    return true;
}

void Link::request_stop(uint32_t wait_ms) {
    std::lock_guard<std::mutex> lock(start_mutex_);
    start_failed_ = true;  // no start after this
    if (!started_.load() || stopping_.exchange(true)) return;
    block_->stop = 1;
    if (stop_event_) SetEvent(static_cast<HANDLE>(stop_event_));
    HANDLE core = static_cast<HANDLE>(process_ ? process_ : core_thread_);
    if (core && !dead_.load() && WaitForSingleObject(core, wait_ms) != WAIT_OBJECT_0)
        LOG("link: the core did not stop within %lu ms", static_cast<unsigned long>(wait_ms));
}

}  // namespace rtlink
