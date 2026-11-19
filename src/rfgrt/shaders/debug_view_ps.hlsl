// Render.DebugView: the core's last result over the presented frame. With the depth compare (Host.Mode=2) R holds
// depth as bands along the ray (0 where nothing is hit) and G whether the mirror agrees with the game's depth: the left
// half shows the game's own depth (gray bands), the right half the mirror's (gray where it agrees, red where not),
// blue where there is nothing.
Texture2D<float2> resultTex : register(t0);

cbuffer Constants : register(b0)
{
    float4 scale;  // xy: result texels per target pixel
};

float4 main(float4 pos : SV_Position) : SV_Target
{
    float2 v = resultTex.Load(int3(int2(pos.xy * scale.xy), 0));
    float band = v.x;
    float3 c = band <= 0.0 ? float3(0.1, 0.2, 0.6) : v.y > 0.5 ? float3(band, band, band) : float3(band, band * 0.3, band * 0.3);
    return float4(c, 1.0);
}
