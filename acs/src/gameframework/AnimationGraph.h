// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L — FAnimationGraph
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
//     より複雑な条件 (== / <= / && / ||) は持たず、単純化優先。
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
// 参考: FSpriteAnimator (frame anim), FAnimationCurve (curve), FStateMachine<T> (FSM 基盤),
//      FModelAnimationPanel (UI 連動可能)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * 標準状態 ID (9 固定)。
 *
 * @details
 * 一般的な 3D キャラ制御で頻出する状態を列挙。Custom1/2 はゲーム固有 (掴み /
 * 詠唱 / 変身等) を吸収する拡張枠。それでも足りない場合は本 enum を拡張する
 * (ACS は「状態は数十個程度」を上限想定; 数百を超えるなら別設計)。
 */
enum class EAnimationGraphState : u8 {
    /** 待機。 */
    Idle    = 0,

    /** 歩き。 */
    Walk    = 1,

    /** 走り。 */
    Run     = 2,

    /** ジャンプ。 */
    Jump    = 3,

    /** 攻撃。 */
    Attack  = 4,

    /** 被弾。 */
    Hit     = 5,

    /** 死亡。 */
    Death   = 6,

    /** ゲーム固有の拡張枠 1。 */
    Custom1 = 7,

    /** ゲーム固有の拡張枠 2。 */
    Custom2 = 8,
};

/**
 * clip メタ情報 (state からは clip_index で参照する)。
 *
 * @details
 * tools/modelview/FModelAnimationPanel.h にある同名構造は acs::game::modelview
 * 名前空間で別物。本構造は acs::game 直下に置く。
 */
struct FAnimationClipBinding {
    /** デバッグ表示 / 外部 FAnimationPlayer 解決用名 (literal 寿命は caller 管理)。 */
    const char* clip_name     = nullptr;

    /** clip の長さ (秒)。0 以下は内部で 0 にクランプして進行させない。 */
    f32         duration_sec  = 0.0f;

    /** true なら local time が duration を超えたら wrap、false なら末尾固定で ClipEndCallback を発火。 */
    bool        is_looping    = false;

    /** Tick で local_time に乗る既定再生速度 (1.0 = 等速)。 */
    f32         default_speed = 1.0f;
};

/**
 * graph 上の 1 ノード (= 1 つの状態)。
 */
struct FAnimationStateNode {
    /** デバッグ表示用 (UI / log)。literal 寿命は caller 管理。 */
    const char*         name             = nullptr;

    /** この state の論理 ID。AddStateNode で重複登録は後勝ち。 */
    EAnimationGraphState id              = EAnimationGraphState::Idle;

    /** 再生する FAnimationClipBinding の index (AddClip 戻り値)。範囲外は Tick 内で 0 にクランプ。 */
    u32                 clip_index       = 0u;

    /** この state に入るときに使う blend 長 (秒)。0 以下なら即時切替 (alpha=1.0 で開始)。 */
    f32                 enter_blend_sec  = 0.0f;

    /** この state から抜けるとき呼出側が参考にする値 (enter 側優先採用のため情報提供のみ)。 */
    f32                 exit_blend_sec   = 0.0f;
};

/**
 * from → to の遷移ルール。
 */
struct FAnimationTransition {
    /** 元状態 (current_state とこれが一致時のみ評価)。 */
    EAnimationGraphState from                      = EAnimationGraphState::Idle;

    /** 遷移先。 */
    EAnimationGraphState to                        = EAnimationGraphState::Idle;

    /** condition_param_name 値が >= この閾値で発火。 */
    f32                  condition_param_threshold = 0.0f;

    /** 比較する param 名 (literal 寿命は caller 管理)。nullptr なら exit_immediately だけ評価。 */
    const char*          condition_param_name      = nullptr;

    /** true なら param 条件成立で即発火、false なら現 clip が終端まで再生してから発火 (Once clip 用)。 */
    bool                 exit_immediately          = true;
};

/**
 * multi-graph 管理用 handle (現状未使用)。
 *
 * @details
 * 24bit index + 8bit gen を packed (Cooldown/FSceneTimer と同方針)。
 * FAnimationGraph 単体使用想定だが、複数グラフ (= 上半身 / 下半身別レイヤ) を
 * 導入する際に再利用するため API として公開しておく。
 */
struct GraphHandle {
    /** index と generation を packed した値 (0 = 無効)。 */
    u32 m_Packed = 0u;

