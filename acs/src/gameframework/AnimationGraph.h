// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L — FAnimationGraph (Phase 4)
//
// 3D skeleton-anim 向けの state machine ベース blend graph。
// FSpriteAnimator (= sprite frame index 計算) と FAnimationCurve (= 任意 f32 補間)
// を補完する、「現在再生中の clip + 遷移ブレンド」を担う高次抽象。
//
// 役割分担:
//   ・FSpriteAnimator  : sprite シートの frame index (2D 用)
//   ・FAnimationCurve  : 任意 f32 を時間で滑らかに動かす汎用パス
//   ・FStateMachine<T> : 汎用 FSM (関数ポインタ駆動)
//   ・FAnimationGraph  : 本ファイル — 3D skeleton clip の選択 + 状態間 blend
//
// 役割の境界:
//   ・本クラスは「どの clip を / いつから / どの blend alpha で再生するか」
//     という meta 制御のみ。bone palette 計算 / GPU upload / final pose 算出は
//     呼出側 renderer + FAnimationPlayer (gameframework_anim 等の別レイヤ) に
//     委譲する。本クラスは clip_index と blend_alpha を提供するだけ。
//   ・clip データ (key 列 / bone curve) は外部 (model loader) が所有。本クラスは
//     `FAnimationClipBinding` (name / duration / looping / default speed) という
//     メタ情報配列を値コピーで保持する。
//
// 使い方 (典型):
//   FAnimationGraph g;
//   g.Init();
//   const u32 idle_clip = g.AddClip({ "Idle", 2.0f, true,  1.0f });
//   const u32 walk_clip = g.AddClip({ "Walk", 1.2f, true,  1.0f });
//   const u32 jump_clip = g.AddClip({ "Jump", 0.8f, false, 1.0f });
//
//   g.AddStateNode({ "Idle", EAnimationGraphState::Idle, idle_clip, 0.15f, 0.15f });
//   g.AddStateNode({ "Walk", EAnimationGraphState::Walk, walk_clip, 0.15f, 0.15f });
//   g.AddStateNode({ "Jump", EAnimationGraphState::Jump, jump_clip, 0.10f, 0.20f });
//
//   // Idle → Walk: speed > 0.1 で遷移
//   g.AddTransition({ EAnimationGraphState::Idle, EAnimationGraphState::Walk,
//                     0.1f, "speed", false });
//   // Walk → Idle: speed < 0.05 で遷移 (= threshold を下回ったとき)
//   // (条件は「>= threshold」なので、逆方向は別途設計; ここではゲーム側で
//   //  TriggerTransition を使うか speed の正負を反転するかで対応)
//
//   // 毎フレーム:
//   g.SetParam("speed", current_speed);
//   g.Tick(dt);
//
//   const EAnimationGraphState cur = g.CurrentState();
//   const f32                  alpha = g.CurrentBlendAlpha();
//   const f32                  t_cur = g.CurrentLocalTime();
//   // → renderer に (current_clip, t_cur, alpha, prev_clip, t_prev) を渡して
//   //    bone palette を blend する
//
// 設計判断:
//   ・**enum 駆動の固定状態 ID** (EAnimationGraphState): UE Blueprint / Unity
//     Mecanim の Any State 風自由 ID と違い、ACS は「状態が極端に多くない 3D
//     キャラ」を想定して 9 個のプリセット (Idle/Walk/Run/Jump/Attack/Hit/Death/
//     Custom1/Custom2) に固定。将来必要なら EAnimationGraphState を拡張。
//   ・**clip と state を分離**: 1 clip を複数 state から参照する余地を残す (= 例
//     えば Attack で Walk の clip を流用、Custom1 で Idle clip を流用する等)。
//   ・**遷移条件は (param_name, threshold)**: param 値 >= threshold で発火。
//     より複雑な条件 (== / <= / && / ||) は将来拡張。本 Phase は単純化優先。
//   ・**TriggerTransition** で明示的遷移も可能 (= 攻撃ボタン入力等を直接遷移に
//     使うケース; 条件式で表現するのは煩雑なため)。
//   ・**enter/exit blend duration を state ごとに保持**: 「Idle → Walk」のとき
//     使う duration は **新状態 (Walk) の enter_blend_sec**。これにより遷移先に
//     応じた blend 長を 1 箇所で管理できる (= UE Mecanim の Transition Duration
//     を「state 入口側に持たせた」設計)。
//   ・**blend 中は previous_state / blend_alpha を出力**: alpha は 0→1 で「新状態
//     の比率」。blend 完了で alpha=1.0、previous_state は last_state として
//     残す (= 履歴照会用)。
//   ・**Tick 中の callback**: state enter / clip end の 2 種類を関数ポインタで
//     提供 (std::function 不使用、ACS 規約)。
//   ・**param 比較**: char* literal をポインタ + strcmp 両方で照合 (= literal
//     共有の場合はポインタ一致 fast path, 異なるバッファでも strcmp で機能)。
//   ・**非コピー・非ムーブ**, 全 noexcept, STL 不使用, `<string>` 禁止。
//
// 将来拡張 (Phase 5+):
//   ・1D blend space (Walk + Run を speed で連続 blend)
//   ・2D blend space (Direction X/Y で 8 方向 walk 等)
//   ・additive layer (Hit reaction を base pose に加算)
//   ・IK target hook (foot IK / look-at)
//   ・遷移条件の表現力拡張 (==, <=, 複合)
//   ・root motion 抽出
//
// 参考: FSpriteAnimator (frame anim), FAnimationCurve (curve), FStateMachine<T> (FSM 基盤),
//      FModelAnimationPanel (UI 連動可能)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// ---------------------------------------------------------------------------
// EAnimationGraphState — 標準状態 ID (Phase 4 では 9 固定)
// ---------------------------------------------------------------------------
// 一般的な 3D キャラ制御で頻出する状態を列挙。Custom1/2 はゲーム固有 (掴み /
// 詠唱 / 変身等) を吸収する拡張枠。それでも足りない場合は本 enum を拡張する
// (= ACS は「状態は数十個程度」を上限想定; 数百を超えるなら別設計)。
enum class EAnimationGraphState : u8 {
    Idle    = 0,
    Walk    = 1,
    Run     = 2,
    Jump    = 3,
    Attack  = 4,
    Hit     = 5,
    Death   = 6,
    Custom1 = 7,
    Custom2 = 8,
};

