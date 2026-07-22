// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — FAnimationCurve
//
// 編集可能な「時間→値」補間曲線。Unity AnimationCurve / Unreal FRichCurve に相当。
// FSpriteAnimator が「frame index 計算」だけを担うのと違い、FAnimationCurve は
// 任意の f32 を時間で滑らかに変化させる汎用パスを提供する。
//
// 用途:
//   ・カメラ FOV / 露出のキー打ち
//   ・カスタム easing (Easing 関数群で表現できない自由曲線)
//   ・体力バー演出やフェード等のスクリプト連動 (FSequence に渡す値生成器として)
//
// 使い方:
//   FAnimationCurve fade;
//   fade.AddKey(0.0f, 0.0f, ECurveInterpolation::Linear);
//   fade.AddKey(0.5f, 0.8f, ECurveInterpolation::Linear);
//   fade.AddKey(1.0f, 1.0f, ECurveInterpolation::Hermite); // 出口を滑らか
//   fade.SetPostWrap(FAnimationCurve::EWrapMode::Clamp);
//   // 毎フレーム:
//   f32 alpha = fade.Evaluate(t);
//
// 設計判断:
//   ・Easing.h は標準 easing の型付きカタログとして十分軽量だが、デザイナが任意の曲線を
//     差し込みたい局面 (ボス演出曲線, カスタム UI スライドカーブ) に対応するため
//     キー打ち式の曲線を別途用意する。
//   ・key は time 昇順で内部 TArray に保持。AddKey は二分探索で適切位置に挿入する
//     ことで Evaluate を O(log N) に保つ (FSequence の sorted insert と同方針)。
//   ・各 key は in_interp / out_interp を持ち、segment [k_i, k_{i+1}] の補間方式は
//     k_i.out_interp で決定。Step → 左 value 保持。Linear → 線形。Hermite →
//     k_i.out_tangent と k_{i+1}.in_tangent を使う 3 次 Hermite。
//   ・Hermite のタンジェントは「単位 1.0 秒あたりの傾き」として保存。Evaluate
//     時に segment 長 dt で乗算してスケールするので、key 間隔を変えても曲線形が
//     直感的に保てる (Unity の Tangent と同セマンティクス)。
//   ・EWrapMode = {Clamp, Loop, PingPong} の前後別指定。Loop / PingPong は
//     FSpriteAnimator と同じ折り返し方式で実装し、長時間呼び出しでも f32 精度を
//     失わないよう time → 内部正規化 time の段階で fold する。
//   ・FSpriteAnimator と同様に「ランタイム状態を不意に複製しないため非コピー・
//     非ムーブ」。曲線を共有したい場合は FAnimationCurve を 1 つ作って参照渡しする。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

namespace Easing {
enum class EEasingType : u8;
}

/**
 * 各 key で in 側 / out 側それぞれに指定する補間方式。
 *
 * @details
 * segment [k_i, k_{i+1}] の補間は k_i.out_interp が支配する。
 * k_{i+1}.in_interp は Hermite tangent 自動計算等で参照される予約フィールド。
 */
enum class ECurveInterpolation : u8 {
    /** 直前 key の value をそのまま使う (階段)。 */
    Step    = 0,

    /** 線形補間。 */
    Linear  = 1,

    /** 3 次 Hermite (in_tangent / out_tangent を使う)。 */
    Hermite = 2,
};

/**
 * 曲線上の 1 つのキー (時刻・値・前後タンジェント・前後補間方式)。
 */
struct FCurveKey {
    /** キーの時刻 (秒)。 */
    f32                time        = 0.0f;

    /** キーの値。 */
    f32                value       = 0.0f;

    /** この key に「入ってくる側」の傾き (単位 1 秒あたり)。 */
    f32                in_tangent  = 0.0f;

    /** この key から「出ていく側」の傾き (単位 1 秒あたり)。 */
    f32                out_tangent = 0.0f;

    /** この key に入る側の補間方式 (予約フィールド)。 */
    ECurveInterpolation in_interp   = ECurveInterpolation::Linear;

    /** この key から右の segment を支配する補間方式。 */
    ECurveInterpolation out_interp  = ECurveInterpolation::Linear;
};

/** FAnimationCurve の checked 更新・評価 API が返す安定したエラー分類。 */
enum class EAnimationCurveError : u8 {
    None = 0,
    NullKeys,
    TooManyKeys,
    NonFiniteValue,
    InvalidInterpolation,
    InvalidEasingType,
    InvalidSampleCount,
    InvalidWrapMode,
    UnsortedKeys,
    DuplicateKeyTime,
    AllocationFailure,
    ResultOutOfRange,
};

/** checked 更新・評価の結果。 */
struct FAnimationCurveResult {
    EAnimationCurveError error = EAnimationCurveError::None;
    u32 key_index = 0u;
    u32 key_count = 0u;

    bool Succeeded() const noexcept {
        return error == EAnimationCurveError::None;
    }

    static const char* ErrorName(EAnimationCurveError error) noexcept;
};

