<!-- SPDX-License-Identifier: Apache-2.0 -->
# ACS Editor Architecture (v2)

> **現行実装との対応 (2026-07-19):** 旧版で用いた `Node2D` / `Node3D` は編集対象の
> 2D/3D 表示モードを示す歴史的表記である。ネイティブのノード型は単一の `ANode`、
> コンポーネント基底は `AComponent` へ統一済み。型名と所有権の現行仕様は
> [`NodeUnification.md`](NodeUnification.md) を参照。

**目的 / Purpose**: ACS GameFramework が提供する **in-game editor 群** (9 つの authoring sample + 共通基盤) の設計を一冊にまとめる。各 editor の API、依存関係、責任分担、ファイル形式、UI 規約を横断的に俯瞰できるドキュメント。

**対象 / Scope**: `src/gameframework/tools/**` 配下と `samples/29_HelloParticleEditor` 〜 `samples/37_HelloCinematicsEditor`。FontEditor と CinematicsTimelineEditor を含む現行実装を対象とする。

**バージョン / Version**: v2 (2026-07-19)

**関連ドキュメント**:
- 詳細仕様: `docs/GameFramework.md` §15.4「著作ツール深度 & 外部ミドルウェアシーム (Pillar K 拡張)」
- コーディング規約: `docs/StyleGuide.md`
- 全体アーキテクチャ: `docs/ARCHITECTURE.md`

---

## 目次 / Table of Contents

