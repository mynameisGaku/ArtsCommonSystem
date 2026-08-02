# 入力 (FInput / FInputMap)

キーボード・マウス・ゲームパッドを読む方法は 2 層あります。

- **`acs::FInput`** … 物理キー直読みのポーリング API。手早く動かしたいとき・1 サンプルで完結するデモ向き。
- **`acs::game::FInputMap`** … 「`Jump` を押した」のような *名前付きアクション* に物理入力を束ねる層。複数キーの OR バインド、デッドゾーン等の明示的な軸補正、キーコンフィグ UI を後付けしやすい。**ゲーム本体ではこちらを推奨**。

座標系の注意: `FInput::MousePos()` は **ウィンドウのクライアント座標 (px)**。スプライト等の **ワールド座標** に変換するには `ScreenToWorld(...)` を通します（後述）。

---

## 最小例

フレーム先頭で `CInput::Update()` を 1 回呼んでから読む、が大原則です（`CApplication` / `CGame` を使う場合はフレームワークが代行するので不要 — 下の「注意点」参照）。

```cpp
#include "platform/Input.h"
using namespace acs;

void OnUpdate(f32 dt) noexcept {
    // エッジ: このフレームで押された瞬間だけ true
    if (FInput::IsKeyPressed(EKey::Escape)) Quit();

    // レベル: 押されている間ずっと true
    if (FInput::IsKeyDown(EKey::W)) MoveUp(dt);

    // マウス (クライアント座標 px)
    FVec2 m = FInput::MousePos();
    if (FInput::IsMouseButtonPressed(EMouseButton::Left)) Shoot(m);

    // ホイール (正:奥 / 負:手前)
    f32 wheel = FInput::MouseWheel();
}
```

`FInputMap` 版（アクション名で書く）:

```cpp
#include "gameframework/InputMap.h"
using namespace acs;
using namespace acs::game;

FInputMap im;
im.BindKey      (FActionId("Jump"),  EKey::Space);
im.BindGamepad  (FActionId("Jump"),  EGamepadButton::A);   // Space でも A でも OK
im.BindAxisKeys (FActionId("MoveX"), EKey::A, EKey::D);    // A=-1, D=+1

if (im.IsPressed(FActionId("Jump"))) DoJump();
const FInputAxisOptions move_options{0.15f, 1.0f, false};
f32 mvx = im.AxisValue(FActionId("MoveX"), move_options);
```

---

## 主要 API

### `acs::FInput`（物理キー直読み）

| メソッド | 説明 |
|---|---|
| `FInput::Update()` | フレーム先頭で 1 回。前フレーム状態を確定する（Pressed/Released 判定の土台） |
| `IsKeyDown(EKey)` | 押されている間 true（レベル） |
| `IsKeyPressed(EKey)` | **このフレームで押された瞬間だけ** true（立ち上がりエッジ） |
| `IsKeyReleased(EKey)` | このフレームで離された瞬間だけ true（立ち下がりエッジ） |
| `IsMouseButtonDown/Pressed/Released(EMouseButton)` | マウスボタンの Down/Pressed/Released |
| `MousePos() -> FVec2` | 現在位置（**クライアント座標 px**） |
| `MouseDelta() -> FVec2` | 前フレームからの移動量（px） |
| `MouseWheel() -> f32` | このフレームのホイール回転（正:奥 / 負:手前） |
| `TextInput() -> const char*` | このフレームの確定入力文字列（UTF-8, IME 確定後）。無ければ `""` |
| `IsGamepadConnected(u32 player)` | プレイヤー `0..3` のパッド接続有無 |
| `IsGamepadButtonDown/Pressed/Released(u32 player, EGamepadButton)` | パッドボタン（Down / Pressed / Released） |
| `GamepadAxisValue(u32 player, EGamepadAxis) -> f32` | スティック/トリガーの **生アナログ値** |

### `acs::game::FInputMap`（アクションマッピング）

| メソッド | 説明 |
|---|---|
| `BindKey(FActionId, EKey)` | アクションにキーを追加バインド |
| `BindMouseButton(FActionId, EMouseButton)` | マウスボタンをバインド |
| `BindGamepad(FActionId, EGamepadButton, u32 player=0)` | パッドボタンをバインド（player 既定 0） |
| `BindAxisKeys(FActionId, EKey neg, EKey pos)` | 1D 軸。neg=-1 / pos=+1 を返す軸を作る |
| `BindGamepadAxis(FActionId, EGamepadAxis, u32 player=0, f32 scale=1)` | アナログ軸を追加。負の `scale` で反転 |
| `Unbind(FActionId)` | 指定アクションの全バインド削除 |
| `ClearAll()` | 全バインド削除 |
| `IsPressed(FActionId)` | いずれかのバインドがこのフレーム押された（OR） |
| `IsHeld(FActionId)` | いずれかが押されている（OR） |
| `IsReleased(FActionId)` | いずれかがこのフレーム離された（OR） |
| `Axis(FActionId) -> f32` | キー軸とアナログ軸を合算 → `clamp(-1, +1)` |
| `AxisValue(FActionId, FInputAxisOptions) -> f32` | 既存の合算値へデッドゾーン、倍率、反転を後処理 |

