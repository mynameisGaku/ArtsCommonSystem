# 当たり判定 & 物理 & トリガー

ACS の 2D 衝突まわりは **3 つの独立した部品** でできています。用途で使い分けます。

| 部品 | これは何 | いつ使う |
|------|---------|---------|
| `FCollisionWorld2D` | 形状 (AABB / 円 / 凸ポリゴン / **OBB=回転矩形**) を登録して overlap / raycast を問い合わせる「クエリ用ワールド」 | 「この範囲に何がいる?」「壁にレイが当たる?」を知りたい |
| `APhysicsBody2D` | velocity + gravity を積分し、`FCollisionWorld2D` の他形状にぶつかったら **押し戻し + スライド** する `AComponent` | プレイヤー/敵を壁にめり込ませず動かしたい (プラットフォーマー/トップダウン) |
| `ATriggerComponent` / `FTriggerWorld2D` | 押し戻し**せず** overlap の `enter` / `exit` だけ通知する「センサー」 | アイテム取得・ダメージ判定・ゴール到達など「触れたら発火」 |

すべて `namespace acs::game`、形状型 (`FAabb2` / `FCircle` / `FConvexPoly2` / `FObb2` / `FRay2` / `FRayHit2`) は `namespace acs`、ヘッダは `math/Collision2D.h`。

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
FShapeId wall   = world.AddAabb  (FAabb2{ FVec2{100, 0}, FVec2{16, 32} });  // center, half_size
FShapeId player = world.AddCircle(FCircle{ FVec2{0, 0}, 16.0f });           // center, radius

// 範囲クエリ: 半径 32 の円と重なる形状を全部集める
TArray<FShapeId> hits;
world.OverlapCircle(FCircle{ FVec2{0, 0}, 32.0f }, hits);
// hits.Size() に重なった shape 数。中身の FShapeId で wall/player を識別できる

// レイキャスト: 最も近い 1 つだけ返す
FRayHit2 rh;
FShapeId hit_id;
if (world.Raycast(FRay2{ FVec2{-50, 0}, FVec2{1, 0} }, /*max_t=*/200.0f, rh, hit_id)) {
    // rh.point / rh.normal / rh.t、hit_id でどの形状か
}
```

> `FAabb2` は **中心 + 半サイズ** です (左上 + 幅ではない)。左上+サイズからは `FAabb2::FromTopLeftSize(tl, size)`、min/max からは `FAabb2::FromMinMax(min, max)` を使います。

### B. 重力で落ちて床を滑る body (APhysicsBody2D)

`54_HelloCollideSlide` の核そのものです。

```cpp
#include "gameframework/PhysicsBody2D.h"
#include "gameframework/ANode.h"
#include "gameframework/CollisionWorld2D.h"
using namespace acs;
using namespace acs::game;

FCollisionWorld2D w; w.Init(8.0f);
w.AddAabb(FAabb2{ FVec2{0, 10}, FVec2{100, 10} });    // 床 (上面 y=0、本体 y∈[0,20]=画面下)

ANode node;
node.SetPosition2D(FVec2{ 0, -5 });              // 床の上空(画面上=小さい Y)から落とす
auto& body = node.AddComponent<APhysicsBody2D>(w);   // ★ world を constructor 引数で渡す
body.SetCircle(1.0f);
body.gravity  = FVec2{ 0, 20 };                      // 下向き重力 (+Y=画面下)
body.velocity = FVec2{ 3, 0 };                       // 右へ

for (int i = 0; i < 90; ++i) node.UpdateTree(1.0f / 60.0f);
// → body は床で y を止め (velocity.y ≈ 0)、x 方向は滑り続ける (collide-and-slide)
```

### C. 触れたら発火するセンサー (ATriggerComponent)

`57_HelloTriggers` ベース。

```cpp
#include "gameframework/TriggerComponent.h"
#include "gameframework/ANode.h"
using namespace acs;
using namespace acs::game;

constexpr u32 kPlayer = 1u << 3;
constexpr u32 kPickup = 1u << 1;

FTriggerWorld2D tw; tw.Init();
ANode root;

