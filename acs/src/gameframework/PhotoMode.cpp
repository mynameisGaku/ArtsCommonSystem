// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — CPhotoMode 実装
//
// 本ファイルは状態遷移ロジックのみを持つ。「実際に time scale を 0 にする」
// 「実際にカメラを動かす」「実際にフィルタを LUT で適用する」のは
// caller (CGame / Scene / ポストプロセスパス) の責務。
#include "gameframework/PhotoMode.h"

namespace acs::game {

/**
 * 撮影モードに入る。
 *
 * @details
 * 二重 Enter は保存値を壊さないため no-op。現在の time_scale を保存し、カメラオフセット・
 * ズーム・回転・撮影 flag をリセットする。フィルタは最後の選択を保持する。
 * @param current_time_scale Enter 時点の time scale (復元用に保存する)。
 */
void CPhotoMode::Enter(f32 current_time_scale) noexcept {
    if (m_Active) {
        return;  // 二重 Enter ガード (m_SavedTimeScale を上書きしない)
    }
    m_Active           = true;
    m_SavedTimeScale = current_time_scale;
    m_Offset           = FVec2{0.0f, 0.0f};
    m_ZoomMult        = 1.0f;
    m_Rot              = 0.0f;
    m_bCapturePending  = false;
    // m_Filter は意図的に保持
}

/**
 * 撮影モードを抜ける。
 *
 * @details active=false にして撮影 flag を落とすだけ。time scale 復元は caller が行う。
 * カメラ offset / zoom / rot は保持され、次回 Enter でリセットされる。
 */
void CPhotoMode::Exit() noexcept {
    m_Active          = false;
    m_bCapturePending = false;
}

/**
 * カメラのパン入力を累積する (active 中のみ反映)。
 *
 * @param delta 加算するオフセット (world units)。
 */
void CPhotoMode::MoveCamera(FVec2 delta) noexcept {
    if (!m_Active) return;
    m_Offset.x += delta.x;
    m_Offset.y += delta.y;
}

/**
 * ズーム倍率を変更する (active 中のみ反映)。
 *
 * @details
 * zoom_delta を現在倍率に加算し ([+0.1] で 1.0→1.1)、結果を [0.1, 10.0] に clamp する。
 * 0.1 未満は LOD / 深度バッファ精度が、10.0 超は被写界深度ボケが破綻するため。
 * @param zoom_delta ズーム倍率への加算量。
 */
void CPhotoMode::ZoomCamera(f32 zoom_delta) noexcept {
    if (!m_Active) return;
    m_ZoomMult += zoom_delta;
    if (m_ZoomMult < 0.1f)  m_ZoomMult = 0.1f;
    if (m_ZoomMult > 10.0f) m_ZoomMult = 10.0f;
}

/**
 * 回転オフセットを累積する (active 中のみ反映)。
 *
 * @details wrap (-π..π) はしない。caller が Sin/Cos に渡すだけなので overflow しても
 * 挙動は変わらない。
 * @param rad_delta 加算する回転量 (ラジアン)。
 */
void CPhotoMode::RotateCamera(f32 rad_delta) noexcept {
    if (!m_Active) return;
    m_Rot += rad_delta;
}

/**
 * 撮影リクエストを読み取って同時に消費する。
 *
 * @details active 外では常に false。成功時は flag を落とすため、同一フレームで複数回
 * 呼んでも 1 枚しか撮影されない。
 * @return 撮影リクエストが立っていれば true。
 */
bool CPhotoMode::ConsumeCaptureRequest() noexcept {
    if (!m_Active) return false;
    if (!m_bCapturePending) return false;
    m_bCapturePending = false;
    return true;
}

} // namespace acs::game
