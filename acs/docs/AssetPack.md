<!-- SPDX-License-Identifier: Apache-2.0 -->
# ACS AssetPack

`AssetPack` は、ACS の virtual path と byte payload を `.acpak` 一ファイルへ保存し、
検証付きで読み戻す runtime module である。圧縮と暗号化は archive 作成時の
option であり、mount、key 設定、GameFramework provider の結線は利用側が
明示的に行う。

## モジュール境界

`src/assetpack/AssetPack.Build.cs` が正規の source manifest であり、生成された
`src/assetpack/Module.cmake` が `ACS::AssetPack` を定義する。公開依存は次の ACS module である。

- `Foundation`、`Container`、`Memory`、`Platform`、`Threading`
- `GameFramework`

Windows の AES-GCM、PBKDF2、乱数生成に `Bcrypt` を private link する。LZ4 block
codec は `CAcpakLz4` に実装しており、別の LZ4 library には依存しない。

| 型 | 責務 |
|---|---|
| `CAcpakWriter` | archive 構築、圧縮、暗号化、原子的な公開 |
| `CAcpakReader` | header/table 検証、path 検索、復号、展開、CRC 検証 |
| `CAcpakCrypto` | AES-256-GCM、PBKDF2-HMAC-SHA256、nonce 生成 |
| `CAcpakLz4` | 境界検証付き LZ4 block 圧縮・展開 |
| `CAcpakGameReader` / `CAcpakGameWriter` | GameFramework interface との UTF-8/UTF-16 bridge |

`CApplication` が所有する `CAssetRegistry` と AssetPack は別の境界である。AssetPack は
registry の下に VFS を挿入せず、loose file と pak の自動切替えも行わない。

## `.acpak` v1 format

数値は little-endian で保存する。先頭は 36 byte 固定 header である。

| offset | size | 項目 | 契約 |
|---:|---:|---|---|
| 0 | 8 | magic | `ACPAK\0\0\0` |
| 8 | 4 | version | `kAcpakVersion` = 1 |
| 12 | 4 | flags | `Encrypted` と `Compressed` の bit |
| 16 | 4 | file count | table の entry 数 |
| 20 | 4 | padding | v1 では 0 |
| 24 | 8 | file table offset | archive 先頭から table までの byte offset |
| 32 | 4 | reserved | v1 では 0 |

header の後に各 entry の stored payload を置き、末尾に可変長 file table を置く。
各 table entry は次の順である。

1. `u32 path_length`
2. NUL を含まない UTF-16 virtual path
3. `u64 offset`
4. `u64 size_uncompressed`
5. `u64 size_stored`
6. 展開後 payload の `u32 crc32`
7. 暗号化 archive の場合だけ 12 byte nonce と 16 byte GCM tag

virtual path と entry metadata は平文である。path hash は disk へ保存せず、reader が
table 読み込み時に保持する。検索は case-preserving の UTF-16 path 完全一致である。

### virtual path

virtual path は `/` 区切りの相対 path で、次を拒否する。

- 空 path、先頭または末尾の `/`、空 segment。
- `.` / `..` segment、backslash、colon、control character、embedded NUL。
- 対応しない UTF-16 surrogate。
- 4,096 code unit を超える path。

archive 内の同一 path は重複であり、writer と reader の両方が拒否する。
一 archive の上限は 1,048,576 entry、reader の path pool 上限は 256 MiB である。

## writer

`CAcpakWriter` の lifecycle は `Open()`、`AddFile()`、`Finalize()`、`Close()` である。

- `Open()` は出力先と同じ directory に一意な一時ファイルを作り、仮 header を書く。
- `AddFile()` は path と payload を pending entry へコピーする。成功後、呼び出し側は
  入力 memory を再利用できる。
- `Finalize()` は pending entry を順番に処理し、payload、table、確定 header を書く。
- 書き込みと `FlushFileBuffers` が成功した後だけ、一時ファイルを出力先へ
  原子的に置換する。
- `Close()` または destructor は未公開の一時ファイルを削除し、pending data と key を破棄する。

`Compressed` flag が立つ場合、全 entry を `CAcpakLz4` に通す。entry ごとの raw
fallback はなく、圧縮後の方が大きい場合もその byte 列を保存する。`Encrypted` も
立つ場合は compress-then-encrypt の順で処理する。CRC32 は圧縮前の元 payload から計算する。

原子的公開、manifest 上限、安定 snapshot の契約は
[AssetPackManifestSafety.md](AssetPackManifestSafety.md) に定める。

## reader

`CAcpakReader::Open()` は新しい reader state を staging し、header、table、path、entry range、
schema をすべて検証してから公開する。新しい archive を開けない場合は、
それまで開いていた handle、manifest、key を変更しない。

table 読み込みの前後で file identity、size、last-write time を比較する。変化を
検出した場合は `kAcpakSubFileChanged` を返す。256 KiB 以上の archive では
read-only mapping を試し、mapping 失敗時は handle read へ戻る。

`ReadFile()` は必要な場合に次の順で処理する。

1. stored payload を mapping または handle から読む。
2. AES-256-GCM tag を検証して復号する。
3. LZ4 を展開し、展開 byte 数を検証する。
4. 最終 plaintext の CRC32 を検証する。
5. 成功した最終 payload だけを caller buffer へコピーする。

`ReadFiles()` は最大 1,024 entry を要求順に読む。all-or-nothing ではなく、後続 entry の
失敗時も先行出力は commit 済みである。`CompletedCount` は成功済み件数を返す。

