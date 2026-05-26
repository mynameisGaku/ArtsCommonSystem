// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — PhotoMode 実装
//
// 本ファイルは状態遷移ロジックのみを持つ。「実際に time scale を 0 にする」
// 「実際にカメラを動かす」「実際にフィルタを LUT で適用する」のは
// caller (Game / Scene / ポストプロセスパス) の責務。
#include "gameframework/PhotoMode.h"

namespace acs::game {

// =============================================================================
// Enter — 撮影モードに入る
//   1. すでに active なら no-op (二重 Enter 防止: 保存値が壊れるため)
//   2. 現在の time_scale を保存
//   3. カメラ操作オフセットを完全リセット
//      → 前回の撮影時のパン/ズーム/回転を持ち越さない (UX 上の自然挙動)
//   4. フィルタはリセット **しない**: ユーザが「今回もセピアで撮りたい」
//      と思うのが普通なので、最後に選んだフィルタを覚えておく
//   5. capture_pending もリセットして「Enter 直後に誤撮影」を防ぐ
// =============================================================================
void PhotoMode::Enter(f32 current_time_scale) noexcept {
    if (_active) {
        return;  // 二重 Enter ガード (_saved_time_scale を上書きしない)
    }
    _active           = true;
    _saved_time_scale = current_time_scale;
    _offset           = FVec2{0.0f, 0.0f};
    _zoom_mult        = 1.0f;
    _rot              = 0.0f;
    _capture_pending  = false;
    // _filter は意図的に保持
}

// =============================================================================
// Exit — 撮影モードを抜ける
//   active=false にするだけ。time_scale 復元は caller が
//   `Game::SetTimeScale(photo.SavedTimeScale())` で行う。
//   カメラ offset / zoom / rot もそのまま保持する。次回 Enter() 時に
//   リセットされるので、Exit→Enter 間に値を見たい用途を阻害しない。
// =============================================================================
void PhotoMode::Exit() noexcept {
    _active          = false;
    _capture_pending = false;
}

// =============================================================================
// MoveCamera — パン入力を累積
//   active 中のみ反映 (撮影モード外で誤って積まないようにガード)。
// =============================================================================
void PhotoMode::MoveCamera(FVec2 delta) noexcept {
    if (!_active) return;
    _offset.x += delta.x;
    _offset.y += delta.y;
}

// =============================================================================
// ZoomCamera — ズーム変化を加算 (乗算ではなく加算)
//   ・zoom_delta = +0.1 → 1.0 → 1.1 (10% ズームイン)
//   ・zoom_delta = -0.5 → 1.0 → 0.5 (50% ズームアウト)
//   ・最終値は [0.1, 10.0] に clamp:
//       - 0.1 未満は LOD / 深度バッファ精度が破綻する
//       - 10.0 超は被写界深度ボケが極端で実用性なし
//   active 外でも操作許可 (Enter() で 1.0 にリセットされるため副作用なし)。
// =============================================================================
void PhotoMode::ZoomCamera(f32 zoom_delta) noexcept {
    if (!_active) return;
    _zoom_mult += zoom_delta;
    if (_zoom_mult < 0.1f)  _zoom_mult = 0.1f;
    if (_zoom_mult > 10.0f) _zoom_mult = 10.0f;
}

// =============================================================================
// RotateCamera — 回転を累積 (radians)
//   wrap (-π..π) はしない。caller がフレーム合成時に Sin/Cos に渡すだけ
//   なので overflow しても挙動は変わらない。
// =============================================================================
void PhotoMode::RotateCamera(f32 rad_delta) noexcept {
    if (!_active) return;
    _rot += rad_delta;
}

// =============================================================================
// ConsumeCaptureRequest — 撮影 flag を読んで同時に落とす
//   ・active 外では常に false を返す (Exit 後の取り残し flag 防止)
//   ・成功時は _capture_pending を false に rear:
//     描画ループが同一フレームで複数回呼んでも 1 枚しか撮影されない
// =============================================================================
bool PhotoMode::ConsumeCaptureRequest() noexcept {
    if (!_active) return false;
    if (!_capture_pending) return false;
    _capture_pending = false;
    return true;
}

} // namespace acs::game
