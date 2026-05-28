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
    const char* msg = api.GetErrorMessage(status);
    acs::FErrorCode err = ACS_ERR(Generic, subcode, msg ? msg : fallback);
    api.ReleaseStatus(status);
    return err;
}

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

struct FOnnxMlRuntime::Impl {
    struct Model {
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
        bool       m_Used = false;
    };

    static constexpr acs::u32 kMaxModels = 16;

    const OrtApi*      m_Api = nullptr;
    OrtEnv*            m_Env = nullptr;
    OrtSessionOptions* m_SessionOptions = nullptr;
    OrtAllocator*      m_Allocator = nullptr;
    Model              m_Models[kMaxModels] = {};
    acs::u64           m_NextId = 1;
    bool               m_Initialized = false;

    Model* Find(acs::game::MlModelHandle h) noexcept {
        if (!h.IsValid()) return nullptr;
        for (acs::u32 i = 0; i < kMaxModels; ++i) {
            if (m_Models[i].m_Used && m_Models[i].m_Id == h.m_Opaque) {
                return &m_Models[i];
            }
        }
        return nullptr;
    }
};

FOnnxMlRuntime::FOnnxMlRuntime() noexcept {
    m_Impl = new Impl();
}

FOnnxMlRuntime::~FOnnxMlRuntime() noexcept {
    Shutdown();
    delete m_Impl;
    m_Impl = nullptr;
}

TResult<void> FOnnxMlRuntime::Init() noexcept {
    if (!m_Impl) return ACS_ERR(Generic, kSubOrtInitFailed, "FOnnxMlRuntime allocation failed");
    if (m_Impl->m_Initialized) return Ok();

    m_Impl->m_Api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!m_Impl->m_Api) {
        return ACS_ERR(Generic, kSubOrtInitFailed, "OrtGetApi returned null");
    }
    const OrtApi& api = *m_Impl->m_Api;

    if (auto r = OrtStatusToResult(api,
            api.CreateEnv(ORT_LOGGING_LEVEL_WARNING, "ACS", &m_Impl->m_Env),
            kSubOrtInitFailed, "CreateEnv failed"); r.IsErr()) {
        return r;
    }
    if (auto r = OrtStatusToResult(api,
            api.CreateSessionOptions(&m_Impl->m_SessionOptions),
            kSubOrtInitFailed, "CreateSessionOptions failed"); r.IsErr()) {
        Shutdown();
        return r;
    }
    api.SetIntraOpNumThreads(m_Impl->m_SessionOptions, 1);
    api.SetSessionGraphOptimizationLevel(m_Impl->m_SessionOptions, ORT_ENABLE_BASIC);
    if (auto r = OrtStatusToResult(api,
            api.GetAllocatorWithDefaultOptions(&m_Impl->m_Allocator),
            kSubOrtInitFailed, "GetAllocatorWithDefaultOptions failed"); r.IsErr()) {
        Shutdown();
        return r;
    }

    m_Impl->m_Initialized = true;
    ACS_LOG_INFO("FOnnxMlRuntime initialized (ONNX Runtime API %u)", ORT_API_VERSION);
    return Ok();
}

