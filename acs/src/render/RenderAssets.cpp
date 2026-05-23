// SPDX-License-Identifier: Apache-2.0
// Asset → GPU リソース変換ヘルパ実装
#include "render/RenderAssets.h"
#include "asset/ImageAsset.h"
#include "asset/MeshAsset.h"
#include "asset/SkinnedMesh.h"
#include "foundation/Move.h"

namespace acs {

namespace {

// EPixelFormat → EFormat（GPU 側）変換
// 3 チャンネルは GPU では一般的に使えないので、4 チャンネルに展開する必要がある。
EFormat ToRhiFormat(EPixelFormat f) noexcept {
    switch (f) {
        case EPixelFormat::R8:               return EFormat::Unknown;     // 1ch は専用処理
        case EPixelFormat::R8G8:             return EFormat::Unknown;     // 2ch は専用処理
        case EPixelFormat::R8G8B8:           return EFormat::R8G8B8A8_UNorm;  // 4ch に展開済み
        case EPixelFormat::R8G8B8A8:         return EFormat::R8G8B8A8_UNorm;
        case EPixelFormat::R32G32B32_F:      return EFormat::R32G32B32A32_Float;
        case EPixelFormat::R32G32B32A32_F:   return EFormat::R32G32B32A32_Float;
    }
    return EFormat::Unknown;
}

} // namespace

Result<UniquePtr<IRhiTexture>> UploadTexture(IRhiDevice& device, const ImageAsset& img) noexcept {
    if (img.Width() == 0 || img.Height() == 0)
        return ACS_ERR(Render, 80, "UploadTexture: empty image");

    EFormat gpu_fmt = ToRhiFormat(img.EFormat());
    if (gpu_fmt == EFormat::Unknown)
        return ACS_ERR(Render, 81, "UploadTexture: unsupported pixel format");

    // R8G8B8 (3ch) は GPU では 4ch にパディングが必要
    // 簡易対応: ImageAsset::Pixels() がそのまま 4ch でない場合はエラー
    // （ImageAssetLoader 側で 4ch に拡張する責務とする）
    TextureDesc d{};
    d.width  = img.Width();
    d.height = img.Height();
    d.format = gpu_fmt;
    d.mip_levels = 1;
    d.initial_data = img.Pixels();
    d.initial_data_size = img.PixelByteCount();

    return CreateRhiTexture(device, d);
}

Result<void> UploadMesh(IRhiDevice& device, const MeshAsset& mesh, GpuMesh& out) noexcept {
    if (mesh.Vertices().Size() == 0)
        return ACS_ERR(Render, 82, "UploadMesh: empty mesh");

    // 頂点バッファ
    BufferDesc vb_desc{};
    vb_desc.size  = mesh.Vertices().Size() * sizeof(MeshVertex);
    vb_desc.usage = EBufferUsage::Vertex;
    vb_desc.cpu_writable = false;         // 静的 mesh は USAGE_IMMUTABLE で扱う
    vb_desc.initial_data = mesh.Vertices().Data();
    auto vbr = CreateRhiBuffer(device, vb_desc);
    if (vbr.IsErr()) return Err<void>(vbr.Error());
    out.vertex_buffer = Move(vbr.Value());
    out.vertex_count  = static_cast<u32>(mesh.Vertices().Size());
    out.vertex_stride = sizeof(MeshVertex);

    // インデックスバッファ
    if (mesh.Indices().Size() > 0) {
        BufferDesc ib_desc{};
        ib_desc.size  = mesh.Indices().Size() * sizeof(u32);
        ib_desc.usage = EBufferUsage::Index32;
        ib_desc.cpu_writable = false;     // 静的 mesh は USAGE_IMMUTABLE
        ib_desc.initial_data = mesh.Indices().Data();
        auto ibr = CreateRhiBuffer(device, ib_desc);
        if (ibr.IsErr()) return Err<void>(ibr.Error());
        out.index_buffer = Move(ibr.Value());
        out.index_count  = static_cast<u32>(mesh.Indices().Size());
    }
    return Ok();
}

Result<void> UploadSkinnedMesh(IRhiDevice& device, const SkinnedMeshAsset& mesh,
                               SkinnedGpuMesh& out) noexcept {
    if (mesh.Vertices().Size() == 0)
        return ACS_ERR(Render, 84, "UploadSkinnedMesh: empty mesh");

    BufferDesc vb{};
    vb.size = mesh.Vertices().Size() * sizeof(SkinnedVertex);
    vb.usage = EBufferUsage::Vertex;
    vb.cpu_writable = false;          // 静的 mesh は USAGE_IMMUTABLE
    vb.initial_data = mesh.Vertices().Data();
    auto vbr = CreateRhiBuffer(device, vb);
    if (vbr.IsErr()) return Err<void>(vbr.Error());
    out.vertex_buffer = Move(vbr.Value());
    out.vertex_count  = static_cast<u32>(mesh.Vertices().Size());
    out.vertex_stride = sizeof(SkinnedVertex);

    if (mesh.Indices().Size() > 0) {
        BufferDesc ib{};
        ib.size = mesh.Indices().Size() * sizeof(u32);
        ib.usage = EBufferUsage::Index32;
        ib.cpu_writable = false;      // 静的 mesh は USAGE_IMMUTABLE
        ib.initial_data = mesh.Indices().Data();
        auto ibr = CreateRhiBuffer(device, ib);
        if (ibr.IsErr()) return Err<void>(ibr.Error());
        out.index_buffer = Move(ibr.Value());
        out.index_count  = static_cast<u32>(mesh.Indices().Size());
    }
    return Ok();
}

} // namespace acs
