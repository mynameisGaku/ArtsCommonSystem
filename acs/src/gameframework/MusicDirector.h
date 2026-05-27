// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FMusicDirector (適応 BGM + Stinger)
//
// FAudioDirector が「低レイヤの mixer / クロスフェード」を担うのに対し、
// 本 FMusicDirector は「ゲームプレイ状態 (戦闘 / 平穏 / 勝利 ...) を BGM へ
// マッピングする上位レイヤ」である。両者はシーン跨ぎ生存で、FGame (or app)
// 側が同列に保持する想定 (BGM 状態はシーン切替で途切れない方が自然)。
//
// 機能:
//   ・EMusicState (6 種): Silent / Calm / Tension / Combat / Victory / GameOver
//   ・state -> track 配列 (1 状態に複数 track を登録、intensity range で選択)
//   ・SetState(state, transition_sec) で状態クロスフェード
//   ・SetIntensity(0..1) で「同じ状態内での激しさ」を表現
//     (Combat 内で 0.2 = 探索的、0.9 = 高激戦、など intensity range で track 分岐)
//   ・PlayStinger() で「BGM を停めずに重ねる一発もの SFX」(ボス出現 / 達成等)
//     実演奏は FAudioDirector::PlaySfx に流す想定 (本クラスは pending 情報のみ管理)
//   ・Tick(dt) で transition / stinger timer を進行
//
// 設計選択 (Phase 1: bridge スケルトン):
//   ・**FAudioEngine 直叩きしない**: 実際の再生は FAudioDirector / FAudioEngine に
//     委ねる前提で、本クラスは「どの track を、どのゲインで鳴らすべきか」を
//     決める state machine に徹する。Phase 2 で FAudioDirector と結線。
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
//     intensity 変化のみによる中間 track 切替は本 Phase では実装せず、
//     SetState(m_Current, 0) のような自己再要求で明示的に発火する形にする。
//   ・**Stinger は単一バッファ**: 1 個分だけ「次に演奏すべき stinger」を保持し、
//     ConsumeStinger() で取り出すまでは pending=true。複数同時投入は警告 +
//     最新を採用 (BGM 上に重ねる前提なので、多重スタックは認知的にノイズ)。
//   ・**name / asset_path は所有しない**: FAudioDirector と同様に文字列リテラル前提。
//     Phase 2 で FStringView / Asset Handle に置き換える。
//
// 範囲外 (Phase 2+ で):
//   ・FAudioDirector との実結線 (実 BGM 再生)
//   ・intensity 変化に追従する中間 track 切替 (state を変えずに track 切替)
//   ・ビート同期 stinger / 拍頭スナップ
//   ・横展開: vertical re-mixing (stem 分離 mix), pre-emption 優先度
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// 適応 BGM の状態。値は安定なので save に書ける (将来 enum 追加時は末尾追加のみ、
// 既存値の意味は不変とする規約)。
enum class EMusicState : u8 {
    Silent   = 0,  // 無音 (タイトル前 / 思索シーン等)
    Calm     = 1,  // 平穏 (探索 / 街)
    Tension  = 2,  // 緊張 (敵接近 / ステルス)
    Combat   = 3,  // 戦闘
    Victory  = 4,  // 勝利 / クリア
    GameOver = 5,  // 敗北 / ゲームオーバー
};

// 適応 BGM 1 トラック分の登録情報。intensity range で再生条件を絞り込める。
//   intensity_min / intensity_max: [0, 1]。現在 intensity がこの範囲に含まれる
//     ときに本 track が候補となる。range を [0, 1] にすれば常時候補。
//   loop: true でループ再生 (Calm / Combat 等)、false で one-shot (Victory 等)。
struct MusicTrack {
    const char* asset_path    = nullptr;  // 所有しない (literal 前提)
    f32         intensity_min = 0.0f;
    f32         intensity_max = 1.0f;
    bool        loop          = true;
};

class FMusicDirector {
public:
    // EMusicState の総数。enum 末尾追加時はここを増やす。
    static constexpr u32 kStateCount = 6;
    // 全 state 合計の track 登録上限 (リザーブ目安、超えても自動拡張する)。
    static constexpr u32 kTrackReserveHint = 16;

    FMusicDirector() noexcept;
    ~FMusicDirector() noexcept = default;

