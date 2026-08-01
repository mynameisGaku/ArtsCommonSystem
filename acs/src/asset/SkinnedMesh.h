// SPDX-License-Identifier: Apache-2.0
// スキンメッシュ（ボーン + アニメーション）
//
// 構成:
//   ASkinnedMeshAsset = 頂点（位置+法線+UV+ボーン indices/weights）
//                    + インデックス
//                    + ボーン階層
//                    + アニメーション群
//
// ランタイム:
//   CAnimationPlayer がアニメーションをスキャンして
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

/**
 * スキニング用の 1 頂点 (64 バイト固定、入力レイアウト 5 要素)。
 *
 * @details
 * レイアウト: POSITION @0 / NORMAL @16 / TEXCOORD @32 / BLENDINDICES @40 / BLENDWEIGHT @44。
 * 最大 4 ボーン影響を持ち、未使用ボーンは index 0 / weight 0 で埋める。
 */
struct FSkinnedVertex {
    /** 頂点位置 (オブジェクト空間)。 */
    FVec3 position;

    /** 頂点法線。 */
    FVec3 normal;

    /** テクスチャ座標 U。 */
    f32  u;

    /** テクスチャ座標 V。 */
    f32  v;

    /** 影響ボーンの index (最大 4、未使用は 0)。 */
    u8   bones[4];

    /** 各影響ボーンの重み (合計 1.0、未使用は 0)。 */
    f32  weights[4];
};

/** 1 本のボーン (バインドポーズの TRS と逆バインド行列)。 */
struct FBone {
    /** ボーン名。 */
    FString name;

    /** 親ボーンの index (-1 ならルート)。 */
    i32    parent = -1;

    /** バインドポーズのローカル平行移動。 */
    FVec3   bind_translation = FVec3{0, 0, 0};

    /** バインドポーズのローカル回転 (既定は単位回転)。 */
    FQuat   bind_rotation    = FQuat{};

    /** バインドポーズのローカルスケール。 */
    FVec3   bind_scale       = FVec3{1, 1, 1};

    /**
     * バインドポーズワールド行列の逆行列。
     *
     * @details 全 bind_* 設定後に ComputeInverseBindMatrices() で計算される (既定は単位行列)。
     */
    FMat4   inverse_bind     = FMat4::Identity();
};

/** 1 つのアニメーションキー (時刻と TRS をまとめて保持する簡易版)。 */
struct FAnimationKey {
    /** キーの時刻 (秒)。 */
    f32  time = 0.0f;

    /** この時刻の平行移動。 */
    FVec3 translation = FVec3{0, 0, 0};

    /** この時刻の回転。 */
    FQuat rotation    = FQuat{};

    /** この時刻のスケール。 */
    FVec3 scale       = FVec3{1, 1, 1};
};

/** 1 本のボーンに対するアニメーションチャネル (時刻昇順のキー列)。 */
struct FAnimationChannel {
    /** 対象ボーンの index (-1 なら無効)。 */
    i32                 bone_index = -1;

    /** 時刻昇順のキー列。 */
    TArray<FAnimationKey> keys;
};

/** 1 つのアニメーションクリップ (名前・長さ・チャネル群)。 */
struct FAnimation {
    /** アニメーション名。 */
    FString                  name;

    /** アニメーションの長さ (秒)。 */
    f32                     duration = 0.0f;

    /** ボーンごとのアニメーションチャネル群。 */
    TArray<FAnimationChannel> channels;
};

/**
 * スキンメッシュアセット (スキニング頂点 + インデックス + ボーン階層 + アニメーション群)。
 *
 * @details MVP ではファイルからのパースは行わず、ランタイムでプログラム的にデータを構築して使う。
 */
class ASkinnedMeshAsset : public AAsset {
public:
    ACS_ASSET_TYPE("FSkinnedMeshAsset")

    /** 空のスキンメッシュアセットを構築する。 */
    ASkinnedMeshAsset() noexcept = default;

    /**
     * 頂点配列への可変参照を返す。
     *
     * @return スキニング頂点配列への可変参照。
     */
    TArray<FSkinnedVertex>& Vertices()     noexcept { return m_Vertices; }

    /**
     * インデックス配列への可変参照を返す。
     *
     * @return インデックス配列への可変参照。
     */
    TArray<u32>&           Indices()      noexcept { return m_Indices; }

    /**
     * ボーン配列への可変参照を返す。
     *
     * @return ボーン階層配列への可変参照。
     */
    TArray<FBone>&          Bones()        noexcept { return m_Bones; }

    /**
     * アニメーション配列への可変参照を返す。
     *
     * @return アニメーションクリップ配列への可変参照。
     */
    TArray<FAnimation>&     Animations()   noexcept { return m_Animations; }

    /**
     * 頂点配列への const 参照を返す。
     *
     * @return スキニング頂点配列への const 参照。
     */
    const TArray<FSkinnedVertex>& Vertices()   const noexcept { return m_Vertices; }

