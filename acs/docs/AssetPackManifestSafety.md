# AssetPack マニフェストとレジストリの安全性

この文書は `.acpak` v1 の検証付き永続化境界と、`FAssetRegistry` が使う
loader 登録境界を定義します。

## archive commit 契約

`FAcpakWriter::Open()` は出力先 archive を切り詰めません。出力先 directory に
`CREATE_NEW` で一意な一時ファイルを作り、`Finalize()` は次の順で処理します。

1. payload と完全な manifest を書く。
2. retry で残り得る古い bytes を切り詰める。
3. canonical header を書き直す。
4. `FlushFileBuffers` を呼ぶ。
5. 開いたままの一時ファイルを replace + POSIX semantics の
   `FileRenameInfoEx` で公開する。
6. 公開済み handle を閉じる。

手順 6 より前の失敗では既存出力を変更しません。`Close()` または destructor は、
未公開の一時ファイルを削除します。置換成功時に既存の `FAcpakReader` が開いていても
安全です。その handle は旧 snapshot を保持し、新しい reader は置換後の内容を読みます。

reader/writer の OS path は UTF-16 code unit 1,023 個までです。空 path と上限超過は、
出力先を変更する前に拒否します。

## manifest 上限と canonical path

既存形式の次の上限を検証付き契約に含めます。

- entry は最大 1,048,576 件。
- virtual path は最大 4,096 UTF-16 code units。
- 所有する path pool storage は最大 256 MiB。
- offset/size の全計算は、allocation または I/O の前に減算形式で overflow を検証する。

canonical virtual path は空でない相対 path で、slash 区切りでなければなりません。
control character、backslash、colon、空 segment、`.`、`..`、埋め込み NUL、
対応しない UTF-16 surrogate を拒否します。writer と非信頼 manifest の reader は
どちらも重複 virtual path を拒否します。

entry data は固定 header より後、manifest より厳密に前になければなりません。
空でない data range 同士の overlap は禁止です。v1 の `padding` と `reserved` は
0 である必要があり、解析済み manifest は EOF で正確に終わらなければなりません。
これにより未知の schema extension を誤って v1 として解釈しません。

## transactional reader

`FAcpakReader::Open()` はまず完全な reader を staging に構築します。magic、version、
flags、reserved fields、件数、path pool、entry range、重複、正確な EOF を検証してから
staged state を commit します。allocation failure や不正入力があっても、それまで
開いていた archive は引き続き利用できます。

reader は manifest 解析の前後で file identity、size、last-write time を snapshot します。
変化は `kAcpakSubFileChanged` として報告します。handle は read/delete sharing で開き、
POSIX rename semantics により既存 snapshot を無効化せず新しい path を公開します。

256 KiB 以上の archive は同じ検証済み handle から read-only mapping を試します。
mapping 失敗時は archive open を失敗させず、従来の `ReadFile` 経路へ戻ります。
atomic replace 後も mapping は旧 handle の snapshot を保持します。`Close()` は view、
mapping handle、file handle の順で閉じます。

raw mapped entry は CRC 成功後だけ caller buffer へ copy します。圧縮/暗号化 entry は
最終 scratch 上で復号・展開・CRC を完了してから commit します。reader が保持する
stored/final scratch は各 16 MiB までで、競合または上限超過時は呼び出し局所 scratch
へ戻ります。

`ReadFiles()` は最大 1,024 entry を要求順に一つの lifecycle lock 境界で読みます。
all-or-nothing ではなく、後続失敗時も先行出力は commit 済みです。任意の
`CompletedCount` で成功済み件数を取得できます。診断 snapshot/reset は進行中 read の
完了境界を作ってから mapped/buffered/scratch/batch counter を集約または初期化します。

安定した manifest error subcode は次のとおりです。

- `kAcpakSubBadPath`
- `kAcpakSubDuplicatePath`
- `kAcpakSubBadSchema`
- `kAcpakSubFileChanged`
- `kAcpakSubAtomicReplace`
- 既存の `BadMagic`、`BadVersion`、`BadFlags`、`BadSize`、`OutOfMemory` と
  I/O classification。

## FAssetRegistry の検証境界

`FAssetRegistry::TryRegisterLoader()` は null loader、extension 文法、重複 extension、
shutdown 状態、allocation failure を検証します。従来の `RegisterLoader()` は
互換 wrapper として残します。

registry path は UTF-16 code unit 1,023 個までです。同期/非同期 load は同じ検証を使い、
非同期 job は受理した完全な path を所有します。切り詰め前の入力から ID を作りながら
path だけを黙って切り詰めることはありません。cache 挿入も allocation を検証し、
fail-fast container 操作の代わりに安定した OOM error を返します。

新規 async job の path は最大 256件/65,536 code unit の
`FAssetPathInterner` で共有します。sync load、cache hit、in-flight hit は interner を
通らず、従来 hot path に lock/allocation を追加しません。pool は未使用 path だけを
evict し、全要素が使用中なら非保持の共有 path を返すため job lifetime を短縮しません。
DDC owner は現時点で存在しないため、この pool を DDC 名目で別 lifetime に流用しません。

## テスト

`tests/assetpack_manifest_safety_tests.cpp` は次を検証します。

- open 失敗時の state 不変。
- 埋め込み NUL、重複、traversal、reserved schema の拒否。
- 不正 `AddFile` 後の pending list 不変。
- 旧 reader を開いたままの原子的置換。
- large archive の旧 mapped view と置換後の新 reader の byte parity。
- mapped CRC 失敗時の caller buffer 不変、batch の部分完了数。
- 圧縮 scratch の上限、再利用、4 並行 read の byte parity。
- finalize 前に中断した場合の既存 archive 保護。
- 検証付き loader 登録、extension 重複、OOM。
- 同期/非同期の path 上限超過を切り詰めず拒否すること。
- async path pool の上限、pin 中の寿命、cache/in-flight hit の非介入、shutdown 競合。
