// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — editor_core / FEditorTheme (Phase 21a)
//
// 役割:
//   ACS の全エディタ panel (FHierarchyPanel / FInspectorPanel / FEditorToolbar /
//   FParticleEditorPanel / Phase 21b+ ModelViewer / LevelEditor / AnimCurveEditor /
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
// 設計選択 (Pillar editor_core Phase 21a):
//   ・**ACS::FVec4 ベース、ImVec4 は .cpp 内変換のみ**: ヘッダから <imgui.h> を
//     漏らさないことで、本ヘッダを include しても include order が壊れない
//     (FInspectorPanel / FParticleEditorPanel と同パターン)。
//   ・**preset = 「色パレット定数 + spacing + corner」のスナップショット**:
//     ApplyPreset は内部 `_colors` を上書きしたあと、`ApplyToImGui()` で
//     ImGui::GetStyle() に流す。Custom はユーザが SetCustomColors した状態を
//     哨兵とする (= 既定の Custom 値は Dark と同等)。
//   ・**font scale は ImGuiIO::FontGlobalScale に流す**: フォント atlas 自体は
//     再構築せず、グローバルスケール変更で済ませる (高 DPI で軽い)。本格的な
//     atlas 再構築 (= 異なる px サイズの bake) は Phase 21c で検討。
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
//     派生ラッパを Phase 21b で別途用意する)。
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
//                  `high_contrast_ui` フラグと連動する想定 (Phase 21b で
//                  FAccessibilityProfile→FEditorTheme bridge を予定)。
//   Sepia        : 焼け紙のような暖色基調 (#3A2E22 系背景、#F4E8D8 系 text)。
//                  長時間作業時の眼精疲労低減 (e-reader 系から着想)。
//   Custom       : SetCustomColors() で渡された値をそのまま使う。
//
// 将来拡張余地 (Phase 21b+ で):
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
//     ボケる課題は Phase 21c の `EditorFontPack` で別途解決。
//   ・カラーピッカーの HSV / LCh 等の高度な色空間。現状は ImGui::ColorEdit4
//     (RGB + α) のみ。
//   ・theme アニメーション (起動時に Dark → DarkBlue へフェード等)。
//   ・theme の zip 化 / アセットパック化 (将来 FAssetPack 経由で配布する場合)。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game::editor_core {

// =============================================================================
// EEditorThemePreset — プリセット種別
// -----------------------------------------------------------------------------
//   Dark         : 標準 dark grey (デフォルト)
//   DarkBlue     : VS Code Dark+ 風 (青みのある dark)
//   Light        : 明るい背景 (屋外 / プロジェクタ向け)
//   HighContrast : 黒 / 白 / 黄 (アクセシビリティ Phase 14 連動可能)
//   Sepia        : 焼け紙風暖色 (長時間作業向け)
//   Custom       : SetCustomColors() で渡された任意パレット
// =============================================================================
enum class EEditorThemePreset : u8 {
    Dark         = 0,
    DarkBlue     = 1,
    Light        = 2,
    HighContrast = 3,
    Sepia        = 4,
    Custom       = 5,
};

// =============================================================================
// EditorThemeColors — preset 1 個分のカラーパレット
// -----------------------------------------------------------------------------
// 各メンバは RGBA `[0, 1]` 範囲の `acs::FVec4`。ImGui の ImVec4 と同じ意味論
// (= sRGB 線形値、α は 0 = 完全透明 / 1 = 不透明)。
//
// パレット内訳:
//   window_bg      : ImGuiCol_WindowBg          (パネル背景)
//   title_bg       : ImGuiCol_TitleBg / Active  (タイトルバー)
//   button_bg      : ImGuiCol_Button            (通常ボタン)
//   button_hover   : ImGuiCol_ButtonHovered     (ホバー時)
//   button_active  : ImGuiCol_ButtonActive      (押下中)
//   frame_bg       : ImGuiCol_FrameBg           (InputText / Combo 等の枠内)
//   text           : ImGuiCol_Text              (通常文字)
//   text_disabled  : ImGuiCol_TextDisabled      (グレーアウト文字)
//   border         : ImGuiCol_Border            (枠線)
//   separator      : ImGuiCol_Separator         (区切り線)
//   accent         : ImGuiCol_CheckMark /
//                    ImGuiCol_SliderGrab /
//                    ImGuiCol_TabActive 等の
//                    強調色 (= ACS ブランド色相当)
//   warning        : 警告メッセージ用 (ImGui の標準色ではなく、本 theme で
//                    `Text("[!]")` 等に手動適用する用途)
//   error          : エラーメッセージ用 (同上)
// =============================================================================
struct EditorThemeColors {
    FVec4 window_bg{};
    FVec4 title_bg{};
    FVec4 button_bg{};
    FVec4 button_hover{};
    FVec4 button_active{};
    FVec4 frame_bg{};
    FVec4 text{};
    FVec4 text_disabled{};
    FVec4 border{};
    FVec4 separator{};
    FVec4 accent{};
    FVec4 warning{};
    FVec4 error{};
};

// =============================================================================
// FEditorTheme — ImGui スタイル統一テーマ管理
// =============================================================================
class FEditorTheme {
public:
    FEditorTheme() noexcept = default;
    ~FEditorTheme() noexcept = default;

