// SPDX-License-Identifier: Apache-2.0
// DX12 テクスチャ実装
#include "render/Dx12/Dx12Texture.h"
#include "render/Dx12/Dx12Device.h"
#include "memory/UniquePtr.h"

#include <cstring>

namespace acs {

namespace {

// EFormat のピクセルあたりバイト数（簡易）
u32 BytesPerPixel(EFormat f) noexcept {
    switch (f) {
        case EFormat::R8G8B8A8_UNorm:
        case EFormat::R8G8B8A8_UNorm_sRGB:
        case EFormat::B8G8R8A8_UNorm:           return 4;
        case EFormat::R16G16_Float:             return 4;
        case EFormat::R16G16B16A16_Float:       return 8;
        case EFormat::R11G11B10_Float:          return 4;
        case EFormat::R32G32_Float:             return 8;
        case EFormat::R32G32B32_Float:          return 12;
        case EFormat::R32G32B32A32_Float:       return 16;
        case EFormat::D24_UNorm_S8_UInt:        return 4;
        case EFormat::D32_Float:                return 4;
        default:                                return 0;
    }
}

} // namespace

Dx12Texture::~Dx12Texture() noexcept {
    if (_device) {
        if (_srv_slot >= 0) _device->FreeSrvSlot(_srv_slot);
        if (_dsv_slot >= 0) _device->FreeDsvSlot(_dsv_slot);
    }
    _srv_slot = -1;
    _dsv_slot = -1;
    ACS_SAFE_RELEASE(_resource);
}

HrResult Dx12Texture::Init(Dx12Device& device, const TextureDesc& desc) noexcept {
    HrResult r{};
    _device = &device;
    _width  = desc.width;
    _height = desc.height;
    _format = desc.format;
    _is_depth = desc.is_depth_target;

    if (desc.width == 0 || desc.height == 0) {
        r.hr = E_INVALIDARG;
        return r;
    }

    const DXGI_FORMAT typed_fmt = ToDxgiFormat(desc.format);
    const u32 bpp = BytesPerPixel(desc.format);
    if (bpp == 0 && !desc.is_depth_target) { r.hr = E_INVALIDARG; return r; }

    // 深度の場合: 後で SRV/DSV 両方作れるよう TYPELESS で確保する
    DXGI_FORMAT resource_fmt = typed_fmt;
    DXGI_FORMAT dsv_fmt      = typed_fmt;
    DXGI_FORMAT depth_srv_fmt = DXGI_FORMAT_UNKNOWN;
    if (desc.is_depth_target) {
        if (typed_fmt == DXGI_FORMAT_D32_FLOAT) {
            resource_fmt   = DXGI_FORMAT_R32_TYPELESS;
            dsv_fmt        = DXGI_FORMAT_D32_FLOAT;
            depth_srv_fmt  = DXGI_FORMAT_R32_FLOAT;
        } else if (typed_fmt == DXGI_FORMAT_D24_UNORM_S8_UINT) {
            resource_fmt   = DXGI_FORMAT_R24G8_TYPELESS;
            dsv_fmt        = DXGI_FORMAT_D24_UNORM_S8_UINT;
            depth_srv_fmt  = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        }
    }

    // ===== 1. DEFAULT ヒープにテクスチャを作成 =====
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Alignment = 0;
    td.Width  = desc.width;
    td.Height = desc.height;
    td.DepthOrArraySize = 1;
    td.MipLevels = static_cast<UINT16>(desc.mip_levels);
    td.Format = resource_fmt;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags  = D3D12_RESOURCE_FLAG_NONE;
    if (desc.is_depth_target) td.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (desc.is_render_target) td.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES default_hp{};
    default_hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_STATES init_state;
    D3D12_CLEAR_VALUE  clear_val{};
    D3D12_CLEAR_VALUE* clear_ptr = nullptr;
    if (desc.is_depth_target) {
        init_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        clear_val.Format = dsv_fmt;          // typed format for clear
        clear_val.DepthStencil.Depth = 1.0f;
        clear_val.DepthStencil.Stencil = 0;
        clear_ptr = &clear_val;
    } else {
        init_state = desc.initial_data ? D3D12_RESOURCE_STATE_COPY_DEST
                                       : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    _current_state = init_state;

    r.hr = device.D3DDevice()->CreateCommittedResource(
        &default_hp, D3D12_HEAP_FLAG_NONE, &td, init_state, clear_ptr,
        IID_PPV_ARGS(&_resource));
    if (r.IsErr()) return r;

    // ===== 1b. 深度バッファの DSV + （任意）SRV =====
    if (desc.is_depth_target) {
        _dsv_slot = device.AllocateDsvSlot();
        if (_dsv_slot < 0) { r.hr = E_OUTOFMEMORY; return r; }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = dsv_fmt;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        device.D3DDevice()->CreateDepthStencilView(
            _resource, &dsv, device.DsvCpuHandle(_dsv_slot));

        // シェーダから読みたい場合のみ SRV も作る
        if (desc.shader_visible_depth && depth_srv_fmt != DXGI_FORMAT_UNKNOWN) {
            _srv_slot = device.AllocateSrvSlot();
            if (_srv_slot < 0) { r.hr = E_OUTOFMEMORY; return r; }
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = depth_srv_fmt;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            device.D3DDevice()->CreateShaderResourceView(
                _resource, &srv, device.SrvCpuHandle(_srv_slot));
        }
        return r;
    }

    // ===== 2. 初期データがあれば UPLOAD ヒープ経由でコピー =====
    if (desc.initial_data && desc.initial_data_size > 0) {
        ID3D12Device* dev = device.D3DDevice();

        // 必要な UPLOAD バッファサイズを取得（GetCopyableFootprints）
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 row_size_bytes = 0;
        UINT   num_rows = 0;
        UINT64 total_bytes = 0;
        dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &num_rows, &row_size_bytes, &total_bytes);

        D3D12_HEAP_PROPERTIES upload_hp{};
        upload_hp.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC ub{};
        ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ub.Width  = total_bytes;
        ub.Height = 1;
        ub.DepthOrArraySize = 1;
        ub.MipLevels = 1;
        ub.Format = DXGI_FORMAT_UNKNOWN;
        ub.SampleDesc.Count = 1;
        ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ID3D12Resource* upload = nullptr;
        r.hr = dev->CreateCommittedResource(
            &upload_hp, D3D12_HEAP_FLAG_NONE, &ub,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
        if (r.IsErr()) return r;

        // CPU 側からアップロードバッファに行ごとに書き込む
        u8* mapped = nullptr;
        D3D12_RANGE read_range{ 0, 0 };
        r.hr = upload->Map(0, &read_range, reinterpret_cast<void**>(&mapped));
        if (r.IsErr()) { upload->Release(); return r; }

        const u8* src = static_cast<const u8*>(desc.initial_data);
        const u32 src_row_pitch = desc.width * bpp;
        u8* dst = mapped + fp.Offset;
        for (u32 y = 0; y < num_rows; ++y) {
            ::memcpy(dst + y * fp.Footprint.RowPitch,
                     src + y * src_row_pitch,
                     src_row_pitch < row_size_bytes ? src_row_pitch
                                                    : static_cast<u32>(row_size_bytes));
        }
        upload->Unmap(0, nullptr);

        // ===== 3. コマンドリストを単発で作って upload → default をコピー =====
        ID3D12CommandAllocator* alloc = nullptr;
        r.hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (r.IsErr()) { upload->Release(); return r; }
        ID3D12GraphicsCommandList* cl = nullptr;
        r.hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                      IID_PPV_ARGS(&cl));
        if (r.IsErr()) { alloc->Release(); upload->Release(); return r; }

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = upload;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint = fp;

        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = _resource;
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = 0;

        cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

        // バリアで PixelShaderResource 状態に遷移
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = _resource;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);

        cl->Close();
        ID3D12CommandList* lists[] = { cl };
        device.GraphicsQueue()->ExecuteCommandLists(1, lists);
        device.WaitIdle();    // 完了待ち（簡易）

        cl->Release();
        alloc->Release();
        upload->Release();
    }

