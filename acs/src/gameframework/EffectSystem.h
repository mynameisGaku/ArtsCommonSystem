// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar I — CEffectSystem (画面演出指揮)
//
// 画面エフェクトの中央指揮塔: Flash (全画面色被せ) / HitStop (一時停止) /
// CCamera Shake (CCamera2D 連携) をひとまとめにし、描画側 / CGame ループ /
// CCamera2D が「pull する側」になる単純なバスとして振る舞う。
//
// 設計選択 (Pillar I):
//   ・**CEffectSystem 自身は何も描画しない / 何も時間を止めない**:
//      Flash → 描画パイプ末尾の overlay が `FlashColor() * FlashIntensity()`
//             を加算 (alpha blend) して画面に被せる。
//      HitStop → CGame ループが `IsHitStop()` を見て `time_scale = 0` を選ぶ。
//                CEffectSystem 自体は real-time dt で Tick され続ける
//                (= hit stop が真の場合でも残時間が減る)。
//      Shake → CCamera2D が `PendingShakeTrauma()` を pull → `AddShake` →
//              `ConsumeShake()` で 0 リセット。
//      これにより CEffectSystem は **副作用ゼロ / 純粋 state machine**。
//      テスト容易 + CCamera2D / CGame との結合が最小限。
//   ・**Flash は線形減衰**: `intensity(t) = m_FlashMax * (m_FlashT / m_FlashTotal)`
//      で 1 → 0 へ落ちる。fade out 寄りで「閃光が引いていく」感が出る。
//      Flash() を再呼出すれば常に新しい flash で上書き (累積しない)。
//   ・**HitStop は単純な timer**: 重ねがけしたとき、残時間と新時間の max を採用
//      (= 強い hit stop が来たら短い hit stop で打ち消されない)。
//   ・**Shake pending は overwrite**: 1 フレームで複数 hit が起きたら最大値を保つ
//      (max-of-frame)。CCamera2D が次フレーム頭で consume するので、
//      ゲームコード側が consume 順序を意識する必要は無い。
//
// 使い方:
//   class FGameplayScene : public AScene {
//       CEffectSystem m_Fx;
//       void OnHit() noexcept {
//           m_Fx.Flash({1,1,1}, 0.8f, 0.15f);   // 白フラッシュ 150ms
//           m_Fx.HitStop(0.08f);                 // 80ms 停止
//           m_Fx.TriggerShake(0.5f);             // trauma 0.5 を CCamera2D に流す
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           const f32 ts = m_Fx.IsHitStop() ? 0.0f : 1.0f;
//           // ... game update with (dt * ts) ...
//           m_Fx.Tick(dt);                       // CEffectSystem は real dt で進む
//           if (m_Fx.PendingShakeTrauma() > 0.0f) {
//               Services().Camera().AddShake(m_Fx.PendingShakeTrauma());
//               m_Fx.ConsumeShake();
//           }
//       }
//   };
//
// 範囲外:
//   ・color grading / chromatic aberration / vignette / radial blur のレシピ
//   ・複数 channel (UI / world / cinematic) で独立 mute
//   ・curve-based intensity (Easing 連携)
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * Flash / HitStop / カメラシェイクをまとめる画面演出の中央指揮塔。
 *
 * @details
 * 自身は何も描画せず時間も止めない純粋 state machine で、描画側 / CGame ループ /
 * CCamera2D が pull する単純なバスとして振る舞う。Flash は描画パイプ末尾が
 * FlashColor() * FlashIntensity() を加算 overlay し、HitStop は CGame が
 * IsHitStop() を見て time_scale=0 を選び、Shake は CCamera2D が PendingShakeTrauma()
 * を pull → AddShake → ConsumeShake() で消費する。Tick は常に real-time dt で進める。
 */
class CEffectSystem {
public:
    /** 全エフェクトを停止状態で構築する。 */
    CEffectSystem() noexcept = default;

    /** デストラクタ (所有リソースなし)。 */
    ~CEffectSystem() noexcept = default;

    /** コピー禁止 (演出状態を単独所有するため)。 */
    CEffectSystem(const CEffectSystem&)            = delete;

