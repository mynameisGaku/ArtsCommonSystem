// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar Q — FAmbientDirector (Time-of-Day)
//
// 1 日の時刻 (0..24h) に応じて sky color / ambient color / sun direction を
// キーフレーム間で線形補間する time-of-day ドライバ。レンダラ側 (FPbrShader /
// SkyShader / 環境光ステージ) は本クラスの 3 つの色 / 方向ベクトルを毎フレーム
// pull するだけで一日の表情が出る。
//
// 使い方:
//   class FWorldScene : public FScene {
//       acs::game::FAmbientDirector m_Ambient;
//
//       void OnEnter() noexcept override {
//           m_Ambient.SetTimeOfDay(6.5f);     // 朝焼け開始
//           m_Ambient.SetTimeScale(0.1f);     // リアル 1s = 0.1 game-hour
//                                            // → 1 game 日 = リアル 240s
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           m_Ambient.Tick(dt);
//           FRenderer().SetSkyColor    (m_Ambient.SkyColor());
//           FRenderer().SetAmbientColor(m_Ambient.AmbientColor());
//           FRenderer().SetSunDir      (m_Ambient.SunDirection());
//       }
//   };
//
// 設計選択 (Pillar Q):
//   ・**キーフレーム = const static**: 6 個の {hour, sky, ambient} stop を
//     クラス内 static 配列で保持。利用者が改変するのは「現在時刻」と
//     「タイムスケール」のみ。色彩設計はライブラリ側のデフォルトで
//     「とりあえず動く」状態に。カスタム stop API は将来追加予定。
//   ・**線形補間** (Lerp): Hermite / Catmull-Rom 補間は将来対応。線形でも
//     6 stop なら継ぎ目の不連続は許容範囲。
//   ・**夜の wrap**: 22h と 翌 4h の stop を「22..24, 0..4」両方に置く形で
//     ラップ。`SetTimeOfDay(23.5f)` でも `(2.5f)` でも自然な夜色になる。
//   ・**SunDirection**: hour_angle = (hour - 6) / 12 * π。
//     6:00 → angle=0 (東 = +X)、12:00 → π/2 (天頂 = +Y)、18:00 → π (西 = -X)。
//     夜間 (0..6 と 18..24) は y < 0 となり「地平線下」を素直に表現。
//     方位 (Z) は固定 0 (= 真東西軌道)、将来 季節 / 緯度オフセット予定。
//   ・**Tick(dt) と AdvanceTime(dh)**: 前者はリアル秒、後者はゲーム時間。
//     SetTimeScale(game_h_per_real_sec) で換算。デフォルト 1/60 = リアル 1 分
//     でゲーム 1 時間。
//
// 範囲外:
//   ・カスタム FTimeStop の動的追加 (= 季節 / 天候モードによる切替)
//   ・Hermite / spline 補間で滑らかな黄昏遷移
//   ・SH9 / ambient cube による方向性 ambient
//   ・月の方向 / 月相
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Math.h"

namespace acs::game {

/**
 * 1 日の時刻に応じて sky / ambient color と sun direction を補間する time-of-day ドライバ。
 *
 * @details
 * 6 個の固定キーフレーム ({hour, sky, ambient}) を時刻間で線形補間し、夜は 22:00→24:00≡0:00
 * でラップさせる。レンダラ側は SkyColor() / AmbientColor() / SunDirection() を毎フレーム pull
 * するだけで一日の表情が出る。Tick(dt) でリアル秒を、AdvanceTime(dh) でゲーム時間を進め、
 * 換算は SetTimeScale() で行う。非コピー・非ムーブ。
 */
class FAmbientDirector {
public:
    /** 既定値で構築する (時刻 12:00、タイムスケール 1/60)。 */
    FAmbientDirector() noexcept = default;

    /** 破棄する (保持するのは値のみで後始末は不要)。 */
    ~FAmbientDirector() noexcept = default;

    /** コピー禁止。 */
    FAmbientDirector(const FAmbientDirector&)            = delete;

    /** コピー代入も禁止。 */
    FAmbientDirector& operator=(const FAmbientDirector&) = delete;

