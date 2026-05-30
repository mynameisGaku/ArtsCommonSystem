# 入力 (Input / FInputMap)

キーボード・マウス・ゲームパッドを読む方法は 2 層あります。

- **`acs::Input`** … 物理キー直読みのポーリング API。手早く動かしたいとき・1 サンプルで完結するデモ向き。
- **`acs::game::FInputMap`** … 「`Jump` を押した」のような *名前付きアクション* に物理入力を束ねる層。複数キーの OR バインドやキーコンフィグ UI を後付けしやすい。**ゲーム本体ではこちらを推奨**。

座標系の注意: `Input::MousePos()` は **ウィンドウのクライアント座標 (px)**。スプライト等の **ワールド座標** に変換するには `ScreenToWorld(...)` を通します（後述）。

---

## 最小例

フレーム先頭で `Input::Update()` を 1 回呼んでから読む、が大原則です（`FApplication` / `FGame` を使う場合はフレームワークが代行するので不要 — 下の「注意点」参照）。

```cpp
#include "platform/Input.h"
using namespace acs;

void OnUpdate(f32 dt) noexcept {
    // エッジ: このフレームで押された瞬間だけ true
    if (Input::IsKeyPressed(EKey::Escape)) Quit();

    // レベル: 押されている間ずっと true
    if (Input::IsKeyDown(EKey::W)) MoveUp(dt);

    // マウス (クライアント座標 px)
    FVec2 m = Input::MousePos();
    if (Input::IsMouseButtonPressed(EMouseButton::Left)) Shoot(m);

    // ホイール (正:奥 / 負:手前)
    f32 wheel = Input::MouseWheel();
}
```

`FInputMap` 版（アクション名で書く）:

```cpp
#include "gameframework/InputMap.h"
using namespace acs;
using namespace acs::game;

FInputMap im;
im.BindKey      (ActionId("Jump"),  EKey::Space);
im.BindGamepad  (ActionId("Jump"),  EGamepadButton::A);   // Space でも A でも OK
im.BindAxisKeys (ActionId("MoveX"), EKey::A, EKey::D);    // A=-1, D=+1

if (im.IsPressed(ActionId("Jump"))) DoJump();
f32 mvx = im.Axis(ActionId("MoveX"));   // -1 / 0 / +1
```

---

## 主要 API

### `acs::Input`（物理キー直読み）

| メソッド | 説明 |
|---|---|
| `Input::Update()` | フレーム先頭で 1 回。前フレーム状態を確定する（Pressed/Released 判定の土台） |
| `IsKeyDown(EKey)` | 押されている間 true（レベル） |
| `IsKeyPressed(EKey)` | **このフレームで押された瞬間だけ** true（立ち上がりエッジ） |
| `IsKeyReleased(EKey)` | このフレームで離された瞬間だけ true（立ち下がりエッジ） |
| `IsMouseButtonDown/Pressed/Released(EMouseButton)` | マウスボタンの Down/Pressed/Released |
| `MousePos() -> FVec2` | 現在位置（**クライアント座標 px**） |
| `MouseDelta() -> FVec2` | 前フレームからの移動量（px） |
| `MouseWheel() -> f32` | このフレームのホイール回転（正:奥 / 負:手前） |
| `TextInput() -> const char*` | このフレームの確定入力文字列（UTF-8, IME 確定後）。無ければ `""` |
| `IsGamepadConnected(u32 player)` | プレイヤー `0..3` のパッド接続有無 |
| `IsGamepadButtonDown/Pressed(u32 player, EGamepadButton)` | パッドボタン（Down / Pressed） |
| `GamepadAxisValue(u32 player, EGamepadAxis) -> f32` | スティック/トリガーの **生アナログ値** |

> 注: `Input` にゲームパッドの *Released* は存在しません（Down / Pressed のみ）。

### `acs::game::FInputMap`（アクションマッピング）

| メソッド | 説明 |
|---|---|
| `BindKey(ActionId, EKey)` | アクションにキーを追加バインド |
| `BindMouseButton(ActionId, EMouseButton)` | マウスボタンをバインド |
| `BindGamepad(ActionId, EGamepadButton, u32 player=0)` | パッドボタンをバインド（player 既定 0） |
| `BindAxisKeys(ActionId, EKey neg, EKey pos)` | 1D 軸。neg=-1 / pos=+1 を返す軸を作る |
| `Unbind(ActionId)` | 指定アクションの全バインド削除 |
| `ClearAll()` | 全バインド削除 |
| `IsPressed(ActionId)` | いずれかのバインドがこのフレーム押された（OR） |
| `IsHeld(ActionId)` | いずれかが押されている（OR） |
| `IsReleased(ActionId)` | いずれかがこのフレーム離された（OR） |
| `Axis(ActionId) -> f32` | 軸バインドの合算 → `clamp(-1, +1)` |

