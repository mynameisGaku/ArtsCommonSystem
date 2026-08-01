// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — fontedit / AFontEditorPanel
//
// 複数 font face の **fallback chain** (= プライマリ + ラテン補助 + CJK 補助 +
// emoji/symbol 補助 …) を ImGui ベースで対話的に編集 + プレビューする
// **エディタパネル**。`editor_core::AEditorPanel` 基底に載せ、
// `CEditorWorkspace::RegisterPanel(&panel)` 1 行で workspace に統合できる形にする。
//
// 役割:
//   ・font face リスト (= fallback 優先順位付き) の add / remove / reorder
//   ・各 face の family / file path / base size / char range / MSDF フラグ
//     等のメタ情報を表示 + 一部編集
//   ・任意テキスト (utf-8) を各 face で「擬似的に」 preview canvas に描画
//     (= 実 atlas を持たない panel なので、ImGui のデフォルト font で代用して
//      レイアウト感だけ確認できれば十分)
//   ・full char range (0x20〜0xFF) を下部に一覧描画 (= ASCII + Latin-1 補助の
//     全文字を確認するペイン)
//
// 役割分担:
//   ・本 panel は「**font face リストの編集 UI + プレビュー**」だけを担当。
//     実際の `FFont` (= render/Font.h) ロードや MSDF atlas 生成は責務外。
//     caller が `FFontFaceInfo` (= path + family + size + range + flag) のリスト
//     を本 panel に登録し、編集結果を取り戻して別途 FFont::LoadFromFile 等を
//     呼ぶ想定 (= panel は「設定エディタ」、loader は外部)。
//   ・実 atlas 描画を panel viewport に出すには ImGui::Image + DescriptorTable
//     統合が必要なため、本 panel では
//     "ImGui デフォルトフォントで代用して、size slider と utf-8 入力の感触
//      だけ確認できる" 簡易プレビューに留める。
//
// 使い方 (典型):
//   AFontEditorPanel panel;
//   panel.Init();
//   workspace.RegisterPanel(&panel);   // AEditorPanel として登録
//
//   FFontFaceInfo primary{};
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
// 設計選択 (FontEditor):
//   ・**AEditorPanel 継承**: 共通基盤を dogfood (ASpriteAtlasEditorPanel と
//     同形)。Title = "Font Editor"、
//     DrawUI / OnInit を override。OnInit では基底実装を必ず呼ぶ。
//   ・**FFontFaceInfo は POD**: `wchar_t* file_path` (Windows API 由来 path、
//     CreateFile / WIC 等で wide が必須) + `char* family_name` (UI 表示用、
//     UTF-8 リテラル) + 数値メタ。文字列は caller 所有のリテラル前提
//     (ACS 規約: STL/`<string>` 禁止)。本 panel は const char* / const wchar_t*
//     を非所有で参照保持。
//   ・**`acs::TArray<FFontFaceInfo>` で順序保持**: 「fallback 優先順位」が UI 上
//     の表示順 = chain 順なので、順序保持配列が自然 (= 重要、FSpritePack の
//     swap remove では順序がブレるので不向き)。RemoveAt / MoveUp / MoveDown は
//     順序保持の swap で実装する。
//   ・**fallback_index は struct メンバとして冗長保持**: chain 順は TArray 内
//     index で決まるが、caller が「この face を chain の k 番目に置きたい」と
//     いう意図情報を別途持ちたいケース (= 永続化、UI 表示) があるので、struct
//     に索引フィールドを残す。Add/Move のたびに本 panel が `fallback_index = i`
//     を再書き込みする (= TArray index と同期)。
//   ・**選択 face は単一 (i32)**、-1 = 未選択。ASpriteAtlasEditorPanel と同形。
//   ・**Preview text は `char[256]`**: ImGui::InputText に渡す固定バッファ。
//     256 byte ≒ 85 utf-8 漢字相当 (3 byte/char) で、preview 用途には十分。
//     ヘッダ末尾の kPreviewTextCapacity で定義。
//   ・**Preview font size は f32 + slider**: range [8, 96] (= 一般的な editor の
//     font size editor が 8〜72 程度をカバー)。kMin/kMaxPreviewFontSize で定義。
//   ・**Char range preview は 0x20〜0xFF 固定**: ASCII (0x20-0x7E) + Latin-1
//     補助 (0xA0-0xFF) を 16 列グリッドで表示。CJK 領域 (0x3000+) は 65k 文字
//     を超えるためグリッド描画は range が広い face では別途 zoom が必要。
//     kCharRangePreviewMin/Max で定義。
//   ・**face 上限は固定**: 過剰に巨大な fallback chain は font matcher 側で
//     負荷になるため、UI 上 32 face を上限とする (= 一般ゲーム 10〜15 chain で
//     十分)。kMaxFontFaces で定義。
//   ・**非コピー / 非ムーブ / 全 noexcept / STL 不使用 / `<string>` 禁止**:
//     ACS 規約。
//   ・**ImGui ヘッダは .cpp 限定**: AParticleEditorPanel / ASpriteAtlasEditorPanel
//     と同形 (header から imgui.h を出さない)。
//
// 範囲外:
//   ・実 MSDF atlas 生成 / 表示
//   ・font hinting / kerning preview
//   ・Unicode range の visual editor (= 現状は SliderU32 で 2 端点入力)
//   ・複数 face のマージ atlas preview
//   ・undo / redo
//   ・font 設定の .acsfont serializer
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "gameframework/Forward.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game::fontedit {

