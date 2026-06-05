// SPDX-License-Identifier: Apache-2.0
// Diligent Engine の IMemoryAllocator を ACS Memory モジュールにブリッジする
// EngineCreateInfo::pRawMemAllocator にこれを渡すと、Diligent の内部 malloc が
// ACS の FAllocator (ESegment::RenderInternal 等) を経由するようになる。
#pragma once

#include "foundation/Types.h"

namespace acs {

class FAllocator;

/**
 * Diligent の IMemoryAllocator を ACS の FAllocator にブリッジするアダプタ。
 *
 * @details
 * 実体は Diligent の IMemoryAllocator vtable と互換になるよう DiligentMemoryAdapter.cpp で
 * 定義する。生成したアダプタを EngineCreateInfo::pRawMemAllocator に渡すと、Diligent の
 * 内部 malloc が ACS の FAllocator を経由するようになる。
 */
class DiligentMemoryAdapter {
public:
    /**
     * backing を呼び出し先に使うアダプタをプロセス寿命で生成する。
     *
     * @details
     * 初回呼び出しでシングルトンを backing から確保して生成し、以降は同じインスタンスを返す。
     * @param backing 実際の確保・解放を委譲する ACS アロケータ。
     * @return Diligent::IMemoryAllocator* として渡せる void* (backing が null や確保失敗なら nullptr)。
     */
    static void* Create(FAllocator* backing) noexcept;
};

} // namespace acs
