/* ACS リファレンス — ui モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "ui",
  order: 33,
  title: "ui — UI 部品",
  blurb: "ボタン・スライダー・テキスト入力などの<t>ウィジェット</t>を木構造で組み立て、画面に描いて操作を受け取るための部品集です。<t>retained-mode</t>(ツリーを保持し続ける方式)で、各部品は <t>MVVM</t> の <t>Observable</t> プロパティを公開するので画面とデータを直結できます。",
  types: [
    {
      name: "Widget",
      kind: "基底クラス", header: "ui/Widget.h",
      summary: "すべての UI 部品の<b>共通の親</b>。子を <t>TUniquePtr</t> で所有してツリーを作り、<t>レイアウト</t>(配置)・描画・<t>ヒットテスト</t>(クリック判定)・入力イベントの仮想メソッドを持ちます。コピー不可。",
      when: "ボタンやパネルなど具体的な部品はこれを継承して作る。直接インスタンス化するより、派生型(<code>StackPanel</code> / <code>Button</code> 等)を使うのが普通。",
      sample: "StackPanel root;\nButton* btn = root.Add&lt;Button&gt;(\"OK\");  // 子を追加して生ポインタを得る\nroot.Layout(0, 0, 1280, 720);          // 画面サイズで配置を確定\n// 毎フレーム ui_renderer.Render(root, ...) で描画",
      members: [
        { sig: "template&lt;W,Args...&gt; W* Add(Args&amp;&amp;... args)", ret: "追加した子の生ポインタ", desc: "子<t>ウィジェット</t>を <code>W</code> 型で作って自分の子に取り込み、その生<t>ポインタ</t>を返す。所有権は親が持つ。", sample: "Label* l = root.Add&lt;Label&gt;(\"スコア\");", when: "UI ツリーに部品を足す時の基本操作。" },
        { sig: "Widget* Parent() const", ret: "親 or null", desc: "自分を取り込んでいる親を返す。ルートなら null。" },
        { sig: "usize ChildCount() const", ret: "子の数", desc: "直接の子<t>ウィジェット</t>の個数。" },
        { sig: "Widget* Child(usize i) const", ret: "i 番目の子", desc: "範囲外なら null。" },
        { sig: "virtual void Layout(f32 x, f32 y, f32 w, f32 h)", desc: "自分の絶対座標(<code>rect</code>)を確定し、子があれば配置する。派生型ごとに並べ方が違う。", when: "毎フレーム描画前に、ルートに対して画面サイズで 1 回呼ぶ。" },
        { sig: "virtual void Render(FUiRenderer&amp; r)", desc: "自分の見た目を描く。既定は <code>visible</code> な子を再帰描画するだけ。", when: "<code>FUiRenderer::Render</code> から自動で呼ばれる。手で呼ばない。" },
        { sig: "virtual bool HitTest(f32 px, f32 py) const", ret: "拾うか", desc: "点 (px,py) が自分の領域内なら true(=その入力を自分が拾う)。既定は <code>rect.Contains</code>。" },
        { sig: "Widget* HitTestRecursive(f32 px, f32 py)", ret: "当たった一番上の子", desc: "ツリーを上に描かれた子から優先して<t>ヒットテスト</t>し、最初に当たった<t>ウィジェット</t>を返す。" },
        { sig: "virtual void OnPointerDown / OnPointerUp / OnPointerMove(f32 px, f32 py)", desc: "マウス押下/離す/移動が来た時に呼ばれる。既定は何もしない。派生型で上書きする。" },
        { sig: "virtual void OnTextInput(u32 codepoint)", desc: "文字入力が来た時に呼ばれる(<code>TextInput</code> 等が使う)。" },
        { sig: "virtual void OnKey(i32 key, bool pressed)", desc: "キー押下/離しが来た時に呼ばれる。" },
        { sig: "bool visible", ret: "表示するか", desc: "false にするとその<t>ウィジェット</t>(と子)は描画・配置されない。" },
        { sig: "FUiRect rect", desc: "<code>Layout</code> 後に確定する<b>絶対座標</b>(画面上の最終的な矩形)。" },
        { sig: "FUiRect requested", desc: "希望サイズ。0 は『親のレイアウトに任せる』の意味。" },
        { sig: "bool hovered / focused / pressed", desc: "マウスが乗っている/フォーカス中/押下中の状態フラグ。<code>FUiInput</code> や派生型が更新する。" }
      ]
    },
    {
      name: "StackPanel",
      kind: "クラス", header: "ui/Widget.h",
      summary: "子<t>ウィジェット</t>を<b>縦または横に等間隔で並べる</b><t>レイアウト</t>パネル。並べる向き・間隔・余白を指定できる。",
      when: "ボタンやラベルを上から順に積みたい/横一列に並べたい時。UI の一番外側のルートとしてもよく使う。",
      sample: "StackPanel panel;\npanel.dir = EStackDir::Vertical;  // 縦積み\npanel.spacing = 6.0f;             // 部品の間隔\npanel.Add&lt;Label&gt;(\"設定\");\npanel.Add&lt;Button&gt;(\"閉じる\");",
      members: [
        { sig: "EStackDir dir", desc: "並べる向き。<code>Vertical</code>(縦) or <code>Horizontal</code>(横)。既定は縦。" },
        { sig: "f32 spacing", desc: "部品どうしの間隔(ピクセル)。既定 4.0。" },
        { sig: "FUiPadding padding", desc: "パネル内側の余白(左/上/右/下)。既定は四辺 8。" }
      ]
    },
    {
      name: "Container",
      kind: "クラス", header: "ui/Widget.h",
      summary: "<b>自分自身は何も描かず</b>、親からもらった範囲をそのまま全部の子に渡す透過パネル。重ね合わせ(同じ場所に複数を配置)に使える。",
      when: "背景は出さずに、複数の子を同じ領域に重ねたい/グループ化だけしたい時。",
      sample: "Container layer;\nlayer.Add&lt;Label&gt;(\"背景の上に乗せる\");\nlayer.Add&lt;Button&gt;(\"OK\");  // 同じ範囲に重なる"
    },
    {
      name: "Label",
      kind: "クラス", header: "ui/Widgets.h",
      summary: "<b>静的テキスト</b>を表示するだけの<t>ウィジェット</t>。文字列は <t>Observable</t> なので、データを <t>Bind</t> すれば自動で表示が更新される。",
      when: "見出し・説明・スコア表示など、押せない文字を出したい時。",
      sample: "Label* l = root.Add&lt;Label&gt;(\"HP\");\nl-&gt;text.Set(FString{\"HP: 100\"});  // 文字を変える\n// vm.score と Bind すれば自動更新も可",
      members: [
        { sig: "Observable&lt;FString&gt; text", desc: "表示する文字列。<code>Set</code> で変更、<code>Bind</code> で<t>ViewModel</t>と同期できる。" }
      ]
    },
    {
      name: "Button",
      kind: "クラス", header: "ui/Widgets.h",
      summary: "<b>押せるボタン</b>。クリック(同じボタン上で押して離す)が完結した瞬間に <code>clicked</code> が一瞬だけ true になり、<code>Subscribe</code> した処理が走る。",
      when: "『OK』『攻撃』などの 1 回押すと何かが起きるボタンが欲しい時。",
      sample: "Button* btn = root.Add&lt;Button&gt;(\"開始\");\nbtn-&gt;clicked.Subscribe([](bool on){\n    if (on) StartGame();   // true の瞬間だけ反応\n});",
      members: [
        { sig: "Observable&lt;FString&gt; text", desc: "ボタンに出すラベル文字列。" },
        { sig: "Observable&lt;bool&gt; clicked", desc: "クリック完了時に <code>true→false</code> と<b>パルス発火</b>する。<code>Subscribe</code> 側は true の時だけ反応する。", sample: "btn-&gt;clicked.Subscribe([](bool on){ if(on) DoIt(); });", when: "ボタンの押下を受け取る標準の方法。" }
      ]
    },
    {
      name: "Slider",
      kind: "クラス", header: "ui/Widgets.h",
      summary: "<b>つまみをドラッグして範囲内の数値を選ぶ</b><t>ウィジェット</t>。下限〜上限を指定し、現在値は <t>Observable</t> で公開される。",
      when: "音量・難易度・パラメータなど連続値をマウスで調整させたい時。<code>value</code> を <t>ViewModel</t> と双方向 <t>Bind</t> すると便利。",
      sample: "Slider* sl = root.Add&lt;Slider&gt;(0.0f, 100.0f);  // 0〜100\nsl-&gt;value.Subscribe([](f32 v){ SetVolume(v); });\nauto bind = MakeTwoWayBind(vm.hp, sl-&gt;value);  // 双方向同期",
      members: [
        { sig: "Observable&lt;f32&gt; value", desc: "現在の値。ドラッグで更新され、<code>Subscribe</code> / <code>Bind</code> で受け取れる。" },
        { sig: "f32 min_value / max_value", desc: "選べる値の下限・上限。コンストラクタ <code>Slider(min, max)</code> で指定。" }
      ]
    },
    {
      name: "Checkbox",
      kind: "クラス", header: "ui/Widgets.h",
      summary: "<b>オン/オフを切り替える</b>チェックボックス。クリックするたび <code>checked</code> が反転する。横にラベルを出せる。",
      when: "『フルスクリーン』『サウンド有効』などの真偽の設定項目に。",
      sample: "Checkbox* cb = root.Add&lt;Checkbox&gt;(\"フルスクリーン\");\ncb-&gt;checked.Set(true);\ncb-&gt;checked.Subscribe([](bool on){ SetFullscreen(on); });",
      members: [
        { sig: "Observable&lt;bool&gt; checked", desc: "オン/オフの状態。クリックで反転、<code>Subscribe</code> / <code>Bind</code> で監視できる。" },
        { sig: "Observable&lt;FString&gt; text", desc: "チェックボックス横に出すラベル文字列。" }
      ]
    },
    {
      name: "TextInput",
      kind: "クラス", header: "ui/Widgets.h",
      summary: "<b>1 行のテキスト入力欄</b>。クリックでフォーカスを得て、文字入力で末尾に追加、Backspace で 1 文字削除する(<t>ASCII</t> 範囲のみ対応の簡易実装)。",
      when: "プレイヤー名やチャットなど、短い文字をキーボードで入力させたい時。",
      sample: "TextInput* in = root.Add&lt;TextInput&gt;();\nin-&gt;text.Subscribe([](const FString&amp; s){\n    SetPlayerName(s);\n});\n// クリックでフォーカス → タイプした文字が text に入る",
      members: [
        { sig: "Observable&lt;FString&gt; text", desc: "入力中の文字列。入力に応じて更新され、<code>Subscribe</code> / <code>Bind</code> で受け取れる。" }
      ]
    },
    {
      name: "FUiRenderer",
      kind: "クラス", header: "ui/UiRenderer.h",
      summary: "<t>Widget</t> ツリーを <code>FSpriteBatch</code> + <code>Font</code> で<b>1 フレーム分まとめて描画</b>する描画役。矩形・枠線・文字を出す低レベルヘルパも持ち、各<t>ウィジェット</t>の <code>Render</code> がこれに描画を依頼する。コピー不可。",
      when: "UI を画面に出す中心。起動時に <code>Init</code> し、毎フレーム <code>Render(root, ...)</code> を呼ぶ。",
      sample: "FUiRenderer ur;\nur.Init(*device, GetRenderer().ColorFormat(), default_font);\n// 毎フレーム:\nroot.Layout(0, 0, w, h);\nur.Render(root, *cmd, w, h);",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&amp; device, EFormat rt_format, Font* default_font)", ret: "成否(<t>Result</t>)", desc: "内部の <code>FSpriteBatch</code> を初期化し、既定<t>フォント</t>と描画先フォーマットを設定する。", when: "アプリ起動時に 1 回。" },
        { sig: "void Shutdown()", desc: "確保した描画リソースを解放する。" },
        { sig: "void Render(Widget&amp; root, IRhiCommandList&amp; cmd, u32 screen_w, u32 screen_h)", desc: "ルートから<t>ウィジェット</t>を再帰描画し、コマンドリストに発行する。", when: "毎フレーム、<code>Layout</code> の後に呼ぶ。" },
        { sig: "void DrawRect(f32 x, f32 y, f32 w, f32 h, const FVec4&amp; color)", desc: "塗りつぶし矩形を描く。<t>ウィジェット</t>の <code>Render</code> 内から使う低レベル部品。" },
        { sig: "void DrawRectOutline(f32 x, f32 y, f32 w, f32 h, const FVec4&amp; color, f32 thickness = 1.0f)", desc: "枠線(中抜き矩形)を描く。" },
        { sig: "void DrawText(const char* utf8, f32 x, f32 y, const FVec4&amp; color)", desc: "文字列を描く。" },
        { sig: "const FUiColors&amp; Colors() / FUiColors&amp; Colors()", ret: "テーマ色", desc: "<t>ウィジェット</t>が参照するテーマ色一式。非 const 版で書き換えると見た目を一括変更できる。", sample: "ur.Colors().button_bg = { 0.8f, 0.2f, 0.2f, 1.0f };" },
        { sig: "Font* DefaultFont() const", ret: "既定フォント", desc: "<code>Init</code> で渡した既定<t>フォント</t>。所有はしない。" }
      ]
    },
    {
      name: "FUiInput",
      kind: "クラス", header: "ui/UiRenderer.h",
      summary: "<code>FWindow</code> のマウス/キーイベントを取り出して、<t>ヒットテスト</t>で当たった<t>ウィジェット</t>の <code>On*</code> メソッドへ<b>配信</b>する入力ディスパッチャ。hover / 押下 / フォーカスの状態も追跡する。",
      when: "UI に入力を効かせたい時。毎フレーム <code>Dispatch(root)</code> を呼ぶだけで、クリックやドラッグが各<t>ウィジェット</t>に届く。",
      sample: "FUiInput ui_input;\n// 毎フレーム:\nui_input.Dispatch(root);  // マウス/キー → 該当 widget の On* へ",
      members: [
        { sig: "void Dispatch(Widget&amp; root)", desc: "現在のマウス位置・クリック・押下キーを <t>Input</t> から取得し、ルートを<t>ヒットテスト</t>して該当<t>ウィジェット</t>の <code>OnPointerDown/Up/Move</code> や <code>OnTextInput</code> / <code>OnKey</code> に届ける。hover / focus の更新も行う。", when: "毎フレーム 1 回呼ぶ。" }
      ]
    },
    {
      name: "FUiRect",
      kind: "構造体", header: "ui/Widget.h",
      summary: "UI 座標の<b>矩形</b>。左上原点・ピクセル単位で <code>x, y, w, h</code> を持つ。点が中に入っているか判定できる。",
      when: "<t>ウィジェット</t>の領域(<code>rect</code> / <code>requested</code>)を表す基本型。当たり判定にも使う。",
      sample: "FUiRect r{ 10, 10, 100, 40 };\nif (r.Contains(mouse_x, mouse_y)) { /* 領域内 */ }",
      members: [
        { sig: "f32 x, y, w, h", desc: "左上座標と幅・高さ(ピクセル)。" },
        { sig: "bool Contains(f32 px, f32 py) const", ret: "内側か", desc: "点 (px,py) が矩形の内側なら true。" }
      ]
    },
    {
      name: "FUiPadding",
      kind: "構造体", header: "ui/Widget.h",
      summary: "<b>四辺の余白</b>(左 <code>l</code> / 上 <code>t</code> / 右 <code>r</code> / 下 <code>b</code>)を表す小さな構造体。",
      when: "<code>StackPanel</code> の内側余白など、要素の周囲の空きを指定する時。",
      sample: "FUiPadding pad{ 12, 8, 12, 8 };  // 左右12, 上下8\npanel.padding = pad;"
    },
    {
      name: "EStackDir",
      kind: "列挙(enum)", header: "ui/Widget.h",
      summary: "<code>StackPanel</code> が子を並べる<b>方向</b>。<code>Vertical</code>(縦) と <code>Horizontal</code>(横)。",
      when: "パネルを縦積みにするか横並びにするか決める時。",
      sample: "panel.dir = EStackDir::Horizontal;  // 横一列に並べる"
    },
    {
      name: "FUiColors",
      kind: "構造体", header: "ui/Widgets.h",
      summary: "UI 全体の<b>テーマ色</b>をまとめた構造体。パネル背景・ボタン・スライダー・チェック・文字・入力欄などの色を <code>FVec4</code>(RGBA) で持つ。",
      when: "アプリの配色を一括で変えたい時。<code>FUiRenderer::Colors()</code> から取得して書き換える。",
      sample: "FUiColors&amp; c = ur.Colors();\nc.button_bg   = { 0.2f, 0.6f, 0.3f, 1.0f };\nc.button_hover = { 0.3f, 0.8f, 0.4f, 1.0f };",
      members: [
        { sig: "FVec4 panel_bg / panel_border", desc: "パネルの背景色・枠線色。" },
        { sig: "FVec4 button_bg / button_hover / button_press / button_text", desc: "ボタンの通常/ホバー/押下/文字色。" },
        { sig: "FVec4 slider_track / slider_fill / slider_knob", desc: "スライダーの溝/塗り/つまみの色。" },
        { sig: "FVec4 check_box / check_mark", desc: "チェックボックスの枠/チェック印の色。" },
        { sig: "FVec4 text / text_dim / input_bg / input_focus", desc: "文字色(通常/淡色)と入力欄の背景/フォーカス色。" }
      ]
    },
    {
      name: "DefaultUiColors()",
      kind: "関数", header: "ui/Widgets.h",
      summary: "プログラム共通の<b>既定テーマ色</b>(<code>FUiColors</code>)への参照を返す。",
      when: "個別の <code>FUiRenderer</code> を作る前に、アプリ全体の既定色を参照/調整したい時。",
      sample: "FUiColors&amp; c = DefaultUiColors();\nc.text = { 0.9f, 0.9f, 0.9f, 1.0f };"
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "ウィジェット": "ボタンやラベルなど画面上の UI 部品 1 つ 1 つのこと。<t>Widget</t> を継承して作る。",
  "retained-mode": "毎フレーム UI を作り直すのではなく、部品のツリーを保持し続けて使う方式。対義は immediate-mode(ImGui 等)。",
  "レイアウト": "各<t>ウィジェット</t>を画面のどこにどの大きさで置くかを決めて配置すること。<code>Layout</code> で行う。",
  "ヒットテスト": "画面上のある点(マウス位置など)がどの<t>ウィジェット</t>の上にあるかを判定すること。",
  "ASCII": "英数記号など 0〜127 の基本的な文字コード。日本語などは含まない。"
});
