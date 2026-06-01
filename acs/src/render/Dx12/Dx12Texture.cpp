// SPDX-License-Identifier: Apache-2.0
// DX12 テクスチャ実装
#include "render/Dx12/Dx12Texture.h"
#include "render/Dx12/Dx12Device.h"
#include "memory/UniquePtr.h"
#include "foundation/Log.h"

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
    if (m_Device) {
        if (m_SrvSlot >= 0) m_Device->FreeSrvSlot(m_SrvSlot);
        if (m_DsvSlot >= 0) m_Device->FreeDsvSlot(m_DsvSlot);
        if (m_RtvSlot >= 0) m_Device->FreeRtvSlot(m_RtvSlot);
    }
    m_SrvSlot = -1;
    m_DsvSlot = -1;
    m_RtvSlot = -1;
    ACS_SAFE_RELEASE(m_Resource);
}

HrResult Dx12Texture::Init(Dx12Device& device, const FTextureDesc& desc) noexcept {
    HrResult r{};
    m_Device = &device;
    m_Width  = desc.width;
    m_Height = desc.height;
    m_Format = desc.format;
    m_IsDepth = desc.is_depth_target;

    if (desc.width == 0 || desc.height == 0) {
        ACS_LOG_ERROR("Dx12Texture: zero dimension %ux%u", desc.width, desc.height);
        r.hr = E_INVALIDARG;
        return r;
    }

    const DXGI_FORMAT typed_fmt = ToDxgiFormat(desc.format);
    const u32 bpp = BytesPerPixel(desc.format);
    if (bpp == 0 && !desc.is_depth_target) {
        ACS_LOG_ERROR("Dx12Texture: bpp=0 for fmt=%d", static_cast<int>(desc.format));
        r.hr = E_INVALIDARG; return r;
    }

    // 配列レイヤ / キューブマップ / per-slice RTV は、この DX12 フォールバック
    // バックエンド (WITH_RENDER_DILIGENT が無効な構成でのみコンパイルされる) では
    // 未配線。以前は array_size/is_cubemap/per_slice_rtv を黙って無視し DepthOrArraySize=1
    // の単一 2D・TEXTURE2D ビューを作っていたため、cubemap (IBL 等、array_size=6) を要求
    // した呼び出し側に「成功したが中身は単一 2D」という偽の成功を返していた。
    // 規約上「黙って偽の成功を返す」のは禁止なので、未対応構成は honest にエラーを返す。
    // (canonical な Diligent バックエンドはこれらを完全サポートしている)
    const u32 req_array = desc.array_size > 0 ? desc.array_size : 1;
    if (desc.is_cubemap || req_array > 1 || desc.per_slice_rtv) {
        ACS_LOG_ERROR("Dx12Texture: array/cubemap/per-slice RTV は DX12 フォールバック "
                      "バックエンドでは未対応 (array_size=%u cubemap=%d per_slice_rtv=%d)。"
                      "Diligent バックエンドを使用してください",
                      desc.array_size, desc.is_cubemap ? 1 : 0,
                      desc.per_slice_rtv ? 1 : 0);
        r.hr = E_NOTIMPL;
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
    if (r.IsErr()) {
        ACS_LOG_ERROR("Dx12Texture CreateCommittedResource failed: hr=0x%08X "
                      "fmt=%d %ux%u rt=%d depth=%d",
                      static_cast<unsigned>(r.hr), static_cast<int>(desc.format),
                      desc.width, desc.height,
                      desc.is_render_target ? 1 : 0, desc.is_depth_target ? 1 : 0);
        return r;
    }

    // ===== 1b. 深度バッファの DSV + （任意）SRV =====
    if (desc.is_depth_target) {
        m_DsvSlot = device.AllocateDsvSlot();
        if (m_DsvSlot < 0) { r.hr = E_OUTOFMEMORY; return r; }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = dsv_fmt;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        device.D3DDevice()->CreateDepthStencilView(
            m_Resource, &dsv, device.DsvCpuHandle(m_DsvSlot));

        // シェーダから読みたい場合のみ SRV も作る
        if (desc.shader_visible_depth && depth_srv_fmt != DXGI_FORMAT_UNKNOWN) {
            m_SrvSlot = device.AllocateSrvSlot();
            if (m_SrvSlot < 0) { r.hr = E_OUTOFMEMORY; return r; }
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

    // ===== 2. 初期データがあれば UPLOAD ヒープ経由でコピー =====
    if (desc.initial_data && desc.initial_data_size > 0) {
        ID3D12Device* dev = device.D3DDevice();

        // mip_levels>1 で initial_data を渡されても、ここでアップロードするのは
        // subresource 0 (mip 0) のみ。上位 mip は未初期化のままなので、黙って
        // 部分初期化にせず警告を出す (Diligent バックエンドの挙動に合わせる)。
        if (desc.mip_levels > 1) {
            ACS_LOG_WARN("Dx12Texture: initial_data は mip 0 のみアップロードされ、"
                         "mip_levels=%u の上位 mip は未初期化になります", desc.mip_levels);
        }

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
        // tightly-packed なソース行ピッチ。これに対し initial_data_size を検証しないと、
        // 呼び出し側が想定より小さいバッファを渡した場合に src + y*pitch の memcpy が
        // バッファ外を読む (OOB read)。期待バイト数 = src_row_pitch * num_rows を計算し、
        // initial_data_size と突き合わせる。
        const u64 src_row_pitch = static_cast<u64>(desc.width) * bpp;
        // 行ごとに実際にコピーするバイト数 (DEFAULT 側の行サイズと src 行サイズの小さい方)。
        const u64 copy_row_len  = src_row_pitch < row_size_bytes ? src_row_pitch : row_size_bytes;
        const u64 expected_src  = src_row_pitch * static_cast<u64>(num_rows);
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
            upload->Release();
            r.hr = E_INVALIDARG;
            return r;
        }
        u8* dst = mapped + fp.Offset;
        for (u32 y = 0; y < num_rows; ++y) {
            ::memcpy(dst + static_cast<usize>(y) * fp.Footprint.RowPitch,
                     src + static_cast<usize>(y) * src_row_pitch,
                     static_cast<usize>(copy_row_len));
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

        cl->Close();
        ID3D12CommandList* lists[] = { cl };
        device.GraphicsQueue()->ExecuteCommandLists(1, lists);
        device.WaitIdle();    // 完了待ち（簡易）

        cl->Release();
        alloc->Release();
        upload->Release();
    }

    // ===== 4. SRV ヒープに SRV を作成 =====
    m_SrvSlot = device.AllocateSrvSlot();
    if (m_SrvSlot < 0) { ACS_LOG_ERROR("Dx12Texture: SRV slot exhausted"); r.hr = E_OUTOFMEMORY; return r; }
    m_CurrentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = typed_fmt;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = static_cast<UINT>(desc.mip_levels);
    device.D3DDevice()->CreateShaderResourceView(
        m_Resource, &srv, device.SrvCpuHandle(m_SrvSlot));

    // ===== 5. オフスクリーン RT は RTV も作成 (BeginRenderToTexture 用) =====
    if (desc.is_render_target) {
        m_RtvSlot = device.AllocateRtvSlot();
        if (m_RtvSlot < 0) { ACS_LOG_ERROR("Dx12Texture: RTV slot exhausted"); r.hr = E_OUTOFMEMORY; return r; }
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format        = typed_fmt;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device.D3DDevice()->CreateRenderTargetView(
            m_Resource, &rtv, device.RtvCpuHandle(m_RtvSlot));
    }

    return r;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Texture::RtvCpuHandle() const noexcept {
    return m_Device ? m_Device->RtvCpuHandle(m_RtvSlot) : D3D12_CPU_DESCRIPTOR_HANDLE{0};
}

D3D12_GPU_DESCRIPTOR_HANDLE Dx12Texture::SrvGpuHandle() const noexcept {
    return m_Device ? m_Device->SrvGpuHandle(m_SrvSlot) : D3D12_GPU_DESCRIPTOR_HANDLE{0};
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Texture::DsvCpuHandle() const noexcept {
    return m_Device ? m_Device->DsvCpuHandle(m_DsvSlot) : D3D12_CPU_DESCRIPTOR_HANDLE{0};
}

// ファクトリ（Diligent バックエンドが有効化されている場合は Diligent 側に譲る）
#if !WITH_RENDER_DILIGENT
TResult<TUniquePtr<IRhiTexture>> CreateRhiTexture(IRhiDevice& device,
                                                const FTextureDesc& desc) noexcept {
    const char* bn = device.BackendName();
    if (!(bn[0] == 'D' && bn[1] == 'X' && bn[2] == '1' && bn[3] == '2'))
        return ACS_ERR(Render, 70, "CreateRhiTexture: device is not DX12");
    Dx12Device* dxd = static_cast<Dx12Device*>(&device);
    auto t = MakeUnique<Dx12Texture>();
    HrResult r = t->Init(*dxd, desc);
    if (r.IsErr())
        return ACS_ERR_OS(Render, 71, "Dx12Texture::Init failed", static_cast<u32>(r.hr));
    TUniquePtr<IRhiTexture> base(t.Release(), t.GetAllocator());
    return TResult<TUniquePtr<IRhiTexture>>(OkInit, Move(base));
}
#endif

} // namespace acs
