// shaderpatch: checks the DXBC tools of src/rfgrt/patch on the game's own shaders, as research/scripts/shaders.py
// extracted them (research/out/shaders/fxo/<effect>/<nn>_<ps|vs>.dxbc):
//   - every container's checksum recomputed and compared with the stored one;
//   - every program decoded, its instructions compared with D3DDisassemble's;
//   - the ambient patch (patch/ambient.h) and the light-pass patch (patch/light_pass.h) applied to every pixel shader;
//     each patched one disassembled (<nn>_ps.patched.asm, <nn>_ps.lightpass.asm, for review) and created on a WARP device;
//   - each patched one run against its original on WARP, one pixel, with every input, texture and constant made up, in
//     settings where the two must agree (below): the patch is checked by what the shader computes, not by its look.
// The report goes to <dir>/patch_report.txt.
//
// The ambient patch must replace the diffuse ambient V = the sky lerp(back, ambient, N.up) with the fake bounce
// (lightColor * indirectLightParams.x, toward the horizon by indirectLightParams.y) blended in, before the
// self-illumination, the AO and the albedo apply. With E at t14:
//   A  ambient = back = S and lightColor * indirectLightParams.x = S, so that V = S whatever the normal: the patched
//      shader with E = S gives the original's colour (every texture and constant made up, self-illumination too);
//   B  the bounce changed alone (indirectLightParams.x), specular textures black: the patched shader's colour does not
//      change (the bounce is inside what E replaces), where the original's may;
//   C  albedo textures black (rgb 0, alpha 1): the diffuse term is 0, so the patched shader gives the original's colour
//      for any E (its specular ambient is untouched).
// The light-pass patch: with t15 = (x, v), the patched shader's colour is the original's times v (alpha unchanged).
// The shadow floor's patch (patch/shadow_floor.h), on the shader as the plugin has it by then (GI- and
// reflection-patched, or the original): FA with t14's alpha 0 it is that shader; FB with the shadow 1 (the floor plays
// no part) too; FC with alpha 1 and the shadow 0 it is that shader with the sun's colour 0 (the horizon's fake bounce,
// indirectLightParams.x, off in both): nothing of the sun is left.
// The glass's patch (patch/glass.h), on the original: GA with t17's alpha 0 it is the original; GB with the light made
// E whatever the normal (ambient = back ambient = lightColor * indirectLightParams.x = E) and t17 = (E, 2: lit) too. The
// highlight is linear in lightColor: O1 with lightColor L and indirectLightParams.x ix, O2 with 2L and ix / 2 (the light
// still E) give the original without its highlight, 2 O1 - O2; GC with t17's alpha 1 (in shadow) that, GD with 1.5 (half
// the sun) 1.5 O1 - 0.5 O2 (colour only).
// The sky reflection's occlusion (patch/spec_occlusion.h), on a GI-patched shader the reflections' patch left: SA with
// t14's alpha 0 it is that shader; SB with the irradiance as bright as the ambient (k = 1) too; SC with the irradiance 0
// (k = 0) it is that shader with indirectLightParams.z 0 (no sky reflection).
// The fog's patch (patch/fog.h), last, on the shader as the plugin has it by then (as the shadow floor's above, or the
// original): XA with t18's alpha 0 it is that shader; XB with alpha 0.6 (the core's fog there) it is that shader with
// the fog's input component 0 (no fog of the game's).
// The fog at the glass (patch/glass.h `fog`, then the fog's patch on t21, on the original as the plugin does): GF0 with
// t21's alpha 0 it is the glass-patched shader; GF1 with t21 = (S, T) it is that shader with the fog's input component 0,
// its colour times T plus S.
//
//   shaderpatch [research\out\shaders]
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "patch/ambient.h"
#include "patch/dxbc.h"
#include "patch/fog.h"
#include "patch/glass.h"
#include "patch/light_pass.h"
#include "patch/shadow_floor.h"
#include "patch/spec_occlusion.h"
#include "patch/sun_tint.h"

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string disassemble(const std::vector<uint8_t>& blob) {
    ID3DBlob* out = nullptr;
    if (FAILED(D3DDisassemble(blob.data(), blob.size(), 0, nullptr, &out)) || !out) return {};
    std::string s(static_cast<const char*>(out->GetBufferPointer()), out->GetBufferSize());
    out->Release();
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::string normalize(std::string w) {
    const size_t paren = w.find('(');
    if (paren != std::string::npos) w.resize(paren);
    for (const char* suffix : {"_sat", "_nz", "_z", "_indexable", "_aoffimmi"}) {
        const size_t n = strlen(suffix);
        if (w.size() > n && w.compare(w.size() - n, n, suffix) == 0) w.resize(w.size() - n);
    }
    return w;
}

// The instruction mnemonics of a disassembly, declarations left out, in order.
std::vector<std::string> mnemonics(const std::string& text) {
    std::vector<std::string> out;
    bool in = false;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("ps_4", 0) == 0 || line.rfind("vs_4", 0) == 0) {
            in = true;
            continue;
        }
        const size_t lead = line.find_first_not_of(' ');  // the bodies of loops and branches are indented
        if (lead == std::string::npos) continue;
        line.erase(0, lead);
        if (!in || line[0] == '/') continue;
        std::string w = line.substr(0, line.find(' '));
        if (w.rfind("dcl_", 0) == 0) continue;
        out.push_back(normalize(w));
    }
    return out;
}

std::vector<std::string> decoded_mnemonics(const dxbc::Program& p) {
    std::vector<std::string> out;
    for (const dxbc::Instruction& in : p.code) {
        if ((in.opcode >= dxbc::kDclResource && in.opcode <= dxbc::kDclGlobalFlags) || in.opcode == dxbc::kCustomData) continue;
        const std::string d = dxbc::describe(p, in);
        out.push_back(normalize(d.substr(0, d.find(' '))));
    }
    return out;
}

bool contains_nocase(const std::string& s, const char* what) {
    std::string a = s, b = what;
    for (auto& ch : a) ch = char(tolower(uint8_t(ch)));
    for (auto& ch : b) ch = char(tolower(uint8_t(ch)));
    return a.find(b) != std::string::npos;
}

struct Float4 {
    float v[4] = {0, 0, 0, 0};
};

// --- running one pixel shader on WARP --------------------------------------------------------------------------------

enum class Role { Normal, Specular, Albedo, Clb, Irradiance, Other };

// What a pixel shader needs bound: its inputs (registers), textures (slot, dimension, role), samplers, constant buffers
// (slot, registers), from its declarations and RDEF.
struct Interface {
    uint32_t input_registers = 0;  // v0 .. v<n-1>
    struct Tex {
        uint32_t slot, dim;  // D3D10_SB_RESOURCE_DIMENSION: 3 texture2d, 5 texture3d, 6 texturecube
        Role role;
    };
    std::vector<Tex> textures;
    std::vector<uint32_t> samplers;
    std::map<uint32_t, uint32_t> cbuffers;  // slot -> registers
    uint32_t common_cb = UINT32_MAX, light_cb = UINT32_MAX, instance_cb = UINT32_MAX;
    std::vector<std::array<uint32_t, 3>> self_illum;  // (slot, register, component)
    std::vector<std::array<uint32_t, 2>> diffuse_colours;  // the material's colours other than specular: (slot, register)
};

