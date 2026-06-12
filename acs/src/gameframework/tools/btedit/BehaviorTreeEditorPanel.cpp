// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — btedit / FBehaviorTreeEditorPanel 実装
//
// 仕様の意図は FBehaviorTreeEditorPanel.h を参照。本ファイルでは:
//   ・Init / Shutdown / Reset / autorun & step 制御
//   ・メタミラー (AddNode / SetNodeStatus / ClearNodes / NodeStatus)
//   ・履歴 ring buffer 操作 (kHistorySize 固定)
//   ・DrawUI: toolbar + history graph + 左 tree view + 右 node inspector
//   ・DrawTreeRecursive: parent_id → 子線形走査の再帰展開
// を実装する。すべて noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。
#include "gameframework/tools/btedit/BehaviorTreeEditorPanel.h"
#include "gameframework/tools/btedit/BtGuardNodes.h"   // FBtConditionNode / FBtCompareNode (bake)

#include <imgui.h>

#include <cstdio>   // std::snprintf / fopen / fgets / sscanf (label 整形・保存読込)
#include <cmath>    // floorf (グリッド描画)
#include <cstring>  // strncmp (保存ヘッダ判定)

namespace acs::game::btedit {

/**
 * tree 再帰描画の深度上限 (= 不正な parent_id による循環参照ガード)。
 *
 * @details 実 BT で 32 階層を超えることはまず無いが、64 まで余裕を取る。
 */
static constexpr u32 kTreeRecursionLimit = 64u;

/**
 * EBtKind を表示用リテラルに変換する。
 *
 * @param k 変換元の node 種別。
 * @return "Selector" / "FSequence" / "Action" (到達不能時は "Unknown")。
 */
static const char* KindLabel(EBtKind k) noexcept {
    switch (k) {
        case EBtKind::Selector:  return "Selector";
        case EBtKind::Sequence:  return "Sequence";
        case EBtKind::Action:    return "Action";
        case EBtKind::Decorator: return "Decorator";
        case EBtKind::Task:      return "Task";
    }
    return "Unknown"; // 到達不能 (enum を u8 で広げない限り)
}

/**
 * EBtDecoratorOp を表示用リテラルに変換する。
 *
 * @param op 変換元の decorator op。
 * @return "Inverter" / "ForceSuccess" / "ForceFailure" / "Repeat"。
 */
static const char* DecoLabel(EBtDecoratorOp op) noexcept {
    switch (op) {
        case EBtDecoratorOp::Inverter:     return "Inverter";
        case EBtDecoratorOp::ForceSuccess: return "ForceSuccess";
        case EBtDecoratorOp::ForceFailure: return "ForceFailure";
        case EBtDecoratorOp::Repeat:       return "Repeat";
    }
    return "Inverter";
}

/**
 * EBtCompareOp を表示用の記号に変換する。
 *
 * @param op 変換元の比較演算子。
 * @return "<" / "<=" / "==" / "!=" / ">=" / ">"。
 */
static const char* CmpOpLabel(EBtCompareOp op) noexcept {
    switch (op) {
        case EBtCompareOp::Less:      return "<";
        case EBtCompareOp::LessEq:    return "<=";
        case EBtCompareOp::Equal:     return "==";
        case EBtCompareOp::NotEqual:  return "!=";
        case EBtCompareOp::GreaterEq: return ">=";
        case EBtCompareOp::Greater:   return ">";
    }
    return "<";
}

/**
 * EBtStatus を表示用リテラルに変換する。
 *
 * @param s 変換元の status。
 * @return "Success" / "Failure" / "Running" (到達不能時は "Unknown")。
 */
static const char* StatusLabel(EBtStatus s) noexcept {
    switch (s) {
        case EBtStatus::Success: return "Success";
        case EBtStatus::Failure: return "Failure";
        case EBtStatus::Running: return "Running";
    }
    return "Unknown";
}

/**
 * EBtStatus を PlotLines 用の float 値に変換する。
 *
 * @details Success=1.0 / Running=0.5 / Failure=0.0。視覚的に「上ほど成功・中段で実行中・下が失敗」と読める並びにする。
 * @param s 変換元の status。
 * @return プロット用の y 値 (到達不能時は 0.0)。
 */
static f32 StatusToPlotValue(EBtStatus s) noexcept {
    switch (s) {
        case EBtStatus::Success: return 1.0f;
        case EBtStatus::Running: return 0.5f;
        case EBtStatus::Failure: return 0.0f;
    }
    return 0.0f;
}

/**
 * name が null なら代替ラベルを返す。
 *
 * @param name 表示名 (nullptr 可)。
 * @return name が非 null ならそのまま、null なら "(unnamed)"。
 */
static const char* SafeName(const char* name) noexcept {
    return (name != nullptr) ? name : "(unnamed)";
}

/**
 * id が m_Nodes の有効範囲内かをチェックする。
 *
 * @param id 検査する node id。
 * @param node_count 現在の node 数 (m_Nodes.Size())。
 * @return kInvalidId でなく id < node_count なら true。
 */
static bool IsValidId(u32 id, usize node_count) noexcept {
    if (id == FBehaviorTreeEditorPanel::kInvalidId) return false;
    return static_cast<usize>(id) < node_count;
}

/**
 * EBtStatus を RGBA float に変換する (header から ImVec4 を隠す目的)。
 *
 * @details
 * Success=緑 (0.2,1.0,0.3)、Failure=赤 (1.0,0.3,0.3)、Running=黄 (1.0,1.0,0.3) に固定。
 * 完全飽和ではなく落ち着いた色味にする (ダーク背景でも明るすぎず、ライト背景でも沈まないトーン)。
 */
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

/** メタミラー / 履歴 / selection / step counter を初期化する (callback は維持)。 */
void FBehaviorTreeEditorPanel::Init() noexcept {
    // メタミラー / 履歴 / selection / step counter を全 reset。
    m_Nodes.Clear();
    m_History.Clear();
    m_History.Resize(static_cast<usize>(kHistorySize)); // 60 frame 分を確保
    for (usize i = 0; i < m_History.Size(); ++i) {
        m_History[i] = static_cast<u8>(EBtStatus::Failure); // 初期は Failure
    }
    m_HistoryHead = 0;

    m_Tree       = nullptr; // BT は SetTree で後付け
    m_Autorun    = false;
    m_StepCount = 0;
    m_Selected   = kInvalidId;
    // callback はリセットしない (Init は state の reset、callback は別操作)。
}

/** メタミラー / 履歴 / callback を全てクリアする (多重 Shutdown 可)。 */
void FBehaviorTreeEditorPanel::Shutdown() noexcept {
    // 多重 Shutdown 可。TArray は Clear() で要素破棄 + 容量保持。
    m_Nodes.Clear();
    m_History.Clear();
    m_HistoryHead = 0;

    m_Tree       = nullptr;
    m_Autorun    = false;
    m_StepCount = 0;
    m_Selected   = kInvalidId;

    // callback も解除する (Shutdown は完全リセットの意味合い)。
    m_StepCb    = nullptr;
    m_StepUser  = nullptr;
}

/** 観察対象の BT を差し替え、Reset() で実行状態を初期化する (メタミラーは維持)。 */
void FBehaviorTreeEditorPanel::SetTree(FBehaviorTree* tree) noexcept {
    m_Tree = tree;
    // 観察対象が変わったので step counter / 履歴 / 全 status を初期化する。
    // メタミラー (m_Nodes) は触らない (= 同構造で別インスタンスを観察する用途に
    // 対応するため、ユーザが ClearNodes を別途呼ばない限り維持する)。
    Reset();
}

/** step counter / history / 全 node の last_status を初期状態に戻す。 */
void FBehaviorTreeEditorPanel::Reset() noexcept {
    m_StepCount   = 0;
    m_HistoryHead = 0;
    for (usize i = 0; i < m_History.Size(); ++i) {
        m_History[i] = static_cast<u8>(EBtStatus::Failure);
    }
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        m_Nodes[i].last_status = EBtStatus::Failure;
    }
    // autorun / selection / メタミラーは触らない (= ユーザ操作で変える物)。
}

/** kStepDt (0.016 秒) 固定で 1 tick 進める。 */
void FBehaviorTreeEditorPanel::StepOnce() noexcept {
    // 0.016 sec (= 60 fps の 1 frame) を仕様で固定。Step は常に同じ dt で進めることで
    // ユーザが "今のステップ数 * 1/60 秒進んだ" と即座に解釈できるようにする。
    TickInternal(kStepDt);
}

/** 実 BT を dt で 1 tick 進め、root status を履歴に push、step counter を +1 する。 */
void FBehaviorTreeEditorPanel::TickInternal(f32 dt) noexcept {
    // レジストリ設定済みなら「グラフを直接インタプリト」して実行 (no-code 実行)。
    if (m_Registry != nullptr) { TickGraph(dt); return; }
    if (m_Tree == nullptr) return;

    // (1) 実 BT を 1 tick 進める
    EBtStatus root_status = EBtStatus::Failure;
    if (m_StepCb != nullptr) {
        // callback に委譲 (ユーザが任意 blackboard を渡せる)。
        // callback 側で `tree->Tick(my_bb, dt)` を呼ぶ規約。戻り値は取れないので、
        // root status は callback が SetNodeStatus(root_id=0, ...) で push する想定。
        // ここでは m_Nodes[0].last_status を採用する fallback (= 履歴 graph 用)。
        m_StepCb(m_StepUser, m_Tree, dt);
        if (!m_Nodes.IsEmpty()) {
            root_status = m_Nodes[0].last_status;
        }
    } else {
        // fallback: blackboard = nullptr で直接呼ぶ。
        // 戻り値を取れるのでそのまま履歴に push する。
        root_status = m_Tree->Tick(nullptr, dt);
        if (!m_Nodes.IsEmpty()) {
            // root の last_status も同期的に更新 (Inspector / TreeView 表示と
            // PlotLines の値を一致させるため)。
            m_Nodes[0].last_status = root_status;
        }
    }

    // (2) 履歴 ring buffer に push
    if (!m_History.IsEmpty()) {
        m_History[m_HistoryHead] = static_cast<u8>(root_status);
        m_HistoryHead = (m_HistoryHead + 1u) % kHistorySize;
    }

    // (3) step counter
    ++m_StepCount;
}

