// SPDX-License-Identifier: Apache-2.0
// HelloOnnx - COnnxDemoApp implementation.
#include "OnnxDemoApp.h"

#include "mlonnx/OnnxMlRuntime.h"

#include <cmath>
#include <cstdio>

namespace helloonnx {

namespace {

constexpr unsigned char kIdentityOnnx[] = {
    0x08, 0x07, 0x12, 0x03, 0x61, 0x63, 0x73, 0x3A, 0x5C, 0x0A, 0x19, 0x0A,
    0x05, 0x69, 0x6E, 0x70, 0x75, 0x74, 0x12, 0x06, 0x6F, 0x75, 0x74, 0x70,
    0x75, 0x74, 0x22, 0x08, 0x49, 0x64, 0x65, 0x6E, 0x74, 0x69, 0x74, 0x79,
    0x12, 0x0C, 0x61, 0x63, 0x73, 0x5F, 0x69, 0x64, 0x65, 0x6E, 0x74, 0x69,
    0x74, 0x79, 0x5A, 0x17, 0x0A, 0x05, 0x69, 0x6E, 0x70, 0x75, 0x74, 0x12,
    0x0E, 0x0A, 0x0C, 0x08, 0x01, 0x12, 0x08, 0x0A, 0x02, 0x08, 0x01, 0x0A,
    0x02, 0x08, 0x04, 0x62, 0x18, 0x0A, 0x06, 0x6F, 0x75, 0x74, 0x70, 0x75,
    0x74, 0x12, 0x0E, 0x0A, 0x0C, 0x08, 0x01, 0x12, 0x08, 0x0A, 0x02, 0x08,
    0x01, 0x0A, 0x02, 0x08, 0x04, 0x42, 0x02, 0x10, 0x0D
};

bool WriteIdentityModel(const char* Path) noexcept {
    FILE* File = nullptr;
    if (fopen_s(&File, Path, "wb") != 0 || !File) {
        return false;
    }
    const size_t Written = fwrite(kIdentityOnnx, 1, sizeof(kIdentityOnnx), File);
    fclose(File);
    return Written == sizeof(kIdentityOnnx);
}

bool NearlyEqual(float A, float B) noexcept {
    return std::fabs(A - B) < 0.0001f;
}

} // namespace

int COnnxDemoApp::Run() noexcept {
    constexpr const char* kModelPath = "acs_identity.onnx";
    std::puts("=== ACS HelloOnnx ===");
    std::puts("backend: REAL ONNX Runtime CPU (COnnxMlRuntime)");

    if (!WriteIdentityModel(kModelPath)) {
        std::puts("write model: FAILED");
        return 2;
    }

    acs::mlonnx::COnnxMlRuntime Ml;
    auto Init = Ml.Init();
    if (Init.IsErr()) {
        std::printf("Init: FAILED (%s)\n", Init.Error().message);
        return 3;
    }

    auto Model = Ml.LoadModel(kModelPath);
    if (Model.IsErr()) {
        std::printf("LoadModel: FAILED (%s)\n", Model.Error().message);
        return 4;
    }

    const acs::f32 Input[4] = { 1.0f, -2.5f, 3.25f, 42.0f };
    acs::f32 Output[4] = {};
    auto RunResult = Ml.RunInference(Model.Value(), Input, 4, Output, 4);
    if (RunResult.IsErr()) {
        std::printf("RunInference: FAILED (%s)\n", RunResult.Error().message);
        return 5;
    }

    bool bOk = true;
    for (int i = 0; i < 4; ++i) {
        bOk = bOk && NearlyEqual(Input[i], Output[i]);
    }
    std::printf("Identity: [%.2f %.2f %.2f %.2f] -> [%.2f %.2f %.2f %.2f]\n",
                Input[0], Input[1], Input[2], Input[3],
                Output[0], Output[1], Output[2], Output[3]);
    std::puts(bOk ? "result: ALL PASS" : "result: FAILED");

    auto Unload = Ml.UnloadModel(Model.Value());
    (void)Unload;
    Ml.Shutdown();
    return bOk ? 0 : 6;
}

} // namespace helloonnx
