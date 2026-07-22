# ACS AssetPack モジュール 設計書

製品化に向けた**アセットのパッケージング + 暗号化**を担う新エンジンモジュール。
開発中はバラのファイル、出荷時は 1 つの暗号化アーカイブにまとめ、ゲームコードを
一切変えずに切り替えられる。`GameFramework`（[[project_gameframework]] /
`docs/GameFramework.md`）の Pillar G から利用する。

- モジュール: `src/assetpack/` ／ ターゲット `ACS::AssetPack`
- 依存: `Foundation Memory Container Threading Platform` + `bcrypt`（Windows CNG）+ `lz4`
- 規約: STL 不使用 / 例外不使用 / `noexcept` / `TResult<T,E>` / Windows 専用

---

## 1. 目的

商用ゲームを出荷したとき、インストールフォルダを開いた利用者が画像・音声・データを
**そのまま取り出せない**ようにする。`textures/hero.png` のようなバラ配置をやめ、
1 個の不透明な `.acpak` にまとめ、暗号化・圧縮・完全性検証を施す。

---

## 2. セキュリティモデル（正直な前提）

**クライアント側のアセット暗号化は本質的に「難読化」であり「セキュリティ」ではない。**
これは本設計の限界ではなく数学的事実である — ゲームはアセットを表示するために
復号せねばならず、ゆえに鍵は必ずプレイヤーのマシン上、ゲーム自身のコードから
届く場所に存在する。デバッガ・メモリダンプ・CNG フックで、本気の解析者は必ず鍵を
得られる。Denuvo 等の数百万ドル規模の対策すら破られる以上、ゲームフレームワークの
アーカイブ暗号化がそれを上回ることはない。**これをユーザーに正直に伝える。**

**この設計が実際に達成すること（現実的な脅威モデル）:**
- インストールフォルダを覗いた利用者には、`.acpak` 1 個の不透明なノイズしか見えない。
  最頻の「アセット流出経路」を完全に塞ぐ。
- 汎用の吸い出しツール（AssetStudio、QuickBMS 等）は既知フォーマットを狙う。独自
  `.acpak` + 暗号化 TOC はどの公開シグネチャにも一致せず、フォーマット固有・鍵固有の
  抽出器（まだ存在しない）を書かない限り何も得られない。
- 流出のハードルを「右クリック→コピー」から「バイナリを解析する」へ引き上げる。
  日和見的な窃取をふるい落とす大きな段差。
- 改竄検知（GCM 認証タグ）— 鍵を持たない者による差し替え MOD を検知できる。
- アーカイブ内のアセット一覧すら隠す（暗号化 TOC / ハッシュ化パス）。

**達成しないこと（明記する）:** 本気のリバースエンジニアは止められない。DRM でも
海賊版対策でもない。実行時にメモリ上で復号されたアセットのダンプは防げない。

→ ドキュメントでは「アセット難読化 / カジュアル流出対策」と表現し、「アセットを
守る暗号化」とは絶対に書かない。

---

## 3. `.acpak` アーカイブ形式

レイアウト: **[ヘッダ] · [TOC] · [パスヒープ(任意)] · [ブロブ領域]**。多バイト整数は
リトルエンディアン。読み手は構造体を `reinterpret_cast` せずフィールド単位で解釈する
（strict-aliasing 回避、`Hash.cpp` の流儀）。拡張子は `.acpak`。

### 3.1 ヘッダ（64 バイト固定・非暗号化）
`magic("ACSPAK\0\0")` / `format_version`(u16) / `min_reader_version`(u16) /
`archive_flags`(u32) / `toc_offset`(u64) / `toc_count`(u32) / `toc_stride`(u32, v1=64) /
`toc_plain_size`(u64) / `blob_offset`(u64) / `blob_size`(u64) / `header_hash`(u64)。
ヘッダは読み手が他の何をするにも先に必要で、機密を漏らさないので非暗号化。
`min_reader_version` で将来の互換破壊を即時・明示的に弾く。