// ---------------------------------------------------------------------------
// FAnimationClipBinding — clip メタ情報 (state からは clip_index で参照)
// ---------------------------------------------------------------------------
// 注: tools/modelview/FModelAnimationPanel.h にある同名構造は
//     `acs::game::modelview` 名前空間で別物。本構造は `acs::game` 直下に置く。
//   clip_name     : デバッグ表示 / 外部 FAnimationPlayer 解決用名 (literal 寿命は
//                   caller 管理)。
//   duration_sec  : clip の長さ (秒)。0 以下は内部で 0 にクランプ (= 進行しない)。
//   is_looping    : true なら local time が duration を超えたら wrap、false なら
//                   末尾に固定して ClipEndCallback を発火。
//   default_speed : Tick で local_time に乗る既定再生速度。1.0 = 等速。
struct FAnimationClipBinding {
    const char* clip_name     = nullptr;
    f32         duration_sec  = 0.0f;
    bool        is_looping    = false;
    f32         default_speed = 1.0f;
};

// ---------------------------------------------------------------------------
// FAnimationStateNode — graph 上の 1 ノード (= 1 つの状態)
// ---------------------------------------------------------------------------
//   name             : デバッグ表示用 (UI / log)。literal 寿命は caller 管理。
//   id               : この state の論理 ID。AddStateNode で重複登録は後勝ち。
//   clip_index       : 再生する FAnimationClipBinding の index (AddClip 戻り値)。
//                      範囲外なら Tick 内で 0 にクランプ (= 安全側)。
//   enter_blend_sec  : この state に **入る** ときに使う blend 長 (秒)。
//                      ≦ 0 なら即時切替 (alpha=1.0 で開始)。
//   exit_blend_sec   : この state から **抜ける** とき呼出側が参考にする値。
//                      Phase 4 では enter 側を優先採用するため、本フィールドは
//                      情報提供のみ (= 将来 cross-fade で両方使う想定)。
struct FAnimationStateNode {
    const char*         name             = nullptr;
    EAnimationGraphState id              = EAnimationGraphState::Idle;
    u32                 clip_index       = 0u;
    f32                 enter_blend_sec  = 0.0f;
    f32                 exit_blend_sec   = 0.0f;
};

// ---------------------------------------------------------------------------
// FAnimationTransition — from → to の遷移ルール
// ---------------------------------------------------------------------------
//   from                       : 元状態 (current_state とこれが一致時のみ評価)
//   to                         : 遷移先
//   condition_param_threshold  : condition_param_name 値が **>= threshold** で発火
//   condition_param_name       : 比較する param 名 (literal 寿命は caller 管理)。
//                                nullptr なら exit_immediately だけ評価する。
//   exit_immediately           : true なら param 条件成立で即発火。
//                                false なら「現 clip が終端まで再生してから」発火
//                                (= looping clip では効果なし、Once clip 用)。
struct FAnimationTransition {
    EAnimationGraphState from                      = EAnimationGraphState::Idle;
    EAnimationGraphState to                        = EAnimationGraphState::Idle;
    f32                  condition_param_threshold = 0.0f;
    const char*          condition_param_name      = nullptr;
    bool                 exit_immediately          = true;
};

