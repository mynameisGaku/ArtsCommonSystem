// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — AnimationCurve (Phase 3)
//
// 編集可能な「時間→値」補間曲線。Unity AnimationCurve / Unreal FRichCurve に相当。
// SpriteAnimator が「frame index 計算」だけを担うのと違い、AnimationCurve は
// 任意の f32 を時間で滑らかに変化させる汎用パスを提供する。
//
// 用途:
//   ・カメラ FOV / 露出のキー打ち
//   ・カスタム easing (Easing 関数群で表現できない自由曲線)
//   ・体力バー演出やフェード等のスクリプト連動 (Sequence に渡す値生成器として)
//
// 使い方:
//   AnimationCurve fade;
//   fade.AddKey(0.0f, 0.0f, ECurveInterpolation::Linear);
//   fade.AddKey(0.5f, 0.8f, ECurveInterpolation::Linear);
//   fade.AddKey(1.0f, 1.0f, ECurveInterpolation::Hermite); // 出口を滑らか
//   fade.SetPostWrap(AnimationCurve::WrapMode::Clamp);
//   // 毎フレーム:
//   f32 alpha = fade.Evaluate(t);
//
// 設計判断:
//   ・Easing.h は固定数式 11 種類で十分軽量だが、デザイナが任意の曲線を
//     差し込みたい局面 (ボス演出曲線, カスタム UI スライドカーブ) に対応するため
//     キー打ち式の曲線を別途用意する。
//   ・key は time 昇順で内部 TArray に保持。AddKey は二分探索で適切位置に挿入する
//     ことで Evaluate を O(log N) に保つ (Sequence の sorted insert と同方針)。
//   ・各 key は in_interp / out_interp を持ち、segment [k_i, k_{i+1}] の補間方式は
//     k_i.out_interp で決定。Step → 左 value 保持。Linear → 線形。Hermite →
//     k_i.out_tangent と k_{i+1}.in_tangent を使う 3 次 Hermite。
//   ・Hermite のタンジェントは「単位 1.0 秒あたりの傾き」として保存。Evaluate
//     時に segment 長 dt で乗算してスケールするので、key 間隔を変えても曲線形が
//     直感的に保てる (Unity の Tangent と同セマンティクス)。
//   ・WrapMode = {Clamp, Loop, PingPong} の前後別指定。Loop / PingPong は
//     SpriteAnimator と同じ折り返し方式で実装し、長時間呼び出しでも f32 精度を
//     失わないよう time → 内部正規化 time の段階で fold する。
//   ・SpriteAnimator と同様に「ランタイム状態を不意に複製しないため非コピー・
//     非ムーブ」。曲線を共有したい場合は AnimationCurve を 1 つ作って参照渡しする。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// 各 key で in 側 / out 側それぞれに指定する補間方式。
// segment [k_i, k_{i+1}] の補間は k_i.out_interp が支配する。
// k_{i+1}.in_interp は (将来) Hermite tangent 自動計算等で参照される予約フィールド。
enum class ECurveInterpolation : u8 {
    Step    = 0,  // 直前 key の value をそのまま使う (階段)
    Linear  = 1,  // 線形補間
    Hermite = 2,  // 3 次 Hermite (in_tangent / out_tangent を使う)
};

struct CurveKey {
    f32                time        = 0.0f;
    f32                value       = 0.0f;
    f32                in_tangent  = 0.0f;  // この key に「入ってくる側」の傾き
    f32                out_tangent = 0.0f;  // この key から「出ていく側」の傾き
    ECurveInterpolation in_interp   = ECurveInterpolation::Linear;
    ECurveInterpolation out_interp  = ECurveInterpolation::Linear;
};

class AnimationCurve {
public:
    // 時間が定義域外に出たときの折り返し方式 (前後別指定可能)。
    enum class WrapMode : u8 {
        Clamp    = 0,  // 端の value を保持
        Loop     = 1,  // duration ごとに繰り返し
        PingPong = 2,  // 端で折り返し
    };

