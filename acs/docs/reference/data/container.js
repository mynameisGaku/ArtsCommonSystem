/* ACS リファレンス — container モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "container",
  order: 8,
  title: "container — 配列・文字列・連想配列",
  blurb: "ACS の配列、文字列、連想配列、非所有ビュー、JSON 値を提供する Runtime モジュール。<t>所有型</t>は <code>IAllocator</code> を通じて領域を管理し、確保失敗と寿命の契約を公開 API ごとに定める。",
  types: [
    {
      name: "TInlineArray&lt;T, InlineCapacity&gt;",
      kind: "クラステンプレート", header: "container/InlineArray.h",
      summary: "少数要素を本体内へ保持し、容量超過時だけ allocator へ退避する可変長配列。",
      when: "通常件数が小さい一時配列で heap 確保を避けつつ、まれな増加も扱う時。",
      members: [
        { sig: "bool Remove(const T& value)", ret: "削除できれば true", desc: "最初に一致した 1 件を順序を保って削除する。動的領域へ移行済みなら、削除後もその領域を再利用する。" }
      ]
    },
    {
      name: "FStableStringKey",
      kind: "構造体", header: "container/StableStringKey.h",
      summary: "文字列 view と事前計算済み hash を組にした非所有キー。",
      when: "同じ文字列キーを繰り返し検索し、毎回の hash 計算を省く時。"
    },
    {
      name: "FStringHasher",
      kind: "構造体", header: "container/StringHasher.h",
      summary: "<code>FString</code> と <code>FStringView</code> を同じ byte hash へ写す関数オブジェクト。",
      when: "文字列 map の所有キーと非所有キーを相互検索する時。"
    },
    {
      name: "TArray&lt;T&gt;",
      kind: "クラステンプレート", header: "container/Array.h",
      summary: "要素を連続領域へ所有する可変長配列。末尾追加と range-for に対応し、コピーは禁止、複製は <code>Clone()</code> で明示する。確保元は <code>IAllocator</code> で指定できる。",
      when: "個数が動的に変わる要素列を持ちたい時の既定の入れ物。敵リスト・頂点・コマンド列など、ほぼ全ての『複数』はこれ。",
      sample: "TArray&lt;int&gt; xs;\nxs.Add(10);\nxs.Emplace(20);          // 直接構築\nfor (int v : xs) { /* range-for 可 */ }\nauto copy = xs.Clone();      // 複製は明示的に",
      members: [
        { sig: "void Add(const T& v) / Add(T&& v)", desc: "末尾に要素を追加する。容量が足りなければ自動で約 1.5 倍に拡張。" },
        { sig: "T& Emplace(Args&&... args)", ret: "追加した要素の参照", desc: "末尾にその場で<b>直接構築</b>する(一時オブジェクトのコピー/ムーブを省く)。", sample: "arr.Emplace(x, y, z);  // FVec3 をその場で構築" },
        { sig: "void Pop()", desc: "末尾要素を 1 つ取り除く(空の時に呼ぶと<t>アサート</t>)。" },
        { sig: "bool Remove(const T& value)", ret: "削除できれば true", desc: "最初に一致した 1 件を順序を保って削除する。見つからなければ配列を変更しない。" },
        { sig: "void RemoveAtSwap(usize i)", desc: "i 番に末尾要素をムーブして縮める。<b>順序は保たれない</b>が削除が O(1) で速い。", when: "順序を気にしないリストから素早く 1 個消したい時。" },
        { sig: "usize Num() const / usize Max() const / bool IsEmpty() const", desc: "現在の要素数 / 確保済み容量 / 空かどうか。" },
        { sig: "void Reserve(usize n)", desc: "あらかじめ容量を確保する。追加中の再確保を避けて速くなる。", when: "追加する個数がだいたい分かっている時に最初に呼ぶ。" },
        { sig: "void SetNum(usize n)", desc: "要素数を n に変える。増えた分はデフォルト構築、減った分は破棄。" },
        { sig: "void Reset()", desc: "要素を全て破棄してサイズ 0 に(容量は保持)。" },
        { sig: "void Shrink()", desc: "容量を現在サイズまで縮める。再確保に失敗した場合は通知せず、内容と現在容量を維持する。" },
        { sig: "T& operator[](usize i)", ret: "要素の参照", desc: "添字アクセス。範囲外は<t>アサート</t>(Debug のみ検出)。" },
        { sig: "T& Front() / T& Last()", desc: "先頭 / 末尾の要素を参照する。" },
        { sig: "T* GetData() / T* begin() / T* end()", desc: "生<t>ポインタ</t> / イテレータを返す。range-for に対応。" },
        { sig: "TSpan<T> AsSpan()", ret: "非所有ビュー", desc: "中身を <code>TSpan</code> として渡せる形にする。", when: "配列の中身を所有権なしで関数に渡したい時。" },
        { sig: "TArray Clone() const", ret: "複製した配列", desc: "中身を丸ごとコピーした新しい配列を作る(高コストなので明示的)。" },
        { sig: "IAllocator* GetAllocator() const", ret: "アロケータ", desc: "確保に使っている<t>アロケータ</t>を返す。" }
      ]
    },
    {
      name: "TSpan&lt;T&gt;",
      kind: "クラステンプレート", header: "container/Span.h",
      summary: "連続メモリ領域を指す<b><t>非所有ビュー</t></b>。先頭ポインタと要素数だけを持ち、<code>TArray</code>、生配列、連続バッファを同じ引数形で受け取れる。",
      when: "関数の引数で『連続した要素列をまとめて見たい』時。所有権を渡さず、配列か生配列かを問わず受けられる。参照先の寿命には注意。",
      sample: "void Sum(TSpan&lt;const int&gt; xs) {\n    int s = 0; for (int v : xs) s += v;\n}\nint raw[3] = {1,2,3};\nSum(raw);          // C 配列から暗黙変換\nSum(myArray.AsSpan());",
      members: [
        { sig: "constexpr TSpan(T* data, usize size)", desc: "先頭ポインタと長さからビューを作る。" },
        { sig: "template<usize N> TSpan(T (&arr)[N])", desc: "C 配列から長さ N を自動推論して作る(暗黙変換)。" },
        { sig: "T* GetData() / usize Num() / bool IsEmpty()", desc: "先頭ポインタ / 要素数 / 空かどうか。" },
        { sig: "T& operator[](usize i)", desc: "添字アクセス。範囲外は<t>アサート</t>(Debug のみ)。" },
        { sig: "T* begin() / T* end()", desc: "イテレータ。range-for に対応。" },
        { sig: "TSpan SubSpan(usize offset, usize count)", ret: "部分ビュー", desc: "一部分だけを切り出したビューを返す(コピーしない)。範囲外は<t>アサート</t>で検出する。", sample: "auto mid = sp.SubSpan(1, 2);  // 1 番目から 2 個" },
        { sig: "bool TrySubSpan(usize offset, usize count, TSpan&amp; out)", ret: "範囲が有効なら true", desc: "外部入力向けの fail-closed 版。null と非 0 size の不正 view、範囲外、加算 overflow をアサートなしで拒否し、失敗時は <code>out</code> を変更しない。", when: "ファイルや通信など信頼できない長さから部分 view を切り出す時。", sample: "TSpan&lt;const u8&gt; part;\nif (!bytes.TrySubSpan(offset, count, part)) {\n    return InvalidRange();\n}" }
      ]
    },
    {
      name: "FString",
      kind: "クラス", header: "container/String.h",
      summary: "NUL 終端を維持する UTF-8 可変長文字列。22 バイト以下は本体内に保持し、超過時だけ指定 <code>IAllocator</code> から確保する。<code>FStringView</code> へ暗黙変換できる。",
      when: "中身を書き換えたり連結したりする文字列が欲しい時。名前・ログ・組み立て中のテキストなど。",
      sample: "FString s = \"Hello\";\ns.Append(\", \");\ns.Append(FStringView(\"world\"));\ns.AppendFormat(\" (%d)\", 42);  // printf 風\nFStringView v = s;            // ビューへ暗黙変換",
      members: [
        { sig: "FString(const char* cstr, IAllocator& a = DefaultAllocator())", desc: "C 文字列から作る。<t>アロケータ</t>も指定できる。" },
        { sig: "FString(FStringView v, IAllocator& a = DefaultAllocator())", desc: "ビューから中身をコピーして作る。" },
        { sig: "void Append(FStringView v) / Append(char c)", desc: "末尾に文字列 / 1 文字を追記する。" },
        { sig: "usize AppendFormat(const char* fmt, ...)", ret: "最終サイズ(失敗時 0)", desc: "<code>printf</code> 風の書式で末尾に整形追記する。", sample: "s.AppendFormat(\"hp=%d/%d\", cur, max);" },
        { sig: "void Reserve(usize n)", desc: "容量を予約して追記中の再確保を減らす。" },
        { sig: "void Clear()", desc: "中身を空にし、現在の確保容量は保持する。" },
        { sig: "const char* Data() / usize Size() / bool IsEmpty()", desc: "先頭ポインタ(NUL 終端) / バイト長 / 空かどうか。" },
        { sig: "usize Capacity() const", ret: "確保容量", desc: "現在の確保済み容量。本体内に保持している時は 22。" },
        { sig: "FStringView View() const / operator FStringView()", ret: "非所有ビュー", desc: "中身を指す <code>FStringView</code> を返す。多くの API はこの形で受ける。" },
        { sig: "char& operator[](usize i)", desc: "i バイト目を参照する(範囲外は<t>アサート</t>)。" },
        { sig: "static constexpr usize kSsoCapacity = 22", desc: "ヒープを使わずインライン保持できる最大バイト数。" },
        { sig: "void Add(char c)", desc: "末尾に 1 文字を追記する(<code>Append(char)</code> の別名)。" },
        { sig: "explicit FString(IAllocator& a)", desc: "空文字列を指定<t>アロケータ</t>で作る。引数なしの既定構築も可能。" },
        { sig: "bool operator==(const FString&/FStringView, const FString&/FStringView)", ret: "等しいか", desc: "<code>FString</code> 同士、または <code>FString</code> と <code>FStringView</code> をバイト単位で比較する自由関数(3 オーバーロード)。" }
      ]
    },
    {
      name: "FStringView",
      kind: "クラス", header: "container/StringView.h",
      summary: "UTF-8 バイト列を指す<b><t>非所有ビュー</t></b>。先頭ポインタとバイト長だけを持ち、確保せずに比較、検索、接頭辞・接尾辞判定を行う。",
      when: "文字列を『読むだけ』の関数引数。<code>FString</code> でも C 文字列リテラルでも同じ型で受け取れる。元の文字列より長生きさせないこと。",
      sample: "void Greet(FStringView name) {\n    if (name.StartsWith(\"Mr.\")) { /* ... */ }\n}\nGreet(\"Alice\");      // C 文字列から暗黙変換\nGreet(myString);     // FString からも暗黙変換",
      members: [
        { sig: "constexpr FStringView(const char* data, usize size)", desc: "ポインタと長さからビューを作る。" },
        { sig: "FStringView(const char* cstr)", desc: "NUL 終端 C 文字列から長さを数えて作る(暗黙変換)。" },
        { sig: "const char* Data() / usize Size() / bool IsEmpty()", desc: "先頭ポインタ / バイト長 / 空かどうか。" },
        { sig: "char operator[](usize i)", desc: "i バイト目を返す(範囲外は<t>アサート</t>)。" },
        { sig: "FStringView SubView(usize offset, usize count)", ret: "部分ビュー", desc: "一部分を切り出したビューを返す(コピーしない)。" },
        { sig: "bool Equals(FStringView other) const", ret: "等しいか", desc: "バイト単位の完全一致比較。<code>operator==</code> / <code>!=</code> も使える。" },
        { sig: "bool StartsWith(FStringView prefix) const", ret: "接頭辞か", desc: "指定の文字列で始まるか判定する。" },
        { sig: "bool EndsWith(FStringView suffix) const", ret: "接尾辞か", desc: "指定の文字列で終わるか判定する。" },
        { sig: "const char* begin() / end()", desc: "イテレータ。range-for に対応。" }
      ]
    },
    {
      name: "THashMap&lt;K, V, H&gt;",
      kind: "クラステンプレート", header: "container/HashMap.h",
      summary: "キーから値を検索する<t>連想配列</t>。探索距離と指紋を持つバケット列と密な値配列を分け、削除済み目印を残さず連続イテレーションに対応する。<b>コピー禁止・<t>ムーブ</t>専用</b>。",
      when: "ID から実体、名前から設定値、のように『キーで素早く引きたい』時。",
      sample: "THashMap&lt;u32, FString&gt; names;\nnames.Add(1, FString(\"Alice\"));\nif (FString* p = names.Find(1)) { /* 見つかった */ }\nfor (auto& kv : names) { /* kv.first, kv.second */ }\nnames.Remove(1);",
      members: [
        { sig: "void Add(const K& key, V value)", desc: "キーと値を追加する。同じキーが既にあれば<b>上書き</b>し、必要時にバケット容量を拡張する。" },
        { sig: "V* Find(const K& key)", ret: "値ポインタ or null", desc: "キーを検索する。見つかれば値への<t>ポインタ</t>、なければ <code>nullptr</code>。", when: "存在チェックと値取得を一度に済ませたい時。" },
        { sig: "bool Contains(const K& key) const", ret: "存在するか", desc: "そのキーがあるかどうかだけ調べる。" },
        { sig: "bool Remove(const K& key)", ret: "削除したか", desc: "キーを削除する。<t>tombstone</t> を残さず後方シフトで詰める。あれば true。" },
        { sig: "void Reserve(usize n)", desc: "n 個入る容量を予約し、挿入中の rehash を防ぐ。", when: "入れる個数の目安が分かっている時に最初に呼ぶ。" },
        { sig: "void Reset()", desc: "全要素を削除して空にする。" },
        { sig: "usize Num() const / bool IsEmpty() const", desc: "要素数 / 空かどうか。" },
        { sig: "EntryType* begin() / end()", desc: "密な値配列を直接走査する(<code>TPair&lt;K,V&gt;</code> の列)。range-for 対応。" }
      ]
    },
    {
      name: "TPair&lt;K, V&gt;",
      kind: "構造体テンプレート", header: "container/HashMap.h",
      summary: "キーと値を <code>first</code> / <code>second</code> に保持する値型。<code>THashMap</code> の <code>EntryType</code> として密な値配列に格納される。",
      when: "<code>THashMap</code> を range-for で回した時の各要素。あるいは 2 値をまとめて返したい時。",
      sample: "for (TPair&lt;u32, FString&gt;& kv : map) {\n    u32 key = kv.first;\n    FString& val = kv.second;\n}",
      members: [
        { sig: "K first", desc: "キー(1 つ目の値)。" },
        { sig: "V second", desc: "値(2 つ目の値)。" }
      ]
    },
    {
      name: "THasher&lt;T&gt; / HashBytes / HashMix64",
      kind: "ハッシュ関数群", header: "container/Hash.h",
      summary: "<code>THashMap</code> 等が使う<t>ハッシュ関数</t>。整数・<t>ポインタ</t>・<code>FStringView</code> 用の <code>THasher</code> 特殊化が既定で用意され、独自キー型は <code>THasher</code> を特殊化して対応する。",
      when: "標準キー(整数 / ポインタ / 文字列ビュー)ならそのまま使える。自作のキー型をマップに入れたい時だけ <code>THasher&lt;FMyKey&gt;</code> を特殊化する。",
      sample: "// 自作キーをマップで使う:\ntemplate&lt;&gt; struct acs::THasher&lt;FMyKey&gt; {\n    u64 operator()(const FMyKey& k) const noexcept {\n        return HashBytes(&amp;k, sizeof(k));\n    }\n};",
      members: [
        { sig: "u64 HashMix64(u64 x)", ret: "混ぜた 64bit 値", desc: "整数 / ポインタキーのビットを拡散し、<code>THashMap</code> のバケット偏りを抑える 64bit 混合関数。" },
        { sig: "u64 HashBytes(const void* data, usize len, u64 seed = ...)", ret: "ハッシュ値", desc: "任意のバイト列のハッシュ。汎用キーや自作型のハッシュ実装に使う。" },
        { sig: "struct THasher<T>", desc: "キー型 T からハッシュ値を作る関数オブジェクト。整数 / <code>T*</code> / <code>FStringView</code> は特殊化済み。" }
      ]
    },
    {
      name: "FJsonValue",
      kind: "クラス", header: "container/Json.h",
      summary: "JSON の値 1 ノードを表す<t>DOM</t>。配列とオブジェクトの子を <code>TArray</code> で再帰所有し、型不一致時は既定値、空ビュー、静的 Null を返す。<b>コピー禁止・<t>ムーブ</t>専用</b>。",
      when: "ACS の設定、保存データ、アセット情報を JSON 値として保持し、型確認付きで読み書きする時。",
      sample: "auto r = acs::ParseJson(text);\nif (r.IsOk()) {\n    const FJsonValue& root = r.Value();\n    FStringView img = root.Get(\"meta\").Get(\"image\").AsString();\n    u32 n = root.Get(\"frames\").Size();\n    for (u32 i = 0; i &lt; n; ++i)\n        i32 x = root.Get(\"frames\").At(i).Get(\"x\").AsInt();\n}",
      members: [
        { sig: "EJsonType Type() const", ret: "種別", desc: "この値が Null / Bool / Number / String / Array / Object のどれか。" },
        { sig: "bool IsNull() / IsBool() / IsNumber() / IsString() / IsArray() / IsObject()", desc: "種別判定のショートカット。" },
        { sig: "bool AsBool(bool def = false) const", ret: "真偽値", desc: "Bool ならその値、違えば def。" },
        { sig: "f64 AsNumber(f64 def = 0.0) const", ret: "数値", desc: "Number ならその値、違えば def。JSON の数値は全て <code>f64</code> で保持。" },
        { sig: "i64 AsInt(i64 def) / u32 AsU32(u32 def)", desc: "型不一致と NaN は def を返す。AsInt は i64 範囲へ、AsU32 は負値を 0、上限超過を最大値へ clamp する。" },
        { sig: "f32 AsF32(f32 def)", desc: "Number を f32 へ単純変換し、型不一致なら def を返す。非有限値と f32 範囲外の値は補正しない。" },
        { sig: "FStringView AsString() const", ret: "文字列ビュー", desc: "String ならその中身、違えば空ビュー。" },
        { sig: "u32 Size() const", ret: "要素数", desc: "Array / Object の子要素数。スカラなら 0。" },
        { sig: "const FJsonValue& At(u32 i) const", ret: "i 番目の子", desc: "配列要素を index で取る。範囲外 / 非配列なら静的 Null 値を返す(落ちない)。" },
        { sig: "const FJsonValue& Get(const char* key) const", ret: "メンバ値", desc: "オブジェクトメンバを key で取る。無ければ静的 Null 値(連鎖が安全)。" },
        { sig: "const FJsonValue* Find(const char* key) const", ret: "値ポインタ or null", desc: "メンバを探す。無ければ <code>nullptr</code>。" },
        { sig: "bool Has(const char* key) const", ret: "あるか", desc: "そのキーのメンバが存在するか。" },
        { sig: "u32 MemberCount() const / FStringView MemberKey(u32 i) const", desc: "オブジェクトのメンバ数 / i 番目メンバのキー名(値は <code>At(i)</code> で取る)。" },
        { sig: "void _SetNull() / _SetBool(bool) / _SetNumber(f64) / _SetString(FStringView)", desc: "<b>ビルダ</b>: この値をスカラ(Null/Bool/Number/String)に設定する。パーサ内部用だがテストや手組みでも使える。" },
        { sig: "void _MakeArray() / _MakeObject()", desc: "<b>ビルダ</b>: この値を空の Array / Object に初期化する。" },
        { sig: "FJsonValue& _PushArrayElem()", ret: "追加した子の参照", desc: "<b>ビルダ</b>: Array に空要素を 1 つ追加し、その参照を返す。" },
        { sig: "FJsonValue& _AddMember(FStringView key)", ret: "追加した値の参照", desc: "<b>ビルダ</b>: Object に key を追加し、その value 参照を返す。" }
      ]
    },
    {
      name: "ParseJson",
      kind: "関数", header: "container/Json.h",
      summary: "JSON テキストを解析して <code>FJsonValue</code> の<t>DOM</t>を返す入口。構文・深さ・入力サイズの失敗は行・列または subcode 付きの <t>Result</t> で返す。DOM 追加時の allocator 枯渇は checked API が fail-fast で検出する。",
      when: "外部の JSON を読み込む全ての場面。結果は <code>IsOk()</code> を確かめてから <code>Value()</code> で取り出す。",
      sample: "TResult&lt;FJsonValue&gt; r = acs::ParseJson(text, len);\nif (!r.IsOk()) { /* r.Error() に line/col 付きエラー */ }\nelse { const FJsonValue& root = r.Value(); /* ... */ }",
      members: [
        { sig: "TResult<FJsonValue> ParseJson(const char* text, usize len)", ret: "DOM or エラー", desc: "長さ指定でパースする。" },
        { sig: "TResult<FJsonValue> ParseJson(FStringView s)", ret: "DOM or エラー", desc: "文字列ビューからパースする。" },
        { sig: "TResult<FJsonValue> ParseJson(const char* text, usize len, IAllocator& allocator)", ret: "DOM or エラー", desc: "root、子、key、文字列を指定 allocator で構築する。" },
        { sig: "TResult<FJsonValue> ParseJson(FStringView s, IAllocator& allocator)", ret: "DOM or エラー", desc: "文字列ビューを指定 allocator でパースする。" }
      ]
    },
    {
      name: "TryWriteJson",
      kind: "関数", header: "container/Json.h",
      summary: "<code>FJsonValue</code> を UTF-8 JSON へ書き出し、成功時だけ出力文字列を置き換える。",
      when: "ACS の JSON 値を保存用文字列へ変換し、深さと出力バイト数を制限する時。",
      sample: "FString output;\nif (!acs::TryWriteJson(value, output)) { /* output は変更されない */ }",
      members: [
        { sig: "bool TryWriteJson(const FJsonValue& Value, FString& Output, u32 MaxDepth = 256u, usize MaxBytes = kMaxJsonInputBytes)", ret: "完全に書き出せれば true", desc: "Output の allocator で一時文字列を構築し、制御文字を escape する。非有限数、深さ超過、サイズ超過、確保失敗では false を返して Output を維持する。" }
      ]
    },
    {
      name: "EJsonType",
      kind: "列挙(enum)", header: "container/Json.h",
      summary: "<code>FJsonValue</code> が保持している値の種別。<code>Null / Bool / Number / String / Array / Object</code> の 6 種。",
      when: "<code>Type()</code> の戻り値で値の種類を分岐したい時。",
      sample: "switch (v.Type()) {\n    case EJsonType::Number: /* ... */ break;\n    case EJsonType::Array:  /* ... */ break;\n    default: break;\n}"
    },
    {
  name: "Json エラー subcode (kSubJson*)",
  kind: "定数群", header: "container/Json.h",
  summary: "<code>ParseJson</code> が失敗時に <t>Result</t> へ載せる<b>エラー subcode</b>。<code>EErrCategory::Generic</code> 配下の <code>u16</code> 定数で、構文・深さ・末尾内容・途中終端・数値・エスケープ・サイズ超過を区別できる。<b>全て <code>inline constexpr u16</code></b>。",
  when: "<code>ParseJson</code> の <code>Error()</code> を受け取り、どの種類の構文エラーかで分岐・ログ出ししたい時。",
  sample: "auto r = acs::ParseJson(text, len);\nif (!r.IsOk()) {\n    const FErrorCode&amp; e = r.Error();\n    if (e.subcode == kSubJsonDepth) { /* nesting が深すぎ */ }\n    else if (e.subcode == kSubJsonBadEscape) { /* \\u 等が不正 */ }\n}",
  members: [
    { sig: "inline constexpr u16 kSubJsonSyntax = 1400", desc: "構文エラー(予期しないトークン)。" },
    { sig: "inline constexpr u16 kSubJsonDepth = 1401", desc: "nesting が深すぎる(<code>kMaxDepth</code> 超過で DoS 防止)。" },
    { sig: "inline constexpr u16 kSubJsonTrailing = 1402", desc: "ルート値の後に余分なゴミがある。" },
    { sig: "inline constexpr u16 kSubJsonEof = 1403", desc: "値の途中で入力が終端した。" },
    { sig: "inline constexpr u16 kSubJsonBadNumber = 1404", desc: "数値として解釈できない。" },
    { sig: "inline constexpr u16 kSubJsonBadEscape = 1405", desc: "不正なエスケープ / <code>\\u</code> シーケンス。" },
    { sig: "inline constexpr u16 kSubJsonSize = 1406", desc: "入力全体または解析中の文字列が設定上限を超えた。" }
  ]
}
  ]
});

