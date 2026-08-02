# ノードツリー & コンポーネント (ANode / AComponent)

ACS GameFramework のシーンは **`ANode` の親子ツリー** でできています。各ノードは共通の `FTransform3D`（位置・回転・スケール）を持ち、2D ゲームでは `Position2D()` / `SetPosition2D()` などの射影ヘルパを使います。振る舞いは **継承（`OnUpdate` を override）** か **コンポーネント合成（`AddComponent<T>()`）** のどちらでも書けます。「キャラやエフェクトを階層構造で並べたい」「Unity 風に `GetComponent<T>()` で部品を組み合わせたい」ときに使います。

- ヘッダ: `acs/src/gameframework/ANode.h` / `AComponent.h` / `Transform3D.h` /
  `Transform2D.h`
- 名前空間: `acs::game`（`acs::f32` などの型は `acs` 直下）

---

## 最小例

プレーンな `ANode` に「毎フレーム回す」コンポーネントを付ける、verified サンプル 28 の構成そのものです。

```cpp
#include "gameframework/GameFramework.h"
using namespace acs;
using namespace acs::game;

// --- 自作コンポーネント ---
class ARotateComponent : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(ARotateComponent)        // 型 ID を登録 (必須・1 行)
    explicit ARotateComponent(f32 speed_rps) noexcept : m_Speed(speed_rps) {}

    void OnUpdate(f32 dt) noexcept override {
        Owner().SetRotation2D(Owner().Rotation2D() + m_Speed * dt);
    }
private:
    f32 m_Speed;
};

// --- ツリーを組む（AScene::OnReady などで）---
void AddRotator(ANode& root) noexcept {
    auto node = NewObject<ANode>();
    node->SetPosition2D(FVec2{-10.0f, 0.0f});
    ANode& child = root.AddChild(Move(node));       // 強参照を移す。戻り値は参照
    child.AddComponent<ARotateComponent>(2.0f);    // 即 OnAttach が呼ばれる
}

// AScene は次の走査を自動実行する。独自 root を使う場合だけ自分で呼ぶ。
void TickRoot(ANode& root, f32 dt) noexcept {
    root.UpdateTree(dt);
    root.ResolveStructuralChanges();
}
```

ポイント: 子ノードは **non-copy / non-move** です。`NewObject<T>(...)` が返す
`TObjectPtr<T>` を `Move()` で `AddChild` に渡します。ノードの生成に `MakeUnique`
は所有モデルが異なるため使いません。

---

## 主要API

### ANode — ツリー操作

| API | 説明 |
| --- | --- |
| `ANode& AddChild(TObjectPtr<ANode> child)` | 子を追加して強参照を保持する。**即時に `OnSpawn`** を呼ぶ。戻り値は追加した子への参照（チェイン用） |
| `ANode* Parent() const` | 親。root なら `nullptr` |
| `u32 ChildCount() const` / `ANode* Child(u32 i)` | 子の数 / i 番目の子（範囲外は `nullptr`） |
| `void Destroy()` | 「破棄予定」マーク。実破棄は次の `ResolveStructuralChanges()`（子から先に reap → `OnDespawn`） |
| `bool IsPendingDestroy() const` | 破棄予定か |
| `void Reparent(ANode& new_parent)` | 別の親へ移動を**要求**（フレーム境界で適用）。`OnSpawn/OnDespawn` は呼ばれない |

### ANode — Transform / フラグ

| API | 説明 |
| --- | --- |
| `FTransform3D& Local()` | 2D/3D 共通のローカル transform（真値） |
| `FTransform3D World() const` | 親をたどって合成した world transform をオンザフライ計算 |
| `Position2D()` / `SetPosition2D()` | x,y を読み書きし、z を温存する |
| `Rotation2D()` / `SetRotation2D()` | Z 軸回転を読み書きする |
| `Scale2D()` / `SetScale2D()` | x,y を読み書きし、scale.z を温存する |
| `Local2D()` / `World2D()` | 2D 描画向けの `FTransform2D` 射影を返す |
| `void SetEnabled(bool)` / `bool IsEnabled()` | 無効ノードは subtree ごと update をスキップ |
| `void SetVisible(bool)` / `bool IsVisible()` | 非可視ノードは subtree ごと draw をスキップ |

### ANode — コンポーネント

| API | 説明 |
| --- | --- |
| `T& AddComponent<T>(Args&&...)` | `T` を構築・attach し参照を返す。**`OnRequire`→`OnAttach` を即時呼出**。同じ型を複数付けても OK |
| `T* GetComponent<T>()` | 最初に見つかった `T` を返す（線形探索）。無ければ `nullptr` |
| `T& GetOrAddComponent<T>(Args&&...)` | 有れば返す、無ければ追加して返す（依存解決に使う） |
| `bool HasComponent<T>() const` | 付いているか |
| `bool RemoveComponent<T>()` | 最初の `T` を 1 つ `OnDetach`→破棄。`true`=除去した |
| `u32 ComponentCount() const` | コンポーネント数 |

