/* ACS リファレンス — gameframework モジュール (第 8 分冊: T〜W)。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "gameframework", order: 47, title: "gameframework — ゲーム部品 (acs::game)",
  blurb: "ゲームを作るときに「だいたい毎回いる」部品の詰め合わせです。当たり判定の<t>トリガ</t>、ターン制、武器・弾薬、敵の<t>ウェーブ</t>、天候や水、UI、ボイスチャットなどを、エンジン本体に手を入れずに組み合わせて使えます。",
  types: [
    {
      name: "FAssetPackReadRequest",
      kind: "構造体", header: "gameframework/AssetPackReadRequest.h",
      summary: "pak 内アセットの読み取り先、範囲、優先度を一つにまとめる要求値。",
      when: "gameplay 側から asset pack の batch 読み取りを組み立てる時。"
    },
    {
      name: "CHierarchyVisibilityBatch",
      kind: "クラス", header: "gameframework/HierarchyVisibilityBatch.h",
      summary: "親子階層の可視状態を親から子へ一括伝播し、変更対象だけを返す作業バッファ。",
      when: "多数 node の表示状態を再帰呼び出しと一件ごとの再確保なしで更新する時。",
      members: [
        { sig: "using FHierarchyVisibilityBatch = CHierarchyVisibilityBatch", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CHierarchyVisibilityBatch</code> を使う。" }
      ]
    },
    {
      name: "CHierarchyWorldTransformBatch",
      kind: "クラス", header: "gameframework/HierarchyWorldTransformBatch.h",
      summary: "親子順に world transform を一括更新し、変更された node 範囲を追跡する作業バッファ。",
      when: "scene 階層の transform 更新を cache-friendly な反復処理へまとめる時。",
      members: [
        { sig: "using FHierarchyWorldTransformBatch = CHierarchyWorldTransformBatch", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CHierarchyWorldTransformBatch</code> を使う。" }
      ]
    },
    {
      name: "FTransformSoAInput",
      kind: "構造体", header: "gameframework/TransformBatchSoA.h",
      summary: "位置、回転、拡大率を別配列で受け渡す transform batch の SoA 入力 view。",
      when: "大量 transform を連続配列から SIMD 向けに処理する時。"
    },
    {
      name: "ATriggerComponent",
      kind: "クラス", header: "gameframework/TriggerComponent.h",
      summary: "<t>ノード</t>を<t>トリガ</t>(重なり検出専用の当たり判定)に繋ぐ <t>コンポーネント</t>。プレイヤー / アイテム / 罠などが「触れた・離れた」を<b>そのノードごと</b>に受け取れます。<t>レイヤ</t>と<t>マスク</t>で反応相手を絞り込めます。",
      when: "アイテム取得・ダメージ床・ゴール判定など「重なったら何か起きる」をノード単位で書きたい時。物理の押し戻しは要らず、検知だけしたい場面。",
      members: [
        { sig: "explicit ATriggerComponent(CTriggerWorld2D& world, u32 layer = kAllLayers, u32 mask = kAllLayers)", desc: "所属する<t>トリガワールド</t>と、自分の<t>レイヤ</t>・反応したい相手の<t>マスク</t>を指定して作る。", when: "node->AddComponent の引数として渡す時。" },
        { sig: "void SetCircle(f32 radius)", desc: "当たり形状を半径 radius の円にする。" },
        { sig: "void SetAabb(FVec2 half_size)", desc: "当たり形状を半サイズ half_size の<t>AABB</t>(軸並行な四角)にする。" },
        { sig: "void SetLayer(u32 layer) / void SetMask(u32 mask)", desc: "自分のレイヤ / 反応する相手レイヤを後から変更する。次の更新で反映。" },
        { sig: "void SetOnEnter(TriggerFn fn, void* user) / void SetOnExit(TriggerFn fn, void* user)", desc: "サブクラス化せず関数ポインタで enter / exit を受ける。<code>void(*)(ATriggerComponent&amp; self, ATriggerComponent* other, void* user)</code> 型。", when: "クラスを継承せず、その場のラムダ / 関数で済ませたい時。" },
        { sig: "virtual void OnTriggerEnter(ATriggerComponent* other)", desc: "相手と重なり始めた時に呼ばれる仮想フック。サブクラスで<code>override</code>する。" },
        { sig: "virtual void OnTriggerExit(ATriggerComponent* other)", desc: "相手と離れた時に呼ばれる仮想フック。" },
        { sig: "u32 Layer() const / u32 Mask() const", desc: "現在のレイヤ / マスク値を返す。" }
      ]
    },
    {
      name: "CTriggerWorld2D",
      kind: "クラス", header: "gameframework/TriggerWorld2D.h",
      summary: "重なりだけを監視する<b>トリガ専用のワールド</b>。物理の押し戻しはせず、毎フレーム全<t>トリガ</t>の重なりを調べ、前フレームと比べて enter(触れた) / stay(触れ続け) / exit(離れた) の<t>イベント</t>を発火します。",
      when: "ダメージ判定・センサー・ゾーン入退場など、押し戻し不要で「重なったか」だけ知りたい仕組みの土台。",
      members: [
        { sig: "void Init()", desc: "初期化する。" },
        { sig: "FTriggerId AddCircle(const FCircle& c, u32 layer = 0)", ret: "トリガ ID", desc: "円トリガを登録し、識別用の<t>ハンドル</t>を返す。", when: "丸いセンサーを置きたい時。" },
        { sig: "FTriggerId AddAabb(const FAabb2& a, u32 layer = 0)", ret: "トリガ ID", desc: "四角(<t>AABB</t>)トリガを登録する。" },
        { sig: "void UpdateCircle(FTriggerId id, const FCircle& c) / void UpdateAabb(FTriggerId id, const FAabb2& a)", desc: "トリガが動いた時に位置・形状を更新する。種類が違う / 古いハンドルなら何もしない。" },
        { sig: "void Remove(FTriggerId id)", desc: "トリガを削除する。スロットは再利用され、古いハンドルは無効になる。" },
        { sig: "void SetUserData(FTriggerId id, void* user) / void* UserData(FTriggerId id) const", desc: "トリガに任意のポインタを紐付け / 取得する。ID から元オブジェクトを逆引きするのに使う。" },
        { sig: "void SetOnEnter(TriggerEventCallback cb, void* user)", desc: "重なり始めイベントの<t>コールバック</t>を登録する。"},
        { sig: "void SetOnStay(TriggerEventCallback cb, void* user) / void SetOnExit(TriggerEventCallback cb, void* user)", desc: "重なり継続中 / 離れた時のコールバックを登録する。" },
        { sig: "void Tick(f32 dt)", desc: "毎フレーム呼ぶ。全ペアの重なりを更新し、前フレと比べて enter / stay / exit を発火する。" },
        { sig: "u32 TriggerCount() const", ret: "登録数", desc: "有効なトリガの総数。" },
        { sig: "void ClearAll()", desc: "全トリガと重なり状態を破棄する。" },
        { sig: "using FTriggerWorld2D = CTriggerWorld2D", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CTriggerWorld2D</code> を使う。" }
      ]
    },
    {
      name: "FTriggerId",
      kind: "構造体", header: "gameframework/TriggerWorld2D.h",
      summary: "<t>トリガ</t>を識別する 32bit の<t>ハンドル</t>(下位 24bit が index、上位 8bit が<t>世代</t>)。削除→再登録でスロットが再利用されても、古いハンドルは無効と判定できます。",
      when: "CTriggerWorld2D の登録・更新・削除でトリガを指し示す時。",
      members: [
        { sig: "bool IsValid() const", desc: "0(無効)でなければ true。" },
        { sig: "u32 Index() const / u8 Generation() const", desc: "index 部 / 世代部を取り出す。" },
        { sig: "bool operator==(FTriggerId) const / bool operator!=(FTriggerId) const", desc: "ハンドル同士を比較する。" }
      ]
    },
    {
      name: "CTurnManager",
      kind: "クラス", header: "gameframework/TurnManager.h",
      summary: "<b>ターン制</b>の進行を管理する部品。複数の陣営(side)を行動順(<t>イニシアチブ</t>)で並べ、各陣営が<t>AP</t>(行動ポイント)予算で行動する、SRPG / ローグライク / 戦略系の定番マネージャです。",
      when: "プレイヤー→敵→環境の順に手番を回し、ラウンドごとに AP を回復する、ターン制ゲームを作る時。",
      members: [
        { sig: "void Init()", desc: "全 side / 行動順を捨て、phase=Setup・round=0 に戻す(コールバックは保持)。" },
        { sig: "FTurnSideId AddSide(const char* display_name, u32 max_ap, u32 initiative, bool is_player_controlled)", ret: "side ハンドル", desc: "陣営を登録する。名前・最大 AP・行動順・プレイヤー操作かを指定。", when: "戦闘開始時に参加陣営を並べる時。" },
        { sig: "void RemoveSide(FTurnSideId id)", desc: "陣営を除外する。手番中の side を消した場合は次へ進む。" },
        { sig: "void StartRound()", desc: "全 side の AP を満タンに回復 + 行動順を再構築 + 先頭 side を current にして、ターン開始<t>コールバック</t>を発火する。" },
        { sig: "void EndCurrentTurn()", desc: "現在の side を行動済みにして次へ。全 side 完了なら<t>ラウンド</t>終了コールバックを呼び、内部で次ラウンドを開始する。" },
        { sig: "bool TryConsumeAP(u32 amount)", ret: "消費できたか", desc: "現在の side から AP を消費する。残量不足なら false で据え置き。", when: "行動の度に必要 AP を引きたい時。" },
        { sig: "FTurnSideId CurrentTurnSide() const", ret: "現手番の side", desc: "今行動できる side。Setup / ラウンド終端中は無効 ID。" },
        { sig: "ETurnPhase CurrentPhase() const / u32 CurrentRound() const", desc: "現在の<t>フェーズ</t> / ラウンド番号を返す。" },
        { sig: "const FTurnSideState* GetSideState(FTurnSideId id) const", ret: "表示用 snapshot", desc: "UI / AI 向けの読み取り専用状態を返す。次の Add/Remove までしか有効でない点に注意。" },
        { sig: "u32 SideCount() const / u32 TurnsRemainingThisRound() const", desc: "登録 side 数 / 今ラウンドで未行動の side 数を返す。" },
        { sig: "void SetOnTurnStartCallback(TurnStartCallback cb, void* user)", desc: "ターン開始時のコールバックを登録する(nullptr で解除)。" },
        { sig: "void SetOnRoundEndCallback(RoundEndCallback cb, void* user)", desc: "ラウンド終了時のコールバックを登録する。報酬計算 / 状態異常処理などに。" },
        { sig: "using FTurnManager = CTurnManager", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CTurnManager</code> を使う。" }
      ]
    },
    {
      name: "FTurnSideId",
      kind: "構造体", header: "gameframework/TurnManager.h",
      summary: "ターンの陣営(side)を識別する 32bit の<t>ハンドル</t>(24bit index + 8bit<t>世代</t>)。除外→再追加でスロットが再利用されても、古い ID を無効と検出できます。",
      when: "CTurnManager の各 API で陣営を指す時。",
      members: [
        { sig: "bool IsValid() const", desc: "有効な ID なら true(packed != 0)。" },
        { sig: "u32 Index() const / u8 Gen() const", desc: "index 部 / 世代部を取り出す。" },
        { sig: "static FTurnSideId Pack(u32 index, u8 gen)", desc: "index と世代からハンドルを組み立てる。" }
      ]
    },
    {
      name: "FTurnSideState",
      kind: "構造体", header: "gameframework/TurnManager.h",
      summary: "陣営 1 つの<b>表示用 snapshot</b>。<code>GetSideState</code> が返す読み取り専用ビューで、UI / AI / セーブで現在の AP や行動済みかを見るのに使います。",
      when: "ターン UI に各陣営の AP や名前を出す時。",
      members: [
        { sig: "const char* display_name", desc: "AddSide で渡した名前(非所有、寿命は呼び出し側)。" },
        { sig: "u32 max_action_points / u32 current_action_points", desc: "最大 AP / 現在の残量。" },
        { sig: "u32 initiative", desc: "行動順(大きいほど先)。" },
        { sig: "bool is_player_controlled / bool has_acted", desc: "プレイヤー操作か / 今ラウンドで行動済みか。" }
      ]
    },
    {
      name: "ETurnPhase",
      kind: "列挙(enum)", header: "gameframework/TurnManager.h",
      summary: "ターン進行の<t>フェーズ</t>を表す列挙。値はセーブ / リプレイに書ける安定値です。",
      when: "「今はプレイヤーの番か敵の番か」で BGM / UI を分岐する時。",
      members: [
        { sig: "Setup = 0", desc: "Init 直後 / StartRound 前(side 登録中)。" },
        { sig: "PlayerTurn = 1", desc: "プレイヤー操作 side の番。" },
        { sig: "EnemyTurn = 2", desc: "通常の敵 side の番。" },
        { sig: "EnvironmentTurn = 3", desc: "環境(天候 / 罠 / 中立 NPC)の番。" },
        { sig: "EndOfRound = 4", desc: "全 side 行動完了直後(コールバック発火用、すぐ次ラウンドへ)。" }
      ]
    },
    {
      name: "CTutorialFlow",
      kind: "クラス", header: "gameframework/TutorialFlow.h",
      summary: "連続する<b>チュートリアルステップ</b>を順番に表示・進行させる小さな<t>状態機械</t>。描画はせず、現在ステップへのポインタを公開するだけなので、見た目は UI 側が自由に描けます。",
      when: "「WASD で移動」「SPACE でジャンプ」のように、ユーザーが操作を達成したら次へ進むガイドを作る時。",
      members: [
        { sig: "void AddStep(const FTutorialStep& step)", desc: "ステップを末尾に追加する。Start 前に並べる想定。" },
        { sig: "void Start()", desc: "ステップ 0 から表示開始する。ステップが無ければ即完了。" },
        { sig: "void AdvanceStep()", desc: "次のステップへ手動で進める。最終ステップで呼ぶと完了になる。", when: "OK ボタンや条件達成後に明示的に進めたい時。" },
        { sig: "void NotifyAction(const char* action_id)", desc: "ユーザー操作を通知する。現在ステップの id と一致し<code>require_user_action</code>なら自動で次へ進む。" },
        { sig: "void Reset()", desc: "ステップ定義は残したまま進行状態だけ初期化する。" },
        { sig: "void Skip()", desc: "チュートリアル全体を飛ばして完了扱いにする(冪等)。" },
        { sig: "bool IsActive() const / bool IsCompleted() const", desc: "進行中か / 完了済みかを返す。" },
        { sig: "const FTutorialStep* CurrentStep() const", ret: "現ステップ or null", desc: "表示用の現在ステップ。開始前 / 完了後は nullptr。", when: "UI 側で今出すべきメッセージを引く時。" },
        { sig: "u32 CurrentStepIndex() const / u32 StepCount() const", desc: "現在のステップ番号 / 登録ステップ数。" },
        { sig: "void Tick(f32 dt)", desc: "毎フレーム呼ぶ(将来の自動進行用フック)。" },
        { sig: "using FTutorialFlow = CTutorialFlow", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CTutorialFlow</code> を使う。" }
      ]
    },
    {
      name: "FTutorialStep",
      kind: "構造体", header: "gameframework/TutorialStep.h",
      summary: "チュートリアル 1 ステップ分の定義。文字列はすべて<b>非所有</b>(寿命は呼び出し側が保証)で、本クラスは保持するだけです。",
      when: "AddStep に渡す 1 件のガイド内容を組み立てる時。",
      members: [
        { sig: "const char* id", desc: "NotifyAction で参照される識別子。" },
        { sig: "const char* message", desc: "UI が表示するテキスト。" },
        { sig: "const char* highlight_target", desc: "ハイライト対象の名前(例 \"player\")。nullptr 可、本クラスは解釈しない。" },
        { sig: "bool require_user_action", desc: "true なら id 一致の NotifyAction か AdvanceStep まで自動進行しない。" }
      ]
    },
    {
      name: "CTweenManager",
      kind: "クラス", header: "gameframework/Tween.h",
      summary: "変数を<b>なめらかに変化</b>させる<t>トゥイーン</t>管理。値(<code>f32</code> / <code>FVec2</code> / <code>FVec3</code>)の<t>ポインタ</t>を渡すと、毎 Tick で from→to を<t>イージング</t>補間して書き込んでくれます。コールバック不要なのが特徴。",
      when: "色・位置・透明度などを一定時間でフワッと変えたい時(フェード、ポップアップ、カメラ寄せなど)。",
      members: [
        { sig: "FTweenHandle Tween(f32* target, f32 from, f32 to, f32 duration, Easing::EasingFn ease = Easing::Linear)", ret: "トゥイーン ハンドル", desc: "f32 変数を from→to へ duration 秒で補間する。duration<=0 は即時設定、target が null は no-op。", when: "1 つのスカラー値を時間補間したい時。" },
        { sig: "FTweenHandle Tween(FVec2* target, FVec2 from, FVec2 to, f32 duration, Easing::EasingFn ease = Easing::Linear)", desc: "2D ベクトル(位置など)を補間する。" },
        { sig: "FTweenHandle Tween(FVec3* target, FVec3 from, FVec3 to, f32 duration, Easing::EasingFn ease = Easing::Linear)", desc: "3D ベクトル(色など)を補間する。" },
        { sig: "void Cancel(FTweenHandle h)", desc: "進行中のトゥイーンを中止する(target は最後に書いた値で止まる)。古いハンドルは無視。" },
        { sig: "void CompleteAll()", desc: "全トゥイーンを即終了させ、target に完了値 to を書く。", when: "シーン終了時に確実に最終状態へ揃えたい時。" },
        { sig: "void CancelAll()", desc: "全トゥイーンを破棄する(最終書き込みなし)。" },
        { sig: "bool IsActive(FTweenHandle h) const / u32 ActiveCount() const", desc: "指定トゥイーンが進行中か / 進行中の総数。" },
        { sig: "void Tick(f32 dt)", desc: "毎フレーム呼ぶ。全トゥイーンを dt ぶん進めて target に書き込む。" },
        { sig: "using FTweenManager = CTweenManager", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CTweenManager</code> を使う。" }
      ]
    },
    {
      name: "FTweenHandle",
      kind: "構造体", header: "gameframework/Tween.h",
      summary: "個々の<t>トゥイーン</t>を指す<t>ハンドル</t>(index + <t>世代</t>)。古い / 完了済みのトゥイーンを誤って操作しないための識別子です。",
      when: "Tween の戻り値を保持して、後で Cancel / IsActive に渡す時。",
      members: [
        { sig: "bool IsValid() const", desc: "generation != 0 なら有効。" }
      ]
    },
    {
      name: "TTypeInfo&lt;T&gt;",
      kind: "クラステンプレート", header: "gameframework/TypeInfo.h",
      summary: "<t>RTTI</t> を使わない最小の<t>リフレクション</t>(型情報の自己記述)。型の名前・サイズ・各フィールドの名前 / オフセット / サイズを<b>コンパイル時</b>に取り出せます。シリアライザやインスペクタの土台。",
      when: "セーブ / ロードやデバッグ表示で「この構造体にはどんなフィールドがあるか」を機械的に知りたい時。",
      members: [
        { sig: "static const FTypeInfoBase& Get()", ret: "型情報", desc: "型 T の<t>リフレクション</t>情報を返す。未反射の型では field_count == 0 の空情報。" }
      ]
    },
    {
      name: "FTypeInfoBase",
      kind: "構造体", header: "gameframework/TypeInfo.h",
      summary: "型のリフレクション情報の本体。<code>Reflect&lt;T&gt;()</code> が返す型で、型名・サイズ・整列・フィールド配列・一意な型 ID を持ちます。",
      when: "リフレクション結果を走査してフィールドを列挙する時。",
      members: [
        { sig: "const c8* type_name", desc: "型名文字列(未反射なら nullptr)。" },
        { sig: "usize size / usize alignment", desc: "sizeof(T) / alignof(T)。" },
        { sig: "const FFieldInfo* fields / u32 field_count", desc: "フィールド配列とその要素数。" },
        { sig: "const void* type_tag", desc: "一意な型 ID(= TypeTag&lt;T&gt;())。" }
      ]
    },
    {
      name: "FFieldInfo",
      kind: "構造体", header: "gameframework/TypeInfo.h",
      summary: "リフレクションされた<b>1 フィールド</b>の記述。名前・型名・型先頭からのバイトオフセット・バイトサイズを持ちます。",
      when: "型情報を走査して、各フィールドのメモリ位置やサイズを使う時(汎用シリアライザなど)。",
      members: [
        { sig: "const c8* name / const c8* type_name", desc: "フィールド名 / ユーザー指定の型名文字列。" },
        { sig: "usize offset / usize size", desc: "型先頭からのバイトオフセット(offsetof) / フィールドのバイトサイズ(sizeof)。" }
      ]
    },
    {
      name: "TypeTag&lt;T&gt;() / Reflect&lt;T&gt;()",
      kind: "関数テンプレート", header: "gameframework/TypeInfo.h",
      summary: "<code>TypeTag&lt;T&gt;()</code> は <t>RTTI</t> を使わず型 T の一意な ID(static 変数のアドレス)を返します。<code>Reflect&lt;T&gt;()</code> は <code>TTypeInfo&lt;T&gt;::Get()</code> の短縮形です。",
      when: "型を ID で識別したい / 型情報を簡潔に取りたい時。",
      members: [
        { sig: "const void* TypeTag<T>()", ret: "一意な型 ID", desc: "型ごとに別アドレスを返す。RTTI 不使用の型識別に。" },
        { sig: "const FTypeInfoBase& Reflect<T>()", ret: "型情報", desc: "TTypeInfo&lt;T&gt;::Get() を返す短縮形。" }
      ]
    },
    {
      name: "ACS_GAME_REFLECT / ACS_GAME_FIELD",
      kind: "マクロ", header: "gameframework/TypeInfo.h",
      summary: "型を<t>リフレクション</t>対象として登録する<t>マクロ</t>。<code>ACS_GAME_FIELD</code> で 1 フィールドを記述し、<code>ACS_GAME_REFLECT</code> でそれらをまとめて型に紐付けます。グローバル空間で書きます。",
      when: "自作の構造体をセーブ / インスペクタ対象にしたい時。",
      members: [
        { sig: "ACS_GAME_FIELD(T, field, type_str)", desc: "FFieldInfo の初期化子を作る。名前・型名文字列・offsetof・sizeof を埋める。" },
        { sig: "ACS_GAME_REFLECT(T, fields...)", desc: "TReflectedFields&lt;T&gt; と TTypeInfo&lt;T&gt; の特殊化を生成し、型 T を反射可能にする。" }
      ]
    },
    {
      name: "CUiLayer",
      kind: "クラス", header: "gameframework/UiLayer.h",
      summary: "<t>シーン</t>に数個の UI 部品(ボタン / テキスト)を貼り付けるための<b>薄い層</b>。Init / Tick / HandleInput / Draw / Shutdown という<t>シーン</t>のライフサイクルに合わせて呼ぶだけで、タイトル画面や HUD の最小 UI が作れます。",
      when: "本格的な UI ツリーを組むほどではないが、Play ボタンや簡単な HUD を手早く出したい時。",
      members: [
        { sig: "void Init() / void Shutdown()", desc: "UI 層を初期化 / 後始末する。どちらも冪等で再呼び出し安全。" },
        { sig: "void Tick(f32 dt)", desc: "AScene::OnUpdate から呼ぶ。押下フラグの伝搬などを処理する。" },
        { sig: "void HandleInput(const acs::FEvent& event)", desc: "AScene::OnEvent から呼ぶ。マウス移動 / 押下 / 離しを処理し、クリック成立で押下フラグを立てる。" },
        { sig: "void Draw(FRenderContext& rc) const", desc: "AScene::OnDrawHud から呼ぶ。登録 widget を描画する(ボタンは hover / 押下で色変化)。" },
        { sig: "u32 AddButton(const char* label, acs::FVec2 pos, acs::FVec2 size)", ret: "ハンドル(非 0)", desc: "ボタンを追加し、label をコピー所有する。", when: "クリックで反応する矩形ボタンを置く時。" },
        { sig: "u32 AddText(const char* text, acs::FVec2 pos)", ret: "ハンドル", desc: "静的テキストを追加し、text をコピー所有する。" },
        { sig: "bool ConsumeButtonPress(u32 handle)", ret: "未消費の押下があったか", desc: "クリック成立を一度だけ取得して消費する。", when: "ボタンクリックを毎フレーム拾う時。" },
        { sig: "bool IsButtonPressed(u32 handle) const", ret: "未消費の押下があったか", desc: "既存コード向けの互換 API。ConsumeButtonPress と同じワンショット動作。" },
        { sig: "bool SetText(u32 handle, const char* text) / const char* Text(u32 handle) const", desc: "表示文字列を安全に差し替える / レイヤ所有の現在値を参照する。" },
        { sig: "void SetVisible(u32 handle, bool visible)", desc: "widget の可視性を切り替える(無効ハンドルは無視)。" },
        { sig: "void Remove(u32 handle) / void Clear()", desc: "1 つ削除 / 全削除する。Clear は初期化状態を保つので続けて追加可能。" },
        { sig: "u32 WidgetCount() const", desc: "登録 widget 数を返す。" },
        { sig: "using FUiLayer = CUiLayer", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CUiLayer</code> を使う。" }
      ]
    },
    {
      name: "FWidgetEntry",
      kind: "構造体", header: "gameframework/UiLayer.h",
      summary: "<t>CUiLayer</t> が持つ widget 1 件分の状態(ハンドル・種別・位置 / サイズ・テキスト・可視 / hover / 押下フラグ)。通常は直接触らず、CUiLayer 経由で扱います。",
      when: "CUiLayer の内部状態を理解したい / 拡張する時。",
      members: [
        { sig: "u32 handle / EWidgetKind kind", desc: "不透明 ID(0 は無効) / 種別(Button / Text)。" },
        { sig: "acs::FVec2 pos / acs::FVec2 size", desc: "画面ピクセルの絶対座標 / サイズ。" },
        { sig: "const char* text / bool visible", desc: "CUiLayer が所有するラベル / 表示テキストの読み取り用ポインタ / 可視か。" },
        { sig: "bool just_pressed / bool hovered / bool pressed_down", desc: "クリック成立(読み取りで消費) / カーソルが上か / 押し込み中か。" }
      ]
    },
    {
      name: "EWidgetKind",
      kind: "列挙(enum)", header: "gameframework/UiLayer.h",
      summary: "<t>CUiLayer</t> が扱う widget の種類。現状はボタンとテキストのみ。",
      when: "FWidgetEntry の種別を判定する時。",
      members: [
        { sig: "None = 0", desc: "無効 / 未設定。" },
        { sig: "Button = 1", desc: "クリックで押下イベントを出す短形ボタン。" },
        { sig: "Text = 2", desc: "押下判定のない静的テキスト。" }
      ]
    },
    {
      name: "IVoiceChatBackend",
      kind: "インターフェース", header: "gameframework/VoiceChat.h",
      summary: "パーティ / チーム / 全体チャンネルの<b>ボイスチャット</b>を抽象化する<t>シーム</t>(純粋仮想<t>インターフェース</t>)。実体は Steam Voice / EOS Voice / Vivox / Discord / 自前 Opus のどれでもよく、ゲーム側はこの I/F 越しに参加者管理・ミュート・音量だけを扱います。",
      when: "オンライン対戦 / 協力プレイにボイスチャットを載せたいが、使う SDK をビルド時に差し替えたい時。",
      members: [
        { sig: "virtual TResult<void> Init(EVoiceProvider p)", desc: "使うプロバイダを選んで SDK を初期化する。", when: "ゲーム開始時に 1 回。" },
        { sig: "virtual void Shutdown()", desc: "SDK を終了する。Init 前に呼んでも安全。" },
        { sig: "virtual bool IsAvailable() const / virtual EVoiceProvider ActiveProvider() const", desc: "利用可能か / 現在のプロバイダを返す。" },
        { sig: "virtual TResult<void> JoinChannel(EVoiceChannel ch, const char* channel_id)", desc: "指定チャンネルに参加する。channel_id は SDK 固有 ID(非所有)。" },
        { sig: "virtual TResult<void> LeaveChannel(EVoiceChannel ch)", desc: "チャンネルから離脱する。" },
        { sig: "virtual TResult<void> SetLocalMute(bool muted)", desc: "自分のマイクをローカルでミュート / 解除する。" },
        { sig: "virtual TResult<void> SetParticipantMute(const char* user_id, bool muted)", desc: "指定参加者を自分の耳でのみ無音化する。" },
        { sig: "virtual TResult<void> SetParticipantVolume(const char* user_id, f32 volume)", desc: "指定参加者の音量を 0.0〜1.0 で設定する。" },
        { sig: "virtual u32 ParticipantCount(EVoiceChannel ch)", ret: "参加者数", desc: "チャンネルの参加者数(自分含む)。未参加なら 0。" },
        { sig: "virtual TResult<FVoiceParticipant> GetParticipant(EVoiceChannel ch, u32 index)", ret: "参加者情報", desc: "index 番目の参加者情報を取得する。戻り値の文字列は次 Tick まで有効。", when: "参加者リスト / 発話インジケータを描く時。" },
        { sig: "virtual void Tick(f32 dt)", desc: "毎フレーム呼ぶ。音声レベル取得 / 発話検出 / イベントポンプを回す。" },
        { sig: "virtual TResult<void> PushLocalFrame(EVoiceChannel ch, const i16* pcm, u32 sample_count)", desc: "自前ループバック用に int16 PCM を 1 フレーム送る。実 SDK backend では既定で未実装。" },
        { sig: "virtual u32 PumpMixedOutput(EVoiceChannel ch, i16* out, u32 out_capacity)", ret: "書き込みサンプル数", desc: "他参加者の音声を音量 / ミュート適用して合成し out に書き込む。既定は無音(0)。" },
        { sig: "virtual f32 LocalAudioLevel() const", desc: "直近に送ったローカルフレームの音量(RMS, 0.0〜1.0)。既定は 0.0。" }
      ]
    },
    {
      name: "CVoiceChatBackendStub",
      kind: "クラス", header: "gameframework/VoiceChat.h",
      summary: "SDK 未統合ビルド / テスト用の<b>何もしない</b>ボイスチャット実装。すべての API が「未実装」エラーを返し、<code>IsAvailable()</code> は常に false です。",
      when: "実 SDK をまだ繋いでいないが、コードはコンパイル / 動作させたい時の既定実装。",
      members: [
        { sig: "bool IsAvailable() const override", desc: "常に false。" },
        { sig: "EVoiceProvider ActiveProvider() const override", desc: "Init で受けた provider を返す。" },
        { sig: "using FVoiceChatBackendStub = CVoiceChatBackendStub", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CVoiceChatBackendStub</code> を使う。" }
      ]
    },
    {
      name: "CVoiceChatLoopbackBackend",
      kind: "クラス", header: "gameframework/VoiceChat.h",
      summary: "外部 SDK なしで<b>実際に音声フレームを往復</b>させるボイスチャット実装。同一プロセス内で push→<t>エンコード</t>→他参加者キュー→<t>デコード</t>→音量 / ミュート→N-way ミックス→pump を成立させます。ネットを使わず<t>決定的</t>なのでユニットテスト可能。",
      when: "ボイスのミュート / 音量 / ミックスのロジックを SDK なしで検証したい / ローカルセッションを組みたい時。",
      members: [
        { sig: "TResult<void> AddParticipant(EVoiceChannel ch, const char* user_id, const char* display_name)", desc: "参加者を追加する。最初の 1 人がローカルユーザ(送信元 / 自分の声は聞かない)。同一 user_id の二重追加は成功扱い。" },
        { sig: "TResult<void> RemoveParticipant(EVoiceChannel ch, const char* user_id)", desc: "参加者を取り除く(受信キューも破棄)。未知 user はエラー。" },
        { sig: "TResult<void> SetPumpTarget(EVoiceChannel ch, const char* user_id)", desc: "PumpMixedOutput が「誰の耳」としてミックスするかを選ぶ(既定はローカル)。" },
        { sig: "TResult<void> PushLocalFrame(EVoiceChannel ch, const i16* pcm, u32 sample_count) override", desc: "int16 PCM を 1 フレーム送る。ch に join 済みの自分以外全員のキューに届く。" },
        { sig: "TResult<void> PushLocalFrameF32(EVoiceChannel ch, const f32* pcm, u32 sample_count) override", desc: "float[-1,1] PCM を送る(内部で int16 に量子化)。" },
        { sig: "u32 PumpMixedOutput(EVoiceChannel ch, i16* out, u32 out_capacity) override", ret: "書き込みサンプル数", desc: "対象参加者のキューを音量 / ミュート適用して合成し out に書く。" },
        { sig: "f32 LocalAudioPeak() const / u32 PendingFrameCount(EVoiceChannel ch, const char* user_id)", desc: "直近フレームのピーク値 / 指定参加者の未消費フレーム数(テスト検証用)。" },
        { sig: "static u32 EncodeFrame(const i16* pcm, u32 sample_count, u32 sequence, u8* out, u32 out_capacity)", desc: "int16 PCM を 16 byte ヘッダ付きフレームにエンコードする。ユニットテストから直接呼べる。" },
        { sig: "static TResult<u32> DecodeFrame(const u8* in, u32 in_size, i16* out, u32 out_capacity)", ret: "復元サンプル数", desc: "フレームのヘッダを検証して PCM を復元する。magic 破損などはエラー。" },
        { sig: "using FVoiceChatLoopbackBackend = CVoiceChatLoopbackBackend", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CVoiceChatLoopbackBackend</code> を使う。" }
      ]
    },
    {
      name: "FVoiceParticipant",
      kind: "構造体", header: "gameframework/VoiceChat.h",
      summary: "ボイスチャット参加者 1 人の情報。<code>GetParticipant</code> が返し、ID / 表示名(非所有)・発話中か・ローカルミュート・音声レベルを持ちます。",
      when: "参加者リストや発話インジケータ・音量メーターを UI に描く時。",
      members: [
        { sig: "const char* user_id / const char* display_name", desc: "SDK 固有 ID / 表示名(UTF-8、非所有、次 Tick まで有効)。" },
        { sig: "bool is_speaking / bool is_muted_local", desc: "発話中か / ローカルでミュート中か。" },
        { sig: "f32 audio_level", desc: "現在の音声レベル(0.0〜1.0、メーター表示用)。" }
      ]
    },
    {
      name: "FVoiceFrameHeader",
      kind: "構造体", header: "gameframework/VoiceChat.h",
      summary: "ループバック backend が運ぶ PCM フレームの固定 16 byte ヘッダ(LE)。magic / シーケンス番号 / サンプル数を持ち、デコード時に破損検出に使います。",
      when: "ループバックの音声フレーム形式を理解 / 自前で組み立てる時。",
      members: [
        { sig: "u32 magic / u32 sequence", desc: "破損検知用マジック(kVoiceFrameMagic) / 送信元ごとの単調増加シーケンス。" },
        { sig: "u32 sample_count / u32 reserved", desc: "payload の int16 サンプル数 / 将来用予約(現状 0)。" }
      ]
    },
    {
      name: "EVoiceProvider / EVoiceChannel",
      kind: "列挙(enum)", header: "gameframework/VoiceChat.h",
      summary: "<code>EVoiceProvider</code> はボイスの実装プロバイダ(None / SteamVoice / EosVoice / Vivox / Discord / OpusSelf)、<code>EVoiceChannel</code> はチャンネル種別(Party / Team / Global / Custom)を表す列挙です。",
      when: "Init でプロバイダを選ぶ / JoinChannel でチャンネル種別を指定する時。",
      members: [
        { sig: "EVoiceProvider::{None, SteamVoice, EosVoice, Vivox, Discord, OpusSelf}", desc: "未選択 / Steam Voice / EOS Voice / Vivox / Discord SDK / 自前 Opus。" },
        { sig: "EVoiceChannel::{Party, Team, Global, Custom}", desc: "パーティ / チーム / 全体 / タイトル固有の追加チャンネル。" }
      ]
    },
    {
      name: "CWaterVolume",
      kind: "クラス", header: "gameframework/WaterVolume.h",
      summary: "<t>AABB</t> で定義した 2D の<b>水域</b>を複数登録し、点が水中かの判定と、物体に掛かる<t>浮力</t> + <t>抗力</t>を力ベクトルで返す部品。「浮く」「沈む」「水中で減速」をゲームに組み込めます。",
      when: "池・川・海などの水場を作り、入った物体を浮かせたり沈ませたりしたい時。",
      members: [
        { sig: "FWaterVolumeId AddVolume(const FWaterVolumeInfo& info)", ret: "水域 ハンドル", desc: "水域を登録し、以降の参照 / 更新に使うハンドルを返す。" },
        { sig: "void RemoveVolume(FWaterVolumeId id)", desc: "水域を削除する(スロット再利用、古いハンドルは無効)。" },
        { sig: "void UpdateVolume(FWaterVolumeId id, FVec2 center, FVec2 half_size)", desc: "幾何(中心 / 半サイズ)だけ更新する。surface_y は引き継ぐので、水面位置を変えたいなら登録し直す。" },
        { sig: "bool IsUnderwater(FVec2 pos) const", ret: "水中か", desc: "どれかの水域 AABB 内に pos が入っていれば true。", when: "毎フレーム物体が水に浸かったか判定する時。" },
        { sig: "f32 SubmersionDepth(FVec2 pos) const", ret: "沈み深さ", desc: "水面からの沈み深さ(pos.y - surface_y、+Y=画面下なので沈むほど大)。水中でなければ 0。" },
        { sig: "FVec2 ComputeBuoyancyForce(FVec2 pos, FVec2 velocity, f32 mass) const", ret: "合力(world)", desc: "浮力(上向き = -Y) + 抗力(-velocity*drag)の合計を返す。重なる水域は加算。水中でなければ (0,0)。", when: "物理ボディに ApplyForce する力を得る時。" },
        { sig: "u32 VolumeCount() const", desc: "有効な水域数を返す。" },
        { sig: "const FWaterVolumeInfo* AllVolumes(u32& out_count) const", ret: "先頭ポインタ", desc: "有効な全水域を詰めた配列を返す(Add/Remove/Update で無効化)。レンダラが水面描画に使う。" },
        { sig: "void ClearAll()", desc: "全水域を破棄する。" },
        { sig: "using FWaterVolume = CWaterVolume", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CWaterVolume</code> を使う。" }
      ]
    },
    {
      name: "FWaterVolumeInfo",
      kind: "構造体", header: "gameframework/WaterVolume.h",
      summary: "水域 1 つの幾何 + パラメータ。中心 / 半サイズ・浮力強度・抗力・水面 y・水色を持ち、<code>AddVolume</code> / <code>UpdateVolume</code> に値渡しします。",
      when: "水域を 1 つ定義する時。",
      members: [
        { sig: "FVec2 center / FVec2 half_size", desc: "AABB の中心 / 半サイズ(world)。" },
        { sig: "f32 buoyancy_strength / f32 drag", desc: "浮力強度(重力相当) / 水中抗力係数。" },
        { sig: "f32 surface_y", desc: "水面 y 座標(+Y=画面下なので水面が最小 y)。center.y - half_size.y が自然値。" },
        { sig: "FVec3 water_color", desc: "レンダラが水面描画に使う色(linear RGB)。" }
      ]
    },
    {
      name: "FWaterVolumeId",
      kind: "構造体", header: "gameframework/WaterVolume.h",
      summary: "水域を識別する 32bit の<t>ハンドル</t>(24bit index + 8bit<t>世代</t>)。削除→再登録でスロット再利用されても古いハンドルは無効化されます。",
      when: "CWaterVolume の参照 / 更新 / 削除で水域を指す時。",
      members: [
        { sig: "bool IsValid() const", desc: "0(無効)でなければ true。" },
        { sig: "u32 Index() const / u8 Generation() const", desc: "index 部 / 世代部を取り出す。" }
      ]
    },
    {
      name: "CWaveSpawner",
      kind: "クラス", header: "gameframework/WaveSpawner.h",
      summary: "敵の<b><t>ウェーブ</t></b>(波状の出現)を管理する部品。複数 wave をキューに並べ、各 wave の<t>スポーンルール</t>を時間軸で順次発火させ、残数→クリア→小休止→次 wave→全完了の<t>状態機械</t>を内蔵します。",
      when: "タワーディフェンス / アリーナ / 防衛系で、敵を波ごとに湧かせ、全滅したら次の波へ進めたい時。",
      members: [
        { sig: "void Init() / void Reset() / void ClearAll()", desc: "進行をリセット(queue 保持) / 進行のみリセット / queue も含め全消去する。" },
        { sig: "void AddWave(const FWaveDef& def)", desc: "wave をキュー末尾に追加する。進行中の追記もできる(連続 wave モード)。" },
        { sig: "void StartWaves()", desc: "最初の wave からスポーン開始する。Spawning / WaitingClear 中の呼び出しは no-op。" },
        { sig: "void StopWaves() / void ResumeWaves()", desc: "一時停止(タイマー進行のみ止める) / 再開する。冪等。" },
        { sig: "void NotifyEnemyKilled(const char* enemy_id)", desc: "敵 1 体撃破を通知し生存数を 1 減らす。0 かつ全 rule 発火済なら次 wave へ進む。", when: "敵を倒した時に呼ぶ。" },
        { sig: "EWaveState CurrentState() const / u32 CurrentWaveIndex() const", desc: "現在の<t>状態</t> / 処理中の wave index を返す。" },
        { sig: "u32 TotalWaves() const / u32 EnemiesRemainingInWave() const / u32 EnemiesSpawnedInWave() const", desc: "総 wave 数 / 現 wave の生存数 / 現 wave で湧かせた総数。" },
        { sig: "f32 WaveTimerSec() const", desc: "現 wave 開始からの経過秒。Pause 中は停止。" },
        { sig: "void SetOnSpawnCallback(SpawnCallback cb, void* user)", desc: "敵 1 体出現時の<t>コールバック</t>を登録する。実 entity 生成は呼び出し側で。" },
        { sig: "void SetOnWaveStateChangeCallback(WaveStateChangeCallback cb, void* user)", desc: "wave 状態遷移時のコールバックを登録する(BGM / UI 演出連動に)。" },
        { sig: "void Tick(f32 dt)", desc: "毎フレーム呼ぶ。タイマーを進め、満たした rule の発火 / 状態遷移を行う。" },
        { sig: "using FWaveSpawner = CWaveSpawner", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CWaveSpawner</code> を使う。" }
      ]
    },
    {
      name: "FSpawnRule",
      kind: "構造体", header: "gameframework/WaveSpawner.h",
      summary: "1 つの<t>スポーンルール</t>。「どの敵を・何体・どの間隔で・どこに」湧かせるかを定義します。1 つの wave が任意数のルールを持てます。",
      when: "wave の中身(敵の出し方)を定義する時。",
      members: [
        { sig: "const char* enemy_id", desc: "敵種別の識別子(リテラル推奨、非所有)。" },
        { sig: "u32 count", desc: "この rule で湧かせる総数。" },
        { sig: "f32 spawn_interval_sec / f32 initial_delay_sec", desc: "2 体目以降の間隔(秒) / wave 開始から 1 体目までの遅延(秒)。" },
        { sig: "FVec2 spawn_position", desc: "出現位置(world 座標)。分散させたいなら別 rule を並べる。" }
      ]
    },
    {
      name: "FWaveDef",
      kind: "構造体", header: "gameframework/WaveSpawner.h",
      summary: "1 つの<t>ウェーブ</t>の定義。<t>スポーンルール</t>配列(呼び出し側所有)と、クリア後の小休止時間を持ちます。",
      when: "AddWave に渡す 1 波分を組み立てる時。",
      members: [
        { sig: "const char* wave_id", desc: "デバッグ / 表示用の識別子(nullable)。" },
        { sig: "u32 rule_count / const FSpawnRule* rules", desc: "ルール数 / ルール配列(呼び出し側所有、寿命を保証する責任あり)。rule_count==0 は即クリアの空 wave。" },
        { sig: "f32 wave_intermission_sec", desc: "クリアから次 wave までの待機秒(最後の wave では全完了までの演出時間)。" }
      ]
    },
    {
      name: "EWaveState",
      kind: "列挙(enum)", header: "gameframework/WaveSpawner.h",
      summary: "<t>ウェーブ</t>進行の<t>状態</t>を表す列挙。値はセーブ / リプレイに書ける安定値です。",
      when: "現在 wave がどの段階かで UI / BGM を分岐する時。",
      members: [
        { sig: "Idle = 0", desc: "開始前 / Reset 後。" },
        { sig: "Spawning = 1", desc: "現 wave の rule を実行中(まだ全部湧かせていない)。" },
        { sig: "WaitingClear = 2", desc: "全 rule 発火済、残敵 0 を待っている。" },
        { sig: "Cleared = 3", desc: "現 wave クリア、小休止で次 wave へ。" },
        { sig: "AllComplete = 4", desc: "全 wave 完了。" }
      ]
    },
    {
      name: "CWeaponSystem",
      kind: "クラス", header: "gameframework/WeaponSystem.h",
      summary: "1 体ぶんの<b>武器ロードアウト + 弾薬 + 連射制御</b>をまとめた小型マネージャ。<t>fire rate</t>・リロード時間・マガジン / 予備弾薬・拡散 / ペレット数を一体管理し、発射可否を判定します。実際の弾生成は<t>コールバック</t>で呼び出し側に任せます。",
      when: "FPS / TPS / シューティングで、複数武器の切替・残弾・連射クールダウンを扱いたい時(1 体につき 1 インスタンス)。",
      members: [
        { sig: "void RegisterWeapon(const FWeaponDef& def)", desc: "武器を登録する。同 id の二重登録は警告して no-op。reserve スロットも 0 で初期化。" },
        { sig: "bool EquipWeapon(const char* weapon_id)", ret: "成功したか", desc: "武器を装備する。未登録 / nullptr は false。reload 中でも切替可能(元武器の reload は破棄)。", when: "武器スロット切替時。" },
        { sig: "bool TryFire()", ret: "発射できたか", desc: "fire rate / mag / reload を満たせば 1 発消費し FireCallback を発火、true を返す。失敗時は副作用なしで false。" },
        { sig: "void StartReload() / void CancelReload()", desc: "リロード開始 / 中断。装備なし / reload 中 / mag 満タン / reserve 0 では開始しない。reload_sec<=0 は即時完了。" },
        { sig: "void AddReserveAmmo(const char* weapon_id, u32 amount)", desc: "予備弾薬を加算する(max_reserve でクランプ)。未登録 / amount==0 は no-op。" },
        { sig: "u32 GetAmmoInMag() const / u32 GetReserveAmmo() const", desc: "マガジン内残弾 / 予備弾薬を返す。" },
        { sig: "const FWeaponDef* CurrentDef() const / const FWeaponState& State() const", desc: "装備中武器の定義 / ランタイム状態を返す。" },
        { sig: "bool IsReloading() const / f32 ReloadProgress() const", desc: "reload 中か / reload 進行度 [0,1] を返す。", when: "リロードゲージを描く時。" },
        { sig: "void SetOnFireCallback(FireCallback cb, void* user)", desc: "発射時の<t>コールバック</t>を登録する(弾種 / ダメージ / 拡散 / ペレット数を通知)。" },
        { sig: "void SetOnReloadCompleteCallback(ReloadCallback cb, void* user)", desc: "リロード完了時のコールバックを登録する。" },
        { sig: "void Tick(f32 dt)", desc: "毎フレーム呼ぶ。内部時計を進め、reload 中なら残り時間を減らし完了処理する。" },
        { sig: "void ClearAll()", desc: "武器定義 / reserve / 状態 / コールバックを全消去する。" },
        { sig: "using FWeaponSystem = CWeaponSystem", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CWeaponSystem</code> を使う。" }
      ]
    },
    {
      name: "FWeaponDef",
      kind: "構造体", header: "gameframework/WeaponSystem.h",
      summary: "武器 1 種類の<b>不変定義</b>。id / 表示名 / 種別と、fire rate・リロード時間・マガジン容量・予備上限・ダメージ・拡散・ペレット数・弾種を持ちます。",
      when: "RegisterWeapon に渡す武器スペックを定義する時。",
      members: [
        { sig: "const char* id / const char* display_name", desc: "武器キー(Register / Equip のキー) / UI 表示名(非所有)。" },
        { sig: "EWeaponKind kind", desc: "武器ジャンル(メタ情報、機械挙動には不使用)。" },
        { sig: "f32 fire_rate_per_sec / f32 reload_sec", desc: "1 秒あたりの発射回数 / リロード秒数。どちらも<=0 はフォールバック(1 発/秒 / 即時)。" },
        { sig: "u32 mag_size / u32 max_reserve", desc: "マガジン容量(0 はマガジン非搭載) / 予備上限(0 は無制限)。" },
        { sig: "f32 base_damage / f32 spread_deg / u32 pellets_per_shot", desc: "1 ペレットの基本ダメージ / 拡散角(片側 deg) / 1 発のペレット数(0 は 1)。" },
        { sig: "const char* projectile_id", desc: "発射する弾種 id(非所有、FireCallback へ伝達)。" }
      ]
    },
    {
      name: "FWeaponState",
      kind: "構造体", header: "gameframework/WeaponSystem.h",
      summary: "現在装備中武器の<b>ランタイム状態</b> snapshot。残弾・次発射可能時刻・reload 中フラグ / 残り時間を持ち、主に UI / デバッグ表示に使います。",
      when: "HUD に残弾やリロードゲージを出す時。",
      members: [
        { sig: "const char* current_def_id", desc: "装備中武器の id。" },
        { sig: "u32 ammo_in_mag / u32 reserve_ammo", desc: "マガジン内残弾 / 予備弾薬。" },
        { sig: "f32 next_fire_time_sec", desc: "この時刻まで TryFire は失敗する(連射間隔の基準)。" },
        { sig: "bool reloading / f32 reload_remaining_sec", desc: "reload 中フラグ / 完了までの残り秒。" }
      ]
    },
    {
      name: "EWeaponKind",
      kind: "列挙(enum)", header: "gameframework/WeaponSystem.h",
      summary: "武器のジャンルを表す列挙。UI 表示 / アニメ分岐 / ダメージ種別判定などの<b>参考情報</b>で、機械的な発射挙動は FWeaponDef のパラメータで決まります。",
      when: "武器アイコンやアニメを種別で切り替える時。",
      members: [
        { sig: "Pistol / Rifle / Shotgun / Sniper", desc: "単発ハンドガン / 長銃 / ペレット拡散 / 高ダメージ低 fire-rate。" },
        { sig: "RocketLauncher / Sword / Bow / Custom", desc: "爆発物 / 近接斬撃 / 弓 / それ以外。" }
      ]
    },
    {
      name: "CWeatherSystem",
      kind: "クラス", header: "gameframework/WeatherSystem.h",
      summary: "<b>天候</b>の状態を保持し、現在天候→目標天候へ線形遷移しながら、描画 / ライティング用の修飾係数(環境光倍率 / 粒子密度 / 空の色補正 / 風 / 霧密度)を提供する部品。レンダラや粒子は毎フレームこれらを pull するだけで天候表現が反映されます。",
      when: "晴れ / 雨 / 雪 / 嵐などをなめらかに切り替え、見た目と粒子・風を連動させたい時。",
      members: [
        { sig: "void SetWeather(EWeatherKind kind, f32 transition_duration = 5.0f)", desc: "目標天候を設定する。同一天候は即完了、duration<=0 は即時切替。", when: "天候を変えたい時。" },
        { sig: "EWeatherKind CurrentWeather() const / EWeatherKind TargetWeather() const", desc: "現在 / 目標の天候を返す。" },
        { sig: "f32 TransitionT() const", ret: "遷移率 [0,1]", desc: "遷移の進み具合。1 で完了(current == target に snap 済み)。" },
        { sig: "void Tick(f32 dt)", desc: "毎フレーム呼ぶ。遷移を進める。" },
        { sig: "f32 AmbientLightMultiplier() const", ret: "環境光倍率", desc: "1.0 で通常。Storm / Sandstorm 中は 0.5 まで暗化。", when: "環境光に掛けて天候の明暗を反映する時。" },
        { sig: "f32 ParticleDensity() const / f32 FogDensityMultiplier() const", desc: "粒子密度倍率(雨 / 雪 / 嵐で増、Clear=0) / 霧密度倍率(Fog / Sandstorm で増)。" },
        { sig: "FVec3 SkyTintMultiplier() const", ret: "空色補正", desc: "空の色に乗算する補正((1,1,1)=無補正、Sandstorm は黄褐色など)。" },
        { sig: "f32 WindStrength() const", ret: "風の強さ [0,1]", desc: "Storm=1.0、Clear=0.0、Rain / Snow は中間。" },
        { sig: "void SetWindDirection(FVec2 dir) / FVec2 WindDirection() const", desc: "風向きを設定 / 取得する(天候とは独立、既定は (1,0))。" },
        { sig: "void Reset()", desc: "初期状態(Clear)へ戻す。" },
        { sig: "using FWeatherSystem = CWeatherSystem", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CWeatherSystem</code> を使う。" }
      ]
    },
    {
      name: "CWeatherSystem::FKindParams",
      kind: "構造体", header: "gameframework/WeatherSystem.h",
      summary: "天候ごとの修飾パラメータ(環境光倍率 / 粒子密度 / 空色 / 風強さ / 霧密度)を 1 つにまとめた表。各 getter が内部でこの表を参照します。",
      when: "天候パラメータの内部構造を理解する時(通常は getter 経由で十分)。",
      members: [
        { sig: "f32 ambient_mult / f32 particle_density / f32 fog_density", desc: "環境光倍率 / 粒子密度 / 霧密度。" },
        { sig: "FVec3 sky_tint / f32 wind_strength", desc: "空色補正 / 風の強さ。" }
      ]
    },
    {
      name: "EWeatherKind",
      kind: "列挙(enum)", header: "gameframework/WeatherSystem.h",
      summary: "天候種別を表す 8 値の列挙(晴れ / 曇り / 雨 / 豪雨 / 雪 / 嵐 / 霧 / 砂嵐)。値はセーブに書ける安定値です。",
      when: "SetWeather で天候を指定する / 現在天候で分岐する時。",
      members: [
        { sig: "Clear / Cloudy / Rain / HeavyRain", desc: "快晴 / 曇り / 雨 / 豪雨。" },
        { sig: "Snow / Storm / Fog / Sandstorm", desc: "雪 / 嵐(強風+豪雨) / 霧 / 砂嵐(強風+視界不良)。" }
      ]
    },
    {
      name: "IWorkshopBridge",
      kind: "インターフェース", header: "gameframework/WorkshopBridge.h",
      summary: "Steam Workshop / EOS UGC / mod.io などの<b>ユーザー生成コンテンツ(<t>UGC</t>)</b>プラットフォームへ橋渡しする<t>シーム</t>(純粋仮想<t>インターフェース</t>)。MOD の公開・購読・ダウンロードを、使う SDK をビルド時に差し替えながら扱えます。",
      when: "MOD やステージ共有などの UGC 機能を載せたいが、配信プラットフォームの SDK を後で差し替えたい時。",
      members: [
        { sig: "virtual TResult<void> Init() / virtual void Shutdown()", desc: "SDK を初期化 / 終了する。Shutdown は Init 前でも安全。" },
        { sig: "virtual bool IsAvailable() const", ret: "利用可能か", desc: "現ビルドで Workshop 機能が使えるか。Stub は常に false。", when: "UI で Workshop ボタンの表示判定に使う時。" },
        { sig: "virtual TResult<u64> CreateItem(const char* title, const char* content_path)", ret: "新規 item_id", desc: "新規 UGC アイテムを公開する。content_path はローカルパス。" },
        { sig: "virtual TResult<void> UpdateItem(u64 item_id, const char* content_path, const char* change_note)", desc: "既存アイテムを差分更新する(change_note は変更履歴に表示)。" },
        { sig: "virtual TResult<FWorkshopItem> QueryItem(u64 item_id)", ret: "アイテム情報", desc: "アイテムのメタ情報を取得する。戻り値の文字列は次 Tick まで有効。" },
        { sig: "virtual TResult<u32> QuerySubscribedCount()", desc: "ローカルプレイヤーが購読中のアイテム数を返す。" },
        { sig: "virtual TResult<void> SubscribeItem(u64 item_id) / virtual TResult<void> UnsubscribeItem(u64 item_id)", desc: "アイテムを購読 / 購読解除する。" },
        { sig: "virtual TResult<void> DownloadItem(u64 item_id)", desc: "明示的にダウンロードを開始する(購読済みの再 DL など)。" },
        { sig: "virtual f32 GetDownloadProgress(u64 item_id)", ret: "進捗 [0,1] / -1", desc: "ダウンロード進捗を返す。完了 / 未開始 / 不明は -1。ポーリング前提。", when: "DL バーを毎フレーム更新する時。" },
        { sig: "virtual void Tick(f32 dt)", desc: "毎フレーム呼ぶ。SteamAPI_RunCallbacks 相当のイベントポンプを回す。" }
      ]
    },
    {
      name: "CWorkshopBridgeStub",
      kind: "クラス", header: "gameframework/WorkshopBridge.h",
      summary: "Workshop SDK 未統合ビルド / テスト用の<b>何もしない</b>実装。<code>Init()</code> だけ成功し、公開 / 購読 / DL 系は「未実装」エラー、<code>IsAvailable()</code> は常に false です。",
      when: "実 SDK を繋ぐ前の既定実装として、コンパイルと UI 判定だけ通したい時。",
      members: [
        { sig: "TResult<void> Init() noexcept override", desc: "常に成功(m_Initialized = true)。" },
        { sig: "bool IsAvailable() const noexcept override", desc: "常に false。" },
        { sig: "f32 GetDownloadProgress(u64) noexcept override", desc: "常に -1 を返す。" },
        { sig: "using FWorkshopBridgeStub = CWorkshopBridgeStub", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CWorkshopBridgeStub</code> を使う。" }
      ]
    },
    {
      name: "FWorkshopItem",
      kind: "構造体", header: "gameframework/WorkshopBridge.h",
      summary: "プラットフォーム共通の<t>UGC</t>アイテムのメタ情報。item_id / タイトル / 説明 / 作者(文字列は非所有)・ファイルサイズ・インストール / 購読状態を持ちます。",
      when: "MOD ブラウザ UI にアイテム一覧を表示する時。",
      members: [
        { sig: "u64 item_id / u64 file_size", desc: "SDK 固有の opaque ID / DL 後のファイルサイズ(バイト)。" },
        { sig: "const char* title / const char* description / const char* author", desc: "表示タイトル / 説明 / 投稿者名(UTF-8、非所有、次 Tick まで有効)。" },
        { sig: "bool installed / bool subscribed", desc: "ローカルに DL 済みか / ローカルプレイヤーが購読中か。" }
      ]
    },
    {
      name: "GetVoiceStub / GetVoiceLoopback / GetWorkshopStub",
      kind: "関数", header: "gameframework/VoiceChat.h, WorkshopBridge.h",
      summary: "各<t>シーム</t>の<b>既定実装</b>を返す static singleton アクセサ。実 SDK を DI する前でも、これらを使うだけでコンパイル / 動作させられます。",
      when: "ボイスチャットや Workshop を、実 SDK を繋がずにとりあえず動かしたい時。",
      members: [
        { sig: "IVoiceChatBackend& GetVoiceStub()", desc: "no-op なボイス実装の singleton を返す。" },
        { sig: "CVoiceChatLoopbackBackend& GetVoiceLoopback()", desc: "実際に音声を往復させるループバック実装の singleton を返す。" },
        { sig: "IWorkshopBridge& GetWorkshopStub()", desc: "no-op な Workshop 実装の singleton を返す。" }
      ]
    },
    {
      name: "EAudioFormat",
      kind: "列挙(enum)", header: "gameframework/audio_backend/IAudioBackend.h",
      summary: "<code>FAudioClipDesc</code> が指す PCM バッファの形式。<code>Pcm16</code>(16bit signed PCM) / <code>Pcm32Float</code>(32bit IEEE float) / <code>Wav</code>(RIFF ヘッダ込み)。",
      when: "再生する clip のサンプル形式を指定する時。",
      members: [
        { sig: "Pcm16 = 0", desc: "16bit signed PCM (典型的な WAV PCM の raw bytes)。" },
        { sig: "Pcm32Float = 1", desc: "32bit IEEE float PCM (高品質・DSP-friendly)。" },
        { sig: "Wav = 2", desc: "ファイルからロード済みの WAV 形式。解析処理は呼び出し側が用意する。" }
      ]
    },
    {
      name: "IAudioBackend",
      kind: "インターフェース", header: "gameframework/audio_backend/IAudioBackend.h",
      summary: "<code>CAudioDirector</code> から見た「実際に音を出す層」の<t>インターフェース</t>。BGM / SFX を <code>FAudioVoiceHandle</code> で扱い、一発再生 / ループ / 停止に加えて再生中 voice の音量・左右パン・pitch 更新を提供する。Windows では <code>CXAudio2Backend</code>、他プラットフォームは別実装で差し替える。",
      when: "director の下に実音声バックエンドを差し込みたい時。backend を nullptr にすると無音 (no-op) で動く。",
      members: [
        { sig: "virtual TResult<void> Init(u32 max_voices = 64)", ret: "成否", desc: "backend を初期化。<code>max_voices</code> は同時発音数の上限 (0 は不正)。多重 Init は <code>kSubAudioAlreadyInitialized</code>。" },
        { sig: "virtual void Shutdown()", desc: "全 voice を停止して資源解放。Init 前に呼んでも安全 (no-op)。" },
        { sig: "virtual bool IsInitialized() const", desc: "Init 済かつ実バックエンドが正常起動した状態か。" },
        { sig: "virtual FAudioVoiceHandle PlayOneShot(const FAudioClipDesc& clip, f32 volume, f32 pitch)", ret: "voice handle", desc: "一発再生 (loop なし、終端で自動回収)。<code>pitch</code> は 1.0=等倍 / 0.5=1 オクターブ低 / 2.0=高。失敗時は <code>kInvalidAudioVoice</code>。", when: "効果音を 1 回鳴らす時。" },
        { sig: "virtual FAudioVoiceHandle PlayLooped(const FAudioClipDesc& clip, f32 volume, f32 pitch)", ret: "voice handle", desc: "ループ再生 (StopVoice までずっと鳴り続ける)。", when: "環境音や BGM を鳴らす時。" },
        { sig: "virtual void StopVoice(FAudioVoiceHandle voice)", desc: "指定 voice を停止し slot を解放。無効 / 既に解放済ハンドルは no-op。" },
        { sig: "virtual void SetVoiceVolume(FAudioVoiceHandle voice, f32 volume)", desc: "指定 voice の音量を変更。無効 / 解放済は no-op。範囲外は clamp 推奨。" },
        { sig: "virtual void StopAllVoices()", desc: "全 voice を停止して slot 解放。Init 前は no-op。" },
        { sig: "virtual u32 ActiveVoiceCount() const", desc: "現在再生中 (slot active) の voice 数。デバッグ / UI メーター用。" },
        { sig: "virtual void Tick(f32 dt)", desc: "完了した一発再生 voice の slot 解放等、内部状態を進める。<code>dt</code> は実時間秒。", when: "ゲームループから毎フレーム呼ぶ。" },
        { sig: "virtual void SetVoiceParameters(FAudioVoiceHandle voice, f32 volume, f32 pan, f32 pitch)", desc: "再生中 voice の音量・左右パン・pitch を 1 回の backend 呼び出しで更新する。既定実装は volume だけを <code>SetVoiceVolume</code> へ渡す。無効 / 解放済 handle は no-op。" }
      ]
    },
    {
      name: "CXAudio2Backend",
      kind: "クラス", header: "gameframework/audio_backend/XAudio2Backend.h",
      summary: "<t>IAudioBackend</t> の Windows 用 concrete 実装 = Win32 <b>XAudio2</b> を叩いて実音声を出す。固定容量 voice pool と不透明 handle で解放済み voice を拒否し、mono source は出力 speaker mask の front-left / front-right へ constant-power pan を適用する。stereo / 多 channel source は既存 matrix を保つ。",
      when: "Windows で <code>CAudioDirector</code> に実音声を鳴らさせたい時。<code>director.SetBackend(&amp;xaudio2)</code> で差し込む。",
      members: [
        { sig: "CXAudio2Backend()", desc: "backend を構築 (まだ未初期化)。Init を呼ぶまで音は出ない。" },
        { sig: "TResult<void> Init(u32 max_voices = 64) override", ret: "成否", desc: "XAudio2 / COM を起動し voice pool を確保する。再 init 不可。" },
        { sig: "void SetVoiceParameters(FAudioVoiceHandle voice, f32 volume, f32 pan, f32 pitch) override", desc: "1 回の backend 操作で volume と pitch を更新し、mono source と有効な左右 speaker 出力にだけ pan matrix を適用する。個別の XAudio2 更新失敗は警告し、次 frame の更新で再試行できる。" },
        { sig: "void SetMasterVolume(f32 volume)", desc: "マスタリングボイス音量 (最終出力の master volume)。0.0〜1.0 推奨 (>1.0 は歪む)。", when: "全体音量をまとめて下げたい時。" },
        { sig: "struct FImpl", desc: "XAudio2 / COM の重ヘッダを <code>.cpp</code> に閉じ込める <t>pimpl</t> の前方宣言 (実体は <code>.cpp</code>)。" },
        { sig: "using FXAudio2Backend = CXAudio2Backend", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CXAudio2Backend</code> を使う。" }
      ]
    },
    {
      name: "FAudioClipDesc",
      kind: "構造体", header: "gameframework/audio_backend/IAudioBackend.h",
      summary: "再生する音声バッファの記述子。フォーマット + データ参照を持つ。<code>pcm_data</code> は backend 側が必要な間 (一発再生終了 / Stop まで) 内部コピーする。",
      when: "<code>PlayOneShot</code> / <code>PlayLooped</code> に渡す clip を組み立てる時。",
      members: [
        { sig: "const void* pcm_data = nullptr", desc: "raw PCM サンプル列 (Wav 形式の場合は RIFF ヘッダ込み)。" },
        { sig: "u64 pcm_size = 0", desc: "<code>pcm_data</code> の有効バイト数。" },
        { sig: "u32 sample_rate = 0", desc: "1 チャネルあたりサンプル/秒 (例 44100 / 48000)。" },
        { sig: "u32 channel_count = 0", desc: "チャネル数 (1=mono / 2=stereo)。" },
        { sig: "EAudioFormat format = EAudioFormat::Pcm16", desc: "上記 <code>EAudioFormat</code>。" }
      ]
    },
    {
      name: "FAudioVoiceHandle / kInvalidAudioVoice",
      kind: "構造体", header: "gameframework/audio_backend/IAudioBackend.h",
      summary: "再生中の 1 voice を指す 32bit ABI の不透明な packed <t>ハンドル</t>。全 0 (<code>kInvalidAudioVoice</code>) は無効。<code>Index</code> / <code>Generation</code> と 2 引数 constructor は index + generation 形式を使う backend 向けの互換表現であり、すべての backend にその内訳を要求しない。<code>CXAudio2Backend</code> は 32bit 全体をプロセス通算の不透明チケットとして発行するため、呼び出し側は値を分解せず保持して返す。",
      when: "<code>PlayOneShot</code> / <code>PlayLooped</code> の戻り値を保持して後で停止・音量変更する時。",
      members: [
        { sig: "constexpr FAudioVoiceHandle()", desc: "無効ハンドル (<code>m_Packed == 0</code>) を作る。" },
        { sig: "constexpr FAudioVoiceHandle(u32 index, u8 gen)", desc: "互換形式として <code>(index &amp; 0x00FFFFFF) | (gen &lt;&lt; 24)</code> に詰めて構築する。" },
        { sig: "static constexpr FAudioVoiceHandle FromPackedValue(u32 packed_value)", ret: "voice handle", desc: "backend が発行した 0 以外の 32bit 不透明値をそのまま保持するハンドルを構築する。" },
        { sig: "bool IsValid() const", desc: "<code>m_Packed != 0</code> か。確保失敗を弾く。" },
        { sig: "u32 Index() const", desc: "互換形式における下位 24bit。不透明チケットの slot index を保証しない。" },
        { sig: "u8 Generation() const", desc: "互換形式における上位 8bit。不透明チケットの generation を保証しない。" },
        { sig: "u32 PackedValue() const", desc: "backend 間の受け渡しに使う 32bit の不透明値を返す。" },
        { sig: "inline constexpr FAudioVoiceHandle kInvalidAudioVoice{}", ret: "無効値", desc: "全 0 の無効ハンドル定数。" }
      ]
    },
    {
      name: "FAssetLockInfo",
      kind: "構造体", header: "gameframework/StudioWorkflow.h",
      summary: "<t>IAssetLockingBackend</t> の <code>QueryLock</code> が返すロック情報 (P4 の <code>p4 fstat</code> 相当の最小情報)。文字列は backend が所有せず、寿命は「次の Backend 呼び出しまで」を保証する。",
      when: "あるアセットが誰にいつロックされているかを問い合わせた結果を読む時。",
      members: [
        { sig: "const char* asset_path = nullptr", desc: "ロック対象パス (例 <code>//depot/game/foo.fbx</code>)。" },
        { sig: "const char* locker_user = nullptr", desc: "ロック保持ユーザー (例 <code>designer_a</code>)。" },
        { sig: "u64 lock_time = 0", desc: "ロック取得時刻 (実装依存; UNIX epoch 推奨)。" }
      ]
    },
    {
      name: "CTypeAutoRegister",
      kind: "構造体", header: "gameframework/Reflect.h",
      summary: "静的初期化時にtypeをregistryへ登録する補助。",
      when: "型登録macroの生成コードから使う時。",
      members: [
        { sig: "using FTypeAutoRegister = CTypeAutoRegister", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CTypeAutoRegister</code> を使う。" }
      ]
    },
    {
      name: "CTypeRegistry",
      kind: "クラス", header: "gameframework/Reflect.h",
      summary: "reflection対象typeの登録と検索を提供する。",
      when: "型情報を名前または識別子から取得する時。",
      members: [
        { sig: "using FTypeRegistry = CTypeRegistry", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CTypeRegistry</code> を使う。" }
      ]
    },
    {
      name: "AWorldClockSubsystem",
      kind: "クラス", header: "gameframework/WorldClockSubsystem.h",
      summary: "worldが所有し、simulation時刻とscaleを更新するsubsystem。",
      when: "world単位の時刻を取得または進める時。",
      members: [
        { sig: "using FWorldClockSubsystem = AWorldClockSubsystem", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AWorldClockSubsystem</code> を使う。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "トリガ": "重なりだけを検出する当たり判定。物理の押し戻しはせず「触れたか」だけを通知する。関連語は <t>トリガワールド</t>。",
  "トリガワールド": "<t>トリガ</t>をまとめて管理し、毎フレーム重なりを調べて enter / stay / exit <t>イベント</t>を発火する場。",
  "AABB": "Axis-Aligned Bounding Box。軸に平行な四角形の当たり判定。回転しない箱として軽量に重なりを調べられる。",
  "AP": "アクションポイント。ターン制で各陣営が 1 ターンに使える行動予算。<t>CTurnManager</t> が管理する。",
  "イニシアチブ": "ターン制での行動順を決める値。大きいほど先に行動できる。",
  "ラウンド": "ターン制で全陣営が 1 巡する単位。1 ラウンドの中に各陣営のターンが並ぶ。",
  "フェーズ": "ターン進行の局面(プレイヤーの番 / 敵の番など)。<t>ETurnPhase</t> で表す。",
  "トゥイーン": "ある値を一定時間かけて始点から終点へなめらかに変化させる仕組み。<t>イージング</t>で変化カーブを指定する。",
  "イージング": "トゥイーンの加減速カーブ。Linear(等速)や InOutSine(ゆっくり始まりゆっくり終わる)など。",
  "RTTI": "Run-Time Type Information。C++ 標準の実行時型情報。ACS では使わず、軽量な型 ID / <t>リフレクション</t>で代替する。",
  "リフレクション": "型が自身の構造(フィールド名 / 型 / オフセット)を自己記述する仕組み。シリアライザやインスペクタの土台になる。",
  "ウェーブ": "敵が波状にまとまって出現する単位。倒しきると次の波へ進む。<t>CWaveSpawner</t> が管理する。",
  "スポーンルール": "ウェーブ内で「どの敵を・何体・どの間隔で・どこに」湧かせるかの 1 設定。<t>FSpawnRule</t>。",
  "状態機械": "限られた状態と、その間の遷移ルールで動きを表す仕組み。ステートマシンとも。",
  "状態": "オブジェクトが取りうる局面を区別した値(例: Idle / Spawning)。<t>状態機械</t>の各点。",
  "fire rate": "1 秒あたりの発射回数。連射間隔 = 1.0 / fire rate で決まる。",
  "浮力": "水中の物体を上向きに押し上げる力。ACS では水深 × 強さ × 質量の簡易モデルで近似する。",
  "抗力": "水中で速度に逆らう減速力(-velocity × drag)。水の粘性的な抵抗を表す。",
  "シーム": "実装を後から差し替えられる純粋仮想<t>インターフェース</t>の境界。実 SDK の有無に関わらずコードを書ける。",
  "UGC": "User Generated Content。ユーザーが作った MOD / ステージ等のコンテンツ。Workshop 等で配布される。",
  "エンコード": "データを送受信 / 保存用の形式(ここではフレーム化したバイト列)に変換すること。逆は<t>デコード</t>。",
  "デコード": "エンコードされたデータを元の形(ここでは int16 PCM)に戻すこと。",
  "決定的": "同じ入力なら必ず同じ結果になる性質。乱数や時刻に依存せず、テストや再現がしやすい。",
  "trauma": "画面振動 (camera shake) の強さを表す [0,1] の連続値。被弾などで加算し、毎フレーム減衰させる。振動量を trauma の 2〜3 乗にすると小さい揺れが目立たず自然になる手法 (GDC の定番)。",
  "pimpl": "pointer to implementation。クラスの実装詳細 (重いヘッダや OS 依存型) を前方宣言したポインタの先に隠し、公開ヘッダのインクルード負荷とコンパイル依存を減らすイディオム。"
});
