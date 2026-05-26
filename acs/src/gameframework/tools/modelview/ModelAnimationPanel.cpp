// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — ModelViewer / FModelAnimationPanel 実装 (Phase 21b)
//
// 仕様の意図は ModelAnimationPanel.h を参照。本ファイルでは:
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

// =============================================================================
// 内部ヘルパ
// =============================================================================
namespace {

// f32 を [lo, hi] にクランプ。`acs::Clamp` (foundation/Move.h) を使わずに
// 局所定義しているのは、本 .cpp が他の foundation ヘッダ依存を増やさず
// 自己完結するため (= EditorCamera.cpp が同形の局所 ClampF を持つのと同方針)。
inline f32 ClampF(f32 v, f32 lo, f32 hi) noexcept {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// i32 index と u32 count の境界チェック。負値 / 範囲外を弾く。
// FParticleEditorPanel と同形 (= editor 系 panel 共通の小ヘルパ)。
inline bool IsValidClipIndex(i32 index, u32 count) noexcept {
    if (index < 0) return false;
    return static_cast<u32>(index) < count;
}

// clip 名が nullptr or 空文字の場合に Combo の表示用に "<unnamed>" を返す。
// ImGui の Combo preview に nullptr を渡すと UB なので必ず非 null 文字列を
// 渡す (= PropertyDrawer.cpp の SafeLabel と同方針)。
inline const char* SafeClipName(const char* name) noexcept {
    if (name == nullptr || name[0] == '\0') return "<unnamed>";
    return name;
}

} // namespace

// =============================================================================
// Init / Shutdown
// =============================================================================
// Init は state を完全に初期値へ戻す (= 多重 Init を完全リセットとして扱う)。
// callback も解除する (= Shutdown と同じ深さの reset)。
// =============================================================================
void FModelAnimationPanel::Init() noexcept {
    _clips.Clear();  // 中身は破棄、容量は保持 (= 次回再構築のアロケ節約)
    _current_clip_idx = kNoClipSelected;
    _state            = EAnimationPlayState::Stopped;
    _current_time_sec = 0.0f;
    _speed            = 1.0f;
    _loop_override    = false;
    _blend_weight     = 1.0f;
    _on_frame_cb      = nullptr;
    _on_frame_user    = nullptr;
}

void FModelAnimationPanel::Shutdown() noexcept {
    // Init と同じ深さの reset。TArray は ~TArray で破棄されるが明示 Clear で
    // 多重 Shutdown / 再 Init の確定状態を作る (FParticleEditorPanel と同形)。
    _clips.Clear();
    _current_clip_idx = kNoClipSelected;
    _state            = EAnimationPlayState::Stopped;
    _current_time_sec = 0.0f;
    _speed            = 1.0f;
    _loop_override    = false;
    _blend_weight     = 1.0f;
    _on_frame_cb      = nullptr;
    _on_frame_user    = nullptr;
}

// =============================================================================
// clip リスト管理
// =============================================================================
void FModelAnimationPanel::SetClips(const FAnimationClipBinding* clips,
                                   u32 count) noexcept {
    // nullptr + count > 0 は呼出側ミスだが、defensive に「count = 0 として扱う」。
    // count == 0 / clips == nullptr ともに「空にする」セマンティクス。
    _clips.Clear();
    if (clips == nullptr || count == 0) {
        _current_clip_idx = kNoClipSelected;
        _state            = EAnimationPlayState::Stopped;
        _current_time_sec = 0.0f;
        return;
    }

    // Resize で領域確保 + 中身を値コピー。FAnimationClipBinding は POD なので
    // 単純代入で安全。`name` ポインタの寿命は呼出側責任 (= 本 panel は
    // const char* リテラル / 永続バッファを前提とする)。
    _clips.Resize(static_cast<usize>(count));
    for (u32 i = 0; i < count; ++i) {
        _clips[i] = clips[i];
        // duration_sec が負 / NaN だと slider が壊れるので保険で 0 にクランプ。
        if (!(_clips[i].duration_sec >= 0.0f)) {
            _clips[i].duration_sec = 0.0f;
        }
    }

    // モデル切替の典型運用 = 「先頭 clip を自動選択 + Stopped から開始」。
    // 既存 selection を保持する設計だと、別モデルの違う index を見続けて
    // 別 clip が再生される事故が起きるため、明示的に reset する。
    _current_clip_idx = 0;
    _state            = EAnimationPlayState::Stopped;
    _current_time_sec = 0.0f;
}

void FModelAnimationPanel::ClearClips() noexcept {
    _clips.Clear();
    _current_clip_idx = kNoClipSelected;
    _state            = EAnimationPlayState::Stopped;
    _current_time_sec = 0.0f;
    // callback / speed / loop_override / blend_weight は保持
    // (= Init / Shutdown より浅い reset、UI 設定は維持したい)。
}

u32 FModelAnimationPanel::ClipCount() const noexcept {
    return static_cast<u32>(_clips.Size());
}

const FAnimationClipBinding* FModelAnimationPanel::CurrentClip() const noexcept {
    if (!IsValidClipIndex(_current_clip_idx, static_cast<u32>(_clips.Size()))) {
        return nullptr;
    }
    return &_clips[static_cast<usize>(_current_clip_idx)];
}

i32 FModelAnimationPanel::CurrentClipIndex() const noexcept {
    return _current_clip_idx;
}

void FModelAnimationPanel::SelectClip(u32 clip_index) noexcept {
    // 範囲外 (>= ClipCount) は no-op (= 静かに無視、エラーは LogWarning 等で
    // 出すべきだが本 panel は log 依存を持たない方針)。
    if (clip_index >= static_cast<u32>(_clips.Size())) return;

    // 同 clip を再選択した場合も time = 0 + Stopped に戻す
    // (= UI 上で同じ clip を「もう一度選ぶ」操作は明確なリスタート意図と解釈)。
    _current_clip_idx = static_cast<i32>(clip_index);
    _current_time_sec = 0.0f;
    _state            = EAnimationPlayState::Stopped;
}

// =============================================================================
// 再生制御
// =============================================================================
void FModelAnimationPanel::Play() noexcept {
    // clip 未選択 = 何も再生できない。silent no-op。
    if (_current_clip_idx < 0) return;

    // Stopped → Playing 移行時のみ time = 0 (= 頭出し)。Paused → Playing
    // では time を保持して continue。Playing → Playing は副作用なし。
    if (_state == EAnimationPlayState::Stopped) {
        _current_time_sec = 0.0f;
    }
    _state = EAnimationPlayState::Playing;
}

void FModelAnimationPanel::Pause() noexcept {
    // Playing 中のみ Paused に遷移。Stopped / Paused からの Pause は no-op
    // (= UX 上、停止中に Pause ボタンを押しても何も起こらないのが自然)。
    if (_state == EAnimationPlayState::Playing) {
        _state = EAnimationPlayState::Paused;
    }
}

void FModelAnimationPanel::Stop() noexcept {
    // どの状態からも Stopped + time = 0 に遷移 (= 完全頭出し)。
    _state            = EAnimationPlayState::Stopped;
    _current_time_sec = 0.0f;
}

EAnimationPlayState FModelAnimationPanel::PlayState() const noexcept {
    return _state;
}

// =============================================================================
// 時刻 / 速度 / Loop / Blend
// =============================================================================
f32 FModelAnimationPanel::CurrentTimeSec() const noexcept {
    return _current_time_sec;
}

void FModelAnimationPanel::SetCurrentTimeSec(f32 t) noexcept {
    // clip 未選択 = 時刻も意味を持たない (= 0 のまま固定)。
    const FAnimationClipBinding* c = CurrentClip();
    if (c == nullptr) {
        _current_time_sec = 0.0f;
        return;
    }
    // duration が 0 の clip (= 静止ポーズ) は常に 0 に。負値も 0、duration
    // 超過も duration にクランプ。state は変更しない (= scrub 中の Play/Pause
    // 状態を保持)。
    const f32 dur = c->duration_sec;
    if (dur <= 0.0f) {
        _current_time_sec = 0.0f;
        return;
    }
    _current_time_sec = ClampF(t, 0.0f, dur);
}

f32 FModelAnimationPanel::PlaybackSpeed() const noexcept {
    return _speed;
}

void FModelAnimationPanel::SetPlaybackSpeed(f32 speed) noexcept {
    // 仕様: [0.1, 4.0] にクランプ。負値も 0.1 へ正規化 (= 逆再生は将来対応)。
    _speed = ClampF(speed, kMinPlaybackSpeed, kMaxPlaybackSpeed);
}

bool FModelAnimationPanel::IsLoopingOverride() const noexcept {
    return _loop_override;
}

void FModelAnimationPanel::SetLoopingOverride(bool b) noexcept {
    _loop_override = b;
}

f32 FModelAnimationPanel::BlendWeight() const noexcept {
    return _blend_weight;
}

void FModelAnimationPanel::SetBlendWeight(f32 w) noexcept {
    _blend_weight = ClampF(w, 0.0f, 1.0f);
}

// =============================================================================
// Tick — 時刻進行 + duration 到達処理 + callback 発火
// =============================================================================
// Playing 中のみ `_current_time += dt * _speed` を進め、duration を超えたら:
//   ・loop (= clip.is_looping || _loop_override) → 余りで wrap
//   ・loop でない                                → Stopped + time = duration
// 終端で _on_frame_cb (設定されていれば) を 1 度発火する。
//
// dt <= 0 は no-op (= 巻き戻し非対応、FSpriteAnimator と同方針)。
// clip 未選択 / Stopped / Paused / duration <= 0 はすべて no-op
// (= callback も発火しない、無駄な GPU 反映を呼び出し側に渡さない)。
// =============================================================================
void FModelAnimationPanel::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;
    if (_state != EAnimationPlayState::Playing) return;

