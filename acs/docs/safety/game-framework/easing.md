# イージングカタログと安全性契約

`gameframework/Easing.h` は、正規化されたイージング曲線を固定カタログとして
全33種類提供する。新しいコードでは `Easing::EEasingType` を保持し、
`Easing::TryEvaluate` で評価する。従来の Tween・Sequence コードとの
ソース互換性を保つため、関数ポインター API も引き続き利用できる。

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

`EEasingType::Count` は終端を表す番兵であり、評価可能な曲線ではない。
永続化や UI では列挙値の数値を保存せず、`GetName` と
`TryParseName` を使用する。正準名は安定した、大文字小文字を区別する
ASCII 識別子である。

## 数学的契約

全ての曲線は正規化時刻 `t` を正規化補間進度へ写像する。

- 全曲線で `t = 0` の値は厳密に `0`、`t = 1` の値は厳密に `1`。
- `InX(t) = 1 - OutX(1 - t)`。
- 全ての `InOutX`、`Linear`、`SmoothStep`、`SmootherStep` は
  `f(t) = 1 - f(1 - t)` を満たす。
- 多項式曲線は2次から5次を使用する。`Sine`、`Expo`、`Circ` は
  ACS が固定した正規化式を使用する。
- `SmoothStep` は `t^2(3 - 2t)`、`SmootherStep` は
  `t^3(t(6t - 15) + 10)`。
- `Back` と `Elastic` は行き過ぎを表現するため意図的に `[0, 1]` を外れる。
  `Bounce` は `[0, 1]` に収まるが、意図的に単調ではない。

個別の互換関数も有限入力を範囲制限する。エラーを返せない従来の呼び出し側に対して、
NaN は決定的に `0`、負の無限大は `0`、正の無限大は `1` へ写像する。
信頼できない入力、計算結果、永続化データ由来の入力には検査付き振り分け処理を
使用し、無効入力を暗黙に正規化せず診断する。

## 検査付き評価

```cpp
f32 progress = previous_progress;
const Easing::FEasingResult result =
    Easing::TryEvaluate(Easing::EEasingType::OutCubic, raw_time, progress);
if (!result.Succeeded()) {
    // progressは変更されない
}
```

`TryEvaluate` は次の安全性を保証する。

- 評価前に有限入力を `[0, 1]` へ範囲制限する。
- NaN、正の無限大、負の無限大を拒否する。
- `Count` と未知の列挙値を拒否する。
- 将来追加された曲線が非有限値を生成した場合は `NonFiniteResult`。
- 失敗時は出力参照を変更しない。
- 成功時は必ず有限値を生成する。
- メモリ確保を行わず、`noexcept`。

`Evaluate(type, t, fallback)` は簡易形式であり、検査付き評価の失敗時に
`fallback` を返す。無効な型と無効入力を呼び出し側で区別する場合は
`TryEvaluate` を使用する。

`GetFunction(type)` は無効値に `nullptr`、`GetName(type)` は
`"Invalid"` を返す。`TryParseName` は null、空文字列、未知名に `false` を返し、
出力列挙値を変更しない。

## 検査付き名前変換

永続化、エディター、外部入力では、原因を分類できる次の API を使用する。

- `TryGetName(type, out_name)` は無効な列挙値を `InvalidType` として拒否する。
- `TryParseNameChecked(name, out_type)` は null を `NullName`、空文字列と
  未知名を `UnknownName` として拒否する。
- どちらも失敗時に出力引数を変更しない。
- 正準名は大文字小文字を区別し、既存の `GetName` /
  `TryParseName` と同じ安定文字列を使用する。

`TryParseName` は互換性のため真偽値形式を維持し、内部では検査付き API と同じ
検証を行う。

## 一括サンプリング

エディターのプレビュー、参照表、デバッグ描画では、個別評価ループの代わりに
`TrySampleCurve(type, out_values, sample_count)` を使用できる。

```cpp
f32 preview[65]{};
const Easing::FEasingResult sample_result =
    Easing::TrySampleCurve(
        Easing::EEasingType::InOutSine, preview, 65);
if (!sample_result.Succeeded()) {
    // preview 全体は変更されない
}
```

- `[0,1]` 上を等間隔に評価し、先頭は厳密な `0`、末尾は厳密な `1`。
- `sample_count` は `kMinSampleCount = 2` 以上、
  `kMaxSampleCount = 65536` 以下。
- 無効な型は `InvalidType`、点数範囲外は `InvalidSampleCount`、
  null の出力は `NullOutput`。
- 将来追加された曲線が非有限値を生成した場合は `NonFiniteResult`。
- 書き込み前に全点を検証するため、どの失敗でも出力配列全体を変更しない。
- メモリ確保を行わず `noexcept`。検証と書き込みの 2 回の走査も上限付きである。

## Tween・Sequence での利用

列挙型オーバーロードを使うと、選択したイージングを永続化・検査可能な状態で
保持できる。

```cpp
FTweenManager tweens;
f32 opacity = 0.0f;
tweens.Tween(&opacity, 0.0f, 1.0f, 0.25f,
             Easing::EEasingType::OutCubic);

FSequence sequence;
sequence.Tween(&opacity, 1.0f, 0.0f, 0.2f,
                Easing::EEasingType::InOutSine);
```

既存の呼び出し側は互換関数ポインタを引き続き渡せる。