/**
 * 1 font face のメタ情報 (= fallback chain の 1 要素)。
 *
 * @details
 * すべて POD (= trivially copyable)。文字列は caller 所有のリテラル前提で
 * 非所有保持する (ACS 規約: `<string>` 禁止、ヒープ所有はしない)。
 */
struct FFontFaceInfo {
    /** font file の絶対 / 相対 path (Windows wide、loader が CreateFile / WIC 等へ渡す前提)。 */
    const wchar_t* file_path      = nullptr;

    /** UI 表示用 family 名 (UTF-8、"Noto Sans JP" 等)。 */
    const char*    family_name    = nullptr;

    /** この face を「基準サイズ」で焼くときの px (実描画時の scale は caller 計算)。 */
    f32            base_size_px   = 24.0f;

    /** 担当 Unicode 範囲の下端 (例: JP face は 0x0020、emoji face は 0x1F300 等)。 */
    u32            char_range_min = 0x0020u;

    /** 担当 Unicode 範囲の上端 (例: JP face は 0xFFFF、emoji face は 0x1FAFF 等)。 */
    u32            char_range_max = 0x00FFu;

    /** chain の何番目か。TArray index と一致するよう Add/Move 時に panel が再書き込みする冗長情報。 */
    u32            fallback_index = 0u;

    /** この face を MSDF アトラスで焼くか (true) / 通常の bitmap で焼くか (false)。 */
    bool           is_msdf        = false;
};

/**
 * 複数 font face の fallback chain を ImGui で編集 + プレビューするエディタパネル。
 *
 * @details
 * editor_core::AEditorPanel 基底に載せ、CEditorWorkspace::RegisterPanel 1 行で
 * workspace に統合できる。font face リスト (= fallback 優先順位付き) の add /
 * remove / reorder と各 face のメタ情報編集、任意 utf-8 テキストの擬似プレビュー、
 * 0x20-0xFF の char range グリッド描画を担う。非コピー / 非ムーブ / 全 noexcept。
 */
class AFontEditorPanel : public acs::game::editor_core::AEditorPanel {
public:
    /** 空のパネルを構築する (state は Init で初期化)。 */
    AFontEditorPanel() noexcept = default;

    /** 派生関係なし。基底 AEditorPanel を正しく破棄するデストラクタ。 */
    ~AFontEditorPanel() noexcept override = default;

    /** コピー禁止 (内部 TArray + 静的 preview バッファの所有を曖昧にしないため)。 */
    AFontEditorPanel(const AFontEditorPanel&)            = delete;

    /** コピー代入も禁止。 */
    AFontEditorPanel& operator=(const AFontEditorPanel&) = delete;

