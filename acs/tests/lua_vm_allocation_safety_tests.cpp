// SPDX-License-Identifier: Apache-2.0
#include "scripting/LuaVmImpl.h"

#include "foundation/Error.h"
#include "memory/SystemAllocator.h"
#include "test/Expect.h"
#include "test/Test.h"

using namespace acs;
using namespace acs::game;
using namespace acs::scripting;

static_assert(IsSameV<FLuaVm, CLuaVm>, "旧Lua実行環境名は正規型の互換別名である必要があります");
#if defined(_WIN64)
static_assert(sizeof(CLuaVm) == 16u && alignof(CLuaVm) == 8u, "Win64のLua実行環境配置が変わりました");
#endif

namespace {

/** 確保拒否と回復を切り替え、Lua 登録簿の失敗経路を再現する。 */
class FRejectableAllocator final : public IAllocator {
public:
    /**
     * 指定領域を確保し、拒否中は nullptr を返す。
     * @param size 確保するバイト数。
     * @param alignment 要求するメモリ境界幅。
     * @param location 確保元を示す位置。
     * @return 確保した領域。拒否中または確保失敗時は nullptr。
     */
    void* Alloc(usize size, usize alignment, FSourceLoc location) noexcept override {
        ++m_AllocationAttemptCount;
        if (m_RejectAllocations) {
            return nullptr;
        }

        /** 下位アロケータが返した領域。 */
        void* const pointer = m_Backing.Alloc(size, alignment, location);
        if (pointer != nullptr) {
            ++m_OutstandingAllocationCount;
        }
        return pointer;
    }

    /**
     * 確保済み領域を下位アロケータへ返す。
     * @param pointer 解放する領域。
     */
    void Free(void* pointer) noexcept override {
        if (pointer == nullptr) {
            return;
        }
        if (m_OutstandingAllocationCount > 0u) {
            --m_OutstandingAllocationCount;
        }
        m_Backing.Free(pointer);
    }

    /** 以降の確保要求を拒否するか設定する。 */
    void SetRejectAllocations(bool reject) noexcept {
        m_RejectAllocations = reject;
    }

    /** 受け取った確保要求数を返す。 */
    u32 AllocationAttemptCount() const noexcept {
        return m_AllocationAttemptCount;
    }

    /** 下位アロケータへ返していない確保数を返す。 */
    u32 OutstandingAllocationCount() const noexcept {
        return m_OutstandingAllocationCount;
    }

private:
    /** 実際の領域確保と解放を行う下位アロケータ。 */
    CSystemAllocator m_Backing;

    /** 確保要求を失敗させる場合は true。 */
    bool m_RejectAllocations = false;

    /** 受け取った確保要求の累計。 */
    u32 m_AllocationAttemptCount = 0u;

    /** 下位アロケータへ返していない確保数。 */
    u32 m_OutstandingAllocationCount = 0u;
};

/**
 * Lua から呼ばれた回数を利用者データへ記録する。
 * @param vm 呼び出し元の Lua 実行環境。
 * @param frame Lua と C++ の値受け渡し情報。
 * @param user_data 呼び出し回数の保存先。
 */
void CountNativeCall(IScriptVm& vm, FScriptCallFrame& frame, void* user_data) noexcept {
    (void)vm;
    (void)frame;
    /** 呼び出し回数の保存先。 */
    auto* const call_count = static_cast<u32*>(user_data);
    if (call_count != nullptr) {
        ++(*call_count);
    }
}

/** allocator callbackから同じLua実行環境への登録再入を一度だけ試す。 */
class FReentrantAllocator final : public IAllocator {
public:
    /**
     * 指定領域を確保し、予約済みなら確保中に登録を再入する。
     * @param size 確保するバイト数。
     * @param alignment 要求するメモリ境界幅。
     * @param location 確保元を示す位置。
     * @return 確保した領域。確保失敗時はnullptr。
     */
    void* Alloc(usize size, usize alignment, FSourceLoc location) noexcept override {
        if (m_RegistrationArmed) {
            m_RegistrationArmed = false;
            m_RegistrationAttempted = true;
            /** allocator callbackから同じ実行環境へ再入した登録結果。 */
            const TResult<void> reentered = m_Vm->RegisterNativeFunction("AllocatorReenteredNative", &CountNativeCall, m_ReenteredCallCount);
            m_RegistrationRejected = reentered.IsErr() && reentered.Error().category == EErrCategory::Generic && reentered.Error().subcode == script_err::kSub_CallFailed;
        }

        /** 下位アロケータが返した領域。 */
        void* const pointer = m_Backing.Alloc(size, alignment, location);
        if (pointer != nullptr) {
            ++m_OutstandingAllocationCount;
        }
        return pointer;
    }