`ActionId("Jump")` は **コンパイル時 FNV-1a ハッシュ**（`u32`）で、実行時の文字列比較は発生しません。毎フレーム `ActionId("Jump")` と書いてもコストはゼロです。

### 主要 enum（`platform/InputCodes.h`）

```cpp
enum class EKey : u16 {
    A..Z, Num0..Num9, F1..F12,
    LeftShift, RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt, LeftSuper, RightSuper,
    Up, Down, Left, Right,
    Space, Enter, Tab, Backspace, Escape, Insert, Delete, Home, End, PageUp, PageDown,
    Minus, Equal, Comma, Period, Slash, Semicolon, /* ... */
    KP0..KP9, KPAdd, KPEnter, /* ... */
};
enum class EMouseButton : u8 { Left=0, Right=1, Middle=2, X1=3, X2=4 };
enum class EGamepadButton : u8 {
    A, B, X, Y, Up, Down, Left, Right,       // D-Pad
    LeftBumper, RightBumper, LeftStick, RightStick, Start, Back, Guide
};
enum class EGamepadAxis : u8 {
    LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger   // スティック -1..+1, トリガー 0..1
};
```

---

## よく使うパターン

### 1. マウスでワールド座標を拾う（ピッキング）

`MousePos()` はクライアント px なので、必ず `ScreenToWorld` を通します。`FScene2D`/`FNode2D` のシーン内では基底クラスの `ScreenToWorld(...)` ヘルパが使えます（ppu = pixels-per-unit を考慮）。

```cpp
// 実サンプル 61_HelloWaterTopDown より
void OnTick(f32 dt) noexcept override {
    if (Input::IsKeyPressed(EKey::Escape)) { GetGame().Quit(); return; }

    const FVec2 mw = ScreenToWorld(Input::MousePos());   // px -> world
    if (m_Player) m_Player->Local().position = mw;        // マウス追従

    const FVec2 md = Input::MouseDelta();                  // なぞり速度に使う
    const bool   click = Input::IsMouseButtonPressed(EMouseButton::Left);
}
```

> シーンの外（生ウィンドウ）では `Camera2D::ScreenToWorld(screen, screen_w, screen_h)` を使います。こちらは画面サイズを明示する必要があります。

### 2. アクションマッピングで移動 + 射撃（FInputMap）

セットアップ（シーン開始時に 1 回）と消費を分けて書きます。`Services().Input()` は `FScene2D` が持つ共有 `FInputMap`。

```cpp
// 実サンプル 38_HelloFullGame: OnEnter でバインド
FInputMap& im = Services().Input();
im.ClearAll();
im.BindAxisKeys   (ActionId("MoveX"), EKey::A, EKey::D);
im.BindAxisKeys   (ActionId("MoveY"), EKey::S, EKey::W);   // 上向き = +Y
im.BindMouseButton(ActionId("Fire"),  EMouseButton::Left);
im.BindKey        (ActionId("Pause"), EKey::P);
im.BindKey        (ActionId("Quit"),  EKey::Escape);

// 毎フレームの消費（別関数）
const FInputMap& im = scene.Services().Input();
FVec2 move{ im.Axis(ActionId("MoveX")), im.Axis(ActionId("MoveY")) };
if (move.x != 0.0f || move.y != 0.0f) { /* 正規化して移動 */ }
if (im.IsHeld(ActionId("Fire")))  Shoot();
if (im.IsPressed(ActionId("Quit"))) GetGame().Quit();
```

### 3. トグル（押した瞬間だけ反転）

レベル系の `IsKeyDown` をそのまま使うと毎フレーム反転して暴れます。エッジ系 `IsKeyPressed` を使うか、自前で前フレーム値を保持します。

```cpp
// 実サンプル 60_HelloStencilMask より（前フレーム保持で確実にエッジ化）
const bool space = Input::IsKeyPressed(EKey::Space);
if (space && !m_SpacePrev) m_Invert = !m_Invert;
m_SpacePrev = space;
```

