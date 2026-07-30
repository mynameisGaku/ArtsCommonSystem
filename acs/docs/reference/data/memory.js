/* ACS リファレンス — memory モジュール（手書き・品質の手本）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > は &lt; &gt;。 */
ACS_REF.modules.push({
  id: "memory",
  order: 10,
  title: "memory — メモリ / スマートポインタ",
  blurb: "<t>所有権</t>と寿命を安全に扱う型。手で <code>new</code>/<code>delete</code> を書かずに済ませ、<t>アロケータ</t>を用途別に使い分けます。",
  types: [
    {
      name: "TTypedPoolAllocator&lt;T, Capacity&gt;",
      kind: "クラステンプレート", header: "memory/TypedPoolAllocator.h",
      summary: "型と件数から block size、alignment、容量をコンパイル時に固定する pool allocator。",
      when: "同じ型を高頻度に確保・返却し、runtime layout 計算を除きたい時。"
    },
    {
      name: "TUniquePtr&lt;T&gt;",
      kind: "クラステンプレート", header: "memory/UniquePtr.h",
      summary: "<b>1 人だけが所有する</b><t>スマートポインタ</t>(std::unique_ptr 相当)。コピー不可・<t>ムーブ</t>のみ。スコープを抜けると自動で解放。",
      when: "あるデータの持ち主が常に 1 箇所だけ、と言い切れる時。最も軽く安全な既定の選択肢。",
      sample: "auto p = MakeUnique&lt;FMesh&gt;(args...);  // 確保\np-&gt;Draw();                         // -&gt; でアクセス\nauto q = static_cast&lt;TUniquePtr&lt;FMesh&gt;&amp;&amp;&gt;(p); // 所有権をムーブ(p は空に)\n// スコープ脱出で自動解放",
      members: [
        { sig: "T* Get() const", ret: "生ポインタ", desc: "中身の生<t>ポインタ</t>を返す。所有権は渡さない。", when: "生ポインタを要求する API に一時的に渡す時。" },
        { sig: "T& operator*() / T* operator->()", desc: "中身にアクセスする。空の時に使うと<t>未定義動作</t>。" },
        { sig: "explicit operator bool() const", ret: "中身があるか", desc: "<code>if (p)</code> で空かどうか判定できる。" },
        { sig: "T* Release()", ret: "生ポインタ", desc: "所有権を手放して生ポインタを返す。以後の解放責任は呼び出し側に移る。", when: "C API に所有権ごと引き渡す等、自動解放を止めたい時。" },
        { sig: "void Reset(T* p = nullptr)", desc: "今の中身を破棄し、新しい対象(または空)を持つ。" },
        { sig: "FAllocator* GetAllocator() const", ret: "アロケータ", desc: "解放に使う<t>アロケータ</t>を返す。" }
      ]
    },
    {
      name: "TSharedPtr&lt;T&gt;",
      kind: "クラステンプレート", header: "memory/SharedPtr.h",
      summary: "<b>複数で共有</b>する<t>スマートポインタ</t>(std::shared_ptr 相当)。コピーで<t>参照カウント</t>が増え、最後の持ち主が消えた時に対象を破棄。カウントは<t>アトミック</t>でスレッド安全。",
      when: "1 つのデータを複数箇所で持ち回りたいが、誰が最後に解放するか決められない時。",
      sample: "auto p = MakeShared&lt;FTexture&gt;();  // 参照カウント=1\nauto q = p;                       // 共有 → カウント=2\np-&gt;Bind();\n// p,q 両方が消えた時に1度だけ解放される",
      members: [
        { sig: "T* Get() const / *p / p->", desc: "中身にアクセスする。" },
        { sig: "u32 UseCount() const", ret: "強参照数", desc: "今この対象を共有している <code>TSharedPtr</code> の数。主にデバッグ用。", sample: "if (p.UseCount() == 1) { /* 自分だけが持っている */ }" },
        { sig: "bool IsValid() const / explicit operator bool()", desc: "中身があるかを返す。" },
        { sig: "void Reset()", desc: "共有から抜ける(カウントを 1 減らす)。空になる。" },
        { sig: "void Swap(TSharedPtr&amp; o)", desc: "2 つの中身を入れ替える(カウント操作なしで高速)。" }
      ]
    },
    {
      name: "TWeakPtr&lt;T&gt;",
      kind: "クラステンプレート", header: "memory/SharedPtr.h",
      summary: "<t>TSharedPtr</t> への<b><t>弱参照</t></b>(std::weak_ptr 相当)。対象の寿命を<b>延ばさない</b>。生きているか確認し、生きていれば一時的に強参照を得る。",
      when: "対象を所有したくないが生死は監視したい時。とくに<t>循環参照</t>(親子が持ち合う)を断ち切る側に使う。",
      sample: "auto p = MakeShared&lt;FEnemy&gt;();\nTWeakPtr&lt;FEnemy&gt; w = p;        // 弱参照(寿命は延ばさない)\nif (auto s = w.Lock()) {          // 生きていれば強参照を取得\n    s-&gt;Update();\n}\np.Reset();                        // 強参照ゼロ → 破棄\n// w.Lock() は以後ずっと空を返す",
      members: [
        { sig: "TSharedPtr<T> Lock() const", ret: "強参照(空かも)", desc: "対象が生きていれば <code>TSharedPtr</code> を得る。破棄済みなら空。安全に昇格できる(<t>アトミック</t>な確認付き)。", when: "弱参照から実際に中身を触る直前に毎回呼ぶ。" },
        { sig: "bool Expired() const", ret: "期限切れか", desc: "対象がすでに破棄されていれば true。" },
        { sig: "bool IsValid() const", desc: "<code>!Expired()</code>。生きていれば true(瞬間値)。" },
        { sig: "void Reset()", desc: "弱参照を手放す。" }
      ]
    },
    {
      name: "TSharedFromThis&lt;T&gt;",
      kind: "基底クラステンプレート", header: "memory/SharedPtr.h",
      summary: "メンバ関数の中から<b>自分自身の <t>TSharedPtr</t> を作れる</b>ようにする基底(enable_shared_from_this 相当)。<code>class T : public TSharedFromThis&lt;T&gt;</code> と継承する。",
      when: "オブジェクトが『自分を共有する <code>TSharedPtr</code>』を他へ渡したい時(コールバック登録など)。必ず <code>MakeShared</code> 経由で生成すること。",
      sample: "struct FNode : TSharedFromThis&lt;FNode&gt; {\n    TSharedPtr&lt;FNode&gt; Self() { return AsShared(); }\n};\nauto n = MakeShared&lt;FNode&gt;();\nauto same = n-&gt;Self();   // n と同じ対象を共有(カウント+1)",
      members: [
        { sig: "TSharedPtr<T> AsShared()", ret: "自分への強参照", desc: "自分自身を指す <code>TSharedPtr</code> を返す。" },
        { sig: "TWeakPtr<T> AsWeak() const", ret: "自分への弱参照", desc: "自分自身を指す <code>TWeakPtr</code> を返す。" }
      ]
    },
    {
      name: "FObject",
      kind: "基底クラス", header: "memory/ObjectPtr.h",
      summary: "<t>参照カウント</t>で管理される『オブジェクト』の基底。内部に自分の制御ブロックへの<t>ポインタ</t>を持つため、<b>生ポインタからでも</b>強/弱参照を作れる(UE の UObject 風)。",
      when: "エンジン上の実体(キャラ・アクター等)を表し、生ポインタを持ち回りつつ安全に生死を監視したい時。<code>NewObject</code> で生成する。",
      sample: "class AEnemy : public FObject {\npublic:\n    int hp = 100;\n};\nTObjectPtr&lt;AEnemy&gt; e = NewObject&lt;AEnemy&gt;();\nAEnemy* raw = e.Get();\nTWeakObjectPtr&lt;AEnemy&gt; w(raw);  // 生ポインタから弱参照が作れる",
      members: [
        { sig: "virtual ~FObject()", desc: "<t>デストラクタ</t>は仮想。派生クラスが正しく破棄される。" }
      ]
    },
    {
      name: "TObjectPtr&lt;T&gt;",
      kind: "クラステンプレート", header: "memory/ObjectPtr.h",
      summary: "<t>FObject</t> 派生への<b>強参照</b>(生かし続ける所有ポインタ)。別名 <code>TStrongObjectPtr&lt;T&gt;</code>。生ポインタ <code>T*</code> からも構築できる。",
      when: "オブジェクトを確実に生存させたい所有者側。",
      sample: "TObjectPtr&lt;FEnemy&gt; e = NewObject&lt;FEnemy&gt;(50);\ne-&gt;hp -= 10;\nTObjectPtr&lt;FEnemy&gt; e2 = e;  // 共有(カウント+1)\ne.Reset();                  // まだ e2 が居るので生存",
      members: [
        { sig: "explicit TObjectPtr(T* obj)", desc: "生ポインタから強参照を作る(<code>NewObject</code> 由来である必要あり)。" },
        { sig: "T* Get() / *o / o->", desc: "中身にアクセスする。" },
        { sig: "u32 UseCount() const", ret: "強参照数", desc: "今この対象を持つ強参照の数。" },
        { sig: "bool IsValid() / void Reset() / void Swap(...)", desc: "<code>TSharedPtr</code> と同じ操作。" }
      ]
    },
    {
      name: "TWeakObjectPtr&lt;T&gt;",
      kind: "クラステンプレート", header: "memory/ObjectPtr.h",
      summary: "<t>FObject</t> 派生への<b><t>弱参照</t></b>。対象が破棄されると自動的に無効になる。生ポインタからも作れる。",
      when: "『さっき掴んだ敵がもう死んでいるかもしれない』ような、所有せず生死だけ追う参照。",
      sample: "TWeakObjectPtr&lt;FEnemy&gt; w = e;   // or  TWeakObjectPtr&lt;FEnemy&gt; w(rawPtr);\nif (w.IsValid()) w.Get()-&gt;Hit();  // 生きていれば\nif (auto s = w.Pin()) s-&gt;Hit();    // 破棄と競合しても安全に強参照化",
      members: [
        { sig: "bool IsValid() const", ret: "生存中か", desc: "対象がまだ生きていれば true。" },
        { sig: "bool IsStale() const", desc: "破棄済みなら true(<code>!IsValid()</code>)。" },
        { sig: "T* Get() const", ret: "対象 or null", desc: "生きていれば対象、破棄済みなら null。簡易アクセス用。", when: "単一スレッドで素早く触りたい時。別スレッドが破棄し得るなら <code>Pin()</code>。" },
        { sig: "TObjectPtr<T> Pin() const", ret: "強参照(空かも)", desc: "生きていれば強参照に昇格して返す。破棄と競合しても安全。" }
      ]
    },
    {
      name: "MakeUnique / MakeShared / NewObject",
      kind: "ファクトリ関数", header: "memory/*.h",
      summary: "スマートポインタを作る入口。既定では <t>DefaultAllocator</t> を使う。<code>...In</code> 版で<t>アロケータ</t>を指定できる。",
      when: "スマートポインタは原則これらで生成する(<code>new</code> を直接書かない)。",
      sample: "auto u = MakeUnique&lt;FFoo&gt;(a, b);             // TUniquePtr\nauto s = MakeShared&lt;FBar&gt;();                 // TSharedPtr (制御ブロックと同居の1確保)\nauto o = NewObject&lt;FEnemy&gt;(100);             // TObjectPtr (FObject 派生)\nauto s2 = MakeSharedIn&lt;FBar&gt;(myAlloc);        // アロケータ指定",
      members: [
        { sig: "TUniquePtr<T> MakeUnique<T>(args...)", desc: "既定アロケータで <code>TUniquePtr</code> を作る。" },
        { sig: "TSharedPtr<T> MakeShared<T>(args...)", desc: "既定アロケータで <code>TSharedPtr</code> を作る(制御ブロックと中身を1回の確保にまとめる)。" },
        { sig: "TObjectPtr<T> NewObject<T>(args...)", desc: "<code>FObject</code> 派生を作り <code>TObjectPtr</code> を返す。" },
        { sig: "...In(FAllocator& a, args...)", desc: "<code>MakeUniqueIn</code> / <code>MakeSharedIn</code> / <code>NewObjectIn</code>。確保に使う<t>アロケータ</t>を明示する版。" }
      ]
    },
    {
      name: "TRc&lt;T&gt; / MakeRc",
      kind: "非推奨エイリアス", header: "memory/Rc.h",
      summary: "<b>旧名</b>。<code>TRc</code> は <t>TSharedPtr</t> の、<code>MakeRc</code> は <code>MakeShared</code> の別名として残っている互換シム。",
      when: "既存コード互換のためだけ。新規コードでは <code>TSharedPtr</code> / <code>MakeShared</code> を使う。",
      sample: "// 旧: TRc<T> p = MakeRc<T>();\n// 新: TSharedPtr<T> p = MakeShared<T>();"
    },
    {
      name: "FAllocator",
      kind: "インターフェース", header: "memory/Allocator.h",
      summary: "メモリ確保/解放の<t>インターフェース</t>。すべての<t>アロケータ</t>はこれを実装し、差し替え可能。",
      when: "確保方法を切り替えたい/独自アロケータを差し込みたい時に、この型で受け渡す。",
      sample: "FAllocator& a = DefaultAllocator();\nvoid* p = a.Alloc(256, 16, FSourceLoc::Current());\na.Free(p);",
      members: [
        { sig: "void* Alloc(usize size, usize alignment, FSourceLoc location)", ret: "確保した領域", desc: "指定サイズ・整列でメモリを確保。失敗時 null。" },
        { sig: "void Free(void* pointer)", desc: "<code>Alloc</code> で得た領域を解放。" },
        { sig: "u64 BytesAllocated() const", ret: "確保中バイト数", desc: "現在確保中の合計(実装による)。リーク調査に。" },
        { sig: "void* Realloc(void* pointer, usize old_size, usize new_size, usize alignment, FSourceLoc location)", ret: "新領域", desc: "既存確保を拡大/縮小する。既定実装は <code>Alloc</code>+<code>MemCopy</code>+<code>Free</code>(実装で上書き可)。" },
        { sig: "u64 PeakBytes() const", ret: "ピーク量", desc: "過去最大の使用量(既定 0、実装で上書き)。" },
        { sig: "const char* Name() const", ret: "識別名", desc: "アロケータの名前(\"System\"/\"TLSF\" 等)。診断/可視化用。" },
        { sig: "u64 LifetimeGeneration() const", ret: "寿命世代", desc: "アロケータの有効期間ごとに異なる非0値。寿命追跡非対応または Shutdown 後は 0。遅延解放先の再初期化検出に使う。" },
        { sig: "void* Alloc(usize size, FSourceLoc location = FSourceLoc::Current())", ret: "確保領域", desc: "整列を <code>kDefaultAlignment</code>(16B)・呼び出し位置を自動キャプチャする利便オーバーロード。" }
      ]
    },
    {
      name: "DefaultAllocator()",
      kind: "関数", header: "memory/Memory.h",
      summary: "既定の<t>アロケータ</t>を返す(起動時は <code>FSystemAllocator</code>)。<code>MakeShared</code> 等が指定なしの時に使う。<code>SetDefaultAllocator</code> で差し替え可能。",
      when: "特別な事情がなければこれでよい。", sample: "FAllocator& a = DefaultAllocator();"
    },
    {
      name: "FArenaAllocator",
      kind: "クラス", header: "memory/ArenaAllocator.h",
      summary: "確保はポインタを進めるだけ(超高速)、解放は<b>まとめて Reset</b>のみ、という使い捨て<t>アロケータ</t>。容量は固定せず、満杯になると<b>ページを足して自動で伸びる</b>(ページバック式バンプ)。同形状の領域は batch 予約でき、Reset は世代を進めて先頭 page だけを即時初期化する。",
      when: "1 フレームだけ使って次フレームに全部捨てる一時データ(描画コマンド等)。個別解放はしない。",
      sample: "FArenaAllocator arena(64 * 1024);   // 引数は1ページのサイズ(既定 64KB)。容量上限ではない\nvoid* a = arena.Alloc(64, 16, FSourceLoc::Current());\n// ...フレーム処理...\narena.Reset();                      // 全確保を一括無効化(超高速)。ページは再利用",
      members: [
        { sig: "FArenaAllocator(usize page_size = 64*1024, FAllocator* backing_allocator = nullptr)", desc: "<b>第1引数はページサイズ</b>(全体容量ではない)。満杯時は backing_allocator から新ページを足して伸びる。" },
        { sig: "void* Alloc(usize size, usize alignment, FSourceLoc location)", ret: "確保した領域", desc: "末尾にポインタを進めて確保。alignment は 2 のべき乗かつ最大 64 KiB。不正値やサイズ計算のオーバーフローは null で拒否する。" },
        { sig: "bool AllocBatch(void** output, usize count, usize size, usize alignment, FSourceLoc location = Current())", ret: "全領域を確保できたか", desc: "同じ size/alignment の <code>count</code> 領域を cursor 更新 1 回で予約する。失敗時は output 全要素を null にし、統計を変えない。" },
        { sig: "void Free(void* pointer)", desc: "<b>no-op</b>(個別解放はしない)。" },
        { sig: "void Reset(bool release_pages = false)", desc: "全確保を一括無効化。開始済み Alloc の完了を待ち、Reset 中の新規 Alloc は null で拒否する。既定経路は世代を進め、先頭以外の保持 page を再利用直前に遅延初期化する。<code>release_pages=true</code> で backing に全ページを返す。" },
        { sig: "FArenaAllocatorDiagnostics Diagnostics() const", ret: "診断 snapshot", desc: "保持 page、batch 回数/領域数、直前 Reset の page visit、遅延初期化数を返す。通常 Alloc に診断用 atomic 更新は追加しない。" }
      ]
    },
    {
      name: "FArenaAllocatorDiagnostics",
      kind: "構造体", header: "memory/ArenaAllocatorDiagnostics.h",
      summary: "Arena の batch と世代 Reset の効果を観測する 40 byte の値 snapshot。owner や寿命は持たない。",
      members: [
        { sig: "u64 retained_pages", desc: "現在保持する page 数。診断取得時に GrowLock 内で数える。" },
        { sig: "u64 batch_allocations", desc: "最後の Reset 以降に成功した batch 呼び出し数。" },
        { sig: "u64 batch_suballocations", desc: "最後の Reset 以降に batch で返した領域数。" },
        { sig: "u64 last_reset_page_visits", desc: "直前の Reset が直接参照した page 数。通常の保持 Reset は 0 または 1。" },
        { sig: "u64 lazy_page_resets", desc: "最後の Reset 以降に再利用直前まで初期化を遅らせた page 数。" }
      ]
    },
    {
      name: "FLinearAllocator / FPoolAllocator",
      kind: "クラス", header: "memory/LinearAllocator.h / PoolAllocator.h",
      summary: "<b>Linear</b>=前から詰める単純高速アロケータ(Arena に近い)。<b>Pool</b>=同じサイズの塊を大量に出し入れする用途に最適化したアロケータ。",
      when: "Linear: 連続確保して一括破棄。Pool: 弾・パーティクル等、同サイズを高速に取り回す時。",
      sample: "FPoolAllocator pool(sizeof(FBullet), 1024); // 1024 個分\nvoid* b = pool.Alloc(...);\npool.Free(b);",
      members: [
        { sig: "FLinearAllocator(usize capacity, FAllocator* backing_allocator = nullptr)", desc: "<b>Linear</b>: <code>capacity</code> バイトのバッファを <code>backing_allocator</code>(null で <code>DefaultAllocator</code>)から確保。" },
        { sig: "void FLinearAllocator::Reset()", desc: "カーソルを 0 へ巻き戻す(全確保を無効化)。並行 Alloc 中に呼ぶと UB。" },
        { sig: "u64 FLinearAllocator::Capacity() const", ret: "容量", desc: "確保したバッファの総バイト数。" },
        { sig: "FPoolAllocator(usize block_size, usize block_count, usize alignment = kDefaultAlignment, FAllocator* backing_allocator = nullptr)", desc: "<b>Pool</b>: <code>block_size</code>×<code>block_count</code> の連続バッファを確保。<code>block_size</code> は最低 8B にラウンドアップ。" },
        { sig: "u64 FPoolAllocator::BlockSize() / BlockCount() const", ret: "統計", desc: "1 ブロックのサイズ / 総ブロック数。" },
        { sig: "bool FPoolAllocator::Contains(const void* pointer) const", ret: "所属か", desc: "<code>pointer</code> がこのプールから払い出されたものか(Heap フォールバックとの区別用)。" }
      ]
    },
    {
      name: "FTlsfAllocator / FShardedTlsfAllocator",
      kind: "クラス", header: "memory/Tlsf.h / ShardedTlsf.h",
      summary: "<b>TLSF</b>=任意サイズを O(1)・低断片化で確保できる汎用<t>アロケータ</t>。確保開始位置を 16B ごとの外部ビットマップ(プール容量の約 0.78%)でも照合し、内部ポインタの偽装解放を拒否する。<b>Sharded</b>=N 個の TLSF に分けて複数<t>スレッド</t>からの同時確保でも競合しにくくした版(<code>kMaxShards=8</code>)。どちらも<b>既定構築してから <code>Init()</code> で初期化</b>する二段階(コンストラクタにサイズは渡さない)。",
      when: "汎用の高速確保が欲しい時。マルチスレッドで確保が多いなら Sharded。",
      sample: "// TLSF: 自前のプール buffer を渡して Init する\nalignas(16) static acs::u8 pool[1u &lt;&lt; 24];   // 16MB\nFTlsfAllocator tlsf;\nif (tlsf.Init(pool, sizeof(pool)).IsErr()) { /* 失敗 */ }\nvoid* p = tlsf.Alloc(200, 16, FSourceLoc::Current());\n\n// Sharded: 予約サイズ・初期コミット量で Init(shard_count 0=コア数で自動)\nFShardedTlsfAllocator sharded;\nsharded.Init(256u &lt;&lt; 20, 8u &lt;&lt; 20);          // 256MB 予約 / 8MB コミット",
      members: [
        { sig: "TResult&lt;void&gt; FTlsfAllocator::Init(void* pool_base, usize pool_size)", ret: "成否", desc: "16B 整列のプール buffer を登録(<code>pool_size</code> は 1KB 以上推奨)。", when: "<b>サイズ引数のコンストラクタは無い</b>。必ず既定構築→Init。" },
        { sig: "TResult&lt;void&gt; FShardedTlsfAllocator::Init(usize total_reserve_bytes, usize commit_initial_bytes, u32 shard_count = 0)", ret: "成否", desc: "予約全体をシャードで分割。<code>shard_count=0</code> で論理コア数から自動(最大 8)。粒度整列の加算オーバーフローは VM 予約前に拒否する。" },
        { sig: "void FShardedTlsfAllocator::EnableThreadCache()", desc: "スレッドローカルの固定長ポインタ配列で小確保のヒット経路をロックなし化。管理リンクを解放済み payload 内へ置かないため、UAF 書込みを次のアドレスとして参照しない。Free は所有シャードをロックして正規の確保開始位置を確認してから格納する。アロケータ切替、スレッド終了、Shutdown/再Initは寿命世代で検証し、二重解放はブロック状態遷移で拒否する。" },
        { sig: "void* Alloc / void Free / void* Realloc", desc: "<code>FAllocator</code> の確保 API。Sharded の Realloc-to-zero は所有権を先に検証し、不正ポインタなら元の pointer を返して未解放を通知する。" },
        { sig: "FTlsfAllocator::FStats FTlsfAllocator::GetStats() const", ret: "統計", desc: "<code>bytes_used / bytes_peak / free_blocks / used_blocks / largest_free_block</code> を返す。" },
        { sig: "bool FTlsfAllocator::ValidateHeap() const", ret: "健全か", desc: "物理ブロックチェイン/フラグの一貫性を検証(O(ブロック数)、診断用)。Alloc/Free と同じロック下で呼ぶこと。" },
        { sig: "bool FTlsfAllocator::ContainsPtr(const void* pointer) const", ret: "所属か", desc: "<code>pointer</code> がこのアロケータの管理範囲か。予約所有時は O(1)。" },
        { sig: "TResult&lt;void&gt; FTlsfAllocator::AddPool(void* pool_base, usize pool_size) / InitWithReservation(FVmReservation&amp;&amp; reservation, usize initial_commit_bytes)", ret: "成否", desc: "追加プール登録 / <code>FVmReservation</code> を保持して初期コミット分をプール化。重複範囲、表現上限超過、追跡上限(64プール)を状態変更前に拒否する。" },
        { sig: "TResult&lt;void&gt; FTlsfAllocator::Reset()", ret: "成否", desc: "未初期化状態へ戻し、VM予約と確保開始ビットマップを解放する。VM予約の解放に失敗した場合は再試行できるよう状態を保持してエラーを返す。" },
        { sig: "static usize FTlsfAllocator::PayloadBlockSize(const void* pointer)", ret: "ブロックサイズ", desc: "払い出した payload のブロックサイズをレイアウトだけからロック無しで読む(thread-cache 用)。" },
        { sig: "u32 FShardedTlsfAllocator::ShardCount() const", ret: "シャード数", desc: "分割されているシャードの数(最大 <code>kMaxShards=8</code>)。" },
        { sig: "bool FShardedTlsfAllocator::ThreadCacheEnabled() const / u64 Epoch() const", ret: "状態/世代", desc: "thread-cache 有効か / Init ごとに更新されるエポック(マガジン世代検証用)。" },
        { sig: "bool FShardedTlsfAllocator::ValidateHeap()", ret: "健全か", desc: "全シャードをロックして整合検証する(診断用)。" }
      ]
    },
    {
      name: "FRelocatableAllocator",
      kind: "クラス", header: "memory/RelocatableAllocator.h",
      summary: "確保した領域を後で<b>動かして詰め直せる</b>(<t>ハンドル</t>越しにアクセス)<t>アロケータ</t>。<code>Compact()</code> で断片化を解消できる。<b>内部同期なし</b>=単一スレッドで使う。既定構築→<code>Init()</code> の二段階。",
      when: "長時間動かし続けてメモリ断片化が問題になる場面で、コンパクションしたい時。生ポインタでなくハンドルで持つ。",
      sample: "FRelocatableAllocator ra;\nra.Init(1u &lt;&lt; 20, 4096);             // 1MB アリーナ / ハンドル最大 4096\nFRelocHandle h = ra.Alloc(128);          // 失敗時は h.IsValid()==false\nvoid* p = ra.Resolve(h);                 // 使う直前に解決(次の Compact まで有効)\nra.Compact();                            // 隙間を詰める→生ポインタは無効化、ハンドルは有効\np = ra.Resolve(h);                       // Compact 後は取り直す",
      members: [
        { sig: "TResult&lt;void&gt; Init(usize capacity_bytes, u32 max_handles, FAllocator* backing = nullptr)", ret: "成否", desc: "アリーナ容量とハンドル表サイズを確保。<code>backing=nullptr</code> で <code>DefaultAllocator</code>。" },
        { sig: "FRelocHandle Alloc(usize size, usize align = 16)", ret: "ハンドル", desc: "確保してハンドルを返す。末尾に入らなくても詰めれば入る時は自動 Compact。失敗時は無効ハンドル。" },
        { sig: "void* Resolve(FRelocHandle h) const", ret: "現ポインタ", desc: "ハンドルの現在位置。無効/解放済みは null。<b>次の Compact まで有効</b>。" },
        { sig: "usize Compact()", ret: "回収バイト数", desc: "生存ブロックを前方に詰めて断片化を解消し、全ハンドルの参照先を更新する。" },
        { sig: "void Free(FRelocHandle h) / usize SizeOf(h) / bool ValidateHandle(h)", desc: "解放・サイズ取得・有効性検証。" },
        { sig: "void Shutdown()", desc: "アリーナとハンドルテーブルを backing へ返却する(デストラクタでも呼ばれる)。" },
        { sig: "usize Capacity() / Used() / HighWater() const", ret: "統計", desc: "アリーナ容量 / 生存ペイロード総量 / bump カーソル位置(gap 込み)。" },
        { sig: "u32 LiveCount() const", ret: "生存数", desc: "現在生きている確保(ハンドル)の数。" }
      ]
    },
    {
      name: "FMemorySystem",
      kind: "クラス(静的)", header: "memory/MemorySystem.h",
      summary: "用途別の領域(<t>セグメント</t>)・ハード予算・統計・リーク追跡を束ねる、エンジン全体のメモリ管理ファサード。通常セグメントは独立した mimalloc first-class heap、Temp は一括リセット可能な arena を使う。<code>install_as_default_allocator=true</code> なら既定確保も Default セグメントへ通す。",
      when: "メモリ使用量の可視化・上限管理・領域分割をまとめて行いたい時。",
      sample: "FMemorySystemConfig configuration = FMemorySystem::DefaultConfig();\nconfiguration.install_as_default_allocator = true;\nif (FMemorySystem::Init(configuration).IsErr()) { /* 失敗 */ }\n\n{ FScopedMemorySegment segment(ESegment::Temp);\n  FAllocator* allocator = FMemorySystem::CurrentAllocator();\n  /* 1フレーム寿命の確保 */\n}\nFMemorySystem::ResetTemp();\n\nSegmentStats statistics[(int)ESegment::_Count];\nu32 count = FMemorySystem::GetStats(statistics, 5);\nFMemorySystem::Shutdown();",
      members: [
        { sig: "static TResult&lt;void&gt; Init(const FMemorySystemConfig&amp;)", ret: "成否", desc: "全セグメントを設定で初期化(多重 Init はエラー)。" },
        { sig: "static FMemorySystemConfig DefaultConfig()", ret: "既定設定", desc: "小規模/デフォルト用の設定を返す。" },
        { sig: "static FAllocator* Get(ESegment)", ret: "アロケータ", desc: "セグメント別アロケータ(Init 前は null)。" },
        { sig: "static void ResetTemp()", desc: "Temp セグメントを巻き戻す(フレーム先頭で 1 回)。開始済み Temp 操作を待ち、arena・ハード予算予約・割り当て追跡を同じ排他区間でリセットする。Reset 中の新規操作は失敗する。" },
        { sig: "static u32 GetStats(FSegmentStats* output, u32 output_capacity)", ret: "件数", desc: "全セグメントの要求量・ピーク・ハード予算・未解放件数を取得。" },
        { sig: "static MemorySegmentInspection InspectSegmentMemory(ESegment segment)", ret: "独立検査", desc: "mimalloc の生存ブロック列挙から統計を再構築し、ACS カウンタとの一致を検証する。保守点で呼ぶ。" },
        { sig: "static MemoryLeakSummary CaptureLeakSummary()", ret: "リーク集計", desc: "一括寿命の frame arena セグメントを除き、未解放件数・要求バイト・該当セグメント数を集計する。" },
        { sig: "static ESegment Current()", ret: "現セグメント", desc: "<code>FScopedMemorySegment</code> が設定中の『現在のセグメント』を返す。" },
        { sig: "static FAllocator* CurrentAllocator()", ret: "アロケータ", desc: "現在セグメントのアロケータ。明示確保やセグメント対応コンテナへ渡す。" },
        { sig: "static void Shutdown()", desc: "全セグメントを解放(既定アロケータも元へ戻す)。" }
      ]
    },
    {
      name: "FMimallocAllocator",
      kind: "クラス", header: "memory/MimallocAllocator.h",
      summary: "mimalloc v3 の <b>first-class heap</b> を所有する汎用アロケータ。要求量の厳密なハード予算、未解放件数、ピーク、サイズ帯ヒストグラム、独立ブロック走査を提供する。malloc/new のグローバル上書きは行わない。",
      when: "独立した仮想メモリヒープ、並列確保、用途別予算、終了時のリーク照合が必要な時。通常は <code>FMemorySystem</code> 経由で使う。",
      sample: "FMimallocAllocator allocator;\nif (allocator.Init(64u &lt;&lt; 20).IsOk()) {\n    void* memory = allocator.Alloc(4096, 64, FSourceLoc::Current());\n    allocator.Free(memory);\n    MimallocHeapInspectionStatistics inspection = allocator.InspectHeap();\n    allocator.Shutdown();\n}",
      members: [
        { sig: "TResult&lt;void&gt; Init(u64 hard_budget_bytes = 0)", ret: "成否", desc: "独立ヒープを作成する。0 は予算無制限。" },
        { sig: "void* Alloc / void Free / void* Realloc", desc: "スレッドセーフな確保・解放・再確保。再確保中は旧領域と新領域の要求量を両方予算へ含める。" },
        { sig: "MimallocHeapInspectionStatistics InspectHeap()", ret: "独立統計", desc: "mimalloc のブロック列挙から予約・コミット・生存要求量を再構築し、atomic カウンタと照合する。" },
        { sig: "MimallocAllocationHistogram CaptureAllocationHistogram() const", ret: "サイズ分布", desc: "small / medium / large の生存件数と要求量を返す。" }
      ]
    },
    {
      name: "FSystemAllocator",
      kind: "クラス", header: "memory/SystemAllocator.h",
      summary: "Win32 <code>HeapAlloc</code>/<code>HeapFree</code> を使う汎用<t>アロケータ</t>。プロセスヒープは OS がロックするので<b>スレッドセーフ</b>。<b>起動時の <code>DefaultAllocator</code> はこれ</b>。",
      when: "特別なアロケータを差さない通常時の確保(<code>DefaultAllocator()</code> 経由で間接的に使われる)。数千回/フレームのホットパスでは専用プール/アリーナを別途用意する。",
      sample: "FSystemAllocator sys;\nvoid* p = sys.Alloc(256, 16, FSourceLoc::Current());\nsys.Free(p);",
      members: [
        { sig: "void* Alloc / void Free / void* Realloc", desc: "<code>FAllocator</code> 実装。16B 超の整列はヘッダ方式で対応。" },
        { sig: "u64 BytesAllocated() const / u64 PeakBytes() const", ret: "統計", desc: "アトミックに集計した現在量/ピーク。" }
      ]
    },
    {
      name: "New / Delete / NewArray / DeleteArray",
      kind: "関数テンプレート", header: "memory/New.h",
      summary: "グローバル <code>new</code>/<code>delete</code> を使わず、<b><t>アロケータ</t>からオブジェクトを構築/破棄</b>する基本ヘルパ。確保→配置 new→デストラクタ→Free を 1 関数化。",
      when: "<code>FAllocator</code> 直下で生のオブジェクトを作りたい低レベルな場面。通常は <code>MakeUnique</code>/<code>MakeShared</code> を使う。",
      sample: "FAllocator&amp; a = DefaultAllocator();\nMyObj* p = New&lt;MyObj&gt;(a, args...);   // 確保 + コンストラクタ\nDelete(a, p);                        // デストラクタ + Free\n\nint* arr = NewArray&lt;int&gt;(a, 64);\nDeleteArray(a, arr, 64);",
      members: [
        { sig: "T* New&lt;T&gt;(FAllocator&amp; a, Args&amp;&amp;... args)", ret: "T*", desc: "確保してコンストラクタ引数を完全転送。失敗時 null。" },
        { sig: "void Delete&lt;T&gt;(FAllocator&amp; a, T* p)", desc: "デストラクタ(トリビアルなら省略)→Free。null は no-op。" },
        { sig: "T* NewArray&lt;T&gt;(FAllocator&amp; a, usize n)", ret: "T*", desc: "n 個確保して各要素を既定構築。オーバーフローは null。" },
        { sig: "void DeleteArray&lt;T&gt;(FAllocator&amp; a, T* arr, usize n)", desc: "後ろから順にデストラクタ→一括 Free。" }
      ]
    },
    {
      name: "MemCopy / MemMove / MemSet / MemCmp",
      kind: "関数", header: "memory/Memory.h",
      summary: "低レベルなバイト操作(<code>std::memcpy</code> 等の代替)。<code>SetDefaultAllocator</code> で既定<t>アロケータ</t>を差し替えられる。",
      when: "生バイト列のコピー/比較/フィル。型のある配列は通常コンテナ側に任せる。",
      sample: "MemCopy(dst, src, n);           // 非重複コピー\nMemMove(dst, src, n);           // 重複可\nMemSet(buf, 0, n);              // 0 クリア\nint c = MemCmp(a, b, n);        // 比較(0=一致)\nSetDefaultAllocator(&amp;myAlloc); // 既定を差し替え(null で System に戻す)",
      members: [
        { sig: "void MemCopy(void* dst, const void* src, usize n)", desc: "領域<b>非重複</b>コピー。" },
        { sig: "void MemMove(void* dst, const void* src, usize n)", desc: "領域<b>重複可</b>コピー。" },
        { sig: "void MemSet(void* dst, int value, usize n)", desc: "バイト単位フィル。" },
        { sig: "int MemCmp(const void* a, const void* b, usize n)", ret: "比較結果", desc: "バイト比較(0=一致)。" },
        { sig: "void SetDefaultAllocator(FAllocator* a)", desc: "既定アロケータを差し替え。<code>nullptr</code> で <code>FSystemAllocator</code> に戻す。起動初期に呼ぶ。" }
      ]
    },
    {
      name: "ESegment / FScopedMemorySegment",
      kind: "enum / クラス", header: "memory/Segment.h",
      summary: "メモリを目的・寿命で分ける論理ヒープの種別。<code>FMemorySystem</code> が各<t>セグメント</t>に独立予算を持つ。<code>FScopedMemorySegment</code> で<b>スコープ中の既定確保先</b>を切り替える(<t>RAII</t>)。",
      when: "確保を用途別に隔離して断片化や予算を管理したい時。",
      sample: "{ FScopedMemorySegment seg(ESegment::Resource);  // アセット用領域へ\n  auto tex = LoadTexture(...);\n}                                               // 抜けると元へ戻る",
      members: [
        { sig: "ESegment::Default / Permanent / Temp / Resource / Develop", desc: "汎用 / 起動時常駐 / 1フレーム / アセット / エディタ・デバッグ。" },
        { sig: "enum class EAllocKind", desc: "プロファイラ分類用のタグ(Generic=0 / Engine=1 / Game=2 / Render=3 / Audio=4 / Asset=5 / UI=6 / Network=7 / Debug=8)。" },
        { sig: "const char* ToString(ESegment s)", ret: "名前文字列", desc: "<code>ESegment</code> を <code>\"Default\"</code> 等の文字列にする(ログ/プロファイラ表示用)。" },
        { sig: "explicit FScopedMemorySegment(ESegment s)", desc: "現在セグメントを切り替え、デストラクタで元に戻す。" }
      ]
    },
    {
      name: "FRelocHandle",
      kind: "構造体", header: "memory/RelocatableAllocator.h",
      summary: "<code>FRelocatableAllocator</code> の確保を指す<t>ハンドル</t>。<code>index</code>+<code>generation</code> 構成で <code>generation==0</code> が無効。<t>世代</t>で use-after-free を検出。",
      when: "再配置アロケータの確保を保持する時。中身のポインタは持たず <code>Resolve(h)</code> で都度引く。",
      sample: "FRelocHandle h = ra.Alloc(128);\nif (h.IsValid()) { void* p = ra.Resolve(h); }",
      members: [
        { sig: "u32 index / u32 generation", desc: "エントリ番号と世代(0=無効)。" },
        { sig: "bool IsValid() const", ret: "有効か", desc: "<code>generation != 0</code>。" },
        { sig: "bool operator==(FRelocHandle, FRelocHandle) / operator!=(...)", desc: "<code>index</code> と <code>generation</code> 両方の一致でハンドル同士を比較する自由関数。" }
      ]
    },
    {
      name: "FVmReservation",
      kind: "クラス", header: "memory/VirtualMemory.h",
      summary: "巨大な仮想アドレス空間を<b>予約(Reserve)</b>し、必要なページだけ<b>物理コミット(Commit)</b>する低レベル <t>RAII</t> ハンドル(VirtualAlloc ラッパ)。Decommit したページは内部 <b>LRU キャッシュ(16 エントリ、既定の総量上限 64 MiB)</b>に保持し、再 Commit がヒットすればシステムコールを省く。コピー不可・<t>ムーブ</t>可。",
      when: "<code>FTlsfAllocator</code> 等の土台として、上限の決まった広大なアリーナを予約だけ先に取り、使う分だけ後からコミットしたい時。通常は直接使わずアロケータ越しに使う。",
      sample: "auto r = FVmReservation::Reserve(256u &lt;&lt; 20);   // 256MB 予約(物理はまだ)\nif (r.IsOk()) {\n    FVmReservation vm = static_cast&lt;FVmReservation&amp;&amp;&gt;(r.Value());\n    vm.Commit(0, 8u &lt;&lt; 20);                    // 先頭 8MB を物理割り当て\n    void* base = vm.Base();\n    vm.Decommit(0, 8u &lt;&lt; 20);                  // 返却(実 VirtualFree は LRU エビクト時)\n}                                                 // スコープ脱出で Release",
      members: [
        { sig: "static TResult&lt;FVmReservation&gt; Reserve(usize capacity_bytes) noexcept", ret: "予約 or エラー", desc: "<code>capacity_bytes</code> の仮想範囲を予約する(MEM_RESERVE)。物理はまだ割り当てない。", when: "アリーナの上限を先に押さえる時。" },
        { sig: "TResult&lt;void&gt; Release() noexcept", ret: "成否", desc: "予約全体を解放する。失敗時は所有状態を保持して再試行可能にし、機械可読ログを出す。デストラクタからも呼ばれる。" },
        { sig: "TResult&lt;void&gt; Commit(usize offset, usize size) noexcept", ret: "成否", desc: "<code>[offset, offset+size)</code> の物理ページを割り当てる。LRU ヒット時はシステムコールを省く。" },
        { sig: "TResult&lt;void&gt; Decommit(usize offset, usize size) noexcept", ret: "成否", desc: "物理ページを返却する。実際の VirtualFree は LRU からエビクトされる時まで遅延する。" },
        { sig: "void* Base() const noexcept", ret: "先頭", desc: "予約範囲の先頭アドレス。未予約なら null。" },
        { sig: "usize Capacity() const noexcept", ret: "予約量", desc: "予約した総バイト数。" },
        { sig: "usize Committed() const noexcept", ret: "利用中コミット量", desc: "現在利用中のコミット済みバイト数。遅延デコミットキャッシュ分は含まない。" },
        { sig: "usize CachedCommittedBytes() const / usize ActualCommittedBytes() const", ret: "保持量", desc: "遅延デコミットキャッシュと OS 上の実コミット合計を区別して返す。" },
        { sig: "TResult&lt;void&gt; SetMaximumCachedDecommitBytes(usize maximum_cached_decommit_bytes)", ret: "成否", desc: "遅延デコミットキャッシュの総量上限を変更する。0 なら無効化し、超過分は LRU 末尾から即時返却する。" },
        { sig: "TResult&lt;void&gt; TrimDecommitCache(usize target_cached_bytes) / FlushDecommitCache()", ret: "成否", desc: "遅延デコミットキャッシュを上限まで縮小、または全解放する。" },
        { sig: "u32 LruHitCount() const / u32 LruMissCount() const noexcept", ret: "統計", desc: "再 Commit が LRU キャッシュにヒット/ミスした回数(プロファイラ用)。" }
      ]
    },
    {
      name: "VmPageSize / VmAllocGranularity / VmIsAligned / VmZeroFastNT",
      kind: "関数 / 定数", header: "memory/VirtualMemory.h",
      summary: "仮想メモリ層の補助関数とページサイズ定数。<b>VmZeroFastNT</b> は x64 の CPU/OS が AVX と XMM/YMM 状態保存に対応し、32B 整列かつ 256B 以上なら <t>Non-Temporal</t> ストアを使う。非対応CPU、ARM64、未整列、小領域は memset へ安全にフォールバックする。",
      when: "VM 予約の整列計算や、巨大バッファを高速にゼロクリアしたい低レベルな場面。",
      sample: "usize ps = VmPageSize();              // OS ページサイズ\nusize gr = VmAllocGranularity();      // VirtualAlloc 予約粒度\nif (VmIsAligned((uptr)p, 4096)) { /* ... */ }\nVmZeroFastNT(buf, 4u &lt;&lt; 20);          // 4MB を非テンポラル書き込みでゼロ化",
      members: [
        { sig: "usize VmPageSize() noexcept", ret: "OS ページサイズ", desc: "OS の物理ページサイズ(通常 4KB)。" },
        { sig: "usize VmAllocGranularity() noexcept", ret: "予約粒度", desc: "VirtualAlloc の予約粒度(Windows では通常 64KB)。" },
        { sig: "bool VmIsAligned(uptr address, usize alignment) noexcept", ret: "整列か", desc: "<code>address</code> が <code>alignment</code> の倍数なら true。" },
        { sig: "void VmZeroFastNT(void* destination, usize size) noexcept", desc: "利用可能なら非テンポラルストアでキャッシュを汚さずゼロクリアし、非対応環境は通常経路へフォールバックする。" },
        { sig: "constexpr usize kVmSmallPageSize = 16*1024", desc: "論理小ページ単位(16 KiB)。" },
        { sig: "constexpr usize kVmMediumPageSize = 128*1024", desc: "論理中ページ単位(128 KiB)。" },
        { sig: "constexpr usize kVmSegmentSize = 8*1024*1024", desc: "論理セグメント単位(8 MiB)。" }
      ]
    },
    {
      name: "FMappedT / FPageT",
      kind: "構造体", header: "memory/VirtualMemory.h",
      summary: "VM 層がマップ済み領域・連続ページ群を<b>各 8 バイトに圧縮</b>して持つビットフィールド <t>POD</t>。アドレスとページ数・フラグをパックする。<code>static_assert</code> で 8 バイトを保証。",
      when: "VM 層の内部記述子。通常は直接触らないが、LRU キャッシュ等の構造を理解する際に参照する。",
      sample: "static_assert(sizeof(FMappedT) == 8);\nstatic_assert(sizeof(FPageT)   == 8);",
      members: [
        { sig: "u64 packed_virtual_addr : 44 / page_count : 16 / sparse : 1 / misc : 3", desc: "<code>FMappedT</code>: マップ領域の仮想アドレス・ページ数・スパース/その他フラグ。" },
        { sig: "u64 continuous_page_count : 12 / misc : 4 / packed_virtual_addr : 48", desc: "<code>FPageT</code>: 連続ページ数・フラグ・ページ整列アドレス。" }
      ]
    },
    {
      name: "FMemorySnapshot",
      kind: "クラス(静的)", header: "memory/MemorySnapshot.h",
      summary: "<t>FMemorySystem</t> の現状(セグメント別の予約/使用/予算)を <b>SVG / BMP / コンソール</b>に可視化出力するユーティリティ。全メソッド static、外部依存ゼロ(自前ライタ)。各セグメントを 1 行のバーとして描画する。",
      when: "メモリ使用率を人間が読める形で書き出してデバッグ/プロファイルしたい時。SVG はブラウザで開け、BMP は外部ツールに流せる。",
      sample: "FMemorySnapshot::WriteSvg(L\"mem.svg\");   // 人間可読・ラベル付き\nFMemorySnapshot::WriteBmp(L\"mem.bmp\");   // 24bpp 画像\nFMemorySnapshot::DumpToStdOut();          // ターミナルへテキスト",
      members: [
        { sig: "static TResult&lt;void&gt; WriteSvg(const wchar_t* path, u32 width = 800, u32 row_height = 40) noexcept", ret: "成否", desc: "SVG ファイルを出力する(ラベル付き、ブラウザで開ける)。" },
        { sig: "static TResult&lt;void&gt; WriteBmp(const wchar_t* path, u32 width = 800, u32 row_height = 30) noexcept", ret: "成否", desc: "24bpp の BMP 画像を出力する(外部依存ゼロ)。" },
        { sig: "static void DumpToStdOut() noexcept", desc: "コンソール(stdout)へテキストで使用率を出力する。" }
      ]
    },
    {
      name: "FMemorySystemConfig / FSegmentConfig / FSegmentStats",
      kind: "構造体", header: "memory/MemorySystem.h",
      summary: "<t>FMemorySystem</t> の設定・統計を運ぶ <t>POD</t> 群。<b>FSegmentConfig</b> はセグメント種別・要求量ハード予算・frame arena 使用を指定する。<b>FMemorySystemConfig</b> は全セグメント設定と既定アロケータへの導入を指定する。<b>FSegmentStats</b> は要求量・ピーク・予算・未解放件数を返す。",
      when: "<code>FMemorySystem::Init</code> に渡す設定の組み立てや、<code>GetStats</code> の結果受け取りに使う。",
      sample: "FMemorySystemConfig configuration = FMemorySystem::DefaultConfig();\nconfiguration.install_as_default_allocator = true;\nconfiguration.segments[(usize)ESegment::Resource].hard_budget_bytes = 1ull &lt;&lt; 30;\nFMemorySystem::Init(configuration);\n\nSegmentStats statistics[(int)ESegment::_Count];\nu32 count = FMemorySystem::GetStats(statistics, 5);\nfor (u32 index = 0; index &lt; count; ++index) { /* requested_bytes / hard_budget_bytes */ }",
      members: [
        { sig: "FSegmentConfig: ESegment segment; usize hard_budget_bytes; bool use_frame_allocator", desc: "通常は mimalloc、<code>use_frame_allocator=true</code> なら一括 Reset する arena を使う。ハード予算超過は確保失敗になる。" },
        { sig: "FMemorySystemConfig: FSegmentConfig segments[ESegment::_Count]; bool install_as_default_allocator = false", desc: "<code>install_as_default_allocator=true</code> なら Init 時に Default セグメントを既定アロケータへ据え、Shutdown で元へ復元する。" },
        { sig: "FSegmentStats: ESegment segment; const char* segment_name / allocator_name; u64 requested_bytes / peak_requested_bytes / hard_budget_bytes / outstanding_allocation_count", desc: "セグメント別の要求量・ピーク・予算・未解放件数。<code>GetStats</code> で取得。" }
      ]
    },
    {
      name: "AlignUp / IsPow2 / kDefaultAlignment",
      kind: "関数 / 定数", header: "memory/Allocator.h",
      summary: "アロケータ実装で多用する整列ヘルパ。<b>AlignUp</b>=値/ポインタを 2 のべき乗の倍数へ切り上げ。<b>IsPow2</b>=2 のべき乗判定。<b>kDefaultAlignment</b>=既定整列(SIMD 16B と一般 8B の妥協で 16)。",
      when: "独自アロケータやバッファ計算で整列を扱う時。<code>AlignUp</code> の <code>a</code> は必ず 2 のべき乗(Debug で <code>ACS_ASSERT</code>)。",
      sample: "usize n = AlignUp(13, 16);        // 16\nvoid* q = AlignUp(p, 64);         // 64B 境界へ\nif (IsPow2(align)) { /* OK */ }\nusize a = kDefaultAlignment;      // 16",
      members: [
        { sig: "constexpr usize kDefaultAlignment", desc: "既定アライメント(<code>alignof(void*)</code> が 8 超ならそれ、でなければ 16)。実質 16。" },
        { sig: "bool IsPow2(usize v) noexcept", ret: "2 のべき乗か", desc: "<code>v != 0 &amp;&amp; (v &amp; (v-1)) == 0</code>。" },
        { sig: "usize AlignUp(usize n, usize a) noexcept", ret: "切り上げ値", desc: "<code>n</code> を <code>a</code>(2 のべき乗)の倍数へ切り上げる。" },
        { sig: "void* AlignUp(void* p, usize a) noexcept", ret: "切り上げポインタ", desc: "ポインタ版。<code>a</code> 境界へ切り上げる。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "TSharedPtr": "複数で共有する<t>スマートポインタ</t>。<t>参照カウント</t>が 0 で自動解放。",
  "TWeakPtr": "<t>TSharedPtr</t> への<t>弱参照</t>。寿命を延ばさず生死だけ見る。",
  "FObject": "参照カウント管理されるオブジェクトの基底。生ポインタからも強/弱参照を作れる。",
  "DefaultAllocator": "既定の<t>アロケータ</t>。指定なしの確保で使われる。",
  "セグメント": "メモリを目的・寿命で分けた論理ヒープ(Default/Permanent/Temp/Resource/Develop)。<code>FMemorySystem</code> が各セグメントに独立した予算を持つ。",
  "FVmReservation": "仮想アドレス空間を予約し、必要なページだけ物理コミットする <t>RAII</t> ハンドル。Decommit は総量上限付き LRU キャッシュ経由で実 VirtualFree を遅延する。",
  "Non-Temporal": "キャッシュ(L1/L2)を経由せずメモリへ直接読み書きする命令。大きなバッファのゼロ化/コピーでキャッシュ汚染を避けられる。"
});
