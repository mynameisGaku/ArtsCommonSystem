/* ACS リファレンス — gameframework モジュール (6/N: ProjectileSystem〜SeasonPass)。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "gameframework", order: 45, title: "gameframework — ゲーム部品 (acs::game)",
  blurb: "ゲームを組み立てる高レベル部品の集まり。シーンの切り替え、弾の発射、スコアやコンボ、乱数、セーブデータ、リプレイ、スクリプト連携、バトルパスなど『ゲームらしさ』をすぐ足せる道具を提供します。",
  types: [
    {
      name: "CProjectileSystem",
      kind: "クラス", header: "gameframework/ProjectileSystem.h",
      summary: "弾・矢・ロケット・魔法弾など『飛んでいって何かに当たる』ものを<b>固定容量の<t>プール</t></b>で一括管理するシステム。発射 → 飛翔 → 命中/寿命切れ までを毎フレームの <code>Tick</code> で進める。",
      when: "シューティングや弾幕、敵の攻撃弾など、大量の投射物を出したい時。<code>CWeaponSystem</code> と組み合わせて使うのが定番。",
      sample: "CProjectileSystem ps;\nps.Init(256);                    // 同時 256 発まで\nFProjectileDef d{};\nd.id = \"bullet\"; d.speed = 800.0f; d.lifetime_sec = 1.5f;\nps.RegisterDef(d);               // 弾種を登録\nps.Spawn(\"bullet\", pos, FVec2{800,0}, ownerId, /*damage=*/15.0f);\nps.Tick(dt);                     // 毎フレーム進める",
      members: [
        { sig: "void Init(u32 max_concurrent = 256)", desc: "<t>プール</t>の上限を確定する。0 を渡すと既定 256。再 <code>Init</code> は何もしない。", when: "システムを使い始める最初に 1 度だけ。" },
        { sig: "void RegisterDef(const FProjectileDef& def)", desc: "弾種 (<t>FProjectileDef</t>) を名前付きで登録する。同じ id があれば上書き。", sample: "ps.RegisterDef(rocketDef);" },
        { sig: "FProjectileId Spawn(const char* def_id, FVec2 pos, FVec2 velocity, u32 owner_id, f32 damage)", ret: "発射した弾の<t>ハンドル</t>", desc: "登録済みの弾種を 1 発撃つ。プール満杯や未登録 id なら invalid を返す。" },
        { sig: "void Despawn(FProjectileId id)", desc: "弾を強制的に消す (寿命切れ callback は呼ばれない)。" },
        { sig: "void Tick(f32 dt)", desc: "dt 秒ぶん全弾を進める。重力・<t>ホーミング</t>・移動・命中判定・寿命チェックをまとめて行う。", when: "ゲームループから毎フレーム呼ぶ。" },
        { sig: "void SetHitTestFn(HitTestFn fn, void* user)", desc: "命中判定を外部に委譲する<t>コールバック</t>を登録する。どの衝突空間を使うかはこの関数で決める。", when: "弾が敵に当たったかどうかを自分の当たり判定で調べたい時。" },
        { sig: "void SetOnHitCallback(HitCallback cb, void* user)", desc: "命中した瞬間に呼ばれる<t>コールバック</t>。ダメージ適用や VFX/SE をここで。" },
        { sig: "void SetOnExpireCallback(ExpireCallback cb, void* user)", desc: "寿命切れで消えた時に呼ばれる<t>コールバック</t>。" },
        { sig: "void SetHomingTarget(FProjectileId id, FVec2 target_pos)", desc: "<t>ホーミング</t>弾の追従先を指定する。homing=false の弾には効かない。" },
        { sig: "u32 AliveCount() const", ret: "生存中の弾数", desc: "今飛んでいる弾の数。" },
        { sig: "const FProjectileInstance* AllAlive(u32& out_count) const", ret: "生存弾の連続配列", desc: "描画用に、生きている弾だけを連続バッファで取り出す。" },
        { sig: "void ClearAll()", desc: "全弾を即座に消す (callback は呼ばない)。登録済み弾種やプール容量は維持。" },
        { sig: "using FProjectileSystem = CProjectileSystem", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CProjectileSystem</code> を使う。" }
      ]
    },
    {
      name: "FProjectileDef",
      kind: "構造体", header: "gameframework/ProjectileSystem.h",
      summary: "<b>弾種ごとの挙動パラメータ</b>。<code>RegisterDef</code> で『弾の設計図』として登録しておき、<code>Spawn</code> 時に名前で参照する。",
      when: "新しい弾の種類 (速い弾・重力で落ちる手榴弾・貫通弾・追尾弾など) を定義する時。",
      sample: "FProjectileDef d{};\nd.id = \"arrow\";\nd.speed = 600.0f;\nd.lifetime_sec = 2.0f;\nd.gravity_y = 300.0f;      // 弧を描いて落ちる\nd.pierces = true; d.max_pierces = 2;  // 3 体目で消える",
      members: [
        { sig: "const char* id", desc: "弾種の名前 (<code>Spawn</code> のキー)。文字列リテラル推奨。" },
        { sig: "EProjectileKind kind", desc: "VFX/SE 振り分け用のヒント。挙動には影響しない。" },
        { sig: "f32 speed / f32 lifetime_sec / f32 radius", desc: "初速の目安・最大寿命(秒)・当たり判定半径。" },
        { sig: "f32 gravity_y", desc: "重力 (正で下向き)。0 で直進、手榴弾や矢は >0。" },
        { sig: "bool pierces / u32 max_pierces", desc: "貫通するか、何体まで貫通するか。max_pierces=2 なら 3 体目で消滅。" },
        { sig: "bool homing / f32 homing_strength", desc: "追尾するか、その強さ [0,1]。" }
      ]
    },
    {
      name: "FProjectileId",
      kind: "構造体", header: "gameframework/ProjectileSystem.h",
      summary: "弾 1 発を指す<b>軽量<t>ハンドル</t></b>。24bit のインデックス + 8bit の<t>世代番号</t>を u32 に詰めたもの。スロット再利用後の古い参照を検出できる。",
      when: "発射した特定の弾を後から触りたい時 (追従先の指定や強制消去など)。",
      sample: "FProjectileId id = ps.Spawn(\"bullet\", pos, vel, owner, 10.0f);\nif (id.IsValid()) ps.SetHomingTarget(id, enemyPos);",
      members: [
        { sig: "bool IsValid() const", ret: "有効か", desc: "<code>m_Packed != 0</code> なら有効。発射失敗時は invalid。" },
        { sig: "u32 Index() const / u8 Gen() const", desc: "詰め込まれたインデックスと<t>世代番号</t>を取り出す。" }
      ]
    },
    {
      name: "FProjectileInstance",
      kind: "構造体", header: "gameframework/ProjectileSystem.h",
      summary: "飛んでいる弾 1 発の<b>生データ</b>。位置・速度・経過時間・命中回数・撃ち主・ダメージを持つ。<code>AllAlive</code> で描画側に渡される。",
      when: "弾を自分で描画する時、各弾の <code>position</code> や <code>def_id</code> を読み取る。",
      members: [
        { sig: "const char* def_id", desc: "弾種名 (描画でアセットを引くキー)。" },
        { sig: "FVec2 position / FVec2 velocity", desc: "現在位置と速度。" },
        { sig: "u32 owner_id / f32 damage", desc: "撃った主体の識別子と 1 ヒット当たりのダメージ。" }
      ]
    },
    {
      name: "EProjectileKind",
      kind: "列挙(enum)", header: "gameframework/ProjectileSystem.h",
      summary: "弾種ヒント。<code>Bullet</code> / <code>Rocket</code> / <code>Arrow</code> / <code>MagicBolt</code> / <code>Grenade</code> / <code>BeamPulse</code> / <code>Custom</code>。描画や効果音の切り替えキーとして使い、挙動そのものには影響しない。",
      when: "弾の見た目や効果音をカテゴリで分けたい時。"
    },
    {
      name: "FRandom",
      kind: "クラス", header: "gameframework/Random.h",
      summary: "<b>決定論的な<t>乱数生成器</t></b>(xoshiro128**)。既存の16byte状態と値列を保ち、定数時間snapshot・検査済み一括生成・偏り除去APIを提供する。",
      when: "敵の出現位置・ドロップ抽選・シャッフルなど、ゲーム中の乱数全般。再現性が欲しいなら<t>シード</t>を明示する。",
      sample: "FRandom r(0x12345678ull);\nFRandomSnapshot saved = r.CaptureSnapshot();\nu32 selected = 0;\nconst f32 weights[] = {1.0f, 2.0f, 3.0f};\nif (r.TryWeightedIndex(weights, 3, selected)) { /* 使用 */ }\nr.TryRestoreSnapshot(saved);",
      members: [
        { sig: "explicit FRandom(u64 seed)", desc: "<t>シード</t>を指定して作る。引数なしだと起動時刻ベース。" },
        { sig: "void Seed(u64 seed)", desc: "任意の u64 で再<t>シード</t>する。" },
        { sig: "u32 NextU32()", ret: "32bit 乱数", desc: "最下層の生の乱数を 1 個取り出す。" },
        { sig: "bool TryDiscard(u64 draw_count)", ret: "成功したか", desc: "最大1,048,576個だけ列を進める。上限超過時は状態を維持する。" },
        { sig: "FRandomSnapshot CaptureSnapshot() const", ret: "現在の再生位置", desc: "raw stateを改変検出付き値としてO(1)で取得する。" },
        { sig: "bool TryRestoreSnapshot(const FRandomSnapshot& snapshot)", ret: "復元したか", desc: "版・予約値・全0状態・検査値を確認してO(1)で復元する。失敗時は状態を維持する。" },
        { sig: "f32 NextF32Unit()", ret: "[0,1) の小数", desc: "0 以上 1 未満の <code>f32</code>。確率判定や補間に。" },
        { sig: "i32 RangeInt(i32 min, i32 max)", ret: "[min,max] の整数", desc: "互換用の1回剰余方式。両端を含み、max&lt;minなら交換する。", sample: "i32 dmg = r.RangeInt(8, 12);" },
        { sig: "f32 RangeF32(f32 min, f32 max)", ret: "[min,max) の小数", desc: "範囲内の <code>f32</code> 乱数。" },
        { sig: "bool NextBool(f32 true_probability = 0.5f)", ret: "真偽値", desc: "指定確率で true を返す。" },
        { sig: "FVec2 PointInCircle(f32 radius = 1.0f)", ret: "円板内の点", desc: "半径 radius の円の内側に一様な点を返す。" },
        { sig: "FVec2 PointOnCircle(f32 radius = 1.0f)", ret: "円周上の点", desc: "円周上に一様な点を返す。" },
        { sig: "void Shuffle<T>(TArray<T>& values)", desc: "互換用の32bit剰余方式で配列をその場で並べ替える。", sample: "r.Shuffle(deck);" },
        { sig: "u32 WeightedChoice(const f32* weights, u32 count)", ret: "選ばれた index", desc: "互換用の24bit重み抽選。新規処理は検査済み版を使う。" },
        { sig: "bool TryWeightedIndex(const f32* weights, u32 count, u32& out_index)", ret: "成功したか", desc: "最大4,096個の有限・非負重みを全検査し、最大値正規化と53bit抽選を行う。失敗時は状態と出力を維持する。" },
        { sig: "bool TryFillRangeF32(f32* values, u32 count, f32 min, f32 max)", ret: "成功したか", desc: "最大4,096個を有限な半開区間で埋める。0件はnull可、同値範囲は乱数を消費しない。" },
        { sig: "bool TryFillRangeIntUnbiased(i32* values, u32 count, i32 min, i32 max)", ret: "成功したか", desc: "32bit棄却法で偏りのない整数列を生成する。i32全域にも対応する。" },
        { sig: "bool TryShuffleIndicesUnbiased(u32* indices, u32 count)", ret: "成功したか", desc: "高位32bitから結合する64bit棄却法で最大4,096個のindexを並べ替える。" },
        { sig: "static FRandom& Global()", ret: "共有インスタンス", desc: "プロセス内で1個の共有値。呼び出し側が単一threadへ閉じ、同期機構は暗黙に追加しない。" }
      ]
    },
    {
      name: "FRandomSnapshot",
      kind: "構造体", header: "gameframework/RandomSnapshot.h",
      summary: "<code>FRandom</code>の4状態を版・予約値・標準FNV-1a 64bit検査値と共に保持する32byte値型。標準layoutかつ単純copy可能だが、native byte列は保存形式ではない。",
      when: "リプレイや決定論テストで同じ乱数位置へO(1)で戻す時。永続化では各fieldを明示的なbyte順で符号化する。",
      members: [
        { sig: "u32 version", desc: "保存形式の版。現行値は<code>kCurrentVersion</code>。" },
        { sig: "u32 state0 / state1 / state2 / state3", desc: "xoshiro128**の4状態。全0は無効。" },
        { sig: "u32 reserved", desc: "将来拡張用。現行版では0だけ有効。" },
        { sig: "u64 signature", desc: "版、状態、予約値をlittle-endian順に畳み込んだ標準FNV-1a 64bit検査値。" },
        { sig: "static constexpr u32 kCurrentVersion = 1", desc: "現行の保存形式版。" }
      ]
    },
    {
      name: "FRenderContext",
      kind: "クラス", header: "gameframework/RenderContext.h",
      summary: "<b>全シーン共有の描画コンテキスト</b>。今フレームの描画コマンドリスト・画面サイズ・共有<t>スプライトバッチ</t>・UI フォントなどをまとめて持ち、<code>AScene::OnRender</code> に渡される。",
      when: "シーンの描画関数の中で、画面に絵や文字を出す入口として受け取る。",
      sample: "void OnRender(FRenderContext& rc) noexcept override {\n    if (rc.HasSprites()) {\n        rc.Sprites().DrawString(rc.GetFont(), \"SCORE\", x, y);\n    }\n    u32 w = rc.Width(), h = rc.Height();\n}",
      members: [
        { sig: "IRhiCommandList& Cmd() const", ret: "コマンドリスト", desc: "今フレームの描画コマンド発行先。素の <t>RHI</t> を直接叩く時に。" },
        { sig: "CRenderer& GetRenderer() const", ret: "レンダラ", desc: "描画システム本体への参照。" },
        { sig: "u32 Width() const / u32 Height() const", desc: "描画対象の画面サイズ (ピクセル)。" },
        { sig: "bool IsFrameActive() const", desc: "今フレームの描画中かどうか。" },
        { sig: "bool HasSprites() const / CSpriteBatch& Sprites() const", desc: "<code>CGame</code> が共有し <code>AScene</code> が現在の描画パスへ配線した<t>スプライトバッチ</t>を取得する。" },
        { sig: "bool HasFont() const / FFont& GetFont() const", desc: "共有 UI フォントを取得する。読めなかった環境では <code>HasFont()==false</code>。" },
        { sig: "bool HasReflection() const / IRhiTexture& Reflection() const", desc: "水面などの平面反射に使うシーンテクスチャを取得する。" },
        { sig: "FVec2 ViewCenter() const / f32 ViewScale() const", desc: "2D の world→screen 変換パラメータ。画面投影に使う。" },
        { sig: "bool StencilMaskActive() const", desc: "<t>ステンシルマスク</t>が有効なパスかどうか。クリップ系コンポーネントが判定に使う。" }
      ]
    },
    {
      name: "CSceneRenderResources",
      kind: "クラス", header: "gameframework/SceneRenderResources.h",
      summary: "全シーンが <t>CGame</t> の寿命で共有する描画リソース束。world/HUD・反射・水深度の 3 本の <t>CSpriteBatch</t> と、反射 RT・水深度 RT・stencil を必要になるまで確保しない。",
      when: "独自のシーン描画パスで共有バッチや遅延生成 RT を準備・参照する時。通常の <t>AScene</t> 描画では <code>CGame</code> が管理するため直接触らない。",
      members: [
        { sig: "bool EnsureSpriteBatch(FRenderContext& rc) / EnsureSceneSprites(rc) / EnsureWaterDepthSprites(rc)", desc: "対応する <t>CSpriteBatch</t> を必要になった時だけ初期化する。失敗時は false。" },
        { sig: "bool EnsureSceneRt(FRenderContext& rc) / EnsureWaterDepthRt(rc) / EnsureStencilBuffer(rc)", desc: "対応する GPU texture を遅延生成する。失敗時は false で既存状態を保つ。" },
        { sig: "CSpriteBatch& SpriteBatch() / SceneSprites() / WaterDepthSprites()", desc: "初期化済みの world/HUD・反射・水深度用バッチを取得する。" },
        { sig: "IRhiTexture* SceneRt() / WaterDepthRt() / StencilBuffer()", desc: "生成済み texture を返す。未生成なら nullptr。" }
      ]
    },
    {
      name: "CReplayDirector",
      kind: "クラス", header: "gameframework/ReplayDirector.h",
      summary: "<b>高レベルな<t>リプレイ</t>制御</b>。録画開始/停止・再生・一時停止・倍速・任意位置への<t>シーク</t>を、ゲーム UI の粒度で扱う。低レベルの入力記録 (<code>CInputRecorder</code>) と<t>ロックステップ</t> (<code>CLockstep</code>) をまとめる。",
      when: "プレイのリプレイを残したい、ベストプレイを後で再生したい、バグ再現用に録画したい時。",
      sample: "CReplayDirector dir;\ndir.Init();\ndir.SetSources(&recorder, &lockstep);\nFReplayMetadata m{}; m.game_version = \"1.0.0\"; m.level_id = \"stage_01\";\ndir.StartRecording(m);\n// ...プレイ...\ndir.StopRecording();\ndir.SaveReplay(L\"user/replay_01.acsr\");",
      members: [
        { sig: "void Init()", desc: "<t>Idle</t> 状態にリセットする。再生速度 1.0・tick 0。" },
        { sig: "void SetSources(CInputRecorder* recorder, CLockstep* lockstep)", desc: "保存/復元の対象となる低レベル source を注入する (非所有)。" },
        { sig: "TResult<void> StartRecording(const FReplayMetadata& meta)", desc: "録画を開始し、メタデータをコピー保存する。Idle 以外で呼ぶと <code>kSub_BadMode</code>。" },
        { sig: "TResult<void> StopRecording()", desc: "録画を止め、総 tick 数を <code>duration_ticks</code> に確定する。" },
        { sig: "TResult<void> StartPlayback()", desc: "録画したものを先頭から再生開始する。" },
        { sig: "void PausePlayback() / ResumePlayback() / StopPlayback()", desc: "再生の一時停止・再開・停止。状態が合わない呼び出しは no-op。" },
        { sig: "void SetPlaybackSpeed(f32 speed)", desc: "0.25x〜16x の倍速を設定する。範囲外は近い有効値に丸める。" },
        { sig: "void SeekToTick(u32 tick)", desc: "再生位置を任意の tick に飛ばす。末尾を超えたら末尾に<t>クランプ</t>。", when: "スクラブバー UI やバグ再現ジャンプ。" },
        { sig: "void Tick(f32 dt)", desc: "毎フレーム呼ぶ。録画中は tick を進め、再生中は速度倍で進める。" },
        { sig: "TResult<void> SaveReplay(const wchar_t* file_path)", desc: "metadata + 入力 blob を 1 ファイル (.acsr) に<t>アトミック書き込み</t>で保存する。" },
        { sig: "TResult<void> LoadReplay(const wchar_t* file_path)", desc: "container を読み、magic/version/CRC を検証して復元する。" },
        { sig: "EReplayMode CurrentMode() const / f32 ProgressNormalized() const", desc: "現在の動作モードと、再生進捗 [0,1] を返す。" },
        { sig: "using FReplayDirector = CReplayDirector", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CReplayDirector</code> を使う。" }
      ]
    },
    {
      name: "FReplayMetadata",
      kind: "構造体", header: "gameframework/ReplayDirector.h",
      summary: "1 録画分の<b>メタデータ</b>。ゲームバージョン・レベル ID・<t>シード</t>・録画日時・録画時間・プレイヤー名・チェックサムを持つ。",
      when: "リプレイ一覧の表示や、バージョン不一致の検出に使う。",
      members: [
        { sig: "const char* game_version / const char* level_id", desc: "ゲームバージョンと録画開始時のシーン ID (長寿命な文字列を渡す)。" },
        { sig: "u64 seed", desc: "決定論再生用の RNG <t>シード</t>。" },
        { sig: "u64 timestamp / u32 duration_ticks", desc: "録画開始時刻 (Unix 秒) と総 tick 数 (停止時に確定)。" },
        { sig: "const char* player_name / const char* checksum_hex", desc: "プレイヤー名と<t>ロックステップ</t>チェックサムの hex 文字列。" }
      ]
    },
    {
      name: "EReplayMode",
      kind: "列挙(enum)", header: "gameframework/ReplayDirector.h",
      summary: "リプレイの動作モード。<code>Idle</code> (停止) / <code>Recording</code> (録画中) / <code>Playback</code> (再生中) / <code>Paused</code> (再生一時停止)。",
      when: "今録画中なのか再生中なのかで UI 表示を切り替えたい時。"
    },
    {
      name: "CSaveArchive",
      kind: "クラス (static のみ)", header: "gameframework/SaveArchive.h",
      summary: "<b>低レベルなセーブファイル I/O</b>。任意の <t>POD</t> バイト列を <code>.acssave</code> 形式 (24 バイトヘッダ + payload + <t>CRC32</t>) で読み書きする。<code>TSaveSlot</code> の土台。",
      when: "可変長データを扱いたい、テンプレートを介さず生バイトを保存したい時。普通は <code>TSaveSlot</code> を使えば十分。",
      sample: "auto wr = CSaveArchive::WriteToFile(L\"profile.acssave\", 1u, &p, sizeof(p));\nif (wr.IsErr()) { /* 報告 */ }\nu64 actual = 0;\nFSaveArchive::ReadFromFile(L\"profile.acssave\", &p, sizeof(p), 1u, actual);",
      members: [
        { sig: "static TResult<void> WriteToFile(const wchar_t* path, u32 version, const void* payload, u64 size)", desc: "payload を 1 ファイルに書き出す。先頭に magic とヘッダ、末尾に <t>CRC32</t> を付ける。" },
        { sig: "static TResult<u32> ReadFromFile(const wchar_t* path, void* out, u64 cap, u32 expected_version, u64& out_size)", ret: "実 version", desc: "検証して payload を読み込む。version 不一致なら <code>kSubMigrationNeeded</code> を返す。" },
        { sig: "static TResult<u32> PeekVersion(const wchar_t* path)", ret: "version", desc: "ヘッダだけ読んで version を返す。", when: "『セーブデータが古い』表示を出す事前判定に。" },
        { sig: "static TResult<u64> PeekPayloadSize(const wchar_t* path)", ret: "payload バイト数", desc: "読み込み前にバッファサイズを知りたい時。" },
        { sig: "static constexpr usize kHeaderSize = 24", desc: "固定ヘッダのバイト数。" },
        { sig: "using FSaveArchive = CSaveArchive", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSaveArchive</code> を使う。" }
      ]
    },
    {
      name: "ESaveArchiveSubCode",
      kind: "列挙(enum)", header: "gameframework/SaveArchive.h",
      summary: "セーブ I/O の失敗理由コード。<code>FErrorCode.subcode</code> に入り、上位が原因で分岐できる。",
      when: "セーブ/ロード失敗を細かく判定したい時 (破損・version 違い・バッファ不足など)。",
      members: [
        { sig: "kSubBadMagic / kSubChecksumFail", desc: "先頭が <code>.acssave</code> でない / <t>CRC32</t> 不一致 (破損や改竄)。" },
        { sig: "kSubBufferTooSmall / kSubFileNotFound / kSubIoError", desc: "バッファ不足・ファイル無し・下層 I/O 失敗。" },
        { sig: "kSubMigrationNeeded", desc: "読めたが version が違う。<t>マイグレーション</t>のきっかけに使う non-fatal 扱い。" }
      ]
    },
    {
      name: "TSaveSlot&lt;T&gt;",
      kind: "クラステンプレート", header: "gameframework/SaveSlot.h",
      summary: "<b>1 個の <t>POD</t> 構造体を 1 ファイルに保存/復元するセーブスロット</b>。タイトル画面の continue 判定やオプション設定、進捗データの土台。",
      when: "プレイヤープロフィールや設定など、固定レイアウトのデータを手軽に保存したい時。",
      sample: "struct FProfile { acs::u32 hi_score; acs::f32 volume; };\nFProfile profile{};\nTSaveSlot<FProfile> slot;\nslot.Init(L\"user/profile.acssave\");\nif (slot.Exists()) { auto r = slot.Load(); if (r) profile = r.Value(); }\nslot.Save(profile);",
      members: [
        { sig: "void Init(const wchar_t* file_path)", desc: "このスロットのファイルパスを設定する (文字列は呼び出し側が寿命を保証)。" },
        { sig: "TResult<void> Save(const T& data, u32 version = 1)", desc: "<code>data</code> を保存する。<code>version</code> を上げると旧データ読み込み時に migrate を促せる。" },
        { sig: "TResult<T> Load(u32 expected_version = 1)", ret: "復元した T", desc: "ファイルから読み出す。version 不一致なら <code>kSubMigrationNeeded</code>。" },
        { sig: "bool Exists() const", ret: "存在するか", desc: "ファイルがあるかだけ判定する (未初期化なら false)。" },
        { sig: "TResult<void> Delete()", desc: "ファイルを削除する (無くても成功扱いの冪等動作)。" }
      ]
    },
    {
      name: "AScene",
      kind: "基底クラス", header: "gameframework/Scene.h",
      summary: "<b>1 画面 / 1 状態を表す基底クラス</b>。root <code>ANode</code> と world/HUD 描画フックを持ち、<code>CGame</code> が共有する描画資源を借りて描く。タイトル・ゲーム本編・ポーズなどを派生クラスで書き、<code>CGame</code> がスタックで切り替え・更新・描画する。",
      when: "ゲームの画面 (状態) を 1 つ作る時の基本。すべての画面はこれを継承する。",
      sample: "class ATitleScene : public acs::game::AScene {\nprotected:\n    void OnReady() noexcept override { /* ノード生成 */ }\n    void OnTick(f32 dt) noexcept override {\n        if (start) Scenes().ChangeScene(MakeUnique<AGameScene>());\n    }\n    void OnDrawHud(FRenderContext& rc, CSpriteBatch& sb) noexcept override { /* HUD */ }\n};",
      members: [
        { sig: "virtual void OnEnter() / OnExit()", desc: "シーンが top に来た直後 / 退場する直前に呼ばれる。アセット読み込みや後片付けに。" },
        { sig: "virtual void OnPause() / OnResume()", desc: "上に別シーンが Push された時 / Pop で戻った時に呼ばれる。" },
        { sig: "virtual void OnUpdate(f32 dt)", desc: "毎フレームのロジック更新。dt はスケール後の秒。", when: "ゲームの毎フレーム処理を書く中心。" },
        { sig: "virtual void OnFixedUpdate(f32 fixed_dt)", desc: "固定タイムステップ更新。物理など一定刻みの処理に。" },
        { sig: "virtual void OnRender(FRenderContext& rc)", desc: "描画。<code>rc</code> から共有<t>スプライトバッチ</t>やフォントを使える。" },
        { sig: "ANode& Root() / const ANode& Root() const", ret: "ルートノード", desc: "シーンが所有する<t>ノードツリー</t>の根。ここに子ノードをぶら下げる。" },
        { sig: "CSpriteBatch& SpriteBatch()", desc: "<code>CGame</code> が game 寿命で共有する world/HUD 用<t>スプライトバッチ</t>を取得する。" },
        { sig: "void SetPixelsPerUnit(f32 ppu) / f32 PixelsPerUnit() const", desc: "1 ワールド単位を何ピクセルにするかの倍率 (既定 64)。" },
        { sig: "FVec2 ScreenToWorld(FVec2 screen_px)", ret: "ワールド座標", desc: "画面ピクセル座標を、直近の画面サイズ・カメラ・PPUに対応するワールド座標へ変換する。" },
        { sig: "virtual void OnReady() / OnTick(f32) / OnFixedTick(f32)", desc: "override 用フック。準備・毎フレーム・固定刻みのロジック。" },
        { sig: "virtual void OnDrawWorld(FRenderContext&, CSpriteBatch&) / OnDrawHud(...)", desc: "world (カメラ追従) と HUD (画面固定) の描画を分けて書く。" },
        { sig: "virtual void OnEvent(const FEvent& e)", desc: "ウィンドウ/入力イベントの受け取り。最上段シーンにのみ届く。" },
        { sig: "virtual ESvc WantedServices() const", ret: "使うサービス", desc: "このシーンが使う<t>サービス</t>を bit flag で宣言する。", sample: "ESvc WantedServices() const noexcept override { return ESvc::Default2D; }" },
        { sig: "CSceneServices& Services() const", ret: "サービスハブ", desc: "宣言済みの <code>CSceneServices</code> を取得する (未宣言で呼ぶと停止)。" },
        { sig: "CGame& GetGame() const / CSceneManager& Scenes() const", desc: "ゲーム本体とシーン管理への参照。シーン遷移は <code>Scenes().ChangeScene(...)</code>。" },
        { sig: "bool HasServices() const", desc: "このシーンに <code>CSceneServices</code> が attach 済みか。<code>Services()</code> を呼ぶ前の安全確認に使える (未宣言で <code>Services()</code> を呼ぶと停止する)。" },
        { sig: "using FScene = AScene", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AScene</code> を使う。" }
      ]
    },
    {
      name: "AScene2D",
      kind: "クラス", header: "gameframework/Scene2D.h",
      summary: "<b>2D ゲーム向けの既定サービスを選ぶ空派生</b>。root・描画配線・描画フックは基底 <code>AScene</code> が持ち、この型は <code>Default2D | Camera2D | Physics2D</code> を既定要求する。",
      when: "カメラと 2D 物理を常に使う画面を作る時。サービスを個別選択する画面は <code>AScene</code> を直接継承する。",
      sample: "class AStageScene : public acs::game::AScene2D {\nprotected:\n    void OnReady() noexcept override { /* ノード生成 */ }\n    void OnTick(f32 dt) noexcept override { /* ロジック */ }\n    void OnDrawWorld(FRenderContext& rc, CSpriteBatch& sb) noexcept override { /* world */ }\n    void OnDrawHud(FRenderContext& rc, CSpriteBatch& sb) noexcept override { /* HUD */ }\n};",
      members: [
        { sig: "ESvc WantedServices() const override", ret: "Default2D | Camera2D | Physics2D", desc: "2D の既定入力・カメラ・物理サービスを要求する。root と描画 API はすべて <code>AScene</code> から継承する。" },
        { sig: "using FScene2D = AScene2D", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AScene2D</code> を使う。" }
      ]
    },
    {
      name: "CSceneManager",
      kind: "クラス", header: "gameframework/SceneManager.h",
      summary: "<b><code>AScene</code> のスタック管理</b>。top のシーンを毎フレーム更新/描画し、画面切替 (Change/Push/Pop) を<b>フレーム境界まで遅延</b>して安全に適用する。",
      when: "シーンを切り替える時。普通は <code>AScene::Scenes()</code> 経由でこれを触る。",
      sample: "// シーン内から:\nScenes().ChangeScene(MakeUnique<AGameScene>());  // 切替\nScenes().PushScene(MakeUnique<APauseScene>());   // 上に重ねる (モーダル)\nScenes().PopScene();                            // 戻る",
      members: [
        { sig: "void ChangeScene(TUniquePtr<AScene> next)", desc: "今の top を pop して <code>next</code> を push (= 画面切替)。次フレーム頭で適用。" },
        { sig: "void PushScene(TUniquePtr<AScene> next)", desc: "今の top を残したまま <code>next</code> を重ねる (= ダイアログ/ポーズ)。" },
        { sig: "void PopScene()", desc: "top を 1 枚 pop する。スタックが 1 枚以下なら何もしない。" },
        { sig: "AScene* Top() const / u32 Depth() const / bool IsEmpty() const", desc: "現在の最上段シーン・スタック段数・空かどうか。" },
        { sig: "using FSceneManager = CSceneManager", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSceneManager</code> を使う。" }
      ]
    },
    {
      name: "CSceneServices",
      kind: "クラス", header: "gameframework/SceneServices.h",
      summary: "<b>シーンが使うサービスの取り付けハブ</b>。<t>シーンクロック</t>・<t>トゥイーン</t>・<t>シーケンス</t>・入力マップ・カメラ・物理・トリガーを、<code>ESvc</code> で宣言した分だけ遅延確保して保持する。<code>CGame</code> が自動で更新する。",
      when: "シーンで時間スケール・補間アニメ・入力アクション・2D カメラ/物理を使いたい時。<code>AScene::Services()</code> 経由でアクセスする。",
      sample: "ESvc WantedServices() const noexcept override { return ESvc::Default2D; }\nvoid OnEnter() noexcept override {\n    Services().Input().BindKey(FActionId(\"Jump\"), EKey::Space);\n    Services().Tweens().Tween(&m_Color, c1, c2, 2.0f, Easing::InOutSine);\n}",
      members: [
        { sig: "bool Has(ESvc s) const / ESvc Wanted() const", desc: "宣言したサービスを持っているか、何を要求したか。" },
        { sig: "CSceneClock& Clock()", desc: "時間スケールや一時停止を扱う<t>シーンクロック</t>。" },
        { sig: "CTweenManager& Tweens()", desc: "値を滑らかに補間する<t>トゥイーン</t>マネージャ。" },
        { sig: "CSequenceRunner& Sequences()", desc: "時間順の演出を組む<t>シーケンス</t>ランナー。" },
        { sig: "FInputMap& Input()", desc: "キー/パッドをアクション名に束ねる入力マップ。" },
        { sig: "CCamera2D& Camera() / CCollisionWorld2D& Physics() / CTriggerWorld2D& Triggers()", desc: "2D カメラ・衝突ワールド・トリガーワールド。宣言していなければ呼ぶと停止。" },
        { sig: "using FSceneServices = CSceneServices", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSceneServices</code> を使う。" }
      ]
    },
    {
      name: "ESvc",
      kind: "列挙(enum)", header: "gameframework/SceneServices.h",
      summary: "シーンが要求する<t>サービス</t>の bit flag。<code>Clock</code>/<code>Tweens</code>/<code>Sequences</code>/<code>Input</code>/<code>Camera2D</code>/<code>Physics2D</code>/<code>Triggers</code>。<code>Default2D</code> は前 4 つの合成。<code>|</code> で組み合わせる。",
      when: "<code>AScene::WantedServices()</code> の戻り値として、使うサービスを宣言する時。",
      sample: "return ESvc::Default2D | ESvc::Camera2D | ESvc::Physics2D;"
    },
    {
      name: "CSceneEventBus",
      kind: "クラス", header: "gameframework/SceneEventBus.h",
      summary: "<b>シーン内の <t>pub/sub</t> メッセージング</b>。同じシーンのコンポーネント同士が、相手を直接知らずに通知をやり取りできる。イベント名は<t>コンパイル時ハッシュ</t>で u32 に畳む。",
      when: "『プレイヤーが死んだ』『敵が湧いた』などの出来事を、疎結合に複数の相手へ知らせたい時。",
      sample: "// 購読側\nFSceneEventBus events;\nm_Sub = events.Subscribe(FEventId(\"PlayerDied\"), &OnDied, this);\n// 発行側\nFPlayerDiedPayload p{ pos, cause };\nevents.Publish(FEventId(\"PlayerDied\"), &p, sizeof(p));",
      members: [
        { sig: "u32 Subscribe(FEventId id, HandlerFn fn, void* user)", ret: "解除用ハンドル", desc: "イベントにハンドラを登録する。0 は失敗。" },
        { sig: "void Unsubscribe(u32 handle)", desc: "ハンドルで購読を解除する。発行中に呼んでも安全。" },
        { sig: "void Publish(FEventId id, const void* payload = nullptr, u32 size = 0)", desc: "イベントを発行し、登録順に購読者を呼ぶ。" },
        { sig: "u32 SubscriberCount(FEventId id) const", desc: "そのイベントの購読者数 (デバッグ用)。" },
        { sig: "void ClearAll()", desc: "全購読を破棄する。<code>OnExit</code> 等で。" },
        { sig: "using FSceneEventBus = CSceneEventBus", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSceneEventBus</code> を使う。" }
      ]
    },
    {
      name: "FEventId",
      kind: "構造体", header: "gameframework/SceneEventBus.h",
      summary: "イベント識別子。文字列リテラルから<t>コンパイル時ハッシュ</t> (FNV-1a) で u32 を生成する。実行時の文字列比較なしで高速。",
      when: "イベントを <code>Subscribe</code>/<code>Publish</code> する時の名前付けに。",
      sample: "FEventId id(\"EnemySpawned\");  // コンパイル時に u32 化"
    },
    {
      name: "CSceneCommandQueue",
      kind: "クラス", header: "gameframework/SceneCommandQueue.h",
      summary: "<b>遅延実行のコマンドキュー</b>。更新/描画の走査中に出された『ノード追加』『削除』などの要求を貯めておき、フレーム末の <code>Flush</code> でまとめて安全に実行する。エディタ連携にも使う。",
      when: "ループ走査の途中で構造を変えたい時 (走査中の削除は危険なので遅延する)。優先度や連打抑制も扱える。",
      sample: "// 走査中に削除要求しても安全 (Flush でまとめて実行)\nm_Cmds.Enqueue(\"DeleteSelected\", &DeleteSelected, this);\nm_Cmds.EnqueueIfAbsent(\"Refresh\", &RefreshUi, this, /*priority=*/50);\n// フレーム末:\nm_Cmds.Flush();",
      members: [
        { sig: "void Enqueue(const char* label, SceneCommandFn fn, void* user, u32 priority = 100, bool one_shot = true)", desc: "コマンドを末尾に積む。<code>one_shot</code> なら 1 回で消え、false なら毎 Flush 繰り返す。" },
        { sig: "void EnqueueIfAbsent(const char* label, SceneCommandFn fn, void* user, u32 priority = 100)", desc: "同 label が既にあれば何もしない (連打/重複の畳み込み)。" },
        { sig: "void Cancel(const char* label)", desc: "label に一致する全コマンドを削除する。" },
        { sig: "void Flush()", desc: "優先度の昇順 (同値は登録順) で全コマンドを実行する。", when: "フレーム末に 1 回呼ぶ。" },
        { sig: "u32 PendingCount() const / bool Contains(const char* label) const", desc: "待機中のコマンド数、特定 label が居るか。" },
        { sig: "void ClearAll()", desc: "全コマンドを破棄する (callback は呼ばない)。" },
        { sig: "using FSceneCommandQueue = CSceneCommandQueue", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSceneCommandQueue</code> を使う。" }
      ]
    },
    {
      name: "CSceneTimer",
      kind: "クラス", header: "gameframework/SceneTimer.h",
      summary: "<b>シーンスコープの遅延コールバック</b>。<code>SetTimeout</code>/<code>SetInterval</code> で『何秒後に』『何秒ごとに』関数を呼ぶ。シーンが死ぬと自動で破棄されるのがグローバルなタイマーとの違い。",
      when: "敵を一定間隔で湧かせる、数秒後にウェーブを終わらせる、など時間ベースの処理を書く時。",
      sample: "m_SpawnTimer = m_Timers.SetInterval(2.0f, &SpawnEnemy, this);\nm_Timers.SetTimeout(10.0f, &EndWave, this);\n// OnUpdate から:\nm_Timers.Tick(dt);",
      members: [
        { sig: "FSceneTimerHandle SetTimeout(f32 delay_sec, TimerCallback cb, void* user)", ret: "シーンタイマーハンドル", desc: "delay 秒後に 1 回だけ実行する。delay&lt;=0 や cb=null は invalid。" },
        { sig: "FSceneTimerHandle SetInterval(f32 period_sec, TimerCallback cb, void* user)", ret: "シーンタイマーハンドル", desc: "period 秒ごとに繰り返し実行する (Cancel まで永続)。" },
        { sig: "bool Cancel(FSceneTimerHandle h)", desc: "タイマーを止める。止められたら true。" },
        { sig: "void CancelAll()", desc: "全タイマーを止める。<code>OnExit</code> 等で。" },
        { sig: "bool IsActive(FSceneTimerHandle h) const / u32 ActiveCount() const", desc: "そのタイマーが生きているか、稼働中の総数。" },
        { sig: "void Tick(f32 dt)", desc: "毎フレーム呼ぶ。大きい dt では Interval が同フレームで複数回発火し得る。" },
        { sig: "using FSceneTimer = CSceneTimer", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSceneTimer</code> を使う。" }
      ]
    },
    {
      name: "FSceneTimerHandle",
      kind: "構造体", header: "gameframework/SceneTimerHandle.h",
      summary: "CSceneTimer 内のタイマー 1 個を指す 4byte の<t>ハンドル</t>。24bit index + 8bit <t>世代番号</t>の packed u32 で、古い参照を検出できる。旧 <code>game::FTimerHandle</code> は ACS 0.x 限定のソース互換名で、binary ABI は維持しない。",
      when: "登録したタイマーを後でキャンセルしたい時に保持しておく。",
      members: [
        { sig: "bool IsValid() const", desc: "有効なハンドルか (<code>m_Packed != 0</code>)。" }
      ]
    },
    {
      name: "CScoreSystem",
      kind: "クラス", header: "gameframework/ScoreSystem.h",
      summary: "<b>スコア累積 + コンボ + 倍率 + マイルストン通知</b>を 1 つにまとめたマネージャ。<code>NotifyHit</code> でコンボが伸び、<code>AddScore</code> でコンボ由来の倍率を掛けて加算する。一定時間ヒットが無いとコンボはリセット。",
      when: "アーケード/アクション系で『連鎖でスコア倍率が上がる』仕組みを作る時。",
      sample: "CScoreSystem ss;\nss.Init();\nss.RegisterMilestone(10000);\nss.SetOnMilestoneCallback(&OnMilestone, user);\n// 敵撃破時:\nss.NotifyHit();\nss.AddScore(\"enemy.normal\", 100);  // 100 × 現在倍率\nss.Tick(dt);",
      members: [
        { sig: "void Init()", desc: "スコア/コンボ/履歴/マイルストン状態をクリアする (ハイスコアは保持)。" },
        { sig: "void AddScore(const char* category, u64 base_value)", desc: "<code>base × 現在倍率</code> を加算し、履歴に記録する。マイルストン通過もここで判定。", when: "得点が入るたびに呼ぶ。" },
        { sig: "void NotifyHit()", desc: "ヒット成功。コンボを 1 増やしてコンボ時間をリセットする。" },
        { sig: "void NotifyMiss()", desc: "ミス/被弾。コンボと倍率を 0/1.0x に戻す。" },
        { sig: "void Tick(f32 dt)", desc: "毎フレーム呼ぶ。コンボ時間を減らし、0 でコンボをリセットする。" },
        { sig: "u64 CurrentScore() const / u64 HighScore() const", desc: "現在スコアとベスト記録。" },
        { sig: "u32 ComboCount() const / f32 ComboTimeRemaining() const / f32 ComboMultiplier() const", desc: "コンボ数・残り時間・現在の倍率。UI 表示に。" },
        { sig: "void SetComboDuration(f32 sec)", desc: "コンボが切れるまでの秒数を設定する。" },
        { sig: "void SetMultiplierFn(MultiplierFn fn)", desc: "コンボ→倍率の計算を差し替える。nullptr で既定 (1.0 + combo×0.1, 上限 10x) に戻す。" },
        { sig: "void RegisterMilestone(u64 milestone_score)", desc: "そのスコアを初めて超えた時に通知するマイルストンを登録する。" },
        { sig: "void SetOnMilestoneCallback(MilestoneCallback cb, void* user)", desc: "マイルストン通過時の<t>コールバック</t>を登録する。" },
        { sig: "void SetHighScore(u64 score)", desc: "ハイスコアを外部から注入する (セーブからの復元用)。" },
        { sig: "void Reset() / void ClearAll()", desc: "<code>Reset</code> はスコア/コンボを消すがベスト記録は残す。<code>ClearAll</code> は全部消す。" },
        { sig: "using FScoreSystem = CScoreSystem", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CScoreSystem</code> を使う。" }
      ]
    },
    {
      name: "FScoreEntry",
      kind: "構造体", header: "gameframework/ScoreSystem.h",
      summary: "1 回の <code>AddScore</code> に対応する記録。カテゴリ・素点・倍率適用後の加算値・倍率(×100 整数)を持つ。",
      when: "スコア内訳の表示やリザルト集計に。<code>AllEntries</code> でまとめて取り出す。",
      members: [
        { sig: "const char* category / u64 base_value / u64 weighted_value", desc: "加算カテゴリ・素点・実際に足された値。" },
        { sig: "u32 multiplier_x100", desc: "その時の倍率を ×100 した整数 (例: 250 = 2.5x)。<t>リプレイ</t>比較で誤差が出ないよう整数化。" }
      ]
    },
    {
      name: "CScriptHost",
      kind: "クラス", header: "gameframework/ScriptHost.h",
      summary: "<b>スクリプト呼び出しの単一窓口</b>。<code>IScriptVm</code> を 1 個保持し、ファイル読み込み・グローバル関数呼び出し・<t>ネイティブ関数</t>登録をまとめる。Mod や UI ロジックの動的拡張に使う。",
      when: "Lua などのスクリプトでゲームを拡張したい時。VM 本体は別モジュールが提供し、ここはその窓口。",
      sample: "CScriptHost host;\nhost.Init(&acs::game::GetDefaultScriptVm());\nhost.RegisterNative(\"Log\", &MyLog, this);\nhost.LoadAndRun(L\"mods/hello.lua\");\nFScriptValue ret{};\nhost.CallGlobalFunction(\"on_start\", nullptr, 0, &ret);",
      members: [
        { sig: "void Init(IScriptVm* vm)", desc: "使う VM を差し込む (所有はしない)。nullptr で切り離し。" },
        { sig: "void Shutdown()", desc: "VM 参照を切り、内部の登録リストもクリアする。" },
        { sig: "IScriptVm* Vm() const", desc: "保持中の VM を返す (未設定なら nullptr)。" },
        { sig: "TResult<void> LoadAndRun(const wchar_t* file_path)", desc: "ファイルを読み込んで実行する (上限 64 MiB)。" },
        { sig: "TResult<void> CallGlobalFunction(const char* name, const FScriptValue* args, u32 arg_count, FScriptValue* ret_out)", desc: "スクリプト側のグローバル関数を呼ぶ。" },
        { sig: "TResult<void> RegisterNative(const char* name, NativeFunction fn, void* user)", desc: "C++ 関数をスクリプトから呼べるよう登録する (内部 registry にも記録)。" },
        { sig: "void RegisterStandardBindings()", desc: "Log/Math/Time 等の標準<t>バインディング</t>を一括登録する (中身は将来フェーズ)。" },
        { sig: "void SetOnErrorCallback(ScriptErrorCallback cb, void* user)", desc: "スクリプト実行エラーを上位に通知する<t>コールバック</t>を設定する。" },
        { sig: "using FScriptHost = CScriptHost", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CScriptHost</code> を使う。" }
      ]
    },
    {
      name: "IScriptVm",
      kind: "インターフェース", header: "gameframework/ScriptHost.h",
      summary: "<b>スクリプト VM の純粋仮想<t>インターフェース</t></b>。Lua 5.4 / Wren / Python3 / 自前 VM などの実体を差し替えるための<t>シーム</t>。gameframework 本体は実ライブラリをリンクせず、別モジュールがこれを実装する。",
      when: "スクリプトバックエンドを実装/差し替える時。普通は <code>CScriptHost</code> 経由で間接的に使う。",
      sample: "// 実 backend を既定にする (例):\n#if WITH_ACS_SCRIPTING\n    acs::scripting::InstallLuaAsDefault();\n#endif\nIScriptVm& vm = acs::game::GetDefaultScriptVm();",
      members: [
        { sig: "virtual TResult<void> Init() / void Shutdown()", desc: "VM の初期化と破棄。" },
        { sig: "virtual EScriptLanguage Language() const", desc: "どの言語の backend かを返す。" },
        { sig: "virtual TResult<void> LoadScript(const char* source, u32 len, const char* chunk_name)", desc: "ソース文字列を読み込んで即実行する。" },
        { sig: "virtual TResult<void> CallFunction(const char* name, const FScriptValue* args, u32 count, FScriptValue* ret)", desc: "グローバル関数を呼ぶ。" },
        { sig: "virtual TResult<void> RegisterNativeFunction(const char* name, NativeFunction fn, void* user)", desc: "C++ 関数を script グローバルに bind する。" },
        { sig: "virtual void SetGlobalNumber(...) / f64 GetGlobalNumber(...) const", desc: "グローバル数値変数の設定/取得。" },
        { sig: "virtual void CollectGarbage() / u64 MemoryUsageBytes() const", desc: "強制 <t>GC</t> と概算メモリ使用量。" }
      ]
    },
    {
      name: "CScriptVmStub",
      kind: "クラス", header: "gameframework/ScriptHost.h",
      summary: "全 method が <code>kSub_NotImplemented</code> や no-op を返す<b>防御的スタブ</b>。実 backend が未リンクでも、スクリプト経路が安全に『常に失敗』するよう振る舞う。",
      when: "Lua 等を組み込まないビルドや、fallback の検証用。<code>GetVmStub()</code> で唯一の静的インスタンスを得る。",
      members: [
        { sig: "using FScriptVmStub = CScriptVmStub", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CScriptVmStub</code> を使う。" }
      ]
    },
    {
      name: "FScriptValue",
      kind: "構造体", header: "gameframework/ScriptHost.h",
      summary: "backend 中立な<b>動的型付け値</b>。<code>Nil</code>/<code>Bool</code>/<code>Number</code>(f64)/<code>FString</code>(非所有)/<code>Handle</code>(u32) のタグ付き union 風 POD。スクリプトと C++ の間で引数・戻り値を受け渡す。",
      when: "<t>ネイティブ関数</t>の引数や戻り値を読み書きする時。",
      sample: "FScriptValue v;\nv.kind = EScriptValueKind::Number;\nv.v.num = 3.14;",
      members: [
        { sig: "EScriptValueKind kind", desc: "今どの型を保持しているか。" },
        { sig: "union { bool b; f64 num; const char* str; u32 handle; } v", desc: "値本体。<code>str</code> は所有しない (寿命は呼び出し側)。" }
      ]
    },
    {
      name: "FScriptCallFrame",
      kind: "構造体", header: "gameframework/ScriptHost.h",
      summary: "<t>ネイティブ関数</t>呼び出し時の引数/戻り値ホルダ。引数配列・件数・戻り値書き込み先を持つ。メモリは backend が所有する。",
      when: "<code>NativeFunction</code> の中で <code>frame.args</code> を読み、<code>frame.ret</code> に結果を書く時。",
      members: [
        { sig: "const FScriptValue* args / u32 arg_count", desc: "引数配列とその長さ。" },
        { sig: "FScriptValue* ret", desc: "戻り値の書き込み先 (nullptr なら戻り値は捨てられる)。" }
      ]
    },
    {
      name: "EScriptLanguage / EScriptValueKind",
      kind: "列挙(enum)", header: "gameframework/ScriptHost.h",
      summary: "backend 言語タグ (<code>Lua54</code>/<code>Wren</code>/<code>Python3</code>/<code>Custom</code>) と、値の種別タグ (<code>Nil</code>/<code>Bool</code>/<code>Number</code>/<code>FString</code>/<code>Handle</code>)。",
      when: "VM の種類を判定したり、<code>FScriptValue</code> の中身を場合分けしたい時。"
    },
    {
      name: "GetVmStub / GetDefaultScriptVm / SetScriptVmProvider",
      kind: "関数", header: "gameframework/ScriptHost.h",
      summary: "既定の <code>IScriptVm</code> を取得/差し替える結線。実 backend モジュールが <code>SetScriptVmProvider</code> で実 VM を提供し、ゲームコードは <code>GetDefaultScriptVm</code> で backend 非依存に既定 VM を得る。未登録なら <code>GetVmStub</code> のスタブが返る。",
      when: "スクリプト backend をアプリ起動時に一度だけ結線したい時。",
      sample: "// 実 backend 側で:\nacs::game::SetScriptVmProvider(&MyLuaVmProvider);\n// ゲーム側で:\nIScriptVm& vm = acs::game::GetDefaultScriptVm();",
      members: [
        { sig: "IScriptVm& GetVmStub()", ret: "静的スタブ", desc: "常に存在する防御的スタブ VM を返す。" },
        { sig: "void SetScriptVmProvider(ScriptVmProvider provider)", desc: "既定 VM を返す関数を登録する。nullptr でスタブに戻す (後勝ち)。" },
        { sig: "IScriptVm& GetDefaultScriptVm()", ret: "既定 VM", desc: "provider 登録済みなら実 VM、未登録ならスタブを返す。" }
      ]
    },
    {
      name: "CSeasonPass",
      kind: "クラス", header: "gameframework/SeasonPass.h",
      summary: "<b>シーズン (バトルパス) の進行管理</b>。期間 (開始/終了 timestamp)・累積 XP による tier 進行・各 tier 報酬の請求状態を 1 クラスで扱う。報酬は固定 (乱数なし)・cosmetic 限定を強く推奨する倫理方針。",
      when: "期間限定シーズンや無料/プレミアムのパス報酬を実装する時。報酬 ID を吐き出すだけで、実体解放は entitlement 側に橋渡しする。",
      sample: "CSeasonPass sp;\nFSeasonInfo info{};\ninfo.season_id = \"season.spring_2026\";\ninfo.start_timestamp = 1748736000ull;\ninfo.end_timestamp   = 1756598400ull;\nsp.StartSeason(info);\nsp.DefineTier({ 0, 100, \"cosmetic.frame_t00\", nullptr });\nsp.AwardXp(150);\nsp.Tick(dt);\nif (sp.ClaimReward(0, /*premium=*/false)) { /* 付与 */ }",
      members: [
        { sig: "void StartSeason(const FSeasonInfo& info)", desc: "シーズンを開始する (xp=0、tier 定義は破棄、現在時刻 = 開始時刻)。" },
        { sig: "void DefineTier(const FTier& t)", desc: "tier を 1 件登録する。<code>tier_index</code> 重複は無視。" },
        { sig: "void AwardXp(u32 amount)", desc: "累積 XP を加算する (オーバーフローは max クランプ)。" },
        { sig: "void Tick(f32 dt)", desc: "内部時刻を進め、終了時刻を過ぎたら自動で <code>Ended</code> に遷移する。" },
        { sig: "void EndSeason()", desc: "timestamp を待たず手動でシーズンを終了させる (管理者/デバッグ用)。" },
        { sig: "u32 CurrentTier() const / u32 CurrentXp() const / ESeasonStatus Status() const", desc: "到達中の最大 tier・累積 XP・時刻ベースの状態。" },
        { sig: "void SetPremiumPass(bool has) / bool HasPremiumPass() const", desc: "プレミアムパス所持フラグの設定/取得 (課金検証は entitlement 側)。" },
        { sig: "bool ClaimReward(u32 tier_index, bool premium)", ret: "新規請求できたか", desc: "報酬を請求する。未解放/既請求/プレミアム未所持/報酬無しなら false。冪等。" },
        { sig: "bool IsRewardClaimed(u32 tier_index, bool premium) const", desc: "その報酬が請求済みか。" },
        { sig: "const char* GetRewardId(u32 tier_index, bool premium) const", ret: "報酬 ID", desc: "tier の報酬 ID を取得する (未定義/無しは nullptr)。" },
        { sig: "u32 ClaimableCount() const", ret: "請求可能数", desc: "未請求の解放済み報酬数 (UI の赤バッジ用)。" },
        { sig: "u32 TierCount() const", desc: "登録済み tier 件数。" },
        { sig: "using FSeasonPass = CSeasonPass", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSeasonPass</code> を使う。" }
      ]
    },
    {
      name: "FTier",
      kind: "構造体", header: "gameframework/SeasonPass.h",
      summary: "シーズンの 1 段階。tier index・解放に必要な XP 閾値・無料報酬 ID・プレミアム報酬 ID を持つ。報酬 ID は cosmetic を指す前提で、どちらも nullptr 可。",
      when: "<code>DefineTier</code> で各段階の到達条件と報酬を定義する時。",
      members: [
        { sig: "u32 tier_index / u32 xp_threshold", desc: "0 起点の一意 index と、解放に必要な累積 XP。" },
        { sig: "const char* reward_id_free / const char* reward_id_premium", desc: "無料/プレミアムストリームの報酬 ID (非所有、nullptr 可)。" }
      ]
    },
    {
      name: "FSeasonInfo",
      kind: "構造体", header: "gameframework/SeasonPass.h",
      summary: "1 シーズンの定義。シーズンキー・表示名・開始/終了 timestamp・想定 tier 数を持つ。",
      when: "<code>StartSeason</code> に渡してシーズンの期間と表示を決める時。",
      members: [
        { sig: "const char* season_id / const char* display_name", desc: "シーズンキー (永続化/分析用) と UI 表示名。" },
        { sig: "u64 start_timestamp / u64 end_timestamp", desc: "シーズン期間 (典型は Unix 秒、<code>Tick</code> の dt 単位と揃える)。" },
        { sig: "u32 max_tier", desc: "UI 表示用の想定 tier 数 (情報用)。" }
      ]
    },
    {
      name: "ESeasonStatus",
      kind: "列挙(enum)", header: "gameframework/SeasonPass.h",
      summary: "時刻ベースのシーズン状態。<code>NotStarted</code> (開始前) / <code>Active</code> (期間中) / <code>Ended</code> (終了後)。",
      when: "シーズン UI の表示や、XP/claim を許すかの判断に。"
    },
    {
      name: "CProjectSettings",
      kind: "クラス", header: "gameframework/ProjectSettings.h",
      summary: "game projectの設定値を読み書きし検証する。",
      when: "project設定を起動時に読み込む時。",
      members: [
        { sig: "using FProjectSettings = CProjectSettings", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CProjectSettings</code> を使う。" }
      ]
    },
    {
      name: "CRigidWorld2D",
      kind: "クラス", header: "gameframework/RigidWorld2D.h",
      summary: "2D rigid bodyの登録、更新、衝突解決をまとめる。",
      when: "scene内の2D物理worldを進める時。",
      members: [
        { sig: "using FRigidWorld2D = CRigidWorld2D", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CRigidWorld2D</code> を使う。" }
      ]
    },
    {
      name: "CRollbackSession",
      kind: "クラス", header: "gameframework/RollbackSession.h",
      summary: "入力履歴とsnapshotを使うrollback進行を管理する。",
      when: "決定論的な対戦状態を巻き戻して再実行する時。",
      members: [
        { sig: "using FRollbackSession = CRollbackSession", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CRollbackSession</code> を使う。" }
      ]
    },
    {
      name: "CScene3D",
      kind: "クラス", header: "gameframework/Scene3D.h",
      summary: "3D sceneのnode、更新、描画連携をまとめる。",
      when: "3D worldをscene単位で実行する時。",
      members: [
        { sig: "using FScene3D = CScene3D", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CScene3D</code> を使う。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "プール": "あらかじめ確保した固定数の枠を使い回す仕組み。確保/解放のコストとフレーム落ちを避けられる。",
  "ホーミング": "発射後に目標へ向きを補正して追尾する弾の挙動。",
  "世代番号": "<t>ハンドル</t>に埋め込む小さなカウンタ。スロット再利用後に古い参照を検出するために使う。",
  "リプレイ": "プレイの入力や状態を記録し、後で同じ進行を再生する仕組み。決定論的な<t>乱数</t>と相性が良い。",
  "ロックステップ": "全プレイヤーが同じ入力フレームを共有して同期する決定論シミュレーション方式。",
  "シーク": "再生位置を任意の時点へ飛ばすこと (スクラブバー操作など)。",
  "クランプ": "値を許される範囲の上限/下限に収めること。",
  "アトミック書き込み": "一時ファイルに書いてから rename する等で、途中クラッシュでも本ファイルを壊さない保存手法。",
  "マイグレーション": "古い形式のセーブデータを新しい形式へ変換すること。",
  "CRC32": "データから計算する 32bit のチェック値。破損や改竄の検出に使う。",
  "POD": "コンストラクタ等を持たない素朴なデータ構造 (memcpy で複製できる型)。",
  "ライフサイクル": "オブジェクトが生成→更新→破棄されるまでの一連の段階。各段階にフック関数が対応する。",
  "サービス": "シーンが必要に応じて使う共通機能 (時間・補間・入力・カメラ・物理など) の単位。",
  "シーンクロック": "シーンごとの時間管理。時間スケールや一時停止を扱う。",
  "トゥイーン": "値を時間をかけて滑らかに補間するアニメーションの仕組み (in/out イージング付き)。",
  "シーケンス": "時間順に処理を並べて演出やイベントを組み立てる仕組み。",
  "ノードツリー": "親子関係を持つノードの木構造。子は親の変形を受け継ぐ (2D/3D 共通シーングラフ)。",
  "pub/sub": "発行(publish)と購読(subscribe)で、送り手と受け手が互いを知らずに通知をやり取りする方式。",
  "コンパイル時ハッシュ": "文字列をコンパイル時に数値へ変換する手法。実行時の文字列比較を避けて高速化する。",
  "シーム": "実装を差し替えられるようにした境界 (インターフェース)。テストや backend 切替を容易にする。",
  "ネイティブ関数": "スクリプトから呼べるよう登録した C++ 側の関数。",
  "バインディング": "スクリプト言語と C++ の機能を結びつける橋渡し (関数や値の登録)。",
  "GC": "ガベージコレクション。不要になったメモリを自動回収する仕組み。",
  "スプライトバッチ": "多数の 2D 画像をまとめて効率よく描画する仕組み。",
  "RHI": "Render Hardware Interface。DirectX12 等の低レベル描画 API を抽象化した層。",
  "ステンシルマスク": "ステンシルバッファを使い、任意形状で描画範囲を切り抜く手法。",
  "シード": "<t>乱数</t>生成の初期値。同じシードからは同じ乱数列が得られる。",
  "シャッフル": "要素の並びをランダムに入れ替えること。"
});
