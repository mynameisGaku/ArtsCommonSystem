# 固定tick入力buffer

`acs::game::FFixedStepInputBuffer`は、可変フレームで取得した入力を固定tickへ渡す値型です。
保持状態と軸は最新値へ更新し、押下・解放は固定更新が来るまで蓄積します。catch-up時は
最初のtickだけが押下・解放を受け取り、二回目以降は保持状態と軸だけを受け取ります。

## snapshot

`TryCaptureSnapshot`は、次の固定tickを待つ入力と初期化状態を
`FFixedStepInputBufferSnapshot`へ保存します。`TryRestoreSnapshot`は保存値を一時bufferで
検証してから反映するため、失敗時に現在状態を変更しません。

```cpp
acs::game::FFixedStepInputBuffer buffer;
acs::game::FFixedStepInputBufferSnapshot saved;

buffer.TryPushFrame(frame_input);
buffer.TryCaptureSnapshot(saved);

// replayやrollbackで時計・simulation状態と同じ境界へ戻す。
buffer.TryRestoreSnapshot(saved);
```

`has_input_state`が`false`の保存値を復元すると、`pending_input`の内容は無視され、bufferは
未初期化状態へ戻ります。再度保存した値ではpayloadも無入力へ正規化されます。

固定時計の`FFixedStepClockSnapshot`、simulation、乱数、シーン状態は別の保存値です。
完全なreplayやrollbackでは、これらを同じフレーム境界でまとめて保存・復元してください。
`FGame`配下では`FFixedStepRuntimeSnapshot`と
`TryCaptureFixedStepRuntimeSnapshot` / `TryRestoreFixedStepRuntimeSnapshot`を使うと、
固定時計とactive sceneの未消費入力を一つのtransactionとして扱えます。

## FGame の入力ソース

`FGame` は既定で現在の `FInput` を一フレームに一度だけ取得します。replay、AI、headless test
では `IInputFrameSource` を実装し、`SetFixedStepInputSource` へ渡すと platform 入力を使わずに
所有 snapshot を提出できます。ソースは非所有なので、`ResetFixedStepInputSource` または
`FGame` の破棄まで呼び出し側が生存させてください。

入力ソースを切り替えると、以前のソースから残った未消費入力は破棄されます。取得が
`false` を返したフレームも同様に未消費入力を破棄し、その固定 tick は無入力になります。
