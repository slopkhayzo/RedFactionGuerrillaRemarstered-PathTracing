#include "skinning.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "skin_cs.h"  // g_skin_cs (dxc, cs_6_0)

namespace rtcore {

namespace {

uint64_t round256(uint64_t v) { return (v + 255) & ~uint64_t(255); }

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

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    return br;
}

constexpr D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS kPosedFlags =
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
constexpr int64_t kUnusedFrames = 500;  // released after this many frames without the instance in a frame scene

}  // namespace

void Skinning::logf(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (log_) log_(line);
}

bool Skinning::init(ID3D12Device5* dev, int ring, uint32_t max_skins, uint32_t max_bones, uint32_t max_instances, LogFn log) {
    dev_ = dev;
    log_ = log;
    ring_ = ring;
    max_skins_ = max_skins;
    max_bones_ = max_bones;
    if (!dev_ || !max_skins_ || !max_bones_) {
        logf("skinning: off (no palettes in the frame scenes): skinned instances keep their bind pose");
        return true;
    }
    // The skinning pass: bind-pose positions, skin and palette (root SRVs t0-t2), posed positions (root UAV u0),
    // vertex and bone counts (b0).
    D3D12_ROOT_PARAMETER params[5]{};
    for (UINT i = 0; i < 3; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[i].Descriptor = {i, 0};
    }
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[3].Descriptor = {0, 0};
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[4].Constants = {0, 0, 4};
    const D3D12_ROOT_SIGNATURE_DESC rsd{5, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *blob = nullptr, *err = nullptr;
    if (SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
        dev_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs_));
    release_ptr(blob);
    release_ptr(err);
    if (!rs_) {
        logf("skinning: root signature failed: skinned instances keep their bind pose");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs_;
    pd.CS = {g_skin_cs, sizeof(g_skin_cs)};
    if (FAILED(dev_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_)))) {
        logf("skinning: pipeline failed: skinned instances keep their bind pose");
        return false;
    }
    bones_ = make_buffer(dev_, D3D12_HEAP_TYPE_UPLOAD, uint64_t(ring_) * max_bones_ * sizeof(rfgrt_frame::Bone), D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ, L"rtcore bone palettes");
    D3D12_RANGE none{0, 0};
    if (!bones_ || FAILED(bones_->Map(0, &none, reinterpret_cast<void**>(&bones_cpu_)))) {
        logf("skinning: palette buffer failed: skinned instances keep their bind pose");
        return false;
    }
    override_.assign(max_instances, 0);
    ready_ = true;
    logf("skinning: ready (%u palettes and %u bones a frame scene)", max_skins_, max_bones_);
    return true;
}

void Skinning::geometry(const Posed& p, const Scene::SkinView& v, std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& out) const {
    out.clear();
    for (uint32_t r = 0; r < v.range_count; ++r) {
        const rfgrt_scene::Range& range = v.ranges[r];
        if (!range.index_count || range.index_count % 3 || uint64_t(range.first_index) + range.index_count > v.index_count) continue;
        D3D12_RAYTRACING_GEOMETRY_DESC g{};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.Triangles.IndexFormat = v.index_bytes == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.IndexCount = range.index_count;
        g.Triangles.VertexCount = v.vertex_count;
        g.Triangles.IndexBuffer = v.indices + uint64_t(range.first_index) * v.index_bytes;
        g.Triangles.VertexBuffer.StartAddress = p.vertices->GetGPUVirtualAddress();
        g.Triangles.VertexBuffer.StrideInBytes = 12;
        out.push_back(g);
    }
}

