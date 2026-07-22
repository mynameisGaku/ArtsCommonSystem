# FReplayDirector 永続化の安全契約

`FReplayDirector` はreplay file、metadata pointer、`FInputRecorder`、`FLockstep`を
信頼境界として扱う。保存と読込は例外に依存せず、上限検証、staging、atomic commitの順で行う。

## 公開上限

| 対象 | 上限 |
|---|---:|
| replay container | 256 MiB |
| recorder / lockstep blob | 各128 MiB |
| input samples / lockstep frames | 各1,000,000件 |
| tick rate | 1–1,000 Hz |
| replay path | 1,023 UTF-16 code units |
| game version | 64 bytes |
| level ID | 255 bytes |
| player name | 255 bytes |
| checksum | 空、または16桁ASCII hex |

上限判定とサイズ計算は `u64` で行い、`u32` / `usize` へ縮約する前に検証する。
pathとC文字列は公開上限までしか走査しない。

## container wire format

version 1のcontainerは次の完全一致形式である。全整数はlittle endian。

```text
[magic "ACRP":4][version:4]
[seed:8][timestamp:8][duration_ticks:4]
[game_version_len:4][game_version]
[level_id_len:4][level_id]
[player_name_len:4][player_name]
[checksum_len:4][checksum]
[input_blob_size:4][input_blob]
[lockstep_blob_size:4][lockstep_blob]
[crc32:4]
```

CRC32はfooterを除くcontainer全体を対象とする。loaderは次の順で検証する。

1. file sizeの正値、最小長、256 MiB上限
2. magicとversion
3. outer CRC
4. 数値metadataと各length-prefixed文字列
5. embedded NUL、field別長さ、checksum canonical form
6. blob別上限とcontainer残量
7. parse cursorがCRC直前と完全一致すること
8. inner blobのmagic、version、tick rate、件数、完全サイズ、inner CRC
9. 全staging allocation

途中まで正しいprefixの後ろにbyteを追加しCRCを再計算したfileも、cursor完全一致で拒否する。
最初の全byte read後にfile sizeを再取得し、現在位置から1 byteのEOF probeも行うため、
読み込み中の伸長・短縮を成功したsnapshotとしてcommitしない。

## transactional load

`TryLoadReplay` はfile snapshotをProcess Heapへ読み、directorが所有する文字列を一時
`FString`へ構築する。既存のowned文字列、metadata、mode、tickはここでは変更しない。

`FInputRecorder::TryLoadFromBuffer` と `FLockstep::TryLoadFromBuffer` は、
全wire検証後に同じallocatorを使う一時 `TArray` へ全recordを復元する。
`TryReserve` / `TryPushBack` のどちらかが失敗した場合、対象sourceの既存stateは不変である。

Replay loadでは、注入された2 sourceをそれぞれtemporary instanceへ読み込む。
各temporaryは置換先sourceと同じallocatorを使うため、commit後に短寿命allocatorへの参照を残さない。
両方が成功した後だけ、privateなno-fail `SwapLoadedState`で既存sourceへ一括commitする。
これにより「recorder成功後にlockstepがOOM」という順序でも、両sourceとdirector stateは変化しない。
swap対象は永続化stateだけで、各sourceのruntime modeは維持する。

最後にowned metadataをmoveし、pointerを新しいowned bufferへ張り直して
`Idle / current_tick=0` をcommitする。

既存 `LoadReplay` は互換wrapperとして `TryLoadReplay`へ委譲する。

## 検証付き recording metadata

`TryStartRecording` はmetadata pointerをbounded scanし、4文字列を一時領域へ複製してから
Recordingへ遷移する。callerの元bufferを後で変更してもdirector metadataには影響しない。
不正checksum、上限超過、OOM、誤modeでは既存sessionを変更しない。

既存 `StartRecording` は互換wrapperとしてchecked APIへ委譲する。

## 原子的 save

`TrySaveReplay` はsource件数からinner blobの正確な必要長を計算し、次を確認する。

- source `SaveToBuffer` が申告長を完全に書いたこと
- 生成されたinner blob自身のmagic、version、size、CRC
- metadataとcontainerの全上限
- container bufferを一度だけ確保し、offsetが計算済み総長と一致すること

diskへのcommit手順は次のとおり。

1. 保存先と同じdirectoryに `<path>.tmp.<pid>.<tid>.<nonce>` を組み立てる
2. `CREATE_NEW` でtempを作り、既存file/reparse pointを上書きしない
3. 全byteを書き、部分writeを拒否する
4. `FlushFileBuffers` の成功を確認する
5. `CloseHandle` の成功を確認する
6. `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` でatomic replaceする
7. 開いている旧snapshotがある場合はPOSIX rename semanticsをfallbackとして使う

write、flush、close、replaceのどこで失敗してもtempを削除し、既存保存先を変更しない。
`FInputRecorder` / `FLockstep` の `SaveToBuffer` も容量、全上限、保存対象floatが有限値で
あることを検証してから書き始める。loadもNaNと正負infinityを拒否する。
失敗時は `out_written=0` かつ出力buffer不変である。

既存 `SaveReplay` は互換wrapperとして `TrySaveReplay`へ委譲する。

## Tickの数値安全性

`Tick` は非正値、NaN、60秒を超える異常deltaを無視する。
tick加算はoverflow前に飽和またはdurationへclampし、巨大deltaからのfloat-to-integer変換や
`u32` wrapを発生させない。

## 回帰テスト

`tests/replay_director_safety_tests.cpp` は以下を検証する。

- metadataのowned copy、上限、checksum、失敗時state不変
- save/load round trip
- CRC改竄、切詰め、CRC再計算済みtrailing byte、過大metadata length
- 256 MiB超のsparse fileをallocation前に拒否
- lower source loadのOOM transactionとsave出力不変
- recorder staging成功後のlockstep OOMでも2 source/directorが全て不変
- readerがatomic replaceを拒む状況で既存replayが保持されること
- share-delete付きreaderが旧snapshotを保持し、pathからは新replayを読めること
