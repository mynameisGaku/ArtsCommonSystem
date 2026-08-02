// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "foundation/Log.h"
#include "foundation/Platform.h"
#include "memory/SystemAllocator.h"

namespace {

/** 文字列が完全一致するかを CRT へ依存せず判定する。 */
bool TextEquals(const char* left, const char* right) noexcept
{
    if (!left || !right) return false;
    while (*left != '\0' && *right != '\0') {
        if (*left++ != *right++) return false;
    }
    return *left == '\0' && *right == '\0';
}

/**
 * CSystemAllocator の未解放破棄を隔離プロセス内で発生させる。
 *
 * @details 累積診断値を通常の単体テストプロセスへ残さず、親が標準エラーを検証できるようにする。
 */
int RunSystemAllocatorDestructionProbe() noexcept
{
    const acs::FSystemAllocatorProcessStatistics baseline = acs::CSystemAllocator::CaptureProcessStatistics();
    acs::u64 abandoned_bytes = 0;
    {
        acs::CSystemAllocator allocator;
        void* const allocation = allocator.Alloc(96u, 32u, acs::FSourceLoc::Current());
        if (!allocation) return 71;

        abandoned_bytes = allocator.BytesAllocated();
    }

    const acs::FSystemAllocatorProcessStatistics after = acs::CSystemAllocator::CaptureProcessStatistics();
    const bool valid = after.live_allocator_count == baseline.live_allocator_count &&
                       after.destroyed_with_live_allocations_count ==
                           baseline.destroyed_with_live_allocations_count + 1u &&
                       after.destroyed_outstanding_bytes == baseline.destroyed_outstanding_bytes + abandoned_bytes &&
                       after.destroyed_outstanding_allocation_count ==
                           baseline.destroyed_outstanding_allocation_count + 1u;

    // 意図的な未解放領域は、この隔離プロセス終了時に OS がプロセスヒープごと回収する。
    return valid ? 0 : 72;
}

/** 実行ファイル自身をコンパイルしたフロントエンドの ASan 計装状態を機械可読で返す。 */
int RunAddressSanitizerCapabilityProbe() noexcept
{
#if ACS_ADDRESS_SANITIZER && defined(__SANITIZE_ADDRESS__)
    constexpr char record[] = "memory_diagnostic_method=address_sanitizer binary_instrumented=true "
                              "compiler_macro=__SANITIZE_ADDRESS__\n";
#else
    constexpr char record[] = "memory_diagnostic_method=address_sanitizer binary_instrumented=false "
                              "compiler_macro=unavailable\n";
#endif

    const HANDLE standard_output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (!standard_output || standard_output == INVALID_HANDLE_VALUE) {
        return 74;
    }
    DWORD written = 0u;
    const DWORD record_length = static_cast<DWORD>(sizeof(record) - 1u);
    if (!::WriteFile(standard_output, record, record_length, &written, nullptr) || written != record_length) {
        return 75;
    }
    return 0;
}

} // namespace

int main(int argument_count, char** arguments)
{
    if (argument_count == 2 && TextEquals(arguments[1], "--system-allocator-destruction-probe")) {
        return RunSystemAllocatorDestructionProbe();
    }
    if (argument_count == 2 && TextEquals(arguments[1], "--address-sanitizer-capability-probe")) {
        return RunAddressSanitizerCapabilityProbe();
    }
    acs::FLogConfig cfg{};
    cfg.console = true;
    cfg.debug_output = false;
    cfg.min_severity = acs::ELogSeverity::Info;
    acs::CLogger::Init(cfg);

    int rc = acs::test::RunAll();

    acs::CLogger::Flush();
    acs::CLogger::Shutdown();
    return rc;
}
