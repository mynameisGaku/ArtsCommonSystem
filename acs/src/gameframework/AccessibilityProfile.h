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
//     FAccessibilityProfile は非コピー・非ムーブ (FGame / FScene のメンバとして
//     1 インスタンスのみ存在する設計)。
//   ・**screen_shake_scale / flash_intensity_scale は [0, 1] 推奨だが clamp しない**:
//     1.0 = フル、0.0 = 完全カット。1.0 を超える値も技術的には許可するが、
//     UI 側で 0..1 に制限する想定。
//
// 非コピー・非ムーブ:
//   FAccessibilityProfile は FGame のメンバとして 1 インスタンスのみ存在する
//   想定。複製可能にすると「どの Profile が真か」の同期ずれが発生する。
//
// 範囲外:
//   ・永続化 (FSettings 経由で Save / Load する想定、本クラスは値保持のみ)
//   ・change notification (オブザーバ pattern)
//   ・地域別法令準拠 (EAA / Section 508 / 韓国 KWCAG) の自動検証
//   ・カスタムプリセット (ユーザー定義 preset の名前付き保存)
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/**
 * 色覚補正モード。
 *
 * @details
 * 実 LUT / シェーダパラメータの解決はポストプロセスパスが担当し、本 enum は
 * どの補正を適用するかの選択値だけを表す。
 */
enum class EColorblindMode : u8 {
    /** 補正なし (デフォルト)。 */
    None         = 0,

    /** 1 型 (赤色弱) 補正。 */
    Protanopia   = 1,

    /** 2 型 (緑色弱) 補正。 */
    Deuteranopia = 2,

    /** 3 型 (青色弱) 補正。 */
    Tritanopia   = 3,

    /** 全色盲 (モノクロ) 補正。 */
    Achromatopsia= 4,
};

/**
 * モーション低減モード (motion sensitivity 対応)。
 *
 * @details
 * 画面の動きの強さをどこまで抑えるかの選択値。実際の抑制はレンダラ / カメラ /
 * UI 側が本値を参照して行う。
 */
enum class EMotionReduction : u8 {
    /** 通常 (全エフェクト ON)。 */
    Full    = 0,

    /** 控えめ (大きなカメラ振動・ホワイトフラッシュを抑制)。 */
    Reduced = 1,

    /** 完全停止 (UI フェード以外の動きを全カット)。 */
    Off     = 2,
};

/**
 * 文字サイズ (UI フォントスケール)。
 *
 * @details
 * 実 px サイズへの解決は UI レイヤが担当し、本 enum はスケール段階の選択値だけを表す。
 */
enum class ETextSize : u8 {
    /** 0.75x 相当。 */
    Small      = 0,

    /** 1.0x (デフォルト)。 */
    Medium     = 1,

    /** 1.25x。 */
    Large      = 2,

    /** 1.5x。 */
    ExtraLarge = 3,
};

/**
 * アクセシビリティ設定値の POD オブジェクト。
 *
 * @details
 * 全フィールド public でデフォルトコンストラクト可能。UI から渡された
 * スナップショットを FAccessibilityProfile::Set() で一括反映する用途のコピー可能な値型。
 */
struct FAccessibilitySettings {
    /** 色覚補正モード。 */
    EColorblindMode  colorblind             = EColorblindMode::None;

    /** モーション低減モード。 */
    EMotionReduction motion                 = EMotionReduction::Full;

    /** UI 文字サイズ。 */
    ETextSize        text_size              = ETextSize::Medium;

    /** スクリーンリーダ (TTS 読み上げ) を有効にするか。 */
    bool            screen_reader_enabled  = false;

    /** 字幕 (会話・ナレーション) を表示するか。 */
    bool            subtitles_enabled      = false;

    /** 効果音字幕 (`[door creaks]` 等) を表示するか。 */
    bool            sfx_subtitles_enabled  = false;

    /** 片手操作モード (入力マッピング・UI 配置を再構成) を有効にするか。 */
    bool            one_handed_mode        = false;

    /** 高コントラスト UI を有効にするか。 */
    bool            high_contrast_ui       = false;

    /** 視線入力デバイスを有効にするか。 */
    bool            eye_tracking_input     = false;

    /** スイッチデバイス入力 (単一ボタン操作) を有効にするか。 */
    bool            switch_device_input    = false;

