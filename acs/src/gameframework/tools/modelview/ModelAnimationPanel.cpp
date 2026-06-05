// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — ModelViewer / FModelAnimationPanel 実装
//
// 仕様の意図は FModelAnimationPanel.h を参照。本ファイルでは:
//   ・clip リストの SetClips / ClearClips (値コピー)
//   ・Play / Pause / Stop / SelectClip の状態遷移
//   ・Tick(dt) の時刻進行 + duration ラップ / Stopped 遷移 + callback 発火
//   ・DrawUI の ImGui レイアウト (ClipCombo / Time slider / Play/Pause/Stop /
//     Loop / Speed / BlendWeight)
// を実装する。すべて noexcept、STL 不使用。ImGui 依存は本 .cpp に閉じる。
#include "gameframework/tools/modelview/ModelAnimationPanel.h"

#include <imgui.h>

#include <cstdio>  // snprintf (clip combo の preview ラベル整形)

namespace acs::game::modelview {

namespace {

/**
 * f32 を [lo, hi] にクランプする。
 *
 * @details acs::Clamp を使わず局所定義することで本 .cpp の foundation ヘッダ依存を増やさない
 * (FEditorCamera.cpp の局所 ClampF と同方針)。
 * @param v クランプする値。
 * @param lo 下限。
 * @param hi 上限。
 * @return [lo, hi] に収めた値。
 */
inline f32 ClampF(f32 v, f32 lo, f32 hi) noexcept {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/**
 * i32 index と u32 count の境界チェックを行う。
 *
 * @param index 検査する index (負値は無効)。
 * @param count 要素数。
 * @return 0 <= index < count なら true。
 */
inline bool IsValidClipIndex(i32 index, u32 count) noexcept {
    if (index < 0) return false;
    return static_cast<u32>(index) < count;
}

/**
 * Combo 表示用に非 null の clip 名を返す。
 *
 * @details ImGui の Combo preview に nullptr を渡すと UB なので、nullptr / 空文字なら
 * "<unnamed>" を返す。
 * @param name 元の clip 名 (nullptr 可)。
 * @return 非 null の表示用文字列。
 */
inline const char* SafeClipName(const char* name) noexcept {
    if (name == nullptr || name[0] == '\0') return "<unnamed>";
    return name;
}

} // namespace

/** 内部 state を完全に初期値へ戻し、callback も解除する。 */
void FModelAnimationPanel::Init() noexcept {
    m_Clips.Clear();  // 中身は破棄、容量は保持 (= 次回再構築のアロケ節約)
    m_CurrentClipIdx = kNoClipSelected;
    _state            = EAnimationPlayState::Stopped;
    m_CurrentTimeSec = 0.0f;
    m_Speed            = 1.0f;
    m_LoopOverride    = false;
    m_BlendWeight     = 1.0f;
    m_OnFrameCb      = nullptr;
    m_OnFrameUser    = nullptr;
}

/** Init と同じ深さで内部 state を全 reset する (多重 Shutdown 安全)。 */
void FModelAnimationPanel::Shutdown() noexcept {
    // TArray は ~TArray で破棄されるが明示 Clear で
    // 多重 Shutdown / 再 Init の確定状態を作る (FParticleEditorPanel と同形)。
    m_Clips.Clear();
    m_CurrentClipIdx = kNoClipSelected;
    _state            = EAnimationPlayState::Stopped;
    m_CurrentTimeSec = 0.0f;
    m_Speed            = 1.0f;
    m_LoopOverride    = false;
    m_BlendWeight     = 1.0f;
    m_OnFrameCb      = nullptr;
    m_OnFrameUser    = nullptr;
}

/** clip メタ一覧を値コピーで取り込み、selection / time / state をリセットする。 */
void FModelAnimationPanel::SetClips(const FAnimationClipBinding* clips,
                                   u32 count) noexcept {
    // nullptr + count > 0 は呼出側ミスだが、defensive に「count = 0 として扱う」。
    // count == 0 / clips == nullptr ともに「空にする」セマンティクス。
    m_Clips.Clear();
    if (clips == nullptr || count == 0) {
        m_CurrentClipIdx = kNoClipSelected;
        _state            = EAnimationPlayState::Stopped;
        m_CurrentTimeSec = 0.0f;
        return;
    }

    // Resize で領域確保 + 中身を値コピー。FAnimationClipBinding は POD なので
    // 単純代入で安全。`name` ポインタの寿命は呼出側責任 (= 本 panel は
    // const char* リテラル / 永続バッファを前提とする)。
    m_Clips.Resize(static_cast<usize>(count));
    for (u32 i = 0; i < count; ++i) {
        m_Clips[i] = clips[i];
        // duration_sec が負 / NaN だと slider が壊れるので保険で 0 にクランプ。
        if (!(m_Clips[i].duration_sec >= 0.0f)) {
            m_Clips[i].duration_sec = 0.0f;
        }
    }

    // モデル切替の典型運用 = 「先頭 clip を自動選択 + Stopped から開始」。
    // 既存 selection を保持する設計だと、別モデルの違う index を見続けて
    // 別 clip が再生される事故が起きるため、明示的に reset する。
    m_CurrentClipIdx = 0;
    _state            = EAnimationPlayState::Stopped;
    m_CurrentTimeSec = 0.0f;
}

/** clip リストを空にして Stopped + time 0 に戻す (callback / 設定値は保持)。 */
void FModelAnimationPanel::ClearClips() noexcept {
    m_Clips.Clear();
    m_CurrentClipIdx = kNoClipSelected;
    _state            = EAnimationPlayState::Stopped;
    m_CurrentTimeSec = 0.0f;
    // callback / speed / loop_override / blend_weight は保持
    // (= Init / Shutdown より浅い reset、UI 設定は維持したい)。
}

/** 現在登録されている clip の数を返す。 */
u32 FModelAnimationPanel::ClipCount() const noexcept {
    return static_cast<u32>(m_Clips.Size());
}

/** 現在選択中の clip メタを返す (未選択 / 範囲外は nullptr)。 */
const FAnimationClipBinding* FModelAnimationPanel::CurrentClip() const noexcept {
    if (!IsValidClipIndex(m_CurrentClipIdx, static_cast<u32>(m_Clips.Size()))) {
        return nullptr;
    }
    return &m_Clips[static_cast<usize>(m_CurrentClipIdx)];
}

/** 現在選択中の clip index を返す (未選択は kNoClipSelected)。 */
i32 FModelAnimationPanel::CurrentClipIndex() const noexcept {
    return m_CurrentClipIdx;
}

/** clip を選択し、time 0 + Stopped にリセットする (範囲外は no-op)。 */
void FModelAnimationPanel::SelectClip(u32 clip_index) noexcept {
    // 範囲外 (>= ClipCount) は no-op (= 静かに無視、エラーは LogWarning 等で
    // 出すべきだが本 panel は log 依存を持たない方針)。
    if (clip_index >= static_cast<u32>(m_Clips.Size())) return;

    // 同 clip を再選択した場合も time = 0 + Stopped に戻す
    // (= UI 上で同じ clip を「もう一度選ぶ」操作は明確なリスタート意図と解釈)。
    m_CurrentClipIdx = static_cast<i32>(clip_index);
    m_CurrentTimeSec = 0.0f;
    _state            = EAnimationPlayState::Stopped;
}

/** 現在位置から再生開始する (Stopped からは頭出し、Paused からは継続)。 */
void FModelAnimationPanel::Play() noexcept {
    // clip 未選択 = 何も再生できない。silent no-op。
    if (m_CurrentClipIdx < 0) return;

    // Stopped → Playing 移行時のみ time = 0 (= 頭出し)。Paused → Playing
    // では time を保持して continue。Playing → Playing は副作用なし。
    if (_state == EAnimationPlayState::Stopped) {
        m_CurrentTimeSec = 0.0f;
    }
    _state = EAnimationPlayState::Playing;
}

/** Playing 中なら Paused に遷移する (それ以外は no-op)。 */
void FModelAnimationPanel::Pause() noexcept {
    // Playing 中のみ Paused に遷移。Stopped / Paused からの Pause は no-op
    // (= UX 上、停止中に Pause ボタンを押しても何も起こらないのが自然)。
    if (_state == EAnimationPlayState::Playing) {
        _state = EAnimationPlayState::Paused;
    }
}

/** どの状態からも Stopped + time 0 に遷移する (完全頭出し)。 */
void FModelAnimationPanel::Stop() noexcept {
    _state            = EAnimationPlayState::Stopped;
    m_CurrentTimeSec = 0.0f;
}

/** 現在の再生状態を返す。 */
EAnimationPlayState FModelAnimationPanel::PlayState() const noexcept {
    return _state;
}

/** 現在の再生位置 (秒) を返す。 */
f32 FModelAnimationPanel::CurrentTimeSec() const noexcept {
    return m_CurrentTimeSec;
}

/** 再生位置を [0, duration] にクランプして設定する (state は変えない)。 */
void FModelAnimationPanel::SetCurrentTimeSec(f32 t) noexcept {
    // clip 未選択 = 時刻も意味を持たない (= 0 のまま固定)。
    const FAnimationClipBinding* c = CurrentClip();
    if (c == nullptr) {
        m_CurrentTimeSec = 0.0f;
        return;
    }
    // duration が 0 の clip (= 静止ポーズ) は常に 0 に。負値も 0、duration
    // 超過も duration にクランプ。state は変更しない (= scrub 中の Play/Pause
    // 状態を保持)。
    const f32 dur = c->duration_sec;
    if (dur <= 0.0f) {
        m_CurrentTimeSec = 0.0f;
        return;
    }
    m_CurrentTimeSec = ClampF(t, 0.0f, dur);
}

/** 現在の再生速度 (倍率) を返す。 */
f32 FModelAnimationPanel::PlaybackSpeed() const noexcept {
    return m_Speed;
}

/** 再生速度を [0.1, 4.0] にクランプして設定する。 */
void FModelAnimationPanel::SetPlaybackSpeed(f32 speed) noexcept {
    // 仕様: [0.1, 4.0] にクランプ。負値も 0.1 へ正規化 (= 逆再生は将来対応)。
    m_Speed = ClampF(speed, kMinPlaybackSpeed, kMaxPlaybackSpeed);
}

/** Loop override が有効かを返す。 */
bool FModelAnimationPanel::IsLoopingOverride() const noexcept {
    return m_LoopOverride;
}

/** Loop override を設定する。 */
void FModelAnimationPanel::SetLoopingOverride(bool b) noexcept {
    m_LoopOverride = b;
}

/** blend weight を返す。 */
f32 FModelAnimationPanel::BlendWeight() const noexcept {
    return m_BlendWeight;
}

/** blend weight を [0, 1] にクランプして設定する。 */
void FModelAnimationPanel::SetBlendWeight(f32 w) noexcept {
    m_BlendWeight = ClampF(w, 0.0f, 1.0f);
}

/** Playing 中の時刻を進め、duration 到達処理 + callback 発火を行う。 */
void FModelAnimationPanel::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;
    if (_state != EAnimationPlayState::Playing) return;