    /**
     * 有効な handle かを返す。
     *
     * @return packed が 0 でなければ true。
     */
    bool IsValid() const noexcept { return m_Packed != 0u; }

    /** index フィールドのビット幅。 */
    static constexpr u32 kIndexBits = 24u;

    /** index フィールドを取り出すマスク (0x00FFFFFF)。 */
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u;

    /** 表現可能な最大 index (16777215)。 */
    static constexpr u32 kMaxIndex  = kIndexMask;

    /**
     * index と generation を packed して handle を作る。
     *
     * @param index 24bit に収まる配列インデックス。
     * @param gen 8bit の generation 値。
     * @return packed した GraphHandle。
     */
    static GraphHandle Pack(u32 index, u8 gen) noexcept {
        GraphHandle h;
        h.m_Packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return h;
    }

    /**
     * packed 値から index を取り出す。
     *
     * @return 下位 24bit の index。
     */
    u32 Index() const noexcept { return m_Packed & kIndexMask; }

    /**
     * packed 値から generation を取り出す。
     *
     * @return 上位 8bit の generation。
     */
    u8  Gen()   const noexcept { return static_cast<u8>(m_Packed >> kIndexBits); }

    /**
     * packed 値が等しいかを比較する。
     *
     * @param o 比較する handle。
     * @return packed が一致すれば true。
     */
    constexpr bool operator==(GraphHandle o) const noexcept { return m_Packed == o.m_Packed; }

    /**
     * packed 値が異なるかを比較する。
     *
     * @param o 比較する handle。
     * @return packed が異なれば true。
     */
    constexpr bool operator!=(GraphHandle o) const noexcept { return m_Packed != o.m_Packed; }
};

/**
 * state machine ベースの blend graph (3D skeleton-anim 向け)。
 *
 * @details
 * 「現在再生中の clip + 遷移ブレンド」という meta 制御のみを担い、どの clip を /
 * いつから / どの blend alpha で再生するかを提供する。bone palette 計算や GPU upload
 * は呼出側 renderer に委譲する。状態は EAnimationGraphState の固定 ID で表し、遷移条件は
 * (param_name, threshold) または TriggerTransition による明示指定。enter blend 長は遷移先
 * state 側に持たせ、blend 中は previous_state と blend_alpha (0→1) を出力する。
 * 非コピー・非ムーブ、全 noexcept、STL 不使用。
 */
class FAnimationGraph {
public:
    /**
     * state 遷移直後 (新 state の OnEnter 相当) に発火する callback 型。
     *
     * @param user SetOnStateEnterCallback で渡した不透明ポインタ。
     * @param from 直前 state (初回 Tick 等で history が無い場合は current 自身)。
     * @param to 新 state (= CurrentState())。
     */
    using StateEnterCallback = void(*)(void* user,
                                       EAnimationGraphState from,
                                       EAnimationGraphState to) noexcept;

    /**
     * 現 clip が末尾に達したとき (looping=false 時のみ) 発火する callback 型。
     *
     * @param user SetOnClipEndCallback で渡した不透明ポインタ。
     * @param state 末尾に達した clip を含む state ID (= CurrentState())。
     * @param clip_index 末尾に達した clip の index。
     */
    using ClipEndCallback = void(*)(void* user,
                                    EAnimationGraphState state,
                                    u32 clip_index) noexcept;

    /** 空のグラフを構築する (clip / state / transition なし)。 */
    FAnimationGraph() noexcept = default;

    /** 破棄する (内部 TArray が解放)。 */
    ~FAnimationGraph() noexcept = default;

    /** コピー禁止 (callback の self ポインタとの競合を防ぐ)。 */
    FAnimationGraph(const FAnimationGraph&)            = delete;

    /** コピー代入も禁止。 */
    FAnimationGraph& operator=(const FAnimationGraph&) = delete;

