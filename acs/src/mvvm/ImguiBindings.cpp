// SPDX-License-Identifier: Apache-2.0
// Mvvm × ImGui バインディング実装
#include "mvvm/ImguiBindings.h"
#include "container/String.h"

#if WITH_ACS_IMGUI

#include "imgui.h"
#include <cstdio>
#include <cstring>

namespace acs::mvvm::imgui {

/** f32 Observable をスライダ描画し、操作されたら Set で書き戻す。 */
void Bind(const char* label, TObservable<f32>& v, f32 v_min, f32 v_max) noexcept {
    f32 tmp = v.Get();
    if (ImGui::SliderFloat(label, &tmp, v_min, v_max)) {
        v.Set(tmp);
    }
}

/** f64 Observable をスライダ描画し、操作されたら Set で書き戻す。 */
void Bind(const char* label, TObservable<f64>& v, f64 v_min, f64 v_max) noexcept {
    f64 tmp = v.Get();
    if (ImGui::SliderScalar(label, ImGuiDataType_Double, &tmp, &v_min, &v_max)) {
        v.Set(tmp);
    }
}

/** i32 Observable をスライダ描画し、操作されたら Set で書き戻す。 */
void Bind(const char* label, TObservable<i32>& v, i32 v_min, i32 v_max) noexcept {
    i32 tmp = v.Get();
    if (ImGui::SliderInt(label, &tmp, v_min, v_max)) {
        v.Set(tmp);
    }
}

/** u32 Observable をスライダ描画し、操作されたら Set で書き戻す。 */
void Bind(const char* label, TObservable<u32>& v, u32 v_min, u32 v_max) noexcept {
    u32 tmp = v.Get();
    if (ImGui::SliderScalar(label, ImGuiDataType_U32, &tmp, &v_min, &v_max)) {
        v.Set(tmp);
    }
}

/** bool Observable をチェックボックス描画し、操作されたら Set で書き戻す。 */
void Bind(const char* label, TObservable<bool>& v) noexcept {
    bool tmp = v.Get();
    if (ImGui::Checkbox(label, &tmp)) {
        v.Set(tmp);
    }
}

/** f32 Observable の値を "label: value" の読み取り専用テキストで表示する。 */
void BindReadOnly(const char* label, const TObservable<f32>& v) noexcept {
    ImGui::Text("%s: %.2f", label, v.Get());
}

/** i32 Observable の値を "label: value" の読み取り専用テキストで表示する。 */
void BindReadOnly(const char* label, const TObservable<i32>& v) noexcept {
    ImGui::Text("%s: %d", label, v.Get());
}

/** bool Observable の値を "label: true/false" の読み取り専用テキストで表示する。 */
void BindReadOnly(const char* label, const TObservable<bool>& v) noexcept {
    ImGui::Text("%s: %s", label, v.Get() ? "true" : "false");
}

/** f32 Observable の値を [v_min, v_max] で正規化しプログレスバー + ラベル表示する。 */
void BindProgress(const char* label, const TObservable<f32>& v, f32 v_min, f32 v_max) noexcept {
    f32 t = (v_max > v_min) ? (v.Get() - v_min) / (v_max - v_min) : 0.0f;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), "%.1f / %.1f", v.Get(), v_max);
    ImGui::ProgressBar(t, ImVec2(-1.0f, 0.0f), overlay);
    ImGui::SameLine();
    ImGui::Text("%s", label);
}

/** FVec2 Observable を 2 成分スライダ描画し、操作されたら Set で書き戻す。 */
void BindSlider2(const char* label, TObservable<FVec2>& v, f32 v_min, f32 v_max) noexcept {
    FVec2 tmp = v.Get();
    if (ImGui::SliderFloat2(label, &tmp.x, v_min, v_max)) {
        v.Set(tmp);
    }
}

/** FVec3 Observable を 3 成分スライダ描画し、操作されたら Set で書き戻す。 */
void BindSlider3(const char* label, TObservable<FVec3>& v, f32 v_min, f32 v_max) noexcept {
    FVec3 tmp = v.Get();
    if (ImGui::SliderFloat3(label, &tmp.x, v_min, v_max)) {
        v.Set(tmp);
    }
}

/** FVec3 Observable を RGB カラーピッカー描画し、操作されたら Set で書き戻す。 */
void BindColor3(const char* label, TObservable<FVec3>& v) noexcept {
    FVec3 tmp = v.Get();
    if (ImGui::ColorEdit3(label, &tmp.x)) {
        v.Set(tmp);
    }
}

