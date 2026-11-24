#include "render/gi.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "core/active.h"
#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "hooks/hooks.h"
#include "patch/ambient.h"
#include "patch/fog.h"
#include "patch/glass.h"
#include "patch/shadow_floor.h"
#include "patch/sun_tint.h"
#include "patch/spec_occlusion.h"
#include "render/seam.h"
#include "rt/link.h"
#include "render/lights.h"

namespace gi {
namespace {

Config g_cfg;
bool g_on = false;

std::atomic<uint32_t> g_patched{0}, g_lit_without_ambient{0}, g_unsupported{0}, g_other{0};
std::atomic<uint8_t*> g_renderer{nullptr}, g_scene{nullptr};

// The irradiance texture (the plugin's own while GI.Debug fills it).
struct Texture {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    uint32_t width = 0, height = 0;
} g_tex;
std::atomic<ID3D11ShaderResourceView*> g_bind{nullptr};  // what set_shadow_states binds at t14 (null: nothing)
std::atomic<ID3D11ShaderResourceView*> g_bind_refl{nullptr};  // and at t16 (the reflections)
bool g_refl = false;  // Reflections.Enabled: the specular ambient patched too
std::atomic<uint32_t> g_refl_patched{0}, g_refl_unsupported{0};
// The sun's specular in full shadow (Shadows.Specular, below the game's 0.05): patch/shadow_floor.h
bool g_floor = false;
std::atomic<uint32_t> g_floor_patched{0}, g_floor_unsupported{0}, g_sun_patched{0};
// The glass's light (GI.Glass): the core's at t17 for the patched glass shader (patch/glass.h)
bool g_glass = false;
std::atomic<ID3D11ShaderResourceView*> g_bind_glass{nullptr};
std::atomic<ID3D11ShaderResourceView*> g_bind_glass_refl{nullptr};  // what the clear glass reflects (t19)
std::atomic<ID3D11ShaderResourceView*> g_bind_tint{nullptr};  // the sun through clear glass (t20; null: read as 0, the game's sun)
bool g_tint = false;  // GI.WindowTint
std::atomic<uint32_t> g_tint_patched{0}, g_tint_unsupported{0};
bool g_glass_refl_shown = true;  // Test.GlassReflectionsAt toggles it
size_t g_glass_refl_next = 0;    // the next of its seconds
std::atomic<uint32_t> g_glass_patched{0}, g_glass_unsupported{0};
// The sky's reflection scaled by the traced light where the reflections' patch could not go (patch/spec_occlusion.h)
std::atomic<uint32_t> g_specocc_patched{0}, g_specocc_unsupported{0};
// The volumetric fog (Fog.Enabled): the core's at t18 from the frame's first lit pass (armed at render()'s start) to the
// end of its lighting passes, where it is blended over the scene (patch/fog.h)
bool g_fog = false;
std::atomic<ID3D11ShaderResourceView*> g_bind_fog{nullptr};
std::atomic<bool> g_fog_armed{false};
std::atomic<uint32_t> g_fog_patched{0}, g_fog_lit{0}, g_fog_unsupported{0};
std::atomic<uint64_t> g_fog_bound{0};  // lit passes that got the core's fog at t18
uint64_t g_fog_blended = 0, g_fog_size_differs = 0, g_fog_unarmed = 0;

// The main scene's passes (rl_scene_renderer: the scene thread's rl_renderer is not render()'s; null: not known yet)
bool main_scene(const void* scene_renderer) {
    const uint8_t* m = seam::main_scene_renderer();
    return !m || m == scene_renderer;
}

// The lighting constants of the main scene's lit passes (gi.h), written at each of them (scene thread), read by the
// frame scene's writer (a worker, at the next render()'s start).
std::mutex g_lit_mutex;
LitConstants g_lit{};
bool g_lit_have = false;
std::atomic<uint64_t> g_lit_captures{0};

void capture_lit(uint8_t* renderer) {
    const float* frag = reinterpret_cast<const float*>(renderer + rfg::off::Renderer_commonFragmentData);
    const float* vert = reinterpret_cast<const float*>(renderer + rfg::off::Renderer_commonVertexData);
    LitConstants v;
    memcpy(v.ambient, frag + 8, 16);
    memcpy(v.back_ambient, frag + 12, 16);
    memcpy(v.fog_color, frag + 16, 16);
    memcpy(v.fog_distance, vert + 52, 16);
    std::lock_guard<std::mutex> lock(g_lit_mutex);
    g_lit = v;
    g_lit_have = true;
    ++g_lit_captures;
}
std::atomic<uint64_t> g_bound{0};
uint64_t g_captured = 0;
uint64_t g_seams = 0, g_core_frames = 0;
bool g_logged_ambient = false;

void release(Texture& t) {
    // never released while the game may still draw with it: a resize keeps the old views (rare, small)
    t = Texture{};
}

bool make(ID3D11Device* dev, uint32_t w, uint32_t h) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    Texture t;
    t.width = w;
    t.height = h;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t.tex)) || FAILED(dev->CreateShaderResourceView(t.tex, nullptr, &t.srv)) ||
        FAILED(dev->CreateRenderTargetView(t.tex, nullptr, &t.rtv)))
        return false;
    g_tex = t;
    LOG("gi: irradiance texture %ux%u (R16G16B16A16_FLOAT) at t%u", w, h, ambient::kIrradianceSlot);
    return true;
}