/** autorun が ON のときだけ実 dt で 1 tick 進める (0 dt は無視)。 */
void FBehaviorTreeEditorPanel::OnFrameBegin(f32 dt) noexcept {
    // autorun ON のときだけ毎フレーム 1 tick 進める。実 dt を渡すことで
    // ゲームの fps に追従させる (Step は 0.016 固定だが autorun は別)。
    if (!m_Autorun) return;
    if (m_Tree == nullptr && m_Registry == nullptr) return;  // 実行対象なし
    if (dt <= 0.0f) return; // 0 dt スパイクで何もしない (= 一時停止と同じ)
    TickInternal(dt);
}

/** 選択 node を設定する (範囲外 / kInvalidId は「未選択」に正規化)。 */
void FBehaviorTreeEditorPanel::SelectNode(u32 node_id) noexcept {
    if (!IsValidId(node_id, m_Nodes.Size())) {
        m_Selected = kInvalidId;
        return;
    }
    m_Selected = node_id;
}

/** 新規 node をメタミラーに追加し、払い出した id を返す (上限到達は kInvalidId)。 */
u32 FBehaviorTreeEditorPanel::AddNode(EBtKind kind, const char* name, u32 parent_id) noexcept {
    if (m_Nodes.Size() >= static_cast<usize>(kMaxNodes)) {
        // 上限到達は silent fail (= kInvalidId 返却で通知)。
        return kInvalidId;
    }

    // parent_id バリデーション: kInvalidId 以外で範囲外なら root 扱いに倒す。
    // (= 不正な parent でも panel が落ちないようにする防衛策)
    if (parent_id != kInvalidId && !IsValidId(parent_id, m_Nodes.Size())) {
        parent_id = kInvalidId;
    }

    NodeMeta n;
    n.id          = static_cast<u32>(m_Nodes.Size()); // 払い出し = 現在の末尾 index
    n.parent_id   = parent_id;
    n.kind        = kind;
    n.name        = name;
    n.last_status = EBtStatus::Failure; // 初期は Failure

    const u32 new_id = n.id;
    m_Nodes.PushBack(n);
    m_DidLayout = false;   // 構成が変わったので次フレームで再レイアウト
    return new_id;
}

/** 指定 node の last_status を更新する (範囲外は no-op)。 */
void FBehaviorTreeEditorPanel::SetNodeStatus(u32 node_id, EBtStatus status) noexcept {
    if (!IsValidId(node_id, m_Nodes.Size())) return;
    m_Nodes[static_cast<usize>(node_id)].last_status = status;
}

/** Decorator node の変換 op を設定する (範囲外 / Decorator 以外は no-op で false)。 */
bool FBehaviorTreeEditorPanel::SetNodeDecoratorOp(u32 node_id, EBtDecoratorOp op) noexcept {
    if (!IsValidId(node_id, m_Nodes.Size())) return false;
    NodeMeta& n = m_Nodes[static_cast<usize>(node_id)];
    if (n.kind != EBtKind::Decorator) return false;
    n.deco = op;
    return true;
}

/** Decorator node の動作モードを設定する (範囲外 / Decorator 以外は no-op で false)。 */
bool FBehaviorTreeEditorPanel::SetNodeDecoratorMode(u32 node_id, EBtDecoMode mode) noexcept {
    if (!IsValidId(node_id, m_Nodes.Size())) return false;
    NodeMeta& n = m_Nodes[static_cast<usize>(node_id)];
    if (n.kind != EBtKind::Decorator) return false;
    n.decoMode = mode;
    return true;
}

/** Decorator node を Compare モードにし、比較条件 (var op rhs) を設定する。 */
bool FBehaviorTreeEditorPanel::SetNodeCompare(u32 node_id, const char* var, EBtCompareOp op, f32 rhs) noexcept {
    if (!IsValidId(node_id, m_Nodes.Size())) return false;
    NodeMeta& n = m_Nodes[static_cast<usize>(node_id)];
    if (n.kind != EBtKind::Decorator) return false;
    n.decoMode = EBtDecoMode::Compare;
    std::snprintf(n.var, sizeof(n.var), "%s", (var != nullptr) ? var : "");
    n.cmpOp  = op;
    n.cmpRhs = rhs;
    return true;
}

/** メタミラーを全削除し selection を解除する (history / step counter / autorun は維持)。 */
void FBehaviorTreeEditorPanel::ClearNodes() noexcept {
    m_Nodes.Clear();
    m_Selected = kInvalidId;
    m_DragNode = kInvalidId;
    m_LinkSrc  = kInvalidId;
    m_DidLayout = false;
    // history / step counter / autorun は触らない (= ユーザの明示操作で変える物)。
}

/** 指定 id の node の last_status を返す (範囲外は Failure)。 */
EBtStatus FBehaviorTreeEditorPanel::NodeStatus(u32 node_id) const noexcept {
    if (!IsValidId(node_id, m_Nodes.Size())) return EBtStatus::Failure;
    return m_Nodes[static_cast<usize>(node_id)].last_status;
}

/** step tick 用の callback とユーザポインタを登録する (nullptr で解除)。 */
void FBehaviorTreeEditorPanel::SetOnStepCallback(StepCallback cb, void* user) noexcept {
    m_StepCb   = cb;
    m_StepUser = user;
}

/**
 * 1 node を TreeNode で描画し、子を線形走査して再帰展開する。
 *
 * @details
 * parent_id が node_id を指す子 node を m_Nodes 内で線形に探して再帰描画する。
 * Action (= leaf) は ImGuiTreeNodeFlags_Leaf を付ける。ImGuiTreeNodeFlags_OpenOnArrow で
 * 「矢印クリックで展開、ラベルクリックで選択」にする (Unity Hierarchy と同形)。
 */
