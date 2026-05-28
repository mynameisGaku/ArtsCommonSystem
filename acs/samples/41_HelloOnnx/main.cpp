// SPDX-License-Identifier: Apache-2.0
// HelloOnnx - real ONNX Runtime smoke test.
#include "mlonnx/OnnxMlRuntime.h"

#include <cstdio>
#include <cmath>

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

bool WriteIdentityModel(const char* path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) {
        return false;
    }
    const size_t n = fwrite(kIdentityOnnx, 1, sizeof(kIdentityOnnx), f);
    fclose(f);
    return n == sizeof(kIdentityOnnx);
}

bool NearlyEqual(float a, float b) {
    return std::fabs(a - b) < 0.0001f;
}

} // namespace

int main() {
    constexpr const char* kModelPath = "acs_identity.onnx";
    std::puts("=== ACS HelloOnnx ===");
    std::puts("backend: REAL ONNX Runtime CPU (FOnnxMlRuntime)");

    if (!WriteIdentityModel(kModelPath)) {
        std::puts("write model: FAILED");
        return 2;
    }

    acs::mlonnx::FOnnxMlRuntime ml;
    auto init = ml.Init();
    if (init.IsErr()) {
        std::printf("Init: FAILED (%s)\n", init.Error().message);
        return 3;
    }

    auto model = ml.LoadModel(kModelPath);
    if (model.IsErr()) {
        std::printf("LoadModel: FAILED (%s)\n", model.Error().message);
        return 4;
    }

    const acs::f32 input[4] = { 1.0f, -2.5f, 3.25f, 42.0f };
    acs::f32 output[4] = {};
    auto run = ml.RunInference(model.Value(), input, 4, output, 4);
    if (run.IsErr()) {
        std::printf("RunInference: FAILED (%s)\n", run.Error().message);
        return 5;
    }

    bool ok = true;
    for (int i = 0; i < 4; ++i) {
        ok = ok && NearlyEqual(input[i], output[i]);
    }
    std::printf("Identity: [%.2f %.2f %.2f %.2f] -> [%.2f %.2f %.2f %.2f]\n",
                input[0], input[1], input[2], input[3],
                output[0], output[1], output[2], output[3]);
    std::puts(ok ? "result: ALL PASS" : "result: FAILED");

    auto unload = ml.UnloadModel(model.Value());
    (void)unload;
    ml.Shutdown();
    return ok ? 0 : 6;
}
