# FSaveArchive / TSaveSlot 永続化境界 V2

この文書は `FSaveArchive` と `TSaveSlot<T>` の追加安全契約を定義します。
既存の `.acssave` 形式（24 byte header、payload、CRC32）と既存 API は変更しません。

## 検証付き API

### `FSaveArchive::ValidateFile`

`ValidateFile(path)` は payload を呼び出し側へ公開せず、次の検証を一つの
file-object snapshot 上で完了してから `FSaveArchiveMetadata` を返します。

1. path が非 null・非空で、32767文字以下
2. header が24 byte以上あり、magic が `ACSSAVE\0`
3. `payload_size <= 256 MiB`
4. `file_size == 24 + payload_size`
5. payload全体を64 KiB単位で完全読み込み
6. headerのCRC32と実payloadのCRC32が一致
7. file handleのcloseに成功

検証は固定サイズのstack bufferを使うため、敵対的な `payload_size` に比例する
heap確保を行いません。成功結果には `Version`、`PayloadSize`、`PayloadCrc32` が
含まれます。versionの採否は上位schemaの責務であり、特定versionを要求するロードには
従来どおり `ReadFromFile(..., expected_version, ...)` を使います。

### `TSaveSlot<T>::TryInit`

従来の `Init(path)` は互換性のため非所有ポインタを保持します。新しい
`TryInit(path)` はパスをslot内へコピーし、呼び出し側の一時文字列を安全に使えます。

- null、空、32767文字超過を診断
- OOM時は `kSubAllocationFailed`
- 検証・確保・コピーをstaging領域で行い、成功時だけcommit
- 失敗時は以前のpath pointerとownershipを変更しない
- `IsPathOwned()` で現在のmodeを確認可能
- allocator指定constructorによりOOMを決定論的にテスト可能

## 読み込みのtransaction境界

`ReadFromFile` はpayloadを一時領域へ読み、サイズ、version、CRC、closeの全成功後に
のみ呼び出し側bufferへコピーします。`TSaveSlot<T>::Load` も別の一時bufferを使い、
`.acssave` payload sizeが `sizeof(T)` と完全一致した後だけ結果を返します。

したがってpartial read、CRC不一致、version不一致、size不一致、OOM、close失敗では
呼び出し側bufferや既存game stateを変更しません。binary payload内のNULは正当な値です。
pathはNUL終端APIであるため、最初のNULがpath終端になります。

読み取りhandleは `FILE_SHARE_READ | FILE_SHARE_DELETE` で開きます。in-place writerは
拒否され、atomic replaceは許可されます。読み込み途中に新しいsaveがpublishされても、
開いているhandleは置換前の一貫したfile objectを最後まで参照します。

## 書き込みとatomic replace

書き込みは保存先と同じdirectoryの一時ファイルを `CREATE_NEW` で作成し、headerと
payloadの完全書き込み、`FlushFileBuffers`、`CloseHandle` の後にのみ置換します。
通常は後方互換の `<path>.tmp.<pid>.<tid>` を最初に試し、クラッシュで残った通常
ファイルと衝突した場合はatomic nonce付き候補へ最大7回retryします。

既存objectやreparse pointは追跡・truncateしません。通常の名前衝突だけをretryし、
access deniedなどそれ以外のcreate失敗はfail closedになります。
publishは `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` を使い、既存readerが
`FILE_SHARE_DELETE` でsnapshotを保持している場合はPOSIX rename semanticsへ
fallbackします。write、flush、close、replaceのどこで失敗しても既存saveは維持されます。

## format・schema・overflow

- magic: 8 byte `ACSSAVE\0`
- version: little-endian `u32`
- payload size: little-endian `u64`、最大256 MiB
- CRC32: payloadのみ、poly `0xEDB88320`
- payload: binary。embedded NULを含めてよい
- file sizeは加算によるoverflowを避け、`payload_size == file_size - 24` で検証
- `TSaveSlot<T>` はtrivially-copyableかつ256 MiB以下をcompile-time検証
- POD内部のpointer、padding、endianness、独自件数・文字列schemaは上位型の責務
- schema変更時はversionを増やし、`kSubMigrationNeeded` からmigrationへ分岐

## 診断

既存の `ESaveArchiveSubCode` を共通利用します。代表値は次のとおりです。

- `kSubBadMagic`: header不足またはmagic不一致
- `kSubMigrationNeeded`: expected version不一致
- `kSubBufferTooSmall`: 出力容量またはPOD size不一致
- `kSubChecksumFail`: CRC不一致
- `kSubSizeMismatch`: truncationまたはtrailing bytes
- `kSubPayloadTooLarge`: 256 MiB超過
- `kSubPathTooLong`: path上限超過
- `kSubAllocationFailed`: transactional bufferまたはowned pathのOOM
- `kSubInvalidArgument`: null/空path、null bufferの契約違反
- `kSubIoError`: open/read/write/flush/close/replace失敗

## 専用回帰テスト

`tests/savegame_archive_safety_v2_tests.cpp` は次を固定します。

- embedded NULを含むbinary payloadの完全検証とmetadata
- CRC改竄、trailing byte、256 MiB超のsize申告、長すぎるpathの拒否
- stale temp collisionからnonce付き候補への安全なretry
- `TryInit` のpath所有と一時bufferからの独立
- allocator注入OOM時の以前のpath/pointer不変
- 空pathをpayload参照より前に拒否

テスト登録時は `tests/CMakeLists.txt` の `acs_unit_tests` sourceへ
`savegame_archive_safety_v2_tests.cpp` を追加してください。また文書索引には
本ファイルを追加してください。
