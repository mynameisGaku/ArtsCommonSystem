# Asset Pack の利用

Asset Pack は、複数のアセットを 1 つの `.acpak` へ格納する ACS のアーカイブ形式です。実装は `src/assetpack/`、CLI は `tools/acs_assetpack/` にあります。

## 形式

`.acpak` v1 の先頭ヘッダーはディスク上 36 バイトです。C++ 構造体の `sizeof` は整列用領域により異なる可能性があるため、読み取り側と書き込み側は `kAcpakHeaderDiskSize` を使って項目単位に読み書きします。

v1 のヘッダーは 36 バイトで固定です。暗号化されたファイル表や 64 バイトのヘッダーは使いません。
AES-GCM の AAD は長さ 0 であり、仮想パスやサイズを追加認証データとして渡しません。

| 項目 | バイト数 | 内容 |
|---|---:|---|
| `magic` | 8 | `ACPAK\0\0\0` |
| `version` | 4 | `kAcpakVersion`、現在は1 |
| `flags` | 4 | 圧縮、暗号化などのビット |
| `file_count` | 4 | 項目数 |
| `padding` | 4 | v1 では0 |
| `file_table_offset` | 8 | ファイル表の開始位置 |
| `reserved` | 4 | v1 では0 |

各項目は仮想パス、データ位置、格納サイズ、展開サイズ、CRC32 を持ちます。暗号化時は 12 バイトのノンスと 16 バイトの認証タグを追加します。

## 読み取り

```cpp
acs::assetpack::CAcpakReader reader;
auto open_result = reader.Open(L"Content.acpak");
if (!open_result) {
    return open_result.Error();
}

const auto size_result = reader.GetUncompressedSize(L"textures/ui/logo.png");
if (!size_result) {
    return size_result.Error();
}
```

暗号化されたアーカイブは `Open` 後、対象データを読む前に `SetKey` で `FAcpakKey` を設定します。`ReadFile` は出力領域が不足している場合に失敗し、必要なサイズは `GetUncompressedSize` で取得できます。

## 書き込み

```cpp
acs::assetpack::CAcpakWriter writer;
auto open_result = writer.Open(L"Content.acpak", acs::assetpack::AcpakFlagNone);
if (!open_result) {
    return open_result.Error();
}

auto add_result = writer.AddFile(L"config/game.json", data, data_size);
if (!add_result) {
    return add_result.Error();
}

return writer.Finalize();
```

`Finalize` はファイル表とヘッダーを確定し、完成した一時ファイルを出力先へ置換します。`Finalize` が成功する前の出力を完成品として公開しません。

## CLI 操作

`ACS_BUILD_TOOLS=ON` で `acs_assetpack` をビルドできます。次の例は ACS リポジトリルートで `dx12-debug` プリセットを構成し、CLIの実行ファイルだけをビルドします。

```pwsh
cmake --preset dx12-debug -S .\engine
cmake --build .\engine\cmake-build-debug --target acs_assetpack_cli

$AssetPackExe = ".\Intermediate\layout\dx12-debug\Binaries\acs_assetpack.exe"
& $AssetPackExe pack Content Content.acpak --compress
& $AssetPackExe pack Content Content.acpak --compress --encrypt --key-file content.key
& $AssetPackExe list Content.acpak
& $AssetPackExe verify Content.acpak
& $AssetPackExe unpack Content.acpak Unpacked
& $AssetPackExe info Content.acpak
```

`diligent-debug` プリセットを使う場合は、構成先を `.\engine\cmake-build-diligent-debug`、実行ファイルを `.\Intermediate\layout\diligent-debug\Binaries\acs_assetpack.exe` に読み替えます。Visual Studioの複数構成ジェネレーターで `ACS_LAYOUT_ROOT` を指定していない場合、Debug版の実行ファイルは `.\Binaries\Debug\acs_assetpack.exe` です。

`--encrypt` は AES-256-GCM を使用し、256 ビットの鍵が必要です。`--compress` は LZ4 を使用し、圧縮後の大きさにかかわらず全項目を LZ4 形式で格納します。

暗号化する各項目には、システムの暗号学的乱数源から新しい 12 バイトのノンスを生成します。
ノンスの生成に失敗した場合は暗号化とアーカイブの確定を中止します。書き込み側と読み取り側が保持する
鍵の内部コピーは `Close` または破棄時にゼロ化されます。LZ4 と AES-GCM の単一項目は
`0xFFFFFFFF` バイト以下に制限され、上限を超える項目は処理前に拒否されます。

詳細は[Asset Pack の暗号化と圧縮の安全契約](../../safety/assets/asset-pack-crypto.md)を参照してください。

## 仮想パス

仮想パスはアーカイブ内の識別子です。空文字、埋め込み NUL、`.`、`..`、不正な区切り、重複パスは拒否されます。CLI の入力走査は再解析ポイントを受理しません。展開時は正規化済みの相対パスだけを出力ディレクトリの内側へ解決します。

## 防御上限

| 定数 | 上限 |
|---|---:|
| `kAcpakMaxFileCount` | 1,048,576項目 |
| `kAcpakMaxPathLength` | 4,096 UTF-16コード単位 |
| `kAcpakMaxPathPoolBytes` | 256 MiB |
| `kAcpakMaxOutputPathLength` | 1,023 UTF-16コード単位 |

CRC32、位置、サイズ、未知フラグ、予約項目、ファイル同一性の変化を読み取り側が検証します。詳細は[Asset Pack Manifest の安全契約](../../safety/assets/asset-pack-manifest.md)と[パス正規化](../../safety/assets/path-normalization.md)を参照してください。