    // 非コピー・非ムーブ: ImGui::GetStyle() への適用は global 副作用なので、
    // インスタンスを複製すると「どれが真の theme か」が曖昧になる。Editor
    // workspace に 1 インスタンスのみ存在する設計。
    FEditorTheme(const FEditorTheme&)            = delete;
    FEditorTheme& operator=(const FEditorTheme&) = delete;
    FEditorTheme(FEditorTheme&&)                 = delete;
    FEditorTheme& operator=(FEditorTheme&&)      = delete;

    // 初期化: default = Dark preset を ImGui::GetStyle() に流す。多重呼び出し可
    // (各回で全 ImGui スタイルが上書きされる)。`ImGui::CreateContext` 後に
    // 呼ぶこと (context 未生成だと GetStyle() / GetIO() が落ちる)。
    void Init() noexcept;

    // ----- preset -----------------------------------------------------------

    // preset 既定の色 / spacing / corner を `_colors` に書き込み、即時 ImGui に
    // 流す。Custom が渡された場合は `_colors` を保持したまま現値を再適用する
    // (= SetCustomColors で書き込み後に preset を切り替えても、Custom に戻す
    //  と直前の Custom パレットが復活する)。
    void ApplyPreset(EEditorThemePreset preset) noexcept;

    // 現在の preset 種別を返す。SetCustomColors を呼ぶと自動で Custom になる。
    EEditorThemePreset CurrentPreset() const noexcept { return _preset; }

    // ----- Custom パレット ---------------------------------------------------

    // 任意パレットを設定し、preset を Custom に切り替えて即時 ImGui に流す。
    // 各カラーは RGBA `[0, 1]` 範囲想定 (clamp しない: ImGui 側で扱える HDR を
    // 妨げないため、1.0 超の値も技術的には許可)。
    void SetCustomColors(const EditorThemeColors& colors) noexcept;

    // 現在のカラーパレット (preset 既定 or Custom) を const 参照で取得。
    const EditorThemeColors& Colors() const noexcept { return _colors; }

    // ----- font / spacing / corner -----------------------------------------

    // global font scale (高 DPI / HighContrast 視認性向上用)。
    // 内部的に ImGuiIO::FontGlobalScale を書き換える。`<= 0` は無視 (no-op)。
    // 4.0 超は警告ログを出して 4.0 に clamp (= 起動時 typo 防御)。
    void SetFontScale(f32 scale) noexcept;

    // 現在の font scale を返す。
    f32 FontScale() const noexcept { return _font_scale; }

    // 全 corner radius (FWindow / Frame / Popup / Grab / Tab / Scrollbar) を
    // `radius` に統一する。負値は 0 に clamp。
    void SetRoundedCorners(f32 radius) noexcept;

    // 現在の corner radius を返す。
    f32 RoundedCorners() const noexcept { return _corner_radius; }

    // ItemSpacing.y を `item_spacing_y` に設定 (情報密度の主軸)。
    // ItemSpacing.x はこの値の 0.5 倍に連動 (見た目バランス用の経験則)。
    // 負値は 0 に clamp。
    void SetSpacing(f32 item_spacing_y) noexcept;

    // 現在の ItemSpacing.y を返す。
    f32 Spacing() const noexcept { return _item_spacing_y; }

    // ----- Theme FSettings UI 描画 -------------------------------------------

    // "Theme FSettings" という独立 ImGui window を描画する。
    // 内容: preset combo + 全カラー ColorEdit4 + font_scale / corner / spacing
    // の SliderFloat + Save/Load ボタン (ファイルパスは固定で
    // `data/editor/theme.acstheme` を使う、Phase 21b でファイルダイアログ化)。
    // Editor 側の毎フレーム loop から `theme.DrawThemeSettingsUI();` 1 行で呼ぶ。
    void DrawThemeSettingsUI() noexcept;

    // ----- 永続化 (`.acstheme` テキスト) ------------------------------------

    // 現在の preset / colors / font_scale / corner / spacing を `.acstheme`
    // テキスト形式で書き出す。失敗時は ACS_LOG_WARN で記録 (戻り値 void)。
    // file_path が nullptr の場合は no-op。
    void SaveTheme(const wchar_t* file_path) noexcept;

    // `.acstheme` テキストから読み込み、即時 ImGui に流す。
    // 失敗時 (ファイル無し / magic mismatch / 数値解析失敗) は ACS_LOG_WARN +
    // 現状維持 (= 旧 theme を保つ)。file_path が nullptr の場合は no-op。
    void LoadTheme(const wchar_t* file_path) noexcept;

    // ---- file format 定数 (テスト / 外部ツールから参照可能) ----------------
    static constexpr const char* kMagic          = "ACS_THEME";
    static constexpr u32         kCurrentVersion = 1u;

private:
    // ApplyPreset / SetCustomColors / Load 後に呼ばれる。`_colors` /
    // `_font_scale` / `_corner_radius` / `_item_spacing_y` を ImGui::GetStyle()
    // 及び ImGuiIO に流し込む。ImGui context 未生成時は何もしない (defensive)。
    void ApplyToImGui() noexcept;

    // preset 種別ごとの既定パレットを `out` に書き込む。Custom が渡された
    // 場合は `_colors` を上書きしない (= 既存 Custom 値を保持)。
    static void FillPresetColors(EEditorThemePreset preset,
                                 EditorThemeColors& out) noexcept;

    EEditorThemePreset _preset          = EEditorThemePreset::Dark;
    EditorThemeColors  _colors          {};
    f32                _font_scale      = 1.0f;
    f32                _corner_radius   = 3.0f;
    f32                _item_spacing_y  = 4.0f;
};

} // namespace acs::game::editor_core