    AnimationCurve() noexcept = default;
    ~AnimationCurve() noexcept = default;

    // ランタイム状態を不意に複製しないため非コピー・非ムーブ (SpriteAnimator と同方針)
    AnimationCurve(const AnimationCurve&)            = delete;
    AnimationCurve& operator=(const AnimationCurve&) = delete;
    AnimationCurve(AnimationCurve&&)                 = delete;
    AnimationCurve& operator=(AnimationCurve&&)      = delete;

    // 既存に同 time の key があれば value / out_interp を上書き (= 重複挿入を回避)。
    // それ以外は time 昇順を保つよう binary search で位置を決めて挿入する。
    // in_interp は新規挿入時のみ interp と同値で初期化。
    void AddKey(f32 time, f32 value,
                ECurveInterpolation interp = ECurveInterpolation::Linear) noexcept;

    // Hermite モード専用の便利 add。タンジェントは「単位 1.0 秒あたりの傾き」で渡す。
    // 既存 key を上書きする場合は in/out 両 tangent を更新し、両 interp を Hermite に。
    void AddKeyHermite(f32 time, f32 value,
                       f32 in_tangent, f32 out_tangent) noexcept;

    // index 範囲外は no-op (debug ビルドでも crash させない方針 = SpriteAnimator 同様)
    void RemoveKey(u32 index) noexcept;

    void ClearKeys() noexcept;

    u32             KeyCount() const noexcept { return static_cast<u32>(_keys.Size()); }
    const CurveKey* EKey(u32 index) const noexcept {
        return index < _keys.Size() ? &_keys[index] : nullptr;
    }

    // 時間 → 値の補間評価。key が 0 個なら 0、1 個ならその value。
    // time が [0, Duration()] を外れる場合は SetPreWrap / SetPostWrap に従う。
    f32 Evaluate(f32 time) const noexcept;

    // 末尾 key の time (絶対値)。key 0 個では 0。
    // 注: 「曲線の長さ」ではなく「最後のキーの time」を返す仕様 (Unity の
    // AnimationCurve.keys[last].time に相当)。最初の key の time が 0 でない
    // 場合は「Duration() - 第 1 key の time」が真の長さとなる。
    f32 Duration() const noexcept;

    // time < (第 1 key の time) のときの挙動。典型ケースで第 1 key.time == 0 なら
    // 「time < 0」のときの挙動 = Unity の preWrapMode と等価。
    void SetPreWrap (WrapMode m) noexcept { _pre_wrap  = m; }
    // time > (末尾 key の time = Duration()) のときの挙動 = Unity の postWrapMode。
    void SetPostWrap(WrapMode m) noexcept { _post_wrap = m; }

    WrapMode PreWrap () const noexcept { return _pre_wrap;  }
    WrapMode PostWrap() const noexcept { return _post_wrap; }

private:
    // 時間を [0, Duration()] にラップ (WrapMode 適用)。空 / Duration==0 の場合は
    // そのまま time を返し (上位で個別処理する)、それ以外で正規化された時間を返す。
    f32 ApplyWrap(f32 time) const noexcept;

    // time が key[i].time <= time <= key[i+1].time となる i を二分探索で取得。
    // 戻り値は左端 key の index。time が末尾以降や先頭未満であれば
    // 端の index にクランプ (呼び出し側はラップ済み time を渡す前提)。
    u32 FindSegmentIndex(f32 time) const noexcept;

    // segment [k0, k1] を out_interp で補間。t は [0,1] の正規化進度、
    // dt は segment 長 (秒)。Hermite ではタンジェントを dt で乗算してスケール。
    static f32 InterpolateSegment(const CurveKey& k0, const CurveKey& k1,
                                  f32 t, f32 dt) noexcept;

    TArray<CurveKey> _keys;
    WrapMode        _pre_wrap  = WrapMode::Clamp;
    WrapMode        _post_wrap = WrapMode::Clamp;
};

} // namespace acs::game