void FBehaviorTreeEditorPanel::DrawTreeRecursive(u32 node_id, u32 depth) noexcept {
    if (depth >= kTreeRecursionLimit) {
        ImGui::TextDisabled("  (tree depth limit reached)");
        return;
    }
    if (!IsValidId(node_id, m_Nodes.Size())) return;

    const NodeMeta& node = m_Nodes[static_cast<usize>(node_id)];
    if (!node.alive) return;

    // 子検索 (= 線形走査で parent_id == node_id の生存要素があるか)。
    bool has_child = false;
    if (!BtKindIsLeaf(node.kind)) {
        // Action / Task は leaf。Selector / Sequence / Decorator のみ子を持ち得る。
        for (usize i = 0; i < m_Nodes.Size(); ++i) {
            if (m_Nodes[i].alive && m_Nodes[i].parent_id == node_id) {
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
    if (m_Selected == node_id) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    // ラベル整形: "[Kind] name  ● Status" (kind と status を視認しやすく)。
    char label[160];
    std::snprintf(label, sizeof(label),
                  "[%s] %s",
                  KindLabel(node.kind),
                  DisplayName(node));

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
        m_Selected = node_id;
    }

    // 同行右側に status バッジを SameLine 表示 ([Success/Failure/Running])。
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(col[0], col[1], col[2], col[3]),
                       "[%s]", StatusLabel(node.last_status));

    if (open && has_child) {
        for (usize i = 0; i < m_Nodes.Size(); ++i) {
            if (m_Nodes[i].alive && m_Nodes[i].parent_id == node_id) {
                DrawTreeRecursive(static_cast<u32>(i), depth + 1u);
            }
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

/** node の表示名 (エディタ名 ename を優先、無ければ name)。 */
const char* FBehaviorTreeEditorPanel::DisplayName(const NodeMeta& n) const noexcept {
    if (n.ename[0] != '\0') return n.ename;
    return SafeName(n.name);
}

/** maybe_ancestor が node の祖先 (自分自身含む) かを返す。 */
bool FBehaviorTreeEditorPanel::IsAncestor(u32 maybe_ancestor, u32 node) const noexcept {
    u32 cur = node;
    for (u32 guard = 0; guard < kTreeRecursionLimit; ++guard) {
        if (cur == maybe_ancestor) return true;
        if (!IsValidId(cur, m_Nodes.Size())) break;
        cur = m_Nodes[static_cast<usize>(cur)].parent_id;
        if (cur == kInvalidId) break;
    }
    return false;
}

namespace {
constexpr f32 kColW = 172.0f;   // 兄弟間の横間隔 (world)
constexpr f32 kRowH = 98.0f;    // 階層間の縦間隔 (world)
}

/** tree 構造から x,y を自動配置する (Reingold-Tilford 風の簡易 tidy layout)。 */
void FBehaviorTreeEditorPanel::AutoLayout() noexcept {
    // 再帰は関数ポインタにできないので明示スタックではなく素朴な再帰ヘルパをラムダ無しで。
    // m_Nodes を直接触る member 再帰を below の static 関数に委譲する代わり、ここで完結させる。
    f32 next_leaf = 0.0f;
    // 再帰ヘルパ (自己呼び出しのため struct local function)
    struct Rec {
        TArray<NodeMeta>& nodes;
        f32& next_leaf;
        f32 run(u32 id, u32 depth, u32 guard) noexcept {
            if (guard >= kTreeRecursionLimit || !(static_cast<usize>(id) < nodes.Size())) {
                const f32 gx = next_leaf; next_leaf += 1.0f; return gx;
            }
            f32 sum = 0.0f; u32 cnt = 0;
            for (usize i = 0; i < nodes.Size(); ++i) {
                if (nodes[i].alive && nodes[i].parent_id == id) {
                    sum += run(static_cast<u32>(i), depth + 1u, guard + 1u);
                    ++cnt;
                }
            }
            f32 gx;
            if (cnt == 0) { gx = next_leaf; next_leaf += 1.0f; }
            else          { gx = sum / static_cast<f32>(cnt); }
            nodes[static_cast<usize>(id)].x = gx * kColW;
            nodes[static_cast<usize>(id)].y = static_cast<f32>(depth) * kRowH;
            return gx;
        }
    } rec{ m_Nodes, next_leaf };

    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        if (m_Nodes[i].alive && m_Nodes[i].parent_id == kInvalidId) {
            rec.run(static_cast<u32>(i), 0u, 0u);
        }
    }
}

/** エディタからノードを追加する (グラフ上に配置、再レイアウトはしない)。 */
u32 FBehaviorTreeEditorPanel::AddNodeGraph(EBtKind kind, u32 parent_id, f32 wx, f32 wy) noexcept {
    if (m_Nodes.Size() >= static_cast<usize>(kMaxNodes)) return kInvalidId;
    if (parent_id != kInvalidId && !IsValidId(parent_id, m_Nodes.Size())) parent_id = kInvalidId;

    // Decorator は単子: 既存の子があれば root へ外してから新しい子を付ける (= 置換)。
    // これで「Add child を Decorator に複数回 → 余分な子が黙って無視される」を防ぐ。
    if (parent_id != kInvalidId && m_Nodes[static_cast<usize>(parent_id)].kind == EBtKind::Decorator) {
        for (usize i = 0; i < m_Nodes.Size(); ++i) {
            if (m_Nodes[i].alive && m_Nodes[i].parent_id == parent_id) {
                m_Nodes[i].parent_id = kInvalidId;
            }
        }
    }

    NodeMeta n;
    n.id          = static_cast<u32>(m_Nodes.Size());
    n.parent_id   = parent_id;
    n.kind        = kind;
    n.name        = nullptr;
    n.x = wx; n.y = wy;
    n.alive = true;
    n.last_status = EBtStatus::Failure;
    std::snprintf(n.ename, sizeof(n.ename), "%s %u", KindLabel(kind), static_cast<unsigned>(n.id));

    const u32 new_id = n.id;
    m_Nodes.PushBack(n);
    m_Selected = new_id;
    // 明示配置なので m_DidLayout は変えない (= 既存ノードの位置を保つ)。
    return new_id;
}

/** ノードを tombstone 削除し、子を祖父へ付け替える。 */
void FBehaviorTreeEditorPanel::DeleteNodeGraph(u32 id) noexcept {
    if (!IsValidId(id, m_Nodes.Size())) return;
    const u32 gp = m_Nodes[static_cast<usize>(id)].parent_id;
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        if (m_Nodes[i].parent_id == id) m_Nodes[i].parent_id = gp;
    }
    m_Nodes[static_cast<usize>(id)].alive     = false;
    m_Nodes[static_cast<usize>(id)].parent_id = kInvalidId;
    if (m_Selected == id) m_Selected = kInvalidId;
    if (m_DragNode == id) m_DragNode = kInvalidId;
    if (m_CtxNode  == id) m_CtxNode  = kInvalidId;
}

/** id の生存子を x 座標 (左→右) 順に集める。 */
u32 FBehaviorTreeEditorPanel::CollectChildrenSorted(u32 id, u32* out, u32 cap) const noexcept {
    u32 cnt = 0;
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        if (!m_Nodes[i].alive || m_Nodes[i].parent_id != id) continue;
        if (cnt >= cap) break;
        // x 昇順に挿入 (見た目の左→右をそのまま評価順にする)
        u32 pos = cnt;
        while (pos > 0 && m_Nodes[static_cast<usize>(out[pos - 1])].x > m_Nodes[i].x) {
            out[pos] = out[pos - 1]; --pos;
        }
        out[pos] = static_cast<u32>(i);
        ++cnt;
    }
    return cnt;
}

/** グラフ 1 ノードを再帰インタプリト (selector/sequence/action 意味論)。 */
EBtStatus FBehaviorTreeEditorPanel::TickGraphNode(u32 id, f32 dt, u32 guard) noexcept {
    if (guard >= kTreeRecursionLimit || !IsValidId(id, m_Nodes.Size())) return EBtStatus::Failure;
    if (!m_Nodes[static_cast<usize>(id)].alive) return EBtStatus::Failure;

    m_Nodes[static_cast<usize>(id)].visit_order = ++m_VisitSeq;   // 実行フロー記録
    const EBtKind kind = m_Nodes[static_cast<usize>(id)].kind;

    EBtStatus result;
    if (BtKindIsLeaf(kind)) {
        // Action / Task はともにレジストリ解決した関数を呼ぶ末端 leaf (種別は表示上の区別)。
        FBtActionRegistry::Fn fn = (m_Registry != nullptr)
            ? m_Registry->Find(DisplayName(m_Nodes[static_cast<usize>(id)])) : nullptr;
        result = (fn != nullptr) ? fn(m_GraphBb, dt) : EBtStatus::Failure;  // 未解決は Failure
    } else if (kind == EBtKind::Decorator) {
        // Decorator は子 1 つ (左端 = 最初の子)。モードで Transform / 条件ガードを切替。
        NodeMeta& dn = m_Nodes[static_cast<usize>(id)];
        u32 kids[kMaxNodes];
        const u32 ck = CollectChildrenSorted(id, kids, kMaxNodes);

        if (dn.decoMode == EBtDecoMode::Transform) {
            // 子を評価し、deco op で変換 (runtime ApplyDecorator と同一実装)。
            if (ck == 0) result = EBtStatus::Failure;          // 装飾対象なし → ソフトフェイル
            else         result = ApplyDecorator(dn.deco, TickGraphNode(kids[0], dt, guard + 1u));
        } else {
            // 条件ガード: 条件 true のときだけ子を実行し、その結果を返す。false は子をスキップして Failure。
            bool pass = false;
            if (dn.decoMode == EBtDecoMode::Condition) {
                FBtConditionRegistry::Fn cf =
                    (m_CondReg != nullptr) ? m_CondReg->Find(DisplayName(dn)) : nullptr;
                pass = (cf != nullptr) ? cf(m_GraphBb) : false; // 未解決は false (ガードで止まる)
            } else { // EBtDecoMode::Compare
                // 変数解決は動的ブラックボード (名前) を優先し、無ければ offset スキーマ。
                if (m_DynBb != nullptr && m_DynBb->Has(dn.var)) {
                    pass = BtCompareF32(m_DynBb->GetAsF32(dn.var), dn.cmpOp, dn.cmpRhs);
                } else if (m_Schema != nullptr) {
                    const u32 vi = m_Schema->IndexOf(dn.var);
                    if (vi != FBtBlackboardSchema::kInvalid) {
                        pass = BtCompareVar(m_GraphBb, m_Schema->OffsetAt(vi),
                                            m_Schema->TypeAt(vi), dn.cmpOp, dn.cmpRhs);
                    }
                }
            }
            result = (pass && ck > 0) ? TickGraphNode(kids[0], dt, guard + 1u) : EBtStatus::Failure;
        }
    } else {
        u32 kids[kMaxNodes];
        const u32 ck = CollectChildrenSorted(id, kids, kMaxNodes);
        // 子なし: Selector=Failure / Sequence=Success (ランタイム FBt* と同規約)
        result = (kind == EBtKind::Selector) ? EBtStatus::Failure : EBtStatus::Success;
        for (u32 k = 0; k < ck; ++k) {
            const EBtStatus s = TickGraphNode(kids[k], dt, guard + 1u);
            if (kind == EBtKind::Selector) {
                if (s == EBtStatus::Running) { result = EBtStatus::Running; break; }
                if (s == EBtStatus::Success) { result = EBtStatus::Success; break; }
                // Failure → 次の子へ
            } else { // Sequence
                if (s == EBtStatus::Running) { result = EBtStatus::Running; break; }
                if (s == EBtStatus::Failure) { result = EBtStatus::Failure; break; }
                // Success → 次の子へ
            }
        }
    }
    m_Nodes[static_cast<usize>(id)].last_status = result;
    return result;
}

/** メタミラーのグラフを 1 tick 直接インタプリトする。 */
EBtStatus FBehaviorTreeEditorPanel::TickGraph(f32 dt) noexcept {
    m_VisitSeq = 0u;
    for (usize i = 0; i < m_Nodes.Size(); ++i) m_Nodes[i].visit_order = 0u;  // フロークリア

    EBtStatus root_status = EBtStatus::Failure;
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        if (m_Nodes[i].alive && m_Nodes[i].parent_id == kInvalidId) {
            root_status = TickGraphNode(static_cast<u32>(i), dt, 0u);
            break;   // root は 1 つ想定 (forest の場合は先頭のみ駆動)
        }
    }
    if (!m_History.IsEmpty()) {
        m_History[m_HistoryHead] = static_cast<u8>(root_status);
        m_HistoryHead = (m_HistoryHead + 1u) % kHistorySize;
    }
    ++m_StepCount;
    return root_status;
}

/** グラフをテキストファイルへ保存する (生存ノードを id 圧縮して書く)。 */
bool FBehaviorTreeEditorPanel::SaveGraph(const char* path) const noexcept {
    if (path == nullptr) return false;
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;

    // 生存ノードに連番 (new id) を振る remap。
    u32 remap[kMaxNodes];
    for (u32 i = 0; i < kMaxNodes; ++i) remap[i] = kInvalidId;
    u32 nc = 0;
    for (usize i = 0; i < m_Nodes.Size(); ++i) if (m_Nodes[i].alive) remap[i] = nc++;

    // version 4: ノード列 (v3 と同形) の後に動的ブラックボード変数セクション "BB <n>" を追加。
    // ノード列: id parent kind deco decoMode cmpOp cmpRhs x y var name
    //   var は 1 トークン (空なら "-")、name は行末まで (空白可)。
    std::fprintf(f, "ACSBT 4\n%u\n", nc);
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        const NodeMeta& n = m_Nodes[i];
        if (!n.alive) continue;
        int parent_out = -1;
        if (n.parent_id != kInvalidId && static_cast<usize>(n.parent_id) < m_Nodes.Size()
            && m_Nodes[static_cast<usize>(n.parent_id)].alive) {
            parent_out = static_cast<int>(remap[n.parent_id]);
        }
        const char* var_out = (n.var[0] != '\0') ? n.var : "-";
        std::fprintf(f, "%u %d %d %d %d %d %.3f %.1f %.1f %s %s\n",
                     remap[i], parent_out, static_cast<int>(n.kind),
                     static_cast<int>(n.deco), static_cast<int>(n.decoMode),
                     static_cast<int>(n.cmpOp), static_cast<double>(n.cmpRhs),
                     n.x, n.y, var_out, DisplayName(n));
    }

    // 動的ブラックボード変数 (v4)。"BB <count>" の後に "<name> <type> <value>" を並べる。
    // value は型に応じた数値 (bool=0/1, i32, f32)。name は 1 トークン想定 (識別子)。
    const u32 vc = (m_DynBb != nullptr) ? m_DynBb->Count() : 0u;
    std::fprintf(f, "BB %u\n", vc);
    for (u32 v = 0; v < vc; ++v) {
        const char* vn = m_DynBb->NameAt(v);
        const int   ty = static_cast<int>(m_DynBb->TypeAt(v));
        switch (m_DynBb->TypeAt(v)) {
            case EBtVarType::Bool: std::fprintf(f, "%s %d %d\n",    vn, ty, m_DynBb->GetBool(vn) ? 1 : 0); break;
            case EBtVarType::I32:  std::fprintf(f, "%s %d %d\n",    vn, ty, m_DynBb->GetI32(vn)); break;
            case EBtVarType::F32:  std::fprintf(f, "%s %d %.6g\n",  vn, ty, static_cast<double>(m_DynBb->GetF32(vn))); break;
        }
    }

    std::fclose(f);
    return true;
}

