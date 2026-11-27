#pragma once
// The core's mirror of the game's scene (research/docs/03, phase 2): the meshes the plugin sends through the scene
// ring, each submesh built into a bottom-level acceleration structure (BLAS) on a queue of its own, then compacted.
//
// Per mesh record: its positions and indices go into an upload ring (the builds read them from there) and, by the same
// batch, into the geometry pool, where they stay for what rays hit (hit attributes: the triangle's normal, later its
// texture coordinates); one BLAS per submesh with one geometry per draw range, PREFER_FAST_TRACE and compaction.
// Builds are batched per pump; a batch goes build -> (fence) -> compaction copies -> (fence) -> ready.
#include <d3d12.h>

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "gpu_pool.h"
#include "rfgrt_ipc.h"
#include "scene_ring.h"

namespace rtcore {

using LogFn = void (*)(const char* line);

class Scene {
public:
    // dev may lack ray tracing (tier 0): records are then read and dropped, so that the plugin never waits.
    bool init(ID3D12Device* dev, rfgrt_ipc::Block* block, const uint8_t* ring, bool raytracing, LogFn log);
    // Reads records and advances batches; never waits for the GPU. True if it did something.
    bool pump();
    // Frames queued on the frame queue and done there (fence B): a BLAS released while a queued frame's TLAS may
    // reference it is freed once that frame is done.
    void set_frames(int64_t queued, uint64_t done) {
        frames_queued_ = queued;
        frames_done_ = done;
    }
    enum class Lookup { Ok, NoMesh, NoBlas, SubmeshBeyond };  // NoBlas: not built (yet); SubmeshBeyond: past the mesh as sent
    // The BLAS of a mesh's submesh and the submesh's offset, for a TLAS instance.
    uint32_t submesh_count(uint32_t mesh_id) const;
    const char* mesh_name(uint32_t mesh_id) const;  // as the plugin named it; "?" for none
    // Which of a submesh's BLAS geometries (its valid ranges, in order; the first 32) draw a material whose bit is set
    // in `materials` (bit i: material map entry i); *geometries: how many it has.
    uint32_t material_geometries(uint32_t mesh_id, uint32_t submesh, uint32_t materials, uint32_t* geometries) const;
    // A mesh's glass by material map entry (kMeshMaterials: bit i entry i): false if the plugin sent none (or none there)
    // A texture id the core has opened (its table entry holds it).
    bool texture_ready(uint32_t id) const { return id < texture_ok_.size() && texture_ok_[id] != 0; }
    bool mesh_glass(uint32_t mesh_id, uint32_t* clear, uint32_t* translucent) const {
        auto it = mesh_glass_.find(mesh_id);
        if (it == mesh_glass_.end()) return false;
        *clear = it->second.first;
        *translucent = it->second.second;
        return true;
    }
    // The plugin sent the mesh's materials (kMeshMaterials)
    bool has_materials(uint32_t mesh_id) const { return mesh_materials_.count(mesh_id) != 0; }
    // The material map entry a submesh's BLAS geometry draws (0xFFFFFFFF: none such)
    uint32_t geometry_material(uint32_t mesh_id, uint32_t submesh, uint32_t geometry) const;
    Lookup lookup(uint32_t mesh_id, uint32_t submesh, D3D12_GPU_VIRTUAL_ADDRESS* blas, const float** offset) const;
    // A skinned mesh's bind pose on the GPU (kMeshSkinned), for posing its instances (skinning.h), once its batch is
    // done: positions (float3), skin (4 bone indices, 4 weights a vertex), indices; the submesh's ranges.
    struct SkinView {
        D3D12_GPU_VIRTUAL_ADDRESS positions = 0, skin = 0, indices = 0;
        uint32_t vertex_count = 0, index_count = 0, index_bytes = 0;
        const rfgrt_scene::Range* ranges = nullptr;
        uint32_t range_count = 0;
    };
    bool skin_view(uint32_t mesh_id, uint32_t submesh, SkinView* out) const;
    // Where a mesh's submesh lies in the geometry pool, for the frame's instance table: the page; byte offsets in it of
    // the positions (float3), the indices and the submesh's geometry table (a uint each: the first index of each of
    // its BLAS's geometries, in their order). Once its batch is done.
    struct GeoRef {
        uint32_t page = 0, positions = 0, indices = 0, table = 0, index_bytes = 0;
    };
    bool geo_ref(uint32_t mesh_id, uint32_t submesh, GeoRef* out) const;
    const GpuPool& geo_pool() const { return geo_pool_; }

