// SPDX-License-Identifier: Apache-2.0
// Diligent Engine 経由の GPU バッファ
#pragma once

#include "render/IRhiBuffer.h"
#include "memory/UniquePtr.h"

namespace Diligent {
    struct IBuffer;
    struct IBufferView;
}

namespace acs {

class FDiligentDevice;

/**
 * Diligent Engine 経由で GPU バッファを実装する IRhiBuffer。
 *
 * @details
 * EBufferUsage に応じて vertex/index/uniform/structured/staging の bind flag と
 * Diligent::USAGE を選び、初期データ付きで作る。cpu_writable=false かつ初期データありの
 * 場合は USAGE_IMMUTABLE、それ以外は USAGE_DEFAULT (UpdateBuffer 経路)、Staging は
 * USAGE_STAGING (Map 経路) を使う。生の Diligent::IBuffer は参照カウントで所有する。
 */
class FDiligentBuffer final : public IRhiBuffer {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FDiligentBuffer() noexcept = default;

    /** 保持している Diligent バッファを解放する。 */
    ~FDiligentBuffer() noexcept override;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    FDiligentBuffer(const FDiligentBuffer&) = delete;

    /** コピー代入も禁止。 */
    FDiligentBuffer& operator=(const FDiligentBuffer&) = delete;

    /**
     * desc に従って GPU バッファを生成する。
     *
     * @details
     * usage から bind flag を、cpu_writable と初期データの有無から Diligent::USAGE を決める。
     * 初期データは USAGE_DEFAULT/IMMUTABLE では CreateBuffer 時に流し込み、Staging では
     * 生成後の Update で書き込む。
     * @param device バッファ生成に使う Diligent デバイス。
     * @param desc 生成するバッファの記述 (サイズ・用途・初期データ等)。
     * @return 成功なら空の TResult、デバイス未初期化や生成失敗ならエラー。
     */
    TResult<void> Init(FDiligentDevice& device, const FBufferDesc& desc) noexcept;

    /**
     * バッファのバイトサイズを返す。
     *
     * @return Init で渡された desc.size。
     */
    usize       Size()  const noexcept override { return m_Size; }

    /**
     * バッファの用途を返す。
     *
     * @return Init で渡された EBufferUsage。
     */
    EBufferUsage Usage() const noexcept override { return m_Usage; }

    /**
     * バッファの内容を更新する。
     *
     * @details
     * Staging は MapBuffer (MAP_FLAG_DISCARD) + memcpy、それ以外は UpdateBuffer で書き込む。
     * バッファ・デバイス・data のいずれかが null、または size が 0 のときは何もしない。
     * @param data 書き込むデータの先頭ポインタ。
     * @param size 書き込むバイト数。
     * @param offset バッファ先頭からの書き込みオフセット (既定 0)。
     */
    void        Update(const void* data, usize size, usize offset = 0) noexcept override;

    /**
     * 生の Diligent バッファを返す (内部公開)。
     *
     * @return 保持している Diligent::IBuffer (未生成なら nullptr)。
     */
    Diligent::IBuffer* Native() const noexcept { return m_Buffer; }

    /** 構造化バッファ SRV (struct_stride>0 のとき。compute の StructuredBuffer)。 */
    Diligent::IBufferView* SrvView() const noexcept { return m_Srv; }

    /** 構造化バッファ UAV (struct_stride>0 + Storage のとき。compute の RWStructuredBuffer)。 */
    Diligent::IBufferView* UavView() const noexcept { return m_Uav; }

private:
    /** 保持リソースと借用参照を空状態へ戻す。 */
    void Reset() noexcept;

    /** Update でコンテキストを引くために保持する生成元デバイス。 */
    FDiligentDevice*    m_Device = nullptr;

    /** 実体の Diligent バッファ (参照カウントで所有)。 */
    Diligent::IBuffer* m_Buffer = nullptr;

    /** バッファのバイトサイズ。 */
    usize              m_Size   = 0;

    /** バッファの用途 (bind flag と Update 経路の選択に使う)。 */
    EBufferUsage        m_Usage  = EBufferUsage::Vertex;

    /** 構造化バッファ SRV (m_Buffer 所有、Phase 0)。 */
    Diligent::IBufferView* m_Srv = nullptr;

    /** 構造化バッファ UAV (m_Buffer 所有、Phase 0)。 */
    Diligent::IBufferView* m_Uav = nullptr;
};

} // namespace acs