/** メタミラー 1 ノードを実行可能な FBtNode へ再帰変換する。 */
TUniquePtr<FBtNode> FBehaviorTreeEditorPanel::BuildRuntimeNode(u32 id, u32 guard) const noexcept {
    if (guard >= kTreeRecursionLimit || !IsValidId(id, m_Nodes.Size())) return TUniquePtr<FBtNode>();
    const NodeMeta& n = m_Nodes[static_cast<usize>(id)];
    if (!n.alive) return TUniquePtr<FBtNode>();

    u32 kids[kMaxNodes];
    const u32 ck = CollectChildrenSorted(id, kids, kMaxNodes);

    switch (n.kind) {
        case EBtKind::Selector: {
            auto s = MakeUnique<FBtSelector>();
            for (u32 k = 0; k < ck; ++k) s->AddChild(BuildRuntimeNode(kids[k], guard + 1u));
            return s;   // TUniquePtr<FBtSelector> → TUniquePtr<FBtNode> (upcast)
        }
        case EBtKind::Sequence: {
            auto s = MakeUnique<FBtSequence>();
            for (u32 k = 0; k < ck; ++k) s->AddChild(BuildRuntimeNode(kids[k], guard + 1u));
            return s;
        }
        case EBtKind::Decorator: {
            TUniquePtr<FBtNode> child = (ck > 0) ? BuildRuntimeNode(kids[0], guard + 1u)
                                                 : TUniquePtr<FBtNode>();
            if (n.decoMode == EBtDecoMode::Transform) {
                auto d = MakeUnique<FBtDecorator>(n.deco);
                d->SetChild(Move(child));
                return d;
            } else if (n.decoMode == EBtDecoMode::Condition) {
                FBtConditionNode::Fn cf = (m_CondReg != nullptr) ? m_CondReg->Find(DisplayName(n)) : nullptr;
                auto d = MakeUnique<FBtConditionNode>(cf);
                d->SetChild(Move(child));
                return d;
            } else { // EBtDecoMode::Compare
                // 変数解決はインタプリタと同順: 動的 BB (名前) を優先、無ければ offset スキーマ。
                TUniquePtr<FBtCompareNode> d;
                if (m_DynBb != nullptr && m_DynBb->Has(n.var)) {
                    d = MakeUnique<FBtCompareNode>(n.var, n.cmpOp, n.cmpRhs);          // dynamic
                } else if (m_Schema != nullptr) {
                    const u32 vi = m_Schema->IndexOf(n.var);
                    if (vi != FBtBlackboardSchema::kInvalid) {
                        d = MakeUnique<FBtCompareNode>(m_Schema->OffsetAt(vi), m_Schema->TypeAt(vi),
                                                       n.cmpOp, n.cmpRhs);             // schema
                    }
                }
                if (!d) d = MakeUnique<FBtCompareNode>(n.var, n.cmpOp, n.cmpRhs);      // 未解決→dynamic名 (Failure)
                d->SetChild(Move(child));
                return d;
            }
        }
        case EBtKind::Action:
        case EBtKind::Task: {
            FBtActionRegistry::Fn fn = (m_Registry != nullptr) ? m_Registry->Find(DisplayName(n)) : nullptr;
            return MakeUnique<FBtAction>(fn);   // fn 未解決でも FBtAction が Failure を返す
        }
    }
    return TUniquePtr<FBtNode>(); // 到達不能
}

/** 現在のグラフを実行可能な FBehaviorTree ノードツリーへ bake する。 */
TUniquePtr<FBtNode> FBehaviorTreeEditorPanel::BuildRuntimeTree() const noexcept {
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        if (m_Nodes[i].alive && m_Nodes[i].parent_id == kInvalidId) {
            return BuildRuntimeNode(static_cast<u32>(i), 0u);   // root は先頭の親なしノード
        }
    }
    return TUniquePtr<FBtNode>();
}

/** テキストファイルからグラフを読み込む (既存はクリア)。 */
bool FBehaviorTreeEditorPanel::LoadGraph(const char* path) noexcept {
    if (path == nullptr) return false;
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;

    char line[256];
    int ver = 1;
    if (std::fgets(line, sizeof(line), f) == nullptr || std::strncmp(line, "ACSBT", 5) != 0) {
        std::fclose(f); return false;
    }
    std::sscanf(line, "ACSBT %d", &ver);   // version (>=2 なら deco 列あり)
    int count = 0;
    if (std::fgets(line, sizeof(line), f) == nullptr || std::sscanf(line, "%d", &count) != 1) {
        std::fclose(f); return false;
    }

    ClearNodes();
    for (int k = 0; k < count; ++k) {
        if (std::fgets(line, sizeof(line), f) == nullptr) break;
        int id = 0, parent = -1, kind = 2, deco = 0, decoMode = 0, cmpOp = 0, off = 0;
        float x = 0.0f, y = 0.0f, cmpRhs = 0.0f;
        char varbuf[48] = "-";
        if (ver >= 3) {
            if (std::sscanf(line, "%d %d %d %d %d %d %f %f %f %47s %n",
                            &id, &parent, &kind, &deco, &decoMode, &cmpOp,
                            &cmpRhs, &x, &y, varbuf, &off) < 10) continue;
        } else if (ver == 2) {
            if (std::sscanf(line, "%d %d %d %d %f %f %n", &id, &parent, &kind, &deco, &x, &y, &off) < 6) continue;
        } else {
            if (std::sscanf(line, "%d %d %d %f %f %n", &id, &parent, &kind, &x, &y, &off) < 5) continue;
        }
        // 名前は残り全部 (空白を含み得る)。改行を除去。
        const char* nm = line + off;
        char namebuf[48];
        std::snprintf(namebuf, sizeof(namebuf), "%s", nm);
        for (u32 c = 0; c < sizeof(namebuf); ++c) {
            if (namebuf[c] == '\n' || namebuf[c] == '\r') { namebuf[c] = '\0'; break; }
        }
        if (m_Nodes.Size() >= static_cast<usize>(kMaxNodes)) break;
        NodeMeta n;
        n.id          = static_cast<u32>(m_Nodes.Size());
        n.parent_id   = (parent < 0) ? kInvalidId : static_cast<u32>(parent);
        n.kind        = (kind == 0) ? EBtKind::Selector
                      : (kind == 1) ? EBtKind::Sequence
                      : (kind == 3) ? EBtKind::Decorator
                      : (kind == 4) ? EBtKind::Task
                      :               EBtKind::Action;
        n.deco        = (deco == 1) ? EBtDecoratorOp::ForceSuccess
                      : (deco == 2) ? EBtDecoratorOp::ForceFailure
                      : (deco == 3) ? EBtDecoratorOp::Repeat
                      :               EBtDecoratorOp::Inverter;
        n.decoMode    = (decoMode == 1) ? EBtDecoMode::Condition
                      : (decoMode == 2) ? EBtDecoMode::Compare
                      :                   EBtDecoMode::Transform;
        n.cmpOp       = (cmpOp >= 0 && cmpOp <= 5) ? static_cast<EBtCompareOp>(cmpOp) : EBtCompareOp::Less;
        n.cmpRhs      = cmpRhs;
        if (std::strcmp(varbuf, "-") != 0) std::snprintf(n.var, sizeof(n.var), "%s", varbuf); // "-" は空
        n.x = x; n.y = y;
        n.alive = true;
        n.last_status = EBtStatus::Failure;
        std::snprintf(n.ename, sizeof(n.ename), "%s", namebuf);
        m_Nodes.PushBack(n);
    }

    // 動的ブラックボード変数 (v4)。"BB <count>" + 各変数行を m_DynBb へ復元する。
    if (ver >= 4 && m_DynBb != nullptr) {
        int vcount = 0;
        if (std::fgets(line, sizeof(line), f) != nullptr && std::sscanf(line, "BB %d", &vcount) == 1) {
            m_DynBb->Clear();
            for (int v = 0; v < vcount; ++v) {
                if (std::fgets(line, sizeof(line), f) == nullptr) break;
                char   vname[48] = {};
                int    vty       = 2;
                double vval      = 0.0;
                if (std::sscanf(line, "%47s %d %lf", vname, &vty, &vval) < 3) continue;
                const EBtVarType t = (vty == 0) ? EBtVarType::Bool
                                   : (vty == 1) ? EBtVarType::I32
                                   :              EBtVarType::F32;
                m_DynBb->Add(vname, t);
                switch (t) {
                    case EBtVarType::Bool: m_DynBb->SetBool(vname, vval != 0.0); break;
                    case EBtVarType::I32:  m_DynBb->SetI32(vname, static_cast<acs::i32>(vval)); break;
                    case EBtVarType::F32:  m_DynBb->SetF32(vname, static_cast<f32>(vval)); break;
                }
            }
        }
    }

    std::fclose(f);
    m_DidLayout = true;          // 保存座標を尊重 (auto-layout で潰さない)
    m_Selected  = kInvalidId;
    return true;
}

