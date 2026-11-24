// DLSS Frame Generation's UI layer (presenter.cpp; kBufferTypeUIColorAndAlpha: the UI's colour premultiplied by its alpha,
// alpha 0 where there is none; final = UI.rgb + (1 - UI.a) * HUD-less). DLSS-G keeps the HUD sharp when it knows where it
// is. The game draws its HUD and interface straight into the back buffer after its scene, so the layer is where the
// finished frame differs from the frame captured before them: opaque there (the HUD's own translucency is not recovered),
// its colour the finished frame's. All three hold the back buffer's bytes (unorm views: no sRGB conversion). As the
// SpaceEngineers-RTHWAccel project's fg_ui.hlsl.

Texture2D<float4> Final : register(t0);    // the finished frame (the HUD in it)
Texture2D<float4> Hudless : register(t1);  // the frame before the HUD
RWTexture2D<float4> Ui : register(u0);

cbuffer Constants : register(b0) {
    uint Width;
    uint Height;
};

// Differences below this (in 8-bit steps) are the same pixel.
static const float kThreshold = 1.5 / 255.0;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Width || id.y >= Height) return;
    const float4 final = Final[id.xy];
    const float3 difference = abs(final.rgb - Hudless[id.xy].rgb);
    const float alpha = max(difference.r, max(difference.g, difference.b)) > kThreshold ? 1.0 : 0.0;
    Ui[id.xy] = float4(final.rgb * alpha, alpha);
}