    /** コピー代入も禁止。 */
    CEffectSystem& operator=(const CEffectSystem&) = delete;

    /** ムーブ禁止。 */
    CEffectSystem(CEffectSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CEffectSystem& operator=(CEffectSystem&&)      = delete;

    /**
     * 画面全体に被せるフラッシュを開始する (最新の呼出で上書き、累積しない)。
     *
     * @details intensity の負値は 0 に clamp、duration <= 0 は走行中の flash を即消去。
     * @param color フラッシュ色 RGB (0..1)。描画側が color * intensity を加算 blend する。
     * @param intensity ピーク強度 (0..1 が無難、1.0 で完全白飛び。負値は 0 に clamp)。
     * @param duration ピークから 0 へ線形減衰するまでの秒。0 以下で即消去。
     */
    void Flash(FVec3 color, f32 intensity, f32 duration) noexcept;

    /**
     * ヒットストップ (一時停止) を要求する。
     *
     * @details
     * duration 秒の間 IsHitStop() が true を返す。CGame ループがこれを見て time_scale=0
     * にする責務を持つ。重ねがけは max を採用 (短い hit stop が長い hit stop を打ち消さない)。
     * @param duration 停止させたい秒。
     */
    void HitStop(f32 duration) noexcept;

    /**
     * カメラシェイクの pending trauma を積む。
     *
     * @details
     * 同一フレーム中の複数呼出は max を保つ (max-of-frame)。CCamera2D が次フレーム頭で
     * ConsumeShake する想定。
     * @param trauma 0..1 のシェイク強度 (CCamera2D::AddShake にそのまま渡せる値)。
     * @param duration_hint 将来用 (現状未使用、API 安定のため shape を確保)。
     */
    void TriggerShake(f32 trauma, f32 duration_hint = 0.0f) noexcept;

    /**
     * 全エフェクトの内部 timer を進める。
     *
     * @details dt は real-time dt を渡す (hit stop 中も 0 にしない)。
     * @param dt 前フレームからの実経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 現在のフラッシュ強度を返す (描画側が overlay 用に pull する)。
     *
     * @return 0 = フラッシュなし、>0 で描画側が overlay に使う強度。
     */
    f32  FlashIntensity() const noexcept;

    /**
     * 現在のフラッシュ色を返す。
     *
     * @return フラッシュ色 RGB。
     */
    FVec3 FlashColor()     const noexcept { return m_FlashColor; }

    /**
     * ヒットストップ中かを返す (CGame ループが time_scale 判定に使う)。
     *
     * @return 残時間が残っていれば true。
     */
    bool IsHitStop()      const noexcept { return m_HitStopRemain > 0.0f; }

    /**
     * ヒットストップの残時間を返す。
     *
     * @return 停止の残り秒。
     */
    f32  HitStopRemain()  const noexcept { return m_HitStopRemain; }

    /**
     * pending のシェイク trauma を返す (CCamera2D が読んで AddShake に流す)。
     *
     * @return 未消費の trauma (0 ならシェイク要求なし)。
     */
    f32  PendingShakeTrauma() const noexcept { return m_PendingShake; }

    /** pending のシェイク trauma を 0 にリセットする (CCamera2D が AddShake 後に呼ぶ)。 */
    void ConsumeShake() noexcept { m_PendingShake = 0.0f; }

private:
    /** フラッシュの残時間 (0..m_FlashTotal)。 */
    f32  m_FlashT        = 0.0f;

    /** フラッシュ開始時の duration (減衰の除算に使う)。 */
    f32  m_FlashTotal    = 0.0f;

    /** フラッシュのピーク強度。 */
    f32  m_FlashMax      = 0.0f;

    /** フラッシュ色 RGB。 */
    FVec3 m_FlashColor   {0.0f, 0.0f, 0.0f};

    /** ヒットストップの残時間。 */
    f32  m_HitStopRemain = 0.0f;

    /** 未消費のシェイク trauma (CCamera2D が次フレーム consume するまで保持)。 */
    f32  m_PendingShake   = 0.0f;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FEffectSystem = CEffectSystem;

} // namespace acs::game