`FActionId("Jump")` は **コンパイル時 FNV-1a ハッシュ**（`u32`）で、実行時の文字列比較は発生しません。毎フレーム `FActionId("Jump")` と書いてもコストはゼロです。

### `acs::game::FInputAxisOptions`（1D軸の後処理）

| 項目 | 既定値 | 説明 |
|---|---:|---|
| `dead_zone` | `0` | 絶対値がこの値以下なら0。`0 <= dead_zone < 1` が有効 |
| `scale` | `1` | デッドゾーン外を再正規化した後の非負倍率 |
| `inverted` | `false` | 正負を反転 |
| `Apply(f32) -> f32` | — | 入力を`[-1,+1]`へ制限して設定を適用。不正設定または非有限入力は0 |

デッドゾーン外は連続するよう0から1へ再正規化され、その後に倍率、`[-1,+1]`制限、反転が適用されます。
`FInputMap::AxisValue(action, options)`は、既存の`Axis(action)`で全バインドを合算した後に
`options.Apply(...)`を一度だけ呼びます。バインドごとの倍率を指定する既存の
`BindGamepadAxis(..., scale)`とは責務が異なり、負のバインド倍率による反転も従来どおり使えます。

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

`MousePos()` はクライアント px なので、必ず `ScreenToWorld` を通します。`AScene` 派生シーン内では基底クラスの `ScreenToWorld(...)` ヘルパが使えます（ppu = pixels-per-unit を考慮）。

```cpp
// 実サンプル 61_HelloWaterTopDown を弱参照で安全にした形
void OnTick(f32 dt) noexcept override {
    if (FInput::IsKeyPressed(EKey::Escape)) { GetGame().Quit(); return; }

    const FVec2 mw = ScreenToWorld(FInput::MousePos());   // px -> world
    if (ANode* player = m_Player.Get()) {
        player->SetPosition2D(mw);                         // マウス追従
    }

    const FVec2 md = FInput::MouseDelta();                  // なぞり速度に使う
    const bool   click = FInput::IsMouseButtonPressed(EMouseButton::Left);
}
```

`m_Player` は `TWeakObjectPtr<ANode>` とし、`NewObject<ANode>()` が返す強参照を
`Root().AddChild(Move(node))` へ渡す前に代入します。ツリーから破棄された後は `Get()` が
`nullptr` になるため、長期間保持する生ポインタより安全です。

> シーンの外（生ウィンドウ）では `CCamera2D::ScreenToWorld(screen, screen_w, screen_h)` を使います。こちらは画面サイズを明示する必要があります。

### 2. アクションマッピングで移動 + 射撃（FInputMap）

セットアップ（シーン開始時に 1 回）と消費を分けて書きます。`Services().Input()` は `AScene2D` が要求する共有 `FInputMap`。

```cpp
// 実サンプル 38_HelloFullGame: OnEnter でバインド
FInputMap& im = Services().Input();
im.ClearAll();
im.BindAxisKeys   (FActionId("MoveX"), EKey::A, EKey::D);
im.BindGamepadAxis(FActionId("MoveX"), EGamepadAxis::LeftX);
im.BindAxisKeys   (FActionId("MoveY"), EKey::W, EKey::S);   // W=画面上=-Y (Y-down: 画面上ほど小さい Y)
im.BindMouseButton(FActionId("Fire"),  EMouseButton::Left);
im.BindKey        (FActionId("Pause"), EKey::P);
im.BindKey        (FActionId("Quit"),  EKey::Escape);

// 毎フレームの消費（別関数）
const FInputMap& im = scene.Services().Input();
FVec2 move{ im.Axis(FActionId("MoveX")), im.Axis(FActionId("MoveY")) };
if (move.x != 0.0f || move.y != 0.0f) { /* 正規化して移動 */ }
if (im.IsHeld(FActionId("Fire")))  Shoot();
if (im.IsPressed(FActionId("Quit"))) GetGame().Quit();
```

### 3. トグル（押した瞬間だけ反転）

レベル系の `IsKeyDown` をそのまま使うと毎フレーム反転して暴れます。エッジ系 `IsKeyPressed` を使うか、自前で前フレーム値を保持します。

