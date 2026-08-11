# テキストアセット読み込みの安全境界

`FMaterial2D` の `.acsmat` と `FProjectSettings` の INI は、外部ファイルや
エディタから渡される非信頼テキストとして扱う。互換 API も checked API を経由し、
解析に失敗した場合は呼び出し側が保持しているマテリアル／設定を変更しない。

## 共通方針

- parser の正本は `const char* + usize` を受け取り、NUL 終端を前提にしない。
- 入力は最大 1 MiB、1 行は最大 511 byte、最大 4096 行。
- 指定範囲内の embedded NUL、長すぎる行、黙った切り詰めを拒否する。
- ファイルパスは最大 1023 byte。ファイルサイズは 64 bit で取得し、読み込み前の
  narrowing を行わない。
- サイズ取得後は宣言サイズを完全に読み、短縮と末尾への増加を検出する。
- allocation failure、read／close failure を成功として commit しない。
- 数値は末尾まで消費する。実数は `NaN`／`Inf`／overflow を拒否し、整数は
  `i32` 範囲を検証する。

## FMaterial2D

推奨 API は `TryParseAcsmatText` です。入力を一時 `FMaterial2D` へ解析し、
全検証が成功した場合だけ呼び出し側のmaterialを置き換えます。

ファイルには `TryLoadAcsmatFile` を使う。`ParseAcsmatText` と
`LoadAcsmatFile` は互換 API であり、同じ検証を通る。

- ヘッダは `ACSMAT 1` の完全一致。
- 既知キーの重複は曖昧性を避けるため拒否する。
- 未知キーは将来のフォーマット拡張との前方互換のため無視する。
- `animated` と `shadingMode` は `0..1`。effect／kind は既知の名前だけを受理する。
- name は最大 63 byte、albedo／normal path は最大 255 byte。
- 解析は一時 `FMaterial2D` に行い、全文が妥当な場合だけ出力へ代入する。

## FProjectSettings

推奨 API は `FProjectSettings::TryLoadText` と
`FProjectSettings::TryLoadFile`。空の INI と存在しないファイルは初回起動として
既定値を commit し、`used_defaults` を返す。それ以外の失敗では既存ストアを保つ。

- section は最大 31 byte、key は最大 63 byte、value は最大 191 byte。
- 入力中の同一 section／key（大文字小文字を含む完全一致）の重複は拒否する。
- schema にないキーは従来互換の custom `String` として保持する。
- 最大エントリ数は builtin を含め 1024。
- builtin の `Float`、`Int`、`Bool`、`Color`、`Enum` は型を検証してから反映する。
- `GetFloat`／`GetInt`／`GetColor` も部分一致や非有限値を受理しない。
- `TryResetToDefaults` と `TryAdd` は allocation failure を返し、途中状態を公開しない。

## エラー処理

両 result は安定した enum、行番号、読み込んだ byte 数を返す。
ログ表示には `FMaterial2DLoadResult::ErrorName` または
`FProjectSettingsLoadResult::ErrorName` を使用できる。UI はエラー名と行番号を表示し、
破損ファイルを既定値として上書き保存しないこと。
