/* ACS リファレンス — gameframework (5/N) ゲーム部品 (acs::game)。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "gameframework", order: 44, title: "gameframework — ゲーム部品 (acs::game)",
  blurb: "ゲームを組み立てるための部品集 (<code>acs::game</code> 名前空間)。シーンの Node を識別する<t>ハンドル</t>、敵 AI 用の経路探索や知覚、パーティクル、ポーズ/写真モード、拾えるアイテム、レベルアップ進行、プライバシー同意など、ジャンルを問わず使える機能をまとめています。",
  types: [
    {
      name: "FNodeId",
      kind: "構造体", header: "gameframework/NodeId.h",
      summary: "シーングラフの Node を一意に指す 32bit の<t>世代付きハンドル</t>。中身は <b>24bit の index + 8bit の generation</b> を 1 個の <code>u32</code> に詰めたもの。<code>packed == 0</code> が無効値。",
      when: "Node を生ポインタで持つと破棄後に<t>ダングリング</t>になる。代わりにこの ID で持っておき、使う直前に <code>FNodePool::Get()</code> で取り出すと、破棄済みなら安全に nullptr が返る。",
      sample: "FNodeId id = pool.RegisterExistingNode(node);\nif (id.IsValid()) {\n    u32 slot = id.Index();       // プール配列の index\n    u8  gen  = id.Generation();  // 世代カウンタ\n}\nif (a == b) { /* 完全一致 */ }",
      members: [
        { sig: "constexpr FNodeId(u32 index, u8 gen)", desc: "index(24bit) と generation(8bit) を pack して構築。index は <code>&amp; 0x00FFFFFF</code> でマスクされ、24bit を超える上位 bit は黙って捨てられる。" },
        { sig: "constexpr u32 Index() const", ret: "0〜16,777,215", desc: "プール / SoA 配列の index を取り出す。" },
        { sig: "constexpr u8 Generation() const", ret: "0〜255", desc: "slot の世代カウンタ。<t>stale</t>(古い) ハンドル検出に使う。" },
        { sig: "constexpr bool IsValid() const", ret: "無効でなければ true", desc: "<code>packed != 0</code> なら true。ただし「pool に該当 slot が今も生きているか」は別途 <code>FNodePool</code> で検証すること。" },
        { sig: "constexpr bool operator==(FNodeId) const", desc: "index と generation の両方が一致した時のみ true。<code>!=</code> もある。" }
      ]
    },
    {
      name: "FNodePool",
      kind: "クラス", header: "gameframework/NodePool.h",
      summary: "シーン内の <code>FNode2D</code> を登録して、安定した <t>FNodeId</t> を発行する<t>レジストリ</t>。Node 本体は親が <code>TUniquePtr</code> で所有し続け、この pool は<b>参照だけ</b>を持つ (<t>非所有</t>)。発行済み ID の<t>stale</t>検出も担う。",
      when: "敵やアイテムなどを ID で安全に持ち回りたい時。破棄された Node を <code>Get()</code> しても nullptr が返るので、無効アクセスを防げる。",
      sample: "FNodePool pool;\npool.Init(512);\nFNodeId id = pool.RegisterExistingNode(enemyPtr);\n// 後で stale 検査付きで取り出し\nif (FNode2D* p = pool.Get(id)) {\n    p-&gt;Local().position += FVec2{1, 0};\n}\npool.Unregister(id);   // slot 解放 + gen++ で古い ID は無効化",
      members: [
        { sig: "void Init(u32 initial_capacity = 256)", desc: "初期容量を予約 (再 alloc 回避)。複数回呼べるが縮小はしない。" },
        { sig: "FNodeId RegisterExistingNode(FNode2D* node)", ret: "新しい ID (失敗時 invalid)", desc: "既存の Node を登録して新 ID を発行し、<code>node-&gt;_SetId</code> する。nullptr や 16M slot 到達時は invalid を返す。" },
        { sig: "void Unregister(FNodeId id)", desc: "slot を解放し、対応 Node の Id を invalid に戻し、generation を進める。二重 Unregister は安全 (no-op)。" },
        { sig: "bool IsValid(FNodeId id) const", desc: "id の slot が active かつ generation 一致なら true。" },
        { sig: "FNode2D* Get(FNodeId id) const", ret: "Node or nullptr", desc: "id 経由で Node を取り出す。stale / invalid なら nullptr。" },
        { sig: "FNodeId IdOf(FNode2D* node) const", ret: "ID (無ければ invalid)", desc: "ポインタから ID を逆引き (線形探索 O(N))。基本は登録時の戻り値を保持して使う。" },
        { sig: "u32 ActiveCount() const", desc: "現在 active な slot 数。" },
        { sig: "void ClearAll()", desc: "全 slot を一括解放。各 Node の Id は invalid に戻すが、Node 自体は削除しない (非所有なので)。" }
      ]
    },
    {
      name: "FOpenXrBridge / IOpenXrBridge",
      kind: "インターフェース", header: "gameframework/OpenXrBridge.h",
      summary: "VR / AR / MR (XR) ランタイムへの接続を抽象化する<t>seam</t>(差し替え点)。HMD のヘッドポーズ、左右コントローラの状態、passthrough の on/off だけを公開する最小の窓口。実 SDK (OpenXR / Oculus / SteamVR / PS VR2 等) は別モジュールで差し替える。",
      when: "VR ゲームを書くが、開発機に HMD が刺さっていなくても動くようにしたい時。未接続なら stub が「常に未トラッキング」を返すので、通常の 2D 描画への<t>フォールバック</t>を最初から書ける。",
      sample: "IOpenXrBridge& xr = GetDefaultOpenXrBridge();\nif (xr.Init().IsOk()) {\n    XrPose head = xr.HeadPose();\n    XrControllerState r = xr.RightController();\n    if (r.trigger &gt; 0.5f) { /* 撃つ */ }\n}\nxr.Tick(dt);",
      members: [
        { sig: "TResult&lt;void&gt; Init(EXrPlatform platform = Unknown)", ret: "成功 / NotImplemented", desc: "backend を初期化 (XR ランタイム接続 / セッション開始)。Unknown なら利用可能な SDK を自動検出。stub は常に NotImplemented。" },
        { sig: "void Shutdown()", desc: "backend を破棄。Init 前に呼んでも安全。" },
        { sig: "bool IsInitialized() const", desc: "Init 成功後かつ Shutdown 前なら true。" },
        { sig: "bool IsTracking() const", desc: "ヘッド/コントローラが今ライブトラッキング中か。false ならポーズは原点なので使ってはならない。IsInitialized() とは別概念。" },
        { sig: "XrPose HeadPose() const", ret: "HMD のポーズ", desc: "直近 Tick 時点の HMD 位置 + 向き。未初期化 / stub は原点 (zero pose)。" },
        { sig: "XrControllerState LeftController() const", desc: "左コントローラの 1 フレーム分の入力 snapshot。<code>RightController()</code> も同様。未接続側はゼロ state。" },
        { sig: "void Tick(f32 dt)", desc: "ポーズ / 入力の取り込みを進める (イベントポンプ相当)。毎フレーム呼ぶ。" },
        { sig: "bool IsPassthroughSupported() const", desc: "passthrough (MR モード) 対応かどうか。stub は常に false。" },
        { sig: "void SetPassthrough(bool on)", desc: "passthrough の on/off を要求。非対応 / 未初期化なら no-op で構わない。" }
      ]
    },
    {
      name: "EXrPlatform",
      kind: "列挙(enum)", header: "gameframework/OpenXrBridge.h",
      summary: "想定する XR backend プラットフォームの種別。<code>Unknown</code> / <code>MetaQuest</code> / <code>ValveIndex</code> / <code>HtcVive</code> / <code>PicoNeo</code> / <code>AppleVisionPro</code> / <code>PsVr2</code> / <code>WindowsMR</code> / <code>SteamLink</code>。",
      when: "<code>IOpenXrBridge::Init()</code> に「この HMD を使う」と明示したい時。Unknown を渡すと backend が自動検出する。",
      sample: "xr.Init(EXrPlatform::MetaQuest);"
    },
    {
      name: "XrPose",
      kind: "構造体", header: "gameframework/OpenXrBridge.h",
      summary: "HMD / コントローラの位置と向き。<code>position</code> は world 座標 (m)、<code>orientation_euler</code> は (pitch, yaw, roll) を<b>ラジアン</b>で持つ。<code>tracked</code> が false なら原点なので使わない。",
      when: "ヘッドの向きでカメラを回す、コントローラの位置に手を描く、といった用途。精密な向き計算が要るなら自分で euler から quaternion / 行列へ変換する。",
      sample: "XrPose p = xr.HeadPose();\nif (p.tracked) {\n    camera.SetPosition(p.position);\n    camera.SetYaw(p.orientation_euler.y);\n}"
    },
    {
      name: "XrControllerState",
      kind: "構造体", header: "gameframework/OpenXrBridge.h",
      summary: "左右コントローラ 1 フレーム分の入力 snapshot。6DoF ポーズ + トリガ <code>trigger</code> [0,1] + グリップ <code>grip</code> [0,1] + ボタン <code>button_a/b</code> + スティック <code>thumbstick</code> [-1,1]×2 という、各プラットフォーム差を吸収した最小公倍数。",
      when: "VR コントローラの入力を読む時。Quest / Index / Vive / PS VR2 のボタン差をこの 1 構造体で統一的に扱える。",
      sample: "XrControllerState c = xr.RightController();\nif (c.button_a) Jump();\nFVec2 move = c.thumbstick;   // [-1,1] スティック"
    },
    {
      name: "FParticleEffectSystem",
      kind: "クラス", header: "gameframework/ParticleEffectSystem.h",
      summary: "2D 向けの軽量 sprite <t>パーティクル</t>システム。<b>emitter</b> (放出口) を複数置いて、炎・煙・スパーク・吹き出しなどの連続演出を出す。描画はせず、<code>AllParticles()</code> で粒子データを渡すだけなので<t>headless</t>でも動く。",
      when: "ヒット時の火花、燃えるたいまつ、走った跡の砂煙など「粒子を出し続ける」演出が欲しい時。単発の Flash / Shake は別の <code>FEffectSystem</code> が担当。",
      sample: "ParticleEmitterDef def{};\ndef.lifetime_sec      = 0.8f;\ndef.emit_rate_per_sec = 30.0f;\ndef.speed_min = 40.0f; def.speed_max = 80.0f;\ndef.gravity = {0.0f, 60.0f};\nFParticleEffectSystem fx;\nfx.Init(2048);\nauto h = fx.CreateEmitter(def, {200.0f, 300.0f});\nfx.Tick(dt);                 // 毎フレーム\nfx.Burst(h);                 // 一気に放出 (爆発)",
      members: [
        { sig: "void Init(u32 max_particles = 1024)", desc: "粒子プールの上限を確定 (再 Init は no-op)。0 を渡すと 1024 を採用。" },
        { sig: "FEmitterHandle CreateEmitter(const ParticleEmitterDef&amp; def, FVec2 pos)", ret: "emitter ハンドル", desc: "emitter を 1 個作る。pos は world 座標。slot 上限到達や lifetime &lt;= 0 の def は invalid を返す。" },
        { sig: "void SetEmitterPosition(FEmitterHandle h, FVec2 pos)", desc: "emitter の位置を変える。invalid / stale は no-op。" },
        { sig: "void SetEmitterActive(FEmitterHandle h, bool active)", desc: "自動放出の on/off。false でも Burst は発火する。" },
        { sig: "void Burst(FEmitterHandle h)", desc: "<code>burst_count</code> 個を一気に放出 (空き数でクランプ)。爆発やヒット用。" },
        { sig: "void DestroyEmitter(FEmitterHandle h)", desc: "emitter を破棄。既存の粒子は寿命まで生き続ける。" },
        { sig: "void Tick(f32 dt)", desc: "dt 秒進める。連続放出 → 全粒子の物理更新 → 寿命切れ返却。dt &lt;= 0 は no-op。" },
        { sig: "const Particle* AllParticles(u32&amp; out_count) const", ret: "粒子の生バッファ", desc: "描画用に粒子配列を渡す。out_count はプール全体サイズ。各粒子の生死は <code>Particle::IsAlive()</code> で判定する。" },
        { sig: "u32 ActiveParticleCount() const", desc: "現在 active な (描画予定の) 粒子数。" },
        { sig: "void ClearAll()", desc: "全 emitter + 全粒子を即クリア。容量は維持。" }
      ]
    },
    {
      name: "ParticleEmitterDef",
      kind: "構造体", header: "gameframework/ParticleEffectSystem.h",
      summary: "emitter の挙動パラメータ。色 (<code>color_start/end</code>)、寿命 (<code>lifetime_sec</code>)、毎秒放出数 (<code>emit_rate_per_sec</code>)、初速 (<code>speed_min/max</code>)、サイズ (<code>scale_start/end</code>)、重力 (<code>gravity</code>)、Burst 数 (<code>burst_count</code>) を持つ。",
      when: "<code>CreateEmitter()</code> に渡す設定の塊。色やサイズは出生時から寿命末まで線形補間される。",
      sample: "ParticleEmitterDef fire{};\nfire.color_start = {1.0f, 0.6f, 0.1f};  // 橙\nfire.color_end   = {0.5f, 0.0f, 0.0f};  // 暗赤\nfire.scale_start = 8.0f; fire.scale_end = 0.0f;"
    },
    {
      name: "Particle",
      kind: "構造体", header: "gameframework/ParticleEffectSystem.h",
      summary: "個別の粒子データ。位置 / 速度 / 経過秒 (<code>age</code>) / 寿命 (<code>lifetime</code>) / 色 / サイズ / 重力を自分で持ち回る<t>self-contained</t>設計なので、emitter が消えても粒子は正しく動き続ける。",
      when: "描画側がこの構造体を直接読み、<code>age/lifetime</code> から補間係数を求めて色・サイズを決める。",
      sample: "u32 n = 0;\nconst Particle* p = fx.AllParticles(n);\nfor (u32 i = 0; i &lt; n; ++i) {\n    if (!p[i].IsAlive()) continue;\n    f32 t = p[i].NormalizedAge();   // 0..1\n    // 描画 (位置 p[i].position、色は補間)\n}",
      members: [
        { sig: "f32 NormalizedAge() const", ret: "0..1 の経過比", desc: "<code>age / lifetime</code> を 0..1 に clamp して返す。色・サイズ補間に使う。" },
        { sig: "bool IsAlive() const", desc: "<code>age &lt; lifetime</code> なら true (まだ生きている)。" }
      ]
    },
    {
      name: "FEmitterHandle",
      kind: "構造体", header: "gameframework/ParticleEffectSystem.h",
      summary: "emitter を指す 24bit index + 8bit gen の packed <t>ハンドル</t>。<code>m_Packed == 0</code> が無効。slot 再利用後の<t>stale</t>参照は <code>IsValid()</code> + 内部の gen 一致で検出する。",
      when: "<code>CreateEmitter()</code> の戻り値として受け取り、位置変更や Burst、破棄の対象指定に使う。",
      sample: "FEmitterHandle h = fx.CreateEmitter(def, pos);\nif (h.IsValid()) fx.Burst(h);"
    },
    {
      name: "FPartySystem",
      kind: "クラス", header: "gameframework/PartySystem.h",
      summary: "ローカルプレイヤーをまとめた「パーティ」の状態と、簡易フレンドリストを扱う窓口。ロビーや Co-op 入室前の集合、ストアのフレンド表示などを 1 つの API で扱う。<b>プラットフォーム非依存</b>で、実 SDK 接続は Storefront 側 (Steam/EOS/PSN 等) が橋渡しする。",
      when: "オンライン協力プレイのパーティ機能を作る時。状態遷移 (Solo → Joining → InParty → Leaving) とメンバ/フレンド管理だけをまず動かしたい時。",
      sample: "FPartySystem ps;\nps.AddFriend({ \"steam:7656...\", \"alice\", true, true });\nif (ps.CreateParty(\"my_squad\")) {\n    ps.InviteFriend(\"steam:7656...\");\n}\nps.Tick(dt);   // Joining/Leaving の timeout 処理",
      members: [
        { sig: "TResult&lt;void&gt; CreateParty(const char* party_name)", desc: "新規パーティ作成 (Solo 状態のみ受理)。状態を Joining → InParty へ遷移。" },
        { sig: "TResult&lt;void&gt; JoinParty(const char* party_id)", desc: "招待で受け取った ID で参加 (Solo のみ)。Joining に遷移し Tick で timeout 監視。" },
        { sig: "TResult&lt;void&gt; LeaveParty()", desc: "現在のパーティから離脱 (InParty のみ)。Leaving に遷移。" },
        { sig: "TResult&lt;void&gt; InviteFriend(const char* friend_id)", desc: "フレンドに招待を送る (InParty のみ)。リーダー権限の判定は呼び出し側責任。" },
        { sig: "bool IsInParty() const", desc: "パーティ所属中なら true。<code>State()</code> で <code>EPartyState</code> も取れる。" },
        { sig: "TResult&lt;void&gt; AddMember(const PartyMember&amp; member)", desc: "メンバを 1 件追加。player_id が nullptr / 既存と重複の場合はエラー。SDK で accept された相手を流し込む用途。" },
        { sig: "bool RemoveMember(const char* player_id)", desc: "一致メンバを削除して true。順序を保ち前詰め。見つからなければ false (no-op)。" },
        { sig: "u32 MemberCount() const", desc: "パーティ内メンバ数 (自分含む)。Solo は 0。<code>Members()</code> で生バッファも取れる。" },
        { sig: "u32 FindMember(const char* player_id) const", ret: "index or kInvalidIndex", desc: "player_id 一致メンバの index。<code>GetMember()</code> にそのまま渡せる。" },
        { sig: "void Tick(f32 dt)", desc: "Joining / Leaving の timeout 監視 + 状態遷移。呼ばないと Joining のまま停滞する。" },
        { sig: "void AddFriend(const Friend&amp; f)", desc: "フレンドを 1 件登録。<code>FriendCount()</code> / <code>Friends()</code> で一覧取得。" }
      ]
    },
    {
      name: "EPartyState",
      kind: "列挙(enum)", header: "gameframework/PartySystem.h",
      summary: "パーティ状態。<code>Solo</code> (非所属) / <code>InParty</code> (所属中) / <code>Joining</code> (参加要求中) / <code>Leaving</code> (離脱要求中)。Joining / Leaving は非同期 SDK 完了待ちの中間状態。",
      when: "UI で「参加中…」の表示を出したり、状態によって操作を許可/禁止したい時。",
      sample: "if (ps.State() == EPartyState::Joining) ShowSpinner();"
    },
    {
      name: "Friend / PartyMember",
      kind: "構造体", header: "gameframework/PartySystem.h",
      summary: "<code>Friend</code> = フレンド 1 件 (platform_id / display_name / online / playing_same_game)。<code>PartyMember</code> = パーティメンバ 1 件 (player_id / display_name / is_leader / is_ready)。文字列はすべて<t>非所有</t> const char* (寿命は呼び出し側保証)。",
      when: "フレンドリストやロビーのメンバ一覧を UI に出す時のデータ単位。",
      sample: "Friend f{ \"epic:abc\", \"bob\", true, false };\nps.AddFriend(f);\nPartyMember m{ \"steam:7656...\", \"me\", true, true };\nps.AddMember(m);"
    },
    {
      name: "NavGrid",
      kind: "クラス", header: "gameframework/Pathfinding.h",
      summary: "2D 格子の通行可否マップ + <t>A*</t> 経路探索。マス目ごとに歩けるか否かを設定し、開始セルからゴールセルまでの最短経路を求める。タワーディフェンス、敵の追跡、クリック移動 RPG などに使える。",
      when: "グリッドベースで「ここからあそこまでどう動くか」を計算したい時。4 方向 (上下左右) のみか、対角も許可するか選べる。",
      sample: "NavGrid nav;\nnav.Init(32, 24);\nnav.SetWalkable(5, 10, false);   // 壁を置く\nnav.SetAllowDiagonal(true);      // 8 方向許可\n\nTArray&lt;NavGrid::PathPoint&gt; path;\nif (nav.FindPath(0, 0, 20, 15, path)) {\n    // path[0]=start, path.Back()=goal の連続セル列\n}",
      members: [
        { sig: "void Init(u32 width, u32 height)", desc: "全セル walkable でグリッドを初期化。width / height が 0 ならサイズ 0 として以降のクエリは no-op。" },
        { sig: "void SetWalkable(u32 x, u32 y, bool walkable)", desc: "1 マスの通行可否を設定。範囲外は静かに無視。" },
        { sig: "bool IsWalkable(u32 x, u32 y) const", desc: "通行可否を取得。範囲外は false。" },
        { sig: "void SetAllowDiagonal(bool allow)", desc: "対角移動の許可。既定 false (4 方向)。true で 8 方向 + heuristic も Euclidean に切替。" },
        { sig: "bool FindPath(u32 sx, u32 sy, u32 gx, u32 gy, TArray&lt;PathPoint&gt;&amp; out)", ret: "見つかれば true", desc: "A* で start→goal の連続セルを out に書き込む。範囲外 / 通行不可 / 到達不能は out を Clear して false。start == goal は長さ 1 の成功。" },
        { sig: "void ClearWalls()", desc: "全セルを walkable に戻す (グリッドサイズは保持)。" }
      ]
    },
    {
      name: "NavGrid::PathPoint",
      kind: "構造体", header: "gameframework/Pathfinding.h",
      summary: "経路の 1 セルを表す座標ペア (<code>u32 x, y</code>)。<code>FindPath()</code> の出力配列の要素。",
      when: "求めた経路をたどってキャラを動かす時、各セルの座標として読む。",
      sample: "for (usize i = 0; i &lt; path.Size(); ++i) {\n    u32 cx = path[i].x, cy = path[i].y;\n}"
    },
    {
      name: "FPerception",
      kind: "クラス", header: "gameframework/Perception.h",
      summary: "NPC が複数の target を「視覚」「聴覚」で検知できるか判定するモジュール。視覚は<b>距離 + 視野角</b>、聴覚は<b>距離のみ</b> (壁ごしも聞こえる)。<t>ビヘイビアツリー</t>の葉や blackboard の値ソースとして使う想定。",
      when: "敵 AI に「プレイヤーが見えたら追う」「物音がしたら警戒する」といった知覚を持たせたい時。",
      sample: "FPerception perc;\nSenseConfig cfg;\ncfg.sight_range   = 10.0f;\ncfg.sight_fov_rad = acs::kPi * 0.5f;   // 90 度\ncfg.hearing_range = 5.0f;\nperc.SetConfig(cfg);\nperc.AddTarget(1, FVec2{3.0f, 0.0f});\nperc.SetEyePos(FVec2{0,0}, FVec2{1,0});   // +X 向き\nperc.Tick(0.016f);\nif (perc.IsTargetVisible(1)) { /* 視認 */ }",
      members: [
        { sig: "void SetConfig(const SenseConfig&amp; cfg)", desc: "感覚パラメータを差し替える。<code>cos(fov/2)</code> もここで 1 回だけ計算してキャッシュ。" },
        { sig: "void SetEyePos(FVec2 pos, FVec2 forward)", desc: "目の位置と前方向 (正規化済み前提) を更新。forward が長さ 0 なら (1,0) にフォールバック。" },
        { sig: "void AddTarget(u32 id, FVec2 pos)", desc: "知覚対象を追加。同 id が既にあれば位置更新だけ (重複防止)。" },
        { sig: "void RemoveTarget(u32 id)", desc: "指定 id の target を削除。順序は保持されない (高速削除)。" },
        { sig: "void UpdateTarget(u32 id, FVec2 pos)", desc: "target の位置を更新。存在しなければ no-op (Add はしない)。" },
        { sig: "bool IsTargetVisible(u32 id) const", desc: "前回 Tick 時点で視認できていたか。<code>IsTargetAudible()</code> は聴取版。" },
        { sig: "void Tick(f32 dt)", desc: "全 target の is_visible / is_audible を現在の eye / forward / config で再計算する。" },
        { sig: "const PerceptionTarget* AllTargets(u32&amp; out_count) const", ret: "target 配列", desc: "全 target を読み取り用に列挙。out_count に件数。次の Add/Remove/Update で無効化される。" }
      ]
    },
    {
      name: "SenseConfig / PerceptionTarget",
      kind: "構造体", header: "gameframework/Perception.h",
      summary: "<code>SenseConfig</code> = 感覚パラメータ (<code>sight_range</code> 視認距離 / <code>sight_fov_rad</code> 視野角ラジアン / <code>hearing_range</code> 聴取距離)。<code>PerceptionTarget</code> = 知覚対象 1 件 (pos / id と、Tick が更新する <code>is_visible</code> / <code>is_audible</code> 出力)。",
      when: "<code>SetConfig()</code> に渡す設定、および <code>AllTargets()</code> で読む結果のデータ単位。",
      sample: "SenseConfig cfg;\ncfg.sight_range = 8.0f;\ncfg.sight_fov_rad = acs::kPi / 3.0f;  // 60 度\ncfg.hearing_range = 4.0f;"
    },
    {
      name: "FPerfBudget",
      kind: "クラス", header: "gameframework/PerfBudget.h",
      summary: "フレーム時間 (ms) とメモリ確保量 (bytes) の<b>予算超過</b>を追跡する state holder。カテゴリ単位で予算を決め、毎フレーム実測値を記録 → 集計し、各カテゴリ / フレーム全体の超過を問い合わせられる。実測 (計測) と UI 描画は呼び出し側責務。",
      when: "「Render は 8ms 以内」「AI は 2ms 以内」のように予算を決めて、超えたら警告を出すパフォーマンス可視化を作りたい時。",
      sample: "FPerfBudget budget;\nbudget.SetFrameBudget(16.67f);                  // 60fps\nbudget.DefineCategory(\"Render\", 8.0f, 64u*1024u*1024u);\n// 毎フレーム\nbudget.BeginFrame();\nbudget.RecordTimeMs(\"Render\", render_ms);\nbudget.EndFrame();\nif (budget.IsOverBudget(\"Render\")) { /* 警告 */ }",
      members: [
        { sig: "void SetFrameBudget(f32 ms)", desc: "目標フレーム時間 (60fps→16.67)。<= 0 でフレーム超過判定を無効化。" },
        { sig: "void DefineCategory(const char* category, f32 budget_ms, u32 budget_bytes)", desc: "カテゴリを新規定義 / 上書き。同名があれば予算だけ差し替え、実測値は保持。名前は呼び出し側所有 (リテラル想定)。" },
        { sig: "void RecordTimeMs(const char* category, f32 elapsed_ms)", desc: "経過時間をカテゴリに加算 (累積)。未定義 / nullptr / <= 0 は no-op。" },
        { sig: "void RecordMemoryAlloc(const char* category, u32 bytes)", desc: "メモリ確保量を加算。<code>RecordMemoryFree()</code> は減算 (0 に clamp)。" },
        { sig: "void BeginFrame()", desc: "全カテゴリの spent_ms を 0 リセット (spent_bytes は累積保持)。" },
        { sig: "void EndFrame()", desc: "全カテゴリの spent_ms 合計を 60 frame 循環バッファに push し、フレーム超過判定を更新。" },
        { sig: "bool IsOverBudget(const char* category) const", desc: "そのカテゴリが ms か bytes のいずれかで予算超過しているか。" },
        { sig: "bool IsFrameOverBudget() const", desc: "直近 EndFrame のフレーム合計 ms が予算を超えたか。" },
        { sig: "f32 AverageFrameMs() const", desc: "直近 60 frame の合計 ms の算術平均。<code>LastFrameMs()</code> は最新値。" },
        { sig: "const BudgetEntry* AllCategories(u32&amp; out_count) const", ret: "カテゴリ配列", desc: "全カテゴリを読み取り用に列挙。out_count に件数。" },
        { sig: "void Reset()", desc: "全カテゴリ削除 + frame 履歴クリア (frame_budget_ms は保持)。" }
      ]
    },
    {
      name: "BudgetEntry",
      kind: "構造体", header: "gameframework/PerfBudget.h",
      summary: "1 カテゴリ分の予算 + 実測 (<code>category</code> 名 / <code>spent_ms</code> / <code>budget_ms</code> / <code>spent_bytes</code> / <code>budget_bytes</code>)。<code>FPerfBudget::AllCategories()</code> が返す要素。",
      when: "予算ビューを自分で描画する時、各カテゴリの現状を読む単位。",
      sample: "u32 n = 0;\nconst BudgetEntry* e = budget.AllCategories(n);\nfor (u32 i = 0; i &lt; n; ++i)\n    Draw(e[i].category, e[i].spent_ms, e[i].budget_ms);"
    },
    {
      name: "FPhotoMode",
      kind: "クラス", header: "gameframework/PhotoMode.h",
      summary: "プレイヤーが任意の瞬間に時間を止めて景色を撮影する「フォトモード」の<t>状態 holder</t>。active フラグ、カメラの自由操作 (パン / ズーム / 回転) のオフセット、色フィルタ種別、撮影リクエストを保持する。実際の時間停止や描画は呼び出し側が行う。",
      when: "近年の AAA タイトル風の写真撮影機能を付けたい時。時間を止め、カメラを自由に動かし、セピアやモノクロのフィルタをかけて 1 枚保存する、という流れを組み立てる。",
      sample: "FPhotoMode photo;\nphoto.Enter(game.TimeScale());          // 現在の time_scale を保存\ngame.SetTimeScale(0.0f);                 // 時間停止 (呼び出し側)\nphoto.MoveCamera({dx, dy});\nphoto.ZoomCamera(0.1f);                  // +10%\nphoto.SetFilter(FPhotoMode::Sepia);\nif (input.shutter) photo.RequestCapture();\nif (photo.ConsumeCaptureRequest()) SaveScreenshot();\nphoto.Exit();\ngame.SetTimeScale(photo.SavedTimeScale());",
      members: [
        { sig: "void Enter(f32 current_time_scale = 1.0f)", desc: "現在の time_scale を保存して active=true、カメラオフセットを初期化。実際の <code>SetTimeScale(0)</code> は呼び出し側。" },
        { sig: "void Exit()", desc: "active=false に戻す。呼び出し側が <code>SavedTimeScale()</code> を見て復元する。" },
        { sig: "void MoveCamera(FVec2 delta)", desc: "パン (入力 delta を累積)。world units 想定。" },
        { sig: "void ZoomCamera(f32 zoom_delta)", desc: "ズーム倍率を乗算的に変更。結果は [0.1, 10.0] に clamp。" },
        { sig: "void RotateCamera(f32 rad_delta)", desc: "回転を累積 (ラジアン)。元カメラの rotation に加算する用途。" },
        { sig: "void SetFilter(FilterKind f)", desc: "色フィルタ種別を設定。<code>GetFilter()</code> で取得。" },
        { sig: "void RequestCapture()", desc: "シャッターを押した時に呼ぶ。撮影 flag を立てるだけ。" },
        { sig: "bool ConsumeCaptureRequest()", ret: "立っていれば true", desc: "描画側が 1 フレーム末尾で呼ぶ。立っていれば true を返して同時に flag を落とす (<t>poll-and-consume</t>)。" },
        { sig: "f32 SavedTimeScale() const", desc: "Enter() 時に保存した time_scale。Exit() 後の復元に使う。" }
      ]
    },
    {
      name: "FPhotoMode::FilterKind",
      kind: "列挙(enum)", header: "gameframework/PhotoMode.h",
      summary: "フォトモードの色フィルタ種別。<code>None</code> (素通し) / <code>Sepia</code> (セピア) / <code>Grayscale</code> (モノクロ) / <code>Vivid</code> (高彩度) / <code>Vignette</code> (周辺光量落とし)。実 LUT / シェーダ解決はポストプロセスパスの責務で、本 enum は「どれが選ばれているか」だけを持つ。",
      when: "撮影画面で色味のプリセットを切り替えたい時。",
      sample: "photo.SetFilter(FPhotoMode::Grayscale);"
    },
    {
      name: "FPhysicsBody2D",
      kind: "クラス (FComponent2D 派生)", header: "gameframework/PhysicsBody2D.h",
      summary: "<t>kinematic</t> な 2D 物理ボディの<t>コンポーネント</t>。velocity + acceleration + gravity を統合し、<code>FCollisionWorld2D</code> に登録された他の形状とぶつかったら止まる / 滑る (<t>collide-and-slide</t>)。剛体ソルバではなく「2D プラットフォーマー / トップダウン用の swept kinematic」。",
      when: "重力で落ちて床で止まる、壁に沿って滑る、といった素朴な 2D キャラ移動を Node に付けたい時。形状は円 / AABB / 多角形 / OBB から選ぶ。",
      sample: "auto ball = MakeUnique&lt;FNode2D&gt;();\nball-&gt;Local().position = FVec2{0, 5};\nauto&amp; body = ball-&gt;AddComponent&lt;FPhysicsBody2D&gt;(Services().Physics());\nbody.SetCircle(0.5f);\nbody.gravity  = FVec2{0, 10};   // +Y=下\nbody.velocity = FVec2{1, 0};\nroot.AddChild(Move(ball));",
      members: [
        { sig: "explicit FPhysicsBody2D(FCollisionWorld2D&amp; world)", desc: "衝突ワールドへの参照を必須で受け取る (constructor)。" },
        { sig: "void SetCircle(f32 radius)", desc: "円形状に設定。<code>SetAabb</code> / <code>SetPolygon</code> / <code>SetObb</code> で AABB / 凸多角形 / 回転矩形にもできる (再設定で上書き)。" },
        { sig: "void SetObb(FVec2 half_size, f32 rotation)", desc: "回転矩形 (OBB) ボディ。回転を中心原点の local poly に焼き込む。" },
        { sig: "FShapeId Handle() const", desc: "現在の衝突ハンドル (deregister 時に無効化される)。" },
        { sig: "void OnUpdate(f32 dt)", desc: "コンポーネントの毎フレーム処理。dt で velocity / gravity を統合し、衝突したら止まる / 滑る。", when: "Scene の更新ループから自動で呼ばれる (直接は呼ばない)。" }
      ],
      note: "公開フィールド: <code>FVec2 velocity / acceleration / gravity</code> (動力学)、<code>bool slide</code> (既定 true。false で旧来の軸独立 block に切替)。"
    },
    {
      name: "FPickupSystem",
      kind: "クラス", header: "gameframework/PickupSystem.h",
      summary: "世界に転がっている「拾える物」(HP オーブ / コイン / ジェム / 弾薬 / パワーアップ / 鍵) を管理する小型マネージャ。プレイヤー位置との距離で<b>磁石効果 (引き寄せ)</b>・<b>拾取判定</b>・<b>寿命切れ消滅</b>を 1 つの Tick でまとめて処理する。",
      when: "Vampire Survivors や Hades のような「近づくと吸い寄せられて自動で拾う」アイテムドロップを作りたい時。拾うと callback で通知が来るので、インベントリ追加や HP 回復は呼び出し側で行う。",
      sample: "FPickupSystem ps;\nps.Init();\nPickupInfo info{};\ninfo.kind = EPickupKind::Coin;\ninfo.world_pos = {100, 50};\ninfo.radius = 12.0f;\ninfo.magnet_radius = 80.0f;\ninfo.value = 5;\nPickupId id = ps.Spawn(info);\nps.SetOnPickupCallback(&amp;OnPickup, user);\n// 毎フレーム\nps.Tick(dt, player_pos, /*magnet_strength=*/200.0f);",
      members: [
        { sig: "void Init()", desc: "初期化。複数回呼べる (再 Init は ClearAll と等価)。" },
        { sig: "PickupId Spawn(const PickupInfo&amp; info)", ret: "新規 ID (失敗時 invalid)", desc: "pickup を世界に登録する。" },
        { sig: "void Despawn(PickupId id)", desc: "pickup を消す (拾取扱いではなく callback も呼ばない)。無効 / 既消滅は no-op。" },
        { sig: "void Tick(f32 dt, FVec2 player_pos, f32 magnet_strength)", desc: "毎フレーム呼ぶ。寿命減算 → 磁石で引き寄せ → radius 内なら拾取 (callback + Despawn) を順に処理。" },
        { sig: "void SetOnPickupCallback(PickupCallback cb, void* user)", desc: "拾取時の callback を設定。<code>SetOnExpireCallback</code> は寿命切れ時。cb=nullptr で detach。" },
        { sig: "void SpawnRandomAt(EPickupKind kind, FVec2 center, f32 spread_radius)", desc: "中心の周り半径 spread_radius の円板内にランダム配置。半径/磁石/寿命/value は kind 別の既定値。" },
        { sig: "u32 CountOfKind(EPickupKind kind) const", desc: "指定 kind の active 数。<code>DespawnAllOfKind</code> で kind 別の一括消去もできる。" },
        { sig: "u32 AlivePickupCount() const", desc: "active な pickup の総数。" },
        { sig: "void ClearAll()", desc: "全 pickup を消滅 (callback 設定は維持)。" }
      ]
    },
    {
      name: "PickupInfo",
      kind: "構造体", header: "gameframework/PickupSystem.h",
      summary: "pickup 1 件の定義。<code>kind</code> 種別 / <code>item_id</code> (非所有) / <code>world_pos</code> / <code>radius</code> 拾取半径 / <code>magnet_radius</code> 磁石半径 / <code>lifetime_sec</code> (0 で無期限) / <code>value</code> 価値。<code>radius &lt; magnet_radius</code> を想定。",
      when: "<code>Spawn()</code> に渡す設定。寿命 0 にすれば明示削除まで残る永続 pickup になる。",
      sample: "PickupInfo hp{};\nhp.kind = EPickupKind::HealthOrb;\nhp.world_pos = {x, y};\nhp.radius = 14.0f; hp.magnet_radius = 90.0f;\nhp.lifetime_sec = 8.0f; hp.value = 25;"
    },
    {
      name: "PickupId",
      kind: "構造体", header: "gameframework/PickupSystem.h",
      summary: "拾取アイテムを指す 24bit index + 8bit gen の packed <t>ハンドル</t>。0 = invalid。slot を再利用しても古い handle は generation 不一致で無効になる。",
      when: "<code>Spawn()</code> の戻り値として持ち、Despawn や <code>GetPickup()</code> の対象指定に使う。",
      sample: "PickupId id = ps.Spawn(info);\nif (const PickupInfo* p = ps.GetPickup(id)) { /* 生存中 */ }"
    },
    {
      name: "EPickupKind",
      kind: "列挙(enum)", header: "gameframework/PickupSystem.h",
      summary: "拾える物の分類。<code>HealthOrb</code> / <code>ManaOrb</code> / <code>Coin</code> / <code>Gem</code> / <code>AmmoBox</code> / <code>PowerUp</code> / <code>EKey</code> / <code>Custom</code> の 8 種。数値は Save/Load で永続化される安定値 (途中に挿入しない)。",
      when: "pickup を種別ごとに扱う / 統計する / <code>SpawnRandomAt</code> で既定値を選ぶ時。",
      sample: "if (ps.CountOfKind(EPickupKind::Coin) == 0) SpawnWave();"
    },
    {
      name: "TPool&lt;T&gt;",
      kind: "クラステンプレート", header: "gameframework/Pool.h",
      summary: "短命オブジェクトを毎フレーム大量に作っては捨てる用途の<t>オブジェクトプール</t>。<b>固定容量</b>で、要素 T は最初に全部 default 構築しておき、<code>m_Active[i]</code> の 0/1 で使用中かを表すだけ。<code>new</code>/<code>delete</code> を避けて<t>GC ヒッチ</t>や断片化を防ぐ。",
      when: "弾・パーティクル・一時的な敵など、生成と破棄が高頻度なものを軽く取り回したい時。Acquire / Release では T のコンストラクタ / デストラクタは走らないので、再初期化は自分で行う。",
      sample: "struct Bullet { f32 x, y, vx, vy; bool alive; };\nTPool&lt;Bullet&gt; bullets;\nbullets.Init(1024);\nif (Bullet* b = bullets.Acquire()) {\n    b-&gt;x = px; b-&gt;y = py; b-&gt;vx = vx; b-&gt;alive = true;\n}\nbullets.ForEachActive([dt](Bullet&amp; b) {\n    b.x += b.vx * dt;\n});",
      members: [
        { sig: "void Init(u32 capacity)", desc: "固定容量を予約 (T は default 構築される)。2 度目以降は no-op (固定容量ポリシー)。" },
        { sig: "T* Acquire()", ret: "空きスロット or nullptr", desc: "空き 1 個を確保して返す。満杯なら nullptr。中身は前回値が残るので再初期化すること。返ったポインタは ResetAll / 破棄まで安定。" },
        { sig: "void Release(T* p)", desc: "p をプールに返す。nullptr / 範囲外ポインタ / 二重 Release は no-op (安全)。" },
        { sig: "void ResetAll()", desc: "全 active を一括解放 (中身の T は触らない)。" },
        { sig: "void ForEachActive(F&amp;&amp; fn)", desc: "active な要素だけ <code>fn(T&amp;)</code> で巡回。update / draw 用。fn 内での Acquire/Release は未定義動作。" },
        { sig: "u32 ActiveCount() const", desc: "使用中の数。<code>Capacity()</code> は容量。" }
      ]
    },
    {
      name: "FPrefabSystem",
      kind: "クラス", header: "gameframework/PrefabSystem.h",
      summary: "名前付きの Node ツリーテンプレート (<b>Prefab</b>) を<t>ファクトリ関数</t>として登録し、ID か名前から <code>TUniquePtr&lt;FNode2D&gt;</code> を生成 (spawn) する軽量レジストリ。Unity の Prefab / Unreal の Blueprint asset に相当する役割を、ファイルではなく関数で表現する。",
      when: "「敵」「コイン」「弾」などの組み立て済みオブジェクトを 1 度登録しておき、ゲーム中に何度も spawn したい時。",
      sample: "static TUniquePtr&lt;FNode2D&gt; SpawnEnemy(void*) noexcept {\n    auto n = MakeUnique&lt;FNode2D&gt;();\n    // 子ノード / コンポーネントを組み立てる\n    return n;\n}\nFPrefabSystem prefabs;\nPrefabId id = prefabs.Register(\"Enemy\", &amp;SpawnEnemy);\nauto a = prefabs.Spawn(id);\nauto b = prefabs.SpawnByName(\"Enemy\");",
      members: [
        { sig: "PrefabId Register(const char* name, PrefabFactoryFn factory, void* user_data = nullptr)", ret: "新規 ID (失敗時 invalid)", desc: "Prefab を登録。name は永続文字列を渡す (複製しない)。nullptr / 空文字 / factory==nullptr は弾く。同名でも別 ID を作る (上書きしない)。" },
        { sig: "PrefabId FindByName(const char* name) const", desc: "名前で検索 (登録順で最初に一致したもの)。一致しなければ invalid。" },
        { sig: "TUniquePtr&lt;FNode2D&gt; Spawn(PrefabId id)", ret: "新しい Node ツリー", desc: "ID から factory を 1 回呼んで生成。invalid / stale / factory==nullptr なら空を返す。" },
        { sig: "TUniquePtr&lt;FNode2D&gt; SpawnByName(const char* name)", desc: "名前から spawn (FindByName → Spawn 相当)。" },
        { sig: "bool Unregister(PrefabId id)", desc: "登録解除。slot 再利用 + generation+1 で古い handle を無効化。実際に解除したか返す。" },
        { sig: "const char* GetName(PrefabId id) const", desc: "デバッグ用の名前取得。invalid / stale なら <code>\"(unknown)\"</code> (nullptr は返さない)。" },
        { sig: "u32 Count() const", desc: "現在 active な登録数。<code>ClearAll()</code> で全破棄。" }
      ]
    },
    {
      name: "PrefabId",
      kind: "構造体", header: "gameframework/PrefabSystem.h",
      summary: "Prefab を指す 24bit index + 8bit gen の packed <t>ハンドル</t>。<code>m_Packed == 0</code> が invalid。<t>FNodeId</t> と完全に同じパターン。",
      when: "<code>Register()</code> の戻り値として持ち、<code>Spawn()</code> / <code>Unregister()</code> の対象指定に使う。",
      sample: "PrefabId id = prefabs.Register(\"Coin\", &amp;SpawnCoin);\nif (id.IsValid()) auto c = prefabs.Spawn(id);"
    },
    {
      name: "PrefabFactoryFn",
      kind: "関数ポインタ型", header: "gameframework/PrefabSystem.h",
      summary: "Prefab を実体化するファクトリ関数の型 = <code>TUniquePtr&lt;FNode2D&gt;(*)(void* user_data) noexcept</code>。<code>user_data</code> は Register 時に渡したコンテキスト (<t>クロージャ</t>の代替)。失敗時は空 <code>TUniquePtr</code> を返してよい。",
      when: "<code>Register()</code> に渡す自前の生成関数を書く時のシグネチャ。",
      sample: "static TUniquePtr&lt;FNode2D&gt; Make(void* user) noexcept {\n    auto* cfg = static_cast&lt;Config*&gt;(user);\n    return BuildNode(cfg);\n}"
    },
    {
      name: "FPrivacyDirector",
      kind: "クラス", header: "gameframework/PrivacyDirector.h",
      summary: "GDPR / CCPA / COPPA 対応で必須となる「ユーザーの同意」を一元管理する director。解析・広告・テレメトリなどカテゴリ別に同意の on/off を持ち、初回ダイアログ判定、ポリシー版の追跡、ローカル永続化を提供する。",
      when: "解析や広告 SDK を呼ぶ前に「ユーザーが同意したか」を確認する仕組みを作る時。カテゴリ別の granular consent が法的に求められる場面で使う。",
      sample: "FPrivacyDirector privacy;\nprivacy.Init(/*current_policy_version=*/2);\nprivacy.LoadConsent(L\"user/consent.bin\");\nif (privacy.RequiresInitialConsent() || privacy.IsPolicyOutdated()) {\n    privacy.GrantConsent(EConsentCategory::Analytics);\n    privacy.MarkInitialConsentShown();\n    privacy.SaveConsent(L\"user/consent.bin\");\n}\nif (privacy.HasConsent(EConsentCategory::Analytics)) SendEvent();",
      members: [
        { sig: "void Init(u32 current_policy_version = 1)", desc: "有効なプライバシーポリシー版を設定。保存済み版がこれより小さければ <code>IsPolicyOutdated()</code> が true になる。" },
        { sig: "void GrantConsent(EConsentCategory cat)", desc: "指定カテゴリの同意 bit を立てる (複合可)。<code>RevokeConsent</code> は落とす。" },
        { sig: "void GrantAll()", desc: "Required を除く全カテゴリを ON (= \"Accept All\")。<code>RevokeAll()</code> は全 OFF。" },
        { sig: "bool HasConsent(EConsentCategory cat) const", desc: "cat の全 bit が立っているか。Required (=0) は常に true。" },
        { sig: "bool RequiresInitialConsent() const", desc: "まだ同意ダイアログを 1 度も提示していなければ true。<code>MarkInitialConsentShown()</code> で false 化。" },
        { sig: "bool IsPolicyOutdated() const", desc: "ロード済み policy_version が現在より古ければ true (再同意を促す)。" },
        { sig: "TResult&lt;void&gt; SaveConsent(const wchar_t* file_path)", desc: "現在の同意状態を <code>.acssave</code> バイナリで保存。<code>LoadConsent</code> は復元 (ファイル無しは初回起動扱いで Ok)。" },
        { sig: "void Reset()", desc: "初期化前の状態に戻す (テスト / デバッグ用)。" }
      ]
    },
    {
      name: "EConsentCategory",
      kind: "列挙(enum)", header: "gameframework/PrivacyDirector.h",
      summary: "同意カテゴリを表す<t>ビットフラグ</t>。<code>Required</code> (=0、同意不要の基本機能) / <code>Analytics</code> / <code>Marketing</code> / <code>Personalization</code> / <code>ThirdPartySharing</code> / <code>Telemetry</code> / <code>CrashReports</code>。<code>|</code> / <code>&amp;</code> 演算子で複合できる。",
      when: "「Analytics は ON だが Marketing は OFF」のようにカテゴリ別に同意を扱う時。",
      sample: "privacy.GrantConsent(EConsentCategory::Analytics | EConsentCategory::Telemetry);"
    },
    {
      name: "ConsentStatus",
      kind: "構造体", header: "gameframework/PrivacyDirector.h",
      summary: "永続化単位の POD。<code>granted_mask</code> (同意 mask) / <code>consent_timestamp</code> (同意時刻) / <code>policy_version</code> (同意時のポリシー版)。<code>SaveConsent</code> / <code>LoadConsent</code> で書き出される最小構造体。",
      when: "同意情報をファイルに保存 / 復元する時の中身。基本は <code>FPrivacyDirector</code> 越しに扱う。",
      sample: "// SaveConsent/LoadConsent が内部で読み書きする POD。\n// 直接組み立てる必要はほぼない。"
    },
    {
      name: "FPauseDirector",
      kind: "クラス", header: "gameframework/PauseDirector.h",
      summary: "ゲーム全体の pause 状態を一元管理する director。pause 理由を<t>ビットフラグ</t>(<code>EPauseReason</code>) の OR で持ち、「全部の理由が 0 になるまで pause を続ける」<t>スタック</t>的な挙動にすることで、「メニュー閉じたらフォーカス喪失中なのに動き出した」系のバグを防ぐ。",
      when: "メニュー表示・OS メニュー・フォーカス喪失・カットシーンなど複数の pause 理由が同時に起きうるゲームで、安全に pause/resume したい時。",
      sample: "FPauseDirector pause;\npause.Pause(EPauseReason::UserMenu);\ngame.SetTimeScale(pause.EffectiveTimeScale());   // 0.0f\npause.Pause(EPauseReason::SystemMenu);\npause.Resume(EPauseReason::UserMenu);            // まだ SystemMenu が残る\ngame.SetTimeScale(pause.EffectiveTimeScale());   // まだ 0.0f\npause.Resume(EPauseReason::SystemMenu);\ngame.SetTimeScale(pause.EffectiveTimeScale());   // 1.0f に復帰",
      members: [
        { sig: "void Pause(EPauseReason reason)", desc: "指定理由の bit を立てる (OR、複合可)。新たに立った bit ごとに callback が paused=true で呼ばれる。" },
        { sig: "void Resume(EPauseReason reason)", desc: "指定理由の bit を落とす。実際に落ちた bit ごとに callback が paused=false で呼ばれる。" },
        { sig: "bool IsPaused() const", desc: "1 つでも理由が立っていれば true。<code>IsPausedFor(reason)</code> は特定理由の全 bit が立っているか。" },
        { sig: "EPauseReason ActiveReasons() const", desc: "現在立っている全理由の bit OR (デバッグ表示 / 永続化用)。" },
        { sig: "void Clear()", desc: "全理由を一気に落とす (立っていた各 bit について callback 発火)。タイトルに戻る時など。" },
        { sig: "f32 EffectiveTimeScale() const", ret: "0 or NormalTimeScale", desc: "呼び出し側が <code>SetTimeScale</code> に渡す値。pause 中は 0、非 pause は <code>NormalTimeScale()</code>。" },
        { sig: "void SetNormalTimeScale(f32 s)", desc: "pause 解除時に戻る scale。スローモーション演出 (0.5x など) と pause を直交管理。負値は 0 に clamp。" },
        { sig: "void SetCallback(PauseEventCallback cb, void* user)", desc: "pause/resume 遷移時に呼ばれる関数ポインタを登録 (cb=nullptr で解除)。UI 更新やオーディオ ducking に使う。" }
      ]
    },
    {
      name: "EPauseReason",
      kind: "列挙(enum)", header: "gameframework/PauseDirector.h",
      summary: "pause 理由を表す<t>ビットフラグ</t>。<code>None</code> (=0、pause していない特異値) / <code>UserMenu</code> / <code>SystemMenu</code> / <code>FocusLost</code> / <code>Cinematic</code> / <code>FPhotoMode</code> / <code>NetworkSync</code> / <code>Custom1</code> / <code>Custom2</code>。<code>|</code> / <code>&amp;</code> 演算子付き。",
      when: "なぜ pause しているか / させたいかを表す時。複数同時に立てられる。",
      sample: "pause.Pause(EPauseReason::FocusLost);\nif (pause.IsPausedFor(EPauseReason::Cinematic)) HideHUD();"
    },
    {
      name: "PauseEventCallback",
      kind: "関数ポインタ型", header: "gameframework/PauseDirector.h",
      summary: "pause/resume 遷移時に発火する関数ポインタ型 = <code>void(*)(void* user, EPauseReason reason, bool paused) noexcept</code>。立った / 落ちた reason の bit ごとに個別に呼ばれ、<code>paused</code> が遷移方向 (true=pause へ / false=resume へ)。",
      when: "pause 状態の変化に合わせて UI を更新したり、音をフェードさせたりしたい時のコールバック。",
      sample: "void OnPause(void* u, EPauseReason r, bool paused) noexcept {\n    if (paused) AudioDuck(); else AudioRestore();\n}\npause.SetCallback(&amp;OnPause, this);"
    },
    {
      name: "FProgression",
      kind: "クラス", header: "gameframework/Progression.h",
      summary: "「累計 XP を加算するとレベルが上がり、所定の XP 閾値で Milestone を達成 → コンテンツが Unlock される」進行システムを 1 クラスにまとめた小型マネージャ。レベルは <code>floor(log2(xp + 1))</code> で典型的な RPG カーブを近似。プラットフォーム SDK 非依存。",
      when: "敵撃破やクエスト完了で XP が貯まり、一定値で「Lv.10 到達で新エリア解放」のようなマイルストーンを達成させたい時。達成時に callback でゲーム側へ通知する。",
      sample: "FProgression p;\np.RegisterMilestone({ \"ms.level_5\", \"Lv.5 到達\", 31, \"content.weapon_b\" });\np.RegisterMilestone({ \"ms.level_10\", \"Lv.10 到達\", 1023, \"content.area_2\" });\np.SetOnAchievedCallback(&amp;OnMilestoneAchieved, &amp;game);\np.AwardXp(50);\nu32 lv = p.CurrentLevel();\nu32 done = p.AchievedCount();",
      members: [
        { sig: "void RegisterMilestone(const MilestoneDef&amp; def)", desc: "起動時に 1 度ずつ登録。同 id の 2 重登録 / id==nullptr は no-op。" },
        { sig: "void AwardXp(u32 amount)", desc: "累計 XP に加算し、各 milestone の達成判定を行う。u32 超過は max にクランプ。amount=0 は no-op。" },
        { sig: "u32 CurrentXp() const", desc: "現在の累計 XP。<code>CurrentLevel()</code> は <code>floor(log2(xp+1))</code> (xp=1→Lv1, xp=7→Lv3, ...)。" },
        { sig: "bool IsMilestoneAchieved(const char* id) const", desc: "指定 id が達成済みか。nullptr / 未登録は false。" },
        { sig: "u32 MilestoneCount() const", desc: "登録総数。<code>AchievedCount()</code> は達成数。UI の \"5/20 unlocked\" 表示に。" },
        { sig: "const MilestoneState* AllStates(u32&amp; out_count) const", ret: "状態配列", desc: "全 milestone 状態の生バッファ。out_count に件数。RegisterMilestone / ResetProgress で無効化される。" },
        { sig: "void SetOnAchievedCallback(MilestoneCallback cb, void* user)", desc: "達成時 callback を登録 / 差し替え / 解除。AwardXp 内で「未達成→達成」遷移ごとに 1 回呼ばれる。" },
        { sig: "void ResetProgress()", desc: "累計 XP=0、全 milestone を未達成に戻す (定義は保持)。NewGame+ / デバッグ用。" },
        { sig: "TResult&lt;void&gt; Save(const wchar_t* file_path)", desc: "累計 XP + 達成済み milestone を <code>FSaveArchive</code> バイナリ (CRC32 付き) で保存。<code>Load</code> は同じ Def 群を登録後に呼ぶと復元 (id の hash でマッチ)。" }
      ]
    },
    {
      name: "MilestoneDef / MilestoneState",
      kind: "構造体", header: "gameframework/Progression.h",
      summary: "<code>MilestoneDef</code> = 起動時に登録する immutable な定義 (<code>id</code> / <code>display_name</code> / <code>required_xp</code> / <code>unlock_content_id</code>)。<code>MilestoneState</code> = 実行時の達成状態 (<code>id</code> / <code>achieved</code> / <code>achieved_timestamp</code>)。同 index で 1:1 対応する。文字列は<t>非所有</t>。",
      when: "<code>RegisterMilestone()</code> に渡す定義、および <code>AllStates()</code> / <code>GetState()</code> で読む状態のデータ単位。",
      sample: "MilestoneDef d{ \"ms.veteran\", \"Veteran\", 16383, \"content.title_x\" };\np.RegisterMilestone(d);\nif (const MilestoneState* s = p.GetState(\"ms.veteran\"))\n    if (s-&gt;achieved) ShowBadge();"
    },
    {
      name: "MilestoneCallback",
      kind: "関数ポインタ型", header: "gameframework/Progression.h",
      summary: "マイルストーン達成時に 1 回だけ呼ばれる関数ポインタ型 = <code>void(*)(void* user, const char* milestone_id) noexcept</code>。<code>milestone_id</code> は登録時の id がそのまま渡る。",
      when: "達成の瞬間に効果音を鳴らす / トースト UI を出す / Unlock 処理に橋渡しする時。",
      sample: "void OnAchieved(void* u, const char* id) noexcept {\n    PlaySe(\"se.unlock\");\n    ShowToast(id);\n}\np.SetOnAchievedCallback(&amp;OnAchieved, &amp;game);"
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "世代付きハンドル": "オブジェクトを直接ポインタで指す代わりに「配列の index + 世代カウンタ」で指す仕組み。slot が再利用されても世代が食い違うので、破棄済みの古いハンドルを安全に検出できる。",
  "stale": "「古くなって無効になった」状態。例えば破棄された Node を指したままのハンドルが stale。<t>世代付きハンドル</t>では世代不一致で検出する。",
  "ダングリング": "破棄済みのメモリを指したままのポインタ。触ると<t>未定義動作</t>。これを避けるため ACS は<t>世代付きハンドル</t>を多用する。",
  "非所有": "そのポインタ/参照は対象を所有しておらず、寿命管理 (解放) は別の誰かが行うこと。non-owning。",
  "seam": "実装を差し替えられるように設けた「継ぎ目」。インターフェースだけを公開し、具象 (実 SDK 等) を後から注入する。",
  "フォールバック": "本命の機能が使えない時に代わりに使う退避経路。例: HMD 未接続なら通常の 2D 描画に切り替える。",
  "headless": "画面 (描画) を持たずに動かせること。テストや CI で見た目なしにロジックだけ検証できる。",
  "パーティクル": "炎・煙・火花などを表現する大量の小さな粒子。1 つ 1 つが位置・速度・寿命を持って動く。",
  "self-contained": "自分が動くのに必要なデータを全部自分で持っていて、外部の状態に依存しないこと。",
  "ビヘイビアツリー": "AI の意思決定を木構造で表す手法。葉ノードが具体的な行動や条件判定 (例: 知覚) になる。",
  "A*": "(エースター) グリッドやグラフ上で最短経路を効率よく求める定番アルゴリズム。実コスト + ゴールまでの推定値で探索する。",
  "kinematic": "自分で速度を持って動くが、他の物体から押し返されない物理ボディ。剛体シミュレーションより軽く、プラットフォーマー等の自機向き。",
  "collide-and-slide": "壁にぶつかった時、止まるのではなく壁に沿って滑るように動く衝突応答。キャラ移動の自然さに効く。",
  "コンポーネント": "Node に付ける部品。1 つの機能 (物理・描画・当たり判定など) を担い、組み合わせて挙動を作る。",
  "オブジェクトプール": "オブジェクトを毎回 new/delete せず、あらかじめ確保した固定数を使い回す手法。確保コストと断片化を抑える。",
  "GC ヒッチ": "メモリ確保 / 解放やガベージコレクションが原因で一瞬だけフレームが詰まること。プールはこれを避けるために使う。",
  "ファクトリ関数": "オブジェクトを組み立てて返す関数。Prefab ではこの関数を登録しておき、必要な時に呼んで実体を作る。",
  "クロージャ": "周囲の変数を一緒に持ち運べる関数オブジェクト。ACS は STL を使わないので、代わりに関数ポインタ + <code>void* user_data</code> で文脈を渡す。",
  "ビットフラグ": "各 bit に意味を割り当て、OR で複数の状態を 1 個の整数にまとめて持つ手法。pause 理由や同意カテゴリの複合表現に使う。",
  "状態 holder": "状態 (フラグや値) を保持するだけのクラス。実際の処理 (時間停止や描画) は呼び出し側が行う、という責務分離の設計。",
  "poll-and-consume": "フラグを立てておき、受け取った側が確認すると同時に落とす方式。1 回の操作が 1 回だけ処理されるのを保証する。",
  "ハンドル": "実体を間接的に指す軽い識別子 (多くは整数)。ポインタの代わりに使い、安全な再利用や stale 検出を可能にする。"
});
