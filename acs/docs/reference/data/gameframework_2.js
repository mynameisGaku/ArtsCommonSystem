/* ACS リファレンス — gameframework モジュール (acs::game) パート 2 (C 〜 D 系)。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "gameframework", order: 41, title: "gameframework — ゲーム部品 (acs::game)",
  blurb: "ゲームを作るときに「だいたいどんなゲームでも要る」部品を、すぐ使える形でまとめたモジュール。チェックポイント・クールダウン・カードのデッキ・クラフト・当たり判定・デバッグ表示などを、描画や音とは切り離した小さな<t>マネージャ</t>として提供します。",
  types: [
    {
      name: "FCharacterCustomizer",
      kind: "クラス", header: "gameframework/CharacterCustomizer.h",
      summary: "プレイヤーキャラの<b>見た目 (cosmetic)</b> だけを slot 単位で着せ替えするマネージャ。帽子・服・武器などを <code>ECosmeticSlot</code> ごとに 1 つずつ装着する。性能 (戦闘パラメータ) は一切いじらない設計で、<t>pay-to-win</t> を構造的に避ける。",
      when: "アバターの着せ替え / スキン選択 UI を作る時。アンロック済みの cosmetic から装着を選ばせ、装着が変わったら<t>callback</t>でレンダラに通知する。",
      sample: "FCharacterCustomizer cc;\ncc.RegisterCosmetic({ \\\"hat.red\\\", \\\"Red Cap\\\", ECosmeticSlot::Head, \\\"art/hat.fbx\\\", false, \\\"common\\\" });\ncc.UnlockCosmetic(\\\"hat.red\\\");\ncc.SetOnEquipCallback(&OnEquip, &renderer);\ncc.EquipCosmetic(\\\"hat.red\\\");  // Head slot に装着 → callback 発火",
      members: [
        { sig: "void RegisterCosmetic(const FCosmeticItem& item)", desc: "起動時に cosmetic 定義を 1 つ登録する。同 id の 2 重登録 / nullptr は無視。", when: "ゲーム開始時にすべての見た目アイテムをまとめて登録する。" },
        { sig: "bool UnlockCosmetic(const char* id)", ret: "新規 unlock したか", desc: "指定 id を入手済みにする。既に unlock 済 / 未登録なら false。", when: "ストア購入やクエスト報酬で見た目を解放した時 (検証は呼出側で済ませてから通知)。" },
        { sig: "bool IsUnlocked(const char* id) const", desc: "その cosmetic が入手済みかを返す。UI のグレーアウト判定に使う。" },
        { sig: "bool EquipCosmetic(const char* id)", ret: "装着できたか", desc: "該当 slot に装着する。同 slot の既存装着は自動で外れる。未登録 / 未 unlock は false。", when: "プレイヤーが着せ替え UI で選んだ時。" },
        { sig: "void UnequipSlot(ECosmeticSlot slot)", desc: "指定 slot の装着を外す (item_id=nullptr で callback 発火)。空なら何もしない。" },
        { sig: "const char* EquippedInSlot(ECosmeticSlot slot) const", ret: "装着中 id or null", desc: "今その slot に着けている cosmetic の id。未装着なら nullptr。" },
        { sig: "u32 UnlockedCount() const", ret: "unlock 済み件数", desc: "\\\"23/120 unlocked\\\" のような進捗表示に使う。" },
        { sig: "u32 CountInSlot(ECosmeticSlot slot) const", desc: "指定 slot に登録されている選択肢の件数 (装着状態とは無関係)。" },
        { sig: "void SetOnEquipCallback(EquipCallback cb, void* user)", desc: "装着 / 解除のたびに呼ばれる callback を登録 (単一購読)。レンダラ側の mesh 差し替え用。" },
        { sig: "void ClearAll()", desc: "登録・unlock・装着をすべてリセット (Save/Load 復元前のクリーンスタート用)。callback は呼ばない。" }
      ]
    },
    {
      name: "FCosmeticItem",
      kind: "構造体", header: "gameframework/CharacterCustomizer.h",
      summary: "1 つの cosmetic (見た目アイテム) の定義。id・表示名・装着 slot・アセットパス・課金フラグ・レアリティラベルを持つ。文字列はすべて<t>非所有</t> (リテラル想定)。",
      when: "<code>RegisterCosmetic()</code> に渡す時。",
      sample: "FCosmeticItem item;\nitem.id = \\\"skin.gold\\\";\nitem.display_name = \\\"Gold Skin\\\";\nitem.slot = ECosmeticSlot::Body;\nitem.asset_path = \\\"art/skin_gold.fbx\\\";\nitem.is_premium = true;\nitem.rarity = \\\"rare\\\";"
    },
    {
      name: "ECosmeticSlot",
      kind: "列挙(enum)", header: "gameframework/CharacterCustomizer.h",
      summary: "見た目を装着する位置。Head / Body / Hands / Legs / Feet / MainHand / OffHand / Backpack / Pet / Mount / ColorPalette の 11 種で固定。各 slot に最大 1 つ装着できる。",
      when: "装着 slot を指定する時。<code>ColorPalette</code> は色変更用の特殊 slot。",
      sample: "if (cc.EquippedInSlot(ECosmeticSlot::Head)) { /* 帽子を着けている */ }"
    },
    {
      name: "FCheckpointSystem",
      kind: "クラス", header: "gameframework/CheckpointSystem.h",
      summary: "プラットフォーマー等の<b>チェックポイント (復活ポイント)</b> を管理するマネージャ。配置済み checkpoint を id で識別し、現在 active な 1 つを保持。死亡時に <code>TriggerRespawn()</code> で復活先の座標と level index を引き出す。",
      when: "「死んだら直近のチェックポイントに戻る」を実装する時。HP システムの死亡 callback から TriggerRespawn を叩くのが定番。",
      sample: "FCheckpointSystem cps;\nFCheckpointInfo cp{}; cp.id = \\\"cp.start\\\"; cp.spawn_pos = {100, 200};\ncps.Register(cp);\ncps.ActivateCheckpoint(\\\"cp.start\\\");  // チェックポイントに触れた\nFVec2 pos; u32 lv;\nif (cps.TriggerRespawn(pos, lv)) { /* pos へ復活 */ }",
      members: [
        { sig: "FCheckpointId Register(const FCheckpointInfo& info)", ret: "新しい handle", desc: "checkpoint を 1 つ配置登録する。同 id の 2 重登録 / id==nullptr は invalid を返す。", when: "レベルロード時に全 checkpoint をまとめて登録。" },
        { sig: "void Unregister(FCheckpointId id)", desc: "checkpoint を解除する。slot は再利用され generation が進む。active を消すと active は invalid 化。" },
        { sig: "bool ActivateCheckpoint(const char* checkpoint_id)", ret: "切替できたか", desc: "指定 checkpoint を「現在地点」にする。one_way 違反 / 未 unlock / 未登録は false。同 id が既に active なら true (再発火なし)。", when: "プレイヤーがチェックポイントトリガに触れた時。", sample: "cps.ActivateCheckpoint(\\\"cp.mid\\\");" },
        { sig: "void UnlockCheckpoint(const char* checkpoint_id)", desc: "requires_unlock な隠し checkpoint を解放する。", when: "隠しスイッチを踏んだ時や DLC アンロック時。" },
        { sig: "bool IsUnlocked(const char* checkpoint_id) const", desc: "その checkpoint が利用可能か。requires_unlock=false な定義は常に true。" },
        { sig: "bool TriggerRespawn(FVec2& out_pos, u32& out_level_index) const", ret: "active があったか", desc: "死亡時の復活先座標と level index を書き出す。active が無ければ false。成功時に RespawnCallback 発火。", when: "プレイヤー死亡時。", sample: "FVec2 p; u32 lv;\nif (cps.TriggerRespawn(p, lv)) MovePlayer(p);" },
        { sig: "FCheckpointId CurrentCheckpoint() const", ret: "現 active handle", desc: "今 active な checkpoint。一度も Activate されていなければ invalid。" },
        { sig: "u32 LastSpawnLevelIndex() const", desc: "直近に active 化された checkpoint の level index。" },
        { sig: "void SetOnActivateCallback(ActivateCallback cb, void* user)", desc: "Activate 成功時に呼ばれる callback (UI トースト / SE 用)。" },
        { sig: "void SetOnRespawnCallback(RespawnCallback cb, void* user)", desc: "TriggerRespawn 成功時に呼ばれる callback。" },
        { sig: "void ClearAll()", desc: "全 checkpoint / unlock / active 状態を消去。callback 設定は維持。" }
      ]
    },
    {
      name: "FCheckpointId",
      kind: "構造体", header: "gameframework/CheckpointSystem.h",
      summary: "チェックポイントを指す軽い<t>ハンドル</t>。32bit に 24bit の index と 8bit の<t>世代 (generation)</t> を詰めてある。解除後に slot が再利用されても、古いハンドルは世代不一致で弾かれる。0 は invalid。",
      when: "checkpoint を handle 経由で扱う時 (id 文字列の代わり)。",
      sample: "FCheckpointId id = cps.Register(info);\nif (id.IsValid()) cps.ActivateCheckpoint(id);"
    },
    {
      name: "FCheckpointInfo",
      kind: "構造体", header: "gameframework/CheckpointSystem.h",
      summary: "1 つの checkpoint の定義。id・復活座標・level_index・sort_order・one_way (戻れなくする) ・requires_unlock (隠し) を持つ。",
      when: "<code>Register()</code> に渡す時。",
      sample: "FCheckpointInfo cp{};\ncp.id = \\\"cp.boss\\\";\ncp.spawn_pos = {500, 200};\ncp.sort_order = 1;\ncp.one_way = true;        // ここまで来たら戻れない\ncp.requires_unlock = false;"
    },
    {
      name: "FCinematicsDirector",
      kind: "クラス", header: "gameframework/CinematicsDirector.h",
      summary: "時間軸に並べた<t>キーフレーム</t>を順に発火していく<b>カットシーン driver</b>。「0 秒で BGM」「2 秒でカメラ移動」「3 秒でセリフ」を 1 本のタイムラインとして宣言的に組める。自分では描画も音も出さず、種別ごとの callback を呼ぶだけ。",
      when: "オープニング・ボス導入・イベントムービー等の演出を時系列で組み立てる時。",
      sample: "FCinematicsDirector cine;\nFTimelineKeyframe kf;\nkf.time_sec = 2.0f; kf.kind = ETimelineTrackKind::MoveCamera;\nkf.payload.camera = { FVec2{100,200}, 1.5f, 3.0f };\ncine.AddKeyframe(kf);\ncine.SetCameraCallback(&DoMoveCamera, this);\ncine.Play();\n// 毎フレーム: cine.Tick(dt);",
      members: [
        { sig: "void AddKeyframe(const FTimelineKeyframe& kf)", desc: "キーフレームを 1 つ追加する。内部で time_sec の昇順に保たれる。", when: "シーンに入った時にまとめて keyframe を積む。" },
        { sig: "void Play()", desc: "先頭から再生開始 (Stop されていれば 0 秒から、Pause からは続き)。" },
        { sig: "void Pause()", desc: "一時停止 (時刻と発火位置は保持)。" },
        { sig: "void Stop()", desc: "完全停止して 0 秒に戻す。" },
        { sig: "void Skip()", desc: "残り全 keyframe を即座に順番に発火し、末尾まで進める。", when: "「ボタンでスキップ」する時。" },
        { sig: "void Tick(f32 dt)", desc: "dt 秒進める。到達した keyframe を時刻順に発火する。再生中でない / dt<=0 は何もしない。", when: "毎フレーム呼ぶ。" },
        { sig: "bool IsFinished() const", desc: "全 keyframe を発火し終わったか。" },
        { sig: "f32 TotalDuration() const", ret: "総尺 [秒]", desc: "最後の keyframe の time_sec。何秒で終わるかを知る用。" },
        { sig: "void SetCameraCallback(CameraCallbackFn cb, void* user)", desc: "MoveCamera keyframe 発火時の callback。target_pos / zoom / duration を受け取る。" },
        { sig: "void SetDialogueCallback(DialogueCallbackFn cb, void* user)", desc: "ShowDialogue keyframe 発火時の callback。line_id を受け取る。" },
        { sig: "void SetMusicCallback(MusicCallbackFn cb, void* user)", desc: "PlayMusic keyframe 発火時の callback。music_id / fade を受け取る。" },
        { sig: "void SetEventCallback(EventCallbackFn cb, void* user)", desc: "FireEvent keyframe 発火時の callback。汎用 event_id を受け取る (フラグ立て等)。" },
        { sig: "void Clear()", desc: "全 keyframe と状態を破棄する。" }
      ]
    },
    {
      name: "FTimelineKeyframe",
      kind: "構造体", header: "gameframework/CinematicsDirector.h",
      summary: "タイムライン上の 1 つのキーフレーム。発火時刻 (time_sec) + 種別 (kind) + 種別ごとの payload (<t>union</t>) を持つ。kind に対応する payload フィールドだけが意味を持つ。",
      when: "<code>AddKeyframe()</code> に渡す時。",
      sample: "FTimelineKeyframe kf;\nkf.time_sec = 3.0f;\nkf.kind = ETimelineTrackKind::ShowDialogue;\nkf.payload.dialogue = { \\\"line_intro_001\\\" };"
    },
    {
      name: "ETimelineTrackKind",
      kind: "列挙(enum)", header: "gameframework/CinematicsDirector.h",
      summary: "キーフレームが何を起こすか。Wait (何もしない時間マーカー) / MoveCamera / ShowDialogue / PlayMusic / FireEvent の 5 種。",
      when: "keyframe の kind を指定する時。",
      sample: "kf.kind = ETimelineTrackKind::PlayMusic;"
    },
    {
      name: "FSceneClock",
      kind: "クラス", header: "gameframework/Clock.h",
      summary: "シーン単位の<b>時間トラッカ</b>。実時間とは別に、pause / slow-mo / 倍速を反映した「シーンが感じる論理時間」を刻む。<t>time_scale</t> を変えるだけでスローモーや早送りが作れる。",
      when: "ポーズ可能なゲームや、スローモー演出・スピードラン倍速モードで、Tween やタイマーに渡す時間を一元管理したい時。",
      sample: "FSceneClock clock;\nclock.SetTimeScale(0.5f);   // スローモー\nvoid OnUpdate(f32 dt) {\n    clock.Tick(dt);\n    UpdateTweens(clock.Dt());  // scaled な dt を渡す\n}",
      members: [
        { sig: "void Tick(f32 dt)", desc: "dt 秒を流す。pause 中は scaled 系が止まり unscaled 系のみ進む。", when: "毎フレーム呼ぶ。" },
        { sig: "void Pause() / void Resume()", desc: "一時停止 / 再開。pause 中は Dt() が 0 になる。" },
        { sig: "f32 Dt() const", ret: "scaled dt", desc: "time_scale と pause を反映した最後の dt。Tween 等にはこれを渡す。" },
        { sig: "f32 DtUnscaled() const", desc: "倍率も pause も無視した生の dt。UI アニメ等の「常に進めたい」もの用。" },
        { sig: "f32 Time() const", ret: "累計 scaled 時間", desc: "開始からの scaled な累積秒。" },
        { sig: "u64 Frame() const", desc: "Tick した回数 (フレーム数)。" },
        { sig: "void SetTimeScale(f32 s)", desc: "倍率を設定。0=フリーズ、1=通常、>1=早送り、<1=スロー。負値は 0 にクランプ。", sample: "clock.SetTimeScale(2.0f);  // 早送り" },
        { sig: "void Reset()", desc: "時間積算とフレーム数だけリセット (pause / time_scale は維持)。" }
      ]
    },
    {
      name: "FCollisionWorld2D",
      kind: "クラス", header: "gameframework/CollisionWorld2D.h",
      summary: "2D 当たり判定の高レベル API。AABB / 円 / 凸ポリゴン / OBB を <code>FShapeId</code> で登録し、内部の<t>空間グリッド (SpatialGrid)</t> で速い重なり検索や<t>レイキャスト</t>を提供する。レイヤ<t>ビットマスク</t>で player/enemy/wall を絞り込める。",
      when: "弾・敵・壁などの当たり判定や「この範囲に何がいる?」「この方向に何がある?」を 2D で問い合わせたい時。",
      sample: "FCollisionWorld2D world;\nworld.Init(64.0f);\nFShapeId player = world.AddCircle({ {0,0}, 16.0f });\nworld.UpdateCircle(player, { pos, 16.0f });\nTArray<FShapeId> hits;\nworld.OverlapCircle({ pos, 32.0f }, hits);  // 周囲のもの全部",
      members: [
        { sig: "void Init(f32 cell_size = 64.0f)", desc: "グリッドのセルサイズを設定。典型的には最大形状の 2〜3 倍。", when: "ワールド作成直後に 1 度呼ぶ。" },
        { sig: "FShapeId AddCircle(const FCircle& c, u32 layer = kAllLayers)", ret: "形状 handle", desc: "円を登録する。layer は所属レイヤの bitmask。", when: "形状をワールドに追加する時。AddAabb / AddPolygon / AddObb も同様。" },
        { sig: "void UpdateCircle(FShapeId id, const FCircle& c)", desc: "形状が移動したら毎フレーム更新する (dirty にして次クエリで再構築)。AABB / Poly / Obb 版あり。" },
        { sig: "void Remove(FShapeId id)", desc: "形状を削除する (slot 再利用、generation 進む)。" },
        { sig: "void OverlapCircle(const FCircle& c, TArray<FShapeId>& out, FShapeId exclude = {}, u32 mask = kAllLayers)", desc: "円と重なる全形状を out に集める。exclude で自分を除外、mask でレイヤ絞り込み。OverlapAabb / OverlapPolygon も同様。", when: "範囲内の対象を一覧したい時。" },
        { sig: "bool Raycast(const FRay2& ray, f32 max_t, FRayHit2& out_hit, FShapeId& out_id, u32 mask = kAllLayers)", ret: "当たったか", desc: "最も近い形状を 1 つ返す。out_hit に交点 / 法線 / 距離、out_id にどの形状か。", when: "視線判定・射撃・地面検出など。", sample: "FRayHit2 rh; FShapeId id;\nif (world.Raycast({org, dir}, 100.0f, rh, id)) Hit(id);" },
        { sig: "FVec2 ResolveCircle(const FCircle& c, FShapeId exclude = {}, u32 mask = kAllLayers)", ret: "押し出しベクトル", desc: "重なっている全形状から押し出す合計ベクトルを返す (重なり無しは {0,0})。<t>collide-and-slide</t> 用。", when: "kinematic body の貫通解消とスライド移動。" },
        { sig: "u32 ShapeCount() const", desc: "登録中の形状数。" },
        { sig: "void ClearAll()", desc: "全形状とグリッドを破棄する。" },
        { sig: "FVec2 ResolvePolygon(const FConvexPoly2& p, FShapeId exclude = {}, u32 mask = kAllLayers)", ret: "押し出しベクトル", desc: "凸ポリゴン版の押し出し合計ベクトル。重なり無しは {0,0}。<code>ResolveCircle</code> と同じ <t>collide-and-slide</t> 用。", when: "ポリゴン形状の kinematic body の貫通解消とスライド移動。" }
      ]
    },
    {
      name: "FShapeId",
      kind: "構造体", header: "gameframework/CollisionWorld2D.h",
      summary: "<code>FCollisionWorld2D</code> 内の形状を指す<t>ハンドル</t>。24bit index + 8bit generation の packed u32。削除済み slot を再利用しても古い handle は無効化される。0 は invalid。",
      when: "形状を更新・削除・クエリ結果として扱う時。",
      sample: "FShapeId id = world.AddAabb(box);\nif (id.IsValid()) world.UpdateAabb(id, newBox);"
    },
    {
      name: "FCombatStateMachine",
      kind: "クラス", header: "gameframework/CombatStateMachine.h",
      summary: "シーン全体の<b>戦闘フェーズ</b>を 6 状態 (Peaceful / Alert / Engaged / BossFight / Victory / Retreat) の<t>有限オートマトン</t>で追跡するディレクタ。敵検出・戦闘開始・ボス出現などを notify すると状態が遷移し、連続値の <code>ThreatLevel()</code> [0,1] も出す。",
      when: "戦況に応じて BGM や環境演出を切り替えたい時。状態が変わったら callback で FMusicDirector 等に通知する。",
      sample: "FCombatStateMachine combat;\ncombat.Init();\ncombat.SetOnStateChangeCallback(&OnCombatState, this);\ncombat.NotifyEnemyDetected(enemyId);  // Peaceful → Alert\ncombat.NotifyCombatStarted(enemyId);  // → Engaged\n// 毎フレーム: combat.Tick(dt);  // ThreatLevel が滑らかに追従",
      members: [
        { sig: "void Init()", desc: "Peaceful 状態に初期化 (callback は保持)。シーン再入時に使う。" },
        { sig: "void NotifyEnemyDetected(u32 enemy_id)", desc: "敵を検出 (Peaceful→Alert)。awareness を 1.0 に。", when: "敵がプレイヤーに気づいた / 視認した時。" },
        { sig: "void NotifyCombatStarted(u32 enemy_id)", desc: "交戦開始 (→Engaged)。その敵を is_engaged=true に。", when: "実際に戦闘が始まった時。" },
        { sig: "void NotifyCombatEnded(u32 enemy_id, bool victory)", desc: "1 敵分の戦闘解消。残敵 0 のとき victory=true なら Victory、false なら Retreat へ。", when: "敵を倒した / 戦闘から離脱した時。" },
        { sig: "void NotifyBossEncountered(u32 boss_id)", desc: "どの状態からでも BossFight に割り込み遷移する。", when: "ボスが出現した時。" },
        { sig: "void NotifyBossDefeated()", desc: "ボスを倒す。残り engaged 敵がいれば Engaged、いなければ Victory へ。" },
        { sig: "f32 ThreatLevel() const", ret: "脅威度 [0,1]", desc: "滑らかに補間された脅威レベル。BGM intensity / 画面の彩度 / カメラシェイクに流す。", when: "演出の強さを連続値で駆動したい時。" },
        { sig: "ECombatState CurrentState() const", desc: "現在の戦闘フェーズ。" },
        { sig: "bool IsInCombat() const", desc: "Engaged か BossFight か。fast-travel 禁止やセーブ抑制の判定に。" },
        { sig: "u32 EngagedEnemyCount() const", desc: "実際に交戦中の敵数。" },
        { sig: "void Tick(f32 dt)", desc: "ThreatLevel を目標値へ指数減衰で追従させる。Engaged 中は時間ドリフトを加算。", when: "毎フレーム呼ぶ。" },
        { sig: "void SetOnStateChangeCallback(StateChangeCallback cb, void* user)", desc: "状態遷移時に呼ばれる callback。from / to を受け取る。" }
      ]
    },
    {
      name: "ECombatState",
      kind: "列挙(enum)", header: "gameframework/CombatStateMachine.h",
      summary: "戦闘フェーズ。Peaceful (平穏) / Alert (検出済・未交戦) / Engaged (交戦中) / BossFight / Victory / Retreat の 6 種。値は安定なので save / replay に書ける。",
      when: "現在の戦況で分岐する時。",
      sample: "if (combat.CurrentState() == ECombatState::BossFight) PlayBossMusic();"
    },
    {
      name: "FEnemyAwareness",
      kind: "構造体", header: "gameframework/CombatStateMachine.h",
      summary: "1 体の敵の認識情報。enemy_id・awareness_level [0,1] (1=完全検出) ・is_engaged を持つ。<code>FCombatStateMachine</code> が内部で配列管理する。",
      when: "通常は内部用。複数敵の検出状態を並列に追跡するためのデータ。"
    },
    {
      name: "AComponent",
      kind: "クラス", header: "gameframework/AComponent.h",
      summary: "<code>ANode</code> に付ける<b>振る舞いパーツ</b>の基底。sprite 描画・当たり判定・アニメ・カスタムロジックを<b>継承でなく合成</b>で組み上げる (composition over inheritance)。<code>OnUpdate</code> 等のフックを override して使う。",
      when: "ノードに独自の振る舞いを足したい時。Unity の MonoBehaviour 的な感覚で 1 機能 = 1 コンポーネントを書く。",
      sample: "class ARotateComponent : public AComponent {\npublic:\n    ACS_GAME_COMPONENT_KIND(ARotateComponent)\n    void OnUpdate(f32 dt) noexcept override {\n        Owner().SetRotation2D(Owner().Rotation2D() + m_Speed * dt);\n    }\n    f32 m_Speed = 1.0f;\n};\nnode-&gt;AddComponent&lt;ARotateComponent&gt;();",
      members: [
        { sig: "virtual const void* Kind() const", desc: "型を識別する ID。派生では <code>ACS_GAME_COMPONENT_KIND(T)</code> マクロで 1 行 override する。" },
        { sig: "virtual void OnRequire(ANode& owner)", desc: "依存コンポーネント宣言フック (Unity の [RequireComponent] 相当)。<code>owner.GetOrAddComponent&lt;Dep&gt;()</code> で兄弟を自動確保。", when: "「このコンポーネントは ASprite2DComponent が必要」のような依存を表明する時。" },
        { sig: "virtual void OnAttach(ANode& owner)", desc: "ノードに付いた瞬間に呼ばれる。初期化に使う。" },
        { sig: "virtual void OnUpdate(f32 dt)", desc: "毎フレーム呼ばれる。可変 dt のロジック用。", when: "毎フレームの移動・回転・状態更新。" },
        { sig: "virtual void OnFixedUpdate(f32 fixed_dt)", desc: "固定刻みで呼ばれる (同フレームで 0 回 / 複数回あり得る)。物理や決定論的ロジック用。" },
        { sig: "virtual void OnDraw(FRenderContext& rc)", desc: "描画フック。ノードの描画タイミングで呼ばれる。" },
        { sig: "virtual void OnDrawPostChildren(FRenderContext& rc)", desc: "子ツリー描画の後に呼ばれる後処理フック。OnDraw で設定したマスク等を解除する用。" },
        { sig: "virtual void OnDetach()", desc: "ノードから外れる / 破棄される時に呼ばれる。後始末用。" },
        { sig: "ANode& Owner()", ret: "所有ノード", desc: "このコンポーネントが付いているノードを返す。" }
      ]
    },
    {
      name: "ComponentKindOf&lt;T&gt; / ACS_GAME_COMPONENT_KIND",
      kind: "関数テンプレート / マクロ", header: "gameframework/AComponent.h",
      summary: "<t>RTTI</t> を使わずにコンポーネント型を一意識別する仕組み。型 T ごとに別アドレスの static 変数を持ち、そのアドレスを型 ID にする。派生クラスは <code>ACS_GAME_COMPONENT_KIND(T)</code> マクロで <code>Kind()</code> を 1 行 override する。",
      when: "コンポーネントを自作する時、クラス内に 1 行書くだけ。",
      sample: "class AMyComp : public AComponent {\npublic:\n    ACS_GAME_COMPONENT_KIND(AMyComp)  // Kind() を生成\n};"
    },
    {
      name: "IContentModerator",
      kind: "インターフェース", header: "gameframework/ContentModerator.h",
      summary: "<t>UGC</t> (ユーザー生成コンテンツ = チャット文 / 画像 / ユーザー名) を「公開可 / 警告付き / ブロック」の 3 値で判定する<t>seam</t>。実 API (Azure / OpenAI Moderation 等) は別モジュールで差し込む前提で、ここは I/F と Stub だけ。",
      when: "オンライン要素でユーザーが入力したテキストや画像を公開前にチェックしたい時。",
      sample: "IContentModerator* mod = &acs::game::GetModeratorStub();\nauto r = mod-&gt;ModerateText(userId, text);\nif (r &amp;&amp; r.Value().verdict == EModerationVerdict::Block)\n    ShowBlockedUi(r.Value().reason);",
      members: [
        { sig: "virtual TResult<FModerationResult> ModerateText(const char* user_id, const char* text)", ret: "判定結果", desc: "チャット / プロフィール文等をモデレーションする。", when: "テキスト投稿前。" },
        { sig: "virtual TResult<FModerationResult> ModerateImage(const char* user_id, const u8* image_data, u64 size)", desc: "投稿画像 (PNG/JPEG 等の生バイト列) をモデレーションする。size==0 / nullptr は BadArgument。" },
        { sig: "virtual TResult<FModerationResult> ModerateUserName(const char* name)", desc: "ユーザー名をモデレーションする (通常 text より厳しめ)。" },
        { sig: "virtual void Tick(f32 dt)", desc: "非同期処理 (REST 結果ポーリング等) のポンプ。毎フレーム呼ぶ。Stub は no-op。" },
        { sig: "virtual bool IsAvailable() const", desc: "API を発火できる状態か。Stub は常に true。" }
      ]
    },
    {
      name: "FContentModeratorStub",
      kind: "クラス", header: "gameframework/ContentModerator.h",
      summary: "<code>IContentModerator</code> の依存ゼロのデフォルト実装。内部の小さな NG 単語リストに対し、生文字列と<b>正規化</b>文字列 (小文字化 + leet 置換 + 記号除去) の 2 経路で照合する。<code>GetModeratorStub()</code> で共有 singleton を取れる。",
      when: "実 SDK を入れる前のローカルテキストフィルタとして、またはテスト用に。",
      sample: "IContentModerator& mod = acs::game::GetModeratorStub();\n// f.u.c.k や sh1t のような変種も同じ辞書で捕捉",
      members: [
        { sig: "TResult<FModerationResult> ModerateImageRgba8(const u8* rgba, u32 width, u32 height)", desc: "デコード済み RGBA8 ピクセルを直接 NSFW 判定する。肌色比率 heuristic で露出度を推定。", when: "CPU 側に既にテクスチャがあり再デコードを避けたい時。" }
      ]
    },
    {
      name: "FModerationResult",
      kind: "構造体", header: "gameframework/ContentModerator.h",
      summary: "1 回のモデレーション結果。verdict (Allow/Warn/Block) ・rating (Safe〜Explicit) ・reason 文字列を持つ。",
      when: "Moderate*() の戻り値を読む時。",
      sample: "if (r.Value().verdict == EModerationVerdict::Warn)\n    ShowWarnUi(r.Value().reason);  // 確認後に続行可"
    },
    {
      name: "EModerationVerdict / EContentRating",
      kind: "列挙(enum)", header: "gameframework/ContentModerator.h",
      summary: "<b>Verdict</b>=UI フロー制御の 3 値 (Allow / Warn / Block)。<b>Rating</b>=年齢ゲート用の 5 値 (Safe / Mild / Mature / Explicit / SexualMinor_HardcodedBlock)。最後の 1 つは実装に関わらず<b>必ず Block</b> される法令遵守ガードレール。",
      when: "判定結果を分岐する時。",
      sample: "switch (r.Value().verdict) {\n  case EModerationVerdict::Allow: Post(); break;\n  case EModerationVerdict::Block: Reject(); break;\n}"
    },
    {
      name: "FCooldownTimer",
      kind: "クラス", header: "gameframework/CooldownTimer.h",
      summary: "複数の<b>クールダウン</b> (スキル / アビリティ / リロード等) を同時追跡する軽量マネージャ。各 cooldown は label と長さを持ち、Tick で残り時間を減らし、0 で使用可 (charged) になる。<code>TryUse</code> は charged の時だけ true を返して即リロード開始。",
      when: "「このスキルは 3 秒に 1 回しか撃てない」のような再使用待ち時間を管理する時。",
      sample: "FCooldownTimer cd;\nFCooldownId fireball = cd.Register(\\\"fireball\\\", 3.0f);\ncd.ForceReady(fireball);  // 初手は即撃てる\nvoid OnUpdate(f32 dt) {\n    cd.Tick(dt);\n    if (pressed &amp;&amp; cd.TryUse(fireball)) SpawnFireball();\n}",
      members: [
        { sig: "FCooldownId Register(const char* label, f32 duration_sec)", ret: "cooldown handle", desc: "cooldown を登録する。duration<=0 は invalid。登録直後はリロード中扱い。", when: "スキルを定義する時。" },
        { sig: "void Unregister(FCooldownId id)", desc: "cooldown を削除する (slot 再利用、stale 検出可)。" },
        { sig: "bool TryUse(FCooldownId id)", ret: "使えたか", desc: "charged なら remaining=duration にして true。リロード中は false。", when: "スキルを発動しようとする時。", sample: "if (cd.TryUse(dash)) DoDash();" },
        { sig: "bool IsCharged(FCooldownId id) const", desc: "今すぐ使えるか。" },
        { sig: "f32 Progress(FCooldownId id) const", ret: "[0,1]", desc: "リロード進捗。1.0 で charged。HUD のゲージ表示に使う。" },
        { sig: "f32 Remaining(FCooldownId id) const", desc: "残り秒数 (charged / stale は 0)。" },
        { sig: "void ForceReady(FCooldownId id)", desc: "即座に charged にする (ReadyCallback も発火)。", when: "初期化時に「最初から撃てる」状態にしたい時。" },
        { sig: "void Reset(FCooldownId id)", desc: "即座に duration までリロードし直す (charged=false)。" },
        { sig: "void SetDuration(FCooldownId id, f32 new_duration_sec)", desc: "進行中 cooldown の長さを変更する (バフ / デバフで短縮・延長)。" },
        { sig: "void Tick(f32 dt)", desc: "全 cooldown の remaining を減らす。0 を跨いだ瞬間に charged 遷移 + ReadyCallback 発火。", when: "毎フレーム呼ぶ。" },
        { sig: "void SetOnReadyCallback(ReadyCallback cb, void* user)", desc: "charged になった瞬間に呼ばれる callback (HUD の \\\"Ready!\\\" 点滅等)。" }
      ]
    },
    {
      name: "FCooldownId",
      kind: "構造体", header: "gameframework/CooldownTimer.h",
      summary: "cooldown を指す<t>ハンドル</t>。24bit index + 8bit generation の packed u32。0 は invalid。",
      when: "登録した cooldown を後で参照する時。",
      sample: "FCooldownId id = cd.Register(\\\"dash\\\", 0.8f);"
    },
    {
      name: "FCooldownState",
      kind: "構造体", header: "gameframework/CooldownTimer.h",
      summary: "UI / デバッグ表示用の cooldown スナップショット。label・duration・remaining・charged を持つ。",
      when: "クールダウンの状態をまとめて表示したい時の読み取り用。"
    },
    {
      name: "FCraftingSystem",
      kind: "クラス", header: "gameframework/CraftingSystem.h",
      summary: "サバイバル / クラフト系の<b>レシピ + 素材消費 + 時間経過で成果物生成</b>を扱う production マネージャ。レシピを登録し、StartCraft で素材を消費して Tick で進行、完了すると成果物を付与する。在庫操作は<t>adapter</t> callback 経由なので在庫表現に依存しない。",
      when: "「木3個 + 鉄2個を4秒かけて斧にする」のようなクラフトを作る時。同時クラフトは 1 件なので、複数同時なら instance を複数持つ。",
      sample: "FCraftingSystem cs;\ncs.RegisterRecipe({ \\\"craft.axe\\\", \\\"Iron Axe\\\", \\\"tool.axe\\\", 1, 2, ingredients, 4.0f, \\\"workbench\\\", 3 });\ncs.SetInventoryAdapter(&Query, &Consume, &Grant, &inv);\nif (cs.CanCraft(\\\"craft.axe\\\", \\\"workbench\\\", level))\n    cs.StartCraft(\\\"craft.axe\\\", \\\"workbench\\\", level);\n// 毎フレーム: cs.Tick(dt);",
      members: [
        { sig: "void RegisterRecipe(const FCraftRecipe& recipe)", desc: "レシピを登録する。同 id の 2 重登録は no-op (WARN)。", when: "起動時に全レシピをまとめて登録。" },
        { sig: "void SetInventoryAdapter(InventoryQueryFn query, InventoryConsumeFn consume, InventoryGrantFn grant, void* user)", desc: "在庫の問い合わせ / 消費 / 付与を担う関数を差し込む。全 nullptr なら在庫無制限 (デバッグ用)。", when: "在庫システムと繋ぐ時。" },
        { sig: "bool CanCraft(const char* recipe_id, const char* current_workbench = nullptr, u32 player_level = 999) const", ret: "作れるか", desc: "素材・ワークベンチ・レベルを副作用なしで事前判定する。true なら StartCraft も成功する。", when: "ボタンの有効/無効を決める時。" },
        { sig: "bool StartCraft(const char* recipe_id, const char* current_workbench, u32 player_level)", ret: "開始できたか", desc: "素材を一括消費して Crafting 状態に遷移する。途中失敗時は巻き戻す。", when: "プレイヤーがクラフト開始した時。" },
        { sig: "void CancelCraft()", desc: "Crafting 中だけ動作。消費した素材を全量返却して Cancelled に。" },
        { sig: "ECraftStatus Status() const", desc: "現在の状態 (Idle / Crafting / Completed / Cancelled)。" },
        { sig: "f32 CraftProgress() const", ret: "[0,1]", desc: "進行率。Crafting 時のみ意味を持つ。プログレスバー表示に。" },
        { sig: "f32 CraftRemainingSec() const", desc: "残り秒数。" },
        { sig: "const char* CurrentRecipeId() const", desc: "今クラフト中のレシピ id (Idle / 未開始は nullptr)。" },
        { sig: "void Tick(f32 dt)", desc: "Crafting 中なら残り時間を減らし、0 で成果物 grant + CompleteCallback 発火 + Completed に。", when: "毎フレーム呼ぶ。" },
        { sig: "void SetOnCompleteCallback(CompleteCallback cb, void* user)", desc: "クラフト完了フレームに呼ばれる callback (SE / トースト用)。" },
        { sig: "const FCraftRecipe* FindRecipe(const char* recipe_id) const", desc: "レシピ定義を取得する (未登録は nullptr)。" }
      ]
    },
    {
      name: "FCraftRecipe",
      kind: "構造体", header: "gameframework/CraftingSystem.h",
      summary: "1 件のレシピ定義。recipe_id・表示名・成果物 (result_item_id / count) ・素材配列 (ingredients) ・所要秒数・必要ワークベンチ・解放レベルを持つ。文字列・素材配列は<t>非所有</t>。",
      when: "<code>RegisterRecipe()</code> に渡す時。",
      sample: "static const FIngredient mats[] = { {\\\"wood\\\",3}, {\\\"iron_ore\\\",2} };\nFCraftRecipe r{ \\\"craft.axe\\\", \\\"Iron Axe\\\", \\\"tool.axe\\\", 1, 2, mats, 4.0f, \\\"workbench\\\", 3 };"
    },
    {
      name: "FIngredient",
      kind: "構造体", header: "gameframework/CraftingSystem.h",
      summary: "レシピの素材エントリ 1 つ。item_id (素材アイテム id) と count (必要個数) を持つ。",
      when: "レシピの素材配列を組む時。",
      sample: "FIngredient i{ \\\"wood\\\", 3 };"
    },
    {
      name: "ECraftStatus",
      kind: "列挙(enum)", header: "gameframework/CraftingSystem.h",
      summary: "クラフトの現在状態。Idle / Crafting / Completed (成果物付与済) / Cancelled の 4 種。",
      when: "クラフト状態で UI を分岐する時。",
      sample: "if (cs.Status() == ECraftStatus::Crafting) ShowProgressBar();"
    },
    {
      name: "ICrashReporterBackend",
      kind: "インターフェース", header: "gameframework/CrashReporter.h",
      summary: "出荷ビルドでプロセスが落ちた時に外部のクラッシュ集約サービス (Sentry / Crashpad / BugSnag 等) へ context を吐き出すための<t>seam</t>。本体は具象 HTTP/IPC を抱えず、I/F と Stub だけを持つ。失敗は黙って握り潰す (二次クラッシュ防止)。",
      when: "クラッシュレポートやエラー報告・<t>breadcrumb</t> をサービスに送りたい時。実装は各タイトルで差し込む。",
      sample: "ICrashReporterBackend& cr = acs::game::GetDefaultCrashReporter();\ncr.Init(\\\"com.example.game\\\", \\\"1.2.3\\\");\ncr.AddBreadcrumb(\\\"save\\\", \\\"autosave start\\\");\n// 毎フレーム: cr.Tick(dt);",
      members: [
        { sig: "virtual TResult<void> Init(const char* product_id, const char* version)", desc: "SDK を初期化する。product_id / version は集計キー。", when: "起動時に 1 度。" },
        { sig: "virtual TResult<void> ReportCrash(const FCrashContext& ctx)", desc: "クラッシュ 1 件を送信する (プロセスがまだ生きている前提)。" },
        { sig: "virtual TResult<void> ReportError(const char* category, const char* message)", desc: "非致命的エラーを送信する。category は \\\"net\\\" / \\\"save\\\" 等の集計キー。" },
        { sig: "virtual TResult<void> AddBreadcrumb(const char* category, const char* message)", desc: "クラッシュ前の足跡を 1 件追加する (送信せず ReportCrash 時に添付)。", when: "重要操作の前後に挟んで後で追跡できるようにする。" },
        { sig: "virtual void SetUserId(const char* anonymous_id)", desc: "匿名ユーザー ID を設定する (生 email でなく UUID 推奨、GDPR/CCPA 配慮)。" },
        { sig: "virtual void Tick(f32 dt)", desc: "非同期送信キューのポンプ。毎フレーム呼ぶ。" },
        { sig: "virtual bool IsAvailable() const", desc: "Init 済みで送信可能なら true。Stub は常に false。" }
      ]
    },
    {
      name: "FCrashReporterStub / GetCrashStub / GetDefaultCrashReporter",
      kind: "クラス / 関数", header: "gameframework/CrashReporter.h",
      summary: "<code>FCrashReporterStub</code> は全 API が NotImplemented を返す<t>null-object</t> 実装。<code>GetCrashStub()</code> で共有 stub を取得。<code>GetDefaultCrashReporter()</code> は実 backend が登録済ならそれを、未登録なら stub を返す。<code>SetCrashReporterProvider()</code> で実 backend を結線する。",
      when: "実 backend が無い環境でもリンクを通したい時、または backend 非依存に既定 reporter を取りたい時。",
      sample: "// 起動時、実 backend がリンクされていれば:\n#if WITH_ACS_CRASH_REPORTER\n    acs::crashwin::InstallWindowsCrashReporterAsDefault();\n#endif\nauto& cr = acs::game::GetDefaultCrashReporter();"
    },
    {
      name: "FCrashHandler",
      kind: "クラス", header: "gameframework/CrashReporter.h",
      summary: "<code>ICrashReporterBackend</code> を 1 つ抱えてタイトル側で使いやすくする薄い wrapper。backend が未 Install (nullptr) のときは全 API が no-op になる (二次クラッシュ防止)。",
      when: "ホットパスから簡単にクラッシュ通知 / breadcrumb を出したい時。",
      sample: "FCrashHandler handler;\nhandler.Install(&backend);\nhandler.AddBreadcrumb(\\\"shader\\\", \\\"compile pass 2\\\");\nhandler.NotifyCrash(\\\"SEH_AV\\\", \\\"null deref in Foo\\\");",
      members: [
        { sig: "void Install(ICrashReporterBackend* backend)", desc: "backend 参照を保持する (寿命は呼出側所有)。nullptr で Uninstall と同義。" },
        { sig: "void Uninstall()", desc: "backend 参照を外す (べき等)。" },
        { sig: "void NotifyCrash(const char* exception_type, const char* message)", desc: "最小フィールドだけ埋めて ReportCrash を呼ぶ簡略パス。" },
        { sig: "void AddBreadcrumb(const char* category, const char* message)", desc: "backend に素通し。未 Install なら no-op。" }
      ]
    },
    {
      name: "FCrashContext",
      kind: "構造体", header: "gameframework/CrashReporter.h",
      summary: "クラッシュ 1 件分の context。exception_type・message・stack_trace (改行区切り) ・scene_name・frame_count・timestamp・build_id を持つ。文字列は<t>非所有</t>。",
      when: "<code>ReportCrash()</code> に渡す時。",
      sample: "FCrashContext ctx;\nctx.exception_type = \\\"SEH_ACCESS_VIOLATION\\\";\nctx.message = \\\"null deref\\\";\nctx.frame_count = frameCount;"
    },
    {
      name: "FDamageFeedback",
      kind: "クラス", header: "gameframework/DamageFeedback.h",
      summary: "プレイヤー被弾時の<b>視覚フィードバック</b>を集中管理する純粋な state machine。赤い被弾オーバーレイ・攻撃源を指す方向矢印・<t>death cam</t> の 3 要素を扱う。自分では描画もカメラ操作もせず、描画 / UI / カメラ層が値を pull する。",
      when: "FPS / アクションで「ダメージを受けたら画面端が赤くなる」「どこから撃たれたか矢印で示す」「死亡時にキラーをズーム」を入れたい時。",
      sample: "FDamageFeedback dmg;\nvoid OnHit(f32 amount, FVec2 attackerPos) {\n    dmg.TakeDamage(amount, player.Pos() - attackerPos);\n    if (player.HP() <= 0) dmg.TriggerDeathCam(killerWorldPos);\n}\nvoid OnUpdate(f32 dt) { dmg.Tick(dt); }\n// 描画: overlay.Apply(dmg.ScreenEdgeRedIntensity());",
      members: [
        { sig: "void TakeDamage(f32 amount, FVec2 source_direction = {})", desc: "ダメージを受けたと通知。赤エッジを累積加算し、source_direction (相手→自分) があれば矢印を更新する。正規化は内部で行う。", when: "プレイヤーが被弾した瞬間。" },
        { sig: "void Tick(f32 dt)", desc: "各エフェクトの decay を進める (赤エッジ・矢印は約 0.5 秒で消える)。death cam progress も進める。", when: "毎フレーム呼ぶ。" },
        { sig: "f32 ScreenEdgeRedIntensity() const", ret: "[0,1]", desc: "画面端を赤く染める強度。描画 overlay が vignette に掛ける。" },
        { sig: "bool HasDirectionalIndicator() const", desc: "方向矢印を描くべきか。" },
        { sig: "FVec2 DirectionalIndicator() const", desc: "矢印が指す向き (正規化済み)。HasDirectionalIndicator() で守ってから読む。" },
        { sig: "f32 DirectionalIndicatorAlpha() const", desc: "矢印の不透明度 [0,1]。" },
        { sig: "void TriggerDeathCam(FVec3 killer_pos)", desc: "death cam 演出を開始する。killer_pos はカメラを向ける対象座標。", when: "致命傷を検知した時。" },
        { sig: "bool IsDeathCamActive() const", desc: "death cam 中か。" },
        { sig: "f32 DeathCamProgress() const", ret: "[0,1]", desc: "dramatic zoom の進行 (2 秒で 0→1、その後 1.0 維持)。カメラ制御がイージングに使う。" },
        { sig: "void ExitDeathCam()", desc: "death cam を明示的に解除する (リスポーン開始時等)。" },
        { sig: "void Reset()", desc: "全 state を初期化する (シーン切替 / respawn 用)。" }
      ]
    },
    {
      name: "FDebugDraw",
      kind: "クラス", header: "gameframework/DebugDraw.h",
      summary: "1 フレーム分の<t>線分プリミティブ</t>を溜めるだけの<t>immediate-mode</t> デバッグ図形バッファ。実描画はしない。ゲームロジックの任意の場所から線 / AABB / 円 / 十字を積み、描画側が <code>Lines()</code> を読んでまとめて描く。",
      when: "当たり判定や物理の様子をその場で線で可視化したい時。テストでは溜まった線配列を直接検査できる。",
      sample: "FDebugDraw dd;\ndd.Clear();\ndd.DrawAabb(playerAabb, FVec4{1,0,0,1});\ndd.DrawCircle(enemyCircle, FVec4{1,1,0,1});\nfor (u32 i = 0; i < dd.LineCount(); ++i) {\n    const auto& ln = dd.Lines()[i];\n    batch.DrawLine(ln.a, ln.b, ln.color);\n}",
      members: [
        { sig: "void DrawLine(FVec2 a, FVec2 b, FVec4 color)", desc: "任意 2 点間の線分を積む。" },
        { sig: "void DrawAabb(const FAabb2& a, FVec4 color)", desc: "AABB の輪郭 (4 辺) を積む。中身は塗らない。", when: "矩形コライダーの可視化。" },
        { sig: "void DrawCircle(const FCircle& c, FVec4 color, u32 segments = 24)", desc: "円を segments 本の線分に分解して積む。segments<3 は丸める。", when: "円コライダーや範囲の可視化。" },
        { sig: "void DrawCross(FVec2 pos, f32 size, FVec4 color)", desc: "中心 pos の \\\"+\\\" 記号を積む。位置可視化に便利。" },
        { sig: "void Clear()", desc: "蓄積をクリアする (容量は保持)。フレーム頭か描画消費後に呼ぶ。" },
        { sig: "u32 LineCount() const", desc: "蓄積した線の本数。" },
        { sig: "const FLine* Lines() const", ret: "生バッファ先頭", desc: "描画システムが読む連続メモリ。空のとき nullptr なので LineCount() でガードする。" }
      ]
    },
    {
      name: "FDebugOverlay",
      kind: "クラス", header: "gameframework/DebugOverlay.h",
      summary: "FPS / シーン名 / カスタム watch をテキスト表示するための<b>状態保持クラス</b>。実描画は呼出側の責務で、本クラスは「何を出すか」と FPS の 60 フレーム移動平均だけを保持する。Headless でも動く。",
      when: "画面に FPS や任意の数値 (Player.HP 等) をデバッグ表示したい時。",
      sample: "FDebugOverlay ov;\nov.Init();\nov.SetSceneName(\\\"Gameplay\\\");\nov.AddWatch(\\\"Player.HP\\\", hpText);  // 値文字列は caller 所有\nvoid OnUpdate(f32 dt) { ov.Tick(dt); }\n// 描画: DrawString(8,8, ov.CurrentFps()...);",
      members: [
        { sig: "void Init()", desc: "内部バッファを事前確保する (60 frame 履歴)。再 Init は履歴クリア。" },
        { sig: "void Tick(f32 dt)", desc: "1 フレーム進めて FPS の移動平均 / min / max を更新する。dt<=0 は無視 (発散防止)。", when: "毎フレーム呼ぶ。" },
        { sig: "f32 CurrentFps() const", desc: "最新フレームの瞬間 FPS (1/dt)。" },
        { sig: "f32 AverageFps() const", desc: "直近 60 フレームの平均 FPS。" },
        { sig: "f32 MinFps() / f32 MaxFps() const", desc: "履歴中の最小 / 最大 FPS。" },
        { sig: "void AddWatch(const char* label, const char* value)", desc: "任意のラベル + 値文字列を表示項目に追加する。同名は後勝ち上書き。値バッファは caller が毎フレーム更新する。", when: "HP やスコア等を画面に出したい時。" },
        { sig: "void RemoveWatch(const char* label)", desc: "ラベル一致の watch を削除する (順序非保持)。" },
        { sig: "const FWatch* AllWatches(u32& out_count) const", desc: "全 watch を読み取り用に列挙する。描画側が順に描く。" },
        { sig: "void Show() / Hide() / Toggle()", desc: "表示の ON/OFF (例: F3 で Toggle)。" },
        { sig: "void SetSceneName(const char* name)", desc: "オーバーレイに出すシーン名を設定する (caller 所有のリテラル)。" }
      ]
    },
    {
      name: "FDeckSystem",
      kind: "クラス", header: "gameframework/DeckSystem.h",
      summary: "1 プレイヤー分のカードゲーム状態を<b>デッキ / 手札 / 捨札 / 除外 (exile)</b> の 4 ゾーンで保持する小型マネージャ。カード定義 (<code>FCardDef</code>) を登録し、シャッフル・ドロー・プレイ・捨札をメソッドで明示的に行う。シャッフルは seed 指定で決定論再現できる。",
      when: "Slay the Spire / Hearthstone / MtG のようなデッキ構築系を作る時。対戦相手がいれば instance を人数分持つ。",
      sample: "FDeckSystem deck;\ndeck.RegisterCard({ \\\"card.bolt\\\", \\\"Lightning Bolt\\\", 1, 2, \\\"spell\\\", \\\"Deal 3.\\\", \\\"ui/bolt.png\\\" });\ndeck.AddToDeck(\\\"card.bolt\\\", 4);\ndeck.SetOnPlayCallback(&OnPlayed, &game);\ndeck.Shuffle();\ndeck.DrawN(7);\ndeck.PlayCard(2);  // 手札 index 2 を場へ → callback → 捨札",
      members: [
        { sig: "void RegisterCard(const FCardDef& def)", desc: "カード定義を 1 種類登録する。同 id の 2 重登録は no-op (WARN)。", when: "起動時に全カードを登録。" },
        { sig: "void AddToDeck(const char* card_id, u32 count = 1)", desc: "指定カードを山札に count 枚積む。未登録 / count==0 は no-op。", when: "デッキ構築時。" },
        { sig: "void Shuffle(u32 seed = 0)", desc: "デッキを Fisher-Yates でシャッフルする。seed==0 はランダム、seed!=0 は決定論再現。", when: "試合開始時やリプレイ再現時。" },
        { sig: "bool Draw()", ret: "引けたか", desc: "デッキトップから 1 枚引いて手札へ。デッキが空なら捨札をシャッフルして戻し再ドロー。両方空なら false。", when: "1 枚引く時。" },
        { sig: "bool DrawN(u32 n)", desc: "n 枚引く。途中で尽きたら引けた分だけ保持して終了 (その場合 false)。", when: "初手 7 枚などまとめて引く時。" },
        { sig: "bool PlayCard(u32 hand_index)", ret: "出せたか", desc: "手札のカードをプレイする: PlayCallback 発火 → 捨札へ。範囲外は false。", when: "プレイヤーがカードを場に出した時。" },
        { sig: "bool DiscardFromHand(u32 hand_index)", desc: "手札 index のカードを捨札へ移す。範囲外は false。" },
        { sig: "bool ExileFromHand(u32 hand_index)", desc: "手札 index のカードを exile (完全除外、戻らない) へ移す。" },
        { sig: "bool DiscardAllHand()", desc: "手札を全部捨札にする (ターン終了等)。" },
        { sig: "u32 DeckSize() / HandSize() / DiscardSize() / ExileSize() const", desc: "各ゾーンの枚数。UI 表示に使う。" },
        { sig: "const char* HandCardAt(u32 index) const", desc: "手札 index のカード id。範囲外は nullptr。" },
        { sig: "void SetOnPlayCallback(PlayCallback cb, void* user)", desc: "PlayCard 時に呼ばれる callback (アニメ / 効果発動 / SE 用)。" },
        { sig: "const FCardDef* FindCardDef(const char* card_id) const", desc: "カード定義を取得する (未登録は nullptr)。" }
      ]
    },
    {
      name: "FCardDef",
      kind: "構造体", header: "gameframework/DeckSystem.h",
      summary: "カード 1 種類の定義 (immutable)。id・表示名・cost・rarity・card_type・description・art_path を持つ。文字列はすべて<t>非所有</t>。",
      when: "<code>RegisterCard()</code> に渡す時。",
      sample: "FCardDef def{ \\\"card.bear\\\", \\\"Grizzly Bears\\\", 2, 1, \\\"creature\\\", \\\"A 2/2 bear.\\\", \\\"ui/bears.png\\\" };"
    },
    {
      name: "FCardId",
      kind: "構造体", header: "gameframework/DeckSystem.h",
      summary: "場に存在する 1 枚のカードを識別する<t>ハンドル</t>。24bit index + 8bit generation の packed u32。0 は invalid。将来「同名カードでも各個に付与効果」を持たせる拡張の土台 (現状は const char* ベース API が主)。",
      when: "カード個体を ID で扱う拡張時。",
      sample: "FCardId id;\nif (id.IsValid()) { /* ... */ }"
    },
    {
      name: "FDevConsole",
      kind: "クラス", header: "gameframework/DevConsole.h",
      summary: "<code>~</code> で開く開発用<b>コマンドコンソール</b>の state コンテナ。コマンド名→関数ポインタの登録 / 検索 / 実行、入力履歴、ログバッファ、開閉状態を持つ。描画 / 入力 UI は上位レイヤの責務。履歴・ログは内部バッファに複製保持する。",
      when: "デバッグ用に「quit」「spawn enemy」のようなコマンドを打てるコンソールを作る時。Ship build では丸ごと除外する想定。",
      sample: "FDevConsole con;\ncon.RegisterCommand(\\\"quit\\\", &App::QuitCmd, this, \\\"exit\\\");\nif (tildePressed) con.Toggle();\nif (con.IsOpen() &amp;&amp; enter) {\n    con.PushHistory(input);\n    con.Execute(input);  // \\\"spawn 3\\\" を tokenize して dispatch\n}",
      members: [
        { sig: "void RegisterCommand(const char* name, CommandFn fn, void* user, const char* help_text)", desc: "コマンドを登録する。重複は警告して後勝ち上書き。", when: "起動時に各コマンドを登録。" },
        { sig: "void Execute(const char* command_line)", desc: "入力行を空白で tokenize → 先頭をコマンド名に照合 → dispatch する。該当無し / 空行は警告ログ。最大 8 引数。", when: "Enter が押された時。" },
        { sig: "void PushHistory(const char* line)", desc: "入力行を内部バッファにコピーして履歴に追加する (上限 100、最古を drop)。", when: "上下キーで再呼び出しできるようにする時。" },
        { sig: "const char* History(u32 i) const", desc: "履歴 i 番目 (0=最古)。範囲外は nullptr。" },
        { sig: "void Log(const char* msg)", desc: "コマンド出力 / エラーを内部バッファにコピーして追加する (上限 100)。" },
        { sig: "const char* LogLine(u32 i) const", desc: "ログ i 行目 (0=最古)。描画側がスクロールバックに表示する。" },
        { sig: "void Open() / Close() / Toggle()", desc: "コンソールの開閉。", sample: "if (tilde) con.Toggle();" },
        { sig: "bool IsOpen() const", desc: "今開いているか。" },
        { sig: "void Clear()", desc: "履歴とログを破棄する (コマンド登録は保持)。" },
        { sig: "u32 CommandCount() const", desc: "登録済みコマンド数。" }
      ]
    },
    {
      name: "FConsoleArg / CommandFn",
      kind: "構造体 / 関数ポインタ型", header: "gameframework/DevConsole.h",
      summary: "<code>FConsoleArg</code> はコマンド引数 1 つ (Phase 1 では raw 文字列のみ)。<code>CommandFn</code> はコマンド本体の関数ポインタ型 <code>void(*)(void* user, u32 argc, const FConsoleArg* args)</code>。argv[0] はコマンド名でなく最初の引数。",
      when: "コマンドを実装する時のシグネチャ。",
      sample: "static void SpawnCmd(void* user, u32 argc, const FConsoleArg* args) noexcept {\n    if (argc >= 1) Spawn(args[0].str);\n}"
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "マネージャ": "ある種類の状態 (cooldown / チェックポイント / カード等) をまとめて持ち、登録・更新・問い合わせの API を提供する小さなクラス。",
  "seam": "実装を後から差し替えられるように切った継ぎ目。抽象<t>インターフェース</t>として宣言し、本体は最小の Stub だけ持ち、実装 (実 SDK 等) は別モジュールで注入する。",
  "callback": "ある出来事が起きた時に呼んでもらう関数。ACS では std::function を使わず、関数ポインタ + <code>void* user</code> の組で渡す。",
  "pay-to-win": "課金で性能 (戦闘力) を直接買えてしまう設計。<t>FCharacterCustomizer</t> は見た目のみ扱い、性能をいじらないことで構造的にこれを避ける。",
  "有限オートマトン": "決められた状態のどれか 1 つにいて、入力で別の状態へ遷移する仕組み (state machine)。戦闘フェーズ管理などに使う。",
  "time_scale": "時間の流れる倍率。1=通常、0=停止、>1=早送り、<1=スロー。<t>FSceneClock</t> がこれを掛けて scaled な時間を作る。",
  "空間グリッド": "2D 空間を格子セルに区切り、形状を重なるセルに登録しておく仕組み (SpatialGrid)。近くのセルだけ調べれば候補が絞れるので当たり判定が速くなる (broad-phase)。",
  "レイキャスト": "ある点からある方向へ光線 (ray) を飛ばし、最初に当たる形状と交点を求める処理。視線判定や射撃に使う。",
  "ビットマスク": "32 個の ON/OFF フラグを 1 つの整数に詰めたもの。レイヤ (player/enemy/wall) を bit で表し、AND で「対象に含むか」を高速判定する。",
  "collide-and-slide": "移動 → 壁にめり込んだら押し出す → 壁に沿ってスライドさせる、を繰り返すキャラ移動の定番アルゴリズム。",
  "ハンドル": "オブジェクトを直接ポインタで持たず、index + 世代番号を詰めた軽い ID で間接参照する仕組み。slot 再利用後の古い参照を世代不一致で安全に弾ける。",
  "世代 (generation)": "<t>ハンドル</t>に含める世代番号。同じ index の slot が作り直されるたびに増やし、古いハンドルを無効と判定するために使う。",
  "RTTI": "実行時型情報 (Run-Time Type Information)。dynamic_cast / typeid で型を判別する C++ の仕組み。ACS は無効化し、代わりに型ごとの static アドレスを ID に使う。",
  "union": "複数の型のうち 1 つだけを同じメモリに置ける C/C++ の仕組み。<t>FTimelineKeyframe</t> は kind ごとの payload を union で持つ。",
  "death cam": "プレイヤーが死んだ瞬間に、致命傷を与えた相手をズームしながら映す演出カメラ。",
  "UGC": "User Generated Content。ユーザーが作って投稿するテキスト / 画像 / 名前など。公開前のモデレーションが要る。",
  "breadcrumb": "クラッシュ前に何が起きていたかを時系列で残す短いログ。クラッシュ報告に添付して原因追跡に使う。",
  "null-object": "「何もしない」を正しく実装した置き換え用オブジェクト。実装が無い時でも安全に呼べる (Stub がこの形)。",
  "adapter": "別システム (ここでは在庫システム) との橋渡しをする差し替え可能な関数群。これ経由にすることで本体が在庫の実装に依存しない。",
  "immediate-mode": "状態を保持せず、毎フレーム「今出したいもの」をその場で積み上げる API スタイル。<t>FDebugDraw</t> がこの方式。",
  "線分プリミティブ": "2 つの端点と色からなる 1 本の線。デバッグ描画の最小単位。",
  "非所有": "ポインタが指す先のメモリを自分では確保も解放もしないこと。文字列リテラル等、寿命を呼出側が保証する前提で借りているだけの状態。"
});