    const FAnimationClipBinding* c = CurrentClip();
    if (c == nullptr) {
        // clip が消えた (= SetClips で空にされた) 場合は安全に Stopped に倒す。
        _state            = EAnimationPlayState::Stopped;
        m_CurrentTimeSec = 0.0f;
        return;
    }
    const f32 dur = c->duration_sec;
    if (dur <= 0.0f) {
        // duration 0 = 静止ポーズ。Playing でも時刻は進まないが、callback は
        // 発火する (= 呼出側に「現在 clip / time=0」を通知して bind pose を
        // 反映してもらう)。
        m_CurrentTimeSec = 0.0f;
        if (m_OnFrameCb != nullptr) {
            m_OnFrameCb(m_OnFrameUser, c->clip_index, m_CurrentTimeSec);
        }
        return;
    }

    // 時刻を進める。
    m_CurrentTimeSec += dt * m_Speed;

    // duration 到達処理。
    const bool looping = c->is_looping || m_LoopOverride;
    if (m_CurrentTimeSec >= dur) {
        if (looping) {
            // 余りで wrap。dt が極端に大きい (= duration 数倍) ケースも
            // fmodf 相当の繰り返し減算で吸収する。Mod を使わないのは
            // 「ヘッダから cmath を持ち込みたくない」 + dur > 0 が保証
            // されているので減算ループで十分速いため。
            // とはいえ pathological case の防御として、減算は count 上限を
            // 64 回に制限する (= dt * speed > dur*64 のケースは破棄)。
            i32 guard = 64;
            while (m_CurrentTimeSec >= dur && guard-- > 0) {
                m_CurrentTimeSec -= dur;
            }
            // guard 越えは「dt が duration の 64 倍以上 = フレーム落ち過剰」
            // とみなして time = 0 に倒す (= ロジック破綻防止)。
            if (m_CurrentTimeSec >= dur || m_CurrentTimeSec < 0.0f) {
                m_CurrentTimeSec = 0.0f;
            }
        } else {
            // 非 loop = 末尾に固定 + Stopped に遷移。
            m_CurrentTimeSec = dur;
            _state            = EAnimationPlayState::Stopped;
        }
    }

