#pragma once
#include <d3d11.h>

// The immediate context state our own draws touch, captured before and put back after them: the game's keen layer
// caches what it bound and would not bind it again.
class D3D11StateBlock {
public:
    void capture(ID3D11DeviceContext* c);
    void restore(ID3D11DeviceContext* c);
    ~D3D11StateBlock() { release(); }

private:
    void release();

    ID3D11InputLayout* layout_ = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* vb_ = nullptr;
    UINT vb_stride_ = 0, vb_offset_ = 0;
    ID3D11Buffer* ib_ = nullptr;
    DXGI_FORMAT ib_format_ = DXGI_FORMAT_UNKNOWN;
    UINT ib_offset_ = 0;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11Buffer* vs_cb_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11Buffer* ps_cb_ = nullptr;
    ID3D11ShaderResourceView* ps_srv_[2] = {};
    ID3D11SamplerState* ps_sampler_ = nullptr;
    ID3D11RasterizerState* rs_ = nullptr;
    D3D11_VIEWPORT viewports_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT viewport_count_ = 0;
    ID3D11RenderTargetView* rtv_[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    FLOAT blend_factor_[4] = {};
    UINT sample_mask_ = 0xFFFFFFFF;
    ID3D11DepthStencilState* dss_ = nullptr;
    UINT stencil_ref_ = 0;
    bool captured_ = false;
};
