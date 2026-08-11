# イージングカタログと安全性契約

`gameframework/Easing.h` は、正規化されたイージング曲線を固定カタログとして
全33種類提供する。新しいコードでは `Easing::EEasingType` を保持し、
`Easing::TryEvaluate` で評価する。従来のTween・Sequenceコードとの
ソース互換性を保つため、関数ポインタAPIも引き続き利用できる。

## 全カタログ

カタログは次の33値で構成する。

- `Linear`
- `InQuad`, `OutQuad`, `InOutQuad`
- `InCubic`, `OutCubic`, `InOutCubic`
- `InQuart`, `OutQuart`, `InOutQuart`
- `InQuint`, `OutQuint`, `InOutQuint`
- `InSine`, `OutSine`, `InOutSine`
- `InExpo`, `OutExpo`, `InOutExpo`
- `InCirc`, `OutCirc`, `InOutCirc`
- `InBack`, `OutBack`, `InOutBack`
- `InElastic`, `OutElastic`, `InOutElastic`
- `InBounce`, `OutBounce`, `InOutBounce`
- `SmoothStep`, `SmootherStep`

`EEasingType::Count` は終端を表すsentinelであり、評価可能な曲線ではない。
永続化やUIではenumの数値を保存せず、`GetName` と
`TryParseName` を使用する。canonical名は安定した、大文字小文字を区別する
ASCII識別子である。

## 旧形式の数値 ID

旧 Kit 形式で保存された数値 ID は、`EEasingType` へ直接castせず
`FLegacyKitEaseIdCodec` で移行する。旧形式とACSでは同じ曲線でも数値順が
異なり、同じ数値を持つのは `Linear = 0` だけである。

固定された33個の対応は次の通り。

| 旧 ID | ACS型 | 旧 ID | ACS型 | 旧 ID | ACS型 |
|---:|---|---:|---|---:|---|
| 0 | `Linear` | 11 | `SmootherStep` | 22 | `InExpo` |
| 1 | `SmoothStep` | 12 | `InSine` | 23 | `OutExpo` |
| 2 | `OutElastic` | 13 | `OutSine` | 24 | `InOutExpo` |
| 3 | `InQuad` | 14 | `InOutQuad` | 25 | `InCirc` |
| 4 | `OutQuad` | 15 | `InOutCubic` | 26 | `OutCirc` |
| 5 | `InCubic` | 16 | `InQuart` | 27 | `InOutCirc` |
| 6 | `OutCubic` | 17 | `OutQuart` | 28 | `InOutBack` |
| 7 | `InOutSine` | 18 | `InOutQuart` | 29 | `InElastic` |
| 8 | `InBack` | 19 | `InQuint` | 30 | `InOutElastic` |
| 9 | `OutBack` | 20 | `OutQuint` | 31 | `InBounce` |
| 10 | `OutBounce` | 21 | `InOutQuint` | 32 | `InOutBounce` |

- `TryDecode` は旧 ID `0` 以上 `33` 未満だけを受理する。
- `TryEncode` は上表にあるACS型だけを旧 IDへ戻す。将来追加される型は、
  対応表を明示更新するまで拒否する。
- どちらも失敗時に出力引数を変更せず、メモリ確保を行わない `noexcept` API。
- 旧形式の別名文字列はACSのcanonical名ではないため、
  `Easing::TryParseName` へ追加しない。
- 変換対象は保存された曲線識別子である。評価はACSの正規実装を使うため、
  過去実装との浮動小数点bit列の完全一致を保証するものではない。

## 数学的契約

全ての曲線は正規化時刻 `t` を正規化補間進度へ写像する。

- 全曲線で `t = 0` の値は厳密に `0`、`t = 1` の値は厳密に `1`。
- `InX(t) = 1 - OutX(1 - t)`。
- 全ての `InOutX`、`Linear`、`SmoothStep`、`SmootherStep` は
  `f(t) = 1 - f(1 - t)` を満たす。
- 多項式曲線は2次から5次を使用する。`Sine`、`Expo`、`Circ` は標準的な
  Penner形式の式を使用する。
- `SmoothStep` は `t^2(3 - 2t)`、`SmootherStep` は
  `t^3(t(6t - 15) + 10)`。
- BackとElasticはovershootを表現するため意図的に `[0, 1]` を外れる。
  Bounceは `[0, 1]` に収まるが、意図的に単調ではない。

個別の互換関数も有限入力をclampする。エラーを返せない従来callerに対して、
NaNは決定的に `0`、負の無限大は `0`、正の無限大は `1` へ写像する。
信頼できない入力、計算結果、永続化データ由来の入力にはchecked dispatcherを
使用し、無効入力を暗黙に正規化せず診断する。

## checked評価

`TryEvaluate` は次の安全性を保証する。

- 評価前に有限入力を `[0, 1]` へclampする。
- NaN、正の無限大、負の無限大を拒否する。
- `Count` と未知のenum値を拒否する。
- 将来追加された曲線が非有限値を生成した場合は `NonFiniteResult`。
- 失敗時は出力参照を変更しない。
- 成功時は必ず有限値を生成する。
- メモリ確保を行わず、`noexcept`。

`Evaluate(type, t, fallback)` は簡易形式であり、checked評価の失敗時に
`fallback` を返す。無効typeと無効入力を呼び出し側で区別する場合は
`TryEvaluate` を使用する。

