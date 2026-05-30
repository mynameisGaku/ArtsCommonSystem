# 当たり判定 & 物理 & トリガー

ACS の 2D 衝突まわりは **3 つの独立した部品** でできています。用途で使い分けます。

| 部品 | これは何 | いつ使う |
|------|---------|---------|
| `FCollisionWorld2D` | 形状 (AABB / 円 / 凸ポリゴン) を登録して overlap / raycast を問い合わせる「クエリ用ワールド」 | 「この範囲に何がいる?」「壁にレイが当たる?」を知りたい |
| `FPhysicsBody2D` | velocity + gravity を積分し、`FCollisionWorld2D` の他形状にぶつかったら **押し戻し + スライド** する `FComponent2D` | プレイヤー/敵を壁にめり込ませず動かしたい (プラットフォーマー/トップダウン) |
| `FTriggerComponent` / `FTriggerWorld2D` | 押し戻し**せず** overlap の `enter` / `exit` だけ通知する「センサー」 | アイテム取得・ダメージ判定・ゴール到達など「触れたら発火」 |

すべて `namespace acs::game`、形状型 (`Aabb2` / `Circle` / `ConvexPoly2` / `Ray2` / `RayHit2`) は `namespace acs`、ヘッダは `math/Collision2D.h`。

---

## 最小例

### A. クエリだけ (FCollisionWorld2D 単体、GPU 不要)

```cpp
#include "gameframework/CollisionWorld2D.h"
using namespace acs;
using namespace acs::game;

FCollisionWorld2D world;
world.Init(64.0f);                                 // cell_size = 形状最大サイズの 2-3 倍が目安

// 形状を登録 → FShapeId が返る
FShapeId wall   = world.AddAabb  (Aabb2{ FVec2{100, 0}, FVec2{16, 32} });  // center, half_size
FShapeId player = world.AddCircle(Circle{ FVec2{0, 0}, 16.0f });           // center, radius

// 範囲クエリ: 半径 32 の円と重なる形状を全部集める
TArray<FShapeId> hits;
world.OverlapCircle(Circle{ FVec2{0, 0}, 32.0f }, hits);
// hits.Size() に重なった shape 数。中身の FShapeId で wall/player を識別できる

// レイキャスト: 最も近い 1 つだけ返す
RayHit2 rh;
FShapeId hit_id;
if (world.Raycast(Ray2{ FVec2{-50, 0}, FVec2{1, 0} }, /*max_t=*/200.0f, rh, hit_id)) {
    // rh.point / rh.normal / rh.t、hit_id でどの形状か
}
```

> `Aabb2` は **中心 + 半サイズ** です (左上 + 幅ではない)。左上+サイズからは `Aabb2::FromTopLeftSize(tl, size)`、min/max からは `Aabb2::FromMinMax(min, max)` を使います。

### B. 重力で落ちて床を滑る body (FPhysicsBody2D)

`54_HelloCollideSlide` の核そのものです。

```cpp
#include "gameframework/PhysicsBody2D.h"
#include "gameframework/Node2D.h"
#include "gameframework/CollisionWorld2D.h"
using namespace acs::game;

FCollisionWorld2D w; w.Init(8.0f);
w.AddAabb(Aabb2{ FVec2{0, -10}, FVec2{100, 10} });   // 床 (上面 y=0)

FNode2D node;
node.Local().position = FVec2{ 0, 5 };               // 床の上空から落とす
auto& body = node.AddComponent<FPhysicsBody2D>(w);   // ★ world を constructor 引数で渡す
body.SetCircle(1.0f);
body.gravity  = FVec2{ 0, -20 };                     // 下向き重力
body.velocity = FVec2{ 3, 0 };                       // 右へ

for (int i = 0; i < 90; ++i) node.UpdateTree(1.0f / 60.0f);
// → body は床で y を止め (velocity.y ≈ 0)、x 方向は滑り続ける (collide-and-slide)
```

### C. 触れたら発火するセンサー (FTriggerComponent)

`57_HelloTriggers` ベース。

```cpp
#include "gameframework/TriggerComponent.h"
#include "gameframework/Node2D.h"
using namespace acs::game;

constexpr u32 kPlayer = 1u << 3;
constexpr u32 kPickup = 1u << 1;

FTriggerWorld2D tw; tw.Init();
FNode2D root;

auto pnode = MakeUnique<FNode2D>();
pnode->Local().position = FVec2{0, 0};
// layer=自分, mask=反応したい相手
auto& ptrig = pnode->AddComponent<FTriggerComponent>(tw, /*layer=*/kPlayer, /*mask=*/kPickup);
ptrig.SetCircle(0.5f);
ptrig.SetOnEnter(+[](FTriggerComponent& self, FTriggerComponent* other, void* user) noexcept {
    // self (player) が pickup に触れた
}, /*user=*/nullptr);
root.AddChild(Move(pnode));

// 毎フレーム: ノードツリーを update してから world.Tick
root.UpdateTree(1.0f / 60.0f);   // 形状を world に同期
tw.Tick(1.0f / 60.0f);           // ★ ここで overlap 比較 → OnEnter/OnExit 発火
```

