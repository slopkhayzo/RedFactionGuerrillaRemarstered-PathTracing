#include "harness/shader_dump.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/active.h"
#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "hooks/hooks.h"
#include "render/gi.h"
#include "render/lights.h"

namespace shader_dump {
namespace {

thread_local const char* t_name = nullptr;  // the shader file being created on this thread
std::atomic<int> g_saved{0};
bool g_device_hooked = false;
bool g_dump = false;  // Telemetry.DumpShaders (the hooks also serve GI's shader patch)

using CreateVsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using CreatePsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
CreateVsFn o_create_vs = nullptr;
CreatePsFn o_create_ps = nullptr;

// A game that switches path tracing: each patched pixel shader in both versions, by the game's slot for it (its
// FragmentShader's pointer). The slot holds one, which the game releases; the other is ours. Under g_versions_mutex.
struct Versions {
    ID3D11PixelShader* original;
    ID3D11PixelShader* patched;
};
std::mutex g_versions_mutex;
std::unordered_map<ID3D11PixelShader**, Versions> g_versions;
bool g_two_versions = false;  // the game switches and has the release seam
void(__cdecl* o_release_ps)(ID3D11PixelShader**) = nullptr;

// The game releases a pixel shader: its slot's, which the game releases itself, and the other version with it.
void __cdecl d_release_ps(ID3D11PixelShader** slot) {
    if (slot) {
        std::lock_guard<std::mutex> lock(g_versions_mutex);
        auto it = g_versions.find(slot);
        if (it != g_versions.end()) {
            ID3D11PixelShader* other = *slot == it->second.patched ? it->second.original : it->second.patched;
            if (other) other->Release();
            g_versions.erase(it);
        }
    }
    o_release_ps(slot);
}

// <run>\shaders\<effect file>\<FNV-1a hash of the bytecode>_<ps|vs>.dxbc: an effect file holds several shaders, each
// kept (the layout research/scripts/shaders.py extracts misc.vpp_pc to, for tools/shaderpatch).
void save(const void* code, SIZE_T size, bool pixel) {
    if (!t_name || !code || !size) return;
    std::wstring dir = session::run_dir() + L"\\shaders";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\";
    for (const char* p = t_name; *p; ++p) {
        const char c = *p;
        dir += (c == '/' || c == '\\' || c == ':') ? L'_' : wchar_t(static_cast<unsigned char>(c));
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    uint32_t hash = 2166136261u;
    for (SIZE_T i = 0; i < size; ++i) hash = (hash ^ static_cast<const uint8_t*>(code)[i]) * 16777619u;
    wchar_t name[32];
    swprintf(name, 32, L"\\%08x_%ls.dxbc", hash, pixel ? L"ps" : L"vs");
    const std::wstring file = dir + name;
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, code, DWORD(size), &written, nullptr);
    CloseHandle(f);
    ++g_saved;
}

HRESULT STDMETHODCALLTYPE d_create_vs(ID3D11Device* self, const void* code, SIZE_T size, ID3D11ClassLinkage* linkage,
                                      ID3D11VertexShader** out) {
    if (g_dump) save(code, size, false);
    return o_create_vs(self, code, size, linkage, out);
}

HRESULT STDMETHODCALLTYPE d_create_ps(ID3D11Device* self, const void* code, SIZE_T size, ID3D11ClassLinkage* linkage,
                                      ID3D11PixelShader** out) {
    if (g_dump) save(code, size, true);
    std::vector<uint8_t> patched;
    bool changed = gi::patch_pixel_shader(code, size, &patched) || lights::patch_pixel_shader(code, size, &patched);
    // the game's fog, last, on the shader as patched so far (lit passes and lights' passes alike)
    if (gi::patch_fog(changed ? patched.data() : code, changed ? patched.size() : size, &patched)) changed = true;
    // the eye's adaptation to a dark scene (rl_hdr's adaptation and bright pass)
    if (gi::patch_tonemap(changed ? patched.data() : code, changed ? patched.size() : size, &patched)) changed = true;
    if (changed && g_two_versions && out) {  // both: the original for path tracing off, the patched one for on
        ID3D11PixelShader* original = nullptr;
        const HRESULT hr = o_create_ps(self, code, size, linkage, &original);
        if (FAILED(hr)) return hr;
        ID3D11PixelShader* ours = nullptr;
        if (FAILED(o_create_ps(self, patched.data(), patched.size(), linkage, &ours)) || !ours) {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 4) LOG("shaders: a patched pixel shader was refused: the original is used");
            *out = original;
            return hr;
        }
        std::lock_guard<std::mutex> lock(g_versions_mutex);
        g_versions[out] = Versions{original, ours};
        *out = active::on() ? ours : original;
        return hr;
    }
    if (changed) {
        const HRESULT hr = o_create_ps(self, patched.data(), patched.size(), linkage, out);
        if (SUCCEEDED(hr)) return hr;
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 4) LOG("shaders: a patched pixel shader was refused (0x%08lX): the original is used", static_cast<unsigned long>(hr));
    }
    return o_create_ps(self, code, size, linkage, out);
}

