<!-- SPDX-License-Identifier: Apache-2.0 -->
# 固定更新ランタイム入力

`GameFramework` は、可変な描画 frame の入力を固定 tick のゲームロジックへ渡し、同じ入力列と
時間状態から同じ action 評価を再現できる境界を提供する。3D の移動・姿勢・視点操作を主用途とするが、
入力値は座標系を持たないため 2D でも同じ仕組みを使える。

## 境界とデータ

- `IInputStateView`: キー、マウスボタン、ゲームパッドの物理状態を読む境界。
- `FInputStateSnapshot`: 一入力時点を所有し、device や `CInput` なしで再評価できる値。
- `FInputActionState`: 一 action の `pressed`、`held`、`released`、`axis` をまとめた結果。
- `FInputMap::Evaluate`: binding と明示した `IInputStateView` だけから action を評価する処理。
- `FFixedStepInputBuffer`: frame 間の保持状態、軸、未消費の押下・解放を所有する値。

依存方向は次の通りである。

```text
Platform / Player / AI / Replay
              ↓
       Input source adapter
              ↓
      FInputStateSnapshot
              ↓
   FFixedStepInputBuffer + Time
              ↓
       FInputMap::Evaluate
              ↓
       3D simulation input
```

`FInputMap` の従来の `IsPressed`、`IsHeld`、`IsReleased`、`Axis` は platform 入力を直接読む
互換 API として残す。再生、AI、headless test、rollback では `Evaluate` を使い、隠れた device 状態を
ゲームロジックへ持ち込まない。

## frame 入力と固定 tick 入力

`CGame` は入力 source を非所有参照として一つだけ接続する。

- 既定: `CPlatformInputStateAdapter` が `CInput` を描画 frame ごとに一度取得する。
- `IInputFrameSource`: Player、AI、headless test が作る frame snapshot を固定 tick まで蓄積する。
- `IFixedTickInputSource`: replay、rollback、network lockstep が固定 tick 番号ごとの snapshot を返す。

frame source と fixed-tick source は排他的である。切替時は以前の source から残った入力を破棄する。
frame source では固定更新がない間の押下・解放を論理和で保持し、次の固定 tick で一度だけ消費する。
一 frame に複数 tick を実行しても、保持状態と軸は継続するが同じ押下・解放は再送しない。

`CInputRecorderFixedTickSource` は `CInputRecorder` の raw key と mouse button の sample を
`IFixedTickInputSource` へ変換する。raw key code の意味は platform ごとに異なるため、呼び出し側が
`FInputSampleKeyDecoder` を渡す。過去 tick を要求した場合は先頭から状態を再構築する。

## scene からの利用

scene は `WantedServices()` で `ESvc::Input` を要求する。固定更新中は次のように、入力取得と
3D シミュレーションを分離する。

```cpp
const FInputActionState forward = Services().Input().Evaluate(FActionId("MoveForward"), Services().FixedInput());
const FInputActionState yaw = Services().Input().Evaluate(FActionId("LookYaw"), Services().FixedInput());

flight_state = SimulateFlight(flight_state, forward.axis, yaw.axis, fixed_delta_seconds);
```

`FixedInput()` は現在の固定 tick 中だけ安定した snapshot を返す。入力 map は物理入力を action へ
変換するだけで、3D transform、camera、physics、Actor を所有しない。Player と AI は同じ action と
シミュレーション契約を共有できる。

## 保存と復元

`CGame::TryCaptureFixedStepRuntimeSnapshot` は次を一つの process 内保存値へ複製する。

- `CFixedStepClock` の設定、持ち越し時間、累積 tick 数。
- active scene の未消費入力。
- 固定更新の有効状態。
- 取得元 game、active scene、入力 source の識別値。

復元は同じ `CGame`、同じ active scene、同じ入力 source 結線の場合だけ成功する。scene 遷移後、
source 切替後、別 game への復元は拒否し、時計と入力を変更しない。この snapshot は rollback と
同一 process 内の検証用であり、version 間の永続保存形式ではない。

## 失敗と所有権

snapshot の setter、buffer、source adapter は範囲外 enum、player 番号、非有限または範囲外の軸値、
不正な recorder sample を拒否する。検証は一時値で完了してから反映し、失敗時は出力と既存状態を保つ。
`CGame` で入力取得に失敗した tick は無入力として進め、以前の入力を誤って再利用しない。

入力 source の寿命と同時実行制御は接続する側が所有する。`CGame`、source、snapshot buffer は内部同期を
持たず、一つのゲームループ thread から操作する。

内部駆動処理は private の `Foo_Internal` とし、`CSceneManager::FExecutionAdapter` と
`CSceneServices::FUpdateAdapter` だけが明示的に公開する。`friend` による無制限な内部アクセスは使わない。

## 検証

`ACS.FixedRuntimeInput` は device、window、renderer、Actor を生成せず、次を固定する。

- 3D 操作用の `MoveForward` と `LookYaw` の明示 snapshot 評価。
- 固定更新がない frame をまたぐ短い押下と解放。
- catch-up 中の edge 一回消費と固定 tick 順序。
- clock と未消費入力の rollback、および game・scene・source 境界の拒否。
- recorder の順次再生、巻き戻し、不正 sample の失敗時不変。
