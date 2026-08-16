// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Math.h"
#include "math/Camera.h"
#include "gameframework/CameraShakePresets.h"   // IShakeTarget

namespace acs::game {

/**
 * 3D カメラ (eye / 注視点、target 追従、screen shake、FCamera への書き出し)。
 *
 * @details
 * `FCamera2D` の 3D 版。math/Camera.h の `FCamera` は view/projection 行列を持つだけで、
 * 「どこから・どこを見て」を毎フレーム決める仕組みは持たない。ここがその状態を持つ。
 *
 * target 追従は framerate independent な指数 smoothing (1 - exp(-smoothing * dt))、
 * screen shake は 2D と同じ Eiserloh trauma 方式 (trauma² * amplitude * noise)。
 * `EffectiveEye()` / `EffectiveLookAt()` = 追従結果 + shake offset をレンダラーが使う。
 *
 * `IShakeTarget` を実装しているので `FCameraShakePresets::ApplyPreset` をそのまま渡せる。
 *
 * 軌道カメラ (yaw/pitch/距離) が要るだけなら math/CameraRig.h の `MakeOrbitCamera` を使う。
 * こちらは**時間で変化する状態**を持つ場合のもの。
 *
 * @code
 * FCamera3D cam;
 * cam.SetFollowOffset(FVec3{0.0f, 3.0f, -6.0f});
 * cam.SetTargetPos(player_pos);
 * cam.SnapToTarget();                       // 場面の始めは遅れなしで合わせる
 *
 * // 毎フレーム
 * cam.SetTargetPos(player_pos);
 * cam.Tick(dt);
 * cam.ApplyTo(render_camera, aspect);
 *
 * // 爆発したとき
 * FCameraShakePresets::ApplyPreset(cam, EShakePreset::ExplosionLarge);
 * @endcode
 */
class FCamera3D : public IShakeTarget {
public:
    /** 既定値 (原点の少し後ろから原点を見る、追従なし) で構築する。 */
    FCamera3D() noexcept = default;

    /** 破棄する。 */
    ~FCamera3D() noexcept override = default;

    /** コピー禁止 (FCamera2D と揃える)。 */
    FCamera3D(const FCamera3D&)            = delete;

    /** コピー代入も禁止。 */
    FCamera3D& operator=(const FCamera3D&) = delete;

    /**
     * shake 前の eye 位置を返す。
     *
     * @return world 座標での eye 位置。
     */
    FVec3 Position() const noexcept { return m_Position; }

    /**
     * eye 位置を直接設定する (追従を使わない場合)。
     *
     * @param p 設定する world 座標。
     */
    void SetPosition(FVec3 p) noexcept { m_Position = p; }

    /**
     * shake 前の注視点を返す。
     *
     * @return world 座標での注視点。
     */
    FVec3 LookAt() const noexcept { return m_LookAt; }

    /**
     * 注視点を直接設定する (追従を使わない場合)。
     *
     * @param p 設定する world 座標。
     */
    void SetLookAt(FVec3 p) noexcept { m_LookAt = p; }

    /**
     * 追従先を設定する。
     *
     * @details 追従先そのものではなく、そこから `FollowOffset()` ずらした位置を eye が狙う。
     * @param target_pos 追う対象の world 座標。
     * @param smoothing 追従の鋭さ (大きいほど snappier、typical 3..10)。0 以下で即座にスナップ。
     */
    void SetTargetPos(FVec3 target_pos, f32 smoothing = 5.0f) noexcept {
        m_TargetPos = target_pos;
        m_Smoothing = smoothing;
        m_HasTarget = true;
    }

    /** 追従を止める (現在位置に留まる)。 */
    void ClearTarget() noexcept { m_HasTarget = false; }

    /**
     * 追従先が設定されているかを返す。
     *
     * @return 追従中なら true。
     */
    bool HasTarget() const noexcept { return m_HasTarget; }

