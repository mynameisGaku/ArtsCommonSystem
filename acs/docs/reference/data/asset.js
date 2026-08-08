/* ACS リファレンス — asset モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "asset",
  order: 35,
  title: "asset — アセット読み込み / レジストリ",
  blurb: "画像・音声・メッシュ・テキストなどの素材ファイルを<b>パスを渡すだけで読み込み</b>、一度読んだものは<t>レジストリ</t>で共有・再利用します。<t>同期</t>でも<t>非同期</t>でも読み込めます。",
  types: [
    {
      name: "FInternedAssetPath",
      kind: "クラス", header: "asset/InternedAssetPath.h",
      summary: "正規化済みアセットパスを共有テーブルの ID として保持する軽量キー。比較とコピーで文字列全体を走査しない。",
      when: "レジストリやキャッシュで同じアセットパスを高頻度に比較する時。"
    },
    {
      name: "CAssetPathInterner",
      kind: "クラス", header: "asset/AssetPathInterner.h",
      summary: "アセットパスを正規化して一度だけ保持し、同じパスへ同じ <code>FInternedAssetPath</code> を返す共有テーブル。",
      when: "パス文字列の重複確保と繰り返しハッシュを抑えたい時。",
      members: [
        { sig: "using FAssetPathInterner = CAssetPathInterner", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CAssetPathInterner</code> を使う。" }
      ]
    },
    {
      name: "FAssetPathInternerDiagnostics",
      kind: "構造体", header: "asset/AssetPathInternerDiagnostics.h",
      summary: "パス interner の登録数、再利用数、保持 byte 数を観測する診断 snapshot。",
      when: "パス共有の効果とメモリ使用量をプロファイルする時。"
    },
    {
      name: "FAssetRegistryDiagnostics",
      kind: "構造体", header: "asset/AssetRegistryDiagnostics.h",
      summary: "アセットレジストリの検索、登録、intern 再利用経路を観測する診断 snapshot。",
      when: "アセット検索 hot path の退行を決定的カウンタで確認する時。"
    },
    {
      name: "AAsset",
      kind: "基底クラス", header: "asset/Asset.h",
      summary: "すべてのアセット(画像・音声・メッシュ・テキスト等)が継承する<b>共通の基底</b>。実行時の型 ID(<code>Type()</code>)・<t>FAssetId</t>・<t>ロード状態</t>を持つ。",
      when: "ローダやレジストリがアセットを型を問わず受け渡す時の共通の入れ物。読み込んだ <code>TSharedPtr&lt;AAsset&gt;</code> を具体型に判別したい時にも使う。",
      sample: "TSharedPtr&lt;AAsset&gt; a = result.Value();\nif (a-&gt;Type() == AImageAsset::StaticType()) {\n    auto* img = static_cast&lt;AImageAsset*&gt;(a.Get());\n    u32 w = img-&gt;Width();\n}",
      members: [
        { sig: "virtual AssetType Type() const", ret: "型 ID", desc: "派生クラスごとの実行時<t>型 ID</t>。<code>FXxx::StaticType()</code> と比較して種類を判別する。" },
        { sig: "FAssetId Id() const", ret: "識別子", desc: "このアセットの <t>FAssetId</t>(パスのハッシュ)。レジストリが設定する。" },
        { sig: "EAssetState State() const", ret: "ロード状態", desc: "<t>EAssetState</t>(未ロード/ロード中/利用可/失敗)を返す。" },
        { sig: "void SetId(FAssetId) / void SetState(EAssetState)", desc: "レジストリが内部で設定する。一般ユーザは触らない。" },
        { sig: "using FAsset = AAsset", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AAsset</code> を使う。" }
      ]
    },
    {
      name: "ACS_ASSET_TYPE(name)",
      kind: "マクロ", header: "asset/Asset.h",
      summary: "派生アセットクラスに<b>型 ID を一行で宣言</b>するマクロ。文字列リテラルから<t>ハッシュ</t>で <code>StaticType()</code> を作り、<code>Type()</code> を override する。",
      when: "自前のアセット型を <code>AAsset</code> から派生させる時、クラス先頭に置くだけ。",
      sample: "class FMyAsset : public AAsset {\npublic:\n    ACS_ASSET_TYPE(\"FMyAsset\")   // StaticType() と Type() を定義\n    // ...\n};\nif (a-&gt;Type() == FMyAsset::StaticType()) { /* これだ */ }"
    },
    {
      name: "EAssetState",
      kind: "列挙(enum)", header: "asset/Asset.h",
      summary: "アセットの<b>ロード状態</b>。<code>Unloaded</code>(未ロード) / <code>Loading</code>(非同期ロード中) / <code>Ready</code>(利用可) / <code>Failed</code>(失敗)。",
      when: "アセットがいま使える状態かを確認したい時。",
      sample: "if (asset-&gt;State() == EAssetState::Ready) {\n    // 安全に中身へアクセスできる\n}"
    },
    {
      name: "AssetType",
      kind: "型エイリアス", header: "asset/Asset.h",
      summary: "アセット型を識別する 32-bit 値(<code>using AssetType = u32</code>)。文字列名から<t>ハッシュ</t>で一意に生成され、衝突しない前提。",
      when: "ローダの担当型や <code>AAsset::Type()</code> の比較で使う数値 ID。",
      sample: "AssetType t = AImageAsset::StaticType();"
    },
    {
      name: "FAssetId",
      kind: "構造体", header: "asset/AssetId.h",
      summary: "アセットの<b>識別子</b>。パス文字列を<t>ハッシュ</t>した 64-bit 値で、同じパスからは常に同じ ID が得られる。<t>レジストリ</t>のキャッシュキーになる。",
      when: "パスではなく ID でアセットを引きたい時(<code>CAssetRegistry::Find</code> など)。",
      sample: "FAssetId id = MakeAssetId(\"textures/hero.png\");\nif (id.IsValid()) {\n    TSharedPtr&lt;AAsset&gt; a = reg.Find(id);\n}",
      members: [
        { sig: "u64 value", desc: "ハッシュ値の本体。0 は「無効」用に予約。" },
        { sig: "bool IsValid() const", ret: "有効か", desc: "<code>value != 0</code> なら true。" },
        { sig: "bool operator==/!=(FAssetId) const", desc: "ID 同士を比較する。" }
      ]
    },
    {
      name: "MakeAssetId(path)",
      kind: "関数", header: "asset/AssetId.h",
      summary: "パス文字列(<t>FStringView</t>)から <t>FAssetId</t> を作る。同じパスなら必ず同じ ID。値が 0 になった場合は 1 に補正(0 は無効用)。",
      when: "事前にパスから ID を求めておき、後で <code>Find</code> 等で引きたい時。",
      sample: "FAssetId id = MakeAssetId(\"audio/bgm.ogg\");",
      members: [
        { sig: "FAssetId MakeAssetId(FStringView path)", ret: "識別子", desc: "パスのバイト列をハッシュして ID を返す。" }
      ]
    },
    {
      name: "CAssetRegistry",
      kind: "クラス", header: "asset/AssetRegistry.h",
      summary: "アセット読み込みの<b>中心</b>。パスを渡すと拡張子に合う<t>ローダ</t>でファイルを読み、結果を <t>TSharedPtr</t> で<b>キャッシュ共有</b>する。同期/非同期どちらも対応。コピー不可。",
      when: "ゲーム内でテクスチャ・音・モデル等を読み込む時の入口。一度読んだものを二度読まずに使い回したい時。",
      sample: "CAssetRegistry reg;\nreg.RegisterDefaultLoaders();             // 標準ローダ一括登録\nauto r = reg.Load(L\"textures/hero.png\");\nif (r.IsOk()) {\n    TSharedPtr&lt;AAsset&gt; a = r.Value();      // 同じパスの再 Load は同じ a を返す\n}",
      members: [
        { sig: "void RegisterLoader(IAssetLoader* loader)", desc: "拡張子マッチで使う<t>ローダ</t>を 1 つ登録する。所有権はレジストリに渡らない(ローダの寿命は呼び出し側が管理)。", when: "自前ローダや特定型だけ追加したい時。" },
        { sig: "void RegisterDefaultLoaders()", desc: "標準ローダ群(Image / Audio / Mesh / Text / Binary)を一括登録する。", when: "とりあえず一般的な素材を読みたい最初の一手。" },
        { sig: "TResult<TSharedPtr<AAsset>> Load(const wchar_t* path)", ret: "アセット or エラー", desc: "<b>同期</b>でファイル読み込み + ローダ呼び出し。キャッシュ済みなら即返却。<t>Result</t> で成否を返す。", sample: "auto r = reg.Load(L\"mesh/box.glb\");\nif (r.IsOk()) use(r.Value());" },
        { sig: "FAssetFuture LoadAsync(const wchar_t* path)", ret: "完了ハンドル", desc: "<b>非同期</b>で <t>CThreadPool</t> ワーカーに読み込みを投げ、<t>FAssetFuture</t> を返す。キャッシュ済みなら即完了状態の future。", when: "大きいモデル等をフレームを止めずに読みたい時。", sample: "FAssetFuture fut = reg.LoadAsync(L\"big.glb\");\n// ...別の処理...\nif (fut.IsReady()) use(fut.Get().Value());" },
        { sig: "TSharedPtr<AAsset> Find(FAssetId id)", ret: "アセット or 空", desc: "キャッシュ<b>からのみ</b>取得。未キャッシュなら空の <code>TSharedPtr</code>(読み込みはしない)。", when: "既に読んだはずのものを ID で引きたい時。" },
        { sig: "void Unload(FAssetId id)", desc: "キャッシュから外す。ファイル変更時の再読み込み準備に使う(保持中の <code>TSharedPtr</code> は生き続ける)。" },
        { sig: "void Clear()", desc: "全キャッシュをクリアする。" },
        { sig: "TResult<void> AsyncCacheInsert(FAssetId, TSharedPtr<AAsset>)", ret: "成功 or エラー", desc: "ワーカースレッドから<t>ロック</t>付きでキャッシュへ挿入する内部 API。レジストリ終了中やキャッシュ挿入失敗はエラーで返す。一般ユーザは使わない。" },
        { sig: "using FAssetRegistry = CAssetRegistry", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CAssetRegistry</code> を使う。" }
      ]
    },
    {
      name: "IAssetLoader",
      kind: "インターフェース", header: "asset/IAssetLoader.h",
      summary: "アセット<b>ローダの共通<t>インターフェース</t></b>。担当する拡張子と型 ID を申告し、バイト列からアセットを作る役。<t>CAssetRegistry</t> に登録して使う。",
      when: "新しいファイル形式に対応したい時、これを実装して登録する。",
      sample: "class FMyLoader final : public IAssetLoader {\n    AssetType   TypeId()    const noexcept override { return FMyAsset::StaticType(); }\n    const char* Extension() const noexcept override { return \"myext\"; }\n    TResult&lt;TSharedPtr&lt;AAsset&gt;&gt; LoadFromBytes(FAssetId id, const TArray&lt;byte&gt;&amp; b) noexcept override;\n};",
      members: [
        { sig: "virtual AssetType TypeId() const", ret: "型 ID", desc: "このローダが作るアセットの<t>型 ID</t>。" },
        { sig: "virtual const char* Extension() const", ret: "拡張子", desc: "担当する拡張子(\"png\" など)。<code>\"*\"</code> は全拡張子のフォールバック。レジストリがこれでマッチングする。" },
        { sig: "virtual TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes)", ret: "アセット or エラー", desc: "ファイルのバイト列を解析してアセットを生成する(同期)。非同期化はレジストリ側がスレッドプールでこれを呼ぶ形で実現。" }
      ]
    },
    {
      name: "FAssetFuture",
      kind: "クラス", header: "asset/AssetFuture.h",
      summary: "<t>非同期</t>ロードの<b>結果ハンドル</b>(<t>フューチャ</t>)。<code>LoadAsync</code> が返す。完了をノンブロッキングで確認したり、待って結果を取り出したりできる。",
      when: "<code>CAssetRegistry::LoadAsync</code> の戻り値を保持し、後で完了を確認・取得する時。",
      sample: "FAssetFuture fut = reg.LoadAsync(L\"big_mesh.glb\");\nwhile (!fut.IsReady()) { /* 他の処理を進める */ }\nauto r = fut.Get();\nif (r.IsOk()) TSharedPtr&lt;AAsset&gt; a = r.Value();",
      members: [
        { sig: "bool IsReady() const", ret: "完了済みか", desc: "ノンブロッキングで完了を確認。true なら <code>Get()</code> がすぐ返る。" },
        { sig: "bool Valid() const", ret: "有効か", desc: "中身のある future か(構築失敗なら false)。" },
        { sig: "TResult<TSharedPtr<AAsset>> Get()", ret: "アセット or エラー", desc: "完了を<b>待って</b>結果を取り出す。呼び出しスレッドがワーカーなら読み込みを手伝う(<t>ワークスチール</t>)。", when: "結果が必要になったら呼ぶ。完了済みなら待たずに返る。" },
        { sig: "TResult<TSharedPtr<AAsset>> Wait()", ret: "アセット or エラー", desc: "<code>Get()</code> の別名。直感的に「待つ」と書きたい時に。" }
      ]
    },
    {
      name: "FAsyncLoadState",
      kind: "構造体", header: "asset/AssetFuture.h",
      summary: "<t>FAssetFuture</t> とワーカーが<b>共有する内部状態</b>。完了カウンタ・結果・エラーを保持する。通常ユーザが直接触ることはない。",
      when: "非同期ロードの仕組みを理解・拡張する時の内部構造。",
      sample: "// 内部用。CAssetRegistry::LoadAsync が生成し future へ渡す。",
      members: [
        { sig: "FCompletionCounter counter{1}", desc: "1 → 0 で完了を表す<t>完了カウンタ</t>。" },
        { sig: "TSharedPtr<AAsset> result", desc: "成功時の読み込み結果。" },
        { sig: "FErrorCode error; bool has_error", desc: "失敗時の<t>エラーコード</t>とその有無。" }
      ]
    },
    {
      name: "AImageAsset",
      kind: "クラス(AAsset 派生)", header: "asset/ImageAsset.h",
      summary: "<b>画像 1 枚</b>分の CPU 側ピクセルデータ。幅・高さ・<t>ピクセルフォーマット</t>・生バイト列を持つ。<code>stb_image</code> 経由で png/jpg/bmp/tga/gif/hdr 等に対応。",
      when: "テクスチャ用画像を読み込んで GPU へアップロードする前の中間データとして。",
      sample: "auto r = reg.Load(L\"hero.png\");\nauto* img = static_cast&lt;AImageAsset*&gt;(r.Value().Get());\nu32 w = img-&gt;Width(), h = img-&gt;Height();\nconst byte* px = img-&gt;Pixels();   // GPU へアップロード等",
      members: [
        { sig: "u32 Width() / u32 Height()", ret: "画素数", desc: "画像の幅・高さ(ピクセル)。" },
        { sig: "EPixelFormat Format()", ret: "フォーマット", desc: "<t>EPixelFormat</t>(チャンネル数と型)を返す。" },
        { sig: "const byte* Pixels()", ret: "先頭ポインタ", desc: "ピクセル列の先頭<t>ポインタ</t>。" },
        { sig: "usize PixelByteCount()", ret: "バイト数", desc: "ピクセルデータの総バイト数。" },
        { sig: "using FImageAsset = AImageAsset", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AImageAsset</code> を使う。" }
      ]
    },
    {
      name: "EPixelFormat",
      kind: "列挙(enum)", header: "asset/ImageAsset.h",
      summary: "画像の<b><t>ピクセルフォーマット</t></b>。チャンネル数とビット深度で識別: <code>R8</code> / <code>R8G8</code> / <code>R8G8B8</code> / <code>R8G8B8A8</code> / <code>R32G32B32_F</code> / <code>R32G32B32A32_F</code>(後 2 つは HDR の float)。",
      when: "画像データを GPU テクスチャフォーマットへ対応付ける時。",
      sample: "if (img-&gt;Format() == EPixelFormat::R8G8B8A8) {\n    // 8-bit RGBA として扱う\n}"
    },
    {
      name: "CImageAssetLoader",
      kind: "クラス(IAssetLoader 派生)", header: "asset/ImageAsset.h",
      summary: "画像ファイルの<t>ローダ</t>(<code>stb_image</code>)。バイト列を <t>AImageAsset</t> にデコードする。<code>RegisterDefaultLoaders()</code> で登録される。",
      when: "通常は標準ローダ登録で自動的に使われる。個別に追加したい時のみ手動登録。",
      sample: "CImageAssetLoader loader;\nreg.RegisterLoader(&amp;loader);",
      members: [
        { sig: "using FImageAssetLoader = CImageAssetLoader", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CImageAssetLoader</code> を使う。" }
      ]
    },
    {
      name: "AAudioAsset",
      kind: "クラス(AAsset 派生)", header: "asset/AudioAsset.h",
      summary: "<b>音声データ</b>。全フォーマットを 16-bit か float の <t>PCM</t> に統一して保持する。サンプルレート・チャンネル数・フレーム数・生サンプルを持つ。再生は <t>CAudioEngine</t> が直接この PCM を扱う。",
      when: "BGM や効果音を読み込んで再生する時。wav/mp3/flac/ogg に対応。",
      sample: "auto r = reg.Load(L\"se/jump.wav\");\nauto* au = static_cast&lt;AAudioAsset*&gt;(r.Value().Get());\nf32 sec = au-&gt;DurationSeconds();\nu32 rate = au-&gt;SampleRate();",
      members: [
        { sig: "u32 SampleRate()", ret: "Hz", desc: "1 秒あたりのサンプル数(44100 など)。" },
        { sig: "u8 Channels()", ret: "チャンネル数", desc: "モノラル=1 / ステレオ=2。" },
        { sig: "ESampleFormat Format()", ret: "サンプル形式", desc: "<t>ESampleFormat</t>(16-bit 整数 or 32-bit float)。" },
        { sig: "u64 FrameCount()", ret: "フレーム数", desc: "1 フレーム = チャンネル数ぶんのサンプル。" },
        { sig: "const byte* Samples() / usize SampleByteCount()", desc: "インターリーブ済みの生サンプル先頭<t>ポインタ</t>と総バイト数。" },
        { sig: "f32 DurationSeconds()", ret: "秒", desc: "再生時間(秒)。<code>FrameCount / SampleRate</code>。" },
        { sig: "using FAudioAsset = AAudioAsset", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AAudioAsset</code> を使う。" }
      ]
    },
    {
      name: "ESampleFormat",
      kind: "列挙(enum)", header: "asset/AudioAsset.h",
      summary: "音声サンプルの<b>形式</b>。<code>PCM_S16</code>(16-bit 符号付き整数) / <code>PCM_F32</code>(32-bit 浮動小数)。",
      when: "音声データのバイト並びを正しく解釈する時。",
      sample: "if (au-&gt;Format() == ESampleFormat::PCM_F32) { /* float として読む */ }"
    },
    {
      name: "CWavAssetLoader / CMp3AssetLoader / CFlacAssetLoader / COggAssetLoader",
      kind: "クラス(IAssetLoader 派生)", header: "asset/AudioAsset.h",
      summary: "音声フォーマット<b>別の<t>ローダ</t></b>。それぞれ wav / mp3 / flac / ogg を担当し、デコード結果はすべて <t>AAudioAsset</t>(統一 PCM)になる。",
      when: "標準ローダ登録で自動使用。特定形式だけ手動で足したい時に個別登録。",
      sample: "CWavAssetLoader  wav;\nFOggAssetLoader  ogg;\nreg.RegisterLoader(&amp;wav);\nreg.RegisterLoader(&amp;ogg);",
      members: [
        { sig: "const char* Extension()", desc: "各クラスが \"wav\" / \"mp3\" / \"flac\" / \"ogg\" を返す。" },
        { sig: "using FFlacAssetLoader = CFlacAssetLoader", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CFlacAssetLoader</code> を使う。" },
        { sig: "using FMp3AssetLoader = CMp3AssetLoader", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CMp3AssetLoader</code> を使う。" },
        { sig: "using FOggAssetLoader = COggAssetLoader", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>COggAssetLoader</code> を使う。" },
        { sig: "using FWavAssetLoader = CWavAssetLoader", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CWavAssetLoader</code> を使う。" }
      ]
    },
    {
      name: "AMeshAsset",
      kind: "クラス(AAsset 派生)", header: "asset/MeshAsset.h",
      summary: "<b>単一メッシュ</b>。<t>頂点</t>配列 + <t>インデックス</t>配列 + サブメッシュ範囲を持つ。頂点は「位置 + 法線 + UV」固定(v1)。gltf/glb/obj/fbx に対応。",
      when: "3D モデルを読み込んで GPU メッシュにアップロードする時。",
      sample: "auto r = reg.Load(L\"models/box.glb\");\nauto* m = static_cast&lt;AMeshAsset*&gt;(r.Value().Get());\nusize vcount = m-&gt;Vertices().Size();\nusize icount = m-&gt;Indices().Size();",
      members: [
        { sig: "const TArray<FMeshVertex>& Vertices()", ret: "頂点配列", desc: "<t>FMeshVertex</t>(位置/法線/UV)の配列。非 const 版で編集も可。" },
        { sig: "const TArray<u32>& Indices()", ret: "インデックス配列", desc: "三角形を作る頂点<t>インデックス</t>。" },
        { sig: "const TArray<FSubMesh>& SubMeshes()", ret: "サブメッシュ配列", desc: "マテリアル単位の描画範囲(<t>FSubMesh</t>)。" },
        { sig: "using FMeshAsset = AMeshAsset", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AMeshAsset</code> を使う。" }
      ]
    },
    {
      name: "FMeshVertex",
      kind: "構造体", header: "asset/MeshAsset.h",
      summary: "標準<b>頂点フォーマット</b>。位置 <code>FVec3</code> + 法線 <code>FVec3</code> + UV(<code>u, v</code>)。<code>FVec3</code> が <code>alignas(16)</code> のため <code>sizeof</code> は <b>48 バイト</b>(末尾 8B パディング込み)。<small>※ ヘッダ <code>MeshAsset.h:19</code> のコメント「32 bytes」は FVec3 が 12B だった頃の古い記述。</small>",
      when: "メッシュの頂点を直接組み立て・読み取りする時。",
      sample: "FMeshVertex v;\nv.position = FVec3{0, 1, 0};\nv.normal   = FVec3{0, 1, 0};\nv.u = 0.5f; v.v = 0.0f;",
      members: [
        { sig: "FVec3 position", desc: "頂点座標。" },
        { sig: "FVec3 normal", desc: "法線ベクトル。" },
        { sig: "f32 u, v", desc: "テクスチャ <t>UV 座標</t>。" }
      ]
    },
    {
      name: "FSubMesh",
      kind: "構造体", header: "asset/MeshAsset.h",
      summary: "<b>サブメッシュ</b>。複数マテリアルに対応するため、インデックス配列内の開始位置と本数だけを持つ範囲指定。",
      when: "1 つのメッシュをマテリアルごとに分けて描画する時。",
      sample: "for (const FSubMesh&amp; s : mesh-&gt;SubMeshes()) {\n    DrawIndexed(s.first_index, s.index_count);\n}",
      members: [
        { sig: "u32 first_index", desc: "このサブメッシュが使う最初のインデックス位置。" },
        { sig: "u32 index_count", desc: "インデックス数。" }
      ]
    },
    {
      name: "CGltfAssetLoader / CGlbAssetLoader / CObjAssetLoader / CFbxAssetLoader",
      kind: "クラス(IAssetLoader 派生)", header: "asset/MeshAsset.h",
      summary: "メッシュ形式<b>別の<t>ローダ</t></b>。gltf/glb は <code>cgltf</code>、obj は自前パーサ、fbx は <code>ufbx</code> で読み、いずれも <t>AMeshAsset</t> を作る。",
      when: "標準ローダ登録で自動使用。特定形式だけ足したい時に個別登録。",
      sample: "CGltfAssetLoader gltf;\nFObjAssetLoader  obj;\nreg.RegisterLoader(&amp;gltf);\nreg.RegisterLoader(&amp;obj);",
      members: [
        { sig: "const char* Extension()", desc: "各クラスが \"gltf\" / \"glb\" / \"obj\" / \"fbx\" を返す。" },
        { sig: "using FFbxAssetLoader = CFbxAssetLoader", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CFbxAssetLoader</code> を使う。" },
        { sig: "using FGlbAssetLoader = CGlbAssetLoader", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CGlbAssetLoader</code> を使う。" },
        { sig: "using FGltfAssetLoader = CGltfAssetLoader", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CGltfAssetLoader</code> を使う。" },
        { sig: "using FObjAssetLoader = CObjAssetLoader", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CObjAssetLoader</code> を使う。" }
      ]
    },
    {
      name: "Primitive::MakeCube / MakeSphere / MakePlane",
      kind: "関数(名前空間 Primitive)", header: "asset/MeshPrimitive.h",
      summary: "アセットファイル不要で<b>立方体・球・平面を生成</b>する便利関数。<t>AMeshAsset</t> を <t>TSharedPtr</t> で返す。プロトタイピングやサンプルに便利。",
      when: "モデルを用意せずにとりあえず形を描きたい/動作確認したい時。",
      sample: "TSharedPtr&lt;AMeshAsset&gt; cube   = Primitive::MakeCube();\nTSharedPtr&lt;AMeshAsset&gt; sphere = Primitive::MakeSphere(1.0f, 32, 16);\nTSharedPtr&lt;AMeshAsset&gt; plane  = Primitive::MakePlane(10.0f, 10.0f);",
      members: [
        { sig: "bool TryMakeCube(f32 size, TSharedPtr<AMeshAsset>& output) noexcept", desc: "成功時だけ output を立方体へ置き換える。入力不正または確保失敗時は変更しない。" },
        { sig: "bool TryMakeSphere(f32 radius, u32 segments, u32 rings, TSharedPtr<AMeshAsset>& output) noexcept", desc: "成功時だけ output を球へ置き換える。入力不正、要素数超過、確保失敗時は変更しない。" },
        { sig: "bool TryMakePlane(f32 width, f32 depth, TSharedPtr<AMeshAsset>& output) noexcept", desc: "成功時だけ output を平面へ置き換える。入力不正または確保失敗時は変更しない。" },
        { sig: "TSharedPtr<AMeshAsset> MakeCube(f32 size = 1.0f)", ret: "立方体", desc: "中心原点の立方体。各面 4 頂点で法線と UV 0..1 を持つ。" },
        { sig: "TSharedPtr<AMeshAsset> MakeSphere(f32 radius = 0.5f, u32 segments = 32, u32 rings = 16)", ret: "球", desc: "UV 球(半径・横分割・縦分割)。" },
        { sig: "TSharedPtr<AMeshAsset> MakePlane(f32 width = 1.0f, f32 depth = 1.0f)", ret: "平面", desc: "XZ 平面(Y=0、法線 +Y)。" }
      ]
    },
    {
      name: "ASkinnedMeshAsset",
      kind: "クラス(AAsset 派生)", header: "asset/SkinnedMesh.h",
      summary: "<b>スキンメッシュ</b>(ボーン + アニメーション付き)。頂点(位置/法線/UV + ボーン indices/weights) + インデックス + <t>ボーン</t>階層 + アニメーション群を持つ。",
      when: "キャラクターなど骨で動かすモデルを扱う時。MVP ではプログラム的にデータを構築する。",
      sample: "ASkinnedMeshAsset mesh;\nmesh.Vertices().Add(FSkinnedVertex{ /* ... */ });\nmesh.Bones().Add(FBone{ /* ... */ });\nmesh.ComputeInverseBindMatrices();   // bind_* 設定後に 1 度",
      members: [
        { sig: "TArray<FSkinnedVertex>& Vertices()", ret: "頂点配列", desc: "<t>FSkinnedVertex</t>(64 バイト、ボーン影響付き)の配列。" },
        { sig: "TArray<u32>& Indices()", ret: "インデックス配列", desc: "三角形のインデックス。" },
        { sig: "TArray<FBone>& Bones()", ret: "ボーン配列", desc: "<t>FBone</t> の階層(parent でつながる)。" },
        { sig: "TArray<FAnimation>& Animations()", ret: "アニメ配列", desc: "<t>FAnimation</t> の一覧。" },
        { sig: "void ComputeInverseBindMatrices()", desc: "全 <code>bind_*</code> を設定してから 1 度呼ぶ。各 <code>FBone::inverse_bind</code> を計算する。", when: "ボーンのバインド姿勢を全部入れ終えた直後。" },
        { sig: "using FSkinnedMeshAsset = ASkinnedMeshAsset", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ASkinnedMeshAsset</code> を使う。" }
      ]
    },
    {
      name: "FSkinnedVertex",
      kind: "構造体", header: "asset/SkinnedMesh.h",
      summary: "スキンメッシュの<b>1 頂点</b>(64 バイト固定)。位置・法線・UV に加え、影響する<b>ボーン番号 4 つ</b>と<b>ウェイト 4 つ</b>を持つ。",
      when: "スキンメッシュの頂点を組み立てる時。",
      sample: "FSkinnedVertex v;\nv.position = FVec3{0,0,0};\nv.bones[0] = 3; v.weights[0] = 1.0f;  // ボーン3 に100%追従",
      members: [
        { sig: "FVec3 position / FVec3 normal", desc: "座標と法線。" },
        { sig: "f32 u, v", desc: "テクスチャ UV。" },
        { sig: "u8 bones[4]", desc: "影響する最大 4 ボーンの番号(未使用は 0)。" },
        { sig: "f32 weights[4]", desc: "各ボーンの重み(合計 1.0、未使用は 0)。" }
      ]
    },
    {
      name: "FBone",
      kind: "構造体", header: "asset/SkinnedMesh.h",
      summary: "<b><t>ボーン</t> 1 本</b>。名前・親番号・バインド姿勢(平行移動/回転/スケール)と、計算済みの逆バインド行列を持つ。",
      when: "スキンメッシュの骨格を定義する時。",
      sample: "FBone b;\nb.name = \"spine\";\nb.parent = 0;                 // -1 = ルート\nb.bind_translation = FVec3{0, 1, 0};",
      members: [
        { sig: "FString name", desc: "ボーン名。" },
        { sig: "i32 parent", desc: "親ボーンの番号。-1 ならルート。" },
        { sig: "FVec3 bind_translation / FQuat bind_rotation / FVec3 bind_scale", desc: "バインド姿勢の TRS。" },
        { sig: "FMat4 inverse_bind", desc: "逆バインド行列。<code>ComputeInverseBindMatrices()</code> で計算される。" }
      ]
    },
    {
      name: "FAnimation / FAnimationChannel / FAnimationKey",
      kind: "構造体", header: "asset/SkinnedMesh.h",
      summary: "<b>アニメーション</b>のデータ構造。<code>FAnimation</code>(名前+尺+チャネル群) → <code>FAnimationChannel</code>(対象ボーン+キー列) → <code>FAnimationKey</code>(時刻ごとの TRS)。",
      when: "ボーンアニメをプログラム的に作る時。",
      sample: "FAnimation anim;\nanim.name = \"walk\"; anim.duration = 1.0f;\nFAnimationChannel ch; ch.bone_index = 0;\nch.keys.Add(FAnimationKey{ /*time=*/0.0f /* ...TRS... */ });\nanim.channels.Add(Move(ch));",
      members: [
        { sig: "FAnimation: FString name; f32 duration; TArray<FAnimationChannel> channels", desc: "1 つのアニメ全体。" },
        { sig: "FAnimationChannel: i32 bone_index; TArray<FAnimationKey> keys", desc: "1 ボーン分の時刻昇順キー列。" },
        { sig: "FAnimationKey: f32 time; FVec3 translation; FQuat rotation; FVec3 scale", desc: "ある時刻の姿勢(TRS まとめて持つ)。" }
      ]
    },
    {
      name: "CAnimationPlayer",
      kind: "クラス", header: "asset/SkinnedMesh.h",
      summary: "スキンメッシュの<b>アニメ再生器</b>。時間を進めて、GPU 用の<t>ボーンパレット</t>(<code>FMat4</code> の配列)を毎フレーム書き出す。",
      when: "スキンメッシュを実際に動かして描画する時。",
      sample: "CAnimationPlayer ap;\nap.SetMesh(&amp;mesh);\nap.Play(0, /*loop=*/true);\nap.Update(dt);\nFMat4 palette[64];\nu32 count = ap.WritePalette(palette, 64);\nshader.SetBonePalette(palette, count);",
      members: [
        { sig: "void SetMesh(const ASkinnedMeshAsset* mesh)", desc: "再生対象のメッシュを設定する(アニメ・時刻はリセット)。" },
        { sig: "void Play(u32 anim_index, bool loop = true)", desc: "指定番号のアニメを再生開始する。" },
        { sig: "void Pause() / void Resume() / void Stop()", desc: "一時停止 / 再開 / 停止(Stop は時刻を 0 に)。" },
        { sig: "void SetTime(f32) / f32 Time() / bool IsPlaying()", desc: "再生時刻の設定・取得と再生中判定。" },
        { sig: "void Update(f32 dt)", desc: "経過時間 <code>dt</code> ぶん再生を進める。毎フレーム呼ぶ。" },
        { sig: "u32 WritePalette(FMat4* out, u32 max_count) const", ret: "ボーン数", desc: "現在時刻の<t>ボーンパレット</t>を <code>out</code> へ書き、書いたボーン数を返す。シェーダへ渡す行列群。" },
        { sig: "using FAnimationPlayer = CAnimationPlayer", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CAnimationPlayer</code> を使う。" }
      ]
    },
    {
      name: "ATextAsset",
      kind: "クラス(AAsset 派生)", header: "asset/TextAsset.h",
      summary: "<b>テキスト</b>(UTF-8 文字列)を保持するアセット。末尾 NUL 付きで C 文字列として取り出せる。txt/json/xml/yaml/csv/md/hlsl/glsl/lua 等に対応。",
      when: "設定ファイル・スクリプト・シェーダソースなどテキストを読み込む時。",
      sample: "auto r = reg.Load(L\"config/level.json\");\nauto* t = static_cast&lt;ATextAsset*&gt;(r.Value().Get());\nconst char* json = t-&gt;CStr();   // NUL 終端\nusize len = t-&gt;Size();",
      members: [
        { sig: "const char* CStr()", ret: "NUL 終端文字列", desc: "中身を C 文字列として返す(空なら \"\")。" },
        { sig: "usize Size()", ret: "文字数", desc: "NUL を除いたバイト長。" },
        { sig: "const TArray<char>& Raw()", ret: "生配列", desc: "末尾 NUL を含む生のバイト配列。" },
        { sig: "using FTextAsset = ATextAsset", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ATextAsset</code> を使う。" }
      ]
    },
    {
      name: "CTextAssetLoader",
      kind: "クラス(IAssetLoader 派生)", header: "asset/TextAsset.h",
      summary: "テキストファイルの<t>ローダ</t>。バイト列を <t>ATextAsset</t> にする。主要拡張子は \"txt\"。",
      when: "標準ローダ登録で自動使用。",
      sample: "CTextAssetLoader txt;\nreg.RegisterLoader(&amp;txt);",
      members: [
        { sig: "using FTextAssetLoader = CTextAssetLoader", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CTextAssetLoader</code> を使う。" }
      ]
    },
    {
      name: "ABinaryAsset",
      kind: "クラス(AAsset 派生)", header: "asset/BinaryAsset.h",
      summary: "<b>バイト列をそのまま保持</b>する最小アセット。専用ローダがない拡張子の<t>フォールバック</t>や、テストデータ用。",
      when: "形式不問で生バイトを読みたい時。専用型がない拡張子はこれで読まれる。",
      sample: "auto r = reg.Load(L\"data/save.bin\");\nauto* b = static_cast&lt;ABinaryAsset*&gt;(r.Value().Get());\nconst TArray&lt;byte&gt;&amp; raw = b-&gt;Bytes();",
      members: [
        { sig: "const TArray<byte>& Bytes()", ret: "バイト配列", desc: "保持している生バイト列。非 const 版で編集も可。" },
        { sig: "using FBinaryAsset = ABinaryAsset", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ABinaryAsset</code> を使う。" }
      ]
    },
    {
      name: "CBinaryAssetLoader",
      kind: "クラス(IAssetLoader 派生)", header: "asset/BinaryAsset.h",
      summary: "<b>拡張子フォールバック</b><t>ローダ</t>。<code>Extension()</code> が <code>\"*\"</code> を返し、専用ローダにマッチしない任意ファイルを <t>ABinaryAsset</t> として読む。",
      when: "未知の拡張子でも生バイトとして読めるよう保険として登録しておく(標準ローダ登録に含まれる)。",
      sample: "CBinaryAssetLoader fallback;\nreg.RegisterLoader(&amp;fallback);   // \"*\" で全拡張子を受ける",
      members: [
        { sig: "using FBinaryAssetLoader = CBinaryAssetLoader", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CBinaryAssetLoader</code> を使う。" }
      ]
    },
    {
      name: "kInvalidAssetId",
      kind: "定数", header: "asset/AssetId.h",
      summary: "<b>無効な <t>FAssetId</t></b> を表す定数(<code>FAssetId{0}</code>)。<code>value == 0</code> は「無効」として予約されており、<code>MakeAssetId</code> は結果が 0 になった場合 1 に補正してこの値と衝突しないようにする。",
      when: "未設定/失敗の ID を初期値として置く時や、<code>id != kInvalidAssetId</code> で有効性を判定したい時(<code>id.IsValid()</code> と同義)。",
      sample: "FAssetId id = kInvalidAssetId;       // まだ無効\nid = MakeAssetId(\"textures/hero.png\");\nif (id != kInvalidAssetId) { /* 有効 */ }",
      members: [
        { sig: "inline constexpr FAssetId kInvalidAssetId = FAssetId{0}", desc: "<code>value == 0</code> の <t>FAssetId</t>。無効値の唯一の表現。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "レジストリ": "読み込んだアセットをキー(<t>FAssetId</t>)で保持・共有し、再利用する管理表。ACS では <t>CAssetRegistry</t>。",
  "FAssetId": "パス文字列をハッシュした 64-bit のアセット識別子。同じパスなら必ず同じ ID。",
  "CAssetRegistry": "パスからアセットを読み込み、<t>TSharedPtr</t> でキャッシュ共有する中心クラス。",
  "FAssetFuture": "<t>非同期</t>ロードの結果を後で受け取るためのハンドル(<t>フューチャ</t>)。",
  "ローダ": "特定のファイル形式を解析してアセットを作る部品。<t>IAssetLoader</t> を実装し<t>レジストリ</t>へ登録する。",
  "ロード状態": "アセットが今どの段階か(未ロード/ロード中/利用可/失敗)。<t>EAssetState</t> で表す。",
  "型 ID": "アセットやローダの種類を表す数値(<t>AssetType</t>)。文字列名のハッシュから作られる。",
  "フューチャ": "「あとで完了する処理の結果」を表すハンドル。完了を待つ/確認することができる。",
  "ワークスチール": "暇なスレッドが他の未処理タスクを横取りして手伝う仕組み。待ち時間を有効活用する。",
  "完了カウンタ": "残タスク数を数える値。0 になったら処理完了とみなす同期用カウンタ。",
  "エラーコード": "失敗の種類を表す値。ACS では <t>Result</t> と組で扱う。",
  "ピクセルフォーマット": "画像 1 画素のチャンネル構成とビット深度(RGBA8 や float RGB など)。",
  "PCM": "音声を一定間隔で数値化した生波形データ。圧縮なしでそのまま再生できる形式。",
  "CAudioEngine": "PCM を実際に鳴らす音声再生エンジン(XAudio2 バックエンド)。",
  "頂点": "メッシュを構成する点。位置・法線・UV などの属性を持つ。",
  "インデックス": "頂点配列のどの点で三角形を作るかを示す番号の列。同じ頂点を使い回せる。",
  "UV 座標": "テクスチャ上のどこを貼るかを示す 0..1 の 2 次元座標。",
  "FMeshVertex": "ACS 標準の頂点(位置+法線+UV)。<code>FVec3</code> が 16B 整列のため sizeof=48 バイト。",
  "FSubMesh": "メッシュをマテリアル単位に分ける描画範囲(開始インデックスと本数)。",
  "ボーン": "スキンメッシュを動かす骨。階層を組み、頂点がこれに追従して変形する。",
  "ボーンパレット": "全ボーンの変形行列をまとめた配列。シェーダへ渡してスキニングに使う。",
  "FSkinnedVertex": "ボーン番号とウェイトを持つスキンメッシュの頂点(64 バイト)。",
  "FBone": "スキンメッシュの 1 本のボーン(名前・親・バインド姿勢)。",
  "FAnimation": "1 つのボーンアニメーション(尺と時刻ごとの姿勢キー)。",
  "AImageAsset": "画像 1 枚分の CPU 側ピクセルデータを持つアセット。",
  "AAudioAsset": "統一 PCM で音声を保持するアセット。",
  "AMeshAsset": "頂点+インデックス+サブメッシュからなる単一メッシュのアセット。",
  "ATextAsset": "UTF-8 テキストを保持するアセット。",
  "ABinaryAsset": "生バイト列をそのまま持つ最小アセット(フォールバック)。",
  "IAssetLoader": "ファイル形式ごとのローダ共通インターフェース。",
  "EAssetState": "アセットのロード状態を表す列挙。",
  "AssetType": "アセット型を識別する 32-bit の数値 ID。",
  "EPixelFormat": "画像のピクセルフォーマットを表す列挙。",
  "ESampleFormat": "音声サンプルの形式(16-bit 整数 / 32-bit float)を表す列挙。",
  "フォールバック": "本命が無い時に使う予備の手段。ここでは専用ローダが無い拡張子の受け皿。",
  "FStringView": "文字列を所有せず参照だけする軽量ビュー。"
});
