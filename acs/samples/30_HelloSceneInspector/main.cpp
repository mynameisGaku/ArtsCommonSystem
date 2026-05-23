// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — GameFramework Phase 20 editor 第二弾: SceneInspector
//
// 動作:
//   ・GameFramework Pillar A の Scene 上に複数階層の Node2D ツリーを構築:
//        root
//        ├── wheel        (回転、SpokeBase 親)
//        │   ├── spoke[0] (オフセット)
//        │   └── spoke[1] (オフセット)
//        └── player       (IInspectableProvider 実装、Inspector で field 編集)
//   ・`inspector::HierarchyPanel _hierarchy_panel` が "Scene Hierarchy" window で
//     ツリーを描画。クリックで `inspector::SelectionService _selection` に選択を
//     伝播。
//   ・`inspector::InspectorPanel _inspector_panel` が "Inspector" window で
//     `_seam` 経由で選択 Node の Provider field を編集。
//   ・`inspector::EditorToolbar _toolbar` が "Editor Toolbar" window で
//     Play/Pause/Step 等のコマンドを発行。Toolbar の DrawUI は `Game&` を取り、
//     Quit 含むトップレベル操作を引き受けられる契約を想定。
//   ・main menu bar "File > Save Scene / Load Scene" で永続化フックを呼び出す
//     (Phase 20 では callback だけ走らせる stub。実シリアライザは Phase 21+ で)。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (samples/21_HelloImGui / 29_HelloParticleEditor
// と同じ理由で、ImGuiCtx が DX12 raw backend 経由のため)。
//
// 設計メモ:
//   ・ImGuiCtx は **Game クラスが所有**。`OnStart/OnShutdown/OnEvent` で
//     lifecycle を回し、`OnRender` を override して `NewFrame() → Scene OnRender
//     → Render()` の順で挟む。Scene 側はそのまま ImGui::* を呼べる。
//     (= sample 29_HelloParticleEditor と同形)。
//   ・Scene は `WantedServices()` を None のままにする (= scene clock 等の自動
//     tick は不要、Node2D::UpdateTree を直接回す)。
//   ・PlayerNode は `Node2D` を継承しつつ `IInspectableProvider` も多重継承する
//     構造を取る。これで HierarchyPanel で選択 → InspectorSeam 側の Provider と
//     紐付けが一行で済む (Phase K-3 の標準パターン)。
//   ・並列に作成中の inspector ヘッダ群:
//        gameframework/tools/inspector/HierarchyPanel.h
//        gameframework/tools/inspector/InspectorPanel.h
//        gameframework/tools/inspector/EditorToolbar.h     (本サンプル想定)
//        gameframework/tools/inspector/SelectionService.h
//     を include。EditorToolbar.h は parallel agent でまだ未確定だが、
//     `Init() / DrawUI(Game&) / Shutdown()` の最小 3 関数 + non-copy / 全 noexcept
//     を仮定して書いている。
//   ・NodeId は Node2D::Id() で取得。default `NodeId{}` は invalid。サンプル
//     では PlayerNode に手動で `_SetId(NodeId{1, 1})` を割り振り、Provider 解決
//     ロジックの最小契約を満たす。本番では NodePool が generational 発行を担う。

#include "gameframework/GameFramework.h"
#include "gameframework/InspectorSeam.h"
#include "gameframework/tools/inspector/HierarchyPanel.h"
#include "gameframework/tools/inspector/InspectorPanel.h"
#include "gameframework/tools/inspector/EditorToolbar.h"
#include "gameframework/tools/inspector/SelectionService.h"

#include "imgui/ImGuiContext.h"
#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

// ----------------------------------------------------------------------------
// PlayerNode — Node2D かつ IInspectableProvider (= Inspector に field を公開)
// ----------------------------------------------------------------------------
// `IInspectableProvider` の典型実装。fields は static 配列で保有し、`GetObject`
// で配列ポインタごと返す。`OnFieldChanged` では値の clamp / 再バリデーション
// だけ行う想定 (現状は Log だけ出す)。
class PlayerNode : public Node2D, public IInspectableProvider {
public:
    PlayerNode() noexcept = default;

