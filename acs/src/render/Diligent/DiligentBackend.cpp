// Diligent バックエンドのファクトリ集約
// IRhi* インターフェイスの Create*() 関数を Diligent 経由で実装する。
// このファイルは WITH_RENDER_DILIGENT が ON のときだけ意味を持つ。
#include "render/IRhiDevice.h"
#include "render/IRhiSwapchain.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiTexture.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentDevice.h"
#include "render/Diligent/DiligentSwapchain.h"
#include "render/Diligent/DiligentCommandList.h"
#include "render/Diligent/DiligentBuffer.h"
#include "render/Diligent/DiligentTexture.h"
#include "render/Diligent/DiligentPipeline.h"
#include "render/Diligent/DiligentShader.h"
#include "memory/UniquePtr.h"

namespace acs {

namespace {
// 受け取った IRhiDevice が Diligent 由来か確認するための簡易チェック
// （RTTI 無効環境下のため、BackendName() の先頭一致で判定）
bool IsDiligentDevice(IRhiDevice& device) noexcept {
    const char* bn = device.BackendName();
    return bn[0] == 'D' && bn[1] == 'i' && bn[2] == 'l' && bn[3] == 'i';
}
} // namespace

Result<UniquePtr<IRhiDevice>> CreateRhiDevice(const DeviceConfig& cfg) noexcept {
    auto d = MakeUnique<DiligentDevice>();
    if (!d) return ACS_ERR(Memory, 200, "DiligentDevice alloc failed");
    auto r = d->Init(cfg);
    if (r.IsErr()) return r.Error();
    UniquePtr<IRhiDevice> base(d.Release(), d.GetAllocator());
    return Result<UniquePtr<IRhiDevice>>(OkInit, Move(base));
}

Result<UniquePtr<IRhiSwapchain>> CreateRhiSwapchain(IRhiDevice& device,
                                                    const SwapchainConfig& cfg) noexcept {
    if (!IsDiligentDevice(device))
        return ACS_ERR(Render, 210, "CreateRhiSwapchain: device is not Diligent");
    auto sc = MakeUnique<DiligentSwapchain>();
    auto r = sc->Init(static_cast<DiligentDevice&>(device), cfg);
    if (r.IsErr()) return r.Error();
    UniquePtr<IRhiSwapchain> base(sc.Release(), sc.GetAllocator());
    return Result<UniquePtr<IRhiSwapchain>>(OkInit, Move(base));
}

Result<UniquePtr<IRhiCommandList>> CreateRhiCommandList(IRhiDevice& device) noexcept {
    if (!IsDiligentDevice(device))
        return ACS_ERR(Render, 220, "CreateRhiCommandList: device is not Diligent");
    auto cl = MakeUnique<DiligentCommandList>();
    auto r = cl->Init(static_cast<DiligentDevice&>(device));
    if (r.IsErr()) return r.Error();
    UniquePtr<IRhiCommandList> base(cl.Release(), cl.GetAllocator());
    return Result<UniquePtr<IRhiCommandList>>(OkInit, Move(base));
}

Result<UniquePtr<IRhiBuffer>> CreateRhiBuffer(IRhiDevice& device, const BufferDesc& desc) noexcept {
    if (!IsDiligentDevice(device))
        return ACS_ERR(Render, 230, "CreateRhiBuffer: device is not Diligent");
    auto b = MakeUnique<DiligentBuffer>();
    auto r = b->Init(static_cast<DiligentDevice&>(device), desc);
    if (r.IsErr()) return r.Error();
    UniquePtr<IRhiBuffer> base(b.Release(), b.GetAllocator());
    return Result<UniquePtr<IRhiBuffer>>(OkInit, Move(base));
}

Result<UniquePtr<IRhiTexture>> CreateRhiTexture(IRhiDevice& device,
                                                const TextureDesc& desc) noexcept {
    if (!IsDiligentDevice(device))
        return ACS_ERR(Render, 240, "CreateRhiTexture: device is not Diligent");
    auto t = MakeUnique<DiligentTexture>();
    auto r = t->Init(static_cast<DiligentDevice&>(device), desc);
    if (r.IsErr()) return r.Error();
    UniquePtr<IRhiTexture> base(t.Release(), t.GetAllocator());
    return Result<UniquePtr<IRhiTexture>>(OkInit, Move(base));
}

Result<UniquePtr<IRhiPipeline>> CreateRhiPipeline(IRhiDevice& device,
                                                  const PipelineDesc& desc) noexcept {
    if (!IsDiligentDevice(device))
        return ACS_ERR(Render, 250, "CreateRhiPipeline: device is not Diligent");
    auto p = MakeUnique<DiligentPipeline>();
    auto r = p->Init(static_cast<DiligentDevice&>(device), desc);
    if (r.IsErr()) return r.Error();
    UniquePtr<IRhiPipeline> base(p.Release(), p.GetAllocator());
    return Result<UniquePtr<IRhiPipeline>>(OkInit, Move(base));
}

Result<UniquePtr<IRhiShader>> CreateRhiShader(IRhiDevice& device, const ShaderDesc& desc) noexcept {
    if (!IsDiligentDevice(device))
        return ACS_ERR(Render, 260, "CreateRhiShader: device is not Diligent");
    auto s = MakeUnique<DiligentShader>();
    auto r = s->Init(static_cast<DiligentDevice&>(device), desc);
    if (r.IsErr()) return r.Error();
    UniquePtr<IRhiShader> base(s.Release(), s.GetAllocator());
    return Result<UniquePtr<IRhiShader>>(OkInit, Move(base));
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