// rl_scene_renderer::set_shadow_states(params, renderer): binds ClbTexture for a lit pass, through the renderer's
// command writer's D3D11 context; the irradiance goes next to it.
void(__thiscall* o_set_shadow_states)(void*, void*, uint8_t*);
// A dynamic light's pass gets its shadow mask at t15 instead (render/lights.h).
void __fastcall d_set_shadow_states(RFG_THIS(void*, self), void* params, uint8_t* renderer) {
    o_set_shadow_states(self, params, renderer);
    const active::Work work;
    if (!work.on) return;  // the game's original shaders: nothing of ours to bind
    if (g_on && renderer && main_scene(self)) capture_lit(renderer);  // the constants this lit pass draws with
    ID3D11ShaderResourceView* srv = g_on ? g_bind.load(std::memory_order_acquire) : nullptr;
    ID3D11ShaderResourceView* t15 = lights::t15_override();
    if ((!srv && !t15) || !renderer) return;
    uint8_t* writer = rfg::field<uint8_t*>(renderer, rfg::off::Renderer_commandWriter);
    void* inner = writer ? *reinterpret_cast<void**>(writer) : nullptr;
    auto* ctx = inner ? *reinterpret_cast<ID3D11DeviceContext**>(inner) : nullptr;
    if (!ctx) return;
    if (srv) {
        ctx->PSSetShaderResources(ambient::kIrradianceSlot, 1, &srv);
        ++g_bound;
    }
    if (ID3D11ShaderResourceView* refl = g_refl ? g_bind_refl.load(std::memory_order_acquire) : nullptr)
        ctx->PSSetShaderResources(ambient::kReflectionSlot, 1, &refl);
    // (the glass is drawn after the lit passes, without this call: the binding stays for it)
    if (ID3D11ShaderResourceView* gl = g_glass ? g_bind_glass.load(std::memory_order_acquire) : nullptr)
        ctx->PSSetShaderResources(glass::kGlassSlot, 1, &gl);
    if (ID3D11ShaderResourceView* gr = g_glass ? g_bind_glass_refl.load(std::memory_order_acquire) : nullptr)
        ctx->PSSetShaderResources(glass::kGlassReflSlot, 1, &gr);
    if (g_tint) {  // (null where the core has none: read as 0, the game's sun)
        ID3D11ShaderResourceView* st = g_bind_tint.load(std::memory_order_acquire);
        ctx->PSSetShaderResources(sun_tint::kSunTintSlot, 1, &st);
    }
    if (g_fog && main_scene(self)) {  // the main scene's lit passes, until their end blends the fog in
        ID3D11ShaderResourceView* f = g_fog_armed.load(std::memory_order_acquire) ? g_bind_fog.load(std::memory_order_acquire) : nullptr;
        ctx->PSSetShaderResources(fog::kFogSlot, 1, &f);
        if (f) ++g_fog_bound;
    }
    if (t15) ctx->PSSetShaderResources(15, 1, &t15);
}

}  // namespace

bool enabled() { return g_on; }

