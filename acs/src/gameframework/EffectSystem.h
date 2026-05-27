// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar I — FEffectSystem (画面演出指揮)
//
// 画面エフェクトの中央指揮塔: Flash (全画面色被せ) / HitStop (一時停止) /
// FCamera Shake (FCamera2D 連携) をひとまとめにし、描画側 / FGame ループ /
// FCamera2D が「pull する側」になる単純なバスとして振る舞う。
//
// 設計選択 (Phase ?? = Pillar I Phase 1):
//   ・**FEffectSystem 自身は何も描画しない / 何も時間を止めない**:
//      Flash → 描画パイプ末尾の overlay が `FlashColor() * FlashIntensity()`
//             を加算 (alpha blend) して画面に被せる。
//      HitStop → FGame ループが `IsHitStop()` を見て `time_scale = 0` を選ぶ。
//                FEffectSystem 自体は real-time dt で Tick され続ける
//                (= hit stop が真の場合でも残時間が減る)。
//      Shake → FCamera2D が `PendingShakeTrauma()` を pull → `AddShake` →
//              `ConsumeShake()` で 0 リセット。
//      これにより FEffectSystem は **副作用ゼロ / 純粋 state machine**。
//      テスト容易 + FCamera2D / FGame との結合が最小限。
//   ・**Flash は線形減衰**: `intensity(t) = m_FlashMax * (m_FlashT / m_FlashTotal)`
//      で 1 → 0 へ落ちる。fade out 寄りで「閃光が引いていく」感が出る。
//      Flash() を再呼出すれば常に新しい flash で上書き (累積しない)。
//   ・**HitStop は単純な timer**: 重ねがけしたとき、残時間と新時間の max を採用
//      (= 強い hit stop が来たら短い hit stop で打ち消されない)。
//   ・**Shake pending は overwrite**: 1 フレームで複数 hit が起きたら最大値を保つ
//      (max-of-frame)。FCamera2D が次フレーム頭で consume するので、
//      ゲームコード側が consume 順序を意識する必要は無い。
//
// 使い方:
//   class GameplayScene : public Scene {
//       FEffectSystem m_Fx;
//       void OnHit() noexcept {
//           m_Fx.Flash({1,1,1}, 0.8f, 0.15f);   // 白フラッシュ 150ms
//           m_Fx.HitStop(0.08f);                 // 80ms 停止
//           m_Fx.TriggerShake(0.5f);             // trauma 0.5 を FCamera2D に流す
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           const f32 ts = m_Fx.IsHitStop() ? 0.0f : 1.0f;
//           // ... game update with (dt * ts) ...
//           m_Fx.Tick(dt);                       // FEffectSystem は real dt で進む
//           if (m_Fx.PendingShakeTrauma() > 0.0f) {
//               Services().Camera().AddShake(m_Fx.PendingShakeTrauma());
//               m_Fx.ConsumeShake();
//           }
//       }
//   };
//
// 範囲外 (Phase 2+ で):
//   ・color grading / chromatic aberration / vignette / radial blur のレシピ
//   ・複数 channel (UI / world / cinematic) で独立 mute
//   ・curve-based intensity (Easing 連携)
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

class FEffectSystem {
public:
    FEffectSystem() noexcept = default;
    ~FEffectSystem() noexcept = default;

    FEffectSystem(const FEffectSystem&)            = delete;
    FEffectSystem& operator=(const FEffectSystem&) = delete;
    FEffectSystem(FEffectSystem&&)                 = delete;
    FEffectSystem& operator=(FEffectSystem&&)      = delete;

    // ----- Flash (画面全体に色を被せる) -----
    // color    : RGB (0..1)、描画側が `color * intensity` を加算 blend する想定
    // intensity: 最大強度 (0..1 が無難。1.0 で完全白飛び)
    // duration : 0 → fully bright、duration 経過で 0 (線形減衰)
    void Flash(FVec3 color, f32 intensity, f32 duration) noexcept;

    // ----- HitStop (一時停止) -----
    // duration 秒の間、IsHitStop() が true を返す。
    // FGame ループはこれを見て time_scale=0 にする責務を持つ。
    // 重ねがけは max を採用 (短い hit stop が長い hit stop を打ち消さない)。
    void HitStop(f32 duration) noexcept;

    // ----- FCamera Shake (FCamera2D 連携) -----
    // trauma      : 0..1、FCamera2D::AddShake にそのまま渡せる値
    // duration_hint: 将来用 (現状未使用、API 安定のため shape を確保)
    // 同一フレーム中の複数呼出は max を保つ (max-of-frame)。
    void TriggerShake(f32 trauma, f32 duration_hint = 0.0f) noexcept;

    // ----- driver -----
    // dt は real-time dt (hit stop 中も 0 にしない)。
    // 全エフェクトの内部 timer を進める。
    void Tick(f32 dt) noexcept;

    // ----- accessors (描画側 / FCamera2D 側が pull する) -----
    // 0 = フラッシュなし、>0 で描画側が overlay
    f32  FlashIntensity() const noexcept;
    FVec3 FlashColor()     const noexcept { return m_FlashColor; }

    bool IsHitStop()      const noexcept { return m_HitStopRemain > 0.0f; }
    f32  HitStopRemain()  const noexcept { return m_HitStopRemain; }

    // FCamera2D 側が読んで AddShake に流す
    f32  PendingShakeTrauma() const noexcept { return m_PendingShake; }
    // FCamera2D 側が AddShake 後に呼んで pending を 0 に
    void ConsumeShake() noexcept { m_PendingShake = 0.0f; }

private:
    // Flash state
    f32  m_FlashT        = 0.0f;        // 残時間 (0..flash_total)
    f32  m_FlashTotal    = 0.0f;        // 開始時 duration (除算用)
    f32  m_FlashMax      = 0.0f;        // ピーク intensity
    FVec3 m_FlashColor   {0.0f, 0.0f, 0.0f};

    // HitStop state
    f32  m_HitStopRemain = 0.0f;

    // Shake pending (FCamera2D が次フレーム consume するまで保持)
    f32  m_PendingShake   = 0.0f;
};

} // namespace acs::game
