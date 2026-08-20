<!-- SPDX-License-Identifier: Apache-2.0 -->
# ACS GameFramework

`GameFramework` は、`App`、`Render`、`Ecs` などの下位モジュールをゲームで扱うための
高水準モジュールである。ゲームループ、シーン、ノード、コンポーネント、入力、時間、
描画補助、保存、ゲームプレイ機能を提供する。

このモジュールは一つの巨大な管理クラスやサブシステムへ機能を集約しない。状態と処理は
責務ごとの型へ分け、共有所有・寿命・更新順序が必要な機能だけをサブシステムとして
owner の寿命へ接続する。

## モジュール境界

`src/gameframework/GameFramework.Build.cs` が正規の source manifest であり、生成された
`src/gameframework/Module.cmake` が CMake target `ACS::GameFramework` を定義する。
公開依存は次の ACS モジュールである。

- `Foundation`、`Memory`、`Container`、`Subsystem`
- `Threading`、`Math`、`Timing`、`Platform`
- `Render`、`App`、`Ecs`

`GameFramework.h` はまとめて利用するための公開入口である。依存を限定したい実装は、
必要な責務の header を直接 include する。

## 責務ごとの構成

### ゲームとシーン

| 型 | 責務 | 所有する状態 |
|---|---|---|
| `CGame` | `CApplication` の hook をゲーム向け順序へ接続する | `CSceneManager`、固定更新時計、入力 source 結線、GameInstance サブシステム、共有描画資源、フェード、app state |
| `CSceneManager` | シーン stack と遅延遷移を管理する | active scene、保留遷移、退場 scene の遅延破棄 ring |
| `AScene` | 一つの world の多態的 owner | `CSceneNodeGraph`、World サブシステム、要求した `CSceneServices`、travel context |
| `CSceneNodeGraph` | scene 文脈に依存しないノード graph 操作 | root `ANode`、`CNodePool` |
| `ANode` | 階層、transform、描画順、局所挙動の合成 | 子 `ANode` と `AComponent` |
| `AComponent` | owner node に付く一つの局所機能 | owner への非所有参照と任意の局所状態 |

ノード graph と ECS `FWorld` は別の owner と状態 model を持つ。`ANode` / `AComponent` の変更を
ECS entity/component へ暗黙に複製しない。両者を連携する機能は、同期方向、owner、寿命を
明示した bridge として分ける。

シーン型は `AScene` 一つである。2D と 3D は別の scene class ではなく、projection、
component、`WantedServices()` の構成で表現する。詳細は
[SceneUnification.md](SceneUnification.md) と [NodeUnification.md](NodeUnification.md) に定める。

### シーン内サービス

`CSceneServices` は `AScene::WantedServices()` の `ESvc` bit に従って必要な機能だけを
個別に確保して束ねる。現在の要素は clock、tween、sequence、input map と固定更新入力、
2D camera、2D collision、trigger である。要求していない accessor の呼び出しは契約違反として検出する。

この束は scene 固有の決定的な機能を簡潔に配線するためのものであり、全機能を
サブシステム化するための入口ではない。

### サブシステム

サブシステムは次の条件をすべて満たす機能に使う。

1. 複数の利用側から同じ実体を共有する。
2. `Application`、`CGame`、`AScene` のいずれかが明確な owner になる。
3. owner と結び付いた初期化・終了、または毎 frame の更新が必要になる。

scope は `Engine`、`GameInstance`、`World` の順に親子関係を持つ。
`CSubsystemCollection` は factory 一覧を検証して一時領域へ生成し、全準備が成功した後だけ
公開する。初期化失敗時は以前の状態を維持し、終了は初期化と逆順に行う。

GameFramework が標準登録する World サブシステムは次の三つである。

- `AEventBus`: world 内 event 配送。
- `ASpawn2DSubsystem`: scene root を生成先とする prefab 生成。
- `AWorldClockSubsystem`: world 共有時計。

局所値、単純な計算、単独 node の状態、保存用 descriptor はサブシステムへ置かない。
それぞれ値型、責務 class、`ANode` / `AComponent`、または独立した serializer へ置く。

### 機能領域

公開型は責務単位の header と、実装を持つ場合は同名 cpp に分ける。複数の型と処理が一つの
機能を構成する場合は、その機能の folder を境界にする。現在の主な領域は次の通りである。

