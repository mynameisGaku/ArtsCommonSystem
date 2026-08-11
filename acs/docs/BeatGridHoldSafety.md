# FBeatGrid のホールドノートと安全契約

`FBeatGrid` は chart をコピー所有し、有限値の再生 clock を進め、chart の各 note に
1 回だけ判定を発行します。検証付き API は取り込みデータやユーザー作成データ向けで、
既存ゲームコードのために従来 wrapper も残します。

## ホールドノートの lifecycle

ホールドノートは全体で 1 scoring unit です。

1. `PressLane(lane)`（または互換 alias の `Tap(lane)`）は、指定 lane にある未判定かつ
   inactive な note を探す。
2. 通常 note は直ちに確定する。
3. hold note は head 判定を保存して active になる。この時点では score、combo、
   judge callback を発行しない。
4. `ReleaseLane(lane)` は tail（`time_sec + hold_duration_sec`）が最も近い active hold を選ぶ。
5. tail の Good window 内で離すと note を確定し、head と tail の悪い方を最終結果にする。
6. window より早い release は Miss。`tail + good_window` を過ぎても active な note は
   `Tick` 中に Miss になる。

すべての完了経路は callback を呼ぶ前に active state を消し、note を判定済みにします。
press、release、tick を繰り返しても同じ note を再度加点しません。hold は合計数と精度で
1 note と数えます。

press/release 呼び出しは edge として扱います。判定したい物理 release ごとに
`ReleaseLane` を 1 回送る必要があります。pause 中は clock を止めますが、停止時刻に
一致する入力は受理し、従来の `Tap` 挙動を維持します。

## 決定的な衝突解決

chart は入力順を保持し、内部 sort しません。

- 距離が等しい未判定 note は chart index が最小のものを選ぶ。
- 距離が等しい active hold tail も chart index が最小のものを選ぶ。
- 1 回の `Tick` は期限切れ note を chart 入力順に処理する。

重なる hold も利用できます。`ActiveHoldCount()` は総数を返し、
`IsLaneHolding(lane)` は lane に 1 個以上の active hold があるかを返します。

## 検証付き chart 読み込み

`TryLoadChart` は `EBeatChartLoadResult` で成否を返し、
`BeatChartLoadResultName` は診断用の安定した名前を返します。

`TryLoadChart` は入力全体を検証し、3 個の一時 array を reserve して chart をコピーした後
だけ live chart を置き換えます。エラー時は以前の chart、再生 state、統計、callbacks を
維持します。

受理する上限は公開定数です。

| field | 契約 |
|---|---|
| note 数 | `0 .. kMaxBeatChartNotes`（65,536） |
| BPM | 有限値、`0 .. kMaxBeatBpm`（1,000） |
| note time | 有限値、`0 .. kMaxBeatChartTimeSec`（24 時間） |
| lane | 6 個の `EBeatLane` 値のいずれか |
| hold duration | 有限かつ非負、最大 1 時間 |
| hold tail | `kMaxBeatChartTimeSec` 以下 |

hold は duration が厳密に正でなければなりません。non-hold の duration は範囲内なら
受理し、0 へ canonicalize します。`nullptr` は空 chart の場合だけ有効です。

`LoadChart` は互換 wrapper です。結果を無視しますが、不正入力や allocation failure で
以前の chart を部分的に clear せず、そのまま維持します。

## timing と数値安全性

`TrySetTimingWindows` は有限、昇順、非負で `kMaxBeatTimingWindowMs` 以下の
millisecond 値だけを受理し、失敗時は state を変更しません。互換用
`SetTimingWindows` は有限値を clamp して昇順へ直し、非有限値を無視します。

`Tick` は次を無視します。

- 0 以下の delta time。
- NaN または infinity。
- 加算すると `f32` 再生 clock が overflow する有限 delta。

拒否した tick は time、hold、判定、score、event を変更しません。

## callback 安全性

judge state は `FJudgeCallback` を呼ぶ前に commit します。元の `FBeatGrid` 契約との
互換性のため、`FBeatEndCallback` は最後の通常 note press または hold release の直後
ではなく、次の有効な `Tick` まで遅延します。end state は callback 前に commit し、
正確に 1 回だけ発火します。

callback は grid の置換、reset、stop、clear を行えます。revision guard により、
実行中の `Tick` は古い chart index へ触れる前に停止します。この class は引き続き
single-threaded であり、callback から別 thread と同時に同じ grid へアクセスしては
いけません。

## テスト範囲

`tests/beat_grid_hold_safety_tests.cpp` は次を検証します。

- 通常 note の即時判定と end callback 遅延の互換挙動。
- hold の遅延 scoring と head/tail 結果の結合。
- 早期 release と未 release timeout の Miss。
- judge/end callback の exactly-once。
- 同時 note の入力順解決。
- 有限値/range/lane/count 検証と安定診断。
- allocator injection による allocation failure 時の transaction。
- 不正または overflow する delta time で state を変更しないこと。
- timing window 更新の transaction。
- Miss scan 中の callback が `ClearAll` する場合。