### 4. ゲームパッドのアナログスティック

`FInputMap::Axis` は **キー入力専用**（`-1/0/+1` のデジタル）。スティックの生アナログ値が欲しいときは `Input` を直接読みます。

```cpp
if (Input::IsGamepadConnected(0)) {
    f32 lx = Input::GamepadAxisValue(0, EGamepadAxis::LeftX);   // -1.0 .. +1.0
    f32 ly = Input::GamepadAxisValue(0, EGamepadAxis::LeftY);
    f32 rt = Input::GamepadAxisValue(0, EGamepadAxis::RightTrigger); // 0.0 .. 1.0
    // 必要ならデッドゾーン処理を自前で
    if (lx*lx + ly*ly > 0.04f) Move(lx, ly);
}
```

---

## 注意点 (gotcha)

- **`Pressed` はそのフレームだけ**。`IsKeyPressed` / `IsMouseButtonPressed` は立ち上がりエッジ。「押されている間」が欲しいなら `IsKeyDown` / `IsHeld`。逆にトグルでエッジが欲しいのに `Down` を使うと毎フレーム反転して暴れる。
- **`MousePos()` はクライアント座標 (px)**。ワールド座標と混同しない。スプライトに当てる前に必ず `ScreenToWorld(...)` を通す。`MouseDelta()` も px 単位。
- **`Input::Update()` の呼び出し場所**。生ウィンドウループでは毎フレーム先頭で自分で呼ぶ。`FApplication`/`FGame`（サンプル 28/38/55〜61 系）ではフレームワークが代行するので、`OnUpdate`/`OnTick` 内で **重ねて呼ばない**。
- **複数バインドは OR**。1 アクションに `BindKey`+`BindGamepad` を重ねると、どれか 1 つでも該当で `IsPressed/IsHeld/IsReleased` が true。「全部押す」AND セマンティクスは無い。
- **軸キーの相殺**。`BindAxisKeys(neg, pos)` で *両方同時押し* は `0`（相殺）。複数の軸バインドは合算後 `clamp(-1, +1)`。
- **`FInputMap` の未実装ポイント（正直な注意）**:
  - 軸バインド (`BindAxisKeys`) に対する `IsPressed` / `IsReleased` は **常に false**（軸にエッジの概念なし）。`IsHeld` だけは「neg か pos が押下」で true。
  - ゲームパッドバインドに対する `IsReleased` は **現状未対応で常に false**（`Input` 側に Gamepad Released が無いため）。離し判定が要るならキーバインドを併用するか `IsHeld` の前フレーム差分を自前で取る。
  - `BindGamepad` の `player_index` は受け取るが、`BindKey`/`BindMouseButton`/`BindAxisKeys` は player 0 固定（マルチプレイヤーの完全分離は未対応）。
- **`ActionId` のハッシュ衝突**。32bit FNV-1a なので理論上は衝突しうる。実用上は無視できるが、同一マップ内でアクション名の総当りを避ける程度の意識でよい。
- **`EGamepadAxis` のレンジ差**。スティックは `-1.0 .. +1.0`、トリガー（`LeftTrigger`/`RightTrigger`）は `0.0 .. 1.0`。同じ axis 値として混ぜない。

---

## 動くサンプル

| 内容 | パス |
|---|---|
| `Input` 直読み（IsKeyPressed/Down で背景色操作） | `acs/samples/01_HelloWindow/HelloWindowApp.cpp` |
| マウス→ワールド + `MouseDelta` + 左クリック | `acs/samples/61_HelloWaterTopDown/WaterTopDownDemo.cpp` |
| 同上（`ScreenToWorld` でピッキング） | `acs/samples/59_HelloEffects2D/EffectsDemo.cpp` |
| トグル（前フレーム保持でエッジ化）+ `IsKeyDown` で連続操作 | `acs/samples/60_HelloStencilMask/StencilMaskDemo.cpp` |
| `FInputMap` 実戦（`BindAxisKeys`/`BindMouseButton`/`Axis`/`IsHeld`/`IsPressed`） | `acs/samples/38_HelloFullGame/GameplayScene.cpp`, `Player.cpp` |

ヘッダ実体: `acs/src/platform/Input.h`, `acs/src/platform/InputCodes.h`, `acs/src/gameframework/InputMap.h`（実装 `InputMap.cpp`）。