auto pnode = NewObject<ANode>();
pnode->SetPosition2D(FVec2{0, 0});
// layer=自分, mask=反応したい相手
auto& ptrig = pnode->AddComponent<ATriggerComponent>(tw, /*layer=*/kPlayer, /*mask=*/kPickup);
ptrig.SetCircle(0.5f);
ptrig.SetOnEnter(+[](ATriggerComponent& self, ATriggerComponent* other, void* user) noexcept {
    // self (player) が pickup に触れた
    (void)self; (void)other; (void)user;
}, /*user=*/nullptr);

auto knode = NewObject<ANode>();
knode->SetPosition2D(FVec2{0, 0});  // 最初から player と重ね、enter を確認する
auto& ktrig = knode->AddComponent<ATriggerComponent>(
    tw, /*layer=*/kPickup, /*mask=*/kPlayer);
ktrig.SetCircle(0.5f);

root.AddChild(Move(pnode));
root.AddChild(Move(knode));

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
| `FShapeId AddAabb(const FAabb2&, u32 layer = kAllLayers)` | AABB 登録。`layer` はこの形状が属する bitmask |
| `FShapeId AddCircle(const FCircle&, u32 layer = kAllLayers)` | 円登録 |
| `FShapeId AddPolygon(const FConvexPoly2&, u32 layer = kAllLayers)` | 凸ポリゴン登録 (最大 16 頂点) |
| `FShapeId AddObb(const FObb2&, u32 layer = kAllLayers)` | OBB (回転矩形) 登録。`FObb2{center, half_size, rotation}`。内部は 4 頂点凸ポリとして SAT 判定 |
| `void UpdateAabb/UpdateCircle/UpdatePolygon/UpdateObb(FShapeId, const ...&)` | 移動時に形状を差し替え (次クエリで grid 再構築)。`UpdateObb` は回転する床/障害物に |
| `void Remove(FShapeId)` | 削除 (slot 再利用、generation が進み旧 handle 無効化) |
| `void ClearAll()` / `u32 ShapeCount()` | 全破棄 / 登録数 |
| `void OverlapAabb/OverlapCircle/OverlapPolygon(shape, TArray<FShapeId>& out, FShapeId exclude = {}, u32 mask = kAllLayers)` | 範囲内の全形状を `out` に。`mask` で絞込、`exclude` で自身除外 |
| `bool Raycast(const FRay2&, f32 max_t, FRayHit2& out_hit, FShapeId& out_id, u32 mask = kAllLayers)` | 最も近い形状 1 つ。当たれば true |
| `FVec2 ResolveCircle/ResolvePolygon(shape, FShapeId exclude = {}, u32 mask = kAllLayers)` | 重なっている全形状からの **押し出し合計ベクトル**。重なり無しなら `{0,0}` |

`static constexpr u32 kAllLayers = 0xFFFFFFFFu;` — layer/mask 既定値 (= 全部が候補)。

### FShapeId / FTriggerId (どちらも同じ generational handle)

- `bool IsValid()` — packed が 0 でなければ true (0 = 無効)。
- `u32 Index()` / `u8 Generation()` — 内訳。直接使うことはほぼ無い。
- `operator== / !=` あり。デフォルト構築 `FShapeId{}` は無効値。

### APhysicsBody2D (AComponent 派生)

| メンバ | 説明 |
|--------|------|
| `explicit APhysicsBody2D(FCollisionWorld2D& world) noexcept` | constructor で衝突ワールドが**必須**。`AddComponent<APhysicsBody2D>(world)` で渡す |
| `void SetCircle(f32 radius)` | 円形状。半径 0 以下は 0.001 に補正 |
| `void SetAabb(FVec2 half_size)` | AABB 形状 (半サイズ指定) |
| `void SetPolygon(const FConvexPoly2& local_poly)` | 凸ポリゴン (body 原点基準のローカル頂点) |
| `void SetObb(FVec2 half_size, f32 rotation)` | OBB body。回転を中心原点ローカル poly に焼いて `SetPolygon` へ委譲 |
| `FVec2 velocity / acceleration / gravity` | 動力学。`OnUpdate(dt)` で積分される |
| `bool slide = true` | `true` = collide-and-slide、`false` = 旧来の軸独立ブロック |
| `FShapeId Handle()` | 現在の collision handle |

