// SPDX-License-Identifier: Apache-2.0
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

TResult<TUniquePtr<IRhiDevice>> CreateRhiDevice(const DeviceConfig& cfg) noexcept {
    auto d = MakeUnique<DiligentDevice>();
    if (!d) return ACS_ERR(Memory, 200, "DiligentDevice alloc failed");
    const auto r = d->Init(cfg);
    if (r.IsErr()) return r.Error();
    TUniquePtr<IRhiDevice> base(d.Release(), d.GetAllocator());
    return TResult<TUniquePtr<IRhiDevice>>(OkInit, Move(base));
}

TResult<TUniquePtr<IRhiSwapchain>> CreateRhiSwapchain(IRhiDevice& device,
                                                    const SwapchainConfig& cfg) noexcept {
    if (!IsDiligentDevice(device))
        return ACS_ERR(Render, 210, "CreateRhiSwapchain: device is not Diligent");
    auto sc = MakeUnique<DiligentSwapchain>();
    // MakeUnique は確保失敗時に null を返し得るため、Init で null-deref する前に検査する
    if (!sc) return ACS_ERR(Memory, 211, "DiligentSwapchain alloc failed");
    const auto r = sc->Init(static_cast<DiligentDevice&>(device), cfg);
    if (r.IsErr()) return r.Error();
    TUniquePtr<IRhiSwapchain> base(sc.Release(), sc.GetAllocator());
    return TResult<TUniquePtr<IRhiSwapchain>>(OkInit, Move(base));
}

TResult<TUniquePtr<IRhiCommandList>> CreateRhiCommandList(IRhiDevice& device) noexcept {
    if (!IsDiligentDevice(device))
        return ACS_ERR(Render, 220, "CreateRhiCommandList: device is not Diligent");
    auto cl = MakeUnique<DiligentCommandList>();
    // MakeUnique は確保失敗時に null を返し得るため、Init で null-deref する前に検査する
    if (!cl) return ACS_ERR(Memory, 221, "DiligentCommandList alloc failed");
    const auto r = cl->Init(static_cast<DiligentDevice&>(device));
    if (r.IsErr()) return r.Error();
    TUniquePtr<IRhiCommandList> base(cl.Release(), cl.GetAllocator());
    return TResult<TUniquePtr<IRhiCommandList>>(OkInit, Move(base));
}

TResult<TUniquePtr<IRhiBuffer>> CreateRhiBuffer(IRhiDevice& device, const FBufferDesc& desc) noexcept {
    if (!IsDiligentDevice(device))
        return ACS_ERR(Render, 230, "CreateRhiBuffer: device is not Diligent");
    auto b = MakeUnique<DiligentBuffer>();
    // MakeUnique は確保失敗時に null を返し得るため、Init で null-deref する前に検査する
    if (!b) return ACS_ERR(Memory, 231, "DiligentBuffer alloc failed");
    const auto r = b->Init(static_cast<DiligentDevice&>(device), desc);
    if (r.IsErr()) return r.Error();
    TUniquePtr<IRhiBuffer> base(b.Release(), b.GetAllocator());
    return TResult<TUniquePtr<IRhiBuffer>>(OkInit, Move(base));
}

TResult<TUniquePtr<IRhiTexture>> CreateRhiTexture(IRhiDevice& device,
                                                const FTextureDesc& desc) noexcept {
    if (!IsDiligentDevice(device))
        return ACS_ERR(Render, 240, "CreateRhiTexture: device is not Diligent");
    auto t = MakeUnique<DiligentTexture>();
    // MakeUnique は確保失敗時に null を返し得るため、Init で null-deref する前に検査する
    if (!t) return ACS_ERR(Memory, 241, "DiligentTexture alloc failed");
    const auto r = t->Init(static_cast<DiligentDevice&>(device), desc);
    if (r.IsErr()) return r.Error();
    TUniquePtr<IRhiTexture> base(t.Release(), t.GetAllocator());
    return TResult<TUniquePtr<IRhiTexture>>(OkInit, Move(base));
}

TResult<TUniquePtr<IRhiPipeline>> CreateRhiPipeline(IRhiDevice& device,
                                                  const FPipelineDesc& desc) noexcept {
    if (!IsDiligentDevice(device))
        return ACS_ERR(Render, 250, "CreateRhiPipeline: device is not Diligent");
    auto p = MakeUnique<DiligentPipeline>();
    // MakeUnique は確保失敗時に null を返し得るため、Init で null-deref する前に検査する
    if (!p) return ACS_ERR(Memory, 251, "DiligentPipeline alloc failed");
    const auto r = p->Init(static_cast<DiligentDevice&>(device), desc);
    if (r.IsErr()) return r.Error();
    TUniquePtr<IRhiPipeline> base(p.Release(), p.GetAllocator());
    return TResult<TUniquePtr<IRhiPipeline>>(OkInit, Move(base));
}

TResult<TUniquePtr<IRhiShader>> CreateRhiShader(IRhiDevice& device, const FShaderDesc& desc) noexcept {
    if (!IsDiligentDevice(device))
        return ACS_ERR(Render, 260, "CreateRhiShader: device is not Diligent");
    auto s = MakeUnique<DiligentShader>();
    // MakeUnique は確保失敗時に null を返し得るため、Init で null-deref する前に検査する
    if (!s) return ACS_ERR(Memory, 261, "DiligentShader alloc failed");
    const auto r = s->Init(static_cast<DiligentDevice&>(device), desc);
    if (r.IsErr()) return r.Error();
    TUniquePtr<IRhiShader> base(s.Release(), s.GetAllocator());
    return TResult<TUniquePtr<IRhiShader>>(OkInit, Move(base));
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
