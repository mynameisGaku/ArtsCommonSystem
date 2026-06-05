// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — editor_core / FEditorTheme  (ImGui スタイル統一テーマ)
//
// 役割:
//   ACS の全エディタ panel (FHierarchyPanel / FInspectorPanel / FEditorToolbar /
//   FParticleEditorPanel / ModelViewer / LevelEditor / AnimCurveEditor /
//   BehaviorTreeEditor 等) が共通で参照する **ImGui スタイル統一テーマ**。
//   色パレット / spacing / font scale / corner radius を 1 ヶ所で管理し、
//   起動時に `Init()` を呼ぶだけで全エディタが統一見た目になる。
//
// 使い方 (editor 起動コード):
//   FEditorTheme theme;
//   theme.Init();                              // default = Dark を ImGui に流す
//   theme.ApplyPreset(EEditorThemePreset::DarkBlue);
//   theme.SetFontScale(1.25f);                 // 高 DPI 対応
//   theme.SetRoundedCorners(4.0f);
//   // ... 毎フレーム panel.DrawUI() ...
//   theme.DrawThemeSettingsUI();               // Theme FSettings window を出す
//   // 終了時:
//   theme.SaveTheme(L"data/editor/theme.acstheme");
//
// 設計選択:
//   ・**ACS::FVec4 ベース、ImVec4 は .cpp 内変換のみ**: ヘッダから <imgui.h> を
//     漏らさないことで、本ヘッダを include しても include order が壊れない
//     (FInspectorPanel / FParticleEditorPanel と同パターン)。
//   ・**preset = 「色パレット定数 + spacing + corner」のスナップショット**:
//     ApplyPreset は内部 `m_Colors` を上書きしたあと、`ApplyToImGui()` で
//     ImGui::GetStyle() に流す。Custom はユーザが SetCustomColors した状態を
//     哨兵とする (= 既定の Custom 値は Dark と同等)。
//   ・**font scale は ImGuiIO::FontGlobalScale に流す**: フォント atlas 自体は
//     再構築せず、グローバルスケール変更で済ませる (高 DPI で軽い)。本格的な
//     atlas 再構築 (= 異なる px サイズの bake) は本クラスでは扱わない。
//   ・**SetRoundedCorners は Frame/FWindow/Popup/Grab/Tab/Scrollbar すべてに
//     同 radius を流す**: 統一感のため。違う値を当てたい派生 panel は ImGui の
//     `ImGui::PushStyleVar` で局所上書きすればよい。
//   ・**SetSpacing は ItemSpacing.y のみを操作する**: 縦詰めは「情報密度」を
//     決める主軸。横 spacing は ItemSpacing.y * 0.5 比例で連動 (見た目バランス
//     のための経験則、後述の ApplyToImGui で計算)。
//   ・**SaveTheme/LoadTheme は人間可読テキスト (`.acstheme`)**: `FFxeditSerializer`
//     と同設計 (1 行 1 key=value、git diff 可能、magic + version)。バイナリで
//     ない理由は「アーティストが直接編集してチームに共有」できることを優先。
//     エラーは ACS_LOG_WARN で握る (戻り値 void = 「ベストエフォート」)。
//   ・**DrawThemeSettingsUI は ImGui::Begin/End を自前で包む**: editor 設定 UI
//     なので独立 window として出す。FEditorPanel 継承はせず、調整 UI 限定の
//     シンプル window として動く (= FEditorPanel として workspace 登録するなら
//     派生ラッパを別途用意する)。
//   ・**全 noexcept / 非コピー / 非ムーブ / STL 不使用**: ACS 規約 + 他
//     editor_core 系コンポーネントと統一。`<string>` 禁止のためファイルパスは
//     `wchar_t*`、preset 名は const char* リテラル。
//
// preset カラー設計指針:
//   Dark         : 標準 dark grey (ImGui 既定の StyleColorsDark に近い + 若干
//                  暖色寄りの中間グレーで目疲れ低減)。
//   DarkBlue     : VS Code "Dark+" 風。FWindow/Frame に青みのある #1F232C 系。
//                  accent は Visual Studio の青 #007ACC 系。
//   Light        : 明るい背景 (ImGui StyleColorsLight 相当)。長時間屋外作業や
//                  プロジェクタ表示向け。
//   HighContrast : 黒 / 白 / 黄 (#FFD700) の三色設計。FAccessibilityProfile.h の
//                  `high_contrast_ui` フラグと連動する想定。
//   Sepia        : 焼け紙のような暖色基調 (#3A2E22 系背景、#F4E8D8 系 text)。
//                  長時間作業時の眼精疲労低減 (e-reader 系から着想)。
//   Custom       : SetCustomColors() で渡された値をそのまま使う。
//
// 将来拡張余地:
//   ・per-panel custom theme: ParticleEditor は赤系、ModelViewer は青系等、
//     panel ごとに別色を適用する (panel_name → EditorThemeColors map を持ち、
//     panel.DrawUI() の前後で Push/PopStyleColor する仕組み)。
//   ・automatic dark/light mode 切替: OS のテーマ設定 (Windows 10+ の
//     `AppsUseLightTheme` レジストリ) を参照して起動時 / 切替時に自動追従。
//   ・FAccessibilityProfile 連動: Colorblind モード時に HighContrast を強制、
//     ColorMode::Protanopia 時に accent を青系に切り替える等の自動マッピング。
//   ・syntax highlighting palette: FBehaviorTree editor の AST node 種別、
//     FDialogueScript の語彙ハイライト、FCombatStateMachine の遷移条件等を
//     色分けするための拡張カラーパレット (EditorThemeColors を継承する派生
//     SyntaxColors 構造体)。
//   ・color picker のリアルタイムプレビュー: 現状は SetCustomColors 経由で
//     パレット差し替え時のみ反映。DrawThemeSettingsUI 内で個別 ColorEdit4 を
//     drag 中にも即時反映する slim pipeline。
//
// 範囲外 (本クラスでは持たない):
//   ・font atlas 再構築 (= 異なる px サイズの bake)。SetFontScale は
//     ImGuiIO::FontGlobalScale を変えるのみで、低 dpi → 高 dpi で文字が
//     ボケる課題は別途解決する。
//   ・カラーピッカーの HSV / LCh 等の高度な色空間。現状は ImGui::ColorEdit4
//     (RGB + α) のみ。
//   ・theme アニメーション (起動時に Dark → DarkBlue へフェード等)。
//   ・theme の zip 化 / アセットパック化 (将来 FAssetPack 経由で配布する場合)。
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

    /** VS Code Dark+ 風 (青みのある dark)。 */
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
struct EditorThemeColors {
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
class FEditorTheme {
public:
    /** 空状態で構築する (ImGui への適用は Init / ApplyPreset で行う)。 */
    FEditorTheme() noexcept = default;

