// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar E — Camera2D (Phase 9)
//
// 2D カメラ: position / zoom / rotation、target 追従 (指数 smoothing)、
// screen shake (trauma 方式)、world↔screen 座標変換、world boundary clamping。
// SceneServices 経由で自動 tick (`ESvc::Camera2D` を WantedServices に含める)。
//
// 使い方:
//   class GameplayScene : public Scene {
//   public:
//       ESvc WantedServices() const noexcept override {
//           return ESvc::Default2D | ESvc::Camera2D;
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           // player の位置にカメラを追従
//           Services().Camera().SetTargetPos(player.Position());
//           // 攻撃ヒットで画面振動
//           if (player.JustHit()) Services().Camera().AddShake(0.5f);
//       }
//   };
//
// 設計選択 (Phase 9 = Pillar E Phase 1):
//   ・**framerate independent smoothing**: `1 - exp(-smoothing * dt)` で
//     dt 不変な指数追従。`smoothing = 5.0` で約 0.2 秒で 63% 詰める典型値。
//   ・**Eiserloh trauma shake** (Squirrel Eiserloh GDC 2016):
//     `trauma += amount` で累積 (clamped [0,1])、毎フレーム `trauma -= decay*dt`
//     で減衰。実 shake 量 = trauma² * amplitude * noise → 「弱 trauma で控えめ、
//     強 trauma で派手」が自然に出る。noise は sin/cos 直流回避。
//   ・**bounds clamp**: SetBounds(min, max) で position を rect 内に clamp。
//     未設定なら無制限。
//   ・**EffectiveViewCenter()** = position + shake_offset。レンダラーが
//     実際に view 設定に使う値。
//   ・**World↔Screen**: 画面中心 = view center、zoom > 1 で拡大表示。
//     rotation は radians、+ で CCW。
//
// 範囲外 (Phase 2+ で):
//   ・deadzone (player が deadzone 内にいる間はカメラ動かさない)
//   ・cinemachine 級の blend graph / virtual camera 切替
//   ・gameplay → cinematic switch
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Math.h"

namespace acs::game {

class Camera2D {
public:
    Camera2D() noexcept = default;
    ~Camera2D() noexcept = default;

    Camera2D(const Camera2D&)            = delete;
    Camera2D& operator=(const Camera2D&) = delete;

    // ----- 基本 transform -----
    Vec2 Position() const noexcept { return _position; }
    void SetPosition(Vec2 p) noexcept { _position = p; }

    f32  Zoom() const noexcept { return _zoom; }
    void SetZoom(f32 z) noexcept { _zoom = z > 0.001f ? z : 0.001f; }

    f32  Rotation() const noexcept { return _rotation; }
    void SetRotation(f32 r) noexcept { _rotation = r; }

    // ----- target follow (毎フレーム値で渡す。stable ptr 不要) -----
    // smoothing: 大きいほど snappier (typical 3..10)。0 以下で即座にスナップ。
    void SetTargetPos(Vec2 target_pos, f32 smoothing = 5.0f) noexcept {
        _target_pos = target_pos;
        _smoothing  = smoothing;
        _has_target = true;
    }
    void ClearTarget() noexcept { _has_target = false; }
    bool HasTarget() const noexcept { return _has_target; }

    // ----- screen shake (Eiserloh trauma 方式) -----
    void AddShake(f32 amount) noexcept {
        _trauma += amount;
        if (_trauma > 1.0f) _trauma = 1.0f;
        if (_trauma < 0.0f) _trauma = 0.0f;
    }
    f32  TraumaLevel() const noexcept { return _trauma; }
    void SetShakeAmplitude(f32 a) noexcept { _shake_amplitude = a; }
    void SetShakeDecayRate(f32 r) noexcept { _shake_decay = r; }

    // shake オフセット込みの実 view center
    Vec2 EffectiveViewCenter() const noexcept {
        return Vec2{_position.x + _shake_offset.x,
                     _position.y + _shake_offset.y};
    }

