// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — fontedit / FFontEditorPanel (Phase 23)
//
// 複数 font face の **fallback chain** (= プライマリ + ラテン補助 + CJK 補助 +
// emoji/symbol 補助 …) を ImGui ベースで対話的に編集 + プレビューする
// **エディタパネル**。Phase 21a の `editor_core::FEditorPanel` 基底に載せ、
// `FEditorWorkspace::RegisterPanel(&panel)` 1 行で workspace に統合できる形にする。
//
// 役割:
//   ・font face リスト (= fallback 優先順位付き) の add / remove / reorder
//   ・各 face の family / file path / base size / char range / MSDF フラグ
//     等のメタ情報を表示 + 一部編集
//   ・任意テキスト (utf-8) を各 face で「擬似的に」 preview canvas に描画
//     (= 実 atlas を持たない panel なので、ImGui のデフォルト font で代用して
//      レイアウト感だけ確認できれば十分。実 face による描画統合は Phase 23+
//      で別 panel / FSpriteBatch ラッパに切り出す予定)
//   ・full char range (0x20〜0xFF) を下部に一覧描画 (= ASCII + Latin-1 補助の
//     全文字を確認するペイン)
//
// 役割分担:
//   ・本 panel は「**font face リストの編集 UI + プレビュー**」だけを担当。
//     実際の `Font` (= render/Font.h) ロードや MSDF atlas 生成は責務外。
//     caller が `FontFaceInfo` (= path + family + size + range + flag) のリスト
//     を本 panel に登録し、編集結果を取り戻して別途 Font::LoadFromFile 等を
//     呼ぶ想定 (= panel は「設定エディタ」、loader は外部)。
//   ・実 atlas 描画を panel viewport に出すのは Phase 23+ で
//     ImGui::Image + DescriptorTable 統合が必要なため、本 panel では
//     "ImGui デフォルトフォントで代用して、size slider と utf-8 入力の感触
//      だけ確認できる" 簡易プレビューに留める。
//
// 使い方 (典型):
//   FFontEditorPanel panel;
//   panel.Init();
//   workspace.RegisterPanel(&panel);   // FEditorPanel として登録
//
//   FontFaceInfo primary{};
//   primary.file_path      = L"assets/fonts/NotoSansJP-Regular.otf";
//   primary.family_name    = "Noto Sans JP";
//   primary.base_size_px   = 24.0f;
//   primary.char_range_min = 0x0020u;
//   primary.char_range_max = 0xFFFFu;
//   primary.fallback_index = 0u;   // chain の最初
//   primary.is_msdf        = true;
//   panel.AddFontFace(primary);
//
//   panel.SetPreviewText("Hello, ACS Font! 日本語 αβγ");
//   panel.SetPreviewFontSize(28.0f);
//
//   // 毎フレーム TickAllPanels(dt) の中で DrawUI が呼ばれる。
//
//   // 終了時:
//   workspace.UnregisterPanel(&panel);
//   panel.Shutdown();
//
// 設計選択 (Phase 23 FontEditor):
//   ・**FEditorPanel 継承**: Phase 21a 共通基盤を dogfood (Phase 21b ModelViewer
//     以来のパターン、FSpriteAtlasEditorPanel と同形)。Title = "Font Editor"、
//     DrawUI / OnInit を override。OnInit では基底実装を必ず呼ぶ。
//   ・**FontFaceInfo は POD**: `wchar_t* file_path` (Windows API 由来 path、
//     CreateFile / WIC 等で wide が必須) + `char* family_name` (UI 表示用、
//     UTF-8 リテラル) + 数値メタ。文字列は caller 所有のリテラル前提
//     (ACS 規約: STL/`<string>` 禁止)。本 panel は const char* / const wchar_t*
//     を非所有で参照保持。
//   ・**`acs::TArray<FontFaceInfo>` で順序保持**: 「fallback 優先順位」が UI 上
//     の表示順 = chain 順なので、順序保持配列が自然 (= 重要、FSpritePack の
//     swap remove では順序がブレるので不向き)。RemoveAt / MoveUp / MoveDown は
//     順序保持の swap で実装する。
//   ・**fallback_index は struct メンバとして冗長保持**: chain 順は TArray 内
//     index で決まるが、caller が「この face を chain の k 番目に置きたい」と
//     いう意図情報を別途持ちたいケース (= 永続化、UI 表示) があるので、struct
//     に索引フィールドを残す。Add/Move のたびに本 panel が `fallback_index = i`
//     を再書き込みする (= TArray index と同期)。
//   ・**選択 face は単一 (i32)**、-1 = 未選択。FSpriteAtlasEditorPanel と同形。
//   ・**Preview text は `char[256]`**: ImGui::InputText に渡す固定バッファ。
//     256 byte ≒ 85 utf-8 漢字相当 (3 byte/char) で、preview 用途には十分。
//     ヘッダ末尾の kPreviewTextCapacity で定義。
//   ・**Preview font size は f32 + slider**: range [8, 96] (= 一般的な editor の
//     font size editor が 8〜72 程度をカバー)。kMin/kMaxPreviewFontSize で定義。
//   ・**Char range preview は 0x20〜0xFF 固定**: ASCII (0x20-0x7E) + Latin-1
//     補助 (0xA0-0xFF) を 16 列グリッドで表示。CJK 領域 (0x3000+) は 65k 文字
//     を超えるためグリッド描画は range が広い face では別途 zoom 必要 → Phase
//     23+ で対応。kCharRangePreviewMin/Max で定義。
//   ・**face 上限は固定**: 過剰に巨大な fallback chain は font matcher 側で
//     負荷になるため、UI 上 32 face を上限とする (= 一般ゲーム 10〜15 chain で
//     十分)。kMaxFontFaces で定義。
//   ・**非コピー / 非ムーブ / 全 noexcept / STL 不使用 / `<string>` 禁止**:
//     ACS 規約。
//   ・**ImGui ヘッダは .cpp 限定**: FParticleEditorPanel / FSpriteAtlasEditorPanel
//     と同形 (header から imgui.h を出さない)。
//
// 範囲外 (Phase 23 では持たない):
//   ・実 MSDF atlas 生成 / 表示 (= Phase 23+ で acs_msdfgen バインディング)
//   ・font hinting / kerning preview (= 同上)
//   ・Unicode range の visual editor (= 現状は SliderU32 で 2 端点入力)
//   ・複数 face のマージ atlas preview (= Phase 24+ で AtlasPacker と統合)
//   ・undo / redo (= Phase 21b FUndoStack 統合は将来 OnUndo / OnRedo hook で)
//   ・font 設定の .acsfont serializer (= sample 36 側で stub callback)
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game::editor_core {
// FEditorWorkspace は FEditorPanel.h から forward-decl 経由で受ける。
class FEditorWorkspace;
} // namespace acs::game::editor_core

