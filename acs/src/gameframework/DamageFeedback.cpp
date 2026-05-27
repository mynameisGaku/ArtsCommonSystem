// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — FDamageFeedback 実装
//
// 仕様の意図は FDamageFeedback.h を参照。本ファイルは純粋 state machine としての
// 加算 / clamp / decay / death cam timer の実装に徹する。
#include "gameframework/DamageFeedback.h"

namespace acs::game {

// =============================================================================
// 内部定数
// -----------------------------------------------------------------------------
// 赤エッジ / 方向矢印の decay rate は同一値 (2.0/s = 0.5 秒で消える) で
// 統一する。挙動を揃えることで「ダメージ余韻」が一括して馴染む。
// _kDamageToRed は「与ダメ 10 で max まで赤くなる」感覚の係数。
// _kDeathCamRate は death cam の dramatic zoom 進行速度 (0.5/s = 2 秒で完走)。
// =============================================================================
namespace {
constexpr f32 kRedDecayRate     = 2.0f;     // /s
constexpr f32 kDirDecayRate     = 2.0f;     // /s
constexpr f32 kDamageToRed      = 0.1f;     // damage 1.0 → +0.1 red
constexpr f32 kDeathCamRate     = 0.5f;     // /s (= 2 秒で 0→1)
} // namespace

// =============================================================================
// TakeDamage
// -----------------------------------------------------------------------------
// 赤エッジ:
//   `m_RedIntensity += amount * kDamageToRed` を加算 → [0,1] clamp。
//   負ダメージは赤を減らさない (回復は別系統 → 何もしない)。
// 方向矢印:
//   source_direction が零ベクトル相当なら矢印を **更新しない** (= 既存の矢印
//   decay をそのまま継続)。これは「環境ダメージは矢印を出さない」要件。
//   非零なら正規化して m_DirVec に格納、m_DirIntensity = 1.0 リセット。
//   Normalize(零) は FVec2::Zero() を返すので、LengthSq による判定で除外。
// =============================================================================
void FDamageFeedback::TakeDamage(f32 amount, FVec2 source_direction) noexcept {
    if (amount <= 0.0f) {
        // 0 / 負ダメージは fail-safe で何もしない (回復扱いは別 API の責務)
        return;
    }

    // 赤エッジ加算 + [0,1] clamp
    m_RedIntensity += amount * kDamageToRed;
    if (m_RedIntensity > 1.0f) m_RedIntensity = 1.0f;
    if (m_RedIntensity < 0.0f) m_RedIntensity = 0.0f;

    // 累積ダメージ (将来拡張用フック)
    m_RecentDamageTotal += amount;

    // 方向矢印: 零ベクトル判定は LengthSq で行い、Normalize の零除算を回避
    const f32 lensq = LengthSq(source_direction);
    if (lensq > kEpsilon * kEpsilon) {
        m_DirVec       = Normalize(source_direction);
        m_DirIntensity = 1.0f;
    }
}

// =============================================================================
// Tick
// -----------------------------------------------------------------------------
//   ・赤エッジ: 線形 decay (kRedDecayRate * dt)。0 を下回らない。
//   ・方向矢印: 線形 decay (kDirDecayRate * dt)。0 になっても m_DirVec は
//      残す (= 次フレームで HasDirectionalIndicator() = false になるだけ)。
//   ・death cam: アクティブ中のみ progress を 0 → 1 に加算 (1 で頭打ち)。
//      ExitDeathCam が呼ばれるまで m_DeathCamActive は true のまま。
// dt が負 (clock 巻き戻し等) のときは 0 扱いで state を破壊しない。
// =============================================================================
void FDamageFeedback::Tick(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;

    // 赤エッジ decay
    if (m_RedIntensity > 0.0f) {
        m_RedIntensity -= kRedDecayRate * dt;
        if (m_RedIntensity < 0.0f) m_RedIntensity = 0.0f;
    }

    // 方向矢印 decay
    if (m_DirIntensity > 0.0f) {
        m_DirIntensity -= kDirDecayRate * dt;
        if (m_DirIntensity < 0.0f) m_DirIntensity = 0.0f;
    }

    // death cam progress (active 中のみ、1 で頭打ち)
    if (m_DeathCamActive) {
        m_DeathCamT += kDeathCamRate * dt;
        if (m_DeathCamT > 1.0f) m_DeathCamT = 1.0f;
    }
}

// =============================================================================
// TriggerDeathCam
// -----------------------------------------------------------------------------
// 致命傷検知時に caller が呼ぶ。既に active なら killer_pos だけ更新
// (= 連続致命傷でターゲットが切り替わっても演出を仕切り直さない)。
// progress リセットは「初回 trigger 時のみ」に限定して、矢印 / 赤エッジは
// 別系統として残す (=「赤エッジが残ったまま death cam に入る」自然な遷移)。
// =============================================================================
void FDamageFeedback::TriggerDeathCam(FVec3 killer_pos) noexcept {
    m_KillerPos = killer_pos;
    if (!m_DeathCamActive) {
        m_DeathCamActive = true;
        m_DeathCamT      = 0.0f;
    }
}

// =============================================================================
// ExitDeathCam
// -----------------------------------------------------------------------------
// リスポーン UI / シーン遷移開始時に caller が呼ぶ。state を全て初期に戻す
// (progress も 0)。m_KillerPos は次の trigger で上書きされるので保持で問題なし。
// =============================================================================
void FDamageFeedback::ExitDeathCam() noexcept {
    m_DeathCamActive = false;
    m_DeathCamT      = 0.0f;
}

// =============================================================================
// Reset
// -----------------------------------------------------------------------------
// シーン切替 / respawn 用の一括クリア。全 state を初期値に戻す。
// =============================================================================
void FDamageFeedback::Reset() noexcept {
    m_RedIntensity        = 0.0f;
    m_DirIntensity        = 0.0f;
    m_DirVec              = FVec2{0.0f, 0.0f};
    m_DeathCamActive     = false;
    m_DeathCamT          = 0.0f;
    m_KillerPos           = FVec3{0.0f, 0.0f, 0.0f};
    m_RecentDamageTotal  = 0.0f;
}

} // namespace acs::game
