# ACS Editor Production Roadmap

## Scene/world transition status (authoritative clarification)

- Accepted target: one scene/world document identity. Perspective,
  Orthographic, and the 2D authoring workspace are presentation/tool presets,
  not alternate project scene identities.
- Current implementation: new 2D/3D projects share `Assets/main.acs3d`, while
  native `AScene`/`CSceneNodeGraph`, their serializers, physics, and renderers remain
  separate compatibility subsystems. `SceneWorldDocumentEnvelope` supplies an
  atomic Editor transaction; it is not yet a canonical mixed-world serializer.
- Existing `.acscene` content remains an explicit Orthographic-only legacy
  adapter. View switching does not convert it, and ACS does not yet ship an
  automatic `.acscene` converter.
- Startup without a publishable source uses a blank unpublished ACS3D world in
  Perspective, keeps the native host suppressed/hidden, and never presents a
  legacy/default 2D frame.
- Scene View owns the editor navigation camera. Game View and Play resolve
  authored game cameras independently; camera ordering is deterministic.
- Camera View V1 supports eight logical leases but one physical shared
  swapchain presenter. Simultaneous live outputs, dedicated offscreen targets,
  and asynchronous readback remain future work.

Required migration gates are: an explicit non-destructive converter with
preflight and loss reporting; stable asset-identity policy; atomic
failure/rollback; a canonical root that can contain dedicated 2D and 3D
domains; golden semantic round-trip/recovery tests; first-present pixel/CRC
coverage; and Play/standalone/package parity before legacy adapter retirement.

更新日: 2026-07-27

## 目的

この文書は、ACS Editor を「見た目だけ UE に似せたツール」ではなく、ゲームの作成、反復確認、ビルド、パッケージ、配布までを一貫して完了できるプロダクション用エディタへ発展させるための実装ロードマップである。

UE と同等という表現は、単一機能の有無ではなく、次のワークフロー品質を目標とする。

- 編集した内容が、Play、Standalone、Package の全経路で同じ結果になる。
- クラッシュや誤操作から作業を復旧できる。
- アセットの移動や改名で参照が壊れず、依存関係を追跡できる。
- すべての主要編集操作を Undo/Redo できる。
- 大規模プロジェクトでも検索、ロード、プレビュー、ビルドが予測可能な時間で完了する。
- エラーを「ビルド成功」に見せず、原因と修正箇所をエディタ上で説明できる。
- 配布物を再現可能な手順で生成し、起動確認まで自動化できる。

本調査は 2026-07-29 時点の WPF Editor、Editor ABI、ランタイム、既存ツール群を対象にした。記載する行番号は調査時点の証跡であり、コード変更により移動する可能性がある。

## 結論

ACS Editor には、シーンアウトライナー、詳細パネル、2D/3D ビューポート、Play 制御、Blueprint、ノードベースの Material Editor、プロジェクト設定、リフレクションベースのコンポーネント編集など、エディタの核になる機能がすでにある。

以前の最優先ブロッカーだった新規 3D authoring と実行・配布の二重経路は、2D/3D 共通の `Assets/main.acs3d`、manifest の canonical Scene Asset ID、3D runtime bootstrap、required-only dependency-closure Cook によって vertical slice が成立した。2D template は別 Scene 形式ではなく、同じ 3D world を XY Orthographic で開く preset である。現時点の最重要課題は、runtime/package adapter の対応範囲を Editor authoring 全体へ広げること、旧 project を安全に移行すること、共通 document/transaction と製品配布基盤を完成させることである。

### 現状スコアカード

| 領域 | 現状 | プロダクション上の主要不足 | 優先度 |
|---|---|---|---|
| シーン / Outliner | 新規2D/3D共通の `.acs3d` world、Orthographic preset、legacy adapter、階層、検索、複数選択、DnD、Scene Save All、自動保存・復旧あり | runtime adapter の全 authoring directive 対応、3D sibling reorder、subscene | P0 |
| Details / Components | Transform、コンポーネント追加削除、反射プロパティ、CallInEditor、3D複数選択のper-axis mixed value・atomic Transform/Enabled/Color編集・Transform reset、共通反射component propertyのmixed/sparse/atomic編集とschema default resetあり | copy/paste/diff、配列・構造体編集、component add/remove/reorderの複数編集、legacy 2D parity | P1 |
| Asset Browser | Import、texture/mesh/audio設定UIとcanonical recipe identity、source-preserving worker、hash検証付きprocessed-import DDC、監視、current/subfolder/all-assets 検索、永続 thumbnail DDC、配置、Material 変換、永続 GUID/DB、依存・参照元、Reference Viewer あり | tag filter、native texture/mesh/audio transcoder、詳細progress | P0 |
| Undo / Redo | Scene、Blueprint、Material graph に個別 snapshot 履歴。Scene 自動保存・復旧、Material graph と Project Settings の共通 host transaction 参加あり | cross-document command routing、Asset の履歴、履歴表示 | P0 |
| Play / Preview | Play/Pause/Step/Stop、状態復元、独立したScene/Game camera、複数authored Camera、frustum表示、単一shared-swapchain Camera View、対応済み `.acs3d` runtime bootstrap あり | runtime adapter の全3D directive parity、dedicated offscreen Camera View/同時live PIP、Possess/Eject/Simulate、Play 設定、複数クライアント | P0/P1 |
| Build / Package | Windows x64、Development/Test/Shipping、canonical Scene Asset ID起点のdependency-closure Cook、`.acpak`/ZIP、native verify、製品metadata付きPE VERSIONINFO、application manifest、3D fail-closed、private stagingでのhidden first-frame launch smoke、deterministic成功report あり | icon/license/release channel、署名、installer、継続smokeのCI運用、他 platform | P0 |
| Project Settings | schema 駆動 UI、検索、検証、未知キー保持、Document Host の dirty/Undo/Save All/close、非同期 atomic 保存、snap 値同期あり | Editor/User 設定分離、Input/Packaging/Platform 等 | P0/P1 |
| Material Editor | typed node graph、semantic Undo/Redo、Document Host dirty/Save All/close、compile diagnostics、CPU-safe async/cancellable preview、192/256/384 px 出力、同一入力 LRU cache、計測表示あり。native preview API に HDR/ACES、1x–4x SSAA、3種 mesh/background 契約あり | legacy property writer の transaction 化、native API を live window へ接続する fenced GPU readback、instance/function、shader cache | P0/P1 |
| Profiler / Diagnostics | Profiler v5 の CPU/GPU/pass/実フラスタムカリング計測、capture reset serial/presented-frame境界、normal/depth・motion・opaque PBR・interactive water・refraction共通submission traversal、aggregate fast pathと可視range結合、Shadow/VXGIの非camera-mask契約、UI stall watchdog、独立した cloud-workload-v1 の dispatch/invocation/history/sample ceiling 表示あり | GPU capture、allocation tracker、platform telemetry、継続 performance budget | P1 |
| Prefab / Blueprint | 保存、配置、Apply/Revert、graph undo あり | property override、nested prefab、variant、conflict/diff、安定 ID | P2 |
| Navigation | 2D grid A* と Tilemap bridge あり | 3D navmesh、bake UI、agent/area/link、debug/cook | P2 |
| Editor UX | メニュー、toolbar、panel toggle、主要 shortcut、command palette、per-user layout 永続化、6 stable-ID toolの個別float/hide/restore、複数同時float、DPI対応snapあり | document/tool tab tear-off、任意dock tree、multi-document、shortcut editor、複数workspaceの完全統合 | P1 |
| Release operations | Windows x64 deterministic ZIP、runtime 依存解決、Cook/`.acpak` native verify、manifest hash、manifest-to-PE metadata verify あり | icon/license/channel、署名、installer、patch、store upload、他platform | P0/P3 |

ステータスを要約すると、現在は「対応済みの Scene/Asset 範囲と基本製品 metadata を決定的かつ fail-closed に Windows x64 へ出荷できる開発用エディタ」であり、Editor の全 authoring 機能、icon/license/channel、署名、installer、継続 smoke を備えた統合環境にはまだ達していない。