/** i32 Observable をコンボボックス描画し、選択されたら index を Set で書き戻す。 */
void BindCombo(const char* label, TObservable<i32>& v,
               const char* const* labels, u32 count) noexcept {
    if (!labels || count == 0) return;
    i32 idx = v.Get();
    if (idx < 0 || idx >= static_cast<i32>(count)) idx = 0;
    if (ImGui::BeginCombo(label, labels[idx])) {
        for (u32 i = 0; i < count; ++i) {
            const bool selected = (static_cast<i32>(i) == idx);
            if (ImGui::Selectable(labels[i], selected)) {
                v.Set(static_cast<i32>(i));
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

/** FString Observable をテキスト入力描画し、確定時に Set、非編集中は最新値を buffer へ同期する。 */
void BindText(const char* label, TObservable<acs::FString>& v,
              char* persistent, usize cap) noexcept {
    if (!persistent || cap == 0) return;
    if (ImGui::InputText(label, persistent, cap, ImGuiInputTextFlags_EnterReturnsTrue)) {
        v.Set(acs::FString{persistent});
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        v.Set(acs::FString{persistent});
    }
    // 編集中でなければ Observable の最新値を buffer に同期 (外部からの変更反映)
    if (!ImGui::IsItemActive()) {
        const char* cur = v.Get().Data();
        if (cur && std::strncmp(persistent, cur, cap) != 0) {
            std::strncpy(persistent, cur, cap - 1);
            persistent[cap - 1] = 0;
        }
    }
}

/** BindFormat の primary template (各型の特殊化が使われるため本体は空)。 */
template<typename T>
void BindFormat(const char* /*fmt*/, const TObservable<T>& /*v*/) noexcept {
    // generic 版は使われない (extern template で各型の specialization を使う)
}

/** f32 Observable の値を fmt で整形してテキスト表示する。 */
template<>
void BindFormat<f32>(const char* fmt, const TObservable<f32>& v) noexcept {
    ImGui::Text(fmt, v.Get());
}

/** f64 Observable の値を fmt で整形してテキスト表示する。 */
template<>
void BindFormat<f64>(const char* fmt, const TObservable<f64>& v) noexcept {
    ImGui::Text(fmt, v.Get());
}

/** i32 Observable の値を fmt で整形してテキスト表示する。 */
template<>
void BindFormat<i32>(const char* fmt, const TObservable<i32>& v) noexcept {
    ImGui::Text(fmt, v.Get());
}

/** u32 Observable の値を fmt で整形してテキスト表示する。 */
template<>
void BindFormat<u32>(const char* fmt, const TObservable<u32>& v) noexcept {
    ImGui::Text(fmt, v.Get());
}

/** FCommand をボタン描画し、実行可能ならクリックで Execute、不可なら grayout する。 */
bool BindCommand(const char* label, FCommand& cmd) noexcept {
    const bool can = cmd.CanExecute();
    if (!can) ImGui::BeginDisabled();
    const bool clicked = ImGui::Button(label);
    if (!can) ImGui::EndDisabled();
    if (clicked && can) {
        cmd.Execute();
        return true;
    }
    return false;
}

} // namespace acs::mvvm::imgui

#else  // WITH_ACS_IMGUI

namespace acs::mvvm::imgui {

// WITH_ACS_IMGUI が無効なビルド向けの no-op スタブ群。
// 呼び出し側のコードを変えずにリンクできるよう、全 API を空実装で提供する。

/** ImGui 無効時の Bind(f32) スタブ (何もしない)。 */
void Bind(const char*, TObservable<f32>&, f32, f32) noexcept {}

/** ImGui 無効時の Bind(f64) スタブ (何もしない)。 */
void Bind(const char*, TObservable<f64>&, f64, f64) noexcept {}

/** ImGui 無効時の Bind(i32) スタブ (何もしない)。 */
void Bind(const char*, TObservable<i32>&, i32, i32) noexcept {}

/** ImGui 無効時の Bind(u32) スタブ (何もしない)。 */
void Bind(const char*, TObservable<u32>&, u32, u32) noexcept {}

/** ImGui 無効時の Bind(bool) スタブ (何もしない)。 */
void Bind(const char*, TObservable<bool>&) noexcept {}

/** ImGui 無効時の BindSlider2 スタブ (何もしない)。 */
void BindSlider2(const char*, TObservable<FVec2>&, f32, f32) noexcept {}

/** ImGui 無効時の BindSlider3 スタブ (何もしない)。 */
void BindSlider3(const char*, TObservable<FVec3>&, f32, f32) noexcept {}

/** ImGui 無効時の BindColor3 スタブ (何もしない)。 */
void BindColor3(const char*, TObservable<FVec3>&) noexcept {}

/** ImGui 無効時の BindCombo スタブ (何もしない)。 */
void BindCombo(const char*, TObservable<i32>&, const char* const*, u32) noexcept {}

/** ImGui 無効時の BindText スタブ (何もしない)。 */
void BindText(const char*, TObservable<acs::FString>&, char*, usize) noexcept {}

/** ImGui 無効時の BindFormat<f32> スタブ (何もしない)。 */
template<> void BindFormat<f32>(const char*, const TObservable<f32>&) noexcept {}

/** ImGui 無効時の BindFormat<f64> スタブ (何もしない)。 */
template<> void BindFormat<f64>(const char*, const TObservable<f64>&) noexcept {}

/** ImGui 無効時の BindFormat<i32> スタブ (何もしない)。 */
template<> void BindFormat<i32>(const char*, const TObservable<i32>&) noexcept {}

/** ImGui 無効時の BindFormat<u32> スタブ (何もしない)。 */
template<> void BindFormat<u32>(const char*, const TObservable<u32>&) noexcept {}

/** ImGui 無効時の BindCommand スタブ (常に false を返す)。 */
bool BindCommand(const char*, FCommand&) noexcept { return false; }

/** ImGui 無効時の BindReadOnly(f32) スタブ (何もしない)。 */
void BindReadOnly(const char*, const TObservable<f32>&) noexcept {}

/** ImGui 無効時の BindReadOnly(i32) スタブ (何もしない)。 */
void BindReadOnly(const char*, const TObservable<i32>&) noexcept {}

/** ImGui 無効時の BindReadOnly(bool) スタブ (何もしない)。 */
void BindReadOnly(const char*, const TObservable<bool>&) noexcept {}

/** ImGui 無効時の BindProgress スタブ (何もしない)。 */
void BindProgress(const char*, const TObservable<f32>&, f32, f32) noexcept {}

} // namespace acs::mvvm::imgui

#endif
