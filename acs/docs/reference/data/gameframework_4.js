/* ACS リファレンス — gameframework モジュール (4/N: HotReload 〜 ANode)。
   形式・口調・粒度は data/memory.js を手本に踏襲。
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "gameframework", order: 43,
  title: "gameframework — ゲーム部品 (acs::game)",
  blurb: "ゲームを組むのに繰り返し必要になる「部品」を集めたモジュール。入力マッピング・インベントリ・空腹/サバイバル統計・適応 BGM・ローカライズ・ネットコード (lockstep / snapshot)・Mod 管理・ホットリロード・LLM NPC 安全フィルタ・シーンの基本ノード (ANode) などを、すぐ使える形で提供します。",
  types: [
    // =====================================================================
    // HotReload.h
    // =====================================================================
    {
      name: "CHotReloadWatcher",
      kind: "クラス", header: "gameframework/HotReload.h",
      summary: "Windows 開発ビルドで <code>ReadDirectoryChangesW</code> を非同期駆動し、変更を bounded FIFO と安定 snapshot の<t>コールバック</t>へ配る single-thread-affine watcher/queue seam。path は UTF-8 の所有コピーで、同一 path の burst を debounce できる。アセット再 import や <code>CMessageBroker</code> publish は上位利用側の責務。",
      when: "ゲームを動かしたまま asset directory の変更を検出する時。出荷ビルド (<code>ACS_GAME_SHIPPING</code>) では互換 API を残した no-op shell となる。<code>TryWatchFile</code> は path 登録だけで native handle を作らないため、単一 file の実監視には親 directory も登録する。",
      members: [
        { sig: "void Init() / void Shutdown()", desc: "初期化 / 後始末。Shutdown は監視パス・コールバック・未処理イベントを全クリア。どちらも多重呼び出し可。" },
        { sig: "EHotReloadResult TryWatchDirectory(const char* dir, bool recursive = true)", desc: "UTF-8 path 検証・所有コピー・OS handle・初回 async read が全て成功した時だけ監視を登録する。" },
        { sig: "EHotReloadResult TryWatchFile(const char* file_path)", desc: "bounded な単一 file path 登録。native watcher は作らないので、OS 変更を得るには親 directory の登録が必要。" },
        { sig: "void Unwatch(const char* path)", desc: "監視対象から外す (完全一致のみ)。" },
        { sig: "EHotReloadResult TryRegisterCallback(FHotReloadCallback cb, void* user) / UnregisterCallback(...)", desc: "最大 64 組の callback を checked 登録/解除する。dispatch は stable snapshot で、callback からの解除・ClearEvents・Shutdown を許す。ClearEvents 後に enqueue した event は次の TryTick まで延期する。" },
        { sig: "EHotReloadResult TrySetDebounceSeconds(f32 seconds) / f32 DebounceSeconds() const", desc: "同一 path の event coalescing を有限な 0〜60 秒で設定/取得する。" },
        { sig: "EHotReloadResult TryEnqueueEvent(const char* path, u64 timestamp, bool removed = false)", desc: "独自 change source から native event と同じ検証・上限・debounce を通して enqueue する。" },
        { sig: "EHotReloadResult TryTick(f32 dt)", desc: "Windows async watcher を非 blocking poll し、FIFO event を callback へ配る。無効 dt と再入を拒否する。native overflow、poll・parser・再 arm error、native event の queue/OOM loss は診断 snapshot に authoritative rescan 要求を残す。" },
        { sig: "bool ConsumeNextEvent(FHotReloadEvent&amp; out)", ret: "取り出せたか", desc: "溜まった未処理イベントを 1 件 FIFO で取り出す。path は次の Consume/Clear/Shutdown まで watcher が所有する。" },
        { sig: "u32 WatchedCount() / u32 PendingEventCount()", desc: "監視中の path 数 / 未処理イベント数。" },
        { sig: "FHotReloadDiagnostics CaptureDiagnostics() const", ret: "診断値の snapshot", desc: "enqueue・coalesce・dispatch・拒否・loss の飽和 counter、直近失敗、全量 rescan 要求を allocation-free で値コピーする。成功操作では sticky な失敗状態を消さない。" },
        { sig: "void ClearDiagnostics()", desc: "診断 counter・直近失敗・rescan 要求だけを明示 clear する。監視登録、callback、pending event は維持する。" },
        { sig: "void ClearEvents()", desc: "未処理イベントを全クリア。callback 中は現在 event の残り callback を完走してから外側 drain を止め、後から enqueue した event を同じ tick に配信しない。" },
        { sig: "using FHotReloadWatcher = CHotReloadWatcher", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CHotReloadWatcher</code> を使う。" }
      ]
    },
    {
      name: "FHotReloadDiagnostics",
      kind: "構造体", header: "gameframework/HotReload.h",
      summary: "1 watcher の通知パイプラインを観測する allocation-free 診断 snapshot。全 counter は <code>u64</code> 最大値で飽和し、<code>last_failure</code> と authoritative rescan 要求は <code>ClearDiagnostics</code> まで sticky。",
      when: "通知欠落を正常 tick で見失わず、監視ダッシュボード、ログ、復旧処理から rescan 要否を判断する時。Shipping shell では storage を持たず決定的なゼロ値を返す。",
      members: [
        { sig: "u64 enqueued_event_count / coalesced_event_count", desc: "新規 pending pair の commit 件数 / debounce で既存 event へ統合した件数。" },
        { sig: "u64 dispatched_event_count", desc: "callback dispatch 対象として FIFO から取り出した event 件数。" },
        { sig: "u64 rejected_event_count / loss_incident_count", desc: "検証・queue 追加で拒否した event 件数 / 通知集合の完全性を保証できなくなった incident 件数。" },
        { sig: "EHotReloadResult last_failure", desc: "enqueue・native poll・dispatch で実際に最後に観測した非 Success 結果。TryTick の代表返却値に使う優先度集約では上書きせず、成功でも消さない。" },
        { sig: "bool authoritative_rescan_required", desc: "true ならディスクや asset DB などの正本から全量再走査が必要。" }
      ]
    },
    {
      name: "FHotReloadEvent",
      kind: "構造体", header: "gameframework/HotReload.h",
      summary: "1 件のホットリロード通知。「どのファイルが」「いつ」「更新か削除か」を表す<t>POD</t>。",
      when: "<code>ConsumeNextEvent</code> やコールバックで受け取り、該当アセットを再読み込みする時。",
      members: [
        { sig: "const char* file_path", desc: "watcher が所有する対象ファイルの借用パス。callback 引数ではその callback が戻るまで、<code>ConsumeNextEvent</code> の出力では次の <code>ConsumeNextEvent</code> / <code>ClearEvents</code> / <code>Shutdown</code> まで有効。" },
        { sig: "u64 modified_timestamp", desc: "変更検出時刻。" },
        { sig: "bool removed", desc: "true=削除、false=更新。" }
      ]
    },
    {
      name: "FHotReloadCallback",
      kind: "関数ポインタ型", header: "gameframework/HotReload.h",
      summary: "ホットリロード通知を受け取る<t>コールバック</t>の型。<code>void(*)(void* user, const FHotReloadEvent&amp;) noexcept</code>。<code>std::function</code> は使わず関数ポインタ + user で渡す。",
      when: "<code>RegisterCallback</code> に渡す関数を定義する時の型。"
    },
    {
      name: "EHotReloadResult",
      kind: "列挙(enum)", header: "gameframework/HotReload.h",
      summary: "登録済み/未登録、引数・UTF-8・path 長、各種 hard limit、OOM、OS error、再入、native notification overflow を区別する checked watcher 結果。<code>NativeOverflow</code> では監視は再 arm されるが caller 側の全量 rescan が必要。",
      members: [
        { sig: "const char* HotReloadResultName(EHotReloadResult result)", ret: "安定した診断名", desc: "全列挙値をログ向け名称へ変換する。範囲外の値は <code>Unknown</code>。" }
      ]
    },
    // =====================================================================
    // HungerSystem.h
    // =====================================================================
    {
      name: "CHungerSystem",
      kind: "クラス", header: "gameframework/HungerSystem.h",
      summary: "サバイバル系ゲーム向けに、複数の survivor (プレイヤー/NPC) の空腹・喉の渇き・疲労・正気度・体温などの「生存統計値」を秒単位で減らし、危険域に入ったら警告、0 で HP ダメージ通知まで行うマネージャ。",
      when: "DayZ / The Long Dark / Don't Starve のような「食べないと死ぬ」ゲームを作る時。HP 自体は持たず、危険/ダメージは<t>コールバック</t>で外部 (CHealthSystem) に橋渡しする。",
      members: [
        { sig: "void Init()", desc: "7 stat 分の設定を既定値で初期化。再呼び出しで全 survivor を破棄しリセット。" },
        { sig: "void ConfigureStat(ESurvivalStat stat, const FStatConfig& config)", desc: "stat ごとの減少速度 / 危険閾値 / 0 時ダメージを上書き。ゲーム起動時に一括設定する想定。" },
        { sig: "FSurvivorId AddSurvivor()", ret: "識別子", desc: "新規 survivor を登録 (全 stat が最大値スタート)。" },
        { sig: "void RemoveSurvivor(FSurvivorId id)", desc: "survivor を破棄。invalid/stale な id は no-op。" },
        { sig: "void RestoreStat / DrainStat / SetStat(...)", desc: "stat を回復 / 消費 / 絶対値設定。すべて [0, max] にクランプされる。" },
        { sig: "f32 GetStat(FSurvivorId id, ESurvivalStat stat) const", ret: "現在値", desc: "stat の現在値。invalid は 0。" },
        { sig: "bool IsCritical(FSurvivorId id, ESurvivalStat stat) const", desc: "その stat が危険閾値を下回っているか。" },
        { sig: "bool IsAlive(FSurvivorId id) const", desc: "全 stat が 0 でなく (HealthBridge があれば) HP&gt;0 か。" },
        { sig: "f32 OverallSurvivalHealth(FSurvivorId id) const", ret: "[0,1]", desc: "全 stat の (current/max) の平均。UI の生存度メータ用。" },
        { sig: "void Tick(f32 dt)", desc: "全 survivor × 全 stat を dt 秒減らし、閾値跨ぎで CriticalCallback、0 滞在で DamageCallback を発火。", when: "毎フレーム 1 回呼ぶ。" },
        { sig: "void SetOnCriticalCallback / SetOnDamageCallback(cb, user)", desc: "危険遷移 / ダメージ通知の<t>コールバック</t>登録。nullptr で外す。" },
        { sig: "u32 SurvivorCount() const / void ClearAll()", desc: "現在の survivor 数 / 全 survivor 破棄 (callback は保持)。" },
        { sig: "using FHungerSystem = CHungerSystem", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CHungerSystem</code> を使う。" }
      ]
    },
    {
      name: "ESurvivalStat",
      kind: "列挙(enum)", header: "gameframework/HungerSystem.h",
      summary: "生存統計値の種別。Hunger(空腹) / Thirst(渇き) / Energy(疲労) / Sanity(正気度) / Warmth(体温) / Custom1 / Custom2 の 7 種。",
      when: "CHungerSystem の各 API で「どの stat か」を指定する時。Custom1/2 はゲーム固有 (放射線・酸素など) の拡張枠。"
    },
    {
      name: "FStatConfig",
      kind: "構造体", header: "gameframework/HungerSystem.h",
      summary: "stat 種別ごとの設定 (全 survivor 共通)。上限値・1 秒あたりの減少量・危険閾値・0 滞在時の毎秒ダメージ。",
      when: "<code>ConfigureStat</code> に渡す。",
      members: [
        { sig: "f32 max_value", desc: "上限値 (AddSurvivor 直後の初期値)。0 以下は 1.0 扱い。" },
        { sig: "f32 decay_per_sec", desc: "毎秒の自然減少量。0 以下なら減らない。" },
        { sig: "f32 critical_threshold", desc: "下回ると危険フラグ + CriticalCallback 発火。" },
        { sig: "f32 zero_damage_per_sec", desc: "stat==0 のとき毎秒 DamageCallback に流すダメージ。0 以下なら死なない stat。" }
      ]
    },
    {
      name: "FStatState",
      kind: "構造体", header: "gameframework/HungerSystem.h",
      summary: "ある survivor の 1 stat の実状態。現在値・上限・危険フラグ。",
      when: "内部状態の表現 (主に CHungerSystem 内部で使われる)。"
    },
    {
      name: "FSurvivorId",
      kind: "構造体", header: "gameframework/HungerSystem.h",
      summary: "survivor を指す不透明な<t>ハンドル</t>。24bit の index + 8bit の<t>世代(generation)</t>を 1 つの u32 に詰めたもの。<code>m_Packed == 0</code> が無効。",
      when: "AddSurvivor の戻り値を保持し、以降の操作に渡す。スロット再利用後の古いハンドルは世代不一致で安全に弾かれる。",
      members: [
        { sig: "bool IsValid() const", desc: "有効なハンドルか (m_Packed != 0)。" },
        { sig: "static FSurvivorId Pack(u32 index, u8 gen)", desc: "index と世代からハンドルを作る。" },
        { sig: "u32 Index() / u8 Gen() const", desc: "内部の index / 世代を取り出す。" }
      ]
    },
    // =====================================================================
    // InputMap.h
    // =====================================================================
    {
      name: "FInputAxisOptions",
      kind: "構造体", header: "gameframework/InputAxisOptions.h",
      summary: "1D軸の合算値へデッドゾーン、非負倍率、反転を適用する12byteの値。デッドゾーン外を連続する0..1へ再正規化してから倍率を掛ける。",
      when: "移動や視点入力へ、物理バインドとは独立した感度・反転設定を適用したい時。FInputMap::AxisValueへ値渡しする。",
      members: [
        { sig: "f32 dead_zone = 0", desc: "絶対値がこの値以下なら0。有効範囲は0以上1未満。" },
        { sig: "f32 scale = 1", desc: "再正規化後の絶対値へ掛ける有限の非負倍率。" },
        { sig: "bool inverted = false", desc: "trueなら出力方向を反転。" },
        { sig: "bool IsValid() const noexcept", desc: "dead_zoneとscaleが有限かつ有効範囲ならtrue。" },
        { sig: "f32 Apply(f32 value) const noexcept", ret: "[-1,+1]", desc: "有限入力を制限して設定を適用。不正設定または非有限入力は0。" }
      ]
    },
    {
      name: "FInputMap",
      kind: "クラス", header: "gameframework/InputMap.h",
      summary: "物理キー/マウス/ゲームパッドを「Jump」「MoveX」のような<b>名前付きアクション</b>に束ねるマッピング層。ゲームロジックが物理キーから切り離され、キーコンフィグ UI も後付けできる。",
      when: "「W / 左stick = MoveForward」のように 1 actionへ複数入力を割り当てたい時。通常queryはplatform入力を読み、replay・AI・testでは明示snapshotをEvaluateへ渡す。",
      members: [
        { sig: "void BindKey(FActionId a, EKey key)", desc: "キーボードのキーをアクションに割り当てる。" },
        { sig: "void BindMouseButton(FActionId a, EMouseButton mb)", desc: "マウスボタンを割り当てる。" },
        { sig: "void BindGamepad(FActionId a, EGamepadButton gb, u32 player_index = 0)", desc: "ゲームパッドボタンを割り当てる。" },
        { sig: "void BindAxisKeys(FActionId a, EKey neg, EKey pos)", desc: "2 キーで -1/+1 の 1D 軸を作る (例: A=-1, D=+1)。両押しは 0。" },
        { sig: "bool TryBindGamepadAxis(FActionId a, EGamepadAxis axis, u32 player, const FInputAxisOptions& options)", desc: "3D移動や視点用axisを検証して割り当てる。不正設定ではbindingを変更しない。" },
        { sig: "FInputActionState Evaluate(FActionId a, const IInputStateView& input) const", desc: "明示入力からedge・保持・合成axisを一度に評価する。deviceやWorldへ依存しない。" },
        { sig: "u32 BindingCount() const", desc: "現在の物理binding数。" },
        { sig: "void Unbind(FActionId a) / void ClearAll()", desc: "指定アクションの全割り当てを削除 / 全削除。" },
        { sig: "bool IsPressed(FActionId a) const", desc: "このフレームで押された瞬間か (OR セマンティクス: どれか 1 つでも該当で true)。" },
        { sig: "bool IsHeld(FActionId a) const", desc: "押されているか。" },
        { sig: "bool IsReleased(FActionId a) const", desc: "このフレームで離された瞬間か。" },
        { sig: "f32 Axis(FActionId a) const", ret: "[-1,+1]", desc: "1D 軸値。複数 axis 割り当ては累積して clamp(-1,+1)。" },
        { sig: "f32 AxisValue(FActionId a, FInputAxisOptions options) const", ret: "[-1,+1]", desc: "既存Axisの合算値へ検査済みのデッドゾーン、倍率、反転を後処理。" }
      ]
    },
    {
      name: "FInputActionState",
      kind: "構造体", header: "gameframework/InputActionState.h",
      summary: "一つの名前付きactionを一入力時点から評価した結果。押下・保持・解放と、[-1,+1]の合成axisをまとめて持つ。",
      when: "固定tickの移動、姿勢、視点、決定などを <code>FInputMap::Evaluate</code> から受け取る時。"
    },
    {
      name: "IInputStateView",
      kind: "インターフェース", header: "gameframework/InputStateView.h",
      summary: "名前付きactionの評価に必要なキー、マウスボタン、ゲームパッド状態を読む境界。",
      when: "platform、AI、replay、testの入力を同じ <code>FInputMap</code> へ渡したい時。"
    },
    {
      name: "FInputStateSnapshot",
      kind: "クラス", header: "gameframework/InputStateSnapshot.h",
      summary: "一入力時点の物理入力を所有する値。同じsnapshotとinput mapから同じaction結果を再現できる。setterは範囲外値を拒否する。",
      when: "deviceなしのtest、AI入力、replay、rollback用に入力状態を明示して保持する時。",
      members: [
        { sig: "void Clear()", desc: "全入力を離された初期状態へ戻す。" },
        { sig: "bool TrySetKeyState(...) / TrySetMouseButtonState(...) / TrySetGamepadButtonState(...)", desc: "保持と今回の押下・解放を検証して設定する。" },
        { sig: "bool TrySetGamepadAxis(u32 player, EGamepadAxis axis, f32 value)", desc: "正規化済みaxisを設定する。非有限値と範囲外値は拒否する。" }
      ]
    },
    {
      name: "FFixedStepInputBuffer",
      kind: "クラス", header: "gameframework/FixedStepInputBuffer.h",
      summary: "可変frame入力を固定tickへ渡す状態保持器。最新の保持状態・axisと、未消費の押下・解放を分けて管理する。",
      when: "短いtapを固定更新まで保持し、catch-up中の同じedgeを一度だけ通知したい時。",
      members: [
        { sig: "bool TryPushFrame(const IInputStateView& input)", desc: "frame入力を蓄積する。不正入力では既存状態を保つ。" },
        { sig: "bool TryConsumeFixedStep(FInputStateSnapshot& out)", desc: "固定tick用snapshotを返し、押下・解放だけを消費する。" },
        { sig: "bool TryCaptureSnapshot(...) / bool TryRestoreSnapshot(...)", desc: "未消費入力を保存・復元する。" },
        { sig: "using CFixedStepInputBuffer = FFixedStepInputBuffer", desc: "旧名向け互換別名。" }
      ]
    },
    {
      name: "FFixedStepInputBufferSnapshot",
      kind: "構造体", header: "gameframework/FixedStepInputBufferSnapshot.h",
      summary: "固定入力bufferの未消費入力と初期化状態を所有する保存値。",
      when: "固定更新位置をrollbackするため、入力edgeを時計と一緒に保存する時。"
    },
    {
      name: "IInputFrameSource",
      kind: "インターフェース", header: "gameframework/InputFrameSource.h",
      summary: "描画frameごとの所有snapshotをCGameへ供給する非所有の差し替え境界。",
      when: "Player以外のAI入力やheadless test入力を固定更新まで蓄積したい時。"
    },
    {
      name: "IFixedTickInputSource",
      kind: "インターフェース", header: "gameframework/FixedTickInputSource.h",
      summary: "0起点の固定tick番号ごとに決定論入力snapshotをCGameへ供給する非所有の差し替え境界。",
      when: "replay、rollback、network lockstepでtick入力を再発行したい時。"
    },
    {
      name: "FFixedStepRuntimeSnapshot",
      kind: "構造体", header: "gameframework/FixedStepRuntimeSnapshot.h",
      summary: "固定時計とactive sceneの未消費入力を同じ復元境界で保持するprocess内保存値。取得元game・scene・sourceも識別する。",
      when: "同じ実行中gameで固定tickをrollbackし、時計と入力edgeをずらさず復元する時。"
    },
    {
      name: "CPlatformInputStateAdapter",
      kind: "クラス", header: "gameframework/PlatformInputStateAdapter.h",
      summary: "現在の <code>CInput</code> を検証済み <code>FInputStateSnapshot</code> へ一度だけ取得するplatformアダプター。",
      when: "platform入力を明示snapshotとして評価・蓄積する時。通常はCGameが自動で使う。"
    },
    {
      name: "FActionId",
      kind: "構造体", header: "gameframework/InputMap.h",
      summary: "アクション識別子。<code>FActionId(\"Jump\")</code> のように文字列リテラルから<t>コンパイル時ハッシュ</t> (FNV-1a) で生成され、中身は u32。実行時の文字列比較が無い。",
      when: "FInputMap の bind / query に「アクション名」を渡す時。同じ文字列なら常に同じ値。",
      members: [
        { sig: "constexpr FActionId(const char* name)", desc: "名前から<t>コンパイル時ハッシュ</t>で生成。" },
        { sig: "constexpr bool operator==/!=(FActionId) const", desc: "u32 値の一致比較。" }
      ]
    },
    {
      name: "ActionHash",
      kind: "関数", header: "gameframework/InputMap.h",
      summary: "文字列を 32bit の FNV-1a ハッシュに変換する <code>constexpr</code> 関数。FActionId の内部で使われる。",
      when: "通常は FActionId 経由で十分。ハッシュ値を直接欲しい時のみ。"
    },
    // =====================================================================
    // InputRecorder.h
    // =====================================================================
    {
      name: "CInputRecorder",
      kind: "クラス", header: "gameframework/InputRecorder.h",
      summary: "OS 寄りの<b>生 (raw) 入力</b> (キー変化 + マウス座標 + ボタン) を 1 tick = 1 サンプルとして録画し、後でそのまま再生できるレコーダ。<code>.acsr</code> 形式でファイル保存も可能。",
      when: "TAS / 自動テスト / バグ再現で「同じ入力列をピクセル単位で再現」したい時。ゲーム解釈済み入力を扱う CLockstep とは別レイヤ (こちらは OS 信号寄り)。",
      members: [
        { sig: "void StartRecording(u32 tick_rate_hz = 60) / StopRecording()", desc: "録画開始 / 停止。tick_rate は再生時の整合検証用 (ファイルに保存)。" },
        { sig: "void StartReplay() / StopReplay()", desc: "再生開始 (cursor を先頭へ) / 停止。録画済み sample は保持。" },
        { sig: "void Capture(const FInputSample& s)", desc: "1 サンプル記録。Recording 以外では no-op。" },
        { sig: "bool ConsumeSample(u32 tick, FInputSample& out)", ret: "見つかったか", desc: "指定 tick のサンプルを取り出す。Replaying 以外では false。線形検索 + cursor 前進で amortised O(1)。" },
        { sig: "ERecorderMode CurrentMode() const", desc: "現在のモード (Idle/Recording/Replaying)。" },
        { sig: "u32 SampleCount() / CurrentTick() / TickRateHz() const", desc: "記録件数 / 現在 tick / tick レート。" },
        { sig: "void Clear()", desc: "全サンプル破棄 + cursor リセット。モードは保持。" },
        { sig: "TResult<void> SaveToBuffer(u8* buf, u32 size, u32& out_written)", desc: "現在のサンプルを <code>.acsr</code> レイアウトでバッファに書き出す (CRC32 付き、round-trip 検証済み)。" },
        { sig: "TResult<void> LoadFromBuffer(const u8* buf, u32 size)", desc: "<code>.acsr</code> バッファを検証してサンプルを置換復元する。" },
        { sig: "using FInputRecorder = CInputRecorder", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CInputRecorder</code> を使う。" }
      ]
    },
    {
      name: "CInputRecorderFixedTickSource",
      kind: "クラス", header: "gameframework/InputRecorderFixedTickSource.h",
      summary: "<code>CInputRecorder</code> のraw key・mouse button sample列を固定tick入力へ変換する非所有アダプター。巻き戻し要求では先頭から状態を再構築する。",
      when: "既存のraw入力記録を <code>CGame::SetFixedTickInputSource</code> へ接続し、3D操作やgame logicを同じtickで再生する時。",
      members: [
        { sig: "CInputRecorderFixedTickSource(const CInputRecorder&, FInputSampleKeyDecoder)", desc: "recorderとplatform固有raw key decoderを非所有参照で保持する。" },
        { sig: "bool TryCaptureFixedTickInput(u64 tick, FInputStateSnapshot& out)", desc: "指定tickまでの変化を適用する。不正sampleでは状態と出力を保つ。" },
        { sig: "void Reset() / u32 TickRateHz() const", desc: "読み取り位置を初期化 / 記録時tick rateを取得する。" },
        { sig: "using FInputRecorderFixedTickSource = CInputRecorderFixedTickSource", desc: "旧名向け互換別名。" }
      ]
    },
    {
      name: "FInputRecordingView",
      kind: "クラス", header: "gameframework/InputRecordingView.h",
      summary: "caller-owned の <code>.acsr</code> バイト列を全検証し、sample 領域を<b>コピーせず借用</b>する immutable decoder。header、製品上限、厳密サイズ、CRC32、mouse の有限値を allocation 前に検査する。",
      when: "mapped file や受信バッファを所有配列へ展開せず、安全に順次参照したい時。記録を長期保持・編集する場合は <code>CInputRecorder::TryLoadFromBuffer</code> を使う。",
      members: [
        { sig: "static TResult&lt;FInputRecordingView&gt; Decode(TSpan&lt;const u8&gt; bytes)", ret: "検証済み view または FErrorCode", desc: "magic/version、tick rate・件数上限、厳密サイズ、CRC32、全 sample の有限 mouse 値を検査する。null、truncation、破損、上限超過は失敗し、入力を所有しない。" },
        { sig: "bool DecodeSample(u32 index, FInputSample& out) const", ret: "範囲内なら true", desc: "固定 29 byte の ABI 非依存 layout から一件を復元する。範囲外では false を返し、out を変更しない。" },
        { sig: "u32 TickRateHz() const / SampleCount() const", desc: "検証済み header の tick rate と sample 件数を返す。" }
      ]
    },
    {
      name: "FInputSample",
      kind: "構造体", header: "gameframework/InputRecorder.h",
      summary: "1 tick 分の生入力サンプル (<t>POD</t>)。状態変化したキー (最大 8) + マウス座標 + マウスボタン bitmask を持つ。",
      when: "OS の raw input / SDL イベントを詰めて <code>Capture</code> に渡す時、または再生で受け取る時。",
      members: [
        { sig: "u32 tick", desc: "フレーム番号 (0 起点)。" },
        { sig: "u8 key_codes_changed[8] / u8 key_states[8]", desc: "状態変化したキーコードと、その press(1)/release(0)。未使用枠は 0。" },
        { sig: "FVec2 mouse_pos", desc: "マウス位置 (window-local px)。" },
        { sig: "u8 mouse_button_states", desc: "マウスボタン bitmask (L=bit0, R=bit1, M=bit2 ...)。" }
      ]
    },
    {
      name: "ERecorderMode",
      kind: "列挙(enum)", header: "gameframework/InputRecorder.h",
      summary: "CInputRecorder の動作モード。Idle (何もしない) / Recording (録画) / Replaying (再生) の 3 値。",
      when: "<code>CurrentMode()</code> で現在の状態を判定する時。"
    },
    // =====================================================================
    // InspectorSeam.h
    // =====================================================================
    {
      name: "CInspectorSeam",
      kind: "クラス", header: "gameframework/InspectorSeam.h",
      summary: "ゲーム内オブジェクトが自分のフィールドを公開する<t>プロバイダ</t>を集める「デバッグインスペクタの<t>シーム(seam)</t>」。UI レイヤ (ImGui 等) はここを回してフィールドを描画/編集する。",
      when: "実行中に Player の HP や速度を画面で覗いて書き換えたい時。本体はレジストリだけで、実際の描画は別レイヤ。出荷ビルドでは登録ごと消す前提。",
      members: [
        { sig: "void Init()", desc: "初期化 (将来 ImGui コンテキスト等の予約点)。多重呼び出し可。" },
        { sig: "void RegisterProvider(IInspectableProvider* p)", desc: "<t>プロバイダ</t>を登録 (非所有)。同一ポインタの二重登録・nullptr は無視。" },
        { sig: "void RegisterProviderForNode(FNodeId node, IInspectableProvider* p)", desc: "FNodeId 紐付きで登録。後で <code>GetProviderForNode</code> で逆引きできる。", when: "シーングラフの特定ノードに対応する provider を引きたい時。" },
        { sig: "void UnregisterProvider(IInspectableProvider* p)", desc: "登録解除 (provider 自体は破棄しない)。" },
        { sig: "u32 ProviderCount() const", desc: "登録済み provider 数。" },
        { sig: "IInspectableProvider* GetProvider(u32 index) const", ret: "provider or null", desc: "登録順 index で provider を取得。範囲外は nullptr。FNodeId の Index() とは別物なので注意。" },
        { sig: "IInspectableProvider* GetProviderForNode(FNodeId node) const", desc: "FNodeId に完全一致する provider を逆引き。node-keyed lookup の正しい入口。" },
        { sig: "void NotifyFieldChanged(u32 provider_index, u32 obj_index, u32 field_index)", desc: "UI が値を書き換えた後に呼び、該当 provider の OnFieldChanged へ転送する。" },
        { sig: "void ClearAll()", desc: "全登録を破棄 (provider 自体は残る)。" },
        { sig: "using FInspectorSeam = CInspectorSeam", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CInspectorSeam</code> を使う。" }
      ]
    },
    {
      name: "IInspectableProvider",
      kind: "インターフェース", header: "gameframework/InspectorSeam.h",
      summary: "自分 (または管理下のオブジェクト群) をインスペクタに公開する抽象<t>インターフェース</t>。ゲーム側クラスが継承して実装する。",
      when: "Player や Camera を実行時に覗ける/編集できるようにしたい時。1 provider が N オブジェクトを公開する Manager 型にもできる。",
      members: [
        { sig: "virtual u32 ObjectCount() noexcept = 0", desc: "公開するオブジェクト数。0 なら何も描かれない。" },
        { sig: "virtual FInspectableObject GetObject(u32 index) noexcept = 0", desc: "index 番目のオブジェクト記述を返す。fields 配列は provider 所有。" },
        { sig: "virtual void OnFieldChanged(u32 obj_index, u32 field_index) noexcept = 0", desc: "UI が値を書き換えた直後に呼ばれる。clamp / 再計算 / dirty フラグをここで。" }
      ]
    },
    {
      name: "FInspectableField",
      kind: "構造体", header: "gameframework/InspectorSeam.h",
      summary: "公開する 1 フィールドの記述。表示名 + 型タグ (<code>EFieldKind</code>) + データへのポインタ + (enum 用) ラベル配列。",
      when: "GetObject() が返す配列の 1 要素として組む時。",
      members: [
        { sig: "const char* name", desc: "表示名 (借用)。" },
        { sig: "EFieldKind kind", desc: "データの型タグ。描画側はこれで switch してキャストする。" },
        { sig: "void* data", desc: "該当型へのポインタ。" },
        { sig: "u32 enum_value_count / const char** enum_values", desc: "kind == Enum のときの値→ラベル配列とその長さ。" }
      ]
    },
    {
      name: "FInspectableObject",
      kind: "構造体", header: "gameframework/InspectorSeam.h",
      summary: "provider が公開する 1 オブジェクト。型名 + インスタンス名 + フィールド配列 + 件数。",
      when: "<code>GetObject()</code> の戻り値として組み立てる時。fields 配列の寿命は provider 所有。",
      members: [
        { sig: "const char* type_name / instance_name", desc: "クラス名相当 (\"Player\") とインスタンス名 (\"P1\" / \"Boss\")。" },
        { sig: "FInspectableField* fields / u32 field_count", desc: "公開フィールド配列とその件数。" }
      ]
    },
    {
      name: "EFieldKind",
      kind: "列挙(enum)", header: "gameframework/InspectorSeam.h",
      summary: "インスペクタのフィールド型タグ。Bool / I32 / U32 / F32 / Vec2-4 / String (read-only) / Enum。",
      when: "FInspectableField の <code>kind</code> に入れ、描画側が switch して <code>data</code> を正しい型にキャストする時。"
    },
    // =====================================================================
    // InventorySystem.h
    // =====================================================================
    {
      name: "CInventorySystem",
      kind: "クラス", header: "gameframework/InventorySystem.h",
      summary: "「N 個の固定スロット × スタック可能アイテム」モデルのインベントリ管理。RPG / サバイバル / クラフト系で定番の持ち物画面のロジックを直接提供する。",
      when: "拾った/報酬で得たアイテムをプレイヤーが持ち歩く状態を管理したい時。即時購入は CEconomyDirector、装着 cosmetic は CCharacterCustomizer と役割分担。",
      members: [
        { sig: "void Init(u32 slot_count = 30)", desc: "スロット数を設定 (固定長)。数が変わると内容クリアして resize、同数なら no-op。" },
        { sig: "void RegisterItem(const FItemDef& def)", desc: "アイテム定義を登録 (起動時に 1 度ずつ)。同 id の二重登録は WARN で無視。" },
        { sig: "const FItemDef* FindItem(const char* id) const", desc: "id から定義を引く。未登録は nullptr。" },
        { sig: "u32 AddItem(const char* id, u32 count)", ret: "追加できた個数", desc: "既存スタックに積み増し → 余りを空スロットへ。満杯なら入った分だけ返す。" },
        { sig: "u32 RemoveItem(const char* id, u32 count)", ret: "削除できた個数", desc: "全スロット横断で前方優先に減らす。在庫不足なら減った分だけ返す。" },
        { sig: "bool HasItem(const char* id, u32 min_count = 1) const", desc: "min_count 個以上持っているか。" },
        { sig: "u32 ItemTotal(const char* id) const", desc: "全スロット合計の所持数。" },
        { sig: "bool MoveSlot(u32 from, u32 to)", desc: "スロット間移動。空なら移動、同 item なら merge、別 item なら swap。異常時 false。", when: "UI のドラッグ&ドロップ。" },
        { sig: "bool DropSlot(u32 index)", desc: "スロットを空にする (地面に落とす想定)。<code>can_drop==false</code> や空/範囲外は false。" },
        { sig: "const FInventorySlot* GetSlot(u32 index) const", desc: "スロット内容を取得。範囲外は nullptr。" },
        { sig: "u32 SlotCount() / u32 EmptySlotCount() const", desc: "総スロット数 / 空きスロット数 (\"X/Y 使用\" 表示用)。" },
        { sig: "void SetOnChangeCallback(ChangeCallback cb, void* user)", desc: "スロット内容が変わるたびに呼ぶ<t>コールバック</t> (UI 反映 / SFX)。" },
        { sig: "void ClearAll()", desc: "定義 + スロット内容を全クリア (再 Init 必須)。" },
        { sig: "using FInventorySystem = CInventorySystem", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CInventorySystem</code> を使う。" }
      ]
    },
    {
      name: "FItemDef",
      kind: "構造体", header: "gameframework/InventorySystem.h",
      summary: "アイテム 1 種類の定義 (不変)。id・表示名・分類・最大スタック数・アイコンパス・落とせるか。",
      when: "<code>RegisterItem</code> に渡す。文字列はすべて非所有 (リテラル想定)。",
      members: [
        { sig: "const char* id / display_name / icon_path", desc: "一意キー / UI 表示名 / アイコン画像パス (すべて非所有)。" },
        { sig: "EItemCategory category", desc: "分類 (UI フィルタ用、Manager は値保存のみ)。" },
        { sig: "u32 max_stack", desc: "同スロットに重ねられる最大数。1=スタック不可。0 指定は 1 にクランプ。" },
        { sig: "bool can_drop", desc: "DropSlot で落とせるか。quest / key 系は false 推奨。" }
      ]
    },
    {
      name: "FInventorySlot",
      kind: "構造体", header: "gameframework/InventorySystem.h",
      summary: "1 スロットの内容。入っているアイテム id (nullptr=空) とスタック数。",
      when: "<code>GetSlot</code> の戻り値として UI 描画に使う時。",
      members: [
        { sig: "const char* item_id", desc: "入っているアイテム id (借用)。nullptr=空スロット。" },
        { sig: "u32 count", desc: "スタック数 (空のとき 0)。" }
      ]
    },
    {
      name: "EItemCategory",
      kind: "列挙(enum)", header: "gameframework/InventorySystem.h",
      summary: "アイテム分類。Consumable / Equipment / Material / Quest / Keys / Cosmetic。",
      when: "UI のタブフィルタや Drop の挙動分岐の参考に使う (Manager は中身を解釈しない)。"
    },
    // =====================================================================
    // LapTimer.h
    // =====================================================================
    {
      name: "CLapTimer",
      kind: "クラス", header: "gameframework/LapTimer.h",
      summary: "レースゲームの<b>ラップ計測 + 順位算出</b>を一元管理するマネージャ。チェックポイント順序検証・ベストラップ更新・フィニッシュ判定・リアルタイム順位ソートをまとめる。",
      when: "レーシング系を作る時。トラックの collider から通過通知を投げ、UI は順位/統計を読むだけで成立する。ショートカットや逆走は順序検証で弾く。",
      members: [
        { sig: "void Init(u32 total_laps, u32 checkpoints_per_lap)", desc: "周回数 / 1 周の checkpoint 数を設定。racer は維持して state を Stopped に。" },
        { sig: "FRacerId AddRacer(const char* display_name) / void RemoveRacer(FRacerId)", desc: "racer 追加 (名前は非所有) / 削除 (スロットは世代付きで再利用)。" },
        { sig: "void Start() / Stop() / Pause() / Resume()", desc: "レース開始 (stats リセット) / 終了確定 / 一時停止 / 再開。" },
        { sig: "bool IsRunning() const", desc: "Running 中なら true。" },
        { sig: "void NotifyCheckpointPassed(FRacerId id, u32 checkpoint_index)", desc: "checkpoint 通過。<code>expected_checkpoint</code> 一致のみ受理 (ショートカット防止)。", when: "トラック中間の collider を踏んだ時。" },
        { sig: "void NotifyLapCompleted(FRacerId id)", desc: "フィニッシュライン通過。全 checkpoint 踏破済みならラップ確定 + LapCallback、total_laps 到達で FinishCallback + 順位確定。" },
        { sig: "void Tick(f32 dt)", desc: "Running 中、全 racer の累計時間を dt 加算。Pause/Stop 中は no-op。" },
        { sig: "f32 RaceTimeSec() const", desc: "Start からの累計秒数 (レースクロック)。" },
        { sig: "const FRacerStats* GetStats(FRacerId id) const", desc: "racer 統計を const 参照。invalid / gen 不一致は nullptr。" },
        { sig: "FRacerId GetLeader() const / u32 PositionOf(FRacerId) const", desc: "現在の 1 位 / 1-origin の現在順位。" },
        { sig: "u32 LapRecordCount(FRacerId) const / const FLapRecord* GetLapRecord(FRacerId, u32)", desc: "完了ラップ数 / n 番目のラップ記録。" },
        { sig: "void SetOnLapCallback / SetOnFinishCallback(cb, user)", desc: "ラップ完了 / レース完了の<t>コールバック</t>登録。" },
        { sig: "u32 RacerCount() const / void ClearAll()", desc: "登録 racer 数 / 全 racer 破棄 (Init 設定と callback は保持)。" },
        { sig: "using FLapTimer = CLapTimer", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CLapTimer</code> を使う。" }
      ]
    },
    {
      name: "FRacerId",
      kind: "構造体", header: "gameframework/LapTimer.h",
      summary: "レース参加者の<t>ハンドル</t>。24bit index + 8bit <t>世代(generation)</t>を packed した u32。0 が無効。",
      when: "AddRacer の戻り値を保持し、通知や問い合わせに渡す。スロット再利用後の古いハンドルは世代不一致で弾かれる。",
      members: [
        { sig: "constexpr u32 Index() / u8 Generation() const", desc: "内部 index / 世代。" },
        { sig: "bool IsValid() const", desc: "有効か (m_Packed != 0)。" }
      ]
    },
    {
      name: "FRacerStats",
      kind: "構造体", header: "gameframework/LapTimer.h",
      summary: "レース中/終了時に読む racer 統計。名前・現在ラップ・通過 checkpoint 数・累計時間・ベストラップ・順位。",
      when: "<code>GetStats</code> で受け取り、HUD に表示する時。",
      members: [
        { sig: "const char* display_name", desc: "AddRacer で渡した名前 (非所有)。" },
        { sig: "u32 current_lap / checkpoints_passed", desc: "完了周回数 / 現ラップで踏んだ checkpoint 数。" },
        { sig: "f32 total_time_sec / best_lap_time_sec", desc: "累計秒数 / 自己ベストラップ秒 (未確定 0)。" },
        { sig: "u32 racer_position", desc: "1-origin 順位 (未計算 0、フィニッシュ後は確定値で固定)。" }
      ]
    },
    {
      name: "FLapRecord",
      kind: "構造体", header: "gameframework/LapTimer.h",
      summary: "1 ラップ分の結果スナップショット。ラップ番号・そのラップ単体の秒数・累計秒数・自己ベスト更新フラグ。",
      when: "<code>GetLapRecord</code> でラップ履歴を表示する時。",
      members: [
        { sig: "u32 lap_index", desc: "周回番号 (1-origin)。" },
        { sig: "f32 lap_time_sec / split_time_sec", desc: "このラップ単体の秒数 / Start からの累計秒数。" },
        { sig: "bool is_personal_best", desc: "自己ベスト更新だったか。" }
      ]
    },
    {
      name: "LapCallback / FinishCallback",
      kind: "関数ポインタ型", header: "gameframework/LapTimer.h",
      summary: "ラップ完了 / レース完了 (total_laps 到達) の通知<t>コールバック</t>型。どちらも <code>void* user</code> + FRacerId + 詳細を受ける。",
      when: "<code>SetOnLapCallback</code> / <code>SetOnFinishCallback</code> に渡す関数の型。"
    },
    // =====================================================================
    // LlmSafetyPipeline.h
    // =====================================================================
    {
      name: "CLlmSafetyPipeline",
      kind: "クラス", header: "gameframework/LlmSafetyPipeline.h",
      summary: "<t>LLM</t> で喋る NPC のセリフ生成経路に対し、入力検証・jailbreak 検出・<t>PII</t>除去・コンテンツレーティング・トークン予算・refusal 強制を bit flag で束ねた安全パイプライン。",
      when: "プレイヤー入力を LLM に渡す前 (<code>ValidateInput</code>) と、LLM 応答を画面に出す前 (<code>FilterOutput</code>) に通す時。ルールベースで常に同じ判定を返すので「NPC が黙る/別セリフ」分岐を確実に書ける。",
      members: [
        { sig: "void Init(ESafetyRule rules = ESafetyRule::Default)", desc: "bit flag で有効機能を選んで初期化。既定は全機能 on。" },
        { sig: "void SetTokenBudget(u32 max_in, u32 max_out)", desc: "入出力のトークン上限 (簡易: 1 token ≒ 4 byte)。0 で当該方向のチェック無効。" },
        { sig: "void SetCharacterAnchor(const char* system_prompt)", desc: "NPC キャラの system prompt (refusal 判定の基準)。非所有。nullptr で RefusalEnforcement は実質無効。" },
        { sig: "FSafetyResult ValidateInput(const char* user_text)", ret: "判定結果", desc: "ユーザー入力を LLM 投入前に検証。空/長すぎ/jailbreak で Refused、トークン超過で BudgetExceeded。" },
        { sig: "FSafetyResult FilterOutput(const char* llm_response)", ret: "判定結果", desc: "LLM 応答を表示前に検証/フィルタ。判定順は Refusal → Rating → PII。PII は <code>[REDACTED]</code> 置換で Filtered。" },
        { sig: "bool IsRuleEnabled(ESafetyRule) const / void EnableRule(ESafetyRule, bool)", desc: "個別ルールの on/off 照会・切替。" },
        { sig: "u32 RefusedCount() / FilteredCount() const", desc: "拒否 / フィルタ回数の統計 (テレメトリ用)。" },
        { sig: "void Reset()", desc: "統計とキャラ anchor を初期化 (rules / budget は保持)。" },
        { sig: "using FLlmSafetyPipeline = CLlmSafetyPipeline", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CLlmSafetyPipeline</code> を使う。" }
      ]
    },
    {
      name: "ESafetyRule",
      kind: "列挙(enum)", header: "gameframework/LlmSafetyPipeline.h",
      summary: "どの安全機能を有効化するかの bit flag。InputValidation / JailbreakDetection / PiiRedaction / ContentRating / TokenBudget / RefusalEnforcement と、全 on の <code>Default</code>。",
      when: "Init や EnableRule で「PII 除去だけ外す」等、シーン別に機能を取捨選択する時。OR / AND 演算子も定義済み。"
    },
    {
      name: "ESafetyVerdict",
      kind: "列挙(enum)", header: "gameframework/LlmSafetyPipeline.h",
      summary: "パイプラインの判定結果。Pass (通過) / Refused (拒否) / Filtered (危険箇所だけ除去して通過) / BudgetExceeded (トークン予算超過)。",
      when: "<code>FSafetyResult::verdict</code> を分岐して表示を決める時。"
    },
    {
      name: "FSafetyResult",
      kind: "構造体", header: "gameframework/LlmSafetyPipeline.h",
      summary: "1 回の Validate / Filter 呼び出しの結果。判定・フィルタ済みテキスト・拒否理由・概算トークン数。",
      when: "ValidateInput / FilterOutput の戻り値を受け取る時。<b>文字列は内部 static バッファを指す</b>ので、次の呼び出し前にコピー or 消費すること。",
      members: [
        { sig: "ESafetyVerdict verdict", desc: "判定結果。" },
        { sig: "const char* filtered_text", desc: "Pass/Filtered で有効な (置換済み) テキスト。それ以外 nullptr。" },
        { sig: "const char* refusal_reason", desc: "Refused で有効な拒否理由。" },
        { sig: "u32 input_tokens / output_tokens", desc: "概算トークン数 (FilterOutput でのみ output が有効)。" }
      ]
    },
    // =====================================================================
    // LocalizationDirector.h
    // =====================================================================
    {
      name: "CLocalizationDirector",
      kind: "クラス", header: "gameframework/LocalizationDirector.h",
      summary: "「ロケール + 文字列 ID」を「翻訳済みの文字列」に解決する<t>i18n</t> (多言語化) ストア。UI ラベル / セリフ / メニュー文言を言語非依存に引ける。",
      when: "ゲームを多言語対応させたい時。現 locale → Default(En) → key 自身、の 3 段フォールバックで、未翻訳箇所が画面で key として見えるので発見しやすい。",
      members: [
        { sig: "void SetLocale(ELocale loc) / ELocale CurrentLocale() const", desc: "取得対象ロケールの切替 / 現在値。UI 言語切替で呼ぶ。" },
        { sig: "void RegisterString(ELocale loc, const char* key, const char* value)", desc: "指定ロケールに key→value を 1 件登録。key/value とも非所有 (リテラル想定)。" },
        { sig: "const char* Get(const char* key) const", ret: "翻訳済み文字列", desc: "現 locale で引く → 無ければ Default(En) → それでも無ければ key 自身を返す。key が nullptr なら空文字。" },
        { sig: "const char* GetForLocale(ELocale loc, const char* key) const", desc: "指定 locale で引く (Default フォールバックなし、欠落は key 自身)。" },
        { sig: "bool Has(const char* key) const", desc: "現 locale に key が登録されているか (Default は見ない)。" },
        { sig: "u32 KeyCount(ELocale loc) const", desc: "指定 locale の登録件数。" },
        { sig: "void Clear() / void ClearLocale(ELocale loc)", desc: "全削除 / 指定 locale のみ削除。" },
        { sig: "using FLocalizationDirector = CLocalizationDirector", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CLocalizationDirector</code> を使う。" }
      ]
    },
    {
      name: "ELocale",
      kind: "列挙(enum)", header: "gameframework/LocalizationDirector.h",
      summary: "ロケール識別子。En / Ja / Fr / De / Es / ZhCn / ZhTw / Ko / Pt / Ru / It の 11 言語 + <code>Default(=En)</code>。",
      when: "RegisterString / SetLocale / GetForLocale で言語を指定する時。値は永続化互換のため連番固定 (追加は末尾)。"
    },
    // =====================================================================
    // Lockstep.h
    // =====================================================================
    {
      name: "CLockstep",
      kind: "クラス", header: "gameframework/Lockstep.h",
      summary: "<t>lockstep</t> ネットコードの入力レイヤ。1 tick = 1 フレーム分の入力 (FInputFrame) を記録/配信し、全クライアントが同じ順序で消費することで<t>決定論</t>シミュレーションを実現する土台。入力リプレイ・同期ずれ検証も担う。",
      when: "格闘 / RTS / GGPO 系のように「結果」ではなく「入力」を配信して全員が同じ計算を回すネット対戦や、replay / TAS を作る時。",
      members: [
        { sig: "void Init(ENetMode mode, u32 tick_rate_hz = 60)", desc: "モード設定 + tick/cursor リセット。既存 frames は破棄しない。" },
        { sig: "void RecordInput(const FInputFrame& frame)", desc: "入力 1 件を記録。Replay モード中は no-op。内部 tick を frame.tick+1 へ進める。" },
        { sig: "void StartReplay()", desc: "Replay モードに切替、cursor を先頭へ。既存 frames を保持。" },
        { sig: "bool ConsumeInput(u32 tick, u32 player_id, FInputFrame& out)", ret: "見つかったか", desc: "指定 tick/player の入力を取り出す。Replay 以外では false。線形検索 + cursor 前進。" },
        { sig: "u32 CurrentTick() / u32 InputCount() / ENetMode Mode() / u32 TickRateHz() const", desc: "現在 tick / 記録件数 / モード / tick レート。" },
        { sig: "u64 ComputeChecksum() const", ret: "ハッシュ", desc: "全 frame の FNV-1a-like u64 ハッシュ。<t>決定論</t>検証 / replay 同期ずれ検知に使う。" },
        { sig: "void Clear()", desc: "全 frames 破棄 + cursor/tick リセット。Mode は保持。" },
        { sig: "TResult<void> SaveToBuffer(u8* buf, u32 size, u32& out_written)", desc: "frames を <code>.acsl</code> レイアウトで書き出す (magic 'ACSL' + CRC32、round-trip 検証済み)。" },
        { sig: "TResult<void> LoadFromBuffer(const u8* buf, u32 size)", desc: "<code>.acsl</code> バッファを検証して frames を置換復元する。" },
        { sig: "using FLockstep = CLockstep", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CLockstep</code> を使う。" }
      ]
    },
    {
      name: "FInputFrame",
      kind: "構造体", header: "gameframework/Lockstep.h",
      summary: "1 tick / 1 プレイヤー分の入力 (<t>POD</t>)。tick・player_id・ボタン bitmask (8 ボタン)・アナログスティック軸。",
      when: "<code>RecordInput</code> に渡す/<code>ConsumeInput</code> で受け取る時。",
      members: [
        { sig: "u32 tick / u32 player_id", desc: "フレーム番号 / プレイヤー ID。" },
        { sig: "u8 buttons", desc: "ボタン bitmask (最大 8)。" },
        { sig: "FVec2 axis", desc: "アナログスティック (-1..1 想定、範囲チェックはしない)。" }
      ]
    },
    {
      name: "ENetMode",
      kind: "列挙(enum)", header: "gameframework/Lockstep.h",
      summary: "CLockstep の動作モード。Local (単独/記録のみ) / Lockstep (ネット対戦) / Replay (再生、記録は受け付けない)。",
      when: "<code>Init</code> や <code>Mode()</code> でモードを扱う時。"
    },
    // =====================================================================
    // MatchGrid.h
    // =====================================================================
    {
      name: "CMatchGrid",
      kind: "クラス", header: "gameframework/MatchGrid.h",
      summary: "Bejeweled / Candy Crush 系 <t>match-3</t> パズルのコアロジックを担う 2D グリッド。隣接 swap → 3 個以上の連続検出 → 消去 (special 効果) → 重力落下 → 補充 → 連鎖、までを内包する。",
      when: "マッチ 3 パズルを作る時。盤面の状態とロジックだけを持ち、VFX/SFX/スコアは消去コールバックで外に出す。",
      members: [
        { sig: "void Init(u32 width, u32 height, u32 color_count)", desc: "グリッドを初期化。color_count は使う色数 (色 ID は 1..color_count)。不正値は安全な既定に。" },
        { sig: "u32 Width() / u32 Height() const", desc: "盤面サイズ。" },
        { sig: "const FGridCell& Get(u32 x, u32 y) const", desc: "セルを const 参照。範囲外は内部の空 dummy を返す (安全)。" },
        { sig: "void Set(u32 x, u32 y, u8 color, ESpecialKind = Normal)", desc: "セルを書き換え。color==0 で空セルに。範囲外は no-op。" },
        { sig: "void FillRandom(u32 seed = 0)", desc: "全セルをランダム色で埋める。初期 3 連続が出ないよう調整。seed=0 はグローバル乱数。", when: "盤面の初期化時。" },
        { sig: "bool TrySwap(u32 x1, u32 y1, u32 x2, u32 y2)", ret: "成立したか", desc: "隣接セルの swap を試す。マッチが出れば ResolveAllMatches まで実行して true、出なければ swap を戻して false。" },
        { sig: "u32 DetectMatches(FMatchInfo* out, u32 max) const", ret: "マッチ数", desc: "3+ 連続マッチを検出して書き出す。out=nullptr なら個数のみ。" },
        { sig: "u32 ResolveAllMatches()", ret: "除去総数", desc: "検出→消去 (special 効果)→重力→補充 を安定するまで反復。各 cascade で chain level +1。" },
        { sig: "u32 ApplyGravity() / u32 RefillFromTop()", desc: "空セルを上向きに吸い上げる 1 step / 上端の空をランダム色で埋める 1 step。" },
        { sig: "void SetOnClearCallback(ClearCallback cb, void* user)", desc: "消去 1 個ごとの<t>コールバック</t> (VFX/SFX/score hook)。" },
        { sig: "u32 TotalClearedThisChain() / u32 ChainLevel() const", desc: "現 chain の累計除去数 / cascade 段数。" },
        { sig: "void ResetChain() / void ClearAll()", desc: "新ターン用に chain カウンタを 0 に / 全セルを空に (サイズは保持)。" },
        { sig: "using FMatchGrid = CMatchGrid", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CMatchGrid</code> を使う。" }
      ]
    },
    {
      name: "FGridCell",
      kind: "構造体", header: "gameframework/MatchGrid.h",
      summary: "セル 1 個の状態。色 (1..color_count、0=空) ・special 種別 (ESpecialKind の u8 値) ・空フラグ。",
      when: "<code>Get</code> でセルを読む時。",
      members: [
        { sig: "u8 color", desc: "1..color_count = 色、0 = 空。" },
        { sig: "u8 special", desc: "ESpecialKind の生値 (Normal/Bomb/...)。" },
        { sig: "bool empty", desc: "空か (color==0 と等価だが明示)。" }
      ]
    },
    {
      name: "FMatchInfo",
      kind: "構造体", header: "gameframework/MatchGrid.h",
      summary: "連続マッチ 1 個の記述。開始座標・長さ (3 以上)・横/縦フラグ・色。",
      when: "<code>DetectMatches</code> の出力を読む時。",
      members: [
        { sig: "u32 start_x / start_y / length", desc: "開始セルと連続長 (length >= 3)。" },
        { sig: "bool horizontal / u8 color", desc: "横方向か (false=縦) / マッチした色。" }
      ]
    },
    {
      name: "ESpecialKind",
      kind: "列挙(enum)", header: "gameframework/MatchGrid.h",
      summary: "セルの特殊種別。Normal (普通) / Bomb (3x3 消去) / Lightning (同行+同列消去) / Rainbow (同色全消去)。",
      when: "<code>Set</code> で特殊宝石を仕込む時、消去コールバックで効果を識別する時。"
    },
    {
      name: "CMatchGrid::ClearCallback",
      kind: "関数ポインタ型", header: "gameframework/MatchGrid.h",
      summary: "セル消去 1 個ごとの<t>コールバック</t>型。<code>void(*)(void* user, u32 x, u32 y, u8 color, ESpecialKind) noexcept</code>。",
      when: "<code>SetOnClearCallback</code> に渡し、VFX/SFX/スコア加算を行う時。"
    },
    // =====================================================================
    // MlRuntime.h
    // =====================================================================
    {
      name: "IMlRuntime",
      kind: "インターフェース", header: "gameframework/MlRuntime.h",
      summary: "ML 推論ランタイム (ONNX Runtime / DirectML / NNAPI / CoreML / TFLite) を差し替えるための抽象<t>シーム(seam)</t>。全メソッドは<t>決定論</t>ゾーンの外で呼ばれる前提。",
      when: "AI/ML 推論をゲームに組み込みたいが、再現性が保証されない計算を sim/物理に混ぜたくない時。具象 backend は別モジュールで差し込む。",
      members: [
        { sig: "virtual TResult<void> Init() / void Shutdown() noexcept = 0", desc: "backend 初期化 (DLL/セッション/GPU 取得) / 破棄。" },
        { sig: "virtual TResult<FMlModelHandle> LoadModel(const char* path) noexcept = 0", desc: "モデルファイルを読み込んでハンドルを返す。nullptr は失敗。" },
        { sig: "virtual TResult<void> UnloadModel(FMlModelHandle h) noexcept = 0", desc: "ハンドルを解放。無効ハンドルは no-op 相当。" },
        { sig: "virtual TResult<void> RunInference(FMlModelHandle h, const f32* in, u32 in_n, f32* out, u32 out_n) noexcept = 0", desc: "推論を 1 回実行 (ブロッキング)。in/out はcaller確保の f32 配列、個数はモデルと一致必須。" }
      ]
    },
    {
      name: "CMlRuntimeStub",
      kind: "クラス", header: "gameframework/MlRuntime.h",
      summary: "全メソッドが NotImplemented を返す <code>IMlRuntime</code> の<t>スタブ</t>実装。「ML 経路が常に失敗する」前提でフォールバックを正しく書けているか検証するためのもの。",
      when: "backend 未統合の状態 (ACS_BUILD_ML_ONNX=OFF) や、ML SDK を持たない CI でロジック試験を回す時。",
      members: [
        { sig: "using FMlRuntimeStub = CMlRuntimeStub", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CMlRuntimeStub</code> を使う。" }
      ]
    },
    {
      name: "FMlModelHandle",
      kind: "構造体", header: "gameframework/MlRuntime.h",
      summary: "不透明な ML モデル<t>ハンドル</t>。backend が内部で持つモデル参照を u64 として公開する。<code>m_Opaque == 0</code> が無効。",
      when: "LoadModel の戻り値を保持し、RunInference / UnloadModel に渡す時。",
      members: [
        { sig: "u64 m_Opaque", desc: "backend 固有の値 (0=無効)。" },
        { sig: "bool IsValid() const", desc: "有効なハンドルか。" }
      ]
    },
    {
      name: "IUpscaler",
      kind: "インターフェース", header: "gameframework/MlRuntime.h",
      summary: "解像度アップスケーラ (AMD FSR / NVIDIA DLSS / Intel XeSS) を差し替える抽象<t>シーム(seam)</t>。低解像度で描いた最終バッファをターゲット解像度に拡大する後段ステージ。",
      when: "アップスケーリングを使いたいが、SDK ライセンスを本体から隔離し、未対応ハードではネイティブ解像度にフォールバックしたい時。",
      members: [
        { sig: "virtual TResult<void> Init(EUpscalerKind k) noexcept = 0", desc: "指定 kind で初期化。Off は no-op 成功。" },
        { sig: "virtual EUpscalerKind ActiveKind() const noexcept = 0", desc: "現在 active な kind。Init 前/Shutdown 後は Off。" },
        { sig: "virtual void Shutdown() noexcept = 0", desc: "破棄 (SDK セッション解放)。多重 Shutdown は no-op。" },
        { sig: "virtual u32 InputWidth/InputHeight/OutputWidth/OutputHeight() const noexcept = 0", desc: "入力 (低解像度) / 出力 (アップスケール後) のサイズ。" }
      ]
    },
    {
      name: "CUpscalerStub",
      kind: "クラス", header: "gameframework/MlRuntime.h",
      summary: "Off 状態だけを返す <code>IUpscaler</code> の<t>スタブ</t>。<code>Init(Off)</code> は成功、それ以外は NotImplemented、入出力サイズは常に 0。",
      when: "SDK 同梱前でも「FSR/DLSS/XeSS が常に使えない」前提でネイティブ描画フォールバックを書く時。",
      members: [
        { sig: "using FUpscalerStub = CUpscalerStub", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CUpscalerStub</code> を使う。" }
      ]
    },
    {
      name: "EUpscalerKind",
      kind: "列挙(enum)", header: "gameframework/MlRuntime.h",
      summary: "アップスケーラ種別。Off (パススルー) / FSR / DLSS / XeSS / Custom (ユーザー実装)。",
      when: "<code>IUpscaler::Init</code> でどのアップスケーラを使うか指定する時。"
    },
    {
      name: "GetMlRuntimeStub / GetUpscalerStub / GetDefaultMlRuntime / SetMlRuntimeProvider",
      kind: "関数", header: "gameframework/MlRuntime.h",
      summary: "ML / IUpscaler の窓口アクセサ群。プロセス内に 1 個だけある stub への参照を返すほか、実 backend を <code>SetMlRuntimeProvider</code> で結線して <code>GetDefaultMlRuntime</code> から backend 非依存に取得できる。",
      when: "ゲームコードから backend を直接 include せずに既定 ML ランタイムを取りたい時。provider 未登録なら自動で stub を返す。",
      members: [
        { sig: "IMlRuntime& GetMlRuntimeStub() / IUpscaler& GetUpscalerStub()", desc: "プロセス共有の stub への参照。" },
        { sig: "void SetMlRuntimeProvider(MlRuntimeProvider provider)", desc: "実 backend が「既定 runtime を返す関数」を登録 (後勝ち、nullptr で stub に戻す)。" },
        { sig: "IMlRuntime& GetDefaultMlRuntime()", desc: "provider 登録済みなら実 runtime、未登録なら stub を返す。" }
      ]
    },
    // =====================================================================
    // ModRegistry.h
    // =====================================================================
    {
      name: "CModRegistry",
      kind: "クラス", header: "gameframework/ModRegistry.h",
      summary: "ユーザー Mod (追加コンテンツパック) の登録・有効化・並び順を管理する薄いレジストリ。各 Mod は <code>.acpak</code> を伴い、load_order 順に重ねていく (後勝ちで同名アセットを上書き) 想定。",
      when: "Mod 対応ゲームで、Editor UI から Mod 一覧を出し、有効/無効や読み込み順を管理したい時。実際の mount は別レイヤ (本体は path を保持するだけ)。",
      members: [
        { sig: "void Register(const FModInfo& info)", desc: "Mod を末尾に追加 (浅いコピー、文字列の寿命は呼び出し側保証)。id==nullptr は無視。" },
        { sig: "bool Enable(const char* id) / bool Disable(const char* id)", desc: "enabled フラグを書き換え。見つかれば true。" },
        { sig: "void SetLoadOrder(const char* id, i32 order)", desc: "load_order を変更 (sort は別途)。見つからなければ no-op。" },
        { sig: "u32 Count() const", desc: "登録 Mod 数。" },
        { sig: "const FModInfo* Find(const char* id) const", desc: "id から Mod を引く。未検出は nullptr。" },
        { sig: "const FModInfo* All() const", desc: "生バッファ先頭 (長さは Count())。", when: "全 Mod を順に列挙する時。" },
        { sig: "void SortByLoadOrder()", desc: "load_order 昇順に安定ソート (同値は登録順を保つ)。" },
        { sig: "void Clear()", desc: "全 Mod を削除。" },
        { sig: "using FModRegistry = CModRegistry", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CModRegistry</code> を使う。" }
      ]
    },
    {
      name: "FModInfo",
      kind: "構造体", header: "gameframework/ModRegistry.h",
      summary: "1 Mod 分のメタデータ。id・表示名・バージョン・load_order・有効フラグ・<code>.acpak</code> パス。",
      when: "<code>Register</code> に渡す。文字列はすべて非所有 (manifest loader が組み立て寿命を保証)。",
      members: [
        { sig: "const char* id / name / pack_path", desc: "一意キー / 表示名 / .acpak パス (すべて非所有、nullptr 許容)。" },
        { sig: "u32 version", desc: "(major&lt;&lt;24)|(minor&lt;&lt;16)|patch エンコーディング想定 (Registry は不透明に扱う)。" },
        { sig: "i32 load_order", desc: "読み込み順 (昇順。小さい方が先 load、大きい方が上書き)。負値は core 系。" },
        { sig: "bool enabled", desc: "有効か。" }
      ]
    },
    // =====================================================================
    // MusicDirector.h
    // =====================================================================
    {
      name: "CMusicDirector",
      kind: "クラス", header: "gameframework/MusicDirector.h",
      summary: "ゲームプレイ状態 (戦闘/平穏/勝利...) を BGM にマッピングする上位レイヤ。state ごとに track を登録し、激しさ (intensity) で曲を選び、クロスフェードや一発もの (stinger) を扱う。実再生は CAudioDirector に委ねる。",
      when: "状況に応じて BGM を自動で切り替える「適応 BGM」を作る時。CAudioDirector が低レイヤの mixer/クロスフェードを担うのに対し、こちらは「どの track をどのゲインで鳴らすか」の state machine。",
      members: [
        { sig: "void RegisterTrack(EMusicState state, const FMusicTrack& track)", desc: "state に track を登録 (複数可、intensity range で選別)。asset_path==nullptr は無視。" },
        { sig: "void SetState(EMusicState state, f32 transition_sec = 2.0f)", desc: "現 state → 指定 state へクロスフェード開始。<=0 は即時、同 state 再要求は no-op。" },
        { sig: "EMusicState CurrentState() / TargetState() const / bool IsTransitioning() const", desc: "現/目標 state とクロスフェード中か。" },
        { sig: "f32 TransitionProgress() const", ret: "[0,1]", desc: "遷移進捗 (1 で完了)。" },
        { sig: "void SetIntensity(f32 v) / f32 Intensity() const", desc: "同 state 内の激しさ [0,1] を設定/取得 (範囲外は clamp)。" },
        { sig: "void PlayStinger(const char* asset_path, f32 volume = 1.0f)", desc: "BGM を停めずに重ねる一発もの SFX (ボス出現/達成)。pending があれば最新で上書き。" },
        { sig: "bool HasPendingStinger() const / const char* ConsumeStinger(f32& out_volume)", desc: "未消費 stinger があるか / 取り出して pending を消す (ゲインを out で返す)。" },
        { sig: "void SetAudioDirector(CAudioDirector* audio) / CAudioDirector* GetAudioDirector() const", desc: "実再生を委ねる低レイヤ director を結線 (非所有)。nullptr で state-only 動作 (無音)。" },
        { sig: "void Tick(f32 dt)", desc: "毎フレーム呼び、transition / stinger タイマを進める。" },
        { sig: "void Stop()", desc: "Silent に即時切替 + stinger 破棄 (intensity は保持)。" },
        { sig: "const FMusicTrack* CurrentTrack() / TargetTrack() const", desc: "現在/遷移先 state で intensity に合う track。該当なしは nullptr。" },
        { sig: "using FMusicDirector = CMusicDirector", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CMusicDirector</code> を使う。" }
      ]
    },
    {
      name: "EMusicState",
      kind: "列挙(enum)", header: "gameframework/MusicDirector.h",
      summary: "適応 BGM の状態。Silent / Calm (平穏) / Tension (緊張) / Combat (戦闘) / Victory (勝利) / GameOver の 6 種。",
      when: "<code>SetState</code> や <code>RegisterTrack</code> で状況を指定する時。値は save 互換のため安定 (追加は末尾)。"
    },
    {
      name: "FMusicTrack",
      kind: "構造体", header: "gameframework/MusicDirector.h",
      summary: "適応 BGM の 1 トラック登録情報。アセットパス・intensity 範囲・ループ可否。",
      when: "<code>RegisterTrack</code> に渡す。現在の intensity が range 内のとき候補になる。",
      members: [
        { sig: "const char* asset_path", desc: "曲のパス (非所有、リテラル前提)。" },
        { sig: "f32 intensity_min / intensity_max", desc: "再生候補になる激しさ範囲 [0,1]。[0,1] にすれば常時候補。" },
        { sig: "bool loop", desc: "ループ再生か (Calm/Combat) / one-shot か (Victory)。" }
      ]
    },
    // =====================================================================
    // NetSnapshot.h
    // =====================================================================
    {
      name: "CNetSnapshot",
      kind: "クラス", header: "gameframework/NetSnapshot.h",
      summary: "<b>server-authoritative</b> な<t>snapshot</t>ベースのネットコード<t>シーム(seam)</t>。server は毎 tick の世界状態を 1 つの snapshot に固めて送信し、client は受信した snapshot を貯めて少し過去の時刻で<t>補間</t>表示する。",
      when: "FPS / TPS / MMORPG のように「結果」だけを配信したい時。「入力」を配信する CLockstep と対になる、もう一つのネットコード流儀。実 socket は INetTransport で差し替える。",
      members: [
        { sig: "void Init(const FNetSnapshotConfig& cfg, ENetRole role, INetTransport* transport)", desc: "設定・役割・transport を結線。Standalone は transport=nullptr 可、他は内部で stub に差し替え。" },
        { sig: "void Shutdown()", desc: "ring buffer / pending / 統計をリセット (transport は触らない)。" },
        { sig: "ENetRole Role() / u32 BufferedSnapshotCount() / u32 LastReceivedTick() const", desc: "役割 / 貯まった snapshot 数 / 最後に受信した tick。" },
        { sig: "void AddEntitySnapshot(u32 entity_id, u32 mask, const void* data, u32 size)", desc: "[server] 1 entity の現在 state を pending に積む (data は内部コピー)。" },
        { sig: "void CommitSnapshot(u32 tick)", desc: "[server] pending を 1 payload に固め header を付けて Send。pending はクリア。" },
        { sig: "bool TryGetInterpolatedSnapshot(f32 client_time, FEntitySnapshot* out, u32 max, u32& out_n)", ret: "補間できたか", desc: "[client] interpolation_delay 前の snapshot を 2 つ取り、線形補間して書き出す。view の component_data は次の Tick まで有効。" },
        { sig: "void Tick(f32 dt)", desc: "[client/listener] transport.Receive を pump し、受信 snapshot を ring buffer に積む。" },
        { sig: "u32 PacketsSent/PacketsReceived/BytesSent/BytesReceived() const", desc: "送受信統計。" },
        { sig: "static u32 EncodedSnapshotSize(u32 payload_size)", desc: "1 frame の総バイト数 = magic+version+header(24)+payload+crc32(4)。" },
        { sig: "static TResult<void> EncodeSnapshot(...) / DecodeSnapshot(...)", desc: "transport 非依存の wire codec。header+payload を byte 列に直列化 / 検証復元 (magic 'ACSN' + CRC32、round-trip 可)。", when: "テスト / loopback / record-replay で再利用する時。" },
        { sig: "using FNetSnapshot = CNetSnapshot", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CNetSnapshot</code> を使う。" }
      ]
    },
    {
      name: "INetTransport",
      kind: "インターフェース", header: "gameframework/NetSnapshot.h",
      summary: "ネットワークの実体を抽象化する<t>シーム(seam)</t>。Connect / Disconnect / Send / Receive 等の最小 API だけを切り、TCP/UDP/Steam Datagram/loopback fake を派生で差し込む。",
      when: "CNetSnapshot に socket を差し替えたい時。Send/Receive は非ブロッキングでメッセージ境界を保持する契約。",
      members: [
        { sig: "virtual TResult<void> Connect(const char* address, u16 port) noexcept = 0", desc: "接続。address は静的/長寿命文字列。" },
        { sig: "virtual void Disconnect() noexcept = 0 / virtual bool IsConnected() const noexcept = 0", desc: "切断 (べき等) / 接続中か。" },
        { sig: "virtual TResult<void> Send(const void* data, u32 size) noexcept = 0", desc: "1 呼出で 1 message を境界保持して送る。" },
        { sig: "virtual TResult<u32> Receive(void* out, u32 capacity) noexcept = 0", desc: "非ブロッキング受信 (なしなら 0 を返す成功)。1 呼出で最大 1 message。" },
        { sig: "virtual u32 PendingBytesIn() / PendingBytesOut() const noexcept = 0", desc: "受信待ち / 送信未 flush のバイト数目安。" }
      ]
    },
    {
      name: "CUdpTransport",
      kind: "クラス", header: "gameframework/NetSnapshot.h",
      summary: "<code>INetTransport</code> の<b>実 Winsock2 UDP 実装</b>。1 Send = 1 sendto = 1 datagram、1 Receive = 1 recvfrom = 1 datagram でメッセージ境界保持の契約を満たす。",
      when: "実際に UDP で snapshot を送受信したい時。loopback round-trip 検証済みの本実装 (stub ではない)。",
      members: [
        { sig: "void SetLocalPort(u16 port) / u16 LocalPort() const", desc: "bind する local port を指定 (0=OS が ephemeral)。Connect 前に呼ぶ。" },
        { sig: "TResult<void> Connect(const char* address, u16 port) noexcept override", desc: "WSAStartup → UDP socket 生成 → 非ブロッキング化 → bind → remote endpoint 保持。" },
        { sig: "TResult<void> Send / TResult<u32> Receive(...) noexcept override", desc: "sendto / recvfrom。WSAEWOULDBLOCK は buffer 満杯 / 受信なしとして扱う。" },
        { sig: "using FUdpTransport = CUdpTransport", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CUdpTransport</code> を使う。" }
      ]
    },
    {
      name: "CNetTransportStub",
      kind: "クラス", header: "gameframework/NetSnapshot.h",
      summary: "<code>INetTransport</code> の null-object <t>スタブ</t>。Connect/Send/Receive は必ず Err を返す。利用側/テスト/リンク互換用。",
      when: "本番ビルドに混入したケースを QA で必ず検出させたい時。<code>GetTransportStub()</code> でプロセス共有インスタンスを取得できる。",
      members: [
        { sig: "using FNetTransportStub = CNetTransportStub", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CNetTransportStub</code> を使う。" }
      ]
    },
    {
      name: "ENetRole",
      kind: "列挙(enum)", header: "gameframework/NetSnapshot.h",
      summary: "CNetSnapshot の役割。Client (受信のみ) / Server (送信のみ) / ServerListener (listen-server で両方) / Standalone (通信なし、全 API no-op)。",
      when: "<code>Init</code> で自機の役割を指定する時。"
    },
    {
      name: "FSnapshotHeader",
      kind: "構造体", header: "gameframework/NetSnapshot.h",
      summary: "wire format 上の 1 snapshot ヘッダ (固定 24 byte, LE)。tick・sequence・server timestamp・payload サイズ・CRC32。",
      when: "Encode/DecodeSnapshot で扱う 1 frame の先頭メタ情報。",
      members: [
        { sig: "u32 tick / u32 sequence", desc: "server tick / モノトニック増加する送信通番 (loss/重複検知)。" },
        { sig: "u64 server_timestamp_us", desc: "server 計測時の Unix microseconds。" },
        { sig: "u32 payload_size / u32 crc32", desc: "後続 payload のバイト数 / frame footer の CRC32。" }
      ]
    },
    {
      name: "FEntitySnapshot",
      kind: "構造体", header: "gameframework/NetSnapshot.h",
      summary: "1 entity 分の component 状態の<b>非所有 view</b>。entity_id・component bitmask・データへのポインタ + サイズ。",
      when: "server で AddEntitySnapshot に渡す/client で TryGetInterpolatedSnapshot から受け取る時。寿命は server: Commit まで、client: 次の Tick まで。",
      members: [
        { sig: "u32 entity_id / u32 component_mask", desc: "entity ID (0=invalid) / どの component が含まれるかの bitmask。" },
        { sig: "const void* component_data / u32 component_data_size", desc: "非所有 view とそのバイト数。" }
      ]
    },
    {
      name: "FNetSnapshotConfig",
      kind: "構造体", header: "gameframework/NetSnapshot.h",
      summary: "CNetSnapshot のランタイム設定。commit 目標 Hz・ring buffer 件数・補間遅延秒・payload 上限。",
      when: "<code>Init</code> に渡す。",
      members: [
        { sig: "u32 snapshot_rate_hz", desc: "server の目標 commit Hz (自動 throttle はしない)。" },
        { sig: "u32 buffer_capacity_snapshots", desc: "client 側 ring buffer 件数 (>=2 必須)。" },
        { sig: "f32 interpolation_delay_sec", desc: "補間の遅延 (= jitter buffer の深さ)。" },
        { sig: "u32 max_payload_bytes", desc: "1 snapshot の payload 上限。" }
      ]
    },
    {
      name: "FNetSnapshotError",
      kind: "構造体", header: "gameframework/NetSnapshot.h",
      summary: "CNetSnapshot / transport 共通のエラー subcode 集。NotConnected / BadArgument / BufferFull / PayloadTooBig / BadMagic / BadCrc や、UDP 固有の WSA エラー等を固定値で持つ。",
      when: "<code>TResult</code> の失敗時に subcode で switch 分岐したい時。"
    },
    {
      name: "GetTransportStub()",
      kind: "関数", header: "gameframework/NetSnapshot.h",
      summary: "プロセス共有の stub <code>INetTransport</code> を返す。常に NotImplemented を返す null-object。",
      when: "transport を結線せずリンク互換を保ちたい時 (Init が nullptr 時に内部で差し替えるのもこれ)。"
    },
    // =====================================================================
    // ANode.h
    // =====================================================================
    {
      name: "ANode",
      kind: "クラス", header: "gameframework/ANode.h",
      summary: "2D/3D 共通シーンの唯一のノードクラス。<code>AObject</code> を基底とし、親が <code>TObjectPtr&lt;ANode&gt;</code> の強参照で子を所有する。transform は <code>FTransform3D</code> 一本で、2D は Position2D/Rotation2D/Scale2D/World2D helper を使う。描画順は DrawLayer/DrawPriority/YSort をノード自身が持つ。",
      when: "ゲームのシーングラフを組む基本単位。<code>AddChild(NewObject&lt;AMyNode&gt;(...))</code> が標準パターンで、ゲームプレイ側の長期参照には stale-safe な <code>TWeakObjectPtr&lt;ANode&gt;</code> を使う。",
      members: [
        { sig: "virtual void OnSpawn/OnUpdate(f32)/OnFixedUpdate(f32)/OnDraw(FRenderContext&amp;)/OnDespawn() noexcept", desc: "ライフサイクル<t>フック</t>。必要なものだけ override する (全 noexcept 必須)。OnFixedUpdate は固定刻みの物理/決定論ロジック用。" },
        { sig: "FTransform3D&amp; Local() / FTransform3D World() const", desc: "統一されたローカル 3D transform (真値) / 親をたどって合成した world transform。" },
        { sig: "Position2D/SetPosition2D, Rotation2D/SetRotation2D, Scale2D/SetScale2D, Local2D/SetLocal2D, World2D", desc: "統一 3D transform の x/y と Z 回転を安全に扱う 2D helper。旧 2D 専用ノードは不要。" },
        { sig: "void SetEnabled(bool) / SetVisible(bool) (+ getter)", desc: "subtree 単位で update/描画を skip する有効/可視フラグ。" },
        { sig: "void SetDrawLayer(i32) / SetDrawPriority(i32) / SetYSortEnabled(bool) / SetYSortBias(f32)", desc: "描画の layer/priority と同一 group 内の Y-sort をノード単位で制御する。", when: "背景/ワールド/前景/HUD を分けたり、足元で Y 遮蔽したい時。" },
        { sig: "ANode* Parent() / u32 ChildCount() / ANode* Child(u32) const", desc: "親 / 子の数 / i 番目の子。" },
        { sig: "ANode&amp; AddChild(TObjectPtr&lt;ANode&gt; child)", ret: "追加した子への参照", desc: "AObject 強参照を親へ移し、未 spawn なら OnSpawn を即時呼ぶ。<code>NewObject</code> と組み合わせる。" },
        { sig: "EAddChildResult TryAddChild(TObjectPtr&lt;ANode&gt;&amp; child)", desc: "null/self/already-parented/cycle/depth/OOM を診断し、失敗時は入力強参照の所有権を変更しない。" },
        { sig: "void Destroy() / bool IsPendingDestroy() const", desc: "破棄予定にマーク (実破棄は次の ResolveStructuralChanges で OnDespawn → 除去)。" },
        { sig: "void Reparent(ANode& new_parent) / bool IsPendingReparent() const", desc: "別の親へ移動要求 (フレーム境界で適用、cycle は検出して無視)。OnSpawn/OnDespawn は呼ばれない。" },
        { sig: "FNodeId Id() const", desc: "ノードの<t>世代付きハンドル</t> (FNodeId)。stale 参照検出用。既定は invalid。" },
        { sig: "T& AddComponent<T>(Args&&...) / T& GetOrAddComponent<T>(...)", ret: "コンポーネント参照", desc: "<t>コンポーネント</t>を構築・attach (依存を先に確保し OnAttach 即時呼出) / 無ければ追加して返す。" },
        { sig: "T* GetComponent<T>() / bool HasComponent<T>() / bool RemoveComponent<T>()", desc: "最初の T 型コンポーネントを取得 / 有無判定 / 1 つ除去 (OnDetach → 破棄)。" },
        { sig: "void UpdateTree(f32) / FixedUpdateTree(f32) / DrawTree(FRenderContext&amp;) / DrawTreeSorted(FRenderContext&amp;)", desc: "root から呼ぶ subtree 走査。sorted 版は可視ノードを安定収集して layer/priority/Y で描く。全 key が既定値なら従来ツリー順の fast path。", when: "毎フレーム root に対して呼ぶ。" },
        { sig: "void ResolveStructuralChanges()", desc: "フレーム境界で 1 回。pending な破棄/Reparent をまとめて適用する (子から先に reap)。" },
        { sig: "u32 ComponentCount() const", desc: "このノードに attach 済みの<t>コンポーネント</t>数。" }
      ]
    },
    {
      name: "ALegacyScene3DAdapter",
      kind: "クラス", header: "gameframework/LegacyScene3DAdapter.h",
      summary: "旧scene 3D入口を現行scene lifecycleと固定tick自由camera入力へ接続する互換object。",
      when: "旧scene実装を段階的に移行する時。",
      members: [
        { sig: "ESvc WantedServices() const", desc: "自由camera用のscene入力serviceを要求する。" },
        { sig: "void SetFreeCameraEnabled(bool enabled)", desc: "固定tickでWASD/QE/矢印/PageUp/PageDownを評価する自由cameraを切り替える。" },
        { sig: "using FLegacyScene3DAdapter = ALegacyScene3DAdapter", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ALegacyScene3DAdapter</code> を使う。" }
      ]
    },
    {
      name: "CMethodAutoRegister",
      kind: "構造体", header: "gameframework/ReflectMethod.h",
      summary: "静的初期化時にmethodをregistryへ登録する補助。",
      when: "method登録macroの生成コードから使う時。",
      members: [
        { sig: "using FMethodAutoRegister = CMethodAutoRegister", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CMethodAutoRegister</code> を使う。" }
      ]
    },
    {
      name: "CMethodRegistry",
      kind: "クラス", header: "gameframework/ReflectMethod.h",
      summary: "reflection methodの登録と検索を提供する。",
      when: "登録済みmethodを名前から呼び出す時。",
      members: [
        { sig: "using FMethodRegistry = CMethodRegistry", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CMethodRegistry</code> を使う。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "ホットリロード": "ゲームを再起動せずにアセットやコードを差し替えて即反映させる開発手法。",
  "i18n": "internationalization (国際化) の略。多言語対応のこと。文字列を言語ごとの辞書から引けるようにする。",
  "PII": "Personally Identifiable Information (個人を特定できる情報)。メール・電話・クレカ番号など。安全フィルタで <code>[REDACTED]</code> 等に伏せる対象。",
  "LLM": "大規模言語モデル (Large Language Model)。NPC のセリフ生成などに使う。出力が非決定論なので<t>決定論</t>ゾーンの外で扱う。",
  "シーム(seam)": "実装本体を切り離して差し替え可能にする「継ぎ目」。<t>インターフェース</t>だけを置き、具象は別モジュールで注入する。テストや SDK 隔離に使う。",
  "決定論": "同じ入力なら必ず同じ結果になる性質。replay / netcode / record-replay の同期で必須。ML 推論や浮動小数のドライバ差はこれを壊すので隔離する。",
  "lockstep": "全クライアントが同じ入力を同じ順序で受け取り、各自で同一の<t>決定論</t>シミュレーションを回すネットコード方式。格闘・RTS 系で標準。",
  "snapshot": "ある時刻の世界全体の状態を 1 つに固めたもの。server がこれを配信し、client は受信した複数 snapshot を<t>補間</t>して表示する (FPS/MMO 系で標準)。",
  "補間": "2 つの値 (ここでは前後の snapshot) の間を滑らかにつないで中間の状態を作ること。少し過去の時刻を狙うことで通信ジッタを吸収する。",
  "match-3": "同じ色を 3 個以上並べて消すパズルの定番ジャンル (Bejeweled / Candy Crush 系)。",
  "コンパイル時ハッシュ": "文字列などをコンパイル時に数値へ変換する仕組み。実行時の文字列比較が無くなり高速・軽量になる (ここでは FNV-1a を <code>constexpr</code> で計算)。",
  "世代(generation)": "<t>ハンドル</t>に付ける版番号。スロットを再利用しても番号が変わるので、古いハンドルを「もう無効」と確実に弾ける。",
  "世代付きハンドル": "index + <t>世代(generation)</t> を 1 つに詰めた識別子。破棄済み対象への参照 (stale 参照) を安全に検出できる。",
  "プロバイダ": "ある機能 (ここではインスペクタ表示) に対してデータや実装を「供給する」側のオブジェクト。<t>インターフェース</t>を実装して登録する。",
  "POD": "Plain Old Data。コンストラクタ等の特別な処理を持たない素朴なデータ構造。memcpy で安全にコピー/保存できる。",
  "transform": "位置・回転・拡縮をまとめた座標変換。親子で合成すると階層的な配置になる。",
  "コンポーネント": "ノードに後付けで attach する機能部品 (Sprite/Collider など)。継承ではなく合成でノードに能力を足す設計。",
  "フック": "特定のタイミングで自動的に呼ばれる差し込み点となる関数 (OnSpawn / OnUpdate など)。override して処理を書く。",
  "スタブ": "本実装の代わりに置く「中身が空 / 常に失敗を返す」仮実装。未統合の backend のフォールバックや、上位層のテストに使う。"
});
