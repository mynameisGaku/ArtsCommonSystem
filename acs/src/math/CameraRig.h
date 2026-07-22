// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Math — CameraRig (軌道カメラ + 射影/逆射影ヘルパ)
// -----------------------------------------------------------------------------
// FCamera (view/projection 行列) の «周辺» でよく要る計算をエンジンの正準実装として
// 集約する。これらは符号・ハンドネス・スクリーンY向きを取り違えやすい «逆になりがち»
// な部分なので、行列ベースで実装し world→screen と screen→ray が数学的に厳密一致する
// (= 往復テストで自己検証できる) ようにした。
//
// 規約 (editor_abi の 3D ビューポートと一致、左手系 DirectX):
//   ・orbit: target→eye 方向 dir = (cosP*sinY, sinP, cosP*cosY)、eye = target + dir*dist
//            (pitch+ で見下ろし、yaw は +Y まわり)。
//   ・screen: 左上原点・Y 下向き。NDC z は [0,1] (near=0, far=1)。
//   ・projection は左手系 (FMat4::PerspectiveFovLH / FCamera)。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "math/Math.h"
#include "math/Camera.h"
#include "math/Collision3D.h"   // FRay3

namespace acs {

/**
 * 軌道カメラの eye 位置を返す (注視点まわりを yaw/pitch/距離で回す)。
 *
 * @details
 * target→eye 方向 dir = (cosP*sinY, sinP, cosP*cosY)、eye = target + dir*dist。
 * pitch+ で見下ろし (eye が上)、yaw は +Y 軸まわり。editor_abi の Cam3DEye と同式。
 * @param target 注視点。
 * @param yaw_rad ヨー (rad、+Y 軸まわり)。
 * @param pitch_rad ピッチ (rad、+ で見下ろし)。
 * @param dist 注視点からの距離。
 * @return カメラの eye ワールド座標。
 */
inline FVec3 OrbitEye(FVec3 target, f32 yaw_rad, f32 pitch_rad, f32 dist) noexcept {
    const f32 cp = Cos(pitch_rad), sp = Sin(pitch_rad);
    const f32 cy = Cos(yaw_rad),   sy = Sin(yaw_rad);
    const FVec3 dir{ cp * sy, sp, cp * cy };   // target→eye 方向
    return FVec3{ target.x + dir.x * dist,
                  target.y + dir.y * dist,
                  target.z + dir.z * dist };
}

/**
 * 軌道カメラを組み立てて返す (OrbitEye + SetLookAt + SetPerspective)。
 *
 * @param target 注視点。
 * @param yaw_rad ヨー (rad)。
 * @param pitch_rad ピッチ (rad、+ で見下ろし)。
 * @param dist 注視点からの距離。
 * @param fov_y_rad 垂直視野角 (rad)。
 * @param aspect アスペクト比 (幅/高さ)。
 * @param near_z 近クリップ面距離。
 * @param far_z 遠クリップ面距離。
 * @return view/projection を設定済みの FCamera。
 */
inline FCamera MakeOrbitCamera(FVec3 target, f32 yaw_rad, f32 pitch_rad, f32 dist,
                               f32 fov_y_rad, f32 aspect, f32 near_z, f32 far_z) noexcept {
    FCamera cam;
    cam.SetPerspective(fov_y_rad, aspect, near_z, far_z);
    cam.SetLookAt(OrbitEye(target, yaw_rad, pitch_rad, dist), target);
    return cam;
}

/**
 * ワールド点をスクリーンピクセル座標へ射影する (左上原点・Y 下向き)。
 *
 * @details
 * clip.w <= 0 (カメラ後方) のときは false を返し out_px は書き換えない。
 * @param view_proj view × projection 行列 (FCamera::ViewProjection())。
 * @param world 射影するワールド点。
 * @param screen_w スクリーン幅 (px)。
 * @param screen_h スクリーン高さ (px)。
 * @param out_px 射影結果のスクリーン座標 (左上原点)。
 * @return カメラ前方で射影できたら true。
 */
inline bool WorldToScreen(const FMat4& view_proj, FVec3 world,
                          f32 screen_w, f32 screen_h, FVec2& out_px) noexcept {
    const FVec4 clip = Transform(FVec4{ world.x, world.y, world.z, 1.0f }, view_proj);
    if (clip.w <= 1e-4f) return false;               // カメラ後方 / 退化
    const f32 ndc_x = clip.x / clip.w;
    const f32 ndc_y = clip.y / clip.w;
    out_px = FVec2{ (ndc_x * 0.5f + 0.5f) * screen_w,
                    (1.0f - (ndc_y * 0.5f + 0.5f)) * screen_h };   // Y 反転 (NDC up → screen down)
    return true;
}

/**
 * ワールド点をスクリーンピクセル座標へ射影する (FCamera 版)。
 *
 * @param cam ビュー/プロジェクションを持つカメラ。
 * @param world 射影するワールド点。
 * @param screen_w スクリーン幅 (px)。
 * @param screen_h スクリーン高さ (px)。
 * @param out_px 射影結果のスクリーン座標 (左上原点)。
 * @return カメラ前方で射影できたら true。
 */
inline bool WorldToScreen(const FCamera& cam, FVec3 world,
                          f32 screen_w, f32 screen_h, FVec2& out_px) noexcept {
    return WorldToScreen(cam.ViewProjection(), world, screen_w, screen_h, out_px);
}

/**
 * スクリーンピクセル座標を通すワールドレイを返す (逆射影、左上原点・Y 下向き)。
 *
 * @details
 * inverse(view*proj) で NDC near (z=0) / far (z=1) を逆射影し、その 2 点を結ぶレイを作る。
 * 同じ view_proj から作る WorldToScreen と数学的に厳密一致する (往復で自己検証可能)。
 * direction は正規化済み。origin は near 平面上の点。
 * @param view_proj view × projection 行列。
 * @param px スクリーン X (左上原点)。
 * @param py スクリーン Y (左上原点)。
 * @param screen_w スクリーン幅 (px)。
 * @param screen_h スクリーン高さ (px)。
 * @return ワールド空間のピックレイ (origin=near 平面点, direction=正規化)。
 */
inline FRay3 ScreenPointToRay(const FMat4& view_proj, f32 px, f32 py,
                             f32 screen_w, f32 screen_h) noexcept {
    const FMat4 inv = Inverse(view_proj);
    const f32 ndc_x = 2.0f * px / screen_w - 1.0f;
    const f32 ndc_y = 1.0f - 2.0f * py / screen_h;   // screen down → NDC up
    // near (z=0) と far (z=1) を逆射影 (w 除算で透視を戻す)
    const FVec4 cn = Transform(FVec4{ ndc_x, ndc_y, 0.0f, 1.0f }, inv);
    const FVec4 cf = Transform(FVec4{ ndc_x, ndc_y, 1.0f, 1.0f }, inv);
    const FVec3 pn{ cn.x / cn.w, cn.y / cn.w, cn.z / cn.w };
    const FVec3 pf{ cf.x / cf.w, cf.y / cf.w, cf.z / cf.w };
    FVec3 d{ pf.x - pn.x, pf.y - pn.y, pf.z - pn.z };
    const f32 len = Sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len > 1e-8f) d = FVec3{ d.x / len, d.y / len, d.z / len };
    return FRay3{ pn, d };
}

/**
 * スクリーンピクセル座標を通すワールドレイを返す (FCamera 版)。
 *
 * @param cam ビュー/プロジェクションを持つカメラ。
 * @param px スクリーン X (左上原点)。
 * @param py スクリーン Y (左上原点)。
 * @param screen_w スクリーン幅 (px)。
 * @param screen_h スクリーン高さ (px)。
 * @return ワールド空間のピックレイ。
 */
inline FRay3 ScreenPointToRay(const FCamera& cam, f32 px, f32 py,
                             f32 screen_w, f32 screen_h) noexcept {
    return ScreenPointToRay(cam.ViewProjection(), px, py, screen_w, screen_h);
}

} // namespace acs
