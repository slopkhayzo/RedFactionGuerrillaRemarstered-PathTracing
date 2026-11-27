#include "mirror/textures.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "rt/link.h"

namespace textures {
namespace {

using rfgrt_scene::kMaxTextures;

std::atomic<bool> g_enabled{false};
std::mutex g_mutex;
std::unordered_map<uint32_t, uint32_t> g_ids;  // game texture handle -> the core's id
std::unordered_map<const void*, uint32_t> g_data_ids;  // game texture object (keen::TextureData*) -> the core's id
struct Ask {
    uint32_t handle, id, tries;
    const void* data;  // the texture's object, for one asked for by it (handle 0)
};
std::vector<Ask> g_asked;  // to copy at a seam, in order
uint32_t g_next = 0;
std::atomic<uint8_t> g_state[kMaxTextures];

// Copies made, waiting for the core to queue the frame whose fence A follows them (render thread only).
struct Copied {
    uint32_t id, width, height, mips, format;
    uint64_t handle;
    int64_t frame;
};
std::vector<Copied> g_copied;

std::atomic<uint64_t> g_sent{0}, g_no_view{0}, g_unusable{0}, g_failed{0}, g_bytes{0}, g_over{0}, g_retries{0};
uint64_t g_seam_ticks = 0, g_seam_max_ticks = 0, g_seams = 0;  // the copies' time on the render thread
HRESULT g_first_failure = S_OK;
std::mutex g_format_mutex;
std::unordered_map<uint32_t, uint64_t> g_formats_taken, g_formats_left;  // DXGI format -> textures

constexpr uint32_t kMaxSize = 256;    // the copy's largest mip, in texels
constexpr size_t kPerSeam = 8;        // textures copied a frame at most
constexpr uint32_t kViewTries = 120;  // seams a texture without a device view yet is asked again (streaming)

// The game texture's D3D11 view: get_dev_texture's object (a bitmap or a tracker pool's texture) holds it at +0x18,
// as rl_renderer::set_texture passes it to PSSetShaderResources.
bool dev_view(uint32_t handle, ID3D11ShaderResourceView** out) {
    __try {
        void* tex = nullptr;
        rfg::fn<void**(__cdecl*)(void**, uint32_t)>(rfg::rva::get_dev_texture)(&tex, handle);
        if (!tex) return false;
        *out = *reinterpret_cast<ID3D11ShaderResourceView**>(static_cast<uint8_t*>(tex) + rfg::off::TextureData_pShaderView);
        return *out != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The same from the texture's object itself.
bool data_view(const void* data, ID3D11ShaderResourceView** out) {
    __try {
        *out = *reinterpret_cast<ID3D11ShaderResourceView* const*>(static_cast<const uint8_t*>(data) + rfg::off::TextureData_pShaderView);
        return *out != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool block_compressed(DXGI_FORMAT f) {
    return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) || (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
}

// The view formats the core samples as colour (four channels, unsigned normalized).
bool usable(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_B5G6R5_UNORM:
            return true;
        default:
            return false;
    }
}

uint64_t mip_bytes(DXGI_FORMAT f, uint32_t w, uint32_t h) {
    if (block_compressed(f)) {
        const uint64_t blocks = uint64_t((w + 3) / 4) * ((h + 3) / 4);
        return blocks * (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC1_UNORM_SRGB ? 8 : 16);
    }
    return uint64_t(w) * h * (f == DXGI_FORMAT_B5G6R5_UNORM ? 2 : 4);
}

void fill_texture(uint8_t* dst, void* ctx) {
    const auto& c = *static_cast<const Copied*>(ctx);
    auto* r = reinterpret_cast<rfgrt_scene::TextureAdd*>(dst);
    memset(r, 0, sizeof(*r));
    r->h.type = rfgrt_scene::kTextureAdd;
    r->h.bytes = uint32_t(sizeof(*r));
    r->id = c.id;
    r->format = c.format;
    r->handle = c.handle;
    r->width = c.width;
    r->height = c.height;
    r->mips = c.mips;
}

enum class Copy { Done, NoView, Unusable, Failed };

// One texture: its mips from kMaxSize down into a shared copy, recorded on `ctx`, its handle duplicated into the core.
Copy copy_one(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t handle, uint32_t id, int64_t frame, const void* data) {
    ID3D11ShaderResourceView* view = nullptr;
    if (data ? !data_view(data, &view) : !dev_view(handle, &view)) return Copy::NoView;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    view->GetDesc(&vd);
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    ID3D11Texture2D* src = nullptr;
    if (res) {
        res->QueryInterface(IID_PPV_ARGS(&src));
        res->Release();
    }
    if (!src) return Copy::Unusable;
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    bool ok = vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && d.ArraySize == 1 && d.SampleDesc.Count == 1 &&
              !(d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) && usable(vd.Format) && d.Width && d.Height && d.MipLevels;
    // the first mip at most kMaxSize either way (block-compressed: whole blocks)
    uint32_t m0 = 0;
    while (ok && m0 + 1 < d.MipLevels && ((d.Width >> m0) > kMaxSize || (d.Height >> m0) > kMaxSize)) ++m0;
    const uint32_t w = d.Width >> m0 ? d.Width >> m0 : 1, h = d.Height >> m0 ? d.Height >> m0 : 1;
    if (ok && block_compressed(d.Format) && ((w & 3) || (h & 3))) ok = false;
    if (!ok) {
        {
            std::lock_guard<std::mutex> lock(g_format_mutex);
            ++g_formats_left[uint32_t(vd.Format)];
        }
        src->Release();
        return Copy::Unusable;
    }
    // the mips that stay whole blocks
    uint32_t mips = d.MipLevels - m0;
    if (block_compressed(d.Format))
        for (uint32_t i = 1; i < mips; ++i)
            if (((w >> i) & 3) || ((h >> i) & 3)) {
                mips = i;
                break;
            }
    D3D11_TEXTURE2D_DESC cd = d;
    cd.Width = w;
    cd.Height = h;
    cd.MipLevels = mips;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cd.CPUAccessFlags = 0;
    cd.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D* copy = nullptr;
    HRESULT hr = dev->CreateTexture2D(&cd, nullptr, &copy);
    if (FAILED(hr) || !copy) {
        if (g_first_failure == S_OK) g_first_failure = FAILED(hr) ? hr : E_FAIL;
        src->Release();
        return Copy::Failed;
    }
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < mips; ++i) {
        ctx->CopySubresourceRegion(copy, i, 0, 0, 0, src, D3D11CalcSubresource(m0 + i, 0, d.MipLevels), nullptr);
        bytes += mip_bytes(d.Format, w >> i ? w >> i : 1, h >> i ? h >> i : 1);
    }
    src->Release();
    const uint64_t remote = rtlink::core_link().share_with_core(copy, &hr);
    // The core holds it through the handle from here on (D3D11 keeps it for the copies recorded until they are done).
    copy->Release();
    if (!remote) {
        if (g_first_failure == S_OK) g_first_failure = FAILED(hr) ? hr : E_FAIL;
        return Copy::Failed;
    }
    g_copied.push_back(Copied{id, w, h, mips, uint32_t(vd.Format), remote, frame});
    {
        std::lock_guard<std::mutex> lock(g_format_mutex);
        ++g_formats_taken[uint32_t(vd.Format)];
    }
    g_bytes += bytes;
    return Copy::Done;
}

// A material's constant by name (rl_material::get_constant_data), only where the material's shader has it: the game's
// lookup copies from before the constants when the name is not one of them (rl_shader_library::get_shader_const_index:
// -1), so its location is asked first.
bool constant(const uint8_t* material, const char* name, float* out, int count) {
    if (!*reinterpret_cast<const uint8_t* const*>(material + rfg::off::Material_data)) return false;
    using Loc = int(__thiscall*)(const uint8_t*, const char*);
    using Get = bool(__thiscall*)(const uint8_t*, float*, const char*, int);
    if (rfg::fn<Loc>(rfg::rva::Material_getConstantLocByName)(material, name) < 0) return false;
    return rfg::fn<Get>(rfg::rva::Material_getConstantDataByName)(material, out, name, count);
}

bool read_material(uint32_t handle, rfgrt_scene::MaterialEntry* out, int* diffuse) {
    __try {
        using Resolve = uint8_t*(__cdecl*)(uint32_t handle, bool);
        const uint8_t* m = rfg::fn<Resolve>(rfg::rva::resolve_material_handle)(handle, false);
        if (!m) return false;
        using TextureByName = bool(__thiscall*)(const uint8_t*, int*, const char*);
        int tex = -1;
        if (rfg::fn<TextureByName>(rfg::rva::Material_getTextureDataByName)(m, &tex, "Diffuse_Map")) *diffuse = tex;
        float v[4] = {};
        if (constant(m, "Diffuse_Color", v, 3)) memcpy(out->color, v, sizeof(float) * 3);
        if (constant(m, "Diffuse_Map_TilingU", v, 1)) out->tiling[0] = v[0];
        if (constant(m, "Diffuse_Map_TilingV", v, 1)) out->tiling[1] = v[0];
        if (constant(m, "Self_Illumination", v, 1)) out->self_illumination = v[0];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void set_enabled(bool on) { g_enabled = on; }
bool enabled() { return g_enabled.load(); }

uint32_t id_of(uint32_t handle) {
    if (handle == kNone || !g_enabled.load()) return kNone;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_ids.find(handle);
    if (it != g_ids.end()) return it->second;
    if (g_next >= kMaxTextures) {
        ++g_over;
        return g_ids[handle] = kNone;
    }
    const uint32_t id = g_next++;
    g_ids[handle] = id;
    g_state[id] = kPending;
    g_asked.push_back(Ask{handle, id, 0, nullptr});
    return id;
}

uint32_t id_of_data(const void* data) {
    if (!data || !g_enabled.load()) return kNone;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_data_ids.find(data);
    if (it != g_data_ids.end()) return it->second;
    if (g_next >= kMaxTextures) {
        ++g_over;
        return g_data_ids[data] = kNone;
    }
    const uint32_t id = g_next++;
    g_data_ids[data] = id;
    g_state[id] = kPending;
    g_asked.push_back(Ask{0, id, 0, data});
    return id;
}

bool data_size(const void* data, uint32_t* width, uint32_t* height) {
    ID3D11ShaderResourceView* view = nullptr;
    if (!data || !data_view(data, &view)) return false;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res) {
        res->QueryInterface(IID_PPV_ARGS(&tex));
        res->Release();
    }
    if (!tex) return false;
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    tex->Release();
    *width = d.Width;
    *height = d.Height;
    return d.Width && d.Height;
}

State state(uint32_t id) { return id < kMaxTextures ? State(g_state[id].load()) : kFailed; }

bool material_entry(uint32_t material_handle, rfgrt_scene::MaterialEntry* out) {
    *out = rfgrt_scene::MaterialEntry{};
    out->texture = kNone;
    out->tiling[0] = out->tiling[1] = 1.0f;
    out->color[0] = out->color[1] = out->color[2] = 1.0f;
    int diffuse = -1;
    if (!read_material(material_handle, out, &diffuse)) return false;
    if (diffuse != -1) out->texture = id_of(uint32_t(diffuse));
    return true;
}

void on_seam(ID3D11Device* dev, ID3D11DeviceContext* ctx, int64_t frame, int64_t core_queued) {
    if (!dev || !ctx || !g_enabled.load()) return;
    const int64_t t0 = session::qpc();
    rtlink::Link& link = rtlink::core_link();
    // the copies the core can take now: it has queued the frame whose fence A came after them
    size_t kept = 0;
    for (size_t i = 0; i < g_copied.size(); ++i) {
        Copied& c = g_copied[i];
        bool done = false;
        if (c.frame <= core_queued) {
            if (link.ring_write(sizeof(rfgrt_scene::TextureAdd), &fill_texture, &c, 0)) {
                g_state[c.id] = kSent;
                ++g_sent;
                done = true;
            } else if (link.ring_stalled() || link.dead()) {
                g_state[c.id] = kFailed;
                ++g_failed;
                done = true;  // (its handle is left in the core's process: it ends with it)
            }
        }
        if (!done) g_copied[kept++] = c;
    }
    g_copied.resize(kept);
    // then a few more copies, for a later seam
    std::vector<Ask> batch;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const size_t n = g_asked.size() < kPerSeam ? g_asked.size() : kPerSeam;
        batch.assign(g_asked.begin(), g_asked.begin() + ptrdiff_t(n));
        g_asked.erase(g_asked.begin(), g_asked.begin() + ptrdiff_t(n));
    }
    std::vector<Ask> again;
    for (const Ask& a : batch) {
        switch (copy_one(dev, ctx, a.handle, a.id, frame, a.data)) {
            case Copy::Done: break;
            case Copy::NoView:
                if (a.tries + 1 < kViewTries) {  // not streamed in yet: later
                    again.push_back(Ask{a.handle, a.id, a.tries + 1, a.data});
                    ++g_retries;
                } else {
                    g_state[a.id] = kFailed;
                    ++g_no_view;
                }
                break;
            case Copy::Unusable:
                g_state[a.id] = kFailed;
                ++g_unusable;
                break;
            case Copy::Failed:
                g_state[a.id] = kFailed;
                ++g_failed;
                break;
        }
    }
    if (!again.empty()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_asked.insert(g_asked.end(), again.begin(), again.end());
    }
    if (!batch.empty() || kept) {
        const uint64_t ticks = uint64_t(session::qpc() - t0);
        g_seam_ticks += ticks;
        if (ticks > g_seam_max_ticks) g_seam_max_ticks = ticks;
        ++g_seams;
    }
}

void on_core_stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ids.clear();
    g_data_ids.clear();
    g_asked.clear();
    g_next = 0;
    for (auto& s : g_state) s.store(kUnknown);
    g_copied.clear();  // (the render thread's: idle here, path tracing off and its frame work finished)
}

void log_stats() {
    if (!g_enabled.load()) return;
    size_t asked = 0, known = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        asked = g_asked.size();
        known = g_ids.size();
    }
    LOG("textures: %zu of the materials' diffuse maps named, %llu sent to the core (%.1f MB, mips from %u texels down), %zu "
        "to copy, %zu copied and waiting for the core's frame; left out: no device view %llu (%llu asked again), a format or "
        "shape not taken %llu, failed %llu (first 0x%08lX), past the table's %u %llu; the copies took %.2f ms a frame over %llu "
        "frames, at most %.2f ms",
        known, static_cast<unsigned long long>(g_sent.load()), g_bytes.load() / 1048576.0, kMaxSize, asked, g_copied.size(),
        static_cast<unsigned long long>(g_no_view.load()), static_cast<unsigned long long>(g_retries.load()),
        static_cast<unsigned long long>(g_unusable.load()), static_cast<unsigned long long>(g_failed.load()),
        static_cast<unsigned long>(g_first_failure), kMaxTextures, static_cast<unsigned long long>(g_over.load()),
        g_seams ? session::ticks_to_ms(int64_t(g_seam_ticks)) / double(g_seams) : 0.0, static_cast<unsigned long long>(g_seams),
        session::ticks_to_ms(int64_t(g_seam_max_ticks)));
    std::string taken, left;
    std::lock_guard<std::mutex> lock(g_format_mutex);
    for (const auto& [f, n] : g_formats_taken) taken += (taken.empty() ? "" : ", ") + std::to_string(f) + ": " + std::to_string(n);
    for (const auto& [f, n] : g_formats_left) left += (left.empty() ? "" : ", ") + std::to_string(f) + ": " + std::to_string(n);
    LOG("textures: their view formats (DXGI) taken {%s}, left out {%s}", taken.empty() ? "none" : taken.c_str(),
        left.empty() ? "none" : left.c_str());
}

}  // namespace textures
