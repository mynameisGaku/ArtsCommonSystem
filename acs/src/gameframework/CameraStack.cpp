// SPDX-License-Identifier: Apache-2.0
// 2D cameraのstack切替と位置・回転・zoomの補間を管理する。
#include "gameframework/CameraStack.h"

#include "foundation/Log.h"
#include "math/Math.h"

namespace acs::game {

/** 2 つの FVec2 を進捗 t で線形補間する。 */
FVec2 CCameraStack::LerpVec2(FVec2 a, FVec2 b, f32 t) noexcept {
    return FVec2{Lerp(a.x, b.x, t), Lerp(a.y, b.y, t)};
}

/** 2 つの zoom を対数空間で補間する (入力は下限 0.001 にクランプ)。 */
f32 CCameraStack::LerpZoom(f32 a, f32 b, f32 t) noexcept {
    // 光学的に自然な中点 → 対数空間で線形補間。zoom <= 0 ガード (CCamera2D 側で
    // 0.001 にクランプされているはずだが、防御的に Abs を取ってから扱う)。
    const f32 sa = a > 0.001f ? a : 0.001f;
    const f32 sb = b > 0.001f ? b : 0.001f;
    return Exp(Lerp(Log(sa), Log(sb), t));
}

/** 2 つの角度を最短角経路で補間する。 */
f32 CCameraStack::LerpAngle(f32 a, f32 b, f32 t) noexcept {
    // 差分を [-π, π] に正規化してから lerp (= 最短角経路)。
    f32 d = b - a;
    while (d >  kPi) d -= kTwoPi;
    while (d < -kPi) d += kTwoPi;
    return a + d * t;
}

/** カメラを top に push し、旧 top からの補間を開始する。 */
void CCameraStack::PushCamera(CCamera2D& cam, f32 blend_duration) noexcept {
    // Pop フェード中に Push が来た場合 (後勝ち契約): フェードアウト中の top は
    // 除去が確定済みなので、新 top の下に埋もれて永久残留する前にここで確定
    // させる (Tick の pop 完了処理と同じ後始末)。放置すると Tick は top しか
    // 進めないため除去されず、後で上層を pop した際に is_in=true へ戻されて
    // Pop 要求が静かに取り消される。
    if (!m_Entries.IsEmpty() && !m_Entries.Last().is_in) {
        m_Entries.Pop();
        if (!m_Entries.IsEmpty()) {
            FCameraEntry& new_top = m_Entries[m_Entries.Num() - 1u];
            new_top.blend_t        = 1.0f;
            new_top.blend_duration = 0.0f;
            new_top.is_in          = true;
        }
    }
    if (m_Entries.Num() >= kMaxLayers) {
        ACS_LOG_WARN("CCameraStack::PushCamera: layer cap reached (%u) — ignored",
                     kMaxLayers);
        return;
    }
    FCameraEntry e;
    e.cam            = &cam;
    e.blend_duration = blend_duration > 0.0f ? blend_duration : 0.0f;
    // blend_duration <= 0 → 即時 active (blend_t=1)。それ以外は 0 から開始。
    e.blend_t        = e.blend_duration > 0.0f ? 0.0f : 1.0f;
    e.is_in          = true;
    m_Entries.Add(e);
}

/** top をフェードアウト状態に切り替える (実際の除去は Tick で行う)。 */
void CCameraStack::PopCamera(f32 blend_duration) noexcept {
    if (m_Entries.Num() <= 1) {
        ACS_LOG_WARN("CCameraStack::PopCamera on stack of size %u (need >=2) — ignored",
                     static_cast<u32>(m_Entries.Num()));
        return;
    }
    FCameraEntry& top = m_Entries[m_Entries.Num() - 1u];
    // top を「フェードアウト」状態に切り替える。is_in=false なら blend_t は
    // 「pop 進捗」(= 完了で 1)。即時 pop の場合は次の Tick で直ちに除去される。
    top.blend_duration = blend_duration > 0.0f ? blend_duration : 0.0f;
    top.blend_t        = top.blend_duration > 0.0f ? 0.0f : 1.0f;
    top.is_in          = false;
}

/** 現在 active なカメラ (= 最上層) を返す (空なら nullptr)。 */
CCamera2D* CCameraStack::Active() const noexcept {
    if (m_Entries.IsEmpty()) return nullptr;
    return m_Entries.Last().cam;
}

/** top が補間途中かどうかを返す。 */
bool CCameraStack::IsBlending() const noexcept {
    if (m_Entries.IsEmpty()) return false;
    const FCameraEntry& top = m_Entries.Last();
    // 「blend 中」= top が補間途中。下層が無いと (= 1 枚だけだと) blending
    // 概念がない (= 補間相手がいない)。Pop 中も top が is_in=false で残って
    // いれば blending と見做す。
    return top.blend_t < 1.0f && top.blend_duration > 0.0f
        && m_Entries.Num() >= 2u;
}

/** top の blend 進捗 [0,1] を返す (非 blend 時は 1)。 */
f32 CCameraStack::BlendProgress() const noexcept {
    if (m_Entries.IsEmpty()) return 1.0f;
    const f32 t = m_Entries.Last().blend_t;
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

/** スタックを空にする。 */
void CCameraStack::Clear() noexcept {
    m_Entries.Reset();
}

/** 描画に使う実 view center を返す (blend 中は下層との線形補間)。 */
FVec2 CCameraStack::EffectivePosition() const noexcept {
    if (m_Entries.IsEmpty()) return FVec2{0.0f, 0.0f};
    const FCameraEntry& top = m_Entries.Last();
    const FVec2 top_pos = top.cam->EffectiveViewCenter();
    // 1 枚しか無い or blend 完了 → top をそのまま。
    if (m_Entries.Num() < 2u || top.blend_t >= 1.0f || top.blend_duration <= 0.0f) {
        return top_pos;
    }
    const FCameraEntry& under = m_Entries[m_Entries.Num() - 2u];
    const FVec2 under_pos = under.cam->EffectiveViewCenter();
    // is_in=true  : under → top (進捗 t で top に近づく)
    // is_in=false : top  → under (進捗 t で under に近づく)
    return top.is_in ? LerpVec2(under_pos, top_pos, top.blend_t)
                     : LerpVec2(top_pos, under_pos, top.blend_t);
}

/** 描画に使う実 zoom を返す (blend 中は下層との対数補間)。 */
f32 CCameraStack::EffectiveZoom() const noexcept {
    if (m_Entries.IsEmpty()) return 1.0f;
    const FCameraEntry& top = m_Entries.Last();
    const f32 top_z = top.cam->Zoom();
    if (m_Entries.Num() < 2u || top.blend_t >= 1.0f || top.blend_duration <= 0.0f) {
        return top_z;
    }
    const FCameraEntry& under = m_Entries[m_Entries.Num() - 2u];
    const f32 under_z = under.cam->Zoom();
    return top.is_in ? LerpZoom(under_z, top_z, top.blend_t)
                     : LerpZoom(top_z, under_z, top.blend_t);
}

/** 描画に使う実 rotation を返す (blend 中は下層との最短角補間)。 */
f32 CCameraStack::EffectiveRotation() const noexcept {
    if (m_Entries.IsEmpty()) return 0.0f;
    const FCameraEntry& top = m_Entries.Last();
    const f32 top_r = top.cam->Rotation();
    if (m_Entries.Num() < 2u || top.blend_t >= 1.0f || top.blend_duration <= 0.0f) {
        return top_r;
    }
    const FCameraEntry& under = m_Entries[m_Entries.Num() - 2u];
    const f32 under_r = under.cam->Rotation();
    return top.is_in ? LerpAngle(under_r, top_r, top.blend_t)
                     : LerpAngle(top_r, under_r, top.blend_t);
}

/** blend timer を進め、active な 2 層を tick し、pop 完了時に top を除去する。 */
void CCameraStack::Tick(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;
    if (m_Entries.IsEmpty()) return;

    // 1) active な 2 層 (top と、blend 中なら下層) だけ CCamera2D::Tick を呼ぶ。
    //    blend 中でなければ top のみ。下層を独自に動かしたい場合は user 側で
    //    個別 Tick できるので、ここでは「描画に絡む層」のみ tick で十分。
    FCameraEntry& top = m_Entries[m_Entries.Num() - 1u];
    const bool has_under = m_Entries.Num() >= 2u;
    const bool blending  = has_under && top.blend_t < 1.0f && top.blend_duration > 0.0f;

    if (top.cam) top.cam->Tick(dt);
    if (blending) {
        FCameraEntry& under = m_Entries[m_Entries.Num() - 2u];
        if (under.cam) under.cam->Tick(dt);
    }

    // 2) blend timer 進行。duration <= 0 は即時完了なので push 時点で 1。
    if (top.blend_duration > 0.0f && top.blend_t < 1.0f) {
        top.blend_t += dt / top.blend_duration;
        if (top.blend_t > 1.0f) top.blend_t = 1.0f;
    }

    // 3) Pop 中 (is_in=false) で blend が完了したフレームで実際に top を除去。
    //    除去後の新 top は「フェードイン完了状態」(blend_t=1) に強制リセット
    //    する (= 既に十分前から下に居たので blend 不要)。
    if (!top.is_in && top.blend_t >= 1.0f) {
        m_Entries.Pop();
        if (!m_Entries.IsEmpty()) {
            FCameraEntry& new_top = m_Entries[m_Entries.Num() - 1u];
            new_top.blend_t        = 1.0f;
            new_top.blend_duration = 0.0f;
            new_top.is_in          = true;
        }
    }
}

} // namespace acs::game
