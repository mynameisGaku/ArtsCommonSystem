# 決定論的 Orbit Camera 3D

`COrbitCameraController3D` は、3D orbit camera の操作計算を描画、device、World、platform入力から
切り離す。`Input + State + Time -> State / View` の境界を持ち、gameplay、AI、replay、headless test が
同じ計算経路を使う。

## 境界

| 区分 | 型 | 内容 |
|---|---|---|
| Input | `FOrbitCameraInput3D` | 前後・左右・上下移動、yaw・pitch、距離zoomの正規化操作量 |
| State | `FOrbitCameraState3D` | target、yaw、pitch、target から eye までの距離 |
| Time | `f32 delta_seconds` | 呼び出し側が確定した固定 tick 秒 |
| Presentation | `f64 interpolation_alpha` | 前回と現在の固定 tick 状態を混ぜる `[0,1]` の描画補間率 |
| Output | 更新済み state / `FOrbitCameraView3D` | eye、look-at、up の world 座標 |

controller は速度と pitch 上限だけを所有する。camera state の owner は scene、component、replay
session などの呼び出し側であり、controller は subsystem ではない。

## 更新契約

- 入力軸は有限値だけを受け付け、計算時に `[-1, 1]` へ制限する。
- 負値または非有限の時間、非有限 state、0以下の距離は拒否する。
- `TryConfigure`、`TryStep`、`TryBuildView` は失敗時に出力を変更しない。
- `TryInterpolateState` は両状態と `[0,1]` の有限な補間率だけを受け付け、失敗時に出力を変更しない。
- yaw 補間は `-pi` / `+pi` 境界を最短経路で越え、target、pitch、distance は線形補間する。
- 補間率0はprevious、1はcurrentを表し、通常描画は一つ前の固定tickから現在tickまでを連続表示する。
- yaw は `[-pi, pi]` へ折り返し、pitch は設定上限へ制限する。
- 正の zoom は target へ近づき、負の zoom は遠ざかる。距離は設定した最小・最大値へ制限する。
- 移動速度は orbit 距離へ比例する。`normalize_movement` を有効にすると斜め入力も同じ速度になる。
- 正の pitch は見下ろし、正の yaw は右回転である。左手座標系の `eye` と `look_at` を出力する。

## 固定入力との接続

```cpp
COrbitCameraController3D Controller;
COrbitCameraController3D::FOrbitCameraState3D State;
FOrbitCameraInputActionSet3D CameraActions;

void OnFixedUpdate(f32 FixedDeltaSeconds) noexcept
{
    COrbitCameraController3D::FOrbitCameraInput3D Input{};
    if (!CameraActions.TryEvaluate(Services().Input(), Services().FixedInput(), Input)) return;
    Controller.TryStep(Input, FixedDeltaSeconds, State);
}
```

`FOrbitCameraInputActionSet3D` の既定 action 名は `MoveForward`、`MoveRight`、`MoveUp`、`LookYaw`、
`LookPitch`、`Zoom` である。ゲーム側は各 `FActionId` を置き換えられる。6 action は有効かつ互いに異なる
必要があり、不正な集合では出力を変更しない。platform、AI、replay のどの入力状態でも同じ
`FOrbitCameraInput3D` へ変換できる。

## 現在の利用先と範囲

`ALegacyScene3DAdapter` は `ESvc::Input` を要求し、W/A/S/D/Q/E、矢印キー、PageUp/PageDown を
scene-local actionへ割り当てる。自由カメラは `OnFixedUpdate` で `Services().FixedInput()` を評価するため、
描画frameの分割に依存せず、platform、AI、replay、headless testの入力sourceを同じ経路で使える。
描画時は一つ前と現在の固定tick状態を `CGame::FixedStepInterpolationAlpha()` で補間し、viewと正射影距離へ
同じpresentation stateを適用する。simulation state自体は固定tick外で変更しない。
移動・回転・zoomは `CGame` の固定timestepを無効にすると停止する。Escapeによる終了だけは固定更新の
有無に関係なく反応させるため、可変frame側に残す。

この型は orbit 操作だけを扱う。衝突回避、camera shake、投影行列、dolly、入力 binding の所有は
別責務である。将来 gameplay camera へ接続するときも、これらを controller 内へ暗黙に追加しない。
