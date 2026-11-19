#pragma once
// Skinned instances posed for ray tracing (research/docs/03, phase 3: casters that move). Each frame scene lists the
// visible rl_skin_instances with their bone palettes (frame_scene.h, Skin and Bone); the core skins the mesh's bind pose
// with the palette (shaders/skin.hlsl) into a vertex buffer of the instance's own, and builds (the first time) or
// refits (after) a BLAS from it, which the frame's TLAS takes instead of the mesh's bind-pose BLAS. Posed instances are
// kept by the game's instance (Skin::key) and released when no frame has named them for a while.
#include <d3d12.h>

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "frame_scene.h"
#include "scene.h"

namespace rtcore {

class Skinning {
public:
    bool init(ID3D12Device5* dev, int ring, uint32_t max_skins, uint32_t max_bones, uint32_t max_instances, LogFn log);
    bool ready() const { return ready_; }
    // Records the posing of ring slot `slot`'s frame scene into `cl`, before the TLAS build: the palettes into the
    // slot's upload buffer, a skinning dispatch and a BLAS build or refit per skinned instance, a UAV barrier.
    void record(ID3D12GraphicsCommandList4* cl, int slot, const rfgrt_frame::SlotHeader* h, uint32_t max_instances,
                uint32_t max_skins, const Scene& scene, int64_t frame);
    // After record(): the list was executed (its first builds count as built from now on), or dropped.
    void commit();
    void abandon();
    // The posed BLAS of the frame's instance `index` (0: none, the mesh's own), until the next record().
    const D3D12_GPU_VIRTUAL_ADDRESS* overrides() const { return override_.empty() ? nullptr : override_.data(); }
    // Releases what no frame has named for a while, once the frames that used it are done.
    void collect(int64_t frame_now, uint64_t frames_done);
    void release();
    void log_stats();

private:
    struct Posed {
        uint32_t mesh_id = 0, submesh = 0, vertex_count = 0;
        ID3D12Resource* vertices = nullptr;  // float3 a vertex: the posed positions
        ID3D12Resource* blas = nullptr;
        ID3D12Resource* scratch = nullptr;   // the larger of the build's and the refit's
        bool built = false;
        int64_t last_frame = 0;
    };
    struct Work {
        Posed* posed;
        Scene::SkinView view;
        uint32_t instance;
        bool first;
    };
    struct Retired {
        ID3D12Resource* r;
        int64_t frame;
    };
    bool create(Posed& p, const Scene::SkinView& v, uint32_t mesh_id, uint32_t submesh);
    void retire(Posed& p);
    void geometry(const Posed& p, const Scene::SkinView& v, std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& out) const;
    void logf(const char* fmt, ...);

    ID3D12Device5* dev_ = nullptr;
    LogFn log_ = nullptr;
    bool ready_ = false;
    uint32_t max_skins_ = 0, max_bones_ = 0;
    int ring_ = 0;
    ID3D12RootSignature* rs_ = nullptr;
    ID3D12PipelineState* pso_ = nullptr;
    ID3D12Resource* bones_ = nullptr;  // upload: ring_ x max_bones_ matrices
    uint8_t* bones_cpu_ = nullptr;
    std::unordered_map<uint32_t, Posed> posed_;
    std::vector<Work> work_;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> override_;
    std::vector<uint32_t> overridden_;
    std::vector<Posed*> first_builds_;
    std::deque<Retired> retired_;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> descs_;
    std::vector<D3D12_RESOURCE_BARRIER> barriers_;

    struct Stats {
        uint64_t frames = 0, posed = 0, built = 0, refits = 0, no_mesh = 0, bad_record = 0, failed = 0, released = 0;
        uint64_t palettes = 0, bones = 0;
    } stats_;
};

}  // namespace rtcore
