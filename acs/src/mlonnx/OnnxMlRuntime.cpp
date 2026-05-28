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

acs::TResult<void> OrtStatusToResult(const OrtApi& Api, OrtStatus* Status,
                                      acs::u16 Subcode, const char* Fallback) noexcept {
    if (Status == nullptr) {
        return acs::Ok();
    }
    const char* Message = Api.GetErrorMessage(Status);
    acs::FErrorCode Error = ACS_ERR(Generic, Subcode, Message ? Message : Fallback);
    Api.ReleaseStatus(Status);
    return Error;
}

void CopyCString(char* Dst, acs::u32 DstSize, const char* Src) noexcept {
    if (!Dst || DstSize == 0) return;
    if (!Src) {
        Dst[0] = 0;
        return;
    }
    std::strncpy(Dst, Src, DstSize - 1);
    Dst[DstSize - 1] = 0;
}

bool Utf8ToWide(const char* Src, wchar_t* Dst, acs::u32 DstCount) noexcept {
    if (!Src || !Dst || DstCount == 0) return false;
    const int n = MultiByteToWideChar(CP_UTF8, 0, Src, -1, Dst, static_cast<int>(DstCount));
    if (n <= 0) {
        Dst[0] = 0;
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

    FModel* Find(acs::game::MlModelHandle H) noexcept {
        if (!H.IsValid()) return nullptr;
        for (acs::u32 i = 0; i < kMaxModels; ++i) {
            if (m_Models[i].m_bUsed && m_Models[i].m_Id == H.m_Opaque) {
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
    const OrtApi& Api = *m_Impl->m_Api;

    if (auto Result = OrtStatusToResult(Api,
            Api.CreateEnv(ORT_LOGGING_LEVEL_WARNING, "ACS", &m_Impl->m_Env),
            kSubOrtInitFailed, "CreateEnv failed"); Result.IsErr()) {
        return Result;
    }
    if (auto Result = OrtStatusToResult(Api,
            Api.CreateSessionOptions(&m_Impl->m_SessionOptions),
            kSubOrtInitFailed, "CreateSessionOptions failed"); Result.IsErr()) {
        Shutdown();
        return Result;
    }
    Api.SetIntraOpNumThreads(m_Impl->m_SessionOptions, 1);
    Api.SetSessionGraphOptimizationLevel(m_Impl->m_SessionOptions, ORT_ENABLE_BASIC);
    if (auto Result = OrtStatusToResult(Api,
            Api.GetAllocatorWithDefaultOptions(&m_Impl->m_Allocator),
            kSubOrtInitFailed, "GetAllocatorWithDefaultOptions failed"); Result.IsErr()) {
        Shutdown();
        return Result;
    }

    m_Impl->m_bInitialized = true;
    ACS_LOG_INFO("FOnnxMlRuntime initialized (ONNX Runtime API %u)", ORT_API_VERSION);
    return Ok();
}

void FOnnxMlRuntime::Shutdown() noexcept {
    if (!m_Impl || !m_Impl->m_Api) return;
    const OrtApi& Api = *m_Impl->m_Api;
    for (acs::u32 i = 0; i < FImpl::kMaxModels; ++i) {
        FImpl::FModel& m = m_Impl->m_Models[i];
        if (m.m_Session) {
            Api.ReleaseSession(m.m_Session);
        }
        m = FImpl::FModel{};
    }
    if (m_Impl->m_SessionOptions) {
        Api.ReleaseSessionOptions(m_Impl->m_SessionOptions);
        m_Impl->m_SessionOptions = nullptr;
    }
    if (m_Impl->m_Env) {
        Api.ReleaseEnv(m_Impl->m_Env);
        m_Impl->m_Env = nullptr;
    }
    m_Impl->m_Allocator = nullptr;
    m_Impl->m_Api = nullptr;
    m_Impl->m_bInitialized = false;
}

TResult<game::MlModelHandle> FOnnxMlRuntime::LoadModel(const char* ModelPath) noexcept {
    if (!m_Impl || !m_Impl->m_bInitialized) {
        return TResult<game::MlModelHandle>(ACS_ERR(Generic, game::ml_err::kSub_NotImplemented,
            "FOnnxMlRuntime::LoadModel called before Init"));
    }
    if (!ModelPath || ModelPath[0] == 0) {
        return TResult<game::MlModelHandle>(ACS_ERR(Generic, game::ml_err::kSub_InvalidArg,
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
        return TResult<game::MlModelHandle>(ACS_ERR(Generic, kSubOrtPoolFull, "ONNX model pool full"));
    }

    wchar_t WPath[MAX_PATH] = {};
    if (!Utf8ToWide(ModelPath, WPath, static_cast<acs::u32>(MAX_PATH))) {
        return TResult<game::MlModelHandle>(ACS_ERR(Generic, game::ml_err::kSub_InvalidArg,
            "model_path UTF-8 conversion failed"));
    }

    const OrtApi& Api = *m_Impl->m_Api;
    OrtSession* Session = nullptr;
    if (auto Result = OrtStatusToResult(Api,
            Api.CreateSession(m_Impl->m_Env, WPath, m_Impl->m_SessionOptions, &Session),
            kSubOrtLoadFailed, "CreateSession failed"); Result.IsErr()) {
        return TResult<game::MlModelHandle>(Result.Error());
    }

    size_t InputCount = 0;
    size_t OutputCount = 0;
    Api.SessionGetInputCount(Session, &InputCount);
    Api.SessionGetOutputCount(Session, &OutputCount);
    if (InputCount != 1 || OutputCount != 1) {
        Api.ReleaseSession(Session);
        return TResult<game::MlModelHandle>(ACS_ERR(Generic, kSubOrtShapeMismatch,
            "Only 1-input/1-output ONNX models are supported by this seam"));
    }

    char* InputName = nullptr;
    char* OutputName = nullptr;
    Api.SessionGetInputName(Session, 0, m_Impl->m_Allocator, &InputName);
    Api.SessionGetOutputName(Session, 0, m_Impl->m_Allocator, &OutputName);
    CopyCString(slot->m_InputName, static_cast<acs::u32>(sizeof(slot->m_InputName)), InputName);
    CopyCString(slot->m_OutputName, static_cast<acs::u32>(sizeof(slot->m_OutputName)), OutputName);
    if (InputName) m_Impl->m_Allocator->Free(m_Impl->m_Allocator, InputName);
    if (OutputName) m_Impl->m_Allocator->Free(m_Impl->m_Allocator, OutputName);

    OrtTypeInfo* InputType = nullptr;
    OrtTypeInfo* OutputType = nullptr;
    Api.SessionGetInputTypeInfo(Session, 0, &InputType);
    Api.SessionGetOutputTypeInfo(Session, 0, &OutputType);
    const OrtTensorTypeAndShapeInfo* InputTensor = nullptr;
    const OrtTensorTypeAndShapeInfo* OutputTensor = nullptr;
    Api.CastTypeInfoToTensorInfo(InputType, &InputTensor);
    Api.CastTypeInfoToTensorInfo(OutputType, &OutputTensor);
    size_t InputRank = 0;
    size_t OutputRank = 0;
    Api.GetDimensionsCount(InputTensor, &InputRank);
    Api.GetDimensionsCount(OutputTensor, &OutputRank);
    if (InputRank > 8 || OutputRank > 8) {
        if (InputType) Api.ReleaseTypeInfo(InputType);
        if (OutputType) Api.ReleaseTypeInfo(OutputType);
        Api.ReleaseSession(Session);
        return TResult<game::MlModelHandle>(ACS_ERR(Generic, kSubOrtShapeMismatch,
            "ONNX tensor rank exceeds ACS fixed limit"));
    }
    Api.GetDimensions(InputTensor, slot->m_InputShape, InputRank);
    Api.GetDimensions(OutputTensor, slot->m_OutputShape, OutputRank);
    if (InputType) Api.ReleaseTypeInfo(InputType);
    if (OutputType) Api.ReleaseTypeInfo(OutputType);

    slot->m_Session = Session;
    slot->m_InputRank = static_cast<acs::u32>(InputRank);
    slot->m_OutputRank = static_cast<acs::u32>(OutputRank);
    slot->m_InputCount = 1;
    for (acs::u32 i = 0; i < slot->m_InputRank; ++i) {
        if (slot->m_InputShape[i] > 0) slot->m_InputCount *= static_cast<acs::u32>(slot->m_InputShape[i]);
    }
    slot->m_OutputCount = 1;
    for (acs::u32 i = 0; i < slot->m_OutputRank; ++i) {
        if (slot->m_OutputShape[i] > 0) slot->m_OutputCount *= static_cast<acs::u32>(slot->m_OutputShape[i]);
    }
    slot->m_Id = m_Impl->m_NextId++;
    slot->m_bUsed = true;

    return TResult<game::MlModelHandle>(OkInit, game::MlModelHandle{slot->m_Id});
}

TResult<void> FOnnxMlRuntime::UnloadModel(game::MlModelHandle H) noexcept {
    if (!m_Impl || !m_Impl->m_bInitialized) return ACS_ERR(Generic, game::ml_err::kSub_NotImplemented, "Init first");
    FImpl::FModel* Model = m_Impl->Find(H);
    if (!Model) return Ok();
    if (Model->m_Session) {
        m_Impl->m_Api->ReleaseSession(Model->m_Session);
    }
    *Model = FImpl::FModel{};
    return Ok();
}

TResult<void> FOnnxMlRuntime::RunInference(game::MlModelHandle H,
                                           const f32* Inputs, u32 InCount,
                                           f32* Outputs, u32 OutCount) noexcept {
    if (!m_Impl || !m_Impl->m_bInitialized) {
        return ACS_ERR(Generic, game::ml_err::kSub_NotImplemented, "FOnnxMlRuntime::RunInference called before Init");
    }
    if (!Inputs || !Outputs || InCount == 0 || OutCount == 0) {
        return ACS_ERR(Generic, game::ml_err::kSub_InvalidArg, "RunInference invalid buffers/counts");
    }
    FImpl::FModel* Model = m_Impl->Find(H);
    if (!Model || !Model->m_Session) {
        return ACS_ERR(Generic, kSubOrtInvalidHandle, "Invalid ONNX model handle");
    }

    int64_t InputShape[8] = {};
    for (u32 i = 0; i < Model->m_InputRank; ++i) InputShape[i] = Model->m_InputShape[i];
    acs::u32 StaticProduct = 1;
    int DynamicAxis = -1;
    for (u32 i = 0; i < Model->m_InputRank; ++i) {
        if (InputShape[i] <= 0) {
            if (DynamicAxis < 0) DynamicAxis = static_cast<int>(i);
            InputShape[i] = 1;
        } else {
            StaticProduct *= static_cast<acs::u32>(InputShape[i]);
        }
    }
    if (DynamicAxis >= 0) {
        InputShape[DynamicAxis] = StaticProduct > 0 ? static_cast<int64_t>(InCount / StaticProduct) : InCount;
    } else if (Model->m_InputCount != InCount) {
        return ACS_ERR(Generic, kSubOrtShapeMismatch, "Input count does not match ONNX tensor shape");
    }

    const OrtApi& Api = *m_Impl->m_Api;
    OrtMemoryInfo* MemoryInfo = nullptr;
    if (auto Result = OrtStatusToResult(Api,
            Api.CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &MemoryInfo),
            kSubOrtRunFailed, "CreateCpuMemoryInfo failed"); Result.IsErr()) {
        return Result;
    }

    OrtValue* InputValue = nullptr;
    if (auto Result = OrtStatusToResult(Api,
            Api.CreateTensorWithDataAsOrtValue(MemoryInfo,
                const_cast<f32*>(Inputs), static_cast<size_t>(InCount) * sizeof(f32),
                InputShape, Model->m_InputRank, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &InputValue),
            kSubOrtRunFailed, "CreateTensorWithDataAsOrtValue failed"); Result.IsErr()) {
        Api.ReleaseMemoryInfo(MemoryInfo);
        return Result;
    }

    const char* InputNames[1] = { Model->m_InputName };
    const char* OutputNames[1] = { Model->m_OutputName };
    const OrtValue* InputValues[1] = { InputValue };
    OrtValue* OutputValues[1] = { nullptr };

    TResult<void> RunResult = OrtStatusToResult(Api,
        Api.Run(Model->m_Session, nullptr, InputNames, InputValues, 1, OutputNames, 1, OutputValues),
        kSubOrtRunFailed, "ONNX Runtime Run failed");
    Api.ReleaseValue(InputValue);
    Api.ReleaseMemoryInfo(MemoryInfo);
    if (RunResult.IsErr()) return RunResult;

    OrtTensorTypeAndShapeInfo* OutputInfo = nullptr;
    Api.GetTensorTypeAndShape(OutputValues[0], &OutputInfo);
    size_t ElemCount = 0;
    Api.GetTensorShapeElementCount(OutputInfo, &ElemCount);
    Api.ReleaseTensorTypeAndShapeInfo(OutputInfo);
    if (ElemCount > OutCount) {
        Api.ReleaseValue(OutputValues[0]);
        return ACS_ERR(Generic, kSubOrtShapeMismatch, "Output buffer too small for ONNX result");
    }

    f32* OrtOutput = nullptr;
    if (auto Result = OrtStatusToResult(Api,
            Api.GetTensorMutableData(OutputValues[0], reinterpret_cast<void**>(&OrtOutput)),
            kSubOrtRunFailed, "GetTensorMutableData failed"); Result.IsErr()) {
        Api.ReleaseValue(OutputValues[0]);
        return Result;
    }
    for (size_t i = 0; i < ElemCount; ++i) {
        Outputs[i] = OrtOutput[i];
    }
    Api.ReleaseValue(OutputValues[0]);
    return Ok();
}

} // namespace acs::mlonnx