### 3.2 TOC（`toc_count` 件 × `toc_stride`(64) バイト）
各レコード: `path_hash`(u64=`HashBytes`(正規化パス)) / `blob_offset`(u64, ブロブ相対) /
`stored_size`(u64, 圧縮+暗号化後) / `original_size`(u64, 復号後＝バッファ事前確保用) /
`content_hash`(u64, 元データのハッシュ) / `entry_flags`(u32) / `path_offset`+`path_length`
(パスヒープへのスライス。ハッシュのみモードでは無効値) / `asset_type_hint`(u32) /
`crypto_nonce`(96bit, AES-GCM ノンス)。
**レコードは `path_hash` 昇順ソート** → 実行時は分岐予測の効く二分探索 O(log n)、
アロケーション無し。

### 3.3 パスヒープ（任意・`ACPAK_KEEP_PATHS` 時のみ）
UTF-8 パス文字列の連結。出荷既定では**省略**し、全レコードを「ハッシュのみ」に
する → パスは一方向ハッシュからは復元不能（暗号化より強い）。存在する場合は
TOC と同じ暗号化エンベロープに含める。

### 3.4 ブロブ領域
エントリ毎ペイロードの連結。各エントリは **compress-then-encrypt** 順（§5）。
16 バイト境界整列。GCM 認証タグ 16 バイトは各エントリの `stored_size` の末尾に含む。

### 3.5 フラグ
`archive_flags`: `TOC_ENCRYPTED` / `KEEP_PATHS` / `BLOB_ENCRYPTED` / `OBFUSCATED_BLOB`。
`entry_flags`: `COMPRESSED` / `ENCRYPTED` / `STORE_RAW`。

### 3.6 TOC は暗号化する（既定で）
TOC を暗号化すると、`.acpak` をバイナリエディタで見ても 64 バイトのヘッダの後は
高エントロピーのノイズのみ — どんなアセットが入っているか名前すら列挙できない。
出荷ビルドでは TOC 暗号化＋パスはハッシュのみ。開発ビルドでは平文 TOC で可視化可。

---

## 4. 暗号化

### 4.1 暗号 — **AES-256-GCM（Windows CNG / `BCrypt`）**
- GCM は AEAD（認証付き暗号）— 機密性と改竄検知を 1 パスで両立。整数値の
  完全性検証（§6）が暗号化エントリでは無料になる。
- x86-64 全 CPU で AES-NI ハードウェア加速。CNG が自動利用。
- Windows 専用なので `bcrypt.lib` は OS 同梱 = サードパーティ依存ゼロ。FIPS 検証済・
  定数時間。`Module.cmake` で `LINK_PRIVATE bcrypt`（`Platform` が `Shell32` を
  リンクするのと同じ流儀）。
- AES-CTR は非認証ゆえ別途 HMAC が要り劣る。ChaCha20 は CNG 露出が不安定で AES-NI
  ありなら AES-GCM の方が速い。→ AES-256-GCM 一択。

### 4.2 エントリ単位の暗号化
各ブロブエントリを独立に、それぞれ固有の 96bit ノンス（TOC に格納）で暗号化。
理由: (a) 実行時はロード対象 1 個だけ復号すればよい（ゲームはアセットを逐次
ストリームする）、(b) ノンス再利用は GCM を破滅させるため、パッカーが
`BCryptGenRandom` の CSPRNG + 単調カウンタで一意性を二重保証、(c) 改竄の影響が
そのエントリだけに隔離される。TOC は 1 個の GCM ブロブとして暗号化（ノンスは
`header_hash` から決定論的に導出）。

### 4.3 GCM の AAD でコンテキスト束縛
各エントリの GCM 呼び出しに、その TOC レコードの `path_hash` + `original_size` を
**AAD（認証付き関連データ）**として渡す。攻撃者がエントリ A の暗号文をエントリ B
として差し込んでもタグ検証が失敗する（cut-and-paste 攻撃の防止、ほぼゼロコスト）。

### 4.4 鍵管理（実用的・正直に）
鍵は真には隠せない以上、目標は「鍵を見つける手間をクラックの他工程と同じくらい
面倒にする」ことだけ。
- 256bit の鍵素材を直接 AES 鍵にせず、`AES_key = SHA-256(鍵素材 ‖ salt)`（CNG の
  KDF。salt 16 バイトはヘッダ予約域に平文格納）。バイナリ中のバイト列がそのまま
  AES 鍵にはならない。
- **既定（Tier 1）**: 鍵素材 32 バイトを 1 本の配列にせず、**4 断片に分割**して別々の
  翻訳単位に散らし、各断片をコンパイル時定数で XOR マスク。起動時に目立たない名前の
  関数で再構成 → KDF → `BCRYPT_KEY_HANDLE` 作成直後に組み立てバッファを
  `SecureZeroMemory`。これは「正直な難読化」— 破られないふりはしない。