    /**
     * インデックス配列への const 参照を返す。
     *
     * @return インデックス配列への const 参照。
     */
    const TArray<u32>&           Indices()    const noexcept { return m_Indices; }

    /**
     * ボーン配列への const 参照を返す。
     *
     * @return ボーン階層配列への const 参照。
     */
    const TArray<FBone>&          Bones()      const noexcept { return m_Bones; }

    /**
     * アニメーション配列への const 参照を返す。
     *
     * @return アニメーションクリップ配列への const 参照。
     */
    const TArray<FAnimation>&     Animations() const noexcept { return m_Animations; }

    /**
     * 各ボーンの逆バインド行列を計算する (全 bind_* 設定後に 1 度呼ぶ)。
     *
     * @details 親→子の順 (親が小さい index) にバインドワールド行列を合成し、その逆行列を
     * FBone::inverse_bind に格納する。
     */
    void ComputeInverseBindMatrices() noexcept;

private:
    /** スキニング頂点配列。 */
    TArray<FSkinnedVertex> m_Vertices;

    /** インデックス配列。 */
    TArray<u32>           m_Indices;

    /** ボーン階層配列。 */
    TArray<FBone>          m_Bones;

    /** アニメーションクリップ配列。 */
    TArray<FAnimation>     m_Animations;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FSkinnedMeshAsset = ASkinnedMeshAsset;

/**
 * アニメーションを再生し、GPU 用ボーンパレットを計算するプレイヤ。
 *
 * @details
 * SetMesh でスキンメッシュを設定し Play でクリップを選択、Update で時刻を進め、
 * WritePalette で現在時刻の (world * inverse_bind) パレットを書き出す。
 */
class CAnimationPlayer {
public:
    /** 空のアニメーションプレイヤを構築する。 */
    CAnimationPlayer() noexcept = default;

    /**
     * 対象スキンメッシュを設定し再生状態をリセットする。
     *
     * @param mesh 参照するスキンメッシュ (所有はしない)。
     */
    void SetMesh(const ASkinnedMeshAsset* mesh) noexcept { m_Mesh = mesh; m_Anim = -1; m_Time = 0; }

    /**
     * 指定インデックスのアニメーションを先頭から再生する。
     *
     * @details mesh 未設定・範囲外 index では何もしない。
     * @param anim_index 再生するアニメーションの index。
     * @param loop true ならループ再生する (既定 true)。
     */
    void Play(u32 anim_index, bool loop = true) noexcept;

    /** 再生を一時停止する (時刻は保持)。 */
    void Pause() noexcept { m_Playing = false; }

    /** 一時停止した再生を再開する。 */
    void Resume() noexcept { m_Playing = true; }

    /** 再生を停止し時刻を 0 に戻す。 */
    void Stop() noexcept { m_Playing = false; m_Time = 0; }

    /**
     * 再生時刻を直接設定する。
     *
     * @param t 設定する時刻 (秒)。
     */
    void SetTime(f32 t) noexcept { m_Time = t; }

    /**
     * 現在の再生時刻を返す。
     *
     * @return 現在の時刻 (秒)。
     */
    f32  Time() const noexcept { return m_Time; }

    /**
     * 再生中かどうかを返す。
     *
     * @return 再生中なら true。
     */
    bool IsPlaying() const noexcept { return m_Playing; }

    /**
     * 再生時刻を dt だけ進める (ループ/終端処理を含む)。
     *
     * @details ループ時は duration で wrap し、非ループ時は終端で時刻をクランプして停止する。
     * @param dt 進める秒数。
     */
    void Update(f32 dt) noexcept;

    /**
     * 現在時刻のボーンパレットを書き込み、書き込んだボーン数を返す。
     *
     * @details
     * 各ボーンのアニメーション後ローカル TRS を求め、親から合成したワールド行列に
     * inverse_bind を掛けたものを out_palette へ書く。アニメ無し (m_Anim==-1) ならバインド姿勢を使う。
     * @param out_palette 最大 max_count 個の FMat4 を書き込む領域。
     * @param max_count 書き込める最大ボーン数。
     * @return 実際に書き込んだボーン数。
     */
    u32 WritePalette(FMat4* out_palette, u32 max_count) const noexcept;

private:
    /** 参照中のスキンメッシュ (所有しない)。 */
    const ASkinnedMeshAsset* m_Mesh    = nullptr;

    /** 再生中アニメーションの index (-1 なら T ポーズ)。 */
    i32                     m_Anim    = -1;

    /** 現在の再生時刻 (秒)。 */
    f32                     m_Time    = 0.0f;

    /** ループ再生フラグ。 */
    bool                    m_bLoop    = true;

    /** 再生中フラグ。 */
    bool                    m_Playing = false;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FAnimationPlayer = CAnimationPlayer;

} // namespace acs