## 最優先の残課題: 3D authoring と runtime/package adapter の完全 parity

新規 `3d` と `2d` project はどちらも `Assets/main.acs3d` を作成し、`2d` は同じ world を XY Orthographic preset で開く。manifest は初期 Scene の persistent Asset ID を保持し、Build/Run/Package は source format と project runtime capability を共通 guard で検証する。

- `editor/AcsEditor/ProjectManager.cs`
- `editor/AcsEditor/MainWindow.BuildCompatibility.cs`
- `editor/AcsEditor/CanonicalSceneAdapter.cs`
- `editor/AcsEditor/PackageCore.cs`

新規 project source は `FLegacyScene3DAdapter` bootstrap capability を宣言する。一方、旧 project がこの capability を持たない場合は、active `.acs3d` の Build/Run/Package を `ACS-BUILD-3D-PARITY-001` で停止し、別の 2D payload を黙って出力しない。Package の reversible `ACS3D v2` adapter も対応 directive だけを Cook し、未対応 content を structured diagnostic で拒否する。

### 残っている統一

1. `PLY3D`、`SPR3D`、`PFAB3D` はruntime/package adapterへ接続済み。引き続き未知命令をfail-closedに保ち、次のauthoring directiveは実行意味を定義してから追加する。
2. 旧 `.acscene` と capability marker を持たない旧 3D project の migration、round-trip、broken-reference recovery を継続検証する。
3. すべての Scene picker、Play、Standalone、Package が path ではなく canonical Asset ID を最終 authority とする。
4. multi-scene/subscene、level streaming、world partition を共通 document/dependency contract 上へ追加する。

### P0 受け入れ条件

- 新規 3D プロジェクトを作成し、Cube と Light を追加して Save、Editor 再起動、Play、Standalone、Package 後の起動で同じ hierarchy、component、transform を確認できる。
- Build 対象 Scene に未保存変更があれば Save All または明示的な中止を選べる。
- source 側の固定 filename に依存せず、Project manifest の Asset ID から既定 Scene を解決し、Cook 後だけを契約済みの `main.acscene` bootstrap path に正規化する。
- 旧 2D Scene の migration test と、新しい 2d/3d template が共有する
  ACS3D Scene document の round-trip test が CI で通る。
- 対応していない Scene 種別は Build を失敗させ、対象 Asset、期待形式、修正手順を Build Results に表示する。

## 現状監査

### 1. Editor Shell と ABI

WPF Shell は `.NET 10 / Windows / win-x64` で、Editor ABI は Raw DX12 構成に限定される。

- `editor/AcsEditor/AcsEditor.csproj`
- `engine/CMakeLists.txt:152-164`

managed/native 接続は `EngineInterop.cs` の P/Invoke と `EditorAbi.cpp` に集中しているが、接続前の数値 contract と capability negotiation は実装済みである。managed host は versioned `acs_editor_abi_query` へ必須 bit を渡し、provider version、既知/未知 capability、構造体 version/size を検証する。packed 256-byte の Profiler v5（先頭 224-byte は v4 compatibility prefix）、packed 168-byte の `cloud-workload-v1`、packed 60-byte の `camera-view-requests-v1` snapshot、packed 256-byte の `optional-service-diagnostics-v2`（先頭 192-byte は v1 compatibility prefix）は独立した optional capability であり、一方を追加しても既存 payload を再解釈しない。

- `editor/AcsEditor/EngineInterop.cs`
- `editor/AcsEditor/EditorAbiContract.cs`
- `editor/AcsEditor/EditorCloudWorkload.cs`
- `src/editor_abi/EditorAbiCapabilities.h`
- `src/editor_abi/EditorCloudWorkload.h`
- `src/editor_abi/EditorCameraViewRequests.h`
- `src/editor_abi/EditorServiceDiagnostics.h`
- `src/editor_abi/EditorAbi.cpp`

旧 DLL、version mismatch、必須 capability 不足、bad image は native host 作成前に fail-closed となる。optional service は enabled/disabled/pending/inactive/failed と exact reason、bounded UTF-8、typed error domain/code、host/diagnostic generation を query でき、Profiler、Cloud workload、Camera View request の UI は service 単位で利用可否を反映する。残る課題は native error と managed operation ID の相関、汎用 async job/cancellation ABI、renderer backend 抽象化、残る optional service への同じ UI 境界の適用である。

### 2. Scene、Outliner、Viewport

実装済み:

- 2D hierarchy、visibility、node icon、collapse state。
- 名前検索と祖先表示。
- Ctrl 複数選択、2D の before/after/child DnD、3D reparent。
- picking、orbit、pan、zoom、gizmo、box selection。
- move/rotate/scale snap、focus、frame、align、distribute。
- Scene dirty 判定、New/Open/Close 時の保存確認。
- Scene View cameraとauthored Game Cameraの分離、複数Camera component、
  deterministic active camera、Scene Viewだけのfrustum表示。
- 最大8件のlogical Camera View request。opaque ABA-safe ID、stable camera
  identity、requested/presented extent、target/history generationを追跡し、
  Scene置換時はstale化して再検証する。現在のlive presenterは既存
  shared swapchainを排他的に使う1件だけである。

主な証跡:

- `editor/AcsEditor/MainWindow.xaml:202-302`
- `editor/AcsEditor/MainWindow.xaml.cs:634-735`
- `editor/AcsEditor/MainWindow.xaml.cs:839-970`
- `editor/AcsEditor/EngineViewport.cs`
- `editor/AcsEditor/MainWindow.Workspace.cs:19-185`

不足:

- 3D sibling order の編集。
- WASD/fly camera、speed modulation、camera bookmark、local/world/pivot mode。
- multi-scene/subscene、level streaming、world partition の編集 UI。
- recent scene、broken-reference recovery。
- selection set、folder/collection、Outliner column/filter、lock/isolate。
- dedicated offscreen target、async readback、viewごとのpost-effect history、
  複数同時live Camera View/PIP。

### 3. Details と Component 編集

実装済み:

- 3D の Transform の下に Components が並び、Mesh Renderer も通常の native component card として配置される。
- 2D/3D の component add/remove、反射 property editor、category、CallInEditor button。
- Details 内検索。
- 3D複数選択の共通Transform/Enabled/Mesh Renderer Color、成分単位のmixed value (`—`)、編集軸だけを対象にするatomic batch、失敗時全rollback、1操作1 Scene Undo/Redo、Transform reset。
- 3D複数選択の共通反射component/property intersection、bool tri-state、integer/enum/object-reference/float/vectorのmixed value、vectorのsparse axis編集、schema default reset、全target preflight/readback/rollback、1操作1 Scene Undo/Redo。duplicate component type、schema drift、unsupported string、非finite/inexact値はmutation前にfail closed。

主な証跡:

- `editor/AcsEditor/MainWindow.Details.cs:16-65`
- `editor/AcsEditor/MainWindow.View3D.cs`
- `editor/AcsEditor/InspectorMultiEditContract.cs`
- `editor/AcsEditor/InspectorMultiEditSelfTest.cs`
- `editor/AcsEditor/InspectorReflectedMultiEditContract.cs`
- `editor/AcsEditor/InspectorReflectedMultiEditSelfTest.cs`
- `docs/DetailsMultiEdit.md`
- `editor/AcsEditor/MainWindow.xaml.cs:2706-2995`

現在の 2D 複数選択 Details は表示専用で、編集を無効化している。

- `editor/AcsEditor/MainWindow.xaml.cs:2226-2264`
- `editor/AcsEditor/MainWindow.xaml:530-532`

不足:

- legacy 2D adapterの複数選択編集。
- copy/paste property/component、component add/remove/reorderの複数選択編集。
- prefab/source/default との差分表示。
- array、map、nested struct、object reference、soft reference の一貫した editor。
- property validation、asset picker の type filter、browse/use-selected。
- Details lock と複数 Inspector。