---

## 主要 API

### FCollisionWorld2D

| メソッド | 説明 |
|---------|------|
| `void Init(f32 cell_size = 64.0f)` | グリッド初期化。`cell_size` は形状最大サイズの 2-3 倍が目安。0 以下は 64 に補正 |
| `FShapeId AddAabb(const Aabb2&, u32 layer = kAllLayers)` | AABB 登録。`layer` はこの形状が属する bitmask |
| `FShapeId AddCircle(const Circle&, u32 layer = kAllLayers)` | 円登録 |
| `FShapeId AddPolygon(const ConvexPoly2&, u32 layer = kAllLayers)` | 凸ポリゴン登録 (最大 16 頂点) |
| `void UpdateAabb/UpdateCircle/UpdatePolygon(FShapeId, const ...&)` | 移動時に形状を差し替え (次クエリで grid 再構築) |
| `void Remove(FShapeId)` | 削除 (slot 再利用、generation が進み旧 handle 無効化) |
| `void ClearAll()` / `u32 ShapeCount()` | 全破棄 / 登録数 |
| `void OverlapAabb/OverlapCircle/OverlapPolygon(shape, TArray<FShapeId>& out, FShapeId exclude = {}, u32 mask = kAllLayers)` | 範囲内の全形状を `out` に。`mask` で絞込、`exclude` で自身除外 |
| `bool Raycast(const Ray2&, f32 max_t, RayHit2& out_hit, FShapeId& out_id, u32 mask = kAllLayers)` | 最も近い形状 1 つ。当たれば true |
| `FVec2 ResolveCircle/ResolvePolygon(shape, FShapeId exclude = {}, u32 mask = kAllLayers)` | 重なっている全形状からの **押し出し合計ベクトル**。重なり無しなら `{0,0}` |

`static constexpr u32 kAllLayers = 0xFFFFFFFFu;` — layer/mask 既定値 (= 全部が候補)。

### FShapeId / FTriggerId (どちらも同じ generational handle)

- `bool IsValid()` — packed が 0 でなければ true (0 = 無効)。
- `u32 Index()` / `u8 Generation()` — 内訳。直接使うことはほぼ無い。
- `operator== / !=` あり。デフォルト構築 `FShapeId{}` は無効値。

### FPhysicsBody2D (FComponent2D 派生)

| メンバ | 説明 |
|--------|------|
| `explicit FPhysicsBody2D(FCollisionWorld2D& world)` | constructor で衝突ワールドが**必須**。`AddComponent<FPhysicsBody2D>(world)` で渡す |
| `void SetCircle(f32 radius)` | 円形状。半径 0 以下は 0.001 に補正 |
| `void SetAabb(FVec2 half_size)` | AABB 形状 (半サイズ指定) |
| `void SetPolygon(const ConvexPoly2& local_poly)` | 凸ポリゴン (body 原点基準のローカル頂点) |
| `FVec2 velocity / acceleration / gravity` | 動力学。`OnUpdate(dt)` で積分される |
| `bool slide = true` | `true` = collide-and-slide、`false` = 旧来の軸独立ブロック |
| `FShapeId Handle()` | 現在の collision handle |

`OnUpdate` 内で velocity を積分→移動→`ResolveCircle/ResolvePolygon` で貫通解消→法線方向の速度成分を消してスライド、を自動で行います。**自分自身は `exclude` で除外**されるので自己衝突しません。

### FTriggerComponent (FComponent2D 派生)

| メンバ | 説明 |
|--------|------|
| `FTriggerComponent(FTriggerWorld2D& world, u32 layer = kAllLayers, u32 mask = kAllLayers)` | trigger ワールド + 自分の layer + 反応する mask |
| `void SetCircle(f32 radius)` / `void SetAabb(FVec2 half_size)` | 形状設定 |
| `void SetLayer(u32)` / `void SetMask(u32)` / `Layer()` / `Mask()` | layer/mask の後変更・取得 |
| `void SetOnEnter(TriggerFn, void* user)` / `void SetOnExit(TriggerFn, void* user)` | 関数ポインタ版コールバック |
| `virtual void OnTriggerEnter(FTriggerComponent* other)` / `OnTriggerExit(...)` | サブクラス override 版 (必要な方だけ override) |

