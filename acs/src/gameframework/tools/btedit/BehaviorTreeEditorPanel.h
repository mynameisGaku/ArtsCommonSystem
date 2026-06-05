// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — btedit / FBehaviorTreeEditorPanel
//
// `gameframework/FBehaviorTree.h` (Pillar L) の BT を **可視化 + ライブデバッグ**
// するための ImGui パネル。Unity の Behavior Designer / Unreal の Behavior Tree
// Editor の `Debugger` モードに相当する役割を担う。スコープは "visualize
// + step debug" に絞り、ノードのグラフ編集 (drag drop で
// composite に子追加 / 配置入替) は範囲外。
//
// 役割分担:
//   ・本パネルは「**実行中の BT を観察する**」のが第一責務。FBehaviorTree 本体は
//     panel が直接 walk しない (= FBtSelector / FBtSequence の `m_Children` は private、
//     ACS は RTTI 無効で `dynamic_cast` も使えない、FBtAction の `m_Fn` も private、
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
// 設計選択:
//   ・**FEditorPanel 継承**: editor_core 基底に乗せる。
//     FEditorWorkspace に登録するだけで自動 dispatch される。Title は
//     "Behavior Tree Editor"。
//   ・**メタミラー方式 (前述)**: FBehaviorTree.h の API を改造しないために採用。
//     panel 内に `TArray<NodeMeta>` を持ち、AddNode で順次積む。FNodeId は 0 から
//     panel が払い出す (= 1 panel 内で unique、複数 BT を同 panel で扱う想定なし)。
//     parent_id は同 TArray 内の id (== index、payload と一致)。kInvalidId
//     (= 0xFFFFFFFFu) を root の parent_id として使う。
//   ・**EBtKind (Selector / FSequence / Action) を u8 enum で持つ**: 表示時の色分け
//     と TreeNode タイプ判別に使う。FBehaviorTree.h の EBtStatus と同じく u8 enum
//     で揃え、ACS の "E-prefix enum" 規約に従う。
//   ・**status 表示色は固定リテラル**: Success=緑 (0,1,0)、Failure=赤 (1,0,0)、
//     Running=黄 (1,1,0)。ImGui::PushStyleColor で TreeNode テキストに反映する。
//   ・**history ring buffer は固定長 60**: 60 frame ≈ 1 秒 @ 60 fps の窓。
//     `TArray<u8>` で各要素は EBtStatus の生値 (0/1/2)。`m_HistoryHead` が次に
//     書き込む位置 (circular)。Reset でクリア。ImGui::PlotLines に float buffer を
//     一度展開して渡す。
//   ・**SelectedNodeId は u32 (-1 = none)**: FParticleEditorPanel の `m_Selected:i32`
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
// 範囲外:
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

/**
 * メタミラー上の BT node 種別 (実体 BT の FBtNode 派生に対応)。
 *
 * @details
 * 実体 BT 側で RTTI 抜きに種別を判定できないため、メタミラー側で明示的に保持する。
 * FBtSelector → Selector、FBtSequence → Sequence、FBtAction → Action に 1:1 対応する想定。
 */
enum class EBtKind : u8 {
    /** Selector composite (子を順に試し、最初に成功した子で成功)。 */
    Selector = 0,

    /** Sequence composite (子を順に実行し、最初に失敗した子で失敗)。 */
    Sequence = 1,

    /** Action leaf (関数を実行する末端ノード、子を持たない)。 */
    Action   = 2,
};

/**
 * FBehaviorTree を可視化 + step debug する ImGui パネル。
 *
 * @details
 * 実行中の BT を観察するのが第一責務。実体ツリーは private メンバ + RTTI 無効で
 * panel から覗けないため、ユーザが AddNode で「親 id・kind・表示名」を push する
 * メタデータミラー方式を採る。各 node の last_status は SetNodeStatus で push してもらい、
 * StepOnce / autorun で BT を 1 tick 進めて root status を history ring に積む。
 * FEditorPanel 継承で FEditorWorkspace に登録すれば自動 dispatch される。
 * 非コピー / 非ムーブ / 全 noexcept / STL 不使用で、name は非所有の永続文字列を想定する。
 */
