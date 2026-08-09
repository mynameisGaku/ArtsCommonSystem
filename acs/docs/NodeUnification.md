<!-- SPDX-License-Identifier: Apache-2.0 -->
# ノード統一 (`ANode`)

ACS の scene graph は 2D/3D 共通の `ANode` と `AComponent` を使う。2D と 3D の違いは
transform の利用範囲、component、描画経路で表現し、別の node 基底を作らない。

所有権移動と深度上限の詳細は [NodeOwnershipSafety.md](NodeOwnershipSafety.md)、scene owner は
[SceneUnification.md](SceneUnification.md) を参照する。

## 責務と所有権

- `AScene` は `CSceneNodeGraph` を一つ所有する。
- `CSceneNodeGraph` は root `ANode` と `CNodePool` を所有する。
- 親 `ANode` は子を `TObjectPtr<ANode>` で所有する。
- `ANode` は component を `TUniquePtr<AComponent>` で所有する。
- gameplay の監視参照は `TWeakObjectPtr<ANode>` を使い、破棄済み object を検出する。

`ANode` と `AComponent` は `AObject` 派生の多態的 object である。処理を統括する独立 class は
`C`、値・descriptor・handle・owner 寿命内の service は `F`、template は `T`、interface は `I`、
enum は `E` とする。`C` と `F` は class / struct 構文ではなく、identity、lifecycle、所有責務で分ける。

## `ANode`

### lifecycle

node の hook は次の役割を持つ。

- `OnSpawn()`: tree へ追加された直後の初期化。
- `OnUpdate()`: 可変刻みの局所更新。
- `OnFixedUpdate()`: 固定刻みの局所更新。
- `OnDraw()`: node 自身の描画。
- `OnDespawn()`: tree から取り除かれる直前の後始末。

`AddChild()` は所有権を親へ移し、追加できた node に `OnSpawn()` を通知する。失敗理由が必要な
呼び出し側は `TryAddChild()` の `EAddChildResult` を使う。

`Destroy()` と `Reparent()` は走査中に tree を直接書き換えない。要求を保持し、
`ResolveStructuralChanges()` が frame 境界で処理する。移動先は lifetime observer で監視し、
適用前に破棄された場合は要求を取り消す。

構造変更時は次を拒否する。

- 自分自身または子孫への reparent。
- 破棄予定の親への reparent。
- `kNodeMaxTreeDepth` を超える tree。
- 最新の tree で循環を作る変更。

失敗した遅延変更で node を所有権のない状態へ残さない。

### transform

`ANode` が保持する局所 transform は `FTransform3D` 一つだけである。

- `Local()` / `World()` は `FVec3`、`FQuat`、`FVec3` の 3D contract を使う。
- `Position2D()` / `SetPosition2D()` は x と y だけを扱い、z を維持する。
- `Rotation2D()` / `SetRotation2D()` は Z 軸回転を quaternion と相互変換する。
- `Scale2D()` / `SetScale2D()` は x と y だけを扱う。
- `Local2D()` / `World2D()` は 2D 描画用の射影を返す。

2D code は `FVec2` を `Local().position` へ代入せず、これらの helper を使う。

### 描画順

各 node は次の描画 key を持つ。

- `DrawLayer`: 大域的な層。小さい値を先に描画する。
- `DrawPriority`: 同じ層内の優先度。小さい値を先に描画する。
- `YSortEnabled` と `YSortBias`: 同じ layer/priority で比較する両 node が有効な場合だけ、
  world y と bias を key に使う。
- tree 出現順: 同じ key の安定順。

scene の 2D 描画は可視 node を収集し、これらの key で安定 sort する。すべての key が既定値の
場合は tree 順を維持する。subtree 全体で stencil などの状態を共有する component は
`WantsAtomicSubtree()` を返し、その subtree を一つの描画 group として扱う。

## `AComponent`

`AComponent` は一つの node に付く局所機能である。component ごとに責務を分け、scene 全体の
所有や更新を必要としない機能を subsystem へ移さない。

