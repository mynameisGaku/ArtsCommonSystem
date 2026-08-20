# 入力 (FInput / FInputMap)

キーボード・マウス・ゲームパッドを読む方法は 2 層あります。

- **`acs::FInput`** … 物理キー直読みのポーリング API。手早く動かしたいとき・1 サンプルで完結するデモ向き。
- **`acs::game::FInputMap`** … 「`Jump` を押した」のような *名前付きアクション* に物理入力を束ねる層。複数キーの OR バインドやキーコンフィグ UI を後付けしやすい。**ゲーム本体ではこちらを推奨**。

座標系の注意: `FInput::MousePos()` は **ウィンドウのクライアント座標 (px)**。スプライト等の **ワールド座標** に変換するには `ScreenToWorld(...)` を通します（後述）。

---

## 最小例

フレーム先頭で `FInput::Update()` を 1 回呼んでから読む、が大原則です（`FApplication` / `FGame` を使う場合はフレームワークが代行するので不要 — 下の「注意点」参照）。

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
f32 mvx = im.Axis(FActionId("MoveX"));   // -1 / 0 / +1
```

固定更新、replay、headless test では、対象時点の入力を明示して評価できます。

```cpp
#include "gameframework/InputStateSnapshot.h"

FInputStateSnapshot input;
input.TrySetKeyState(EKey::Space, true, true, false);

const FInputActionState jump = im.Evaluate(FActionId("Jump"), input);
if (jump.pressed) DoJump();
```

`FGame` のシーンでは `ESvc::Input` を要求すると固定入力が自動配線されます。可変フレームで
短く押して離した入力も次の固定 tick まで保持され、catch-up の二回目以降には
Pressed/Released を再送しません。

```cpp
class FGameplayScene final : public FScene {
public:
    ESvc WantedServices() const noexcept override { return ESvc::Input; }

    void OnEnter() noexcept override {
        Services().Input().BindKey(FActionId("Jump"), EKey::Space);
    }

    void OnFixedUpdate(f32 fixed_dt) noexcept override {
        const FInputActionState jump = Services().Input().Evaluate(FActionId("Jump"), Services().FixedInput());
        SimulatePlayer(fixed_dt, jump);
    }
};
```

rollback では `FFixedStepRuntimeSnapshot` を使うと、固定時計、active scene の未消費入力、
固定更新の有効状態を同じ境界で保存・復元できます。シーン本体や乱数の状態も別途同じ
境界で保存してください。

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

`FActionId("Jump")` は **コンパイル時 FNV-1a ハッシュ**（`u32`）で、実行時の文字列比較は発生しません。毎フレーム `FActionId("Jump")` と書いてもコストはゼロです。

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

`MousePos()` はクライアント px なので、必ず `ScreenToWorld` を通します。`FScene2D` 派生シーン内では基底クラスの `ScreenToWorld(...)` ヘルパが使えます（ppu = pixels-per-unit を考慮）。

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

> シーンの外（生ウィンドウ）では `FCamera2D::ScreenToWorld(screen, screen_w, screen_h)` を使います。こちらは画面サイズを明示する必要があります。

### 2. アクションマッピングで移動 + 射撃（FInputMap）

セットアップ（シーン開始時に 1 回）と消費を分けて書きます。`Services().Input()` は `FScene2D` が持つ共有 `FInputMap`。

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
`FInputMap::Axis` で合算され、最終的に `[-1,+1]` へ clamp されます。デッドゾーン等を
独自処理したい場合だけ `FInput` の生アナログ値を直接読みます。

```cpp
if (FInput::IsGamepadConnected(0)) {
    f32 lx = FInput::GamepadAxisValue(0, EGamepadAxis::LeftX);   // -1.0 .. +1.0
    f32 ly = FInput::GamepadAxisValue(0, EGamepadAxis::LeftY);
    f32 rt = FInput::GamepadAxisValue(0, EGamepadAxis::RightTrigger); // 0.0 .. 1.0
    // 必要ならデッドゾーン処理を自前で
    if (lx*lx + ly*ly > 0.04f) Move(lx, ly);
}
```

---

## 注意点 (gotcha)

- **`Pressed` はそのフレームだけ**。`IsKeyPressed` / `IsMouseButtonPressed` は立ち上がりエッジ。「押されている間」が欲しいなら `IsKeyDown` / `IsHeld`。逆にトグルでエッジが欲しいのに `Down` を使うと毎フレーム反転して暴れる。
- **`MousePos()` はクライアント座標 (px)**。ワールド座標と混同しない。スプライトに当てる前に必ず `ScreenToWorld(...)` を通す。`MouseDelta()` も px 単位。
- **`FInput::Update()` の呼び出し場所**。生ウィンドウループでは毎フレーム先頭で自分で呼ぶ。`FApplication`/`FGame`（サンプル 28/38/55〜61 系）ではフレームワークが代行するので、`OnUpdate`/`OnTick` 内で **重ねて呼ばない**。
- **複数バインドは OR**。1 アクションに `BindKey`+`BindGamepad` を重ねると、どれか 1 つでも該当で `IsPressed/IsHeld/IsReleased` が true。「全部押す」AND セマンティクスは無い。
- **軸キーの相殺**。`BindAxisKeys(neg, pos)` で *両方同時押し* は `0`（相殺）。複数の軸バインドは合算後 `clamp(-1, +1)`。
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

ヘッダ実体: `acs/src/platform/Input.h`, `acs/src/platform/InputCodes.h`, `acs/src/gameframework/InputMap.h`（実装 `InputMap.cpp`）。