`TriggerFn` のシグネチャは `void(*)(FTriggerComponent& self, FTriggerComponent* other, void* user) noexcept`。

発火条件は **`other.layer & this.mask != 0`** のときだけ this のハンドラが鳴ります (幾何の overlap 自体は全 pair で計算され、フィルタはコンポーネント層で適用)。

### ESvc — サービス有効化

シーン (`Scene`) 内で `FCollisionWorld2D` / `FTriggerWorld2D` を使うときは `WantedServices()` で宣言すると、`Services().Physics()` / `Services().Triggers()` で取り出せ、Triggers は自動で `Tick` されます。

```cpp
class GameplayScene : public acs::game::Scene {
public:
    ESvc WantedServices() const noexcept override {
        return ESvc::Default2D | ESvc::Physics2D | ESvc::Triggers;
    }
    void OnEnter() noexcept override {
        Services().Physics().Init(64.0f);
        Services().Triggers().Init();
        // body は Services().Physics() を渡して構築:
        // node->AddComponent<FPhysicsBody2D>(Services().Physics());
    }
};
```

`ESvc` 値: `Clock`/`Tweens`/`Sequences`/`Input`/`Camera2D`/`Physics2D`/`Triggers`、合成 `Default2D = Clock|Tweens|Sequences|Input`。`|` / `&` / `SvcHas` あり。

---

## よく使うパターン

### 1. layer / mask で絞り込む (player / pickup / wall)

`57_HelloTriggers` の検証コードがそのままレシピです。

```cpp
constexpr u32 kWall   = 1u << 0;
constexpr u32 kPickup = 1u << 1;

FCollisionWorld2D world; world.Init(1.0f);
world.AddAabb  (Aabb2{ FVec2{0,0}, FVec2{1,1} }, kWall);
world.AddCircle(Circle{ FVec2{0,0}, 0.5f },      kPickup);

TArray<FShapeId> hits;
world.OverlapCircle(Circle{ FVec2{0,0}, 3.0f }, hits, {}, kWall);          // → 1 (壁のみ)
world.OverlapCircle(Circle{ FVec2{0,0}, 3.0f }, hits, {}, kWall|kPickup);  // → 2 (両方)
world.OverlapCircle(Circle{ FVec2{0,0}, 3.0f }, hits, {}, kPickup);        // → 1 (pickup のみ)
world.OverlapCircle(Circle{ FVec2{0,0}, 3.0f }, hits);                     // → 2 (mask 既定=全部)
```

`out` 引数 (`hits`) は呼ぶたびに上書きされます。前回結果は残りません。

### 2. ポリゴン body も同じくスライドする

`54_HelloCollideSlide` の (2)。凸包コライダーを `SetPolygon` に渡すだけ。

```cpp
auto& body = node.AddComponent<FPhysicsBody2D>(w);
ConvexPoly2 local;                            // 2x2 ボックス (中心原点)
local.Add(FVec2{-1,-1}); local.Add(FVec2{1,-1});
local.Add(FVec2{ 1, 1}); local.Add(FVec2{-1, 1});
body.SetPolygon(local);                       // local_poly は body 原点基準
body.gravity  = FVec2{0, -20};
body.velocity = FVec2{4, 0};
```

`ConvexPoly2` は `Add(FVec2)` で頂点を足し、最大 16 頂点。巻き順は問いません (凸であること前提)。

### 3. enter/exit を 2 物体で受ける (双方向)

`57_HelloTriggers` の core。player と pickup を互いの mask に入れると、両方のハンドラが鳴ります。

```cpp
auto& ptrig = pnode->AddComponent<FTriggerComponent>(tw, kPlayer, kPickup);  // player は pickup に反応
auto& ktrig = knode->AddComponent<FTriggerComponent>(tw, kPickup, kPlayer);  // pickup は player に反応
ptrig.SetCircle(0.5f);  ktrig.SetCircle(0.5f);
// ... SetOnEnter/SetOnExit を各々設定 ...

root.UpdateTree(1.0f/60.0f);  tw.Tick(1.0f/60.0f);   // 重なれば両方 enter
// player を遠くへ動かす:
player->Local().position = FVec2{10, 10};
root.UpdateTree(1.0f/60.0f);  tw.Tick(1.0f/60.0f);   // 両方 exit
```

サブクラス派生で書く場合 (header の推奨スタイル):

```cpp
class FPickup : public FTriggerComponent {
public:
    ACS_GAME_COMPONENT_KIND(FPickup)
    using FTriggerComponent::FTriggerComponent;
    void OnTriggerEnter(FTriggerComponent* other) noexcept override { /* 取得処理 */ }
};
auto& pk = node->AddComponent<FPickup>(tw, /*layer=*/kPickup, /*mask=*/kPlayer);
pk.SetCircle(0.4f);
```