`OnUpdate` 内で velocity を積分→移動→`ResolveCircle/ResolvePolygon` で貫通解消→法線方向の速度成分を消してスライド、を自動で行います。**自分自身は `exclude` で除外**されるので自己衝突しません。

### ATriggerComponent (AComponent 派生)

| メンバ | 説明 |
|--------|------|
| `explicit ATriggerComponent(FTriggerWorld2D& world, u32 layer = kAllLayers, u32 mask = kAllLayers) noexcept` | trigger ワールド + 自分の layer + 反応する mask |
| `void SetCircle(f32 radius)` / `void SetAabb(FVec2 half_size)` | 形状設定 |
| `void SetLayer(u32)` / `void SetMask(u32)` / `Layer()` / `Mask()` | layer/mask の後変更・取得 |
| `void SetOnEnter(TriggerFn, void* user)` / `void SetOnExit(TriggerFn, void* user)` | 関数ポインタ版コールバック |
| `virtual void OnTriggerEnter(ATriggerComponent* other) noexcept` / `OnTriggerExit(...) noexcept` | サブクラス override 版 (必要な方だけ override) |

`TriggerFn` のシグネチャは `void(*)(ATriggerComponent& self, ATriggerComponent* other, void* user) noexcept`。

発火条件は **`other.layer & this.mask != 0`** のときだけ this のハンドラが鳴ります (幾何の overlap 自体は全 pair で計算され、フィルタはコンポーネント層で適用)。

### ESvc — サービス有効化

シーン (`AScene`) 内で `FCollisionWorld2D` / `FTriggerWorld2D` を使うときは `WantedServices()` で宣言すると、`Services().Physics()` / `Services().Triggers()` で取り出せます。両ワールドはサービス構築時に初期化済みで、Triggers はシーンから自動で `Tick` されます。

```cpp
using namespace acs::game;

class AGameplayScene : public acs::game::AScene {
public:
    ESvc WantedServices() const noexcept override {
        return ESvc::Default2D | ESvc::Physics2D | ESvc::Triggers;
    }
    void OnEnter() noexcept override {
        // Init() の再呼び出しは不要。再初期化すると既存登録を消すため、
        // cell_size を意図的に変えて全登録を作り直す場合だけ行う。
        // body は初期化済みの Services().Physics() を渡して構築:
        // node->AddComponent<APhysicsBody2D>(Services().Physics());
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
world.AddAabb  (FAabb2{ FVec2{0,0}, FVec2{1,1} }, kWall);
world.AddCircle(FCircle{ FVec2{0,0}, 0.5f },      kPickup);

TArray<FShapeId> hits;
world.OverlapCircle(FCircle{ FVec2{0,0}, 3.0f }, hits, {}, kWall);          // → 1 (壁のみ)
world.OverlapCircle(FCircle{ FVec2{0,0}, 3.0f }, hits, {}, kWall|kPickup);  // → 2 (両方)
world.OverlapCircle(FCircle{ FVec2{0,0}, 3.0f }, hits, {}, kPickup);        // → 1 (pickup のみ)
world.OverlapCircle(FCircle{ FVec2{0,0}, 3.0f }, hits);                     // → 2 (mask 既定=全部)
```

`out` 引数 (`hits`) は呼ぶたびに上書きされます。前回結果は残りません。

### 2. ポリゴン body も同じくスライドする

`54_HelloCollideSlide` の (2)。凸包コライダーを `SetPolygon` に渡すだけ。

```cpp
auto& body = node.AddComponent<APhysicsBody2D>(w);
FConvexPoly2 local;                            // 2x2 ボックス (中心原点)
local.Add(FVec2{-1,-1}); local.Add(FVec2{1,-1});
local.Add(FVec2{ 1, 1}); local.Add(FVec2{-1, 1});
body.SetPolygon(local);                       // local_poly は body 原点基準
body.gravity  = FVec2{0, 20};                 // +Y=画面下
body.velocity = FVec2{4, 0};
```

