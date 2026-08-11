// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game::editor_core {

/**
 * エディタテーマのプリセット種別。
 *
 * @details
 * ApplyPreset に渡すと既定の色パレット / spacing / corner が `m_Colors` に書き込まれ
 * 即時 ImGui に流れる。SetCustomColors を呼ぶと自動で Custom に切り替わる。
 */
enum class EEditorThemePreset : u8 {
    /** 標準 dark grey (デフォルト)。 */
    Dark         = 0,

    /** 青みのある暗色テーマ。 */
    DarkBlue     = 1,

    /** 明るい背景 (屋外 / プロジェクタ向け)。 */
    Light        = 2,

    /** 黒 / 白 / 黄 の三色設計 (アクセシビリティ連動可能)。 */
    HighContrast = 3,

    /** 焼け紙風の暖色基調 (長時間作業向け)。 */
    Sepia        = 4,

    /** SetCustomColors() で渡された任意パレット。 */
    Custom       = 5,
};

/**
 * preset 1 個分のカラーパレット。
 *
 * @details
 * 各メンバは RGBA `[0, 1]` 範囲の `acs::FVec4`。ImGui の ImVec4 と同じ意味論
 * (= sRGB 線形値、α は 0 = 完全透明 / 1 = 不透明)。
 */
struct FEditorThemeColors {
    /** パネル背景 (ImGuiCol_WindowBg)。 */
    FVec4 window_bg{};

    /** タイトルバー (ImGuiCol_TitleBg / Active)。 */
    FVec4 title_bg{};

    /** 通常ボタン (ImGuiCol_Button)。 */
    FVec4 button_bg{};

    /** ホバー時のボタン (ImGuiCol_ButtonHovered)。 */
    FVec4 button_hover{};

    /** 押下中のボタン (ImGuiCol_ButtonActive)。 */
    FVec4 button_active{};

    /** InputText / Combo 等の枠内 (ImGuiCol_FrameBg)。 */
    FVec4 frame_bg{};

    /** 通常文字 (ImGuiCol_Text)。 */
    FVec4 text{};

    /** グレーアウト文字 (ImGuiCol_TextDisabled)。 */
    FVec4 text_disabled{};

    /** 枠線 (ImGuiCol_Border)。 */
    FVec4 border{};

    /** 区切り線 (ImGuiCol_Separator)。 */
    FVec4 separator{};

    /** 強調色。CheckMark / SliderGrab / TabActive 等に適用する ACS ブランド色相当。 */
    FVec4 accent{};

    /** 警告メッセージ用。ImGui 標準色ではなく本 theme で手動適用する。 */
    FVec4 warning{};

    /** エラーメッセージ用。ImGui 標準色ではなく本 theme で手動適用する。 */
    FVec4 error{};
};

/** `.acstheme` checked persistence の安定したエラー種別。 */
enum class EEditorThemePersistenceError : u8 {
    /** エラーなし。 */
    None = 0,
    /** 必須引数が null。 */
    NullArgument,
    /** 指定パスが上限を超過。 */
    PathTooLong,
    /** 入力全体が上限を超過。 */
    InputTooLarge,
    /** 入力に埋め込み NUL を検出。 */
    EmbeddedNul,
    /** 行数が上限を超過。 */
    TooManyLines,
    /** 1 行の長さが上限を超過。 */
    LineTooLong,
    /** ファイル識別子が不正。 */
    BadMagic,
    /** 形式バージョンが未対応。 */
    UnsupportedVersion,
    /** 構文が不正。 */
    InvalidSyntax,
    /** 未知の設定キーを検出。 */
    UnknownKey,
    /** 同じ設定キーが重複。 */
    DuplicateKey,
    /** 必須の設定キーが不足。 */
    MissingKey,
    /** 設定値の型が不一致。 */
    InvalidType,
    /** 設定値の表現が不正。 */
    InvalidValue,
    /** 設定値が許容範囲を超過。 */
    ValueOutOfRange,
    /** 必要なメモリを確保できない。 */
    AllocationFailure,
    /** 対象ファイルが存在しない。 */
    FileNotFound,
    /** 対象ファイルを開けない。 */
    FileOpenFailed,
    /** ファイルサイズを取得できない。 */
    FileSizeFailed,
    /** 読み取り中にファイルが変更された。 */
    FileChanged,
    /** ファイル読み取りに失敗。 */
    FileReadFailed,
    /** ファイル書き込みに失敗。 */
    FileWriteFailed,
    /** ファイル内容の同期に失敗。 */
    FileFlushFailed,
    /** ファイルを正常に閉じられない。 */
    FileCloseFailed,
    /** 一時ファイルからの置換に失敗。 */
    AtomicReplaceFailed,
};