    /** ムーブ禁止 (ACS 規約)。 */
    FAnimationGraph(FAnimationGraph&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FAnimationGraph& operator=(FAnimationGraph&&)      = delete;

    /**
     * 内部 state / clip / transition / param を全てクリアし callback を nullptr にする。
     *
     * @details 多重 Init 可 (= 既存内容を破棄して再構築)。
     */
    void Init() noexcept;

    /**
     * 内部 TArray を Clear して実行時状態をリセットする。
     *
     * @details ~TArray が容量を解放するので明示 Shutdown は主にライフサイクル明示用。多重 Shutdown 可。
     */
    void Shutdown() noexcept;

    /**
     * clip を登録し、内部 index を返す。
     *
     * @param clip 登録する clip メタ情報。
     * @return 登録した clip の index。
     */
    u32                         AddClip(const FAnimationClipBinding& clip) noexcept;

    /**
     * 登録済み clip の数を返す。
     *
     * @return clip の個数。
     */
    u32                         ClipCount() const noexcept;

    /**
     * 指定 index の clip メタ情報を返す。
     *
     * @param i 取得する clip のインデックス。
     * @return clip への const ポインタ (範囲外なら nullptr)。
     */
    const FAnimationClipBinding* GetClip(u32 i) const noexcept;

    /**
     * state node を追加する (同 ID は後勝ちで上書き)。
     *
     * @details 同 ID 重複は許容せず、初回追加なら初期状態としてセットされる。
     * @param node 追加する state node。
     */
    void                       AddStateNode(const FAnimationStateNode& node) noexcept;

    /**
     * 登録済み state node の数を返す。
     *
     * @return state node の個数。
     */
    u32                        StateNodeCount() const noexcept;

    /**
     * 指定 index の state node を返す。
     *
     * @param i 取得する state node のインデックス。
     * @return state node への const ポインタ (範囲外なら nullptr)。
     */
    const FAnimationStateNode*  GetStateNode(u32 i) const noexcept;

    /**
     * transition を追加する。
     *
     * @details 重複チェックなし。AddTransition 順がそのまま評価順となり、同 from→to は先頭が優先される。
     * @param trans 追加する遷移ルール。
     */
    void                        AddTransition(const FAnimationTransition& trans) noexcept;

    /**
     * 登録済み transition の数を返す。
     *
     * @return transition の個数。
     */
    u32                         TransitionCount() const noexcept;

    /**
     * 指定 index の transition を返す。
     *
     * @param i 取得する transition のインデックス。
     * @return transition への const ポインタ (範囲外なら nullptr)。
     */
    const FAnimationTransition*  GetTransition(u32 i) const noexcept;

    /**
     * 遷移条件で参照される f32 param を設定する。
     *
     * @details name 既存なら値更新、無ければ追加。name の寿命は caller 管理。name == nullptr は no-op。
     * @param name param 名。
     * @param value 設定する値。
     */
    void SetParam(const char* name, f32 value) noexcept;

    /**
     * param の値を取得する。
     *
     * @param name 取得する param 名。
     * @return param 値 (未登録 / name==nullptr なら 0.0f)。
     */
    f32  GetParam(const char* name) const noexcept;

    /**
     * 明示的な遷移を要求する (次 Tick で transition 評価より優先)。
     *
     * @details
     * 攻撃ボタン等の input event を直接遷移に使うための機構。同 state を指定した場合は
     * 意図せぬ再 enter を防ぐため no-op。
     * @param target_state 遷移先の state ID。
     */
    void TriggerTransition(EAnimationGraphState target_state) noexcept;

    /**
     * 現在の state ID を返す。
     *
     * @return 現在の EAnimationGraphState。
     */
    EAnimationGraphState CurrentState()      const noexcept { return m_CurrentState;  }

    /**
     * 直前の state ID を返す。
     *
     * @return 直前の EAnimationGraphState。
     */
    EAnimationGraphState PreviousState()     const noexcept { return m_PreviousState; }

    /**
     * 現 clip 内の経過秒 (local time) を返す。
     *
     * @return 現 clip 内の経過秒。
     */
    f32                  CurrentLocalTime()  const noexcept { return m_LocalTime;     }

    /**
     * 現在の blend alpha を返す。
     *
     * @details blend_duration <= 0 のときは即時切替なので常に 1.0。
     * @return blend 中なら 0..1 (新状態の比率)、否なら 1.0。
     */
    f32                  CurrentBlendAlpha() const noexcept;

    /**
     * 現 state に紐づく clip index を返す。
     *
     * @return 現 state の clip index (state 未設定なら 0)。
     */
    u32                  CurrentClipIndex()  const noexcept;

    /**
     * グラフを 1 フレーム進める。
     *
     * @details
     * pending trigger を最優先で処理し、blend timer を進め、current state の clip
     * local_time を前進させ (looping は wrap、once は終端固定 + ClipEndCallback)、
     * from==current の transition を評価して条件成立で遷移する。dt <= 0 は no-op。
     * @param dt 経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 実行時状態を初期化する (current = 先頭 state、local_time = 0、blend = 完了)。
     *
     * @details
     * clip / state / transition / param 設定と callback はそのまま維持する (Init との違い)。
     * state node が 0 個なら current_state を変更しない。
     */
    void Reset() noexcept;

    /**
     * state 遷移時に呼ぶ callback を設定する。
     *
     * @param cb 遷移直後に呼ぶ callback (nullptr で無効化)。
     * @param user callback に渡す不透明ポインタ。
     */
    void SetOnStateEnterCallback(StateEnterCallback cb, void* user) noexcept;

    /**
     * clip 終端到達時に呼ぶ callback を設定する。
     *
     * @param cb clip 末尾到達時に呼ぶ callback (nullptr で無効化)。
     * @param user callback に渡す不透明ポインタ。
     */
    void SetOnClipEndCallback   (ClipEndCallback    cb, void* user) noexcept;

private:
    /**
     * 名前付きの f32 パラメータ 1 個。
     */
    struct Param {
        /** param 名 (literal 寿命は caller 管理)。 */
        const char* name  = nullptr;

        /** param の現在値。 */
        f32         value = 0.0f;
    };

    /**
     * state ID から内部 _state_nodes 配列の index を引く。
     *
     * @param id 探す state ID。
     * @return 見つかった index (無ければ kInvalidIndex)。
     */
    u32 FindStateNodeIndex(EAnimationGraphState id) const noexcept;

    /**
     * 新 state への遷移を実行する (blend timer set、prev/cur 更新、callback 発火)。
     *
     * @details
     * target が現状態と同じ場合でも強制 enter する。未登録 state への遷移は無視する。
     * @param target 遷移先の state ID。
     */
    void DoTransition(EAnimationGraphState target) noexcept;

    /**
     * 現 state の transition を順に評価し、成立すれば遷移する。
     *
     * @details
     * exit_immediately=false の場合は Once clip 終端到達まで発火を保留する。
     * 1 Tick で複数連続遷移を防ぐため、遷移したら呼出側はループを止める。
     * @return 遷移が起きたら true。
     */
    bool EvaluateTransitions() noexcept;

    /**
     * local_time を current clip の duration に応じて進行させる (wrap/clamp)。
     *
     * @param dt 経過秒。
     * @return 末尾到達で is_looping=false なら true (ClipEndCallback 発火条件)。
     */
    bool AdvanceLocalTime(f32 dt) noexcept;

    /** 登録済み clip メタ情報。 */
    TArray<FAnimationClipBinding> m_Clips;

    /** 登録済み state node。 */
    TArray<FAnimationStateNode>   _state_nodes;

    /** 登録済み transition (評価順 = 追加順)。 */
    TArray<FAnimationTransition>  m_Transitions;

    /** 遷移条件で参照される param 群。 */
    TArray<Param>                m_Params;

    /** 現在の state ID。 */
    EAnimationGraphState m_CurrentState    = EAnimationGraphState::Idle;

    /** 直前の state ID。 */
    EAnimationGraphState m_PreviousState   = EAnimationGraphState::Idle;

    /** 現 clip 内の経過秒。 */
    f32                  m_LocalTime       = 0.0f;

    /** blend 残時間 (0 = blend 完了 / >0 = 残時間)。 */
    f32                  m_BlendTimer      = 0.0f;

    /** 現遷移の総 blend 時間。 */
    f32                  m_BlendDuration   = 0.0f;

    /** state node が 1 つでも入って初期状態がセット済みか。 */
    bool                 m_HasCurrent      = false;

    /** 同 Once clip での ClipEndCallback 多重発火を防ぐフラグ。 */
    bool                 m_ClipEndedFired = false;

    /** TriggerTransition で要求された遷移先 (m_bTriggerPending が true のとき有効)。 */
    EAnimationGraphState m_PendingTrigger  = EAnimationGraphState::Idle;

    /** 明示的遷移の予約が立っているか。 */
    bool                 m_bTriggerPending  = false;

    /** state 遷移時 callback。 */
    StateEnterCallback _state_enter_cb   = nullptr;

    /** state 遷移時 callback に渡す不透明ポインタ。 */
    void*              _state_enter_user = nullptr;

    /** clip 終端到達時 callback。 */
    ClipEndCallback    m_ClipEndCb      = nullptr;

    /** clip 終端到達時 callback に渡す不透明ポインタ。 */
    void*              m_ClipEndUser    = nullptr;
};

} // namespace acs::game
