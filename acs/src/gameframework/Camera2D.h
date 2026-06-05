// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar E — FCamera2D
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
// 設計選択:
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
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Math.h"

namespace acs::game {

/**
 * 2D カメラ (position / zoom / rotation、target 追従、screen shake、座標変換、bounds clamp)。
 *
 * @details
 * target 追従は framerate independent な指数 smoothing (1 - exp(-smoothing * dt))、
 * screen shake は Eiserloh trauma 方式 (trauma² * amplitude * noise)、bounds clamp は
 * SetBounds の rect 内に position を抑える。EffectiveViewCenter() = position + shake_offset を
 * レンダラーが view 設定に使う。FSceneServices 経由で PostUpdate に自動 tick される。
 */
class FCamera2D {
public:
    /** 既定値 (原点・zoom 1・無回転・追従/bounds 無し) で構築する。 */
    FCamera2D() noexcept = default;

    /** 破棄する。 */
    ~FCamera2D() noexcept = default;

    /** コピー禁止 (Services が単一インスタンスとして tick するため)。 */
    FCamera2D(const FCamera2D&)            = delete;

    /** コピー代入も禁止。 */
    FCamera2D& operator=(const FCamera2D&) = delete;

    /**
     * カメラ位置 (= shake 前の view center) を返す。
     *
     * @return world 座標でのカメラ位置。
     */
    FVec2 Position() const noexcept { return m_Position; }

    /**
     * カメラ位置を直接設定する。
     *
     * @param p 設定する world 座標。
     */
    void SetPosition(FVec2 p) noexcept { m_Position = p; }

    /**
     * 現在の zoom 倍率を返す。
     *
     * @return zoom 倍率 (> 1 で拡大表示)。
     */
    f32  Zoom() const noexcept { return m_Zoom; }

    /**
     * zoom 倍率を設定する (下限 0.001 にクランプ)。
     *
     * @param z 設定する zoom 倍率。
     */
    void SetZoom(f32 z) noexcept { m_Zoom = z > 0.001f ? z : 0.001f; }

    /**
     * 現在の回転角を返す。
     *
     * @return 回転角 (radians、+ で CCW)。
     */
    f32  Rotation() const noexcept { return m_Rotation; }

    /**
     * 回転角を設定する。
     *
     * @param r 設定する回転角 (radians、+ で CCW)。
     */
    void SetRotation(f32 r) noexcept { m_Rotation = r; }

    /**
     * 追従ターゲット位置を設定する (毎フレーム値で渡す。stable ptr 不要)。
     *
     * @param target_pos 追従先の world 座標。
     * @param smoothing 追従の鋭さ (大きいほど snappier、typical 3..10)。0 以下で即座にスナップ。
     */
    void SetTargetPos(FVec2 target_pos, f32 smoothing = 5.0f) noexcept {
        m_TargetPos = target_pos;
        m_Smoothing  = smoothing;
        m_HasTarget = true;
    }

    /** 追従を解除する (以後 position は手動制御)。 */
    void ClearTarget() noexcept { m_HasTarget = false; }

    /**
     * 追従が有効かを返す。
     *
     * @return 追従中なら true。
     */
    bool HasTarget() const noexcept { return m_HasTarget; }

    /**
     * trauma を加算する (Eiserloh trauma 方式、[0,1] にクランプ)。
     *
     * @param amount 加算する trauma 量。
     */
    void AddShake(f32 amount) noexcept {
        m_Trauma += amount;
        if (m_Trauma > 1.0f) m_Trauma = 1.0f;
        if (m_Trauma < 0.0f) m_Trauma = 0.0f;
    }

    /**
     * 現在の trauma 値を返す。
     *
     * @return trauma 値 ([0,1])。
     */
    f32  TraumaLevel() const noexcept { return m_Trauma; }

    /**
     * shake の最大振幅を設定する。
     *
     * @param a 振幅 (trauma=1 時の world units 最大)。
     */
    void SetShakeAmplitude(f32 a) noexcept { m_ShakeAmplitude = a; }