    /**
     * 確保済み領域を下位アロケータへ返す。
     * @param pointer 解放する領域。
     */
    void Free(void* pointer) noexcept override {
        if (pointer == nullptr) {
            return;
        }
        if (m_OutstandingAllocationCount > 0u) {
            --m_OutstandingAllocationCount;
        }
        m_Backing.Free(pointer);
    }

    /**
     * 次の確保中に同じ実行環境への登録再入を試す。
     * @param vm 再入先のLua実行環境。
     * @param reentered_call_count 誤登録された関数の呼び出し回数。
     */
    void ArmRegistration(IScriptVm& vm, u32& reentered_call_count) noexcept {
        m_Vm = &vm;
        m_ReenteredCallCount = &reentered_call_count;
        m_RegistrationArmed = true;
    }

    /** allocator callbackから登録を試したか返す。 */
    bool RegistrationAttempted() const noexcept {
        return m_RegistrationAttempted;
    }

    /** allocator callbackからの登録が規定どおり拒否されたか返す。 */
    bool RegistrationRejected() const noexcept {
        return m_RegistrationRejected;
    }

    /** 下位アロケータへ返していない確保数を返す。 */
    u32 OutstandingAllocationCount() const noexcept {
        return m_OutstandingAllocationCount;
    }

private:
    /** 実際の領域確保と解放を行う下位アロケータ。 */
    CSystemAllocator m_Backing;

    /** allocator callbackから登録を再入するLua実行環境。 */
    IScriptVm* m_Vm = nullptr;

    /** 誤登録された関数の呼び出し回数。 */
    u32* m_ReenteredCallCount = nullptr;

    /** 次の確保中に登録を再入する場合はtrue。 */
    bool m_RegistrationArmed = false;

    /** allocator callbackが登録を呼び出したか示す。 */
    bool m_RegistrationAttempted = false;

    /** 再入登録が規定の分類で拒否されたか示す。 */
    bool m_RegistrationRejected = false;

    /** 下位アロケータへ返していない確保数。 */
    u32 m_OutstandingAllocationCount = 0u;
};

/** Luaメタ関数経由の再入拒否と各closureの呼び出し回数を記録する。 */
struct FReentrantRegistrationState {
    /** 既存native関数が呼ばれた回数。 */
    u32 existing_call_count = 0u;

    /** 再入先として要求したnative関数が呼ばれた回数。 */
    u32 reentered_call_count = 0u;

    /** 外側失敗後に登録したnative関数が呼ばれた回数。 */
    u32 subsequent_call_count = 0u;

    /** 既存native callbackが登録を一度呼び出したか示す。 */
    bool registration_attempted = false;

    /** 再入登録が規定の分類で拒否されたか示す。 */
    bool registration_rejected = false;
};

/**
 * 既存native関数の呼び出しを記録し、初回だけ同じ実行環境へ登録を再入する。
 * @param vm 呼び出し元のLua実行環境。
 * @param frame LuaとC++の値受け渡し情報。
 * @param user_data 再入結果と呼び出し回数の保存先。
 */
void AttemptReentrantRegistration(IScriptVm& vm, FScriptCallFrame& frame, void* user_data) noexcept {
    (void)frame;
    /** 再入結果と呼び出し回数の保存先。 */
    auto* const state = static_cast<FReentrantRegistrationState*>(user_data);
    if (state == nullptr) {
        return;
    }

    ++state->existing_call_count;
    if (state->registration_attempted) {
        return;
    }

    state->registration_attempted = true;
    /** Lua公開処理中に同じ実行環境へ再入した登録結果。 */
    const TResult<void> reentered = vm.RegisterNativeFunction("ReenteredNative", &CountNativeCall, &state->reentered_call_count);
    state->registration_rejected = reentered.IsErr() && reentered.Error().category == EErrCategory::Generic && reentered.Error().subcode == script_err::kSub_CallFailed;
}

} // namespace

