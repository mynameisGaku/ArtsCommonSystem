// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FMusicDirector 実装
//
// state machine + transition / stinger pending 管理を完全実装し、
// SetAudioDirector で結線された実 FAudioDirector へ BGM/SFX を delegate する。
// 未結線 (m_Audio == nullptr) のときは state-only で動作する (無音、crash しない)。
#include "gameframework/MusicDirector.h"
#include "gameframework/AudioDirector.h"
#include "foundation/Log.h"

namespace acs::game {

/** 値を [0, 1] にクランプする。 */
f32 FMusicDirector::Clamp01(f32 v) noexcept {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/** 現在 state / intensity に合う track を選び、結線済みなら BGM 再生へ流す。 */
void FMusicDirector::RouteCurrentTrackToAudio(f32 fade_in_sec) noexcept {
    if (m_Audio == nullptr) return;  // state-only 動作 (無音、crash しない)

    const usize idx = FindTrackForState(m_TargetState, m_Intensity);
    if (idx >= m_Tracks.Size()) {
        // 対象 state に track 未登録 → 鳴らすものが無いので BGM 停止。
        m_Audio->StopBgm(fade_in_sec);
        return;
    }
    const MusicTrack& t = m_Tracks[idx];
    m_Audio->PlayBgm(t.asset_path, fade_in_sec, t.loop);
}

/** track 配列を事前確保し、各 state の index を 0 で初期化する。 */
FMusicDirector::FMusicDirector() noexcept {
    // 想定登録数で track 配列を事前確保 (拡張時の reallocation を抑える)。
    m_Tracks.Reserve(kTrackReserveHint);
    for (u32 i = 0; i < kStateCount; ++i) {
        _state_first[i] = 0;
        _state_count[i] = 0;
    }
}

/** 指定 state の連続区間末尾に track を挿入し、state インデックスを更新する。 */
void FMusicDirector::RegisterTrack(EMusicState state, const MusicTrack& track) noexcept {
    if (track.asset_path == nullptr) {
        ACS_LOG_WARN("FMusicDirector::RegisterTrack: asset_path=nullptr → ignored");
        return;
    }
    const u32 state_idx = static_cast<u32>(state);
    if (state_idx >= kStateCount) {
        ACS_LOG_WARN("FMusicDirector::RegisterTrack: invalid state=%u → ignored", state_idx);
        return;
    }

    // intensity range を [0, 1] にクランプし、min <= max を保証する。
    MusicTrack normalized = track;
    normalized.intensity_min = Clamp01(track.intensity_min);
    normalized.intensity_max = Clamp01(track.intensity_max);
    if (normalized.intensity_min > normalized.intensity_max) {
        ACS_LOG_WARN("FMusicDirector::RegisterTrack: intensity_min(%.3f) > max(%.3f) → swapped",
                     normalized.intensity_min, normalized.intensity_max);
        const f32 tmp = normalized.intensity_min;
        normalized.intensity_min = normalized.intensity_max;
        normalized.intensity_max = tmp;
    }

    // 該当 state の末尾位置に挿入するため、それより後ろの track を 1 つずつ後方シフト。
    // (state ごとに連続区間を維持する SoA 戦略)
    const u32 insert_at = _state_first[state_idx] + _state_count[state_idx];
    m_Tracks.PushBack(MusicTrack{});  // 末尾に空き枠を確保
    for (usize i = m_Tracks.Size() - 1; i > insert_at; --i) {
        m_Tracks[i] = m_Tracks[i - 1];
    }
    m_Tracks[insert_at] = normalized;
    _state_count[state_idx] += 1;

    // 自身より後ろの state は first を 1 つずつ繰り下げる。
    for (u32 s = state_idx + 1; s < kStateCount; ++s) {
        _state_first[s] += 1;
    }
}

/** state インデックスの再構築 hook (現状は逐次更新のため空)。 */
void FMusicDirector::RebuildStateIndex() noexcept {
    // 現状は RegisterTrack 内で逐次更新しており、明示再構築は不要。
    // 将来 UnregisterTrack を導入する際の hook 用に空関数として残す。
}

/** 指定 state 内で intensity に合う track index を返す (なければ fallback)。 */
usize FMusicDirector::FindTrackForState(EMusicState state, f32 intensity) const noexcept {
    const u32 state_idx = static_cast<u32>(state);
    if (state_idx >= kStateCount) return m_Tracks.Size();
    const u32 first = _state_first[state_idx];
    const u32 count = _state_count[state_idx];
    if (count == 0) return m_Tracks.Size();

    // 1) intensity 範囲ヒットを線形探索 (count は通常 1..4 個程度)。
    usize fallback = m_Tracks.Size();
    f32   fallback_max = -1.0f;
    for (u32 i = 0; i < count; ++i) {
        const usize idx = static_cast<usize>(first + i);
        const MusicTrack& t = m_Tracks[idx];
        if (intensity >= t.intensity_min && intensity <= t.intensity_max) {
            return idx;
        }
        // 範囲外でも「最も上限の高い track」を fallback として記憶。
        if (t.intensity_max > fallback_max) {
            fallback_max = t.intensity_max;
            fallback     = idx;
        }
    }
    // 2) どれも範囲ヒットせず → fallback (= 最強 intensity 帯の track) を返す。
    return fallback;
}

/** current → state へクロスフェード開始 (即時切替・同一 state 再要求を処理)。 */
void FMusicDirector::SetState(EMusicState state, f32 transition_sec) noexcept {
    const u32 state_idx = static_cast<u32>(state);
    if (state_idx >= kStateCount) {
        ACS_LOG_WARN("FMusicDirector::SetState: invalid state=%u → ignored", state_idx);
        return;
    }

    // 同一 state 再要求: no-op (現行を継続)。遷移中ならそれも継続。
    if (state == m_TargetState && !IsTransitioning()) {
        return;
    }

    if (transition_sec <= 0.0f) {
        // 即時切替: current / target ともに即合わせ、progress=1 にスナップ。
        m_CurrentState        = state;
        m_TargetState         = state;
        m_TransitionDuration  = 0.0f;
        m_TransitionElapsed   = 0.0f;
        m_TransitionProgress  = 1.0f;
        // 結線済みなら新 state の track を即時 BGM 再生 (fade=0)。
        RouteCurrentTrackToAudio(0.0f);
        return;
    }

    // クロスフェード開始: current は据え置き、target を新 state に。
    // 既に遷移中だった場合は「現在の current を起点に、新 target へ」再遷移。
    // (= 中断して新方向に切り替える。前の target は破棄される)
    m_TargetState         = state;
    m_TransitionDuration  = transition_sec;
    m_TransitionElapsed   = 0.0f;
    m_TransitionProgress  = 0.0f;
    // 結線済みなら新 target の track を同じ fade 長で BGM クロスフェード開始。
    // (FAudioDirector 側が実際のゲイン補間を行うため、本層 Tick での progress
    //  進行とは独立に低レイヤで滑らかに遷移する)
    RouteCurrentTrackToAudio(transition_sec);
}

/** current != target かつ progress < 1 なら true。 */
bool FMusicDirector::IsTransitioning() const noexcept {
    return m_CurrentState != m_TargetState && m_TransitionProgress < 1.0f;
}

/** intensity を [0, 1] にクランプして設定する (範囲外は警告)。 */
void FMusicDirector::SetIntensity(f32 intensity_0_to_1) noexcept {
    const f32 c = Clamp01(intensity_0_to_1);
    if (c != intensity_0_to_1) {
        ACS_LOG_WARN("FMusicDirector::SetIntensity: out-of-range %.3f → clamped to %.3f",
                     intensity_0_to_1, c);
    }
    m_Intensity = c;
}

/** pending stinger を更新し、結線済みなら即時に SFX を重ね再生する。 */
void FMusicDirector::PlayStinger(const char* asset_path, f32 volume) noexcept {
    if (asset_path == nullptr) {
        ACS_LOG_WARN("FMusicDirector::PlayStinger: asset_path=nullptr → ignored");
        return;
    }
    if (volume <= 0.0f) {
        ACS_LOG_WARN("FMusicDirector::PlayStinger: volume=%.3f <= 0 → ignored", volume);
        return;
    }
    if (m_StingerPending) {
        // 既存 pending を上書き (最新採用ポリシー、スタックしない)。
        ACS_LOG_WARN("FMusicDirector::PlayStinger: pending stinger overwritten (%s → %s)",
                     m_StingerPath, asset_path);
    }
    // pending 情報は ConsumeStinger 用に常に保持 (結線有無に依らず)。
    m_StingerPath    = asset_path;
    m_StingerVolume  = volume;
    m_StingerPending = true;

    // 結線済みなら BGM を停めずに一発 SFX として即時重ね再生する。
    // (PlaySfx は混ぜて並列発火 = BGM クロスフェードに干渉しない)
    if (m_Audio != nullptr) {
        m_Audio->PlaySfx(asset_path, volume);
    }
}

/** 保持中の stinger を取り出し、pending=false に戻す。 */
const char* FMusicDirector::ConsumeStinger(f32& out_volume) noexcept {
    if (!m_StingerPending) {
        out_volume = 0.0f;
        return nullptr;
    }
    const char* path = m_StingerPath;
    out_volume       = m_StingerVolume;
    m_StingerPending = false;
    m_StingerPath    = nullptr;
    m_StingerVolume  = 0.0f;
    return path;
}

/** クロスフェードの進捗を進め、完了したら current を target に snap する。 */
void FMusicDirector::Tick(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;

    // 実 BGM/SFX のゲイン補間・クロスフェード進行は FAudioDirector 側で行われる。
    // 本層は state machine (current↔target の遷移進捗) のみを進める。
    // FAudioDirector::Tick は所有者 (FGame / app) が直接呼ぶ規約のため、ここから
    // 二重に forward はしない (timer の二重進行を避ける)。

    // クロスフェード進行。current == target なら何もしない (定常状態)。
    if (m_CurrentState != m_TargetState) {
        if (m_TransitionDuration <= 0.0f) {
            // 不正状態 (duration=0 で current != target) → 即 snap して防御。
            m_CurrentState       = m_TargetState;
            m_TransitionProgress = 1.0f;
            return;
        }
        m_TransitionElapsed += dt;
        const f32 t = m_TransitionElapsed / m_TransitionDuration;
        if (t >= 1.0f) {
            // 遷移完了: current を target に snap。
            m_CurrentState       = m_TargetState;
            m_TransitionProgress = 1.0f;
            m_TransitionElapsed  = 0.0f;
            m_TransitionDuration = 0.0f;
        } else {
            m_TransitionProgress = t;
        }
    }
}

/** state を Silent に即時切替し、stinger を破棄して実 BGM も停止する。 */
void FMusicDirector::Stop() noexcept {
    m_CurrentState        = EMusicState::Silent;
    m_TargetState         = EMusicState::Silent;
    m_TransitionDuration  = 0.0f;
    m_TransitionElapsed   = 0.0f;
    m_TransitionProgress  = 1.0f;
    // stinger pending も破棄 (一律停止のため)。
    m_StingerPending = false;
    m_StingerPath    = nullptr;
    m_StingerVolume  = 0.0f;
    // intensity は保持 (next state 開始時の体験を断絶させない)。

    // 結線済みなら実 BGM も即時停止 (fade=0、一律停止のため)。
    if (m_Audio != nullptr) {
        m_Audio->StopBgm(0.0f);
    }
}

/** 現在 state で intensity に合致する track を返す。 */
const MusicTrack* FMusicDirector::CurrentTrack() const noexcept {
    const usize idx = FindTrackForState(m_CurrentState, m_Intensity);
    if (idx >= m_Tracks.Size()) return nullptr;
    return &m_Tracks[idx];
}

/** 遷移先 state 側の track を返す (遷移していなければ nullptr)。 */
const MusicTrack* FMusicDirector::TargetTrack() const noexcept {
    if (m_CurrentState == m_TargetState) return nullptr;
    const usize idx = FindTrackForState(m_TargetState, m_Intensity);
    if (idx >= m_Tracks.Size()) return nullptr;
    return &m_Tracks[idx];
}

} // namespace acs::game