/**
 * 編集可能な「時間→値」補間曲線 (Unity AnimationCurve / Unreal FRichCurve 相当)。
 *
 * @details
 * key を time 昇順で内部 TArray に保持し、AddKey は二分探索で適切位置に挿入するため
 * Evaluate は O(log N)。segment [k_i, k_{i+1}] の補間方式は k_i.out_interp で決まり
 * (Step / Linear / Hermite)、Hermite は単位 1 秒あたりの傾きを segment 長で乗算して
 * 評価する。EWrapMode で定義域外の前後折り返しを別々に指定でき、ランタイム状態を
 * 不意に複製しないよう非コピー・非ムーブ。
 */
class FAnimationCurve {
public:
    /**
     * 時間が定義域外に出たときの折り返し方式 (前後別指定可能)。
     */
    enum class EWrapMode : u8 {
        /** 端の value を保持。 */
        Clamp    = 0,

        /** duration ごとに繰り返し。 */
        Loop     = 1,

        /** 端で折り返し。 */
        PingPong = 2,
    };

    /** 空の曲線を構築する (key なし、前後 EWrapMode は Clamp)。 */
    FAnimationCurve() noexcept = default;

    /** allocator を明示して空の曲線を構築する。失敗注入と専用 arena に利用できる。 */
    explicit FAnimationCurve(FAllocator& allocator) noexcept
        : m_Keys(allocator) {}

    /** 破棄する (内部 TArray が key を解放)。 */
    ~FAnimationCurve() noexcept = default;

    /** コピー禁止 (ランタイム状態を不意に複製しないため)。 */
    FAnimationCurve(const FAnimationCurve&)            = delete;

    /** コピー代入も禁止。 */
    FAnimationCurve& operator=(const FAnimationCurve&) = delete;