    // ===== 4. SRV ヒープに SRV を作成 =====
    _srv_slot = device.AllocateSrvSlot();
    if (_srv_slot < 0) { r.hr = E_OUTOFMEMORY; return r; }
    _current_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = typed_fmt;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = static_cast<UINT>(desc.mip_levels);
    device.D3DDevice()->CreateShaderResourceView(
        _resource, &srv, device.SrvCpuHandle(_srv_slot));

    return r;
}

D3D12_GPU_DESCRIPTOR_HANDLE Dx12Texture::SrvGpuHandle() const noexcept {
    return _device ? _device->SrvGpuHandle(_srv_slot) : D3D12_GPU_DESCRIPTOR_HANDLE{0};
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Texture::DsvCpuHandle() const noexcept {
    return _device ? _device->DsvCpuHandle(_dsv_slot) : D3D12_CPU_DESCRIPTOR_HANDLE{0};
}

// ファクトリ（Diligent バックエンドが有効化されている場合は Diligent 側に譲る）
#if !WITH_RENDER_DILIGENT
Result<UniquePtr<IRhiTexture>> CreateRhiTexture(IRhiDevice& device,
                                                const TextureDesc& desc) noexcept {
    const char* bn = device.BackendName();
    if (!(bn[0] == 'D' && bn[1] == 'X' && bn[2] == '1' && bn[3] == '2'))
        return ACS_ERR(Render, 70, "CreateRhiTexture: device is not DX12");
    Dx12Device* dxd = static_cast<Dx12Device*>(&device);
    auto t = MakeUnique<Dx12Texture>();
    HrResult r = t->Init(*dxd, desc);
    if (r.IsErr())
        return ACS_ERR_OS(Render, 71, "Dx12Texture::Init failed", static_cast<u32>(r.hr));
    UniquePtr<IRhiTexture> base(t.Release(), t.GetAllocator());
    return Result<UniquePtr<IRhiTexture>>(OkInit, Move(base));
}
#endif

} // namespace acs