    /** 画面シェイク強度のスケール係数 (1.0 = フル、0.0 = 完全カット)。 */
    f32             screen_shake_scale     = 1.0f;

    /** フラッシュ強度のスケール係数 (1.0 = フル、0.0 = 完全カット)。 */
    f32             flash_intensity_scale  = 1.0f;
};

/**
 * 一括適用するアクセシビリティプリセット種別。
 *
 * @details
 * 各プリセットは ApplyPreset() で現設定を破棄したうえで、その種別に応じた
 * フィールド群を上書きする。
 */
enum class EPreset : u8 {
    /** 全デフォルト (補正なし)。 */
    Default   = 0,

    /** 識字困難 (text_size=Large, high_contrast=true, subtitles=true)。 */
    Dyslexia  = 1,

    /** 注意欠如 (motion=Reduced, screen_shake=0.5)。 */
    ADHD      = 2,

    /** 自閉スペクトラム (motion=Reduced, flash=0.3, sfx_subtitles=true)。 */
    Autism    = 3,

    /** 失語症 (text_size=Large, screen_reader=true, subtitles=true)。 */
    Aphasia   = 4,

    /** 補助代替コミュニケーション (screen_reader=true, switch_device=true)。 */
    AAC       = 5,

    /** 片手操作 (one_handed_mode=true)。 */
    OneHanded = 6,
};

/**
 * アクセシビリティ設定の状態 holder。
 *
 * @details
 * 色覚補正・モーション低減・字幕・スクリーンリーダ・片手操作などの設定値を 1 つ保持する。
 * 本クラスは値を持つだけで、実際の機能適用 (色覚 LUT 差し替え・字幕表示・TTS 発話) は
 * caller (FRenderer / UI / TTS バックエンド) の責務。FGame のメンバとして 1 インスタンスのみ
 * 存在する設計のため非コピー・非ムーブ。
 */
class FAccessibilityProfile {
public:
    /** 全フィールドをデフォルト値で構築する。 */
    FAccessibilityProfile()  noexcept = default;

    /** 破棄する (保持するのは POD 設定のみで後始末は不要)。 */
    ~FAccessibilityProfile() noexcept = default;

    /** コピー禁止 (state holder の唯一性を担保するため)。 */
    FAccessibilityProfile(const FAccessibilityProfile&)            = delete;

    /** コピー代入も禁止。 */
    FAccessibilityProfile& operator=(const FAccessibilityProfile&) = delete;

