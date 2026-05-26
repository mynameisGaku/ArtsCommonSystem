// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — animcurve / FAnimCurveEditorPanel 実装 (Phase 22)
//
// 仕様の意図は AnimCurveEditorPanel.h を参照。本ファイルでは:
//   ・Init / Shutdown / SetCurve / dirty / callback: 状態管理
//   ・DrawUI: ImGui で
//       - Toolbar (Interp Combo / WrapMode Combo / Add Key / Clear /
//         Eval preview slider)
//       - Canvas (InvisibleButton + ChannelsSplit + 線描画 + key marker +
//         tangent handle + drag 処理 + 右クリック popup)
//     を描画する
//   ・NotifyChanged: dirty flag 立てと callback 発火
// すべて noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。
#include "gameframework/tools/animcurve/AnimCurveEditorPanel.h"

#include "gameframework/AnimationCurve.h"
#include "math/Math.h"

#include <imgui.h>

#include <cstdio>   // std::snprintf (label 整形)

namespace acs::game::animcurve {

// =============================================================================
// ローカルヘルパ
// =============================================================================

// 小さな clamp ヘルパ (Math.h は f32 専用の Saturate しか持たないため
// 任意レンジ用に自前で 1 個用意する)。
static f32 ClampF(f32 v, f32 lo, f32 hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

// FAnimationCurve から bbox (= curve 全体の [time_min, time_max] /
// [value_min, value_max]) を取得する。空 curve なら適当な default を返す
// (canvas を描画可能な状態にするため)。
//   ・time:  [0, 1] を最低範囲として、key が外に出ていれば拡張
//   ・value: [-1, 1] を最低範囲として、key が外に出ていれば拡張
// これで「key が密集していても canvas が崩壊しない」 + 「キー値が大きく
// なれば自動でズームアウト」する直感的な振る舞いになる。
struct CurveBounds {
    f32 time_min;
    f32 time_max;
    f32 value_min;
    f32 value_max;
};
static CurveBounds ComputeBounds(const FAnimationCurve* curve) noexcept {
    CurveBounds b { 0.0f, 1.0f, -1.0f, 1.0f };
    if (curve == nullptr) return b;
    const u32 n = curve->KeyCount();
    for (u32 i = 0; i < n; ++i) {
        const FCurveKey* k = curve->EKey(i);
        if (k == nullptr) continue;
        if (k->time  < b.time_min)  b.time_min  = k->time;
        if (k->time  > b.time_max)  b.time_max  = k->time;
        if (k->value < b.value_min) b.value_min = k->value;
        if (k->value > b.value_max) b.value_max = k->value;
    }
    // 退化防止 (= range == 0 だと canvas → curve の座標変換が NaN になる)。
    if (b.time_max - b.time_min < 0.001f)  b.time_max  = b.time_min  + 1.0f;
    if (b.value_max - b.value_min < 0.001f) b.value_max = b.value_min + 1.0f;
    // 上下 / 左右に少し余白 (= 10%)、key が枠ぴったりだと marker が
    // canvas 境界に潰れるため。
    const f32 t_pad = (b.time_max  - b.time_min ) * 0.10f;
    const f32 v_pad = (b.value_max - b.value_min) * 0.10f;
    b.time_min  -= t_pad; b.time_max  += t_pad;
    b.value_min -= v_pad; b.value_max += v_pad;
    return b;
}

// curve 空間 → canvas (px) 空間への変換 (= key 描画 / 線描画用)。
static ImVec2 CurveToCanvas(f32 time, f32 value,
                            const CurveBounds& b,
                            const ImVec2& canvas_origin,
                            const ImVec2& canvas_size) noexcept {
    const f32 nx = (time  - b.time_min ) / (b.time_max  - b.time_min );
    const f32 ny = (value - b.value_min) / (b.value_max - b.value_min);
    // y は画面下向きが正なので反転 (= curve 値が大きい = 上)。
    return ImVec2(canvas_origin.x + nx * canvas_size.x,
                  canvas_origin.y + (1.0f - ny) * canvas_size.y);
}

// canvas (px) → curve 空間への逆変換 (= ドラッグや右クリック追加用)。
static void CanvasToCurve(const ImVec2& pos,
                          const CurveBounds& b,
                          const ImVec2& canvas_origin,
                          const ImVec2& canvas_size,
                          f32& out_time, f32& out_value) noexcept {
    // ゼロ除算防御 (canvas_size が 0 なら curve_min を返す)。
    const f32 nx = (canvas_size.x > 0.0f)
                 ? (pos.x - canvas_origin.x) / canvas_size.x : 0.0f;
    const f32 ny = (canvas_size.y > 0.0f)
                 ? 1.0f - (pos.y - canvas_origin.y) / canvas_size.y : 0.0f;
    out_time  = b.time_min  + nx * (b.time_max  - b.time_min );
    out_value = b.value_min + ny * (b.value_max - b.value_min);
}

// interp combo / wrap mode combo の表示文字列。
static const char* InterpName(ECurveInterpolation interp) noexcept {
    switch (interp) {
    case ECurveInterpolation::Step:    return "Step";
    case ECurveInterpolation::Linear:  return "Linear";
    case ECurveInterpolation::Hermite: return "Hermite";
    }
    return "?";
}
static const char* WrapName(FAnimationCurve::WrapMode m) noexcept {
    switch (m) {
    case FAnimationCurve::WrapMode::Clamp:    return "Clamp";
    case FAnimationCurve::WrapMode::Loop:     return "Loop";
    case FAnimationCurve::WrapMode::PingPong: return "PingPong";
    }
    return "?";
}

// 既存 key の time, value, in_tangent, out_tangent, in_interp, out_interp を
// 編集する低レベルヘルパ。FAnimationCurve は AddKey で同 time の上書きが
// 「value + out_interp」だけ、AddKeyHermite で「value + in/out_tangent +
// in/out_interp」を更新する仕様なので、time を変えるには「古 key を Remove
// → 新 time で AddKeyHermite で再生成」する形にする。
//
// new_time: 新しい time (clamp 済みで渡すこと)
// 戻り値: 編集後の key の新 index (= sort 後の位置)。
static i32 ReplaceKeyAtNewTime(FAnimationCurve& curve, u32 old_idx,
                               f32 new_time, f32 new_value,
                               f32 new_in_tan, f32 new_out_tan,
                               ECurveInterpolation new_interp) noexcept {
    if (old_idx >= curve.KeyCount()) return -1;
    // 古 key を破棄
    curve.RemoveKey(old_idx);
    // 新規 key を追加 (Hermite なら tangent ごと、それ以外は AddKey で OK だが
    // 簡単のため常に AddKeyHermite で tangent も保存し、interp で in/out を
    // 上書きする 2 段構え)。
    curve.AddKeyHermite(new_time, new_value, new_in_tan, new_out_tan);
    // interp が Hermite 以外なら AddKey で out_interp だけ上書きする
    // (= AddKey の重複 time 検出で同じ key の out_interp / value を更新する
    //  仕様を利用)。
    if (new_interp != ECurveInterpolation::Hermite) {
        curve.AddKey(new_time, new_value, new_interp);
    }
    // 新 index を線形走査で取得 (KeyCount は典型で数十、線形でも問題なし)。
    const u32 n = curve.KeyCount();
    for (u32 i = 0; i < n; ++i) {
        const FCurveKey* k = curve.EKey(i);
        if (k != nullptr && k->time == new_time) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

// =============================================================================
// Init / Shutdown / SetCurve / IsDirty / ClearDirty / SetOnChangeCallback
// =============================================================================
void FAnimCurveEditorPanel::Init() noexcept {
    _curve            = nullptr;
    _selected_key_idx = kNoKeySelected;
    _dirty            = false;
    _drag_kind        = 0u;
    _drag_key_idx     = -1;
    _on_change_cb     = nullptr;
    _on_change_user   = nullptr;
}

void FAnimCurveEditorPanel::Shutdown() noexcept {
    _curve            = nullptr;
    _selected_key_idx = kNoKeySelected;
    _dirty            = false;
    _drag_kind        = 0u;
    _drag_key_idx     = -1;
    _on_change_cb     = nullptr;
    _on_change_user   = nullptr;
}

void FAnimCurveEditorPanel::SetCurve(FAnimationCurve* curve) noexcept {
    _curve            = curve;
    _selected_key_idx = kNoKeySelected;
    _dirty            = false;
    _drag_kind        = 0u;
    _drag_key_idx     = -1;
}

FAnimationCurve* FAnimCurveEditorPanel::CurrentCurve() const noexcept {
    return _curve;
}

bool FAnimCurveEditorPanel::IsDirty() const noexcept {
    return _dirty;
}

void FAnimCurveEditorPanel::ClearDirty() noexcept {
    _dirty = false;
}

void FAnimCurveEditorPanel::SetOnChangeCallback(CurveChangeCallback cb,
                                               void* user) noexcept {
    _on_change_cb   = cb;
    _on_change_user = user;
}

void FAnimCurveEditorPanel::NotifyChanged(bool immediate) noexcept {
    _dirty = true;
    if (immediate && _on_change_cb != nullptr) {
        _on_change_cb(_on_change_user, _curve);
    }
}

// =============================================================================
// DrawUI — ImGui で toolbar + canvas を描画
// =============================================================================
void FAnimCurveEditorPanel::DrawUI() noexcept {
    if (!IsVisible()) return;

    if (!ImGui::Begin(Title(), &_visible)) {
        ImGui::End();
        return;
    }

    if (_curve == nullptr) {
        ImGui::TextDisabled("(No curve bound)");
        ImGui::TextDisabled("Call SetCurve(&curve) on this panel to start editing.");
        ImGui::End();
        return;
    }

    FAnimationCurve& curve = *_curve;

    // ------------------------------------------------------------------------
    // Toolbar 1: Interpolation Combo (= 選択中 key の out_interp を切替)
    // ------------------------------------------------------------------------
    // 選択中 key の現在 interp を combo の初期値にする。未選択時は disable。
    {
        ImGui::TextUnformatted("Interp:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        const bool has_sel = (_selected_key_idx >= 0)
                          && (static_cast<u32>(_selected_key_idx) < curve.KeyCount());
        ECurveInterpolation cur = ECurveInterpolation::Linear;
        if (has_sel) {
            const FCurveKey* k = curve.EKey(static_cast<u32>(_selected_key_idx));
            if (k != nullptr) cur = k->out_interp;
        }
        ImGui::BeginDisabled(!has_sel);
        if (ImGui::BeginCombo("##interp_combo", InterpName(cur))) {
            const ECurveInterpolation kAll[3] = {
                ECurveInterpolation::Step,
                ECurveInterpolation::Linear,
                ECurveInterpolation::Hermite,
            };
            for (u32 i = 0; i < 3u; ++i) {
                const bool selected = (cur == kAll[i]);
                if (ImGui::Selectable(InterpName(kAll[i]), selected)) {
                    if (has_sel) {
                        const u32 idx = static_cast<u32>(_selected_key_idx);
                        const FCurveKey* k = curve.EKey(idx);
                        if (k != nullptr) {
                            // AddKey は同 time 上書きで value と out_interp を更新する。
                            curve.AddKey(k->time, k->value, kAll[i]);
                            NotifyChanged(true);
                        }
                    }
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }

    // ------------------------------------------------------------------------
    // Toolbar 2: WrapMode Combo (Pre / Post 個別)
    // ------------------------------------------------------------------------
    ImGui::SameLine();
    ImGui::TextUnformatted("Pre:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(95.0f);
    {
        const FAnimationCurve::WrapMode cur = curve.PreWrap();
        if (ImGui::BeginCombo("##pre_wrap", WrapName(cur))) {
            const FAnimationCurve::WrapMode kAll[3] = {
                FAnimationCurve::WrapMode::Clamp,
                FAnimationCurve::WrapMode::Loop,
                FAnimationCurve::WrapMode::PingPong,
            };
            for (u32 i = 0; i < 3u; ++i) {
                const bool selected = (cur == kAll[i]);
                if (ImGui::Selectable(WrapName(kAll[i]), selected)) {
                    curve.SetPreWrap(kAll[i]);
                    NotifyChanged(true);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Post:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(95.0f);
    {
        const FAnimationCurve::WrapMode cur = curve.PostWrap();
        if (ImGui::BeginCombo("##post_wrap", WrapName(cur))) {
            const FAnimationCurve::WrapMode kAll[3] = {
                FAnimationCurve::WrapMode::Clamp,
                FAnimationCurve::WrapMode::Loop,
                FAnimationCurve::WrapMode::PingPong,
            };
            for (u32 i = 0; i < 3u; ++i) {
                const bool selected = (cur == kAll[i]);
                if (ImGui::Selectable(WrapName(kAll[i]), selected)) {
                    curve.SetPostWrap(kAll[i]);
                    NotifyChanged(true);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    // ------------------------------------------------------------------------
    // Toolbar 3: Add Key / Clear
    // ------------------------------------------------------------------------
    ImGui::SameLine();
    if (ImGui::FButton("Add Key")) {
        // 「現在の curve の中点 time」あたりに新 key を追加するのが直感的。
        // 末尾 key.time / 2 を使う。0 key なら time=0.5, 1 key なら same+0.5。
        const u32 nk = curve.KeyCount();
        f32 new_t = 0.5f;
        if (nk >= 1u) {
            const FCurveKey* last = curve.EKey(nk - 1u);
            if (last != nullptr) new_t = last->time + 0.5f;
            if (nk >= 2u) {
                const FCurveKey* first = curve.EKey(0);
                if (first != nullptr) {
                    new_t = (first->time + last->time) * 0.5f + 0.001f;
                    // 既存 key と同 time にならないよう微小オフセット
                    // (AddKey は同 time だと上書きしてしまう = 新規追加にならない)。
                }
            }
        }
        curve.AddKey(new_t, 0.0f, ECurveInterpolation::Linear);
        // 追加 key を selection に
        const u32 newn = curve.KeyCount();
        for (u32 i = 0; i < newn; ++i) {
            const FCurveKey* k = curve.EKey(i);
            if (k != nullptr && k->time == new_t) {
                _selected_key_idx = static_cast<i32>(i);
                break;
            }
        }
        NotifyChanged(true);
    }
    ImGui::SameLine();
    if (ImGui::FButton("Clear")) {
        curve.ClearKeys();
        _selected_key_idx = kNoKeySelected;
        NotifyChanged(true);
    }

    // ------------------------------------------------------------------------
    // Toolbar 4: Eval preview slider (= 現在時刻でサンプルした値表示)
    // ------------------------------------------------------------------------
    {
        static f32 s_preview_t = 0.0f;   // 「現在時刻」preview 用
        const f32 dur = curve.Duration();
        ImGui::SameLine();
        ImGui::TextUnformatted("Eval t:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        const bool eval_enabled = (dur > 0.0f);
        ImGui::BeginDisabled(!eval_enabled);
        if (eval_enabled) {
            ImGui::SliderFloat("##eval_slider", &s_preview_t, 0.0f, dur, "%.3f");
        } else {
            float dummy = 0.0f;
            ImGui::SliderFloat("##eval_slider", &dummy, 0.0f, 1.0f, "n/a");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        const f32 sampled = eval_enabled ? curve.Evaluate(s_preview_t) : 0.0f;
        ImGui::Text("= %.3f", static_cast<double>(sampled));
    }

    ImGui::Separator();

    // ------------------------------------------------------------------------
    // Canvas — curve / key marker / tangent handle を描画 + drag 処理
    // ------------------------------------------------------------------------
    const CurveBounds bounds = ComputeBounds(&curve);

    // canvas サイズ: 残り window 領域を全て使う (高さは最小 200px 確保)。
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    if (canvas_size.x < 50.0f) canvas_size.x = 50.0f;
    if (canvas_size.y < 200.0f) canvas_size.y = 200.0f;
    const ImVec2 canvas_origin = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_end = ImVec2(canvas_origin.x + canvas_size.x,
                                     canvas_origin.y + canvas_size.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 背景 (= 暗グレー枠 + 細グリッド)
    dl->AddRectFilled(canvas_origin, canvas_end, IM_COL32(35, 35, 40, 255));
    dl->AddRect      (canvas_origin, canvas_end, IM_COL32(80, 80, 80, 255));
    // 縦線 (= time 軸の等分割) 11 本
    for (u32 i = 1; i < 11u; ++i) {
        const f32 x = canvas_origin.x
                    + canvas_size.x * static_cast<f32>(i) / 11.0f;
        dl->AddLine(ImVec2(x, canvas_origin.y),
                    ImVec2(x, canvas_end.y),
                    IM_COL32(60, 60, 60, 255));
    }
    // 横線 (= value 軸の 0 ライン)
    {
        const f32 ny = (0.0f - bounds.value_min)
                     / (bounds.value_max - bounds.value_min);
        const f32 y = canvas_origin.y + (1.0f - ClampF(ny, 0.0f, 1.0f)) * canvas_size.y;
        dl->AddLine(ImVec2(canvas_origin.x, y),
                    ImVec2(canvas_end.x, y),
                    IM_COL32(100, 100, 100, 255));
    }

    // クリック / drag を取るための InvisibleButton。
    ImGui::InvisibleButton("##curve_canvas", canvas_size,
                           ImGuiButtonFlags_MouseButtonLeft
                         | ImGuiButtonFlags_MouseButtonRight);
    const bool canvas_hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    // 曲線の連続線描画 (= kCurveSampleCount 点を線で繋ぐ)。
    // key が 2 個未満なら線にならないので marker のみで OK。
    if (curve.KeyCount() >= 2u) {
        const u32 n = kCurveSampleCount;
        const f32 t_step = (bounds.time_max - bounds.time_min)
                         / static_cast<f32>(n - 1u);
        ImVec2 prev_pt = CurveToCanvas(bounds.time_min,
                                       curve.Evaluate(bounds.time_min),
                                       bounds, canvas_origin, canvas_size);
        for (u32 i = 1; i < n; ++i) {
            const f32 t = bounds.time_min + static_cast<f32>(i) * t_step;
            const f32 v = curve.Evaluate(t);
            const ImVec2 cur_pt = CurveToCanvas(t, v, bounds, canvas_origin, canvas_size);
            dl->AddLine(prev_pt, cur_pt, IM_COL32(80, 200, 255, 255), 1.5f);
            prev_pt = cur_pt;
        }
    } else if (curve.KeyCount() == 1u) {
        // 1 key のみ: 横一直線 (= constant) を描画。
        const FCurveKey* k = curve.EKey(0);
        if (k != nullptr) {
            const ImVec2 a = CurveToCanvas(bounds.time_min, k->value,
                                           bounds, canvas_origin, canvas_size);
            const ImVec2 b = CurveToCanvas(bounds.time_max, k->value,
                                           bounds, canvas_origin, canvas_size);
            dl->AddLine(a, b, IM_COL32(80, 200, 255, 200), 1.5f);
        }
    }

    // key marker 描画 + tangent handle 描画 + クリック判定 (drag 開始)。
    // クリック判定はマウス位置と marker 中心の距離で行い、近い方優先。
    const f32 hit_radius = kKeyMarkerRadiusPx + 3.0f;
    const u32 nk = curve.KeyCount();

    // mouse down 時にどの要素にヒットしているかを評価。
    // _drag_kind が 0 (= 未 drag) のときだけ判定する (drag 中は既存 _drag_kind を維持)。
    if (canvas_hovered && _drag_kind == 0u
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        i32 best_idx = -1;
        u8  best_kind = 0u;
        f32 best_dist2 = hit_radius * hit_radius;
        for (u32 i = 0; i < nk; ++i) {
            const FCurveKey* k = curve.EKey(i);
            if (k == nullptr) continue;
            const ImVec2 kp = CurveToCanvas(k->time, k->value, bounds,
                                            canvas_origin, canvas_size);
            // key 本体
            {
                const f32 dx = mouse.x - kp.x;
                const f32 dy = mouse.y - kp.y;
                const f32 d2 = dx * dx + dy * dy;
                if (d2 < best_dist2) {
                    best_dist2 = d2;
                    best_idx   = static_cast<i32>(i);
                    best_kind  = 1u;
                }
            }
            // tangent handles (Hermite key のみ)
            if (k->out_interp == ECurveInterpolation::Hermite
                || k->in_interp  == ECurveInterpolation::Hermite) {
                // in: 左 (時間が小さい方向)
                const ImVec2 in_p = ImVec2(kp.x - kTangentHandleLengthPx,
                                           kp.y + k->in_tangent * 10.0f);
                {
                    const f32 dx = mouse.x - in_p.x;
                    const f32 dy = mouse.y - in_p.y;
                    const f32 d2 = dx * dx + dy * dy;
                    if (d2 < best_dist2) {
                        best_dist2 = d2;
                        best_idx   = static_cast<i32>(i);
                        best_kind  = 2u;
                    }
                }
                // out: 右
                const ImVec2 out_p = ImVec2(kp.x + kTangentHandleLengthPx,
                                            kp.y - k->out_tangent * 10.0f);
                {
                    const f32 dx = mouse.x - out_p.x;
                    const f32 dy = mouse.y - out_p.y;
                    const f32 d2 = dx * dx + dy * dy;
                    if (d2 < best_dist2) {
                        best_dist2 = d2;
                        best_idx   = static_cast<i32>(i);
                        best_kind  = 3u;
                    }
                }
            }
        }
        if (best_idx >= 0) {
            _selected_key_idx = best_idx;
            _drag_kind        = best_kind;
            _drag_key_idx     = best_idx;
        }
    }

    // drag 継続中の処理 (drag_kind != 0 のとき)。
    if (_drag_kind != 0u && _drag_key_idx >= 0
        && static_cast<u32>(_drag_key_idx) < curve.KeyCount()) {
        const u32 didx = static_cast<u32>(_drag_key_idx);
        const FCurveKey* k_const = curve.EKey(didx);
        if (k_const != nullptr) {
            if (_drag_kind == 1u) {
                // key 本体: マウス位置を curve 空間に逆変換して time/value 上書き。
                f32 new_t = 0.0f, new_v = 0.0f;
                CanvasToCurve(mouse, bounds, canvas_origin, canvas_size, new_t, new_v);
                // time は前後 key とのオーダー衝突を回避するため隣接 key の time
                // で clamp する (= 重複 time にならないよう微小 epsilon)。
                const f32 eps = 0.0001f;
                f32 lo = -1e9f, hi = 1e9f;
                if (didx > 0u) {
                    const FCurveKey* prev = curve.EKey(didx - 1u);
                    if (prev != nullptr) lo = prev->time + eps;
                }
                if (didx + 1u < curve.KeyCount()) {
                    const FCurveKey* nxt = curve.EKey(didx + 1u);
                    if (nxt != nullptr) hi = nxt->time - eps;
                }
                new_t = ClampF(new_t, lo, hi);
                // 古 key を削除 → 新 time/value で同 interp/tangent を保存して再挿入。
                const f32 in_t  = k_const->in_tangent;
                const f32 out_t = k_const->out_tangent;
                const ECurveInterpolation interp = k_const->out_interp;
                const i32 new_idx = ReplaceKeyAtNewTime(
                    curve, didx, new_t, new_v, in_t, out_t, interp);
                if (new_idx >= 0) {
                    _selected_key_idx = new_idx;
                    _drag_key_idx     = new_idx;
                }
            } else if (_drag_kind == 2u) {
                // in tangent: handle 位置からマウス y オフセットを 1/10 倍して
                // tangent 値に。X 軸 (= 時間方向) は固定 handle なので無視。
                const ImVec2 kp = CurveToCanvas(k_const->time, k_const->value,
                                                bounds, canvas_origin, canvas_size);
                const ImVec2 in_anchor = ImVec2(kp.x - kTangentHandleLengthPx, kp.y);
                const f32 dy = mouse.y - in_anchor.y;
                const f32 new_in_tan = dy / 10.0f;
                // 値は維持、out_tangent も維持、interp は Hermite に。
                curve.AddKeyHermite(k_const->time, k_const->value,
                                    new_in_tan, k_const->out_tangent);
            } else if (_drag_kind == 3u) {
                // out tangent: in と同様、ただし上向き = 正に。
                const ImVec2 kp = CurveToCanvas(k_const->time, k_const->value,
                                                bounds, canvas_origin, canvas_size);
                const ImVec2 out_anchor = ImVec2(kp.x + kTangentHandleLengthPx, kp.y);
                const f32 dy = out_anchor.y - mouse.y;
                const f32 new_out_tan = dy / 10.0f;
                curve.AddKeyHermite(k_const->time, k_const->value,
                                    k_const->in_tangent, new_out_tan);
            }
            // drag 中は dirty だけ立てる (callback は drag end で 1 度)。
            _dirty = true;
        }
    }

    // マウス release で drag 終了 + callback 1 回。
    if (_drag_kind != 0u && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (_on_change_cb != nullptr) {
            _on_change_cb(_on_change_user, &curve);
        }
        _drag_kind    = 0u;
        _drag_key_idx = -1;
    }

    // key marker を全描画 (drag 後の最新状態を反映)。
    for (u32 i = 0; i < curve.KeyCount(); ++i) {
        const FCurveKey* k = curve.EKey(i);
        if (k == nullptr) continue;
        const ImVec2 kp = CurveToCanvas(k->time, k->value, bounds,
                                        canvas_origin, canvas_size);
        const bool is_sel = (_selected_key_idx == static_cast<i32>(i));
        const ImU32 col = is_sel ? IM_COL32(255, 220, 100, 255)
                                  : IM_COL32(180, 180, 180, 255);
        dl->AddCircleFilled(kp, kKeyMarkerRadiusPx, col, 12);
        dl->AddCircle      (kp, kKeyMarkerRadiusPx, IM_COL32(0, 0, 0, 255), 12, 1.5f);

        // Hermite なら tangent handle を 2 本描画
        if (k->out_interp == ECurveInterpolation::Hermite
            || k->in_interp  == ECurveInterpolation::Hermite) {
            const ImVec2 in_p  = ImVec2(kp.x - kTangentHandleLengthPx,
                                        kp.y + k->in_tangent * 10.0f);
            const ImVec2 out_p = ImVec2(kp.x + kTangentHandleLengthPx,
                                        kp.y - k->out_tangent * 10.0f);
            dl->AddLine(kp, in_p,  IM_COL32(100, 220, 100, 200), 1.0f);
            dl->AddLine(kp, out_p, IM_COL32(100, 220, 100, 200), 1.0f);
            dl->AddCircleFilled(in_p,  3.0f, IM_COL32(100, 220, 100, 255), 8);
            dl->AddCircleFilled(out_p, 3.0f, IM_COL32(100, 220, 100, 255), 8);
        }
    }

    // 右クリックで context menu を開く (= curve に key 追加 / 選択 key 削除)。
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("##curve_ctx");
    }
    if (ImGui::BeginPopup("##curve_ctx")) {
        // popup を開いた瞬間のマウス位置を保存しないと、メニュー hover で
        // 値が変わってしまうため、開く直前のマウス座標から逆変換した値を表示。
        static ImVec2 s_ctx_open_pos { 0, 0 };
        if (ImGui::IsWindowAppearing()) {
            s_ctx_open_pos = mouse;
        }
        f32 ctx_t = 0.0f, ctx_v = 0.0f;
        CanvasToCurve(s_ctx_open_pos, bounds, canvas_origin, canvas_size,
                      ctx_t, ctx_v);

        if (ImGui::MenuItem("Add Key Here")) {
            curve.AddKey(ctx_t, ctx_v, ECurveInterpolation::Linear);
            // 追加した key を選択
            const u32 newn = curve.KeyCount();
            for (u32 i = 0; i < newn; ++i) {
                const FCurveKey* k = curve.EKey(i);
                if (k != nullptr && k->time == ctx_t) {
                    _selected_key_idx = static_cast<i32>(i);
                    break;
                }
            }
            NotifyChanged(true);
        }
        const bool has_sel = (_selected_key_idx >= 0)
                          && (static_cast<u32>(_selected_key_idx) < curve.KeyCount());
        ImGui::BeginDisabled(!has_sel);
        if (ImGui::MenuItem("Delete Selected Key")) {
            if (has_sel) {
                curve.RemoveKey(static_cast<u32>(_selected_key_idx));
                _selected_key_idx = kNoKeySelected;
                NotifyChanged(true);
            }
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    // 選択 key の情報を canvas 下にテキスト表示 (= デバッグ用 + 視認性向上)。
    if (_selected_key_idx >= 0
        && static_cast<u32>(_selected_key_idx) < curve.KeyCount()) {
        const FCurveKey* k = curve.EKey(static_cast<u32>(_selected_key_idx));
        if (k != nullptr) {
            ImGui::TextDisabled("Key %d: t=%.3f v=%.3f  in_tan=%.3f out_tan=%.3f  interp=%s",
                                static_cast<int>(_selected_key_idx),
                                static_cast<double>(k->time),
                                static_cast<double>(k->value),
                                static_cast<double>(k->in_tangent),
                                static_cast<double>(k->out_tangent),
                                InterpName(k->out_interp));
        }
    } else {
        ImGui::TextDisabled("(no key selected — click a key, or right-click canvas to add)");
    }

    ImGui::End();
}

} // namespace acs::game::animcurve
