// SPDX-License-Identifier: Apache-2.0
// ImGui を ACS FWindow + FRenderer に統合する薄いラッパ
//
// 使い方:
//   ImGuiCtx imgui;
//   imgui.Init(window, renderer);
//
//   while (!window.ShouldClose()) {
//       ...
//       renderer.BeginFrame(...);
//       imgui.NewFrame();
//       ImGui::Begin("Hello");
//       ImGui::Text("Hello, world!");
//       ImGui::End();
//       imgui.Render();           // 描画コマンドをコマンドリストに発行
//       renderer.EndFrame();
//   }
//   imgui.Shutdown();
#pragma once

#include "foundation/Result.h"
#include "platform/Event.h"

namespace acs {

class FWindow;
class FRenderer;

// ImGui ラッパ（Win32 + DX12 backend を組み合わせる）
class ImGuiCtx {
public:
    ImGuiCtx() noexcept = default;
    ~ImGuiCtx() noexcept;

    ImGuiCtx(const ImGuiCtx&) = delete;
    ImGuiCtx& operator=(const ImGuiCtx&) = delete;

    // 初期化（FWindow と FRenderer に紐付け）
    TResult<void> Init(FWindow& window, FRenderer& renderer) noexcept;

    // 解放
    void Shutdown() noexcept;

    // フレーム開始（毎フレーム最初に呼ぶ。BeginFrame の後）
    void NewFrame() noexcept;

    // 描画（毎フレーム終わりに呼ぶ。EndFrame の前）
    void Render() noexcept;

    // ウィンドウイベントを ImGui に転送（FApplication::OnEvent で呼ぶ）
    void OnEvent(const Event& e) noexcept;

private:
    FWindow*    m_Window    = nullptr;
    FRenderer*  m_Renderer  = nullptr;
    void*      m_SrvHeap  = nullptr;  // ID3D12DescriptorHeap*
    bool       m_Initialized = false;
};

} // namespace acs
