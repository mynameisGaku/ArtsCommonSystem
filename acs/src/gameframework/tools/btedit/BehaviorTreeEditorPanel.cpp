// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — btedit / FBehaviorTreeEditorPanel 実装 (Phase 22)
//
// 仕様の意図は FBehaviorTreeEditorPanel.h を参照。本ファイルでは:
//   ・Init / Shutdown / Reset / autorun & step 制御
//   ・メタミラー (AddNode / SetNodeStatus / ClearNodes / NodeStatus)
//   ・履歴 ring buffer 操作 (kHistorySize 固定)
//   ・DrawUI: toolbar + history graph + 左 tree view + 右 node inspector
//   ・DrawTreeRecursive: parent_id → 子線形走査の再帰展開
// を実装する。すべて noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。
#include "gameframework/tools/btedit/BehaviorTreeEditorPanel.h"

#include <imgui.h>

#include <cstdio>   // std::snprintf (label 整形)

namespace acs::game::btedit {

// ============================================================================
// 定数 / ローカルヘルパ
// ============================================================================

// tree 再帰深度上限 (= 不正な parent_id による循環参照ガード)。
// 実 BT で 32 階層を超えることはまず無いが、64 まで余裕を取る。
static constexpr u32 kTreeRecursionLimit = 64u;

// EBtKind → 表示用リテラル ("Selector" / "FSequence" / "Action")。
static const char* KindLabel(EBtKind k) noexcept {
    switch (k) {
        case EBtKind::Selector: return "Selector";
        case EBtKind::Sequence: return "FSequence";
        case EBtKind::Action:   return "Action";
    }
    return "Unknown"; // 到達不能 (enum を u8 で広げない限り)
}

// EBtStatus → 表示用リテラル。
static const char* StatusLabel(EBtStatus s) noexcept {
    switch (s) {
        case EBtStatus::Success: return "Success";
        case EBtStatus::Failure: return "Failure";
        case EBtStatus::Running: return "Running";
    }
    return "Unknown";
}

// EBtStatus → PlotLines 用 float 値 (Success=1.0, Running=0.5, Failure=0.0)。
// 視覚的に「上に行くほど成功」「中段で実行中」「下が失敗」と読める並べ方。
static f32 StatusToPlotValue(EBtStatus s) noexcept {
    switch (s) {
        case EBtStatus::Success: return 1.0f;
        case EBtStatus::Running: return 0.5f;
        case EBtStatus::Failure: return 0.0f;
    }
    return 0.0f;
}

// "(unnamed)" 代替ラベル (caller が name = nullptr を渡してきた場合)。
static const char* SafeName(const char* name) noexcept {
    return (name != nullptr) ? name : "(unnamed)";
}

// id が _nodes の有効範囲内かをチェック。
static bool IsValidId(u32 id, usize node_count) noexcept {
    if (id == FBehaviorTreeEditorPanel::kInvalidId) return false;
    return static_cast<usize>(id) < node_count;
}

// ============================================================================
// StatusColor — EBtStatus を RGBA float に変換 (header から ImVec4 を隠す目的)
// ============================================================================
// Success = 緑 (0.2, 1.0, 0.3)、Failure = 赤 (1.0, 0.3, 0.3)、
// Running = 黄 (1.0, 1.0, 0.3) に固定。完全飽和ではなく落ち着いた色味にする
// (ダーク背景でも明るすぎず、ライト背景でも沈まないトーン)。
void FBehaviorTreeEditorPanel::StatusColor(EBtStatus s, f32 out_rgba[4]) noexcept {
    if (out_rgba == nullptr) return;
    switch (s) {
        case EBtStatus::Success:
            out_rgba[0] = 0.2f; out_rgba[1] = 1.0f; out_rgba[2] = 0.3f; out_rgba[3] = 1.0f; return;
        case EBtStatus::Failure:
            out_rgba[0] = 1.0f; out_rgba[1] = 0.3f; out_rgba[2] = 0.3f; out_rgba[3] = 1.0f; return;
        case EBtStatus::Running:
            out_rgba[0] = 1.0f; out_rgba[1] = 1.0f; out_rgba[2] = 0.3f; out_rgba[3] = 1.0f; return;
    }
    out_rgba[0] = 0.7f; out_rgba[1] = 0.7f; out_rgba[2] = 0.7f; out_rgba[3] = 1.0f;
}

// ============================================================================
// Init / Shutdown
// ============================================================================
void FBehaviorTreeEditorPanel::Init() noexcept {
    // メタミラー / 履歴 / selection / step counter を全 reset。
    _nodes.Clear();
    _history.Clear();
    _history.Resize(static_cast<usize>(kHistorySize)); // 60 frame 分を確保
    for (usize i = 0; i < _history.Size(); ++i) {
        _history[i] = static_cast<u8>(EBtStatus::Failure); // 初期は Failure
    }
    _history_head = 0;

    _tree       = nullptr; // BT は SetTree で後付け
    _autorun    = false;
    _step_count = 0;
    _selected   = kInvalidId;
    // callback はリセットしない (Init は state の reset、callback は別操作)。
}

void FBehaviorTreeEditorPanel::Shutdown() noexcept {
    // 多重 Shutdown 可。TArray は Clear() で要素破棄 + 容量保持。
    _nodes.Clear();
    _history.Clear();
    _history_head = 0;

    _tree       = nullptr;
    _autorun    = false;
    _step_count = 0;
    _selected   = kInvalidId;

    // callback も解除する (Shutdown は完全リセットの意味合い)。
    _step_cb    = nullptr;
    _step_user  = nullptr;
}

// ============================================================================
// SetTree / Reset
// ============================================================================
void FBehaviorTreeEditorPanel::SetTree(FBehaviorTree* tree) noexcept {
    _tree = tree;
    // 観察対象が変わったので step counter / 履歴 / 全 status を初期化する。
    // メタミラー (_nodes) は触らない (= 同構造で別インスタンスを観察する用途に
    // 対応するため、ユーザが ClearNodes を別途呼ばない限り維持する)。
    Reset();
}

void FBehaviorTreeEditorPanel::Reset() noexcept {
    _step_count   = 0;
    _history_head = 0;
    for (usize i = 0; i < _history.Size(); ++i) {
        _history[i] = static_cast<u8>(EBtStatus::Failure);
    }
    for (usize i = 0; i < _nodes.Size(); ++i) {
        _nodes[i].last_status = EBtStatus::Failure;
    }
    // autorun / selection / メタミラーは触らない (= ユーザ操作で変える物)。
}

// ============================================================================
// StepOnce / TickInternal / OnFrameBegin
// ============================================================================
void FBehaviorTreeEditorPanel::StepOnce() noexcept {
    // 0.016 sec (= 60 fps の 1 frame) を仕様で固定。Step は常に同じ dt で進めることで
    // ユーザが "今のステップ数 * 1/60 秒進んだ" と即座に解釈できるようにする。
    TickInternal(kStepDt);
}

void FBehaviorTreeEditorPanel::TickInternal(f32 dt) noexcept {
    if (_tree == nullptr) return;

    // ----- (1) 実 BT を 1 tick 進める -----
    EBtStatus root_status = EBtStatus::Failure;
    if (_step_cb != nullptr) {
        // callback に委譲 (ユーザが任意 blackboard を渡せる)。
        // callback 側で `tree->Tick(my_bb, dt)` を呼ぶ規約。戻り値は取れないので、
        // root status は callback が SetNodeStatus(root_id=0, ...) で push する想定。
        // ここでは _nodes[0].last_status を採用する fallback (= 履歴 graph 用)。
        _step_cb(_step_user, _tree, dt);
        if (!_nodes.IsEmpty()) {
            root_status = _nodes[0].last_status;
        }
    } else {
        // fallback: blackboard = nullptr で直接呼ぶ。
        // 戻り値を取れるのでそのまま履歴に push する。
        root_status = _tree->Tick(nullptr, dt);
        if (!_nodes.IsEmpty()) {
            // root の last_status も同期的に更新 (Inspector / TreeView 表示と
            // PlotLines の値を一致させるため)。
            _nodes[0].last_status = root_status;
        }
    }

    // ----- (2) 履歴 ring buffer に push -----
    if (!_history.IsEmpty()) {
        _history[_history_head] = static_cast<u8>(root_status);
        _history_head = (_history_head + 1u) % kHistorySize;
    }

    // ----- (3) step counter -----
    ++_step_count;
}

void FBehaviorTreeEditorPanel::OnFrameBegin(f32 dt) noexcept {
    // autorun ON のときだけ毎フレーム 1 tick 進める。実 dt を渡すことで
    // ゲームの fps に追従させる (Step は 0.016 固定だが autorun は別)。
    if (!_autorun) return;
    if (_tree == nullptr) return;
    if (dt <= 0.0f) return; // 0 dt スパイクで何もしない (= 一時停止と同じ)
    TickInternal(dt);
}

// ============================================================================
// selection
// ============================================================================
void FBehaviorTreeEditorPanel::SelectNode(u32 node_id) noexcept {
    if (!IsValidId(node_id, _nodes.Size())) {
        _selected = kInvalidId;
        return;
    }
    _selected = node_id;
}

// ============================================================================
// AddNode / SetNodeStatus / ClearNodes / NodeStatus
// ============================================================================
u32 FBehaviorTreeEditorPanel::AddNode(EBtKind kind, const char* name, u32 parent_id) noexcept {
    if (_nodes.Size() >= static_cast<usize>(kMaxNodes)) {
        // 上限到達は silent fail (= kInvalidId 返却で通知)。
        return kInvalidId;
    }

    // parent_id バリデーション: kInvalidId 以外で範囲外なら root 扱いに倒す。
    // (= 不正な parent でも panel が落ちないようにする防衛策)
    if (parent_id != kInvalidId && !IsValidId(parent_id, _nodes.Size())) {
        parent_id = kInvalidId;
    }

    NodeMeta n;
    n.id          = static_cast<u32>(_nodes.Size()); // 払い出し = 現在の末尾 index
    n.parent_id   = parent_id;
    n.kind        = kind;
    n.name        = name;
    n.last_status = EBtStatus::Failure; // 初期は Failure

    const u32 new_id = n.id;
    _nodes.PushBack(n);
    return new_id;
}

void FBehaviorTreeEditorPanel::SetNodeStatus(u32 node_id, EBtStatus status) noexcept {
    if (!IsValidId(node_id, _nodes.Size())) return;
    _nodes[static_cast<usize>(node_id)].last_status = status;
}

void FBehaviorTreeEditorPanel::ClearNodes() noexcept {
    _nodes.Clear();
    _selected = kInvalidId;
    // history / step counter / autorun は触らない (= ユーザの明示操作で変える物)。
}

EBtStatus FBehaviorTreeEditorPanel::NodeStatus(u32 node_id) const noexcept {
    if (!IsValidId(node_id, _nodes.Size())) return EBtStatus::Failure;
    return _nodes[static_cast<usize>(node_id)].last_status;
}

// ============================================================================
// SetOnStepCallback
// ============================================================================
void FBehaviorTreeEditorPanel::SetOnStepCallback(StepCallback cb, void* user) noexcept {
    _step_cb   = cb;
    _step_user = user;
}

// ============================================================================
// DrawTreeRecursive — 1 node を TreeNode で描画し、子を線形走査して再帰
// ============================================================================
// parent_id が node_id を指す子 node を _nodes 内で線形に探して再帰描画する。
// Action (= leaf) は ImGuiTreeNodeFlags_Leaf を付ける。Selectable と TreeNode を
// 兼ねるため `ImGuiTreeNodeFlags_OpenOnArrow` で「矢印クリックで展開、ラベル
// クリックで選択」にする (Unity Hierarchy と同形)。
void FBehaviorTreeEditorPanel::DrawTreeRecursive(u32 node_id, u32 depth) noexcept {
    if (depth >= kTreeRecursionLimit) {
        ImGui::TextDisabled("  (tree depth limit reached)");
        return;
    }
    if (!IsValidId(node_id, _nodes.Size())) return;

    const NodeMeta& node = _nodes[static_cast<usize>(node_id)];

    // 子検索 (= 線形走査で parent_id == node_id の要素があるか)。
    bool has_child = false;
    if (node.kind != EBtKind::Action) {
        // Action は仕様上 leaf。Selector / FSequence のみ子を持ち得る。
        for (usize i = 0; i < _nodes.Size(); ++i) {
            if (_nodes[i].parent_id == node_id) {
                has_child = true;
                break;
            }
        }
    }

    // TreeNode flag 設定。
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_SpanAvailWidth
                             | ImGuiTreeNodeFlags_DefaultOpen;
    if (!has_child) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (_selected == node_id) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    // ラベル整形: "[Kind] name  ● Status" (kind と status を視認しやすく)。
    char label[160];
    std::snprintf(label, sizeof(label),
                  "[%s] %s",
                  KindLabel(node.kind),
                  SafeName(node.name));

    // ID 衝突回避のため node_id で PushID。
    ImGui::PushID(static_cast<int>(node_id));

    // ラベル全体を status color で描画する (緑/赤/黄)。
    f32 col[4];
    StatusColor(node.last_status, col);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(col[0], col[1], col[2], col[3]));

