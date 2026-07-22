// SPDX-License-Identifier: Apache-2.0
// Real ONNX Runtime backend for acs::game::IMlRuntime.
#include "mlonnx/OnnxMlRuntime.h"

#include "foundation/Error.h"
#include "foundation/Log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "onnxruntime_c_api.h"

#include <cstring>

namespace acs::mlonnx {

namespace {

constexpr acs::u16 kSubOrtInitFailed      = 201;
constexpr acs::u16 kSubOrtLoadFailed      = 202;
constexpr acs::u16 kSubOrtRunFailed       = 203;
constexpr acs::u16 kSubOrtShapeMismatch   = 204;
constexpr acs::u16 kSubOrtInvalidHandle   = 205;
constexpr acs::u16 kSubOrtPoolFull        = 206;

acs::TResult<void> OrtStatusToResult(const OrtApi& api, OrtStatus* status,
                                      acs::u16 subcode, const char* fallback) noexcept {
    if (status == nullptr) {
        return acs::Ok();
    }
    // GetErrorMessage のポインタは OrtStatus と同時に無効になる。詳細は解放前に
    // ログへ写し、TResult には寿命が静的な fallback と数値コードだけを保持する。
    const char* const message = api.GetErrorMessage(status);
    const OrtErrorCode ort_error = api.GetErrorCode(status);
    ACS_LOG_ERROR("%s: %s", fallback, message ? message : "ONNX Runtime error");
    const acs::FErrorCode error = ACS_ERR_OS(Generic, subcode, fallback, static_cast<acs::u32>(ort_error));
    api.ReleaseStatus(status);
    return error;
}

/** 途中失敗時にも OrtSession を必ず解放する所有ガード。 */
struct FScopedOrtSession {
    const OrtApi& api;
    OrtSession* value = nullptr;
    ~FScopedOrtSession() noexcept
    {
        if (value) api.ReleaseSession(value);
    }
    OrtSession* Release() noexcept
    {
        OrtSession* result = value;
        value = nullptr;
        return result;
    }
};

/** 途中失敗時にも OrtTypeInfo を必ず解放する所有ガード。 */
struct FScopedOrtTypeInfo {
    const OrtApi& api;
    OrtTypeInfo* value = nullptr;
    ~FScopedOrtTypeInfo() noexcept
    {
        if (value) api.ReleaseTypeInfo(value);
    }
};

/** OrtAllocator が返した名前文字列を必ず同じ allocator へ返す所有ガード。 */
struct FScopedOrtName {
    OrtAllocator* allocator = nullptr;
    char* value = nullptr;
    ~FScopedOrtName() noexcept
    {
        if (allocator && value) allocator->Free(allocator, value);
    }
};

/** 推論用 OrtMemoryInfo の所有ガード。 */
struct FScopedOrtMemoryInfo {
    const OrtApi& api;
    OrtMemoryInfo* value = nullptr;
    ~FScopedOrtMemoryInfo() noexcept
    {
        if (value) api.ReleaseMemoryInfo(value);
    }
};

/** 入出力 OrtValue の所有ガード。 */
struct FScopedOrtValue {
    const OrtApi& api;
    OrtValue* value = nullptr;
    ~FScopedOrtValue() noexcept
    {
        if (value) api.ReleaseValue(value);
    }
};

/** 出力テンソル形状情報の所有ガード。 */
struct FScopedOrtTensorShapeInfo {
    const OrtApi& api;
    OrtTensorTypeAndShapeInfo* value = nullptr;
    ~FScopedOrtTensorShapeInfo() noexcept
    {
        if (value) api.ReleaseTensorTypeAndShapeInfo(value);
    }
};

void CopyCString(char* dst, acs::u32 dst_size, const char* src) noexcept {
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    std::strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = 0;
}

bool Utf8ToWide(const char* src, wchar_t* dst, acs::u32 dst_count) noexcept {
    if (!src || !dst || dst_count == 0) return false;
    const int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, static_cast<int>(dst_count));
    if (n <= 0) {
        dst[0] = 0;
        return false;
    }
    return true;
}

} // namespace

struct FOnnxMlRuntime::FImpl {
    struct FModel {
        acs::u64   m_Id = 0;
        OrtSession* m_Session = nullptr;
        char       m_InputName[128] = {};
        char       m_OutputName[128] = {};
        int64_t    m_InputShape[8] = {};
        int64_t    m_OutputShape[8] = {};
        acs::u32   m_InputRank = 0;
        acs::u32   m_OutputRank = 0;
        acs::u32   m_InputCount = 0;
        acs::u32   m_OutputCount = 0;
        bool       m_bUsed = false;
    };