bool install(const Config& cfg) {
    g_cfg = cfg;
    g_on = cfg.gi_enabled;
    g_refl = cfg.gi_enabled && cfg.refl_enabled && cfg.host_enabled && cfg.host_mode == 3;
    g_floor = cfg.gi_enabled && cfg.host_enabled && cfg.host_mode == 3 && cfg.shadow_specular >= 0.0 && cfg.shadow_specular < 0.05;
    g_glass = cfg.gi_enabled && cfg.gi_glass && cfg.host_enabled && cfg.host_mode == 3;
    g_fog = cfg.gi_enabled && cfg.fog_enabled && cfg.gi_debug == 0 && cfg.host_enabled && cfg.host_mode == 3;
    g_tint = cfg.gi_enabled && cfg.gi_window_tint && cfg.gi_debug == 0 && cfg.host_enabled && cfg.host_mode == 3;
    if (!g_on && !lights::enabled()) return true;  // the hook below serves both (lights::install comes first)
    if (g_on) session::add_flush_handler(&report);
    const bool ok = hooks::create_this(rfg::rva::rl_scene_renderer__set_shadow_states, &d_set_shadow_states, &o_set_shadow_states,
                                       "rl_scene_renderer::set_shadow_states");
    if (g_on) LOG("gi: on (debug %d): lit base-pass shaders read their ambient from t%u", cfg.gi_debug, ambient::kIrradianceSlot);
    return ok;
}

void on_core_stop() {
    for (std::atomic<ID3D11ShaderResourceView*>* b : {&g_bind, &g_bind_refl, &g_bind_glass, &g_bind_glass_refl, &g_bind_tint, &g_bind_fog})
        b->store(nullptr, std::memory_order_release);
    g_fog_armed.store(false, std::memory_order_release);
}

void on_render(uint8_t* renderer, uint8_t* scene) {
    g_renderer = renderer;
    g_scene = scene;
    if (g_fog) g_fog_armed.store(true, std::memory_order_release);
}