    const FAnimationClipBinding* c = CurrentClip();
    if (c == nullptr) {
        // clip が消えた (= SetClips で空にされた) 場合は安全に Stopped に倒す。
        _state            = EAnimationPlayState::Stopped;
        _current_time_sec = 0.0f;
        return;
    }
    const f32 dur = c->duration_sec;
    if (dur <= 0.0f) {
        // duration 0 = 静止ポーズ。Playing でも時刻は進まないが、callback は
        // 発火する (= 呼出側に「現在 clip / time=0」を通知して bind pose を
        // 反映してもらう)。
        _current_time_sec = 0.0f;
        if (_on_frame_cb != nullptr) {
            _on_frame_cb(_on_frame_user, c->clip_index, _current_time_sec);
        }
        return;
    }

    // 時刻を進める。
    _current_time_sec += dt * _speed;

    // duration 到達処理。
    const bool looping = c->is_looping || _loop_override;
    if (_current_time_sec >= dur) {
        if (looping) {
            // 余りで wrap。dt が極端に大きい (= duration 数倍) ケースも
            // fmodf 相当の繰り返し減算で吸収する。Mod を使わないのは
            // 「ヘッダから cmath を持ち込みたくない」 + dur > 0 が保証
            // されているので減算ループで十分速いため。
            // とはいえ pathological case の防御として、減算は count 上限を
            // 64 回に制限する (= dt * speed > dur*64 のケースは破棄)。
            i32 guard = 64;
            while (_current_time_sec >= dur && guard-- > 0) {
                _current_time_sec -= dur;
            }
            // guard 越えは「dt が duration の 64 倍以上 = フレーム落ち過剰」
            // とみなして time = 0 に倒す (= ロジック破綻防止)。
            if (_current_time_sec >= dur || _current_time_sec < 0.0f) {
                _current_time_sec = 0.0f;
            }
        } else {
            // 非 loop = 末尾に固定 + Stopped に遷移。
            _current_time_sec = dur;
            _state            = EAnimationPlayState::Stopped;
        }
    }

