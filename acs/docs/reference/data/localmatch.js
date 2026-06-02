/* ACS リファレンス — localmatch モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > は &lt; &gt;。 */
ACS_REF.modules.push({
  id: "localmatch",
  order: 56,
  title: "localmatch — ローカルマッチメイク",
  blurb: "サーバを使わず、<b>同じプロセスの中だけ</b>でプレイヤー同士を引き合わせる<t>マッチメイク</t>の実装です。検索を出した者同士を<t>レーティング</t>(腕前の数値)が近い順に組ませます。テストやオフライン開発、ローカル対戦の動作確認に使います。",
  types: [
    {
      name: "FLocalMatchmakerConfig",
      kind: "構造体", header: "localmatch/LocalMatchmaker.h",
      summary: "<t>FLocalMatchmaker</t> の設定。今は『どれだけ<t>レーティング</t>が離れていてもマッチさせてよいか』の上限値だけを持ちます。",
      when: "腕前の差が大きくても組ませたい(値を大きく)、近い相手だけ組ませたい(値を小さく)を調整したい時。",
      sample: "FLocalMatchmakerConfig cfg;\ncfg.MaxRatingDelta = 50;          // ±50 まで離れた相手のみ許可\nFLocalMatchmaker mm(cfg);",
      members: [
        { sig: "acs::u32 MaxRatingDelta = 150", ret: "許容レート差", desc: "2 人の<t>レーティング</t>差がこの値以内なら互換と見なしてマッチさせる。既定は 150。" }
      ]
    },
    {
      name: "FLocalMatchmaker",
      kind: "クラス", header: "localmatch/LocalMatchmaker.h",
      summary: "<t>マッチメイク</t>の本体。<code>acs::game::IMatchmaker</code> を実装し、同じプロセス内で出された検索同士をレートが近い順に組ませる<b>決定的</b>な実装です。最大 32 件の検索を同時に扱えます。コピー/<t>ムーブ</t>は禁止。",
      when: "オンラインサーバ無しでマッチング処理を動かしたい時(自動テスト・オフライン開発・ローカル対戦のロジック確認)。通常は直接生成せず <t>InstallLocalMatchmakerAsDefault</t> 経由で使う。",
      sample: "FLocalMatchmaker mm;\nauto a = mm.StartSearch(\"ranked_1v1\", 1500);  // プレイヤーA が検索\nauto b = mm.StartSearch(\"ranked_1v1\", 1480);  // プレイヤーB が検索 → 互いに発見\nif (mm.PollStatus(a.Value()) == acs::game::EMatchStatus::Matched) {\n    mm.AcceptMatch(a.Value());                 // 双方が Accept で確定\n}",
      members: [
        { sig: "explicit FLocalMatchmaker(const FLocalMatchmakerConfig& Config)", desc: "設定を渡して構築する。引数なし版は既定設定(レート差 150)。" },
        { sig: "TResult<MatchTicket> StartSearch(const char* Mode, acs::u32 EloHint)", ret: "発行された ticket", desc: "<code>Mode</code>(\"ranked_1v1\" 等のキー)と自分の<t>レーティング</t>推定値で検索を開始し、<t>MatchTicket</t> を発行する。同じ Mode でレートが近い待機者がいれば即座に双方を <code>Matched</code> にする。空きが無い時は <t>Result</t> がエラー。", when: "プレイヤーが『対戦相手を探す』ボタンを押した時。", sample: "auto t = mm.StartSearch(\"casual\", 1500);\nif (t.IsOk()) { /* t.Value() を保持して PollStatus で監視 */ }" },
        { sig: "TResult<void> CancelSearch(MatchTicket Ticket)", desc: "進行中の検索を取りやめる。状態は <code>Cancelled</code> になる。無効な ticket は安全に無視(<t>べき等</t>)。", when: "プレイヤーが検索をやめた / 画面を閉じた時。" },
        { sig: "acs::game::EMatchStatus PollStatus(MatchTicket Ticket)", ret: "現在の状態", desc: "ticket の現在状態(<code>Searching</code>/<code>Matched</code>/<code>Cancelled</code>/<code>Failed</code>)を返す。副作用なしなので毎フレーム呼んでよい。無効 ticket は <code>Failed</code>。", when: "検索中、相手が見つかったかを定期的に確認する時。" },
        { sig: "TResult<void> AcceptMatch(MatchTicket Ticket)", desc: "<code>Matched</code> 状態の ticket を確定する。双方が Accept した時点でマッチ成立。<code>Matched</code> 以外で呼ぶとエラー。", when: "相手が見つかり、プレイヤーが『参加』を押した時。" },
        { sig: "void Clear()", desc: "保持している全検索を破棄して初期状態に戻す。", when: "テストの後始末や、セッションを丸ごとリセットしたい時。" },
        { sig: "acs::u32 ActiveTicketCount() const", ret: "有効な検索数", desc: "今アクティブな(終端に達していない)検索の件数を返す。主にデバッグ・テスト用。" }
      ]
    },
    {
      name: "GetDefaultLocalMatchmaker()",
      kind: "関数", header: "localmatch/LocalMatchmaker.h",
      summary: "プロセス全体で共有される既定の <t>FLocalMatchmaker</t>(<t>シングルトン</t>)を <code>acs::game::IMatchmaker&amp;</code> として返します。",
      when: "ローカル matchmaker の実体に直接触れたい時。普通はこれを直接呼ばず <t>InstallLocalMatchmakerAsDefault</t> で既定登録し、上位は <code>GetDefaultMatchmaker()</code> を使う。",
      sample: "acs::game::IMatchmaker& mm = acs::localmatch::GetDefaultLocalMatchmaker();\nauto t = mm.StartSearch(\"ranked_1v1\", 1500);"
    },
    {
      name: "InstallLocalMatchmakerAsDefault()",
      kind: "関数", header: "localmatch/LocalMatchmaker.h",
      summary: "gameframework の matchmaker <t>provider</t> にローカル実装を登録します。これ以降 <code>acs::game::GetDefaultMatchmaker()</code> がローカル matchmaker を返すようになります。",
      when: "アプリ起動時に一度だけ呼ぶ。これで上位コードは backend を意識せず <code>GetDefaultMatchmaker()</code> だけで実 matchmaker を得られる(未登録時は stub が返る)。",
      sample: "// 起動時に一度だけ\nacs::localmatch::InstallLocalMatchmakerAsDefault();\n// 以降どこでも:\nacs::game::IMatchmaker& mm = acs::game::GetDefaultMatchmaker();"
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "マッチメイク": "対戦/協力するプレイヤー同士を、腕前(<t>レーティング</t>)やモードを見て組み合わせる仕組み。",
  "レーティング": "プレイヤーの腕前を表す数値(Elo / Glicko-2 等)。値が近い者同士を組ませると公平な対戦になりやすい。",
  "MatchTicket": "検索 1 件分を表す不透明な ID(u64)。0 は無効。StartSearch で発行され、PollStatus / AcceptMatch / CancelSearch に渡す。",
  "InstallLocalMatchmakerAsDefault": "ローカル matchmaker を既定 <t>provider</t> として登録する関数。以降 <code>GetDefaultMatchmaker()</code> がこの実装を返す。",
  "FLocalMatchmaker": "サーバ無しで同一プロセス内のプレイヤーを組ませる <code>IMatchmaker</code> 実装。",
  "provider": "『既定の実装を返す関数』のこと。backend 側が登録し、上位は実装を知らずに既定を取得できる仕組み。",
  "べき等": "同じ操作を何度繰り返しても結果が変わらない性質。無効な ticket のキャンセルが安全に無視されるなど。",
  "シングルトン": "プロセス内にただ 1 つだけ存在する共有インスタンス。"
});