    /** ムーブ禁止 (他 panel 群と同形、ACS 規約)。 */
    AFontEditorPanel(AFontEditorPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    AFontEditorPanel& operator=(AFontEditorPanel&&)      = delete;

    /**
     * 内部 state を初期状態にリセットする (多重呼び出し = 完全リセット)。
     *
     * @details
     * face 配列をクリア、selection を解除、preview text をデフォルト文字列、
     * preview size を 24.0f に戻し、visible / docked を true にする。
     */
    void Init() noexcept;

    /**
     * 内部 state を解放する (face 配列 / preview バッファをゼロ化、多重呼び出し可)。
     *
     * @details Workspace から外す前に panel 単体で reset したい場合の API (OnShutdown とは別物)。
     */
    void Shutdown() noexcept;

    /**
     * 現在の face 数を返す。
     *
     * @return 登録済み face の数。
     */
    u32 FontFaceCount() const noexcept;

    /**
     * i 番目の face を返す。
     *
     * @param i 取得する face のインデックス。
     * @return i 番目の face (範囲外なら nullptr)。
     */
    const FFontFaceInfo* GetFontFace(u32 i) const noexcept;

    /**
     * face を末尾に追加する。
     *
     * @details
     * fallback_index は内部で末尾 index に上書きされる (= chain 内位置と同期)。
     * 未選択だった場合は追加した face を選択する。上限 (kMaxFontFaces) 到達なら no-op。
     * @param info 追加する face のメタ情報 (値コピーで保持)。
     */
    void AddFontFace(const FFontFaceInfo& info) noexcept;

    /**
     * i 番目の face を削除する (順序保持)。
     *
     * @details
     * 削除後、後続 face の fallback_index を詰めて再書き込みする。selection が
     * 削除対象なら -1 (空なら) または有効範囲へクランプ、後ろなら 1 詰める。範囲外は no-op。
     * @param i 削除する face のインデックス。
     */
    void RemoveFontFace(u32 i) noexcept;

    /**
     * i 番目の face を 1 つ上に移動する (= chain で優先度を上げる、index 0 へ近づける)。
     *
     * @details
     * 移動後、入れ替えた両者の fallback_index を再書き込みする。selection が move 対象
     * (どちらか) なら追随する。i == 0 または範囲外は no-op。
     * @param i 移動する face のインデックス。
     */
    void MoveFaceUp(u32 i) noexcept;

    /**
     * i 番目の face を 1 つ下に移動する (= chain で優先度を下げる、末尾へ近づける)。
     *
     * @details 移動後、両者の fallback_index を再書き込みし、selection も追随する。i == count-1 または範囲外は no-op。
     * @param i 移動する face のインデックス。
     */
    void MoveFaceDown(u32 i) noexcept;

    /**
     * 現在の選択 face index を返す。
     *
     * @return 選択中の face index (-1 = 未選択)。
     */
    i32 SelectedIndex() const noexcept;

    /**
     * face を選択する。
     *
     * @param i 選択する face index。範囲外 (< 0 or >= FontFaceCount) は -1 (未選択) に正規化する。
     */
    void SelectFace(i32 i) noexcept;

    /**
     * preview 文字列を設定する (UTF-8、末尾 '\0' 必須)。
     *
     * @details kPreviewTextCapacity-1 を超える分は切り詰める。nullptr が来たら空文字列にする。
     * @param utf8 設定する UTF-8 文字列 (nullptr 可)。
     */
    void SetPreviewText(const char* utf8) noexcept;

    /**
     * 現在の preview 文字列を返す。
     *
     * @return preview 文字列 (静的バッファのアドレス、寿命 = panel 寿命)。
     */
    const char* PreviewText() const noexcept;

    /**
     * 現在の preview font size を返す。
     *
     * @return preview font size (px)。
     */
    f32 PreviewFontSize() const noexcept;

    /**
     * preview font size を設定する。
     *
     * @param px 設定する size (px)。[kMinPreviewFontSize, kMaxPreviewFontSize] にクランプされる。
     */
    void SetPreviewFontSize(f32 px) noexcept;

    /**
     * window タイトルを返す (ImGui::Begin の引数兼 ID、固定リテラル)。
     *
     * @return "Font Editor"。
     */
    const char* Title() const noexcept override { return "Font Editor"; }

    /**
     * Workspace 登録時に呼ばれる初期化フック。
     *
     * @details 基底実装で Workspace ポインタを保存し、本クラスでは preview バッファの終端 0 を確定する。
     * @param workspace 登録先の CEditorWorkspace。
     */
    void OnInit(acs::game::editor_core::CEditorWorkspace& workspace) noexcept override;

    /**
     * 毎フレームの UI 描画フック。
     *
     * @details
     * ImGui::Begin "Font Editor" + toolbar / left list / center preview /
     * right inspector / bottom char-range strip を描画する。IsVisible() が false なら早期 return。
     */
    void DrawUI() noexcept override;

    /** fallback chain の最大 face 数 (一般ゲーム十分量 + font matcher の上限負荷を抑える)。 */
    static constexpr u32 kMaxFontFaces            = 32u;

    /** Preview 文字列の最大 byte 長 (終端 '\0' 込み、256 byte ≒ 85 UTF-8 漢字)。 */
    static constexpr u32 kPreviewTextCapacity     = 256u;

    /** Preview font size slider の下限 (px)。 */
    static constexpr f32 kMinPreviewFontSize      = 8.0f;

    /** Preview font size slider の上限 (px)。 */
    static constexpr f32 kMaxPreviewFontSize      = 96.0f;

    /** 下部 char range preview の Unicode 範囲の下端 (ASCII + Latin-1 補助で固定)。 */
    static constexpr u32 kCharRangePreviewMin     = 0x0020u;

    /** 下部 char range preview の Unicode 範囲の上端 (ASCII + Latin-1 補助で固定)。 */
    static constexpr u32 kCharRangePreviewMax     = 0x00FFu;

private:
    /** 編集対象 face 配列 (順序保持、index = fallback chain 順位)。 */
    acs::TArray<FFontFaceInfo> m_Faces;

    /** 選択中 face index (-1 = 未選択)。 */
    i32 m_Selected = -1;

    /** preview 用 utf-8 文字列バッファ (静的、ImGui::InputText 直結、panel 寿命中アドレス不変)。 */
    c8 m_PreviewText[kPreviewTextCapacity] = {};

    /** preview font size (px)。 */
    f32 m_PreviewSize = 24.0f;
};

using FFontEditorPanel = AFontEditorPanel;

} // namespace acs::game::fontedit