    // callback 発火 (= 外部 FAnimationPlayer に時刻反映)。
    if (_on_frame_cb != nullptr) {
        _on_frame_cb(_on_frame_user, c->clip_index, _current_time_sec);
    }
}

void FModelAnimationPanel::SetOnFrameCallback(AnimationFrameCallback cb,
                                             void* user) noexcept {
    _on_frame_cb   = cb;
    _on_frame_user = user;
}

// =============================================================================
// DrawUI — メイン ImGui window
// =============================================================================
// レイアウト (単一 "FAnimation" window 内):
//   ClipCombo (dropdown)
//   ────────────
//   Time slider [0, duration]
//   [Play] [Pause] [Stop]    (3 ボタン横並び)
//   [x] Loop (override)
//   Speed slider [0.1, 4.0]
//   BlendWeight slider [0, 1]
// =============================================================================
void FModelAnimationPanel::DrawUI() noexcept {
    if (!IsVisible()) return;

    // Begin の戻り値で「window が collapsed / hidden」を判定し、それ以外
    // 早期 End。`&_visible` を渡すことで close ボタン (× 印) が `_visible` を
    // false にする (= FEditorPanel 規約)。
    if (!ImGui::Begin(Title(), &_visible)) {
        ImGui::End();
        return;
    }

    const u32 clip_count = static_cast<u32>(_clips.Size());

    // ----- ClipCombo (dropdown) ------------------------------------------
    // current preview 文字列: 未選択 / 空 list なら "(no clip)"、それ以外は
    // 選択中 clip 名。
    const char* preview = "(no clip)";
    if (clip_count > 0 && _current_clip_idx >= 0
        && static_cast<u32>(_current_clip_idx) < clip_count) {
        preview = SafeClipName(_clips[static_cast<usize>(_current_clip_idx)].name);
    }

    if (ImGui::BeginCombo("Clip", preview)) {
        for (u32 i = 0; i < clip_count; ++i) {
            const bool is_selected = (static_cast<i32>(i) == _current_clip_idx);
            const char* label      = SafeClipName(_clips[i].name);
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

    // ----- 現在 clip のメタ情報 + Time slider ------------------------------
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
        f32 t = _current_time_sec;
        // SliderFloat の戻り値で「ユーザが drag した」か判定。
        if (ImGui::SliderFloat("Time", &t, 0.0f, dur, "%.3f s")) {
            // SetCurrentTimeSec 経由で clamp + 安全反映 (= state は変えない)。
            SetCurrentTimeSec(t);
        }
    }

    // Play / Pause / Stop ボタン (3 個横並び)。
    {
        if (ImGui::FButton("Play")) {
            Play();
        }
        ImGui::SameLine();
        if (ImGui::FButton("Pause")) {
            Pause();
        }
        ImGui::SameLine();
        if (ImGui::FButton("Stop")) {
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
        bool loop = _loop_override;
        if (ImGui::FCheckbox("Loop (override)", &loop)) {
            SetLoopingOverride(loop);
        }
        if (cur != nullptr) {
            ImGui::SameLine();
            ImGui::TextDisabled("(clip default: %s)",
                                cur->is_looping ? "loop" : "once");
        }
    }

    // Speed slider [0.1, 4.0]。logarithmic にすると 1x 付近の微調整がしやすい
    // が、Phase 21b は線形で十分 (= 後で SliderFloat の flags に
    // ImGuiSliderFlags_Logarithmic を足せる)。
    {
        f32 s = _speed;
        if (ImGui::SliderFloat("Speed", &s,
                               kMinPlaybackSpeed, kMaxPlaybackSpeed,
                               "%.2fx")) {
            SetPlaybackSpeed(s);
        }
    }

    // BlendWeight slider [0, 1]。Phase 21b は表示用、Phase 22+ で複数 clip
    // blending の primary slot として再利用する予定 (ヘッダ設計選択節参照)。
    {
        f32 w = _blend_weight;
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
