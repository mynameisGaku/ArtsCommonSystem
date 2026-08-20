/* ACS リファレンス — gameframework モジュール (パート 1: A〜C)。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "gameframework", order: 40,
  title: "gameframework — ゲーム部品 (acs::game)",
  blurb: "実際のゲームを組み立てるための<b>すぐ使える部品集</b>。カメラ・アニメ・音・AI・実績・バフ・アクセシビリティなど、ジャンルを問わず必要になる仕組みを <code>acs::game</code> 名前空間にまとめています。多くは「<t>状態</t>を保持して毎フレーム <code>Tick(dt)</code> で進める」共通スタイルです。",
  types: [
    {
      name: "FAccessibilityProfile",
      kind: "クラス", header: "gameframework/AccessibilityProfile.h",
      summary: "色覚補正・モーション低減・字幕・片手操作などの<b>アクセシビリティ設定を保持する箱</b>。値を持つだけで、実際の適用 (LUT 差し替え / 字幕表示など) はレンダラや UI 側が <code>Get()</code> で読んで行う。",
      when: "近代タイトル並みの配慮機能を 1 箇所で管理したい時。<code>CGame</code> に 1 個だけ持たせる想定 (非コピー・非ムーブ)。",
      members: [
        { sig: "void Set(const FAccessibilitySettings& s)", desc: "UI から受け取ったスナップショットで全フィールドを上書きする。" },
        { sig: "const FAccessibilitySettings& Get() const", ret: "現在の設定", desc: "今の設定値一式を const 参照で取り出す。レンダラ/UI が毎フレーム pull する。" },
        { sig: "void ApplyPreset(EPreset p)", desc: "プリセット (Dyslexia / ADHD / Autism / Aphasia / AAC / OneHanded など) を適用する。現値を破棄して上書き。", when: "「とりあえずこの特性向けの推奨設定」を一発で入れたい時。" },
        { sig: "void Reset()", desc: "全フィールドを既定値 (補正なし) に戻す。" },
        { sig: "void SetColorblindMode(EColorblindMode m) / void SetMotionReduction(EMotionReduction m) / void SetTextSize(ETextSize s)", desc: "色覚補正モード・モーション低減・文字サイズを個別に設定する。" },
        { sig: "void SetScreenShakeScale(f32 s) / void SetFlashIntensityScale(f32 s)", desc: "画面振動・フラッシュの強さの係数 (1.0=フル, 0.0=完全カット) を設定する。" },
        { sig: "bool AreSubtitlesEnabled() const / bool IsOneHandedMode() const ...", desc: "各機能が有効かを個別に問い合わせる getter 群。" }
      ]
    },
    {
      name: "FAccessibilitySettings",
      kind: "構造体", header: "gameframework/AccessibilityProfile.h",
      summary: "アクセシビリティ設定の<b>値オブジェクト</b> (<t>POD</t>)。色覚モード・文字サイズの enum と、字幕/スクリーンリーダ/片手モード等の bool、振動/フラッシュ係数をまとめて持つ。コピー可能。",
      when: "UI 画面の入力をスナップショットとして組み立て、<code>FAccessibilityProfile::Set()</code> に渡す時。"
    },
    {
      name: "EColorblindMode / EMotionReduction / ETextSize / EPreset",
      kind: "列挙(enum)", header: "gameframework/AccessibilityProfile.h",
      summary: "アクセシビリティ用の選択肢を表す enum 群。<b>EColorblindMode</b>=色覚補正 (None/Protanopia/Deuteranopia/Tritanopia/Achromatopsia)、<b>EMotionReduction</b>=動きの抑制 (Full/Reduced/Off)、<b>ETextSize</b>=文字サイズ (Small〜ExtraLarge)、<b>EPreset</b>=一括プリセット種別。",
      when: "個別 setter や <code>ApplyPreset()</code> に渡す値として使う。"
    },
    {
      name: "CAchievementManager",
      kind: "クラス", header: "gameframework/AchievementManager.h",
      summary: "実績 (アチーブメント) の<b>定義・進捗・解除</b>を一括管理し、解除時に Steamworks 等の<t>プラットフォーム SDK</t> へ伝える高レベルマネージャ。SDK 未接続ならローカル進捗だけ追うオフラインモードで動く。",
      when: "「ボス撃破」「累計 100km 歩いた」のような実績を扱いたい時。起動時に全実績を登録し、ゲーム中に進捗を流し込む。",
      members: [
        { sig: "void RegisterAchievement(const FAchievementDef& def)", desc: "実績を定義として登録する (起動時に 1 度ずつ)。同 id の 2 重登録は無視。", when: "ゲーム起動時に全実績をまとめて登録する。" },
        { sig: "void SetProgress(const char* id, u32 progress)", desc: "進捗を絶対値で設定。max に達したら自動で解除 + SDK 送信。" },
        { sig: "void IncrementProgress(const char* id, u32 delta = 1)", desc: "進捗を delta だけ加算する (上限でクランプ)。", when: "「歩いた距離」など積み上げ式の実績。" },
        { sig: "void Unlock(const char* id)", desc: "即時解除する (進捗を max に固定 + SDK 送信)。既に解除済みなら何もしない。" },
        { sig: "bool IsUnlocked(const char* id) const", ret: "解除済みか", desc: "指定実績がもう解除されているかを返す。" },
        { sig: "u32 TotalCount() const / u32 UnlockedCount() const", desc: "総数・解除済み数。「23/50 unlocked」表示用。" },
        { sig: "const FAchievementProgress* AllStates(u32& out_count) const", ret: "全状態の配列", desc: "全実績の状態を連続バッファで返す。一覧 UI で走査する。" },
        { sig: "void AttachSteamworks(ISteamworksBridge* bridge)", desc: "Storefront SDK を接続する。nullptr で切断 (= ローカルのみ追跡)。" },
        { sig: "void Tick(f32 dt)", desc: "将来の統計/タイマー更新用フック。現状は no-op。" },
        { sig: "using FAchievementManager = CAchievementManager", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CAchievementManager</code> を使う。" }
      ]
    },
    {
      name: "FAchievementDef / FAchievementProgress",
      kind: "構造体", header: "gameframework/AchievementManager.h",
      summary: "<b>FAchievementDef</b>=変化しない実績の定義 (id / 表示名 / 説明 / max_progress / secret)。<b>FAchievementProgress</b>=実行時に変わる状態 (現在進捗 / 解除済みフラグ / 解除時刻)。両者は同じ index で 1:1 対応する。",
      when: "Def は <code>RegisterAchievement()</code> に渡し、Progress は <code>GetState()</code> / <code>AllStates()</code> で読む。"
    },
    {
      name: "CAmbientDirector",
      kind: "クラス", header: "gameframework/AmbientDirector.h",
      summary: "1 日の<b>時刻 (0〜24h) に応じて空の色・環境光・太陽方向を補間する</b>時刻ドライバ (time-of-day)。レンダラは 3 つの色/ベクトルを毎フレーム pull するだけで朝昼夕夜の表情が出る。",
      when: "屋外シーンで時間経過の昼夜サイクルを出したい時。",
      members: [
        { sig: "void SetTimeOfDay(f32 hours)", desc: "現在時刻を [0,24) で設定する (範囲外は剰余で正規化)。" },
        { sig: "void AdvanceTime(f32 dt_hours)", desc: "ゲーム時間を dt_hours だけ進める (負値は 0 にクランプ)。" },
        { sig: "FVec3 SkyColor() const / FVec3 AmbientColor() const / FVec3 SunDirection() const", ret: "補間結果", desc: "現在時刻での空色・環境光色・太陽方向ベクトルを返す。" },
        { sig: "bool IsDay() const / bool IsNight() const", desc: "6:00〜18:00 を昼として昼夜を判定する。" },
        { sig: "void SetTimeScale(f32 game_hours_per_real_sec)", desc: "リアル 1 秒で進めるゲーム時間を設定する。既定 1/60 (= リアル 1 分でゲーム 1 時間)。" },
        { sig: "void Tick(f32 dt)", desc: "リアル秒 dt をタイムスケール換算してゲーム時間を進める。毎フレーム呼ぶ。" },
        { sig: "using FAmbientDirector = CAmbientDirector", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CAmbientDirector</code> を使う。" }
      ]
    },
    {
      name: "FAnimationCurve",
      kind: "クラス", header: "gameframework/AnimationCurve.h",
      summary: "編集できる「<b>時間 → 値</b>」の補間曲線。キーを打って、任意の <code>f32</code> 値を時間で滑らかに変化させる。Step / Linear / Hermite 補間と前後の折り返し (Clamp/Loop/PingPong) に対応。",
      when: "FOV やフェード、体力バー演出など、固定の <t>イージング</t>式では表せない自由なカーブが欲しい時。",
      members: [
        { sig: "void AddKey(f32 time, f32 value, ECurveInterpolation interp = ECurveInterpolation::Linear)", desc: "互換追加 API。新規コードでは失敗を診断できる <code>TryAddKey</code> を推奨。" },
        { sig: "void AddKeyHermite(f32 time, f32 value, f32 in_tangent, f32 out_tangent)", desc: "Hermite 用に接線付きでキーを追加する。接線は「1 秒あたりの傾き」。" },
        { sig: "void RemoveKey(u32 index) / void ClearKeys()", desc: "キーを削除する / 全消去する (範囲外は安全に無視)。" },
        { sig: "f32 Evaluate(f32 time) const", ret: "補間値", desc: "指定時刻の値を補間で求める。定義域外は前後の <code>FAnimationCurve::EWrapMode</code> に従う。", when: "毎フレーム、現在時刻の値を取り出す主役メソッド。" },
        { sig: "f32 Duration() const", ret: "末尾キーの time", desc: "最後のキーの時刻を返す。先頭キーとの差ではなく、末尾キーが持つ絶対時刻。" },
        { sig: "void SetPreWrap(EWrapMode m) / void SetPostWrap(EWrapMode m)", desc: "定義域の前/後に出たときの挙動 (Clamp/Loop/PingPong) を指定する互換 API。" },
        { sig: "FAnimationCurveResult TryAddKey(...) / TryAddKeyHermite(...)", desc: "入力と補間 enum を検証して key を追加する。失敗時は既存曲線を変更しない。" },
        { sig: "FAnimationCurveResult TrySetKeys(const FCurveKey*, u32, EWrapMode, EWrapMode)", desc: "sort 済み key 列と wrap mode を全検証して一括置換する。" },
        { sig: "FAnimationCurveResult TrySetEasingPreset(Easing::EEasingType, u32 sample_count = 65)", desc: "33 種の型付き easing を線形 key 列へ sampling する。" },
        { sig: "FAnimationCurveResult TryEvaluate(f32 time, f32&amp; out_value) const", desc: "非有限入力と補間 overflow を診断し、成功時だけ出力を更新する。" },
        { sig: "FAnimationCurveResult TrySetWrapModes(EWrapMode pre, EWrapMode post)", desc: "Clamp/Loop/PingPong を検証して同時に更新する。" },
        { sig: "u32 KeyCount() const / const FCurveKey* Key(u32 index) const", desc: "キー数 / index 番目のキーを取得する (範囲外は nullptr)。" }
      ]
    },
    {
      name: "FCurveKey",
      kind: "構造体", header: "gameframework/AnimationCurve.h",
      summary: "<t>FAnimationCurve</t> の 1 キーを表す値。time / value のほか、入口側・出口側それぞれの接線 (in_tangent / out_tangent) と補間方式 (in_interp / out_interp) を持つ。",
      when: "曲線の中身を直接覗いたり、Hermite 接線を細かく作り込みたい時。"
    },
    {
      name: "ECurveInterpolation",
      kind: "列挙(enum)", header: "gameframework/AnimationCurve.h",
      summary: "曲線セグメントの補間方式。<b>Step</b>=直前キーの値を保持 (階段)、<b>Linear</b>=線形、<b>Hermite</b>=接線を使う 3 次補間。",
      when: "<code>AddKey()</code> の第 3 引数として補間の種類を選ぶ時。"
    },
    {
      name: "FAnimationCurve::EWrapMode",
      kind: "列挙(enum)", header: "gameframework/AnimationCurve.h",
      summary: "定義域外の時間を処理する方式。<b>Clamp</b> は端値を保持、<b>Loop</b> は周期反復、<b>PingPong</b> は端で折り返す。",
      when: "<code>TrySetKeys</code> または <code>TrySetWrapModes</code> で曲線の前後を設定する時。"
    },
    {
      name: "CAnimationCurveArchive",
      kind: "クラス", header: "gameframework/AnimationCurveArchive.h",
      summary: "<t>FAnimationCurve</t> を固定幅 little-endian wire record として保存・復元する bounded codec。CRC、version、exact size、全 key と wrap mode を検証し、失敗時は出力 buffer と復元先曲線を変更しない。",
      when: "曲線をエディタデータや save file に永続化し、破損・旧 version・過大入力を明示的に診断したい時。",
      members: [
        { sig: "u64 EncodedSize(const FAnimationCurve&amp;) noexcept", desc: "正準 wire record の必要 byte 数を返す。" },
        { sig: "FAnimationCurveArchiveResult Encode(const FAnimationCurve&amp;, void*, u64 capacity, u64&amp; out_size) noexcept", desc: "全入力を検証後に caller buffer へ正準 encoding する。容量不足でも必要量を返し、buffer は変更しない。" },
        { sig: "FAnimationCurveArchiveResult Decode(const void*, u64 size, FAnimationCurve&amp;) noexcept", desc: "magic/version/size/CRC/key を全検証し、成功時だけ復元先を置換する。trailing data も拒否する。" },
        { sig: "FAnimationCurveArchiveResult SaveToFile(const wchar_t*, const FAnimationCurve&amp;) / LoadFromFile(const wchar_t*, FAnimationCurve&amp;)", desc: "<code>CSaveArchive</code> の検証済み atomic envelope を介して file 保存/復元する。" },
        { sig: "kWireVersion / kHeaderSize / kKeyRecordSize / kMaxEncodedSize", desc: "wire format と最大 65,536 keys に対応する公開境界。" },
        { sig: "using FAnimationCurveArchive = CAnimationCurveArchive", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CAnimationCurveArchive</code> を使う。" }
      ]
    },
    {
      name: "FAnimationCurveArchiveResult / EAnimationCurveArchiveError",
      kind: "構造体・列挙(enum)", header: "gameframework/AnimationCurveArchive.h",
      summary: "buffer 不足、magic/version/header/size/CRC 不正、key 過多、曲線データ不正、OOM、永続化失敗を安定 enum と詳細 field で返す。<code>curve_error</code> は曲線 semantic error、<code>required_size</code> は必要容量を保持する。"
    },
    {
      name: "CAnimationGraph",
      kind: "クラス", header: "gameframework/AnimationGraph.h",
      summary: "3D スケルトンアニメ向けの<b><t>ステートマシン</t>型ブレンドグラフ</b>。「どの clip を / いつから / どの blend 比率で再生するか」を制御する。実際のボーン計算や描画は呼出側に任せ、本クラスは clip index と blend alpha を提供する。",
      when: "Idle/Walk/Run/Jump などキャラの状態に応じてアニメを切り替え、状態間を滑らかにブレンドしたい時。",
      members: [
        { sig: "void Init() / void Shutdown()", desc: "全 state/clip/transition/param を初期化する / 解放する (多重呼び出し可)。" },
        { sig: "u32 AddClip(const FAnimationClipBinding& clip)", ret: "clip index", desc: "再生 clip のメタ情報を登録し、index を返す。" },
        { sig: "void AddStateNode(const FAnimationStateNode& node)", desc: "状態ノード (状態 ID + clip + blend 長) を追加する。同 ID は後勝ち。" },
        { sig: "void AddTransition(const FAnimationTransition& trans)", desc: "from→to の遷移ルールを追加する。条件は (param 名, しきい値)。" },
        { sig: "void SetParam(const char* name, f32 value)", desc: "遷移条件で参照される f32 パラメータ (speed 等) を設定する。" },
        { sig: "void TriggerTransition(EAnimationGraphState target_state)", desc: "条件を介さず明示的に遷移要求する (攻撃ボタン等を直接遷移に使う)。" },
        { sig: "EAnimationGraphState CurrentState() const / f32 CurrentBlendAlpha() const / u32 CurrentClipIndex() const", desc: "現在の状態・ブレンド比率 (0..1)・再生 clip index を取得する。描画側へ渡す。" },
        { sig: "void Tick(f32 dt)", desc: "clip 時刻とブレンドを進め、トリガー/遷移条件を評価する主処理。dt<=0 は無視。" },
        { sig: "void Reset()", desc: "実行時状態だけ初期化する (clip/state/transition 設定は維持)。" },
        { sig: "void SetOnStateEnterCallback(StateEnterCallback cb, void* user) / void SetOnClipEndCallback(ClipEndCallback cb, void* user)", desc: "状態遷移時 / clip 終端到達時に呼ぶ<t>コールバック</t>を登録する。" },
        { sig: "using FAnimationGraph = CAnimationGraph", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CAnimationGraph</code> を使う。" }
      ]
    },
    {
      name: "FAnimationClipBinding / FAnimationStateNode / FAnimationTransition",
      kind: "構造体", header: "gameframework/AnimationGraph.h",
      summary: "<t>CAnimationGraph</t> を組み立てる 3 つの部品。<b>FAnimationClipBinding</b>=clip メタ情報 (名前/長さ/ループ/速度)、<b>FAnimationStateNode</b>=1 状態 (ID/clip/入退場 blend 長)、<b>FAnimationTransition</b>=遷移ルール (from/to/条件 param/しきい値)。",
      when: "<code>AddClip</code> / <code>AddStateNode</code> / <code>AddTransition</code> にそれぞれ渡す。"
    },
    {
      name: "EAnimationGraphState",
      kind: "列挙(enum)", header: "gameframework/AnimationGraph.h",
      summary: "標準的なキャラ状態 ID の固定 enum (Idle/Walk/Run/Jump/Attack/Hit/Death/Custom1/Custom2 の 9 個)。Custom1/2 はゲーム固有状態の拡張枠。",
      when: "状態ノードや遷移の from/to を指定する時。"
    },
    {
      name: "FGraphHandle",
      kind: "構造体", header: "gameframework/AnimationGraph.h",
      summary: "将来の複数グラフ管理用の <t>ハンドル</t> (24bit index + 8bit 世代を packed)。現フェーズでは未使用だが API として公開されている。",
      when: "上半身/下半身を別レイヤで持つなど将来拡張時。今は意識しなくてよい。"
    },
    {
      name: "FAppStateSlot",
      kind: "クラス", header: "gameframework/AppState.h",
      summary: "シーンを跨いで生き残る<b>型消去の永続状態スロット</b>。任意の型 <code>T</code> を 1 つだけ格納し、どのシーンからも取り出せる。<t>RTTI</t> 不使用で、型不一致の取得は nullptr を返す。",
      when: "ハイスコアやプレイヤープロフィールなど、シーン遷移で消えてほしくないデータを 1 箇所に置きたい時 (<code>CGame</code> が 1 個保持する)。",
      members: [
        { sig: "T& Emplace<T>(Args&&... args)", ret: "構築した参照", desc: "既存を破棄して T をその場で構築し、参照を返す。", when: "起動時に永続状態を 1 度だけ作る。" },
        { sig: "T* Get<T>()", ret: "T* または nullptr", desc: "型 T として取り出す。未設定または型不一致なら nullptr。" },
        { sig: "void Reset()", desc: "保持中の値を破棄して空にする。" },
        { sig: "bool IsSet() const", desc: "値が入っているかを返す。" }
      ]
    },
    {
      name: "CAssetBundle",
      kind: "クラス", header: "gameframework/AssetBundle.h",
      summary: "シーンが使う<b>複数アセットをまとめて宣言・一括ロード</b>し、集約した進捗を見られる箱。個別の Future を散らかさず 1 つの bundle で扱える。シーン破棄で参照が drop し、決定的に解放される。",
      when: "ローディング画面付きで「このシーンに必要な N 個のアセット」をまとめて読み込みたい時。",
      members: [
        { sig: "void Add(const char* asset_path)", desc: "ロード対象のパスを追加する (実ロードはまだ走らない)。BeginLoad 後の Add は無視。" },
        { sig: "void BeginLoad(CAssetRegistry& registry)", desc: "登録済み全パスを registry 経由で実ロードする。多重呼び出しは無視。", when: "シーン入場時にロードを開始する。" },
        { sig: "TSharedPtr<Asset> GetAsset(u32 index) const / TSharedPtr<Asset> FindAsset(const char* path) const", ret: "ロード済みアセット", desc: "index 順 / パス一致でロード済みアセットを取り出す (未ロード/失敗は空)。" },
        { sig: "f32 Progress() const", ret: "0〜1", desc: "完了割合。プログレスバー表示用。" },
        { sig: "bool IsLoaded() const / bool HasFailed() const", desc: "全完了したか / 1 個でも失敗があるかを返す。" },
        { sig: "u32 AssetCount() const / u32 LoadedCount() const", desc: "登録総数 / 成功した数を返す。" },
        { sig: "void Unload()", desc: "全アセットを解放する。シーン破棄前に呼ぶと決定的タイミングで refcount を落とせる。" },
        { sig: "using FAssetBundle = CAssetBundle", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CAssetBundle</code> を使う。" }
      ]
    },
    {
      name: "IAssetPackReader / IAssetPackWriter",
      kind: "インターフェース", header: "gameframework/AssetPack.h",
      summary: "<code>.acpak</code> 暗号化アーカイブの<b>読み書き<t>シーム</t></b>。開発中はバラのファイル、出荷時は 1 つの不透明な pak、をゲームコードを変えずに切り替えられる。実体 (AES 暗号 + LZ4 圧縮) は別 backend モジュールが実装する。",
      when: "Reader=ランタイムで pak からファイルを読む時。Writer=パッキングツールで pak を作る時。",
      members: [
        { sig: "TResult<void> Mount(const char* pack_path)", desc: "[Reader] pak をマウントして以降の API を有効化する。" },
        { sig: "TResult<u64> FileSize(const char* name)", ret: "復号+解凍後のサイズ", desc: "[Reader] 仮想ファイルのオリジナルサイズを取得する (バッファ確保用)。" },
        { sig: "TResult<void> ReadFile(const char* name, u8* out_buffer, u64 buffer_size)", desc: "[Reader] 仮想ファイルを復号+解凍して out_buffer にコピーする。" },
        { sig: "TResult<u32> FileCount() / TResult<const char*> FileName(u32 index)", desc: "[Reader] 含まれるファイル数 / index 番目の仮想パスを取得する。" },
        { sig: "TResult<void> BeginPack(const char* output_path) / TResult<void> AddFile(...) / TResult<void> FinishPack()", desc: "[Writer] pak 作成を開始 → ファイル追加 → 確定の 3 段で書き出す。" }
      ]
    },
    {
      name: "FAssetPackInfo",
      kind: "構造体", header: "gameframework/AssetPack.h",
      summary: "現在マウント中の pak の最小情報 (ファイルパス / content hash / 暗号化フラグ / マウント中フラグ)。",
      when: "マウント済み pak の素性や改竄検知の hash を確認したい時。"
    },
    {
      name: "GetDefaultAssetPackReader / GetDefaultAssetPackWriter ほか provider 結線",
      kind: "関数", header: "gameframework/AssetPack.h",
      summary: "AssetPack 実装を backend 非依存に取得する仕組み。実 backend が <code>SetAssetPackReaderProvider()</code> で実装を登録し、ゲームコードは <code>GetDefaultAssetPackReader()</code> で取得する。未登録なら <t>スタブ</t>を返す。",
      when: "循環依存を避けつつ、実 <code>.acpak</code> backend があればそれを、無ければ no-op stub を、自動で使い分けたい時。",
      members: [
        { sig: "IAssetPackReader& GetReaderStub() / IAssetPackWriter& GetWriterStub()", desc: "依存ゼロの no-op stub (全 API が NotImplemented) を返す。" },
        { sig: "void SetAssetPackReaderProvider(AssetPackReaderProvider p) / void SetAssetPackWriterProvider(...)", desc: "既定実装を返す関数を登録する。nullptr で stub に戻す (後勝ち)。" },
        { sig: "IAssetPackReader& GetDefaultAssetPackReader() / IAssetPackWriter& GetDefaultAssetPackWriter()", desc: "provider 登録済みならその実装、未登録なら stub を返す。" }
      ]
    },
    {
      name: "CAssetPackReaderStub / CAssetPackWriterStub",
      kind: "クラス", header: "gameframework/AssetPack.h",
      summary: "<t>IAssetPackReader</t> / IAssetPackWriter の no-op スタブ実装。全 API が <code>NotImplemented</code> を返し、依存ゼロでコンパイルを通すための fallback。",
      when: "実 AssetPack backend を未統合のビルドやユニットテストで、ポインタ DI だけ通したい時。",
      members: [
        { sig: "using FAssetPackReaderStub = CAssetPackReaderStub", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CAssetPackReaderStub</code> を使う。" },
        { sig: "using FAssetPackWriterStub = CAssetPackWriterStub", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CAssetPackWriterStub</code> を使う。" }
      ]
    },
    {
      name: "CAudioDirector",
      kind: "クラス", header: "gameframework/AudioDirector.h",
      summary: "シーンを跨いで生きる<b>音声の指揮層</b>。Master/Bgm/Sfx の 3 段ボリューム、BGM クロスフェード、SFX 連射、<t>ダッキング</t>、Pause/Resume をまとめて扱う。<code>IAudioBackend</code> を差すと実音が鳴り、<code>CSpatialAudio</code> の距離減衰と左右パンを再生中 SFX voice へ反映できる。",
      when: "シーン切替で途切れない BGM や、位置に応じて遠近・左右が変わる SFX を 1 箇所で管理したい時 (<code>CGame</code> に持たせる)。",
      members: [
        { sig: "void SetMasterVolume(f32 v) / void SetBgmVolume(f32 v) / void SetSfxVolume(f32 v)", desc: "3 段のボリュームバスを [0,1] で設定する (範囲外は警告 + clamp)。" },
        { sig: "void PlayBgm(const char* name, f32 fade_in_sec = 1.0f, bool loop = true)", desc: "BGM をクロスフェードで再生する。同名は no-op、fade<=0 で即時切替。" },
        { sig: "void StopBgm(f32 fade_out_sec = 0.5f)", desc: "BGM をフェードアウトして停止する。" },
        { sig: "void PlaySfx(const char* name, f32 volume_scale = 1.0f)", desc: "SFX を即時 one-shot 再生する (最大 32 本、満杯なら最古を上書き)。" },
        { sig: "void Duck(f32 duration_sec, f32 depth)", desc: "短時間だけ BGM 音量を下げる (ボイス再生中など)。前後に短い線形 fade。" },
        { sig: "void Pause() / void Resume() / void StopAll()", desc: "全体の一時停止 / 再開 / 停止+リセット。" },
        { sig: "void Tick(f32 dt)", desc: "クロスフェード/ダッキングの内部タイマーを進める。毎フレーム呼ぶ (Pause 中は凍結)。" },
        { sig: "void SetBackend(IAudioBackend* backend)", desc: "実音再生 backend (CXAudio2Backend 等) の raw 非所有 pointer を差し込む。nullptr は結線だけを外し、既存 BGM/SFX voice は停止しない。backend は Director からの最終利用まで生存させる。破棄時は scene の voice を StopVoice、残りを StopAllVoices で停止してから SetBackend(nullptr) を呼び、その後に Shutdown / 破棄して dangling pointer を残さない。" },
        { sig: "void SetAssetRegistry(CAssetRegistry* registry)", desc: "name→clip 解決用の registry を差すと、name から実ロードして実音を鳴らせる。" },
        { sig: "FAudioVoiceHandle PlayBgmClip(const FAudioClipDesc&, f32 fade=1, bool loop=true) / FAudioVoiceHandle PlaySfxClip(const FAudioClipDesc&, f32 vol=1, f32 pitch=1)", desc: "raw PCM clip から直接 BGM/SFX を再生する。backend 未設定時は無効ハンドル。" },
        { sig: "void UpdateSpatialSfxVoice(FAudioVoiceHandle voice, const CSpatialAudio&amp; spatial, u32 source_id, f32 pitch = 1)", desc: "再生中 SFX へ Master×Sfx×source 音量×距離減衰と左右パンを 1 回の backend 更新で反映する。再生開始と同じ frame から毎 frame 呼ぶ。" },
        { sig: "f32 EffectiveBgmVolume() const / f32 EffectiveSfxVolume() const", desc: "master×バス×ダッキングを合成した実際の出力ボリュームを返す (デバッグ/backend 用)。" },
        { sig: "using FAudioDirector = CAudioDirector", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CAudioDirector</code> を使う。" }
      ]
    },
    {
      name: "IBackendClient",
      kind: "インターフェース", header: "gameframework/BackendClient.h",
      summary: "サーバ側 (テレメトリ送信・設定取得など) への<b>汎用バックエンド窓口の<t>シーム</t></b>。具体的な通信スタック (HTTP/gRPC/Steam) は抱えず interface だけ提供し、実装はプロジェクト側で差し込む。失敗は <t>Result</t> で返す。",
      when: "プレイ統計をサーバに送る等の通信が要る時。オフラインビルドでもリンクが通るよう抽象越しに使う。",
      members: [
        { sig: "TResult<void> Connect(const char* server_url)", desc: "指定 URL のサーバに接続する。完了/失敗を Result で返す。" },
        { sig: "void Disconnect()", desc: "接続を切る (多重/未接続呼び出しは安全に no-op)。" },
        { sig: "bool IsConnected() const", desc: "現在接続中かを返す。" },
        { sig: "TResult<void> SendTelemetry(const char* event_name, const char* json_payload)", desc: "テレメトリイベントを送信する (内部キュー + 非同期 flush は実装次第)。" },
        { sig: "void Tick(f32 dt)", desc: "非同期 RPC の応答をメインスレッドに引き上げる pump。毎フレーム呼ぶ。" }
      ]
    },
    {
      name: "IMatchmaker",
      kind: "インターフェース", header: "gameframework/BackendClient.h",
      summary: "対戦相手を探す<b>マッチメイキングの<t>シーム</t></b> (Glicko-2 / TrueSkill 等の rating ベースを想定)。StartSearch で検索開始 → PollStatus で状態確認 → AcceptMatch / CancelSearch で完了、という流れ。",
      when: "オンライン対戦のマッチ検索を実装したい時。実アルゴリズムはプロジェクト側実装に委ねる。",
      members: [
        { sig: "TResult<FMatchTicket> StartSearch(const char* mode, u32 elo_hint)", ret: "検索チケット", desc: "mode (\"ranked_1v1\" 等) と rating 推定値で検索を開始する。" },
        { sig: "TResult<void> CancelSearch(FMatchTicket t)", desc: "検索をキャンセルする (無効チケットは no-op)。" },
        { sig: "EMatchStatus PollStatus(FMatchTicket t)", ret: "検索状態", desc: "現在の検索状態 (Searching/Matched/Cancelled/Failed) を返す。副作用なし。" },
        { sig: "TResult<void> AcceptMatch(FMatchTicket t)", desc: "Matched 状態のチケットを確定する。失敗時は再検索に戻る設計を許容。" }
      ]
    },
    {
      name: "FMatchTicket / EMatchStatus / FBackendError",
      kind: "構造体・列挙(enum)", header: "gameframework/BackendClient.h",
      summary: "<b>FMatchTicket</b>=マッチ検索 1 件分の不透明ハンドル (u64、0 で無効)。<b>EMatchStatus</b>=検索状態 (Searching/Matched/Cancelled/Failed)。<b>FBackendError</b>=失敗サブコード (NotConnected / Timeout / NetworkFailure / NotImplemented 等)。",
      when: "<t>IMatchmaker</t> の検索状態管理、および backend 系のエラー判別に使う。"
    },
    {
      name: "GetDefaultBackendClient / GetDefaultMatchmaker ほか provider 結線",
      kind: "関数", header: "gameframework/BackendClient.h",
      summary: "backend 実装を backend 非依存に取得する仕組み。実 backend (TelemetryFile / CLocalMatchmaker 等) が provider を登録し、ゲームコードは <code>GetDefaultBackendClient()</code> / <code>GetDefaultMatchmaker()</code> で取得する。未登録なら<t>スタブ</t>を返す。",
      when: "通信実装の有無で挙動を切り替えつつ、ゲームコードを backend に依存させたくない時。",
      members: [
        { sig: "IBackendClient& GetBackendStub() / IMatchmaker& GetMatchmakerStub()", desc: "常に NotImplemented を返す stub への参照。リンクを通す fallback。" },
        { sig: "void SetBackendClientProvider(BackendClientProvider p) / void SetMatchmakerProvider(MatchmakerProvider p)", desc: "既定実装を返す関数を登録する。nullptr で stub に戻す (後勝ち)。" },
        { sig: "IBackendClient& GetDefaultBackendClient() / IMatchmaker& GetDefaultMatchmaker()", desc: "provider 登録済みならその実装、未登録なら stub を返す。" }
      ]
    },
    {
      name: "CBeatGrid",
      kind: "クラス", header: "gameframework/BeatGrid.h",
      summary: "リズムゲームの<b>タイミング判定 + 譜面再生</b>。1 譜面分の note 配列を持ち、再生時刻を進めながらプレイヤーの Tap を最近 note と突き合わせ、Perfect / Great / Good / Miss の 4 段階で判定する。コンボや精度も集計する。",
      when: "音ゲー (DDR/太鼓系) の判定ロジックを組みたい時。音の再生 (CAudioDirector) とは独立に動く軽量 state machine。",
      members: [
        { sig: "void Init()", desc: "統計/状態を初期化する (何度でも呼べる)。" },
        { sig: "EBeatChartLoadResult TryLoadChart(const FBeatNote* notes, u32 count, f32 bpm)", desc: "譜面・lane・有限 time/hold/BPM・上限を検証して transactional にコピーする。失敗時は既存譜面を保持する。" },
        { sig: "void LoadChart(const FBeatNote* notes, u32 count, f32 bpm)", desc: "<code>TryLoadChart</code> の互換 wrapper。失敗時は既存譜面を保持する。" },
        { sig: "bool TrySetTimingWindows(f32 perfect_ms, f32 great_ms, f32 good_ms)", desc: "有限・非負・上限内かつ perfect &lt;= great &lt;= good を満たす時だけ 3 判定窓を更新する。" },
        { sig: "void Start() / void Stop() / void Pause() / void Resume()", desc: "再生制御。Start で時刻 0 から開始、Stop で即停止+リセット。" },
        { sig: "EJudgement PressLane(EBeatLane lane) / Tap(EBeatLane lane)", ret: "head 判定", desc: "通常 note は即時確定し、hold note は active state に入る。<code>Tap</code> は互換 alias。" },
        { sig: "EJudgement ReleaseLane(EBeatLane lane)", ret: "tail/最終判定", desc: "active hold の tail を判定する。早すぎる release は Miss。head と tail の悪い方を最終判定として 1 回だけ集計する。" },
        { sig: "bool IsLaneHolding(EBeatLane lane) const / u32 ActiveHoldCount() const", desc: "lane または譜面全体の active hold 状態を問い合わせる。" },
        { sig: "void Tick(f32 dt)", desc: "有限・非負で overflow しない時だけ時間を進め、未入力 note と hold timeout を 1 回だけ確定する。" },
        { sig: "f32 Accuracy() const / u32 MaxCombo() const / u32 CurrentCombo() const", desc: "精度 (0〜1)・最大コンボ・現在コンボを取得する。" },
        { sig: "u32 TotalNotes() const / u32 HitNotes() const / u32 MissedNotes() const", desc: "総 note 数・ヒット数・ミス数を取得する。" },
        { sig: "void SetOnJudgeCallback(FJudgeCallback cb, void* user) / void SetOnEndCallback(FBeatEndCallback cb, void* user)", desc: "判定ごと / 譜面終了時に呼ぶ<t>コールバック</t>を登録する。" },
        { sig: "using FBeatGrid = CBeatGrid", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CBeatGrid</code> を使う。" }
      ]
    },
    {
      name: "FBeatNote / EBeatLane / EJudgement / EBeatChartLoadResult",
      kind: "構造体・列挙(enum)", header: "gameframework/BeatGrid.h",
      summary: "<b>FBeatNote</b>=譜面の 1 note (絶対秒 time_sec / lane / is_hold / hold_duration_sec)。<b>EBeatLane</b>=入力レーン (Left/Down/Up/Right + Custom1/2)。<b>EJudgement</b>=判定結果。<b>EBeatChartLoadResult</b>=null、上限超過、非有限値、不正 lane/BPM/hold、OOM を区別する checked load 結果。",
      when: "<t>CBeatGrid</t> に譜面を渡したり、判定結果を受け取ったりする時。"
    },
    {
      name: "CBehaviorTree",
      kind: "クラス", header: "gameframework/BehaviorTree.h",
      summary: "AI / 敵挙動 / 演出分岐のための最小構成の<b><t>ビヘイビアツリー</t></b>。root ノードを 1 つ持ち、毎フレーム <code>Tick(blackboard, dt)</code> で評価する。状態は型を持たない <code>void*</code> の <t>ブラックボード</t>に置く。",
      when: "「敵が見えたら追跡、見えなければパトロール」のような条件分岐 AI を組みたい時。",
      members: [
        { sig: "void SetRoot(TUniquePtr<ABtNode> root)", desc: "root ノードを差し替える (古い root は破棄)。nullptr で空にできる。" },
        { sig: "EBtStatus Tick(void* blackboard, f32 dt)", ret: "Running/Success/Failure", desc: "1 フレーム分ツリーを評価する。root 未設定なら Failure。", when: "毎フレーム呼ぶ AI の駆動点。" },
        { sig: "bool HasRoot() const", desc: "root が設定済みかを返す。" },
        { sig: "using FBehaviorTree = CBehaviorTree", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CBehaviorTree</code> を使う。" }
      ]
    },
    {
      name: "ABtNode / ABtSelector / ABtSequence / ABtAction",
      kind: "クラス", header: "gameframework/BehaviorTree.h",
      summary: "ビヘイビアツリーのノード群。<b>ABtNode</b>=抽象基底、<b>ABtSelector</b>=OR 合成 (どれか成功で成功)、<b>ABtSequence</b>=AND 合成 (全部成功で成功)、<b>ABtAction</b>=関数ポインタを呼ぶ末端 leaf。子は <t>TUniquePtr</t> で所有する。",
      when: "<code>MakeUnique</code> で作って <code>AddChild</code> でツリーを組み立てる時。",
      members: [
        { sig: "virtual EBtStatus Tick(void* blackboard, f32 dt)", desc: "[ABtNode] 1 フレーム分の評価。派生で実装する。" },
        { sig: "void AddChild(TUniquePtr<ABtNode> child)", desc: "[Selector/Sequence] 子の所有権を奪って末尾に追加する。nullptr は no-op。" },
        { sig: "explicit ABtAction(Fn fn)", desc: "[ABtAction] leaf 関数 (EBtStatus(*)(void*, f32)) を受け取る。fn が null なら常に Failure。" },
        { sig: "usize ChildCount() const", desc: "[Selector/Sequence] 子の数を返す。" },
        { sig: "using FBtAction = ABtAction", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ABtAction</code> を使う。" },
        { sig: "using FBtNode = ABtNode", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ABtNode</code> を使う。" },
        { sig: "using FBtSelector = ABtSelector", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ABtSelector</code> を使う。" },
        { sig: "using FBtSequence = ABtSequence", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ABtSequence</code> を使う。" }
      ]
    },
    {
      name: "EBtStatus",
      kind: "列挙(enum)", header: "gameframework/BehaviorTree.h",
      summary: "ビヘイビアツリーのノード Tick の戻り値。<b>Running</b>=実行中 (次フレーム継続)、<b>Success</b>=達成、<b>Failure</b>=失敗。合成ノードはこの値で次へ進むか決める。",
      when: "自作 leaf 関数の戻り値や、Tick 結果の判定に使う。"
    },
    {
      name: "CBuffSystem",
      kind: "クラス", header: "gameframework/BuffSystem.h",
      summary: "<b>状態異常・バフ・デバフの管理</b>マネージャ。複数の owner (キャラ) に時間制限付きの効果を載せ、毎フレーム進行させて tick (毒ダメージ等) / 期限切れを通知する。実際のステータス計算は呼出側が <code>AllBuffsOfOwner()</code> で列挙して行う。",
      when: "RPG/アクション/ローグライクで「攻撃力アップ」「毒」「シールド」などの効果を一元管理したい時。",
      members: [
        { sig: "void RegisterBuff(const FBuffDef& def)", desc: "バフ定義を登録する (起動時に一括)。同 id の 2 重登録は無視。" },
        { sig: "FBuffOwnerId CreateOwner()", ret: "owner ハンドル", desc: "効果を載せる対象 (キャラ) を発行する。空き slot を再利用する世代付きハンドル。" },
        { sig: "void DestroyOwner(FBuffOwnerId owner)", desc: "owner を破棄して全 buff をクリアする (ExpireCallback は発火しない)。" },
        { sig: "bool ApplyBuff(FBuffOwnerId owner, const char* buff_id)", ret: "適用できたか", desc: "owner にバフを適用する。既存への挙動は定義の StackPolicy (Refresh/Stack/Ignore) に従う。" },
        { sig: "bool RemoveBuff(FBuffOwnerId owner, const char* buff_id)", desc: "owner から指定バフを完全に外す (ExpireCallback を 1 回発火)。" },
        { sig: "bool HasBuff(...) const / u32 GetStack(...) const / f32 GetRemaining(...) const", desc: "バフの有無 / 重ねがけ数 / 残り秒を問い合わせる。" },
        { sig: "const FBuffInstance* AllBuffsOfOwner(FBuffOwnerId owner, u32& out_count) const", ret: "buff の配列", desc: "owner の全バフを生バッファで返す。毎フレーム列挙して効果倍率を計算する用途。", when: "AttackUp などの倍率を反映したい時、毎フレーム読む。" },
        { sig: "void SetOnTickCallback(TickCallback cb, void* user) / void SetOnExpireCallback(ExpireCallback cb, void* user)", desc: "tick (周期効果) / 期限切れ時に呼ぶ<t>コールバック</t>を登録する。" },
        { sig: "void Tick(f32 dt)", desc: "全 owner の全バフを dt 進める。周期 tick 発火 + 期限切れ除去をまとめて行う。dt<=0 は no-op。" },
        { sig: "void ClearAll()", desc: "全 owner・全 buff・registry・コールバックを破棄する。" },
        { sig: "using FBuffSystem = CBuffSystem", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CBuffSystem</code> を使う。" }
      ]
    },
    {
      name: "FBuffDef / FBuffInstance / FBuffOwnerId",
      kind: "構造体", header: "gameframework/BuffSystem.h",
      summary: "<b>FBuffDef</b>=バフ 1 種の定義 (id/kind/持続秒/tick 間隔/効果量/StackPolicy/最大 stack/デバフフラグ)。<b>FBuffInstance</b>=今 owner に掛かっている実体 (残り秒 / 重ねがけ数)。<b>FBuffOwnerId</b>=効果対象の<t>世代付きハンドル</t> (24bit index + 8bit gen)。",
      when: "<code>RegisterBuff</code> に Def を渡し、<code>AllBuffsOfOwner</code> で Instance を読む。owner は CreateOwner が返す。"
    },
    {
      name: "EBuffStackPolicy / EBuffKind",
      kind: "列挙(enum)", header: "gameframework/BuffSystem.h",
      summary: "<b>EBuffStackPolicy</b>=既存バフへの再適用挙動 (Refresh=時間更新 / Stack=重ねがけ / Ignore=無視)。<b>EBuffKind</b>=大分類タグ (AttackUp/DefenseUp/SpeedUp/Regen/Poison/Burn/Freeze/Stun/Shield/Custom)。ロジック分岐は呼出側が kind を見て行う。",
      when: "バフ定義 (<t>FBuffDef</t>) を作る時に指定する。"
    },
    {
      name: "CCamera2D",
      kind: "クラス", header: "gameframework/Camera2D.h",
      summary: "2D <b>カメラ</b>。position/zoom/rotation、target 追従 (フレームレート非依存の指数 smoothing)、画面振動 (<t>trauma</t> 方式)、world↔screen 座標変換、移動範囲の clamp を持つ。<code>CSceneServices</code> 経由で自動 Tick できる。",
      when: "2D ゲームでプレイヤー追従・ヒット時の画面振動・座標変換が欲しい時。",
      members: [
        { sig: "void SetPosition(FVec2 p) / FVec2 Position() const", desc: "カメラ位置を設定/取得する。" },
        { sig: "void SetZoom(f32 z) / void SetRotation(f32 r)", desc: "ズーム (>1 で拡大) と回転 (radians, + で CCW) を設定する。" },
        { sig: "void SetTargetPos(FVec2 target_pos, f32 smoothing = 5.0f)", desc: "追従先を毎フレーム値で渡す (安定ポインタ不要)。smoothing 大ほどキビキビ、0 以下で即スナップ。", when: "プレイヤー位置にカメラを追わせる時。" },
        { sig: "void AddShake(f32 amount)", desc: "trauma を加算して画面振動を起こす ([0,1] にクランプ)。重ねると派手になる。", when: "被弾/爆発などのインパクト時。" },
        { sig: "void SetShakeAmplitude(f32 a) / void SetShakeDecayRate(f32 r)", desc: "振動の最大幅 / 減衰速度を設定する。" },
        { sig: "FVec2 EffectiveViewCenter() const", ret: "振動込みの中心", desc: "shake オフセットを足した実際の view 中心。レンダラが view に使う値。" },
        { sig: "void SetBounds(FVec2 min, FVec2 max) / void ClearBounds()", desc: "カメラ位置を矩形内に clamp する範囲を設定/解除する。" },
        { sig: "FVec2 ScreenToWorld(FVec2 screen, u32 w, u32 h) const / FVec2 WorldToScreen(FVec2 world, u32 w, u32 h) const", desc: "画面ピクセル↔world 座標を相互変換する (zoom/rotation 考慮)。" },
        { sig: "void Tick(f32 dt)", desc: "追従 smoothing・範囲 clamp・trauma 減衰と振動オフセット計算を行う。Services が自動で呼ぶ。" },
        { sig: "f32 TraumaLevel() const", ret: "trauma [0,1]", desc: "現在の <t>trauma</t> 値 (振動の強さ)。0 で無振動。" },
        { sig: "bool HasTarget() const / void ClearTarget()", desc: "追従先が設定済みか / 追従を解除する (位置は固定される)。" },
        { sig: "bool HasBounds() const", desc: "移動範囲の clamp が設定済みか (<code>SetBounds</code> 済みなら true)。" },
        { sig: "using FCamera2D = CCamera2D", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CCamera2D</code> を使う。" }
      ]
    },
    {
      name: "COrbitCameraController3D",
      kind: "クラス", header: "gameframework/OrbitCameraController3D.h",
      summary: "3D orbit camera の移動・回転・距離zoomを renderer・device・World・platform 入力から切り離す決定論 controller。入力、外部所有 state、固定時間から次 state と eye/look-at を計算する。",
      when: "gameplay、AI、replay、headless test で同じ 3D camera 移動を再現したい時。",
      members: [
        { sig: "bool TryConfigure(const FOrbitCameraSettings3D& settings)", desc: "有限な速度と安全範囲だけを transactional に反映する。" },
        { sig: "bool TryStep(const FOrbitCameraInput3D& input, f32 dt, FOrbitCameraState3D& state) const", desc: "入力、状態、時間から次状態を計算する。失敗時は state を変更しない。" },
        { sig: "bool TryBuildView(const FOrbitCameraState3D& state, FOrbitCameraView3D& view) const", desc: "左手座標系の eye、look-at、up を計算する。失敗時は view を変更しない。" },
        { sig: "const FOrbitCameraSettings3D& Settings() const", desc: "現在の検証済み速度と pitch 上限を返す。" }
      ]
    },
    {
      name: "FOrbitCameraInputActionSet3D",
      kind: "構造体", header: "gameframework/OrbitCameraInputActionSet3D.h",
      summary: "6個の名前付きactionを3D orbit camera入力へ変換する値。固定tick、AI、replayの明示入力状態を同じ経路で評価する。",
      when: "FInputMapとIInputStateViewをCOrbitCameraController3Dへ接続したい時。",
      members: [
        { sig: "bool IsValid() const", desc: "6 action IDが有効かつ互いに異なる場合はtrueを返す。" },
        { sig: "bool TryEvaluate(const FInputMap& map, const IInputStateView& input, FOrbitCameraInput3D& output) const", desc: "明示入力状態を評価し、成功時だけ6軸のcamera入力を反映する。" }
      ]
    },
    {
      name: "CCameraStack",
      kind: "クラス", header: "gameframework/CameraStack.h",
      summary: "複数の <t>CCamera2D</t> を<b>仮想カメラのスタック</b>として持ち、最上層を active として扱う Cinemachine 風スイッチャ。Push/Pop で旧 top→新 top を線形補間でブレンドし、描画側は Effective* で「今どこを写しているか」を読む。",
      when: "平常カメラと演出カメラを滑らかに切り替えたい時 (ボス登場の寄りなど)。最大 4 層。",
      members: [
        { sig: "void PushCamera(CCamera2D& cam, f32 blend_duration = 0.5f)", desc: "カメラを top に積み、blend_duration 秒かけて旧 top から補間する (<=0 で即時)。カメラは非所有。" },
        { sig: "void PopCamera(f32 blend_duration = 0.5f)", desc: "top をフェードアウトしてから取り除く。1 枚以下では無視 (空にしない)。" },
        { sig: "CCamera2D* Active() const", ret: "現在の top", desc: "最上層のカメラを返す (空なら nullptr)。" },
        { sig: "bool IsBlending() const / f32 BlendProgress() const", desc: "ブレンド中か / その進捗 [0,1] (非ブレンド時は 1) を返す。" },
        { sig: "FVec2 EffectivePosition() const / f32 EffectiveZoom() const / f32 EffectiveRotation() const", ret: "補間済みの値", desc: "ブレンド中は下層と補間した値を返す。zoom は対数補間、rotation は最短角補間。", when: "描画フレームで view 設定に使う。" },
        { sig: "void Tick(f32 dt)", desc: "ブレンドタイマーを進め、active な 2 層だけ Tick し、pop blend 完了で実際に top を外す。" },
        { sig: "u32 Depth() const / void Clear()", desc: "スタックの深さを返す / 全部クリアする。" },
        { sig: "using FCameraStack = CCameraStack", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CCameraStack</code> を使う。" }
      ]
    },
    {
      name: "CCameraShakePresets",
      kind: "クラス", header: "gameframework/CameraShakePresets.h",
      summary: "「爆発 / 地震 / 着弾」などの<b>ジャンル別 shake パラメータを 1 行で流し込む</b> preset ライブラリ。trauma/amplitude/decay を名前付きで提供し、ゲームコードに magic number を書かずに済む。trauma を吸う側は <t>IShakeTarget</t> 抽象で受ける。",
      when: "<code>CCamera2D</code> 等に「爆発っぽい振動」を即座に与えたい時。カスタム preset も名前付きで登録できる。",
      members: [
        { sig: "static FShakeParams GetPreset(EShakePreset preset)", ret: "shake パラメータ", desc: "組み込み preset の trauma/amplitude/decay/freq 値を返す (Custom は中立値)。" },
        { sig: "static void ApplyPreset(IShakeTarget& target, EShakePreset preset)", desc: "preset を target に流し込む (SetAmp→SetDecay→AddShake の順、trauma は加算)。" },
        { sig: "void RegisterCustomPreset(const char* name, const FShakeParams& params)", desc: "名前付きカスタム preset を登録する (同名は上書き、null は no-op)。" },
        { sig: "bool ApplyCustomByName(IShakeTarget& target, const char* name)", ret: "見つかったか", desc: "名前で引いたカスタム preset を target に適用する。" },
        { sig: "u32 CustomCount() const", desc: "登録済みカスタム preset 数を返す。" },
        { sig: "using FCameraShakePresets = CCameraShakePresets", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CCameraShakePresets</code> を使う。" }
      ]
    },
    {
      name: "FShakeParams / EShakePreset / IShakeTarget",
      kind: "構造体・列挙(enum)・インターフェース", header: "gameframework/CameraShakePresets.h",
      summary: "<b>FShakeParams</b>=preset 1 個分のパラメータ (trauma/amplitude/decay_rate/frequency/duration_hint)。<b>EShakePreset</b>=組み込み種別 (ExplosionSmall/Large, EarthquakeShort/Long, HitImpact, RocketLaunch, MeteorImpact, Custom)。<b>IShakeTarget</b>=trauma を受ける側の純粋仮想 I/F (AddShake / SetShakeAmplitude / SetShakeDecayRate)。",
      when: "preset の値を組み立てたり、自前の振動対象を <t>CCameraShakePresets</t> に渡せるようにする時。"
    },
    {
      name: "ABtDecorator",
      kind: "クラス", header: "gameframework/BehaviorTree.h",
      summary: "子behavior tree nodeの実行条件や結果を包む管理object。",
      when: "一つのnodeへguardや結果変換を追加する時。",
      members: [
        { sig: "using FBtDecorator = ABtDecorator", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ABtDecorator</code> を使う。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "状態": "そのオブジェクトが今持っている値の集まり。多くの gameframework 型は状態を保持し、<code>Tick(dt)</code> で時間とともに更新する。",
  "POD": "Plain Old Data。コンストラクタやデストラクタで特別なことをしない、ただの値の入れ物。コピーが安全。",
  "RTTI": "実行時型情報 (Run-Time Type Information)。C++ が型を実行時に判別する仕組み。ACS は使わず、別の方法で型を識別する。",
  "シーム": "実装を後から差し替えられるよう間に挟む抽象境界 (純粋仮想インターフェース)。本体は具体実装に直接依存しない。",
  "スタブ": "本物の実装が無い時に置く、最小限の代用品。多くは「未実装」を返すだけで、リンクとビルドを通すために使う。",
  "プラットフォーム SDK": "Steam や各家庭機が提供する、実績・課金・マッチング等を扱う公式ライブラリ。",
  "ステートマシン": "「状態」と「状態間の遷移」で動きを記述する仕組み。Idle→Walk→Jump のようにキャラの挙動管理に使う。",
  "ビヘイビアツリー": "AI の判断を木構造で表す手法。Selector(OR) と Sequence(AND) と末端 Action を組み合わせて条件分岐を作る。",
  "ブラックボード": "ビヘイビアツリーのノード間で共有するデータ置き場。ACS では型を持たない <code>void*</code> で渡す。",
  "ダッキング": "ある音を目立たせるため、一時的に別の音 (主に BGM) の音量を下げる手法。",
  "trauma": "画面振動の強さを 0〜1 の蓄積値で表す方式 (Squirrel Eiserloh 提唱)。値の二乗で振幅を決め、時間で減衰させる。重ねがけで自然に派手になる。",
  "イージング": "値を時間で滑らかに変化させる加減速のカーブ (ease-in / ease-out 等)。",
  "世代付きハンドル": "index に世代番号 (gen) を付けた ID。対象が破棄されて slot が再利用されても、gen の不一致で古いハンドルを確実に弾ける。",
  "コールバック": "特定のタイミングで呼んでもらう関数。ACS では <code>std::function</code> を使わず関数ポインタ + <code>void* user</code> で渡す。"
});
