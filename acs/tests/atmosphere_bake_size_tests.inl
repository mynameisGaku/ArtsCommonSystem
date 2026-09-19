// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_TESTS_ATMOSPHERE_BAKE_SIZE_TESTS_INL
#define ACS_TESTS_ATMOSPHERE_BAKE_SIZE_TESTS_INL

ACS_TEST(AtmosphereBakeContract, InvalidDimensionsHaveNoSideEffects)
{
    // ゼロ、4GiB境界、片方だけ極端な寸法、要素数積が64bitでも桁あふれする組。
    constexpr u32 invalid_dimensions[][2] = {{0u,0u}, {0u,1u}, {1u,0u}, {0u,0xffffffffu}, {0xffffffffu,0u}, {16384u,16384u}, {0x10000000u,1u}, {1u,0x10000000u}, {0xffffffffu,1u}, {1u,0xffffffffu}, {0x80000000u,0x80000000u}, {0xffffffffu,0xffffffffu}};
    // 高度指定の有無にかかわらず、同じ公開契約で拒否する。
    for (u32 entry = 0u; entry < 2u; ++entry) {
        // 各条件を初期化直後から試し、前条件の参照表状態を使い回さない。
        for (const auto& dimensions : invalid_dimensions) {
            // 利用者出力の確保元は、既定確保元の差替えから分離する。
            CSystemAllocator output_allocator;
            // 画像生成要求とバッファ更新を観測する偽デバイス。
            ABakeContractDevice device;
            // 不正な入力を渡す実製品の大気オブジェクト。
            CSkyAtmosphere atmosphere;
            // GPUへ提出せず、記録された処理だけを数える命令口。
            ABakeContractCommand command(device);
            // 準備後は既定確保元からの新規確保を拒否する。出力の確保元とは別。
            ABakeContractBudgetAllocator guard_allocator(DefaultAllocator(), device);
            // 失敗時にも維持されるべき既存画像。
            TArray<f32> output(output_allocator);
            // 未初期化による早期失敗と取り違えないための準備結果。
            const auto initialized = atmosphere.Init(device);
            EXPECT_TRUE(initialized.IsOk());
            if (initialized.IsErr()) return;
            // 既存出力を、生成される画像と異なる値で満たす。
            const bool prepared = PrepareOriginalOutput_Internal(output);
            EXPECT_TRUE(prepared);
            if (!prepared) return;
            // 初期化中の要求・更新は、無効入力による副作用と区別する。
            const u32 initial_texture_calls = device.TextureFactoryCalls;
            // 変更前のバッファ更新回数。
            const u32 initial_buffer_updates = device.BufferUpdates;
            // 差替えが元へ戻ることを確認する借用先。
            IAllocator* const previous_default = &DefaultAllocator();
            device.RejectTextureCreation = true;
            guard_allocator.SetBudget(0u);
            // 寸法だけを変え、物理パラメータは既定値を使用する。
            const FAtmosphereParams parameters{};
            // 差替え区間で検査ログを出さないための結果保存先。
            bool baked = true;
            {
                // 製品の寸法検査が欠けていても、偽生成口と確保予算の両方で安全に止める。
                FScopedBakeContractDefaultAllocator allocator_scope(guard_allocator);
                command.Begin();
                baked = entry == 0u ? atmosphere.BakeEquirect(device, command, parameters, dimensions[0], dimensions[1], output) : atmosphere.BakeEquirectAtAltitude(device, command, parameters, dimensions[0], dimensions[1], 20.0f, output);
                command.End();
            }
            EXPECT_FALSE(baked);
            EXPECT_TRUE(&DefaultAllocator() == previous_default);
            EXPECT_EQ(device.TextureFactoryCalls, initial_texture_calls);
            EXPECT_EQ(device.BufferUpdates, initial_buffer_updates);
            EXPECT_EQ(command.Statistics().dispatch_calls, u64{0});
            EXPECT_EQ(device.ReadCalls, 0u);
            EXPECT_EQ(device.WrittenGeneration, 0u);
            EXPECT_EQ(guard_allocator.AllocationAttempts, 0u);
            EXPECT_EQ(guard_allocator.LiveAllocations, 0u);
            EXPECT_EQ(guard_allocator.InvalidLifetimeOperations, 0u);
            ExpectOriginalOutput_Internal(output, output_allocator);
        }
    }
}