    // callback 発火 (= 外部 FAnimationPlayer に時刻反映)。
    if (m_OnFrameCb != nullptr) {
        m_OnFrameCb(m_OnFrameUser, c->clip_index, m_CurrentTimeSec);
    }
}

/** Tick 終端で呼ばれる callback と user ポインタを設定する。 */
void FModelAnimationPanel::SetOnFrameCallback(AnimationFrameCallback cb,
                                             void* user) noexcept {
    m_OnFrameCb   = cb;
    m_OnFrameUser = user;
}

/** ImGui window (ClipCombo / Time / Play-Pause-Stop / Loop / Speed / BlendWeight) を描画する。 */
void FModelAnimationPanel::DrawUI() noexcept {
    if (!IsVisible()) return;

    // Begin の戻り値で「window が collapsed / hidden」を判定し、それ以外
    // 早期 End。`&m_Visible` を渡すことで close ボタン (× 印) が `m_Visible` を
    // false にする (= FEditorPanel 規約)。
    if (!ImGui::Begin(Title(), &m_Visible)) {
        ImGui::End();
        return;
    }

    const u32 clip_count = static_cast<u32>(m_Clips.Size());

    // ClipCombo (dropdown)。
    // current preview 文字列: 未選択 / 空 list なら "(no clip)"、それ以外は
    // 選択中 clip 名。
    const char* preview = "(no clip)";
    if (clip_count > 0 && m_CurrentClipIdx >= 0
        && static_cast<u32>(m_CurrentClipIdx) < clip_count) {
        preview = SafeClipName(m_Clips[static_cast<usize>(m_CurrentClipIdx)].name);
    }

    if (ImGui::BeginCombo("Clip", preview)) {
        for (u32 i = 0; i < clip_count; ++i) {
            const bool is_selected = (static_cast<i32>(i) == m_CurrentClipIdx);
            const char* label      = SafeClipName(m_Clips[i].name);
            // PushID で同名 clip があっても ID 衝突しないようにする。
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label, is_selected)) {
                SelectClip(i);
            }
            // 既選択にスクロール / フォーカスを当てるヒント (UX 改善)。
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();

    // 現在 clip のメタ情報 + Time slider。
    const FAnimationClipBinding* cur = CurrentClip();
    if (cur == nullptr) {
        // clip 未選択時は disabled テキストで案内し、controls は disable する。
        ImGui::TextDisabled("(No clip selected)");
        ImGui::BeginDisabled();
    }

    // Time slider: 0 〜 duration (clip 未選択時は dur=1 のダミーで表示用)。
    {
        const f32 dur = (cur != nullptr && cur->duration_sec > 0.0f)
                            ? cur->duration_sec
                            : 1.0f;
        f32 t = m_CurrentTimeSec;
        // SliderFloat の戻り値で「ユーザが drag した」か判定。
        if (ImGui::SliderFloat("Time", &t, 0.0f, dur, "%.3f s")) {
            // SetCurrentTimeSec 経由で clamp + 安全反映 (= state は変えない)。
            SetCurrentTimeSec(t);
        }
    }

    // Play / Pause / Stop ボタン (3 個横並び)。
    {
        if (ImGui::Button("Play")) {
            Play();
        }
        ImGui::SameLine();
        if (ImGui::Button("Pause")) {
            Pause();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
            Stop();
        }

        // 状態テキストを横に小さく表示 (= 視覚フィードバック)。
        ImGui::SameLine();
        const char* state_str =
            (_state == EAnimationPlayState::Playing) ? "(Playing)" :
            (_state == EAnimationPlayState::Paused)  ? "(Paused)"  :
                                                       "(Stopped)";
        ImGui::TextDisabled("%s", state_str);
    }

    // Loop checkbox (override)。clip 既定 (is_looping) も小さく併記して、
    // ユーザに「override が現在の clip 設定とどう違うか」をわかりやすくする。
    {
        bool loop = m_LoopOverride;
        if (ImGui::Checkbox("Loop (override)", &loop)) {
            SetLoopingOverride(loop);
        }
        if (cur != nullptr) {
            ImGui::SameLine();
            ImGui::TextDisabled("(clip default: %s)",
                                cur->is_looping ? "loop" : "once");
        }
    }

    // Speed slider [0.1, 4.0]。logarithmic にすると 1x 付近の微調整がしやすい
    // が、線形で十分 (= 後で SliderFloat の flags に
    // ImGuiSliderFlags_Logarithmic を足せる)。
    {
        f32 s = m_Speed;
        if (ImGui::SliderFloat("Speed", &s,
                               kMinPlaybackSpeed, kMaxPlaybackSpeed,
                               "%.2fx")) {
            SetPlaybackSpeed(s);
        }
    }

    // BlendWeight slider [0, 1]。現状は表示用 (ヘッダ設計選択節参照)。
    {
        f32 w = m_BlendWeight;
        if (ImGui::SliderFloat("BlendWeight", &w, 0.0f, 1.0f, "%.2f")) {
            SetBlendWeight(w);
        }
    }

    if (cur == nullptr) {
        ImGui::EndDisabled();
    }

    ImGui::End();
}

} // namespace acs::game::modelview
