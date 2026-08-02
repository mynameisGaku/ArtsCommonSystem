/* ACS リファレンス — openxr モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "openxr",
  order: 54,
  title: "openxr — VR/XR 連携",
  blurb: "VR/AR ヘッドセット(<t>HMD</t>)を扱う窓口です。<t>OpenXR</t> という業界標準を通して、頭やコントローラの位置・向きを読み取り、<t>パススルー</t>(現実映像の表示)を切り替えます。実機が無くても安全に動く<t>フォールバック</t>付き。",
  types: [
    {
      name: "CKhronosOpenXrBridge",
      kind: "クラス", header: "openxr/KhronosOpenXrBridge.h",
      summary: "<t>OpenXR</t> 公式ローダ(Khronos loader)を使った実<t>backend</t>。共通インターフェース <t>IOpenXrBridge</t> を実装し、<t>HMD</t> やコントローラの<t>ポーズ</t>を提供する。コピー/ムーブ不可の<t>singleton</t>運用前提。",
      when: "実機の VR ランタイムに接続して頭・手の位置を取りたい時。通常は直接 new せず、<code>GetDefaultKhronosOpenXrBridge()</code> か <code>GetDefaultOpenXrBridge()</code> 経由で使う。",
      sample: "auto&amp; xr = acs::openxr::GetDefaultKhronosOpenXrBridge();\nif (xr.IsTracking()) {\n    auto head = xr.HeadPose();      // 頭の位置・向き\n    auto left = xr.LeftController(); // 左手の入力\n}",
      members: [
        { sig: "TResult&lt;void&gt; Init(EXrPlatform platform = Unknown)", ret: "成功 or 失敗", desc: "XR ランタイムへ接続し instance を作る。<code>platform</code> 省略時は backend が自動検出。ランタイムが無ければ失敗(<t>Result</t> でエラー)を返す。", when: "起動時に一度。失敗したら 2D 描画へ<t>フォールバック</t>する。" },
        { sig: "void Shutdown()", desc: "接続を破棄し loader を解放する。Init 前に呼んでも安全。" },
        { sig: "bool IsInitialized() const", ret: "初期化済みか", desc: "Init() が成功し Shutdown() 前なら true。" },
        { sig: "bool IsTracking() const", ret: "追跡中か", desc: "実セッションが確立してポーズが実際に更新されているか。本 backend は session loop 未実装のため<b>現状は常に false</b>。<code>IsInitialized()</code> とは別概念。", when: "ポーズを使う前に必ず確認する。false ならゼロポーズなので使わない。" },
        { sig: "EXrPlatform ActivePlatform() const", ret: "プラットフォーム種別", desc: "今選ばれている XR <t>プラットフォーム</t>(Meta Quest 等)を返す。未接続は Unknown。" },
        { sig: "FXrPose HeadPose() const", ret: "頭のポーズ", desc: "直近 Tick 時点の <t>HMD</t> の位置と向き。未追跡時は原点(ゼロポーズ)。" },
        { sig: "FXrControllerState LeftController() const", ret: "左手の状態", desc: "左コントローラの 1 フレーム分の入力<t>スナップショット</t>。" },
        { sig: "FXrControllerState RightController() const", ret: "右手の状態", desc: "右コントローラの入力スナップショット。" },
        { sig: "void Tick(f32 Dt)", desc: "XR ランタイムのイベントを取り込み、ポーズ/入力を更新する。ゲームループから毎フレーム呼ぶ。<code>Dt</code> は実時間秒。" },
        { sig: "bool IsPassthroughSupported() const", ret: "対応可否", desc: "<t>パススルー</t>(MR モード)に対応しているか。" },
        { sig: "void SetPassthrough(bool bOn)", desc: "パススルーの on/off を要求する。非対応/未初期化なら何もしない。" },
        { sig: "using FKhronosOpenXrBridge = CKhronosOpenXrBridge", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CKhronosOpenXrBridge</code> を使う。" }
      ]
    },
    {
      name: "GetDefaultKhronosOpenXrBridge()",
      kind: "関数", header: "openxr/KhronosOpenXrBridge.h",
      summary: "プロセス共有の実 Khronos bridge(<t>singleton</t>)を返す。初回アクセス時に <code>Init()</code> を 1 回だけ自動で走らせる(失敗しても参照は返る)。",
      when: "実 OpenXR backend を直接掴みたい時。多くの場合は backend 非依存の <code>acs::game::GetDefaultOpenXrBridge()</code> を使えば十分。",
      sample: "auto&amp; xr = acs::openxr::GetDefaultKhronosOpenXrBridge();\nxr.Tick(dt);\nif (xr.IsInitialized()) { /* 接続できた */ }"
    },
    {
      name: "InstallOpenXrAsDefault()",
      kind: "関数", header: "openxr/KhronosOpenXrBridge.h",
      summary: "gameframework の既定 provider に、この実 Khronos bridge を登録する<t>結線</t>関数。以後 <code>acs::game::GetDefaultOpenXrBridge()</code> が stub ではなく実 bridge を返すようになる。",
      when: "アプリ起動時に一度だけ呼ぶ。これで上位ゲームコードは backend に直接依存せず、実 XR を使える(<t>循環依存</t>回避のための仕組み)。",
      sample: "#if WITH_ACS_OPENXR\n    acs::openxr::InstallOpenXrAsDefault(); // 既定を実 bridge に差し替え\n#endif\n// 以降どこでも:\nauto&amp; xr = acs::game::GetDefaultOpenXrBridge();"
    },
    {
      name: "kSubOpenXrRuntimeUnavailable / kSubOpenXrInitFailed",
      kind: "定数(エラー subcode)", header: "openxr/KhronosOpenXrBridge.h",
      summary: "Init 失敗時に <t>Result</t> へ載るエラーの細分番号。<code>301</code>=XR ランタイムが見つからない、<code>302</code>=初期化に失敗。",
      when: "Init() の失敗理由を分岐したい時に <t>Result</t> のエラーと突き合わせる。",
      sample: "auto r = xr.Init();\nif (!r) {\n    // r.Error().subcode が kSubOpenXrRuntimeUnavailable なら\n    //   → ランタイム未導入。2D 描画へフォールバック。\n}"
    },
    {
      name: "IOpenXrBridge",
      kind: "インターフェース", header: "gameframework/OpenXrBridge.h",
      summary: "XR ランタイム/<t>HMD</t> を抽象化した共通<t>インターフェース</t>(<t>seam</t>)。ゲームロジックはこの型越しに頭・手のポーズや<t>パススルー</t>を叩き、具象 backend(<t>CKhronosOpenXrBridge</t> / stub 等)は差し替えで選ぶ。シングルスレッド専用・コピー/ムーブ不可。",
      when: "XR を使う側のコードはこの参照型で受け取る。これにより実機が無い CI でも <t>mock</t> に差し替えてテストできる。",
      sample: "void UpdateVr(acs::game::IOpenXrBridge&amp; xr, float dt) {\n    xr.Tick(dt);\n    if (!xr.IsTracking()) return;     // 未追跡なら 2D へ\n    auto head = xr.HeadPose();\n    if (xr.LeftController().trigger &gt; 0.5f) Shoot();\n}",
      members: [
        { sig: "virtual TResult&lt;void&gt; Init(EXrPlatform = Unknown)", ret: "成功 or 失敗", desc: "backend を初期化。<code>Unknown</code> なら利用可能な SDK を自動検出。" },
        { sig: "virtual void Shutdown()", desc: "backend を破棄。Init 前に呼んでも安全。" },
        { sig: "virtual bool IsInitialized() const", ret: "初期化済みか", desc: "Init 成功後 Shutdown 前なら true。" },
        { sig: "virtual bool IsTracking() const", ret: "追跡中か", desc: "ポーズが実際にライブ追跡されているか。false ならポーズは原点なので使わない。既定実装は false(安全側)。", when: "ポーズを使う前に毎回確認。" },
        { sig: "virtual EXrPlatform ActivePlatform() const", ret: "プラットフォーム", desc: "今 active な XR <t>プラットフォーム</t>種別。stub は Unknown。" },
        { sig: "virtual FXrPose HeadPose() const", ret: "頭のポーズ", desc: "直近 Tick の HMD ポーズ。stub/未初期化は原点。" },
        { sig: "virtual FXrControllerState LeftController() const", ret: "左手状態", desc: "左コントローラの入力スナップショット。未接続はゼロ state。" },
        { sig: "virtual FXrControllerState RightController() const", ret: "右手状態", desc: "右コントローラの入力スナップショット。" },
        { sig: "virtual void Tick(f32 dt)", desc: "ポーズ/入力の取り込みを進める。毎フレーム呼ぶ。" },
        { sig: "virtual bool IsPassthroughSupported() const", ret: "対応可否", desc: "<t>パススルー</t>に対応するか。stub は常に false。" },
        { sig: "virtual void SetPassthrough(bool on)", desc: "パススルー on/off を要求。非対応/未初期化なら no-op。" }
      ]
    },
    {
      name: "FXrPose",
      kind: "構造体", header: "gameframework/OpenXrBridge.h",
      summary: "<t>HMD</t> やコントローラの位置と向き。向きは <t>quaternion</t> ではなく <t>オイラー角</t>(pitch, yaw, roll)を<b>ラジアン</b>で持つ簡素な表現。",
      when: "頭や手の姿勢を読むとき。<code>tracked</code> が false の時は原点なので姿勢として使わない。",
      sample: "acs::game::FXrPose p = xr.HeadPose();\nif (p.tracked) {\n    acs::FVec3 pos = p.position;            // world 位置 (m)\n    float yaw = p.orientation_euler.y;      // 向き(ラジアン)\n}",
      members: [
        { sig: "FVec3 position", desc: "world-space の位置(メートル)。既定は原点。" },
        { sig: "FVec3 orientation_euler", desc: "向きを (pitch, yaw, roll) のラジアンで。精密な計算が要るなら自分で <t>quaternion</t>/行列へ変換する。" },
        { sig: "bool tracked", ret: "有効か", desc: "このポーズがライブ追跡由来なら true。false の時 position/orientation は原点で、有効な姿勢として使ってはならない。" }
      ]
    },
    {
      name: "FXrControllerState",
      kind: "構造体", header: "gameframework/OpenXrBridge.h",
      summary: "左右コントローラ 1 フレーム分の入力<t>スナップショット</t>。各プラットフォーム差を吸収した最小公倍数(ポーズ+トリガ+グリップ+2 ボタン+スティック)。",
      when: "VR コントローラの入力(撃つ・掴む・移動)を読むとき。",
      sample: "auto c = xr.RightController();\nif (c.trigger &gt; 0.5f) Fire();             // 引き金\nif (c.button_a) Jump();\nMove(c.thumbstick.x, c.thumbstick.y);      // スティックで移動",
      members: [
        { sig: "FXrPose pose", desc: "6DoF のコントローラ位置+向き。" },
        { sig: "f32 trigger", desc: "人差し指トリガのアナログ値 [0, 1]。" },
        { sig: "f32 grip", desc: "グリップ(中指)ボタンのアナログ値 [0, 1]。" },
        { sig: "bool button_a / button_b", desc: "親指側の主要 2 ボタン(Quest の A/B または X/Y 等にマップ)。" },
        { sig: "FVec2 thumbstick", desc: "親指スティック/トラックパッド入力 [-1, 1] × 2 軸。" }
      ]
    },
    {
      name: "EXrPlatform",
      kind: "列挙(enum)", header: "gameframework/OpenXrBridge.h",
      summary: "想定する XR <t>プラットフォーム</t>(HMD 種別)の列挙。<code>Init()</code> に渡して指定するか、<code>ActivePlatform()</code> で今どれが選ばれたかを知る。",
      when: "特定機種向けの分岐や、自動検出させたい(<code>Unknown</code>)時。",
      sample: "using P = acs::game::EXrPlatform;\nxr.Init(P::MetaQuest);            // Quest 指定\n// or\nxr.Init(P::Unknown);              // 自動検出に任せる",
      members: [
        { sig: "Unknown = 0", desc: "未指定/検出失敗。stub の既定。Init に渡すと自動検出。" },
        { sig: "MetaQuest = 1", desc: "Meta Quest 2 / 3 / Pro。" },
        { sig: "ValveIndex = 2", desc: "Valve Index(OpenVR/SteamVR)。" },
        { sig: "HtcVive = 3", desc: "HTC Vive 系。" },
        { sig: "PicoNeo = 4", desc: "ByteDance Pico Neo 3 / 4。" },
        { sig: "AppleVisionPro = 5", desc: "Apple Vision Pro(VisionOS/ARKit)。" },
        { sig: "PsVr2 = 6", desc: "Sony PlayStation VR2。" },
        { sig: "WindowsMR = 7", desc: "Microsoft Windows Mixed Reality。" },
        { sig: "SteamLink = 8", desc: "Steam Link(ネットワークストリーミング)。" }
      ]
    },
    {
      name: "GetDefaultOpenXrBridge()",
      kind: "関数", header: "gameframework/OpenXrBridge.h",
      summary: "backend に依存せず既定の <t>IOpenXrBridge</t> を返す。実 backend が <code>InstallOpenXrAsDefault()</code> で登録済みなら実 bridge、未登録なら <code>GetXrStub()</code>(stub)を返す。",
      when: "ゲームコードから XR を使う標準の入口。これだけ使えば実機ありでもなしでも同じコードで動く。",
      sample: "auto&amp; xr = acs::game::GetDefaultOpenXrBridge();\nxr.Tick(dt);\nif (xr.IsTracking()) UseVrCamera(xr.HeadPose());\nelse UseFlatCamera();      // backend 未登録でも安全"
    },
    {
      name: "SetOpenXrBridgeProvider()",
      kind: "関数", header: "gameframework/OpenXrBridge.h",
      summary: "既定 bridge を返す関数(<t>provider</t>)を登録する。実 backend モジュールの Install* から呼ぶ。<code>nullptr</code> 登録で stub に戻す。後勝ち。",
      when: "通常は直接呼ばない。<code>InstallOpenXrAsDefault()</code> が内部でこれを使う。独自 backend を差し込みたい時のみ。",
      sample: "using Prov = acs::game::OpenXrBridgeProvider;\nProv p = []() noexcept -&gt; acs::game::IOpenXrBridge&amp; { return MyBridge(); };\nacs::game::SetOpenXrBridgeProvider(p);",
      members: [
        { sig: "void SetOpenXrBridgeProvider(OpenXrBridgeProvider provider)", desc: "provider を登録。nullptr で stub に戻す。" }
      ]
    },
    {
      name: "OpenXrBridgeProvider",
      kind: "型エイリアス", header: "gameframework/OpenXrBridge.h",
      summary: "「既定 bridge を返す関数」の型。<code>IOpenXrBridge&amp; (*)() noexcept</code> という関数<t>ポインタ</t>。",
      when: "<code>SetOpenXrBridgeProvider()</code> に渡す関数の型として使う。",
      sample: "acs::game::OpenXrBridgeProvider p =\n    []() noexcept -&gt; acs::game::IOpenXrBridge&amp; { return MyBridge(); };"
    },
    {
      name: "COpenXrBridgeStub",
      kind: "クラス", header: "gameframework/OpenXrBridge.h",
      summary: "「常に未初期化/原点ポーズ/パススルー非対応」を返す stub 実装。実 backend をリンクしていない状態でも、上位が<t>フォールバック</t>(2D 描画)を書けるようにする placeholder。",
      when: "HMD が無い開発機/CI での既定。直接 new せず <code>GetXrStub()</code> で得る。",
      sample: "auto&amp; xr = acs::game::GetXrStub();\nauto r = xr.Init();   // 常に NotImplemented で失敗\n// → IsTracking() も false なので 2D 経路へ",
      members: [
        { sig: "TResult&lt;void&gt; Init(EXrPlatform = Unknown)", desc: "常に NotImplemented を返す(副作用なし)。" },
        { sig: "FXrPose HeadPose() const", desc: "常にゼロポーズ。" },
        { sig: "bool IsPassthroughSupported() const", desc: "常に false。<code>SetPassthrough()</code> は no-op。" },
        { sig: "using FOpenXrBridgeStub = COpenXrBridgeStub", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>COpenXrBridgeStub</code> を使う。" }
      ]
    },
    {
      name: "GetXrStub()",
      kind: "関数", header: "gameframework/OpenXrBridge.h",
      summary: "プロセス内に 1 個だけ存在する静的 <t>COpenXrBridgeStub</t> への参照を返す。provider 未登録時に <code>GetDefaultOpenXrBridge()</code> が返すのもこれ。",
      when: "明示的に stub を使いたい/テストで原点ポーズを期待する時。",
      sample: "acs::game::IOpenXrBridge&amp; xr = acs::game::GetXrStub();"
    },
    {
      name: "xr_err::kSub_NotImplemented",
      kind: "定数(エラー subcode)", header: "gameframework/OpenXrBridge.h",
      summary: "「未実装」を表すエラー細分番号(<code>99</code>)。stub の <code>Init()</code> や、backend 未統合時に <t>Result</t> へ載る。他モジュール(ML 等)と番号を揃え、横断で「未実装」を一律に扱える。",
      when: "Init 失敗が「ランタイム無し」ではなく「そもそも未実装/未リンク」なのかを判別したい時。",
      sample: "auto r = acs::game::GetDefaultOpenXrBridge().Init();\nif (r.IsErr() &amp;&amp; r.Error().subcode == acs::game::xr_err::kSub_NotImplemented) {\n    // backend 未リンク。常に 2D で動かす。\n}"
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "HMD": "Head-Mounted Display。頭に被る VR/AR ディスプレイ(ヘッドセット)。",
  "OpenXR": "Khronos 策定の XR(VR/AR)機器を扱う業界標準 API。機種差をこの一枚で吸収する。",
  "パススルー": "ヘッドセットの外向きカメラ映像を画面に映し、現実の景色を見られるようにする MR モードの機能。",
  "ポーズ": "位置と向きをまとめた姿勢情報。XR では頭や手の <t>ポーズ</t> を追跡する。",
  "seam": "差し替え可能な境界面。実装(backend)を後から入れ替えられるよう、<t>インターフェース</t>だけを切り出した接合点。",
  "backend": "インターフェースの裏で実際に仕事をする具象実装。ここでは <t>OpenXR</t> ローダ等の XR 実装本体。",
  "provider": "既定の実装オブジェクトを返す登録関数。これを差し替えることで実 backend と stub を切り替える。",
  "フォールバック": "本命が使えない時の代替経路。XR では HMD 未接続時に通常の 2D 画面描画へ切り替えること。",
  "プラットフォーム": "対象とする機種/環境の種別。ここでは Meta Quest や Valve Index など XR 機器の系統。",
  "オイラー角": "向きを pitch/yaw/roll の 3 つの回転角で表す方式。直感的だが gimbal lock の弱点がある。",
  "quaternion": "回転を 4 要素で表す数学表現。<t>オイラー角</t>の弱点(gimbal lock)が無く、補間にも強い。",
  "スナップショット": "ある瞬間の状態をまとめて切り取ったコピー。ここでは 1 フレーム分の入力値の束。",
  "mock": "テスト用の偽実装。本物の代わりに差し込み、実機なしで上位ロジックを検証する。",
  "singleton": "プロセス内に 1 個だけ存在するインスタンス。XR ランタイムのように 1 接続前提のものに使う。"
});
