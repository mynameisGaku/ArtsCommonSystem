// Mvvm × ImGui バインディング実装
#include "mvvm/ImguiBindings.h"

#if WITH_ACS_IMGUI

#include "imgui.h"
#include <cstdio>

namespace acs::mvvm::imgui {

void Bind(const char* label, Observable<f32>& v, f32 v_min, f32 v_max) noexcept {
    f32 tmp = v.Get();
    if (ImGui::SliderFloat(label, &tmp, v_min, v_max)) {
        v.Set(tmp);
    }
}

void Bind(const char* label, Observable<f64>& v, f64 v_min, f64 v_max) noexcept {
    f64 tmp = v.Get();
    if (ImGui::SliderScalar(label, ImGuiDataType_Double, &tmp, &v_min, &v_max)) {
        v.Set(tmp);
    }
}

void Bind(const char* label, Observable<i32>& v, i32 v_min, i32 v_max) noexcept {
    i32 tmp = v.Get();
    if (ImGui::SliderInt(label, &tmp, v_min, v_max)) {
        v.Set(tmp);
    }
}

void Bind(const char* label, Observable<u32>& v, u32 v_min, u32 v_max) noexcept {
    u32 tmp = v.Get();
    if (ImGui::SliderScalar(label, ImGuiDataType_U32, &tmp, &v_min, &v_max)) {
        v.Set(tmp);
    }
}

void Bind(const char* label, Observable<bool>& v) noexcept {
    bool tmp = v.Get();
    if (ImGui::Checkbox(label, &tmp)) {
        v.Set(tmp);
    }
}

void BindReadOnly(const char* label, const Observable<f32>& v) noexcept {
    ImGui::Text("%s: %.2f", label, v.Get());
}
void BindReadOnly(const char* label, const Observable<i32>& v) noexcept {
    ImGui::Text("%s: %d", label, v.Get());
}
void BindReadOnly(const char* label, const Observable<bool>& v) noexcept {
    ImGui::Text("%s: %s", label, v.Get() ? "true" : "false");
}

void BindProgress(const char* label, const Observable<f32>& v, f32 v_min, f32 v_max) noexcept {
    f32 t = (v_max > v_min) ? (v.Get() - v_min) / (v_max - v_min) : 0.0f;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), "%.1f / %.1f", v.Get(), v_max);
    ImGui::ProgressBar(t, ImVec2(-1.0f, 0.0f), overlay);
    ImGui::SameLine();
    ImGui::Text("%s", label);
}

} // namespace acs::mvvm::imgui

#else  // WITH_ACS_IMGUI

namespace acs::mvvm::imgui {
void Bind(const char*, Observable<f32>&, f32, f32) noexcept {}
void Bind(const char*, Observable<f64>&, f64, f64) noexcept {}
void Bind(const char*, Observable<i32>&, i32, i32) noexcept {}
void Bind(const char*, Observable<u32>&, u32, u32) noexcept {}
void Bind(const char*, Observable<bool>&) noexcept {}
void BindReadOnly(const char*, const Observable<f32>&) noexcept {}
void BindReadOnly(const char*, const Observable<i32>&) noexcept {}
void BindReadOnly(const char*, const Observable<bool>&) noexcept {}
void BindProgress(const char*, const Observable<f32>&, f32, f32) noexcept {}
} // namespace acs::mvvm::imgui

#endif
