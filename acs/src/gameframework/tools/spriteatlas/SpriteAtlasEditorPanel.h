// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — spriteatlas / ASpriteAtlasEditorPanel
//
// `acs::game::FSpritePack` (gameframework/SpritePack.h) が保持する atlas メタ情報
// と名前付き frame 矩形のリストを、ImGui ベースで対話的に編集する **エディタ
// パネル**。editor 共通基盤
// (`editor_core::AEditorPanel` / `CEditorWorkspace`) を継承し、ModelViewer
// と同形の組立方を持つ。
//
// 役割:
//   ・atlas texture path + 名前付き FSpriteFrame 列の **編集** のみを担う
//     (atlas texture そのものの読込 / 描画は責務外、FSample 35 側で素のグリッド
//      placeholder を出す)。
//   ・toolbar (New Frame / Delete Selected / Pivot プリセット切替) + 中央 viewport
//     (atlas placeholder + 矩形 overlay) + 左 frame list + 右 inspector の
//     3 カラム + toolbar の 1 window レイアウト。
//   ・mouse drag による frame rect の resize (4 corner + 4 edge の 8 handle)。
//   ・FSpritePack そのものの所有はしない (= caller 所有、本 panel は raw pointer
//     を非所有で保持する。FSpritePack は非コピー / 非ムーブなので、参照保持しか
//     できない設計とも整合)。
//
// 役割分担 (SpriteAtlasEditor 全体):
//   ・本 panel (ASpriteAtlasEditorPanel) : ImGui UI + frame rect 編集
//   ・FSample 35 (HelloSpriteAtlasEditor) : Workspace + 256x256 dummy atlas を
//                                          初期登録 + Save/Load .acsatlas stub menu
//
// 使い方 (典型):
//   acs::game::FSpritePack pack;
//   FSpritePackInfo info;
//   info.atlas_texture_path = "assets/hero_atlas.png";
//   info.atlas_width  = 256;
//   info.atlas_height = 256;
//   pack.Init(info);
//   // ... 既存 frame を AddFrame ...
//
//   spriteatlas::ASpriteAtlasEditorPanel panel;
//   panel.Init();
//   panel.SetSpritePack(&pack);
//   workspace.RegisterPanel(&panel);   // AEditorPanel として登録
//
//   // 毎フレーム TickAllPanels(dt) の中で DrawUI が呼ばれる。
//
//   // 終了時:
//   workspace.UnregisterPanel(&panel); // → OnShutdown が呼ばれる
//   panel.Shutdown();
//
// 設計選択:
//   ・**AEditorPanel 継承**: editor 共通基盤を dogfood。Title / DrawUI /
//     OnInit を override。OnInit では基底実装を必ず呼ぶ。AModelViewerPanel と
//     同形。
//   ・**FSpritePack は raw pointer 非所有保持**: FSpritePack は非コピー / 非ムーブ
//     なので参照保持しか選択肢が無く、それを noexcept ABI に乗せるため
//     `class FSpritePack*` (forward decl + .h からは触らない実装)。caller が
//     `panel.SetSpritePack(&pack)` で注入し、`panel.CurrentPack()` で取り出す。
//     `nullptr` を渡されたら "no pack attached" として UI を gracefully 退化
//     (ボタンを disabled 化 + 説明文表示)。
//   ・**選択は単一 frame index (i32)**: -1 = 未選択。AParticleEditorPanel と
//     同形の規約 (`SelectedFrameIndex()` / `SelectFrame(i32)`)。
//   ・**Pivot toggle = EPivotPreset enum** (Center / TopLeft / Custom):
//     Custom は inspector で pivot_x/pivot_y を slider で直接編集する。
//     Center / TopLeft は AddFrame / 「Pivot 適用」ボタンで selected frame に
//     pivot 値を流し込む。E-prefix enum (ACS 規約)。
//   ・**Zoom level (f32)**: atlas placeholder の表示倍率。1.0 = 1 atlas-pixel が
//     1 screen-pixel。`SetZoomLevel(f32)` で範囲 [kMinZoom, kMaxZoom] にクランプ。
//   ・**frame rect 編集は inspector の SliderInt で代替可能**: mouse drag による
//     handle resize は ImGui Item ID + InvisibleButton + GetMouseDragDelta で
//     実装するが、編集精度を担保するため inspector 側に SliderInt(x/y/w/h) を
//     必ず置く (= mouse drag は粗調整、slider は精密入力)。
//   ・**Drag handle 数 = 8** (4 corner + 4 edge): typedef EFrameHandle で
//     列挙。Hit-test は viewport 座標系で行い、現在は最も近い handle 1 個に
//     hover フォーカスを当てる。現状は handle 描画と drag 計算のみ提供。
//   ・**v1 では atlas texture を実描画しない**: 上位仕様の「実 texture 描画は
//     v1 では省略可」を採用。背景は ImGui::ChildBackground + 自前 grid 線
//     (DrawList::AddLine) で誤魔化す。texture 描画は ImTextureID + DX12
//     descriptor heap 接続が必要なため対象外。
//   ・**非コピー / 非ムーブ / 全 noexcept / STL 不使用 / `<string>` 禁止**:
//     ACS 規約。frame name は FSpritePack 側 (= caller 所有) のリテラル前提。
//     panel 内で新規 frame を AddFrame するときの name は静的バッファ
//     (`m_DefaultFrameNamePool[]`) に書き込み、FSpritePack に const char*
//     ポインタを渡す。TPool は kMaxOwnedFrames * 32 byte 固定。
//   ・**ImGui ヘッダは .cpp 限定**: AParticleEditorPanel / AModelViewerPanel と
//     同形 (header から imgui.h を出さない)。
//
// 範囲外:
//   ・atlas texture の実 GPU 描画 (= ImGui::Image + descriptor heap 統合)
//   ・undo / redo (= CUndoStack 統合は OnUndo / OnRedo hook で)
//   ・自動矩形検出 (Aseprite 風 trimming / connected component 解析)
//   ・複数 frame の box-select / 一括操作
//   ・カスタム pivot guide 線描画 (現状は数値表示のみ)
//   ・animation timeline 連携 (FSpriteAnimator との結合は別 panel)
//   ・.acsatlas serializer 実装 (= FSample 35 側で stub callback のみ)
#pragma once

