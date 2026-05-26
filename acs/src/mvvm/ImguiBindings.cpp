// SPDX-License-Identifier: Apache-2.0
// Mvvm × ImGui バインディング実装
#include "mvvm/ImguiBindings.h"
#include "container/String.h"

#if WITH_ACS_IMGUI

#include "imgui.h"
#include <cstdio>
#include <cstring>

namespace acs::mvvm::imgui {

void Bind(const char* label, FObservable<f32>& v, f32 v_min, f32 v_max) noexcept {
    f32 tmp = v.Get();
    if (ImGui::SliderFloat(label, &tmp, v_min, v_max)) {
        v.Set(tmp);
    }
}

void Bind(const char* label, FObservable<f64>& v, f64 v_min, f64 v_max) noexcept {
    f64 tmp = v.Get();
    if (ImGui::SliderScalar(label, ImGuiDataType_Double, &tmp, &v_min, &v_max)) {
        v.Set(tmp);
    }
}

void Bind(const char* label, FObservable<i32>& v, i32 v_min, i32 v_max) noexcept {
    i32 tmp = v.Get();
    if (ImGui::SliderInt(label, &tmp, v_min, v_max)) {
        v.Set(tmp);
    }
}

void Bind(const char* label, FObservable<u32>& v, u32 v_min, u32 v_max) noexcept {
    u32 tmp = v.Get();
    if (ImGui::SliderScalar(label, ImGuiDataType_U32, &tmp, &v_min, &v_max)) {
        v.Set(tmp);
    }
}

void Bind(const char* label, FObservable<bool>& v) noexcept {
    bool tmp = v.Get();
    if (ImGui::FCheckbox(label, &tmp)) {
        v.Set(tmp);
    }
}

void BindReadOnly(const char* label, const FObservable<f32>& v) noexcept {
    ImGui::Text("%s: %.2f", label, v.Get());
}
void BindReadOnly(const char* label, const FObservable<i32>& v) noexcept {
    ImGui::Text("%s: %d", label, v.Get());
}
void BindReadOnly(const char* label, const FObservable<bool>& v) noexcept {
    ImGui::Text("%s: %s", label, v.Get() ? "true" : "false");
}

void BindProgress(const char* label, const FObservable<f32>& v, f32 v_min, f32 v_max) noexcept {
    f32 t = (v_max > v_min) ? (v.Get() - v_min) / (v_max - v_min) : 0.0f;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), "%.1f / %.1f", v.Get(), v_max);
    ImGui::ProgressBar(t, ImVec2(-1.0f, 0.0f), overlay);
    ImGui::SameLine();
    ImGui::Text("%s", label);
}

// ---- FVec2/FVec3 ----
void BindSlider2(const char* label, FObservable<FVec2>& v, f32 v_min, f32 v_max) noexcept {
    FVec2 tmp = v.Get();
    if (ImGui::SliderFloat2(label, &tmp.x, v_min, v_max)) {
        v.Set(tmp);
    }
}
void BindSlider3(const char* label, FObservable<FVec3>& v, f32 v_min, f32 v_max) noexcept {
    FVec3 tmp = v.Get();
    if (ImGui::SliderFloat3(label, &tmp.x, v_min, v_max)) {
        v.Set(tmp);
    }
}
void BindColor3(const char* label, FObservable<FVec3>& v) noexcept {
    FVec3 tmp = v.Get();
    if (ImGui::ColorEdit3(label, &tmp.x)) {
        v.Set(tmp);
    }
}

// ---- Combo ----
void BindCombo(const char* label, FObservable<i32>& v,
               const char* const* labels, u32 count) noexcept {
    if (!labels || count == 0) return;
    i32 idx = v.Get();
    if (idx < 0 || idx >= static_cast<i32>(count)) idx = 0;
    if (ImGui::BeginCombo(label, labels[idx])) {
        for (u32 i = 0; i < count; ++i) {
            bool selected = (static_cast<i32>(i) == idx);
            if (ImGui::Selectable(labels[i], selected)) {
                v.Set(static_cast<i32>(i));
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

// ---- Text 入力 ----
void BindText(const char* label, FObservable<acs::FString>& v,
              char* persistent, usize cap) noexcept {
    if (!persistent || cap == 0) return;
    if (ImGui::InputText(label, persistent, cap, ImGuiInputTextFlags_EnterReturnsTrue)) {
        v.Set(acs::FString{persistent});
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        v.Set(acs::FString{persistent});
    }
    // 編集中でなければ FObservable の最新値を buffer に同期 (外部からの変更反映)
    if (!ImGui::IsItemActive()) {
        const char* cur = v.Get().Data();
        if (cur && std::strncmp(persistent, cur, cap) != 0) {
            std::strncpy(persistent, cur, cap - 1);
            persistent[cap - 1] = 0;
        }
    }
}

// ---- BindFormat ----
template<typename T>
void BindFormat(const char* /*fmt*/, const FObservable<T>& /*v*/) noexcept {
    // generic 版は使われない (extern template で各型の specialization を使う)
}

template<>
void BindFormat<f32>(const char* fmt, const FObservable<f32>& v) noexcept {
    ImGui::Text(fmt, v.Get());
}
template<>
void BindFormat<f64>(const char* fmt, const FObservable<f64>& v) noexcept {
    ImGui::Text(fmt, v.Get());
}
template<>
void BindFormat<i32>(const char* fmt, const FObservable<i32>& v) noexcept {
    ImGui::Text(fmt, v.Get());
}
template<>
void BindFormat<u32>(const char* fmt, const FObservable<u32>& v) noexcept {
    ImGui::Text(fmt, v.Get());
}

// ---- FCommand (ボタン化 + 自動 grayout) ----
bool BindCommand(const char* label, FCommand& cmd) noexcept {
    bool can = cmd.CanExecute();
    if (!can) ImGui::BeginDisabled();
    bool clicked = ImGui::FButton(label);
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
void Bind(const char*, FObservable<f32>&, f32, f32) noexcept {}
void Bind(const char*, FObservable<f64>&, f64, f64) noexcept {}
void Bind(const char*, FObservable<i32>&, i32, i32) noexcept {}
void Bind(const char*, FObservable<u32>&, u32, u32) noexcept {}
void Bind(const char*, FObservable<bool>&) noexcept {}
void BindSlider2(const char*, FObservable<FVec2>&, f32, f32) noexcept {}
void BindSlider3(const char*, FObservable<FVec3>&, f32, f32) noexcept {}
void BindColor3(const char*, FObservable<FVec3>&) noexcept {}
void BindCombo(const char*, FObservable<i32>&, const char* const*, u32) noexcept {}
void BindText(const char*, FObservable<acs::FString>&, char*, usize) noexcept {}
template<> void BindFormat<f32>(const char*, const FObservable<f32>&) noexcept {}
template<> void BindFormat<f64>(const char*, const FObservable<f64>&) noexcept {}
template<> void BindFormat<i32>(const char*, const FObservable<i32>&) noexcept {}
template<> void BindFormat<u32>(const char*, const FObservable<u32>&) noexcept {}
bool BindCommand(const char*, FCommand&) noexcept { return false; }
void BindReadOnly(const char*, const FObservable<f32>&) noexcept {}
void BindReadOnly(const char*, const FObservable<i32>&) noexcept {}
void BindReadOnly(const char*, const FObservable<bool>&) noexcept {}
void BindProgress(const char*, const FObservable<f32>&, f32, f32) noexcept {}
} // namespace acs::mvvm::imgui

#endif
