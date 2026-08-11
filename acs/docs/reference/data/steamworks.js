/* ACS リファレンス — steamworks モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "steamworks",
  order: 53,
  title: "steamworks — Steam 連携",
  blurb: "<t>Steam</t> の実績・リーダーボード・クラウドセーブ・フレンドなどを使う<t>バックエンド</t>です。ゲーム側は <code>acs::game</code> の <t>シーム</t>(抽象 I/F)越しに呼ぶだけで、このモジュールが本物の <t>Steamworks SDK</t> につなぎます。",
  types: [
    {
      name: "CSteamworksBridgeImpl",
      kind: "クラス", header: "steamworks/SteamworksBridgeImpl.h",
      summary: "<t>Steamworks SDK</t> を実際に呼ぶ<b>本物の実装</b>(<code>acs::game::ISteamworksBridge</code> を <t>Override</t>)。<code>SteamAPI_Init()</code> を起点に、実績解除・リーダーボード・統計(Stats)・DLC 所有・リッチプレゼンス・フレンド・クラウドセーブ・Workshop・ボイス・Steam Input を提供します。",
      when: "出荷ビルドで本物の Steam 機能を使いたい時。開発中で Steam クライアントを立ち上げられない時は、代わりに <code>acs::game::SteamworksBridgeStub</code> を使う(自動切り替えは <code>GetDefaultSteamworksBridge()</code> 参照)。",
      members: [
        { sig: "TResult&lt;void&gt; Init()", ret: "成功 or エラー", desc: "<code>SteamAPI_Init()</code> を呼んで Steam クライアント連携を確立する。Steam 未起動 / AppID 不一致なら <t>Err</t>(<code>kSubSteamworksInitFailed</code>)。", when: "起動時に一度。失敗したら stub へフォールバックする。" },
        { sig: "void Shutdown()", desc: "SDK 終了処理。<code>Init()</code> 前に呼んでも安全(<t>no-op</t>)。" },
        { sig: "bool IsInitialized() const", ret: "初期化済みか", desc: "<code>Init()</code> 成功後かつ <code>Shutdown()</code> 前なら true。失敗時の判定に使う。" },
        { sig: "PlayerIdentity GetLocalPlayer() const", ret: "ローカルプレイヤー", desc: "自分の表示名(<code>GetPersonaName</code>)と SteamID64 を <code>acs::game::PlayerIdentity</code> で返す。未初期化なら空。文字列の寿命は「次の <code>Tick()</code> まで」。" },
        { sig: "TResult&lt;void&gt; UnlockAchievement(const char* ach_id)", ret: "成功 or エラー", desc: "実績を解除する(<code>SetAchievement</code> + <code>StoreStats</code> を即時呼び出し)。"},
        { sig: "TResult&lt;void&gt; SetLeaderboardScore(const char* board_id, i64 score)", ret: "成功 or エラー", desc: "リーダーボードへスコアを送信する。内部は <t>非同期</t>(Find → Upload の 2 段)で、<code>Tick()</code> 経由でコールバックを進めて完了を待つため数フレーム遅延する。", when: "スコア更新時に。board_id は Steam 側で定義したボード名。" },
        { sig: "TResult&lt;i64&gt; GetLeaderboardScore(const char* board_id)", ret: "自分のスコア", desc: "自分の現在スコアを取得する。こちらも <t>非同期</t>(Find → Download)。" },
        { sig: "void Tick(f32 dt)", desc: "<code>SteamAPI_RunCallbacks()</code> を呼ぶ<b>必須</b>処理。毎フレーム呼ぶこと(非同期 API の完了もここで進む)。", when: "ゲームループから毎フレーム。" },
        { sig: "TResult&lt;void&gt; SetStat(const char* stat_name, i64 value) / TResult&lt;i64&gt; GetStat(...)", desc: "整数の<t>統計値(Stat)</t>を設定/取得する。Steam 側で事前定義した key 名(\"kills\" 等)を渡す。" },
        { sig: "TResult&lt;void&gt; SetFloatStat(const char* stat_name, f32 value) / TResult&lt;f32&gt; GetFloatStat(...)", desc: "小数の<t>統計値(Stat)</t>を設定/取得する。" },
        { sig: "bool IsDlcOwned(u32 app_id) const", ret: "DLC 所有か", desc: "指定 DLC の AppID をユーザーが所有していれば true。" },
        { sig: "TResult&lt;void&gt; SetRichPresence(const char* key, const char* value)", desc: "<t>リッチプレゼンス</t>を設定する。<code>key=\"status\"</code> の値はフレンド一覧の「今プレイ中」行に出る。"},
        { sig: "u32 GetFriendCount() const", ret: "フレンド数", desc: "自分のフレンド数。未初期化/未取得時は 0。" },
        { sig: "PlayerIdentity GetFriendByIndex(u32 index) const", ret: "フレンド情報", desc: "<code>0 &lt;= index &lt; GetFriendCount()</code> のフレンド情報。範囲外は空。" },
        { sig: "TResult&lt;void&gt; CloudWriteFile(const char* path, const void* data, u32 size)", ret: "成功 or エラー", desc: "<t>Steam Cloud</t>(Remote Storage)へファイルを書き込み永続化する。", when: "クラウドセーブを書き出す時。" },
        { sig: "TResult&lt;u32&gt; CloudReadFile(const char* path, void* out_buf, u32 buf_size)", ret: "読んだバイト数", desc: "Steam Cloud から読み出す。戻り値はファイルサイズ。0 は「ファイル無し」or エラー。" },
        { sig: "bool CloudFileExists(const char* path) const", ret: "存在するか", desc: "Cloud に該当ファイルがあれば true。" },
        { sig: "TResult&lt;void&gt; CloudDeleteFile(const char* path)", desc: "Cloud から削除する。存在しなくても成功扱い(<t>冪等</t>)。" },
        { sig: "void CloudGetQuota(u64&amp; out_available, u64&amp; out_total) const", desc: "Cloud の空き容量と総容量を返す。エラー時は両方 0。" },
        { sig: "u32 WorkshopGetSubscribedCount() const", ret: "購読アイテム数", desc: "ユーザーがサブスクライブ済みの <t>Workshop</t>(UGC)アイテム数。" },
        { sig: "void WorkshopGetSubscribedItem(u32 index, u64&amp; out_item_id, const char*&amp; out_install_path) const", desc: "index 番目の Workshop アイテム ID とインストールパスを返す。範囲外は id=0 / path=nullptr。" },
        { sig: "TResult&lt;void&gt; VoiceStartRecording() / VoiceStopRecording()", desc: "<t>ボイスチャット</t>の録音を開始/停止する(プッシュトゥトーク想定)。" },
        { sig: "TResult&lt;u32&gt; VoiceGetCompressed(void* out_buf, u32 buf_size)", ret: "取得バイト数", desc: "圧縮済み音声データを取り出す。0 は「データなし」。" },
        { sig: "TResult&lt;void&gt; InputInit()", desc: "<t>Steam Input</t> を初期化する(アクションセットを SDK へ通知)。" },
        { sig: "u32 InputGetControllerCount() const", ret: "接続数", desc: "Steam Input が認識しているコントローラ数。" },
        { sig: "using FSteamworksBridgeImpl = CSteamworksBridgeImpl", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSteamworksBridgeImpl</code> を使う。" }
      ]
    },
    {
      name: "GetDefaultSteamworksBridge()",
      kind: "関数", header: "steamworks/SteamworksBridgeImpl.h",
      summary: "プロセス共有の<b>実 Bridge シングルトン</b>を返す。初回アクセス時に <code>Init()</code> を 1 回だけ走らせる(Steam 未起動でも参照は返す — 成否は <code>IsInitialized()</code> で判定)。",
      when: "「とりあえず実 Bridge が欲しい」時。自前で <code>CSteamworksBridgeImpl</code> を持ちたくない場合の手軽な入口。"
    },
    {
      name: "InstallSteamworksAsDefault()",
      kind: "関数", header: "steamworks/SteamworksBridgeImpl.h",
      summary: "実 Bridge を <code>acs::game</code> 側の<t>既定 provider</t>として登録する。これを呼ぶと、以降ゲームコードは <t>バックエンド</t>非依存に <code>acs::game::GetDefaultSteamworksBridge()</code> で実 Bridge を取得できる(未登録なら自動で <t>Stub</t> が返る)。",
      when: "アプリ起動時に一度だけ呼ぶ。<code>gameframework</code> はこのモジュールに直接依存できない(<t>循環依存</t>回避)ため、登録はこの関数経由で行う。"
    },
    {
      name: "kSubSteamworks* (エラー subcode)",
      kind: "定数(u16)", header: "steamworks/SteamworksBridgeImpl.h",
      summary: "<code>TResult</code> が <t>Err</t> の時に <code>err.subcode</code> で原因を見分けるための定数群(カテゴリは <code>ErrCategory::Generic</code>)。実装固有の失敗理由を表します。",
      when: "<code>Init()</code> 等が失敗した時、原因別に分岐したい場合に <code>err.subcode == ...</code> で照合する。",
      members: [
        { sig: "kSubSteamworksInitFailed = 1003", desc: "<code>SteamAPI_Init()</code> が失敗した(Steam 未起動 / AppID 不一致 等)。" },
        { sig: "kSubSteamworksFindBoardFailed = 1004", desc: "<code>FindLeaderboard</code> に失敗した。" },
        { sig: "kSubSteamworksUploadFailed = 1005", desc: "<code>UploadLeaderboardScore</code> に失敗した。" },
        { sig: "kSubSteamworksDownloadFailed = 1006", desc: "<code>DownloadLeaderboardEntries</code> に失敗した。" },
        { sig: "kSubSteamworksTimeout = 1007", desc: "<t>非同期</t> API が完了せずタイムアウトした。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "Steam": "Valve のゲーム配信プラットフォーム。実績・フレンド・クラウドセーブ等の機能を <t>Steamworks SDK</t> 経由で使える。",
  "Steamworks SDK": "Steam の機能(実績/リーダーボード/クラウド/Workshop 等)をゲームから呼ぶための公式ライブラリ。",
  "バックエンド": "抽象的な機能呼び出しを、実際のライブラリや OS API につなぐ実装側のこと。差し替え可能。",
  "シーム": "プラットフォーム固有 SDK との結合点を抽象化した純粋仮想<t>インターフェース</t>。ゲーム側はこれ越しに呼び、実装をビルド時に差し替える。",
  "Override": "基底クラスの仮想関数を派生クラスで上書き実装すること。",
  "Stub": "本物の代わりに置く最小実装。依存ゼロでコンパイル・テストできる no-op 版。",
  "no-op": "何もしない処理。呼んでも安全で副作用が無いこと。",
  "リッチプレゼンス": "フレンド一覧に表示される「今この人は何をしているか」の状態文字列。",
  "統計値(Stat)": "Steam 側で永続化される数値。累計キル数やプレイ時間など。実績の達成条件にも使う。",
  "Steam Cloud": "Steam が提供するクラウドストレージ。セーブデータ等を端末間で同期できる。",
  "Workshop": "ユーザー作成コンテンツ(UGC, MOD やマップ等)を配布・購読する Steam の仕組み。",
  "ボイスチャット": "マイク音声を圧縮して送受信する仕組み(Steam VOIP)。",
  "Steam Input": "各種コントローラ(XInput / DualSense 等)の差を吸収する Steam の入力 API。",
  "冪等": "同じ操作を何度行っても結果が変わらない性質。削除済みを再削除しても成功扱い、など。",
  "既定 provider": "明示指定が無い時に使われる既定の実装を返す登録関数。後勝ちで差し替えられる。"
});
