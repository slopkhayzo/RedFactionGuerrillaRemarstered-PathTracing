#include "render/seam.h"

#include <windows.h>
#include <d3d11_4.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "harness/screenshot.h"
#include "hooks/hooks.h"
#include "mirror/instances.h"
#include "mirror/textures.h"
#include "render/d3d_state.h"
#include "render/gi.h"
#include "render/lights.h"
#include "rt/link.h"

// Shader bytecode compiled by fxc at build time.
#include "apply_result_ps.h"
#include "copy_depth_ps.h"
#include "debug_view_ps.h"
#include "radiance_view_ps.h"
#include "fog_composite_ps.h"
#include "fullscreen_vs.h"
#include "pattern_ps.h"

namespace seam {
namespace {

Config g_cfg;
std::atomic<bool> g_skip_cascades{false};  // decided at a seam, used by the next frame's clb_render
// The cascades' casters are left out of this clb_render (its render_shadow_pass calls, on this thread), and ClbTexture's
// sun-shadow channel holds only the game's cloud shadows when the seam comes: ours is multiplied into it.
thread_local bool t_drop_casters = false;
std::atomic<bool> g_casters_dropped{false};
uint64_t g_frames_dropped = 0;
// The game's SSAO (renderSsaoEffect, on the scene thread after the seam) writes ClbTexture's R, where the core's ambient
// occlusion goes: it is skipped while ours fills R (decided at a seam, used by the next frame's).
std::atomic<bool> g_skip_ssao{false};
std::atomic<uint64_t> g_ssao_skipped{0};
bool g_ao = false;  // the core computes ambient occlusion (AO.Enabled with Host.Mode=3) and R is ours

// Our D3D11 objects, made on the first seam.
struct Gpu {
    bool ready = false, failed = false;
    ID3D11Device* device = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* pattern_ps = nullptr;
    ID3D11PixelShader* copy_depth_ps = nullptr;
    ID3D11PixelShader* apply_ps = nullptr;
    ID3D11PixelShader* debug_ps = nullptr;
    ID3D11PixelShader* radiance_ps = nullptr;  // DebugView 3
    ID3D11PixelShader* fog_ps = nullptr;       // the volumetric fog over the lit scene
    ID3D11BlendState* fog_blend = nullptr;     // ONE, SRC_ALPHA: the scene times the fog's transmittance plus its light
    ID3D11BlendState* blend_alpha = nullptr;  // the debug view over the frame
    ID3D11Buffer* cb = nullptr;
    ID3D11BlendState* write_r = nullptr;   // AO only
    ID3D11BlendState* write_g = nullptr;   // sun shadow only
    ID3D11BlendState* mul_g = nullptr;     // sun shadow multiplied into the game's (its cloud shadows)
    ID3D11BlendState* write_rg = nullptr;
    ID3D11BlendState* write_all = nullptr;
    ID3D11DepthStencilState* no_depth = nullptr;
    ID3D11RasterizerState* no_cull = nullptr;
} g_gpu;

bool g_host_off = false;  // the core failed or died: the game's own ClbTexture from then on
bool g_vanilla = false;   // Test.VanillaKey: the game's own lighting and shadows shown, the core kept in step

// Test.VanillaKey: a press while the game's window has the keyboard switches g_vanilla (render thread).
void poll_vanilla_key() {
    static size_t next_at = 0;  // Test.VanillaAt: the next of its seconds
    if (next_at < g_cfg.vanilla_at.size() && session::gameplay_seconds() >= g_cfg.vanilla_at[next_at]) {
        ++next_at;
        g_vanilla = !g_vanilla;
        LOG("seam: %s (Test.VanillaAt)", g_vanilla ? "the game's own lighting and shadows shown" : "the path tracing shown again");
    }
    static bool was_down = false;
    const int key = g_cfg.vanilla_key;
    if (key <= 0 || key >= 256) return;
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    if (down && !was_down) {
        DWORD pid = 0;
        const HWND fg = GetForegroundWindow();
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) {
            g_vanilla = !g_vanilla;
            LOG("seam: %s (Test.VanillaKey)", g_vanilla ? "the game's own lighting and shadows shown" : "the path tracing shown again");
        }
    }
    was_down = down;
}
int64_t g_frame = 0;      // frames exchanged with the core (fence values)
bool g_killed_for_test = false;
int g_since_answer = 1 << 30;      // frames since the core's result was last applied
constexpr int kCoverMisses = 4;     // the most frames in a row the core's last result stands in for

// The core's last applied result (ClbTexture's R and G as it gave them), in a texture of the plugin's own: a frame the
// core misses after its cascades went without casters gets it, a frame old, rather than no sun shadow at all.
struct LastResult {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
} g_last;

void keep_result(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* result) {
    if (!result) return;
    ID3D11Resource* res = nullptr;
    result->GetResource(&res);
    ID3D11Texture2D* src = nullptr;
    if (res) {
        res->QueryInterface(IID_PPV_ARGS(&src));
        res->Release();
    }
    if (!src) return;
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    if (!g_last.tex || g_last.width != d.Width || g_last.height != d.Height || g_last.format != d.Format) {
        if (g_last.srv) g_last.srv->Release();
        if (g_last.tex) g_last.tex->Release();
        g_last = LastResult{};
        D3D11_TEXTURE2D_DESC c = d;
        c.MipLevels = 1;
        c.ArraySize = 1;
        c.SampleDesc = {1, 0};
        c.Usage = D3D11_USAGE_DEFAULT;
        c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        c.CPUAccessFlags = 0;
        c.MiscFlags = 0;
        if (SUCCEEDED(dev->CreateTexture2D(&c, nullptr, &g_last.tex)) &&
            SUCCEEDED(dev->CreateShaderResourceView(g_last.tex, nullptr, &g_last.srv))) {
            g_last.width = d.Width;
            g_last.height = d.Height;
            g_last.format = d.Format;
        } else {
            if (g_last.tex) g_last.tex->Release();
            g_last = LastResult{};
            LOG("seam: no copy of the core's result could be made: a missed frame has no ray-traced shadow");
        }
    }
    if (g_last.tex) ctx->CopyResource(g_last.tex, src);
    src->Release();
}

struct Stats {
    uint64_t seams = 0, applied = 0, missed = 0, stale = 0, patterns = 0, others = 0;
    uint64_t waits = 0, wait_timeouts = 0;  // frames that waited for the core to queue them; of them, given up
    int64_t wait_ticks = 0;
    int64_t since = 0;
    // the render thread's time in rl_scene_renderer::render (the scene, the cascades among it), and between frames
    int64_t render_ticks = 0, render_max = 0;
    uint64_t renders = 0;
    int64_t last_render_start = 0, interval_ticks = 0;
    uint64_t intervals = 0;
} g_stats;

// The frame's scene is written on a worker thread while render() runs: the game's render data stays still for all of
// render() (the sim thread changes it only inside the next handshake, which waits for render() to return), and
// render() waits for the worker before it returns. The render thread only hands the job over.
struct SnapshotWorker {
    HANDLE thread = nullptr, start = nullptr, done = nullptr;
    uint8_t *renderer = nullptr, *scene = nullptr, *camera = nullptr;
    int64_t frame = 0;
    int64_t wait_ticks = 0;  // the render thread's waits for it, for the log
    uint64_t waits = 0;
} g_worker;

// The seam follows one scene renderer, the first one seen (the main view). rl_scene_renderer::render is virtual, so
// whether the game runs others (a second view) is only known at run time: they are logged and left alone.
uint8_t* g_main_scene = nullptr;

void note_other_scene(uint8_t* scene) {
    static uint8_t* seen[4] = {};
    for (uint8_t*& s : seen) {
        if (s == scene) return;
        if (!s) {
            s = scene;
            LOG("seam: another scene renderer %p (main scene %dx%d): left alone, the seam follows %p", static_cast<void*>(scene),
                rfg::field<int32_t>(scene, rfg::off::SceneRenderer_mainWidth),
                rfg::field<int32_t>(scene, rfg::off::SceneRenderer_mainHeight), static_cast<void*>(g_main_scene));
            return;
        }
    }
}

void(__cdecl* o_end_command_buffer)(uint8_t*, void*);
void(__thiscall* o_clb_render)(void*, void*, void*, void*, void*, void*, void**, int);
void(__thiscall* o_shadow_pass)(void*, void*, void*, void*, void*, uint32_t, uint32_t, void*, void*, uint32_t, void**, uint32_t,
                                uint32_t);
void(__thiscall* o_scene_render)(void*, void*, void*, void*, void*, void*, void*);

// The scene renderer rl_scene_renderer::render is running for, on the render thread (null outside it). The game's own
// Current_scene_renderer is cleared before render() reaches the seam.
thread_local uint8_t* t_scene_renderer = nullptr;
// The frame number this render() exchanges with the core: given at its start, with the frame's scene, when frame
// scenes are on (0: no exchange this frame); given at the seam otherwise.
thread_local int64_t t_frame = 0;
std::atomic<int64_t> g_frame_rendering{0};  // t_frame, for the scene thread recording the same frame
bool g_snapshots = false;

// Logs the first time a seam is skipped for a given reason.
void skipped(const char* why) {
    static const char* last = nullptr;
    if (why != last) {
        LOG("seam: skipped: %s", why);
        last = why;
    }
}

bool make_gpu(ID3D11Device* dev) {
    g_gpu.device = dev;
    HRESULT hr = dev->CreateVertexShader(g_fullscreen_vs, sizeof(g_fullscreen_vs), nullptr, &g_gpu.vs);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(g_pattern_ps, sizeof(g_pattern_ps), nullptr, &g_gpu.pattern_ps);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(g_copy_depth_ps, sizeof(g_copy_depth_ps), nullptr, &g_gpu.copy_depth_ps);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(g_apply_result_ps, sizeof(g_apply_result_ps), nullptr, &g_gpu.apply_ps);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(g_debug_view_ps, sizeof(g_debug_view_ps), nullptr, &g_gpu.debug_ps);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(g_radiance_view_ps, sizeof(g_radiance_view_ps), nullptr, &g_gpu.radiance_ps);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(g_fog_composite_ps, sizeof(g_fog_composite_ps), nullptr, &g_gpu.fog_ps);
    if (SUCCEEDED(hr)) {
        D3D11_BLEND_DESC d{};
        d.RenderTarget[0].BlendEnable = TRUE;
        d.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_ALPHA;
        d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                                  D3D11_COLOR_WRITE_ENABLE_BLUE;
        hr = dev->CreateBlendState(&d, &g_gpu.fog_blend);
    }
    if (SUCCEEDED(hr)) {
        D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        hr = dev->CreateBuffer(&bd, nullptr, &g_gpu.cb);
    }
    auto blend = [&](UINT8 mask, ID3D11BlendState** out) {
        D3D11_BLEND_DESC d{};
        d.RenderTarget[0].BlendEnable = FALSE;
        d.RenderTarget[0].RenderTargetWriteMask = mask;
        return dev->CreateBlendState(&d, out);
    };
    if (SUCCEEDED(hr)) hr = blend(D3D11_COLOR_WRITE_ENABLE_RED, &g_gpu.write_r);
    if (SUCCEEDED(hr)) hr = blend(D3D11_COLOR_WRITE_ENABLE_GREEN, &g_gpu.write_g);
    if (SUCCEEDED(hr)) hr = blend(D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN, &g_gpu.write_rg);
    if (SUCCEEDED(hr)) hr = blend(D3D11_COLOR_WRITE_ENABLE_ALL, &g_gpu.write_all);
    if (SUCCEEDED(hr)) {
        D3D11_BLEND_DESC d{};  // dest * src
        d.RenderTarget[0].BlendEnable = TRUE;
        d.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
        d.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
        d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_GREEN;
        hr = dev->CreateBlendState(&d, &g_gpu.mul_g);
    }
    if (SUCCEEDED(hr)) {
        D3D11_BLEND_DESC d{};
        d.RenderTarget[0].BlendEnable = TRUE;
        d.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        d.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                                  D3D11_COLOR_WRITE_ENABLE_BLUE;
        hr = dev->CreateBlendState(&d, &g_gpu.blend_alpha);
    }
    if (SUCCEEDED(hr)) {
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = FALSE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        hr = dev->CreateDepthStencilState(&d, &g_gpu.no_depth);
    }
    if (SUCCEEDED(hr)) {
        D3D11_RASTERIZER_DESC d{};
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_NONE;
        d.DepthClipEnable = TRUE;
        hr = dev->CreateRasterizerState(&d, &g_gpu.no_cull);
    }
    if (FAILED(hr)) {
        LOG("seam: creating our shaders and states failed 0x%08lX: nothing is drawn", static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

// One full-screen triangle into `rtv` (w x h): `srv` at t0, the constant buffer at b0, `blend` picks the channels.
void fullscreen(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h, ID3D11PixelShader* ps,
                ID3D11ShaderResourceView* srv, ID3D11BlendState* blend) {
    // Targets first: binding them unbinds whatever of them the game had bound for reading, and vice versa.
    c->OMSetRenderTargets(1, &rtv, nullptr);
    c->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
    c->OMSetDepthStencilState(g_gpu.no_depth, 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* no_vb = nullptr;
    UINT zero = 0;
    c->IASetVertexBuffers(0, 1, &no_vb, &zero, &zero);
    c->VSSetShader(g_gpu.vs, nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &g_gpu.cb);
    c->PSSetShaderResources(0, 1, &srv);
    c->RSSetState(g_gpu.no_cull);
    const D3D11_VIEWPORT vp{0.f, 0.f, float(w), float(h), 0.f, 1.f};
    c->RSSetViewports(1, &vp);
    c->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    c->PSSetShaderResources(0, 1, &none);
}

void set_constants(ID3D11DeviceContext* c, float x, float y, float z, float w) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(c->Map(g_gpu.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float* v = static_cast<float*>(m.pData);
        v[0] = x, v[1] = y, v[2] = z, v[3] = w;
        c->Unmap(g_gpu.cb, 0);
    }
}

const char* format_name(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
        case DXGI_FORMAT_R8G8_TYPELESS: return "R8G8_TYPELESS";
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8_TYPELESS";
        case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        default: return "?";
    }
}

void describe_texture(const char* what, uint8_t* texture_data) {
    if (!texture_data) {
        LOG("seam: %s: none", what);
        return;
    }
    auto* res = rfg::field<ID3D11Resource*>(texture_data, rfg::off::TextureData_pTexture);
    auto* srv = rfg::field<ID3D11ShaderResourceView*>(texture_data, rfg::off::TextureData_pShaderView);
    D3D11_TEXTURE2D_DESC d{};
    ID3D11Texture2D* t = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&t)))) {
        t->GetDesc(&d);
        t->Release();
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    if (srv) srv->GetDesc(&sd);
    LOG("seam: %s: %ux%u, format %d (%s), view format %d (%s), bind flags %X, samples %u, misc %X", what, d.Width, d.Height,
        int(d.Format), format_name(d.Format), int(sd.Format), format_name(sd.Format), d.BindFlags, d.SampleDesc.Count,
        d.MiscFlags);
}

void log_stats(bool force) {
    const int64_t now = session::qpc();
    if (!g_stats.since) g_stats.since = now;
    if (!force && session::ticks_to_ms(now - g_stats.since) < 10000) return;
    rtlink::Link& link = rtlink::core_link();
    rfgrt_ipc::Block* b = link.started() ? link.block() : nullptr;
    if (g_worker.waits) {
        LOG("seam: the render thread waited %.1f us per frame for the frame scene's worker",
            session::ticks_to_ms(g_worker.wait_ticks) * 1000.0 / double(g_worker.waits));
        g_worker.wait_ticks = 0;
        g_worker.waits = 0;
    }
    if (g_stats.renders && g_stats.intervals) {
        LOG("seam: rl_scene_renderer::render took %.3f ms on the render thread on average (%.3f ms at most); frames %.3f ms "
            "apart (%.1f FPS); cascades %s",
            session::ticks_to_ms(g_stats.render_ticks) / double(g_stats.renders), session::ticks_to_ms(g_stats.render_max),
            session::ticks_to_ms(g_stats.interval_ticks) / double(g_stats.intervals),
            1000.0 * double(g_stats.intervals) / session::ticks_to_ms(g_stats.interval_ticks),
            g_cfg.skip_cascades ? "without casters while ClbTexture's shadow channel is ours" : "rendered");
    }
    if (g_cfg.skip_cascades) {
        LOG("seam: cascades without casters in %llu frames (their cloud shadows kept, ours multiplied in)",
            static_cast<unsigned long long>(g_frames_dropped));
        g_frames_dropped = 0;
    }
    if (g_ao) LOG("seam: the game's SSAO skipped in %llu frames (ClbTexture's R is the ray-traced occlusion)",
                  static_cast<unsigned long long>(g_ssao_skipped.exchange(0)));
    LOG("seam: %llu frames: pattern %llu, core results applied %llu, missed %llu (core not ahead, or on other frame "
        "textures; %llu of them given its previous result); core pass %lld us, queued up to frame %lld of %lld; other "
        "scene renderers %llu",
        static_cast<unsigned long long>(g_stats.seams), static_cast<unsigned long long>(g_stats.patterns),
        static_cast<unsigned long long>(g_stats.applied), static_cast<unsigned long long>(g_stats.missed),
        static_cast<unsigned long long>(g_stats.stale),
        static_cast<long long>(b ? rfgrt_ipc::load64(&b->host_gpu_us) : -1),
        static_cast<long long>(b ? rfgrt_ipc::load64(&b->host_submitted) : -1), static_cast<long long>(g_frame),
        static_cast<unsigned long long>(g_stats.others));
    if (g_stats.waits)
        LOG("seam: %llu frames waited for the core to queue them, %.3f ms on average; %llu of them gave up after %g ms",
            static_cast<unsigned long long>(g_stats.waits), session::ticks_to_ms(g_stats.wait_ticks) / double(g_stats.waits),
            static_cast<unsigned long long>(g_stats.wait_timeouts), g_cfg.host_wait_ms);
    const int64_t last = g_stats.last_render_start;
    g_stats = Stats{};
    g_stats.since = now;
    g_stats.last_render_start = last;
}

// The exchange with the core for this frame. Returns true when ClbTexture got the core's result.
// Once the core is ready, every frame signals fence A, also those that cannot use its result: the core queues its
// frames ahead and waits for them, so a frame left out would stall it.
bool host_frame(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint8_t* scene, ID3D11RenderTargetView* clb_rtv,
                uint32_t w, uint32_t h, bool apply, bool multiply) {
    if (g_host_off) return false;
    rtlink::Link& link = rtlink::core_link();
    if (link.dead()) {
        LOG("seam: the core is gone: the game's own shadows from now on");
        g_host_off = true;
        return false;
    }
    if (!link.start(dev)) {
        LOG("seam: the core did not start: the game's own shadows from now on");
        g_host_off = true;
        return false;
    }
    if (!link.ready()) return false;  // starting
    ID3D11DeviceContext4* ctx4 = nullptr;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx4)))) return false;
    const int64_t n = g_snapshots ? t_frame : ++g_frame;
    if (!n) {
        ctx4->Release();
        return false;  // the core became ready during this frame: from the next one on
    }
    bool have = link.ensure_frame_resources(w, h, n);
    if (have) {
        auto* depth = rfg::field<uint8_t*>(scene, rfg::off::SceneRenderer_mainDepthTexture);
        auto* depth_srv = depth ? rfg::field<ID3D11ShaderResourceView*>(depth, rfg::off::TextureData_pShaderView) : nullptr;
        if (depth_srv)
            fullscreen(ctx, link.depth_rtv(), w, h, g_gpu.copy_depth_ps, depth_srv, g_gpu.write_all);
        else
            have = false;
        // The game's own ClbTexture as its cascades left it, for the core to compare against.
        auto* clb = rfg::field<uint8_t*>(scene, rfg::off::SceneRenderer_clbTexture);
        auto* clb_res = clb ? rfg::field<ID3D11Resource*>(clb, rfg::off::TextureData_pTexture) : nullptr;
        if (clb_res && link.game_clb()) ctx->CopyResource(link.game_clb(), clb_res);
    }
    // the materials' textures (mirror/textures.h): the copies the core can take now sent, a few more recorded before this
    // frame's fence A
    textures::on_seam(dev, ctx, n, rfgrt_ipc::load64(&link.block()->host_submitted));
    ctx4->Signal(link.fence_a(), UINT64(n));
    ctx->Flush();  // the signal reaches the GPU now, so the core's queued work can start
    rfgrt_ipc::Block* b = link.block();
    rfgrt_ipc::store64(&b->plugin_frame, n);
    bool applied = false;
    // (Test.MissEvery: every Nth frame's result left unused, as if the core had not got to it)
    const bool test_miss = g_cfg.test_miss_every > 0 && n % g_cfg.test_miss_every == 0;
    // In a big scene the core queues a frame a moment after its seam (the frame's instances take it a few ms): wait for
    // it, up to Host.WaitMs, rather than show the game's own shadows and lighting for the frame. The game then runs at
    // the core's pace, as it would on a slower graphics card. Where the GPU is the limit, the core queues frame n once
    // its frame n-3 is done (kRing in rtcore.cpp): each frame waits nearly a frame's GPU time, so the cap must stay well
    // above the slowest frames. At 10 ms (0.1.17, GPU passes of 10-15 ms with debris) up to half the frames gave up: each
    // showed the previous frame's lighting under its own picture, the copies drifting apart as the view moved (the user's
    // "layers"), and the glass's light read while the core was writing it.
    if (!test_miss && have && g_cfg.host_wait_ms > 0 && rfgrt_ipc::load64(&b->host_submitted) < n &&
        b->host_generation == link.generation()) {
        const int64_t t0 = session::qpc();
        int spins = 0;
        bool timed_out = false;
        while (rfgrt_ipc::load64(&b->host_submitted) < n && !link.dead()) {
            if ((++spins & 63) == 0 && session::ticks_to_ms(session::qpc() - t0) >= g_cfg.host_wait_ms) {
                timed_out = true;
                break;
            }
            if (spins < 256)
                YieldProcessor();
            else
                SwitchToThread();
        }
        ++g_stats.waits;
        g_stats.wait_ticks += session::qpc() - t0;
        if (timed_out) ++g_stats.wait_timeouts;
    }
    // Applied only if the core queued this frame with the textures it was copied into.
    if (!test_miss && have && rfgrt_ipc::load64(&b->host_submitted) >= n && b->host_generation == link.generation() &&
        n >= rfgrt_ipc::load64(&b->host_generation_first_frame)) {
        ctx4->Wait(link.fence_b(), UINT64(n));
        if (apply) {
            if (g_ao) fullscreen(ctx, clb_rtv, w, h, g_gpu.apply_ps, link.result_srv(), g_gpu.write_r);  // the occlusion
            fullscreen(ctx, clb_rtv, w, h, g_gpu.apply_ps, link.result_srv(), multiply ? g_gpu.mul_g : g_gpu.write_g);
            keep_result(dev, ctx, link.result_srv());
            applied = true;
        }
        ++g_stats.applied;
    } else {
        ++g_stats.missed;
    }
    ctx4->Release();
    return applied;
}

