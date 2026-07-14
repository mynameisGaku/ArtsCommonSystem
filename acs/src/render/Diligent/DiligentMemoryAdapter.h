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
     * backing を呼び出し先に使う静的寿命のアダプタを返す。
     *
     * @details
     * アダプタ本体は backing から確保しない。Create の成功を新しい Diligent 寿命の開始とみなし、
     * 委譲先アドレスが同一でも未解放領域が 1 件以上あれば再設定を拒否する。成功時はバインド世代を
     * 単調に進める。各確保は backing 領域外のレジストリへ backing 寿命世代とともに記録するため、
     * MemorySystem 再初期化で allocator のアドレスが再利用されても、旧領域を読み取らずに拒否できる。
     * backing オブジェクト本体と LifetimeGeneration() は、アダプタの全呼び出しと未解放記録がなくなるまで
     * 有効でなければならない。利用側は Diligent の解放と MemorySystem の終了・再初期化を直列化し、
     * Diligent オブジェクトを全解放してからメモリシステムを終了・再初期化すること。寿命世代の検査は、
     * 完了済みの終了・再初期化後に残った呼び出しを拒否するもので、backing 自体の並行破棄は許可しない。
     * @param backing 実際の確保・解放を委譲する ACS アロケータ。
     * @return Diligent::IMemoryAllocator* として渡せる void*。引数不正または再設定不能なら nullptr。
     */
    static void* Create(FAllocator* backing) noexcept;

    /**
     * Diligent が現在保持しているアダプタ経由の確保件数を返す。
     *
     * @return Free されていない Diligent 内部確保の件数。
     */
    static u64 OutstandingAllocationCount() noexcept;

    /**
     * Diligent が現在保持している要求バイト数の合計を返す。
     *
     * @return Free されていない Diligent 内部確保の要求サイズ合計。
     */
    static u64 OutstandingRequestedBytes() noexcept;

    /**
     * 現在の成功バインド世代を返す。
     *
     * @details Create が成功するたびに進み、失敗した Create では変化しない。0 は未バインドを表す。
     * @return 静的アダプタの現在のバインド世代。
     */
    static u64 BindingGeneration() noexcept;

    /**
     * 現在バインドしている backing allocator の寿命世代を返す。
     *
     * @return Create 成功時に取得した FAllocator::LifetimeGeneration。未バインドなら0。
     */
    static u64 BackingLifetimeGeneration() noexcept;
};

} // namespace acs
