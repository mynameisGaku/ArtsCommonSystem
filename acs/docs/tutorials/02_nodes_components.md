# ノードツリー & コンポーネント (FNode2D / FComponent2D)

ACS GameFramework のシーンは **`FNode2D` の親子ツリー** でできています。各ノードは `FTransform2D`（位置・回転・スケール）を持ち、振る舞いは **継承（`OnUpdate` を override）** か **コンポーネント合成（`AddComponent<T>()`）** のどちらでも書けます。「キャラやエフェクトを階層構造で並べたい」「Unity 風に `GetComponent<T>()` で部品を組み合わせたい」ときに使います。

- ヘッダ: `acs/src/gameframework/Node2D.h` / `Component2D.h` / `Transform2D.h`
- 名前空間: `acs::game`（`acs::f32` などの型は `acs` 直下）

---

## 最小例

プレーンな `FNode2D` に「毎フレーム回す」コンポーネントを付ける、verified サンプル 28 の構成そのものです。

```cpp
#include "gameframework/GameFramework.h"
using namespace acs;
using namespace acs::game;

// --- 自作コンポーネント ---
class RotateComponent : public FComponent2D {
public:
    ACS_GAME_COMPONENT_KIND(RotateComponent)        // 型 ID を登録 (必須・1 行)
    explicit RotateComponent(f32 speed_rps) noexcept : m_Speed(speed_rps) {}

    void OnUpdate(f32 dt) noexcept override {
        Owner().Local().rotation += m_Speed * dt;   // Owner() = 取り付け先 FNode2D
    }
private:
    f32 m_Speed;
};

// --- ツリーを組む (シーンの OnEnter などで) ---
FNode2D root;                                        // root は自前で持つ (Scene が握る)

auto node = MakeUnique<FNode2D>();
node->Local().position = FVec2{-10.0f, 0.0f};
FNode2D& n = root.AddChild(Move(node));              // 所有権を渡す。戻り値は参照
n.AddComponent<RotateComponent>(/*speed_rps=*/2.0f); // 即 OnAttach が呼ばれる

// --- 毎フレーム (root から 1 回ずつ) ---
root.UpdateTree(dt);                 // subtree 全体の OnUpdate + コンポーネント OnUpdate
root.ResolveStructuralChanges();     // Destroy/Reparent をフレーム境界で適用
```

ポイント: ノードは **non-copy / non-move**。必ず `MakeUnique<T>(...)` で作り `Move()` で `AddChild` に渡します。

---

## 主要API

### FNode2D — ツリー操作

| API | 説明 |
| --- | --- |
| `FNode2D& AddChild(TUniquePtr<FNode2D> child)` | 子を追加し所有権を奪う。**即時に `OnSpawn`** を呼ぶ。戻り値は追加した子への参照（チェイン用） |
| `FNode2D* Parent() const` | 親。root なら `nullptr` |
| `u32 ChildCount() const` / `FNode2D* Child(u32 i)` | 子の数 / i 番目の子（範囲外は `nullptr`） |
| `void Destroy()` | 「破棄予定」マーク。実破棄は次の `ResolveStructuralChanges()`（子から先に reap → `OnDespawn`） |
| `bool IsPendingDestroy() const` | 破棄予定か |
| `void Reparent(FNode2D& new_parent)` | 別の親へ移動を**要求**（フレーム境界で適用）。`OnSpawn/OnDespawn` は呼ばれない |

### FNode2D — Transform / フラグ

| API | 説明 |
| --- | --- |
| `FTransform2D& Local()` | ローカル transform（真値）。`Local().position` 等を直接書き換える |
| `FTransform2D World() const` | 親をたどって合成した world transform を**オンザフライ計算**（Phase 1 はキャッシュなし） |
| `void SetEnabled(bool)` / `bool IsEnabled()` | 無効ノードは subtree ごと update をスキップ |
| `void SetVisible(bool)` / `bool IsVisible()` | 非可視ノードは subtree ごと draw をスキップ |

### FNode2D — コンポーネント

| API | 説明 |
| --- | --- |
| `T& AddComponent<T>(Args&&...)` | `T` を構築・attach し参照を返す。**`OnRequire`→`OnAttach` を即時呼出**。同じ型を複数付けても OK |
| `T* GetComponent<T>()` | 最初に見つかった `T` を返す（線形探索）。無ければ `nullptr` |
| `T& GetOrAddComponent<T>(Args&&...)` | 有れば返す、無ければ追加して返す（依存解決に使う） |
| `bool HasComponent<T>() const` | 付いているか |
| `bool RemoveComponent<T>()` | 最初の `T` を 1 つ `OnDetach`→破棄。`true`=除去した |
| `u32 ComponentCount() const` | コンポーネント数 |

