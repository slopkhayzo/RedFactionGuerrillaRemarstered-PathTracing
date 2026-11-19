// The core's result (R8G8: R = ambient occlusion, G = sun shadow, ClbTexture's layout) into ClbTexture. The blend
// state's write mask picks which channels replace the game's.
Texture2D<float2> resultTex : register(t0);

float4 main(float4 pos : SV_Position) : SV_Target
{
    float2 v = resultTex.Load(int3(int2(pos.xy), 0));
    return float4(v.x, v.y, 0.0, 0.0);
}