- 時間と進行: `Clock`、`SceneTimer`、`Tween`、`Sequence`、`StateMachine`。
- 入力: `InputMap`、`InputAxisOptions`、固定更新入力 snapshot/buffer/source、`InputRecorder`。
- camera と描画: `Camera2D`、`Camera3D`、`OrbitCameraController3D`、`CameraStack`、`Draw`、`RenderContext`、`SceneRenderResources`。
- 物理と衝突: `CollisionWorld2D`、`RigidWorld2D`、`TriggerWorld2D` と対応 component。
- asset と永続化: `AssetBundle`、`AssetPack`、`SaveArchive`、`SaveSlot`、scene serializer、prefab、reflection。
- UI、音声、演出: `UiLayer`、`AudioDirector`、`MusicDirector`、`EffectSystem`、`FadeTransition`。
- ゲームプレイ部品: dialogue、behavior、inventory、combat、progression などの独立した責務 class。
- 任意 backend の境界: platform service、network、script、XR、診断の interface と adapter。
- `tools/`: runtime の world 所有から分離した editor panel と authoring 補助。

外部 SDK や backend に依存する実装は interface と provider 境界の内側へ置く。機能を使わない
構成では、未接続 provider を明示的に報告するか、契約で定めた stub 動作だけを提供する。

公開型と member の索引は `GameFramework.h` と ACS の
[GameFramework API reference](reference/gameframework.html) に置く。本書は個別 signature を
重複掲載せず、型同士の責務、所有権、更新順序、失敗条件だけを定める。WPF desktop Editor は
別の owner と native ABI を持ち、[EditorArchitecture.md](EditorArchitecture.md) に定める。

## 主要な公開契約

### 時間、補間、sequence

- `CSceneClock` は scene 内の経過時間、delta、time scale、pause を値として保持する。
  負の time scale は 0 に制限する。
- `CTweenManager` と `CSequenceRunner` は generation 付き handle で要素を識別する。
  stale handle の cancel は別の要素を変更しない。
- scene service の更新順は clock、scene update、tween、sequence、camera、trigger である。
  scene update 中に追加した補間は同じ update の途中から進めない。
- easing catalog は `EEasingType` と checked API を持つ。無効な type、非有限入力、容量不足は
  失敗として分類し、checked API の出力を変更しない。
- `FSceneTimerHandle` と event module の `FTimerHandle` は owner、寿命、layout が異なるため
  統合しない。

easing の入力境界は [EasingSafety.md](EasingSafety.md)、delegate と timer の owner 境界は
[SimpleDelegateAndTimerSubsystem.md](SimpleDelegateAndTimerSubsystem.md) に定める。

### 入力

`FInputMap` は名前を hash 化した `FActionId` に key、mouse button、gamepad button、
axis key pair、gamepad axis を複数 bind する。query 時に platform input を読み、独自の
入力状態 owner を増やさない。

- digital query は `IsPressed()`、`IsHeld()`、`IsReleased()`。
- axis query は digital pair と analog axis を同じ action に束ねる。
- 一 action の複数 bind は OR または axis 合成として評価する。
- gamepad bind は player index を明示する。
- dead zone、反転、感度などの axis 補正値は `FInputAxisOptions` に分ける。

物理入力の lifetime は `Platform` module が所有し、GameFramework は action mapping だけを
担当する。

### camera と座標

`CCamera2D` は position、zoom、rotation、追従 target、dead zone、bounds、shake を一つの
camera 状態として持つ。zoom は正の最小値で制限する。`ScreenToWorld()` と
`WorldToScreen()` は同じ view center、rotation、zoom を逆向きに適用する。

`AScene` は `PixelsPerUnit()` を加えて scene 描画座標を変換する。camera service がない
scene では原点・等倍を使う。

現行の `AScene` world pass と `AScene::ScreenToWorld()` が適用するのは view center、zoom、
pixels-per-unit だけである。`CCamera2D` が保持する rotation はこの scene 描画と
scene picking 経路には反映されない。

### 3D SFX と音声 backend

app が `IAudioBackend` を所有し、backend は voice resources と内部コピーした PCM を所有する。
`CAudioDirector` は backend を非所有参照する。scene は `CSpatialAudio`、source ID、
`FAudioVoiceHandle` の対応を所有する。scene 終了時は `StopVoice(handle)` を先に呼び、その後に
`RemoveSource()` / `Clear()` / scene 破棄を行う。
backend は Director からの最終利用まで生存させる。backend の切断または破棄時は scene の voice を
`StopVoice()` で停止し、残る voice を `StopAllVoices()` で停止してから `SetBackend(nullptr)` を呼ぶ。
その後に backend を `Shutdown()` / 破棄し、Director に破棄済み backend の参照を残さない。

