#include "tlas.h"

#include <cmath>
#include <cstring>

namespace rtcore {

namespace {

constexpr uint32_t kPassAdditive = 512;          // rl_renderable_instance::PASS_FLAGS_ADDITIVE
constexpr uint32_t kPassDistortionOnly = 1024;   // PASS_FLAGS_DISTORTION_ONLY

ID3D12Resource* make_buffer(ID3D12Device* dev, D3D12_HEAP_TYPE heap, uint64_t size, D3D12_RESOURCE_FLAGS flags,
                            D3D12_RESOURCE_STATES state, const wchar_t* name) {
    D3D12_HEAP_PROPERTIES hp{heap};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    r->SetName(name);
    return r;
}

template <typename T>
void release_ptr(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

}  // namespace

bool Tlas::init(ID3D12Device5* dev, uint32_t max_instances, int ring, LogFn log, bool fast_trace) {
    dev_ = dev;
    max_ = max_instances;
    ring_ = ring;
    log_ = log;
    // Rebuilt every frame (its instances move), but the rays (the GI's, the reflections', the glass's) take far more of
    // the frame than its build: thousands of Geo-Mod pieces and debris overlap, which a better tree sorts out.
    flags_ = fast_trace ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
                        : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.Flags = flags_;
    in.NumDescs = max_;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    if (ring_ > kMaxRing) return false;
    descs_ = make_buffer(dev_, D3D12_HEAP_TYPE_UPLOAD, uint64_t(ring_) * max_ * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                         D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, L"rtcore TLAS instance descs");
    for (int i = 0; i < ring_; ++i) {
        result_[i] = make_buffer(dev_, D3D12_HEAP_TYPE_DEFAULT, info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"rtcore TLAS");
        scratch_[i] = make_buffer(dev_, D3D12_HEAP_TYPE_DEFAULT, info.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"rtcore TLAS scratch");
        if (!result_[i] || !scratch_[i]) return false;
    }
    D3D12_RANGE none{0, 0};
    if (!descs_ || FAILED(descs_->Map(0, &none, reinterpret_cast<void**>(&descs_cpu_)))) return false;
    geo_ = make_buffer(dev_, D3D12_HEAP_TYPE_UPLOAD, uint64_t(ring_) * max_ * 16, D3D12_RESOURCE_FLAG_NONE,
                       D3D12_RESOURCE_STATE_GENERIC_READ, L"rtcore instance table");
    if (!geo_ || FAILED(geo_->Map(0, &none, reinterpret_cast<void**>(&geo_cpu_)))) return false;
    return true;
}

uint32_t Tlas::record(ID3D12GraphicsCommandList4* cl, int slot, const rfgrt_frame::Instance* in, uint32_t count,
                      const Scene& scene, const D3D12_GPU_VIRTUAL_ADDRESS* posed) {
    auto* descs = reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(descs_cpu_ + uint64_t(slot) * max_ * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    auto* table = reinterpret_cast<uint32_t*>(geo_cpu_ + uint64_t(slot) * max_ * 16);
    uint32_t n = 0;
    float nearest2 = 1e30f;
    // by index, not by count accepted: an InstanceID indexes the instance table, which has max_ entries a slot
    for (uint32_t i = 0; i < count && i < max_; ++i) {
        const rfgrt_frame::Instance& s = in[i];
        D3D12_GPU_VIRTUAL_ADDRESS blas = 0;
        const float* offset = nullptr;
        switch (scene.lookup(s.mesh_id, s.submesh, &blas, &offset)) {
            case Scene::Lookup::NoMesh: ++stats.missing_mesh; continue;
            case Scene::Lookup::NoBlas:
                if (!stats.missing_blas++) {
                    stats.sample_blas[0] = s.mesh_id;
                    stats.sample_blas[1] = s.submesh;
                    stats.sample_blas[2] = s.kind;
                }
                continue;
            case Scene::Lookup::SubmeshBeyond:
                if (!stats.submesh_beyond++) {
                    stats.sample_beyond[0] = s.mesh_id;
                    stats.sample_beyond[1] = s.submesh;
                    stats.sample_beyond[2] = scene.submesh_count(s.mesh_id);
                }
                continue;
            default: break;
        }
        if (posed && posed[i]) {
            blas = posed[i];
            ++stats.posed;
        }
        if (!scene.has_materials(s.mesh_id)) ++stats.no_materials[s.kind < 6 ? s.kind : 0];
        // Glass (frame_scene.h): which material map entries are clear glass and which translucent, as the BLAS's geometries
        uint32_t geometries = 0, clear = 0, translucent = 0;
        if (s.glass) {
            uint32_t clear_entries = 0, translucent_entries = 0;
            if (scene.mesh_glass(s.mesh_id, &clear_entries, &translucent_entries)) {  // the mesh's, all 32 entries
                ++stats.glass_by_mesh;
            } else if (s.glass & 0x40000000u) {  // each kind its own (the first 15 entries)
                clear_entries = s.glass & 0x7FFFu;
                translucent_entries = (s.glass >> 15) & 0x7FFFu;
            } else if (s.glass & 0x80000000u) {  // all of it translucent
                translucent_entries = s.glass & 0x7FFFFFFFu;
            } else {
                clear_entries = s.glass & 0x7FFFFFFFu;
            }
            if (clear_entries) clear = scene.material_geometries(s.mesh_id, s.submesh, clear_entries, &geometries);
            if (translucent_entries)
                translucent = scene.material_geometries(s.mesh_id, s.submesh, translucent_entries, &geometries);
        }
        {  // the instance table, by InstanceID (i); x: the page and the translucent glass geometries (rt_query.hlsli)
            Scene::GeoRef g;
            uint32_t* e = table + uint64_t(i) * 4;
            if (scene.geo_ref(s.mesh_id, s.submesh, &g)) {
                e[0] = (g.page & 0xFFu) | ((translucent & 0xFFFFFFu) << 8);
                e[1] = g.positions;
                e[2] = g.indices | (g.index_bytes == 2 ? 0x80000000u : 0u);
                e[3] = g.table;
            } else {
                e[0] = 0xFFFFFFFFu;
            }
        }
        D3D12_RAYTRACING_INSTANCE_DESC& d = descs[n++];
        memcpy(d.Transform, s.object_to_world.m, sizeof(d.Transform));
        // the submesh's offset comes first: t' = t + R o
        for (int r = 0; r < 3; ++r)
            d.Transform[r][3] += d.Transform[r][0] * offset[0] + d.Transform[r][1] * offset[1] + d.Transform[r][2] * offset[2];
        d.InstanceID = i & 0xFFFFFF;
        // mask 1: drawn in the main pass; 2: drawn only as distortion or additive (no depth, no shadow)
        d.InstanceMask = (s.flags & (kPassDistortionOnly | kPassAdditive)) ? 0x02 : 0x01;
        d.InstanceContributionToHitGroupIndex = 0;
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
        // Glass (any transparent material) lets rays through (shaders/rt_query.hlsli): an instance drawing some is
        // non-opaque, with which of its geometries are glass (the first 23) where its hit group offset would be (inline
        // rays: no hit groups); one drawing only such ranges too, every geometry a clear pane: the rays pass it, and those
        // that gather the panes they cross (the window tint, trace_common.hlsli RT_PROCEED_PANES) meet them in their own
        // traversal (until 0.1.17 such instances left the rays' mask, and two more queries a ray found the panes: the GI
        // a quarter dearer). More than 23 geometries: out of the rays' mask (0x02), not tinting. Translucent glass
        // (rfg-eosglass) keeps its instance non-opaque with bit 23: a share of the light stops on each pane of it (its
        // geometries in the instance table), the clear glass beside it lets all through.
        if (s.glass) {
            const uint32_t glass = clear | translucent;
            if (glass && stats.glass_mesh_count < 6) {
                bool listed = false;
                for (uint32_t m = 0; m < stats.glass_mesh_count; ++m) listed |= stats.glass_meshes[m] == s.mesh_id;
                if (!listed) {
                    if (glass == (geometries >= 32 ? 0xFFFFFFFFu : (1u << geometries) - 1u)) stats.glass_whole_bits |= 1u << stats.glass_mesh_count;
                    stats.glass_meshes[stats.glass_mesh_count++] = s.mesh_id;
                }
            }
            if (translucent & 0x7FFFFFu) {
                d.InstanceContributionToHitGroupIndex = (glass & 0x7FFFFFu) | 0x800000u;
                d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
                ++stats.glass_translucent;
            } else if (glass && geometries <= 32 && glass == (geometries == 32 ? 0xFFFFFFFFu : (1u << geometries) - 1u)) {
                if (geometries <= 23) {
                    d.InstanceContributionToHitGroupIndex = glass & 0x7FFFFFu;
                    d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
                } else {
                    d.InstanceMask = 0x02;
                    ++stats.glass_whole_out;
                }
                ++stats.glass_whole;
            } else if (glass & 0x7FFFFFu) {
                d.InstanceContributionToHitGroupIndex = glass & 0x7FFFFFu;
                d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
                ++stats.glass_partial;
            }
        }
        d.AccelerationStructure = blas;
        const float dx = d.Transform[0][3] - camera_[0], dy = d.Transform[1][3] - camera_[1], dz = d.Transform[2][3] - camera_[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        nearest2 = d2 < nearest2 ? d2 : nearest2;
    }
    if (n) stats.nearest_sum += double(std::sqrt(nearest2));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    bd.Inputs.Flags = flags_;
    bd.Inputs.NumDescs = n;
    bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bd.Inputs.InstanceDescs = descs_->GetGPUVirtualAddress() + uint64_t(slot) * max_ * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    bd.DestAccelerationStructureData = result_[slot]->GetGPUVirtualAddress();
    bd.ScratchAccelerationStructureData = scratch_[slot]->GetGPUVirtualAddress();
    cl->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = result_[slot];
    cl->ResourceBarrier(1, &b);
    ++stats.frames;
    stats.instances += n;
    return n;
}

void Tlas::release() {
    if (descs_ && descs_cpu_) descs_->Unmap(0, nullptr);
    descs_cpu_ = nullptr;
    release_ptr(descs_);
    if (geo_ && geo_cpu_) geo_->Unmap(0, nullptr);
    geo_cpu_ = nullptr;
    release_ptr(geo_);
    for (int i = 0; i < kMaxRing; ++i) {
        release_ptr(result_[i]);
        release_ptr(scratch_[i]);
    }
}

}  // namespace rtcore
