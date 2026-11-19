#include "render/d3d_state.h"

namespace {

template <typename T>
void rel(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

}  // namespace

void D3D11StateBlock::capture(ID3D11DeviceContext* c) {
    release();
    c->IAGetInputLayout(&layout_);
    c->IAGetPrimitiveTopology(&topology_);
    c->IAGetVertexBuffers(0, 1, &vb_, &vb_stride_, &vb_offset_);
    c->IAGetIndexBuffer(&ib_, &ib_format_, &ib_offset_);
    c->VSGetShader(&vs_, nullptr, nullptr);
    c->VSGetConstantBuffers(0, 1, &vs_cb_);
    c->PSGetShader(&ps_, nullptr, nullptr);
    c->PSGetConstantBuffers(0, 1, &ps_cb_);
    c->PSGetShaderResources(0, 2, ps_srv_);
    c->PSGetSamplers(0, 1, &ps_sampler_);
    c->RSGetState(&rs_);
    viewport_count_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetViewports(&viewport_count_, viewports_);
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv_, &dsv_);
    c->OMGetBlendState(&blend_, blend_factor_, &sample_mask_);
    c->OMGetDepthStencilState(&dss_, &stencil_ref_);
    captured_ = true;
}

void D3D11StateBlock::restore(ID3D11DeviceContext* c) {
    if (!captured_) return;
    c->IASetInputLayout(layout_);
    c->IASetPrimitiveTopology(topology_);
    c->IASetVertexBuffers(0, 1, &vb_, &vb_stride_, &vb_offset_);
    c->IASetIndexBuffer(ib_, ib_format_, ib_offset_);
    c->VSSetShader(vs_, nullptr, 0);
    c->VSSetConstantBuffers(0, 1, &vs_cb_);
    c->PSSetShader(ps_, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &ps_cb_);
    c->PSSetShaderResources(0, 2, ps_srv_);
    c->PSSetSamplers(0, 1, &ps_sampler_);
    c->RSSetState(rs_);
    c->RSSetViewports(viewport_count_, viewports_);
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv_, dsv_);
    c->OMSetBlendState(blend_, blend_factor_, sample_mask_);
    c->OMSetDepthStencilState(dss_, stencil_ref_);
    release();
}

void D3D11StateBlock::release() {
    rel(layout_);
    rel(vb_);
    rel(ib_);
    rel(vs_);
    rel(vs_cb_);
    rel(ps_);
    rel(ps_cb_);
    rel(ps_srv_[0]);
    rel(ps_srv_[1]);
    rel(ps_sampler_);
    rel(rs_);
    for (auto& r : rtv_) rel(r);
    rel(dsv_);
    rel(blend_);
    rel(dss_);
    captured_ = false;
}