`CSpatialAudio` の登録・更新・削除と voice parameter の読出しは scene/update thread で
呼び出し側が直列化する。backend 呼び出し以降だけを backend 内部で同期し、この結線を
thread-safe とは扱わない。

- `PlaySfxClip()` で voice を開始した同じ frame に `UpdateSpatialSfxVoice()` を呼び、その後も
  listener または source の移動後に毎 frame 呼ぶ。`Pause()` 中も呼ぶことで音量 0 を反映し、
  `Resume()` 後の次回更新で距離減衰後の音量へ戻す。
- 更新音量は Master、Sfx、source の基準音量、距離減衰を合成する。削除済み source と無効 voice
  は Director から backend へ渡さず、解放済み voice は backend が安全な no-op として扱う。
  source の解除だけでは対応 voice を自動停止しない。
- XAudio2 backend の左右 pan は mono source と、左右 front speaker を識別できる出力だけに
  適用する。stereo または多 channel source、出力 channel mask を取得できない環境では既存の
  matrix を保ち、音量と pitch だけを更新する。
- この更新は Doppler pitch を計算しない。呼び出し側が渡した有限な pitch だけを反映する。

### 2D collision

`CCollisionWorld2D` は既存の math collision primitive と一様 grid を組み合わせる。
shape は generation 付き `FShapeId` で識別し、layer mask と除外 ID を query に渡せる。

- overlap: AABB、circle、convex polygon。
- raycast: broad phase の候補から最も近い有効 hit を返す。
- stale `FShapeId` は再利用 slot の別 shape を参照しない。
- query の narrow phase は `math/Collision2D.h` の判定を再利用し、別の shape 数学を
  GameFramework 内へ複製しない。

rigid body、trigger、tilemap physics はこの world または専用 world class に接続し、
component 自身へ world 全体の broad-phase state を持たせない。

### 乱数

`FRandom` は seed から再現可能な xoshiro128** の状態を持つ値型である。暗号用途には使わない。

- `RangeInt()` と `Shuffle()` は既存の値列と消費回数を維持する互換 API である。
- `TryDiscard()` は 1,048,576 draw を上限とし、超過時に状態を変更しない。
- `CaptureSnapshot()` / `TryRestoreSnapshot()` は版、予約値、状態、検査値を検証する。
- checked weighted choice、範囲配列生成、unbiased shuffle は一度に 4,096 要素まで扱い、
  失敗時に乱数状態と出力を変更しない。
- `Global()` は process 共有だが、同時利用の同期は暗黙に追加しない。呼び出し側が一つの
  thread に閉じる。

### asset と永続化

- `CApplication` が `CAssetRegistry` を所有する。GameFramework は既存 registry を受け取り、
  loader、cache、worker、shutdown を再実装しない。
- `CAssetBundle::BeginLoad()` は登録 path を `CAssetRegistry::Load()` で同期 load する。
  完了後は各 entry が `Loaded` または `Failed` になり、`Progress()` は成功と失敗の両方を
  完了数へ含める。開始後の `Add()` と二回目の `BeginLoad()` は無視する。
- `IAssetPackReader` / `IAssetPackWriter` は GameFramework の backend 境界である。
  provider 未登録時は stub が `NotImplemented` を返す。実 `.acpak` backend は
  `ACS::AssetPack` が提供する。
- `CSaveArchive` は version、payload size、checksum を検証する低水準 file contract を持つ。
- `CSettings::TryLoad()` は入力全体を一時状態へ読み、どの失敗でも現在の entry と所有 string、
  既存の `GetString()` pointer を変更しない。

詳細は [AssetPack.md](AssetPack.md)、[SaveArchive.md](SaveArchive.md)、
[SettingsPersistenceSafety.md](SettingsPersistenceSafety.md)、
[SaveGamePersistenceSafetyV2.md](SaveGamePersistenceSafetyV2.md) に定める。

### reflection、scene、authoring data

reflection と serializer は入力を検証してから対象へ適用する。field 数、name 長、payload 長などの
上限を越えた入力を部分適用しない。transient field と範囲外 metadata を実体 memory へ
書き込まない。node tree の live 構造変更と保存は深度上限超過を拒否する。
`SceneSerialize` の読み込みだけは、過深な親 chain を root 直下へ正規化し、
`DepthCappedNodeCount` で件数を返す。

対応する正規文書は次の通りである。

