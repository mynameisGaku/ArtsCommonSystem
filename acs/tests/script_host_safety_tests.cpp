// SPDX-License-Identifier: Apache-2.0
// CScriptHost の外部source・call・native registryトランザクション契約。
#include "test/Test.h"
#include "test/Expect.h"
#include "foundation/Platform.h"
#include "gameframework/ScriptHost.h"

using namespace acs;
using namespace acs::game;

namespace {

class CMockScriptVm final : public IScriptVm {
public:
    TResult<void> Init() noexcept override { return Ok(); }
    void Shutdown() noexcept override {}
    EScriptLanguage Language() const noexcept override { return EScriptLanguage::Custom; }

    TResult<void> LoadScript(const char* source,
                             u32 source_len,
                             const char*) noexcept override {
        ++LoadCalls;
        LastSourceLength = source_len;
        const u32 copy_count =
            source_len < static_cast<u32>(sizeof(LastSource) - 1u)
                ? source_len
                : static_cast<u32>(sizeof(LastSource) - 1u);
        for (u32 i = 0u; i < copy_count; ++i) LastSource[i] = source[i];
        LastSource[copy_count] = '\0';
        if (FailLoad) {
            return ACS_ERR(Generic, script_err::kSub_LoadFailed,
                           "mock load failure");
        }
        return Ok();
    }

    TResult<void> CallFunction(const char*,
                               const FScriptValue*,
                               u32,
                               FScriptValue* ret_out) noexcept override {
        ++CallCalls;
        if (ret_out != nullptr) {
            ret_out->kind = EScriptValueKind::Handle;
            ret_out->v.handle = ReturnHandle;
        }
        if (FailCall) {
            return ACS_ERR(Generic, script_err::kSub_CallFailed,
                           "mock call failure");
        }
        return Ok();
    }

    TResult<void> RegisterNativeFunction(const char* function_name,
                                         NativeFunction fn,
                                         void* user) noexcept override {
        ++RegisterCalls;
        LastRegisteredName = function_name;
        LastRegisteredFunction = fn;
        LastRegisteredUser = user;
        if (FailRegister) {
            return ACS_ERR(Generic, script_err::kSub_LoadFailed,
                           "mock register failure");
        }
        return Ok();
    }

    void SetGlobalNumber(const char*, f64) noexcept override {}
    f64 GetGlobalNumber(const char*, f64 default_value) const noexcept override {
        return default_value;
    }
    void CollectGarbage() noexcept override {}
    u64 MemoryUsageBytes() const noexcept override { return 0u; }

    bool FailLoad = false;
    bool FailCall = false;
    bool FailRegister = false;
    u32 LoadCalls = 0u;
    u32 CallCalls = 0u;
    u32 RegisterCalls = 0u;
    u32 LastSourceLength = 0u;
    u32 ReturnHandle = 0x1234u;
    char LastSource[128] = {};
    const char* LastRegisteredName = nullptr;
    NativeFunction LastRegisteredFunction = nullptr;
    void* LastRegisteredUser = nullptr;
};

void NativeA(IScriptVm&, FScriptCallFrame&, void*) noexcept {}
void NativeB(IScriptVm&, FScriptCallFrame&, void*) noexcept {}

struct FErrorCapture {
    u32 Count = 0u;
    const char* LastChunk = nullptr;
};

void CaptureError(void* user,
                  const char* chunk_name,
                  u32,
                  const char*) noexcept {
    auto* capture = static_cast<FErrorCapture*>(user);
    ++capture->Count;
    capture->LastChunk = chunk_name;
}

class FTempScriptPath {
public:
    explicit FTempScriptPath(const wchar_t* tag) noexcept {
        const DWORD pid = ::GetCurrentProcessId();
        const DWORD tid = ::GetCurrentThreadId();
        ::wsprintfW(m_Path, L"acs_script_host_%s_%lu_%lu.tmp",
                    tag,
                    static_cast<unsigned long>(pid),
                    static_cast<unsigned long>(tid));
        ::DeleteFileW(m_Path);
    }

    ~FTempScriptPath() noexcept { ::DeleteFileW(m_Path); }

    const wchar_t* Get() const noexcept { return m_Path; }

