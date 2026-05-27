// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar Q — FAmbientDirector (Time-of-Day)
//
// 1 日の時刻 (0..24h) に応じて sky color / ambient color / sun direction を
// キーフレーム間で線形補間する time-of-day ドライバ。レンダラ側 (FPbrShader /
// SkyShader / 環境光ステージ) は本クラスの 3 つの色 / 方向ベクトルを毎フレーム
// pull するだけで一日の表情が出る。
//
// 使い方:
//   class WorldScene : public Scene {
//       acs::game::FAmbientDirector _ambient;
//
//       void OnEnter() noexcept override {
//           _ambient.SetTimeOfDay(6.5f);     // 朝焼け開始
//           _ambient.SetTimeScale(0.1f);     // リアル 1s = 0.1 game-hour
//                                            // → 1 game 日 = リアル 240s
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           _ambient.Tick(dt);
//           FRenderer().SetSkyColor    (_ambient.SkyColor());
//           FRenderer().SetAmbientColor(_ambient.AmbientColor());
//           FRenderer().SetSunDir      (_ambient.SunDirection());
//       }
//   };
//
// 設計選択 (Pillar Q Phase 1):
//   ・**キーフレーム = const static**: 6 個の {hour, sky, ambient} stop を
//     クラス内 static 配列で保持。利用者が改変するのは「現在時刻」と
//     「タイムスケール」のみ。色彩設計はライブラリ側のデフォルトで
//     「とりあえず動く」状態に。Phase 2 でカスタム stop API を追加予定。
//   ・**線形補間** (Lerp): Hermite / Catmull-Rom 補間は Phase 2。線形でも
//     6 stop なら継ぎ目の不連続は許容範囲。
//   ・**夜の wrap**: 22h と 翌 4h の stop を「22..24, 0..4」両方に置く形で
//     ラップ。`SetTimeOfDay(23.5f)` でも `(2.5f)` でも自然な夜色になる。
//   ・**SunDirection**: hour_angle = (hour - 6) / 12 * π。
//     6:00 → angle=0 (東 = +X)、12:00 → π/2 (天頂 = +Y)、18:00 → π (西 = -X)。
//     夜間 (0..6 と 18..24) は y < 0 となり「地平線下」を素直に表現。
//     方位 (Z) は固定 0 (= 真東西軌道)、Phase 2 で季節 / 緯度オフセット予定。
//   ・**Tick(dt) と AdvanceTime(dh)**: 前者はリアル秒、後者はゲーム時間。
//     SetTimeScale(game_h_per_real_sec) で換算。デフォルト 1/60 = リアル 1 分
//     でゲーム 1 時間。
//
// 範囲外 (Phase 2+ で):
//   ・カスタム TimeStop の動的追加 (= 季節 / 天候モードによる切替)
//   ・Hermite / spline 補間で滑らかな黄昏遷移
//   ・SH9 / ambient cube による方向性 ambient
//   ・月の方向 / 月相
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Math.h"

namespace acs::game {

class FAmbientDirector {
public:
    FAmbientDirector() noexcept = default;
    ~FAmbientDirector() noexcept = default;

    FAmbientDirector(const FAmbientDirector&)            = delete;
    FAmbientDirector& operator=(const FAmbientDirector&) = delete;
    FAmbientDirector(FAmbientDirector&&)                 = delete;
    FAmbientDirector& operator=(FAmbientDirector&&)      = delete;

    // ----- 時刻設定 -----
    // hours は [0, 24)。24 以上 / 負値は 24 で剰余を取って正規化。
    void SetTimeOfDay(f32 hours) noexcept;

    // dt_hours だけゲーム時間を進める。負値は 0 にクランプ (時を戻さない方針)。
    void AdvanceTime(f32 dt_hours) noexcept;

    f32 TimeOfDay() const noexcept { return _hours; }

    // ----- 色・方向 (補間結果を pull) -----
    FVec3 SkyColor()     const noexcept;
    FVec3 AmbientColor() const noexcept;
    FVec3 SunDirection() const noexcept;

    // ----- 昼夜判定 -----
    bool IsDay()   const noexcept { return _hours >= 6.0f && _hours < 18.0f; }
    bool IsNight() const noexcept { return !IsDay(); }

    // ----- リアル時間 → ゲーム時間 換算 -----
    // game_hours_per_real_sec: リアル 1 秒で進めるゲーム時間。
    // 既定 1/60 = リアル 1 分でゲーム 1 時間 (= 1 日 24 分)。
    void SetTimeScale(f32 game_hours_per_real_sec) noexcept {
        _time_scale = game_hours_per_real_sec >= 0.0f ? game_hours_per_real_sec : 0.0f;
    }
    f32 TimeScale() const noexcept { return _time_scale; }

    // FSceneServices などから毎フレーム呼ぶ。dt はリアル秒。
    void Tick(f32 dt) noexcept {
        if (dt < 0.0f) dt = 0.0f;
        AdvanceTime(dt * _time_scale);
    }

public:
    // 補間用 stop。`hour` は [0, 24]。内部 helper だが .cpp の自由関数からも
    // アクセスする必要があるので public 配置 (テストや拡張カスタムにも便利)。
    struct TimeStop {
        f32  hour;
        FVec3 sky;
        FVec3 ambient;
    };

private:
    // 6 個の固定 stop。0:00 と 24:00 は夜色で揃えてラップを連続化。
    static const TimeStop _stops[6];

    // 現在時刻 [0, 24)。
    f32 _hours      = 12.0f;
    f32 _time_scale = 1.0f / 60.0f;  // リアル 1s = 1/60 game-h = 1 game-min
};

} // namespace acs::game