- **Tier 2（任意）**: 鍵をバイナリに置かず、別ファイル / レジストリ / 初回起動時の
  サーバ取得にする。`IArchiveKeyProvider` インターフェースから `FAcpakKey` を供給して
  差し替え可能。
- パッカーとランタイムは同じ鍵で合意する。パッカーはビルド時に鍵をファイル/環境
  変数から読む（CLI 引数で生鍵は渡さない — シェル履歴/CI ログに残るため）。
  **鍵ファイルはビルド秘密。バージョン管理にコミットしない**（ドキュメントで明記）。

```cpp
struct FAcpakKey { u8 bytes[32]; };
class IArchiveKeyProvider {
public:
    virtual ~IArchiveKeyProvider() noexcept = default;
    virtual TResult<void> ProvideKey(FAcpakKey& out) noexcept = 0;
};
```

---

## 5. 圧縮

- **順序は compress-then-encrypt 厳守**。暗号化後のデータは乱数同然で圧縮不能なので、
  先に圧縮、後に暗号化。復号経路は逆順（スライス読み → GCM 復号+検証 → LZ4 展開）。
- 圧縮器は **LZ4**（block 形式）。`stb` 等と同じ FetchContent で取り込む小さな単一
  ファイルライブラリ。展開が極めて高速（複数 GB/s）。`LZ4_decompress_safe` 一発、
  展開先は TOC の `original_size` で事前確保。BSD ライセンスで商用出荷可。
  （サードパーティ依存を嫌うなら CNG Compression API を `WITH_ACS_ASSETPACK_CNG_COMPRESS`
  でビルドオプション化。）
- **エントリ単位・任意**。PNG/JPG/OGG/MP3/FLAC/GLB は既にエントロピー圧縮済 →
  パッカーが拡張子/マジックで検出し**生格納**（`STORE_RAW`、ただし暗号化はする）。
  JSON/XML/シェーダ/生バイナリ等は LZ4。
- **安全規則**: 圧縮後に `compressed_size < original_size * 0.97` を満たさなければ
  生格納に切替。アーカイブがバラより大きくなることは決して無い。

---

## 6. 完全性検証

2 層:
- **`content_hash`（全エントリ・常時）** — 元の復号済みバイトの `HashBytes`。実行時に
  復号結果を再ハッシュして照合。高速な**破損**検知（不良セクタ・転送失敗・パッチ
  失敗）。元データに対して計算するのでコーデック/暗号設定に依存しない。
- **GCM 認証タグ（暗号化エントリ）** — 1 ビットの改変や AAD 不一致で
  `BCryptDecrypt` が失敗。**暗号学的な改竄**検知。
- **`header_hash`** — ヘッダ破損/切詰めを、オフセットを信用する前に検知。

正直な注記: GCM の改竄検知は「鍵を持たない攻撃者」に対してのみ有効。鍵を抽出した
攻撃者は改変アセットを再暗号化して正当なタグを偽造できる。§2 のカジュアル攻撃者
クラスが対象。

新エラーカテゴリ `EErrCategory::Crypto` を追加推奨。エラーコード: `ACPAK_BAD_MAGIC` /
`ACPAK_VERSION_UNSUPPORTED` / `ACPAK_HEADER_CORRUPT` / `ACPAK_TOC_DECRYPT_FAILED` /
`ACPAK_ENTRY_NOT_FOUND` / `ACPAK_ENTRY_DECRYPT_FAILED` / `ACPAK_ENTRY_DECOMPRESS_FAILED` /
`ACPAK_CONTENT_HASH_MISMATCH` / `ACPAK_KEY_UNAVAILABLE`。

---

## 7. パッカーツール `acs_assetpack`

ライブラリ中核 + 薄い CLI の 2 部構成:

- **`FAcpakWriter`（ライブラリ）** — CLI 非依存。エントリを蓄積し、ノンス割当
  （`BCryptGenRandom`）、エントリ毎に compress-then-encrypt、ブロブを `.tmp` へ
  ストリーム書き出し、TOC（`path_hash` ソート・任意で暗号化）+ ヘッダを書き、
  最後に `.tmp` → 本ファイルへアトミックリネーム。エントリ毎処理は独立なので
  `FThreadPool` で並列化。
  ```cpp
  class FAcpakWriter {
  public:
      TResult<void> Open(const wchar_t* out, EAcpakFlags flags) noexcept;
      void SetKey(const FAcpakKey& key) noexcept;
      TResult<void> AddFile(const wchar_t* virtual_path, const void* data, u64 size) noexcept;
      TResult<void> Finalize() noexcept;
  };
  ```
- **`acs_assetpack` CLI** — `tools/assetpack/`（新設の `tools/` トップディレクトリ）の
  コンソール実行ファイル。`main` は引数を解析して `FAcpakWriter` を駆動するだけ。
  ```
  acs_assetpack pack    --in <assets_dir> --out <file.acpak>
                        [--key-file <path> | --key-env <VAR>]
                        [--no-encrypt] [--plain-toc] [--keep-paths] [--no-compress]
                        [--manifest <file.toml>] [--verbose]
  acs_assetpack list    --in <file.acpak> [--key-file <path>]
  acs_assetpack verify  --in <file.acpak> [--key-file <path>]   # CI ゲートに最適
  acs_assetpack extract --in <file.acpak> --out <dir> [--key-file <path>]
  ```
- `FFileSystem` にディレクトリ列挙 API が無いため、パッカーは Win32
  `FindFirstFileW`/`FindNextFileW` の再帰ウォーカを追加（`FFileSystem::EnumerateDirectory`
  としてエンジンに足すのが望ましい）。
- **論理パスの正規化**: アセットのルート相対パスを「前方スラッシュ・小文字・生 UTF-8」に
  正規化したものを `HashBytes` して `path_hash` にする。**パッカーとランタイムが同一の
  `NormalizeLogicalPath()` 関数を共有する**ことが本設計で最重要の正当性不変条件。
- ビルド統合: 開発中はバラ `assets/` を直接使用（パック不要）。`.acpak` は出荷/Release
  ビルドのステップ（CMake カスタムコマンドや梱包スクリプト）で生成。

---

## 8. VFS / マウント層

### 8.1 中核 — `FAssetRegistry` の 2 つの読み出し地点に介入
`FAssetRegistry::Load` は全ディスク読みを `FFileSystem::ReadAllBytes(path)` の
**わずか 2 箇所**（同期 `Load` と非同期ワーカ）に集約している。この 2 箇所を
`FVirtualFileSystem::ReadAsset(path)` に差し替えるだけで、`.acpak` 透過対応が
**ローダ変更ゼロ・ゲームコード変更ゼロ**で実現する。ローダの契約
`LoadFromBytes(FAssetId, const TArray<byte>&)` は不変 — バイト列がバラファイル由来か
復号済み pak エントリ由来かをローダは知らないし気にしない。

### 8.2 `FVirtualFileSystem` — マウントスタック
```cpp
class IMountSource {                       // バラディレクトリ or 開いた .acpak
public:
    virtual ~IMountSource() noexcept = default;
    virtual bool Exists(u64 path_hash, FStringView logical) const noexcept = 0;
    virtual TResult<TArray<byte>> Read(u64 path_hash, FStringView logical) noexcept = 0;
};
class FVirtualFileSystem {
public:
    TResult<void> MountDirectory(const wchar_t* dir, i32 priority) noexcept;
    TResult<void> MountArchive(const wchar_t* acpak, i32 priority, IArchiveKeyProvider&) noexcept;
    void         Unmount(i32 priority) noexcept;
    TResult<TArray<byte>> ReadAsset(const wchar_t* logical_path) noexcept;
    bool         Exists(const wchar_t* logical_path) const noexcept;
};
```
- `FLooseDirectorySource` — 基準ディレクトリをラップ。`Read` = `FFileSystem::ReadAllBytes`。
  開発経路、現状と完全に同一挙動。
- `FArchiveSource` — `.acpak` をメモリマップ（`CreateFileMapping`/`MapViewOfFile`）して
  `FAcpakReader` でラップ。`Read` = TOC を `path_hash` で二分探索 → ブロブをスライス
  → GCM 復号（CNG）→ LZ4 展開 → `content_hash` 照合 → `TArray<byte>` 返却。

