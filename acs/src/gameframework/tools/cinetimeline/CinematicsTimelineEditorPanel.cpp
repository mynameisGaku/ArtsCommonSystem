// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — cinetimeline / ACinematicsTimelineEditorPanel 実装
//
// 仕様の意図は ACinematicsTimelineEditorPanel.h を参照。本ファイルでは:
//   ・Init / Shutdown / SetCinematicsDirector / Play / Pause / Stop / Step:
//     状態管理 + director への bake / proxy
//   ・SetCurrentTimeSec / SetDurationSec / SelectKeyframe / AddKeyframe /
//     RemoveSelectedKeyframe: 内部 storage 編集 + clamp
//   ・DrawUI: ImGui で
//       - Toolbar (Play / Pause / Stop / Step / Time slider / Add combo + button)
//       - Timeline canvas (InvisibleButton + DrawList + 5 row 横並び marker)
//       - Inspector pane (kind / time / kind 別 payload float drag)
//       - Ruler (1s ごとの目盛 + 現在カーソル縦線)
// すべて noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。
#include "gameframework/tools/cinetimeline/CinematicsTimelineEditorPanel.h"

#include "gameframework/CinematicsDirector.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>   // std::snprintf (marker label 整形)

namespace acs::game::cinetimeline {

/**
 * f32 を任意レンジ [lo, hi] にクランプする。
 *
 * @param v クランプ対象の値。
 * @param lo 下限。
 * @param hi 上限。
 * @return [lo, hi] に収めた値。
 */
static f32 ClampF(f32 v, f32 lo, f32 hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

/**
 * editor payload を runtime event_id に可逆エンコードするためのタグとマスク。
 *
 * @details
 * runtime 側 FTimelineKeyframe の FireEvent payload は u32 event_id 1 個しか
 * 持たないため、editor のリッチな実値 (色 / scale / effect_id) を event_id の
 * 32bit に詰める。bit 31..24 が kind タグ (kEventTag*)、bit 23..0 が kind 別
 * ペイロード (FadeColor=8bit RGB、TimeScale=24bit 固定小数、SpawnEffect/
 * TriggerCallback=24bit の id)。caller はタグで分岐し下位 24bit を復号する。
 * FadeColor の start_color のみ 32bit に収まらず載らない (runtime 契約の上限)。
 */
enum : u32 {
    /** FadeColor の kind タグ (下位 24bit = end_color の RGB)。 */
    kEventTagFadeColor       = 0x01u << 24,

    /** TimeScale の kind タグ (下位 24bit = scale の固定小数)。 */
    kEventTagTimeScale       = 0x02u << 24,

    /** SpawnEffect の kind タグ (下位 24bit = effect_id)。 */
    kEventTagSpawnEffect     = 0x03u << 24,

    /** TriggerCallback の kind タグ (下位 24bit = event_id)。 */
    kEventTagTriggerCallback = 0x04u << 24,

    /** ペイロード部 (下位 24bit) を取り出すマスク。 */
    kEventPayloadMask        = 0x00FFFFFFu,
};

/**
 * f32 チャンネル [0,1] を 8bit (0..255) に量子化する。
 *
 * @param v 量子化対象の値 (範囲外は [0,1] にクランプ)。
 * @return 量子化した 8bit 値 (0..255)。
 */
static u32 QuantizeUnit8(f32 v) noexcept {
    const f32 c = ClampF(v, 0.0f, 1.0f);
    i32 q = static_cast<i32>(c * 255.0f + 0.5f);
    if (q < 0)   q = 0;
    if (q > 255) q = 255;
    return static_cast<u32>(q);
}

/**
 * FVec3 色 (r,g,b in [0,1]) を 0x00RRGGBB (24bit) にエンコードする。
 *
 * @param c エンコードする色。
 * @return 0x00RRGGBB 形式の 24bit 値。
 */
static u32 EncodeColorRGB(const FVec3& c) noexcept {
    return (QuantizeUnit8(c.x) << 16)
         | (QuantizeUnit8(c.y) << 8)
         | (QuantizeUnit8(c.z));
}

/** time_scale 固定小数化の分母 (2^21)。caller は (raw & mask) / この値で復号する。 */
static constexpr f32 kTimeScaleFixedDenom = 2097152.0f;  // 2^21

/**
 * time_scale [0,8] を 24bit 固定小数にエンコードする。
 *
 * @details 1/(2^21) 刻みで、編集 step 0.01 を十分上回る精度を持つ。念のため 24bit に飽和する。
 * @param s エンコードする時間スケール (範囲外は [0,8] にクランプ)。
 * @return 下位 24bit に収めた固定小数値 (kEventTagTimeScale と OR して使う)。
 */
static u32 EncodeTimeScale(f32 s) noexcept {
    const f32 c = ClampF(s, 0.0f, 8.0f);  // inspector DragFloat と同レンジ
    u32 q = static_cast<u32>(c * kTimeScaleFixedDenom + 0.5f);
    if (q > kEventPayloadMask) q = kEventPayloadMask;  // 念のため 24bit に飽和
    return q;
}

/**
 * ETimelineKeyKind の表示名を返す (toolbar combo / inspector / marker label 用)。
 *
 * @param k 表示名を求める kind。
 * @return kind の英字名 (未知の値は "?")。
 */
static const char* KindName(ETimelineKeyKind k) noexcept {
    switch (k) {
    case ETimelineKeyKind::CameraCut:       return "CameraCut";
    case ETimelineKeyKind::FadeColor:       return "FadeColor";
    case ETimelineKeyKind::TimeScale:       return "TimeScale";
    case ETimelineKeyKind::SpawnEffect:     return "SpawnEffect";
    case ETimelineKeyKind::TriggerCallback: return "TriggerCallback";
    }
    return "?";
}

/**
 * kind ごとの marker 塗り色を返す。
 *
 * @details 視認性のため意味のある色を割り当てる (camera=cyan / fade=yellow / time=magenta / spawn=green / trigger=orange)。
 * @param k 色を求める kind。
 * @return marker の塗り色 (ImU32)。未知の値はグレー。
 */
static ImU32 KindColor(ETimelineKeyKind k) noexcept {
    switch (k) {
    case ETimelineKeyKind::CameraCut:       return IM_COL32( 80, 200, 255, 255);
    case ETimelineKeyKind::FadeColor:       return IM_COL32(255, 220, 100, 255);
    case ETimelineKeyKind::TimeScale:       return IM_COL32(220, 100, 220, 255);
    case ETimelineKeyKind::SpawnEffect:     return IM_COL32(100, 220, 120, 255);
    case ETimelineKeyKind::TriggerCallback: return IM_COL32(255, 150,  80, 255);
    }
    return IM_COL32(200, 200, 200, 255);
}

/**
 * editor kind を director runtime kind にマッピングする。
 *
 * @details
 * editor は 5 種 (CameraCut / FadeColor / TimeScale / SpawnEffect /
 * TriggerCallback)、runtime は {Wait / MoveCamera / ShowDialogue / PlayMusic /
 * FireEvent} の 5 種で意味が異なる。CameraCut は MoveCamera に、その他 4 種は
 * すべて FireEvent にマッピングする。FireEvent 系の event_id には kind タグと
 * 実ペイロードが詰められる (BakeToDirector / Encode* ヘルパ参照)。
 * @param k マッピングする editor kind。
 * @return 対応する runtime の ETimelineTrackKind (未知の値は Wait)。
 */
static ETimelineTrackKind ToTrackKind(ETimelineKeyKind k) noexcept {
    switch (k) {
    case ETimelineKeyKind::CameraCut:       return ETimelineTrackKind::MoveCamera;
    case ETimelineKeyKind::FadeColor:       return ETimelineTrackKind::FireEvent;
    case ETimelineKeyKind::TimeScale:       return ETimelineTrackKind::FireEvent;
    case ETimelineKeyKind::SpawnEffect:     return ETimelineTrackKind::FireEvent;
    case ETimelineKeyKind::TriggerCallback: return ETimelineTrackKind::FireEvent;
    }
    return ETimelineTrackKind::Wait;
}

/**
 * time [0, Duration] を canvas x 座標 (px) に変換する。
 *
 * @param t 変換する時刻 [秒]。
 * @param duration タイムライン全体の長さ [秒]。
 * @param canvas_x canvas 左端の x 座標 (px)。
 * @param canvas_w canvas の幅 (px)。
 * @return 対応する canvas 内 x 座標 (px)。duration <= 0 なら canvas_x。
 */
static f32 TimeToCanvasX(f32 t, f32 duration,
                         f32 canvas_x, f32 canvas_w) noexcept {
    if (duration <= 0.0f) return canvas_x;
    const f32 n = t / duration;
    return canvas_x + ClampF(n, 0.0f, 1.0f) * canvas_w;
}

/**
 * canvas x 座標 (px) を time [0, Duration] に変換する。
 *
 * @param x 変換する canvas 内 x 座標 (px)。
 * @param duration タイムライン全体の長さ [秒]。
 * @param canvas_x canvas 左端の x 座標 (px)。
 * @param canvas_w canvas の幅 (px)。
 * @return 対応する時刻 [秒] (0..duration)。canvas_w <= 0 なら 0。
 */
static f32 CanvasXToTime(f32 x, f32 duration,
                         f32 canvas_x, f32 canvas_w) noexcept {
    if (canvas_w <= 0.0f) return 0.0f;
    const f32 n = (x - canvas_x) / canvas_w;
    return ClampF(n, 0.0f, 1.0f) * duration;
}

/** 内部 state を既定値にリセットする。 */
void ACinematicsTimelineEditorPanel::Init() noexcept {
    m_Director        = nullptr;
    m_Keyframes.Reset();
    m_SelectedIdx    = kNoKeySelected;
    m_CurrentTime    = 0.0f;
    m_Duration        = kDefaultDurationSec;
    m_Playing         = false;
    m_bDraggingMarker = false;
    m_DragIdx        = -1;
    m_AddKind        = ETimelineKeyKind::CameraCut;
}

/** 内部 state を全解放する (director は非所有なので破棄しない)。 */
void ACinematicsTimelineEditorPanel::Shutdown() noexcept {
    m_Director        = nullptr;
    m_Keyframes.Reset();
    m_SelectedIdx    = kNoKeySelected;
    m_CurrentTime    = 0.0f;
    m_Playing         = false;
    m_bDraggingMarker = false;
    m_DragIdx        = -1;
}

/** 編集対象の director を raw 参照でセットし、現状を即時 bake する。 */
void ACinematicsTimelineEditorPanel::SetCinematicsDirector(
        acs::game::CCinematicsDirector* dir) noexcept {
    m_Director     = dir;
    m_SelectedIdx = kNoKeySelected;
    m_CurrentTime = 0.0f;
    m_Playing      = false;
    // director を切替えたら editor の現状を即時 bake する (= 既に keyframe を
    // 編集していた場合、新 director にも同じ keyframe が出るのが直感的)。
    BakeToDirector();
}

/** 現在編集対象の director を返す。 */
acs::game::CCinematicsDirector*
ACinematicsTimelineEditorPanel::CurrentDirector() const noexcept {
    return m_Director;
}

/** 現状を director に bake してから頭出し再生する。 */
void ACinematicsTimelineEditorPanel::Play() noexcept {
    // editor の現状を director に bake してから再生開始 (= scrub で変更があった
    // 場合も runtime に反映される)。
    BakeToDirector();
    if (m_Director != nullptr) {
        // director は内部 m_Time / m_LastFiredIndex を Stop で 0 リセットする仕様。
        // editor の m_CurrentTime に合わせて再生を始めたいが、director は途中位置
        // 開始 API が無いため、簡略のため m_CurrentTime を 0 に戻して頭から再生。
        // (= 厳密な部分再生は将来 director に SeekTo(t) を足して対応)
        m_Director->Stop();
        m_Director->Play();
    }
    m_CurrentTime = 0.0f;
    m_Playing      = true;
}

/** 再生を一時停止する (時刻は保持)。 */
void ACinematicsTimelineEditorPanel::Pause() noexcept {
    m_Playing = false;
    if (m_Director != nullptr) {
        m_Director->Pause();
    }
}

/** 再生を完全停止し、時刻を 0 に戻す。 */
void ACinematicsTimelineEditorPanel::Stop() noexcept {
    m_Playing      = false;
    m_CurrentTime = 0.0f;
    if (m_Director != nullptr) {
        m_Director->Stop();
    }
}

/** 再生中のみ dt 秒進め、director と時刻を同期する。 */
void ACinematicsTimelineEditorPanel::Step(f32 dt) noexcept {
    if (!m_Playing) return;
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    if (m_Director != nullptr) {
        // director側で次に許容できる有限時刻を先に検証する。
        const f32 director_next_time = m_Director->CurrentTime() + dt;
        if (!std::isfinite(director_next_time)) return;
        m_Director->Tick(dt);
        // director の時刻と同期する (= editor cursor が一緒に進む)。
        m_CurrentTime = m_Director->CurrentTime();
    } else {
        const f32 next_time = m_CurrentTime + dt;
        if (!std::isfinite(next_time)) return;
        m_CurrentTime = next_time;
    }
    // duration を超えたら再生終了 (= Pause 相当、m_CurrentTime は最大値に固定)。
    if (m_CurrentTime >= m_Duration) {
        m_CurrentTime = m_Duration;
        m_Playing      = false;
        if (m_Director != nullptr) {
            m_Director->Pause();
        }
    }
}

/** 再生中かを返す。 */
bool ACinematicsTimelineEditorPanel::IsPlaying() const noexcept {
    return m_Playing;
}

/** 現在のタイムカーソル位置 [秒] を返す。 */
f32 ACinematicsTimelineEditorPanel::CurrentTimeSec() const noexcept {
    return m_CurrentTime;
}

/** タイムカーソル位置を [0, Duration] にクランプして設定する。 */
void ACinematicsTimelineEditorPanel::SetCurrentTimeSec(f32 t) noexcept {
    if (!std::isfinite(t)) return;
    m_CurrentTime = ClampF(t, 0.0f, m_Duration);
}

/** タイムライン全体の長さ [秒] を返す。 */
f32 ACinematicsTimelineEditorPanel::DurationSec() const noexcept {
    return m_Duration;
}

/** 全体の長さを設定し、範囲外の keyframe / 現在時刻をクランプする。 */
void ACinematicsTimelineEditorPanel::SetDurationSec(f32 d) noexcept {
    if (!std::isfinite(d)) return;
    if (d < kMinDurationSec) d = kMinDurationSec;
    m_Duration = d;
    // duration が縮んだ場合、既存 keyframe が範囲外に出ていれば clamp する。
    for (usize i = 0; i < m_Keyframes.Num(); ++i) {
        if (m_Keyframes[i].time_sec > m_Duration) {
            m_Keyframes[i].time_sec = m_Duration;
        }
    }
    if (m_CurrentTime > m_Duration) m_CurrentTime = m_Duration;
}

/** 現在選択中の keyframe index を返す。 */
i32 ACinematicsTimelineEditorPanel::SelectedKeyframeIndex() const noexcept {
    return m_SelectedIdx;
}

/** selection を変更する (有効範囲外は kNoKeySelected に丸める)。 */
void ACinematicsTimelineEditorPanel::SelectKeyframe(i32 i) noexcept {
    if (i < 0 || static_cast<usize>(i) >= m_Keyframes.Num()) {
        m_SelectedIdx = kNoKeySelected;
    } else {
        m_SelectedIdx = i;
    }
}

/** 新規 keyframe を time 昇順で追加し、selection と director を更新する。 */
void ACinematicsTimelineEditorPanel::AddKeyframe(ETimelineKeyKind kind,
                                                f32 time_sec) noexcept {
    if (!std::isfinite(time_sec)) return;
    FEditorKeyframe kf;
    kf.kind     = kind;
    kf.time_sec = ClampF(time_sec, 0.0f, m_Duration);
    // kind 別 default payload は struct のデフォルト初期値を使うので明示しない。
    const i32 inserted = InsertKeyframeSorted(kf);
    if (inserted >= 0) {
        m_SelectedIdx = inserted;
    }
    // director に即時 bake (= 次の Step / Play で発火対象になる)。
    // 編集オペレーション 1 回ごとに全 keyframe を bake するのは O(N) コストだが、
    // 典型 N < 200 なので実用上問題なし。
    BakeToDirector();
}

/** 選択中の keyframe を順序保存削除し、selection 解除後 director を更新する。 */
void ACinematicsTimelineEditorPanel::RemoveSelectedKeyframe() noexcept {
    if (m_SelectedIdx < 0) return;
    const usize idx = static_cast<usize>(m_SelectedIdx);
    if (idx >= m_Keyframes.Num()) {
        m_SelectedIdx = kNoKeySelected;
        return;
    }
    // 順序保存削除 (= shift)。TArray に Erase が無いので手動で詰める。
    const usize n = m_Keyframes.Num();
    for (usize i = idx; i + 1 < n; ++i) {
        m_Keyframes[i] = m_Keyframes[i + 1];
    }
    m_Keyframes.Pop();
    m_SelectedIdx = kNoKeySelected;
    // director に即時 bake (= 次の Step / Play で発火対象から外れる)。
    BakeToDirector();
}

/** time 昇順 (同時刻は登録順) を保ったまま keyframe を挿入し、挿入位置を返す。 */
i32 ACinematicsTimelineEditorPanel::InsertKeyframeSorted(
        const FEditorKeyframe& kf) noexcept {
    // time 昇順 (同時刻は登録順 = stable) を維持する挿入位置を線形探索。
    // 典型 N < 200 で線形でも実用問題なし。
    const usize n = m_Keyframes.Num();
    usize insert_at = n;
    for (usize i = 0; i < n; ++i) {
        if (kf.time_sec < m_Keyframes[i].time_sec) {
            insert_at = i;
            break;
        }
    }

    if (insert_at == n) {
        m_Keyframes.Add(kf);
    } else {
        // 末尾を 1 つ伸ばし、[insert_at..n-1] を 1 つ後ろにずらして空きを作る。
        m_Keyframes.Add(m_Keyframes[n - 1]);
        for (usize i = n - 1; i > insert_at; --i) {
            m_Keyframes[i] = m_Keyframes[i - 1];
        }
        m_Keyframes[insert_at] = kf;
    }
    return static_cast<i32>(insert_at);
}

/** editor の全 keyframe を runtime FTimelineKeyframe に変換して director に焼く。 */
void ACinematicsTimelineEditorPanel::BakeToDirector() noexcept {
    if (m_Director == nullptr) return;
    m_Director->Clear();
    const usize n = m_Keyframes.Num();
    for (usize i = 0; i < n; ++i) {
        const FEditorKeyframe& ek = m_Keyframes[i];
        FTimelineKeyframe rk;
        rk.time_sec = ek.time_sec;
        rk.kind     = ToTrackKind(ek.kind);
        // kind 別 payload を rk に詰める。FireEvent 系は各 kind の実値 (色 / scale /
        // effect_id) を event_id の 32bit に可逆エンコードして載せる (= 値を落とさない。
        // エンコード仕様は冒頭ヘルパ参照)。
        switch (ek.kind) {
        case ETimelineKeyKind::CameraCut:
            // camera payload に target を載せる (x,y)。z/zoom/duration は editor で
            // オーサリングしないので default のまま (実値の欠落ではない)。
            rk.payload.camera.target_pos =
                FVec2{ ek.camera_target.x, ek.camera_target.y };
            rk.payload.camera.zoom     = 1.0f;
            rk.payload.camera.duration = 0.0f;
            break;
        case ETimelineKeyKind::FadeColor:
            // フェード先の色 (end_color) を 24bit RGB で載せる (= 実値を保持)。
            rk.payload.event.event_id =
                kEventTagFadeColor | EncodeColorRGB(ek.fade_end_color);
            break;
        case ETimelineKeyKind::TimeScale:
            // time_scale を 24bit 固定小数で載せる (= 実値を保持)。
            rk.payload.event.event_id =
                kEventTagTimeScale | EncodeTimeScale(ek.time_scale);
            break;
        case ETimelineKeyKind::SpawnEffect:
            // effect_id (editor の event_id) を 24bit でそのまま載せる。
            rk.payload.event.event_id =
                kEventTagSpawnEffect | (ek.event_id & kEventPayloadMask);
            break;
        case ETimelineKeyKind::TriggerCallback:
            // 汎用 event_id を 24bit でそのまま載せる。
            rk.payload.event.event_id =
                kEventTagTriggerCallback | (ek.event_id & kEventPayloadMask);
            break;
        }
        m_Director->AddKeyframe(rk);
    }
}

/** Toolbar + timeline canvas + inspector + ruler を ImGui で描画する。 */
void ACinematicsTimelineEditorPanel::DrawUI() noexcept {
    if (!IsVisible()) return;

    if (!ImGui::Begin(Title(), &m_Visible)) {
        ImGui::End();
        return;
    }

    // Toolbar: Play / Pause / Stop / Step / Time slider / Add combo + button
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
        ImGui::SameLine();
        if (ImGui::Button("Step+0.1s")) {
            // 1 フレーム的に director を 0.1s 進める (= scrub テスト用)。
            // 再生中フラグに依存せず、director.Tick を強制呼出。
            if (m_Director != nullptr) {
                const bool was_playing = m_Director->IsPlaying();
                if (!was_playing) m_Director->Play();
                m_Director->Tick(0.1f);
                if (!was_playing) m_Director->Pause();
                m_CurrentTime = m_Director->CurrentTime();
            } else {
                m_CurrentTime = ClampF(m_CurrentTime + 0.1f, 0.0f, m_Duration);
            }
        }

        // Time slider (現在カーソル位置)。drag で scrub。
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        float t = m_CurrentTime;
        if (ImGui::SliderFloat("##time_slider", &t, 0.0f, m_Duration, "t=%.2fs")) {
            m_CurrentTime = ClampF(t, 0.0f, m_Duration);
        }

        // Duration 編集 (= 全体の長さを変える)。
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        float d = m_Duration;
        if (ImGui::DragFloat("##duration", &d, 0.1f, kMinDurationSec, 600.0f,
                              "dur=%.1fs")) {
            SetDurationSec(d);
        }

        // Add Keyframe combo + button (= 現在の time にこの kind を新規追加)。
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::BeginCombo("##add_kind", KindName(m_AddKind))) {
            const ETimelineKeyKind kAll[5] = {
                ETimelineKeyKind::CameraCut,
                ETimelineKeyKind::FadeColor,
                ETimelineKeyKind::TimeScale,
                ETimelineKeyKind::SpawnEffect,
                ETimelineKeyKind::TriggerCallback,
            };
            for (u32 i = 0; i < 5u; ++i) {
                const bool selected = (m_AddKind == kAll[i]);
                if (ImGui::Selectable(KindName(kAll[i]), selected)) {
                    m_AddKind = kAll[i];
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("+Add")) {
            AddKeyframe(m_AddKind, m_CurrentTime);
        }
        ImGui::SameLine();
        // 削除ボタン (選択中 keyframe が無ければ disable)。
        ImGui::BeginDisabled(m_SelectedIdx < 0);
        if (ImGui::Button("-Del")) {
            RemoveSelectedKeyframe();
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();

    // Layout: 左 (timeline canvas + ruler) と 右 (inspector) の二分割。
    const f32 right_pane_w = 260.0f;
    const f32 avail_w = ImGui::GetContentRegionAvail().x;
    const f32 left_pane_w = (avail_w > right_pane_w + 50.0f)
                          ? (avail_w - right_pane_w - 8.0f) : (avail_w);

    // ---- 左ペイン: timeline canvas + ruler ----
    ImGui::BeginChild("##timeline_left", ImVec2(left_pane_w, 0.0f), false);
    {
        const f32 canvas_w = ImGui::GetContentRegionAvail().x - 4.0f;
        const f32 track_h  = kTrackRowHeightPx * static_cast<f32>(kTrackCount);
        const f32 ruler_h  = 22.0f;
        const f32 canvas_h = track_h + ruler_h + 4.0f;

        const ImVec2 canvas_origin = ImGui::GetCursorScreenPos();
        const ImVec2 canvas_end    = ImVec2(canvas_origin.x + canvas_w,
                                            canvas_origin.y + canvas_h);

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // 背景 (= 暗グレー枠)
        dl->AddRectFilled(canvas_origin, canvas_end, IM_COL32(30, 30, 35, 255));
        dl->AddRect      (canvas_origin, canvas_end, IM_COL32(80, 80, 90, 255));

        // ルーラー (上端): 1s ごとに目盛 + テキスト。Duration が大きいと目盛間隔が狭くなるが
        // とりあえず 1s ステップで固定。
        const f32 ruler_y0 = canvas_origin.y;
        const f32 ruler_y1 = ruler_y0 + ruler_h;
        dl->AddRectFilled(ImVec2(canvas_origin.x, ruler_y0),
                          ImVec2(canvas_end.x, ruler_y1),
                          IM_COL32(40, 40, 48, 255));
        const i32 max_tick = static_cast<i32>(m_Duration + 0.5f);
        for (i32 s = 0; s <= max_tick; ++s) {
            const f32 x = TimeToCanvasX(static_cast<f32>(s), m_Duration,
                                        canvas_origin.x, canvas_w);
            dl->AddLine(ImVec2(x, ruler_y0),
                        ImVec2(x, ruler_y1),
                        IM_COL32(160, 160, 160, 220), 1.0f);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%ds", static_cast<int>(s));
            dl->AddText(ImVec2(x + 2.0f, ruler_y0 + 2.0f),
                        IM_COL32(200, 200, 200, 255), buf);
        }

        // トラック行 (5 行): 各行の y 範囲を [row_y0, row_y1] で計算、行ごとに zebra 縞背景。
        const f32 tracks_y0 = ruler_y1 + 2.0f;
        for (u32 r = 0; r < kTrackCount; ++r) {
            const f32 row_y0 = tracks_y0 + kTrackRowHeightPx * static_cast<f32>(r);
            const f32 row_y1 = row_y0 + kTrackRowHeightPx;
            // 行背景 (zebra 縞: 偶奇で色を少し変える)
            const ImU32 bg = (r & 1u)
                ? IM_COL32(38, 38, 44, 255)
                : IM_COL32(32, 32, 38, 255);
            dl->AddRectFilled(ImVec2(canvas_origin.x, row_y0),
                              ImVec2(canvas_end.x, row_y1), bg);
            // 行ラベル (= kind 名を左端に小さく表示)
            const ETimelineKeyKind row_kind = static_cast<ETimelineKeyKind>(r);
            dl->AddText(ImVec2(canvas_origin.x + 4.0f, row_y0 + 6.0f),
                        IM_COL32(170, 170, 170, 255),
                        KindName(row_kind));
        }
        // 全行の下端ライン
        dl->AddLine(ImVec2(canvas_origin.x, tracks_y0 + track_h),
                    ImVec2(canvas_end.x,    tracks_y0 + track_h),
                    IM_COL32(80, 80, 90, 255));

        // マウス hit-test / drag 処理用 InvisibleButton
        ImGui::InvisibleButton("##timeline_canvas",
                               ImVec2(canvas_w, canvas_h),
                               ImGuiButtonFlags_MouseButtonLeft);
        const bool canvas_hovered = ImGui::IsItemHovered();
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2  mouse = io.MousePos;

        // mouse down: marker の AABB hit-test で m_DragIdx を決定。
        if (canvas_hovered && !m_bDraggingMarker
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            i32 hit = -1;
            const f32 half_w = kMarkerWidthPx * 0.5f + kMarkerHitSlackPx;
            for (usize i = 0; i < m_Keyframes.Num(); ++i) {
                const FEditorKeyframe& kf = m_Keyframes[i];
                const u32 row = static_cast<u32>(kf.kind);
                if (row >= kTrackCount) continue;
                const f32 cx = TimeToCanvasX(kf.time_sec, m_Duration,
                                              canvas_origin.x, canvas_w);
                const f32 ry0 = tracks_y0
                              + kTrackRowHeightPx * static_cast<f32>(row);
                const f32 ry1 = ry0 + kTrackRowHeightPx;
                if (mouse.x >= cx - half_w && mouse.x <= cx + half_w
                 && mouse.y >= ry0 && mouse.y <= ry1) {
                    hit = static_cast<i32>(i);
                    break;
                }
            }
            if (hit >= 0) {
                m_SelectedIdx    = hit;
                m_bDraggingMarker = true;
                m_DragIdx        = hit;
            } else {
                // ルーラー領域クリックなら time scrub
                if (mouse.y >= ruler_y0 && mouse.y <= ruler_y1) {
                    m_CurrentTime = CanvasXToTime(mouse.x, m_Duration,
                                                  canvas_origin.x, canvas_w);
                } else {
                    // 何にも当たらないクリックは selection 解除
                    m_SelectedIdx = kNoKeySelected;
                }
            }
        }

        // drag 継続中: マウス x → time へ逆変換して keyframe.time_sec を更新。
        // 並び順を維持するため、変更後に sorted insert で並び直す簡易方式
        // (= 抜いて入れ直す)。
        if (m_bDraggingMarker && m_DragIdx >= 0
            && static_cast<usize>(m_DragIdx) < m_Keyframes.Num()) {
            const f32 new_t = CanvasXToTime(mouse.x, m_Duration,
                                            canvas_origin.x, canvas_w);
            FEditorKeyframe modified = m_Keyframes[static_cast<usize>(m_DragIdx)];
            modified.time_sec = ClampF(new_t, 0.0f, m_Duration);
            // 抜く → 並び直して挿入
            const usize remove_at = static_cast<usize>(m_DragIdx);
            const usize n = m_Keyframes.Num();
            for (usize i = remove_at; i + 1 < n; ++i) {
                m_Keyframes[i] = m_Keyframes[i + 1];
            }
            m_Keyframes.Pop();
            const i32 new_idx = InsertKeyframeSorted(modified);
            if (new_idx >= 0) {
                m_DragIdx     = new_idx;
                m_SelectedIdx = new_idx;
            }
        }

        // マウス release で drag 終了 + director に最終位置を bake。
        // drag 中は AnimCurveEditor と同様に毎フレーム再 sort はするが、
        // BakeToDirector は drag end で 1 度だけ (= drag 中の毎フレーム bake は
        // O(N) なので panel→director copy の overhead を最小化)。
        if (m_bDraggingMarker
            && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            m_bDraggingMarker = false;
            m_DragIdx        = -1;
            BakeToDirector();
        }

        // marker 描画: 全 keyframe を縦長四角で描く
        const f32 marker_pad_y = 4.0f;
        for (usize i = 0; i < m_Keyframes.Num(); ++i) {
            const FEditorKeyframe& kf = m_Keyframes[i];
            const u32 row = static_cast<u32>(kf.kind);
            if (row >= kTrackCount) continue;
            const f32 cx = TimeToCanvasX(kf.time_sec, m_Duration,
                                          canvas_origin.x, canvas_w);
            const f32 ry0 = tracks_y0
                          + kTrackRowHeightPx * static_cast<f32>(row);
            const f32 ry1 = ry0 + kTrackRowHeightPx;
            const f32 half_w = kMarkerWidthPx * 0.5f;
            const bool is_sel = (m_SelectedIdx == static_cast<i32>(i));
            // 選択中は白い太枠 + 元色塗り。非選択は元色塗り + 暗い細枠。
            const ImU32 fill = KindColor(kf.kind);
            const ImU32 stroke = is_sel
                ? IM_COL32(255, 255, 255, 255)
                : IM_COL32( 10,  10,  10, 220);
            const ImVec2 a(cx - half_w, ry0 + marker_pad_y);
            const ImVec2 b(cx + half_w, ry1 - marker_pad_y);
            dl->AddRectFilled(a, b, fill, 2.0f);
            dl->AddRect      (a, b, stroke, 2.0f, 0, is_sel ? 2.0f : 1.0f);
        }

        // 現在カーソルの縦線 (赤)
        {
            const f32 cx = TimeToCanvasX(m_CurrentTime, m_Duration,
                                          canvas_origin.x, canvas_w);
            dl->AddLine(ImVec2(cx, canvas_origin.y),
                        ImVec2(cx, canvas_end.y),
                        IM_COL32(255, 80, 80, 220), 2.0f);
        }
    }
    ImGui::EndChild();

    // ---- 右ペイン: inspector ----
    ImGui::SameLine();
    ImGui::BeginChild("##timeline_right", ImVec2(0.0f, 0.0f), true);
    {
        ImGui::TextUnformatted("Inspector");
        ImGui::Separator();
        if (m_SelectedIdx < 0
            || static_cast<usize>(m_SelectedIdx) >= m_Keyframes.Num()) {
            ImGui::TextDisabled("(no keyframe selected)");
            ImGui::TextDisabled("Click a marker in the timeline,");
            ImGui::TextDisabled("or use +Add to create one.");
        } else {
            // 参照が reorder で無効化されないよう、UI 編集前に kind を controlled
            // 変数として捕獲しておく (= time の DragFloat が並び替え + Add
            // による TArray 再確保を引き起こすと kf 参照が dangling になる)。
            const ETimelineKeyKind kind_for_payload =
                m_Keyframes[static_cast<usize>(m_SelectedIdx)].kind;
            ImGui::Text("Kind: %s", KindName(kind_for_payload));

            // time 編集 (drag float、[0, Duration] clamp)。reorder が発生したら
            // 以降の payload 編集は今フレームではスキップ (= 次フレームで安全に
            // 新位置の kf を編集)。
            {
                float t = m_Keyframes[static_cast<usize>(m_SelectedIdx)].time_sec;
                if (ImGui::DragFloat("time (s)", &t, 0.05f, 0.0f, m_Duration, "%.3f")) {
                    FEditorKeyframe modified =
                        m_Keyframes[static_cast<usize>(m_SelectedIdx)];
                    modified.time_sec = ClampF(t, 0.0f, m_Duration);
                    // 古 entry を順序保存削除 → 新時刻で挿入し直す。
                    const usize remove_at = static_cast<usize>(m_SelectedIdx);
                    const usize n = m_Keyframes.Num();
                    for (usize i = remove_at; i + 1 < n; ++i) {
                        m_Keyframes[i] = m_Keyframes[i + 1];
                    }
                    m_Keyframes.Pop();
                    const i32 new_idx = InsertKeyframeSorted(modified);
                    if (new_idx >= 0) m_SelectedIdx = new_idx;
                    BakeToDirector();
                }
            }
            ImGui::Separator();

            // payload 編集 (= 上記 time 編集で reorder されていなければ index は
            // 有効なので、改めて参照を取り直して安全に編集)。
            if (static_cast<usize>(m_SelectedIdx) < m_Keyframes.Num()) {
                FEditorKeyframe& kf = m_Keyframes[static_cast<usize>(m_SelectedIdx)];
                switch (kind_for_payload) {
                case ETimelineKeyKind::CameraCut: {
                    float v[3] = { kf.camera_target.x,
                                   kf.camera_target.y,
                                   kf.camera_target.z };
                    if (ImGui::DragFloat3("target", v, 0.1f)) {
                        kf.camera_target = FVec3{ v[0], v[1], v[2] };
                        BakeToDirector();
                    }
                    break;
                }
                case ETimelineKeyKind::FadeColor: {
                    float c0[3] = { kf.fade_start_color.x,
                                    kf.fade_start_color.y,
                                    kf.fade_start_color.z };
                    float c1[3] = { kf.fade_end_color.x,
                                    kf.fade_end_color.y,
                                    kf.fade_end_color.z };
                    if (ImGui::ColorEdit3("start", c0)) {
                        kf.fade_start_color = FVec3{ c0[0], c0[1], c0[2] };
                    }
                    if (ImGui::ColorEdit3("end", c1)) {
                        kf.fade_end_color = FVec3{ c1[0], c1[1], c1[2] };
                    }
                    break;
                }
                case ETimelineKeyKind::TimeScale: {
                    float s = kf.time_scale;
                    if (ImGui::DragFloat("scale", &s, 0.01f, 0.0f, 8.0f, "%.3f")) {
                        kf.time_scale = s;
                    }
                    break;
                }
                case ETimelineKeyKind::SpawnEffect: {
                    int eid = static_cast<int>(kf.event_id);
                    if (ImGui::DragInt("effect_id", &eid, 1.0f, 0, 65535)) {
                        kf.event_id = static_cast<u32>(eid < 0 ? 0 : eid);
                        BakeToDirector();
                    }
                    float p[3] = { kf.camera_target.x,
                                   kf.camera_target.y,
                                   kf.camera_target.z };
                    if (ImGui::DragFloat3("position", p, 0.1f)) {
                        kf.camera_target = FVec3{ p[0], p[1], p[2] };
                    }
                    break;
                }
                case ETimelineKeyKind::TriggerCallback: {
                    int eid = static_cast<int>(kf.event_id);
                    if (ImGui::DragInt("event_id", &eid, 1.0f, 0, 65535)) {
                        kf.event_id = static_cast<u32>(eid < 0 ? 0 : eid);
                        BakeToDirector();
                    }
                    break;
                }
                }
            }
        }
        ImGui::Separator();
        ImGui::Text("Count: %u", static_cast<unsigned>(m_Keyframes.Num()));
        ImGui::Text("Director: %s", m_Director ? "bound" : "(none)");
        ImGui::Text("Playing: %s", m_Playing ? "yes" : "no");
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace acs::game::cinetimeline
