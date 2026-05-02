// DX12 GPU バッファ実装
#include "render/Dx12/Dx12Buffer.h"
#include "render/Dx12/Dx12Device.h"
#include "memory/UniquePtr.h"

#include <cstring>

namespace acs {

Dx12Buffer::~Dx12Buffer() noexcept {
    if (_mapped && _resource) _resource->Unmap(0, nullptr);
    ACS_SAFE_RELEASE(_resource);
}

HrResult Dx12Buffer::Init(Dx12Device& device, const BufferDesc& desc) noexcept {
    HrResult r{};
    _size  = desc.size;
    _usage = desc.usage;
    _cpu_writable = desc.cpu_writable;

    // バッファリソース記述（行レイアウトのリニアバッファ）
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment = 0;
    rd.Width  = static_cast<UINT64>(desc.size);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;

    // CPU 書込み可なら UPLOAD ヒープ、それ以外は DEFAULT ヒープ
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_RESOURCE_STATES init_state;
    if (desc.cpu_writable) {
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        init_state = D3D12_RESOURCE_STATE_GENERIC_READ;  // UPLOAD は GENERIC_READ 固定
    } else {
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        init_state = D3D12_RESOURCE_STATE_COMMON;
    }
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    r.hr = device.D3DDevice()->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr,
        IID_PPV_ARGS(&_resource));
    if (r.IsErr()) return r;

    // CPU 書込み可なら永続マップしてポインタを保持
    if (desc.cpu_writable) {
        D3D12_RANGE read_range{ 0, 0 };  // 読まない
        r.hr = _resource->Map(0, &read_range, &_mapped);
        if (r.IsErr()) return r;
    }

    // 初期データを書き込む（cpu_writable のときのみ簡易対応）
    if (desc.initial_data && desc.size > 0 && _mapped) {
        ::memcpy(_mapped, desc.initial_data, desc.size);
    }
    return r;
}

void Dx12Buffer::Update(const void* data, usize size, usize offset) noexcept {
    if (!_mapped || !data) return;
    if (offset + size > _size) return;
    ::memcpy(static_cast<u8*>(_mapped) + offset, data, size);
}

// ファクトリ
Result<UniquePtr<IRhiBuffer>> CreateRhiBuffer(IRhiDevice& device, const BufferDesc& desc) noexcept {
    const char* bn = device.BackendName();
    if (!(bn[0] == 'D' && bn[1] == 'X' && bn[2] == '1' && bn[3] == '2'))
        return ACS_ERR(Render, 30, "CreateRhiBuffer: device is not DX12");
    Dx12Device* dxd = static_cast<Dx12Device*>(&device);
    auto b = MakeUnique<Dx12Buffer>();
    HrResult r = b->Init(*dxd, desc);
    if (r.IsErr())
        return ACS_ERR_OS(Render, 31, "Dx12Buffer::Init failed", static_cast<u32>(r.hr));
    UniquePtr<IRhiBuffer> base(b.Release(), b.GetAllocator());
    return Result<UniquePtr<IRhiBuffer>>(OkInit, Move(base));
}

} // namespace acs
