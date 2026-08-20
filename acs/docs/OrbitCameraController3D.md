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
| Obstruction | `obstruction_distance` / `camera_clearance` | scene queryが返したtargetからのhit距離とcamera余白 |
| Snapshot | `FOrbitCameraFixedStepSnapshot3D` | 補間区間を再現するprevious/current固定tick状態 |
| Output | 更新済み state / `FOrbitCameraView3D` | eye、look-at、up の world 座標 |

controller は速度と pitch 上限だけを所有する。camera state の owner は scene、component、replay
session などの呼び出し側であり、controller は subsystem ではない。

## 更新契約

- 入力軸は有限値だけを受け付け、計算時に `[-1, 1]` へ制限する。
- 負値または非有限の時間、非有限 state、0以下の距離は拒否する。
- `TryConfigure`、`TryStep`、`TryBuildView` は失敗時に出力を変更しない。
- `TryInterpolateState` は両状態と `[0,1]` の有限な補間率だけを受け付け、失敗時に出力を変更しない。
- `IsSnapshotValid` はsnapshotのprevious/currentを現在設定に対して同時検証する。
- `TryResolveObstructedState` はdesired stateを変更せず、障害物の手前へ短縮したpresentation stateを返す。
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

## 保存と復元

`ALegacyScene3DAdapter::TryCaptureOrbitCameraSnapshot` は自由cameraのprevious/currentを
`FOrbitCameraFixedStepSnapshot3D` へ複製する。復元時は先に
`CGame::TryRestoreFixedStepRuntimeSnapshot` で固定時計と未消費入力を戻し、その後
`TryRestoreOrbitCameraSnapshot` でcamera区間を戻す。次のupdateまたはrenderで保存時の時計alphaから
presentation stateを再計算するため、派生値はsnapshotへ重複保存しない。

snapshotのいずれかが非有限、pitch上限外、距離範囲外なら復元を拒否し、previous/current、表示viewを
変更しない。これは同一process内のrollback用であり、version間の永続保存形式ではない。

## 3D障害物回避

`ALegacyScene3DAdapter::FOrbitCameraObstructionSettings3D` は既定で無効であり、既存sceneの見え方を
変更しない。`TrySetOrbitCameraObstructionSettings` で有効にすると、adapterはtargetからdesired eyeへ
正規化rayを飛ばし、`CSceneNodeGraph::RaycastActiveRange` で有効かつ可視なmeshだけを検索する。

- `TargetClearance` より手前のhitは追従対象自身として除外する。
- 最初のhitから `CameraClearance` を引いた距離を `TryResolveObstructedState` へ渡す。
- `TargetClearance - CameraClearance` がcontrollerの最小距離を下回る設定は、障害物の奥へ丸めないよう拒否する。
- 衝突で短縮するのはpresentation stateだけで、fixed tickのdesired distanceとrollback snapshotは変えない。
- 障害物が無効または非表示になると、次のupdate/renderでdesired distanceへ戻る。
- queryはnodeのworld変形を反映したmesh AABBを使う。mesh三角形やsphere sweep、復帰smoothingは別責務である。

## 現在の利用先と範囲

`ALegacyScene3DAdapter` は `ESvc::Input` を要求し、W/A/S/D/Q/E、矢印キー、PageUp/PageDown を
scene-local actionへ割り当てる。自由カメラは `OnFixedUpdate` で `Services().FixedInput()` を評価するため、
描画frameの分割に依存せず、platform、AI、replay、headless testの入力sourceを同じ経路で使える。
描画時は一つ前と現在の固定tick状態を `CGame::FixedStepInterpolationAlpha()` で補間し、viewと正射影距離へ
同じpresentation stateを適用する。simulation state自体は固定tick外で変更しない。
移動・回転・zoomは `CGame` の固定timestepを無効にすると停止する。Escapeによる終了だけは固定更新の
有無に関係なく反応させるため、可変frame側に残す。

controllerはscene queryを所有せず、明示されたhit距離からpresentation stateを計算するだけである。
camera shake、投影行列、dolly、入力 bindingの所有も別責務とし、controllerへ暗黙に追加しない。
