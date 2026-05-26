// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — spriteatlas / FSpriteAtlasEditorPanel (Phase 22 第一弾)
//
// `acs::game::FSpritePack` (gameframework/SpritePack.h) が保持する atlas メタ情報
// と名前付き frame 矩形のリストを、ImGui ベースで対話的に編集する **エディタ
// パネル**。Phase 21a で導入された editor 共通基盤
// (`editor_core::FEditorPanel` / `FEditorWorkspace`) を継承し、ModelViewer
// (Phase 21b) と同形の組立方を持つ。
//
// 役割:
//   ・atlas texture path + 名前付き FSpriteFrame 列の **編集** のみを担う
//     (atlas texture そのものの読込 / 描画は責務外、Sample 35 側で素のグリッド
//      placeholder を出すか、将来 texture 表示を追加する想定)。
//   ・toolbar (New Frame / Delete Selected / Pivot プリセット切替) + 中央 viewport
//     (atlas placeholder + 矩形 overlay) + 左 frame list + 右 inspector の
//     3 カラム + toolbar の 1 window レイアウト。
//   ・mouse drag による frame rect の resize (4 corner + 4 edge の 8 handle)。
//   ・FSpritePack そのものの所有はしない (= caller 所有、本 panel は raw pointer
//     を非所有で保持する。FSpritePack は非コピー / 非ムーブなので、参照保持しか
//     できない設計とも整合)。
//
// 役割分担 (Phase 22 SpriteAtlasEditor 全体):
//   ・本 panel (FSpriteAtlasEditorPanel) : ImGui UI + frame rect 編集
//   ・Sample 35 (HelloSpriteAtlasEditor) : Workspace + 256x256 dummy atlas を
//                                          初期登録 + Save/Load .acsatlas stub menu
//   ・将来 (Phase 22+) : `.acsatlas` シリアライザ実装 / atlas texture の実描画
//                       (ImGui Image + DescriptorTable 統合) / 自動矩形検出
//                       (Aseprite 風 trimming) / pivot guide 線描画
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
//   spriteatlas::FSpriteAtlasEditorPanel panel;
//   panel.Init();
//   panel.SetSpritePack(&pack);
//   workspace.RegisterPanel(&panel);   // FEditorPanel として登録
//
//   // 毎フレーム TickAllPanels(dt) の中で DrawUI が呼ばれる。
//
//   // 終了時:
//   workspace.UnregisterPanel(&panel); // → OnShutdown が呼ばれる
//   panel.Shutdown();
//
// 設計選択 (Phase 22 第一弾):
//   ・**FEditorPanel 継承**: Phase 21a 共通基盤を dogfood。Title / DrawUI /
//     OnInit を override。OnInit では基底実装を必ず呼ぶ。FModelViewerPanel と
//     同形 (Phase 21b 確立済パターン)。
//   ・**FSpritePack は raw pointer 非所有保持**: FSpritePack は非コピー / 非ムーブ
//     なので参照保持しか選択肢が無く、それを noexcept ABI に乗せるため
//     `class FSpritePack*` (forward decl + .h からは触らない実装)。caller が
//     `panel.SetSpritePack(&pack)` で注入し、`panel.CurrentPack()` で取り出す。
//     `nullptr` を渡されたら "no pack attached" として UI を gracefully 退化
//     (ボタンを disabled 化 + 説明文表示)。
//   ・**選択は単一 frame index (i32)**: -1 = 未選択。FParticleEditorPanel と
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
//     hover フォーカスを当てる。Phase 22 第一弾では handle 描画と drag 計算
//     のみ提供 (= 矩形の外接ピクセル単位スナップは Phase 22+ で追加)。
//   ・**v1 では atlas texture を実描画しない**: 上位仕様の「実 texture 描画は
//     v1 では省略可」を採用。背景は ImGui::ChildBackground + 自前 grid 線
//     (DrawList::AddLine) で誤魔化す。texture 描画は ImTextureID + DX12
//     descriptor heap 接続が必要で、Phase 22 範囲外。
//   ・**非コピー / 非ムーブ / 全 noexcept / STL 不使用 / `<string>` 禁止**:
//     ACS 規約。frame name は FSpritePack 側 (= caller 所有) のリテラル前提。
//     panel 内で新規 frame を AddFrame するときの name は静的バッファ
//     (`_default_frame_name_pool[]`) に書き込み、FSpritePack に const char*
//     ポインタを渡す。TPool は kMaxOwnedFrames * 32 byte 固定。
//   ・**ImGui ヘッダは .cpp 限定**: FParticleEditorPanel / FModelViewerPanel と
//     同形 (header から imgui.h を出さない)。
//
// 範囲外 (Phase 22 では持たない):
//   ・atlas texture の実 GPU 描画 (= 将来、ImGui::Image + descriptor heap 統合)
//   ・undo / redo (= Phase 21b FUndoStack 統合は将来 OnUndo / OnRedo hook で)
//   ・自動矩形検出 (Aseprite 風 trimming / connected component 解析)
//   ・複数 frame の box-select / 一括操作
//   ・カスタム pivot guide 線描画 (現状は数値表示のみ)
//   ・animation timeline 連携 (FSpriteAnimator との結合は Phase 22+ で別 panel)
//   ・.acsatlas serializer 実装 (= Sample 35 側で stub callback のみ)
#pragma once