    /** ムーブ禁止 (FSpriteAnimator と同方針)。 */
    FAnimationCurve(FAnimationCurve&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FAnimationCurve& operator=(FAnimationCurve&&)      = delete;

    /**
     * key を追加する (time 昇順を保つよう挿入)。
     *
     * @details
     * 既存に同 time の key があれば value / out_interp を上書きして重複挿入を回避する。
     * それ以外は二分探索で位置を決めて挿入し、in_interp は新規挿入時のみ interp と
     * 同値で初期化される。
     * @param time 追加するキーの時刻 (秒)。
     * @param value 追加するキーの値。
     * @param interp この key から右 segment の補間方式 (既定 Linear)。
     */
    void AddKey(f32 time, f32 value,
                ECurveInterpolation interp = ECurveInterpolation::Linear) noexcept;

    /**
     * key を検証して追加または同一 time の key を更新する checked API。
     * 非有限値、不正 enum、上限超過、OOM では既存状態を変更しない。
     */
    FAnimationCurveResult TryAddKey(
        f32 time, f32 value,
        ECurveInterpolation interp = ECurveInterpolation::Linear) noexcept;

    /**
     * Hermite モード専用の便利 add (前後 interp を Hermite に設定)。
     *
     * @details
     * タンジェントは「単位 1.0 秒あたりの傾き」で渡す。既存 key を上書きする場合は
     * in/out 両 tangent を更新し、両 interp を Hermite にする。
     * @param time 追加するキーの時刻 (秒)。
     * @param value 追加するキーの値。
     * @param in_tangent 入る側の傾き (単位 1 秒あたり)。
     * @param out_tangent 出ていく側の傾き (単位 1 秒あたり)。
     */
    void AddKeyHermite(f32 time, f32 value,
                       f32 in_tangent, f32 out_tangent) noexcept;

    /** Hermite key の checked 追加 API。 */
    FAnimationCurveResult TryAddKeyHermite(
        f32 time, f32 value,
        f32 in_tangent, f32 out_tangent) noexcept;

    /**
     * sort 済み key 列と wrap mode を全検証し、成功時だけ一括置換する。
     * count==0 では keys==nullptr を許可する。
     */
    FAnimationCurveResult TrySetKeys(
        const FCurveKey* keys, u32 count,
        EWrapMode pre_wrap = EWrapMode::Clamp,
        EWrapMode post_wrap = EWrapMode::Clamp) noexcept;

    /**
     * 型付き easing を [0,1] の編集可能な線形 key 列へサンプリングする。
     *
     * @details Back / Elastic の overshoot も保持する。入力検証、全サンプル評価、
     * key 用メモリ確保がすべて成功した場合だけ現在の曲線を置換するため、無効 type、
     * 不正 sample_count、OOM では既存 key と wrap mode を変更しない。
     */
    FAnimationCurveResult TrySetEasingPreset(
        Easing::EEasingType type, u32 sample_count = 65u) noexcept;

    /**
     * 指定 index の key を順序を保ったまま除去する。
     *
     * @details index 範囲外は no-op (debug ビルドでも crash させない方針)。
     * @param index 除去するキーのインデックス。
     */
    void RemoveKey(u32 index) noexcept;

    /** 全ての key を除去して空にする。 */
    void ClearKeys() noexcept;

    /**
     * 保持している key の数を返す。
     *
     * @return key の個数。
     */
    u32             KeyCount() const noexcept { return static_cast<u32>(m_Keys.Size()); }

    /**
     * 指定 index の key への const ポインタを返す。
     *
     * @param index 取得するキーのインデックス。
     * @return key への const ポインタ (範囲外なら nullptr)。
     */
    const FCurveKey* Key(u32 index) const noexcept {
        return index < m_Keys.Size() ? &m_Keys[index] : nullptr;
    }

    /**
     * 時間 → 値の補間評価を行う。
     *
     * @details
     * key が 0 個なら 0、1 個ならその value を返す。time が [0, Duration()] を外れる
     * 場合は SetPreWrap / SetPostWrap に従って折り返す。
     * @param time 評価する時刻 (秒)。
     * @return 補間された値。
     */
    f32 Evaluate(f32 time) const noexcept;

    /**
     * 非有限入力と補間 overflow を診断し、成功時だけ out_value を更新する。
     */
    FAnimationCurveResult TryEvaluate(
        f32 time, f32& out_value) const noexcept;

    /**
     * 末尾 key の time (絶対値) を返す。
     *
     * @details
     * 「曲線の長さ」ではなく「最後のキーの time」を返す仕様 (Unity の
     * FAnimationCurve.keys[last].time に相当)。最初の key の time が 0 でない場合は
     * 「Duration() - 第 1 key の time」が真の長さとなる。key 0 個では 0。
     * @return 末尾 key の time。
     */
    f32 Duration() const noexcept;

    /**
     * 定義域より前 (time < 第 1 key の time) の折り返し方式を設定する。
     *
     * @details 典型ケースで第 1 key.time == 0 なら time < 0 の挙動 = Unity の preWrapMode と等価。
     * @param m 適用する EWrapMode。
     */
    void SetPreWrap(EWrapMode mode) noexcept;

    /**
     * 定義域より後 (time > 末尾 key の time = Duration()) の折り返し方式を設定する (= Unity の postWrapMode)。
     *
     * @param m 適用する EWrapMode。
     */
    void SetPostWrap(EWrapMode mode) noexcept;

    /**
     * 前後の wrap mode を検証し、成功時だけ同時に更新する。
     */
    FAnimationCurveResult TrySetWrapModes(
        EWrapMode pre_wrap, EWrapMode post_wrap) noexcept;

    /**
     * 前側 (pre) の折り返し方式を返す。
     *
     * @return 設定済みの pre EWrapMode。
     */
    EWrapMode PreWrap () const noexcept { return m_PreWrap;  }

    /**
     * 後側 (post) の折り返し方式を返す。
     *
     * @return 設定済みの post EWrapMode。
     */
    EWrapMode PostWrap() const noexcept { return m_PostWrap; }

    /** 1 curve が保持できる key 数の上限。 */
    static constexpr u32 kMaxKeys = 65536u;

    /** easing preset の最大サンプル数。UI操作や入力データによる過剰確保を防ぐ。 */
    static constexpr u32 kMaxEasingPresetSamples = 4096u;

private:
    /**
     * 時間を [0, Duration()] にラップする (EWrapMode 適用)。
     *
     * @details
     * 空 / Duration==0 の場合はそのまま time を返し (上位で個別処理する)、
     * それ以外で正規化された時間を返す。
     * @param time ラップ前の時刻 (秒)。
     * @return 折り返し適用後の時刻。
     */
    f32 ApplyWrap(f32 time) const noexcept;

    /**
     * time を含む segment の左端 key の index を二分探索で求める。
     *
     * @details
     * key[i].time <= time <= key[i+1].time となる i を返す。time が末尾以降や先頭未満なら
     * 端の index にクランプする (呼び出し側はラップ済み time を渡す前提)。
     * @param time 探索する時刻 (秒)。
     * @return segment 左端 key の index。
     */
    u32 FindSegmentIndex(f32 time) const noexcept;

    /**
     * segment [k0, k1] を k0.out_interp に従って補間する。
     *
     * @details Hermite ではタンジェントを dt で乗算してこの segment 内の傾きにスケールする。
     * @param k0 segment 左端の key。
     * @param k1 segment 右端の key。
     * @param t segment 内の正規化進度 [0,1]。
     * @param dt segment の長さ (秒)。
     * @return 補間された値。
     */
    static f32 InterpolateSegment(const FCurveKey& k0, const FCurveKey& k1,
                                  f32 t, f32 dt) noexcept;

    /** time 昇順に保持された key 列。 */
    TArray<FCurveKey> m_Keys;

    /** 定義域より前の折り返し方式。 */
    EWrapMode        m_PreWrap  = EWrapMode::Clamp;

    /** 定義域より後の折り返し方式。 */
    EWrapMode        m_PostWrap = EWrapMode::Clamp;
};

} // namespace acs::game
