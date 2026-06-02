/* ACS リファレンス — event モジュール。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > は &lt; &gt;。 */
ACS_REF.modules.push({
  id: "event",
  order: 22,
  title: "event — イベント / シグナル",
  blurb: "「○○が起きた」を発信する側と受け取る側を<b>疎結合</b>につなぐ仕組み。同じスレッド内で即配信する<t>pub/sub</t>、スレッドをまたいで値を受け渡すキュー、時間差で関数を呼ぶタイマーが揃っています。",
  types: [
    {
      name: "MessageBroker",
      kind: "クラス", header: "event/MessageBroker.h",
      summary: "<b>型ごと</b>にイベントを配る<t>pub/sub</t>(出版/購読)バス。イベント型 <code>E</code> を購読しておくと、誰かが <code>Publish&lt;E&gt;()</code> した瞬間に登録した関数が<b>同期で</b>(その場で)呼ばれます。発信側と受信側が互いを知らなくてよいのが利点。",
      when: "ゲーム内で「ダメージが入った」「アイテムを拾った」等の<b>出来事</b>を、それを気にする複数の場所へ一斉に知らせたい時。1 スレッド内でのみ使う。",
      sample: "struct DamageEvent { EntityId target; f32 amount; };\n\nstatic void OnDamage(const void* payload, void* user) {\n    const auto&amp; e = *static_cast&lt;const DamageEvent*&gt;(payload);\n    // ... user は登録時に渡した自分の状態 ...\n}\n\nMessageBroker bus;\nauto h = bus.Subscribe&lt;DamageEvent&gt;(&amp;OnDamage, &amp;state);\nbus.Publish&lt;DamageEvent&gt;(DamageEvent{ enemy, 25.0f }); // ここで OnDamage が呼ばれる\nbus.Unsubscribe(h);",
      members: [
        { sig: "template<typename E> SubscriptionHandle Subscribe(MessageCallback cb, void* user)", ret: "購読ハンドル", desc: "<code>E</code> 型イベントを購読する。配信時に <code>cb(payload, user)</code> が呼ばれる。<code>user</code> は自分の状態を持ち回るための任意ポインタ。", when: "そのイベントに反応したい受信側で。返ったハンドルは解除に使うので保持する。" },
        { sig: "template<typename E> void Publish(const E& payload)", desc: "<code>E</code> 型イベントを発行し、購読者全員を<b>その場で順に</b>呼ぶ(同期配信)。", sample: "bus.Publish&lt;DamageEvent&gt;(DamageEvent{ enemy, 10.0f });", when: "出来事が起きた瞬間に全員へ知らせたい時。" },
        { sig: "bool Unsubscribe(SubscriptionHandle h)", ret: "解除できたか", desc: "購読を解除する。<b>配信(Publish)中に呼んでも安全</b>(予約して遅延適用される)。", when: "受信側が不要になった/破棄される時。必ず解除する。" },
        { sig: "u32 SubscriberCount(EventTypeId channel) const", ret: "購読者数", desc: "あるチャンネル(イベント型)の現在の購読者数。主にデバッグ用。" }
      ]
    },
    {
      name: "SubscriptionHandle",
      kind: "構造体", header: "event/MessageBroker.h",
      summary: "<code>MessageBroker::Subscribe</code> が返す<b>購読の控え</b>。これを <code>Unsubscribe</code> に渡して解除する。<t>世代</t>付きなので、解除済みの古いハンドルで誤って別の購読を消すことはありません。",
      when: "購読を後で解除するために保持しておく値。",
      sample: "SubscriptionHandle h = bus.Subscribe&lt;DamageEvent&gt;(&amp;OnDamage, &amp;state);\nif (h.IsValid()) {\n    // ... 必要なくなったら ...\n    bus.Unsubscribe(h);\n}",
      members: [
        { sig: "bool IsValid() const", ret: "有効か", desc: "購読が有効なら true(<code>id != 0</code>)。" },
        { sig: "bool operator==(const SubscriptionHandle& o) const", desc: "チャンネル・id・世代がすべて一致するか。" },
        { sig: "EventTypeId channel / u32 id / u32 generation", desc: "どのチャンネルの何番目の購読か(+再利用を見分ける<t>世代</t>)。通常は直接触らない。" }
      ]
    },
    {
      name: "MessageCallback",
      kind: "型エイリアス (関数ポインタ)", header: "event/MessageBroker.h",
      summary: "<code>MessageBroker</code> の購読コールバックの型。<code>void (*)(const void* payload, void* user)</code>。<code>payload</code> は発行されたイベントの中身、<code>user</code> は登録時に渡した任意ポインタ。STL 非依存方針のため<t>ラムダ</t>のキャプチャではなく <code>user</code> で状態を持ち回ります。",
      when: "<code>Subscribe</code> に渡す受信関数を書く時の型。",
      sample: "static void OnPickup(const void* payload, void* user) {\n    const auto&amp; e = *static_cast&lt;const PickupEvent*&gt;(payload);\n    auto* self = static_cast&lt;MyState*&gt;(user);\n    self-&gt;score += e.value;\n}"
    },
    {
      name: "GetEventTypeId&lt;E&gt;()",
      kind: "関数テンプレート", header: "event/MessageBroker.h",
      summary: "イベント型 <code>E</code> に対して<b>一意な番号</b>(<code>EventTypeId</code>)を割り当てて返す。同じ型なら常に同じ番号になり、<code>MessageBroker</code> が型ごとのチャンネルを区別するのに使います。",
      when: "通常は <code>Subscribe</code>/<code>Publish</code> が内部で呼ぶので、直接使うことは稀。チャンネル番号が欲しい時だけ。",
      sample: "EventTypeId id = GetEventTypeId&lt;DamageEvent&gt;();\nu32 n = bus.SubscriberCount(id);   // この型の購読者数",
      members: [
        { sig: "EventTypeId = u32", desc: "イベント型 ID の実体は <code>u32</code>。最大 <code>kMaxEventTypes</code>(256)種類。" }
      ]
    },
    {
      name: "MessagePipe&lt;T&gt;",
      kind: "クラステンプレート", header: "event/MessagePipe.h",
      summary: "<b>スレッドをまたいで</b>値を渡すための <t>MPMC</t> キュー(複数の生産者/複数の消費者)。生産側が <code>Push</code> で値を積み、消費側が別<t>スレッド</t>で <code>TryPop</code>/<code>Pop</code> で取り出します。内部は <t>ミューテックス</t>+<t>条件変数</t>でスレッド安全。",
      when: "ワーカースレッドの結果をメインスレッドで受け取る等、<b>違うスレッド間</b>でイベントや値を受け渡したい時。同一スレッド内の即時配信は <t>MessageBroker</t> を使う。",
      sample: "MessagePipe&lt;DamageEvent&gt; pipe;\n\n// 生産スレッド:\npipe.Push(DamageEvent{ enemy, 25.0f });\n\n// 消費スレッド (毎フレーム):\nDamageEvent e;\nwhile (pipe.TryPop(e)) {\n    ApplyDamage(e);   // 溜まっている分を全部処理\n}",
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
      name: "FTimerManager",
      kind: "クラス", header: "event/Timer.h",
      summary: "<b>時間差</b>で関数を呼ぶタイマー集。「N 秒後に 1 回」や「N 秒ごとに繰り返し」を登録し、毎フレーム <code>Tick(dt)</code> を回すと条件を満たしたタイマが発火します。実時間ではなく<b>フレームの経過時間</b>で進むので、ポーズや早送りにも自然に追従します。",
      when: "クールダウン明け、遅延スポーン、定期回復、点滅の切り替え等、「少し後で/定期的に」何かを実行したい時。",
      sample: "static void OnFire(void* user) {\n    auto* s = static_cast&lt;MyState*&gt;(user);\n    s-&gt;Spawn();\n}\n\nFTimerManager timers;\nauto h = timers.SetTimeout(2.5f, &amp;OnFire, &amp;st);  // 2.5 秒後に 1 回\n\n// フレームループで毎回:\ntimers.Tick(dt);",
      members: [
        { sig: "FTimerHandle SetTimeout(f32 delay_seconds, TimerCallback cb, void* user)", ret: "タイマーハンドル", desc: "<code>delay_seconds</code> 秒後に <code>cb(user)</code> を<b>1 回だけ</b>呼ぶ。", when: "遅延実行・ワンショットの予約に。" },
        { sig: "FTimerHandle SetInterval(f32 period_seconds, TimerCallback cb, void* user)", ret: "タイマーハンドル", desc: "<code>period_seconds</code> 秒経つごとに<b>繰り返し</b> <code>cb(user)</code> を呼ぶ(発火後に自動で再カウント)。", when: "定期処理(秒間回復、定期スポーン等)に。", sample: "auto h = timers.SetInterval(1.0f, &amp;OnTick, &amp;st); // 毎秒" },
        { sig: "bool Cancel(FTimerHandle h)", ret: "止められたか", desc: "指定タイマをキャンセルする。すでに発火/解放済みなら false。", when: "周期タイマを止める/予約を取り消す時。" },
        { sig: "void Tick(f32 dt)", desc: "毎フレーム呼ぶ。<code>dt</code>(前フレームからの秒数)だけ全タイマを進め、発火条件を満たしたものを呼ぶ。<b>コールバック内で新規タイマ追加や Cancel をしても安全</b>。", when: "ゲームループの更新部分で必ず 1 回呼ぶ。" },
        { sig: "u32 ActiveCount() const", ret: "稼働中の数", desc: "現在アクティブなタイマ数。主にデバッグ用。" }
      ]
    },
    {
      name: "FTimerHandle",
      kind: "構造体", header: "event/Timer.h",
      summary: "<code>FTimerManager</code> が返す<b>タイマーの控え</b>。<code>Cancel</code> に渡して止める。<t>世代</t>付きなので、ID が再利用されても古いハンドルで別タイマを誤ってキャンセルすることはありません。",
      when: "登録したタイマを後で止めたい(特に周期タイマ)時に保持しておく値。",
      sample: "FTimerHandle h = timers.SetInterval(0.5f, &amp;OnBlink, &amp;st);\n// ... 点滅をやめる ...\ntimers.Cancel(h);",
      members: [
        { sig: "bool IsValid() const", ret: "有効か", desc: "<code>id != 0</code> なら true。" },
        { sig: "bool operator==(const FTimerHandle& o) const", desc: "id と<t>世代</t>が一致するか。" },
        { sig: "u32 id / u32 generation", desc: "タイマの番号(1 始まり)と再利用を見分ける<t>世代</t>。通常は直接触らない。" }
      ]
    },
    {
      name: "TimerCallback",
      kind: "型エイリアス (関数ポインタ)", header: "event/Timer.h",
      summary: "<code>FTimerManager</code> のタイマが発火した時に呼ばれる関数の型。<code>void (*)(void* user)</code>。<code>user</code> は登録時に渡した任意ポインタで、ここに自分の状態を入れて持ち回ります(<t>スレッドプール</t>のタスク関数と同じ流儀)。",
      when: "<code>SetTimeout</code>/<code>SetInterval</code> に渡す関数を書く時の型。",
      sample: "static void OnCooldownEnd(void* user) {\n    auto* s = static_cast&lt;Player*&gt;(user);\n    s-&gt;canAttack = true;\n}"
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "pub/sub": "出版(publish)/購読(subscribe)型の通信。発信側はイベントを発行するだけ、受信側は型を購読するだけで、互いを直接知らずにつながる方式。",
  "MessageBroker": "型ごとにイベントを同期配信する<t>pub/sub</t>バス。1 スレッド内で使う。",
  "MPMC": "Multi-Producer Multi-Consumer。複数のスレッドが同時に積み(producer)、複数のスレッドが同時に取り出す(consumer)キュー。",
  "条件変数": "あるスレッドを「ある条件が満たされるまで」眠らせ、満たした側が起こす同期の仕組み(condition variable)。",
  "世代": "再利用される番号(ID)に付ける通し番号。古いハンドルと新しい中身を取り違えないための目印。"
});
