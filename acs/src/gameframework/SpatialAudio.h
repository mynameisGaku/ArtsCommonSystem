// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "math/Vec.h"
#include "container/Array.h"

namespace acs::game {

/**
 * プレイヤの耳位置と向き (pan / attenuation の基準)。
 *
 * @details 既定値は原点 + 標準 Z+ forward / Y+ up (DirectXMath 慣習)。
 */
struct FAudioListener {
    /** 世界座標での位置 (pan / attenuation の基準点)。 */
    FVec3 position = FVec3::Zero();

    /** 正規化済み前方ベクトル (pan の右ベクトル算出に使う)。 */
    FVec3 forward  = FVec3::Forward();

    /** 正規化済み上方ベクトル (forward × up = right)。 */
    FVec3 up       = FVec3::Up();
};

/**
 * 1 個の 3D 音源。
 */
struct FAudioSource3D {
    /** CSpatialAudio が払い出す一意 ID (0 = 無効、1.. = 有効)。 */
    u32                source_id    = 0;

    /** 世界座標での位置。 */
    FVec3               position     = FVec3::Zero();

    /** Doppler 計算用の速度。距離減衰と pan 計算では参照しない。 */
    FVec3               velocity     = FVec3::Zero();

    /** 基準ゲイン [0, 1] (距離減衰前の素の音量)。 */
    f32                volume       = 1.0f;

    /** culling 距離 (これより遠いと vol=0)。 */
    f32                max_distance = 20.0f;

    /** false なら attenuation / pan 計算をスキップ。 */
    bool               active       = true;

    /** attenuation curve 種別 (u8 詰めのため末尾に配置、既定 Linear)。 */
    u8                 curve        = 0; // = EAttenuationCurve::Linear
};

/**
 * 距離 → 音量の減衰カーブ種別。
 */
enum class EAttenuationCurve : u8 {
    /** 線形減衰。シューティングの被弾音などゲーム的にわかりやすい。 */
    Linear      = 0,

    /** 1/r ベースの自然な減衰 (物理に近い)。 */
    Inverse     = 1,

    /** 急峻に消える減衰。足音や草擦れ音向け。 */
    Exponential = 2,
};

/**
 * HRTF 畳み込みの抽象 interface (seam)。
 *
 * @details
 * stub は単純 stereo panning のみで、KEMAR 256-tap convolution を別モジュールに
 * 実装し本 interface 越しに差し替える。ライフタイムは Init() → SetListener /
 * ProcessSource* → Shutdown() の順。Init() 未呼出での ProcessSource や Shutdown 後の
 * 再 Init は実装定義 (stub は再利用可)。1 instance はオーディオ専用スレッド
 * (典型: WASAPI render thread) からのみ叩く想定。
 */
class IHrtfRenderer {
public:
    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~IHrtfRenderer() noexcept = default;

    /**
     * HRTF レイヤを初期化する。
     *
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> Init() noexcept = 0;

    /** 確保したリソースを解放する。 */
    virtual void Shutdown() noexcept = 0;

    /**
     * HRTF が有効か (= 実 convolution が走るか) を返す。
     *
     * @return 実 HRTF 効果があれば true (stub は false)。
     */
    virtual bool IsHrtfEnabled() const noexcept = 0;

    /**
     * listener 状態を HRTF レイヤに伝える。
     *
     * @details head orientation 角度を KEMAR IR インデックスに変換する材料となる。
     * @param listener 現在の listener 状態。
     */
    virtual void SetListener(const FAudioListener& listener) noexcept = 0;

    /**
     * mono 入力を interleaved stereo (LRLR...) に変換して書き込む。
     *
     * @details
     * source.active == false の場合の動作は実装定義 (stub は 0 埋め)。volume / pan の
     * 事前計算は本層で済ませても実装内で計算してもよい。
     * @param source 処理対象の 3D 音源。
     * @param mono_input mono 入力サンプル (sample_count 要素)。
     * @param stereo_output 出力先 (interleaved、sample_count * 2 要素書き込む)。
     * @param sample_count 入力サンプル数。
     */
    virtual void ProcessSource(const FAudioSource3D& source,
                               const f32* mono_input,
                               f32* stereo_output,
                               u32 sample_count) noexcept = 0;

protected:
    /** 派生クラスからのみ構築可能にする既定コンストラクタ。 */
    IHrtfRenderer() noexcept = default;

    /** コピー禁止 (interface seam のため)。 */
    IHrtfRenderer(const IHrtfRenderer&)            = delete;

    /** コピー代入も禁止。 */
    IHrtfRenderer& operator=(const IHrtfRenderer&) = delete;

    /** ムーブ禁止。 */
    IHrtfRenderer(IHrtfRenderer&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IHrtfRenderer& operator=(IHrtfRenderer&&)      = delete;
};

/**
 * constant-power stereo panning + 距離減衰を実数学で行う IHrtfRenderer 実装。
 *
 * @details
 * 真のバイノーラル化 (HRTF convolution) のみ stub で、stereo 空間化は本物:
 * pan = 右ベクトル投影 (dot(dir, right))、L = mono·cos((pan+1)·π/4)·volume·atten、
 * R = mono·sin((pan+1)·π/4)·volume·atten (constant-power パン則)、atten は
 * source.curve に応じた距離減衰 (ComputeAttenuatedVolume と同式)。この型が唯一の
 * IHrtfRenderer 実装で、IsHrtfEnabled() は false を返す (真の HRTF 効果のみ無い)。
 */
class CHrtfRendererStub final : public IHrtfRenderer {
public:
    /** 空状態で構築する。 */
    CHrtfRendererStub() noexcept = default;

    /** 破棄する。 */
    ~CHrtfRendererStub() noexcept override = default;

