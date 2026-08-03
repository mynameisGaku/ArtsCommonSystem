/* ACS リファレンス — threading モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "threading",
  order: 15,
  title: "threading — スレッド / ジョブ / アトミック / 排他",
  blurb: "複数の処理を同時に走らせ(<t>スレッド</t>・<t>ジョブ</t>)、その時にデータが壊れないよう守る(<t>アトミック</t>・<t>排他</t>)ための道具一式。STL の <code>&lt;thread&gt;</code> / <code>&lt;atomic&gt;</code> / <code>&lt;mutex&gt;</code> を ACS 流に置き換えたものです。",
  types: [
    {
      name: "FJobGraphDiagnostics",
      kind: "構造体", header: "threading/JobGraphDiagnostics.h",
      summary: "topology 構築、submit scan、reset visit、inline / heap job 数を保持する診断 snapshot。",
      when: "同じ JobGraph の再利用と callable 保持経路を検証する時。"
    },
    {
      name: "FThreadPoolDiagnostics",
      kind: "構造体", header: "threading/ThreadPoolDiagnostics.h",
      summary: "投入 drain、通知、競合、queued work、callable 保持経路を集計する診断 snapshot。",
      when: "batch submit が余分な lock、wake、heap 確保を発生させていないか確認する時。"
    },
    {
      name: "TAtomic&lt;T&gt;",
      kind: "クラステンプレート", header: "threading/Atomic.h",
      summary: "<b>分割されない</b>読み書き・更新ができる値(std::atomic 相当)。複数<t>スレッド</t>が同時に触っても途中状態が見えず壊れない。整数・<t>列挙(enum)</t>・<t>ポインタ</t>(1/2/4/8 バイト)に使える。コピー不可。",
      when: "カウンタ・フラグ・状態など『1 つの小さな値』を<t>ロック</t>なしで複数スレッドから安全に共有したい時。複雑なデータには <t>FMutex</t> を使う。",
      sample: "TAtomic&lt;u32&gt; counter{0};\ncounter.FetchAdd(1);              // どのスレッドから呼んでも正しく +1\nu32 now = counter.Load();        // 現在値を読む\nTAtomic&lt;bool&gt; ready{false};\nready.Store(true, EMemoryOrder::Release); // 公開",
      members: [
        { sig: "T Load(EMemoryOrder o = SeqCst) const", ret: "現在値", desc: "値を読む。<code>Relaxed</code> 指定で最速(順序保証なし)。", when: "他スレッドが書いた値を読み取る時。" },
        { sig: "void Store(T v, EMemoryOrder o = SeqCst)", desc: "値を書き込む。<code>Release</code> 指定でそれ以前の書き込みを公開できる。" },
        { sig: "T Exchange(T v)", ret: "古い値", desc: "新しい値を書きつつ、書く前の値を返す(XCHG 相当)。" },
        { sig: "bool CompareExchange(T& expected, T desired)", ret: "成功したか", desc: "現在値が <code>expected</code> と一致すれば <code>desired</code> に書き換え true。失敗時は <code>expected</code> に実際の値が入り false。<t>CAS</t>。", when: "『今が X なら Y にする』というロックフリーな更新ループを書く時。", sample: "u32 cur = a.Load();\nwhile (!a.CompareExchange(cur, cur + 1)) { /* cur は実値に更新済 */ }" },
        { sig: "T FetchAdd(T v) / FetchSub(T v)", ret: "演算前の値", desc: "加算/減算しつつ、演算する前の値を返す。<b>32/64bit 型用</b>(<code>FetchOr/And/Exchange/CompareExchange</code> は 1/2/4/8 byte 全対応)。順序引数は取らない。" },
        { sig: "T FetchOr(T v) / FetchAnd(T v)", ret: "演算前の値", desc: "ビット OR / AND しつつ演算前の値を返す。フラグ集合の操作に。" },
        { sig: "T operator++() / operator--()", desc: "アトミックなインクリメント/デクリメント。前置は演算後、後置(<code>++(int)</code>)は演算前の値を返す。" }
      ]
    },
    {
      name: "EMemoryOrder",
      kind: "列挙(enum)", header: "threading/MemoryOrder.h",
      summary: "<t>アトミック</t>操作の<b>順序保証の強さ</b>を選ぶタグ(std::memory_order 相当)。強いほど安全だが遅い。",
      when: "<code>TAtomic</code> の各操作に渡す。迷ったら既定の <code>SeqCst</code> でよい。性能を詰める段階で緩める。",
      sample: "a.Store(1, EMemoryOrder::Release);   // これ以前の書き込みを公開\nu32 v = a.Load(EMemoryOrder::Acquire); // 公開された書き込みを取り込む\nb.FetchAdd(1);                         // RMW(FetchAdd 等)は順序引数を取らない",
      members: [
        { sig: "Relaxed", desc: "順序保証なし。値そのものはアトミックだが、周りの読み書きの順序は守られない。最速。" },
        { sig: "Acquire", desc: "このロード以降の読み書きが上に動かない。<code>Release</code> された値を取り込む側。" },
        { sig: "Release", desc: "このストア以前の読み書きが下に動かない。書いた内容を他スレッドへ公開する側。" },
        { sig: "AcqRel", desc: "読み書き両方を行う <t>RMW</t> 操作向け(Acquire + Release)の意味タグ。ただし <code>TAtomic</code> の RMW(<code>FetchAdd</code>/<code>Exchange</code>/<code>CompareExchange</code> 等)は<b>順序引数を取らない</b>(x64 では常に完全バリア)ため、この値を渡す先は <code>Load</code>/<code>Store</code> のみ。<code>Load</code> は <code>Relaxed</code> 以外なら Acquire、<code>Store</code> は <code>Relaxed</code> 以外なら Release として扱われる(SeqCst も同じ扱いで、<code>Load</code>/<code>Store</code> に完全バリアは入らない)。" },
        { sig: "SeqCst", desc: "全スレッドで唯一の順序が見える最強の保証。既定値。" }
      ]
    },
    {
      name: "CompilerBarrier / HardwareFence / SpinHint",
      kind: "関数", header: "threading/MemoryOrder.h",
      summary: "順序やスピン待ちを細かく制御する低レベル関数群。<b>CompilerBarrier</b>=コンパイラの命令並び替えだけ禁止(CPU 命令は出さない)。<b>HardwareFence</b>=CPU レベルの完全<t>フェンス</t>。<b>SpinHint</b>=忙しく待つループで CPU を一瞬譲るヒント命令。",
      when: "ロックフリーな自作データ構造の最終調整など、めったに使わない上級用途。普段は <code>TAtomic</code> + <code>EMemoryOrder</code> で足りる。",
      sample: "while (!flag.Load(EMemoryOrder::Acquire)) {\n    SpinHint();   // ハイパースレッディングの相方に CPU を譲る\n}",
      members: [
        { sig: "void CompilerBarrier()", desc: "コンパイラの並び替えのみを禁止する。" },
        { sig: "void HardwareFence()", desc: "CPU + コンパイラ両方の<t>フェンス</t>(x64 では mfence)。" },
        { sig: "void SpinHint()", desc: "busy-wait ループ内に 1 回入れて消費電力と発熱を抑える(x86 PAUSE)。" }
      ]
    },
    {
      name: "FMutex",
      kind: "クラス", header: "threading/Mutex.h",
      summary: "一度に <b>1 スレッドだけ</b>が通れる関所(std::mutex 相当)。守りたいデータを触る前に <code>Lock</code> し、終わったら <code>Unlock</code> する。Win32 SRWLOCK ベースで軽量。コピー不可。",
      when: "複数の値をまとめて更新する等、<code>TAtomic</code> 1 個では守れない『データのかたまり』を保護したい時。",
      sample: "FMutex m;\nm.Lock();\n// ここは同時に1スレッドだけ\nlist.Add(x);\nm.Unlock();\n// 実際は手で Unlock せず FScopedLock を使うのが安全",
      members: [
        { sig: "void Lock()", desc: "排他<t>ロック</t>を取る。他が持っていれば取れるまで待つ(ブロッキング)。" },
        { sig: "bool TryLock()", ret: "取れたか", desc: "取れたら true、取れなければ待たずに即 false を返す。", when: "ロックが空いていれば処理し、塞がっていれば後回しにしたい時。" },
        { sig: "void Unlock()", desc: "ロックを手放す。<code>Lock</code> と必ず対で呼ぶ。" }
      ]
    },
    {
      name: "FRwLock",
      kind: "クラス", header: "threading/RwLock.h",
      summary: "<b>読み手は同時に何人でも</b>、<b>書き手は 1 人だけ</b>通す<t>ロック</t>(std::shared_mutex 相当)。読み取りが多いデータでスループットが上がる。Win32 SRWLOCK ベース。",
      when: "設定値・リソースカタログのように『たまに書き換え、ほとんど読むだけ』のデータを守りたい時。",
      sample: "FRwLock rw;\nrw.LockShared();      // 読み取り(複数同時OK)\nint v = config.value;\nrw.UnlockShared();\nrw.LockExclusive();   // 書き込み(独占)\nconfig.value = 42;\nrw.UnlockExclusive();",
      members: [
        { sig: "void LockShared() / bool TryLockShared() / void UnlockShared()", desc: "共有(読み取り)<t>ロック</t>の取得・試行・解除。複数スレッドが同時に保持できる。" },
        { sig: "void LockExclusive() / bool TryLockExclusive() / void UnlockExclusive()", desc: "排他(書き込み)ロックの取得・試行・解除。1 スレッドだけが保持でき、読み手も締め出す。" }
      ]
    },
    {
      name: "FScopedLock",
      kind: "クラス", header: "threading/ScopedLock.h",
      summary: "<t>FMutex</t> を<b>自動で</b>ロック/解除する <t>RAII</t> ガード(std::lock_guard 相当)。作った瞬間 Lock し、スコープを抜けると必ず Unlock する。",
      when: "<code>FMutex</code> を使う時は原則これ。途中で <code>return</code> しても <code>FPanic</code> しても解除し忘れがない。コピー/ムーブ不可。",
      sample: "FMutex m;\n{\n    FScopedLock lk(m);   // ここで Lock\n    list.Add(x);\n    // 早期 return しても安全\n}                        // スコープ脱出で自動 Unlock",
      members: [
        { sig: "explicit FScopedLock(FMutex&amp; m) noexcept", desc: "構築と同時に <code>m.Lock()</code> を呼び、デストラクタで <code>m.Unlock()</code> する。<code>FMutex&amp;</code> を参照で保持するため <code>m</code> はガードより長く生存させること。" }
      ]
    },
    {
      name: "FScopedSharedLock / FScopedExclusiveLock",
      kind: "クラス", header: "threading/ScopedLock.h",
      summary: "<t>FRwLock</t> 用の <t>RAII</t> ガード。<b>Shared</b>=共有(読み取り)を自動ロック/解除、<b>Exclusive</b>=排他(書き込み)を自動ロック/解除。",
      when: "<code>FRwLock</code> を使う時は手動の Lock/Unlock ではなくこれで包む。",
      sample: "FRwLock rw;\n{\n    FScopedSharedLock lk(rw);     // 読み取りで保持\n    Read(config);\n}\n{\n    FScopedExclusiveLock lk(rw);  // 書き込みで独占\n    Write(config);\n}",
      members: [
        { sig: "explicit FScopedSharedLock(FRwLock&amp; r) noexcept", desc: "構築時に <code>r.LockShared()</code>、デストラクタで <code>r.UnlockShared()</code>(読み取り、複数同時可)。" },
        { sig: "explicit FScopedExclusiveLock(FRwLock&amp; r) noexcept", desc: "構築時に <code>r.LockExclusive()</code>、デストラクタで <code>r.UnlockExclusive()</code>(書き込み、独占)。" }
      ]
    },
    {
      name: "FConditionVar",
      kind: "クラス", header: "threading/ConditionVar.h",
      summary: "『ある条件が満たされるまでスレッドを眠らせ、別スレッドが満たしたら起こす』ための<t>条件変数</t>(std::condition_variable 相当)。必ず <t>FMutex</t> と組で使う。コピー不可。",
      when: "生産者・消費者のように『キューが空なら待ち、要素が来たら起きる』待ち合わせを CPU を無駄に使わず実現したい時。",
      sample: "FMutex m; FConditionVar cv;\n// 消費者側\n{\n    FScopedLock lk(m);\n    while (queue.IsEmpty()) cv.Wait(m); // 空なら眠る\n    auto item = queue.Pop();\n}\n// 生産者側: queue.Push(x); cv.NotifyOne();",
      members: [
        { sig: "void Wait(FMutex& m)", desc: "起こされるまで眠る。呼ぶ前に <code>m</code> を Lock しておくこと。眠る間は内部で一時的に Unlock し、起きたら再 Lock してから返る。", when: "条件が未成立の間 <code>while</code> ループで囲んで待つ。" },
        { sig: "bool WaitFor(FMutex& m, u32 timeout_ms)", ret: "起こされたか", desc: "最大 <code>timeout_ms</code> ミリ秒待つ。起こされたら true、時間切れなら false。" },
        { sig: "void NotifyOne()", desc: "待機中のスレッドを 1 つだけ起こす。" },
        { sig: "void NotifyAll()", desc: "待機中の全スレッドを起こす。" }
      ]
    },
    {
      name: "FThread",
      kind: "クラス", header: "threading/Thread.h",
      summary: "新しい<t>スレッド</t>を起動して関数を別並列で走らせる <t>RAII</t> ハンドル(std::thread 相当)。例外を投げず、起動失敗は <t>Result</t> で返す。コピー不可・<t>ムーブ</t>可。",
      when: "重い処理(読み込み・計算)をメインスレッドと別に走らせたい時。多数の小タスクを捌くなら <code>FThread</code> を直接作らず <t>CThreadPool</t> を使う方が効率的。",
      sample: "void Work(void* user) { /* 別スレッドで走る */ }\nauto r = FThread::Spawn(&Work, &ctx);\nif (r.IsOk()) {\n    FThread t = static_cast&lt;FThread&amp;&amp;&gt;(r.Value());\n    t.Join();   // 終わるまで待つ\n}",
      members: [
        { sig: "static TResult<FThread> Spawn(ThreadEntry entry, void* user, const FThreadConfig& cfg = {})", ret: "スレッド or エラー", desc: "<code>entry(user)</code> を別スレッドで起動する。失敗時は Err。", when: "スレッドを 1 本立ち上げる時。" },
        { sig: "bool Joinable() const", desc: "まだ <code>Join</code> も <code>Detach</code> もしていない有効なハンドルなら true。" },
        { sig: "void Join()", desc: "スレッドが終わるまで待ち、その後ハンドルを解放する。" },
        { sig: "void Detach()", desc: "ハンドルだけ閉じてスレッドは走らせ続ける(待たない)。" },
        { sig: "FThreadId Id() const", ret: "スレッド ID", desc: "このスレッドの <code>FThreadId</code>。" }
      ]
    },
    {
      name: "ThreadEntry / FThreadConfig",
      kind: "型エイリアス / 構造体", header: "threading/Thread.h",
      summary: "<b>ThreadEntry</b>=スレッドの入口関数の型 <code>void(*)(void* user)</code>。<b>FThreadConfig</b>=スレッド起動時の設定(名前・スタックサイズ・優先度・CPU アフィニティ)。",
      when: "<code>FThread::Spawn</code> に渡す。名前を付けるとデバッガで識別しやすい。",
      sample: "FThreadConfig cfg;\ncfg.name        = L\"AssetLoader\";  // デバッガ表示名\ncfg.stack_bytes = 0;               // 0 = OS 既定\ncfg.priority    = 0;               // -2..+2\nFThread::Spawn(&Work, &ctx, cfg);",
      members: [
        { sig: "using ThreadEntry = void (*)(void* user)", desc: "スレッドで呼ばれる関数。<code>user</code> は Spawn 時に渡した任意ポインタ。" },
        { sig: "const wchar_t* name", desc: "デバッガ表示用の名前(null 可)。" },
        { sig: "u32 stack_bytes", desc: "スタックサイズ。0 で OS 既定。" },
        { sig: "i32 priority", desc: "優先度 -2..+2(THREAD_PRIORITY_* に対応)。" },
        { sig: "u64 affinity", desc: "CPU アフィニティマスク。0 でピン留めなし。" }
      ]
    },
    {
      name: "SleepMs / Yield / HardwareConcurrency",
      kind: "関数", header: "threading/Thread.h",
      summary: "現在スレッドを操作するフリー関数。<b>SleepMs</b>=指定ミリ秒眠る。<b>Yield</b>=同優先度の他スレッドに実行を譲る。<b>HardwareConcurrency</b>=論理 CPU 数を返す。",
      when: "SleepMs はポーリング間隔の調整、Yield は短い待ち、HardwareConcurrency は並列数の決定(<code>CThreadPool</code> の既定 worker 数等)に。",
      sample: "SleepMs(16);                     // ~1 フレーム眠る\nu32 cpus = HardwareConcurrency(); // 例: 8\nFThreadPool::Init(cpus);",
      members: [
        { sig: "void SleepMs(u32 ms)", desc: "指定ミリ秒スリープする。" },
        { sig: "void Yield()", desc: "同優先度の他スレッドへ CPU を譲る。" },
        { sig: "u32 HardwareConcurrency()", ret: "論理 CPU 数", desc: "この機械の論理コア数を返す。" }
      ]
    },
    {
      name: "FThreadId",
      kind: "構造体", header: "threading/ThreadId.h",
      summary: "OS のスレッド ID を <code>u32</code> 1 個で持つ軽量な値(std::thread::id 相当)。比較可能でハッシュにも使える <t>POD</t>。",
      when: "『今どのスレッドから呼ばれたか』を識別・比較・記録したい時。",
      sample: "FThreadId me = CurrentThreadId();\nif (me == g_MainThread) {\n    // メインスレッドからの呼び出し\n}",
      members: [
        { sig: "u32 raw", desc: "OS の DWORD と等価な生の ID。" },
        { sig: "bool operator==(FThreadId) / operator!=(FThreadId)", desc: "ID 同士の一致/不一致を比べる。" }
      ]
    },
    {
      name: "CurrentThreadId()",
      kind: "関数", header: "threading/ThreadId.h",
      summary: "今このコードを実行している<t>スレッド</t>の <code>FThreadId</code> を返す(GetCurrentThreadId のラッパ)。",
      when: "スレッド固有の処理分岐や、後述の <code>ACS_THREAD_AFFINITY_*</code> の検証に使われる。",
      sample: "FThreadId tid = CurrentThreadId();"
    },
    {
      name: "ACS_THREAD_AFFINITY_FIELD / _CHECK / _RESET",
      kind: "マクロ", header: "threading/ThreadAffinity.h",
      summary: "あるクラスを『決まった 1 スレッドからしか触ってはいけない』と<b>Debug ビルドで検証</b>する仕掛け。初回呼び出しのスレッドに固定し、別スレッドから呼ぶと <code>panic</code>。Release では完全に消える(0 バイト・no-op)。",
      when: "UI ビューなど『メインスレッド専用』のクラスで、誤って別スレッドから触っていないかを開発中に自動検出したい時。",
      sample: "class FMyView {\n    ACS_THREAD_AFFINITY_FIELD();      // 検査用メンバを生やす\npublic:\n    void Set(int v) {\n        ACS_THREAD_AFFINITY_CHECK();  // 初回スレッドに固定、違反で panic\n        m_v = v;\n    }\n};",
      members: [
        { sig: "ACS_THREAD_AFFINITY_FIELD()", desc: "クラス内に検査用メンバを宣言する(Debug のみ。Release で 0 バイト)。" },
        { sig: "ACS_THREAD_AFFINITY_CHECK()", desc: "最初に呼ばれたスレッドに固定し、以後そのスレッド以外から呼ばれたら <code>ACS_ASSERTF</code> で停止。" },
        { sig: "ACS_THREAD_AFFINITY_RESET()", desc: "固定をリセットする(テストで別スレッドに付け替えたい時)。" }
      ]
    },
    {
      name: "CThreadPool",
      kind: "クラス", header: "threading/ThreadPool.h",
      summary: "あらかじめ用意したワーカー<t>スレッド</t>群に小さなタスクを投げて並列実行させる<t>スレッドプール</t>。<t>ワークスチール</t>方式で、暇なワーカーが忙しいワーカーの仕事を奪い負荷を均す。全 static の単一プール。ParallelFor は 32 分割まで stack、超過分は再利用 block を使う。内部では実行制御・タスク流量・API寿命の共有値を別cache lineへ分け、投入とworker loopの干渉を抑える。",
      when: "数百〜数千の独立した小タスク(描画準備・更新・計算)を CPU 全コアで捌きたい時。<code>FThread</code> を毎回作るより遥かに軽い。依存関係があるなら <t>CJobGraph</t>。",
      sample: "CThreadPool::Init();                 // 論理CPU数でワーカー起動\nFCompletionCounter done;\nFTask t{ &MyTask, &ctx, &done };\nFThreadPool::Submit(t);              // 投入(counter は自動 Add)\nFThreadPool::Wait(done);            // 完了待ち(待機中も手伝う)\nFThreadPool::Shutdown();",
      members: [
        { sig: "static TResult<void> Init(u32 worker_count = 0)", ret: "成否", desc: "プールを起動する。<code>0</code> で論理 CPU 数を採用。二重 Init はエラー。" },
        { sig: "static void Shutdown()", desc: "全ワーカーを停止しプールを片付ける。" },
        { sig: "static u32 WorkerCount()", ret: "ワーカー数", desc: "起動中のワーカースレッド数。" },
        { sig: "static u32 CurrentWorkerIndex()", ret: "0..N-1 or kNotAWorker", desc: "呼び出しスレッドがプールのワーカーなら通し番号、そうでなければ <code>kNotAWorker</code>。" },
        { sig: "static FParallelForDiagnostics CaptureParallelForDiagnostics()", ret: "診断 snapshot", desc: "ParallelFor の stack、固定 block、heap 退避と同時使用量を返す。既存 <code>FThreadPoolDiagnostics</code> の 96 byte ABI は変えない。" },
        { sig: "static constexpr u32 kNotAWorker = 0xFFFFFFFF", desc: "<code>CurrentWorkerIndex()</code> がワーカー外スレッドで返す番兵値。" },
        { sig: "static TResult<void> Submit(const FTask& t)", ret: "成否", desc: "タスクを投入する。<code>t.counter</code> が非 null なら自動で <code>Add(1)</code> される。", when: "1 個のタスクを並列キューに積む時。" },
        { sig: "static void Wait(FCompletionCounter& counter)", desc: "<code>counter</code> が 0 になるまで待つ。待機中の呼び出しスレッドも仕事を手伝う(スティーリング参加)。" },
        { sig: "static TResult<void> ParallelFor(u32 begin, u32 end, u32 grain, body, void* user)", ret: "成否", desc: "<code>[begin, end)</code> を <code>grain</code> 個ずつに分割して並列実行し、全部終わるまで待つ。", when: "配列を範囲分割して並列処理したい時の定番。", sample: "CThreadPool::ParallelFor(0, n, 64,\n  [](u32 i, u32 w, void* u){ /* i 番目を処理 */ }, &ctx);" },
        { sig: "using FThreadPool = CThreadPool", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CThreadPool</code> を使う。" }
      ]
    },
    {
      name: "FParallelForDiagnostics",
      kind: "構造体", header: "threading/ParallelForDiagnostics.h",
      summary: "<code>CThreadPool::ParallelFor</code> が一時 context をどこへ置いたかと、固定 block の同時使用量を返す 40 byte の値 snapshot。",
      when: "分割数や同時 ParallelFor 数に対して固定 pool が足りているか、heap 退避が起きていないかを profiler や性能 test で確認する時。",
      members: [
        { sig: "u64 inline_calls", desc: "32 分割以下で stack だけを使った呼び出し数。" },
        { sig: "u64 pool_blocks", desc: "固定 free-list から取得した block の累計数。" },
        { sig: "u64 pool_blocks_in_use", desc: "現在貸し出している block 数。" },
        { sig: "u64 pool_blocks_high_water", desc: "同時貸し出し block 数の最大値。" },
        { sig: "u64 heap_blocks", desc: "固定 pool 枯渇後に OS heap へ退避した block 数。" }
      ]
    },
    {
      name: "FTask / TaskFn",
      kind: "構造体 / 型エイリアス", header: "threading/ThreadPool.h",
      summary: "<t>CThreadPool</t> に投げる 1 件の仕事。<b>TaskFn</b>=タスク本体の関数型 <code>void(*)(void* user, u32 worker_index)</code>。<b>FTask</b>=その関数・ユーザーデータ・完了通知先をまとめた <t>POD</t>。",
      when: "<code>CThreadPool::Submit</code> に渡すために組み立てる。<code>counter</code> を付けると <code>Wait</code> で完了を待てる。",
      sample: "void MyTask(void* user, u32 worker) { /* 実処理 */ }\nFCompletionCounter done;\nFTask t;\nt.fn      = &MyTask;\nt.user    = &ctx;\nt.counter = &done;   // 任意(null 可)\nFThreadPool::Submit(t);",
      members: [
        { sig: "using TaskFn = void (*)(void* user, u32 worker_index)", desc: "実行される関数。<code>worker_index</code> は実行ワーカーの番号 0..N-1。" },
        { sig: "TaskFn fn", desc: "実行する関数。" },
        { sig: "void* user", desc: "関数に渡される任意のユーザーデータ。" },
        { sig: "FCompletionCounter* counter", desc: "完了通知先(任意・null 可)。Submit 時に自動で 1 加算され、タスク終了時に減算される。" }
      ]
    },
    {
      name: "FCompletionCounter",
      kind: "クラス", header: "threading/ThreadPool.h",
      summary: "投げたタスク群が『あと何個残っているか』を数える<t>アトミック</t>カウンタ。0 になったら全完了。<code>Done</code> は 0 で<b>飽和</b>し、呼びすぎても下にあふれない(待ちがハングしない)。コピー不可。",
      when: "複数タスクをまとめて投入し、全部終わるのを <code>CThreadPool::Wait</code> で待ちたい時の合図役。",
      sample: "FCompletionCounter c;\nfor (auto& job : jobs)\n    CThreadPool::Submit(FTask{ job.fn, job.user, &c }); // 自動 Add\nFThreadPool::Wait(c);  // 全部終わるまで(自分も手伝いつつ)待つ",
      members: [
        { sig: "explicit FCompletionCounter(u32 initial)", desc: "初期残数を指定して作る(既定は 0)。" },
        { sig: "void Add(u32 n = 1)", desc: "残数を <code>n</code> 増やす(タスク投入時)。<code>Submit</code> 経由なら自動。" },
        { sig: "void Done()", desc: "残数を 1 減らす(タスク完了時)。0 では何もしない(下限飽和)。" },
        { sig: "u32 Pending() const", ret: "残数", desc: "まだ終わっていないタスクの数。" },
        { sig: "bool Finished() const", ret: "全完了か", desc: "残数が 0、つまり全タスク完了なら true。" }
      ]
    },
    {
      name: "CJobGraph",
      kind: "クラス", header: "threading/JobGraph.h",
      summary: "<b>依存関係のある</b>タスク(ジョブ)を並べて並列実行するスケジューラ。『B は A の後』のような順序を指定すると、依存が解けたジョブから自動的に <t>CThreadPool</t> 上で走り出す。Submit 冒頭で全 job の完了数を 1 回だけ予約し、依存解決ごとの加算を避ける。",
      when: "読み込み→構築→GPU アップロードのように、一部は並列・一部は順序ありの処理パイプラインを組みたい時。",
      sample: "CJobGraph g;\nauto loadA = g.Add(&LoadA, &ctx);\nauto loadB = g.Add(&LoadB, &ctx);\nauto build = g.Add(&Build, &ctx);\nbuild.DependOn(loadA);   // loadA,loadB 完了後に build\nbuild.DependOn(loadB);\ng.Submit();              // 依存0のジョブから開始\ng.Wait();                // 全完了まで待つ",
      members: [
        { sig: "FJobHandle Add(JobFn fn, void* user)", ret: "ジョブのハンドル", desc: "ジョブを 1 件追加する。<code>Submit</code> より前にだけ呼べる。", when: "グラフを組み立てる段階。" },
        { sig: "void AddDependency(FJobHandle upstream, FJobHandle downstream)", desc: "<code>upstream</code> が終わるまで <code>downstream</code> を走らせない依存を張る(<code>FJobHandle::DependOn</code> と同じ)。" },
        { sig: "TResult<void> Submit()", ret: "成否", desc: "全 job の完了数を一括予約してから、依存 0 の job を <code>CThreadPool</code> に投入する。未投入・依存待ち job も先に残数へ含むため、待機側へ一時的な 0 を公開しない。投入後はグラフ変更不可。" },
        { sig: "void Wait()", desc: "全ジョブが終わるまで待つ(待機中もスティーリングに参加)。" },
        { sig: "void Reset()", desc: "依存関係はそのままに残カウントを初期値へ戻し、同じグラフを再実行できるようにする。" },
        { sig: "u32 JobCount() const", ret: "ジョブ数", desc: "グラフに登録されたジョブの数。" },
        { sig: "FJobGraphCompletionDiagnostics CompletionDiagnostics() const", ret: "現在予約", desc: "現在 Submit 済みなら <code>{1, JobCount()}</code>、未 Submit または Reset 後なら <code>{0, 0}</code>。既存 <code>FJobGraphDiagnostics</code> の 40B ABI を変えない独立値型。" },
        { sig: "using FJobGraph = CJobGraph", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CJobGraph</code> を使う。" }
      ]
    },
    {
      name: "FJobGraphCompletionDiagnostics",
      kind: "構造体", header: "threading/JobGraphCompletionDiagnostics.h",
      summary: "現在の JobGraph 完了カウンタ一括予約だけを返す 16B の診断値。累計ではなく、Reset 後の job 追加を含む次回 Submit の実 job 数を正しく表すため現在状態に限定する。",
      when: "依存 job ごとの <code>Add(1)</code> が復活していないことと、Submit が全 job を一括予約したことを決定的に検証する時。",
      sample: "auto d = graph.CompletionDiagnostics();\nif (d.reservation_batch_count == 1)\n    ACS_ASSERT(d.reserved_job_count == graph.JobCount());",
      members: [
        { sig: "u64 reservation_batch_count", desc: "現在 Submit 済みなら 1、未 Submit または Reset 後なら 0。" },
        { sig: "u64 reserved_job_count", desc: "現在の一括予約に含めた job 数。予約なしなら 0。" }
      ]
    },
    {
      name: "FJobHandle / JobFn",
      kind: "構造体 / 型エイリアス", header: "threading/JobGraph.h",
      summary: "<b>FJobHandle</b>=<t>CJobGraph</t> に追加したジョブを指す軽い参照で、これ越しに依存関係を張る。<b>JobFn</b>=ジョブ本体の関数型 <code>void(*)(void* user, u32 worker_index)</code>(<code>TaskFn</code> と同形式)。",
      when: "<code>CJobGraph::Add</code> の戻り値として受け取り、<code>DependOn</code> で順序を組む時。",
      sample: "auto build  = g.Add(&Build,  &ctx);\nauto upload = g.Add(&Upload, &ctx);\nupload.DependOn(build);   // build → upload の順\nif (build.IsValid()) { /* 追加成功 */ }",
      members: [
        { sig: "CJobGraph* graph", desc: "このハンドルが属するグラフ。" },
        { sig: "u32 index", desc: "グラフ内のジョブ番号。" },
        { sig: "bool IsValid() const", ret: "有効か", desc: "正しいジョブを指していれば true。" },
        { sig: "void DependOn(FJobHandle upstream)", desc: "<code>upstream</code> が完了するまでこのジョブを走らせない、という依存を張る。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "アトミック": "複数<t>スレッド</t>から同時に触っても途中状態が見えず、分割されずに完了する操作。<t>TAtomic</t> が代表。",
  "排他": "あるデータを一度に 1 スレッドだけが触れるように制限すること。<t>FMutex</t> や<t>ロック</t>で実現する。",
  "ロック": "データを守るための『鍵』。取った人だけが対象を触れ、他は解放されるまで待つ。",
  "スレッド": "1 つのプログラム内で同時に進む実行の流れ。複数あると処理を並列にできる。",
  "ジョブ": "並列実行する小さな仕事の単位。依存関係を付けて <t>CJobGraph</t> で順序立てて走らせられる。",
  "条件変数": "ある条件が満たされるまでスレッドを眠らせ、別スレッドの通知で起こす待ち合わせの仕組み。",
  "スレッドプール": "ワーカー<t>スレッド</t>を前もって用意し、タスクを投げて使い回す仕組み。毎回スレッドを作るより軽い。",
  "ワークスチール": "暇なワーカーが忙しいワーカーの未処理タスクを奪って実行し、全体の負荷を均す方式。",
  "フェンス": "メモリ操作の順序を区切る境界。これを跨いで読み書きが並び替えられないよう保証する。",
  "CAS": "Compare-And-Swap。『今の値が期待通りなら新しい値に書き換える』をアトミックに行う基本操作。",
  "RMW": "Read-Modify-Write。読んで・変えて・書く、を一気にアトミックに行う操作(FetchAdd 等)。",
  "POD": "Plain Old Data。コンストラクタ等の特別な振る舞いを持たない単純な構造体。コピーが速く扱いやすい。",
  "RAII": "オブジェクトの寿命に資源の確保/解放を結びつける手法。スコープを抜けると自動で後始末される。",
  "TAtomic": "ACS のアトミック値型(std::atomic 相当)。ロックなしで安全に共有できる小さな値。",
  "FMutex": "一度に 1 スレッドだけ通す<t>排他</t><t>ロック</t>(std::mutex 相当)。",
  "FRwLock": "読み手は同時に複数、書き手は 1 人だけ通すロック(std::shared_mutex 相当)。",
  "CThreadPool": "タスクを並列実行する<t>スレッドプール</t>(<t>ワークスチール</t>方式)。",
  "CJobGraph": "依存関係付きの<t>ジョブ</t>を並列実行するスケジューラ。"
});