    static constexpr acs::u32 kMaxModels = 16;

    const OrtApi*      m_Api = nullptr;
    OrtEnv*            m_Env = nullptr;
    OrtSessionOptions* m_SessionOptions = nullptr;
    OrtAllocator*      m_Allocator = nullptr;
    FModel             m_Models[kMaxModels] = {};
    acs::u64           m_NextId = 1;
    bool               m_bInitialized = false;

    FModel* Find(acs::game::FMlModelHandle h) noexcept {
        if (!h.IsValid()) return nullptr;
        for (acs::u32 i = 0; i < kMaxModels; ++i) {
            if (m_Models[i].m_bUsed && m_Models[i].m_Id == h.m_Opaque) {
                return &m_Models[i];
            }
        }
        return nullptr;
    }
};

FOnnxMlRuntime::FOnnxMlRuntime() noexcept {
    m_Impl = new FImpl();
}

FOnnxMlRuntime::~FOnnxMlRuntime() noexcept {
    Shutdown();
    delete m_Impl;
    m_Impl = nullptr;
}

TResult<void> FOnnxMlRuntime::Init() noexcept {
    if (!m_Impl) return ACS_ERR(Generic, kSubOrtInitFailed, "FOnnxMlRuntime allocation failed");
    if (m_Impl->m_bInitialized) return Ok();

    m_Impl->m_Api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!m_Impl->m_Api) {
        return ACS_ERR(Generic, kSubOrtInitFailed, "OrtGetApi returned null");
    }
    const OrtApi& api = *m_Impl->m_Api;

    if (auto result = OrtStatusToResult(api,
            api.CreateEnv(ORT_LOGGING_LEVEL_WARNING, "ACS", &m_Impl->m_Env),
            kSubOrtInitFailed, "CreateEnv failed"); result.IsErr()) {
        Shutdown();
        return result;
    }
    if (auto result = OrtStatusToResult(api,
            api.CreateSessionOptions(&m_Impl->m_SessionOptions),
            kSubOrtInitFailed, "CreateSessionOptions failed"); result.IsErr()) {
        Shutdown();
        return result;
    }
    if (auto result = OrtStatusToResult(api, api.SetIntraOpNumThreads(m_Impl->m_SessionOptions, 1), kSubOrtInitFailed,
                                        "SetIntraOpNumThreads failed");
        result.IsErr()) {
        Shutdown();
        return result;
    }
    if (auto result =
            OrtStatusToResult(api, api.SetSessionGraphOptimizationLevel(m_Impl->m_SessionOptions, ORT_ENABLE_BASIC),
                              kSubOrtInitFailed, "SetSessionGraphOptimizationLevel failed");
        result.IsErr()) {
        Shutdown();
        return result;
    }
    if (auto result = OrtStatusToResult(api,
            api.GetAllocatorWithDefaultOptions(&m_Impl->m_Allocator),
            kSubOrtInitFailed, "GetAllocatorWithDefaultOptions failed"); result.IsErr()) {
        Shutdown();
        return result;
    }

    m_Impl->m_bInitialized = true;
    ACS_LOG_INFO("FOnnxMlRuntime initialized (ONNX Runtime API %u)", ORT_API_VERSION);
    return Ok();
}

void FOnnxMlRuntime::Shutdown() noexcept {
    if (!m_Impl || !m_Impl->m_Api) return;
    const OrtApi& api = *m_Impl->m_Api;
    for (acs::u32 i = 0; i < FImpl::kMaxModels; ++i) {
        FImpl::FModel& m = m_Impl->m_Models[i];
        if (m.m_Session) {
            api.ReleaseSession(m.m_Session);
        }
        m = FImpl::FModel{};
    }
    if (m_Impl->m_SessionOptions) {
        api.ReleaseSessionOptions(m_Impl->m_SessionOptions);
        m_Impl->m_SessionOptions = nullptr;
    }
    if (m_Impl->m_Env) {
        api.ReleaseEnv(m_Impl->m_Env);
        m_Impl->m_Env = nullptr;
    }
    m_Impl->m_Allocator = nullptr;
    m_Impl->m_Api = nullptr;
    m_Impl->m_bInitialized = false;
}

