#pragma once
// The scene ring: what the plugin tells the core about the game's scene, as records in a ring of shared memory that
// follows the Block (rfgrt_ipc.h). One writer at a time (the plugin serializes its threads), one reader (the core).
// Records are 8-byte aligned and never wrap: one that does not fit before the end of the ring is preceded by a pad
// record to the end, and starts at offset 0.
#include <cstdint>

#include "rfgrt_ipc.h"

namespace rfgrt_scene {

enum RecordType : uint32_t {
    kPad = 0,  // filler to the end of the ring
    kMeshAdd = 1,
    kMeshRemove = 2,
    kMeshMaterials = 3,
    kTextureAdd = 4,
};

struct RecordHeader {
    uint32_t type;
    uint32_t bytes;  // the whole record, this header included; a multiple of 8
};

// Who made the mesh (the creator of its keen::RenderGeometry).
enum MeshSource : uint32_t {
    kSourceMesh = 1,    // rl_mesh: static meshes, characters, vehicles, buildings before they break
    kSourceGeoMod = 2,  // rl_destroyable_model_instance::compile_cache_data: a building as Geo-Mod recompiled it
    kSourceOther = 3,
    kSourceTerrain = 4, // a terrain height mesh (rl_mesh with int16 positions, decoded as rl_terrain_depth_only's VS does)
};

// A mesh the renderer created (keen::RenderGeometry), decoded by the plugin: positions as float3 in the mesh's own
// space, triangle lists only (strips converted). The record continues, each part 8-byte aligned (mesh_layout), with
//   float    positions[3 * vertex_count]
//   uint16_t or uint32_t indices[index_count]   (index_bytes 2 or 4)
//   Submesh  submeshes[submesh_count]
//   Range    ranges[range_count]
//   uint8_t  skin[8 * vertex_count]              (kMeshSkinned: each vertex's 4 bone indices, then its 4 weights, unorm)
//   float    uv[2 * vertex_count]                (kMeshUV: each vertex's texture coordinates, TexCoord0 as the game's
//                                                 vertex shaders scale it; the materials' tiling not applied)
constexpr uint32_t kMeshSkinned = 1;  // MeshAdd::flags: skinned on the GPU by the game (rl_skin_instance's palette)
constexpr uint32_t kMeshUV = 2;       // MeshAdd::flags: texture coordinates follow
struct MeshAdd {
    RecordHeader h;
    uint32_t id;  // the plugin's number for the mesh, never reused in a session
    uint32_t source;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t index_bytes;
    uint32_t submesh_count;
    uint32_t range_count;
    uint32_t flags;
    char name[64];
};
// What an instance draws (rl_mesh_instance names one submesh): its ranges, with `offset` added to the positions
// (rl_submesh_data::offset_transform, applied before the instance's transform).
struct Submesh {
    uint32_t first_range;
    uint32_t range_count;
    float offset[3];
    uint32_t pad;
};
// One draw (rl_render_block): a run of whole triangles in the converted index list.
struct Range {
    uint32_t first_index;
    uint32_t index_count;  // a multiple of 3
    uint32_t material;     // rl_render_block::material_map_idx
    uint32_t pad;
};
struct MeshRemove {
    RecordHeader h;
    uint32_t id;
    uint32_t pad;
};
// What a mesh's material map entries draw, for the rays, from the map of the first instance of it the plugin saw (a
// mesh's instances share theirs, as a rule): which are glass (bit i for entry i), and each entry's surface (`count`
// MaterialEntry records follow, entry i the map's entry i). Sent once a mesh, once the textures it names were sent.
constexpr uint32_t kMaxMaterialEntries = 32;
struct MeshMaterials {
    RecordHeader h;
    uint32_t mesh_id;
    uint32_t clear;        // transparent materials (rl_material_data::mat_flags bit 0) but rfg-eosglass's: rays pass
    uint32_t translucent;  // rfg-eosglass's: a share of the light passes
    uint32_t count;        // MaterialEntry records that follow (at most kMaxMaterialEntries)
};
// A material as the rays' hits see it: its diffuse map and the constants the game's shaders multiply it by.
constexpr uint32_t kNoTexture = 0xFFFFFFFFu;
struct MaterialEntry {
    uint32_t texture;          // the diffuse map: the core's texture id (TextureAdd), or kNoTexture
    float tiling[2];           // Diffuse_Map_TilingU, _TilingV (1 without)
    float color[3];            // Diffuse_Color's rgb (1 without)
    float self_illumination;   // Self_Illumination: the lit colour goes from albedo x light to albedo x 4 (0 without)
    uint32_t flags;            // none yet
};

// A material's texture for the rays (mirror/textures.cpp): a copy of the game's, its mips from at most 256 texels down,
// shared with the core (an NT handle duplicated into its process: the core closes it), for the core's table at `id`
// (below kMaxTextures, never reused in a session). Sent once the core has queued the frame whose fence A follows the
// copy: a frame it queues after reading this record waits for a later fence value, so it never reads the copy unfinished.
constexpr uint32_t kMaxTextures = 4096;
struct TextureAdd {
    RecordHeader h;
    uint32_t id;
    uint32_t format;  // DXGI_FORMAT of the game's view of it (the core views it the same way)
    uint64_t handle;
    uint32_t width, height, mips, pad;
};

constexpr uint64_t align8(uint64_t n) { return (n + 7) & ~uint64_t(7); }

struct MeshLayout {
    uint64_t positions, indices, submeshes, ranges, skin, uv, bytes;  // offsets from the record's start, and its size
};
inline MeshLayout mesh_layout(uint64_t vertex_count, uint64_t index_count, uint64_t index_bytes, uint64_t submesh_count,
                              uint64_t range_count, bool skinned = false, bool uv = false) {
    MeshLayout l{};
    l.positions = sizeof(MeshAdd);
    l.indices = l.positions + align8(vertex_count * 12);
    l.submeshes = l.indices + align8(index_count * index_bytes);
    l.ranges = l.submeshes + align8(submesh_count * sizeof(Submesh));
    l.skin = l.ranges + align8(range_count * sizeof(Range));
    l.uv = l.skin + (skinned ? align8(vertex_count * 8) : 0);
    l.bytes = l.uv + (uv ? align8(vertex_count * 8) : 0);
    return l;
}

// The plugin's side. Not thread-safe: the caller serializes.
class RingWriter {
public:
    void attach(rfgrt_ipc::Block* b, uint8_t* ring) {
        b_ = b;
        ring_ = ring;
        write_ = rfgrt_ipc::load64(&b->ring_write);
    }
    // The largest record the ring takes.
    uint64_t max_record() const { return b_ ? b_->ring_bytes / 2 : 0; }
    // `bytes` (a multiple of 8, at most max_record()) of contiguous space at the write position, or nullptr when the
    // core has not freed that much yet. The record becomes visible to the core at commit().
    uint8_t* try_reserve(uint64_t bytes) {
        if (!b_ || bytes > max_record() || bytes < sizeof(RecordHeader) || (bytes & 7)) return nullptr;
        const uint64_t size = b_->ring_bytes;
        const uint64_t off = uint64_t(write_) & (size - 1);
        const uint64_t tail = size - off;
        const uint64_t need = tail < bytes ? tail + bytes : bytes;
        const int64_t read = rfgrt_ipc::load64(&b_->ring_read);
        if (uint64_t(write_ - read) + need > size) return nullptr;
        if (tail < bytes) {
            auto* pad = reinterpret_cast<RecordHeader*>(ring_ + off);
            pad->type = kPad;
            pad->bytes = uint32_t(tail);
            pad_ = tail;
            at_ = 0;
        } else {
            pad_ = 0;
            at_ = off;
        }
        reserved_ = bytes;
        return ring_ + at_;
    }
    void commit() {
        write_ += int64_t(pad_ + reserved_);
        pad_ = reserved_ = 0;
        rfgrt_ipc::store64(&b_->ring_write, write_);
    }

private:
    rfgrt_ipc::Block* b_ = nullptr;
    uint8_t* ring_ = nullptr;
    int64_t write_ = 0;
    uint64_t pad_ = 0, reserved_ = 0, at_ = 0;
};

// The core's side.
class RingReader {
public:
    void attach(rfgrt_ipc::Block* b, const uint8_t* ring) {
        b_ = b;
        ring_ = ring;
        read_ = rfgrt_ipc::load64(&b->ring_read);
    }
    // The next complete record, or nullptr. Pads are skipped. A malformed header stops the reader for good (corrupt()).
    const RecordHeader* peek() {
        if (!b_ || corrupt_) return nullptr;
        const int64_t write = rfgrt_ipc::load64(&b_->ring_write);
        const uint64_t size = b_->ring_bytes;
        while (read_ < write) {
            const uint64_t off = uint64_t(read_) & (size - 1);
            const auto* h = reinterpret_cast<const RecordHeader*>(ring_ + off);
            if (h->bytes < sizeof(RecordHeader) || (h->bytes & 7) || h->bytes > size - off ||
                int64_t(h->bytes) > write - read_) {
                corrupt_ = true;
                return nullptr;
            }
            if (h->type != kPad) {
                current_ = h->bytes;
                return h;
            }
            read_ += h->bytes;
            rfgrt_ipc::store64(&b_->ring_read, read_);
        }
        return nullptr;
    }
    // Frees the record peek() returned.
    void consume() {
        read_ += int64_t(current_);
        current_ = 0;
        rfgrt_ipc::store64(&b_->ring_read, read_);
    }
    bool corrupt() const { return corrupt_; }

private:
    rfgrt_ipc::Block* b_ = nullptr;
    const uint8_t* ring_ = nullptr;
    int64_t read_ = 0;
    uint64_t current_ = 0;
    bool corrupt_ = false;
};

}  // namespace rfgrt_scene
