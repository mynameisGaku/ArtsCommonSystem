// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — btedit / FBehaviorTreeEditorPanel (Phase 22)
//
// `gameframework/FBehaviorTree.h` (Pillar L) の BT を **可視化 + ライブデバッグ**
// するための ImGui パネル。Unity の Behavior Designer / Unreal の Behavior Tree
// Editor の `Debugger` モードに相当する役割を担う。Phase 22 では v1 = "visualize
// + step debug" にスコープを絞り、ノードのグラフ編集 (drag drop で
// composite に子追加 / 配置入替) は v2 (将来) で対応する。
//
// 役割分担:
//   ・本パネルは「**実行中の BT を観察する**」のが第一責務。FBehaviorTree 本体は
//     panel が直接 walk しない (= FBtSelector / FBtSequence の `_children` は private、
//     ACS は RTTI 無効で `dynamic_cast` も使えない、FBtAction の `_fn` も private、
//     という三重の事情で実体ツリーを panel から覗けない)。
//     代わりに「**メタデータミラー**」: ユーザ (sample / ゲーム側) が AddNode で
//     「親 id・kind・表示名」を panel に push し、panel はそのミラーを描画する。
//     実体 BT とメタミラーの構造が乖離しない責務はユーザ側にあるが、最も
//     一般的な「BT 構築直後にミラーも組み立てる」運用なら手書きでも整合は楽。
//   ・ノードごとの `last_status` は SetNodeStatus で push してもらう。`FBtAction`
//     の関数ポインタが Tick されるたびに `panel.SetNodeStatus(my_id, ret)` を
//     呼ぶ規約。composite (Selector / FSequence) の status は root から伝搬する
//     必要があるが、panel 側は気にしない (= ユーザ自由)。
//   ・StepOnce は `tree->Tick(nullptr, 0.016f)` を 1 回回す + step_count++ +
//     history ring へ root status を push する最小実装。blackboard は nullptr 固定。
//     blackboard を渡したい場合は、ユーザが SetOnStepCallback で independent な
//     tick 関数を登録し、そちらが `tree->Tick(my_bb, dt)` を呼ぶ。callback 登録時は
//     panel は `tree->Tick` を直接呼ばず、callback だけを呼ぶ (= 排他)。
//
// 設計選択 (Pillar 22 — btedit 第一弾):
//   ・**FEditorPanel 継承**: Phase 21a で確立した editor_core 基底に乗せる。
//     FEditorWorkspace に登録するだけで自動 dispatch される。Title は
//     "Behavior Tree Editor"。
//   ・**メタミラー方式 (前述)**: FBehaviorTree.h の API を改造しないために採用。
//     panel 内に `TArray<NodeMeta>` を持ち、AddNode で順次積む。FNodeId は 0 から
//     panel が払い出す (= 1 panel 内で unique、複数 BT を同 panel で扱う想定なし)。
//     parent_id は同 TArray 内の id (== index、payload と一致)。kInvalidId
//     (= 0xFFFFFFFFu) を root の parent_id として使う。
//   ・**EBtKind (Selector / FSequence / Action) を u8 enum で持つ**: 表示時の色分け
//     と TreeNode タイプ判別に使う。FBehaviorTree.h の EBtStatus と同じく u8 enum
//     で揃え、ACS の "E-prefix enum" 規約 (project_acs_coding_conventions) に従う。
//   ・**status 表示色は固定リテラル**: Success=緑 (0,1,0)、Failure=赤 (1,0,0)、
//     Running=黄 (1,1,0)。ImGui::PushStyleColor で TreeNode テキストに反映する。
//   ・**history ring buffer は固定長 60**: 60 frame ≈ 1 秒 @ 60 fps の窓。
//     `TArray<u8>` で各要素は EBtStatus の生値 (0/1/2)。`_history_head` が次に
//     書き込む位置 (circular)。Reset でクリア。ImGui::PlotLines に float buffer を
//     一度展開して渡す。
//   ・**SelectedNodeId は u32 (-1 = none)**: FParticleEditorPanel の `_selected:i32`
//     と違って u32 を採用する理由は FNodeId 自体が u32 ベース (= AddNode 払い出し
//     も u32)。none signal は `static_cast<u32>(-1) = 0xFFFFFFFF` で表現。
//   ・**Autorun**: 毎フレーム OnFrameBegin で 1 tick 進める toggle。ImGui 上では
//     "Continuous" ボタンと呼ぶ (ユーザに分かりやすい)。Step / Reset と排他では
//     なく、autorun ON 中でも Step ボタンは押せる (= 1 frame 強制進行)。
//   ・**StepCallback**: 関数ポインタ + void* user。`std::function` 不使用 (ACS 規約)。
//     callback が登録されていれば panel は `tree->Tick` を直接呼ばず callback だけ
//     呼ぶ (= ユーザに blackboard を渡す自由を与える)。null なら blackboard=nullptr
//     で直接 Tick する fallback。
//   ・**非コピー / 非ムーブ / 全 noexcept / STL 不使用 / `<string>` 禁止**: ACS 規約。
//     name は `const char*` リテラル / 永続文字列を想定 (panel は所有しない)。
//   ・**ImGui ヘッダは .cpp に閉じる**: FParticleEditorPanel / FInspectorPanel /
//     FModelInspectorPanel と同形。
//
// ImGui レイアウト (DrawUI):
//   ┌────────────── "Behavior Tree Editor" window ─────────────────┐
//   │ [Reset] [Step] [Continuous: ON/OFF] | Active: N | Step: M    │
//   │ ┌─ Tick history (60 frame ring graph) ─────────────────────┐ │
//   │ │  PlotLines: Success=1.0 / Running=0.5 / Failure=0.0       │ │
//   │ └───────────────────────────────────────────────────────────┘ │
//   │ ┌── left: Tree view ─────┐  ┌── right: Node Inspector ────┐ │
//   │ │  Selector ▶            │  │ Name : <selected name>      │ │
//   │ │   FSequence ▶           │  │ Kind : Selector/FSequence/   │ │
//   │ │    Action "Pickup" ●   │  │        Action               │ │
//   │ │    Action "Move"   ●   │  │ Id   : <u32>                │ │
//   │ │   FSequence ▶           │  │ Parent: <u32 or "(root)">   │ │
//   │ │    Action "Wait"   ●   │  │ Children: <u32>             │ │
//   │ │    Action "Attack" ●   │  │ Last Status: <colored text> │ │
//   │ └────────────────────────┘  └─────────────────────────────┘ │
//   └──────────────────────────────────────────────────────────────┘
//
// 範囲外 (Phase 22 v1、将来追加候補):
//   ・グラフ編集 (drag-drop でツリー再配置、AddChild / RemoveChild)
//   ・blackboard inspector (任意型の void* を可視化する仕組み)
//   ・条件ブレークポイント (特定 node が Running になったら autorun を止める等)
//   ・複数 BT の同時編集 (現状 1 panel = 1 tree)
//   ・time-scaled tick (= 通常の dt の N 倍速で進める、slow-motion debug)
//   ・履歴の長さ可変 (現状 60 frame 固定)
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "gameframework/BehaviorTree.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game::btedit {