```cpp
// 実サンプル 60_HelloStencilMask より（前フレーム保持で確実にエッジ化）
const bool space = FInput::IsKeyPressed(EKey::Space);
if (space && !m_SpacePrev) m_Invert = !m_Invert;
m_SpacePrev = space;
```

### 4. ゲームパッドのアナログスティック

アクションへ統合する場合は `BindGamepadAxis` を使います。キー軸とスティック値は
`FInputMap::Axis` で合算され、最終的に `[-1,+1]` へ制限されます。ゲーム固有のデッドゾーン、
感度、反転は`FInputAxisOptions`で合算後に適用できます。

```cpp
const FInputAxisOptions look_options{0.12f, 1.4f, true};
const f32 look_x = input_map.AxisValue(FActionId("LookX"), look_options);
LookHorizontal(look_x);
```

物理デバイス値そのものを調査する場合は、従来どおり
`FInput::GamepadAxisValue(player, axis)`で生アナログ値を読めます。

---

## 注意点 (gotcha)

- **`Pressed` はそのフレームだけ**。`IsKeyPressed` / `IsMouseButtonPressed` は立ち上がりエッジ。「押されている間」が欲しいなら `IsKeyDown` / `IsHeld`。逆にトグルでエッジが欲しいのに `Down` を使うと毎フレーム反転して暴れる。
- **`MousePos()` はクライアント座標 (px)**。ワールド座標と混同しない。スプライトに当てる前に必ず `ScreenToWorld(...)` を通す。`MouseDelta()` も px 単位。
- **`CInput::Update()` の呼び出し場所**。生ウィンドウループでは毎フレーム先頭で自分で呼ぶ。`CApplication`/`CGame`（サンプル 28/38/55〜61 系）ではフレームワークが代行するので、`OnUpdate`/`OnTick` 内で **重ねて呼ばない**。
- **複数バインドは OR**。1 アクションに `BindKey`+`BindGamepad` を重ねると、どれか 1 つでも該当で `IsPressed/IsHeld/IsReleased` が true。「全部押す」AND セマンティクスは無い。
- **軸キーの相殺**。`BindAxisKeys(neg, pos)` で *両方同時押し* は `0`（相殺）。複数の軸バインドは合算後 `clamp(-1, +1)`。
- **軸補正は合算後**。`FInputAxisOptions`は各物理バインドではなく`Axis(action)`の最終値へ一度だけ適用される。不正な`dead_zone`、負または非有限の`scale`、非有限入力は安全に0を返す。
- **`FInputMap` の未実装ポイント（正直な注意）**:
  - 軸バインド (`BindAxisKeys` / `BindGamepadAxis`) に対する `IsPressed` / `IsReleased` は **常に false**（軸にエッジの概念なし）。`IsHeld` は現在値が非ゼロなら true。
  - キーボード/マウスには player 概念がなく、`BindKey` / `BindMouseButton` / `BindAxisKeys` は全プレイヤー共通。ゲームパッドだけ `player_index` で分離する。
- **`FActionId` のハッシュ衝突**。32bit FNV-1a なので理論上は衝突しうる。実用上は無視できるが、同一マップ内でアクション名の総当りを避ける程度の意識でよい。
- **`EGamepadAxis` のレンジ差**。スティックは `-1.0 .. +1.0`、トリガー（`LeftTrigger`/`RightTrigger`）は `0.0 .. 1.0`。同じ axis 値として混ぜない。

---

## 動くサンプル

| 内容 | パス |
|---|---|
| `FInput` 直読み（IsKeyPressed/Down で背景色操作） | `acs/samples/01_HelloWindow/HelloWindowApp.cpp` |
| マウス→ワールド + `MouseDelta` + 左クリック | `acs/samples/61_HelloWaterTopDown/WaterTopDownDemo.cpp` |
| 同上（`ScreenToWorld` でピッキング） | `acs/samples/59_HelloEffects2D/EffectsDemo.cpp` |
| トグル（前フレーム保持でエッジ化）+ `IsKeyDown` で連続操作 | `acs/samples/60_HelloStencilMask/StencilMaskDemo.cpp` |
| `FInputMap` 実戦（`BindAxisKeys`/`BindMouseButton`/`Axis`/`IsHeld`/`IsPressed`） | `acs/samples/38_HelloFullGame/GameplayScene.cpp`, `Player.cpp` |

ヘッダ実体: `acs/src/platform/Input.h`, `acs/src/platform/InputCodes.h`, `acs/src/gameframework/InputAxisOptions.h`, `acs/src/gameframework/InputMap.h`（実装 `InputAxisOptions.cpp`, `InputMap.cpp`）。
