# FAnimationCurve の安全な更新・評価

`FAnimationCurve` は editor、asset loader、runtime animation から同じ key 列を受け取る。
入力元が増えても壊れた key が内部の sort 不変条件を破らないよう、checked API と
一括反映契約を提供する。

## 検証付き API

- `TryAddKey`
- `TryAddKeyHermite`
- `TrySetKeys`
- `TrySetEasingPreset`
- `TrySetWrapModes`
- `TryEvaluate`

既存の `AddKey`、`AddKeyHermite`、`Evaluate` は互換ラッパーとして残る。
不正な legacy add は no-op、評価不能な legacy evaluate は安全な `0` を返す。
詳細な回復処理が必要な呼び出し元は `FAnimationCurveResult` の安定した
`EAnimationCurveError` を使う。

## key 不変条件

- key 数は最大 65,536
- time、value、in/out tangent はすべて有限値
- interpolation と wrap mode は定義済み enum のみ。legacy wrap setter の不正値は no-op
- bulk import の time は厳密な昇順
- 同一 time は bulk import ではエラー、単一 add では既存 key の更新

非有限 time を内部配列へ入れないため、二分探索の全順序が保たれる。
不正 enum を補間 switch の fallback に流さず、入力境界で診断する。

## transactional な一括取り込み

`TrySetKeys` は全 key と wrap mode を先に検証し、同じ allocator を使う一時配列へ
全量 staging する。検証または allocation が失敗した場合、既存 key、pre-wrap、
post-wrap を変更しない。staging 完了後の move commit は allocation を伴わない。

`TrySetEasingPreset(type, sample_count)` は型付き easing を `[0,1]` の線形key列へ
変換する。33種類すべてに対応し、Back/Elasticのovershootも保持する。
sample数は2～4,096で、既定65。無効type、不正sample数、OOM、評価失敗では
既存keyとwrap modeを変更せず、成功時は前後wrapをClampへ正規化する。
AnimCurve EditorのPresetコンボもこのAPIだけを通して曲線を置換する。

`FAnimationCurve(FAllocator&)` により subsystem 固有 allocator と決定的な OOM
失敗注入を利用できる。曲線が保持する配列は、その allocator で最後まで解放される。

## 検証付き評価

`TryEvaluate` は非有限 time を拒否し、補間結果が overflow して非有限になった場合は
`ResultOutOfRange` を返す。失敗時は caller の出力値を変更しない。これにより NaN が
camera、UI、physics など後段の状態へ伝播するのを防ぐ。

Wrap 計算後の時刻、segment 幅、segment 内の正規化値も補間前に有限性を検証する。
この検証は Step 補間にも適用され、wrap の減算や PingPong の周期計算が overflow
した場合に、左 key の有限値を返して成功扱いになることを防ぐ。

## テスト観点

`animation_curve_safety_tests.cpp` は sorted insert、同一時刻更新、重複・非有限値・
不正 enum、OOM 時の状態不変、評価 overflow、legacy API の安全な no-opに加え、
全33 easing preset、sample境界、preset OOM rollbackを検証する。