### ANode — root から呼ぶ走査

| API | 説明 |
| --- | --- |
| `void UpdateTree(f32 dt)` | subtree の `OnUpdate` とコンポーネント `OnUpdate` を伝播 |
| `void FixedUpdateTree(f32 fixed_dt)` | 固定刻み `OnFixedUpdate` を伝播（`CGame` の fixed-step から呼ぶ） |
| `void DrawTree(FRenderContext& rc)` | subtree の描画（`OnDraw` → 子 → `OnDrawPostChildren`） |
| `void ResolveStructuralChanges()` | `Destroy`/`Reparent` をまとめて適用。**毎フレーム 1 回**必須 |

### ANode — ライフサイクル override

`OnSpawn()` / `OnUpdate(f32 dt)` / `OnFixedUpdate(f32 fixed_dt)` / `OnDraw(FRenderContext&)` / `OnDespawn()` — 全て `noexcept` 必須、必要なものだけ override。

### AComponent — ライフサイクル override

| フック | 呼ばれるタイミング |
| --- | --- |
| `OnRequire(ANode& owner)` | `AddComponent` 内で **`OnAttach` の前に 1 回**。`owner.GetOrAddComponent<Dep>()` で依存を先に確保 |
| `OnAttach(ANode& owner)` | attach 直後（owner ref はここで `_SetOwner` 済、以降 `Owner()` で取得） |
| `OnUpdate(f32 dt)` | ノードの `OnUpdate` の**後** |
| `OnFixedUpdate(f32 fixed_dt)` | 固定刻み。catch-up で複数回、slow-down clamp で 0 回もあり |
| `OnDraw(FRenderContext&)` | ノードの `OnDraw` の後 |
| `OnDrawPostChildren(FRenderContext&)` | `OnDraw` と **子ツリー描画の後**（ステンシルマスク解除など） |
| `OnDetach()` | コンポーネント除去・ノード破棄時 |

その他: `ANode& Owner()`（取り付け先）、`bool HasOwner()`、`const void* Kind()`（`ACS_GAME_COMPONENT_KIND` で実装）。

### FTransform3D と 2D 射影

`ANode` が保持する真値は `FTransform3D` です。`position` は `FVec3`、`rotation` は
`FQuat`、`scale` は `FVec3` です。2D コードでは `Position2D()` /
`SetPosition2D()`、`Rotation2D()` / `SetRotation2D()`、`Scale2D()` /
`SetScale2D()` を使うと、未使用の Z 成分を壊しません。

`Local2D()` / `World2D()` が返す `FTransform2D` は描画や当たり判定向けの値スナップショットで、
`FVec2 position` / `f32 rotation`（ラジアン）/ `FVec2 scale` を持ちます。

| API | 説明 |
| --- | --- |
| `FTransform2D Compose(const FTransform2D& local) const` | `world = parent.Compose(local)`。親座標系に local を載せる |
| `FMat4 ToMat4() const` | 4x4 が要る場面だけ（`CSpriteBatch::SetView` 等）。合成内では使わない |
| `static FTransform2D Identity()` | 単位 transform |

合成規約: `scale=親*子`、`rotation=親+子`、`position=親pos + Rotate(親scale*子pos, 親rot)`。

---

## よく使うパターン

### 1. 親子で transform が伝播する「車輪とスポーク」（サンプル 28）

```cpp
auto wheel_up = NewObject<ARotatingNode>(/*speed=*/1.0f, "wheel");
wheel_up->SetPosition2D(FVec2{10.0f, 0.0f});
ANode& wheel = root.AddChild(Move(wheel_up));     // root の子

auto spoke_up = NewObject<ARotatingNode>(/*speed=*/0.0f, "spoke");
spoke_up->SetPosition2D(FVec2{2.0f, 0.0f});     // wheel から見た相対位置
wheel.AddChild(Move(spoke_up));                      // wheel が回ると spoke も回る
```
`wheel.SetRotation2D(...)` で回転が変わると、子 `spoke` の `World2D()` は親回転と
親位置を含めて再計算されます。

### 2. 継承 vs 合成、同じ動きを 2 通りで（サンプル 28）

```cpp
// (a) 継承版: ANode を継承して OnUpdate を override
class ARotatingNode : public ANode {
public:
    void OnUpdate(f32 dt) noexcept override {
        SetRotation2D(Rotation2D() + m_Speed * dt);
    }
};

// (b) 合成版: プレーン ANode + ARotateComponent (上の最小例)
auto rotator = NewObject<ANode>();
rotator->AddComponent<ARotateComponent>(2.0f);
```
キャラ固有のロジックは継承、横展開する汎用部品（回転・点滅・物理）はコンポーネントが目安です。

### 3. 依存コンポーネントを自動で揃える（RequireComponent）

