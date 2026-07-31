/* ACS リファレンス — foundation モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "foundation",
  order: 5,
  title: "foundation — 基盤型 / Result / ログ・パニック",
  blurb: "ACS の一番下の土台。<code>u32</code>/<code>f32</code> などの<b>短い型名</b>、例外を使わずに失敗を返す <t>TResult</t>、<t>アサート</t>やログといった、どのモジュールも頼る共通部品を提供します。STL（標準ライブラリ）に依存しないのが特徴です。",
  types: [
    {
      name: "基本型エイリアス (u8〜u64 / i8〜i64 / f32 / f64 / usize 等)",
      kind: "型エイリアス", header: "foundation/Types.h",
      summary: "ビット幅が一目で分かる<b>短い数値型名</b>。<code>unsigned int</code> ではなく <code>u32</code>、<code>float</code> ではなく <code>f32</code> と書く。どのプラットフォームでも幅が固定（<code>u32</code> は必ず 4 バイト）なので移植で困らない。",
      when: "ACS では <code>int</code> や <code>long</code> の代わりに常にこれらを使う。サイズが重要な構造体やファイル I/O で特に効く。",
      sample: "acs::u32 count = 0;        // 32bit 符号なし\nacs::i64 offset = -100;    // 64bit 符号付き\nacs::f32 x = 1.5f;         // 単精度浮動小数\nacs::usize n = sizeof(T);  // サイズ・添字用(ポインタ幅と同一)\nacs::byte raw[16];         // 生バイト列",
      members: [
        { sig: "u8 / u16 / u32 / u64", desc: "符号なし整数（8/16/32/64 ビット）。" },
        { sig: "i8 / i16 / i32 / i64", desc: "符号付き整数（8/16/32/64 ビット）。" },
        { sig: "f32 / f64", desc: "単精度 / 倍精度の浮動小数（IEEE 754）。" },
        { sig: "usize / isize", desc: "<code>usize</code>=サイズ・配列添字用（ポインタ幅と同一）。<code>isize</code>=ポインタ差分用。" },
        { sig: "uptr / iptr", desc: "ポインタを整数として扱う時の符号なし / 符号付き型。" },
        { sig: "byte / c8 / c16 / c32", desc: "<code>byte</code>=生バイト。<code>c8/c16/c32</code>=UTF-8/16/32 文字。" }
      ]
    },
    {
      name: "TResult&lt;T, E&gt;",
      kind: "クラステンプレート", header: "foundation/Result.h",
      summary: "<b>成功値 T かエラー値 E のどちらか一方</b>を持つ型（Rust の <code>Result</code> / C++ の <code>std::expected</code> 相当）。ACS は<t>例外</t>を使わず、失敗をこの戻り値で伝える。E の既定は <t>FErrorCode</t>。<code>[[nodiscard]]</code> 付きなので戻り値を捨てると警告。",
      when: "失敗し得る関数（ファイルを開く・確保する・パースする等）の戻り型に使う。呼ぶ側は <code>if (r)</code> や <code>IsOk()</code> でまず成否を確認する。",
      sample: "TResult&lt;FFile&gt; r = OpenFile(\"foo\");\nif (!r) {                              // エラーなら\n    ACS_LOG_ERROR(\"開けません: %s\", r.Error().message);\n    return;\n}\nFFile&amp; f = r.Value();                  // 成功 → 値を取り出す\nint n = r.ValueOr(0);                  // ※ T=int の例: 失敗なら 0",
      members: [
        { sig: "bool IsOk() const / IsErr() const", ret: "成否", desc: "成功側を持っていれば <code>IsOk()</code> が true。", when: "値やエラーに触る前に必ず確認する。" },
        { sig: "explicit operator bool() const", desc: "<code>if (r)</code> で成功判定できる（<code>IsOk()</code> と同じ）。" },
        { sig: "T& Value()", ret: "成功値の参照", desc: "成功側の値を取り出す。<b>エラー状態で呼ぶと <t>アサート</t>で即停止</b>するので、先に <code>IsOk()</code> を確認すること。" },
        { sig: "E& Error()", ret: "エラー値の参照", desc: "エラー側の値（既定は <code>FErrorCode</code>）を取り出す。成功状態で呼ぶとアサートで停止。" },
        { sig: "T ValueOr(T fallback) const", ret: "値 or 既定", desc: "成功なら値、失敗なら渡した <code>fallback</code> を返す。分岐を書かずに既定値で済ませたい時に便利。", sample: "u32 size = ReadSize().ValueOr(0u);" }
      ]
    },
    {
      name: "TResult&lt;void, E&gt;",
      kind: "クラステンプレート(特殊化)", header: "foundation/Result.h",
      summary: "<b>返す値が無い</b>処理用の <t>TResult</t>。「成功したか、失敗したか」だけを表す。",
      when: "保存・初期化・送信など、結果の<b>成否だけ</b>知りたい関数の戻り型に使う。",
      sample: "TResult&lt;void&gt; Save() {\n    ACS_TRY(WriteHeader());   // 失敗なら即 return\n    return Ok();              // 成功\n}\n\nif (auto r = Save(); !r)\n    ACS_LOG_ERROR(\"save failed\");",
      members: [
        { sig: "TResult()", desc: "<b>既定構築は成功</b>。引数なしで返すと「成功」を意味する。" },
        { sig: "bool IsOk() / IsErr() / operator bool()", desc: "成否を判定する（値版と同じ）。" },
        { sig: "E& Error()", ret: "エラー値", desc: "失敗時のエラーを取り出す。成功状態で呼ぶとアサート停止。" }
      ]
    },
    {
      name: "Ok / Err",
      kind: "ヘルパー関数", header: "foundation/Result.h",
      summary: "<t>TResult</t> を簡潔に作る入口。<code>Ok(v)</code> で成功側、<code>Err(e)</code> でエラー側を生成する。<code>Ok()</code>（引数なし）は <code>TResult&lt;void&gt;</code> の成功。",
      when: "関数の中で結果を返す時。成功なら <code>Ok(...)</code>、失敗なら <code>Err(...)</code> または <code>ACS_ERR(...)</code> をそのまま <code>return</code> する。",
      sample: "TResult&lt;u32&gt; Parse(const char* s) {\n    if (!s) return Err&lt;u32&gt;(ACS_ERR(IO, 1, \"null 入力\"));\n    return Ok(ToU32(s));   // 成功側\n}",
      members: [
        { sig: "TResult<T> Ok(T v)", desc: "値を成功側に包んだ <code>TResult&lt;T&gt;</code> を返す。" },
        { sig: "TResult<void> Ok()", desc: "<code>TResult&lt;void&gt;</code> の成功を返す。" },
        { sig: "TResult<T> Err(FErrorCode e)", desc: "エラーを包んだ <code>TResult&lt;T&gt;</code> を返す（<code>T</code> 既定は <code>void</code>）。" }
      ]
    },
    {
      name: "ACS_TRY / ACS_TRY_ASSIGN",
      kind: "マクロ", header: "foundation/Result.h",
      summary: "Rust の <code>?</code> 演算子に相当する<b>早期リターン</b>。<t>TResult</t> を返す処理を呼び、エラーならその場で関数から抜ける。エラー処理の <code>if</code> 連打を消せる。",
      when: "ある関数の中で、失敗し得る別関数を次々呼びたい時。途中で失敗したら呼び出し元へエラーを素通しできる。",
      sample: "TResult&lt;void&gt; Load() {\n    ACS_TRY(OpenFile());              // 失敗なら return Err(...)\n    ACS_TRY_ASSIGN(data, ReadAll()); // 成功なら data に束縛\n    Process(data);\n    return Ok();\n}",
      members: [
        { sig: "ACS_TRY(expr)", desc: "<code>expr</code> を評価し、エラーなら <code>Err(...)</code> を <code>return</code>。値は捨てる（<code>TResult&lt;void&gt;</code> 向け）。" },
        { sig: "ACS_TRY_ASSIGN(name, expr)", desc: "<code>expr</code> がエラーなら <code>return</code>、成功なら値を <code>name</code> という名前に束縛する。" }
      ]
    },
    {
      name: "FErrorCode",
      kind: "構造体", header: "foundation/Error.h",
      summary: "<t>TResult</t> の既定エラー型。<b>カテゴリ + サブコード + OS エラー値 + メッセージ + 発生位置</b>をまとめた軽い <t>POD</t>。例外の代わりに「何がどこで失敗したか」を運ぶ。",
      when: "失敗を表現する時の標準の入れ物。普段は手書きせず <code>ACS_ERR</code> マクロで作る。受け取った側はカテゴリで分類したり <code>message</code> を出力したりする。",
      sample: "FErrorCode e = ACS_ERR(IO, 42, \"ファイルが見つかりません\");\nif (e) {                                  // bool 変換: エラーあり\n    ACS_LOG_ERROR(\"[%s] %s (%s:%u)\",\n        ToString(e.category), e.message,\n        e.loc.File(), e.loc.Line());\n}",
      members: [
        { sig: "EErrCategory category", desc: "大分類（<code>IO</code> / <code>Memory</code> / <code>Render</code> 等）。<code>None</code> は成功を意味する。" },
        { sig: "u16 subcode", desc: "モジュール固有の細分化コード。カテゴリ内での区別に使う。" },
        { sig: "u32 os_error", desc: "<code>GetLastError()</code> や <code>HRESULT</code> など OS 由来のエラー値（任意）。" },
        { sig: "const char* message", desc: "人間向けメッセージ。静的文字列リテラル推奨。" },
        { sig: "FSourceLoc loc", desc: "発生位置（ファイル/行/関数）。<code>ACS_ERR</code> が自動で埋める。" },
        { sig: "bool IsOk() const", desc: "<code>category == None</code> なら true（＝成功状態）。" },
        { sig: "explicit operator bool() const", desc: "<b>true なら「エラーあり」</b>。<code>if (e) { ... }</code> で失敗を判定する。" }
      ]
    },
    {
      name: "EErrCategory",
      kind: "列挙(enum)", header: "foundation/Error.h",
      summary: "エラーの<b>大分類</b>を表す <code>enum class</code>（基底は <code>u16</code>）。ロガーでのフィルタや、原因の大まかな切り分けに使う。",
      when: "<code>ACS_ERR</code> の第 1 引数に渡す。受け取り側はこの値で <code>switch</code> したり <code>ToString</code> でログに出したりする。",
      sample: "switch (e.category) {\n    case EErrCategory::IO:     /* 入出力 */ break;\n    case EErrCategory::Memory: /* 確保失敗 */ break;\n    default: break;\n}\nconst char* name = ToString(e.category);  // \"IO\" 等",
      members: [
        { sig: "None = 0", desc: "成功（エラー無し）。" },
        { sig: "Generic / Memory / OS / IO", desc: "一般 / メモリ確保失敗 / OS コール失敗 / ファイル・ネットワーク I/O。" },
        { sig: "Container / Threading / Math", desc: "範囲外・容量超過 / 排他制御・スレッド / 数学領域（NaN・0 除算）。" },
        { sig: "Render / Asset / UserCancel", desc: "描画バックエンド失敗 / アセット読み込み失敗 / ユーザーキャンセル。" },
        { sig: "const char* ToString(EErrCategory)", desc: "カテゴリを文字列化する（ログ出力用）。" }
      ]
    },
    {
      name: "ACS_ERR / ACS_ERR_OS",
      kind: "マクロ", header: "foundation/Error.h",
      summary: "<t>FErrorCode</t> を 1 行で作るマクロ。<b>発生位置を自動でキャプチャ</b>するので、どこで失敗したかが常に残る。",
      when: "失敗を返したい箇所すべて。手で <code>FSourceLoc</code> を埋める必要がない。",
      sample: "return ACS_ERR(IO, 1, \"開けません\");\n// OS のエラー値も一緒に運ぶ:\nreturn ACS_ERR_OS(OS, 5, \"CreateFile 失敗\", GetLastError());",
      members: [
        { sig: "ACS_ERR(cat, sub, msg)", desc: "カテゴリ <code>cat</code>・サブコード <code>sub</code>・メッセージ <code>msg</code> でエラーを生成。位置は自動。" },
        { sig: "ACS_ERR_OS(cat, sub, msg, os_err)", desc: "上記に加え OS エラー値 <code>os_err</code> も格納する。" }
      ]
    },
    {
      name: "FSourceLoc",
      kind: "クラス", header: "foundation/SourceLoc.h",
      summary: "<b>ソース上の位置</b>（ファイル名・関数名・行・列）を持つ軽い型（<code>std::source_location</code> 相当）。コンパイラ組み込みで取得する。",
      when: "「どこで呼ばれたか / どこで失敗したか」を記録したい時。引数の既定値に <code>FSourceLoc::Current()</code> を置くと、<b>呼び出し側の位置</b>を自動取得できる。",
      sample: "void LogHere(FSourceLoc loc = FSourceLoc::Current()) {\n    ACS_LOG_INFO(\"%s:%u (%s)\",\n        loc.File(), loc.Line(), loc.Function());\n}\nLogHere();  // ← この行の位置が記録される",
      members: [
        { sig: "static consteval FSourceLoc Current(...)", ret: "呼び出し位置", desc: "呼び出された場所の情報を取得する。引数の既定値に置くのが定石。" },
        { sig: "const char* File() const", desc: "ソースファイルのパス。" },
        { sig: "const char* Function() const", desc: "関数名。" },
        { sig: "u32 Line() const / u32 Column() const", desc: "行番号 / 列番号。" }
      ]
    },
    {
      name: "アサートマクロ (ACS_ASSERT 系)",
      kind: "マクロ", header: "foundation/Assert.h",
      summary: "条件が偽なら <t>FPanic</t> を呼んで<b>その場で止める</b>チェック群。バグを早期に・確実に検出する。<b>デバッグ専用</b>（リリースで消える）と<b>常時有効</b>の 2 系統がある。",
      when: "事前条件・不変条件・到達しないはずの分岐を主張する時。安いチェックは <code>ACS_ASSERT</code>、リリースでも守りたい不変条件は <code>ACS_CHECK</code>。",
      sample: "ACS_ASSERT(idx &lt; size);                         // デバッグのみ\nACS_ASSERTF(p != nullptr, \"id=%u\", id);          // メッセージ付き\nACS_CHECK(buffer != nullptr);                    // リリースでも検査\nif (kind == 0) { /*...*/ } else { ACS_NOTREACHED(); }",
      members: [
        { sig: "ACS_ASSERT(expr)", desc: "<b>デバッグ専用</b>。偽なら停止。リリース（<code>-DACS_ENABLE_ASSERTS=OFF</code>）では除去。", when: "安価な事前条件・範囲チェック。" },
        { sig: "ACS_ASSERTF(expr, fmt, ...)", desc: "<code>ACS_ASSERT</code> に printf 風メッセージを付けた版。" },
        { sig: "ACS_VERIFY(expr)", desc: "デバッグ専用だが <b>expr は常に評価される</b>。副作用のある式のチェックに使う。" },
        { sig: "ACS_CHECK(expr) / ACS_CHECKF(...)", desc: "<b>常に有効</b>。リリースでも偽なら停止。範囲外・null・セキュリティ前提など倒れた方が安全な不変条件に。" },
        { sig: "ACS_NOTREACHED()", desc: "到達したら必ず停止する防御マーカー（リリースでも有効）。" },
        { sig: "ACS_NOT_IMPLEMENTED()", desc: "未実装の分岐に置く。リリースでもパニックする。" },
        { sig: "ACS_UNREACHABLE()", desc: "「絶対に来ない」と最適化器に教える。デバッグでは停止、リリースでは最適化ヒント（来ると<t>未定義動作</t>）。" }
      ]
    },
    {
      name: "FPanic / SetPanicHook",
      kind: "関数", header: "foundation/Panic.h",
      summary: "<b>最終エラー経路</b>。場所・失敗式・メッセージ・<t>スタックトレース</t>を出力した後、プロセスを終了させる（戻らない）。通常は直接呼ばず <t>アサート</t>経由で呼ばれる。",
      when: "致命的でこれ以上続行できない時。<code>SetPanicHook</code> でパニック直前の処理（ログのフラッシュ、クラッシュレポート保存）を差し込める。",
      sample: "// パニック直前にログを書き出す\nSetPanicHook([](void*, const char* msg, acs::usize) {\n    FLogger::Flush();\n}, nullptr);\n// 直接呼ぶことは稀（普通は ACS_ASSERT 経由）",
      members: [
        { sig: "void FPanic(FSourceLoc, const char* expr, const char* fmt, ...)", desc: "パニックを発生させ、出力後にプロセス終了する。<code>[[noreturn]]</code> なので戻らない。" },
        { sig: "void SetPanicHook(PanicHook hook, void* user)", desc: "パニック直前に呼ぶフックを登録する。スレッドセーフ。", when: "クラッシュ時にログをフラッシュしたい等。" },
        { sig: "using PanicHook = void(*)(void* user, const char* msg, usize len)", desc: "フック関数の型。<code>user</code> は登録時に渡したポインタ。" }
      ]
    },
    {
      name: "FLogger",
      kind: "クラス", header: "foundation/Log.h",
      summary: "<b>非同期でスレッド安全</b>なロガー。<code>printf</code> 互換の書式で記録でき、専用の書き込みスレッドがファイル/コンソールと登録済み通知先へ出す。通常は <code>ACS_LOG_*</code> マクロ経由で使う。",
      when: "デバッグ出力・診断ログを残したい時。<code>Init</code> で一度設定し、各所では <code>ACS_LOG_INFO</code> 等を呼ぶだけ。",
      sample: "FLogConfig cfg;\ncfg.min_severity = ELogSeverity::Debug;\ncfg.file_path = L\"game.log\";\nFLogger::Init(cfg);\n\nACS_LOG_INFO(\"起動 %s build\", ACS_PLATFORM_NAME);\nACS_LOG_WARN(\"残り HP %d\", hp);\nFLogger::Shutdown();",
      members: [
        { sig: "static void Init(const FLogConfig& cfg)", desc: "ロガーを初期化する（多重呼び出しは無視）。" },
        { sig: "static void Shutdown()", desc: "書き込みスレッドと実行中 sink callback を待ってリソースを解放する。外部呼び出しの復帰後は非所有 user を破棄できる。" },
        { sig: "static void Flush()", desc: "未書き込みのレコードを全部出すまで待つ。" },
        { sig: "static FLogSinkHandle SubscribeSink(LogSinkCallback callback, void* user)", desc: "非所有の通知先を最大4096件まで登録する。severity はレコード値、message は writer 所有の null終端コピーで callback 中だけ有効。保持時は複製し、user は成功した外部解除または Shutdown 復帰まで生存させる。" },
        { sig: "static FLogSinkSubscription SubscribeSinkOwned(LogSinkCallback callback, void* user)", desc: "破棄時に自動解除する移動専用の購読を返す。" },
        { sig: "static bool UnsubscribeSink(FLogSinkHandle handle)", desc: "通知先を解除する。callback 外で true を返した時点で実行中通知も終わり、非所有 user を破棄できる。" },
        { sig: "static bool IsSinkSubscribed(FLogSinkHandle handle)", desc: "現在の Logger 世代で購読が有効かを返す。" },
        { sig: "static u32 SinkCount()", desc: "現在有効な複数利用者向け通知先の数を返す。" },
        { sig: "static bool TryCopySinkHandles(FLogSinkHandle* output, u32 capacity, u32& count)", desc: "共有 lock 内の登録順 snapshot を一括コピーする。0件でも output は非 null。容量・整列・桁あふれ・output と count のbyte重複が無効なら両出力を変更しない。" },
        { sig: "static void SetMinSeverity(ELogSeverity s)", desc: "出力する最小レベルを実行中に変更する（スレッドセーフ）。" },
        { sig: "static bool Enabled(ELogSeverity s)", desc: "そのレベルが出力対象かを返す（マクロが内部で使う）。" },
        { sig: "static u64 DroppedCount()", desc: "リング満杯で破棄したレコード総数。負荷時の取りこぼし確認用。" }
      ]
    },
    {
      name: "FLogSinkHandle / FLogSinkSubscription",
      kind: "構造体 / クラス", header: "foundation/LogSinkHandle.h / foundation/LogSinkSubscription.h",
      summary: "複数利用者向けログ通知先の世代付き識別子と、破棄時に解除する移動専用所有権。",
      when: "エディタ、開発用表示、外部診断など複数の利用者が同じ <t>FLogger</t> のログを受け取る時。",
      sample: "auto subscription = FLogger::SubscribeSinkOwned(\n    [](ELogSeverity severity, const char* message, void* user) noexcept {\n        ConsumeLog(user, severity, message);\n    }, context);\n// subscription の破棄時に自動解除",
      members: [
        { sig: "bool FLogSinkHandle::IsValid() const", desc: "枠番号と世代番号が有効範囲内かを返す。" },
        { sig: "bool FLogSinkSubscription::IsValid() const", desc: "保持中の購読が現在も有効かを返す。" },
        { sig: "bool FLogSinkSubscription::Reset()", desc: "保持中の購読を解除する。" },
        { sig: "FLogSinkHandle FLogSinkSubscription::Handle() const", desc: "保持中の世代付きハンドルを返す。" }
      ]
    },
    {
      name: "ACS_LOG / ACS_LOG_* マクロ",
      kind: "マクロ", header: "foundation/Log.h",
      summary: "<t>FLogger</t> に 1 行書くための入口。レベル別のショートカットがあり、<b>無効レベルなら書式評価ごと省く</b>ので軽い。発生位置は自動で付く。",
      when: "コードのあちこちでログを出す時。普段はこのマクロだけ使えばよい。",
      sample: "ACS_LOG_TRACE(\"x=%f\", x);\nACS_LOG_INFO(\"loaded %u assets\", n);\nACS_LOG_ERROR(\"open failed: %s\", path);",
      members: [
        { sig: "ACS_LOG_TRACE / DEBUG / INFO", desc: "詳細トレース / デバッグ情報 / 通常情報。" },
        { sig: "ACS_LOG_WARN / ERROR / FATAL", desc: "警告 / エラー / 致命エラー。" },
        { sig: "ACS_LOG(sev, fmt, ...)", desc: "レベルを直接指定する基底マクロ。" }
      ]
    },
    {
      name: "ELogSeverity",
      kind: "列挙(enum)", header: "foundation/Log.h",
      summary: "ログの<b>重要度レベル</b>（基底 <code>u8</code>）。数値が小さいほど詳細。最小レベル以上のものだけが出力される。",
      when: "<code>FLogConfig::min_severity</code> や <code>SetMinSeverity</code> でしきい値を決める時。",
      sample: "FLogger::SetMinSeverity(ELogSeverity::Warn);\n// → Warn / Error / Fatal だけ出力、Info 以下は無視\nconst char* lv = ToString(ELogSeverity::Error);  // \"ERROR\"",
      members: [
        { sig: "Trace=0 / Debug=1 / Info=2", desc: "詳細トレース / デバッグ / 通常情報。" },
        { sig: "Warn=3 / Error=4 / Fatal=5", desc: "警告 / エラー / 致命エラー。" },
        { sig: "Off=6", desc: "出力を完全に止める。" },
        { sig: "const char* ToString(ELogSeverity)", desc: "レベルを文字列化する。" }
      ]
    },
    {
      name: "FLogConfig",
      kind: "構造体", header: "foundation/Log.h",
      summary: "<t>FLogger</t> の初期設定。出力先（ファイル/コンソール/デバッグ出力）と最小レベル、内部リングの長さを指定する。",
      when: "<code>FLogger::Init</code> に渡す。ゲーム起動時に一度だけ作る。",
      sample: "FLogConfig cfg;\ncfg.file_path = L\"game.log\";       // nullptr ならファイル無効\ncfg.min_severity = ELogSeverity::Info;\ncfg.console = true;\nFLogger::Init(cfg);",
      members: [
        { sig: "const wchar_t* file_path", desc: "出力ファイルのパス。<code>nullptr</code> ならファイル出力なし。" },
        { sig: "ELogSeverity min_severity", desc: "これ未満のレベルは出力しない（既定 <code>Info</code>）。" },
        { sig: "u32 ring_capacity", desc: "内部リングの長さ（2 のべき乗、最小 16）。負荷時の取りこぼしに影響。" },
        { sig: "bool console / bool debug_output", desc: "標準出力に出すか / <code>OutputDebugString</code> に出すか。" }
      ]
    },
    {
      name: "FStackTrace / FStackFrame",
      kind: "クラス / 構造体", header: "foundation/StackTrace.h",
      summary: "<b>呼び出し履歴（コールスタック）</b>を取得し、関数名・ファイル・行に解決する。パニック時の診断に使われる。<code>FStackFrame</code> は 1 段分の情報。",
      when: "クラッシュやアサート失敗の原因を「どの関数から来たか」で追いたい時。普通は <t>FPanic</t> が裏で使う。",
      sample: "FStackTrace st;\nst.Capture();    // 現在のスタックを取得\nst.Resolve();    // シンボル/ファイル/行に解決\nst.Print([](void*, const char* line, acs::usize) {\n    ACS_LOG_INFO(\"%s\", line);\n}, nullptr);",
      members: [
        { sig: "void Capture(u32 skip = 1)", desc: "現在のスタックを取得する。<code>skip</code> 段は除外（自身のフレーム等）。" },
        { sig: "void Resolve()", desc: "取得済みアドレスを関数名/ファイル/行に解決する（スレッドセーフ）。" },
        { sig: "u32 FrameCount() const / const FStackFrame& Frame(u32 i) const", desc: "フレーム数と各フレームの取得。" },
        { sig: "void Print(Sink sink, void* user) const", desc: "1 行ずつテキスト化して <code>sink</code> コールバックに渡す。" },
        { sig: "using Sink = void(*)(void* user, const char* line, usize len)", desc: "<code>Print</code> が 1 行ごとに呼ぶコールバック型。<code>user</code> は呼び出し時に渡したポインタがそのまま渡る。" },
        { sig: "FStackFrame { void* address; u64 line; char symbol[256]; char file[256]; }", desc: "1 フレーム分の情報。命令ポインタ・ソース行番号（失敗時 0）・関数名（失敗時 <code>\"??? @ 0xADDR\"</code>）・ファイルパス。" },
        { sig: "inline constexpr u32 kStackTraceMaxFrames = 64", desc: "取得する最大フレーム数（名前空間スコープ定数）。" }
      ]
    },
    {
      name: "Move / Forward / Swap / Min / Max / Clamp",
      kind: "関数テンプレート", header: "foundation/Move.h",
      summary: "<code>&lt;utility&gt;</code> / <code>&lt;algorithm&gt;</code> の代替となる超基本ユーティリティ。<code>Move</code>=<t>ムーブ</t>へのキャスト、<code>Forward</code>=<t>完全転送</t>、<code>Swap</code>=入れ替え、<code>Min/Max/Clamp</code>=比較。すべて <code>constexpr</code> で最適化時に消える。",
      when: "所有権を移したい（<code>Move</code>）、テンプレートで受けた引数をそのまま渡したい（<code>Forward</code>）、値を範囲内に収めたい（<code>Clamp</code>）時など、日常的に使う。",
      sample: "TUniquePtr&lt;T&gt; b = Move(a);          // a の中身を b へ移譲\nf32 v = Clamp(x, 0.0f, 1.0f);        // 0〜1 に丸める\nMin(hp, maxHp);  Max(0, dmg);        // 比較\nSwap(lhs, rhs);                       // 入れ替え",
      members: [
        { sig: "RemoveRefT<T>&& Move(T&& v)", desc: "値を rvalue 参照にキャストし、ムーブ可能にする（<code>std::move</code> 相当）。" },
        { sig: "T&& Forward<T>(...)", desc: "受け取った引数を lvalue/rvalue の区別を保ったまま転送する（<code>std::forward</code> 相当）。" },
        { sig: "void Swap(T& a, T& b)", desc: "2 つの値をムーブで入れ替える。" },
        { sig: "T Min(T,T) / T Max(T,T) / T Clamp(T v, T lo, T hi)", desc: "最小 / 最大 / 範囲内へクランプ。整数・浮動小数どちらも可。" }
      ]
    },
    {
      name: "配置 new (placement new)",
      kind: "演算子", header: "foundation/Move.h",
      summary: "<b>すでに確保済みのメモリ上にオブジェクトを構築する</b>ための <code>::new (ptr) T(...)</code> 構文を有効にする宣言。STL（<code>&lt;new&gt;</code>）を取り込まずに自前で用意している。",
      when: "<t>アロケータ</t>や <code>TResult</code> など、確保と構築を分けて扱う低レベルコードで使う。通常のゲームコードで直接書くことは稀。",
      sample: "void* mem = alloc.Alloc(sizeof(T), alignof(T), FSourceLoc::Current());\nT* obj = ::new (mem) T(args...);   // mem の上に構築\nobj-&gt;~T();                          // 破棄は手動"
    },
    {
      name: "TNumLimits&lt;T&gt;",
      kind: "クラステンプレート", header: "foundation/Limits.h",
      summary: "数値型の<b>最小値・最大値・イプシロン・無限大</b>を返す（<code>std::numeric_limits</code> 代替）。すべて <code>constexpr</code>。",
      when: "「ありえない大きな初期値」を入れたい、桁あふれを避けたい、浮動小数の比較許容差が欲しい時など。",
      sample: "u32 best = TNumLimits&lt;u32&gt;::Max();   // 最大で初期化\nfor (auto v : xs) best = Min(best, v);\nf32 eps = TNumLimits&lt;f32&gt;::Epsilon();\nif (Abs(a - b) &lt; eps) { /* ほぼ等しい */ }",
      members: [
        { sig: "static constexpr T Min() / Max()", desc: "その型の最小値 / 最大値。整数は表現可能な範囲、浮動小数は最小正規化数 / 最大有限値。" },
        { sig: "static constexpr T Epsilon()  // f32/f64 のみ", desc: "1.0 と次に表現できる値の差。浮動小数の「ほぼ等しい」比較に使う。" },
        { sig: "static constexpr T Infinity() // f32/f64 のみ", desc: "正の無限大。" }
      ]
    },
    {
      name: "型特性 (IsSameV / RemoveRefT / EnableIfT 等)",
      kind: "型特性テンプレート", header: "foundation/TypeTraits.h",
      summary: "<code>&lt;type_traits&gt;</code> の最小代替。型を<b>調べる</b>（整数か・ポインタか・同じ型か）／<b>変形する</b>（参照や const を外す）／<b>テンプレートを条件付きで有効化</b>する道具一式。主にテンプレートを書く時の部品。",
      when: "汎用コンテナやアロケータ、<code>TResult</code> のようなテンプレートを自作・拡張する時。普通のゲームロジックでは表に出ない。",
      sample: "static_assert(IsSameV&lt;u32, unsigned int&gt;);\nusing Bare = RemoveCVRefT&lt;const int&amp;&gt;;   // → int\n// 整数型のときだけ有効になる関数:\ntemplate&lt;typename T, typename = EnableIfT&lt;IsIntegralV&lt;T&gt;&gt;&gt;\nT Double(T x) { return x * 2; }",
      members: [
        { sig: "bool IsSameV<A,B>", desc: "2 つの型が同一か。" },
        { sig: "RemoveRefT<T> / RemoveConstT<T> / RemoveCVT<T> / RemoveCVRefT<T>", desc: "参照 / const / cv / その両方を取り除いた素の型を得る。" },
        { sig: "bool IsIntegralV<T> / IsFloatingV<T> / IsArithmeticV<T>", desc: "整数型 / 浮動小数型 / 算術型かどうか。" },
        { sig: "bool IsPointerV<T> / IsLvalueRefV<T> / IsRvalueRefV<T>", desc: "ポインタ / lvalue 参照 / rvalue 参照かどうか。" },
        { sig: "bool IsTriviallyCopyableV<T> / IsTriviallyDestructibleV<T> / IsTriviallyConstructibleV<T>", desc: "コンパイラ組み込み trait の薄いラッパ（自明コピー可 / 自明破棄可 / 自明構築可）。" },
        { sig: "bool IsEnumV<T> / IsEmptyV<T> / IsAbstractV<T> / IsBaseOfV<Base,Derived>", desc: "列挙型か / 空クラス（メンバ無し）か / 抽象クラス（純粋仮想あり）か / <code>Base</code> が <code>Derived</code> の基底かを判定する組み込み trait ラッパ。" },
        { sig: "EnableIfT<C, T>", desc: "<t>SFINAE</t> 用。条件 <code>C</code> が真の時だけ型 <code>T</code> を露出させ、テンプレートの有効/無効を切り替える。" },
        { sig: "ConditionalT<C, A, B>", desc: "コンパイル時の三項演算子。<code>C</code> が真なら <code>A</code>、偽なら <code>B</code> 型を選ぶ。" }
      ]
    },
    {
      name: "WriteLittleEndian / ReadLittleEndian",
      kind: "関数テンプレート", header: "foundation/EndianSerialization.h",
      summary: "1/2/4/8 byte の整数・浮動小数・列挙値を、host CPU に依存しない little endian byte 列へ書き出し・読み戻す。無効表現を持つ <code>bool</code> は compile-time に拒否する。",
      when: "AssetPack や保存データなど、CPU endian に左右されない固定 wire 形式を実装する時。",
      sample: "u8 bytes[8]{};\nWriteLittleEndian(bytes, u32{0x12345678});\nu32 value = ReadLittleEndian&lt;u32&gt;(bytes);",
      members: [
        { sig: "template&lt;typename T&gt; void WriteLittleEndian(u8* destination, T value)", desc: "<code>sizeof(T)</code> byte を little endian 順で書く。" },
        { sig: "template&lt;typename T&gt; T ReadLittleEndian(const u8* source)", ret: "復元した値", desc: "little endian byte 列から同じ固定幅型を復元する。" }
      ]
    },
    {
      name: "TContiguousEnumLookup&lt;Enum, Count&gt;",
      kind: "クラステンプレート", header: "foundation/EnumLookup.h",
      summary: "0 始まりで欠番のない列挙値の妥当性判定と名前取得を、constexpr 名前表から生成する。",
      when: "wire 値の検証や診断名で同じ switch/比較列を重複させず、compile-time に範囲を固定したい時。",
      sample: "constexpr const char* names[]{\"A\", \"B\"};\nconstexpr TContiguousEnumLookup&lt;EMode, 2&gt; lookup(names);\nif (lookup.Contains(mode)) Log(lookup.Name(mode));",
      members: [
        { sig: "constexpr bool Contains(Enum value) const", ret: "有効範囲なら true", desc: "値が 0 以上 Count 未満かを検証する。" },
        { sig: "constexpr const char* Name(Enum value, const char* fallback = \"Unknown\") const", ret: "名前または fallback", desc: "有効値を添字にして名前を返す。" },
        { sig: "static constexpr usize Size()", ret: "有効値数", desc: "compile-time の Count を返す。" }
      ]
    },
    {
      name: "TGenerationHandleLayoutTraits&lt;T&gt;",
      kind: "型特性テンプレート", header: "foundation/GenerationHandleLayoutTraits.h",
      summary: "世代付きハンドルの<b>物理配置だけ</b>を公開する ABI 監査用 trait。identity・generation の位置と幅、domain prefix、型全体の size/alignment を compile time で取得できる。",
      when: "異種ハンドルの serialize・ABI 検査・静的契約テストで、field 配置の意図しない変更を検出したい時。無効値、生存判定、generation 更新規則の共通化には使わない。",
      sample: "static_assert(TGenerationHandleLayoutTraits&lt;FEntityId&gt;::kAvailable);\nstatic_assert(TGenerationHandleLayoutTraits&lt;FEntityId&gt;::kGenerationOffset == offsetof(FEntityId, generation));",
      members: [
        { sig: "static constexpr bool kAvailable", desc: "対象型の特殊化が物理配置情報を提供する場合は true。未登録型の primary template は false。" },
        { sig: "static constexpr usize kIdentityOffset / kGenerationOffset", desc: "identity field と generation field の byte offset。" },
        { sig: "static constexpr usize kIdentityBytes / kGenerationBytes", desc: "identity field と generation field の byte 幅。" },
        { sig: "static constexpr usize kDomainPrefixBytes", desc: "identity より前に channel 等の domain field がある場合の byte 幅。無い型は 0。" },
        { sig: "static constexpr usize kStorageBytes / kStorageAlignment", desc: "ハンドル型全体の size と alignment。" }
      ]
    },
    {
      name: "コンパイラ / プラットフォーム検出マクロ",
      kind: "マクロ", header: "foundation/Compiler.h",
      summary: "どの<b>コンパイラ・OS・アーキテクチャ・ビルド種別</b>かを判定するマクロと、方言差を吸収した<b>関数属性ラッパ</b>（強制インライン化・到達ヒント等）をまとめたもの。依存なしでどこからでも include できる。",
      when: "コンパイラごとに分岐したい、特定関数を強制インライン化したい、ホットパスに分岐予測ヒントを付けたい時。",
      sample: "#if ACS_COMPILER_MSVC\n    // MSVC 固有\n#endif\nACS_FORCEINLINE f32 Sq(f32 x) { return x * x; }\nif (ACS_UNLIKELY(err)) { /* まれな経路 */ }",
      members: [
        { sig: "ACS_COMPILER_MSVC / _CLANG / _GCC", desc: "使用中コンパイラに対応するものが 1。" },
        { sig: "ACS_PLATFORM_WINDOWS / ACS_ARCH_X64 / ACS_ARCH_ARM64", desc: "OS / アーキテクチャ判定（現状 Windows・x64/ARM64）。" },
        { sig: "ACS_BUILD_DEBUG", desc: "デバッグビルドなら 1。" },
        { sig: "ACS_FORCEINLINE / ACS_NEVERINLINE", desc: "強制インライン化 / インライン抑制。" },
        { sig: "ACS_NORETURN / ACS_DEBUGBREAK() / ACS_THREAD_LOCAL", desc: "戻らない関数 / デバッガブレーク / スレッドローカル変数。" },
        { sig: "ACS_LIKELY(x) / ACS_UNLIKELY(x)", desc: "分岐予測ヒント（成立しやすい / しにくい）。" },
        { sig: "ACS_CACHELINE_SIZE / ACS_CACHELINE_ALIGN", desc: "キャッシュライン（64B）サイズと整列指定。" },
        { sig: "ACS_CONCAT(a,b) / ACS_STRINGIFY(x) / ACS_UNUSED(x)", desc: "トークン連結 / 文字列化 / 未使用警告抑制。" },
        { sig: "ACS_RESTRICT", desc: "<code>restrict</code> 修飾子のラッパ（MSVC は <code>__restrict</code>、それ以外は <code>__restrict__</code>）。ポインタが指す領域が他とエイリアスしないと最適化器に伝える。" },
        { sig: "ACS_DLLEXPORT / ACS_DLLIMPORT", desc: "DLL シンボルのエクスポート / インポート指定（MSVC は <code>__declspec(dllexport/dllimport)</code>、GCC/Clang は <code>visibility(\"default\")</code> 相当）。" },
        { sig: "ACS_TARGET_AVX / ACS_TARGET_AVX2", desc: "その関数だけ AVX / AVX2(+FMA) を有効化する関数属性。Clang/GCC は <code>target(...)</code> を付与、MSVC は空。ランタイム CPU 判定で AVX 経路を選ぶ関数に付ける。" },
        { sig: "ACS_PRETTY_FUNC", desc: "関数シグネチャ文字列（MSVC は <code>__FUNCSIG__</code>、それ以外は <code>__PRETTY_FUNCTION__</code>）。" },
        { sig: "ACS_COMPILER_NAME / ACS_PLATFORM_NAME / ACS_ARCH_NAME", desc: "コンパイラ / OS / アーキテクチャ名の文字列リテラル（例 <code>\"msvc\"</code> / <code>\"windows\"</code> / <code>\"x64\"</code>）。" }
      ]
    },
    {
      name: "Platform.h (OS ヘッダ集約)",
      kind: "ヘッダ", header: "foundation/Platform.h",
      summary: "<code>&lt;windows.h&gt;</code> など<b>重い OS ヘッダを 1 箇所に閉じ込める</b>窓口。不要なサブシステムを除外し、<code>min/max</code> や <code>CreateFile</code> など ACS の名前と衝突する Win32 マクロを <code>#undef</code> して安全にする。",
      when: "OS API を直接呼ぶ実装ファイル（.cpp）で <code>&lt;windows.h&gt;</code> の代わりにこれを include する。これにより他のヘッダを軽量に保てる。",
      sample: "// .cpp の中で:\n#include \"foundation/Platform.h\"   // windows.h を安全に取り込む\nHANDLE h = ::CreateFileW(...);    // OS API がそのまま使える"
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "TResult": "成功値かエラー値の<b>どちらか一方</b>を持つ型。<t>例外</t>を使わずに失敗を戻り値で伝える（既定のエラー型は <t>FErrorCode</t>）。",
  "FErrorCode": "ACS の標準エラー型。カテゴリ・サブコード・OS エラー値・メッセージ・<t>発生位置</t>をまとめて持つ <t>POD</t>。",
  "FPanic": "回復不能な状況でログとスタックトレースを出してプロセスを終了させる最終経路。通常は<t>アサート</t>から呼ばれる。",
  "FLogger": "非同期でスレッド安全なログ出力クラス。<code>ACS_LOG_*</code> マクロ経由で使う。",
  "例外": "C++ の <code>throw</code>/<code>catch</code> による失敗伝搬。ACS は性能と単純さのため使わず <t>TResult</t> を選ぶ。",
  "アサート": "条件が偽ならその場でプログラムを止めてバグを知らせる仕組み（ACS_ASSERT 等）。",
  "POD": "Plain Old Data。コンストラクタや仮想関数を持たない単純な構造体で、メモリコピーで安全に扱える型。",
  "完全転送": "テンプレート関数が受け取った引数を、lvalue/rvalue の区別を保ったまま別の関数に渡すこと（<code>Forward</code>）。",
  "SFINAE": "テンプレートの置換に失敗した候補を<b>エラーにせず単に除外</b>する C++ の仕組み。条件付きでオーバーロードを有効/無効化するのに使う。",
  "スタックトレース": "ある時点でどの関数からどの関数が呼ばれてきたか、という呼び出し履歴。クラッシュ原因の追跡に使う。",
  "発生位置": "問題が起きたソース上の場所（ファイル名・行・関数名）。<t>FSourceLoc</t> が表す。"
});