// ---------------------------------------------------------------------------
// GraphHandle — 将来の multi-graph 管理用 handle (Phase 4 では未使用)
// ---------------------------------------------------------------------------
// 24bit index + 8bit gen を packed (Cooldown/FSceneTimer と同方針)。
// 本 Phase では FAnimationGraph 単体使用想定だが、複数グラフ (= 上半身 / 下半身
// 別レイヤ) を将来導入する際に再利用するため API として公開しておく。
struct GraphHandle {
    u32 m_Packed = 0u;

    bool IsValid() const noexcept { return m_Packed != 0u; }

    static constexpr u32 kIndexBits = 24u;
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u; // 0x00FFFFFF
    static constexpr u32 kMaxIndex  = kIndexMask;              // 16777215

    static GraphHandle Pack(u32 index, u8 gen) noexcept {
        GraphHandle h;
        h.m_Packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return h;
    }
    u32 Index() const noexcept { return m_Packed & kIndexMask; }
    u8  Gen()   const noexcept { return static_cast<u8>(m_Packed >> kIndexBits); }

    constexpr bool operator==(GraphHandle o) const noexcept { return m_Packed == o.m_Packed; }
    constexpr bool operator!=(GraphHandle o) const noexcept { return m_Packed != o.m_Packed; }
};

// ---------------------------------------------------------------------------
// FAnimationGraph — state machine ベースの blend graph
// ---------------------------------------------------------------------------
class FAnimationGraph {
public:
    // state 遷移直後 (新 state の OnEnter 相当) に発火。
    // user : SetOnStateEnterCallback で渡した不透明ポインタ。
    // from : 直前 state (初回 Tick 等で history 無い場合は current 自身)。
    // to   : 新 state (= CurrentState())。
    using StateEnterCallback = void(*)(void* user,
                                       EAnimationGraphState from,
                                       EAnimationGraphState to) noexcept;

    // 現 clip が末尾に達したとき (looping=false 時のみ) 発火。
    // user        : SetOnClipEndCallback で渡した不透明ポインタ。
    // state       : 末尾に達した clip を含む state ID (= CurrentState())。
    // clip_index  : 末尾に達した clip の index。
    using ClipEndCallback = void(*)(void* user,
                                    EAnimationGraphState state,
                                    u32 clip_index) noexcept;

    FAnimationGraph() noexcept = default;
    ~FAnimationGraph() noexcept = default;

    // 非コピー・非ムーブ (callback の self ポインタとの競合を防ぐ; ACS 規約)
    FAnimationGraph(const FAnimationGraph&)            = delete;
    FAnimationGraph& operator=(const FAnimationGraph&) = delete;
    FAnimationGraph(FAnimationGraph&&)                 = delete;
    FAnimationGraph& operator=(FAnimationGraph&&)      = delete;

    // ----- 初期化 / 解放 ---------------------------------------------------

    // 内部 state / clip / transition / param を全てクリア + callback を nullptr に。
    // 多重 Init 可 (= 既存内容を破棄して再構築)。
    void Init() noexcept;

    // 内部 TArray を Clear するだけ (~TArray が容量を解放するので明示 Shutdown は
    // 主にライフサイクル明示用)。多重 Shutdown 可。
    void Shutdown() noexcept;

    // ----- clip 管理 -------------------------------------------------------

    // clip を登録し、内部 index を返す。失敗は無いが超過時は最終 index を返す。
    u32                         AddClip(const FAnimationClipBinding& clip) noexcept;
    u32                         ClipCount() const noexcept;
    const FAnimationClipBinding* GetClip(u32 i) const noexcept;

    // ----- state node 管理 -------------------------------------------------

    // 既存 ID と同じ node を追加すると "後勝ち" で上書き (= 同 ID 重複を許容しない)。
    void                       AddStateNode(const FAnimationStateNode& node) noexcept;
    u32                        StateNodeCount() const noexcept;
    const FAnimationStateNode*  GetStateNode(u32 i) const noexcept;

    // ----- transition 管理 -------------------------------------------------

    // transition を追加。重複チェックなし (= 同 from→to を複数登録すれば最初の
    // ものが優先される; AddTransition 順がそのまま評価順)。
    void                        AddTransition(const FAnimationTransition& trans) noexcept;
    u32                         TransitionCount() const noexcept;
    const FAnimationTransition*  GetTransition(u32 i) const noexcept;

    // ----- パラメータ -------------------------------------------------------

    // 遷移条件で参照される f32 param を設定。name 既存なら値更新、無ければ追加。
    // name == nullptr は no-op。name の寿命は caller 管理。
    void SetParam(const char* name, f32 value) noexcept;

