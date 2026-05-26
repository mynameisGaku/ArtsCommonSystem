// SPDX-License-Identifier: Apache-2.0
// メッシュアセット
//
// 対応拡張子: gltf / glb / obj / fbx
//
// 単一メッシュ = 頂点配列 + インデックス配列 + サブメッシュ範囲。
// 頂点フォーマットは「位置 + 法線 + UV」固定（v1）。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "container/Array.h"
#include "asset/Asset.h"
#include "asset/IAssetLoader.h"
#include "container/Hash.h"

namespace acs {

// 標準頂点フォーマット (32 bytes)
struct MeshVertex {
    FVec3 position;
    FVec3 normal;
    f32  u;
    f32  v;
};

// サブメッシュ（複数マテリアル対応用、開始 index と数だけ持つ）
struct SubMesh {
    u32 first_index = 0;
    u32 index_count = 0;
};

class MeshAsset : public Asset {
public:
    ACS_ASSET_TYPE("MeshAsset")

    MeshAsset() noexcept = default;

    const TArray<MeshVertex>& Vertices()  const noexcept { return _vertices; }
    TArray<MeshVertex>&       Vertices()        noexcept { return _vertices; }
    const TArray<u32>&        Indices()   const noexcept { return _indices; }
    TArray<u32>&              Indices()         noexcept { return _indices; }
    const TArray<SubMesh>&    SubMeshes() const noexcept { return _submeshes; }
    TArray<SubMesh>&          SubMeshes()       noexcept { return _submeshes; }

private:
    TArray<MeshVertex> _vertices;
    TArray<u32>        _indices;
    TArray<SubMesh>    _submeshes;
};

// glTF / GLB ローダ (cgltf)
class GltfAssetLoader final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return MeshAsset::StaticType(); }
    const char* Extension() const noexcept override { return "gltf"; }
    TResult<TRc<Asset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

class GlbAssetLoader final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return MeshAsset::StaticType(); }
    const char* Extension() const noexcept override { return "glb"; }
    TResult<TRc<Asset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

// Wavefront OBJ ローダ（自前パーサ）
class ObjAssetLoader final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return MeshAsset::StaticType(); }
    const char* Extension() const noexcept override { return "obj"; }
    TResult<TRc<Asset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

// FBX ローダ (ufbx)
class FbxAssetLoader final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return MeshAsset::StaticType(); }
    const char* Extension() const noexcept override { return "fbx"; }
    TResult<TRc<Asset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

} // namespace acs
