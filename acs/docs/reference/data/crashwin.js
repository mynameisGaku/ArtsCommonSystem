/* ACS リファレンス — crashwin モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "crashwin", order: 57,
  title: "crashwin — クラッシュレポート",
  blurb: "ゲームが落ちた時に <b>Windows のミニダンプ（.dmp）とテキスト報告書を自動で吐き出す</b>実 backend です。<t>gameframework</t> の抽象 <t>ICrashReporterBackend</t> を Windows の <t>DbgHelp</t> で実装し、起動時に 1 行登録するだけで使えます。",
  types: [
    {
      name: "FWindowsCrashReporterConfig",
      kind: "構造体", header: "crashwin/WindowsCrashReporter.h",
      summary: "<t>CWindowsCrashReporter</t> の初期設定。今は<b>ダンプ／報告書の出力先フォルダ</b>を 1 つ指定するだけのシンプルな設定です。",
      when: "クラッシュ報告ファイルの保存先を既定の <code>\"crash_dumps\"</code> 以外にしたい時。",
      sample: "FWindowsCrashReporterConfig cfg;\ncfg.DumpDirectory = \"Saved/Crashes\";   // 出力先を変更\nFWindowsCrashReporter reporter(cfg);",
      members: [
        { sig: "const char* DumpDirectory = \"crash_dumps\"", ret: "出力フォルダ名", desc: "ダンプ（.dmp）とテキスト報告書（.txt）を書き出すフォルダ。<b>呼出側が寿命を保証</b>する文字列（<code>static</code> / リテラル推奨）。" }
      ]
    },
    {
      name: "CWindowsCrashReporter",
      kind: "クラス", header: "crashwin/WindowsCrashReporter.h",
      summary: "Windows 専用のクラッシュ報告 backend。<t>gameframework</t> の <t>ICrashReporterBackend</t> を実装し、<t>DbgHelp</t> の <code>MiniDumpWriteDump</code> で<b>ミニダンプ</b>を、合わせて<b>人間が読めるテキスト報告書</b>を書き出します。直前の <t>パンくず</t>（breadcrumb）も最大 32 件まで添付します。コピー／<t>ムーブ</t>は禁止。",
      when: "Windows タイトルで、クラッシュや非致命エラーをローカルファイルとして残したい時。多くの場合は直接 new せず <t>InstallWindowsCrashReporterAsDefault</t> 経由で <t>singleton</t> として使います。",
      sample: "CWindowsCrashReporter reporter;\nif (reporter.Init(\"com.example.mygame\", \"1.2.3\").IsOk()) {\n    reporter.AddBreadcrumb(\"scene\", \"TitleScreen ロード完了\");\n    // ... 何か落ちそうな処理 ...\n    reporter.ReportError(\"save\", \"セーブファイルが壊れています\");\n}\nreporter.Shutdown();",
      members: [
        { sig: "explicit CWindowsCrashReporter(const FWindowsCrashReporterConfig& Config)", desc: "出力フォルダ等を指定して構築する。引数なしの既定構築も可（出力先は <code>\"crash_dumps\"</code>）。" },
        { sig: "TResult<void> Init(const char* ProductId, const char* Version)", ret: "成否（<t>Result</t>）", desc: "報告に付ける<b>製品 ID とバージョン</b>を控えて初期化する。両文字列の寿命は呼出側が保証。", when: "アプリ起動直後に一度だけ。", sample: "auto r = reporter.Init(\"com.example.mygame\", \"1.2.3\");\nif (r.IsErr()) { /* 失敗は握り潰す（二次クラッシュ防止） */ }" },
        { sig: "void Shutdown()", desc: "後始末。<code>Init()</code> 前に呼んでも安全（<t>no-op</t>）。" },
        { sig: "bool IsAvailable() const", ret: "利用可能か", desc: "<code>Init()</code> 成功後かつ <code>Shutdown()</code> 前なら true。" },
        { sig: "TResult<void> ReportCrash(const FCrashContext& Context)", ret: "成否（<t>Result</t>）", desc: "クラッシュ 1 件を処理する。<b>ミニダンプ（.dmp）</b>と、例外種別・メッセージ・スタック・<t>パンくず</t>を含む<b>テキスト報告書</b>を出力フォルダに書く。", when: "自前のクラッシュハンドラからプロセスがまだ生きている間に呼ぶ。", sample: "acs::game::FCrashContext ctx;\nctx.exception_type = \"SEH_ACCESS_VIOLATION\";\nctx.message        = \"null deref in Foo::Bar\";\nreporter.ReportCrash(ctx);" },
        { sig: "TResult<void> ReportError(const char* Category, const char* Message)", ret: "成否（<t>Result</t>）", desc: "<b>非致命的エラー</b>を 1 件、テキスト報告書として書き出す。<code>Category</code> は集計用のキー（\"net\" / \"save\" 等）。", sample: "reporter.ReportError(\"shader\", \"PSO のコンパイルに失敗\");" },
        { sig: "TResult<void> AddBreadcrumb(const char* Category, const char* Message)", ret: "成否（<t>Result</t>）", desc: "クラッシュ前の文脈を<b>リングバッファ</b>に 1 件積む（最大 32 件、古いものから上書き）。送信はせず、次の <code>ReportCrash</code> でまとめて添付される。", when: "「どこまで進んでいたか」を残したい節目ごとに呼ぶ。", sample: "reporter.AddBreadcrumb(\"input\", \"ボス部屋に入室\");" },
        { sig: "void SetUserId(const char* AnonymousId)", desc: "報告に付ける<b>匿名ユーザー ID</b>を設定する。GDPR/CCPA 対応のため、生の email でなく初回起動時に作った UUID 等を渡す。", sample: "reporter.SetUserId(\"a1b2c3d4-...\");" },
        { sig: "void Tick(f32 Dt)", desc: "毎フレーム呼ぶ pump 用フック（<code>Dt</code> は経過秒）。本 backend は同期書き込みなので実質 <t>no-op</t>。" },
        { sig: "const char* LastDumpPath() const", ret: "最後の .dmp パス", desc: "直近に書き出したミニダンプの<b>ファイルパス</b>。未出力なら空文字列。", when: "出力されたダンプの場所をログに出す／UI で案内する時。" },
        { sig: "const char* LastReportPath() const", ret: "最後の .txt パス", desc: "直近に書き出したテキスト報告書の<b>ファイルパス</b>。未出力なら空文字列。" },
        { sig: "using FWindowsCrashReporter = CWindowsCrashReporter", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CWindowsCrashReporter</code> を使う。" }
      ]
    },
    {
      name: "GetDefaultWindowsCrashReporter()",
      kind: "関数", header: "crashwin/WindowsCrashReporter.h",
      summary: "プロセス全体で共有される<b>既定の <t>CWindowsCrashReporter</t> <t>singleton</t></b>を返す。これが <t>provider</t> として登録される実体。",
      when: "通常は直接呼ばず <t>InstallWindowsCrashReporterAsDefault</t> を使う。明示的に同じ singleton を触りたい時だけ使う。",
      sample: "auto& crash = acs::crashwin::GetDefaultWindowsCrashReporter();\ncrash.AddBreadcrumb(\"boot\", \"engine init 完了\");",
      members: [
        { sig: "ICrashReporterBackend& GetDefaultWindowsCrashReporter()", ret: "既定 backend 参照", desc: "プロセス共有の <code>CWindowsCrashReporter</code> を <t>ICrashReporterBackend</t> 参照として返す。" }
      ]
    },
    {
      name: "InstallWindowsCrashReporterAsDefault()",
      kind: "関数", header: "crashwin/WindowsCrashReporter.h",
      summary: "<t>gameframework</t> の <t>CrashReporter</t> <t>provider</t> に本 Windows backend を登録する結線ヘルパ。これを<b>起動時に一度だけ</b>呼ぶと、以降 <code>acs::game::GetDefaultCrashReporter()</code> が stub でなく<b>本物の Windows backend</b> を返すようになる。",
      when: "アプリ起動時。<code>#if WITH_ACS_CRASH_REPORTER</code> で囲って一度呼べば、上位コードは backend 非依存のまま実 backend を使える。",
      sample: "#if WITH_ACS_CRASH_REPORTER\n    acs::crashwin::InstallWindowsCrashReporterAsDefault();\n#endif\n// 以降はどこでも:\nauto& crash = acs::game::GetDefaultCrashReporter();  // 実 backend が返る",
      members: [
        { sig: "void InstallWindowsCrashReporterAsDefault()", desc: "<code>acs::game::SetCrashReporterProvider()</code> に <t>GetDefaultWindowsCrashReporter</t> を登録する。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "DbgHelp": "Windows 標準のデバッグ支援ライブラリ。<code>MiniDumpWriteDump</code> で<t>ミニダンプ</t>を生成できる。",
  "ミニダンプ": "クラッシュ時のメモリ状態を抜き出した <code>.dmp</code> ファイル。後でデバッガに読み込んでスタックを解析できる。",
  "パンくず": "クラッシュ前の出来事を時系列で残す短いログ（breadcrumb）。「どこまで進んでいたか」の手掛かりになる。",
  "ICrashReporterBackend": "<t>gameframework</t> 側のクラッシュ報告 backend 抽象<t>インターフェース</t>。crashwin はその Windows 実装。",
  "FCrashContext": "クラッシュ 1 件分の文脈（例外種別・メッセージ・スタック・シーン名等）をまとめた構造体（<t>gameframework</t> 側）。",
  "CrashReporter": "<t>gameframework</t> のクラッシュ報告 seam 一式（stub・provider・ハンドラ）。crashwin が実 backend を供給する。",
  "provider": "「既定 backend を返す関数」を後から差し替える仕組み。crashwin が自身を登録すると stub から実 backend に切り替わる。",
  "singleton": "プロセス内にただ 1 つだけ存在する共有インスタンス。",
  "no-op": "何もしない安全な動作。呼んでも副作用がないこと。"
});