    bool Write(const void* data, u32 size) noexcept {
        HANDLE file = ::CreateFileW(m_Path,
                                    GENERIC_WRITE,
                                    0u,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0u;
        const BOOL ok = size == 0u ||
            (::WriteFile(file, data, size, &written, nullptr) && written == size);
        const BOOL closed = ::CloseHandle(file);
        return ok && closed;
    }

private:
    wchar_t m_Path[MAX_PATH] = {};
};

} // namespace

ACS_TEST(ScriptHostSafety, SourceValidationPrecedesBackendExecution)
{
    CMockScriptVm vm;
    CScriptHost host;
    host.Init(&vm);

    const char embedded_nul[] = {'a', '\0', 'b'};
    const auto embedded = host.LoadAndRunSource(
        embedded_nul,
        static_cast<u32>(sizeof(embedded_nul)),
        "memory");
    EXPECT_TRUE(embedded.IsErr());
    if (embedded.IsErr()) {
        EXPECT_EQ(embedded.Error().subcode, script_err::kSub_EmbeddedNul);
    }
    EXPECT_EQ(vm.LoadCalls, 0u);

    const char one_byte = 'x';
    const auto oversized = host.LoadAndRunSource(
        &one_byte,
        static_cast<u32>(kMaxScriptFileBytes + 1u),
        "memory");
    EXPECT_TRUE(oversized.IsErr());
    if (oversized.IsErr()) {
        EXPECT_EQ(oversized.Error().subcode, script_err::kSub_FileTooLarge);
    }
    EXPECT_EQ(vm.LoadCalls, 0u);

    const auto valid = host.LoadAndRunSource("return 7", 8u, "memory");
    EXPECT_TRUE(valid.IsOk());
    EXPECT_EQ(vm.LoadCalls, 1u);
    EXPECT_EQ(vm.LastSourceLength, 8u);
}

ACS_TEST(ScriptHostSafety, BackendFailureFiresCallback)
{
    CMockScriptVm vm;
    vm.FailLoad = true;
    CScriptHost host;
    FErrorCapture capture;
    host.Init(&vm);
    host.SetOnErrorCallback(&CaptureError, &capture);

    const auto result = host.LoadAndRunSource("bad", 3u, "chunk");
    EXPECT_TRUE(result.IsErr());
    EXPECT_EQ(capture.Count, 1u);
    EXPECT_TRUE(capture.LastChunk != nullptr);
}

ACS_TEST(ScriptHostSafety, FailedCallPreservesCallerReturnValue)
{
    CMockScriptVm vm;
    CScriptHost host;
    host.Init(&vm);

    FScriptValue output{};
    output.kind = EScriptValueKind::Handle;
    output.v.handle = 77u;
    vm.FailCall = true;
    const auto failed =
        host.CallGlobalFunction("Tick", nullptr, 0u, &output);
    EXPECT_TRUE(failed.IsErr());
    EXPECT_TRUE(output.kind == EScriptValueKind::Handle);
    EXPECT_EQ(output.v.handle, 77u);

    vm.FailCall = false;
    const auto succeeded =
        host.CallGlobalFunction("Tick", nullptr, 0u, &output);
    EXPECT_TRUE(succeeded.IsOk());
    EXPECT_TRUE(output.kind == EScriptValueKind::Handle);
    EXPECT_EQ(output.v.handle, vm.ReturnHandle);
}

ACS_TEST(ScriptHostSafety, CallRejectsInvalidArgumentsBeforeBackend)
{
    CMockScriptVm vm;
    CScriptHost host;
    host.Init(&vm);

    FScriptValue one{};
    const auto too_many = host.CallGlobalFunction(
        "Tick",
        &one,
        kMaxScriptCallArguments + 1u,
        nullptr);
    EXPECT_TRUE(too_many.IsErr());
    if (too_many.IsErr()) {
        EXPECT_EQ(too_many.Error().subcode, script_err::kSub_ArgumentLimit);
    }

    one.kind = EScriptValueKind::String;
    one.v.str = nullptr;
    const auto null_string =
        host.CallGlobalFunction("Tick", &one, 1u, nullptr);
    EXPECT_TRUE(null_string.IsErr());
    EXPECT_EQ(vm.CallCalls, 0u);
}

ACS_TEST(ScriptHostSafety, NativeRegistryRollsBackBackendFailures)
{
    CMockScriptVm vm;
    CScriptHost host;
    host.Init(&vm);
    int first_user = 1;
    int second_user = 2;

    EXPECT_TRUE(host.RegisterNative("Native", &NativeA, &first_user).IsOk());
    EXPECT_EQ(host.RegisteredNativeCount(), 1u);

    NativeFunction found = nullptr;
    void* found_user = nullptr;
    EXPECT_TRUE(host.TryGetRegisteredNative("Native", found, found_user));
    EXPECT_TRUE(found == &NativeA);
    EXPECT_TRUE(found_user == &first_user);

    vm.FailRegister = true;
    EXPECT_TRUE(host.RegisterNative("Native", &NativeB, &second_user).IsErr());
    EXPECT_EQ(host.RegisteredNativeCount(), 1u);
    found = nullptr;
    found_user = nullptr;
    EXPECT_TRUE(host.TryGetRegisteredNative("Native", found, found_user));
    EXPECT_TRUE(found == &NativeA);
    EXPECT_TRUE(found_user == &first_user);

    EXPECT_TRUE(host.RegisterNative("Rejected", &NativeB, &second_user).IsErr());
    EXPECT_EQ(host.RegisteredNativeCount(), 1u);
    EXPECT_FALSE(host.TryGetRegisteredNative("Rejected", found, found_user));
}

ACS_TEST(ScriptHostSafety, NativeNamesAreBounded)
{
    CMockScriptVm vm;
    CScriptHost host;
    host.Init(&vm);

    char too_long[kMaxScriptFunctionNameBytes + 2u] = {};
    for (u32 i = 0u; i < kMaxScriptFunctionNameBytes + 1u; ++i) {
        too_long[i] = 'x';
    }
    const auto result = host.RegisterNative(too_long, &NativeA, nullptr);
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(result.Error().subcode, script_err::kSub_InvalidName);
    }
    EXPECT_EQ(vm.RegisterCalls, 0u);
}