/** 「ノード追加」メニュー項目群を描画する (popup / submenu から共通利用)。 */
void FBehaviorTreeEditorPanel::DrawAddMenu(u32 parent, f32 wx, f32 wy) noexcept {
    if (ImGui::MenuItem("Selector")) AddNodeGraph(EBtKind::Selector, parent, wx, wy);
    if (ImGui::MenuItem("Sequence")) AddNodeGraph(EBtKind::Sequence, parent, wx, wy);

    // Decorator (結果変換 op)。
    if (ImGui::BeginMenu("Decorator (op)")) {
        for (int d = 0; d < 4; ++d) {
            const EBtDecoratorOp op = static_cast<EBtDecoratorOp>(d);
            if (ImGui::MenuItem(DecoLabel(op)))
                SetNodeDecoratorOp(AddNodeGraph(EBtKind::Decorator, parent, wx, wy), op);
        }
        ImGui::EndMenu();
    }

    // Condition デコレーター (catalog の bool 関数で子をガード)。
    if (ImGui::BeginMenu("Condition (fn)")) {
        if (m_CondReg != nullptr && m_CondReg->Count() > 0) {
            for (u32 c = 0; c < m_CondReg->Count(); ++c) {
                const char* cn = m_CondReg->NameAt(c);
                if (ImGui::MenuItem(cn)) {
                    const u32 nid = AddNodeGraph(EBtKind::Decorator, parent, wx, wy);
                    if (IsValidId(nid, m_Nodes.Size())) {
                        std::snprintf(m_Nodes[static_cast<usize>(nid)].ename,
                                      sizeof(m_Nodes[static_cast<usize>(nid)].ename), "%s", cn);
                        m_Nodes[static_cast<usize>(nid)].decoMode = EBtDecoMode::Condition;
                    }
                }
            }
        } else {
            ImGui::TextDisabled("(no conditions registered)");
        }
        ImGui::EndMenu();
    }

    // Compare デコレーター (変数と定数の比較で子をガード。var/op/const は Inspector で設定)。
    if (ImGui::MenuItem("Compare (var)")) {
        const u32 nid = AddNodeGraph(EBtKind::Decorator, parent, wx, wy);
        if (IsValidId(nid, m_Nodes.Size())) m_Nodes[static_cast<usize>(nid)].decoMode = EBtDecoMode::Compare;
    }

    ImGui::Separator();

    // Task (catalog の Action 関数にバインド)。
    if (ImGui::BeginMenu("Task (catalog)")) {
        if (m_Registry != nullptr && m_Registry->Count() > 0) {
            for (u32 t = 0; t < m_Registry->Count(); ++t) {
                const char* tn = m_Registry->NameAt(t);
                if (ImGui::MenuItem(tn)) {
                    const u32 nid = AddNodeGraph(EBtKind::Task, parent, wx, wy);
                    if (IsValidId(nid, m_Nodes.Size()))
                        std::snprintf(m_Nodes[static_cast<usize>(nid)].ename,
                                      sizeof(m_Nodes[static_cast<usize>(nid)].ename), "%s", tn);
                }
            }
        } else {
            ImGui::TextDisabled("(no actions registered)");
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Task (blank)"))   AddNodeGraph(EBtKind::Task,   parent, wx, wy);
    if (ImGui::MenuItem("Action (blank)")) AddNodeGraph(EBtKind::Action, parent, wx, wy);
}

/** ノードグラフ canvas を描画 + 操作する。 */
void FBehaviorTreeEditorPanel::DrawGraph() noexcept {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 80.0f) size.x = 80.0f;
    if (size.y < 80.0f) size.y = 80.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (!m_DidLayout && !m_Nodes.IsEmpty()) { AutoLayout(); m_DidLayout = true; }

    const ImVec2 br = ImVec2(origin.x + size.x, origin.y + size.y);
    const float  z  = m_Zoom;

    // 背景 + グリッド
    dl->AddRectFilled(origin, br, IM_COL32(22, 24, 30, 255));
    const float grid = 32.0f * z;
    if (grid > 5.0f) {
        for (float gx = m_PanX - floorf(m_PanX / grid) * grid; gx < size.x; gx += grid)
            dl->AddLine(ImVec2(origin.x + gx, origin.y), ImVec2(origin.x + gx, br.y), IM_COL32(38, 42, 50, 255));
        for (float gy = m_PanY - floorf(m_PanY / grid) * grid; gy < size.y; gy += grid)
            dl->AddLine(ImVec2(origin.x, origin.y + gy), ImVec2(br.x, origin.y + gy), IM_COL32(38, 42, 50, 255));
    }
    dl->PushClipRect(origin, br, true);

    const float NW = 156.0f, NH = 52.0f;
#define BT_S(wx, wy) ImVec2(origin.x + m_PanX + (wx) * z, origin.y + m_PanY + (wy) * z)

    // エッジ (親→子)。直近 tick で訪問された子へのエッジは status 色で太く光らせ、
    // 「今どの経路を処理が流れたか」を可視化する。
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        const NodeMeta& n = m_Nodes[i];
        if (!n.alive || !IsValidId(n.parent_id, m_Nodes.Size())) continue;
        const NodeMeta& p = m_Nodes[static_cast<usize>(n.parent_id)];
        if (!p.alive) continue;
        const ImVec2 a = BT_S(p.x + NW * 0.5f, p.y + NH);
        const ImVec2 b = BT_S(n.x + NW * 0.5f, n.y);
        const float dyy = (b.y - a.y) * 0.5f;
        ImU32 ecol; float ew;
        if (n.visit_order > 0u) {
            f32 c[4]; StatusColor(n.last_status, c);
            ecol = IM_COL32(static_cast<int>(c[0]*255), static_cast<int>(c[1]*255), static_cast<int>(c[2]*255), 255);
            ew   = 3.5f;
        } else {
            ecol = IM_COL32(78, 86, 100, 255);
            ew   = 2.0f;
        }
        dl->AddBezierCubic(a, ImVec2(a.x, a.y + dyy), ImVec2(b.x, b.y - dyy), b, ecol, ew);
    }

    // リンクドラッグのプレビュー
    if (IsValidId(m_LinkSrc, m_Nodes.Size())) {
        const NodeMeta& s = m_Nodes[static_cast<usize>(m_LinkSrc)];
        const ImVec2 a = BT_S(s.x + NW * 0.5f, s.y + NH);
        const ImVec2 b = io.MousePos;
        const float dyy = (b.y - a.y) * 0.5f;
        dl->AddBezierCubic(a, ImVec2(a.x, a.y + dyy), ImVec2(b.x, b.y - dyy), b, IM_COL32(255, 215, 90, 230), 2.5f);
    }

    // ノード
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        const NodeMeta& n = m_Nodes[i];
        if (!n.alive) continue;
        const ImVec2 p0 = BT_S(n.x, n.y);
        const ImVec2 p1 = BT_S(n.x + NW, n.y + NH);
        ImU32 kindcol;
        switch (n.kind) {
            case EBtKind::Selector:  kindcol = IM_COL32(60, 110, 200, 255); break; // 青
            case EBtKind::Sequence:  kindcol = IM_COL32(210, 130, 50, 255); break; // 橙
            case EBtKind::Decorator: kindcol = IM_COL32(150, 90, 200, 255); break; // 紫
            case EBtKind::Task:      kindcol = IM_COL32(40, 160, 170, 255); break; // 青緑
            default:                 kindcol = IM_COL32(70, 160, 90, 255);  break; // Action 緑
        }
        const float th = 18.0f * z;
        dl->AddRectFilled(p0, p1, IM_COL32(44, 48, 60, 255), 6.0f);
        dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + th), kindcol, 6.0f, ImDrawFlags_RoundCornersTop);
        const bool sel = (m_Selected == static_cast<u32>(i));
        dl->AddRect(p0, p1, sel ? IM_COL32(255, 215, 90, 255) : IM_COL32(90, 100, 120, 255), 6.0f, 0, sel ? 2.5f : 1.0f);

        // 実行フロー: 直近 tick で訪問されたノードを status 色で発光 + 訪問順バッジ。
        if (n.visit_order > 0u) {
            f32 gc[4]; StatusColor(n.last_status, gc);
            const ImU32 glow = IM_COL32(static_cast<int>(gc[0]*255), static_cast<int>(gc[1]*255), static_cast<int>(gc[2]*255), 200);
            dl->AddRect(ImVec2(p0.x - 2.0f, p0.y - 2.0f), ImVec2(p1.x + 2.0f, p1.y + 2.0f), glow, 7.0f, 0, 2.0f);
            char ob[8]; std::snprintf(ob, sizeof(ob), "%u", static_cast<unsigned>(n.visit_order));
            const ImVec2 bp = ImVec2(p0.x - 5.0f * z, p0.y - 5.0f * z);
            dl->AddCircleFilled(bp, 8.0f * z, IM_COL32(20, 22, 28, 255));
            dl->AddCircle(bp, 8.0f * z, glow, 0, 1.5f);
            dl->AddText(ImGui::GetFont(), 11.0f * z, ImVec2(bp.x - 3.5f * z, bp.y - 6.5f * z), IM_COL32(238, 238, 242, 255), ob);
        }
        // 未解決ノードを赤 "?" で警告: leaf(registry未登録) / Condition(条件未登録) / Compare(変数未定義)。
        bool unbound = false;
        if (BtKindIsLeaf(n.kind)) {
            unbound = (m_Registry != nullptr) && (m_Registry->Find(DisplayName(n)) == nullptr);
        } else if (n.kind == EBtKind::Decorator && n.decoMode == EBtDecoMode::Condition) {
            unbound = (m_CondReg != nullptr) && (m_CondReg->Find(DisplayName(n)) == nullptr);
        } else if (n.kind == EBtKind::Decorator && n.decoMode == EBtDecoMode::Compare) {
            const bool known = (m_DynBb  != nullptr && m_DynBb->Has(n.var))
                            || (m_Schema != nullptr && m_Schema->IndexOf(n.var) != FBtBlackboardSchema::kInvalid);
            unbound = (m_DynBb != nullptr || m_Schema != nullptr) && (n.var[0] != '\0') && !known;
        }
        if (unbound) {
            dl->AddText(ImGui::GetFont(), 13.0f * z, ImVec2(p1.x - 13.0f * z, p1.y - 16.0f * z), IM_COL32(255, 90, 90, 255), "?");
        }

        f32 col[4]; StatusColor(n.last_status, col);
        dl->AddCircleFilled(ImVec2(p1.x - 9.0f * z, p0.y + th * 0.5f), 4.0f * z,
                            IM_COL32(static_cast<int>(col[0]*255), static_cast<int>(col[1]*255), static_cast<int>(col[2]*255), 255));
        if (z > 0.45f) {
            // ヘッダ: 種別。Decorator は mode に応じて変換 op / 条件 / 比較式を併記。
            char head[48];
            if (n.kind == EBtKind::Decorator) {
                switch (n.decoMode) {
                    case EBtDecoMode::Transform:
                        std::snprintf(head, sizeof(head), "Decorator: %s", DecoLabel(n.deco)); break;
                    case EBtDecoMode::Condition:
                        std::snprintf(head, sizeof(head), "Condition"); break;
                    case EBtDecoMode::Compare:
                        std::snprintf(head, sizeof(head), "If %s %s %g",
                                      (n.var[0] != '\0' ? n.var : "?"), CmpOpLabel(n.cmpOp),
                                      static_cast<double>(n.cmpRhs)); break;
                }
            } else {
                std::snprintf(head, sizeof(head), "%s", KindLabel(n.kind));
            }
            dl->AddText(ImGui::GetFont(), 13.0f * z, ImVec2(p0.x + 8.0f * z, p0.y + 2.0f * z),  IM_COL32(248, 248, 250, 255), head);
            dl->AddText(ImGui::GetFont(), 12.0f * z, ImVec2(p0.x + 8.0f * z, p0.y + th + 5.0f * z), IM_COL32(206, 212, 224, 255), DisplayName(n));
        }
        dl->AddCircleFilled(BT_S(n.x + NW * 0.5f, n.y), 4.0f * z, IM_COL32(180, 190, 205, 255));   // 入力(上)
        if (!BtKindIsLeaf(n.kind))
            dl->AddCircleFilled(BT_S(n.x + NW * 0.5f, n.y + NH), 4.5f * z, IM_COL32(255, 215, 90, 255)); // 出力(下: composite/decorator)
    }
    dl->PopClipRect();

    // ===== 操作 =====
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##bt_canvas", size,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 mp = io.MousePos;

    u32 hitN = kInvalidId, hitOut = kInvalidId;
    for (int i = static_cast<int>(m_Nodes.Size()) - 1; i >= 0; --i) {
        const NodeMeta& n = m_Nodes[static_cast<usize>(i)];
        if (!n.alive) continue;
        if (!BtKindIsLeaf(n.kind) && hitOut == kInvalidId) {
            const ImVec2 pc = BT_S(n.x + NW * 0.5f, n.y + NH);
            const float dx = mp.x - pc.x, dy = mp.y - pc.y;
            if (dx * dx + dy * dy <= (10.0f * z) * (10.0f * z)) hitOut = static_cast<u32>(i);
        }
        if (hitN == kInvalidId) {
            const ImVec2 p0 = BT_S(n.x, n.y), p1 = BT_S(n.x + NW, n.y + NH);
            if (mp.x >= p0.x && mp.x <= p1.x && mp.y >= p0.y && mp.y <= p1.y) hitN = static_cast<u32>(i);
        }
    }

    if (hov && io.MouseWheel != 0.0f) {
        const float oldZ = m_Zoom;
        float nz = m_Zoom * (1.0f + io.MouseWheel * 0.12f);
        nz = (nz < 0.3f) ? 0.3f : (nz > 2.5f ? 2.5f : nz);
        m_Zoom = nz;
        const float relx = mp.x - origin.x, rely = mp.y - origin.y;
        m_PanX = relx - (relx - m_PanX) * (nz / oldZ);
        m_PanY = rely - (rely - m_PanY) * (nz / oldZ);
    }

    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hitOut != kInvalidId)      { m_LinkSrc = hitOut; }
        else if (hitN != kInvalidId)   { m_Selected = hitN; m_DragNode = hitN; }
        else                           { m_Selected = kInvalidId; m_PanningBg = true; }
    }
    if (IsValidId(m_DragNode, m_Nodes.Size()) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_Nodes[static_cast<usize>(m_DragNode)].x += io.MouseDelta.x / z;
        m_Nodes[static_cast<usize>(m_DragNode)].y += io.MouseDelta.y / z;
    }
    if (m_PanningBg && ImGui::IsMouseDown(ImGuiMouseButton_Left)) { m_PanX += io.MouseDelta.x; m_PanY += io.MouseDelta.y; }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))         { m_PanX += io.MouseDelta.x; m_PanY += io.MouseDelta.y; }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (m_LinkSrc != kInvalidId) {
            if (hitN != kInvalidId && hitN != m_LinkSrc && !IsAncestor(hitN, m_LinkSrc)) {
                // Decorator は単子: 既存の子 (hitN 以外) を root へ外してから付け替える。
                if (m_Nodes[static_cast<usize>(m_LinkSrc)].kind == EBtKind::Decorator) {
                    for (usize i = 0; i < m_Nodes.Size(); ++i) {
                        if (m_Nodes[i].alive && m_Nodes[i].parent_id == m_LinkSrc
                            && static_cast<u32>(i) != hitN) {
                            m_Nodes[i].parent_id = kInvalidId;
                        }
                    }
                }
                m_Nodes[static_cast<usize>(hitN)].parent_id = m_LinkSrc;  // 接続 (子の親を付け替え)
            }
            m_LinkSrc = kInvalidId;
        }
        m_DragNode  = kInvalidId;
        m_PanningBg = false;
    }

    // 右クリックメニュー
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hitN != kInvalidId) { m_CtxNode = hitN; m_Selected = hitN; ImGui::OpenPopup("##bt_node_ctx"); }
        else {
            m_AddX = (mp.x - origin.x - m_PanX) / z;
            m_AddY = (mp.y - origin.y - m_PanY) / z;
            ImGui::OpenPopup("##bt_add");
        }
    }
    if (ImGui::BeginPopup("##bt_add")) {
        ImGui::TextDisabled("Add node"); ImGui::Separator();
        DrawAddMenu(kInvalidId, m_AddX, m_AddY);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##bt_node_ctx")) {
        if (IsValidId(m_CtxNode, m_Nodes.Size())) {
            const f32 ax = m_Nodes[static_cast<usize>(m_CtxNode)].x + 40.0f;
            const f32 ay = m_Nodes[static_cast<usize>(m_CtxNode)].y + kRowH;
            ImGui::SetNextItemWidth(170.0f);
            ImGui::InputText("name", m_Nodes[static_cast<usize>(m_CtxNode)].ename,
                             sizeof(m_Nodes[static_cast<usize>(m_CtxNode)].ename));
            // Decorator が既に子を持つ場合、Add child は単子制約で既存子を置換する旨を明示。
            if (m_Nodes[static_cast<usize>(m_CtxNode)].kind == EBtKind::Decorator) {
                u32 ctx_kids = 0;
                for (usize i = 0; i < m_Nodes.Size(); ++i)
                    if (m_Nodes[i].alive && m_Nodes[i].parent_id == m_CtxNode) ++ctx_kids;
                if (ctx_kids >= 1) ImGui::TextDisabled("(Add child replaces current child)");
            }
            // leaf (Action/Task) は子を持てないので Add child を出さない。
            if (!BtKindIsLeaf(m_Nodes[static_cast<usize>(m_CtxNode)].kind) && ImGui::BeginMenu("Add child")) {
                DrawAddMenu(m_CtxNode, ax, ay);
                ImGui::EndMenu();
            }
            // Decorator は op をその場で切替できると便利。
            if (m_Nodes[static_cast<usize>(m_CtxNode)].kind == EBtKind::Decorator
                && ImGui::BeginMenu("Decorator op")) {
                for (int d = 0; d < 4; ++d) {
                    const EBtDecoratorOp op = static_cast<EBtDecoratorOp>(d);
                    const bool cur = (m_Nodes[static_cast<usize>(m_CtxNode)].deco == op);
                    if (ImGui::MenuItem(DecoLabel(op), nullptr, cur)) SetNodeDecoratorOp(m_CtxNode, op);
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Detach (root)")) m_Nodes[static_cast<usize>(m_CtxNode)].parent_id = kInvalidId;
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) DeleteNodeGraph(m_CtxNode);
        }
        ImGui::EndPopup();
    }
#undef BT_S
}

