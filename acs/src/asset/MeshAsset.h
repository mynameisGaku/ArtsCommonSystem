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

/** 標準頂点フォーマット (32 bytes、位置 + 法線 + UV)。 */
struct FMeshVertex {
    /** 頂点位置 (オブジェクト空間)。 */
    FVec3 position;

    /** 頂点法線。 */
    FVec3 normal;

    /** テクスチャ座標 U。 */
    f32  u;

    /** テクスチャ座標 V。 */
    f32  v;
};

/**
 * サブメッシュ (複数マテリアル対応用にインデックス範囲だけを持つ)。
 *
 * @details インデックスバッファ中の [first_index, first_index + index_count) を 1 つの描画単位とする。
 */
struct FSubMesh {
    /** インデックスバッファ中の開始インデックス。 */
    u32 first_index = 0;

    /** このサブメッシュが使うインデックス数。 */
    u32 index_count = 0;
};

/**
 * 単一の静的メッシュアセット (頂点配列 + インデックス配列 + サブメッシュ範囲)。
 *
 * @details 頂点は FMeshVertex (位置 + 法線 + UV) 固定。各ローダ (glTF/GLB/OBJ/FBX) が
 * 全プリミティブを 1 つの本アセットへ flatten して構築する。
 */
class AMeshAsset : public AAsset {
public:
    ACS_ASSET_TYPE("FMeshAsset")

    /** 空のメッシュアセットを構築する。 */
    AMeshAsset() noexcept = default;

    /**
     * 頂点配列への const 参照を返す。
     *
     * @return 頂点配列への const 参照。
     */
    const TArray<FMeshVertex>& Vertices()  const noexcept { return m_Vertices; }

    /**
     * 頂点配列への可変参照を返す。
     *
     * @return 頂点配列への可変参照。
     */
    TArray<FMeshVertex>&       Vertices()        noexcept {
        MarkGeometryDirty();
        return m_Vertices;
    }

    /**
     * インデックス配列への const 参照を返す。
     *
     * @return インデックス配列への const 参照。
     */
    const TArray<u32>&        Indices()   const noexcept { return m_Indices; }

    /**
     * インデックス配列への可変参照を返す。
     *
     * @return インデックス配列への可変参照。
     */
    TArray<u32>&              Indices()         noexcept {
        MarkGeometryDirty();
        return m_Indices;
    }

    /**
     * サブメッシュ配列への const 参照を返す。
     *
     * @return サブメッシュ配列への const 参照。
     */
    const TArray<FSubMesh>&    SubMeshes() const noexcept { return m_Submeshes; }

    /**
     * サブメッシュ配列への可変参照を返す。
     *
     * @return サブメッシュ配列への可変参照。
     */
    TArray<FSubMesh>&          SubMeshes()       noexcept {
        MarkGeometryDirty();
        return m_Submeshes;
    }

    /**
     * Monotonic content revision used by geometry-dependent render caches.
     *
     * Mutable array access conservatively advances this value. Code retaining
     * a mutable reference across frames must call MarkGeometryDirty() after
     * each later mutation.
     */
    u64 GeometryRevision() const noexcept { return m_GeometryRevision; }
    void MarkGeometryDirty() noexcept {
        ++m_GeometryRevision;
        if (m_GeometryRevision == 0u) {
            m_GeometryRevision = 1u;
        }
    }

private:
    /** 頂点配列 (位置 + 法線 + UV)。 */
    TArray<FMeshVertex> m_Vertices;

    /** インデックス配列。 */
    TArray<u32>        m_Indices;

    /** サブメッシュ範囲の配列。 */
    TArray<FSubMesh>    m_Submeshes;

    /** Monotonic revision for geometry-dependent render caches. */
    u64 m_GeometryRevision = 1u;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FMeshAsset = AMeshAsset;

/**
 * glTF (.gltf) を AMeshAsset へロードするローダ (cgltf 使用)。
 *
 * @details v1 では外部バイナリ参照は非対応で、埋め込みバッファのみサポートする。
 */
class CGltfAssetLoader final : public IAssetLoader {
public:
    /**
     * このローダが生成するアセット型を返す。
     *
     * @return AMeshAsset の静的型 ID。
     */
    AssetType   TypeId()    const noexcept override { return AMeshAsset::StaticType(); }

    /**
     * 対応する拡張子を返す。
     *
     * @return "gltf"。
     */
    const char* Extension() const noexcept override { return "gltf"; }

    /**
     * バイト列を glTF としてパースし AMeshAsset を構築する。
     *
     * @param id 生成するアセットに割り当てる ID。
     * @param bytes .gltf のバイト列。
     * @return 成功なら Ready 状態の AMeshAsset、パース/バッファロード失敗ならエラー。
     */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FGltfAssetLoader = CGltfAssetLoader;

/** GLB (.glb) を AMeshAsset へロードするローダ (cgltf 使用)。 */
class CGlbAssetLoader final : public IAssetLoader {
public:
    /**
     * このローダが生成するアセット型を返す。
     *
     * @return AMeshAsset の静的型 ID。
     */
    AssetType   TypeId()    const noexcept override { return AMeshAsset::StaticType(); }

    /**
     * 対応する拡張子を返す。
     *
     * @return "glb"。
     */
    const char* Extension() const noexcept override { return "glb"; }

    /**
     * バイト列を GLB としてパースし AMeshAsset を構築する。
     *
     * @param id 生成するアセットに割り当てる ID。
     * @param bytes .glb のバイト列。
     * @return 成功なら Ready 状態の AMeshAsset、パース/バッファロード失敗ならエラー。
     */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FGlbAssetLoader = CGlbAssetLoader;

/**
 * Wavefront OBJ (.obj) を AMeshAsset へロードするローダ (自前パーサ)。
 *
 * @details v / vn / vt / f のみ対応し、マテリアルは無視する。
 */
class CObjAssetLoader final : public IAssetLoader {
public:
    /**
     * このローダが生成するアセット型を返す。
     *
     * @return AMeshAsset の静的型 ID。
     */
    AssetType   TypeId()    const noexcept override { return AMeshAsset::StaticType(); }

    /**
     * 対応する拡張子を返す。
     *
     * @return "obj"。
     */
    const char* Extension() const noexcept override { return "obj"; }

    /**
     * バイト列を OBJ としてパースし AMeshAsset を構築する。
     *
     * @param id 生成するアセットに割り当てる ID。
     * @param bytes .obj のバイト列。
     * @return 構築した Ready 状態の AMeshAsset。
     */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FObjAssetLoader = CObjAssetLoader;

/**
 * FBX (.fbx) を AMeshAsset へロードするローダ (ufbx 使用)。
 *
 * @details ufbx の三角形化ヘルパで全メッシュを三角形へ分割して取り込む。
 */
class CFbxAssetLoader final : public IAssetLoader {
public:
    /**
     * このローダが生成するアセット型を返す。
     *
     * @return AMeshAsset の静的型 ID。
     */
    AssetType   TypeId()    const noexcept override { return AMeshAsset::StaticType(); }

    /**
     * 対応する拡張子を返す。
     *
     * @return "fbx"。
     */
    const char* Extension() const noexcept override { return "fbx"; }

    /**
     * バイト列を FBX としてパースし AMeshAsset を構築する。
     *
     * @param id 生成するアセットに割り当てる ID。
     * @param bytes .fbx のバイト列。
     * @return 成功なら Ready 状態の AMeshAsset、ロード失敗ならエラー。
     */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FFbxAssetLoader = CFbxAssetLoader;

} // namespace acs
