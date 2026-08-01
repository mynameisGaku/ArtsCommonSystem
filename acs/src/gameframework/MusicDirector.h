// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — CMusicDirector (適応 BGM + Stinger)
//
// CAudioDirector が「低レイヤの mixer / クロスフェード」を担うのに対し、
// 本 CMusicDirector は「ゲームプレイ状態 (戦闘 / 平穏 / 勝利 ...) を BGM へ
// マッピングする上位レイヤ」である。両者はシーン跨ぎ生存で、CGame (or app)
// 側が同列に保持する想定 (BGM 状態はシーン切替で途切れない方が自然)。
//
// 機能:
//   ・EMusicState (6 種): Silent / Calm / Tension / Combat / Victory / GameOver
//   ・state -> track 配列 (1 状態に複数 track を登録、intensity range で選択)
//   ・SetState(state, transition_sec) で状態クロスフェード
//   ・SetIntensity(0..1) で「同じ状態内での激しさ」を表現
//     (Combat 内で 0.2 = 探索的、0.9 = 高激戦、など intensity range で track 分岐)
//   ・PlayStinger() で「BGM を停めずに重ねる一発もの SFX」(ボス出現 / 達成等)
//     実演奏は CAudioDirector::PlaySfx に流す (本クラスは pending 情報も保持)
//   ・Tick(dt) で transition / stinger timer を進行
//
// 設計選択:
//   ・**CAudioEngine 直叩きしない**: 実際の再生は CAudioDirector に委ね、本クラスは
//     「どの track を、どのゲインで鳴らすべきか」を決める state machine に徹する。
//     SetAudioDirector(CAudioDirector*) で結線すると、SetState → PlayBgm、
//     PlayStinger → PlaySfx、Stop → StopBgm を実 director へ delegate する。
//     未結線 (nullptr) のときは従来通り state-only で動作する (無音、crash しない)。
//   ・**state -> track 配列は SoA で分離**: `m_Tracks` は全 track をフラットに
//     保持し、`_state_first[state]` + `_state_count[state]` で各 state の
//     区間を表現。挿入順は state ごとにグループ化される。RegisterTrack で
//     後付け挿入時は末尾追加 + state インデックス再構築 (簡素 vs O(N)、
//     登録は init 時に集中して呼ぶ前提で十分速い)。
//   ・**Track 選択**: 同一 state に複数 track が登録されている場合、現在 intensity が
//     [intensity_min, intensity_max] に含まれる最初の track を再生対象とする。
//     どれにも該当しなければ「最も intensity_max が大きい (= 最強) track」を選ぶ
//     (fallback、無音にしない方が音楽体験として自然)。
//   ・**transition は state 単位**: track 切替もすべて state 切替に伴う。
//     intensity 変化のみによる中間 track 切替は実装せず、
//     SetState(m_Current, 0) のような自己再要求で明示的に発火する形にする。
//   ・**Stinger は単一バッファ**: 1 個分だけ「次に演奏すべき stinger」を保持し、
//     ConsumeStinger() で取り出すまでは pending=true。複数同時投入は警告 +
//     最新を採用 (BGM 上に重ねる前提なので、多重スタックは認知的にノイズ)。
//   ・**name / asset_path は所有しない**: CAudioDirector と同様に文字列リテラル前提。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Forward.h"

namespace acs::game {

/**
 * 適応 BGM の状態。
 *
 * @details
 * 値は安定なので save に書ける (将来 enum 追加時は末尾追加のみ、既存値の意味は
 * 不変とする規約)。SetState で状態を切り替えると state→track マッピングに従って
 * BGM がクロスフェードする。
 */
enum class EMusicState : u8 {
    /** 無音 (タイトル前 / 思索シーン等)。 */
    Silent   = 0,

    /** 平穏 (探索 / 街)。 */
    Calm     = 1,

    /** 緊張 (敵接近 / ステルス)。 */
    Tension  = 2,

    /** 戦闘。 */
    Combat   = 3,

    /** 勝利 / クリア。 */
    Victory  = 4,

    /** 敗北 / ゲームオーバー。 */
    GameOver = 5,
};

/**
 * 適応 BGM 1 トラック分の登録情報。
 *
 * @details
 * intensity range で再生条件を絞り込める。現在 intensity が
 * [intensity_min, intensity_max] に含まれるとき本 track が候補となり、range を
 * [0, 1] にすれば常時候補となる。
 */
struct FMusicTrack {
    /** 再生するアセットのパス (所有しない。literal 前提)。 */
    const char* asset_path    = nullptr;

    /** 候補となる intensity 下限 [0, 1]。 */
    f32         intensity_min = 0.0f;

    /** 候補となる intensity 上限 [0, 1]。 */
    f32         intensity_max = 1.0f;