// ---------------------------------------------------------------------------
// EBtKind — メタミラー上の node 種別 (実体 BT の FBtNode 派生に対応)
// ---------------------------------------------------------------------------
// 実体 BT 側で RTTI 抜きに種別を判定できないため、メタミラー側で明示的に
// 保持する。FBtSelector → Selector、FBtSequence → FSequence、FBtAction → Action
// に 1:1 対応する想定 (= 将来 BtParallel 等が追加されたらここに enum を増やす)。
enum class EBtKind : u8 {
    Selector = 0,
    Sequence = 1,
    Action   = 2,
};

// ---------------------------------------------------------------------------
// FBehaviorTreeEditorPanel — FBehaviorTree の visualize + step debug パネル
// ---------------------------------------------------------------------------
class FBehaviorTreeEditorPanel : public editor_core::FEditorPanel {
public:
    // StepCallback: panel が「1 tick 進めたい」時に呼ばれる関数ポインタ。
    // 登録されていれば panel は `tree->Tick(nullptr, dt)` を直接呼ばず、
    // この callback だけを呼ぶ (= ユーザに blackboard を渡す自由を与える)。
    // `user` は SetOnStepCallback の第二引数で渡したポインタがそのまま戻る。
    using StepCallback = void(*)(void* user, FBehaviorTree* tree, f32 dt) noexcept;

    // 履歴 ring buffer の長さ (frame 数)。仕様で 60 frame 固定。
    static constexpr u32 kHistorySize = 60u;

    // SelectedNodeId / parent_id の "none" シグネル。u32 max。
    static constexpr u32 kInvalidId   = 0xFFFFFFFFu;

