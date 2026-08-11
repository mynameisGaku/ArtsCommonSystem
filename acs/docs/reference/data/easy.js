/* ACS リファレンス — easy モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "easy",
  order: 4,
  title: "easy — かんたんモード",
  blurb: "クラスもテンプレートも使わず、<b>関数を呼ぶだけ</b>で 2D ゲームを書くための入口。ウィンドウ・図形・画像・文字・音・入力・乱数・当たり判定・カメラ・セーブが手続き的に扱えます。<code>using namespace acs::easy;</code> で全関数が使えます。",
  types: [
    {
      name: "（最小プログラム）",
      kind: "使い方",
      header: "easy/Easy.h",
      summary: "<code>OpenWindow</code> で開き、<code>while (NextFrame())</code> ループの中で入力を読んで図形を描く、が基本形。ゲームの状態は <code>main</code> の中のローカル変数でよく、すべての関数を<b>メインスレッド 1 本</b>から呼びます。座標は<b>左上が原点</b>・ピクセル単位・Y は下向き、角度は<b>度（°）</b>で正が時計回り。",
      when: "DXLib のような感覚で、設計を気にせずまず動くゲームを書き始めたい時。本格化したくなったら <code>acs::CApplication</code> へ進めます。"
    },

    {
      name: "FColor",
      kind: "構造体", header: "easy/Easy.h",
      summary: "色を表す値。各成分 <code>r,g,b,a</code> は <b>0.0〜1.0</b>。<code>a</code> は不透明度（0=透明, 1=不透明）。<code>FColor::Red</code> などの定数を使うか、<code>Rgb(255,0,0)</code> で作ります。",
      when: "すべての描画関数に色として渡す。半透明にしたい時は <code>a</code> を下げるか <code>Fade()</code> を使う。",
      members: [
        { sig: "f32 r, g, b, a", desc: "赤・緑・青・不透明度。すべて 0.0〜1.0。<code>a</code> の既定は 1（不透明）。" },
        { sig: "static const FColor Red, Green, Blue, Yellow, Cyan, Magenta", desc: "よく使う色の定数。" },
        { sig: "static const FColor White, Black, Gray, Orange, Sky, Clear", desc: "<code>Clear</code> は完全透明。<code>Sky</code> は空色。" }
      ]
    },
    {
      name: "Rgb / Rgba / Fade",
      kind: "関数", header: "easy/Easy.h",
      summary: "<b>0〜255 の整数</b>から <code>FColor</code> を作るヘルパと、不透明度だけ変える <code>Fade</code>。",
      when: "DXLib 等で慣れた 0〜255 表記で色を作りたい時。フェードイン/アウトや半透明描画に <code>Fade</code>。",
      members: [
        { sig: "FColor Rgb(u8 r, u8 g, u8 b)", ret: "不透明な色", desc: "0〜255 の RGB から色を作る。" },
        { sig: "FColor Rgba(u8 r, u8 g, u8 b, u8 a)", desc: "不透明度も 0〜255 で指定する版。" },
        { sig: "FColor Fade(FColor color, f32 alpha)", ret: "半透明にした色", desc: "色はそのままで不透明度（0〜1）だけ差し替える。" }
      ]
    },
    {
      name: "FSprite / FSound",
      kind: "構造体", header: "easy/Easy.h",
      summary: "<code>LoadSprite</code> / <code>LoadSound</code> が返す、コピー可能な軽い<b>素材ハンドル</b>（中身は番号 <code>id</code> だけ）。<code>id==0</code> は無効。",
      when: "読み込んだ画像・音を持ち回って描画/再生に渡す時。失敗しても落ちず、無効ハンドルが返る（描画/再生は無視される）。",
      members: [
        { sig: "u32 id", desc: "素材を指す内部番号。0 は無効（読み込み失敗など）。" }
      ]
    },

    {
      name: "OpenWindow / NextFrame / Quit",
      kind: "関数", header: "easy/Easy.h",
      summary: "ゲームの<b>開始・1 フレーム進行・終了</b>。<code>OpenWindow</code> を最初に 1 回だけ呼び、<code>while (NextFrame())</code> でループします。",
      when: "すべての easy プログラムの骨格。描画・入力の関数は必ず <code>NextFrame()</code> の後（ループの中）で呼ぶ。",
      members: [
        { sig: "void OpenWindow(i32 width=1280, i32 height=720, const char* title=\"ACS Game\")", desc: "ウィンドウを開いて初期化。失敗時はコンソールにエラーを出し、最初の <code>NextFrame()</code> が false を返す（静かに終了）。" },
        { sig: "bool NextFrame()", ret: "続行するか", desc: "直前のフレームを画面に出し、次の準備をして true。閉じられたら後始末して false。<code>while</code> の条件に使う。" },
        { sig: "void Quit()", desc: "ゲームを終了する。以後 <code>NextFrame()</code> が false を返すようになる。" }
      ]
    },
    {
      name: "SetWindowTitle / SetBackground",
      kind: "関数", header: "easy/Easy.h",
      summary: "ウィンドウのタイトル文字列と背景色を変える。背景色の既定は濃い紺色。",
      when: "スコア表示をタイトルに出す、ステージごとに背景色を変える、など。",
      members: [
        { sig: "void SetWindowTitle(const char* title)", desc: "タイトルバーの文字列を変える。" },
        { sig: "void SetBackground(FColor color)", desc: "毎フレームの塗りつぶし背景色（次フレームから反映）。" }
      ]
    },
    {
      name: "SetFullscreen / ToggleFullscreen / IsFullscreen",
      kind: "関数", header: "easy/Easy.h",
      summary: "全画面表示（ボーダーレス）の ON/OFF。次のフレームから反映されます。",
      when: "設定画面の全画面トグルや、Alt+Enter 相当の切替に。",
      members: [
        { sig: "void SetFullscreen(bool on)", desc: "全画面の ON/OFF を直接指定。" },
        { sig: "void ToggleFullscreen()", desc: "現在の状態を反転する。" },
        { sig: "bool IsFullscreen()", ret: "全画面か", desc: "今が全画面なら true。" }
      ]
    },

    {
      name: "図形を描く（DrawRect ほか）",
      kind: "関数", header: "easy/Easy.h",
      summary: "四角・円・線・三角・点を描く関数群。<b>(x,y) は図形の左上</b>（円だけは中心）、回転は図形の中心まわりで角度は度。<code>Outline</code> 版は枠線だけ、<code>thickness</code> で太さを指定。",
      when: "画像を使わずに当たり判定の可視化やシンプルなゲームを作る時。<code>NextFrame()</code> の後で呼ぶ。",
      members: [
        { sig: "void DrawRect(f32 x, f32 y, f32 w, f32 h, FColor color)", desc: "塗りつぶし長方形。(x,y) は左上。" },
        { sig: "void DrawRectOutline(f32 x, f32 y, f32 w, f32 h, FColor color, f32 thickness=2)", desc: "長方形の枠線だけ。" },
        { sig: "void DrawRectRotated(f32 x, f32 y, f32 w, f32 h, f32 degrees, FColor color)", desc: "回転した塗り長方形。(x,y) は回転前の左上、回転は中心まわり、角度は度。" },
        { sig: "void DrawCircle(f32 x, f32 y, f32 radius, FColor color)", desc: "塗りつぶし円。<b>(x,y) は中心</b>。", when: "弾やボールなど。" },
        { sig: "void DrawCircleOutline(f32 x, f32 y, f32 radius, FColor color, f32 thickness=2)", desc: "円の枠線だけ。(x,y) は中心。" },
        { sig: "void DrawLine(f32 x1, f32 y1, f32 x2, f32 y2, FColor color, f32 thickness=2)", desc: "2 点を結ぶ線。" },
        { sig: "void DrawTriangle(f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3, FColor color)", desc: "3 頂点の塗り三角形。" },
        { sig: "void DrawTriangleOutline(f32 x1,f32 y1,f32 x2,f32 y2,f32 x3,f32 y3, FColor color, f32 thickness=2)", desc: "三角形の枠線だけ。" },
        { sig: "void DrawPixel(f32 x, f32 y, FColor color)", desc: "1 ピクセルの点を打つ。" }
      ]
    },

    {
      name: "画像を描く（DrawSprite ほか）",
      kind: "関数", header: "easy/Easy.h",
      summary: "<code>LoadSprite</code> で読んだ画像を描く関数群。元サイズ・伸縮・回転・色掛け（<t>ティント</t>）・反転・<b>一部切り出し</b>（スプライトシート）に対応。位置 (x,y) は画像の左上。",
      when: "キャラやタイルの描画。アニメーションは <code>DrawSpritePart</code> でシートのコマを切り替える。",
      members: [
        { sig: "void DrawSprite(FSprite s, f32 x, f32 y)", desc: "画像を元サイズで描く。" },
        { sig: "void DrawSprite(FSprite s, f32 x, f32 y, f32 w, f32 h)", desc: "指定サイズに伸縮して描く。" },
        { sig: "void DrawSpriteRotated(FSprite s, f32 x, f32 y, f32 degrees, f32 scale=1, FColor tint={1,1,1,1})", desc: "回転（＋拡縮・色掛け）。(x,y) は回転前の左上、回転は中心まわり。" },
        { sig: "void DrawSpriteTinted(FSprite s, f32 x, f32 y, FColor tint)", desc: "<t>ティント</t>（色掛け）して描く。点滅やダメージ表現に。" },
        { sig: "void DrawSpriteFlipped(FSprite s, f32 x, f32 y, bool flip_x, bool flip_y)", desc: "左右/上下を反転して描く。向きの切替に。" },
        { sig: "void DrawSpritePart(FSprite s, f32 x,f32 y,f32 w,f32 h, f32 sx,f32 sy,f32 sw,f32 sh)", desc: "画像内の矩形 (sx,sy,sw,sh) だけを切り出して描く。スプライトシートのコマ送りに。" },
        { sig: "f32 SpriteWidth(FSprite s) / f32 SpriteHeight(FSprite s)", ret: "画像の幅/高さ", desc: "読み込んだ画像のピクセルサイズ。" }
      ]
    },

    {
      name: "文字を描く（DrawString ほか）",
      kind: "関数", header: "easy/Easy.h",
      summary: "<b>UTF-8 の文字列</b>（日本語可、<code>\\n</code> で改行）を描く。中央そろえ版や、文字の幅・高さを測る関数もある。<code>size</code> で文字サイズを指定。",
      when: "スコア・メッセージ・メニューの表示。中央そろえはタイトルやボタンに便利。",
      members: [
        { sig: "void DrawString(f32 x, f32 y, const char* text, FColor color)", desc: "(x,y) を左上に文字を描く。" },
        { sig: "void DrawString(f32 x, f32 y, const char* text, FColor color, f32 size)", desc: "文字サイズ指定版。" },
        { sig: "void DrawStringCentered(f32 center_x, f32 y, const char* text, FColor color)", desc: "<code>center_x</code> を横の中心にそろえて描く。" },
        { sig: "void DrawStringCentered(f32 center_x, f32 y, const char* text, FColor color, f32 size)", desc: "中央そろえ＋サイズ指定版。" },
        { sig: "f32 TextWidth(const char* text) / f32 TextWidth(const char* text, f32 size)", ret: "横幅(px)", desc: "描いたときの横幅。配置計算に。" },
        { sig: "f32 TextHeight() / f32 TextHeight(f32 size)", ret: "高さ(px)", desc: "1 行の高さ。行送りの計算に。" }
      ]
    },

    {
      name: "カメラ（SetCamera ほか）",
      kind: "関数", header: "easy/Easy.h",
      summary: "<b>ワールド座標</b>と<b>画面</b>を分ける仕組み。<code>SetCamera</code> 後に描く図形はワールド座標として扱われ、カメラの位置・ズームに応じて画面に映る。カメラ設定は<b>毎フレーム呼ぶ</b>（呼ばなければカメラ無し）。",
      when: "プレイヤー追従のスクロールや、見下ろし/横スクロールのマップ表示。HUD を描く前に <code>ResetCamera()</code> で固定表示に戻す。",
      members: [
        { sig: "void SetCamera(f32 x, f32 y)", desc: "(x,y) のワールド座標が画面中央に映るようカメラを置く。" },
        { sig: "void SetCameraZoom(f32 zoom)", desc: "ズーム倍率（1.0 等倍、2.0 で 2 倍に拡大）。" },
        { sig: "void ResetCamera()", desc: "スクロール無し・ズーム 1.0 に戻す。HUD を描く前に。" },
        { sig: "f32 CameraX() / f32 CameraY()", ret: "カメラ中心", desc: "現在のカメラ中心のワールド座標。" },
        { sig: "f32 MouseWorldX() / f32 MouseWorldY()", ret: "マウスのワールド座標", desc: "スクロール・ズームを反映したマウス位置。クリックでワールド上のものを掴む時に。" }
      ]
    },
    {
      name: "SetClipRect / ClearClipRect",
      kind: "関数", header: "easy/Easy.h",
      summary: "描画をある矩形の中だけに制限する<b>クリップ</b>。座標は<b>画面座標</b>（カメラの影響を受けない）。",
      when: "ミニマップ枠やメニューパネルの中だけに描きたい時。終わったら必ず解除する。",
      members: [
        { sig: "void SetClipRect(f32 x, f32 y, f32 w, f32 h)", desc: "この矩形の外には描かれなくなる。" },
        { sig: "void ClearClipRect()", desc: "クリップを解除して画面全体に描けるようにする。" }
      ]
    },

    {
      name: "ポストプロセス（SetBloom ほか）",
      kind: "関数", header: "easy/Easy.h",
      summary: "<t>Bloom</t>（発光）・ビネット・カラーグレーディング等、<b>画面全体</b>にかかる効果。<b>Diligent バックエンドでビルドした時だけ有効</b>（DX12 raw では無視）。<code>IsPostProcessAvailable()</code> で可否を確認。既定はすべて無効。",
      when: "ネオン調の発光、シネマティックな色味、画面端の暗化などで見た目を底上げしたい時。文字も含め画面全体にかかる点に注意。",
      members: [
        { sig: "bool IsPostProcessAvailable()", ret: "使えるか", desc: "Diligent バックエンドなら true。false なら以下は無視される。" },
        { sig: "void SetBloom(f32 intensity)", desc: "発光の強さ（0=無効、0.6 が目安）。" },
        { sig: "void SetBloomThreshold(f32 threshold)", desc: "発光させる明るさの閾値。" },
        { sig: "void SetExposure(f32 exposure)", desc: "露出（明るさ。1.0=標準）。" },
        { sig: "void SetVignette(f32 intensity)", desc: "画面端の暗化（0〜1、0=無効）。" },
        { sig: "void SetChromaticAberration(f32 amount)", desc: "色収差（0=無効、0.002 が目安）。" },
        { sig: "void SetFilmGrain(f32 intensity)", desc: "フィルムノイズ（0=無効）。" },
        { sig: "void SetColorGrading(f32 saturation, f32 contrast, f32 temperature)", desc: "彩度・コントラスト（各 1=標準）・色温度（-1=青寄り〜+1=橙寄り）。" },
        { sig: "void SetSharpness(f32 strength)", desc: "輪郭強調（0〜1、0=無効）。" },
        { sig: "void SetTonemap(i32 mode)", desc: "トーンマップ方式。0=ACES、1=AgX、2=Reinhard。" },
        { sig: "void SetAutoExposure(bool enabled)", desc: "自動露出（明るさを自動調整）。" }
      ]
    },

    {
      name: "LoadSprite / LoadSound",
      kind: "関数", header: "easy/Easy.h",
      summary: "画像・音を読み込んで<b>ハンドル</b>を返す。同じパスは読み直さず同じハンドルを返す。失敗しても落ちず、無効ハンドル（<code>id==0</code>）を返す。",
      when: "ゲーム開始時にまとめて読む（ループ内で毎回呼んでも再読込はされないが、開始時が分かりやすい）。<code>OpenWindow</code> の後で呼ぶ。",
      members: [
        { sig: "FSprite LoadSprite(const char* path)", ret: "画像ハンドル", desc: "画像ファイルを読む。失敗で <code>id==0</code>。" },
        { sig: "FSound LoadSound(const char* path)", ret: "音ハンドル", desc: "音ファイルを読む。失敗で <code>id==0</code>。" }
      ]
    },

    {
      name: "音（Play ほか）",
      kind: "関数", header: "easy/Easy.h",
      summary: "効果音の 1 回再生・<t>BGM</t> のループ再生・停止・音量制御。音量はすべて 0.0〜1.0。",
      when: "ジャンプ音や爆発音は <code>Play</code>、BGM は <code>PlayLoop</code>。設定画面の音量スライダーには <code>SetMasterVolume</code>。",
      members: [
        { sig: "void Play(FSound s) / void Play(FSound s, f32 volume)", desc: "効果音を 1 回鳴らす。音量は 0.0〜1.0。" },
        { sig: "void PlayLoop(FSound s) / void PlayLoop(FSound s, f32 volume)", desc: "ループ再生（BGM 向け）。" },
        { sig: "void StopSound(FSound s)", desc: "その音のループを止める。" },
        { sig: "void StopAllSounds()", desc: "鳴っている音を全部止める。" },
        { sig: "void SetMasterVolume(f32 volume)", desc: "全体音量（0.0〜1.0）。音量スライダー用。" },
        { sig: "void PauseAllSounds() / void ResumeAllSounds()", desc: "全音を一時停止／再開（位置は保たれる）。ポーズ画面に。" }
      ]
    },

    {
      name: "キーボード入力（IsKeyDown ほか）",
      kind: "関数", header: "easy/Easy.h",
      summary: "キーの状態を読む。<b>Down</b>=押されている間ずっと、<b>Pressed</b>=押した瞬間だけ、<b>Released</b>=離した瞬間だけ true。<code>TextInput()</code> は<t>IME</t>確定後の入力文字（名前入力用）。キーの種類は <code>EKey</code> 列挙。",
      when: "移動は <code>IsKeyDown</code>（押しっぱなしで動く）、ジャンプや決定は <code>IsKeyPressed</code>（1 回だけ反応）。名前入力には <code>TextInput</code>。",
      members: [
        { sig: "bool IsKeyDown(EKey key)", ret: "押下中か", desc: "押されている間ずっと true。移動などに。" },
        { sig: "bool IsKeyPressed(EKey key)", ret: "押した瞬間か", desc: "押した瞬間のフレームだけ true。1 回限りの操作に。" },
        { sig: "bool IsKeyReleased(EKey key)", ret: "離した瞬間か", desc: "離した瞬間のフレームだけ true。" },
        { sig: "const char* TextInput()", ret: "入力文字 or \"\"", desc: "このフレームに入力された文字（UTF-8、IME 確定後）。無ければ空文字列。", when: "プレイヤー名やチャットの入力欄に。" }
      ]
    },

    {
      name: "マウス入力（MouseX ほか）",
      kind: "関数", header: "easy/Easy.h",
      summary: "マウス座標（ウィンドウ内ピクセル＝画面座標）・ボタン状態・ホイール。ボタンは <code>EMouseButton</code>（省略時は左）。Down/Pressed/Released の違いはキーボードと同じ。",
      when: "クリックで撃つ・ボタンを押す、ドラッグ、ホイールでズーム/スクロール。ワールド上の対象を掴むなら <code>MouseWorldX/Y</code>（カメラ節）。",
      members: [
        { sig: "f32 MouseX() / f32 MouseY()", ret: "マウス座標(px)", desc: "ウィンドウ内のマウス位置（画面座標）。" },
        { sig: "bool IsMouseDown(EMouseButton button=Left)", ret: "押下中か", desc: "ボタンが押されている間ずっと true。" },
        { sig: "bool IsMousePressed(EMouseButton button=Left)", desc: "押した瞬間だけ true。" },
        { sig: "bool IsMouseReleased(EMouseButton button=Left)", desc: "離した瞬間だけ true。" },
        { sig: "f32 MouseWheel()", ret: "回転量", desc: "ホイールの回転（奥が正、手前が負）。" }
      ]
    },

    {
      name: "ゲームパッド入力（IsGamepadDown ほか）",
      kind: "関数", header: "easy/Easy.h",
      summary: "最大 4 台（<code>player</code> 0〜3、省略時 0）のゲームパッドを読む。ボタンは <code>EGamepadButton</code>、スティックは -1.0〜+1.0、トリガーは 0.0〜1.0。",
      when: "コントローラ対応。スティックで移動、トリガーでアクセル、など。接続確認は <code>IsGamepadConnected</code>。",
      members: [
        { sig: "bool IsGamepadConnected(i32 player=0)", ret: "接続中か", desc: "そのパッドが繋がっていれば true。" },
        { sig: "bool IsGamepadDown(EGamepadButton button, i32 player=0)", desc: "ボタンが押下中なら true。" },
        { sig: "bool IsGamepadPressed(EGamepadButton button, i32 player=0)", desc: "押した瞬間だけ true。" },
        { sig: "f32 GamepadLeftX/LeftY/RightX/RightY(i32 player=0)", ret: "-1.0〜+1.0", desc: "左右スティックの傾き。" },
        { sig: "f32 GamepadLeftTrigger/RightTrigger(i32 player=0)", ret: "0.0〜1.0", desc: "アナログトリガーの踏み込み量。" }
      ]
    },

    {
      name: "乱数（RandomInt ほか）",
      kind: "関数", header: "easy/Easy.h",
      summary: "整数・小数・真偽値の乱数。<code>RandomSeed</code> で種を固定すると、毎回<b>同じ並び</b>になる（再現性が必要なテストやリプレイに）。",
      when: "敵の出現位置、ドロップ判定、ランダム挙動。デバッグで毎回同じ展開にしたい時は種を固定。",
      members: [
        { sig: "i32 RandomInt(i32 min, i32 max)", ret: "整数", desc: "min 以上 max <b>以下</b>の整数。" },
        { sig: "f32 RandomFloat(f32 min, f32 max)", ret: "小数", desc: "min 以上 max <b>未満</b>の小数。" },
        { sig: "bool RandomBool()", ret: "true/false", desc: "半々で true か false。" },
        { sig: "void RandomSeed(u32 seed)", desc: "乱数の種を固定する。同じ種なら同じ並びになる。" }
      ]
    },

    {
      name: "当たり判定（RectsOverlap ほか）",
      kind: "関数", header: "easy/Easy.h",
      summary: "矩形どうし・円どうしの<b>重なり</b>、点が矩形/円の中にあるかを判定する。重なっていれば true。",
      when: "敵と弾の衝突、プレイヤーとアイテムの接触、ボタンのクリック判定（点 vs 矩形）。",
      members: [
        { sig: "bool RectsOverlap(f32 x1,f32 y1,f32 w1,f32 h1, f32 x2,f32 y2,f32 w2,f32 h2)", ret: "重なるか", desc: "2 つの矩形が重なっていれば true。(x,y) は各左上。" },
        { sig: "bool CirclesOverlap(f32 x1,f32 y1,f32 r1, f32 x2,f32 y2,f32 r2)", ret: "重なるか", desc: "2 つの円が重なっていれば true。(x,y) は各中心。" },
        { sig: "bool PointInRect(f32 px,f32 py, f32 x,f32 y,f32 w,f32 h)", ret: "中にあるか", desc: "点が矩形の中にあれば true。クリック判定に。" },
        { sig: "bool PointInCircle(f32 px,f32 py, f32 cx,f32 cy,f32 r)", ret: "中にあるか", desc: "点が円の中にあれば true。" }
      ]
    },

    {
      name: "数学ヘルパ（Clamp / Lerp / Sin ほか）",
      kind: "関数", header: "easy/Easy.h",
      summary: "範囲制限・線形補間・距離・最小最大・絶対値・平方根・三角関数・向きの角度。<b>三角関数の角度は度（°）で渡す</b>（C++ 標準の sin/cos はラジアンなので注意）。",
      when: "値を範囲に収める（HP バー等）、なめらかな移動（<t>Lerp</t>）、円運動（<code>Sin/Cos</code>）、狙い撃ち（<code>AngleTo</code>）。",
      members: [
        { sig: "f32 Clamp(f32 value, f32 lo, f32 hi)", ret: "範囲内の値", desc: "value を lo〜hi に収める。" },
        { sig: "f32 Lerp(f32 a, f32 b, f32 t)", ret: "補間値", desc: "a と b を t（0〜1）で<t>線形補間</t>する。" },
        { sig: "f32 Distance(f32 x1,f32 y1,f32 x2,f32 y2)", ret: "距離", desc: "2 点間の直線距離。" },
        { sig: "f32 Min(f32 a, f32 b) / f32 Max(f32 a, f32 b)", desc: "小さい方／大きい方。" },
        { sig: "f32 Abs(f32 value)", ret: "絶対値", desc: "符号を取り除いた値。" },
        { sig: "f32 Sin(f32 degrees) / f32 Cos(f32 degrees)", ret: "正弦/余弦", desc: "<b>角度は度</b>で渡す。円運動や揺れに。" },
        { sig: "f32 Sqrt(f32 value)", ret: "平方根", desc: "値の平方根。" },
        { sig: "f32 AngleTo(f32 x1,f32 y1,f32 x2,f32 y2)", ret: "角度(度)", desc: "(x1,y1) から (x2,y2) へ向かう向きの角度。<code>DrawSpriteRotated</code> にそのまま渡せる。", when: "敵をプレイヤーへ向ける・弾を狙う方向に飛ばす時。" }
      ]
    },

    {
      name: "セーブ／ロード（SaveInt ほか）",
      kind: "関数", header: "easy/Easy.h",
      summary: "実行ファイルの場所の <code>save.dat</code> に <b>key-value</b> で永続化する。整数・小数・文字列を保存／読み出し。<code>Load</code> 系は未保存ならデフォルト値を返す。<code>OpenWindow</code> の前でも後でも呼べる。",
      when: "ハイスコア・設定・進行状況の保存。文字列の値に改行（<code>\\n</code>）は使えない。",
      members: [
        { sig: "void SaveInt(const char* key, i32 value)", desc: "整数を保存する。" },
        { sig: "i32 LoadInt(const char* key, i32 default_value)", ret: "値 or 既定", desc: "整数を読む。未保存なら default_value。" },
        { sig: "void SaveFloat(const char* key, f32 value)", desc: "小数を保存する。" },
        { sig: "f32 LoadFloat(const char* key, f32 default_value)", ret: "値 or 既定", desc: "小数を読む。未保存なら default_value。" },
        { sig: "void SaveString(const char* key, const char* value)", desc: "文字列を保存する（改行は不可）。" },
        { sig: "const char* LoadString(const char* key, const char* default_value)", ret: "値 or 既定", desc: "文字列を読む。戻り値は次に Save 系を呼ぶまで有効。" },
        { sig: "bool HasSaveKey(const char* key)", ret: "保存済みか", desc: "そのキーが保存されていれば true。" },
        { sig: "void DeleteAllSaves()", desc: "保存内容を全消去する。" }
      ]
    },

    {
      name: "情報（DeltaTime / Fps ほか）",
      kind: "関数", header: "easy/Easy.h",
      summary: "フレーム間隔・累積時間・FPS・画面サイズを得る。<code>DeltaTime</code> を移動量に掛けると、フレームレートに依らず一定速度で動かせる。",
      when: "フレームレート非依存の移動（<code>x += speed * DeltaTime()</code>）、タイマー、画面端の判定、FPS 表示。",
      members: [
        { sig: "f32 DeltaTime()", ret: "経過秒", desc: "前フレームからの経過秒。移動量に掛けて速度を一定にする。" },
        { sig: "f32 ElapsedTime()", ret: "累積秒", desc: "<code>OpenWindow</code> からの累積秒。アニメや揺れの時間入力に。" },
        { sig: "i32 Fps()", ret: "フレーム毎秒", desc: "おおよその 1 秒あたりフレーム数。" },
        { sig: "f32 ScreenWidth() / f32 ScreenHeight()", ret: "画面サイズ(px)", desc: "描画領域の幅と高さ。配置や端判定に。" }
      ]
    },

    {
      name: "Ease / TryEase / EEasingType",
      kind: "関数・列挙", header: "easy/Easy.h",
      summary: "GameFramework と同じ<t>イージング</t> catalog の評価可能な全 33 曲線を easy から型付きで利用する入口。<code>EEasingType</code> は Linear、Quad/Cubic/Quart/Quint/Sine/Expo/Circ/Back/Elastic/Bounce の In/Out/InOut と SmoothStep/SmootherStep を列挙する。<code>Count = 33</code> は末尾 sentinel で、評価可能な曲線ではない。Easy は GameFramework へ一方向に依存し、曲線の数式を重複実装しない。",
      when: "移動・UI・フェードの進行度を等速以外にしたい時。保存した canonical 名から曲線を復元したい時や、プレビューを一括生成したい時、非有限入力・無効 enum を診断したい時は checked API を使う。",
      members: [
        { sig: "using EEasingType / EEasingError / FEasingResult", desc: "GameFramework の型付き Easing catalog・エラー・checked 結果を easy 名前空間へ再公開する alias。" },
        { sig: "f32 Ease(f32 t, EEasingType type, f32 fallback = 0)", desc: "全 33 種を型付き評価。無効 type または非有限 t では fallback を返し、finite t は [0,1] に clamp。" },
        { sig: "FEasingResult TryEase(f32 t, EEasingType type, f32& out)", desc: "checked 評価。無効 type、非有限入力・結果を拒否し、失敗時は out を変更しない。" },
        { sig: "FEasingResult TrySampleEasing(EEasingType type, f32* out, usize sample_count)", desc: "GameFramework の上限付き一括サンプリングへ転送する。2〜65536点、確保なし。失敗時は配列全体を変更しない。" },
        { sig: "const char* EasingName(EEasingType type)", desc: "canonical 名を返す。無効 type では <code>\"Invalid\"</code>。" },
        { sig: "FEasingResult TryGetEasingName(EEasingType type, const char*& out)", desc: "canonical 名の checked 取得。無効 type は <code>InvalidType</code> で拒否し、失敗時は out を変更しない。" },
        { sig: "bool TryParseEasingName(const char* name, EEasingType& out)", desc: "canonical 名を parse。null・空・未知名では false で out を変更しない。" },
        { sig: "FEasingResult TryParseEasingNameChecked(const char* name, EEasingType& out)", desc: "canonical 名を大文字小文字を区別して解析する checked API。null は <code>NullName</code>、空・未知名は <code>UnknownName</code> で拒否し、失敗時は out を変更しない。" },
        { sig: "f32 EaseIn/EaseOut/EaseInOut(f32 t)", desc: "InQuad / OutQuad / InOutQuad へ委譲する既存互換 shortcut。" },
        { sig: "f32 EaseOutBack/EaseOutBounce/EaseOutElastic(f32 t)", desc: "OutBack / OutBounce / OutElastic へ委譲する既存互換 shortcut。" }
      ]
    },

    {
      name: "FJobBatch / FJobNode",
      kind: "構造体", header: "easy/Easy.h",
      summary: "並列処理のハンドル。<code>FJobBatch</code> は <code>RunAsync</code> で投げた非同期ジョブ群を、<code>FJobNode</code> は依存グラフの 1 ノードを指す軽量値型（<code>id==0</code> は無効）。",
      when: "<code>RunAsync</code> の戻りを <code>WaitJobs</code> に渡す、<code>Job</code> の戻りを <code>Then</code> でつなぐ、といった並列 API の受け渡しに使う。",
      members: [
        { sig: "u32 id", desc: "ジョブ群／ノードを指す内部番号。0 は無効。" }
      ]
    },
    {
      name: "ParallelFor",
      kind: "関数テンプレート", header: "easy/Easy.h",
      summary: "<code>[begin,end)</code> を全コアで分担して <code>fn(i)</code> を呼び、<b>完了まで待つ</b>（同期）。内部は本格的なワークスチール<t>スレッドプール</t>。並列にできない環境では自動的に順次実行へフォールバック。",
      when: "大量の独立した計算（パーティクル更新、画像処理など）を一気に速くしたい時。<b>各 i は独立して</b>処理し、描画など他の easy 関数を中で呼ばないこと（<t>スレッド</t>はメイン専用）。",
      members: [
        { sig: "void ParallelFor<Fn>(i32 begin, i32 end, Fn fn)", desc: "区間を自動チャンクで分担して並列実行し、全完了まで待つ。" },
        { sig: "void ParallelFor<Fn>(i32 begin, i32 end, i32 grain, Fn fn)", desc: "<code>grain</code>（1 チャンクの要素数）を明示する版。0 で自動。" }
      ]
    },
    {
      name: "RunAsync / WaitJobs / JobsDone",
      kind: "関数", header: "easy/Easy.h",
      summary: "1 つの処理を<b>非同期に</b>走らせて即座に戻り、<code>FJobBatch</code> を返す。後で <code>WaitJobs</code> で完了を待つか <code>JobsDone</code> で待たずに確認する。同じ batch に追加投入もできる。",
      when: "重い読み込みや計算を裏で進めつつ、ローディング画面を回したい時。中では計算だけして結果は捕捉変数に書き、描画はしないこと。",
      members: [
        { sig: "FJobBatch RunAsync<Fn>(Fn fn)", ret: "ジョブハンドル", desc: "fn を非同期で 1 つ走らせ、すぐ戻る。" },
        { sig: "void RunAsync<Fn>(FJobBatch batch, Fn fn)", desc: "既存 batch に追加投入する（同じ <code>WaitJobs</code> でまとめて待てる）。" },
        { sig: "void WaitJobs(FJobBatch batch)", desc: "batch の全ジョブ完了まで待つ。待ち終えた batch は無効化される。" },
        { sig: "bool JobsDone(FJobBatch batch)", ret: "完了したか", desc: "待たずに完了状態を確認する。ローディング表示の継続判定に。" }
      ]
    },
    {
      name: "Job / Then / RunJobs",
      kind: "関数", header: "easy/Easy.h",
      summary: "<b>依存つき</b>の並列実行。<code>Job</code> でノードを作り、<code>Then</code> で「A の後に B」と順序を張り、<code>RunJobs</code> で全ノードを依存順に実行して全完了まで待つ。内部は依存グラフ。",
      when: "処理に前後関係がある時（『地形生成 → ナビ構築 → 敵配置』など）。順序を <code>Then</code> で表現すれば、独立な部分は自動で並列化される。",
      members: [
        { sig: "FJobNode Job<Fn>(Fn fn)", ret: "ノードハンドル", desc: "実行する処理を 1 ノードとして登録する（まだ走らない）。" },
        { sig: "void Then(FJobNode before, FJobNode after)", desc: "before が終わってから after が走る、という依存を張る。" },
        { sig: "void RunJobs()", desc: "作った全ノードを依存順に実行し、全完了まで待つ。" }
      ]
    },
    {
      name: "WorkerCount / IsWorker",
      kind: "関数", header: "easy/Easy.h",
      summary: "並列ワーカ数と、今のコードがワーカ<t>スレッド</t>上で動いているかを返す。",
      when: "分割数の目安にしたり、ジョブの中（ワーカ上）で誤って描画関数を呼んでいないか確認したりする時。",
      members: [
        { sig: "i32 WorkerCount()", ret: "ワーカ数", desc: "並列ワーカの数（1 以上）。" },
        { sig: "bool IsWorker()", ret: "ワーカ上か", desc: "今このコードがワーカスレッド上で動いていれば true。" }
      ]
    },
    {
      name: "入力コード（EKey / EMouseButton / EGamepadButton）",
      kind: "列挙", header: "platform/InputCodes.h",
      summary: "easy が再公開している入力用の列挙。<code>using acs::EKey;</code> 等で <code>acs::easy</code> 名前空間からそのまま <code>EKey::Space</code> のように書ける。実体は <t>platform</t> モジュールの <code>InputCodes.h</code> で定義され、easy 側では値一覧が見えないため、よく使う値をここにまとめる。<code>IsKeyDown</code> / <code>IsMouseDown</code> / <code>IsGamepadDown</code> などに渡す。",
      when: "キーボード・マウス・ゲームパッドの入力関数に「どのキー/ボタンか」を渡す時。先頭の <code>EKey::</code> 等を省略せず、列挙の値名で指定する。",
      members: [
        { sig: "EKey::A 〜 EKey::Z", desc: "文字キー(<code>A</code>〜<code>Z</code>、26 個)。" },
        { sig: "EKey::Num0 〜 EKey::Num9", desc: "最上段の数字キー(テンキーではない)。テンキーは <code>EKey::KP0</code>〜<code>EKey::KP9</code>。" },
        { sig: "EKey::F1 〜 EKey::F12", desc: "ファンクションキー。全画面トグルに <code>EKey::F11</code> など。" },
        { sig: "EKey::Up / Down / Left / Right", desc: "矢印キー。移動入力の定番。" },
        { sig: "EKey::Space / Enter / Tab / Backspace / Escape", desc: "主要な制御キー。決定・ジャンプ・キャンセル・文字消去などに。" },
        { sig: "EKey::Insert / Delete / Home / End / PageUp / PageDown", desc: "編集・ページ移動系のキー。" },
        { sig: "EKey::LeftShift / RightShift / LeftCtrl / RightCtrl / LeftAlt / RightAlt / LeftSuper / RightSuper", desc: "修飾キー(左右別)。<code>Super</code> は Windows キー / Cmd キー。" },
        { sig: "EKey::CapsLock / NumLock / ScrollLock", desc: "ロック系キー。" },
        { sig: "EKey::Minus / Equal / LeftBracket / RightBracket / Backslash / Semicolon / Apostrophe / Comma / Period / Slash / Grave", desc: "記号キー各種。" },
        { sig: "EKey::KP0 〜 KP9 / KPAdd / KPSubtract / KPMultiply / KPDivide / KPEnter / KPDecimal", desc: "テンキー(KeyPad)の数字と演算キー。" },
        { sig: "EKey::Unknown", desc: "未知/未対応キー(値 0)。" },
        { sig: "EMouseButton::Left = 0", desc: "左ボタン。マウス系関数の既定値。" },
        { sig: "EMouseButton::Right = 1", desc: "右ボタン。" },
        { sig: "EMouseButton::Middle = 2", desc: "中ボタン(ホイール押し込み)。" },
        { sig: "EMouseButton::X1 = 3 / X2 = 4", desc: "サイドボタン 1 / 2。" },
        { sig: "EGamepadButton::A / B / X / Y", desc: "フェイスボタン(XInput 互換配置)。" },
        { sig: "EGamepadButton::Up / Down / Left / Right", desc: "方向パッド(D-Pad)。" },
        { sig: "EGamepadButton::LeftBumper / RightBumper", desc: "左右のショルダー(バンパー)ボタン。" },
        { sig: "EGamepadButton::LeftStick / RightStick", desc: "スティックの押し込み。" },
        { sig: "EGamepadButton::Start / Back / Guide", desc: "スタート / バック / ガイド(ホーム)ボタン。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "ティント": "画像に色を掛け合わせて見た目の色を変えること。点滅やダメージ表現に使う。",
  "BGM": "バックグラウンドミュージック。場面の間ずっとループで流す音楽。",
  "IME": "日本語などを入力するための変換ソフト。確定後の文字が <code>TextInput()</code> で取れる。",
  "Bloom": "明るい部分をにじませて光って見せる<t>ポストプロセス</t>効果。",
  "ポストプロセス": "描き終えた画面全体に後からかける効果（発光・色補正・ノイズなど）。",
  "スレッドプール": "あらかじめ用意したワーカ<t>スレッド</t>に仕事を割り振って並列実行する仕組み。",
  "線形補間": "2 つの値の間を比率 t（0〜1）でまっすぐ補間すること（Lerp）。なめらかな移動に使う。",
  "platform": "OS ごとのウィンドウ・入力・時間などを抽象化する低レベルモジュール。easy の入力コード列挙 <code>EKey</code> 等もここで定義される。"
});