bool Skinning::create(Posed& p, const Scene::SkinView& v, uint32_t mesh_id, uint32_t submesh) {
    p = Posed{};
    p.mesh_id = mesh_id;
    p.submesh = submesh;
    p.vertex_count = v.vertex_count;
    p.vertices = make_buffer(dev_, D3D12_HEAP_TYPE_DEFAULT, round256(uint64_t(v.vertex_count) * 12), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COMMON, L"rtcore posed vertices");
    if (!p.vertices) return false;
    geometry(p, v, descs_);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    if (!descs_.empty()) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = kPosedFlags;
        in.NumDescs = UINT(descs_.size());
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = descs_.data();
        dev_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    }
    if (info.ResultDataMaxSizeInBytes) {
        p.blas = make_buffer(dev_, D3D12_HEAP_TYPE_DEFAULT, round256(info.ResultDataMaxSizeInBytes), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"rtcore posed BLAS");
        p.scratch = make_buffer(dev_, D3D12_HEAP_TYPE_DEFAULT,
                                round256(std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes)),
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, L"rtcore posed BLAS scratch");
    }
    if (!p.blas || !p.scratch) {
        release_ptr(p.vertices);
        release_ptr(p.blas);
        release_ptr(p.scratch);
        return false;
    }
    return true;
}

void Skinning::retire(Posed& p) {
    ID3D12Resource* rs[3] = {p.vertices, p.blas, p.scratch};
    for (ID3D12Resource* r : rs)
        if (r) retired_.push_back({r, p.last_frame});
    p = Posed{};
}

