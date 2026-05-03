// GPU バッファ抽象（頂点・インデックス・定数バッファ用）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/RhiTypes.h"

namespace acs {

class IRhiDevice;

// バッファの用途
enum class BufferUsage : u8 {
    Vertex,    // 頂点バッファ
    Index16,   // 16-bit インデックスバッファ
    Index32,   // 32-bit インデックスバッファ
    Uniform,   // 定数バッファ (CB)
    Storage,   // UAV / SSBO
    Staging,   // CPU からのアップロード用
};

struct BufferDesc {
    usize       size         = 0;
    BufferUsage usage        = BufferUsage::Vertex;
    bool        cpu_writable = false;     // 動的更新したいなら true
    const void* initial_data = nullptr;   // 初期データ（任意）
};

class IRhiBuffer {
public:
    virtual ~IRhiBuffer() noexcept = default;

    virtual usize       Size()  const noexcept = 0;
    virtual BufferUsage Usage() const noexcept = 0;

    // CPU からデータを書き込む（cpu_writable=true で作ったバッファのみ可能）
    virtual void Update(const void* data, usize size, usize offset = 0) noexcept = 0;
};

Result<UniquePtr<IRhiBuffer>> CreateRhiBuffer(IRhiDevice& device, const BufferDesc& desc) noexcept;

} // namespace acs