ACS_TEST(AtmosphereBakeContract, RepresentableDimensionsReachBackendWithoutHugeAllocation)
{
    // RGBA32Fのu32転送上限直下と、単純な幅上限や幅+高さ判定では拒否してしまう組。
    constexpr u32 valid_dimensions[][2] = {{0x0fffffffu,1u}, {1u,0x0fffffffu}, {16383u,16384u}, {16384u,16383u}};
    // 寸法が表現可能でも、実GPUがその寸法を支えるという意味ではない。
    for (const auto& dimensions : valid_dimensions) {
        // 出力の確保元。巨大領域は要求しない。
        CSystemAllocator output_allocator;
        // 画像生成関数で必ず止める偽デバイス。
        ABakeContractDevice device;
        // 事前検査の対象となる実製品の大気オブジェクト。
        CSkyAtmosphere atmosphere;
        // 計算命令を即時に模擬する命令口。
        ABakeContractCommand command(device);
        // 画像生成関数で止まった後に保持される既存出力。
        TArray<f32> output(output_allocator);
        // 初期化失敗で誤合格させないための結果。
        const auto initialized = atmosphere.Init(device);
        EXPECT_TRUE(initialized.IsOk());
        if (initialized.IsErr()) return;
        // 比較可能な小さい既存出力を準備する。
        const bool prepared = PrepareOriginalOutput_Internal(output);
        EXPECT_TRUE(prepared);
        if (!prepared) return;
        // 製品から画像生成要求が一度届いたことを検査する基準値。
        const u32 initial_texture_calls = device.TextureFactoryCalls;
        device.RejectTextureCreation = true;
        // 寸法のみの条件なので物理パラメータは既定値。
        const FAtmosphereParams parameters{};
        command.Begin();
        EXPECT_FALSE(atmosphere.BakeEquirect(device, command, parameters, dimensions[0], dimensions[1], output));
        command.End();
        EXPECT_EQ(device.TextureFactoryCalls, initial_texture_calls + 1u);
        EXPECT_EQ(device.LastTextureWidth, dimensions[0]);
        EXPECT_EQ(device.LastTextureHeight, dimensions[1]);
        EXPECT_EQ(device.ReadCalls, 0u);
        EXPECT_EQ(device.WrittenGeneration, 0u);
        ExpectOriginalOutput_Internal(output, output_allocator);
    }
}

ACS_TEST(AtmosphereBakeContract, InvalidDimensionsPreservePreparedImageAndRetry)
{
    // ゼロ寸法、転送上限、u32積が0または小さい非0へ回り込む組を再試行する。
    constexpr u32 invalid_dimensions[][2] = {{0u,3u}, {9u,0u}, {16384u,16384u}, {65536u,65536u}, {65537u,65537u}, {0xffffffffu,0xffffffffu}};
    // 利用者出力は失敗注入する既定確保元と分離する。
    CSystemAllocator output_allocator;
    // 資源と確保元より長く生きる観測先。
    ABakeContractDevice device;
    // 無効入力中の、既定確保元からの新規確保を拒否する局所的な確保元。
    ABakeContractBudgetAllocator guard_allocator(DefaultAllocator(), device);
    // 初回の生成済み画像と参照表を、無効入力の後も同じオブジェクトで使う。
    CSkyAtmosphere atmosphere;
    // 同じ命令口の計算回数を、無効入力ごとに初期化して検査する。
    ABakeContractCommand command(device);
    // 成功済み画像の全要素が残ることを検査する出力。
    TArray<f32> output(output_allocator);
    // サイズ以外の条件を変えない物理パラメータ。
    const FAtmosphereParams parameters{};
    // 未初期化による拒否とは区別する。
    const auto initialized = atmosphere.Init(device);
    EXPECT_TRUE(initialized.IsOk());
    if (initialized.IsErr()) return;
    command.Begin();
    // 最初に正常画像を作り、資源と大気表が既に使われた状態にする。
    const bool warmed = atmosphere.BakeEquirect(device, command, parameters, kBakeWidth, kBakeHeight, output);
    command.End();
    EXPECT_TRUE(warmed);
    if (!warmed) return;
    ExpectGeneratedOutput_Internal(output, output_allocator, 1u);
    // 以後は同じ寸法の正常画像を使うため、新しい画像の生成要求は不要。
    const u32 cached_texture_calls = device.TextureFactoryCalls;
    // 無効入力の前に完成している画像の世代。
    u32 generation = 1u;
    for (const auto& dimensions : invalid_dimensions) {
        // 定数更新にも触れず戻ることを確認する基準値。
        const u32 previous_updates = device.BufferUpdates;
        command.ResetStatistics();
        device.RejectTextureCreation = true;
        guard_allocator.SetBudget(0u);
        // 差替え区間では検査ログを出さず結果だけを保存する。
        bool baked = true;
        {
            // 大きい寸法は偽生成口でも止め、製品が退行しても実際には確保しない。
            FScopedBakeContractDefaultAllocator allocator_scope(guard_allocator);
            command.Begin();
            baked = atmosphere.BakeEquirect(device, command, parameters, dimensions[0], dimensions[1], output);
            command.End();
        }
        EXPECT_FALSE(baked);
        EXPECT_EQ(device.TextureFactoryCalls, cached_texture_calls);
        EXPECT_EQ(device.BufferUpdates, previous_updates);
        EXPECT_EQ(device.ReadCalls, generation);
        EXPECT_EQ(device.WrittenGeneration, generation);
        EXPECT_EQ(command.Statistics().dispatch_calls, u64{0});
        EXPECT_EQ(guard_allocator.AllocationAttempts, 0u);
        ExpectGeneratedOutput_Internal(output, output_allocator, generation);
        device.RejectTextureCreation = false;
        command.Begin();
        // 無効入力で参照表を失効させたり、画像寸法だけを誤更新する不備も検出する。
        const bool retried = atmosphere.BakeEquirect(device, command, parameters, kBakeWidth, kBakeHeight, output);
        command.End();
        EXPECT_TRUE(retried);
        ++generation;
        EXPECT_EQ(command.Statistics().dispatch_calls, u64{1});
        EXPECT_EQ(device.TextureFactoryCalls, cached_texture_calls);
        EXPECT_EQ(device.ReadCalls, generation);
        EXPECT_EQ(device.WrittenGeneration, generation);
        ExpectGeneratedOutput_Internal(output, output_allocator, generation);
    }
}

#endif
