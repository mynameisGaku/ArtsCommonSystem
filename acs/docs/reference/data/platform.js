/* ACS リファレンス — platform モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "platform",
  order: 26,
  title: "platform — ウィンドウ / 入力 / 時刻 / OS",
  blurb: "ゲームの<b>器</b>になる部分。<t>ウィンドウ</t>を開き、キーボード・マウス・<t>ゲームパッド</t>の<t>入力</t>を読み取り、<t>フレーム</t>の経過時間を計り、ファイルや設定を保存します。OS（Windows）との橋渡し役です。",
  types: [
    {
      name: "FWindow",
      kind: "クラス", header: "platform/Window.h",
      summary: "OS の<t>ウィンドウ</t>そのもの。タイトルバー付きの四角い領域を開き、× ボタンや入力などの<t>イベント</t>を受け取る。ゲームの一番外側のループはこれを中心に回す。",
      when: "ゲームを起動して最初に作るもの。1 つ作って、閉じる要求が来るまでメインループを回し続ける。",
      sample: "FWindowConfig cfg;\ncfg.title = L&quot;MyGame&quot;; cfg.width = 1280; cfg.height = 720;\nauto wr = FWindow::Create(cfg);\nif (wr.IsErr()) return -1;\nFWindow&amp; w = wr.Value();\nwhile (!w.ShouldClose()) {\n    w.PollEvents();   // OS メッセージを処理\n    // ...ゲーム処理...\n}",
      members: [
        { sig: "static TResult&lt;FWindow&gt; Create(const FWindowConfig&amp; cfg)", ret: "ウィンドウ or エラー", desc: "設定どおりにウィンドウを生成する。失敗すると <code>Err</code> を返す（<t>Result</t> なので必ず成否を確認する）。", when: "起動時に 1 回。" },
        { sig: "void PollEvents()", desc: "OS にたまった<t>イベント</t>（入力・サイズ変更・閉じる要求など）を取り出して処理する。<b>毎フレーム必ず呼ぶ</b>。呼ばないとウィンドウが固まる。" },
        { sig: "bool ShouldClose() const", ret: "閉じる要求が来たか", desc: "× ボタンなどで閉じる要求が出たら true。メインループの継続条件に使う。" },
        { sig: "void Close()", desc: "ゲームのコード側から「閉じたい」と要求する（次の <code>ShouldClose()</code> が true になる）。", when: "「ゲーム終了」メニューを押した時など。" },
        { sig: "u32 Width() const / u32 Height() const", ret: "中身の幅・高さ", desc: "描画できる領域（クライアント領域）のピクセルサイズ。" },
        { sig: "void* NativeHandle() const", ret: "HWND", desc: "Windows の生のウィンドウハンドル（HWND）。<t>スワップチェーン</t>作成など描画モジュールに渡す。", when: "Render モジュールに描画先を教える時。" },
        { sig: "void SetEventCallback(EventCallback cb, void* user)", desc: "イベントが起きた時に呼ぶ関数を登録する。<code>user</code> はそのままコールバックに渡される任意の<t>ポインタ</t>。", when: "<code>FInput</code> にイベントを橋渡しする時など。", sample: "w.SetEventCallback([](void* u, const FEvent&amp; e){ FInput::OnEvent(e); }, nullptr);" },
        { sig: "void SetTitle(const wchar_t* title)", desc: "タイトルバーの文字を変える。", when: "FPS や状態をタイトルに出したい時。" },
        { sig: "void SetFullscreen(bool on)", desc: "ボーダーレス全画面の ON/OFF。元のウィンドウ位置・サイズを覚えていて、OFF にすると元に戻る。" },
        { sig: "bool IsFullscreen() const", ret: "全画面中か", desc: "今が全画面なら true。" }
      ]
    },
    {
      name: "FWindowConfig",
      kind: "構造体", header: "platform/Window.h",
      summary: "<t>ウィンドウ</t>を作る時の設定をまとめた小さな箱。タイトル・サイズ・全画面かどうかなどを指定する。",
      when: "<code>FWindow::Create</code> に渡す。必要な項目だけ書き換えれば、残りは無難な既定値が入る。",
      sample: "FWindowConfig cfg;\ncfg.title      = L&quot;勇者の冒険&quot;;\ncfg.width      = 1920;\ncfg.height     = 1080;\ncfg.resizable  = false;   // 固定サイズ窓\ncfg.fullscreen = false;",
      members: [
        { sig: "const wchar_t* title", desc: "タイトルバーに出す文字（既定 <code>L&quot;ACS Window&quot;</code>）。" },
        { sig: "u32 width / u32 height", desc: "ウィンドウの中身のサイズ（既定 1280 × 720）。" },
        { sig: "bool resizable", desc: "ユーザーが端をドラッグしてサイズ変更できるか（既定 true）。" },
        { sig: "bool fullscreen", desc: "起動時から全画面にするか（既定 false）。" },
        { sig: "bool vsync_hint", desc: "垂直同期を使いたい、というヒント（既定 true）。" }
      ]
    },
    {
      name: "FEvent",
      kind: "構造体", header: "platform/Event.h",
      summary: "ウィンドウや入力で起きた『出来事』1 つ分を表す。種別（<code>type</code>）に応じて、中の<t>union</t>のうち対応するフィールドだけが有効になる。",
      when: "<code>SetEventCallback</code> で受け取る。<code>type</code> を見て分岐し、対応するフィールドを読む。",
      sample: "void OnEvent(void* u, const FEvent&amp; e) {\n    if (e.type == EEventType::WindowResize) {\n        u32 w = e.resize.width, h = e.resize.height;\n    } else if (e.type == EEventType::KeyPressed) {\n        EKey k = e.key.key;\n    }\n}",
      members: [
        { sig: "EEventType type", desc: "イベントの種別。これを見てどのフィールドが有効か判断する。" },
        { sig: "{ u32 width, height; } resize", desc: "<code>WindowResize</code> 時の新しいサイズ。" },
        { sig: "{ EKey key; bool repeat; } key", desc: "キー系イベントの対象キーと、押しっぱなしリピートか。" },
        { sig: "{ EMouseButton button; } mouse_button", desc: "マウスボタン系イベントの対象ボタン。" },
        { sig: "{ f32 x, y; f32 dx, dy; } mouse_move", desc: "<code>MouseMoved</code> 時の位置と移動量。" },
        { sig: "{ f32 x, y; } mouse_scroll", desc: "<code>MouseScrolled</code> 時のホイール量。" },
        { sig: "{ u32 codepoint; } char_input", desc: "<code>CharInput</code> 時の確定文字（Unicode コードポイント）。" }
      ]
    },
    {
      name: "EEventType",
      kind: "列挙(enum)", header: "platform/Event.h",
      summary: "<t>FEvent</t> の種別を表す列挙。ウィンドウ系（閉じる・サイズ変更・フォーカス）と入力系（キー・マウス・文字）がある。",
      when: "受け取った <code>FEvent</code> の <code>type</code> と比較して分岐する。",
      sample: "switch (e.type) {\n    case EEventType::WindowClose:  /* 閉じる */ break;\n    case EEventType::KeyPressed:   /* 押下   */ break;\n    case EEventType::MouseScrolled:/* ホイール*/ break;\n}",
      members: [
        { sig: "None", desc: "未設定（無効なイベント）。" },
        { sig: "WindowClose / WindowResize", desc: "× ボタン押下 / ウィンドウサイズ変更。" },
        { sig: "WindowFocus / WindowLostFocus", desc: "フォーカスの獲得 / 喪失。" },
        { sig: "KeyPressed / KeyReleased / KeyRepeat", desc: "キーの押下開始 / 解放 / 押しっぱなしリピート。" },
        { sig: "MouseButtonPressed / MouseButtonReleased", desc: "マウスボタンの押下 / 解放。" },
        { sig: "MouseMoved / MouseScrolled", desc: "カーソル移動 / ホイール回転。" },
        { sig: "CharInput", desc: "文字入力（IME 確定後のテキスト用）。" }
      ]
    },
    {
      name: "FInput",
      kind: "クラス（静的）", header: "platform/Input.h",
      summary: "キーボード・マウス・<t>ゲームパッド</t>の状態を<b>ポーリング</b>（その瞬間に問い合わせ）で読む入口。すべて静的関数なので <code>FInput::IsKeyDown(...)</code> のように呼ぶ。",
      when: "ゲームの操作処理。毎フレーム先頭で <code>Update()</code> を呼んでから状態を読む。",
      sample: "FInput::Update();          // フレーム先頭で1回\nw.PollEvents();\nif (FInput::IsKeyDown(EKey::Space))            Jump();\nif (FInput::IsMouseButtonPressed(EMouseButton::Left)) Shoot();\nFVec2 m = FInput::MousePos();",
      members: [
        { sig: "static void Update()", desc: "フレームの先頭で 1 回呼ぶ。前フレームの状態を保存し、Pressed/Released の判定を成立させる。<b>呼び忘れると Pressed が効かない</b>。", when: "メインループの一番上。" },
        { sig: "static void OnEvent(const FEvent&amp; e)", desc: "<code>FWindow</code> から来た<t>イベント</t>を <code>FInput</code> に流し込む橋渡し。<code>SetEventCallback</code> でこれを呼ぶよう繋ぐ。" },
        { sig: "static bool IsKeyDown(EKey k)", ret: "押されているか", desc: "そのキーが今<b>押されている</b>か（押しっぱなしの間ずっと true）。", when: "移動キーなど、押している間ずっと反応させたい時。" },
        { sig: "static bool IsKeyPressed(EKey k)", ret: "今押されたか", desc: "<b>このフレームで押された瞬間</b>だけ true（前フレームは押されていない）。", when: "ジャンプや決定など、1 回だけ反応させたい時。" },
        { sig: "static bool IsKeyReleased(EKey k)", ret: "今離されたか", desc: "<b>このフレームで離された瞬間</b>だけ true。" },
        { sig: "static bool IsMouseButtonDown / Pressed / Released(EMouseButton b)", desc: "マウスボタン版の Down / Pressed / Released。意味はキーと同じ。" },
        { sig: "static FVec2 MousePos()", ret: "カーソル位置", desc: "現在のマウス位置（クライアント座標、左上原点）。" },
        { sig: "static FVec2 MouseDelta()", ret: "移動量", desc: "前フレームからのマウス移動量。", when: "FPS 視点の回転など、相対移動で操作する時。" },
        { sig: "static f32 MouseWheel()", ret: "ホイール量", desc: "このフレームのホイール回転（正で奥、負で手前）。" },
        { sig: "static const char* TextInput()", ret: "入力文字列", desc: "このフレームに確定した文字列（UTF-8、IME 確定後）。無ければ空文字。", when: "名前入力欄やチャットなど、文字を打たせる時。" },
        { sig: "static bool IsGamepadConnected(u32 player_index)", ret: "接続中か", desc: "指定プレイヤー番号（0〜3）のゲームパッドが繋がっているか。" },
        { sig: "static bool IsGamepadButtonDown / Pressed(u32 idx, EGamepadButton b)", desc: "ゲームパッドのボタン状態。Down=押している間、Pressed=押した瞬間。" },
        { sig: "static f32 GamepadAxisValue(u32 idx, EGamepadAxis axis)", ret: "軸の値", desc: "スティックは -1.0〜+1.0、トリガーは 0.0〜1.0。", sample: "f32 x = FInput::GamepadAxisValue(0, EGamepadAxis::LeftX);" }
      ]
    },
    {
      name: "EKey",
      kind: "列挙(enum)", header: "platform/InputCodes.h",
      summary: "キーボードのキーを表す列挙。A〜Z、数字、ファンクション、修飾キー、矢印、記号、テンキーまで初学者にも分かりやすい名前で揃っている。",
      when: "<code>FInput::IsKeyDown</code> などにキーを指定する時。",
      sample: "if (FInput::IsKeyDown(EKey::W))      MoveUp();\nif (FInput::IsKeyPressed(EKey::Escape)) OpenMenu();\nif (FInput::IsKeyDown(EKey::LeftShift)) Run();",
      members: [
        { sig: "A 〜 Z", desc: "文字キー。" },
        { sig: "Num0 〜 Num9", desc: "上段の数字キー。" },
        { sig: "F1 〜 F12", desc: "ファンクションキー。" },
        { sig: "LeftShift / RightShift / LeftCtrl / RightCtrl / LeftAlt / RightAlt / LeftSuper / RightSuper", desc: "修飾キー（Super は Windows / Cmd キー）。" },
        { sig: "Up / Down / Left / Right", desc: "矢印キー。" },
        { sig: "Space / Enter / Tab / Backspace / Escape", desc: "代表的な制御キー。" },
        { sig: "Insert / Delete / Home / End / PageUp / PageDown / CapsLock / NumLock / ScrollLock", desc: "その他の制御キー。" },
        { sig: "Minus / Equal / LeftBracket / RightBracket / Backslash / Semicolon / Apostrophe / Comma / Period / Slash / Grave", desc: "記号キー。" },
        { sig: "KP0 〜 KP9 / KPAdd / KPSubtract / KPMultiply / KPDivide / KPEnter / KPDecimal", desc: "テンキー。" },
        { sig: "Unknown", desc: "対応する列挙のないキー。" }
      ]
    },
    {
      name: "EMouseButton",
      kind: "列挙(enum)", header: "platform/InputCodes.h",
      summary: "マウスのボタンを表す列挙。左・右・中（ホイール押し込み）とサイドボタン 2 つ。",
      when: "<code>FInput::IsMouseButtonDown</code> などに渡す。",
      sample: "if (FInput::IsMouseButtonPressed(EMouseButton::Left))   Fire();\nif (FInput::IsMouseButtonDown(EMouseButton::Right))     Aim();",
      members: [
        { sig: "Left = 0 / Right = 1 / Middle = 2", desc: "左・右・中ボタン（中はホイール押し込み）。基底型は <code>u8</code>。" },
        { sig: "X1 = 3 / X2 = 4", desc: "サイドボタン 1・2（戻る/進む等）。末尾に内部用の番兵 <code>_Count</code> がある。" }
      ]
    },
    {
      name: "EGamepadButton",
      kind: "列挙(enum)", header: "platform/InputCodes.h",
      summary: "<t>ゲームパッド</t>のボタンを表す列挙（XInput 互換）。フェイスボタン・十字キー・バンパー・スティック押し込み・システムボタン。",
      when: "<code>FInput::IsGamepadButtonDown / Pressed</code> に渡す。",
      sample: "if (FInput::IsGamepadButtonPressed(0, EGamepadButton::A))     Jump();\nif (FInput::IsGamepadButtonDown(0, EGamepadButton::RightBumper)) Guard();",
      members: [
        { sig: "A / B / X / Y", desc: "フェイスボタン（XInput 配置）。" },
        { sig: "Up / Down / Left / Right", desc: "十字キー（D-Pad）。" },
        { sig: "LeftBumper / RightBumper", desc: "左右のバンパー（L1/R1 相当）。" },
        { sig: "LeftStick / RightStick", desc: "スティックの押し込み（L3/R3 相当）。" },
        { sig: "Start / Back / Guide", desc: "スタート・バック・ガイド（中央）ボタン。" }
      ]
    },
    {
      name: "EGamepadAxis",
      kind: "列挙(enum)", header: "platform/InputCodes.h",
      summary: "<t>ゲームパッド</t>のアナログ軸を表す列挙。左右スティックの X/Y と、左右トリガー。",
      when: "<code>FInput::GamepadAxisValue</code> に渡す。スティックは -1.0〜+1.0、トリガーは 0.0〜1.0 が返る。",
      sample: "f32 mx = FInput::GamepadAxisValue(0, EGamepadAxis::LeftX);\nf32 my = FInput::GamepadAxisValue(0, EGamepadAxis::LeftY);\nf32 rt = FInput::GamepadAxisValue(0, EGamepadAxis::RightTrigger);",
      members: [
        { sig: "LeftX / LeftY", desc: "左スティックの横・縦（-1.0〜+1.0）。" },
        { sig: "RightX / RightY", desc: "右スティックの横・縦（-1.0〜+1.0）。" },
        { sig: "LeftTrigger / RightTrigger", desc: "左右トリガーの踏み込み（0.0〜1.0）。" }
      ]
    },
    {
      name: "FClock",
      kind: "クラス（静的）", header: "platform/Time.h",
      summary: "高精度な<t>タイマ</t>。起動からの経過時間を秒・ミリ秒・<t>ティック</t>で取得できる。すべて静的関数。",
      when: "ベンチマークや経過時間の計測。フレーム間の <code>dt</code> が欲しいだけなら <t>FFrameTimer</t> の方が便利。",
      sample: "f64 t0 = FClock::SecondsSinceStartup();\nDoHeavyWork();\nf64 elapsed = FClock::SecondsSinceStartup() - t0;",
      members: [
        { sig: "static f64 SecondsSinceStartup()", ret: "経過秒", desc: "起動からの経過秒（高精度）。" },
        { sig: "static u64 MillisSinceStartup()", ret: "経過ミリ秒", desc: "起動からの経過ミリ秒。" },
        { sig: "static u64 Ticks()", ret: "経過ティック", desc: "起動からの生の<t>ティック</t>値。最も細かい計測に。実装は <code>std::chrono::steady_clock</code> ベース（Windows では内部的に QPC、Linux/macOS では <code>CLOCK_MONOTONIC</code>）。1 ティックの粒度はプラットフォーム依存（多くの環境でナノ秒）なので、固定値を仮定せず必ず <code>TicksPerSecond()</code> で秒に換算する。" },
        { sig: "static u64 TicksPerSecond()", ret: "1秒分のティック数", desc: "1 秒あたりのティック数。<code>Ticks()</code> を秒に換算する時の分母に使う（例: <code>秒 = Ticks() / TicksPerSecond()</code>）。" }
      ]
    },
    {
      name: "FFrameTimer",
      kind: "クラス", header: "platform/Time.h",
      summary: "<t>フレーム</t>ごとの経過時間（<t>デルタタイム</t>）を計るタイマ。毎フレーム <code>Tick()</code> を呼ぶと前回からの経過秒を返す。平均 FPS も取れる。",
      when: "ゲームループの中で、移動量などを <code>速度 × dt</code> で計算したい時。フレームレートに依存しない動きの基本。",
      sample: "FFrameTimer ft;\nwhile (running) {\n    f32 dt = ft.Tick();   // 前フレームからの経過秒\n    pos += velocity * dt; // フレームレート非依存\n    if (ft.FrameCount() % 60 == 0) ShowFps(ft.SmoothedFPS());\n}",
      members: [
        { sig: "f32 Tick()", ret: "経過秒(dt)", desc: "前回 <code>Tick()</code> からの経過秒を返し、内部時刻を更新する。停止後の暴走を防ぐため最大 0.25 秒に制限される。", when: "毎フレーム先頭で 1 回。" },
        { sig: "f32 SmoothedDelta() const", ret: "平均dt", desc: "直近数フレームを平滑化した <code>dt</code>。表示用に滑らかな値が欲しい時。" },
        { sig: "f32 SmoothedFPS() const", ret: "平均FPS", desc: "平滑化された FPS。タイトルバーや HUD の FPS 表示に。" },
        { sig: "u64 FrameCount() const", ret: "累積フレーム数", desc: "起動からの総フレーム数。" },
        { sig: "f64 TotalSeconds() const", ret: "累積秒", desc: "起動からの累積秒。" }
      ]
    },
    {
      name: "FFileSystem",
      kind: "クラス（静的）", header: "platform/FileSystem.h",
      summary: "ファイルの読み書きとパス操作をまとめた静的クラス。ファイル全体をまるごと読む / 書く、存在確認、ディレクトリ作成などができる。結果は <t>Result</t> で返るので成否を必ず確認する。",
      when: "アセットの読み込み、ログやセーブのバイト書き出しなど、生のファイル操作が必要な時。設定/セーブの key-value 永続化なら <t>FStorage</t> が便利。",
      sample: "auto r = FFileSystem::ReadAllBytes(L&quot;data/map.bin&quot;);\nif (r.IsErr()) return;\nTArray&lt;byte&gt; bytes = static_cast&lt;TArray&lt;byte&gt;&amp;&amp;&gt;(r.Value());\n// ...\nFFileSystem::WriteAllBytes(L&quot;data/out.bin&quot;, bytes.Data(), bytes.Size());",
      members: [
        { sig: "static TResult&lt;TArray&lt;byte&gt;&gt; ReadAllBytes(const wchar_t* path)", ret: "バイト列 or エラー", desc: "ファイル全体をバイト配列として読み込む。", when: "バイナリアセットやセーブデータを丸ごと読む時。" },
        { sig: "static TResult&lt;TArray&lt;char&gt;&gt; ReadAllText(const wchar_t* path)", ret: "文字列 or エラー", desc: "ファイル全体を文字列として読み込む（末尾に NUL を付与）。", when: "テキスト設定や JSON を読む時。" },
        { sig: "static TResult&lt;void&gt; WriteAllBytes(const wchar_t* path, const byte* data, usize size)", desc: "バイト列をファイルに書き出す（既存は上書き）。" },
        { sig: "static TResult&lt;void&gt; WriteAllText(const wchar_t* path, const char* text)", desc: "文字列をファイルに書き出す（NUL は書き込まない）。" },
        { sig: "static TResult&lt;u64&gt; FileSize(const wchar_t* path)", ret: "バイト数 or エラー", desc: "ファイルサイズを取得する。" },
        { sig: "static bool Exists(const wchar_t* path)", ret: "存在するか", desc: "ファイルが存在すれば true。" },
        { sig: "static bool DirectoryExists(const wchar_t* path)", ret: "存在するか", desc: "ディレクトリが存在すれば true。" },
        { sig: "static TResult&lt;void&gt; CreateDirectory(const wchar_t* path)", desc: "ディレクトリを作る（既存なら成功扱い、親ディレクトリも再帰的に作成）。" },
        { sig: "static TResult&lt;void&gt; Delete(const wchar_t* path)", desc: "ファイルを削除する。" }
      ]
    },
    {
      name: "FStorage",
      kind: "クラス", header: "platform/Storage.h",
      summary: "設定やセーブデータ用の <b>key-value ストア</b>（INI 形式）。<code>master_volume=0.8</code> のような人間が読める形でファイルに保存できる。文字列・整数・小数・真偽を型ごとに出し入れする。",
      when: "音量・解像度などの設定や、ハイスコア・進行度などのセーブを手軽に永続化したい時。",
      sample: "FStorage cfg;\nwchar_t path[260];\nFStorage::GetAppDataPath(L&quot;MyGame&quot;, L&quot;settings.ini&quot;, path, 260);\ncfg.Load(path);\nf64 vol = cfg.GetFloat(&quot;master_volume&quot;, 0.8);\ncfg.SetInt(&quot;high_score&quot;, 12345);\ncfg.Save(path);",
      members: [
        { sig: "TResult&lt;void&gt; Load(const wchar_t* path) / Load(const char* path_utf8)", desc: "INI ファイルから読み込む。ファイルが無くても空のまま成功扱い。" },
        { sig: "TResult&lt;void&gt; LoadFromBytes(const u8* data, usize size)", desc: "メモリ上のバイト列（INI 形式 UTF-8）から読み込む。実行ファイル埋め込み用。" },
        { sig: "TResult&lt;void&gt; Save(const wchar_t* path) / Save(const char* path_utf8)", desc: "INI ファイルへ保存する。親ディレクトリが無ければ作る。" },
        { sig: "void Clear()", desc: "全エントリを消す（ファイルには触らない）。" },
        { sig: "void SetString / SetInt / SetFloat / SetBool(const char* key, value)", desc: "キーに値を設定する（型ごとに 4 種）。" },
        { sig: "const char* GetString(const char* key, const char* default_v = \"\")", ret: "文字列", desc: "キーの文字列を取得。無ければ既定値。返る参照は内部バッファなので、長く持つならコピーする。" },
        { sig: "i64 GetInt / f64 GetFloat / bool GetBool(const char* key, default_v)", desc: "キーの値を型付きで取得。無ければ既定値を返す。" },
        { sig: "bool Has(const char* key) const", ret: "存在するか", desc: "そのキーがあれば true。" },
        { sig: "void Remove(const char* key)", desc: "1 つのキーを削除する。" },
        { sig: "usize Count() const", ret: "件数", desc: "保持しているエントリ数。" },
        { sig: "static TResult&lt;void&gt; GetAppDataPath(sub_dir, file_name, out, cap)", ret: "フルパス", desc: "<code>%APPDATA%/&lt;sub_dir&gt;/&lt;file_name&gt;</code> のフルパスを <code>out</code> に書き込む。親ディレクトリも自動作成。wchar_t 版と UTF-8 版がある。", when: "ユーザー別のセーブ場所を OS の作法で決めたい時。" }
      ]
    },
    {
      name: "FLocalization",
      kind: "クラス", header: "platform/Localization.h",
      summary: "<b>多言語対応（i18n）</b>の仕組み。各言語を <t>FStorage</t>（INI 形式の翻訳辞書）として持ち、キーから現在言語の文字列を引く。見つからなければ代替言語、それも無ければキー名そのものを返す。",
      when: "メニューやセリフを日本語・英語など複数言語で出したい時。表示文字列をコードに直書きせず、言語ファイルにまとめる。",
      sample: "FLocalization L;\nL.LoadActive(L&quot;lang/ja.lang&quot;);     // 現在言語\nL.LoadFallback(L&quot;lang/en.lang&quot;);   // 代替\n// 描画時:\nsb.DrawText(font, L.Tr(&quot;menu.start&quot;), x, y, color);",
      members: [
        { sig: "TResult&lt;void&gt; LoadActive(const wchar_t* path)", desc: "現在表示する言語をファイルから読み込む。" },
        { sig: "TResult&lt;void&gt; LoadFallback(const wchar_t* path)", desc: "キーが無い時に使う代替言語を読み込む。" },
        { sig: "TResult&lt;void&gt; LoadActiveBytes / LoadFallbackBytes(const u8* data, usize size)", desc: "メモリ上のバイト列から読み込む（埋め込み用）。" },
        { sig: "void Swap()", desc: "active と fallback を入れ替える（簡易な言語切替）。" },
        { sig: "void Clear()", desc: "両方の言語辞書を空にする。" },
        { sig: "const char* Tr(const char* key) const", ret: "翻訳文字列", desc: "キーを翻訳して返す。active → fallback → キー自体 の順で探す。", when: "画面に出すすべての文字列をこれ経由で取る。", sample: "const char* s = L.Tr(&quot;greeting&quot;);" },
        { sig: "bool Has(const char* key) const", ret: "存在するか", desc: "active か fallback にキーがあれば true。" },
        { sig: "FStorage&amp; Active() / Fallback()", ret: "FStorage 参照", desc: "中の翻訳辞書（<code>FStorage</code>）に直接アクセスする。独自に加工したい時用。" }
      ]
    },
    {
      name: "EventCallback",
      kind: "型エイリアス", header: "platform/Window.h",
      summary: "<code>FWindow</code> が<t>イベント</t>を通知するときに呼ぶ<b>関数ポインタ型</b>。<code>void (*)(void* user, const FEvent&amp; e)</code> の別名で、<code>user</code> には <code>SetEventCallback</code> に渡した任意<t>ポインタ</t>がそのまま渡る。<code>PollEvents</code> 中に 1 フレームで複数回呼ばれうる。",
      when: "<code>FWindow::SetEventCallback</code> に渡すコールバックの型として使う。多くは <code>FInput::OnEvent</code> へ橋渡しするために用いる。",
      sample: "void OnEvent(void* user, const FEvent&amp; e) {\n    FInput::OnEvent(e);   // FWindow のイベントを FInput へ流す\n}\nw.SetEventCallback(OnEvent, nullptr);\n// ラムダ(キャプチャ無し)も関数ポインタに変換できる:\nw.SetEventCallback([](void* u, const FEvent&amp; e){ FInput::OnEvent(e); }, nullptr);"
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "ウィンドウ": "OS が画面に出す四角い領域。タイトルバーや × ボタンを持ち、ゲームの描画先になる。ACS では <t>FWindow</t>。",
  "イベント": "『× ボタンが押された』『キーが押された』のような、その瞬間に起きた出来事の通知。ACS では <t>FEvent</t> として届く。",
  "入力": "キーボード・マウス・<t>ゲームパッド</t>からの操作。ACS では <code>FInput</code> でその瞬間の状態を読む。",
  "ゲームパッド": "コントローラ。ACS は XInput 互換でボタンやスティック（軸）を読む。",
  "ポーリング": "状態を必要なタイミングで自分から問い合わせる方式。<code>FInput::IsKeyDown</code> はその瞬間の押下状態を返す。",
  "フレーム": "ゲームループの 1 回分の更新＋描画。1 秒に何フレーム回るかが FPS。",
  "デルタタイム": "前フレームから今フレームまでの経過秒（dt）。<code>速度 × dt</code> で動かすとフレームレートに依存しない。",
  "ティック": "高精度タイマ（QPC）が刻む最小単位の時間。<code>TicksPerSecond()</code> で秒に換算する。",
  "タイマ": "時間を計る仕組み。ACS では <t>FClock</t>（経過時間）と <t>FFrameTimer</t>（フレーム間 dt）。",
  "union": "同じメモリ領域を複数の型で共用する仕組み。<t>FEvent</t> は種別ごとに有効なフィールドだけを使う。",
  "スワップチェーン": "描画した絵を画面に出すための表裏のバッファ列。作成にウィンドウの <code>NativeHandle()</code> を渡す。"
});