    void OnSpawn() noexcept override {
        ACS_LOG_INFO("[SceneInspector] PlayerNode spawned (provider exposed)");
    }
    void OnDespawn() noexcept override {
        ACS_LOG_INFO("[SceneInspector] PlayerNode despawn");
    }

    // ---- IInspectableProvider 実装 ----
    u32 ObjectCount() noexcept override { return 1; }

    InspectableObject GetObject(u32 /*index*/) noexcept override {
        // fields 配列の所有は Provider (= 本 PlayerNode インスタンス)。
        // InspectorSeam はコピーしない (InspectorSeam.h §53 設計選択)。
        _fields[0] = { "speed",      EFieldKind::F32,  &_speed,      0, nullptr };
        _fields[1] = { "hp",         EFieldKind::I32,  &_hp,         0, nullptr };
        _fields[2] = { "invincible", EFieldKind::Bool, &_invincible, 0, nullptr };
        _fields[3] = { "color",      EFieldKind::Vec3, &_color,      0, nullptr };
        _fields[4] = { "position",   EFieldKind::Vec2, &_position,   0, nullptr };
        InspectableObject obj{};
        obj.type_name     = "Player";
        obj.instance_name = "P1";
        obj.fields        = _fields;
        obj.field_count   = kFieldCount;
        return obj;
    }

    void OnFieldChanged(u32 /*obj_index*/, u32 field_index) noexcept override {
        // 値変更通知: 現状は log のみ。本番では clamp / 派生値再計算をここで行う。
        const char* name = (field_index < kFieldCount) ? _fields[field_index].name : "?";
        ACS_LOG_INFO("[SceneInspector] PlayerNode field changed: %s", name);
    }

private:
    static constexpr u32 kFieldCount = 5;

    // Inspector で編集される public-ish state (`InspectableField::data` が指す)。
    f32  _speed      = 5.0f;
    i32  _hp         = 100;
    bool _invincible = false;
    Vec3 _color      { 0.2f, 0.8f, 0.3f };
    Vec2 _position   { 0.0f, 0.0f };

    // 永続所有 fields (GetObject が毎回同じ配列のポインタを返す)。
    InspectableField _fields[kFieldCount]{};
};

// ----------------------------------------------------------------------------
// WheelNode — 回転する Node2D サブクラス (Hierarchy で複数階層を見せるため)
// ----------------------------------------------------------------------------
class WheelNode : public Node2D {
public:
    explicit WheelNode(f32 speed_rps) noexcept : _speed(speed_rps) {}
    void OnUpdate(f32 dt) noexcept override {
        Local().rotation += _speed * dt;
    }
private:
    f32 _speed;
};

// ----------------------------------------------------------------------------
// SceneInspectorScene — Node2D ツリー + 4 panel + Inspector seam を持つシーン
// ----------------------------------------------------------------------------
class SceneInspectorScene : public Scene {
public:
    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnRender(RenderContext& rc) noexcept override;

private:
    // File メニュー stub の保存先 (現状 callback だけ走らせるため未使用)。
    static constexpr const char* kScenePath = "scene.acscene";

    // Node2D ツリーのルート。ChildCount() == 2 (wheel, player) になる。
    Node2D                       _root_node;
    // 弱参照: ResolveStructuralChanges 後に dangling になり得るが、本サンプルでは
    // OnExit 以外で Destroy を呼ばないので OnUpdate 中は安全。
    PlayerNode*                  _player      = nullptr;
    WheelNode*                   _wheel       = nullptr;
    Node2D*                      _spoke[2]    = { nullptr, nullptr };

    // Editor panels (4 つ) + Inspector seam + Selection service。
    inspector::HierarchyPanel    _hierarchy_panel;
    inspector::InspectorPanel    _inspector_panel;
    inspector::EditorToolbar     _toolbar;
    inspector::SelectionService  _selection;
    InspectorSeam                _seam;
};

