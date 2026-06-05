// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — SceneInspector / FEditorToolbar
//
// editor ウィンドウ上端に表示するツールバー。再生制御 (Play / Pause / Step)、
// Scene 保存、FDebugOverlay 表示切替を 1 行に並べる。FSelectionService と並ぶ
// 「editor の中央 UI 部品」で、FHierarchyPanel / FInspectorPanel と同じ親
// ウィンドウから DrawUI を呼ばれる想定。
//
// 使い方:
//   FEditorToolbar toolbar;
//   toolbar.Init();
//   toolbar.SetOnSaveSceneCallback(&SaveSceneToDisk, &my_editor);
//
//   // 毎フレーム:
//   toolbar.DrawUI(game);
//   // toolbar が内部で FGame::SetTimeScale(0/1) を切り替えている。
//
// 設計選択 (Pillar SceneInspector):
//   ・**Play / Pause / Step / Save / FDebugOverlay の 5 アクション**: 最低限の
//     editor インタラクション集合。Unity / Godot エディタの中央バーから
//     "再生制御 + 保存 + デバッグ表示" だけ抜き出した形。
//   ・**状態は EEditorState で 1 個保持**: Playing / Paused / Stepping の 3 値。
//     Stepping は「1 fixed step 進めたら自動で Paused に戻る」一時状態として
//     扱い、外側の FGame ループは TimeScale=1 で 1 フレーム走らせた後に
//     再度 SetTimeScale(0) する責任を持つ (本クラスは状態保持と遷移のみ)。
//   ・**FGame への time_scale 反映は本クラスが行う**: FPauseDirector のように
//     「caller に値だけ返す」設計も検討したが、editor はゲームコードを
//     触らずに使われる前提なので、本クラスが直接 `FGame::SetTimeScale(...)` を
//     呼ぶ。Play 時に戻す scale は `m_NormalTimeScale` (= Pause 直前の値) を
//     記憶しておく。
//   ・**Save callback は外部委譲**: scene の serialize 形式 (JSON / binary /
//     ACS Pak) は editor 統合層が決める。本クラスは「Save ボタンが押された」
//     という通知だけを発火する。
//   ・**FDebugOverlay の所有は外側**: ここでは bool flag (`m_ShowDebugOverlay`)
//     だけを保持する。実描画は外側コードが flag を見て OverlayDirector を
//     呼ぶ責務。これで FDebugOverlay 本体への依存を回避し、ship build で
//     editor だけ消したいケースをサポートする。
//   ・**E prefix enum (E + 形容詞)**: ACS 規約。
//   ・**全 noexcept / 非コピー / 非ムーブ / STL 不使用**: ACS 規約。
//   ・**ImGui include 可 (.cpp 側)**: ヘッダには imgui.h を出さない方針は
//     FParticleEditorPanel と同じ。
//
// 範囲外:
//   ・Undo / Redo (Ctrl+Z 等のキーバインドも本クラス外)
//   ・Scene Load ボタン (現状は Save のみ)
//   ・複数 Step (N frame まとめて) — 現状は 1 step のみ
//   ・カメラ操作モード切替 (Pan / Orbit) — SceneView 側の責務
//   ・time scale slider (倍速プレイ) — 現状は 0/1 切替のみ
#pragma once

#include "foundation/Types.h"

namespace acs::game {
// FGame への前方宣言 (DrawUI / TogglePlayPause で SetTimeScale を呼ぶため
// 本ヘッダから可視である必要があるが、ヘッダ依存最小化のため FGame.h は
// 直接 include せず、利用側 / .cpp 側で include する)。
class FGame;
} // namespace acs::game

#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game::inspector {

/**
 * editor の再生状態 (Playing / Paused / Stepping)。
 *
 * @details
 * TimeScale との対応は Playing = m_NormalTimeScale、Paused = 0、Stepping = 本フレームのみ
 * m_NormalTimeScale で 1 回進めた後 Paused へ自動復帰する一時状態。
 */
enum class EEditorState : u8 {
    /** 通常実行中 (TimeScale = m_NormalTimeScale)。 */
    Playing  = 0,

    /** 一時停止中 (TimeScale = 0)。 */
    Paused   = 1,

    /** 1 fixed step 進める一時状態 (本フレームで Playing 相当の dt を 1 回 → Paused 復帰)。 */
    Stepping = 2,
};

/**
 * Save ボタンクリック時に発火する callback 型。
 *
 * @details serialize 先 (file path / format) は user 側で決定する。
 * @param user 登録時に渡したユーザポインタ。
 */
using SaveSceneCallback = void (*)(void* user) noexcept;

/**
 * 再生制御 + Save + FDebugOverlay 切替を 1 行に並べる editor ツールバー。
 *
 * @details
 * Play / Pause / Step / Save / FDebugOverlay の 5 アクションを持ち、再生状態を
 * EEditorState で 1 個保持する。TimeScale への反映 (Play 復帰時の m_NormalTimeScale
 * 書き戻し、Pause 時の 0 設定) は本クラスが直接 FGame に対して行う。Save は外部
 * callback へ委譲し、FDebugOverlay は表示要望 flag のみを保持する。
 * editor_core::FEditorPanel を継承する。
 */