void Skinning::record(ID3D12GraphicsCommandList4* cl, int slot, const rfgrt_frame::SlotHeader* h, uint32_t max_instances,
                      uint32_t max_skins, const Scene& scene, int64_t frame) {
    for (uint32_t i : overridden_) override_[i] = 0;
    overridden_.clear();
    work_.clear();
    first_builds_.clear();
    if (!ready_ || !h) return;
    const uint32_t skins = std::min(h->skin_count, std::min(max_skins, max_skins_));
    if (!skins) return;
    ++stats_.frames;
    const uint32_t bone_total = std::min(h->bone_count, max_bones_);
    const uint32_t instances = std::min(h->instance_count, max_instances);
    const rfgrt_frame::Skin* sk = rfgrt_frame::slot_skins(h, max_instances);
    const rfgrt_frame::Instance* inst = rfgrt_frame::slot_instances(h);
    // The palettes into this ring slot's part of the upload buffer (its previous frame is done: the caller's ring check).
    const uint64_t slot_base = uint64_t(slot) * max_bones_ * sizeof(rfgrt_frame::Bone);
    memcpy(bones_cpu_ + slot_base, rfgrt_frame::slot_bones(h, max_instances, max_skins), size_t(bone_total) * sizeof(rfgrt_frame::Bone));
    const D3D12_GPU_VIRTUAL_ADDRESS bones_va = bones_->GetGPUVirtualAddress() + slot_base;
    cl->SetPipelineState(pso_);
    cl->SetComputeRootSignature(rs_);
    for (uint32_t i = 0; i < skins; ++i) {
        const rfgrt_frame::Skin s = sk[i];  // a copy: the plugin may be rewriting the slot (such frames are dropped)
        if (s.instance >= instances || !s.bone_count || uint64_t(s.first_bone) + s.bone_count > bone_total) {
            ++stats_.bad_record;
            continue;
        }
        const rfgrt_frame::Instance in = inst[s.instance];
        Scene::SkinView v{};
        if (!scene.skin_view(in.mesh_id, in.submesh, &v)) {
            ++stats_.no_mesh;
            continue;
        }
        Posed& p = posed_[s.key];
        if (p.last_frame == frame && p.vertices) continue;  // named twice in one frame scene
        if (!p.vertices || p.mesh_id != in.mesh_id || p.submesh != in.submesh || p.vertex_count != v.vertex_count) {
            retire(p);
            if (!create(p, v, in.mesh_id, in.submesh)) {
                ++stats_.failed;
                posed_.erase(s.key);
                continue;
            }
        }
        p.last_frame = frame;
        cl->SetComputeRootShaderResourceView(0, v.positions);
        cl->SetComputeRootShaderResourceView(1, v.skin);
        cl->SetComputeRootShaderResourceView(2, bones_va + uint64_t(s.first_bone) * sizeof(rfgrt_frame::Bone));
        cl->SetComputeRootUnorderedAccessView(3, p.vertices->GetGPUVirtualAddress());
        const uint32_t c[4] = {v.vertex_count, s.bone_count, 0, 0};
        cl->SetComputeRoot32BitConstants(4, 4, c, 0);
        cl->Dispatch((v.vertex_count + 63) / 64, 1, 1);
        work_.push_back({&p, v, s.instance, !p.built});
        ++stats_.palettes;
        stats_.bones += s.bone_count;
    }
    if (work_.empty()) return;
    // The posed positions (a buffer: promoted from COMMON to UAV by the dispatch) become the builds' input.
    barriers_.clear();
    for (const Work& w : work_)
        barriers_.push_back(transition(w.posed->vertices, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    cl->ResourceBarrier(UINT(barriers_.size()), barriers_.data());
    for (const Work& w : work_) {
        geometry(*w.posed, w.view, descs_);
        if (descs_.empty()) continue;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
        bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bd.Inputs.Flags = kPosedFlags | (w.first ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE
                                                 : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
        bd.Inputs.NumDescs = UINT(descs_.size());
        bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        bd.Inputs.pGeometryDescs = descs_.data();
        bd.DestAccelerationStructureData = w.posed->blas->GetGPUVirtualAddress();
        bd.SourceAccelerationStructureData = w.first ? 0 : bd.DestAccelerationStructureData;
        bd.ScratchAccelerationStructureData = w.posed->scratch->GetGPUVirtualAddress();
        cl->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        if (w.first) {
            first_builds_.push_back(w.posed);
            ++stats_.built;
        } else {
            ++stats_.refits;
        }
        if (w.instance < override_.size()) {
            override_[w.instance] = bd.DestAccelerationStructureData;
            overridden_.push_back(w.instance);
        }
        ++stats_.posed;
    }
    // the BLAS before the TLAS build reads them
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = nullptr;
    cl->ResourceBarrier(1, &b);
}

void Skinning::commit() {
    for (Posed* p : first_builds_) p->built = true;
    first_builds_.clear();
}

void Skinning::abandon() { first_builds_.clear(); }

void Skinning::collect(int64_t frame_now, uint64_t frames_done) {
    while (!retired_.empty() && retired_.front().frame >= 0 && uint64_t(retired_.front().frame) <= frames_done) {
        retired_.front().r->Release();
        retired_.pop_front();
    }
    for (auto it = posed_.begin(); it != posed_.end();) {
        if (frame_now - it->second.last_frame > kUnusedFrames) {
            retire(it->second);
            ++stats_.released;
            it = posed_.erase(it);
        } else {
            ++it;
        }
    }
}

void Skinning::release() {
    for (auto& kv : posed_) retire(kv.second);
    posed_.clear();
    for (Retired& r : retired_) r.r->Release();
    retired_.clear();
    if (bones_ && bones_cpu_) bones_->Unmap(0, nullptr);
    bones_cpu_ = nullptr;
    release_ptr(bones_);
    release_ptr(pso_);
    release_ptr(rs_);
    ready_ = false;
}

void Skinning::log_stats() {
    if (!ready_) return;
    const double f = stats_.frames ? double(stats_.frames) : 1.0;
    logf("skinning: %.1f skinned instances posed a frame (%.0f bones), %llu BLAS built, %llu refits; left in their bind pose: %llu "
         "without their mesh's bind pose on the GPU, %llu malformed records, %llu failed allocations; %zu posed instances kept, "
         "%llu released",
         double(stats_.posed) / f, double(stats_.bones) / f, static_cast<unsigned long long>(stats_.built),
         static_cast<unsigned long long>(stats_.refits), static_cast<unsigned long long>(stats_.no_mesh),
         static_cast<unsigned long long>(stats_.bad_record), static_cast<unsigned long long>(stats_.failed), posed_.size(),
         static_cast<unsigned long long>(stats_.released));
    stats_ = Stats{};
}

}  // namespace rtcore
