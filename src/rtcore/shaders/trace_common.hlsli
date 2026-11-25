// What the path tracer's passes share (gi_trace.hlsl, reflect_trace.hlsl): the frame's scene and textures, the camera,
// each pixel's surface from the game's depth, and the light leaving a surface a ray hits.

#define RT_INSTANCE_TABLE instances  // (rt_query.hlsli: which glass geometries are translucent)
#include "rt_query.hlsli"
RaytracingAccelerationStructure scene : register(t5);
// per InstanceID: x the page (bits 0-7) and the translucent glass geometries (bits 8-31: geometry g at bit 8 + g; tlas.cpp),
// positions, indices (bit 31: 16-bit), table
StructuredBuffer<uint4> instances : register(t6);
ByteAddressBuffer pages[64] : register(t0, space1);  // the geometry pool
// The meshes' materials (scene.h): a slot of 32 entries a mesh (its index in its geometry table's header), an entry per
// material map entry: x bits 0-15 its diffuse map's texture + 1 (0: none), bit 31 set when the entry is known; y its
// tiling (half2); z its colour's r and g (half2); w its colour's b and its self-illumination (half2).
static const uint kMaterialSlots = 16384;
static const uint kMaterialEntries = 32;
static const uint kMaterialTextures = 4096;
StructuredBuffer<uint4> materials : register(t7);
Texture2D<float4> material_textures[4096] : register(t0, space2);
SamplerState material_sampler : register(s1);  // linear, wrapping
Texture2D<float> depth : register(t0);
Texture2D<float4> radiance : register(t1);   // the game's lit colour of the previous frame (its units), the opaque scene
Texture2D<float> prev_depth : register(t2);  // that frame's depth
#ifndef TRACE_NO_TRACED  // (the fog's passes have their own u0)
RWTexture2D<float4> traced : register(u0);
#endif

cbuffer Frame : register(b0) {
    float4 cam_right;     // xyz: camera axes and position (render-offset space)
    float4 cam_up;
    float4 cam_forward;
    float4 cam_origin;
    float4 proj;          // P00, P11, P22, P32 of the game's projection
    float4 range;         // x: the main scene's depth range end
    float4 sun;           // xyz: toward the sun; w: 1 when there is a sun
    uint2 size;
    float tolerance;
    uint flags;           // 2: reversed depth; 16: the previous frame's lit colour and depth are there
    uint4 shadow;         // w: frame
    float4 reproject[4];  // render-offset space now -> the previous frame's clip space (row-vector)
    float4 prev;          // the previous frame's P22, P32, depth range end
    float4 ambient;       // the game's ambient colour (the sky above), in its shaders' units
    float4 back_ambient;  // below the horizon
    float4 sun_color;     // the sun's light colour, the same units
    float4 gi;            // GI: rays a pixel, their length (m), the albedo of what they hit, max frames averaged;
                          // reflections: the lobe's exponent, the rays' length, the albedo, max frames
    float4 gi2;           // x: the bounced light's scale (GI.Bounce), y: the sky's as a light (GI.Sky), z: the least
                          // albedo sunlit surfaces bounce the sun with (GI.SunBounceAlbedo; 0: as drawn; sun_bounce)
    float4 world;         // xyz: the render offset (render-offset space + it = the world); w: 1 with the radiance cache
    float4 sky_probe;     // x: 1 with the drawn sky; y: the cosine of the angle about the sun it leaves out; z: GI.SkyDrawn;
                          // w: the tangent of the sun's angular radius (Shadows.SunAngle)
    // the volumetric fog (fog_common.hlsli)
    float4 fog;           // the game's fog: its density a, height falloff b, start c; w 1
    float4 fog_color;     // rgb: the game's fogColor; w: the sun's share of the fog's light (Fog.SunShare)
    float4 fog2;          // x: the scattering's anisotropy (Fog.Anisotropy); y, z: the froxel grid's near and far depth (m);
                          // w: its slices
    float4 fog3;          // x, y: froxels across the screen; z: 1 when the previous frame's grid is there; w: frames averaged
    float4 glass;         // the clear glass's reflections (glass_light.hlsl): x its reflectance at normal incidence
                          // (Reflections.GlassReflectance), y 1 when they are on (Reflections.Glass)
    float4 probe;         // xy: where the probe aims (probe.hlsl), fractions of the screen (Test.ProbePixel)
    float4 terrain;       // rgb: the albedo of a surface whose material is not known, the terrain (GI.TerrainAlbedo)
    float4 upscale;       // DLSS (dlss.h): xy the GI's render size (0: the frame's), zw the sample's offset from each of its
                          // pixels' centres (its pixels, y down); gi_size, gi_source
};

static const float PI = 3.14159265;

// The game's fog's transmittance from the camera to a point v away from it (render-offset space, y up; fog_common.hlsli).
// 1 without the fog's constants (Fog.Enabled=0).
float fog_transmittance(float3 v) {
    const float d = length(v);
    const float x = fog.y * v.y;
    const float h = (abs(v.y) > 0.01 && abs(x) > 1e-6) ? (1.0 - exp(-x)) / x : 1.0;
    return exp2(-max(h * fog.x * d - fog.z, 0.0));
}

