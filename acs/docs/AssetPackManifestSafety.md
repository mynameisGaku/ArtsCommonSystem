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

## テスト

`tests/assetpack_manifest_safety_tests.cpp` は次を検証します。

- open 失敗時の state 不変。
- 埋め込み NUL、重複、traversal、reserved schema の拒否。
- 不正 `AddFile` 後の pending list 不変。
- 旧 reader を開いたままの原子的置換。
- finalize 前に中断した場合の既存 archive 保護。
- 検証付き loader 登録、extension 重複、OOM。
- 同期/非同期の path 上限超過を切り詰めず拒否すること。