    const bool open = ImGui::TreeNodeEx("##bt_tn", flags, "%s", label);

    ImGui::PopStyleColor();

    // ラベルクリックで選択 (TreeNode の click 領域に対する反応を捕捉)。
    // arrow click は TreeNode が消費するので、それ以外の click を selection に。
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        _selected = node_id;
    }

    // 同行右側に status バッジを SameLine 表示 ([Success/Failure/Running])。
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(col[0], col[1], col[2], col[3]),
                       "[%s]", StatusLabel(node.last_status));

    if (open && has_child) {
        for (usize i = 0; i < _nodes.Size(); ++i) {
            if (_nodes[i].parent_id == node_id) {
                DrawTreeRecursive(static_cast<u32>(i), depth + 1u);
            }
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

// ============================================================================
// DrawUI — toolbar + history graph + tree view + node inspector
// ============================================================================
void FBehaviorTreeEditorPanel::DrawUI() noexcept {
    if (!IsVisible()) return;

    if (!ImGui::Begin(Title(), &_visible)) {
        ImGui::End();
        return;
    }

    // ------------------------------------------------------------------------
    // (1) Toolbar 行: Reset / Step / Continuous(autorun) | Active / Step counter
    // ------------------------------------------------------------------------
    if (ImGui::Button("Reset")) {
        Reset();
    }
    ImGui::SameLine();
    // Step は tree 未設定なら disable (押しても no-op だが UX 改善)。
    const bool step_enabled = (_tree != nullptr);
    if (!step_enabled) ImGui::BeginDisabled();
    if (ImGui::Button("Step")) {
        StepOnce();
    }
    if (!step_enabled) ImGui::EndDisabled();

    ImGui::SameLine();
    // Continuous: autorun toggle。ON/OFF をラベルで明示。
    {
        bool autorun = _autorun;
        if (ImGui::Checkbox("Continuous", &autorun)) {
            _autorun = autorun;
        }
    }

    ImGui::SameLine();
    // Active node count: status != Failure な node 数を毎フレーム集計。
    // (= Inspector で「今いくつの node が Success/Running か」を一目で見る指標)
    u32 active = 0;
    for (usize i = 0; i < _nodes.Size(); ++i) {
        if (_nodes[i].last_status != EBtStatus::Failure) ++active;
    }
    ImGui::Text("| Active: %u  Step: %u",
                static_cast<unsigned>(active),
                static_cast<unsigned>(_step_count));

    ImGui::Separator();

    // ------------------------------------------------------------------------
    // (2) History graph: 60 frame の root status を PlotLines で表示
    // ------------------------------------------------------------------------
    // ring buffer を新→旧の時系列順に float へ展開する。
    // _history_head は「次に書き込む位置」 = "ちょうど 1 frame 前 + 1" なので、
    // PlotLines に対して `(head, head+1, ..., head+kHistorySize-1) mod size` の
    // 順に並べると左 → 右 = 古い → 新しい時系列になる。
    if (!_history.IsEmpty()) {
        f32 plot[kHistorySize];
        for (u32 i = 0; i < kHistorySize; ++i) {
            const u32 idx = (_history_head + i) % kHistorySize;
            const EBtStatus s = static_cast<EBtStatus>(_history[idx]);
            plot[i] = StatusToPlotValue(s);
        }
        // overlay 表示: 最新値の文字列ラベル。
        char overlay[32];
        const u32 last_idx = (_history_head + kHistorySize - 1u) % kHistorySize;
        std::snprintf(overlay, sizeof(overlay), "Root: %s",
                      StatusLabel(static_cast<EBtStatus>(_history[last_idx])));
        ImGui::PlotLines("##bt_history", plot, static_cast<int>(kHistorySize),
                         0, overlay,
                         0.0f, 1.0f, // y range [Failure=0 .. Success=1]
                         ImVec2(0.0f, 60.0f));
    }

    ImGui::Separator();

    // ------------------------------------------------------------------------
    // (3) 2 カラム: 左 Tree View / 右 Node Inspector
    // ------------------------------------------------------------------------
    const float content_w = ImGui::GetContentRegionAvail().x;
    const float left_w    = (content_w > 540.0f) ? content_w * 0.55f : content_w * 0.50f;

    // ===== 左カラム: Tree View =====
    ImGui::BeginChild("##bt_tree_left", ImVec2(left_w, 0), true);
    {
        ImGui::TextUnformatted("Behavior Tree");
        ImGui::Separator();

        if (_nodes.IsEmpty()) {
            ImGui::TextDisabled("(No nodes registered)");
            ImGui::TextDisabled("Call panel.AddNode(kind, name, parent_id) from your sample.");
        } else {
            // root (= parent_id == kInvalidId) を全て描画。
            // 一般的には root は 1 つだが、複数 root も許容 (= forest 表示)。
            bool drew_any = false;
            for (u32 i = 0; i < _nodes.Size(); ++i) {
                if (_nodes[i].parent_id == kInvalidId) {
                    DrawTreeRecursive(i, 0u);
                    drew_any = true;
                }
            }
            if (!drew_any) {
                ImGui::TextDisabled("(No root node — possible cycle)");
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ===== 右カラム: Node Inspector =====
    ImGui::BeginChild("##bt_inspector_right", ImVec2(0, 0), true);
    {
        ImGui::TextUnformatted("Node Inspector");
        ImGui::Separator();

        if (!IsValidId(_selected, _nodes.Size())) {
            ImGui::TextDisabled("(No node selected)");
            ImGui::TextDisabled("Click a node in the tree view.");
        } else {
            const NodeMeta& n = _nodes[static_cast<usize>(_selected)];

            // child count を線形走査でカウント (Action なら 0)。
            u32 child_count = 0;
            if (n.kind != EBtKind::Action) {
                for (usize i = 0; i < _nodes.Size(); ++i) {
                    if (_nodes[i].parent_id == _selected) ++child_count;
                }
            }

            ImGui::Text("Name     : %s", SafeName(n.name));
            ImGui::Text("Kind     : %s", KindLabel(n.kind));
            ImGui::Text("Id       : %u", static_cast<unsigned>(n.id));
            if (n.parent_id == kInvalidId) {
                ImGui::Text("Parent   : (root)");
            } else {
                ImGui::Text("Parent   : %u", static_cast<unsigned>(n.parent_id));
            }
            ImGui::Text("Children : %u", static_cast<unsigned>(child_count));

            ImGui::Separator();

            // Last Status (status color text)。
            f32 col[4];
            StatusColor(n.last_status, col);
            ImGui::TextUnformatted("Last Status : ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(col[0], col[1], col[2], col[3]),
                               "%s", StatusLabel(n.last_status));
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace acs::game::btedit