    // ----- world bounds clamp -----
    void SetBounds(Vec2 min, Vec2 max) noexcept {
        _bounds_min = min;
        _bounds_max = max;
        _has_bounds = true;
    }
    void ClearBounds() noexcept { _has_bounds = false; }
    bool HasBounds() const noexcept { return _has_bounds; }

    // ----- 座標変換 -----
    // 画面ピクセル → world 座標 (画面中心が view center、zoom > 1 で拡大)
    Vec2 ScreenToWorld(Vec2 screen, u32 screen_w, u32 screen_h) const noexcept {
        const f32 cx = static_cast<f32>(screen_w) * 0.5f;
        const f32 cy = static_cast<f32>(screen_h) * 0.5f;
        const f32 dx = (screen.x - cx) / _zoom;
        const f32 dy = (screen.y - cy) / _zoom;
        // rotation を考慮 (逆回転で screen→world)
        const f32 c = Cos(-_rotation);
        const f32 s = Sin(-_rotation);
        const f32 rx = dx * c - dy * s;
        const f32 ry = dx * s + dy * c;
        const Vec2 vc = EffectiveViewCenter();
        return Vec2{vc.x + rx, vc.y + ry};
    }

    Vec2 WorldToScreen(Vec2 world, u32 screen_w, u32 screen_h) const noexcept {
        const Vec2 vc = EffectiveViewCenter();
        const f32 dx = world.x - vc.x;
        const f32 dy = world.y - vc.y;
        const f32 c = Cos(_rotation);
        const f32 s = Sin(_rotation);
        const f32 rx = dx * c - dy * s;
        const f32 ry = dx * s + dy * c;
        const f32 cx = static_cast<f32>(screen_w) * 0.5f;
        const f32 cy = static_cast<f32>(screen_h) * 0.5f;
        return Vec2{cx + rx * _zoom, cy + ry * _zoom};
    }

    // ----- driver (Services が PostUpdate で自動呼出) -----
    void Tick(f32 dt) noexcept {
        if (dt < 0.0f) dt = 0.0f;
        // 1) target follow (framerate-independent exponential smoothing)
        if (_has_target) {
            if (_smoothing <= 0.0f) {
                _position = _target_pos;
            } else {
                const f32 t = 1.0f - Exp(-_smoothing * dt);
                _position.x += (_target_pos.x - _position.x) * t;
                _position.y += (_target_pos.y - _position.y) * t;
            }
        }
        // 2) bounds clamp
        if (_has_bounds) {
            if (_position.x < _bounds_min.x) _position.x = _bounds_min.x;
            if (_position.y < _bounds_min.y) _position.y = _bounds_min.y;
            if (_position.x > _bounds_max.x) _position.x = _bounds_max.x;
            if (_position.y > _bounds_max.y) _position.y = _bounds_max.y;
        }
        // 3) trauma decay + shake offset
        _trauma -= _shake_decay * dt;
        if (_trauma < 0.0f) _trauma = 0.0f;
        _shake_seed += dt * 25.0f;          // 周波数 25 (= ~4 Hz の主要振動)
        const f32 power = _trauma * _trauma;
        _shake_offset.x = Sin(_shake_seed)         * power * _shake_amplitude;
        _shake_offset.y = Cos(_shake_seed * 1.3f)  * power * _shake_amplitude;
    }

private:
    Vec2 _position    {0.0f, 0.0f};
    f32  _zoom         = 1.0f;
    f32  _rotation     = 0.0f;

    Vec2 _target_pos  {0.0f, 0.0f};
    f32  _smoothing    = 5.0f;
    bool _has_target   = false;

    f32  _trauma           = 0.0f;
    Vec2 _shake_offset    {0.0f, 0.0f};
    f32  _shake_seed       = 0.0f;
    f32  _shake_amplitude  = 0.5f;     // world units max @ trauma=1
    f32  _shake_decay      = 1.0f;     // 1.0 → 0.0 を 1 秒で

    Vec2 _bounds_min      {0.0f, 0.0f};
    Vec2 _bounds_max      {0.0f, 0.0f};
    bool _has_bounds       = false;
};

} // namespace acs::game