TResult<game::FMlModelHandle> FOnnxMlRuntime::LoadModel(const char* model_path) noexcept {
    if (!m_Impl || !m_Impl->m_bInitialized) {
        return TResult<game::FMlModelHandle>(ACS_ERR(Generic, game::ml_err::kSub_NotImplemented,
            "FOnnxMlRuntime::LoadModel called before Init"));
    }
    if (!model_path || model_path[0] == 0) {
        return TResult<game::FMlModelHandle>(ACS_ERR(Generic, game::ml_err::kSub_InvalidArg,
            "model_path is null or empty"));
    }

    FImpl::FModel* slot = nullptr;
    for (acs::u32 i = 0; i < FImpl::kMaxModels; ++i) {
        if (!m_Impl->m_Models[i].m_bUsed) {
            slot = &m_Impl->m_Models[i];
            break;
        }
    }
    if (!slot) {
        return TResult<game::FMlModelHandle>(ACS_ERR(Generic, kSubOrtPoolFull, "ONNX model pool full"));
    }

    wchar_t wpath[MAX_PATH] = {};
    if (!Utf8ToWide(model_path, wpath, static_cast<acs::u32>(MAX_PATH))) {
        return TResult<game::FMlModelHandle>(ACS_ERR(Generic, game::ml_err::kSub_InvalidArg,
            "model_path UTF-8 conversion failed"));
    }

    const OrtApi& api = *m_Impl->m_Api;
    FScopedOrtSession session{api};
    if (auto result =
            OrtStatusToResult(api, api.CreateSession(m_Impl->m_Env, wpath, m_Impl->m_SessionOptions, &session.value),
                              kSubOrtLoadFailed, "CreateSession failed");
        result.IsErr()) {
        return TResult<game::FMlModelHandle>(result.Error());
    }

    size_t input_count = 0;
    size_t output_count = 0;
    if (auto result = OrtStatusToResult(api, api.SessionGetInputCount(session.value, &input_count), kSubOrtLoadFailed,
                                        "SessionGetInputCount failed");
        result.IsErr()) {
        return TResult<game::FMlModelHandle>(result.Error());
    }
    if (auto result = OrtStatusToResult(api, api.SessionGetOutputCount(session.value, &output_count), kSubOrtLoadFailed,
                                        "SessionGetOutputCount failed");
        result.IsErr()) {
        return TResult<game::FMlModelHandle>(result.Error());
    }
    if (input_count != 1 || output_count != 1) {
        return TResult<game::FMlModelHandle>(ACS_ERR(Generic, kSubOrtShapeMismatch,
            "Only 1-input/1-output ONNX models are supported by this seam"));
    }

    FScopedOrtName input_name{m_Impl->m_Allocator};
    FScopedOrtName output_name{m_Impl->m_Allocator};
    if (auto result =
            OrtStatusToResult(api, api.SessionGetInputName(session.value, 0, m_Impl->m_Allocator, &input_name.value),
                              kSubOrtLoadFailed, "SessionGetInputName failed");
        result.IsErr()) {
        return TResult<game::FMlModelHandle>(result.Error());
    }
    if (auto result =
            OrtStatusToResult(api, api.SessionGetOutputName(session.value, 0, m_Impl->m_Allocator, &output_name.value),
                              kSubOrtLoadFailed, "SessionGetOutputName failed");
        result.IsErr()) {
        return TResult<game::FMlModelHandle>(result.Error());
    }
    if (input_name.value == nullptr || output_name.value == nullptr) {
        return TResult<game::FMlModelHandle>(
            ACS_ERR(Generic, kSubOrtLoadFailed, "ONNX Runtime returned a null input or output name"));
    }

    FScopedOrtTypeInfo input_type{api};
    FScopedOrtTypeInfo output_type{api};
    if (auto result = OrtStatusToResult(api, api.SessionGetInputTypeInfo(session.value, 0, &input_type.value),
                                        kSubOrtLoadFailed, "SessionGetInputTypeInfo failed");
        result.IsErr()) {
        return TResult<game::FMlModelHandle>(result.Error());
    }
    if (auto result = OrtStatusToResult(api, api.SessionGetOutputTypeInfo(session.value, 0, &output_type.value),
                                        kSubOrtLoadFailed, "SessionGetOutputTypeInfo failed");
        result.IsErr()) {
        return TResult<game::FMlModelHandle>(result.Error());
    }

    const OrtTensorTypeAndShapeInfo* input_tensor = nullptr;
    const OrtTensorTypeAndShapeInfo* output_tensor = nullptr;
    if (auto result = OrtStatusToResult(api, api.CastTypeInfoToTensorInfo(input_type.value, &input_tensor),
                                        kSubOrtShapeMismatch, "Input is not a tensor");
        result.IsErr()) {
        return TResult<game::FMlModelHandle>(result.Error());
    }
    if (auto result = OrtStatusToResult(api, api.CastTypeInfoToTensorInfo(output_type.value, &output_tensor),
                                        kSubOrtShapeMismatch, "Output is not a tensor");
        result.IsErr()) {
        return TResult<game::FMlModelHandle>(result.Error());
    }
    if (input_tensor == nullptr || output_tensor == nullptr) {
        return TResult<game::FMlModelHandle>(
            ACS_ERR(Generic, kSubOrtShapeMismatch, "ONNX input or output is not a tensor"));
    }

    size_t input_rank = 0;
    size_t output_rank = 0;
    if (auto result = OrtStatusToResult(api, api.GetDimensionsCount(input_tensor, &input_rank), kSubOrtLoadFailed,
                                        "Get input dimensions count failed");
        result.IsErr()) {
        return TResult<game::FMlModelHandle>(result.Error());
    }
    if (auto result = OrtStatusToResult(api, api.GetDimensionsCount(output_tensor, &output_rank), kSubOrtLoadFailed,
                                        "Get output dimensions count failed");
        result.IsErr()) {
        return TResult<game::FMlModelHandle>(result.Error());
    }
    if (input_rank > 8 || output_rank > 8) {
        return TResult<game::FMlModelHandle>(ACS_ERR(Generic, kSubOrtShapeMismatch,
            "ONNX tensor rank exceeds ACS fixed limit"));
    }

    int64_t input_shape[8] = {};
    int64_t output_shape[8] = {};
    if (auto result = OrtStatusToResult(api, api.GetDimensions(input_tensor, input_shape, input_rank),
                                        kSubOrtLoadFailed, "Get input dimensions failed");
        result.IsErr()) {
        return TResult<game::FMlModelHandle>(result.Error());
    }
    if (auto result = OrtStatusToResult(api, api.GetDimensions(output_tensor, output_shape, output_rank),
                                        kSubOrtLoadFailed, "Get output dimensions failed");
        result.IsErr()) {
        return TResult<game::FMlModelHandle>(result.Error());
    }

    // ここまで全検証に成功してからスロットへ所有権とメタデータを確定する。
    *slot = FImpl::FModel{};
    CopyCString(slot->m_InputName, static_cast<acs::u32>(sizeof(slot->m_InputName)), input_name.value);
    CopyCString(slot->m_OutputName, static_cast<acs::u32>(sizeof(slot->m_OutputName)), output_name.value);
    for (acs::u32 i = 0; i < 8; ++i) {
        slot->m_InputShape[i] = input_shape[i];
        slot->m_OutputShape[i] = output_shape[i];
    }
    slot->m_Session = session.Release();
    slot->m_InputRank = static_cast<acs::u32>(input_rank);
    slot->m_OutputRank = static_cast<acs::u32>(output_rank);
    slot->m_InputCount = 1;
    for (acs::u32 i = 0; i < slot->m_InputRank; ++i) {
        if (slot->m_InputShape[i] > 0) {
            slot->m_InputCount *= static_cast<acs::u32>(slot->m_InputShape[i]);
        }
    }
    slot->m_OutputCount = 1;
    for (acs::u32 i = 0; i < slot->m_OutputRank; ++i) {
        if (slot->m_OutputShape[i] > 0) {
            slot->m_OutputCount *= static_cast<acs::u32>(slot->m_OutputShape[i]);
        }
    }
    slot->m_Id = m_Impl->m_NextId++;
    slot->m_bUsed = true;

    return TResult<game::FMlModelHandle>(OkInit, game::FMlModelHandle{slot->m_Id});
}

