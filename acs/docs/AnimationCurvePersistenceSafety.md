# FAnimationCurve 永続化と安全性

`FAnimationCurveArchive` は `FAnimationCurve` をアセット、エディター、
ランタイム間で安定して受け渡すための、固定幅バイナリ codec です。
メモリ buffer の `Encode` / `Decode` と、アトミックな
`SaveToFile` / `LoadFromFile` を提供します。

## 形式

wire format は ABI や compiler の padding に依存しません。すべて
little endian で、32 byte header の後に 1 key あたり 20 byte の record が
続きます。

| 領域 | 内容 |
|---|---|
| 0..7 | `ACSCURV\0` magic |
| 8..9 | wire version (`1`) |
| 10..11 | header size (`32`) |
| 12..15 | key count |
| 16..17 | pre/post wrap mode |
| 18..19 | reserved zero |
| 20..23 | payload byte count |
| 24..27 | wire 全体の CRC32（checksum 領域自身は zero として計算） |
| 28..31 | reserved zero |

各 key record は `time`、`value`、`in_tangent`、`out_tangent` の
IEEE-754 `f32` bit pattern、2つの interpolation enum、2 byte の
reserved zero で構成されます。native の `FCurveKey` 全体を `memcpy` しないため、
構造体の padding や enum の native size が形式へ漏れません。

## 境界

- 最大 key 数は `FAnimationCurve::kMaxKeys`（65,536）。
- 最大 wire size は 1,310,752 byte。
- header size、payload size、実 buffer size は完全一致が必要。末尾データも拒否。
- 未知 version、未知 enum、非ゼロ reserved、NaN/Infinity、未整列・同時刻 key、
  CRC 不一致を拒否。CRC は wrap mode を含む semantic header と全 key を覆う。
- `out_capacity` 不足時、`Encode` は必要 size を返し、出力 buffer を変更しない。
- `Decode` / `LoadFromFile` は全検証と全確保が完了してから
  `TrySetKeys` で commit する。破損、OOM、I/O 失敗時も既存の key と wrap mode は不変。

`FAnimationCurveArchiveResult` の `error` は安定した大分類です。
曲線内容の失敗では `curve_error` と `key_index`、ファイル envelope の失敗では
`persistence_subcode` と `os_error` を追加診断に使えます。

## ファイル保存

ファイル helper は canonical wire record を `FSaveArchive` の version 1 payload として
保存します。このため inner CRC に加えて、`.acssave` envelope の magic、payload size、
CRC、および同一ディレクトリ一時ファイルからの flush 済み atomic replace が適用されます。
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

wire version を変更する場合は既存 layout を上書きせず、新 version の decoder と
migration path を追加してください。`UnsupportedVersion` は migration 分岐のための
明示的な診断です。
