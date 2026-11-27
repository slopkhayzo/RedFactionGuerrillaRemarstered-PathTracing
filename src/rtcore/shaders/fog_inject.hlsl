// The volumetric fog, pass 1 of 3: how much of the sun and of the sky reaches each froxel of a camera-aligned grid
// (fog3.x by fog3.y cells across the screen, fog2.w slices in depth from fog2.y to fog2.z metres, exponential). From
// points of the froxel each frame (fog4.x of them, Fog.Rays; kYoungRays while the froxel has fewer samples than that:
// the screen's edges as the camera turns, the first frame after a cut): a shadow ray toward a point of the sun's disk
// (sky_probe.w: the tangent of its angular radius, as the shadows'), and a ray up into a cosine-weighted direction
// (kSkyRay long: under a roof, indoors, it meets something). The points follow low-discrepancy sequences over the frames
// (R3 for the point in the froxel, R2 for the point of the sun's disk and for the sky's direction; each froxel its own
// offsets into them), so that the frames averaged cover the froxel evenly where independent random points would bunch
// up and leave gaps: the noise the average keeps. Averaged with what the froxel's centre had in the previous frame
// (reprojected, read between froxels; fog3.z: that frame's grid is there) over fog3.w frames' samples at most.
// With fog4.w (Fog.Walls) a point behind the scene's surface at its own pixel (the depth buffer's: behind a wall, the
// ground, the player) does not count: a froxel's light is that of its air the camera sees, so that one across a wall
// (a wall seen at a slant crosses many, each a few pixels wide and metres deep) does not carry the fog beyond the wall,
// lit outside, into a dark room in front of it. A frame whose points are all behind leaves the froxel as it is, but for
// one hardly seen in front of the surfaces (w, the share of its points in front, under kSeen): there they count, the
// more the less it is seen, so that the froxels behind the surfaces keep the light where they are (read between
// froxels past a silhouette, or seen through a crack).
//   u0 (RGBA16F): the sun's visibility, the sky's, the samples averaged, w the share of the points in front of the
//   surfaces (1 without Fog.Walls)
#include "fog_common.hlsli"
Texture3D<float4> fog_prev : register(t3);
RWTexture3D<float4> fog_out : register(u0);

static const float kSkyRay = 250.0;
static const uint kYoungRays = 4;  // the rays a froxel without that many samples yet takes (at most kRaysAtMost)
static const uint kRaysAtMost = 4;  // the rays a frame at most: each frame's share of the sequences
static const float kSeen = 0.1;     // (Fog.Walls) under this share of its points in front of the surfaces, a froxel takes
                                    // those behind them too

// Kronecker sequences in 32-bit fixed point, exact at any frame (Roberts' R_d: the increments 1 / phi_d^i, phi_d the
// root of x^(d+1) = x + 1): R3 for points in 3D, R2 in 2D. Frame f's ray r takes the index f kRaysAtMost + r (the
// sequences keep their spread at that stride).
static const uint3 kR3 = uint3(0xD1B54A33u, 0xABC98389u, 0x8CB92BA7u);
static const uint2 kR2 = uint2(0xC13FA9A9u, 0x91E10DA6u);
float fixed_unit(uint v) { return float(v >> 8) * (1.0 / 16777216.0); }