void on_seam(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t width, uint32_t height, ID3D11ShaderResourceView* core_irradiance,
             ID3D11ShaderResourceView* core_reflection, ID3D11ShaderResourceView* core_glass,
             ID3D11ShaderResourceView* core_glass_refl, ID3D11ShaderResourceView* core_fog,
             ID3D11ShaderResourceView* core_sun_tint) {
    if (!g_on || !dev || !ctx || !width || !height) return;
    ++g_seams;
    // The next frame's lit passes get the fog at t18 (armed here, after this frame's recording ended, and at render()'s
    // start, whichever the scene thread's next recording follows; disarmed at the end of its lighting passes). The
    // core's fog only with its irradiance (the patched shaders leave the game's fog out only where it is there).
    if (g_fog) g_fog_armed.store(true, std::memory_order_release);
    g_bind_fog.store(g_fog && core_irradiance && g_cfg.gi_debug == 0 ? core_fog : nullptr, std::memory_order_release);
    g_bind_tint.store(g_tint && core_irradiance ? core_sun_tint : nullptr, std::memory_order_release);
    if (g_glass_refl_next < g_cfg.glass_refl_at.size() && session::gameplay_seconds() >= g_cfg.glass_refl_at[g_glass_refl_next]) {
        ++g_glass_refl_next;
        g_glass_refl_shown = !g_glass_refl_shown;
        LOG("gi: the clear glass's reflections %s (Test.GlassReflectionsAt)", g_glass_refl_shown ? "shown again" : "hidden");
    }
    if (core_irradiance && g_cfg.gi_debug == 0) {
        g_bind.store(core_irradiance, std::memory_order_release);
        g_bind_refl.store(core_reflection ? core_reflection : core_irradiance, std::memory_order_release);
        if (g_glass && core_glass) {
            g_bind_glass.store(core_glass, std::memory_order_release);
            ctx->PSSetShaderResources(glass::kGlassSlot, 1, &core_glass);
            // what it reflects (none: t19 unbound, read as 0: no reflection)
            ID3D11ShaderResourceView* gr = g_glass_refl_shown ? core_glass_refl : nullptr;  // (null: reads 0)
            g_bind_glass_refl.store(gr, std::memory_order_release);
            ctx->PSSetShaderResources(glass::kGlassReflSlot, 1, &gr);
        } else {
            g_bind_glass.store(nullptr, std::memory_order_release);
            g_bind_glass_refl.store(nullptr, std::memory_order_release);
        }
        ++g_core_frames;
        return;
    }
    if (g_tex.width != width || g_tex.height != height) {
        release(g_tex);
        if (!make(dev, width, height)) {
            LOG("gi: the irradiance texture could not be made: the patched shaders read nothing at t%u", ambient::kIrradianceSlot);
            g_on = false;
            g_bind = nullptr;
            return;
        }
    }
    // Nothing traced to bind (the core not answering yet, or stopped; or GI.Debug=2): alpha 0, where the patched shaders
    // keep the game's own ambient and specular ambient, exactly as the originals compute them. GI.Debug=1 fills magenta
    // (alpha 1: every patched surface's ambient), GI.Debug=3 black (no ambient light at all).
    const uint8_t* rr = g_renderer.load();
    const float* frag = rr ? reinterpret_cast<const float*>(rr + rfg::off::Renderer_commonFragmentData) : nullptr;
    if (!g_logged_ambient && g_scene.load()) {
        g_logged_ambient = true;
        const float* amb = reinterpret_cast<const float*>(g_scene.load() + rfg::off::Scene_ambientColor);
        const float* back = reinterpret_cast<const float*>(g_scene.load() + rfg::off::Scene_backAmbientColor);
        LOG("gi: rl_scene ambient (%.3f %.3f %.3f), back ambient (%.3f %.3f %.3f); the renderer's fragment constants: ambient "
            "(%.3f %.3f %.3f), back ambient (%.3f %.3f %.3f), indirect light params (%.3f %.3f %.3f %.3f)",
            amb[0], amb[1], amb[2], back[0], back[1], back[2], frag ? frag[8] : -1.f, frag ? frag[9] : -1.f,
            frag ? frag[10] : -1.f, frag ? frag[12] : -1.f, frag ? frag[13] : -1.f, frag ? frag[14] : -1.f, frag ? frag[28] : -1.f,
            frag ? frag[29] : -1.f, frag ? frag[30] : -1.f, frag ? frag[31] : -1.f);
    }
    float colour[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (g_cfg.gi_debug == 1) {
        colour[0] = 1.0f;
        colour[2] = 1.0f;
        colour[3] = 1.0f;
    } else if (g_cfg.gi_debug == 3) {
        colour[3] = 1.0f;  // no ambient at all: what still lights a surface is the sun, the lights, a glow or the fog
    }
    ctx->ClearRenderTargetView(g_tex.rtv, colour);
    g_bind.store(g_tex.srv, std::memory_order_release);
    g_bind_refl.store(g_tex.srv, std::memory_order_release);
    if (g_glass) {  // (alpha 0 there: the glass's own light, and no reflection)
        g_bind_glass.store(g_tex.srv, std::memory_order_release);
        ctx->PSSetShaderResources(glass::kGlassSlot, 1, &g_tex.srv);
        ID3D11ShaderResourceView* none = g_cfg.gi_debug == 0 ? g_tex.srv : nullptr;  // (the debug fills are not alpha 0)
        g_bind_glass_refl.store(none, std::memory_order_release);
        ctx->PSSetShaderResources(glass::kGlassReflSlot, 1, &none);
    }
}

void on_lit(uint8_t* scene_renderer, uint8_t* renderer) {
    if (!g_on || g_cfg.gi_debug != 0 || !renderer) return;
    rtlink::Link& link = rtlink::core_link();
    if (!link.ready()) return;
    uint8_t* writer = rfg::field<uint8_t*>(renderer, rfg::off::Renderer_commandWriter);
    void* inner = writer ? *reinterpret_cast<void**>(writer) : nullptr;
    auto* ctx = inner ? *reinterpret_cast<ID3D11DeviceContext**>(inner) : nullptr;
    if (!ctx) return;
    // the fog's texture leaves t18 now: what is drawn after (the transparents) keeps the game's fog
    const bool fog = g_fog && main_scene(scene_renderer) && g_fog_armed.exchange(false, std::memory_order_acq_rel);
    if (g_fog && !fog) ++g_fog_unarmed;
    if (fog) {
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(fog::kFogSlot, 1, &none);
    }
    // the target the light passes drew into: the scene's colour
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
        // the lit colour for the rays' hits first: without the fog (the patched shaders left the game's out where the
        // core's is)
        if (link.capture_radiance(ctx, tex, seam::frame_rendering())) ++g_captured;
        ID3D11ShaderResourceView* f = fog ? g_bind_fog.load(std::memory_order_acquire) : nullptr;
        if (f) {
            D3D11_TEXTURE2D_DESC td{}, fd{};
            tex->GetDesc(&td);
            ID3D11Resource* fr = nullptr;
            f->GetResource(&fr);
            ID3D11Texture2D* ft = nullptr;
            if (fr && SUCCEEDED(fr->QueryInterface(IID_PPV_ARGS(&ft)))) {
                ft->GetDesc(&fd);
                ft->Release();
            }
            if (fr) fr->Release();
            if (fd.Width == td.Width && fd.Height == td.Height) {
                seam::composite_fog(ctx, rtv, td.Width, td.Height, f);
                ++g_fog_blended;
            } else if (g_fog_size_differs++ < 4) {
                LOG("gi: the fog (%ux%u) is not the lit scene's size (%ux%u): not blended", fd.Width, fd.Height, td.Width, td.Height);
            }
        }
        tex->Release();
    }
    if (res) res->Release();
    rtv->Release();
}

