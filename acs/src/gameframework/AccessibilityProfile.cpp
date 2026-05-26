// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — AccessibilityProfile 実装
//
// 本ファイルは状態保持と preset 適用ロジックのみを持つ。「実際に色覚補正を
// LUT で適用する」「実際に字幕を表示する」「実際にスクリーンリーダに発話
// させる」のは caller (Renderer / UI / TTS バックエンド) の責務。
#include "gameframework/AccessibilityProfile.h"

namespace acs::game {

// =============================================================================
// Set — UI スナップショットで全フィールド上書き
//   POD コピーで完結。aliasing 問題はない (同じ Profile への自己代入も安全:
//   `Set(profile.Get())` は値コピーで一時を介して書き戻すのと等価)。
// =============================================================================
void AccessibilityProfile::Set(const AccessibilitySettings& s) noexcept {
    _settings = s;
}

// =============================================================================
// Reset — デフォルトコンストラクトの値に戻す
//   AccessibilitySettings の inline 初期化子と一致させるため `= {}` で再構築。
// =============================================================================
void AccessibilityProfile::Reset() noexcept {
    _settings = AccessibilitySettings{};
}

// =============================================================================
// ApplyPreset — プリセット既定で全フィールド上書き
//
// 設計方針:
//   ・「上書き」前提なので、まず Reset() 相当でデフォルトに戻してから
//     preset 固有のフィールドのみを変更する。これにより
//     `ApplyPreset(Dyslexia) → ApplyPreset(ADHD)` が
//     ADHD だけの状態になることを保証する (旧 Dyslexia 値が残らない)。
//   ・各プリセットの妥当性は障害支援団体 (AbleGamers / SpecialEffect 等) の
//     提唱パラメータを参考にしているが、最終値はゲーム側が UI で
//     微調整可能 (preset 適用後に個別 setter で上書きする想定)。
//
// プリセット詳細:
//   Default   : 全デフォルト (Reset() のみ)
//   Dyslexia  : 識字困難サポート
//               - 文字サイズ Large (1.25x)
//               - 高コントラスト UI (背景との明度差を確保)
//               - 字幕 ON (音声情報を視覚的にも補助)
//   ADHD      : 注意欠如サポート (集中阻害要素を抑制)
//               - モーション Reduced (大きなカメラ動きを抑制)
//               - 画面シェイク 50% (注意散漫の原因を半減)
//   Autism    : 自閉スペクトラムサポート (感覚過敏対策)
//               - モーション Reduced
//               - フラッシュ 30% (光過敏発作リスク低減)
//               - SFX 字幕 ON (環境音の意味を明示)
//   Aphasia   : 失語症サポート (読み書き困難)
//               - 文字サイズ Large
//               - スクリーンリーダ ON (音声で情報補助)
//               - 字幕 ON
//   AAC       : 補助代替コミュニケーション (発話困難)
//               - スクリーンリーダ ON (TTS 経由でゲーム内発話)
//               - スイッチデバイス入力 ON (ボタン押下のみで操作)
//   OneHanded : 片手操作
//               - one_handed_mode ON (UI 配置・入力マッピング再構成)
// =============================================================================
void AccessibilityProfile::ApplyPreset(EPreset p) noexcept {
    // まずデフォルトに戻して、preset 固有の差分のみ上書きする方針。
    _settings = AccessibilitySettings{};

    switch (p) {
        case EPreset::Default:
            // 何もしない (Reset 済み)。
            break;

        case EPreset::Dyslexia:
            _settings.text_size         = ETextSize::Large;
            _settings.high_contrast_ui  = true;
            _settings.subtitles_enabled = true;
            break;

        case EPreset::ADHD:
            _settings.motion             = EMotionReduction::Reduced;
            _settings.screen_shake_scale = 0.5f;
            break;

        case EPreset::Autism:
            _settings.motion                = EMotionReduction::Reduced;
            _settings.flash_intensity_scale = 0.3f;
            _settings.sfx_subtitles_enabled = true;
            break;

        case EPreset::Aphasia:
            _settings.text_size             = ETextSize::Large;
            _settings.screen_reader_enabled = true;
            _settings.subtitles_enabled     = true;
            break;

        case EPreset::AAC:
            _settings.screen_reader_enabled = true;
            _settings.switch_device_input   = true;
            break;

        case EPreset::OneHanded:
            _settings.one_handed_mode = true;
            break;

        // default 句は意図的に省略:
        //   enum 拡張時のコンパイラ警告で漏れを検知する (gcc/clang
        //   -Wswitch / MSVC C4061)。万一未知の値が渡ってもデフォルト
        //   設定のままなのでクラッシュはしない (Reset 済み状態を維持)。
    }
}

} // namespace acs::game
