# ノード統一 (ANode) 設計書

2026-07-18 確定。FNode2D / FNode3D を単一の `ANode` に統一する大規模移行の設計と手順。

## 決定事項 (ユーザー確定)

1. **2D/3D ノードを分けない**。単一クラス `ANode` に統一する。
2. **Transform は 3D 一本化** (Unity 方式)。2D は「x,y を使い z を depth に流用する特殊ケース」。
3. **移行は一括置換**。旧 `FNode2D` / `FNode3D` は互換層を作らず削除し、全利用箇所 (~825 箇所 / 107+ ファイル) を書き換える。
4. **命名規約の新設**: `F` = 構造体・素のクラス (従来通り)。**`A` = ACS のオブジェクト基底
   (`FObject`, memory/ObjectPtr.h) を継承するオブジェクト** (UE の U/A プレフィックスと同じ発想)。
   よって統一ノードは `ANode`、コンポーネント基底は `AComponent`。
5. **描画順はオブジェクト持ち**: `DrawLayer` (i32) + `DrawPriority` (i32) + ノード別 Y-sort
   フラグをノード標準装備とし、シーンが自動で並べる。draw コールにレイヤーを渡す方式は廃止方向
   (FSpriteSortList は手書き即時描画向けに存置)。

## ANode 設計

### 基底とライフサイクル

- `class ANode : public FObject` — `NewObject<T>()` で生成し、親が `TObjectPtr<ANode>` で所有、
  ゲームプレイ側参照は `TWeakObjectPtr<ANode>` (stale 安全)。
  旧 `AddChild(MakeUnique<FNode2D>())` は `AddChild(NewObject<MyNode>(...))` になる。
- ライフサイクル / 階層 API は **FNode2D の成熟実装をそのまま継承**:
  OnSpawn / OnUpdate / OnFixedUpdate / OnDespawn、AddChild 即時 OnSpawn、
  `Destroy()` → フレーム境界 `ResolveStructuralChanges()`、`Reparent()`、
  `FNodeId` / `SerialId`、iteration safety (index 走査)、`MoveChild`、
  `FSceneServices` / `FSubsystemCollection` 配線、名前 / 検索。

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
- **原子グループ**: subtree スコープの状態を持つノード (FStencilClip2DComponent 等) は
  subtree を従来どおり再帰で一塊描画し、グループ自体をルートのキーで整列する。
  マテリアル効果 (FMaterialState) はノード単位なのでフラット化と両立する。
- `EChildDrawOrder` (Tree/Layer/LayerThenY の兄弟ソート) は廃止 — グローバルソートが上位互換。
- 3D メッシュは深度バッファがあるため不透明は従来通り。半透明・エフェクト・オーバーレイの
  提出順に (DrawLayer, DrawPriority) を適用する (Scene3D 側の収集順)。

### AComponent

- `class AComponent : public FObject` — フックは FComponent2D の全量を継承:
  OnRequire / OnAttach(ANode&) / OnUpdate / OnFixedUpdate / OnDraw(RenderContext&) /
  OnDrawPostChildren / OnAttachServices / OnDetach / QueryLight / QueryShadowCaster /
  QueryPrimitive。3D 向け追加フックは **vtable 末尾追加** の従来方針。
- `ComponentKindOf<T>()` (RTTI 不使用型タグ) / `ACS_GAME_COMPONENT_KIND` は維持。
- 既存の 2D 系コンポーネント (Sprite2D/SpriteAnim/Trigger/RigidBody2D/...) と
  3D 系 (MeshComponent3D 等) は AComponent 派生に書き換え、クラス名は既存のまま
  (F プレフィックス維持 — ACS 基底を継承するのは Node/Component の「基底」であり、
  個別コンポーネント名の一括改名は本移行のスコープ外)。

### プール / シリアライズ / シーン

- `FNodePool` + `FNode3DPool` → `ANodePool` に統合。
- SceneSerialize / SceneTextLoader は transform を 3D フィールド (pos3/quat/scale3) に拡張。
  旧 2D フォーマットの読み込み互換は **切る** (一括置換方針、pre-1.0)。
- FScene2D / FScene3D は当面 ANode ツリーを持つ形に置換 (シーン自体の統合は次期フェーズ)。

## 移行手順 (タスク #2〜#5)

1. ANode / AComponent / ANodePool 実装 + 旧 4 クラス削除 (task #2)
2. Scene2D/Scene3D 描画パス置換 + グローバルソート実装 (task #3)
3. gameframework 内部 (~40 ファイル) + tests (~10) 一括置換、全テスト緑 (task #4)
4. samples ~15 本 + docs/tutorials 10 章 + reference data + editor C# (task #5)

削除対象: Node2D.{h,cpp} / Node3D.{h,cpp} / Component2D.h / Component3D.h /
NodePool.{h,cpp} / Node3DPool.{h,cpp} (統合後)。