    /**
     * 追従先から見た eye の相対位置を設定する。
     *
     * @param offset X 右 / Y 上 / Z 前 のずらし量。
     */
    void SetFollowOffset(FVec3 offset) noexcept { m_FollowOffset = offset; }

    /**
     * 追従先から見た eye の相対位置を返す。
     *
     * @return 現在のずらし量。
     */
    FVec3 FollowOffset() const noexcept { return m_FollowOffset; }

    /**
     * 追従先のどこを見るかを設定する。
     *
     * @details 足元を見ると画面が下に寄るので、既定では少し上を見る。
     * @param offset 追従先から見た注視点のずらし量。
     */
    void SetLookAtOffset(FVec3 offset) noexcept { m_LookAtOffset = offset; }

    /**
     * 追従先から見た注視点の相対位置を返す。
     *
     * @return 現在のずらし量。
     */
    FVec3 LookAtOffset() const noexcept { return m_LookAtOffset; }

    /**
     * 遅れなしで追従先へ合わせる。
     *
     * @details
     * 場面の開始時や対象をワープさせた直後に呼ぶ。呼ばないと前の位置から新しい位置まで
     * カメラが延々と飛んでいく画になる。追従先が未設定なら何もしない。
     */
    void SnapToTarget() noexcept {
        if (!m_HasTarget) return;
        m_Position = FVec3{ m_TargetPos.x + m_FollowOffset.x,
                            m_TargetPos.y + m_FollowOffset.y,
                            m_TargetPos.z + m_FollowOffset.z };
        m_LookAt   = FVec3{ m_TargetPos.x + m_LookAtOffset.x,
                            m_TargetPos.y + m_LookAtOffset.y,
                            m_TargetPos.z + m_LookAtOffset.z };
        m_Placed   = true;
    }

    /**
     * trauma を加算する (0..1 にクランプ)。
     *
     * @param amount 加算する trauma。負値は無視する。
     */
    void AddShake(f32 amount) noexcept override {
        if (amount <= 0.0f) return;
        m_Trauma += amount;
        if (m_Trauma > 1.0f) m_Trauma = 1.0f;
    }

    /**
     * shake の振幅を設定する。
     *
     * @param a trauma=1 のときに動く world 距離。
     */
    void SetShakeAmplitude(f32 a) noexcept override { if (a >= 0.0f) m_ShakeAmplitude = a; }

    /**
     * shake の減衰速度を設定する。
     *
     * @param r 1 秒あたりに減る trauma 量。
     */
    void SetShakeDecayRate(f32 r) noexcept override { if (r > 0.0f) m_ShakeDecay = r; }

    /**
     * shake のノイズ周波数を設定する。
     *
     * @param f 大きいほど細かく震える。
     */
    void SetShakeFrequency(f32 f) noexcept { if (f > 0.0f) m_ShakeFrequency = f; }

    /**
     * 現在の trauma を返す。
     *
     * @return 0..1 の trauma。
     */
    f32 TraumaLevel() const noexcept { return m_Trauma; }

    /** shake を即座に止め、offset を 0 に戻す。 */
    void StopShake() noexcept {
        m_Trauma      = 0.0f;
        m_ShakeOffset = FVec3{ 0.0f, 0.0f, 0.0f };
    }

    /**
     * 垂直視野角 (度) を返す。
     *
     * @return 視野角。
     */
    f32 FovYDegrees() const noexcept { return m_FovYDegrees; }

    /**
     * 垂直視野角 (度) を設定する (0 < fov < 180 のみ受け付ける)。
     *
     * @param deg 設定する視野角。
     */
    void SetFovYDegrees(f32 deg) noexcept { if (deg > 0.0f && deg < 180.0f) m_FovYDegrees = deg; }

    /**
     * 近クリップ面を返す。
     *
     * @return 近クリップ距離。
     */
    f32 NearPlane() const noexcept { return m_NearPlane; }

