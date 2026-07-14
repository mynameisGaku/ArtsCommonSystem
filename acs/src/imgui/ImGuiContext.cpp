// SPDX-License-Identifier: Apache-2.0
// ImGui を ACS FWindow + FRenderer に統合する薄いラッパ実装
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

/** Shutdown を呼んで backend とコンテキストを破棄する。 */
ImGuiCtx::~ImGuiCtx() noexcept {
    Shutdown();
}

/** ImGui コンテキストと Win32/DX12 backend を初期化する。 */
TResult<void> ImGuiCtx::Init(FWindow& window, FRenderer& renderer) noexcept {
    if (m_Initialized || m_Context != nullptr || m_SrvHeap != nullptr || m_Win32Initialized || m_Dx12Initialized) {
        return ACS_ERR(Render, 104, "ImGuiCtx::Init: already initialized");
    }

    HWND hwnd = static_cast<HWND>(window.NativeHandle());
    Dx12Device* dev = static_cast<Dx12Device*>(renderer.Device());
    Dx12Swapchain* sc = static_cast<Dx12Swapchain*>(renderer.Swapchain());
    if (hwnd == nullptr || dev == nullptr || sc == nullptr || dev->D3DDevice() == nullptr) {
        return ACS_ERR(Render, 101, "ImGuiCtx::Init: window or renderer not initialized");
    }

    ImGuiContext* const previous_context = ImGui::GetCurrentContext();

    // ImGui コンテキスト作成 + キーボード/ナビゲーション有効化
    IMGUI_CHECKVERSION();
    ImGuiContext* const context = ImGui::CreateContext();
    if (context == nullptr) {
        if (previous_context != nullptr) ImGui::SetCurrentContext(previous_context);
        return ACS_ERR(Render, 105, "ImGui::CreateContext failed");
    }
    m_Context = context;
    m_Window = &window;
    m_Renderer = &renderer;
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // Win32 backend 初期化（HWND を渡す）
    if (!ImGui_ImplWin32_Init(hwnd)) {
        Shutdown();
        if (previous_context != nullptr) ImGui::SetCurrentContext(previous_context);
        return ACS_ERR(Render, 100, "ImGui_ImplWin32_Init failed");
    }
    m_Win32Initialized = true;

    // DX12 backend に必要な SRV ヒープを作成（フォントテクスチャ等を置く場所）
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 64;  // フォント + 任意のユーザーテクスチャ用余裕
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* srv_heap = nullptr;
    if (FAILED(dev->D3DDevice()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap)))) {
        Shutdown();
        if (previous_context != nullptr) ImGui::SetCurrentContext(previous_context);
        return ACS_ERR(Render, 102, "ImGui SRV heap create failed");
    }
    m_SrvHeap = srv_heap;

    // DX12 backend 初期化
    if (!ImGui_ImplDX12_Init(
            dev->D3DDevice(),
            static_cast<int>(sc->BufferCount()),
            DXGI_FORMAT_B8G8R8A8_UNORM,
            srv_heap,
            srv_heap->GetCPUDescriptorHandleForHeapStart(),
            srv_heap->GetGPUDescriptorHandleForHeapStart())) {
        Shutdown();
        if (previous_context != nullptr) ImGui::SetCurrentContext(previous_context);
        return ACS_ERR(Render, 103, "ImGui_ImplDX12_Init failed");
    }
    m_Dx12Initialized = true;

    m_Initialized = true;
    ACS_LOG_INFO("ImGui initialized (Win32 + DX12)");
    return Ok();
}

/** DX12/Win32 backend と ImGui コンテキスト・SRV ヒープを解放する。 */
void ImGuiCtx::Shutdown() noexcept {
    ImGuiContext* const context = static_cast<ImGuiContext*>(m_Context);
    ImGuiContext* const previous_context = ImGui::GetCurrentContext();
    if (context != nullptr) ImGui::SetCurrentContext(context);

    if (m_Dx12Initialized && context != nullptr) ImGui_ImplDX12_Shutdown();
    if (m_Win32Initialized && context != nullptr) ImGui_ImplWin32_Shutdown();
    if (m_SrvHeap) {
        static_cast<ID3D12DescriptorHeap*>(m_SrvHeap)->Release();
        m_SrvHeap = nullptr;
    }
    if (context != nullptr) ImGui::DestroyContext(context);
    if (previous_context != nullptr && previous_context != context) {
        ImGui::SetCurrentContext(previous_context);
    }

    m_Context = nullptr;
    m_Window = nullptr;
    m_Renderer = nullptr;
    m_Dx12Initialized = false;
    m_Win32Initialized = false;
    m_Initialized = false;
}

/** 新しい ImGui フレームを開始する。 */
void ImGuiCtx::NewFrame() noexcept {
    if (!m_Initialized || m_Context == nullptr) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_Context));
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

/** 構築済み ImGui の描画コマンドを現在のコマンドリストへ発行する。 */
void ImGuiCtx::Render() noexcept {
    if (!m_Initialized || !m_Renderer || m_Context == nullptr) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_Context));
    ImGui::Render();

    // 現在のコマンドリストに ImGui の描画コマンドを発行。
    // CommandList() は BeginFrame 前や未初期化時に null を返しうる（m_Cmd.Get()）。
    // 旧コードは戻り値を直接 ->NativeHandle() で deref しており null 参照になるため、
    // ポインタ自体を先にガードしてから native handle を取得する。
    IRhiCommandList* rhi_cmd = m_Renderer->CommandList();
    if (!rhi_cmd) return;
    auto* cmd_list = static_cast<ID3D12GraphicsCommandList*>(rhi_cmd->NativeHandle());
    if (!cmd_list || !m_SrvHeap) return;

    // ImGui は SRV ヒープをバインドする必要がある
    ID3D12DescriptorHeap* heaps[] = { static_cast<ID3D12DescriptorHeap*>(m_SrvHeap) };
    cmd_list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd_list);
}

/** ウィンドウイベントを ImGui の IO に転送する。 */
void ImGuiCtx::OnEvent(const Event& e) noexcept {
    if (!m_Initialized || !m_Window || m_Context == nullptr) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_Context));
    // ImGui の Win32 backend は WndProc 経由でメッセージを受け取る設計。
    // ACS は独自イベントを使っているため、ここで Win32 メッセージに復元するか、
    // ImGui の IO に直接書き込む必要がある。
    // 簡易対応: ImGui_ImplWin32_WndProcHandler を呼ぶには HWND/UINT/WPARAM/LPARAM が
    // 必要なので、ここでは IO に直接設定する形にフォールバック。
    ImGuiIO& io = ImGui::GetIO();
    switch (e.type) {
        case EventType::KeyPressed:
        case EventType::KeyRepeat:
            // ACS の EKey と ImGui の EKey は別マッピング — 主要キーのみ対応
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
