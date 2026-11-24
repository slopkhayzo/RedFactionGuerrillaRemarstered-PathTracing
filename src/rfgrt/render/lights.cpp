#include "render/lights.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "core/active.h"
#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "frame_scene.h"
#include "game/rfg.h"
#include "hooks/hooks.h"
#include "patch/light_pass.h"
#include "render/gi.h"
#include "rfgrt_ipc.h"
#include "rt/link.h"

namespace lights {
namespace {

constexpr uint32_t kMaxCandidates = 256;  // visible lights looked at a frame

bool g_on = false;
float g_fixture = 0.3f;

// The frame being rendered: its lights, collected at render()'s start, written into its frame scene by the worker,
// bound in its lighting passes (all on the render thread after collect, but for the worker's copy).
uint8_t* g_frame_light[rfgrt_frame::kMaxLights] = {};
rfgrt_frame::Light g_frame_record[rfgrt_frame::kMaxLights] = {};
uint32_t g_frame_count = 0, g_frame_dropped = 0;

// Masks: the light holding each (kept while it stays listed), and whether the mask holds its visibility yet.
uint8_t* g_owner[rfgrt_ipc::kMaxLightMasks] = {};
bool g_valid[rfgrt_ipc::kMaxLightMasks] = {};
bool g_fresh[rfgrt_ipc::kMaxLightMasks] = {};  // given this frame
bool g_core_ok = false;                        // the core runs with the current frame textures

thread_local uint8_t* t_light = nullptr;  // the light set_light set up last (none for the main light)
thread_local bool t_in_passes = false;    // inside render_lighting_passes

// The scene renderer's visible lights as render_lighting_passes found them (the list is filled during render() and
// is empty at its start): the next frame's collect reads these, each under SEH (one may be gone by then).
uint8_t* g_seen[kMaxCandidates] = {};
uint32_t g_seen_count = 0;

std::atomic<uint64_t> g_patched{0}, g_reads_shadow{0}, g_unsupported{0};
std::atomic<uint64_t> g_bound_mask{0}, g_bound_white{0};
uint64_t g_frames = 0, g_listed = 0, g_dropped = 0, g_unanswered = 0, g_faults = 0, g_passes = 0;
uint64_t g_visible_sum = 0, g_visible_max = 0, g_light_passes = 0;

// One visible light as the frame scene takes it; false for what is not a point or spot light, or cannot be read (the
// list is the previous frame's: a light may be gone since).
bool read_light(uint8_t* light, const float offset[3], const float camera[3], rfgrt_frame::Light* out, float* distance2) {
    __try {
        const int type = *reinterpret_cast<const int*>(light + rfg::off::Light_type);
        if (type != 1 && type != 2) return false;  // point and spot lights (0: directional)
        const float* c = reinterpret_cast<const float*>(light + rfg::off::Light_color);
        if (!(c[0] > 0.0f || c[1] > 0.0f || c[2] > 0.0f)) return false;
        const float end = *reinterpret_cast<const float*>(light + rfg::off::Light_attenuationEnd);
        if (!(end > 0.05f && end < 5000.0f)) return false;
        const float* m = reinterpret_cast<const float*>(light + rfg::off::Light_worldMatrix);  // matrix43: rows, then position
        for (int k = 0; k < 3; ++k) {
            out->position[k] = m[9 + k] - offset[k];
            out->direction[k] = 0.0f;
        }
        if (!(std::fabs(out->position[0]) < 1e5f && std::fabs(out->position[1]) < 1e5f && std::fabs(out->position[2]) < 1e5f)) return false;
        out->radius = end;
        out->cos_cone = -1.0f;  // the cone is the light pass's own: outside it the light adds nothing, shadowed or not
        out->flags = uint32_t(type);
        out->source_radius = 0.0f;
        out->fixture = g_fixture;
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k) d2 += (out->position[k] - camera[k]) * (out->position[k] - camera[k]);
        *distance2 = d2;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
        return false;
    }
}

// list[i], or false when the list cannot be read
bool read_entry(uint8_t** data, int i, uint8_t** out) {
    __try {
        *out = data[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
        return false;
    }
}

ID3D11DeviceContext* context_of(uint8_t* renderer) {
    uint8_t* writer = renderer ? rfg::field<uint8_t*>(renderer, rfg::off::Renderer_commandWriter) : nullptr;
    void* inner = writer ? *reinterpret_cast<void**>(writer) : nullptr;
    return inner ? *reinterpret_cast<ID3D11DeviceContext**>(inner) : nullptr;
}

// rl_renderer::set_light(light, main): the light for the passes that follow (its constants); the main light's own
// passes keep ClbTexture.
void(__thiscall* o_set_light)(void*, uint8_t*, uint32_t);
void __fastcall d_set_light(RFG_THIS(void*, self), uint8_t* light, uint32_t main) {
    o_set_light(self, light, main);
    if (!active::on()) return;
    t_light = (main & 0xFF) ? nullptr : light;
    if (t_light && t_in_passes) ++g_light_passes;
}

// rl_scene_renderer::render_lighting_passes(renderer, camera, main light, ...): one additive pass per visible light,
// each after set_light and set_shadow_states. ClbTexture goes back to t15 afterwards.
void(__thiscall* o_lighting)(void*, uint8_t*, void*, void*, void*, void*, void*, void*);
void __fastcall d_lighting(RFG_THIS(void*, self), uint8_t* renderer, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6) {
    const active::Work work;
    if (!work.on) return o_lighting(self, renderer, a1, a2, a3, a4, a5, a6);
    struct LitAtEnd {  // the lit colour for the GI rays, once the passes are drawn; the volumetric fog (render/gi.h)
        uint8_t* scene_renderer;
        uint8_t* renderer;
        ~LitAtEnd() { gi::on_lit(scene_renderer, renderer); }
    } lit{static_cast<uint8_t*>(self), renderer};
    t_in_passes = g_on;
    t_light = nullptr;
    if (g_on) {  // the visible lights, for the next frame's masks
        uint8_t* list = static_cast<uint8_t*>(self) + rfg::off::SceneRenderer_visibleLights;
        uint8_t** data = rfg::field<uint8_t**>(list, rfg::off::LightList_data);
        const int count = rfg::field<int>(list, rfg::off::LightList_count);
        g_seen_count = 0;
        for (int i = 0; data && i < count && g_seen_count < kMaxCandidates; ++i) {
            uint8_t* light = nullptr;
            if (read_entry(data, i, &light) && light) g_seen[g_seen_count++] = light;
        }
        g_visible_sum += uint64_t(count > 0 ? count : 0);
        if (uint64_t(count) > g_visible_max && count > 0) g_visible_max = uint64_t(count);
    }
    o_lighting(self, renderer, a1, a2, a3, a4, a5, a6);
    const bool overrode = t_in_passes && g_core_ok;
    t_in_passes = false;
    t_light = nullptr;
    if (!overrode) return;
    ++g_passes;
    auto* clb = rfg::field<uint8_t*>(static_cast<uint8_t*>(self), rfg::off::SceneRenderer_clbTexture);
    auto* srv = clb ? rfg::field<ID3D11ShaderResourceView*>(clb, rfg::off::TextureData_pShaderView) : nullptr;
    ID3D11DeviceContext* ctx = context_of(renderer);
    if (ctx && srv) ctx->PSSetShaderResources(light_pass::kShadowSlot, 1, &srv);
}

}  // namespace

bool enabled() { return g_on; }

void on_core_stop() {
    for (uint32_t m = 0; m < rfgrt_ipc::kMaxLightMasks; ++m) {
        g_owner[m] = nullptr;
        g_valid[m] = false;
        g_fresh[m] = false;
    }
    g_core_ok = false;
    g_frame_count = 0;
}

bool install(const Config& cfg) {
    g_on = cfg.lights_enabled && cfg.host_enabled && cfg.host_mode == rfgrt_ipc::kModeSunShadows;
    g_fixture = float(cfg.lights_fixture);
    // the lighting passes' hook also gives GI the lit colour (render/gi.h)
    const bool gi_rays = cfg.gi_enabled && cfg.gi_debug == 0 && cfg.host_enabled && cfg.host_mode == rfgrt_ipc::kModeSunShadows;
    bool ok = true;
    if (g_on || gi_rays)
        ok &= hooks::create_this(rfg::rva::rl_scene_renderer__render_lighting_passes, &d_lighting, &o_lighting,
                                 "rl_scene_renderer::render_lighting_passes");
    if (!g_on) return ok;
    session::add_flush_handler(&report);
    ok &= hooks::create_this(rfg::rva::rl_renderer__set_light, &d_set_light, &o_set_light, "rl_renderer::set_light");
    LOG("lights: on: the visible point and spot lights get ray-traced shadow masks (up to %d), at t15 for their passes",
        cfg.lights_max);
    if (!ok) g_on = false;
    return ok;
}

void collect(uint8_t* scene_renderer, uint8_t* scene, uint8_t* camera) {
    g_frame_count = 0;
    g_frame_dropped = 0;
    if (!g_on || !scene_renderer || !scene || !camera) return;
    const int masks = rtlink::core_link().light_masks();
    if (masks <= 0) return;
    float offset[3], cam[3];
    memcpy(offset, scene + rfg::off::Scene_renderOffset, sizeof(offset));
    const float* cm = reinterpret_cast<const float*>(camera + rfg::off::Entity_worldMatrix);
    for (int k = 0; k < 3; ++k) cam[k] = cm[9 + k] - offset[k];
    // the visible lights of the previous frame's lighting passes (g_seen), nearest first
    struct Candidate {
        uint8_t* light;
        rfgrt_frame::Light record;
        float distance2;
    };
    static Candidate cands[kMaxCandidates];
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_seen_count && n < kMaxCandidates; ++i) {
        uint8_t* light = g_seen[i];
        Candidate& c = cands[n];
        if (read_light(light, offset, cam, &c.record, &c.distance2)) {
            c.light = light;
            ++n;
        }
    }
    std::sort(cands, cands + n, [](const Candidate& a, const Candidate& b) { return a.distance2 < b.distance2; });
    const uint32_t take = std::min<uint32_t>(n, uint32_t(masks));
    g_frame_dropped = n - take;
    // masks: kept by the lights that hold one, then given to the new ones
    bool kept[rfgrt_ipc::kMaxLightMasks] = {};
    int mask_of[rfgrt_frame::kMaxLights];
    for (uint32_t i = 0; i < take; ++i) {
        mask_of[i] = -1;
        for (int m = 0; m < masks; ++m)
            if (g_owner[m] == cands[i].light && !kept[m]) {
                mask_of[i] = m;
                kept[m] = true;
                break;
            }
    }
    for (uint32_t i = 0; i < take; ++i) {
        if (mask_of[i] >= 0) continue;
        for (int m = 0; m < masks; ++m)
            if (!kept[m]) {
                kept[m] = true;
                mask_of[i] = m;
                g_owner[m] = cands[i].light;
                g_valid[m] = false;
                g_fresh[m] = true;
                break;
            }
    }
    for (int m = 0; m < int(rfgrt_ipc::kMaxLightMasks); ++m)
        if (!kept[m]) {
            g_owner[m] = nullptr;
            g_valid[m] = false;
        }
    for (uint32_t i = 0; i < take; ++i) {
        g_frame_light[i] = cands[i].light;
        g_frame_record[i] = cands[i].record;
        g_frame_record[i].mask = uint32_t(mask_of[i]);
    }
    g_frame_count = take;
    ++g_frames;
    g_listed += take;
    g_dropped += g_frame_dropped;
}

