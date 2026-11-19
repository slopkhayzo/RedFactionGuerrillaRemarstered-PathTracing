// Render.DebugView=3: the game's lit colour of the frame before, as the path tracer's rays read it for their hits
// (link.cpp, capture_radiance: the main scene's target at the end of the lighting passes), over the presented frame,
// with a simple curve (x / (1 + x)) for its HDR values. Black where the game had drawn nothing there yet.
Texture2D<float4> radianceTex : register(t0);

cbuffer Constants : register(b0)
{
    float4 scale;  // xy: capture texels per target pixel
};

float4 main(float4 pos : SV_Position) : SV_Target
{
    const float3 c = max(radianceTex.Load(int3(int2(pos.xy * scale.xy), 0)).rgb, 0.0);
    return float4(c / (1.0 + c), 1.0);
}
