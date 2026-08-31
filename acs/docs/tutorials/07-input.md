# 07 入力

`FInputMap` はキー、マウスボタン、ゲームパッドボタン、入力軸を `FActionId` へ割り当てます。

```cpp
inline constexpr acs::game::FActionId kMoveX{"MoveX"};
inline constexpr acs::game::FActionId kQuit{"Quit"};

acs::game::FInputMap input_map;
input_map.BindAxisKeys(kMoveX, acs::EKey::A, acs::EKey::D);
input_map.BindKey(kQuit, acs::EKey::Escape);
```

明示的な入力ビューを使う場合は `FInputActionState state = input_map.Evaluate(action, input)` と評価し、`state.pressed`、`state.held`、`state.released`、`state.axis` を読みます。現在のプラットフォーム入力を直接評価する互換APIとして `IsPressed`、`IsHeld`、`IsReleased`、`Axis` もあります。`FActionId` はコンパイル時定数として定義すると、同じ名前から安定したIDを利用できます。

`FScene2D::OnFixedTick` では `Services().FixedInput()` を `Evaluate` へ渡します。内部の `FFixedStepInputBuffer` は、表示フレームの押下エッジを固定ステップへ重複配送せず、0ステップのフレームでは次の適用機会まで保持します。

リプレイやロールバックでは `FFixedStepRuntimeSnapshot` を保存します。この型は固定時計の `clock` と、未消費入力を含む `input` を同じ保存値に持ちます。

[次章: 衝突、物理、トリガー](08-collision-physics-triggers.md)
