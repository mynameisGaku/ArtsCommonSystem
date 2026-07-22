// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — animcurve / FAnimCurveEditorPanel 実装
//
// 仕様の意図は FAnimCurveEditorPanel.h を参照。本ファイルでは:
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
#include "gameframework/Easing.h"
#include "math/Math.h"

#include <imgui.h>

#include <cstdio>   // std::snprintf (label 整形)

namespace acs::game::animcurve {

/**
 * 任意レンジ用の clamp ヘルパ。
 *
 * @details Math.h は f32 専用の Saturate ([0,1]) しか持たないため自前で用意する。
 * @param v クランプ対象の値。
 * @param lo 下限。
 * @param hi 上限。
 * @return [lo, hi] にクランプした値。
 */
static f32 ClampF(f32 v, f32 lo, f32 hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

/**
 * curve 全体の表示用 bounding box (canvas ↔ curve 座標変換のレンジ)。
 *
 * @details time は最低 [0,1]、value は最低 [-1,1] を確保し、key がそれより外に
 * 出ていれば拡張する。これで key が密集しても canvas が崩壊せず、キー値が大きく
 * なれば自動でズームアウトする。
 */
struct FCurveBounds {
    /** time 軸の最小値 (左端)。 */
    f32 time_min;

    /** time 軸の最大値 (右端)。 */
    f32 time_max;

    /** value 軸の最小値 (下端)。 */
    f32 value_min;

    /** value 軸の最大値 (上端)。 */
    f32 value_max;
};

/**
 * curve から表示用の bounding box を計算する。
 *
 * @details 全 key を走査して最低レンジ ([0,1] x [-1,1]) を拡張し、退化防止
 * (range ~0 で座標変換が NaN になるのを防ぐ) と上下左右 10% の余白付けを行う。
 * 空 curve / nullptr なら既定レンジを返す (canvas を描画可能にするため)。
 * @param curve 計測対象の curve (nullptr 可)。
 * @return 余白込みの表示レンジ。
 */
static FCurveBounds ComputeBounds(const FAnimationCurve* curve) noexcept {
    FCurveBounds b { 0.0f, 1.0f, -1.0f, 1.0f };
    if (curve == nullptr) return b;
    const u32 n = curve->KeyCount();
    for (u32 i = 0; i < n; ++i) {
        const FCurveKey* k = curve->Key(i);
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

/**
 * curve 空間の点を canvas (px) 空間へ変換する (key 描画 / 線描画用)。
 *
 * @details y は画面下向きが正なので反転する (= curve 値が大きいほど画面上)。
 * @param time curve の time 座標。
 * @param value curve の value 座標。
 * @param b 座標変換のレンジ。
 * @param canvas_origin canvas 左上のスクリーン座標。
 * @param canvas_size canvas の px サイズ。
 * @return 対応する canvas 上の px 座標。
 */
static ImVec2 CurveToCanvas(f32 time, f32 value,
                            const FCurveBounds& b,
                            const ImVec2& canvas_origin,
                            const ImVec2& canvas_size) noexcept {
    const f32 nx = (time  - b.time_min ) / (b.time_max  - b.time_min );
    const f32 ny = (value - b.value_min) / (b.value_max - b.value_min);
    // y は画面下向きが正なので反転 (= curve 値が大きい = 上)。
    return ImVec2(canvas_origin.x + nx * canvas_size.x,
                  canvas_origin.y + (1.0f - ny) * canvas_size.y);
}

/**
 * canvas (px) 上の点を curve 空間へ逆変換する (ドラッグ / 右クリック追加用)。
 *
 * @details canvas_size が 0 のときはゼロ除算を避けて正規化座標 0 とみなす。
 * @param pos canvas 上の px 座標。
 * @param b 座標変換のレンジ。
 * @param canvas_origin canvas 左上のスクリーン座標。
 * @param canvas_size canvas の px サイズ。
 * @param out_time 逆変換した time を書き戻す。
 * @param out_value 逆変換した value を書き戻す。
 */
static void CanvasToCurve(const ImVec2& pos,
                          const FCurveBounds& b,
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

/**
 * 補間モードの表示文字列を返す (interp combo 用)。
 *
 * @param interp 補間モード。
 * @return "Step" / "Linear" / "Hermite" (未知なら "?")。
 */
static const char* InterpName(ECurveInterpolation interp) noexcept {
    switch (interp) {
    case ECurveInterpolation::Step:    return "Step";
    case ECurveInterpolation::Linear:  return "Linear";
    case ECurveInterpolation::Hermite: return "Hermite";
    }
    return "?";
}

/**
 * wrap モードの表示文字列を返す (wrap mode combo 用)。
 *
 * @param m wrap モード。
 * @return "Clamp" / "Loop" / "PingPong" (未知なら "?")。
 */
static const char* WrapName(FAnimationCurve::EWrapMode m) noexcept {
    switch (m) {
    case FAnimationCurve::EWrapMode::Clamp:    return "Clamp";
    case FAnimationCurve::EWrapMode::Loop:     return "Loop";
    case FAnimationCurve::EWrapMode::PingPong: return "PingPong";
    }
    return "?";
}

/**
 * 既存 key を別 time へ移動する低レベルヘルパ (古 key 削除 → 新 time で再生成)。
 *
 * @details
 * FAnimationCurve は AddKey で同 time 上書き時に value + out_interp のみ、
 * AddKeyHermite で value + in/out_tangent + in/out_interp を更新する仕様のため、
 * time を変えるには古 key を RemoveKey して新 time で再挿入する必要がある。常に
 * AddKeyHermite で tangent を保存し、interp が Hermite 以外なら AddKey で out_interp
 * を上書きする 2 段構えにしている。
 * @param curve 編集対象の curve。
 * @param old_idx 移動元の key index。
 * @param new_time 新しい time (呼び出し側で clamp 済みで渡すこと)。
 * @param new_value 新しい value。
 * @param new_in_tan 新しい in tangent。
 * @param new_out_tan 新しい out tangent。
 * @param new_interp 適用する out 補間モード。
 * @return 再挿入後 (sort 後) の key index。old_idx 範囲外や検索失敗なら -1。
 */
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
        const FCurveKey* k = curve.Key(i);
        if (k != nullptr && k->time == new_time) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

/** 内部 state を初期値に戻す。 */
void FAnimCurveEditorPanel::Init() noexcept {
    m_Curve            = nullptr;
    m_SelectedKeyIdx = kNoKeySelected;
    m_Dirty            = false;
    m_SelectedEasingPreset =
        static_cast<u8>(Easing::EEasingType::Linear);
    m_DragKind        = 0u;
    m_DragKeyIdx     = -1;
    m_OnChangeCb     = nullptr;
    m_OnChangeUser   = nullptr;
}

/** 内部 state を全解放する。 */
void FAnimCurveEditorPanel::Shutdown() noexcept {
    m_Curve            = nullptr;
    m_SelectedKeyIdx = kNoKeySelected;
    m_Dirty            = false;
    m_SelectedEasingPreset =
        static_cast<u8>(Easing::EEasingType::Linear);
    m_DragKind        = 0u;
    m_DragKeyIdx     = -1;
    m_OnChangeCb     = nullptr;
    m_OnChangeUser   = nullptr;
}

/** 編集対象の curve をセットし selection / dirty をリセットする。 */
void FAnimCurveEditorPanel::SetCurve(FAnimationCurve* curve) noexcept {
    m_Curve            = curve;
    m_SelectedKeyIdx = kNoKeySelected;
    m_Dirty            = false;
    m_DragKind        = 0u;
    m_DragKeyIdx     = -1;
}

/** 現在バインド中の curve を返す。 */
FAnimationCurve* FAnimCurveEditorPanel::CurrentCurve() const noexcept {
    return m_Curve;
}

/** dirty flag を返す。 */
bool FAnimCurveEditorPanel::IsDirty() const noexcept {
    return m_Dirty;
}

/** dirty flag を false に戻す。 */
void FAnimCurveEditorPanel::ClearDirty() noexcept {
    m_Dirty = false;
}

/** 変更通知 callback と user ポインタを登録する。 */
void FAnimCurveEditorPanel::SetOnChangeCallback(CurveChangeCallback cb,
                                               void* user) noexcept {
    m_OnChangeCb   = cb;
    m_OnChangeUser = user;
}

/** dirty を立て、immediate なら callback を即時発火する。 */
void FAnimCurveEditorPanel::NotifyChanged(bool immediate) noexcept {
    m_Dirty = true;
    if (immediate && m_OnChangeCb != nullptr) {
        m_OnChangeCb(m_OnChangeUser, m_Curve);
    }
}

/** ImGui で toolbar + canvas を描画する。 */
void FAnimCurveEditorPanel::DrawUI() noexcept {
    if (!IsVisible()) return;

    if (!ImGui::Begin(Title(), &m_Visible)) {
        ImGui::End();
        return;
    }

    if (m_Curve == nullptr) {
        ImGui::TextDisabled("(No curve bound)");
        ImGui::TextDisabled("Call SetCurve(&curve) on this panel to start editing.");
        ImGui::End();
        return;
    }

    FAnimationCurve& curve = *m_Curve;

    // Toolbar 1: Interpolation Combo (= 選択中 key の out_interp を切替)。
    // 選択中 key の現在 interp を combo の初期値にする。未選択時は disable。
    {
        ImGui::TextUnformatted("Interp:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        const bool has_sel = (m_SelectedKeyIdx >= 0)
                          && (static_cast<u32>(m_SelectedKeyIdx) < curve.KeyCount());
        ECurveInterpolation cur = ECurveInterpolation::Linear;
        if (has_sel) {
            const FCurveKey* k = curve.Key(static_cast<u32>(m_SelectedKeyIdx));
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
                        const u32 idx = static_cast<u32>(m_SelectedKeyIdx);
                        const FCurveKey* k = curve.Key(idx);
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

    // Toolbar 2: WrapMode Combo (Pre / Post 個別)。
    ImGui::SameLine();
    ImGui::TextUnformatted("Pre:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(95.0f);
    {
        const FAnimationCurve::EWrapMode cur = curve.PreWrap();
        if (ImGui::BeginCombo("##pre_wrap", WrapName(cur))) {
            const FAnimationCurve::EWrapMode kAll[3] = {
                FAnimationCurve::EWrapMode::Clamp,
                FAnimationCurve::EWrapMode::Loop,
                FAnimationCurve::EWrapMode::PingPong,
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
        const FAnimationCurve::EWrapMode cur = curve.PostWrap();
        if (ImGui::BeginCombo("##post_wrap", WrapName(cur))) {
            const FAnimationCurve::EWrapMode kAll[3] = {
                FAnimationCurve::EWrapMode::Clamp,
                FAnimationCurve::EWrapMode::Loop,
                FAnimationCurve::EWrapMode::PingPong,
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

    // Toolbar 3: Add Key / Clear。
    ImGui::SameLine();
    if (ImGui::Button("Add Key")) {
        // 「現在の curve の中点 time」あたりに新 key を追加するのが直感的。
        // 末尾 key.time / 2 を使う。0 key なら time=0.5, 1 key なら same+0.5。
        const u32 nk = curve.KeyCount();
        f32 new_t = 0.5f;
        if (nk >= 1u) {
            const FCurveKey* last = curve.Key(nk - 1u);
            if (last != nullptr) new_t = last->time + 0.5f;
            if (nk >= 2u) {
                const FCurveKey* first = curve.Key(0);
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
            const FCurveKey* k = curve.Key(i);
            if (k != nullptr && k->time == new_t) {
                m_SelectedKeyIdx = static_cast<i32>(i);
                break;
            }
        }
        NotifyChanged(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        curve.ClearKeys();
        m_SelectedKeyIdx = kNoKeySelected;
        NotifyChanged(true);
    }

    // Toolbar 4: 全 easing catalog から編集可能な curve preset を生成する。
    {
        Easing::EEasingType selected = static_cast<Easing::EEasingType>(
            m_SelectedEasingPreset);
        if (Easing::GetFunction(selected) == nullptr) {
            selected = Easing::EEasingType::Linear;
            m_SelectedEasingPreset = static_cast<u8>(selected);
        }

        ImGui::SameLine();
        ImGui::TextUnformatted("Preset:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(125.0f);
        if (ImGui::BeginCombo("##easing_preset", Easing::GetName(selected))) {
            const u32 count =
                static_cast<u32>(Easing::EEasingType::Count);
            for (u32 index = 0u; index < count; ++index) {
                const Easing::EEasingType type =
                    static_cast<Easing::EEasingType>(index);
                const bool is_selected = type == selected;
                if (ImGui::Selectable(Easing::GetName(type), is_selected)) {
                    selected = type;
                    m_SelectedEasingPreset = static_cast<u8>(type);
                }
                if (is_selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply Preset")) {
            const FAnimationCurveResult preset_result =
                curve.TrySetEasingPreset(selected, 65u);
            if (preset_result.Succeeded()) {
                m_SelectedKeyIdx = kNoKeySelected;
                NotifyChanged(true);
            }
        }
    }

    // Toolbar 5: Eval preview slider (= 現在時刻でサンプルした値表示)。
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

    // Canvas — curve / key marker / tangent handle を描画 + drag 処理。
    const FCurveBounds bounds = ComputeBounds(&curve);

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
        const FCurveKey* k = curve.Key(0);
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
    // m_DragKind が 0 (= 未 drag) のときだけ判定する (drag 中は既存 m_DragKind を維持)。
    if (canvas_hovered && m_DragKind == 0u
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        i32 best_idx = -1;
        u8  best_kind = 0u;
        f32 best_dist2 = hit_radius * hit_radius;
        for (u32 i = 0; i < nk; ++i) {
            const FCurveKey* k = curve.Key(i);
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
            m_SelectedKeyIdx = best_idx;
            m_DragKind        = best_kind;
            m_DragKeyIdx     = best_idx;
        }
    }

    // drag 継続中の処理 (drag_kind != 0 のとき)。
    if (m_DragKind != 0u && m_DragKeyIdx >= 0
        && static_cast<u32>(m_DragKeyIdx) < curve.KeyCount()) {
        const u32 didx = static_cast<u32>(m_DragKeyIdx);
        const FCurveKey* k_const = curve.Key(didx);
        if (k_const != nullptr) {
            if (m_DragKind == 1u) {
                // key 本体: マウス位置を curve 空間に逆変換して time/value 上書き。
                f32 new_t = 0.0f, new_v = 0.0f;
                CanvasToCurve(mouse, bounds, canvas_origin, canvas_size, new_t, new_v);
                // time は前後 key とのオーダー衝突を回避するため隣接 key の time
                // で clamp する (= 重複 time にならないよう微小 epsilon)。
                const f32 eps = 0.0001f;
                f32 lo = -1e9f, hi = 1e9f;
                if (didx > 0u) {
                    const FCurveKey* prev = curve.Key(didx - 1u);
                    if (prev != nullptr) lo = prev->time + eps;
                }
                if (didx + 1u < curve.KeyCount()) {
                    const FCurveKey* nxt = curve.Key(didx + 1u);
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
                    m_SelectedKeyIdx = new_idx;
                    m_DragKeyIdx     = new_idx;
                }
            } else if (m_DragKind == 2u) {
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
            } else if (m_DragKind == 3u) {
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
            m_Dirty = true;
        }
    }

    // マウス release で drag 終了 + callback 1 回。
    if (m_DragKind != 0u && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (m_OnChangeCb != nullptr) {
            m_OnChangeCb(m_OnChangeUser, &curve);
        }
        m_DragKind    = 0u;
        m_DragKeyIdx = -1;
    }

    // key marker を全描画 (drag 後の最新状態を反映)。
    for (u32 i = 0; i < curve.KeyCount(); ++i) {
        const FCurveKey* k = curve.Key(i);
        if (k == nullptr) continue;
        const ImVec2 kp = CurveToCanvas(k->time, k->value, bounds,
                                        canvas_origin, canvas_size);
        const bool is_sel = (m_SelectedKeyIdx == static_cast<i32>(i));
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
                const FCurveKey* k = curve.Key(i);
                if (k != nullptr && k->time == ctx_t) {
                    m_SelectedKeyIdx = static_cast<i32>(i);
                    break;
                }
            }
            NotifyChanged(true);
        }
        const bool has_sel = (m_SelectedKeyIdx >= 0)
                          && (static_cast<u32>(m_SelectedKeyIdx) < curve.KeyCount());
        ImGui::BeginDisabled(!has_sel);
        if (ImGui::MenuItem("Delete Selected Key")) {
            if (has_sel) {
                curve.RemoveKey(static_cast<u32>(m_SelectedKeyIdx));
                m_SelectedKeyIdx = kNoKeySelected;
                NotifyChanged(true);
            }
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    // 選択 key の情報を canvas 下にテキスト表示 (= デバッグ用 + 視認性向上)。
    if (m_SelectedKeyIdx >= 0
        && static_cast<u32>(m_SelectedKeyIdx) < curve.KeyCount()) {
        const FCurveKey* k = curve.Key(static_cast<u32>(m_SelectedKeyIdx));
        if (k != nullptr) {
            ImGui::TextDisabled("Key %d: t=%.3f v=%.3f  in_tan=%.3f out_tan=%.3f  interp=%s",
                                static_cast<int>(m_SelectedKeyIdx),
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