    FBehaviorTreeEditorPanel() noexcept = default;
    ~FBehaviorTreeEditorPanel() noexcept override = default;

    // 非コピー / 非ムーブ: 内部 `TArray<NodeMeta>` + `TArray<u8>` の所有を曖昧に
    // しない (FEditorPanel 基底もデフォルトで非コピー / 非ムーブ宣言済)。
    FBehaviorTreeEditorPanel(const FBehaviorTreeEditorPanel&)            = delete;
    FBehaviorTreeEditorPanel& operator=(const FBehaviorTreeEditorPanel&) = delete;
    FBehaviorTreeEditorPanel(FBehaviorTreeEditorPanel&&)                 = delete;
    FBehaviorTreeEditorPanel& operator=(FBehaviorTreeEditorPanel&&)      = delete;

    // ----- ライフサイクル ---------------------------------------------------

    // 初期化: メタミラー / 履歴を空にし、autorun / step counter / selection を
    // default に戻す。多重 Init 可。callback はリセットしない (= 別操作)。
    void Init() noexcept;

    // 後片付け: メタミラーと履歴と callback を全部クリア。多重 Shutdown 可。
    void Shutdown() noexcept;

    // ----- FBehaviorTree 紐付け ---------------------------------------------

    // 観察対象の BT を差し替える (nullptr で解除)。所有しない (caller 所有)。
    // 差し替え時に Reset() 相当 (step counter / history / status を初期化) を
    // 行うが、メタミラーは触らない (= 同じ構造で別インスタンスを観察する用途)。
    void SetTree(FBehaviorTree* tree) noexcept;

    // 現在観察中の BT (なければ nullptr)。
    FBehaviorTree* CurrentTree() const noexcept { return _tree; }

    // ----- autorun / step 制御 ----------------------------------------------

    // autorun (= 毎フレーム自動 Tick) の現在値。
    bool IsAutorun() const noexcept { return _autorun; }
    // autorun を切替。ON にすると OnFrameBegin で毎 frame 1 tick 進む。
    void SetAutorun(bool b) noexcept { _autorun = b; }

    // 1 tick (dt = 0.016f 固定) だけ手動で進める。StepCallback が登録されていれば
    // そちらに委譲、無ければ `tree->Tick(nullptr, 0.016f)` を直接呼ぶ。
    // tree 未設定 (= CurrentTree() == nullptr) なら no-op。
    // history ring に root status を 1 件 push、step_count++。
    void StepOnce() noexcept;

    // step counter / history / 全 node の last_status を初期状態に戻す。
    // メタミラー (NodeMeta 配列) と autorun フラグは触らない。
    void Reset() noexcept;

    // 現在の step counter (Reset で 0 に戻る、StepOnce で +1)。
    u32 StepCount() const noexcept { return _step_count; }

    // ----- selection ---------------------------------------------------------

    // 現在の選択 node id。未選択は kInvalidId (= 0xFFFFFFFF)。
    u32 SelectedNodeId() const noexcept { return _selected; }

    // 選択 node を設定。範囲外 / kInvalidId 渡しで「未選択」に正規化される。
    void SelectNode(u32 node_id) noexcept;

    // ----- メタミラー操作 (= caller が AddNode で BT 構造を panel に教える) -

    // 新規 node をメタミラーに追加し、払い出した node_id (= 0..N-1) を返す。
    // ・kind         : Selector / FSequence / Action のいずれか
    // ・name         : ImGui 表示名 (リテラル / 永続領域、panel は所有しない)
    // ・parent_id    : 既存 node の id、root の場合は kInvalidId
    // parent_id が kInvalidId 以外で範囲外を指す場合は parent_id を kInvalidId
    // に置き換えて root 扱いで追加する (= 安全に倒す)。
    // 上限 (kMaxNodes) に達したら kInvalidId を返す (= 失敗を id で通知)。
    u32 AddNode(EBtKind kind, const char* name, u32 parent_id) noexcept;

    // 既存 node の last_status を更新する。FBtAction の Fn から呼ぶことを想定。
    // 範囲外は no-op。Reset で全 node が Failure に戻る。
    void SetNodeStatus(u32 node_id, EBtStatus status) noexcept;

    // メタミラー全削除 + selection 解除。BT 構造を組み直す前に呼ぶ。
    // (autorun / step counter / history はクリアしない、Reset を別途呼ぶこと)
    void ClearNodes() noexcept;

