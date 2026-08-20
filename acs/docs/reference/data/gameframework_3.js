/* ACS リファレンス — gameframework モジュール (パート 3)。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "gameframework", order: 42, title: "gameframework — ゲーム部品 (acs::game)",
  blurb: "ゲームを組み立てるための高レベル部品。会話・ダンジョン生成・難易度調整・画面演出・エフェクト・体力管理など、よくある仕組みを<t>state holder</t>として用意し、描画や音は呼び出し側に任せます。<code>acs::game</code> 名前空間。",
  types: [
    // =====================================================================
    // 会話 / ノベル系
    // =====================================================================
    {
      name: "FDialogueSystem",
      kind: "クラス", header: "gameframework/DialogueSystem.h",
      summary: "1 行ずつ会話テキストを送り出す<t>state holder</t>。<t>タイプライタ</t>演出 (1 文字ずつ表示) と分岐<t>選択肢</t>を扱う。描画や入力には触れず、「今どの行か」「何文字まで見えているか」「選択肢を出している最中か」だけを保持する。",
      when: "NPC との会話、シナリオ、イベントシーンを作りたい時。実際の文字描画は UI 層で行い、この型は進行状態だけを管理する。",
      sample: "FDialogueSystem dlg;\ndlg.AddLine({ \"アリス\", \"こんにちは、勇者よ。\", 30.0f });\ndlg.AddLine({ \"アリス\", \"旅の目的を聞かせて。\", 30.0f });\ndlg.Start();\n// 毎フレーム:\ndlg.Tick(dt);                // タイプライタを進める\nif (input.JustPressed(EKey::Enter)) dlg.AdvanceLine();",
      members: [
        { sig: "void AddLine(const FDialogueLine& line)", desc: "会話行を末尾に追加。挿入順に再生される。" },
        { sig: "void AddChoices(u32 at_line_index, const FDialogueChoice* choices, u32 count)", desc: "指定行の表示完了後に出す選択肢群を登録。同じ行に 2 回目以降の登録は無視。", when: "「はい / いいえ」のような分岐を入れたい時。" },
        { sig: "void Start()", desc: "先頭行から再生開始。行が空なら何もしない。" },
        { sig: "void AdvanceLine()", desc: "次の行へ。タイプ完了済みで、かつ選択肢が pending でない時のみ進む。末尾で呼ぶと完了。" },
        { sig: "void SkipTypewriter()", desc: "現在行を即座に全文表示 (タイプ中なら強制完了)。", when: "プレイヤーがボタン連打で早送りしたい時。" },
        { sig: "void ChooseOption(u32 choice_index)", desc: "分岐選択を確定し、選択肢の次行 index へジャンプ (範囲外なら終了)。" },
        { sig: "bool IsActive() / bool IsCompleted()", desc: "再生中か / 全行を終えたか。" },
        { sig: "bool HasChoicesPending()", ret: "選択肢提示中か", desc: "タイプ完了 + 選択肢登録あり + 未選択の状態。" },
        { sig: "const FDialogueLine* CurrentLine()", ret: "現在行 or null", desc: "非アクティブ時は nullptr。" },
        { sig: "u32 VisibleCharCount()", ret: "表示文字数", desc: "タイプライタで今見えている文字数 (本文先頭から)。" },
        { sig: "void Tick(f32 dt)", desc: "dt 秒進める。タイプライタ進行と auto-advance を行う。" },
        { sig: "void SetAutoAdvanceDelay(f32 delay_sec)", desc: "タイプ完了 + 一定秒で自動的に次行へ進む。<=0 で無効 (既定)。選択肢のある行では発火しない。" }
      ]
    },
    {
      name: "FDialogueLine",
      kind: "構造体", header: "gameframework/DialogueSystem.h",
      summary: "会話 1 行のデータ。発話者名・本文・タイプ速度を持つ。文字列は<b>所有しない</b> (リテラルや外部バンドルを参照)。",
      when: "<code>FDialogueSystem::AddLine</code> に渡す 1 行を組み立てる時。",
      sample: "FDialogueLine l;\nl.speaker        = \"アリス\";   // nullptr 可\nl.text           = \"やあ！\";    // nullptr は空行扱い\nl.type_speed_cps = 30.0f;        // 1 秒あたり 30 文字、<=0 で瞬時表示",
      members: [
        { sig: "const char* speaker", desc: "発話者名 (nullptr 可)。" },
        { sig: "const char* text", desc: "本文 (nullptr は空行)。" },
        { sig: "f32 type_speed_cps", desc: "タイプ速度 (chars per second)。<=0 で瞬時表示。" }
      ]
    },
    {
      name: "FDialogueChoice",
      kind: "構造体", header: "gameframework/DialogueSystem.h",
      summary: "分岐選択肢 1 つ。表示テキストとジャンプ先行 index を持つ。<code>next_line_index</code> が行数以上なら「会話終了」を表す。",
      when: "<code>FDialogueSystem::AddChoices</code> に渡す選択肢配列を作る時。",
      sample: "FDialogueChoice ch[2];\nch[0] = { \"はい\", 5 };   // 行 5 へジャンプ\nch[1] = { \"いいえ\", 0xFFFFFFFFu }; // 範囲外 = 会話終了\ndlg.AddChoices(/*at_line=*/4, ch, 2);"
    },
    {
      name: "FDialogueScript",
      kind: "クラス", header: "gameframework/DialogueScript.h",
      summary: "ノベル / アドベンチャー向けのシーンスクリプト再生機。「セリフ → ポートレート表示 → BGM 切替 → 選択肢 → ジャンプ」を命令列 (<code>FScriptOp[]</code>) とラベルで宣言的に組む<t>ステートマシン</t>。各命令は<t>コールバック</t>で外部へ通知し、実際の描画・音・入力は呼び出し側が行う。",
      when: "VN (ビジュアルノベル) やイベントシーンを、台本のように命令列で書きたい時。",
      sample: "FScriptOp ops[] = {\n  { EScriptOpKind::Background, \"bg_room\", nullptr },\n  { EScriptOpKind::Say,        \"アリス\", \"おかえり。\" },\n  { EScriptOpKind::EndScene },\n};\nscript.LoadScript(ops, 3, \"scene1\");\nscript.SetOnSayCallback(&OnSay, this);\nscript.Start();\n// 毎フレーム:\nscript.Tick(dt);\nif (clicked) script.Advance();   // Say 後の入力待ちを解除",
      members: [
        { sig: "void LoadScript(const FScriptOp* ops, u32 op_count, const char* script_id)", desc: "命令列をロード (内部に複製するので caller が解放しても安全)。State は Idle に。" },
        { sig: "void AddLabel(const char* label, u32 op_index)", desc: "ラベル名 → 命令 index を登録。Jump や Start(label) の飛び先になる。同名は最初の登録のみ有効。" },
        { sig: "void Start(const char* start_label = nullptr)", desc: "再生開始。ラベル省略で先頭から。" },
        { sig: "void Advance()", desc: "Say 命令後の入力待ち (AwaitingInput) を解除して次へ。", when: "プレイヤーが「次へ」を押した時。" },
        { sig: "void SelectChoice(u32 choice_index)", desc: "選択肢を確定し、その jump_label へジャンプする。" },
        { sig: "void Tick(f32 dt)", desc: "dt 秒進める。Wait 命令のタイマや即進行する命令を消化する。" },
        { sig: "EDialogueScriptState State()", ret: "現在の状態", desc: "Idle / Playing / AwaitingInput / AwaitingChoice / Finished。" },
        { sig: "const FScriptOp* CurrentOp()", desc: "今実行中の命令を返す。" },
        { sig: "u32 CurrentChoiceCount() / const FScriptChoice* CurrentChoice(u32 index)", desc: "選択待ち中に展開された選択肢の数と各要素。" },
        { sig: "void SetOnSayCallback(SayCallback cb, void* user)", desc: "セリフ命令の通知先を登録。ほかに Show/Hide/Background/PlayBgm/StopBgm/PlaySe/ChoicePresent/End 用の Set*Callback がある。" },
        { sig: "void Stop() / void ClearAll()", desc: "完全停止 / 命令・ラベル・コールバック・状態を全破棄。" }
      ]
    },
    {
      name: "FScriptOp",
      kind: "構造体", header: "gameframework/DialogueScript.h",
      summary: "<t>FDialogueScript</t> の命令 1 つ。種別 (<code>EScriptOpKind</code>) と引数 (文字列 2 つ + 数値 + u32) を持つ。文字列は所有しない。",
      when: "スクリプトの台本配列を直接組み立てる時。",
      sample: "FScriptOp say  { EScriptOpKind::Say, \"アリス\", \"やあ\" };\nFScriptOp bgm  { EScriptOpKind::PlayBgm, \"town\", nullptr, 0.8f }; // arg_f=音量\nFScriptOp wait { EScriptOpKind::Wait, nullptr, nullptr, 1.5f };  // 1.5 秒待つ"
    },
    {
      name: "FScriptChoice",
      kind: "構造体", header: "gameframework/DialogueScript.h",
      summary: "<t>FDialogueScript</t> の選択肢 1 つ。表示ラベルと選択時のジャンプ先ラベル名を持つ。",
      when: "選択待ち中に <code>CurrentChoice()</code> で取り出して UI に並べる時。"
    },
    {
      name: "EScriptOpKind",
      kind: "列挙(enum)", header: "gameframework/DialogueScript.h",
      summary: "スクリプト命令の種別。Say / Show / Hide / Background / PlayBgm / StopBgm / PlaySe / Choice / Wait / Jump / EndScene の 11 種。",
      when: "<code>FScriptOp::kind</code> に設定する命令の種類を選ぶ時。"
    },
    {
      name: "EDialogueScriptState",
      kind: "列挙(enum)", header: "gameframework/DialogueScript.h",
      summary: "スクリプト全体の状態。Idle (停止) / Playing (進行中) / AwaitingInput (Say 後の入力待ち) / AwaitingChoice (選択待ち) / Finished (終了)。",
      when: "<code>FDialogueScript::State()</code> の戻り値を見て UI を切り替える時。"
    },
    {
      name: "FDialogueLocalizer",
      kind: "クラス", header: "gameframework/DialogueLocalizer.h",
      summary: "会話と<t>ローカライズ</t> (多言語化) を疎結合で繋ぐ橋渡し。本文を持たず「行 → 翻訳キー」だけを保持し、現在の<t>ロケール</t>で翻訳を解決して<t>コールバック</t>に渡す。発話者名も翻訳キー扱い。",
      when: "同じシナリオ行を言語ごとに別文字列で出したい時。<code>FLocalizationDirector</code> (翻訳辞書) と <code>FDialogueSystem</code> を直結せず繋ぎたい時。",
      sample: "FDialogueLocalizer dl;\ndl.RegisterLine({ \"char.alice\", \"scene1.line0\", 30.0f });\ndl.SetLocalizer(&loc);   // 翻訳辞書を差し込む\ndl.StartFromLine(0, [](void*, const char* sp, const char* tx, f32 cps) noexcept {\n    // sp=\"アリス\", tx=\"こんにちは\" のように現ロケールで解決される\n}, nullptr);",
      members: [
        { sig: "void RegisterLine(const FLocalizedDialogueLine& line)", desc: "行を末尾に追加 (0,1,2... と index が振られる)。本文ではなく翻訳キーを登録する。" },
        { sig: "void RegisterChoice(u32 at_line_index, const FLocalizedDialogueChoice* choices, u32 count)", desc: "指定行に紐づく選択肢群を登録。同じ行に複数回は無視。" },
        { sig: "void SetLocalizer(FLocalizationDirector* loc)", desc: "翻訳ソースを差し込む。nullptr で切り離し (以降はキー文字列がそのまま返る)。所有しない。" },
        { sig: "void StartFromLine(u32 line_index, BindCallback cb, void* user)", desc: "指定行の発話者と本文を現ロケールで解決し cb を発火。未翻訳時もキー自身が返るので空欄事故は起きない。" },
        { sig: "u32 LineCount()", ret: "登録行数", desc: "登録済みの行数。" },
        { sig: "void Clear()", desc: "全行・全選択肢を破棄 (Localizer 参照は保持)。" }
      ]
    },
    {
      name: "FLocalizedDialogueLine",
      kind: "構造体", header: "gameframework/DialogueLocalizer.h",
      summary: "<t>FDialogueLocalizer</t> の 1 行ぶんのメタ。本文を持たず、発話者キー・本文キー・タイプ速度を持つ。文字列は所有しない。",
      when: "<code>RegisterLine</code> に渡す行を組み立てる時。"
    },
    {
      name: "FLocalizedDialogueChoice",
      kind: "構造体", header: "gameframework/DialogueLocalizer.h",
      summary: "<t>FDialogueLocalizer</t> の選択肢メタ。表示テキストの翻訳キーと、次行 index を持つ。",
      when: "<code>RegisterChoice</code> に渡す選択肢配列を作る時。"
    },
    // =====================================================================
    // ダンジョン生成 (ローグライク)
    // =====================================================================
    {
      name: "FDungeonGenerator",
      kind: "クラス", header: "gameframework/DungeonGenerator.h",
      summary: "2D グリッド上に部屋と廊下を配置する<b><t>BSP</t></b> (空間二分割) ダンジョン生成器。全体を再帰的に 2 分割し各リーフに 1 部屋を置き、隣り合う部屋を L 字廊下で繋ぐ。<t>シード</t>で結果を再現できる。",
      when: "ローグライクの「毎フロア違うレイアウト」を自動生成したい時。地形のみ生成し、モンスターやアイテム配置は呼び出し側で行う。",
      sample: "FDungeonGenConfig cfg;\ncfg.width = 80; cfg.height = 50;\ncfg.seed = 0xDEADBEEFu;\nFDungeonGenerator gen;\ngen.Generate(cfg);\nfor (u32 y = 0; y &lt; gen.Height(); ++y)\n  for (u32 x = 0; x &lt; gen.Width(); ++x)\n    if (gen.At(x, y) == ETileKind::Floor) DrawSprite(floor, x, y);\nu32 px, py; gen.GetRoomCenter(0, px, py);  // 開始位置 = 最初の部屋の中心",
      members: [
        { sig: "void Generate(const FDungeonGenConfig& config)", desc: "設定に従って全体を再生成 (既存状態は破棄)。不正値はサイレントに既定へフォールバックして必ず有効なグリッドを作る。" },
        { sig: "u32 Width() / u32 Height()", ret: "グリッドの大きさ", desc: "生成されたダンジョンの幅・高さ (タイル数)。" },
        { sig: "ETileKind At(u32 x, u32 y)", ret: "タイル種別", desc: "指定座標のタイルを返す。範囲外は Wall (通行不可)。" },
        { sig: "void SetTile(u32 x, u32 y, ETileKind kind)", desc: "タイルを書き換える。範囲外は no-op。", when: "生成後に手で宝箱や入口を埋め込みたい時。" },
        { sig: "u32 RoomCount() / const FRoom* GetRoom(u32 index)", desc: "部屋の数と各部屋 (範囲外は nullptr)。" },
        { sig: "void GetRoomCenter(u32 room_index, u32& out_x, u32& out_y)", desc: "部屋の中心座標を取得 (範囲外は (0,0))。", when: "プレイヤーや階段のスポーン位置を決める時。" },
        { sig: "bool IsWalkable(u32 x, u32 y)", ret: "通行可能か", desc: "Floor/Door/Corridor/Stairs を通行可とみなす。範囲外と Wall は false。" },
        { sig: "u32 FindRandomFloor(u32& out_x, u32& out_y)", ret: "試行回数 (0=失敗)", desc: "ランダムな床タイルの座標を探して書き込む。最大 100 試行。", when: "敵やアイテムをランダムな床に置きたい時。" },
        { sig: "void Clear()", desc: "グリッド・部屋・サイズをすべてリセット。" }
      ]
    },
    {
      name: "FDungeonGenConfig",
      kind: "構造体", header: "gameframework/DungeonGenerator.h",
      summary: "<t>FDungeonGenerator</t> の生成パラメータ。幅・高さ・部屋の最小/最大サイズ・分割の最小サイズ・<t>シード</t>・BSP 再帰の最大深さを持つ。0 や不整合な値は内部で安全な既定に補正される。",
      when: "ダンジョンの規模や部屋の大きさ、再現用シードを指定する時。"
    },
    {
      name: "FRoom",
      kind: "構造体", header: "gameframework/DungeonGenerator.h",
      summary: "生成された 1 部屋。左上座標 (x,y)・幅 w・高さ h・生成順 id を持つ軸並行矩形。",
      when: "<code>GetRoom()</code> で取り出し、部屋ごとのイベント配置や領域判定に使う時。"
    },
    {
      name: "ETileKind",
      kind: "列挙(enum)", header: "gameframework/DungeonGenerator.h",
      summary: "ダンジョンのタイル種別。Wall (壁) / Floor (床) / Door (扉) / Corridor (廊下) / Stairs (階段) の 5 種。",
      when: "<code>At()</code> の戻り値で分岐して描画やゲームロジックを切り替える時。"
    },
    // =====================================================================
    // 難易度調整
    // =====================================================================
    {
      name: "FDynamicDifficulty",
      kind: "クラス", header: "gameframework/DynamicDifficulty.h",
      summary: "プレイヤーの実プレイ統計 (死亡 / 撃破 / リトライ / クリア時間 / パワーアップ取得) を追跡し、難易度を調整する<t>DDA</t> (動的難易度調整)。Easy/Normal/Hard/VeryHard の固定 4 段と、腕前から自動で寄せる Adaptive を持つ。敵 HP・ダメージ・速度・プレイヤー HP の乗数を返すので、戦闘ロジックが pull して使う。",
      when: "下手なプレイヤーには優しく、上手いプレイヤーには手応えのある難易度に自動調整したい時。Adaptive 以外は固定値なので<t>リプレイ</t>同期にも使える。",
      sample: "FDynamicDifficulty dda;\ndda.Init(EDifficultyLevel::Adaptive);\n// イベント発生時:\ndda.RecordDeath(); dda.RecordRetry();\ndda.RecordKill();\ndda.RecordLevelComplete(/*time=*/45.0f);\n// 毎フレーム:\ndda.Tick(dt);\nf32 hp_mul  = dda.EnemyHealthMultiplier();\nf32 dmg_mul = dda.EnemyDamageMultiplier();",
      members: [
        { sig: "void Init(EDifficultyLevel base_level = EDifficultyLevel::Normal)", desc: "初期モードで初期化。Adaptive 指定時は Normal 相当からスタートして目標へ寄せていく。" },
        { sig: "void SetMode(EDifficultyLevel mode)", desc: "モード切替。固定モードは即その段の値にスナップ、Adaptive は現在値を保持して追従続行。" },
        { sig: "EDifficultyLevel CurrentMode()", desc: "現在のモードを返す。" },
        { sig: "void RecordDeath() / RecordKill() / RecordRetry() / RecordPowerupCollected()", desc: "ゲーム側がイベント駆動で呼ぶ統計記録。" },
        { sig: "void RecordLevelComplete(f32 time_taken)", desc: "レベルクリア時間を記録。指数移動平均で平均を更新し、リトライ数を 0 リセット。" },
        { sig: "f32 CurrentDifficulty()", ret: "[0,1] の連続難易度", desc: "0=最易、1=最難。Adaptive 中はゆっくり追従する現在値。" },
        { sig: "f32 EnemyHealthMultiplier()", ret: "敵 HP 倍率", desc: "Easy 0.5 / Normal 1.0 / Hard 1.5 / VeryHard 2.0。" },
        { sig: "f32 EnemyDamageMultiplier()", ret: "敵ダメージ倍率", desc: "Easy 0.6 / Normal 1.0 / Hard 1.4 / VeryHard 1.8。" },
        { sig: "f32 EnemySpeedMultiplier()", ret: "敵速度倍率", desc: "Easy 0.85 / Normal 1.0 / Hard 1.15 / VeryHard 1.3。" },
        { sig: "f32 PlayerHealthMultiplier()", ret: "プレイヤー HP 倍率", desc: "難易度と逆相関。Easy 1.5 / Normal 1.0 / Hard 0.85 / VeryHard 0.75。" },
        { sig: "const FPlayerSkillStats& Stats()", desc: "現在の統計を参照する。" },
        { sig: "void ResetStats()", desc: "統計のみ初期化 (モード・難易度値は維持)。新規ゲームやシーン切替向け。" },
        { sig: "void Tick(f32 dt)", desc: "Adaptive 時のみ腕前推定 → 目標 → なめらかな追従を進める。固定モードでは no-op。" }
      ]
    },
    {
      name: "EDifficultyLevel",
      kind: "列挙(enum)", header: "gameframework/DynamicDifficulty.h",
      summary: "難易度モード。Easy / Normal / Hard / VeryHard の固定 4 段と、自動調整の Adaptive。",
      when: "<code>FDynamicDifficulty::Init</code> / <code>SetMode</code> で難易度モードを指定する時。"
    },
    {
      name: "FPlayerSkillStats",
      kind: "構造体", header: "gameframework/DynamicDifficulty.h",
      summary: "腕前判定に使う実プレイ統計。死亡数・撃破数・現レベルのリトライ数・直近クリア時間の指数移動平均・パワーアップ取得数を持つ。",
      when: "<code>FDynamicDifficulty::Stats()</code> でデバッグ表示や難易度の根拠を見たい時。"
    },
    // =====================================================================
    // Easing (補間カーブ)
    // =====================================================================
    {
      name: "Easing (名前空間)",
      kind: "関数群 (名前空間)", header: "gameframework/Easing.h",
      summary: "<t>イージング</t>関数群 (acs::game::Easing)。従来の関数ポインタ互換を保ちつつ、標準 31 種 (Linear + 10 family の In/Out/InOut) と SmoothStep/SmootherStep を合わせた全 33 種を <code>EEasingType</code> catalog で扱える。直接関数は入力を [0,1] へ安全に正規化し、checked API は無効 type と非有限入力を診断する。",
      when: "アニメーションを「等速」ではなく「最初ゆっくり後で加速」「行き過ぎて戻る」のように自然に動かしたい時。<code>FTweenManager::Tween</code> と <code>FSequence::Tween</code> には関数ポインタ版と enum 版の両 overload がある。",
      sample: "using namespace acs::game;\nconst auto type = Easing::EEasingType::InOutCubic;\nf32 e = Easing::Evaluate(type, 0.5f);  // 型付き評価\ntweens.Tween(&player.x, 0.0f, 100.0f, 0.5f, type);\nsequence.Tween(&alpha, 0.0f, 1.0f, 0.25f, Easing::EEasingType::SmoothStep);\nEasing::EEasingType parsed = Easing::EEasingType::Linear;\nif (Easing::TryParseName(\"OutBounce\", parsed)) { /* 保存名を復元 */ }",
      members: [
        { sig: "f32 Linear(f32 t)", ret: "= t", desc: "等速 (補間なし)。" },
        { sig: "f32 InQuad/OutQuad/InOutQuad(f32 t)", desc: "2 乗カーブ。In=徐々に加速、Out=徐々に減速、InOut=両端なめらか。", when: "もっとも素直で自然な加減速。迷ったらこれ。" },
        { sig: "f32 InCubic/OutCubic/InOutCubic(f32 t)", desc: "3 乗カーブ。Quad より強めの加減速。" },
        { sig: "f32 InQuart/.../InQuint/...(f32 t)", desc: "4 乗 / 5 乗カーブ。さらに鋭い加減速。" },
        { sig: "f32 InSine/OutSine/InOutSine(f32 t)", desc: "正弦カーブ。やわらかい加減速。" },
        { sig: "f32 InExpo/OutExpo/InOutExpo(f32 t)", desc: "指数カーブ。非常に急な立ち上がり / 立ち下がり。" },
        { sig: "f32 InCirc/OutCirc/InOutCirc(f32 t)", desc: "円弧カーブ。" },
        { sig: "f32 InBack/OutBack/InOutBack(f32 t)", desc: "一度行き過ぎてから戻る (overshoot) 演出。一時的に [0,1] を外れる。", when: "ポップアップが少し弾んで出る等。" },
        { sig: "f32 InElastic/OutElastic/InOutElastic(f32 t)", desc: "ゴムのように振動しながら収束する。" },
        { sig: "f32 InBounce/OutBounce/InOutBounce(f32 t)", desc: "ボールが跳ねるような減衰バウンド。", when: "落下物の着地演出など。" },
        { sig: "f32 SmoothStep/SmootherStep(f32 t)", desc: "端点の微分を滑らかにする 3 次 / 5 次 smooth step。全 33 種の末尾 2 catalog 値。" },
        { sig: "enum class EEasingType : u8", desc: "Linear、10 family の In/Out/InOut、SmoothStep、SmootherStep という評価可能な 33 曲線を順序付きで列挙する型付き catalog。<code>Count = 33</code> は末尾 sentinel で、評価可能な曲線ではない。" },
        { sig: "FEasingResult TryEvaluate(EEasingType type, f32 t, f32& out)", desc: "checked 評価。無効 type、非有限 t、非有限な計算結果を拒否し、失敗時は out を変更しない。finite t は [0,1] に clamp。" },
        { sig: "f32 Evaluate(EEasingType type, f32 t, f32 fallback = 0)", desc: "型付き評価。checked 評価が失敗した場合は fallback を返す。" },
        { sig: "FEasingResult TrySampleCurve(EEasingType type, f32* out, usize sample_count)", desc: "[0,1] を 2〜65536 点で等間隔サンプリングする。先頭・末尾は厳密な端点。無効 type、点数範囲外、null出力、非有限結果を分類し、失敗時は配列全体を変更しない。確保なし・noexcept。" },
        { sig: "EasingFn GetFunction(EEasingType) / const char* GetName(EEasingType)", desc: "型付き catalog から従来の関数ポインタまたは canonical 名を取得。無効 type は nullptr / \"Invalid\"。" },
        { sig: "FEasingResult TryGetName(EEasingType type, const char*& out)", desc: "canonical 名の checked 取得。無効 type は <code>InvalidType</code> で拒否し、失敗時は out を変更しない。" },
        { sig: "bool TryParseName(const char* name, EEasingType& out)", desc: "canonical 名を enum に変換。null・未知名は false で、失敗時は out を変更しない。" },
        { sig: "FEasingResult TryParseNameChecked(const char* name, EEasingType& out)", desc: "canonical 名を大文字小文字を区別して解析する checked API。null は <code>NullName</code>、空・未知名は <code>UnknownName</code> で拒否し、失敗時は out を変更しない。" },
        { sig: "FTweenManager::Tween(..., EEasingType) / FSequence::Tween(..., EEasingType)", desc: "f32/FVec2/FVec3 の tween を catalog enum で指定する overload。無効 enum は安全に Linear へ fallback。" },
        { sig: "using EasingFn = f32 (*)(f32) noexcept", desc: "既存コードと互換の関数ポインタ型。直接関数は finite 入力を [0,1] に clamp し、NaN や無限大でも NaN/domain error を生成しない。" }
      ]
    },
    // =====================================================================
    // 経済 / 権利 (LiveOps)
    // =====================================================================
    {
      name: "FEconomyDirector",
      kind: "クラス", header: "gameframework/EconomyDirector.h",
      summary: "ゲーム内通貨の残高 + 固定価格のショップアイテム + 購入<t>コールバック</t>をまとめた小型マネージャ。通貨は soft (ゲーム内獲得) と premium (課金) を区別でき、商品は<t>コスメティック</t>専用フラグを持つ。<b>ガチャ (ランダム報酬) は意図的に非対応</b>、すべて「払えば必ず指定の品が手に入る」固定価格方式。",
      when: "見た目アイテムのショップやゲーム内通貨を実装したい時。実際の課金トランザクションは別 (ストア層) で行い、ここはゲーム内売買のみ扱う。",
      sample: "FEconomyDirector ed;\ned.RegisterCurrency({ \"gold\", \"Gold\", false });  // soft 通貨\ned.SetBalance(\"gold\", 1000);\ned.RegisterItem({ \"skin.knight_red\", \"Red Knight\", \"gold\", 500, ~0u, true });\nif (ed.PurchaseItem(\"skin.knight_red\")) {\n    // 成功 → 残高 500 減、コスメ解放\n}",
      members: [
        { sig: "void RegisterCurrency(const FCurrencyDef& def)", desc: "通貨を 1 種定義 (残高 0 で初期化)。同 id 重複は無視。" },
        { sig: "void SetBalance(const char* currency_id, u32 amount)", desc: "残高を絶対値で設定。未登録通貨は no-op。" },
        { sig: "u32 GetBalance(const char* currency_id)", ret: "残高", desc: "指定通貨の残高。未登録は 0。" },
        { sig: "bool AddToBalance(const char* currency_id, u32 delta)", desc: "残高を加算 (オーバーフロー時は max クランプ)。", when: "報酬付与やリアル課金後の残高反映で。" },
        { sig: "bool DeductFromBalance(const char* currency_id, u32 delta)", ret: "成功したか", desc: "残高を減算。不足なら false で残高据置。" },
        { sig: "void RegisterItem(const FShopItem& item)", desc: "商品を 1 件定義。同 item_id 重複は無視。通貨未登録でも登録は受理 (購入時に判定)。" },
        { sig: "bool PurchaseItem(const char* item_id)", ret: "購入成功したか", desc: "残高・在庫・通貨をチェックし、成功時のみ残高減算 + 在庫減 + コールバック発火。失敗時は副作用なし。" },
        { sig: "const FShopItem* FindItem(const char* item_id)", desc: "商品を 1 件取得 (未登録は nullptr)。" },
        { sig: "u32 ItemCount() / const FShopItem* AllItems(u32& out_count)", desc: "商品件数 / 全商品の生バッファ。" },
        { sig: "void SetOnPurchaseCallback(PurchaseCallback cb, void* user)", desc: "購入結果 (成功 / 失敗) の通知先を登録。失敗トースト UI などに使う。" },
        { sig: "void ClearAll()", desc: "通貨・残高・商品・在庫・コールバックを全クリア。" }
      ]
    },
    {
      name: "FCurrencyDef",
      kind: "構造体", header: "gameframework/EconomyDirector.h",
      summary: "通貨 1 種類の定義。id・表示名・premium フラグ (課金通貨か) を持つ。文字列は所有しない。",
      when: "<code>RegisterCurrency</code> に渡して通貨を定義する時。"
    },
    {
      name: "FShopItem",
      kind: "構造体", header: "gameframework/EconomyDirector.h",
      summary: "ショップに並ぶ商品 1 件。item_id・表示名・支払い通貨・価格・在庫数・コスメティック専用フラグを持つ。在庫は <code>~0u</code> で無制限、0 で売り切れ。",
      when: "<code>RegisterItem</code> に渡して商品を定義する時。"
    },
    {
      name: "FEntitlementRegistry",
      kind: "クラス", header: "gameframework/Entitlement.h",
      summary: "プレイヤーが DLC・シーズンパス・コスメパック・引換コードなどの<t>権利</t> (entitlement) を「持っているか」をゲーム側から問い合わせる窓口。ストアやプラットフォーム SDK には依存せず、取得結果を <code>Add()</code> で流し込んでもらう設計。",
      when: "「拡張マップを解放するか」「シーズンパス所持バッジを出すか」を権利の有無で判定したい時。",
      sample: "FEntitlementRegistry reg;\nreg.Add({ \"dlc.expansion_1\",  EEntitlementKind::Dlc,          true });\nreg.Add({ \"cosmetic.hat_red\", EEntitlementKind::CosmeticPack, true });\nif (reg.IsActive(\"dlc.expansion_1\")) UnlockExpansionMap();\nif (reg.HasAny(EEntitlementKind::CosmeticPack)) ShowCosmeticBadge();",
      members: [
        { sig: "void Add(FEntitlementInfo info)", desc: "権利を登録。id==nullptr は no-op。重複 dedup はストア側責務。" },
        { sig: "bool Has(const char* id)", ret: "登録済みか", desc: "active 不問で id があるか。" },
        { sig: "bool IsActive(const char* id)", ret: "有効な権利か", desc: "登録済みかつ active なら true。" },
        { sig: "bool HasAny(EEntitlementKind k)", ret: "種別に有効なものがあるか", desc: "指定種別の active な権利が 1 つでもあるか。", when: "「シーズンパス所持者向け UI を出すか」等の判定で。" },
        { sig: "void Clear()", desc: "全削除 (ストア再同期時に呼ぶ想定)。" },
        { sig: "u32 Count() / const FEntitlementInfo* AllInfos()", desc: "件数 / 生バッファ (デバッグ・列挙用)。" }
      ]
    },
    {
      name: "FEntitlementInfo",
      kind: "構造体", header: "gameframework/Entitlement.h",
      summary: "権利情報 1 つ。id・種別 (<code>EEntitlementKind</code>)・active フラグを持つ。id は所有しない。",
      when: "<code>FEntitlementRegistry::Add</code> に渡す権利を組み立てる時。"
    },
    {
      name: "EEntitlementKind",
      kind: "列挙(enum)", header: "gameframework/Entitlement.h",
      summary: "権利種別。Dlc / SeasonPass / BattlePass / CosmeticPack / GoodsRedeemCode の 5 種。検索キーや分類タグとして使う。",
      when: "<code>HasAny()</code> で種別ごとの所持判定をする時。"
    },
    // =====================================================================
    // 演出 / 画面エフェクト
    // =====================================================================
    {
      name: "FEffectSystem",
      kind: "クラス", header: "gameframework/EffectSystem.h",
      summary: "画面演出の中央指揮塔。Flash (全画面色被せ) / HitStop (一瞬の停止) / Camera Shake (画面揺れ) をまとめた純粋な<t>state holder</t>。自身は描画も停止もせず、描画側・ゲームループ・カメラが「pull する」だけのバスとして働く。",
      when: "被弾時の「白フラッシュ + 一瞬停止 + 画面揺れ」のような手応え演出をまとめて出したい時。",
      sample: "FEffectSystem fx;\n// 被弾時:\nfx.Flash({1,1,1}, 0.8f, 0.15f);  // 白フラッシュ 150ms\nfx.HitStop(0.08f);               // 80ms 停止\nfx.TriggerShake(0.5f);           // 画面揺れ trauma 0.5\n// 毎フレーム:\nconst f32 ts = fx.IsHitStop() ? 0.0f : 1.0f;  // 停止中は time_scale 0\nfx.Tick(dt);                     // fx は実時間で進む\nif (fx.PendingShakeTrauma() > 0) { cam.AddShake(fx.PendingShakeTrauma()); fx.ConsumeShake(); }",
      members: [
        { sig: "void Flash(FVec3 color, f32 intensity, f32 duration)", desc: "画面全体に色を被せて線形に減衰させる。再呼出で常に上書き (累積しない)。" },
        { sig: "void HitStop(f32 duration)", desc: "duration 秒の間 IsHitStop() を true に。重ねがけは長い方を採用。", when: "ヒット時の「タメ」を作る時。" },
        { sig: "void TriggerShake(f32 trauma, f32 duration_hint = 0.0f)", desc: "画面揺れの trauma 値を pending に積む。同フレーム複数呼出は最大値を保持。" },
        { sig: "void Tick(f32 dt)", desc: "実時間 dt で全エフェクトのタイマを進める (hit stop 中も止めない)。" },
        { sig: "f32 FlashIntensity() / FVec3 FlashColor()", desc: "描画側が overlay を被せるための現在の強度と色。" },
        { sig: "bool IsHitStop() / f32 HitStopRemain()", desc: "停止中か / 残り停止時間。ゲームループが time_scale=0 を選ぶのに使う。" },
        { sig: "f32 PendingShakeTrauma() / void ConsumeShake()", desc: "カメラが読む揺れ値 / 読み終えたら 0 に戻す。" }
      ]
    },
    {
      name: "FFadeTransition",
      kind: "クラス", header: "gameframework/FadeTransition.h",
      summary: "シーン切替時のフェード演出を司る<t>state holder</t>。描画はせず、現在の overlay の alpha・色・<t>フェーズ</t>だけを返す。フルスクリーン矩形をその色・alpha で塗れば、フェードイン / アウト / 暗転切替 / 軽いクロスフェードが成立する。",
      when: "シーン切替の暗転や、タイトル画面の明け演出を入れたい時。",
      sample: "FFadeTransition fade;\n// 暗黒から明けるフェードイン\nfade.StartFade(EFadeKind::FadeIn, 0.0f, 0.5f);\n// 毎フレーム:\nfade.Tick(dt);\nif (fade.IsMidPause()) Manager().ChangeScene&lt;FGameplayScene&gt;();  // 暗転中に切替\n// 描画:\nif (fade.IsActive()) {\n    FVec3 c = fade.OverlayColor(); f32 a = fade.OverlayAlpha();\n    batch.FillFullScreen(c.x, c.y, c.z, a);\n}",
      members: [
        { sig: "void StartFade(EFadeKind kind, f32 out_duration = 0.3f, f32 in_duration = 0.3f, f32 mid_pause = 0.0f)", desc: "フェードを開始。種別と各区間の秒数を指定する。" },
        { sig: "bool IsActive() / bool IsMidPause()", desc: "演出中か / 暗転待機中か。MidPause はシーン切替の想定タイミング (mid_pause=0 でも必ず 1 Tick は true)。" },
        { sig: "f32 OverlayAlpha() / FVec3 OverlayColor()", desc: "描画側が塗る overlay の不透明度 [0,1] と色 (既定は黒)。" },
        { sig: "void SetOverlayColor(FVec3 c)", desc: "overlay の色を変える (白フラッシュにするなど)。" },
        { sig: "EFadePhase CurrentPhase() / EFadeKind CurrentKind()", desc: "現在のフェーズと種別。" },
        { sig: "void Tick(f32 dt)", desc: "フェード進行を進める driver。" },
        { sig: "void Cancel()", desc: "直ちに Idle に戻す (alpha=0)。途中キャンセル用。" }
      ]
    },
    {
      name: "EFadeKind",
      kind: "列挙(enum)", header: "gameframework/FadeTransition.h",
      summary: "フェード種別。None / FadeIn (暗→明) / FadeOut (明→暗) / FadeInOut (暗転して切替) / CrossFade (軽量クロスフェード)。",
      when: "<code>FFadeTransition::StartFade</code> でどの演出を出すか選ぶ時。"
    },
    {
      name: "EFadePhase",
      kind: "列挙(enum)", header: "gameframework/FadeTransition.h",
      summary: "フェードの進行段階。Idle / FadingOut (暗くなる) / MidPause (暗転待機) / FadingIn (明るくなる)。",
      when: "<code>CurrentPhase()</code> を見て切替タイミングを判定する時。"
    },
    // =====================================================================
    // 2D エフェクトコンポーネント
    // =====================================================================
    {
      name: "AWater2DComponent",
      kind: "クラス (コンポーネント)", header: "gameframework/Effects2D.h",
      summary: "波打つ水面を描く<t>コンポーネント</t>。<t>シェーダ</t>もアセットも不要で、<code>FSpriteBatch</code> の三角形を時間で手続き生成して描く。横視点 (海/プール) と見下ろし (Core Keeper 風) の 2 スタイル、深さ勾配・泡・縁取り・平面反射・コースティクス・リップル (波紋) に対応。",
      when: "HLSL を書かずに海・川・池・水溜まりを置きたい時。ノードに <code>AddComponent</code> するだけで動く。プレイヤーが触れると波紋を立てる等のインタラクションも可能。",
      sample: "auto& w = node->AddComponent&lt;AWater2DComponent&gt;();\nw.SetRect({0, 3}, {16, 2});       // 形 (owner 相対、+Y=下なので水面は下)\nw.SetColor({0.1f, 0.35f, 0.6f});\nw.SetWaves(0.15f, 1.5f);          // 振幅・速度\nw.Disturb(mouse_world, 0.4f);     // 触れた点に波紋",
      members: [
        { sig: "void SetRect(FVec2 center, FVec2 half_size, u32 top_segments = 32)", desc: "矩形の水域 (海/プール)。上辺を分割して波打たせる。<code>SetArea</code> は旧名エイリアス。" },
        { sig: "void SetEllipse(FVec2 center, f32 rx, f32 ry, u32 segments = 28)", desc: "楕円の水域 (池・水溜まり)。" },
        { sig: "void SetRiver(const FVec2* path, u32 count, f32 width)", desc: "折れ線パスを幅 width の帯にして川にする。" },
        { sig: "void SetPolygon(const FVec2* pts, u32 count)", desc: "任意の単純多角形の水域。" },
        { sig: "void SetSplineRiver(...) / SetSplineRegion(...)", desc: "Catmull-Rom スプラインで滑らかな川 / 閉ループの湖を作る。" },
        { sig: "void SetColor(FVec3 rgb) / SetAlpha(f32 a) / SetWaves(f32 amplitude, f32 speed)", desc: "色 (深さ勾配も自動設定) / 不透明度 / 波の振幅と速度。" },
        { sig: "void SetFlow(FVec2 dir, f32 speed)", desc: "波と泡が流れる向きと速さ (川なら流れに沿わせる)。" },
        { sig: "void SetFoam(...) / EnableFoam(bool) / SetRim(...) / EnableRim(bool)", desc: "陸際の白泡 / 水際の縁取りの設定と ON/OFF。" },
        { sig: "void SetReflection(bool enable, FVec3 tint, f32 alpha)", desc: "平面反射で水面上のシーンを鏡像表示 (シーンの反射有効化が前提)。" },
        { sig: "void SetStyle(EWaterStyle s)", desc: "横視点 / 見下ろしを切り替える。" },
        { sig: "void EnableCaustics(bool) / SetCaustics(...) / EnableGlints(bool)", desc: "見下ろし水面の光の網目 (コースティクス) ときらめきの設定。" },
        { sig: "void Disturb(FVec2 world_point, f32 strength)", desc: "指定点から放射状の波紋を立てる。", when: "キャラや弾が着水した時。" },
        { sig: "bool ContainsPoint(FVec2 world) / f32 SurfaceY()", desc: "点が水域内か / 水面の world Y 座標。入水判定に。" }
      ]
    },
    {
      name: "AFire2DComponent",
      kind: "クラス (コンポーネント)", header: "gameframework/Effects2D.h",
      summary: "手続き生成の炎を描く<t>コンポーネント</t>。シェーダ・アセット不要で、ちらつきと揺らぎを時間で生成する。強さ (intensity) を動的に変えられる。",
      when: "たいまつ・かがり火・燃え盛る演出を簡単に置きたい時。近づくと勢いが増す等の連動も可能。",
      sample: "auto& f = node->AddComponent&lt;AFire2DComponent&gt;();\nf.SetSize(0.8f, 1.8f);\nf.SetIntensity(1.0f);   // 0 で消火、大きいほど激しく",
      members: [
        { sig: "void SetSize(f32 width, f32 height)", desc: "炎の幅と高さ。" },
        { sig: "void SetIntensity(f32 i)", desc: "炎の強さ (負値は 0 にクランプ)。", when: "風や燃料で勢いを変える演出。" },
        { sig: "f32 Intensity()", desc: "現在の強さを返す。" }
      ]
    },
    {
      name: "ATrail2DComponent",
      kind: "クラス (コンポーネント)", header: "gameframework/Effects2D.h",
      summary: "owner ノードを追従するモーショントレイル (残像) を描く<t>コンポーネント</t>。過去の位置を FIFO で保持し、細くなる帯として描く。",
      when: "高速移動するキャラ・弾・剣閃に残像を付けたい時。",
      sample: "auto& t = node->AddComponent&lt;ATrail2DComponent&gt;();\nt.SetColor({0.3f, 0.8f, 1.0f});\nt.SetWidth(0.3f);\nt.SetPoints(24);   // 残像の節の数 (2..48)",
      members: [
        { sig: "void SetColor(FVec3 rgb) / SetWidth(f32 w)", desc: "トレイルの色 / 太さ。" },
        { sig: "void SetPoints(u32 k)", desc: "残像を構成する点数 (2〜48 にクランプ)。多いほど長く滑らか。" }
      ]
    },
    {
      name: "AStencilClip2DComponent",
      kind: "クラス (コンポーネント)", header: "gameframework/Effects2D.h",
      summary: "任意形状の<t>ステンシルマスク</t>で子ツリーをクリップする<t>コンポーネント</t>。attach したノードの子を、指定形状の内側 (既定) か外側だけに切り抜いて描く。シェーダ不要で <code>FSpriteBatch</code> のステンシルモードを使う。",
      when: "窓越しに別のエフェクトを覗かせる、穴あきマスクを作る、円形ミニマップを切り抜く等。シーンで <code>SetStencilMaskEnabled(true)</code> が前提。",
      sample: "auto win = NewObject&lt;ANode&gt;();\nauto& clip = win->AddComponent&lt;AStencilClip2DComponent&gt;();\nclip.SetCircle({0, 0}, 2.5f);     // 円形の窓 (owner 相対)\nwin->AddChild(MakeWaterNode());   // 子 = 窓の中だけに見える\nroot.AddChild(Move(win));",
      members: [
        { sig: "void SetRect(...) / SetCircle(...) / SetEllipse(...) / SetPolygon(...)", desc: "マスク形状を矩形 / 円 / 楕円 / 単純多角形で指定 (owner 相対)。" },
        { sig: "void SetInvert(bool clip_outside)", desc: "内側ではなく外側だけを残す (穴あき)。", when: "ドーナツ状や、特定領域だけを隠したい時。" },
        { sig: "void SetRef(u8 ref)", desc: "ネスト用のステンシル参照値 (1..255)。入れ子マスクは別値にする。" },
        { sig: "void SetDebugVisualize(bool on, FVec4 color)", desc: "マスク形状自体を可視化する (デバッグ用)。" }
      ]
    },
    {
      name: "EWaterStyle",
      kind: "列挙(enum)", header: "gameframework/Effects2D.h",
      summary: "水の見せ方。SideView (横視点。上端が水面で波打ち、平面反射あり) / TopDown (見下ろし。縁が浅く中心が深い + コースティクス)。",
      when: "<code>AWater2DComponent::SetStyle</code> で視点に合わせて選ぶ時。"
    },
    // =====================================================================
    // ゲーム土台 / フロー
    // =====================================================================
    {
      name: "FGame",
      kind: "クラス", header: "gameframework/Game.h",
      summary: "ゲームアプリの基底クラス。<code>FApplication</code> を継承し、<code>FSceneManager</code> を駆動する。利用者は派生クラスで <code>InitialScene()</code> を override して最初のシーンを返すだけでよい。固定タイムステップ・time scale・シーン跨ぎの永続状態 (<t>AppState</t>)・フェード付きシーン遷移を内蔵する。",
      when: "ゲームの土台を作る時。<code>ACS_GAME_MAIN</code> マクロと組み合わせて起動する。",
      sample: "class FMyGame : public acs::game::FGame {\nprotected:\n    acs::TUniquePtr&lt;acs::game::FScene&gt; InitialScene() noexcept override {\n        return acs::MakeUnique&lt;FTitleScene&gt;();\n    }\n};\nACS_GAME_MAIN(FMyGame)",
      members: [
        { sig: "virtual TUniquePtr<FScene> InitialScene()", ret: "最初のシーン", desc: "派生クラスで必ず実装。起動時に最初に push されるシーンを返す。" },
        { sig: "FSceneManager& Scenes()", desc: "シーンスタック管理を取得 (push/pop/change)。" },
        { sig: "void SetTimeScale(f32 s) / f32 TimeScale()", desc: "時間スケール。FScene の dt に乗算される (0 でポーズ、2 で倍速)。" },
        { sig: "void SetFixedTimestep(f32 fixed_dt, u32 max_steps_per_frame = 8)", desc: "固定タイムステップ設定。step が 0 以下、または最大回数が 0 なら固定更新を無効化する。" },
        { sig: "bool TrySetFixedTimestep(const FFixedStepOptions& options)", ret: "適用できたか", desc: "step・最大回数・蓄積上限をまとめて検証し、成功時だけ設定する。" },
        { sig: "void DisableFixedTimestep() / bool IsFixedTimestepEnabled() const", desc: "固定更新を無効化する / 現在有効かを返す。" },
        { sig: "f64 FixedStepInterpolationAlpha() const", ret: "[0,1) の補間率", desc: "次の固定 step までの剰余率。無効時は 0。" },
        { sig: "void SetFixedStepInputSource(IInputFrameSource& source)", desc: "描画フレーム入力の取得元をAI・headless testへ差し替える。" },
        { sig: "void SetFixedTickInputSource(IFixedTickInputSource& source) / void ResetFixedStepInputSource()", desc: "固定tick入力の取得元をreplay・rollbackへ差し替える / 既定のplatform入力へ戻す。" },
        { sig: "bool TryCaptureFixedStepSnapshot(FFixedStepClockSnapshot& out) const / bool TryRestoreFixedStepSnapshot(const FFixedStepClockSnapshot& snapshot)", desc: "固定時計の設定・剰余・統計を検証付きで保存 / 復元する。" },
        { sig: "bool TryCaptureFixedStepRuntimeSnapshot(FFixedStepRuntimeSnapshot& out) const / bool TryRestoreFixedStepRuntimeSnapshot(const FFixedStepRuntimeSnapshot& snapshot)", desc: "固定時計、active scene の未消費入力、固定更新の有効状態を一括保存 / 復元する。" },
        { sig: "T& EmplaceAppState<T>(args...) / T* AppState<T>()", desc: "シーン跨ぎで残る永続状態を構築 / 取り出す (型消去、1 個)。未設定・型不一致は nullptr。", when: "プレイヤー所持金やセーブデータをシーン間で持ち回りたい時。" },
        { sig: "void TransitionTo(TUniquePtr<FScene> next, f32 out_sec = 0.3f, f32 in_sec = 0.3f)", desc: "フェードアウト → シーン切替 → フェードインを 1 行で行う。フェードは実時間で進む。" },
        { sig: "FFadeTransition& Fade()", desc: "進行中のフェード状態を参照する。" }
      ]
    },
    {
      name: "IInputFrameSource",
      kind: "インターフェース", header: "gameframework/InputFrameSource.h",
      summary: "FGame が一フレーム分の固定更新入力を所有 snapshot として取得する差し替え境界。",
      when: "platform入力の代わりにAI、headless test、描画frame単位の入力列を固定更新へ蓄積したい時。",
      members: [
        { sig: "bool TryCaptureFrameInput(FInputStateSnapshot& output)", desc: "現在フレームの入力を取得する。false では FGame が未消費入力を破棄する。" }
      ]
    },
    {
      name: "IFixedTickInputSource",
      kind: "インターフェース", header: "gameframework/FixedTickInputSource.h",
      summary: "FGameが各固定tickの直前に決定論入力を取得する差し替え境界。catch-upではtickごとに呼ばれる。",
      when: "固定tick単位のreplayやrollback入力で、描画frame rateに依存せずsimulationを駆動したい時。",
      members: [
        { sig: "bool TryCaptureFixedTickInput(u64 fixed_tick, FInputStateSnapshot& output)", desc: "指定tickの入力を取得する。時計復元後は同じtick番号が再要求され、falseのtickは無入力になる。" }
      ]
    },
    {
      name: "FFixedStepClock",
      kind: "クラス", header: "gameframework/FixedStepClock.h",
      summary: "可変の経過秒を上限付きの固定更新回数へ変換する値型。不正入力では状態を変更せず、剰余・破棄時間・補間率を明示的に返す。",
      when: "物理、lockstep、独立 simulation を一定刻みで進めたい時。通常のゲームでは FGame が所有する。",
      members: [
        { sig: "bool Configure(const FFixedStepOptions& options) / FFixedStepAdvanceResult Advance(f64 delta_seconds)", desc: "設定を検証し、経過秒から今回の固定 step 数を求める。" },
        { sig: "bool TryCaptureSnapshot(FFixedStepClockSnapshot& out) const / bool TryRestoreSnapshot(const FFixedStepClockSnapshot& snapshot)", desc: "設定・剰余・累積統計を保存 / 復元する。" },
        { sig: "void Reset() / f64 InterpolationAlpha() const", desc: "設定を維持して進行状態を初期化する / 描画補間率を返す。" }
      ]
    },
    {
      name: "FFixedStepInputBuffer",
      kind: "クラス", header: "gameframework/FixedStepInputBuffer.h",
      summary: "可変フレーム入力を固定 tick 入力へ変換する値型。短い押下・解放を次の tick まで保持し、catch-up の最初の tick だけへ渡す。",
      when: "独自ループで入力エッジの欠落と多重発火を防ぎたい時。FGame では同じ処理が自動配線される。",
      members: [
        { sig: "bool TryPushFrame(const IInputStateView& input) / bool TryConsumeFixedStep(FInputStateSnapshot& output)", desc: "フレーム入力を蓄積し、固定 tick 入力として一度だけ消費する。" },
        { sig: "bool TryCaptureSnapshot(FFixedStepInputBufferSnapshot& out) const / bool TryRestoreSnapshot(const FFixedStepInputBufferSnapshot& snapshot)", desc: "未消費入力を検証付きで保存 / 復元する。" }
      ]
    },
    {
      name: "FFixedStepRuntimeSnapshot",
      kind: "構造体", header: "gameframework/FixedStepRuntimeSnapshot.h",
      summary: "FGame の固定時計、active scene の未消費入力、固定更新の有効状態を同じ境界で所有する保存値。",
      when: "固定 tick の replay や rollback で、時計と短い入力エッジを transactional に復元したい時。"
    },
    {
      name: "ACS_GAME_MAIN",
      kind: "マクロ", header: "gameframework/EntryPoint.h",
      summary: "<t>FGame</t> 派生クラスから main エントリポイントを生成するマクロ。<code>app/EntryPoint.h</code> の <code>ACS_DEFINE_MAIN</code> を FGame 向けに薄くラップしたもの。",
      when: "ゲームクラスを定義した後、1 行書いて起動コードを生成する時。",
      sample: "class FMyGame : public acs::game::FGame { /* ... */ };\nACS_GAME_MAIN(FMyGame)   // これだけで main() が生成される"
    },
    {
      name: "FGameFlow",
      kind: "クラス", header: "gameframework/GameFlow.h",
      summary: "タイトル / メニュー / ゲームプレイ / ポーズ / ゲームオーバー等、ゲーム全体のフローを管理する高レベル<t>ステートマシン</t>。<code>FSceneManager</code> より 1 段上で「今ゲームのどの段階か」を表す。状態の出入りで<t>コールバック</t>を発火し、fade overlay の進捗も提供する。不正な遷移は遷移許可テーブルで弾く。",
      when: "シーン管理とは別に、ゲーム全体の論理状態 (タイトル中か、プレイ中か等) を 1 箇所で扱いたい時。状態切替時に BGM 切替やセーブ等の副作用を発火したい時。",
      sample: "FGameFlow flow;\nflow.SetOnEnterCallback(EFlowState::Gameplay, &OnGameplayEnter, this);\nflow.Init(EFlowState::Splash);\n// 毎フレーム:\nflow.Tick(dt);\nif (pressed_start && flow.CurrentState() == EFlowState::MainTitle)\n    flow.RequestTransition(EFlowState::MainMenu, 0.5f);\nif (flow.IsTransitioning()) DrawFadeOverlay(flow.FadeProgress());",
      members: [
        { sig: "void Init(EFlowState initial_state = EFlowState::Splash)", desc: "10 個の状態スロットと遷移許可テーブルを構築し、初期状態に入る (OnEnter 発火)。" },
        { sig: "EFlowState CurrentState() / EFlowState PendingState()", desc: "現在の状態 / 遷移先の状態。" },
        { sig: "bool IsTransitioning()", desc: "遷移 (fade) 中か。" },
        { sig: "bool CanTransitionTo(EFlowState to)", ret: "遷移可能か", desc: "遷移許可テーブルに照らした合法性。同一状態は false。" },
        { sig: "f32 FadeProgress()", ret: "[0,1] の overlay 不透明度", desc: "fade_out 中は 0→1、切替後の fade_in 中は 1→0。非遷移中は 0。" },
        { sig: "void RequestTransition(EFlowState to, f32 fade_sec = 0.3f)", desc: "状態遷移を要求。不正遷移や遷移中の追加要求は no-op (後勝ちしない)。fade_sec=0 で即時。" },
        { sig: "void SetOnEnterCallback(EFlowState state, StateCallback cb, void* user)", desc: "状態への入場コールバックを登録 (各状態 1 個)。OnExit 用の SetOnExitCallback もある。" },
        { sig: "void Tick(f32 dt)", desc: "fade タイマを進め、必要なら状態切替と enter/exit コールバックを発火する driver。" }
      ]
    },
    {
      name: "EFlowState",
      kind: "列挙(enum)", header: "gameframework/GameFlow.h",
      summary: "高レベルゲームフロー状態 (10 種固定)。Splash / MainTitle / MainMenu / Settings / Credits / Loading / Gameplay / PauseMenu / GameOver / ExitingGame。",
      when: "<code>FGameFlow</code> で現在のゲーム段階を識別・遷移指定する時。"
    },
    {
      name: "FFlowTransition",
      kind: "構造体", header: "gameframework/GameFlow.h",
      summary: "遷移メタデータ。遷移元 from・遷移先 to・fade-in / fade-out 秒数を持つ。<code>FGameFlow</code> が内部で保持し Tick で進行させる。",
      when: "遷移の進行情報を参照する場面 (主に内部用)。"
    },
    {
      name: "GameFramework.h (まとめ include)",
      kind: "ヘッダ", header: "gameframework/GameFramework.h",
      summary: "GameFramework の主要ヘッダを 1 行でまとめて取り込むための集約ヘッダ。FGame / FScene / Easing / Tween / 統一済みの ANode・AComponent / 各種コンポーネント・ディレクタ等を一括 include する。",
      when: "個別 include を書かずに gameframework をまとめて使い始めたい時。",
      sample: "#include \"gameframework/GameFramework.h\"\nusing namespace acs::game;"
    },
    // =====================================================================
    // 体力管理
    // =====================================================================
    {
      name: "FHealthSystem",
      kind: "クラス", header: "gameframework/HealthSystem.h",
      summary: "敵・プレイヤー・破壊可能オブジェクトの HP を一元管理する高レベル API。<code>FHealthId</code> (slot+世代の<t>ハンドル</t>) で entity を識別し、ダメージ適用・回復・蘇生・無敵時間付与を提供する。死亡は<t>コールバック</t>で通知。",
      when: "複数の entity の HP・死亡・無敵時間 (i-frame) をまとめて扱いたい時。属性ダメージは値をそのまま callback に伝えるだけで、倍率計算は呼び出し側の責務。",
      sample: "FHealthSystem hp;\nFHealthId player = hp.Spawn(100.0f);\nFHealthId enemy  = hp.Spawn(50.0f);\nif (hp.ApplyDamage(enemy, 30.0f, EDamageType::Fire)) {\n    // true = この一撃で致死\n}\nhp.SetInvulnerable(player, 0.5f);  // 被弾後の無敵 0.5 秒\nhp.Tick(dt);                        // 無敵タイマを進める",
      members: [
        { sig: "FHealthId Spawn(f32 max_hp)", ret: "新しいハンドル", desc: "full HP で entity を登録。max_hp<=0 は 1.0 にクランプ。" },
        { sig: "void Despawn(FHealthId id)", desc: "entity を破棄 (slot は世代を進めて再利用)。古い id は以後無効。" },
        { sig: "bool ApplyDamage(FHealthId id, f32 amount, EDamageType type = EDamageType::Physical)", ret: "致死したか", desc: "ダメージ適用。無敵中・死亡中・amount<=0 は no-op で false。致死時は死亡確定 + DeathCallback 発火。" },
        { sig: "void Heal(FHealthId id, f32 amount)", desc: "回復 (max_hp で clamp)。死亡中も HP は加算されるが復活はしない (Revive 専用)。" },
        { sig: "void SetInvulnerable(FHealthId id, f32 duration_sec)", desc: "一定秒間 無敵にする。重ねがけは長い方で延長。", when: "被弾直後の i-frame を付ける時。" },
        { sig: "void Revive(FHealthId id, f32 hp_fraction = 1.0f)", desc: "死亡状態から復活 (生存中は no-op)。hp_fraction で復活時 HP 割合を指定 (0 でも最低 1 HP)。" },
        { sig: "void SetMaxHp(FHealthId id, f32 new_max, bool refill)", desc: "最大 HP を変更。refill=true で現在 HP も満タンに。" },
        { sig: "const FHealthState* GetState(FHealthId id)", desc: "HP 状態を参照 (無効 id は nullptr)。" },
        { sig: "f32 GetCurrentHp(...) / f32 GetHpFraction(...) / bool IsAlive(...) / bool IsInvulnerable(...)", desc: "現在 HP / HP 割合 / 生存判定 / 無敵判定。HP バー表示などに。" },
        { sig: "u32 EntityCount() / u32 AliveCount()", desc: "全 entity 数 / 生存数。" },
        { sig: "void Tick(f32 dt)", desc: "毎フレーム呼んで無敵タイマを進める driver。" },
        { sig: "void SetOnDeathCallback(DeathCallback cb, void* user)", desc: "死亡時の通知先を登録 (state 確定後に呼ばれる。callback 内で Revive も可)。" },
        { sig: "void ClearAll()", desc: "全 entity を破棄 (コールバック設定は保持)。" }
      ]
    },
    {
      name: "FHealthId",
      kind: "構造体", header: "gameframework/HealthSystem.h",
      summary: "HP 管理対象 entity の識別子。32bit に 24bit index + 8bit 世代を詰めた<t>ハンドル</t>。0 は invalid 予約。Despawn → 再利用しても古いハンドルは世代不一致で弾かれる。",
      when: "<code>FHealthSystem</code> の各操作で entity を指す時。",
      sample: "FHealthId id = hp.Spawn(100.0f);\nif (id.IsValid() && hp.IsAlive(id)) { /* ... */ }",
      members: [
        { sig: "u32 Index() / u8 Generation() / bool IsValid()", desc: "詰め込まれた index / 世代 / 有効性 (m_Packed != 0)。" },
        { sig: "bool operator==/!=(FHealthId o)", desc: "ハンドル同士の等値比較。" }
      ]
    },
    {
      name: "FHealthState",
      kind: "構造体", header: "gameframework/HealthSystem.h",
      summary: "1 entity の HP 状態。現在 HP・最大 HP・生存フラグ・無敵フラグ・無敵残り時間を持つ。<code>GetState()</code> で const 参照が返る。",
      when: "HP バーや無敵中の点滅表示などで現在状態を読む時。"
    },
    {
      name: "EDamageType",
      kind: "列挙(enum)", header: "gameframework/HealthSystem.h",
      summary: "ダメージ属性。Physical / Fire / Ice / Lightning / Poison / True (耐性無効の確定ダメージ) の 6 種。本クラスは倍率を掛けず、種別を callback に伝えるだけ。",
      when: "<code>ApplyDamage</code> でダメージ種別を渡し、VFX 振り分けや将来の耐性計算に使う時。"
    }
  ]
});