    /** true でループ再生 (Calm / Combat 等)、false で one-shot (Victory 等)。 */
    bool        loop          = true;
};

/**
 * ゲームプレイ状態を BGM へマッピングする適応 BGM の上位 state machine。
 *
 * @details
 * EMusicState (Silent / Calm / Tension / Combat / Victory / GameOver) を track 配列に
 * マッピングし、SetState で状態クロスフェード、SetIntensity で同状態内の激しさを
 * 表現する。PlayStinger は BGM を停めずに一発 SFX を重ねる。実再生は
 * SetAudioDirector で結線した CAudioDirector へ delegate し、未結線時は state-only
 * で動作する (無音、crash しない)。1 インスタンスをシーン跨ぎで保持する想定の
 * non-copy / non-move 型。
 */
class CMusicDirector {
public:
    /** EMusicState の総数 (enum 末尾追加時はここを増やす)。 */
    static constexpr u32 kStateCount = 6;

    /** track 配列の事前確保目安 (超えても自動拡張する)。 */
    static constexpr u32 kTrackReserveHint = 16;

    /** track 配列を事前確保し state インデックスを初期化して構築する。 */
    CMusicDirector() noexcept;

    /** 破棄する (内部 TArray が解放される。m_Audio は非所有なので触らない)。 */
    ~CMusicDirector() noexcept = default;

    /** コピー禁止 (シーン跨ぎの単一インスタンスを保つため)。 */
    CMusicDirector(const CMusicDirector&)            = delete;

    /** コピー代入も禁止。 */
    CMusicDirector& operator=(const CMusicDirector&) = delete;

