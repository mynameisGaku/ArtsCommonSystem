// SPDX-License-Identifier: Apache-2.0
// DX12 テクスチャ実装
#include "render/Dx12/Dx12Texture.h"
#include "render/Dx12/Dx12Device.h"
#include "memory/UniquePtr.h"
#include "foundation/Log.h"

#include <cstring>

namespace acs {

namespace {

/**
 * EFormat 1 ピクセルあたりのバイト数を返す (簡易テーブル)。
 *
 * @details 初期データのアップロード時に行ピッチを計算するのに使う。
 * @param f 問い合わせるピクセルフォーマット。
 * @return 1 ピクセルのバイト数 (未知フォーマットは 0)。
 */
u32 BytesPerPixel(EFormat f) noexcept {
    switch (f) {
        case EFormat::R8G8B8A8_UNorm:
        case EFormat::R8G8B8A8_UNorm_sRGB:
        case EFormat::R8G8B8A8_UInt:
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

/** 割り当て済みの SRV/DSV/RTV スロットを解放しリソースを Release する。 */
FDx12Texture::~FDx12Texture() noexcept {
    Reset();
}

void FDx12Texture::Reset() noexcept
{
    FDx12Device* device = m_Device;
    ID3D12Resource* resource = m_Resource;
    const i32 srv_slot = m_SrvSlot;
    const i32 uav_slot = m_UavSlot;
    const i32 dsv_slot = m_DsvSlot;
    TArray<i32> rtv_slots = Move(m_RtvSlots);

    // Transfer ownership before clearing public state. Descriptors must travel
    // with the resource: recycling a slot while an old command list is still
    // in flight can make that list observe a replacement view.
    m_Device = nullptr;
    m_Resource = nullptr;
    m_SrvSlot = -1;
    m_UavSlot = -1;
    m_DsvSlot = -1;
    if (device != nullptr) {
        device->RetireTextureResource(
            resource, srv_slot, uav_slot, dsv_slot, Move(rtv_slots));
    } else {
        // A resource cannot be GPU-visible without its owning device. This is
        // only the empty/partial-construction fallback.
        ACS_SAFE_RELEASE(resource);
    }
    m_Width = 0;
    m_Height = 0;
    m_Depth = 1;
    m_Format = EFormat::Unknown;
    m_MipLevels = 1;
    m_ArraySize = 1;
    m_SampleCount = 1;
    m_IsCubemap = false;
    m_IsDepth = false;
    m_CurrentState = D3D12_RESOURCE_STATE_COMMON;
}

/** desc に従って DX12 リソースと SRV/DSV/RTV を生成する (詳細はヘッダ参照)。 */
FHrResult FDx12Texture::Init(FDx12Device& device, const FTextureDesc& desc) noexcept {
    FHrResult r{};
    Reset();

    if (!device.D3DDevice() || !device.GraphicsQueue()) {
        r.hr = E_INVALIDARG;
        return r;
    }
    m_Device = &device;
    m_Width  = desc.width;
    m_Height = desc.height;
    m_Depth  = desc.depth > 0 ? desc.depth : 1;
    m_Format = desc.format;
    m_IsDepth = desc.is_depth_target;
    m_MipLevels = desc.mip_levels > 0 ? desc.mip_levels : 1;
    m_ArraySize = desc.array_size > 0 ? desc.array_size : 1;
    m_IsCubemap = desc.is_cubemap;

    if (desc.width == 0 || desc.height == 0) {
        ACS_LOG_ERROR("Dx12Texture: zero dimension %ux%u", desc.width, desc.height);
        r.hr = E_INVALIDARG;
        Reset();
        return r;
    }

    const DXGI_FORMAT typed_fmt = ToDxgiFormat(desc.format);
    const u32 bpp = BytesPerPixel(desc.format);
    if (typed_fmt == DXGI_FORMAT_UNKNOWN || (bpp == 0 && !desc.is_depth_target)) {
        ACS_LOG_ERROR("Dx12Texture: bpp=0 for fmt=%d", static_cast<int>(desc.format));
        r.hr = E_INVALIDARG;
        Reset();
        return r;
    }

    if (m_ArraySize > 0xFFFFu || m_MipLevels > 0xFFFFu || m_Depth > 0xFFFFu ||
        (desc.initial_data == nullptr) != (desc.initial_data_size == 0) ||
        (desc.is_depth_target && desc.is_render_target) || (desc.shader_visible_depth && !desc.is_depth_target) ||
        (desc.is_depth_target && desc.is_uav) ||
        (desc.is_depth_target && desc.format != EFormat::D24_UNorm_S8_UInt && desc.format != EFormat::D32_Float) ||
        (m_Depth > 1 && (m_ArraySize != 1 || desc.is_cubemap || desc.is_depth_target ||
                         desc.is_render_target || desc.per_slice_rtv || desc.initial_data))) {
        ACS_LOG_ERROR("Dx12Texture: descriptor の組み合わせまたは範囲が不正です");
        r.hr = E_INVALIDARG;
        Reset();
        return r;
    }

    // 配列レイヤ / キューブマップ / per-slice RTV / mip を DX12 backend で本実装する
    // (IBL の env/irradiance/prefilter cubemap = array_size=6 + per_slice_rtv で必須)。
    const u32 req_array = m_ArraySize;
    // cubemap は DX12 では 6 の倍数スライスを要求する。IBL は array_size=6 を渡す。
    if (desc.is_cubemap && (req_array % 6u != 0u)) {
        ACS_LOG_ERROR("Dx12Texture: cubemap は array_size を 6 の倍数にしてください (array_size=%u)",
                      req_array);
        r.hr = E_INVALIDARG;
        Reset();
        return r;
    }
    // per_slice_rtv は RT 用 (各 face/mip を個別の RTV として書く)。RT 以外で立っていたら無効。
    if (desc.per_slice_rtv && !desc.is_render_target) {
        ACS_LOG_ERROR("Dx12Texture: per_slice_rtv は is_render_target=true と併用してください");
        r.hr = E_INVALIDARG;
        Reset();
        return r;
    }

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

    // MSAA は RT 専用 (MS テクスチャは mip 不可・SRV 無し・初期データ不可)
    m_SampleCount = (desc.is_render_target && desc.sample_count > 1) ? desc.sample_count : 1;
    if (desc.sample_count > 1 && (!desc.is_render_target || desc.initial_data || desc.is_uav ||
                                  m_MipLevels != 1 || m_ArraySize != 1 ||
                                  desc.is_cubemap || desc.per_slice_rtv)) {
        ACS_LOG_ERROR("Dx12Texture: MSAA は単一スライス・単一 mip の初期データなし RT 専用です");
        r.hr = E_INVALIDARG;
        Reset();
        return r;
    }

    // 1. DEFAULT ヒープにテクスチャを作成
    D3D12_RESOURCE_DESC td{};
    td.Dimension = m_Depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                               : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Alignment = 0;
    td.Width  = desc.width;
    td.Height = desc.height;
    td.DepthOrArraySize = static_cast<UINT16>(m_Depth > 1 ? m_Depth : req_array);
    td.MipLevels = static_cast<UINT16>(m_MipLevels);
    td.Format = resource_fmt;
    td.SampleDesc.Count = m_SampleCount;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags  = D3D12_RESOURCE_FLAG_NONE;
    if (desc.is_depth_target) td.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (desc.is_render_target) td.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (desc.is_uav) td.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

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
        // RT は最適化クリア値を渡す (ALLOW_RENDER_TARGET で null clear だと一部
        // ドライバ / debug layer が作成を弾く / 警告するため、format 一致値を付ける)。
        if (desc.is_render_target) {
            clear_val.Format   = typed_fmt;
            clear_val.Color[0] = 0.0f;
            clear_val.Color[1] = 0.0f;
            clear_val.Color[2] = 0.0f;
            clear_val.Color[3] = 1.0f;
            clear_ptr = &clear_val;
        }
    }
    m_CurrentState = init_state;

    r.hr = device.D3DDevice()->CreateCommittedResource(
        &default_hp, D3D12_HEAP_FLAG_NONE, &td, init_state, clear_ptr,
        IID_PPV_ARGS(&m_Resource));
    if (r.IsErr() || !m_Resource) {
        if (r.IsOk()) r.hr = E_FAIL;
        ACS_LOG_ERROR("Dx12Texture CreateCommittedResource failed: hr=0x%08X "
                      "fmt=%d %ux%u rt=%d depth=%d",
                      static_cast<unsigned>(r.hr), static_cast<int>(desc.format),
                      desc.width, desc.height,
                      desc.is_render_target ? 1 : 0, desc.is_depth_target ? 1 : 0);
        Reset();
        return r;
    }

    // 1b. 深度バッファの DSV + （任意）SRV
    if (desc.is_depth_target) {
        m_DsvSlot = device.AllocateDsvSlot();
        if (m_DsvSlot < 0) {
            r.hr = E_OUTOFMEMORY;
            Reset();
            return r;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = dsv_fmt;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        device.D3DDevice()->CreateDepthStencilView(
            m_Resource, &dsv, device.DsvCpuHandle(m_DsvSlot));

        // シェーダから読みたい場合のみ SRV も作る
        if (desc.shader_visible_depth && depth_srv_fmt != DXGI_FORMAT_UNKNOWN) {
            m_SrvSlot = device.AllocateSrvSlot();
            if (m_SrvSlot < 0) {
                r.hr = E_OUTOFMEMORY;
                Reset();
                return r;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = depth_srv_fmt;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            device.D3DDevice()->CreateShaderResourceView(
                m_Resource, &srv, device.SrvCpuHandle(m_SrvSlot));
        }
        return r;
    }

    // 2. 初期データがあれば UPLOAD ヒープ経由でコピー
    if (desc.initial_data && desc.initial_data_size > 0) {
        ID3D12Device* dev = device.D3DDevice();

        // mip_levels>1 で initial_data を渡されても、ここでアップロードするのは
        // subresource 0 (mip 0) のみ。上位 mip は未初期化のままなので、黙って
        // 部分初期化にせず警告を出す (Diligent バックエンドの挙動に合わせる)。
        if (m_MipLevels > 1 || m_ArraySize > 1) {
            ACS_LOG_WARN("Dx12Texture: initial_data は subresource 0 (slice0/mip0) のみ"
                         "アップロードされます (mip_levels=%u array_size=%u の残りは未初期化)。"
                         "配列/ミップの初期データが必要なら per-subresource アップロードを使用してください",
                         m_MipLevels, m_ArraySize);
        }

        // 必要な UPLOAD バッファサイズを取得（GetCopyableFootprints）
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 row_size_bytes = 0;
        UINT   num_rows = 0;
        UINT64 total_bytes = 0;
        dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &num_rows, &row_size_bytes, &total_bytes);
        if (total_bytes == 0 || num_rows == 0) {
            r.hr = E_INVALIDARG;
            Reset();
            return r;
        }

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
        if (r.IsErr() || !upload) {
            if (r.IsOk()) r.hr = E_FAIL;
            ACS_SAFE_RELEASE(upload);
            Reset();
            return r;
        }

        // CPU 側からアップロードバッファに行ごとに書き込む
        u8* mapped = nullptr;
        D3D12_RANGE read_range{ 0, 0 };
        r.hr = upload->Map(0, &read_range, reinterpret_cast<void**>(&mapped));
        if (r.IsErr() || !mapped) {
            if (r.IsOk()) r.hr = E_FAIL;
            if (mapped) upload->Unmap(0, nullptr);
            ACS_SAFE_RELEASE(upload);
            Reset();
            return r;
        }

        const u8* src = static_cast<const u8*>(desc.initial_data);
        // tightly-packed なソース行ピッチ。これに対し initial_data_size を検証しないと、
        // 呼び出し側が想定より小さいバッファを渡した場合に src + y*pitch の memcpy が
        // バッファ外を読む (OOB read)。期待バイト数 = src_row_pitch * num_rows を計算し、
        // initial_data_size と突き合わせる。
        const u64 src_row_pitch = static_cast<u64>(desc.width) * bpp;
        // 行ごとに実際にコピーするバイト数 (DEFAULT 側の行サイズと src 行サイズの小さい方)。
        const u64 copy_row_len  = src_row_pitch < row_size_bytes ? src_row_pitch : row_size_bytes;
        const u64 row_count = static_cast<u64>(num_rows);
        if (row_count == 0 || src_row_pitch > static_cast<u64>(-1) / row_count) {
            upload->Unmap(0, nullptr);
            ACS_SAFE_RELEASE(upload);
            r.hr = E_INVALIDARG;
            Reset();
            return r;
        }
        const u64 expected_src = src_row_pitch * row_count;
        if (desc.initial_data_size < expected_src) {
            // 不足: 想定サブリソースサイズを満たさない。OOB を避けるためエラーにする
            // (足りないデータで黙って成功扱いにしない)。
            ACS_LOG_ERROR("Dx12Texture: initial_data_size=%llu が想定値 %llu (=%ux%u rows x %llu bytes) "
                          "未満です。OOB read を避けるため中断します",
                          static_cast<unsigned long long>(desc.initial_data_size),
                          static_cast<unsigned long long>(expected_src),
                          desc.width, num_rows,
                          static_cast<unsigned long long>(src_row_pitch));
            upload->Unmap(0, nullptr);
            ACS_SAFE_RELEASE(upload);
            r.hr = E_INVALIDARG;
            Reset();
            return r;
        }
        u8* dst = mapped + fp.Offset;
        for (u32 y = 0; y < num_rows; ++y) {
            ::memcpy(dst + static_cast<usize>(y) * fp.Footprint.RowPitch,
                     src + static_cast<usize>(y) * src_row_pitch,
                     static_cast<usize>(copy_row_len));
        }
        upload->Unmap(0, nullptr);

        // 3. コマンドリストを単発で作って upload → default をコピー
        ID3D12CommandAllocator* alloc = nullptr;
        r.hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (r.IsErr()) {
            ACS_SAFE_RELEASE(alloc);
            ACS_SAFE_RELEASE(upload);
            Reset();
            return r;
        }
        ID3D12GraphicsCommandList* cl = nullptr;
        r.hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                      IID_PPV_ARGS(&cl));
        if (r.IsErr()) {
            ACS_SAFE_RELEASE(cl);
            ACS_SAFE_RELEASE(alloc);
            ACS_SAFE_RELEASE(upload);
            Reset();
            return r;
        }

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = upload;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint = fp;

        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = m_Resource;
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = 0;

        cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

        // バリアで PixelShaderResource 状態に遷移
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_Resource;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);

        r.hr = cl->Close();
        if (r.IsOk()) {
            const u64 fence_value =
                device.ExecuteOneOffGraphicsCommandList(cl);
            if (fence_value == 0) {
                r.hr = E_FAIL;
                // Execute may have reached the queue even when Signal failed.
                // No fence means there is no safe point at which to release
                // the allocator/list/upload resource, so fail closed by
                // intentionally abandoning their COM ownership.
                cl = nullptr;
                alloc = nullptr;
                upload = nullptr;
            } else {
                device.WaitForFenceValue(fence_value);
            }
        }

        ACS_SAFE_RELEASE(cl);
        ACS_SAFE_RELEASE(alloc);
        ACS_SAFE_RELEASE(upload);
        if (r.IsErr()) {
            Reset();
            return r;
        }
    }

    // 4. SRV ヒープに SRV を作成 (MSAA RT は sample 不可なので SRV を作らない → resolve して使う)
    if (m_SampleCount > 1) {
        const i32 slot = device.AllocateRtvSlot();
        if (slot < 0) {
            ACS_LOG_ERROR("Dx12Texture: RTV slot exhausted (MSAA)");
            r.hr = E_OUTOFMEMORY;
            Reset();
            return r;
        }
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = typed_fmt;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        device.D3DDevice()->CreateRenderTargetView(m_Resource, &rtv, device.RtvCpuHandle(slot));
        m_RtvSlots.PushBack(slot);
        return r;
    }
    m_SrvSlot = device.AllocateSrvSlot();
    if (m_SrvSlot < 0) {
        ACS_LOG_ERROR("Dx12Texture: SRV slot exhausted");
        r.hr = E_OUTOFMEMORY;
        Reset();
        return r;
    }
    m_CurrentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = typed_fmt;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (m_Depth > 1) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MostDetailedMip = 0;
        srv.Texture3D.MipLevels = m_MipLevels;
        srv.Texture3D.ResourceMinLODClamp = 0.0f;
    } else if (m_IsCubemap) {
        // cubemap: シェーダは TextureCube として全 mip をサンプルする (prefilter roughness LOD)
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MostDetailedMip     = 0;
        srv.TextureCube.MipLevels           = m_MipLevels;
        srv.TextureCube.ResourceMinLODClamp = 0.0f;
    } else if (m_ArraySize > 1) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray.MostDetailedMip     = 0;
        srv.Texture2DArray.MipLevels           = m_MipLevels;
        srv.Texture2DArray.FirstArraySlice     = 0;
        srv.Texture2DArray.ArraySize           = m_ArraySize;
        srv.Texture2DArray.PlaneSlice          = 0;
        srv.Texture2DArray.ResourceMinLODClamp = 0.0f;
    } else {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = m_MipLevels;
    }
    device.D3DDevice()->CreateShaderResourceView(
        m_Resource, &srv, device.SrvCpuHandle(m_SrvSlot));

