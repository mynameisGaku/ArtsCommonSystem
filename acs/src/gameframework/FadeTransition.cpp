// SPDX-License-Identifier: Apache-2.0
// FFadeTransition 実装
#include "gameframework/FadeTransition.h"

namespace acs::game {

namespace {

/**
 * 進捗 t を [0,1] にクランプして返す小ヘルパ。
 *
 * @details duration <= 0 なら 0 除算を避けるため「完了済」扱いで 1.0 を返す。
 * @param elapsed 現在 phase 内での経過秒。
 * @param duration phase の総秒数。
 * @return [0, 1] にクランプした進捗。
 */
inline f32 SafeProgress(f32 elapsed, f32 duration) noexcept {
    if (duration <= 0.0f) return 1.0f;
    const f32 t = elapsed / duration;
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return t;
}

} // namespace

void FFadeTransition::StartFade(EFadeKind kind,
                                f32 out_duration,
                                f32 in_duration,
                                f32 mid_pause) noexcept {
    m_Kind           = kind;
    m_OutDuration   = out_duration > 0.0f ? out_duration : 0.0f;
    m_InDuration    = in_duration  > 0.0f ? in_duration  : 0.0f;
    m_MidPause      = mid_pause    > 0.0f ? mid_pause    : 0.0f;
    m_Elapsed        = 0.0f;
    m_MidPauseConsumed = false;

    switch (kind) {
        case EFadeKind::None:
            m_Phase = EFadePhase::Idle;
            m_Alpha = 0.0f;
            break;
        case EFadeKind::FadeIn:
            // 画面は最初「黒幕全開」、そこから明けていく → FadingIn から開始
            m_Phase = EFadePhase::FadingIn;
            m_Alpha = 1.0f;
            break;
        case EFadeKind::FadeOut:
            // 0 → 1 で閉じる → FadingOut から開始
            m_Phase = EFadePhase::FadingOut;
            m_Alpha = 0.0f;
            break;
        case EFadeKind::FadeInOut:
            // 0 → 1 → pause → 0
            m_Phase = EFadePhase::FadingOut;
            m_Alpha = 0.0f;
            break;
        case EFadeKind::CrossFade:
            // 0 → peak → 0 (mid_pause は無視)
            m_Phase = EFadePhase::FadingOut;
            m_Alpha = 0.0f;
            m_MidPause = 0.0f;
            break;
    }
}

void FFadeTransition::Cancel() noexcept {
    m_Phase              = EFadePhase::Idle;
    m_Kind               = EFadeKind::None;
    m_Alpha              = 0.0f;
    m_Elapsed            = 0.0f;
    m_MidPauseConsumed = false;
}

void FFadeTransition::Tick(f32 dt) noexcept {
    if (m_Phase == EFadePhase::Idle) return;
    if (dt < 0.0f) dt = 0.0f;

    m_Elapsed += dt;

    switch (m_Phase) {
        case EFadePhase::FadingOut: {
            const f32 t = SafeProgress(m_Elapsed, m_OutDuration);
            // CrossFade は peak が 0.5、その他は 1.0 まで上げる
            const f32 peak = (m_Kind == EFadeKind::CrossFade) ? kCrossFadePeak : 1.0f;
            m_Alpha = t * peak;
            if (t >= 1.0f) {
                m_Alpha = peak;
                if (m_Kind == EFadeKind::FadeOut) {
                    // 暗黒で停止 (ユーザーが Cancel するか、別 fade を Start するまで)
                    // → Idle にはせず、MidPause も持たない。ここで安定状態に入る。
                    // ただし「FadeOut 完了」も IsActive() = true を保ちたいので
                    // MidPause に遷移して止める設計とする (mid_pause=∞ 扱い)。
                    m_Phase   = EFadePhase::MidPause;
                    m_Elapsed = 0.0f;
                    m_MidPauseConsumed = false;
                    // FadeOut の MidPause は無期限。Tick での自動進行を抑止。
                    // → mid_pause を「絶対通過しない sentinel」に設定。
                    m_MidPause = -1.0f;  // 負値 = 無期限
                } else if (m_Kind == EFadeKind::FadeInOut) {
                    m_Phase   = EFadePhase::MidPause;
                    m_Elapsed = 0.0f;
                    m_MidPauseConsumed = false;
                } else if (m_Kind == EFadeKind::CrossFade) {
                    // CrossFade は mid_pause なしで即 FadingIn へ
                    m_Phase   = EFadePhase::FadingIn;
                    m_Elapsed = 0.0f;
                }
            }
            break;
        }

        case EFadePhase::MidPause: {
            // mid_pause < 0 は「FadeOut 完了の無期限保持」。Cancel まで動かない。
            if (m_MidPause < 0.0f) {
                m_Alpha = 1.0f;
                break;
            }
            // mid_pause >= 0: 経過秒で抜ける。ただし mid_pause=0 でも
            // ユーザーが「MidPause を必ず観測できる」よう、最低 1 Tick は留まる。
            m_Alpha = (m_Kind == EFadeKind::CrossFade) ? kCrossFadePeak : 1.0f;
            if (!m_MidPauseConsumed) {
                // 今回 Tick で初めて MidPause に入った状態を観測した → 次から抜ける
                m_MidPauseConsumed = true;
                break;
            }
            if (m_Elapsed >= m_MidPause) {
                m_Phase   = EFadePhase::FadingIn;
                m_Elapsed = 0.0f;
            }
            break;
        }

        case EFadePhase::FadingIn: {
            const f32 t = SafeProgress(m_Elapsed, m_InDuration);
            // CrossFade は peak=0.5 から、その他は 1.0 から減衰
            const f32 peak = (m_Kind == EFadeKind::CrossFade) ? kCrossFadePeak : 1.0f;
            m_Alpha = peak * (1.0f - t);
            if (t >= 1.0f) {
                m_Alpha = 0.0f;
                m_Phase = EFadePhase::Idle;
                m_Kind  = EFadeKind::None;
                m_Elapsed = 0.0f;
            }
            break;
        }

        case EFadePhase::Idle:
        default:
            // 到達不能 (冒頭で return 済)
            break;
    }
}

} // namespace acs::game