bool read_interface(const std::vector<uint8_t>& blob, Interface* out) {
    dxbc::Container c;
    dxbc::Program p;
    std::string error;
    const dxbc::Chunk* shdr = nullptr;
    if (!dxbc::parse(blob.data(), blob.size(), &c, &error) || !(shdr = c.find("SHDR")) || !dxbc::decode(*shdr, &p, &error)) return false;
    const std::vector<dxbc::Binding> binds = dxbc::bindings(c);
    for (const dxbc::Binding& b : binds) {
        if (b.type != 0) continue;
        if (contains_nocase(b.name, "commonFragment")) out->common_cb = b.slot;
        if (contains_nocase(b.name, "lightingFragment")) out->light_cb = b.slot;
        if (contains_nocase(b.name, "instanceFragment")) out->instance_cb = b.slot;
    }
    uint32_t material_cb = UINT32_MAX;
    for (const dxbc::Binding& b : binds)
        if (b.type == 0 && contains_nocase(b.name, "material")) material_cb = b.slot;  // g_material(Fragment)Constants
    for (const dxbc::Variable& v : dxbc::variables(c)) {
        if (contains_nocase(v.name, "Self_Illum") || contains_nocase(v.name, "illum_cycle"))
            out->self_illum.push_back({v.slot, v.offset / 16, (v.offset % 16) / 4});
        if (v.slot == material_cb && contains_nocase(v.name, "Color") && !contains_nocase(v.name, "Specular") && v.offset % 16 == 0)
            out->diffuse_colours.push_back({v.slot, v.offset / 16});
    }
    for (const dxbc::Instruction& in : p.code) {
        if (in.opcode == dxbc::kDclResource && !in.operands.empty()) {
            const uint32_t slot = in.operands[0].index[0];
            const uint32_t dim = (p.tokens[in.offset] >> 11) & 0x1F;
            Role role = Role::Other;
            if (slot == 15) role = Role::Clb;
            if (slot == ambient::kIrradianceSlot) role = Role::Irradiance;
            for (const dxbc::Binding& b : binds) {
                if (b.type != 2 || b.slot != slot || slot >= 14) continue;
                if (contains_nocase(b.name, "Normal")) role = Role::Normal;
                else if (contains_nocase(b.name, "Specular") || contains_nocase(b.name, "reflection") ||
                         contains_nocase(b.name, "BRDF_Spec"))
                    role = Role::Specular;
                else if (contains_nocase(b.name, "Diffuse") || contains_nocase(b.name, "Decal") || contains_nocase(b.name, "Alpha") ||
                         contains_nocase(b.name, "Combine") || contains_nocase(b.name, "BRDF_Diff") ||
                         contains_nocase(b.name, "Environment"))  // a 2D layer blended into the albedo in these materials
                    role = Role::Albedo;
            }
            out->textures.push_back({slot, dim, role});
        } else if (in.opcode == dxbc::kDclSampler && !in.operands.empty()) {
            out->samplers.push_back(in.operands[0].index[0]);
        } else if (in.opcode == dxbc::kDclConstantBuffer && !in.operands.empty()) {
            out->cbuffers[in.operands[0].index[0]] = in.operands[0].index[1];
        } else if ((in.opcode == 98 || in.opcode == 99 || in.opcode == 100) && !in.operands.empty()) {
            out->input_registers = std::max(out->input_registers, in.operands[0].index[0] + 1);
        }
    }
    return true;
}

class Runner {
public:
    bool init() {
        const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_)))
            return false;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 1;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &target_)) || FAILED(dev_->CreateRenderTargetView(target_, nullptr, &rtv_))) return false;
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &staging_))) return false;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev_->CreateSamplerState(&sd, &sampler_))) return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 64 * 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &vs_values_))) return false;
        return true;
    }
    ~Runner() {
        for (auto& [k, v] : vs_) v->Release();
        for (IUnknown* u : std::initializer_list<IUnknown*>{vs_values_, sampler_, staging_, rtv_, target_, ctx_, dev_})
            if (u) u->Release();
    }
    ID3D11Device* device() { return dev_; }

    // The shader's colour for one pixel: inputs v1.. as given, each texture a constant colour, each constant buffer as
    // given (registers). False when something could not be made.
    bool run(const std::vector<uint8_t>& ps_blob, const Interface& f, const std::vector<Float4>& inputs,
             const std::map<uint32_t, Float4>& texture_colours, const std::map<uint32_t, std::vector<Float4>>& constants,
             Float4* out) {
        ID3D11VertexShader* vs = vertex_shader(f.input_registers);
        ID3D11PixelShader* ps = nullptr;
        if (!vs || FAILED(dev_->CreatePixelShader(ps_blob.data(), ps_blob.size(), nullptr, &ps))) return false;
        std::vector<IUnknown*> keep;
        Float4 values[64];
        for (size_t i = 0; i < inputs.size() && i < 64; ++i) values[i] = inputs[i];
        ctx_->UpdateSubresource(vs_values_, 0, nullptr, values, 0, 0);
        for (const Interface::Tex& t : f.textures) {
            auto it = texture_colours.find(t.slot);
            Float4 colour = it != texture_colours.end() ? it->second : Float4{};
            ID3D11ShaderResourceView* srv = texture(t.dim, colour, &keep);
            if (!srv) return false;
            ctx_->PSSetShaderResources(t.slot, 1, &srv);
        }
        for (uint32_t s : f.samplers) ctx_->PSSetSamplers(s, 1, &sampler_);
        for (const auto& [slot, regs] : f.cbuffers) {
            std::vector<Float4> data(std::max<uint32_t>(regs, 1));
            auto it = constants.find(slot);
            if (it != constants.end())
                for (size_t i = 0; i < data.size() && i < it->second.size(); ++i) data[i] = it->second[i];
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = UINT(data.size() * 16);
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA init{data.data()};
            ID3D11Buffer* cb = nullptr;
            if (FAILED(dev_->CreateBuffer(&bd, &init, &cb))) return false;
            keep.push_back(cb);
            ctx_->PSSetConstantBuffers(slot, 1, &cb);
        }
        const float clear[4] = {-7.0f, -7.0f, -7.0f, -7.0f};
        ctx_->ClearRenderTargetView(rtv_, clear);
        ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
        D3D11_VIEWPORT vp{0, 0, 1, 1, 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->IASetInputLayout(nullptr);
        ctx_->VSSetShader(vs, nullptr, 0);
        ctx_->VSSetConstantBuffers(0, 1, &vs_values_);
        ctx_->PSSetShader(ps, nullptr, 0);
        ctx_->Draw(3, 0);
        ctx_->CopyResource(staging_, target_);
        D3D11_MAPPED_SUBRESOURCE m{};
        bool ok = SUCCEEDED(ctx_->Map(staging_, 0, D3D11_MAP_READ, 0, &m));
        if (ok) {
            memcpy(out->v, m.pData, 16);
            ctx_->Unmap(staging_, 0);
        }
        ctx_->ClearState();
        ps->Release();
        for (IUnknown* u : keep) u->Release();
        return ok;
    }

private:
    // SV_Position, then a float4 TEXCOORD<r> for each input register r (so each lands in its own register).
    ID3D11VertexShader* vertex_shader(uint32_t registers) {
        auto it = vs_.find(registers);
        if (it != vs_.end()) return it->second;
        std::ostringstream s;
        s << "cbuffer Inputs : register(b0) { float4 values[64]; };\nstruct O {\n    float4 pos : SV_Position;\n";
        for (uint32_t r = 1; r < registers; ++r) s << "    float4 t" << r << " : TEXCOORD" << r << ";\n";
        s << "};\nO main(uint id : SV_VertexID) {\n    O o;\n    const float2 uv = float2((id << 1) & 2, id & 2);\n"
             "    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);\n";
        for (uint32_t r = 1; r < registers; ++r) s << "    o.t" << r << " = values[" << r << "];\n";
        s << "    return o;\n}\n";
        const std::string src = s.str();
        ID3DBlob *code = nullptr, *err = nullptr;
        ID3D11VertexShader* vs = nullptr;
        if (SUCCEEDED(D3DCompile(src.data(), src.size(), "vs", nullptr, nullptr, "main", "vs_4_0", 0, 0, &code, &err)))
            dev_->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs);
        if (code) code->Release();
        if (err) err->Release();
        vs_[registers] = vs;
        return vs;
    }

    ID3D11ShaderResourceView* texture(uint32_t dim, const Float4& colour, std::vector<IUnknown*>* keep) {
        ID3D11ShaderResourceView* srv = nullptr;
        if (dim == 5) {  // texture3d
            D3D11_TEXTURE3D_DESC d{1, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
            D3D11_SUBRESOURCE_DATA init{colour.v, 16, 16};
            ID3D11Texture3D* t = nullptr;
            if (FAILED(dev_->CreateTexture3D(&d, &init, &t))) return nullptr;
            keep->push_back(t);
            if (FAILED(dev_->CreateShaderResourceView(t, nullptr, &srv))) return nullptr;
        } else {
            const bool cube = dim == 6;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = d.Height = 1;
            d.MipLevels = 1;
            d.ArraySize = cube ? 6 : 1;
            d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.SampleDesc.Count = 1;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
            D3D11_SUBRESOURCE_DATA init[6];
            for (auto& i : init) i = {colour.v, 16, 16};
            ID3D11Texture2D* t = nullptr;
            if (FAILED(dev_->CreateTexture2D(&d, init, &t))) return nullptr;
            keep->push_back(t);
            if (FAILED(dev_->CreateShaderResourceView(t, nullptr, &srv))) return nullptr;
        }
        keep->push_back(srv);
        return srv;
    }

    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    ID3D11Texture2D *target_ = nullptr, *staging_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11Buffer* vs_values_ = nullptr;
    std::map<uint32_t, ID3D11VertexShader*> vs_;
};

