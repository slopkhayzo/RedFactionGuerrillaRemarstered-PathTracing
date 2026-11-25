#include "render/sky_particles.h"

#include <atomic>
#include <cstdint>

#include "core/active.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace sky_particles {
namespace {

constexpr int kAlwaysOnTop = 0x80;  // rl_particle_material_data::MATERIAL_FLAG_ALWAYS_ON_TOP

bool g_on = false;
std::atomic<uint64_t> g_drawn{0};
std::atomic<bool> g_told{false};

using RenderFn = void(__thiscall*)(void*, void*, void*, void*, void*, bool);
using SetZRangeFn = void(__thiscall*)(void*, float, float);
using GetZRangeFn = void(__thiscall*)(void*, float*, float*);
RenderFn o_render = nullptr;

// rl_particle_renderer::render(renderer, camera, instance, lighting_state, is_distortion_pass): one emitter's particles.
void __fastcall d_render(RFG_THIS(void*, self), void* renderer, void* camera, void* instance, void* lighting, bool distortion) {
    if (!g_on || distortion || !renderer || !instance || !active::on())
        return o_render(self, renderer, camera, instance, lighting, distortion);
    auto* renderable = rfg::field<uint8_t*>(instance, rfg::off::ParticlesInstance_renderable);
    auto* material = renderable ? rfg::field<uint8_t*>(renderable, rfg::off::Particles_material) : nullptr;
    if (!material || (rfg::field<int>(material, rfg::off::ParticleMaterial_flags) & kAlwaysOnTop) == 0)
        return o_render(self, renderer, camera, instance, lighting, distortion);
    int& flags = rfg::field<int>(material, rfg::off::ParticleMaterial_flags);
    float z_near = 0.0f, z_far = 1.0f;
    rfg::fn<GetZRangeFn>(rfg::rva::rl_renderer__get_viewport_z_range)(renderer, &z_near, &z_far);
    rfg::fn<SetZRangeFn>(rfg::rva::rl_renderer__set_viewport_z_range)(renderer, 1.0f, 1.0f);
    flags &= ~kAlwaysOnTop;
    o_render(self, renderer, camera, instance, lighting, distortion);
    flags |= kAlwaysOnTop;
    rfg::fn<SetZRangeFn>(rfg::rva::rl_renderer__set_viewport_z_range)(renderer, z_near, z_far);
    g_drawn.fetch_add(1, std::memory_order_relaxed);
    if (!g_told.exchange(true))
        LOG("particles: an always-on-top emitter drawn with the sky (behind every world object; the game's depth range "
            "was [%g, %g])", z_near, z_far);
}

}  // namespace

bool install(const Config& cfg) {
    g_on = cfg.particles_always_on_top_with_sky;
    if (!g_on) return true;
    if (!rfg::rva::rl_particle_renderer__render || !rfg::rva::rl_renderer__set_viewport_z_range ||
        !rfg::rva::rl_renderer__get_viewport_z_range || !rfg::off::ParticlesInstance_renderable) {
        LOG("particles: this build lacks the particle renderer's names: the always-on-top particles stay the game's");
        g_on = false;
        return false;
    }
    const bool ok = hooks::create_this(rfg::rva::rl_particle_renderer__render, &d_render, &o_render, "rl_particle_renderer::render");
    if (!ok) g_on = false;
    LOG("particles: %s", ok ? "while path tracing, the always-on-top particles (the camera's wind dust) are drawn with the "
                              "sky, behind every world object (Particles.AlwaysOnTopWithSky)"
                            : "the particle renderer's hook failed: the always-on-top particles stay the game's");
    if (ok) session::add_flush_handler(&report);
    return ok;
}

void report(bool) {
    if (!g_on) return;
    LOG("particles: always-on-top emitters drawn with the sky %llu times", static_cast<unsigned long long>(g_drawn.load()));
}

}  // namespace sky_particles
