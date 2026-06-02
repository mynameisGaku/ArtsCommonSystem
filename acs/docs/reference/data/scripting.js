/* ACS リファレンス — scripting モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "scripting",
  order: 52,
  title: "scripting — スクリプト(Lua)連携",
  blurb: "ゲームの一部のロジックを <b>C++ を再ビルドせずに</b> <t>Lua</t> スクリプトで書けるようにするモジュール。<t>Lua 5.4</t> の本物の <t>VM</t> を <code>FLuaVm</code> として提供し、C++ の関数を Lua から呼んだり、Lua の関数を C++ から呼んだりできます。",
  types: [
    {
      name: "FLuaVm",
      kind: "クラス", header: "scripting/LuaVm.h",
      summary: "<b>Lua 5.4 の本物の<t>VM</t></b>。スクリプト文字列を読み込んで実行し、Lua の関数呼び出し / C++ 関数の登録 / グローバル変数の読み書き / <t>GC</t> ができる。<t>gameframework</t> の <t>IScriptVm</t> <t>インターフェース</t>を実装しており、<t>Pimpl</t> で <code>lua_State*</code> を隠すので Lua のヘッダを include せずに使える。",
      when: "ゲームに『プレイヤーやモッダーが書き換えられるロジック』を入れたい時。UI イベント・カットシーン・Mod フック・データ駆動のクエスト等、<b>毎フレームの決定論が要らない部分</b>に向く(物理や当たり判定など決定論が必要な所では使わない)。",
      sample: "acs::scripting::FLuaVm vm;\nif (vm.Init().IsOk()) {\n    vm.LoadScript(\"function add(a,b) return a+b end\", 0, \"inline\");\n    acs::game::ScriptValue args[2];\n    args[0].kind = acs::game::EScriptValueKind::Number; args[0].v.num = 2;\n    args[1].kind = acs::game::EScriptValueKind::Number; args[1].v.num = 3;\n    acs::game::ScriptValue ret;\n    vm.CallFunction(\"add\", args, 2, &amp;ret);   // ret.v.num == 5\n    vm.Shutdown();\n}",
      members: [
        { sig: "FLuaVm() noexcept", desc: "VM を生成する(まだ初期化はされていない)。コピー / <t>ムーブ</t>はできない(単一所有運用)。" },
        { sig: "TResult&lt;void&gt; Init() noexcept", ret: "成否", desc: "Lua の状態(<code>lua_State</code>)を作り、標準ライブラリを開く。これを呼ぶまで <code>LoadScript</code> / <code>CallFunction</code> は失敗してよい。", when: "VM を使い始める最初に 1 回だけ呼ぶ。" },
        { sig: "void Shutdown() noexcept", desc: "Lua の状態を破棄する(<code>lua_close</code> 相当)。登録した <t>NativeFunction</t> もここで無効になる。" },
        { sig: "EScriptLanguage Language() const noexcept", ret: "言語タグ", desc: "常に <code>EScriptLanguage::Lua54</code> を返す。" },
        { sig: "TResult&lt;void&gt; LoadScript(const char* source, u32 source_len, const char* chunk_name) noexcept", ret: "成否", desc: "スクリプト文字列を 1 つの『chunk』として読み込み、<b>即時実行</b>する(<code>luaL_dostring</code> 相当)。<code>source_len</code> に 0 を渡すと <code>source</code> の長さを自動判定。<code>chunk_name</code> はエラー表示用の名前(ファイル名等)。", sample: "vm.LoadScript(\"print('hello')\", 0, \"boot.lua\");", when: "Lua ソースを読み込んで実行したい時。文字列は所有されないので呼び出し側が寿命を保証する。" },
        { sig: "TResult&lt;void&gt; CallFunction(const char* function_name, const ScriptValue* args, u32 arg_count, ScriptValue* ret_out) noexcept", ret: "成否", desc: "Lua のグローバル関数を呼ぶ。<code>args</code> / <code>arg_count</code> で引数を渡し、<code>ret_out</code> に戻り値を受け取る(<code>nullptr</code> で戻り値を捨てる)。未登録関数や実行時エラーは失敗で返る。", when: "C++ から Lua 側のロジック(イベントハンドラ等)を呼びたい時。" },
        { sig: "TResult&lt;void&gt; RegisterNativeFunction(const char* function_name, NativeFunction fn, void* user) noexcept", ret: "成否", desc: "C++ の関数を Lua のグローバル空間に登録し、Lua 側から名前で呼べるようにする。<code>user</code> は呼び出し時に <t>NativeFunction</t> の第 3 引数へそのまま渡る(<code>this</code> を束縛する常套手段)。", sample: "vm.RegisterNativeFunction(\"PlaySound\", &amp;OnPlaySound, this);", when: "Lua スクリプトから C++ のゲーム機能(音を鳴らす・スポーンする等)を呼ばせたい時。" },
        { sig: "void SetGlobalNumber(const char* name, f64 value) noexcept", desc: "Lua のグローバル数値変数を設定する。スクリプト側から <code>name</code> で参照できる。" },
        { sig: "f64 GetGlobalNumber(const char* name, f64 default_value) const noexcept", ret: "数値", desc: "Lua のグローバル数値変数を読む。未定義 / 型が違う時は <code>default_value</code> を返す。" },
        { sig: "void CollectGarbage() noexcept", desc: "<t>GC</t> を 1 サイクル強制実行する(<code>lua_gc(LUA_GCCOLLECT)</code> 相当)。フレーム境界で明示的に呼ぶ運用が推奨。" },
        { sig: "u64 MemoryUsageBytes() const noexcept", ret: "バイト数", desc: "VM が現在使っているメモリ量(近似)。デバッグ HUD 等に。" }
      ]
    },
    {
      name: "GetDefaultLuaVm()",
      kind: "関数", header: "scripting/LuaVm.h",
      summary: "プロセスで 1 個だけ共有される既定の <code>FLuaVm</code>(<t>シングルトン</t>)への参照を返す。<code>InstallLuaAsDefault()</code> が <t>gameframework</t> の provider へ登録する実体でもある。",
      when: "アプリ全体で 1 つの Lua VM を共有したい時。通常は直接呼ばず <code>InstallLuaAsDefault()</code> 経由で <code>acs::game::GetDefaultScriptVm()</code> から使う。",
      sample: "acs::game::IScriptVm& vm = acs::scripting::GetDefaultLuaVm();\nvm.Init();"
    },
    {
      name: "InstallLuaAsDefault()",
      kind: "関数", header: "scripting/LuaVm.h",
      summary: "<t>gameframework</t> の『既定 <t>ScriptVm</t> provider』に <code>GetDefaultLuaVm</code> を登録する。これを呼ぶと、以降 <code>acs::game::GetDefaultScriptVm()</code> が <b>本物の Lua 5.4 VM</b> を返すようになる。",
      when: "アプリ起動時に <b>一度だけ</b>呼ぶ。これにより上位コードは backend を意識せず <code>GetDefaultScriptVm()</code> だけで実 VM を取得でき、<code>#if</code> 分岐がこの 1 箇所だけで済む。",
      sample: "// アプリ起動時 (一度だけ)\n#if WITH_ACS_SCRIPTING\n    acs::scripting::InstallLuaAsDefault();\n#endif\n// 以降どこでも:\nacs::game::IScriptVm& vm = acs::game::GetDefaultScriptVm(); // 実 Lua VM"
    },
    {
      name: "ScriptValue",
      kind: "構造体", header: "gameframework/ScriptHost.h",
      summary: "C++ と Lua の間で値を受け渡すための<b>タグ付き共用体風の <t>POD</t></b>。<code>kind</code> で種類を、<code>v</code> で中身を表す。種類は Nil / Bool / Number / FString / Handle の 5 つ。<code>FLuaVm</code> の引数 / 戻り値はすべてこの型の配列で表現する。",
      when: "<code>CallFunction</code> に引数を渡す / 戻り値を受け取る時や、<t>NativeFunction</t> の中で値を読み書きする時に使う。",
      sample: "acs::game::ScriptValue v;\nv.kind = acs::game::EScriptValueKind::Number;\nv.v.num = 3.14;          // Number は f64\n// 文字列は所有しない(寿命は呼び出し側が保証)\nacs::game::ScriptValue s;\ns.kind = acs::game::EScriptValueKind::FString;\ns.v.str = \"hello\";",
      members: [
        { sig: "EScriptValueKind kind", desc: "値の種類(既定は <code>Nil</code>)。これを見てから <code>v</code> の正しいフィールドを読む。" },
        { sig: "union { bool b; f64 num; const char* str; u32 handle; } v", desc: "中身。<code>Number</code> は <code>num</code>(整数も <code>f64</code> に丸めて格納)、文字列は <code>str</code>(<b>所有しない</b>)、<code>Handle</code> は <code>handle</code>(エンティティ等の不透明 ID)。" }
      ]
    },
    {
      name: "EScriptValueKind",
      kind: "列挙(enum)", header: "gameframework/ScriptHost.h",
      summary: "<t>ScriptValue</t> がどの種類の値かを表すタグ。Lua / Wren / Python の動的型の最大公約数。",
      when: "<code>ScriptValue</code> を作る / 読む時に <code>kind</code> へ設定・判定する。",
      sample: "if (ret.kind == acs::game::EScriptValueKind::Number) {\n    f64 n = ret.v.num;\n}",
      members: [
        { sig: "Nil = 0", desc: "値なし。既定状態 / 戻り値なしを表す。" },
        { sig: "Bool = 1", desc: "真偽値(<code>v.b</code>)。" },
        { sig: "Number = 2", desc: "数値。<code>f64</code>(<code>v.num</code>)で統一。整数もここに丸めて入る。" },
        { sig: "FString = 3", desc: "文字列(<code>v.str</code>)。<b>所有しない</b><t>ポインタ</t>。" },
        { sig: "Handle = 4", desc: "<code>u32</code> の不透明 ID(<code>v.handle</code>)。生ポインタを Lua に渡さないための安全な間接層。" }
      ]
    },
    {
      name: "ScriptCallFrame",
      kind: "構造体", header: "gameframework/ScriptHost.h",
      summary: "Lua から <t>NativeFunction</t>(登録した C++ 関数)が呼ばれた瞬間に渡される、引数と戻り値のまとめ。引数配列・件数・戻り値書き込み先だけを持つ薄い <t>POD</t>。",
      when: "<code>RegisterNativeFunction</code> で登録した C++ 関数の中で、引数を読み戻り値を書く時に使う。",
      sample: "void OnAdd(acs::game::IScriptVm&, acs::game::ScriptCallFrame& f, void*) noexcept {\n    f64 a = f.args[0].v.num;\n    f64 b = f.args[1].v.num;\n    if (f.ret) { f.ret->kind = acs::game::EScriptValueKind::Number; f.ret->v.num = a + b; }\n}",
      members: [
        { sig: "const ScriptValue* args", desc: "引数配列(読み取り専用、長さは <code>arg_count</code>)。" },
        { sig: "u32 arg_count", desc: "<code>args</code> の長さ。0 でも有効(引数なし関数)。" },
        { sig: "ScriptValue* ret", desc: "戻り値の書き込み先。<code>nullptr</code> なら戻り値は捨てられる。" }
      ]
    },
    {
      name: "NativeFunction",
      kind: "関数ポインタ型", header: "gameframework/ScriptHost.h",
      summary: "Lua 側から呼べる C++ 関数の型。<code>void(*)(IScriptVm&amp; vm, ScriptCallFrame&amp; frame, void* user) noexcept</code>。<code>user</code> には登録時に渡したコンテキスト(<code>this</code> 等)がそのまま流れてくる。<b>例外を投げてはいけない</b>(Lua は C 由来で巻き戻し非対応)。",
      when: "Lua から呼ばせたい C++ 関数を <code>RegisterNativeFunction</code> に登録する時の関数シグネチャとして使う。",
      sample: "void OnLog(acs::game::IScriptVm&, acs::game::ScriptCallFrame& f, void* user) noexcept {\n    // f.args[0].v.str をログ出力 等\n}\nvm.RegisterNativeFunction(\"Log\", &amp;OnLog, nullptr);"
    },
    {
      name: "EScriptLanguage",
      kind: "列挙(enum)", header: "gameframework/ScriptHost.h",
      summary: "VM の backend(言語)を識別するタグ。<code>FLuaVm</code> は常に <code>Lua54</code> を返す。複数 backend を併用する場合の判定に使う。",
      when: "<code>IScriptVm::Language()</code> の戻り値で、どの言語の VM かを見分けたい時。",
      sample: "if (vm.Language() == acs::game::EScriptLanguage::Lua54) { /* Lua 用処理 */ }",
      members: [
        { sig: "Lua54 = 0", desc: "Lua 5.4(推奨 backend、<code>FLuaVm</code> が返す)。" },
        { sig: "Wren = 1", desc: "Wren(小規模・組み込み向け)。" },
        { sig: "Python3 = 2", desc: "CPython 3.x(重量級)。" },
        { sig: "Custom = 3", desc: "ユーザー独自実装の <code>IScriptVm</code> 派生。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "Lua": "ゲーム組み込みで広く使われる軽量スクリプト言語。C++ を再ビルドせずにロジックを差し替えられる。",
  "Lua 5.4": "<t>Lua</t> の安定版の 1 つ。ACS が <code>FLuaVm</code> として同梱する本物のランタイム。",
  "VM": "Virtual Machine。スクリプトを読み込んで実行する仮想機械。<code>FLuaVm</code> は <t>Lua</t> の VM。",
  "IScriptVm": "スクリプト VM の純粋仮想<t>インターフェース</t>。Lua/Wren/Python 等を差し替えるための seam。<code>FLuaVm</code> がこれを実装する。",
  "NativeFunction": "<t>Lua</t> 側から呼べる C++ 関数の型。例外を投げてはいけない。",
  "GC": "Garbage Collection(ガベージコレクション)。不要になったメモリを自動回収する仕組み。<t>Lua</t> が内部で行う。",
  "Pimpl": "実装の詳細を別構造体へ隠し、ヘッダに公開しない手法。<code>FLuaVm</code> は <code>lua_State*</code> をこれで隠す。",
  "シングルトン": "プロセス内に 1 個だけ存在するインスタンス。<code>GetDefaultLuaVm()</code> はこれを返す。"
});