namespace {
/** FNV-1a 風の畳み込みヘルパ (undo の変更検出シグネチャ用)。 */
inline u64 SigMix(u64 h, u64 v) noexcept { return (h ^ v) * 1099511628211ull; }
inline u64 SigF32(u64 h, f32 f) noexcept { u32 b = 0; std::memcpy(&b, &f, sizeof(b)); return SigMix(h, b); }
inline u64 SigStr(u64 h, const char* s) noexcept {
    if (s != nullptr) for (; *s != '\0'; ++s) h = SigMix(h, static_cast<u64>(static_cast<unsigned char>(*s)));
    return h;
}
/** undo 履歴の上限 (memory ガード)。 */
constexpr u32 kMaxUndo = 64u;
} // namespace

/** 現在のグラフ状態 (ノード + 動的 BB) を out へコピーする。 */
void FBehaviorTreeEditorPanel::CaptureSnapshot(GraphSnapshot& out) const noexcept {
    out.nodes.Clear();
    out.nodes.Reserve(m_Nodes.Size());
    for (usize i = 0; i < m_Nodes.Size(); ++i) out.nodes.PushBack(m_Nodes[i]);   // NodeMeta 値コピー
    out.hasBb = (m_DynBb != nullptr);
    if (out.hasBb) out.bb = *m_DynBb;                                            // FBtBlackboard コピー
}

/** スナップショットから現在のグラフ状態を復元する。 */
void FBehaviorTreeEditorPanel::RestoreSnapshot(const GraphSnapshot& s) noexcept {
    m_Nodes.Clear();
    m_Nodes.Reserve(s.nodes.Size());
    for (usize i = 0; i < s.nodes.Size(); ++i) m_Nodes.PushBack(s.nodes[i]);
    if (s.hasBb && m_DynBb != nullptr) {
        // bb は「構造 (名前+型)」だけ snapshot に合わせ、生存変数の現在値は保持する。
        // signature は bb 値を追跡しない (graph-run の値変動でフラッドしないため) ので、
        // 構造編集の undo で間に挟まった値 poke を stale 値へ巻き戻さないよう値を引き継ぐ。
        FBtBlackboard rebuilt;
        for (u32 i = 0; i < s.bb.Count(); ++i) {
            const char*      nm = s.bb.NameAt(i);
            const EBtVarType ty = s.bb.TypeAt(i);
            rebuilt.Add(nm, ty);
            if (m_DynBb->Has(nm)) {                       // 現在値を引き継ぐ (新規復活変数は 0)
                const f32 cur = m_DynBb->GetAsF32(nm);
                switch (ty) {
                    case EBtVarType::Bool: rebuilt.SetBool(nm, cur != 0.0f); break;
                    case EBtVarType::I32:  rebuilt.SetI32(nm, static_cast<acs::i32>(cur)); break;
                    case EBtVarType::F32:  rebuilt.SetF32(nm, cur); break;
                }
            }
        }
        *m_DynBb = rebuilt;
    }
    // 進行中の操作状態はリセット (復元したノードに対して無効なため)。
    m_Selected = kInvalidId; m_DragNode = kInvalidId; m_LinkSrc = kInvalidId;
    m_CtxNode  = kInvalidId; m_PanningBg = false;
    m_DidLayout = true;   // 復元座標を尊重 (auto-layout で潰さない)
}