| 責務 | ACS 文書 |
|---|---|
| reflection と object serialization | [SerializationSafety.md](SerializationSafety.md) |
| binary scene graph | [Scene3DSerialization.md](Scene3DSerialization.md) |
| text scene load | [SceneTextLoadingSafety.md](SceneTextLoadingSafety.md) |
| text asset | [TextAssetLoadingSafety.md](TextAssetLoadingSafety.md) |
| prefab と save-time graph | [SaveGamePersistenceSafetyV2.md](SaveGamePersistenceSafetyV2.md) |
| animation curve | [AnimationCurveSafety.md](AnimationCurveSafety.md)、[AnimationCurvePersistenceSafety.md](AnimationCurvePersistenceSafety.md) |
| behavior tree | [BehaviorTreePersistenceSafety.md](BehaviorTreePersistenceSafety.md) |

### runtime service と bridge

network snapshot、replay、script、hot reload、studio lock などは一つの subsystem へ統合せず、
独立した owner class と wire-format class に分ける。外部入力、非同期完了、保存、lock を持つ
境界は次の ACS 文書を正規契約とする。

- [NetSnapshotSafety.md](NetSnapshotSafety.md)
- [FixedStepRuntimeInput.md](FixedStepRuntimeInput.md)
- [OrbitCameraController3D.md](OrbitCameraController3D.md)
- [ReplayDirectorSafety.md](ReplayDirectorSafety.md)
- [ScriptHostSafety.md](ScriptHostSafety.md)
- [HotReloadSafety.md](HotReloadSafety.md)
- [StudioWorkflowLockSafety.md](StudioWorkflowLockSafety.md)

## 所有権とライフサイクル

### 起動

`CGame::OnStart()` は次の順に処理する。

1. GameFramework 標準サブシステムの登録結果を確認する。
2. Engine collection を親として GameInstance collection を初期化する。
3. `InitialScene()` を取得する。
4. scene の service と World collection を準備し、成功した scene だけを stack へ公開する。

登録、確保、scene 準備のいずれかが失敗した場合は、部分初期化した scene を公開せず終了を
要求する。

### frame 更新

可変更新は次の順序で進む。

1. fade と保留中の scene 遷移。
2. 描画 frame 入力を一度取得するか、各固定 tick の決定論入力を取得する。
3. 固定刻みの `AScene::OnFixedUpdate()`。一 frame の最大 step 数を超えた遅延は捨てる。
4. GameInstance `PreUpdate`。
5. scene service `PreUpdate`。
6. World サブシステム `PreUpdate`。
7. `AScene::OnUpdate()` と node graph 更新。
8. scene service `PostUpdate`。
9. World サブシステム `PostUpdate`。
10. GameInstance `PostUpdate`。

scene service の clock を要求した場合は、その clock が求めた world delta を scene、node、
World サブシステムへ渡す。GameInstance には game の time scale を反映した値と実時間を
別々に渡す。

`ESvc::Input` を要求した scene は `Services().FixedInput()` から現在の固定 tick 入力を読み、
`Services().Input().Evaluate()` で名前付き action へ変換する。固定更新が来ない短い frame の
押下・解放は次の固定 tick まで保持し、catch-up 中の同じ変化は一度だけ通知する。入力 source、
rollback 用 snapshot、失敗時の無入力化は [FixedStepRuntimeInput.md](FixedStepRuntimeInput.md) に定める。

3D orbit camera の移動と視点計算は `COrbitCameraController3D` へ入力値、外部所有 state、固定 tick 秒を
渡す。controller は renderer や device を参照せず、出力した eye と look-at だけを描画 camera へ
接続する。`ALegacyScene3DAdapter` の自由カメラも scene-local action と固定 tick 入力をこの境界へ
接続し、描画時だけ previous/current 状態を固定時計の補間率で混ぜる。契約と固定入力からの変換例は
[OrbitCameraController3D.md](OrbitCameraController3D.md) に定める。rollbackでは固定runtime snapshotを
先に戻し、orbit cameraのprevious/current snapshotを明示的なLegacyアダプターから復元する。
任意の障害物回避は `CSceneNodeGraph::RaycastActiveRange` でtarget近傍と無効meshを除外し、fixed tickの
desired距離を変えずpresentation距離だけを短縮する。

### scene 遷移

`ChangeScene`、`PushScene`、`PopScene` は要求を保留し、frame 境界で適用する。同じ境界までに
複数要求された場合は最後の要求が有効になる。

- `ChangeScene` と `PushScene` は stack 容量、service、World サブシステムを先に準備する。
- 準備失敗時は現在の scene と pause 状態を変更しない。
- `PushScene` は新 scene の準備成功後にだけ以前の top を pause する。
- `PopScene` は二つ以上の scene がある場合だけ行い、戻り値用 travel context を
  `OnResume()` より前に設定する。
