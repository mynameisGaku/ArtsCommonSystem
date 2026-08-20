# 固定ステップ時計

`acs::game::FFixedStepClock` は、可変フレーム時間を固定更新回数と描画補間率へ
変換する軽量な値型です。`FGame` の実行ループ、実時間時計、callback、共有 service は
所有しません。独立した simulation、tool、test ごとに呼び出し側が値として所有します。

関連型は責務ごとに分離されています。

- `FFixedStepOptions`: step 幅、1 回の最大 step 数、蓄積時間の上限
- `FFixedStepAdvanceResult`: 実行 step 数、補間率、破棄時間、受付・clamp 状態
- `FFixedStepClockSnapshot`: 設定、剰余、累積 step 数、累積破棄時間

```cpp
acs::game::FFixedStepClock clock;
acs::game::FFixedStepOptions options{};
options.step_seconds = 1.0 / 60.0;
options.maximum_steps_per_advance = 8;
options.maximum_accumulated_seconds = 0.25;

if (clock.Configure(options)) {
    const auto result = clock.Advance(frame_seconds);
    for (acs::u32 i = 0; i < result.step_count; ++i) {
        Simulate(options.step_seconds);
    }
    Draw(result.interpolation_alpha);
}
```

## 入力契約

- `step_seconds` は有限値かつ `1.0e-9` 以上、`3600.0` 以下です。
- `maximum_steps_per_advance` は 1 以上、1,000,000 以下です。
- `maximum_accumulated_seconds` は step 幅以上、86,400 秒以下です。
- 負値、NaN、無限大の delta は拒否され、時計の状態は変化しません。
- 巨大な有限 delta は蓄積上限で clamp されます。最大 step 数を超えた時間も捨て、
  `dropped_seconds` と累積統計へ加算します。
- `Configure` と `TryRestoreSnapshot` は候補全体を先に検証し、失敗時には現在状態を
  維持します。
- snapshot の剰余は `0 <= accumulated_seconds < step_seconds` を満たします。

`Reset` は設定を維持し、剰余と累積統計だけを初期化します。共有の既定時計が必要に
なっても、この値型自体へ process 寿命や自動更新を追加せず、別の subsystem が明示的に
所有してください。

## FGame との接続

`FGame` はこの値型を所有し、時間倍率を反映した delta を `Advance` へ渡します。返された
`step_count` の回数だけ `FScene::OnFixedUpdate` を呼ぶため、独立 simulation と実ゲームで
固定更新の上限・剰余・破棄時間の契約が共通になります。

- `SetFixedTimestep(step, max)` は既存の簡易入口です。step が 0 以下、または max が 0 なら無効化します。
- `TrySetFixedTimestep(options)` は蓄積上限を含む完全な設定を検証し、失敗時は現状態を維持します。
- `TryCaptureFixedStepSnapshot` / `TryRestoreFixedStepSnapshot` で固定更新位置を保存・復元できます。
- `TryCaptureFixedStepRuntimeSnapshot` / `TryRestoreFixedStepRuntimeSnapshot` は固定時計に加え、
  active scene が次の tick へ繰り越す入力と固定更新の有効状態を一括保存・復元します。同じ
  `FGame`、active scene、入力source結線でだけ復元でき、境界不一致では現状態を維持します。
- `SetFixedStepInputSource`は描画フレーム入力の取得元をAI、headless testへ差し替えます。
- `SetFixedTickInputSource`は固定tick入力の取得元をreplay、rollbackへ差し替えます。catch-upでも
  tickごとに呼ばれ、時計snapshot復元後は同じtick番号を再要求します。
- `FInputRecorderFixedTickSource`はraw key code decoderを受け取り、既存`.acsr`のsample列を
  cursor非変更で固定tick入力へ変換します。
- `ResetFixedStepInputSource`でどちらの差し替え元からも既定のplatform入力へ戻ります。
- `FixedStepInterpolationAlpha` は描画補間に使える剰余率を返します。
- 起動前に復元した時計状態は `FGame::OnStart` 後も維持されます。

時計 snapshot が扱うのは固定更新時計だけです。完全な replay や rollback では、シーンや
ゲーム状態の snapshot も同じフレーム境界で保存・復元してください。通常の `FGame` では
`FFixedStepRuntimeSnapshot` を使うと時計と未消費入力の境界ずれを防げます。
これはprocess内のrollback値であり、永続化や別`FGame`への移送を行う形式ではありません。