class FBehaviorTreeEditorPanel : public editor_core::FEditorPanel {
public:
    /**
     * panel が 1 tick 進めたい時に呼ばれる関数ポインタ型。
     *
     * @details
     * 登録されていれば panel は tree->Tick(nullptr, dt) を直接呼ばず、この callback
     * だけを呼ぶ (= ユーザに blackboard を渡す自由を与える)。callback 側で
     * tree->Tick(my_bb, dt) を呼ぶ規約。
     * @param user SetOnStepCallback の第二引数で渡したポインタがそのまま戻る。
     * @param tree 観察中の FBehaviorTree。
     * @param dt この tick で進める秒数。
     */
    using StepCallback = void(*)(void* user, FBehaviorTree* tree, f32 dt) noexcept;

    /** 履歴 ring buffer の長さ (frame 数)。仕様で 60 frame 固定。 */
    static constexpr u32 kHistorySize = 60u;

    /** SelectedNodeId / parent_id の "none" シグナル (u32 max)。 */
    static constexpr u32 kInvalidId   = 0xFFFFFFFFu;

    /** 空状態で構築する (メタミラー・履歴は Init で確保)。 */
    FBehaviorTreeEditorPanel() noexcept = default;

    /** 破棄する (TArray が内部リソースを解放)。 */
    ~FBehaviorTreeEditorPanel() noexcept override = default;

    /** コピー禁止 (内部 TArray の所有を曖昧にしないため)。 */
    FBehaviorTreeEditorPanel(const FBehaviorTreeEditorPanel&)            = delete;

    /** コピー代入も禁止。 */
    FBehaviorTreeEditorPanel& operator=(const FBehaviorTreeEditorPanel&) = delete;