void FOnnxMlRuntime::Shutdown() noexcept {
    if (!m_Impl || !m_Impl->m_Api) return;
    const OrtApi& api = *m_Impl->m_Api;
    for (acs::u32 i = 0; i < Impl::kMaxModels; ++i) {
        Impl::Model& m = m_Impl->m_Models[i];
        if (m.m_Session) {
            api.ReleaseSession(m.m_Session);
        }
        m = Impl::Model{};
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
    m_Impl->m_Initialized = false;
}

TResult<game::MlModelHandle> FOnnxMlRuntime::LoadModel(const char* model_path) noexcept {
    if (!m_Impl || !m_Impl->m_Initialized) {
        return TResult<game::MlModelHandle>(ACS_ERR(Generic, game::ml_err::kSub_NotImplemented,
            "FOnnxMlRuntime::LoadModel called before Init"));
    }
    if (!model_path || model_path[0] == 0) {
        return TResult<game::MlModelHandle>(ACS_ERR(Generic, game::ml_err::kSub_InvalidArg,
            "model_path is null or empty"));
    }

    Impl::Model* slot = nullptr;
    for (acs::u32 i = 0; i < Impl::kMaxModels; ++i) {
        if (!m_Impl->m_Models[i].m_Used) {
            slot = &m_Impl->m_Models[i];
            break;
        }
    }
    if (!slot) {
        return TResult<game::MlModelHandle>(ACS_ERR(Generic, kSubOrtPoolFull, "ONNX model pool full"));
    }

    wchar_t wpath[MAX_PATH] = {};
    if (!Utf8ToWide(model_path, wpath, static_cast<acs::u32>(MAX_PATH))) {
        return TResult<game::MlModelHandle>(ACS_ERR(Generic, game::ml_err::kSub_InvalidArg,
            "model_path UTF-8 conversion failed"));
    }

    const OrtApi& api = *m_Impl->m_Api;
    OrtSession* session = nullptr;
    if (auto r = OrtStatusToResult(api,
            api.CreateSession(m_Impl->m_Env, wpath, m_Impl->m_SessionOptions, &session),
            kSubOrtLoadFailed, "CreateSession failed"); r.IsErr()) {
        return TResult<game::MlModelHandle>(r.Error());
    }

    size_t input_count = 0;
    size_t output_count = 0;
    api.SessionGetInputCount(session, &input_count);
    api.SessionGetOutputCount(session, &output_count);
    if (input_count != 1 || output_count != 1) {
        api.ReleaseSession(session);
        return TResult<game::MlModelHandle>(ACS_ERR(Generic, kSubOrtShapeMismatch,
            "Only 1-input/1-output ONNX models are supported by this seam"));
    }

    char* input_name = nullptr;
    char* output_name = nullptr;
    api.SessionGetInputName(session, 0, m_Impl->m_Allocator, &input_name);
    api.SessionGetOutputName(session, 0, m_Impl->m_Allocator, &output_name);
    CopyCString(slot->m_InputName, static_cast<acs::u32>(sizeof(slot->m_InputName)), input_name);
    CopyCString(slot->m_OutputName, static_cast<acs::u32>(sizeof(slot->m_OutputName)), output_name);
    if (input_name) m_Impl->m_Allocator->Free(m_Impl->m_Allocator, input_name);
    if (output_name) m_Impl->m_Allocator->Free(m_Impl->m_Allocator, output_name);

    OrtTypeInfo* input_type = nullptr;
    OrtTypeInfo* output_type = nullptr;
    api.SessionGetInputTypeInfo(session, 0, &input_type);
    api.SessionGetOutputTypeInfo(session, 0, &output_type);
    const OrtTensorTypeAndShapeInfo* input_tensor = nullptr;
    const OrtTensorTypeAndShapeInfo* output_tensor = nullptr;
    api.CastTypeInfoToTensorInfo(input_type, &input_tensor);
    api.CastTypeInfoToTensorInfo(output_type, &output_tensor);
    size_t input_rank = 0;
    size_t output_rank = 0;
    api.GetDimensionsCount(input_tensor, &input_rank);
    api.GetDimensionsCount(output_tensor, &output_rank);
    if (input_rank > 8 || output_rank > 8) {
        if (input_type) api.ReleaseTypeInfo(input_type);
        if (output_type) api.ReleaseTypeInfo(output_type);
        api.ReleaseSession(session);
        return TResult<game::MlModelHandle>(ACS_ERR(Generic, kSubOrtShapeMismatch,
            "ONNX tensor rank exceeds ACS fixed limit"));
    }
    api.GetDimensions(input_tensor, slot->m_InputShape, input_rank);
    api.GetDimensions(output_tensor, slot->m_OutputShape, output_rank);
    if (input_type) api.ReleaseTypeInfo(input_type);
    if (output_type) api.ReleaseTypeInfo(output_type);

    slot->m_Session = session;
    slot->m_InputRank = static_cast<acs::u32>(input_rank);
    slot->m_OutputRank = static_cast<acs::u32>(output_rank);
    slot->m_InputCount = 1;
    for (acs::u32 i = 0; i < slot->m_InputRank; ++i) {
        if (slot->m_InputShape[i] > 0) slot->m_InputCount *= static_cast<acs::u32>(slot->m_InputShape[i]);
    }
    slot->m_OutputCount = 1;
    for (acs::u32 i = 0; i < slot->m_OutputRank; ++i) {
        if (slot->m_OutputShape[i] > 0) slot->m_OutputCount *= static_cast<acs::u32>(slot->m_OutputShape[i]);
    }
    slot->m_Id = m_Impl->m_NextId++;
    slot->m_Used = true;

    return TResult<game::MlModelHandle>(OkInit, game::MlModelHandle{slot->m_Id});
}

TResult<void> FOnnxMlRuntime::UnloadModel(game::MlModelHandle h) noexcept {
    if (!m_Impl || !m_Impl->m_Initialized) return ACS_ERR(Generic, game::ml_err::kSub_NotImplemented, "Init first");
    Impl::Model* m = m_Impl->Find(h);
    if (!m) return Ok();
    if (m->m_Session) {
        m_Impl->m_Api->ReleaseSession(m->m_Session);
    }
    *m = Impl::Model{};
    return Ok();
}

TResult<void> FOnnxMlRuntime::RunInference(game::MlModelHandle h,
                                           const f32* inputs, u32 in_count,
                                           f32* outputs, u32 out_count) noexcept {
    if (!m_Impl || !m_Impl->m_Initialized) {
        return ACS_ERR(Generic, game::ml_err::kSub_NotImplemented, "FOnnxMlRuntime::RunInference called before Init");
    }
    if (!inputs || !outputs || in_count == 0 || out_count == 0) {
        return ACS_ERR(Generic, game::ml_err::kSub_InvalidArg, "RunInference invalid buffers/counts");
    }
    Impl::Model* m = m_Impl->Find(h);
    if (!m || !m->m_Session) {
        return ACS_ERR(Generic, kSubOrtInvalidHandle, "Invalid ONNX model handle");
    }

    int64_t input_shape[8] = {};
    for (u32 i = 0; i < m->m_InputRank; ++i) input_shape[i] = m->m_InputShape[i];
    acs::u32 static_product = 1;
    int dynamic_axis = -1;
    for (u32 i = 0; i < m->m_InputRank; ++i) {
        if (input_shape[i] <= 0) {
            if (dynamic_axis < 0) dynamic_axis = static_cast<int>(i);
            input_shape[i] = 1;
        } else {
            static_product *= static_cast<acs::u32>(input_shape[i]);
        }
    }
    if (dynamic_axis >= 0) {
        input_shape[dynamic_axis] = static_product > 0 ? static_cast<int64_t>(in_count / static_product) : in_count;
    } else if (m->m_InputCount != in_count) {
        return ACS_ERR(Generic, kSubOrtShapeMismatch, "Input count does not match ONNX tensor shape");
    }

    const OrtApi& api = *m_Impl->m_Api;
    OrtMemoryInfo* mem_info = nullptr;
    if (auto r = OrtStatusToResult(api,
            api.CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info),
            kSubOrtRunFailed, "CreateCpuMemoryInfo failed"); r.IsErr()) {
        return r;
    }

    OrtValue* input_value = nullptr;
    if (auto r = OrtStatusToResult(api,
            api.CreateTensorWithDataAsOrtValue(mem_info,
                const_cast<f32*>(inputs), static_cast<size_t>(in_count) * sizeof(f32),
                input_shape, m->m_InputRank, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_value),
            kSubOrtRunFailed, "CreateTensorWithDataAsOrtValue failed"); r.IsErr()) {
        api.ReleaseMemoryInfo(mem_info);
        return r;
    }

    const char* input_names[1] = { m->m_InputName };
    const char* output_names[1] = { m->m_OutputName };
    const OrtValue* input_values[1] = { input_value };
    OrtValue* output_values[1] = { nullptr };

    TResult<void> run_result = OrtStatusToResult(api,
        api.Run(m->m_Session, nullptr, input_names, input_values, 1, output_names, 1, output_values),
        kSubOrtRunFailed, "ONNX Runtime Run failed");
    api.ReleaseValue(input_value);
    api.ReleaseMemoryInfo(mem_info);
    if (run_result.IsErr()) return run_result;

    OrtTensorTypeAndShapeInfo* out_info = nullptr;
    api.GetTensorTypeAndShape(output_values[0], &out_info);
    size_t elem_count = 0;
    api.GetTensorShapeElementCount(out_info, &elem_count);
    api.ReleaseTensorTypeAndShapeInfo(out_info);
    if (elem_count > out_count) {
        api.ReleaseValue(output_values[0]);
        return ACS_ERR(Generic, kSubOrtShapeMismatch, "Output buffer too small for ONNX result");
    }

    f32* ort_output = nullptr;
    if (auto r = OrtStatusToResult(api,
            api.GetTensorMutableData(output_values[0], reinterpret_cast<void**>(&ort_output)),
            kSubOrtRunFailed, "GetTensorMutableData failed"); r.IsErr()) {
        api.ReleaseValue(output_values[0]);
        return r;
    }
    for (size_t i = 0; i < elem_count; ++i) {
        outputs[i] = ort_output[i];
    }
    api.ReleaseValue(output_values[0]);
    return Ok();
}

} // namespace acs::mlonnx
