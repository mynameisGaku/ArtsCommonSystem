// SPDX-License-Identifier: Apache-2.0
// コンパイル時ログゲートが無効レベルの引数評価とリンク依存を除去することを検証する。
#ifdef ACS_COMPILED_LOG_MIN_SEVERITY
    #undef ACS_COMPILED_LOG_MIN_SEVERITY
#endif
#define ACS_COMPILED_LOG_MIN_SEVERITY 2

#include "foundation/Log.h"

static_assert(!acs::FLogger::CompiledEnabled<acs::ELogSeverity::Trace>());
static_assert(!acs::FLogger::CompiledEnabled<acs::ELogSeverity::Debug>());
static_assert(acs::FLogger::CompiledEnabled<acs::ELogSeverity::Info>());

int main()
{
    int side_effect = 0;
    ACS_LOG_TRACE("除去されるログ引数: %d", ++side_effect);
    return side_effect == 0 ? 0 : 1;
}