bool lit_constants(LitConstants* out) {
    std::lock_guard<std::mutex> lock(g_lit_mutex);
    if (!g_lit_have) return false;
    *out = g_lit;
    return true;
}

bool patch_fog(const void* code, size_t size, std::vector<uint8_t>* out) {
    if (!g_fog) return false;
    std::vector<uint8_t> patched;
    fog::Report fr;
    const fog::Result f = fog::patch(code, size, &patched, &fr);
    if (f == fog::Result::Patched) {
        ++g_fog_patched;
        if (fr.colour) ++g_fog_lit;
        *out = std::move(patched);
        return true;
    }
    if (f == fog::Result::Unsupported && g_fog_unsupported.fetch_add(1) < 4) LOG("gi: a shader's fog not patched: %s", fr.reason.c_str());
    return false;
}

bool patch_pixel_shader(const void* code, size_t size, std::vector<uint8_t>* out) {
    if (!g_on) return false;
    // the glass's light first: a shader of the plain glass's make (rfg-glass, rfg-glass-etched) takes it in place of the
    // GI's irradiance, which is the light of what is behind it (patch/glass.h)
    if (g_glass) {
        glass::Report gr;
        const glass::Result gres = glass::patch(code, size, out, &gr);
        if (gres == glass::Result::Patched) {
            ++g_glass_patched;
            return true;
        }
        if (gres == glass::Result::Unsupported && g_glass_unsupported.fetch_add(1) < 4)
            LOG("gi: a glass shader not patched: %s", gr.reason.c_str());
    }
    ambient::Report report;
    const ambient::Result r = ambient::patch(code, size, out, &report);
    bool reflected = false;
    if (r == ambient::Result::Patched && g_refl) {  // the specular ambient too: reflections at t16
        std::vector<uint8_t> refl;
        ambient::Report rr;
        if (ambient::patch_reflection(out->data(), out->size(), &refl, &rr) == ambient::Result::Patched) {
            *out = std::move(refl);
            ++g_refl_patched;
            reflected = true;
        } else if (g_refl_unsupported.fetch_add(1) < 4) {
            LOG("gi: a lit shader's specular ambient not patched for reflections: %s", rr.reason.c_str());
        }
    }
    if (r == ambient::Result::Patched && !reflected) {  // its sky reflection, then, scaled by the light that reaches it
        std::vector<uint8_t> so;
        spec_occlusion::Report sr;
        const spec_occlusion::Result s = spec_occlusion::patch(out->data(), out->size(), &so, &sr);
        if (s == spec_occlusion::Result::Patched) {
            *out = std::move(so);
            ++g_specocc_patched;
        } else if (s == spec_occlusion::Result::Unsupported && g_specocc_unsupported.fetch_add(1) < 4) {
            LOG("gi: a lit shader's sky reflection not scaled: %s", sr.reason.c_str());
        }
    }
    switch (r) {
        case ambient::Result::Patched: ++g_patched; break;
        case ambient::Result::NoAmbient: ++g_lit_without_ambient; break;
        case ambient::Result::Unsupported: {
            if (g_unsupported.fetch_add(1) < 8) LOG("gi: a lit shader not patched: %s", report.reason.c_str());
            break;
        }
        default: ++g_other; break;
    }
    bool patched = r == ambient::Result::Patched;
    // the sun's specular in shadow: on the shader as patched so far, or the original (a lit pass without ambient)
    if (g_floor) {
        std::vector<uint8_t> fl;
        shadow_floor::Report fr;
        const shadow_floor::Result f = patched ? shadow_floor::patch(out->data(), out->size(), float(g_cfg.shadow_specular), &fl, &fr)
                                               : shadow_floor::patch(code, size, float(g_cfg.shadow_specular), &fl, &fr);
        if (f == shadow_floor::Result::Patched) {
            *out = std::move(fl);
            patched = true;
            ++g_floor_patched;
            if (fr.sun_terms) ++g_sun_patched;
        } else if (f == shadow_floor::Result::Unsupported && g_floor_unsupported.fetch_add(1) < 4) {
            LOG("gi: a lit shader's specular shadow floor not patched: %s", fr.reason.c_str());
        }
    }
    // the sun through clear glass (t20), on a base lit pass as patched so far: its sun's colour times what gets through
    if (g_tint && r == ambient::Result::Patched) {
        std::vector<uint8_t> tn;
        sun_tint::Report tr;
        const sun_tint::Result t = sun_tint::patch(out->data(), out->size(), &tn, &tr);
        if (t == sun_tint::Result::Patched) {
            *out = std::move(tn);
            ++g_tint_patched;
        } else if (t == sun_tint::Result::Unsupported && g_tint_unsupported.fetch_add(1) < 4) {
            LOG("gi: a lit shader's sun not tinted by the clear glass: %s", tr.reason.c_str());
        }
    }
    return patched;
}