    /** ムーブ禁止 (state holder の唯一性を担保するため)。 */
    FAccessibilityProfile(FAccessibilityProfile&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FAccessibilityProfile& operator=(FAccessibilityProfile&&)      = delete;

    /**
     * UI から受け取ったスナップショットで全フィールドを上書きする。
     *
     * @param s 反映する設定値スナップショット。
     */
    void Set(const FAccessibilitySettings& s) noexcept;

    /**
     * 現在の設定値を const 参照で返す。
     *
     * @return 保持中の設定値への const 参照。
     */
    const FAccessibilitySettings& Get() const noexcept { return m_Settings; }

    /**
     * プリセットを適用する (現値を破棄して preset 既定で上書きする)。
     *
     * @details EPreset::Default を指定すると Reset() と同等になる。
     * @param p 適用するプリセット種別。
     */
    void ApplyPreset(EPreset p) noexcept;

    /** 全フィールドをコンストラクト時のデフォルト値に戻す。 */
    void Reset() noexcept;

    /**
     * 色覚補正モードを設定する。
     *
     * @param m 設定する色覚補正モード。
     */
    void SetColorblindMode (EColorblindMode m)  noexcept { m_Settings.colorblind = m; }

    /**
     * モーション低減モードを設定する。
     *
     * @param m 設定するモーション低減モード。
     */
    void SetMotionReduction(EMotionReduction m) noexcept { m_Settings.motion     = m; }

    /**
     * UI 文字サイズを設定する。
     *
     * @param s 設定する文字サイズ。
     */
    void SetTextSize       (ETextSize s)        noexcept { m_Settings.text_size  = s; }

    /**
     * スクリーンリーダの有効/無効を設定する。
     *
     * @param b true で有効。
     */
    void EnableScreenReader(bool b)            noexcept { m_Settings.screen_reader_enabled = b; }

    /**
     * 字幕の有効/無効を設定する。
     *
     * @param b true で有効。
     */
    void EnableSubtitles   (bool b)            noexcept { m_Settings.subtitles_enabled     = b; }

    /**
     * 効果音字幕の有効/無効を設定する。
     *
     * @param b true で有効。
     */
    void EnableSfxSubtitles(bool b)            noexcept { m_Settings.sfx_subtitles_enabled = b; }

    /**
     * 片手操作モードの有効/無効を設定する。
     *
     * @param b true で有効。
     */
    void EnableOneHandedMode(bool b)           noexcept { m_Settings.one_handed_mode       = b; }

    /**
     * 高コントラスト UI の有効/無効を設定する。
     *
     * @param b true で有効。
     */
    void EnableHighContrastUi(bool b)          noexcept { m_Settings.high_contrast_ui      = b; }

    /**
     * 視線入力の有効/無効を設定する。
     *
     * @param b true で有効。
     */
    void EnableEyeTrackingInput(bool b)        noexcept { m_Settings.eye_tracking_input    = b; }

    /**
     * スイッチデバイス入力の有効/無効を設定する。
     *
     * @param b true で有効。
     */
    void EnableSwitchDeviceInput(bool b)       noexcept { m_Settings.switch_device_input   = b; }

    /**
     * 画面シェイク強度のスケール係数を設定する (1.0 = フル、0.0 = 完全カット)。
     *
     * @param s 設定するスケール係数。
     */
    void SetScreenShakeScale   (f32 s)         noexcept { m_Settings.screen_shake_scale    = s; }

    /**
     * フラッシュ強度のスケール係数を設定する (1.0 = フル、0.0 = 完全カット)。
     *
     * @param s 設定するスケール係数。
     */
    void SetFlashIntensityScale(f32 s)         noexcept { m_Settings.flash_intensity_scale = s; }

    /**
     * 現在の色覚補正モードを返す。
     *
     * @return 設定済みの色覚補正モード。
     */
    EColorblindMode  GetColorblindMode () const noexcept { return m_Settings.colorblind; }

    /**
     * 現在のモーション低減モードを返す。
     *
     * @return 設定済みのモーション低減モード。
     */
    EMotionReduction GetMotionReduction() const noexcept { return m_Settings.motion;     }

    /**
     * 現在の UI 文字サイズを返す。
     *
     * @return 設定済みの文字サイズ。
     */
    ETextSize        GetTextSize       () const noexcept { return m_Settings.text_size;  }

    /**
     * スクリーンリーダが有効かを返す。
     *
     * @return 有効なら true。
     */
    bool IsScreenReaderEnabled  () const noexcept { return m_Settings.screen_reader_enabled; }

    /**
     * 字幕が有効かを返す。
     *
     * @return 有効なら true。
     */
    bool AreSubtitlesEnabled    () const noexcept { return m_Settings.subtitles_enabled;     }

    /**
     * 効果音字幕が有効かを返す。
     *
     * @return 有効なら true。
     */
    bool AreSfxSubtitlesEnabled () const noexcept { return m_Settings.sfx_subtitles_enabled; }

    /**
     * 片手操作モードが有効かを返す。
     *
     * @return 有効なら true。
     */
    bool IsOneHandedMode        () const noexcept { return m_Settings.one_handed_mode;       }

    /**
     * 高コントラスト UI が有効かを返す。
     *
     * @return 有効なら true。
     */
    bool IsHighContrastUi       () const noexcept { return m_Settings.high_contrast_ui;      }

    /**
     * 視線入力が有効かを返す。
     *
     * @return 有効なら true。
     */
    bool IsEyeTrackingInput     () const noexcept { return m_Settings.eye_tracking_input;    }

    /**
     * スイッチデバイス入力が有効かを返す。
     *
     * @return 有効なら true。
     */
    bool IsSwitchDeviceInput    () const noexcept { return m_Settings.switch_device_input;   }

    /**
     * 画面シェイク強度のスケール係数を返す。
     *
     * @return 設定済みのスケール係数。
     */
    f32  ScreenShakeScale       () const noexcept { return m_Settings.screen_shake_scale;    }

    /**
     * フラッシュ強度のスケール係数を返す。
     *
     * @return 設定済みのスケール係数。
     */
    f32  FlashIntensityScale    () const noexcept { return m_Settings.flash_intensity_scale; }

private:
    /** 保持中のアクセシビリティ設定値。 */
    FAccessibilitySettings m_Settings{};
};

} // namespace acs::game