### 8.3 マウント優先度
`(priority, IMountSource)` を優先度降順で保持。`ReadAsset` はパス正規化 → ハッシュ →
高優先度から走査、最初に `Exists` の真な source が応える。これだけで:
- **パッチ / DLC / MOD**: `patch.acpak` を優先度 100、`game.acpak` を 0 でマウント →
  差分アセットが原本を上書き、未変更は素通り。
- **開発ホットリロード**: バラ `assets/` を優先度 100、`game.acpak` を 0 で同時マウント
  → バラの `textures/hero.png` を置けば pak の同名を即上書き、再パック不要。

### 8.4 開発 vs 出荷 — 違うのはこの 1 行だけ
```cpp
#if ACS_SHIPPING
    vfs.MountArchive(L"game.acpak", /*priority*/0, keyProvider);
#else
    vfs.MountDirectory(L"assets", /*priority*/0);     // バラ、高速イテレーション
#endif
```
レジストリ・ローダ・シーン・あらゆる `Load(L"…")` は dev/ship で完全に同一。
**起動時のマウント 1 行が、バラ開発ビルドと暗号化アーカイブ出荷ビルドの全差分** —
これが本サブシステムの中心的成果。

### 8.5 非同期の正当性
非同期ワーカも `vfs.ReadAsset` を呼ぶ。`FArchiveSource::Read` はスレッド安全に:
メモリマップビューは読み取り専用で共有安全、CNG 鍵ハンドルはワーカ毎に複製
（複製は安価）。マウント後の TOC 配列は不変なので二分探索はロックフリー。

---

## 9. モジュール配置 — 新エンジンモジュール `src/assetpack/`

**`asset` モジュールの拡張でも `GameFramework` の一部でもなく、独立した新エンジン
モジュール `ACS::AssetPack` とする。**
- `asset` 拡張にしない: `asset` は `stb`/`cgltf`/`ufbx`/`drlibs` をリンクする重い
  デコーダ群。crypto/圧縮/VFS/別実行ファイルを混ぜると肥大化し、層構造が崩れる
  （VFS は概念的に `FAssetRegistry` の**下**＝バイト供給源）。
- `GameFramework` に入れない: `GameFramework`（`acs::game`）はアプリ層で「エンジンへ
  一方向依存」が鉄則。`FAssetRegistry`（エンジン）が呼ぶ VFS をアプリ層に置くと
  エンジンがアプリ層へ逆依存し、その鉄則を破る。format/crypto/VFS は純粋にエンジン。
- 新モジュール: format + crypto + 圧縮 + VFS + パッカーの自己完結サブシステム。
  独自外部依存（`bcrypt`/`lz4`）は他のどのモジュールも要らない。

```
src/assetpack/
  Module.cmake   acs_module(NAME AssetPack TYPE Runtime
                   PUBLIC_DEPS Foundation Memory Container Threading Platform
                   LINK_PRIVATE bcrypt  ...lz4)
  AcpakFormat.h           オンディスク構造・マジック・フラグ・バージョン定数
  NormalizeLogicalPath.h/.cpp  パッカー/ランタイム共有のパス正規化（最重要）
  AcpakCrypto.h/.cpp      FAcpakCrypto と FAcpakKey
  AcpakLz4.h/.cpp         FAcpakLz4
  AcpakReader.h/.cpp      FAcpakReader
  AcpakWriter.h/.cpp      FAcpakWriter
  VirtualFileSystem.h/.cpp FVirtualFileSystem・IMountSource 実装
tools/assetpack/
  CMakeLists.txt / Main.cpp   acs_assetpack CLI（ACS::AssetPack をリンク）
```
`modules.cmake` に `acs_enable_module(AssetPack)`。`cmake/ACSThirdParty.cmake` に
`acs_third_party_lz4()` を追加。`asset` モジュールの `Module.cmake` の `PUBLIC_DEPS` に
`AssetPack` を追加（既存コードへの唯一の変更）。

`FAssetRegistry` は任意の `FVirtualFileSystem*` を 1 つ持つ（既定 null）。null なら現状
通り `FFileSystem::ReadAllBytes`（既存コード・サンプルは無改変）。設定されていれば
2 つの読み出し地点が `vfs->ReadAsset` を呼ぶ。

---

## 10. GameFramework との統合（製品化ワークフロー）

