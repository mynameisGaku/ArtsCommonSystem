/* ACS リファレンス — assetpack モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > は &lt; &gt;。 */
ACS_REF.modules.push({
  id: "assetpack",
  order: 36,
  title: "assetpack — アセットパック(配布/暗号化)",
  blurb: "ゲームの画像・音・データなど大量の<t>アセット</t>を <code>.acpak</code> という 1 つの書庫ファイルにまとめ、必要に応じて <t>圧縮</t>(LZ4)・<t>暗号化</t>(AES-256-GCM)して配布する仕組みです。実行中はその書庫から名前でファイルを取り出します。",
  types: [
    {
      name: "FAcpakWriter",
      kind: "クラス", header: "assetpack/AcpakWriter.h",
      summary: "バラバラのファイル(バイト列)を 1 つの <code>.acpak</code> <t>アーカイブ</t>にまとめて書き出す<b>梱包係</b>。配布前のパッキング工程で使う。",
      when: "ゲームの全アセットを 1 ファイルに固めたい時。圧縮や暗号化を有効にして「中身を覗かれにくく」したい時。",
      sample: "acs::assetpack::FAcpakWriter w;\nw.Open(L\"out/game.acpak\", acs::assetpack::AcpakFlagNone);\nfor (auto&amp; a : assets)\n    w.AddFile(a.path, a.bytes, a.size);  // この場では貯めるだけ\nw.Finalize();   // ここで一気に書き出す\nw.Close();",
      members: [
        { sig: "TResult<void> Open(const wchar_t* output_path, EAcpakFlags flags)", ret: "成否(<t>Result</t>)", desc: "出力ファイルを作る(既存は上書き)。<code>flags</code> で圧縮/暗号化を選ぶ。暗号化する場合は <b>Open より前に</b> <code>SetKey</code> を呼ぶこと。二重 Open はエラー。" },
        { sig: "void SetKey(const FAcpakKey& key)", desc: "暗号化に使う<t>鍵</t>を設定する。<code>AcpakFlagEncrypted</code> を立てた時に <code>Finalize</code> で使われる。Open の前後どちらでも呼べる。", when: "暗号化付きの pak を作る時(鍵未設定だと Finalize でエラー)。" },
        { sig: "TResult<void> AddFile(const wchar_t* virtual_path, const void* data, u64 size)", desc: "1 ファイルを追加する。<b>その場では書かず</b>、パス・ポインタ・サイズを内部に積むだけ。<code>data</code> は Writer がコピーしないので、<code>Finalize</code> まで寿命を保つこと。", sample: "w.AddFile(L\"textures/hero.png\", pngBytes, pngLen);", when: "梱包したいファイルを 1 つずつ登録していく時。" },
        { sig: "TResult<void> Finalize()", desc: "pak を確定する。ヘッダ→全データ→<t>ファイルテーブル</t>の順で書き出し、各ファイルの <t>CRC32</t> も計算して記録する。これを呼ぶまで実ファイルには何も書かれない。", when: "全ファイルの AddFile が済んだ後、最後に 1 度だけ。" },
        { sig: "void Close()", desc: "ハンドルを閉じる。<code>Finalize</code> 前に呼ぶと書きかけの中途半端なファイルが残る。多重 Close は安全(何もしない)。" }
      ]
    },
    {
      name: "FAcpakReader",
      kind: "クラス", header: "assetpack/AcpakReader.h",
      summary: "1 つの <code>.acpak</code> を開き、中の仮想ファイルを<b>名前で取り出す</b>読み出し係。<t>圧縮</t>・<t>暗号化</t>されていれば自動で<t>解凍</t>・<t>復号</t>して返す。",
      when: "実行時にアーカイブからテクスチャや音データを読み込みたい時。普通はゲーム起動時に 1 度 Open し、必要な時に ReadFile する。",
      sample: "acs::assetpack::FAcpakReader reader;\nreader.Open(L\"game.acpak\");\nauto sz = reader.GetUncompressedSize(L\"textures/hero.png\");\nacs::byte* buf = MyAlloc(sz.Value());\nreader.ReadFile(L\"textures/hero.png\", buf, sz.Value());\nreader.Close();",
      members: [
        { sig: "TResult<void> Open(const wchar_t* file_path)", desc: "pak を開いてヘッダと<t>ファイルテーブル</t>を読む。多重 Open は前回を自動 Close。<b>暗号化 pak</b> は Open 前に <code>SetKey</code> で鍵を渡しておくこと(無いと ReadFile でエラー)。magic/version/flags 不一致はエラー。" },
        { sig: "void SetKey(const FAcpakKey& key)", desc: "暗号化 pak の<t>復号</t>鍵を設定する。<code>ReadFile</code> 内の AES-256-GCM 復号で使う。Close すると鍵情報は 0 クリアされる。", when: "暗号化された pak を読む時。" },
        { sig: "void Close()", desc: "ハンドルを閉じ、文字列<t>プール</t>と entry 配列を解放する。Open 前・Close 済でも安全。" },
        { sig: "bool IsOpen() const", ret: "開いているか", desc: "Open 成功後かつ Close 前なら true。" },
        { sig: "u32 FileCount() const", ret: "ファイル数", desc: "今開いている pak に含まれる仮想ファイルの数。未 Open なら 0。" },
        { sig: "const FAcpakFileEntry* GetEntry(u32 index) const", ret: "エントリ or null", desc: "<code>index</code> 番目のファイル情報を返す。範囲外・未 Open なら null。返り値の寿命は次の Close まで。" },
        { sig: "const FAcpakFileEntry* FindEntry(const wchar_t* path) const", ret: "エントリ or null", desc: "仮想パスからファイル情報を探す(<t>線形探索</t>・完全一致)。見つからない・未 Open なら null。" },
        { sig: "TResult<u64> ReadFile(const wchar_t* path, void* out_buffer, u64 buffer_size)", ret: "書き込んだバイト数", desc: "ファイルを <code>out_buffer</code> へ読み出す。<code>buffer_size</code> は <code>GetUncompressedSize</code> 以上必要。読み出し後 <t>CRC32</t> を照合し、不一致(破損/改竄)はエラー。", sample: "auto r = reader.ReadFile(L\"data/level1.bin\", buf, bufSize);\nif (r.IsOk()) { /* buf に生データ */ }", when: "実際にファイルの中身が欲しい時。" },
        { sig: "TResult<u64> GetUncompressedSize(const wchar_t* path) const", ret: "復号+解凍後のバイト数", desc: "ファイルの最終サイズを返す。<code>ReadFile</code> 用バッファの事前確保に使う。未存在パスはエラー。", when: "ReadFile の前に必要なバッファ量を知りたい時。" },
        { sig: "u32 Flags() const", ret: "<t>フラグ</t>", desc: "ヘッダから読んだ <code>flags</code>(暗号化/圧縮の有無)をそのまま返す。診断用。" }
      ]
    },
    {
      name: "FAcpakCrypto",
      kind: "クラス(static 関数群)", header: "assetpack/AcpakCrypto.h",
      summary: "<t>AES-256-GCM</t> 暗号化と <t>PBKDF2</t> による<t>鍵導出</t>をまとめた薄いラッパ。中身は Windows 標準の <t>CNG</t>(BCrypt)で、外部ライブラリに依存しない。インスタンス化はせず静的関数だけを使う。",
      when: "通常は <code>FAcpakWriter</code>/<code>FAcpakReader</code> が内部で使うので直接触る必要は少ない。パスワードから鍵を作る時だけ <code>DeriveKey</code> を呼ぶ。",
      sample: "// パスワードから鍵を作る\nconst u8 pw[] = \"my-secret\";\nconst u8 salt[] = \"acs-salt\";\nauto key = FAcpakCrypto::DeriveKey(pw, 9, salt, 8);\nwriter.SetKey(key.Value());",
      members: [
        { sig: "TResult<FAcpakKey> DeriveKey(const u8* password, u32 password_len, const u8* salt, u32 salt_len)", ret: "32 バイトの鍵", desc: "パスワードと <t>salt</t> から AES-256 鍵を作る(PBKDF2-HMAC-SHA256、10000 回)。ポインタは即時消費され、戻り後は参照されない。", when: "ユーザー入力のパスワードを直接 32 バイト鍵に変換したい時。" },
        { sig: "TResult<void> Encrypt(const FAcpakKey& key, const u8 nonce[12], const u8* plaintext, u64 size, u8* ciphertext, u8 tag_out[16])", desc: "平文を暗号化し、暗号文と認証<t>タグ</t>(16 バイト)を出力する。<code>nonce</code> は per-entry に必ず一意であること(同じ <code>(key, nonce)</code> の再利用は致命的)。in-place(plaintext==ciphertext)可。" },
        { sig: "TResult<void> Decrypt(const FAcpakKey& key, const u8 nonce[12], const u8 tag[16], const u8* ciphertext, u64 size, u8* plaintext)", desc: "暗号文とタグから平文を復号する。タグ検証に失敗すると(=少しでも改竄/破損していると)全データを弾く。in-place 可。" },
        { sig: "TResult<void> GenerateRandomNonce(u8 nonce_out[12])", ret: "ランダムな nonce", desc: "<t>CSPRNG</t> で 12 バイトの一意な <t>nonce</t> を作る。失敗時は決して成功を返さず 0 クリア+エラー(予測可能な nonce での暗号化は禁止のため)。", when: "Encrypt のたびに新しい nonce を用意する時。" }
      ]
    },
    {
      name: "FAcpakLz4",
      kind: "クラス(static 関数群)", header: "assetpack/AcpakLz4.h",
      summary: "<code>.acpak</code> の各ファイルを<t>圧縮</t>/<t>解凍</t>する自前 <t>LZ4</t> 実装(外部依存ゼロ)。LZ4 は「速い・そこそこ縮む」高速圧縮形式。インスタンス化はせず静的関数を使う。",
      when: "通常は Writer/Reader が内部で使う。任意のバイト列を手早く圧縮/解凍したい時に直接呼んでもよい。",
      sample: "u32 cap = FAcpakLz4::MaxCompressedSize(srcLen);\nu8* dst = MyAlloc(cap);\nauto n = FAcpakLz4::Compress(src, srcLen, dst, cap);\n// n.Value() = 圧縮後の実バイト数",
      members: [
        { sig: "u32 MaxCompressedSize(u32 input_size)", ret: "最悪ケースの出力サイズ", desc: "入力サイズから「これ以下に必ず収まる」圧縮後サイズを返す。圧縮できないデータでもこの範囲。出力バッファ確保に使う。", when: "Compress に渡す出力バッファのサイズを決める時。" },
        { sig: "TResult<u32> Compress(const u8* src, u32 src_size, u8* dst, u32 dst_capacity)", ret: "圧縮後バイト数", desc: "<code>src</code> を <code>dst</code> へ圧縮する。<code>dst_capacity</code> は <code>MaxCompressedSize</code> 以上推奨。13 バイト未満は素通し。" },
        { sig: "TResult<u32> Decompress(const u8* src, u32 src_size, u8* dst, u32 dst_capacity)", ret: "解凍後バイト数", desc: "圧縮データを <code>dst</code> へ解凍する。<code>dst_capacity</code> は元サイズ以上必要。不正な入力でも<t>境界検査</t>で安全に弾く。" }
      ]
    },
    {
      name: "FAcpakKey",
      kind: "構造体", header: "assetpack/AcpakCrypto.h",
      summary: "AES-256 用の <b>256bit(32 バイト)の<t>鍵</t></b>を表す単純な <t>POD</t>。1 つの <code>.acpak</code> は 1 鍵で全エントリを暗号化する。",
      when: "<code>FAcpakWriter::SetKey</code> / <code>FAcpakReader::SetKey</code> に渡す。普通は <code>FAcpakCrypto::DeriveKey</code> で作る。",
      sample: "FAcpakKey key{};               // 0 埋め\nauto k = FAcpakCrypto::DeriveKey(pw, pwLen, salt, saltLen);\nreader.SetKey(k.Value());",
      members: [
        { sig: "u8 bytes[32]", desc: "32 バイトの鍵データ。" }
      ]
    },
    {
      name: "FAcpakHeader",
      kind: "構造体", header: "assetpack/AcpakFormat.h",
      summary: "<code>.acpak</code> 先頭に書かれる固定長の<b>ヘッダ</b>(<t>POD</t>)。magic・version・flags・ファイル数・<t>ファイルテーブル</t>の位置を持つ。ディスク上は 36 バイト。",
      when: "フォーマットを自前で読み書きする時の参照用。普通は Reader/Writer が内部で扱うので直接触らない。",
      sample: "// ディスク上のレイアウト(参考):\n// magic[8] \"ACPAK\\0\\0\\0\" / version=1 / flags / file_count\n// / padding / file_table_offset(u64) / reserved",
      members: [
        { sig: "u8 magic[8]", desc: "<code>\"ACPAK\\0\\0\\0\"</code>。pak 判定に使う 8 バイトの<t>マジックナンバー</t>。" },
        { sig: "u32 version", desc: "フォーマット版数(現在 1)。" },
        { sig: "u32 flags", desc: "暗号化/圧縮の有無を表す <code>EAcpakFlags</code> の<t>ビットフラグ</t>。" },
        { sig: "u32 file_count", desc: "<t>ファイルテーブル</t>内のエントリ数。" },
        { sig: "u64 file_table_offset", desc: "アーカイブ先頭からファイルテーブルまでの絶対オフセット。" }
      ]
    },
    {
      name: "FAcpakFileEntry",
      kind: "構造体", header: "assetpack/AcpakFormat.h",
      summary: "<t>ファイルテーブル</t>の 1 行を <t>Reader</t> がメモリ上に展開した形。仮想パス・オフセット・サイズ・<t>CRC32</t>、暗号化 pak なら <t>nonce</t>/<t>タグ</t>を持つ。",
      when: "<code>FAcpakReader::GetEntry</code> / <code>FindEntry</code> の戻り値として受け取り、ファイルの素性を調べる時。",
      sample: "const FAcpakFileEntry* e = reader.FindEntry(L\"bgm/title.ogg\");\nif (e) {\n    u64 raw = e->size_stored;          // アーカイブ上の生サイズ\n    u64 real = e->size_uncompressed;   // 解凍後サイズ\n}",
      members: [
        { sig: "const wchar_t* path", desc: "仮想パス(Reader 内文字列<t>プール</t>への参照、寿命は次の Close まで)。" },
        { sig: "u64 offset", desc: "アーカイブ先頭からこのファイルデータまでの絶対オフセット。" },
        { sig: "u64 size_uncompressed", desc: "復号+解凍後のバイト数(= 最終的な実サイズ)。" },
        { sig: "u64 size_stored", desc: "アーカイブ上の生バイト数(= 圧縮+暗号化済サイズ)。" },
        { sig: "u32 crc32", desc: "<code>size_uncompressed</code> バイトに対する <t>CRC32</t>。読み出し時の破損検証に使う。" },
        { sig: "u8 cipher_nonce[12] / u8 cipher_tag[16]", desc: "暗号化 pak のときの AES-256-GCM <t>nonce</t> と認証<t>タグ</t>。非暗号化 pak では 0。" }
      ]
    },
    {
      name: "EAcpakFlags",
      kind: "列挙(enum)", header: "assetpack/AcpakFormat.h",
      summary: "ヘッダ <code>flags</code> の<t>ビットフラグ</t>。pak 全体が暗号化されているか/圧縮されているかを表す。Writer の <code>Open</code> に渡し、Reader が読んで処理を組み立てる。",
      when: "<code>FAcpakWriter::Open</code> で圧縮/暗号化を選ぶ時。複数を <code>|</code> で組み合わせ可(順序は compress→encrypt)。",
      sample: "// 圧縮+暗号化で梱包\nw.SetKey(key);\nw.Open(L\"out/game.acpak\",\n       EAcpakFlags(AcpakFlagCompressed | AcpakFlagEncrypted));",
      members: [
        { sig: "AcpakFlagNone = 0", desc: "圧縮も暗号化もしない(素の生バイト)。" },
        { sig: "AcpakFlagEncrypted = 1 << 0", desc: "各ファイルを AES-256-GCM 暗号化する。" },
        { sig: "AcpakFlagCompressed = 1 << 1", desc: "各ファイルを LZ4 圧縮する。" }
      ]
    },
    {
      name: "FAcpakGameReader",
      kind: "クラス", header: "assetpack/AcpakGameBridge.h",
      summary: "<code>FAcpakReader</code> を <t>GameFramework</t> の <code>acs::game::IAssetPackReader</code> <t>インターフェース</t>に適合させた<b>橋渡し</b>クラス。パスを <code>char*</code>(UTF-8)で扱える。",
      when: "ゲーム側の汎用アセット読み込み API(<code>IAssetPackReader</code>)経由で <code>.acpak</code> を使いたい時。",
      sample: "acs::assetpack::FAcpakGameReader r;\nr.Mount(\"game.acpak\");\nu64 sz = r.FileSize(\"textures/hero.png\").Value();\nr.ReadFile(\"textures/hero.png\", buf, sz);\nr.Unmount();",
      members: [
        { sig: "TResult<void> Mount(const char* PackPath)", desc: "pak を開く(<code>Open</code> 相当)。" },
        { sig: "void Unmount()", desc: "pak を閉じる(<code>Close</code> 相当)。" },
        { sig: "bool IsMounted() const", desc: "マウント中なら true。" },
        { sig: "TResult<u32> FileCount()", ret: "ファイル数", desc: "含まれるファイル数。" },
        { sig: "TResult<const char*> FileName(u32 Index)", ret: "ファイル名(UTF-8)", desc: "<code>Index</code> 番目の仮想ファイル名。" },
        { sig: "TResult<u64> FileSize(const char* Name)", ret: "サイズ", desc: "名前指定でファイルの最終サイズを返す。" },
        { sig: "TResult<void> ReadFile(const char* Name, u8* OutBuffer, u64 BufferSize)", desc: "名前指定でファイルを読み出す。" }
      ]
    },
    {
      name: "FAcpakGameWriter",
      kind: "クラス", header: "assetpack/AcpakGameBridge.h",
      summary: "<code>FAcpakWriter</code> を <code>acs::game::IAssetPackWriter</code> <t>インターフェース</t>に適合させた橋渡しクラス。パスを <code>char*</code>(UTF-8)で扱える。",
      when: "ゲーム側の汎用パッキング API(<code>IAssetPackWriter</code>)経由で <code>.acpak</code> を作りたい時。",
      sample: "acs::assetpack::FAcpakGameWriter w;\nw.BeginPack(\"out/game.acpak\");\nw.AddFile(\"data/level1.bin\", bytes, size);\nw.FinishPack();",
      members: [
        { sig: "TResult<void> BeginPack(const char* OutputPath)", desc: "出力 pak を開く(<code>Open</code> 相当)。" },
        { sig: "TResult<void> AddFile(const char* VirtualName, const u8* Data, u64 Size)", desc: "ファイルを 1 つ追加する。" },
        { sig: "TResult<void> FinishPack()", desc: "pak を確定して書き出す(<code>Finalize</code> 相当)。" }
      ]
    },
    {
      name: "既定 provider 結線関数",
      kind: "関数群", header: "assetpack/AcpakGameBridge.h",
      summary: "<t>GameFramework</t> 側のアセット読み書きを、実体である <code>.acpak</code> の Reader/Writer に<b>差し込む</b>結線関数。アプリ起動時に 1 度呼べば、以降はバックエンド非依存に既定 Reader/Writer を取得できる。",
      when: "アプリ初期化時。<code>InstallAcpak...AsDefault</code> を呼んでおくと、<code>acs::game::GetDefaultAssetPackReader()</code> 等が実 <code>.acpak</code> 実装を返すようになる。",
      sample: "// 起動時に 1 度だけ\nacs::assetpack::InstallAcpakReaderAsDefault();\nacs::assetpack::InstallAcpakWriterAsDefault();\n// 以降はゲーム側 API が実 .acpak を使う",
      members: [
        { sig: "acs::game::IAssetPackReader& GetDefaultAcpakReader()", ret: "既定 Reader", desc: "プロセス共有の既定 <code>.acpak</code> Reader を返す(<t>シングルトン</t>)。" },
        { sig: "void InstallAcpakReaderAsDefault()", desc: "上記 Reader を GameFramework の既定 provider として登録する。" },
        { sig: "acs::game::IAssetPackWriter& GetDefaultAcpakWriter()", ret: "既定 Writer", desc: "プロセス共有の既定 <code>.acpak</code> Writer を返す(<t>シングルトン</t>)。" },
        { sig: "void InstallAcpakWriterAsDefault()", desc: "上記 Writer を GameFramework の既定 provider として登録する。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "アセット": "ゲームで使う画像・音・モデル・データなどの素材ファイル。",
  "アーカイブ": "複数のファイルを 1 つにまとめた書庫ファイル。<code>.acpak</code> はその一種。",
  "圧縮": "データを規則性を使って小さくすること。ここでは <t>LZ4</t> を使う。",
  "解凍": "<t>圧縮</t>したデータを元に戻すこと。",
  "暗号化": "鍵を知らないと中身を読めない形にデータを変換すること。",
  "復号": "<t>暗号化</t>したデータを鍵で元に戻すこと。",
  "鍵": "暗号化/復号に使う秘密のデータ。ACS では 256bit(32 バイト)の <t>FAcpakKey</t>。",
  "LZ4": "速度重視の可逆<t>圧縮</t>形式。そこそこ縮み、展開がとても速い。",
  "AES-256-GCM": "256bit 鍵の共通鍵<t>暗号化</t>方式。暗号化と同時に改竄検知(認証<t>タグ</t>)も行う AEAD。",
  "PBKDF2": "パスワードを何千回も繰り返しハッシュして強い<t>鍵</t>を作る<t>鍵導出</t>方式。総当たり攻撃を遅くする。",
  "鍵導出": "パスワードなど人間が覚えられる入力から、暗号用の<t>鍵</t>を計算で作ること。",
  "CNG": "Windows 標準の暗号 API(Cryptography Next Generation)。外部ライブラリ無しで AES などが使える。",
  "nonce": "暗号化のたびに変える使い捨ての値。同じ鍵で再利用すると安全性が崩れるため毎回変える。",
  "タグ": "AES-GCM が出力する 16 バイトの認証データ。これが合わないとデータが改竄/破損とみなす。",
  "salt": "<t>鍵導出</t>に混ぜるランダム値。同じパスワードでも違う鍵になるようにする。",
  "CSPRNG": "暗号に使える品質の乱数生成器(Cryptographically Secure PRNG)。<t>nonce</t> 生成などに使う。",
  "CRC32": "32bit のチェックサム。データが壊れていないかを素早く検証する。",
  "マジックナンバー": "ファイル先頭に置く決まったバイト列。形式を判定する目印。",
  "ファイルテーブル": "アーカイブ内の各ファイルの名前・位置・サイズなどを並べた目次。",
  "ビットフラグ": "1 つの整数の各ビットに ON/OFF の意味を割り当てた値。<code>|</code> で組み合わせる。",
  "フラグ": "ON/OFF を表す印。ここでは暗号化/圧縮の有無。",
  "プール": "同種のデータをまとめて確保しておく領域。ここでは文字列を連結して保持する。",
  "境界検査": "配列やバッファの範囲外アクセスを起こさないよう、毎回インデックスを確かめること。",
  "線形探索": "先頭から順に 1 つずつ比較して目的の要素を探す単純な探索。",
  "GameFramework": "ACS のゲーム作成用モジュール(<code>acs::game</code>)。アセット読み書きの抽象 API を持つ。",
  "シングルトン": "プロセス内に 1 つだけ存在する共有インスタンス。"
});