    /** ムーブ禁止 (内部 TArray の所有を曖昧にしないため)。 */
    FBehaviorTreeEditorPanel(FBehaviorTreeEditorPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FBehaviorTreeEditorPanel& operator=(FBehaviorTreeEditorPanel&&)      = delete;

    /**
     * 初期化する。
     *
     * @details
     * メタミラーをクリアし、履歴を kHistorySize 個確保して Failure で埋め、autorun /
     * step counter / selection を default に戻す。多重 Init 可。callback はリセットしない
     * (= 別操作扱い)。
     */
    void Init() noexcept;

    /**
     * 後片付けする。
     *
     * @details メタミラー・履歴・callback を全てクリアする。多重 Shutdown 可。
     */
    void Shutdown() noexcept;

    /**
     * 観察対象の BT を差し替える (nullptr で解除)。
     *
     * @details
     * panel は tree を所有しない (caller 所有)。差し替え時に Reset() を呼んで step counter /
     * history / 全 node status を初期化するが、メタミラーは触らない (= 同じ構造で別
     * インスタンスを観察する用途に対応)。
     * @param tree 観察する FBehaviorTree (nullptr で解除)。
     */
    void SetTree(FBehaviorTree* tree) noexcept;

    /**
     * 現在観察中の BT を返す。
     *
     * @return 観察中の FBehaviorTree (未設定なら nullptr)。
     */
    FBehaviorTree* CurrentTree() const noexcept { return m_Tree; }

    /**
     * autorun (= 毎フレーム自動 Tick) が有効かを返す。
     *
     * @return autorun が ON なら true。
     */
    bool IsAutorun() const noexcept { return m_Autorun; }

    /**
     * autorun を切り替える。
     *
     * @param b true なら OnFrameBegin で毎 frame 1 tick 進める。
     */
    void SetAutorun(bool b) noexcept { m_Autorun = b; }

    /**
     * 1 tick だけ手動で進める。
     *
     * @details
     * dt = kStepDt (0.016f) 固定で TickInternal を呼ぶ。StepCallback が登録されていれば
     * そちらに委譲、無ければ tree->Tick(nullptr, kStepDt) を直接呼ぶ。tree 未設定なら
     * no-op。history ring に root status を 1 件 push、step counter を +1 する。
     */
    void StepOnce() noexcept;

    /**
     * step counter / history / 全 node の last_status を初期状態に戻す。
     *
     * @details メタミラー (NodeMeta 配列) と autorun フラグ・selection は触らない。
     */
    void Reset() noexcept;

    /**
     * 現在の step counter を返す。
     *
     * @return 累積 step 数 (Reset で 0、StepOnce / autorun tick で +1)。
     */
    u32 StepCount() const noexcept { return m_StepCount; }

    /**
     * 現在の選択 node id を返す。
     *
     * @return 選択中の node id (未選択は kInvalidId)。
     */
    u32 SelectedNodeId() const noexcept { return m_Selected; }

    /**
     * 選択 node を設定する。
     *
     * @param node_id 選択する node id (範囲外 / kInvalidId で「未選択」に正規化)。
     */
    void SelectNode(u32 node_id) noexcept;

    /**
     * 新規 node をメタミラーに追加し、払い出した node_id を返す。
     *
     * @details
     * caller が BT 構造を panel に教えるための API。id は現在の末尾 index (0..N-1) を払い出す。
     * parent_id が kInvalidId 以外で範囲外を指す場合は root 扱い (kInvalidId) に倒して安全に追加する。
     * 上限 kMaxNodes に達した場合は追加せず kInvalidId を返す (= 失敗を id で通知)。
     * @param kind Selector / Sequence / Action のいずれか。
     * @param name ImGui 表示名 (リテラル / 永続領域、panel は所有しない)。
     * @param parent_id 親 node の id、root の場合は kInvalidId。
     * @return 払い出した node_id (上限到達時は kInvalidId)。
     */
    u32 AddNode(EBtKind kind, const char* name, u32 parent_id) noexcept;

    /**
     * 既存 node の last_status を更新する。
     *
     * @details FBtAction の Fn から呼ぶことを想定。範囲外は no-op。Reset で全 node が Failure に戻る。
     * @param node_id 更新対象の node id。
     * @param status 新しい last_status。
     */
    void SetNodeStatus(u32 node_id, EBtStatus status) noexcept;

    /**
     * メタミラーを全削除し、selection を解除する。
     *
     * @details BT 構造を組み直す前に呼ぶ。history / step counter / autorun は触らない (Reset を別途呼ぶ)。
     */
    void ClearNodes() noexcept;

    /**
     * 現在登録済みの node 数を返す。
     *
     * @return メタミラー内の node 数。
     */
    u32 NodeCount() const noexcept { return static_cast<u32>(m_Nodes.Size()); }

    /**
     * 指定 id の node の last_status を返す。
     *
     * @param node_id 問い合わせる node id。
     * @return その node の last_status (範囲外は Failure)。
     */
    EBtStatus NodeStatus(u32 node_id) const noexcept;

    /**
     * 1 tick 進める時の独自処理を登録する (blackboard を渡したい場合等)。
     *
     * @details nullptr で解除すると panel が tree->Tick(nullptr, dt) を直接呼ぶ fallback に戻る。
     * @param cb 登録する StepCallback (nullptr で解除)。
     * @param user callback の第一引数として戻すユーザポインタ。
     */
    void SetOnStepCallback(StepCallback cb, void* user) noexcept;

    /**
     * ImGui::Begin に渡す window タイトルを返す。
     *
     * @return "Behavior Tree Editor" (リテラル)。
     */
    const char* Title() const noexcept override { return "Behavior Tree Editor"; }

    /**
     * 毎フレーム呼ばれる non-ImGui hook。
     *
     * @details
     * autorun が ON のときに実 dt で 1 tick 進める (Step は kStepDt 固定だが、autorun は
     * ゲームの実 dt を反映するため step 単位ではなく実時間進行に追従する)。
     * @param dt 前フレームからの経過秒。
     */
    void OnFrameBegin(f32 dt) noexcept override;

    /** メイン ImGui 描画 (toolbar / history graph / tree view / node inspector)。 */
    void DrawUI() noexcept override;

    /**
     * 同時登録可能な node 数の上限 (overflow ガード)。
     *
     * @details 実用 BT で 128 で十分。上限到達時の AddNode は kInvalidId を返す (= silent fail 通知)。
     */
    static constexpr u32 kMaxNodes = 128u;

    /** StepOnce で使う固定 dt (= 60 fps の 1 frame 分)。 */
    static constexpr f32 kStepDt   = 0.016f;

private:
    /**
     * 1 個の BT node の表示用情報 + last_status を持つ value 型 struct。
     *
     * @details 配列内 index == id == NodeMeta::id (= 三位一体で常に等しい)。
     */
    struct NodeMeta {
        /** この node の id (== 配列内 index)。 */
        u32         id          = kInvalidId;

        /** 親 node の id (root は kInvalidId)。 */
        u32         parent_id   = kInvalidId;

        /** node 種別 (Selector / Sequence / Action)。 */
        EBtKind     kind        = EBtKind::Action;

        /** ImGui 表示名 (リテラル / 永続領域、非所有)。 */
        const char* name        = nullptr;

        /** 直近の tick での status (初期値 Failure)。 */
        EBtStatus   last_status = EBtStatus::Failure;
    };

    /**
     * 引数 dt で 1 tick 進める実装本体 (StepOnce / autorun から共通利用)。
     *
     * @details
     * callback 登録時は callback だけ呼び、未登録時は tree->Tick(nullptr, dt) を呼ぶ。
     * 取得した root status を history ring に push し、step counter を +1 する。
     * @param dt この tick で進める秒数。
     */
    void TickInternal(f32 dt) noexcept;

    /**
     * tree view を再帰描画する。
     *
     * @details node_id の子 node を m_Nodes 内で線形走査して再帰展開する。
     * @param node_id 描画する node の id。
     * @param depth 表示インデント兼暴走防止用の再帰深度。
     */
    void DrawTreeRecursive(u32 node_id, u32 depth) noexcept;

    /**
     * status を ImGui 文字色 (RGBA float) に変換するヘルパ。
     *
     * @details
     * header に ImGui 型を持ち込まないため ImVec4 ではなく float[4] で受け渡す。
     * caller 側は r/g/b/a を直接 ImVec4 にキャストする。
     * @param s 変換元の EBtStatus。
     * @param out_rgba 変換結果を書き込む長さ 4 の RGBA 配列。
     */
    static void StatusColor(EBtStatus s, f32 out_rgba[4]) noexcept;

    /** 観察中の FBehaviorTree (非所有)。 */
    FBehaviorTree* m_Tree         = nullptr;

    /** autorun フラグ (毎フレーム OnFrameBegin で TickInternal を呼ぶ)。 */
    bool          m_Autorun      = false;

    /** 累積 step 数 (Reset で 0、StepOnce / autorun tick で +1)。 */
    u32           m_StepCount   = 0;

    /** 選択中 node id (kInvalidId = 未選択)。 */
    u32           m_Selected     = kInvalidId;

    /** メタミラー本体 (index == id)。 */
    TArray<NodeMeta> m_Nodes;

    /**
     * root status の履歴 ring buffer (要素は EBtStatus の u8 生値)。
     *
     * @details 容量は Init で kHistorySize 個を Resize し、以降 Resize しない。
     */
    TArray<u8>     m_History;

    /** 次に書き込む履歴位置 (circular、0..kHistorySize-1)。 */
    u32           m_HistoryHead = 0;

    /** tick callback (= 独自 blackboard を渡したい場合の hook、未登録は nullptr)。 */
    StepCallback  m_StepCb      = nullptr;

    /** StepCallback に渡すユーザポインタ。 */
    void*         m_StepUser    = nullptr;
};

} // namespace acs::game::btedit