### 4. Asset Browser と Asset Registry

Asset Browser はファイル監視、Import、current folder / subfolders / all Assets の明示的な検索スコープ、サムネイル、配置に加え、隣接 `.acsmeta` の安定 GUID と `Assets/.acsdb/index.v1.json` の決定的 index を使う。全 Assets 検索は UI thread から filesystem を再走査せず、immutable DB snapshot を background task で絞り込む。DB は source/importer/version/import settings、SHA-256、依存 Asset ID を保持し、Reference Viewer は直接・推移依存と参照元、欠損 ID、循環を読み取り専用で表示する。

New Asset の実装は認識済み ACS 形式だけに閉じている。ツールバーと背景コンテキストメニューから Folder、Material (`.acsmat`)、統一 Scene (`.acs3d`)、Blueprint (`.acsbp`)、Prefab (`.acsprefab`) を作成でき、作成後は新規項目を選択してインライン rename を開始する。payload は同一ディレクトリの一時名へ完全に書いてから no-overwrite move で公開し、Windows 予約名、大小文字を無視した payload/metadata/material-graph 衝突、`Assets/.acsdb` と一時領域、reparse point、Assets 外への書き込みを拒否する。キャンセルまたは serializer 失敗時は material graph と metadata を含む一時 family を除去する。

検証入口は `--asset-creation-selftest`。同じ契約は `--asset-browser-selftest` の aggregate にも含まれる。

- `editor/AcsEditor/AssetBrowserPanel.xaml:12-117`
- `editor/AcsEditor/AssetBrowserPanel.xaml.cs:68-197`
- `editor/AcsEditor/AssetBrowserPanel.xaml.cs:225-316`
- `editor/AcsEditor/AssetDatabase.cs`
- `editor/AcsEditor/AssetReferenceViewerWindow.xaml.cs`
- `tools/acsassetdb/Program.cs`

runtime の `FAssetRegistry` は loader と path-hash cache であり、editor database とは責務が異なる。現在の editor database では GUID が identity であり、DB API 経由の move と一意 content hash による外部 rename recovery で identity を維持する。

- `src/asset/AssetRegistry.h:47-168`
- `src/asset/AssetRegistry.cpp:90-98`

不足:

- importer ごとの native texture/mesh/audio transcoder と詳細progress。source-preserving workerとImport/Reimport共通pipelineは実装済み。
- tag filter と検索条件を再利用できる dynamic collection。
- 追加 asset type の作成を公開する場合の canonical serializer と schema migration。
- processed-import DDCのhit/miss/再構築cache observability。

Asset DB より先に自由な rename/move/delete を追加すると参照破壊を UI から簡単に起こせるため、順序を逆にしてはならない。

### 5. Undo、Redo、Document

Scene は native snapshot を最大 128 件保持し、drag 中の連続操作を一つにまとめる。Blueprint Editor は別の snapshot 履歴を最大 100 件保持する。

- `src/editor_abi/EditorAbi.cpp:1585-1598`
- `src/editor_abi/EditorAbi.cpp:7716-7774`
- `editor/AcsEditor/MainWindow.xaml.cs:3108-3124`
- `editor/AcsEditor/BlueprintEditor.xaml.cs:3383-3416`

Scene は dirty 状態に連動する世代管理付き自動保存、checksum 検証、起動時 recovery dialog と、ACS3D / legacy `.acscene` adapter の初期化済み dirty 文書を view mode switch なしで原子的に保存する Save All を持つ。共通の deterministic/async Document Host と Scene adapter は実装済みで、Scene の dirty、Save、Close を host 経由で扱える。Material Editor の authored Substrate graph も stable Asset ID、dirty、Save All、共通 close confirmation、graph gesture transaction まで host に参加した。Project Settings も settings file identity、unknown key 保持、dirty、transaction、非同期 atomic Save All、共通 close と fail-closed restore latch まで host に参加した。legacy material property writer、Material autosave/recovery、Blueprint/Prefab adapter、multi-document tab UI、cross-document command routing はまだ移行作業として残る。

- `editor/AcsEditor/MainWindow.Autosave.cs`
- `editor/AcsEditor/SceneAutosaveStore.cs`
- `editor/AcsEditor/SceneRecoveryDialog.xaml.cs`
- `editor/AcsEditor/MainWindow.SaveAll.cs`
- `editor/AcsEditor/SceneSourceFile.cs`

不足:

- `EditorDocumentHost` への Blueprint/Prefab adapter、Material legacy property adapter、multi-document tab、`IEditorCommand`、cross-document command routing。
- property delta と object identity に基づく Undo。
- Scene、Material、Blueprint、Prefab、Settings、Asset operation の共通履歴。
- Undo History UI、coalescing policy、transaction test。
- Scene 以外の document autosave/recovery と共通 close confirmation。

### 6. Play、Game View、Hot Reload

Play/Pause/Resume/Step/Stop、native physics、reflect DLL の user logic、停止時の Scene 復元、Game View、Standalone 起動、source watcher による Hot Reload は存在する。

- `editor/AcsEditor/MainWindow.xaml.cs:1069-1135`
- `editor/AcsEditor/MainWindow.xaml.cs:1787-1906`
- `src/editor_abi/EditorAbi.cpp:6954-6975`

不足:

- 3D Scene と packaged runtime の parity。
- Selected Viewport、New Window、Standalone、Simulate の明確な Play mode。
- Possess/Eject、spawn location、game instance 設定。
- runtime world と editor world の分離表示。
- multiplayer / multi-client launch。
- breakpoint、frame profiler、memory/GPU capture への導線。

### 7. Build、Package、Distribution

現状の Build は CMake configure/build、reflect DLL build、Release executable build、Scene copy、external launch までを行う。Package の vertical slice はWindows x64 Release executable、解決済みruntime DLL、ConfigとCook済み`game.acpak`をstagingし、native CRC verify、pack SHA-256、content build ID、固定timestamp/orderのdeterministic ZIPをatomic replaceで生成する。private staging copyにはproject名/versionとpublisher/description/copyright/support URLをcanonical PE VERSIONINFOとして埋め込み、既存のcompatibleなapplication manifestは保持し、欠落時はAMD64 asInvoker manifestを生成する。archive verifyはpackage manifestから期待値を再構成してPE resourceとの完全一致を検証する。Development/Test/Shipping profileを持ち、Test/Shipping runtimeはpackからscene/material/textureを直読する。Cook は manifest の canonical Scene Asset ID を唯一のrootとしてAsset DB dependency closureだけを決定的順序で収集し、未使用Assetを除外する。到達可能な欠損・循環・path escape・stale metadata・未対応形式はstructured diagnosticでfail-closedとなり、3D runtime bootstrap capabilityを検証できない旧projectのactive 3D documentは共通guardでfail-closedになる。

- `editor/AcsEditor/BuildService.cs:41-127`
- `editor/AcsEditor/PackagingService.cs:15-101`
- `editor/AcsEditor/PackageCore.cs`
- `editor/AcsEditor/PackageExecutableContract.cs`
- `editor/AcsEditor/PackageExecutableMetadata.cs`
- `editor/AcsEditor/MainWindow.BuildCompatibility.cs:12-71`
- `editor/AcsEditor/MainWindow.xaml.cs:1787-1837`
- `tools/acspackage/Program.cs`

既存の `acs_assetpack` のrecursive pack、list、unpack、verify、info、LZ4を再利用し、packer固有形式を新設していない。入力順はvirtual path ordinalへ固定し、reparse pointもCLI自身が拒否する。

- `tools/acs_assetpack/main.cpp:59-99`
- `tools/acs_assetpack/main.cpp:305-`
- `docs/AssetPack.md:183-210`
- `docs/AssetPack.md:335-336`
- `engine/CMakeLists.txt:201-206`

不足:

- icon source、license、credits、release channel 等の追加製品 metadata。
- signing、installer、delta/update 方針。
- packaged executable の継続的なautomated smoke launch。
- cook/packageの詳細timing report。
- Windows 以外の target、または remote build contract。

### 8. Project Settings と Editor Preferences

Project Settings は native schema catalog から UI を生成し、検索、category、validation、apply を行う。Rendering、Editor snap、Physics、Game の一部設定がある。変更は stable settings document の transaction と dirty state へ入り、Save/Save All/owner close が共通の非同期保存契約を使用する。managed preflight と native round-trip 照合は unknown/custom key を保持し、parse/write/restore rollback の不確定状態を fail closed にする。startup は project root→Config→exact file の reparse-safe snapshot と bounded parse を worker 上で行い、current generation の immutable result だけを Dispatcher 上の native state へ適用する。Build/Run/Standalone/Package は clean/dirty に関係なく Settings を共通 host gate で durable 検証してから進み、失敗、cancel、suspend、open transaction では開始しない。Package action は dialog 待機後にも同じ gate を再実行し、authoritative DefaultScene rewrite は verified native restore と `Project.InitialScene` の収束後にだけ成功する。さらに exact durable UTF-8 source の SHA-256 を Config Stage へ伝播し、canonical `ProjectSettings.ini` が一つだけ存在して hash が一致することを要求する。gate 後 Stage 前の unknown/custom key 編集は `CONFIG_CHANGED_DURING_PACKAGE`、Stage 後の編集は最終 source snapshot 検証で拒否する。

- `editor/AcsEditor/ProjectSettingsWindow.xaml.cs:47-76`
- `src/gameframework/ProjectSettings.cpp:25-125`

以前は native 側で Project Settings の snap 値を適用した後、WPF workspace 初期化が managed 既定値を再送していた。現在は native settings を managed toolbar state へ同期し、初期化時には `acs_editor_set_snap` を呼ばないため、この上書きは解消している。

- `src/editor_abi/EditorAbi.cpp:5220-5222`
- `editor/AcsEditor/MainWindow.Workspace.cs:33-79`
- `editor/AcsEditor/MainWindow.xaml.cs:452-457`

不足:

- Project Settings と per-user Editor Preferences の分離。
- Input mapping、Packaging、Platform、Audio、Localization、Source Control。
- last folder と viewport preference の per-user 永続化。workspace layout と
  panel size の per-user 永続化は実装済み。
- schema migration、settings diff、invalid setting の recovery。

### 9. Material Editor と Preview

Material Editor は typed node palette、graph canvas、compile diagnostics、PBR property、native GPU preview API、CPU-safe fallback を持つ。native API 側には linear HDR render target、ACES/sRGB resolve、1x/2x/4x SSAA、Sphere/Cube/Plane、Studio/Checker/Black background の契約があるが、共有 viewport RHI の resource-lifetime race を避けるため、現在の live window にはまだ接続していない。live window は単一 mesh/background の CPU-safe renderer と 192/256/384 px の出力解像度 preset、180 ms debounce を使い、未接続の mesh/background selector は無効化して理由を表示する。

- `editor/AcsEditor/MaterialEditorWindow.xaml:234-296`
- `editor/AcsEditor/MaterialEditorWindow.xaml.cs`
- `editor/AcsEditor/MaterialPreviewPipeline.cs`
- `editor/AcsEditor/MaterialPreviewSelfTest.cs`
- `src/editor_abi/EditorAbi.cpp:12144-12310`

第2 vertical slice では CPU generation を dispatcher 外へ移し、入力変更時点での cooperative cancellation と generation token による stale result suppression、同一入力の in-flight coalescing、8 entry LRU cache、generation time/cache hit 表示を追加した。出力解像度は immutable request key に含め、失敗時は last-good image を保持する。mesh/background は将来の fenced native GPU path 用 request contract として残すが、live window からは既定値だけを渡す。

不足:

- Material graph の既存 semantic Undo/Redo と host transaction を統合する共通 Undo History UI。
- Material Instance、parameter collection、function/subgraph。
- native GPU submit/readback の専用 queue、fence、非同期 readback、shutdown drain。
- compiled shader cache と runtime 基準 screenshot test。

### 10. Prefab と Blueprint

Prefab と Blueprint は subtree 保存、2D/3D 配置、source link、Apply/Revert を持つ。Blueprint graph には独自 Undo もある。

- `editor/AcsEditor/MainWindow.xaml.cs:2429-2601`
- `editor/AcsEditor/MainWindow.xaml.cs:2666-2751`
- `editor/AcsEditor/BlueprintEditor.xaml.cs:3383-3416`

現在の Prefab Apply は subtree 全体を書き戻し、他 instance を再生成する。property 単位の override model ではない。

不足:

- stable instance/object ID。
- property/component override と selective Apply/Revert。
- nested prefab、variant、inheritance。
- source 更新時の conflict、diff、rebase。
- break/relink、missing source recovery。
- Asset rename と dependency graph への参加。

### 11. Navigation

runtime には 2D grid A* と Tilemap からの walkability bridge がある。

- `src/gameframework/Pathfinding.h:46-147`
- `src/gameframework/TilemapNav.h:3-41`
- `src/gameframework/TilemapNav.cpp:11-31`

WPF Editor/Editor ABI には NavMesh authoring の統合が見当たらない。

不足:

- 3D navmesh generation。
- agent radius/height/slope/step、area/cost、modifier volume、off-mesh link。
- incremental bake、tile cache、bake report。
- viewport overlay、path query debug、invalid geometry visualization。
- runtime query filter、dynamic obstacle、crowd。
- cooked nav data と Scene dependency。

Navigation は Scene collider contract と Asset DB に依存するため、P0 基盤の後に実装する。

### 12. 既存 authoring tool の再利用

`src/gameframework/tools/` には animation curve、behavior tree、cinematic timeline、level、model、particle、sprite atlas、font などの既存 tool がある。しかし、主要 WPF workspace から統合されていない。

再実装を始める前に、各 tool を次の三層に分解する。

1. document/model と serializer。
2. preview/runtime adapter。
3. ImGui または WPF の view。

アルゴリズム、serializer、validation は再利用し、WPF workspace には document tab と command/transaction adapter を追加する。これにより UI の一貫性と既存資産の両方を維持できる。

## 目標アーキテクチャ

```mermaid
flowchart TD
    Shell["WPF Editor Shell<br/>Docking / Commands / Workspace"]
    Docs["Document Host<br/>Dirty / Save All / Autosave / Recovery"]
    Tx["Transaction Service<br/>Undo / Redo / History"]
    Sel["Selection and Details Service"]
    Assets["Asset Database<br/>GUID / Import / Dependencies / Thumbnails"]
    Preview["Preview Service<br/>Async / Progressive / Cached"]
    Build["Build and Package Orchestrator<br/>Cook / Stage / acpak / Verify / Smoke"]
    Bridge["Versioned Editor ABI<br/>Capabilities / Handles / Async Jobs"]
    Engine["Engine Runtime<br/>Scene / Renderer / Physics / Audio / Navigation"]

    Shell --> Docs
    Shell --> Sel
    Shell --> Preview
    Docs --> Tx
    Docs --> Assets
    Sel --> Tx
    Preview --> Assets
    Build --> Assets
    Docs --> Bridge
    Tx --> Bridge
    Preview --> Bridge
    Build --> Bridge
    Bridge --> Engine
```

重要な原則:

- UI control から直接 P/Invoke と file I/O を呼ばず、application service を介す。
- object pointer や path ではなく、stable document/object/asset ID を境界に使う。
- 長時間処理は job ID、progress、cancellation、structured diagnostic を持つ。
- Scene、Material、Blueprint、Prefab を同じ document lifecycle に参加させる。
- Editor 表示用 preview と Shipping renderer の shader/material contract を共有する。
- Build と Package は UI 専用ロジックにせず、CLI/CI から同じ pipeline を呼べるようにする。

## 実装ロードマップ

### P0-A: Scene と Runtime の統一（実装済み）