TResult<void> FOnnxMlRuntime::UnloadModel(game::FMlModelHandle h) noexcept {
    if (!m_Impl || !m_Impl->m_bInitialized) return ACS_ERR(Generic, game::ml_err::kSub_NotImplemented, "Init first");
    FImpl::FModel* model = m_Impl->Find(h);
    if (!model) return Ok();
    if (model->m_Session) {
        m_Impl->m_Api->ReleaseSession(model->m_Session);
    }
    *model = FImpl::FModel{};
    return Ok();
}

TResult<void> FOnnxMlRuntime::RunInference(game::FMlModelHandle h,
                                           const f32* inputs, u32 in_count,
                                           f32* outputs, u32 out_count) noexcept {
    if (!m_Impl || !m_Impl->m_bInitialized) {
        return ACS_ERR(Generic, game::ml_err::kSub_NotImplemented, "FOnnxMlRuntime::RunInference called before Init");
    }
    if (!inputs || !outputs || in_count == 0 || out_count == 0) {
        return ACS_ERR(Generic, game::ml_err::kSub_InvalidArg, "RunInference invalid buffers/counts");
    }
    FImpl::FModel* model = m_Impl->Find(h);
    if (!model || !model->m_Session) {
        return ACS_ERR(Generic, kSubOrtInvalidHandle, "Invalid ONNX model handle");
    }

    int64_t input_shape[8] = {};
    for (u32 i = 0; i < model->m_InputRank; ++i) input_shape[i] = model->m_InputShape[i];
    acs::u32 static_product = 1;
    int dynamic_axis = -1;
    for (u32 i = 0; i < model->m_InputRank; ++i) {
        if (input_shape[i] <= 0) {
            if (dynamic_axis < 0) dynamic_axis = static_cast<int>(i);
            input_shape[i] = 1;
        } else {
            static_product *= static_cast<acs::u32>(input_shape[i]);
        }
    }
    if (dynamic_axis >= 0) {
        input_shape[dynamic_axis] = static_product > 0 ? static_cast<int64_t>(in_count / static_product) : in_count;
    } else if (model->m_InputCount != in_count) {
        return ACS_ERR(Generic, kSubOrtShapeMismatch, "Input count does not match ONNX tensor shape");
    }

    const OrtApi& api = *m_Impl->m_Api;
    FScopedOrtMemoryInfo memory_info{api};
    if (auto result =
            OrtStatusToResult(api, api.CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info.value),
                              kSubOrtRunFailed, "CreateCpuMemoryInfo failed");
        result.IsErr()) {
        return result;
    }

    FScopedOrtValue input_value{api};
    if (auto result = OrtStatusToResult(api,
                                        api.CreateTensorWithDataAsOrtValue(memory_info.value, const_cast<f32*>(inputs),
                                                                           static_cast<size_t>(in_count) * sizeof(f32),
                                                                           input_shape, model->m_InputRank,
                                                                           ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                                           &input_value.value),
                                        kSubOrtRunFailed, "CreateTensorWithDataAsOrtValue failed");
        result.IsErr()) {
        return result;
    }

    const char* input_names[1] = { model->m_InputName };
    const char* output_names[1] = { model->m_OutputName };
    const OrtValue* input_values[1] = {input_value.value};
    FScopedOrtValue output_value{api};

    const TResult<void> run_result = OrtStatusToResult(api,
                                                       api.Run(model->m_Session, nullptr, input_names, input_values, 1,
                                                               output_names, 1, &output_value.value),
                                                       kSubOrtRunFailed, "ONNX Runtime Run failed");
    if (run_result.IsErr()) return run_result;
    if (output_value.value == nullptr) {
        return ACS_ERR(Generic, kSubOrtRunFailed, "ONNX Runtime returned no output value");
    }

    FScopedOrtTensorShapeInfo output_info{api};
    if (auto result = OrtStatusToResult(api, api.GetTensorTypeAndShape(output_value.value, &output_info.value),
                                        kSubOrtRunFailed, "GetTensorTypeAndShape failed");
        result.IsErr()) {
        return result;
    }
    size_t elem_count = 0;
    if (auto result = OrtStatusToResult(api, api.GetTensorShapeElementCount(output_info.value, &elem_count),
                                        kSubOrtRunFailed, "GetTensorShapeElementCount failed");
        result.IsErr()) {
        return result;
    }
    if (elem_count > out_count) {
        return ACS_ERR(Generic, kSubOrtShapeMismatch, "Output buffer too small for ONNX result");
    }

    f32* ort_output = nullptr;
    if (auto result =
            OrtStatusToResult(api, api.GetTensorMutableData(output_value.value, reinterpret_cast<void**>(&ort_output)),
                              kSubOrtRunFailed, "GetTensorMutableData failed");
        result.IsErr()) {
        return result;
    }
    for (size_t i = 0; i < elem_count; ++i) {
        outputs[i] = ort_output[i];
    }
    return Ok();
}

} // namespace acs::mlonnx
