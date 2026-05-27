// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — FAccessibilityProfile
//
// プレイヤーの身体的・認知的特性に合わせたゲーム体験のカスタマイズを保持する
// 「アクセシビリティ設定 holder」。色覚補正・モーション低減・字幕・スクリーン
// リーダ・片手操作・視線入力など、近代 AAA タイトル (Last of Us 2 / Forza
// Horizon 5 等) が標準装備するアクセシビリティ機能を一括管理する。
//
// 役割は **状態 holder** のみ:
//   ・色覚モード / モーション低減 / 文字サイズの enum 選択値
//   ・スクリーンリーダ / 字幕 / SFX 字幕 / 片手モード / 高コントラスト UI /
//     視線入力 / スイッチデバイス入力の各 bool flag
//   ・画面シェイク / フラッシュ強度のスケール係数 (motion sensitivity 用)
//   ・プリセット (Dyslexia / ADHD / Autism / Aphasia / AAC / OneHanded) の一括適用
//
// 設計選択:
//   ・**caller が機能を適用する**: 本クラスは設定値を保持するだけ。実際に
//     「色覚 LUT を差し替える」「字幕を表示する」「スクリーンリーダに発話させる」
//     のは FRenderer / UI / TTS バックエンドの責務。GameFramework モジュールから
//     具体的サブシステムへの依存を切る (Pillar 規約)。
//   ・**プリセットは「上書き」**: ApplyPreset() は現設定を破棄して preset の
//     値で全フィールドを再設定する。「Dyslexia の上に colorblind=Protanopia」
//     のような追加変更はプリセット適用後に個別 setter で行う想定。
//   ・**POD struct + class wrapper**: FAccessibilitySettings は値オブジェクト
//     としてコピー可能 (UI から渡されたスナップショットを Set() で受け取れる)。
//     FAccessibilityProfile は非コピー・非ムーブ (FGame / Scene のメンバとして
//     1 インスタンスのみ存在する設計)。
//   ・**screen_shake_scale / flash_intensity_scale は [0, 1] 推奨だが clamp しない**:
//     1.0 = フル、0.0 = 完全カット。1.0 を超える値も技術的には許可するが、
//     UI 側で 0..1 に制限する想定。
//
// 非コピー・非ムーブ:
//   FAccessibilityProfile は FGame のメンバとして 1 インスタンスのみ存在する
//   想定。複製可能にすると「どの Profile が真か」の同期ずれが発生する。
//
// 範囲外 (Phase 2+):
//   ・永続化 (FSettings 経由で Save / Load する想定、本クラスは値保持のみ)
//   ・change notification (オブザーバ pattern)
//   ・地域別法令準拠 (EAA / Section 508 / 韓国 KWCAG) の自動検証
//   ・カスタムプリセット (ユーザー定義 preset の名前付き保存)
#pragma once

#include "foundation/Types.h"

namespace acs::game {

// =============================================================================
// 色覚補正モード
//   None         : 補正なし (デフォルト)
//   Protanopia   : 1 型 (赤色弱) 補正
//   Deuteranopia : 2 型 (緑色弱) 補正
//   Tritanopia   : 3 型 (青色弱) 補正
//   Achromatopsia: 全色盲 (モノクロ)
// 実 LUT / シェーダパラメータの解決はポストプロセスパスが担当。
// =============================================================================
enum class EColorblindMode : u8 {
    None         = 0,
    Protanopia   = 1,
    Deuteranopia = 2,
    Tritanopia   = 3,
    Achromatopsia= 4,
};

// =============================================================================
// モーション低減モード (motion sensitivity 対応)
//   Full    : 通常 (全エフェクト ON)
//   Reduced : 控えめ (大きなカメラ振動・ホワイトフラッシュを抑制)
//   Off     : 完全停止 (UI フェード以外の動きを全カット)
// =============================================================================
enum class EMotionReduction : u8 {
    Full    = 0,
    Reduced = 1,
    Off     = 2,
};

// =============================================================================
// 文字サイズ (UI フォントスケール)
//   Small      : 0.75x 相当
//   Medium     : 1.0x  (デフォルト)
//   Large      : 1.25x
//   ExtraLarge : 1.5x
// 実 px サイズの解決は UI レイヤが担当。
// =============================================================================
enum class ETextSize : u8 {
    Small      = 0,
    Medium     = 1,
    Large      = 2,
    ExtraLarge = 3,
};

// =============================================================================
// POD 設定値オブジェクト
//   全フィールド public、デフォルトコンストラクト可能。
//   UI から渡されたスナップショットを Set() で一括反映する用途。
// =============================================================================
struct FAccessibilitySettings {
    EColorblindMode  colorblind             = EColorblindMode::None;
    EMotionReduction motion                 = EMotionReduction::Full;
    ETextSize        text_size              = ETextSize::Medium;
    bool            screen_reader_enabled  = false;
    bool            subtitles_enabled      = false;
    bool            sfx_subtitles_enabled  = false;  // `[door creaks]` 等
    bool            one_handed_mode        = false;
    bool            high_contrast_ui       = false;
    bool            eye_tracking_input     = false;
    bool            switch_device_input    = false;
    f32             screen_shake_scale     = 1.0f;   // 0.0 = 完全カット
    f32             flash_intensity_scale  = 1.0f;   // 0.0 = 完全カット
};

// =============================================================================
// プリセット種別
//   Default   : 全デフォルト (補正なし)
//   Dyslexia  : 識字困難 (text_size=Large, high_contrast=true, subtitles=true)
//   ADHD      : 注意欠如 (motion=Reduced, screen_shake=0.5)
//   Autism    : 自閉スペクトラム (motion=Reduced, flash=0.3, sfx_subtitles=true)
//   Aphasia   : 失語症 (text_size=Large, screen_reader=true, subtitles=true)
//   AAC       : 補助代替コミュニケーション (screen_reader=true, switch_device=true)
//   OneHanded : 片手操作 (one_handed_mode=true)
// =============================================================================
enum class EPreset : u8 {
    Default   = 0,
    Dyslexia  = 1,
    ADHD      = 2,
    Autism    = 3,
    Aphasia   = 4,
    AAC       = 5,
    OneHanded = 6,
};

class FAccessibilityProfile {
public:
    FAccessibilityProfile()  noexcept = default;
    ~FAccessibilityProfile() noexcept = default;

