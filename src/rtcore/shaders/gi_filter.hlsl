// Global illumination, pass 3 of 3: the accumulated irradiance averaged over a neighbourhood (radius gi_radius,
// weighted by how close each neighbour's surface is in view depth, so that edges stay put) into the irradiance texture
// the game's patched lit shaders read at t14 as their ambient colour (render/gi.h in the plugin).

Texture2D<float4> history : register(t0);
Texture2D<float> depth : register(t1);
RWTexture2D<float4> irradiance : register(u0);

cbuffer Frame : register(b0) {
    float4 cam_right;
    float4 cam_up;
    float4 cam_forward;
    float4 cam_origin;
    float4 proj;
    float4 range;
    float4 sun;
    uint2 size;
    float tolerance;
    uint flags;           // 2: reversed depth
    uint4 shadow;         // y: the filter's radius (GI)
    float4 reproject[4];
    float4 prev;
    float4 ambient;
    float4 back_ambient;
    float4 sun_color;
    float4 gi;
};

float view_depth(int2 p) {
    const float d = depth[uint2(p)] / range.x;
    const bool reversed = (flags & 2) != 0;
    if (reversed ? d <= 0.0 : d >= 1.0) return -1.0;
    return proj.w / (d - proj.z);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    const int2 px = int2(id.xy);
    const float z0 = view_depth(px);
    if (z0 < 0.0) {
        // sky and the landmark layer: nothing traced (alpha 0), where the patched shaders keep the game's own ambient
        irradiance[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const int r = int(shadow.y);
    float3 sum = 0.0;
    float wsum = 0.0;
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            const int2 q = clamp(px + int2(x, y), int2(0, 0), int2(size) - 1);
            const float z = view_depth(q);
            if (z < 0.0) continue;
            const float w = exp(-abs(z - z0) / max(z0 * 0.02, 0.02)) * exp(-float(x * x + y * y) / float(r * r + 1));
            sum += history[uint2(q)].rgb * w;
            wsum += w;
        }
    }
    irradiance[id.xy] = float4(wsum > 0.0 ? sum / wsum : history[id.xy].rgb, 1.0);
}
