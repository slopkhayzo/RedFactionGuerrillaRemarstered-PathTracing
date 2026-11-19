#include "gpu_pool.h"

#include <iterator>
#include <utility>

namespace rtcore {

namespace {
constexpr uint64_t kAlign = 256;
uint64_t round_up(uint64_t v) { return (v + kAlign - 1) & ~(kAlign - 1); }
}  // namespace

void GpuPool::init(ID3D12Device* dev, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, uint64_t page_bytes,
                   const wchar_t* name) {
    dev_ = dev;
    flags_ = flags;
    state_ = state;
    page_bytes_ = round_up(page_bytes);
    name_ = name;
}

bool GpuPool::add_page(uint64_t size) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags_;
    Page p;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state_, nullptr, IID_PPV_ARGS(&p.res)))) return false;
    p.res->SetName(name_);
    p.size = size;
    p.free[0] = size;
    pages_.push_back(std::move(p));
    reserved_ += size;
    return true;
}

GpuPool::Alloc GpuPool::alloc(uint64_t size) {
    size = round_up(size ? size : 1);
    for (int attempt = 0; attempt < 2; ++attempt) {
        for (uint32_t pi = 0; pi < pages_.size(); ++pi) {
            Page& p = pages_[pi];
            for (auto it = p.free.begin(); it != p.free.end(); ++it) {
                if (it->second < size) continue;
                Alloc a;
                a.buffer = p.res;
                a.offset = it->first;
                a.size = size;
                a.va = p.res->GetGPUVirtualAddress() + a.offset;
                a.page = pi;
                const uint64_t rest = it->second - size;
                const uint64_t rest_at = it->first + size;
                p.free.erase(it);
                if (rest) p.free[rest_at] = rest;
                used_ += size;
                return a;
            }
        }
        if (attempt == 0 && !add_page(size > page_bytes_ ? size : page_bytes_)) break;
    }
    return {};
}

void GpuPool::free(Alloc& a) {
    if (!a.buffer || a.page >= pages_.size()) {
        a = {};
        return;
    }
    Page& p = pages_[a.page];
    uint64_t off = a.offset, size = a.size;
    auto next = p.free.lower_bound(off);
    if (next != p.free.end() && off + size == next->first) {  // merge with the following free piece
        size += next->second;
        next = p.free.erase(next);
    }
    if (next != p.free.begin()) {  // and with the preceding one
        auto prev = std::prev(next);
        if (prev->first + prev->second == off) {
            off = prev->first;
            size += prev->second;
            p.free.erase(prev);
        }
    }
    p.free[off] = size;
    used_ -= a.size;
    a = {};
}

void GpuPool::release() {
    for (Page& p : pages_)
        if (p.res) p.res->Release();
    pages_.clear();
    used_ = reserved_ = 0;
}

}  // namespace rtcore
