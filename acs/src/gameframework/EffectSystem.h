// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar I — EffectSystem (画面演出指揮)
//
// 画面エフェクトの中央指揮塔: Flash (全画面色被せ) / HitStop (一時停止) /
// FCamera Shake (Camera2D 連携) をひとまとめにし、描画側 / Game ループ /
// Camera2D が「pull する側」になる単純なバスとして振る舞う。
//
// 設計選択 (Phase ?? = Pillar I Phase 1):
//   ・**EffectSystem 自身は何も描画しない / 何も時間を止めない**:
//      Flash → 描画パイプ末尾の overlay が `FlashColor() * FlashIntensity()`
//             を加算 (alpha blend) して画面に被せる。
//      HitStop → Game ループが `IsHitStop()` を見て `time_scale = 0` を選ぶ。
//                EffectSystem 自体は real-time dt で Tick され続ける
//                (= hit stop が真の場合でも残時間が減る)。
//      Shake → Camera2D が `PendingShakeTrauma()` を pull → `AddShake` →
//              `ConsumeShake()` で 0 リセット。
//      これにより EffectSystem は **副作用ゼロ / 純粋 state machine**。
//      テスト容易 + Camera2D / Game との結合が最小限。
//   ・**Flash は線形減衰**: `intensity(t) = _flash_max * (_flash_t / _flash_total)`
//      で 1 → 0 へ落ちる。fade out 寄りで「閃光が引いていく」感が出る。
//      Flash() を再呼出すれば常に新しい flash で上書き (累積しない)。
//   ・**HitStop は単純な timer**: 重ねがけしたとき、残時間と新時間の max を採用
//      (= 強い hit stop が来たら短い hit stop で打ち消されない)。
//   ・**Shake pending は overwrite**: 1 フレームで複数 hit が起きたら最大値を保つ
//      (max-of-frame)。Camera2D が次フレーム頭で consume するので、
//      ゲームコード側が consume 順序を意識する必要は無い。
//
// 使い方:
//   class GameplayScene : public Scene {
//       EffectSystem _fx;
//       void OnHit() noexcept {
//           _fx.Flash({1,1,1}, 0.8f, 0.15f);   // 白フラッシュ 150ms
//           _fx.HitStop(0.08f);                 // 80ms 停止
//           _fx.TriggerShake(0.5f);             // trauma 0.5 を Camera2D に流す
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           const f32 ts = _fx.IsHitStop() ? 0.0f : 1.0f;
//           // ... game update with (dt * ts) ...
//           _fx.Tick(dt);                       // EffectSystem は real dt で進む
//           if (_fx.PendingShakeTrauma() > 0.0f) {
//               Services().Camera().AddShake(_fx.PendingShakeTrauma());
//               _fx.ConsumeShake();
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

class EffectSystem {
public:
    EffectSystem() noexcept = default;
    ~EffectSystem() noexcept = default;

    EffectSystem(const EffectSystem&)            = delete;
    EffectSystem& operator=(const EffectSystem&) = delete;
    EffectSystem(EffectSystem&&)                 = delete;
    EffectSystem& operator=(EffectSystem&&)      = delete;

    // ----- Flash (画面全体に色を被せる) -----
    // color    : RGB (0..1)、描画側が `color * intensity` を加算 blend する想定
    // intensity: 最大強度 (0..1 が無難。1.0 で完全白飛び)
    // duration : 0 → fully bright、duration 経過で 0 (線形減衰)
    void Flash(FVec3 color, f32 intensity, f32 duration) noexcept;

    // ----- HitStop (一時停止) -----
    // duration 秒の間、IsHitStop() が true を返す。
    // Game ループはこれを見て time_scale=0 にする責務を持つ。
    // 重ねがけは max を採用 (短い hit stop が長い hit stop を打ち消さない)。
    void HitStop(f32 duration) noexcept;

    // ----- FCamera Shake (Camera2D 連携) -----
    // trauma      : 0..1、Camera2D::AddShake にそのまま渡せる値
    // duration_hint: 将来用 (現状未使用、API 安定のため shape を確保)
    // 同一フレーム中の複数呼出は max を保つ (max-of-frame)。
    void TriggerShake(f32 trauma, f32 duration_hint = 0.0f) noexcept;

    // ----- driver -----
    // dt は real-time dt (hit stop 中も 0 にしない)。
    // 全エフェクトの内部 timer を進める。
    void Tick(f32 dt) noexcept;

    // ----- accessors (描画側 / Camera2D 側が pull する) -----
    // 0 = フラッシュなし、>0 で描画側が overlay
    f32  FlashIntensity() const noexcept;
    FVec3 FlashColor()     const noexcept { return _flash_color; }

    bool IsHitStop()      const noexcept { return _hit_stop_remain > 0.0f; }
    f32  HitStopRemain()  const noexcept { return _hit_stop_remain; }

    // Camera2D 側が読んで AddShake に流す
    f32  PendingShakeTrauma() const noexcept { return _pending_shake; }
    // Camera2D 側が AddShake 後に呼んで pending を 0 に
    void ConsumeShake() noexcept { _pending_shake = 0.0f; }

private:
    // Flash state
    f32  _flash_t        = 0.0f;        // 残時間 (0..flash_total)
    f32  _flash_total    = 0.0f;        // 開始時 duration (除算用)
    f32  _flash_max      = 0.0f;        // ピーク intensity
    FVec3 _flash_color   {0.0f, 0.0f, 0.0f};

    // HitStop state
    f32  _hit_stop_remain = 0.0f;

    // Shake pending (Camera2D が次フレーム consume するまで保持)
    f32  _pending_shake   = 0.0f;
};

} // namespace acs::game
