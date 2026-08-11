/* ACS リファレンス — ui モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "ui",
  order: 33,
  title: "ui — UI 部品",
  blurb: "ボタン・スライダー・テキスト入力などの<t>ウィジェット</t>を木構造で組み立て、画面に描いて操作を受け取るための部品集です。<t>retained-mode</t>(ツリーを保持し続ける方式)で、各部品は <t>MVVM</t> の <code>TObservable</code> プロパティを公開するので画面とデータを直結できます。",
  types: [
    {
      name: "AWidget",
      kind: "基底クラス", header: "ui/Widget.h",
      summary: "すべての UI 部品の<b>共通の親</b>。子を <t>TUniquePtr</t> で所有してツリーを作り、<t>レイアウト</t>(配置)・描画・<t>ヒットテスト</t>(クリック判定)・入力イベントの仮想メソッドを持ちます。コピー不可。",
      when: "ボタンやパネルなど具体的な部品はこれを継承して作る。直接インスタンス化するより、派生型(<code>AStackPanel</code> / <code>AButton</code> 等)を使うのが普通。",
      members: [
        { sig: "template&lt;W,Args...&gt; W* Add(Args&amp;&amp;... args)", ret: "追加した子の生ポインタ", desc: "子<t>ウィジェット</t>を <code>W</code> 型で作って自分の子に取り込み、その生<t>ポインタ</t>を返す。所有権は親が持つ。", when: "UI ツリーに部品を足す時の基本操作。" },
        { sig: "AWidget* Parent() const", ret: "親 or null", desc: "自分を取り込んでいる親を返す。ルートなら null。" },
        { sig: "usize ChildCount() const", ret: "子の数", desc: "直接の子<t>ウィジェット</t>の個数。" },
        { sig: "AWidget* Child(usize i) const", ret: "i 番目の子", desc: "範囲外なら null。" },
        { sig: "virtual void Layout(f32 x, f32 y, f32 w, f32 h)", desc: "自分の絶対座標(<code>rect</code>)を確定し、子があれば配置する。派生型ごとに並べ方が違う。", when: "<code>CUiRenderer::Render</code> が毎フレーム root に対して呼ぶ。layout 単体の検証時は直接呼べる。" },
        { sig: "virtual void Render(CUiRenderer&amp; r)", desc: "自分の見た目を描く。既定は <code>visible</code> な子を再帰描画するだけ。", when: "<code>CUiRenderer::Render</code> から自動で呼ばれる。手で呼ばない。" },
        { sig: "virtual bool HitTest(f32 px, f32 py) const", ret: "拾うか", desc: "点 (px,py) が自分の領域内なら true(=その入力を自分が拾う)。既定は <code>rect.Contains</code>。" },
        { sig: "AWidget* HitTestRecursive(f32 px, f32 py)", ret: "当たった一番上の子", desc: "ツリーを上に描かれた子から優先して<t>ヒットテスト</t>し、最初に当たった<t>ウィジェット</t>を返す。" },
        { sig: "virtual void OnPointerDown / OnPointerUp / OnPointerMove(f32 px, f32 py)", desc: "マウス押下/離す/移動が来た時に呼ばれる。既定は何もしない。派生型で上書きする。" },
        { sig: "virtual void OnTextInput(u32 codepoint)", desc: "文字入力が来た時に呼ばれる(<code>ATextInput</code> 等が使う)。" },
        { sig: "virtual void OnKey(i32 key, bool pressed)", desc: "従来互換のキー押下/解放イベント。<code>CUiInput</code> は押下を true、解放を false で配信する。修飾キー付き既定実装からも転送されるため、既存の override はそのまま動作する。" },
        { sig: "virtual void OnKey(i32 key, bool pressed, const FUiKeyModifiers&amp; modifiers)", desc: "Shift / Ctrl / Alt / Super の押下中スナップショットを含むキー押下/解放イベント。既定実装は2引数版へ転送する。" },
        { sig: "bool visible", ret: "表示するか", desc: "false にするとその<t>ウィジェット</t>(と子)は描画・配置されない。" },
        { sig: "FUiRect rect", desc: "<code>Layout</code> 後に確定する<b>絶対座標</b>(画面上の最終的な矩形)。" },
        { sig: "FUiRect requested", desc: "希望サイズ。0 は『親のレイアウトに任せる』の意味。" },
        { sig: "bool hovered / focused / pressed", desc: "マウスが乗っている/フォーカス中/押下中の状態フラグ。<code>CUiInput</code> や派生型が更新する。" },
        { sig: "using FWidget = AWidget", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AWidget</code> を使う。" }
      ]
    },
    {
      name: "AStackPanel",
      kind: "クラス", header: "ui/Widget.h",
      summary: "子<t>ウィジェット</t>を<b>縦または横に等間隔で並べる</b><t>レイアウト</t>パネル。並べる向き・間隔・余白を指定できる。",
      when: "ボタンやラベルを上から順に積みたい/横一列に並べたい時。UI の一番外側のルートとしてもよく使う。",
      members: [
        { sig: "EStackDir dir", desc: "並べる向き。<code>Vertical</code>(縦) or <code>Horizontal</code>(横)。既定は縦。" },
        { sig: "f32 spacing", desc: "部品どうしの間隔(ピクセル)。既定 4.0。" },
        { sig: "FUiPadding padding", desc: "パネル内側の余白(左/上/右/下)。既定は四辺 8。" },
        { sig: "using FStackPanel = AStackPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AStackPanel</code> を使う。" }
      ]
    },
    {
      name: "AContainer",
      kind: "クラス", header: "ui/Widget.h",
      summary: "<b>自分自身は何も描かず</b>、親からもらった範囲をそのまま全部の子に渡す透過パネル。重ね合わせ(同じ場所に複数を配置)に使える。",
      when: "背景は出さずに、複数の子を同じ領域に重ねたい/グループ化だけしたい時。",
      members: [
        { sig: "（継承メンバ）", desc: "独自のメンバは持たず、<code>Layout(x,y,w,h)</code> だけを上書きして全子に親と同じ矩形を渡す。<code>Add&lt;W&gt;()</code> / <code>Parent()</code> / <code>Child()</code> / <code>ChildCount()</code> / <code>HitTestRecursive()</code> や <code>visible</code> / <code>rect</code> / <code>requested</code> などはすべて基底 <t>AWidget</t> から継承する。<code>Render</code> も基底既定（子の再帰描画のみ）で、自身は何も描かない。" },
        { sig: "using FContainer = AContainer", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AContainer</code> を使う。" }
      ]
    },
    {
      name: "ALabel",
      kind: "クラス", header: "ui/Widgets.h",
      summary: "<b>静的テキスト</b>を表示するだけの<t>ウィジェット</t>。文字列は <t>TObservable</t> なので、データを <t>Bind</t> すれば自動で表示が更新される。",
      when: "見出し・説明・スコア表示など、押せない文字を出したい時。",
      members: [
        { sig: "TObservable&lt;FString&gt; text", desc: "表示する文字列。<code>Set</code> で変更、<code>Bind</code> で<t>ViewModel</t>と同期できる。" },
        { sig: "using FLabel = ALabel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ALabel</code> を使う。" }
      ]
    },
    {
      name: "AButton",
      kind: "クラス", header: "ui/Widgets.h",
      summary: "<b>押せるボタン</b>。クリック(同じボタン上で押して離す)が完結した瞬間に <code>clicked</code> が一瞬だけ true になり、<code>Subscribe</code> した処理が走る。",
      when: "『OK』『攻撃』などの 1 回押すと何かが起きるボタンが欲しい時。",
      members: [
        { sig: "TObservable&lt;FString&gt; text", desc: "ボタンに出すラベル文字列。" },
        { sig: "TObservable&lt;bool&gt; clicked", desc: "クリック完了時に <code>true→false</code> と<b>パルス発火</b>する。<code>Subscribe</code> 側は true の時だけ反応する。subscriber が current button を親ツリーから同期除去した場合は、破棄後の false 書き戻しを行わず安全に終了する。", when: "ボタンの押下を受け取る標準の方法。" },
        { sig: "using FButton = AButton", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AButton</code> を使う。" }
      ]
    },
    {
      name: "ASlider",
      kind: "クラス", header: "ui/Widgets.h",
      summary: "<b>つまみをドラッグして範囲内の数値を選ぶ</b><t>ウィジェット</t>。下限〜上限を指定し、現在値は <t>TObservable</t> で公開される。",
      when: "音量・難易度・パラメータなど連続値をマウスで調整させたい時。<code>value</code> を <t>ViewModel</t> と双方向 <t>Bind</t> すると便利。",
      members: [
        { sig: "TObservable&lt;f32&gt; value", desc: "現在の値。ドラッグで更新され、<code>Subscribe</code> / <code>Bind</code> で受け取れる。" },
        { sig: "f32 min_value / max_value", desc: "選べる値の下限・上限。コンストラクタ <code>ASlider(min, max)</code> で指定。" },
        { sig: "using FSlider = ASlider", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ASlider</code> を使う。" }
      ]
    },
    {
      name: "ACheckbox",
      kind: "クラス", header: "ui/Widgets.h",
      summary: "<b>オン/オフを切り替える</b>チェックボックス。クリックするたび <code>checked</code> が反転する。横にラベルを出せる。",
      when: "『フルスクリーン』『サウンド有効』などの真偽の設定項目に。",
      members: [
        { sig: "TObservable&lt;bool&gt; checked", desc: "オン/オフの状態。クリックで反転、<code>Subscribe</code> / <code>Bind</code> で監視できる。" },
        { sig: "TObservable&lt;FString&gt; text", desc: "チェックボックス横に出すラベル文字列。" },
        { sig: "using FCheckbox = ACheckbox", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ACheckbox</code> を使う。" }
      ]
    },
    {
      name: "ATextInput",
      kind: "クラス", header: "ui/Widgets.h",
      summary: "<b>1 行の UTF-8 テキスト入力欄</b>。コードポイント境界を保つカーソル移動・選択・置換・Backspace/Delete・Home/End、Shift による選択拡張、Ctrl+A と、確保量を制限する byte 上限を持つ。クリック時のカーソルは現行 pointer API の制約から末尾へ移動する。",
      when: "プレイヤー名やチャットなど、短い文字をキーボードで入力させたい時。プログラム側から UTF-8 byte 範囲を選択して置換・削除する用途にも使える。",
      members: [
        { sig: "TObservable&lt;FString&gt; text", desc: "入力中の UTF-8 文字列。編集成功時に更新され、<code>Subscribe</code> / <code>Bind</code> で受け取れる。" },
        { sig: "bool TryInsertCodepoint(u32 codepoint)", ret: "挿入できたか", desc: "Unicode scalar value を現在の cursor へ挿入する。選択中は範囲を置換する。無効値・上限超過・確保失敗では文字列、cursor、選択 anchor、末尾追従状態を変更せず、<code>text</code> の通知も発生させない。" },
        { sig: "bool TryEraseBeforeCursor() / bool TryEraseAtCursor()", desc: "選択中は選択範囲を優先して削除し、それ以外は cursor の前または位置上の 1 コードポイントを UTF-8 境界単位で削除する。確保失敗時は編集状態と通知回数を維持する。" },
        { sig: "void MoveCursorLeft/Right/ToStart/ToEnd()", desc: "コードポイント境界を保って cursor を移動する。選択中の Left/Right は選択の先頭/末尾へ折り畳む。" },
        { sig: "void OnKey(i32 key, bool pressed, const FUiKeyModifiers&amp; modifiers) noexcept", desc: "Shift+Left/Right/Home/End で anchor を固定した選択拡張・縮小・方向反転を行い、Ctrl+A で全体を選択する。Ctrl+Alt の AltGr は全選択と誤認しない。3引数版は modifier snapshot を保持して仮想2引数版へ転送するため、2引数版だけを override した既存 ATextInput 派生にも実 Dispatch の押下/解放が届く。" },
        { sig: "usize CursorByteOffset() / bool TrySetCursorByteOffset(usize)", desc: "cursor の UTF-8 byte offset を取得/設定する。コードポイント途中への設定は拒否し、成功時は選択を解除する。" },
        { sig: "bool HasSelection() / usize SelectionStart() / usize SelectionEnd()", desc: "空でない選択の有無と、正規化済みの選択範囲を UTF-8 byte offset で返す。外部 binding で文字列が短縮されていても現在のコードポイント境界へ安全に補正する。" },
        { sig: "bool TrySetSelection(usize selection_start, usize selection_end)", ret: "設定できたか", desc: "両端が現在の UTF-8 コードポイント境界で、先頭が末尾以下の範囲だけを選択する。成功時は cursor を選択末尾へ置く。範囲外・境界途中では false を返し、cursor、anchor、末尾追従を含む全状態を維持する。" },
        { sig: "void ClearSelection() / void SelectAll()", desc: "選択を現在の cursor へ折り畳む、または文字列全体を選択して cursor を末尾へ置く。" },
        { sig: "void Render(CUiRenderer&amp; r) noexcept", desc: "フォーカス中の選択範囲は <code>input_selection</code> 色で背景・枠の後、文字列・caret の前に描画する。矩形は入力欄の左右 6 px の内容領域と上下 3 px の範囲へ clamp し、隣接 UI へはみ出させない。" },
        { sig: "bool TrySetMaxTextBytes(usize) / usize MaxTextBytes() const", desc: "入力上限を設定/取得する。既定 4096 bytes、hard limit 1 MiB。設定変更だけでは既存文字列を切り詰めない。" },
        { sig: "using FTextInput = ATextInput", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ATextInput</code> を使う。" }
      ]
    },
    {
      name: "CUiRenderer",
      kind: "クラス", header: "ui/UiRenderer.h",
      summary: "<t>AWidget</t> ツリーを <code>CSpriteBatch</code> + <code>FFont</code> で<b>1 フレーム分まとめて描画</b>する描画役。矩形・枠線・文字・カーソル位置用の文字列計測ヘルパを持ち、各<t>ウィジェット</t>の <code>Render</code> がこれに描画を依頼する。コピー不可。",
      when: "UI を画面に出す中心。起動時に <code>Init</code> し、毎フレーム <code>Render(root, ...)</code> を呼ぶ。",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&amp; device, EFormat rt_format, FFont* default_font) noexcept", ret: "成否(<t>Result</t>)", desc: "内部の <code>CSpriteBatch</code> を初期化し、既定<t>フォント</t>と描画先フォーマットを設定する。", when: "アプリ起動時に 1 回。" },
        { sig: "void Shutdown()", desc: "確保した描画リソースを解放する。" },
        { sig: "void Render(AWidget&amp; root, IRhiCommandList&amp; cmd, u32 screen_w, u32 screen_h) noexcept", desc: "visible な root を画面全体へ Layout してから<t>ウィジェット</t>を再帰描画し、コマンドリストに発行する。<code>root.visible == false</code> なら Layout、batch Begin/End、root/child Render をすべて省略する。", when: "毎フレーム 1 回呼ぶ。" },
        { sig: "void DrawRect(f32 x, f32 y, f32 w, f32 h, const FVec4&amp; color)", desc: "塗りつぶし矩形を描く。<t>ウィジェット</t>の <code>Render</code> 内から使う低レベル部品。" },
        { sig: "void DrawRectOutline(f32 x, f32 y, f32 w, f32 h, const FVec4&amp; color, f32 thickness = 1.0f)", desc: "枠線(中抜き矩形)を描く。" },
        { sig: "void DrawText(const char* utf8, f32 x, f32 y, const FVec4&amp; color) noexcept", desc: "UTF-8 文字列を描く。" },
        { sig: "f32 MeasureText(const char* utf8) const / f32 MeasureTextBytes(const char* utf8, usize byte_count) const", desc: "文字列全体または指定 byte prefix の描画幅を測る。<code>ATextInput</code> の caret 位置計算にも使う。" },
        { sig: "const FUiColors&amp; Colors() / FUiColors&amp; Colors()", ret: "テーマ色", desc: "<t>ウィジェット</t>が参照するテーマ色一式。非 const 版で書き換えると見た目を一括変更できる。"},
        { sig: "FFont* DefaultFont() const noexcept", ret: "既定フォント", desc: "<code>Init</code> で渡した既定<t>フォント</t>。所有はしない。" },
        { sig: "using FUiRenderer = CUiRenderer", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CUiRenderer</code> を使う。" }
      ]
    },
    {
      name: "FUiKeyModifiers",
      kind: "構造体", header: "ui/Widget.h",
      summary: "UI キーイベントを配信する瞬間の<b>修飾キー押下状態</b>。左右キーをまとめた Shift / Ctrl / Alt / Super の4フラグを持つ。",
      when: "独自 widget が Shift や Ctrl を組み合わせたキー操作を実装する時。通常は <code>CUiInput</code> が構築して3引数版 <code>OnKey</code> へ渡す。",
      members: [
        { sig: "bool bShift / bControl / bAlt / bSuper", desc: "左右いずれかの対応する修飾キーが押下中なら true。既定はすべて false。" }
      ]
    },
    {
      name: "CUiInput",
      kind: "クラス", header: "ui/UiRenderer.h",
      summary: "<code>FWindow</code> のマウス/キーイベントを取り出して、<t>ヒットテスト</t>で当たった<t>ウィジェット</t>の <code>On*</code> メソッドへ<b>配信</b>する入力ディスパッチャ。hover / 押下 / フォーカスは非所有ポインタではなく比較専用 address token + 構築時に保存した module token + 非0 module generation の3要素 identity で追跡する。別 DLL が同じ widget address / generation を同時または逐次使っても、両 module が load 中なら module token で分離する。コピー/ムーブ不可。この identity とクラスレイアウトは永続 handle / 安定 plugin ABI ではなく、更新時は関連 DLL を同じ revision からフル rebuild する。",
      when: "UI に入力を効かせたい時。毎フレーム <code>Dispatch(root)</code> を呼ぶだけで、クリックやドラッグが各<t>ウィジェット</t>に届く。",
      members: [
        { sig: "void Dispatch(AWidget&amp; root) noexcept", desc: "現在のマウス位置・クリック・キー押下/解放を <t>Input</t> から取得し、ルートを<t>ヒットテスト</t>して該当<t>ウィジェット</t>へ届ける。編集キーには左右 Shift / Ctrl / Alt / Super の押下中状態を <code>FUiKeyModifiers</code> として付与し、Ctrl+A も選択コマンドとして配信する。保存した複合 identity は毎回現在の生存 subtree から解決するため、各 On* callback は current/other child を同期除去できる。呼び出し中は root 自身を破棄してはならない。", when: "毎フレーム 1 回呼ぶ。" },
        { sig: "void Reset() noexcept / void Reset(AWidget&amp; live_root) noexcept", desc: "保存中の root / hover / pressed / focus / Ctrl+A 状態を破棄する。引数なし版は widget を参照しないので root 破棄直前にも安全で、同じ live root を次に Dispatch すると新規採用時に subtree の一時入力フラグも初期化する。生存 root を渡す版は、その場で subtree の <code>hovered</code> / <code>focused</code> / <code>pressed</code> も再帰的に解除する。DLL unload/reload では module address まで再利用され得るため、host は module 所有 root が生存中なら Reset(root) の後に破棄・unloadし、既に破棄済みなら reload / 次の Dispatch より前に Reset() を必ず呼ぶ。", when: "scene 切り替え、UI ツリー再構築、module root destruction / DLL unload 境界。root 変更自体は通常 Dispatch でも自動検出するが、DLL reload 境界の Reset は必須。" },
        { sig: "using FUiInput = CUiInput", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CUiInput</code> を使う。" }
      ]
    },
    {
      name: "FUiRect",
      kind: "構造体", header: "ui/Widget.h",
      summary: "UI 座標の<b>矩形</b>。左上原点・ピクセル単位で <code>x, y, w, h</code> を持つ。点が中に入っているか判定できる。",
      when: "<t>ウィジェット</t>の領域(<code>rect</code> / <code>requested</code>)を表す基本型。当たり判定にも使う。",
      members: [
        { sig: "f32 x, y, w, h", desc: "左上座標と幅・高さ(ピクセル)。" },
        { sig: "bool Contains(f32 px, f32 py) const", ret: "内側か", desc: "点 (px,py) が矩形の内側なら true。" }
      ]
    },
    {
      name: "FUiPadding",
      kind: "構造体", header: "ui/Widget.h",
      summary: "<b>四辺の余白</b>(左 <code>l</code> / 上 <code>t</code> / 右 <code>r</code> / 下 <code>b</code>)を表す小さな構造体。",
      when: "<code>AStackPanel</code> の内側余白など、要素の周囲の空きを指定する時。"
    },
    {
      name: "EStackDir",
      kind: "列挙(enum)", header: "ui/Widget.h",
      summary: "<code>AStackPanel</code> が子を並べる<b>方向</b>。<code>Vertical</code>(縦) と <code>Horizontal</code>(横)。",
      when: "パネルを縦積みにするか横並びにするか決める時。"
    },
    {
      name: "FUiColors",
      kind: "構造体", header: "ui/Widgets.h",
      summary: "UI 全体の<b>テーマ色</b>をまとめた構造体。パネル背景・ボタン・スライダー・チェック・文字・入力欄などの色を <code>FVec4</code>(RGBA) で持つ。",
      when: "アプリの配色を一括で変えたい時。<code>CUiRenderer::Colors()</code> から取得して書き換える。",
      members: [
        { sig: "FVec4 panel_bg / panel_border", desc: "パネルの背景色・枠線色。" },
        { sig: "FVec4 button_bg / button_hover / button_press / button_text", desc: "ボタンの通常/ホバー/押下/文字色。" },
        { sig: "FVec4 slider_track / slider_fill / slider_knob", desc: "スライダーの溝/塗り/つまみの色。" },
        { sig: "FVec4 check_box / check_mark", desc: "チェックボックスの枠/チェック印の色。" },
        { sig: "FVec4 text / text_dim / input_bg / input_focus / input_selection", desc: "文字色(通常/淡色)、入力欄の背景/フォーカス色、フォーカス中の選択範囲色。" }
      ]
    },
    {
      name: "DefaultUiColors()",
      kind: "関数", header: "ui/Widgets.h",
      summary: "プログラム共通の<b>既定テーマ色</b>(<code>FUiColors</code>)への参照を返す。",
      when: "個別の <code>CUiRenderer</code> を作る前に、アプリ全体の既定色を参照/調整したい時。"
    },
    {
      name: "AAnchorPanel",
      kind: "クラス", header: "ui/Widget.h",
      summary: "子widgetをanchor基準で配置し、所有するpanel object。",
      when: "親領域の端や中央を基準にUIを配置する時。",
      members: [
        { sig: "using FAnchorPanel = AAnchorPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AAnchorPanel</code> を使う。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "ウィジェット": "ボタンやラベルなど画面上の UI 部品 1 つ 1 つのこと。<t>AWidget</t> を継承して作る。",
  "retained-mode": "毎フレーム UI を作り直すのではなく、部品のツリーを保持し続けて使う方式。対義は immediate-mode(ImGui 等)。",
  "レイアウト": "各<t>ウィジェット</t>を画面のどこにどの大きさで置くかを決めて配置すること。<code>Layout</code> で行う。",
  "ヒットテスト": "画面上のある点(マウス位置など)がどの<t>ウィジェット</t>の上にあるかを判定すること。",
  "UTF-8 cursor": "<code>ATextInput</code> が UTF-8 の byte offset とコードポイント境界を対応付けて保持する編集位置。"
});