Object.assign(ACS_REF.glossary, {
  "所有型": "保持する値を自身の状態として管理し、必要な確保と解放を型の寿命に合わせて行う ACS の型。",
  "非所有ビュー": "中身を所有(確保/解放)せず、既存データを<t>ポインタ</t>+長さで参照するだけの軽い型。元データより長生きさせないこと。",
  "連想配列": "キーから値を引く入れ物。ハッシュで高速に検索する(<code>THashMap</code>)。",
  "SSO": "Small String Optimization。短い文字列をヒープ確保せずオブジェクト内に直接埋め込み、確保コストを省く最適化。",
  "DOM": "Document Object Model。文書(JSON 等)を木構造のオブジェクト群としてメモリ上に表現したもの。",
  "ハッシュ関数": "任意のデータから固定長の数値(ハッシュ値)を作る関数。連想配列の格納位置決めに使う。",
  "Robin Hood ハッシュ": "オープンアドレス法のハッシュ表で、探索距離の長い要素を優先して場所を譲り合い、最悪距離を均す方式。検索が安定して速い。",
  "tombstone": "削除済みスロットの目印。ACS の <code>THashMap</code> はこれを残さず後方シフトで詰める。",
  "アサート": "前提条件が崩れていないか実行時に検査する仕組み。Debug ビルドで破ると停止して原因を知らせる。"
});
