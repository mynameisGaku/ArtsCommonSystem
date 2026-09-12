// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "foundation/Log.h"
#include "render/Dx12/Dx12Shader.h"

namespace {

/** DLLを欠いた別プロセスで、SM6だけが明示失敗し、既定経路は使えることを確認する。 */
int ProbeMissingDxcRuntime_Internal() noexcept
{
    /** 外部ファイルを読まない最小計算シェーダー。 */
    acs::FShaderDesc desc{};
    desc.stage = acs::EShaderStage::Compute;
    desc.hlsl_source = "[numthreads(1,1,1)] void main() {}";
    desc.target = "cs_6_0";
    /** 作成失敗で空のままになる命令所有者。 */
    acs::FDx12Shader shader;
    const acs::FHrResult missing = shader.Init(desc);
    if (missing.hr != HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) || shader.Bytecode() || shader.BytecodeSize() != 0u) return 11;
    desc.target = nullptr;
    if (shader.Init(desc).IsErr() || !shader.Bytecode() || shader.BytecodeSize() == 0u) return 12;
    return 0;
}

/** 実行方式の文字列を、余分な末尾を許さず比較する。 */
bool EqualArgument_Internal(const char* actual, const char* expected) noexcept
{
    if (!actual || !expected) return false;
    while (*actual != '\0' && *actual == *expected) {
        ++actual;
        ++expected;
    }
    return *actual == *expected;
}

} // namespace

/** 通常の製品試験またはDLL欠落の隔離検査を実行し、その成否を終了値へ返す。 */
int main(int argc, char** argv)
{
    /** 検査結果を呼出し元が保存できる標準出力へ出す設定。 */
    acs::FLogConfig configuration{};
    configuration.console = true;
    configuration.debug_output = false;
    acs::CLogger::Init(configuration);
    /** 引数なしでは登録済みの作成試験をすべて実行する。 */
    int result = 13;
    if (argc == 1) result = acs::test::RunAll();
    else if (argc == 2 && EqualArgument_Internal(argv[1], "--expect-missing-runtime")) result = ProbeMissingDxcRuntime_Internal();
    acs::CLogger::Flush();
    acs::CLogger::Shutdown();
    return result;
}
