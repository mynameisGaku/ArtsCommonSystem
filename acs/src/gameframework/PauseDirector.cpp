// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — CPauseDirector 実装
//
// pause 状態は bit flag (EPauseReason) を 1 個の u32 にまとめて持つ。
// Pause/Resume は単純な OR / AND-NOT で、複合 reason (UserMenu | SystemMenu)
// にも対応する。None (= 0) は「pause 中ではない」を表す特異値で:
//   ・Pause(None)  は no-op (OR で変化なし → callback も呼ばれない)
//   ・Resume(None) も no-op (& ~0 = & ~0x0 = 全 bit 保持 → callback も呼ばれない)
//   ・IsPausedFor(None) は仕様により常に true (空集合は包含される)
//
// 遷移 callback は「実際に bit が変化した」場合のみ発火する。同じ reason の
// 重複 Pause / 立っていない reason の Resume では呼ばれない (no-op として
// 静かに無視)。これにより caller は idempotent に Pause/Resume を呼んで
// 問題ない設計になっている (e.g. 毎フレーム FocusLost を Pause しても
// callback は最初の 1 回だけ)。
#include "gameframework/PauseDirector.h"

namespace acs::game {

/**
 * b に含まれる bit を a から落とす (a & ~b) ユーティリティ。
 *
 * @param a 元の bit mask。
 * @param b 落とす bit mask。
 * @return a から b の bit を除いた mask。
 */
static constexpr u32 AndNotBits(u32 a, u32 b) noexcept {
    return a & ~b;
}

void CPauseDirector::Pause(EPauseReason reason) noexcept {
    // OR で bit を立てる。複合 (UserMenu | SystemMenu) もそのまま受理。
    // None (= 0) を渡されても OR で変化なしなので分岐不要。
    const u32 add        = static_cast<u32>(reason);
    const u32 newly_set  = AndNotBits(add, m_Mask);  // 立っていなかった bit のみ抽出
    m_Mask                = m_Mask | add;
    // callback は "新たに立った" bit のみ通知 (= 重複 Pause は no-op)。
    FireTransitions(newly_set, /*paused=*/true);
}

void CPauseDirector::Resume(EPauseReason reason) noexcept {
    // & ~reason で bit を落とす。None (= 0) は ~0 で全 bit になるが
    // m_Mask & 全 bit = m_Mask なので変化なし (no-op)。明示判定は不要。
    const u32 remove     = static_cast<u32>(reason);
    const u32 actually   = remove & m_Mask;          // 実際に落ちる bit のみ抽出
    m_Mask                = AndNotBits(m_Mask, remove);
    // callback は "実際に落ちた" bit のみ通知 (= 立っていなかった bit の
    // Resume は no-op)。
    FireTransitions(actually, /*paused=*/false);
}

bool CPauseDirector::IsPausedFor(EPauseReason reason) const noexcept {
    // 複合 reason 判定: reason の **全ての** bit が立っているかを確認する。
    // 部分一致を許すと「UserMenu | SystemMenu の片方だけで true」のような
    // 紛らわしい挙動になる。
    // None (= 0) を渡した場合は (mask & 0) == 0 となり true (空集合包含)。
    const u32 want = static_cast<u32>(reason);
    return (m_Mask & want) == want;
}

bool CPauseDirector::IsPaused() const noexcept {
    // 1 つでも reason が立っていれば pause 中。
    // None (= 0) を保持しているだけの状態は m_Mask == 0 で false。
    return m_Mask != 0u;
}

EPauseReason CPauseDirector::ActiveReasons() const noexcept {
    return static_cast<EPauseReason>(m_Mask);
}

void CPauseDirector::Clear() noexcept {
    // 立っていた全 bit について callback を発火してから mask を 0 に。
    // callback 中に Pause が再帰呼び出しされた場合は新 mask が再度立つが、
    // それは caller の意図と見なす (= 競合は caller 側で解決)。
    const u32 was   = m_Mask;
    m_Mask           = 0u;
    FireTransitions(was, /*paused=*/false);
}

f32 CPauseDirector::EffectiveTimeScale() const noexcept {
    // pause 中は 0、非 pause は m_NormalTimeScale を返す。
    // slow-motion (= 通常時 0.5x) は SetNormalTimeScale(0.5f) で表現。
    return IsPaused() ? 0.0f : m_NormalTimeScale;
}

void CPauseDirector::SetNormalTimeScale(f32 s) noexcept {
    // 負値は 0 に clamp。負の時間進行 (= 逆再生) は別概念なので
    // この API では扱わない (Cinematics / リプレイ側の責務)。
    m_NormalTimeScale = (s < 0.0f) ? 0.0f : s;
}

f32 CPauseDirector::NormalTimeScale() const noexcept {
    return m_NormalTimeScale;
}

void CPauseDirector::SetCallback(PauseEventCallback cb, void* user) noexcept {
    // 重複登録は不可 (上書き)。cb == nullptr で実質登録解除。
    // user は cb と一緒に保持しないと nullptr 化されて呼び出し時に死ぬ。
    m_Callback      = cb;
    m_CallbackUser = user;
}

void CPauseDirector::FireTransitions(u32 changed_bits, bool paused) const noexcept {
    if (m_Callback == nullptr) return;
    if (changed_bits == 0u)   return;

    // EPauseReason は 8 bit (Custom2 = 1<<7) しか定義していないが、
    // 将来拡張に備えて 32 bit 全てを舐める。0 bit は分岐で skip するので
    // コストは無視できる。
    for (u32 i = 0u; i < 32u; ++i) {
        const u32 bit = 1u << i;
        if ((changed_bits & bit) == 0u) continue;
        m_Callback(m_CallbackUser,
                  static_cast<EPauseReason>(bit),
                  paused);
    }
}

} // namespace acs::game
