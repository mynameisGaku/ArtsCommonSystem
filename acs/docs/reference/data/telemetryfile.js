/* ACS リファレンス — telemetryfile モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > は &lt; &gt;。 */
ACS_REF.modules.push({
  id: "telemetryfile",
  order: 58,
  title: "telemetryfile — テレメトリ記録",
  blurb: "ゲーム内で起きた出来事(<t>テレメトリ</t>イベント)をサーバではなく<b>ローカルのファイル</b>に書き出す、お手軽なログ記録の実装です。<code>gameframework</code> の <t>IBackendClient</t> 窓口にそのまま差し込めるので、サーバが無くてもイベント計測のコードを動かせます。",
  types: [
    {
      name: "CFileTelemetryBackendClient",
      kind: "クラス", header: "telemetryfile/FileTelemetryBackendClient.h",
      summary: "<t>テレメトリ</t>イベントを<b>1 行 1 件のテキストファイル</b>に追記していく <t>IBackendClient</t> の実装。サーバ送信の代わりに <code>file:</code> パス先のファイルへ書き込む。<t>Connect</t> でファイルを開き、<code>SendTelemetry</code> でイベントを書き、<code>Disconnect</code> で閉じる。",
      when: "オンラインサーバをまだ用意していない開発初期や、オフラインのプレイログを手元に貯めたい時。計測コードを <t>IBackendClient</t> 越しに書いておけば、後で本物のサーバ実装に差し替えられる。",
      sample: "acs::telemetryfile::CFileTelemetryBackendClient tel;\ntel.Connect(\"file:Saved/telemetry.jsonl\");   // ファイルを開く\ntel.SendTelemetry(\"level_completed\", \"{\\\"level\\\":3}\"); // 1 行追記\ntel.Tick(0.016f);                              // 毎フレーム呼ぶ(flush 等)\ntel.Disconnect();                              // 閉じる",
      members: [
        { sig: "TResult&lt;void&gt; Connect(const char* ServerUrl)", ret: "成功/失敗", desc: "出力先ファイルを開く(無ければ作る)。<code>ServerUrl</code> は <code>\"file:パス\"</code> 形式で、<code>file:</code> 接頭辞は自動で取り除かれる。開けない時は失敗を返す。", when: "イベントを書き始める前に一度呼ぶ。", sample: "auto r = tel.Connect(\"file:Saved/events.log\");\nif (r.IsErr()) { /* 開けなかった */ }" },
        { sig: "void Disconnect()", desc: "開いているファイルを閉じる。未接続でも安全(<t>べき等</t>)。" },
        { sig: "bool IsConnected() const", ret: "接続中か", desc: "ファイルが開いていれば true。" },
        { sig: "TResult&lt;void&gt; SendTelemetry(const char* EventName, const char* JsonPayload)", ret: "成功/失敗", desc: "イベント 1 件を 1 行としてファイルに追記する。<code>JsonPayload</code> 内の特殊文字は安全に<t>エスケープ</t>される。書き込み失敗時は失敗を返す。", when: "計測したい出来事が起きたタイミングで呼ぶ。", sample: "tel.SendTelemetry(\"item_pickup\", \"{\\\"id\\\":\\\"potion\\\"}\");" },
        { sig: "void Tick(f32 Dt)", desc: "毎フレーム呼ぶ前提の更新処理。<code>Dt</code> は前フレームからの経過秒。", when: "ゲームループの中で毎フレーム呼ぶ。" },
        { sig: "const char* Path() const", ret: "出力先パス", desc: "現在書き込んでいるファイルのパスを返す。" },
        { sig: "u32 WrittenCount() const", ret: "書込件数", desc: "これまでにファイルへ書いたイベントの累計件数。動作確認やテストに便利。", sample: "if (tel.WrittenCount() == 0) { /* まだ何も書いていない */ }" },
        { sig: "using FFileTelemetryBackendClient = CFileTelemetryBackendClient", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CFileTelemetryBackendClient</code> を使う。" }
      ]
    },
    {
      name: "GetDefaultFileTelemetryBackendClient()",
      kind: "関数", header: "telemetryfile/FileTelemetryBackendClient.h",
      summary: "プロセス全体で共有される既定の <code>CFileTelemetryBackendClient</code> 1 個(<t>シングルトン</t>)への参照を返す。自分でインスタンスを作らず、これ 1 つを使い回す入口。",
      when: "ファイルテレメトリの実体を 1 個だけ用意し、あちこちから同じ記録先を使いたい時。多くは次の <code>InstallFileTelemetryAsDefault()</code> が内部で利用する。",
      sample: "auto& tel = acs::telemetryfile::GetDefaultFileTelemetryBackendClient();\ntel.Connect(\"file:Saved/telemetry.jsonl\");",
      members: [
        { sig: "acs::game::IBackendClient& GetDefaultFileTelemetryBackendClient()", ret: "共有の実体", desc: "共有 <code>CFileTelemetryBackendClient</code> を <t>IBackendClient</t> 参照で返す。" }
      ]
    },
    {
      name: "InstallFileTelemetryAsDefault()",
      kind: "関数", header: "telemetryfile/FileTelemetryBackendClient.h",
      summary: "<code>gameframework</code> の backend <t>provider</t> にこのファイル実装を登録する関数。一度呼ぶと、以降 <code>acs::game::GetDefaultBackendClient()</code> がファイルバック実装を返すようになる。",
      when: "アプリ起動時に一度だけ呼ぶ。これで上位コードは backend の種類を意識せず <code>GetDefaultBackendClient()</code> だけで実 backend を取得できる。",
      sample: "// 起動時に一度だけ\nacs::telemetryfile::InstallFileTelemetryAsDefault();\n// 以降はどこでも:\nauto& b = acs::game::GetDefaultBackendClient();\nb.SendTelemetry(\"game_start\", \"{}\");",
      members: [
        { sig: "void InstallFileTelemetryAsDefault()", desc: "既定 backend provider をファイル実装に切り替える(<t>後勝ち</t>)。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "テレメトリ": "ゲーム内の出来事(クリア・購入・離脱など)を記録して集計する計測の仕組み。改善のヒントを得るために使う。",
  "IBackendClient": "<code>gameframework</code> が定める『サーバ側との窓口』<t>インターフェース</t>。テレメトリ送信などを抽象化し、実装を差し替えられる。",
  "provider": "『既定の実装を返す関数』を後から登録する仕組み。登録するとアプリ全体の既定実装が差し替わる。",
  "べき等": "同じ操作を何度繰り返しても結果が変わらない性質。未接続で <code>Disconnect()</code> しても安全、など。",
  "後勝ち": "同じ登録を複数回行った時、最後に登録した内容が有効になること。"
});
