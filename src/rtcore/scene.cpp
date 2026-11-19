#include "scene.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace rtcore {

using namespace rfgrt_scene;

namespace {

// IEEE half from float: rounded to nearest, clamped to the largest finite value, NaN kept.
uint32_t to_half(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t exp = (x >> 23) & 0xFFu;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp == 0xFFu) return sign | 0x7C00u | (mant ? 0x200u : 0u);
    const int32_t e = int32_t(exp) - 127 + 15;
    if (e >= 31) return sign | 0x7BFFu;
    if (e <= 0) {
        if (e < -10) return sign;
        mant |= 0x800000u;
        const uint32_t shift = uint32_t(14 - e);
        uint32_t h = mant >> shift;
        if ((mant >> (shift - 1)) & 1u) ++h;
        return sign | h;
    }
    uint32_t h = sign | (uint32_t(e) << 10) | (mant >> 13);
    if (mant & 0x1000u) ++h;  // (a carry into the exponent is the right rounding)
    return h;
}

float finite_or(float v, float fallback, float lo, float hi) {
    if (!std::isfinite(v)) return fallback;
    return v < lo ? lo : v > hi ? hi : v;
}

// A material entry as the shaders read it (trace_common.hlsli): x the texture + 1 (0: none) and bit 31 (known), y the
// tiling, z the colour's r and g, w its b and the self-illumination, halves.
void pack_material(const MaterialEntry& e, bool texture_ok, uint32_t out[4]) {
    const bool tex = texture_ok && e.texture < kMaxTextures;
    out[0] = 0x80000000u | (tex ? (e.texture + 1u) & 0xFFFFu : 0u);
    float tu = finite_or(e.tiling[0], 1.0f, -4096.0f, 4096.0f), tv = finite_or(e.tiling[1], 1.0f, -4096.0f, 4096.0f);
    if (tu == 0.0f) tu = 1.0f;
    if (tv == 0.0f) tv = 1.0f;
    out[1] = to_half(tu) | (to_half(tv) << 16);
    out[2] = to_half(finite_or(e.color[0], 1.0f, 0.0f, 16.0f)) | (to_half(finite_or(e.color[1], 1.0f, 0.0f, 16.0f)) << 16);
    out[3] = to_half(finite_or(e.color[2], 1.0f, 0.0f, 16.0f)) | (to_half(finite_or(e.self_illumination, 0.0f, 0.0f, 1.0f)) << 16);
}

constexpr uint64_t kUploadBytes = 64ull << 20;   // positions and indices waiting for their builds
constexpr uint64_t kScratchBytes = 32ull << 20;  // build scratch, reused
constexpr uint64_t kAsPageBytes = 64ull << 20;   // acceleration structure pool pages
constexpr uint64_t kGeoPageBytes = 64ull << 20;  // geometry pool pages (a shader resource view each: at most 64)

uint64_t round256(uint64_t v) { return (v + 255) & ~uint64_t(255); }

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

D3D12_RESOURCE_DESC buffer_desc(uint64_t size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    return rd;
}

ID3D12Resource* make_buffer(ID3D12Device* dev, D3D12_HEAP_TYPE heap, uint64_t size, D3D12_RESOURCE_FLAGS flags,
                            D3D12_RESOURCE_STATES state, const wchar_t* name) {
    D3D12_HEAP_PROPERTIES hp{heap};
    D3D12_RESOURCE_DESC rd = buffer_desc(size, flags);
    ID3D12Resource* r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    r->SetName(name);
    return r;
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

D3D12_RESOURCE_BARRIER uav_all() {
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    br.UAV.pResource = nullptr;
    return br;
}

}  // namespace

void Scene::logf(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (log_) log_(line);
}

bool Scene::init(ID3D12Device* dev, rfgrt_ipc::Block* block, const uint8_t* ring, bool raytracing, LogFn log) {
    log_ = log;
    b_ = block;
    reader_.attach(block, ring);
    raytracing_ = raytracing && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev_)));
    if (!raytracing_) {
        logf("scene: no hardware ray tracing on this device: meshes are read and dropped");
        return true;
    }
    if (!create_objects()) {
        logf("scene: creating the build queue and buffers failed: meshes are read and dropped");
        raytracing_ = false;
    }
    return true;
}