namespace acs::game::fontedit {

// ---------------------------------------------------------------------------
// FontFaceInfo — 1 font face のメタ情報 (= fallback chain の 1 要素)
// ---------------------------------------------------------------------------
// すべて POD (= trivially copyable)。文字列は caller 所有のリテラル前提で
// 非所有保持 (ACS 規約: `<string>` 禁止、ヒープ所有はしない)。
//
// メンバ意図:
//   - file_path     : font file の絶対 / 相対 path (Windows wide)。loader が
//                     CreateFile / WIC 等に渡す前提のため wchar_t*。
//   - family_name   : UI 表示用 family 名 (UTF-8、"Noto Sans JP" 等)。
//   - base_size_px  : この face を「基準サイズ」で焼くときの px。実描画時の
//                     scale は別途 caller 計算。
//   - char_range_min/max : 担当 Unicode 範囲。例えば JP face は 0x0020-0xFFFF
//                          全域、emoji face は 0x1F300-0x1FAFF だけ、等。
//   - fallback_index : chain の何番目か。TArray index と一致させるよう panel が
//                      Add/Move のたびに再書き込みする。caller が永続化したい
//                      場合に便利な冗長情報。
//   - is_msdf       : この face を MSDF アトラスで焼くか (= true) / 通常の
//                     bitmap で焼くか (= false)。MSDF は拡大に強いが生成
//                     コストが高いので body text のみ true 等の運用想定。
struct FontFaceInfo {
    const wchar_t* file_path      = nullptr;
    const char*    family_name    = nullptr;
    f32            base_size_px   = 24.0f;
    u32            char_range_min = 0x0020u;
    u32            char_range_max = 0x00FFu;
    u32            fallback_index = 0u;
    bool           is_msdf        = false;
};

// ---------------------------------------------------------------------------
// FFontEditorPanel — 複数 font face の fallback chain を編集 + プレビュー
// ---------------------------------------------------------------------------
class FFontEditorPanel : public acs::game::editor_core::FEditorPanel {
public:
    FFontEditorPanel() noexcept = default;
    ~FFontEditorPanel() noexcept override = default;

    // 非コピー・非ムーブ: 内部 TArray<FontFaceInfo> + 静的 preview バッファの
    // 所有関係を曖昧にしない (ACS 規約 + 他 panel 群と同形)。
    FFontEditorPanel(const FFontEditorPanel&)            = delete;
    FFontEditorPanel& operator=(const FFontEditorPanel&) = delete;
    FFontEditorPanel(FFontEditorPanel&&)                 = delete;
    FFontEditorPanel& operator=(FFontEditorPanel&&)      = delete;

    // ----- 初期化 / 解放 ---------------------------------------------------

    // 内部 face 配列をクリア、selection を解除、preview text をデフォルト
    // 文字列、preview size を 24.0f にリセット。多重 Init 可 (= 完全リセット)。
    void Init() noexcept;

