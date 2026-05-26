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

// 画像アセットを GPU テクスチャにアップロード（同期、即座に使用可能）
TResult<TUniquePtr<IRhiTexture>> UploadTexture(IRhiDevice& device, const FImageAsset& img) noexcept;

// メッシュ用の VB + IB のセット
struct FGpuMesh {
    TUniquePtr<IRhiBuffer> vertex_buffer;
    TUniquePtr<IRhiBuffer> index_buffer;
    u32                    vertex_count = 0;
    u32                    index_count  = 0;
    u32                    vertex_stride = 0;
};

// メッシュアセットから FGpuMesh を作る（位置 + 法線 + UV、stride=32B）
TResult<void> UploadMesh(IRhiDevice& device, const FMeshAsset& mesh, FGpuMesh& out) noexcept;

// スキンメッシュ用の GPU バッファセット（FSkinnedShader が消費する形式）
struct FSkinnedGpuMesh {
    TUniquePtr<IRhiBuffer> vertex_buffer;
    TUniquePtr<IRhiBuffer> index_buffer;
    u32 vertex_count = 0;
    u32 index_count  = 0;
    u32 vertex_stride = 0;        // FSkinnedVertex のサイズ
};

// FSkinnedMeshAsset → FSkinnedGpuMesh
TResult<void> UploadSkinnedMesh(IRhiDevice& device,
                                const FSkinnedMeshAsset& mesh,
                                FSkinnedGpuMesh& out) noexcept;

} // namespace acs
