/* ACS リファレンス — ecs モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > は &lt; &gt;。 */
ACS_REF.modules.push({
  id: "ecs",
  order: 20,
  title: "ecs — エンティティ・コンポーネント・システム",
  blurb: "ゲームの登場物を<t>エンティティ</t>(ただの ID)と、それにくっつけた<t>コンポーネント</t>(位置・速度などのデータ)で表す仕組みです。データを型ごとにまとめて持つので、<b>大量の物体を高速に走査</b>できます。",
  types: [
    {
      name: "FEntityId",
      kind: "構造体", header: "ecs/Entity.h",
      summary: "1 つの<t>エンティティ</t>を指す軽い識別子。中身は <code>index</code>(スロット番号)と <code>generation</code>(<t>世代</t>番号)の組。スロットが再利用されても世代が違うので、<b>古い ID を使うと検出できる</b>(<t>世代付きハンドル</t>)。",
      when: "キャラや弾など『1 つの実体』を指して持ち回る時。ポインタ代わりの安全な軽量参照として使う。",
      sample: "FEntityId e = world.Create();\nif (e.IsValid()) { /* 有効な ID */ }\nif (e == kInvalidEntity) { /* 無効 */ }",
      members: [
        { sig: "u32 index", desc: "<code>FWorld</code> 内のスロット番号。初期値は無効を表す <code>0xFFFFFFFF</code>。" },
        { sig: "u32 generation", desc: "<t>世代</t>番号。同じ <code>index</code> が再利用されるたびに +1 され、古い ID と区別する。" },
        { sig: "bool IsValid() const", ret: "有効か", desc: "<code>index</code> が無効値でなければ true。" },
        { sig: "bool operator==(FEntityId) const / operator!=", desc: "<code>index</code> と <code>generation</code> の両方が一致して初めて同一とみなす。" }
      ]
    },
    {
      name: "kInvalidEntity",
      kind: "定数", header: "ecs/Entity.h",
      summary: "無効な <t>FEntityId</t>。戻り値や初期値の『該当なし』を表す番兵。",
      when: "ID を返す関数が『見つからなかった』を示す時や、未代入の初期値として使う。",
      sample: "FEntityId target = kInvalidEntity;\n// ...探索...\nif (target == kInvalidEntity) return;  // 見つからなかった"
    },
    {
      name: "FWorld",
      kind: "クラス", header: "ecs/World.h",
      summary: "<t>エンティティ</t>と<t>コンポーネント</t>を束ねる ECS の中心。エンティティの生成/破棄、コンポーネントの追加/取得/削除、<t>クエリ</t>反復をすべてここから行う。コピー不可。",
      when: "ECS でゲーム状態を持つなら最初に 1 つ作る入れ物。ステージやシーン単位で 1 つ持つのが基本。",
      sample: "FWorld w;\nFEntityId e = w.Create();\nw.Add&lt;Position&gt;(e, {0, 0, 0});\nw.Add&lt;Velocity&gt;(e, {1, 0, 0});\nif (Position* p = w.Get&lt;Position&gt;(e)) p-&gt;x += 1.0f;\nw.Destroy(e);",
      members: [
        { sig: "FEntityId Create()", ret: "新しい ID", desc: "<t>エンティティ</t>を 1 つ作る。空きスロットがあれば再利用する(<t>世代</t>は <code>Destroy</code> 時に進むので、再利用された ID は古い ID と区別できる)。", when: "新しいキャラや弾を出す瞬間。" },
        { sig: "void Destroy(FEntityId e)", desc: "エンティティを消す。付いていた全<t>コンポーネント</t>も削除し、スロットを空きに戻す。", when: "キャラの死亡・退場時。" },
        { sig: "bool IsAlive(FEntityId e) const", ret: "生存中か", desc: "その ID が今も有効か(<t>世代</t>一致 + 生存中)を返す。古い ID なら false。", when: "保存しておいた ID を使う前の安全確認に。" },
        { sig: "void Add<T>(FEntityId e, T value)", desc: "エンティティに<t>コンポーネント</t> T を付ける。既にあれば上書き。エンティティが死んでいれば何もしない。", sample: "w.Add&lt;Health&gt;(e, {100});" },
        { sig: "T* Get<T>(FEntityId e)", ret: "値ポインタ or null", desc: "コンポーネント T を取り出す。付いていなければ null。const 版もある。", when: "1 体だけ狙って値を読み書きする時。", sample: "if (auto* h = w.Get&lt;Health&gt;(e)) h-&gt;hp -= 10;" },
        { sig: "bool Has<T>(FEntityId e) const", ret: "所持しているか", desc: "コンポーネント T を持っているかを返す(<code>Get&lt;T&gt;(e) != nullptr</code>)。" },
        { sig: "void Remove<T>(FEntityId e)", desc: "コンポーネント T を外す。無くても安全(何もしない)。" },
        { sig: "auto Query<Comps...>()", ret: "TQueryView", desc: "指定した全コンポーネントを持つエンティティをまとめて反復する<t>クエリ</t>を返す。実装は Query.h。", when: "『位置と速度を持つ全員』のように条件で一括処理する時。", sample: "w.Query&lt;Position, Velocity&gt;().Each([](FEntityId, Position&amp; p, Velocity&amp; v){ p.x += v.x; });" },
        { sig: "u32 EntityCount() const", ret: "生存数", desc: "今 FWorld にいる生きたエンティティの数。" },
        { sig: "FEntityId MakeIdFromIndex(u32 index) const", ret: "FEntityId", desc: "スロット番号だけから世代を補って <code>FEntityId</code> を復元する。主に <t>クエリ</t>内部用。" }
      ]
    },
    {
      name: "TQueryView&lt;Comps...&gt;",
      kind: "クラステンプレート", header: "ecs/Query.h",
      summary: "<code>FWorld::Query&lt;A, B, ...&gt;()</code> が返す反復ヘルパ。指定した<b>全</b>コンポーネントを持つエンティティだけを選び、各々に<t>ラムダ</t>を適用する。内部では一番要素の少ない集合を主軸にして効率良く絞り込む。",
      when: "毎フレームの処理(移動・当たり判定・描画準備など)を『該当する全エンティティ』に対して回す時。ECS の主役。",
      sample: "w.Query&lt;Position, Velocity&gt;().Each(\n    [dt](FEntityId id, Position&amp; p, Velocity&amp; v) {\n        p.x += v.x * dt;\n        p.y += v.y * dt;\n    });",
      members: [
        { sig: "void Each(Fn fn)", desc: "条件に合う各エンティティへ <code>fn(FEntityId, Comps&amp;...)</code> を呼ぶ。反復前に dense 列を<b>スナップショット</b>するので、<code>fn</code> 内で Add/Remove しても安全。", when: "通常の毎フレーム処理。", sample: "w.Query&lt;Health&gt;().Each([](FEntityId e, Health&amp; h){ if (h.hp &lt;= 0) {/* 死亡処理 */} });" },
        { sig: "void EachParallel(Fn fn, u32 grain = 1024)", desc: "<t>FThreadPool</t> で chunk 分割し<b>並列</b>に <code>fn</code> を呼ぶ。各エンティティは独立に処理される前提。共有資源を触るなら自分で同期する。<code>grain</code> は 1 chunk の最小エンティティ数。", when: "エンティティ数が非常に多く、各処理が独立しているとき。", sample: "w.Query&lt;Position, Velocity&gt;().EachParallel(\n    [](FEntityId, Position&amp; p, Velocity&amp; v){ p.x += v.x; }, 1024);" }
      ]
    },
    {
      name: "SystemFn",
      kind: "型エイリアス", header: "ecs/System.h",
      summary: "システム関数の型 <code>void(*)(FWorld&amp;, f32 dt)</code>。<code>FWorld</code> と<t>デルタタイム</t>を受け取り、毎フレーム呼ばれる処理の単位。",
      when: "移動・AI・物理など『毎フレームやること』を 1 つの関数にまとめ、スケジューラに登録する時。",
      sample: "void MovementSystem(FWorld&amp; w, f32 dt) {\n    w.Query&lt;Position, Velocity&gt;().Each(\n        [dt](FEntityId, Position&amp; p, Velocity&amp; v){ p.x += v.x * dt; });\n}"
    },
    {
      name: "FSystemScheduler",
      kind: "クラス", header: "ecs/System.h",
      summary: "<t>システム</t>関数(<t>SystemFn</t>)を登録し、毎フレームまとめて順番に呼ぶ実行器。登録順に実行される。",
      when: "ゲームループの中で複数のシステム(移動→当たり判定→描画準備…)を一括で回したい時。",
      sample: "FSystemScheduler sched;\nsched.Add(&amp;MovementSystem);\nsched.Add(&amp;CollisionSystem);\nwhile (running) {\n    sched.Tick(world, dt);  // 全システムを順に実行\n}",
      members: [
        { sig: "void Add(SystemFn fn)", desc: "システム関数を末尾に登録する。実行はこの登録順。" },
        { sig: "void Tick(FWorld& world, f32 dt)", desc: "登録された全システムを順に 1 回ずつ呼ぶ。<code>dt</code> は前フレームからの経過秒。", when: "毎フレーム 1 回呼ぶ。" },
        { sig: "void Clear()", desc: "登録済みシステムを全消去する。" },
        { sig: "usize Count() const", ret: "登録数", desc: "今登録されているシステムの数を返す。" }
      ]
    },
    {
      name: "ComponentTypeId / GetComponentTypeId&lt;T&gt;()",
      kind: "型エイリアス / 関数テンプレート", header: "ecs/ComponentId.h",
      summary: "<t>コンポーネント</t>型ごとに一意な番号(<code>u32</code>)を割り当てる仕組み。<code>GetComponentTypeId&lt;T&gt;()</code> は型 T に固有の番号を返し、ストレージ配列の添字に使う。初回呼び出しで割り当て、以降は同じ値(<t>スレッド</t>安全)。",
      when: "通常ユーザーが直接触ることは少なく、<code>FWorld</code> 内部がコンポーネントの保管場所を引くために使う。低レベルに自前管理したい時のみ。",
      sample: "ComponentTypeId pid = GetComponentTypeId&lt;Position&gt;();\nComponentTypeId vid = GetComponentTypeId&lt;Velocity&gt;();\n// pid != vid（型ごとに別番号）",
      members: [
        { sig: "constexpr ComponentTypeId kMaxComponentTypes = 256", desc: "扱えるコンポーネント型の上限(0..255)。" },
        { sig: "ComponentTypeId GetComponentTypeId<T>()", ret: "型固有の番号", desc: "型 T に対応する番号を返す。初めて呼んだ型に新しい番号を振る。" }
      ]
    },
    {
      name: "FComponentOps",
      kind: "構造体", header: "ecs/ComponentRegistry.h",
      summary: "<b>型を消去した</b>コンポーネント操作の束。サイズ・整列・破棄関数・<t>ムーブ</t>構築関数・名前を実行時に問い合わせられる形で保持する(<t>型消去</t>)。",
      when: "コンパイル時に型を知らないコード(汎用ストレージなど)が、実行時にコンポーネントを破棄・移動したい時に参照する。低レベル機構。",
      sample: "const FComponentOps&amp; ops = FComponentRegistry::Register&lt;Position&gt;();\nvoid* slot = /* 確保した領域 */;\nops.destroy(slot);  // 型を知らずに ~Position() を呼ぶ",
      members: [
        { sig: "usize size / usize alignment", desc: "コンポーネント 1 個分のバイト数と整列要求。" },
        { sig: "void (*destroy)(void* p)", desc: "対象の<t>デストラクタ</t>を呼ぶ関数ポインタ(<code>~T()</code>)。" },
        { sig: "void (*move)(void* dst, void* src)", desc: "<code>src</code> から <code>dst</code> へ<t>ムーブ</t>構築する関数ポインタ。" },
        { sig: "const char* name", desc: "型名(既定は固定文字列)。" }
      ]
    },
    {
      name: "FComponentRegistry",
      kind: "クラス", header: "ecs/ComponentRegistry.h",
      summary: "コンポーネント型ごとの操作(<t>FComponentOps</t>)を登録・引き出す静的レジストリ。型 T を 1 度登録すれば、以後は番号から型を知らずに操作を取り出せる。",
      when: "型消去したコンポーネント管理を自作する時の土台。通常は <code>FWorld</code> の内側で使われる。",
      sample: "const FComponentOps&amp; ops = FComponentRegistry::Register&lt;Velocity&gt;();\nComponentTypeId id = GetComponentTypeId&lt;Velocity&gt;();\nconst FComponentOps&amp; same = FComponentRegistry::Get(id);  // 同じ Ops",
      members: [
        { sig: "static const FComponentOps& Register<T>()", ret: "型 T の Ops", desc: "型 T を登録(初回のみ実体作成)し、その <code>FComponentOps</code> を返す。", when: "新しいコンポーネント型を初めて使う前に。" },
        { sig: "static const FComponentOps& Get(ComponentTypeId id)", ret: "Ops", desc: "番号から登録済みの <code>FComponentOps</code> を引く。" }
      ]
    },
    {
      name: "FSparseSetBase",
      kind: "基底クラス", header: "ecs/SparseSet.h",
      summary: "<t>TSparseSet</t> の型に依らない共通部分。エンティティ番号 → 密配列上の位置の対応や、要素数・dense 走査を提供する。<code>FWorld</code> が型を知らずに集合を扱うための基底。",
      when: "ふつう直接使わない。<code>FWorld</code> がコンポーネント型を区別せず保管・破棄するための内部基盤。",
      sample: "// FWorld 内部で FSparseSetBase* として保持される\nFSparseSetBase* s = /* ... */;\nif (s-&gt;Contains(e.index)) { usize n = s-&gt;Size(); }",
      members: [
        { sig: "constexpr u32 kInvalid = 0xFFFFFFFF", desc: "未登録を表す番兵値。" },
        { sig: "u32 IndexOf(u32 entity_index) const", ret: "dense 位置 or kInvalid", desc: "そのエンティティの密配列上の位置。未登録なら <code>kInvalid</code>。" },
        { sig: "bool Contains(u32 entity_index) const", ret: "登録済みか", desc: "そのエンティティが値を持っているか。" },
        { sig: "usize Size() const", ret: "要素数", desc: "この集合に入っているエンティティの数。" },
        { sig: "const u32* DenseEntities() const", ret: "密配列", desc: "登録エンティティ番号を詰めた密配列。<t>クエリ</t>の線形走査に使う。" },
        { sig: "virtual void RemoveErased(u32 entity_index)", desc: "型を知らずにそのエンティティの値を削除する(<code>FWorld::Destroy</code> 用)。" }
      ]
    },
    {
      name: "TSparseSet&lt;T&gt;",
      kind: "クラステンプレート", header: "ecs/SparseSet.h",
      summary: "エンティティ番号 → コンポーネント値 T の高速対応表(<t>Sparse Set</t>)。Add/Remove/Contains が <b>O(1)</b> で、全要素は<b>密配列</b>に並ぶため線形走査がキャッシュに優しい。<t>FSparseSetBase</t> を継承。",
      when: "1 種類のコンポーネントを大量のエンティティに対して高速に出し入れ・全走査したい時。<code>FWorld</code> がコンポーネント型ごとに 1 つ内部保有する。",
      sample: "TSparseSet&lt;Position&gt; positions;\npositions.Add(e.index, {1, 2, 3});\nif (Position* p = positions.Get(e.index)) p-&gt;x += 1;\npositions.Remove(e.index);",
      members: [
        { sig: "void Add(u32 entity_index, T value)", desc: "そのエンティティに値を追加。既にあれば上書き。", when: "コンポーネントを付ける時。" },
        { sig: "void Remove(u32 entity_index)", desc: "値を削除。末尾要素と入れ替えて O(1) で詰める(順序は保たれない)。" },
        { sig: "T* Get(u32 entity_index)", ret: "値ポインタ or null", desc: "値へのポインタ。未登録なら null。const 版もある。" },
        { sig: "T* Data()", ret: "値の密配列", desc: "全コンポーネント値の生配列(dense 順)。一括走査に使う。const 版あり。" },
        { sig: "void RemoveErased(u32 entity_index)", desc: "基底経由で型を知らずに削除する実装(内部は <code>Remove</code>)。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "エンティティ": "ゲームの登場物を表すただの ID(番号)。データ自体は持たず、<t>コンポーネント</t>を付けて意味を与える。",
  "コンポーネント": "<t>エンティティ</t>にくっつける 1 種類のデータ(位置・速度・体力など)。型ごとにまとめて保管される。",
  "システム": "<t>コンポーネント</t>を持つエンティティ群を毎フレーム処理する関数(移動・AI など)。<t>FSystemScheduler</t> で順に実行する。",
  "クエリ": "条件(指定コンポーネントを全部持つ)に合うエンティティを選んで反復する仕組み。ECS の主役。",
  "世代": "同じスロット番号が再利用されたかを区別する番号。古い<t>FEntityId</t>を検出するのに使う。",
  "世代付きハンドル": "番号 + <t>世代</t>で構成される安全な参照。スロット再利用後の古い参照を無効と判定できる。",
  "Sparse Set": "疎な参照表と密な値配列を組み合わせ、追加/削除/検索を O(1)、全走査をキャッシュ効率良くするデータ構造。",
  "TSparseSet": "ACS の <t>Sparse Set</t> 実装。エンティティ番号→コンポーネント値の高速対応表。",
  "型消去": "コンパイル時の型情報を隠し、サイズや破棄/移動操作だけを実行時に扱えるようにする手法。",
  "FEntityId": "1 つの<t>エンティティ</t>を指す軽量な識別子。<code>index</code> と <code>generation</code> の組。",
  "FComponentOps": "型を消去したコンポーネントのサイズ・破棄・<t>ムーブ</t>などの操作束。",
  "デルタタイム": "前フレームからの経過時間(秒)。フレームレートに依らない動きの計算に使う。",
  "FThreadPool": "複数<t>スレッド</t>に仕事を分配する実行基盤。<code>ParallelFor</code> で範囲を並列処理する。"
});
