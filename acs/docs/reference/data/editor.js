/* ACS リファレンス — editor / 制作支援モジュール（手書き: 各エージェントが acs/src/gameframework/tools/ の実ヘッダを読んで作成）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "editor",
  order: 41,
  title: "editor — 制作支援 / エディタ (acs::game::tools)",
  blurb: "ゲーム制作を支援する <b>ImGui ベースのエディタ部品群</b>(<code>acs::game::tools</code>)。パネル基底 <code>AEditorPanel</code> とワークスペース <code>CEditorWorkspace</code> を土台に、シーン階層 / インスペクタ / アセットブラウザ / ギズモ、そして Particle・Level・SpriteAtlas・Font・BehaviorTree・Cinematics・ModelViewer といった専用エディタが載る。サンプル 29〜37 がこの層を使う。",
  types: [
{
  name: "AEditorPanel",
  kind: "クラス(抽象基底)", header: "gameframework/tools/editor_core/EditorPanel.h",
  summary: "<b>全エディタパネルの共通基底</b>。ModelViewer / LevelEditor / Inspector などあらゆる panel が継承し、<b>ライフサイクル + 選択通知 + レイアウト永続化</b>を統一する。<code>Title()</code> と <code>DrawUI()</code> だけが純粋仮想で必須、それ以外の hook (<code>OnInit</code> / <code>OnFrameBegin</code> / <code>OnSelectionChanged</code> / <code>OnAssetSelected</code> など) は no-op 既定なので必要な panel だけ override する。<code>ImGui::Begin/End</code> は派生側責務(基底は wrap しない)。非コピー / 非ムーブ、全 <code>noexcept</code>。",
  when: "新しいエディタ panel を作る時に継承する。<code>CEditorWorkspace</code> に登録すると毎フレーム自動で駆動される。",
  sample: "class FMyPanel : public acs::game::editor_core::AEditorPanel {\npublic:\n    const char* Title() const noexcept override { return \"My Panel\"; }\n    void DrawUI() noexcept override {\n        if (!IsVisible()) return;\n        // ... ImGui::Begin(Title(), &amp;m_Visible) ... ImGui::End()\n    }\n};",
  members: [
    { sig: "virtual const char* Title() const noexcept = 0", ret: "ウィンドウ名", desc: "<b>純粋仮想</b>。<code>ImGui::Begin</code> に渡す window タイトル。リテラル / 静的領域文字列を返す(基底はコピー所有しない)。Workspace 内で一意な名前が望ましい。", when: "必須 override。" },
    { sig: "virtual void DrawUI() noexcept = 0", ret: "—", desc: "<b>純粋仮想</b>。メインの ImGui 描画。<code>ImGui::Begin(Title(), ...)</code> / <code>ImGui::End()</code> は派生側で書く。", when: "必須 override。" },
    { sig: "virtual void OnInit(CEditorWorkspace&amp; workspace) noexcept", ret: "—", desc: "Workspace 登録時に 1 度呼ばれる。基底実装が <code>workspace</code> 参照を保存し以降 <code>Workspace()</code> で取得可能にする。override する場合は基底呼び出しを先に。", when: "追加初期化が要る時。" },
    { sig: "virtual void OnShutdown() noexcept", ret: "—", desc: "登録解除 / editor 終了時に呼ばれる。GPU リソース解放やコールバック解除をここで。既定 no-op(多重呼び出し可)。" },
    { sig: "virtual void OnFrameBegin(f32 dt) noexcept", ret: "—", desc: "フレーム開始時(UI 描画より前)に呼ばれる。input 取得 / state 更新 / 非同期ポーリング用。<code>dt</code> はスケール後の秒。既定 no-op。" },
    { sig: "virtual void OnSelectionChanged(inspector::CSelectionService&amp; selection) noexcept", ret: "—", desc: "AScene 内 ANode の選択が変わった時に呼ばれる(Phase 20 <code>CSelectionService</code> 統合)。既定 no-op。", when: "Inspector 等 node 系 panel。" },
    { sig: "virtual void OnAssetSelected(const char* asset_path) noexcept", ret: "—", desc: "<code>CAssetBrowser</code> からファイル選択された時の通知。<code>asset_path</code> は Browser 所有文字列(コピー禁止)。<code>nullptr</code> は「選択解除」。既定 no-op。", when: "ModelViewer 等 asset 系 panel。" },
    { sig: "virtual bool WantsFocus() const noexcept", ret: "Focus 要求", desc: "起動時 / dock reset 時に <code>true</code> を返すと一度だけ window focus が当たる。既定 <code>false</code>。" },
    { sig: "virtual void OnSaveLayout(FEditorLayoutSerializer&amp; out) noexcept / OnLoadLayout(... in)", ret: "—", desc: "panel 独自のレイアウト情報(splitter 比率 / カラム幅 / 表示モード)の永続化予約点。Phase 21c 本実装、現状 stub(no-op)。" },
    { sig: "bool IsVisible() const noexcept / void SetVisible(bool)", ret: "表示状態", desc: "panel 表示状態(close ボタン or プログラム的 hide)の取得 / 設定。" },
    { sig: "bool IsDockTarget() const noexcept / void SetDockTarget(bool)", ret: "dock 可否", desc: "dock target にできるかのヒント。ステータスバー / ツールバーは false、ビュー / インスペクタは true が典型。" },
    { sig: "CEditorWorkspace* Workspace() const noexcept", ret: "Workspace*", desc: "<code>OnInit</code> で保存された Workspace ポインタ(non-owning)。OnInit 前 / OnShutdown 後は <code>nullptr</code>。" },
        { sig: "using FEditorPanel = AEditorPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AEditorPanel</code> を使う。" }
      ]
},
{
  name: "CEditorWorkspace",
  kind: "クラス(サービス hub)", header: "gameframework/tools/editor_core/EditorWorkspace.h",
  summary: "<b>複数の <code>AEditorPanel</code> を統括するワークスペース</b>。panel 群の登録・探索、毎フレームの main loop(<code>OnFrameBegin</code> → DockSpace → MenuBar → <code>DrawUI</code>)、ImGui DockSpace / Window メニュー、<code>.acslayout</code> でのレイアウト永続化、選択 / asset 選択イベントの全 panel への broadcast を担う。panel は <b>raw pointer の非所有保持</b>(caller 所有)で、登録順 = dispatch 順。非コピー / 非ムーブ、全 <code>noexcept</code>。",
  when: "複数 panel を 1 つの editor アプリとしてまとめて配線する中央 hub が要る時。",
  sample: "CEditorWorkspace ws;\nws.Init();\nws.RegisterPanel(&amp;inspector_panel);   // panel は caller 所有\nws.RegisterPanel(&amp;model_viewer);\n// 毎フレーム:\nws.TickAllPanels(dt);\n// 終了時:\nws.Shutdown();",
  members: [
    { sig: "void Init() noexcept", ret: "—", desc: "panel list を空、selection service を <code>nullptr</code>、フラグを default に。多重 Init 可(完全リセット)。<b>登録 panel に OnShutdown を呼ばず破棄するので Init 前に Shutdown すること</b>。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "全 panel に <code>OnShutdown</code> を呼んでから list を解放。selection service 参照も解除。多重 Shutdown 可。" },
    { sig: "void RegisterPanel(AEditorPanel* panel) noexcept", ret: "—", desc: "panel を末尾追加し <code>OnInit</code> を呼ぶ。所有は caller。二重登録 / <code>nullptr</code> は no-op。<code>kMaxPanels</code> 到達後も silent no-op。" },
    { sig: "void UnregisterPanel(AEditorPanel* panel) noexcept", ret: "—", desc: "登録解除して <code>OnShutdown</code> を呼ぶ。順序保存削除(shift)。未登録 / <code>nullptr</code> は no-op。" },
    { sig: "u32 PanelCount() const noexcept", ret: "件数", desc: "現在の登録 panel 数。" },
    { sig: "AEditorPanel* GetPanelByIndex(u32 i) const noexcept", ret: "panel or null", desc: "<code>i</code> 番目の panel。範囲外は <code>nullptr</code>。" },
    { sig: "AEditorPanel* FindPanelByTitle(const char* title) const noexcept", ret: "panel or null", desc: "<code>Title()</code> が完全一致(strcmp)する panel。なし / <code>nullptr</code> は <code>nullptr</code>。" },
    { sig: "void TogglePanelVisible(const char* title) noexcept", ret: "—", desc: "title 一致 panel の <code>m_Visible</code> を反転。未発見 / null は no-op。" },
    { sig: "void TickAllPanels(f32 dt) noexcept", ret: "—", desc: "全 panel の 1 フレーム分を駆動: 各 <code>OnFrameBegin(dt)</code> → DockSpace 描画 → MenuBar 描画 → 各 <code>DrawUI</code>。", when: "毎フレーム呼ぶ。" },
    { sig: "void DrawDockSpace() noexcept", ret: "—", desc: "<code>ImGui::DockSpaceOverViewport</code> で main viewport に dock 空間を出す。<b><code>IMGUI_HAS_DOCK</code> 未定義時は no-op</b>(各 panel は float window として並ぶ)。<code>TickAllPanels</code> から自動呼出。" },
    { sig: "void DrawMenuBar() noexcept", ret: "—", desc: "MainMenuBar に \"Window\"(panel toggle) / \"Layout\"(Save / Load) メニューを追加。<code>SetEnableMenuBar(false)</code> で抑制可。" },
    { sig: "void SaveLayout(const wchar_t* file_path) noexcept", ret: "—", desc: "ImGui ini + 各 panel の visible / dock_target を <code>.acslayout</code> テキストに書き出す。失敗時は silent(ログのみ)。" },
    { sig: "void LoadLayout(const wchar_t* file_path) noexcept", ret: "—", desc: "<code>.acslayout</code> を復元。version 不一致 / 構文エラー / 未登録 title は安全に skip。失敗時も silent。" },
    { sig: "void SetSelectionService(inspector::CSelectionService* svc) noexcept", ret: "—", desc: "<code>CSelectionService</code> 参照を登録 / 解除(<code>nullptr</code> で解除)。caller 所有。" },
    { sig: "inspector::CSelectionService* GetSelectionService() const noexcept", ret: "service*", desc: "登録中の selection service。未注入時 <code>nullptr</code>。" },
    { sig: "void BroadcastSelectionChanged() noexcept", ret: "—", desc: "全 panel に <code>OnSelectionChanged</code> を fan-out。selection service 未注入時は no-op。" },
    { sig: "void BroadcastAssetSelected(const char* asset_path) noexcept", ret: "—", desc: "全 panel に <code>OnAssetSelected</code> を fan-out。<code>asset_path</code> は pass-through(workspace は保持しない)。<code>nullptr</code> は選択解除。" },
    { sig: "void SetNoDockingInCentralNode(bool) / bool NoDockingInCentralNode() const", ret: "—", desc: "central node 内 docking を無効化(central を「メインビュー」専用にしたい時 true)。既定 false。" },
    { sig: "void SetEnableDockSpace(bool) / bool IsDockSpaceEnabled() const", ret: "—", desc: "DockSpace 自動描画の on/off。既定 true。" },
    { sig: "void SetEnableMenuBar(bool) / bool IsMenuBarEnabled() const", ret: "—", desc: "MainMenuBar 自動描画の on/off。既定 true。" },
    { sig: "static constexpr const char* kLayoutMagic = \"ACS_EDLAYOUT\"", desc: "<code>.acslayout</code> ファイルの magic 文字列。" },
    { sig: "static constexpr u32 kLayoutVersion = 1u", desc: "レイアウトファイルの version。" },
    { sig: "static constexpr u32 kMaxPanels = 32u", desc: "同時登録可能 panel 数の上限。超過した <code>RegisterPanel</code> は silent no-op。" },
        { sig: "using FEditorWorkspace = CEditorWorkspace", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CEditorWorkspace</code> を使う。" }
      ]
},
{
  name: "EAssetKind",
  kind: "列挙(enum class : u8)", header: "gameframework/tools/editor_core/AssetBrowser.h",
  summary: "<b>拡張子から推測されるアセット種別</b>。<code>CAssetBrowser</code> の kind フィルタ / アイコン表示 / <code>ClassifyByExtension</code> の戻り値に使う。<code>Unknown</code> は未分類(未知拡張子 or path が <code>nullptr</code> / 空)、<code>Other</code> は既知だがどの分類にも該当しない汎用ファイル(現状未使用 / 利用側拡張用)。",
  when: "アセットの種類で分岐 / フィルタしたい時。",
  members: [
    { sig: "Unknown = 0", desc: "未分類(未知拡張子 or 空 path)。フィルタ解除値としても使う。" },
    { sig: "Texture = 1", desc: ".png .jpg .jpeg .tga .bmp .dds .ktx .hdr" },
    { sig: "Mesh = 2", desc: ".mdl .fbx .gltf .glb .obj" },
    { sig: "Font = 3", desc: ".ttf .otf" },
    { sig: "Audio = 4", desc: ".wav .ogg .mp3 .flac" },
    { sig: "Material = 5", desc: ".mat .material" },
    { sig: "Particle = 6", desc: ".fx .particle" },
    { sig: "Animation = 7", desc: ".anim" },
    { sig: "BehaviorTree = 8", desc: ".bt" },
    { sig: "Tilemap = 9", desc: ".tilemap .tmx" },
    { sig: "Prefab = 10", desc: ".prefab" },
    { sig: "Cinematic = 11", desc: ".cine" },
    { sig: "Scene = 12", desc: ".scene" },
    { sig: "Other = 13", desc: "既知だがどれにも該当しないファイル(現状未使用 / 利用側拡張用)。" }
  ]
},
{
  name: "FAssetEntry",
  kind: "構造体", header: "gameframework/tools/editor_core/AssetBrowser.h",
  summary: "<b>列挙された 1 件のディレクトリ / ファイル</b>。<code>path</code> / <code>short_name</code> は <code>CAssetBrowser</code> の内部 pool を指し、<b>寿命は次の <code>Refresh()</code> まで</b>(再列挙で pool がクリアされ pointer 無効化)。保持したいなら利用側でバッファに退避する。",
  when: "<code>CAssetBrowser::GetEntry</code> で current directory のエントリを 1 件ずつ読む時。",
  members: [
    { sig: "const wchar_t* path = nullptr", desc: "assets/ ルートからの相対パス(区切りは <code>\\</code>)。" },
    { sig: "const char* short_name = nullptr", desc: "path の末尾セグメント(UTF-8)。" },
    { sig: "EAssetKind kind = EAssetKind::Unknown", desc: "拡張子から判定したアセット種別。" },
    { sig: "u64 file_size_bytes = 0", desc: "ファイルサイズ。<code>is_directory == true</code> なら 0。" },
    { sig: "u64 last_modified = 0", desc: "Win32 FILETIME(100ns 単位)。" },
    { sig: "bool is_directory = false", desc: "ディレクトリなら true。" }
  ]
},
{
  name: "CAssetBrowser",
  kind: "クラス(panel)", header: "gameframework/tools/editor_core/AssetBrowser.h",
  summary: "<b>assets/ ディレクトリツリーを ImGui で参照し、Drag &amp; Drop で他 panel にアセットパスを供給する</b> panel(Unity の Project Window / Godot の FileSystem Dock 相当)。左ペインに tree、右ペインに current directory 一覧を表示。各エントリは <code>\"ASSET_PATH\"</code> payload(<code>const wchar_t*</code> 直渡し)の Drag Source。拡張子から <code>EAssetKind</code> を判定。非コピー / 非ムーブ、全 <code>noexcept</code>、文字列は内部 pool 管理。",
  when: "エディタにアセットブラウザを組み込み、ファイルを panel へドラッグ供給したい時。",
  sample: "CAssetBrowser browser;\nbrowser.Init(L\"assets\");\n// 毎フレーム:\nbrowser.DrawUI();\nif (const wchar_t* p = browser.SelectedAssetPath())\n    use(p);   // 寿命は次の Refresh まで",
  members: [
    { sig: "using AssetSelectedCallback = void (*)(void* user, const wchar_t* path, EAssetKind kind) noexcept", desc: "選択変更通知 callback 型。<code>user</code> は登録時のポインタがそのまま戻る。<code>path</code> の寿命は次の <code>Refresh()</code> まで。" },
    { sig: "using AssetDoubleClickedCallback = void (*)(void* user, const wchar_t* path, EAssetKind kind) noexcept", desc: "ダブルクリック通知 callback 型(ディレクトリは Navigate、ファイルは Open 相当)。" },
    { sig: "void Init(const wchar_t* root_directory) noexcept", ret: "—", desc: "<code>root_directory</code> を assets/ ルートとして記録し初回 <code>Refresh()</code>。<code>nullptr</code> / 空文字は <code>L\"assets\"</code> を既定採用。多重 Init 可(root 切替)。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "文字列 pool / TArray / callback を全解放。多重 Shutdown 可。" },
    { sig: "void Refresh() noexcept", ret: "—", desc: "current directory を rescan。<b>pool が再構築され既存 <code>FAssetEntry::path</code> / <code>short_name</code> が無効化される</b>ので Refresh 越しに pointer を保持しないこと。" },
    { sig: "void DrawUI() noexcept", ret: "—", desc: "<code>Begin(\"Asset Browser\")</code> 1 window で完結する描画。左 tree + 右 list。各エントリは <code>\"ASSET_PATH\"</code> Drag Source。" },
    { sig: "u32 EntryCount() const noexcept", ret: "件数", desc: "current directory の entry 数(ディレクトリ + ファイル合算)。" },
    { sig: "const FAssetEntry* GetEntry(u32 index) const noexcept", ret: "entry or null", desc: "<code>index</code> 番目の entry。範囲外は <code>nullptr</code>。寿命は次の Refresh まで。" },
    { sig: "const wchar_t* CurrentDirectory() const noexcept", ret: "相対パス", desc: "現在表示中のディレクトリ(ルートからの相対)。root 直下では <code>L\"\"</code>。" },
    { sig: "void SetCurrentDirectory(const wchar_t* path) noexcept", ret: "—", desc: "current directory 変更(空文字でルート)。内部で必ず <code>Refresh()</code>。存在しなければ no-op。" },
    { sig: "const wchar_t* SelectedAssetPath() const noexcept", ret: "path or null", desc: "選択中 entry の path。未選択は <code>nullptr</code>。寿命は次の Refresh まで。" },
    { sig: "EAssetKind SelectedAssetKind() const noexcept", ret: "kind", desc: "選択中 entry の kind。未選択は <code>EAssetKind::Unknown</code>。" },
    { sig: "void SetOnAssetSelectedCallback(AssetSelectedCallback cb, void* user) noexcept", ret: "—", desc: "選択変更通知 callback を登録 / 解除(<code>nullptr</code> で解除)。" },
    { sig: "void SetOnAssetDoubleClickedCallback(AssetDoubleClickedCallback cb, void* user) noexcept", ret: "—", desc: "ダブルクリック通知 callback を登録 / 解除。" },
    { sig: "void SetFilterByKind(EAssetKind kind) noexcept", ret: "—", desc: "kind フィルタを設定。<code>Unknown</code> でフィルタ解除(全表示)。<b>ディレクトリ entry は常に表示</b>(フィルタ非対象)。" },
    { sig: "static EAssetKind ClassifyByExtension(const wchar_t* path) noexcept", ret: "kind", desc: "拡張子から <code>EAssetKind</code> を判定する static ヘルパ。<code>nullptr</code> / 拡張子無しは <code>Unknown</code>。大文字小文字無視。" },
    { sig: "static constexpr const char* kDragDropPayloadId = \"ASSET_PATH\"", desc: "Drag &amp; Drop payload identifier(ImGui 仕様で 32 文字以内)。" },
    { sig: "static constexpr usize kInitialPathPoolBytes = 64u * 1024u", desc: "パス / 名前 pool の初期容量(足りなければ自動成長)。" },
    { sig: "static constexpr usize kMaxPathChars = 512u", desc: "1 entry あたりの最大 path 長(wchar_t 単位、終端含まず)。" },
        { sig: "using FAssetBrowser = CAssetBrowser", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CAssetBrowser</code> を使う。" }
      ]
},
{
  name: "EGizmoMode",
  kind: "列挙(enum class : u8)", header: "gameframework/tools/editor_core/EditorGizmo.h",
  summary: "<b>ギズモの操作モード</b>。<code>None</code> はハンドルを描画も操作もしない OFF 状態。editor の Q/W/E/R 系キーで切り替える想定。",
  when: "<code>CEditorGizmo::SetMode</code> で translate / rotate / scale を切り替える時。",
  members: [
    { sig: "None = 0", desc: "gizmo 完全 OFF(描画も操作もしない)。" },
    { sig: "Translate = 1", desc: "移動(axis arrow + plane handle)。" },
    { sig: "Rotate = 2", desc: "回転(axis ring)。" },
    { sig: "Scale = 3", desc: "拡縮(axis box)。" }
  ]
},
{
  name: "EGizmoSpace",
  kind: "列挙(enum class : u8)", header: "gameframework/tools/editor_core/EditorGizmo.h",
  summary: "<b>ギズモの操作座標系</b>。<code>World</code> は固定ワールド軸、<code>Local</code> は対象 ANode の rotation を考慮したローカル軸を表示・操作する。",
  when: "<code>CEditorGizmo::SetSpace</code> で world / local を切り替える時。",
  members: [
    { sig: "World = 0", desc: "ワールド軸(X/Y/Z 固定)。既定。" },
    { sig: "Local = 1", desc: "ANode のローカル軸(rotation を考慮)。" }
  ]
},
{
  name: "EGizmoAxis",
  kind: "列挙(enum class : u8)", header: "gameframework/tools/editor_core/EditorGizmo.h",
  summary: "<b>どの軸 / 平面ハンドルが hot(ホバー or drag 中)か</b>。<code>None_</code> の末尾アンダースコアは <code>None</code> キーワード衝突回避の ACS 規約。XY / XZ / YZ は translate モードの平面ハンドル。<code>ScreenAlign</code> は rotate の画面平行ハンドル用で Phase 21a では値だけ予約。",
  when: "<code>CEditorGizmo::HotAxis()</code> や <code>FGizmoState::hot_axis</code> で現在 hot な軸を読む時。",
  members: [
    { sig: "None_ = 0", desc: "どのハンドルもホバー / drag していない。" },
    { sig: "X = 1 / Y = 2 / Z = 3", desc: "各軸ハンドル。" },
    { sig: "XY = 4 / XZ = 5 / YZ = 6", desc: "平面ハンドル(translate モードで 2 軸同時 drag)。" },
    { sig: "ScreenAlign = 7", desc: "画面平行ハンドル(rotate 用、Phase 21a は予約)。" }
  ]
},
{
  name: "FGizmoState",
  kind: "構造体(POD)", header: "gameframework/tools/editor_core/EditorGizmo.h",
  summary: "<b>現フレームのギズモ操作状態</b>。公開フィールドだが書き換えは <code>CEditorGizmo</code> 内部のみ。テストや inspector から「今 drag 中か」「どの軸が hot か」を <code>ImGui::Text</code> 等で覗ける。<code>drag_start_world</code> は drag 開始時の world ヒット点(delta 計算の基点)。",
  when: "<code>CEditorGizmo::State()</code> でギズモの内部状態を読み取る時。",
  members: [
    { sig: "EGizmoMode mode = EGizmoMode::Translate", desc: "現在のモード。" },
    { sig: "EGizmoSpace space = EGizmoSpace::World", desc: "現在の座標系。" },
    { sig: "EGizmoAxis hot_axis = EGizmoAxis::None_", desc: "ホバー or drag 中の軸。" },
    { sig: "bool dragging = false", desc: "LMB 押下中の drag フラグ。" },
    { sig: "acs::FVec3 drag_start_world{}", desc: "drag 開始時の world hit point。" }
  ]
},
{
  name: "ManipulateCallback",
  kind: "型エイリアス(関数ポインタ)", header: "gameframework/tools/editor_core/EditorGizmo.h",
  summary: "<b>drag 終了時に外部へ delta を通知する callback 型</b>。drag 完了時に 1 度だけ呼ばれ、<code>CUndoStack</code> に <code>AMoveNodeCommand</code> 等を push する適切なタイミングになる。<code>delta</code> はモード依存(Translate=移動量 / Rotate=euler 差分[radians] / Scale=倍率差分)。<code>user</code> は <code>SetOnManipulateCallback</code> の第二引数がそのまま戻る。",
  when: "ギズモ操作完了を undo stack やエディタの dirty フラグに反映したい時。",
  members: [
    { sig: "using ManipulateCallback = void (*)(void* user, EGizmoMode mode, acs::FVec3 delta) noexcept", desc: "drag 終了通知。<code>mode</code> で delta の解釈が変わる。" }
  ]
},
{
  name: "CEditorGizmo",
  kind: "クラス(サービス)", header: "gameframework/tools/editor_core/EditorGizmo.h",
  summary: "<b>選択 ANode の Transform を viewport 上で直接ドラッグ操作するハンドル</b>。translate / rotate / scale の 3 モードを持ち、各モードで X/Y/Z 軸 + 平面 + 画面並列ハンドルを提供する。<b><code>ProcessInput</code> → <code>Manipulate</code> → <code>DrawGizmo</code> の 3 段</b>に完全分離(headless test 可)。描画は <code>CDebugDraw</code> 経由でレンダラ非依存(現状 2D 射影 = top-down)。非コピー / 非ムーブ、全 <code>noexcept</code>。",
  when: "シーンビュー上で ANode の移動 / 回転 / 拡縮ハンドルを実装する時。",
  sample: "CEditorGizmo gizmo;\ngizmo.Init();\ngizmo.SetMode(EGizmoMode::Translate);\ngizmo.ProcessInput(ray_o, ray_d, lmb_down, lmb_held, lmb_up);\nif (gizmo.Manipulate(pos, rot, scl)) {\n    node.SetWorldPosition(pos);\n}\ngizmo.DrawGizmo(debug_draw, pos, rot, scl);",
  members: [
    { sig: "void Init() noexcept", ret: "—", desc: "state を default に戻す(callback はクリアしない)。多重 Init 可。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "state + callback + snap step を完全初期化。多重 Shutdown 可。" },
    { sig: "void SetMode(EGizmoMode mode) noexcept / EGizmoMode Mode() const", ret: "—", desc: "操作モードの設定 / 取得。" },
    { sig: "void SetSpace(EGizmoSpace space) noexcept / EGizmoSpace Space() const", ret: "—", desc: "操作座標系の設定 / 取得。" },
    { sig: "void SetSnapTranslate(f32 step) noexcept", ret: "—", desc: "移動 snap step(world units)。<code>step &lt;= 0</code> で無効。" },
    { sig: "void SetSnapRotate(f32 step_deg) noexcept", ret: "—", desc: "回転 snap step(degrees、内部で radians 換算)。" },
    { sig: "void SetSnapScale(f32 step) noexcept", ret: "—", desc: "拡縮 snap step(倍率、例 0.1 で 10% 刻み)。" },
    { sig: "f32 SnapTranslate() const / f32 SnapRotateDeg() const / f32 SnapScale() const", ret: "step", desc: "各 snap step の取得(rotate は degree で返す)。" },
    { sig: "void ProcessInput(acs::FVec3 mouse_ray_origin, acs::FVec3 mouse_ray_direction, bool lmb_down, bool lmb_held, bool lmb_up) noexcept", ret: "—", desc: "毎フレーム、マウス ray と LMB の edge/level 状態を渡して drag 開始 / 継続 / 終了の遷移を更新する。<b><code>Manipulate</code> / <code>DrawGizmo</code> の前に呼ぶ</b>。drag 中は hot_axis を保持、非 drag 時のみ毎フレーム再判定。", when: "毎フレーム最初に。" },
    { sig: "bool Manipulate(acs::FVec3&amp; inout_position, acs::FVec3&amp; inout_rotation_euler, acs::FVec3&amp; inout_scale) noexcept", ret: "変更あり", desc: "<code>ProcessInput</code> の後に呼ぶ。drag 中なら inout を in-place 更新し <code>true</code>。Translate=position に delta 加算 / Rotate=euler に angular delta 加算(radians) / Scale=scale に倍率 delta 加算。", when: "ProcessInput の直後。" },
    { sig: "void DrawGizmo(CDebugDraw&amp; dd, acs::FVec3 position, acs::FVec3 rotation_euler, acs::FVec3 scale) noexcept", ret: "—", desc: "<code>CDebugDraw</code> 経由で軸 line + ハンドルを描く。CDebugDraw が 2D のため Z は XY 平面へ射影。色: X=red / Y=green / Z=blue / 平面=半透明黄 / hot=白。" },
    { sig: "bool IsDragging() const noexcept / EGizmoAxis HotAxis() const noexcept", ret: "状態", desc: "drag 中か / 現在 hot な軸を返す。" },
    { sig: "const FGizmoState&amp; State() const noexcept", ret: "FGizmoState&amp;", desc: "<code>FGizmoState</code> の現値をまるごと参照で返す(テスト / inspector 用)。" },
    { sig: "void SetOnManipulateCallback(ManipulateCallback cb, void* user) noexcept", ret: "—", desc: "drag 終了時に 1 度呼ばれる callback を登録(<code>nullptr</code> で解除)。" },
    { sig: "void SetAxisLength(f32) / SetHandleRadius(f32) / SetPlaneHandleSize(f32) noexcept", ret: "—", desc: "ハンドル形状: 軸長(既定 1.0) / 軸 hit 円柱半径(既定 0.05) / 平面 hit 矩形半サイズ(既定 0.2)。camera 距離スケーリングは外側責務。" },
    { sig: "f32 AxisLength() const / f32 HandleRadius() const / f32 PlaneHandleSize() const", ret: "値", desc: "各ハンドル形状パラメータの取得。" },
        { sig: "using FEditorGizmo = CEditorGizmo", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CEditorGizmo</code> を使う。" }
      ]
},
{
  name: "EEditorCameraMode",
  kind: "列挙(enum class : u8)", header: "gameframework/tools/editor_core/EditorCamera.h",
  summary: "<b><code>CEditorCamera</code> の操作モデル</b>。<code>Mode2D</code> は pan / zoom のみ(top-down / tilemap editor)、<code>Mode3D</code> は target を中心に orbit + pan + dolly(model viewer)。",
  when: "<code>CEditorCamera::Init</code> / <code>SetMode</code> で 2D / 3D を選ぶ時。",
  members: [
    { sig: "Mode2D = 0", desc: "2D pan / zoom 操作。" },
    { sig: "Mode3D = 1", desc: "3D orbit + pan + dolly 操作。" }
  ]
},
{
  name: "FEditorCameraState",
  kind: "構造体(POD)", header: "gameframework/tools/editor_core/EditorCamera.h",
  summary: "<b>カメラの公開 state</b>。debug overlay / serializer から position / target 等を直接覗くための可視 struct(<code>CEditorCamera::State()</code> が const 参照で返す)。直接書き換えは <code>SetTarget</code> 等の accessor 経由推奨(smooth_target との 2 重管理が崩れるため)。",
  when: "カメラの現在値を読み取り / シリアライズしたい時。",
  members: [
    { sig: "FVec3 position{0,0,0}", desc: "実 eye position(3D)。2D では (x,y) を使用。" },
    { sig: "FVec3 target{0,0,0}", desc: "orbit center(3D)。2D では未使用。" },
    { sig: "FVec3 up{0,1,0}", desc: "up vector(LookAt 用)。" },
    { sig: "f32 fov_deg = 60.0f", desc: "3D perspective の vertical FoV(degree)。" },
    { sig: "f32 zoom_2d = 1.0f", desc: "2D ortho zoom(1.0 で基本、&gt;1 で拡大)。" },
    { sig: "f32 yaw_rad = 0.0f", desc: "Y 軸まわりの orbit 角(0 = +Z 方向から見る)。" },
    { sig: "f32 pitch_rad = 0.0f", desc: "水平面からの仰角(負で見下ろし)。" },
    { sig: "f32 distance = 10.0f", desc: "target からの距離(3D orbit)。" }
  ]
},
{
  name: "CEditorCamera",
  kind: "クラス(サービス)", header: "gameframework/tools/editor_core/EditorCamera.h",
  summary: "<b>2D / 3D を 1 クラスで扱う editor カメラコントローラ</b>。3D は yaw + pitch + distance + target の Maya / Blender 風 orbit、2D は position + zoom_2d の pan/zoom。<code>HandleMouseInput</code> でマウス IO を集約し、<code>Tick</code> が framerate-independent な <code>1 - exp(-rate*dt)</code> 補間で smoothing する。view / projection 行列を生成。非コピー / 非ムーブ、全 <code>noexcept</code>。",
  when: "ModelViewer / LevelEditor 等の viewport カメラを動かす時。",
  sample: "CEditorCamera cam;\ncam.Init(EEditorCameraMode::Mode3D);\n// 毎フレーム:\ncam.HandleMouseInput(mouse_delta, lmb, rmb, mmb, wheel);\ncam.Tick(dt);\nFMat4 view = cam.ViewMatrix();\nFMat4 proj = cam.ProjectionMatrix(aspect, 0.1f, 1000.0f);",
  members: [
    { sig: "void Init(EEditorCameraMode mode) noexcept", ret: "—", desc: "指定モードで完全初期化(Reset 相当 + mode 設定)。多重呼び出し可。" },
    { sig: "void SetMode(EEditorCameraMode mode) noexcept / EEditorCameraMode Mode() const", ret: "—", desc: "mode 切替 / 取得。state は保持(3D ↔ 2D で position / zoom は独立)。" },
    { sig: "void HandleMouseInput(FVec2 mouse_delta, bool lmb, bool rmb, bool mmb, f32 wheel_delta) noexcept", ret: "—", desc: "マウス IO を流し込むエントリ。3D: L/R drag=orbit / M drag=pan / wheel=dolly。2D: L/M drag=pan / wheel=zoom。", when: "毎フレーム。" },
    { sig: "void Pan(FVec2 screen_delta) noexcept", ret: "—", desc: "2D は world position を平行移動、3D は orbit target を camera right / up 方向に平行移動。" },
    { sig: "void Orbit(f32 yaw_delta, f32 pitch_delta) noexcept", ret: "—", desc: "3D 専用: yaw / pitch を加算(2D では no-op)。pitch は ±89° にクランプ。" },
    { sig: "void Dolly(f32 delta) noexcept", ret: "—", desc: "3D は distance を倍率変化(&gt;0 で寄る)、2D は zoom_2d を倍率変化(&gt;0 で拡大)。" },
    { sig: "void Reset() noexcept", ret: "—", desc: "初期状態へ。3D: target=origin / distance=10 / yaw=0 / pitch=-30°。2D: position=origin / zoom_2d=1.0。" },
    { sig: "void FrameToBoundingSphere(FVec3 center, f32 radius) noexcept", ret: "—", desc: "bounding sphere が画面に収まるよう target / distance を配置(margin 1.2x)。2D は ortho 全幅 fit。" },
    { sig: "void FrameToBoundingBox2D(FVec2 min_xy, FVec2 max_xy) noexcept", ret: "—", desc: "2D 専用: XY 平面の AABB 全体が見える zoom / pos を計算。" },
    { sig: "FMat4 ViewMatrix() const noexcept", ret: "FMat4", desc: "LookAt ベースの view 行列(LH)。3D は (eye, target, up)、2D は XY 平面 LookAt。" },
    { sig: "FMat4 ProjectionMatrix(f32 aspect, f32 near_plane, f32 far_plane) const noexcept", ret: "FMat4", desc: "mode に応じ perspective(3D) / orthographic(2D) を生成。2D ortho 幅は <code>base_ortho_size / zoom_2d</code>。" },
    { sig: "const FEditorCameraState&amp; State() const noexcept", ret: "state&amp;", desc: "公開 state(実値)を const 参照で返す。" },
    { sig: "void SetFovDeg(f32 deg) noexcept / f32 GetFovDeg() const", ret: "—", desc: "3D FoV(degree)の設定 / 取得。" },
    { sig: "void SetTarget(FVec3 target) noexcept / FVec3 GetTarget() const", ret: "—", desc: "orbit target を直接設定(smooth_target にも反映 = 即追従) / 取得。" },
    { sig: "void SetPosition(FVec3 position) noexcept / FVec3 GetPosition() const", ret: "—", desc: "eye position を直接設定(3D は distance / yaw / pitch も逆算) / 取得。" },
    { sig: "void Tick(f32 dt) noexcept", ret: "—", desc: "smooth target follow + smooth zoom 補間を進める。<code>1 - exp(-rate*dt)</code> モデル。", when: "毎フレーム呼ぶ。" },
    { sig: "void SetSmoothing(f32 rate) noexcept / f32 GetSmoothing() const", ret: "—", desc: "smoothing rate の設定 / 取得(0 = 即時、5.0 = 既定)。" },
    { sig: "void SetBaseOrthoSize(f32 world_width) noexcept / f32 GetBaseOrthoSize() const", ret: "—", desc: "2D の zoom_2d=1 時の表示幅(world units、既定 20.0)の設定 / 取得。" },
    { sig: "FVec3 ComputeEye() const noexcept", ret: "FVec3", desc: "3D orbit の現 eye 位置を spherical 計算で算出(<code>ViewMatrix</code> 内部と等価)。ray pick 用。" },
        { sig: "using FEditorCamera = CEditorCamera", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CEditorCamera</code> を使う。" }
      ]
},
{
  name: "CUndoStack",
  kind: "クラス(サービス)", header: "gameframework/tools/editor_core/UndoStack.h",
  summary: "<b>全エディタが共有する undo / redo の中央ハブ</b>。<code>AEditorCommand</code> 派生を所有し、<code>Push</code> で 1 操作を実行・登録、<code>Undo</code> / <code>Redo</code> で巻き戻し / やり直す。<b><code>Push</code> 内で <code>Execute</code> も実行</b>(Execute 忘れを構造防止)、<b>Push は redo stack を破棄</b>、連続 drag は <code>CanMerge</code> / <code>MergeWith</code> で 1 件にまとめる、上限超過時は最古を drop。非コピー / 非ムーブ、全 <code>noexcept</code>、内部は <code>TArray&lt;TUniquePtr&lt;AEditorCommand&gt;&gt;</code>。",
  when: "エディタ操作の undo / redo を統一管理する時。",
  sample: "CUndoStack stack;\nstack.Init(64);\nstack.Push(cmd);   // 所有権を渡す + Execute 実行\nif (stack.CanUndo()) stack.Undo();",
  members: [
    { sig: "void Init(u32 max_history = 64) noexcept", ret: "—", desc: "初期化(履歴を空にし上限を設定)。多重 Init 可。<code>0</code> 渡しは既定 64 にフォールバック。" },
    { sig: "void Push(AEditorCommand* cmd) noexcept", ret: "—", desc: "<b>所有権を奪う</b>。<code>cmd-&gt;Execute()</code> を呼び、直前と merge 可なら merge して破棄、不可なら積む。redo stack を Clear し、上限超過なら最古を drop、callback を発火。<code>nullptr</code> は no-op(delete されない)。" },
    { sig: "bool Undo() noexcept", ret: "成否", desc: "undo を 1 件巻き戻す。top の <code>Undo()</code> を呼び redo stack へ Move、callback 発火。空なら <code>false</code>。" },
    { sig: "bool Redo() noexcept", ret: "成否", desc: "redo を 1 件やり直す。<code>Execute()</code> を呼び undo stack へ Move、callback を <code>is_redo=true</code> で発火。空なら <code>false</code>。" },
    { sig: "bool CanUndo() const noexcept / bool CanRedo() const noexcept", ret: "可否", desc: "undo / redo 可能か(ImGui MenuItem の enabled に渡す想定)。" },
    { sig: "u32 UndoCount() const noexcept / u32 RedoCount() const noexcept", ret: "件数", desc: "undo / redo stack の件数。" },
    { sig: "const char* UndoDescription() const noexcept / const char* RedoDescription() const noexcept", ret: "ラベル", desc: "top の <code>Description()</code>(UI 表示用)。空のときは <code>nullptr</code> ではなく空文字列 <code>\"\"</code> を返す。" },
    { sig: "void Clear() noexcept", ret: "—", desc: "全 cmd 破棄(<code>TUniquePtr</code> の dtor で自動 delete)。" },
    { sig: "void SetOnExecutedCallback(CommandExecutedCallback cb, void* user) noexcept", ret: "—", desc: "Push / Undo / Redo 後の副反応 callback を 1 個保持(単一購読)。<code>nullptr</code> で解除。" },
        { sig: "using FUndoStack = CUndoStack", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CUndoStack</code> を使う。" }
      ]
},
{
  name: "CommandExecutedCallback",
  kind: "型エイリアス(関数ポインタ)", header: "gameframework/tools/editor_core/UndoStack.h",
  summary: "<b><code>CUndoStack</code> の Push / Undo / Redo 後の副反応 callback 型</b>。「Undo されたから hierarchy 再描画」「Redo されたから dirty 立て直し」等を書くための出口。<code>cmd</code> は最終的にスタックに載った command(merge があれば top)、<code>is_redo</code> は Redo 経路なら <code>true</code>(Push / Undo は <code>false</code>)。",
  when: "undo / redo 操作に連動した UI 更新や dirty フラグ管理が要る時。",
  members: [
    { sig: "using CommandExecutedCallback = void (*)(void* user, const class AEditorCommand* cmd, bool is_redo) noexcept", desc: "副反応通知。<code>user</code> は <code>SetOnExecutedCallback</code> の第二引数がそのまま戻る。" }
  ]
},
{
  name: "AEditorCommand",
  kind: "クラス(抽象基底)", header: "gameframework/tools/editor_core/EditorCommand.h",
  summary: "<b>undo / redo の原子単位</b>(GoF Command パターン)。1 操作 = 1 派生インスタンス。<code>Execute</code> / <code>Undo</code> / <code>Description</code> を必ず override し、<code>CanMerge</code> / <code>MergeWith</code> は既定で merge 拒否、連続 drag をまとめたい派生のみ override する。RTTI を避けるため <code>Kind()</code> が返す<b>静的アドレスで型を識別</b>する。純粋抽象 + virtual dtor、非コピー / 非ムーブ、全 <code>noexcept</code>。",
  when: "新しい undo 可能な操作を <code>CUndoStack</code> 経由で実装する時に継承する。",
  sample: "class FMyCmd : public acs::game::editor_core::AEditorCommand {\npublic:\n    void Execute() noexcept override { /* do */ }\n    void Undo() noexcept override    { /* undo */ }\n    const char* Description() const noexcept override { return \"My Op\"; }\n};",
  members: [
    { sig: "virtual const void* Kind() const noexcept", ret: "型 tag", desc: "RTTI なしで command の「種類」を識別する static アドレス。既定 <code>nullptr</code>(= kind 不明 / merge 対象外)。派生は自前の静的アドレスを返す。" },
    { sig: "virtual void Execute() noexcept = 0", ret: "—", desc: "<b>純粋仮想</b>。「Do」を実行。Push 直後と Redo 経路の両方から呼ばれるので idempotent に書く。" },
    { sig: "virtual void Undo() noexcept = 0", ret: "—", desc: "<b>純粋仮想</b>。Execute の逆操作。Execute → Undo → Execute の往復対称性が要件。" },
    { sig: "virtual const char* Description() const noexcept = 0", ret: "ラベル", desc: "<b>純粋仮想</b>。undo history UI 用の短い人間可読ラベル。<b>永続文字列</b>(literal or 派生内 char[N])を返すこと。" },
    { sig: "virtual bool CanMerge(const AEditorCommand&amp; next) const noexcept", ret: "merge 可否", desc: "直後に Push される <code>next</code> を自分に merge 可能か返す。既定 <code>false</code>。同対象 + 同 kind のときのみ true を返すよう派生で実装。" },
    { sig: "virtual void MergeWith(const AEditorCommand&amp; next) noexcept", ret: "—", desc: "<code>CanMerge</code> が true の直後に <code>CUndoStack</code> が呼ぶ。<code>next</code> の「new 値」を取り込み(old 値は保持)。既定 no-op。" },
        { sig: "using FEditorCommand = AEditorCommand", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AEditorCommand</code> を使う。" }
      ]
},
{
  name: "AMoveNodeCommand",
  kind: "クラス(AEditorCommand 派生サンプル)", header: "gameframework/tools/editor_core/EditorCommand.h",
  summary: "<b><code>ANode</code> の 2D position を変更する <code>AEditorCommand</code> 派生の教科書的サンプル</b>。old / new 位置を保持し、<code>Execute</code> / <code>Undo</code> から <code>SetPosition2D</code> で new / old を設定する。<b>同一 target の連続 <code>AMoveNodeCommand</code> を 1 件にまとめる <code>CanMerge</code> / <code>MergeWith</code></b> を実装(Kind tag のポインタ一致 + target 一致で判定、new 値だけ更新し old 値は始点を保持)。",
  when: "ドラッグでノード位置を変えた操作を undo 可能にする時のひな形として。",
  sample: "const acs::FVec2 old_pos = node.Position2D();\nconst acs::FVec2 new_pos = old_pos + delta;\nauto command = acs::MakeUnique&lt;acs::game::editor_core::AMoveNodeCommand&gt;(\n    &amp;node, old_pos, new_pos);\nundo_stack.Push(acs::Move(command));",
  members: [
    { sig: "AMoveNodeCommand(ANode* target, FVec2 old_pos, FVec2 new_pos) noexcept", ret: "—", desc: "対象ノードと変更前 / 後の位置で構築。" },
    { sig: "void Execute() noexcept override / void Undo() noexcept override", ret: "—", desc: "<code>target != nullptr</code> のとき <code>SetPosition2D</code> で new / old を設定する。" },
    { sig: "const char* Description() const noexcept override", ret: "\"Move Node\"", desc: "固定ラベル <code>\"Move Node\"</code> を返す。" },
    { sig: "const void* Kind() const noexcept override", ret: "型 tag", desc: "派生固有の静的アドレス(function-local static)を返し、同型判定 = ポインタ比較を可能にする。" },
    { sig: "bool CanMerge(const AEditorCommand&amp; next) const noexcept override", ret: "merge 可否", desc: "<code>next.Kind()</code> が一致 かつ target ポインタが一致するときだけ <code>true</code>(連続 drag 用)。" },
    { sig: "void MergeWith(const AEditorCommand&amp; next) noexcept override", ret: "—", desc: "<code>m_NewPos</code> を next の new 値で上書き(old 値 = drag 始点は保持)。" },
    { sig: "const ANode* Target() const / FVec2 OldPosition() const / FVec2 NewPosition() const", ret: "値", desc: "テスト / inspector 表示用アクセサ。" },
        { sig: "using FMoveNodeCommand = AMoveNodeCommand", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AMoveNodeCommand</code> を使う。" }
      ]
},
{
  name: "FPropertyContext",
  kind: "構造体(POD)", header: "gameframework/tools/editor_core/PropertyDrawer.h",
  summary: "<b>drawer に渡す全パラメータをまとめた POD</b>。フィールドはどれも省略可で、drawer 側が必要なものだけ参照する。<code>data_ptr</code> は型に応じて drawer がキャスト(F32Slider なら <code>f32*</code>、AssetPath なら <code>char[]</code>)。<code>out_changed</code> は drawer が「値を書き換えたか」を呼び出し側に返す出口(<code>nullptr</code> なら書き戻し不要)。引数増減で <code>DrawerFn</code> シグネチャが変わらないよう構造体束ねを採用。",
  when: "カスタム drawer を呼ぶ / 実装する時に編集対象や表示情報を渡す入れ物として。",
  members: [
    { sig: "void* data_ptr = nullptr", desc: "編集対象データへのポインタ(drawer がキャストする)。" },
    { sig: "const char* label = nullptr", desc: "ImGui widget の表示ラベル(<code>nullptr</code> 時は <code>\"##unnamed\"</code>)。" },
    { sig: "const char* tooltip = nullptr", desc: "hover tooltip(<code>nullptr</code> 時は非表示)。" },
    { sig: "f32 min_value = 0.0f / f32 max_value = 1.0f", desc: "F32Slider 等で使う最小 / 最大値。" },
    { sig: "const char* enum_values = nullptr", desc: "EnumCombo 用ラベル(<code>\"Item0\\0Item1\\0...\\0\"</code> 終端、ImGui::Combo 形式)。" },
    { sig: "u32 enum_count = 0", desc: "EnumCombo の有効ラベル数。" },
    { sig: "bool* out_changed = nullptr", desc: "drawer が編集発生時に <code>*out_changed = true</code> を書く出口(<code>nullptr</code> 可)。" }
  ]
},
{
  name: "DrawerFn",
  kind: "型エイリアス(関数ポインタ)", header: "gameframework/tools/editor_core/PropertyDrawer.h",
  summary: "<b>1 つの property 描画関数</b>の型。<code>std::function</code> を使わず raw 関数ポインタで実装する(ACS no-exception のため <code>noexcept</code> 必須)。drawer 内から ImGui を直接叩いてよい。",
  when: "<code>CPropertyDrawerRegistry::RegisterDrawer</code> に渡すカスタム drawer を定義する時。",
  members: [
    { sig: "using DrawerFn = void (*)(const FPropertyContext&amp; ctx) noexcept", desc: "<code>ctx</code> を受け取って 1 property を描画する関数ポインタ型。" }
  ]
},
{
  name: "CPropertyDrawerRegistry",
  kind: "クラス(サービス)", header: "gameframework/tools/editor_core/PropertyDrawer.h",
  summary: "<b><code>type_name</code>(const char* literal)→ <code>DrawerFn</code> の登録レジストリ</b>。<code>AInspectorPanel</code> の hardcode switch を超える「ゲーム固有 / エディタ拡張型」の field 表示を後付け登録できる拡張点。<code>Init()</code> が <b>bundled drawer 9 種</b>(F32Slider / Vec2Drag / Vec3Drag / Vec4Drag / ColorRGB / ColorRGBA / AssetPath / EnumCombo / TextInput)を自動登録する。同名は<b>後勝ち</b>で上書き。内部は線形探索(少数想定)。非コピー / 非ムーブ、全 <code>noexcept</code>。",
  when: "ゲーム固有型(Health / WeaponSlot 等)を inspector で美麗表示したい / field drawer を差し替えたい時。",
  sample: "CPropertyDrawerRegistry reg;\nreg.Init();   // bundled 9 種を自動登録\nreg.RegisterDrawer(\"Health\", &amp;DrawHealth);\nFPropertyContext ctx{};\nif (!reg.DrawProperty(\"Health\", ctx)) {\n    // 未登録 → 既存 EFieldKind switch にフォールバック\n}",
  members: [
    { sig: "void Init() noexcept", ret: "—", desc: "既存登録を全破棄したうえで bundled drawer 9 種を自動登録。多重呼び出し可(state を完全再構築)。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "全 drawer 登録を破棄。多重呼び出し可。" },
    { sig: "void RegisterDrawer(const char* type_name, DrawerFn fn) noexcept", ret: "—", desc: "drawer を登録 / 上書き。同 <code>type_name</code> は<b>後勝ち</b>。<code>type_name</code> が <code>nullptr</code> / 空、または <code>fn</code> が <code>nullptr</code> なら no-op。<code>type_name</code> は呼び出し側が永続所有する文字列。" },
    { sig: "void UnregisterDrawer(const char* type_name) noexcept", ret: "—", desc: "<code>type_name</code> 一致の登録 1 件を解除(末尾 swap、順序非保持)。未登録 / <code>nullptr</code> は no-op。" },
    { sig: "bool HasDrawer(const char* type_name) const noexcept", ret: "登録済か", desc: "対応 drawer が登録済みか。<code>nullptr</code> / 空文字は <code>false</code>。" },
    { sig: "bool DrawProperty(const char* type_name, const FPropertyContext&amp; ctx) const noexcept", ret: "描画した", desc: "該当 drawer を呼んで <code>ctx</code> で描画。成功なら <code>true</code>、未登録 / <code>nullptr</code> なら <code>false</code>(呼び出し側はフォールバックする)。" },
    { sig: "u32 DrawerCount() const noexcept", ret: "件数", desc: "登録済み drawer 数(bundled 含む)。" },
    { sig: "const char* DrawerName(u32 index) const noexcept", ret: "name", desc: "<code>index</code> 番目の drawer name。範囲外は <code>nullptr</code>(デバッグ / イントロスペクション用)。" },
    { sig: "void ClearAll() noexcept", ret: "—", desc: "全 drawer 登録を破棄(bundled も含め空に)。bundled を戻したいなら <code>Init()</code> を呼ぶ。" },
    { sig: "static constexpr const char* kAssetPathPayloadId = \"ASSET_PATH\"", desc: "AssetPath drawer が <code>AcceptDragDropPayload</code> で受け取る payload id。" },
    { sig: "static constexpr u32 kTextInputBufferSize = 256u", desc: "TextInput drawer が使う固定長バッファサイズ。" },
        { sig: "using FPropertyDrawerRegistry = CPropertyDrawerRegistry", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CPropertyDrawerRegistry</code> を使う。" }
      ]
},
{
  name: "EEditorThemePreset",
  kind: "列挙(enum class : u8)", header: "gameframework/tools/editor_core/EditorTheme.h",
  summary: "<b><code>CEditorTheme</code> のプリセット種別</b>。<code>ApplyPreset</code> に渡すと色 / spacing / corner のスナップショットが適用される。<code>SetCustomColors</code> を呼ぶと自動で <code>Custom</code> になる。",
  when: "エディタの配色を切り替える時。",
  members: [
    { sig: "Dark = 0", desc: "標準 dark grey(デフォルト)。" },
    { sig: "DarkBlue = 1", desc: "VS Code Dark+ 風(青みのある dark)。" },
    { sig: "Light = 2", desc: "明るい背景(屋外 / プロジェクタ向け)。" },
    { sig: "HighContrast = 3", desc: "黒 / 白 / 黄(アクセシビリティ連動可)。" },
    { sig: "Sepia = 4", desc: "焼け紙風暖色(長時間作業向け)。" },
    { sig: "Custom = 5", desc: "<code>SetCustomColors()</code> で渡された任意パレット。" }
  ]
},
{
  name: "FEditorThemeColors",
  kind: "構造体(POD)", header: "gameframework/tools/editor_core/EditorTheme.h",
  summary: "<b>preset 1 個分のカラーパレット</b>。各メンバは RGBA <code>[0,1]</code> の <code>acs::FVec4</code>(ImGui の ImVec4 と同じ意味論)。window 背景 / タイトルバー / ボタン各状態 / frame / text / border / separator / accent(強調色 = ACS ブランド色相当) / warning / error をまとめる。",
  when: "<code>SetCustomColors</code> で独自配色を渡す / <code>Colors()</code> で現パレットを読む時。",
  members: [
    { sig: "FVec4 window_bg", desc: "ImGuiCol_WindowBg(パネル背景)。" },
    { sig: "FVec4 title_bg", desc: "ImGuiCol_TitleBg / Active(タイトルバー)。" },
    { sig: "FVec4 button_bg / button_hover / button_active", desc: "ボタンの通常 / ホバー / 押下色。" },
    { sig: "FVec4 frame_bg", desc: "ImGuiCol_FrameBg(InputText / Combo 等の枠内)。" },
    { sig: "FVec4 text / text_disabled", desc: "通常文字 / グレーアウト文字。" },
    { sig: "FVec4 border / separator", desc: "枠線 / 区切り線。" },
    { sig: "FVec4 accent", desc: "CheckMark / SliderGrab / TabActive 等の強調色(ACS ブランド色相当)。" },
    { sig: "FVec4 warning / error", desc: "警告 / エラーメッセージ用(本 theme で手動適用する用途)。" }
  ]
},
{
  name: "CEditorTheme",
  kind: "クラス(サービス)", header: "gameframework/tools/editor_core/EditorTheme.h",
  summary: "<b>全エディタ panel が共通参照する ImGui スタイル統一テーマ管理</b>。色パレット / spacing / font scale / corner radius を 1 ヶ所で管理し、<code>Init()</code> + <code>ApplyPreset</code> で全エディタが統一見た目になる。<code>acs::FVec4</code> ベースで ImVec4 変換は .cpp 内のみ(ヘッダから ImGui を漏らさない)。<code>.acstheme</code> 人間可読テキストで永続化。<b>ImGui::CreateContext 後に呼ぶこと</b>。非コピー / 非ムーブ(global 副作用のため複製不可)、全 <code>noexcept</code>。",
  when: "エディタ起動時に配色 / DPI スケール / 角丸を統一したい時。",
  sample: "CEditorTheme theme;\ntheme.Init();                              // default Dark を適用\ntheme.ApplyPreset(EEditorThemePreset::DarkBlue);\ntheme.SetFontScale(1.25f);                 // 高 DPI\ntheme.SetRoundedCorners(4.0f);\ntheme.DrawThemeSettingsUI();",
  members: [
    { sig: "void Init() noexcept", ret: "—", desc: "default = Dark preset を <code>ImGui::GetStyle()</code> に流す。多重呼び出し可。<b><code>ImGui::CreateContext</code> 後に呼ぶこと</b>(未生成だと GetStyle / GetIO が落ちる)。" },
    { sig: "void ApplyPreset(EEditorThemePreset preset) noexcept", ret: "—", desc: "preset 既定の色 / spacing / corner を <code>m_Colors</code> に書き、即時 ImGui に流す。<code>Custom</code> は直前の Custom パレットを保持・再適用。" },
    { sig: "EEditorThemePreset CurrentPreset() const noexcept", ret: "preset", desc: "現在の preset 種別(<code>SetCustomColors</code> 後は自動で <code>Custom</code>)。" },
    { sig: "void SetCustomColors(const FEditorThemeColors&amp; colors) noexcept", ret: "—", desc: "任意パレットを設定し preset を <code>Custom</code> に切替・即時適用。各色は RGBA <code>[0,1]</code> 想定(clamp しない = HDR 許可)。" },
    { sig: "const FEditorThemeColors&amp; Colors() const noexcept", ret: "colors&amp;", desc: "現在のカラーパレットを const 参照で取得。" },
    { sig: "void SetFontScale(f32 scale) noexcept / f32 FontScale() const", ret: "—", desc: "global font scale(<code>ImGuiIO::FontGlobalScale</code>)の設定 / 取得。<code>&lt;= 0</code> は無視、4.0 超は警告して 4.0 に clamp。" },
    { sig: "void SetRoundedCorners(f32 radius) noexcept / f32 RoundedCorners() const", ret: "—", desc: "全 corner radius(Window / Frame / Popup / Grab / Tab / Scrollbar)を統一 / 取得。負値は 0 に clamp。" },
    { sig: "void SetSpacing(f32 item_spacing_y) noexcept / f32 Spacing() const", ret: "—", desc: "ItemSpacing.y(情報密度の主軸)の設定 / 取得。x はこの 0.5 倍に連動。負値は 0 に clamp。" },
    { sig: "void DrawThemeSettingsUI() noexcept", ret: "—", desc: "\"Theme Settings\" 独立 window を描画(preset combo + 全 ColorEdit4 + slider + Save/Load)。毎フレーム 1 行で呼ぶ。" },
    { sig: "void SaveTheme(const wchar_t* file_path) noexcept", ret: "—", desc: "現在の preset / colors / font_scale / corner / spacing を <code>.acstheme</code> テキストに書き出す。失敗時はログのみ。<code>nullptr</code> は no-op。" },
    { sig: "void LoadTheme(const wchar_t* file_path) noexcept", ret: "—", desc: "<code>.acstheme</code> から読み込み即時適用。失敗時(ファイル無 / magic mismatch / 解析失敗)は警告 + 現状維持。<code>nullptr</code> は no-op。" },
    { sig: "static constexpr const char* kMagic = \"ACS_THEME\"", desc: "<code>.acstheme</code> ファイルの magic 文字列。" },
    { sig: "static constexpr u32 kCurrentVersion = 1u", desc: "テーマファイルの version。" },
        { sig: "using FEditorTheme = CEditorTheme", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CEditorTheme</code> を使う。" }
      ]
},
{
  name: "AInspectorPanel",
  kind: "クラス(AEditorPanel 派生)", header: "gameframework/tools/inspector/InspectorPanel.h",
  summary: "選択中の ANode に紐づく <t>CInspectorSeam</t> 経由の <code>FInspectableField</code> 配列を、ImGui の field widget (Checkbox / InputInt / SliderFloat / DragFloatN / ColorEdit / InputText / Combo 等) として描画・編集する<b>プロパティエディタ</b>パネル。値の<b>永続化や undo は行わず</b>、変更を <t>FieldChangeCallback</t> で外部へ通知するだけ。<code>editor_core::AEditorPanel</code> を継承し、ヘッダから ImGui 依存は漏らさない。<b>ランタイム側の同名 CInspectorSeam とは別レイヤの UI 部品</b>。",
  when: "<code>AHierarchyPanel</code> 等で選択された ANode の各フィールドを GUI で編集させたい時。<code>SetInspectorSeam</code> で provider 解決元を、<code>SetSelectionService</code> で選択取得元を注入し、毎フレーム <code>DrawUI()</code> を呼ぶ。",
  sample: "AInspectorPanel insp;\ninsp.Init();\ninsp.SetInspectorSeam(&amp;seam);        // ANode → provider 解決元\ninsp.SetSelectionService(&amp;sel);       // 現在の選択を共有\n// 毎フレーム:\ninsp.DrawUI();\nif (insp.IsAnyFieldDirty()) {          // 何か編集された\n    // ... 永続化 / undo 記録 ...\n    insp.ClearDirtyFlag();\n}",
  members: [
    { sig: "using FieldChangeCallback = void (*)(void* user, FNodeId target, const char* field_name, EFieldKind kind) noexcept", desc: "field 変更通知 <t>コールバック</t>型。<code>user</code> は <code>SetOnFieldChangeCallback</code> 第二引数がそのまま戻る。<code>field_name</code> は Provider 所有のリテラル (<code>FInspectableField::name</code>)。", when: "undo / dirty tracker / 永続化を外部に委譲する時。" },
    { sig: "void Init() noexcept", ret: "—", desc: "内部状態 (dirty / selection / callback) を初期値に戻す。多重 <code>Init</code> 可。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "dirty / callback / selection を解除する。多重 <code>Shutdown</code> 可。" },
    { sig: "void SetInspectorSeam(CInspectorSeam* seam) noexcept", ret: "—", desc: "ANode → Provider 解決に使う <code>CInspectorSeam</code> を事前 set する。<code>DrawUI</code> 内で <code>seam.GetProvider(node_id)</code> を呼ぶ。" },
    { sig: "CInspectorSeam* InspectorSeamPtr() const noexcept", ret: "seam ポインタ", desc: "現在設定中の <code>CInspectorSeam</code> を返す。未設定なら nullptr。" },
    { sig: "const char* Title() const noexcept override", ret: "\"Inspector\"", desc: "ImGui window タイトル。" },
    { sig: "void DrawUI() noexcept override", ret: "—", desc: "<code>Begin(\"Inspector\")</code> で 1 window を描画。選択は <code>CSelectionService</code> 設定時はその <code>CurrentSelection()</code> を採用。Provider が取れなければ \"(No provider)\"、選択無しなら \"(Nothing selected)\" を表示。", when: "毎フレーム。" },
    { sig: "void SetSelectionService(CSelectionService* svc) noexcept", ret: "—", desc: "選択取得元の <code>CSelectionService</code> を注入する。nullptr で解除。保持期間は呼び出し側責務 (non-owning)。" },
    { sig: "bool IsAnyFieldDirty() const noexcept", ret: "編集有無", desc: "直近の <code>DrawUI</code> 内でいずれかの field が編集されたら true。" },
    { sig: "void ClearDirtyFlag() noexcept", ret: "—", desc: "dirty フラグをクリアする。外部の永続化 / undo 処理完了後に呼ぶ。" },
    { sig: "void SetOnFieldChangeCallback(FieldChangeCallback cb, void* user) noexcept", ret: "—", desc: "field 変更通知 callback を登録する。nullptr で解除。" },
    { sig: "static constexpr f32 kDefaultSliderMin = -1000.0f", desc: "F32 を <code>SliderFloat</code> で編集する際の既定下限。<code>FInspectableField</code> に min/max metadata が無い間の暫定値。" },
    { sig: "static constexpr f32 kDefaultSliderMax = 1000.0f", desc: "同上、既定上限。" },
    { sig: "static constexpr u32 kStringBufferSize = 256u", desc: "FString 編集用スタックバッファ長 (= <code>InputText</code> 制限)。" },
        { sig: "using FInspectorPanel = AInspectorPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AInspectorPanel</code> を使う。" }
      ]
},
{
  name: "CSelectionService",
  kind: "クラス", header: "gameframework/tools/inspector/SelectionService.h",
  summary: "今「選択されている <code>FNodeId</code>」を全 editor panel (<code>AHierarchyPanel</code> / <code>AInspectorPanel</code> / <code>AEditorToolbar</code> / SceneView 等) で共有するための<b>選択ハブ</b>。単一の選択だけを持ち、変化時に登録済み <t>コールバック</t>を <code>(from, to)</code> 付きで一斉発火する。<code>CGame</code> / <code>CSceneManager</code> には依存せず、対象 ANode が生きているかの検証は購読側責務。STL 不使用 (内部は <code>TArray</code>)、非コピー・非ムーブ。",
  when: "editor 起動コードが 1 個だけ所有し、各 panel が選択を読み書き・購読する集中点として使う。クリックで <code>SelectNode</code>、ハイライト判定に <code>IsSelected</code>、選択追従に <code>RegisterCallback</code>。",
  sample: "CSelectionService sel;\nsel.Init();\nsel.RegisterCallback(&amp;OnSelChanged, &amp;hierarchy);\nif (ImGui::Selectable(label, sel.IsSelected(node_id)))\n    sel.SelectNode(node_id);   // 登録済み callback が一斉に呼ばれる",
  members: [
    { sig: "using SelectionChangeCallback = void (*)(void* user, FNodeId from, FNodeId to) noexcept", desc: "選択変更 <t>コールバック</t>型。<code>from</code>=変更前 (未選択時 invalid)、<code>to</code>=変更後 (<code>ClearSelection</code> 時 invalid)、<code>user</code>=登録時のコンテキスト。同じ選択を再 <code>SelectNode</code> しても呼ばれない (no-op)。" },
    { sig: "void Init() noexcept", ret: "—", desc: "callback list と現選択を空に戻す。多重呼び出し可。" },
    { sig: "void SelectNode(FNodeId id) noexcept", ret: "—", desc: "現選択を <code>id</code> に切り替える。値が変化した場合のみ callback を一斉発火。invalid handle を渡すと <code>ClearSelection</code> と等価。" },
    { sig: "void ClearSelection() noexcept", ret: "—", desc: "現選択を invalid に戻す。前選択が valid だった場合のみ callback 発火。" },
    { sig: "FNodeId CurrentSelection() const noexcept", ret: "現選択", desc: "現在の選択 <code>FNodeId</code> を返す。未選択時は invalid handle (<code>IsValid() == false</code>)。" },
    { sig: "bool IsSelected(FNodeId id) const noexcept", ret: "一致可否", desc: "<code>id</code> が現選択と完全一致 (index + generation) なら true。invalid id を渡した場合は現選択が invalid のとき true。", when: "<code>ImGui::Selectable</code> のハイライト判定に。" },
    { sig: "void RegisterCallback(SelectionChangeCallback cb, void* user) noexcept", ret: "—", desc: "<code>(cb, user)</code> ペアを登録する。同一ペアの重複登録と <code>cb == null</code> は no-op で弾く。dispatch 順は登録順。" },
    { sig: "void UnregisterCallback(SelectionChangeCallback cb, void* user) noexcept", ret: "—", desc: "<code>(cb, user)</code> 完全一致 1 件を解除する。該当なしは no-op。" },
    { sig: "u32 CallbackCount() const noexcept", ret: "登録数", desc: "登録済みコールバック数を返す。" },
    { sig: "void ClearAll() noexcept", ret: "—", desc: "全 callback 登録を破棄し選択も invalid に戻す。<b><code>ClearSelection</code> 相当の通知は行わない</b> (shutdown 時の一括破棄想定)。" },
        { sig: "using FSelectionService = CSelectionService", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSelectionService</code> を使う。" }
      ]
},
{
  name: "AHierarchyPanel",
  kind: "クラス(AEditorPanel 派生)", header: "gameframework/tools/inspector/HierarchyPanel.h",
  summary: "<code>ANode</code> ツリー (シーン) を ImGui の hierarchy パネルで可視化・編集する、Unity の Hierarchy / UE の World Outliner 相当の panel。root 配下を再帰 <code>TreeNode</code> で描画し、クリックで選択 (<code>CSelectionService</code> 経由 or 内部保持)、右クリックで Delete / Duplicate / Reparent、Drag &amp; Drop で reparent、ExpandAll / CollapseAll で一括開閉する。<code>editor_core::AEditorPanel</code> を継承。STL 不使用 (折りたたみ状態は <code>TArray</code> の linear search)。",
  when: "シーンの ANode 階層を GUI で見せ・選択・削除・親子付けさせたい時。<code>SetRootNode</code> で起点 ANode を、必要なら <code>SetSelectionService</code> で選択を他 panel と共有し、毎フレーム <code>DrawUI()</code> を呼ぶ。",
  sample: "AHierarchyPanel hier;\nhier.Init();\nhier.SetRootNode(&amp;scene_root);\nhier.SetSelectionService(&amp;sel);   // Inspector と選択を共有\n// 毎フレーム:\nhier.DrawUI();",
  members: [
    { sig: "using NodeRightClickCallback = void (*)(void* user, ANode* node) noexcept", desc: "右クリック context menu の<b>追加</b>項目を外部委譲する <t>コールバック</t>型。標準の Delete / Duplicate / Reparent はパネル内描画で、これは \"Create Child\" 等の追加項目のみ担当。<code>user</code> は登録時の第二引数。" },
    { sig: "void Init() noexcept", ret: "—", desc: "折りたたみマップを空に、selection を解除する。多重 <code>Init</code> 可。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "折りたたみマップを解放し callback を解除する。多重 <code>Shutdown</code> 可。" },
    { sig: "void SetRootNode(ANode* root) noexcept", ret: "—", desc: "再帰描画の起点となる root ANode を事前 set する。root 自体も最上位 <code>TreeNode</code> として表示される。" },
    { sig: "ANode* RootNode() const noexcept", ret: "root ポインタ", desc: "現在設定中の root ANode を返す。" },
    { sig: "const char* Title() const noexcept override", ret: "\"Scene Hierarchy\"", desc: "ImGui window タイトル。" },
    { sig: "void DrawUI() noexcept override", ret: "—", desc: "<code>Begin(\"Scene Hierarchy\")</code> 1 window で root 配下を再帰 <code>TreeNode</code> 描画する。", when: "毎フレーム。" },
    { sig: "void SetSelectionService(CSelectionService* svc) noexcept", ret: "—", desc: "選択ハブを注入する。nullptr で内部 selection モードに戻る。" },
    { sig: "FNodeId SelectedNodeId() const noexcept", ret: "選択 id", desc: "現選択 ANode の <code>FNodeId</code>。<code>CSelectionService</code> 注入時はそちら経由、なければ内部値。未選択は <code>FNodeId{}</code> (packed==0)。" },
    { sig: "void SelectNode(ANode* node) noexcept", ret: "—", desc: "選択を <code>node</code> に切り替える。nullptr で選択解除。<code>CSelectionService</code> 注入時はそちらにも反映する。" },
    { sig: "void ExpandAll() noexcept", ret: "—", desc: "全 <code>TreeNode</code> を展開状態にする (= 折りたたみマップを空にする)。" },
    { sig: "void CollapseAll() noexcept", ret: "—", desc: "全 <code>TreeNode</code> を折りたたむ。実適用は次回 <code>DrawUI</code> で行う (内部 pending フラグを立てるのみ)。" },
    { sig: "void DeleteSelected() noexcept", ret: "—", desc: "選択中 ANode に <code>ANode::Destroy()</code> を呼ぶ (実 destroy は次フレームの structural change 解決で)。選択は解除される。未選択時は no-op。" },
    { sig: "void SetOnNodeRightClickCallback(NodeRightClickCallback cb, void* user) noexcept", ret: "—", desc: "右クリック追加項目 callback を登録する。nullptr で解除可。" },
        { sig: "using FHierarchyPanel = AHierarchyPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AHierarchyPanel</code> を使う。" }
      ]
},
{
  name: "EEditorState",
  kind: "列挙(enum class : u8)", header: "gameframework/tools/inspector/EditorToolbar.h",
  summary: "<code>AEditorToolbar</code> が保持する editor の<b>再生状態</b>。Playing / Paused / Stepping の 3 値で、<code>CGame</code> の TimeScale 反映 (Play=通常値 / Pause=0) と 1 step 進行を表す。",
  when: "ツールバーの再生制御状態を読み取りたい時 (<code>AEditorToolbar::State()</code> の戻り値)。",
  members: [
    { sig: "Playing = 0", desc: "通常実行中 (TimeScale = <code>m_NormalTimeScale</code>)。" },
    { sig: "Paused = 1", desc: "一時停止中 (TimeScale = 0)。" },
    { sig: "Stepping = 2", desc: "1 fixed step だけ進める一時状態。本フレームで Playing 相当の dt を 1 回処理した後 Paused に復帰する。" }
  ]
},
{
  name: "AEditorToolbar",
  kind: "クラス(AEditorPanel 派生)", header: "gameframework/tools/inspector/EditorToolbar.h",
  summary: "editor ウィンドウ上端の<b>ツールバー</b>。Play / Pause / Step の再生制御、Scene 保存、CDebugOverlay 表示切替の 5 アクションを 1 行に並べる。再生状態を <t>EEditorState</t> 1 個で保持し、<code>CGame::SetTimeScale</code> への反映を本クラスが <code>DrawUI</code> 内で行う。Save は外部委譲 (<t>SaveSceneCallback</t> 発火のみ)、CDebugOverlay は bool flag のみ持ち実描画は外側責務。<code>editor_core::AEditorPanel</code> を継承。",
  when: "editor の中央バーから再生制御・保存・デバッグ表示を出したい時。<code>SetGame</code> で対象 <code>CGame</code> を、<code>SetOnSaveSceneCallback</code> で保存処理を注入し、毎フレーム <code>DrawUI()</code> を呼ぶ。",
  sample: "AEditorToolbar toolbar;\ntoolbar.Init();\ntoolbar.SetGame(&amp;game);\ntoolbar.SetOnSaveSceneCallback(&amp;SaveSceneToDisk, &amp;my_editor);\n// 毎フレーム:\ntoolbar.DrawUI();   // 内部で CGame::SetTimeScale(0/1) を切り替える",
  members: [
    { sig: "using SaveSceneCallback = void (*)(void* user) noexcept", desc: "Save ボタンクリック時に発火する <t>コールバック</t>型。serialize 先 (file path / format) は <code>user</code> 側で決定する。" },
    { sig: "void Init() noexcept", ret: "—", desc: "state を <code>Playing</code> に戻す (save callback はクリアしない)。多重呼び出し可 (state リセットとして使える)。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "callback / 内部参照を解除する。多重呼び出し可。" },
    { sig: "void SetGame(CGame* game) noexcept", ret: "—", desc: "TimeScale 反映先の <code>CGame</code> を事前 set する。nullptr のときは UI 更新のみで反映を skip。" },
    { sig: "CGame* GameRef() const noexcept", ret: "game ポインタ", desc: "現在設定中の <code>CGame</code> を返す。" },
    { sig: "const char* Title() const noexcept override", ret: "\"Editor Toolbar\"", desc: "ImGui window タイトル。" },
    { sig: "void DrawUI() noexcept override", ret: "—", desc: "Play / Pause / Step / Save / CDebugOverlay ボタン行を描画する。<code>m_Game</code> が nullptr なら TimeScale 反映を skip (UI のみ動作)。", when: "毎フレーム。" },
    { sig: "void TogglePlayPause() noexcept", ret: "—", desc: "Play ↔ Pause を切り替える。Playing→Paused で現 TimeScale を記録して 0 に、Paused→Playing で記録値を書き戻す。<code>CGame</code> への実反映は <code>DrawUI</code> 内。" },
    { sig: "void Step() noexcept", ret: "—", desc: "1 fixed step 進める要求。state を <code>Stepping</code> に遷移するのみで、実進行と Paused 復帰は <code>DrawUI</code> 末尾で行う。Stepping 中の再 <code>Step</code> は no-op。" },
    { sig: "EEditorState State() const noexcept", ret: "現状態", desc: "現在の editor 再生状態 (<code>EEditorState</code>) を返す。" },
    { sig: "void SetOnSaveSceneCallback(SaveSceneCallback cb, void* user) noexcept", ret: "—", desc: "Save callback を登録する。null 渡しで解除可能。" },
    { sig: "void SetShowDebugOverlay(bool b) noexcept", ret: "—", desc: "CDebugOverlay の表示要望 flag を設定する。実描画は外側コードが flag を見て切り替える。" },
    { sig: "bool ShowDebugOverlay() const noexcept", ret: "表示要望", desc: "CDebugOverlay 表示要望 flag を返す。" },
        { sig: "using FEditorToolbar = AEditorToolbar", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AEditorToolbar</code> を使う。" }
      ]
},
{
  name: "CFxeditSerializer",
  kind: "クラス(static ユーティリティ)", header: "gameframework/tools/fxedit/FxeditSerializer.h",
  summary: "ParticleEditor が編集中の <code>FParticleEmitterDef</code> 群を、人間可読で <t>git diff</t> しやすい <code>.fxedit</code> テキスト形式で保存・復元する <t>シリアライザ</t>。バイナリの <code>TSaveSlot</code> と違い、<b>作業中アセットをそのまま版管理に乗せる</b>ことを優先する。全メンバ <code>static</code>、生成・コピー・ムーブはすべて <code>= delete</code>。",
  when: "emitter 定義をテキストでセーブ/ロードしたい時。1 行 1 key=value なので 1 パラメータ変更が 1 行 diff になる。ファイル I/O は <code>acs::CFileSystem</code> 経由。",
  sample: "acs::game::FParticleEmitterDef defs[2] = {};\ndefs[0].lifetime_sec = 2.0f;\nconst char* names[] = {\"fire\", \"smoke\"};\nnamespace fx = acs::game::fxedit;\nfx::CFxeditSerializer::Save(L\"data/fx.fxedit\", defs, names, 2);\n\nacs::game::FParticleEmitterDef loaded[16] = {};\nchar name_buf[16 * 32] = {};\nauto r = fx::CFxeditSerializer::Load(L\"data/fx.fxedit\", loaded, name_buf, sizeof(name_buf), 16);\nif (r.IsOk()) { acs::u32 n = r.Value(); (void)n; }",
  members: [
    { sig: "static TResult&lt;void, FErrorCode&gt; Save(const wchar_t* file_path, const FParticleEmitterDef* defs, const char* const* names, u32 count) noexcept", ret: "成否", desc: "emitter 群を <code>.fxedit</code> テキストへ書き出す。既存ファイルは上書き。<code>names</code> ポインタ自体が nullptr なら <code>kSub_NullArgs</code>。count=0 はヘッダだけ書く。", when: "セーブ時。" },
    { sig: "static TResult&lt;u32, FErrorCode&gt; Load(const wchar_t* file_path, FParticleEmitterDef* out_defs, char* out_name_buffer, u32 name_buffer_capacity, u32 max_emitters) noexcept", ret: "ロードした emitter 数", desc: "<code>.fxedit</code> から emitter 群を読む。名前は <code>name0\\0name1\\0...</code> 形式で <code>out_name_buffer</code> に連結配置。成功時の値は実際にロードした個数 (&lt;= max_emitters)。", when: "ロード時。" },
    { sig: "static u32 ParseHeaderVersion(const char* text, u32 text_len) noexcept", ret: "version", desc: "先頭行 <code>\"ACS_FXEDIT &lt;v&gt;\"</code> を検査して v を返す。0 は magic 不一致 / version 解析失敗。<code>text</code> は NUL 終端不要 (長さを別途渡す)。", when: "低レベル parse 自前実装時。" },
    { sig: "static bool ParseLine(const char* line, const char*& out_key, f32& out_v0, f32& out_v1, f32& out_v2, f32& out_v3) noexcept", ret: "成否", desc: "1 行 <code>\"&lt;key&gt; &lt;v0&gt; [&lt;v1&gt;..]\"</code> を解釈。<code>out_key</code> は line 内の key 開始ポインタ (最初の空白までが key)。出現しない値スロットには 0.0 が入る。完全な空行なら false。", when: "低レベル parse。" },
    { sig: "static const char* SkipWhitespace(const char* p) noexcept", ret: "ポインタ", desc: "<code>' '</code> / <code>'\\t'</code> / <code>'\\r'</code> / <code>'\\n'</code> を読み飛ばす。末端 NUL は跨がない。", when: "低レベル parse。" },
    { sig: "static constexpr const char* kMagic = \"ACS_FXEDIT\"", desc: "ファイル先頭の magic 文字列。" },
    { sig: "static constexpr u32 kCurrentVersion = 1u", desc: "現在のフォーマットバージョン。" },
    { sig: "static constexpr usize kMaxBytesPerEmitter = 2048", desc: "emitter 1 個分のテキスト出力の最大バイト見積。" },
    { sig: "static constexpr usize kMaxLineLength = 512", desc: "1 行 parse 時に許容する最大長 (超過分は切り捨て)。" },
    { sig: "static constexpr usize kMaxEmitterName = 31", desc: "1 emitter 名の最大バイト数 (NUL 終端含まず)。" },
    { sig: "enum ESubCode : u16 { ... }", desc: "<code>FErrorCode</code> のサブコード (700〜707)。<code>kSub_NullArgs=700</code> / <code>kSub_TooManyEmitters=701</code> / <code>kSub_BufferOverflow=702</code> / <code>kSub_BadMagic=703</code> / <code>kSub_BadVersion=704</code> / <code>kSub_BadFormat=705</code> / <code>kSub_FileNotFound=706</code> / <code>kSub_IOFailure=707</code>。" },
        { sig: "using FFxeditSerializer = CFxeditSerializer", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CFxeditSerializer</code> を使う。" }
      ]
},
{
  name: "AParticleEditorPanel",
  kind: "クラス(AEditorPanel 派生)", header: "gameframework/tools/fxedit/ParticleEditorPanel.h",
  summary: "<code>FParticleEmitterDef</code> を ImGui で実機編集するための emitter プロパティエディタ <t>パネル</t>。<code>editor_core::AEditorPanel</code> を継承し、内部に <code>TArray&lt;FParticleEmitterDef&gt;</code> を持つ。emitter handle や pool への反映 (CreateEmitter 等) は呼び出し側の責務で、本 panel は <b>def の編集のみ</b>を担う。Save / Load は callback hook に委譲。非コピー・非ムーブ・全 noexcept。",
  when: "Editor / DevTool ビルドで emitter パラメータをライブ編集したい時。<code>CEditorWorkspace::RegisterPanel(&amp;panel)</code> で登録する。",
  sample: "namespace fx = acs::game::fxedit;\nfx::AParticleEditorPanel panel;\npanel.Init();\npanel.AddEmitter();\nif (auto* def = panel.GetEmitterDefMutable(0)) {\n    def-&gt;lifetime_sec = 1.5f;\n}\nif (panel.IsDirty()) {\n    // ... 保存 ...\n    panel.ClearDirty();\n}\npanel.Shutdown();",
  members: [
    { sig: "using SaveCallback = void (*)(void* user, const FParticleEmitterDef* defs, u32 count) noexcept", desc: "Save ボタン押下時に呼ばれる関数ポインタ型。<code>user</code> は登録時に渡したポインタがそのまま戻る。" },
    { sig: "using LoadCallback = void (*)(void* user, FParticleEmitterDef* defs, u32& inout_count) noexcept", desc: "Load ボタン押下時に呼ばれる関数ポインタ型。" },
    { sig: "void Init() noexcept", ret: "—", desc: "emitter list を空に戻し selection を解除。多重 Init 可 (完全リセット)。", when: "起動時。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "emitter list を解放。冪等で多重 Shutdown 可。", when: "終了時。" },
    { sig: "void SetTargetSystem(CParticleEffectSystem* system) noexcept", ret: "—", desc: "active particle 数表示用の read-only target system を set する。", when: "DrawUI 前。" },
    { sig: "CParticleEffectSystem* TargetSystem() const noexcept", ret: "system or nullptr", desc: "現在 set されている target system を返す。" },
    { sig: "const char* Title() const noexcept override", ret: "\"Particle Editor\"", desc: "AEditorPanel override: window タイトル。" },
    { sig: "void DrawUI() noexcept override", ret: "—", desc: "AEditorPanel override: <code>Begin(\"Particle Editor\")</code> から始まる単一 window レイアウトを描画。" },
    { sig: "void AddEmitter() noexcept", ret: "—", desc: "default param の emitter を末尾に追加し selection を移す。<code>kMaxEmitters</code> 到達なら no-op。", when: "追加時。" },
    { sig: "void RemoveSelectedEmitter() noexcept", ret: "—", desc: "選択中 emitter を削除、selection を前 index に詰める。未選択 / 範囲外は no-op。" },
    { sig: "void DuplicateSelectedEmitter() noexcept", ret: "—", desc: "選択中 emitter を複製して直後に挿入、selection を複製先へ。未選択 / 上限到達は no-op。" },
    { sig: "i32 SelectedIndex() const noexcept", ret: "index or -1", desc: "現在の選択 index。未選択は -1。" },
    { sig: "void SelectEmitter(i32 index) noexcept", ret: "—", desc: "選択 index を設定。範囲外 (&gt;= EmitterCount()) や負値は -1 に正規化。" },
    { sig: "u32 EmitterCount() const noexcept", ret: "個数", desc: "現在の emitter 数。" },
    { sig: "const FParticleEmitterDef* GetEmitterDef(i32 index) const noexcept", ret: "def or nullptr", desc: "index 番目の emitter def (read-only)。範囲外は nullptr。" },
    { sig: "FParticleEmitterDef* GetEmitterDefMutable(i32 index) noexcept", ret: "def or nullptr", desc: "index 番目の emitter def (mutable)。範囲外は nullptr。書き換え後は dirty 想定。" },
    { sig: "void SetSaveCallback(SaveCallback cb, void* user) noexcept", ret: "—", desc: "Save callback を登録。nullptr で解除。" },
    { sig: "void SetLoadCallback(LoadCallback cb, void* user) noexcept", ret: "—", desc: "Load callback を登録。nullptr で解除。" },
    { sig: "bool IsDirty() const noexcept", ret: "bool", desc: "UI 編集 / Add / Remove / Dup されたか。外部の Save タイミング判定用。" },
    { sig: "void ClearDirty() noexcept", ret: "—", desc: "dirty フラグをクリア。Save 完了後に呼ぶ。" },
    { sig: "static constexpr u32 kMaxEmitters = 64u", desc: "emitter list の上限。超えると AddEmitter / Duplicate は no-op。" },
        { sig: "using FParticleEditorPanel = AParticleEditorPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AParticleEditorPanel</code> を使う。" }
      ]
},
{
  name: "CParticleEditorPreview",
  kind: "クラス", header: "gameframework/tools/fxedit/ParticleEditorPreview.h",
  summary: "編集中の <code>FParticleEmitterDef</code> を 1 個だけ instance 化して、リアルタイムに visual feedback を返す <b>preview canvas + stats</b>。外部の <code>CParticleEffectSystem</code> 上に 1 emitter を立てる方式で system は所有しない。frame budget は 60-frame の <t>リングバッファ</t>で保持。非コピー・非ムーブ・全 noexcept。",
  when: "ParticleEditor で emitter を編集しながら見た目を即時確認したい時。値変更時に <code>RecreatePreviewEmitter</code> で再生成すると即反映される。",
  sample: "namespace fx = acs::game::fxedit;\nfx::CParticleEditorPreview preview;\npreview.Init();\nacs::game::CParticleEffectSystem system;  // 別途 Init 済とする\nacs::game::FParticleEmitterDef def {};\npreview.RecreatePreviewEmitter(system, &amp;def);\npreview.Tick(0.016f, system);\npreview.DrawUI(system, &amp;def);\npreview.StopAll(system);\npreview.Shutdown();",
  members: [
    { sig: "void Init() noexcept", ret: "—", desc: "frame budget ring (60 frame) を事前確保。再 Init は履歴をクリアし内部 handle を無効化。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "履歴 / handle をクリア。<b>引数で system を取らないため DestroyEmitter は行わない</b> (handle を invalid にするだけ)。明示停止は先に <code>StopAll</code>。" },
    { sig: "void Tick(f32 dt, CParticleEffectSystem& system) noexcept", ret: "—", desc: "1 フレーム進める。<code>dt &lt;= 0</code> は ring に push せず無視。system 側の Tick は呼ばない (呼出側責務)。", when: "毎フレーム。" },
    { sig: "void DrawUI(CParticleEffectSystem& system, const FParticleEmitterDef* def) noexcept", ret: "—", desc: "<code>\"Particle Preview\"</code> window を描画。<code>def == nullptr</code> なら \"no def selected\" を表示。" },
    { sig: "void RecreatePreviewEmitter(CParticleEffectSystem& system, const FParticleEmitterDef* def) noexcept", ret: "—", desc: "編集中 emitter を即時再生成 (既存 handle が valid なら Destroy 後、新 def の copy で Create)。<code>def == nullptr</code> は no-op。", when: "def 変更時。" },
    { sig: "void TriggerBurst(CParticleEffectSystem& system) noexcept", ret: "—", desc: "<code>burst_count</code> に従い 1 回 Burst (handle が valid のときのみ)。" },
    { sig: "void StopAll(CParticleEffectSystem& system) noexcept", ret: "—", desc: "preview emitter を破棄 + particle pool 全消去。編集セッションをクリーンに戻す。" },
    { sig: "FVec2 SpawnPos() const noexcept", ret: "FVec2", desc: "preview canvas 上の出生座標。既定は中央 (320, 240)。" },
    { sig: "void SetSpawnPos(FVec2 pos) noexcept", ret: "—", desc: "出生座標を設定。" },
    { sig: "u32 ActiveParticleCount() const noexcept", ret: "個数", desc: "直近 Tick で記録した active particle 数。" },
    { sig: "u32 MaxParticleCount() const noexcept", ret: "容量", desc: "pool 容量 (直近 Tick で更新)。" },
    { sig: "bool IsAutoEmit() const noexcept", ret: "bool", desc: "連続放出モードか (true なら emitter active、false なら手動 Burst のみ)。" },
    { sig: "void SetAutoEmit(bool b) noexcept", ret: "—", desc: "連続放出モードを切替。" },
    { sig: "f32 GraphFps() const noexcept", ret: "fps", desc: "直近 60 frame の平均 fps。履歴が空なら 0。" },
        { sig: "using FParticleEditorPreview = CParticleEditorPreview", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CParticleEditorPreview</code> を使う。" }
      ]
},
{
  name: "EBrushKind",
  kind: "列挙(enum class : u8)", header: "gameframework/tools/leveledit/LevelEditorPanel.h",
  summary: "<code>ALevelEditorPanel</code> のペイントブラシ種別。<t>tilemap</t> 上のマウス操作の意味を切り替える。E-prefix は ACS 規約。",
  when: "LevelEditor のツールバーで現在ブラシを選ぶ時。<code>SetCurrentBrush</code> / <code>CurrentBrush</code> で読み書き。",
  members: [
    { sig: "Paint = 0", desc: "塗り。<code>current_tile_id</code> を SetTile (drag 対応)。" },
    { sig: "Erase = 1", desc: "消し。<code>FTileId{0}</code> を SetTile (drag 対応)。" },
    { sig: "Fill = 2", desc: "flood-fill。クリック位置の連結成分を <code>current_tile_id</code> で塗替 (click 単発)。" },
    { sig: "Pick = 3", desc: "スポイト。クリック位置の tile id を <code>current_tile_id</code> にコピー (click 単発)。" }
  ]
},
{
  name: "ALevelEditorPanel",
  kind: "クラス(AEditorPanel 派生)", header: "gameframework/tools/leveledit/LevelEditorPanel.h",
  summary: "<code>FTilemap</code> (multi-layer u16 FTileId grid) を対話的に編集する <b>tilemap painter パネル</b>。Paint / Erase / Fill / Pick の 4 ブラシ + アクティブレイヤ切替 + tile id picker + grid 表示 + snap-to-grid を提供。2D 用 <code>CEditorCamera</code> を内包し pan / zoom できる。tilemap は <b>非所有 (caller 所有)</b>。非コピー・非ムーブ・全 noexcept。",
  when: "tilemap を ImGui 上でペイント編集したい時。<code>SetTilemap(&amp;map)</code> で編集対象を渡し、workspace に登録する。",
  sample: "namespace lv = acs::game::leveledit;\nlv::ALevelEditorPanel panel;\npanel.Init();\nworkspace.RegisterPanel(&amp;panel);\n\nacs::game::FTilemap map;  // 別途 Init 済とする\npanel.SetTilemap(&amp;map);\npanel.SetCurrentBrush(lv::EBrushKind::Paint);\npanel.SetCurrentTileId(1);\n// ... TickAllPanels(dt) 内で DrawUI ...\nworkspace.UnregisterPanel(&amp;panel);\npanel.Shutdown();",
  members: [
    { sig: "void Init() noexcept", ret: "—", desc: "内部 state をデフォルトに (brush=Paint / tile_id=1 / active_layer=0 / show_grid=true / snap=true)、CEditorCamera を 2D mode で Init。多重 Init 可。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "tilemap 参照を解除し CEditorCamera を Reset。tilemap 自体は破棄しない。多重 Shutdown 可。" },
    { sig: "void SetTilemap(FTilemap* tm) noexcept", ret: "—", desc: "編集対象 FTilemap を raw 参照でセット (nullptr で解除)。寿命は caller 責任。active_layer を 0 にクランプ、selected coord をリセット。", when: "編集対象差し替え時。" },
    { sig: "FTilemap* CurrentTilemap() const noexcept", ret: "tilemap or nullptr", desc: "現在編集対象の FTilemap (未バインドは nullptr)。" },
    { sig: "editor_core::CEditorCamera& Camera() noexcept", ret: "camera 参照", desc: "内部 2D camera への参照。呼出側が <code>HandleMouseInput</code> / <code>Tick</code> を呼ぶ。" },
    { sig: "EBrushKind CurrentBrush() const noexcept / void SetCurrentBrush(EBrushKind b) noexcept", ret: "brush", desc: "現在ブラシの取得 / 設定。" },
    { sig: "u16 CurrentTileId() const noexcept / void SetCurrentTileId(u16 id) noexcept", ret: "tile id", desc: "ペイント中 tile id。setter は <code>kTileIdMax</code> (1023) でクランプ。" },
    { sig: "u32 ActiveLayer() const noexcept / void SetActiveLayer(u32 layer) noexcept", ret: "layer", desc: "アクティブレイヤ番号。バインド後 <code>FTilemap::LayerCount()</code> でクランプ。" },
    { sig: "bool ShowGrid() const noexcept / void SetShowGrid(bool b) noexcept", ret: "bool", desc: "grid line 表示 toggle。" },
    { sig: "bool SnapToGrid() const noexcept / void SetSnapToGrid(bool b) noexcept", ret: "bool", desc: "snap-to-grid 表示 toggle (ホバー強調用、ペイント自体は常に tile 単位)。" },
    { sig: "const char* Title() const noexcept override", ret: "\"Level Editor\"", desc: "AEditorPanel override: window タイトル。" },
    { sig: "void OnInit(editor_core::CEditorWorkspace& workspace) noexcept override", ret: "—", desc: "AEditorPanel override: workspace 登録時。基底実装 + CEditorCamera を 2D mode で Init し直す保険。" },
    { sig: "void DrawUI() noexcept override", ret: "—", desc: "AEditorPanel override: Toolbar + Canvas + Inspector を描画。tilemap 未バインドなら \"(No tilemap bound)\"。" },
    { sig: "static constexpr u16 kTileIdMax = 1023u", desc: "tile id picker の上限。<code>SetCurrentTileId</code> のクランプ値。" },
    { sig: "static constexpr u32 kFloodFillMaxCells = 4096u", desc: "flood-fill の最大処理セル数 (暴走防止の打ち切り)。" },
    { sig: "static constexpr u32 kNoCoord = 0xFFFFFFFFu", desc: "\"未選択\" を表す sentinel coord。" },
        { sig: "using FLevelEditorPanel = ALevelEditorPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ALevelEditorPanel</code> を使う。" }
      ]
},
{
  name: "EBtKind",
  kind: "列挙(enum class : u8)", header: "gameframework/tools/btedit/BehaviorTreeEditorPanel.h",
  summary: "<code>ABehaviorTreeEditorPanel</code> のメタミラー上の node 種別。実体 BT は RTTI 無効で種別判定できないため、editor 側で明示保持する。<code>ABtSelector→Selector</code> / <code>ABtSequence→Sequence</code> / <code>ABtAction→Action</code> に 1:1 対応。",
  when: "<code>AddNode</code> で node の種別を指定する時、および tree view の色分け / アイコン判別。",
  members: [
    { sig: "Selector = 0", desc: "Selector node (ABtSelector に対応)。" },
    { sig: "Sequence = 1", desc: "Sequence node (ABtSequence に対応)。" },
    { sig: "Action = 2", desc: "Action node (ABtAction に対応)。" }
  ]
},
{
  name: "ABehaviorTreeEditorPanel",
  kind: "クラス(AEditorPanel 派生)", header: "gameframework/tools/btedit/BehaviorTreeEditorPanel.h",
  summary: "<code>CBehaviorTree</code> を <b>可視化 + step debug</b> する ImGui パネル (Unreal の Behavior Tree Debugger 相当)。実体 BT は private で覗けないため、ユーザが <code>AddNode</code> で「親 id / kind / 表示名」を push する <t>メタミラー</t>方式を採る。node 状態は <code>SetNodeStatus</code> で push。root status の履歴を 60-frame ring に保持。非コピー・非ムーブ・全 noexcept。",
  when: "実行中の BT を観察 / step 実行したい時。BT 構築直後にミラーも組み立て、tick のたびに status を push する。",
  sample: "namespace bt = acs::game::btedit;\nbt::ABehaviorTreeEditorPanel panel;\npanel.Init();\nacs::game::CBehaviorTree tree;  // 別途構築済とする\npanel.SetTree(&amp;tree);\nacs::u32 root = panel.AddNode(bt::EBtKind::Selector, \"Root\", bt::ABehaviorTreeEditorPanel::kInvalidId);\nacs::u32 act  = panel.AddNode(bt::EBtKind::Action, \"Move\", root);\npanel.SetNodeStatus(act, acs::game::EBtStatus::Running);\npanel.StepOnce();\npanel.Shutdown();",
  members: [
    { sig: "using StepCallback = void(*)(void* user, CBehaviorTree* tree, f32 dt) noexcept", desc: "「1 tick 進めたい」時に呼ばれる関数ポインタ型。登録時は panel は <code>tree-&gt;Tick</code> を直接呼ばず callback だけ呼ぶ (blackboard を渡す自由を与える)。" },
    { sig: "static constexpr u32 kHistorySize = 60u", desc: "履歴 ring buffer の長さ (frame 数)。" },
    { sig: "static constexpr u32 kInvalidId = 0xFFFFFFFFu", desc: "SelectedNodeId / parent_id の \"none\" シグネル。root の parent_id にも使う。" },
    { sig: "static constexpr u32 kMaxNodes = 128u", desc: "同時登録可能 node 数の上限。到達時 AddNode は kInvalidId を返す。" },
    { sig: "static constexpr f32 kStepDt = 0.016f", desc: "StepOnce で使う固定 dt (60 fps の 1 frame)。" },
    { sig: "void Init() noexcept", ret: "—", desc: "メタミラー / 履歴を空に、autorun / step counter / selection を default に。callback はリセットしない。多重 Init 可。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "メタミラー / 履歴 / callback を全クリア。多重 Shutdown 可。" },
    { sig: "void SetTree(CBehaviorTree* tree) noexcept", ret: "—", desc: "観察対象 BT を差し替え (nullptr で解除、非所有)。step counter / history / status を初期化するがメタミラーは触らない。", when: "観察対象設定時。" },
    { sig: "CBehaviorTree* CurrentTree() const noexcept", ret: "tree or nullptr", desc: "現在観察中の BT。" },
    { sig: "bool IsAutorun() const noexcept / void SetAutorun(bool b) noexcept", ret: "bool", desc: "autorun (毎フレーム自動 Tick) の取得 / 切替。ON 時は <code>OnFrameBegin</code> で毎 frame 1 tick 進む。" },
    { sig: "void StepOnce() noexcept", ret: "—", desc: "dt=0.016f で 1 tick 手動進行。StepCallback 登録時はそちらに委譲。tree 未設定なら no-op。history に root status を push、step_count++。", when: "手動 step 時。" },
    { sig: "void Reset() noexcept", ret: "—", desc: "step counter / history / 全 node の last_status を初期化。メタミラーと autorun は触らない。" },
    { sig: "u32 StepCount() const noexcept", ret: "回数", desc: "現在の step counter (Reset で 0、StepOnce で +1)。" },
    { sig: "u32 SelectedNodeId() const noexcept", ret: "id or kInvalidId", desc: "現在の選択 node id。未選択は kInvalidId。" },
    { sig: "void SelectNode(u32 node_id) noexcept", ret: "—", desc: "選択 node を設定。範囲外 / kInvalidId は「未選択」に正規化。" },
    { sig: "u32 AddNode(EBtKind kind, const char* name, u32 parent_id) noexcept", ret: "払い出した node_id or kInvalidId", desc: "メタミラーに node を追加し id (0..N-1) を返す。<code>name</code> は永続領域 (非所有)。parent_id が範囲外なら root 扱い。上限到達は kInvalidId。", when: "BT 構造を panel に教える時。" },
    { sig: "void SetNodeStatus(u32 node_id, EBtStatus status) noexcept", ret: "—", desc: "node の last_status を更新。ABtAction の Fn から呼ぶ想定。範囲外は no-op。" },
    { sig: "void ClearNodes() noexcept", ret: "—", desc: "メタミラー全削除 + selection 解除。autorun / step counter / history はクリアしない。" },
    { sig: "u32 NodeCount() const noexcept", ret: "個数", desc: "現在登録済 node 数。" },
    { sig: "EBtStatus NodeStatus(u32 node_id) const noexcept", ret: "status", desc: "指定 id の last_status (範囲外は Failure)。" },
    { sig: "void SetOnStepCallback(StepCallback cb, void* user) noexcept", ret: "—", desc: "1 tick 進める独自処理を登録 (nullptr で解除)。" },
    { sig: "const char* Title() const noexcept override", ret: "\"Behavior Tree Editor\"", desc: "AEditorPanel override: window タイトル。" },
    { sig: "void OnFrameBegin(f32 dt) noexcept override", ret: "—", desc: "AEditorPanel override: autorun が ON のとき実 dt で 1 tick 進める non-ImGui hook。" },
    { sig: "void DrawUI() noexcept override", ret: "—", desc: "AEditorPanel override: toolbar / history graph / tree view / node inspector を描画。" },
        { sig: "using FBehaviorTreeEditorPanel = ABehaviorTreeEditorPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ABehaviorTreeEditorPanel</code> を使う。" }
      ]
},
{
  name: "AAnimCurveEditorPanel",
  kind: "クラス(AEditorPanel 派生)", header: "gameframework/tools/animcurve/AnimCurveEditorPanel.h",
  summary: "<code>FAnimationCurve</code> (Hermite/Linear/Step + <code>FAnimationCurve::EWrapMode</code>) を ImGui で対話編集する <b>curve editor パネル</b>。canvas に波形を 1024 sample で描画し、各 key を marker で表示・drag 編集、Hermite key は in/out tangent handle も drag 可。右クリックで Add/Delete。curve は <b>非所有 (caller 所有)</b>。変更は <code>CurveChangeCallback</code> で通知。非コピー・非ムーブ・全 noexcept。",
  when: "easing / FOV カーブ / fade alpha など任意の FAnimationCurve を視覚的に編集したい時。<code>SetCurve(&amp;curve)</code> でバインドする。",
  sample: "namespace ac = acs::game::animcurve;\nac::AAnimCurveEditorPanel panel;\npanel.Init();\nacs::game::FAnimationCurve curve;  // 別途 key 追加済とする\npanel.SetCurve(&amp;curve);\npanel.SetOnChangeCallback([](void* user, acs::game::FAnimationCurve* c) noexcept {\n    (void)user; (void)c;   // autosave 等\n}, nullptr);\nif (panel.IsDirty()) panel.ClearDirty();\npanel.Shutdown();",
  members: [
    { sig: "using CurveChangeCallback = void (*)(void* user, acs::game::FAnimationCurve* curve) noexcept", desc: "curve 変更時に呼ばれる関数ポインタ型。key 追加 / 削除 / interp / wrap 変更は即時 1 回、drag は drag end で 1 回発火。" },
    { sig: "void Init() noexcept", ret: "—", desc: "curve 参照を nullptr、selection 未選択、dirty=false、callback を nullptr に。多重 Init 可。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "curve 参照 / selection / callback を解除。curve 自体は破棄しない。多重 Shutdown 可。" },
    { sig: "void SetCurve(acs::game::FAnimationCurve* curve) noexcept", ret: "—", desc: "編集対象 curve を raw 参照でセット (nullptr で解除、非所有)。selection / dirty をリセット。", when: "編集対象バインド時。" },
    { sig: "acs::game::FAnimationCurve* CurrentCurve() const noexcept", ret: "curve or nullptr", desc: "現在編集対象の curve (未バインドは nullptr)。" },
    { sig: "bool IsDirty() const noexcept", ret: "bool", desc: "最後の ClearDirty 以降に編集があったか。Save ボタン enable 判定等に使う。" },
    { sig: "void ClearDirty() noexcept", ret: "—", desc: "dirty を false に戻す。curve 保存直後に呼ぶ。" },
    { sig: "void SetOnChangeCallback(CurveChangeCallback cb, void* user) noexcept", ret: "—", desc: "変更通知 callback を登録 (nullptr で解除)。" },
    { sig: "const char* Title() const noexcept override", ret: "\"Animation Curve Editor\"", desc: "AEditorPanel override: window タイトル (実体リテラルは \"Animation Curve Editor\")。" },
    { sig: "void DrawUI() noexcept override", ret: "—", desc: "AEditorPanel override: Toolbar + Canvas を描画。curve 未バインドなら \"(No curve bound)\"。" },
    { sig: "static constexpr u32 kCurveSampleCount = 1024u", desc: "canvas 上で curve をサンプルする点数 (線描画の解像度)。" },
    { sig: "static constexpr i32 kNoKeySelected = -1", desc: "「未選択」を表す sentinel (i32 戻り値で使用)。" },
    { sig: "static constexpr f32 kTangentHandleLengthPx = 30.0f", desc: "接線 handle 描画用の固定 px 長 (実 tangent 値とは無関係)。" },
    { sig: "static constexpr f32 kKeyMarkerRadiusPx = 5.0f", desc: "key marker の半径 (px)。クリック判定にも使う。" },
        { sig: "using FAnimCurveEditorPanel = AAnimCurveEditorPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AAnimCurveEditorPanel</code> を使う。" }
      ]
},
{
  name: "FFontFaceInfo",
  kind: "構造体(POD)", header: "gameframework/tools/fontedit/FontEditorPanel.h",
  summary: "1 font face のメタ情報 (= <t>fallback chain</t> の 1 要素)。全フィールド POD (trivially copyable)。文字列 (<code>file_path</code> / <code>family_name</code>) は caller 所有のリテラル前提で <b>非所有保持</b> (ACS 規約: <code>&lt;string&gt;</code> 禁止)。",
  when: "<code>AFontEditorPanel::AddFontFace</code> に渡す font face 定義として。loader が <code>file_path</code> を CreateFile / WIC 等に渡す。",
  sample: "acs::game::fontedit::FFontFaceInfo primary {};\nprimary.file_path      = L\"assets/fonts/NotoSansJP-Regular.otf\";\nprimary.family_name    = \"Noto Sans JP\";\nprimary.base_size_px   = 24.0f;\nprimary.char_range_min = 0x0020u;\nprimary.char_range_max = 0xFFFFu;\nprimary.is_msdf        = true;",
  members: [
    { sig: "const wchar_t* file_path = nullptr", desc: "font file の絶対 / 相対 path (Windows wide)。非所有。" },
    { sig: "const char* family_name = nullptr", desc: "UI 表示用 family 名 (UTF-8、\"Noto Sans JP\" 等)。非所有。" },
    { sig: "f32 base_size_px = 24.0f", desc: "この face を基準サイズで焼くときの px。" },
    { sig: "u32 char_range_min = 0x0020u", desc: "担当 Unicode 範囲の下端。" },
    { sig: "u32 char_range_max = 0x00FFu", desc: "担当 Unicode 範囲の上端。" },
    { sig: "u32 fallback_index = 0u", desc: "chain の何番目か。panel が Add/Move のたびに TArray index と同期して再書き込みする冗長情報。" },
    { sig: "bool is_msdf = false", desc: "MSDF アトラスで焼くか (true) / 通常 bitmap か (false)。" }
  ]
},
{
  name: "AFontEditorPanel",
  kind: "クラス(AEditorPanel 派生)", header: "gameframework/tools/fontedit/FontEditorPanel.h",
  summary: "複数 font face の <t>fallback chain</t> を ImGui で対話編集 + プレビューする <b>エディタパネル</b>。<code>FFontFaceInfo</code> の順序保持配列を持ち、add / remove / reorder (MoveUp/Down) と、任意 UTF-8 テキストの擬似プレビュー (実 atlas は持たず ImGui デフォルト font で代用) を提供。非コピー・非ムーブ・全 noexcept。",
  when: "font の fallback 優先順位と各 face のメタ情報を編集したい時。編集結果を取り戻して別途 <code>FFont::LoadFromFile</code> 等を呼ぶ (panel は設定エディタ、loader は外部)。",
  sample: "namespace fe = acs::game::fontedit;\nfe::AFontEditorPanel panel;\npanel.Init();\nworkspace.RegisterPanel(&amp;panel);\n\nfe::FFontFaceInfo primary {};\nprimary.file_path   = L\"assets/fonts/NotoSansJP-Regular.otf\";\nprimary.family_name = \"Noto Sans JP\";\npanel.AddFontFace(primary);\npanel.SetPreviewText(\"Hello, ACS! 日本語\");\npanel.SetPreviewFontSize(28.0f);\npanel.Shutdown();",
  members: [
    { sig: "void Init() noexcept", ret: "—", desc: "face 配列をクリア、selection 解除、preview text をデフォルト、preview size を 24.0f に。多重 Init 可。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "face 配列 / preview バッファをゼロ化。<code>OnShutdown</code> とは別の panel 単体 reset API。多重 Shutdown 可。" },
    { sig: "u32 FontFaceCount() const noexcept", ret: "個数", desc: "現在の face 数。" },
    { sig: "const FFontFaceInfo* GetFontFace(u32 i) const noexcept", ret: "info or nullptr", desc: "i 番目の face。範囲外は nullptr。" },
    { sig: "void AddFontFace(const FFontFaceInfo& info) noexcept", ret: "—", desc: "face を末尾に追加。<code>fallback_index</code> は末尾 index で上書き。<code>kMaxFontFaces</code> 到達なら no-op。", when: "追加時。" },
    { sig: "void RemoveFontFace(u32 i) noexcept", ret: "—", desc: "i 番目を削除 (順序保持)。後続の fallback_index を詰めて再書き込み。selection も追随。範囲外は no-op。" },
    { sig: "void MoveFaceUp(u32 i) noexcept", ret: "—", desc: "i 番目を 1 つ上へ (優先度を上げる)。i==0 / 範囲外は no-op。両者の fallback_index を再書き込み、selection 追随。" },
    { sig: "void MoveFaceDown(u32 i) noexcept", ret: "—", desc: "i 番目を 1 つ下へ (優先度を下げる)。i==count-1 / 範囲外は no-op。" },
    { sig: "i32 SelectedIndex() const noexcept", ret: "index or -1", desc: "現在の選択 face index (-1 = 未選択)。" },
    { sig: "void SelectFace(i32 i) noexcept", ret: "—", desc: "face を選択。範囲外 (&lt;0 or &gt;=FontFaceCount) は -1 に正規化。" },
    { sig: "void SetPreviewText(const char* utf8) noexcept", ret: "—", desc: "preview 文字列を設定 (UTF-8、末尾 '\\0' 必須)。<code>kPreviewTextCapacity-1</code> 超過分は切り詰め。nullptr は空文字列。" },
    { sig: "const char* PreviewText() const noexcept", ret: "文字列", desc: "現在の preview 文字列 (静的バッファ、寿命 = panel 寿命)。" },
    { sig: "f32 PreviewFontSize() const noexcept / void SetPreviewFontSize(f32 px) noexcept", ret: "px", desc: "preview font size (px) の取得 / 設定。" },
    { sig: "const char* Title() const noexcept override", ret: "\"Font Editor\"", desc: "AEditorPanel override: window タイトル。" },
    { sig: "void OnInit(editor_core::CEditorWorkspace& workspace) noexcept override", ret: "—", desc: "AEditorPanel override: workspace 登録時。基底実装 + preview バッファの終端 0 確認。" },
    { sig: "void DrawUI() noexcept override", ret: "—", desc: "AEditorPanel override: toolbar / list / preview / inspector + char-range strip を描画。<code>IsVisible()</code> が false なら早期 return。" },
    { sig: "static constexpr u32 kMaxFontFaces = 32u", desc: "fallback chain の最大 face 数。" },
    { sig: "static constexpr u32 kPreviewTextCapacity = 256u", desc: "preview 文字列の最大 byte 長 (終端 '\\0' 込み)。" },
    { sig: "static constexpr f32 kMinPreviewFontSize = 8.0f", desc: "preview font size slider の下限 (px)。" },
    { sig: "static constexpr f32 kMaxPreviewFontSize = 96.0f", desc: "preview font size slider の上限 (px)。" },
    { sig: "static constexpr u32 kCharRangePreviewMin = 0x0020u", desc: "下部 char range preview の Unicode 下端 (ASCII 開始)。" },
    { sig: "static constexpr u32 kCharRangePreviewMax = 0x00FFu", desc: "下部 char range preview の Unicode 上端 (Latin-1 補助末尾)。" },
        { sig: "using FFontEditorPanel = AFontEditorPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AFontEditorPanel</code> を使う。" }
      ]
},
{
  name: "EPivotPreset",
  kind: "列挙(enum class : u8)", header: "gameframework/tools/spriteatlas/SpriteAtlasEditorPanel.h",
  summary: "<code>ASpriteAtlasEditorPanel</code> のツールバー「Pivot toggle」用 enum。frame の <t>pivot</t> をプリセット切替する。E-prefix は ACS 規約。",
  when: "Pivot プリセットを選ぶ時。<code>SetPivotPreset</code> / <code>PivotPreset</code> で読み書き。Custom 以外を選ぶと selected frame に pivot 値が流し込まれる。",
  members: [
    { sig: "Center = 0", desc: "pivot = (0.5, 0.5)。" },
    { sig: "TopLeft = 1", desc: "pivot = (0.0, 0.0)。" },
    { sig: "Custom = 2", desc: "inspector で pivot_x / pivot_y を slider で直接編集。" }
  ]
},
{
  name: "ASpriteAtlasEditorPanel",
  kind: "クラス(AEditorPanel 派生)", header: "gameframework/tools/spriteatlas/SpriteAtlasEditorPanel.h",
  summary: "<code>FSpritePack</code> の atlas texture path + 名前付き <t>FSpriteFrame</t> 列を ImGui で対話編集する <b>エディタパネル</b>。toolbar (New Frame / Delete / Pivot 切替) + viewport (atlas placeholder + 矩形 overlay + 8 handle resize) + frame list + inspector の構成。FSpritePack は <b>非所有 (caller 所有)</b>。新規 frame 名は内部静的プールに書き込む。非コピー・非ムーブ・全 noexcept。",
  when: "sprite atlas の frame 矩形と pivot を編集したい時。<code>SetSpritePack(&amp;pack)</code> で注入する。",
  sample: "namespace sa = acs::game::spriteatlas;\nsa::ASpriteAtlasEditorPanel panel;\npanel.Init();\nacs::game::FSpritePack pack;  // 別途 Init 済とする\npanel.SetSpritePack(&amp;pack);\nworkspace.RegisterPanel(&amp;panel);\npanel.AddFrame();\npanel.SetPivotPreset(sa::EPivotPreset::Center);\nworkspace.UnregisterPanel(&amp;panel);\npanel.Shutdown();",
  members: [
    { sig: "void Init() noexcept", ret: "—", desc: "FSpritePack 参照を nullptr、selection 解除、zoom=1.0、pivot=Center に。多重 Init 可。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "frame name pool / selection / FSpritePack* をクリア。<code>OnShutdown</code> とは別の panel 単体 reset API。多重 Shutdown 可。" },
    { sig: "void SetSpritePack(acs::game::FSpritePack* pack) noexcept", ret: "—", desc: "編集対象 FSpritePack を注入 (raw / 非所有)。nullptr で「未接続」状態 (UI が gracefully 退化)。selection は 0 にリセット。", when: "編集対象注入時。" },
    { sig: "acs::game::FSpritePack* CurrentPack() const noexcept", ret: "pack or nullptr", desc: "現在注入されている FSpritePack。未注入は nullptr。" },
    { sig: "i32 SelectedFrameIndex() const noexcept", ret: "index or -1", desc: "現在の選択 frame index。未選択は -1。" },
    { sig: "void SelectFrame(i32 idx) noexcept", ret: "—", desc: "frame を選択。範囲外 (&lt;0 or &gt;=FrameCount) は -1 に正規化。" },
    { sig: "void AddFrame() noexcept", ret: "—", desc: "新規 frame を default 64x64 (原点 0,0) で追加。名前は内部プールに \"Frame_NN\" で書く。selection を新規へ。pack 未注入 / 上限到達は no-op。", when: "追加時。" },
    { sig: "void DeleteSelectedFrame() noexcept", ret: "—", desc: "選択中 frame を FSpritePack から削除。selection を前 index に詰め、FSpritePack::AllFrames を再走査して index 整合を取り直す。未選択 / 範囲外 / pack 未注入は no-op。" },
    { sig: "f32 ZoomLevel() const noexcept / void SetZoomLevel(f32 z) noexcept", ret: "倍率", desc: "atlas placeholder 表示の倍率 (1.0 = 等倍)。setter は <code>[kMinZoom, kMaxZoom]</code> にクランプ。" },
    { sig: "EPivotPreset PivotPreset() const noexcept / void SetPivotPreset(EPivotPreset p) noexcept", ret: "preset", desc: "Pivot toggle の取得 / 設定。Custom 以外を選ぶと selected frame に pivot 値が流し込まれる。" },
    { sig: "const char* Title() const noexcept override", ret: "\"Sprite Atlas Editor\"", desc: "AEditorPanel override: window タイトル。" },
    { sig: "void OnInit(editor_core::CEditorWorkspace& workspace) noexcept override", ret: "—", desc: "AEditorPanel override: workspace 登録時。基底実装 + frame name pool の終端 0 確認。" },
    { sig: "void DrawUI() noexcept override", ret: "—", desc: "AEditorPanel override: toolbar / list / viewport / inspector を描画。<code>IsVisible()</code> が false なら早期 return。" },
    { sig: "static constexpr u32 kMaxOwnedFrames = 64u", desc: "panel 内で命名する default frame name の最大個数。超えると AddFrame は no-op。" },
    { sig: "static constexpr u32 kFrameNameMaxChars = 32u", desc: "1 frame name の最大文字数 (内部プール 1 slot)。" },
    { sig: "static constexpr f32 kMinZoom = 0.25f", desc: "Zoom 下限 (1/4 縮小)。" },
    { sig: "static constexpr f32 kMaxZoom = 8.0f", desc: "Zoom 上限 (8x 拡大)。" },
    { sig: "static constexpr u32 kDefaultFrameW = 64u", desc: "AddFrame で作る default frame の幅 (px)。" },
    { sig: "static constexpr u32 kDefaultFrameH = 64u", desc: "AddFrame で作る default frame の高さ (px)。" },
        { sig: "using FSpriteAtlasEditorPanel = ASpriteAtlasEditorPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ASpriteAtlasEditorPanel</code> を使う。" }
      ]
},
{
  name: "ETimelineKeyKind",
  kind: "列挙(enum class : u8)", header: "gameframework/tools/cinetimeline/CinematicsTimelineEditorPanel.h",
  summary: "<code>ACinematicsTimelineEditorPanel</code> の編集 UI 上で扱う 5 種の <b>演出概念</b>。runtime 側の <code>CCinematicsDirector::ETimelineTrackKind</code> とは意図的に概念を分離し、オーサリング向けの要素を表現する。Play 時に <code>ToTrackKind()</code> 相当で director の TrackKind にマップされる。",
  when: "<code>AddKeyframe</code> で新規キーの種別を指定する時、および timeline の 5 トラックの行割り当て。",
  members: [
    { sig: "CameraCut = 0", desc: "カメラ位置を瞬時に切り替え (target FVec3)。" },
    { sig: "FadeColor = 1", desc: "画面全体を start_color → end_color にフェード。" },
    { sig: "TimeScale = 2", desc: "ゲーム時間スケールを倍率指定 (1.0 = 等倍、0.5 = スロー)。" },
    { sig: "SpawnEffect = 3", desc: "エフェクト発火 (effect_id + position)。" },
    { sig: "TriggerCallback = 4", desc: "汎用 callback 発火 (event_id をユーザ定義で解釈)。" }
  ]
},
{
  name: "FEditorKeyframe",
  kind: "構造体", header: "gameframework/tools/cinetimeline/CinematicsTimelineEditorPanel.h",
  summary: "<code>ACinematicsTimelineEditorPanel</code> が内部保持する keyframe (リッチ payload 付き)。runtime の <code>CCinematicsDirector::FTimelineKeyframe</code> より広い payload (FVec3 / 色 / scale / event_id) を持つ。Play 時に kind に応じた FTimelineKeyframe へ変換 (ベイク) される。active な kind に対応するフィールドのみ意味を持つ。",
  when: "panel 内部の keyframe 配列の要素として。通常は <code>AddKeyframe</code> 経由で生成され、直接構築はあまりしない。",
  sample: "acs::game::cinetimeline::FEditorKeyframe kf {};\nkf.time_sec = 2.0f;\nkf.kind = acs::game::cinetimeline::ETimelineKeyKind::FadeColor;\nkf.fade_start_color = { 0.0f, 0.0f, 0.0f };\nkf.fade_end_color   = { 1.0f, 1.0f, 1.0f };",
  members: [
    { sig: "f32 time_sec = 0.0f", desc: "タイムライン上の発火時刻 [秒]。" },
    { sig: "ETimelineKeyKind kind = ETimelineKeyKind::TriggerCallback", desc: "この keyframe の種別。" },
    { sig: "FVec3 camera_target { 0,0,0 }", desc: "CameraCut / SpawnEffect で使用 (target / spawn position)。" },
    { sig: "FVec3 fade_start_color { 0,0,0 }", desc: "FadeColor で使用 (r,g,b in [0,1])。" },
    { sig: "FVec3 fade_end_color { 1,1,1 }", desc: "FadeColor で使用 (r,g,b in [0,1])。" },
    { sig: "f32 time_scale { 1.0f }", desc: "TimeScale で使用 (時間倍率)。" },
    { sig: "u32 event_id { 0u }", desc: "SpawnEffect / TriggerCallback で使用。" },
    { sig: "FEditorKeyframe() noexcept = default", desc: "既定構築 (全フィールド上記既定値)。" }
  ]
},
{
  name: "ACinematicsTimelineEditorPanel",
  kind: "クラス(AEditorPanel 派生)", header: "gameframework/tools/cinetimeline/CinematicsTimelineEditorPanel.h",
  summary: "<code>CCinematicsDirector</code> を対話編集する <b>timeline editor パネル</b> (Unreal Sequencer の最小版)。水平タイムライン + 5 トラック (ETimelineKeyKind ごと) + keyframe marker + 再生制御 (Play/Pause/Stop/Step) + scrub slider を提供。リッチな <code>FEditorKeyframe</code> を panel 所有で持ち、Play 時に director へベイクする。director は <b>非所有 (caller 所有)</b>。非コピー・非ムーブ・全 noexcept。",
  when: "cutscene の keyframe を時間軸上で配置・編集し、再生確認したい時。<code>SetCinematicsDirector(&amp;dir)</code> でバインドする。",
  sample: "namespace ct = acs::game::cinetimeline;\nct::ACinematicsTimelineEditorPanel panel;\npanel.Init();\nworkspace.RegisterPanel(&amp;panel);\nacs::game::CCinematicsDirector director;\npanel.SetCinematicsDirector(&amp;director);\npanel.AddKeyframe(ct::ETimelineKeyKind::CameraCut, 0.0f);\npanel.AddKeyframe(ct::ETimelineKeyKind::FadeColor, 2.0f);\npanel.Play();\npanel.Step(0.016f);\npanel.Shutdown();",
  members: [
    { sig: "void Init() noexcept", ret: "—", desc: "director 参照を nullptr、selection 未選択、duration を default (10s)、keyframes を空に。多重 Init 可。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "director 参照 / selection / keyframes を解除。director 自体は破棄しない。多重 Shutdown 可。" },
    { sig: "void SetCinematicsDirector(acs::game::CCinematicsDirector* dir) noexcept", ret: "—", desc: "編集対象 director を raw 参照でセット (nullptr で解除、非所有)。selection と <code>m_CurrentTime</code> を 0 に戻す。", when: "編集対象バインド時。" },
    { sig: "acs::game::CCinematicsDirector* CurrentDirector() const noexcept", ret: "director or nullptr", desc: "現在編集対象の director (未バインドは nullptr)。" },
    { sig: "void Play() noexcept", ret: "—", desc: "現在位置から再生開始。director がバインドされていれば全 keyframe を Clear → editor の現状で再 bake → Play する。", when: "再生時。" },
    { sig: "void Pause() noexcept", ret: "—", desc: "再生一時停止 (m_Playing=false、m_CurrentTime 保持)。" },
    { sig: "void Stop() noexcept", ret: "—", desc: "完全停止 (m_Playing=false、m_CurrentTime=0、director.Stop も呼ぶ)。" },
    { sig: "void Step(f32 dt) noexcept", ret: "—", desc: "dt 秒進める。m_Playing が true なら director.Tick(dt) も呼んで keyframe を発火し、m_CurrentTime を director.CurrentTime() と同期。再生中専用。", when: "再生中の毎フレーム。" },
    { sig: "bool IsPlaying() const noexcept", ret: "bool", desc: "再生中か。" },
    { sig: "f32 CurrentTimeSec() const noexcept / void SetCurrentTimeSec(f32 t) noexcept", ret: "秒", desc: "タイムカーソルの現在時刻。setter は [0, Duration] にクランプ。" },
    { sig: "f32 DurationSec() const noexcept / void SetDurationSec(f32 d) noexcept", ret: "秒", desc: "タイムライン全体の長さ。setter は最低 <code>kMinDurationSec</code> (0.1s) にクランプ。" },
    { sig: "i32 SelectedKeyframeIndex() const noexcept", ret: "index or kNoKeySelected", desc: "現在選択中の keyframe index (内部 TArray の index)。未選択は kNoKeySelected。" },
    { sig: "void SelectKeyframe(i32 i) noexcept", ret: "—", desc: "selection を変更。<code>-1..KeyframeCount-1</code> 範囲外は kNoKeySelected に丸める。" },
    { sig: "void AddKeyframe(ETimelineKeyKind kind, f32 time_sec) noexcept", ret: "—", desc: "新規 keyframe を kind + time_sec で追加。time_sec は [0, Duration] にクランプ、配列は time 昇順を維持。追加後は新 keyframe を selection に。", when: "キー追加時。" },
    { sig: "void RemoveSelectedKeyframe() noexcept", ret: "—", desc: "選択中 keyframe を削除 (selection 無効なら no-op)。削除後 selection を解除。" },
    { sig: "const char* Title() const noexcept override", ret: "\"Cinematics Timeline\"", desc: "AEditorPanel override: window タイトル。" },
    { sig: "void DrawUI() noexcept override", ret: "—", desc: "AEditorPanel override: Toolbar + Timeline canvas + Inspector + Ruler を描画。" },
    { sig: "static constexpr i32 kNoKeySelected = -1", desc: "「未選択」を表す sentinel (i32 戻り値で使用)。" },
    { sig: "static constexpr u32 kTrackCount = 5u", desc: "タイムライン全 track の数 (= ETimelineKeyKind の要素数)。" },
    { sig: "static constexpr f32 kTrackRowHeightPx = 28.0f", desc: "1 トラックの行高さ (px)。" },
    { sig: "static constexpr f32 kMarkerWidthPx = 10.0f", desc: "marker の幅 (px)。" },
    { sig: "static constexpr f32 kMarkerHitSlackPx = 3.0f", desc: "marker AABB hit-test の余裕 (px)。" },
    { sig: "static constexpr f32 kMinDurationSec = 0.1f", desc: "Duration の最低値 (秒)。" },
    { sig: "static constexpr f32 kDefaultDurationSec = 10.0f", desc: "Duration のデフォルト値 (秒)。" },
        { sig: "using FCinematicsTimelineEditorPanel = ACinematicsTimelineEditorPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ACinematicsTimelineEditorPanel</code> を使う。" }
      ]
},
{
  name: "AModelViewerPanel",
  kind: "クラス(AEditorPanel 派生)", header: "gameframework/tools/modelview/ModelViewerPanel.h",
  summary: "ModelViewer の<b>メインビューポート</b>パネル。<t>AEditorPanel</t> を継承し、3D モデルの表示領域と「表示パラメータ」(<t>CEditorCamera</t> / ライティング / 背景色 / グリッド / ボーン表示) を ImGui で操作する。<b>実際の 3D 描画は持たず</b>、カメラ state・光・背景・トグル群といった<b>数値パラメータの保管 + UI</b> だけを担い、描画は外部の <code>ModelViewportRenderer</code>(将来) が <code>CurrentAssetPath()</code> 等を見て行う。<t>CAssetBrowser</t> からの選択通知を受けて mesh 拡張子なら自動でモデルを切り替える。非コピー / 非ムーブ・全 noexcept。",
  when: "モデルビューアの中央ビューポートとして使う。<code>CEditorWorkspace::RegisterPanel</code> で登録し、毎フレームの <code>DrawUI</code> でビューポートと control bar を出す。カメラ行列や光パラメータを外部レンダラに渡したい時にアクセサ経由で取得する。",
  sample: "AModelViewerPanel viewer;\nviewer.Init();\nworkspace.RegisterPanel(&amp;viewer);    // AEditorPanel として登録\n\n// CAssetBrowser からの選択通知で自動 LoadModelAsset:\nworkspace.BroadcastAssetSelected(\"models/hero.mdl\");\n\nif (viewer.HasModel()) {\n    auto&amp; cam = viewer.Camera();      // renderer が ViewMatrix を取り出す\n    (void)cam;\n}",
  members: [
    { sig: "void Init() noexcept", ret: "—", desc: "内部 state をデフォルトに戻し、<t>CEditorCamera</t> を 3D mode で初期化、asset path を空に。多重呼び出し可(= 完全リセット)。", when: "panel 生成直後 / 再利用時。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "内部 state を全解放しカメラを Reset。<code>OnShutdown</code> とは別物で、Workspace から外す前に単体 reset したい時の API。多重呼び出し可。", when: "任意" },
    { sig: "void LoadModelAsset(const wchar_t* asset_path) noexcept", ret: "—", desc: "<code>asset_path</code> をコピーして内部バッファに保存し <code>HasModel()</code> を true に。<code>nullptr</code> / 空文字なら <code>ClearModel()</code> 相当。実 mesh 読込は行わず外部レンダラの責務。", when: "モデルを切り替える時。" },
    { sig: "void ClearModel() noexcept", ret: "—", desc: "asset path を空に戻し <code>HasModel()</code> を false に。カメラ / ライティング / 背景は保持(= モデルを替えても視点と照明が残る)。", when: "モデルを外す時。" },
    { sig: "bool HasModel() const noexcept", ret: "有無", desc: "model asset が(path として)セットされているか。実 GPU リソースの有無とは独立。", when: "任意" },
    { sig: "const wchar_t* CurrentAssetPath() const noexcept", ret: "パス", desc: "現在の asset path(UTF-16)。未設定 / Clear 後は <code>nullptr</code> ではなく <code>L\"\"</code> を返す。", when: "外部レンダラが読み込み対象を知る時。" },
    { sig: "editor_core::CEditorCamera& Camera() noexcept", ret: "カメラ参照", desc: "内部 <t>CEditorCamera</t> への参照。renderer / UI が <code>HandleMouseInput</code> / <code>Tick</code> / <code>ViewMatrix</code> 等を呼ぶ。寿命は panel と同一。", when: "視点操作・行列取得時。" },
    { sig: "void SetLightDirection(acs::FVec3 dir) noexcept", ret: "—", desc: "sun light の方向を設定。既定 <code>(0.3, -0.7, 0.6)</code>。正規化は panel では行わず renderer 側が必要なら行う。", when: "光の向きを変える時。" },
    { sig: "acs::FVec3 LightDirection() const noexcept", ret: "方向", desc: "現在の sun light 方向。", when: "任意" },
    { sig: "void SetLightColor(acs::FVec3 color) noexcept", ret: "—", desc: "sun light の色(RGB linear)。既定 white。HDR 強度は >1.0 を入れる。", when: "光色を変える時。" },
    { sig: "acs::FVec3 LightColor() const noexcept", ret: "色", desc: "現在の sun light 色。", when: "任意" },
    { sig: "void SetIblEnabled(bool b) noexcept", ret: "—", desc: "<t>IBL</t>(環境光)を有効にするか。既定 true。", when: "IBL を切り替える時。" },
    { sig: "bool IsIblEnabled() const noexcept", ret: "真偽", desc: "IBL が有効か。", when: "任意" },
    { sig: "void SetToneMappingMode(u32 mode) noexcept", ret: "—", desc: "tonemap mode: 0=ACES(既定) / 1=Reinhard / 2=Linear。範囲外値は無視(既存値を維持)。renderer 側 enum と疎結合にするため u32。", when: "トーンマップ方式を切り替える時。" },
    { sig: "u32 ToneMappingMode() const noexcept", ret: "モード", desc: "現在の tonemap mode(0..2)。", when: "任意" },
    { sig: "void SetBackgroundColor(acs::FVec4 color) noexcept", ret: "—", desc: "viewport 背景色(RGBA linear)。既定 <code>(0.15,0.15,0.18,1.0)</code>。renderer の clear color に使う。", when: "背景色を変える時。" },
    { sig: "acs::FVec4 BackgroundColor() const noexcept", ret: "色", desc: "現在の背景色。", when: "任意" },
    { sig: "bool ShowGrid() const noexcept / void SetShowGrid(bool b) noexcept", ret: "—", desc: "XZ 平面グリッドを描くか。既定 true。", when: "グリッド表示を切り替える時。" },
    { sig: "bool ShowBoneSkeleton() const noexcept / void SetShowBoneSkeleton(bool b) noexcept", ret: "—", desc: "ボーンスケルトンを line overlay で描くか。既定 false。", when: "ボーン表示を切り替える時。" },
    { sig: "const char* Title() const noexcept override", ret: "\"Model Viewport\"", desc: "ImGui ウインドウタイトル兼 ID。固定リテラル。", when: "基底からの呼び出し。" },
    { sig: "void OnInit(editor_core::CEditorWorkspace& workspace) noexcept override", ret: "—", desc: "Workspace 登録時に呼ばれる。基底実装で Workspace ポインタを保存し、本クラスで CEditorCamera を 3D mode で Init し直す。", when: "RegisterPanel から間接的に。" },
    { sig: "void DrawUI() noexcept override", ret: "—", desc: "ImGui::Begin \"Model Viewport\" + ビューポートプレースホルダ + control bar を描画。<code>IsVisible()</code> が false なら早期 return。実 3D 描画は外部レンダラの責務。", when: "毎フレーム。" },
    { sig: "void OnAssetSelected(const char* asset_path) noexcept override", ret: "—", desc: "CAssetBrowser からのファイル選択通知。拡張子が mesh 相当(<code>.mdl/.fbx/.gltf/.glb/.obj</code>)なら自動 <code>LoadModelAsset</code>。それ以外は無視。", when: "ブラウザでファイルを選んだ時。" },
    { sig: "static constexpr u32 kMaxAssetPathChars = 512u", ret: "定数", desc: "asset path バッファ長(wchar_t 単位、終端含む)。<code>CAssetBrowser::kMaxPathChars</code> と同値。", when: "—" },
    { sig: "static constexpr u32 kToneMappingModeCount = 3u", ret: "定数", desc: "ToneMappingMode の有効値数(= 0,1,2)。これを超える値は無視される。", when: "—" },
        { sig: "using FModelViewerPanel = AModelViewerPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AModelViewerPanel</code> を使う。" }
      ]
},
{
  name: "FMeshSummary",
  kind: "構造体(POD)", header: "gameframework/tools/modelview/ModelInspectorPanel.h",
  summary: "model 全体の<b>集計情報</b>を 1 インスタンスにまとめた値型。caller(<code>AModelViewerPanel</code> 等)が model load 後に集計し、<code>AModelInspectorPanel::UpdateFromModel</code> に渡す。全フィールド値型なので Mesh / Skeleton 側のリソース解放と panel 表示が完全に decouple される。",
  when: "Inspector パネルに「頂点数 / 三角形数 / submesh / material slot / ボーン / クリップ数 / バウンディング球」を渡す時に組み立てる。",
  sample: "FMeshSummary s;\ns.vertex_count    = 12000;\ns.triangle_count  = 4000;\ns.submesh_count   = 3;\ns.bone_count      = 52;\ns.bounding_radius = 1.8f;",
  members: [
    { sig: "u32 vertex_count = 0", desc: "全 vertex 数(submesh 跨ぎ合算後)。" },
    { sig: "u32 triangle_count = 0", desc: "全 triangle 数(index_count / 3 合算)。" },
    { sig: "u32 submesh_count = 0", desc: "submesh セクション数。" },
    { sig: "u32 material_slot_count = 0", desc: "material slot 数(= 異なる material の数)。" },
    { sig: "u32 bone_count = 0", desc: "skeleton 内 bone 数(skeleton 無しなら 0)。" },
    { sig: "u32 animation_clip_count = 0", desc: "関連付けられた animation clip 数(0 可)。" },
    { sig: "f32 bounding_radius = 0.0f", desc: "model 全体の bounding sphere 半径。" },
    { sig: "acs::FVec3 bounding_center = {}", desc: "同 bounding sphere の中心(object 空間)。" }
  ]
},
{
  name: "FSubmeshInfo",
  kind: "構造体(POD)", header: "gameframework/tools/modelview/ModelInspectorPanel.h",
  summary: "1 つの submesh セクションの情報(名前 + index 範囲 + 使用する material slot)。<code>name</code> は caller 所有のリテラル / 永続文字列を想定(panel 寿命 ≦ name 寿命)。",
  when: "Inspector に submesh テーブルを表示するため、submesh ごとに 1 件作って配列で渡す。",
  sample: "FSubmeshInfo sm;\nsm.name                = \"Body\";\nsm.index_start         = 0;\nsm.index_count         = 3600;   // 3 の倍数想定\nsm.material_slot_index = 0;",
  members: [
    { sig: "const char* name = nullptr", desc: "submesh 名(例: \"Body\", \"Hair\", \"Eyes\")。caller 所有のポインタ。" },
    { sig: "u32 index_start = 0", desc: "全 index buffer 内の開始 index。" },
    { sig: "u32 index_count = 0", desc: "この submesh の index 数(3 の倍数想定)。" },
    { sig: "u32 material_slot_index = 0", desc: "この submesh が使う material slot 番号。" }
  ]
},
{
  name: "FBoneInfo",
  kind: "構造体(POD)", header: "gameframework/tools/modelview/ModelInspectorPanel.h",
  summary: "1 つの bone の情報(名前 + 親 index)。<code>parent_index &lt; 0</code> は root bone を意味する。Inspector は子リストを保持せず、描画時に <code>parent_index</code> から子を線形走査して TreeNode 階層を再構築する。",
  when: "Inspector のボーン階層ツリーを表示するため、bone ごとに 1 件作って配列で渡す。配列内 index が親参照のキーになる。",
  sample: "FBoneInfo bones[2];\nbones[0].name = \"Hips\";  bones[0].parent_index = -1;  // root\nbones[1].name = \"Spine\"; bones[1].parent_index = 0;   // Hips の子",
  members: [
    { sig: "const char* name = nullptr", desc: "bone 名(例: \"Hips\", \"Spine_01\")。caller 所有のポインタ。" },
    { sig: "i32 parent_index = -1", desc: "同 bones 配列内の親 index。root は -1。" }
  ]
},
{
  name: "FAnimationClipInfo",
  kind: "構造体(POD)", header: "gameframework/tools/modelview/ModelInspectorPanel.h",
  summary: "Inspector が表示する 1 つの animation clip のメタ情報(名前 / 長さ / サンプル数 / ループ可否)。<code>AModelAnimationPanel</code> の <code>FAnimationClipBinding</code> とは別物(こちらは read-only 表示用)。",
  when: "Inspector の Animations テーブルに clip 行を表示するため、clip ごとに 1 件作って配列で渡す。",
  sample: "FAnimationClipInfo c;\nc.name         = \"Idle\";\nc.duration_sec = 2.0f;\nc.sample_count = 60;\nc.is_looping   = true;",
  members: [
    { sig: "const char* name = nullptr", desc: "clip 名(例: \"Idle\", \"Run\", \"Attack01\")。caller 所有のポインタ。" },
    { sig: "f32 duration_sec = 0.0f", desc: "クリップの長さ(秒)。" },
    { sig: "u32 sample_count = 0", desc: "サンプル数(= keyframe 数の合計など)。" },
    { sig: "bool is_looping = false", desc: "ループ再生フラグ(clip metadata)。" }
  ]
},
{
  name: "AModelInspectorPanel",
  kind: "クラス(AEditorPanel 派生)", header: "gameframework/tools/modelview/ModelInspectorPanel.h",
  summary: "ModelViewer 内の<b>読み取り専用 mesh / skeleton / anim 情報ビューア</b>。<t>AEditorPanel</t> を継承し、<code>UpdateFromModel</code> で push された <code>FMeshSummary</code> / submesh / bone / clip 配列を<b>値コピーでスナップショット保持</b>して ImGui に表示する(Summary / Submeshes テーブル / Bones ツリー / Animations テーブルの 4 セクション)。callback も編集も GPU リソースも持たない純粋な viewer。非コピー / 非ムーブ。Unity の Mesh Inspector 相当。",
  when: "モデルのメタ情報を確認するパネルとして使う。<code>AModelViewerPanel</code> や sample がモデル load 完了時に <code>UpdateFromModel</code> を呼んで情報を流し込む。",
  sample: "AModelInspectorPanel insp;\ninsp.Init();\nworkspace.RegisterPanel(&amp;insp);\n\nFMeshSummary    sum;       // 集計を埋める\nFSubmeshInfo    subs[1]  = { { \"Body\", 0, 3600, 0 } };\ninsp.UpdateFromModel(sum, subs, 1,\n                     nullptr, 0,    // bones なし\n                     nullptr, 0);   // clips なし",
  members: [
    { sig: "void Init() noexcept", ret: "—", desc: "内部状態を空にする。多重呼び出し可。", when: "生成直後 / 再利用時。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "全配列 + summary をクリアし <code>has_model = false</code>。多重呼び出し可。", when: "任意" },
    { sig: "void UpdateFromModel(const FMeshSummary& summary, const FSubmeshInfo* submeshes, u32 submesh_count, const FBoneInfo* bones, u32 bone_count, const FAnimationClipInfo* clips, u32 clip_count) noexcept", ret: "—", desc: "表示するモデルのスナップショットを更新する。summary は値コピー、submesh / bone / clip 各配列は <t>TArray</t> に値コピーで PushBack。各ポインタは <code>nullptr</code> 可(count==0 と整合させる)。呼び出し後 <code>HasModel()</code> が true に。", when: "モデル load 完了時に caller が呼ぶ。" },
    { sig: "void Clear() noexcept", ret: "—", desc: "表示内容を空に戻し \"(No model loaded)\" 表示に切り替える。多重呼び出し可。", when: "モデル unload / workspace clear 時。" },
    { sig: "const FMeshSummary& Summary() const noexcept", ret: "summary 参照", desc: "保持中の集計情報を返す。", when: "任意" },
    { sig: "u32 SubmeshCount() const noexcept", ret: "件数", desc: "保持中の submesh 数。", when: "任意" },
    { sig: "const FSubmeshInfo* Submesh(u32 i) const noexcept", ret: "要素 or nullptr", desc: "i 番目の submesh。<code>i &gt;= SubmeshCount()</code> なら <code>nullptr</code>(防衛的アクセス、UB なし)。", when: "個別 submesh を参照する時。" },
    { sig: "u32 BoneCount() const noexcept", ret: "件数", desc: "保持中の bone 数。", when: "任意" },
    { sig: "const FBoneInfo* Bone(u32 i) const noexcept", ret: "要素 or nullptr", desc: "i 番目の bone。範囲外は <code>nullptr</code>。", when: "個別 bone を参照する時。" },
    { sig: "u32 AnimationClipCount() const noexcept", ret: "件数", desc: "保持中の clip 数。", when: "任意" },
    { sig: "const FAnimationClipInfo* AnimationClip(u32 i) const noexcept", ret: "要素 or nullptr", desc: "i 番目の clip。範囲外は <code>nullptr</code>。", when: "個別 clip を参照する時。" },
    { sig: "bool HasModel() const noexcept", ret: "真偽", desc: "<code>UpdateFromModel</code> 経由でモデルが push されたか。", when: "任意" },
    { sig: "const char* Title() const noexcept override", ret: "\"Model Info\"", desc: "ImGui ウインドウタイトル。", when: "基底からの呼び出し。" },
    { sig: "void DrawUI() noexcept override", ret: "—", desc: "ImGui::Begin(\"Model Info\") + Summary / Submeshes(CollapsingHeader + Table) / Bones(CollapsingHeader + TreeNode 再帰) / Animation Clips(CollapsingHeader + Table) を順に描画。未 load 時は \"(No model loaded)\" のみ表示。", when: "毎フレーム。" },
        { sig: "using FModelInspectorPanel = AModelInspectorPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AModelInspectorPanel</code> を使う。" }
      ]
},
{
  name: "FMaterialOverride",
  kind: "構造体(POD)", header: "gameframework/tools/modelview/ModelMaterialPanel.h",
  summary: "1 つの material slot 分の <b>PBR override データ</b>(base color / metallic / roughness / normal strength / AO strength / emissive / emissive intensity)。<code>is_overridden</code> が false の slot は「元の material そのまま」を意味し、各値は物理的に neutral な default で初期化される。ユーザーが UI で 1 つでも触ると <code>is_overridden</code> が true になる。<code>TArray</code> Resize 時に MemSet ゼロ初期化される trivially-constructible レイアウト。",
  when: "<code>AModelMaterialPanel</code> から callback で 1 件ずつ外部に渡され、受け側が <code>CPbrShader</code> 等の constant buffer に反映する。CUndoStack には「変更前 → 変更後」を 1 件分として atomic に積める。",
  sample: "void OnMatChanged(void* user, u32 slot, const FMaterialOverride&amp; ov) noexcept {\n    if (!ov.is_overridden) return;        // 元のマテリアルそのまま\n    // ov.base_color / ov.metallic / ov.roughness を shader CB に書く\n    (void)user; (void)slot;\n}",
  members: [
    { sig: "u32 slot_index = 0u", desc: "slot 番号(0..SlotCount()-1)。TArray index と一致する冗長情報だが、callback で外部に渡す際に index を引き直さずに済む。" },
    { sig: "FVec4 base_color = {1,1,1,1}", desc: "PBR base color(RGBA)。RGB は albedo / F0 tint、A は将来の alpha cutout 用。既定 white opaque。" },
    { sig: "f32 metallic = 0.0f", desc: "0=dielectric(非金属) / 1=metal。PBR Metallic workflow に直結。範囲 [0,1]。" },
    { sig: "f32 roughness = 0.5f", desc: "0=mirror smooth / 1=perfectly rough。GGX の roughness にそのまま渡す。範囲 [0,1]。" },
    { sig: "f32 normal_strength = 1.0f", desc: "normal map 影響度の倍率。1.0 で neutral、<1 で flatten、>1 で誇張。範囲 [0,2]。" },
    { sig: "f32 ao_strength = 1.0f", desc: "ambient occlusion 影響度。1.0 で AO テクスチャをそのまま、0 で AO 無効。範囲 [0,1]。" },
    { sig: "FVec3 emissive = {0,0,0}", desc: "自己発光色(ColorEdit3 値域 [0,1])。既定 黒(= 発光 OFF)。" },
    { sig: "f32 emissive_intensity = 1.0f", desc: "発光強度倍率。emissive との積で >1 に増幅され bloom / HDR で光って見える。範囲 [0,10]。" },
    { sig: "bool is_overridden = false", desc: "ユーザーがこの slot を 1 度でも編集したか。false なら ModelViewer は override を無視して元 material を使う。" }
  ]
},
{
  name: "AModelMaterialPanel",
  kind: "クラス(AEditorPanel 派生)", header: "gameframework/tools/modelview/ModelMaterialPanel.h",
  summary: "ModelViewer 内で、現在 load されたモデルの各 material slot に対し PBR マテリアルを<b>ライブ編集</b>する ImGui パネル。<t>AEditorPanel</t> を継承し、slot ごとの <code>FMaterialOverride</code> を内部 <t>TArray</t> に保持して CollapsingHeader + ColorEdit / SliderFloat で編集する。<b>編集 UI と override 値の保持だけ</b>を担い、実 shader への反映は <code>MaterialChangeCallback</code>(raw 関数ポインタ + void*) 経由で外部が行う。非コピー / 非ムーブ・全 noexcept。",
  when: "モデルのマテリアルを試行錯誤で調整するパネルとして使う。モデル load 時に <code>SetMaterialSlotCount</code> で slot 数を設定し、callback を登録して変更を shader に流す。",
  sample: "AModelMaterialPanel mat;\nmat.Init();\nmat.SetMaterialSlotCount(3);          // モデルの slot 数\nmat.SetOnMaterialChangeCallback(&amp;OnMatChanged, nullptr);\nworkspace.RegisterPanel(&amp;mat);\n\nif (FMaterialOverride* ov = mat.GetOverrideMutable(0)) {\n    ov-&gt;roughness = 0.2f;\n    ov-&gt;is_overridden = true;          // panel 自身は変更検知しない\n}",
  members: [
    { sig: "using MaterialChangeCallback = void (*)(void* user, u32 slot_index, const FMaterialOverride& override) noexcept", ret: "型", desc: "override 1 件分の変更通知 callback 型。<code>user</code> は登録時のポインタがそのまま戻る。<code>override</code> は panel が保持する最新コピー。", when: "—" },
    { sig: "void Init() noexcept", ret: "—", desc: "slot list を空に戻し、callback は維持したまま dirty / state をクリア。多重呼び出し可。", when: "Workspace 登録前 / 単独利用時。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "slot list を解放し callback を解除。多重呼び出し可。", when: "任意" },
    { sig: "void SetMaterialSlotCount(u32 count) noexcept", ret: "—", desc: "TArray を <code>count</code> 個に Resize し各 slot の <code>slot_index</code> を 0..count-1 でフィル。既存値は維持されず default 初期化(古い override が新モデルに流れない)。count=0 で全 slot 解除。<code>kMaxSlots</code> で clamp。", when: "モデル load 時。" },
    { sig: "void ResetSlot(u32 slot_index) noexcept", ret: "—", desc: "指定 slot を「元の material に戻す」(<code>is_overridden=false</code> + 各値 default)。範囲外は no-op。callback も発火する。", when: "1 slot をリセットする時。" },
    { sig: "void ResetAll() noexcept", ret: "—", desc: "全 slot を ResetSlot 相当に。callback は slot 数だけ発火する。", when: "全リセット時。" },
    { sig: "bool IsSlotOverridden(u32 slot_index) const noexcept", ret: "真偽", desc: "指定 slot が override 状態か。範囲外は false。", when: "任意" },
    { sig: "const FMaterialOverride* GetOverride(u32 slot_index) const noexcept", ret: "要素 or nullptr", desc: "指定 slot の override(const)。範囲外 / 未 Init は <code>nullptr</code>。", when: "値を読む時。" },
    { sig: "FMaterialOverride* GetOverrideMutable(u32 slot_index) noexcept", ret: "要素 or nullptr", desc: "指定 slot の override(mutable)。範囲外 / 未 Init は <code>nullptr</code>。外部から書き換える際は <code>is_overridden=true</code> を立てるか手動で callback を呼ぶ責務がある(panel は変更検知しない)。", when: "外部から値を書く時。" },
    { sig: "u32 SlotCount() const noexcept", ret: "件数", desc: "現在の slot 数(SetMaterialSlotCount の最終値)。", when: "任意" },
    { sig: "void SetOnMaterialChangeCallback(MaterialChangeCallback cb, void* user) noexcept", ret: "—", desc: "変更通知 callback を登録。<code>nullptr</code> で解除。発火は DrawUI 内で field が変わった時 + ResetSlot / ResetAll 時。1 frame で複数 slot 変更なら複数回。", when: "shader 反映 / Undo 連携時。" },
    { sig: "const char* Title() const noexcept override", ret: "\"Material Override\"", desc: "ImGui ウインドウタイトル兼 ID。", when: "基底からの呼び出し。" },
    { sig: "void DrawUI() noexcept override", ret: "—", desc: "ImGui::Begin(\"Material Override\") で start し、各 slot を CollapsingHeader 内に展開(Override enabled / Base Color / Metallic / Roughness / Normal / AO / Emissive / Emissive Intensity / Reset)。", when: "毎フレーム。" },
    { sig: "static constexpr f32 kMinMetallic=0 / kMaxMetallic=1", desc: "Metallic SliderFloat の値域(Phase 21b 暫定)。" },
    { sig: "static constexpr f32 kMinRoughness=0 / kMaxRoughness=1", desc: "Roughness SliderFloat の値域。" },
    { sig: "static constexpr f32 kMinNormalStrength=0 / kMaxNormalStrength=2", desc: "Normal Strength SliderFloat の値域。" },
    { sig: "static constexpr f32 kMinAoStrength=0 / kMaxAoStrength=1", desc: "AO Strength SliderFloat の値域。" },
    { sig: "static constexpr f32 kMinEmissiveIntensity=0 / kMaxEmissiveIntensity=10", desc: "Emissive Intensity SliderFloat の値域。" },
    { sig: "static constexpr u32 kMaxSlots = 256u", desc: "slot 上限(UI と内部の安全弁)。SetMaterialSlotCount に大きすぎる値が来ても clamp される。" },
        { sig: "using FModelMaterialPanel = AModelMaterialPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AModelMaterialPanel</code> を使う。" }
      ]
},
{
  name: "EAnimationPlayState",
  kind: "列挙型(enum class : u8)", header: "gameframework/tools/modelview/ModelAnimationPanel.h",
  summary: "<code>AModelAnimationPanel</code> の<b>再生状態</b>を表す 3 状態 enum。Phase 19a の E-prefix 規約(<code>enum class E*</code> + 基底 <code>u8</code>)に従う。",
  when: "<code>PlayState()</code> の戻り値や再生制御の状態判定に使う。",
  sample: "if (anim.PlayState() == EAnimationPlayState::Playing) {\n    anim.Pause();\n}",
  members: [
    { sig: "Stopped = 0", desc: "未再生(<code>m_CurrentTime = 0</code>、Play で頭から開始)。" },
    { sig: "Playing = 1", desc: "再生中(Tick(dt) で <code>m_CurrentTime += dt * speed</code>)。" },
    { sig: "Paused = 2", desc: "一時停止(<code>m_CurrentTime</code> 保持、Play で同位置から再開)。" }
  ]
},
{
  name: "FAnimationClipBinding",
  kind: "構造体(POD)", header: "gameframework/tools/modelview/ModelAnimationPanel.h",
  summary: "モデルに含まれる 1 個の animation clip の<b>メタ情報</b>。<code>AModelAnimationPanel::SetClips</code> で外部から渡す POD で、panel は値コピーで内部 TArray に保持する(呼出側はビルド済み temp 配列を渡してよい)。<code>name</code> はコピー所有しないため寿命は呼出側責任。",
  when: "モデル load 時に clip 一覧を組み立てて <code>SetClips</code> に渡す。<code>clip_index</code> は外部 CAnimationPlayer のクリップ ID で、callback にそのまま返る。",
  sample: "FAnimationClipBinding clips[] = {\n    { \"Idle\", 2.0f, true,  0 },\n    { \"Walk\", 1.2f, true,  1 },\n    { \"Jump\", 0.8f, false, 2 },\n};\nanim.SetClips(clips, 3);",
  members: [
    { sig: "const char* name = nullptr", desc: "clip 表示名。const char* リテラル / 永続バッファ想定(panel はコピー所有しない)。" },
    { sig: "f32 duration_sec = 0.0f", desc: "clip の長さ(秒)。0 以下が来た場合は内部で clamp される。" },
    { sig: "bool is_looping = false", desc: "clip 既定のループ可否。UI の Loop checkbox で override 可能。" },
    { sig: "u32 clip_index = 0u", desc: "外部 CAnimationPlayer 内のクリップ ID。callback にそのまま渡される。panel は中身を解釈しない。" }
  ]
},
{
  name: "AModelAnimationPanel",
  kind: "クラス(AEditorPanel 派生)", header: "gameframework/tools/modelview/ModelAnimationPanel.h",
  summary: "ModelViewer 内でロード済みモデルの <b>animation clip 再生制御 + timeline</b> を行うパネル。<t>AEditorPanel</t> を継承し、clip dropdown / Play / Pause / Stop / Time slider / Loop checkbox / Speed slider / BlendWeight slider を提供する。<b>時刻の進行 + clip 選択 + UI control だけ</b>を持ち、実 bone palette 計算 / GPU 反映は <code>OnFrameCallback</code>(raw 関数ポインタ + void*) 経由で外部 renderer + CAnimationPlayer に委譲する。時刻進行は <code>Tick(dt)</code> に分離(DrawUI は描画専任)。非コピー / 非ムーブ・全 noexcept。",
  when: "モデルのアニメーションを切り替えて再生確認するパネルとして使う。<code>SetClips</code> で clip 一覧を渡し、callback を登録して毎フレーム <code>Tick(dt)</code> を呼ぶと、終端で外部に時刻が通知される。",
  sample: "AModelAnimationPanel anim;\nanim.Init();\nworkspace.RegisterPanel(&amp;anim);\n\nFAnimationClipBinding clips[] = { { \"Idle\", 2.0f, true, 0 } };\nanim.SetClips(clips, 1);\nanim.SelectClip(0);\nanim.Play();\n\n// 毎フレーム:\nanim.Tick(dt);    // Playing 中は m_CurrentTime += dt * speed",
  members: [
    { sig: "using AnimationFrameCallback = void (*)(void* user, u32 clip_index, f32 time_sec) noexcept", ret: "型", desc: "Tick 終端で呼ばれる callback 型。<code>clip_index</code> は <code>FAnimationClipBinding::clip_index</code>(外部 CAnimationPlayer ID)。std::function 禁止のため raw 関数ポインタ規約。", when: "—" },
    { sig: "void Init() noexcept", ret: "—", desc: "内部 state をデフォルト、clip リストを空、selection / time を 0、callback も nullptr に(= 完全リセット)。多重呼び出し可。", when: "生成直後 / 再利用時。" },
    { sig: "void Shutdown() noexcept", ret: "—", desc: "内部 state を全解放(明示 Clear で再 Init の確定状態を作る)。多重呼び出し可。", when: "任意" },
    { sig: "void SetClips(const FAnimationClipBinding* clips, u32 count) noexcept", ret: "—", desc: "clip メタ一覧を push。内部 TArray を <code>count</code> 個に Resize し値コピー。<code>count==0</code> で空。selection は自動的に index 0(count==0 なら -1)、時刻 / state も Stopped + 0 にリセット。", when: "モデル load / 切替時。" },
    { sig: "void ClearClips() noexcept", ret: "—", desc: "clip リストを完全に空に(selection=-1、state=Stopped、time=0)。callback はクリアしない(Init / Shutdown と区別)。", when: "モデル unload 時。" },
    { sig: "u32 ClipCount() const noexcept", ret: "件数", desc: "現在登録されている clip の数。", when: "任意" },
    { sig: "const FAnimationClipBinding* CurrentClip() const noexcept", ret: "要素 or nullptr", desc: "現在選択中の clip メタ(read-only)。未選択 / 範囲外は <code>nullptr</code>。", when: "任意" },
    { sig: "i32 CurrentClipIndex() const noexcept", ret: "index", desc: "現在選択中の clip index(ClipCount() 未満、未選択は <code>kNoClipSelected</code>= -1)。", when: "任意" },
    { sig: "void SelectClip(u32 clip_index) noexcept", ret: "—", desc: "clip を選択。範囲外(>= ClipCount())は no-op。selection 切替時は時刻を 0 にリセット + Stopped に戻す。", when: "clip を切り替える時。" },
    { sig: "void Play() noexcept", ret: "—", desc: "現在位置から再生開始。clip 未選択(-1)なら no-op。Stopped からは 0 から、Paused からは <code>m_CurrentTime</code> 維持で再開。", when: "再生開始時。" },
    { sig: "void Pause() noexcept", ret: "—", desc: "一時停止(<code>m_CurrentTime</code> 保持)。Playing 以外からは no-op。", when: "一時停止時。" },
    { sig: "void Stop() noexcept", ret: "—", desc: "停止(<code>m_CurrentTime = 0</code> + state=Stopped)。Stopped でも安全。", when: "停止時。" },
    { sig: "EAnimationPlayState PlayState() const noexcept", ret: "状態", desc: "現在の再生状態(Stopped / Playing / Paused)。", when: "任意" },
    { sig: "f32 CurrentTimeSec() const noexcept", ret: "秒", desc: "現在の再生位置(秒)。Stopped 時は 0、Playing / Paused 時は最新 Tick 結果 or SetCurrentTimeSec の値。", when: "任意" },
    { sig: "void SetCurrentTimeSec(f32 t) noexcept", ret: "—", desc: "再生位置を直接設定(Time slider / スクラブ用)。0 未満は 0、duration 超過は duration にクランプ(clip 未選択時は 0 のまま no-op)。state は変更しない。", when: "スクラブ操作時。" },
    { sig: "f32 PlaybackSpeed() const noexcept", ret: "倍率", desc: "現在の再生速度。Tick で <code>m_CurrentTime += dt * speed</code> に乗る。", when: "任意" },
    { sig: "void SetPlaybackSpeed(f32 speed) noexcept", ret: "—", desc: "再生速度を設定。<code>[0.1, 4.0]</code> にクランプ。0 は実質一時停止だが下限 0.1 を強制。負値(逆再生)は Phase 21b 範囲外。", when: "速度変更時。" },
    { sig: "bool IsLoopingOverride() const noexcept / void SetLoopingOverride(bool b) noexcept", ret: "—", desc: "UI の Loop checkbox 状態。true なら clip 側 <code>is_looping</code> を無視して強制 loop、false なら clip 既定に従う。", when: "ループ切替時。" },
    { sig: "f32 BlendWeight() const noexcept / void SetBlendWeight(f32 w) noexcept", ret: "—", desc: "blend weight <code>[0,1]</code>(0 未満 / 1 超過はクランプ)。Phase 21b は単一 clip 再生のため表示用 + callback 経由で renderer が参考にする値。", when: "ブレンド重み設定時。" },
    { sig: "void Tick(f32 dt) noexcept", ret: "—", desc: "Playing 中は <code>m_CurrentTime += dt * m_Speed</code>。duration 到達時、loop 有効なら wrap、無効なら duration に固定 + Stopped に遷移。終端で callback を発火。clip 未選択 / Stopped / Paused / <code>dt &lt;= 0</code> は no-op(巻き戻し非対応)。", when: "毎フレーム。" },
    { sig: "void SetOnFrameCallback(AnimationFrameCallback cb, void* user) noexcept", ret: "—", desc: "Tick 終端で 1 度呼ばれる callback を設定。<code>nullptr</code> で解除。引数は (user, clip_index, time_sec)。", when: "CAnimationPlayer 連携時。" },
    { sig: "const char* Title() const noexcept override", ret: "\"Animation\"", desc: "ImGui ウインドウタイトル兼 ID。固定リテラル。", when: "基底からの呼び出し。" },
    { sig: "void DrawUI() noexcept override", ret: "—", desc: "ImGui::Begin \"Animation\" + ClipCombo + Time slider + Play / Pause / Stop + Loop checkbox + Speed slider + BlendWeight slider を描画。<code>IsVisible()</code> が false なら早期 return。", when: "毎フレーム。" },
    { sig: "static constexpr f32 kMinPlaybackSpeed=0.1f / kMaxPlaybackSpeed=4.0f", desc: "Speed slider の範囲。SetPlaybackSpeed の clamp にも使う。" },
    { sig: "static constexpr i32 kNoClipSelected = -1", desc: "「未選択」を表す sentinel(i32 戻り値で使用)。" },
        { sig: "using FModelAnimationPanel = AModelAnimationPanel", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AModelAnimationPanel</code> を使う。" }
      ]
},
    {
      name: "CRegistry",
      kind: "クラス", header: "editor_abi/EditorCameraViewRequests.h",
      summary: "editor camera view requestを型ごとに登録し、呼び出し側へ同じ窓口を提供する。",
      when: "editor camera viewの処理を登録または取得する時。",
      members: [
        { sig: "using FRegistry = CRegistry", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CRegistry</code> を使う。" }
      ]
    },
    {
      name: "CBtActionRegistry",
      kind: "クラス", header: "gameframework/tools/btedit/BtActionRegistry.h",
      summary: "behavior tree editorで選べるaction factoryを登録する。",
      when: "action nodeの候補を列挙または生成する時。",
      members: [
        { sig: "using FBtActionRegistry = CBtActionRegistry", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CBtActionRegistry</code> を使う。" }
      ]
    },
    {
      name: "CBtConditionRegistry",
      kind: "クラス", header: "gameframework/tools/btedit/BtCatalog.h",
      summary: "behavior tree editorで選べるcondition factoryを登録する。",
      when: "condition nodeの候補を列挙または生成する時。",
      members: [
        { sig: "using FBtConditionRegistry = CBtConditionRegistry", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CBtConditionRegistry</code> を使う。" }
      ]
    },
    {
      name: "ABtCompareNode",
      kind: "クラス", header: "gameframework/tools/btedit/BtGuardNodes.h",
      summary: "blackboard値の比較条件を保持して評価するbehavior tree node。",
      when: "値比較をguardとしてtreeへ配置する時。",
      members: [
        { sig: "using FBtCompareNode = ABtCompareNode", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ABtCompareNode</code> を使う。" }
      ]
    },
    {
      name: "ABtConditionNode",
      kind: "クラス", header: "gameframework/tools/btedit/BtGuardNodes.h",
      summary: "登録済みconditionを呼び出して評価するbehavior tree node。",
      when: "再利用可能なconditionをtreeへ配置する時。",
      members: [
        { sig: "using FBtConditionNode = ABtConditionNode", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ABtConditionNode</code> を使う。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
"ギズモ": "viewport 上で選択 ANode の Transform(移動 / 回転 / 拡縮)を直接ドラッグ操作するハンドル。ACS では CEditorGizmo が実装する。",
"ワークスペース": "複数のエディタ panel を 1 つの editor アプリとしてまとめ、ライフサイクル・メインループ・レイアウト永続化・イベント broadcast を統括する中央 hub。ACS では CEditorWorkspace。",
"orbit": "3D カメラが注視点(target)を中心に yaw / pitch / distance で周回する操作モデル(Maya / Blender 風)。",
"dolly": "カメラを前後に寄せる / 引く操作。3D では target からの distance、2D では zoom_2d を変化させる。",
"Drag Source": "ImGui のドラッグ &amp; ドロップで、つかんで運ばれる側(payload を提供する元)。受け側は BeginDragDropTarget で受け取る。",
"payload": "ImGui のドラッグ &amp; ドロップで運ばれるデータ本体。ACS の CAssetBrowser では \"ASSET_PATH\" id で wchar_t* を渡す。",
"drawer": "inspector 上で 1 つの property を描画する関数。ACS では DrawerFn(関数ポインタ)で表現し CPropertyDrawerRegistry に登録する。",
"preset": "テーマの色 / spacing / corner をまとめた既定スナップショット。ACS では EEditorThemePreset(Dark / DarkBlue / Light など)。",
"accent": "UI の強調色(CheckMark / SliderGrab / TabActive など)。ACS のブランド色相当。",
"merge(コマンド)": "連続 drag のような細かい操作を 1 件の undo エントリにまとめること。AEditorCommand の CanMerge / MergeWith で判定・統合する。",
"FieldChangeCallback": "AInspectorPanel が field 編集時に外部へ変更を通知する raw 関数ポインタ型 (undo / 永続化への委譲口)。",
"SaveSceneCallback": "AEditorToolbar の Save ボタン押下で発火する raw 関数ポインタ型。serialize 先は user 側で決める。",
"EEditorState": "AEditorToolbar が持つ editor の再生状態列挙 (Playing / Paused / Stepping)。",
"CInspectorSeam": "ANode から FInspectableObject / FInspectableField 配列を解決する seam。AInspectorPanel が provider 解決元として消費する。",
"fallback chain": "複数の font face を優先順位付きで並べ、ある文字が前の face に無ければ次の face で補う仕組み。CJK や emoji を別 face で埋める用途に使う。",
"メタミラー": "実体オブジェクト (ここでは CBehaviorTree) を直接覗けない時に、ユーザが構造情報 (親 id / 種別 / 名前) を別途 push して作る描画用のミラー (写し)。CBehaviorTree エディタが RTTI 無効下で tree を可視化するために使う。",
"FSpriteFrame": "sprite atlas 内の 1 枚分の矩形領域 (x/y/w/h + pivot + 名前) を表す単位。FSpritePack が名前付きで複数保持する。",
"pivot": "スプライトの基準点 (回転・配置の原点)。0〜1 の正規化座標で (0.5,0.5)=中央、(0,0)=左上 を表す。",
"IBL": "Image-Based Lighting。環境マップ(パノラマ等)を光源として使う間接照明手法。物体の周囲を映り込ませ、より自然な陰影を得る。",
"AEditorPanel": "ACS の editor_core が提供するエディタパネルの抽象基底クラス。Title / DrawUI / OnInit などを override し、CEditorWorkspace に登録して使う。",
"CEditorCamera": "editor_core のエディタ用カメラ。3D orbit / pan / dolly 操作と ViewMatrix / ProjectionMatrix の取得を提供する。各パネルが値メンバとして内包する Unity SceneView 風モデル。",
"CAssetBrowser": "editor_core のアセットブラウザパネル。ファイル選択を OnAssetSelected で各パネルに通知し、拡張子で mesh / texture などに分類する。"
});
