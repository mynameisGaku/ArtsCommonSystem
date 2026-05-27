// SPDX-License-Identifier: Apache-2.0
// スキンメッシュ（ボーン + アニメーション）
//
// 構成:
//   FSkinnedMeshAsset = 頂点（位置+法線+UV+ボーン indices/weights）
//                    + インデックス
//                    + ボーン階層
//                    + アニメーション群
//
// ランタイム:
//   FAnimationPlayer がアニメーションをスキャンして
//   GPU 用ボーンパレット (FMat4×N) を毎フレーム計算する。
//
// MVP では glTF パースは省略し、ランタイムでプログラム的に
// データを構築する API のみ提供する（asset/MeshPrimitive と同パターン）。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "math/Quat.h"
#include "container/Array.h"
#include "container/String.h"
#include "asset/Asset.h"

namespace acs {

// ===== 1 頂点（64 バイト固定、入力レイアウト 5 要素）=====
//   POSITION  : R32G32B32_Float    @  0
//   NORMAL    : R32G32B32_Float    @ 16
//   TEXCOORD  : R32G32_Float       @ 32
//   BLENDINDICES : R8G8B8A8_UINT   @ 40
//   BLENDWEIGHT  : R32G32B32A32_Float @ 44
struct FSkinnedVertex {
    FVec3 position;       // alignas 16 → 16 byte
    FVec3 normal;         // alignas 16 → 16 byte
    f32  u, v;           // 8 byte
    u8   bones[4];       // 4 byte（最大 4 ボーン影響、未使用は 0 埋め）
    f32  weights[4];     // 16 byte（合計 = 1.0、未使用は 0）
};
// sizeof = 60 + 4 padding = 64

// ===== ボーン =====
struct FBone {
    FString name;
    i32    parent = -1;                // -1 = ルート
    FVec3   bind_translation = FVec3{0, 0, 0};
    FQuat   bind_rotation    = FQuat{};   // identity
    FVec3   bind_scale       = FVec3{1, 1, 1};

    // バインドポーズワールドの逆行列（後で初期化）
    // すべての bind_* が設定されてから ComputeInverseBindMatrices() で計算する
    FMat4   inverse_bind     = FMat4::Identity();
};

// ===== アニメーションキー =====
// 簡易版: 1 つのキーが TRS まとめて持つ（glTF と異なるが MVP として十分）
struct FAnimationKey {
    f32  time = 0.0f;
    FVec3 translation = FVec3{0, 0, 0};
    FQuat rotation    = FQuat{};
    FVec3 scale       = FVec3{1, 1, 1};
};

// ===== アニメーションチャネル =====
struct FAnimationChannel {
    i32                 bone_index = -1;
    TArray<FAnimationKey> keys;             // 時刻昇順
};

// ===== アニメーション =====
struct FAnimation {
    FString                  name;
    f32                     duration = 0.0f;
    TArray<FAnimationChannel> channels;
};

// ===== FSkinnedMeshAsset =====
class FSkinnedMeshAsset : public Asset {
public:
    ACS_ASSET_TYPE("FSkinnedMeshAsset")

    FSkinnedMeshAsset() noexcept = default;

    TArray<FSkinnedVertex>& Vertices()     noexcept { return m_Vertices; }
    TArray<u32>&           Indices()      noexcept { return m_Indices; }
    TArray<FBone>&          Bones()        noexcept { return m_Bones; }
    TArray<FAnimation>&     Animations()   noexcept { return m_Animations; }

    const TArray<FSkinnedVertex>& Vertices()   const noexcept { return m_Vertices; }
    const TArray<u32>&           Indices()    const noexcept { return m_Indices; }
    const TArray<FBone>&          Bones()      const noexcept { return m_Bones; }
    const TArray<FAnimation>&     Animations() const noexcept { return m_Animations; }

    // 全 bind_* が設定されてから 1 度呼ぶ。
    // FBone::inverse_bind を計算する。
    void ComputeInverseBindMatrices() noexcept;

private:
    TArray<FSkinnedVertex> m_Vertices;
    TArray<u32>           m_Indices;
    TArray<FBone>          m_Bones;
    TArray<FAnimation>     m_Animations;
};

// =============================================================================
// FAnimationPlayer — アニメ再生 + ボーンパレット計算
// =============================================================================
//
// 使い方:
//   FAnimationPlayer ap;
//   ap.SetMesh(&mesh);
//   ap.Play(0, /*loop=*/true);
//   ...
//   ap.Update(dt);
//   FMat4 palette[64];
//   u32  count = ap.WritePalette(palette, 64);
//   shader.SetBonePalette(palette, count);
class FAnimationPlayer {
public:
    FAnimationPlayer() noexcept = default;

    void SetMesh(const FSkinnedMeshAsset* mesh) noexcept { m_Mesh = mesh; m_Anim = -1; m_Time = 0; }
    void Play(u32 anim_index, bool loop = true) noexcept;
    void Pause() noexcept { m_Playing = false; }
    void Resume() noexcept { m_Playing = true; }
    void Stop() noexcept { m_Playing = false; m_Time = 0; }

    void SetTime(f32 t) noexcept { m_Time = t; }
    f32  Time() const noexcept { return m_Time; }
    bool IsPlaying() const noexcept { return m_Playing; }

    void Update(f32 dt) noexcept;

    // 現在の time から palette を書き込む。書き込んだボーン数を返す。
    // out_palette はボーン数ぶん（最大 max_count）の FMat4 を保持する領域。
    u32 WritePalette(FMat4* out_palette, u32 max_count) const noexcept;

private:
    const FSkinnedMeshAsset* m_Mesh    = nullptr;
    i32                     m_Anim    = -1;       // -1 = T-pose（バインド姿勢のまま）
    f32                     m_Time    = 0.0f;
    bool                    m_bLoop    = true;
    bool                    m_Playing = false;
};

} // namespace acs
