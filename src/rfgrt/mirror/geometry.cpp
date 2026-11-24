#include "mirror/geometry.h"

#include <windows.h>
#include <d3d11.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "hooks/hooks.h"
#include "mirror/geometry_census.h"
#include "rt/link.h"
#include "scene_ring.h"

namespace geometry {
namespace {

using namespace rfgrt_scene;

bool g_census = false;
bool g_mirror = false;
bool g_terrain_chunks = true;  // Mirror.TerrainChunks

// --- who creates a mesh (initializeRenderGeometry's callers, research/docs/01 section 5) ---------------------------

enum class Creator { Mesh, GeoMod, Morph, MaterialMesh, Particles, StarField, Other };

Creator creator_of(uintptr_t rva) {
    struct Fn {
        uint32_t rva, size;
        Creator creator;
    };
    const Fn fns[] = {
        {rfg::rva::rl_mesh__init, rfg::size::rl_mesh__init, Creator::Mesh},
        {rfg::rva::rl_destroyable_model_instance__compile_cache_data, rfg::size::rl_destroyable_model_instance__compile_cache_data,
         Creator::GeoMod},
        {rfg::rva::rl_morph_container__create_render_data, rfg::size::rl_morph_container__create_render_data, Creator::Morph},
        {rfg::rva::keen__MaterialMesh__create, rfg::size::keen__MaterialMesh__create, Creator::MaterialMesh},
        {rfg::rva::rl_particle_renderer__create_buffers, rfg::size::rl_particle_renderer__create_buffers, Creator::Particles},
        {rfg::rva::rl_star_field__initialize, rfg::size::rl_star_field__initialize, Creator::StarField},
    };
    for (const Fn& f : fns)
        if (rva - f.rva < f.size) return f.creator;
    return Creator::Other;
}

const char* creator_name(Creator c, uintptr_t rva, char (&buf)[32]) {
    switch (c) {
        case Creator::Mesh: return "rl_mesh::init";
        case Creator::GeoMod: return "Geo-Mod compiled mesh";
        case Creator::Morph: return "morph";
        case Creator::MaterialMesh: return "keen::MaterialMesh";
        case Creator::Particles: return "particles";
        case Creator::StarField: return "star field";
        default: snprintf(buf, sizeof(buf), "rva %08X", static_cast<unsigned>(rva)); return buf;
    }
}

// --- the mirror's bookkeeping --------------------------------------------------------------------------------------

std::mutex g_ids_mutex;
std::unordered_map<const uint8_t*, uint32_t> g_ids;  // live meshes the core has, by their RenderGeometry
std::atomic<uint32_t> g_next_id{1};
std::unordered_set<uint32_t> g_chunked;  // height meshes sent chunk by chunk (geometry.h, terrain()); under g_ids_mutex

// Height meshes waiting for their chunk table, by RenderGeometry: their positions (the first 4 bytes of each vertex:
// height and grid cell) and indices, copied while initializeRenderGeometry ran. Under g_pending_mutex (taken after
// g_ids_mutex when both are).
struct PendingTerrain {
    uint32_t id = 0;
    std::vector<uint8_t> vertices, indices;
    uint32_t vertex_count = 0, index_count = 0, index_bytes = 0;
    bool strip = false;
    int tries = 0;  // snapshots that found it still in one piece
};
std::mutex g_pending_mutex;
std::unordered_map<const uint8_t*, PendingTerrain> g_pending;

constexpr int kFormats = 26;
struct Counters {
    std::atomic<uint64_t> sent{0}, vertices{0}, triangles{0}, submeshes{0}, bytes{0}, removed{0};
    std::atomic<uint64_t> skip_creator{0}, skip_skinned{0}, skip_no_data{0}, skip_no_position{0}, skip_layout{0},
        skip_primitive{0}, skip_empty{0}, skip_large{0}, skip_ring{0}, bad_ranges{0}, table_fallback{0};
    // rl_render_block's minimum/maximum_index against the indices in its range: absolute (min <= i <= max), relative to
    // minimum_index (i <= max - min, as with a base vertex), or neither
    std::atomic<uint64_t> blocks_absolute{0}, blocks_relative{0}, blocks_neither{0}, blocks_trivial{0};
    std::atomic<uint64_t> terrain{0}, skinned_sent{0};
    // height meshes: sent a submesh a chunk, sent whole for want of a chunk table, released before they could be sent
    std::atomic<uint64_t> terrain_chunked{0}, terrain_whole{0}, terrain_dropped{0};
    std::atomic<uint64_t> skip_position_format[kFormats]{};
    std::atomic<uint64_t> uv_format[kFormats]{};  // texture coordinates in a format not read (sent without them)
    std::atomic<uint64_t> with_uv{0};
} g_n;

const char* position_format_name(int f) {
    static const char* names[] = {"f32", "f32x2", "f32x3", "f32x4", "f16x4", "s16x4", "u16x4", "sn16x4", "un16x4",
                                  "un8x4", "u8x4", "sn8x4", "s8x4", "f16x2", "s16x2", "sn16x2", "u16x2", "un16x2",
                                  "sn10x3", "sn10x3w2", "sn11_11_10", "un8x4zyx", "d3dcolor", "sn16x3", "sn8x3", "?"};
    return names[f >= 0 && f < kFormats ? f : kFormats - 1];
}

// --- decoding ------------------------------------------------------------------------------------------------------

// A draw range of the source mesh (an rl_render_block, or the whole mesh).
struct Block {
    uint32_t start, count, material, min_index, max_index;
};
struct SourceSubmesh {
    uint32_t first_block, block_count;
    float offset[3];
};

// Everything a MeshAdd record is made from; the pointers are the game's CPU copies, valid while
// initializeRenderGeometry's caller waits for it to return.
struct Source {
    uint32_t id = 0;
    MeshSource source = kSourceOther;
    const char* name = nullptr;
    const uint8_t* vertices = nullptr;
    uint32_t vertex_count = 0, stride = 0, pos_offset = 0;
    bool terrain = false;  // int16 height-mesh positions (decoded in fill_mesh)
    bool skinned = false;  // bone indices (u8x4) and weights (un8x4) in the vertex: sent along (kMeshSkinned)
    uint32_t bone_offset = 0, weight_offset = 0;
    bool uv = false;       // TexCoord0 in the vertex: sent along (kMeshUV)
    uint32_t uv_offset = 0, uv_format = 0;
    const uint8_t* indices = nullptr;
    uint32_t index_bytes = 0;  // 0: no index buffer (vertex order)
    uint32_t index_count = 0;  // of the source (vertex_count without an index buffer)
    bool strip = false;
    std::vector<Block> blocks;
    std::vector<SourceSubmesh> source_submeshes;
    // planned output
    uint32_t out_index_bytes = 0, out_index_count = 0;
    std::vector<Submesh> submeshes;
    std::vector<Range> ranges;
    std::vector<uint32_t> range_block;  // the block each range comes from
    MeshLayout layout{};
};

float half_to_float(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (!man) {
            bits = sign;
        } else {  // subnormal: normalize
            uint32_t e = 113, m = man;
            while (!(m & 0x400u)) {
                m <<= 1;
                --e;
            }
            bits = sign | (e << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp + 112) << 23) | (man << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

uint32_t index_at(const Source& s, uint32_t k) {
    switch (s.index_bytes) {
        case 2: return reinterpret_cast<const uint16_t*>(s.indices)[k];
        case 4: return reinterpret_cast<const uint32_t*>(s.indices)[k];
        default: return k;
    }
}

// Triangles of the strip s[start, start + count): strips restart after a cut index (all bits set, as D3D11 does for
// strips) and degenerate triangles are dropped. Writes them to `out` when given; returns the number of indices, or
// -1 if an index is outside the vertex buffer.
int64_t strip_triangles(const Source& s, uint32_t start, uint32_t count, uint8_t* out) {
    const uint32_t cut = s.index_bytes == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    size_t written = 0;
    uint32_t run = 0, a = 0, b = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t c = index_at(s, start + i);
        if (s.index_bytes && c == cut) {
            run = 0;
            continue;
        }
        if (c >= s.vertex_count) return -1;
        if (run >= 2 && a != b && b != c && a != c) {
            if (out) {
                const uint32_t t[3] = {(run & 1) ? b : a, (run & 1) ? a : b, c};  // alternate winding
                for (int k = 0; k < 3; ++k) {
                    if (s.out_index_bytes == 2)
                        reinterpret_cast<uint16_t*>(out)[written + k] = uint16_t(t[k]);
                    else
                        reinterpret_cast<uint32_t*>(out)[written + k] = t[k];
                }
            }
            written += 3;
        }
        a = b;
        b = c;
        ++run;
    }
    return int64_t(written);
}

bool list_valid(const Source& s, uint32_t start, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i)
        if (index_at(s, start + i) >= s.vertex_count) return false;
    return true;
}

// rl_mesh's submesh table (rl_mesh_data_runtime, set before rl_mesh::init creates the geometry). Plain C with
// structured exception handling: a table that cannot be read is the whole mesh instead of a crash.
struct RawBlock {
    uint32_t start, count, material, min_index, max_index;
};
struct RawSubmesh {
    uint32_t first_block, block_count;
    float offset[3];
};
// How big the table is: its submeshes (the return), their render blocks in all (block_total); below 1 if unreadable.
int count_submesh_table(const uint8_t* mesh, int* block_total) {
    __try {
        if (!*(mesh + rfg::off::Mesh_dataSet)) return -1;
        const uint8_t* data = mesh + rfg::off::Mesh_data;
        const int n = *reinterpret_cast<const int32_t*>(data + rfg::off::MeshData_numSubMeshes);
        const uint8_t* table = *reinterpret_cast<const uint8_t* const*>(data + rfg::off::MeshData_subMeshes);
        if (n <= 0 || n > 65536 || !table) return -1;
        int64_t total = 0;
        for (int i = 0; i < n; ++i) {
            const int nb = *reinterpret_cast<const int32_t*>(table + size_t(i) * rfg::off::Submesh_size + rfg::off::Submesh_numRenderBlocks);
            if (nb < 0) return -1;
            total += nb;
        }
        if (total > (1 << 20)) return -1;
        *block_total = int(total);
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

int read_submesh_table(const uint8_t* mesh, RawSubmesh* subs, int max_subs, RawBlock* blocks, int max_blocks, int* block_total) {
    __try {
        if (!*(mesh + rfg::off::Mesh_dataSet)) return -1;
        const uint8_t* data = mesh + rfg::off::Mesh_data;
        const int n = *reinterpret_cast<const int32_t*>(data + rfg::off::MeshData_numSubMeshes);
        const uint8_t* table = *reinterpret_cast<const uint8_t* const*>(data + rfg::off::MeshData_subMeshes);
        if (n <= 0 || n > max_subs || !table) return -1;
        int total = 0;
        for (int i = 0; i < n; ++i) {
            const uint8_t* sm = table + size_t(i) * rfg::off::Submesh_size;
            const int nb = *reinterpret_cast<const int32_t*>(sm + rfg::off::Submesh_numRenderBlocks);
            if (nb < 0 || total + nb > max_blocks) return -1;
            // et_ptr_offset: an offset from the field's own address
            const uint8_t* field = sm + rfg::off::Submesh_renderBlocks;
            const uint8_t* rb = field + *reinterpret_cast<const int32_t*>(field);
            subs[i].first_block = uint32_t(total);
            subs[i].block_count = uint32_t(nb);
            memcpy(subs[i].offset, sm + rfg::off::Submesh_offsetTransform, 12);
            for (int j = 0; j < nb; ++j) {
                const uint8_t* b = rb + size_t(j) * rfg::off::RenderBlock_size;
                blocks[total].material = *reinterpret_cast<const uint16_t*>(b + rfg::off::RenderBlock_materialMapIdx);
                blocks[total].start = *reinterpret_cast<const uint32_t*>(b + rfg::off::RenderBlock_startIndex);
                blocks[total].count = *reinterpret_cast<const uint32_t*>(b + rfg::off::RenderBlock_numIndices);
                blocks[total].min_index = *reinterpret_cast<const uint32_t*>(b + rfg::off::RenderBlock_minIndex);
                blocks[total].max_index = *reinterpret_cast<const uint32_t*>(b + rfg::off::RenderBlock_maxIndex);
                ++total;
            }
        }
        *block_total = total;
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

// Reads what the record needs from the creation call. False (and a counter) for meshes the mirror leaves out.
bool describe(uint8_t* geometry, uint8_t* desc, uint8_t* init, Creator creator, const char* name, Source& s) {
    if (creator == Creator::Mesh)
        s.source = kSourceMesh;
    else if (creator == Creator::GeoMod)
        s.source = kSourceGeoMod;
    else
        return ++g_n.skip_creator, false;
    if (rfg::field<void*>(init, rfg::off::GeometryInit_pSkinningData)) return ++g_n.skip_skinned, false;
    auto* format = rfg::field<uint8_t*>(desc, rfg::off::GeometryDesc_pVertexFormat);
    const uint32_t vc = rfg::field<uint32_t>(desc, rfg::off::GeometryDesc_vertexCount);
    const uint32_t ic = rfg::field<uint32_t>(desc, rfg::off::GeometryDesc_indexCount);
    const int32_t primitive = rfg::field<int32_t>(desc, rfg::off::GeometryDesc_primitiveType);
    const int32_t index_format = rfg::field<int32_t>(desc, rfg::off::GeometryDesc_indexFormat);
    const uint32_t stream = rfg::field<uint32_t>(desc, rfg::off::GeometryDesc_streamIndex);
    auto* vdata = rfg::field<const uint8_t*>(init, rfg::off::GeometryInit_pVertexData);
    auto* idata = rfg::field<const uint8_t*>(init, rfg::off::GeometryInit_pIndexData);
    if (!format || !vc || !vdata) return ++g_n.skip_no_data, false;
    const uint32_t count = rfg::field<uint32_t>(format, rfg::off::VertexFormat_attributeCount);
    int pos = -1;
    for (uint32_t i = 0; i < count && i < 17; ++i) {
        const uint8_t* a = format + rfg::off::VertexFormat_attributes + i * rfg::off::VertexAttributeDescription_size;
        if (a[0] == 0 /* VertexAttributeId position */ && a[2] == stream) {
            pos = int(i);
            break;
        }
    }
    if (pos < 0) return ++g_n.skip_no_position, false;
    const uint8_t* a = format + rfg::off::VertexFormat_attributes + uint32_t(pos) * rfg::off::VertexAttributeDescription_size;
    s.pos_offset = rfg::field<uint32_t>(format, rfg::off::VertexFormat_attributeOffsets + uint32_t(pos) * 4);
    s.stride = stream < 6 ? rfg::field<uint32_t>(format, rfg::off::VertexFormat_streamStride + stream * 4) : 0;
    // Terrain height meshes: two int16 per vertex at the start of an 8-byte vertex, the height in 1/64 m and the
    // grid cell (x in the low byte, z in the high byte of the second, biased by 32768), as rl_terrain_depth_only's
    // vertex shader unpacks them.
    s.terrain = s.source == kSourceMesh && a[1] == 14 /* x16y16_sint */ && s.pos_offset == 0 && s.stride == 8;
    if (!s.terrain && a[1] != 2 /* x32y32z32_float */ && a[1] != 3 /* x32y32z32w32_float */) {
        ++g_n.skip_position_format[std::min<int>(a[1], kFormats - 1)];
        return false;
    }
    // GPU-skinned meshes (characters, the rfg-*_c / _mc materials): their bone indices and weights, for the core to
    // pose each instance with its palette.
    int bone = -1, weight = -1;
    for (uint32_t i = 0; i < count && i < 17; ++i) {
        const uint8_t* b = format + rfg::off::VertexFormat_attributes + i * rfg::off::VertexAttributeDescription_size;
        if (b[2] != stream) continue;
        if (b[0] == 15 /* VertexAttributeId blend indices */ && b[1] == 10 /* x8y8z8w8_uint */) bone = int(i);
        if (b[0] == 16 /* VertexAttributeId blend weights */ && b[1] == 9 /* x8y8z8w8_unorm */) weight = int(i);
    }
    if (!s.terrain && bone >= 0 && weight >= 0) {
        s.bone_offset = rfg::field<uint32_t>(format, rfg::off::VertexFormat_attributeOffsets + uint32_t(bone) * 4);
        s.weight_offset = rfg::field<uint32_t>(format, rfg::off::VertexFormat_attributeOffsets + uint32_t(weight) * 4);
        s.skinned = s.bone_offset + 4 <= s.stride && s.weight_offset + 4 <= s.stride;
    }
    if (s.terrain)
        s.source = kSourceTerrain;
    else if (s.stride < 12 || s.pos_offset + 12 > s.stride)
        return ++g_n.skip_layout, false;
    // The texture coordinates (TexCoord0): x16y16_sint, which the game's vertex shaders scale by 1/1024, else half or
    // float pairs; for the rays' hits to read the materials' textures.
    for (uint32_t i = 0; i < count && i < 17 && !s.terrain; ++i) {
        const uint8_t* b = format + rfg::off::VertexFormat_attributes + i * rfg::off::VertexAttributeDescription_size;
        if (b[2] != stream || b[0] != 7 /* VertexAttributeId TexCoord0 */) continue;
        const uint32_t off = rfg::field<uint32_t>(format, rfg::off::VertexFormat_attributeOffsets + i * 4);
        const uint32_t size = b[1] == 1 /* x32y32_float */ ? 8u : (b[1] == 13 /* x16y16_float */ || b[1] == 14 /* x16y16_sint */) ? 4u : 0u;
        if (size && off + size <= s.stride) {
            s.uv = true;
            s.uv_offset = off;
            s.uv_format = b[1];
        } else {
            ++g_n.uv_format[std::min<int>(b[1], kFormats - 1)];
        }
        break;
    }
    if (primitive != 0 /* TriangleList */ && primitive != 1 /* TriangleStrip */) return ++g_n.skip_primitive, false;
    s.strip = primitive == 1;
    s.index_bytes = index_format == 1 ? 2 : index_format == 2 ? 4 : 0;
    if (s.index_bytes && (!idata || !ic)) return ++g_n.skip_no_data, false;
    s.vertices = vdata;
    s.vertex_count = vc;
    s.indices = s.index_bytes ? idata : nullptr;
    s.index_count = s.index_bytes ? ic : vc;
    s.name = name;

    if (s.source == kSourceMesh) {
        // Sized to the table (a Wrecking Crew building has thousands of pieces, each a submesh): a fixed table of 2,048
        // made such a mesh one whole draw, its pieces beyond it left out and the whole building drawn at its first
        // piece's place. Allocated for the read only: the 32-bit game's address space is short.
        int want_blocks = 0;
        const uint8_t* mesh = geometry - rfg::off::Mesh_geometry;
        const int want = count_submesh_table(mesh, &want_blocks);
        std::vector<RawSubmesh> subs(size_t(want > 0 ? want : 1));
        std::vector<RawBlock> blocks(size_t(want_blocks > 0 ? want_blocks : 1));
        int total = 0;
        const int n = want > 0 ? read_submesh_table(mesh, subs.data(), int(subs.size()), blocks.data(), int(blocks.size()), &total) : -1;
        if (n > 0) {
            s.blocks.assign(reinterpret_cast<Block*>(blocks.data()), reinterpret_cast<Block*>(blocks.data()) + total);
            for (int i = 0; i < n; ++i) {
                SourceSubmesh sm{subs[i].first_block, subs[i].block_count, {subs[i].offset[0], subs[i].offset[1], subs[i].offset[2]}};
                s.source_submeshes.push_back(sm);
            }
            return true;
        }
        ++g_n.table_fallback;
    }
    // Geo-Mod's compiled meshes (and an rl_mesh without a readable table): the whole mesh, one draw, of a material
    // not known (0xFFFF: never taken for glass).
    s.blocks.push_back({0, s.index_count, 0xFFFFu, 0, 0});
    s.source_submeshes.push_back({0, 1, {0, 0, 0}});
    return true;
}

// Plans the converted index list and the ranges; drops draw ranges that are out of bounds.
void plan(Source& s) {
    s.out_index_bytes = s.index_bytes ? s.index_bytes : 4;
    uint64_t cursor = 0;
    for (const SourceSubmesh& sm : s.source_submeshes) {
        Submesh out{};
        out.first_range = uint32_t(s.ranges.size());
        memcpy(out.offset, sm.offset, sizeof(out.offset));
        for (uint32_t j = 0; j < sm.block_count; ++j) {
            const uint32_t bi = sm.first_block + j;
            const Block& b = s.blocks[bi];
            if (uint64_t(b.start) + b.count > s.index_count) {
                ++g_n.bad_ranges;
                continue;
            }
            if (b.max_index > b.min_index && b.count) {
                uint32_t lo = UINT32_MAX, hi = 0;
                const uint32_t cut = s.index_bytes == 2 ? 0xFFFFu : 0xFFFFFFFFu;
                for (uint32_t k = 0; k < b.count; ++k) {
                    const uint32_t v = index_at(s, b.start + k);
                    if (s.strip && s.index_bytes && v == cut) continue;
                    lo = v < lo ? v : lo;
                    hi = v > hi ? v : hi;
                }
                const bool absolute = lo >= b.min_index && hi <= b.max_index;
                const bool relative = hi <= b.max_index - b.min_index;
                if (absolute)
                    ++g_n.blocks_absolute;
                else if (relative)
                    ++g_n.blocks_relative;
                else
                    ++g_n.blocks_neither;
                static std::atomic<int> logged{0};
                if (!absolute && logged.fetch_add(1) < 4)
                    LOG("mirror: draw range of \"%s\": indices %u..%u, block says %u..%u (start %u, count %u, %u vertices)",
                        s.name ? s.name : "?", lo, hi, b.min_index, b.max_index, b.start, b.count, s.vertex_count);
            } else {
                ++g_n.blocks_trivial;
            }
            Range r{};
            r.material = b.material;
            if (s.strip) {
                const int64_t n = strip_triangles(s, b.start, b.count, nullptr);
                if (n < 0) {
                    ++g_n.bad_ranges;
                    continue;
                }
                r.first_index = uint32_t(cursor);
                r.index_count = uint32_t(n);
                cursor += uint64_t(n);
            } else {
                r.first_index = b.start;
                r.index_count = b.count - b.count % 3;
                if (!list_valid(s, r.first_index, r.index_count)) {
                    ++g_n.bad_ranges;
                    continue;
                }
            }
            if (!r.index_count) continue;
            s.ranges.push_back(r);
            s.range_block.push_back(bi);
            ++out.range_count;
        }
        s.submeshes.push_back(out);
    }
    s.out_index_count = s.strip ? uint32_t(cursor) : s.index_count;
    s.layout = mesh_layout(s.vertex_count, s.out_index_count, s.out_index_bytes, s.submeshes.size(), s.ranges.size(), s.skinned, s.uv);
}

// rl_terrain_depth_only's vertex shader: y = v0.x / 64; u = v0.y + 32768; x = u mod 256; z = u div 256.
void terrain_position(const uint8_t* vertex, float* p) {
    int16_t v[2];
    memcpy(v, vertex, sizeof(v));
    const uint32_t cell = uint32_t(int32_t(v[1]) + 32768);
    p[0] = float(cell & 0xFF);
    p[1] = float(v[0]) * (1.0f / 64.0f);
    p[2] = float(cell >> 8);
}

void fill_mesh(uint8_t* dst, void* ctx) {
    const Source& s = *static_cast<const Source*>(ctx);
    auto* m = reinterpret_cast<MeshAdd*>(dst);
    memset(m, 0, sizeof(*m));
    m->h.type = kMeshAdd;
    m->h.bytes = uint32_t(s.layout.bytes);
    m->id = s.id;
    m->source = s.source;
    m->vertex_count = s.vertex_count;
    m->index_count = s.out_index_count;
    m->index_bytes = s.out_index_bytes;
    m->submesh_count = uint32_t(s.submeshes.size());
    m->range_count = uint32_t(s.ranges.size());
    m->flags = (s.skinned ? kMeshSkinned : 0) | (s.uv ? kMeshUV : 0);
    if (s.name) strncpy_s(m->name, s.name, _TRUNCATE);
    auto* pos = reinterpret_cast<float*>(dst + s.layout.positions);
    if (s.terrain) {
        for (uint32_t i = 0; i < s.vertex_count; ++i) terrain_position(s.vertices + size_t(i) * s.stride, pos + size_t(i) * 3);
    } else {
        for (uint32_t i = 0; i < s.vertex_count; ++i) memcpy(pos + size_t(i) * 3, s.vertices + size_t(i) * s.stride + s.pos_offset, 12);
    }
    uint8_t* idx = dst + s.layout.indices;
    if (s.strip) {
        for (size_t r = 0; r < s.ranges.size(); ++r) {
            const Block& b = s.blocks[s.range_block[r]];
            strip_triangles(s, b.start, b.count, idx + size_t(s.ranges[r].first_index) * s.out_index_bytes);
        }
    } else if (s.index_bytes) {
        memcpy(idx, s.indices, size_t(s.index_count) * s.index_bytes);
    } else {
        for (uint32_t k = 0; k < s.index_count; ++k) reinterpret_cast<uint32_t*>(idx)[k] = k;
    }
    if (!s.submeshes.empty()) memcpy(dst + s.layout.submeshes, s.submeshes.data(), s.submeshes.size() * sizeof(Submesh));
    if (!s.ranges.empty()) memcpy(dst + s.layout.ranges, s.ranges.data(), s.ranges.size() * sizeof(Range));
    if (s.skinned) {
        uint8_t* skin = dst + s.layout.skin;
        for (uint32_t i = 0; i < s.vertex_count; ++i) {
            const uint8_t* v = s.vertices + size_t(i) * s.stride;
            memcpy(skin + size_t(i) * 8, v + s.bone_offset, 4);
            memcpy(skin + size_t(i) * 8 + 4, v + s.weight_offset, 4);
        }
    }
    if (s.uv) {
        auto* uv = reinterpret_cast<float*>(dst + s.layout.uv);
        for (uint32_t i = 0; i < s.vertex_count; ++i) {
            const uint8_t* v = s.vertices + size_t(i) * s.stride + s.uv_offset;
            float* o = uv + size_t(i) * 2;
            if (s.uv_format == 1) {
                memcpy(o, v, 8);
            } else if (s.uv_format == 14) {
                int16_t c[2];
                memcpy(c, v, 4);
                o[0] = float(c[0]) * (1.0f / 1024.0f);
                o[1] = float(c[1]) * (1.0f / 1024.0f);
            } else {
                uint16_t h[2];
                memcpy(h, v, 4);
                o[0] = half_to_float(h[0]);
                o[1] = half_to_float(h[1]);
            }
        }
    }
}

std::mutex g_samples_mutex;
std::unordered_map<uint32_t, SkinSample> g_skin_samples;  // a few skinned meshes, for the convention check

void keep_skin_sample(const Source& s) {
    std::lock_guard<std::mutex> lock(g_samples_mutex);
    if (g_skin_samples.size() >= 64) return;
    SkinSample& k = g_skin_samples[s.id];
    k.count = s.vertex_count < 16 ? s.vertex_count : 16;
    for (uint32_t i = 0; i < k.count; ++i) {
        // spread over the mesh, not only its first triangles
        const uint8_t* v = s.vertices + size_t(i * (s.vertex_count / k.count)) * s.stride;
        memcpy(k.pos[i], v + s.pos_offset, 12);
        memcpy(k.bone[i], v + s.bone_offset, 4);
        memcpy(k.weight[i], v + s.weight_offset, 4);
    }
}

void send_remove(uint32_t id) {
    MeshRemove r{};
    r.h.type = kMeshRemove;
    r.h.bytes = sizeof(MeshRemove);
    r.id = id;
    rtlink::core_link().ring_write(sizeof(r), [](uint8_t* dst, void* ctx) { memcpy(dst, ctx, sizeof(MeshRemove)); }, &r);
    ++g_n.removed;
}

void mirror_create(uint8_t* geometry, uint8_t* graphics, uint8_t* desc, uint8_t* init, const char* name, Creator creator) {
    uint32_t stale = 0;
    {
        std::lock_guard<std::mutex> lock(g_ids_mutex);
        auto it = g_ids.find(geometry);
        if (it != g_ids.end()) {
            stale = it->second;
            g_ids.erase(it);
        }
    }
    if (stale) send_remove(stale);  // made again without a release
    Source s;
    if (!describe(geometry, desc, init, creator, name, s)) return;
    plan(s);
    if (s.ranges.empty()) return void(++g_n.skip_empty);
    rtlink::Link& link = rtlink::core_link();
    if (!link.start(rfg::field<ID3D11Device*>(graphics, rfg::off::GraphicsSystem_pDevice))) return void(++g_n.skip_ring);
    if (s.layout.bytes > link.ring_max_record()) return void(++g_n.skip_large);
    if (s.terrain && g_terrain_chunks) {  // kept until its chunk table is there (terrain())
        PendingTerrain p;
        p.id = g_next_id.fetch_add(1);
        p.vertex_count = s.vertex_count;
        p.vertices.resize(size_t(s.vertex_count) * 4);
        for (uint32_t i = 0; i < s.vertex_count; ++i) memcpy(p.vertices.data() + size_t(i) * 4, s.vertices + size_t(i) * s.stride, 4);
        p.index_bytes = s.index_bytes;
        p.index_count = s.index_count;
        if (s.index_bytes) p.indices.assign(s.indices, s.indices + size_t(s.index_count) * s.index_bytes);
        p.strip = s.strip;
        {
            std::lock_guard<std::mutex> lock(g_ids_mutex);
            g_ids[geometry] = p.id;
        }
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending[geometry] = std::move(p);
        return;
    }
    s.id = g_next_id.fetch_add(1);
    if (!link.ring_write(s.layout.bytes, &fill_mesh, &s)) return void(++g_n.skip_ring);
    {
        std::lock_guard<std::mutex> lock(g_ids_mutex);
        g_ids[geometry] = s.id;
    }
    ++g_n.sent;
    if (s.terrain) {
        ++g_n.terrain;
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 3) {
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (uint32_t i = 0; i < s.vertex_count; ++i) {
                float p[3];
                terrain_position(s.vertices + size_t(i) * s.stride, p);
                for (int k = 0; k < 3; ++k) {
                    lo[k] = p[k] < lo[k] ? p[k] : lo[k];
                    hi[k] = p[k] > hi[k] ? p[k] : hi[k];
                }
            }
            LOG("mirror: terrain mesh %u (%s): %u vertices, %zu submeshes, grid (%g %g %g) .. (%g %g %g)", s.id,
                s.name ? s.name : "?", s.vertex_count, s.submeshes.size(), lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        }
    }
    if (s.skinned) {
        ++g_n.skinned_sent;
        keep_skin_sample(s);
    }
    if (s.uv) ++g_n.with_uv;
    g_n.vertices += s.vertex_count;
    uint64_t tris = 0;
    for (const Range& r : s.ranges) tris += r.index_count / 3;
    g_n.triangles += tris;
    g_n.submeshes += s.submeshes.size();
    g_n.bytes += s.layout.bytes;
}

// --- hooks ---------------------------------------------------------------------------------------------------------

bool(__cdecl* o_init_geometry)(uint8_t*, uint8_t*, uint8_t*, uint8_t*, const char*);
void(__cdecl* o_free_geometry)(uint8_t*, uint8_t*, void*);

bool __cdecl d_init_geometry(uint8_t* geometry, uint8_t* graphics, uint8_t* desc, uint8_t* init, const char* name) {
    const void* caller = hooks::caller(_ReturnAddress());  // where the game called from (a seam's caller, through one)
    const bool ok = o_init_geometry(geometry, graphics, desc, init, name);
    if (!ok || !geometry || !desc || !init) return ok;
    const uintptr_t from = reinterpret_cast<uintptr_t>(caller) - rfg::base();
    const Creator creator = creator_of(from);
    if (g_census) {
        char buf[32];
        geometry_census::record(geometry, creator_name(creator, from, buf));
    }
    if (g_mirror) mirror_create(geometry, graphics, desc, init, name, creator);
    return ok;
}

void __cdecl d_free_geometry(uint8_t* geometry, uint8_t* graphics, void* allocator) {
    if (g_mirror && geometry) {
        uint32_t id = 0;
        {
            std::lock_guard<std::mutex> lock(g_ids_mutex);
            auto it = g_ids.find(geometry);
            if (it != g_ids.end()) {
                id = it->second;
                g_ids.erase(it);
            }
        }
        if (id) send_remove(id);
        bool dropped = false;
        {
            std::lock_guard<std::mutex> lock(g_pending_mutex);
            dropped = g_pending.erase(geometry) != 0;
        }
        if (dropped) ++g_n.terrain_dropped;
        if (id) {
            std::lock_guard<std::mutex> lock(g_ids_mutex);
            g_chunked.erase(id);
        }
    }
    o_free_geometry(geometry, graphics, allocator);
}

void log_mirror(bool at_exit) {
    size_t live = 0;
    {
        std::unique_lock<std::mutex> lock(g_ids_mutex, std::defer_lock);
        if (at_exit ? lock.try_lock() : (lock.lock(), true)) live = g_ids.size();
    }
    LOG("mirror: %llu meshes sent (%llu of them terrain, %llu skinned; %llu vertices, %llu triangles, %llu submeshes, %.1f MB), "
        "%llu released, %zu live",
        static_cast<unsigned long long>(g_n.sent.load()), static_cast<unsigned long long>(g_n.terrain.load()),
        static_cast<unsigned long long>(g_n.skinned_sent.load()),
        static_cast<unsigned long long>(g_n.vertices.load()),
        static_cast<unsigned long long>(g_n.triangles.load()), static_cast<unsigned long long>(g_n.submeshes.load()),
        g_n.bytes.load() / 1048576.0, static_cast<unsigned long long>(g_n.removed.load()), live);
    std::string formats;
    for (int f = 0; f < kFormats; ++f) {
        if (const uint64_t n = g_n.skip_position_format[f].load()) {
            char item[48];
            snprintf(item, sizeof(item), " %s:%llu", position_format_name(f), static_cast<unsigned long long>(n));
            formats += item;
        }
    }
    LOG("mirror: left out: other creators %llu, CPU-skinned %llu, no CPU data %llu, no position %llu, position format%s, "
        "layout %llu, primitive %llu, no triangles %llu, too large %llu, not sent %llu; draw ranges out of bounds %llu, "
        "rl_mesh without a readable submesh table %llu",
        static_cast<unsigned long long>(g_n.skip_creator.load()), static_cast<unsigned long long>(g_n.skip_skinned.load()),
        static_cast<unsigned long long>(g_n.skip_no_data.load()), static_cast<unsigned long long>(g_n.skip_no_position.load()),
        formats.empty() ? " none" : formats.c_str(), static_cast<unsigned long long>(g_n.skip_layout.load()),
        static_cast<unsigned long long>(g_n.skip_primitive.load()), static_cast<unsigned long long>(g_n.skip_empty.load()),
        static_cast<unsigned long long>(g_n.skip_large.load()), static_cast<unsigned long long>(g_n.skip_ring.load()),
        static_cast<unsigned long long>(g_n.bad_ranges.load()), static_cast<unsigned long long>(g_n.table_fallback.load()));
    std::string uvf;
    for (int f = 0; f < kFormats; ++f) {
        if (const uint64_t n = g_n.uv_format[f].load()) {
            char item[48];
            snprintf(item, sizeof(item), " %s:%llu", position_format_name(f), static_cast<unsigned long long>(n));
            uvf += item;
        }
    }
    LOG("mirror: texture coordinates sent with %llu meshes; in a format not read:%s", static_cast<unsigned long long>(g_n.with_uv.load()),
        uvf.empty() ? " none" : uvf.c_str());
    LOG("mirror: terrain: %llu height meshes sent chunk by chunk, %llu whole (no chunk table), %llu released before their "
        "chunk table came", static_cast<unsigned long long>(g_n.terrain_chunked.load()),
        static_cast<unsigned long long>(g_n.terrain_whole.load()), static_cast<unsigned long long>(g_n.terrain_dropped.load()));
    LOG("mirror: draw ranges against their blocks' index bounds: absolute %llu, relative to the first vertex %llu, neither %llu, "
        "no bounds %llu",
        static_cast<unsigned long long>(g_n.blocks_absolute.load()), static_cast<unsigned long long>(g_n.blocks_relative.load()),
        static_cast<unsigned long long>(g_n.blocks_neither.load()), static_cast<unsigned long long>(g_n.blocks_trivial.load()));
    rtlink::Link& link = rtlink::core_link();
    if (!link.started()) return;
    const rtlink::Link::RingStats rs = link.ring_stats();
    const rfgrt_ipc::Block* b = link.block();
    LOG("mirror: ring %llu records, %.1f MB, waited %llu ms, dropped %llu | core: %lld meshes, %lld BLAS, %lld triangles, "
        "%.1f MB on the GPU, builds %.1f ms of GPU time",
        static_cast<unsigned long long>(rs.records), rs.bytes / 1048576.0, static_cast<unsigned long long>(rs.waits_ms),
        static_cast<unsigned long long>(rs.dropped), static_cast<long long>(rfgrt_ipc::load64(&b->scene_meshes)),
        static_cast<long long>(rfgrt_ipc::load64(&b->scene_blas)), static_cast<long long>(rfgrt_ipc::load64(&b->scene_triangles)),
        rfgrt_ipc::load64(&b->scene_gpu_bytes) / 1048576.0, rfgrt_ipc::load64(&b->scene_build_us) / 1000.0);
}

void on_frame(int) {
    static int64_t last = 0;
    const int64_t now = session::qpc();
    if (!last) last = now;
    if (session::ticks_to_ms(now - last) < 15000) return;
    last = now;
    log_mirror(false);
}

}  // namespace

bool install(const Config& cfg) {
    g_census = cfg.geometry_census;
    g_mirror = cfg.host_enabled && cfg.mirror_enabled;
    g_terrain_chunks = cfg.terrain_chunks;
    if (!g_census && !g_mirror) return true;
    if (g_census) geometry_census::enable();
    if (g_mirror) session::add_frame_callback(&on_frame);
    bool ok = hooks::create(rfg::rva::keen__graphics__initializeRenderGeometry, &d_init_geometry, &o_init_geometry,
                            "keen::graphics::initializeRenderGeometry");
    if (g_mirror)
        ok &= hooks::create(rfg::rva::keen__graphics__freeRenderGeometry, &d_free_geometry, &o_free_geometry,
                            "keen::graphics::freeRenderGeometry");
    LOG("geometry: hooks installed (census %s, scene mirror %s)", g_census ? "on" : "off", g_mirror ? "on" : "off");
    return ok;
}

void report(bool at_exit) {
    if (g_census) geometry_census::report(at_exit);
    if (g_mirror) log_mirror(at_exit);
}

std::unique_lock<std::mutex> lock_ids() { return std::unique_lock<std::mutex>(g_ids_mutex); }

uint32_t mesh_id(const std::unique_lock<std::mutex>&, const uint8_t* render_geometry) {
    auto it = g_ids.find(render_geometry);
    return it == g_ids.end() ? 0 : it->second;
}

Terrain terrain(const std::unique_lock<std::mutex>&, uint32_t mesh_id, uint8_t* mesh) {
    if (g_chunked.count(mesh_id)) return Terrain::Chunks;
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    auto it = g_pending.find(mesh + rfg::off::Mesh_geometry);
    if (it == g_pending.end()) return Terrain::Whole;
    PendingTerrain& p = it->second;
    int want_blocks = 0;
    const int want = count_submesh_table(mesh, &want_blocks);
    // still in one piece: wait for the table, up to ~10 s of snapshots with its instances in view, then send it whole
    if (want <= 1 && ++p.tries < 600) return Terrain::Pending;
    Source s;
    s.id = p.id;
    s.source = kSourceTerrain;
    s.terrain = true;
    s.vertices = p.vertices.data();
    s.vertex_count = p.vertex_count;
    s.stride = 4;
    s.pos_offset = 0;
    s.indices = p.index_bytes ? p.indices.data() : nullptr;
    s.index_bytes = p.index_bytes;
    s.index_count = p.index_count;
    s.strip = p.strip;
    bool chunks = false;
    if (want > 1) {
        std::vector<RawSubmesh> subs(static_cast<size_t>(want));
        std::vector<RawBlock> blocks(size_t(want_blocks > 0 ? want_blocks : 1));
        int total = 0;
        const int n = read_submesh_table(mesh, subs.data(), int(subs.size()), blocks.data(), int(blocks.size()), &total);
        if (n > 1) {
            s.blocks.assign(reinterpret_cast<Block*>(blocks.data()), reinterpret_cast<Block*>(blocks.data()) + total);
            for (int i = 0; i < n; ++i) {
                SourceSubmesh sm{subs[i].first_block, subs[i].block_count, {subs[i].offset[0], subs[i].offset[1], subs[i].offset[2]}};
                s.source_submeshes.push_back(sm);
            }
            chunks = true;
        }
    }
    if (!chunks) {
        s.blocks.push_back({0, s.index_count, 0xFFFFu, 0, 0});
        s.source_submeshes.push_back({0, 1, {0, 0, 0}});
    }
    plan(s);
    rtlink::Link& link = rtlink::core_link();
    if (s.ranges.empty() || s.layout.bytes > link.ring_max_record()) {
        ++g_n.skip_empty;
        g_pending.erase(it);
        return Terrain::Pending;  // not sendable: its instances stay out
    }
    if (!link.ring_write(s.layout.bytes, &fill_mesh, &s, 0)) return Terrain::Pending;  // the ring is full: next snapshot
    ++g_n.sent;
    ++g_n.terrain;
    g_n.vertices += s.vertex_count;
    uint64_t tris = 0;
    for (const Range& r : s.ranges) tris += r.index_count / 3;
    g_n.triangles += tris;
    g_n.submeshes += s.submeshes.size();
    g_n.bytes += s.layout.bytes;
    g_pending.erase(it);
    if (!chunks) {
        ++g_n.terrain_whole;
        return Terrain::Whole;
    }
    ++g_n.terrain_chunked;
    g_chunked.insert(mesh_id);
    return Terrain::Chunks;
}

bool skin_sample(uint32_t mesh_id, SkinSample* out) {
    std::lock_guard<std::mutex> lock(g_samples_mutex);
    auto it = g_skin_samples.find(mesh_id);
    if (it == g_skin_samples.end() || !it->second.count) return false;
    *out = it->second;
    return true;
}

}  // namespace geometry
