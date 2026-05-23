// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — SceneInspector / EditorToolbar (Phase 20 editor 第二弾)
//
// editor ウィンドウ上端に表示するツールバー。再生制御 (Play / Pause / Step)、
// Scene 保存、DebugOverlay 表示切替を 1 行に並べる。SelectionService と並ぶ
// 「editor の中央 UI 部品」で、HierarchyPanel / InspectorPanel と同じ親
// ウィンドウから DrawUI を呼ばれる想定。
//
// 使い方:
//   EditorToolbar toolbar;
//   toolbar.Init();
//   toolbar.SetOnSaveSceneCallback(&SaveSceneToDisk, &my_editor);
//
//   // 毎フレーム:
//   toolbar.DrawUI(game);
//   // toolbar が内部で Game::SetTimeScale(0/1) を切り替えている。
//
// 設計選択 (Pillar SceneInspector Phase 20):
//   ・**Play / Pause / Step / Save / DebugOverlay の 5 アクション**: 最低限の
//     editor インタラクション集合。Unity / Godot エディタの中央バーから
//     "再生制御 + 保存 + デバッグ表示" だけ抜き出した形。
//   ・**状態は EEditorState で 1 個保持**: Playing / Paused / Stepping の 3 値。
//     Stepping は「1 fixed step 進めたら自動で Paused に戻る」一時状態として
//     扱い、外側の Game ループは TimeScale=1 で 1 フレーム走らせた後に
//     再度 SetTimeScale(0) する責任を持つ (本クラスは状態保持と遷移のみ)。
//   ・**Game への time_scale 反映は本クラスが行う**: PauseDirector のように
//     「caller に値だけ返す」設計も検討したが、editor はゲームコードを
//     触らずに使われる前提なので、本クラスが直接 `Game::SetTimeScale(...)` を
//     呼ぶ。Play 時に戻す scale は `_normal_time_scale` (= Pause 直前の値) を
//     記憶しておく。
//   ・**Save callback は外部委譲**: scene の serialize 形式 (JSON / binary /
//     ACS Pak) は editor 統合層が決める。本クラスは「Save ボタンが押された」
//     という通知だけを発火する。
//   ・**DebugOverlay の所有は外側**: ここでは bool flag (`_show_debug_overlay`)
//     だけを保持する。実描画は外側コードが flag を見て OverlayDirector を
//     呼ぶ責務。これで DebugOverlay 本体への依存を回避し、ship build で
//     editor だけ消したいケースをサポートする。
//   ・**E prefix enum (E + 形容詞)**: Phase 19a で確立した規約。
//   ・**全 noexcept / 非コピー / 非ムーブ / STL 不使用**: ACS 規約。
//   ・**ImGui include 可 (.cpp 側)**: ヘッダには imgui.h を出さない方針は
//     ParticleEditorPanel と同じ。
//
// 範囲外 (Phase 2+ で):
//   ・Undo / Redo (Ctrl+Z 等のキーバインドも本クラス外)
//   ・Scene Load ボタン (現状は Save のみ)
//   ・複数 Step (N frame まとめて) — 現状は 1 step のみ
//   ・カメラ操作モード切替 (Pan / Orbit) — SceneView 側の責務
//   ・time scale slider (倍速プレイ) — 現状は 0/1 切替のみ
#pragma once

#include "foundation/Types.h"

namespace acs::game {
// Game への前方宣言 (DrawUI / TogglePlayPause で SetTimeScale を呼ぶため
// 本ヘッダから可視である必要があるが、ヘッダ依存最小化のため Game.h は
// 直接 include せず、利用側 / .cpp 側で include する)。
class Game;
} // namespace acs::game

namespace acs::game::inspector {

// ---- editor 再生状態 -----------------------------------------------------
// E prefix は Phase 19a の ACS 規約 (Enum は E から、各値は名詞)。
enum class EEditorState : u8 {
    Playing  = 0,   // 通常実行中 (TimeScale = _normal_time_scale)
    Paused   = 1,   // 一時停止中 (TimeScale = 0)
    Stepping = 2,   // 1 fixed step 進める一時状態 (本フレームで Playing 相当の dt 1 回 → Paused 復帰)
};

// ---- Save Scene コールバック型 ------------------------------------------
// Save ボタンクリック時に発火。serialize 先 (file path / format) は user 側で決定。
using SaveSceneCallback = void (*)(void* user) noexcept;

// ---- EditorToolbar — 再生制御 + Save + DebugOverlay 切替 ----------------
class EditorToolbar {
public:
    EditorToolbar() noexcept = default;
    ~EditorToolbar() noexcept = default;

    // 非コピー・非ムーブ (state holder の唯一性を担保)
    EditorToolbar(const EditorToolbar&)            = delete;
    EditorToolbar& operator=(const EditorToolbar&) = delete;
    EditorToolbar(EditorToolbar&&)                 = delete;
    EditorToolbar& operator=(EditorToolbar&&)      = delete;

    // 初期化 (state を Playing に / save callback はクリアしない)。
    // 多重呼び出し可 (= state リセットとして使える)。
    void Init() noexcept;

    // 後始末 (callback / 内部参照を解除)。多重呼び出し可。
    void Shutdown() noexcept;

    // 毎フレーム描画。ImGui main menu bar の下 (or 独立 window) に Play /
    // Pause / Step / Save / DebugOverlay ボタン行を描画する。
    // game は SetTimeScale 反映に使う。
    void DrawUI(Game& game) noexcept;

    // Play ↔ Pause の切替。
    //   Playing → Paused: 現 TimeScale を `_normal_time_scale` に記録して 0 にする。
    //   Paused  → Playing: 記録した `_normal_time_scale` を TimeScale に書き戻す。
    //   Stepping → 仕様外 (Step 中に呼ばれた場合は Playing 扱いで遷移する)。
    // Game への直接反映は DrawUI 内で行う (本 API は state 切替まで)。
    void TogglePlayPause() noexcept;

    // 1 fixed step だけ進める要求。state を Stepping に遷移するだけで、
    // 実際に "1 step 進めた後に Paused に戻す" のは DrawUI 末尾で行う。
    // 既に Stepping 中の再 Step は no-op (連続 Step は呼び出し側で間に
    // DrawUI を挟むこと)。
    void Step() noexcept;

    // 現在の editor 状態。
    EEditorState State() const noexcept { return _state; }

    // Save callback を登録。null 渡しで解除可能。
    void SetOnSaveSceneCallback(SaveSceneCallback cb, void* user) noexcept;

    // DebugOverlay の表示要望 flag。実描画は外側のコードが flag を見て切替える。
    void SetShowDebugOverlay(bool b) noexcept { _show_debug_overlay = b; }
    bool ShowDebugOverlay() const noexcept    { return _show_debug_overlay; }

private:
    // DrawUI が「ボタンが押されたら state を変える」内部分岐から呼ぶ。
    // game に対する SetTimeScale も含めてここで行う (DrawUI は ImGui だけ持つ)。
    void ApplyStateToGame(Game& game) noexcept;

    EEditorState      _state              = EEditorState::Playing;
    // Pause 直前の time scale を保持。Play 復帰時にこの値を書き戻す。
    // slow-motion 演出 (= 通常時 0.5x など) と pause を直交管理する考え方は
    // PauseDirector の NormalTimeScale と同じ。既定 1.0f。
    f32               _normal_time_scale  = 1.0f;
    // DebugOverlay 表示要望。caller が flag を見て描画する。
    bool              _show_debug_overlay = false;

    SaveSceneCallback _save_cb            = nullptr;
    void*             _save_user          = nullptr;
};

} // namespace acs::game::inspector