    /**
     * 近クリップ面を設定する (正の値のみ受け付ける)。
     *
     * @param z 設定する近クリップ距離。
     */
    void SetNearPlane(f32 z) noexcept { if (z > 0.0f) m_NearPlane = z; }

    /**
     * 遠クリップ面を返す。
     *
     * @return 遠クリップ距離。
     */
    f32 FarPlane() const noexcept { return m_FarPlane; }

    /**
     * 遠クリップ面を設定する (正の値のみ受け付ける)。
     *
     * @param z 設定する遠クリップ距離。
     */
    void SetFarPlane(f32 z) noexcept { if (z > 0.0f) m_FarPlane = z; }

    /**
     * shake を加えた eye 位置を返す。
     *
     * @return レンダラーが使う eye 位置。
     */
    FVec3 EffectiveEye() const noexcept {
        return FVec3{ m_Position.x + m_ShakeOffset.x,
                      m_Position.y + m_ShakeOffset.y,
                      m_Position.z + m_ShakeOffset.z };
    }

    /**
     * shake を加えた注視点を返す。
     *
     * @details
     * eye と同じ量だけずらす。片方だけ揺らすと画面が回って見えるため。
     * @return レンダラーが使う注視点。
     */
    FVec3 EffectiveLookAt() const noexcept {
        return FVec3{ m_LookAt.x + m_ShakeOffset.x,
                      m_LookAt.y + m_ShakeOffset.y,
                      m_LookAt.z + m_ShakeOffset.z };
    }

    /**
     * 1 フレーム更新する。
     *
     * @details
     * target follow (framerate-independent な指数 smoothing) → trauma 減衰 + shake offset
     * (trauma² * amplitude * noise) の順で更新する。FCamera2D::Tick と同じ順序。
     * @param dt 経過秒 (負値は 0 にクランプ)。
     */
    void Tick(f32 dt) noexcept {
        if (dt < 0.0f) dt = 0.0f;

        // 1) target follow (framerate-independent exponential smoothing)
        if (m_HasTarget) {
            const FVec3 want_eye{ m_TargetPos.x + m_FollowOffset.x,
                                  m_TargetPos.y + m_FollowOffset.y,
                                  m_TargetPos.z + m_FollowOffset.z };
            const FVec3 want_at { m_TargetPos.x + m_LookAtOffset.x,
                                  m_TargetPos.y + m_LookAtOffset.y,
                                  m_TargetPos.z + m_LookAtOffset.z };

            // 初回は遅れなしで置く。原点から対象まで飛んでいく画を避ける。
            if (!m_Placed || m_Smoothing <= 0.0f) {
                m_Position = want_eye;
                m_LookAt   = want_at;
                m_Placed   = true;
            } else {
                const f32 t = 1.0f - Exp(-m_Smoothing * dt); // 今回近づく比率。
                m_Position.x = Lerp(m_Position.x, want_eye.x, t);
                m_Position.y = Lerp(m_Position.y, want_eye.y, t);
                m_Position.z = Lerp(m_Position.z, want_eye.z, t);
                m_LookAt.x   = Lerp(m_LookAt.x,   want_at.x,  t);
                m_LookAt.y   = Lerp(m_LookAt.y,   want_at.y,  t);
                m_LookAt.z   = Lerp(m_LookAt.z,   want_at.z,  t);
            }
        }

        // 2) trauma 減衰 + shake offset
        if (m_Trauma <= 0.0f) {
            m_ShakeOffset = FVec3{ 0.0f, 0.0f, 0.0f };
            return;
        }

        m_Trauma -= m_ShakeDecay * dt;
        if (m_Trauma <= 0.0f) {
            // 止まるときは必ず 0 に戻す。ずれたまま固まると画面が微妙にずれて見える。
            m_Trauma      = 0.0f;
            m_ShakeOffset = FVec3{ 0.0f, 0.0f, 0.0f };
            return;
        }

        const f32 power = m_Trauma * m_Trauma * m_ShakeAmplitude; // 強さは trauma の 2 乗。
        m_ShakeSeed += m_ShakeFrequency * dt;

        // 3 軸を割り切れない比でずらし、往復に見えないようにする。
        m_ShakeOffset.x = Sin(m_ShakeSeed)          * power;
        m_ShakeOffset.y = Sin(m_ShakeSeed * 1.37f)  * power * 0.7f;  // 縦は控えめ。
        m_ShakeOffset.z = Sin(m_ShakeSeed * 0.71f)  * power * 0.4f;  // 前後はさらに控えめ。
    }

