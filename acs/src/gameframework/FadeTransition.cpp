// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FadeTransition 実装 (Phase A polish)
#include "gameframework/FadeTransition.h"

namespace acs::game {

namespace {

// duration=0 を安全に扱うため、進捗 t を [0,1] で返す小ヘルパ。
// duration <= 0 なら「完了済」扱いで 1.0 を返す。
inline f32 SafeProgress(f32 elapsed, f32 duration) noexcept {
    if (duration <= 0.0f) return 1.0f;
    const f32 t = elapsed / duration;
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return t;
}

} // namespace

void FadeTransition::StartFade(FadeKind kind,
                                f32 out_duration,
                                f32 in_duration,
                                f32 mid_pause) noexcept {
    _kind           = kind;
    _out_duration   = out_duration > 0.0f ? out_duration : 0.0f;
    _in_duration    = in_duration  > 0.0f ? in_duration  : 0.0f;
    _mid_pause      = mid_pause    > 0.0f ? mid_pause    : 0.0f;
    _elapsed        = 0.0f;
    _mid_pause_consumed = false;

    switch (kind) {
        case FadeKind::None:
            _phase = FadePhase::Idle;
            _alpha = 0.0f;
            break;
        case FadeKind::FadeIn:
            // 画面は最初「黒幕全開」、そこから明けていく → FadingIn から開始
            _phase = FadePhase::FadingIn;
            _alpha = 1.0f;
            break;
        case FadeKind::FadeOut:
            // 0 → 1 で閉じる → FadingOut から開始
            _phase = FadePhase::FadingOut;
            _alpha = 0.0f;
            break;
        case FadeKind::FadeInOut:
            // 0 → 1 → pause → 0
            _phase = FadePhase::FadingOut;
            _alpha = 0.0f;
            break;
        case FadeKind::CrossFade:
            // 0 → peak → 0 (mid_pause は無視)
            _phase = FadePhase::FadingOut;
            _alpha = 0.0f;
            _mid_pause = 0.0f;
            break;
    }
}

void FadeTransition::Cancel() noexcept {
    _phase              = FadePhase::Idle;
    _kind               = FadeKind::None;
    _alpha              = 0.0f;
    _elapsed            = 0.0f;
    _mid_pause_consumed = false;
}

void FadeTransition::Tick(f32 dt) noexcept {
    if (_phase == FadePhase::Idle) return;
    if (dt < 0.0f) dt = 0.0f;

    _elapsed += dt;

    switch (_phase) {
        case FadePhase::FadingOut: {
            const f32 t = SafeProgress(_elapsed, _out_duration);
            // CrossFade は peak が 0.5、その他は 1.0 まで上げる
            const f32 peak = (_kind == FadeKind::CrossFade) ? kCrossFadePeak : 1.0f;
            _alpha = t * peak;
            if (t >= 1.0f) {
                _alpha = peak;
                if (_kind == FadeKind::FadeOut) {
                    // 暗黒で停止 (ユーザーが Cancel するか、別 fade を Start するまで)
                    // → Idle にはせず、MidPause も持たない。ここで安定状態に入る。
                    // ただし「FadeOut 完了」も IsActive() = true を保ちたいので
                    // MidPause に遷移して止める設計とする (mid_pause=∞ 扱い)。
                    _phase   = FadePhase::MidPause;
                    _elapsed = 0.0f;
                    _mid_pause_consumed = false;
                    // FadeOut の MidPause は無期限。Tick での自動進行を抑止。
                    // → mid_pause を「絶対通過しない sentinel」に設定。
                    _mid_pause = -1.0f;  // 負値 = 無期限
                } else if (_kind == FadeKind::FadeInOut) {
                    _phase   = FadePhase::MidPause;
                    _elapsed = 0.0f;
                    _mid_pause_consumed = false;
                } else if (_kind == FadeKind::CrossFade) {
                    // CrossFade は mid_pause なしで即 FadingIn へ
                    _phase   = FadePhase::FadingIn;
                    _elapsed = 0.0f;
                }
            }
            break;
        }

        case FadePhase::MidPause: {
            // mid_pause < 0 は「FadeOut 完了の無期限保持」。Cancel まで動かない。
            if (_mid_pause < 0.0f) {
                _alpha = 1.0f;
                break;
            }
            // mid_pause >= 0: 経過秒で抜ける。ただし mid_pause=0 でも
            // ユーザーが「MidPause を必ず観測できる」よう、最低 1 Tick は留まる。
            _alpha = (_kind == FadeKind::CrossFade) ? kCrossFadePeak : 1.0f;
            if (!_mid_pause_consumed) {
                // 今回 Tick で初めて MidPause に入った状態を観測した → 次から抜ける
                _mid_pause_consumed = true;
                break;
            }
            if (_elapsed >= _mid_pause) {
                _phase   = FadePhase::FadingIn;
                _elapsed = 0.0f;
            }
            break;
        }

        case FadePhase::FadingIn: {
            const f32 t = SafeProgress(_elapsed, _in_duration);
            // CrossFade は peak=0.5 から、その他は 1.0 から減衰
            const f32 peak = (_kind == FadeKind::CrossFade) ? kCrossFadePeak : 1.0f;
            _alpha = peak * (1.0f - t);
            if (t >= 1.0f) {
                _alpha = 0.0f;
                _phase = FadePhase::Idle;
                _kind  = FadeKind::None;
                _elapsed = 0.0f;
            }
            break;
        }

        case FadePhase::Idle:
        default:
            // 到達不能 (冒頭で return 済)
            break;
    }
}

} // namespace acs::game
