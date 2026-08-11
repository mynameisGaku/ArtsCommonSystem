/* ACS リファレンス — scripting モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "scripting",
  order: 52,
  title: "scripting — スクリプト(Lua)連携",
  blurb: "ゲームの一部のロジックを <b>C++ を再ビルドせずに</b> <t>Lua</t> スクリプトで書けるようにするモジュール。<t>Lua 5.4</t> の本物の <t>VM</t> を <code>CLuaVm</code> として提供し、C++ の関数を Lua から呼んだり、Lua の関数を C++ から呼んだりできます。",
  types: [
    {
      name: "CLuaVm",
      kind: "クラス", header: "scripting/LuaVm.h",
      summary: "<b>Lua 5.4 の本物の<t>VM</t></b>。スクリプト文字列を読み込んで実行し、Lua の関数呼び出し / C++ 関数の登録 / グローバル変数の読み書き / <t>GC</t> ができる。<t>gameframework</t> の <t>IScriptVm</t> <t>インターフェース</t>を実装しており、<t>Pimpl</t> で <code>lua_State*</code> を隠すので Lua のヘッダを include せずに使える。",
      when: "ゲームに『プレイヤーやモッダーが書き換えられるロジック』を入れたい時。UI イベント・カットシーン・Mod フック・データ駆動のクエスト等、<b>毎フレームの決定論が要らない部分</b>に向く(物理や当たり判定など決定論が必要な所では使わない)。",
      members: [
        { sig: "CLuaVm() noexcept", desc: "VM を生成する(まだ初期化はされていない)。コピー / <t>ムーブ</t>はできない(単一所有運用)。" },
        { sig: "explicit CLuaVm(IAllocator&amp; allocator) noexcept", desc: "native関数登録簿の保存領域に使う確保器を注入してVMを生成する。VMは確保器を所有せず参照を保持するため、確保器は構築したVMより後まで生存させる。確保失敗経路の制御や所有元と同じ寿命の確保器を使う場合に選ぶ。" },
        { sig: "TResult&lt;void&gt; Init() noexcept", ret: "成否", desc: "Lua の状態(<code>lua_State</code>)を作り、標準ライブラリを開く。これを呼ぶまで <code>LoadScript</code> / <code>CallFunction</code> は失敗してよい。", when: "VM を使い始める最初に 1 回だけ呼ぶ。" },
        { sig: "void Shutdown() noexcept", desc: "Lua の状態を破棄する(<code>lua_close</code> 相当)。登録した <t>NativeFunction</t> もここで無効になる。" },
        { sig: "EScriptLanguage Language() const noexcept", ret: "言語タグ", desc: "常に <code>EScriptLanguage::Lua54</code> を返す。" },
        { sig: "TResult&lt;void&gt; LoadScript(const char* source, u32 source_len, const char* chunk_name) noexcept", ret: "成否", desc: "スクリプト文字列を 1 つの『chunk』として読み込み、<b>即時実行</b>する(<code>luaL_dostring</code> 相当)。<code>source_len</code> に 0 を渡すと <code>source</code> の長さを自動判定。<code>chunk_name</code> はエラー表示用の名前(ファイル名等)。", when: "Lua ソースを読み込んで実行したい時。文字列は所有されないので呼び出し側が寿命を保証する。" },
        { sig: "TResult&lt;void&gt; CallFunction(const char* function_name, const FScriptValue* args, u32 arg_count, FScriptValue* ret_out) noexcept", ret: "成否", desc: "Lua のグローバル関数を呼ぶ。<code>args</code> / <code>arg_count</code> で引数を渡し、<code>ret_out</code> に戻り値を受け取る(<code>nullptr</code> で戻り値を捨てる)。未登録関数や実行時エラーは失敗で返る。", when: "C++ から Lua 側のロジック(イベントハンドラ等)を呼びたい時。" },
        { sig: "TResult&lt;void&gt; RegisterNativeFunction(const char* function_name, NativeFunction fn, void* user) noexcept", ret: "成否", desc: "C++ の関数を Lua のグローバル空間に登録し、Lua 側から名前で呼べるようにする。<code>user</code> は呼び出し時に <t>NativeFunction</t> の第 3 引数へそのまま渡る。同じVMへの登録再入は拒否し、失敗時はstack・登録簿・同名globalの復元を試みる。復元中にLuaのメモリ不足が再発した場合だけglobal値を保証しない。Lua側へ退避された失敗closureは登録IDで後続登録から分離される。登録簿確保失敗はIDを消費せず、最大IDの公開開始後は再初期化しても後続登録を恒久拒否する。", when: "Lua スクリプトから C++ のゲーム機能(音を鳴らす・スポーンする等)を呼ばせたい時。" },
        { sig: "void SetGlobalNumber(const char* name, f64 value) noexcept", desc: "Lua のグローバル数値変数を設定する。スクリプト側から <code>name</code> で参照できる。" },
        { sig: "f64 GetGlobalNumber(const char* name, f64 default_value) const noexcept", ret: "数値", desc: "Lua のグローバル数値変数を読む。未定義 / 型が違う時は <code>default_value</code> を返す。" },
        { sig: "void CollectGarbage() noexcept", desc: "<t>GC</t> を 1 サイクル強制実行する(<code>lua_gc(LUA_GCCOLLECT)</code> 相当)。フレーム境界で明示的に呼ぶ運用が推奨。" },
        { sig: "u64 MemoryUsageBytes() const noexcept", ret: "バイト数", desc: "VM が現在使っているメモリ量(近似)。デバッグ HUD 等に。" },
        { sig: "using FLuaVm = CLuaVm", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CLuaVm</code> を使う。" }
      ]
    },
    {
      name: "GetDefaultLuaVm()",
      kind: "関数", header: "scripting/LuaVm.h",
      summary: "プロセスで 1 個だけ共有される既定の <code>CLuaVm</code>(<t>シングルトン</t>)への参照を返す。<code>InstallLuaAsDefault()</code> が <t>gameframework</t> の provider へ登録する実体でもある。",
      when: "アプリ全体で 1 つの Lua VM を共有したい時。通常は直接呼ばず <code>InstallLuaAsDefault()</code> 経由で <code>acs::game::GetDefaultScriptVm()</code> から使う。"
    },
    {
      name: "InstallLuaAsDefault()",
      kind: "関数", header: "scripting/LuaVm.h",
      summary: "<t>gameframework</t> の『既定 <t>ScriptVm</t> provider』に <code>GetDefaultLuaVm</code> を登録する。これを呼ぶと、以降 <code>acs::game::GetDefaultScriptVm()</code> が <b>本物の Lua 5.4 VM</b> を返すようになる。",
      when: "アプリ起動時に <b>一度だけ</b>呼ぶ。これにより上位コードは backend を意識せず <code>GetDefaultScriptVm()</code> だけで実 VM を取得でき、<code>#if</code> 分岐がこの 1 箇所だけで済む。"
    },
    {
      name: "FScriptValue",
      kind: "構造体", header: "gameframework/ScriptHost.h",
      summary: "C++ と Lua の間で値を受け渡すための<b>タグ付き共用体風の <t>POD</t></b>。<code>kind</code> で種類を、<code>v</code> で中身を表す。種類は Nil / Bool / Number / String / Handle の 5 つ。<code>CLuaVm</code> の引数 / 戻り値はすべてこの型の配列で表現する。",
      when: "<code>CallFunction</code> に引数を渡す / 戻り値を受け取る時や、<t>NativeFunction</t> の中で値を読み書きする時に使う。",
      members: [
        { sig: "EScriptValueKind kind", desc: "値の種類(既定は <code>Nil</code>)。これを見てから <code>v</code> の正しいフィールドを読む。" },
        { sig: "union { bool b; f64 num; const char* str; u32 handle; } v", desc: "中身。<code>Number</code> は <code>num</code>(整数も <code>f64</code> に丸めて格納)、文字列は <code>str</code>(<b>所有しない</b>)、<code>Handle</code> は <code>handle</code>(エンティティ等の不透明 ID)。" }
      ]
    },
    {
      name: "EScriptValueKind",
      kind: "列挙(enum)", header: "gameframework/ScriptHost.h",
      summary: "<t>FScriptValue</t> がどの種類の値かを表すタグ。Lua / Wren / Python の動的型の最大公約数。",
      when: "<code>FScriptValue</code> を作る / 読む時に <code>kind</code> へ設定・判定する。",
      members: [
        { sig: "Nil = 0", desc: "値なし。既定状態 / 戻り値なしを表す。" },
        { sig: "Bool = 1", desc: "真偽値(<code>v.b</code>)。" },
        { sig: "Number = 2", desc: "数値。<code>f64</code>(<code>v.num</code>)で統一。整数もここに丸めて入る。" },
        { sig: "String = 3", desc: "文字列(<code>v.str</code>)。<b>所有しない</b><t>ポインタ</t>。" },
        { sig: "Handle = 4", desc: "<code>u32</code> の不透明 ID(<code>v.handle</code>)。生ポインタを Lua に渡さないための安全な間接層。" }
      ]
    },
    {
      name: "FScriptCallFrame",
      kind: "構造体", header: "gameframework/ScriptHost.h",
      summary: "Lua から <t>NativeFunction</t>(登録した C++ 関数)が呼ばれた瞬間に渡される、引数と戻り値のまとめ。引数配列・件数・戻り値書き込み先だけを持つ薄い <t>POD</t>。",
      when: "<code>RegisterNativeFunction</code> で登録した C++ 関数の中で、引数を読み戻り値を書く時に使う。",
      members: [
        { sig: "const FScriptValue* args", desc: "引数配列(読み取り専用、長さは <code>arg_count</code>)。" },
        { sig: "u32 arg_count", desc: "<code>args</code> の長さ。0 でも有効(引数なし関数)。" },
        { sig: "FScriptValue* ret", desc: "戻り値の書き込み先。<code>nullptr</code> なら戻り値は捨てられる。" }
      ]
    },
    {
      name: "NativeFunction",
      kind: "関数ポインタ型", header: "gameframework/ScriptHost.h",
      summary: "Lua 側から呼べる C++ 関数の型。<code>void(*)(IScriptVm&amp; vm, FScriptCallFrame&amp; frame, void* user) noexcept</code>。<code>user</code> には登録時に渡したコンテキスト(<code>this</code> 等)がそのまま流れてくる。<b>例外を投げてはいけない</b>(Lua は C 由来で巻き戻し非対応)。",
      when: "Lua から呼ばせたい C++ 関数を <code>RegisterNativeFunction</code> に登録する時の関数シグネチャとして使う。"
    },
    {
      name: "EScriptLanguage",
      kind: "列挙(enum)", header: "gameframework/ScriptHost.h",
      summary: "VM の backend(言語)を識別するタグ。<code>CLuaVm</code> は常に <code>Lua54</code> を返す。複数 backend を併用する場合の判定に使う。",
      when: "<code>IScriptVm::Language()</code> の戻り値で、どの言語の VM かを見分けたい時。",
      members: [
        { sig: "Lua54 = 0", desc: "Lua 5.4(推奨 backend、<code>CLuaVm</code> が返す)。" },
        { sig: "Wren = 1", desc: "Wren(小規模・組み込み向け)。" },
        { sig: "Python3 = 2", desc: "CPython 3.x(重量級)。" },
        { sig: "Custom = 3", desc: "ユーザー独自実装の <code>IScriptVm</code> 派生。" }
      ]
    },
    {
      name: "スクリプト窓口の概要",
      kind: "クラス", header: "gameframework/ScriptHost.h",
      summary: "<b>ゲームコードから見たスクリプトの単一窓口</b>。<t>IScriptVm</t>* を 1 つ保持し、関数呼び出し・ファイル実行・C++ 関数登録・エラー通知をまとめる(<t>DI</t> ポイントを 1 つに絞る)。<b>vm は所有しない</b>(生成/破棄は <code>CGame</code>/<code>Scene</code> 側の責任)。コピー/<t>ムーブ</t>不可。",
      when: "ゲーム側で「スクリプトを呼ぶ」窓口が欲しい時。<code>CLuaVm</code>(実)や <code>GetVmStub()</code>(未統合時)を <code>Init()</code> で差し込んで使う。",
      members: [
        { sig: "void Init(IScriptVm* vm)", desc: "使う VM を差し込む。<code>nullptr</code> は Shutdown 相当。多重呼び出し可(後勝ち)。<b>vm は所有しない</b>。" },
        { sig: "void Shutdown()", desc: "vm 参照を切り、内部の native 登録リストもクリア(vm の破棄は呼び出し側)。" },
        { sig: "IScriptVm* Vm() const", ret: "VM or null", desc: "保持中の VM。未 Init / Shutdown 後は <code>nullptr</code>。" },
        { sig: "TResult&lt;void&gt; LoadAndRun(const wchar_t* file_path)", ret: "成否", desc: "ファイルを読み込んで <code>LoadScript</code> に流す。読み込み上限 64 MiB。失敗は subcode で区別(未設定/open 失敗/サイズ超過)。" },
        { sig: "TResult&lt;void&gt; CallGlobalFunction(const char* name, const FScriptValue* args, u32 argc, FScriptValue* ret_out)", ret: "成否", desc: "グローバル関数を呼ぶ薄い委譲。" },
        { sig: "TResult&lt;void&gt; RegisterNative(const char* name, NativeFunction fn, void* user)", ret: "成否", desc: "C++ 関数を vm に登録し、<b>ホスト内の registry にも記録</b>(backend 差し替え時の再登録の素地)。失敗時は registry にも追加しない。" },
        { sig: "void RegisterStandardBindings()", desc: "Log/Math/Time/Input/Audio 等の標準 binding を一括登録(現状はプレースホルダで件数 0)。" },
        { sig: "u32 RegisteredNativeCount() const", ret: "件数", desc: "<code>RegisterNative</code> で登録した native function の数。" },
        { sig: "void SetOnErrorCallback(ScriptErrorCallback cb, void* user)", desc: "スクリプト実行/ロードエラーを上位 UI/ログへ通知する callback を設定(<code>nullptr</code> で無効化)。" }
      ]
    },
    {
      name: "スクリプトVM fallbackの概要",
      kind: "クラス / 関数", header: "gameframework/ScriptHost.h",
      summary: "実 backend(Lua 等)が<b>未統合のときの安全側 <t>IScriptVm</t></b>。<code>Init</code>/<code>Shutdown</code> だけ no-op 成功し、<code>LoadScript</code>/<code>CallFunction</code>/<code>RegisterNativeFunction</code> は <code>kSub_NotImplemented</code> を返す。これで上位層の「スクリプトが常に失敗する」fallback を検証できる。",
      when: "<code>ACS_BUILD_SCRIPTING</code> OFF や backend 未リンクの状態でも起動シーケンスを通したい時。",
      members: [
        { sig: "IScriptVm&amp; GetVmStub()", ret: "stub 参照", desc: "プロセス内に 1 つの静的 stub(Meyers singleton)への参照。" },
        { sig: "Init() / Shutdown()", desc: "no-op 成功(起動シーケンスを通すため)。" },
        { sig: "LoadScript / CallFunction / RegisterNativeFunction", desc: "<code>kSub_NotImplemented</code>(99)を返す。" }
      ]
    },
    {
      name: "SetScriptVmProvider / GetDefaultScriptVm",
      kind: "関数", header: "gameframework/ScriptHost.h",
      summary: "<b>backend 非依存で既定 VM を取得する結線点</b>。<t>gameframework</t> は実 backend(<code>ACS::Scripting</code>)に依存できない(循環依存)ため、実 backend 側が <code>SetScriptVmProvider()</code> で「既定 VM を返す関数」を登録し、ゲームは <code>GetDefaultScriptVm()</code> で取得する。未登録なら <code>GetVmStub()</code> を返す。",
      when: "アプリ起動時に 1 度 backend を結線したい時。以降はどこでも backend 非依存に既定 VM を引ける。",
      members: [
        { sig: "using ScriptVmProvider = IScriptVm&amp; (*)() noexcept", desc: "既定 VM を返す関数ポインタ型。" },
        { sig: "void SetScriptVmProvider(ScriptVmProvider provider)", desc: "provider を登録(<code>nullptr</code> で stub に戻す。後勝ち)。実 backend の <code>Install*</code> から呼ぶ。" },
        { sig: "IScriptVm&amp; GetDefaultScriptVm()", ret: "VM 参照", desc: "provider 登録済みなら実 VM、未登録なら <code>GetVmStub()</code>。" }
      ]
    },
    {
      name: "script_err:: サブコード",
      kind: "定数(u16)", header: "gameframework/ScriptHost.h",
      summary: "<code>TResult</code> が <t>Err</t> の時に <code>err.subcode</code> で原因を見分ける定数群(カテゴリは <code>ErrCategory::Generic</code>)。<code>TSaveSlot</code>/<code>FMlRuntime</code> と同じ流儀で番号を固定。",
      when: "<code>LoadAndRun</code>/<code>CallGlobalFunction</code> 等の失敗理由で分岐したい時に <code>r.Error().subcode == ...</code> で照合する。",
      members: [
        { sig: "kSub_NotImplemented = 99", desc: "stub / backend 未統合。" },
        { sig: "kSub_InvalidArg = 1", desc: "nullptr / 不正引数。" },
        { sig: "kSub_NotInitialized = 2", desc: "<code>Init()</code> 前の API 呼び出し。" },
        { sig: "kSub_LoadFailed = 10", desc: "<code>LoadScript</code> 失敗(parse error 等)。" },
        { sig: "kSub_CallFailed = 11", desc: "<code>CallFunction</code> 失敗(runtime error 等)。" },
        { sig: "kSub_FileNotFound = 20 / kSub_FileTooLarge = 21", desc: "<code>LoadAndRun</code> のファイル読み込み失敗 / 上限超過。" },
        { sig: "kSub_NoVm = 30", desc: "<code>CScriptHost::Init</code> 未呼出。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "Lua": "ゲーム組み込みで広く使われる軽量スクリプト言語。C++ を再ビルドせずにロジックを差し替えられる。",
  "Lua 5.4": "<t>Lua</t> の安定版の 1 つ。ACS が <code>CLuaVm</code> として同梱する本物のランタイム。",
  "VM": "Virtual Machine。スクリプトを読み込んで実行する仮想機械。<code>CLuaVm</code> は <t>Lua</t> の VM。",
  "IScriptVm": "スクリプト VM の純粋仮想<t>インターフェース</t>。Lua/Wren/Python 等を差し替えるための seam。<code>CLuaVm</code> がこれを実装する。",
  "NativeFunction": "<t>Lua</t> 側から呼べる C++ 関数の型。例外を投げてはいけない。",
  "GC": "Garbage Collection(ガベージコレクション)。不要になったメモリを自動回収する仕組み。<t>Lua</t> が内部で行う。",
  "Pimpl": "実装の詳細を別構造体へ隠し、ヘッダに公開しない手法。<code>CLuaVm</code> は <code>lua_State*</code> をこれで隠す。",
  "シングルトン": "プロセス内に 1 個だけ存在するインスタンス。<code>GetDefaultLuaVm()</code> はこれを返す。",
  "DI": "Dependency Injection(依存性注入)。使う実装を外から差し込み、呼び出し側を具体実装に依存させない設計。<code>CScriptHost</code> は VM を差し替え可能にする。"
});
