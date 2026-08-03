// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "memory/UniquePtr.h"
#include "render/IRhiBuffer.h"

namespace acs {

class IRhiDevice;

/** 同一サイズの一時定数を少数のGPUバッファへまとめるフレームarena。 */
class CTransientUploadArena final {
public:
    /** GPU資源を持たない空状態を作る。 */
    CTransientUploadArena() noexcept = default;

    /** 所有ページを解放する。 */
    ~CTransientUploadArena() noexcept = default;

    /** 単独所有を保つためコピーを禁止する。 */
    CTransientUploadArena(const CTransientUploadArena&) = delete;

    /** 単独所有を保つためコピー代入を禁止する。 */
    CTransientUploadArena& operator=(const CTransientUploadArena&) = delete;

    /** arenaの所有権を移す。 */
    CTransientUploadArena(CTransientUploadArena&&) noexcept = default;

    /** arenaの所有権を移して代入する。 */
    CTransientUploadArena& operator=(CTransientUploadArena&&) noexcept = default;

    /**
     * 一時定数の大きさと初期個数を設定して最初の共有ページを作る。
     *
     * @param device ページを作るRHIデバイス。
     * @param allocation_size 1割り当てで公開する最大バイト数。
     * @param initial_capacity 最初に確保する割り当て数。
     * @return 成功なら空の結果、入力不正または確保失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, usize allocation_size, u32 initial_capacity) noexcept;

    /** 全ページを解放して未初期化状態へ戻す。 */
    void Reset() noexcept;

    /**
     * 次フレームの必要数を予約し、使用位置だけを定数時間で先頭へ戻す。
     *
     * @details Raw backendはcommand recording開始時にfence完了済みframe slotを選び、
     * Diligent backendは同じqueue上のUpdateBuffer copy完了後にframe slotを再利用する。
     * renderer通常のframe境界で呼ぶ限り、CPU cursorのresetは実行中GPU範囲を上書きしない。
     * @param required_allocations 今フレームに必要な割り当て数の上限。
     * @return 必要容量を確保できた場合はtrue。
     */
    bool BeginFrame(u32 required_allocations = 0u) noexcept;

    /**
     * 必要数まで共有ページを追加する。
     *
     * @param required_allocations 必要な総割り当て数。
     * @return 既存容量で足りるか追加確保に成功した場合はtrue。
     */
    bool Reserve(u32 required_allocations) noexcept;

    /**
     * 次の論理sliceへ定数を書き、通常のIRhiBufferとして返す。
     *
     * @param data 書き込む定数の先頭。
     * @param size 書き込むバイト数。
     * @return 成功時は割り当てslice、入力不正または確保失敗時はnullptr。
     */
    IRhiBuffer* Upload(const void* data, usize size) noexcept;

    /** 指定した論理sliceを返す。 */
    IRhiBuffer* Get(u32 allocation_index) noexcept;

    /** 指定した論理sliceをconst arenaから返す。 */
    IRhiBuffer* Get(u32 allocation_index) const noexcept;

    /** 確保済みの論理slice数を返す。 */
    u32 Capacity() const noexcept { return m_Capacity; }

    /** 現フレームで使用済みの論理slice数を返す。 */
    u32 Used() const noexcept { return m_Cursor; }

    /** 所有する実GPUバッファ数を返す。 */
    u32 GpuBufferCount() const noexcept { return static_cast<u32>(m_Pages.Num()); }

    /** RHIページ記述へ要求した一frame分の論理総バイト数を返す。 */
    usize ReservedBytes() const noexcept { return m_ReservedBytes; }

    /** D3D12とDiligentで共通に使う定数バッファoffset境界を返す。 */
    static constexpr usize AllocationAlignment() noexcept { return 256u; }

private:
    /** 共有GPUバッファの一範囲をIRhiBufferとして公開する論理slice。 */
    class FSlice final : public IRhiBuffer {
    public:
        /** 親を持たない空sliceを作る。 */
        FSlice() noexcept = default;

        /** 親バッファと公開範囲を設定する。 */
        void Configure(IRhiBuffer& buffer, usize offset, usize size) noexcept;

        /** 公開範囲のバイト数を返す。 */
        usize Size() const noexcept override { return m_Size; }

        /** 定数バッファ用途を返す。 */
        EBufferUsage Usage() const noexcept override { return EBufferUsage::Uniform; }

        /** 公開範囲内へデータを書き込む。 */
        void Update(const void* data, usize size, usize offset = 0u) noexcept override;

        /** backendがbindする実バッファを返す。 */
        IRhiBuffer& BindingBuffer() noexcept override;

        /** 実バッファ先頭からのbind offsetを返す。 */
        usize BindingOffset() const noexcept override;

    private:
        /** sliceを保持する実GPUバッファ。 */
        IRhiBuffer* m_Buffer = nullptr;

        /** 実GPUバッファ先頭からのバイトoffset。 */
        usize m_Offset = 0u;

        /** 呼び出し側へ公開する最大バイト数。 */
        usize m_Size = 0u;
    };

    /** 一つの実GPUバッファと固定数sliceを所有するページ。 */
    struct FPage {
        /** ページが所有する実GPUバッファ。 */
        TUniquePtr<IRhiBuffer> buffer;

        /** 再配置しない論理slice配列。 */
        TArray<FSlice> slices;

        /** arena全体での先頭slice番号。 */
        u32 first_allocation = 0u;
    };

    /** 指定個数を持つページを末尾へ追加する。 */
    bool AddPage(u32 allocation_count) noexcept;

    /** 要求値以上へ幾何成長させる次ページ個数を返す。 */
    u32 NextPageCapacity(u32 required_allocations) const noexcept;

    /** ページ生成に使うRHIデバイス。 */
    IRhiDevice* m_Device = nullptr;

    /** 一割り当てで公開する最大バイト数。 */
    usize m_AllocationSize = 0u;

    /** GPU上で隣接sliceを分離するアライン済み間隔。 */
    usize m_Stride = 0u;

    /** 全ページの論理slice総数。 */
    u32 m_Capacity = 0u;

    /** 現フレームで次に返すslice番号。 */
    u32 m_Cursor = 0u;

    /** 次の検索を始めるページ番号。 */
    u32 m_ActivePage = 0u;

    /** 実GPUバッファと固定sliceを所有するページ列。 */
    TArray<FPage> m_Pages;

    /** RHIページ記述へ要求した一frame分の論理総バイト数。 */
    usize m_ReservedBytes = 0u;
};

/** 旧名を使う既存コード向けの互換別名。 */
using FTransientUploadArena = CTransientUploadArena;


} // namespace acs
