// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace acs::game {

/** アセット束を管理する正規型。 */
class CAssetBundle;
/** 旧公開名から正規アセット束型へ接続する互換別名。 */
using FAssetBundle = CAssetBundle;

/** 音声演出を管理する正規型。 */
class CAudioDirector;
/** 旧公開名から正規音声演出型へ接続する互換別名。 */
using FAudioDirector = CAudioDirector;

/** 映像演出を管理する正規型。 */
class CCinematicsDirector;
/** 旧公開名から正規映像演出型へ接続する互換別名。 */
using FCinematicsDirector = CCinematicsDirector;

/** 二次元衝突世界を管理する正規型。 */
class CCollisionWorld2D;
/** 旧公開名から正規二次元衝突世界型へ接続する互換別名。 */
using FCollisionWorld2D = CCollisionWorld2D;

/** 三次元衝突query shapeを管理する正規型。 */
class CCollisionWorld3D;

/** デバッグ線を管理する正規型。 */
class CDebugDraw;
/** 旧公開名から正規デバッグ線型へ接続する互換別名。 */
using FDebugDraw = CDebugDraw;

/** 名前付きイベントを中継する正規所有型。 */
class AEventBus;
/** 旧公開名から正規イベント中継型へ接続する互換別名。 */
using FEventBus = AEventBus;

/** ゲーム進行を管理する正規型。 */
class CGame;
/** 旧公開名から正規ゲーム型へ接続する互換別名。 */
using FGame = CGame;

/** 入力記録を管理する正規型。 */
class CInputRecorder;
/** 旧公開名から正規入力記録型へ接続する互換別名。 */
using FInputRecorder = CInputRecorder;

/** inspectorとの接続を管理する正規型。 */
class CInspectorSeam;
/** 旧公開名から正規inspector接続型へ接続する互換別名。 */
using FInspectorSeam = CInspectorSeam;

/** 表示言語を管理する正規型。 */
class CLocalizationDirector;
/** 旧公開名から正規表示言語型へ接続する互換別名。 */
using FLocalizationDirector = CLocalizationDirector;

/** 同期進行を管理する正規型。 */
class CLockstep;
/** 旧公開名から正規同期進行型へ接続する互換別名。 */
using FLockstep = CLockstep;

/** 経路探索格子を管理する正規型。 */
class CNavGrid;
/** 旧公開名から正規経路探索格子型へ接続する互換別名。 */
using FNavGrid = CNavGrid;

/** 個人情報保護設定を管理する正規型。 */
class CPrivacyDirector;
/** 旧公開名から正規個人情報保護型へ接続する互換別名。 */
using FPrivacyDirector = CPrivacyDirector;

/** 再生処理を管理する正規型。 */
class CReplayDirector;
/** 旧公開名から正規再生管理型へ接続する互換別名。 */
using FReplayDirector = CReplayDirector;

/** 二次元剛体世界を管理する正規型。 */
class CRigidWorld2D;
/** 旧公開名から正規二次元剛体世界型へ接続する互換別名。 */
using FRigidWorld2D = CRigidWorld2D;

/** 一つの画面状態を所有する正規型。 */
class AScene;
/** 旧公開名から正規scene型へ接続する互換別名。 */
using FScene = AScene;

/** scene遷移を管理する正規型。 */
class CSceneManager;
/** 旧公開名から正規scene管理型へ接続する互換別名。 */
using FSceneManager = CSceneManager;

/** sceneのnode treeとnode poolを所有する正規型。 */
class CSceneNodeGraph;

/** scene描画資源をgame寿命で共有する正規型。 */
class CSceneRenderResources;

/** sceneへ共有機能を渡す正規型。 */
class CSceneServices;
/** 旧公開名から正規scene共有機能型へ接続する互換別名。 */
using FSceneServices = CSceneServices;

namespace editor_core {

/** editor操作を表す正規所有型。 */
class AEditorCommand;
/** 旧公開名から正規editor操作型へ接続する互換別名。 */
using FEditorCommand = AEditorCommand;

/** editor panelを表す正規所有型。 */
class AEditorPanel;
/** 旧公開名から正規editor panel型へ接続する互換別名。 */
using FEditorPanel = AEditorPanel;

/** editor作業領域を管理する正規型。 */
class CEditorWorkspace;
/** 旧公開名から正規editor作業領域型へ接続する互換別名。 */
using FEditorWorkspace = CEditorWorkspace;

} // namespace editor_core

namespace inspector {

/** editor選択状態を管理する正規型。 */
class CSelectionService;
/** 旧公開名から正規editor選択型へ接続する互換別名。 */
using FSelectionService = CSelectionService;

} // namespace inspector
} // namespace acs::game

namespace acs {

/** GameFrameworkの正規型を短い名前で参照する公開入口。 */
using game::CAssetBundle;
using game::CAudioDirector;
using game::CCinematicsDirector;
using game::CCollisionWorld2D;
using game::CDebugDraw;
using game::AEventBus;
using game::CGame;
using game::CInputRecorder;
using game::CInspectorSeam;
using game::CLocalizationDirector;
using game::CLockstep;
using game::CNavGrid;
using game::CPrivacyDirector;
using game::CReplayDirector;
using game::CRigidWorld2D;
using game::AScene;
using game::CSceneManager;
using game::CSceneNodeGraph;
using game::CSceneRenderResources;
using game::CSceneServices;
using game::editor_core::AEditorCommand;
using game::editor_core::AEditorPanel;
using game::editor_core::CEditorWorkspace;
using game::inspector::CSelectionService;

/** GameFrameworkの旧公開名を正規型へ接続する互換入口。 */
using game::FAssetBundle;
using game::FAudioDirector;
using game::FCinematicsDirector;
using game::FCollisionWorld2D;
using game::FDebugDraw;
using game::FEventBus;
using game::FGame;
using game::FInputRecorder;
using game::FInspectorSeam;
using game::FLocalizationDirector;
using game::FLockstep;
using game::FNavGrid;
using game::FPrivacyDirector;
using game::FReplayDirector;
using game::FRigidWorld2D;
using game::FScene;
using game::FSceneManager;
using game::FSceneServices;
using game::editor_core::FEditorCommand;
using game::editor_core::FEditorPanel;
using game::editor_core::FEditorWorkspace;
using game::inspector::FSelectionService;

} // namespace acs