void SceneInspectorScene::OnEnter() noexcept {
    // 背景色 (editor らしいニュートラルグレー)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    // ---- Node2D ツリー組立: root → wheel → spoke[0/1] + root → player ----
    // wheel: 30 deg/s で回転
    auto wheel_up = MakeUnique<WheelNode>(0.5f /*rad/s*/);
    wheel_up->Local().position = Vec2{ 4.0f, 0.0f };
    wheel_up->_SetId(NodeId{ 2u, 1u });
    Node2D& wheel_ref = _root_node.AddChild(Move(wheel_up));
    _wheel = static_cast<WheelNode*>(&wheel_ref);

    // spoke[0]
    auto sp0_up = MakeUnique<Node2D>();
    sp0_up->Local().position = Vec2{ 2.0f, 0.0f };
    sp0_up->_SetId(NodeId{ 3u, 1u });
    _spoke[0] = &wheel_ref.AddChild(Move(sp0_up));

    // spoke[1]
    auto sp1_up = MakeUnique<Node2D>();
    sp1_up->Local().position = Vec2{ 0.0f, 2.0f };
    sp1_up->_SetId(NodeId{ 4u, 1u });
    _spoke[1] = &wheel_ref.AddChild(Move(sp1_up));

    // player (Inspector 編集対象、root の直下、wheel と兄弟)
    auto player_up = MakeUnique<PlayerNode>();
    player_up->Local().position = Vec2{ -4.0f, 0.0f };
    // NodeId{1, 1}: index=1 (root が将来 0 に振られる想定)、generation=1。
    player_up->_SetId(NodeId{ 1u, 1u });
    Node2D& player_ref = _root_node.AddChild(Move(player_up));
    _player = static_cast<PlayerNode*>(&player_ref);
    // root 自体にも id を割り振っておく (Hierarchy の根クリック対応)。
    _root_node._SetId(NodeId{ 0u, 1u });

    // ---- InspectorSeam に Player Provider を登録 ----
    // 本サンプルでは Player のみ Provider を実装。他ノードは Inspector で
    // "(No provider)" 表示になる契約。
    _seam.Init();
    _seam.RegisterProvider(_player);

    // ---- SelectionService 初期化 + 4 panel への注入 ----
    _selection.Init();
    _hierarchy_panel.Init();
    _hierarchy_panel.SetSelectionService(&_selection);
    _inspector_panel.Init();
    _inspector_panel.SetSelectionService(&_selection);

    // ---- Toolbar 初期化 ----
    // EditorToolbar の最小契約: `Init()` で内部状態を default に戻す。
    // Play/Pause/Step トグルは内部で持つ想定。
    _toolbar.Init();

    // 起動時の selection を Player に向けておく (UX: 最初から Inspector に
    // 値が見えるようにする)。
    _selection.SelectNode(_player->Id());

    ACS_LOG_INFO("[SceneInspector] entered (root + wheel + 2 spokes + player, selection=Player)");
}

void SceneInspectorScene::OnExit() noexcept {
    // 4 panel + service + seam を逆順に shutdown / clear。Provider 自体は
    // PlayerNode が握っているので、seam 側は登録解除だけ行う (= ClearAll)。
    _toolbar.Shutdown();
    _inspector_panel.Shutdown();
    _hierarchy_panel.Shutdown();
    _selection.ClearAll();
    _seam.ClearAll();

    // ツリーを破棄 (Phase 5 標準パターン)。
    for (u32 i = 0; i < _root_node.ChildCount(); ++i) {
        if (auto* c = _root_node.Child(i)) c->Destroy();
    }
    _root_node.ResolveStructuralChanges();

    ACS_LOG_INFO("[SceneInspector] exited");
}