    // --- materials (what the rays' hits look like: trace_common.hlsli hit_surface) ---
    // The materials' textures (kTextureAdd) get their views in the shader-visible heap from `first` on (kMaxTextures of
    // them, null until their texture arrives; each written once, before any material names it). Before pump().
    void set_texture_table(D3D12_CPU_DESCRIPTOR_HANDLE first, UINT increment) {
        texture_table_ = first;
        texture_inc_ = increment;
    }
    // Each mesh has a slot of kMaxMaterialEntries uint4 in the materials buffer (its index in its geometry tables'
    // header): its material map entries' textures and constants (kMeshMaterials), packed (scene.cpp pack_material).
    static constexpr uint32_t kMaterialSlots = 16384;
    // The slots changed since, copied into the materials buffer by `cl` (the frame's first list: before any ray reads
    // them); `ring`: the frame's ring slot (its previous frame done: its upload space is free). Then commit once the list
    // is executed (a list dropped instead leaves them to the next frame's).
    void record_material_uploads(ID3D12GraphicsCommandList* cl, int ring);
    void commit_material_uploads();
    D3D12_GPU_VIRTUAL_ADDRESS materials_va() const { return materials_ ? materials_->GetGPUVirtualAddress() : 0; }
    // Waits (up to timeout_ms) for the batches in flight, then releases everything.
    void shutdown(uint32_t timeout_ms);
    void log_stats();

    struct Blas {
        GpuPool::Alloc as;
        uint32_t triangles = 0;
        bool ready = false;
    };
    struct Mesh {
        uint32_t id = 0, source = 0;
        uint32_t vertex_count = 0, index_count = 0, index_bytes = 0;
        std::vector<rfgrt_scene::Submesh> submeshes;
        std::vector<rfgrt_scene::Range> ranges;
        std::vector<Blas> blas;  // one per submesh
        bool removed = false;    // released by the game while its batch was in flight
        int batch_slot = -1;     // the batch building it, until its BLAS are ready
        char name[64] = {};
        // In the geometry pool: positions, indices, skin (kMeshSkinned), the geometry tables (at table_at[submesh]).
        GpuPool::Alloc geo;
        uint64_t indices_at = 0, skin_at = 0, tables_at = 0;
        std::vector<uint32_t> table_at;
        bool skinned = false;
        uint32_t material_slot = 0xFFFFFFFFu;  // in the materials buffer (none: past kMaterialSlots)
    };

private:
    static constexpr int kSlots = 4;  // batches in flight
    // BLAS per batch, and a mesh's most submeshes (a Wrecking Crew building has more than 2,000 Geo-Mod pieces)
    static constexpr uint32_t kMaxItems = 32768;
    struct Item {
        uint32_t mesh_id, submesh;
        GpuPool::Alloc built;      // uncompacted
        GpuPool::Alloc compacted;
        uint32_t triangles;
    };
    struct Slot {
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12Resource* postbuild = nullptr;  // default heap, UAV: compacted sizes
        ID3D12Resource* readback = nullptr;   // [2 timestamps][compacted sizes]
        enum Stage { Free, Building, Compacting } stage = Free;
        uint64_t fence = 0;        // build-queue fence value the stage ends at
        uint64_t upload_end = 0;   // upload ring position released when the build is done
        std::vector<Item> items;
        std::vector<ID3D12Resource*> big_scratch;  // for builds larger than the scratch arena
    };
    enum class Added { Yes, NoSpace, Rejected };

    bool create_objects();
    bool read_records(int slot);
    Added add_mesh(const rfgrt_scene::MeshAdd& m, const uint8_t* record, Slot& slot, int slot_index);
    void settle_meshes(Slot& s);
    void remove_mesh(uint32_t id);
    bool advance(int slot);
    void finish_build(Slot& s);
    void finish_compaction(Slot& s);
    void free_mesh(Mesh& m);
    uint64_t upload_alloc(uint64_t bytes);
    void publish();
    void logf(const char* fmt, ...);

    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> mesh_glass_;  // kMeshMaterials: clear, translucent