    /** 破棄する (特別な後始末なし)。 */
    ~FEditorTheme() noexcept = default;

    /** コピー禁止 (ImGui スタイルへの適用は global 副作用で唯一性が崩れるため)。 */
    FEditorTheme(const FEditorTheme&)            = delete;

    /** コピー代入も禁止。 */
    FEditorTheme& operator=(const FEditorTheme&) = delete;

    /** ムーブ禁止 (workspace に 1 インスタンスのみ存在する設計)。 */
    FEditorTheme(FEditorTheme&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FEditorTheme& operator=(FEditorTheme&&)      = delete;

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
    void SetCustomColors(const EditorThemeColors& colors) noexcept;

    /**
     * 現在のカラーパレットを返す。
     *
     * @return preset 既定 or Custom のカラーパレットへの const 参照。
     */
    const EditorThemeColors& Colors() const noexcept { return m_Colors; }

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
     * @details FWindow / Frame / Popup / Grab / Tab / Scrollbar すべてに同 radius を流す。
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
     * "Theme FSettings" 独立 ImGui window を描画する。
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

    /**
     * `.acstheme` テキストから読み込み、即時 ImGui に流す。
     *
     * @details
     * 失敗時 (ファイル無し / magic mismatch / 数値解析失敗) は ACS_LOG_WARN +
     * 現状維持 (旧 theme を保つ)。file_path が nullptr の場合は no-op。
     * @param file_path 読み込み元のファイルパス (nullptr なら no-op)。
     */
    void LoadTheme(const wchar_t* file_path) noexcept;

    /** `.acstheme` ファイル先頭の magic 文字列 (テスト / 外部ツールから参照可)。 */
    static constexpr const char* kMagic          = "ACS_THEME";

    /** `.acstheme` ファイルフォーマットの現行バージョン。 */
    static constexpr u32         kCurrentVersion = 1u;

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
                                 EditorThemeColors& out) noexcept;

    /** 現在の preset 種別 (既定 Dark)。 */
    EEditorThemePreset m_Preset          = EEditorThemePreset::Dark;

    /** 現在のカラーパレット (preset 既定 or Custom)。 */
    EditorThemeColors  m_Colors          {};

    /** global font scale (ImGuiIO::FontGlobalScale へ流す)。 */
    f32                m_FontScale      = 1.0f;

    /** 全 UI 要素共通の corner radius。 */
    f32                m_CornerRadius   = 3.0f;

    /** ItemSpacing.y (情報密度の主軸)。 */
    f32                m_ItemSpacingY  = 4.0f;
};

} // namespace acs::game::editor_core