    /**
     * レンダリング用カメラへ view/projection を書き出す。
     *
     * @details
     * eye と注視点が同じ、視野角や near/far が不正、aspect が 0 以下の場合は
     * **何も書かずに false を返す** (行列が壊れて画面が真っ暗になるのを防ぐ)。
     * @param out 書き出し先。
     * @param aspect アスペクト比 (幅/高さ)。
     * @return 書き出せたら true。
     */
    bool ApplyTo(FCamera& out, f32 aspect) const noexcept {
        if (aspect <= 0.0f) return false;
        if (m_NearPlane <= 0.0f || m_FarPlane <= m_NearPlane) return false;
        if (m_FovYDegrees <= 0.0f || m_FovYDegrees >= 180.0f) return false;

        const FVec3 eye = EffectiveEye();
        const FVec3 at  = EffectiveLookAt();
        const f32 dx = eye.x - at.x, dy = eye.y - at.y, dz = eye.z - at.z;
        if (dx * dx + dy * dy + dz * dz <= 1.0e-8f) return false; // 向きが決まらない。

        out.SetLookAt(eye, at);
        out.SetPerspective(m_FovYDegrees * kDegToRad, aspect, m_NearPlane, m_FarPlane);
        return true;
    }

private:
    /** 度をラジアンへ直す係数。 */
    static constexpr f32 kDegToRad = 3.14159265358979323846f / 180.0f;

    /** shake 前の eye 位置。 */
    FVec3 m_Position{ 0.0f, 0.0f, -5.0f };

    /** shake 前の注視点。 */
    FVec3 m_LookAt{ 0.0f, 0.0f, 0.0f };

    /** 追従先の world 座標。 */
    FVec3 m_TargetPos{ 0.0f, 0.0f, 0.0f };

    /** 追従先から見た eye のずらし量。 */
    FVec3 m_FollowOffset{ 0.0f, 3.0f, -6.0f };

    /** 追従先から見た注視点のずらし量。 */
    FVec3 m_LookAtOffset{ 0.0f, 1.0f, 0.0f };

    /** shake による現在のずれ。 */
    FVec3 m_ShakeOffset{ 0.0f, 0.0f, 0.0f };

    /** 追従の鋭さ。 */
    f32 m_Smoothing = 5.0f;

    /** 現在の trauma (0..1)。 */
    f32 m_Trauma = 0.0f;

    /** trauma=1 のときの振幅。 */
    f32 m_ShakeAmplitude = 0.5f;

    /** 1 秒あたりの trauma 減衰量。 */
    f32 m_ShakeDecay = 1.0f;

    /** shake のノイズ周波数。 */
    f32 m_ShakeFrequency = 25.0f;

    /** shake のノイズ位相。 */
    f32 m_ShakeSeed = 0.0f;

    /** 垂直視野角 (度)。 */
    f32 m_FovYDegrees = 60.0f;

    /** 近クリップ距離。 */
    f32 m_NearPlane = 0.1f;

    /** 遠クリップ距離。 */
    f32 m_FarPlane = 1000.0f;

    /** 追従先が設定されているか。 */
    bool m_HasTarget = false;

    /** 一度でも追従先へ置かれたか (初回スナップの判定)。 */
    bool m_Placed = false;
};

} // namespace acs::game