/** 登録簿の確保失敗が Lua 公開前に戻り、同じ VM で再試行できることを確認する。 */
ACS_TEST(LuaVmAllocationSafety, NativeRegistrationReportsAllocationFailureAndRecovers) {
    /** 登録簿の確保失敗と回復を制御するアロケータ。 */
    FRejectableAllocator allocator;
    /** Lua から C++ 関数が呼ばれた回数。 */
    u32 call_count = 0u;

    {
        /** 検証対象の Lua 実行環境。 */
        CLuaVm vm(allocator);
        EXPECT_TRUE(vm.Init().IsOk());

        /** 確保拒否後も残す既存の同名 Lua 関数。 */
        constexpr char kExistingNativeScript[] = "ExistingNativeCallCount = 0; Native = function() ExistingNativeCallCount = ExistingNativeCallCount + 1 end";
        EXPECT_TRUE(vm.LoadScript(kExistingNativeScript, static_cast<u32>(sizeof(kExistingNativeScript) - 1u), "existing-native").IsOk());

        allocator.SetRejectAllocations(true);
        /** 登録簿の初回確保を拒否した結果。 */
        const TResult<void> rejected = vm.RegisterNativeFunction("Native", &CountNativeCall, &call_count);
        EXPECT_TRUE(rejected.IsErr());
        if (rejected.IsErr()) {
            EXPECT_EQ(rejected.Error().category, EErrCategory::Memory);
            EXPECT_EQ(rejected.Error().subcode, script_err::kSub_AllocationFailed);
        }
        EXPECT_EQ(allocator.AllocationAttemptCount(), 1u);
        EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);

        /** 確保失敗後に既存の同名 Lua 関数を呼ぶコード。 */
        constexpr char kExistingCallScript[] = "Native()";
        EXPECT_TRUE(vm.LoadScript(kExistingCallScript, static_cast<u32>(sizeof(kExistingCallScript) - 1u), "existing-native-call").IsOk());
        EXPECT_EQ(vm.GetGlobalNumber("ExistingNativeCallCount", -1.0), 1.0);

        allocator.SetRejectAllocations(false);
        EXPECT_TRUE(vm.RegisterNativeFunction("Native", &CountNativeCall, &call_count).IsOk());
        EXPECT_EQ(allocator.AllocationAttemptCount(), 2u);
        EXPECT_EQ(allocator.OutstandingAllocationCount(), 1u);

        /** 回復後の登録関数を呼ぶ Lua コード。 */
        constexpr char kRecoveredCallScript[] = "Native()";
        EXPECT_TRUE(vm.LoadScript(kRecoveredCallScript, static_cast<u32>(sizeof(kRecoveredCallScript) - 1u), "allocation-recovery").IsOk());
        EXPECT_EQ(call_count, 1u);

        vm.Shutdown();
        EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
    }

    EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
}

