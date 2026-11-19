// Test pattern for ClbTexture (R = ambient occlusion, G = sun shadow): a checkerboard of lit and shadowed cells.
// The blend state's write mask picks the channel(s) it lands in.
cbuffer P : register(b0)
{
    float4 params;  // x: cell size in pixels
};

float4 main(float4 pos : SV_Position) : SV_Target
{
    uint2 cell = uint2(pos.xy) / max(uint(params.x), 1u);
    float v = ((cell.x + cell.y) & 1u) ? 1.0 : 0.1;
    return float4(v, v, 0.0, 0.0);
}
