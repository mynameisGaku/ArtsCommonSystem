// SPDX-License-Identifier: Apache-2.0
// XAudio2Backend の共有操作と別スレッド Shutdown の寿命契約を検証する。
#include "foundation/Limits.h"
#include "foundation/Move.h"
#include "gameframework/audio_backend/XAudio2Backend.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"

using namespace acs;
using namespace acs::game;

namespace {

constexpr u32 kWaitTimeoutMilliseconds = 3000u;

/** 浮動小数の pure helper 結果を比較する許容誤差。 */
constexpr f32 kFloatTolerance = 0.0001f;

struct FBackendOperationContext {
    CXAudio2Backend* Backend = nullptr;
    TAtomic<u32> Finished{0u};
    u32 ActiveVoiceCount = 0u;
};

struct FBackendShutdownContext {
    CXAudio2Backend* Backend = nullptr;
    TAtomic<u32> Finished{0u};
};

bool WaitForValue(const TAtomic<u32>& Value, u32 Expected) noexcept
{
    for (u32 Elapsed = 0u; Elapsed < kWaitTimeoutMilliseconds; ++Elapsed) {
        if (Value.Load(EMemoryOrder::Acquire) == Expected) {
            return true;
        }
        SleepMs(1u);
    }
    return false;
}

bool WaitForShutdownRequest(const CXAudio2Backend& Backend) noexcept
{
    for (u32 Elapsed = 0u; Elapsed < kWaitTimeoutMilliseconds; ++Elapsed) {
        if (Backend.IsShutdownRequestedForTesting()) {
            return true;
        }
        SleepMs(1u);
    }
    return false;
}

void ActiveVoiceCountThread(void* UserData)
{
    auto* const Context = static_cast<FBackendOperationContext*>(UserData);
    Context->ActiveVoiceCount = Context->Backend->ActiveVoiceCount();
    Context->Finished.Store(1u, EMemoryOrder::Release);
}

void SetVoiceParametersThread(void* UserData)
{
    auto* const Context = static_cast<FBackendOperationContext*>(UserData);
    Context->Backend->SetVoiceParameters(FAudioVoiceHandle::FromPackedValue(1u), 0.5f, 0.25f, 2.0f);
    Context->Finished.Store(1u, EMemoryOrder::Release);
}

void ShutdownThread(void* UserData)
{
    auto* const Context = static_cast<FBackendShutdownContext*>(UserData);
    Context->Backend->Shutdown();
    Context->Finished.Store(1u, EMemoryOrder::Release);
}

bool NearlyEqual(f32 Left, f32 Right) noexcept
{
    const f32 Difference = Left >= Right ? Left - Right : Right - Left;
    return Difference <= kFloatTolerance;
}

bool VerifyVoiceParameterPureHelpers() noexcept
{
    constexpr u32 kStereoMask = 0x00000003u;
    constexpr u32 kFivePointOneBackMask = 0x0000003Fu;
    f32 Matrix[8]{};
    if (!CXAudio2Backend::BuildMonoPanMatrixForTesting(1u, 2u, kStereoMask, 0.0f, Matrix, 8u) ||
        !NearlyEqual(Matrix[0], 0.70710678f) || !NearlyEqual(Matrix[1], 0.70710678f)) {
        return false;
    }

    if (!CXAudio2Backend::BuildMonoPanMatrixForTesting(1u, 2u, kStereoMask, -2.0f, Matrix, 8u) ||
        !NearlyEqual(Matrix[0], 1.0f) || !NearlyEqual(Matrix[1], 0.0f)) {
        return false;
    }
    if (!CXAudio2Backend::BuildMonoPanMatrixForTesting(1u, 2u, kStereoMask, 2.0f, Matrix, 8u) ||
        !NearlyEqual(Matrix[0], 0.0f) || !NearlyEqual(Matrix[1], 1.0f)) {
        return false;
    }

    for (f32& Value : Matrix) {
        Value = -1.0f;
    }
    if (!CXAudio2Backend::BuildMonoPanMatrixForTesting(1u, 6u, kFivePointOneBackMask, 0.0f, Matrix, 8u) ||
        !NearlyEqual(Matrix[0], 0.70710678f) || !NearlyEqual(Matrix[1], 0.70710678f)) {
        return false;
    }
    for (u32 Index = 2u; Index < 6u; ++Index) {
        if (!NearlyEqual(Matrix[Index], 0.0f)) {
            return false;
        }
    }

    const f32 PositiveInfinity = TNumLimits<f32>::Infinity();
    if (!CXAudio2Backend::BuildMonoPanMatrixForTesting(1u, 2u, kStereoMask, PositiveInfinity, Matrix, 8u) ||
        !NearlyEqual(Matrix[0], 0.70710678f) || !NearlyEqual(Matrix[1], 0.70710678f)) {
        return false;
    }
    if (CXAudio2Backend::BuildMonoPanMatrixForTesting(1u, 1u, kStereoMask, 0.0f, Matrix, 8u) ||
        CXAudio2Backend::BuildMonoPanMatrixForTesting(1u, 2u, 0u, 0.0f, Matrix, 8u) ||
        CXAudio2Backend::BuildMonoPanMatrixForTesting(1u, 2u, 0x0000000Cu, 0.0f, Matrix, 8u) ||
        CXAudio2Backend::BuildMonoPanMatrixForTesting(1u, 6u, kStereoMask, 0.0f, Matrix, 8u) ||
        CXAudio2Backend::BuildMonoPanMatrixForTesting(1u, 2u, kStereoMask, 0.0f, Matrix, 1u)) {
        return false;
    }
    Matrix[0] = -7.0f;
    Matrix[1] = -7.0f;
    if (CXAudio2Backend::BuildMonoPanMatrixForTesting(2u, 2u, kStereoMask, 0.0f, Matrix, 8u) || Matrix[0] != -7.0f ||
        Matrix[1] != -7.0f) {
        return false;
    }
    Matrix[0] = -9.0f;
    Matrix[1] = -9.0f;
    Matrix[2] = -9.0f;
    if (CXAudio2Backend::BuildMonoPanMatrixForTesting(1u, 3u, 0x80000003u, 0.0f, Matrix, 8u) || Matrix[0] != -9.0f ||
        Matrix[1] != -9.0f || Matrix[2] != -9.0f) {
        return false;
    }
    if (CXAudio2Backend::BuildMonoPanMatrixForTesting(1u, 3u, 0x00040003u, 0.0f, Matrix, 8u) || Matrix[0] != -9.0f ||
        Matrix[1] != -9.0f || Matrix[2] != -9.0f) {
        return false;
    }
    Matrix[0] = -11.0f;
    Matrix[1] = -11.0f;
    if (CXAudio2Backend::BuildMonoPanMatrixForTesting(1u, 2u, 0x00000006u, 0.0f, Matrix, 8u) ||
        Matrix[0] != -11.0f || Matrix[1] != -11.0f) {
        return false;
    }
    Matrix[0] = -13.0f;
    Matrix[1] = -13.0f;
    if (CXAudio2Backend::BuildMonoPanMatrixForTesting(1u, 2u, 0x00000005u, 0.0f, Matrix, 8u) ||
        Matrix[0] != -13.0f || Matrix[1] != -13.0f) {
        return false;
    }

    f32 Volume = -1.0f;
    f32 Pan = -1.0f;
    f32 Pitch = -1.0f;
    CXAudio2Backend::NormalizeVoiceParametersForTesting(PositiveInfinity, PositiveInfinity, PositiveInfinity, Volume,
                                                        Pan, Pitch);
    if (Volume != 0.0f || Pan != 0.0f || Pitch != 1.0f) {
        return false;
    }
    volatile f32 RuntimeInfinity = PositiveInfinity;
    const f32 NotANumber = RuntimeInfinity - RuntimeInfinity;
    CXAudio2Backend::NormalizeVoiceParametersForTesting(NotANumber, 0.25f, 2.0f, Volume, Pan, Pitch);
    if (Volume != 0.0f || Pan != 0.25f || Pitch != 2.0f) {
        return false;
    }
    CXAudio2Backend::NormalizeVoiceParametersForTesting(0.5f, NotANumber, 2.0f, Volume, Pan, Pitch);
    if (Volume != 0.5f || Pan != 0.0f || Pitch != 2.0f) {
        return false;
    }
    CXAudio2Backend::NormalizeVoiceParametersForTesting(0.5f, 0.25f, NotANumber, Volume, Pan, Pitch);
    if (Volume != 0.5f || Pan != 0.25f || Pitch != 1.0f) {
        return false;
    }
    CXAudio2Backend::NormalizeVoiceParametersForTesting(-1.0f, -2.0f, 0.0f, Volume, Pan, Pitch);
    if (Volume != 0.0f || Pan != -1.0f || Pitch != 0.25f) {
        return false;
    }
    CXAudio2Backend::NormalizeVoiceParametersForTesting(2.0f, 2.0f, 8.0f, Volume, Pan, Pitch);
    if (Volume != 1.0f || Pan != 1.0f || Pitch != 4.0f) {
        return false;
    }
    CXAudio2Backend::NormalizeVoiceParametersForTesting(1.0f, 0.0f, 4.0f, Volume, Pan, Pitch);
    return Volume == 1.0f && Pan == 0.0f && Pitch == 4.0f &&
           CXAudio2Backend::DestroySlotResetsSourceChannelsForTesting();
}

} // namespace