bool open_toward(float3 o, float3 d, float tmax) {
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 0.0;
    ray.TMax = tmax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
    RT_PROCEED(q);
    return q.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

// The view depth of the scene's surface at the screen position uv (0 to 1, y down); a great depth where there is none
// (the sky, the landmark layer).
float surface_depth(float2 uv) {
    float3 s;
    return position_at(int2(uv * float2(size)), s) ? dot(s - cam_origin.xyz, cam_forward.xyz) : 1e30;
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint3 n = froxel_count();
    if (any(id >= n)) return;
    float4 hist = 0.0;
    if (fog3.z != 0.0) {  // the froxel's centre in the previous frame's grid
        const float3 pc = cam_origin.xyz + view_ray_uv((float2(id.xy) + 0.5) / float2(n.xy)) * slice_depth(float(id.z) + 0.5);
        const float4 c = pc.x * reproject[0] + pc.y * reproject[1] + pc.z * reproject[2] + reproject[3];
        if (c.w > 1e-3) {
            const float2 ndc = c.xy / c.w;
            const float3 tc = float3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, depth_slice(c.w) / float(n.z));
            if (all(tc >= 0.0) && all(tc <= 1.0)) hist = fog_prev.SampleLevel(linear_clamp, tc, 0);
        }
    }
    const uint rays = clamp(uint(fog4.x), 1u, kRaysAtMost);
    const uint k = hist.z < float(kYoungRays) ? max(rays, kYoungRays) : rays;
    // the froxel's own offsets into the sequences (the same every frame)
    const uint h0 = hash((id.x * 73856093u) ^ (id.y * 19349663u) ^ (id.z * 83492791u));
    const uint h1 = hash(h0 + 0x9E3779B9u), h2 = hash(h1 + 0x9E3779B9u), h3 = hash(h2 + 0x9E3779B9u);
    const uint h4 = hash(h3 + 0x9E3779B9u), h5 = hash(h4 + 0x9E3779B9u), h6 = hash(h5 + 0x9E3779B9u);
    float3 l = 0.0, ta = 0.0, tb = 0.0;
    if (sun.w != 0.0) {
        l = normalize(sun.xyz);
        ta = normalize(abs(l.y) < 0.99 ? cross(l, float3(0, 1, 0)) : cross(l, float3(1, 0, 0)));
        tb = cross(l, ta);
    }
    // Fog.Walls: how much the points behind the surfaces count when a frame has only those (1 without it)
    const bool walls = fog4.w != 0.0;
    const float behind = walls ? saturate(1.0 - hist.w / kSeen) : 1.0;
    float2 all = 0.0, front = 0.0;  // the sun's and the sky's visibility summed: over all the points, over those in front
    uint nfront = 0;
    for (uint r = 0; r < k; ++r) {
        const uint i = shadow.w * kRaysAtMost + r;
        const float2 uv = (float2(id.xy) + float2(fixed_unit(h0 + i * kR3.x), fixed_unit(h1 + i * kR3.y))) / float2(n.xy);
        const float z = slice_depth(float(id.z) + fixed_unit(h2 + i * kR3.z));
        const bool seen = !walls || z <= surface_depth(uv);
        if (!seen && behind == 0.0) continue;  // (it would not count: no rays)
        const float3 p = cam_origin.xyz + view_ray_uv(uv) * z;
        float2 v = 0.0;
        if (sun.w != 0.0) {
            const float rr = sqrt(fixed_unit(h3 + i * kR2.x)) * sky_probe.w, ph = 6.2831853 * fixed_unit(h4 + i * kR2.y);
            // (smooth through glass; the clouds' shadow, GI.CloudShadows)
            v.x = light_through(p, normalize(l + ta * (rr * cos(ph)) + tb * (rr * sin(ph)))) * cloud_vis(p);
        }
        const float3 up = cosine_dir(float3(0, 1, 0), fixed_unit(h5 + i * kR2.x), fixed_unit(h6 + i * kR2.y));
        v.y = open_toward(p, up, kSkyRay) ? 1.0 : 0.0;
        all += v;
        if (seen) {
            front += v;
            ++nfront;
        }
    }
    // the running average of the samples, over fog3.w frames' at most: each frame's weighs the same whatever its rays;
    // the points in front of the surfaces when the frame has any, else all of them as much as `behind` says
    const float samples = min(hist.z + float(k), max(fog3.w, 1.0) * float(rays));
    const float a = min(float(k) / samples, 1.0);
    const float2 v = nfront ? front / float(nfront) : all / float(k);
    const float b = nfront ? a : a * behind;
    fog_out[id] = float4(lerp(hist.xy, v, b), samples, walls ? lerp(hist.w, float(nfront) / float(k), a) : 1.0);
}
