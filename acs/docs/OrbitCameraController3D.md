# 決定論的 Orbit Camera 3D

`COrbitCameraController3D` は、3D orbit camera の操作計算を描画、device、World、platform入力から
切り離す。`Input + State + Time -> State / View` の境界を持ち、gameplay、AI、replay、headless test が
同じ計算経路を使う。

## 境界

| 区分 | 型 | 内容 |
|---|---|---|
| Input | `FOrbitCameraInput3D` | 前後・左右・上下移動と yaw・pitch の正規化操作量 |
| State | `FOrbitCameraState3D` | target、yaw、pitch、target から eye までの距離 |
| Time | `f32 delta_seconds` | 呼び出し側が確定した固定 tick 秒 |
| Output | 更新済み state / `FOrbitCameraView3D` | eye、look-at、up の world 座標 |

controller は速度と pitch 上限だけを所有する。camera state の owner は scene、component、replay
session などの呼び出し側であり、controller は subsystem ではない。

## 更新契約

- 入力軸は有限値だけを受け付け、計算時に `[-1, 1]` へ制限する。
- 負値または非有限の時間、非有限 state、0以下の距離は拒否する。
- `TryConfigure`、`TryStep`、`TryBuildView` は失敗時に出力を変更しない。
- yaw は `[-pi, pi]` へ折り返し、pitch は設定上限へ制限する。
- 移動速度は orbit 距離へ比例する。`normalize_movement` を有効にすると斜め入力も同じ速度になる。
- 正の pitch は見下ろし、正の yaw は右回転である。左手座標系の `eye` と `look_at` を出力する。

## 固定入力との接続

```cpp
COrbitCameraController3D Controller;
COrbitCameraController3D::FOrbitCameraState3D State;

void OnFixedUpdate(f32 FixedDeltaSeconds) noexcept
{
    COrbitCameraController3D::FOrbitCameraInput3D Input{};
    Input.move_forward = Services().Input().Evaluate(FActionId("MoveForward"), Services().FixedInput()).axis;
    Input.move_right = Services().Input().Evaluate(FActionId("MoveRight"), Services().FixedInput()).axis;
    Input.look_yaw = Services().Input().Evaluate(FActionId("LookYaw"), Services().FixedInput()).axis;
    Input.look_pitch = Services().Input().Evaluate(FActionId("LookPitch"), Services().FixedInput()).axis;
    Controller.TryStep(Input, FixedDeltaSeconds, State);
}
```

入力 action 名は controller が固定しない。ゲーム側が mapping を決め、platform、AI、replay の
どの入力状態でも同じ `FOrbitCameraInput3D` へ変換する。

## 現在の利用先と範囲

`ALegacyScene3DAdapter` の自由カメラはこの controller を使う。以前の W/A/S/D/Q/E と矢印キーの
直接取得は adapter に残し、移動、角度制限、view 計算だけを共通化した。

この型は orbit 操作だけを扱う。衝突回避、camera shake、投影行列、dolly、入力 binding の所有は
別責務である。将来 gameplay camera へ接続するときも、これらを controller 内へ暗黙に追加しない。