/** 現在のグラフ状態の変更検出用シグネチャ。 */
u64 FBehaviorTreeEditorPanel::GraphSignature() const noexcept {
    u64 h = 1469598103934665603ull;
    h = SigMix(h, static_cast<u64>(m_Nodes.Size()));
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        const NodeMeta& n = m_Nodes[i];
        h = SigMix(h, n.alive ? 1u : 0u);
        if (!n.alive) continue;
        h = SigMix(h, n.parent_id);
        h = SigMix(h, static_cast<u64>(n.kind));
        h = SigMix(h, static_cast<u64>(n.deco));
        h = SigMix(h, static_cast<u64>(n.decoMode));
        h = SigMix(h, static_cast<u64>(n.cmpOp));
        h = SigF32(h, n.cmpRhs);
        h = SigF32(h, n.x);
        h = SigF32(h, n.y);
        h = SigStr(h, n.ename);
        h = SigStr(h, n.name);
        h = SigStr(h, n.var);
    }
    // 動的 BB は「変数の構成 (名前・型・個数)」だけを追跡し、値は含めない。
    // 値を含めると graph-run 中に条件関数が変数を書き換えるたびに「変更」と誤検知し、
    // undo 履歴が毎フレーム溢れてしまうため。値の poke は undo 対象外 (transient なデバッグ操作)。
    if (m_DynBb != nullptr) {
        h = SigMix(h, static_cast<u64>(m_DynBb->Count()));
        for (u32 v = 0; v < m_DynBb->Count(); ++v) {
            h = SigStr(h, m_DynBb->NameAt(v));
            h = SigMix(h, static_cast<u64>(m_DynBb->TypeAt(v)));
        }
    }
    return h;
}

/** 現在の baseline を undo stack へ積み、baseline を現在状態へ更新する。 */
void FBehaviorTreeEditorPanel::CommitBaseline() noexcept {
    m_UndoStack.PushBack(Move(m_UndoBaseline));   // 直前の確定状態を保存
    CaptureSnapshot(m_UndoBaseline);              // 新しい baseline = 現在
    m_BaselineSig = GraphSignature();
    m_RedoStack.Clear();                          // 新しい分岐 → redo 無効化

    // 上限超過分は古い物 (front) から落とす。
    while (m_UndoStack.Size() > static_cast<usize>(kMaxUndo)) {
        for (usize i = 0; i + 1 < m_UndoStack.Size(); ++i) m_UndoStack[i] = Move(m_UndoStack[i + 1]);
        m_UndoStack.PopBack();
    }
}

/** 毎フレーム呼び、編集が確定したら baseline を undo stack へ積む。 */
void FBehaviorTreeEditorPanel::UpdateUndoTracking() noexcept {
    if (!m_UndoInit) {
        CaptureSnapshot(m_UndoBaseline);
        m_BaselineSig = GraphSignature();
        m_UndoInit = true;
        return;
    }
    // 編集中 (マウス押下 / テキスト入力中) はコミットしない → drag/typing を 1 手にまとめる。
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsAnyItemActive()) return;
    if (GraphSignature() == m_BaselineSig) return;   // 変更なし
    CommitBaseline();
}

/** 現在のグラフ状態を undo 履歴へ明示的にコミットする (チェックポイント)。 */
void FBehaviorTreeEditorPanel::PushUndoCheckpoint() noexcept {
    if (!m_UndoInit) {   // 初回は baseline 初期化のみ
        CaptureSnapshot(m_UndoBaseline);
        m_BaselineSig = GraphSignature();
        m_UndoInit = true;
        return;
    }
    CommitBaseline();
}

/** 1 手戻す。 */
void FBehaviorTreeEditorPanel::Undo() noexcept {
    if (m_UndoStack.IsEmpty()) return;
    GraphSnapshot cur; CaptureSnapshot(cur);
    m_RedoStack.PushBack(Move(cur));                                 // 現在 → redo
    GraphSnapshot prev = Move(m_UndoStack[m_UndoStack.Size() - 1]);
    m_UndoStack.PopBack();
    RestoreSnapshot(prev);
    m_BaselineSig  = GraphSignature();
    m_UndoBaseline = Move(prev);
}

/** 1 手やり直す。 */
void FBehaviorTreeEditorPanel::Redo() noexcept {
    if (m_RedoStack.IsEmpty()) return;
    GraphSnapshot cur; CaptureSnapshot(cur);
    m_UndoStack.PushBack(Move(cur));                                 // 現在 → undo
    GraphSnapshot next = Move(m_RedoStack[m_RedoStack.Size() - 1]);
    m_RedoStack.PopBack();
    RestoreSnapshot(next);
    m_BaselineSig  = GraphSignature();
    m_UndoBaseline = Move(next);
}

