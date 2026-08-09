// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — btedit / ABehaviorTreeEditorPanel
//
// `gameframework/BehaviorTree.h` (Pillar L) の BT を **可視化 + ライブデバッグ**
// するための ImGui パネル。実行状態の可視化と 1 tick ずつのデバッグに絞り、
// ノードのグラフ編集 (drag drop で composite に子追加 / 配置入替) は範囲外。
//
// 役割分担:
//   ・本パネルは「**実行中の BT を観察する**」のが第一責務。CBehaviorTree 本体は
//     panel が直接 walk しない (= ABtSelector / ABtSequence の `m_Children` は private、
//     ACS は RTTI 無効で `dynamic_cast` も使えない、ABtAction の `m_Fn` も private、
//     という三重の事情で実体ツリーを panel から覗けない)。
//     代わりに「**メタデータミラー**」: ユーザ (sample / ゲーム側) が AddNode で
//     「親 id・kind・表示名」を panel に push し、panel はそのミラーを描画する。
//     実体 BT とメタミラーの構造が乖離しない責務はユーザ側にあるが、最も
//     一般的な「BT 構築直後にミラーも組み立てる」運用なら手書きでも整合は楽。
//   ・ノードごとの `last_status` は SetNodeStatus で push してもらう。`ABtAction`
//     の関数ポインタが Tick されるたびに `panel.SetNodeStatus(my_id, ret)` を
//     呼ぶ規約。composite (Selector / Sequence) の status は root から伝搬する
//     必要があるが、panel 側は気にしない (= ユーザ自由)。
//   ・StepOnce は `tree->Tick(nullptr, 0.016f)` を 1 回回す + step_count++ +
//     history ring へ root status を push する最小実装。blackboard は nullptr 固定。
//     blackboard を渡したい場合は、ユーザが SetOnStepCallback で independent な
//     tick 関数を登録し、そちらが `tree->Tick(my_bb, dt)` を呼ぶ。callback 登録時は
//     panel は `tree->Tick` を直接呼ばず、callback だけを呼ぶ (= 排他)。
//
// 設計選択:
//   ・**AEditorPanel 継承**: editor_core 基底に乗せる。
//     CEditorWorkspace に登録するだけで自動 dispatch される。Title は
//     "Behavior Tree Editor"。
//   ・**メタミラー方式 (前述)**: BehaviorTree.h の API を改造しないために採用。
//     panel 内に `TArray<FNodeMeta>` を持ち、AddNode で順次積む。FNodeId は 0 から
//     panel が払い出す (= 1 panel 内で unique、複数 BT を同 panel で扱う想定なし)。
//     parent_id は同 TArray 内の id (== index、payload と一致)。kInvalidId
//     (= 0xFFFFFFFFu) を root の parent_id として使う。
//   ・**EBtKind (Selector / Sequence / Action) を u8 enum で持つ**: 表示時の色分け
//     と TreeNode タイプ判別に使う。BehaviorTree.h の EBtStatus と同じく u8 enum
//     で揃え、ACS の "E-prefix enum" 規約に従う。
//   ・**status 表示色は固定リテラル**: Success=緑 (0,1,0)、Failure=赤 (1,0,0)、
//     Running=黄 (1,1,0)。ImGui::PushStyleColor で TreeNode テキストに反映する。
//   ・**history ring buffer は固定長 60**: 60 frame ≈ 1 秒 @ 60 fps の窓。
//     `TArray<u8>` で各要素は EBtStatus の生値 (0/1/2)。`m_HistoryHead` が次に
//     書き込む位置 (circular)。Reset でクリア。ImGui::PlotLines に float buffer を
//     一度展開して渡す。
//   ・**SelectedNodeId は u32 (-1 = none)**: AParticleEditorPanel の `m_Selected:i32`
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
//   ・**ImGui ヘッダは .cpp に閉じる**: AParticleEditorPanel / AInspectorPanel /
//     AModelInspectorPanel と同形。
//
// ImGui レイアウト (DrawUI):
//   ┌────────────── "Behavior Tree Editor" window ─────────────────┐
//   │ [Reset] [Step] [Continuous: ON/OFF] | Active: N | Step: M    │
//   │ ┌─ Tick history (60 frame ring graph) ─────────────────────┐ │
//   │ │  PlotLines: Success=1.0 / Running=0.5 / Failure=0.0       │ │
//   │ └───────────────────────────────────────────────────────────┘ │
//   │ ┌── left: Tree view ─────┐  ┌── right: Node Inspector ────┐ │
//   │ │  Selector ▶            │  │ Name : <selected name>      │ │
//   │ │   Sequence ▶            │  │ Kind : Selector/Sequence/    │ │
//   │ │    Action "Pickup" ●   │  │        Action               │ │
//   │ │    Action "Move"   ●   │  │ Id   : <u32>                │ │
//   │ │   Sequence ▶            │  │ Parent: <u32 or "(root)">   │ │
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
#include "gameframework/tools/btedit/BtActionRegistry.h"
#include "gameframework/tools/btedit/BtCatalog.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game::btedit {

