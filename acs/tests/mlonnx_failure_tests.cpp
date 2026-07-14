// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "container/StringView.h"
#include "mlonnx/OnnxMlRuntime.h"

using namespace acs;
using namespace acs::mlonnx;

ACS_TEST(MlOnnx, RepeatedCreateSessionFailuresRemainRestartable)
{
    FOnnxMlRuntime runtime;
    EXPECT_TRUE(runtime.Init().IsOk());

    for (u32 i = 0; i < 32; ++i) {
        auto model = runtime.LoadModel("Z:/acs/does-not-exist/model.onnx");
        EXPECT_TRUE(model.IsErr());
        if (model.IsErr()) {
            // OrtStatus の解放後も、エラーメッセージは静的寿命で有効であること。
            EXPECT_TRUE(FStringView(model.Error().message) == FStringView("CreateSession failed"));
        }
    }

    runtime.Shutdown();
    runtime.Shutdown();
    EXPECT_TRUE(runtime.Init().IsOk());
    runtime.Shutdown();
}