int main()
{
    if (!VerifyVoiceParameterPureHelpers()) {
        return 12;
    }

    CXAudio2Backend Backend;
    if (Backend.InitializeLifecycleTestState().IsErr()) {
        return 1;
    }

    TAtomic<u32> GateEntered{0u};
    TAtomic<u32> GateRelease{0u};
    CXAudio2Backend::ConfigureLifecycleOperationTestGate(&GateEntered, &GateRelease);

    i32 ExitCode = 0;
    FBackendOperationContext FirstOperation;
    FirstOperation.Backend = &Backend;
    FThread FirstThread;
    TResult<FThread> FirstResult = FThread::Spawn(&SetVoiceParametersThread, &FirstOperation);
    if (FirstResult.IsErr()) {
        ExitCode = 2;
    } else {
        FirstThread = Move(FirstResult.Value());
    }

    if (ExitCode == 0 && !WaitForValue(GateEntered, 1u)) {
        ExitCode = 3;
    }

    FBackendShutdownContext ShutdownContext;
    ShutdownContext.Backend = &Backend;
    FThread ShutdownWorker;
    if (ExitCode == 0) {
        TResult<FThread> ShutdownResult = FThread::Spawn(&ShutdownThread, &ShutdownContext);
        if (ShutdownResult.IsErr()) {
            ExitCode = 4;
        } else {
            ShutdownWorker = Move(ShutdownResult.Value());
        }
    }

    if (ExitCode == 0 && !WaitForShutdownRequest(Backend)) {
        ExitCode = 5;
    }

    FBackendOperationContext SecondOperation;
    SecondOperation.Backend = &Backend;
    FThread SecondThread;
    if (ExitCode == 0) {
        TResult<FThread> SecondResult = FThread::Spawn(&ActiveVoiceCountThread, &SecondOperation);
        if (SecondResult.IsErr()) {
            ExitCode = 6;
        } else {
            SecondThread = Move(SecondResult.Value());
        }
    }

    if (ExitCode == 0 && !WaitForValue(SecondOperation.Finished, 1u)) {
        ExitCode = 7;
    }
    if (ExitCode == 0 && ShutdownContext.Finished.Load(EMemoryOrder::Acquire) != 0u) {
        ExitCode = 8;
    }
    if (ExitCode == 0 && FirstOperation.Finished.Load(EMemoryOrder::Acquire) != 0u) {
        ExitCode = 9;
    }

    GateRelease.Store(1u, EMemoryOrder::Release);
    if (FirstThread.Joinable()) {
        FirstThread.Join();
    }
    if (SecondThread.Joinable()) {
        SecondThread.Join();
    }
    if (ShutdownWorker.Joinable()) {
        ShutdownWorker.Join();
    } else {
        Backend.Shutdown();
    }
    CXAudio2Backend::ConfigureLifecycleOperationTestGate(nullptr, nullptr);

    if (ExitCode == 0 && ShutdownContext.Finished.Load(EMemoryOrder::Acquire) == 0u) {
        ExitCode = 10;
    }
    if (ExitCode == 0 && Backend.HasLifecycleStateForTesting()) {
        ExitCode = 11;
    }
    return ExitCode;
}