float3 view_ray(int2 p) {
    const float2 ndc = float2((p.x + 0.5) / size.x * 2.0 - 1.0, 1.0 - (p.y + 0.5) / size.y * 2.0);
    return (ndc.x / proj.x) * cam_right.xyz + (ndc.y / proj.y) * cam_up.xyz + cam_forward.xyz;
}

bool position_at(int2 p, out float3 pos) {
    pos = 0;
    if (any(p < 0) || any(p >= int2(size))) return false;
    const float d = depth[uint2(p)] / range.x;
    const bool reversed = (flags & 2) != 0;
    if (reversed ? d <= 0.0 : d >= 1.0) return false;
    pos = cam_origin.xyz + view_ray(p) * (proj.w / (d - proj.z));
    return true;
}

// A pixel's surface: its position, and its normal from the neighbouring depths (the side with the smaller step), facing
// the camera. False for the sky and the landmark layer.
bool surface_at(int2 px, out float3 p, out float3 n) {
    n = float3(0, 1, 0);
    if (!position_at(px, p)) return false;
    float3 l, r, u, d;
    const bool hl = position_at(px + int2(-1, 0), l), hr = position_at(px + int2(1, 0), r);
    const bool hu = position_at(px + int2(0, -1), u), hd = position_at(px + int2(0, 1), d);
    const float3 dx = (hr && (!hl || length(r - p) < length(p - l))) ? r - p : (hl ? p - l : float3(1, 0, 0));
    const float3 dy = (hd && (!hu || length(d - p) < length(p - u))) ? d - p : (hu ? p - u : float3(0, 0, 1));
    n = normalize(cross(dy, dx));
    if (dot(n, cam_origin.xyz - p) < 0.0) n = -n;
    return true;
}

// The GI's size: the frame's, or with DLSS upscaling (a mode below DLAA) its render size, the top-left part of the
// GI's textures.
uint2 gi_size() { return upscale.x > 0.0 ? uint2(upscale.xy) : size; }

// The frame's pixel a GI pixel traces from: itself, or with DLSS upscaling the one under the pixel's jittered point
// (rr_guides.hlsl picks the same).
int2 gi_source(uint2 q) {
    if (upscale.x <= 0.0) return int2(q);
    const float2 at = (float2(q) + 0.5 + upscale.zw) * (float2(size) / upscale.xy);
    return clamp(int2(floor(at)), int2(0, 0), int2(size) - 1);
}