bool Scene::create_objects() {
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_COMPUTE};
    if (FAILED(dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)))) return false;
    queue_->SetName(L"rtcore BLAS builds");
    if (FAILED(dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
    D3D12_QUERY_HEAP_DESC qhd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots * 2, 0};
    if (FAILED(dev_->CreateQueryHeap(&qhd, IID_PPV_ARGS(&timestamps_)))) return false;
    queue_->GetTimestampFrequency(&ts_freq_);
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = slots_[i];
        if (FAILED(dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&s.alloc)))) return false;
        s.postbuild = make_buffer(dev_, D3D12_HEAP_TYPE_DEFAULT, 8ull * kMaxItems, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"rtcore postbuild sizes");
        s.readback = make_buffer(dev_, D3D12_HEAP_TYPE_READBACK, 16 + 8ull * kMaxItems, D3D12_RESOURCE_FLAG_NONE,
                                 D3D12_RESOURCE_STATE_COPY_DEST, L"rtcore postbuild readback");
        if (!s.postbuild || !s.readback) return false;
    }
    if (FAILED(dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, slots_[0].alloc, nullptr, IID_PPV_ARGS(&cl_))))
        return false;
    cl_->Close();
    upload_ = make_buffer(dev_, D3D12_HEAP_TYPE_UPLOAD, kUploadBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                          L"rtcore mesh upload ring");
    if (!upload_) return false;
    D3D12_RANGE none{0, 0};
    if (FAILED(upload_->Map(0, &none, reinterpret_cast<void**>(&upload_cpu_)))) return false;
    upload_size_ = kUploadBytes;
    scratch_ = make_buffer(dev_, D3D12_HEAP_TYPE_DEFAULT, kScratchBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"rtcore build scratch");
    if (!scratch_) return false;
    scratch_size_ = kScratchBytes;
    as_pool_.init(dev_, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                  kAsPageBytes, L"rtcore BLAS pool");
    geo_pool_.init(dev_, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, kGeoPageBytes, L"rtcore geometry pool");
    // The materials: their buffer (zeroed as committed resources are: every entry not known) and its upload space. A
    // session goes on without them (every hit a plain grey) if they fail.
    materials_ = make_buffer(dev_, D3D12_HEAP_TYPE_DEFAULT, uint64_t(kMaterialSlots) * kMaterialSlotBytes, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_COMMON, L"rtcore materials");
    material_upload_ = make_buffer(dev_, D3D12_HEAP_TYPE_UPLOAD, uint64_t(kMaterialRing) * kMaterialUploadSlots * kMaterialSlotBytes,
                                   D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, L"rtcore material uploads");
    if (material_upload_ && FAILED(material_upload_->Map(0, &none, reinterpret_cast<void**>(&material_upload_cpu_))))
        material_upload_cpu_ = nullptr;
    if (!materials_ || !material_upload_cpu_) {
        release(materials_);
        release(material_upload_);
        material_upload_cpu_ = nullptr;
        logf("scene: the materials' buffers failed: the rays' hits are a plain grey");
    } else {
        material_cpu_.assign(size_t(kMaterialSlots) * kMaxMaterialEntries * 4, 0u);
        material_dirty_.assign(kMaterialSlots, 0);
    }
    textures_.assign(kMaxTextures, nullptr);
    texture_ok_.assign(kMaxTextures, 0);
    logf("scene: ready (upload ring %llu MB, scratch %llu MB, BLAS pages %llu MB, materials %u slots of %u entries)",
         static_cast<unsigned long long>(kUploadBytes >> 20), static_cast<unsigned long long>(kScratchBytes >> 20),
         static_cast<unsigned long long>(kAsPageBytes >> 20), kMaterialSlots, kMaxMaterialEntries);
    return true;
}

uint64_t Scene::upload_alloc(uint64_t bytes) {
    uint64_t head = upload_head_;
    uint64_t off = head % upload_size_;
    if (off + bytes > upload_size_) {  // to the start: the rest of the ring is skipped
        head += upload_size_ - off;
        off = 0;
    }
    if (head + bytes - upload_tail_ > upload_size_) return UINT64_MAX;
    upload_head_ = head + bytes;
    return off;
}

bool Scene::pump() {
    bool did = false;
    while (!retired_.empty() && uint64_t(retired_.front().frame) <= frames_done_) {
        as_pool_.free(retired_.front().as);
        retired_.pop_front();
    }
    while (!retired_geo_.empty() && uint64_t(retired_geo_.front().frame) <= frames_done_) {
        geo_pool_.free(retired_geo_.front().as);
        retired_geo_.pop_front();
    }
    while (!retired_slots_.empty() && uint64_t(retired_slots_.front().second) <= frames_done_) {
        free_slots_.push_back(retired_slots_.front().first);
        retired_slots_.pop_front();
    }
    for (int i = 0; i < kSlots; ++i) did |= advance(i);
    if (!raytracing_) {
        // Without ray tracing the records are only consumed, so that the plugin never waits on a full ring (a texture's
        // handle closed: it is ours).
        while (const RecordHeader* h = reader_.peek()) {
            if (h->type == kTextureAdd && h->bytes >= sizeof(TextureAdd)) {
                const uint64_t handle = reinterpret_cast<const TextureAdd*>(h)->handle;
                if (handle) CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)));
            }
            reader_.consume();
            did = true;
        }
    } else {
        for (int i = 0; i < kSlots; ++i) {
            if (slots_[i].stage == Slot::Free) {
                did |= read_records(i);
                break;
            }
        }
    }
    if (reader_.corrupt() && !told_corrupt_) {
        told_corrupt_ = true;
        logf("scene: a malformed record in the scene ring: no more scene updates this session");
    }
    publish();
    return did;
}

