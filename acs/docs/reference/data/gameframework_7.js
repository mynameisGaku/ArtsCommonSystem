/* ACS リファレンス — gameframework モジュール (7/N: Sequence〜Transform2D)。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "gameframework", order: 46, title: "gameframework — ゲーム部品 (acs::game)",
  blurb: "ゲームを作るときに繰り返し必要になる「部品」を集めた層です。スプライトアニメ・タイルマップ・状態機械・演出シーケンス・3D 音響・設定保存・実績/解析の窓口などを、<t>STL 不使用</t>・例外なし (<t>Result</t>) の ACS 規約で提供します。",
  types: [
    {
      name: "FSequence",
      kind: "クラス", header: "gameframework/Sequence.h",
      summary: "「待つ → 関数を呼ぶ → 値をなめらかに変える」を <b>1 行で連鎖記述</b>するビルダー。作った <code>FSequence</code> は <t>FSequenceRunner</t> に <t>ムーブ</t>で渡して再生する。",
      when: "カットシーン・出現ウェーブ・タイトルのロゴ演出など、時間に沿った一連の動きを台本のように書きたい時。",
      sample: "FSequence s;\ns.Wait(0.3f)\n .Tween(&logoColor, FVec3{0,0,0}, FVec3{1,1,1}, 0.5f, Easing::OutCubic)\n .Wait(1.0f)\n .Call(&FTitleScene::FadeOutBegin, this);\nrunner.Start(static_cast&lt;FSequence&amp;&amp;&gt;(s));",
      members: [
        { sig: "FSequence& Wait(f32 seconds)", desc: "指定秒だけ何もせず待つ動作を 1 つ足す。", when: "次の動きまで間を空けたい時。" },
        { sig: "FSequence& Call(void(*fn)(void* user) noexcept, void* user = nullptr)", desc: "その時点で関数を 1 度だけ呼ぶ。<code>std::function</code> でなく関数ポインタ + ユーザーデータ。", sample: "s.Call(&MyScene::OnDone, this);" },
        { sig: "FSequence& Tween(f32* target, f32 from, f32 to, f32 duration, Easing::EasingFn ease = Easing::Linear)", desc: "<code>f32</code> 変数を <code>from</code> から <code>to</code> へ <code>duration</code> 秒かけて補間する。<t>イージング</t>で緩急を付けられる。", when: "透明度・音量・座標 1 軸などを滑らかに変えたい時。" },
        { sig: "FSequence& Tween(FVec2* / FVec3* target, from, to, duration, ease)", desc: "2D / 3D ベクトルを補間する版。色や位置をまとめて動かせる。" },
        { sig: "FSequence& Loop(u32 count)", desc: "全体を何回繰り返すか。<code>0</code> で無限ループ、既定は <code>1</code> 回。" },
        { sig: "const TArray<FSeqAction>& Actions() const", desc: "積んだ動作リストを参照する (主に Runner 内部用)。" }
      ]
    },
    {
      name: "FSeqAction",
      kind: "構造体", header: "gameframework/Sequence.h",
      summary: "<t>FSequence</t> の中に積まれる「動作 1 つ」を表す <t>POD</t>。<code>EKind</code>(Wait / Call / TweenF / TweenV2 / TweenV3) で種類を区別する。",
      when: "通常は <code>FSequence</code> のビルダー経由で間接的に作られる。中身を直接覗いて独自処理したい時だけ触る。",
      sample: "// FSequence::Wait/Call/Tween が内部で 1 個ずつ生成する。\n// 直接組み立てることは普通ない。",
      members: [
        { sig: "enum class EKind : u8 { Wait, Call, TweenF, TweenV2, TweenV3 }", desc: "この動作が待ちか・呼び出しか・どの型の補間か。" },
        { sig: "f32 duration", desc: "待ち / 補間の所要秒 (Call は 0)。" }
      ]
    },
    {
      name: "FSeqHandle",
      kind: "構造体", header: "gameframework/Sequence.h",
      summary: "<t>FSequenceRunner</t> で再生中の 1 本のシーケンスを指す<t>ハンドル</t>。<code>index</code> と <code>generation</code> を持ち、使い回しスロットの取り違えを防ぐ。",
      when: "後から特定のシーケンスを止めたい (<code>Cancel</code>) / 生きているか確認したい (<code>IsActive</code>) 時に保持しておく。",
      sample: "FSeqHandle h = runner.Start(static_cast&lt;FSequence&amp;&amp;&gt;(s));\nif (h.IsValid()) { /* ... */ }\nrunner.Cancel(h);",
      members: [
        { sig: "bool IsValid() const", desc: "有効なハンドルか (<code>generation != 0</code>)。" }
      ]
    },
    {
      name: "FSequenceRunner",
      kind: "クラス", header: "gameframework/Sequence.h",
      summary: "複数の <t>FSequence</t> を同時に再生・管理する実行係。<code>Start</code> でシーケンスを所有して走らせ、毎フレーム <code>Tick(dt)</code> で進める。",
      when: "シーンが演出シーケンスを 1 本以上動かす時。シーンに 1 つ持たせて使うのが典型。",
      sample: "FSequenceRunner runner;\nFSeqHandle h = runner.Start(static_cast&lt;FSequence&amp;&amp;&gt;(s));\n// 毎フレーム\nrunner.Tick(dt);\n// シーン終了時\nrunner.CancelAll();",
      members: [
        { sig: "FSeqHandle Start(FSequence seq)", ret: "ハンドル", desc: "シーケンスの所有権を奪って再生開始。空シーケンスは無効ハンドルを返す。" },
        { sig: "void Cancel(FSeqHandle h)", desc: "進行中の 1 本を中止する (補間は最後に書いた値で停止)。" },
        { sig: "void CancelAll()", desc: "全シーケンスを破棄する。<code>FScene::OnExit</code> 等で呼ぶ。" },
        { sig: "bool IsActive(FSeqHandle h) const", desc: "そのハンドルのシーケンスがまだ動いているか。" },
        { sig: "u32 ActiveCount() const", ret: "稼働本数", desc: "今動いているシーケンスの数。" },
        { sig: "void Tick(f32 dt)", desc: "経過時間を流して全シーケンスを進める。毎フレーム呼ぶ。" }
      ]
    },
    {
      name: "FSettings",
      kind: "クラス", header: "gameframework/Settings.h",
      summary: "音量・解像度・キー設定など「ゲームをまたいで残したい設定値」を<b>型付き key-value</b>で持つ小さなストア。INI 風テキストで <code>Save</code>/<code>Load</code> できる。",
      when: "オプション画面の値を保存/復元したい時。<code>audio.master</code> のようなドット区切りのキーで管理する。",
      sample: "FSettings s;\ns.SetF32(\"audio.master\", 0.8f);\ns.SetBool(\"display.vsync\", true);\nf32 v = s.GetF32(\"audio.master\", 1.0f);\n(void)s.Save(L\"settings.ini\");",
      members: [
        { sig: "void SetF32/SetI32/SetBool(const char* key, v)", desc: "数値 / 真偽を書き込む。同名キーは上書き。<code>key == nullptr</code> は無視。" },
        { sig: "void SetString(const char* key, const char* v)", desc: "文字列を書き込む。値の寿命は呼び出し側が保証 (リテラル想定)。" },
        { sig: "f32 GetF32(const char* key, f32 default_value = 0.0f) const", ret: "値 or 既定", desc: "未設定 / 型不一致 / <code>nullptr</code> のときは既定値を返す (安全な fallback)。" },
        { sig: "i32 GetI32 / bool GetBool / const char* GetString(key, default)", desc: "それぞれの型で読み出す。未設定なら既定値。" },
        { sig: "bool Has(const char* key) const", desc: "キーが登録済みか (型は不問)。" },
        { sig: "void Remove(const char* key) / void Clear()", desc: "1 件削除 / 全削除。" },
        { sig: "u32 Count() const", ret: "件数", desc: "登録されている設定数。" },
        { sig: "TResult<void> Save(const wchar_t* file_path)", ret: "成否", desc: "INI 風テキストへ安全に書き出す (一時ファイル → rename で破損防止)。", when: "オプション確定時 / 終了時。" },
        { sig: "TResult<void> Load(const wchar_t* file_path)", ret: "成否", desc: "保存したファイルから型ごと復元する。" }
      ]
    },
    {
      name: "ESettingKind",
      kind: "列挙(enum)", header: "gameframework/Settings.h",
      summary: "<t>FSettings</t> の 1 件が保持する値の型タグ。<code>F32 / I32 / Bool / String</code> (と内部用 <code>None</code>)。",
      when: "通常は内部で使われる。Get 系が型一致を確認する際の基準。",
      sample: "// SetF32 した key を GetI32 すると kind 不一致で既定値が返る。"
    },
    {
      name: "FSocialModeration",
      kind: "クラス", header: "gameframework/SocialModeration.h",
      summary: "ローカルの<b>ブロックリスト</b>と<b>通報キュー</b>を管理する窓口。ブロックは即時ローカル反映、通報は失敗時にキューへ残して再送できる。実プラットフォーム SDK 送信は<t>シーム</t>として後付けする設計。",
      when: "オンライン要素のあるゲームで「このプレイヤーをブロック / 通報」を扱う時。",
      sample: "FSocialModeration mod;\nmod.Init();\nmod.BlockUser(\"steam:7656119800000099\");\nFReportRecord r{};\nr.reported_user_id = \"steam:7656119800000099\";\nr.category = EReportCategory::Harassment;\n(void)mod.SubmitReport(r);",
      members: [
        { sig: "void Init()", desc: "内部状態を初期化する (多重呼び出し可)。" },
        { sig: "void BlockUser(const char* user_id)", desc: "ユーザーをローカルブロックリストへ追加。重複 / <code>nullptr</code> は no-op。" },
        { sig: "void UnblockUser(const char* user_id)", desc: "ブロック解除。未登録は no-op。" },
        { sig: "bool IsBlocked(const char* user_id) const", ret: "ブロック済みか", desc: "招待などの前段ガードに使う。" },
        { sig: "u32 BlockedCount() const", ret: "件数", desc: "ブロック中の人数。" },
        { sig: "const FBlockEntry* AllBlocked(u32& out_count) const", ret: "生バッファ", desc: "ブロック一覧の先頭ポインタ。<code>out_count</code> に件数を返す。" },
        { sig: "TResult<void> SubmitReport(const FReportRecord& rep)", ret: "成否", desc: "通報を送信 (現状は pending queue に積む)。<code>reported_user_id == nullptr</code> は弾く。" },
        { sig: "u32 PendingReportCount() / u32 DeliveredReportCount() const", desc: "未送信件数 / 受理累計件数。" },
        { sig: "void SetBackendConnected(bool) / bool IsBackendConnected() const", desc: "送信先 backend の接続状態を切り替える / 確認する<t>シーム</t>。" },
        { sig: "TResult<void> FlushReports()", ret: "成否", desc: "未送信通報をまとめて再送。1 件でも残れば集約エラーを返す。", when: "オンライン復帰時など。" },
        { sig: "void ClearLocalState()", desc: "ブロックリストと通報キューを両方クリアする。" }
      ]
    },
    {
      name: "EReportCategory",
      kind: "列挙(enum)", header: "gameframework/SocialModeration.h",
      summary: "通報の種別。各プラットフォーム規約で共通的に求められる最小カテゴリを定義 (Harassment / HateSpeech / Cheating / Spam / InappropriateName / OtherToxicity)。",
      when: "通報 UI でユーザーに種別を選ばせる時。<code>FReportRecord.category</code> に入れる。",
      sample: "rep.category = EReportCategory::Cheating;"
    },
    {
      name: "FReportRecord",
      kind: "構造体", header: "gameframework/SocialModeration.h",
      summary: "通報 1 件分の情報。通報対象 / 通報者 / 種別 / 自由記述 / 時刻を持つ。文字列はすべて<b>非所有</b> (寿命は呼び出し側保証)。",
      when: "<code>FSocialModeration::SubmitReport</code> に渡す前に組み立てる。",
      sample: "FReportRecord r{};\nr.reported_user_id = targetId;\nr.reporter_user_id = myId;\nr.category = EReportCategory::Spam;\nr.note = \"広告連投\";\nr.timestamp = NowUnixSec();",
      members: [
        { sig: "const char* reported_user_id / reporter_user_id / note", desc: "通報対象 / 通報者 / 自由記述 (非所有)。" },
        { sig: "EReportCategory category", desc: "通報種別。" },
        { sig: "u64 timestamp", desc: "通報時刻 (呼び出し側基準の単調増加値)。" }
      ]
    },
    {
      name: "FBlockEntry",
      kind: "構造体", header: "gameframework/SocialModeration.h",
      summary: "ブロックリスト 1 件分。ブロック対象 ID とブロック時刻を持つ (<code>blocked_user_id</code> は非所有)。",
      when: "<code>AllBlocked</code> でブロック一覧を取り出して UI 表示する時。",
      sample: "u32 n; const FBlockEntry* list = mod.AllBlocked(n);\nfor (u32 i = 0; i &lt; n; ++i) { /* list[i].blocked_user_id */ }"
    },
    {
      name: "FSpatialAudio",
      kind: "クラス", header: "gameframework/SpatialAudio.h",
      summary: "3D 音源 (<t>FAudioSource3D</t>) と聞き手 1 つ (<t>FAudioListener</t>) から、<b>距離減衰</b>と<b>左右パン</b>を計算する音の空間化レイヤ。<t>HRTF</t> 本格化は <t>IHrtfRenderer</t> として差し替え可能。",
      when: "敵や環境音を「位置によって遠近・左右に聞こえる」ようにしたい時。各フレームに音量とパンを取り出してミキサに渡す。",
      sample: "FSpatialAudio sp;\nsp.SetListener({{0,0,0}, {0,0,1}, {0,1,0}});\nu32 src = sp.RegisterSource({5,0,3}, 20.0f, EAttenuationCurve::Linear);\nsp.UpdateSource(src, enemy.WorldPos());\nf32 vol = sp.ComputeAttenuatedVolume(src);\nf32 pan = sp.ComputePan(src);",
      members: [
        { sig: "void SetListener(const FAudioListener& l)", desc: "聞き手 (プレイヤーの耳) の位置と向きを設定する。" },
        { sig: "const FAudioListener& GetListener() const", desc: "現在の聞き手情報を返す。" },
        { sig: "u32 RegisterSource(FVec3 pos, f32 max_distance, EAttenuationCurve curve)", ret: "source_id", desc: "新しい音源を登録。1 始まりの ID を返す。<code>max_distance <= 0</code> は 20m に補正。" },
        { sig: "void UpdateSource(u32 id, FVec3 pos, FVec3 vel = FVec3::Zero())", desc: "音源の位置 / 速度を更新する。無効 ID は no-op。" },
        { sig: "void SetSourceVolume(u32 id, f32 v)", desc: "基準音量 [0,1] を変える (範囲外は clamp)。" },
        { sig: "void RemoveSource(u32 id)", desc: "音源を削除する (内部スロットは使い回さない)。" },
        { sig: "f32 ComputeAttenuatedVolume(u32 id) const", ret: "最終音量 [0,1]", desc: "距離とカーブから音量を算出。範囲外 / 無効は 0。", when: "毎フレーム取り出してミキサ音量に掛ける。" },
        { sig: "f32 ComputePan(u32 id) const", ret: "パン [-1,+1]", desc: "-1=完全左 / 0=正面・真後ろ / +1=完全右。" },
        { sig: "u32 SourceCount() const", ret: "稼働音源数", desc: "有効な音源の数。" },
        { sig: "void Tick(f32 dt) / void Clear()", desc: "毎フレームの更新 / 全音源を空にする (聞き手は保持)。" }
      ]
    },
    {
      name: "FAudioListener",
      kind: "構造体", header: "gameframework/SpatialAudio.h",
      summary: "聞き手 (プレイヤーの耳) の位置と向き。<code>position</code> / <code>forward</code> / <code>up</code> を持ち、<t>パン</t>の左右基準を決める。",
      when: "カメラやキャラの耳位置に同期させて <code>FSpatialAudio::SetListener</code> に渡す。",
      sample: "FAudioListener l;\nl.position = cam.pos;\nl.forward  = cam.forward;\nl.up       = cam.up;\nsp.SetListener(l);"
    },
    {
      name: "FAudioSource3D",
      kind: "構造体", header: "gameframework/SpatialAudio.h",
      summary: "3D 音源 1 個分。位置 / 速度 / 基準音量 / 最大距離 / 減衰カーブを持つ。<code>source_id</code> は <code>FSpatialAudio</code> が払い出す (0 = 無効)。",
      when: "通常は <code>RegisterSource</code> 経由で生成される。<t>IHrtfRenderer</t> に直接渡して処理する時に触る。",
      sample: "// RegisterSource が内部で 1 個生成する。\n// src.max_distance を超えると音量 0 (culling)。"
    },
    {
      name: "EAttenuationCurve",
      kind: "列挙(enum)", header: "gameframework/SpatialAudio.h",
      summary: "距離から音量への減衰カーブ種別。<code>Linear</code>(素直な線形) / <code>Inverse</code>(自然な 1/r) / <code>Exponential</code>(急に消える)。",
      when: "音源登録時にどう遠ざかると小さくなるかを選ぶ。足音は Exponential、被弾音は Linear など。",
      sample: "sp.RegisterSource(pos, 30.0f, EAttenuationCurve::Inverse);"
    },
    {
      name: "IHrtfRenderer",
      kind: "インターフェース", header: "gameframework/SpatialAudio.h",
      summary: "モノラル入力をステレオに空間化する処理の抽象<t>インターフェース</t> (<t>シーム</t>)。本格的な <t>HRTF</t> 畳み込みを後から差し替えられる。",
      when: "音の空間化処理を自前 / 別モジュールの実装に切り替えたい時にこの型で受け渡す。",
      sample: "FHrtfRendererStub stub;\n(void)stub.Init();\nstub.SetListener(listener);\nstub.ProcessSource(src, monoIn, stereoOut, n);",
      members: [
        { sig: "TResult<void> Init() / void Shutdown()", desc: "初期化 / 終了。" },
        { sig: "bool IsHrtfEnabled() const", ret: "本格HRTFか", desc: "真の HRTF 効果が走るか。stub は false (パン + 距離減衰のみ)。" },
        { sig: "void SetListener(const FAudioListener& listener)", desc: "聞き手状態を空間化レイヤへ伝える。" },
        { sig: "void ProcessSource(const FAudioSource3D& source, const f32* mono_input, f32* stereo_output, u32 sample_count)", desc: "モノラル <code>sample_count</code> サンプルを interleaved ステレオ (LRLR...) に変換する。" }
      ]
    },
    {
      name: "FHrtfRendererStub",
      kind: "クラス", header: "gameframework/SpatialAudio.h",
      summary: "<t>IHrtfRenderer</t> の唯一の既定実装。真の <t>HRTF</t> 畳み込みは無いが、<b>等パワーパン + 距離減衰は実数学</b>で行う (= ステレオ空間化は本物)。",
      when: "HRTF 専用データを用意せず、左右パンと遠近だけで十分な時の標準実装。",
      sample: "FHrtfRendererStub r;\n(void)r.Init();\n// IsHrtfEnabled() == false だが pan/減衰は本物",
      members: [
        { sig: "bool IsHrtfEnabled() const override", desc: "常に false を返す。" }
      ]
    },
    {
      name: "ASprite2DComponent",
      kind: "クラス", header: "gameframework/Sprite2DComponent.h",
      summary: "<t>ANode</t> に付ける<b>スプライト描画</b>コンポーネント。色付き矩形やテクスチャを描く。サイズはワールド単位で、位置 / 回転 / 拡縮は親の <t>FTransform2D</t> が供給する。",
      when: "2D のキャラ・背景・UI 部品など、1 枚の四角い絵を表示したい時の基本部品。",
      sample: "auto& spr = node->AddComponent<ASprite2DComponent>(FVec2{1, 1});\nspr.SetTexture(hero_tex);\nspr.SetTint(FVec4{1, 1, 1, 1});\nspr.SetUvRect(0, 0, 0.25f, 1.0f); // シートの 1 コマ",
      members: [
        { sig: "void SetTexture(IRhiTexture* tex) / IRhiTexture* Texture() const", desc: "描くテクスチャ (非所有) を設定 / 取得する。<code>nullptr</code> なら単色矩形。" },
        { sig: "void SetSize(FVec2 s) / FVec2 Size() const", desc: "ワールド単位の表示サイズ。" },
        { sig: "void SetTint(FVec4 c) / FVec4 Tint() const", desc: "乗算する色 (RGBA)。半透明や色付けに使う。" },
        { sig: "void SetPivot(FVec2 p) / FVec2 Pivot() const", desc: "回転・配置の基準点 (0..1、既定は中心 {0.5,0.5})。" },
        { sig: "void SetUvRect(f32 u0, f32 v0, f32 u1, f32 v1)", desc: "テクスチャ内の表示矩形を指定。スプライトシートの 1 コマを切り出す。", when: "アニメ用に <t>ASpriteAnimComponent</t> が毎フレーム書き換える。" },
        { sig: "FVec2 UvMin() const / FVec2 UvMax() const", desc: "現在の UV 矩形の左上 / 右下。" },
        { sig: "void OnDraw(FRenderContext& rc) override", desc: "描画処理。<code>FScene2D</code> の <t>FSpriteBatch</t> 経由で描く (利用者は通常呼ばない)。" }
      ]
    },
    {
      name: "ASpriteAnimComponent",
      kind: "クラス", header: "gameframework/SpriteAnimComponent.h",
      summary: "<t>FSpriteAnimator</t>(時間→フレーム) と <t>ASprite2DComponent</t>(UV 矩形) を橋渡しするコンポーネント。同じノードのスプライトの UV を毎フレーム書き換えて<b>スプライトシートアニメ</b>を再生する。",
      when: "歩行・攻撃などのコマ送りアニメをスプライトに付けたい時。グリッドシートか名前付きフレーム列から作れる。",
      sample: "auto& anim = node->AddComponent<ASpriteAnimComponent>();\nanim.InitGrid(/*cols=*/4, /*rows=*/1, /*frame_count=*/4, /*fps=*/8.0f);\nanim.Play();",
      members: [
        { sig: "void InitGrid(u32 cols, u32 rows, u32 frame_count, f32 fps, EPlayMode mode = EPlayMode::Loop)", desc: "テクスチャを格子に等分し、先頭から <code>frame_count</code> 枚を順に再生する。", when: "1 枚を等間隔セルに割ったよくあるシートの時。" },
        { sig: "void BeginFrames(f32 fps, EPlayMode mode) / void AddFrameUv(FVec4 uv) / void EndFrames()", desc: "任意の UV コマ列を積んでアニメ列を作る。<t>FSpritePack</t> の <code>ComputeUv</code> と組み合わせる。" },
        { sig: "void Play() / Pause() / Stop()", desc: "再生 / 一時停止 / 停止 (先頭へ)。" },
        { sig: "void SetFps(f32 fps)", desc: "再生速度を変える。" },
        { sig: "bool IsPlaying() / IsFinished() const", desc: "再生中か / Once モードで終わったか。" },
        { sig: "u32 CurrentFrame() const", ret: "現フレーム", desc: "今表示中のフレーム番号。" },
        { sig: "FSpriteAnimator& Animator()", ret: "下位アニメータ", desc: "フレームイベント登録など細かい制御をしたい時に直接触る。" }
      ]
    },
    {
      name: "FSpriteAnimator",
      kind: "クラス", header: "gameframework/SpriteAnimator.h",
      summary: "「経過時間から<b>現在フレーム番号</b>を計算する」ロジックだけを担う部品。画像 / 描画 API には触れず、利用者が <code>CurrentFrame()</code> を取り出して自分の絵に適用する。",
      when: "コマ送りの時間管理だけ欲しい時。描画と切り離してテストしやすい。指定フレームでイベント (足音など) を鳴らせる。",
      sample: "FSpriteAnimator anim;\nanim.Init(/*frame_count=*/8, /*fps=*/12.0f, EPlayMode::Loop);\nanim.AddFrameEvent(4, [](void* ud) noexcept {\n    static_cast<MyActor*>(ud)->OnFootstep();\n}, this);\nanim.Play();\nanim.Tick(dt);\nu32 idx = anim.CurrentFrame();",
      members: [
        { sig: "void Init(u32 frame_count, f32 fps, EPlayMode mode = EPlayMode::Loop)", desc: "フレーム数・速度・再生モードを設定。不正値は安全な既定 (frame=1, fps=1) に。" },
        { sig: "void Play() / Pause() / Stop()", desc: "現在位置から再開 / 位置維持で停止 / 先頭に戻して停止。" },
        { sig: "void Tick(f32 dt)", desc: "経過時間を流す。<code>dt <= 0</code> は no-op (巻き戻し非対応)。" },
        { sig: "u32 CurrentFrame() const", ret: "フレーム番号", desc: "今のフレーム。これを UV 切替に使う。" },
        { sig: "bool IsFinished() const", desc: "Once モードで末尾に達したか。" },
        { sig: "f32 NormalizedTime() const", ret: "進行率 [0,1]", desc: "周期内 (Loop/PingPong) または 0→1 (Once) の進み具合。" },
        { sig: "void SetCurrentFrame(u32 i) / void SetFps(f32 fps)", desc: "強制シーク / 速度変更。" },
        { sig: "void AddFrameEvent(u32 frame, FrameEventFn cb, void* user)", desc: "指定フレームに入った瞬間 1 度だけ関数を呼ぶ (Loop の周回毎に再発火)。", when: "足音・エフェクト発生など特定コマに同期させたい時。" },
        { sig: "u32 FrameCount() const / f32 Fps() const / EPlayMode Mode() const", desc: "設定済みの総フレーム数 / 再生速度 (fps) / 再生モードを取得する。" }
      ]
    },
    {
      name: "EPlayMode",
      kind: "列挙(enum)", header: "gameframework/SpriteAnimator.h",
      summary: "アニメの再生モード。<code>Loop</code>(循環) / <code>PingPong</code>(端で折り返し) / <code>Once</code>(末尾で停止)。",
      when: "アニメ初期化時にループのしかたを選ぶ。",
      sample: "anim.Init(6, 10.0f, EPlayMode::PingPong);"
    },
    {
      name: "FSpritePack",
      kind: "クラス", header: "gameframework/SpritePack.h",
      summary: "「1 枚の <t>アトラス</t>テクスチャ + その中に並ぶ複数フレームの矩形」を持つ<b>データ層</b>。名前でフレームを引き、[0,1] の UV を計算する。Aseprite / TexturePacker の JSON も読める。",
      when: "スプライトシートを名前付きフレームで管理したい時。<t>FSpriteAnimator</t> と組んでアニメ再生する。",
      sample: "FSpritePack pack;\nFSpritePackInfo info; info.atlas_texture_path = \"hero.png\";\ninfo.atlas_width = 1024; info.atlas_height = 1024;\npack.Init(info);\nif (const FSpriteFrame* f = pack.FindFrame(\"Idle_00\")) {\n    FVec4 uv = pack.ComputeUv(*f);\n}",
      members: [
        { sig: "void Init(const FSpritePackInfo& info)", desc: "アトラスのメタ情報 (パス / サイズ) を設定する。既存フレームは保持。" },
        { sig: "void AddFrame(const FSpriteFrame& frame)", desc: "フレーム矩形を 1 つ追加する。<code>name == nullptr</code> は無視。" },
        { sig: "TResult<void> LoadAtlasJson(const char* json_text, usize len)", ret: "成否", desc: "Aseprite / TexturePacker の atlas JSON を読み込む。名前 / 画像パスは内部所有。", when: "ツールが吐いた JSON からまとめて取り込む時。" },
        { sig: "const FSpriteFrame* FindFrame(const char* name) const", ret: "フレーム or null", desc: "名前で矩形を検索。見つからなければ <code>nullptr</code>。" },
        { sig: "bool HasFrame(const char* name) const", desc: "名前のフレームがあるか。" },
        { sig: "u32 FrameCount() const", ret: "フレーム数", desc: "登録済みフレーム数。" },
        { sig: "const FSpriteFrame* AllFrames(u32& out_count) const", ret: "生バッファ", desc: "全フレームの先頭ポインタ。<code>out_count</code> に件数。" },
        { sig: "void RemoveFrame(const char* name) / void ClearAll()", desc: "名前指定で削除 (全一致) / 全フレーム削除 (メタは保持)。" },
        { sig: "FVec4 ComputeUv(const FSpriteFrame& frame) const", ret: "{u0,v0,u1,v1}", desc: "矩形をアトラスサイズで割って [0,1] UV を返す。サイズ 0 のときは {0,0,0,0}。" }
      ]
    },
    {
      name: "FSpriteFrame",
      kind: "構造体", header: "gameframework/SpritePack.h",
      summary: "<t>アトラス</t>内の 1 矩形 = 1 フレーム。名前・ピクセル矩形 (x,y,w,h)・<t>ピボット</t> (0..1) を持つ。<code>name</code> は非所有 (リテラル想定)。",
      when: "<code>FSpritePack::AddFrame</code> で手動登録する時、または <code>FindFrame</code> の戻り値として受け取る時。",
      sample: "FSpriteFrame f;\nf.name = \"Run_00\";\nf.x = 0; f.y = 0; f.w = 64; f.h = 64;\nf.pivot_x = 0.5f; f.pivot_y = 1.0f; // 足元基準\npack.AddFrame(f);"
    },
    {
      name: "FSpritePackInfo",
      kind: "構造体", header: "gameframework/SpritePack.h",
      summary: "<t>アトラス</t>全体のメタ情報。テクスチャパスと全体サイズ (px) を持つ。テクスチャ本体は別モジュールが所有する。",
      when: "<code>FSpritePack::Init</code> に渡してアトラスの寸法を伝える。",
      sample: "FSpritePackInfo info;\ninfo.atlas_texture_path = \"assets/hero.png\";\ninfo.atlas_width = 1024;\ninfo.atlas_height = 1024;"
    },
    {
      name: "TStateMachine&lt;Owner&gt;",
      kind: "クラステンプレート", header: "gameframework/StateMachine.h",
      summary: "小さな汎用<b>有限状態機械</b> (FSM)。状態を <code>u32</code> ID で識別し、状態ごとに <code>on_enter</code>/<code>on_update</code>/<code>on_exit</code> を関数ポインタで登録する。<code>&lt;Owner&gt;</code> に所有者型を渡すと各関数が <code>Owner&amp;</code> を受け取る。",
      when: "敵 AI・ゲームフロー・アニメ制御など「状態が切り替わる」ロジックを整理したい時 (最大 16 状態)。",
      sample: "enum EEnemyState { Idle = 0, Chase };\nTStateMachine<FEnemy> sm;\nsm.Configure(Idle,  { &FEnemy::EnterIdle, &FEnemy::UpdateIdle, nullptr });\nsm.Configure(Chase, { nullptr, &FEnemy::UpdateChase, nullptr });\nsm.Start(Idle, *this);\n// 毎フレーム: sm.Update(*this, dt);",
      members: [
        { sig: "void Configure(u32 state_id, FState state)", desc: "状態 ID に enter/update/exit の組を登録する。範囲外は no-op。" },
        { sig: "void Start(u32 initial_state, Owner& owner)", desc: "初期状態に入る (その状態の <code>on_enter</code> を呼ぶ)。" },
        { sig: "void ChangeState(u32 new_state, Owner& owner)", desc: "<b>即時遷移</b>。旧の <code>on_exit</code> → 新の <code>on_enter</code> を同期で呼ぶ。", when: "<code>on_update</code> の中から条件に応じて呼ぶのが典型。" },
        { sig: "void Update(Owner& owner, f32 dt)", desc: "現在状態の <code>on_update</code> を発火する。毎フレーム呼ぶ。" },
        { sig: "u32 Current() const / bool IsIn(u32 state_id) const", desc: "現在の状態 ID / 指定状態にいるか。" }
      ]
    },
    {
      name: "ISteamworksBridge",
      kind: "インターフェース", header: "gameframework/SteamworksBridge.h",
      summary: "Steam / EOS / PS / Xbox / Switch などのプラットフォーム SDK へ橋渡しする抽象<t>シーム</t>。実績解除・リーダーボード・プレイヤー情報・クラウドセーブ・フレンド・ボイスなどを共通 API で扱い、実 SDK 結合はビルド時に差し替える。",
      when: "実績やランキングなどプラットフォーム機能を、SDK 非依存のゲームコードから呼びたい時。",
      sample: "ISteamworksBridge* social = &FSteamworksBridgeStub::GetStub();\n(void)social->Init();\n// ボス撃破時\n(void)social->UnlockAchievement(\"ACH_BOSS_01\");\n// 毎フレーム\nsocial->Tick(dt);",
      members: [
        { sig: "TResult<void> Init() / void Shutdown() / bool IsInitialized() const", desc: "SDK の初期化 / 終了 / 状態確認。" },
        { sig: "FPlayerIdentity GetLocalPlayer() const", ret: "ローカルプレイヤー", desc: "自分のプレイヤー情報。文字列の寿命は「次の Tick まで」。" },
        { sig: "TResult<void> UnlockAchievement(const char* ach_id)", ret: "成否", desc: "実績を解除する。<code>ach_id</code> はプラットフォームの実績 ID 文字列。" },
        { sig: "TResult<void> SetLeaderboardScore(const char* board_id, i64 score)", ret: "成否", desc: "リーダーボードへスコア送信。" },
        { sig: "TResult<i64> GetLeaderboardScore(const char* board_id)", ret: "スコア", desc: "自分の現在スコアを取得する。" },
        { sig: "void Tick(f32 dt)", desc: "コールバックポンプ (<code>SteamAPI_RunCallbacks</code> 相当)。毎フレーム必須。" },
        { sig: "TResult<void> SetStat / TResult<i64> GetStat / SetFloatStat / GetFloatStat", desc: "永続統計値 (kills, play_minutes 等) の読み書き。" },
        { sig: "bool IsDlcOwned(u32 app_id) const / TResult<void> SetRichPresence(key, value)", desc: "DLC 所有確認 / リッチプレゼンス設定。" },
        { sig: "u32 GetFriendCount() const / FPlayerIdentity GetFriendByIndex(u32 index) const", desc: "フレンド数 / index 番目のフレンド情報。" },
        { sig: "TResult<void> CloudWriteFile / TResult<u32> CloudReadFile / CloudFileExists / CloudDeleteFile / CloudGetQuota", desc: "クラウドセーブ (Steam Cloud 等) のファイル読み書き・存在確認・削除・容量。" },
        { sig: "u32 WorkshopGetSubscribedCount() / WorkshopGetSubscribedItem(...)", desc: "購読中の Workshop (UGC) アイテム数 / 各アイテム情報。" },
        { sig: "TResult<void> VoiceStartRecording / VoiceStopRecording / TResult<u32> VoiceGetCompressed", desc: "ボイスチャットの録音開始 / 停止 / 圧縮音声取得。" },
        { sig: "TResult<void> InputInit() / u32 InputGetControllerCount() const", desc: "Steam Input 初期化 / 接続コントローラ数。" }
      ]
    },
    {
      name: "FSteamworksBridgeStub",
      kind: "クラス", header: "gameframework/SteamworksBridge.h",
      summary: "<t>ISteamworksBridge</t> の依存ゼロ既定実装。<code>Init()</code> のみ成功し、実績 / ランキング系は <t>Result</t> でエラーを返す。<code>GetStub()</code> で <t>シングルトン</t>を取得できる。",
      when: "実 SDK を未統合のままビルド / テストしたい時。実装が差し込まれる前のデフォルト。",
      sample: "auto& social = FSteamworksBridgeStub::GetStub();\n(void)social.Init();",
      members: [
        { sig: "static FSteamworksBridgeStub& GetStub()", ret: "共有 stub", desc: "プロセス共有の static シングルトン。" }
      ]
    },
    {
      name: "FPlayerIdentity",
      kind: "構造体", header: "gameframework/SteamworksBridge.h",
      summary: "プラットフォーム横断の最小プレイヤー情報。<code>platform_id</code>(SDK 固有 ID 文字列) / <code>display_name</code> / <code>session_token</code> を持つ。文字列は非所有 (寿命は次の Tick まで)。",
      when: "<code>GetLocalPlayer</code> / <code>GetFriendByIndex</code> の戻り値として受け取る時。",
      sample: "FPlayerIdentity me = social->GetLocalPlayer();\n// me.display_name を UI 表示 (次 Tick 前にコピー)"
    },
    {
      name: "GetDefaultSteamworksBridge / SetSteamworksBridgeProvider",
      kind: "関数", header: "gameframework/SteamworksBridge.h",
      summary: "実 backend モジュールへの<b>結線点</b>。backend 側が provider を登録し、ゲームは <code>GetDefaultSteamworksBridge()</code> で backend 非依存に既定 Bridge を得る (未登録なら stub)。",
      when: "起動時に一度だけ実装を登録し、以降どこからでも既定 Bridge を取りたい時。",
      sample: "// 起動時 (backend リンク時のみ)\n// acs::steamworks::InstallSteamworksAsDefault();\nISteamworksBridge& b = GetDefaultSteamworksBridge();",
      members: [
        { sig: "void SetSteamworksBridgeProvider(SteamworksBridgeProvider provider)", desc: "既定 Bridge を返す関数を登録する。<code>nullptr</code> で stub に戻す (後勝ち)。" },
        { sig: "ISteamworksBridge& GetDefaultSteamworksBridge()", ret: "既定 Bridge", desc: "登録済みなら実 Bridge、未登録なら stub を返す。" }
      ]
    },
    {
      name: "FStreamingDirector",
      kind: "クラス", header: "gameframework/StreamingDirector.h",
      summary: "オープンワールド等でカメラ周辺の<b>チャンク</b>だけをロードし、範囲外をアンロードする<t>ストリーミング</t>管理係。各チャンク = 1 <t>FAssetBundle</t> を実ロードする (registry 未設定時は擬似ロードで動く)。",
      when: "全アセットを常駐できない広いマップで、視点に応じて読み込み / 解放を自動化したい時。",
      sample: "FStreamingDirector dir;\ndir.Init(/*chunk_size=*/100.0f, /*view_radius=*/2);\ndir.SetMaxConcurrentLoads(4);\n// 毎フレーム\ndir.SetViewerPos({ cam.x, cam.y });\ndir.Tick(dt);",
      members: [
        { sig: "void Init(f32 chunk_size = 100.0f, i32 view_radius_chunks = 2)", desc: "1 チャンクの大きさと保持半径を設定。半径 2 なら 5x5 = 25 チャンクを保つ。" },
        { sig: "void SetAssetRegistry(FAssetRegistry* registry) / FAssetRegistry* GetAssetRegistry() const", desc: "実アセットロード用 registry を差し込む / 取得する。<code>nullptr</code> なら擬似ロード。" },
        { sig: "void SetChunkPathFormat(const char* fmt)", desc: "チャンク (cx,cy) → アセットパスの printf 風書式。<code>%d</code> を 2 つこの順で含める。" },
        { sig: "void SetViewerPos(FVec2 pos)", desc: "カメラのワールド座標を更新する。" },
        { sig: "void SetMaxConcurrentLoads(u32 n)", desc: "同時ロード数の上限 (既定 4、0 は 1 に補正)。" },
        { sig: "void Tick(f32 dt)", desc: "範囲評価とロード / アンロードの状態遷移を 1 フレーム進める。", when: "毎フレーム <code>SetViewerPos</code> の後に呼ぶ。" },
        { sig: "EChunkState GetState(FChunkId id) const", ret: "状態", desc: "指定チャンクの現状態。未登録は <code>Unloaded</code>。" },
        { sig: "u32 LoadedCount() / u32 LoadingCount() const", desc: "ロード済み / ロード中のチャンク数。UI 表示などに。" },
        { sig: "void ForceUnload(FChunkId id) / void ClearAll()", desc: "1 チャンクを強制解放 / 全チャンク破棄 (シーン遷移時)。" }
      ]
    },
    {
      name: "FChunkId",
      kind: "構造体", header: "gameframework/StreamingDirector.h",
      summary: "ストリーミングチャンクの 2D 整数座標 <code>(cx, cy)</code>。同じ座標は常に同じチャンクを指す (generation なし)。比較は値ベース。",
      when: "<code>FStreamingDirector::GetState</code> / <code>ForceUnload</code> で特定チャンクを指す時。",
      sample: "FChunkId id{3, -1};\nif (dir.GetState(id) == EChunkState::Loaded) { /* ... */ }"
    },
    {
      name: "EChunkState",
      kind: "列挙(enum)", header: "gameframework/StreamingDirector.h",
      summary: "チャンクのライフサイクル状態。<code>Unloaded → Queued → Loading → Loaded → Unloading → Unloaded</code> と遷移する。",
      when: "<code>GetState</code> の戻り値を見てチャンクの読み込み具合を判断する時。",
      sample: "if (dir.GetState(id) == EChunkState::Loading) showSpinner();"
    },
    {
      name: "IAssetLockingBackend / IBuildFarmBackend",
      kind: "インターフェース", header: "gameframework/StudioWorkflow.h",
      summary: "チーム開発 / CI のための 2 つの抽象<t>シーム</t>。<t>IAssetLockingBackend</t>(アセット排他ロック: P4 / Plastic 等) と <t>IBuildFarmBackend</t>(ビルドファーム: Jenkins 等)。stub に加え<b>外部 SDK 不要のローカル本実装</b>も同梱する。",
      when: "エディタ / ツールから「アセットをロックして編集」「ビルドジョブ投入」を扱いたい時。",
      sample: "IAssetLockingBackend* locks = &GetLocalFileAssetLocking();\n(void)locks->LockAsset(\"art/hero.fbx\", \"designer_a\");\nIBuildFarmBackend* farm = &GetBuildFarmStub();",
      members: [
        { sig: "IAssetLockingBackend& GetAssetLockingStub()", ret: "stub", desc: "常に NotImplemented を返す依存ゼロのロック stub。" },
        { sig: "IBuildFarmBackend& GetBuildFarmStub()", ret: "stub", desc: "常に NotImplemented を返すビルドファーム stub。" },
        { sig: "FLocalFileAssetLocking& GetLocalFileAssetLocking()", ret: "実ロック", desc: "オンディスクの lock ファイルで本当に排他ロックする本実装。" },
        { sig: "FLocalBuildRunner& GetLocalBuildRunner()", ret: "実ランナー", desc: "<code>CreateProcessW</code> でローカルビルドを実行する本実装。" }
      ]
    },
    {
      name: "IAssetLockingBackend",
      kind: "インターフェース", header: "gameframework/StudioWorkflow.h",
      summary: "アセットの排他ロックを抽象化する<t>シーム</t>。「アセットパス 1 本につき 1 ロック」を前提に、ロック取得 / 解除 / 問い合わせを提供する。",
      when: "バイナリアセットを複数人で同時編集して壊さないよう、編集前にロックを取りたい時。",
      sample: "IAssetLockingBackend* locks = &GetLocalFileAssetLocking();\nauto r = locks->LockAsset(\"//depot/hero.fbx\", \"alice\");\nauto info = locks->QueryLock(\"//depot/hero.fbx\");",
      members: [
        { sig: "TResult<void> LockAsset(const char* asset_path, const char* user)", ret: "成否", desc: "アセットを <code>user</code> 名でロック。既にロック中なら <code>kSub_AlreadyLocked</code>。" },
        { sig: "TResult<void> UnlockAsset(const char* asset_path)", ret: "成否", desc: "ロック解除。未ロックなら <code>kSub_NotFound</code>。" },
        { sig: "TResult<FAssetLockInfo> QueryLock(const char* asset_path)", ret: "ロック情報", desc: "現在のロック状態を取得。未ロックなら <code>kSub_NotFound</code>。" },
        { sig: "bool IsConnected() const", desc: "backend に接続できているか。stub は常に false。" }
      ]
    },
    {
      name: "IBuildFarmBackend",
      kind: "インターフェース", header: "gameframework/StudioWorkflow.h",
      summary: "ビルドファームを抽象化する<t>シーム</t>。「ビルド投入 → ポーリングで完了確認 → artifact URL 受け取り」のフローを提供する。ビルド ID は不透明 <code>u64</code>。",
      when: "ナイトリービルドや CI ジョブをプログラムから投入・追跡したい時。",
      sample: "IBuildFarmBackend* farm = &GetLocalBuildRunner();\nIBuildFarmBackend::FBuildRequest req{\"Shipping_Win64\", \"main\", \"1e6c12b\"};\nauto id = farm->SubmitBuild(req);\n// あとで farm->PollBuild(id.Value())",
      members: [
        { sig: "TResult<u64> SubmitBuild(const FBuildRequest& req)", ret: "build_id", desc: "ビルドを投入。成功時は非ゼロのビルド ID を返す。" },
        { sig: "TResult<FBuildResult> PollBuild(u64 build_id)", ret: "結果", desc: "ビルド状態を取得。<code>build_id == 0</code> は不正、未知 ID は <code>kSub_NotFound</code>。" },
        { sig: "TResult<void> CancelBuild(u64 build_id)", ret: "成否", desc: "ビルドのキャンセル要求。" },
        { sig: "bool IsConnected() const", desc: "backend に接続できているか。stub は常に false。" }
      ]
    },
    {
      name: "FLocalFileAssetLocking",
      kind: "クラス", header: "gameframework/StudioWorkflow.h",
      summary: "<t>IAssetLockingBackend</t> の<b>ローカル本実装</b>。外部 SCM を使わず、<code>&lt;asset&gt;.lock</code> サイドカーファイルを <code>CreateFileW(CREATE_NEW)</code> の原子性で作って協調ロックする (stub ではなく実動作)。",
      when: "Perforce 等のサーバを立てずに、ローカル / 共有フォルダで簡易な排他ロックを使いたい時。",
      sample: "FLocalFileAssetLocking& locks = GetLocalFileAssetLocking();\n(void)locks.LockAsset(\"assets/hero.fbx\", \"alice\");\n// 自分のロックだけ厳格に外す\n(void)locks.UnlockAssetAs(\"assets/hero.fbx\", \"alice\");",
      members: [
        { sig: "TResult<void> UnlockAssetAs(const char* asset_path, const char* user)", ret: "成否", desc: "lock の owner が <code>user</code> と一致する時だけ解除する厳格版。不一致は <code>kSub_PermissionDenied</code>。" },
        { sig: "bool IsConnected() const override", desc: "ローカル FS は常に使えるので true。" }
      ]
    },
    {
      name: "FLocalBuildRunner",
      kind: "クラス", header: "gameframework/StudioWorkflow.h",
      summary: "<t>IBuildFarmBackend</t> の<b>ローカル本実装</b>。<code>CreateProcessW</code> で実際にビルドコマンドを起動して終了コードを回収する「サイズ 1 のビルドファーム」。同期実行ヘルパも持つ (stub ではなく実動作)。",
      when: "外部ファーム無しで、ローカルマシン上でビルドコマンドを実行・追跡したい時。",
      sample: "FLocalBuildRunner& runner = GetLocalBuildRunner();\nu32 ec = 0;\n(void)runner.RunBuild(L\"cmd /c exit 3\", ec); // ec == 3",
      members: [
        { sig: "TResult<void> RunBuild(const wchar_t* command_line, u32& out_exit_code, u32 timeout_ms = 0)", ret: "成否", desc: "コマンドを起動し完了まで待って終了コードを返す<b>同期</b>ヘルパ。<code>timeout_ms = 0</code> で無限待ち。" },
        { sig: "TResult<void> RunBuildUtf8(const char* command_line, u32& out_exit_code, u32 timeout_ms = 0)", ret: "成否", desc: "コマンドラインを UTF-8 で受ける版 (内部で UTF-16 変換)。" }
      ]
    },
    {
      name: "FTelemetryDirector",
      kind: "クラス", header: "gameframework/TelemetryDirector.h",
      summary: "「level_completed」「player_died」などの<b>解析イベント</b>を集めてサーバへまとめ送りする director。一定間隔または明示 <code>Flush()</code> で <t>IBackendClient</t> 経由送信。<t>GDPR</t> 同意が無ければ全 no-op。",
      when: "ゲームのプレイ状況を解析したい時。backend 未接続でもオフラインで貯めておける。",
      sample: "FTelemetryDirector td;\ntd.Init(&backend, &privacy);\ntd.TrackEvent(\"level_started\",\n              R\"({\\\"level\\\":3})\",\n              EEventPriority::Important, \"gameplay\");\ntd.Tick(dt); // 毎フレーム\ntd.Flush();",
      members: [
        { sig: "void Init(IBackendClient* backend, FPrivacyDirector* privacy = nullptr)", desc: "送信先 backend (必須) と同意管理 (任意) を注入する。" },
        { sig: "void Shutdown()", desc: "キューを空にし backend / privacy 参照を切る (送信はしない)。" },
        { sig: "void TrackEvent(const char* event_name, const char* json_payload = \"{}\", EEventPriority priority = EEventPriority::Info, const char* category = \"general\")", desc: "イベントを投入する。<code>nullptr</code> / 同意なし / 無効カテゴリは no-op。上限超過時は最古を捨てる。", when: "ゲーム進行の節目で呼ぶ。" },
        { sig: "void Flush()", desc: "貯まったイベントを backend へ送る。失敗分はキューに残し次回再送。" },
        { sig: "u32 PendingCount() / SentCount() / FailedCount() const", desc: "未送信 / 送信成功 / 送信失敗の累計件数。" },
        { sig: "void Tick(f32 dt)", desc: "時間を蓄積し間隔超過で自動 Flush する。毎フレーム呼ぶ。" },
        { sig: "void SetFlushInterval(f32 sec)", desc: "自動 Flush の間隔 (秒)。0 以下は 5 秒既定に補正。" },
        { sig: "void EnableCategory(const char* category, bool enabled)", desc: "カテゴリ単位で送信可否を切り替える (既定は未登録 = 有効)。" },
        { sig: "void Clear()", desc: "キュー / フィルタ / カウンタを初期化 (backend 参照は保持)。" }
      ]
    },
    {
      name: "EEventPriority",
      kind: "列挙(enum)", header: "gameframework/TelemetryDirector.h",
      summary: "解析イベントの重要度ヒント。<code>Debug</code> / <code>Info</code> / <code>Important</code>(KPI・収益) / <code>Critical</code>(障害監視)。キュー溢れ時の優先度や将来の sampling に使う。",
      when: "<code>TrackEvent</code> の第 3 引数に渡してイベントの重みを示す時。",
      sample: "td.TrackEvent(\"purchase_completed\", payload, EEventPriority::Important);"
    },
    {
      name: "FTelemetryEvent",
      kind: "構造体", header: "gameframework/TelemetryDirector.h",
      summary: "送信待ちキューに積まれる解析イベント 1 件の <t>POD</t>。イベント名 / カテゴリ / JSON ペイロード / 優先度 / タイムスタンプを持つ。文字列はすべて非所有。",
      when: "通常は <code>TrackEvent</code> が内部で生成する。中身を参照したい時に触る。",
      sample: "// TrackEvent が 1 件ずつ生成しキューへ積む。"
    },
    {
      name: "FTilemap",
      kind: "クラス", header: "gameframework/Tilemap.h",
      summary: "2D グリッドに <t>FTileId</t> を並べる<b>データ専用</b>のタイルマップ。レイヤー対応・tile↔world 座標変換・<code>Fill</code> / <code>FillRect</code> を提供。Tiled の JSON も読める。",
      when: "2D マップ (床 / 壁 / 背景) をグリッドで持ちたい時。描画は別途 <t>ATilemapComponent</t> 等で行う。",
      sample: "FTilemap map;\nmap.Init(/*w=*/64, /*h=*/48, /*layers=*/2, /*tile_size=*/16.0f);\nmap.Fill(FTileId{1}, 0);\nmap.SetTile(10, 5, FTileId{2}, 0);\nmap.FillRect(0, 0, 4, 4, FTileId{3}, 1);",
      members: [
        { sig: "void Init(u32 width, u32 height, u32 layer_count = 1, f32 tile_size = 16.0f)", desc: "グリッドとレイヤーを確保し全タイルを 0 (空) に。不正値は安全な既定に。" },
        { sig: "TResult<void> LoadTiledJson(const char* json_text, usize len)", ret: "成否", desc: "Tiled Map Editor の JSON を読み込む (tilelayer のみ対象)。", when: "ツールで作ったマップを取り込む時。" },
        { sig: "void SetTile(u32 x, u32 y, FTileId tile, u32 layer = 0)", desc: "個別タイルを設定。範囲外は no-op。" },
        { sig: "FTileId GetTile(u32 x, u32 y, u32 layer = 0) const", ret: "タイル ID", desc: "個別タイルを取得。範囲外は空 <code>FTileId{0}</code>。" },
        { sig: "void Fill(FTileId tile, u32 layer = 0)", desc: "指定レイヤー全体を 1 種で埋める。" },
        { sig: "void FillRect(u32 x0, u32 y0, u32 x1, u32 y1, FTileId tile, u32 layer = 0)", desc: "<b>閉区間</b>の矩形を塗る (境界 clamp)。" },
        { sig: "u32 Width() / Height() / LayerCount() / f32 TileSize() const", desc: "マップの寸法・レイヤー数・タイル辺長。" },
        { sig: "FVec2 TileToWorld(u32 x, u32 y) const", ret: "world 座標", desc: "tile (x,y) の<b>中心</b>ワールド座標 (+Y = 画面下)。" },
        { sig: "bool WorldToTile(FVec2 world, u32& out_x, u32& out_y) const", ret: "範囲内か", desc: "world → tile 変換。範囲外 / 負値は false。", when: "マウス座標がどのタイルかを当てる時。" },
        { sig: "const FTileId* LayerData(u32 layer) const", ret: "生バッファ", desc: "row-major (<code>y*width+x</code>) のタイル配列先頭。GPU upload / レンダラ用。範囲外 layer は <code>nullptr</code>。" }
      ]
    },
    {
      name: "FTileId",
      kind: "構造体", header: "gameframework/Tilemap.h",
      summary: "タイル 1 セルの ID (<code>u16</code>)。<code>0</code> を「空 (タイル無し)」として予約。描画側が <t>アトラス</t>の index や辞書キーとして解釈する。",
      when: "<code>FTilemap</code> のセルにタイル種別を入れる時。",
      sample: "FTileId wall{5};\nif (!map.GetTile(x, y, 0).IsEmpty()) { /* 何か置いてある */ }",
      members: [
        { sig: "bool IsEmpty() const", desc: "空セル (<code>value == 0</code>) か。" }
      ]
    },
    {
      name: "ATilemapComponent",
      kind: "クラス", header: "gameframework/TilemapComponent.h",
      summary: "<t>FTilemap</t> のデータを <t>FScene2D</t> 上に<b>描画</b>するコンポーネント。格子アトラステクスチャを持ち、非空タイルを対応セルの UV で <t>FSpriteBatch</t> に描く。当たり判定の生成もできる。",
      when: "タイルマップを実際に画面表示し、必要なら壁として物理ワールドに登録したい時。",
      sample: "auto& tm = node->AddComponent<ATilemapComponent>();\ntm.Map().Init(16, 12, 1, 1.0f);\ntm.Map().FillRect(0, 0, 15, 0, FTileId{1});\ntm.SetTexture(atlas_tex);\ntm.SetAtlasGrid(4, 4);\ntm.BuildCollision(world, 0, kWall);",
      members: [
        { sig: "FTilemap& Map() / const FTilemap& Map() const", ret: "タイルマップ", desc: "所有するタイルマップ。Init / Fill / SetTile はここ経由で行う。" },
        { sig: "void SetTexture(IRhiTexture* tex)", desc: "格子アトラステクスチャ (非所有) を設定する。" },
        { sig: "void SetAtlasGrid(u32 cols, u32 rows)", desc: "アトラス内のタイル格子の列数 / 行数。タイル ID v は cell index (v-1) に対応。" },
        { sig: "void SetTint(FVec4 tint)", desc: "全タイルに乗算する色。" },
        { sig: "void BuildCollision(FCollisionWorld2D& world, u32 layer, u32 collision_layer_bit)", desc: "指定レイヤーの非空タイルを 1 タイル = 1 AABB として物理ワールドに登録する (ソリッド化)。", when: "床や壁を当たり判定にしたい時。" },
        { sig: "void OnDraw(FRenderContext& rc) override", desc: "描画処理 (利用者は通常呼ばない)。" }
      ]
    },
    {
      name: "FTransform2D",
      kind: "構造体", header: "gameframework/Transform2D.h",
      summary: "2D ノードの<b>位置 / 回転 / スケール</b>を表す軽量な値型 (20 byte)。<code>FMat4</code> より小さく合成が速く、親 transform の回転 / スケールを取り出せる (分解可能)。",
      when: "2D ノードの座標系を扱う時。親子階層で <code>Compose</code> によりワールド transform を求める。",
      sample: "FTransform2D parent{{100, 50}, 0.0f, {2, 2}};\nFTransform2D local{{10, 0}, 0.0f, {1, 1}};\nFTransform2D world = parent.Compose(local);\nFMat4 m = world.ToMat4(); // 行列が要る描画で",
      members: [
        { sig: "FVec2 position / f32 rotation / FVec2 scale", desc: "位置・回転 (ラジアン)・スケール。" },
        { sig: "FTransform2D Compose(const FTransform2D& local) const", ret: "world transform", desc: "親の座標系に <code>local</code> を載せたワールド transform を返す。", when: "親子ノードのワールド座標を求める時。" },
        { sig: "FMat4 ToMat4() const", ret: "4x4 行列", desc: "<t>FSpriteBatch</t> 等で 4x4 行列が要るときの変換 (合成内では使わない)。" },
        { sig: "static FTransform2D Identity()", ret: "単位 transform", desc: "原点 / 無回転 / 等倍の単位 transform。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "イージング": "値を補間するときの緩急 (加速/減速) のカーブ。<code>Easing::OutCubic</code> 等で動きに自然さを与える。",
  "シーム": "実装を後から差し替えられるようにした抽象境界 (<t>インターフェース</t>)。SDK 非依存でビルドし、本番で実装を注入する。",
  "HRTF": "頭部伝達関数。耳の形状による音の変化を畳み込んで、ヘッドホンで方向感のある立体音響を作る技術。",
  "パン": "音を左右どちらのスピーカーへ振るか。-1=左 / 0=中央 / +1=右。",
  "ピボット": "スプライトの回転中心 / 配置基準となる点。0..1 のローカル座標で表す。",
  "POD": "Plain Old Data。コンストラクタ等を持たない単純なデータ構造体。コピーや memcpy が安全。",
  "ストリーミング": "必要な範囲のアセットだけを動的にロード/アンロードして、メモリに収まらない大規模世界を扱う仕組み。",
  "GDPR": "EU の一般データ保護規則。解析データ収集前にユーザー同意 (consent) を要する。同意が無ければ送信しない。",
  "IBackendClient": "解析イベント等をサーバへ送る通信層の<t>インターフェース</t>。HTTP/gRPC 等の具体実装を差し替える。",
  "FAssetBundle": "1 まとまりのアセット集合をロード/アンロードする単位。チャンクごとのアセットを束ねる。",
  "ANode": "2D/3D 共通シーングラフの唯一のノード。<code>AddComponent</code> で <t>ASprite2DComponent</t> 等の部品を付ける。",
  "ASprite2DComponent": "ノードにスプライト (1 枚絵) を描画する部品。",
  "FSpriteAnimator": "経過時間から現在のアニメフレーム番号を計算する部品。",
  "FSpritePack": "1 枚のアトラスと名前付きフレーム矩形を持つデータ層。",
  "FTilemap": "2D グリッドにタイル ID を並べるデータ専用のマップ。",
  "ATilemapComponent": "タイルマップを画面に描画するコンポーネント。",
  "FScene2D": "2D シーンの管理係。FSpriteBatch を FRenderContext に用意し、ノードを描画する。",
  "FSpriteBatch": "多数のスプライトをまとめて効率よく描画する仕組み。",
  "FAudioSource3D": "3D 空間内の音源 1 個 (位置 / 音量 / 減衰)。",
  "FAudioListener": "音を聞く側 (プレイヤーの耳) の位置と向き。",
  "IHrtfRenderer": "モノラルをステレオへ空間化する処理の抽象<t>インターフェース</t>。",
  "ISteamworksBridge": "プラットフォーム SDK (Steam 等) への抽象橋渡し<t>シーム</t>。",
  "IAssetLockingBackend": "アセット排他ロックの抽象<t>シーム</t> (Perforce 等)。",
  "IBuildFarmBackend": "ビルドファームの抽象<t>シーム</t> (Jenkins 等)。",
  "FTransform2D": "2D の位置 / 回転 / スケールを表す値型。"
});