主な hook は次の通りである。

- `OnRequire()`: 必要な companion component を owner へ要求する。
- `OnAttach()` / `OnDetach()`: owner 接続と解除。
- `OnUpdate()` / `OnFixedUpdate()`: 局所更新。
- `OnDraw()` / `OnDrawPostChildren()`: node 描画の前後処理。
- `OnAttachServices()`: scene service が利用可能になった時の接続。
- `QueryLight()` / `QueryShadowCaster()` / `QueryPrimitive()`: 描画側への型付き情報提供。

型識別は RTTI ではなく `ComponentKindOf<T>()` と `ACS_GAME_COMPONENT_KIND` を使う。
公開 virtual を追加する場合は既存 vtable slot を動かさず、末尾へ追加する。

component は owner node から `CSceneServices` と World `CSubsystemCollection` を参照できる。
取得できない機能を必須として扱う場合は、null または service bit を確認して失敗を明示する。

## pool と識別子

`CNodePool` は node の generational `FNodeId` を管理する。破棄済み slot の generation が
一致しない ID は無効であり、新しい node を同じ index へ登録しても古い ID は復活しない。

`SerialId` は scene serialization 内の決定的な参照に使う。runtime lifetime の検証には
`FNodeId` を使い、二つの役割を混ぜない。

## serialization

`SceneSerialize` の現行保存形式は 3D transform を保持する。旧形式を読み込む場合は不足する
成分を明示的に補完する。

標準 API は次の通りである。

- `TrySaveNodeTree()`: 検証、必要容量の計測、書き込みを分離した詳細結果を返す。
- `TryLoadNodeTree()`: 入力を全体検証し、詳細な失敗理由を返す。
- `SaveNodeTree()` / `LoadNodeTree()`: 互換用の簡易結果だけを返す。

上限は source の公開定数で固定する。

| 対象 | 上限 |
|---|---:|
| node 数 | `kSceneSerializeMaxNodeCount` = 65,536 |
| tree 深度 | `kSceneSerializeMaxTreeDepth` = 512 |
| 一 node の component 数 | `kSceneSerializeMaxComponentCountPerNode` = 1,024 |
| 一 component の payload | `kSceneSerializeMaxComponentPayloadBytes` = 4,096 bytes |

保存は循環、共有子、重複参照、深度上限超過、無効な反射名、容量不足を出力変更前に拒否する。
`buf == nullptr` かつ `cap == 0` は必要容量の照会として扱う。読み込みは親 index、長さ、個数、
component payload の消費量を検証し、破損した部分 tree を返さない。読み込みは
v2、v3、v4 を受理する。親 chain が深度上限を超える場合は node を失わず root 直下へ
付け替え、`DepthCappedNodeCount` で件数を返す。

## 互換境界

- 旧 2D/3D node と component の実型は再導入しない。
- `FNodePool` は source 再コンパイル向けに `CNodePool` を指す互換 alias としてだけ残す。
- editor の `acs_editor_node_*` / `acs_editor_node3d_*` は公開 C ABI 名なので変更しない。
- 旧 scene serialization を受理する場合も、公開後の in-memory graph は `ANode` へ統一する。

## 検証

次は `acs/` を作業 directory として実行する。

```powershell
python -B scripts\audit_cpp_type_roles.py --root src
python -B scripts\audit_cpp_conventions.py --root .
python -B scripts\audit_cpp_prefix_consumers.py --root .
python -B scripts\audit_reference_type_names.py --root .
python -B scripts\amalgamate.py --check
cmake --build Intermediate\vs --config Debug --target acs_gameframework acs_unit_tests
cmake --build Intermediate\vs --config Release --target acs_gameframework acs_unit_tests
ctest --test-dir Intermediate\vs -C Debug --output-on-failure
ctest --test-dir Intermediate\vs -C Release --output-on-failure
```

focused coverage では add/destroy/reparent、深度上限、stale ID、2D helper、stable draw order、
component lifecycle、serialization の破損入力と caller buffer 不変を確認する。
