// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — FAccessibilityProfile 実装
//
// 本ファイルは状態保持と preset 適用ロジックのみを持つ。「実際に色覚補正を
// LUT で適用する」「実際に字幕を表示する」「実際にスクリーンリーダに発話
// させる」のは caller (CRenderer / UI / TTS バックエンド) の責務。
#include "gameframework/AccessibilityProfile.h"

namespace acs::game {

/** UI スナップショットで全フィールドを上書きする (POD コピー、自己代入も安全)。 */
void FAccessibilityProfile::Set(const FAccessibilitySettings& s) noexcept {
    m_Settings = s;
}

/** デフォルトコンストラクトの値に戻す (inline 初期化子と一致させるため再構築)。 */
void FAccessibilityProfile::Reset() noexcept {
    m_Settings = FAccessibilitySettings{};
}

/**
 * プリセット既定で全フィールドを上書きする。
 *
 * @details
 * まずデフォルトに戻してから preset 固有のフィールドのみを変更するため、
 * `ApplyPreset(Dyslexia) → ApplyPreset(ADHD)` は ADHD だけの状態になる
 * (旧プリセット値が残らない)。各値は preset 適用後に個別 setter で微調整できる。
 * @param p 適用するプリセット種別。
 */
void FAccessibilityProfile::ApplyPreset(EPreset p) noexcept {
    // まずデフォルトに戻して、preset 固有の差分のみ上書きする方針。
    m_Settings = FAccessibilitySettings{};

    switch (p) {
        case EPreset::Default:
            // 何もしない (Reset 済み)。
            break;

        case EPreset::Dyslexia:
            m_Settings.text_size         = ETextSize::Large;
            m_Settings.high_contrast_ui  = true;
            m_Settings.subtitles_enabled = true;
            break;

        case EPreset::ADHD:
            m_Settings.motion             = EMotionReduction::Reduced;
            m_Settings.screen_shake_scale = 0.5f;
            break;

        case EPreset::Autism:
            m_Settings.motion                = EMotionReduction::Reduced;
            m_Settings.flash_intensity_scale = 0.3f;
            m_Settings.sfx_subtitles_enabled = true;
            break;

        case EPreset::Aphasia:
            m_Settings.text_size             = ETextSize::Large;
            m_Settings.screen_reader_enabled = true;
            m_Settings.subtitles_enabled     = true;
            break;

        case EPreset::AAC:
            m_Settings.screen_reader_enabled = true;
            m_Settings.switch_device_input   = true;
            break;

        case EPreset::OneHanded:
            m_Settings.one_handed_mode = true;
            break;

        // default 句は意図的に省略:
        //   enum 拡張時のコンパイラ警告で漏れを検知する (gcc/clang
        //   -Wswitch / MSVC C4061)。万一未知の値が渡ってもデフォルト
        //   設定のままなのでクラッシュはしない (Reset 済み状態を維持)。
    }
}

} // namespace acs::game