`FConvexPoly2` は `Add(FVec2)` で頂点を足し、最大 16 頂点。巻き順は問いません (凸であること前提)。

### 3. enter/exit を 2 物体で受ける (双方向)

`57_HelloTriggers` の core。player と pickup を互いの mask に入れると、両方のハンドラが鳴ります。

```cpp
using namespace acs;
using namespace acs::game;

constexpr u32 kPlayer = 1u << 3;
constexpr u32 kPickup = 1u << 1;

FTriggerWorld2D tw;
tw.Init();
ANode root;  // root は tw より後に破棄される

auto pnode = NewObject<ANode>();
auto knode = NewObject<ANode>();
auto& ptrig = pnode->AddComponent<ATriggerComponent>(tw, kPlayer, kPickup);  // player は pickup に反応
auto& ktrig = knode->AddComponent<ATriggerComponent>(tw, kPickup, kPlayer);  // pickup は player に反応
ptrig.SetCircle(0.5f);  ktrig.SetCircle(0.5f);
// ... SetOnEnter/SetOnExit を各々設定 ...

TWeakObjectPtr<ANode> player = pnode;
root.AddChild(Move(pnode));
root.AddChild(Move(knode));
root.UpdateTree(1.0f/60.0f);  tw.Tick(1.0f/60.0f);   // 重なれば両方 enter
// player を遠くへ動かす:
if (ANode* p = player.Get()) p->SetPosition2D(FVec2{10, 10});
root.UpdateTree(1.0f/60.0f);  tw.Tick(1.0f/60.0f);   // 両方 exit
```

サブクラス派生で書く場合 (header の推奨スタイル):

```cpp
class APickup : public ATriggerComponent {
public:
    ACS_GAME_COMPONENT_KIND(APickup)
    using ATriggerComponent::ATriggerComponent;
    void OnTriggerEnter(ATriggerComponent* other) noexcept override { /* 取得処理 */ }
};
auto& pk = node->AddComponent<APickup>(tw, /*layer=*/kPickup, /*mask=*/kPlayer);
pk.SetCircle(0.4f);
```

### 4. ResolveCircle を直接使った貫通解消

body を使わず手動で押し戻したいとき (`54_HelloCollideSlide` の (0))。

```cpp
FCollisionWorld2D w; w.Init(8.0f);
w.AddAabb(FAabb2{ FVec2{0, 10}, FVec2{100, 10} });             // 上面 y=0
FVec2 push = w.ResolveCircle(FCircle{ FVec2{0, -0.5f}, 1.0f }); // 下端 y=0.5 が床へ 0.5 めり込む
// push ≈ (0, -0.5) → my_pos += push で画面上側へ抜け出す
```

---

## 注意点 (gotcha)

