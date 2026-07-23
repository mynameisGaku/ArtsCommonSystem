// SPDX-License-Identifier: Apache-2.0
// シェーダ抽象（HLSL ソースをコンパイルして保持）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "container/Array.h"

namespace acs {

class IRhiDevice;

/**
 * シェーダのパイプラインステージ種別。
 */
enum class EShaderStage : u8 {
    /** 頂点シェーダ。 */
    Vertex,

    /** ピクセル（フラグメント）シェーダ。 */
    Pixel,

    /** コンピュートシェーダ。 */
    Compute,
};

/** Non-blocking shader compilation state exposed by supporting RHI backends. */
enum class EShaderStatus : u8 {
    Compiling,
    Ready,
    Failed,
};

/**
 * シェーダ生成パラメータ。
 *
 * @details
 * HLSL ソース文字列とエントリポイント・コンパイルターゲット・デバッグ名を指定する。
 * CreateRhiShader に渡すとバックエンドがコンパイルして IRhiShader を返す。
 */
struct FShaderDesc {
    /** コンパイル対象のシェーダステージ。 */
    EShaderStage stage     = EShaderStage::Vertex;

    /** HLSL ソース文字列。 */
    const char* hlsl_source = nullptr;

    /** エントリポイント関数名。 */
    const char* entry_point = "main";

    /** コンパイルターゲット（例: "vs_5_1" / "ps_5_1"）。null なら stage から自動決定。 */
    const char* target      = nullptr;

    /** デバッグ用の名前。 */
    const char* debug_name  = "shader";

    /** Request backend-managed asynchronous compilation when supported. */
    bool compile_async = false;
};

/**
 * コンパイル済みシェーダの抽象インタフェース。
 *
 * @details
 * HLSL からコンパイルしたバイトコードを保持し、パイプライン作成時に
 * バックエンドが内部参照する。CreateRhiShader で生成する。
 */
class IRhiShader {
public:
    /** 派生バックエンド実装を正しく破棄するための仮想デストラクタ。 */
    virtual ~IRhiShader() noexcept = default;

    /**
     * このシェーダのステージ種別を返す。
     *
     * @return シェーダステージ。
     */
    virtual EShaderStage Stage() const noexcept = 0;

    /**
     * コンパイル済みバイトコードの先頭ポインタを返す。
     *
     * @details パイプライン作成時にバックエンドが内部参照する。
     * @return バイトコード先頭へのポインタ。
     */
    virtual const byte* Bytecode() const noexcept = 0;

    /**
     * コンパイル済みバイトコードのバイト数を返す。
     *
     * @return バイトコードのサイズ（バイト）。
     */
    virtual usize       BytecodeSize() const noexcept = 0;

    /**
     * Query compilation without waiting. Synchronous backends are ready when
     * CreateRhiShader succeeds and use this default implementation.
     */
    virtual EShaderStatus Status() const noexcept
    {
        return EShaderStatus::Ready;
    }
};

/**
 * HLSL ソースをコンパイルして IRhiShader を生成する。
 *
 * @param device シェーダコンパイルに使う RHI デバイス。
 * @param desc シェーダ生成パラメータ。
 * @return 成功なら所有権付きの IRhiShader、コンパイル失敗ならエラー。
 */
TResult<TUniquePtr<IRhiShader>> CreateRhiShader(IRhiDevice& device, const FShaderDesc& desc) noexcept;

} // namespace acs