ACS_TEST(ScriptHostSafety, NativeRegistryOwnsFunctionNames)
{
    CMockScriptVm vm;
    CScriptHost host;
    host.Init(&vm);

    char transient_name[] = "Transient";
    EXPECT_TRUE(host.RegisterNative(transient_name, &NativeA, nullptr).IsOk());
    EXPECT_TRUE(vm.LastRegisteredName != transient_name);
    transient_name[0] = 'X';

    NativeFunction found = nullptr;
    void* found_user = reinterpret_cast<void*>(1);
    EXPECT_TRUE(host.TryGetRegisteredNative("Transient", found, found_user));
    EXPECT_TRUE(found == &NativeA);
    EXPECT_TRUE(found_user == nullptr);
    EXPECT_FALSE(host.TryGetRegisteredNative("Xransient", found, found_user));
}

ACS_TEST(ScriptHostSafety, FileInputIsValidatedBeforeBackend)
{
    CMockScriptVm vm;
    CScriptHost host;
    host.Init(&vm);
    FTempScriptPath path(L"input");

    const char valid[] = "print('ok')";
    EXPECT_TRUE(path.Write(valid, static_cast<u32>(sizeof(valid) - 1u)));
    EXPECT_TRUE(host.LoadAndRun(path.Get()).IsOk());
    EXPECT_EQ(vm.LoadCalls, 1u);
    EXPECT_EQ(vm.LastSourceLength, static_cast<u32>(sizeof(valid) - 1u));

    const char embedded[] = {'a', '\0', 'b'};
    EXPECT_TRUE(path.Write(embedded, static_cast<u32>(sizeof(embedded))));
    const auto rejected = host.LoadAndRun(path.Get());
    EXPECT_TRUE(rejected.IsErr());
    if (rejected.IsErr()) {
        EXPECT_EQ(rejected.Error().subcode, script_err::kSub_EmbeddedNul);
    }
    EXPECT_EQ(vm.LoadCalls, 1u);

    EXPECT_TRUE(path.Write(nullptr, 0u));
    EXPECT_TRUE(host.LoadAndRun(path.Get()).IsOk());
    EXPECT_EQ(vm.LoadCalls, 2u);
    EXPECT_EQ(vm.LastSourceLength, 0u);
}

ACS_TEST(ScriptHostSafety, FilePathsAreBoundedBeforeIo)
{
    CMockScriptVm vm;
    CScriptHost host;
    host.Init(&vm);

    const TResult<void> null_path = host.LoadAndRun(nullptr);
    EXPECT_TRUE(null_path.IsErr());
    if (null_path.IsErr()) {
        EXPECT_EQ(null_path.Error().subcode, script_err::kSub_InvalidPath);
    }

    const TResult<void> empty_path = host.LoadAndRun(L"");
    EXPECT_TRUE(empty_path.IsErr());
    if (empty_path.IsErr()) {
        EXPECT_EQ(empty_path.Error().subcode, script_err::kSub_InvalidPath);
    }

    wchar_t too_long[kMaxScriptPathChars + 2u] = {};
    for (u32 i = 0u; i <= kMaxScriptPathChars; ++i) {
        too_long[i] = L'x';
    }
    const TResult<void> rejected = host.LoadAndRun(too_long);
    EXPECT_TRUE(rejected.IsErr());
    if (rejected.IsErr()) {
        EXPECT_EQ(rejected.Error().subcode, script_err::kSub_InvalidPath);
    }
    EXPECT_EQ(vm.LoadCalls, 0u);
}
