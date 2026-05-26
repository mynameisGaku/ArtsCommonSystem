// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — DamageFeedback 実装
//
// 仕様の意図は DamageFeedback.h を参照。本ファイルは純粋 state machine としての
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
//   `_red_intensity += amount * kDamageToRed` を加算 → [0,1] clamp。
//   負ダメージは赤を減らさない (回復は別系統 → 何もしない)。
// 方向矢印:
//   source_direction が零ベクトル相当なら矢印を **更新しない** (= 既存の矢印
//   decay をそのまま継続)。これは「環境ダメージは矢印を出さない」要件。
//   非零なら正規化して _dir_vec に格納、_dir_intensity = 1.0 リセット。
//   Normalize(零) は FVec2::Zero() を返すので、LengthSq による判定で除外。
// =============================================================================
void DamageFeedback::TakeDamage(f32 amount, FVec2 source_direction) noexcept {
    if (amount <= 0.0f) {
        // 0 / 負ダメージは fail-safe で何もしない (回復扱いは別 API の責務)
        return;
    }

    // 赤エッジ加算 + [0,1] clamp
    _red_intensity += amount * kDamageToRed;
    if (_red_intensity > 1.0f) _red_intensity = 1.0f;
    if (_red_intensity < 0.0f) _red_intensity = 0.0f;

    // 累積ダメージ (将来拡張用フック)
    _recent_damage_total += amount;

    // 方向矢印: 零ベクトル判定は LengthSq で行い、Normalize の零除算を回避
    const f32 lensq = LengthSq(source_direction);
    if (lensq > kEpsilon * kEpsilon) {
        _dir_vec       = Normalize(source_direction);
        _dir_intensity = 1.0f;
    }
}

// =============================================================================
// Tick
// -----------------------------------------------------------------------------
//   ・赤エッジ: 線形 decay (kRedDecayRate * dt)。0 を下回らない。
//   ・方向矢印: 線形 decay (kDirDecayRate * dt)。0 になっても _dir_vec は
//      残す (= 次フレームで HasDirectionalIndicator() = false になるだけ)。
//   ・death cam: アクティブ中のみ progress を 0 → 1 に加算 (1 で頭打ち)。
//      ExitDeathCam が呼ばれるまで _death_cam_active は true のまま。
// dt が負 (clock 巻き戻し等) のときは 0 扱いで state を破壊しない。
// =============================================================================
void DamageFeedback::Tick(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;

    // 赤エッジ decay
    if (_red_intensity > 0.0f) {
        _red_intensity -= kRedDecayRate * dt;
        if (_red_intensity < 0.0f) _red_intensity = 0.0f;
    }

    // 方向矢印 decay
    if (_dir_intensity > 0.0f) {
        _dir_intensity -= kDirDecayRate * dt;
        if (_dir_intensity < 0.0f) _dir_intensity = 0.0f;
    }

    // death cam progress (active 中のみ、1 で頭打ち)
    if (_death_cam_active) {
        _death_cam_t += kDeathCamRate * dt;
        if (_death_cam_t > 1.0f) _death_cam_t = 1.0f;
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
void DamageFeedback::TriggerDeathCam(FVec3 killer_pos) noexcept {
    _killer_pos = killer_pos;
    if (!_death_cam_active) {
        _death_cam_active = true;
        _death_cam_t      = 0.0f;
    }
}

// =============================================================================
// ExitDeathCam
// -----------------------------------------------------------------------------
// リスポーン UI / シーン遷移開始時に caller が呼ぶ。state を全て初期に戻す
// (progress も 0)。_killer_pos は次の trigger で上書きされるので保持で問題なし。
// =============================================================================
void DamageFeedback::ExitDeathCam() noexcept {
    _death_cam_active = false;
    _death_cam_t      = 0.0f;
}

// =============================================================================
// Reset
// -----------------------------------------------------------------------------
// シーン切替 / respawn 用の一括クリア。全 state を初期値に戻す。
// =============================================================================
void DamageFeedback::Reset() noexcept {
    _red_intensity        = 0.0f;
    _dir_intensity        = 0.0f;
    _dir_vec              = FVec2{0.0f, 0.0f};
    _death_cam_active     = false;
    _death_cam_t          = 0.0f;
    _killer_pos           = FVec3{0.0f, 0.0f, 0.0f};
    _recent_damage_total  = 0.0f;
}

} // namespace acs::game