/** Lua 公開失敗が登録簿末尾と同名グローバルを戻すことを確認する。 */
ACS_TEST(LuaVmAllocationSafety, NativeRegistrationRejectsLuaGlobalSpoofAndRollsBackPublicationFailure) {
    /** 通常登録で登録簿容量が成長する位置。 */
    u32 reference_growth_registration = 0u;
    {
        /** 通常登録時の容量成長位置を測るアロケータ。 */
        FRejectableAllocator reference_allocator;
        /** 通常登録を行う Lua 実行環境。 */
        CLuaVm reference_vm(reference_allocator);
        /** 基準関数から参照する呼び出し回数。 */
        u32 reference_call_count = 0u;
        EXPECT_TRUE(reference_vm.Init().IsOk());

        /** 初回容量を超えるまでに試す登録数。 */
        constexpr u32 kMaximumReferenceRegistrations = 64u;
        for (u32 registration_number = 1u; registration_number <= kMaximumReferenceRegistrations; ++registration_number) {
            EXPECT_TRUE(reference_vm.RegisterNativeFunction("ReferenceNative", &CountNativeCall, &reference_call_count).IsOk());
            if (reference_allocator.AllocationAttemptCount() > 1u) {
                reference_growth_registration = registration_number;
                break;
            }
        }
        EXPECT_TRUE(reference_growth_registration > 0u);
        reference_vm.Shutdown();
        EXPECT_EQ(reference_allocator.OutstandingAllocationCount(), 0u);
    }

    /** Lua 公開失敗で残る容量と論理要素数を検証するアロケータ。 */
    FRejectableAllocator allocator;
    /** Lua から C++ 関数が呼ばれた回数。 */
    u32 call_count = 0u;
    {
        /** Lua 公開失敗と回復を検証する実行環境。 */
        CLuaVm vm(allocator);
        EXPECT_TRUE(vm.Init().IsOk());

        /** 書き込み後に error を返し、同名グローバルの復元を要求する設定。 */
        constexpr char kRejectGlobalScript[] = "setmetatable(_G, {__newindex = function(target, name, value) rawset(target, name, 'mutated'); error('registration blocked') end})";
        EXPECT_TRUE(vm.LoadScript(kRejectGlobalScript, static_cast<u32>(sizeof(kRejectGlobalScript) - 1u), "reject-global").IsOk());

        /** Lua 側が公開を拒否した登録結果。 */
        const TResult<void> rejected = vm.RegisterNativeFunction("BlockedNative", &CountNativeCall, &call_count);
        EXPECT_TRUE(rejected.IsErr());
        if (rejected.IsErr()) {
            EXPECT_EQ(rejected.Error().category, EErrCategory::Generic);
            EXPECT_EQ(rejected.Error().subcode, script_err::kSub_CallFailed);
        }
        EXPECT_EQ(allocator.AllocationAttemptCount(), 1u);
        EXPECT_EQ(allocator.OutstandingAllocationCount(), 1u);

        /** 書き込み拒否を解除し、失敗した同名グローバルが残らないことを確認する。 */
        constexpr char kRestoreGlobalScript[] = "setmetatable(_G, nil); assert(BlockedNative == nil)";
        EXPECT_TRUE(vm.LoadScript(kRestoreGlobalScript, static_cast<u32>(sizeof(kRestoreGlobalScript) - 1u), "restore-global").IsOk());

        /** Lua 側が大域変数への代入を無視して正常終了する状態を作る。 */
        constexpr char kSwallowGlobalScript[] = "setmetatable(_G, {__newindex = function(target, name, value) end})";
        EXPECT_TRUE(vm.LoadScript(kSwallowGlobalScript, static_cast<u32>(sizeof(kSwallowGlobalScript) - 1u), "swallow-global").IsOk());

        /** 代入を無視された登録結果。 */
        const TResult<void> swallowed = vm.RegisterNativeFunction("SwallowedNative", &CountNativeCall, &call_count);
        EXPECT_TRUE(swallowed.IsErr());
        if (swallowed.IsErr()) {
            EXPECT_EQ(swallowed.Error().category, EErrCategory::Generic);
            EXPECT_EQ(swallowed.Error().subcode, script_err::kSub_CallFailed);
        }
        EXPECT_EQ(allocator.AllocationAttemptCount(), 1u);
        EXPECT_EQ(allocator.OutstandingAllocationCount(), 1u);

        /** 書き込み無視を解除し、未公開のグローバルが残らないことを確認する。 */
        constexpr char kRestoreSwallowedGlobalScript[] = "setmetatable(_G, nil); assert(SwallowedNative == nil)";
        EXPECT_TRUE(vm.LoadScript(kRestoreSwallowedGlobalScript, static_cast<u32>(sizeof(kRestoreSwallowedGlobalScript) - 1u), "restore-swallowed-global").IsOk());

        /** 書き込みを退避し、読み取り時だけ同じLua関数を返して公開成功を偽装する設定。 */
        constexpr char kSpoofGlobalScript[] =
            "SpoofIndexReadCount = 0; setmetatable(_G, {"
            "__newindex = function(target, name, value) "
            "if name == 'SpoofedNative' then rawset(target, 'CapturedSpoofedNative', value) return end "
            "rawset(target, name, value) end, "
            "__index = function(target, name) "
            "SpoofIndexReadCount = SpoofIndexReadCount + 1; "
            "if name == 'SpoofedNative' then return rawget(target, 'CapturedSpoofedNative') end end})";
        EXPECT_TRUE(vm.LoadScript(kSpoofGlobalScript, static_cast<u32>(sizeof(kSpoofGlobalScript) - 1u), "spoof-global").IsOk());

        /** 読み取りを偽装しても実global表へ未公開なので失敗する登録結果。 */
        const TResult<void> spoofed = vm.RegisterNativeFunction("SpoofedNative", &CountNativeCall, &call_count);
        EXPECT_TRUE(spoofed.IsErr());
        if (spoofed.IsErr()) {
            EXPECT_EQ(spoofed.Error().category, EErrCategory::Generic);
            EXPECT_EQ(spoofed.Error().subcode, script_err::kSub_CallFailed);
        }
        EXPECT_EQ(vm.GetGlobalNumber("SpoofIndexReadCount", -1.0), 0.0);

        /** 読み取り偽装を外し、登録名が未公開で退避されたLua関数だけが残ることを確認するコード。 */
        constexpr char kRestoreSpoofedGlobalScript[] =
            "setmetatable(_G, nil); assert(SpoofedNative == nil); assert(type(CapturedSpoofedNative) == 'function')";
        EXPECT_TRUE(vm.LoadScript(kRestoreSpoofedGlobalScript, static_cast<u32>(sizeof(kRestoreSpoofedGlobalScript) - 1u), "restore-spoofed-global").IsOk());

        /** rollback 後に再び容量が成長した登録位置。 */
        u32 recovered_growth_registration = 0u;
        for (u32 registration_number = 1u; registration_number <= reference_growth_registration; ++registration_number) {
            EXPECT_TRUE(vm.RegisterNativeFunction("RecoveredNative", &CountNativeCall, &call_count).IsOk());
            if (recovered_growth_registration == 0u && allocator.AllocationAttemptCount() > 1u) {
                recovered_growth_registration = registration_number;
            }
        }
        EXPECT_EQ(recovered_growth_registration, reference_growth_registration);

        /** 復元した登録簿末尾の native 関数を呼ぶ Lua コード。 */
        constexpr char kRecoveredCallScript[] = "RecoveredNative(); CapturedSpoofedNative()";
        EXPECT_TRUE(vm.LoadScript(kRecoveredCallScript, static_cast<u32>(sizeof(kRecoveredCallScript) - 1u), "recovered-call").IsOk());
        EXPECT_EQ(call_count, 1u);

        vm.Shutdown();
        EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
    }

    EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
}