    /** ムーブ禁止。 */
    FAmbientDirector(FAmbientDirector&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FAmbientDirector& operator=(FAmbientDirector&&)      = delete;

    /**
     * 現在時刻を設定する。
     *
     * @details 24 以上 / 負値は 24 で剰余を取って [0, 24) に正規化する。
     * @param hours 設定する時刻 (時)。
     */
    void SetTimeOfDay(f32 hours) noexcept;

    /**
     * ゲーム時間を dt_hours だけ進める。
     *
     * @details 負値は 0 にクランプする (時を戻さない方針)。結果は [0, 24) にラップする。
     * @param dt_hours 進める時間 (時)。
     */
    void AdvanceTime(f32 dt_hours) noexcept;

    /**
     * 現在時刻を返す。
     *
     * @return 現在時刻 [0, 24)。
     */
    f32 TimeOfDay() const noexcept { return m_Hours; }

    /**
     * 現在時刻での空の色を返す。
     *
     * @return キーフレームを線形補間した sky color (RGB)。
     */
    FVec3 SkyColor()     const noexcept;

    /**
     * 現在時刻での環境光の色を返す。
     *
     * @return キーフレームを線形補間した ambient color (RGB)。
     */
    FVec3 AmbientColor() const noexcept;

    /**
     * 現在時刻での太陽方向を返す。
     *
     * @details hour_angle = (hour - 6) / 12 * π で算出。06:00→東地平線、12:00→天頂、
     * 18:00→西地平線、夜間は y < 0 (地平線下)。方位 (z) は 0 固定。
     * @return 単位長の太陽方向ベクトル。
     */
    FVec3 SunDirection() const noexcept;

    /**
     * 現在が昼かを返す。
     *
     * @return 時刻が [6, 18) なら true。
     */
    bool IsDay()   const noexcept { return m_Hours >= 6.0f && m_Hours < 18.0f; }

    /**
     * 現在が夜かを返す。
     *
     * @return 昼でなければ true。
     */
    bool IsNight() const noexcept { return !IsDay(); }

    /**
     * リアル時間からゲーム時間への換算係数を設定する。
     *
     * @details 既定 1/60 = リアル 1 分でゲーム 1 時間 (= 1 日 24 分)。負値は 0 にクランプ。
     * @param game_hours_per_real_sec リアル 1 秒で進めるゲーム時間。
     */
    void SetTimeScale(f32 game_hours_per_real_sec) noexcept {
        m_TimeScale = game_hours_per_real_sec >= 0.0f ? game_hours_per_real_sec : 0.0f;
    }

    /**
     * 現在のタイムスケールを返す。
     *
     * @return リアル 1 秒あたりのゲーム時間。
     */
    f32 TimeScale() const noexcept { return m_TimeScale; }

    /**
     * リアル経過秒に応じてゲーム時間を進める (毎フレーム呼ぶ)。
     *
     * @details dt をタイムスケール換算して AdvanceTime に渡す。負値は 0 にクランプ。
     * @param dt 前フレームからの経過秒 (リアル秒)。
     */
    void Tick(f32 dt) noexcept {
        if (dt < 0.0f) dt = 0.0f;
        AdvanceTime(dt * m_TimeScale);
    }

public:
    /**
     * 補間用のキーフレーム stop ({hour, sky, ambient})。
     *
     * @details .cpp の自由関数からもアクセスするため public 配置 (テストや拡張カスタムにも便利)。
     */
    struct FTimeStop {
        /** この stop の時刻 [0, 24]。 */
        f32  hour;

        /** この時刻の空の色 (RGB)。 */
        FVec3 sky;

        /** この時刻の環境光の色 (RGB)。 */
        FVec3 ambient;
    };

private:
    /** 6 個の固定 stop (hour 昇順。0:00 と 22:00 を同じ夜色にしてラップを連続化)。 */
    static const FTimeStop m_Stops[6];

    /** 現在時刻 [0, 24)。 */
    f32 m_Hours      = 12.0f;

    /** リアル時間→ゲーム時間の換算係数 (既定 1/60 = リアル 1s で 1 game-min)。 */
    f32 m_TimeScale = 1.0f / 60.0f;
};

} // namespace acs::game
