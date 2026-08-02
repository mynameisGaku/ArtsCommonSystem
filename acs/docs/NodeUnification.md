# ノード統一 (ANode) 設計書

2026-07-18 確定・実装済み。FNode2D / FNode3D を単一の `ANode` に統一した大規模移行の
設計、現行仕様、安全境界を記録する。
所有権移動 API と深度上限の実装契約は
[`NodeOwnershipSafety.md`](NodeOwnershipSafety.md) も参照する。

## 決定事項 (ユーザー確定)

1. **2D/3D ノードを分けない**。単一クラス `ANode` に統一する。
2. **Transform は 3D 一本化** (Unity 方式)。2D は「x,y を使い z を depth に流用する特殊ケース」。
3. **移行は一括置換**。旧 `FNode2D` / `FNode3D` は互換層を作らず削除し、全利用箇所 (~825 箇所 / 107+ ファイル) を書き換える。
4. **命名規約**: `F` = データ・値・handle、`C` = 機能を持つ具象class。**`A` = owner /
   registryに所有され、多態的に扱われるobject**。現waveでは`AObject`推移派生または
   実登録macroを機械確定し、その他の候補はmanual debtでreviewする。
   よって統一ノードは `ANode`、コンポーネント基底は `AComponent`。
5. **描画順はオブジェクト持ち**: `DrawLayer` (i32) + `DrawPriority` (i32) + ノード別 Y-sort
   フラグをノード標準装備とし、シーンが自動で並べる。draw コールにレイヤーを渡す方式は廃止方向
   (FSpriteSortList は手書き即時描画向けに存置)。

## ANode 設計

### 基底とライフサイクル

- `class ANode : public AObject` — `NewObject<T>()` で生成し、親が `TObjectPtr<ANode>` で所有、
  ゲームプレイ側参照は `TWeakObjectPtr<ANode>` (stale 安全)。
  旧 `AddChild(MakeUnique<FNode2D>())` は `AddChild(NewObject<MyNode>(...))` になる。
- ライフサイクル / 階層 API は **FNode2D の成熟実装をそのまま継承**:
  OnSpawn / OnUpdate / OnFixedUpdate / OnDespawn、AddChild 即時 OnSpawn、
  `Destroy()` → フレーム境界 `ResolveStructuralChanges()`、`Reparent()`、
  `FNodeId` / `SerialId`、iteration safety (index 走査)、`MoveChild`、
  `CSceneServices` / `CSubsystemCollection`配線、名前 / 検索。
- 遅延 `Reparent()` の移動先は侵入型 lifetime observer で監視する。`NewObject` 所有ノードと
  Scene が値所有する root の双方を扱え、適用前に移動先が破棄された場合は要求を自動取消する。
  フレーム境界では破棄予定の祖先、最新ツリーでの循環、`kNodeMaxTreeDepth` を再検証し、
  不成立ならノードを元の親へ安全に戻す。

### Transform

- `FTransform3D m_Local` のみ保持 (`FVec3 position / FQuat rotation / FVec3 scale`)。
  `World()` は親をたどって `Compose`。
- **2D ヘルパを標準装備** (2D ゲームの書き味を維持):
  - `Position2D() / SetPosition2D(FVec2)` — x,y のみ触り z 温存
  - `Rotation2D() / SetRotation2D(f32)` — Z 軸回転角 (quat との相互変換)
  - `Scale2D() / SetScale2D(FVec2)`
  - `World2D()` — world の FTransform2D 射影 (x,y / Z 回転角 / x,y スケール)。
    2D 描画パス (スプライト) はこれを使う。
- 2D 座標規約 (Y-down / 左上原点) は変更しない。

### 描画順 (グローバル安定ソート標準化)

- ノードのフィールド:
  - `SetDrawLayer(i32) / DrawLayer()` — 第 1 キー (旧 SortLayer を改名・意味同一: 小 = 奥)
  - `SetDrawPriority(i32) / DrawPriority()` — 第 2 キー (層内順序、小 = 奥)
  - `SetYSortEnabled(bool)` + `SetYSortBias(f32)` — ノード別フラグ。有効ノードは同 layer 内で
    (world.y + bias) が priority より優先される (見下ろし遮蔽)。
- **シーンの 2D 描画パスは常時グローバルソート**:
  可視ノードをフラット収集 → `(DrawLayer, [YSort: y+bias], DrawPriority, ツリー出現順)` の
  安定ソート → 各ノードの自前描画 (OnDraw + components) を実行。
  全ノードがキー 0 なら出現順 = 従来のツリー順と完全一致するため **既定動作は後方互換**。
  全キー 0 と検出したフレームはソートをスキップする (ゼロオーバーヘッド)。
- **原子グループ**: subtree スコープの状態を持つノード (AStencilClip2DComponent 等) は
  subtree を従来どおり再帰で一塊描画し、グループ自体をルートのキーで整列する。
  マテリアル効果 (FMaterialState) はノード単位なのでフラット化と両立する。