### FNode2D — root から呼ぶ走査

| API | 説明 |
| --- | --- |
| `void UpdateTree(f32 dt)` | subtree の `OnUpdate` とコンポーネント `OnUpdate` を伝播 |
| `void FixedUpdateTree(f32 fixed_dt)` | 固定刻み `OnFixedUpdate` を伝播（`FGame` の fixed-step から呼ぶ） |
| `void DrawTree(RenderContext& rc)` | subtree の描画（`OnDraw` → 子 → `OnDrawPostChildren`） |
| `void ResolveStructuralChanges()` | `Destroy`/`Reparent` をまとめて適用。**毎フレーム 1 回**必須 |

### FNode2D — ライフサイクル override

`OnSpawn()` / `OnUpdate(f32 dt)` / `OnFixedUpdate(f32 fixed_dt)` / `OnDraw(RenderContext&)` / `OnDespawn()` — 全て `noexcept` 必須、必要なものだけ override。

### FComponent2D — ライフサイクル override

| フック | 呼ばれるタイミング |
| --- | --- |
| `OnRequire(FNode2D& owner)` | `AddComponent` 内で **`OnAttach` の前に 1 回**。`owner.GetOrAddComponent<Dep>()` で依存を先に確保 |
| `OnAttach(FNode2D& owner)` | attach 直後（owner ref はここで `_SetOwner` 済、以降 `Owner()` で取得） |
| `OnUpdate(f32 dt)` | ノードの `OnUpdate` の**後** |
| `OnFixedUpdate(f32 fixed_dt)` | 固定刻み。catch-up で複数回、slow-down clamp で 0 回もあり |
| `OnDraw(RenderContext&)` | ノードの `OnDraw` の後 |
| `OnDrawPostChildren(RenderContext&)` | `OnDraw` と **子ツリー描画の後**（ステンシルマスク解除など） |
| `OnDetach()` | コンポーネント除去・ノード破棄時 |

その他: `FNode2D& Owner()`（取り付け先）、`bool HasOwner()`、`const void* Kind()`（`ACS_GAME_COMPONENT_KIND` で実装）。

### FTransform2D

`FVec2 position` / `f32 rotation`(ラジアン) / `FVec2 scale` の値型（20 byte）。

| API | 説明 |
| --- | --- |
| `FTransform2D Compose(const FTransform2D& local) const` | `world = parent.Compose(local)`。親座標系に local を載せる |
| `FMat4 ToMat4() const` | 4x4 が要る場面だけ（`FSpriteBatch::SetView` 等）。合成内では使わない |
| `static FTransform2D Identity()` | 単位 transform |

合成規約: `scale=親*子`、`rotation=親+子`、`position=親pos + Rotate(親scale*子pos, 親rot)`。

---

## よく使うパターン

### 1. 親子で transform が伝播する「車輪とスポーク」（サンプル 28）

```cpp
auto wheel_up = MakeUnique<RotatingNode>(/*speed=*/1.0f, "wheel");
wheel_up->Local().position = FVec2{10.0f, 0.0f};
FNode2D& wheel = root.AddChild(Move(wheel_up));     // root の子

auto spoke_up = MakeUnique<RotatingNode>(/*speed=*/0.0f, "spoke");
spoke_up->Local().position = FVec2{2.0f, 0.0f};     // wheel から見た相対位置
wheel.AddChild(Move(spoke_up));                      // wheel が回ると spoke も回る
```
`wheel.Local().rotation` が変わると、子 `spoke` の `World()` は親回転 + 親位置を含めて再計算されます。

### 2. 継承 vs 合成、同じ動きを 2 通りで（サンプル 28）

```cpp
// (a) 継承版: FNode2D を継承して OnUpdate を override
class RotatingNode : public FNode2D {
public:
    void OnUpdate(f32 dt) noexcept override { Local().rotation += m_Speed * dt; }
};

// (b) 合成版: プレーン FNode2D + RotateComponent (上の最小例)
auto rotator = MakeUnique<FNode2D>();
rotator->AddComponent<RotateComponent>(2.0f);
```
キャラ固有のロジックは継承、横展開する汎用部品（回転・点滅・物理）はコンポーネントが目安です。

### 3. 依存コンポーネントを自動で揃える（RequireComponent）

