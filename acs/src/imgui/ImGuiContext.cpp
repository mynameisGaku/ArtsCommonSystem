// SPDX-License-Identifier: Apache-2.0
// ImGui を ACS Window + Renderer に統合する薄いラッパ実装
#include "imgui/ImGuiContext.h"
#include "platform/Window.h"
#include "render/Renderer.h"
#include "render/Dx12/Dx12Common.h"
#include "render/Dx12/Dx12Device.h"
#include "render/Dx12/Dx12Swapchain.h"
#include "foundation/Log.h"

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>

// ImGui の Win32 backend が提供するメッセージプロシジャ転送関数
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace acs {

ImGuiCtx::~ImGuiCtx() noexcept {
    Shutdown();
}

Result<void> ImGuiCtx::Init(Window& window, Renderer& renderer) noexcept {
    _window = &window;
    _renderer = &renderer;

    // ImGui コンテキスト作成 + キーボード/ナビゲーション有効化
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // Win32 backend 初期化（HWND を渡す）
    HWND hwnd = static_cast<HWND>(window.NativeHandle());
    if (!ImGui_ImplWin32_Init(hwnd)) {
        return ACS_ERR(Render, 100, "ImGui_ImplWin32_Init failed");
    }

    // DX12 backend に必要な SRV ヒープを作成（フォントテクスチャ等を置く場所）
    Dx12Device* dev = static_cast<Dx12Device*>(renderer.Device());
    Dx12Swapchain* sc = static_cast<Dx12Swapchain*>(renderer.Swapchain());
    if (!dev || !sc) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return ACS_ERR(Render, 101, "ImGuiCtx::Init: Renderer not initialized");
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 64;  // フォント + 任意のユーザーテクスチャ用余裕
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* srv_heap = nullptr;
    if (FAILED(dev->D3DDevice()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap)))) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return ACS_ERR(Render, 102, "ImGui SRV heap create failed");
    }
    _srv_heap = srv_heap;

    // DX12 backend 初期化
    if (!ImGui_ImplDX12_Init(
            dev->D3DDevice(),
            static_cast<int>(sc->BufferCount()),
            DXGI_FORMAT_B8G8R8A8_UNORM,
            srv_heap,
            srv_heap->GetCPUDescriptorHandleForHeapStart(),
            srv_heap->GetGPUDescriptorHandleForHeapStart())) {
        srv_heap->Release();
        _srv_heap = nullptr;
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return ACS_ERR(Render, 103, "ImGui_ImplDX12_Init failed");
    }

    _initialized = true;
    ACS_LOG_INFO("ImGui initialized (Win32 + DX12)");
    return Ok();
}

void ImGuiCtx::Shutdown() noexcept {
    if (!_initialized) return;
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (_srv_heap) {
        static_cast<ID3D12DescriptorHeap*>(_srv_heap)->Release();
        _srv_heap = nullptr;
    }
    _initialized = false;
}

void ImGuiCtx::NewFrame() noexcept {
    if (!_initialized) return;
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiCtx::Render() noexcept {
    if (!_initialized || !_renderer) return;
    ImGui::Render();

    // 現在のコマンドリストに ImGui の描画コマンドを発行
    auto* cmd_list = static_cast<ID3D12GraphicsCommandList*>(
        _renderer->CommandList()->NativeHandle());
    if (!cmd_list) return;

    // ImGui は SRV ヒープをバインドする必要がある
    ID3D12DescriptorHeap* heaps[] = { static_cast<ID3D12DescriptorHeap*>(_srv_heap) };
    cmd_list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd_list);
}

// Window のイベントを ImGui に転送（Application::OnEvent から呼ぶ）
void ImGuiCtx::OnEvent(const Event& e) noexcept {
    if (!_initialized || !_window) return;
    // ImGui の Win32 backend は WndProc 経由でメッセージを受け取る設計。
    // ACS は独自イベントを使っているため、ここで Win32 メッセージに復元するか、
    // ImGui の IO に直接書き込む必要がある。
    // 簡易対応: ImGui_ImplWin32_WndProcHandler を呼ぶには HWND/UINT/WPARAM/LPARAM が
    // 必要なので、ここでは IO に直接設定する形にフォールバック。
    ImGuiIO& io = ImGui::GetIO();
    switch (e.type) {
        case EventType::KeyPressed:
        case EventType::KeyRepeat:
            // ACS の Key と ImGui の Key は別マッピング — 主要キーのみ対応
            // 完全対応は v2（VK→ImGuiKey 変換テーブル追加）
            break;
        case EventType::KeyReleased:
            break;
        case EventType::MouseButtonPressed:
            io.AddMouseButtonEvent(static_cast<int>(e.mouse_button.button), true);
            break;
        case EventType::MouseButtonReleased:
            io.AddMouseButtonEvent(static_cast<int>(e.mouse_button.button), false);
            break;
        case EventType::MouseMoved:
            io.AddMousePosEvent(e.mouse_move.x, e.mouse_move.y);
            break;
        case EventType::MouseScrolled:
            io.AddMouseWheelEvent(e.mouse_scroll.x, e.mouse_scroll.y);
            break;
        case EventType::CharInput:
            io.AddInputCharacter(e.char_input.codepoint);
            break;
        default: break;
    }
}

} // namespace acs
