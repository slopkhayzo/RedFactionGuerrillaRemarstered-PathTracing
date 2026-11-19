// After DLSS Ray Reconstruction (dlss.h): its denoised irradiance into the texture the game's patched lit shaders read
// at t14, alpha 1 where a surface was traced, 0 for the sky and the landmark layer (where they keep the game's own
// ambient), as gi_filter.hlsl and gi_atrous.hlsl write it.

Texture2D<float4> denoised : register(t0);
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
    uint flags;  // 2: reversed depth
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    const float d = depth[id.xy] / range.x;
    const bool reversed = (flags & 2) != 0;
    const bool surface = !(reversed ? d <= 0.0 : d >= 1.0);
    irradiance[id.xy] = surface ? float4(max(denoised[id.xy].rgb, 0.0), 1.0) : float4(0.0, 0.0, 0.0, 0.0);
}