    FMusicDirector(const FMusicDirector&)            = delete;
    FMusicDirector& operator=(const FMusicDirector&) = delete;
    FMusicDirector(FMusicDirector&&)                 = delete;
    FMusicDirector& operator=(FMusicDirector&&)      = delete;

    // ----- track 登録 -----
    // 同一 state に複数 track 登録可。intensity range が重なる場合は登録順で
    // 最初に該当した track が選択される (Phase 2 で優先度 API を追加予定)。
    // asset_path == nullptr は警告 + 無視。
    void RegisterTrack(EMusicState state, const MusicTrack& track) noexcept;

    // ----- 状態遷移 -----
    // current state → state にクロスフェード開始。
    //   transition_sec <= 0: 即時切替 (progress=1 にスナップ)。
    //   同一 state 再要求: no-op (現行 BGM 継続、transition は途中なら継続)。
    void SetState(EMusicState state, f32 transition_sec = 2.0f) noexcept;

    EMusicState CurrentState() const noexcept { return m_CurrentState; }
    EMusicState TargetState()  const noexcept { return m_TargetState; }

    // クロスフェード中 (current != target かつ progress < 1) なら true。
    bool IsTransitioning() const noexcept;

    // [0, 1]。遷移中の進捗。1 で current = target に snap 完了。
    f32 TransitionProgress() const noexcept { return m_TransitionProgress; }

    // ----- intensity (戦闘激しさ等) -----
    // [0, 1] にクランプ。範囲外は警告 + clamp して受理。
    void SetIntensity(f32 intensity_0_to_1) noexcept;
    f32  Intensity() const noexcept { return m_Intensity; }

    // ----- Stinger (一回限りの SFX、BGM を停めず重ねる) -----
    // asset_path == nullptr / volume <= 0 は警告 + 無視。
    // 既に pending stinger がある場合は上書き (最新を採用)。
    void PlayStinger(const char* asset_path, f32 volume = 1.0f) noexcept;

    // 未消費の stinger があれば true。
    bool HasPendingStinger() const noexcept { return m_StingerPending; }

    // 保持中の stinger を取り出し、pending=false に戻す。out_volume にゲインを返す。
    // pending なしの場合 nullptr を返し、out_volume = 0。
    const char* ConsumeStinger(f32& out_volume) noexcept;

    // ----- driver -----
    // FSceneServices / FGame から毎フレーム呼ぶ。dt はリアル秒。
    void Tick(f32 dt) noexcept;

    // ----- 全停止 -----
    // 状態を Silent に即時切替、stinger pending を破棄、intensity は保持。
    void Stop() noexcept;

    // ----- 派生情報 (debug / Phase 2 で FAudioDirector に流す予定) -----
    // 現在 state で intensity に合致する track を返す。該当なし or 未登録なら nullptr。
    const MusicTrack* CurrentTrack() const noexcept;
    // 遷移中 (slot[1]) の target state 側 track。遷移していない場合は nullptr。
    const MusicTrack* TargetTrack() const noexcept;

private:
    // 内部 helper: state インデックスの整合性を取り直す。Register 後に呼ぶ。
    void RebuildStateIndex() noexcept;
    // state 内の track 配列から intensity に合う track index を m_Tracks 上で返す。
    // 該当なしの場合 npos 相当 (= m_Tracks.Size()) を返す。
    usize FindTrackForState(EMusicState state, f32 intensity) const noexcept;
    static f32 Clamp01(f32 v) noexcept;
    static void LogTodoOnce(const char* what) noexcept;

    // ----- track 倉庫 (フラット配列) -----
    TArray<MusicTrack> m_Tracks;
    // 各 state の m_Tracks 上の開始 index と個数 (SoA で state→range mapping)。
    u32 _state_first[kStateCount] {};
    u32 _state_count[kStateCount] {};

    // ----- 状態遷移 -----
    EMusicState m_CurrentState = EMusicState::Silent;
    EMusicState m_TargetState  = EMusicState::Silent;
    f32        m_TransitionDuration = 0.0f;
    f32        m_TransitionElapsed  = 0.0f;
    f32        m_TransitionProgress = 1.0f;  // [0, 1]。1 = 完了 / snap 済み

    // ----- intensity -----
    f32 m_Intensity = 0.0f;

    // ----- stinger (単一バッファ) -----
    const char* m_StingerPath    = nullptr;
    f32         m_StingerVolume  = 0.0f;
    bool        m_StingerPending = false;
};

} // namespace acs::game