void write(rfgrt_frame::SlotHeader* h) {
    if (!h) return;
    h->light_count = g_on ? g_frame_count : 0;
    h->lights_dropped = g_on ? g_frame_dropped : 0;
    h->pad4[0] = h->pad4[1] = 0;
    if (h->light_count) memcpy(h->lights, g_frame_record, sizeof(rfgrt_frame::Light) * h->light_count);
}

void on_seam(bool core_answered, bool core_ok) {
    if (!g_on) return;
    g_core_ok = core_ok;
    for (uint32_t m = 0; m < rfgrt_ipc::kMaxLightMasks; ++m) {
        if (core_answered && g_owner[m]) g_valid[m] = true;
        g_fresh[m] = false;
    }
    if (!core_answered) ++g_unanswered;
}

ID3D11ShaderResourceView* t15_override() {
    if (!g_on || !t_in_passes || !g_core_ok) return nullptr;
    rtlink::Link& link = rtlink::core_link();
    ID3D11ShaderResourceView* white = link.white_srv();
    if (!white) return nullptr;  // no frame textures yet: the game's own (ClbTexture)
    if (uint8_t* light = t_light) {
        for (int m = 0; m < link.light_masks(); ++m)
            if (g_owner[m] == light && g_valid[m]) {
                ++g_bound_mask;
                return link.light_mask_srv(m);
            }
    }
    ++g_bound_white;
    return white;
}