    // 内部 state を解放 (face 配列 / preview バッファをゼロ化)。
    // OnShutdown とは別物 (= Workspace から外す前に panel 単体で reset したい
    // 場合の API)。多重 Shutdown 可。
    void Shutdown() noexcept;

    // ----- face リスト操作 -------------------------------------------------

    // 現在の face 数。
    u32 FontFaceCount() const noexcept;

    // i 番目の face を返す。範囲外は nullptr。
    const FontFaceInfo* GetFontFace(u32 i) const noexcept;

    // face を末尾に追加。`fallback_index` は内部で末尾 index で上書きされる
    // (= chain 内位置と同期)。上限 (kMaxFontFaces) 到達なら no-op。
    void AddFontFace(const FontFaceInfo& info) noexcept;

    // i 番目の face を削除 (順序保持)。範囲外は no-op。
    // 削除後、後続 face の `fallback_index` を詰めて再書き込みする。
    // selection が削除対象なら -1、後ろなら 1 詰める。
    void RemoveFontFace(u32 i) noexcept;

    // i 番目の face を 1 つ上に移動 (= chain で優先度を上げる、index 0 へ近づける)。
    // i == 0 or 範囲外は no-op。移動後、両者の `fallback_index` を再書き込み。
    // selection が move 対象 (どちらか) なら追随する。
    void MoveFaceUp(u32 i) noexcept;

    // i 番目の face を 1 つ下に移動 (= chain で優先度を下げる、末尾へ近づける)。
    // i == count-1 or 範囲外は no-op。
    void MoveFaceDown(u32 i) noexcept;

    // ----- 選択 ------------------------------------------------------------

    // 現在の選択 face index (-1 = 未選択)。
    i32 SelectedIndex() const noexcept;

    // face を選択。範囲外 (< 0 or >= FontFaceCount) は -1 (未選択) に正規化する。
    void SelectFace(i32 i) noexcept;

    // ----- Preview text / size ---------------------------------------------

    // preview 文字列を設定 (UTF-8、末尾 '\0' 必須)。kPreviewTextCapacity-1 を
    // 超える分は切り詰める。nullptr が来たら空文字列にする。
    void SetPreviewText(const char* utf8) noexcept;

    // 現在の preview 文字列 (静的バッファのアドレス、寿命 = panel 寿命)。
    const char* PreviewText() const noexcept;

    // preview font size (px)。
    f32 PreviewFontSize() const noexcept;
    void SetPreviewFontSize(f32 px) noexcept;

    // ----- FEditorPanel override -------------------------------------------

    // window タイトル (ImGui::Begin の引数兼 ID)。固定リテラル。
    const char* Title() const noexcept override { return "Font Editor"; }

    // Workspace 登録時に呼ばれる。基底実装で Workspace ポインタ保存、
    // 本クラスでは追加初期化 (preview バッファの終端 0 確認) を行う。
    void OnInit(acs::game::editor_core::FEditorWorkspace& workspace) noexcept override;

    // ImGui::Begin "Font Editor" + 4 領域 (toolbar / left list / center
    // preview / right inspector + bottom char-range strip) を描画。
    // IsVisible() が false なら早期 return。
    void DrawUI() noexcept override;

    // ----- 公開定数 --------------------------------------------------------

    // fallback chain の最大 face 数。32 = 一般ゲーム十分量 + font matcher の
    // 上限負荷を抑える。
    static constexpr u32 kMaxFontFaces            = 32u;

    // Preview 文字列の最大 byte 長 (終端 '\0' 込み)。
    // 256 byte ≒ 85 UTF-8 漢字 (3 byte/char) で preview には十分。
    static constexpr u32 kPreviewTextCapacity     = 256u;

    // Preview font size slider の上下限 (px)。
    static constexpr f32 kMinPreviewFontSize      = 8.0f;
    static constexpr f32 kMaxPreviewFontSize      = 96.0f;

    // 下部 char range preview の Unicode 範囲。Phase 23 では ASCII + Latin-1
    // 補助 (0x20-0xFF) 固定。CJK 全域などはグリッドが過密になるので Phase 23+
    // で zoom / paging UI を追加する。
    static constexpr u32 kCharRangePreviewMin     = 0x0020u;
    static constexpr u32 kCharRangePreviewMax     = 0x00FFu;

private:
    // ---- 編集対象 face 配列 (順序保持、index = fallback chain 順位) ----
    // PushBack / RemoveAt / 自前 swap で順序を維持する。
    acs::TArray<FontFaceInfo> _faces;

    // ---- 選択中 face index (-1 = 未選択) ----
    i32 _selected = -1;

    // ---- preview 用 utf-8 文字列バッファ (静的、ImGui::InputText 直結) ----
    // panel 寿命中アドレス不変なので、ImGui に渡す char* として安全に使える。
    c8 _preview_text[kPreviewTextCapacity] = {};

    // ---- preview font size (px) ----
    f32 _preview_size = 24.0f;
};

} // namespace acs::game::fontedit