bool same(const Float4& a, const Float4& b, int comps = 4) {
    for (int k = 0; k < comps; ++k) {
        const float x = a.v[k], y = b.v[k];
        if (std::isnan(x) && std::isnan(y)) continue;
        if (std::fabs(x - y) > 1e-4f + 1e-4f * std::max(std::fabs(x), std::fabs(y))) return false;
    }
    return true;
}
bool cleared(const Float4& a) { return a.v[0] == -7.0f && a.v[1] == -7.0f && a.v[2] == -7.0f; }
std::string str(const Float4& a) {
    char b[96];
    snprintf(b, sizeof(b), "(%.5g %.5g %.5g %.5g)", a.v[0], a.v[1], a.v[2], a.v[3]);
    return b;
}

// One made-up setting: inputs, texture colours by role, constant buffers.
struct Setting {
    std::vector<Float4> inputs;
    std::map<uint32_t, Float4> textures;
    std::map<uint32_t, std::vector<Float4>> constants;
};

Setting made_up(const Interface& f, std::mt19937& rng) {
    std::uniform_real_distribution<float> u(0.05f, 1.0f);
    Setting s;
    s.inputs.resize(f.input_registers);
    for (auto& v : s.inputs)
        for (float& x : v.v) x = u(rng);
    for (const Interface::Tex& t : f.textures) {
        Float4 c;
        for (float& x : c.v) x = u(rng);
        if (t.role == Role::Albedo || t.role == Role::Other) c.v[3] = 1.0f;  // alpha tests and decal blends pass
        s.textures[t.slot] = c;
    }
    for (const auto& [slot, regs] : f.cbuffers) {
        std::vector<Float4> d(std::max<uint32_t>(regs, 1));
        for (auto& v : d)
            for (float& x : v.v) x = u(rng);
        if (slot == f.instance_cb) {  // tint 1, no explicit alpha test
            d[0] = Float4{{1, 1, 1, 1}};
            if (d.size() > 1) d[1] = Float4{};
        }
        s.constants[slot] = d;
    }
    return s;
}