/** allocatorとLua公開処理からの登録再入を拒否し、登録簿の整合性を保つことを確認する。 */
ACS_TEST(LuaVmAllocationSafety, NativeRegistrationRejectsSameVmReentryWithoutRegistryCorruption) {
    {
        /** allocator callbackからの再入を発生させる確保元。 */
        FReentrantAllocator allocator;
        /** 外側登録のclosureが呼ばれた回数。 */
        u32 outer_call_count = 0u;
        /** allocator再入が誤って公開された場合に増える検出用回数。 */
        u32 reentered_call_count = 0u;
        /** allocator確保開始前から再入を拒否するか検証するLua実行環境。 */
        CLuaVm vm(allocator);
        EXPECT_TRUE(vm.Init().IsOk());

        allocator.ArmRegistration(vm, reentered_call_count);
        EXPECT_TRUE(vm.RegisterNativeFunction("AllocatorOuterNative", &CountNativeCall, &outer_call_count).IsOk());
        EXPECT_TRUE(allocator.RegistrationAttempted());
        EXPECT_TRUE(allocator.RegistrationRejected());

        /** 再入名が未公開で外側登録だけが利用できることを確認するコード。 */
        constexpr char kVerifyAllocatorReentryScript[] = "assert(AllocatorReenteredNative == nil); AllocatorOuterNative()";
        EXPECT_TRUE(vm.LoadScript(kVerifyAllocatorReentryScript, static_cast<u32>(sizeof(kVerifyAllocatorReentryScript) - 1u), "verify-allocator-reentry").IsOk());
        EXPECT_EQ(outer_call_count, 1u);
        EXPECT_EQ(reentered_call_count, 0u);

        vm.Shutdown();
        EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
    }

    /** 再入結果と各closureの呼び出し回数。 */
    FReentrantRegistrationState state;
    /** 再入拒否後の登録簿とclosure indexを検証するLua実行環境。 */
    CLuaVm vm;
    EXPECT_TRUE(vm.Init().IsOk());
    EXPECT_TRUE(vm.RegisterNativeFunction("ExistingNative", &AttemptReentrantRegistration, &state).IsOk());

    /** 外側登録中に既存native関数を呼び、再入後に外側公開も失敗させる設定。 */
    constexpr char kReentrantGlobalScript[] = "EscapedNativeClosures = {}; setmetatable(_G, {__newindex = function(target, name, value) if name == 'OuterNative' then EscapedNativeClosures[1] = value; ExistingNative(); error('outer registration blocked') end rawset(target, name, value) end})";
    EXPECT_TRUE(vm.LoadScript(kReentrantGlobalScript, static_cast<u32>(sizeof(kReentrantGlobalScript) - 1u), "reentrant-global").IsOk());

    /** Lua公開処理から同じ実行環境へ再入した外側登録結果。 */
    const TResult<void> outer_rejected = vm.RegisterNativeFunction("OuterNative", &CountNativeCall, &state.subsequent_call_count);
    EXPECT_TRUE(outer_rejected.IsErr());
    if (outer_rejected.IsErr()) {
        EXPECT_EQ(outer_rejected.Error().category, EErrCategory::Generic);
        EXPECT_EQ(outer_rejected.Error().subcode, script_err::kSub_CallFailed);
    }
    EXPECT_TRUE(state.registration_attempted);
    EXPECT_TRUE(state.registration_rejected);
    EXPECT_EQ(state.existing_call_count, 1u);
    EXPECT_EQ(state.reentered_call_count, 0u);
    EXPECT_EQ(state.subsequent_call_count, 0u);

    /** メタ関数を外し、失敗した外側名と再入名が未公開であることを確認するコード。 */
    constexpr char kVerifyRollbackScript[] = "setmetatable(_G, nil); assert(OuterNative == nil); assert(ReenteredNative == nil); ExistingNative()";
    EXPECT_TRUE(vm.LoadScript(kVerifyRollbackScript, static_cast<u32>(sizeof(kVerifyRollbackScript) - 1u), "verify-reentrant-rollback").IsOk());
    EXPECT_EQ(state.existing_call_count, 2u);
    EXPECT_EQ(state.reentered_call_count, 0u);

    EXPECT_TRUE(vm.RegisterNativeFunction("SubsequentNative", &CountNativeCall, &state.subsequent_call_count).IsOk());
    /** 既存と後続のclosureが正しい要素を参照し、失敗時に退避されたclosureが後続要素を参照しないことを確認するコード。 */
    constexpr char kVerifyClosureIndicesScript[] = "assert(ReenteredNative == nil); ExistingNative(); SubsequentNative(); EscapedNativeClosures[1]()";
    EXPECT_TRUE(vm.LoadScript(kVerifyClosureIndicesScript, static_cast<u32>(sizeof(kVerifyClosureIndicesScript) - 1u), "verify-closure-indices").IsOk());
    EXPECT_EQ(state.existing_call_count, 3u);
    EXPECT_EQ(state.reentered_call_count, 0u);
    EXPECT_EQ(state.subsequent_call_count, 1u);

    vm.Shutdown();
}