`GameFramework` の Pillar G（リソース・永続化）は `FAssetBundle`・型付きハンドル・
`FSaveArchive`・`FSettings` を `FAssetRegistry` の上に持つ。VFS はそれら全ての**下**に入る:
- `FGame`（Pillar A）が `FAssetRegistry` をグローバルサービスとして所有。加えて
  `FVirtualFileSystem` もグローバルサービスとして所有し、**起動時に 1 回**マウント →
  `registry.SetVfs(&vfs)`。
- `FGame` に `virtual OnConfigureAssets(FVirtualFileSystem&)` フック、または
  ビルドフラグ `ACS_GAME_SHIPPING`（既存 `ACS_GAME_DEBUG` と対）で dev/ship を分岐。
  既定挙動: 実行ファイル隣に `game.acpak` があればマウント、無ければバラ `assets/`。
  → 開発者が何もしなくても「dev はバラ・ship はパック」が成立。
- `FAssetBundle` の非同期一括ロード（`LoadAsync`/`FAssetFuture` 経由）も自動で VFS を
  通る — Pillar G のコード変更不要。ローディング画面シーンの進捗ポーリングは
  バラでも暗号化 pak でも同一に動く。
- **開発フロー**: バラで実行、変更ファイルは即リロード、`acs_assetpack` は不要。
  **出荷フロー**: Release 梱包ステップで `acs_assetpack pack`（CI で `verify`）、
  バイナリは `game.acpak` 同梱、`FGame` がマウント。ゲームコード差分ゼロ。

---

## 11. セーブファイルの改竄対策

`GameFramework` の `FSaveArchive` は magic+version+CRC32+アトミックリネームを予定済。
セーブはプレイヤー自身のマシンで書かれるので「所有者から隠す」目的は無意味
（自分のセーブを編集するのは本人の自由とも言える）。意味があるのは (a) 偶発破損
（CRC32 で対処済）と (b) **チート目的の改竄**（リーダーボード・実績がある場合）。
後者には暗号化でなく **HMAC-SHA256（CNG）による鍵付き認証タグ**が正しい — セーブ
ペイロードに、`AssetPack` の `IArchiveKeyProvider` から得た難読化埋め込み鍵で HMAC を
付ける。手編集セーブを検知して拒否/フラグできる。`FSaveArchive` に**任意の HMAC タグ**を
追加（`AssetPack` の `FAcpakCrypto` を再利用）。セーブ内容の暗号化はスキーマ進化設計の
デバッグ性を損ねるので既定オフ、希望者向けのオプトインのみ。

---

## 12. 主要判断まとめ

| 項目 | 決定 |
|---|---|
| 形式 | `.acpak`: 64B 平文ヘッダ + path_hash ソート済 TOC + 16B 整列ブロブ。TOC は既定で暗号化、出荷ではパスをハッシュ化（復元不能） |
| 暗号 | AES-256-GCM（Windows CNG/BCrypt）。エントリ単位、ノンス一意、`path_hash` を AAD に |
| セキュリティ正直性 | クライアント側暗号化＝難読化。カジュアル流出と汎用吸い出しを阻止、解析の段差を上げる。本気の攻撃者は止めない。「アセット難読化」と表現 |
| 鍵管理 | 256bit 素材 → SHA-256 KDF。既定: 4 断片に分割・XOR マスク・散在、起動時再構成、使用後 `SecureZeroMemory`。`IArchiveKeyProvider` でファイル/サーバ鍵も可。鍵ファイルはコミット禁止 |
| 圧縮 | LZ4、compress-then-encrypt。既圧縮形式は生格納。「97% 切らねば生格納」安全規則 |
| 完全性 | エントリ毎 `content_hash`（破損検知）+ GCM タグ（改竄検知）+ `header_hash` |
| パッカー | `FAcpakWriter` ライブラリ + 薄い CLI `acs_assetpack`（pack/list/verify/extract）。並列パック |
| VFS | `FAssetRegistry` の 2 読み出し地点に介入。マウントスタックでバラ/アーカイブ透過。優先度でパッチ/MOD/ホットリロード。dev/ship はマウント 1 行差 |
| 配置 | 新エンジンモジュール `src/assetpack/`（`ACS::AssetPack`）。`GameFramework` でも `asset` 拡張でもない（VFS はエンジンがアプリ層へ逆依存できないため） |
| セーブ | CRC32 維持 + 任意の HMAC-SHA256 改竄タグ（`AssetPack` の `FAcpakCrypto` 再利用）。内容暗号化は既定オフ |