- **`FShapeId{}` / `FTriggerId{}` (packed==0) は無効値**。`AddXxx` の戻り値が有効な最初の handle で、無効値は「自身除外なし」の意味でも使えます (`OverlapCircle(..., {}, mask)`)。
- **SpatialGrid は lazy rebuild**。`Add` / `Update` / `Remove` は `m_Dirty` を立てるだけで、**クエリ直前に full rebuild** されます。だから「移動 → `UpdateCircle` → そのまま `OverlapCircle`」で最新状態が反映されます。が、大量形状で毎フレーム full rebuild はコスト源 (Phase 1 仕様)。
- **layer/mask の既定は `kAllLayers` (全部)**。1 つでも layer を指定したら、クエリ側 mask も合わせて指定しないと「全部ヒット」のままです。
- **`FTriggerWorld2D::Tick(dt)` を毎フレーム必ず呼ぶ**。これが overlap の前フレ/今フレ比較 → enter/exit 発火の本体です。形状の `Update` だけでは何も鳴りません。シーン内で `ESvc::Triggers` を有効化していれば、シーンの `OnUpdate` 後に自動で `Tick` されます (手動 Tick 不要)。
- **`APhysicsBody2D` の積分は body 自身の `OnUpdate`** で行われます。`node.UpdateTree(dt)` を回す (= ノードツリーが update される) ことが前提。`FCollisionWorld2D` 側に「physics tick」は無く、Physics2D サービスは自動 tick の対象**外**です (= body コンポーネントが駆動)。
- **`APhysicsBody2D` は kinematic / swept**。剛体ソルバ (回転・運動量交換) ではありません。「めり込まず動く + 床/壁で速度を殺してスライド」が責務。質量・反発・回転が要るボール同士の弾性衝突は別物 (`08_HelloPhysics2D` は `math/Collision2D.h` の `Resolve(FCircle,FCircle)` を手書きで回しており、`APhysicsBody2D` は使っていません)。
- **dt が大きすぎると破綻**します。`08_HelloPhysics2D` は `if (dt > 0.05f) dt = 0.05f;` で clamp しています。スパイク時は同様に clamp 推奨。
- **`FAabb2` は中心 + 半サイズ**。`FCircle` は中心 + 半径。`FConvexPoly2` の頂点は最大 16 (`kMaxVerts`)。`FObb2` は中心 + 半サイズ + 回転 (rad、反時計回り)。`FRay2` の `direction` は正規化不要ですが、`t` の意味 (`origin + direction * t`) は direction の長さに依存します。
- **OBB (回転矩形) は 4 頂点凸ポリとして判定**されます。`world.AddObb(FObb2{ FVec2{cx,cy}, FVec2{hw,hh}, rad });` で斜めの壁/プラットフォームを置けて、円 body は `ResolveCircle` 経由で角度のある面に沿って collide-and-slide します。可視化は `sb.DrawRectRotated(cx, cy, hw*2, hh*2, rad, color)`（OBB と同じ中心/回転）。毎フレーム `UpdateObb` すれば回転する障害物に。実例 = `63_HelloVerticalSlice`（斜め OBB バー）。
- **`SetPolygon` の `local_poly` は body 原点基準のローカル頂点**。world 形状は「body 位置 + ローカル頂点」。スプライト凸包をそのまま渡すなら、中心を原点へずらしてから渡します。
- **body / trigger は world を所有しません**。`APhysicsBody2D` と `ATriggerComponent` は constructor で受けた world を参照するため、`FCollisionWorld2D` / `FTriggerWorld2D` を、それらを付けたノードより先に破棄しないでください。シーンサービスを使う場合はシーン寿命に揃います。
- **`FTriggerWorld2D` / `FCollisionWorld2D` は非コピー・非ムーブ** (handle 安定性のため)。参照かポインタで持ち回します。`FTriggerWorld2D` のブロードフェーズは現状 O(N^2) (Phase 3 仕様、将来 grid 化予定)。

---

## 動くサンプル

| サンプル | パス | 何が見られる |
|---------|------|------------|
| `57_HelloTriggers` | `acs/samples/57_HelloTriggers/main.cpp` | layer/mask 絞り込み + ATriggerComponent の enter/exit。ヘッドレス (GPU 不要) で assert 検証 |
| `54_HelloCollideSlide` | `acs/samples/54_HelloCollideSlide/main.cpp` | 円/ポリゴン body の重力落下 + 床スライド + `ResolveCircle` 直接呼び。ヘッドレス検証 |
| `08_HelloPhysics2D` | `acs/samples/08_HelloPhysics2D/HelloPhysics2DApp.cpp` | `math/Collision2D.h` の `Resolve` を手書きで回す円のバウンド (描画あり)。`APhysicsBody2D` は未使用な点に注意 |
| `63_HelloVerticalSlice` | `acs/samples/63_HelloVerticalSlice/main.cpp` | 円プレイヤーが石壁 (AABB) / 水 / 斜め **OBB** バーに collide-and-slide する見下ろし (描画あり、Y-down) |
| `62_HelloPersistVerify` | `acs/samples/62_HelloPersistVerify/main.cpp` | `TestObb` = OBB の math + world 全クエリ経路 (overlap/resolve/raycast) のヘッドレス検証 |

ヘッダ実体: `acs/src/gameframework/CollisionWorld2D.h` / `PhysicsBody2D.h` / `TriggerComponent.h` / `TriggerWorld2D.h`、形状型は `acs/src/math/Collision2D.h`、サービス宣言は `acs/src/gameframework/SceneServices.h`。