void SceneInspectorScene::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }

    // dt の安全 clamp (大 dt スパイクで wheel 回転が暴れない)。
    if (dt > 0.1f) dt = 0.1f;

    // ツリー全体に dt を伝播 (wheel が回り、spoke の World() が変化する)。
    _root_node.UpdateTree(dt);
    _root_node.ResolveStructuralChanges();

    // 現選択ノードを掴む (= sample 仕様 §5)。SelectionService が source of truth。
    const NodeId selected_id = _selection.CurrentSelection();
    // (現状 selected_id は Hierarchy → SelectionService → Inspector で使う。
    // OnUpdate 内では log 出力など以外には未使用なので参照だけ。)
    (void)selected_id;
}

void SceneInspectorScene::OnRender(RenderContext& /*rc*/) noexcept {
    // ---- main menu bar: File > Save / Load (stub) ----
    // ImGui の draw コマンドは Game::OnRender の NewFrame() と Render() の間で
    // 発行される。Scene::OnRender はその内側なのでそのまま ImGui::* を呼べる。
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Scene")) {
                // Phase 20: serializer は未実装。callback hook だけ走らせる stub。
                // 将来は `SceneSerializer::Save(kScenePath, _root_node)` を呼ぶ。
                ACS_LOG_INFO("[SceneInspector] Save Scene -> '%s' (stub, no-op)", kScenePath);
            }
            if (ImGui::MenuItem("Load Scene")) {
                // Phase 20: serializer は未実装。callback hook だけ走らせる stub。
                ACS_LOG_INFO("[SceneInspector] Load Scene <- '%s' (stub, no-op)", kScenePath);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) {
                GetGame().Quit();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // ---- Editor Toolbar: Play / Pause / Step / 等の global コマンド ----
    // EditorToolbar の DrawUI は `Game&` を取り、Quit 含む top-level 操作を
    // 受け持つ契約 (= 仕様メモ §4 の依頼)。
    _toolbar.DrawUI(GetGame());

    // ---- Hierarchy: Node2D ツリー描画 + 選択操作 ----
    // root 配下を再帰 TreeNode で表示。クリック → SelectionService に伝播 →
    // Inspector が次フレームで反映する。
    _hierarchy_panel.DrawUI(_root_node);

    // ---- Inspector: 選択 Node の field を Provider 経由で編集 ----
    // 第 2 引数 selected_id は SelectionService 注入済みなので fallback 用。
    // 注入時は InspectorPanel が SelectionService::CurrentSelection() を優先採用する。
    _inspector_panel.DrawUI(_seam, _selection.CurrentSelection());
}

// ----------------------------------------------------------------------------
// SceneInspectorGame — ImGui lifecycle を Game に持たせる薄いラッパ
// ----------------------------------------------------------------------------
// 構造は sample 29_HelloParticleEditor と完全に同形。OnRender で NewFrame と
// Render を Scene::OnRender の両側に挟むのが key (= Scene 側が ImGui::* を
// そのまま呼べるようにする)。
class SceneInspectorGame : public Game {
public:
    void OnStart() noexcept override {
        // ImGui を Window + Renderer に紐付け。失敗時は早期 Quit。
        if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
            ACS_LOG_ERROR("[SceneInspectorGame] ImGuiCtx.Init failed -> Quit");
            Quit();
            return;
        }
        // 基底の OnStart は InitialScene() を push する。
        Game::OnStart();
    }

    void OnRender() noexcept override {
        // ImGui フレーム開始 → Scene::OnRender で ImGui::* が呼ばれる →
        // ImGui の描画コマンドをコマンドリストに発行、の順。
        _imgui.NewFrame();
        Game::OnRender();
        _imgui.Render();
    }

    void OnShutdown() noexcept override {
        // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
        // ないことを保証)。
        Game::OnShutdown();
        _imgui.Shutdown();
    }

    void OnEvent(const Event& e) noexcept override {
        _imgui.OnEvent(e);
        Game::OnEvent(e);
    }

protected:
    UniquePtr<Scene> InitialScene() noexcept override {
        return MakeUnique<SceneInspectorScene>();
    }

private:
    ImGuiCtx _imgui;
};

ACS_GAME_MAIN(SceneInspectorGame)