    // 現在登録済 node 数。
    u32 NodeCount() const noexcept { return static_cast<u32>(_nodes.Size()); }

    // 指定 id の node の last_status (範囲外は Failure)。
    EBtStatus NodeStatus(u32 node_id) const noexcept;

    // ----- callback 登録 ----------------------------------------------------

    // 1 tick 進める時の独自処理を登録 (blackboard を渡したい場合等)。
    // nullptr で解除 (= panel が `tree->Tick(nullptr, dt)` を直接呼ぶ fallback)。
    void SetOnStepCallback(StepCallback cb, void* user) noexcept;

    // ----- FEditorPanel override --------------------------------------------

    // ImGui::Begin に渡す window タイトル (リテラル)。
    const char* Title() const noexcept override { return "Behavior Tree Editor"; }

    // 毎フレーム呼ばれる non-ImGui hook。autorun が ON のときに dt で 1 tick 進める。
    // (Step は 0.016f 固定だが、autorun はゲームの実 dt を反映するため step 単位
    //  ではなく実時間進行に追従する。)
    void OnFrameBegin(f32 dt) noexcept override;

    // メイン ImGui 描画: toolbar / history graph / tree view / node inspector。
    void DrawUI() noexcept override;

    // ----- 公開定数 ---------------------------------------------------------

    // 同時登録可能 node 数の上限 (overflow ガード)。実用 BT で 128 で十分。
    // 上限到達時の AddNode は kInvalidId を返す (= silent fail 通知)。
    static constexpr u32 kMaxNodes = 128u;

    // StepOnce で使う固定 dt (= 60 fps の 1 frame 分)。
    static constexpr f32 kStepDt   = 0.016f;

private:
    // ----- 内部メタ node ----------------------------------------------------
    // 1 個の BT node の表示用情報 + last_status を持つ value 型 struct。
    // 配列内 index == id == NodeMeta::id (= 三位一体で常に等しい)。
    struct NodeMeta {
        u32         id          = kInvalidId;  // == 配列内 index
        u32         parent_id   = kInvalidId;  // 親 id、root は kInvalidId
        EBtKind     kind        = EBtKind::Action;
        const char* name        = nullptr;     // リテラル / 永続領域 (非所有)
        EBtStatus   last_status = EBtStatus::Failure; // 初期値は Failure
    };

    // ----- 内部 helper -----------------------------------------------------
    // 引数 dt で 1 tick 進める実装本体 (StepOnce / autorun から共通利用)。
    // callback 登録時は callback だけ呼ぶ、未登録時は `tree->Tick(nullptr, dt)`。
    // history ring に root status を push、step_count++。
    void TickInternal(f32 dt) noexcept;

    // tree view 再帰描画 (depth は表示インデント / 暴走防止用)。
    // node_id の子 node を線形走査して再帰展開する。
    void DrawTreeRecursive(u32 node_id, u32 depth) noexcept;

    // status (Success/Failure/Running) → ImGui 文字色 (4f ベース) を返すヘルパ。
    // 戻り値は ImVec4 を float[4] で受け渡しできる形にする (header に ImGui 型を
    // 持ち込まないため)。caller 側は r/g/b/a を直接 ImVec4 にキャストする。
    static void StatusColor(EBtStatus s, f32 out_rgba[4]) noexcept;

    // ----- 内部状態 ---------------------------------------------------------

    // 観察中の FBehaviorTree (非所有)。
    FBehaviorTree* _tree         = nullptr;

    // autorun (毎フレーム OnFrameBegin で TickInternal を呼ぶ)。
    bool          _autorun      = false;

    // 累積 step 数 (Reset で 0、StepOnce / autorun tick で +1)。
    u32           _step_count   = 0;

    // 選択中 node id (kInvalidId = 未選択)。
    u32           _selected     = kInvalidId;

    // メタミラー本体。index == id。
    TArray<NodeMeta> _nodes;

    // root status の履歴 ring buffer (要素は EBtStatus の u8 生値)。
    // 容量は Init で kHistorySize 個を Resize、以降 Resize しない。
    TArray<u8>     _history;
    // 次に書き込む位置 (circular)。0..kHistorySize-1。
    u32           _history_head = 0;

    // tick callback (= 独自 blackboard を渡したい場合の hook)。
    StepCallback  _step_cb      = nullptr;
    void*         _step_user    = nullptr;
};

} // namespace acs::game::btedit
