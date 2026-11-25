// DLSS Frame Generation's motion vectors (presenter.cpp; kBufferTypeMotionVectors, in pixels: previous = current +
// motion): each pixel's surface, from the game's depth, carried into the previous frame by the clip-to-previous-clip
// matrix the constants give DLSS-G too (framegen.cpp: this frame's inverse view-projection, the cameras' offset, the
// previous frame's view-projection; camera-relative). The camera's motion only, as for Ray Reconstruction: the game gives
// no object motion, and DLSS-G's optical flow covers what moves on its own. A point at infinity (the sky, w 0 back in
// the world) moves with the camera's turn alone.

Texture2D<float> Depth : register(t0);  // the game's hardware depth, in [0, DepthMax] (its viewport's depth range)
RWTexture2D<float2> Motion : register(u0);

cbuffer Constants : register(b0) {
    float4 ClipToPrevClip[4];  // its rows (row vectors: clip now -> the previous frame's clip)
    uint Width;
    uint Height;
    float DepthMax;  // 1 - the game's landmark depth range: its main scene's depth is the projection's scaled by it
    float Pad;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Width || id.y >= Height) return;
    const float2 here = float2(id.xy) + 0.5;
    const float2 ndc = float2(here.x / Width * 2.0 - 1.0, 1.0 - here.y / Height * 2.0);
    const float z = min(Depth[id.xy] / DepthMax, 1.0);
    const float4 clip = float4(ndc, z, 1.0);
    const float4 prev = clip.x * ClipToPrevClip[0] + clip.y * ClipToPrevClip[1] + clip.z * ClipToPrevClip[2] + ClipToPrevClip[3];
    if (abs(prev.w) <= 1e-7) {
        Motion[id.xy] = float2(0.0, 0.0);
        return;
    }
    const float2 then_ndc = prev.xy / prev.w;
    const float2 then = float2((then_ndc.x + 1.0) * 0.5 * Width, (1.0 - then_ndc.y) * 0.5 * Height);
    Motion[id.xy] = then - here;
}
