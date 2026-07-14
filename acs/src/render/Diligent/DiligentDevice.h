// SPDX-License-Identifier: Apache-2.0
// Diligent Engine 経由の RHI デバイス実装
// IRhiDevice を継承し、内部で IRenderDevice / IDeviceContext / EngineFactory を保持。
//
// 設計方針:
//   - Diligent の重い型 (RefCntAutoPtr) はヘッダに漏らさず、.cpp で扱う
//   - ヘッダはバックエンド非依存で完結。ユーザーコードはこのヘッダを include しなくて良い
//     （CreateRhiDevice() 経由で IRhiDevice ハンドルだけ受け取る）
#pragma once

#include "render/IRhiDevice.h"
#include "memory/UniquePtr.h"

// 前方宣言（DiligentCommon.h を漏らさない）
namespace Diligent {
struct IEngineFactory;
struct IEngineFactoryD3D12;
struct IEngineFactoryVk;
struct IRenderDevice;
struct IDeviceContext;
struct IFence;
} // namespace Diligent

namespace acs {

/**
 * Diligent Engine 経由の RHI デバイス実装 (D3D12 / Vulkan 両対応)。
 *
 * @details
 * IRhiDevice を継承し、内部で IRenderDevice / IDeviceContext / EngineFactory を保持する。
 * Diligent の重い型 (RefCntAutoPtr) はヘッダに漏らさず生ポインタで保持し、デストラクタで
 * 明示的に Release() する。バックエンド非依存で完結するため、ユーザーコードは CreateRhiDevice()
 * 経由で IRhiDevice ハンドルだけを受け取り、このヘッダを include する必要はない。
 */
class DiligentDevice final : public IRhiDevice {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    DiligentDevice() noexcept = default;

    /** Diligent オブジェクトを Release して破棄する。 */
    ~DiligentDevice() noexcept override;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    DiligentDevice(const DiligentDevice&) = delete;

    /** コピー代入も禁止。 */
    DiligentDevice& operator=(const DiligentDevice&) = delete;

    /**
     * デバイスを初期化する (CreateRhiDevice から呼ばれる)。
     *
     * @details configuration.backend に応じて D3D12 / Vulkan の経路を選ぶ (Auto は D3D12 を優先)。
     * @param configuration バックエンド種別・デバッグレイヤ等のデバイス作成オプション。
     * @return 成功なら空の TResult、初期化失敗ならエラー。
     */
    TResult<void> Init(const DeviceConfig& configuration) noexcept;

    /**
     * バックエンド名を返す。
     *
     * @return "Diligent-D3D12" / "Diligent-Vulkan" 等の文字列。
     */
    const char* BackendName() const noexcept override
    {
        return m_BackendName;
    }

    /**
     * 選択された GPU アダプタ名を返す。
     *
     * @return アダプタの説明文字列 (デバッグ表示用)。
     */
    const char* AdapterName() const noexcept override
    {
        return m_AdapterName;
    }

    /** GPU の処理完了を待つ (フェンス Signal + WaitForIdle、Shutdown 前等に呼ぶ)。 */
    void WaitIdle() noexcept override;

    /**
     * テクスチャ内容を CPU へ読み戻す (staging texture 経由、遅い・同期)。
     *
     * @details USAGE_STAGING + CPU_ACCESS_READ の一時テクスチャへ CopyTexture → Flush →
     * WaitIdle → MapTextureSubresource で取り出す。行は stride 詰めして out へ密に書く。
     * destination_size はテクスチャの密サイズ (w*h*bpp) 以上が必要。任意フォーマット対応 (bpp は format から算出)。
     * @param texture 読み戻すテクスチャ。
     * @param destination_pixels 書き込み先 (密 row-major)。
     * @param destination_size destination_pixels のバイト数。
     * @return 成功で true。
     */
    bool ReadTexture(IRhiTexture& texture, void* destination_pixels, u32 destination_size) noexcept override;

    /**
     * 実際に選ばれたバックエンド種別を返す。
     *
     * @return Init で確定した ERhiBackendKind (D3D12 / Vulkan)。
     */
    ERhiBackendKind ActualBackend() const noexcept
    {
        return m_ActualBackend;
    }

    /**
     * 汎用の EngineFactory を返す (Vulkan / D3D12 共通の基底型)。
     *
     * @return IEngineFactory へのポインタ (m_Factory または m_FactoryVk と同じ実体)。
     */
    Diligent::IEngineFactory* FactoryGeneric() const noexcept
    {
        return m_FactoryGeneric;
    }

