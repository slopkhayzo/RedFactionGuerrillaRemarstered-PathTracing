#include "render/sky_particles.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "core/active.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace sky_particles {
namespace {

constexpr int kAlwaysOnTop = 0x80;      // rl_particle_material_data::MATERIAL_FLAG_ALWAYS_ON_TOP
constexpr int kWithSky = 0x20000000;    // ours: a material of a Particles.WithSkyEffects effect (the game uses 0x1-0x400)

bool g_on_top = false;                  // Particles.AlwaysOnTopWithSky
std::vector<std::string> g_prefixes;    // Particles.WithSkyEffects, lower case
std::atomic<uint64_t> g_drawn{0};
std::atomic<uint32_t> g_tagged{0};
std::atomic<bool> g_told{false};

using RenderFn = void(__thiscall*)(void*, void*, void*, void*, void*, bool);
using SetZRangeFn = void(__thiscall*)(void*, float, float);
using GetZRangeFn = void(__thiscall*)(void*, float*, float*);
using InitFn = bool(__thiscall*)(void*);
RenderFn o_render = nullptr;
InitFn o_init = nullptr;

bool listed(const char* name) {
    for (const std::string& p : g_prefixes) {
        size_t i = 0;
        while (i < p.size() && name[i] && char(tolower(uint8_t(name[i]))) == p[i]) ++i;
        if (i == p.size()) return true;
    }
    return false;
}

// vfx_effect_template::init(): its emitters and their renderables made; a listed effect's materials tagged.
bool __fastcall d_init(RFG_THIS(void*, self)) {
    const bool ok = o_init(self);
    if (!ok || !self || g_prefixes.empty()) return ok;
    auto* data = rfg::field<uint8_t*>(self, rfg::off::VfxTemplate_data);
    const char* name = data ? rfg::field<const char*>(data, rfg::off::VfxData_effectName) : nullptr;
    if (!name || !listed(name)) return ok;
    const int count = rfg::field<int>(data, rfg::off::VfxData_numEmitters);
    auto* emitters = rfg::field<uint8_t*>(data, rfg::off::VfxData_emitters);
    if (!emitters || count <= 0 || count > 1024) return ok;
    for (int i = 0; i < count; ++i) {
        uint8_t* material = emitters + size_t(i) * rfg::off::VfxEmitter_size + rfg::off::VfxEmitter_material;
        rfg::field<int>(material, rfg::off::ParticleMaterial_flags) |= kWithSky;
    }
    if (g_tagged.fetch_add(1) < 32)
        LOG("particles: %s, its %d emitters drawn with the sky while path tracing (Particles.WithSkyEffects)", name, count);
    return ok;
}

// rl_particle_renderer::render(renderer, camera, instance, lighting_state, is_distortion_pass): one emitter's particles.
void __fastcall d_render(RFG_THIS(void*, self), void* renderer, void* camera, void* instance, void* lighting, bool distortion) {
    if (distortion || !renderer || !instance || !active::on()) return o_render(self, renderer, camera, instance, lighting, distortion);
    auto* renderable = rfg::field<uint8_t*>(instance, rfg::off::ParticlesInstance_renderable);
    auto* material = renderable ? rfg::field<uint8_t*>(renderable, rfg::off::Particles_material) : nullptr;
    if (!material) return o_render(self, renderer, camera, instance, lighting, distortion);
    int& flags = rfg::field<int>(material, rfg::off::ParticleMaterial_flags);
    const bool on_top = g_on_top && (flags & kAlwaysOnTop) != 0;
    if (!on_top && (flags & kWithSky) == 0) return o_render(self, renderer, camera, instance, lighting, distortion);
    float z_near = 0.0f, z_far = 1.0f;
    rfg::fn<GetZRangeFn>(rfg::rva::rl_renderer__get_viewport_z_range)(renderer, &z_near, &z_far);
    rfg::fn<SetZRangeFn>(rfg::rva::rl_renderer__set_viewport_z_range)(renderer, 1.0f, 1.0f);
    if (on_top) flags &= ~kAlwaysOnTop;
    o_render(self, renderer, camera, instance, lighting, distortion);
    if (on_top) flags |= kAlwaysOnTop;
    rfg::fn<SetZRangeFn>(rfg::rva::rl_renderer__set_viewport_z_range)(renderer, z_near, z_far);
    g_drawn.fetch_add(1, std::memory_order_relaxed);
    if (!g_told.exchange(true))
        LOG("particles: an emitter of the wind drawn with the sky (behind every world object; the game's depth range was "
            "[%g, %g])", z_near, z_far);
}

}  // namespace

bool install(const Config& cfg) {
    g_on_top = cfg.particles_always_on_top_with_sky;
    g_prefixes.clear();
    std::string list = cfg.particles_with_sky_effects;
    for (size_t at = 0; at <= list.size();) {
        size_t end = list.find(',', at);
        if (end == std::string::npos) end = list.size();
        std::string p = list.substr(at, end - at);
        while (!p.empty() && isspace(uint8_t(p.front()))) p.erase(p.begin());
        while (!p.empty() && isspace(uint8_t(p.back()))) p.pop_back();
        for (char& ch : p) ch = char(tolower(uint8_t(ch)));
        if (!p.empty()) g_prefixes.push_back(p);
        at = end + 1;
    }
    if (!g_on_top && g_prefixes.empty()) return true;
    if (!rfg::rva::rl_particle_renderer__render || !rfg::rva::rl_renderer__set_viewport_z_range ||
        !rfg::rva::rl_renderer__get_viewport_z_range || !rfg::off::ParticlesInstance_renderable) {
        LOG("particles: this build lacks the particle renderer's names: the wind's particles stay the game's");
        g_on_top = false;
        g_prefixes.clear();
        return false;
    }
    if (!g_prefixes.empty() && (!rfg::rva::vfx_effect_template__init || !rfg::off::VfxEmitter_size ||
                                !hooks::create_this(rfg::rva::vfx_effect_template__init, &d_init, &o_init, "vfx_effect_template::init"))) {
        LOG("particles: the effect templates' hook failed: Particles.WithSkyEffects left out");
        g_prefixes.clear();
        if (!g_on_top) return false;
    }
    if (!hooks::create_this(rfg::rva::rl_particle_renderer__render, &d_render, &o_render, "rl_particle_renderer::render")) {
        LOG("particles: the particle renderer's hook failed: the wind's particles stay the game's");
        g_on_top = false;
        g_prefixes.clear();
        return false;
    }
    LOG("particles: while path tracing, %s drawn with the sky, behind every world object",
        g_on_top && !g_prefixes.empty() ? "the always-on-top particles (the camera's wind dust) and Particles.WithSkyEffects' are"
        : g_on_top                       ? "the always-on-top particles (the camera's wind dust) are"
                                         : "Particles.WithSkyEffects' particles are");
    session::add_flush_handler(&report);
    return true;
}

void report(bool) {
    LOG("particles: emitters of the wind drawn with the sky %llu times; %u effects tagged",
        static_cast<unsigned long long>(g_drawn.load()), g_tagged.load());
}

}  // namespace sky_particles