void report(bool) {
    if (!g_on) return;
    LOG("gi: the lit colour copied for the rays' hits in %llu frames", static_cast<unsigned long long>(g_captured));
    if (g_refl) LOG("gi: reflections patched into %u shaders' specular ambient (%u not)", g_refl_patched.load(), g_refl_unsupported.load());
    LOG("gi: the sky's reflection scaled by the traced light in %u shaders (where the reflections could not go; %u not)",
        g_specocc_patched.load(), g_specocc_unsupported.load());
    if (g_glass)
        LOG("gi: the glass lit by the path tracer (t%u) in %u shaders (%u not patched)", glass::kGlassSlot, g_glass_patched.load(),
            g_glass_unsupported.load());
    if (g_tint)
        LOG("gi: the sun's light tinted by the clear glass it crosses (t%u) in %u shaders (%u not patched)", sun_tint::kSunTintSlot,
            g_tint_patched.load(), g_tint_unsupported.load());
    if (g_fog)
        LOG("gi: the volumetric fog (t%u): the game's fog left out where it is in %u shaders (%u lit passes adding its colour, "
            "the others lights' passes; %u not patched); bound for %llu lit passes, blended over the lit scene in %llu frames "
            "(%llu ends of the lighting passes found it unarmed)",
            fog::kFogSlot, g_fog_patched.load(), g_fog_lit.load(), g_fog_unsupported.load(),
            static_cast<unsigned long long>(g_fog_bound.load()), static_cast<unsigned long long>(g_fog_blended),
            static_cast<unsigned long long>(g_fog_unarmed));
    if (g_floor)
        LOG("gi: the sun's specular in full shadow %.3g where traced (the game's 0.05 elsewhere), patched into %u shaders (%u not); "
            "sun terms that never met the shadow shadowed in %u",
            g_cfg.shadow_specular, g_floor_patched.load(), g_floor_unsupported.load(), g_sun_patched.load());
    LOG("gi: pixel shaders patched %u (lit passes without ambient %u, not patched %u, others %u); t%u bound %llu times over %llu "
        "frames, %llu of them with the core's path-traced irradiance",
        g_patched.load(), g_lit_without_ambient.load(), g_unsupported.load(), g_other.load(), ambient::kIrradianceSlot,
        static_cast<unsigned long long>(g_bound.load()), static_cast<unsigned long long>(g_seams),
        static_cast<unsigned long long>(g_core_frames));
}

}  // namespace gi
