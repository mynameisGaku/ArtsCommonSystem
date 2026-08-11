// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "gameframework/Forward.h"

namespace acs::game::editor_core {

class FEditorLayoutSerializer;

} // namespace acs::game::editor_core

namespace acs::game::editor_core {

/**
 * 全エディタパネルの抽象基底クラス。
 *
 * @details
 * 各 panel (AHierarchyPanel / AInspectorPanel / AEditorToolbar 等) や複数の editor
 * 群を統一的に配線するため、ライフサイクル + 選択通知 + レイアウト永続化を共通化する。
 * Title() と DrawUI() は純粋仮想で各 panel 必須、それ以外の hook は no-op default を
 * 持ち必要な panel だけ override する。ImGui::Begin/End の wrap は行わず派生側責務とし、
 * Workspace ポインタは OnInit で受けて non-owning で保持する。
 */
class AEditorPanel {
public:
    /** 空状態で構築する (表示 ON、dock target OFF、Workspace 未設定)。 */
    AEditorPanel() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~AEditorPanel() noexcept = default;

    /** コピー禁止 (panel は Workspace と固有の lifecycle を持つ unique 存在)。 */
    AEditorPanel(const AEditorPanel&)            = delete;

    /** コピー代入も禁止。 */
    AEditorPanel& operator=(const AEditorPanel&) = delete;

    /** ムーブ禁止 (panel は Workspace と固有の lifecycle を持つ unique 存在)。 */
    AEditorPanel(AEditorPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    AEditorPanel& operator=(AEditorPanel&&)      = delete;

    /**
     * ImGui::Begin に渡す window タイトルを返す (純粋仮想)。
     *
     * @details
     * リテラル / 静的領域文字列を返すこと (本基底はコピー所有しない)。同じ
     * CEditorWorkspace 内で重複しない一意な名前が望ましい (ImGui の window id 衝突回避)。
     * @return window タイトル文字列。
     */
    virtual const char* Title() const noexcept = 0;

    /**
     * メインの ImGui 描画を行う (純粋仮想)。
     *
     * @details
     * ImGui::Begin(Title(), ...) / ImGui::End() は派生クラス側責務で、基底は wrap しない。
     * IsVisible() が false のとき早期 return するかは派生側判断 (典型は冒頭で
     * if (!IsVisible()) return;)。
     */
    virtual void DrawUI() noexcept = 0;

    /**
     * Workspace への登録時に 1 度呼ばれる初期化フック。
     *
     * @details
     * workspace への参照を内部に保存し、以降の hook 内から Workspace() で取り出せる
     * ようにする (基底実装)。派生クラスで追加初期化が必要なら override + 冒頭で
     * AEditorPanel::OnInit(ws) を呼ぶ。
     * @param workspace 登録先の editor workspace (参照を non-owning で保持)。
     */
    virtual void OnInit(CEditorWorkspace& workspace) noexcept;

    /**
     * Workspace からの登録解除時 / editor shutdown 時に呼ばれる後始末フック。
     *
     * @details
     * 派生クラスは GPU リソース解放やコールバック解除をここで行う。多重呼び出し可
     * (二重 Shutdown は派生側で no-op に書くこと)。
     */
    virtual void OnShutdown() noexcept {}

    /**
     * フレーム開始時 (UI 描画より前) に呼ばれるフック。
     *
     * @details input 取得 / state 更新 / 非同期タスクのポーリング等、ImGui 描画前に済ませたい処理をここで行う。
     * @param dt スケール後の経過秒。
     */
    virtual void OnFrameBegin(f32 /*dt*/) noexcept {}

    /**
     * AScene 内 ANode の選択が変わったときに呼ばれるフック。
     *
     * @details
     * selection から CurrentSelection() 等を取り出して反映する。CSelectionService の
     * lifecycle 管理は Workspace 側責務。
     * @param selection 現在の選択状態を保持する選択サービス。
     */
    virtual void OnSelectionChanged(inspector::CSelectionService& /*selection*/) noexcept {}

    /**
     * CAssetBrowser からファイルが選択された時に呼ばれるフック。
     *
     * @details
     * asset_path は CAssetBrowser が所有する文字列で、保持したい場合は固定長 char
     * バッファ等を派生クラスで持つこと (STL / <string> 不使用)。nullptr は「選択解除」と解釈する。
     * @param asset_path 選択された asset のパス (nullptr で選択解除)。
     */
    virtual void OnAssetSelected(const char* /*asset_path*/) noexcept {}

    /**
     * 起動時 / dock layout reset 時に Workspace が問い合わせる Focus 要求。
     *
     * @details true を返した panel は ImGui::SetWindowFocus 相当が一度だけ呼ばれる (典型: SceneView)。
     * @return Focus を要求するなら true (既定 false)。
     */
    virtual bool WantsFocus() const noexcept { return false; }

    /**
     * panel 独自の追加レイアウト情報をシリアライズするフック。
     *
     * @details
     * 内部 splitter 比率 / カラム幅 / 表示モード等を out に書く。ImGui の window 位置 /
     * size / dock state は ImGui 自体が ini file に保存するため対象外。
     * @param out レイアウト情報の書き出し先シリアライザ。
     */
    virtual void OnSaveLayout(FEditorLayoutSerializer& /*out*/) noexcept {}

    /**
     * OnSaveLayout で書き出した情報を復元するフック。
     *
     * @details フォーマット互換が無い場合は安全に no-op に倒すこと (例外を投げず古いレイアウトは黙って捨てる)。
     * @param in レイアウト情報の読み込み元シリアライザ。
     */
    virtual void OnLoadLayout(FEditorLayoutSerializer& /*in*/) noexcept {}

    /**
     * panel が表示状態かを返す。
     *
     * @return 表示中なら true。
     */
    bool IsVisible() const noexcept { return m_Visible; }

    /**
     * panel の表示状態を設定する (close ボタン or プログラム的 hide)。
     *
     * @param b 表示するなら true。
     */
    void SetVisible(bool b) noexcept { m_Visible = b; }

    /**
     * panel が dock target にできるかを返す。
     *
     * @return dock target 可なら true。
     */
    bool IsDockTarget() const noexcept { return m_DockedTarget; }

    /**
     * panel を dock target にできるかを設定する。
     *
     * @details Workspace 側で dock 対象 panel を絞るためのヒント (例: ステータスバーは false、ビューやインスペクタは true)。
     * @param b dock target 可とするなら true。
     */
    void SetDockTarget(bool b) noexcept { m_DockedTarget = b; }

    /**
     * OnInit で保存された Workspace ポインタを返す。
     *
     * @return Workspace へのポインタ (OnInit 前 / OnShutdown 後は nullptr、non-owning)。
     */
    CEditorWorkspace* Workspace() const noexcept { return m_Workspace; }

protected:
    /** panel 表示 toggle (派生から ImGui::Begin の close ボタンに直接バインド可能)。 */
    bool m_Visible = true;

    /** dock target 可否 (派生がコンストラクタや OnInit で設定する想定)。 */
    bool m_DockedTarget = false;

private:
    /** OnInit で保存される Workspace 参照 (non-owning)。 */
    CEditorWorkspace* m_Workspace = nullptr;
};

} // namespace acs::game::editor_core
