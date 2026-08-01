/* ACS リファレンス — mvvm モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "mvvm",
  order: 24,
  title: "mvvm — データバインディング (MVVM)",
  blurb: "値が変わったら自動で UI に反映させる仕組み(<t>MVVM</t>)を提供します。<code>TObservable&lt;T&gt;</code> に値を入れて <code>Set</code> すると、それを<t>購読</t>している UI やロジックへ通知が飛びます。",
  types: [
    {
      name: "TObservable&lt;T&gt;",
      kind: "クラステンプレート", header: "mvvm/Observable.h",
      summary: "<b>監視できる 1 個の値</b>(MVVM の中核)。<code>Set</code> で値を変えると、変わった時だけ<t>購読</t>者へ通知が飛ぶ。通知中の解除に加え、nested 通知中に listener が所有 object ごと TObservable 自身を同期破棄するケースも stack lifetime guard で検出し、破棄済み member へ触れず残りの通知を停止する。同じ storage に新実体を placement-new しても old Notify は新しい frame chain へ侵入しない。owner を破棄する callback は破棄を最後の操作にし、その後 new_value、user、owner を再参照せず直ちに return する必要がある。UI thread 専用・コピー禁止。",
      when: "HP やレベルなど「変わったら画面に反映したい」値を持たせる時。<t>FViewModel</t> のメンバとして公開するのが基本形。",
      sample: "TObservable&lt;f32&gt; hp { 100.0f };         // 初期値つき\nauto h = hp.Subscribe([](const f32&amp; v, void*){\n    ACS_LOG_INFO(\"HP=%.1f\", v);              // 値が変わるたび呼ばれる\n}, nullptr);\nhp.Set(75.0f);                              // → 通知発火\nhp.Unsubscribe(h);                          // 監視解除",
      members: [
        { sig: "explicit TObservable(T initial)", desc: "初期値を与えて作る。引数なしなら <code>T{}</code> から開始。" },
        { sig: "const T& Get() const", ret: "現在値", desc: "今の値を読む。<code>operator const T&amp;()</code> で暗黙変換もできる。" },
        { sig: "void Set(const T& v) / Set(T&& v)", desc: "値を変更する。<b>前と違う値の時だけ</b>(<code>operator==</code> で判定)<t>購読</t>者へ通知する。", when: "値を更新して UI に反映させたい時。毎フレーム同値を入れても無駄な通知は飛ばない。" },
        { sig: "void ForceNotify()", desc: "値が同じでも強制的に通知する。中の<t>コンテナ</t>を直接書き換えた後などに使う。" },
        { sig: "FObservableHandle Subscribe(Listener cb, void* user)", ret: "購読ハンドル", desc: "値変更時に呼ばれる<t>コールバック</t>を登録する。<code>cb</code> は <code>void(const T&amp;, void*)</code> の関数ポインタ(捕獲なし<t>ラムダ</t>可)。callback が所有 object ごとこの TObservable を同期破棄した場合は、その callback 復帰後に通知を終了し、後続 listener は呼ばない。破棄は callback の最後に行い、破棄後は new_value、user、owner を再参照せず直ちに return する。", sample: "auto h = hp.Subscribe(&amp;OnHpChanged, this);" },
        { sig: "bool Unsubscribe(FObservableHandle h)", ret: "解除できたか", desc: "登録した監視を外す。通知の最中でも安全(遅延キャンセルされる)。" },
        { sig: "u32 SubscriberCount() const", ret: "購読者数", desc: "今アクティブな<t>購読</t>者の数。主にデバッグ用。" }
      ]
    },
    {
      name: "FObservableHandle",
      kind: "構造体", header: "mvvm/Observable.h",
      summary: "<code>Subscribe</code> が返す<b>購読チケット</b>。これを <code>Unsubscribe</code> に渡すと監視を外せる。<code>id</code> と <code>generation</code> で「同じ枠の使い回し」を誤判定しないようになっている。",
      when: "登録した監視を後で解除するために保持しておく。",
      sample: "FObservableHandle h = hp.Subscribe(cb, this);\nif (h.IsValid()) hp.Unsubscribe(h);",
      members: [
        { sig: "bool IsValid() const", ret: "有効か", desc: "<code>id != 0</code> なら有効なハンドル。" },
        { sig: "bool operator==(const FObservableHandle&) const", desc: "<code>id</code> と <code>generation</code> の両方が一致すれば同じハンドル。" },
        { sig: "u32 id / u64 generation", desc: "枠番号と世代。直接触る必要はほぼない。" }
      ]
    },
    {
      name: "kInvalidObservable",
      kind: "定数", header: "mvvm/Observable.h",
      summary: "無効な <code>FObservableHandle</code> を表す定数(<code>id==0</code>)。<code>Subscribe</code> が失敗した時に返る値でもある。",
      when: "ハンドルが未設定/無効かを比較で判定したい時。",
      sample: "FObservableHandle h = kInvalidObservable;\n// ... 後で h = obs.Subscribe(...);\nif (h == kInvalidObservable) { /* まだ未登録 */ }"
    },
    {
      name: "FViewModel",
      kind: "基底クラス", header: "mvvm/ViewModel.h",
      summary: "<t>MVVM</t> の中央 <b>VM(ビューモデル)</b> の基底。中身はあえて空で、命名と意図だけを与える。継承して <code>TObservable&lt;T&gt;</code> のプロパティを並べて公開する。",
      when: "UI に見せたいデータ群を 1 つの「画面の状態」としてまとめたい時。これを継承して各値を <code>TObservable</code> で持つ。",
      sample: "class FPlayerViewModel : public FViewModel {\npublic:\n    TObservable&lt;f32&gt;  hp    { 100.0f };\n    TObservable&lt;i32&gt;  level { 1 };\n    TObservable&lt;bool&gt; invincible { false };\n};\nFPlayerViewModel vm;\nvm.hp.Set(vm.hp.Get() - 10.0f);   // View 側へ自動反映",
      members: [
        { sig: "virtual ~FViewModel()", desc: "<t>デストラクタ</t>は仮想。派生 VM を基底ポインタで安全に破棄できる。" }
      ]
    },
    {
      name: "FCommand",
      kind: "クラス", header: "mvvm/Command.h",
      summary: "<b>VM 側に置くアクション</b>(ボタン押下などの処理)。実行関数と任意の <code>can_execute</code> 条件を持つ。条件が false の間は <code>Execute</code> が何もしない。",
      when: "「攻撃」「リスポーン」などのボタン動作を VM に持たせ、UI からは押すだけにしたい時。ImGui の <code>BindCommand</code> でボタン化+自動グレーアウトできる。",
      sample: "FCommand attack {\n    [](void* u){ static_cast&lt;FPlayerViewModel*&gt;(u)-&gt;hp.Set(0.0f); },\n    this\n};\n// 条件付き: is_dead が true の時だけ実行可\nTObservable&lt;bool&gt; is_dead { false };\nFCommand revive {\n    [](void* u){ static_cast&lt;FPlayerViewModel*&gt;(u)-&gt;hp.Set(100.0f); },\n    this, &amp;is_dead\n};",
      members: [
        { sig: "FCommand(ExecFn fn, void* user)", desc: "常時実行可能なコマンドを作る。<code>fn</code> は <code>void(void*)</code> の関数ポインタ。" },
        { sig: "FCommand(ExecFn fn, void* user, TObservable<bool>* can_execute)", desc: "<code>can_execute</code> が true の間だけ実行できる条件付きコマンドを作る。<code>TObservable&lt;bool&gt;</code> の<b>生存期間は呼び出し側の責任</b>。" },
        { sig: "void Execute()", desc: "コマンドを実行する。条件が不成立なら何もしない(no-op)。" },
        { sig: "bool CanExecute() const", ret: "実行可能か", desc: "今実行できる状態か。UI のグレーアウト判定に使う。" },
        { sig: "void Invalidate()", desc: "内部関数を無効化する。VM の<t>デストラクタ</t>で呼ぶと、後から残った参照経由の誤実行を防げる。" },
        { sig: "TObservable<bool>* CanExecuteSource() const", ret: "条件 Observable", desc: "<code>can_execute</code> に使っている <code>TObservable&lt;bool&gt;</code> を返す。<code>BindCommand</code> がこれを監視する。" }
      ]
    },
    {
      name: "TTwoWayBinder&lt;T&gt;",
      kind: "クラステンプレート", header: "mvvm/Binder.h",
      summary: "2 つの <code>TObservable&lt;T&gt;</code> を<b>双方向に同期</b>させる。片方を <code>Set</code> するともう片方も同じ値になり、再帰更新は内部フラグで防ぐ。伝播先 <code>Set</code> の listener が binder を同期破棄しても stack lifetime guard が検出し、復帰後に破棄済みフラグへ書き込まない。同じ storage への placement-new も旧 callback と寿命分離する。生成時は <code>a</code> の値を <code>b</code> に初期反映する。",
      when: "View 側の値と Model 側の値を、どちらを変えても揃えたい時(スライダ↔データなど)。破棄されると同期は自動で外れる。伝播先 listener から binder の owner を破棄する場合、その破棄を callback の最後の操作にする。",
      sample: "TObservable&lt;f32&gt; view_hp;\nTObservable&lt;f32&gt; model_hp { 100.0f };\nTTwoWayBinder&lt;f32&gt; bind(view_hp, model_hp); // 双方向\nmodel_hp.Set(50.0f);   // → view_hp も 50\nview_hp.Set(0.0f);     // → model_hp も 0",
      members: [
        { sig: "TTwoWayBinder(TObservable<T>& a, TObservable<T>& b)", desc: "2 つを双方向に結ぶ。<code>a</code> の値を <code>b</code> へ初期コピーしてから両方を購読する。伝播中に同期破棄された callback は guard だけを確認して直ちに終了する。" }
      ]
    },
    {
      name: "TOneWayBinder&lt;T&gt;",
      kind: "クラステンプレート", header: "mvvm/Binder.h",
      summary: "<b>片方向同期</b>(<code>src</code> → <code>dst</code> のみ)。<code>src</code> が変わると <code>dst</code> へ流れるが、逆は流れない。生成時に <code>src</code> の値を <code>dst</code> へ初期反映。",
      when: "Model → View のように一方通行で十分な時。View → VM 方向にしたいなら <code>TOneWayBinder(view, vm)</code> と引数の順で表す(別名は提供しない)。",
      sample: "TObservable&lt;i32&gt; score { 0 };\nTObservable&lt;i32&gt; ui_score;\nTOneWayBinder&lt;i32&gt; b(score, ui_score); // score → ui_score\nscore.Set(10);   // ui_score も 10\n// ui_score.Set(99) は score に影響しない",
      members: [
        { sig: "TOneWayBinder(TObservable<T>& src, TObservable<T>& dst)", desc: "<code>src</code> を <code>dst</code> へ片方向結合する。" }
      ]
    },
    {
      name: "TOneWayConvertBinder&lt;Src, Dst&gt;",
      kind: "クラステンプレート", header: "mvvm/Binder.h",
      summary: "<b>型を変換しながら</b>片方向同期する。<code>src</code> が変わるたび変換関数 <code>fn</code> を通して <code>dst</code> へ書き込む。<code>Src</code> と <code>Dst</code> が別の型でもよい。",
      when: "数値の <code>TObservable</code> を文字列ラベルの <code>TObservable</code> に流すなど、型を変えて橋渡しする時。",
      sample: "TObservable&lt;i32&gt; hp { 100 };\nTObservable&lt;FString&gt; hp_text;\nstatic FString IntToText(const i32&amp; v, void*) noexcept {\n    char buf[32]; std::snprintf(buf, sizeof(buf), \"HP: %d\", v);\n    return FString{buf};\n}\nTOneWayConvertBinder&lt;i32, FString&gt; b(hp, hp_text, &amp;IntToText, nullptr);",
      members: [
        { sig: "using ConvertFn = Dst (*)(const Src& v, void* user)", desc: "変換関数の型。値と <code>user</code> ポインタを受け取り <code>Dst</code> を返す。" },
        { sig: "TOneWayConvertBinder(TObservable<Src>& src, TObservable<Dst>& dst, ConvertFn fn, void* user)", desc: "変換関数つきで結合する。生成時に <code>fn(src)</code> を <code>dst</code> へ初期反映する。" }
      ]
    },
    {
      name: "Bind / TwoWayBind / CopyOnce",
      kind: "ファクトリ関数", header: "mvvm/Binder.h",
      summary: "バインダを手軽に作る入口。<code>Bind</code> は同じ型なら <t>TOneWayBinder</t>、違う型なら組込み変換(<t>TDefaultConverter</t>)経由の <t>TOneWayConvertBinder</t> を返す。返り値はそのまま <code>auto</code> で受けられる。",
      when: "1 行でサッと結合したい時。返り値の<t>ライフタイム</t>が結合の寿命なので、変数として保持し続ける必要がある。",
      sample: "TObservable&lt;f32&gt; hp { 100 };\nTObservable&lt;FString&gt; hp_text;\nauto b = Bind(hp, hp_text);     // f32 → FString 自動変換\nTObservable&lt;i32&gt; a, c;\nauto b2 = Bind(a, c);           // 同型 → TOneWayBinder&lt;i32&gt;\nauto b3 = TwoWayBind(a, c);     // 双方向",
      members: [
        { sig: "TOneWayBinder<T> Bind(TObservable<T>& src, TObservable<T>& dst)", desc: "同じ型同士を片方向結合して返す。" },
        { sig: "TOneWayConvertBinder<Src,Dst> Bind(TObservable<Src>& src, TObservable<Dst>& dst)", desc: "別の型同士を、組込み変換 <code>TDefaultConverter&lt;Src,Dst&gt;</code> 経由で片方向結合して返す。" },
        { sig: "TTwoWayBinder<T> TwoWayBind(TObservable<T>& a, TObservable<T>& b)", desc: "同じ型同士を双方向結合して返す。" },
        { sig: "void CopyOnce(const TObservable<T>& src, TObservable<T>& dst)", desc: "<b>1 回だけ</b> <code>src</code> の値を <code>dst</code> へコピーする。継続的な結合ではなく初期化用。" }
      ]
    },
    {
      name: "MakeBind / MakeTwoWayBind / MakeBindConvert",
      kind: "ファクトリ関数", header: "mvvm/Binder.h",
      summary: "バインダを <code>TUniquePtr</code> で返す版。バインダ自体はコピー/ムーブ不可なので、<b>クラスのメンバとして持ちたい時</b>はこちらを使う。<t>デストラクタ</t>で自動的に <code>Unsubscribe</code> される。",
      when: "結合をオブジェクトの寿命に紐づけてメンバ変数に格納したい時(<code>Bind</code> の生の返り値はメンバに置けない)。",
      sample: "class FMyApp {\n    TUniquePtr&lt;TOneWayBinder&lt;f32&gt;&gt; m_Bind;\n    void OnStart() {\n        m_Bind = MakeBind(vm.hp, view.hp);\n    }\n};",
      members: [
        { sig: "TUniquePtr<TOneWayBinder<T>> MakeBind(TObservable<T>& src, TObservable<T>& dst)", desc: "片方向バインダを <code>TUniquePtr</code> で作る。" },
        { sig: "TUniquePtr<TTwoWayBinder<T>> MakeTwoWayBind(TObservable<T>& a, TObservable<T>& b)", desc: "双方向バインダを <code>TUniquePtr</code> で作る。" },
        { sig: "TUniquePtr<TOneWayConvertBinder<Src,Dst>> MakeBindConvert(TObservable<Src>& src, TObservable<Dst>& dst)", desc: "組込み変換つき片方向バインダを <code>TUniquePtr</code> で作る。" },
        { sig: "TUniquePtr<TOneWayConvertBinder<Src,Dst>> MakeBindConvert(TObservable<Src>& src, TObservable<Dst>& dst, Dst (*fn)(const Src&, void*), void* user)", desc: "自前の変換関数 <code>fn</code> を指定する版。" }
      ]
    },
    {
      name: "TDerived&lt;T, D&gt;",
      kind: "クラステンプレート", header: "mvvm/Derived.h",
      summary: "<b>複数の <code>TObservable</code> から計算で導く</b>読み取り専用の派生値(1〜4 個の依存に対応)。同じ <code>D</code> 型の依存を取り、生成時と各依存の変更通知中に即時再計算して、結果が変わった時だけ下流へ通知する。出力 listener が owner ごと本体を同期破棄しても stack lifetime guard が検出し、復帰後に破棄済み <code>m_Dirty</code> へ触れない。同じ storage への placement-new も旧 callback と寿命分離する。",
      when: "「HP ÷ 最大HP の割合」のように、他の値から自動で求まる値が欲しい時。<t>菱形依存</t>(A→B,C→D)では D が中間状態と最終状態を複数回観測し得るため、複数入力を原子的に揃える必要がある処理は外側で batch 化する。",
      sample: "TObservable&lt;f32&gt; hp { 100.0f };\nTObservable&lt;f32&gt; max_hp { 100.0f };\nTDerived&lt;f32, f32&gt; ratio(\n    [](const f32&amp; h, const f32&amp; m){ return h / (m &gt; 0 ? m : 1.0f); },\n    hp, max_hp);\nratio.Get();      // 1.0\nhp.Set(50.0f);   // 依存通知中に ratio を 0.5 へ即時再計算\nratio.Get();      // 0.5",
      members: [
        { sig: "TDerived(Fn fn, DepRefs&... deps)", desc: "計算関数 <code>fn</code> と 1〜4 個の依存 <code>TObservable&lt;D&gt;</code> から作る。<code>fn</code> は捕獲なし<t>ラムダ</t>/関数ポインタ。生成時に初回計算し、以後は依存通知ごとに即時再計算する。" },
        { sig: "const T& Get()", ret: "派生値", desc: "現在の派生値を返す。通常は依存通知時に更新済みで、dirty が残る例外経路では返す前に再計算する。" },
        { sig: "TObservable<T>& AsObservable()", ret: "内部 Observable", desc: "派生値を持つ内部 <code>TObservable&lt;T&gt;</code> を直接露出する。" },
        { sig: "FObservableHandle Subscribe(typename TObservable<T>::Listener cb, void* user)", desc: "派生値の変化を購読する。callback が本体の owner を同期破棄する場合は破棄を最後の操作にし、その後は値・user・owner を再参照せず return する。" },
        { sig: "bool Unsubscribe(FObservableHandle h)", desc: "派生値の購読を解除する。" }
      ]
    },
    {
      name: "TObservableArray&lt;T&gt;",
      kind: "クラステンプレート", header: "mvvm/ObservableArray.h",
      summary: "<b>要素の追加・削除・変更を通知する配列</b>。1 種類の<t>コールバック</t>で受け取り、<t>EArrayChange</t> の種類で分岐する。通知 callback 中の <code>PushBack</code> / <code>PopBack</code> / <code>Remove</code> / <code>RemoveAt</code> / <code>SetAt</code> / <code>Clear</code> はすべて禁止で、Debug では assert する。assert 無効構成で処理が続いても呼び出し側の契約違反となるため、必要な変更は通知後の処理キューへ積む。callback が owner ごと <code>TObservableArray&lt;T&gt;</code> 自身を同期破棄した場合、復帰直後に破棄済み owner へ触れず return し、後続 listener を停止する。リスト UI を差分で更新する用途向け。コピー禁止。",
      when: "インベントリやリストなど、中身が増減する集合を UI に反映したい時。全更新ではなく「どこに何が起きたか」で受け取れる。",
      sample: "TObservableArray&lt;int&gt; inv;\ninv.Subscribe([](EArrayChange kind, usize idx, const int* v, void*){\n    if (kind == EArrayChange::Inserted) ACS_LOG_INFO(\"Add[%zu]=%d\", idx, *v);\n}, nullptr);\ninv.PushBack(7);   // Inserted, idx=0, v=7\ninv.SetAt(0, 99);  // Changed,  idx=0, v=99\ninv.RemoveAt(0);   // Removed,  idx=0",
      members: [
        { sig: "using Listener = void (*)(EArrayChange kind, usize index, const T* value, void* user)", desc: "通知<t>コールバック</t>の型。種類・位置・値ポインタ(削除/全消去時は <code>nullptr</code>)を受ける。" },
        { sig: "void PushBack(T v)", desc: "末尾に追加し <code>Inserted</code> を通知する。" },
        { sig: "void PopBack()", desc: "末尾を削除し <code>Removed</code> を通知する(空なら no-op)。" },
        { sig: "bool Remove(const T& value)", ret: "削除できれば true", desc: "最初に一致した 1 件を順序を保って削除し、一致位置で <code>Removed</code> を 1 回通知する。" },
        { sig: "void RemoveAt(usize index)", desc: "指定位置を削除(前詰めで順序保持)し <code>Removed</code> を通知する。" },
        { sig: "void SetAt(usize index, T v)", desc: "指定位置を書き換える。<b>同値ならスキップ</b>、変われば <code>Changed</code> を通知する。" },
        { sig: "void Clear()", desc: "全削除して <code>Cleared</code> を通知する。" },
        { sig: "usize Size() const / bool Empty() const", desc: "要素数 / 空かどうか。" },
        { sig: "const T& At(usize index) const / T& At(usize index)", ret: "要素参照", desc: "指定位置の要素を読む/書く。" },
        { sig: "const T* Data() const / T* Data()", ret: "先頭ポインタ", desc: "内部配列の先頭<t>ポインタ</t>。連続領域として一括処理する時に。" },
        { sig: "FArrayObserverHandle Subscribe(Listener cb, void* user)", ret: "購読ハンドル", desc: "変更通知を購読する。callback が owner ごとこの <code>TObservableArray&lt;T&gt;</code> を同期破棄した場合は、callback 復帰直後に通知を終了し、後続 listener は呼ばない。" },
        { sig: "bool Unsubscribe(FArrayObserverHandle h)", desc: "購読を解除する(通知中でも安全)。" },
        { sig: "u32 SubscriberCount() const", ret: "購読者数", desc: "アクティブな購読者の数。" }
      ]
    },
    {
      name: "EArrayChange",
      kind: "列挙(enum)", header: "mvvm/ObservableArray.h",
      summary: "<t>TObservableArray</t> の変更の種類を表す列挙。通知<t>コールバック</t>の中でこれを <code>switch</code> して処理を分ける。",
      when: "配列の変更通知を受け取った時、何が起きたか(追加/削除/書換/全消去)を判別する。",
      sample: "switch (kind) {\n    case EArrayChange::Inserted: /* idx に value を追加 */ break;\n    case EArrayChange::Removed:  /* idx を削除 (value=nullptr) */ break;\n    case EArrayChange::Changed:  /* idx を value に書換 */ break;\n    case EArrayChange::Cleared:  /* 全削除 (value=nullptr) */ break;\n}",
      members: [
        { sig: "Inserted", desc: "<code>index</code> に <code>value</code> を挿入した。" },
        { sig: "Removed", desc: "<code>index</code> の要素を削除した(<code>value</code> はもう無いので <code>nullptr</code>)。" },
        { sig: "Changed", desc: "<code>index</code> の要素を別の値に書き換えた(<code>value</code> = 新値)。" },
        { sig: "Cleared", desc: "全要素を削除した(<code>index=0</code>, <code>value=nullptr</code>)。" }
      ]
    },
    {
      name: "FArrayObserverHandle",
      kind: "構造体", header: "mvvm/ObservableArray.h",
      summary: "<t>TObservableArray</t> の <code>Subscribe</code> が返す<b>購読チケット</b>。仕様は <code>FObservableHandle</code> と同じく <code>id</code>+<code>generation</code> 方式。",
      when: "配列の監視を後で解除するために保持しておく。",
      sample: "FArrayObserverHandle h = inv.Subscribe(cb, this);\nif (h.IsValid()) inv.Unsubscribe(h);",
      members: [
        { sig: "bool IsValid() const", ret: "有効か", desc: "<code>id != 0</code> なら有効。" },
        { sig: "bool operator==(const FArrayObserverHandle&) const", desc: "<code>id</code> と <code>generation</code> の両方一致で同一。" }
      ]
    },
    {
      name: "kInvalidArrayObserver",
      kind: "定数", header: "mvvm/ObservableArray.h",
      summary: "無効な <code>FArrayObserverHandle</code> を表す定数。<code>TObservableArray::Subscribe</code> が失敗した時にも返る。",
      when: "配列購読ハンドルが無効かを比較で判定したい時。",
      sample: "if (h == kInvalidArrayObserver) { /* 未登録 */ }"
    },
    {
      name: "TDefaultConverter&lt;Src, Dst&gt;",
      kind: "クラステンプレート", header: "mvvm/Convert.h",
      summary: "<code>Bind</code> が使う<b>組込みの型変換テーブル</b>。<code>i32/u32/f32/f64/bool/FString</code> の主要ペアを <code>Convert</code> 静的メソッドで提供する。対応の無いペアを使うと <code>static_assert</code> で「既定変換が無い」と教えてくれる。",
      when: "通常は <code>Bind(src, dst)</code> 経由で間接的に使われる。直接呼ぶことは少ないが、変換の挙動を知りたい時の参照になる。",
      sample: "// Bind が内部でこれを呼ぶ:\nf32 x = mvvm::TDefaultConverter&lt;i32, f32&gt;::Convert(42, nullptr); // 42.0f\n// 数値↔数値は static_cast、FString 絡みは .cpp 実装でパース/整形\n// FString のパース失敗時は T{} (0/false/空文字) になる",
      members: [
        { sig: "static Dst Convert(const Src& v, void* user)", ret: "変換後の値", desc: "<code>Src</code> から <code>Dst</code> へ変換する。数値間は <code>static_cast</code>、<code>FString</code> 絡みは文字列⇄数値の整形/パース。" }
      ]
    },
    {
      name: "mvvm::imgui::Bind / BindReadOnly ほか",
      kind: "関数群", header: "mvvm/ImguiBindings.h",
      summary: "<code>TObservable&lt;T&gt;</code> を <b>ImGui ウィジェットに毎フレーム結ぶ</b>ヘルパ群。現在値を読んで描画し、ユーザー操作があれば <code>Set</code> で書き戻す。即時 mode 前提なので毎フレーム呼ぶ。",
      when: "デバッグ UI を素早く作りたい時(本番 UI は <code>src/ui/</code> 推奨)。スライダ・チェックボックス・カラーピッカー・コンボ・文字列入力・ボタン・読み取り専用表示が揃う。",
      sample: "// ImGui::Begin/End の中で毎フレーム:\nacs::mvvm::imgui::Bind(\"HP\",   vm.hp,   0.0f, 100.0f);\nacs::mvvm::imgui::Bind(\"Lv\",   vm.level, 1, 99);\nacs::mvvm::imgui::Bind(\"無敵\", vm.invincible);\nacs::mvvm::imgui::BindReadOnly(\"表示\", vm.hp);\nacs::mvvm::imgui::BindCommand(\"攻撃\", vm.attack);",
      members: [
        { sig: "void Bind(const char* label, TObservable<f32>& v, f32 v_min, f32 v_max)", desc: "<code>f32</code> スライダ。範囲 <code>v_min..v_max</code> で編集し <code>Set</code> で書き戻す。" },
        { sig: "void Bind(const char* label, TObservable<f64>& v, f64 v_min, f64 v_max)", desc: "<code>f64</code> スライダ。範囲 <code>v_min..v_max</code> で編集し <code>Set</code> で書き戻す。" },
        { sig: "void Bind(const char* label, TObservable<i32>& v, i32 v_min, i32 v_max)", desc: "<code>i32</code> スライダ。範囲 <code>v_min..v_max</code> で編集し <code>Set</code> で書き戻す。" },
        { sig: "void Bind(const char* label, TObservable<u32>& v, u32 v_min, u32 v_max)", desc: "<code>u32</code> スライダ。範囲 <code>v_min..v_max</code> で編集し <code>Set</code> で書き戻す。" },
        { sig: "void Bind(const char* label, TObservable<bool>& v)", desc: "チェックボックスで <code>bool</code> を編集する。" },
        { sig: "void BindSlider2(const char* label, TObservable<FVec2>& v, f32 v_min, f32 v_max)", desc: "<code>FVec2</code> の各成分をスライダで編集する。" },
        { sig: "void BindSlider3(const char* label, TObservable<FVec3>& v, f32 v_min, f32 v_max)", desc: "<code>FVec3</code> の各成分をスライダで編集する。" },
        { sig: "void BindColor3(const char* label, TObservable<FVec3>& v)", desc: "<code>FVec3</code> を 0..1 の RGB として扱うカラーピッカー。" },
        { sig: "void BindCombo(const char* label, TObservable<i32>& v, const char* const* labels, u32 count)", desc: "<code>i32</code> を index としてラベル配列から選ぶコンボボックス。" },
        { sig: "void BindText(const char* label, TObservable<acs::FString>& v, char* persistent, usize cap)", desc: "<code>FString</code> を文字列入力で編集する。<code>persistent</code> はフレーム間で保持される caller 持ちバッファ。" },
        { sig: "void BindFormat<T>(const char* fmt, const TObservable<T>& v)", desc: "printf 形式(例 <code>\"HP: %d\"</code>)の<b>表示専用</b>ラベル。<code>f32/f64/i32/u32</code> に特殊化済み。" },
        { sig: "bool BindCommand(const char* label, FCommand& cmd)", ret: "押されたか", desc: "<code>FCommand</code> をボタン化する。<code>CanExecute()==false</code> の間は自動でグレーアウトする。" },
        { sig: "void BindReadOnly(const char* label, const TObservable<f32>& v)", desc: "<code>f32</code> 値を編集不可のテキストとして表示する。" },
        { sig: "void BindReadOnly(const char* label, const TObservable<i32>& v)", desc: "<code>i32</code> 値を編集不可のテキストとして表示する。" },
        { sig: "void BindReadOnly(const char* label, const TObservable<bool>& v)", desc: "<code>bool</code> 値を編集不可のテキストとして表示する。" },
        { sig: "void BindProgress(const char* label, const TObservable<f32>& v, f32 v_min, f32 v_max)", desc: "HP/MP などをプログレスバーで表示する。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "MVVM": "Model(データ) / View(画面) / ViewModel(その橋渡し) の 3 層で UI を組む設計手法。VM が <t>Observable</t> で状態を公開し、View が<t>購読</t>して自動反映する。",
  "Observable": "監視できる 1 個の値。<code>Set</code> で変えると<t>購読</t>者へ通知が飛ぶ。MVVM の中核。",
  "購読": "値や配列の変化を受け取るために<t>コールバック</t>を登録すること(Subscribe)。要らなくなったら解除(Unsubscribe)する。",
  "コールバック": "あとで呼んでもらうために登録しておく関数。ACS では関数ポインタ + <code>void*</code> ユーザーデータの形を取る(STL 不依存方針)。",
  "TOneWayBinder": "<t>Observable</t> 同士を片方向(src→dst)で同期させる小さなクラス。",
  "TOneWayConvertBinder": "型を変換しながら片方向同期させるバインダ。",
  "TDefaultConverter": "<code>Bind</code> が使う組込みの型変換テーブル(数値・bool・文字列の主要ペア)。",
  "EArrayChange": "<t>TObservableArray</t> の変更種別(追加/削除/書換/全消去)を表す列挙。",
  "TObservableArray": "要素の追加・削除・変更を通知する配列。リスト UI の差分更新向け。",
  "菱形依存": "A が B と C に影響し、B と C が両方 D に影響する依存形。<t>TDerived</t> は依存通知ごとに即時再計算するため、D が中間状態と最終状態を複数回観測し得る。原子的な一括更新が必要なら外側で batch 化する。",
  "遅延": "必要になるまで計算を先延ばしにする方式(lazy)。現行 <t>TDerived</t> は生成時と依存通知時に即時評価し、<code>Get</code> の再計算は dirty が残った場合の補完経路。"
});