#include "foundation/Types.h"
#include "gameframework/Forward.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game {
// FSpritePack の forward-decl。SpritePack.h は .cpp 側でのみ include する
// (= ヘッダ依存最小化、利用側が FSpritePack の API 変更で再ビルドさせられない)。
class FSpritePack;
} // namespace acs::game

namespace acs::game::spriteatlas {

/**
 * toolbar の Pivot toggle が選ぶ pivot プリセット。
 *
 * @details
 * Center = (0.5, 0.5)、TopLeft = (0.0, 0.0)、Custom = inspector の slider で
 * pivot_x/pivot_y を直接編集する。E-prefix は ACS 規約。
 */
enum class EPivotPreset : u8 {
    /** pivot を矩形中心 (0.5, 0.5) に置く。 */
    Center  = 0,

    /** pivot を矩形左上 (0.0, 0.0) に置く。 */
    TopLeft = 1,

    /** pivot を inspector の slider で手動編集する。 */
    Custom  = 2,
};

/**
 * atlas texture path と名前付き frame 矩形列を ImGui で対話編集するエディタパネル。
 *
 * @details
 * editor 共通基盤 `AEditorPanel` を継承し、toolbar / 左 frame list / 中央 atlas
 * viewport (placeholder + 矩形 overlay + 8 個の drag handle) / 右 inspector の
 * 4 領域を 1 window に描く。編集対象の `FSpritePack` は raw pointer で非所有保持し
 * (非コピー / 非ムーブ型のため参照保持しか取れない)、新規 frame の名前は内部の
 * 静的バッファ `m_DefaultFrameNamePool[]` に書いて const char* で FSpritePack に渡す。
 */
class ASpriteAtlasEditorPanel : public acs::game::editor_core::AEditorPanel {
public:
    /** 空状態で構築する (state は Init / SetSpritePack で確定)。 */
    ASpriteAtlasEditorPanel() noexcept = default;