[ADR 0001: Single 3D scene document with Orthographic 2D authoring](adr/0001-single-scene-document.md)
で Scene identity、Orthographic 2D authoring、legacy adapter、package bootstrap
の境界を固定した。

実装済み成果物:

- versioned Scene schema と migration。
- 2D/3D 共通 document contract。
- Project manifest の default Scene Asset ID。
- Editor Play、Standalone、Package 共通 loader。
- 3D project template。

実装済み基盤:

- Initial Scene の移動追従を永続 journal に記録し、Project Settings と manifest の片側だけが更新された状態を起動時に検証して復旧する。
- `.acsproject` version 1 の bounded/strict UTF-8 contract、case-insensitive duplicate/property binding、未対応 version・不正 field type・zero GUID・Unicode format spoof の fail-closed 検証を共通 snapshot parser に統合した。BOM 付き legacy manifest の移動追従と journal recovery は canonical no-BOM 出力へ安全に収束する。
- `canonicalSceneAssetId` が空の旧 manifest は、起動時の共通 recovery lease 内で初期 Scene の authoritative `.acsmeta` を確定してから、未知 field を保持した atomic manifest 更新で一度だけ backfill する。欠落 Scene、path/ID 不一致、重複 identity は manifest を変更せず fail-closed とし、既に ID を持つ project では全 Asset scan を行わない。`--scene-save-selftest` が BOM/未知 field 保持、再試行の冪等性、競合 ID と欠落 Scene の拒否を固定する。

依存: なし。すべての最優先作業の起点。

固定済み検証:

- `--scene-editor-migration-selftest` が共通 `.acs3d` template、Orthographic view preset、transaction round trip、package bootstrap/source-format discrimination を固定する。
- legacy `.acscene` は明示的 adapter のまま保持し、view 切替では変換しない。
- 未対応 format、malformed envelope、path/Asset ID 不一致は fail closed にする。

### P0-B: Versioned Editor ABI と Service 境界（managed operation diagnostic 基盤まで実装済み）

成果物:

- 実装済み: 数値 contract version、feature bit、required/optional capability query、構造体 version/size 検証。
- 実装済み: legacy/future/missing capability の fail-closed smoke test と起動診断。
- 実装済み: Profiler v5 から独立した `cloud-workload-v1` snapshot。dispatch、logical/launched invocation、history、sample ceiling、skip reason を exact native workload から表示する。
- 実装済み: optional `camera-view-requests-v1`。1 hostにつき最大8件の
  logical request、slot generationを含むopaque ID、stable camera identityを
  registryで保持する。60-byte v1 snapshotはcamera node、requested/presented
  extent、target/history generation、latest frame metadataを公開し、Scene置換
  によるstale化とbind前の再検証を固定した。
  shared-swapchain presenterは排他的な1件で、別requestからの暗黙stealを拒否する。
- 実装済み: managed version-1 operation diagnostic。非zero operation GUID、service、severity、stable `ACS.*` code、message、optional Asset/path、連番、bounded aggregate、success/failure/cancel の単一 terminal を持ち、Build と Package で legacy log と並走する。
- 実装済み: optional `optional-service-diagnostics-v2`。192-byte v1 prefix と 256-byte v2 typed payload、service state/reason、bounded UTF-8、error domain/code、stable code、単調増加する host/diagnostic generation を持つ。malformed header は host を読む前に fail closed、managed は generation 不一致の late result と不正 UTF-8 を破棄する。
- 残り: native error と managed operation ID の相関、native async job API/cancellation、managed の残り service への適用。
- 残り: managed 側の Scene、Asset、Preview、Build service interface と、残る optional service への同じ単位の UI disable。
- 残り: Camera View用のdedicated offscreen target、fence付き非同期
  readback/presentation、viewごとのpost-effect history、複数同時live PIP。
  `camera-view-requests-v1` はこれらをadvertiseしない。

依存: P0-A と並行可能。新 Scene API の境界を先に定義する。

完了条件:

- 実装済み: Editor と ABI の version 不一致で crash せず、native host を作らず理由を表示する。
- 実装済み: optional feature を product label や symbol の推測ではなく capability で判定する。
- 実装済み: stale Camera View leaseのABA、Scene置換、presenter排他、
  current Sceneに対するstable-ID再検証をnative lifecycle testで固定する。
- 実装済み: managed Build/Package diagnostic を Build log の operation ID、Asset、path に紐づける。
- 実装済み: native optional-service error/state を versioned typed payload と generation で取得し、legacy prefix、malformed input、host再生成を lifecycle/self-test で固定する。
- 残り: native error も同じ managed operation に紐づけ、残る optional service の取得済み reason を UI に接続する。

### P0-C: Document、Transaction、Autosave（共通 host 基盤、Scene、Material graph、Project Settings 統合済み）

成果物:

- 実装済み: deterministic/async `EditorDocumentHost` と `EditorDocument`、Scene adapter、Scene の Save、Save All、dirty、close confirmation、Material graph adapter の stable Asset ID、dirty、Save All、close、gesture transaction。
- 実装済み: Project Settings adapter の stable file identity、unknown key 保持、transaction、dirty、worker task 上の atomic Save All、failed-save dirty 保持、owner close、restore rollback safety latch、startup generation/cancel/late-result safety、project-contained read、Build/Run/Standalone/Package durability gate、Package action の exact-byte SHA-256 と Config Stage の TOCTOU 検証。
- 残り: Material legacy property、Blueprint/Prefab adapter と multi-document tab。
- 共通 transaction scope と Undo History。
- autosave journal、recovery dialog、世代管理。

依存: stable object/document ID は P0-A/B と合わせる。

完了条件:

- Scene property、Material node、Blueprint node、Prefab override、Project setting を各 10 操作行い、順逆に Undo/Redo して byte-equivalent または semantic-equivalent に戻る。
- drag と text typing が policy に従って coalesce される。
- Editor process を強制終了して再起動し、最後の autosave から document を選択復旧できる。
- Save All 後は全 tab と Project の dirty state が一致する。

### P0-D: Asset Database、Import、Dependency

成果物:

- stable GUID と metadata。
- persistent Asset DB と schema migration。
- importer registry、import settings、source hash、reimport。
- dependency/referencer index。
- atomic rename/move と redirect/migration。
- thumbnail/derived data cache。

実装済み基盤:

- Import/Reimport の prepare/commit journal を Project 起動時に同一の変更リース内で復旧し、不完全なペイロードとサイドカーを Asset DB 走査前に確定する。
- 画像 decode を UI モデルから独立したワーカーへ分離し、decode 済みの不変な結果だけをディスパッチャーへ戻す。
- Asset Browser の image/material thumbnail は、content SHA-256、generator version、kind、requested edge を key にする永続 DDC を使う。schema-v2 envelope は key/length/payload hash に加え、固定 little-endian raw Pbgra32 header の version/format/dimension/stride/exact byte count を検証して atomic publish する。永続 bytes を in-process compressed-image codec に渡さず、旧 PNG schema と破損・stale entry は同品質で再生成する。managed disk budget は 256 MiB/4096 entries で、freshな別process tempをcleanupせず、世代 cancellation/latest-wins と path/reparse safety を維持する。Cook/thumbnail DDCのentry pathは各owner固有の512 key/256 Ki code unit LRUで共有し、request/hit/miss/eviction/bypass/保持量を診断できる。
- Asset View の手動 Import は texture/mesh/audio の設定 UI を publication 前に開き、strict/bounded な project-local profile を使う。importer/version/schema/normalized settings の destination-independent SHA-256 recipe と完全な設定辞書は既存 journal で `.acsmeta` へ原子的に公開され、DDC identity に参加する。folder drop は同じ immutable profile snapshot を再利用し、無効値やprofile保存失敗は source publication 前に fail closed となる。source-preserving worker v1はstaged sourceの全bytesをpublication前に検証し、source contentとcanonical recipeからpath-independent artifact keyを作る。`Temp/DerivedDataCache/AssetImports/v1` のstrict/bounded/hash-verified envelopeをatomic publishし、破損entryは無視して再構築する。Import/Reimportは同じworker/DDC経路を使う。native texture/mesh/audio transcodingと詳細progressは次段階とする。