uint hash(uint x) {  // lowbias32
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float unit(uint h) { return float(h & 0xFFFFFFu) / 16777216.0; }

#include "radiance_cache.hlsli"
#include "sky_probe.hlsli"

// The sky's radiance along dir: the sky the game draws there (sky_probe.hlsli, GI.SkyDrawn), else its ambient colour over
// the upper hemisphere (an open upward surface gets exactly it), fading to the back ambient below the horizon, over pi.
float3 sky_radiance(float3 dir) {
    return sky_drawn(dir, (dir.y >= 0.0 ? ambient.rgb : lerp(back_ambient.rgb, ambient.rgb, saturate(1.0 + dir.y))) / PI, ambient.rgb / PI);
}

// A cosine-weighted direction about n, from two uniform numbers.
float3 cosine_dir(float3 n, float u1, float u2) {
    const float3 tn = normalize(abs(n.y) < 0.99 ? cross(n, float3(0, 1, 0)) : cross(n, float3(1, 0, 0)));
    const float3 bn = cross(n, tn);
    const float rr = sqrt(u1), phi = 6.2831853 * u2;
    return normalize(tn * (rr * cos(phi)) + bn * (rr * sin(phi)) + n * sqrt(max(1.0 - u1, 0.0)));
}

// The light leaving a hit toward the ray's origin, taken from what the game drew there in the previous frame: the
// hit reprojected into that frame, where its depth agrees (otherwise the game did not see it there: false). The game's
// colour is albedo times the irradiance in its units, so the radiance is that colour over pi (the sky's is the ambient
// over pi): its sun, local lights, ambient (this GI: a bounce more each frame) and glow come with it.
bool seen_radiance(float3 hp, out float3 L) {
    L = 0;
    if ((flags & 16) == 0) return false;
    const float4 c = hp.x * reproject[0] + hp.y * reproject[1] + hp.z * reproject[2] + reproject[3];
    if (c.w <= 1e-3) return false;
    const float2 ndc = c.xy / c.w;
    const float2 pix = float2((ndc.x + 1.0) * 0.5 * size.x, (1.0 - ndc.y) * 0.5 * size.y);
    if (any(pix < 0.0) || any(pix >= float2(size))) return false;
    const uint2 q = uint2(pix);
    const bool reversed = (flags & 2) != 0;
    const float pd = prev_depth[q] / prev.z;
    if (reversed ? pd <= 0.0 : pd >= 1.0) return false;
    const float pz = prev.y / (pd - prev.x);
    if (abs(pz - c.w) > max(c.w * 0.02, 0.1)) return false;
    L = max(radiance[q].rgb, 0.0) / PI;
    return true;
}

// How much of the light from direction d gets to o: 0 behind anything opaque, the translucent glass's share for each
// pane crossed (RT_THROUGH, smooth).
float light_through(float3 o, float3 d) {
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 0.0;
    ray.TMax = 5000.0;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
    float through = 1.0;
    RT_THROUGH(q, through);
    return through;
}

bool lit_toward(float3 o, float3 d) {
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 0.0;
    ray.TMax = 5000.0;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
    RT_PROCEED(q);
    return q.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

uint index_at(uint page, uint base, bool short_indices, uint k) {
    if (!short_indices) return pages[NonUniformResourceIndex(page)].Load(base + k * 4);
    const uint a = base + k * 2;
    const uint w = pages[NonUniformResourceIndex(page)].Load(a & ~3u);
    return (a & 2u) ? (w >> 16) : (w & 0xFFFFu);
}

// A submesh's geometry table (scene.cpp add_mesh): a header (its texture coordinates' offset from its positions, all bits
// set without; its mesh's material slot), then per BLAS geometry its first index and its material map entry.
// The hit triangle's normal in world space, facing where the ray came from; -dir when the instance has no geometry.
float3 hit_normal(uint instance, uint geometry, uint primitive, float3x4 to_world, float3 dir) {
    const uint4 g = instances[instance];
    const uint page = g.x & 0xFFu;
    if (page >= 64u) return -dir;
    const uint first = pages[NonUniformResourceIndex(page)].Load(g.w + 8 + geometry * 8);  // (after the header)
    const bool short_indices = (g.z & 0x80000000u) != 0;
    const uint ibase = g.z & 0x7FFFFFFFu;
    const uint k = first + primitive * 3;
    const uint i0 = index_at(page, ibase, short_indices, k), i1 = index_at(page, ibase, short_indices, k + 1),
               i2 = index_at(page, ibase, short_indices, k + 2);
    const float3 v0 = asfloat(pages[NonUniformResourceIndex(page)].Load3(g.y + i0 * 12));
    const float3 v1 = asfloat(pages[NonUniformResourceIndex(page)].Load3(g.y + i1 * 12));
    const float3 v2 = asfloat(pages[NonUniformResourceIndex(page)].Load3(g.y + i2 * 12));
    // the edges in the world first: right under any scale (a normal would need the inverse transpose)
    const float3x3 m = (float3x3)to_world;
    float3 n = cross(mul(m, v1 - v0), mul(m, v2 - v0));
    if (dot(n, n) < 1e-20) return -dir;
    n = normalize(n);
    return dot(n, dir) < 0.0 ? n : -n;
}

// What a ray hit, for its light: the triangle's normal facing where the ray came from, the material's albedo there (its
// diffuse map at the hit's texture coordinates times its colour; gi.z, a plain grey, where the mesh has no texture
// coordinates, its material is not known or has no texture) and its self-illumination.
struct Surface {
    float3 n;
    float3 albedo;
    float3 mean;  // the material's mean albedo (its texture's last mip times its colour; the albedo without a texture)
    float glow;
    bool known;  // its material is known (the plugin sent it; not the terrain's, which draws without a material map)
};

// `bary`: the hit's barycentrics (CommittedTriangleBarycentrics); `width`: the ray's footprint there (m), for the
// texture's level of detail (ray cones: Akenine-Moller et al. 2021, "Improved Shader and Texture Level of Detail Using
// Ray Cones": half the log2 of the triangle's texel area over its world area, plus the log2 of the footprint over the
// cosine of the hit).
Surface hit_surface(uint instance, uint geometry, uint primitive, float2 bary, float3x4 to_world, float3 dir, float width) {
    Surface s;
    s.n = -dir;
    s.albedo = gi.z;
    s.mean = gi.z;
    s.glow = 0.0;
    s.known = false;
    const uint4 g = instances[instance];
    const uint page = g.x & 0xFFu;
    if (page >= 64u) return s;
    const uint2 head = pages[NonUniformResourceIndex(page)].Load2(g.w);
    const uint2 geo = pages[NonUniformResourceIndex(page)].Load2(g.w + 8 + geometry * 8);  // first index, material entry
    const bool short_indices = (g.z & 0x80000000u) != 0;
    const uint ibase = g.z & 0x7FFFFFFFu;
    const uint k = geo.x + primitive * 3;
    const uint i0 = index_at(page, ibase, short_indices, k), i1 = index_at(page, ibase, short_indices, k + 1),
               i2 = index_at(page, ibase, short_indices, k + 2);
    const float3 v0 = asfloat(pages[NonUniformResourceIndex(page)].Load3(g.y + i0 * 12));
    const float3 v1 = asfloat(pages[NonUniformResourceIndex(page)].Load3(g.y + i1 * 12));
    const float3 v2 = asfloat(pages[NonUniformResourceIndex(page)].Load3(g.y + i2 * 12));
    const float3x3 m = (float3x3)to_world;
    float3 n = cross(mul(m, v1 - v0), mul(m, v2 - v0));
    const float area2 = length(n);  // twice the triangle's area in the world
    if (!(area2 > 1e-10)) return s;
    n /= area2;
    s.n = dot(n, dir) < 0.0 ? n : -n;
    if (head.y >= kMaterialSlots || geo.y >= kMaterialEntries) return s;
    const uint4 mat = materials[head.y * kMaterialEntries + geo.y];
    if ((mat.x & 0x80000000u) == 0u) {  // not known: the terrain (its own shader), what has no material yet
        s.albedo = terrain.rgb;
        s.mean = terrain.rgb;
        return s;
    }
    s.known = true;
    s.glow = f16tof32(mat.w >> 16);
    const uint tex = mat.x & 0xFFFFu;
    if (tex == 0u || tex > kMaterialTextures || head.x == 0xFFFFFFFFu) return s;
    const float2 tiling = float2(f16tof32(mat.y & 0xFFFFu), f16tof32(mat.y >> 16));
    const float3 color = float3(f16tof32(mat.z & 0xFFFFu), f16tof32(mat.z >> 16), f16tof32(mat.w & 0xFFFFu));
    const uint uvBase = g.y + head.x;
    const float2 t0 = asfloat(pages[NonUniformResourceIndex(page)].Load2(uvBase + i0 * 8)) * tiling;
    const float2 t1 = asfloat(pages[NonUniformResourceIndex(page)].Load2(uvBase + i1 * 8)) * tiling;
    const float2 t2 = asfloat(pages[NonUniformResourceIndex(page)].Load2(uvBase + i2 * 8)) * tiling;
    const float2 uv = t0 + bary.x * (t1 - t0) + bary.y * (t2 - t0);
    uint w, h, levels;
    material_textures[NonUniformResourceIndex(tex - 1u)].GetDimensions(0, w, h, levels);
    const float2 d1 = t1 - t0, d2 = t2 - t0;
    const float texels2 = abs(d1.x * d2.y - d1.y * d2.x) * float(w) * float(h);  // twice its area in texels
    float lod = 0.5 * log2(max(texels2, 1e-12) / area2) + log2(max(width, 1e-5) / max(abs(dot(n, dir)), 0.05));
    lod = clamp(lod, 0.0, float(levels) - 1.0);
    const float4 texel = material_textures[NonUniformResourceIndex(tex - 1u)].SampleLevel(material_sampler, uv, lod);
    const float3 a = texel.rgb * color;
    if (all(a == a)) s.albedo = saturate(a);
    const float3 meanTexel =
        material_textures[NonUniformResourceIndex(tex - 1u)].SampleLevel(material_sampler, uv, float(levels) - 1.0).rgb * color;
    s.mean = all(meanTexel == meanTexel) ? saturate(meanTexel) : s.albedo;
    return s;
}

// The light a diffuse surface sends back under irradiance `e` (the game's units): the game lights a self-illuminated
// material toward albedo x 4 instead of its ambient (rfg-standard: lerp(ambient light, 4, Self_Illumination)), here
// toward it instead of all of its light.
float3 surface_radiance(Surface s, float3 e) {
    return s.albedo / PI * lerp(e, float3(4.0, 4.0, 4.0), saturate(s.glow));
}

// The sun's irradiance at a surface point (a shadow ray toward it).
float3 sun_at(float3 hp, float3 nh) {
    if (sun.w == 0.0) return 0.0;
    const float3 to_sun = normalize(sun.xyz);
    const float c = dot(nh, to_sun);
    return (c > 0.0 && lit_toward(hp, to_sun)) ? sun_color.rgb * c : 0.0;
}

// Frosted glass (RTQ_FROSTED): whether a hit is on a translucent pane (its instance table entry's bits 8-31).
bool hit_translucent(uint instance, uint geometry) {
    if (!RTQ_FROSTED || geometry >= 24u) return false;
    const uint x = instances[instance].x;
    return (x & 0xFFu) < 64u && (((x >> 8) >> geometry) & 1u) != 0u;
}

// The light a frosted pane sends toward a ray that hit it: what it diffuses through itself of the light on its far side
// (the sun beyond it, through a shadow ray; the sky's ambient for its far face), tinted by its pattern's hue, and what it
// reflects of its near side's (its texture as the albedo). The pattern's brightness is its drawn look, not how much light
// it passes (the patterned glass's is a dark teal): only its hue tints the light, half way. Not the game's colour of the
// pane nor the radiance cache's: those are its look, lit by the game's fake glow.
static const float kPaneTransmission = 0.6;
static const float kPaneTint = 0.5;
static const float kPaneReflection = 0.1;
float3 pane_radiance(float3 hp, Surface s, float t) {
    const float3 nf = -s.n;
    const float3 pf = hp + nf * (kPaneSelf + 0.05);
    const float3 pn = hp + s.n * (0.02 + 0.002 * t);
    const float3 far_sky = gi2.y * lerp(back_ambient.rgb, ambient.rgb, saturate(nf.y * 0.5 + 0.5));
    const float3 near_sky = gi2.y * lerp(back_ambient.rgb, ambient.rgb, saturate(s.n.y * 0.5 + 0.5));
    const float peak = max(max(s.albedo.r, s.albedo.g), max(s.albedo.b, 1e-3));
    const float3 tint = lerp(float3(1.0, 1.0, 1.0), saturate(s.albedo / peak), kPaneTint);
    return (kPaneTransmission * tint * (sun_at(pf, nf) + far_sky) + kPaneReflection * s.albedo * (sun_at(pn, s.n) + near_sky)) / PI;
}

// The sky's light at a surface as a mirror shows it: the game's ambient for its normal, times the share of four fixed
// rays about the normal (45 degrees off it) that get out. Fixed, not random: the reflection is not averaged over frames.
// (Unoccluded, every wall and floor of a room behind the camera showed flat blue in a window.)
float3 sky_seen(float3 hp, float3 n) {
    const float3 tn = normalize(abs(n.y) < 0.99 ? cross(n, float3(0, 1, 0)) : cross(n, float3(1, 0, 0)));
    const float3 bn = cross(n, tn);
    float open = 0.0;
    [unroll] for (uint i = 0; i < 4u; ++i) {
        const float a = 1.5707963 * (float(i) + 0.5);
        if (lit_toward(hp, normalize(n + tn * cos(a) + bn * sin(a)))) open += 0.25;
    }
    return open * gi2.y * lerp(back_ambient.rgb, ambient.rgb, saturate(n.y * 0.5 + 0.5));
}

// The light a mirror shows of a surface the game did not draw in the previous frame (mirror_radiance, the probe):
// `cached`/`cacheL` the radiance cache's radiance there. Not known (the terrain): the cache's colour where it has it.
// Known: where the cache has it, the larger of the sun on it (sharp shadows) and the cell's light (its colour over the
// material's mean albedo: the irradiance the game drew it with, the bounce and the sky as seen there); elsewhere the sun
// and the sky it sees (sky_seen).
float3 mirror_shade(Surface s, float3 hp, bool cached, float3 cacheL) {
    if (!s.known && cached) return cacheL;
    const float3 direct = sun_at(hp, s.n);
    const float3 e = s.known && cached ? max(direct, PI * cacheL / max(s.mean, 0.02)) : direct + sky_seen(hp, s.n);
    return surface_radiance(s, e);
}

// The radiance a mirror shows of the surface a ray from `origin` along `dir` hit at `t`: what the game drew there in the
// previous frame where it saw it; otherwise the surface itself (hit_surface: its texture where its material has one), lit
// as mirror_shade says, without a random ray: the reflection stays still. `cone`: the ray's footprint at its origin (m)
// and its spread (radians) for the texture's level of detail.
float3 mirror_radiance(float3 origin, float3 dir, float t, uint instance, uint geometry, uint primitive, float2 bary,
                       float3x4 to_world, float2 cone) {
    if (hit_translucent(instance, geometry))
        return pane_radiance(origin + dir * t, hit_surface(instance, geometry, primitive, bary, to_world, dir, cone.x + cone.y * t), t);
    float3 seen;
    if (seen_radiance(origin + dir * t, seen)) return seen;
    const Surface s = hit_surface(instance, geometry, primitive, bary, to_world, dir, cone.x + cone.y * t);
    const bool cached = cache_radiance(origin + dir * t, s.n, seen);
    return mirror_shade(s, origin + dir * t + s.n * (0.02 + 0.002 * t), cached, seen);
}

// The footprint of the GI's and the glossy reflections' rays for the texture's level of detail: a wide cone (their lobes
// are wide; a blurred texture keeps their few rays from flickering).
static const float kDiffuseSpread = 0.15;

// What a clear pane passes where a ray crosses it: lerp(1, colour, alpha) of its material's texel there (1 without one).
float3 pane_pass(uint instance, uint geometry, uint primitive, float2 bary, float3x4 to_world, float3 dir, float width) {
    const uint4 g = instances[instance];
    const uint page = g.x & 0xFFu;
    if (page >= 64u) return 1.0;
    const uint2 head = pages[NonUniformResourceIndex(page)].Load2(g.w);
    const uint2 geo = pages[NonUniformResourceIndex(page)].Load2(g.w + 8 + geometry * 8);
    if (head.y >= kMaterialSlots || geo.y >= kMaterialEntries || head.x == 0xFFFFFFFFu) return 1.0;
    const uint4 mat = materials[head.y * kMaterialEntries + geo.y];
    const uint tex = mat.x & 0xFFFFu;
    if ((mat.x & 0x80000000u) == 0u || tex == 0u || tex > kMaterialTextures) return 1.0;
    const bool short_indices = (g.z & 0x80000000u) != 0;
    const uint ibase = g.z & 0x7FFFFFFFu;
    const uint k = geo.x + primitive * 3;
    const uint i0 = index_at(page, ibase, short_indices, k), i1 = index_at(page, ibase, short_indices, k + 1),
               i2 = index_at(page, ibase, short_indices, k + 2);
    const float3 v0 = asfloat(pages[NonUniformResourceIndex(page)].Load3(g.y + i0 * 12));
    const float3 v1 = asfloat(pages[NonUniformResourceIndex(page)].Load3(g.y + i1 * 12));
    const float3 v2 = asfloat(pages[NonUniformResourceIndex(page)].Load3(g.y + i2 * 12));
    const float3x3 m = (float3x3)to_world;
    float3 n = cross(mul(m, v1 - v0), mul(m, v2 - v0));
    const float area2 = length(n);
    if (!(area2 > 1e-10)) return 1.0;
    n /= area2;
    const float2 tiling = float2(f16tof32(mat.y & 0xFFFFu), f16tof32(mat.y >> 16));
    const float3 color = float3(f16tof32(mat.z & 0xFFFFu), f16tof32(mat.z >> 16), f16tof32(mat.w & 0xFFFFu));
    const uint uvBase = g.y + head.x;
    const float2 t0 = asfloat(pages[NonUniformResourceIndex(page)].Load2(uvBase + i0 * 8)) * tiling;
    const float2 t1 = asfloat(pages[NonUniformResourceIndex(page)].Load2(uvBase + i1 * 8)) * tiling;
    const float2 t2 = asfloat(pages[NonUniformResourceIndex(page)].Load2(uvBase + i2 * 8)) * tiling;
    const float2 uv = t0 + bary.x * (t1 - t0) + bary.y * (t2 - t0);
    uint w, h, levels;
    material_textures[NonUniformResourceIndex(tex - 1u)].GetDimensions(0, w, h, levels);
    const float2 d1 = t1 - t0, d2 = t2 - t0;
    const float texels2 = abs(d1.x * d2.y - d1.y * d2.x) * float(w) * float(h);
    float lod = 0.5 * log2(max(texels2, 1e-12) / area2) + log2(max(width, 1e-5) / max(abs(dot(n, dir)), 0.05));
    lod = clamp(lod, 0.0, float(levels) - 1.0);
    const float4 texel = material_textures[NonUniformResourceIndex(tex - 1u)].SampleLevel(material_sampler, uv, lod);
    const float3 c = saturate(texel.rgb * color);
    const float a = saturate(texel.a);
    const float3 pass = lerp(float3(1.0, 1.0, 1.0), c, a);
    return all(pass == pass) ? pass : float3(1.0, 1.0, 1.0);
}

// counts a pane at distance d along the ray unless one within kPaneThickness of it was (its other face)
bool new_pane(float d, inout float4 seen, inout uint count) {
    if (abs(d - seen.x) < kPaneThickness || abs(d - seen.y) < kPaneThickness || abs(d - seen.z) < kPaneThickness ||
        abs(d - seen.w) < kPaneThickness)
        return false;
    if (count == 0u) seen.x = d;
    else if (count == 1u) seen.y = d;
    else if (count == 2u) seen.z = d;
    else seen.w = d;
    ++count;
    return true;
}

// The clear panes a ray crosses (GI.WindowTint, the pass's flags & 64), so that the light it brings back through a window
// takes the window's colour, as the sun's does (sun_tint.hlsl): gathered in the ray's own traversal (RT_PROCEED_PANES:
// the clear glass candidates it passes, the glass-only instances' and the clear glass geometries of the others, all
// non-opaque in the rays' scene: tlas.cpp), each pane once (its faces within kPaneThickness), at most 4, with what it lets
// through where the ray crosses it (pane_pass); then panes_through: the product of those before the ray's hit. Until
// 0.1.17 glass_through traced two more queries along each ray for them: the GI a quarter dearer.
#define RTQ_TINT ((flags & 64u) != 0u)
struct Panes {
    float4 t;   // their distances along the ray
    float3 c0;  // what each lets through
    float3 c1;
    float3 c2;
    float3 c3;
    uint n;
};

Panes panes_none() {
    Panes p;
    p.t = float4(-1e30, -1e30, -1e30, -1e30);
    p.c0 = float3(1.0, 1.0, 1.0);
    p.c1 = p.c0;
    p.c2 = p.c0;
    p.c3 = p.c0;
    p.n = 0u;
    return p;
}

// a pane at d not counted yet (none within kPaneThickness of it: its other face), with room for it
bool panes_new(Panes p, float d) {
    return p.n < 4u && abs(d - p.t.x) >= kPaneThickness && abs(d - p.t.y) >= kPaneThickness &&
           abs(d - p.t.z) >= kPaneThickness && abs(d - p.t.w) >= kPaneThickness;
}

void panes_add(inout Panes p, float d, float3 c) {
    if (p.n == 0u) {
        p.t.x = d;
        p.c0 = c;
    } else if (p.n == 1u) {
        p.t.y = d;
        p.c1 = c;
    } else if (p.n == 2u) {
        p.t.z = d;
        p.c2 = c;
    } else {
        p.t.w = d;
        p.c3 = c;
    }
    ++p.n;
}

// What the panes before `tmax` (the ray's hit, or its length) let through of the light along the ray: a candidate the
// traversal reported before it found a nearer hit is left out.
float3 panes_through(Panes p, float tmax) {
    float3 c = float3(1.0, 1.0, 1.0);
    if (p.n > 0u && p.t.x < tmax) c *= p.c0;
    if (p.n > 1u && p.t.y < tmax) c *= p.c1;
    if (p.n > 2u && p.t.z < tmax) c *= p.c2;
    if (p.n > 3u && p.t.w < tmax) c *= p.c3;
    return c;
}

// RT_PROCEED (rt_query.hlsli) that also gathers the clear panes the ray passes into `panes` (a Panes from panes_none),
// with RTQ_TINT; `spread`: the ray's cone, for the texel's level of detail at a pane (spread x its distance).
#define RT_PROCEED_PANES(q, panes, spread)                                                                        \
    while (q.Proceed()) {                                                                                         \
        const uint rtq_c = q.CandidateInstanceContributionToHitGroupIndex();                                      \
        const uint rtq_g = q.CandidateGeometryIndex();                                                            \
        if (rtq_g >= 23u || !((rtq_c >> rtq_g) & 1u)) {                                                           \
            q.CommitNonOpaqueTriangleHit();                                                                       \
        } else if (RTQ_TRANSLUCENT(q, rtq_c, rtq_g)) {                                                            \
            RTQ_TRANSLUCENT_STEP(q)                                                                               \
        } else if (RTQ_TINT) {                                                                                    \
            const float rtq_d = q.CandidateTriangleRayT();                                                        \
            if (panes_new(panes, rtq_d))                                                                          \
                panes_add(panes, rtq_d,                                                                           \
                          pane_pass(q.CandidateInstanceID(), rtq_g, q.CandidatePrimitiveIndex(),                  \
                                    q.CandidateTriangleBarycentrics(), q.CandidateObjectToWorld3x4(),             \
                                    q.WorldRayDirection(), (spread) * rtq_d));                                    \
        }                                                                                                         \
    }

// The light leaving a surface a second-bounce ray hit: what the game drew there where it saw it (in the previous frame,
// else in the radiance cache); otherwise the surface itself (hit_surface) under the sun and the sky it sees (one ray,
// the end of the path).
float3 shade_hit2(float3 origin, float3 dir, float t, uint instance, uint geometry, uint primitive, float2 bary, float3x4 to_world,
                  uint seed) {
    if (hit_translucent(instance, geometry))
        return pane_radiance(origin + dir * t, hit_surface(instance, geometry, primitive, bary, to_world, dir, kDiffuseSpread * t), t);
    float3 seen;
    if (seen_radiance(origin + dir * t, seen)) return seen;
    const float3 nh = hit_normal(instance, geometry, primitive, to_world, dir);
    if (cache_radiance(origin + dir * t, nh, seen)) return seen;
    const Surface s = hit_surface(instance, geometry, primitive, bary, to_world, dir, kDiffuseSpread * t);
    const float3 hp = origin + dir * t + s.n * (0.02 + 0.002 * t);
    float3 e = sun_at(hp, s.n);
    if (gi2.y > 0.0) {
        const uint s1 = hash(seed), s2 = hash(s1 + 0x9E3779B9u);
        const float3 d = cosine_dir(s.n, unit(s1), unit(s2));
        if (lit_toward(hp, d)) e += gi2.y * PI * sky_radiance(d);  // cosine-weighted: E = pi L
    }
    return surface_radiance(s, e);
}

// The radiance leaving the surface a ray from `origin` along `dir` hit at `t`: what the game drew there in the previous
// frame where it saw it (its sun, lights, glow and GI: the bounces before, a frame each), or earlier as the world
// radiance cache keeps it (radiance_cache.hlsli: out of view now); otherwise the surface itself (hit_surface: its
// texture's colour where its material has one) under the sun (a shadow ray) and what one cosine-weighted ray from it
// brings back (`seed`): the sky's radiance where it gets out, the light of the surface it meets otherwise (shade_hit2: a
// second bounce). Never the whole sky regardless of what is around it: lit that way, every hidden or off-screen wall and
// ceiling lit interiors with a flat ambient light.
// GI.SunBounceAlbedo (gi2.z): the game's textures are authored dark under a very bright sun (its terrain sends on about
// 0.03 of the sun it gets, where real ground sends on 0.15 to 0.3), so what lights the shade beside sunlit ground, its
// bounce, came out far weaker than the game's own fill. The light a sunlit surface sends on is scaled as if its material
// (its mean albedo; the terrain's: GI.TerrainAlbedo) were no darker than gi2.z, at most 32 times (the terrain's mean is
// 0.014: at most 8 times stopped its floor at 0.11); 1 for a surface in shade (its light, the sky's and the bounces',
// stays as traced: the bounces' feedback through the game's colour stays where it was) and with gi2.z 0. Whether the sun reaches it: a ray toward the sun from just before the hit (its own
// surface stops it when it faces away).
float sun_bounce_scale(float mean) {
    return gi2.z > mean ? min(gi2.z / max(mean, 1e-3), 32.0) : 1.0;
}

// The mean albedo's luminance of what a ray hit, as hit_surface gives it (s.mean), without the triangle: its material's
// texture's last mip times its colour, the terrain's GI.TerrainAlbedo, gi.z without a material or a texture.
float hit_mean(uint instance, uint geometry) {
    const float3 lw = float3(0.2126, 0.7152, 0.0722);
    const uint4 g = instances[instance];
    const uint page = g.x & 0xFFu;
    if (page >= 64u) return gi.z;
    const uint2 head = pages[NonUniformResourceIndex(page)].Load2(g.w);
    const uint2 geo = pages[NonUniformResourceIndex(page)].Load2(g.w + 8 + geometry * 8);
    if (head.y >= kMaterialSlots || geo.y >= kMaterialEntries) return gi.z;
    const uint4 mat = materials[head.y * kMaterialEntries + geo.y];
    if ((mat.x & 0x80000000u) == 0u) return dot(terrain.rgb, lw);
    const uint tex = mat.x & 0xFFFFu;
    if (tex == 0u || tex > kMaterialTextures || head.x == 0xFFFFFFFFu) return gi.z;
    const float3 color = float3(f16tof32(mat.z & 0xFFFFu), f16tof32(mat.z >> 16), f16tof32(mat.w & 0xFFFFu));
    uint w, h, levels;
    material_textures[NonUniformResourceIndex(tex - 1u)].GetDimensions(0, w, h, levels);
    const float3 m = material_textures[NonUniformResourceIndex(tex - 1u)].SampleLevel(material_sampler, float2(0.5, 0.5),
                                                                                      float(levels) - 1.0).rgb * color;
    return all(m == m) ? dot(saturate(m), lw) : gi.z;
}

// The scale for a hit the game drew (or the radiance cache holds): its material first (no ray for one bright enough),
// then whether the sun reaches it.
float sun_bounce(float3 hp, float3 dir, float t, uint instance, uint geometry) {
    if (gi2.z <= 0.0 || sun.w == 0.0) return 1.0;
    const float mean = hit_mean(instance, geometry);
    if (mean >= gi2.z) return 1.0;
    if (!lit_toward(hp - dir * (0.02 + 0.002 * t), normalize(sun.xyz))) return 1.0;
    return sun_bounce_scale(mean);
}

float3 shade_hit(float3 origin, float3 dir, float t, uint instance, uint geometry, uint primitive, float2 bary, float3x4 to_world,
                 uint seed) {
    if (hit_translucent(instance, geometry))
        return pane_radiance(origin + dir * t, hit_surface(instance, geometry, primitive, bary, to_world, dir, kDiffuseSpread * t), t);
    float3 seen;
    if (seen_radiance(origin + dir * t, seen)) return seen * sun_bounce(origin + dir * t, dir, t, instance, geometry);
    const float3 nh = hit_normal(instance, geometry, primitive, to_world, dir);
    if (cache_radiance(origin + dir * t, nh, seen))  // seen before, out of view now
        return seen * sun_bounce(origin + dir * t, dir, t, instance, geometry);
    const Surface s = hit_surface(instance, geometry, primitive, bary, to_world, dir, kDiffuseSpread * t);
    const float3 hp = origin + dir * t + s.n * (0.02 + 0.002 * t);
    float3 e = sun_at(hp, s.n) * sun_bounce_scale(dot(s.mean, float3(0.2126, 0.7152, 0.0722)));
    const uint s1 = hash(seed), s2 = hash(s1 + 0x9E3779B9u);
    const float3 d = cosine_dir(s.n, unit(s1), unit(s2));
    RayDesc ray;
    ray.Origin = hp;
    ray.Direction = d;
    ray.TMin = 0.0;
    ray.TMax = gi.y;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
    RT_PROCEED(q);
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        e += gi2.y * PI * sky_radiance(d);  // cosine-weighted: E = pi L
    else
        e += PI * shade_hit2(hp, d, q.CommittedRayT(), q.CommittedInstanceID(), q.CommittedGeometryIndex(),
                             q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(), q.CommittedObjectToWorld3x4(),
                             hash(s2 ^ 0x27D4EB2Fu));
    return surface_radiance(s, e);
}
