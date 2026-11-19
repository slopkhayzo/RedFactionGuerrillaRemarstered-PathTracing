// The core's volumetric fog over the lit opaque scene (render/gi.cpp, on_lit): rgb the light the fog in front of the
// pixel's surface scatters toward the camera, alpha the share of the surface's light that gets through; blended as
// scene * alpha + rgb (ONE, SRC_ALPHA). Alpha 0 where the fog is not the core's (the sky, the landmark layer, nothing
// written yet): the scene as it is (its shaders kept the game's fog there, patch/fog.h).
Texture2D<float4> fogTex : register(t0);

float4 main(float4 pos : SV_Position) : SV_Target
{
    const float4 f = fogTex.Load(int3(int2(pos.xy), 0));
    return f.a > 0.0 ? float4(max(f.rgb, 0.0), min(f.a, 1.0)) : float4(0.0, 0.0, 0.0, 1.0);
}
