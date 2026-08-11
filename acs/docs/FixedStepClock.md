# 固定更新時計

`CFixedStepClock`は可変frame時間を固定tickへ変換し、過負荷時の追従上限と補間率を管理します。

## 目的

`Timing` moduleの`acs::timing::CFixedStepClock`は、可変な経過秒を固定更新回数と描画補間率へ変換する。
時計は利用側が値として所有し、OS時刻、ゲームループ、World、共有サービスの寿命を持たない。
局所的で決定論的な計算状態なのでsubsystemには登録しない。

`acs::game::FSceneClock`はpauseや時間倍率を反映するシーン時間、`FWorldClockSubsystem`はWorld全体で共有する
経過時間とframe数を扱う。責務と所有期間が異なるため、これらを`CFixedStepClock`へ置換しない。

## 基本操作

一回の`Advance`は、蓄積上限を超えた秒数と実行回数上限を超えた固定更新を破棄する。
破棄量は今回結果と累積統計の両方へ記録する。負値、NaN、無限大は拒否し、時計を変更しない。
累積回数と累積破棄秒は型の最大値で飽和し、巻き戻りや無限大を保存しない。

## 設定と保存

公開値は次の四種類に分ける。

- `FFixedStepOptions`: 刻み秒、一回の最大更新回数、一回の最大蓄積秒
- `FFixedStepAdvanceResult`: 更新回数、補間率、破棄秒、受付状態、上限適用状態
- `FFixedStepClockSnapshot`: 設定、持ち越し秒、累積破棄秒、累積更新回数
- `CFixedStepClock`: 現在状態を所有して変換を行う48バイトの値

`TryCaptureSnapshot`と`TryRestoreSnapshot`はnull、整列違反、時計自身との領域重複、不正内容を拒否する。
失敗時は出力または時計を変更しない。`TryReconfigurePreservingProgress`は補間率と累積統計を保ったまま
刻み幅を変更し、不正設定では現在状態を維持する。

保存値は同じACS版の決定論的な再生位置を一時保存するための値であり、異なるversion間の永続形式ではない。
永続化する場合は利用側のversion付き形式へ各fieldを明示して格納する。

## 一括処理

一括入力と複数時計の保存・復元は最大4096件を受け付ける。

- `CFixedStepClock::TryAdvanceBatch`
- `TryCaptureFixedStepClockSnapshots`
- `TryRestoreFixedStepClockSnapshots`

各関数はnull、整列、アドレス加算、容量、入力と出力の領域重複、全入力内容を先に検証する。
失敗時は時計、出力配列、出力件数をまとめて維持する。容量値は書き込む件数だけでなく指定された出力領域全体の
重複判定に使う。0件では配列を参照せず成功し、件数出力がある関数は0を設定する。

時計と配列は内部同期を持たない。同じ時計へ複数threadから同時に書き込む場合は利用側が所有権または同期を提供する。

## 検証

`ACS.FixedStepClock`は型の大きさと整列、十進小数境界、上限、飽和、失敗時不変、一括処理の単発順序一致、
最大4096件、null、整列違反、アドレス加算・乗算のoverflow、時計・入力・出力・件数・容量末尾の領域重複、
複数保存の一括復元を固定する。
同じ試験源を専用targetと全体単体試験の両方で実行する。
