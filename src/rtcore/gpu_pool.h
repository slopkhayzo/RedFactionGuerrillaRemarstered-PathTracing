#pragma once
// GPU memory for many small objects: large buffers (pages) handed out in 256-byte-aligned pieces, first fit with
// coalescing. The scene has thousands of meshes and tens of thousands of acceleration structures; a committed resource
// each would cost 64 KB apiece.
#include <d3d12.h>

#include <cstdint>
#include <map>
#include <vector>

namespace rtcore {

class GpuPool {
public:
    struct Alloc {
        ID3D12Resource* buffer = nullptr;
        uint64_t offset = 0, size = 0;
        D3D12_GPU_VIRTUAL_ADDRESS va = 0;
        uint32_t page = UINT32_MAX;
        explicit operator bool() const { return buffer != nullptr; }
    };

    // Pages are default-heap buffers with `flags`, created in `state`, of `page_bytes` (or larger for a larger piece).
    void init(ID3D12Device* dev, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, uint64_t page_bytes, const wchar_t* name);
    // A piece of at least `size` bytes, 256-byte aligned; empty when the GPU is out of memory.
    Alloc alloc(uint64_t size);
    void free(Alloc& a);
    uint64_t used() const { return used_; }
    // The pages (a piece's `page`), for views of them: they are only ever added, until release().
    size_t page_count() const { return pages_.size(); }
    ID3D12Resource* page(size_t i) const { return pages_[i].res; }
    uint64_t page_size(size_t i) const { return pages_[i].size; }
    uint64_t reserved() const { return reserved_; }
    void release();

private:
    struct Page {
        ID3D12Resource* res = nullptr;
        uint64_t size = 0;
        std::map<uint64_t, uint64_t> free;  // offset -> size
    };
    bool add_page(uint64_t size);

    ID3D12Device* dev_ = nullptr;
    D3D12_RESOURCE_FLAGS flags_ = D3D12_RESOURCE_FLAG_NONE;
    D3D12_RESOURCE_STATES state_ = D3D12_RESOURCE_STATE_COMMON;
    uint64_t page_bytes_ = 0;
    const wchar_t* name_ = L"";
    std::vector<Page> pages_;
    uint64_t used_ = 0, reserved_ = 0;
};

}  // namespace rtcore