`FRwLock` が Open/Close と read の寿命を分離し、I/O cursor と保持 scratch は個別の
lock で保護する。保持 scratch は stored/final それぞれ 16 MiB までとし、競合または
上限超過時は呼び出し局所領域を使う。diagnostics は mapped/buffered read、scratch、
batch の完了値を snapshot/reset で扱う。

## 圧縮と暗号化

`CAcpakLz4` は LZ4 block を自己完結で圧縮・展開する。展開は source cursor、
destination capacity、match offset、最終 byte 数を検証し、不正 block で範囲外アクセスを
行わない。

`CAcpakCrypto` は次の契約を持つ。

- 32 byte key を使う AES-256-GCM。
- entry ごとに CSPRNG が生成する 12 byte nonce と、16 byte 認証 tag。
- CSPRNG 失敗時は nonce を 0 にして失敗を返し、暗号化を続けない。
- GCM の追加認証データは使わない。path と table は GCM の認証範囲外である。
- `DeriveKey()` は password と salt から PBKDF2-HMAC-SHA256、10,000 回で key を作る。
  salt は `.acpak` v1 header には保存しない。

writer/reader の key は呼び出し側が `SetKey()` で渡す。暗号化 archive は key なしで
Open できるが、payload read は `kAcpakSubCryptoKey` で失敗する。CLI は 64 hex 文字の
raw key を `--key-file` または `--key` から受け取る。

暗号化は payload をそのまま閲覧することを防ぎ、entry の認証 tag で改変を検出する。
virtual path、件数、size、offset、CRC は隠さない。key の保存・配布は利用側の責務であり、
AssetPack 自体は key provider や利用側からの秘匿を提供しない。

## GameFramework bridge

GameFramework は `IAssetPackReader` / `IAssetPackWriter` と provider 登録を定義する。
provider 未登録時は stub が `kSubAssetPackNotImplemented` を返す。

`CAcpakGameReader` と `CAcpakGameWriter` はこれらの interface を実装する。

- `InstallAcpakReaderAsDefault()` / `InstallAcpakWriterAsDefault()` は process 共有の
  default backend を登録する。自動登録はしない。
- reader は `Mount()` / `Unmount()`、writer は `BeginPack()` / `FinishPack()` で lifecycle を
  開始・終了する。自動 mount はしない。
- bridge reader は UTF-8 path を UTF-16 へ変換し、`FileName()` が返す UTF-8 string を
  次の Mount/Unmount まで保持する。
- bridge writer の `BeginPack()` は `AcpakFlagNone` を使う。圧縮または暗号化した
  archive は core `CAcpakWriter` または `acs_assetpack` で作る。
- bridge reader は key 設定 API を公開しない。暗号化 archive の読み込みは
  key を設定できる `CAcpakReader` または別の `IAssetPackReader` 実装を使う。

`SceneTextLoader` は `IAssetPackReader` を受け取る overload を持つ。利用側が mount 済み
reader と virtual path を渡し、scene と参照 asset を同じ pack から読み込む。

## `acs_assetpack`

`tools/acs_assetpack` は `.acpak` v1 を操作する Windows CLI である。

```text
acs_assetpack pack   <input_dir> <output.acpak> [--compress] [--encrypt (--key-file <path> | --key <hex64>)]
acs_assetpack unpack <input.acpak> <output_dir> [--key-file <path> | --key <hex64>]
acs_assetpack list   <input.acpak>
acs_assetpack verify <input.acpak> [--key-file <path> | --key <hex64>]
acs_assetpack info   <input.acpak>
acs_assetpack help   [subcommand]
```

`pack` は入力 directory を再帰的に読み、virtual path 昇順に sort してから追加する。
入力 directory がない、空である、option が矛盾する、key が不正な場合は失敗する。
exit code は成功 0、usage error 1、runtime error 2 である。

packaging 全体の cook、verify、staging 契約は [Packaging.md](Packaging.md) に定める。

## 失敗条件

次を成功として公開しない。

- magic、version、flag、padding/reserved、table 終端が v1 schema と一致しない。
- file count、path pool、offset/size 計算、entry range が上限または archive 範囲を超える。
- path が正規形でない、重複する、entry data が重なる。
- Open 中に file snapshot が変化する。
- key がない、GCM tag、LZ4 境界、展開後 size、CRC32 が一致しない。
- caller buffer が小さい、allocator または I/O が失敗する。
- 完成済み一時ファイルを出力先へ原子的に置換できない。

reader は Open 失敗時に公開済み state を変更せず、個々の `ReadFile()` 失敗で対応する
caller buffer を変更しない。`ReadFiles()` の先行成功 entry は commit 済みである。writer は
Finalize の公開境界より前の失敗で既存出力を置換しない。詳細な error subcode は
`AcpakFormat.h`、`AcpakCrypto.h`、`AcpakLz4.h` を正規定義とする。

## 検証

次は `acs/` を作業 directory として実行する。

```powershell
python -B scripts\audit_cpp_type_roles.py --root src
python -B scripts\audit_cpp_conventions.py --root .
python -B scripts\audit_module_sources.py --root .
python -B scripts\amalgamate.py --check
cmake --build Intermediate\vs --config Debug --target acs_assetpack acs_unit_tests
cmake --build Intermediate\vs --config Release --target acs_assetpack acs_unit_tests
ctest --test-dir Intermediate\vs -C Debug -R "^ACS.UnitTests$" --output-on-failure
ctest --test-dir Intermediate\vs -C Release -R "^ACS.UnitTests$" --output-on-failure
```

focused coverage では format round trip、圧縮、暗号化、tag/CRC 改変、path/table 上限、
Open 失敗時の state 不変、caller buffer 不変、原子的置換、並行 read/unmount、
GameFramework bridge の lifetime を確認する。
