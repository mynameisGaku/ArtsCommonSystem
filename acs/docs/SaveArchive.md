# FSaveArchive 安全性契約

`FSaveArchive` は `.acssave` の固定ヘッダー、payload、CRC32 を扱う低レベルAPIです。
外部入力を直接扱うため、成功時だけ状態を反映する transactional な境界として設計されています。

## ファイル形式

すべて little-endian です。

| offset | size | field | 内容 |
| ---: | ---: | --- | --- |
| `0x00` | 8 | magic | `ACSSAVE\0` |
| `0x08` | 4 | version | 呼び出し側が管理する schema version |
| `0x0C` | 8 | payload_size | payload のバイト数 |
| `0x14` | 4 | crc32 | payload の CRC-32 |
| `0x18` | N | payload | 保存対象のバイト列 |

`payload_size` は実ファイルサイズからヘッダー24バイトを除いた値と完全一致する必要があります。
短いファイルだけでなく、payload末尾へデータが付加されたファイルも拒否します。

## 読み込み契約

`ReadFromFile` は次の検証を完了した後にだけ `out_payload` を変更します。

1. magic と固定ヘッダー長
2. payload の256 MiB安全上限
3. 申告サイズと実サイズの完全一致
4. schema version
5. 出力容量
6. payload の完全読み込み
7. CRC32
8. ファイルハンドルの正常終了

payload は検証用の一時領域へ読み込まれます。CRC不一致、途中EOF、確保失敗など、
どのエラーでも呼び出し側の既存オブジェクトやバッファは部分変更されません。

version不一致と容量不足では、移行や再確保に使えるよう `out_payload_size` に検証済みの
payloadサイズを返します。ヘッダーやサイズ整合性を検証できなかった場合は0のままです。

## 書き込み契約

`WriteToFile` は最終パスを直接 truncate しません。

1. `<path>.tmp.<process-id>.<thread-id>` を `CREATE_NEW` で同一ディレクトリに作成
2. ヘッダーとpayloadを完全書き込み
3. `FlushFileBuffers`
4. `CloseHandle`
5. `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` で最終パスを置換
6. 旧ファイルを開いた reader が置換を妨げる環境では
   `FileRenameInfoEx(REPLACE_IF_EXISTS | POSIX_SEMANTICS)` へフォールバック

置換より前に失敗した場合、既存の保存ファイルは保持されます。失敗した一時ファイルは
best-effortで削除されます。同一ディレクトリを使うため、最終置換はvolumeを跨ぎません。
`CREATE_NEW` により、既存の一時ファイルやreparse pointを追跡してtruncateすることもありません。
読み込みhandleは `FILE_SHARE_DELETE` を許可するため、読み込み中のhandleは置換前の一貫した
file objectを参照したまま、並行するatomic replaceを妨げません。POSIX semanticsが未対応の
Windows/filesystemでは安全にI/Oエラーを返し、既存ファイルを保持します。

## 安全上限と診断

| subcode | 意味 |
| --- | --- |
| `kSubBadMagic` | magic不一致、または固定ヘッダー未満 |
| `kSubMigrationNeeded` | version不一致 |
| `kSubBufferTooSmall` | 出力容量不足 |
| `kSubChecksumFail` | CRC32不一致 |
| `kSubPayloadTooLarge` | payloadが256 MiB上限を超過 |
| `kSubSizeMismatch` | 申告payloadサイズと実ファイルサイズが不一致 |
| `kSubPathTooLong` | atomic write用suffixを含めるとパス長超過 |
| `kSubAllocationFailed` | transactional readまたはatomic write path用一時領域の確保失敗 |
| `kSubInvalidArgument` | null path/payload/outputなどAPI事前条件違反 |
| `kSubIoError` | Win32 I/O、flush、close、atomic replaceの失敗 |

呼び出し側は `FErrorCode.subcode` を分岐に使い、`os_error` がある場合は診断ログへ残してください。

## 上位APIへの伝播

### TSaveSlot

`TSaveSlot<T>` は `T` がtrivially-copyableで、256 MiB上限以下であることをコンパイル時に
検証します。Load helperは型サイズと同じ一時bufferへ読み、archive検証と型サイズ完全一致の
後にだけ対象へコピーします。小さい旧schemaを大きい型へ読み込んだ場合も、失敗時に対象objectの
先頭だけが更新されることはありません。

version不一致、CRC不一致、payload上限、確保失敗などの `FSaveArchive` subcodeはそのまま
伝播します。slot未初期化は従来どおり100番台の固有診断です。削除は競合による
`ERROR_FILE_NOT_FOUND` / `ERROR_PATH_NOT_FOUND` もべき等成功として扱います。

### FProgression

`FProgression` はさらに厳しいdomain制約を持ちます。

- milestoneは最大4096件
- payload長は `8 + count * 16` と完全一致
- `achieved` は0/1、padは0、未達成timestampは0
- 同じFNV-1a hashの重複entryと登録時hash collisionを拒否
- 保存サイズ計算、payload/staging確保はchecked処理

LoadはXPと全milestone状態を別bufferへstagingし、全entryの検証後にだけ一括反映します。
version、CRC、内部schema、重複、OOMのどの失敗でも現在の進捗は変化しません。
archive由来のsubcodeはそのまま伝播し、Progression内部schema固有の診断は
`EProgressionPersistenceSubCode` の200番台を使います。
