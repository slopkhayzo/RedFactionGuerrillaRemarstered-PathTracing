#pragma once
// The scene of one frame: where every render instance is and which mesh it draws, and the camera. The plugin writes
// it at the start of rl_scene_renderer::render, on the render thread, while the game holds its render data still
// (the sim thread changes instances only inside the frame handshake: rl_unlock .. rl_lock in
// game_handshake_do_middle_of_frame, research/docs/01 section 5); the core builds the frame's top-level acceleration
// structure from it.
//
// The region (rfgrt_ipc.h) holds kSlots of these; frame n goes to slot n % kSlots. A slot is a seqlock: the plugin
// sets `frame` to 0, writes, then sets it to n; the core copies and checks that `frame` is still n.
#include <cstdint>

namespace rfgrt_frame {

constexpr uint32_t kSlots = 4;

// 3x4 transform as D3D12_RAYTRACING_INSTANCE_DESC::Transform takes it: row r is output coordinate r, so
// p' = (m[0].xyz . p + m[0].w, m[1].xyz . p + m[1].w, m[2].xyz . p + m[2].w). Positions are relative to the scene's
// render offset (rl_scene::m_render_offset), as the game renders them.
struct Transform {
    float m[3][4];
};

enum InstanceKind : uint32_t {
    kKindMesh = 1,           // rl_mesh_instance: one submesh of an rl_mesh
    kKindSkinned = 2,        // rl_skin_instance (posed by the core when the slot has a Skin record for it)
    kKindCompoundPiece = 3,  // one sub-instance of an rl_compound_mesh_instance (Geo-Mod buildings)
    kKindOther = 4,
    kKindTerrain = 5,        // rl_height_mesh_instance
};

struct Instance {
    Transform object_to_world;  // the submesh's offset is applied by the core (it has the submesh table)
    uint32_t mesh_id;           // the scene ring's mesh id
    uint32_t submesh;
    uint16_t kind;
    uint16_t flags;  // rl_scene_entity::m_pass_flags (rl_renderable_instance::pass_flags)
    // Its transparent materials (glass): bit i for its material map's entry i (the first 31), rl_material_data::mat_flags
    // bit 0 (rl_material::get_is_transparent). Rays go through the ranges that draw them (the core's tlas.cpp). With
    // translucent glass (rfg-eosglass's, which stops a share of the light: bit 31): bit 30 set, the clear glass's entries
    // in bits 0-14 and the translucent's in bits 15-29 (both among the first 15); bit 30 clear, bits 0-30 are all
    // translucent.
    uint32_t glass;
};
static_assert(sizeof(Instance) == 64, "fixed layout");

// A skinned instance's bone palette (rl_skin_instance::m_bone_matrices_p, m_num_bones_matrices), for the core to pose
// its mesh with (skinning.h).
struct Skin {
    uint32_t instance;    // its record in this slot's instances
    uint32_t key;         // the game's rl_skin_instance, the same in every frame (the core keeps its posed BLAS by it)
    uint32_t first_bone;  // in this slot's bones
    uint32_t bone_count;
};
// One bone as matrix43 holds it: the rotation's rows (right, up, forward), then the translation; p' = p R + t.
struct Bone {
    float m[12];
};
static_assert(sizeof(Bone) == 48, "matrix43");

struct Camera {
    Transform camera_to_world;  // rl_camera's world matrix: rows of the rotation are right, up, forward
    float projection[16];       // rl_camera::m_projection_transform, as the game holds it (row-vector convention)
    float view[16];             // rl_camera::m_view_transform
    // The renderer's own (rl_state_manager) at render()'s start: the last 3D pass's, for comparison
    float state_view[16];
    float state_projection[16];
    float near_clip, far_clip, fov;
    // The main scene's viewport depth range is [0, depth_max]: the game keeps [depth_max, 1] for its landmark LOD layer
    // (1 - RL_reserved_landmark_depth_range), so its depth values are the projection's scaled by depth_max.
    float depth_max;
};

// The sun (or moon): the scene's largest light, as game_render_get_TOD_light finds it.
struct Sun {
    float direction[3];  // rl_light::m_light_direction
    float present;       // 1 when there is one
    float color[4];      // rl_light::m_color
};

// A dynamic light the game may draw this frame (an rl_light other than the sun), for its shadow mask (render/lights.h).
constexpr uint32_t kMaxLights = 16;
struct Light {
    float position[3];   // render-offset space
    float radius;        // where its attenuation ends: it lights nothing beyond
    float direction[3];  // spot lights: where they point (unit); zero for the others
    float cos_cone;      // spot lights: the cosine of the cone's outer half-angle; -1 for the others
    uint32_t mask;       // the shadow mask it gets (rfgrt_ipc.h light_mask_handles)
    uint32_t flags;
    float source_radius; // the size of its source (0: hard shadows)
    float fixture;       // within this distance of the light nothing shadows it (its lamp around it)
};
static_assert(sizeof(Light) == 48, "fixed layout");

struct SlotHeader {
    volatile int64_t frame;  // the frame this slot holds; 0 while the plugin writes it
    uint32_t instance_count;
    uint32_t dropped;        // instances that did not fit
    float render_offset[3];
    uint32_t pad;
    Camera camera;
    Sun sun;
    uint32_t counts[8];      // per InstanceKind, for the log
    uint32_t skin_count;     // Skin records
    uint32_t bone_count;     // bones in them
    uint32_t skins_dropped;  // skinned instances without a palette here (no room, none readable)
    uint32_t pad2;
    // The game's lighting in its shaders' units (rl_renderer's g_commonFragmentConstants): the ambient colour (the sky
    // above), the back ambient (below), and the sun's light colour.
    float ambient[4];
    float back_ambient[4];
    float light_color[4];
    // The game's fog as its shaders get it: g_commonVertexConstants.fogDistance (b the height falloff, a the density, -,
    // c the start, as optical depth: tau = a d (1 - e^(-b dy)) / (b dy) - c, dy the height above the camera) and
    // g_commonFragmentConstants.fogColor.
    float fog_distance[4];
    float fog_color[4];
    // The game's cloud shadows (rl_scene_shadows.cpp buildShadowParamsProjectionTexture, the CLB projector's shader): the
    // overhead clouds' texture laid straight down and scrolled. At a point p (render-offset space) its alpha at uv =
    // (p.x cloud_uv[0] + cloud_uv[2], p.z cloud_uv[1] + cloud_uv[3]) (wrapping) shadows saturate(saturate(alpha +
    // cloud_shadow[1]) cloud_shadow[0]) of the sun. cloud_texture: the core's id of that texture + 1 (mirror/textures.h),
    // 0 for none (no clouds, GI.CloudShadows 0, or not with the core yet).
    float cloud_uv[4];
    float cloud_shadow[2];
    uint32_t cloud_texture;
    uint32_t pad5;
    // The dynamic lights with a shadow mask this frame.
    uint32_t light_count;
    uint32_t lights_dropped;  // more lights than masks: the farthest go without
    uint32_t pad4[2];
    Light lights[kMaxLights];
};

// A slot: the header, then max_instances Instance records, max_skins Skin records and max_bones Bone matrices.
constexpr uint64_t kHeaderBytes = (sizeof(SlotHeader) + 63) & ~uint64_t(63);
inline uint64_t slot_bytes(uint32_t max_instances, uint32_t max_skins, uint32_t max_bones) {
    return kHeaderBytes + uint64_t(max_instances) * sizeof(Instance) + uint64_t(max_skins) * sizeof(Skin) +
           uint64_t(max_bones) * sizeof(Bone);
}
inline Instance* slot_instances(SlotHeader* h) { return reinterpret_cast<Instance*>(reinterpret_cast<uint8_t*>(h) + kHeaderBytes); }
inline const Instance* slot_instances(const SlotHeader* h) {
    return reinterpret_cast<const Instance*>(reinterpret_cast<const uint8_t*>(h) + kHeaderBytes);
}
inline Skin* slot_skins(SlotHeader* h, uint32_t max_instances) {
    return reinterpret_cast<Skin*>(reinterpret_cast<uint8_t*>(h) + kHeaderBytes + uint64_t(max_instances) * sizeof(Instance));
}
inline const Skin* slot_skins(const SlotHeader* h, uint32_t max_instances) {
    return reinterpret_cast<const Skin*>(reinterpret_cast<const uint8_t*>(h) + kHeaderBytes + uint64_t(max_instances) * sizeof(Instance));
}
inline Bone* slot_bones(SlotHeader* h, uint32_t max_instances, uint32_t max_skins) {
    return reinterpret_cast<Bone*>(reinterpret_cast<uint8_t*>(h) + kHeaderBytes + uint64_t(max_instances) * sizeof(Instance) +
                                   uint64_t(max_skins) * sizeof(Skin));
}
inline const Bone* slot_bones(const SlotHeader* h, uint32_t max_instances, uint32_t max_skins) {
    return reinterpret_cast<const Bone*>(reinterpret_cast<const uint8_t*>(h) + kHeaderBytes + uint64_t(max_instances) * sizeof(Instance) +
                                         uint64_t(max_skins) * sizeof(Skin));
}

}  // namespace rfgrt_frame