依存: P0-A の Scene reference、P0-C の asset operation transaction と協調する。

完了条件:

- texture、material、prefab、scene の参照 chain を作り、元 texture を folder 間で移動・改名しても参照が維持される。
- source file 変更を検出し、reimport preview と apply/cancel を選べる。
- safe delete が referencer を列挙し、未解決参照を残す削除を既定で拒否する。
- Project 再起動後も GUID、import settings、dependency が一致する。
- 10 万 Asset 相当の index を fixture で検証し、検索が UI thread の全件列挙にならない。

### P0-E: Build Profile、Cook、Package、Smoke Test

実装済み vertical slice:

- Windows x64 Release build、runtime DLL dependency resolution。
- deterministic staging/ZIP、file hash manifest、atomic replacement。
- Package preflight/progress UI と active 3D fail-closed。
- Development/Test/Shipping profile。
- deterministic asset Cook、`acs_assetpack pack` + native `verify`。
- pack SHA-256/format/compression/source countを含むmanifest。
- project名/versionとpublisher/description/copyright/support URLを一つのcanonical PE VERSIONINFOへ発行し、package manifestとのround-trip一致をarchive verifyで検証する。既存のcompatibleなapplication manifestは保持し、欠落時だけdeterministic AMD64 asInvoker manifestを追加する。
- Test/Shipping runtimeのpack直読とno-loose-fallback。
- Editor と CI が共有する CLI/API。
- Package の事前検証をデバウンス付きの非同期ワーカーで実行し、公開直前に変更リースを再取得して Initial Scene journal とアセット identity を再検証する。
- `canonicalSceneAssetId` を唯一のrootにするrequired-only dependency closure。未使用AssetはCookの形式・参照検証とgraph hashから除外し、到達可能な欠損、循環、escape、stale metadata、未対応形式だけを明示的に拒否する。complete treeに対するmetadata authority、path、reparse-point safetyは維持する。空または不正なcanonical IDは旧pathへfallbackせず、Editorでのmigrationを案内してfail-closedにする。
- 公開済みZIPを変更せずprivate TEMPへ固定copyし、同じarchive verifierを通した後だけ上限付きで展開する。64桁nonceで認証されたruntimeはwindowをshow/activateせず、Scene startupと最初のsubmit/present成功後にready handshakeしてclean exitする。45秒deadline、cancel/timeout時のprocess-tree kill、8 MiB/streamのcapture、bounded cleanupをEditor/CLIで共有する。
- ZIP hash、package/build/profile/executable identity、各verify/extract/startup/exit/cleanup gate、stable diagnosticをschema-v1 package reportへatomic publishする。成功reportはtimestamp、nonce、TEMP path、観測時間を含まず同一入力・limitでbyte-identical。観測時間はEditor Build Resultsへ分離して表示する。

残り成果物:

- icon/license/release channel metadata、署名、installer。
- clean GPU workerでの継続smoke、複数adapter/GPUのmatrix、release artifact保管ポリシー。

依存: P0-A と P0-D が必須。P0-B の async job/diagnostic を利用する。

完了条件:

- Clean checkout から一つのコマンドで Shipping package を作成できる。
- package に未使用 Asset が入らず、必要 Asset の欠落は cook 時に失敗する。
- `acpak verify`、manifest hash 検証、別 staging directory からの executable 起動が成功する。
- 同一 source/config/toolchain から生成した manifest と logical asset hash が一致する。
- Build Results にpackage archive phaseとsmokeの所要時間、verify/readiness/exit結果、report pathを表示する。configure、compile、cook、stage、pack、verifyの個別timing分解は継続する。

### P1-A: UE 型 Workspace UX

成果物:

- 実装済み: `hierarchy`、`inspector`、`console`、`build`、`assets`、
  `profiler` の6 stable-ID registry。各toolを個別にfloat/hide/restoreでき、
  複数toolを同時にfloatできる。dockedな下部toolが1件以上なら1 active tabを
  共有する。`Ctrl+J` / View menuはaggregate presentationだけを折り畳み、
  子toolの配置状態とactive tabを変更しない。dock内Hideは選択toolだけを隠す。
- 実装済み: schema-v2のper-user placement、multi-monitor/DPI snap、
  monitor topology変更時のclamp、全6 panelとactive bottom tabを対象にする
  transactional reset。
- 実装済み: named workspaceとpanel sizeのper-user永続化。
- 残り: document/tool tab tear-off、任意dock tree、tab groupの分割・移動。
- multi-document tab、Recent、Favorites。
- command registry、command palette、shortcut editor。
- Output Log、Message Log、Build Results、Task Progress の統一。

依存: P0-B/C。

完了条件:

- Editor 再起動後に window、dock、tab、panel size、last active document が復元される。
- 全主要 command を menu、shortcut、command palette から同じ command ID で実行できる。
- 失敗した非同期 task から関連 Asset/setting/source location を開ける。

### P1-B: Outliner、Details、Content Browser

成果物:

- Outliner folder、type/tag filter、lock/isolate、3D reorder。
- Details mixed value、multi-edit、reset/copy/paste、diff、locked inspector。
- Content Browser folder tree、breadcrumb、recursive query、collection。
- Asset create/duplicate/rename/move/safe delete/reimport/reference viewer。

依存: P0-C/D。

完了条件:

- 異なる値を持つ 100 object の共通 property を一度に編集し、一回の transaction で Undo できる。
- Asset rename/move 中に Scene、Material、Prefab の参照が維持される。
- filter/query が index を利用し、UI thread を長時間 block しない。

### P1-C: Viewport、Preview、Play Workflow

実装済み camera/view vertical slice:

- Scene View cameraとauthored Game Cameraを分離し、複数Camera component、
  deterministic active camera、Scene Viewだけのfrustum表示を持つ。
- Camera Viewはnative child HWNDを1つのowned WPF windowへ移し、camera選択を
  Scene dirty、Undo、authored Activeから分離する。
- `camera-view-requests-v1` は最大8 logical requestのidentity/extent/generation
  lifecycleを提供する。managed Camera Viewは最大8 tabの追加、即時切替、閉じる、
  stable-ID再解決を実装し、各tabのrequested extentとhistory generationを分離する。
  Scene置換後は一意なstable IDだけを新nodeへ追従させ、曖昧な重複はfail closedとする。
  ただしlive描画は1つのshared-swapchain presenterだけで、logical request数を
  同時live view数として扱わない。

実装済み live preview vertical slice:

- 単一 mesh/background の CPU-safe material preview と 192/256/384 px 出力解像度 selector。
- native preview API の HDR/ACES、1x/2x/4x SSAA、Sphere/Cube/Plane、background 契約（live window には未接続）。
- 未接続の mesh/background selector の無効化と理由表示。
- 180 ms debounce。
- dispatcher 外の cancellable CPU generation と generation token による stale result suppression。
- 同一入力の in-flight coalescing、8 entry LRU cache、last-good image 保持。
- output resolution、generation time、cache hit/shared job の状態表示。

残り成果物:

- fly navigation、camera speed、bookmark、local/world/pivot mode。
- render mode、debug overlay、resolution scale、performance stats。
- Camera Viewのdedicated offscreen target、専用queue/fenceまたは同等の
  ownership、async readback/presentation、viewごとのTAA/cloud/depth/post
  history、複数同時live PIP。別capabilityを定義するまで未対応として扱う。
- native GPU preview の fenced async readback と compiled shader cache。
- Asset View preview への共通 async/cancellable/cache service 適用。
- Selected Viewport/New Window/Standalone/Simulate のPlay mode matrix。
- Possess/Eject、spawn location、Play setting。

依存: P0-A/B/C、preview cache は P0-D。