/**
 * メタミラー上の BT node 種別 (実体 BT の ABtNode 派生に対応)。
 *
 * @details
 * 実体 BT 側で RTTI 抜きに種別を判定できないため、メタミラー側で明示的に保持する。
 * ABtSelector → Selector、ABtSequence → Sequence、ABtAction → Action に 1:1 対応する想定。
 */
enum class EBtKind : u8 {
    /** Selector composite (子を順に試し、最初に成功した子で成功)。 */
    Selector  = 0,

    /** Sequence composite (子を順に実行し、最初に失敗した子で失敗)。 */
    Sequence  = 1,

    /** Action leaf (関数を実行する末端ノード、子を持たない。条件/即時判定向き)。 */
    Action    = 2,

    /** Decorator (子を 1 つ持ち、その結果を deco op で変換する単子ノード)。 */
    Decorator = 3,

    /** Task leaf (関数を実行する末端ノード。Action と同じくレジストリ解決、"仕事"系)。 */
    Task      = 4,
};

/**
 * EBtKind が末端 leaf (Action / Task = 子を持たない実行ノード) かを返す。
 *
 * @details
 * leaf はグラフ上で出力ポートを持たず、TickGraph ではレジストリ解決した関数を呼ぶ。
 * Selector / Sequence (composite) と Decorator (単子) は非 leaf で子を持てる。
 * @param k 判定する種別。
 * @return Action または Task なら true。
 */
inline bool BtKindIsLeaf(EBtKind k) noexcept {
    return k == EBtKind::Action || k == EBtKind::Task;
}

/**
 * Decorator ノードの動作モード (kind==Decorator のときのみ意味を持つ)。
 *
 * @details
 * 1 つの Decorator 種別の中で「結果変換」と「条件ガード」を切り替えるための区別。
 * Transform は子の結果を deco op で変換、Condition/Compare は条件を評価し、true の
 * ときだけ子を実行してその結果を返す (false なら子をスキップして Failure) ガード。
 */
enum class EBtDecoMode : u8 {
    /** 子の結果を EBtDecoratorOp で変換する (Inverter / ForceSuccess / …)。 */
    Transform = 0,

    /** Condition レジストリの bool 関数 (ノード名で解決) で子をガードする。 */
    Condition = 1,

    /** ブラックボード変数と定数の比較 (var <op> const) で子をガードする。 */
    Compare   = 2,
};

