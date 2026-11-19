#include "mirror/geometry_census.h"

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"

namespace geometry_census {
namespace {

// keen::VertexAttributeId and keen::VertexAttributeFormat (research/docs/01 section 5; enum values from the PDB).
const char* attribute_name(int id) {
    static const char* names[] = {"pos", "nrm", "tan", "bitan", "col0", "col1", "col2", "uv0", "uv1", "uv2",
                                  "uv3", "uv4", "uv5", "uv6", "uv7", "bone", "weight"};
    return id >= 0 && id < 17 ? names[id] : "?";
}
const char* format_name(int f) {
    static const char* names[] = {"f32", "f32x2", "f32x3", "f32x4", "f16x4", "s16x4", "u16x4", "sn16x4", "un16x4",
                                  "un8x4", "u8x4", "sn8x4", "s8x4", "f16x2", "s16x2", "sn16x2", "u16x2", "un16x2",
                                  "sn10x3", "sn10x3w2", "sn11_11_10", "un8x4zyx", "d3dcolor", "sn16x3", "sn8x3"};
    return f >= 0 && f < 25 ? names[f] : "?";
}

struct Key {
    std::string caller, layout;
    bool operator<(const Key& o) const { return caller != o.caller ? caller < o.caller : layout < o.layout; }
};
struct Count {
    uint64_t meshes = 0, vertices = 0, indices = 0, vb_bytes = 0;
    int vb_usage = -1, ib_usage = -1;
    bool shared_ok = false;
};

std::mutex g_mutex;
std::map<Key, Count> g_counts, g_total;
uint64_t g_since_report = 0;

std::string layout_of(uint8_t* format) {
    if (!format) return "no format";
    const uint32_t count = rfg::field<uint32_t>(format, rfg::off::VertexFormat_attributeCount);
    std::string s;
    for (uint32_t i = 0; i < count && i < 17; ++i) {
        uint8_t* a = format + rfg::off::VertexFormat_attributes + i * rfg::off::VertexAttributeDescription_size;
        const uint32_t offset = rfg::field<uint32_t>(format, rfg::off::VertexFormat_attributeOffsets + i * 4);
        char item[48];
        snprintf(item, sizeof(item), "%s%s:%s@%u%s", s.empty() ? "" : " ", attribute_name(a[0]), format_name(a[1]), offset,
                 a[2] ? (a[3] ? "/inst" : "/s1") : "");
        s += item;
    }
    char stride[32];
    snprintf(stride, sizeof(stride), " | stride %u", rfg::field<uint32_t>(format, rfg::off::VertexFormat_streamStride));
    return s + stride;
}

int usage_of(ID3D11Buffer* b, uint32_t* bytes) {
    if (!b) return -1;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    if (bytes) *bytes = d.ByteWidth;
    return int(d.Usage);
}

}  // namespace

void record(uint8_t* geometry, const char* creator) {
    Key k{creator, layout_of(rfg::field<uint8_t*>(geometry, rfg::off::RenderGeometry_pVertexFormat))};
    uint32_t vb_bytes = 0;
    const int vb_usage = usage_of(rfg::field<ID3D11Buffer*>(geometry, rfg::off::RenderGeometry_pVertexBuffer), &vb_bytes);
    const int ib_usage = usage_of(rfg::field<ID3D11Buffer*>(geometry, rfg::off::RenderGeometry_pIndexBuffer), nullptr);
    std::lock_guard<std::mutex> lock(g_mutex);
    Count& c = g_counts[k];
    ++c.meshes;
    c.vertices += rfg::field<uint32_t>(geometry, rfg::off::RenderGeometry_vertexCount);
    c.indices += rfg::field<uint32_t>(geometry, rfg::off::RenderGeometry_indexCount);
    c.vb_bytes += vb_bytes;
    c.vb_usage = vb_usage;
    c.ib_usage = ib_usage;
    ++g_since_report;
}

namespace {

const char* usage_name(int u) {
    switch (u) {
        case D3D11_USAGE_DEFAULT: return "default";
        case D3D11_USAGE_IMMUTABLE: return "immutable";
        case D3D11_USAGE_DYNAMIC: return "dynamic";
        case D3D11_USAGE_STAGING: return "staging";
        default: return "none";
    }
}

void dump(const std::map<Key, Count>& counts, const char* title) {
    uint64_t meshes = 0, vertices = 0, bytes = 0;
    for (const auto& [k, c] : counts) meshes += c.meshes, vertices += c.vertices, bytes += c.vb_bytes;
    LOG("geometry census, %s: %llu meshes, %llu vertices, %.1f MB of vertex buffers", title,
        static_cast<unsigned long long>(meshes), static_cast<unsigned long long>(vertices), bytes / 1048576.0);
    for (const auto& [k, c] : counts)
        LOG("  %-22s %6llu meshes %9llu verts %9llu idx  vb %-9s ib %-9s %s", k.caller.c_str(),
            static_cast<unsigned long long>(c.meshes), static_cast<unsigned long long>(c.vertices),
            static_cast<unsigned long long>(c.indices), usage_name(c.vb_usage), usage_name(c.ib_usage), k.layout.c_str());
}

// Logs what was made since the last report and adds it to the session's total (g_mutex held).
void report_new() {
    if (g_since_report == 0) return;
    dump(g_counts, "new since the last report");
    for (const auto& [k, c] : g_counts) {
        Count& t = g_total[k];
        t.meshes += c.meshes, t.vertices += c.vertices, t.indices += c.indices, t.vb_bytes += c.vb_bytes;
        t.vb_usage = c.vb_usage, t.ib_usage = c.ib_usage;
    }
    g_counts.clear();
    g_since_report = 0;
}

void on_frame(int) {
    static int64_t last = 0;
    const int64_t now = session::qpc();
    if (!last) last = now;
    if (session::ticks_to_ms(now - last) < 15000) return;
    last = now;
    std::lock_guard<std::mutex> lock(g_mutex);
    report_new();
}

}  // namespace

void enable() { session::add_frame_callback(&on_frame); }

void report(bool at_exit) {
    std::unique_lock<std::mutex> lock(g_mutex, std::defer_lock);
    if (at_exit) {
        if (!lock.try_lock()) return;  // process teardown: never wait on a lock
    } else {
        lock.lock();
    }
    report_new();
    dump(g_total, "session total");
}

}  // namespace geometry_census