#include "foundation/Types.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game {
// FSpritePack の forward-decl。SpritePack.h は .cpp 側でのみ include する
// (= ヘッダ依存最小化、利用側が FSpritePack の API 変更で再ビルドさせられない)。
class FSpritePack;
} // namespace acs::game

namespace acs::game::editor_core {
// FEditorWorkspace は EditorPanel.h から forward-decl 経由で受ける。
class FEditorWorkspace;
} // namespace acs::game::editor_core

namespace acs::game::spriteatlas {

// ---------------------------------------------------------------------------
// EPivotPreset — toolbar の「Pivot toggle」用 enum
// ---------------------------------------------------------------------------
// Center  = (0.5, 0.5)、TopLeft = (0.0, 0.0)、Custom = inspector で直接編集。
// E-prefix は ACS 規約 (Phase 19a)。
enum class EPivotPreset : u8 {
    Center  = 0,
    TopLeft = 1,
    Custom  = 2,
};

// ---------------------------------------------------------------------------
// FSpriteAtlasEditorPanel — atlas texture path + 名前付き frame 列を編集
// ---------------------------------------------------------------------------
class FSpriteAtlasEditorPanel : public acs::game::editor_core::FEditorPanel {
public:
    FSpriteAtlasEditorPanel() noexcept = default;
    ~FSpriteAtlasEditorPanel() noexcept override = default;

    // 非コピー・非ムーブ: 内部 frame name pool (静的配列) と FSpritePack* の
    // 参照関係を曖昧にしない (ACS 規約 + 他 panel 群と同形)。
    FSpriteAtlasEditorPanel(const FSpriteAtlasEditorPanel&)            = delete;
    FSpriteAtlasEditorPanel& operator=(const FSpriteAtlasEditorPanel&) = delete;
    FSpriteAtlasEditorPanel(FSpriteAtlasEditorPanel&&)                 = delete;
    FSpriteAtlasEditorPanel& operator=(FSpriteAtlasEditorPanel&&)      = delete;

    // ----- 初期化 / 解放 ---------------------------------------------------

    // 内部 state をデフォルトに、FSpritePack 参照を nullptr、selection を解除、
    // zoom = 1.0、pivot preset = Center に。Workspace 登録は別途
    // `FEditorWorkspace::RegisterPanel(&this)` で行う (= OnInit が呼ばれる)。
    // 多重 Init 可 (= 完全リセット)。
    void Init() noexcept;

    // 内部 state を解放 (frame name pool / selection / FSpritePack* をクリア)。
    // OnShutdown とは別物 (= Workspace から外す前に panel 単体で reset したい
    // 場合の API)。多重 Shutdown 可。
    void Shutdown() noexcept;

    // ----- FSpritePack 注入 / 取得 ------------------------------------------

    // 編集対象 FSpritePack を注入する (raw pointer / 非所有)。
    // `nullptr` を渡すと「pack 未接続」状態になり、UI は gracefully 退化する
    // (Add/Delete/Pivot ボタンが disabled、説明文 "(No FSpritePack attached)" を表示)。
    // selection は 0 にリセットされる (= 新しい pack の最初の frame を選ぶ意図)。
    void SetSpritePack(class acs::game::FSpritePack* pack) noexcept;

    // 現在注入されている FSpritePack。未注入時は nullptr。
    class acs::game::FSpritePack* CurrentPack() const noexcept;

    // ----- frame 選択 ------------------------------------------------------

    // 現在の選択 frame index。未選択は -1。
    i32 SelectedFrameIndex() const noexcept;

    // frame を選択。範囲外 (< 0 or >= FrameCount) は -1 (未選択) に正規化する。
    void SelectFrame(i32 idx) noexcept;