    // 5. Compute write target は同じ shader-visible heap に UAV も作成。
    if (desc.is_uav) {
        m_UavSlot = device.AllocateSrvSlot();
        if (m_UavSlot < 0) {
            ACS_LOG_ERROR("Dx12Texture: UAV slot exhausted");
            r.hr = E_OUTOFMEMORY;
            Reset();
            return r;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = typed_fmt;
        if (m_Depth > 1) {
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            uav.Texture3D.MipSlice = 0;
            uav.Texture3D.FirstWSlice = 0;
            uav.Texture3D.WSize = m_Depth;
        } else if (m_ArraySize > 1) {
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uav.Texture2DArray.MipSlice = 0;
            uav.Texture2DArray.FirstArraySlice = 0;
            uav.Texture2DArray.ArraySize = m_ArraySize;
            uav.Texture2DArray.PlaneSlice = 0;
        } else {
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Texture2D.MipSlice = 0;
            uav.Texture2D.PlaneSlice = 0;
        }
        device.D3DDevice()->CreateUnorderedAccessView(
            m_Resource, nullptr, &uav, device.SrvCpuHandle(m_UavSlot));
    }

    // 6. オフスクリーン RT は RTV も作成 (BeginRenderToTexture(Slice) 用)
    if (desc.is_render_target) {
        if (desc.per_slice_rtv) {
            // array_size * mip_levels 個の per-slice RTV を作成 (cube face / 配列スライス / mip
            // を個別の描画先にする)。並び順 index = slice*mip_levels + mip で
            // RtvCpuHandleForSlice() がこの順に引く。
            for (u32 s = 0; s < m_ArraySize; ++s) {
                for (u32 mip = 0; mip < m_MipLevels; ++mip) {
                    const i32 slot = device.AllocateRtvSlot();
                    if (slot < 0) {
                        ACS_LOG_ERROR("Dx12Texture: per-slice RTV slot 枯渇 (slice=%u mip=%u)", s, mip);
                        r.hr = E_OUTOFMEMORY;
                        Reset();
                        return r;
                    }
                    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
                    rtv.Format = typed_fmt;
                    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    rtv.Texture2DArray.MipSlice        = mip;
                    rtv.Texture2DArray.FirstArraySlice = s;
                    rtv.Texture2DArray.ArraySize       = 1;
                    rtv.Texture2DArray.PlaneSlice      = 0;
                    device.D3DDevice()->CreateRenderTargetView(
                        m_Resource, &rtv, device.RtvCpuHandle(slot));
                    m_RtvSlots.PushBack(slot);
                }
            }
        } else {
            const i32 slot = device.AllocateRtvSlot();
            if (slot < 0) {
                ACS_LOG_ERROR("Dx12Texture: RTV slot exhausted");
                r.hr = E_OUTOFMEMORY;
                Reset();
                return r;
            }
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = typed_fmt;
            if (m_ArraySize > 1) {
                // 全スライスを覆う配列 RTV (mip0)
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtv.Texture2DArray.MipSlice        = 0;
                rtv.Texture2DArray.FirstArraySlice = 0;
                rtv.Texture2DArray.ArraySize       = m_ArraySize;
                rtv.Texture2DArray.PlaneSlice      = 0;
            } else {
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            }
            device.D3DDevice()->CreateRenderTargetView(
                m_Resource, &rtv, device.RtvCpuHandle(slot));
            m_RtvSlots.PushBack(slot);
        }
    }

    return r;
}

/** 先頭 (slice0/mip0) の RTV CPU ハンドルを返す。 */
D3D12_CPU_DESCRIPTOR_HANDLE FDx12Texture::RtvCpuHandle() const noexcept {
    if (!m_Device || m_RtvSlots.IsEmpty()) return D3D12_CPU_DESCRIPTOR_HANDLE{0};
    return m_Device->RtvCpuHandle(m_RtvSlots[0]);
}

/** index = slice*mip_levels + mip の per-slice RTV CPU ハンドルを返す。 */
D3D12_CPU_DESCRIPTOR_HANDLE FDx12Texture::RtvCpuHandleForSlice(u32 slice, u32 mip) const noexcept {
    if (!m_Device) return D3D12_CPU_DESCRIPTOR_HANDLE{0};
    const usize idx = static_cast<usize>(slice) * m_MipLevels + mip;
    if (idx >= m_RtvSlots.Size()) return D3D12_CPU_DESCRIPTOR_HANDLE{0};
    return m_Device->RtvCpuHandle(m_RtvSlots[idx]);
}

/** SRV の GPU ディスクリプタハンドルを返す。 */
D3D12_GPU_DESCRIPTOR_HANDLE FDx12Texture::SrvGpuHandle() const noexcept {
    return (m_Device && m_SrvSlot >= 0) ? m_Device->SrvGpuHandle(m_SrvSlot) : D3D12_GPU_DESCRIPTOR_HANDLE{0};
}

D3D12_GPU_DESCRIPTOR_HANDLE FDx12Texture::UavGpuHandle() const noexcept {
    return (m_Device && m_UavSlot >= 0) ? m_Device->SrvGpuHandle(m_UavSlot)
                                        : D3D12_GPU_DESCRIPTOR_HANDLE{0};
}

/** DSV の CPU ディスクリプタハンドルを返す。 */
D3D12_CPU_DESCRIPTOR_HANDLE FDx12Texture::DsvCpuHandle() const noexcept {
    return (m_Device && m_DsvSlot >= 0) ? m_Device->DsvCpuHandle(m_DsvSlot) : D3D12_CPU_DESCRIPTOR_HANDLE{0};
}

#if !WITH_RENDER_DILIGENT
/**
 * RHI テクスチャを生成するファクトリ (DX12 バックエンド版)。
 *
 * @details
 * Diligent バックエンドが有効なときは Diligent 側の実装に譲るため、この定義は
 * WITH_RENDER_DILIGENT が無効なときのみ有効。device が DX12 でない場合はエラーを返す。
 * @param device テクスチャを生成する RHI デバイス (DX12 でなければエラー)。
 * @param desc 生成するテクスチャの記述。
 * @return 成功なら IRhiTexture を所有する TUniquePtr、失敗ならエラー。
 */
TResult<TUniquePtr<IRhiTexture>> CreateRhiTexture(IRhiDevice& device,
                                                const FTextureDesc& desc) noexcept {
    const char* bn = device.BackendName();
    if (!(bn[0] == 'D' && bn[1] == 'X' && bn[2] == '1' && bn[3] == '2'))
        return ACS_ERR(Render, 70, "CreateRhiTexture: device is not DX12");
    FDx12Device* dxd = static_cast<FDx12Device*>(&device);
    auto t = MakeUnique<FDx12Texture>();
    const FHrResult r = t->Init(*dxd, desc);
    if (r.IsErr())
        return ACS_ERR_OS(Render, 71, "Dx12Texture::Init failed", static_cast<u32>(r.hr));
    TUniquePtr<IRhiTexture> base(t.Release(), t.GetAllocator());
    return TResult<TUniquePtr<IRhiTexture>>(OkInit, Move(base));
}
#endif

} // namespace acs