void on_seam(uint8_t* graphics, uint8_t* scene) {
    if (g_gpu.failed) return;
    if (!scene || !graphics) return skipped("no scene renderer or graphics system");
    if (!g_main_scene) g_main_scene = scene;
    if (scene != g_main_scene) {
        ++g_stats.others;
        return note_other_scene(scene);
    }
    auto* dev = rfg::field<ID3D11Device*>(graphics, rfg::off::GraphicsSystem_pDevice);
    auto* ctx = rfg::field<ID3D11DeviceContext*>(graphics, rfg::off::GraphicsSystem_pImmediateContext);
    auto* clb_target = rfg::field<uint8_t*>(scene, rfg::off::SceneRenderer_clbTarget);
    if (!dev || !ctx) return skipped("no D3D11 device or immediate context");
    if (!clb_target) return skipped("the scene renderer has no ClbTexture target");
    auto* clb_rtv = rfg::field<ID3D11RenderTargetView*>(clb_target, rfg::off::RenderTarget_renderTargetViews);
    const uint32_t w = rfg::field<uint32_t>(clb_target, rfg::off::RenderTarget_width);
    const uint32_t h = rfg::field<uint32_t>(clb_target, rfg::off::RenderTarget_height);
    if (!clb_rtv || !w || !h) return skipped("ClbTexture's target has no view or no size");
    if (!g_gpu.ready) {
        LOG("seam: first frame: scene renderer %p, ClbTexture target %ux%u (main scene %dx%d)", static_cast<void*>(scene), w, h,
            rfg::field<int32_t>(scene, rfg::off::SceneRenderer_mainWidth), rfg::field<int32_t>(scene, rfg::off::SceneRenderer_mainHeight));
        describe_texture("ClbTexture", rfg::field<uint8_t*>(scene, rfg::off::SceneRenderer_clbTexture));
        describe_texture("main scene depth texture", rfg::field<uint8_t*>(scene, rfg::off::SceneRenderer_mainDepthTexture));
        if (!make_gpu(dev)) {
            g_gpu.failed = true;
            return;
        }
        g_gpu.ready = true;
    }
    ++g_stats.seams;

    poll_vanilla_key();
    const double t = session::gameplay_seconds();
    const bool active = t >= 0 && t >= g_cfg.pattern_from_s && !g_vanilla;  // (vanilla: the core kept in step, unused)
    // This frame's cascades had no casters: the shadow channel holds the game's cloud shadows alone.
    const bool clouds_only = g_casters_dropped.exchange(false);
    D3D11StateBlock saved;
    saved.capture(ctx);
    bool clb_written = false;
    bool answered = false;  // the core's result for this frame applied (it waited for it)
    if (g_cfg.clb_pattern && active) {
        set_constants(ctx, 64.f, 0.f, 0.f, 0.f);
        ID3D11BlendState* mask = g_cfg.clb_pattern == 1 ? g_gpu.write_g : g_cfg.clb_pattern == 2 ? g_gpu.write_r : g_gpu.write_rg;
        fullscreen(ctx, clb_rtv, w, h, g_gpu.pattern_ps, nullptr, mask);
        ++g_stats.patterns;
        clb_written = g_cfg.clb_pattern != 2;
    }
    if (g_cfg.host_enabled) {
        if (g_cfg.kill_host_at_s > 0 && t >= g_cfg.kill_host_at_s && !g_killed_for_test) {
            g_killed_for_test = true;
            rtlink::core_link().kill_for_test();
        }
        answered = host_frame(dev, ctx, scene, clb_rtv, w, h, active, clouds_only);
        clb_written |= answered;
        // A frame the core did not answer (busy building a newly streamed area, or gone) whose cascades went without
        // casters, counting on it: the core's last result, a frame or a few old, rather than no sun shadow and no
        // occlusion at all (how many misses in a row it covers: below).
        if (!answered && clouds_only && active && g_last.srv) {
            if (g_ao) fullscreen(ctx, clb_rtv, w, h, g_gpu.apply_ps, g_last.srv, g_gpu.write_r);
            fullscreen(ctx, clb_rtv, w, h, g_gpu.apply_ps, g_last.srv, g_gpu.mul_g);
            ++g_stats.stale;
        }
    }
    // The irradiance the patched lit shaders read (scene thread, after this): the core's while it runs.
    {
        rtlink::Link& link = rtlink::core_link();
        const bool core_ok = g_cfg.host_enabled && !g_host_off && !g_vanilla && link.ready() && link.block() &&
                             link.block()->host_generation == link.generation();
        gi::on_seam(dev, ctx, w, h, core_ok ? link.irradiance_srv() : nullptr, core_ok ? link.reflection_srv() : nullptr,
                    core_ok ? link.glass_srv() : nullptr, core_ok ? link.glass_refl_srv() : nullptr,
                    core_ok && link.block()->host_fog ? link.fog_srv() : nullptr, core_ok ? link.sun_tint_srv() : nullptr);
        lights::on_seam(answered, core_ok);  // the dynamic lights' masks, bound in this frame's lighting passes
    }
    saved.restore(ctx);
    // Next frame's cascades go without casters while something else fills the shadow channel: this frame's result, or,
    // for up to kCoverMisses frames the core misses in a row, its last one (each such frame gets it, above). So a miss
    // does not switch a frame to the game's shadows and back (whose cloud shadows our result would then overwrite).
    if (answered)
        g_since_answer = 0;
    else if (g_since_answer < (1 << 30))
        ++g_since_answer;
    const bool covered = !g_vanilla && (clb_written || (!g_host_off && g_last.srv && g_since_answer <= kCoverMisses));
    g_skip_cascades = g_cfg.skip_cascades && covered;
    g_skip_ssao = g_ao && covered;
    log_stats(false);
}