/** 最大登録番号を公開した後に、Lua状態へ触れず恒久拒否することを確認する。 */
ACS_TEST(LuaVmAllocationSafety, NativeRegistrationMaximumIdentifierSucceedsBeforePermanentExhaustion) {
    /** 最大番号の登録後に確保要求が増えないことを確認する確保元。 */
    FRejectableAllocator allocator;
    /** 最大番号のLua関数が呼び出した回数。 */
    u32 maximum_call_count = 0u;
    /** 枯渇後の登録が誤って呼び出した場合に増える検出用回数。 */
    u32 rejected_call_count = 0u;
    /** 最大番号の成功と後続拒否を検証するLua実行環境。 */
    CLuaVm vm(allocator);
    EXPECT_TRUE(vm.Init().IsOk());

    /** 未登録名へのLua書き込み回数を数える設定。 */
    constexpr char kTrackGlobalWritesScript[] =
        "MaximumSuccessWriteCount = 0; "
        "setmetatable(_G, {__newindex = function(target, name, value) "
        "MaximumSuccessWriteCount = MaximumSuccessWriteCount + 1; rawset(target, name, value) end})";
    EXPECT_TRUE(vm.LoadScript(kTrackGlobalWritesScript, static_cast<u32>(sizeof(kTrackGlobalWritesScript) - 1u), "track-maximum-success-writes").IsOk());

    vm.SetNextNativeRegistrationIdToMaximumForTest();
    EXPECT_TRUE(vm.RegisterNativeFunction("MaximumSuccessfulNative", &CountNativeCall, &maximum_call_count).IsOk());
    /** 最大番号登録後までに登録簿が要求した確保回数。 */
    const u32 allocation_attempts_before_rejection = allocator.AllocationAttemptCount();
    /** 最大番号登録後に登録簿が保持している確保数。 */
    const u32 outstanding_allocations_before_rejection = allocator.OutstandingAllocationCount();
    EXPECT_EQ(vm.GetGlobalNumber("MaximumSuccessWriteCount", -1.0), 1.0);

    /** 枯渇後も無効な関数名を引数不正として返す登録結果。 */
    const TResult<void> null_name_rejected = vm.RegisterNativeFunction(nullptr, &CountNativeCall, &rejected_call_count);
    EXPECT_TRUE(null_name_rejected.IsErr());
    if (null_name_rejected.IsErr()) {
        EXPECT_EQ(null_name_rejected.Error().category, EErrCategory::Generic);
        EXPECT_EQ(null_name_rejected.Error().subcode, script_err::kSub_InvalidArg);
    }

    /** 枯渇後も空の関数名を引数不正として返す登録結果。 */
    const TResult<void> empty_name_rejected = vm.RegisterNativeFunction("", &CountNativeCall, &rejected_call_count);
    EXPECT_TRUE(empty_name_rejected.IsErr());
    if (empty_name_rejected.IsErr()) {
        EXPECT_EQ(empty_name_rejected.Error().category, EErrCategory::Generic);
        EXPECT_EQ(empty_name_rejected.Error().subcode, script_err::kSub_InvalidArg);
    }

    /** 枯渇後も空の関数を引数不正として返す登録結果。 */
    const TResult<void> null_function_rejected = vm.RegisterNativeFunction("InvalidFunctionAfterMaximumSuccess", nullptr, &rejected_call_count);
    EXPECT_TRUE(null_function_rejected.IsErr());
    if (null_function_rejected.IsErr()) {
        EXPECT_EQ(null_function_rejected.Error().category, EErrCategory::Generic);
        EXPECT_EQ(null_function_rejected.Error().subcode, script_err::kSub_InvalidArg);
    }

    /** 最大番号の次に拒否された登録結果。 */
    const TResult<void> rejected = vm.RegisterNativeFunction("RejectedAfterMaximumSuccess", &CountNativeCall, &rejected_call_count);
    EXPECT_TRUE(rejected.IsErr());
    if (rejected.IsErr()) {
        EXPECT_EQ(rejected.Error().category, EErrCategory::Generic);
        EXPECT_EQ(rejected.Error().subcode, script_err::kSub_CallFailed);
    }

    /** 枯渇状態が解除されず二回目も拒否された登録結果。 */
    const TResult<void> rejected_again = vm.RegisterNativeFunction("RejectedAgainAfterMaximumSuccess", &CountNativeCall, &rejected_call_count);
    EXPECT_TRUE(rejected_again.IsErr());
    if (rejected_again.IsErr()) {
        EXPECT_EQ(rejected_again.Error().category, EErrCategory::Generic);
        EXPECT_EQ(rejected_again.Error().subcode, script_err::kSub_CallFailed);
    }
    EXPECT_EQ(allocator.AllocationAttemptCount(), allocation_attempts_before_rejection);
    EXPECT_EQ(allocator.OutstandingAllocationCount(), outstanding_allocations_before_rejection);
    EXPECT_EQ(vm.GetGlobalNumber("MaximumSuccessWriteCount", -1.0), 1.0);

    /** 最大番号のLua関数だけが公開済みで呼び出せることを確認するコード。 */
    constexpr char kVerifyMaximumSuccessScript[] =
        "setmetatable(_G, nil); "
        "assert(rawget(_G, 'InvalidFunctionAfterMaximumSuccess') == nil); "
        "assert(rawget(_G, 'RejectedAfterMaximumSuccess') == nil); "
        "assert(rawget(_G, 'RejectedAgainAfterMaximumSuccess') == nil); "
        "MaximumSuccessfulNative()";
    EXPECT_TRUE(vm.LoadScript(kVerifyMaximumSuccessScript, static_cast<u32>(sizeof(kVerifyMaximumSuccessScript) - 1u), "verify-maximum-success").IsOk());
    EXPECT_EQ(maximum_call_count, 1u);
    EXPECT_EQ(rejected_call_count, 0u);

    vm.Shutdown();
    EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
}

