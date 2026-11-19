// Phase 1 test pass: from the game's depth (copied by the plugin into an R32_FLOAT texture) to ClbTexture's layout
// (R = ambient occlusion, G = sun shadow). Bands by distance, each band twice as far as the last: whatever the depth
// convention, the bands follow the scene's geometry, which proves the depth reached the core and the result came back.
Texture2D<float> depthTex : register(t0);
RWTexture2D<unorm float2> result : register(u0);
cbuffer C : register(b0)
{
    uint frame;
    uint mode;
    uint width;
    uint height;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    float d = depthTex[id.xy];
    float distanceLike = 1.0 / max(1.0 - d, 1e-6);   // grows with distance for a [0 near, 1 far] depth buffer
    float band = frac(log2(distanceLike) * 1.5 + frame * 0.002);
    float shadow = band > 0.5 ? 1.0 : 0.15;
    result[id.xy] = float2(1.0, shadow);
}