// The device's methods can only be hooked once the device exists: at the first shader the game loads by name.
void hook_device(uint8_t* graphics) {
    if (g_device_hooked || !graphics) return;
    g_device_hooked = true;
    auto* dev = rfg::field<ID3D11Device*>(graphics, rfg::off::GraphicsSystem_pDevice);
    if (!dev) return;
    void** vtable = *reinterpret_cast<void***>(dev);
    bool ok = hooks::create_now(vtable[12], reinterpret_cast<void*>(&d_create_vs), reinterpret_cast<void**>(&o_create_vs),
                                "ID3D11Device::CreateVertexShader");
    ok &= hooks::create_now(vtable[15], reinterpret_cast<void*>(&d_create_ps), reinterpret_cast<void**>(&o_create_ps),
                            "ID3D11Device::CreatePixelShader");
    LOG("shaders: device hooks %s (dump %s, GI patch %s, light-pass patch %s)", ok ? "in place" : "failed", g_dump ? "on" : "off",
        gi::enabled() ? "on" : "off", lights::enabled() ? "on" : "off");
}

void*(__cdecl* o_create_vs_file)(uint8_t*, const char*, const char*);
void*(__cdecl* o_create_fs_file)(uint8_t*, const char*, const char*);

void* __cdecl d_create_vs_file(uint8_t* graphics, const char* name, const char* defines) {
    hook_device(graphics);
    t_name = name;
    void* r = o_create_vs_file(graphics, name, defines);
    t_name = nullptr;
    return r;
}

void* __cdecl d_create_fs_file(uint8_t* graphics, const char* name, const char* defines) {
    hook_device(graphics);
    t_name = name;
    void* r = o_create_fs_file(graphics, name, defines);
    t_name = nullptr;
    return r;
}

void report(bool) {
    if (g_device_hooked) LOG("shader dump: %d shaders saved", g_saved.load());
}

}  // namespace

void use_patched(bool on) {
    std::lock_guard<std::mutex> lock(g_versions_mutex);
    for (auto& entry : g_versions) *entry.first = on ? entry.second.patched : entry.second.original;
    LOG("shaders: %zu patched pixel shaders %s", g_versions.size(), on ? "bound (path tracing)" : "swapped for the game's originals");
}

bool install(const Config& cfg) {
    g_dump = cfg.dump_shaders;
    if (!cfg.dump_shaders && !cfg.gi_enabled && !cfg.lights_enabled) return true;
    session::add_flush_handler(&report);
    if (active::switchable()) {
        g_two_versions = hooks::seam("openrfg_release_pixel_shader", reinterpret_cast<void*>(&d_release_ps),
                                     reinterpret_cast<void**>(&o_release_ps));
        LOG("shaders: %s", g_two_versions ? "the patched pixel shaders made beside the originals (path tracing switches)"
                                          : "no seam for the shaders' release: the patched ones stay with path tracing off");
    }
    bool ok = hooks::create(rfg::rva::keen__graphics__createVertexShaderFromFile, &d_create_vs_file, &o_create_vs_file,
                            "keen::graphics::createVertexShaderFromFile");
    ok &= hooks::create(rfg::rva::keen__graphics__createFragmentShaderFromFile, &d_create_fs_file, &o_create_fs_file,
                        "keen::graphics::createFragmentShaderFromFile");
    return ok;
}

}  // namespace shader_dump