bool Scene::read_records(int si) {
    Slot& s = slots_[si];
    bool open = false;
    int records = 0;
    scratch_at_ = 0;
    for (;;) {
        const RecordHeader* h = reader_.peek();
        if (!h) break;
        if (h->type == kMeshRemove) {
            if (h->bytes >= sizeof(MeshRemove)) remove_mesh(reinterpret_cast<const MeshRemove*>(h)->id);
            reader_.consume();
            ++records;
            continue;
        }
        if (h->type == kMeshMaterials) {  // (kept by mesh id whether or not the mesh is here yet)
            if (h->bytes >= sizeof(MeshMaterials)) set_materials(*reinterpret_cast<const MeshMaterials*>(h));
            reader_.consume();
            continue;
        }
        if (h->type == kTextureAdd) {
            if (h->bytes >= sizeof(TextureAdd)) add_texture(*reinterpret_cast<const TextureAdd*>(h));
            reader_.consume();
            continue;
        }
        if (h->type != kMeshAdd || h->bytes < sizeof(MeshAdd)) {
            reader_.consume();  // a record this core does not know
            continue;
        }
        const auto* m = reinterpret_cast<const MeshAdd*>(h);
        if (s.items.size() + m->submesh_count > kMaxItems && !s.items.empty()) break;  // the next batch
        if (!open) {
            s.alloc->Reset();
            cl_->Reset(s.alloc, nullptr);
            const D3D12_RESOURCE_BARRIER b = uav_all();  // the previous batch's scratch use
            cl_->ResourceBarrier(1, &b);
            cl_->EndQuery(timestamps_, D3D12_QUERY_TYPE_TIMESTAMP, UINT(si * 2));
            open = true;
        }
        const Added a = add_mesh(*m, reinterpret_cast<const uint8_t*>(h), s, si);
        if (a == Added::NoSpace) break;  // the upload ring is full: the record waits for the next batch
        if (a == Added::Rejected) ++stats_.rejected;
        reader_.consume();
        ++records;
    }
    if (open) {
        const UINT n = UINT(s.items.size());
        D3D12_RESOURCE_BARRIER to_copy[2] = {uav_all(), transition(s.postbuild, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                   D3D12_RESOURCE_STATE_COPY_SOURCE)};
        cl_->ResourceBarrier(2, to_copy);
        cl_->EndQuery(timestamps_, D3D12_QUERY_TYPE_TIMESTAMP, UINT(si * 2 + 1));
        cl_->ResolveQueryData(timestamps_, D3D12_QUERY_TYPE_TIMESTAMP, UINT(si * 2), 2, s.readback, 0);
        if (n) cl_->CopyBufferRegion(s.readback, 16, s.postbuild, 0, 8ull * n);
        const D3D12_RESOURCE_BARRIER back = transition(s.postbuild, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl_->ResourceBarrier(1, &back);
        cl_->Close();
        ID3D12CommandList* lists[] = {cl_};
        queue_->ExecuteCommandLists(1, lists);
        queue_->Signal(fence_, ++fence_value_);
        s.fence = fence_value_;
        s.upload_end = upload_head_;
        s.stage = Slot::Building;
    }
    return records > 0;
}

Scene::Added Scene::add_mesh(const MeshAdd& m, const uint8_t* record, Slot& s, int si) {
    const bool skinned = (m.flags & kMeshSkinned) != 0;
    const bool uv = (m.flags & kMeshUV) != 0;
    const MeshLayout l = mesh_layout(m.vertex_count, m.index_count, m.index_bytes, m.submesh_count, m.range_count, skinned, uv);
    if (l.bytes != m.h.bytes || (m.index_bytes != 2 && m.index_bytes != 4) || !m.vertex_count || !m.submesh_count ||
        m.submesh_count > kMaxItems)
        return Added::Rejected;
    // Sent again (its submesh table changed): it replaces the mesh, once no build of the old one is in flight.
    auto old = meshes_.find(m.id);
    if (old != meshes_.end()) {
        if (old->second.batch_slot >= 0) return Added::NoSpace;  // next pump
        free_mesh(old->second);
        meshes_.erase(old);
        ++stats_.meshes_replaced;
    }
    const uint64_t pos_bytes = uint64_t(m.vertex_count) * 12, idx_bytes = uint64_t(m.index_count) * m.index_bytes;
    const uint64_t skin_bytes = skinned ? uint64_t(m.vertex_count) * 8 : 0;
    const uint64_t uv_bytes = uv ? uint64_t(m.vertex_count) * 8 : 0;
    // The geometry tables: per submesh a header (the texture coordinates' offset from the positions, or all bits set; the
    // mesh's material slot), then for each of its BLAS's geometries (its valid ranges, in order, as the build loop below
    // takes them) its first index and its material map entry, for what rays hit (trace_common.hlsli).
    std::vector<uint32_t> tables, table_at(m.submesh_count, 0);
    std::vector<size_t> headers;
    {
        const Submesh* subs = reinterpret_cast<const Submesh*>(record + l.submeshes);
        const Range* rngs = reinterpret_cast<const Range*>(record + l.ranges);
        for (uint32_t k = 0; k < m.submesh_count; ++k) {
            table_at[k] = uint32_t(tables.size() * 4);
            headers.push_back(tables.size());
            tables.push_back(0xFFFFFFFFu);
            tables.push_back(0xFFFFFFFFu);
            const Submesh& sm = subs[k];
            if (uint64_t(sm.first_range) + sm.range_count > m.range_count) continue;
            for (uint32_t r = 0; r < sm.range_count; ++r) {
                const Range& range = rngs[sm.first_range + r];
                if (!range.index_count || range.index_count % 3 || uint64_t(range.first_index) + range.index_count > m.index_count)
                    continue;
                tables.push_back(range.first_index);
                tables.push_back(range.material);
            }
        }
    }
    const uint64_t table_bytes = tables.size() * 4;
    const uint64_t uv_at = round256(pos_bytes) + round256(idx_bytes) + round256(skin_bytes) + round256(table_bytes);
    if (uv && uv_at < 0xFFFFFFFFull)
        for (size_t h : headers) tables[h] = uint32_t(uv_at);
    const uint64_t need = uv_at + round256(uv_bytes);
    if (need > upload_size_ / 2) return Added::Rejected;
    const uint64_t off = upload_alloc(need);
    if (off == UINT64_MAX) return Added::NoSpace;
    // its material slot (a freed one, else the next never used), filled with what the plugin sent of its materials
    uint32_t slot = 0xFFFFFFFFu;
    if (materials_) {
        if (!free_slots_.empty()) {
            slot = free_slots_.back();
            free_slots_.pop_back();
        } else if (slots_used_ < kMaterialSlots) {
            slot = slots_used_++;
        } else {
            ++mstats_.slots_over;
        }
        if (slot != 0xFFFFFFFFu) {
            fill_slot(slot, m.id);
            for (size_t h : headers) tables[h + 1] = slot;
        }
    }
    memcpy(upload_cpu_ + off, record + l.positions, pos_bytes);
    memcpy(upload_cpu_ + off + round256(pos_bytes), record + l.indices, idx_bytes);
    if (skinned) memcpy(upload_cpu_ + off + round256(pos_bytes) + round256(idx_bytes), record + l.skin, skin_bytes);
    if (table_bytes)
        memcpy(upload_cpu_ + off + round256(pos_bytes) + round256(idx_bytes) + round256(skin_bytes), tables.data(), table_bytes);
    if (uv_bytes) memcpy(upload_cpu_ + off + uv_at, record + l.uv, uv_bytes);
    const D3D12_GPU_VIRTUAL_ADDRESS vb = upload_->GetGPUVirtualAddress() + off;
    const D3D12_GPU_VIRTUAL_ADDRESS ib = vb + round256(pos_bytes);

    Mesh mesh;
    mesh.id = m.id;
    mesh.material_slot = slot;
    mesh.source = m.source;
    mesh.vertex_count = m.vertex_count;
    mesh.index_count = m.index_count;
    mesh.index_bytes = m.index_bytes;
    memcpy(mesh.name, m.name, sizeof(mesh.name));
    mesh.name[sizeof(mesh.name) - 1] = 0;
    mesh.submeshes.assign(reinterpret_cast<const Submesh*>(record + l.submeshes),
                          reinterpret_cast<const Submesh*>(record + l.submeshes) + m.submesh_count);
    mesh.ranges.assign(reinterpret_cast<const Range*>(record + l.ranges), reinterpret_cast<const Range*>(record + l.ranges) + m.range_count);
    mesh.blas.resize(m.submesh_count);
    // Into the geometry pool, by this batch (it stays: rays read the triangles they hit, skinning the bind pose).
    mesh.geo = geo_pool_.alloc(need);
    if (mesh.geo) {
        cl_->CopyBufferRegion(mesh.geo.buffer, mesh.geo.offset, upload_, off, need);
        mesh.indices_at = round256(pos_bytes);
        mesh.skin_at = mesh.indices_at + round256(idx_bytes);
        mesh.tables_at = mesh.skin_at + round256(skin_bytes);
        mesh.table_at = std::move(table_at);
        mesh.skinned = skinned;
        stats_.geo_bytes += int64_t(need);
        if (skinned) ++stats_.skinned_live;
    } else if (stats_.geo_full++ == 0) {
        logf("scene: the geometry pool is full (%.1f MB): meshes from here on have no hit attributes", geo_pool_.used() / 1048576.0);
    }

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> descs;
    for (uint32_t k = 0; k < m.submesh_count; ++k) {
        const Submesh& sm = mesh.submeshes[k];
        if (uint64_t(sm.first_range) + sm.range_count > m.range_count) continue;
        descs.clear();
        uint32_t tris = 0;
        for (uint32_t r = 0; r < sm.range_count; ++r) {
            const Range& range = mesh.ranges[sm.first_range + r];
            if (!range.index_count || range.index_count % 3 || uint64_t(range.first_index) + range.index_count > m.index_count) continue;
            D3D12_RAYTRACING_GEOMETRY_DESC g{};
            g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            g.Triangles.IndexFormat = m.index_bytes == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
            g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            g.Triangles.IndexCount = range.index_count;
            g.Triangles.VertexCount = m.vertex_count;
            g.Triangles.IndexBuffer = ib + uint64_t(range.first_index) * m.index_bytes;
            g.Triangles.VertexBuffer.StartAddress = vb;
            g.Triangles.VertexBuffer.StrideInBytes = 12;
            descs.push_back(g);
            tris += range.index_count / 3;
        }
        if (descs.empty()) continue;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
                   D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
        in.NumDescs = UINT(descs.size());
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = descs.data();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        dev_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
        if (!info.ResultDataMaxSizeInBytes) continue;
        GpuPool::Alloc result = as_pool_.alloc(info.ResultDataMaxSizeInBytes);
        if (!result) {
            logf("scene: out of GPU memory for acceleration structures (%.1f MB in use)", as_pool_.used() / 1048576.0);
            break;
        }
        const uint64_t scratch_bytes = round256(info.ScratchDataSizeInBytes);
        D3D12_GPU_VIRTUAL_ADDRESS scratch = 0;
        if (scratch_bytes > scratch_size_) {
            ID3D12Resource* big = make_buffer(dev_, D3D12_HEAP_TYPE_DEFAULT, scratch_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"rtcore build scratch (large)");
            if (!big) {
                as_pool_.free(result);
                continue;
            }
            s.big_scratch.push_back(big);
            scratch = big->GetGPUVirtualAddress();
        } else {
            if (scratch_at_ + scratch_bytes > scratch_size_) {  // reuse from the start once the builds before are done
                const D3D12_RESOURCE_BARRIER b = uav_all();
                cl_->ResourceBarrier(1, &b);
                scratch_at_ = 0;
            }
            scratch = scratch_->GetGPUVirtualAddress() + scratch_at_;
            scratch_at_ += scratch_bytes;
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
        bd.Inputs = in;
        bd.DestAccelerationStructureData = result.va;
        bd.ScratchAccelerationStructureData = scratch;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC pb{};
        pb.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
        pb.DestBuffer = s.postbuild->GetGPUVirtualAddress() + 8ull * s.items.size();
        cl_->BuildRaytracingAccelerationStructure(&bd, 1, &pb);
        Item item{};
        item.mesh_id = m.id;
        item.submesh = k;
        item.built = result;
        item.triangles = tris;
        s.items.push_back(item);
        stats_.uncompacted_bytes += result.size;
    }
    mesh.batch_slot = si;
    meshes_[m.id] = std::move(mesh);
    ++stats_.meshes_added;
    return Added::Yes;
}

void Scene::remove_mesh(uint32_t id) {
    mesh_glass_.erase(id);
    mesh_materials_.erase(id);
    auto it = meshes_.find(id);
    if (it == meshes_.end()) {
        ++stats_.unknown_removes;
        return;
    }
    ++stats_.meshes_removed;
    if (it->second.batch_slot >= 0) {  // its batch frees it when done
        it->second.removed = true;
        return;
    }
    // Phase 2b: frames in flight will reference BLAS through the TLAS; their release will wait for them.
    free_mesh(it->second);
    meshes_.erase(it);
}

void Scene::free_mesh(Mesh& m) {
    if (m.material_slot != 0xFFFFFFFFu) {  // a frame still queued may read it
        if (frames_queued_ > 0 && uint64_t(frames_queued_) > frames_done_)
            retired_slots_.push_back({m.material_slot, frames_queued_});
        else
            free_slots_.push_back(m.material_slot);
        m.material_slot = 0xFFFFFFFFu;
    }
    if (m.geo) {
        // a frame still queued may read it (hit attributes, skinning)
        stats_.geo_bytes -= int64_t(m.geo.size);
        if (m.skinned) --stats_.skinned_live;
        if (frames_queued_ > 0 && uint64_t(frames_queued_) > frames_done_)
            retired_geo_.push_back({m.geo, frames_queued_});
        else
            geo_pool_.free(m.geo);
        m.geo = {};
    }
    for (Blas& b : m.blas) {
        if (b.ready) {
            --stats_.live_blas;
            stats_.live_triangles -= b.triangles;
            // a TLAS of a frame still queued may reference it
            if (b.as && frames_queued_ > 0 && uint64_t(frames_queued_) > frames_done_) {
                retired_.push_back({b.as, frames_queued_});
                b = Blas{};
                continue;
            }
        }
        as_pool_.free(b.as);
        b = Blas{};
    }
}

const char* Scene::mesh_name(uint32_t mesh_id) const {
    auto it = meshes_.find(mesh_id);
    return it == meshes_.end() || !it->second.name[0] ? "?" : it->second.name;
}

uint32_t Scene::submesh_count(uint32_t mesh_id) const {
    auto it = meshes_.find(mesh_id);
    return it == meshes_.end() ? 0u : uint32_t(it->second.blas.size());
}

uint32_t Scene::material_geometries(uint32_t mesh_id, uint32_t submesh, uint32_t materials, uint32_t* geometries) const {
    *geometries = 0;
    auto it = meshes_.find(mesh_id);
    if (it == meshes_.end()) return 0;
    const Mesh& m = it->second;
    if (submesh >= m.submeshes.size()) return 0;
    const Submesh& sm = m.submeshes[submesh];
    if (uint64_t(sm.first_range) + sm.range_count > m.ranges.size()) return 0;
    uint32_t mask = 0, g = 0;
    for (uint32_t r = 0; r < sm.range_count; ++r) {
        const Range& range = m.ranges[sm.first_range + r];
        // as add_mesh builds them: a geometry per valid range
        if (!range.index_count || range.index_count % 3 || uint64_t(range.first_index) + range.index_count > m.index_count) continue;
        if (g < 32 && range.material < 32 && ((materials >> range.material) & 1u)) mask |= 1u << g;
        ++g;
    }
    *geometries = g;
    return mask;
}

uint32_t Scene::geometry_material(uint32_t mesh_id, uint32_t submesh, uint32_t geometry) const {
    auto it = meshes_.find(mesh_id);
    if (it == meshes_.end()) return 0xFFFFFFFFu;
    const Mesh& m = it->second;
    if (submesh >= m.submeshes.size()) return 0xFFFFFFFFu;
    const Submesh& sm = m.submeshes[submesh];
    if (uint64_t(sm.first_range) + sm.range_count > m.ranges.size()) return 0xFFFFFFFFu;
    uint32_t g = 0;
    for (uint32_t r = 0; r < sm.range_count; ++r) {
        const Range& range = m.ranges[sm.first_range + r];
        if (!range.index_count || range.index_count % 3 || uint64_t(range.first_index) + range.index_count > m.index_count) continue;
        if (g++ == geometry) return range.material;
    }
    return 0xFFFFFFFFu;
}

Scene::Lookup Scene::lookup(uint32_t mesh_id, uint32_t submesh, D3D12_GPU_VIRTUAL_ADDRESS* blas, const float** offset) const {
    auto it = meshes_.find(mesh_id);
    if (it == meshes_.end() || it->second.removed) return Lookup::NoMesh;
    const Mesh& m = it->second;
    if (submesh >= m.blas.size()) return Lookup::SubmeshBeyond;
    if (!m.blas[submesh].ready) return Lookup::NoBlas;
    *blas = m.blas[submesh].as.va;
    *offset = m.submeshes[submesh].offset;
    return Lookup::Ok;
}

bool Scene::skin_view(uint32_t mesh_id, uint32_t submesh, SkinView* out) const {
    auto it = meshes_.find(mesh_id);
    if (it == meshes_.end()) return false;
    const Mesh& m = it->second;
    if (m.removed || m.batch_slot >= 0 || !m.geo || !m.skinned || submesh >= m.submeshes.size()) return false;
    const Submesh& sm = m.submeshes[submesh];
    if (!sm.range_count || uint64_t(sm.first_range) + sm.range_count > m.ranges.size()) return false;
    const D3D12_GPU_VIRTUAL_ADDRESS base = m.geo.va;
    out->positions = base;
    out->indices = base + m.indices_at;
    out->skin = base + m.skin_at;
    out->vertex_count = m.vertex_count;
    out->index_count = m.index_count;
    out->index_bytes = m.index_bytes;
    out->ranges = m.ranges.data() + sm.first_range;
    out->range_count = sm.range_count;
    return true;
}

bool Scene::geo_ref(uint32_t mesh_id, uint32_t submesh, GeoRef* out) const {
    auto it = meshes_.find(mesh_id);
    if (it == meshes_.end()) return false;
    const Mesh& m = it->second;
    if (m.removed || m.batch_slot >= 0 || !m.geo || submesh >= m.table_at.size()) return false;
    out->page = m.geo.page;
    out->positions = uint32_t(m.geo.offset);
    out->indices = uint32_t(m.geo.offset + m.indices_at);
    out->table = uint32_t(m.geo.offset + m.tables_at + m.table_at[submesh]);
    out->index_bytes = m.index_bytes;
    return true;
}

bool Scene::advance(int si) {
    Slot& s = slots_[si];
    if (s.stage == Slot::Free || fence_->GetCompletedValue() < s.fence) return false;
    if (s.stage == Slot::Building)
        finish_build(s);
    else
        finish_compaction(s);
    return true;
}

void Scene::finish_build(Slot& s) {
    upload_tail_ = std::max(upload_tail_, s.upload_end);
    for (ID3D12Resource* r : s.big_scratch) r->Release();
    s.big_scratch.clear();
    const size_t n = s.items.size();
    D3D12_RANGE rr{0, SIZE_T(16 + 8 * n)};
    uint64_t* rb = nullptr;
    std::vector<uint64_t> sizes(n, 0);
    if (SUCCEEDED(s.readback->Map(0, &rr, reinterpret_cast<void**>(&rb)))) {
        if (rb[1] > rb[0]) stats_.build_ticks += rb[1] - rb[0];
        for (size_t i = 0; i < n; ++i) sizes[i] = rb[2 + i];
        D3D12_RANGE none{0, 0};
        s.readback->Unmap(0, &none);
    }
    bool any = false;
    s.alloc->Reset();
    cl_->Reset(s.alloc, nullptr);
    for (size_t i = 0; i < n; ++i) {
        Item& it = s.items[i];
        auto m = meshes_.find(it.mesh_id);
        if (m == meshes_.end() || m->second.removed) {
            as_pool_.free(it.built);
            continue;
        }
        if (sizes[i] && sizes[i] < it.built.size) it.compacted = as_pool_.alloc(sizes[i]);
        if (!it.compacted) {  // keep it as built
            m->second.blas[it.submesh] = Blas{it.built, it.triangles, true};
            it.built = {};
            ++stats_.live_blas;
            stats_.live_triangles += it.triangles;
            ++stats_.blas_built;
            continue;
        }
        cl_->CopyRaytracingAccelerationStructure(it.compacted.va, it.built.va,
                                                 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
        any = true;
    }
    cl_->Close();
    if (any) {
        ID3D12CommandList* lists[] = {cl_};
        queue_->ExecuteCommandLists(1, lists);
        queue_->Signal(fence_, ++fence_value_);
        s.fence = fence_value_;
        s.stage = Slot::Compacting;
        return;
    }
    settle_meshes(s);
}

void Scene::finish_compaction(Slot& s) {
    for (Item& it : s.items) {
        if (!it.compacted) continue;
        as_pool_.free(it.built);
        auto m = meshes_.find(it.mesh_id);
        if (m == meshes_.end() || m->second.removed) {
            as_pool_.free(it.compacted);
            continue;
        }
        stats_.compacted_bytes += it.compacted.size;
        m->second.blas[it.submesh] = Blas{it.compacted, it.triangles, true};
        it.compacted = {};
        ++stats_.live_blas;
        stats_.live_triangles += it.triangles;
        ++stats_.blas_built;
    }
    settle_meshes(s);
}

// The batch is done: its meshes are complete, or gone if the game released them meanwhile.
void Scene::settle_meshes(Slot& s) {
    for (auto it = meshes_.begin(); it != meshes_.end();) {
        Mesh& m = it->second;
        if (m.batch_slot >= 0 && &slots_[m.batch_slot] == &s) {
            if (m.removed) {
                free_mesh(m);
                it = meshes_.erase(it);
                continue;
            }
            m.batch_slot = -1;
        }
        ++it;
    }
    s.items.clear();
    s.stage = Slot::Free;
}

void Scene::add_texture(const TextureAdd& t) {
    HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(t.handle));
    ID3D12Resource* res = nullptr;
    HRESULT hr = E_INVALIDARG;
    if (handle && t.id < kMaxTextures && !textures_[t.id] && texture_table_.ptr) hr = dev_->OpenSharedHandle(handle, IID_PPV_ARGS(&res));
    if (handle) CloseHandle(handle);  // ours either way (the resource keeps its own reference)
    if (FAILED(hr) || !res) {
        if (mstats_.texture_failed++ < 4)
            logf("scene: texture %u (%ux%u, format %u, %u mips) could not be opened (0x%08lX)", t.id, t.width, t.height, t.format,
                 t.mips, static_cast<unsigned long>(hr));
        return;
    }
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize != 1 || !d.MipLevels) {
        if (mstats_.texture_bad++ < 4) logf("scene: texture %u is not a plain 2D texture (dimension %d)", t.id, int(d.Dimension));
        res->Release();
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = static_cast<DXGI_FORMAT>(t.format);
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = d.MipLevels;
    D3D12_CPU_DESCRIPTOR_HANDLE at = texture_table_;
    at.ptr += SIZE_T(t.id) * texture_inc_;
    dev_->CreateShaderResourceView(res, &sd, at);  // no frame reads it yet: no material names it until now
    textures_[t.id] = res;
    texture_ok_[t.id] = 1;
    ++mstats_.textures;
    const D3D12_RESOURCE_ALLOCATION_INFO ai = dev_->GetResourceAllocationInfo(0, 1, &d);
    mstats_.texture_bytes += ai.SizeInBytes;
}

void Scene::set_materials(const MeshMaterials& mm) {
    ++mstats_.records;
    if (mm.clear | mm.translucent)
        mesh_glass_[mm.mesh_id] = {mm.clear, mm.translucent};
    else
        mesh_glass_.erase(mm.mesh_id);
    if (!materials_) return;
    const uint32_t count = std::min<uint32_t>(mm.count, kMaxMaterialEntries);
    if (sizeof(MeshMaterials) + uint64_t(count) * sizeof(MaterialEntry) > mm.h.bytes) return;
    const auto* in = reinterpret_cast<const MaterialEntry*>(reinterpret_cast<const uint8_t*>(&mm) + sizeof(MeshMaterials));
    std::vector<uint32_t>& packed = mesh_materials_[mm.mesh_id];
    packed.assign(size_t(count) * 4, 0u);
    for (uint32_t i = 0; i < count; ++i) {
        const bool ok = in[i].texture < kMaxTextures && texture_ok_[in[i].texture];
        pack_material(in[i], ok, packed.data() + size_t(i) * 4);
        ++mstats_.entries;
        if (ok) ++mstats_.textured_entries;
    }
    auto it = meshes_.find(mm.mesh_id);
    if (it != meshes_.end() && it->second.material_slot != 0xFFFFFFFFu) fill_slot(it->second.material_slot, mm.mesh_id);
}

void Scene::fill_slot(uint32_t slot, uint32_t mesh_id) {
    uint32_t* dst = material_cpu_.data() + size_t(slot) * kMaxMaterialEntries * 4;
    memset(dst, 0, size_t(kMaterialSlotBytes));
    auto it = mesh_materials_.find(mesh_id);
    if (it != mesh_materials_.end())
        memcpy(dst, it->second.data(), std::min<size_t>(it->second.size() * 4, size_t(kMaterialSlotBytes)));
    if (!material_dirty_[slot]) {
        material_dirty_[slot] = 1;
        dirty_slots_.push_back(slot);
    }
}

void Scene::record_material_uploads(ID3D12GraphicsCommandList* cl, int ring) {
    recorded_slots_ = 0;
    if (!materials_ || dirty_slots_.empty()) return;
    const size_t n = std::min<size_t>(dirty_slots_.size(), kMaterialUploadSlots);
    const uint64_t base = uint64_t(ring % kMaterialRing) * kMaterialUploadSlots * kMaterialSlotBytes;
    const D3D12_RESOURCE_BARRIER to_copy = transition(materials_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->ResourceBarrier(1, &to_copy);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t slot = dirty_slots_[i];
        memcpy(material_upload_cpu_ + base + i * kMaterialSlotBytes, material_cpu_.data() + size_t(slot) * kMaxMaterialEntries * 4,
               size_t(kMaterialSlotBytes));
        cl->CopyBufferRegion(materials_, uint64_t(slot) * kMaterialSlotBytes, material_upload_, base + i * kMaterialSlotBytes,
                             kMaterialSlotBytes);
    }
    // read by the frame's passes (a later list: the buffer is back in the common state by then, read from it)
    const D3D12_RESOURCE_BARRIER to_read =
        transition(materials_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl->ResourceBarrier(1, &to_read);
    recorded_slots_ = n;
}

void Scene::commit_material_uploads() {
    const size_t n = std::min(recorded_slots_, dirty_slots_.size());
    if (!n) return;
    for (size_t i = 0; i < n; ++i) material_dirty_[dirty_slots_[i]] = 0;
    dirty_slots_.erase(dirty_slots_.begin(), dirty_slots_.begin() + ptrdiff_t(n));
    recorded_slots_ = 0;
    ++mstats_.uploads;
    mstats_.uploaded_slots += n;
}

void Scene::publish() {
    rfgrt_ipc::store64(&b_->scene_meshes, int64_t(meshes_.size()));
    rfgrt_ipc::store64(&b_->scene_blas, stats_.live_blas);
    rfgrt_ipc::store64(&b_->scene_triangles, stats_.live_triangles);
    rfgrt_ipc::store64(&b_->scene_gpu_bytes, int64_t(as_pool_.used()));
    rfgrt_ipc::store64(&b_->scene_build_us, int64_t(double(stats_.build_ticks) * 1e6 / double(ts_freq_)));
}

void Scene::log_stats() {
    logf("scene: %zu meshes (%llu added, %llu removed, %llu replaced; %lld skinned; geometry pool %.1f MB), %lld BLAS with %lld triangles, "
         "%.1f MB of %.1f MB in the BLAS pool; compaction %.0f%% of the built size; builds %.1f ms of GPU time; rejected %llu, unknown "
         "removes %llu; upload ring %.1f MB in use",
         meshes_.size(), static_cast<unsigned long long>(stats_.meshes_added), static_cast<unsigned long long>(stats_.meshes_removed),
         static_cast<unsigned long long>(stats_.meshes_replaced), static_cast<long long>(stats_.skinned_live),
         double(stats_.geo_bytes) / 1048576.0, static_cast<long long>(stats_.live_blas), static_cast<long long>(stats_.live_triangles), as_pool_.used() / 1048576.0,
         as_pool_.reserved() / 1048576.0,
         stats_.uncompacted_bytes ? 100.0 * double(stats_.compacted_bytes) / double(stats_.uncompacted_bytes) : 0.0,
         double(stats_.build_ticks) * 1e3 / double(ts_freq_), static_cast<unsigned long long>(stats_.rejected),
         static_cast<unsigned long long>(stats_.unknown_removes), (upload_head_ - upload_tail_) / 1048576.0);
    logf("scene: materials: %llu records (%llu entries, %llu with a texture), %u of %u slots handed out (%zu free, %zu waiting "
         "for frames, %llu meshes without one); textures %llu (%.1f MB; %llu not opened, %llu not 2D); %llu uploads of %llu "
         "slots, %zu waiting",
         static_cast<unsigned long long>(mstats_.records), static_cast<unsigned long long>(mstats_.entries),
         static_cast<unsigned long long>(mstats_.textured_entries), slots_used_, kMaterialSlots, free_slots_.size(),
         retired_slots_.size(), static_cast<unsigned long long>(mstats_.slots_over), static_cast<unsigned long long>(mstats_.textures),
         mstats_.texture_bytes / 1048576.0, static_cast<unsigned long long>(mstats_.texture_failed),
         static_cast<unsigned long long>(mstats_.texture_bad), static_cast<unsigned long long>(mstats_.uploads),
         static_cast<unsigned long long>(mstats_.uploaded_slots), dirty_slots_.size());
}

void Scene::shutdown(uint32_t timeout_ms) {
    if (fence_ && fence_->GetCompletedValue() < fence_value_) {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence_->SetEventOnCompletion(fence_value_, ev);
        WaitForSingleObject(ev, timeout_ms);
        CloseHandle(ev);
    }
    frames_queued_ = 0;  // the frame queue is drained by now
    for (auto& kv : meshes_) free_mesh(kv.second);
    meshes_.clear();
    for (Retired& r : retired_) as_pool_.free(r.as);
    retired_.clear();
    for (Retired& r : retired_geo_) geo_pool_.free(r.as);
    retired_geo_.clear();
    for (Slot& s : slots_) {
        for (Item& it : s.items) {
            as_pool_.free(it.built);
            as_pool_.free(it.compacted);
        }
        for (ID3D12Resource* r : s.big_scratch) r->Release();
        s.big_scratch.clear();
        release(s.alloc);
        release(s.postbuild);
        release(s.readback);
    }
    as_pool_.release();
    geo_pool_.release();
    for (ID3D12Resource*& t : textures_) release(t);
    if (material_upload_ && material_upload_cpu_) material_upload_->Unmap(0, nullptr);
    material_upload_cpu_ = nullptr;
    release(material_upload_);
    release(materials_);
    release(scratch_);
    if (upload_) upload_->Unmap(0, nullptr);
    release(upload_);
    release(timestamps_);
    release(cl_);
    release(fence_);
    release(queue_);
    release(dev_);
}

}  // namespace rtcore