```cpp
class FFollowComponent : public FComponent2D {
public:
    ACS_GAME_COMPONENT_KIND(FFollowComponent)
    void OnRequire(FNode2D& owner) noexcept override {
        owner.GetOrAddComponent<FSprite2DComponent>(FVec2{1,1}, FVec4{1,1,1,1});
    }
    void OnAttach(FNode2D&) noexcept override {
        // OnRequire で先に積まれているので、ここで必ず取れる
        m_Sprite = Owner().GetComponent<FSprite2DComponent>();
    }
private:
    FSprite2DComponent* m_Sprite = nullptr;
};
```
`OnRequire` は `OnAttach` の前に走るので、依存先は `OnAttach`/`OnUpdate` から `GetComponent<Dep>()` で確実に取れます。

### 4. 実コンポーネントで body を持たせる（サンプル 55）

```cpp
auto player = MakeUnique<FNode2D>();
player->Local().position = FVec2{0.0f, 0.0f};
player->AddComponent<FSprite2DComponent>(FVec2{0.9f, 0.9f}, FVec4{0.15f, 0.85f, 1.0f, 1.0f});
FPhysicsBody2D& body = player->AddComponent<FPhysicsBody2D>(physics);  // 参照を保持して操作
body.SetCircle(0.45f);
body.gravity = FVec2{0.0f, -14.0f};
m_Player = &Root().AddChild(Move(player));   // 後で参照する用にポインタ控え
```
`AddComponent` の戻り値（参照）を保持しておくと、毎フレーム `body.velocity.x = ...` のように直接いじれます。

---

## 注意点 (gotcha)

- **non-copy/non-move**: `FNode2D`/`FComponent2D` はコピー・ムーブ禁止。生成は必ず `MakeUnique<T>(...)`、参照は生 `FNode2D*`/`T&` で持つ。`AddChild` には `Move()` で渡す。
- **`AddChild` は即 `OnSpawn`**（Phase 1 簡略化）。一方 `Destroy()` / `Reparent()` は**遅延**で、次の `ResolveStructuralChanges()` まで反映されない。`UpdateTree` の後に `ResolveStructuralChanges()` を毎フレーム呼ぶこと（サンプル 28 の `OnUpdate` 末尾参照）。
- **走査中の追加と削除**: `UpdateTree`/`DrawTree` は index ベース。走査中に `AddChild` された子は同フレームで走る（Unity 互換）。`Destroy` は遅延 reap なので走査中に即時除去はされない。
- **`World()` はキャッシュなし**（Phase 1）。深い階層で毎フレーム何度も呼ぶとコストがかさむ。ループ内で使うなら一度ローカル変数に取る。
- **回転はラジアン**。`FTransform2D::rotation` は度ではない。
- **`AddComponent` の戻り値を捨てない**: 後でいじるコンポーネントは戻り値の参照（またはアドレス）を控える。`GetComponent<T>()` で取り直すこともできるが、最初の一致しか返さない線形探索。
- **`Reparent` の制約**: `new_parent == nullptr`、自分自身、または自分の子孫を指定すると **cycle 検出で警告ログ + 無視**。`OnSpawn/OnDespawn` は呼ばれない（生きたまま移動）。
- **`ACS_GAME_COMPONENT_KIND(T)` を忘れない**: これが `Kind()` を実装し、`GetComponent<T>()` の型判定に使われる。書き忘れると純粋仮想のままコンパイルエラー。
- **同じ型を複数 attach 可**: 例えば点滅タイマーを 2 つ。`GetComponent<T>()` は**最初の一致**だけ返す。
- **`World()` の合成順**は `parent.Compose(local)`。`local` を `parent` に対して `Compose` するのではない点に注意（規約は `Transform2D.h` 冒頭コメント）。

---

## 動くサンプル

| 見たいもの | パス |
| --- | --- |
| 継承ノード + 自作コンポーネント + 親子 transform 伝播 | `acs/samples/28_HelloGameFramework/`（特に `GameplayScene.cpp`, `RotateComponent.{h,cpp}`, `RotatingNode.{h,cpp}`） |
| `FScene2D` 上で sprite + physics body を組む実用スターター | `acs/samples/55_HelloScene2D/Scene2DStarter.cpp` |
| エフェクト系コンポーネント（`FWater2DComponent`/`FFire2DComponent`/`FTrail2DComponent`）を `AddComponent` する例 | `acs/samples/59_HelloEffects2D/EffectsDemo.cpp` |

上記 3 サンプルはいずれも実機ビルド + スクリーンショット確認済みです。