- `EChildDrawOrder` (Tree/Layer/LayerThenY の兄弟ソート) は廃止 — グローバルソートが上位互換。
- 3D メッシュは深度バッファがあるため不透明は従来通り。半透明・エフェクト・オーバーレイの
  提出順に (DrawLayer, DrawPriority) を適用する（`CScene3D`側の収集順）。

### AComponent

- `class AComponent : public AObject` — フックは FComponent2D の全量を継承:
  OnRequire / OnAttach(ANode&) / OnUpdate / OnFixedUpdate / OnDraw(FRenderContext&) /
  OnDrawPostChildren / OnAttachServices / OnDetach / QueryLight / QueryShadowCaster /
  QueryPrimitive。3D 向け追加フックは **vtable 末尾追加** の従来方針。
- `ComponentKindOf<T>()` (RTTI 不使用型タグ) / `ACS_GAME_COMPONENT_KIND` は維持。
- 既存の 2D 系コンポーネント (`ASprite2DComponent` / `ASpriteAnimComponent` /
  `ATriggerComponent` / `ARigidBody2D` / ...) と 3D 系 (`AMeshComponent3D` 等) は
  `AComponent` 派生に書き換え、管理オブジェクトである
  個別コンポーネントも `ASprite2DComponent` / `AMeshComponent3D` のように `A` 接頭辞へ
  統一する。機能を持つ具象classは`C`、データ中心のstruct・値・handleは`F`、templateは`T`、純粋interfaceは
  `I`、enum は `E` とする (`docs/StyleGuide.md` 参照)。

### プール / シリアライズ / シーン

- generational IDレジストリは正規型`CNodePool`に統一し、旧`FNode3DPool`を削除する。
  `FNodePool`は再コンパイルするsourceだけの互換`using`として残し、consumerはclean rebuildする。
- SceneSerialize v4 は transform を 3D フィールド (pos3/quat/scale3) に拡張する。
  保存は v4、読み込みは旧 v2/v3 も受理して 3D transform へ補完する。
- 読み込みは `TryLoadNodeTree` を標準とし、破損データを部分成功させない。結果の
  `FSceneLoadResult` からエラー種別、消費バイト数、入力フォーマット版、深度制限による
  付け替え件数を取得できる。互換用 `LoadNodeTree` は失敗時 null の簡易 API として残す。
- 保存は `TrySaveNodeTree` を標準とし、検証・正確なサイズ計測・書き込みの二段階で行う。
  `FSceneSaveResult` から失敗理由、必要/書込 bytes、ノード/コンポーネント数を取得できる。
  `buf=nullptr, cap=0` のサイズ照会と、容量不足時に出力を一切変更しない再試行が可能。
  互換用 `SaveNodeTree` は成功時 bytes、失敗時 0 の簡易 API として残す。
- 敵対的入力への上限はノード 65,536、1 ノード当たりコンポーネント 1,024、コンポーネント
  payload 4,096 bytes、構築深度 512。親 index は DFS pre-order 上の既出ノードだけを許す。
  保存時の平坦化は明示スタックと visiting/complete 訪問表で行い、深いツリーで呼び出し
  スタックを枯渇させず、循環と共有子/重複参照を区別して拒否する。ReflectName は最大
  255 bytes の NUL 終端を境界内で検証し、異常名や payload 超過も書き込み前に拒否する。
  読み込み時は既知コンポーネントの反射 payload 全体を検証してから attach し、破損や
  未消費の余剰 bytes があれば部分シーンを返さない。
- `AScene2D` / `CScene3D`は`TObjectPtr<ANode>`でrootを所有する（シーン自体の統合は次期フェーズ）。
  この次期フェーズの設計、責務境界の実測、影響範囲、不変条件、移行手順は
  [`SceneUnification.md`](SceneUnification.md) に切り出した（未実装）。
- `acs_editor_node_*` / `acs_editor_node3d_*` は C# editor と native DLL 間で互換維持する
  C ABI の export 名であり、C++ 型名ではない。内部実装は `ANode` に統一済みだが、
  P/Invoke の entry point は既存 editor/project との ABI を壊さないため改名しない。

## 実施済み移行手順 (タスク #2〜#5)

1. ANode / AComponent / CNodePool 実装 + 旧 4 クラス削除 (task #2)
2. `AScene2D` / `CScene3D` 描画パス置換 + グローバルソート実装 (task #3)
3. gameframework 内部 (~40 ファイル) + tests (~10) 一括置換、全テスト緑 (task #4)
4. samples ~15 本 + docs/tutorials 10 章 + reference data + editor C# (task #5)

削除対象: Node2D.{h,cpp} / Node3D.{h,cpp} / Component2D.h / Component3D.h /
Node3DPool.{h,cpp}。`NodePool.{h,cpp}`は統一後の`CNodePool`実装として残し、`FNodePool`は
再コンパイルするsourceだけの互換`using`とする。