/** `.acstheme` checked load/save の結果。 */
struct FEditorThemePersistenceResult {
    /** 永続化処理が返したエラー。 */
    EEditorThemePersistenceError error = EEditorThemePersistenceError::None;
    /** 構文エラーを検出した行。該当しない場合は 0。 */
    u32 line = 0u;
    /** 正常に処理できたバイト数。 */
    u64 bytes_processed = 0u;
    /** OS が返したエラーコード。該当しない場合は 0。 */
    u32 os_error = 0u;

    bool Succeeded() const noexcept {
        return error == EEditorThemePersistenceError::None;
    }
    static const char* ErrorName(EEditorThemePersistenceError error) noexcept;
};

/**
 * 全エディタ panel が共通参照する ImGui スタイル統一テーマ管理。
 *
 * @details
 * 色パレット / spacing / font scale / corner radius を 1 ヶ所で管理し、起動時に
 * Init() を呼ぶだけで全エディタが統一見た目になる。色は ACS::FVec4 で保持し、
 * ImVec4 への変換は .cpp 内のみ (ヘッダから <imgui.h> を漏らさない)。preset /
 * colors / font_scale / corner / spacing は人間可読の `.acstheme` テキストへ
 * 永続化できる。ImGui::GetStyle() への適用は global 副作用なので非コピー・非ムーブ。
 */
class CEditorTheme {
public:
    /** 空状態で構築する (ImGui への適用は Init / ApplyPreset で行う)。 */
    CEditorTheme() noexcept = default;

    /** 破棄する (特別な後始末なし)。 */
    ~CEditorTheme() noexcept = default;

    /** コピー禁止 (ImGui スタイルへの適用は global 副作用で唯一性が崩れるため)。 */
    CEditorTheme(const CEditorTheme&)            = delete;

    /** コピー代入も禁止。 */
    CEditorTheme& operator=(const CEditorTheme&) = delete;

