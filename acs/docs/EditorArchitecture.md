<!-- SPDX-License-Identifier: Apache-2.0 -->
# ACS Editor Architecture (v1)

**目的 / Purpose**: ACS GameFramework が提供する **in-game editor 群** (8 つの authoring tool + 共通基盤) の設計を一冊にまとめる。各 editor の API、依存関係、責任分担、ファイル形式、UI 規約を横断的に俯瞰できるドキュメント。

**対象 / Scope**: `src/gameframework/tools/**` 配下と `samples/29_HelloParticleEditor` 〜 `samples/35_HelloSpriteAtlasEditor`。Phase 23 で並行作成中の FontEditor / CinematicsTimelineEditor は「予定」セクションで触れる。

**バージョン / Version**: v1 (2026-05-24)

**関連ドキュメント**:
- 詳細仕様: `docs/GameFramework.md` §15.4「著作ツール深度 & 外部ミドルウェアシーム (Pillar K 拡張)」
- コーディング規約: `docs/StyleGuide.md`
- 全体アーキテクチャ: `docs/ARCHITECTURE.md`

---

## 目次 / Table of Contents

1. [概要 / Overview](#1-概要--overview)
2. [ディレクトリ構成 / Directory Layout](#2-ディレクトリ構成--directory-layout)
3. [editor_core 共通基盤 / Common Foundation](#3-editor_core-共通基盤--common-foundation)
4. [8 種の editor sample 一覧 / Editor Sample Catalogue](#4-8-種の-editor-sample-一覧--editor-sample-catalogue)
5. [設計パターン / Design Patterns](#5-設計パターン--design-patterns)
6. [共通制約 / Cross-cutting Constraints](#6-共通制約--cross-cutting-constraints)
7. [将来拡張余地 / Future Extensions](#7-将来拡張余地--future-extensions)
8. [メモリ参照 / Memory References](#8-メモリ参照--memory-references)

---

## 1. 概要 / Overview

### 1.1 設計方針 (v10「著作ツール深度」より)

ACS の editor は **in-game UiKit ベース** (外部 IDE なし) という方針を採る。これは `docs/GameFramework.md` §15.4 で確定した v10 著作ツール深度の指針に従う:

- **In-engine tooling**: editor は engine と同じプロセス内で動作する。Unity Editor のような「別アプリ」モデルではなく、Unreal Engine の editor mode に近い「engine の特殊な起動形態」。
- **UiKit と疎結合**: editor の UI は ImGui (debug/dev build) を主要レイヤとして使い、ACS の独自 UI レイヤ (`acs::ui`) は **ship build 用** に保留 (= 「ゲーム本体の UI に editor を組み込む」のではなく、「dev build でのみ editor window を出す」モデル)。
- **共通基盤の dogfooding**: 全 editor が `editor_core::FEditorPanel` 基底を継承し、`FEditorWorkspace` で統括することで、新規 editor を追加する際の boilerplate を最小化。

### 1.2 ImGui を採用する理由

| 観点 | ImGui を主 UI に使う理由 |
|---|---|
| **開発速度** | Immediate-mode で「描画 = state 反映」が 1 行 1 widget で書ける。retained UI (Unity uGUI 風) に比べて editor 開発が 10x 速い。 |
| **iteration** | hot reload なしでも widget 追加が即座に反映される (= IMGUI の本領)。 |
| **dependency** | Diligent Engine ベースの ACS RHI に既に統合済 (`samples/21_HelloImGui`)。再利用コストゼロ。 |
| **ship 切り離し** | `#if ACS_EDITOR_HAS_IMGUI_DOCK` などのコンパイル時 guard で retail build から完全に消せる。 |

ACS UI (`acs::ui::FUiLayer`) は **ship 用** に保留している理由:
- editor を game build と分けたいため、editor だけ ImGui で書いて、ゲーム本体の UI (`acs::ui`) には触らせない方針。
- ImGui は 100ms 規模の dev-only overhead を許容できる前提の library であり、game build のシビアな budget には合わない。

### 1.3 階層構造の概念図

```
+----------------------------------------------------------------------+
|                   FEditorWorkspace (Phase 21a hub)                    |
| - panels: TArray<FEditorPanel*>                                        |
| - DockSpace + MenuBar 自動描画                                       |
| - SaveLayout/LoadLayout (.acslayout)                                 |
| - BroadcastSelectionChanged / BroadcastAssetSelected                 |
+----------------------------------------------------------------------+
   |              |              |              |              |
   v              v              v              v              v
+---------+ +-----------+ +-----------+ +--------------+ +-----------+
| FAsset   | | Inspector | | Hierarchy | | ModelViewer  | | ...8 種   |
| Browser | | Panel     | | Panel     | | Panel        | | editor    |
| (drag   | | (field    | | (FNode2D   | | (3D viewport | | panel     |
|  src)   | |  edit)    | |  tree)    | |  + Lighting) | | (継承)    |
+---------+ +-----------+ +-----------+ +--------------+ +-----------+
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

### 1.4 ファイル統計 (2026-05-24 現在)

| 区分 | ファイル数 | 概略 LOC |
|---|---|---|
| **editor_core** (Phase 21a) | 8 .h + 8 .cpp = 16 | ~3400 |
| **fxedit** (Phase 19b) | 3 .h + 3 .cpp = 6 | ~1100 |
| **inspector** (Phase 20) | 4 .h + 4 .cpp = 8 | ~1600 |
| **modelview** (Phase 21b) | 4 .h + 4 .cpp = 8 | ~1700 |
| **animcurve** (Phase 22) | 1 .h + 1 .cpp = 2 | ~700 |
| **btedit** (Phase 22) | 1 .h + 1 .cpp = 2 | ~600 |
| **leveledit** (Phase 22) | 1 .h + 1 .cpp = 2 | ~700 |
| **spriteatlas** (Phase 22) | 1 .h + 1 .cpp = 2 | ~600 |
| **samples 29-35** | 7 .cpp + 7 CMakeLists = 14 | ~1500 |
| **合計** | **60 ファイル** | **~11900 LOC** |

---

## 2. ディレクトリ構成 / Directory Layout

```
src/gameframework/tools/
├── editor_core/   - 共通基盤 (Phase 21a)
│   ├── EditorPanel.h/.cpp         - 全 panel の抽象基底
│   ├── EditorWorkspace.h/.cpp     - 複数 panel の統括 hub
│   ├── UndoStack.h/.cpp           - undo/redo 中央ハブ
│   ├── EditorCommand.h            - undo の原子単位 (header-only)
│   ├── EditorCamera.h/.cpp        - 2D pan/zoom + 3D orbit
│   ├── AssetBrowser.h/.cpp        - assets/ 配下の file tree + drag source
│   ├── PropertyDrawer.h/.cpp      - field type → drawer registry
│   ├── EditorGizmo.h/.cpp         - Translate/Rotate/Scale handle
│   └── EditorTheme.h/.cpp         - Dark/DarkBlue/Light/HighContrast/Sepia
│
├── fxedit/        - ParticleEditor (Phase 19b)
│   ├── ParticleEditorPanel.h/.cpp - emitter param 編集 UI
│   ├── ParticleEditorPreview.h/.cpp - emitter 単体プレビュー
│   └── FxeditSerializer.h/.cpp    - .fxedit テキスト I/O
│
├── inspector/     - SceneInspector 4 panel (Phase 20)
│   ├── SelectionService.h/.cpp    - 選択 FNodeId 集中点 (callback hub)
│   ├── HierarchyPanel.h/.cpp      - FNode2D ツリー表示 + reparent
│   ├── InspectorPanel.h/.cpp      - FInspectableField 編集
│   └── EditorToolbar.h/.cpp       - Play/Pause/Step/Save/FDebugOverlay
│
├── modelview/     - ModelViewer 4 panel (Phase 21b)
│   ├── ModelViewerPanel.h/.cpp    - 3D viewport + Lighting/Background
│   ├── ModelInspectorPanel.h/.cpp - mesh 統計 (vertex/material/bone)
│   ├── ModelMaterialPanel.h/.cpp  - material override 編集
│   └── ModelAnimationPanel.h/.cpp - animation clip 切替 + 再生
│
├── animcurve/     - AnimCurveEditor (Phase 22)
│   └── AnimCurveEditorPanel.h/.cpp - Hermite/Linear/Step curve 編集
│
├── btedit/        - BehaviorTreeEditor (Phase 22)
│   └── BehaviorTreeEditorPanel.h/.cpp - BT 可視化 + step debug
│
├── leveledit/     - LevelEditor / FTilemap painter (Phase 22)
│   └── LevelEditorPanel.h/.cpp    - tilemap painter (Paint/Erase/Fill/Pick)
│
├── spriteatlas/   - SpriteAtlasEditor (Phase 22)
│   └── SpriteAtlasEditorPanel.h/.cpp - FSpritePack atlas + frame rect 編集
│
├── fontedit/      - FontEditor (Phase 23、並列作成中、未作成)
└── cinetimeline/  - CinematicsTimelineEditor (Phase 23、並列作成中、未作成)
```

各 editor sample は `samples/<NN>_Hello<Name>/` に対応する main.cpp + CMakeLists.txt を持つ。Phase 23 の FontEditor / CinematicsTimelineEditor が完成すると `samples/36_HelloFontEditor/` と `samples/37_HelloCinematicsEditor/` が追加される予定。

---

## 3. editor_core 共通基盤 / Common Foundation

Phase 21a で導入された **8 つの共通コンポーネント**。Phase 19b ParticleEditor と Phase 20 SceneInspector は本基盤より早く実装されたため、現状 `FEditorPanel` 継承していない (将来 refactor 予定)。Phase 21b 以降の editor (ModelViewer / AnimCurveEditor / BehaviorTreeEditor / LevelEditor / SpriteAtlasEditor) は全て本基盤に統合済。

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
| `void OnSelectionChanged(FSelectionService& sel) noexcept` | virtual | FScene 内 FNode 選択変更通知 |
| `void OnAssetSelected(const char* asset_path) noexcept` | virtual | FAssetBrowser からのファイル選択通知 |
| `void OnSaveLayout(FEditorLayoutSerializer& out) noexcept` | virtual | panel 独自の表示設定を書き出す (Phase 21c 本実装) |
| `void OnLoadLayout(FEditorLayoutSerializer& in) noexcept` | virtual | OnSaveLayout の逆操作 |
| `bool WantsFocus() const noexcept` | virtual | 起動時に Focus を奪うかのヒント (default false) |
| `bool IsVisible() / SetVisible(bool)` | concrete | panel 表示 toggle (close ボタン連動) |
| `FEditorWorkspace* Workspace() const` | concrete | OnInit で保存された Workspace ポインタ |

**使い方例**:

```cpp
class FModelViewerPanel : public acs::game::editor_core::FEditorPanel {
public:
    const char* Title() const noexcept override { return "Model Viewer"; }

    void DrawUI() noexcept override {
        if (!IsVisible()) return;
        if (ImGui::Begin(Title(), &_visible)) {
            // ... viewer 描画 ...
        }
        ImGui::End();
    }

    void OnAssetSelected(const char* asset_path) noexcept override {
        // asset_path が .mdl/.fbx/.gltf なら LoadModelAsset を呼ぶ
    }
};
```

**設計選択**:
- ImGui::Begin/End は派生側責務 (ImGuiWindowFlags / dock target / MenuBar の有無が panel ごとに違うため基底で wrap しない)
- `OnSelectionChanged` (FNode 系) と `OnAssetSelected` (file 系) を独立 hook にして、panel ごとに必要な方だけ override できるようにする
- 非コピー / 非ムーブ (panel は workspace と紐づく lifecycle)

### 3.2 FEditorWorkspace — 複数 panel の統括 hub

`src/gameframework/tools/editor_core/EditorWorkspace.h`

複数の FEditorPanel を **1 つの editor アプリケーション** としてまとめて配線する中央 hub。

**主要 API**:

| メソッド | 用途 |
|---|---|
| `void Init() / Shutdown()` | panel list + FSelectionService を初期化 / 破棄 |
| `void RegisterPanel(FEditorPanel*)` | 末尾追加 + OnInit 呼出 (二重登録は no-op) |
| `void UnregisterPanel(FEditorPanel*)` | 順序保存削除 + OnShutdown 呼出 |
| `u32 PanelCount() const` | 現在の登録 panel 数 |
| `FEditorPanel* GetPanelByIndex(u32)` | 登録順 (= dispatch 順) で取得 |
| `FEditorPanel* FindPanelByTitle(const char*)` | strcmp 完全一致検索 |
| `void TogglePanelVisible(const char*)` | FWindow メニュー連動 |
| `void TickAllPanels(f32 dt)` | OnFrameBegin → DockSpace → MenuBar → DrawUI |
| `void DrawDockSpace()` | ImGui::DockSpaceOverViewport (`IMGUI_HAS_DOCK` 未定義時は no-op) |
| `void DrawMenuBar()` | FWindow/Layout メニュー追加 |
| `void SaveLayout(const wchar_t* path)` | `.acslayout` 形式テキスト保存 |
| `void LoadLayout(const wchar_t* path)` | `.acslayout` 復元 |
| `void SetSelectionService(FSelectionService*)` | 注入 (non-owning) |
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
- `kMaxPanels = 32u` (overflow 時は silent no-op)

**`.acslayout` フォーマット** (テキスト):
```
ACS_EDLAYOUT 1
IMGUI_INI <byte_size>
<raw ini bytes>
PANEL <title> <visible:0/1> <dock_target:0/1>
...
```

### 3.3 FUndoStack + FEditorCommand — undo/redo 中央ハブ

`src/gameframework/tools/editor_core/UndoStack.h`, `EditorCommand.h`

全 editor が共有する undo/redo の中央ハブ。FCommand パターン (GoF) ベース。

**FEditorCommand API** (純粋抽象):

| メソッド | 種別 | 用途 |
|---|---|---|
| `void Execute() noexcept` | 純粋仮想 | "Do" を実行 (Push と Redo の両方から呼ばれる) |
| `void Undo() noexcept` | 純粋仮想 | Execute の逆操作 |
| `const char* Description() const` | 純粋仮想 | UI 表示用短文 ("Move FNode" 等) |
| `const void* Kind() const` | virtual | RTTI 抜き型識別 tag (default nullptr) |
| `bool CanMerge(const FEditorCommand& next) const` | virtual | 連続 drag 等を 1 件にまとめるか (default false) |
| `void MergeWith(const FEditorCommand& next)` | virtual | CanMerge true の後に呼ばれる (new 値を取り込み) |

**FUndoStack API**:

| メソッド | 用途 |
|---|---|
| `void Init(u32 max_history = 64)` | 初期化 (history 上限再設定可) |
| `void Push(FEditorCommand* cmd)` | 所有権を奪う + Execute 呼出 + Merge 判定 + Redo クリア |
| `bool Undo() / bool Redo()` | 1 件巻き戻し / やり直し |
| `bool CanUndo() / CanRedo()` | UI MenuItem の enable 判定用 |
| `u32 UndoCount() / RedoCount()` | history 件数 |
| `const char* UndoDescription() / RedoDescription()` | top の Description (空時 "") |
| `void SetOnExecutedCallback(cb, user)` | Push/Undo/Redo 後の副反応用 |

**使い方例**:

```cpp
acs::game::editor_core::FUndoStack stack;
stack.Init(64);

auto* cmd = acs::New<FMoveNodeCommand>(acs::DefaultAllocator(),
                                       &node, old_pos, new_pos);
stack.Push(cmd);

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
| `void Tick(f32 dt)` | smooth target follow (FCamera2D と同じ指数補間) |

**操作マッピング**:
- 3D: LMB drag = orbit / MMB drag = pan / RMB drag = orbit (Maya 風代替) / wheel = dolly
- 2D: LMB drag = pan / MMB drag = pan / wheel = zoom

**使い方例**:

```cpp
acs::game::editor_core::FEditorCamera cam;
cam.Init(EEditorCameraMode::Mode3D);

cam.HandleMouseInput(mouse_delta, lmb, rmb, mmb, wheel);
cam.Tick(dt);
FMat4 view = cam.ViewMatrix();
FMat4 proj = cam.ProjectionMatrix(aspect, 0.1f, 1000.0f);
cam.FrameToBoundingSphere(node_center, node_radius);
```

### 3.5 FAssetBrowser — file tree + drag source

`src/gameframework/tools/editor_core/AssetBrowser.h`

プロジェクト `assets/` 配下のファイルツリーを ImGui で参照 + 各 panel へ drag-drop で path を供給する Unity Project FWindow 相当。

**主要 API**:

| メソッド | 用途 |
|---|---|
| `void Init(const wchar_t* root)` | assets/ ルートを記録 + 初回 Refresh |
| `void Shutdown()` | pool / callback 解放 |
| `void Refresh()` | current_directory 配下の rescan |
| `void DrawUI()` | "FAsset Browser" 1 window (左 tree + 右 list) |
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
| FFont | .ttf .otf |
| Audio | .wav .ogg .mp3 .flac |
| Material | .mat .material |
| FParticle | .fx .particle |
| FAnimation | .anim |
| FBehaviorTree | .bt |
| FTilemap | .tilemap .tmx |
| Prefab | .prefab |
| Cinematic | .cine |
| FScene | .scene |

**Drag payload**: identifier `"ASSET_PATH"` (32 文字以内) で wchar_t* 直渡し。受け側は `ImGui::AcceptDragDropPayload("ASSET_PATH")` で取り出す。

### 3.6 PropertyDrawer — field 型 → drawer 登録レジストリ

`src/gameframework/tools/editor_core/PropertyDrawer.h`

カスタム field drawer の登録レジストリ。Phase 20 FInspectorPanel が hardcode する EFieldKind 9 種を超えた拡張型 (Curve / Gradient / AssetPath / NodeIdSelector 等) を後付け可能にする。

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
- `EnumCombo` / `FTextInput`

**FPropertyContext** (POD):

```cpp
struct FPropertyContext {
    void*       data_ptr;     // 編集対象 (drawer がキャスト)
    const char* label;        // ImGui ラベル
    const char* tooltip;      // hover tooltip
    f32         min_value;    // F32Slider 等で使う
    f32         max_value;
    const char* enum_values;  // "Item0\0Item1\0...\0"
    u32         enum_count;
    bool*       out_changed;  // drawer が dirty を書く出口
};
```

**使い方例**:

```cpp
static void DrawHealth(const FPropertyContext& ctx) noexcept {
    auto* hp = static_cast<Health*>(ctx.data_ptr);
    ImGui::ProgressBar(hp->Ratio(), ImVec2(-1, 0));
    if (ctx.out_changed) *ctx.out_changed = false;
}

FPropertyDrawerRegistry reg;
reg.Init();
reg.RegisterDrawer("Health", &DrawHealth);

FPropertyContext ctx { /* ... */ };
if (!reg.DrawProperty("Health", ctx)) {
    // 未登録 → EFieldKind switch にフォールバック
}
```

### 3.7 FEditorGizmo — Translate/Rotate/Scale handle

`src/gameframework/tools/editor_core/EditorGizmo.h`

選択中 FNode の Transform を viewport 上で直接ドラッグ操作するハンドル。Unity / Godot / UE のシーンビュー操作系の最小集合。

**主要 API**:

| メソッド | 用途 |
|---|---|
| `void Init() / Shutdown()` | state を default に |
| `void SetMode(EGizmoMode)` | None / Translate / Rotate / Scale |
| `void SetSpace(EGizmoSpace)` | FWorld / Local |
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
- `EGizmoSpace`: FWorld / Local
- `EGizmoAxis`: None_ / X / Y / Z / XY / XZ / YZ / ScreenAlign

**使い方例**:

```cpp
FEditorGizmo gizmo;
gizmo.Init();
gizmo.SetMode(EGizmoMode::Translate);
gizmo.SetSnapTranslate(0.5f);

gizmo.ProcessInput(ray_o, ray_d, lmb_down, lmb_held, lmb_up);
FVec3 pos = node.WorldPosition();
FVec3 rot = node.EulerRotation();
FVec3 scl = node.WorldScale();
if (gizmo.Manipulate(pos, rot, scl)) {
    node.SetWorldPosition(pos);
    node.SetEulerRotation(rot);
    node.SetWorldScale(scl);
}
gizmo.DrawGizmo(debug_draw, pos, rot, scl);
```

### 3.8 FEditorTheme — Dark/Light/HighContrast 等 5 preset

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
| `void SetRoundedCorners(f32)` | Frame/FWindow/Popup/Grab/Tab/Scrollbar 統一 radius |
| `void SetSpacing(f32 item_spacing_y)` | ItemSpacing.y (x は 0.5x 連動) |
| `void DrawThemeSettingsUI()` | "Theme FSettings" 独立 window 描画 |
| `void SaveTheme(const wchar_t*) / LoadTheme(const wchar_t*)` | `.acstheme` |

**5 preset**:

| preset | 想定用途 |
|---|---|
| Dark | 標準 dark grey (ImGui StyleColorsDark 寄り、若干暖色) |
| DarkBlue | VS Code Dark+ 風 (#1F232C / #007ACC 系) |
| Light | 明るい背景 (屋外 / プロジェクタ向け) |
| HighContrast | 黒 / 白 / 黄 (#FFD700) の三色設計 (FAccessibilityProfile 連動予定) |
| Sepia | 焼け紙風暖色 (長時間作業の眼精疲労低減) |
| Custom | SetCustomColors() で渡された任意パレット |

**FEditorThemeColors** (13 フィールド): `window_bg / title_bg / button_bg / button_hover / button_active / frame_bg / text / text_disabled / border / separator / accent / warning / error`

---

## 4. 8 種の editor sample 一覧 / Editor Sample Catalogue

| Sample # | Editor 名 | 対象 data | Phase | 主要 panel ファイル |
|---|---|---|---|---|
| **29** | ParticleEditor | FParticleEmitterDef | 19b | `fxedit/ParticleEditorPanel.h` |
| **30** | SceneInspector | FNode2D + FInspectorSeam | 20 | `inspector/HierarchyPanel.h` + `InspectorPanel.h` + `EditorToolbar.h` |
| **31** | ModelViewer | mesh + material + anim | 21b | `modelview/ModelViewerPanel.h` + 3 つの sub panel |
| **32** | AnimCurveEditor | FAnimationCurve | 22 | `animcurve/AnimCurveEditorPanel.h` |
| **33** | BehaviorTreeEditor | FBehaviorTree | 22 | `btedit/BehaviorTreeEditorPanel.h` |
| **34** | LevelEditor | FTilemap | 22 | `leveledit/LevelEditorPanel.h` |
| **35** | SpriteAtlasEditor | FSpritePack | 22 | `spriteatlas/SpriteAtlasEditorPanel.h` |
| **36** | FontEditor (予定) | font face list | 23 | `fontedit/` (未作成) |
| **37** | CinematicsEditor (予定) | FTimelineKeyframe | 23 | `cinetimeline/` (未作成) |

### 4.1 Sample 29 — ParticleEditor (Phase 19b)

**path**: `samples/29_HelloParticleEditor/main.cpp`

- **編集対象**: `acs::game::FParticleEmitterDef` (gameframework/ParticleEffectSystem.h)
- **UI レイアウト**: 左 emitter list + 右 property pane の 2 カラム
- **編集パラメータ**: lifetime_sec / emit_rate_per_sec / burst_count / speed_min/max / scale_start/end / spread_radians / gravity / color_start/end
- **永続化**: `FFxeditSerializer` で `.fxedit` テキスト形式 (`ACS_FXEDIT 1`)
- **特徴**: 本 panel は **FEditorPanel 継承していない** (Phase 19b は Phase 21a 共通基盤の前に実装)。将来 refactor 予定。
- **callback**: `SaveCallback` / `LoadCallback` で外部に save/load 委譲

### 4.2 Sample 30 — SceneInspector (Phase 20)

**path**: `samples/30_HelloSceneInspector/main.cpp`

- **4 panel 構成** (3 つの panel + 1 service):
  - `FHierarchyPanel` — FNode2D ツリー、reparent (drag drop payload `"HIER_NODE_PTR"`)、Delete/Duplicate context menu
  - `FInspectorPanel` — FInspectorSeam 経由で FInspectableField を編集 (EFieldKind 9 種 hardcode switch)
  - `FEditorToolbar` — Play/Pause/Step/Save/FDebugOverlay の 5 ボタン
  - `FSelectionService` — 選択 FNodeId の集中点 (callback 複数登録、callback hub)
- **特徴**: 本 panel 群も **FEditorPanel 継承していない** (Phase 20 は Phase 21a 前)。将来 refactor 予定。
- **連携**: FHierarchyPanel ⇆ FInspectorPanel は FSelectionService 経由で疎結合
- **Drag drop**: `"HIER_NODE_PTR"` payload で FNode2D* 直渡し

### 4.3 Sample 31 — ModelViewer (Phase 21b)

**path**: `samples/31_HelloModelViewer/main.cpp`

- **4 panel 構成** (全て `FEditorPanel` 継承):
  - `FModelViewerPanel` — 3D viewport + Lighting (sun dir/color + IBL toggle + tonemap mode) + Background + Grid/FBone toggle
  - `FModelInspectorPanel` — mesh 統計 (vertex/triangle/submesh/material/bone/animation count + bounding sphere) を read-only 表示
  - `FModelMaterialPanel` — material slot 編集 (Base FColor / Metallic / Roughness / Normal / AO / Emissive)
  - `FModelAnimationPanel` — animation clip 切替 + Play/Pause/Stop + Time slider + Speed + Loop + BlendWeight
- **特徴**: Phase 21a 共通基盤 (`FEditorPanel` / `FEditorCamera` / `FEditorWorkspace` / `FAssetBrowser`) の **最初の dogfood**
- **連携**: FAssetBrowser → `BroadcastAssetSelected("models/hero.mdl")` → `FModelViewerPanel::OnAssetSelected` で自動 LoadModelAsset
- **camera**: 各 panel が独自 `FEditorCamera` (Mode3D orbit) を内包する Unity SceneView 風モデル

### 4.4 Sample 32 — AnimCurveEditor (Phase 22)

**path**: `samples/32_HelloAnimCurveEditor/main.cpp`

- **編集対象**: `acs::game::FAnimationCurve` (Hermite/Linear/Step + Pre/Post WrapMode)
- **UI**: 単一 window 内に toolbar (Interpolation Combo / WrapMode Combo / Add Key / Clear / Eval preview slider) + canvas (1024-sample 線描画)
- **操作**:
  - 各 key を丸 marker で描画 + drag で time/value 編集
  - Hermite key は in/out tangent を小さい handle として描画 (固定 30px 長) + drag で接線編集
  - 右クリック context menu (Add key here / Delete selected)
- **callback**: `CurveChangeCallback` (drag 中は連続発火を避け、drag end で 1 度発火)
- **公開定数**: `kCurveSampleCount = 1024u`, `kNoKeySelected = -1`

### 4.5 Sample 33 — BehaviorTreeEditor (Phase 22)

**path**: `samples/33_HelloBehaviorTreeEditor/main.cpp`

- **編集対象**: `acs::game::FBehaviorTree` (`FBtSelector` / `FBtSequence` / `FBtAction`)
- **v1 = visualize + step debug にスコープ集中** (グラフ編集 = v2 future)
- **メタミラー方式**: FBehaviorTree 本体の private member (`_children` 等) に panel から触れないため、user が `AddNode(kind, name, parent_id)` で「親 id・kind・表示名」を panel に push する別ミラー。`SetNodeStatus(node_id, EBtStatus)` で FBtAction の Fn から status を push。
- **UI**:
  - toolbar: Reset / Step / Continuous (autorun) toggle / Active / Step counter
  - 上部: Tick history (60 frame ring graph、PlotLines)
  - 左: TreeNode (色分け Success=緑/Failure=赤/Running=黄)
  - 右: FNode Inspector (Name / Kind / Id / Parent / Children / Last Status)
- **公開定数**: `kHistorySize = 60u`, `kInvalidId = 0xFFFFFFFFu`
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
- **viewport**: ImDrawList で色付き矩形 placeholder (本物 texture atlas は Phase 22+ で差替)
- **公開定数**: `kTileIdMax = 1023`, `kFloodFillMaxCells = 4096`

### 4.7 Sample 35 — SpriteAtlasEditor (Phase 22)

**path**: `samples/35_HelloSpriteAtlasEditor/main.cpp`

- **編集対象**: `acs::game::FSpritePack` (atlas メタ + 名前付き frame 矩形リスト)
- **UI**: toolbar (New/Delete/Pivot プリセット) + 中央 viewport (atlas placeholder + 矩形 overlay) + 左 frame list + 右 inspector
- **操作**:
  - frame rect の resize: 4 corner + 4 edge の 8 handle (mouse drag、`EFrameHandle`)
  - inspector に SliderInt(x/y/w/h) で精密入力
  - Pivot toggle: Center / TopLeft / Custom (`EPivotPreset`)
- **特徴**: v1 では atlas texture 実描画なし (ImTextureID + DX12 descriptor heap 統合は Phase 22+)
- **永続化**: Sample 35 側で `.acsatlas` stub menu (シリアライザ実装は Phase 22+)

### 4.8 Sample 36/37 — FontEditor / CinematicsEditor (Phase 23、未着手)

**予定**:
- **FontEditor**: `samples/36_HelloFontEditor/`、`src/gameframework/tools/fontedit/`
  - 編集対象: font face list (font path + size + fallback chain)
- **CinematicsEditor**: `samples/37_HelloCinematicsEditor/`、`src/gameframework/tools/cinetimeline/`
  - 編集対象: `FTimelineKeyframe` (cine 形式 `.acscine`)

両者とも Phase 23 で並列作成中。本書 v1 時点 (2026-05-24) では未実装。

---

## 5. 設計パターン / Design Patterns

8 つの editor 全体に通底するパターンを抽出。新規 editor を追加する際に守るべき規約集。

### 5.1 Drag-drop payload identifier 統一

ImGui の drag-drop payload identifier は 32 文字以内が仕様上限。ACS editor 群では以下の 2 種を統一定数として使う:

| identifier | payload data | 送信側 | 受信側 |
|---|---|---|---|
| `"ASSET_PATH"` | `const wchar_t*` (1 pointer = 8 bytes) | FAssetBrowser | FModelViewerPanel.OnAssetSelected / PropertyDrawer "AssetPath" |
| `"HIER_NODE_PTR"` | `class FNode2D*` (1 pointer = 8 bytes) | FHierarchyPanel (drag source) | FHierarchyPanel (drop target、Reparent 用) |

両者とも **pointer 直渡し** で、寿命は FAssetBrowser/FHierarchyPanel の Refresh まで保証される (= 次の Refresh 後は無効化)。

**公開定数の場所**:
- `FAssetBrowser::kDragDropPayloadId = "ASSET_PATH"`
- `FPropertyDrawerRegistry::kAssetPathPayloadId = "ASSET_PATH"` (同値で AssetPath drawer 側で受け取り)
- `FHierarchyPanel::kDragDropId = "HIER_NODE_PTR"`

### 5.2 FSelectionService 駆動の panel 間同期

1 panel で選択 → 他 panel が `OnSelectionChanged` 通知で同期する `FSelectionService` ハブパターン:

```
FHierarchyPanel.SelectNode(node)
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

### 5.3 FAssetBrowser → workspace.BroadcastAssetSelected → 各 panel の OnAssetSelected cascade

```
FAssetBrowser でユーザがファイル選択
       |
       v
FAssetBrowser の _on_selected_cb が呼ばれる (例: sample 31 main.cpp で登録)
       |
       v
sample 側 callback 内で workspace.BroadcastAssetSelected(asset_path)
       |
       v
FEditorWorkspace が全 registered panel の OnAssetSelected を順に呼ぶ
       |
       +-- FModelViewerPanel.OnAssetSelected("models/hero.mdl")
       |     → 拡張子フィルタ → LoadModelAsset()
       +-- 他 panel は OnAssetSelected を override してなければ no-op
```

**asset_path の寿命**: FAssetBrowser 所有 (= FAssetBrowser の次の Refresh まで)。panel 側でコピー保持したい場合は固定長 wchar_t バッファに自前で写すこと (FModelViewerPanel が `kMaxAssetPathChars = 512u` で実装)。

### 5.4 Save/Load フォーマット統一

全 editor が「ファイル拡張子 = 特化テキスト形式」を採用 (git diff 可能を最優先):

| editor | 拡張子 | magic ヘッダ | 備考 |
|---|---|---|---|
| ParticleEditor | `.fxedit` | `ACS_FXEDIT 1` | 1 行 1 key=value、`E0 emit_rate 50.0` 形式 |
| AnimCurveEditor | `.acscurve` | (Phase 23 実装予定) | Hermite key + WrapMode |
| BehaviorTreeEditor | `.acsbt` | (実装予定) | メタミラー dump |
| LevelEditor | `.acstilemap` | (Phase 22+ で実装) | layer x cell grid |
| SpriteAtlasEditor | `.acsatlas` | (Phase 22+ で実装) | atlas path + frame rect list |
| CinematicsEditor | `.acscine` | (Phase 23 実装予定) | FTimelineKeyframe list |
| FEditorTheme | `.acstheme` | `ACS_THEME 1` | preset + colors + font scale |
| FEditorWorkspace | `.acslayout` | `ACS_EDLAYOUT 1` | ImGui ini + panel visible/dock_target |

**共通フォーマット規約**:
1. 1 行目に magic + version (例: `ACS_FXEDIT 1`)
2. 1 行 1 key=value (`%g` で float、`strtof` で逆変換)
3. 文字列は二重引用符 (現状エスケープなし、ASCII printable のみ)
4. `#` 始まりはコメント、空行スキップ
5. 未知 key は無視 (前方互換: 将来 key を増やしても旧ローダで読める)
6. version 不一致 / parse 失敗は ACS_LOG_WARN で握る (戻り値 silent or TResult<T,E>)

### 5.5 callback 駆動 (`SetOn*FCallback`)

全 panel が `SetOn*FCallback` 系の C-style 関数ポインタ + `void* user` を持つ。`std::function` 禁止 (ACS 規約)。

**典型 signature**:
```cpp
using CallbackFn = void (*)(void* user, /* payload */) noexcept;
panel.SetOnXxxCallback(&MyHandler, &my_editor);
```

**hookable 場所** (各 panel):

| panel | callback |
|---|---|
| FParticleEditorPanel | SaveCallback / LoadCallback |
| FInspectorPanel | FieldChangeCallback (FNodeId target, const char* field_name, EFieldKind kind) |
| FHierarchyPanel | NodeRightClickCallback (FNode2D* node) |
| FEditorToolbar | SaveSceneCallback |
| FSelectionService | SelectionChangeCallback (FNodeId from, FNodeId to) |
| FAssetBrowser | AssetSelectedCallback / AssetDoubleClickedCallback |
| FUndoStack | CommandExecutedCallback (const FEditorCommand*, bool is_redo) |
| FEditorGizmo | ManipulateCallback (EGizmoMode, FVec3 delta) |
| FModelAnimationPanel | AnimationFrameCallback (u32 clip_index, f32 time_sec) |
| FModelMaterialPanel | MaterialChangeCallback (u32 slot, const FMaterialOverride&) |
| FAnimCurveEditorPanel | CurveChangeCallback (FAnimationCurve*) |
| FBehaviorTreeEditorPanel | StepCallback (FBehaviorTree*, f32 dt) |

FUndoStack 連携は同じ pattern で書ける: callback の中で `New<XxxCommand>(...)` を生成して `undo_stack.Push(cmd)`。

### 5.6 panel API 統一規約

全 panel が以下の共通形を採る (`FEditorPanel` 継承の有無に関わらず):

| API | 用途 |
|---|---|
| `void Init() noexcept` | 内部 state を default に (多重 Init 可) |
| `void Shutdown() noexcept` | 解放 (多重 Shutdown 可) |
| `void DrawUI(...) noexcept` | ImGui::Begin/End まで完結する 1 window 描画 |
| 非コピー / 非ムーブ | 内部 TArray/callback 状態の所有を曖昧にしない |
| `i32 SelectedIndex()` / `void Select(i32)` | -1 = 未選択 sentinel (`u32` ではなく `i32`) |
| `void SetOn*FCallback(cb, user) noexcept` | nullptr 渡しで解除 |

---

## 6. 共通制約 / Cross-cutting Constraints

ACS 全体規約 (詳細は `docs/StyleGuide.md`) を editor 文脈で再確認:

### 6.1 言語制約 (5 不変条件、StyleGuide §1)

- **STL 不使用**: `<vector>` / `<string>` / `<unordered_map>` / `<memory>` / `<functional>` 全部禁止。代替: `acs::TArray<T>` / `acs::FString` (使う場合) / `acs::THashMap<K,V>` / `acs::TUniquePtr<T>` / 関数ポインタ + `void* user`。
- **`<string>` 禁止**: editor 内の文字列は `const char*` (リテラル想定) または `wchar_t[N]` 固定長バッファ (`kMaxPathChars = 512u` 等)。
- **No exceptions**: `throw` / `try` / `catch` 禁止、全関数 `noexcept`。エラーは `TResult<T, FErrorCode>` または silent no-op + `ACS_LOG_WARN`。
- **No RTTI**: `dynamic_cast` / `typeid` 禁止。型識別は `Kind()` で static アドレスを返す idiom (例: `FEditorCommand::Kind()`, `BehaviorTreeEditor::EBtKind` enum)。
- **canonical FCallback**: `using Cb = void (*)(void* user, /* payload */) noexcept;`

### 6.2 ImGui include の局所化

`#include <imgui.h>` は **.cpp 側のみ**。ヘッダから ImGui 依存を漏らさない:

```cpp
// ParticleEditorPanel.h
#pragma once
#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/ParticleEffectSystem.h"
// ↑ ImGui ヘッダは include しない

class FParticleEditorPanel {
    // ImVec2 / ImGuiCol_* も使わない (FVec2 / FVec4 で持つ)
};
```

理由: editor 上位レイヤから panel を include しても include order が壊れないようにする。FEditorTheme が `acs::FVec4` ベースで `FEditorThemeColors` を持つのも同じ理由。

### 6.3 非コピー / 非ムーブ

全 panel + service クラスは非コピー / 非ムーブ:

```cpp
class XxxPanel {
public:
    XxxPanel() noexcept = default;
    ~XxxPanel() noexcept = default;
    XxxPanel(const XxxPanel&)            = delete;
    XxxPanel& operator=(const XxxPanel&) = delete;
    XxxPanel(XxxPanel&&)                 = delete;
    XxxPanel& operator=(XxxPanel&&)      = delete;
    // ...
};
```

理由: 内部 `TArray<T>` + raw pointer + callback の所有を曖昧にしない (ACS 規約)。

### 6.4 E-prefix enum (Phase 19a 規約)

全 enum class は `E` prefix + `: u8 / u16 / u32` underlying type 明示:

| editor | enum |
|---|---|
| editor_core | `EAssetKind`, `EEditorCameraMode`, `EGizmoMode`, `EGizmoSpace`, `EGizmoAxis`, `EEditorThemePreset` |
| inspector | `EEditorState` |
| leveledit | `EBrushKind` |
| spriteatlas | `EPivotPreset`, `EFrameHandle` |
| btedit | `EBtKind` |

例外: FBehaviorTree 本体の `EBtStatus` は既に E prefix。`EAssetKind::None` ではなく `Unknown` を使うのは「不明 / 未分類」のニュアンスを強調するため。

### 6.5 ACS_RENDER_DX12_RAW guard

sample 21 HelloImGui と同じく、editor sample 群は ImGui バックエンドの選択を `#if ACS_RENDER_DX12_RAW` で切り替える。新規 editor sample を書くときは sample 31 ModelViewer の CMakeLists.txt と main.cpp 冒頭を template として使う:

```cmake
# samples/<NN>_Hello<Name>/CMakeLists.txt
if(NOT ACS_RENDER_DX12_RAW)
    return()
endif()
add_executable(<NN>_Hello<Name> main.cpp)
target_link_libraries(<NN>_Hello<Name> PRIVATE acs_runtime acs_gameframework)
```

```cpp
// main.cpp
#if !ACS_RENDER_DX12_RAW
int main() { return 0; }
#else
// ... 本実装 ...
#endif
```

---

## 7. 将来拡張余地 / Future Extensions

各セクションは現状の「実装済み」を超えた **将来の到達点**。優先度順に並べる。

### 7.1 全 editor の FUndoStack 統合

現状: FUndoStack は editor_core にあるが、各 panel が独自に「dirty フラグ」を持っており FUndoStack を直接使っていない panel が多い (FParticleEditorPanel の `_dirty`、FInspectorPanel の `_dirty` 等)。

将来: `FEditorWorkspace` が 1 個の `FUndoStack` を保持し、全 panel の編集アクションが `Push(New<XxxCommand>(...))` で集約される形に refactor。各 panel に `OnUndo() / OnRedo() noexcept` virtual hook を追加 (`EditorPanel.h` の「将来拡張余地」コメントに既に予約)。

### 7.2 ParticleEditor / SceneInspector を FEditorPanel 基底に refactor

Phase 19b ParticleEditor と Phase 20 SceneInspector は **Phase 21a 共通基盤の前** に実装されたため、`FEditorPanel` を継承していない。今後の refactor で:

- `FParticleEditorPanel` → `editor_core::FEditorPanel` 継承
- `FHierarchyPanel` / `FInspectorPanel` / `FEditorToolbar` → 同上

これにより `FEditorWorkspace::RegisterPanel(&panel)` で統一的に dispatch できるようになり、ParticleEditor も `OnAssetSelected` で `.fxedit` ファイルを自動 load する道が開ける。

### 7.3 panel 間 dependency declaration

現状: `FEditorWorkspace` の panel 配列は登録順 = dispatch 順で、依存関係を明示する仕組みは無い。

将来 (`EditorPanel.h` 末尾「将来拡張余地」より):
```cpp
virtual u32 GetDependencyMask() const noexcept;
```
panel ごとに「依存先 panel」を bit flag で宣言し、Workspace が dependency 順に OnFrameBegin / DrawUI を呼ぶ schedule を組む。例: ModelViewer は FAssetBrowser に依存 → FAssetBrowser を先に Tick。

### 7.4 shortcut key dispatcher

現状: 各 panel が独自に ImGui::IsKeyPressed をチェック (例: BehaviorTreeEditor の Step ボタンに割り当てるキーは無い)。

将来: `FEditorWorkspace` 内に shortcut key dispatcher を追加し、`Ctrl+S` → 全 panel の `OnSaveLayout`、`Ctrl+Z` → FUndoStack.Undo() を統一処理。各 panel に `OnKeyShortcut(KeyCombo combo) noexcept` virtual hook を追加 (`KeyCombo` 型は Phase 21b で input/ 配下に予定)。

### 7.5 ImGui docking branch 統合

現状: `FEditorWorkspace::DrawDockSpace()` は `#ifdef IMGUI_HAS_DOCK` guard で囲まれており、ACS の現状 (master branch ImGui) では no-op になる。各 panel は通常の float window として並ぶ。

将来: Phase 21c 以降で ImGui docking branch (`docking` 公式 fork) に切替えると自動的に有効化される。`SetDockTarget(bool)` ヒントを DockSpace に強制反映する仕組みも Phase 21c で追加予定。

### 7.6 multi-window (sub-viewport)

ImGui docking branch の `ImGuiConfigFlags_ViewportsEnable` を有効化すると、各 panel を OS ネイティブ window として外に飛ばせる。デュアルモニタ workflow の editor (Unity / Unreal の "Detach Panel") に対応するため、Phase 21c で:
- `FEditorWorkspace` が `_enable_viewports` フラグを持つ
- panel ごとに `WantsExternalViewport() const` virtual hook を追加

### 7.7 editor の AssetPack 統合 (Phase 23 並行)

Phase 23 で AssetPack (`.acpak` 形式) が実装されると、FAssetBrowser が「実 file system + AssetPack overlay」の 2 層を統合表示する必要がある。

- `FAssetBrowser::Init(const wchar_t* root)` に加えて `MountAssetPack(const wchar_t* pak_path)` API 追加
- `FAssetEntry` に「source: filesystem / assetpack」フラグ追加
- AssetPack 内 entry はアイコンを変えて視覚的に区別

### 7.8 残: NodeGraph 系 editor

Phase 22 で実装した editor 群 (FBehaviorTree / AnimCurve) は「ツリー or 1D curve」だが、ACS が UE5 級グラフィックを目指す上で **NodeGraph 系 editor** が複数必要:

| 予定 editor | 編集対象 | 想定 phase |
|---|---|---|
| FBehaviorTree graph 編集 (= btedit v2) | Selector/FSequence の子追加 / 配置入替 | Phase 24+ |
| FAnimation StateGraph editor | state node + transition arrow | Phase 24+ |
| MaterialGraph editor | shader node (UE Material Editor 風) | Phase 25+ |

共通基盤として `editor_core/NodeGraphCanvas.h` (仮称) を Phase 24 で追加予定。

### 7.9 Cinematics timeline keyframe drag

Phase 23 CinematicsEditor が完成すると、timeline 上のキーフレーム drag (Unity Timeline 風) が必要になる。AnimCurveEditor の canvas + 8 handle の paradigm を流用して `cinetimeline/TimelineCanvas.h` (仮称) を実装予定。

### 7.10 SpriteAtlas SDF / 9-slice 編集

現状 SpriteAtlasEditor は frame rect (x/y/w/h + pivot) のみ。Phase 22+ で:
- **SDF rendering** mode (font glyph 風) のメタデータ編集
- **9-slice border** (UI で stretch する際の corner/edge 不変領域) の border 編集 (4 値: top/right/bottom/left)

将来の `FMaterialOverride` 型に texture path swap を追加するのと同じ pattern で `SpriteFrameMetadata` 構造体を拡張する。

---

## 8. メモリ参照 / Memory References

詳細な仕様 / 設計判断 / Phase 履歴は以下の MEMORY.md エントリ + docs を参照:

### 8.1 ドキュメント

- **`docs/GameFramework.md` §15.4** — 著作ツール深度 & 外部ミドルウェアシーム (Pillar K 拡張)
  - 4 不変条件: (a) Pillar K seam 上に構築・重複実装しない (b) v1 はすべて in-engine UiKit (c) FMOD/Wwise seam (d) Cinematics editor は FCinematicsDirector 上に被せる
  - Phase 56〜58 (著作ツール深度): FParticle Editor → BT visual editor + Level editor → FMOD/Wwise seam + Cinematics editor
- **`docs/StyleGuide.md`** — ACS Coding Style Guide v1
  - §1 基本不変条件 (No STL / No exceptions / No RTTI / TResult<T,E> / canonical FCallback)
  - §2 命名 (PascalCase / snake_case / E-prefix enum / I-prefix interface)
- **`docs/ARCHITECTURE.md`** — ACS 全体アーキテクチャ
- **`docs/QUICKSTART.md`** — beginner-UX 入門 (Phase 4 配布パッケージング含む)

### 8.2 user MEMORY.md エントリ

- `project_acs_overview` — ACS の overview (軽量 C++ ゲームフレームワーク / Win/DX12 / STL 不使用)
- `project_gameframework` — GameFramework Pillar A〜W + メタ層 + 完成度 9 + ジャンルキット 7 の進捗
- `project_acs_coding_conventions` — Phase 0 完了 = `.clang-format` / `.clang-tidy` / `.editorconfig` / `.gitattributes` / `LICENSE` (Apache-2.0)
- `project_phase20_roadmap` — Phase 20〜36-3 ロードマップ (Phase 19b/20/21a/21b/22 = editor 関連)

### 8.3 関連ヘッダ (editor 群の隣接モジュール)

- `src/gameframework/Node2D.h` — FScene グラフのノード (FHierarchyPanel が表示対象)
- `src/gameframework/InspectorSeam.h` — IInspectableProvider / FInspectableField / EFieldKind (FInspectorPanel が描画対象)
- `src/gameframework/ParticleEffectSystem.h` — FParticleEmitterDef (FParticleEditorPanel の編集対象)
- `src/gameframework/AnimationCurve.h` — Hermite/Linear/Step curve (FAnimCurveEditorPanel の編集対象)
- `src/gameframework/BehaviorTree.h` — FBtSelector / FBtSequence / FBtAction (FBehaviorTreeEditorPanel の観察対象)
- `src/gameframework/Tilemap.h` — multi-layer u16 FTileId grid (FLevelEditorPanel の編集対象)
- `src/gameframework/SpritePack.h` — atlas + frame rect (FSpriteAtlasEditorPanel の編集対象)
- `src/gameframework/DebugDraw.h` — 2D line buffer (FEditorGizmo が出力先として使用)
- `src/platform/FileSystem.h` — Win32 CreateFileW ベース I/O (FFxeditSerializer / FEditorTheme / FEditorWorkspace の永続化)
- `src/render/PostProcess.h` — ETonemapMode (FModelViewerPanel が u32 で受け流し、Sample 31 renderer で変換)

---

**改訂履歴 / Revision History**:
- v1 (2026-05-24): 初版。Phase 19b〜Phase 22 の 7 editor + editor_core 8 コンポーネントを統合的に整理。Phase 23 (FontEditor / CinematicsEditor) は予定欄のみ。
