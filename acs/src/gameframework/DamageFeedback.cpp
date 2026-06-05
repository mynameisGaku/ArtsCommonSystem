// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — FDamageFeedback 実装
//
// 仕様の意図は FDamageFeedback.h を参照。本ファイルは純粋 state machine としての
// 加算 / clamp / decay / death cam timer の実装に徹する。
#include "gameframework/DamageFeedback.h"

namespace acs::game {

namespace {
/** 赤エッジ強度の decay 速度 (2.0/s = 約 0.5 秒で消える)。 */
constexpr f32 kRedDecayRate     = 2.0f;

/** 方向矢印不透明度の decay 速度 (赤エッジと揃えた 2.0/s)。 */
constexpr f32 kDirDecayRate     = 2.0f;

/** ダメージ量 → 赤エッジ加算量への変換係数 (damage 1.0 で +0.1)。 */
constexpr f32 kDamageToRed      = 0.1f;

/** death cam の dramatic zoom 進行速度 (0.5/s = 2 秒で 0→1 完走)。 */
constexpr f32 kDeathCamRate     = 0.5f;
} // namespace

/** 赤エッジを amount*0.1 加算 [0,1] clamp し、非零方向なら矢印を正規化更新する。 */
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

/** 赤エッジ・方向矢印を線形 decay し、death cam active 中は progress を 1 まで進める。 */
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

/** killer 位置を保存し、未 active のときのみ death cam を起動して progress を 0 に戻す。 */
void FDamageFeedback::TriggerDeathCam(FVec3 killer_pos) noexcept {
    m_KillerPos = killer_pos;
    if (!m_DeathCamActive) {
        m_DeathCamActive = true;
        m_DeathCamT      = 0.0f;
    }
}

/** death cam を非 active にして progress を 0 に戻す (killer 位置は次 trigger で上書き)。 */
void FDamageFeedback::ExitDeathCam() noexcept {
    m_DeathCamActive = false;
    m_DeathCamT      = 0.0f;
}

/** 赤エッジ・方向矢印・death cam・累積ダメージを含む全 state を初期値へ戻す。 */
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
