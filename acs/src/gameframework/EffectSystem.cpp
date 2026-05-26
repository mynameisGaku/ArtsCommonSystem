// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar I — FEffectSystem 実装
//
// 仕様の意図は EffectSystem.h を参照。本ファイルは純粋 state machine としての
// timer 更新 / clamp / 上書き規則の実装に徹する。
#include "gameframework/EffectSystem.h"

namespace acs::game {

// =============================================================================
// Flash
// -----------------------------------------------------------------------------
// 後から呼ばれた Flash で常に上書き (累積しない)。
// duration <= 0 はノーオプ (Flash を消したい場合は intensity=0 を渡せばよいが、
// 0 duration は除算を避けたいので明示的に early-return)。
// =============================================================================
void FEffectSystem::Flash(FVec3 color, f32 intensity, f32 duration) noexcept {
    if (duration <= 0.0f) {
        // すでに走っていた flash を即座に消す
        _flash_t     = 0.0f;
        _flash_total = 0.0f;
        _flash_max   = 0.0f;
        _flash_color = color;
        return;
    }
    // intensity の負値は意味不明なので 0 に clamp (上限は描画側に任せる)。
    const f32 i_safe = intensity < 0.0f ? 0.0f : intensity;
    _flash_color = color;
    _flash_max   = i_safe;
    _flash_total = duration;
    _flash_t     = duration;  // ピーク状態でスタート
}

// =============================================================================
// HitStop
// -----------------------------------------------------------------------------
// 残時間と新時間の max を採用 (= 強い hit stop が後発の弱い hit stop で
// 打ち消されない)。負値はノーオプ。
// =============================================================================
void FEffectSystem::HitStop(f32 duration) noexcept {
    if (duration <= 0.0f) return;
    if (duration > _hit_stop_remain) {
        _hit_stop_remain = duration;
    }
}

// =============================================================================
// TriggerShake
// -----------------------------------------------------------------------------
// 同一フレーム中の複数呼出は max を保つ (= 大ダメージ hit が小ヒットで
// 上書きされない)。FCamera2D が次フレーム頭で ConsumeShake する想定。
// duration_hint は将来 FEffectSystem 内で trauma decay の倍率に使いたい場合の
// 予約 (現状は無視)。
// =============================================================================
void FEffectSystem::TriggerShake(f32 trauma, f32 /*duration_hint*/) noexcept {
    if (trauma <= 0.0f) return;
    // pending は max-of-frame (FCamera2D の AddShake が累積側を担う)
    if (trauma > _pending_shake) {
        _pending_shake = trauma;
    }
}

// =============================================================================
// Tick
// -----------------------------------------------------------------------------
// real-time dt で呼ばれる前提 (hit stop 中も 0 ではない)。
//   ・Flash timer: 線形減衰。0 を下回ったら停止状態 (_flash_t=0)。
//   ・HitStop timer: 線形減衰。
//   ・Pending shake: ここでは消さない (FCamera2D が ConsumeShake する責務)。
// dt が負 (= clock 巻き戻し等) のときは 0 扱いにして state を破壊しない。
// =============================================================================
void FEffectSystem::Tick(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;

    // Flash decay
    if (_flash_t > 0.0f) {
        _flash_t -= dt;
        if (_flash_t <= 0.0f) {
            _flash_t     = 0.0f;
            _flash_max   = 0.0f;
            _flash_total = 0.0f;
        }
    }

    // HitStop decay
    if (_hit_stop_remain > 0.0f) {
        _hit_stop_remain -= dt;
        if (_hit_stop_remain < 0.0f) {
            _hit_stop_remain = 0.0f;
        }
    }

    // pending shake は ConsumeShake() が来るまで保持
}

// =============================================================================
// FlashIntensity
// -----------------------------------------------------------------------------
// `_flash_max * (_flash_t / _flash_total)` で 1 → 0 の線形減衰。
// _flash_total <= 0 のときは 0 を返す (除算回避)。
// =============================================================================
f32 FEffectSystem::FlashIntensity() const noexcept {
    if (_flash_total <= 0.0f || _flash_t <= 0.0f) return 0.0f;
    return _flash_max * (_flash_t / _flash_total);
}

} // namespace acs::game
