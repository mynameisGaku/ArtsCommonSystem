// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — FEditorCamera 実装 (Phase 21a editor_core 共通基盤)
//
// 仕様の意図は FEditorCamera.h を参照。本ファイルでは:
//   ・2D / 3D の Pan / Orbit / Dolly の数式実装
//   ・spherical 座標 (yaw, pitch, distance) から eye 算出
//   ・Reset / FrameToBoundingSphere / FrameToBoundingBox2D の自動配置
//   ・Tick での FCamera2D 互換 smoothing 補間
// を実装する。すべて noexcept、STL 不使用、内部 state は POD のみ。

#include "gameframework/tools/editor_core/EditorCamera.h"

#include "math/Math.h"   // Sin/Cos/Sqrt/Exp/Lerp/Clamp 系

namespace acs::game::editor_core {

// =============================================================================
// ローカルヘルパ
// =============================================================================
namespace {

// f32 を [lo, hi] にクランプ。Math.h に Clamp が無いためここで定義。
inline f32 ClampF(f32 v, f32 lo, f32 hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

// FVec3 同士の lerp (Math.h の Lerp(FVec3) は SIMD 経由で提供されている)。
// state field を 1 つずつ補間したいので、ここでは展開した形を使う。
inline FVec3 LerpVec3(FVec3 a, FVec3 b, f32 t) noexcept {
    return FVec3{a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t};
}

// マウス感度 (1 ピクセル → orbit / pan の換算係数)。editor 全体で統一する
// ためにここで定数化。将来 user 設定で上書きできるよう accessor を生やしても良い。
constexpr f32 kOrbitSensitivity = 0.005f;   // rad/pixel (≒ 1 ピクセル = 0.29°)
constexpr f32 kPanScale3D       = 0.0015f;  // 3D pan: world unit / (pixel * distance)
constexpr f32 kPanScale2D       = 1.0f;     // 2D pan: world unit / pixel (zoom で割る)
constexpr f32 kDollyWheelStep   = 0.1f;     // wheel 1 notch あたりの倍率変化

} // namespace

// =============================================================================
// Init / SetMode
// =============================================================================
void FEditorCamera::Init(EEditorCameraMode mode) noexcept {
    _mode      = mode;
    _smoothing = 5.0f;
    _base_ortho_size = 20.0f;
    Reset();   // mode に応じた初期 state へ
}

void FEditorCamera::SetMode(EEditorCameraMode mode) noexcept {
    if (_mode == mode) {
        return;
    }
    _mode = mode;
    // mode 切替時は state を保持 (3D <-> 2D 間で互いに独立)。Tick smoothing 用
    // の target も同期させて変な補間が走らないようにする。
    _smooth_target = _state;
}

// =============================================================================
// 入力ハンドラ
// =============================================================================
void FEditorCamera::HandleMouseInput(FVec2 mouse_delta,
                                    bool lmb, bool rmb, bool mmb,
                                    f32 wheel_delta) noexcept {
    // 規約 (ヘッダ参照):
    //   3D: LMB drag or RMB drag = orbit / MMB drag = pan / wheel = dolly
    //   2D: LMB drag or MMB drag = pan / wheel = zoom (= Dolly)
    if (_mode == EEditorCameraMode::Mode3D) {
        if (mmb) {
            // 中ボタンドラッグ = pan (Maya / Blender 風)
            Pan(mouse_delta);
        } else if (lmb || rmb) {
            // 左 / 右ボタンドラッグ = orbit (どちらでも同じ; Maya は alt 必須
            // だが editor は無修飾でも回せる方が初学者に親切)
            Orbit(mouse_delta.x * kOrbitSensitivity,
                  mouse_delta.y * kOrbitSensitivity);
        }
    } else {
        // 2D: orbit という概念が無いため、左 / 中いずれでも pan
        if (lmb || mmb) {
            Pan(mouse_delta);
        }
        (void)rmb;  // 2D では未割り当て (将来 zoom-rect 等で使用予定)
    }
    if (wheel_delta != 0.0f) {
        Dolly(wheel_delta);
    }
}

// =============================================================================
// Pan / Orbit / Dolly
// =============================================================================
void FEditorCamera::Pan(FVec2 screen_delta) noexcept {
    if (_mode == EEditorCameraMode::Mode3D) {
        // 3D pan: orbit target を camera right / up 方向に平行移動。
        // delta は distance * sensitivity でスケール (距離に応じて pan 速度が
        // 変わる = 遠くから見ているときは粗く、近づくと細かく動く)。
        const FVec3 forward = -OrbitDirection();           // target - eye 方向
        const FVec3 world_up{0.0f, 1.0f, 0.0f};
        FVec3 right = Cross(world_up, forward);
        if (LengthSq(right) < kEpsilon) {
            // ほぼ真上 / 真下を向いている場合は X 軸を fallback として使用。
            right = FVec3::UnitX();
        } else {
            right = Normalize(right);
        }
        const FVec3 up = Normalize(Cross(forward, right));
        const f32 scale = _state.distance * kPanScale3D;
        // screen X の +delta で 「右へドラッグ」 → カメラ視点は左に動く感覚に
        // するため target を -right 方向に。Y も同様 (画面上方向ドラッグで上へ)。
        const FVec3 shift = right * (-screen_delta.x * scale)
                         + up    * ( screen_delta.y * scale);
        _smooth_target.target  = _smooth_target.target  + shift;
    } else {
        // 2D pan: position の (x,y) を screen delta で平行移動。
        // zoom_2d が大きい (拡大) ほど 1 ピクセル分の world delta は小さい。
        const f32 zoom = _smooth_target.zoom_2d > 0.001f ? _smooth_target.zoom_2d : 0.001f;
        const f32 scale = kPanScale2D / zoom;
        _smooth_target.position.x -= screen_delta.x * scale;
        _smooth_target.position.y += screen_delta.y * scale;   // 画面 Y は下向き、world Y は上向き
    }
}

void FEditorCamera::Orbit(f32 yaw_delta, f32 pitch_delta) noexcept {
    if (_mode != EEditorCameraMode::Mode3D) {
        return;   // 2D は orbit を持たない
    }
    _smooth_target.yaw_rad   += yaw_delta;
    _smooth_target.pitch_rad += pitch_delta;
    // pitch を ±89° にクランプ (極点で gimbal flip しないように)
    _smooth_target.pitch_rad = ClampF(_smooth_target.pitch_rad,
                                       -kPitchLimit, kPitchLimit);
}

void FEditorCamera::Dolly(f32 delta) noexcept {
    if (_mode == EEditorCameraMode::Mode3D) {
        // 3D: distance を倍率変化 (寄り = distance 縮)
        // wheel 1 notch (delta = 1.0) で約 10% 寄る/離れる。
        const f32 factor = 1.0f - delta * kDollyWheelStep;
        f32 d = _smooth_target.distance * factor;
        if (d < 0.05f)   d = 0.05f;        // ターゲットめり込み防止
        if (d > 10000.0f) d = 10000.0f;    // 暴走防止
        _smooth_target.distance = d;
    } else {
        // 2D: zoom_2d を倍率変化 (寄り = zoom 増)
        const f32 factor = 1.0f + delta * kDollyWheelStep;
        f32 z = _smooth_target.zoom_2d * factor;
        if (z < 0.001f) z = 0.001f;
        if (z > 1000.0f) z = 1000.0f;
        _smooth_target.zoom_2d = z;
    }
}

// =============================================================================
// Reset / Frame
// =============================================================================
void FEditorCamera::Reset() noexcept {
    FEditorCameraState s{};
    if (_mode == EEditorCameraMode::Mode3D) {
        // 3D の標準: target=origin、distance=10、pitch=-30° で見下ろし、yaw=0。
        s.target    = FVec3{0.0f, 0.0f, 0.0f};
        s.distance  = 10.0f;
        s.yaw_rad   = 0.0f;
        s.pitch_rad = -30.0f * kDeg2Rad;
        s.up        = FVec3{0.0f, 1.0f, 0.0f};
        s.fov_deg   = 60.0f;
        s.zoom_2d   = 1.0f;
        // position は ComputeEye 相当の値を入れておく (debug overlay 用)
        const f32 cy = Cos(s.pitch_rad);
        const FVec3 dir{Sin(s.yaw_rad) * cy, Sin(s.pitch_rad), Cos(s.yaw_rad) * cy};
        s.position = s.target + dir * s.distance;
    } else {
        // 2D の標準: position=origin、zoom=1.0
        s.position = FVec3{0.0f, 0.0f, 0.0f};
        s.target   = FVec3{0.0f, 0.0f, 0.0f};
        s.zoom_2d  = 1.0f;
        s.fov_deg  = 60.0f;
        s.up       = FVec3{0.0f, 1.0f, 0.0f};
    }
    _state         = s;
    _smooth_target = s;
}

void FEditorCamera::FrameToBoundingSphere(FVec3 center, f32 radius) noexcept {
    if (radius < kEpsilon) radius = kEpsilon;
    if (_mode == EEditorCameraMode::Mode3D) {
        // distance = radius / sin(fov/2) で sphere がちょうど縦に収まる。
        // 1.2x の margin を載せて余白を作る。
        const f32 fov_rad  = _smooth_target.fov_deg * kDeg2Rad;
        const f32 half_fov = fov_rad * 0.5f;
        const f32 s = Sin(half_fov);
        const f32 d = (s > kEpsilon ? radius / s : radius) * 1.2f;
        _smooth_target.target   = center;
        _smooth_target.distance = d;
    } else {
        // 2D: 直径 (2*radius) が画面幅 = base_ortho_size / zoom に収まる
        const f32 diameter = radius * 2.0f * 1.2f;   // margin 1.2x
        const f32 z = diameter > kEpsilon ? _base_ortho_size / diameter : 1.0f;
        _smooth_target.position.x = center.x;
        _smooth_target.position.y = center.y;
        _smooth_target.position.z = 0.0f;
        _smooth_target.zoom_2d    = z;
    }
}

void FEditorCamera::FrameToBoundingBox2D(FVec2 min_xy, FVec2 max_xy) noexcept {
    // 中心 / 半幅・半高
    const FVec2 c{(min_xy.x + max_xy.x) * 0.5f,
                 (min_xy.y + max_xy.y) * 0.5f};
    const f32 hw = (max_xy.x - min_xy.x) * 0.5f;
    const f32 hh = (max_xy.y - min_xy.y) * 0.5f;
    const f32 r  = Sqrt(hw * hw + hh * hh);   // 外接円半径
    if (_mode == EEditorCameraMode::Mode2D) {
        // box の長辺基準で zoom を決める (縦横どちらも収まるよう max を取る)
        const f32 longer = (hw > hh ? hw : hh) * 2.0f * 1.2f;
        const f32 z = longer > kEpsilon ? _base_ortho_size / longer : 1.0f;
        _smooth_target.position.x = c.x;
        _smooth_target.position.y = c.y;
        _smooth_target.position.z = 0.0f;
        _smooth_target.zoom_2d    = z;
    } else {
        // 3D: 2D bounds を XZ 平面の外接 sphere とみなして再委譲
        FrameToBoundingSphere(FVec3{c.x, 0.0f, c.y}, r);
    }
}

// =============================================================================
// 行列
// =============================================================================
FVec3 FEditorCamera::OrbitDirection() const noexcept {
    // yaw=0, pitch=0 で +Z 方向 (LH 既定の forward 反対)。pitch 上昇で +Y へ。
    // ここで返すのは (eye - target) の正規化方向 (= "back" 方向)。
    const f32 cy = Cos(_state.pitch_rad);
    return FVec3{Sin(_state.yaw_rad) * cy,
                Sin(_state.pitch_rad),
                Cos(_state.yaw_rad) * cy};
}

FVec3 FEditorCamera::ComputeEye() const noexcept {
    return _state.target + OrbitDirection() * _state.distance;
}

FMat4 FEditorCamera::ViewMatrix() const noexcept {
    if (_mode == EEditorCameraMode::Mode3D) {
        const FVec3 eye = ComputeEye();
        return FMat4::LookAtLH(eye, _state.target, _state.up);
    } else {
        // 2D: 真上 (= Z 軸負側) から XY 平面を見下ろす。eye の Z は適当な
        // 負値で、target は eye の Z=0 プロジェクション。LookAt の up は +Y。
        const FVec3 eye    {_state.position.x, _state.position.y, -1.0f};
        const FVec3 target {_state.position.x, _state.position.y,  0.0f};
        return FMat4::LookAtLH(eye, target, FVec3{0.0f, 1.0f, 0.0f});
    }
}

FMat4 FEditorCamera::ProjectionMatrix(f32 aspect, f32 near_plane, f32 far_plane) const noexcept {
    if (aspect < kEpsilon) aspect = 1.0f;
    if (_mode == EEditorCameraMode::Mode3D) {
        const f32 fov_rad = _state.fov_deg * kDeg2Rad;
        return FMat4::PerspectiveFovLH(fov_rad, aspect, near_plane, far_plane);
    } else {
        // 2D: ortho。幅 = base_ortho_size / zoom_2d、高さ = 幅 / aspect。
        const f32 zoom = _state.zoom_2d > 0.001f ? _state.zoom_2d : 0.001f;
        const f32 width  = _base_ortho_size / zoom;
        const f32 height = width / aspect;
        return FMat4::OrthoLH(width, height, near_plane, far_plane);
    }
}

// =============================================================================
// アクセサ (smooth target も同期させて即追従)
// =============================================================================
void FEditorCamera::SetFovDeg(f32 deg) noexcept {
    // FoV を [10, 170] にクランプ (極端値は描画破綻)
    if (deg < 10.0f)  deg = 10.0f;
    if (deg > 170.0f) deg = 170.0f;
    _state.fov_deg         = deg;
    _smooth_target.fov_deg = deg;
}

void FEditorCamera::SetTarget(FVec3 target) noexcept {
    _state.target         = target;
    _smooth_target.target = target;
    if (_mode == EEditorCameraMode::Mode2D) {
        // 2D は position (x,y) を target と同期 (target は意味的に center)
        _state.position.x = target.x;
        _state.position.y = target.y;
        _smooth_target.position.x = target.x;
        _smooth_target.position.y = target.y;
    }
}

void FEditorCamera::SetPosition(FVec3 position) noexcept {
    if (_mode == EEditorCameraMode::Mode3D) {
        // 3D: position = eye として扱い、(target からの spherical) を逆算。
        // direction = position - target, distance = length, yaw/pitch = polar.
        const FVec3 dir = position - _state.target;
        const f32 d    = Length(dir);
        if (d > kEpsilon) {
            _state.distance        = d;
            _smooth_target.distance = d;
            const FVec3 nd = dir * (1.0f / d);
            // pitch = asin(y), yaw = atan2(x, z)
            f32 pitch = ASin(nd.y);
            pitch = ClampF(pitch, -kPitchLimit, kPitchLimit);
            const f32 yaw = ATan2(nd.x, nd.z);
            _state.pitch_rad         = pitch;
            _smooth_target.pitch_rad = pitch;
            _state.yaw_rad           = yaw;
            _smooth_target.yaw_rad   = yaw;
        }
        _state.position         = position;
        _smooth_target.position = position;
    } else {
        // 2D は position の (x,y) のみ意味を持つ
        _state.position         = position;
        _smooth_target.position = position;
    }
}

// =============================================================================
// Tick — FCamera2D 互換 smoothing
// =============================================================================
void FEditorCamera::Tick(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;
    // smoothing <= 0 で即座にスナップ
    if (_smoothing <= 0.0f) {
        _state = _smooth_target;
        return;
    }
    // framerate-independent exponential smoothing: t = 1 - exp(-rate * dt)
    const f32 t = 1.0f - Exp(-_smoothing * dt);
    // 主要 field を順に補間。位相に意味があるもの (yaw_rad など) も単純
    // lerp で済ます — editor の orbit では ±π の境界をまたぐドラッグは
    // 連続マウス移動で発生せず、無視可能 (将来 wrap が必要なら shortest-path
    // を考慮)。
    _state.position  = LerpVec3(_state.position,  _smooth_target.position,  t);
    _state.target    = LerpVec3(_state.target,    _smooth_target.target,    t);
    _state.up        = LerpVec3(_state.up,        _smooth_target.up,        t);
    _state.fov_deg   = Lerp   (_state.fov_deg,   _smooth_target.fov_deg,   t);
    _state.zoom_2d   = Lerp   (_state.zoom_2d,   _smooth_target.zoom_2d,   t);
    _state.yaw_rad   = Lerp   (_state.yaw_rad,   _smooth_target.yaw_rad,   t);
    _state.pitch_rad = Lerp   (_state.pitch_rad, _smooth_target.pitch_rad, t);
    _state.distance  = Lerp   (_state.distance,  _smooth_target.distance,  t);

    // 3D の場合、補間後の (yaw, pitch, distance) から position (= eye) を
    // 再計算して整合させる (state.position を debug 用に正しい eye 値に保つ)。
    if (_mode == EEditorCameraMode::Mode3D) {
        _state.position = ComputeEye();
    }
}

void FEditorCamera::SetSmoothing(f32 rate) noexcept {
    if (rate < 0.0f) rate = 0.0f;
    _smoothing = rate;
}

void FEditorCamera::SetBaseOrthoSize(f32 world_width) noexcept {
    if (world_width < 0.01f) world_width = 0.01f;
    _base_ortho_size = world_width;
}

} // namespace acs::game::editor_core