/** behavior graph text 永続化で使う安定した失敗 category。 */
enum class EBtGraphPersistenceError : u8 {
    None = 0,
    NullArgument,
    EmptyPath,
    PathTooLong,
    EmptyInput,
    InputTooLarge,
    EmbeddedNul,
    TooManyLines,
    LineTooLong,
    InvalidMagic,
    UnsupportedVersion,
    InvalidCount,
    NodeCountLimit,
    InvalidNodeRecord,
    DuplicateNodeId,
    InvalidNodeId,
    InvalidParentReference,
    InvalidStructure,
    CycleDetected,
    DepthLimitExceeded,
    InvalidKind,
    InvalidDecorator,
    InvalidDecoratorMode,
    InvalidCompareOp,
    InvalidNumber,
    NonFiniteNumber,
    NameTooLong,
    InvalidName,
    BlackboardCountLimit,
    InvalidBlackboardRecord,
    DuplicateBlackboardName,
    IntegerOutOfRange,
    AllocationFailure,
    FileOpenFailed,
    FileSizeFailed,
    FileChanged,
    FileReadFailed,
    FileWriteFailed,
    FileFlushFailed,
    FileCloseFailed,
    TemporaryFileExhausted,
    AtomicReplaceFailed,
};

/** 検証付き behavior graph 永続化 API が返す結果。 */
struct FBtGraphPersistenceResult {
    EBtGraphPersistenceError error = EBtGraphPersistenceError::None;
    u32 line = 0u;
    u64 bytes = 0u;
    u32 os_error = 0u;

    bool Succeeded() const noexcept {
        return error == EBtGraphPersistenceError::None;
    }

    explicit operator bool() const noexcept { return Succeeded(); }

    static const char* ErrorName(EBtGraphPersistenceError value) noexcept;
};

/**
 * CBehaviorTree の実行状態を可視化し、1 tick ずつデバッグする ImGui パネル。
 *
 * @details
 * 実行中の BT を観察するのが第一責務。実体ツリーは private メンバ + RTTI 無効で
 * panel から覗けないため、ユーザが AddNode で「親 id・kind・表示名」を push する
 * メタデータミラー方式を採る。各 node の last_status は SetNodeStatus で push してもらい、
 * StepOnce / autorun で BT を 1 tick 進めて root status を history ring に積む。
 * AEditorPanel 継承で CEditorWorkspace に登録すれば自動 dispatch される。
 * 非コピー / 非ムーブ / 全 noexcept / STL 不使用で、name は非所有の永続文字列を想定する。
 */
class ABehaviorTreeEditorPanel : public editor_core::AEditorPanel {
public:
    /**
     * panel が 1 tick 進めたい時に呼ばれる関数ポインタ型。
     *
     * @details
     * 登録されていれば panel は tree->Tick(nullptr, dt) を直接呼ばず、この callback
     * だけを呼ぶ (= ユーザに blackboard を渡す自由を与える)。callback 側で
     * tree->Tick(my_bb, dt) を呼ぶ規約。
     * @param user SetOnStepCallback の第二引数で渡したポインタがそのまま戻る。
     * @param tree 観察中の CBehaviorTree。
     * @param dt この tick で進める秒数。
     */
    using StepCallback = void(*)(void* user, CBehaviorTree* tree, f32 dt) noexcept;

    /** 履歴 ring buffer の長さ (frame 数)。仕様で 60 frame 固定。 */
    static constexpr u32 kHistorySize = 60u;

    /** SelectedNodeId / parent_id の "none" シグナル (u32 max)。 */
    static constexpr u32 kInvalidId   = 0xFFFFFFFFu;

    /** 空状態で構築する (メタミラー・履歴は Init で確保)。 */
    ABehaviorTreeEditorPanel() noexcept = default;

    /** 破棄する (TArray が内部リソースを解放)。 */
    ~ABehaviorTreeEditorPanel() noexcept override = default;

    /** コピー禁止 (内部 TArray の所有を曖昧にしないため)。 */
    ABehaviorTreeEditorPanel(const ABehaviorTreeEditorPanel&)            = delete;

    /** コピー代入も禁止。 */
    ABehaviorTreeEditorPanel& operator=(const ABehaviorTreeEditorPanel&) = delete;