    /** ムーブ禁止 (workspace に 1 インスタンスのみ存在する設計)。 */
    CEditorTheme(CEditorTheme&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CEditorTheme& operator=(CEditorTheme&&)      = delete;

    /**
     * default = Dark preset を ImGui::GetStyle() に流して初期化する。
     *
     * @details
     * 多重呼び出し可 (各回で全 ImGui スタイルが上書きされる)。ImGui::CreateContext
     * 後に呼ぶこと (context 未生成だと GetStyle() / GetIO() が落ちる)。
     */
    void Init() noexcept;

    /**
     * preset 既定の色 / spacing / corner を適用して即時 ImGui に流す。
     *
     * @details
     * Custom が渡された場合は `m_Colors` を保持したまま現値を再適用する
     * (= SetCustomColors で書き込み後に別 preset へ切り替えても、Custom に戻すと
     * 直前の Custom パレットが復活する)。
     * @param preset 適用するプリセット種別。
     */
    void ApplyPreset(EEditorThemePreset preset) noexcept;

    /**
     * 現在の preset 種別を返す。
     *
     * @return 現在の preset (SetCustomColors を呼ぶと Custom になる)。
     */
    EEditorThemePreset CurrentPreset() const noexcept { return m_Preset; }

    /**
     * 任意パレットを設定し、preset を Custom に切り替えて即時 ImGui に流す。
     *
     * @details
     * 各カラーは RGBA `[0, 1]` 範囲想定 (clamp しない: ImGui 側で扱える HDR を
     * 妨げないため 1.0 超の値も技術的には許可)。
     * @param colors 適用するカラーパレット。
     */
    void SetCustomColors(const FEditorThemeColors& colors) noexcept;

    /**
     * 現在のカラーパレットを返す。
     *
     * @return preset 既定 or Custom のカラーパレットへの const 参照。
     */
    const FEditorThemeColors& Colors() const noexcept { return m_Colors; }

    /**
     * global font scale を設定する (高 DPI / HighContrast 視認性向上用)。
     *
     * @details
     * 内部的に ImGuiIO::FontGlobalScale を書き換える。`<= 0` は無視 (no-op)。
     * 4.0 超は警告ログを出して 4.0 に clamp する (起動時 typo 防御)。
     * @param scale 適用する global font scale。
     */
    void SetFontScale(f32 scale) noexcept;

    /**
     * 現在の font scale を返す。
     *
     * @return 設定済みの global font scale。
     */
    f32 FontScale() const noexcept { return m_FontScale; }

    /**
     * 全 corner radius を統一する。
     *
     * @details Window / Frame / Popup / Grab / Tab / Scrollbar すべてに同 radius を流す。
     * @param radius 適用する corner radius (負値は 0 に clamp)。
     */
    void SetRoundedCorners(f32 radius) noexcept;

    /**
     * 現在の corner radius を返す。
     *
     * @return 設定済みの corner radius。
     */
    f32 RoundedCorners() const noexcept { return m_CornerRadius; }

    /**
     * ItemSpacing.y を設定する (情報密度の主軸)。
     *
     * @details ItemSpacing.x はこの値の 0.5 倍に連動する (見た目バランス用の経験則)。
     * @param item_spacing_y 適用する縦 spacing (負値は 0 に clamp)。
     */
    void SetSpacing(f32 item_spacing_y) noexcept;

    /**
     * 現在の ItemSpacing.y を返す。
     *
     * @return 設定済みの縦 spacing。
     */
    f32 Spacing() const noexcept { return m_ItemSpacingY; }

    /**
     * "Theme Settings" 独立 ImGui window を描画する。
     *
     * @details
     * 内容: preset combo + 全カラー ColorEdit4 + font_scale / corner / spacing の
     * SliderFloat + Save/Load ボタン (ファイルパスは固定で
     * `data/editor/theme.acstheme` を使う)。Editor 側の毎フレーム loop から 1 行で呼ぶ。
     */
    void DrawThemeSettingsUI() noexcept;

    /**
     * 現在の状態を `.acstheme` テキスト形式で書き出す。
     *
     * @details
     * preset / colors / font_scale / corner / spacing を保存する。失敗時は
     * ACS_LOG_WARN で記録する (戻り値 void)。file_path が nullptr の場合は no-op。
     * @param file_path 書き出し先のファイルパス (nullptr なら no-op)。
     */
    void SaveTheme(const wchar_t* file_path) noexcept;

    /** SaveTheme の checked atomic 版。 */
    FEditorThemePersistenceResult TrySaveTheme(const wchar_t* file_path) noexcept;

    /**
     * `.acstheme` テキストから読み込み、即時 ImGui に流す。
     *
     * @details
     * 失敗時 (ファイル無し / magic mismatch / 数値解析失敗) は ACS_LOG_WARN +
     * 現状維持 (旧 theme を保つ)。file_path が nullptr の場合は no-op。
     * @param file_path 読み込み元のファイルパス (nullptr なら no-op)。
     */
    void LoadTheme(const wchar_t* file_path) noexcept;

    /** LoadTheme の checked transaction 版。 */
    FEditorThemePersistenceResult TryLoadTheme(const wchar_t* file_path) noexcept;

    /**
     * 長さ付き `.acstheme` text を厳密に解析し、全検証後だけ theme を更新する。
     */
    FEditorThemePersistenceResult TryParseThemeText(
        const char* text, usize text_size) noexcept;

    /** `.acstheme` ファイル先頭の magic 文字列 (テスト / 外部ツールから参照可)。 */
    static constexpr const char* kMagic          = "ACS_THEME";

    /** `.acstheme` ファイルフォーマットの現行バージョン。 */
    static constexpr u32         kCurrentVersion = 1u;

    static constexpr usize kMaxThemeBytes = 64u * 1024u;
    static constexpr usize kMaxThemeLineBytes = 255u;
    static constexpr u32 kMaxThemeLines = 64u;
    static constexpr usize kMaxPersistencePathChars = 1023u;

private:
    /**
     * 現在の状態を ImGui::GetStyle() / ImGuiIO に流し込む。
     *
     * @details
     * ApplyPreset / SetCustomColors / Load 後に呼ばれ、`m_Colors` / `m_FontScale` /
     * `m_CornerRadius` / `m_ItemSpacingY` を反映する。ImGui context 未生成時は
     * 何もしない (defensive)。
     */
    void ApplyToImGui() noexcept;

    /**
     * preset 種別ごとの既定パレットを書き込む。
     *
     * @details Custom が渡された場合は out を上書きしない (既存 Custom 値を保持)。
     * @param preset 既定パレットを取り出す preset 種別。
     * @param out 書き込み先のカラーパレット。
     */
    static void FillPresetColors(EEditorThemePreset preset,
                                 FEditorThemeColors& out) noexcept;

    /** 現在の preset 種別 (既定 Dark)。 */
    EEditorThemePreset m_Preset          = EEditorThemePreset::Dark;

    /** 現在のカラーパレット (preset 既定 or Custom)。 */
    FEditorThemeColors  m_Colors          {};

    /** global font scale (ImGuiIO::FontGlobalScale へ流す)。 */
    f32                m_FontScale      = 1.0f;

    /** 全 UI 要素共通の corner radius。 */
    f32                m_CornerRadius   = 3.0f;

    /** ItemSpacing.y (情報密度の主軸)。 */
    f32                m_ItemSpacingY  = 4.0f;
};

using FEditorTheme = CEditorTheme;

} // namespace acs::game::editor_core