完了条件:

- Preview の入力中は操作を阻害せず、idle 後に選択品質へ収束する。
- stale generation は cancellation され、古い結果が新しい parameter を上書きしない。
- HDR/tonemap/AA 条件が明示され、preview と runtime の基準 screenshot test が許容差内に収まる。
- 各 Play mode の開始/停止後に Editor Scene が正確に復元される。

### P2-A: Prefab と Blueprint Production Workflow

成果物:

- stable instance ID と property/component override。
- selective Apply/Revert、diff UI。
- nested prefab、variant、break/relink。
- source update conflict と migration。
- Blueprint compile dependency と diagnostic link。

依存: P0-C/D。

完了条件:

- nested instance の一 property override を保持したまま source Prefab の別 property を更新できる。
- conflict を diff 表示し、source/instance/手動 merge を選べる。
- Asset rename、Undo、reopen、Package を通して instance identity が維持される。

### P2-B: 既存 Authoring Tool の Workspace 統合

対象:

- Behavior Tree。
- Animation Curve / Animation。
- Cinematic Timeline。
- Particle。
- Model/Sprite Atlas/Font。
- Level/Tilemap。
- Input Mapping。

依存: P0-B/C/D と P1-A。

完了条件:

- 各 tool が Document Host、Save All、Undo/Redo、Asset picker、dependency index に参加する。
- serializer/validation の core test を UI なしで実行できる。
- tool ごとに sample asset の open/edit/save/reopen/package smoke test がある。

### P2-C: Navigation Authoring

成果物:

- 2D grid navigation editor の可視化。
- 3D navmesh generator と agent profile。
- area、cost、modifier volume、off-mesh link。
- bake task、diagnostics、viewport overlay。
- cooked navigation asset と runtime query。

依存: P0-A/D、collider contract、P1-C viewport overlay。

完了条件:

- static geometry 変更で dirty tile のみ rebake できる。
- agent profile ごとの reachable/unreachable と path cost を viewport で検証できる。
- packaged game が Editor bake と同一 version/hash の nav data をロードする。

### P3: Release、Scale、Team Workflow

成果物:

- platform target と remote build contract。
- shader Derived Data Cache、PSO cache、distributed cook。
- Git/Perforce provider、checkout/lock、changelist。
- CPU/GPU/memory/frame profiler、crash symbol/report。
- Audio、Localization、Accessibility 設定と authoring。
- automated migration、golden screenshot、large-project performance suite。
- signing、installer/update、release metadata。

依存: P0 全体と P1 workspace。

完了条件:

- CI が editor-free command で cook/package/test できる。
- cache hit/miss と invalidation reason が観測できる。
- source-controlled Asset の rename/lock/conflict が Editor 上で説明される。
- release artifact、symbol、manifest、license、version、hash を一つの release record として追跡できる。

## 依存順

```text
P0-A Scene contract ─┬─> P0-C Document/Transaction ─┬─> P1 Workspace/Details
                    │                              └─> P2 Prefab/Tools
P0-B ABI/Services ──┼─> P0-C                       └─> P1 Preview/Play
                    └─> P0-E Build/Package
P0-D Asset DB ──────┬─> P0-E Build/Package
                    ├─> P1 Content Browser
                    ├─> P2 Prefab/Tools
                    └─> P2 Navigation
P0-A + P0-D + P0-E ───────────────────────────────────> P3 Release/Scale
```

作業を並行化する場合も、UI 拡張が基盤契約を先行して固定しないようにする。特に Content Browser の rename/delete、Prefab override、3D Nav、Package は Asset ID と dependency graph を前提にする。

## すぐ実施できる安全性の高い改善候補

以下は大規模設計を待たずに価値が出るが、P0 の代替ではない。

### 1. Snap setting の上書き修正（実装済み）

Project Settings 読み込み後に WPF の既定値を再送せず、native の値を query して managed toolbar state と同期するよう修正した。

検証:

- Snap Move を 25 に保存。
- Editor 再起動。
- UI 表示と実際の gizmo delta が 25 のままであることを確認。

### 2. 3D Build の誤成功を防ぐ guard（実装済み）

Scene pipeline 統一まで、3D document が active の場合は既存 2D `SceneText` を保存して成功扱いにせず、Build、Run、Package を共通 diagnostic code で fail-closed にした。

検証:

- 3D Scene 編集後の Build が、別の 2D Scene を暗黙出力しない。
- message から Project Scene 設定または対応 issue を開ける。

### 3. Material Preview の非同期化（live CPU 第2 vertical slice 実装済み）

live CPU-safe generation を dispatcher 外の latest-wins job にした。入力変更時に debounce を待たず旧 job を無効化し、renderer 内では行単位に cooperative cancellation を検査する。同一入力は実行中 job と凍結済み bitmap の 8 entry LRU cache を共有し、UI は出力解像度、generation time、cache hit/shared job を表示する。native API にある linear HDR、ACES/sRGB resolve、1x/2x/4x SSAA、Sphere/Cube/Plane、background 契約は、専用 queue/fence と async readback を備えるまで live window へ接続しない。

`--material-preview-selftest` と既存 `--material-workflow-selftest` aggregate は、同一入力 cache、PixelSize を含む cache identity、LRU 上限、in-flight coalescing、stale result suppression、debounce 前 invalidation、dispatcher-safe freeze、失敗境界を検証する。

残り:

- native GPU preview を viewport frame と競合しない専用 queue/fence と async readback に移す。
- compiled shader cache と preview/runtime 基準 screenshot test を追加する。
- Asset View の image/material preview を同じ scheduler contract へ統合する。

### 4. Workspace layout の per-user 永続化（初期実装済み）

panel show/hide/reset、window bounds、row/column size、visibility を versioned user-local JSON として保存する。破損値と monitor topology 変更は安全な既定値へ fallback し、Project file には混ぜない。明示registryは `hierarchy`、`inspector`、`console`、`build`、`assets`、`profiler` の6 stable IDで、各toolを個別にfloat・hide・restoreでき、複数toolを同時にfloatできる。dockedなConsole/Build/Assets/Profilerが1件以上なら1 active tabを共有する。`Ctrl+J` / View menuのaggregate suppressionは子toolの状態とactive tabを保持し、dock内Hideだけが選択toolをHiddenへ移す。placement schemaはv2で、owner/monitor端へ12 DIPのDPI対応snapを行う。unknown/duplicate ID、非finite配置、unsupported versionはfail closedにし、detach失敗はDockedへrollback、redock失敗はFloatingを維持する。document/tool tab tear-offと任意dock treeは次段階とする。

検証:

- panel resize/非表示後の再起動で復元する。
- 破損 JSON は既定 layout に fallback し、Project を壊さない。
- 負座標monitor、切断monitor、DPI変更後もfloating panelのタイトル領域を最近傍work areaへ復元する。
- float/re-dock失敗時にvisualが複製・消失せず、Scene dirty/Undo/Project設定を変更しない。
- schema-v2 snapshotは6 IDの欠落・重複・未知IDを一部適用せず拒否する。
- layout reset途中の失敗は全6 panelを開始時のDocked/Floating/Hiddenとactive bottom tabへ戻し、全成功時だけdefault配置をcommitする。

### 5. Save All、dirty indicator、close confirmation の統一（Scene と Material graph 統合済み）

deterministic/async Document Host と Scene adapter は実装済みで、単一 Scene document は ACS3D / legacy `.acscene` のどちらの adapter でも、view mode switch なしの Save All、原子的 source write、dirty indicator、close confirmation、自動保存・復旧を持つ。Material graph もstable Asset ID、dirty、Save All、owner close、gesture transaction、path-mutation suspension/rebindに参加した。Project Settings もstable file identity、unknown key保持、dirty/transaction、非同期atomic Save All、owner close、fail-closed restore latchに参加した。次にMaterial legacy property、Blueprint/Prefab adapterとmulti-document tabをhostへ移行し、共通command routingとautosave/recoveryを全documentへ拡張する。

