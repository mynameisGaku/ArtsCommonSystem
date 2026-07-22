# FSettings 永続化の安全契約

`FSettings` のファイル形式は、UTF-8バイト列の
`<tag>:<key>=<value>\n` です。`tag` は `f`、`i`、`b`、`s` の4種類です。
外部入力を直接オブジェクトへ反映せず、`TryLoad` と `TrySave` を永続化境界として
使用します。既存の `Load` / `Save` は互換ラッパーとして残り、checked APIの
エラー番号を `FErrorCode::subcode` にそのまま伝えます。

## 読み込み契約

`TryLoad` は次の順序で処理します。

1. 読み取り専用handleを `FILE_SHARE_READ | FILE_SHARE_DELETE` で開く
2. サイズ上限と完全read、EOF、CloseHandleを確認する
3. embedded NUL、行長、構文、型、値、件数を全件検証する
4. 別の `TArray<FString>` と `TArray<FEntry>` に全件構築する
5. 失敗しないmove代入だけで2配列をcommitする

1から4のどこで失敗しても、既存のentry、文字列pool、capacity、および
`GetString` が以前返したポインタは一切変更されません。空ファイルだけは正常な
「空の設定」としてcommitされます。

同名keyは、型が異なる場合を含めて `DuplicateKey` で拒否します。「最後の値を採用」
にはしません。破損や人手編集の誤りを黙って隠さず、入力順に依存しないためです。
コメントは先頭byteが `#` の行、空行は長さ0の行だけを許可します。未知tagや既知tagの
不正値は読み飛ばさずエラーにします。

値の規則は次のとおりです。

- `i`: 10進の符号付き整数だけを許可し、`i32` 範囲外を拒否
- `f`: 符号、小数点、10進指数の厳密な文法だけを許可し、末尾junk、NaN、Inf、
  overflow、underflowを拒否
- `b`: 小文字の `true` または `false` だけを許可
- `s`: CR/LFとembedded NULを拒否
- key: 空、`=`、CR/LFを拒否

## 上限

| 項目 | 上限 |
|---|---:|
| ファイル・保存出力 | 4 MiB |
| entry数 | 4,096 |
| 1行 | 4,096 bytes |
| key | 255 bytes |
| string値 | 4,096 bytes |
| int / floatの入力表現 | 96 bytes |

上限はファイルを確保する前、または段階的な文字列追加の前に検査します。
加算は残容量との差分で判定し、`usize` wrapによる過少確保を避けます。

## 保存契約

`TrySave` は全entryを先に検証してから、上限付き `TryAppend` で文書を作ります。
非有限float、null string、長すぎる値、改行を含む値など、読み戻せない状態は
ファイルI/Oの前に拒否します。

保存は保存先と同じディレクトリに
`<path>.tmp.<pid>.<tid>.<serial>` を `CREATE_NEW` で作成します。衝突時は別serialで
最大32回再試行するため、別プロセスの一時ファイルをtruncateしません。全byteのwrite、
`FlushFileBuffers`、`CloseHandle` が成功した後だけ保存先を置換します。通常は
`MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`、保存先をreaderが
`FILE_SHARE_DELETE` で保持している場合は
`FileRenameInfoEx(REPLACE_IF_EXISTS | POSIX_SEMANTICS)` を使用します。

最終置換より前の失敗では既存ファイルは不変です。一時ファイルはbest effortで削除
します。最終置換が成功した後は、古いreaderは従来のfile objectを読み続け、新しい
readerは新しい完全な文書を読みます。

## エラー診断

`FSettingsPersistenceResult` は次を返します。

- `Error`: 数値を再利用しない安定した `ESettingsPersistenceError`
- `Line`: 構文・値エラーの1始まり行番号。ファイルI/Oでは0
- `Entries`: 成功済み検証・構築件数
- `OsError`: Win32 API失敗時の `GetLastError`

ログには `SettingsPersistenceErrorName` の固定名を使用できます。エラーenumへ値を
追加するときは末尾へ追加し、既存の数値を変更または再利用しないでください。
