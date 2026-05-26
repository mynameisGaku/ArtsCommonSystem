// SPDX-License-Identifier: Apache-2.0
// ImGui を ACS Window + Renderer に統合する薄いラッパ
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

class Window;
class Renderer;

// ImGui ラッパ（Win32 + DX12 backend を組み合わせる）
class ImGuiCtx {
public:
    ImGuiCtx() noexcept = default;
    ~ImGuiCtx() noexcept;

    ImGuiCtx(const ImGuiCtx&) = delete;
    ImGuiCtx& operator=(const ImGuiCtx&) = delete;

    // 初期化（Window と Renderer に紐付け）
    TResult<void> Init(Window& window, Renderer& renderer) noexcept;

    // 解放
    void Shutdown() noexcept;

    // フレーム開始（毎フレーム最初に呼ぶ。BeginFrame の後）
    void NewFrame() noexcept;

    // 描画（毎フレーム終わりに呼ぶ。EndFrame の前）
    void Render() noexcept;

    // ウィンドウイベントを ImGui に転送（Application::OnEvent で呼ぶ）
    void OnEvent(const Event& e) noexcept;

private:
    Window*    _window    = nullptr;
    Renderer*  _renderer  = nullptr;
    void*      _srv_heap  = nullptr;  // ID3D12DescriptorHeap*
    bool       _initialized = false;
};

} // namespace acs