// 新たに <t> で使った専門用語を glossary に追加 (既出語は再定義しない)。
Object.assign(ACS_REF.glossary, {
  "state holder": "描画や入力などの副作用を持たず、「今どういう状態か」だけを保持する型。実際の描画・音・入力は呼び出し側が状態を見て行う。テストしやすく結合が薄い。",
  "タイプライタ": "テキストを 1 文字ずつ順に表示していく演出。会話やノベルでよく使う。",
  "ローカライズ": "ゲームのテキストやリソースを言語・地域ごとに切り替えること (多言語化、i18n)。",
  "ロケール": "言語と地域の設定 (例: 日本語 / 英語)。どの翻訳を出すかの選択キー。",
  "BSP": "Binary Space Partitioning。空間を再帰的に 2 分割していく手法。ダンジョン生成では領域を割って各リーフに部屋を置く。",
  "シード": "乱数生成の初期値。同じシードからは同じ結果が再現できる。",
  "DDA": "Dynamic Difficulty Adjustment。プレイヤーの腕前に応じてゲーム難易度を自動で調整する仕組み。",
  "イージング": "アニメーションの進み方 (加速・減速・跳ね返り等) を表す補間カーブ。等速でない自然な動きを作る。",
  "リプレイ": "プレイ内容を記録して後で再生する機能。再現には決定論的 (同じ入力で同じ結果) であることが必要。",
  "コスメティック": "ゲームの性能に影響しない、見た目だけのアイテム (スキン・装飾)。pay-to-win を避ける指針。",
  "権利": "プレイヤーがコンテンツを「持っているか」を表す情報 (entitlement)。DLC・シーズンパス・引換コード等。",
  "ステンシルマスク": "描画範囲を任意形状に切り抜くための仕組み。マスク内側 (or 外側) だけに描画を限定できる。",
  "フェーズ": "処理の進行段階を表す状態区分 (例: フェードの FadingOut → MidPause → FadingIn)。",
  "コースティクス": "水面を通った光が水底に作る、揺らめく網目状の明暗模様。",
  "AppState": "シーンを跨いで残しておきたい永続状態 (所持金・セーブデータ等) を型消去で 1 個保持する仕組み。"
});