    /** 派生破棄に対応する仮想デストラクタ (FSpritePack は非所有なので解放しない)。 */
    ~ASpriteAtlasEditorPanel() noexcept override = default;

    /** コピー禁止 (frame name pool と FSpritePack* の参照関係を曖昧にしないため)。 */
    ASpriteAtlasEditorPanel(const ASpriteAtlasEditorPanel&)            = delete;

    /** コピー代入も禁止。 */
    ASpriteAtlasEditorPanel& operator=(const ASpriteAtlasEditorPanel&) = delete;

    /** ムーブ禁止 (内部静的バッファのアドレス安定性を保つため)。 */
    ASpriteAtlasEditorPanel(ASpriteAtlasEditorPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    ASpriteAtlasEditorPanel& operator=(ASpriteAtlasEditorPanel&&)      = delete;

    /**
     * 内部 state を初期値へ完全リセットする (多重呼び出し可)。
     *
     * @details
     * FSpritePack 参照を nullptr、selection を未選択、zoom を 1.0、pivot preset を
     * Center に戻し、frame name pool を全クリアして命名済個数を 0 にする。可視 +
     * dock 中央配置にもする。Workspace 登録は別途 `CEditorWorkspace::RegisterPanel`
     * で行う (= OnInit が呼ばれる)。
     */
    void Init() noexcept;

    /**
     * 内部 state を解放する (frame name pool / selection / FSpritePack* をクリア。多重呼び出し可)。
     *
     * @details OnShutdown とは別物で、Workspace から外す前に panel 単体で reset したいとき用。
     */
    void Shutdown() noexcept;

    /**
     * 編集対象の FSpritePack を注入する (raw pointer / 非所有)。
     *
     * @details
     * frame があれば selection を 0 (先頭 frame)、無ければ -1 に設定する。nullptr を
     * 渡すと「pack 未接続」状態になり、UI は gracefully 退化する (Add/Delete ボタンが
     * disabled、説明文 "(No sprite pack attached)" を表示)。
     * @param pack 注入する FSpritePack (非所有、caller が寿命を管理)。nullptr 可。
     */
    void SetSpritePack(class acs::game::FSpritePack* pack) noexcept;

    /**
     * 現在注入されている FSpritePack を返す。
     *
     * @return 注入済み FSpritePack (未注入なら nullptr)。
     */
    class acs::game::FSpritePack* CurrentPack() const noexcept;

    /**
     * 現在選択中の frame index を返す。
     *
     * @return 選択中の frame index (未選択なら -1)。
     */
    i32 SelectedFrameIndex() const noexcept;

    /**
     * frame を選択する。
     *
     * @details 範囲外 (< 0 または >= FrameCount) や pack 未注入のときは -1 (未選択) に正規化する。
     * @param idx 選択する frame index。
     */
    void SelectFrame(i32 idx) noexcept;

    /**
     * 新規 frame を default 64x64 (atlas 左上 0,0 起点) で追加し、選択を移す。
     *
     * @details
     * 名前は内部 `m_DefaultFrameNamePool[]` に "Frame_NN" 形式で書き込み、const char*
     * として FSpritePack に渡す。pivot は現在の preset を反映する (Custom は 0.5, 0.5)。
     * pack 未注入、または name pool 上限 (kMaxOwnedFrames) 到達時は no-op。
     */
    void AddFrame() noexcept;

    /**
     * 選択中 frame を FSpritePack から削除し、name pool slot を回収する。
     *
     * @details
     * FSpritePack::RemoveFrame は name ベース (同名は全削除) のため、本 panel が
     * `m_DefaultFrameNamePool` で命名した frame (name pointer 一致) のみ削除する契約で、
     * panel 外で追加された frame には触らない。削除後は末尾 slot を空いた slot へ
     * swap-fill して参照する frame の name pointer を repoint し、命名済個数を 1 減らす。
     * selection は前の index に詰める (空なら -1)。未選択 / 範囲外 / pack 未注入は no-op。
     */
    void DeleteSelectedFrame() noexcept;

    /**
     * atlas placeholder の表示倍率を返す。
     *
     * @return zoom 倍率 (1.0 = 等倍、1 atlas-pixel = 1 screen-pixel)。
     */
    f32 ZoomLevel() const noexcept;

    /**
     * atlas placeholder の表示倍率を設定する。
     *
     * @param z 設定する zoom 倍率 (内部で [kMinZoom, kMaxZoom] にクランプされる)。
     */
    void SetZoomLevel(f32 z) noexcept;

    /**
     * toolbar の Pivot toggle の現在値を返す。
     *
     * @return 現在の EPivotPreset (Center / TopLeft / Custom)。
     */
    EPivotPreset PivotPreset() const noexcept { return m_PivotPreset; }

    /**
     * Pivot preset を設定し、Custom 以外なら selected frame に pivot を即適用する。
     *
     * @details Custom を渡した場合や pack 未注入 / 未選択のときは frame への適用は行わない。
     * @param p 設定する EPivotPreset。
     */
    void SetPivotPreset(EPivotPreset p) noexcept;

    /**
     * window タイトル (ImGui::Begin の引数兼 ID) を返す。
     *
     * @return 固定リテラル "Sprite Atlas Editor"。
     */
    const char* Title() const noexcept override { return "Sprite Atlas Editor"; }

    /**
     * Workspace 登録時に呼ばれる初期化フック。
     *
     * @details 基底実装で Workspace ポインタを保存したあと、frame name pool 各行の終端 0 を確認する。
     * @param workspace この panel を登録した Workspace。
     */
    void OnInit(acs::game::editor_core::CEditorWorkspace& workspace) noexcept override;

    /**
     * 1 window "Sprite Atlas Editor" を描画する (toolbar / list / viewport / inspector の 4 領域)。
     *
     * @details IsVisible() が false なら早期 return する。
     */
    void DrawUI() noexcept override;

    /** panel 内で命名する default frame name の最大個数 (= name pool 行数。超過時 AddFrame は no-op)。 */
    static constexpr u32 kMaxOwnedFrames        = 64u;

    /** frame name pool 1 行あたりの最大文字数 (終端 0 含む)。 */
    static constexpr u32 kFrameNameMaxChars     = 32u;

    /** zoom の下限 (1/4 縮小)。 */
    static constexpr f32 kMinZoom               = 0.25f;

    /** zoom の上限 (8x 拡大)。 */
    static constexpr f32 kMaxZoom               = 8.0f;

    /** AddFrame で作成するデフォルト frame の幅 (px)。 */
    static constexpr u32 kDefaultFrameW         = 64u;

    /** AddFrame で作成するデフォルト frame の高さ (px)。 */
    static constexpr u32 kDefaultFrameH         = 64u;

private:
    /** 注入された編集対象 FSpritePack (raw / 非所有、未注入なら nullptr)。 */
    class acs::game::FSpritePack* m_Pack = nullptr;

    /** 選択中 frame index (-1 = 未選択)。 */
    i32 m_Selected = -1;

    /** atlas placeholder の表示倍率 (1.0 = 等倍)。 */
    f32 m_Zoom = 1.0f;

    /** toolbar の Pivot toggle 現在値。 */
    EPivotPreset m_PivotPreset = EPivotPreset::Center;

    /**
     * 命名済 frame name を保持する固定長静的バッファ (kMaxOwnedFrames × kFrameNameMaxChars)。
     *
     * @details
     * FSpritePack に const char* を渡すため panel 寿命中に確実に生存する領域が要る。
     * 生成済個数は AddFrame で増加、Shutdown / Init で 0 に戻す。DeleteSelectedFrame は
     * 削除 slot を末尾 slot で swap-fill して回収し (参照する frame の name pointer も
     * repoint)、上限 64 で枯渇しないようにする。
     */
    c8 m_DefaultFrameNamePool[kMaxOwnedFrames][kFrameNameMaxChars] = {};

    /** name pool の live 区間 [0, m_OwnedNameCount) のサイズ (命名済 frame 数)。 */
    u32 m_OwnedNameCount = 0;
};

using FSpriteAtlasEditorPanel = ASpriteAtlasEditorPanel;

} // namespace acs::game::spriteatlas