void __cdecl d_end_command_buffer(uint8_t* graphics, void* command_buffer) {
    o_end_command_buffer(graphics, command_buffer);
    // Only the scene thread's list that rl_scene_renderer::render finishes and executes next.
    if (t_scene_renderer) on_seam(graphics, t_scene_renderer);
}

// Render.DebugView: the core's last result over the frame about to be presented (endFrame, render thread). At that
// point the result holds this frame's answer: the core's next pass waits for the next frame's fence A.
void draw_overlay(uint8_t* graphics, IDXGISwapChain* sc) {
    if (!g_gpu.ready || g_host_off) return;
    rtlink::Link& link = rtlink::core_link();
    if (!link.ready() || !link.result_srv() || !link.block()) return;
    auto* ctx = rfg::field<ID3D11DeviceContext*>(graphics, rfg::off::GraphicsSystem_pImmediateContext);
    ID3D11Texture2D* back = nullptr;
    if (!ctx || FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return;
    static ID3D11Texture2D* s_back = nullptr;  // the back buffer the view below was made for
    static ID3D11RenderTargetView* s_rtv = nullptr;
    if (back != s_back) {
        if (s_rtv) s_rtv->Release();
        s_rtv = nullptr;
        if (FAILED(g_gpu.device->CreateRenderTargetView(back, nullptr, &s_rtv))) s_rtv = nullptr;
        s_back = back;
    }
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    back->Release();
    if (!s_rtv || !d.Width || !d.Height) return;
    const rfgrt_ipc::Block* b = link.block();
    D3D11StateBlock saved;
    saved.capture(ctx);
    set_constants(ctx, float(b->width) / float(d.Width), float(b->height) / float(d.Height), 0.f, 0.f);
    if (g_cfg.debug_view == 3) {  // the game's lit colour the rays' hits read
        if (link.radiance_srv()) fullscreen(ctx, s_rtv, d.Width, d.Height, g_gpu.radiance_ps, link.radiance_srv(), g_gpu.blend_alpha);
    } else if (g_cfg.debug_view == 4) {  // the path-traced irradiance the patched shaders read (the light alone, denoised)
        if (link.irradiance_srv()) fullscreen(ctx, s_rtv, d.Width, d.Height, g_gpu.radiance_ps, link.irradiance_srv(), g_gpu.blend_alpha);
    } else {
        fullscreen(ctx, s_rtv, d.Width, d.Height, g_gpu.debug_ps, link.result_srv(), g_gpu.blend_alpha);
    }
    saved.restore(ctx);
}

// The frame's scene is written on a worker thread while render() runs (g_worker, declared at the top).
DWORD WINAPI snapshot_worker_main(LPVOID) {
    for (;;) {
        if (WaitForSingleObject(g_worker.start, INFINITE) != WAIT_OBJECT_0) return 0;
        instances::snapshot(g_worker.renderer, g_worker.scene, g_worker.camera, g_worker.frame);
        SetEvent(g_worker.done);
    }
}

void __fastcall d_scene_render(RFG_THIS(void*, self), void* renderer, void* sim_renderer, void* scene, void* camera,
                               void* hdr, void* color_correct) {
    t_scene_renderer = static_cast<uint8_t*>(self);
    t_frame = 0;
    if (!g_main_scene || g_main_scene == self) gi::on_render(static_cast<uint8_t*>(renderer), static_cast<uint8_t*>(scene));
    bool on_worker = false;
    // Only for the main scene renderer, once the core is ready.
    if (g_snapshots && !g_host_off && (!g_main_scene || g_main_scene == self) && rtlink::core_link().ready()) {
        t_frame = ++g_frame;
        g_frame_rendering.store(t_frame, std::memory_order_release);
        lights::collect(static_cast<uint8_t*>(self), static_cast<uint8_t*>(scene), static_cast<uint8_t*>(camera));
        if (g_worker.thread) {
            g_worker.renderer = static_cast<uint8_t*>(renderer);
            g_worker.scene = static_cast<uint8_t*>(scene);
            g_worker.camera = static_cast<uint8_t*>(camera);
            g_worker.frame = t_frame;
            SetEvent(g_worker.start);
            on_worker = true;
        } else {
            instances::snapshot(static_cast<uint8_t*>(renderer), static_cast<uint8_t*>(scene), static_cast<uint8_t*>(camera), t_frame);
        }
    }
    const bool main_scene = !g_main_scene || g_main_scene == self;
    const int64_t r0 = session::qpc();
    o_scene_render(self, renderer, sim_renderer, scene, camera, hdr, color_correct);
    if (main_scene) {
        const int64_t r1 = session::qpc();
        g_stats.render_ticks += r1 - r0;
        g_stats.render_max = r1 - r0 > g_stats.render_max ? r1 - r0 : g_stats.render_max;
        ++g_stats.renders;
        if (g_stats.last_render_start) {
            g_stats.interval_ticks += r0 - g_stats.last_render_start;
            ++g_stats.intervals;
        }
        g_stats.last_render_start = r0;
    }
    if (on_worker) {
        // Normally done long ago; the game's render data must not change while the worker reads it.
        const int64_t t0 = session::qpc();
        WaitForSingleObject(g_worker.done, INFINITE);
        g_worker.wait_ticks += session::qpc() - t0;
        ++g_worker.waits;
    }
    t_scene_renderer = nullptr;
    t_frame = 0;
}

// rl_scene_renderer::clb_render clears ClbTexture, then for each cascade renders its shadow map
// (render_shadow_pass) and projects it together with the cloud texture (rl_clb_projector_system_render_
// DepthAndProjectionTexture), and projects the clouds alone beyond the cascades. While the shadow channel is ours, the
// cascades get no casters: render_shadow_pass still clears its map (depth 1: lit everywhere) but draws nothing, as
// its pass mask then meets no instance's and no batch's; the cloud shadows stay.
void __fastcall d_clb_render(RFG_THIS(void*, self), void* renderer, void* camera, void* light, void* vis, void* batched,
                             void** instances, int count) {
    const bool drop = g_skip_cascades.load(std::memory_order_relaxed);
    t_drop_casters = drop;
    o_clb_render(self, renderer, camera, light, vis, batched, instances, count);
    t_drop_casters = false;
    if (drop) {
        g_casters_dropped = true;
        ++g_frames_dropped;
    }
}

// renderSsaoEffect (cdecl, 8 arguments, rl_scene_renderer::render_scene_part2 calls it): left out while ClbTexture's R
// holds the core's occlusion, which it would overwrite.
void(__cdecl* o_ssao)(void*, void*, void*, void*, void*, void*, void*, void*);
void __cdecl d_ssao(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    if (g_skip_ssao.load(std::memory_order_relaxed)) {
        ++g_ssao_skipped;
        return;
    }
    o_ssao(a0, a1, a2, a3, a4, a5, a6, a7);
}

// __thiscall render_shadow_pass(renderer, camera, light, begin-pass info, width, height, view-projection, batches,
// pass mask, instances, count, flag): instances and batches are drawn where their pass bits meet the mask.
void __fastcall d_shadow_pass(RFG_THIS(void*, self), void* renderer, void* camera, void* light, void* pass, uint32_t w,
                              uint32_t h, void* view, void* batches, uint32_t mask, void** instances, uint32_t count,
                              uint32_t flag) {
    o_shadow_pass(self, renderer, camera, light, pass, w, h, view, batches, t_drop_casters ? 0u : mask, instances, count, flag);
}

}  // namespace

