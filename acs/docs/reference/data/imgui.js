/* ACS リファレンス — imgui モジュール。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > は &lt; &gt;。 */
ACS_REF.modules.push({
  id: "imgui",
  order: 59,
  title: "imgui — ImGui 連携",
  blurb: "デバッグ用の <t>GUI</t> ライブラリ <t>Dear ImGui</t> を ACS の<t>ウィンドウ</t>と<t>レンダラ</t>に繋ぐ薄いラッパです。たった数行で、開発中のパラメータ調整パネルやデバッグ表示を画面に出せます。",
  types: [
    {
      name: "FImGuiCtx",
      kind: "クラス",
      header: "imgui/ImGuiContext.h",
      summary: "<t>Dear ImGui</t> を ACS に統合する入口クラス。内部で <b>Win32 backend</b>(入力)と <b>DX12 backend</b>(描画)を初期化し、<code>ImGui::Begin()</code> などの素の ImGui API をそのまま使えるようにします。<t>コピー</t>は禁止(片付けを 1 つに保つため)。",
      when: "ゲームやツールに『その場でいじれるデバッグパネル』を足したい時。スライダーでパラメータを調整したり、内部状態をテキスト表示したりするのに使います。",
      members: [
        { sig: "TResult&lt;void&gt; Init(FWindow&amp; window, CRenderer&amp; renderer)", ret: "成功 or エラー", desc: "<t>ImGui</t> を起動し、与えた<t>ウィンドウ</t>と<t>レンダラ</t>に紐付ける。フォント用の <t>SRV ヒープ</t>もここで確保する。<t>レンダラ</t>が未初期化だとエラー(<t>Result</t>)を返す。", when: "アプリ起動時に 1 回だけ。失敗し得るので戻り値を確認する。"},
        { sig: "void NewFrame()", desc: "新しいフレームの ImGui 入力を開始する。この後に <code>ImGui::Begin()</code> 等の UI 構築を書く。<t>レンダラ</t>の <code>BeginFrame()</code> の<b>後</b>に呼ぶ。", when: "毎フレーム、UI を組み立てる直前に 1 回。" },
        { sig: "void Render()", desc: "組み立てた UI の描画コマンドを、今のコマンドリストに発行する。<t>レンダラ</t>の <code>EndFrame()</code> の<b>前</b>に呼ぶ。コマンドリストが無い場合は安全に何もしない。", when: "毎フレーム、UI を組み終えた直後に 1 回。" },
        { sig: "void OnEvent(const Event&amp; e)", desc: "<t>ウィンドウ</t>イベント(マウス移動・クリック・ホイール・文字入力など)を ImGui に転送する。これを呼ばないと UI がマウス操作に反応しない。", when: "<code>CApplication::OnEvent</code> など、イベントを受け取る場所から毎回呼ぶ。"},
        { sig: "void Shutdown()", desc: "<t>ImGui</t> と両 backend を破棄し、確保した <t>SRV ヒープ</t>を解放する。何度呼んでも安全(二重解放しない)。<t>デストラクタ</t>からも自動で呼ばれる。", when: "アプリ終了時。明示的に呼ばなくてもオブジェクト破棄時に走る。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "Dear ImGui": "C++ 向けの定番<t>GUI</t>ライブラリ。毎フレーム関数を呼ぶだけでパネルやスライダーを描ける『即時モード GUI』。デバッグ用 UI に最適。",
  "ImGui": "<t>Dear ImGui</t> の略称・名前空間。<code>ImGui::Begin()</code> のように呼び出す。",
  "GUI": "ボタンやスライダーなど、マウスで操作するグラフィカルな画面要素のこと。",
  "Win32 backend": "ImGui がマウス・キーボード入力を Windows から受け取るための連携部分。",
  "DX12 backend": "ImGui の描画を <t>DirectX 12</t> 経由で画面に出すための連携部分。",
  "SRV ヒープ": "GPU 上でテクスチャ(ImGui のフォント等)を参照するための記述子をまとめて置く領域。"
});