検証:

- 複数種 document を変更して Editor を閉じると、対象ごとに保存/破棄/キャンセルを選べる。

### 6. Asset Browser の低リスク操作

永続 Asset DB と reference graph に加え、New Folder/ACS asset、breadcrumb、current/subfolder/all-assets の read-only search scope、Reveal in Explorer、transactional rename/move/duplicate、redirector、project-local Trash を実装済み。全 Assets 検索は immutable DB snapshot のみを worker task で評価し、Assets root 外の current folder/candidate は fail closed で除外する。新規作成は canonical serializer を持つ形式だけを公開し、同一ディレクトリの atomic publish と共通 project mutation lease を使う。今後追加する asset type も、拡張子だけを UI に足すのではなく serializer、index importer、open/edit 導線、rollback self-test を一組で導入する。

検証:

- symlink、`..`、大文字小文字差を含む path が project root 外へ書き込まない。
- payload/`.acsmeta`/material graph の大小文字差衝突で上書きせず suffix を選ぶ。
- キャンセル、serializer 失敗、予約 staging target で部分ファイルを残さない。
- current/subfolder/all-assets search が UI thread で filesystem を列挙せず、scope 変更中も stale materialization を公開しない。

### 7. ABI capability 表示

初期 vertical slice を実装済み。managed host は product label を解析せず、
versioned `acs_editor_abi_query` と capability bitmask で必須の frame-result、
incremental-startup、resize-result 契約を検証する。旧 DLL、version 不一致、必須
capability 不足、bad image は native host を作る前に fail closed とし、
About/起動診断へ backend、provider version、既知/未知 bit、欠落理由を表示する。
native lifecycle test と headless managed self-test で current/future/legacy/missing
capability を固定した。Profiler v5 は 256-byte version-5 とし、先頭
224-byte のversion-4 prefix要求も受理する後方互換contractとしている。実際のmain-view
frustum tested/visible/culled数と解決済みgame-camera nodeを追加した。volumetric
cloud の exact workload は独立した 168-byte `cloud-workload-v1` optional contract
として追加した。Cloud panel は dispatch、logical/launched invocation、history、
sample ceiling、skip reason を表示するが、品質設定や march count は変更しない。
Camera View requestは独立した `camera-view-requests-v1` optional contractで、
最大8 logical request、ABA-safe ID、registry内のstable camera identity、
Scene replacement時のstale化と再検証を固定した。60-byte snapshotはcamera
node、requested/presented extent、target/history generationを公開する。
managed側はNew、current Open、Undo/Redo/recovery、成功rollbackの公開境界で
stable IDを1回だけ再解決し、superseded/unpublished loadでは更新しない。
presenterは既存shared swapchainの排他的な1件だけで、
DedicatedOffscreen、async readback、per-view history、複数同時live PIPはこの
capabilityに含めずadvertiseしない。
main-view cullingはnormal/depth、motion、opaque PBR、interactive water、
refractionで同じproduction submission traversalを使う。normal/depthは
rejectなしならaggregate 1 drawを維持し、部分cull時だけ隣接visible rangeを
結合する。Shadowはlight-space、VXGIはworld-spaceなのでcamera maskから明示的に
除外する。perspective/orthographic plane contract、Profiler default値、
fake-RHI command記録、range overflow、利用可能なDX12 adapter上の実
`DrawScene3D` publicationをnative testで検証する。
TAA/SSR/SSGIは共通`TemporalHistory` policyでframe 0をcurrent-onlyにし、
Scene・logical camera owner・projection・view mode・Play復帰・明示camera cut・
品質toggleを不連続点として一括invalidateする。SSR/SSGIの無効化または
G-buffer prerequisite欠落も次回をcold-startさせる。
managed Build/Package は version-1 typed diagnostic、operation ID、bounded
aggregation、cancellation terminal を持つ。native optional service は
versioned typed error/state、legacy prefix、host generationを持つ。managed
EditorはProfiler、Cloud workload、Camera View requestごとにexact reasonを
取得し、managed/native host generationとservice別diagnostic generationを
照合してからUIへ公開する。診断capabilityが無いproviderは従来互換、広告済み
診断のpending/failed/stale/contract errorは対象serviceだけをfail closedにする。
Profiler ResetとCamera request mutationだけを個別disableし、Cloudのnative
snapshot queryだけを抑止する。Pause/既存capture export/Cloudのローカル表示
filter/Re-dock/closeとCameraの単一legacy previewは維持する。
次段階はmanaged operation IDとの相関とasync job/cancellation ABIである。

## 推奨する直近の着手順

1. 完了: 3D Build/Run/Package guard と snap 同期修正で、現状の誤動作を停止。
2. 完了: P0-A の Scene manifest/schema/loader を ADR 0001 と `SceneContractFixtureSelfTest` で固定。
3. 進行中: P0-B は ABI version/capability negotiation、`cloud-workload-v1`、`camera-view-requests-v1`、`optional-service-diagnostics-v2`、取得済みreasonによるProfiler/Cloud/Camera Viewのservice単位UI disable、managed Build/Package operation diagnostic を実装済み。native/managed operation ID の相関、native job cancellation、Camera Viewのdedicated offscreen/async presentation capabilityを追加する。
4. 進行中: P0-C は deterministic/async Document Host、Scene adapter、Scene Save All/autosave/recovery、Material graph adapter/transaction、Project Settings adapter/transaction/非同期保存を実装済み。Project Settings の startup snapshot/parse は worker 化され、generation/cancel/late-result gate、project root→Config→file containment、Build/Run/Standalone/Package durability gate まで実装済み。Material legacy property、Blueprint、Prefab、multi-document tabを追加する。
5. 進行中: P0-D は GUID/metadata/dependency index/Reference Viewer、reimport、safe rename/move/delete、global search、Cook DDC、Asset Browser thumbnail DDC、owner固有DDC path reuse診断、texture/mesh/audio importer設定UIとcanonical recipe cache identity、source-preserving worker、hash検証付きprocessed-import DDC、Import/Reimport parityを実装済み。native texture/mesh/audio transcoder、詳細progress、tag/dynamic collection、processed-import payload cache observability、10 万 Asset 規模の継続検証を追加する。
6. 進行中: P0-E はcanonical Scene Asset ID起点のrequired-only dependency closure、deterministic Cook/pack/native verify、manifest-to-PE製品metadata/application manifest、private stagingのhidden first-frame runtime smoke、deterministic成功reportまで実装済み。icon/license/channel、resource更新後の署名、installer、継続smokeのCI/GPU matrixを追加する。
7. その後に残るdocument/tool tab tear-offと任意dock tree、Content Browser、Details、Viewport、Prefab、Navigationを依存順に拡張する。

live Material Preview は debounce、dispatcher 外の cancellable latest-wins generation、in-flight coalescing、8 entry LRU cache、last-good image、192/256/384 px 出力まで完了した。HDR/ACES、SSAA、mesh/background は native preview API のみで、live window への接続には専用 queue/fence と async readback が残る。

## 完成の定義

ACS Editor を「ゲームを作って配布できる状態」と呼ぶ最低条件は次のとおり。

- 2D/3D の編集内容が Save、Play、Standalone、Package で一致する。
- Scene、Material、Blueprint、Prefab、Settings、Asset 操作に Undo/Redo、dirty、Save All、recovery がある。
- Asset は path ではなく stable identity を持ち、rename/move/reimport/dependency/safe delete が機能する。
- Development/Test/Shipping package を clean environment から生成し、verify と smoke launch を自動実行できる。
- Editor の各長時間処理に progress、cancel、structured diagnostic がある。
- 主要 authoring tool が共通 workspace、document、transaction、asset system に統合されている。
- 代表 sample project の edit-to-package end-to-end test と migration test が CI で継続的に通る。

この基盤を満たした後であれば、UE に寄せた UI/UX 拡張は単なる外観変更ではなく、実際の制作速度、復旧性、出荷品質に直結する。
