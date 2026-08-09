<!-- SPDX-License-Identifier: Apache-2.0 -->
# シーン統一 (`AScene`)

ACS の scene owner は `AScene` 一つである。2D と 3D は scene class で分けず、projection、
component、scene service の構成で表現する。旧 2D/3D scene 型とその互換 alias は公開 API に
残さない。

ノードとコンポーネントの契約は [NodeUnification.md](NodeUnification.md) を参照する。

## 責務境界

| 型 | 責務 | 所有権 |
|---|---|---|
| `CGame` | game loop と scene manager の駆動、scene 共通描画資源 | game 寿命 |
| `CSceneManager` | scene stack、遅延遷移、退場 scene の遅延破棄 | `CGame` が所有 |
| `AScene` | world lifecycle と構成 | graph、World subsystem、任意 service、travel context を所有 |
| `CSceneNodeGraph` | scene 文脈に依存しない node graph 操作 | root `ANode` と `CNodePool` を所有 |
| `CSceneRenderResources` | sprite batch と scene render target | `CGame` が共有所有 |

`AScene` は render resource を所有しない。world/HUD pass の実行中だけ `CGame` の
`CSceneRenderResources` を借りる。menu や headless scene が不要な GPU resource を個別に
確保せず、scene stack の深さによって共有 batch の個数が増えない。

`CSceneNodeGraph` は owner scene のない一時 graph としても使える。serializer と editor ABI は
staging graph を stack に置けるため、scene lifecycle と graph 構築を混ぜない。

## scene の構成

### node graph

すべての `AScene` は root `ANode` を持つ `CSceneNodeGraph` を一つ所有する。graph は node の
登録、検索、破棄、raycast、更新、固定更新、`SwapContents()` を担当する。

`SwapContents()` は root 自体を置き換える。置換後は `AScene` の root-swap hook が次を
再接続する。

- `CSceneServices`
- World `CSubsystemCollection`
- `ASpawn2DSubsystem` の生成先 root

graph は接続先の意味を知らず、owner `AScene` が再配線する。

### service

`AScene::WantedServices()` は scene が必要な `ESvc` bit を返す。既定は `ESvc::None` であり、
要求していない機能へ費用を払わない。

2D scene の標準構成は `kScene2DServices` である。これは 2D 用 clock/tween/sequence/input と
camera/collision を組み合わせるが、`AScene` の派生型を増やさない。必要なら派生 scene が
`WantedServices()` を override する。

camera service がない場合、`ViewCenter()` は原点、`ViewZoom()` は等倍を返す。
`ScreenToWorld()` もこの fallback と `PixelsPerUnit()` を使う。

### subsystem

各 `AScene` は World scope の `CSubsystemCollection` を所有し、GameInstance collection を
親として初期化する。標準の World subsystem は event bus、2D prefab 生成、world clock である。

subsystem は共有 owner、lifecycle、更新順序が必要な機能に限る。node 固有状態や単純計算は
`ANode`、`AComponent`、値型、または責務 class に置く。

## lifecycle

### 準備と公開

`CSceneManager` は scene を stack へ追加する前に次を完了する。

1. root graph が準備可能であることを確認する。
2. `CGame` と `CSceneManager` の非所有参照を設定する。
3. 要求された `CSceneServices` を構築する。
4. World subsystem を初期化する。
5. travel context を設定する。

全準備に成功した scene だけを stack へ追加して `_Enter()` を呼ぶ。準備に失敗した場合は
現在の top、pause 状態、stack を変更しない。

`_Enter()` は service を root へ公開してから `OnEnter()` を呼び、hook 後に既存 component へ
service 接続を通知する。既定の `OnEnter()` は `OnReady()` を呼ぶ。

### 更新

top scene の可変更新順は次の通りである。

1. scene service `PreUpdate`
2. World subsystem `PreUpdate`
3. `AScene::OnUpdate()`
4. node graph `Update()` と構造変更解決
5. scene service `PostUpdate`
6. World subsystem `PostUpdate`

固定更新は `AScene::OnFixedUpdate()` の後に node graph `FixedUpdate()` を行う。
下に積まれた scene は `OnPause()` 後に通常更新・描画されず、top へ戻ると `OnResume()` を受ける。

