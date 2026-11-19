// Poses a skinned mesh for ray tracing (skinning.h): each vertex of the bind pose, moved by its bones' matrices from
// the instance's palette (rl_skin_instance::m_bone_matrices_p, matrix43 as the game keeps it: p' = p R + t), blended by
// its weights. The result is in the instance's own space, as the bind pose was: the TLAS instance transform stays the
// instance's.

ByteAddressBuffer positions : register(t0);  // the bind pose: float3 a vertex
ByteAddressBuffer skin : register(t1);       // 4 bone indices (u8), then 4 weights (unorm8), a vertex
ByteAddressBuffer bones : register(t2);      // matrix43 a bone: the rotation's 3 rows, then the translation
RWByteAddressBuffer posed : register(u0);    // float3 a vertex

cbuffer Constants : register(b0) {
    uint vertex_count;
    uint bone_count;
    uint2 unused;
};

float3 transform(uint bone, float3 p) {
    const uint at = min(bone, bone_count - 1) * 48;
    const float3 r0 = asfloat(bones.Load3(at)), r1 = asfloat(bones.Load3(at + 12)), r2 = asfloat(bones.Load3(at + 24)),
                 t = asfloat(bones.Load3(at + 36));
    return p.x * r0 + p.y * r1 + p.z * r2 + t;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= vertex_count) return;
    const float3 p = asfloat(positions.Load3(id.x * 12));
    const uint2 s = skin.Load2(id.x * 8);
    const float4 w = float4(s.y & 0xFF, (s.y >> 8) & 0xFF, (s.y >> 16) & 0xFF, s.y >> 24) / 255.0;
    const float total = w.x + w.y + w.z + w.w;
    float3 q = 0;
    if (total <= 0.0) {
        q = transform(s.x & 0xFF, p);
    } else {
        if (w.x > 0.0) q += w.x * transform(s.x & 0xFF, p);
        if (w.y > 0.0) q += w.y * transform((s.x >> 8) & 0xFF, p);
        if (w.z > 0.0) q += w.z * transform((s.x >> 16) & 0xFF, p);
        if (w.w > 0.0) q += w.w * transform(s.x >> 24, p);
        q /= total;
    }
    posed.Store3(id.x * 12, asuint(q));
}
