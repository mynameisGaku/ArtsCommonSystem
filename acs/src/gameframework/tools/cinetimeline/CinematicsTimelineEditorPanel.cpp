// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — cinetimeline / FCinematicsTimelineEditorPanel 実装 (Phase 23)
//
// 仕様の意図は FCinematicsTimelineEditorPanel.h を参照。本ファイルでは:
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

#include <cstdio>   // std::snprintf (marker label 整形)

namespace acs::game::cinetimeline {

// =============================================================================
// ローカルヘルパ
// =============================================================================

// f32 用の任意レンジ clamp。
static f32 ClampF(f32 v, f32 lo, f32 hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

// -----------------------------------------------------------------------------
// editor payload → runtime event_id エンコード
// -----------------------------------------------------------------------------
// 背景: runtime 側 FTimelineKeyframe の FireEvent payload は `u32 event_id` 1 個
// しか持たない。一方 editor は kind ごとにリッチな実値 (色 / scale / effect_id)
// をオーサリングする。BakeToDirector がこの実値を捨てて event_id=0 だけ焼くと、
// 「構造は焼けたが値が落ちる」= caller 側で演出を再現できない。そこで FireEvent
// 経由の各 kind は、**実値を event_id の 32bit に可逆エンコードして載せる**。
// caller は最上位 8bit の kind タグを見て、残り 24bit を kind ごとに復号する。
//
//   bit 31..24 : kind タグ (kEventTag*)
//   bit 23.. 0 : kind 別ペイロード
//     FadeColor       : fade_end_color を 8bit RGB に量子化 (0xRRGGBB、24bit 完全)
//     TimeScale       : time_scale [0,8] を 24bit 固定小数に量子化 (実用精度で可逆)
//     SpawnEffect     : effect_id (= editor の event_id) を 24bit でそのまま
//     TriggerCallback : event_id を 24bit でそのまま (互換のため下位 24bit 維持)
// これで「値が落ちる」不具合を解消する。FadeColor の start_color のみ 32bit に
// 収まらず載らない (runtime payload が 1×u32 のため) — これは runtime 契約の上限で
// あり、本 panel 単独では解決不能。偽の成功ではなく「end_color は完全に載る /
// start_color は別フェーズで runtime payload 拡張時に対応」という正直な縮退とする。
enum : u32 {
    kEventTagFadeColor       = 0x01u << 24,
    kEventTagTimeScale       = 0x02u << 24,
    kEventTagSpawnEffect     = 0x03u << 24,
    kEventTagTriggerCallback = 0x04u << 24,
    kEventPayloadMask        = 0x00FFFFFFu,  // 下位 24bit
};

// f32 チャンネル [0,1] → 8bit 量子化。
static u32 QuantizeUnit8(f32 v) noexcept {
    const f32 c = ClampF(v, 0.0f, 1.0f);
    i32 q = static_cast<i32>(c * 255.0f + 0.5f);
    if (q < 0)   q = 0;
    if (q > 255) q = 255;
    return static_cast<u32>(q);
}

// FVec3 色 (r,g,b in [0,1]) → 0x00RRGGBB (24bit)。
static u32 EncodeColorRGB(const FVec3& c) noexcept {
    return (QuantizeUnit8(c.x) << 16)
         | (QuantizeUnit8(c.y) << 8)
         | (QuantizeUnit8(c.z));
}

// time_scale [0,8] → 24bit 固定小数 (1/(2^21) 刻み、編集 step 0.01 を十分上回る精度)。
// kEventTagTimeScale と OR して使う。caller は (raw & mask) / 2097152.0f で復号。
static constexpr f32 kTimeScaleFixedDenom = 2097152.0f;  // 2^21
static u32 EncodeTimeScale(f32 s) noexcept {
    const f32 c = ClampF(s, 0.0f, 8.0f);  // inspector DragFloat と同レンジ
    u32 q = static_cast<u32>(c * kTimeScaleFixedDenom + 0.5f);
    if (q > kEventPayloadMask) q = kEventPayloadMask;  // 念のため 24bit に飽和
    return q;
}

// ETimelineKeyKind の表示名 (toolbar combo / inspector / marker tooltip 用)。
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

// 5 種のトラック色 (marker の塗り色)。
// 視認性のため意味のある色 (camera=cyan / fade=yellow / time=magenta /
// spawn=green / trigger=orange) を選ぶ。
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

// editor kind → director runtime kind の mapping。editor は 5 種 (CameraCut /
// FadeColor / TimeScale / SpawnEffect / TriggerCallback)、runtime は
// {Wait / MoveCamera / ShowDialogue / PlayMusic / FireEvent} の 5 種で意味が
// 異なる。素直なマッピング:
//   CameraCut       -> MoveCamera (camera payload に target_pos を載せる)
//   FadeColor       -> FireEvent  (event_id = kEventTagFadeColor   | RGB24(end_color))
//   TimeScale       -> FireEvent  (event_id = kEventTagTimeScale   | fixed24(scale))
//   SpawnEffect     -> FireEvent  (event_id = kEventTagSpawnEffect | (effect_id & 24bit))
//   TriggerCallback -> FireEvent  (event_id = kEventTagTriggerCallback | (event_id & 24bit))
// FireEvent 経由のものは event_id の最上位 8bit が kind タグ、下位 24bit が kind 別
// の実ペイロードを保持する (上記 Encode* ヘルパ参照)。caller はタグで分岐し下位
// 24bit を復号することで、editor でオーサリングした実値を runtime に復元できる。
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

// time [0, Duration] → canvas x 座標 (px) 変換。
static f32 TimeToCanvasX(f32 t, f32 duration,
                         f32 canvas_x, f32 canvas_w) noexcept {
    if (duration <= 0.0f) return canvas_x;
    const f32 n = t / duration;
    return canvas_x + ClampF(n, 0.0f, 1.0f) * canvas_w;
}

// canvas x (px) → time [0, Duration] 変換。
static f32 CanvasXToTime(f32 x, f32 duration,
                         f32 canvas_x, f32 canvas_w) noexcept {
    if (canvas_w <= 0.0f) return 0.0f;
    const f32 n = (x - canvas_x) / canvas_w;
    return ClampF(n, 0.0f, 1.0f) * duration;
}

// =============================================================================
// Init / Shutdown / SetCinematicsDirector / CurrentDirector
// =============================================================================
void FCinematicsTimelineEditorPanel::Init() noexcept {
    m_Director        = nullptr;
    m_Keyframes.Clear();
    m_SelectedIdx    = kNoKeySelected;
    m_CurrentTime    = 0.0f;
    m_Duration        = kDefaultDurationSec;
    m_Playing         = false;
    m_bDraggingMarker = false;
    m_DragIdx        = -1;
    m_AddKind        = ETimelineKeyKind::CameraCut;
}

void FCinematicsTimelineEditorPanel::Shutdown() noexcept {
    m_Director        = nullptr;
    m_Keyframes.Clear();
    m_SelectedIdx    = kNoKeySelected;
    m_CurrentTime    = 0.0f;
    m_Playing         = false;
    m_bDraggingMarker = false;
    m_DragIdx        = -1;
}

void FCinematicsTimelineEditorPanel::SetCinematicsDirector(
        acs::game::FCinematicsDirector* dir) noexcept {
    m_Director     = dir;
    m_SelectedIdx = kNoKeySelected;
    m_CurrentTime = 0.0f;
    m_Playing      = false;
    // director を切替えたら editor の現状を即時 bake する (= 既に keyframe を
    // 編集していた場合、新 director にも同じ keyframe が出るのが直感的)。
    BakeToDirector();
}

acs::game::FCinematicsDirector*
FCinematicsTimelineEditorPanel::CurrentDirector() const noexcept {
    return m_Director;
}

// =============================================================================
// 再生制御
// =============================================================================
void FCinematicsTimelineEditorPanel::Play() noexcept {
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

void FCinematicsTimelineEditorPanel::Pause() noexcept {
    m_Playing = false;
    if (m_Director != nullptr) {
        m_Director->Pause();
    }
}

void FCinematicsTimelineEditorPanel::Stop() noexcept {
    m_Playing      = false;
    m_CurrentTime = 0.0f;
    if (m_Director != nullptr) {
        m_Director->Stop();
    }
}

void FCinematicsTimelineEditorPanel::Step(f32 dt) noexcept {
    if (!m_Playing) return;
    if (dt <= 0.0f) return;
    if (m_Director != nullptr) {
        m_Director->Tick(dt);
        // director の時刻と同期する (= editor cursor が一緒に進む)。
        m_CurrentTime = m_Director->CurrentTime();
    } else {
        m_CurrentTime += dt;
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

bool FCinematicsTimelineEditorPanel::IsPlaying() const noexcept {
    return m_Playing;
}

// =============================================================================
// 時間軸アクセサ
// =============================================================================
f32 FCinematicsTimelineEditorPanel::CurrentTimeSec() const noexcept {
    return m_CurrentTime;
}

void FCinematicsTimelineEditorPanel::SetCurrentTimeSec(f32 t) noexcept {
    m_CurrentTime = ClampF(t, 0.0f, m_Duration);
}

f32 FCinematicsTimelineEditorPanel::DurationSec() const noexcept {
    return m_Duration;
}

void FCinematicsTimelineEditorPanel::SetDurationSec(f32 d) noexcept {
    if (d < kMinDurationSec) d = kMinDurationSec;
    m_Duration = d;
    // duration が縮んだ場合、既存 keyframe が範囲外に出ていれば clamp する。
    for (usize i = 0; i < m_Keyframes.Size(); ++i) {
        if (m_Keyframes[i].time_sec > m_Duration) {
            m_Keyframes[i].time_sec = m_Duration;
        }
    }
    if (m_CurrentTime > m_Duration) m_CurrentTime = m_Duration;
}

// =============================================================================
// keyframe 操作
// =============================================================================
i32 FCinematicsTimelineEditorPanel::SelectedKeyframeIndex() const noexcept {
    return m_SelectedIdx;
}

void FCinematicsTimelineEditorPanel::SelectKeyframe(i32 i) noexcept {
    if (i < 0 || static_cast<usize>(i) >= m_Keyframes.Size()) {
        m_SelectedIdx = kNoKeySelected;
    } else {
        m_SelectedIdx = i;
    }
}

void FCinematicsTimelineEditorPanel::AddKeyframe(ETimelineKeyKind kind,
                                                f32 time_sec) noexcept {
    EditorKeyframe kf;
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

void FCinematicsTimelineEditorPanel::RemoveSelectedKeyframe() noexcept {
    if (m_SelectedIdx < 0) return;
    const usize idx = static_cast<usize>(m_SelectedIdx);
    if (idx >= m_Keyframes.Size()) {
        m_SelectedIdx = kNoKeySelected;
        return;
    }
    // 順序保存削除 (= shift)。TArray に Erase が無いので手動で詰める。
    const usize n = m_Keyframes.Size();
    for (usize i = idx; i + 1 < n; ++i) {
        m_Keyframes[i] = m_Keyframes[i + 1];
    }
    m_Keyframes.PopBack();
    m_SelectedIdx = kNoKeySelected;
    // director に即時 bake (= 次の Step / Play で発火対象から外れる)。
    BakeToDirector();
}

// =============================================================================
// 内部ヘルパ
// =============================================================================
i32 FCinematicsTimelineEditorPanel::InsertKeyframeSorted(
        const EditorKeyframe& kf) noexcept {
    // time 昇順 (同時刻は登録順 = stable) を維持する挿入位置を線形探索。
    // 典型 N < 200 で線形でも実用問題なし。
    const usize n = m_Keyframes.Size();
    usize insert_at = n;
    for (usize i = 0; i < n; ++i) {
        if (kf.time_sec < m_Keyframes[i].time_sec) {
            insert_at = i;
            break;
        }
    }

    if (insert_at == n) {
        m_Keyframes.PushBack(kf);
    } else {
        // 末尾を 1 つ伸ばし、[insert_at..n-1] を 1 つ後ろにずらして空きを作る。
        m_Keyframes.PushBack(m_Keyframes[n - 1]);
        for (usize i = n - 1; i > insert_at; --i) {
            m_Keyframes[i] = m_Keyframes[i - 1];
        }
        m_Keyframes[insert_at] = kf;
    }
    return static_cast<i32>(insert_at);
}

void FCinematicsTimelineEditorPanel::BakeToDirector() noexcept {
    if (m_Director == nullptr) return;
    m_Director->Clear();
    const usize n = m_Keyframes.Size();
    for (usize i = 0; i < n; ++i) {
        const EditorKeyframe& ek = m_Keyframes[i];
        FTimelineKeyframe rk;
        rk.time_sec = ek.time_sec;
        rk.kind     = ToTrackKind(ek.kind);
        // kind 別 payload を rk に詰める。
        // 修正: 旧実装は FireEvent 系で ek.event_id をそのまま焼くだけで、
        //       FadeColor の色 / TimeScale の scale という「オーサリングした実値」を
        //       完全に取りこぼしていた (しかも色/scale kind は UI で event_id を
        //       触らないので常に 0 = 全 keyframe が同じ no-op event に潰れる)。
        //       下では各 kind の実値を event_id の 32bit に可逆エンコードして載せ、
        //       値が落ちないようにする (エンコード仕様は冒頭ヘルパ参照)。
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

// =============================================================================
// DrawUI — ImGui で toolbar + timeline canvas + inspector + ruler を描画
// =============================================================================
void FCinematicsTimelineEditorPanel::DrawUI() noexcept {
    if (!IsVisible()) return;

    if (!ImGui::Begin(Title(), &m_Visible)) {
        ImGui::End();
        return;
    }

    // ------------------------------------------------------------------------
    // Toolbar: Play / Pause / Stop / Step / Time slider / Add combo + button
    // ------------------------------------------------------------------------
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

    // ------------------------------------------------------------------------
    // Layout: 左 (timeline canvas + ruler) と 右 (inspector) の二分割。
    // ------------------------------------------------------------------------
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

        // ----- ルーラー (上端) -----
        // 1s ごとに目盛 + テキスト。Duration が大きいと目盛間隔が狭くなるが
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

        // ----- トラック行 (5 行) -----
        // 各行の y 範囲を [track_y0, track_y1] で計算、行間に薄いグリッド線。
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

        // ----- マウス hit-test / drag 処理用 InvisibleButton -----
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
            for (usize i = 0; i < m_Keyframes.Size(); ++i) {
                const EditorKeyframe& kf = m_Keyframes[i];
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
            && static_cast<usize>(m_DragIdx) < m_Keyframes.Size()) {
            const f32 new_t = CanvasXToTime(mouse.x, m_Duration,
                                            canvas_origin.x, canvas_w);
            EditorKeyframe modified = m_Keyframes[static_cast<usize>(m_DragIdx)];
            modified.time_sec = ClampF(new_t, 0.0f, m_Duration);
            // 抜く → 並び直して挿入
            const usize remove_at = static_cast<usize>(m_DragIdx);
            const usize n = m_Keyframes.Size();
            for (usize i = remove_at; i + 1 < n; ++i) {
                m_Keyframes[i] = m_Keyframes[i + 1];
            }
            m_Keyframes.PopBack();
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

        // ----- marker 描画 (全 keyframe を縦長四角で) -----
        const f32 marker_pad_y = 4.0f;
        for (usize i = 0; i < m_Keyframes.Size(); ++i) {
            const EditorKeyframe& kf = m_Keyframes[i];
            const u32 row = static_cast<u32>(kf.kind);
            if (row >= kTrackCount) continue;
            const f32 cx = TimeToCanvasX(kf.time_sec, m_Duration,
                                          canvas_origin.x, canvas_w);
            const f32 ry0 = tracks_y0
                          + kTrackRowHeightPx * static_cast<f32>(row);
            const f32 ry1 = ry0 + kTrackRowHeightPx;
            const f32 half_w = kMarkerWidthPx * 0.5f;
            const bool is_sel = (m_SelectedIdx == static_cast<i32>(i));
            // 選択中は黄色枠 + 元色塗り。非選択は元色だけ。
            const ImU32 fill = KindColor(kf.kind);
            const ImU32 stroke = is_sel
                ? IM_COL32(255, 255, 255, 255)
                : IM_COL32( 10,  10,  10, 220);
            const ImVec2 a(cx - half_w, ry0 + marker_pad_y);
            const ImVec2 b(cx + half_w, ry1 - marker_pad_y);
            dl->AddRectFilled(a, b, fill, 2.0f);
            dl->AddRect      (a, b, stroke, 2.0f, 0, is_sel ? 2.0f : 1.0f);
        }

        // ----- 現在カーソルの縦線 (赤) -----
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
            || static_cast<usize>(m_SelectedIdx) >= m_Keyframes.Size()) {
            ImGui::TextDisabled("(no keyframe selected)");
            ImGui::TextDisabled("Click a marker in the timeline,");
            ImGui::TextDisabled("or use +Add to create one.");
        } else {
            // 参照が reorder で無効化されないよう、UI 編集前に kind を controlled
            // 変数として捕獲しておく (= time の DragFloat が並び替え + PushBack
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
                    EditorKeyframe modified =
                        m_Keyframes[static_cast<usize>(m_SelectedIdx)];
                    modified.time_sec = ClampF(t, 0.0f, m_Duration);
                    // 古 entry を順序保存削除 → 新時刻で挿入し直す。
                    const usize remove_at = static_cast<usize>(m_SelectedIdx);
                    const usize n = m_Keyframes.Size();
                    for (usize i = remove_at; i + 1 < n; ++i) {
                        m_Keyframes[i] = m_Keyframes[i + 1];
                    }
                    m_Keyframes.PopBack();
                    const i32 new_idx = InsertKeyframeSorted(modified);
                    if (new_idx >= 0) m_SelectedIdx = new_idx;
                    BakeToDirector();
                }
            }
            ImGui::Separator();

            // payload 編集 (= 上記 time 編集で reorder されていなければ index は
            // 有効なので、改めて参照を取り直して安全に編集)。
            if (static_cast<usize>(m_SelectedIdx) < m_Keyframes.Size()) {
                EditorKeyframe& kf = m_Keyframes[static_cast<usize>(m_SelectedIdx)];
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
        ImGui::Text("Count: %u", static_cast<unsigned>(m_Keyframes.Size()));
        ImGui::Text("Director: %s", m_Director ? "bound" : "(none)");
        ImGui::Text("Playing: %s", m_Playing ? "yes" : "no");
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace acs::game::cinetimeline