Float4& reg(Setting& s, uint32_t slot, uint32_t r) {
    auto& d = s.constants[slot];
    if (d.size() <= r) d.resize(r + 1);
    return d[r];
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::path(L"research\\out\\shaders");
    const fs::path fxo = dir / "fxo";
    if (!fs::exists(fxo)) {
        fwprintf(stderr, L"%ls not found (research/scripts/shaders.py extract first)\n", fxo.c_str());
        return 2;
    }
    Runner runner;
    if (!runner.init()) {
        fprintf(stderr, "no WARP device\n");
        return 2;
    }
    ID3D11Device* dev = runner.device();
    std::mt19937 rng(20261114);
    size_t containers = 0, checksum_ok = 0, decoded = 0, mnemonics_ok = 0, created_original = 0;
    std::map<std::string, size_t> results, light_results;
    std::map<std::string, size_t> patched_by_family;
    std::vector<std::string> details, problems;
    size_t patched_created = 0, patched_disassembled = 0, light_created = 0;
    size_t checks_run = 0, checks_passed = 0, checks_vacuous = 0, bounce_seen = 0, light_checks = 0, light_passed = 0;
    std::map<std::string, size_t> refl_results;
    std::vector<std::string> refl_details;
    size_t refl_created = 0, refl_checks = 0, refl_passed = 0;
    std::map<std::string, size_t> floor_results;
    size_t floor_created = 0, floor_checks = 0, floor_passed = 0;
    std::vector<std::string> sun_term_shaders;
    std::map<std::string, size_t> specocc_results;
    size_t specocc_created = 0, specocc_checks = 0, specocc_passed = 0;
    std::map<std::string, size_t> fog_results;
    std::map<std::string, size_t> tint_results;
    size_t tint_created = 0, tint_checks = 0, tint_passed = 0;
    size_t fog_created = 0, fog_checks = 0, fog_passed = 0, fog_lit = 0;
    std::map<std::string, size_t> glass_results;
    std::vector<std::string> glass_shaders;
    size_t glass_created = 0, glass_checks = 0, glass_passed = 0, glass_fogged = 0;
    for (const auto& entry : fs::recursive_directory_iterator(fxo)) {
        if (entry.path().extension() != ".dxbc") continue;
        const std::vector<uint8_t> blob = read_file(entry.path());
        const std::string effect = entry.path().parent_path().filename().string();
        const std::string shader = effect + "/" + entry.path().stem().string();
        ++containers;
        if (blob.size() >= 32) {
            uint32_t hash[4];
            dxbc::checksum(blob.data(), blob.size(), hash);
            if (!memcmp(hash, blob.data() + 4, 16))
                ++checksum_ok;
            else
                problems.push_back(shader + ": checksum differs");
        }
        dxbc::Container c;
        dxbc::Program p;
        std::string error;
        const dxbc::Chunk* shdr = nullptr;
        if (dxbc::parse(blob.data(), blob.size(), &c, &error) && (shdr = c.find("SHDR")) != nullptr && dxbc::decode(*shdr, &p, &error)) {
            ++decoded;
            const std::vector<std::string> mine = decoded_mnemonics(p), theirs = mnemonics(disassemble(blob));
            if (mine == theirs) {
                ++mnemonics_ok;
            } else {
                size_t k = 0;
                while (k < mine.size() && k < theirs.size() && mine[k] == theirs[k]) ++k;
                problems.push_back(shader + ": instructions differ at " + std::to_string(k) + " (" + (k < mine.size() ? mine[k] : "end") +
                                   " / " + (k < theirs.size() ? theirs[k] : "end") + ")");
            }
        } else {
            problems.push_back(shader + ": " + error);
        }
        if (entry.path().stem().string().find("_ps") == std::string::npos) continue;
        ID3D11PixelShader* ps = nullptr;
        if (SUCCEEDED(dev->CreatePixelShader(blob.data(), blob.size(), nullptr, &ps))) {
            ++created_original;
            ps->Release();
        }

        // --- the light-pass patch ---
        {
            std::vector<uint8_t> patched;
            light_pass::Report report;
            const light_pass::Result r = light_pass::patch(blob.data(), blob.size(), &patched, &report);
            ++light_results[light_pass::name(r)];
            if (r == light_pass::Result::Unsupported) problems.push_back(shader + ": light pass not patched: " + report.reason);
            if (r == light_pass::Result::Patched) {
                const std::string text = disassemble(patched);
                if (!text.empty())
                    std::ofstream(entry.path().parent_path() / (entry.path().stem().string() + ".lightpass.asm"), std::ios::binary) << text;
                else
                    problems.push_back(shader + ": the light-pass patched shader does not disassemble");
                ps = nullptr;
                if (SUCCEEDED(dev->CreatePixelShader(patched.data(), patched.size(), nullptr, &ps))) {
                    ++light_created;
                    ps->Release();
                } else {
                    problems.push_back(shader + ": the light-pass patched shader is refused by CreatePixelShader");
                }
                Interface fo, fp;
                if (read_interface(blob, &fo) && read_interface(patched, &fp)) {
                    for (int trial = 0; trial < 3; ++trial) {
                        Setting s = made_up(fp, rng);
                        const float v = trial == 0 ? 0.0f : trial == 1 ? 1.0f : 0.37f;
                        s.textures[light_pass::kShadowSlot] = Float4{{0.8f, v, 0.5f, 1.0f}};
                        Float4 o, q;
                        ++light_checks;
                        if (!runner.run(blob, fo, s.inputs, s.textures, s.constants, &o) ||
                            !runner.run(patched, fp, s.inputs, s.textures, s.constants, &q)) {
                            problems.push_back(shader + ": light pass: a run failed");
                            continue;
                        }
                        Float4 want = o;
                        for (int k = 0; k < 3; ++k) want.v[k] = o.v[k] * v;
                        if (cleared(o) || same(q, want)) {
                            ++light_passed;
                        } else {
                            problems.push_back(shader + ": light pass: t15.y " + std::to_string(v) + ": " + str(q) + ", want " + str(want));
                        }
                    }
                }
            }
        }

        // --- the glass's patch (an unlit shader of the plain glass's make) ---
        {
            std::vector<uint8_t> gl;
            glass::Report gr;
            const glass::Result gres = glass::patch(blob.data(), blob.size(), &gl, &gr);
            ++glass_results[glass::name(gres)];
            if (gres == glass::Result::Unsupported) problems.push_back(shader + ": glass not patched: " + gr.reason);
            Interface fo, fg;
            if (gres == glass::Result::Patched) {
                glass_shaders.push_back(shader);
                const std::string text = disassemble(gl);
                if (!text.empty())
                    std::ofstream(entry.path().parent_path() / (entry.path().stem().string() + ".glass.asm"), std::ios::binary) << text;
                else
                    problems.push_back(shader + ": the glass-patched shader does not disassemble");
                ps = nullptr;
                if (SUCCEEDED(dev->CreatePixelShader(gl.data(), gl.size(), nullptr, &ps))) {
                    ++glass_created;
                    ps->Release();
                } else {
                    problems.push_back(shader + ": the glass-patched shader is refused by CreatePixelShader");
                }
                if (!read_interface(blob, &fo) || !read_interface(gl, &fg) || fo.light_cb == UINT32_MAX)
                    problems.push_back(shader + ": glass: interface unreadable");
            }
            if (gres == glass::Result::Patched && fo.light_cb != UINT32_MAX) {
                auto glass_check = [&](const char* what, Setting& so, Setting& sg) {
                    Float4 x, y;
                    ++glass_checks;
                    if (!runner.run(blob, fo, so.inputs, so.textures, so.constants, &x) ||
                        !runner.run(gl, fg, sg.inputs, sg.textures, sg.constants, &y)) {
                        problems.push_back(shader + ": glass " + what + ": a run failed");
                        return;
                    }
                    if ((cleared(x) && cleared(y)) || same(x, y)) {
                        ++glass_passed;
                        return;
                    }
                    problems.push_back(shader + ": glass " + what + ": original " + str(x) + ", patched " + str(y));
                };
                std::uniform_real_distribution<float> gu(0.05f, 1.0f);
                const Float4 no_reflection{{0.0f, 0.0f, 0.0f, 0.0f}};
                for (int trial = 0; trial < 3; ++trial) {
                    Setting s = made_up(fg, rng);
                    s.textures[glass::kGlassSlot] = Float4{{gu(rng), gu(rng), gu(rng), 0.0f}};
                    s.textures[glass::kGlassReflSlot] = no_reflection;
                    glass_check("GA (alpha 0: the glass's own light)", s, s);
                    // GE: what the glass reflects (F L, F) over the original's output (t17's alpha 0: the original)
                    {
                        Setting se = s;
                        const float F = gu(rng);
                        Float4 refl{{gu(rng) * F, gu(rng) * F, gu(rng) * F, F}};
                        se.textures[glass::kGlassReflSlot] = refl;
                        Float4 o, q;
                        ++glass_checks;
                        if (!runner.run(blob, fo, s.inputs, s.textures, s.constants, &o) ||
                            !runner.run(gl, fg, se.inputs, se.textures, se.constants, &q)) {
                            problems.push_back(shader + ": glass GE: a run failed");
                        } else {
                            const float a = o.v[3], a2 = a + F * (1.0f - a);
                            Float4 want = o;
                            want.v[3] = a2;
                            for (int k = 0; k < 3; ++k) want.v[k] = (o.v[k] * a + refl.v[k]) / (a2 > 1e-6f ? a2 : 1e-6f);
                            if ((cleared(o) && cleared(q)) || same(q, want))
                                ++glass_passed;
                            else
                                problems.push_back(shader + ": glass GE (a reflection): patched " + str(q) + ", want " + str(want));
                        }
                    }
                    // the light E whatever the normal, in the original as in t17 (its occlusion, ClbTexture's R, 1: the
                    // traced light holds it, the patched glass leaves it out)
                    s = made_up(fg, rng);
                    s.textures[glass::kGlassReflSlot] = no_reflection;
                    if (s.textures.count(15)) s.textures[15].v[0] = 1.0f;
                    Float4 E;
                    for (int k = 0; k < 3; ++k) E.v[k] = gu(rng);
                    E.v[3] = 1.0f;
                    const float ix = gu(rng);
                    reg(s, 0, 2) = E;
                    reg(s, 0, 3) = E;
                    reg(s, 0, 7).v[0] = ix;
                    for (int k = 0; k < 3; ++k) reg(s, fo.light_cb, 0).v[k] = E.v[k] / ix;
                    Setting sg = s;
                    sg.textures[glass::kGlassSlot] = Float4{{E.v[0], E.v[1], E.v[2], 2.0f}};
                    glass_check("GB (lit: the traced light)", s, sg);
                    // the highlight by linearity: O1 as s, O2 with twice the sun's colour and half the bounce factor
                    Setting s2 = s;
                    reg(s2, 0, 7).v[0] = ix * 0.5f;
                    for (int k = 0; k < 3; ++k) reg(s2, fo.light_cb, 0).v[k] = 2.0f * E.v[k] / ix;
                    Float4 o1, o2;
                    if (!runner.run(blob, fo, s.inputs, s.textures, s.constants, &o1) ||
                        !runner.run(blob, fo, s2.inputs, s2.textures, s2.constants, &o2)) {
                        problems.push_back(shader + ": glass GC/GD: a run failed");
                        continue;
                    }
                    for (const float vis : {0.0f, 0.5f}) {
                        Setting sv = s;
                        sv.textures[glass::kGlassSlot] = Float4{{E.v[0], E.v[1], E.v[2], 1.0f + vis}};
                        Float4 q, want = o1;
                        for (int k = 0; k < 3; ++k) want.v[k] = (2.0f - vis) * o1.v[k] - (1.0f - vis) * o2.v[k];
                        ++glass_checks;
                        if (!runner.run(gl, fg, sv.inputs, sv.textures, sv.constants, &q)) {
                            problems.push_back(shader + ": glass GC/GD: a run failed");
                            continue;
                        }
                        if ((cleared(o1) && cleared(q)) || same(q, want, 3)) {
                            ++glass_passed;
                        } else {
                            problems.push_back(shader + ": glass " + (vis == 0.0f ? "GC (in shadow)" : "GD (half the sun)") +
                                               ": patched " + str(q) + ", want " + str(want));
                        }
                    }
                }
                // GF: the fog at the glass (glass::patch's `fog`, then fog::patch on t21, as the plugin does with the
                // volumetric fog): with t21's alpha 0 it is the glass-patched shader (GF0); with t21 = (S, T), that shader
                // with the fog's input 0 (none of the game's), its colour times T plus S, its alpha as it was (GF1)
                {
                    std::vector<uint8_t> gf0, gff;
                    glass::Report gr2;
                    fog::Report fr2;
                    Interface ff;
                    if (glass::patch(blob.data(), blob.size(), &gf0, &gr2, true) != glass::Result::Patched ||
                        fog::patch(gf0.data(), gf0.size(), &gff, &fr2, glass::kGlassFogSlot) != fog::Result::Patched ||
                        !read_interface(gff, &ff)) {
                        problems.push_back(shader + ": glass GF: the fog at the glass not patched: " + gr2.reason + fr2.reason);
                    } else {
                        ++glass_fogged;
                        const std::string ftext = disassemble(gff);
                        if (!ftext.empty())
                            std::ofstream(entry.path().parent_path() / (entry.path().stem().string() + ".glassfog.asm"),
                                          std::ios::binary) << ftext;
                        else
                            problems.push_back(shader + ": the glass's fog-patched shader does not disassemble");
                        ID3D11PixelShader* fps = nullptr;
                        if (SUCCEEDED(dev->CreatePixelShader(gff.data(), gff.size(), nullptr, &fps)))
                            fps->Release();
                        else
                            problems.push_back(shader + ": the glass's fog-patched shader is refused by CreatePixelShader");
                        for (int trial = 0; trial < 3; ++trial) {
                            Setting s = made_up(ff, rng);
                            const float F = gu(rng);
                            s.textures[glass::kGlassSlot] = Float4{{gu(rng), gu(rng), gu(rng), trial == 0 ? 0.0f : 1.0f + gu(rng)}};
                            s.textures[glass::kGlassReflSlot] = Float4{{gu(rng) * F, gu(rng) * F, gu(rng) * F, F}};
                            s.textures[glass::kGlassFogSlot] = Float4{{0.0f, 0.0f, 0.0f, 0.0f}};
                            Float4 o, q;
                            ++glass_checks;
                            if (!runner.run(gl, fg, s.inputs, s.textures, s.constants, &o) ||
                                !runner.run(gff, ff, s.inputs, s.textures, s.constants, &q)) {
                                problems.push_back(shader + ": glass GF0: a run failed");
                            } else if ((cleared(o) && cleared(q)) || same(q, o)) {
                                ++glass_passed;
                            } else {
                                problems.push_back(shader + ": glass GF0 (alpha 0: the game's fog): glass-patched " + str(o) +
                                                   ", with the fog " + str(q));
                            }
                            Setting sf = s, none = s;
                            const float T = gu(rng);
                            const Float4 S{{gu(rng), gu(rng), gu(rng), T}};
                            sf.textures[glass::kGlassFogSlot] = S;
                            if (fr2.input < none.inputs.size()) none.inputs[fr2.input].v[fr2.component] = 0.0f;
                            ++glass_checks;
                            if (!runner.run(gl, fg, none.inputs, none.textures, none.constants, &o) ||
                                !runner.run(gff, ff, sf.inputs, sf.textures, sf.constants, &q)) {
                                problems.push_back(shader + ": glass GF1: a run failed");
                                continue;
                            }
                            Float4 want = o;
                            for (int k = 0; k < 3; ++k) want.v[k] = o.v[k] * T + S.v[k];
                            if ((cleared(o) && cleared(q)) || same(q, want))
                                ++glass_passed;
                            else
                                problems.push_back(shader + ": glass GF1 (the core's fog at the glass): patched " + str(q) +
                                                   ", want " + str(want));
                        }
                    }
                }
            }
        }

        // --- the shadow floor's patch, on the shader as the plugin has it by then ---
        std::vector<uint8_t> chain = blob;  // the shader as the plugin has it after the floor's patch
        {
            std::vector<uint8_t> base = blob, gi_out, refl_out;
            const bool gi_patched = ambient::patch(blob.data(), blob.size(), &gi_out, nullptr) == ambient::Result::Patched;
            if (gi_patched) {
                base = gi_out;
                if (ambient::patch_reflection(gi_out.data(), gi_out.size(), &refl_out, nullptr) == ambient::Result::Patched) base = refl_out;
            }
            std::vector<uint8_t> fl;
            shadow_floor::Report fr;
            const shadow_floor::Result fres = shadow_floor::patch(base.data(), base.size(), 0.0f, &fl, &fr);
            ++floor_results[shadow_floor::name(fres)];
            chain = fres == shadow_floor::Result::Patched ? fl : base;
            if (fres == shadow_floor::Result::Unsupported) problems.push_back(shader + ": shadow floor not patched: " + fr.reason);
            if (fres == shadow_floor::Result::Patched && fr.sun_terms)
                sun_term_shaders.push_back(shader + " (" + std::to_string(fr.sun_terms) + ")");
            Interface fb, ff;
            if (fres == shadow_floor::Result::Patched) {
                const std::string text = disassemble(fl);
                if (!text.empty())
                    std::ofstream(entry.path().parent_path() / (entry.path().stem().string() + ".floor.asm"), std::ios::binary) << text;
                else
                    problems.push_back(shader + ": the floor-patched shader does not disassemble");
                ps = nullptr;
                if (SUCCEEDED(dev->CreatePixelShader(fl.data(), fl.size(), nullptr, &ps))) {
                    ++floor_created;
                    ps->Release();
                } else {
                    problems.push_back(shader + ": the floor-patched shader is refused by CreatePixelShader");
                }
                if (!read_interface(base, &fb) || !read_interface(fl, &ff)) problems.push_back(shader + ": floor: interface unreadable");
            }
            if (fres == shadow_floor::Result::Patched && !ff.textures.empty()) {
                // the patched shader in `sf` against the one before it in `sb`
                auto floor_check = [&](const char* what, Setting& sb, Setting& sf) {
                    Float4 x, y;
                    ++floor_checks;
                    if (!runner.run(base, fb, sb.inputs, sb.textures, sb.constants, &x) ||
                        !runner.run(fl, ff, sf.inputs, sf.textures, sf.constants, &y)) {
                        problems.push_back(shader + ": floor " + what + ": a run failed");
                        return;
                    }
                    if ((cleared(x) && cleared(y)) || same(x, y)) {
                        ++floor_passed;
                        return;
                    }
                    problems.push_back(shader + ": floor " + what + ": before " + str(x) + ", patched " + str(y));
                };
                for (int trial = 0; trial < 3; ++trial) {
                    Setting s = made_up(ff, rng);
                    s.textures[ambient::kIrradianceSlot].v[3] = 0.0f;
                    floor_check("FA (alpha 0: the game's floor)", s, s);
                    s = made_up(ff, rng);
                    s.textures[ambient::kIrradianceSlot] = Float4{{0.0f, 0.0f, 0.0f, 1.0f}};  // (no bounce: the sun's terms as lit)
                    s.textures[15].v[1] = 1.0f;
                    floor_check("FB (lit: no floor needed)", s, s);
                    if (fb.light_cb != UINT32_MAX) {
                        s = made_up(ff, rng);
                        s.textures[ambient::kIrradianceSlot] = Float4{{0.0f, 0.0f, 0.0f, 1.0f}};  // (no bounce either)
                        s.textures[15].v[1] = 0.0f;
                        reg(s, 0, 7).v[0] = 0.0f;
                        Setting dark = s;
                        for (int k = 0; k < 3; ++k) reg(dark, fb.light_cb, 0).v[k] = 0.0f;
                        floor_check("FC (in shadow: nothing of the sun)", dark, s);
                    }
                }
            }
            // --- the sun through clear glass, on a base lit pass as the floor's patch left it ---
            if (gi_patched) {
                std::vector<uint8_t> tn;
                sun_tint::Report tr;
                const sun_tint::Result tres = sun_tint::patch(chain.data(), chain.size(), &tn, &tr);
                ++tint_results[sun_tint::name(tres)];
                if (tres == sun_tint::Result::Unsupported) problems.push_back(shader + ": sun tint not patched: " + tr.reason);
                Interface tc, tx;
                if (tres == sun_tint::Result::Patched) {
                    const std::string ttext = disassemble(tn);
                    if (!ttext.empty())
                        std::ofstream(entry.path().parent_path() / (entry.path().stem().string() + ".tint.asm"), std::ios::binary) << ttext;
                    else
                        problems.push_back(shader + ": the tint-patched shader does not disassemble");
                    ID3D11PixelShader* tps = nullptr;
                    if (SUCCEEDED(dev->CreatePixelShader(tn.data(), tn.size(), nullptr, &tps))) {
                        ++tint_created;
                        tps->Release();
                    } else {
                        problems.push_back(shader + ": the tint-patched shader is refused by CreatePixelShader");
                    }
                    if (!read_interface(chain, &tc) || !read_interface(tn, &tx) || tc.light_cb == UINT32_MAX)
                        problems.push_back(shader + ": tint: interface unreadable");
                    else {
                        // the patched shader in `sx` against the one before it in `sc`
                        auto tint_check = [&](const char* what, Setting& sc, Setting& sx) {
                            Float4 x, y;
                            ++tint_checks;
                            if (!runner.run(chain, tc, sc.inputs, sc.textures, sc.constants, &x) ||
                                !runner.run(tn, tx, sx.inputs, sx.textures, sx.constants, &y)) {
                                problems.push_back(shader + ": tint " + what + ": a run failed");
                                return;
                            }
                            if ((cleared(x) && cleared(y)) || same(x, y)) {
                                ++tint_passed;
                                return;
                            }
                            problems.push_back(shader + ": tint " + what + ": before " + str(x) + ", patched " + str(y));
                        };
                        for (int trial = 0; trial < 3; ++trial) {
                            Setting s = made_up(tx, rng);
                            s.textures[sun_tint::kSunTintSlot] = Float4{{0.3f, 0.6f, 0.9f, 0.0f}};
                            tint_check("TA (alpha 0: the game's sun)", s, s);
                            s = made_up(tx, rng);
                            s.textures[sun_tint::kSunTintSlot] = Float4{{1.0f, 1.0f, 1.0f, 1.0f}};
                            tint_check("TB (no glass: the sun as it is)", s, s);
                            s = made_up(tx, rng);
                            const float tint[3] = {0.25f, 0.6f, 0.9f};
                            s.textures[sun_tint::kSunTintSlot] = Float4{{tint[0], tint[1], tint[2], 1.0f}};
                            Setting before = s;
                            for (int k = 0; k < 3; ++k) reg(before, tc.light_cb, 0).v[k] *= tint[k];
                            tint_check("TC (tinted: the sun's colour times the tint)", before, s);
                        }
                    }
                    chain = tn;
                }
            }
        }

        // --- the fog's patch, last, on the shader as the plugin has it by then ---
        {
            std::vector<uint8_t> fg;
            fog::Report xr;
            const fog::Result xres = fog::patch(chain.data(), chain.size(), &fg, &xr);
            ++fog_results[fog::name(xres)];
            if (xres == fog::Result::Unsupported) problems.push_back(shader + ": fog not patched: " + xr.reason);
            if (xres == fog::Result::Patched) {
                if (xr.colour) ++fog_lit;
                const std::string text = disassemble(fg);
                if (!text.empty())
                    std::ofstream(entry.path().parent_path() / (entry.path().stem().string() + ".fog.asm"), std::ios::binary) << text;
                else
                    problems.push_back(shader + ": the fog-patched shader does not disassemble");
                ID3D11PixelShader* xps = nullptr;
                if (SUCCEEDED(dev->CreatePixelShader(fg.data(), fg.size(), nullptr, &xps))) {
                    ++fog_created;
                    xps->Release();
                } else {
                    problems.push_back(shader + ": the fog-patched shader is refused by CreatePixelShader");
                }
                Interface fc, fx;
                if (!read_interface(chain, &fc) || !read_interface(fg, &fx)) {
                    problems.push_back(shader + ": fog: interface unreadable");
                } else {
                    // the patched shader in `sx` against the one before it in `sc`
                    auto fog_check = [&](const char* what, Setting& sc, Setting& sx) {
                        Float4 x, y;
                        ++fog_checks;
                        if (!runner.run(chain, fc, sc.inputs, sc.textures, sc.constants, &x) ||
                            !runner.run(fg, fx, sx.inputs, sx.textures, sx.constants, &y)) {
                            problems.push_back(shader + ": fog " + what + ": a run failed");
                            return;
                        }
                        if ((cleared(x) && cleared(y)) || same(x, y)) {
                            ++fog_passed;
                            return;
                        }
                        problems.push_back(shader + ": fog " + what + ": before " + str(x) + ", patched " + str(y));
                    };
                    for (int trial = 0; trial < 3; ++trial) {
                        Setting s = made_up(fx, rng);
                        s.textures[fog::kFogSlot] = Float4{{0.3f, 0.2f, 0.1f, 0.0f}};
                        fog_check("XA (alpha 0: the game's fog)", s, s);
                        s = made_up(fx, rng);
                        s.textures[fog::kFogSlot] = Float4{{0.3f, 0.2f, 0.1f, 0.6f}};
                        Setting none = s;
                        if (xr.input < none.inputs.size()) none.inputs[xr.input].v[xr.component] = 0.0f;
                        fog_check("XB (the core's fog: none of the game's)", none, s);
                    }
                }
            }
        }

        // --- the ambient patch ---
        std::vector<uint8_t> patched;
        ambient::Report report;
        const ambient::Result r = ambient::patch(blob.data(), blob.size(), &patched, &report);
        ++results[ambient::name(r)];
        if (r == ambient::Result::Unsupported) problems.push_back(shader + ": not patched: " + report.reason + (report.ambient.empty() ? "" : " (" + report.ambient + ")"));
        if (r != ambient::Result::Patched) continue;
        std::string family = effect.substr(0, effect.find(".fxo_kg"));
        const size_t us = family.find_last_of('_');
        if (us != std::string::npos) family.resize(us);
        ++patched_by_family[family];
        details.push_back(shader + ": ambient at instruction " + std::to_string(report.ambient_index) + ": " + report.ambient +
                          (report.loose ? " (loose)" : ""));
        const std::string text = disassemble(patched);
        if (!text.empty()) {
            ++patched_disassembled;
            std::ofstream f(entry.path().parent_path() / (entry.path().stem().string() + ".patched.asm"), std::ios::binary);
            f << text;
        } else {
            problems.push_back(shader + ": the patched shader does not disassemble");
        }
        ps = nullptr;
        if (SUCCEEDED(dev->CreatePixelShader(patched.data(), patched.size(), nullptr, &ps))) {
            ++patched_created;
            ps->Release();
        } else {
            problems.push_back(shader + ": the patched shader is refused by CreatePixelShader");
            continue;
        }
        Interface fo, fp;
        if (!read_interface(blob, &fo) || !read_interface(patched, &fp) || fo.common_cb != 0) {
            problems.push_back(shader + ": its interface could not be read");
            continue;
        }
        std::uniform_real_distribution<float> u(0.05f, 1.0f);
        auto colour = [&] {
            Float4 c;
            for (int k = 0; k < 3; ++k) c.v[k] = u(rng);
            c.v[3] = 1.0f;
            return c;
        };
        // the original in setting `so` against the patched one in `sp` (or the patched one in both)
        auto check = [&](const char* what, Setting& so, Setting& sp, bool expect_same, bool patched_both = false) {
            Float4 o, q;
            ++checks_run;
            const bool ran = patched_both ? runner.run(patched, fp, so.inputs, so.textures, so.constants, &o)
                                          : runner.run(blob, fo, so.inputs, so.textures, so.constants, &o);
            if (!ran || !runner.run(patched, fp, sp.inputs, sp.textures, sp.constants, &q)) {
                problems.push_back(shader + ": " + what + ": a run failed");
                return false;
            }
            if (cleared(o) && cleared(q)) {
                ++checks_vacuous;
                ++checks_passed;
                return true;
            }
            if (same(o, q) == expect_same) {
                ++checks_passed;
                return true;
            }
            problems.push_back(shader + ": " + what + ": " + (patched_both ? "patched " : "original ") + str(o) + ", patched " + str(q));
            return false;
        };
        // the AO neutral (R = 1, as with GI on): a patch may take an AO multiplied in with the ambient
        auto ao_one = [&](Setting& s) {
            auto it = s.textures.find(15);
            if (it != s.textures.end()) it->second.v[0] = 1.0f;
        };
        for (int trial = 0; trial < 2; ++trial) {
            // A: V = S whatever the normal; the patched shader with E = S matches.
            {
                Setting s = made_up(fo, rng);
                ao_one(s);
                const Float4 S = colour();
                const float ix = u(rng);
                reg(s, 0, 2) = S;
                reg(s, 0, 3) = S;
                reg(s, 0, 7).v[0] = ix;
                if (fo.light_cb != UINT32_MAX)
                    for (int k = 0; k < 3; ++k) reg(s, fo.light_cb, 0).v[k] = S.v[k] / ix;
                Setting sp = s;
                sp.textures[ambient::kIrradianceSlot] = S;
                check("A (E = the ambient)", s, sp, true);
            }
            // B: only the bounce changes (indirectLightParams.x), the specular ambient off (its textures black, its
            // scale indirectLightParams.z 0): the patched colour stays.
            {
                Setting s = made_up(fo, rng);
                ao_one(s);
                for (const Interface::Tex& t : fo.textures)
                    if (t.role == Role::Specular) s.textures[t.slot] = Float4{{0, 0, 0, 1}};
                reg(s, 0, 7).v[2] = 0.0f;
                const Float4 E = colour();
                Setting s1 = s, s2 = s;
                reg(s2, 0, 7).v[0] = reg(s1, 0, 7).v[0] * 3.0f + 0.5f;
                Setting p1 = s1, p2 = s2;
                p1.textures[ambient::kIrradianceSlot] = E;
                p2.textures[ambient::kIrradianceSlot] = E;
                Float4 o1, o2;
                if (runner.run(blob, fo, s1.inputs, s1.textures, s1.constants, &o1) &&
                    runner.run(blob, fo, s2.inputs, s2.textures, s2.constants, &o2) && !same(o1, o2))
                    ++bounce_seen;
                check("B (the bounce is inside E)", p1, p2, true, true);
            }
            // C: albedo black: the patched shader matches for any E.
            {
                Setting s = made_up(fo, rng);
                ao_one(s);
                // (albedos from masks and colour constants too: colorshift's, oasisroof's Diffuse_Color)
                for (const Interface::Tex& t : fo.textures)
                    if (t.role == Role::Albedo || t.role == Role::Other) s.textures[t.slot] = Float4{{0, 0, 0, 1}};
                for (const auto& dc : fo.diffuse_colours) {
                    Float4& col = reg(s, dc[0], dc[1]);
                    col.v[0] = col.v[1] = col.v[2] = 0.0f;
                }
                for (const auto& si : fo.self_illum) reg(s, si[0], si[1]).v[si[2]] = 0.0f;
                Setting sp = s;
                sp.textures[ambient::kIrradianceSlot] = colour();
                check("C (the specular ambient is the game's)", s, sp, true);
            }
            // F: the irradiance's alpha 0 (no traced value: the core not answering, the sky's pixels): the patched
            // shader is the original, whatever the AO and the colour there.
            {
                Setting s = made_up(fo, rng);
                Setting sp = s;
                Float4 none = colour();
                none.v[3] = 0.0f;
                sp.textures[ambient::kIrradianceSlot] = none;
                check("F (alpha 0: the game's own ambient)", s, sp, true);
            }
        }

        // --- the reflections' patch, on the GI-patched shader (as the game will have it) ---
        std::vector<uint8_t> refl;
        ambient::Report rr;
        const ambient::Result rres = ambient::patch_reflection(patched.data(), patched.size(), &refl, &rr);
        ++refl_results[ambient::name(rres)];
        if (rres == ambient::Result::Unsupported) problems.push_back(shader + ": reflections not patched: " + rr.reason);
        if (rres != ambient::Result::Patched) {
            // the sky reflection's occlusion instead, on the GI-patched shader
            std::vector<uint8_t> so;
            spec_occlusion::Report sr;
            const spec_occlusion::Result sres = spec_occlusion::patch(patched.data(), patched.size(), &so, &sr);
            ++specocc_results[spec_occlusion::name(sres)];
            if (sres == spec_occlusion::Result::Unsupported) problems.push_back(shader + ": sky reflection not scaled: " + sr.reason);
            Interface fs;
            if (sres == spec_occlusion::Result::Patched && read_interface(so, &fs)) {
                const std::string stext = disassemble(so);
                if (!stext.empty())
                    std::ofstream(entry.path().parent_path() / (entry.path().stem().string() + ".specocc.asm"), std::ios::binary) << stext;
                ID3D11PixelShader* sps = nullptr;
                if (SUCCEEDED(dev->CreatePixelShader(so.data(), so.size(), nullptr, &sps))) {
                    ++specocc_created;
                    sps->Release();
                } else {
                    problems.push_back(shader + ": the sky-occlusion-patched shader is refused by CreatePixelShader");
                }
                auto spec_check = [&](const char* what, Setting& sb, Setting& ss) {
                    Float4 x, y;
                    ++specocc_checks;
                    if (!runner.run(patched, fp, sb.inputs, sb.textures, sb.constants, &x) ||
                        !runner.run(so, fs, ss.inputs, ss.textures, ss.constants, &y)) {
                        problems.push_back(shader + ": sky occlusion " + what + ": a run failed");
                        return;
                    }
                    if ((cleared(x) && cleared(y)) || same(x, y)) {
                        ++specocc_passed;
                        return;
                    }
                    problems.push_back(shader + ": sky occlusion " + what + ": before " + str(x) + ", patched " + str(y));
                };
                for (int trial = 0; trial < 3; ++trial) {
                    Setting s = made_up(fs, rng);
                    s.textures[ambient::kIrradianceSlot].v[3] = 0.0f;
                    spec_check("SA (alpha 0: the game's)", s, s);
                    s = made_up(fs, rng);
                    Float4& amb = reg(s, 0, 2);
                    Float4 E;
                    for (int k = 0; k < 3; ++k) E.v[k] = amb.v[k] * 1.5f + 0.01f;
                    E.v[3] = 1.0f;
                    s.textures[ambient::kIrradianceSlot] = E;
                    spec_check("SB (open: k = 1)", s, s);
                    s = made_up(fs, rng);
                    s.textures[ambient::kIrradianceSlot] = Float4{{0.0f, 0.0f, 0.0f, 1.0f}};
                    Setting none = s;
                    reg(none, 0, 7).v[2] = 0.0f;
                    spec_check("SC (dark: no sky reflection)", none, s);
                }
            }
            continue;
        }
        refl_details.push_back(shader + ": specular ambient at instruction " + std::to_string(rr.ambient_index) + ": " + rr.ambient +
                               (rr.loose ? " (loose)" : ""));
        {
            const std::string rtext = disassemble(refl);
            if (!rtext.empty())
                std::ofstream(entry.path().parent_path() / (entry.path().stem().string() + ".reflection.asm"), std::ios::binary) << rtext;
            ID3D11PixelShader* rps = nullptr;
            if (SUCCEEDED(dev->CreatePixelShader(refl.data(), refl.size(), nullptr, &rps))) {
                ++refl_created;
                rps->Release();
            } else {
                problems.push_back(shader + ": the reflection-patched shader is refused by CreatePixelShader");
                continue;
            }
        }
        Interface fr;
        if (!read_interface(refl, &fr)) {
            problems.push_back(shader + ": the reflection-patched shader's interface could not be read");
            continue;
        }
        // a against b, each in its own setting
        auto compare = [&](const char* what, const std::vector<uint8_t>& ba, const Interface& fa, Setting& sa,
                           const std::vector<uint8_t>& bb, const Interface& fb, Setting& sb) {
            Float4 x, y;
            ++refl_checks;
            if (!runner.run(ba, fa, sa.inputs, sa.textures, sa.constants, &x) || !runner.run(bb, fb, sb.inputs, sb.textures, sb.constants, &y)) {
                problems.push_back(shader + ": " + what + ": a run failed");
                return;
            }
            if ((cleared(x) && cleared(y)) || same(x, y)) {
                ++refl_passed;
                return;
            }
            problems.push_back(shader + ": " + what + ": " + str(x) + " against " + str(y));
        };
        for (int trial = 0; trial < 2; ++trial) {
            // RA: ambient = back = bounce = S: the sky is S along any direction; with E = S and the reflection = S,
            // the colour is the original's.
            {
                Setting s = made_up(fo, rng);
                ao_one(s);
                const Float4 S = colour();
                const float ix = u(rng);
                reg(s, 0, 2) = S;
                reg(s, 0, 3) = S;
                reg(s, 0, 7).v[0] = ix;
                if (fo.light_cb != UINT32_MAX)
                    for (int k = 0; k < 3; ++k) reg(s, fo.light_cb, 0).v[k] = S.v[k] / ix;
                Setting sr = s;
                sr.textures[ambient::kIrradianceSlot] = S;
                sr.textures[ambient::kReflectionSlot] = S;
                compare("RA (the reflection = the specular ambient)", blob, fo, s, refl, fr, sr);
            }
            // RB: only the bounce changes, E and the reflection kept: the colour stays (the bounce is inside both).
            {
                Setting s = made_up(fo, rng);
                ao_one(s);
                s.textures[ambient::kIrradianceSlot] = colour();
                s.textures[ambient::kReflectionSlot] = colour();
                Setting s2 = s;
                reg(s2, 0, 7).v[0] = reg(s, 0, 7).v[0] * 3.0f + 0.5f;
                compare("RB (the bounce is inside the reflection)", refl, fr, s, refl, fr, s2);
            }
            // RC: the specular off (its textures black, its colours 0): any reflection gives the GI-patched colour.
            {
                Setting s = made_up(fo, rng);
                ao_one(s);
                for (const Interface::Tex& tx : fo.textures)
                    if (tx.role == Role::Specular) s.textures[tx.slot] = Float4{{0, 0, 0, 1}};
                reg(s, 0, 7).v[2] = 0.0f;
                s.textures[ambient::kIrradianceSlot] = colour();
                Setting sr = s;
                sr.textures[ambient::kReflectionSlot] = colour();
                compare("RC (the diffuse is untouched)", patched, fp, s, refl, fr, sr);
            }
            // RF: the reflection's alpha 0: the GI-patched shader's colour (with the irradiance's alpha 0 too, in the
            // second trial: the original's).
            {
                Setting s = made_up(fo, rng);
                Float4 e = colour();
                if (trial == 1) e.v[3] = 0.0f;
                s.textures[ambient::kIrradianceSlot] = e;
                Setting sr = s;
                Float4 none = colour();
                none.v[3] = 0.0f;
                sr.textures[ambient::kReflectionSlot] = none;
                compare("RF (alpha 0: the game's own specular ambient)", patched, fp, s, refl, fr, sr);
                if (trial == 1) compare("RF0 (both alphas 0: the original)", blob, fo, s, refl, fr, sr);
            }
        }
    }
    std::ostringstream rep;
    rep << "containers " << containers << ", checksums recomputed equal " << checksum_ok << ", decoded " << decoded
        << ", instructions as D3DDisassemble's " << mnemonics_ok << ", original pixel shaders created " << created_original << "\n";
    rep << "ambient patch:";
    for (const auto& [k, v] : results) rep << " " << k << " " << v << ";";
    rep << " patched ones disassembled " << patched_disassembled << ", created " << patched_created << "\n";
    rep << "ambient patch run against the originals on WARP: " << checks_passed << " of " << checks_run << " checks agree (" << checks_vacuous
        << " with both discarded); the original's bounce seen to change its colour in " << bounce_seen << " settings\n";
    rep << "light-pass patch:";
    for (const auto& [k, v] : light_results) rep << " " << k << " " << v << ";";
    rep << " patched ones created " << light_created << "; run against the originals: " << light_passed << " of " << light_checks
        << " agree\n";
    rep << "reflections' patch (on the GI-patched shaders):";
    for (const auto& [k, v] : refl_results) rep << " " << k << " " << v << ";";
    rep << " patched ones created " << refl_created << "; " << refl_passed << " of " << refl_checks << " checks agree\n";
    rep << "shadow floor's patch (on the GI- and reflection-patched shaders):";
    for (const auto& [k, v] : floor_results) rep << " " << k << " " << v << ";";
    rep << " patched ones created " << floor_created << "; " << floor_passed << " of " << floor_checks << " checks agree\n";
    rep << "sky reflection's occlusion (on the GI-patched shaders the reflections' patch left):";
    for (const auto& [k, v] : specocc_results) rep << " " << k << " " << v << ";";
    rep << " patched ones created " << specocc_created << "; " << specocc_passed << " of " << specocc_checks << " checks agree\n";
    rep << "the sun's terms that never met the shadow, shadowed (" << sun_term_shaders.size() << " shaders):";
    for (const std::string& g : sun_term_shaders) rep << " " << g;
    rep << "\n";
    rep << "sun tint's patch (on the GI-patched shaders as the floor's left them):";
    for (const auto& [k, v] : tint_results) rep << " " << k << " " << v << ";";
    rep << " patched ones created " << tint_created << "; " << tint_passed << " of " << tint_checks << " checks agree\n";
    rep << "fog patch (last, on the shaders as patched by then):";
    for (const auto& [k, v] : fog_results) rep << " " << k << " " << v << ";";
    rep << " patched ones created " << fog_created << " (" << fog_lit << " adding the fog's colour); " << fog_passed << " of "
        << fog_checks << " checks agree\n";
    rep << "glass patch:";
    for (const auto& [k, v] : glass_results) rep << " " << k << " " << v << ";";
    rep << " patched ones created " << glass_created << "; the fog at the glass patched into " << glass_fogged << "; "
        << glass_passed << " of " << glass_checks << " checks agree; patched:";
    for (const std::string& g : glass_shaders) rep << " " << g;
    rep << "\n\npatched by family:\n";
    for (const auto& [k, v] : patched_by_family) rep << "  " << k << " " << v << "\n";
    rep << "\nproblems (" << problems.size() << "):\n";
    for (const std::string& s : problems) rep << "  " << s << "\n";
    rep << "\nthe ambient found in each patched shader:\n";
    for (const std::string& s : details) rep << "  " << s << "\n";
    rep << "\nthe specular ambient found in each:\n";
    for (const std::string& s : refl_details) rep << "  " << s << "\n";
    const std::string text = rep.str();
    std::ofstream(dir / "patch_report.txt", std::ios::binary) << text;
    fwrite(text.data(), 1, std::min<size_t>(text.size(), 8000), stdout);
    return 0;
}
