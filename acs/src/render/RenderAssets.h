// SPDX-License-Identifier: Apache-2.0
// FAsset → GPU リソース変換ヘルパ
//
// FImageAsset を GPU テクスチャに、FMeshAsset を頂点+インデックスバッファに変換する
// 高レベル関数群。ゲーム側コードはこの関数を呼ぶだけで描画できる。
//
// 使い方:
//   auto img = registry.Load(L"hero.png").Value();
//   auto tex = UploadTexture(*device, *img).Value();
//
//   auto m   = registry.Load(L"cube.gltf").Value();
//   FGpuMesh gm;
//   UploadMesh(*device, *m, gm);
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/IRhiDevice.h"
#include "render/IRhiTexture.h"
#include "render/IRhiBuffer.h"

namespace acs {

class FImageAsset;
class FMeshAsset;
class FSkinnedMeshAsset;

/**
 * 画像アセットを GPU テクスチャにアップロードする (同期、戻った時点で即座に使用可能)。
 *
 * @param device テクスチャ生成に使う RHI デバイス。
 * @param img アップロード元の画像アセット。
 * @return 生成したテクスチャを所有する TUniquePtr、失敗ならエラー。
 */
TResult<TUniquePtr<IRhiTexture>> UploadTexture(IRhiDevice& device, const FImageAsset& img) noexcept;

/**
 * メッシュ 1 つ分の GPU バッファセット (頂点バッファ + インデックスバッファ)。
 */
struct FGpuMesh {
    /** 頂点バッファ (所有権を持つ)。 */
    TUniquePtr<IRhiBuffer> vertex_buffer;

    /** インデックスバッファ (所有権を持つ)。 */
    TUniquePtr<IRhiBuffer> index_buffer;

    /** 頂点数。 */
    u32                    vertex_count = 0;

    /** インデックス数。 */
    u32                    index_count  = 0;

    /** 1 頂点のバイト数 (stride)。 */
    u32                    vertex_stride = 0;
};

/**
 * メッシュアセットから FGpuMesh を作る (位置 + 法線 + UV、stride=32B)。
 *
 * @param device バッファ生成に使う RHI デバイス。
 * @param mesh アップロード元のメッシュアセット。
 * @param out 生成した VB/IB と各カウントを書き込む出力先。
 * @return 成功なら空の TResult、失敗ならエラー。
 */
TResult<void> UploadMesh(IRhiDevice& device, const FMeshAsset& mesh, FGpuMesh& out) noexcept;

/**
 * スキンメッシュ 1 つ分の GPU バッファセット (CSkinnedShader が消費する形式)。
 */
struct FSkinnedGpuMesh {
    /** 頂点バッファ (所有権を持つ)。 */
    TUniquePtr<IRhiBuffer> vertex_buffer;

    /** インデックスバッファ (所有権を持つ)。 */
    TUniquePtr<IRhiBuffer> index_buffer;

    /** 頂点数。 */
    u32 vertex_count = 0;

    /** インデックス数。 */
    u32 index_count  = 0;

    /** 1 頂点のバイト数 (FSkinnedVertex のサイズ)。 */
    u32 vertex_stride = 0;
};

/**
 * スキンメッシュアセットから FSkinnedGpuMesh を作る。
 *
 * @param device バッファ生成に使う RHI デバイス。
 * @param mesh アップロード元のスキンメッシュアセット。
 * @param out 生成した VB/IB と各カウントを書き込む出力先。
 * @return 成功なら空の TResult、失敗ならエラー。
 */
TResult<void> UploadSkinnedMesh(IRhiDevice& device,
                                const FSkinnedMeshAsset& mesh,
                                FSkinnedGpuMesh& out) noexcept;

} // namespace acs
