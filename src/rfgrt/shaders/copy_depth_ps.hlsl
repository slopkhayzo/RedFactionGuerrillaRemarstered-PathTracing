// The game's depth (the scene renderer's depth texture, whatever its view format) into the R32_FLOAT texture shared
// with the ray tracing core: depth-stencil textures cannot be shared themselves.
Texture2D<float> depthTex : register(t0);

float main(float4 pos : SV_Position) : SV_Target
{
    return depthTex.Load(int3(int2(pos.xy), 0));
}