/** toolbar + history graph + 左 tree view + 右 node inspector を描画する。 */
void FBehaviorTreeEditorPanel::DrawUI() noexcept {
    if (!IsVisible()) return;

    // ノードグラフが見えるよう十分な初期サイズを与える (ユーザが変えたら保存値を尊重)。
    ImGui::SetNextWindowSize(ImVec2(960.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Title(), &m_Visible)) {
        ImGui::End();
        return;
    }

    // キーボードショートカット: Ctrl+Z = undo / Ctrl+Shift+Z or Ctrl+Y = redo。
    // テキスト入力中は item 側の undo を優先するため無効化する。
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) Redo(); else Undo(); }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
    }

    // (1) Toolbar 行: Reset / Undo / Redo / Step / Continuous(autorun) | Active / Step counter
    if (ImGui::Button("Reset")) {
        Reset();
    }
    ImGui::SameLine();
    {
        const bool can_undo = !m_UndoStack.IsEmpty();
        if (!can_undo) ImGui::BeginDisabled();
        if (ImGui::Button("Undo")) Undo();
        if (!can_undo) ImGui::EndDisabled();
        ImGui::SameLine();
        const bool can_redo = !m_RedoStack.IsEmpty();
        if (!can_redo) ImGui::BeginDisabled();
        if (ImGui::Button("Redo")) Redo();
        if (!can_redo) ImGui::EndDisabled();
    }
    ImGui::SameLine();
    // Step は実行対象 (tree もしくは graph レジストリ) が無ければ disable。
    const bool step_enabled = (m_Tree != nullptr) || (m_Registry != nullptr);
    if (!step_enabled) ImGui::BeginDisabled();
    if (ImGui::Button("Step")) {
        StepOnce();
    }
    if (!step_enabled) ImGui::EndDisabled();

    ImGui::SameLine();
    // Continuous: autorun toggle。ON/OFF をラベルで明示。
    {
        bool autorun = m_Autorun;
        if (ImGui::Checkbox("Continuous", &autorun)) {
            m_Autorun = autorun;
        }
    }

    ImGui::SameLine();
    // 表示モード切替: ノードグラフ / 従来のツリーリスト。
    if (ImGui::Button(m_GraphMode ? "View: Graph" : "View: Tree")) {
        m_GraphMode = !m_GraphMode;
    }
    ImGui::SameLine();
    // ルートノードを追加 (空のツリーから組み始めるとき用)。
    if (ImGui::Button("+ Root")) {
        AddNodeGraph(EBtKind::Selector, kInvalidId, 60.0f, 40.0f);
        m_GraphMode = true;
    }
    ImGui::SameLine();
    // グラフの保存 / 読み込み (固定ファイル)。
    if (ImGui::Button("Save")) SaveGraph("behavior_tree.btg");
    ImGui::SameLine();
    if (ImGui::Button("Load")) { LoadGraph("behavior_tree.btg"); m_GraphMode = true; }
    if (m_Registry != nullptr) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "[graph-run]");  // no-code 実行モード
    }

    ImGui::SameLine();
    // Active node count: status != Failure な node 数を毎フレーム集計。
    // (= Inspector で「今いくつの node が Success/Running か」を一目で見る指標)
    u32 active = 0;
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        if (m_Nodes[i].last_status != EBtStatus::Failure) ++active;
    }
    ImGui::Text("| Active: %u  Step: %u",
                static_cast<unsigned>(active),
                static_cast<unsigned>(m_StepCount));

    ImGui::Separator();

    // (2) History graph: 60 frame の root status を PlotLines で表示
    // ring buffer を新→旧の時系列順に float へ展開する。
    // m_HistoryHead は「次に書き込む位置」 = "ちょうど 1 frame 前 + 1" なので、
    // PlotLines に対して `(head, head+1, ..., head+kHistorySize-1) mod size` の
    // 順に並べると左 → 右 = 古い → 新しい時系列になる。
    if (!m_History.IsEmpty()) {
        f32 plot[kHistorySize];
        for (u32 i = 0; i < kHistorySize; ++i) {
            const u32 idx = (m_HistoryHead + i) % kHistorySize;
            const EBtStatus s = static_cast<EBtStatus>(m_History[idx]);
            plot[i] = StatusToPlotValue(s);
        }
        // overlay 表示: 最新値の文字列ラベル。
        char overlay[32];
        const u32 last_idx = (m_HistoryHead + kHistorySize - 1u) % kHistorySize;
        std::snprintf(overlay, sizeof(overlay), "Root: %s",
                      StatusLabel(static_cast<EBtStatus>(m_History[last_idx])));
        ImGui::PlotLines("##bt_history", plot, static_cast<int>(kHistorySize),
                         0, overlay,
                         0.0f, 1.0f, // y range [Failure=0 .. Success=1]
                         ImVec2(0.0f, 60.0f));
    }

    ImGui::Separator();

    // (3) 2 カラム: 左 Tree View / 右 Node Inspector
    const float content_w = ImGui::GetContentRegionAvail().x;
    const float left_w    = m_GraphMode ? content_w * 0.74f
                          : ((content_w > 540.0f) ? content_w * 0.55f : content_w * 0.50f);

    // 左カラム: ノードグラフ (m_GraphMode) または従来のツリーリスト
    ImGui::BeginChild("##bt_tree_left", ImVec2(left_w, 0),
                      m_GraphMode ? false : true,
                      m_GraphMode ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                                  : 0);
    {
        if (m_GraphMode) {
            if (m_Nodes.IsEmpty()) {
                ImGui::TextDisabled("(No nodes) — right-click to add, or press [+ Root].");
            }
            DrawGraph();
        } else {
            ImGui::TextUnformatted("Behavior Tree");
            ImGui::Separator();
            if (m_Nodes.IsEmpty()) {
                ImGui::TextDisabled("(No nodes registered)");
                ImGui::TextDisabled("Call panel.AddNode(kind, name, parent_id) from your sample.");
            } else {
                // root (= parent_id == kInvalidId) を全て描画。複数 root も許容 (forest)。
                bool drew_any = false;
                for (u32 i = 0; i < m_Nodes.Size(); ++i) {
                    if (m_Nodes[i].alive && m_Nodes[i].parent_id == kInvalidId) {
                        DrawTreeRecursive(i, 0u);
                        drew_any = true;
                    }
                }
                if (!drew_any) {
                    ImGui::TextDisabled("(No root node — possible cycle)");
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // 右カラム: Node Inspector
    ImGui::BeginChild("##bt_inspector_right", ImVec2(0, 0), true);
    {
        ImGui::TextUnformatted("Node Inspector");
        ImGui::Separator();

        if (!IsValidId(m_Selected, m_Nodes.Size()) || !m_Nodes[static_cast<usize>(m_Selected)].alive) {
            ImGui::TextDisabled("(No node selected)");
            ImGui::TextDisabled(m_GraphMode ? "Click a node. Right-click: add/delete."
                                            : "Click a node in the tree view.");
        } else {
            NodeMeta& n = m_Nodes[static_cast<usize>(m_Selected)];

            // child count を線形走査でカウント (leaf なら 0、生存のみ)。
            u32 child_count = 0;
            if (!BtKindIsLeaf(n.kind)) {
                for (usize i = 0; i < m_Nodes.Size(); ++i) {
                    if (m_Nodes[i].alive && m_Nodes[i].parent_id == m_Selected) ++child_count;
                }
            }

            // 名前はインスペクタから直接編集可能。
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##bt_name", n.ename, sizeof(n.ename));
            ImGui::Text("Name     : %s", DisplayName(n));
            ImGui::Text("Kind     : %s", KindLabel(n.kind));
            ImGui::Text("Id       : %u", static_cast<unsigned>(n.id));
            if (n.parent_id == kInvalidId) {
                ImGui::Text("Parent   : (root)");
            } else {
                ImGui::Text("Parent   : %u", static_cast<unsigned>(n.parent_id));
            }
            ImGui::Text("Children : %u", static_cast<unsigned>(child_count));

            // Decorator: モード (Transform / Condition / Compare) と各モードの設定を編集。
            if (n.kind == EBtKind::Decorator) {
                int modeI = static_cast<int>(n.decoMode);
                const char* modes[] = { "Transform (op)", "Condition (fn)", "Compare (var)" };
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("##bt_decomode", &modeI, modes, 3)) {
                    n.decoMode = static_cast<EBtDecoMode>(modeI);
                }

                if (n.decoMode == EBtDecoMode::Transform) {
                    // 結果変換 op。
                    int cur = static_cast<int>(n.deco);
                    const char* items[] = { "Inverter", "ForceSuccess", "ForceFailure", "Repeat" };
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::Combo("##bt_deco", &cur, items, 4)) n.deco = static_cast<EBtDecoratorOp>(cur);
                } else if (n.decoMode == EBtDecoMode::Condition) {
                    // 条件関数を catalog から選ぶ (ノード名 = 関数名)。
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::BeginCombo("##bt_condpick", DisplayName(n))) {
                        if (m_CondReg != nullptr) {
                            for (u32 c = 0; c < m_CondReg->Count(); ++c) {
                                const char* cn = m_CondReg->NameAt(c);
                                if (ImGui::Selectable(cn)) std::snprintf(n.ename, sizeof(n.ename), "%s", cn);
                            }
                        } else {
                            ImGui::TextDisabled("(no condition registry set)");
                        }
                        ImGui::EndCombo();
                    }
                    if (m_CondReg != nullptr) {
                        const bool bound = (m_CondReg->Find(DisplayName(n)) != nullptr);
                        ImGui::TextColored(bound ? ImVec4(0.4f, 0.9f, 0.5f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                           "Condition : %s", bound ? "bound" : "UNBOUND");
                    }
                } else { // EBtDecoMode::Compare
                    // 変数を schema から選び、op と定数を指定 (var <op> const)。
                    ImGui::SetNextItemWidth(130.0f);
                    if (ImGui::BeginCombo("var", (n.var[0] != '\0' ? n.var : "(pick)"))) {
                        bool any = false;
                        if (m_DynBb != nullptr) {                       // 動的 BB の変数
                            for (u32 v = 0; v < m_DynBb->Count(); ++v) {
                                const char* vn = m_DynBb->NameAt(v);
                                if (ImGui::Selectable(vn)) std::snprintf(n.var, sizeof(n.var), "%s", vn);
                                any = true;
                            }
                        }
                        if (m_Schema != nullptr) {                      // offset スキーマの変数
                            for (u32 v = 0; v < m_Schema->Count(); ++v) {
                                const char* vn = m_Schema->NameAt(v);
                                if (ImGui::Selectable(vn)) std::snprintf(n.var, sizeof(n.var), "%s", vn);
                                any = true;
                            }
                        }
                        if (!any) ImGui::TextDisabled("(no blackboard variables)");
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    int opI = static_cast<int>(n.cmpOp);
                    const char* ops[] = { "<", "<=", "==", "!=", ">=", ">" };
                    ImGui::SetNextItemWidth(56.0f);
                    if (ImGui::Combo("##bt_cmpop", &opI, ops, 6)) n.cmpOp = static_cast<EBtCompareOp>(opI);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputFloat("##bt_cmprhs", &n.cmpRhs, 0.0f, 0.0f, "%.2f");
                    // 動的 BB と offset スキーマの「両方」に無いときだけ未解決警告
                    // (canvas の "?" バッジ / インタプリタの解決順と一致させる)。
                    if (n.var[0] != '\0' && (m_DynBb != nullptr || m_Schema != nullptr)) {
                        const bool known = (m_DynBb  != nullptr && m_DynBb->Has(n.var))
                                        || (m_Schema != nullptr && m_Schema->IndexOf(n.var) != FBtBlackboardSchema::kInvalid);
                        if (!known) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "var not bound");
                    }
                }

                if (n.decoMode != EBtDecoMode::Transform) {
                    ImGui::TextDisabled("true => run child, false => Failure");
                }
                if (child_count > 1) {
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                       "Note: decorator uses 1 child (leftmost).");
                }
            }
            // leaf は registry 解決状況を表示 (graph-run 時)。
            if (BtKindIsLeaf(n.kind) && m_Registry != nullptr) {
                const bool bound = (m_Registry->Find(DisplayName(n)) != nullptr);
                ImGui::TextColored(bound ? ImVec4(0.4f, 0.9f, 0.5f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "Action fn : %s", bound ? "bound" : "UNBOUND (rename to match registry)");
            }

            ImGui::Separator();

            // Last Status (status color text)。
            f32 col[4];
            StatusColor(n.last_status, col);
            ImGui::TextUnformatted("Last Status : ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(col[0], col[1], col[2], col[3]),
                               "%s", StatusLabel(n.last_status));
        }

        // ===== Blackboard =====
        // 動的ブラックボード (エディタ所有): 変数の 追加 / リネーム / 型変更 / 値編集 / 削除。
        // ここで足した変数はそのまま Compare デコレーターの変数候補になり、値を poke すると
        // 実行中のグラフへ即反映される (= コードに無い変数をエディタだけで足して条件に使える)。
        if (m_DynBb != nullptr) {
            ImGui::Separator();
            ImGui::TextUnformatted("Blackboard (editable)");
            if (ImGui::SmallButton("+ Add var")) m_DynBb->Add(nullptr, EBtVarType::F32);
            u32 removeIdx = FBtBlackboard::kInvalid;
            // 幅に依存しない 2 行レイアウト: 行1 = [x][name]、行2 = [type][value]。
            for (u32 v = 0; v < m_DynBb->Count(); ++v) {
                ImGui::PushID(static_cast<int>(1000 + v));
                // 行1: 削除ボタン + 名前 (rename はバッファ直接編集、残り幅いっぱい)。
                if (ImGui::SmallButton("x")) removeIdx = v;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.0f);
                // 変数名は空白禁止 (シリアライズが 1 トークン %s で読むため、空白だと load 時に消える)。
                ImGui::InputText("##nm", m_DynBb->NameBufAt(v), FBtBlackboard::kNameLen,
                                 ImGuiInputTextFlags_CharsNoBlank);
                // 行2: 型コンボ + 値エディタ。
                int ti = static_cast<int>(m_DynBb->TypeAt(v));
                const char* types[] = { "Bool", "I32", "F32" };
                ImGui::SetNextItemWidth(64.0f);
                if (ImGui::Combo("##ty", &ti, types, 3)) m_DynBb->SetType(v, static_cast<EBtVarType>(ti));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.0f);
                switch (m_DynBb->TypeAt(v)) {
                    case EBtVarType::Bool: { bool* p = m_DynBb->BoolPtrAt(v); if (p) ImGui::Checkbox("##vl", p); break; }
                    case EBtVarType::I32:  { i32*  p = m_DynBb->I32PtrAt(v);  if (p) ImGui::InputInt("##vl", reinterpret_cast<int*>(p), 0, 0); break; }
                    case EBtVarType::F32:  { f32*  p = m_DynBb->F32PtrAt(v);  if (p) ImGui::InputFloat("##vl", p); break; }
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            if (removeIdx != FBtBlackboard::kInvalid) m_DynBb->Remove(removeIdx);
            ImGui::TextDisabled("Add/rename vars; edit a value to poke the tree.");
        }
        // offset スキーマ (コード所有、読み取り専用 schema): 値の poke のみ。
        if (m_Schema != nullptr && m_GraphBb != nullptr && m_Schema->Count() > 0) {
            ImGui::Separator();
            ImGui::TextUnformatted("Blackboard (schema, poke)");
            for (u32 v = 0; v < m_Schema->Count(); ++v) {
                const char* vn   = m_Schema->NameAt(v);
                char*       base = static_cast<char*>(m_GraphBb) + m_Schema->OffsetAt(v);
                ImGui::PushID(static_cast<int>(2000 + v));
                ImGui::SetNextItemWidth(130.0f);
                switch (m_Schema->TypeAt(v)) {
                    case EBtVarType::Bool: ImGui::Checkbox(vn,   reinterpret_cast<bool*>(base)); break;
                    case EBtVarType::I32:  ImGui::InputInt(vn,   reinterpret_cast<int*>(base));  break;
                    case EBtVarType::F32:  ImGui::InputFloat(vn, reinterpret_cast<f32*>(base));  break;
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();

    // この frame の編集が確定したら undo 履歴へコミットする (drag/typing は settle 後に 1 手)。
    UpdateUndoTracking();

    ImGui::End();
}

} // namespace acs::game::btedit
