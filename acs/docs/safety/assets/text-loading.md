# テキストアセット読み込みの安全境界

`FMaterial2D` の `.acsmat` と `FProjectSettings` の INI は、外部ファイルや
エディターから渡される非信頼テキストとして扱う。互換 API も検査付き API を経由し、
解析に失敗した場合は呼び出し側が保持しているマテリアル／設定を変更しない。

## 共通方針

- 解析器の正本は `const char* + usize` を受け取り、NUL 終端を前提にしない。
- 入力は最大 1 MiB、最大 4096 行。1 行の上限は `.acsmat` が 1023 バイト、`FProjectSettings` が 511 バイト。
- 指定範囲内の埋め込み NUL、長すぎる行、黙った切り詰めを拒否する。
- ファイルパスは最大 1023 バイト。ファイルサイズは 64 ビットで取得し、読み込み前の
  縮小変換を行わない。
- サイズ取得後は宣言サイズを完全に読み、短縮と末尾への増加を検出する。
- 確保失敗、読み取り失敗、終了失敗を成功として反映しない。
- 数値は末尾まで消費する。実数は `NaN` / `Inf` / オーバーフローを拒否し、整数は
  `i32` 範囲を検証する。

## FMaterial2D

推奨 API は次のとおり。

```cpp
FMaterial2D next = current;
FMaterial2DLoadResult result =
    TryParseAcsmatText(bytes, byte_count, next);
if (result.Succeeded()) {
    current = next;
}
```

ファイルには `TryLoadAcsmatFile` を使う。`ParseAcsmatText` と
`LoadAcsmatFile` は互換 API であり、同じ検証を通る。

- ヘッダは `ACSMAT 1` の完全一致。
- 既知キーの重複は曖昧性を避けるため拒否する。
- 未知キーは将来のフォーマット拡張との前方互換のため無視する。
- `animated` と `shadingMode` は `0..1`。効果と種類は既知の名前だけを受理する。
- 名前は最大 63 バイト、アルベドと法線のパスは最大 255 バイト。
- 解析は一時 `FMaterial2D` に行い、全文が妥当な場合だけ出力へ代入する。

## FProjectSettings

推奨 API は `FProjectSettings::TryLoadText` と
`FProjectSettings::TryLoadFile`。空の INI と存在しないファイルは初回起動として
既定値を反映し、`used_defaults` を返す。それ以外の失敗では既存ストアを保つ。

- 区画名は最大 31 バイト、キーは最大 63 バイト、値は最大 191 バイト。
- 入力中の同一区画とキー（大文字小文字を含む完全一致）の重複は拒否する。
- スキーマにないキーは従来互換の独自 `String` として保持する。
- 最大エントリ数は組み込み項目を含め 1024。
- 組み込みの `Float`、`Int`、`Bool`、`Color`、`Enum` は型を検証してから反映する。
- `GetFloat`／`GetInt`／`GetColor` も部分一致や非有限値を受理しない。
- `TryResetToDefaults` と `TryAdd` は確保失敗を返し、途中状態を公開しない。

## エラー処理

両結果は安定した列挙値、行番号、読み込んだバイト数を返す。
ログ表示には `FMaterial2DLoadResult::ErrorName` または
`FProjectSettingsLoadResult::ErrorName` を使用できる。UI はエラー名と行番号を表示し、
破損ファイルを既定値として上書き保存しないこと。