    /**
     * stub を初期化する (初回のみ HRTF off のログを出す)。
     *
     * @return 常に Ok。
     */
    TResult<void> Init() noexcept override;

    /** 初期化フラグを下ろす。 */
    void         Shutdown() noexcept override;

    /**
     * HRTF が有効かを返す。
     *
     * @return 常に false (panning だけで真の HRTF 効果は無い)。
     */
    bool         IsHrtfEnabled() const noexcept override { return false; }

    /**
     * listener 状態を保持する (pan 算出に使う)。
     *
     * @param listener 現在の listener 状態。
     */
    void         SetListener(const FAudioListener& listener) noexcept override;

    /**
     * mono 入力に constant-power パン + 距離減衰を掛けて stereo 出力する。
     *
     * @param source 処理対象の 3D 音源 (active==false なら 0 埋め)。
     * @param mono_input mono 入力サンプル (sample_count 要素)。
     * @param stereo_output 出力先 (interleaved、sample_count * 2 要素書き込む)。
     * @param sample_count 入力サンプル数。
     */
    void         ProcessSource(const FAudioSource3D& source,
                               const f32* mono_input,
                               f32* stereo_output,
                               u32 sample_count) noexcept override;

private:
    /** 保持中の listener 状態 (SetListener で更新)。 */
    FAudioListener m_Listener {};

    /** 初期化済みフラグ。 */
    bool          m_Initialized = false;
};

/**
 * 3D listener + source を集中管理する空間化レイヤ。
 *
 * @details
 * AScene 局所 instance としての所有を想定。1 listener + N source を保持し、
 * 毎フレーム attenuation と pan を pull で取得できる API を提供する。
 */
class CSpatialAudio {
public:
    /** source 配列の初期容量 (Reserve のヒント)。 */
    static constexpr u32 kInitialSourceCapacity = 16;

    /** source 配列を初期容量で Reserve して構築する。 */
    CSpatialAudio() noexcept;

    /** 破棄する。 */
    ~CSpatialAudio() noexcept = default;

    /** コピー禁止 (シーン局所 instance として単独所有するため)。 */
    CSpatialAudio(const CSpatialAudio&)            = delete;

    /** コピー代入も禁止。 */
    CSpatialAudio& operator=(const CSpatialAudio&) = delete;

    /** ムーブ禁止。 */
    CSpatialAudio(CSpatialAudio&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CSpatialAudio& operator=(CSpatialAudio&&)      = delete;

    /**
     * listener (耳位置と向き) を設定する。
     *
     * @param l 新しい listener 状態。
     */
    void                 SetListener(const FAudioListener& l) noexcept;

    /**
     * 現在の listener への const 参照を返す。
     *
     * @return 保持中の listener。
     */
    const FAudioListener& GetListener() const noexcept { return m_Listener; }

    /**
     * 新規 source を登録する。
     *
     * @param pos source の初期世界座標。
     * @param max_distance culling 距離 (<= 0 は既定 20m にクランプ)。
     * @param curve 距離減衰カーブ種別。
     * @return 払い出した source_id (1.. の単調増加)。
     */
    u32  RegisterSource(FVec3 pos, f32 max_distance,
                        EAttenuationCurve curve) noexcept;

    /**
     * source の位置 / 速度を更新する。
     *
     * @details stale ID は no-op (警告ログのみ)。
     * @param id 更新対象の source_id。
     * @param pos 新しい世界座標。
     * @param vel 新しい速度 (既定はゼロ)。
     */
    void UpdateSource(u32 id, FVec3 pos, FVec3 vel = FVec3::Zero()) noexcept;

    /**
     * source の基準ゲインを変更する。
     *
     * @details 範囲外は [0, 1] に clamp し警告ログを出す。stale ID は no-op。
     * @param id 対象の source_id。
     * @param v 新しい基準ゲイン [0, 1]。
     */
    void SetSourceVolume(u32 id, f32 v) noexcept;

    /**
     * source を削除する。
     *
     * @details 末尾と swap して除去する (内部 slot は使い回さない)。stale ID は静かに無視。
     * @param id 削除対象の source_id。
     */
    void RemoveSource(u32 id) noexcept;

    /**
     * listener との距離と curve から算出した最終 volume を返す。
     *
     * @param id 対象の source_id。
     * @return 最終 volume [0, 1]。無効 ID / inactive / dist >= max_distance では 0。
     */
    f32  ComputeAttenuatedVolume(u32 id) const noexcept;

    /**
     * listener 基準の左右パンを返す。
     *
     * @param id 対象の source_id。
     * @return パン値 (-1 = 完全左、0 = 正面 / 真後ろ、+1 = 完全右)。無効 ID / inactive で 0。
     */
    f32  ComputePan(u32 id) const noexcept;

    /**
     * active な source の数を返す。
     *
     * @return active==true の source 数。
     */
    u32  SourceCount() const noexcept;

    /**
     * state を 1 フレーム進める。
     *
     * @details dt は更新契約として受け取るが、内部 state を変更しない no-op。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

    /** 全 source を空にする (listener は保持、source_id カウンタは継続)。 */
    void Clear() noexcept;

private:
    /**
     * source_id を index に線形検索で変換する。
     *
     * @param id 検索する source_id (0 は無効予約)。
     * @return 見つかった index、見つからなければ m_Sources.Size()。
     */
    usize FindIndex(u32 id) const noexcept;

    /** 保持中の listener 状態。 */
    FAudioListener     m_Listener {};

    /** 登録済み 3D 音源の配列。 */
    TArray<FAudioSource3D> m_Sources;

    /** 次に払い出す source_id (0 = 無効予約)。 */
    u32               m_NextSourceId = 1;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FHrtfRendererStub = CHrtfRendererStub;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FSpatialAudio = CSpatialAudio;

} // namespace acs::game
