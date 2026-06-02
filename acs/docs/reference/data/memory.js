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
      sample: "class FEnemy : public FObject {\npublic:\n    int hp = 100;\n};\nTObjectPtr&lt;FEnemy&gt; e = NewObject&lt;FEnemy&gt;();\nFEnemy* raw = e.Get();\nTWeakObjectPtr&lt;FEnemy&gt; w(raw);  // 生ポインタから弱参照が作れる",
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
        { sig: "void* Alloc(usize size, usize align, FSourceLoc)", ret: "確保した領域", desc: "指定サイズ・整列でメモリを確保。失敗時 null。" },
        { sig: "void Free(void* p)", desc: "<code>Alloc</code> で得た領域を解放。" },
        { sig: "u64 BytesAllocated() const", ret: "確保中バイト数", desc: "現在確保中の合計(実装による)。リーク調査に。" }
      ]
    },
    {
      name: "DefaultAllocator()",
      kind: "関数", header: "memory/Allocator.h",
      summary: "既定の<t>アロケータ</t>(<t>ヒープ</t>確保)を返す。<code>MakeShared</code> 等が指定なしの時に使う。",
      when: "特別な事情がなければこれでよい。", sample: "FAllocator& a = DefaultAllocator();"
    },
    {
      name: "FArenaAllocator",
      kind: "クラス", header: "memory/ArenaAllocator.h",
      summary: "確保はポインタを進めるだけ(超高速)、解放は<b>まとめてリセット</b>のみ、という使い捨て<t>アロケータ</t>。",
      when: "1 フレームだけ使って次フレームに全部捨てる一時データ(描画コマンド等)。個別解放はしない。",
      sample: "FArenaAllocator arena(1u &lt;&lt; 20);  // 1MB\nvoid* a = arena.Alloc(64, 16, FSourceLoc::Current());\n// ...フレーム処理...\narena.Reset();                  // 全部まとめて解放(超高速)",
      members: [
        { sig: "void* Alloc(size, align, loc)", desc: "末尾にポインタを進めて確保。" },
        { sig: "void Reset()", desc: "全確保を一括で無効化(個別 Free は不要)。" }
      ]
    },
    {
      name: "FLinearAllocator / FPoolAllocator",
      kind: "クラス", header: "memory/LinearAllocator.h / PoolAllocator.h",
      summary: "<b>Linear</b>=前から詰める単純高速アロケータ(Arena に近い)。<b>Pool</b>=同じサイズの塊を大量に出し入れする用途に最適化したアロケータ。",
      when: "Linear: 連続確保して一括破棄。Pool: 弾・パーティクル等、同サイズを高速に取り回す時。",
      sample: "FPoolAllocator pool(sizeof(FBullet), 1024); // 1024 個分\nvoid* b = pool.Alloc(...);\npool.Free(b);"
    },
    {
      name: "FTlsfAllocator / FShardedTlsfAllocator",
      kind: "クラス", header: "memory/Tlsf.h / ShardedTlsf.h",
      summary: "<b>TLSF</b>=任意サイズを高速・低断片化で確保できる汎用<t>アロケータ</t>。<b>Sharded</b>=スレッドごとに分割し、複数<t>スレッド</t>から同時に確保しても競合しにくくした版。",
      when: "汎用の高速確保が欲しい時。マルチスレッドで確保が多いなら Sharded。",
      sample: "// 既定アロケータの内部などで使われる。直接使う時:\nFTlsfAllocator tlsf(1u &lt;&lt; 24);  // 16MB プール\nvoid* p = tlsf.Alloc(200, 16, FSourceLoc::Current());"
    },
    {
      name: "FRelocatableAllocator",
      kind: "クラス", header: "memory/RelocatableAllocator.h",
      summary: "確保した領域を後で<b>動かして詰め直せる</b>(<t>ハンドル</t>越しにアクセス)<t>アロケータ</t>。断片化を解消できる。",
      when: "長時間動かし続けてメモリ断片化が問題になる場面で、コンパクションしたい時。生ポインタでなくハンドルで持つ。",
      sample: "FRelocatableAllocator ra(1u &lt;&lt; 20);\nFRelocHandle h = ra.Alloc(128);\nvoid* p = ra.Resolve(h);   // 使う直前に解決\nra.Compact();              // 隙間を詰める(ポインタは無効化、ハンドルは有効)"
    },
    {
      name: "FMemorySystem",
      kind: "クラス", header: "memory/MemorySystem.h",
      summary: "用途別の領域(セグメント)・予算・スナップショットを束ねる、エンジン全体のメモリ管理の入口。",
      when: "メモリ使用量の可視化・上限管理・領域分割をまとめて行いたい時。",
      sample: "// 既定構成を作って使う\nauto sys = FMemorySystem::make_default();\n// セグメント別に確保/集計できる"
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "TSharedPtr": "複数で共有する<t>スマートポインタ</t>。<t>参照カウント</t>が 0 で自動解放。",
  "TWeakPtr": "<t>TSharedPtr</t> への<t>弱参照</t>。寿命を延ばさず生死だけ見る。",
  "FObject": "参照カウント管理されるオブジェクトの基底。生ポインタからも強/弱参照を作れる。",
  "DefaultAllocator": "既定の<t>アロケータ</t>。指定なしの確保で使われる。"
});
