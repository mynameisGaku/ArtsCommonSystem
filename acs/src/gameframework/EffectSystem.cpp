// SPDX-License-Identifier: Apache-2.0
// flash、hit stop、camera shakeの状態とtimerを更新し、描画・game loop・cameraへ現在値を提供する。
#include "gameframework/EffectSystem.h"

namespace acs::game {

/**
 * 画面全体に被せるフラッシュを開始する (常に最新の呼出で上書き、累積しない)。
 *
 * @details
 * intensity の負値は 0 に clamp する。duration <= 0 は走行中の flash を即座に消す
 * (0 duration は減衰の除算を避けたいため明示的に early-return)。
 * @param color フラッシュ色 RGB (0..1)。描画側が color * intensity を加算 blend する。
 * @param intensity ピーク強度 (負値は 0 に clamp)。
 * @param duration フラッシュが 0 に線形減衰するまでの秒。0 以下で即消去。
 */
void CEffectSystem::Flash(FVec3 color, f32 intensity, f32 duration) noexcept {
    if (duration <= 0.0f) {
        // すでに走っていた flash を即座に消す
        m_FlashT     = 0.0f;
        m_FlashTotal = 0.0f;
        m_FlashMax   = 0.0f;
        m_FlashColor = color;
        return;
    }
    // intensity の負値は意味不明なので 0 に clamp (上限は描画側に任せる)。
    const f32 i_safe = intensity < 0.0f ? 0.0f : intensity;
    m_FlashColor = color;
    m_FlashMax   = i_safe;
    m_FlashTotal = duration;
    m_FlashT     = duration;  // ピーク状態でスタート
}

/**
 * ヒットストップ (一時停止) を要求する。
 *
 * @details
 * 残時間と新時間の max を採用するため、強い hit stop が後発の弱い hit stop で
 * 打ち消されない。duration <= 0 はノーオプ。
 * @param duration 停止させたい秒。
 */
void CEffectSystem::HitStop(f32 duration) noexcept {
    if (duration <= 0.0f) return;
    if (duration > m_HitStopRemain) {
        m_HitStopRemain = duration;
    }
}

/**
 * カメラシェイクの pending trauma を積む。
 *
 * @details
 * 同一フレーム中の複数呼出は max を保つ (max-of-frame: 大ダメージ hit が小ヒットで
 * 上書きされない)。CCamera2D が次フレーム頭で ConsumeShake する想定。trauma <= 0
 * はノーオプ。duration_hint は将来 trauma decay 倍率に使う予約で現状は無視する。
 * @param trauma 0..1 のシェイク強度 (CCamera2D::AddShake にそのまま渡せる値)。
 * @param duration_hint 将来用 (現状未使用)。
 */
void CEffectSystem::TriggerShake(f32 trauma, f32 /*duration_hint*/) noexcept {
    if (trauma <= 0.0f) return;
    // pending は max-of-frame (CCamera2D の AddShake が累積側を担う)
    if (trauma > m_PendingShake) {
        m_PendingShake = trauma;
    }
}

/**
 * 全エフェクトの内部 timer を進める (real-time dt で呼ぶ)。
 *
 * @details
 * hit stop 中も dt は 0 にしない前提。Flash / HitStop の timer を線形減衰させ、
 * 0 を下回ったら停止状態にリセットする。pending shake はここでは消さない
 * (CCamera2D が ConsumeShake する責務)。dt が負 (clock 巻き戻し等) なら 0 扱いにして
 * state を破壊しない。
 * @param dt 前フレームからの実経過秒。
 */
void CEffectSystem::Tick(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;

    // Flash decay
    if (m_FlashT > 0.0f) {
        m_FlashT -= dt;
        if (m_FlashT <= 0.0f) {
            m_FlashT     = 0.0f;
            m_FlashMax   = 0.0f;
            m_FlashTotal = 0.0f;
        }
    }

    // HitStop decay
    if (m_HitStopRemain > 0.0f) {
        m_HitStopRemain -= dt;
        if (m_HitStopRemain < 0.0f) {
            m_HitStopRemain = 0.0f;
        }
    }

    // pending shake は ConsumeShake() が来るまで保持
}

/**
 * 現在のフラッシュ強度を線形減衰で返す。
 *
 * @details m_FlashMax * (m_FlashT / m_FlashTotal) で 1 → 0 へ落とす。
 * m_FlashTotal <= 0 または残時間 0 のときは 0 を返す (除算回避)。
 * @return 0 = フラッシュなし、>0 で描画側が overlay に使う強度。
 */
f32 CEffectSystem::FlashIntensity() const noexcept {
    if (m_FlashTotal <= 0.0f || m_FlashT <= 0.0f) return 0.0f;
    return m_FlashMax * (m_FlashT / m_FlashTotal);
}

} // namespace acs::game