    /**
     * D3D12 用の EngineFactory を返す。
     *
     * @return D3D12 経路の IEngineFactoryD3D12 (Vulkan 経路なら nullptr)。
     */
    Diligent::IEngineFactoryD3D12* Factory() const noexcept
    {
        return m_Factory;
    }

    /**
     * Vulkan 用の EngineFactory を返す。
     *
     * @return Vulkan 経路の IEngineFactoryVk (D3D12 経路なら nullptr)。
     */
    Diligent::IEngineFactoryVk* FactoryVk() const noexcept
    {
        return m_FactoryVk;
    }

    /**
     * レンダーデバイスを返す (リソース生成に使う)。
     *
     * @return Diligent の IRenderDevice。
     */
    Diligent::IRenderDevice* RenderDev() const noexcept
    {
        return m_Device;
    }

    /**
     * デバイスコンテキスト (即時コマンドキュー) を返す。
     *
     * @return Diligent の IDeviceContext。
     */
    Diligent::IDeviceContext* Context() const noexcept
    {
        return m_Context;
    }

    /**
     * グラフィックスキューにフェンス Signal を積み、その値を返す。
     *
     * @details ExecuteCommandLists/Flush の後に呼ぶ想定で、CommandList::Submit から使う。
     * @return Signal したフェンス値 (未初期化なら 0)。
     */
    u64 SignalGraphicsQueue() noexcept;

    /**
     * 指定フェンス値の完了を待つ。
     *
     * @param fence_value 待機対象のフェンス値。
     */
    void WaitForFenceValue(u64 fence_value) noexcept;

    /** 同時に in-flight にできるフレーム数。 */
    static constexpr u32 kFramesInFlight = 2;

    /**
     * 現在のフレームスロットを返す。
     *
     * @return 0..kFramesInFlight-1 のフレームスロット番号。
     */
    u32 CurrentFrameSlot() const noexcept
    {
        return m_FrameSlot;
    }

    /** フレームスロットを次へ進める (kFramesInFlight で循環)。 */
    void AdvanceFrameSlot() noexcept
    {
        m_FrameSlot = (m_FrameSlot + 1) % kFramesInFlight;
    }

private:
    /** 所有中または初期化途中の Diligent 資源を解放し、再初期化可能な空状態へ戻す。 */
    void Reset() noexcept;

    /**
     * D3D12 バックエンドでデバイスを初期化する。
     *
     * @param configuration デバイス作成オプション。
     * @return 成功なら空の TResult、初期化失敗ならエラー。
     */
    TResult<void> InitD3D12(const DeviceConfig& configuration) noexcept;

    /**
     * Vulkan バックエンドでデバイスを初期化する。
     *
     * @param configuration デバイス作成オプション。
     * @return 成功なら空の TResult、初期化失敗または未ビルドならエラー。
     */
    TResult<void> InitVulkan(const DeviceConfig& configuration) noexcept;

    /** 汎用の EngineFactory (m_Factory or m_FactoryVk と同じ実体)。 */
    Diligent::IEngineFactory* m_FactoryGeneric = nullptr;

    /** D3D12 経路の EngineFactory。 */
    Diligent::IEngineFactoryD3D12* m_Factory = nullptr;

    /** Vulkan 経路の EngineFactory。 */
    Diligent::IEngineFactoryVk* m_FactoryVk = nullptr;

    /** レンダーデバイス (リソース生成元)。 */
    Diligent::IRenderDevice* m_Device = nullptr;

    /** 即時コマンドキューを持つデバイスコンテキスト。 */
    Diligent::IDeviceContext* m_Context = nullptr;

    /** WaitIdle / フレーム同期に使う汎用フェンス。 */
    Diligent::IFence* m_IdleFence = nullptr;

    /** 直近に Signal したフェンス値 (単調増加)。 */
    u64 m_IdleValue = 0;

    /** 現在のフレームスロット (0..kFramesInFlight-1)。 */
    u32 m_FrameSlot = 0;

    /** 実際に選ばれたバックエンド種別。 */
    ERhiBackendKind m_ActualBackend = ERhiBackendKind::Auto;

    /** 選択された GPU アダプタ名。 */
    char m_AdapterName[128]{};

    /** バックエンド名 (Init で確定するまでの既定値 "Diligent")。 */
    const char* m_BackendName = "Diligent";
};

} // namespace acs