    // param 取得。未登録 / name==nullptr は 0.0f。
    f32  GetParam(const char* name) const noexcept;

    // ----- 遷移制御 --------------------------------------------------------

    // 明示的遷移要求。next Tick で transition 評価より優先して新 state へ移る
    // (= 攻撃ボタン等の input event を直接遷移に使うため)。
    // 同 state を指定した場合は no-op (= 同 state への self-loop は明示的に
    // クリアしないと意図せぬ再 enter を招くため弾く)。
    void TriggerTransition(EAnimationGraphState target_state) noexcept;

    // ----- 状態取得 --------------------------------------------------------

    EAnimationGraphState CurrentState()      const noexcept { return m_CurrentState;  }
    EAnimationGraphState PreviousState()     const noexcept { return m_PreviousState; }
    f32                  CurrentLocalTime()  const noexcept { return m_LocalTime;     }

    // blend 中なら 0..1 (新状態の比率)、否なら 1.0。
    // blend_duration <= 0 のときは即時切替なので常に 1.0。
    f32                  CurrentBlendAlpha() const noexcept;

    // 現 state に紐づく clip index。state 未設定なら 0。
    u32                  CurrentClipIndex()  const noexcept;

    // ----- 主処理 ----------------------------------------------------------

    // ・current state の clip local_time を進める (looping は wrap、once は終端
    //   固定 + ClipEndCallback)
    // ・blend timer を進める (alpha 計算は CurrentBlendAlpha() の lazy 計算)
    // ・pending trigger があれば優先処理 (= 即遷移)
    // ・各 transition (from==current) を評価し、条件成立で遷移
    // dt <= 0 は no-op (= 巻き戻し非対応; FSpriteAnimator と同方針)。
    void Tick(f32 dt) noexcept;

    // 全状態を初期化 (current = first state、local_time = 0、blend = 完了)。
    // clip / state / transition / param 設定はそのまま維持 (= Init との違い)。
    // callback も保持。state node が 0 個なら何もせず current_state も変更しない。
    void Reset() noexcept;

    // ----- callback --------------------------------------------------------

    void SetOnStateEnterCallback(StateEnterCallback cb, void* user) noexcept;
    void SetOnClipEndCallback   (ClipEndCallback    cb, void* user) noexcept;

private:
    struct Param {
        const char* name  = nullptr;
        f32         value = 0.0f;
    };

    // state ID から内部 _state_nodes 配列の index を引く。見つからなければ -1
    // (= u32 で UINT32_MAX) を返す。
    u32 FindStateNodeIndex(EAnimationGraphState id) const noexcept;

    // 新 state への遷移を実行 (blend timer set、prev/cur 更新、callback 発火)。
    // target が現状態と同じ場合でも強制 enter する (= TriggerTransition での
    // 明示再起動は許容したい場合; ただし内部呼び出しは事前に弾く想定)。
    void DoTransition(EAnimationGraphState target) noexcept;

    // 現 state の transition を 1 つ評価し、成立すれば遷移して true を返す。
    // exit_immediately=false の場合は「Once clip 終端到達まで」発火を保留する。
    // 1 Tick で複数連続遷移を防ぐため、true を返したら呼出側はループを止める。
    bool EvaluateTransitions() noexcept;

    // local_time を current clip の duration に応じて進行 + wrap/clamp。
    // 末尾到達で is_looping=false なら true を返す (ClipEndCallback 発火条件)。
    bool AdvanceLocalTime(f32 dt) noexcept;

    // ---- データ ----
    TArray<FAnimationClipBinding> m_Clips;
    TArray<FAnimationStateNode>   _state_nodes;
    TArray<FAnimationTransition>  m_Transitions;
    TArray<Param>                m_Params;

    // ---- 実行時状態 ----
    EAnimationGraphState m_CurrentState    = EAnimationGraphState::Idle;
    EAnimationGraphState m_PreviousState   = EAnimationGraphState::Idle;
    f32                  m_LocalTime       = 0.0f; // 現 clip 内の経過秒
    f32                  m_BlendTimer      = 0.0f; // 0 = blend 完了 / >0 = blend 残時間
    f32                  m_BlendDuration   = 0.0f; // 現遷移の総 blend 時間
    bool                 m_HasCurrent      = false;// state node が 1 つでも入って Reset 済か
    bool                 m_ClipEndedFired = false;// 同 Once clip で多重発火を防ぐ
    EAnimationGraphState m_PendingTrigger  = EAnimationGraphState::Idle;
    bool                 m_TriggerPending  = false;

    // ---- callback ----
    StateEnterCallback _state_enter_cb   = nullptr;
    void*              _state_enter_user = nullptr;
    ClipEndCallback    m_ClipEndCb      = nullptr;
    void*              m_ClipEndUser    = nullptr;
};

} // namespace acs::game