- 退場 scene は GPU の参照期間を考慮した固定長 ring へ移し、直ちには破棄しない。

### 終了

scene は `OnExit()` の後に World サブシステムを終了する。`CGame::OnShutdown()` は scene、
GameInstance、UI font、fade overlay をこの順に後始末する。hook の復帰後に
`CApplication` が Engine scope を終了する。共有描画資源は `OnShutdown()` で明示的に
reset されず、`CGame` の member として破棄時まで所有される。

## ノードとコンポーネント

`ANode` は `FTransform3D` を一つだけ持つ。2D code は `Position2D()`、`SetPosition2D()`、
`Rotation2D()`、`SetRotation2D()`、`Scale2D()`、`World2D()` を使い、未使用の 3D 成分を
壊さない。

子 node は `TObjectPtr<ANode>`、component は `TUniquePtr<AComponent>` で所有する。
`Destroy()` と `Reparent()` は走査中に構造を変更せず、graph の構造変更解決時に適用する。
循環、深度上限超過、破棄済みの移動先は拒否する。

component は node の局所能力である。`OnAttach`、更新、描画、query、`OnDetach` を持ち、
共有サービスが必要な場合だけ owner node から scene service または subsystem を参照する。

## 描画

`CGame` が world/HUD 用 sprite batch と scene render target を game 寿命で所有する。
`AScene` は frame 中だけ借り、使わない scene はそれらを個別所有しない。

`AScene::OnRender()` は world pass と HUD pass を実行する。`Draw.h` の即時描画関数は pass 中の
`FRenderContext` だけを参照し、pass 外または sprite batch 未接続時は何も行わない。

エンジン、エディタ、外部の描画側が `FRenderContext` を構成する場合は、`WiringAccess()` が
返す短寿命の接続窓口から `BeginFrame`、各 `Set*`、`EndFrame` を呼ぶ。実処理の
`Foo_Internal` は `private` であり、通常の描画処理から直接変更しない。旧 `_BeginFrame`、
`_SetFont`、`_EndFrame` は C++ 処理系の予約名であるため公開しない。

2D node は `(DrawLayer, DrawPriority, Y sort, tree order)` の安定順で描画する。subtree 単位の
描画状態を要求する component は、その subtree を一つの原子 group として扱う。

## 永続化と互換性

- `SceneSerialize` は `TrySaveNodeTree` / `TryLoadNodeTree` を詳細結果付きの標準 API とする。
- 保存は検証と容量計測を完了してから出力へ書き、容量不足時に出力を部分変更しない。
- 読み込みは入力全体と component payload を検証し、破損した部分 scene を返さない。
- editor と runtime の C ABI export 名は C++ 型名とは別の互換境界であり、公開済み名を維持する。
- 公開 virtual は既存 slot を動かさず、必要な追加は末尾へ置く。
- `dist/acs.h` は source header から生成し、直接編集しない。

## 失敗条件

次の状態は成功として公開しない。

- 必須 subsystem の登録、生成、owner 検証、親 scope 検証に失敗した。
- scene root、service、World collection の準備に失敗した。
- live node 構造変更または保存で循環、深度上限超過、無効 owner、無効 component payload を検出した。
- serializer の版、長さ、個数、参照関係、出力容量が契約を満たさない。
- 任意 backend が未接続なのに、その機能が利用可能であると報告しようとした。

失敗時は現在公開中の scene、保存先、呼び出し側 buffer を可能な限り維持し、具体的な結果値
または診断を返す。

## 検証

次は `acs/` を作業 directory として実行する。

```powershell
python -B scripts\audit_cpp_type_roles.py --root src
python -B scripts\audit_cpp_conventions.py --root .
python -B scripts\audit_module_sources.py --root .
python -B scripts\amalgamate.py --check
cmake --build Intermediate\vs --config Debug --target acs_gameframework acs_unit_tests
cmake --build Intermediate\vs --config Release --target acs_gameframework acs_unit_tests
ctest --test-dir Intermediate\vs -C Debug --output-on-failure
ctest --test-dir Intermediate\vs -C Release --output-on-failure
```

公開 header、ABI、scene 遷移、subsystem scope、node ownership、serializer、即時描画の変更は、
それぞれの focused test と全 CTest の両方で確認する。生成配布を変更する場合は
`dist/acs.h` の drift、構文、consumer build も確認する。