bool install(const Config& cfg) {
    g_cfg = cfg;
    g_snapshots = cfg.host_enabled && cfg.mirror_enabled;
    if (!cfg.clb_pattern && !cfg.host_enabled && !cfg.gi_enabled) {
        LOG("seam: nothing to do (Render.ClbPattern=0, Host.Enabled=0): the game renders as usual");
        return true;
    }
    bool ok = hooks::create(rfg::rva::keen__graphics__endCommandBuffer, &d_end_command_buffer, &o_end_command_buffer,
                            "keen::graphics::endCommandBuffer");
    ok &= hooks::create_this(rfg::rva::rl_scene_renderer__render, &d_scene_render, &o_scene_render,
                             "rl_scene_renderer::render");
    if (g_snapshots) {
        g_worker.start = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_worker.done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_worker.start && g_worker.done)
            g_worker.thread = CreateThread(nullptr, 256 * 1024, &snapshot_worker_main, nullptr, 0, nullptr);
        if (!g_worker.thread) LOG("seam: no snapshot worker thread: frame scenes are written on the render thread");
    }
    if (cfg.skip_cascades) {
        ok &= hooks::create_this(rfg::rva::rl_scene_renderer__clb_render, &d_clb_render, &o_clb_render,
                                 "rl_scene_renderer::clb_render");
        ok &= hooks::create_this(rfg::rva::rl_scene_renderer__render_shadow_pass, &d_shadow_pass, &o_shadow_pass,
                                 "rl_scene_renderer::render_shadow_pass");
    }
    // The core's occlusion goes into R (not with the shadow comparison view, which uses R for its display).
    g_ao = cfg.host_enabled && cfg.host_mode == rfgrt_ipc::kModeSunShadows && cfg.ao_enabled && cfg.debug_view != 1;
    if (g_ao) ok &= hooks::create(rfg::rva::renderSsaoEffect, &d_ssao, &o_ssao, "renderSsaoEffect");
    if (cfg.debug_view && cfg.host_enabled) {
        screenshot::set_overlay(&draw_overlay);
        ok &= screenshot::install();
    }
    const bool ao_in_gi = cfg.gi_enabled && cfg.gi_debug == 0 && !cfg.ao_with_gi;
    LOG("seam: installed (pattern %d, core %s, cascades %s, ambient occlusion %s)", cfg.clb_pattern, cfg.host_enabled ? "on" : "off",
        cfg.skip_cascades ? "without casters while ClbTexture's shadow channel is ours (cloud shadows kept)" : "kept",
        !g_ao      ? "the game's"
        : ao_in_gi ? "in the path-traced irradiance (R is 1; the game's SSAO skipped)"
                   : "ray traced (the game's SSAO skipped while ours is there)");
    return ok;
}

int64_t frame_rendering() { return g_frame_rendering.load(std::memory_order_acquire); }

uint8_t* main_scene_renderer() { return g_main_scene; }

void composite_fog(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, uint32_t w, uint32_t h, ID3D11ShaderResourceView* fog) {
    if (!g_gpu.ready || !g_gpu.fog_ps || !g_gpu.fog_blend || !ctx || !target || !fog || !w || !h) return;
    D3D11StateBlock saved;
    saved.capture(ctx);
    fullscreen(ctx, target, w, h, g_gpu.fog_ps, fog, g_gpu.fog_blend);
    saved.restore(ctx);
}

void shutdown(bool at_exit) {
    log_stats(true);
    // The core's log ends cleanly; nothing is released (the render thread may still be drawing).
    if (!at_exit) rtlink::core_link().request_stop(2000);
}

}  // namespace seam
