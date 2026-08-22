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
#include "foundation/Result.h"
#include "foundation/Error.h"
#include "memory/SharedPtr.h"

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

/**
 * 1 秒あたり何回サンプリングしてアニメーションを取り込むか。
 *
 * @details
 * FBX の曲線をそのまま持たず、**一定間隔で焼いて `FAnimationKey` の列にする**。曲線の
 * 種類 (ベジェ・TCB・オイラー角の巻き方) を全部解釈するより確実で、`CAnimationPlayer` の
 * 補間 (線形 + slerp) にそのまま合う。
 *
 * 上げるとキーが増えて滑らかになり、記憶も増える。30 は «見て分からない» 下限のあたり。
 */
inline constexpr f32 kSkinnedFbxDefaultSampleRate = 30.0f;

/**
 * FBX のバイト列からスキン付きメッシュを読む。
 *
 * @details
 * **静的メッシュの `CFbxAssetLoader` とは別口。** 同じ `.fbx` でも、骨の要る読み方と
 * 要らない読み方で欲しいものが違うため、拡張子で自動に選ばせず呼び分ける。
 *
 * 取り込むもの:
 * - 最初に見つかった «スキンの付いた» メッシュ 1 つ (複数は取らない)
 * - その skin が指すボーンと、**そこから根までの祖先ノード全部**
 *   (祖先を落とすと、腰から上だけが親の回転を失って崩れる)
 * - 逆バインド行列は ufbx の `geometry_to_bone` をそのまま使う
 *   (バインド姿勢を local から組み直すより正確)
 * - 全アニメーションを `sample_rate` で焼く
 *
 * 1 頂点あたりの影響は**重みの大きい順に 4 本**まで。ufbx が既に降順で並べているので
 * 先頭から取り、合計が 1 になるように正規化する。
 *
 * @param data FBX のバイト列。
 * @param size バイト数。
 * @param sample_rate アニメーションを焼く間隔 (回 / 秒)。0 以下なら既定。
 * @return 読めたスキンメッシュ。スキンが 1 つも無ければエラー。
 */
TResult<TSharedPtr<ASkinnedMeshAsset>> LoadSkinnedMeshFromFbxMemory(
    const byte* data, usize size,
    f32 sample_rate = kSkinnedFbxDefaultSampleRate) noexcept;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FSkinnedMeshAsset = ASkinnedMeshAsset;

/**
 * アニメーションを再生し、GPU 用ボーンパレットを計算するプレイヤ。
 *
 * @details
 * SetMesh でスキンメッシュを設定し Play または BlendTo でクリップを選択、Update で
 * 時刻を進め、WritePalette で現在時刻の (world * inverse_bind) パレットを書き出す。
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
    void SetMesh(const ASkinnedMeshAsset* mesh) noexcept;

    /**
     * 指定インデックスのアニメーションを先頭から再生する。
     *
     * @details mesh 未設定・範囲外 index では何もしない。
     * @param anim_index 再生するアニメーションの index。
     * @param loop true ならループ再生する (既定 true)。
     */
    void Play(u32 anim_index, bool loop = true) noexcept;

    /**
     * 現在姿勢から指定アニメーションの先頭へ滑らかに切り替える。
     *
     * @details 遷移中は切替元と切替先の時刻をともに進め、各ボーンのローカルTRSを
     * 平行移動・スケールは線形、回転はslerpで混ぜてから親子階層を合成する。
     * blend_seconds == 0 は Play と同じ即時切替になる。mesh未設定、範囲外index、
     * 非有限または負の期間、非有限な現在時刻では何も変更せずfalseを返す。
     * 進行中の遷移へ新しい遷移を重ねる要求も、現在の姿勢と再生状態を保ってfalseを返す。
     * 呼び出し側は現在の遷移が完了した後に再試行できる。
     * @param animation_index 切替先アニメーションのindex。
     * @param blend_seconds 姿勢を混ぜる有限かつ0以上の秒数。
     * @param loop 切替先を繰り返すならtrue。
     * @return 切替要求を受理したらtrue。
     */
    bool BlendTo(u32 animation_index, f32 blend_seconds, bool loop = true) noexcept;

    /** 再生を一時停止する (時刻は保持)。 */
    void Pause() noexcept { m_Playing = false; }

    /** 一時停止した再生を再開する。 */
    void Resume() noexcept { m_Playing = true; }

    /** 再生と姿勢遷移を停止し、切替先の時刻を0へ戻す。 */
    void Stop() noexcept;

    /**
     * 再生時刻を直接設定する。
     *
     * @details 有限値だけを受理する。姿勢遷移中は遷移を解除して切替先だけを残す。
     * NaNと正負の無限大は現在時刻と姿勢遷移を変更しない。
     * @param t 設定する時刻 (秒)。
     */
    void SetTime(f32 t) noexcept;

    /**
     * 現在の再生時刻を返す。
     *
     * @return 現在の時刻 (秒)。
     */
    f32  Time() const noexcept;

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
     * 各ボーンのアニメーション後ローカルTRSを求め、姿勢遷移中は階層合成より前に
     * 切替元と切替先を混ぜる。親から合成したワールド行列にinverse_bindを掛けたものを
     * out_paletteへ書く。アニメ無し (m_Anim==-1) ならバインド姿勢を使う。
     * @param out_palette 最大 max_count 個の FMat4 を書き込む領域。
     * @param max_count 書き込める最大ボーン数。
     * @return 実際に書き込んだボーン数。
     */
    u32 WritePalette(FMat4* out_palette, u32 max_count) const noexcept;

private:
    /** 姿勢遷移中ならtrueを返す。 */
    bool IsBlending_Internal() const noexcept { return m_BlendDuration > 0.0f; }

    /** 姿勢遷移の保存値を初期状態へ戻す。 */
    void ClearBlend_Internal() noexcept;

    /** packed値から切替元アニメーションのindexを返す。 */
    u32 BlendSourceIndex_Internal() const noexcept;

    /** packed値から切替元のloop指定を返す。 */
    bool BlendSourceLoops_Internal() const noexcept { return m_BlendFromAnimation < 0; }

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

    /** 切替元index。負値はloop指定を兼ね、-index-1で格納する。 */
    i32                     m_BlendFromAnimation = 0;

    /** 姿勢遷移を開始した時点の切替元clip時刻。 */
    f32                     m_BlendFromTime = 0.0f;

    /** 姿勢を混ぜる総秒数。0なら遷移なし。 */
    f32                     m_BlendDuration = 0.0f;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FAnimationPlayer = CAnimationPlayer;

} // namespace acs
