#include "mirror/instances.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "frame_scene.h"
#include "game/rfg.h"
#include "hooks/hooks.h"
#include "mirror/geometry.h"
#include "mirror/textures.h"
#include "render/gi.h"
#include "render/lights.h"
#include "rt/link.h"
#include "scene_ring.h"

namespace instances {
namespace {

using rfgrt_frame::Instance;
using rfgrt_frame::Transform;

std::mutex g_mutex;
std::unordered_map<uint8_t*, uint8_t*> g_instances;  // render instance -> its scene

// Geo-Mod buildings are thousands of pieces that rarely change: their records are kept per building and remade only
// when the building's world matrix, the render offset, its mesh or any of its pieces' records changed (a piece broke
// off, the building was recompiled). Under g_mutex.
struct BuildingCache {
    std::vector<uint8_t> key;  // world matrix (48 bytes), render offset (12), mesh id, pass flags, glass (12), the pieces' records
    std::vector<rfgrt_frame::Instance> records;
};
std::unordered_map<const uint8_t*, BuildingCache> g_buildings;

// Glass: which of an instance's materials are transparent (rl_material_data::mat_flags bit 0, as
// rl_material::get_is_transparent reads it), bit i for its material map's entry i (the first 32); the core lets rays
// through what they draw (tlas.cpp). Kept per material map, checked against its entries' array and count each time and
// asked again every 256 scenes (another map made where a freed one was). On the render thread only. Where the flag
// cannot tell (an object made to fade in and out has every material flagged so: the vehicles), the glass is what draws
// with the plain glass's shaders (rfg-glass, rfg-glass-etched: the windows).
struct GlassEntry {
    const void* materials = nullptr;
    uint32_t num = 0, mask = 0;
    uint32_t clear = 0, translucent = 0;  // every entry (the first 32): clear glass, translucent glass
    int64_t scene = -1000000;
    bool vehicle = false;  // read as a vehicle's
};
std::unordered_map<const uint8_t*, GlassEntry> g_glass;
int64_t g_glass_scene = 0;
uint64_t g_glass_reads = 0;  // maps read this scene
uint64_t g_glass_faders = 0;  // maps all transparent, taken for objects that fade (all along; the vehicles' not counted)
uint64_t g_glass_vehicle_maps = 0, g_glass_by_shader = 0;  // vehicles' maps read; those and the faders' with glass by shader
bool g_glass_on = true;      // Mirror.Glass
std::vector<double> g_vehicle_glass_at;  // Test.VehicleGlassAt
size_t g_vehicle_glass_next = 0;
bool g_vehicle_glass = true;  // the vehicles' windows glass (Test.VehicleGlassAt switches them solid and back)
int g_probe_key = 0;         // Test.ProbeKey
double g_probe_at = 0;       // Test.ProbeAt
int g_probe_at_frame = -1;   // Test.ProbeAtFrame
float g_probe_x = 0.5f, g_probe_y = 0.5f;  // Test.ProbePixel
struct PlannedProbe {
    int64_t frame;
    float x, y;
    bool fired;
};
std::vector<PlannedProbe> g_probes;  // Test.Probes, in frame order

// The map's entries (rl_material_map_data_runtime); false if it cannot be read.
bool map_entries(const uint8_t* map, const uint32_t** materials, uint32_t* num) {
    __try {
        const uint8_t* data = map + rfg::off::MaterialMap_data;
        *materials = *reinterpret_cast<const uint32_t* const*>(data + rfg::off::MaterialMapData_materials);
        *num = *reinterpret_cast<const uint32_t*>(data + rfg::off::MaterialMapData_num);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Translucent glass: the patterned glass of the multiplayer maps (rfg-eosglass), whose materials are not flagged
// transparent, lets a share of the rays through (the core's rt_query.hlsli): its light comes through partly. Its
// materials are those drawing with that shader, whose handle the shader library gives for the name; a name it does not
// know gives what a made-up one gives, and then none is taken.
uint32_t g_eosglass = 0xFFFFFFFFu;
bool g_eosglass_looked = false, g_eosglass_ok = false;
uint64_t g_translucent_maps = 0;

// Telemetry: the shaders the transparent and translucent materials draw with, by name (which make a map's windows):
// these names' handles looked up once, the others shown as numbers.
const char* const kShaderNames[] = {
    "rfg-glass", "rfg-glass-etched", "rfg-eosglass", "rfg-standard", "rfg-nodecal", "rfg-singledecal", "rfg-nospec",
    "rfg-nospecnodecal", "rfg-nospecsingledecal", "rfg-animated", "rfg-mp-animated", "rfg-colorshift", "rfg-lightsource",
    "rfg-vfx-glow", "rfg-vfx-glow2", "rfg-vfx-light-cone", "rfg-vfx-anim-fade", "rfg-vfx-mask-scroll-a",
    "rfg-vfx-mask-scroll-b", "rfg-vfx-shield", "rfg-vfx-volfog", "rfg-gps_chevron", "rfg-decal-glasscrack",
    "rfg-decal-bloodsplat", "rfg-stitchpiece", "rfg-metal", "rfg-brdf", "rfg-edf-vhc", "rfg-ed-oasisroof", "rfg-radiance",
    "rfg-heat-map", "rfg-orbital", "rfg-mp-nodecal_rimlight", "rfg-nodecal_rimlight", "rfg-blend-std-metal", "rfg-e-metal",
    "rfg-ed-metal", "rfg-brdf-env", "rfg-skin", "rfg-terrainblend", "rfg-brdf-cloth_rimlight", "rfg-brdf-cloth",
    "rfg-skinshader"};
constexpr size_t kShaderNameCount = sizeof(kShaderNames) / sizeof(kShaderNames[0]);
uint32_t g_shader_handles[kShaderNameCount];  // [0] rfg-glass, [1] rfg-glass-etched: also the faders' glass (glass_of)
constexpr uint32_t kShaderUnread = 0xFFFFFFFDu;  // an entry whose material was not read (no handle is)
std::mutex g_glass_shader_mutex;
std::unordered_map<uint32_t, uint64_t> g_glass_shaders;  // shader handle -> transparent or translucent entries read
std::unordered_map<uint32_t, uint64_t> g_fader_shaders;  // shader handle -> the vehicles' and the faders' entries read
std::unordered_map<uint32_t, uint64_t> g_skinned_shaders;  // shader handle -> the skinned meshes' (characters') entries
std::unordered_map<const uint8_t*, const void*> g_skinned_maps;  // their maps read (-> their entries' array)

void lookup_eosglass() {
    if (g_eosglass_looked) return;
    g_eosglass_looked = true;
    using Instance = uint8_t*(__cdecl*)();
    using Lookup = uint32_t(__thiscall*)(uint8_t*, const char*);
    uint32_t none = 0xFFFFFFFFu, standard = 0xFFFFFFFFu;
    for (size_t i = 0; i < kShaderNameCount; ++i) g_shader_handles[i] = 0xFFFFFFFFu;
    __try {
        uint8_t* lib = rfg::fn<Instance>(rfg::rva::ShaderLibrary_instance)();
        if (lib) {
            const Lookup lookup = rfg::fn<Lookup>(rfg::rva::ShaderLibrary_lookup);
            none = lookup(lib, "rfgrt-no-such-shader");
            standard = lookup(lib, "rfg-standard");
            g_eosglass = lookup(lib, "rfg-eosglass");
            for (size_t i = 0; i < kShaderNameCount; ++i) g_shader_handles[i] = lookup(lib, kShaderNames[i]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_eosglass = none;
    }
    for (size_t i = 0; i < kShaderNameCount; ++i)
        if (g_shader_handles[i] == none) g_shader_handles[i] = 0xFFFFFFFEu;  // not in this library: matches nothing
    g_eosglass_ok = g_eosglass != none && g_eosglass != standard;
    LOG("instances: the shader library's handles: rfg-eosglass %08X, rfg-standard %08X, an unknown name %08X: %s", g_eosglass,
        standard, none, g_eosglass_ok ? "rfg-eosglass's materials are translucent glass (rays partly through)" : "no translucent glass");
}

// The map's entries (the first 31) drawing transparent materials; `translucent` those drawing rfg-eosglass's; `shaders`
// (31) each such entry's shader handle.
uint32_t transparent_entries(const uint32_t* materials, uint32_t num, uint32_t* translucent, uint32_t* shaders) {
    using Resolve = uint8_t*(__cdecl*)(uint32_t handle, bool);
    const Resolve resolve = rfg::fn<Resolve>(rfg::rva::resolve_material_handle);
    *translucent = 0;
    __try {
        uint32_t mask = 0;
        for (uint32_t i = 0; i < num && i < 31; ++i) {
            const uint8_t* m = resolve(materials[i], false);
            if (!m) continue;
            const uint8_t* d = *reinterpret_cast<const uint8_t* const*>(m + rfg::off::Material_data);
            if (!d) continue;
            const uint32_t shader = *reinterpret_cast<const uint32_t*>(d + rfg::off::MaterialData_shader);
            if (*reinterpret_cast<const uint32_t*>(d + rfg::off::MaterialData_flags) & 1u)
                mask |= 1u << i;
            else if (g_eosglass_ok && shader == g_eosglass)
                *translucent |= 1u << i;
            shaders[i] = shader;
        }
        return mask;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *translucent = 0;
        return 0;
    }
}

std::string shaders_described(const std::unordered_map<uint32_t, uint64_t>& counts) {
    std::lock_guard<std::mutex> lock(g_glass_shader_mutex);
    std::string s;
    for (const auto& [handle, count] : counts) {
        const char* name = nullptr;
        for (size_t i = 0; i < kShaderNameCount && !name; ++i)
            if (g_shader_handles[i] == handle) name = kShaderNames[i];
        char item[96];
        if (name)
            snprintf(item, sizeof(item), "%s%s %llu", s.empty() ? "" : ", ", name, static_cast<unsigned long long>(count));
        else
            snprintf(item, sizeof(item), "%s(shader %08X) %llu", s.empty() ? "" : ", ", handle, static_cast<unsigned long long>(count));
        s += item;
    }
    return s.empty() ? "none" : s;
}

// Telemetry: the shaders a skinned mesh's (a character's) material map draws with, each map's entries once.
void count_skinned_shaders(const uint8_t* map) {
    const uint32_t* materials = nullptr;
    uint32_t num = 0;
    if (!map || !map_entries(map, &materials, &num) || !materials || !num || num > 4096) return;
    if (g_skinned_maps.size() > 4096) g_skinned_maps.clear();
    const void*& seen = g_skinned_maps[map];
    if (seen == materials) return;
    seen = materials;
    lookup_eosglass();
    uint32_t translucent = 0, shaders[31];
    for (uint32_t& h : shaders) h = kShaderUnread;
    transparent_entries(materials, num, &translucent, shaders);
    std::lock_guard<std::mutex> lock(g_glass_shader_mutex);
    for (uint32_t i = 0; i < 31; ++i)
        if (shaders[i] != kShaderUnread) ++g_skinned_shaders[shaders[i]];
}

// `vehicle`: a vehicle's map (rl_deformable_mesh_instance), every entry flagged transparent however many it has.
uint32_t glass_of(const uint8_t* map, uint32_t* clear_out = nullptr, uint32_t* translucent_out = nullptr, bool vehicle = false) {
    const uint32_t* materials = nullptr;
    uint32_t num = 0;
    if (clear_out) *clear_out = 0;
    if (translucent_out) *translucent_out = 0;
    if (!g_glass_on || !map || !map_entries(map, &materials, &num) || !materials || !num || num > 4096) return 0;
    if (g_glass.size() > 65536) g_glass.clear();
    GlassEntry& e = g_glass[map];
    if (e.materials == materials && e.num == num && e.vehicle == vehicle && g_glass_scene - e.scene < 256) {
        if (clear_out) *clear_out = e.clear;
        if (translucent_out) *translucent_out = e.translucent;
        return e.mask;
    }
    e.materials = materials;
    e.num = num;
    e.scene = g_glass_scene;
    e.vehicle = vehicle;
    lookup_eosglass();
    uint32_t translucent = 0, shaders[31];
    for (uint32_t& s : shaders) s = kShaderUnread;
    e.mask = transparent_entries(materials, num, &translucent, shaders);
    // A map whose every entry (three or more) is transparent is an object made to fade in and out (the vehicles' are
    // all so, whatever their number), not glass all over: its glass is the entries drawing the plain glass's shaders
    // (the windows), the rest solid.
    const uint32_t entries = num < 32 ? num : 32;
    if (vehicle || (entries >= 3 && e.mask == (entries == 32 ? 0xFFFFFFFFu : (1u << entries) - 1u))) {
        uint32_t panes = 0;
        std::lock_guard<std::mutex> lock(g_glass_shader_mutex);
        for (uint32_t i = 0; i < 31; ++i) {
            if (shaders[i] == kShaderUnread) continue;
            ++g_fader_shaders[shaders[i]];
            if (shaders[i] == g_shader_handles[0] || shaders[i] == g_shader_handles[1]) panes |= 1u << i;
        }
        e.mask = panes;
        translucent = 0;
        ++(vehicle ? g_glass_vehicle_maps : g_glass_faders);
        if (panes) ++g_glass_by_shader;
    } else if (e.mask | translucent) {
        std::lock_guard<std::mutex> lock(g_glass_shader_mutex);
        for (uint32_t i = 0; i < 31; ++i)
            if (((e.mask | translucent) >> i) & 1u) ++g_glass_shaders[shaders[i]];
    }
    e.clear = e.mask;
    e.translucent = translucent;
    if (clear_out) *clear_out = e.clear;
    if (translucent_out) *translucent_out = e.translucent;
    if (translucent) {  // translucent glass (rays partly through) beside the clear (frame_scene.h)
        if (((e.mask | translucent) >> 15) == 0)  // all among the first 15 entries: each kind its own
            e.mask = e.mask | (translucent << 15) | 0x40000000u | 0x80000000u;
        else  // all of it translucent
            e.mask |= translucent | 0x80000000u;
        ++g_translucent_maps;
    }
    ++g_glass_reads;
    return e.mask;
}

// The core gets each mesh's materials, once a mesh, from the map of the first instance of it seen (kMeshMaterials): its
// glass by material map entry, all 32 (the instance's glass word keeps only 15 apart), and each entry's surface for the
// rays' hits (mirror/textures.h). A mesh whose diffuse maps are still being copied waits for them (a material names a
// texture only once the core has it), up to kMaterialsWait scenes; sent sooner, it is sent again once they are all there.
// Under g_mutex (the snapshot's).
enum : uint8_t { kMatNew = 0, kMatWaiting = 1, kMatPartial = 2, kMatDone = 3 };
std::vector<uint8_t> g_mesh_materials;  // by mesh id (the plugin's numbers count up from 1)
struct MaterialsWaiting {
    uint32_t clear = 0, translucent = 0;
    std::vector<rfgrt_scene::MaterialEntry> entries;
    int64_t since = 0, checked = -1;
};
std::unordered_map<uint32_t, MaterialsWaiting> g_materials_waiting;
std::atomic<size_t> g_materials_waiting_n{0};  // its size, for the log (another thread)
constexpr int64_t kMaterialsWait = 300;
uint64_t g_materials_records = 0, g_materials_partial = 0, g_materials_unread = 0, g_material_entries = 0,
         g_material_textured = 0;
struct MeshMaterialsCtx {
    uint32_t mesh, clear, translucent;
    const rfgrt_scene::MaterialEntry* entries;
    uint32_t count;
    bool partial;  // the pending textures named as none
};
void fill_mesh_materials(uint8_t* dst, void* ctx) {
    const auto& c = *static_cast<const MeshMaterialsCtx*>(ctx);
    auto* r = reinterpret_cast<rfgrt_scene::MeshMaterials*>(dst);
    r->h.type = rfgrt_scene::kMeshMaterials;
    r->h.bytes = uint32_t(sizeof(*r) + c.count * sizeof(rfgrt_scene::MaterialEntry));
    r->mesh_id = c.mesh;
    r->clear = c.clear;
    r->translucent = c.translucent;
    r->count = c.count;
    auto* e = reinterpret_cast<rfgrt_scene::MaterialEntry*>(dst + sizeof(*r));
    for (uint32_t i = 0; i < c.count; ++i) {
        e[i] = c.entries[i];
        if (textures::state(e[i].texture) != textures::kSent) e[i].texture = rfgrt_scene::kNoTexture;
    }
}
bool write_mesh_materials(uint32_t mesh, const MaterialsWaiting& w, bool partial) {
    MeshMaterialsCtx c{mesh, w.clear, w.translucent, w.entries.data(), uint32_t(w.entries.size()), partial};
    const uint64_t bytes = sizeof(rfgrt_scene::MeshMaterials) + w.entries.size() * sizeof(rfgrt_scene::MaterialEntry);
    if (!rtlink::core_link().ring_write(bytes, &fill_mesh_materials, &c, 0)) return false;
    ++g_materials_records;
    if (partial) ++g_materials_partial;
    return true;
}
void send_mesh_materials(uint32_t mesh, const uint8_t* map, uint32_t clear, uint32_t translucent) {
    if (mesh < g_mesh_materials.size() && g_mesh_materials[mesh] == kMatDone) return;
    if (mesh >= (1u << 24)) return;
    if (mesh >= g_mesh_materials.size()) g_mesh_materials.resize(size_t(mesh) + 1024, kMatNew);
    uint8_t& st = g_mesh_materials[mesh];
    if (st == kMatNew) {  // read its map's materials once
        MaterialsWaiting w;
        w.clear = clear;
        w.translucent = translucent;
        w.since = g_glass_scene;
        const uint32_t* materials = nullptr;
        uint32_t num = 0;
        if (map && map_entries(map, &materials, &num) && materials && num && num <= 4096) {
            const uint32_t n = num < rfgrt_scene::kMaxMaterialEntries ? num : rfgrt_scene::kMaxMaterialEntries;
            w.entries.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                if (!textures::material_entry(materials[i], &w.entries[i])) ++g_materials_unread;
                ++g_material_entries;
                if (w.entries[i].texture != textures::kNone) ++g_material_textured;
            }
        }
        if (g_materials_waiting.size() > 65536) g_materials_waiting.clear();
        g_materials_waiting[mesh] = std::move(w);
        g_materials_waiting_n = g_materials_waiting.size();
        st = kMatWaiting;
    }
    auto it = g_materials_waiting.find(mesh);
    if (it == g_materials_waiting.end()) {  // (dropped: the list grew too long) read again
        st = kMatNew;
        return;
    }
    MaterialsWaiting& w = it->second;
    if (w.checked == g_glass_scene) return;  // once a scene
    w.checked = g_glass_scene;
    bool pending = false;
    for (const auto& e : w.entries) pending |= textures::state(e.texture) == textures::kPending;
    if (pending) {
        if (st == kMatWaiting && g_glass_scene - w.since >= kMaterialsWait && write_mesh_materials(mesh, w, true))
            st = kMatPartial;  // its glass and the textures there so far; the rest when they come
        return;
    }
    if (write_mesh_materials(mesh, w, false)) {
        st = kMatDone;
        g_materials_waiting.erase(it);
        g_materials_waiting_n = g_materials_waiting.size();
    }
}

// The renderer's class ids are handed out at startup by its class factory (rl_class_factory_info::m_class_id).
struct ClassIds {
    uint32_t mesh = 0, mesh_instance = 0, skin_instance = 0, skirt_instance = 0, compound = 0, destroyable = 0,
             height_mesh = 0, clone_mesh = 0;
    // rl_deformable_mesh_instance: a compound mesh instance whose pieces can be dented (the vehicles); the others are
    // only named in the counts of what is left out
    uint32_t deformable = 0, low_lod = 0, landmark = 0, corona = 0, decal = 0, particles = 0, fog = 0;
    bool known = false;
} g_class;

const char* class_name(uint32_t id) {
    if (!id) return "none";
    if (id == g_class.mesh) return "rl_mesh";
    if (id == g_class.mesh_instance) return "rl_mesh_instance";
    if (id == g_class.skin_instance) return "rl_skin_instance";
    if (id == g_class.skirt_instance) return "rl_skirt_mesh_instance";
    if (id == g_class.compound) return "rl_compound_mesh_instance";
    if (id == g_class.destroyable) return "rl_destroyable_model_instance";
    if (id == g_class.height_mesh) return "rl_height_mesh_instance";
    if (id == g_class.clone_mesh) return "rl_clone_mesh_instance";
    if (id == g_class.deformable) return "rl_deformable_mesh_instance";
    if (id == g_class.low_lod) return "rl_low_lod_mesh_instance";
    if (id == g_class.landmark) return "rl_landmark_lod_mesh_instance";
    if (id == g_class.corona) return "rl_corona_instance";
    if (id == g_class.decal) return "rl_planar_decal";
    if (id == g_class.particles) return "rl_renderable_particles_instance";
    if (id == g_class.fog) return "rl_fog_volume";
    return nullptr;
}

// What is left out, by class id: a few counters (an id per slot, the rest counted together under id 0xFFFFFFFF).
struct ClassCounts {
    uint32_t id[12] = {};
    uint64_t n[12] = {};
    void add(uint32_t cid, uint64_t k = 1) {
        for (int i = 0; i < 12; ++i) {
            if (n[i] && id[i] == cid) return void(n[i] += k);
            if (!n[i]) {
                id[i] = cid;
                n[i] = k;
                return;
            }
        }
        add2(k);
    }
    void add2(uint64_t k) {  // the last slot holds the rest
        id[11] = 0xFFFFFFFFu;
        n[11] += k;
    }
    void merge(const ClassCounts& o) {
        for (int i = 0; i < 12; ++i)
            if (o.n[i]) add(o.id[i], o.n[i]);
    }
    std::string describe(double per) const {
        std::string out;
        for (int i = 0; i < 12; ++i) {
            if (!n[i]) continue;
            char item[96];
            const char* nm = id[i] == 0xFFFFFFFFu ? "others" : class_name(id[i]);
            if (nm)
                snprintf(item, sizeof(item), "%s%s %.1f", out.empty() ? "" : ", ", nm, double(n[i]) / per);
            else
                snprintf(item, sizeof(item), "%sclass %u %.1f", out.empty() ? "" : ", ", id[i], double(n[i]) / per);
            out += item;
        }
        return out.empty() ? "none" : out;
    }
};

void read_class_ids() {
    auto id = [](uint32_t rva) { return rfg::field<uint32_t>(rfg::addr(rva), rfg::off::ClassInfo_classId); };
    g_class.mesh = id(rfg::rva::ClassInfo_mesh);
    g_class.mesh_instance = id(rfg::rva::ClassInfo_meshInstance);
    g_class.skin_instance = id(rfg::rva::ClassInfo_skinInstance);
    g_class.skirt_instance = id(rfg::rva::ClassInfo_skirtMeshInstance);
    g_class.compound = id(rfg::rva::ClassInfo_compoundMeshInstance);
    g_class.destroyable = id(rfg::rva::ClassInfo_destroyableModelInstance);
    g_class.height_mesh = id(rfg::rva::ClassInfo_heightMeshInstance);
    g_class.clone_mesh = id(rfg::rva::ClassInfo_cloneMeshInstance);
    g_class.deformable = id(rfg::rva::ClassInfo_deformableMeshInstance);
    g_class.low_lod = id(rfg::rva::ClassInfo_lowLodMeshInstance);
    g_class.landmark = id(rfg::rva::ClassInfo_landmarkLodMeshInstance);
    g_class.corona = id(rfg::rva::ClassInfo_coronaInstance);
    g_class.decal = id(rfg::rva::ClassInfo_planarDecal);
    g_class.particles = id(rfg::rva::ClassInfo_particlesInstance);
    g_class.fog = id(rfg::rva::ClassInfo_fogVolume);
    g_class.known = g_class.mesh && g_class.mesh_instance && g_class.compound;
    if (g_class.known)
        LOG("instances: class ids: rl_mesh %u, rl_mesh_instance %u, rl_skin_instance %u, rl_skirt_mesh_instance %u, "
            "rl_compound_mesh_instance %u, rl_destroyable_model_instance %u, rl_height_mesh_instance %u, rl_clone_mesh_instance %u",
            g_class.mesh, g_class.mesh_instance, g_class.skin_instance, g_class.skirt_instance, g_class.compound,
            g_class.destroyable, g_class.height_mesh, g_class.clone_mesh);
    if (g_class.known)
        LOG("instances: class ids: rl_deformable_mesh_instance %u (traced as a compound), rl_low_lod_mesh_instance %u, "
            "rl_landmark_lod_mesh_instance %u, rl_corona_instance %u, rl_planar_decal %u, rl_renderable_particles_instance %u, "
            "rl_fog_volume %u",
            g_class.deformable, g_class.low_lod, g_class.landmark, g_class.corona, g_class.decal, g_class.particles, g_class.fog);
}

// --- transforms: the game's matrix43 is row-vector (p' = p R + t; R's rows are right, up, forward) ------------------

struct M43 {
    float r[3][3];
    float t[3];
};

M43 load_m43(const uint8_t* p) {
    M43 m;
    memcpy(&m, p, sizeof(m));
    return m;
}

// a then b (row-vector): p' = (p Ra + ta) Rb + tb
M43 mul(const M43& a, const M43& b) {
    M43 o{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) o.r[i][j] = a.r[i][0] * b.r[0][j] + a.r[i][1] * b.r[1][j] + a.r[i][2] * b.r[2][j];
    }
    for (int j = 0; j < 3; ++j) o.t[j] = a.t[0] * b.r[0][j] + a.t[1] * b.r[1][j] + a.t[2] * b.r[2][j] + b.t[j];
    return o;
}

// To DXR's layout (column-vector rows): m[row][col] = R[col][row], m[row][3] = t[row].
Transform to_dxr(const M43& m) {
    Transform x{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) x.m[row][col] = m.r[col][row];
        x.m[row][3] = m.t[row];
    }
    return x;
}

void apply(const M43& m, const float* p, float* out) {
    for (int j = 0; j < 3; ++j) out[j] = p[0] * m.r[0][j] + p[1] * m.r[1][j] + p[2] * m.r[2][j] + m.t[j];
}

// --- skinned instances: their bone palettes go into the frame scene (frame_scene.h Skin and Bone) --------------------

bool copy_palette(rfgrt_frame::Bone* dst, const uint8_t* src, uint32_t count) {
    __try {
        memcpy(dst, src, size_t(count) * sizeof(rfgrt_frame::Bone));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t skin_key(const uint8_t* inst) {
    const uint64_t v = uint64_t(reinterpret_cast<uintptr_t>(inst));
    return uint32_t(v) ^ uint32_t(v >> 32);
}

// rl_skin_instance::m_bone_matrices_p (matrix43 a bone) and m_num_bones_matrices. False when it has none readable or
// the slot is full (the core then leaves the instance in its bind pose).
bool add_palette(uint8_t* inst, uint32_t index, rfgrt_frame::Skin* skins, rfgrt_frame::Bone* bones, uint32_t max_skins,
                 uint32_t max_bones, uint32_t& skin_n, uint32_t& bone_n) {
    const uint8_t* palette = rfg::field<const uint8_t*>(inst, rfg::off::SkinInstance_boneMatrices);
    const int32_t count = rfg::field<int32_t>(inst, rfg::off::SkinInstance_numBones);
    if (!palette || count <= 0 || count > 1024 || skin_n >= max_skins || bone_n + uint32_t(count) > max_bones) return false;
    if (!copy_palette(bones + bone_n, palette, uint32_t(count))) return false;
    rfgrt_frame::Skin& sk = skins[skin_n++];
    sk.instance = index;
    sk.key = skin_key(inst);
    sk.first_bone = bone_n;
    sk.bone_count = uint32_t(count);
    bone_n += uint32_t(count);
    return true;
}

// Once (the sample frame): a skinned instance's sample vertices posed by its palette under both readings of it, bones
// in the instance's own space (then its world matrix) or in the world (absolute coordinates). The right one lands
// around the instance, where the bind pose does.
bool check_skin(uint8_t* inst, uint32_t mesh, const M43& world, const float* offset) {
    geometry::SkinSample smp;
    if (!geometry::skin_sample(mesh, &smp)) return false;
    const uint8_t* palette = rfg::field<const uint8_t*>(inst, rfg::off::SkinInstance_boneMatrices);
    const int32_t count = rfg::field<int32_t>(inst, rfg::off::SkinInstance_numBones);
    if (!palette || count <= 0 || count > 1024) return false;
    std::vector<rfgrt_frame::Bone> raw(static_cast<size_t>(count));
    if (!copy_palette(raw.data(), palette, uint32_t(count))) return false;
    float box[3][2][3];
    for (auto& b : box)
        for (int k = 0; k < 3; ++k) {
            b[0][k] = 1e30f;
            b[1][k] = -1e30f;
        }
    auto grow = [](float (&b)[2][3], const float* p) {
        for (int k = 0; k < 3; ++k) {
            b[0][k] = p[k] < b[0][k] ? p[k] : b[0][k];
            b[1][k] = p[k] > b[1][k] ? p[k] : b[1][k];
        }
    };
    for (uint32_t i = 0; i < smp.count; ++i) {
        float posed[3] = {0, 0, 0}, wsum = 0;
        for (int k = 0; k < 4; ++k) {
            const float w = smp.weight[i][k] / 255.0f;
            if (w <= 0.0f) continue;
            const int bi = smp.bone[i][k] < count ? smp.bone[i][k] : count - 1;
            M43 b;
            memcpy(&b, raw[size_t(bi)].m, sizeof(b));
            float q[3];
            apply(b, smp.pos[i], q);
            for (int c = 0; c < 3; ++c) posed[c] += w * q[c];
            wsum += w;
        }
        if (wsum > 0.0f)
            for (float& c : posed) c /= wsum;
        float bind[3], local[3], absolute[3];
        apply(world, smp.pos[i], bind);
        apply(world, posed, local);
        for (int c = 0; c < 3; ++c) absolute[c] = posed[c] - offset[c];
        grow(box[0], bind);
        grow(box[1], local);
        grow(box[2], absolute);
    }
    LOG("instances: skin check: rl_skin_instance %p at (%.2f %.2f %.2f), mesh %u, %d bones, %u sample vertices: bind pose "
        "(%.2f %.2f %.2f)..(%.2f %.2f %.2f); palette in the instance's space (%.2f %.2f %.2f)..(%.2f %.2f %.2f); palette in "
        "the world (%.2f %.2f %.2f)..(%.2f %.2f %.2f)",
        static_cast<void*>(inst), world.t[0], world.t[1], world.t[2], mesh, count, smp.count, box[0][0][0], box[0][0][1],
        box[0][0][2], box[0][1][0], box[0][1][1], box[0][1][2], box[1][0][0], box[1][0][1], box[1][0][2], box[1][1][0],
        box[1][1][1], box[1][1][2], box[2][0][0], box[2][0][1], box[2][0][2], box[2][1][0], box[2][1][1], box[2][1][2]);
    const float* b0 = raw[0].m;
    LOG("instances:   bone 0: rows (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f) t (%.3f %.3f %.3f); sample vertex 0 bones "
        "%u %u %u %u weights %u %u %u %u",
        b0[0], b0[1], b0[2], b0[3], b0[4], b0[5], b0[6], b0[7], b0[8], b0[9], b0[10], b0[11], smp.bone[0][0], smp.bone[0][1],
        smp.bone[0][2], smp.bone[0][3], smp.weight[0][0], smp.weight[0][1], smp.weight[0][2], smp.weight[0][3]);
    return true;
}

// rl_sub_instance's rotation, made by the game's own matrix::set(pitch, bank, heading) (as get_sub_instance_info does).
void angles_to_matrix(float pitch, float bank, float heading, float (&r)[3][3]) {
    using SetFn = void(__thiscall*)(void*, float, float, float);
    rfg::fn<SetFn>(rfg::rva::matrix__set_angles)(r, pitch, bank, heading);
}

// --- hooks ---------------------------------------------------------------------------------------------------------

void*(__thiscall* o_add)(uint8_t*, void*, uint8_t*);
void(__thiscall* o_remove)(uint8_t*, uint8_t*);

// rl_handle rl_scene::add_renderable_instance(rl_renderable_instance*): the handle is returned through a hidden pointer.
void* __fastcall d_add(RFG_THIS(uint8_t*, scene), void* ret, uint8_t* inst) {
    void* r = o_add(scene, ret, inst);
    if (inst) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances[inst] = scene;
    }
    return r;
}

void __fastcall d_remove(RFG_THIS(uint8_t*, scene), uint8_t* inst) {
    if (inst) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances.erase(inst);
        g_buildings.erase(inst);
    }
    o_remove(scene, inst);
}

// Over the instance cap (a Wrecking Crew map levelled with rfgphys keeping the debris: 80,000 pieces and more) the
// farthest go first, not whichever come last: each frame scene counts its instances by distance from the camera (in
// quarter steps of log2 of the squared distance), and when one held more than the cap, the next leaves out those beyond
// the distance that keeps 95 % of it. The terrain and the characters are always kept. Render thread only.
constexpr int kDistBuckets = 96;  // up to 2^24 m^2: 4 km
float g_cut_d2 = INFINITY;        // this frame's: instances farther are left out (squared metres)
uint32_t g_cap_logged = 0;        // the cap, for the log
int dist_bucket(float d2) {
    const int b = int(log2f(d2 + 1.0f) * 4.0f);
    return b < 0 ? 0 : b >= kDistBuckets ? kDistBuckets - 1 : b;
}

struct Stats {
    uint64_t snapshots = 0, instances = 0, dropped = 0, no_mesh = 0, other_renderable = 0, other_class = 0, stale = 0;
    uint64_t culled = 0, cut_frames = 0;  // left out for being far (over the cap), frame scenes that cut
    double cut_m = 0.0;                   // the distance they were cut at, summed over those
    uint64_t hidden = 0, faded = 0, skins = 0, skins_dropped = 0, bones = 0;
    uint64_t kinds[8] = {};
    uint64_t buildings_reused = 0, buildings_remade = 0;
    uint64_t skirts = 0, vehicles = 0;  // terrain skirts left out; deformable (vehicle) instances traced
    uint64_t glass = 0, glass_maps = 0;  // instances drawing transparent materials; material maps read for that
    uint64_t lit_taken = 0, lit_differ = 0, fog_off_read = 0;  // the lit passes' constants used; render()'s renderer held
                                                               // others (and the fog off) at the snapshot
    ClassCounts other_classes, other_renderables;
    int64_t ticks = 0;
    uint32_t last_count = 0;
    size_t registered = 0;
} g_stats;
std::mutex g_stats_mutex;  // written on the render thread, reported on the main loop's

void log_stats(Stats st) {
    if (!st.snapshots) return;
    const double f = double(st.snapshots);
    LOG("instances: %zu registered; %llu frame scenes, %.0f instances each (%u in the last): meshes %.0f, skinned %.0f, "
        "Geo-Mod pieces %.0f, terrain %.0f; %.1f us per scene; per scene left out: hidden %.0f, no mesh in the mirror %.0f, not an "
        "rl_mesh %.0f, other classes %.0f, stale %.1f; opacity 0 (kept) %.0f; dropped %llu; buildings per scene: %.0f "
        "reused, %.1f remade; skinned instances posed by the core %.1f a scene (%.0f bones), without a palette %.1f",
        st.registered, static_cast<unsigned long long>(st.snapshots), double(st.instances) / f, st.last_count,
        double(st.kinds[rfgrt_frame::kKindMesh]) / f, double(st.kinds[rfgrt_frame::kKindSkinned]) / f,
        double(st.kinds[rfgrt_frame::kKindCompoundPiece]) / f, double(st.kinds[rfgrt_frame::kKindTerrain]) / f,
        session::ticks_to_ms(st.ticks) * 1000.0 / f,
        double(st.hidden) / f, double(st.no_mesh) / f, double(st.other_renderable) / f, double(st.other_class) / f,
        double(st.stale) / f, double(st.faded) / f, static_cast<unsigned long long>(st.dropped),
        double(st.buildings_reused) / f, double(st.buildings_remade) / f, double(st.skins) / f, double(st.bones) / f,
        double(st.skins_dropped) / f);
    if (st.cut_frames)
        LOG("instances: over the cap (%u): %.0f a scene left out for being farthest, beyond %.0f m on average, in %llu of %llu scenes",
            g_cap_logged, double(st.culled) / double(st.cut_frames), st.cut_m / double(st.cut_frames),
            static_cast<unsigned long long>(st.cut_frames), static_cast<unsigned long long>(st.snapshots));
    LOG("instances: per scene: vehicles (deformable meshes) %.1f, terrain skirts left out %.1f; left out by class: %s; by the "
        "class of what they draw (not an rl_mesh): %s",
        double(st.vehicles) / f, double(st.skirts) / f, st.other_classes.describe(f).c_str(),
        st.other_renderables.describe(f).c_str());
    LOG("instances: per scene: %.1f instances draw transparent materials (glass: rays go through those), %.2f material maps "
        "read for that; so far %llu vehicles' maps and %llu others all transparent (objects that fade) read, their glass "
        "the entries drawing rfg-glass or rfg-glass-etched (the rest solid) in %llu of them", double(st.glass) / f,
        double(st.glass_maps) / f, static_cast<unsigned long long>(g_glass_vehicle_maps),
        static_cast<unsigned long long>(g_glass_faders), static_cast<unsigned long long>(g_glass_by_shader));
    LOG("instances: the transparent and translucent materials' shaders (material map entries read): %s",
        shaders_described(g_glass_shaders).c_str());
    LOG("instances: the vehicles' and the fading objects' materials' shaders (material map entries read): %s",
        shaders_described(g_fader_shaders).c_str());
    LOG("instances: the skinned meshes' (the characters') materials' shaders (each map's entries once): %s",
        shaders_described(g_skinned_shaders).c_str());
    LOG("instances: meshes' materials sent to the core: %llu records (%llu of them before all their textures were there), "
        "%zu meshes waiting for theirs; %llu material map entries read (%llu naming a diffuse map, %llu unreadable)",
        static_cast<unsigned long long>(g_materials_records), static_cast<unsigned long long>(g_materials_partial),
        g_materials_waiting_n.load(), static_cast<unsigned long long>(g_material_entries),
        static_cast<unsigned long long>(g_material_textured), static_cast<unsigned long long>(g_materials_unread));
    textures::log_stats();
    LOG("instances: the lighting constants of the lit passes in %llu of %llu scenes; render()'s renderer held others in %llu "
        "of them (the fog off or of another colour in %llu)",
        static_cast<unsigned long long>(st.lit_taken), static_cast<unsigned long long>(st.snapshots),
        static_cast<unsigned long long>(st.lit_differ), static_cast<unsigned long long>(st.fog_off_read));
}

// The 32-bit game shares its 4 GB of address space with every mod in it (rfgphys's pools, this plugin's shared region):
// what is left, and the largest block (what a big allocation, a texture, needs). The game ends with a fatal error when
// it runs out.
void log_address_space() {
    if (sizeof(void*) != 4) return;
    uint64_t free_total = 0, largest = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = 0x10000;
    while (VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_FREE) {
            free_total += mbi.RegionSize;
            if (mbi.RegionSize > largest) largest = mbi.RegionSize;
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= a) break;
        a = next;
    }
    LOG("address space: %llu MB free, the largest free block %llu MB", static_cast<unsigned long long>(free_total >> 20),
        static_cast<unsigned long long>(largest >> 20));
}

// Test.ProbeKey and Test.ProbeAt: the core logs what the rays under the crosshair meet (rtcore, shaders/probe.hlsl).
void poll_probe() {
    static bool was_down = false, fired_at = false;
    bool fire = false;
    if (g_probe_key > 0 && g_probe_key < 256) {
        const bool down = (GetAsyncKeyState(g_probe_key) & 0x8000) != 0;
        if (down && !was_down) {
            DWORD pid = 0;
            const HWND fg = GetForegroundWindow();
            if (fg) GetWindowThreadProcessId(fg, &pid);
            fire = pid == GetCurrentProcessId();  // the game's window has the keyboard
        }
        was_down = down;
    }
    if (g_probe_at > 0 && !fired_at && session::gameplay_seconds() >= g_probe_at) {
        fired_at = true;
        fire = true;
    }
    static bool fired_frame = false;
    if (g_probe_at_frame >= 0 && !fired_frame && session::gameplay_frames() >= g_probe_at_frame) {
        fired_frame = true;
        fire = true;
        LOG("probe: gameplay frame %lld (Test.ProbeAtFrame %d)", static_cast<long long>(session::gameplay_frames()), g_probe_at_frame);
    }
    // Test.Probes: one a frame at most (the core takes a request a frame), each at its own point of the screen
    float x = g_probe_x, y = g_probe_y;
    if (!fire)
        for (PlannedProbe& pp : g_probes) {
            if (pp.fired || session::gameplay_frames() < pp.frame) continue;
            pp.fired = fire = true;
            x = pp.x;
            y = pp.y;
            LOG("probe: gameplay frame %lld (Test.Probes: frame %lld)", static_cast<long long>(session::gameplay_frames()),
                static_cast<long long>(pp.frame));
            break;
        }
    if (!fire) return;
    rtlink::Link& link = rtlink::core_link();
    if (!link.ready()) return;
    link.block()->probe_x = x;
    link.block()->probe_y = y;
    link.request_probe();
    LOG("probe: asked the core what the rays at (%.3f, %.3f) of the screen meet (its log, rfgrt_host.log, says)", x, y);
}

void on_frame(int) {
    poll_probe();
    static int64_t last = 0;
    static int calls = 0;
    const int64_t now = session::qpc();
    if (!last) last = now;
    if (session::ticks_to_ms(now - last) < 15000) return;
    last = now;
    if (calls++ % 4 == 0) log_address_space();  // every minute
    Stats st;
    {
        std::lock_guard<std::mutex> lock(g_stats_mutex);
        st = g_stats;
        g_stats = Stats{};
    }
    log_stats(st);
}

}  // namespace

bool install(const Config& cfg) {
    if (!cfg.host_enabled || !cfg.mirror_enabled) return true;
    g_glass_on = cfg.glass;
    g_vehicle_glass_at = cfg.vehicle_glass_at;
    g_probe_key = cfg.probe_key;
    g_probe_at = cfg.probe_at_s;
    g_probe_at_frame = cfg.probe_at_frame;
    if (cfg.probe_pixel.size() >= 2) {
        g_probe_x = float(cfg.probe_pixel[0]);
        g_probe_y = float(cfg.probe_pixel[1]);
    }
    // Test.Probes: frame@x,y/frame@x,y/... ('|' too, where a command line takes it)
    for (size_t at = 0; at < cfg.probes.size();) {
        size_t end = cfg.probes.find_first_of("/|", at);
        if (end == std::string::npos) end = cfg.probes.size();
        const std::string item = cfg.probes.substr(at, end - at);
        long long frame = -1;
        float px = 0.5f, py = 0.5f;
        if (sscanf_s(item.c_str(), "%lld@%f,%f", &frame, &px, &py) == 3 && frame >= 0)
            g_probes.push_back(PlannedProbe{frame, px, py, false});
        else if (!item.empty())
            LOG("probe: Test.Probes item \"%s\" not understood (frame@x,y)", item.c_str());
        at = end + 1;
    }
    std::sort(g_probes.begin(), g_probes.end(), [](const PlannedProbe& a, const PlannedProbe& b) { return a.frame < b.frame; });
    if (!g_probes.empty()) LOG("probe: %zu planned (Test.Probes)", g_probes.size());
    session::add_frame_callback(&on_frame);
    bool ok = hooks::create_this(rfg::rva::rl_scene__add_renderable_instance, &d_add, &o_add, "rl_scene::add_renderable_instance");
    ok &= hooks::create_this(rfg::rva::rl_scene__remove_renderable_instance, &d_remove, &o_remove,
                             "rl_scene::remove_renderable_instance");
    return ok;
}

void snapshot(uint8_t* renderer, uint8_t* scene, uint8_t* camera, int64_t frame) {
    rtlink::Link& link = rtlink::core_link();
    rfgrt_frame::SlotHeader* h = link.frame_slot(frame);
    if (!h || !scene || !camera) return;
    if (!g_class.known) read_class_ids();
    const int64_t t0 = session::qpc();
    rfgrt_ipc::store64(&h->frame, 0);  // being written
    Instance* out = rfgrt_frame::slot_instances(h);
    const uint32_t cap = link.max_instances();
    const uint32_t max_skins = link.max_skins(), max_bones = link.max_bones();
    rfgrt_frame::Skin* skins = max_skins ? rfgrt_frame::slot_skins(h, cap) : nullptr;
    rfgrt_frame::Bone* bones = max_skins ? rfgrt_frame::slot_bones(h, cap, max_skins) : nullptr;
    uint32_t skin_n = 0, bone_n = 0, skins_dropped = 0;
    int skin_checks = 0;
    uint32_t n = 0, dropped = 0;
    uint32_t counts[8] = {};
    uint64_t no_mesh = 0, other_renderable = 0, other_class = 0, stale = 0, hidden = 0, faded = 0;
    uint64_t reused = 0, remade = 0, skirts = 0, vehicles = 0, glassy = 0;
    ++g_glass_scene;
    g_glass_reads = 0;
    if (g_vehicle_glass_next < g_vehicle_glass_at.size() &&
        session::gameplay_seconds() >= g_vehicle_glass_at[g_vehicle_glass_next]) {
        ++g_vehicle_glass_next;
        g_vehicle_glass = !g_vehicle_glass;
        LOG("instances: the vehicles' windows %s (Test.VehicleGlassAt)", g_vehicle_glass ? "glass again" : "solid, as before");
    }
    ClassCounts other_classes, other_renderables;
    // A subzone's height mesh is drawn as ~25 chunk instances (m_sub_mesh_index into a chunk table it gets after its
    // geometry is made); the mirror has it whole, as its first submesh when it was made: once a frame is enough.
    uint32_t terrain_meshes[64];
    int terrain_count = 0;
    size_t registered = 0;
    float offset[3];
    memcpy(offset, scene + rfg::off::Scene_renderOffset, sizeof(offset));
    float cam_pos[3];  // render-offset space, as the instances' transforms
    {
        const M43 c0 = load_m43(camera + rfg::off::Entity_worldMatrix);
        for (int k = 0; k < 3; ++k) cam_pos[k] = c0.t[k] - offset[k];
    }
    uint32_t hist[kDistBuckets] = {};
    uint64_t always = 0, culled = 0;
    const float cut = g_cut_d2;
    g_cap_logged = cap;
    auto far_out = [&](float x, float y, float z) {
        const float dx = x - cam_pos[0], dy = y - cam_pos[1], dz = z - cam_pos[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        ++hist[dist_bucket(d2)];
        if (d2 <= cut) return false;
        ++culled;
        return true;
    };
    auto emit = [&](const M43& m, uint32_t mesh, uint32_t submesh, uint32_t kind, uint32_t flags, uint32_t glass) {
        if (kind == rfgrt_frame::kKindTerrain || kind == rfgrt_frame::kKindSkinned)
            ++always;
        else if (far_out(m.t[0], m.t[1], m.t[2]))
            return;
        if (n >= cap) return void(++dropped);
        Instance& i = out[n++];
        i.object_to_world = to_dxr(m);
        i.mesh_id = mesh;
        i.submesh = submesh;
        i.kind = uint16_t(kind);
        i.flags = uint16_t(flags);
        i.glass = glass;
        ++counts[kind & 7];
        if (glass) ++glassy;
    };
    // Once, well into the session: one building and one plain instance as the snapshot sees them (phase 2 checks).
    static int64_t s_count = 0;
    bool sample = ++s_count == 2000;
    bool sampled_mesh = false, sampled_compound = false, sampled_terrain = false;
    auto log_m43 = [](const char* what, const M43& m) {
        LOG("instances: sample %s: rows (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f) t (%.2f %.2f %.2f)", what, m.r[0][0],
            m.r[0][1], m.r[0][2], m.r[1][0], m.r[1][1], m.r[1][2], m.r[2][0], m.r[2][1], m.r[2][2], m.t[0], m.t[1], m.t[2]);
    };
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto ids = geometry::lock_ids();
        registered = g_instances.size();
        for (const auto& [inst, sc] : g_instances) {
            if (sc != scene) continue;
            if (rfg::field<uint8_t*>(inst, rfg::off::SceneEntity_scene) != scene) {
                ++stale;
                continue;
            }
            // rl_scene_entity::FLAGS_NOT_VISIBLE: the game does not draw it (add_renderable_instance keeps it out of
            // the partition system)
            if (rfg::field<uint16_t>(inst, rfg::off::SceneEntity_flags) & 2) {
                ++hidden;
                continue;
            }
            if (!(rfg::field<float>(inst, rfg::off::Instance_opacity) > 0.0f)) ++faded;  // counted only, for now
            const uint32_t pass = rfg::field<uint16_t>(inst, rfg::off::SceneEntity_passFlags);
            uint8_t* renderable = rfg::field<uint8_t*>(inst, rfg::off::Instance_renderable);
            if (!renderable || rfg::field<uint32_t>(renderable, rfg::off::BaseObject_leafClassId) != g_class.mesh) {
                ++other_renderable;
                other_renderables.add(renderable ? rfg::field<uint32_t>(renderable, rfg::off::BaseObject_leafClassId) : 0);
                continue;
            }
            const uint32_t mesh = geometry::mesh_id(ids, renderable + rfg::off::Mesh_geometry);
            if (!mesh) {
                ++no_mesh;
                continue;
            }
            M43 world = load_m43(inst + rfg::off::Entity_worldMatrix);
            for (int k = 0; k < 3; ++k) world.t[k] -= offset[k];
            const uint32_t cls = rfg::field<uint32_t>(inst, rfg::off::BaseObject_leafClassId);
            if (sample && !sampled_mesh && cls == g_class.mesh_instance) {
                sampled_mesh = true;
                LOG("instances: sample rl_mesh_instance %p, mesh %u, submesh %u, render offset (%.1f %.1f %.1f)",
                    static_cast<void*>(inst), mesh, rfg::field<uint32_t>(inst, rfg::off::MeshInstance_submeshIndex), offset[0],
                    offset[1], offset[2]);
                log_m43("  world (render offset taken off)", world);
            }
            if (cls == g_class.height_mesh) {
                const int32_t sub = rfg::field<int32_t>(inst, rfg::off::HeightMesh_subMeshIndex);
                if (sample && !sampled_terrain) {
                    sampled_terrain = true;
                    const float* tp = reinterpret_cast<const float*>(inst + rfg::off::HeightMesh_targetPosition);
                    const float* ip = reinterpret_cast<const float*>(inst + rfg::off::Instance_parameters);
                    LOG("instances: sample rl_height_mesh_instance %p, mesh %u, submesh %d, target (%.2f %.2f %.2f), parameters "
                        "(%.3f %.3f %.3f %.3f)",
                        static_cast<void*>(inst), mesh, sub, tp[0], tp[1], tp[2], ip[0], ip[1], ip[2], ip[3]);
                    log_m43("  world (render offset taken off)", world);
                    // the mesh's submesh table now (it was read when the mesh was made)
                    const uint8_t* data = renderable + rfg::off::Mesh_data;
                    const int32_t nsub = *reinterpret_cast<const int32_t*>(data + rfg::off::MeshData_numSubMeshes);
                    const uint8_t* table = *reinterpret_cast<const uint8_t* const*>(data + rfg::off::MeshData_subMeshes);
                    LOG("instances:   its mesh now: data set %d, %d submeshes at %p", int(renderable[rfg::off::Mesh_dataSet]), nsub,
                        static_cast<const void*>(table));
                    if (table && sub >= 0 && sub < nsub) {
                        const uint8_t* sm = table + size_t(sub) * rfg::off::Submesh_size;
                        const float* off = reinterpret_cast<const float*>(sm + rfg::off::Submesh_offsetTransform);
                        const uint8_t* field = sm + rfg::off::Submesh_renderBlocks;
                        const uint8_t* rb = field + *reinterpret_cast<const int32_t*>(field);
                        LOG("instances:   submesh %d: %d blocks, offset (%.2f %.2f %.2f), first block: start %u, count %u, "
                            "indices %u..%u",
                            sub, *reinterpret_cast<const int32_t*>(sm + rfg::off::Submesh_numRenderBlocks), off[0], off[1], off[2],
                            *reinterpret_cast<const uint32_t*>(rb + rfg::off::RenderBlock_startIndex),
                            *reinterpret_cast<const uint32_t*>(rb + rfg::off::RenderBlock_numIndices),
                            *reinterpret_cast<const uint32_t*>(rb + rfg::off::RenderBlock_minIndex),
                            *reinterpret_cast<const uint32_t*>(rb + rfg::off::RenderBlock_maxIndex));
                    }
                }
                // KAIKO's terrain vertex shader (heightmesh_*.vso) adds g_terrainVertexConstants.Translation to the
                // decoded grid position, no rotation, no scale: heightMeshInstanceRenderDepthOnly makes it from the
                // instance's m_target_position minus the render offset (not from its world matrix).
                M43 terrain{};
                terrain.r[0][0] = terrain.r[1][1] = terrain.r[2][2] = 1.0f;
                const float* target = reinterpret_cast<const float*>(inst + rfg::off::HeightMesh_targetPosition);
                for (int k = 0; k < 3; ++k) terrain.t[k] = target[k] - offset[k];
                const geometry::Terrain how = geometry::terrain(ids, mesh, renderable);
                if (how == geometry::Terrain::Pending) continue;  // not in the core yet
                if (how == geometry::Terrain::Chunks) {  // the chunk the game draws, as its own instance
                    if (sub >= 0) emit(terrain, mesh, uint32_t(sub), rfgrt_frame::kKindTerrain, pass, 0);
                    continue;
                }
                bool seen = false;  // sent whole: once a mesh
                for (int t = 0; t < terrain_count && !seen; ++t) seen = terrain_meshes[t] == mesh;
                if (seen) continue;
                if (terrain_count < 64) terrain_meshes[terrain_count++] = mesh;
                emit(terrain, mesh, 0, rfgrt_frame::kKindTerrain, pass, 0);
                continue;
            }
            // Terrain skirts: strips hanging under each terrain tile's edges to hide the cracks between levels of detail;
            // hidden under the ground as the game draws them, but rays meet them where they stand out of it (thin lines
            // of shadow across the slopes).
            if (cls == g_class.skirt_instance) {
                ++skirts;
                continue;
            }
            if (cls == g_class.mesh_instance || cls == g_class.skin_instance) {
                const uint32_t before = n;
                uint32_t mclear = 0, mtranslucent = 0;
                uint8_t* const mmap = rfg::field<uint8_t*>(inst, rfg::off::MeshInstance_matMap);
                const uint32_t gw = glass_of(mmap, &mclear, &mtranslucent);
                if (cls == g_class.skin_instance) count_skinned_shaders(mmap);
                send_mesh_materials(mesh, mmap, mclear, mtranslucent);
                emit(world, mesh, rfg::field<uint32_t>(inst, rfg::off::MeshInstance_submeshIndex),
                     cls == g_class.skin_instance ? rfgrt_frame::kKindSkinned : rfgrt_frame::kKindMesh, pass, gw);
                if (cls == g_class.skin_instance && skins && n > before) {
                    if (!add_palette(inst, n - 1, skins, bones, max_skins, max_bones, skin_n, bone_n)) ++skins_dropped;
                    if (sample && skin_checks < 3 && check_skin(inst, mesh, world, offset)) ++skin_checks;
                }
            } else if (cls == g_class.compound || cls == g_class.destroyable || (g_class.deformable && cls == g_class.deformable)) {
                if (cls == g_class.deformable) ++vehicles;  // (their dents are not followed: the undamaged shape)
                const uint8_t* subs = rfg::field<uint8_t*>(inst, rfg::off::Compound_subInstances);
                const int count = rfg::field<int32_t>(inst, rfg::off::Compound_numSubInstances);
                const size_t subs_bytes = subs && count > 0 ? size_t(count) * rfg::off::SubInstance_size : 0;
                // the vehicles' materials are all flagged transparent (they fade in and out): their glass is the
                // windows' shaders' (glass_of)
                uint32_t mclear = 0, mtranslucent = 0;
                uint8_t* const cmap = rfg::field<uint8_t*>(inst, rfg::off::Compound_matMap);
                uint32_t glass = glass_of(cmap, &mclear, &mtranslucent, cls == g_class.deformable);
                if (cls == g_class.deformable && !g_vehicle_glass) glass = 0;  // (Test.VehicleGlassAt)
                send_mesh_materials(mesh, cmap, mclear, mtranslucent);
                const uint32_t key_ids[3] = {mesh, pass, glass};
                BuildingCache& bc = g_buildings[inst];
                const bool same = !sample && bc.key.size() == 72 + subs_bytes &&
                                  !memcmp(bc.key.data(), inst + rfg::off::Entity_worldMatrix, 48) &&
                                  !memcmp(bc.key.data() + 48, offset, 12) && !memcmp(bc.key.data() + 60, key_ids, 12) &&
                                  (!subs_bytes || !memcmp(bc.key.data() + 72, subs, subs_bytes));
                if (same) {
                    ++reused;
                    for (const Instance& r : bc.records) {
                        if (far_out(r.object_to_world.m[0][3], r.object_to_world.m[1][3], r.object_to_world.m[2][3])) continue;
                        if (n >= cap) {
                            ++dropped;
                            continue;
                        }
                        out[n++] = r;
                        ++counts[rfgrt_frame::kKindCompoundPiece];
                        if (r.glass) ++glassy;
                    }
                    continue;
                }
                ++remade;
                bc.key.resize(72 + subs_bytes);
                memcpy(bc.key.data(), inst + rfg::off::Entity_worldMatrix, 48);
                memcpy(bc.key.data() + 48, offset, 12);
                memcpy(bc.key.data() + 60, key_ids, 12);
                if (subs_bytes) memcpy(bc.key.data() + 72, subs, subs_bytes);
                bc.records.clear();
                const uint32_t first = n;
                const bool log_this = sample && !sampled_compound && count >= 3;
                if (log_this) {
                    sampled_compound = true;
                    LOG("instances: sample compound %p (class %u), mesh %u, %d pieces", static_cast<void*>(inst), cls, mesh, count);
                    log_m43("  world (render offset taken off)", world);
                }
                for (int s = 0; subs && s < count; ++s) {
                    const uint8_t* sub = subs + size_t(s) * rfg::off::SubInstance_size;
                    const uint32_t bits = *reinterpret_cast<const uint32_t*>(sub + rfg::off::SubInstance_flags);
                    if (!(bits & 2)) continue;  // m_is_active
                    const float* tr = reinterpret_cast<const float*>(sub + rfg::off::SubInstance_transform);
                    M43 local{};
                    angles_to_matrix(tr[3], tr[4], tr[5], local.r);
                    memcpy(local.t, tr, sizeof(local.t));
                    if (log_this && s < 4) {
                        LOG("instances:   piece %d: submesh %u, flags %08X, pitch %.4f bank %.4f heading %.4f", s,
                            *reinterpret_cast<const uint16_t*>(sub + rfg::off::SubInstance_submeshId), bits, tr[3], tr[4], tr[5]);
                        log_m43("    piece", local);
                        log_m43("    piece in the world", mul(local, world));
                    }
                    emit(mul(local, world), mesh, *reinterpret_cast<const uint16_t*>(sub + rfg::off::SubInstance_submeshId),
                         rfgrt_frame::kKindCompoundPiece, pass, glass);
                }
                bc.records.assign(out + first, out + n);
                if (dropped) bc.key.clear();  // incomplete: remade next time
            } else {
                ++other_class;
                other_classes.add(cls);
            }
        }
    }
    h->instance_count = n;
    h->dropped = dropped;
    // the next frame's cut: none while everything fits
    {
        uint64_t total = always;
        for (uint32_t c : hist) total += c;
        float next = INFINITY;
        if (total > cap) {
            const uint64_t keep = uint64_t(double(cap) * 0.95);
            const uint64_t budget = keep > always ? keep - always : 0;
            uint64_t acc = 0;
            int b = 0;
            for (; b < kDistBuckets && acc + hist[b] <= budget; ++b) acc += hist[b];
            next = exp2f(float(b) / 4.0f) - 1.0f;  // bucket b's lower bound: it and the farther ones go
        }
        g_cut_d2 = next;
    }
    memcpy(h->render_offset, offset, sizeof(offset));
    memcpy(h->counts, counts, sizeof(counts));
    M43 cam = load_m43(camera + rfg::off::Entity_worldMatrix);
    for (int k = 0; k < 3; ++k) cam.t[k] -= offset[k];
    h->camera.camera_to_world = to_dxr(cam);
    memcpy(h->camera.projection, camera + rfg::off::Camera_projection, sizeof(h->camera.projection));
    memcpy(h->camera.view, camera + rfg::off::Camera_view, sizeof(h->camera.view));
    uint8_t* state = renderer ? rfg::field<uint8_t*>(renderer, rfg::off::Renderer_state) : nullptr;
    if (state) {
        memcpy(h->camera.state_view, state + rfg::off::State_worldView, sizeof(h->camera.state_view));
        memcpy(h->camera.state_projection, state + rfg::off::State_projection, sizeof(h->camera.state_projection));
    } else {
        memset(h->camera.state_view, 0, sizeof(h->camera.state_view));
        memset(h->camera.state_projection, 0, sizeof(h->camera.state_projection));
    }
    h->camera.near_clip = rfg::field<float>(camera, rfg::off::Camera_nearClip);
    h->camera.far_clip = rfg::field<float>(camera, rfg::off::Camera_farClip);
    h->camera.fov = rfg::field<float>(camera, rfg::off::Camera_fov);
    h->camera.depth_max = 1.0f - rfg::global<float>(rfg::rva::RL_reserved_landmark_depth_range);
    uint8_t* sun = rfg::field<uint8_t*>(scene, rfg::off::Scene_largestLight);
    if (!sun) sun = rfg::field<uint8_t*>(scene, rfg::off::Scene_largestLightDefault);
    if (sun) {
        memcpy(h->sun.direction, sun + rfg::off::Light_direction, sizeof(h->sun.direction));
        memcpy(h->sun.color, sun + rfg::off::Light_color, sizeof(h->sun.color));
        h->sun.present = 1.0f;
    } else {
        memset(&h->sun, 0, sizeof(h->sun));
    }
    // The game's lighting as its shaders get it: rl_renderer's g_commonFragmentConstants (ambient at float 8, the back
    // ambient at 12: rl_scene's colours times g_ambientColorScale and g_backAmbientColorScale, set_ambient_color and
    // set_back_ambient_color). The sun reaches the light passes as the main light, its colour times
    // g_mainLightColorScale (rl_renderer::set_light).
    bool lit_taken = false, lit_differ = false, fog_off_read = false;
    if (renderer) {
        const float* frag = reinterpret_cast<const float*>(renderer + rfg::off::Renderer_commonFragmentData);
        const float* vert = reinterpret_cast<const float*>(renderer + rfg::off::Renderer_commonVertexData);
        gi::LitConstants lc;
        if (gi::lit_constants(&lc)) {  // the lit passes' own (gi.h): this runs beside render()'s passes
            memcpy(h->ambient, lc.ambient, 16);
            memcpy(h->back_ambient, lc.back_ambient, 16);
            memcpy(h->fog_color, lc.fog_color, 16);
            memcpy(h->fog_distance, lc.fog_distance, 16);
            lit_taken = true;
            lit_differ = memcmp(vert + 52, lc.fog_distance, 16) != 0 || memcmp(frag + 8, lc.ambient, 32) != 0;
            // what reading render()'s renderer would have given: the fog off, or of another colour
            fog_off_read = vert[53] <= 0.0f && lc.fog_distance[1] > 0.0f;
            for (int k = 0; k < 3; ++k)
                if (fabsf(frag[16 + k] - lc.fog_color[k]) > 0.02f + 0.1f * fabsf(lc.fog_color[k])) fog_off_read = true;
        } else {  // none seen yet: render()'s renderer's
            memcpy(h->ambient, frag + 8, 16);
            memcpy(h->back_ambient, frag + 12, 16);
            memcpy(h->fog_color, frag + 16, 16);  // fogColor (cb0[4])
            memcpy(h->fog_distance, vert + 52, 16);  // fogDistance (byte 208)
        }
        const float scale = rfg::global<float>(rfg::rva::g_mainLightColorScale);
        for (int k = 0; k < 3; ++k) h->light_color[k] = h->sun.color[k] * scale;
        h->light_color[3] = scale;
    }
    h->skin_count = skin_n;
    h->bone_count = bone_n;
    h->skins_dropped = skins_dropped;
    h->pad2 = 0;
    lights::write(h);  // the dynamic lights collected for this frame (render/lights.h)
    rfgrt_ipc::store64(&h->frame, frame);
    link.frame_published(frame);
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    ++g_stats.snapshots;
    g_stats.lit_taken += lit_taken ? 1 : 0;
    g_stats.lit_differ += lit_differ ? 1 : 0;
    g_stats.fog_off_read += fog_off_read ? 1 : 0;
    g_stats.instances += n;
    g_stats.dropped += dropped;
    g_stats.culled += culled;
    if (cut < INFINITY) {
        ++g_stats.cut_frames;
        g_stats.cut_m += sqrt(double(cut));
    }
    g_stats.no_mesh += no_mesh;
    g_stats.other_renderable += other_renderable;
    g_stats.other_class += other_class;
    g_stats.skirts += skirts;
    g_stats.vehicles += vehicles;
    g_stats.glass += glassy;
    g_stats.glass_maps += g_glass_reads;
    g_stats.other_classes.merge(other_classes);
    g_stats.other_renderables.merge(other_renderables);
    g_stats.stale += stale;
    g_stats.hidden += hidden;
    g_stats.faded += faded;
    g_stats.buildings_reused += reused;
    g_stats.buildings_remade += remade;
    g_stats.skins += skin_n;
    g_stats.bones += bone_n;
    g_stats.skins_dropped += skins_dropped;
    for (int k = 0; k < 8; ++k) g_stats.kinds[k] += counts[k];
    g_stats.ticks += session::qpc() - t0;
    g_stats.last_count = n;
    g_stats.registered = registered;
}

void on_core_stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mesh_materials.clear();
    g_materials_waiting.clear();
    g_materials_waiting_n = 0;
}

void report(bool at_exit) {
    Stats st;
    {
        std::unique_lock<std::mutex> lock(g_stats_mutex, std::defer_lock);
        if (at_exit ? !lock.try_lock() : (lock.lock(), false)) return;
        st = g_stats;
        g_stats = Stats{};
    }
    log_stats(st);
}

}  // namespace instances
