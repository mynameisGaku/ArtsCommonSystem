/* ACS リファレンス — event モジュール。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > は &lt; &gt;。 */
ACS_REF.modules.push({
  id: "event",
  order: 22,
  title: "event — イベント / シグナル",
  blurb: "「○○が起きた」を発信する側と受け取る側を<b>疎結合</b>につなぐ仕組み。型付きデリゲートと局所イベント、同じスレッド内で即配信する<t>pub/sub</t>、スレッドをまたいで値を受け渡すキュー、時間差で関数を呼ぶタイマーが揃っています。",
  types: [
    {
      name: "EMessagePipePolicy",
      kind: "列挙", header: "event/MessagePipePolicy.h",
      summary: "<code>TMessagePipe</code> の MPMC mutex 経路と固定容量 SPSC lock-free 経路を選ぶ同期方針。",
      when: "producer / consumer 数と待機要件に合わせてパイプ実装を静的に選ぶ時。"
    },
    {
      name: "FTimerDiagnostics",
      kind: "構造体", header: "event/TimerDiagnostics.h",
      summary: "timer の cancel probe、active slot 走査、bitset 読み取り量を保持する診断 snapshot。",
      when: "大量 timer の cancel と Tick が件数に対して適切に拡張するか確認する時。"
    },
    {
      name: "ETimerSchedulePolicy",
      kind: "列挙", header: "event/TimerSchedulePolicy.h",
      summary: "型付き timer 登録で単発実行と反復実行をコンパイル時に選ぶ方針。",
      when: "callback thunk の分岐を登録時に固定したい時。"
    },
    {
      name: "TDelegate&lt;Signature&gt;",
      kind: "クラステンプレート", header: "event/Delegate.h",
      summary: "関数形式を型引数に取り、静的関数またはメンバー関数と任意ポインターをメモリ確保なしで一組だけ保持する。",
      when: "引数や戻り値を持つ処理を、対象と一緒に軽量な値として保持したい時。",
      members: [
        { sig: "CreateStatic(function, user) / CreateStatic&lt;Function&gt;()", ret: "設定済みデリゲート", desc: "関数ポインターまたはコンパイル時に指定した静的関数を結び付ける。" },
        { sig: "CreateRaw&lt;Method&gt;(object)", ret: "設定済みデリゲート", desc: "メンバー関数を対象へ結び付ける。登録中の対象寿命は呼出し側が保つ。" },
        { sig: "bool ExecuteIfBound(arguments...) / bool TryExecute(out_result, arguments...)", ret: "実行できたか", desc: "戻り値なしまたは戻り値ありの関数を、設定済みの場合だけ呼ぶ。" },
        { sig: "void Unbind() / bool IsBound() const", desc: "結び付きを解除し、現在設定済みかを確認する。" }
      ]
    },
    {
      name: "FSimpleDelegate",
      kind: "型エイリアス", header: "event/SimpleDelegate.h",
      summary: "引数なしの関数またはメンバー関数と任意ポインターを、メモリ確保なしで一組だけ保持する <code>TDelegate&lt;void()&gt;</code>。",
      when: "処理を後から一度だけ呼ぶ場所や、タイマーへ関数と対象をまとめて渡す時。",
      members: [
        { sig: "CreateStatic(function, user) / CreateStatic&lt;Function&gt;()", ret: "設定済みデリゲート", desc: "静的関数を結び付ける。空の関数は未設定になる。" },
        { sig: "CreateRaw&lt;Method&gt;(object)", ret: "設定済みデリゲート", desc: "メンバー関数を対象へ結び付ける。登録中の対象寿命は呼出し側が保つ。" },
        { sig: "bool ExecuteIfBound()", ret: "実行できたか", desc: "設定済みなら処理を呼び、未設定なら何もせず false を返す。" },
        { sig: "void Unbind()", desc: "関数と任意ポインターの結び付きを解除する。" }
      ]
    },
    {
      name: "TEvent&lt;Arguments...&gt;",
      kind: "クラステンプレート", header: "event/TypedEvent.h",
      summary: "引数型を保ったまま、同じスレッド内の複数購読先へ同期通知する局所イベント。世代付き解除、一回購読、優先度、所有購読に対応する。",
      when: "通知する引数型がコンパイル時に決まり、実行時の型番号を使わず部品内や所有者内で通知したい時。",
      members: [
        { sig: "FTypedEventHandle Subscribe(callback, user)", ret: "購読ハンドル", desc: "関数を追加する。配信中に追加した関数は次回配信から有効になる。" },
        { sig: "SubscribeOnce / SubscribeWithPriority / SubscribeOnceWithPriority", ret: "購読ハンドル", desc: "一回限り、優先度付き、または両方の条件で関数を追加する。" },
        { sig: "TEventSubscription&lt;Arguments...&gt; SubscribeOwned(callback, user)", ret: "所有購読", desc: "返り値の破棄時に自動解除する購読を追加する。" },
        { sig: "bool Unsubscribe(FTypedEventHandle handle)", ret: "解除できたか", desc: "同じイベント個体と世代に一致する購読だけを解除する。" },
        { sig: "void Publish(Arguments... values)", desc: "優先度の高い順、同値なら登録枠順で、配信開始時点の購読へ同期通知する。" },
        { sig: "void Clear() / u32 SubscriptionCount() const", desc: "全購読の解除と、現在有効な購読数の取得を行う。" }
      ]
    },
    {
      name: "FTypedEventHandle",
      kind: "構造体", header: "event/TypedEventHandle.h",
      summary: "型付きイベント個体、購読枠、世代を一組で識別する値。別イベントや解除済み世代への誤操作を防ぐ。",
      when: "<code>TEvent</code>または<code>TMulticastDelegate</code>の購読を後で明示解除する時。",
      members: [
        { sig: "u64 event_id / u32 slot_index / u32 generation", desc: "イベント個体、購読枠、再利用世代を識別する値。" },
        { sig: "bool IsValid() const", ret: "有効値か", desc: "イベント番号と世代番号がともに0以外なら true。" }
      ]
    },
    {
      name: "TEventSubscription&lt;Arguments...&gt;",
      kind: "クラステンプレート", header: "event/TypedEventSubscription.h",
      summary: "破棄時に購読を自動解除するムーブ専用値。イベント本体より長く生存しても弱参照が失効するため安全に終了する。",
      when: "スコープや所有オブジェクトの寿命と購読期間を一致させたい時。",
      members: [
        { sig: "bool IsValid() const", ret: "購読中か", desc: "イベントと同じ世代の購読が残っているかを返す。" },
        { sig: "bool Reset()", ret: "解除できたか", desc: "購読を解除して保持状態を空にする。" },
        { sig: "FTypedEventHandle Handle() const", ret: "購読ハンドル", desc: "現在保持している世代付きハンドルを返す。" }
      ]
    },
    {
      name: "TMulticastDelegate&lt;void(Arguments...)&gt;",
      kind: "クラステンプレート", header: "event/MulticastDelegate.h",
      summary: "<code>TEvent</code>を使い、静的関数やメンバー関数を複数登録して型付き引数を一斉通知する。",
      when: "関数を直接登録するデリゲート形式で、複数の受信先へ同じ値を通知したい時。",
      members: [
        { sig: "Add / AddStatic / AddRaw", ret: "購読ハンドル", desc: "関数ポインター、静的関数、またはメンバー関数を追加する。" },
        { sig: "AddOnce / AddWithPriority / AddOwnedRaw", ret: "購読ハンドルまたは所有購読", desc: "一回、優先度付き、自動解除の条件で処理を追加する。" },
        { sig: "bool Remove(FTypedEventHandle handle)", ret: "解除できたか", desc: "指定した世代の処理を解除する。" },
        { sig: "void Broadcast(Arguments... arguments)", desc: "登録済みの処理へ同期通知する。" },
        { sig: "void Clear() / u32 Count() const / bool IsBound() const", desc: "全解除、件数、一件以上登録済みかを取得する。" }
      ]
    },
    {
      name: "FSimpleMulticastDelegate",
      kind: "型エイリアス", header: "event/SimpleMulticastDelegate.h",
      summary: "引数なしの複数処理を登録できる <code>TMulticastDelegate&lt;void()&gt;</code>。",
      when: "引数のない変更通知や完了通知を複数の受信先へ送る時。"
    },
    {
      name: "CMessageBroker",
      kind: "クラス", header: "event/MessageBroker.h",
      summary: "<b>型ごと</b>にイベントを配る<t>pub/sub</t>(出版/購読)バス。イベント型 <code>E</code> を購読しておくと、誰かが <code>Publish&lt;E&gt;()</code> した瞬間に登録した関数が<b>同期で</b>(その場で)呼ばれます。配信中の追加は次回から有効になり、配信中の解除と全解除も安全に処理します。",
      when: "ゲーム内で「ダメージが入った」「アイテムを拾った」等の<b>出来事</b>を、それを気にする複数の場所へ一斉に知らせたい時。1 スレッド内でのみ使う。",
      members: [
        { sig: "template<typename E> FSubscriptionHandle Subscribe(MessageCallback cb, void* user)", ret: "購読ハンドル", desc: "<code>E</code> 型イベントを購読する。配信中に追加した購読は、空き枠の有無にかかわらず次回の配信から呼ばれる。関数が空、型数が上限外、全解除中、世代番号の使い切り後、または保持領域を確保できない場合は無効なハンドルを返す。", when: "そのイベントに反応したい受信側で。返ったハンドルは解除に使うので保持する。" },
        { sig: "template<typename E, auto Callback> FSubscriptionHandle SubscribeTyped(void* user)", ret: "購読ハンドル", desc: "<code>void(const E&amp;, void*) noexcept</code>として呼べる関数をコンパイル時に検証し、型消去された既存配信経路へ登録する。" },
        { sig: "template<typename E> void Publish(const E& payload)", desc: "<code>E</code> 型イベントを発行し、配信開始時点の購読者を<b>その場で順に</b>呼ぶ(同期配信)。配信中に全解除された場合は残りを呼ばない。", when: "出来事が起きた瞬間に全員へ知らせたい時。" },
        { sig: "bool Unsubscribe(FSubscriptionHandle h)", ret: "解除できたか", desc: "購読を解除する。<b>配信(Publish)中でも直ちに無効</b>になり、まだ呼ばれていない購読ならその回から呼ばれない。購読枠の再利用だけを最外側の配信終了まで遅らせる。", when: "受信側が不要になった/破棄される時。必ず解除する。" },
        { sig: "void Clear()", desc: "全購読を直ちに無効化する。配信中は残りの処理を止め、新しい購読を拒否し、最外側の配信終了時に保持領域を解放する。" },
        { sig: "u32 SubscriberCount(FEventTypeId channel) const", ret: "購読者数", desc: "あるチャンネル(イベント型)の現在の購読者数。主にデバッグ用。" },
        { sig: "using FMessageBroker = CMessageBroker", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CMessageBroker</code> を使う。" }
      ]
    },
    {
      name: "FSubscriptionHandle",
      kind: "構造体", header: "event/SubscriptionHandle.h",
      summary: "<code>CMessageBroker::Subscribe</code> が返す<b>購読の控え</b>。これを <code>Unsubscribe</code> に渡して解除する。<t>世代</t>付きなので、解除済みの古いハンドルで誤って別の購読を消すことはありません。",
      when: "購読を後で解除するために保持しておく値。",
      members: [
        { sig: "bool IsValid() const", ret: "有効か", desc: "通路が範囲内で、購読番号と世代番号がともに0以外なら true。" },
        { sig: "bool operator==(const FSubscriptionHandle& o) const", desc: "チャンネル・id・世代がすべて一致するか。" },
        { sig: "FEventTypeId channel / u32 id / u32 generation", desc: "どのチャンネルの何番目の購読か(+再利用を見分ける<t>世代</t>)。通常は直接触らない。" },
        { sig: "kInvalidSubscription", desc: "無効な購読を表す定数(<code>event/SubscriptionHandle.h</code>)。未購読の控えや「まだ Subscribe していない」状態の初期値に使う。" }
      ]
    },
    {
      name: "MessageCallback",
      kind: "型エイリアス (関数ポインタ)", header: "event/MessageBroker.h",
      summary: "<code>CMessageBroker</code> の購読コールバックの型。<code>void (*)(const void* payload, void* user)</code>。<code>payload</code> は発行されたイベントの中身、<code>user</code> は登録時に渡した任意ポインタ。STL 非依存方針のため<t>ラムダ</t>のキャプチャではなく <code>user</code> で状態を持ち回ります。",
      when: "<code>Subscribe</code> に渡す受信関数を書く時の型。"
    },
    {
      name: "GetEventTypeId&lt;E&gt;()",
      kind: "関数テンプレート", header: "event/MessageBroker.h",
      summary: "イベント型 <code>E</code> に対して<b>一意な番号</b>(<code>FEventTypeId</code>)を割り当てて返す。同じ型なら常に同じ番号になり、<code>CMessageBroker</code> が型ごとのチャンネルを区別するのに使います。",
      when: "通常は <code>Subscribe</code>/<code>Publish</code> が内部で呼ぶので、直接使うことは稀。チャンネル番号が欲しい時だけ。"
    },
    {
      name: "FEventTypeId",
      kind: "型エイリアス", header: "event/EventTypeId.h",
      summary: "<code>CMessageBroker</code> がメッセージ型ごとの通路を識別する番号。",
      when: "購読数の確認など、型から得た通路番号を明示的に保持する時。",
      members: [
        { sig: "FEventTypeId = u32", desc: "通路番号の実体は <code>u32</code>。" },
        { sig: "using EventTypeId = FEventTypeId", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>FEventTypeId</code> を使う。" },
        { sig: "kMaxEventTypes = 256", desc: "同時に扱えるメッセージ型の上限。" },
        { sig: "bool IsValidEventTypeId(FEventTypeId channel)", ret: "範囲内か", desc: "通路番号が256未満ならtrue。仲介器は範囲外の通路を作成しない。" }
      ]
    },
    {
      name: "TMessagePipe&lt;T&gt;",
      kind: "クラステンプレート", header: "event/MessagePipe.h",
      summary: "<b>スレッドをまたいで</b>値を渡すための <t>MPMC</t> キュー(複数の生産者/複数の消費者)。生産側が <code>Push</code> で値を積み、消費側が別<t>スレッド</t>で <code>TryPop</code>/<code>Pop</code> で取り出します。内部は <t>ミューテックス</t>+<t>条件変数</t>でスレッド安全。",
      when: "ワーカースレッドの結果をメインスレッドで受け取る等、<b>違うスレッド間</b>でイベントや値を受け渡したい時。同一スレッド内の即時配信は <t>CMessageBroker</t> を使う。",
      members: [
        { sig: "bool Push(T value)", ret: "積めたか", desc: "値を末尾に積む。<code>Close()</code> 済みなら false。待っている消費者が居れば 1 人起こす。", when: "生産側スレッドからイベント/結果を送る時。" },
        { sig: "bool TryPop(T& out)", ret: "取れたか", desc: "<b>待たずに</b>1 件取り出す。空なら即 false。", when: "毎フレームのループで「来ている分だけ」処理する時。" },
        { sig: "bool Pop(T& out)", ret: "取れたか", desc: "値が来るまで<b>ブロックして待つ</b>。<code>Close()</code> されて空になったら false で抜ける。", when: "受信専用スレッドで、来るまで眠って待ちたい時。" },
        { sig: "void Close()", desc: "閉じる。<code>Pop</code> で待っている全スレッドを false で起こして解放する。", when: "終了時。待ち中の <code>Pop</code> が居るなら破棄前に必ず呼ぶ。" },
        { sig: "bool IsClosed() const", ret: "閉じているか", desc: "すでに <code>Close()</code> されていれば true。" },
        { sig: "usize Size() const", ret: "件数", desc: "現在キューに溜まっている要素数(瞬間値)。" }
      ]
    },
    {
      name: "CTimerManager",
      kind: "クラス", header: "event/TimerManager.h",
      summary: "<b>時間差</b>で関数を呼ぶタイマー集。「N 秒後に 1 回」や「N 秒ごとに繰り返し」を登録し、毎フレーム <code>Tick(dt)</code> を回すと条件を満たしたタイマが発火します。実時間ではなく<b>フレームの経過時間</b>で進むので、ポーズや早送りにも自然に追従します。",
      when: "クールダウン明け、遅延スポーン、定期回復、点滅の切り替え等、「少し後で/定期的に」何かを実行したい時。",
      members: [
        { sig: "FTimerHandle SetTimeout(f32 delay_seconds, TimerCallback cb, void* user)", ret: "タイマーハンドル", desc: "<code>delay_seconds</code> 秒後に <code>cb(user)</code> を<b>1 回だけ</b>呼ぶ。世代番号の使い切り後は無効なハンドルを返す。", when: "遅延実行・ワンショットの予約に。" },
        { sig: "FTimerHandle SetTimeout(f32 delay_seconds, FSimpleDelegate delegate)", ret: "タイマーハンドル", desc: "関数と任意データをまとめたデリゲートを、指定秒数後に1回呼ぶ。" },
        { sig: "FTimerHandle SetInterval(f32 period_seconds, TimerCallback cb, void* user)", ret: "タイマーハンドル", desc: "<code>period_seconds</code> 秒経つごとに<b>繰り返し</b> <code>cb(user)</code> を呼ぶ(発火後に自動で再カウント)。世代番号の使い切り後は無効なハンドルを返す。", when: "定期処理(秒間回復、定期スポーン等)に。"},
        { sig: "FTimerHandle SetInterval(f32 period_seconds, FSimpleDelegate delegate)", ret: "タイマーハンドル", desc: "関数と任意データをまとめたデリゲートを、指定周期で繰り返し呼ぶ。" },
        { sig: "template&lt;ETimerSchedulePolicy Policy, auto Callback, typename User&gt; FTimerHandle Schedule(f32 seconds, User* user)", ret: "タイマーハンドル", desc: "単発または周期方針と型付きコールバックをコンパイル時に検証して登録する。" },
        { sig: "bool Cancel(FTimerHandle h)", ret: "止められたか", desc: "指定タイマをキャンセルする。すでに発火/解放済みなら false。", when: "周期タイマを止める/予約を取り消す時。" },
        { sig: "bool IsActive(FTimerHandle h) const", ret: "登録中か", desc: "番号と世代が一致するタイマーが現在も登録中なら true。" },
        { sig: "void CancelAll() / void Clear()", desc: "全タイマーを解除する。呼出し中は外側の更新完了時に保持領域を解放する。" },
        { sig: "void Tick(f32 dt)", desc: "毎フレーム呼ぶ。<code>dt</code>(前フレームからの秒数)だけ全タイマを進める。処理からの再入更新は無視し、更新中の新規登録は次回から進める。", when: "ゲームループの更新部分で必ず 1 回呼ぶ。" },
        { sig: "u32 ActiveCount() const", ret: "稼働中の数", desc: "現在アクティブなタイマ数。主にデバッグ用。" },
        { sig: "FTimerDiagnostics Diagnostics() const / void ResetDiagnostics()", ret: "走査診断値", desc: "直近更新のactive走査量と累積cancel probeを取得し、必要なら診断値だけを0へ戻す。" },
        { sig: "using FTimerManager = CTimerManager", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CTimerManager</code> を使う。" }
      ]
    },
    {
      name: "FTimerHandle",
      kind: "構造体", header: "event/TimerHandle.h",
      summary: "<code>CTimerManager</code> が返す<b>タイマーの控え</b>。<code>Cancel</code> に渡して止める。<t>世代</t>付きなので、ID が再利用されても古いハンドルで別タイマを誤ってキャンセルすることはありません。",
      when: "登録したタイマを後で止めたい(特に周期タイマ)時に保持しておく値。",
      members: [
        { sig: "bool IsValid() const", ret: "有効か", desc: "<code>id != 0 &amp;&amp; generation != 0</code> なら true。" },
        { sig: "bool operator==(const FTimerHandle& o) const", desc: "id と<t>世代</t>が一致するか。" },
        { sig: "u32 id / u32 generation", desc: "タイマの番号(1 始まり)と再利用を見分ける<t>世代</t>。通常は直接触らない。" },
        { sig: "kInvalidTimer", desc: "無効なタイマを表す定数(<code>event/TimerHandle.h</code>)。未設定の控えや「まだ予約していない」状態の初期値に使う。" }
      ]
    },
    {
      name: "TimerCallback",
      kind: "型エイリアス (関数ポインタ)", header: "event/TimerManager.h",
      summary: "<code>CTimerManager</code> のタイマが発火した時に呼ばれる関数の型。<code>void (*)(void* user)</code>。<code>user</code> は登録時に渡した任意ポインタで、ここに自分の状態を入れて持ち回ります(<t>スレッドプール</t>のタスク関数と同じ流儀)。",
      when: "<code>SetTimeout</code>/<code>SetInterval</code> に渡す関数を書く時の型。"
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "pub/sub": "出版(publish)/購読(subscribe)型の通信。発信側はイベントを発行するだけ、受信側は型を購読するだけで、互いを直接知らずにつながる方式。",
  "CMessageBroker": "型ごとにイベントを同期配信する<t>pub/sub</t>バス。1 スレッド内で使う。",
  "MPMC": "Multi-Producer Multi-Consumer。複数のスレッドが同時に積み(producer)、複数のスレッドが同時に取り出す(consumer)キュー。",
  "条件変数": "あるスレッドを「ある条件が満たされるまで」眠らせ、満たした側が起こす同期の仕組み(condition variable)。",
  "世代": "再利用される番号(ID)に付ける通し番号。古いハンドルと新しい中身を取り違えないための目印。"
});
