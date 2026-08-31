# FAnimationCurve 永続化と安全性

`FAnimationCurveArchive` は `FAnimationCurve` をアセット、エディター、
ランタイム間で安定して受け渡すための、固定幅バイナリ符号化器です。
メモリバッファーの `Encode` / `Decode` と、原子的な
`SaveToFile` / `LoadFromFile` を提供します。

## 形式

バイナリ形式は ABI やコンパイラーの詰め物に依存しません。すべて
リトルエンディアンで、32 バイトのヘッダーの後に 1 キーあたり 20 バイトのレコードが
続きます。

| 領域 | 内容 |
|---|---|
| 0..7 | マジック値 `ACSCURV\0` |
| 8..9 | 形式バージョン（`1`） |
| 10..11 | ヘッダーサイズ（`32`） |
| 12..15 | キー数 |
| 16..17 | 前後の範囲外方式 |
| 18..19 | 予約領域のゼロ値 |
| 20..23 | データ本体のバイト数 |
| 24..27 | 形式全体の CRC32（検査値領域自身はゼロとして計算） |
| 28..31 | 予約領域のゼロ値 |

各キーレコードは `time`、`value`、`in_tangent`、`out_tangent` の
IEEE-754 `f32` ビット表現、2 つの補間方式列挙値、2 バイトの
予約ゼロ値で構成されます。ネイティブの `FCurveKey` 全体を `memcpy` しないため、
構造体の詰め物や列挙型のネイティブサイズが形式へ漏れません。

## 境界

- 最大キー数は `FAnimationCurve::kMaxKeys`（65,536）。
- 最大形式サイズは 1,310,752 バイト。
- ヘッダーサイズ、データ本体サイズ、実バッファーサイズは完全一致が必要。末尾データも拒否。
- 未知バージョン、未知列挙値、非ゼロの予約領域、NaN / 無限大、未整列または同時刻のキー、
  CRC 不一致を拒否。CRC は範囲外方式を含む意味情報ヘッダーと全キーを覆う。
- `out_capacity` 不足時、`Encode` は必要サイズを返し、出力バッファーを変更しない。
- `Decode` / `LoadFromFile` は全検証と全確保が完了してから
  `TrySetKeys` で反映する。破損、メモリ不足、I/O 失敗時も既存キーと範囲外方式は不変。

`FAnimationCurveArchiveResult` の `error` は安定した大分類です。
曲線内容の失敗では `curve_error` と `key_index`、ファイル包絡の失敗では
`persistence_subcode` と `os_error` を追加診断に使えます。

## ファイル保存

ファイル補助関数は正準バイナリレコードを `FSaveArchive` のバージョン 1 データ本体として
保存します。このため内側の CRC に加えて、`.acssave` 包絡のマジック値、データ本体サイズ、
CRC、および同一ディレクトリ一時ファイルからの永続化済み原子的置換が適用されます。
保存中の失敗で既存ファイルを途中状態へ置き換えません。

```cpp
FAnimationCurve curve;
// ... checked API で key を設定 ...

auto saved = FAnimationCurveArchive::SaveToFile(
    L"camera_fov.acssave", curve);
if (!saved.Succeeded()) {
    // saved.error / persistence_subcode / os_error を記録
}

FAnimationCurve loaded;
auto restored = FAnimationCurveArchive::LoadFromFile(
    L"camera_fov.acssave", loaded);
if (!restored.Succeeded()) {
    // loaded は呼び出し前の状態を保持
}
```

未知バージョンは `UnsupportedVersion` として拒否します。