### 4. ResolveCircle を直接使った貫通解消

body を使わず手動で押し戻したいとき (`54_HelloCollideSlide` の (0))。

```cpp
FCollisionWorld2D w; w.Init(8.0f);
w.AddAabb(Aabb2{ FVec2{0,-10}, FVec2{100,10} });          // 上面 y=0
FVec2 push = w.ResolveCircle(Circle{ FVec2{0, 0.5f}, 1.0f });  // 中心 y=0.5 が床に 0.5 めり込む
// push ≈ (0, +0.5) → my_pos += push で抜け出す
```

---

## 注意点 (gotcha)

- **`FShapeId{}` / `FTriggerId{}` (packed==0) は無効値**。`AddXxx` の戻り値が有効な最初の handle で、無効値は「自身除外なし」の意味でも使えます (`OverlapCircle(..., {}, mask)`)。
- **SpatialGrid は lazy rebuild**。`Add` / `Update` / `Remove` は `m_Dirty` を立てるだけで、**クエリ直前に full rebuild** されます。だから「移動 → `UpdateCircle` → そのまま `OverlapCircle`」で最新状態が反映されます。が、大量形状で毎フレーム full rebuild はコスト源 (Phase 1 仕様)。
- **layer/mask の既定は `kAllLayers` (全部)**。1 つでも layer を指定したら、クエリ側 mask も合わせて指定しないと「全部ヒット」のままです。
- **`FTriggerWorld2D::Tick(dt)` を毎フレーム必ず呼ぶ**。これが overlap の前フレ/今フレ比較 → enter/exit 発火の本体です。形状の `Update` だけでは何も鳴りません。シーン内で `ESvc::Triggers` を有効化していれば、シーンの `OnUpdate` 後に自動で `Tick` されます (手動 Tick 不要)。
- **`FPhysicsBody2D` の積分は body 自身の `OnUpdate`** で行われます。`node.UpdateTree(dt)` を回す (= ノードツリーが update される) ことが前提。`FCollisionWorld2D` 側に「physics tick」は無く、Physics2D サービスは自動 tick の対象**外**です (= body コンポーネントが駆動)。
- **`FPhysicsBody2D` は kinematic / swept**。剛体ソルバ (回転・運動量交換) ではありません。「めり込まず動く + 床/壁で速度を殺してスライド」が責務。質量・反発・回転が要るボール同士の弾性衝突は別物 (`08_HelloPhysics2D` は `math/Collision2D.h` の `Resolve(Circle,Circle)` を手書きで回しており、`FPhysicsBody2D` は使っていません)。
- **dt が大きすぎると破綻**します。`08_HelloPhysics2D` は `if (dt > 0.05f) dt = 0.05f;` で clamp しています。スパイク時は同様に clamp 推奨。
- **`Aabb2` は中心 + 半サイズ**。`Circle` は中心 + 半径。`ConvexPoly2` の頂点は最大 16 (`kMaxVerts`)。`Ray2` の `direction` は正規化不要ですが、`t` の意味 (`origin + direction * t`) は direction の長さに依存します。
- **`SetPolygon` の `local_poly` は body 原点基準のローカル頂点**。world 形状は「body 位置 + ローカル頂点」。スプライト凸包をそのまま渡すなら、中心を原点へずらしてから渡します。
- **`FTriggerWorld2D` / `FCollisionWorld2D` は非コピー・非ムーブ** (handle 安定性のため)。参照かポインタで持ち回します。`FTriggerWorld2D` のブロードフェーズは現状 O(N^2) (Phase 3 仕様、将来 grid 化予定)。

---

## 動くサンプル

| サンプル | パス | 何が見られる |
|---------|------|------------|
| `57_HelloTriggers` | `acs/samples/57_HelloTriggers/main.cpp` | layer/mask 絞り込み + FTriggerComponent の enter/exit。ヘッドレス (GPU 不要) で assert 検証 |
| `54_HelloCollideSlide` | `acs/samples/54_HelloCollideSlide/main.cpp` | 円/ポリゴン body の重力落下 + 床スライド + `ResolveCircle` 直接呼び。ヘッドレス検証 |
| `08_HelloPhysics2D` | `acs/samples/08_HelloPhysics2D/PhysicsScene.cpp` | `math/Collision2D.h` の `Resolve` を手書きで回す円のバウンド (描画あり)。`FPhysicsBody2D` は未使用な点に注意 |

ヘッダ実体: `acs/src/gameframework/CollisionWorld2D.h` / `PhysicsBody2D.h` / `TriggerComponent.h` / `TriggerWorld2D.h`、形状型は `acs/src/math/Collision2D.h`、サービス宣言は `acs/src/gameframework/SceneServices.h`。