```cpp
class AFollowComponent : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(AFollowComponent)
    void OnRequire(ANode& owner) noexcept override {
        owner.GetOrAddComponent<ASprite2DComponent>(FVec2{1,1}, FVec4{1,1,1,1});
    }
    void OnAttach(ANode&) noexcept override {
        // OnRequire で先に積まれているので、ここで必ず取れる
        m_Sprite = Owner().GetComponent<ASprite2DComponent>();
    }
private:
    ASprite2DComponent* m_Sprite = nullptr;
};
```
`OnRequire` は `OnAttach` の前に走るので、attach 時点では依存先を確実に取得できます。
ただし後から `RemoveComponent<Dep>()` できるため、生ポインタを保存する場合は依存先を
動的に除去しない設計にするか、使用時に `GetComponent<Dep>()` で取り直してください。

### 4. 実コンポーネントで body を持たせる（サンプル 55）

```cpp
auto player = NewObject<ANode>();
player->SetPosition2D(FVec2{0.0f, 0.0f});
player->AddComponent<ASprite2DComponent>(FVec2{0.9f, 0.9f}, FVec4{0.15f, 0.85f, 1.0f, 1.0f});
APhysicsBody2D& body = player->AddComponent<APhysicsBody2D>(physics);  // 参照を保持して操作
body.SetCircle(0.45f);
body.gravity = FVec2{0.0f, 14.0f};   // +Y=画面下: 下向き重力は正の Y
m_Player = player;                   // TWeakObjectPtr<ANode> へ非所有参照を控える
Root().AddChild(Move(player));        // 強参照はツリーへ移す
```
`AddComponent` の戻り値は owner ノードの生存中だけ有効です。長期間使うノード参照は
`TWeakObjectPtr<ANode>` で保持し、`Get()` で生存確認してからコンポーネントを取り直すと安全です。

---

## 注意点 (gotcha)

- **non-copy/non-move**: `ANode`/`AComponent` はコピー・ムーブ禁止。子ノードは
  `NewObject<T>(...)` で生成し、`AddChild` へ `Move()` する。ツリーは
  `TObjectPtr<ANode>` で所有し、長期の外部参照には `TWeakObjectPtr<ANode>` を使う。
- **`AddChild` は即 `OnSpawn`**（Phase 1 簡略化）。一方 `Destroy()` / `Reparent()` は**遅延**で、次の `ResolveStructuralChanges()` まで反映されない。`UpdateTree` の後に `ResolveStructuralChanges()` を毎フレーム呼ぶこと（サンプル 28 の `OnUpdate` 末尾参照）。
- **走査中の追加と削除**: `UpdateTree`/`DrawTree` は index ベース。走査中に `AddChild` された子は同フレームで走る（Unity 互換）。`Destroy` は遅延 reap なので走査中に即時除去はされない。
- **`World()` はキャッシュなし**（Phase 1）。深い階層で毎フレーム何度も呼ぶとコストがかさむ。ループ内で使うなら一度ローカル変数に取る。
- **2D 回転はラジアン**。`Rotation2D()` / `SetRotation2D()` は度ではない。
- **`AddComponent` の戻り値を捨てない**: 後でいじるコンポーネントは戻り値の参照（またはアドレス）を控える。`GetComponent<T>()` で取り直すこともできるが、最初の一致しか返さない線形探索。
- **コンポーネントは owner が単独所有**: `ANode` は各 `AComponent` を `TUniquePtr` で保持する。参照や生ポインタは寿命を延ばさず、`RemoveComponent` または owner 破棄後は無効。
- **`Reparent` の制約**: `new_parent == nullptr`、自分自身、または自分の子孫を指定すると **cycle 検出で警告ログ + 無視**。`OnSpawn/OnDespawn` は呼ばれない（生きたまま移動）。
- **`ACS_GAME_COMPONENT_KIND(T)` を忘れない**: これが `Kind()` を実装し、`GetComponent<T>()` の型判定に使われる。書き忘れると純粋仮想のままコンパイルエラー。
- **同じ型を複数 attach 可**: 例えば点滅タイマーを 2 つ。`GetComponent<T>()` は**最初の一致**だけ返す。
- **`World()` の合成順**は `parent.Compose(local)`。`local` を `parent` に対して `Compose` するのではない点に注意（規約は `Transform3D.h` 冒頭コメント）。

---

## 動くサンプル

| 見たいもの | パス |
| --- | --- |
| 継承ノード + 自作コンポーネント + 親子 transform 伝播 | `acs/samples/28_HelloGameFramework/`（特に `GameplayScene.cpp`, `RotateComponent.{h,cpp}`, `RotatingNode.{h,cpp}`） |
| `AScene` 上で sprite + physics body を組む実用スターター | `acs/samples/55_HelloScene2D/Scene2DStarter.cpp` |
| エフェクト系コンポーネント（`AWater2DComponent`/`AFire2DComponent`/`ATrail2DComponent`）を `AddComponent` する例 | `acs/samples/59_HelloEffects2D/EffectsDemo.cpp` |

上記 3 サンプルはいずれも実機ビルド + スクリーンショット確認済みです。
