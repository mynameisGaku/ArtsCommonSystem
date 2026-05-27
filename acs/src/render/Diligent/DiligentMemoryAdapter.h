// SPDX-License-Identifier: Apache-2.0
// Diligent Engine の IMemoryAllocator を ACS Memory モジュールにブリッジする
// EngineCreateInfo::pRawMemAllocator にこれを渡すと、Diligent の内部 malloc が
// ACS の FAllocator (ESegment::RenderInternal 等) を経由するようになる。
#pragma once

#include "foundation/Types.h"

namespace acs {

class FAllocator;

// IMemoryAllocator (Diligent) と互換のフットプリントを持つ簡易ラッパ。
// 実体は Diligent の vtable と互換になるよう DiligentMemoryAdapter.cpp で定義する。
class DiligentMemoryAdapter {
public:
    // 引数の FAllocator を呼び出し先として使うアダプタを生成する（プロセス寿命）。
    // 戻り値: Diligent::IMemoryAllocator* として渡せる void*。
    static void* Create(FAllocator* backing) noexcept;
};

} // namespace acs