    // 非コピー・非ムーブ (state holder の唯一性を担保)
    FAccessibilityProfile(const FAccessibilityProfile&)            = delete;
    FAccessibilityProfile& operator=(const FAccessibilityProfile&) = delete;
    FAccessibilityProfile(FAccessibilityProfile&&)                 = delete;
    FAccessibilityProfile& operator=(FAccessibilityProfile&&)      = delete;

    // ----- 一括設定 -----
    // UI から受け取ったスナップショットで全フィールド上書き。
    void Set(const FAccessibilitySettings& s) noexcept;

    // 現在の設定値を const 参照で取得。
    const FAccessibilitySettings& Get() const noexcept { return _settings; }

    // ----- プリセット -----
    // プリセット適用 (現値を破棄して preset 既定で上書き)。
    // Default を指定すると Reset() と同等。
    void ApplyPreset(EPreset p) noexcept;

    // 全フィールドをコンストラクト時のデフォルトに戻す。
    void Reset() noexcept;

    // ----- 個別 setter (頻用フィールドのみ) -----
    void SetColorblindMode (EColorblindMode m)  noexcept { _settings.colorblind = m; }
    void SetMotionReduction(EMotionReduction m) noexcept { _settings.motion     = m; }
    void SetTextSize       (ETextSize s)        noexcept { _settings.text_size  = s; }
    void EnableScreenReader(bool b)            noexcept { _settings.screen_reader_enabled = b; }
    void EnableSubtitles   (bool b)            noexcept { _settings.subtitles_enabled     = b; }
    void EnableSfxSubtitles(bool b)            noexcept { _settings.sfx_subtitles_enabled = b; }
    void EnableOneHandedMode(bool b)           noexcept { _settings.one_handed_mode       = b; }
    void EnableHighContrastUi(bool b)          noexcept { _settings.high_contrast_ui      = b; }
    void EnableEyeTrackingInput(bool b)        noexcept { _settings.eye_tracking_input    = b; }
    void EnableSwitchDeviceInput(bool b)       noexcept { _settings.switch_device_input   = b; }
    void SetScreenShakeScale   (f32 s)         noexcept { _settings.screen_shake_scale    = s; }
    void SetFlashIntensityScale(f32 s)         noexcept { _settings.flash_intensity_scale = s; }

    // ----- 個別 getter (頻用フィールドのみ) -----
    EColorblindMode  GetColorblindMode () const noexcept { return _settings.colorblind; }
    EMotionReduction GetMotionReduction() const noexcept { return _settings.motion;     }
    ETextSize        GetTextSize       () const noexcept { return _settings.text_size;  }
    bool IsScreenReaderEnabled  () const noexcept { return _settings.screen_reader_enabled; }
    bool AreSubtitlesEnabled    () const noexcept { return _settings.subtitles_enabled;     }
    bool AreSfxSubtitlesEnabled () const noexcept { return _settings.sfx_subtitles_enabled; }
    bool IsOneHandedMode        () const noexcept { return _settings.one_handed_mode;       }
    bool IsHighContrastUi       () const noexcept { return _settings.high_contrast_ui;      }
    bool IsEyeTrackingInput     () const noexcept { return _settings.eye_tracking_input;    }
    bool IsSwitchDeviceInput    () const noexcept { return _settings.switch_device_input;   }
    f32  ScreenShakeScale       () const noexcept { return _settings.screen_shake_scale;    }
    f32  FlashIntensityScale    () const noexcept { return _settings.flash_intensity_scale; }

private:
    FAccessibilitySettings _settings{};
};

} // namespace acs::game