```cpp
Easing::EasingFn easing = Easing::GetFunction(
    Easing::EEasingType::OutBounce);
tweens.Tween(&opacity, 0.0f, 1.0f, 0.5f, easing);

// 名前付き直接関数も引き続き利用できる
sequence.Tween(&opacity, 1.0f, 0.0f, 0.3f, Easing::InQuad);
```

エディターデータ、永続化、リフレクション、新規 API では列挙型オーバーロードを優先する。
`EasingFn` は互換境界、または呼び出し側が意図的にカスタムイージング関数を
渡す場合に使用する。

## `acs::easy` の窓口

`easy/Easy.h` をインクルードすると、`acs::game` 名前空間へ入らずに全カタログを
利用できる。

```cpp
using namespace acs::easy;

const EEasingType type = EEasingType::SmootherStep;
const f32 progress = Ease(raw_time, type, /*fallback=*/0.0f);

f32 checked_progress = previous_progress;
const FEasingResult result =
    TryEase(raw_time, EEasingType::OutElastic, checked_progress);
if (!result.Succeeded()) {
    // checked_progressは変更されない
}

const char* name = EasingName(type);
EEasingType parsed = EEasingType::Linear;
const bool parsed_ok = TryParseEasingName(name, parsed);

const char* checked_name = nullptr;
const FEasingResult name_result =
    TryGetEasingName(type, checked_name);

const FEasingResult parse_result =
    TryParseEasingNameChecked(checked_name, parsed);

f32 preview[33]{};
const FEasingResult sample_result =
    TrySampleEasing(type, preview, 33);
```

`acs::easy::EEasingType`、`EEasingError`、`FEasingResult` は GameFramework 型の
別名である。`Ease`、`TryEase`、`TrySampleEasing`、`EasingName`、`TryParseEasingName`、
`TryGetEasingName`、`TryParseEasingNameChecked` は同一カタログへ転送する。
そのため範囲制限、代替値、無効入力、正準名、失敗時の出力不変性は
両方の窓口で一致する。

既存の `EaseIn`、`EaseOut`、`EaseInOut`、`EaseOutBack`、
`EaseOutBounce`、`EaseOutElastic` ショートカットはソース互換性を維持する。
重複した式を持たず、それぞれ `InQuad`、`OutQuad`、`InOutQuad`、
`OutBack`、`OutBounce`、`OutElastic` へ委譲する。

モジュール依存は意図的に一方向とし、Easy は GameFramework へ公開依存するが、
GameFramework は Easy へ依存しない。この境界を変更する場合は
`src/easy/Easy.Build.cs` と生成済み `src/easy/Module.cmake` の依存リストを
同期する。

## 回帰カバレッジ

### `tests/easing_completeness_tests.cpp`

型付き GameFramework カタログ自体を検証する。

- 全 33 の評価可能な曲線値と固定数値 ID、正準名、名前解析の往復、関数解決。
- 厳密な両端値、独立した固定期待値、標本結果の有限性、入出力の対称性。
- 数学的契約が単調な曲線の単調性。
- `Back` と `Elastic` の行き過ぎ、`Bounce` の有界性と非単調性。
- 有限入力の範囲制限。
- NaN、無限大、無効な列挙値の拒否と失敗時の出力不変性。
- 従来関数ポインタ境界での全域関数動作。
- 代替値、検査付き名前変換の全 33 種の往復、エラー分類、出力不変性。
- 一括サンプリングの全33種一致、厳密な両端、点数上限、失敗時の配列全体不変性。

### `tests/animation_curve_persistence_tests.cpp`

全 33 種類を `FAnimationCurve::TrySetEasingPreset` で 65 キーの曲線へ変換し、
`FAnimationCurveArchive` の符号化、復号、再符号化を行う。全キーと範囲外方式の
一致と正準バイト列の再現を検証する。

### `tests/easy_easing_facade_tests.cpp`

Easy の窓口を検証する。

- 全 33 種類の評価値と GameFramework カタログの一致。
- 全正準名の往復。
- 有限範囲外入力の範囲制限と非有限入力、無効な列挙値の診断。
- 代替値と失敗時の出力不変性。
- 検査付き名前 API のエラー分類と全 33 種の往復。
- 一括サンプリング結果と GameFramework カタログの一致。
- 従来ショートカットとカタログ関数の一致。

## 旧保存値の移行

ACS が受理する旧保存形式と `Easing::EEasingType` は同じ 33 曲線を持ちますが、固定数値 ID の順序が異なります。旧保存値を `static_cast` すると、例えば値 1 の
`SmoothStep` が ACS では `InQuad` として解釈される。

`gameframework/LegacyKitEaseCodec.h` の `FLegacyKitEaseCodec` を使い、
読み込み時は `TryDecode`、旧形式への書き戻し時は `TryEncode` を呼ぶ。
範囲外の整数、`Count`、不明な ACS 列挙値は拒否され、失敗時の出力は変更されない。

```cpp
i32 saved_legacy_value = 1;
Easing::EEasingType type = Easing::EEasingType::Linear;
if (!FLegacyKitEaseCodec::TryDecode(saved_legacy_value, type)) {
    // 破損または未対応の保存値として扱う。
}
```

対応表は旧保存形式の0～32を明示的に保持し、
`tests/legacy_kit_ease_codec_tests.cpp` が全33値、双方向往復、
範囲外入力、失敗時の出力不変性を検証する。
