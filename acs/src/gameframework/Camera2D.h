// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar E — FCamera2D (Phase 9)
//
// 2D カメラ: position / zoom / rotation、target 追従 (指数 smoothing)、
// screen shake (trauma 方式)、world↔screen 座標変換、world boundary clamping。
// FSceneServices 経由で自動 tick (`ESvc::Camera2D` を WantedServices に含める)。
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

class FCamera2D {
public:
    FCamera2D() noexcept = default;
    ~FCamera2D() noexcept = default;

    FCamera2D(const FCamera2D&)            = delete;
    FCamera2D& operator=(const FCamera2D&) = delete;

    // ----- 基本 transform -----
    FVec2 Position() const noexcept { return m_Position; }
    void SetPosition(FVec2 p) noexcept { m_Position = p; }

    f32  Zoom() const noexcept { return m_Zoom; }
    void SetZoom(f32 z) noexcept { m_Zoom = z > 0.001f ? z : 0.001f; }

    f32  Rotation() const noexcept { return m_Rotation; }
    void SetRotation(f32 r) noexcept { m_Rotation = r; }

    // ----- target follow (毎フレーム値で渡す。stable ptr 不要) -----
    // smoothing: 大きいほど snappier (typical 3..10)。0 以下で即座にスナップ。
    void SetTargetPos(FVec2 target_pos, f32 smoothing = 5.0f) noexcept {
        m_TargetPos = target_pos;
        m_Smoothing  = smoothing;
        m_HasTarget = true;
    }
    void ClearTarget() noexcept { m_HasTarget = false; }
    bool HasTarget() const noexcept { return m_HasTarget; }

    // ----- screen shake (Eiserloh trauma 方式) -----
    void AddShake(f32 amount) noexcept {
        m_Trauma += amount;
        if (m_Trauma > 1.0f) m_Trauma = 1.0f;
        if (m_Trauma < 0.0f) m_Trauma = 0.0f;
    }
    f32  TraumaLevel() const noexcept { return m_Trauma; }
    void SetShakeAmplitude(f32 a) noexcept { m_ShakeAmplitude = a; }
    void SetShakeDecayRate(f32 r) noexcept { m_ShakeDecay = r; }

    // shake オフセット込みの実 view center
    FVec2 EffectiveViewCenter() const noexcept {
        return FVec2{m_Position.x + m_ShakeOffset.x,
                     m_Position.y + m_ShakeOffset.y};
    }

    // ----- world bounds clamp -----
    void SetBounds(FVec2 min, FVec2 max) noexcept {
        m_BoundsMin = min;
        m_BoundsMax = max;
        m_HasBounds = true;
    }
    void ClearBounds() noexcept { m_HasBounds = false; }
    bool HasBounds() const noexcept { return m_HasBounds; }

    // ----- 座標変換 -----
    // 画面ピクセル → world 座標 (画面中心が view center、zoom > 1 で拡大)
    FVec2 ScreenToWorld(FVec2 screen, u32 screen_w, u32 screen_h) const noexcept {
        const f32 cx = static_cast<f32>(screen_w) * 0.5f;
        const f32 cy = static_cast<f32>(screen_h) * 0.5f;
        const f32 dx = (screen.x - cx) / m_Zoom;
        const f32 dy = (screen.y - cy) / m_Zoom;
        // rotation を考慮 (逆回転で screen→world)
        const f32 c = Cos(-m_Rotation);
        const f32 s = Sin(-m_Rotation);
        const f32 rx = dx * c - dy * s;
        const f32 ry = dx * s + dy * c;
        const FVec2 vc = EffectiveViewCenter();
        return FVec2{vc.x + rx, vc.y + ry};
    }

    FVec2 WorldToScreen(FVec2 world, u32 screen_w, u32 screen_h) const noexcept {
        const FVec2 vc = EffectiveViewCenter();
        const f32 dx = world.x - vc.x;
        const f32 dy = world.y - vc.y;
        const f32 c = Cos(m_Rotation);
        const f32 s = Sin(m_Rotation);
        const f32 rx = dx * c - dy * s;
        const f32 ry = dx * s + dy * c;
        const f32 cx = static_cast<f32>(screen_w) * 0.5f;
        const f32 cy = static_cast<f32>(screen_h) * 0.5f;
        return FVec2{cx + rx * m_Zoom, cy + ry * m_Zoom};
    }

    // ----- driver (Services が PostUpdate で自動呼出) -----
    void Tick(f32 dt) noexcept {
        if (dt < 0.0f) dt = 0.0f;
        // 1) target follow (framerate-independent exponential smoothing)
        if (m_HasTarget) {
            if (m_Smoothing <= 0.0f) {
                m_Position = m_TargetPos;
            } else {
                const f32 t = 1.0f - Exp(-m_Smoothing * dt);
                m_Position.x += (m_TargetPos.x - m_Position.x) * t;
                m_Position.y += (m_TargetPos.y - m_Position.y) * t;
            }
        }
        // 2) bounds clamp
        if (m_HasBounds) {
            if (m_Position.x < m_BoundsMin.x) m_Position.x = m_BoundsMin.x;
            if (m_Position.y < m_BoundsMin.y) m_Position.y = m_BoundsMin.y;
            if (m_Position.x > m_BoundsMax.x) m_Position.x = m_BoundsMax.x;
            if (m_Position.y > m_BoundsMax.y) m_Position.y = m_BoundsMax.y;
        }
        // 3) trauma decay + shake offset
        m_Trauma -= m_ShakeDecay * dt;
        if (m_Trauma < 0.0f) m_Trauma = 0.0f;
        m_ShakeSeed += dt * 25.0f;          // 周波数 25 (= ~4 Hz の主要振動)
        const f32 power = m_Trauma * m_Trauma;
        m_ShakeOffset.x = Sin(m_ShakeSeed)         * power * m_ShakeAmplitude;
        m_ShakeOffset.y = Cos(m_ShakeSeed * 1.3f)  * power * m_ShakeAmplitude;
    }

private:
    FVec2 m_Position    {0.0f, 0.0f};
    f32  m_Zoom         = 1.0f;
    f32  m_Rotation     = 0.0f;

    FVec2 m_TargetPos  {0.0f, 0.0f};
    f32  m_Smoothing    = 5.0f;
    bool m_HasTarget   = false;

    f32  m_Trauma           = 0.0f;
    FVec2 m_ShakeOffset    {0.0f, 0.0f};
    f32  m_ShakeSeed       = 0.0f;
    f32  m_ShakeAmplitude  = 0.5f;     // world units max @ trauma=1
    f32  m_ShakeDecay      = 1.0f;     // 1.0 → 0.0 を 1 秒で

    FVec2 m_BoundsMin      {0.0f, 0.0f};
    FVec2 m_BoundsMax      {0.0f, 0.0f};
    bool m_HasBounds       = false;
};

} // namespace acs::game
