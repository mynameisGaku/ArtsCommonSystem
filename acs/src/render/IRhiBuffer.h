// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/RhiTypes.h"

namespace acs {

class IRhiDevice;

/**
 * バッファの用途を表す。
 *
 * @details バッファ生成時の最適なメモリ配置・バインド先を決めるために使う。
 */
enum class EBufferUsage : u8 {
    /** 頂点バッファ。 */
    Vertex,

    /** 16-bit インデックスバッファ。 */
    Index16,

    /** 32-bit インデックスバッファ。 */
    Index32,

    /** 定数バッファ (CB)。 */
    Uniform,

    /** UAV / SSBO。 */
    Storage,

    /** CPU からのアップロード用ステージングバッファ。 */
    Staging,
};

/**
 * バッファ生成パラメータ。
 *
 * @details サイズ・用途に加え、動的更新可否と初期データを指定する。
 */
struct FBufferDesc {
    /** バッファのバイトサイズ。 */
    usize       size         = 0;

    /** バッファの用途。 */
    EBufferUsage usage        = EBufferUsage::Vertex;

    /** 動的に Update したいなら true。 */
    bool        cpu_writable = false;

    /** 初期データへのポインタ (任意、不要なら nullptr)。 */
    const void* initial_data = nullptr;

    /** 構造化バッファの 1 要素のバイト数。>0 で BUFFER_MODE_STRUCTURED + ElementByteStride
     *  → SRV/UAV view が作られ compute から StructuredBuffer / RWStructuredBuffer として読み書き可能に。
     *  usage=Storage と併用する (light culling のタイルバケット等)。 */
    u32         struct_stride = 0;

    /** true で BIND_INDIRECT_DRAW_ARGS を追加し、DispatchIndirect の引数バッファに使える。
     *  compute が結果を書き込む場合は usage=Storage + struct_stride>0 と併用する
     *  (ThreadGroupCountX/Y/Z を RWStructuredBuffer<uint> で書き、そのまま indirect 引数に使う)。 */
    bool        indirect_args = false;
};

/**
 * GPU バッファの抽象インターフェイス (頂点・インデックス・定数バッファ)。
 *
 * @details バックエンド (DX12 / Vulkan 等) が実装する。生成は CreateRhiBuffer で行う。
 */
class IRhiBuffer {
public:
    /** 派生バックエンド実装を正しく破棄するための仮想デストラクタ。 */
    virtual ~IRhiBuffer() noexcept = default;

    /**
     * バッファのバイトサイズを返す。
     *
     * @return 確保したバイトサイズ。
     */
    virtual usize       Size()  const noexcept = 0;

    /**
     * バッファの用途を返す。
     *
     * @return 生成時に指定した EBufferUsage。
     */
    virtual EBufferUsage Usage() const noexcept = 0;

    /**
     * CPU からデータを書き込む。
     *
     * @details cpu_writable=true で生成したバッファのみ可能。
     * @param data 書き込むデータの先頭。
     * @param size 書き込むバイト数。
     * @param offset バッファ先頭からの書き込み開始オフセット (既定 0)。
     */
    virtual void Update(const void* data, usize size, usize offset = 0) noexcept = 0;

    /**
     * backendが実際にbindする親バッファを返す。
     *
     * @details 通常バッファは自身を返す。一時arenaの論理sliceは共有親を返す。
     * @return backend固有型へ変換できる実バッファ。
     */
    virtual IRhiBuffer& BindingBuffer() noexcept { return *this; }

    /**
     * 実バッファ先頭からのbind offsetを返す。
     *
     * @details backend がフレームごとに物理領域を分ける通常バッファは現在領域の先頭を返す。
     * 一時 arena の論理 slice は、親バッファの物理先頭へ自身の部分領域を加えた位置を返す。
     * @return backend が実際の bind に使う実バッファ先頭からのバイト位置。
     */
    virtual usize BindingOffset() const noexcept { return 0u; }
};

/**
 * GPU バッファを生成する。
 *
 * @param device バッファ生成に使う RHI デバイス。
 * @param desc バッファ生成パラメータ。
 * @return 成功なら所有権付きバッファ、生成失敗ならエラー。
 */
TResult<TUniquePtr<IRhiBuffer>> CreateRhiBuffer(IRhiDevice& device, const FBufferDesc& desc) noexcept;

} // namespace acs
