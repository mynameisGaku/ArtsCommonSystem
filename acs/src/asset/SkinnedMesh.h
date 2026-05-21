// SPDX-License-Identifier: Apache-2.0
// スキンメッシュ（ボーン + アニメーション）
//
// 構成:
//   SkinnedMeshAsset = 頂点（位置+法線+UV+ボーン indices/weights）
//                    + インデックス
//                    + ボーン階層
//                    + アニメーション群
//
// ランタイム:
//   AnimationPlayer がアニメーションをスキャンして
//   GPU 用ボーンパレット (Mat4×N) を毎フレーム計算する。
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
struct SkinnedVertex {
    Vec3 position;       // alignas 16 → 16 byte
    Vec3 normal;         // alignas 16 → 16 byte
    f32  u, v;           // 8 byte
    u8   bones[4];       // 4 byte（最大 4 ボーン影響、未使用は 0 埋め）
    f32  weights[4];     // 16 byte（合計 = 1.0、未使用は 0）
};
// sizeof = 60 + 4 padding = 64

// ===== ボーン =====
struct Bone {
    String name;
    i32    parent = -1;                // -1 = ルート
    Vec3   bind_translation = Vec3{0, 0, 0};
    Quat   bind_rotation    = Quat{};   // identity
    Vec3   bind_scale       = Vec3{1, 1, 1};

    // バインドポーズワールドの逆行列（後で初期化）
    // すべての bind_* が設定されてから ComputeInverseBindMatrices() で計算する
    Mat4   inverse_bind     = Mat4::Identity();
};

// ===== アニメーションキー =====
// 簡易版: 1 つのキーが TRS まとめて持つ（glTF と異なるが MVP として十分）
struct AnimationKey {
    f32  time = 0.0f;
    Vec3 translation = Vec3{0, 0, 0};
    Quat rotation    = Quat{};
    Vec3 scale       = Vec3{1, 1, 1};
};

// ===== アニメーションチャネル =====
struct AnimationChannel {
    i32                 bone_index = -1;
    Array<AnimationKey> keys;             // 時刻昇順
};

// ===== アニメーション =====
struct Animation {
    String                  name;
    f32                     duration = 0.0f;
    Array<AnimationChannel> channels;
};

// ===== SkinnedMeshAsset =====
class SkinnedMeshAsset : public Asset {
public:
    ACS_ASSET_TYPE("SkinnedMeshAsset")

    SkinnedMeshAsset() noexcept = default;

    Array<SkinnedVertex>& Vertices()     noexcept { return _vertices; }
    Array<u32>&           Indices()      noexcept { return _indices; }
    Array<Bone>&          Bones()        noexcept { return _bones; }
    Array<Animation>&     Animations()   noexcept { return _animations; }

    const Array<SkinnedVertex>& Vertices()   const noexcept { return _vertices; }
    const Array<u32>&           Indices()    const noexcept { return _indices; }
    const Array<Bone>&          Bones()      const noexcept { return _bones; }
    const Array<Animation>&     Animations() const noexcept { return _animations; }

    // 全 bind_* が設定されてから 1 度呼ぶ。
    // Bone::inverse_bind を計算する。
    void ComputeInverseBindMatrices() noexcept;

private:
    Array<SkinnedVertex> _vertices;
    Array<u32>           _indices;
    Array<Bone>          _bones;
    Array<Animation>     _animations;
};

// =============================================================================
// AnimationPlayer — アニメ再生 + ボーンパレット計算
// =============================================================================
//
// 使い方:
//   AnimationPlayer ap;
//   ap.SetMesh(&mesh);
//   ap.Play(0, /*loop=*/true);
//   ...
//   ap.Update(dt);
//   Mat4 palette[64];
//   u32  count = ap.WritePalette(palette, 64);
//   shader.SetBonePalette(palette, count);
class AnimationPlayer {
public:
    AnimationPlayer() noexcept = default;

    void SetMesh(const SkinnedMeshAsset* mesh) noexcept { _mesh = mesh; _anim = -1; _time = 0; }
    void Play(u32 anim_index, bool loop = true) noexcept;
    void Pause() noexcept { _playing = false; }
    void Resume() noexcept { _playing = true; }
    void Stop() noexcept { _playing = false; _time = 0; }

    void SetTime(f32 t) noexcept { _time = t; }
    f32  Time() const noexcept { return _time; }
    bool IsPlaying() const noexcept { return _playing; }

    void Update(f32 dt) noexcept;

    // 現在の time から palette を書き込む。書き込んだボーン数を返す。
    // out_palette はボーン数ぶん（最大 max_count）の Mat4 を保持する領域。
    u32 WritePalette(Mat4* out_palette, u32 max_count) const noexcept;

private:
    const SkinnedMeshAsset* _mesh    = nullptr;
    i32                     _anim    = -1;       // -1 = T-pose（バインド姿勢のまま）
    f32                     _time    = 0.0f;
    bool                    _loop    = true;
    bool                    _playing = false;
};

} // namespace acs