    /**
     * shake の trauma 減衰レートを設定する。
     *
     * @param r 1 秒あたりの trauma 減衰量。
     */
    void SetShakeDecayRate(f32 r) noexcept { m_ShakeDecay = r; }

    /**
     * shake オフセット込みの実 view center を返す。
     *
     * @return position + shake_offset (レンダラーが view 設定に使う値)。
     */
    FVec2 EffectiveViewCenter() const noexcept {
        return FVec2{m_Position.x + m_ShakeOffset.x,
                     m_Position.y + m_ShakeOffset.y};
    }

    /**
     * world boundary clamp を設定する (position を rect 内に抑える)。
     *
     * @param min clamp 矩形の最小座標。
     * @param max clamp 矩形の最大座標。
     */
    void SetBounds(FVec2 min, FVec2 max) noexcept {
        m_BoundsMin = min;
        m_BoundsMax = max;
        m_HasBounds = true;
    }

    /** bounds clamp を解除する (以後は無制限)。 */
    void ClearBounds() noexcept { m_HasBounds = false; }

    /**
     * bounds clamp が有効かを返す。
     *
     * @return clamp 有効なら true。
     */
    bool HasBounds() const noexcept { return m_HasBounds; }

    /**
     * 画面ピクセル座標を world 座標へ変換する。
     *
     * @details 画面中心を view center、zoom > 1 で拡大、rotation を逆回転して合成する。
     * @param screen 画面ピクセル座標。
     * @param screen_w 画面幅 (ピクセル)。
     * @param screen_h 画面高さ (ピクセル)。
     * @return 対応する world 座標。
     */
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

    /**
     * world 座標を画面ピクセル座標へ変換する。
     *
     * @details ScreenToWorld の逆変換。view center からの差分を rotation で回し zoom を掛けて画面中心に足す。
     * @param world world 座標。
     * @param screen_w 画面幅 (ピクセル)。
     * @param screen_h 画面高さ (ピクセル)。
     * @return 対応する画面ピクセル座標。
     */
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

    /**
     * カメラを 1 フレーム進める (Services が PostUpdate で自動呼出)。
     *
     * @details
     * target follow (framerate-independent な指数 smoothing) → bounds clamp →
     * trauma 減衰 + shake offset (trauma² * amplitude * sin/cos noise) の順で更新する。
     * @param dt 経過秒 (負値は 0 にクランプ)。
     */
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
    /** カメラ位置 (= shake 前の view center)。 */
    FVec2 m_Position    {0.0f, 0.0f};

    /** zoom 倍率 (> 1 で拡大、下限 0.001)。 */
    f32  m_Zoom         = 1.0f;

    /** 回転角 (radians、+ で CCW)。 */
    f32  m_Rotation     = 0.0f;

    /** 追従ターゲットの world 座標。 */
    FVec2 m_TargetPos  {0.0f, 0.0f};

    /** 追従の鋭さ (大きいほど snappier、0 以下で即スナップ)。 */
    f32  m_Smoothing    = 5.0f;

    /** 追従が有効かどうか。 */
    bool m_HasTarget   = false;

    /** 現在の trauma 値 ([0,1])。 */
    f32  m_Trauma           = 0.0f;

    /** 今フレームの shake オフセット (EffectiveViewCenter で position に加算)。 */
    FVec2 m_ShakeOffset    {0.0f, 0.0f};

    /** shake noise の位相シード (Tick で時間進行)。 */
    f32  m_ShakeSeed       = 0.0f;

    /** shake の最大振幅 (trauma=1 時の world units 最大)。 */
    f32  m_ShakeAmplitude  = 0.5f;

    /** trauma の減衰レート (1 秒あたりの減衰量)。 */
    f32  m_ShakeDecay      = 1.0f;

    /** bounds clamp 矩形の最小座標。 */
    FVec2 m_BoundsMin      {0.0f, 0.0f};

    /** bounds clamp 矩形の最大座標。 */
    FVec2 m_BoundsMax      {0.0f, 0.0f};

    /** bounds clamp が有効かどうか。 */
    bool m_HasBounds       = false;
};

} // namespace acs::game
