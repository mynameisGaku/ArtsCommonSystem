// SPDX-License-Identifier: Apache-2.0
// Real ONNX Runtime backend for acs::game::IMlRuntime.
#pragma once

#include "gameframework/MlRuntime.h"

namespace acs::mlonnx {

/**
 * 本物の ONNX Runtime を使う acs::game::IMlRuntime backend 実装。
 *
 * @details
 * ONNX Runtime C API (onnxruntime_c_api.h) を pimpl (FImpl) 経由で叩き、
 * モデルの読み込み・推論・解放を行う。1-input / 1-output の float テンソルモデルのみ
 * 対応し、最大 kMaxModels (16) 個のモデルを固定スロットで保持する。Init() →
 * (LoadModel / RunInference / UnloadModel)* → Shutdown() のライフタイムで、
 * シングルスレッドでのみ使う前提。
 */
class FOnnxMlRuntime final : public acs::game::IMlRuntime {
public:
    /** pimpl (FImpl) を確保して構築する (Init は別途呼ぶ必要がある)。 */
    FOnnxMlRuntime() noexcept;

    /** Shutdown 後に pimpl を解放して破棄する。 */
    ~FOnnxMlRuntime() noexcept override;

    /**
     * ONNX Runtime を初期化する (API 取得 / Env・SessionOptions・Allocator 作成)。
     *
     * @details 初期化済みなら no-op で Ok を返す。失敗時は Shutdown して途中確保分を巻き戻す。
     * @return 成功なら空の TResult、初期化失敗ならエラー。
     */
    acs::TResult<void> Init() noexcept override;

    /**
     * 全モデルセッションと ONNX Runtime リソースを解放する (多重呼び出し安全)。
     */
    void Shutdown() noexcept override;

    /**
     * ONNX モデルファイルを読み込んでセッションを作り、ハンドルを返す。
     *
     * @details
     * 入出力が各 1 個の float テンソルモデルのみ対応し、テンソル rank は最大 8。
     * model_path は UTF-8 として wide path に変換して CreateSession に渡す。空きスロットが
     * 無い場合はプール満杯エラー。読み込んだ入出力名・形状・要素数をスロットに記録する。
     * @param model_path 読み込むモデルファイルのパス (UTF-8、nullptr/空は失敗)。
     * @return 成功ならモデルハンドル、失敗ならエラー。
     */
    acs::TResult<acs::game::FMlModelHandle> LoadModel(const char* model_path) noexcept override;

    /**
     * モデルセッションを解放してスロットを空ける。
     *
     * @details 未初期化なら NotImplemented エラー。無効/未知ハンドルは no-op で Ok を返す。
     * @param h 解放するモデルハンドル。
     * @return 成功なら空の TResult、未初期化ならエラー。
     */
    acs::TResult<void> UnloadModel(acs::game::FMlModelHandle h) noexcept override;

    /**
     * 指定モデルで推論を 1 回同期実行する。
     *
     * @details
     * inputs を入力テンソルとして OrtValue 化し、ONNX Runtime の Run を同期呼び出しする。
     * 動的軸 (shape <= 0) が 1 つあれば in_count から逆算し、無ければ in_count が
     * モデルの入力要素数と一致する必要がある。出力要素数が out_count を超える場合は失敗。
     * @param h 推論に使うモデルハンドル。
     * @param inputs 入力 f32 配列。
     * @param in_count 入力要素数。
     * @param outputs 出力を書き込む f32 配列。
     * @param out_count 出力バッファの要素数。
     * @return 成功なら空の TResult、未初期化/無効引数/形状不一致/実行失敗ならエラー。
     */
    acs::TResult<void> RunInference(acs::game::FMlModelHandle h,
                                    const acs::f32* inputs, acs::u32 in_count,
                                    acs::f32* outputs, acs::u32 out_count) noexcept override;

private:
    struct FImpl;

    /** ONNX Runtime 状態 (Env / SessionOptions / モデルスロット等) を抱える pimpl。 */
    FImpl* m_Impl = nullptr;
};

/**
 * プロセス共有の既定 FOnnxMlRuntime singleton への参照を返す。
 *
 * @details
 * SetMlRuntimeProvider に登録される provider の実体。static 単一インスタンス
 * (process lifetime) として本物の ONNX Runtime backend を返す。
 * @return 共有 FOnnxMlRuntime への参照 (IMlRuntime として)。
 */
acs::game::IMlRuntime& GetDefaultOnnxMlRuntime() noexcept;

/**
 * gameframework の MlRuntime provider に ONNX backend を登録する。
 *
 * @details
 * アプリ起動時に一度だけ呼ぶと、以降 acs::game::GetDefaultMlRuntime() が
 * GetDefaultOnnxMlRuntime() の本物の ONNX Runtime backend を返すようになる。これにより
 * 上位コードは backend 非依存に GetDefaultMlRuntime() だけで実 runtime を取得でき、
 * #if WITH_ACS_ML_ONNX はこの Install 呼び出し 1 箇所だけで済む。
 */
void InstallOnnxAsDefault() noexcept;

} // namespace acs::mlonnx