    /** ムーブ禁止 (内部 TArray の所有を曖昧にしないため)。 */
    ABehaviorTreeEditorPanel(ABehaviorTreeEditorPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    ABehaviorTreeEditorPanel& operator=(ABehaviorTreeEditorPanel&&)      = delete;

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
     * @param tree 観察する CBehaviorTree (nullptr で解除)。
     */
    void SetTree(CBehaviorTree* tree) noexcept;

    /**
     * 現在観察中の BT を返す。
     *
     * @return 観察中の CBehaviorTree (未設定なら nullptr)。
     */
    CBehaviorTree* CurrentTree() const noexcept { return m_Tree; }

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
     * @details メタミラー (FNodeMeta 配列) と autorun フラグ・selection は触らない。
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
     * @details ABtAction の Fn から呼ぶことを想定。範囲外は no-op。Reset で全 node が Failure に戻る。
     * @param node_id 更新対象の node id。
     * @param status 新しい last_status。
     */
    void SetNodeStatus(u32 node_id, EBtStatus status) noexcept;

    /**
     * Decorator node の変換 op を設定する (kind!=Decorator の node は no-op)。
     *
     * @details AddNode(EBtKind::Decorator, ...) で作った node に op を後付けするための API。
     * @param node_id 対象 node の id。
     * @param op 設定する変換 op (Inverter / ForceSuccess / ForceFailure / Repeat)。
     * @return 設定できたら true (範囲外 / Decorator 以外は false)。
     */
    bool SetNodeDecoratorOp(u32 node_id, EBtDecoratorOp op) noexcept;

    /**
     * Decorator node の動作モードを設定する (kind!=Decorator は no-op)。
     *
     * @param node_id 対象 node の id。
     * @param mode Transform / Condition / Compare。
     * @return 設定できたら true (範囲外 / Decorator 以外は false)。
     */
    bool SetNodeDecoratorMode(u32 node_id, EBtDecoMode mode) noexcept;

    /**
     * Decorator node を Compare モードに設定し、比較条件 (var <op> const) を与える。
     *
     * @details mode を Compare に切り替え、var/op/rhs を設定する。kind!=Decorator は no-op。
     * @param node_id 対象 node の id。
     * @param var 比較する blackboard 変数名 (schema のキー)。
     * @param op 比較演算子。
     * @param rhs 比較定数 (右辺)。
     * @return 設定できたら true (範囲外 / Decorator 以外は false)。
     */
    bool SetNodeCompare(u32 node_id, const char* var, EBtCompareOp op, f32 rhs) noexcept;

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
    u32 NodeCount() const noexcept { return static_cast<u32>(m_Nodes.Num()); }

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

    // ===== no-code 実行 (グラフを直接インタプリトする) =====

    /**
     * Action 名を関数へ解決するレジストリを設定する (no-code 実行を有効化)。
     *
     * @details 非 null をセットすると Step / Continuous はハンドビルドの CBehaviorTree では
     *          なく「メタミラーのグラフを直接インタプリト」して実行する。Action ノードの
     *          表示名をキーに registry から関数を引いて呼ぶ。null で従来動作に戻る。
     * @param reg アクションレジストリ (非所有、null で解除)。
     */
    void SetActionRegistry(const CBtActionRegistry* reg) noexcept { m_Registry = reg; }

    /**
     * Condition デコレーター用の bool 関数レジストリを設定する (no-code 条件を有効化)。
     *
     * @details Condition モードの Decorator が、ノード名をキーに bool 関数を引いて子をガードする。
     * @param reg 条件レジストリ (非所有、null で解除)。
     */
    void SetConditionRegistry(const CBtConditionRegistry* reg) noexcept { m_CondReg = reg; }

    /**
     * 比較条件用の blackboard スキーマを設定する (変数リンクを有効化)。
     *
     * @details Compare モードの Decorator が、変数名→(型・オフセット) を schema で解決して
     *          BtCompareVar で値を読み、定数と比較する。エディタは変数候補の提示にも使う。
     * @param schema blackboard スキーマ (非所有、null で解除)。
     */
    void SetBlackboardSchema(const FBtBlackboardSchema* schema) noexcept { m_Schema = schema; }

    /**
     * エディタ所有の動的ブラックボードを設定する (変数のエディタ編集を有効化)。
     *
     * @details
     * 非 null をセットすると、エディタの Blackboard パネルで変数の追加 / リネーム /
     * 削除 / 型変更 / 値編集ができ、Compare デコレーターは変数名をここから引いて評価する
     * (offset スキーマより優先)。通常はこの動的 BB を SetGraphBlackboard にも渡し、
     * Action/Condition 関数が同じインスタンスを名前アクセスする。
     * @param bb 動的ブラックボード (非所有、null で解除)。
     */
    void SetDynamicBlackboard(FBtBlackboard* bb) noexcept { m_DynBb = bb; }

    /** 設定済みの Condition レジストリを返す (UI 用、未設定は nullptr)。 */
    const CBtConditionRegistry* ConditionRegistry() const noexcept { return m_CondReg; }

    /** 設定済みの blackboard スキーマを返す (UI 用、未設定は nullptr)。 */
    const FBtBlackboardSchema* BlackboardSchema() const noexcept { return m_Schema; }

    /** 設定済みの動的ブラックボードを返す (UI 用、未設定は nullptr)。 */
    FBtBlackboard* DynamicBlackboard() const noexcept { return m_DynBb; }

    /**
     * グラフインタプリト時に Action 関数へ渡す blackboard を設定する。
     *
     * @param bb ゲーム状態へのポインタ (非所有)。
     */
    void SetGraphBlackboard(void* bb) noexcept { m_GraphBb = bb; }

    /** レジストリ設定済みで「グラフ直接実行」モードかを返す。 */
    bool IsGraphRunnable() const noexcept { return m_Registry != nullptr; }

    /**
     * メタミラーのグラフを 1 tick 直接インタプリトする (selector/sequence/action 意味論)。
     *
     * @details 各ノードの last_status と visit_order を更新し、root status を履歴へ push する。
     *          Action は registry から名前で引いた関数を blackboard 付きで呼ぶ。ゲーム側の
     *          毎フレームループから直接呼べば、エディタにライブ実行フローが流れる。
     * @param dt この tick の経過秒。
     * @return root の status (root 不在/未解決は Failure)。
     */
    EBtStatus TickGraph(f32 dt) noexcept;

    /**
     * 現在のグラフをテキストファイルへ保存する。
     *
     * @param path 保存先パス。
     * @return 成功なら true。
     */
    bool SaveGraph(const char* path) const noexcept;

    /**
     * canonical な ACSBT v4 text 形式を検証し、atomic に保存する。
     * temporary file が完成した後にだけ出力先を置き換える。
     */
    FBtGraphPersistenceResult TrySaveGraph(const char* path) const noexcept;

    /**
     * テキストファイルからグラフを読み込む (既存ノードはクリアされる)。
     *
     * @param path 読み込み元パス。
     * @return 成功なら true。
     */
    bool LoadGraph(const char* path) noexcept;

    /**
     * 上限付き file snapshot を読み、完全な検証後にだけ commit する。
     * 従来の ACSBT version 1 から 3 も引き続き読み込める。
     */
    FBtGraphPersistenceResult TryLoadGraph(const char* path) noexcept;

    /**
     * 長さ指定された ACSBT document を transactional に parse する。
     * 失敗時は graph、selection、layout state、dynamic blackboard を変更しない。
     */
    FBtGraphPersistenceResult TryParseGraphText(
        const char* text, usize text_size) noexcept;

    /**
     * 現在のグラフを実行可能な CBehaviorTree ノードツリーへ bake する。
     *
     * @details
     * メタミラーを walk し、Selector/Sequence/Action(Task)/Decorator(Transform) は core
     * ランタイムノードへ、Condition/Compare デコレーターは btedit の ABtConditionNode /
     * ABtCompareNode へ変換した 1 本のツリーを構築して返す。Action/Condition 名は
     * 設定済みレジストリ (SetActionRegistry / SetConditionRegistry) で解決し、Compare の
     * 変数は実行時に FBtBlackboard 名前アクセスで解決する。返り値を CBehaviorTree::SetRoot
     * に渡せば、エディタ外 (通常のゲームループ) で `bt.Tick(&blackboard, dt)` として走らせられる。
     * @return root ノード (root 不在なら空の TUniquePtr)。
     */
    TUniquePtr<ABtNode> BuildRuntimeTree() const noexcept;

    // ===== undo / redo (ユーザ操作。ホストがメニュー/ツールバーに束縛してもよい) =====

    /** 1 手戻す (undo 履歴が空なら no-op)。 */
    void Undo() noexcept;

    /** 1 手やり直す (redo 履歴が空なら no-op)。 */
    void Redo() noexcept;

    /** undo 可能か。 */
    bool CanUndo() const noexcept { return !m_UndoStack.IsEmpty(); }

    /** redo 可能か。 */
    bool CanRedo() const noexcept { return !m_RedoStack.IsEmpty(); }

    /**
     * 現在のグラフ状態を undo 履歴へ明示的にコミットする (チェックポイント)。
     *
     * @details
     * 通常は DrawUI 内の自動追跡 (編集確定時) でコミットされるが、ホストがバッチ操作の
     * 前に明示チェックポイントを打ちたい場合や、ImGui frame を回さずにプログラム的に
     * 編集して undo 単位を区切りたい場合に使う。初回は baseline 初期化のみ。
     */
    void PushUndoCheckpoint() noexcept;

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

    /** 末尾 newline を含む、受け入れ可能な ACSBT document の最大サイズ。 */
    static constexpr usize kMaxGraphTextBytes = 256u * 1024u;

    /** CR/LF を除く、ACSBT source 1 行の最大バイト数。 */
    static constexpr usize kMaxGraphLineBytes = 255u;

    /** ACSBT document 1 つに含められる物理行の最大数。 */
    static constexpr u32 kMaxGraphLines = 256u;

    /** 終端 NUL より前の path 最大バイト数。 */
    static constexpr usize kMaxGraphPathBytes = 1023u;

    /** 受け入れ可能な parent chain の最大深さ。 */
    static constexpr u32 kMaxGraphDepth = 64u;

    /** StepOnce で使う固定 dt (= 60 fps の 1 frame 分)。 */
    static constexpr f32 kStepDt   = 0.016f;

private:
    /**
     * 1 個の BT node の表示用情報 + last_status を持つ value 型 struct。
     *
     * @details 配列内 index == id == FNodeMeta::id (= 三位一体で常に等しい)。
     */
    struct FNodeMeta {
        /** この node の id (== 配列内 index)。 */
        u32         id          = kInvalidId;

        /** 親 node の id (root は kInvalidId)。 */
        u32         parent_id   = kInvalidId;

        /** node 種別 (Selector / Sequence / Action / Decorator / Task)。 */
        EBtKind     kind        = EBtKind::Action;

        /** kind==Decorator のときの変換 op (decoMode==Transform でのみ使用)。 */
        EBtDecoratorOp deco     = EBtDecoratorOp::Inverter;

        /** kind==Decorator の動作モード (Transform / Condition / Compare)。 */
        EBtDecoMode    decoMode = EBtDecoMode::Transform;

        /** decoMode==Compare の比較対象 blackboard 変数名 (schema のキー)。 */
        char           var[48]  = { 0 };

        /** decoMode==Compare の比較演算子。 */
        EBtCompareOp   cmpOp    = EBtCompareOp::Less;

        /** decoMode==Compare の比較定数 (右辺)。 */
        f32            cmpRhs   = 0.0f;

        /** ImGui 表示名 (リテラル / 永続領域、非所有)。 */
        const char* name        = nullptr;

        /** 直近の tick での status (初期値 Failure)。 */
        EBtStatus   last_status = EBtStatus::Failure;

        /** エディタで付けた名前 (空なら name を使う)。エディタ作成/リネーム用。 */
        char        ename[48]   = { 0 };

        /** グラフ canvas 上の位置 (world 座標。auto-layout / ドラッグで決まる)。 */
        f32         x           = 0.0f;
        f32         y           = 0.0f;

        /** false = 削除済み (tombstone)。id==index を保つため配列からは消さない。 */
        bool        alive       = true;

        /** 直近 tick でこのノードが訪問された順番 (0=未訪問。実行フロー可視化用)。 */
        u32         visit_order = 0u;
    };

    /**
     * undo/redo 用のグラフ状態スナップショット (ノード配列 + 動的 BB のコピー)。
     *
     * @details TArray はコピー不可なので move-only。要素 (FNodeMeta) は値コピーで詰める。
     */
    struct FGraphSnapshot {
        /** ノード配列のコピー。 */
        TArray<FNodeMeta> nodes;

        /** 動的ブラックボードのコピー (hasBb のときのみ有効)。 */
        FBtBlackboard    bb;

        /** bb が有効か (m_DynBb 設定時に true)。 */
        bool             hasBb = false;
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

    /** ノードグラフ canvas を描画 + 操作 (pan/zoom/ドラッグ/追加/接続/削除)。 */
    void DrawGraph() noexcept;

    /**
     * 「ノード追加」メニュー項目群を描画する (popup / submenu 内から共通利用)。
     *
     * @details
     * Selector / Sequence / Decorator(op) / Condition(catalog) / Compare / Task(catalog) /
     * Task / Action を列挙し、選択で AddNodeGraph する。catalog 系は登録済み name を一覧する。
     * @param parent_id 追加先の親 (kInvalidId で root)。
     * @param wx,wy 配置 world 座標。
     */
    void DrawAddMenu(u32 parent_id, f32 wx, f32 wy) noexcept;

    /** tree 構造から各 node の x,y を自動配置する (tidy layout)。 */
    void AutoLayout() noexcept;

    /** node の表示名を返す (ename が空でなければそれ、無ければ name)。 */
    const char* DisplayName(const FNodeMeta& n) const noexcept;

    /**
     * エディタからノードを追加する (グラフ上に配置)。
     *
     * @param kind 種別。
     * @param parent_id 親 (kInvalidId で root)。
     * @param wx,wy 配置 world 座標。
     * @return 払い出した id (上限到達は kInvalidId)。
     */
    u32  AddNodeGraph(EBtKind kind, u32 parent_id, f32 wx, f32 wy) noexcept;

    /** ノードを tombstone 削除し、子を祖父へ付け替える。 */
    void DeleteNodeGraph(u32 id) noexcept;

    /** maybe_ancestor が node の祖先 (自分含む) かを返す (循環接続ガード用)。 */
    bool IsAncestor(u32 maybe_ancestor, u32 node) const noexcept;

    /**
     * グラフ 1 ノードを再帰インタプリトする (TickGraph の本体)。
     *
     * @param id ノード id。
     * @param dt 経過秒。
     * @param guard 再帰深度ガード。
     * @return このノードの status。
     */
    EBtStatus TickGraphNode(u32 id, f32 dt, u32 guard) noexcept;

    /**
     * id の生存子を x 座標 (= 左→右の見た目順) に並べて out へ集める。
     *
     * @param id 親ノード id。
     * @param out 子 id の出力先 (最大 cap 個)。
     * @param cap out の容量。
     * @return 集めた子の数。
     */
    u32 CollectChildrenSorted(u32 id, u32* out, u32 cap) const noexcept;

    /**
     * メタミラー 1 ノードを実行可能な ABtNode へ再帰変換する (BuildRuntimeTree の本体)。
     *
     * @param id 変換するノード id。
     * @param guard 再帰深度ガード。
     * @return 構築した ABtNode (不正/未解決は空の TUniquePtr)。
     */
    TUniquePtr<ABtNode> BuildRuntimeNode(u32 id, u32 guard) const noexcept;

    /** 現在のグラフ状態 (ノード + 動的 BB) を out へコピーする (undo 用)。 */
    void CaptureSnapshot(FGraphSnapshot& out) const noexcept;

    /** スナップショットから現在のグラフ状態を復元する (selection 等はリセット)。 */
    void RestoreSnapshot(const FGraphSnapshot& s) noexcept;

    /** 現在のグラフ状態の変更検出用シグネチャ (ノード + 動的 BB を畳み込む)。 */
    u64 GraphSignature() const noexcept;

    /** 毎フレーム呼び、編集が「確定」したら baseline を undo stack へ積む。 */
    void UpdateUndoTracking() noexcept;

    /** 現在の baseline を undo stack へ積み、baseline を現在状態へ更新する (redo はクリア)。 */
    void CommitBaseline() noexcept;

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

    /** 観察中の CBehaviorTree (非所有)。 */
    CBehaviorTree* m_Tree         = nullptr;

    /** autorun フラグ (毎フレーム OnFrameBegin で TickInternal を呼ぶ)。 */
    bool          m_Autorun      = false;

    /** 累積 step 数 (Reset で 0、StepOnce / autorun tick で +1)。 */
    u32           m_StepCount   = 0;

    /** 選択中 node id (kInvalidId = 未選択)。 */
    u32           m_Selected     = kInvalidId;

    /** メタミラー本体 (index == id)。 */
    TArray<FNodeMeta> m_Nodes;

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

    // ===== ノードグラフ表示の状態 =====
    /** グラフ表示モード (false = 従来のツリーリスト表示)。 */
    bool m_GraphMode  = true;

    /** auto-layout 済みフラグ (node 構成が変わると false に戻して再配置)。 */
    bool m_DidLayout  = false;

    /** canvas のパン (スクリーン px) とズーム。 */
    f32  m_PanX       = 40.0f;
    f32  m_PanY       = 40.0f;
    f32  m_Zoom       = 1.0f;

    /** ドラッグ中ノード (kInvalidId = なし)。 */
    u32  m_DragNode   = kInvalidId;

    /** 接続ドラッグの元ノード (出力ポートから引く、kInvalidId = なし)。 */
    u32  m_LinkSrc    = kInvalidId;

    /** 右クリックメニューの対象ノード。 */
    u32  m_CtxNode    = kInvalidId;

    /** 空所での左ドラッグによるパン中フラグ。 */
    bool m_PanningBg  = false;

    /** Add ポップアップで配置する world 座標。 */
    f32  m_AddX       = 0.0f;
    f32  m_AddY       = 0.0f;

    // ===== no-code 実行 / 実行フロー可視化 =====
    /** Action 名→関数のレジストリ (非所有、非 null でグラフ直接実行モード)。 */
    const CBtActionRegistry* m_Registry = nullptr;

    /** Condition 名→bool 関数のレジストリ (非所有、Condition デコレーター用)。 */
    const CBtConditionRegistry* m_CondReg = nullptr;

    /** blackboard 変数スキーマ (非所有、offset 参照型。Compare / 変数候補提示用)。 */
    const FBtBlackboardSchema* m_Schema = nullptr;

    /** エディタ所有の動的ブラックボード (非所有、Compare 優先解決 + 変数のエディタ編集用)。 */
    FBtBlackboard* m_DynBb = nullptr;

    /** グラフ実行時に Action へ渡す blackboard (非所有)。 */
    void* m_GraphBb   = nullptr;

    /** 直近 tick の訪問順カウンタ (visit_order 採番用)。 */
    u32   m_VisitSeq  = 0u;

    // ===== undo / redo =====
    /** undo スタック (古い→新しい順。top が直前の確定状態)。 */
    TArray<FGraphSnapshot> m_UndoStack;

    /** redo スタック (undo で押し戻された状態)。 */
    TArray<FGraphSnapshot> m_RedoStack;

    /** 現在の確定状態のスナップショット (変更検出の基準)。 */
    FGraphSnapshot m_UndoBaseline;

    /** m_UndoBaseline のシグネチャ (現在状態と比較して変更を検出)。 */
    u64  m_BaselineSig = 0u;

    /** baseline 初期化済みか (最初の DrawUI で 1 度キャプチャ)。 */
    bool m_UndoInit    = false;
};

using FBehaviorTreeEditorPanel = ABehaviorTreeEditorPanel;

} // namespace acs::game::btedit