class FEditorToolbar : public ::acs::game::editor_core::FEditorPanel {
public:
    /** 空のツールバーを構築する (state は Playing)。 */
    FEditorToolbar() noexcept = default;

    /** ツールバーを破棄する。 */
    ~FEditorToolbar() noexcept = default;

    /** コピー禁止 (state holder の唯一性を担保するため)。 */
    FEditorToolbar(const FEditorToolbar&)            = delete;

    /** コピー代入も禁止。 */
    FEditorToolbar& operator=(const FEditorToolbar&) = delete;

    /** ムーブ禁止 (state holder の唯一性を担保するため)。 */
    FEditorToolbar(FEditorToolbar&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FEditorToolbar& operator=(FEditorToolbar&&)      = delete;

    /**
     * 状態を Playing に戻して初期化する。
     *
     * @details
     * Save callback は意図的にクリアしないため state リセットとして多重呼び出しできる
     * (完全初期化は Shutdown → Init の順で呼ぶ)。
     */
    void Init() noexcept;

    /** 状態・callback・flag を全てデフォルトに戻して後始末する (多重呼び出し可)。 */
    void Shutdown() noexcept;

    /**
     * DrawUI / TogglePlayPause が TimeScale 反映に使う FGame を事前に設定する。
     *
     * @param game 対象の FGame (nullptr で TimeScale 反映を無効化)。
     */
    void SetGame(FGame* game) noexcept { m_Game = game; }

    /**
     * 設定済みの FGame ポインタを返す。
     *
     * @return 設定済みの FGame (未設定なら nullptr)。
     */
    FGame* GameRef() const noexcept { return m_Game; }

    /**
     * パネルのタイトル文字列を返す。
     *
     * @return 固定タイトル "Editor Toolbar"。
     */
    const char* Title() const noexcept override { return "Editor Toolbar"; }

    /**
     * 毎フレーム呼ぶツールバー描画。
     *
     * @details
     * 独立 ImGui window に Play / Pause / Step / Save / FDebugOverlay のボタン行を描画する。
     * ボタン操作の結果は末尾で 1 度だけ FGame に反映し、Stepping は同フレームで Paused へ戻す。
     * m_Game が nullptr のときは UI のみ描画して TimeScale 反映を skip する。
     */
    void DrawUI() noexcept override;

    /**
     * Play ↔ Pause を切り替える。
     *
     * @details
     * Playing → Paused、Paused → Playing。Stepping 中の呼び出しは明示 Play 要求とみなして
     * Playing へ遷移する。実際の TimeScale 反映は DrawUI 内の ApplyStateToGame が行う。
     */
    void TogglePlayPause() noexcept;

    /**
     * 1 fixed step だけ進める要求を出す。
     *
     * @details
     * state を Stepping に遷移するだけで、1 step 進めた後に Paused へ戻すのは DrawUI 末尾が行う。
     * 既に Stepping 中の再 Step は no-op (連続 Step は呼び出し側で間に DrawUI を挟む)。
     */
    void Step() noexcept;

    /**
     * 現在の editor 状態を返す。
     *
     * @return 現在の EEditorState。
     */
    EEditorState State() const noexcept { return _state; }

    /**
     * Save Scene の callback を登録する。
     *
     * @param cb 呼ぶ callback (nullptr で解除)。
     * @param user callback に渡すユーザポインタ。
     */
    void SetOnSaveSceneCallback(SaveSceneCallback cb, void* user) noexcept;

    /**
     * FDebugOverlay の表示要望 flag を設定する。
     *
     * @details 実描画は外側のコードが本 flag を見て切り替える。
     * @param b 表示したいなら true。
     */
    void SetShowDebugOverlay(bool b) noexcept { m_ShowDebugOverlay = b; }

    /**
     * FDebugOverlay の表示要望 flag を返す。
     *
     * @return 表示要望が立っていれば true。
     */
    bool ShowDebugOverlay() const noexcept    { return m_ShowDebugOverlay; }

private:
    /**
     * 現在の state を FGame の TimeScale に反映する (DrawUI から呼ぶ)。
     *
     * @details
     * Playing → m_NormalTimeScale 書き戻し、Paused → 0 設定 (直前の正の TimeScale を復帰値として記録)、
     * Stepping → m_NormalTimeScale を本フレームだけ設定する。
     * @param game TimeScale を書き換える対象の FGame。
     */
    void ApplyStateToGame(FGame& game) noexcept;

    /** 現在の editor 状態 (既定 Playing)。 */
    EEditorState      _state              = EEditorState::Playing;

    /** Pause 直前の time scale (Play 復帰時にこの値を書き戻す。既定 1.0f)。 */
    f32               m_NormalTimeScale  = 1.0f;

    /** FDebugOverlay の表示要望 flag (caller が flag を見て描画する)。 */
    bool              m_ShowDebugOverlay = false;

    /** Save ボタンクリック時に呼ぶ callback (未登録なら nullptr)。 */
    SaveSceneCallback m_SaveCb            = nullptr;

    /** Save callback に渡すユーザポインタ。 */
    void*             m_SaveUser          = nullptr;

    /** 事前 set される FGame ポインタ (nullptr のとき TimeScale 反映を skip)。 */
    FGame*             m_Game               = nullptr;
};

} // namespace acs::game::inspector