    // Materials: each mesh's packed entries as the plugin sent them (kept by mesh id whether or not the mesh is here yet),
    // the slots (free list; a freed slot waits for the frames that may read it), the buffer and its upload space.
    void add_texture(const rfgrt_scene::TextureAdd& t);
    void set_materials(const rfgrt_scene::MeshMaterials& mm);
    void fill_slot(uint32_t slot, uint32_t mesh_id);  // the mesh's entries, or none, into the slot's CPU copy (dirty)
    static constexpr uint64_t kMaterialSlotBytes = uint64_t(rfgrt_scene::kMaxMaterialEntries) * 16;
    static constexpr uint32_t kMaterialUploadSlots = 512;  // slots a frame copies at most
    static constexpr int kMaterialRing = 4;                 // the frames' upload areas (at least the frames in flight)
    std::unordered_map<uint32_t, std::vector<uint32_t>> mesh_materials_;  // mesh id -> 4 uints an entry
    std::vector<uint32_t> material_cpu_;   // kMaterialSlots x kMaxMaterialEntries x 4
    std::vector<uint8_t> material_dirty_;  // per slot
    std::vector<uint32_t> dirty_slots_;
    size_t recorded_slots_ = 0;  // of them, the first copied by the list being recorded
    std::vector<uint32_t> free_slots_;
    uint32_t slots_used_ = 0;  // slots handed out once (below kMaterialSlots)
    std::deque<std::pair<uint32_t, int64_t>> retired_slots_;  // freed once the frame queue has finished the frame
    ID3D12Resource* materials_ = nullptr;         // default heap, kMaterialSlots x kMaterialSlotBytes
    ID3D12Resource* material_upload_ = nullptr;   // kMaterialRing x kMaterialUploadSlots x kMaterialSlotBytes
    uint8_t* material_upload_cpu_ = nullptr;
    bool materials_initialized_ = false;          // the buffer's slots all set to no material once
    // Textures: the core's references (kept for the session), whether each view is there.
    D3D12_CPU_DESCRIPTOR_HANDLE texture_table_{};
    UINT texture_inc_ = 0;
    std::vector<ID3D12Resource*> textures_;
    std::vector<uint8_t> texture_ok_;
    struct MaterialStats {
        uint64_t textures = 0, texture_bytes = 0, texture_failed = 0, texture_bad = 0, records = 0, entries = 0,
                 textured_entries = 0, slots_over = 0, uploads = 0, uploaded_slots = 0;
    } mstats_;

    LogFn log_ = nullptr;
    rfgrt_ipc::Block* b_ = nullptr;
    rfgrt_scene::RingReader reader_;
    bool raytracing_ = false;
    ID3D12Device5* dev_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ID3D12GraphicsCommandList4* cl_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    uint64_t fence_value_ = 0;
    ID3D12QueryHeap* timestamps_ = nullptr;
    uint64_t ts_freq_ = 1;
    Slot slots_[kSlots];

    // upload ring: positions and indices until their builds are done
    ID3D12Resource* upload_ = nullptr;
    uint8_t* upload_cpu_ = nullptr;
    uint64_t upload_size_ = 0, upload_head_ = 0, upload_tail_ = 0;

    ID3D12Resource* scratch_ = nullptr;  // reused by every batch; builds past its end wait on a UAV barrier
    uint64_t scratch_size_ = 0, scratch_at_ = 0;

    GpuPool as_pool_;
    GpuPool geo_pool_;
    std::unordered_map<uint32_t, Mesh> meshes_;
    struct Retired {
        GpuPool::Alloc as;
        int64_t frame;  // freed once the frame queue has finished this frame
    };
    std::deque<Retired> retired_;
    std::deque<Retired> retired_geo_;  // geometry pool pieces, freed once the frames that may read them are done
    int64_t frames_queued_ = 0;
    uint64_t frames_done_ = 0;

    struct Stats {
        uint64_t meshes_added = 0, meshes_removed = 0, meshes_replaced = 0, blas_built = 0, unknown_removes = 0, rejected = 0;
        uint64_t build_ticks = 0, uncompacted_bytes = 0, compacted_bytes = 0;
        int64_t skinned_live = 0, geo_bytes = 0;
        uint64_t geo_full = 0;
        int64_t live_blas = 0, live_triangles = 0;
    } stats_;
    bool told_corrupt_ = false;
};

}  // namespace rtcore