bool patch_pixel_shader(const void* code, size_t size, std::vector<uint8_t>* out) {
    if (!g_on) return false;
    light_pass::Report report;
    switch (light_pass::patch(code, size, out, &report)) {
        case light_pass::Result::Patched: ++g_patched; return true;
        case light_pass::Result::ReadsShadow: ++g_reads_shadow; return false;
        case light_pass::Result::Unsupported:
            if (g_unsupported.fetch_add(1) < 8) LOG("lights: a light-pass shader not patched: %s", report.reason.c_str());
            return false;
        default: return false;
    }
}

void report(bool) {
    if (!g_on) return;
    LOG("lights: the lighting passes saw %.1f visible lights a frame (at most %llu), %llu light passes set up; light-pass "
        "shaders patched %llu (already reading t15 %llu, not patched %llu); %llu frames with %.1f lights "
        "listed a frame (%llu beyond the masks), %llu frames the core did not answer; passes: %llu masks and %llu white "
        "bound at t15, ClbTexture restored after %llu lighting passes; %llu unreadable lights",
        g_passes ? double(g_visible_sum) / double(g_passes) : 0.0, static_cast<unsigned long long>(g_visible_max),
        static_cast<unsigned long long>(g_light_passes),
        static_cast<unsigned long long>(g_patched.load()), static_cast<unsigned long long>(g_reads_shadow.load()),
        static_cast<unsigned long long>(g_unsupported.load()), static_cast<unsigned long long>(g_frames),
        g_frames ? double(g_listed) / double(g_frames) : 0.0, static_cast<unsigned long long>(g_dropped),
        static_cast<unsigned long long>(g_unanswered), static_cast<unsigned long long>(g_bound_mask.load()),
        static_cast<unsigned long long>(g_bound_white.load()), static_cast<unsigned long long>(g_passes),
        static_cast<unsigned long long>(g_faults));
}

}  // namespace lights