`GetFunction(type)` は無効値に `nullptr`、`GetName(type)` は
`"Invalid"` を返す。`TryParseName` はnull、空文字列、未知名に `false` を返し、
出力enumを変更しない。

## checked名前変換

永続化、エディタ、外部入力では、原因を分類できる次のAPIを使用する。

- `TryGetName(type, out_name)` は無効enumを `InvalidType` として拒否する。
- `TryParseNameChecked(name, out_type)` はnullを `NullName`、空文字列と
  未知名を `UnknownName` として拒否する。
- どちらも失敗時に出力引数を変更しない。
- canonical名は大文字小文字を区別し、既存の `GetName` /
  `TryParseName` と同じ安定文字列を使用する。

`TryParseName` は互換性のためbool形式を維持し、内部ではchecked APIと同じ
検証を行う。

## 一括サンプリング

エディタのプレビュー、lookup table、デバッグ描画では、個別評価ループの代わりに
`TrySampleCurve(type, out_values, sample_count)` を使用できる。

- `[0,1]` 上を等間隔に評価し、先頭は厳密な `0`、末尾は厳密な `1`。
- `sample_count` は `kMinSampleCount = 2` 以上、
  `kMaxSampleCount = 65536` 以下。
- 無効typeは `InvalidType`、点数範囲外は `InvalidSampleCount`、
  null出力は `NullOutput`。
- 将来追加された曲線が非有限値を生成した場合は `NonFiniteResult`。
- 書き込み前に全点を検証するため、どの失敗でも出力配列全体を変更しない。
- メモリ確保を行わず `noexcept`。検証と書き込みの2 passも上限付きである。

## Tween・Sequenceでの利用

enumオーバーロードを使うと、選択したイージングを永続化・検査可能な状態で
保持できる。

既存の呼び出し側は互換関数ポインタを引き続き渡せる。

エディタデータ、永続化、reflection、新規APIではenumオーバーロードを優先する。
`EasingFn` は互換境界、または呼び出し側が意図的にカスタムイージング関数を
渡す場合に使用する。

## Easy facade

`easy/Easy.h` をincludeすると、`acs::game` namespaceへ入らずに全カタログを
利用できる。

`acs::easy::EEasingType`、`EEasingError`、`FEasingResult` はGameFramework型の
aliasである。`Ease`、`TryEase`、`TrySampleEasing`、`EasingName`、`TryParseEasingName`、
`TryGetEasingName`、`TryParseEasingNameChecked` は同一カタログへ転送する。
そのためclamp、fallback、無効入力、canonical名、失敗時の出力不変性は
両facadeで一致する。

既存の `EaseIn`、`EaseOut`、`EaseInOut`、`EaseOutBack`、
`EaseOutBounce`、`EaseOutElastic` ショートカットはソース互換性を維持する。
重複した式を持たず、それぞれ `InQuad`、`OutQuad`、`InOutQuad`、
`OutBack`、`OutBounce`、`OutElastic` へ委譲する。

モジュール依存は意図的に一方向とし、EasyはGameFrameworkへpublic依存するが、
GameFrameworkはEasyへ依存しない。`src/easy/Easy.Build.cs`と生成済み
`src/easy/Module.cmake`は、この一方向依存を同じ内容で記録する。

## 回帰カバレッジ

回帰検査はcatalog完全性、数値契約、互換入口を責務別testへ分けます。

### `tests/easing_completeness_tests.cpp`

型付きGameFrameworkカタログ自体を検証する。

- 全33の評価可能な曲線値と固定数値ID、canonical名、名前parse roundtrip、関数解決。
- 厳密な両端値、独立ゴールデン値、sample結果の有限性、In/Out対称性。
- 数学的契約が単調な曲線の単調性。
- Back・ElasticのovershootとBounceの有界性・非単調性。
- 有限入力のclamp。
- NaN、無限大、無効enumの拒否と失敗時の出力不変性。
- 従来関数ポインタ境界での全域関数動作。
- fallback、checked名前変換の全33種roundtrip、エラー分類、出力不変性。
- 一括サンプリングの全33種一致、厳密な両端、点数上限、失敗時の配列全体不変性。

### `tests/legacy_kit_ease_id_codec_tests.cpp`

旧形式の固定ID変換を検証する。

- 独立した全33件のgolden対応表。
- 旧IDとACS型の両方向roundtrip。
- `-1`、`33`、32bit整数の両端、`Count`、`0xff` の拒否。
- 失敗時の出力不変性と両APIの `noexcept`。
- 直接castでは旧IDを移行できないこと。

### `tests/animation_curve_persistence_tests.cpp`

全33種類を `FAnimationCurve::TrySetEasingPreset` で65 keyのcurveへ変換し、
`FAnimationCurveArchive` のencode・decode・再encodeを行う。全key・wrap modeの
一致とcanonical byte列の再現を検証する。

### `tests/easy_easing_facade_tests.cpp`

Easy facadeを検証する。

- 全33種類の評価値とGameFrameworkカタログの一致。
- 全canonical名のroundtrip。
- 有限範囲外入力のclampと非有限入力・無効enumの診断。
- fallbackと失敗時の出力不変性。
- checked名前APIのエラー分類と全33種roundtrip。
- 一括サンプリング結果とGameFrameworkカタログの一致。
- 従来ショートカットとカタログ関数の一致。
