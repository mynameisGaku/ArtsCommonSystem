// SPDX-License-Identifier: Apache-2.0
// Asset → GPU リソース変換ヘルパ実装
#include "render/RenderAssets.h"
#include "asset/ImageAsset.h"
#include "asset/MeshAsset.h"
#include "asset/SkinnedMesh.h"
#include "foundation/Move.h"

namespace acs {

namespace {

/**
 * EPixelFormat を GPU 側の EFormat に変換する。
 *
 * @details
 * 3 チャンネルは GPU では一般的に使えないため 4 チャンネルへ展開した EFormat を返す。
 * 1ch / 2ch は専用処理が必要なため EFormat::Unknown を返す。
 * @param f 変換元の CPU 側ピクセルフォーマット。
 * @return 対応する GPU 側 EFormat。未対応なら EFormat::Unknown。
 */
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

/**
 * 画像アセットを GPU テクスチャにアップロードする（同期、即座に使用可能）。
 *
 * @param device テクスチャ生成に使う RHI デバイス。
 * @param img アップロード元の画像アセット。
 * @return 成功なら所有権付きの IRhiTexture、空画像・未対応フォーマット・生成失敗ならエラー。
 */
TResult<TUniquePtr<IRhiTexture>> UploadTexture(IRhiDevice& device, const FImageAsset& img) noexcept {
    if (img.Width() == 0 || img.Height() == 0)
        return ACS_ERR(Render, 80, "UploadTexture: empty image");

    const EFormat gpu_fmt = ToRhiFormat(img.Format());
    if (gpu_fmt == EFormat::Unknown)
        return ACS_ERR(Render, 81, "UploadTexture: unsupported pixel format");

    // R8G8B8 (3ch) は GPU では 4ch にパディングが必要
    // 簡易対応: FImageAsset::Pixels() がそのまま 4ch でない場合はエラー
    // （ImageAssetLoader 側で 4ch に拡張する責務とする）
    FTextureDesc d{};
    d.width  = img.Width();
    d.height = img.Height();
    d.format = gpu_fmt;
    d.mip_levels = 1;
    d.initial_data = img.Pixels();
    d.initial_data_size = img.PixelByteCount();

    return CreateRhiTexture(device, d);
}

/**
 * メッシュアセットから頂点バッファ・インデックスバッファを作って FGpuMesh に詰める。
 *
 * @details 静的 mesh は USAGE_IMMUTABLE で扱う。インデックスが無ければ IB は生成しない。
 * @param device バッファ生成に使う RHI デバイス。
 * @param mesh アップロード元のメッシュアセット。
 * @param out 生成した VB/IB と頂点数・インデックス数・ストライドの書き込み先。
 * @return 成功なら空の TResult、空メッシュ・バッファ生成失敗ならエラー。
 */
TResult<void> UploadMesh(IRhiDevice& device, const FMeshAsset& mesh, FGpuMesh& out) noexcept {
    if (mesh.Vertices().Size() == 0)
        return ACS_ERR(Render, 82, "UploadMesh: empty mesh");

    // 頂点バッファ
    FBufferDesc vb_desc{};
    vb_desc.size  = mesh.Vertices().Size() * sizeof(FMeshVertex);
    vb_desc.usage = EBufferUsage::Vertex;
    vb_desc.cpu_writable = false;         // 静的 mesh は USAGE_IMMUTABLE で扱う
    vb_desc.initial_data = mesh.Vertices().Data();
    auto vbr = CreateRhiBuffer(device, vb_desc);
    if (vbr.IsErr()) return Err<void>(vbr.Error());
    out.vertex_buffer = Move(vbr.Value());
    out.vertex_count  = static_cast<u32>(mesh.Vertices().Size());
    out.vertex_stride = sizeof(FMeshVertex);

    // インデックスバッファ
    if (mesh.Indices().Size() > 0) {
        FBufferDesc ib_desc{};
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

/**
 * スキンメッシュアセットから GPU バッファを作って SkinnedGpuMesh に詰める。
 *
 * @details
 * FSkinnedVertex 形式の頂点で VB を作る（FSkinnedShader が消費する形式）。静的 mesh は
 * USAGE_IMMUTABLE で扱い、インデックスが無ければ IB は生成しない。
 * @param device バッファ生成に使う RHI デバイス。
 * @param mesh アップロード元のスキンメッシュアセット。
 * @param out 生成した VB/IB と頂点数・インデックス数・ストライドの書き込み先。
 * @return 成功なら空の TResult、空メッシュ・バッファ生成失敗ならエラー。
 */
TResult<void> UploadSkinnedMesh(IRhiDevice& device, const FSkinnedMeshAsset& mesh,
                               FSkinnedGpuMesh& out) noexcept {
    if (mesh.Vertices().Size() == 0)
        return ACS_ERR(Render, 84, "UploadSkinnedMesh: empty mesh");

    FBufferDesc vb{};
    vb.size = mesh.Vertices().Size() * sizeof(FSkinnedVertex);
    vb.usage = EBufferUsage::Vertex;
    vb.cpu_writable = false;          // 静的 mesh は USAGE_IMMUTABLE
    vb.initial_data = mesh.Vertices().Data();
    auto vbr = CreateRhiBuffer(device, vb);
    if (vbr.IsErr()) return Err<void>(vbr.Error());
    out.vertex_buffer = Move(vbr.Value());
    out.vertex_count  = static_cast<u32>(mesh.Vertices().Size());
    out.vertex_stride = sizeof(FSkinnedVertex);

    if (mesh.Indices().Size() > 0) {
        FBufferDesc ib{};
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