### 退場

`_Exit()` は `OnExit()` の後に未解決の node 構造変更を処理する。要求済みの
Physics2D はすべての shape を除き、tween はすべて cancel する。その後で manager が
World subsystem を終了する。`CSceneServices` とその他の service 実体は scene が所有した
まま retire ring へ移り、scene の破棄時に解放される。

退場 scene は固定長 ring へ移し、GPU が直前の frame を参照しうる期間だけ破棄を遅らせる。
共有描画資源は game 寿命なので scene 退場では破棄しない。

## scene 遷移

`ChangeScene()`、`PushScene()`、`PopScene()` は即時に stack を変更せず、frame 境界で適用する。

- `ChangeScene`: 新 scene の準備成功後に旧 top を退場させて置換する。
- `PushScene`: 容量と新 scene の準備成功後にだけ旧 top を pause する。
- `PopScene`: stack depth が二以上の場合だけ top を退場させ、戻り先を resume する。
- 同じ適用境界までに複数要求された場合は最後の要求を保持する。
- `PopScene` の travel context は戻り先の `OnResume()` より前に設定する。

null scene、容量確保失敗、service 構築失敗、World subsystem 初期化失敗は遷移失敗である。
これらは現在の scene を部分的に pause、exit、置換しない。

## 描画

`AScene::OnRender()` は world pass と HUD pass を順に実行する。`FRenderContext` へ sprite batch を
設定し、pass の間だけ `Draw.h` の即時描画 context を公開する。

利用側は次のどちらかを override できる。

- `OnDrawWorld(FRenderContext&, CSpriteBatch&)` / `OnDrawHud(...)`
- 引数なしの `OnDrawWorld()` / `OnDrawHud()`

即時描画 context がない場合、または sprite batch が接続されていない場合、`DrawRect()`、
`DrawTexture()`、`DrawString()` などは何も行わない。pass 終了時は必ず context を解除し、次の
scene や frame へ漏らさない。

2D node 描画は scene graph が可視 node を収集し、安定した描画 key で並べる。3D component は
同じ root graph に存在でき、専用 component と render 経路が projection と depth を扱う。

## 互換境界

- `FScene` は `AScene` を指す source 互換 alias である。
- 削除済みの 2D/3D scene class 名とその alias は再導入しない。
- `acs_editor_scene_*` と `acs_editor_node3d_*` は editor と native DLL 間の C ABI 名である。
  C++ 型名統一を理由に変更しない。
- `ALegacyScene3DAdapter` は既存形式を統一 `AScene` graph へ接続する adapter であり、別の
  scene owner ではない。
- `dist/acs.h` は source header から再生成し、直接編集しない。

## 不変条件

1. `AScene::WantedServices()` の既定を `ESvc::None` から変更しない。
2. `Scene.h` に重い render 実装 header を持ち込まず、描画資源は `CGame` に所有させる。
3. root 置換後に service、subsystem、spawn target を再接続する。
4. scene 準備失敗時に現在の stack と pause 状態を変更しない。
5. World subsystem は `OnExit()` の後に終了する。
6. editor C ABI の公開済み export 名を維持する。
7. 使わない scene が 2D service や scene 固有 GPU resource を確保しない。

## 検証

次は `acs/` を作業 directory として実行する。

```powershell
python -B scripts\audit_cpp_type_roles.py --root src
python -B scripts\audit_cpp_conventions.py --root .
python -B scripts\audit_cpp_prefix_consumers.py --root .
python -B scripts\audit_module_sources.py --root .
python -B scripts\amalgamate.py --check
cmake --build Intermediate\vs --config Debug --target acs_gameframework acs_unit_tests
cmake --build Intermediate\vs --config Release --target acs_gameframework acs_unit_tests
ctest --test-dir Intermediate\vs -C Debug --output-on-failure
ctest --test-dir Intermediate\vs -C Release --output-on-failure
```

focused coverage では scene change/push/pop の確保失敗、service がない scene、root swap の再配線、
World subsystem 順序、spawn target、即時描画の pass 外 no-op を確認する。
