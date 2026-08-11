// SPDX-License-Identifier: Apache-2.0
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
