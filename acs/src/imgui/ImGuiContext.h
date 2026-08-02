// SPDX-License-Identifier: Apache-2.0
// ImGui を ACS FWindow + CRenderer に統合する薄いラッパ
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
//       if (!renderer.EndFrame()) break;
//   }
//   imgui.Shutdown();
#pragma once

#include "foundation/Result.h"
#include "platform/Event.h"

namespace acs {

class FWindow;
class CRenderer;

/**
 * ImGui を ACS の FWindow + CRenderer に統合する薄いラッパ。
 *
 * @details
 * Win32 backend (imgui_impl_win32) と DX12 backend (imgui_impl_dx12) を組み合わせ、
 * ImGui コンテキスト・SRV ディスクリプタヒープ・各 backend のライフサイクルを 1 つに
 * まとめて管理する。標準フローは Init → 毎フレーム NewFrame/Render → Shutdown。
 */
class FImGuiCtx {
public:
    /** 空状態で構築する (実体は Init で確保)。 */
    FImGuiCtx() noexcept = default;

    /** Shutdown を呼んで backend とコンテキストを破棄する。 */
    ~FImGuiCtx() noexcept;

    /** コピー禁止 (ImGui コンテキスト・GPU リソースを単独所有するため)。 */
    FImGuiCtx(const FImGuiCtx&) = delete;

    /** コピー代入も禁止。 */
    FImGuiCtx& operator=(const FImGuiCtx&) = delete;

    /**
     * ImGui コンテキストと Win32/DX12 backend を初期化する。
     *
     * @details
     * ImGui コンテキストを作ってキーボードナビゲーションを有効化し、Win32 backend に
     * HWND を渡し、DX12 backend 用の SHADER_VISIBLE な SRV ヒープ (64 ディスクリプタ) を
     * 確保して紐付ける。CRenderer が未初期化 (Device/Swapchain が null) の場合は失敗する。
     * @param window 紐付ける対象ウィンドウ (HWND を取得する)。
     * @param renderer DX12 デバイス・スワップチェインの取得元レンダラ。
     * @return 成功なら空の TResult、初期化失敗ならエラー。
     */
    TResult<void> Init(FWindow& window, CRenderer& renderer) noexcept;

    /** DX12/Win32 backend と ImGui コンテキスト・SRV ヒープを解放する (多重呼び出し安全)。 */
    void Shutdown() noexcept;

    /** 新しい ImGui フレームを開始する (毎フレーム最初、BeginFrame の後に呼ぶ)。 */
    void NewFrame() noexcept;

    /** 構築済み ImGui の描画コマンドを現在のコマンドリストへ発行する (毎フレーム終盤、EndFrame の前に呼ぶ)。 */
    void Render() noexcept;

    /**
     * ウィンドウイベントを ImGui の IO に転送する (CApplication::OnEvent から呼ぶ)。
     *
     * @details マウスのボタン・移動・スクロールと文字入力を ImGuiIO へ反映する (キー入力は未対応)。
     * @param e 転送する入力・ウィンドウイベント。
     */
    void OnEvent(const FEvent& e) noexcept;

private:
    /** 紐付け対象のウィンドウ (Init で受け取る)。 */
    FWindow*    m_Window    = nullptr;

    /** DX12 デバイス・スワップチェインの取得元レンダラ (Init で受け取る)。 */
    CRenderer*  m_Renderer  = nullptr;

    /** DX12 backend 用の SHADER_VISIBLE な SRV ヒープ (ID3D12DescriptorHeap*)。 */
    void*      m_SrvHeap  = nullptr;

    /** このラッパが所有する ImGuiContext。複数コンテキスト時の切替にも使う。 */
    void* m_Context = nullptr;

    /** Win32 backend の初期化に成功したか。部分失敗時の巻き戻し判定用。 */
    bool m_Win32Initialized = false;

    /** DX12 backend の初期化に成功したか。部分失敗時の巻き戻し判定用。 */
    bool m_Dx12Initialized = false;

    /** 初期化済みフラグ (NewFrame/Render/Shutdown のガードに使う)。 */
    bool       m_Initialized = false;
};

} // namespace acs