1. [概要 / Overview](#1-概要--overview)
2. [ディレクトリ構成 / Directory Layout](#2-ディレクトリ構成--directory-layout)
3. [editor_core 共通基盤 / Common Foundation](#3-editor_core-共通基盤--common-foundation)
4. [9 種の editor sample 一覧 / Editor Sample Catalogue](#4-9-種の-editor-sample-一覧--editor-sample-catalogue)
5. [設計パターン / Design Patterns](#5-設計パターン--design-patterns)
6. [共通制約 / Cross-cutting Constraints](#6-共通制約--cross-cutting-constraints)
7. [将来拡張余地 / Future Extensions](#7-将来拡張余地--future-extensions)
8. [参照先 / References](#8-参照先--references)

---

## 1. 概要 / Overview

### 1.1 設計方針 (v10「著作ツール深度」より)

ACS の editor は **in-engine ImGui ベース** (外部 IDE なし) という方針を採る。これは `docs/GameFramework.md` §15.4 で確定した v10 著作ツール深度の指針に従う:

- **In-engine tooling**: editor は engine と同じプロセス内で動作する。Unity Editor のような「別アプリ」モデルではなく、Unreal Engine の editor mode に近い「engine の特殊な起動形態」。
- **UiKit と疎結合**: editor の UI は ImGui (debug/dev build) を主要レイヤとして使い、ACS の独自 UI (`acs::FUiLayer` と `acs::ui::FWidget` tree) とは分離する。
- **共通基盤の dogfooding**: editor panel は `editor_core::FEditorPanel` 基底を継承し、`FEditorWorkspace` で統括することで、新規 editor を追加する際の boilerplate を最小化する。`FAssetBrowser` などの service/helper は panel ではないため直接描画する。

### 1.2 ImGui を採用する理由

| 観点 | ImGui を主 UI に使う理由 |
|---|---|
| **開発速度** | Immediate-mode で「描画 = state 反映」を短い widget code として書ける。 |
| **iteration** | hot reload なしでも widget 追加が即座に反映される (= IMGUI の本領)。 |
| **dependency** | Diligent Engine ベースの ACS RHI に既に統合済 (`samples/21_HelloImGui`)。再利用コストゼロ。 |
| **機能差の隔離** | docking API は `ACS_EDITOR_HAS_IMGUI_DOCK` で guard し、非 docking build でも通常 window として動作させる。 |

ACS UI (`acs::FUiLayer`) を editor UI に流用しない理由:
- editor を game build と分けたいため、editor だけ ImGui で書いて、ゲーム本体の UI (`acs::ui`) には触らせない方針。
- editor 固有の診断・authoring state を game UI の runtime budget と lifecycle から分離できる。

### 1.3 階層構造の概念図

```
+----------------------------------------------------------------------+
|                   FEditorWorkspace (Phase 21a hub)                    |
| - panels: TArray<FEditorPanel*>                                        |
| - DockSpace + MenuBar 自動描画                                       |
| - SaveLayout/LoadLayout (.acslayout)                                 |
| - BroadcastSelectionChanged / BroadcastAssetSelected                 |
+----------------------------------------------------------------------+
   |              |              |              |
   v              v              v              v
+---------------+ +-----------------+ +-----------------+ +-------------------+
| FAssetBrowser | | FInspectorPanel | | FHierarchyPanel | | FModelViewerPanel |
| (helper)      | | (field edit)    | | (ANode tree)    | | (3D viewport)     |
| direct draw   | | FEditorPanel    | | FEditorPanel    | | FEditorPanel      |
+---------------+ +-----------------+ +-----------------+ +-------------------+
                 |              |               |
                 +--------------+               |
                                |               |
                                v               v
                       +------------------+ +----------------+
                       | FSelectionService | | FEditorCommand  |
                       | (Phase 20 hub)   | | + FUndoStack    |
                       +------------------+ +----------------+
                                                    |
                                                    v
                                          +-----------------+
                                          | FEditorGizmo     |
                                          | (Translate/     |
                                          |  Rotate/Scale)  |
                                          +-----------------+
```

### 1.4 ファイル統計 (2026-07-19 現在)

| 区分 | ファイル数 | 物理 LOC |
|---|---|---|
| **editor_core** | 9 .h + 8 .cpp = 17 | 8,292 |
| **fxedit** | 3 .h + 3 .cpp = 6 | 2,812 |
| **inspector** | 4 .h + 4 .cpp = 8 | 2,133 |
| **modelview** | 4 .h + 4 .cpp = 8 | 3,111 |
| **animcurve** | 1 .h + 1 .cpp = 2 | 1,057 |
| **btedit** | 4 .h + 1 .cpp = 5 | 4,378 |
| **leveledit** | 1 .h + 1 .cpp = 2 | 992 |
| **spriteatlas** | 1 .h + 1 .cpp = 2 | 1,078 |
| **fontedit** | 1 .h + 1 .cpp = 2 | 977 |
| **cinetimeline** | 1 .h + 1 .cpp = 2 | 1,260 |
| **samples 29-37** | 69 files | 3,638 |
| **合計** | **123 files** | **29,728** |

LOC は空行・コメントを含む対象ファイルの物理行数。sample の行は各ディレクトリ直下の `CMakeLists.txt`、`.h`、`.cpp` を含む。

---

## 2. ディレクトリ構成 / Directory Layout

```
src/gameframework/tools/
├── editor_core/   - 共通基盤 (Phase 21a)
│   ├── EditorPanel.h/.cpp         - FEditorPanel (全 panel の抽象基底)
│   ├── EditorWorkspace.h/.cpp     - FEditorWorkspace (複数 panel の統括 hub)
│   ├── UndoStack.h/.cpp           - FUndoStack
│   ├── EditorCommand.h            - FEditorCommand / FMoveNodeCommand
│   ├── EditorCamera.h/.cpp        - FEditorCamera / FEditorCameraState
│   ├── AssetBrowser.h/.cpp        - FAssetBrowser / FAssetEntry
│   ├── PropertyDrawer.h/.cpp      - FPropertyDrawerRegistry / FPropertyContext
│   ├── EditorGizmo.h/.cpp         - FEditorGizmo / FGizmoState
│   └── EditorTheme.h/.cpp         - FEditorTheme / FEditorThemeColors
│
├── fxedit/        - ParticleEditor (Phase 19b)
│   ├── ParticleEditorPanel.h/.cpp - FParticleEditorPanel
│   ├── ParticleEditorPreview.h/.cpp - FParticleEditorPreview
│   └── FxeditSerializer.h/.cpp    - FFxeditSerializer
│
├── inspector/     - SceneInspector 3 panel + 1 service (Phase 20)
│   ├── SelectionService.h/.cpp    - FSelectionService
│   ├── HierarchyPanel.h/.cpp      - FHierarchyPanel
│   ├── InspectorPanel.h/.cpp      - FInspectorPanel
│   └── EditorToolbar.h/.cpp       - FEditorToolbar
│
├── modelview/     - ModelViewer 4 panel (Phase 21b)
│   ├── ModelViewerPanel.h/.cpp    - FModelViewerPanel
│   ├── ModelInspectorPanel.h/.cpp - FModelInspectorPanel
│   ├── ModelMaterialPanel.h/.cpp  - FModelMaterialPanel
│   └── ModelAnimationPanel.h/.cpp - FModelAnimationPanel
│
├── animcurve/     - AnimCurveEditor (Phase 22)
│   └── AnimCurveEditorPanel.h/.cpp - FAnimCurveEditorPanel
│
├── btedit/        - BehaviorTreeEditor (Phase 22)
│   ├── BehaviorTreeEditorPanel.h/.cpp - FBehaviorTreeEditorPanel
│   ├── BtActionRegistry.h             - FBtActionRegistry
│   ├── BtCatalog.h                    - FBtConditionRegistry / FBtBlackboard
│   └── BtGuardNodes.h                 - FBtConditionNode / FBtCompareNode
│
├── leveledit/     - LevelEditor / FTilemap painter (Phase 22)
│   └── LevelEditorPanel.h/.cpp    - FLevelEditorPanel
│
├── spriteatlas/   - SpriteAtlasEditor (Phase 22)
│   └── SpriteAtlasEditorPanel.h/.cpp - FSpriteAtlasEditorPanel
│
├── fontedit/      - FontEditor
│   └── FontEditorPanel.h/.cpp         - FFontEditorPanel / FFontFaceInfo
│
└── cinetimeline/ - CinematicsTimelineEditor
    └── CinematicsTimelineEditorPanel.h/.cpp - FCinematicsTimelineEditorPanel / FEditorKeyframe
```

各 editor sample は `samples/<NN>_Hello<Name>/` に `main.cpp`、`CMakeLists.txt`、app/scene の分割実装を持つ。`samples/36_HelloFontEditor/` と `samples/37_HelloCinematicsEditor/` も実装済み。

---

## 3. editor_core 共通基盤 / Common Foundation

`editor_core` は **9 ヘッダー / 8 実装ファイル**からなる共通基盤。`FParticleEditorPanel`、`FHierarchyPanel`、`FInspectorPanel`、`FEditorToolbar` を含む現行の editor panel は `FEditorPanel` に統合済みである。`FAssetBrowser`、`FPropertyDrawerRegistry`、`FEditorCamera` などは workspace へ登録する panel ではなく、panel から利用する service/helper として独立している。

### 3.1 FEditorPanel — 抽象基底クラス

`src/gameframework/tools/editor_core/EditorPanel.h`

全エディタパネルの **ライフサイクル + 選択通知 + レイアウト永続化** を共通化する純粋抽象基底。

**主要 API**:

| メソッド | 種別 | 用途 |
|---|---|---|
| `const char* Title() const noexcept` | 純粋仮想 | ImGui::Begin に渡す window タイトル (リテラル) |
| `void DrawUI() noexcept` | 純粋仮想 | メインの ImGui 描画 (Begin/End は派生側責務) |
| `void OnInit(FEditorWorkspace& ws) noexcept` | virtual | Workspace 登録時に 1 度呼ばれる |
| `void OnShutdown() noexcept` | virtual | 登録解除時 / editor shutdown 時 |
| `void OnFrameBegin(f32 dt) noexcept` | virtual | UI 描画より前のフレーム頭で呼ばれる |
| `void OnSelectionChanged(FSelectionService& sel) noexcept` | virtual | Scene 内 `ANode` 選択変更通知 |
| `void OnAssetSelected(const char* asset_path) noexcept` | virtual | `FAssetBrowser` adapter 等からの file 選択通知 |
| `void OnSaveLayout(FEditorLayoutSerializer& out) noexcept` | virtual | panel 独自設定の保存予約 hook (serializer は前方宣言のみ) |
| `void OnLoadLayout(FEditorLayoutSerializer& in) noexcept` | virtual | panel 独自設定の復元予約 hook |
| `bool WantsFocus() const noexcept` | virtual | 起動時に Focus を奪うかのヒント (default false) |
| `bool IsVisible() / SetVisible(bool)` | concrete | panel 表示 toggle (close ボタン連動) |
| `FEditorWorkspace* Workspace() const` | concrete | OnInit で保存された Workspace ポインタ |

**使い方例**:

現行 `FModelViewerPanel` の override 宣言は次の形:

```cpp
class FModelViewerPanel : public acs::game::editor_core::FEditorPanel {
public:
    const char* Title() const noexcept override;
    void OnInit(
        acs::game::editor_core::FEditorWorkspace& workspace) noexcept override;
    void DrawUI() noexcept override;
    void OnAssetSelected(const char* asset_path) noexcept override;
};
```

`DrawUI()` の実装は `IsVisible()` を確認し、close button を使う場合は protected の `m_Visible` を `ImGui::Begin` へ渡す。

**設計選択**:
- ImGui::Begin/End は派生側責務 (ImGuiWindowFlags / dock target / MenuBar の有無が panel ごとに違うため基底で wrap しない)
- `OnSelectionChanged` (Node 系) と `OnAssetSelected` (file 系) を独立 hook にして、panel ごとに必要な方だけ override できるようにする
- 非コピー / 非ムーブ (panel は workspace と紐づく lifecycle)

### 3.2 FEditorWorkspace — 複数 panel の統括 hub

`src/gameframework/tools/editor_core/EditorWorkspace.h`

複数の FEditorPanel を **1 つの editor アプリケーション** としてまとめて配線する中央 hub。

**主要 API**:

| メソッド | 用途 |
|---|---|
| `void Init() / Shutdown()` | panel list と non-owning service 参照を初期化 / 破棄 |
| `void RegisterPanel(FEditorPanel*)` | 末尾追加 + OnInit 呼出 (二重登録は no-op) |
| `void UnregisterPanel(FEditorPanel*)` | 順序保存削除 + OnShutdown 呼出 |
| `u32 PanelCount() const` | 現在の登録 panel 数 |
| `FEditorPanel* GetPanelByIndex(u32)` | 登録順 (= dispatch 順) で取得 |
| `FEditorPanel* FindPanelByTitle(const char*)` | strcmp 完全一致検索 |
| `void TogglePanelVisible(const char*)` | Window メニュー連動 |
| `void TickAllPanels(f32 dt)` | OnFrameBegin → DockSpace → MenuBar → DrawUI |
| `void DrawDockSpace()` | ImGui::DockSpaceOverViewport (`IMGUI_HAS_DOCK` 未定義時は no-op) |
| `void DrawMenuBar()` | Window/Layout メニュー追加 |
| `void SaveLayout(const wchar_t* path)` | `.acslayout` 形式テキスト保存 |
| `void LoadLayout(const wchar_t* path)` | `.acslayout` 復元 |
| `TrySaveLayout / TryLoadLayout / TryParseLayoutText` | checked atomic save / transactional load / 長さ付き parse |
| `void SetSelectionService(FSelectionService*)` | 注入 (non-owning) |
| `FSelectionService* GetSelectionService() const` | 注入済み service を取得 |
| `void BroadcastSelectionChanged()` | 全 panel に OnSelectionChanged を fan-out |
| `void BroadcastAssetSelected(const char* path)` | 全 panel に OnAssetSelected を fan-out |

**使い方例**:

```cpp
acs::game::editor_core::FEditorWorkspace ws;
ws.Init();
ws.SetSelectionService(&selection);
ws.RegisterPanel(&hierarchy_panel);
ws.RegisterPanel(&inspector_panel);
ws.RegisterPanel(&model_viewer);
ws.LoadLayout(L"data/editor/last.acslayout");

// 毎フレーム:
ws.TickAllPanels(dt);

// 終了時:
ws.SaveLayout(L"data/editor/last.acslayout");
ws.Shutdown();
```

**公開定数**:
- `kLayoutMagic = "ACS_EDLAYOUT"`, `kLayoutVersion = 1u`
- `kMaxPanels = 32u` (上限到達時は警告を記録して登録を無視)
- `kMaxLayoutBytes = 4 MiB`, `kMaxIniBytes = 2 MiB`
- `kMaxLayoutLineBytes = 255u`, `kMaxLayoutLines = 4096u`
- `kMaxPanelTitleBytes = 127u`, `kMaxPersistencePathChars = 1023u`

**`.acslayout` フォーマット** (テキスト):
```
ACS_EDLAYOUT 1
IMGUI_INI <byte_size>
<raw ini bytes>
PANEL <title> <visible:0/1> <dock_target:0/1>
...
```

> **v1 title contract（解消済み）**: `PANEL` 行は右端 2 token を
> `visible` / `dock_target` として読み、その手前を title 全体として扱う。このため
> `"Particle Editor"` / `"Scene Hierarchy"` / `"Model Viewport"` のような内部 ASCII
> space を含む title も version 1 のまま round-trip でき、従来の空白なし title とも
> 互換である。空 title、先頭・末尾 space、制御文字、非 ASCII、127 byte 超過は
> `InvalidTitle` / `TitleTooLong` として保存・parse の双方で拒否し、parse 失敗時は
> live panel state を変更しない。

### 3.3 FUndoStack + FEditorCommand — undo/redo 中央ハブ

`src/gameframework/tools/editor_core/UndoStack.h`, `EditorCommand.h`

全 editor が共有する undo/redo の中央ハブ。Command パターン (GoF) ベース。

**FEditorCommand API** (純粋抽象):

| メソッド | 種別 | 用途 |
|---|---|---|
| `void Execute() noexcept` | 純粋仮想 | "Do" を実行 (Push と Redo の両方から呼ばれる) |
| `void Undo() noexcept` | 純粋仮想 | Execute の逆操作 |
| `const char* Description() const noexcept` | 純粋仮想 | UI 表示用短文 ("Move Node" 等) |
| `const void* Kind() const noexcept` | virtual | RTTI 抜き型識別 tag (default nullptr) |
| `bool CanMerge(const FEditorCommand& next) const noexcept` | virtual | 連続 drag 等を 1 件にまとめるか (default false) |
| `void MergeWith(const FEditorCommand& next) noexcept` | virtual | CanMerge true の後に呼ばれる (new 値を取り込み) |

**FUndoStack API**:

| メソッド | 用途 |
|---|---|
| `void Init(u32 max_history = 64)` | 初期化 (history 上限再設定可) |
| `void Push(TUniquePtr<FEditorCommand> command)` | 推奨。確保元ごと所有権を渡す + Execute + Merge + Redo clear |
| `void Push(FEditorCommand* command)` | default allocator 由来 raw pointer の互換 overload |
| `void Push(FEditorCommand*, FAllocator&)` | custom allocator 由来 raw pointer の明示 overload |
| `bool Undo() / bool Redo()` | 1 件巻き戻し / やり直し |
| `bool CanUndo() / CanRedo()` | UI MenuItem の enable 判定用 |
| `u32 UndoCount() / RedoCount()` | history 件数 |
| `const char* UndoDescription() / RedoDescription()` | top の Description (空時 "") |
| `void SetOnExecutedCallback(cb, user)` | Push/Undo/Redo 後の副反応用 |

**使い方例**:

```cpp
acs::game::editor_core::FUndoStack stack;
stack.Init(64);

auto command =
    acs::MakeUnique<acs::game::editor_core::FMoveNodeCommand>(
        &node, old_pos, new_pos);
stack.Push(acs::Move(command));

if (ImGui::MenuItem("Undo", "Ctrl+Z", false, stack.CanUndo())) {
    stack.Undo();
}
```

**bundled 派生**: `FMoveNodeCommand` を `EditorCommand.h` 末尾に教科書的サンプルとして同梱 (header-only)。同 target への連続 drag を 1 件に merge する CanMerge / MergeWith の実装例。

### 3.4 FEditorCamera — 2D/3D 統合カメラ

`src/gameframework/tools/editor_core/EditorCamera.h`

ModelViewer (3D) / LevelEditor (2D top-down) / TilemapEditor などが共有する **カメラコントローラ**。1 クラス内 mode フラグで 2D/3D を統合。

**主要 API**:

| メソッド | 用途 |
|---|---|
| `void Init(EEditorCameraMode mode)` | Mode2D / Mode3D で完全初期化 |
| `void SetMode(EEditorCameraMode)` | mode 切替 (state は保持) |
| `void HandleMouseInput(mouse_delta, lmb, rmb, mmb, wheel)` | 操作系の集約エントリポイント |
| `void Pan(FVec2 screen_delta)` | 2D は world 平行移動、3D は orbit target 移動 |
| `void Orbit(f32 yaw_delta, f32 pitch_delta)` | 3D 専用、pitch ±89° clamp |
| `void Dolly(f32 delta)` | 3D = distance / 2D = zoom_2d |
| `void Reset()` | 初期状態に戻す |
| `void FrameToBoundingSphere(FVec3 center, f32 radius)` | 選択へ寄せる |
| `void FrameToBoundingBox2D(FVec2 min, FVec2 max)` | 2D 専用 |
| `FMat4 ViewMatrix() / ProjectionMatrix(...)` | LookAt + perspective/ortho |
| `void Tick(f32 dt)` | smooth target follow (`FCamera2D` と同じ指数補間) |

**操作マッピング**:
- 3D: LMB drag = orbit / MMB drag = pan / RMB drag = orbit (Maya 風代替) / wheel = dolly
- 2D: LMB drag = pan / MMB drag = pan / wheel = zoom

**使い方例**:

```cpp
acs::game::editor_core::FEditorCamera cam;
cam.Init(acs::game::editor_core::EEditorCameraMode::Mode3D);

cam.HandleMouseInput(mouse_delta, lmb, rmb, mmb, wheel);
cam.Tick(dt);
acs::FMat4 view = cam.ViewMatrix();
acs::FMat4 proj = cam.ProjectionMatrix(aspect, 0.1f, 1000.0f);
cam.FrameToBoundingSphere(node_center, node_radius);
```

### 3.5 FAssetBrowser — file tree + drag source

`src/gameframework/tools/editor_core/AssetBrowser.h`

プロジェクト `assets/` 配下のファイルツリーを ImGui で参照 + 各 panel へ drag-drop で path を供給する Unity Project Window 相当。

**主要 API**:

| メソッド | 用途 |
|---|---|
| `void Init(const wchar_t* root)` | assets/ ルートを記録 + 初回 Refresh |
| `void Shutdown()` | pool / callback 解放 |
| `void Refresh()` | current_directory 配下の rescan |
| `void DrawUI()` | "Asset Browser" 1 window (左 tree + 右 list) |
| `u32 EntryCount() / GetEntry(u32)` | 列挙結果アクセサ |
| `const wchar_t* CurrentDirectory()` | 現在表示中ディレクトリ |
| `void SetCurrentDirectory(const wchar_t*)` | 切替 + 自動 Refresh |
| `const wchar_t* SelectedAssetPath() / EAssetKind SelectedAssetKind()` | 現選択 |
| `void SetOnAssetSelectedCallback(cb, user)` | 選択変更通知 |
| `void SetOnAssetDoubleClickedCallback(cb, user)` | ダブルクリック (= Open) |
| `void SetFilterByKind(EAssetKind)` | 種別フィルタ |
| `static EAssetKind ClassifyByExtension(const wchar_t*)` | 拡張子判定ヘルパ |

**EAssetKind 拡張子マップ**:

| 種別 | 拡張子 |
|---|---|
| Texture | .png .jpg .jpeg .tga .bmp .dds .ktx .hdr |
| Mesh | .mdl .fbx .gltf .glb .obj |
| Font | .ttf .otf |
| Audio | .wav .ogg .mp3 .flac |
| Material | .mat .material |
| Particle | .fx .particle |
| Animation | .anim |
| BehaviorTree | .bt |
| Tilemap | .tilemap .tmx |
| Prefab | .prefab |
| Cinematic | .cine |
| Scene | .scene |

`Unknown` は null / 空 path または未認識拡張子、`Other` は利用側拡張用の予約値で、
現行 `ClassifyByExtension()` はどの拡張子も `Other` へ分類しない。

**Drag payload**: identifier `"ASSET_PATH"` (32 文字以内) の payload buffer に
`const wchar_t*` pointer 値を格納する。安全な受け側は
`ImGui::AcceptDragDropPayload("ASSET_PATH")` で取得し、サイズ検証後に `memcpy`
相当で pointer 値を復元する。現行受信側ごとの差は §5.1 に記す。

### 3.6 FPropertyDrawerRegistry — field 型 → drawer 登録レジストリ

`src/gameframework/tools/editor_core/PropertyDrawer.h`

カスタム field drawer の登録レジストリ。`FInspectorPanel` が扱う `EFieldKind` の既定表示を超えた拡張型 (Curve / Gradient / AssetPath / NodeIdSelector 等) を後付け可能にする。

**主要 API**:

| メソッド | 用途 |
|---|---|
| `void Init()` | bundled drawer 9 種を自動登録 |
| `void Shutdown() / ClearAll()` | 全登録破棄 |
| `void RegisterDrawer(const char* type_name, DrawerFn fn)` | 後勝ち上書き |
| `void UnregisterDrawer(const char* type_name)` | 解除 |
| `bool HasDrawer(const char* type_name)` | 登録済みか |
| `bool DrawProperty(const char* type_name, const FPropertyContext& ctx)` | 該当 drawer 呼出 (失敗時 false でフォールバック) |
| `u32 DrawerCount() / const char* DrawerName(u32)` | イントロスペクション |

**bundled 9 種**:
- `F32Slider` / `Vec2Drag` / `Vec3Drag` / `Vec4Drag`
- `ColorRGB` / `ColorRGBA`
- `AssetPath` (`"ASSET_PATH"` payload 受信対応)
- `EnumCombo` / `TextInput`

**FPropertyContext** (集約パラメータ):

```cpp
struct FPropertyContext {
    void*       data_ptr    = nullptr;  // 編集対象 (drawer がキャスト)
    const char* label       = nullptr;  // ImGui ラベル
    const char* tooltip     = nullptr;  // hover tooltip
    f32         min_value   = 0.0f;     // F32Slider 等で使う
    f32         max_value   = 1.0f;
    const char* enum_values = nullptr;  // "Item0\0Item1\0...\0"
    u32         enum_count  = 0;
    bool*       out_changed = nullptr;  // drawer が dirty を書く出口
};
```

**使い方例**:

```cpp
static void DrawEmitter(
    const acs::game::editor_core::FPropertyContext& ctx) noexcept {
    auto* def = static_cast<acs::game::FParticleEmitterDef*>(ctx.data_ptr);
    const bool changed =
        ImGui::DragFloat(ctx.label, &def->emit_rate_per_sec);
    if (ctx.out_changed) *ctx.out_changed = changed;
}

acs::game::editor_core::FPropertyDrawerRegistry reg;
reg.Init();
reg.RegisterDrawer("FParticleEmitterDef", &DrawEmitter);

acs::game::FParticleEmitterDef emitter {};
bool changed = false;
acs::game::editor_core::FPropertyContext ctx {
    &emitter, "Emit rate", nullptr, 0.0f, 1000.0f, nullptr, 0u, &changed
};
if (!reg.DrawProperty("FParticleEmitterDef", ctx)) {
    // 未登録 → EFieldKind switch にフォールバック
}
```

### 3.7 FEditorGizmo — Translate/Rotate/Scale handle

`src/gameframework/tools/editor_core/EditorGizmo.h`

選択対象の position / Euler rotation / scale を viewport 上で直接ドラッグ操作するハンドル。`ANode` へ適用する場合は、caller が `FTransform3D` / quaternion との変換境界を受け持つ。

**主要 API**:

| メソッド | 用途 |
|---|---|
| `void Init() / Shutdown()` | state を default に |
| `void SetMode(EGizmoMode)` | None / Translate / Rotate / Scale |
| `void SetSpace(EGizmoSpace)` | World / Local |
| `void SetSnapTranslate(f32 step)` | snap step (0 で無効) |
| `void SetSnapRotate(f32 step_deg)` | 同上、度数 |
| `void SetSnapScale(f32 step)` | 同上、倍率 |
| `void ProcessInput(ray_origin, ray_dir, lmb_down, lmb_held, lmb_up)` | drag 開始/継続/終了の遷移管理 |
| `bool Manipulate(FVec3& pos, FVec3& rot, FVec3& scl)` | drag 中なら inout を in-place 更新、true 戻り |
| `void DrawGizmo(FDebugDraw& dd, pos, rot, scl)` | FDebugDraw 経由で軸 + ハンドル描画 |
| `bool IsDragging() / EGizmoAxis HotAxis()` | 状態問い合わせ |
| `void SetOnManipulateCallback(cb, user)` | drag 終了時 1 度発火 (FUndoStack push 用) |

**enum**:
- `EGizmoMode`: None / Translate / Rotate / Scale
- `EGizmoSpace`: World / Local
- `EGizmoAxis`: None_ / X / Y / Z / XY / XZ / YZ / ScreenAlign

**使い方例**:

```cpp
acs::game::editor_core::FEditorGizmo gizmo;
gizmo.Init();
gizmo.SetMode(acs::game::editor_core::EGizmoMode::Translate);
gizmo.SetSnapTranslate(0.5f);

gizmo.ProcessInput(ray_o, ray_d, lmb_down, lmb_held, lmb_up);
acs::FVec3 position = edited_position;
acs::FVec3 euler_rotation = edited_euler_rotation;
acs::FVec3 scale = edited_scale;
const bool changed =
    gizmo.Manipulate(position, euler_rotation, scale);
gizmo.DrawGizmo(debug_draw, position, euler_rotation, scale);
```

### 3.8 FEditorTheme — 組み込み preset 5 種 + Custom

`src/gameframework/tools/editor_core/EditorTheme.h`

ImGui スタイル統一テーマ管理。`.acstheme` テキスト形式で保存・復元可能。

**主要 API**:

| メソッド | 用途 |
|---|---|
| `void Init()` | default = Dark preset を ImGui::GetStyle() に流す |
| `void ApplyPreset(EEditorThemePreset)` | preset 切替 + 即時 ImGui 反映 |
| `void SetCustomColors(const FEditorThemeColors&)` | 任意パレット (preset → Custom 自動切替) |
| `const FEditorThemeColors& Colors() const` | 現パレット |
| `void SetFontScale(f32)` | ImGuiIO::FontGlobalScale 書換 (clamp 4.0) |
| `void SetRoundedCorners(f32)` | Frame/Window/Popup/Grab/Tab/Scrollbar 統一 radius |
| `void SetSpacing(f32 item_spacing_y)` | ItemSpacing.y (x は 0.5x 連動) |
| `void DrawThemeSettingsUI()` | "Theme Settings" 独立 window 描画 |
| `void SaveTheme(const wchar_t*) / LoadTheme(const wchar_t*)` | `.acstheme` |
| `TrySaveTheme / TryLoadTheme / TryParseThemeText` | checked atomic save / transactional load / 長さ付き parse |

**組み込み preset 5 種 + Custom**:

| preset | 想定用途 |
|---|---|
| Dark | 標準 dark grey (ImGui StyleColorsDark 寄り、若干暖色) |
| DarkBlue | VS Code Dark+ 風 (#1F232C / #007ACC 系) |
| Light | 明るい背景 (屋外 / プロジェクタ向け) |
| HighContrast | 黒 / 白 / 黄 (#FFD700) の三色設計 (`FAccessibilityProfile` 連動予定) |
| Sepia | 焼け紙風暖色 (長時間作業の眼精疲労低減) |
| Custom | SetCustomColors() で渡された任意パレット |

**FEditorThemeColors** (13 フィールド): `window_bg / title_bg / button_bg / button_hover / button_active / frame_bg / text / text_disabled / border / separator / accent / warning / error`

---

## 4. 9 種の editor sample 一覧 / Editor Sample Catalogue

| Sample # | Editor 名 | 対象 data | Phase | 主要 panel ファイル |
|---|---|---|---|---|
| **29** | ParticleEditor | `FParticleEmitterDef` | 19b | `fxedit/ParticleEditorPanel.h` |
| **30** | SceneInspector | `ANode` + `FInspectorSeam` | 20 | `inspector/HierarchyPanel.h` + `InspectorPanel.h` + `EditorToolbar.h` |
| **31** | ModelViewer | mesh + material + anim | 21b | `modelview/ModelViewerPanel.h` + 3 つの sub panel |
| **32** | AnimCurveEditor | `FAnimationCurve` | 22 | `animcurve/AnimCurveEditorPanel.h` |
| **33** | BehaviorTreeEditor | `FBehaviorTree` | 22 | `btedit/BehaviorTreeEditorPanel.h` |
| **34** | LevelEditor | `FTilemap` | 22 | `leveledit/LevelEditorPanel.h` |
| **35** | SpriteAtlasEditor | `FSpritePack` | 22 | `spriteatlas/SpriteAtlasEditorPanel.h` |
| **36** | FontEditor | `FFontFaceInfo` list | 23 | `fontedit/FontEditorPanel.h` |
| **37** | CinematicsEditor | `FCinematicsDirector` + `FEditorKeyframe` | 23 | `cinetimeline/CinematicsTimelineEditorPanel.h` |

### 4.1 Sample 29 — ParticleEditor (Phase 19b)

**path**: `samples/29_HelloParticleEditor/main.cpp`

- **編集対象**: `acs::game::FParticleEmitterDef` (gameframework/ParticleEffectSystem.h)
- **UI レイアウト**: 左 emitter list + 右 property pane の 2 カラム
- **編集パラメータ**: lifetime_sec / emit_rate_per_sec / burst_count / speed_min/max / scale_start/end / spread_radians / gravity / color_start/end
- **永続化**: `FFxeditSerializer` で `.fxedit` テキスト形式 (`ACS_FXEDIT 1`)。`TryParseText` / `TryLoad` / `TrySave` は上限付き parse、transactional load、atomic save を返す。
- **特徴**: `FParticleEditorPanel` は `FEditorPanel` を継承し、workspace から共通 dispatch できる。
- **callback**: `SaveCallback` / `LoadCallback` で外部に save/load 委譲

### 4.2 Sample 30 — SceneInspector (Phase 20)

**path**: `samples/30_HelloSceneInspector/main.cpp`

- **4 component 構成** (3 つの panel + 1 service):
  - `FHierarchyPanel` — `ANode` ツリー、reparent (drag drop payload `"HIER_NODE_PTR"`)、Delete/Duplicate context menu
  - `FInspectorPanel` — `FInspectorSeam` 経由で `FInspectableField` を編集。`EFieldKind` は 10 種で、現行 panel は `ObjectRef` 以外の 9 種を描画する (`ObjectRef` は未対応表示)
  - `FEditorToolbar` — Play/Pause/Step/Save/DebugOverlay の 5 ボタン
  - `FSelectionService` — 選択 FNodeId の集中点 (callback 複数登録、callback hub)
- **特徴**: 3 panel はすべて `FEditorPanel` を継承する。`FSelectionService` は独立 service。
- **連携**: `FHierarchyPanel` ⇆ `FInspectorPanel` は `FSelectionService` 経由で疎結合
- **Drag drop**: `"HIER_NODE_PTR"` payload で ANode* 直渡し

### 4.3 Sample 31 — ModelViewer (Phase 21b)

**path**: `samples/31_HelloModelViewer/main.cpp`

- **4 panel 構成** (全て `FEditorPanel` 継承):
  - `FModelViewerPanel` — 3D viewport + Lighting (sun dir/color + IBL toggle + tonemap mode) + Background + grid/bone toggle
  - `FModelInspectorPanel` — mesh 統計 (vertex/triangle/submesh/material/bone/animation count + bounding sphere) を read-only 表示
  - `FModelMaterialPanel` — material slot と `FMaterialOverride` を編集
  - `FModelAnimationPanel` — animation clip 切替 + Play/Pause/Stop + Time slider + Speed + Loop + BlendWeight
- **特徴**: 共通基盤 (`FEditorPanel` / `FEditorCamera` / `FEditorWorkspace` / `FAssetBrowser`) の dogfood sample
- **連携**: `FModelViewerPanel::OnAssetSelected(const char*)` は narrow path を受け、対応 asset を `LoadModelAsset(const wchar_t*)` へ渡す境界を持つ。
- **camera**: `FModelViewerPanel` が `FEditorCamera` (Mode3D orbit) を内包する Unity SceneView 風モデル

### 4.4 Sample 32 — AnimCurveEditor (Phase 22)

**path**: `samples/32_HelloAnimCurveEditor/main.cpp`

- **編集対象**: `acs::game::FAnimationCurve` (Hermite/Linear/Step + Pre/Post `EWrapMode`)
- **UI**: 単一 window 内に toolbar (Interpolation Combo / WrapMode Combo / Add Key / Clear / Easing Preset Combo + Apply / Eval preview slider) + canvas (1024-sample 線描画)
- **操作**:
  - 各 key を丸 marker で描画 + drag で time/value 編集
  - Hermite key は in/out tangent を小さい handle として描画 (固定 30px 長) + drag で接線編集
  - 右クリック context menu (Add key here / Delete selected)
  - Easing Preset Combo から `EEasingType` の全 33 種を選択し、Apply で `[0,1]` を
    65 samples の編集可能な linear key 列へ変換。適用成功時だけ既存 curve を置換
- **callback**: `CurveChangeCallback` (drag 中は連続発火を避け、drag end で 1 度発火)
- **公開定数**: `kCurveSampleCount = 1024u`, `kNoKeySelected = -1`,
  `FAnimationCurve::kMaxEasingPresetSamples = 4096u`

### 4.5 Sample 33 — BehaviorTreeEditor (Phase 22)

**path**: `samples/33_HelloBehaviorTreeEditor/main.cpp`

- **編集対象**: `acs::game::FBehaviorTree` (`FBtSelector` / `FBtSequence` / `FBtAction`)
- **2 表示 mode**: metadata mirror の tree/debug view と、編集可能な graph view
- **メタミラー方式**: `FBehaviorTree` 本体の private storage に panel から触れないため、user が `AddNode(kind, name, parent_id)` で「親 id・kind・表示名」を panel に push する別ミラー。`SetNodeStatus(node_id, EBtStatus)` で `FBtAction` の Fn から status を push。
- **graph authoring**: Selector / Sequence / Action / Decorator / Task の追加・削除、node 配置、condition/compare、action/condition registry、dynamic blackboard 編集を提供する。`TickGraph()` と `BuildRuntimeTree()` で editor graph を実行/runtime tree 化できる。
- **安全性**: snapshot 型 undo/redo、`ACSBT 4` の上限付き checked parse、atomic save、transactional load を実装する。legacy v1〜v3 は読み込み互換。
- **UI**:
  - toolbar: Reset / Step / Continuous (autorun) toggle / Active / Step counter
  - 上部: Tick history (60 frame ring graph、PlotLines)
  - 左: TreeNode (色分け Success=緑/Failure=赤/Running=黄)
  - 右: Node Inspector (Name / Kind / Id / Parent / Children / Last Status)
- **公開定数**: `kHistorySize = 60u`, `kInvalidId = 0xFFFFFFFFu`, `kMaxNodes = 128u`, `kMaxGraphTextBytes = 256 KiB`, `kMaxGraphDepth = 64u`
- **callback**: `StepCallback` (登録時 panel は `tree->Tick` を直接呼ばず callback に委譲 = blackboard を渡す自由を user に与える)

### 4.6 Sample 34 — LevelEditor (Phase 22)

**path**: `samples/34_HelloLevelEditor/main.cpp`

- **編集対象**: `acs::game::FTilemap` (multi-layer u16 FTileId grid)
- **4 ブラシ** (`EBrushKind`):
  - Paint (塗り、drag 対応)
  - Erase (FTileId{0} 強制、drag 対応)
  - Fill (flood-fill、連結成分塗替、click 単発、`kFloodFillMaxCells = 4096` 打切)
  - Pick (スポイト、クリック位置の tile id を current にコピー)
- **toolbar**: Brush kind / Active layer dropdown / Tile id picker (DragInt 0-1023) / Show grid toggle / Snap-to-grid toggle
- **camera**: `FEditorCamera` Mode2D (pan/zoom)
- **viewport**: ImDrawList で色付き矩形 placeholder (実 texture atlas 表示は future)
- **公開定数**: `kTileIdMax = 1023`, `kFloodFillMaxCells = 4096`

### 4.7 Sample 35 — SpriteAtlasEditor (Phase 22)

**path**: `samples/35_HelloSpriteAtlasEditor/main.cpp`

- **編集対象**: `acs::game::FSpritePack` (atlas メタ + 名前付き frame 矩形リスト)
- **UI**: toolbar (New/Delete/Pivot プリセット) + 中央 viewport (atlas placeholder + 矩形 overlay) + 左 frame list + 右 inspector
- **操作**:
  - frame rect の resize: 4 corner + 4 edge の 8 handle (mouse drag)。`EFrameHandle` は `.cpp` 内の実装詳細で、公開 API ではない。
  - inspector に SliderInt(x/y/w/h) で精密入力
  - Pivot toggle: Center / TopLeft / Custom (`EPivotPreset`)
- **特徴**: 現在は atlas texture 実描画なし (ImTextureID + DX12 descriptor heap 統合は future)
- **永続化**: Sample 35 側で `.acsatlas` stub menu (serializer は未実装)

### 4.8 Sample 36/37 — FontEditor / CinematicsEditor

- **FontEditor**: `samples/36_HelloFontEditor/`、`src/gameframework/tools/fontedit/`
  - `FFontEditorPanel` が `TArray<FFontFaceInfo>` を所有し、font path、family、base size、文字範囲、fallback index、MSDF フラグを編集する。
  - face の追加/削除/並べ替え、preview text、8〜96 px の preview size を提供する。
- **CinematicsEditor**: `samples/37_HelloCinematicsEditor/`、`src/gameframework/tools/cinetimeline/`
  - `FCinematicsTimelineEditorPanel` が `FCinematicsDirector` を non-owning で受け、`FEditorKeyframe` を timeline 上で編集して runtime keyframe へ bake する。
  - `ETimelineKeyKind` は CameraCut / FadeColor / TimeScale / SpawnEffect / TriggerCallback の 5 種。Play/Pause/Stop、scrub、marker drag を実装済み。
  - file save/load は sample menu 上の stub で、timeline serializer は未実装。

---

## 5. 設計パターン / Design Patterns

9 つの editor sample 全体に通底するパターンを抽出。新規 editor を追加する際に守るべき規約集。

### 5.1 Drag-drop payload identifier 統一

ImGui の drag-drop payload identifier は 32 文字以内が仕様上限。ACS editor 群では以下の 2 種を統一定数として使う:

| identifier | payload data | 送信側 | 受信側 |
|---|---|---|---|
| `"ASSET_PATH"` | `const wchar_t*` pointer を格納した payload | `FAssetBrowser` | `FModelViewerPanel` viewport |
| `"HIER_NODE_PTR"` | `ANode*` pointer を格納した payload | `FHierarchyPanel` (drag source) | `FHierarchyPanel` (drop target、Reparent 用) |

両者とも payload buffer に pointer 値をコピーする。asset path の寿命は
`FAssetBrowser::Refresh()` まで、node の寿命は scene graph 側が保証する。
`FHierarchyPanel` は payload size を検証して `memcpy` 相当の byte copy で pointer を
復元する。`FModelViewerPanel` の現行 drop target は size/alignment を仮定して直接
dereference するため、同じ size 検証 + byte copy へ揃える余地がある。

> **既知の境界**: `FPropertyDrawerRegistry` の AssetPath drawer も `"ASSET_PATH"` を
> 受けるが、現行実装は payload data を narrow 文字列バイト列として解釈する。
> `FAssetBrowser` の `const wchar_t*` pointer payload を直接渡してはいけない。drawer
> へ接続する場合は、終端 NUL と `kTextInputBufferSize = 256u` の上限を保証した UTF-8
> 文字列 payload へ正規化する adapter が必要である。

**公開定数の場所**:
- `FAssetBrowser::kDragDropPayloadId = "ASSET_PATH"`
- `FPropertyDrawerRegistry::kAssetPathPayloadId = "ASSET_PATH"` (同値で AssetPath drawer 側で受け取り)
- `FHierarchyPanel::kDragDropId = "HIER_NODE_PTR"`

### 5.2 FSelectionService 駆動の panel 間同期

1 panel で選択 → 他 panel が `OnSelectionChanged` 通知で同期する `FSelectionService` ハブパターン:

```
FHierarchyPanel::SelectNode(node)
       |
       v
FSelectionService::SelectNode(FNodeId id)
       |
       +-- 全 callback を一斉発火 (from, to) ペア
       |
       v
FInspectorPanel が購読していた callback → 自身の表示を更新
```

**特徴**:
- `FSelectionService::RegisterCallback(cb, user)` で複数 panel が購読可
- `from / to` ペアで渡されるため購読側で diff 取得が容易
- panel 側は FSelectionService と疎結合 (forward decl のみで OK)

**`FEditorWorkspace::BroadcastSelectionChanged()`** は別の API で、これは workspace が登録 panel 全部の `OnSelectionChanged(FSelectionService&)` メソッドを呼ぶ fan-out。FSelectionService の callback hub と併用可能 (両方使う実装パターンも OK)。

### 5.3 FAssetBrowser → FEditorWorkspace の asset 選択境界

```
FAssetBrowser でユーザがファイル選択
       |
       v
AssetSelectedCallback(void*, const wchar_t*, EAssetKind) が呼ばれる
       |
       v
adapter が UTF-16 path を UTF-8 path へ変換
       |
       v
FEditorWorkspace::BroadcastAssetSelected(const char*) を呼ぶ
       |
       +-- FModelViewerPanel::OnAssetSelected("models/hero.mdl")
       |     → 拡張子フィルタ → LoadModelAsset()
       +-- 他 panel は OnAssetSelected を override してなければ no-op
```

`FAssetBrowser` の callback は wide path、`FEditorWorkspace` の broadcast は narrow path であり、型が異なる。sample 31 は現在 callback/broadcast を配線せず、`FAssetBrowser` を直接描画している。配線する場合は変換失敗と切り詰めを検査する adapter を置き、同期 callback 中だけ path を参照する。保持が必要な panel はコピーする (`FModelViewerPanel` は `kMaxAssetPathChars = 512u` の wide buffer を持つ)。

### 5.4 Save/Load フォーマット統一

永続化は用途に応じて text と固定 wire binary を使い分ける。UI が存在しても serializer が未配線の sample があるため、次表では実装状態を明示する。

| editor / service | 拡張子 | magic / version | 現在の状態 |
|---|---|---|---|
| ParticleEditor (`FFxeditSerializer`) | `.fxedit` | `ACS_FXEDIT 1` | text、checked parse/load/save 実装済み |
| AnimationCurve (`FAnimationCurveArchive`) | caller 指定 | `ACSCURV\0` | fixed little-endian binary + CRC。sample 32 の `.acscurve` menu は未配線 |
| BehaviorTreeEditor | `.btg` | `ACSBT 4` | canonical text、checked atomic save / transactional load 実装済み |
| LevelEditor | `.acstilemap` | — | sample 34 menu は stub |
| SpriteAtlasEditor | `.acsatlas` | — | sample 35 menu は stub |
| CinematicsEditor | `.acscinetimeline` | — | sample 37 menu は stub |
| FEditorTheme | `.acstheme` | `ACS_THEME 1` | text、checked atomic save / transactional load 実装済み |
| FEditorWorkspace | `.acslayout` | `ACS_EDLAYOUT 1` | ImGui ini + panel visible/dock_target、内部 ASCII space 入り title 対応、checked atomic save / transactional load 実装済み |

checked API は入力サイズ・行長・件数を上限付きで検証し、parse 完了前に live state を変更しない。保存は一時ファイルを完成させてから置換する。legacy の `void` / `bool` wrapper は互換用であり、診断が必要な呼び出し側は result struct を返す `Try*` API を使う。

### 5.5 callback 駆動 (`SetOn*Callback` / `RegisterCallback`)

外部へ処理を委譲する panel/service は C-style 関数ポインタ + `void* user` を使う。callback alias の先頭文字は固定せず、型である class/struct/enum の `F` / `E` 規約と分けて扱う。`std::function` は使わない。

**典型 signature**:
```cpp
using ManipulateCallback =
    void (*)(void* user,
             acs::game::editor_core::EGizmoMode mode,
             acs::FVec3 delta) noexcept;

gizmo.SetOnManipulateCallback(&OnGizmoManipulated, &editor_state);
```

**hookable 場所** (各 panel):

| panel | callback |
|---|---|
| FParticleEditorPanel | `SetSaveCallback` / `SetLoadCallback` |
| FInspectorPanel | `SetOnFieldChangeCallback` (`FNodeId`, field name, `EFieldKind`) |
| FHierarchyPanel | `SetOnNodeRightClickCallback` (`ANode*`) |
| FEditorToolbar | `SetOnSaveSceneCallback` |
| FSelectionService | `RegisterCallback` / `UnregisterCallback` (`FNodeId` from/to) |
| FAssetBrowser | `SetOnAssetSelectedCallback` / `SetOnAssetDoubleClickedCallback` |
| FUndoStack | CommandExecutedCallback (const FEditorCommand*, bool is_redo) |
| FEditorGizmo | ManipulateCallback (EGizmoMode, FVec3 delta) |
| FModelAnimationPanel | `SetOnFrameCallback` (u32 clip_index, f32 time_sec) |
| FModelMaterialPanel | `SetOnMaterialChangeCallback` (u32 slot, const FMaterialOverride&) |
| FAnimCurveEditorPanel | `SetOnChangeCallback` (FAnimationCurve*) |
| FBehaviorTreeEditorPanel | `SetOnStepCallback` (FBehaviorTree*, f32 dt) |

`FUndoStack` 連携も同じ pattern で書ける。例えば `MakeUnique<FMoveNodeCommand>(...)` で生成し、`undo_stack.Push(Move(command))` へ所有権を渡す。

### 5.6 panel API 統一規約

全 concrete panel は `FEditorPanel` を継承し、`Title()` / `DrawUI()` と lifecycle hook を共通 contract にする。多くの panel は subsystem state 用の `Init()` / `Shutdown()` も持つ。

| API | 用途 |
|---|---|
| `Title() / DrawUI()` | `FEditorPanel` の必須 override |
| `OnInit / OnShutdown / OnFrameBegin` | workspace lifecycle hook |
| `Init() / Shutdown()` | concrete panel 固有 state の初期化・解放 |
| 非コピー / 非ムーブ | 内部 TArray/callback 状態の所有を曖昧にしない |
| selection API | 対象に応じて `SelectedIndex` / `SelectedFrameIndex` / `SelectedNodeId` 等。index 系は通常 `i32` の `-1` を未選択に使う |
| callback setter / register | `nullptr` または対応 unregister API で解除 |

---

## 6. 共通制約 / Cross-cutting Constraints

ACS 全体規約 (詳細は `docs/StyleGuide.md`) を editor 文脈で再確認:

### 6.1 言語制約 (5 不変条件、StyleGuide §1)

- **STL 不使用**: `<vector>` / `<string>` / `<unordered_map>` / `<memory>` / `<functional>` 全部禁止。代替: `acs::TArray<T>` / `acs::FString` (使う場合) / `acs::THashMap<K,V>` / `acs::TUniquePtr<T>` / 関数ポインタ + `void* user`。
- **`<string>` 禁止**: editor 内の文字列は `const char*` (リテラル想定) または `wchar_t[N]` 固定長バッファ (`kMaxPathChars = 512u` 等)。
- **No exceptions**: `throw` / `try` / `catch` 禁止、全関数 `noexcept`。エラーは `TResult<T, FErrorCode>` または silent no-op + `ACS_LOG_WARN`。
- **No RTTI**: `dynamic_cast` / `typeid` 禁止。型識別は `Kind()` で static アドレスを返す idiom (例: `FEditorCommand::Kind()`) または `EBtKind` のような enum を使う。
- **raw callback**: `using Cb = void (*)(void* user, /* payload */) noexcept;`。alias 名の先頭文字は固定しない。

### 6.2 ImGui include の局所化

`#include <imgui.h>` は **.cpp 側のみ**。ヘッダから ImGui 依存を漏らさない:

```cpp
// ParticleEditorPanel.h
#pragma once
#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/ParticleEffectSystem.h"
#include "gameframework/tools/editor_core/EditorPanel.h"
// ↑ ImGui ヘッダは include しない

namespace acs::game::fxedit {

class FParticleEditorPanel
    : public acs::game::editor_core::FEditorPanel {
public:
    void DrawUI() noexcept override;
};

} // namespace acs::game::fxedit
```

理由: editor 上位レイヤから panel を include しても include order が壊れないようにする。`FEditorTheme` が `acs::FVec4` ベースで `FEditorThemeColors` を持つのも同じ理由。

### 6.3 非コピー / 非ムーブ

全 panel + service クラスは非コピー / 非ムーブ:

```cpp
class FFontEditorPanel
    : public acs::game::editor_core::FEditorPanel {
public:
    FFontEditorPanel() noexcept = default;
    ~FFontEditorPanel() noexcept override = default;
    FFontEditorPanel(const FFontEditorPanel&)            = delete;
    FFontEditorPanel& operator=(const FFontEditorPanel&) = delete;
    FFontEditorPanel(FFontEditorPanel&&)                 = delete;
    FFontEditorPanel& operator=(FFontEditorPanel&&)      = delete;
};
```

理由: 内部 `TArray<T>` + raw pointer + callback の所有を曖昧にしない (ACS 規約)。

### 6.4 E-prefix enum (Phase 19a 規約)

`src/gameframework/tools` 配下の enum class は、`.cpp` 内部型を含めて `E` prefix +
`: u8 / u16 / u32` underlying type を明示する:

| editor | enum |
|---|---|
| editor_core | `EAssetKind`, `EEditorCameraMode`, `EGizmoMode`, `EGizmoSpace`, `EGizmoAxis`, `EEditorThemePreset`, `EEditorThemePersistenceError`, `EEditorWorkspacePersistenceError` (`EThemeNumberStatus` は `.cpp` 内部型) |
| fxedit | `EFxeditSerializeError` (`ENumberStatus` / `EFxKnownKey` は `.cpp` 内部型) |
| inspector | `EEditorState` |
| modelview | `EAnimationPlayState` |
| leveledit | `EBrushKind` |
| spriteatlas | `EPivotPreset` (`EFrameHandle` は `.cpp` 内部型) |
| btedit | `EBtKind`, `EBtDecoMode`, `EBtGraphPersistenceError` |
| cinetimeline | `ETimelineKeyKind` |

隣接する `FBehaviorTree` の `EBtStatus` と `InspectorSeam.h` の `EFieldKind`
(10 種、`ObjectRef` を含む) も同じ規約に従う。`EAssetKind` の未分類値は
`None` ではなく `Unknown`。

### 6.5 ImGui editor sample の build target

editor sample 群は `ACS::Imgui` と raw DX12 backend を前提にする。現行の各 sample `CMakeLists.txt` はソース分割を列挙し、共通 target helper と ACS module target を使う。sample 本体に `#if ACS_RENDER_DX12_RAW` の fallback `main()` は置いていない。

```cmake
# samples/<NN>_Hello<Name>/CMakeLists.txt
add_executable(hello_name WIN32
    main.cpp
    NameApp.cpp
    NameScene.cpp
    NameApp.h
    NameScene.h
)
acs_apply_compiler_options(hello_name)
target_include_directories(hello_name PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_name PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::App
    ACS::Platform
    ACS::Imgui
    ACS::Math
)
```

---

## 7. 将来拡張余地 / Future Extensions

各セクションは現状の「実装済み」を超えた **将来の到達点**。優先度順に並べる。

### 7.1 全 editor の FUndoStack 統合

現状: `FUndoStack` は `editor_core` にあるが、各 panel が独自に dirty state を持ち、`FUndoStack` を直接使っていない panel が多い (`FParticleEditorPanel::m_Dirty`、`FInspectorPanel::m_Dirty` 等)。

将来: `FEditorWorkspace` が 1 個の `FUndoStack` を保持し、全 panel の編集アクションを、現行 `FMoveNodeCommand` と同じ `MakeUnique` + `Push(Move(command))` 形式で集約する。`OnUndo()` / `OnRedo()` は `EditorPanel.h` の設計コメントにある候補で、まだ実宣言ではない。

### 7.2 ParticleEditor / SceneInspector の FEditorPanel 統合 (完了)

次の panel は現在すべて `FEditorPanel` を継承する:

- `FParticleEditorPanel`
- `FHierarchyPanel` / `FInspectorPanel` / `FEditorToolbar`

これらは `FEditorWorkspace::RegisterPanel(&panel)` で統一 dispatch できる。`FSelectionService` と `FFxeditSerializer` は panel ではないため独立 service のまま使う。

### 7.3 panel 間 dependency declaration

現状: `FEditorWorkspace` の panel 配列は登録順 = dispatch 順で、依存関係を明示する仕組みは無い。

`EditorPanel.h` の設計コメントには `GetDependencyMask()` 候補があるが、現行 class には宣言されていない。将来は panel ごとに依存先を bit flag で宣言し、workspace が dependency 順に `OnFrameBegin` / `DrawUI` を呼ぶ schedule を組める。例: `FModelViewerPanel` が `FAssetBrowser` adapter に依存する場合は、asset event を先に処理する。

### 7.4 shortcut key dispatcher

現状: 各 panel が独自に ImGui::IsKeyPressed をチェック (例: BehaviorTreeEditor の Step ボタンに割り当てるキーは無い)。

将来: `FEditorWorkspace` 内に shortcut key dispatcher を追加し、`Ctrl+S` → layout/save command、`Ctrl+Z` → `FUndoStack::Undo()` を統一処理する。shortcut value type と `OnKeyShortcut` hook はまだ宣言されていないため、導入時に `F` prefix を含む正式型名を決める。

### 7.5 ImGui docking branch 統合

現状: `FEditorWorkspace::DrawDockSpace()` は内部の `ACS_EDITOR_HAS_IMGUI_DOCK` guard (`IMGUI_HAS_DOCK` から導出) で囲まれており、ACS の現状 (master branch ImGui) では no-op になる。各 panel は通常の floating window として並ぶ。

将来: ImGui docking branch (`docking` 公式 fork) に切り替えると自動的に有効化される。`FEditorPanel::SetDockTarget(bool)` の永続化は実装済みだが、dock placement への強制反映は未実装。

### 7.6 multi-window (sub-viewport)

ImGui docking branch の `ImGuiConfigFlags_ViewportsEnable` を有効化すると、各 panel を OS ネイティブ window として外に出せる。現行 `FEditorWorkspace` には viewport enable state も `WantsExternalViewport()` hook もないため、renderer/resource lifetime と合わせて設計する必要がある。

### 7.7 editor の AssetPack 統合

AssetPack (`.acpak` 形式) と `FAcpakReader` / `FAcpakWriter` は実装済みだが、`FAssetBrowser` はまだ file system のみを列挙する。editor 側には「実 file system + AssetPack overlay」の 2 層表示が必要である。

- `FAssetBrowser::Init(const wchar_t* root)` に加えて mount API を追加
- `FAssetEntry` に「source: filesystem / assetpack」情報を追加
- AssetPack 内 entry はアイコンを変えて視覚的に区別

### 7.8 NodeGraph 系 editor

`FBehaviorTreeEditorPanel` は graph canvas、node add/delete、接続、decorator、blackboard、snapshot undo/redo、persistence まで実装済み。

WPF editor の Material Editor は、Substrate-style closure graph と型付き shader-expression graph を同じ canvas で編集する。closure graph は Slab と Coverage Weight / Horizontal Blend / Vertical Layer / Add / Select、expression graph は Constant / Parameter / Texture Sample / UV / Time / world inputs と算術 node を提供する。接続時に型と cycle を検証し、backend compiler が 64 node、4 texture slot、32 parameter の上限と最終 topology を再検証する。`ACSMAT` は closure、expression、binding、texture slot、parameter name、editor layout を原子的に保存し、production PBR shader は同じコンパイル済み expression VM を per-pixel で評価する。

現在の dynamic binding は Front Material が直接 Slab を参照する topology に限定する。operator で合成した closure graph 自体は静的に解決できるが、その内部 Slab へ動的 expression を binding する構成は editor/compiler が明示的に拒否する。preview は production PBR path と同じ sphere mesh、normal/albedo/expression texture、shader VM を使う。

残る graph editor と polish は次のとおり:

| 項目 | 編集対象 | 状態 |
|---|---|---|
| BehaviorTree graph polish | drag reparent、複数選択、auto layout | future |
| `FAnimationGraph` editor | state node + transition arrow | future |
| Material Graph editor | 複数 Slab 内の dynamic expression と material-instance authoring | partial |

共通 canvas を `editor_core` へ抽出する場合は、`FBehaviorTreeEditorPanel` 内の現行 graph behavior と persistence contract を先にテストで固定する。

### 7.9 Cinematics timeline の高度な keyframe 編集

`FCinematicsTimelineEditorPanel` は単一 marker の選択と drag を実装済み。今後は複数選択、一括 drag、snap grid、補間 curve、複数 `FCinematicsDirector` の編集、専用 serializer を追加する。共通 canvas を分離する場合も、現行の `FEditorKeyframe` / `ETimelineKeyKind` contract を壊さない。

### 7.10 SpriteAtlas SDF / 9-slice 編集

現状 SpriteAtlasEditor は frame rect (x/y/w/h + pivot) のみ。今後の拡張候補:
- **SDF rendering** mode (font glyph 風) のメタデータ編集
- **9-slice border** (UI で stretch する際の corner/edge 不変領域) の border 編集 (4 値: top/right/bottom/left)

`FMaterialOverride` に texture path swap を追加するのと同じ pattern で、現行 `FSpriteFrame` を互換性を保って拡張するか、新しい `F` prefix の metadata struct を明示的に宣言する。

---

## 8. 参照先 / References

詳細な仕様 / 設計判断は、repository 内の docs と現行ヘッダーを参照する。

### 8.1 ドキュメント

- **`docs/GameFramework.md` §15.4** — 著作ツール深度 & 外部ミドルウェアシーム (Pillar K 拡張)
  - 4 不変条件: (a) Pillar K seam 上に構築・重複実装しない (b) v1 はすべて in-engine UiKit (c) FMOD/Wwise seam (d) Cinematics editor は `FCinematicsDirector` 上に被せる
  - Phase 56〜58 (著作ツール深度): Particle Editor → BT visual editor + Level editor → FMOD/Wwise seam + Cinematics editor
- **`docs/StyleGuide.md`** — ACS Coding Style Guide
  - §1 基本不変条件 (No STL / No exceptions / No RTTI / checked result / callback)
  - §2 命名 (PascalCase / snake_case / E-prefix enum / I-prefix interface)
- **`docs/ARCHITECTURE.md`** — ACS 全体アーキテクチャ
- **`docs/QUICKSTART.md`** — beginner-UX 入門 (Phase 4 配布パッケージング含む)
- **`docs/AnimationCurvePersistenceSafety.md`** — curve の固定LE wire、CRC、atomic file、transactional load
- **`docs/UiTextInputSafety.md`** — UTF-8 cursor 編集、入力上限、OOM時不変性、caret 描画
- **`docs/HotReloadSafety.md`** — watcher/event 上限、通知欠落診断、callback再入・path寿命

### 8.2 関連ヘッダ (editor 群の隣接モジュール)

- `src/gameframework/ANode.h` — Scene graph の node (`FHierarchyPanel` が表示対象)
- `src/gameframework/InspectorSeam.h` — `IInspectableProvider` / `FInspectableField` / `EFieldKind` / `FInspectorSeam` (`FInspectorPanel` が描画対象)
- `src/gameframework/ParticleEffectSystem.h` — `FParticleEmitterDef` (`FParticleEditorPanel` の編集対象)
- `src/gameframework/AnimationCurve.h` — Hermite/Linear/Step curve (`FAnimCurveEditorPanel` の編集対象)
- `src/gameframework/AnimationCurveArchive.h` — `FAnimationCurve` の canonical buffer/file 永続化
- `src/gameframework/BehaviorTree.h` — `FBtSelector` / `FBtSequence` / `FBtAction` (`FBehaviorTreeEditorPanel` の観察対象)
- `src/gameframework/Tilemap.h` — multi-layer u16 `FTileId` grid (`FLevelEditorPanel` の編集対象)
- `src/gameframework/SpritePack.h` — atlas + `FSpriteFrame` (`FSpriteAtlasEditorPanel` の編集対象)
- `src/gameframework/CinematicsDirector.h` — runtime timeline (`FCinematicsTimelineEditorPanel` の bake 先)
- `src/gameframework/DebugDraw.h` — 2D line buffer (`FEditorGizmo` が出力先として使用)
- `src/platform/FileSystem.h` — file I/O (`FFxeditSerializer` / `FEditorTheme` / `FEditorWorkspace` の永続化)
- `src/render/PostProcess.h` — `FPostProcessParams::tonemap_kind` は 0=ACES / 1=AgX / 2=Reinhard。`FModelViewerPanel` の 0=ACES / 1=Reinhard / 2=Linear とは数値意味が異なるため、renderer 境界で明示変換する。

---

**改訂履歴 / Revision History**:
- v2 (2026-07-19): samples 29〜37 と現行 `src/gameframework/tools` を再監査。`F` / `E` naming、全 panel の `FEditorPanel` 統合、checked persistence、Font/Cinematics 実装、build target、asset path 境界を現行宣言へ同期。
- v1 (2026-05-24): 初版。Phase 19b〜Phase 22 の 7 editor + editor_core 8 コンポーネントを統合的に整理。Phase 23 (FontEditor / CinematicsEditor) は予定欄のみ。