    // ----- frame 操作 ------------------------------------------------------

    // 新規 frame を default 64x64 (atlas 左上 0,0 起点) で追加する。
    // 名前は内部の `_default_frame_name_pool[]` に "Frame_NN" 形式で書き込み、
    // FSpritePack に const char* として渡す。追加後 selection を新規 frame に移す。
    // pack 未注入 or 上限到達 (= kMaxOwnedFrames) なら no-op。
    void AddFrame() noexcept;

    // 選択中 frame を FSpritePack から削除する。
    // 削除は FSpritePack::RemoveFrame で行うが、FSpritePack の RemoveFrame は
    // name ベース (= 同名 frame は全削除) なので、本 panel では一意性を担保
    // するため `_default_frame_name_pool` で命名済 frame しか削除しない契約。
    // 削除後 selection は前の index に詰める。未選択 / 範囲外 / pack 未注入は
    // no-op。FSpritePack は順序非保持 swap remove のため、削除後の index 整合は
    // panel 側で FSpritePack::AllFrames を再走査して取り直す (= state 同期)。
    void DeleteSelectedFrame() noexcept;

    // ----- Zoom ------------------------------------------------------------

    // atlas placeholder 表示の倍率。1.0 = 等倍 (1 atlas-pixel = 1 screen-pixel)。
    // 値は [kMinZoom, kMaxZoom] にクランプされる。
    f32 ZoomLevel() const noexcept;
    void SetZoomLevel(f32 z) noexcept;

    // ----- Pivot preset (toolbar 表示用) -----------------------------------

    // toolbar の Pivot toggle の現在値。Center / TopLeft / Custom。
    // Custom 以外を選んだ瞬間、selected frame があれば pivot 値が流し込まれる。
    EPivotPreset PivotPreset() const noexcept { return _pivot_preset; }
    void SetPivotPreset(EPivotPreset p) noexcept;

    // ----- FEditorPanel override -------------------------------------------

    // window タイトル (ImGui::Begin の引数兼 ID)。固定リテラル。
    const char* Title() const noexcept override { return "FSprite Atlas Editor"; }

    // Workspace 登録時に呼ばれる。基底実装で Workspace ポインタ保存、
    // 本クラスでは追加初期化 (frame name pool の終端 0 確認) を行う。
    void OnInit(acs::game::editor_core::FEditorWorkspace& workspace) noexcept override;

    // ImGui::Begin "FSprite Atlas Editor" + 4 領域 (toolbar / list / viewport /
    // inspector) を描画。IsVisible() が false なら早期 return。
    void DrawUI() noexcept override;

    // ----- 公開定数 --------------------------------------------------------

    // panel 内で命名する default frame name の最大個数。これを超えると
    // AddFrame は no-op (= 既存 frame の name pool 領域が尽きるため)。
    // 32 文字 × 64 個 = 2KB の静的バッファ。
    static constexpr u32 kMaxOwnedFrames        = 64u;
    static constexpr u32 kFrameNameMaxChars     = 32u;

    // Zoom 範囲。0.25 = 縮小 1/4、8.0 = 8x 拡大。
    static constexpr f32 kMinZoom               = 0.25f;
    static constexpr f32 kMaxZoom               = 8.0f;

    // AddFrame で作成するデフォルト frame のピクセルサイズ。
    static constexpr u32 kDefaultFrameW         = 64u;
    static constexpr u32 kDefaultFrameH         = 64u;

private:
    // ---- 注入された編集対象 FSpritePack (raw / 非所有) ----
    class acs::game::FSpritePack* _pack = nullptr;

    // ---- 選択中 frame index (-1 = 未選択) ----
    i32 _selected = -1;

    // ---- atlas placeholder 表示倍率 ----
    f32 _zoom = 1.0f;

    // ---- toolbar の Pivot toggle 現在値 ----
    EPivotPreset _pivot_preset = EPivotPreset::Center;

    // ---- 命名済 frame name の静的バッファ ----
    // FSpritePack に const char* を渡すため、panel 寿命中に確実に生存する
    // 領域が必要。固定長 2 次元配列 (kMaxOwnedFrames × kFrameNameMaxChars)
    // で provision する。生成済個数は AddFrame で単調増加、Shutdown で 0 に。
    // 削除した name の slot は再利用しない (= シンプル優先、上限 64 で十分)。
    c8 _default_frame_name_pool[kMaxOwnedFrames][kFrameNameMaxChars] = {};
    u32 _owned_name_count = 0;
};

} // namespace acs::game::spriteatlas