    /** ムーブ禁止。 */
    CMusicDirector(CMusicDirector&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CMusicDirector& operator=(CMusicDirector&&)      = delete;

    /**
     * 指定 state に track を登録する。
     *
     * @details
     * 同一 state に複数 track 登録可。intensity range が重なる場合は登録順で最初に
     * 該当した track が選択される。asset_path == nullptr は警告 + 無視。intensity は
     * [0, 1] にクランプし min <= max を保証する。
     * @param state 登録先の state。
     * @param track 登録する track 情報。
     */
    void RegisterTrack(EMusicState state, const FMusicTrack& track) noexcept;

    /**
     * current state から指定 state へクロスフェードを開始する。
     *
     * @details
     * transition_sec <= 0 は即時切替 (progress=1 にスナップ)。同一 state の再要求は
     * 遷移中でなければ no-op (現行 BGM 継続)。結線済みなら新 state の track を
     * CAudioDirector へ流す。
     * @param state 遷移先の state。
     * @param transition_sec クロスフェード秒数 (既定 2.0、0 以下で即時)。
     */
    void SetState(EMusicState state, f32 transition_sec = 2.0f) noexcept;

    /**
     * 現在再生中の state を返す。
     *
     * @return current state。
     */
    EMusicState CurrentState() const noexcept { return m_CurrentState; }

    /**
     * 遷移先の state を返す。
     *
     * @return target state (遷移中でなければ current と同値)。
     */
    EMusicState TargetState()  const noexcept { return m_TargetState; }

    /**
     * クロスフェード中かを返す。
     *
     * @return current != target かつ progress < 1 なら true。
     */
    bool IsTransitioning() const noexcept;

    /**
     * 遷移の進捗を返す。
     *
     * @return [0, 1] の進捗 (1 で current = target に snap 完了)。
     */
    f32 TransitionProgress() const noexcept { return m_TransitionProgress; }

    /**
     * intensity (同 state 内の激しさ) を設定する。
     *
     * @details [0, 1] にクランプ。範囲外は警告 + clamp して受理する。
     * @param intensity_0_to_1 設定する intensity [0, 1]。
     */
    void SetIntensity(f32 intensity_0_to_1) noexcept;

    /**
     * 現在の intensity を返す。
     *
     * @return [0, 1] の intensity。
     */
    f32  Intensity() const noexcept { return m_Intensity; }

    /**
     * 一回限りの stinger SFX を投入する (BGM を停めず重ねる)。
     *
     * @details
     * asset_path == nullptr / volume <= 0 は警告 + 無視。既に pending stinger がある
     * 場合は上書き (最新を採用)。pending 情報は ConsumeStinger 用に常に保持し、
     * 結線済みなら即時に CAudioDirector::PlaySfx へ流す。
     * @param asset_path 鳴らす SFX アセットのパス (所有しない)。
     * @param volume 再生ゲイン (既定 1.0、0 以下で無視)。
     */
    void PlayStinger(const char* asset_path, f32 volume = 1.0f) noexcept;

    /**
     * 未消費の stinger があるかを返す。
     *
     * @return pending な stinger があれば true。
     */
    bool HasPendingStinger() const noexcept { return m_StingerPending; }

    /**
     * 保持中の stinger を取り出し、pending=false に戻す。
     *
     * @param out_volume 取り出した stinger のゲインを書き込む先 (pending なしなら 0)。
     * @return stinger のアセットパス (pending なしなら nullptr)。
     */
    const char* ConsumeStinger(f32& out_volume) noexcept;

    /**
     * 実再生を委ねる低レイヤ CAudioDirector を結線する。
     *
     * @details
     * 結線後は SetState → PlayBgm、PlayStinger → PlaySfx、Stop → StopBgm を実
     * director へ delegate する。nullptr で切断すると state-only 動作に戻る。
     * 呼び出し側が director を所有する (本クラスは非所有 raw ptr で保持)。
     * @param audio 結線する CAudioDirector (nullptr で切断)。
     */
    void           SetAudioDirector(CAudioDirector* audio) noexcept { m_Audio = audio; }

    /**
     * 結線済みの CAudioDirector を返す。
     *
     * @return 結線中の CAudioDirector (未結線なら nullptr)。
     */
    CAudioDirector* GetAudioDirector() const noexcept { return m_Audio; }

    /**
     * 毎フレーム呼んで transition / stinger timer を進める。
     *
     * @details CSceneServices / CGame から呼ぶ。state machine の遷移進捗のみを進める。
     * @param dt 前フレームからの経過実秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 全停止する。
     *
     * @details
     * state を Silent に即時切替、stinger pending を破棄、intensity は保持する。
     * 結線済みなら実 BGM も即時停止 (fade=0)。
     */
    void Stop() noexcept;

    /**
     * 現在 state で intensity に合致する track を返す。
     *
     * @return 該当 track へのポインタ (該当なし or 未登録なら nullptr)。
     */
    const FMusicTrack* CurrentTrack() const noexcept;

    /**
     * 遷移先 state 側の track を返す。
     *
     * @return target state の track へのポインタ (遷移していなければ nullptr)。
     */
    const FMusicTrack* TargetTrack() const noexcept;

private:
    /**
     * state インデックスの整合性を取り直す (将来の Unregister 用 hook)。
     *
     * @details 現状は RegisterTrack 内で逐次更新するため空関数。
     */
    void RebuildStateIndex() noexcept;

    /**
     * 指定 state 内で intensity に合う track の index を返す。
     *
     * @details
     * intensity 範囲ヒットを線形探索し、どれにも該当しなければ最も intensity_max が
     * 大きい track を fallback として返す。
     * @param state 探す state。
     * @param intensity 判定に使う現在 intensity。
     * @return m_Tracks 上の index (該当 state に track 無しなら m_Tracks.Size())。
     */
    usize FindTrackForState(EMusicState state, f32 intensity) const noexcept;

    /**
     * 値を [0, 1] にクランプする。
     *
     * @param v クランプ対象。
     * @return [0, 1] に収めた値。
     */
    static f32 Clamp01(f32 v) noexcept;

    /**
     * 現在 state / intensity に応じた track を選び、結線済みなら BGM 再生へ流す。
     *
     * @details
     * track 未登録なら StopBgm を呼ぶ。未結線 (m_Audio == nullptr) なら no-op
     * (state-only)。
     * @param fade_in_sec CAudioDirector へ渡すクロスフェード秒 (即時切替なら 0)。
     */
    void RouteCurrentTrackToAudio(f32 fade_in_sec) noexcept;

    /** 全 state の track をフラットに保持する倉庫。 */
    TArray<FMusicTrack> m_Tracks;

    /** 各 state の m_Tracks 上の開始 index (SoA で state→range mapping)。 */
    u32 _state_first[kStateCount] {};

    /** 各 state の m_Tracks 上の track 個数 (SoA で state→range mapping)。 */
    u32 _state_count[kStateCount] {};

    /** 現在再生中の state。 */
    EMusicState m_CurrentState = EMusicState::Silent;

    /** 遷移先の state (遷移中でなければ current と同値)。 */
    EMusicState m_TargetState  = EMusicState::Silent;

    /** 現在の遷移の総時間 (秒)。 */
    f32        m_TransitionDuration = 0.0f;

    /** 現在の遷移の経過時間 (秒)。 */
    f32        m_TransitionElapsed  = 0.0f;

    /** 遷移進捗 [0, 1] (1 = 完了 / snap 済み)。 */
    f32        m_TransitionProgress = 1.0f;

    /** 現在の intensity [0, 1]。 */
    f32 m_Intensity = 0.0f;

    /** pending な stinger のアセットパス (所有しない)。 */
    const char* m_StingerPath    = nullptr;

    /** pending な stinger のゲイン。 */
    f32         m_StingerVolume  = 0.0f;

    /** stinger が未消費かを表すフラグ。 */
    bool        m_StingerPending = false;

    /** 実再生を委ねる低レイヤ director (非所有 raw ptr。nullptr で state-only)。 */
    CAudioDirector* m_Audio = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FMusicDirector = CMusicDirector;

} // namespace acs::game