/** 最大番号の公開失敗で退避されたLua関数が後続登録へ再接続しないことを確認する。 */
ACS_TEST(LuaVmAllocationSafety, NativeRegistrationMaximumIdentifierRollbackCannotReconnect) {
    /** 登録簿への確保要求が枯渇後に増えないことを確認する確保元。 */
    FRejectableAllocator allocator;
    /** 公開失敗時に退避された最大番号のLua関数が誤って呼び出した回数。 */
    u32 escaped_maximum_call_count = 0u;
    /** 枯渇後の登録が誤って公開された場合に増える検出用回数。 */
    u32 rejected_call_count = 0u;
    /** 最大番号の公開失敗から枯渇状態へ進めるLua実行環境。 */
    CLuaVm vm(allocator);
    EXPECT_TRUE(vm.Init().IsOk());

    /** 最大番号のLua関数を退避し、読み取りだけを偽装するLua設定。 */
    constexpr char kCaptureMaximumFunctionScript[] =
        "ExhaustionWriteCount = 0; "
        "setmetatable(_G, {"
        "__newindex = function(target, name, value) "
        "ExhaustionWriteCount = ExhaustionWriteCount + 1; "
        "if name == 'MaximumNative' then rawset(target, 'EscapedMaximumNative', value); return end "
        "rawset(target, name, value) end, "
        "__index = function(target, name) "
        "if name == 'MaximumNative' then return rawget(target, 'EscapedMaximumNative') end end})";
    EXPECT_TRUE(vm.LoadScript(kCaptureMaximumFunctionScript, static_cast<u32>(sizeof(kCaptureMaximumFunctionScript) - 1u), "capture-maximum-function").IsOk());

    vm.SetNextNativeRegistrationIdToMaximumForTest();
    allocator.SetRejectAllocations(true);
    /** Lua関数作成前の登録簿確保に失敗し、最大番号を消費しなかった登録結果。 */
    const TResult<void> allocation_rejected_at_maximum = vm.RegisterNativeFunction("AllocatorRejectedAtMaximum", &CountNativeCall, &rejected_call_count);
    EXPECT_TRUE(allocation_rejected_at_maximum.IsErr());
    if (allocation_rejected_at_maximum.IsErr()) {
        EXPECT_EQ(allocation_rejected_at_maximum.Error().category, EErrCategory::Memory);
        EXPECT_EQ(allocation_rejected_at_maximum.Error().subcode, script_err::kSub_AllocationFailed);
    }
    EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
    EXPECT_EQ(vm.GetGlobalNumber("ExhaustionWriteCount", -1.0), 0.0);

    allocator.SetRejectAllocations(false);
    /** 最大番号を消費し、実global表へ未公開なので失敗した登録結果。 */
    const TResult<void> maximum_rejected = vm.RegisterNativeFunction("MaximumNative", &CountNativeCall, &escaped_maximum_call_count);
    EXPECT_TRUE(maximum_rejected.IsErr());
    if (maximum_rejected.IsErr()) {
        EXPECT_EQ(maximum_rejected.Error().category, EErrCategory::Generic);
        EXPECT_EQ(maximum_rejected.Error().subcode, script_err::kSub_CallFailed);
    }

    /** 枯渇拒否前までに登録簿が要求した確保回数。 */
    const u32 allocation_attempts_before_rejection = allocator.AllocationAttemptCount();
    /** 枯渇拒否前に登録簿が保持している確保数。 */
    const u32 outstanding_allocations_before_rejection = allocator.OutstandingAllocationCount();
    EXPECT_EQ(vm.GetGlobalNumber("ExhaustionWriteCount", -1.0), 1.0);

    /** 最大番号割り当て後に初めて拒否された登録結果。 */
    const TResult<void> rejected = vm.RegisterNativeFunction("RejectedAfterExhaustion", &CountNativeCall, &rejected_call_count);
    EXPECT_TRUE(rejected.IsErr());
    if (rejected.IsErr()) {
        EXPECT_EQ(rejected.Error().category, EErrCategory::Generic);
        EXPECT_EQ(rejected.Error().subcode, script_err::kSub_CallFailed);
    }

    /** 枯渇状態が解除されず二回目も拒否される登録結果。 */
    const TResult<void> rejected_again = vm.RegisterNativeFunction("RejectedAfterExhaustion", &CountNativeCall, &rejected_call_count);
    EXPECT_TRUE(rejected_again.IsErr());
    if (rejected_again.IsErr()) {
        EXPECT_EQ(rejected_again.Error().category, EErrCategory::Generic);
        EXPECT_EQ(rejected_again.Error().subcode, script_err::kSub_CallFailed);
    }
    EXPECT_EQ(allocator.AllocationAttemptCount(), allocation_attempts_before_rejection);
    EXPECT_EQ(allocator.OutstandingAllocationCount(), outstanding_allocations_before_rejection);
    EXPECT_EQ(vm.GetGlobalNumber("ExhaustionWriteCount", -1.0), 1.0);

    /** 拒否した名前が未公開で、退避された最大番号のLua関数が復元済み位置を呼ばないことを確認するコード。 */
    constexpr char kVerifyExhaustionScript[] =
        "setmetatable(_G, nil); "
        "assert(rawget(_G, 'AllocatorRejectedAtMaximum') == nil); "
        "assert(rawget(_G, 'MaximumNative') == nil); "
        "assert(rawget(_G, 'RejectedAfterExhaustion') == nil); "
        "assert(type(EscapedMaximumNative) == 'function'); "
        "EscapedMaximumNative()";
    EXPECT_TRUE(vm.LoadScript(kVerifyExhaustionScript, static_cast<u32>(sizeof(kVerifyExhaustionScript) - 1u), "verify-registration-exhaustion").IsOk());
    EXPECT_EQ(escaped_maximum_call_count, 0u);
    EXPECT_EQ(rejected_call_count, 0u);

    vm.Shutdown();
    EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
    EXPECT_TRUE(vm.Init().IsOk());

    /** 再初期化後の拒否がLuaへの書き込みを始めないことを確認する設定。 */
    constexpr char kTrackRestartWritesScript[] =
        "RestartWriteCount = 0; "
        "setmetatable(_G, {__newindex = function(target, name, value) "
        "RestartWriteCount = RestartWriteCount + 1; rawset(target, name, value) end})";
    EXPECT_TRUE(vm.LoadScript(kTrackRestartWritesScript, static_cast<u32>(sizeof(kTrackRestartWritesScript) - 1u), "track-restart-writes").IsOk());

    /** 再初期化後も枯渇番号を再利用しない登録結果。 */
    const TResult<void> rejected_after_restart = vm.RegisterNativeFunction("RejectedAfterRestart", &CountNativeCall, &rejected_call_count);
    EXPECT_TRUE(rejected_after_restart.IsErr());
    if (rejected_after_restart.IsErr()) {
        EXPECT_EQ(rejected_after_restart.Error().category, EErrCategory::Generic);
        EXPECT_EQ(rejected_after_restart.Error().subcode, script_err::kSub_CallFailed);
    }
    EXPECT_EQ(allocator.AllocationAttemptCount(), allocation_attempts_before_rejection);
    EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
    EXPECT_EQ(vm.GetGlobalNumber("RestartWriteCount", -1.0), 0.0);
    EXPECT_EQ(rejected_call_count, 0u);

    /** 再初期化後の拒否名も実global表へ追加されていないことを確認するコード。 */
    constexpr char kVerifyRestartRejectionScript[] = "setmetatable(_G, nil); assert(rawget(_G, 'RejectedAfterRestart') == nil)";
    EXPECT_TRUE(vm.LoadScript(kVerifyRestartRejectionScript, static_cast<u32>(sizeof(kVerifyRestartRejectionScript) - 1u), "verify-restart-rejection").IsOk());

    vm.Shutdown();
}
