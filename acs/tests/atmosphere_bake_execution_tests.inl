// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_TESTS_ATMOSPHERE_BAKE_EXECUTION_TESTS_INL
#define ACS_TESTS_ATMOSPHERE_BAKE_EXECUTION_TESTS_INL

namespace {

/** 記録・提出・完了を分け、完了前の読み戻しを誤用として記録する試験デバイス。 */
class ADeferredBakeDevice final : public ABakeContractDevice {
public:
    /** 既定の即時型と区別する試験専用名。実描画方式の代わりには使わない。 */
    const char* BackendName() const noexcept override { return "AtmosphereBakeDeferredTest"; }
    /** 提出済みの画像だけを完成させる。未提出の命令には触れない。 */
    void WaitIdle() noexcept override
    {
        ++WaitCalls;
        if (m_SubmittedImage == nullptr) return;
        m_SubmittedImage->WriteFromDispatch_Internal(m_SubmittedGeneration);
        WrittenGeneration = m_SubmittedGeneration;
        m_SubmittedImage = nullptr;
        m_SubmittedGeneration = 0u;
    }
    /** 最新の記録世代が完成した後だけ、元の読み戻し処理へ渡す。 */
    bool ReadTexture(IRhiTexture& texture, void* destination, u32 destination_size) noexcept override
    {
        ++ReadAttempts;
        // 偽生成関数が作った固定長の画像だけを扱う。
        const auto& source = static_cast<const ABakeContractTexture&>(texture);
        if (source.Generation() == 0u || source.Generation() != LastRecordedGeneration) {
            ++EarlyReadAttempts;
            return false;
        }
        return ABakeContractDevice::ReadTexture(texture, destination, destination_size);
    }
    /** 画像を書いたことにはせず、記録した世代だけを進める。 */
    u32 RecordImageGeneration() noexcept { return ++LastRecordedGeneration; }
    /** 命令口から提出済みの画像を借用する。次の完了待ちまで資源を破棄してはならない。 */
    bool QueueImage(ABakeContractTexture* image, u32 generation) noexcept
    {
        if (image == nullptr || generation == 0u || m_SubmittedImage != nullptr) return false;
        m_SubmittedImage = image;
        m_SubmittedGeneration = generation;
        ++SubmittedImages;
        return true;
    }

    /** 最後に命令口へ記録した画像の世代。未提出でも進む。 */
    u32 LastRecordedGeneration = 0u;
    /** 完成済みかにかかわらず読み戻し口へ入った回数。 */
    u32 ReadAttempts = 0u;
    /** 必要な世代が完成する前に読み戻した回数。 */
    u32 EarlyReadAttempts = 0u;
    /** 明示的な完了待ちの回数。読み戻しからは増やさない。 */
    u32 WaitCalls = 0u;
    /** 命令口から受け取った提出済み画像の数。 */
    u32 SubmittedImages = 0u;

private:
    /** 提出済みだが未完成の画像。未提出の記録はここへ入れない。 */
    ABakeContractTexture* m_SubmittedImage = nullptr;
    /** 提出済み画像へ書く世代。 */
    u32 m_SubmittedGeneration = 0u;
};

/** 即時型の描画用の空実装を再利用し、画像の記録と提出だけを遅延型へ差し替える。 */
class ADeferredBakeCommand final : public ABakeContractCommand {
public:
    /** 命令口より長く生きる試験デバイスを借用する。 */
    explicit ADeferredBakeCommand(ADeferredBakeDevice& device) noexcept : ABakeContractCommand(device), m_Device(device) {}
    /** 未提出の記録を捨て、新しい記録を始める。GPUの画像は完成させない。 */
    void Begin() noexcept override
    {
        ++BeginCalls;
        m_Ended = false;
        m_IsImagePipeline = false;
        m_Output = nullptr;
        m_RecordedImage = nullptr;
        m_RecordedGeneration = 0u;
    }
    /** 記録を閉じるだけで、提出や画像の完成は行わない。 */
    void End() noexcept override { ++EndCalls; m_Ended = true; }
    /** 閉じた記録をデバイスへ渡す。完了待ちまでは画像を更新しない。 */
    bool Submit() noexcept override
    {
        ++SubmitCalls;
        return m_Ended && m_Device.QueueImage(m_RecordedImage, m_RecordedGeneration);
    }
    /** 画像を出力する処理かを保持し、前の出力先を失効させる。 */
    void SetComputePipeline(IRhiPipeline& pipeline) noexcept override
    {
        m_IsImagePipeline = static_cast<const ABakeContractPipeline&>(pipeline).IsBake();
        m_Output = nullptr;
    }
    /** 書込み先を記録するだけで、画像は変更しない。 */
    void BindUav(u32 slot, IRhiTexture& texture) noexcept override
    {
        if (slot == 0u) m_Output = &static_cast<ABakeContractTexture&>(texture);
    }
    /** 有効な画像生成命令を保存する。大気表の依存関係や計算式は今回の対象外。 */
    void Dispatch(u32 groups_x, u32 groups_y, u32 groups_z) noexcept override
    {
        if (m_Ended || groups_x == 0u || groups_y == 0u || groups_z == 0u) return;
        RecordDispatch(m_DeferredStatistics);
        if (!m_IsImagePipeline || m_Output == nullptr || !m_Output->IsTestImage()) return;
        if (groups_x < (kBakeWidth + 7u) / 8u || groups_y < (kBakeHeight + 7u) / 8u) return;
        m_RecordedImage = m_Output;
        m_RecordedGeneration = m_Device.RecordImageGeneration();
    }

    /** 呼出側が記録を再開させられていないことを確認する回数。 */
    u32 BeginCalls = 0u;
    /** 呼出側の記録を勝手に終了していないことを確認する回数。 */
    u32 EndCalls = 0u;
    /** 呼出側の描画を勝手に提出していないことを確認する回数。 */
    u32 SubmitCalls = 0u;

private:
    /** この命令口の統計だけを保持する。 */
    FRhiCommandStatistics& StatisticsStorage() noexcept override { return m_DeferredStatistics; }
    /** 記録済み命令数を読み取る。 */
    const FRhiCommandStatistics& StatisticsStorage() const noexcept override { return m_DeferredStatistics; }
    /** 提出と完了待ちを担当する借用先。 */
    ADeferredBakeDevice& m_Device;
    /** 選択中の処理が画像を書き出すか。 */
    bool m_IsImagePipeline = false;
    /** 記録を閉じたか。 */
    bool m_Ended = false;
    /** 次の計算命令の出力先。 */
    ABakeContractTexture* m_Output = nullptr;
    /** 未提出の画像生成命令。Beginで捨ててもGPUの完了状態は変わらない。 */
    ABakeContractTexture* m_RecordedImage = nullptr;
    /** 未提出の画像生成世代。 */
    u32 m_RecordedGeneration = 0u;
    /** 即時型の基底クラスとは分離した命令統計。 */
    FRhiCommandStatistics m_DeferredStatistics{};
};

} // namespace

ACS_TEST(AtmosphereBakeExecution, ProbeSeparatesRecordingSubmissionAndCompletion)
{
    // 画像より長く生き、提出済みの命令だけを完成させるデバイス。
    ADeferredBakeDevice device;
    // 実体は常に小さい9×3画像だけで、実GPUは作らない。
    FTextureDesc description{};
    description.width = kBakeWidth;
    description.height = kBakeHeight;
    description.format = EFormat::R32G32B32A32_Float;
    description.is_uav = true;
    // 完了待ちまで生存する試験画像。
    ABakeContractTexture texture(description);
    // 画像生成という役割だけを表す試験用処理。
    ABakeContractPipeline pipeline(true);
    // 明示的な提出と待機の各段階を観測する命令口。
    ADeferredBakeCommand command(device);
    // 読み戻し先は初めから必要寸法を用意する。
    TArray<f32> output;
    // 通常の小さい確保が失敗した場合は、失敗を記録して終了する。
    const bool prepared = output.TrySetNum(kBakeElements);
    EXPECT_TRUE(prepared);
    if (!prepared) return;
    command.Begin();
    command.SetComputePipeline(pipeline);
    command.BindUav(0u, texture);
    command.Dispatch(2u, 1u, 1u);
    device.WaitIdle();
    EXPECT_EQ(texture.Generation(), 0u);
    EXPECT_FALSE(device.ReadTexture(texture, output.GetData(), kBakeTransferBytes));
    command.End();
    EXPECT_TRUE(command.Submit());
    EXPECT_EQ(texture.Generation(), 0u);
    EXPECT_FALSE(device.ReadTexture(texture, output.GetData(), kBakeTransferBytes));
    device.WaitIdle();
    EXPECT_TRUE(device.ReadTexture(texture, output.GetData(), kBakeTransferBytes));
    EXPECT_EQ(device.EarlyReadAttempts, 2u);
    EXPECT_EQ(device.SubmittedImages, 1u);
    ExpectGeneratedOutput_Internal(output, *output.GetAllocator(), 1u);
}

ACS_TEST(AtmosphereBakeExecution, UnsupportedSynchronousBakeRejectsBeforeRecordingOrReading)
{
    // 出力専用の確保元。
    CSystemAllocator output_allocator;
    // 命令を記録しただけでは画像を完成させないデバイス。
    ADeferredBakeDevice device;
    // 同期画像生成の実製品オブジェクト。
    CSkyAtmosphere atmosphere;
    // 利用者の描画所有権を表す、借用される命令口。
    ADeferredBakeCommand command(device);
    // 呼出前の値が失敗時には保持される出力。
    TArray<f32> output(output_allocator);
    // 初期化失敗と実行順の失敗を取り違えないための結果。
    const auto initialized = atmosphere.Init(device);
    EXPECT_TRUE(initialized.IsOk());
    if (initialized.IsErr()) return;
    // 小さい既存画像の準備結果。
    const bool prepared = PrepareOriginalOutput_Internal(output);
    EXPECT_TRUE(prepared);
    if (!prepared) return;
    // 今回は実行順だけが対象なので、物理パラメータは既定値。
    const FAtmosphereParams parameters{};
    command.Begin();
    // 従来の同期口はDiligent専用。未対応方式では生成命令も記録せず拒否する。
    const bool baked = atmosphere.BakeEquirect(device, command, parameters, kBakeWidth, kBakeHeight, output);
    EXPECT_EQ(device.ReadAttempts, 0u);
    EXPECT_EQ(device.EarlyReadAttempts, 0u);
    EXPECT_EQ(device.BufferUpdates, 0u);
    EXPECT_EQ(device.LastRecordedGeneration, 0u);
    EXPECT_EQ(command.Statistics().dispatch_calls, 0u);
    EXPECT_EQ(command.BeginCalls, 1u);
    EXPECT_EQ(command.EndCalls, 0u);
    EXPECT_EQ(command.SubmitCalls, 0u);
    EXPECT_FALSE(baked);
    ExpectOriginalOutput_Internal(output, output_allocator);
    // 未提出の記録は借用側自身が捨てる。試験終了後へ未完成資源を持ち越さない。
    command.Begin();
}

ACS_TEST(AtmosphereBakeExecution, RecordedImageWaitsForOwnerSubmissionAndCompletion)
{
    // GPU完了と命令記録を混同しない試験デバイス。
    ADeferredBakeDevice device;
    // 提出する画像資源を所有する製品オブジェクト。
    CSkyAtmosphere atmosphere;
    // Begin/End/Submitは、この呼出側だけが担当する。
    ADeferredBakeCommand command(device);
    // 完成した全要素を検査する読み戻し領域。
    TArray<f32> output;
    // 資源初期化の成否。
    const auto initialized = atmosphere.Init(device);
    EXPECT_TRUE(initialized.IsOk());
    if (initialized.IsErr()) return;
    // 全画像分だけを確保する。
    const bool prepared = output.TrySetNum(kBakeElements);
    EXPECT_TRUE(prepared);
    if (!prepared) return;
    // 実行順に限定した試験なので、大気条件は既定値。
    const FAtmosphereParams parameters{};
    for (u32 generation = 1u; generation <= 2u; ++generation) {
        command.Begin();
        command.ResetStatistics();
        // ここでは命令が記録されるだけで、読み戻しは起きない。
        IRhiTexture* const image = atmosphere.RecordEquirectAtAltitude(device, command, parameters, kBakeWidth, kBakeHeight, 2.0f);
        EXPECT_TRUE(image != nullptr);
        if (image == nullptr) return;
        EXPECT_EQ(device.ReadAttempts, (generation - 1u) * 3u);
        EXPECT_EQ(device.WaitCalls, generation - 1u);
        EXPECT_EQ(command.BeginCalls, generation);
        EXPECT_EQ(command.EndCalls, generation - 1u);
        EXPECT_EQ(command.SubmitCalls, generation - 1u);
        EXPECT_EQ(command.Statistics().dispatch_calls, u64{3});
        // 未提出および未完了の画像を、試験デバイスは成功として読ませない。
        EXPECT_FALSE(device.ReadTexture(*image, output.GetData(), kBakeTransferBytes));
        command.End();
        EXPECT_TRUE(command.Submit());
        EXPECT_FALSE(device.ReadTexture(*image, output.GetData(), kBakeTransferBytes));
        device.WaitIdle();
        EXPECT_TRUE(device.ReadTexture(*image, output.GetData(), kBakeTransferBytes));
        ExpectGeneratedOutput_Internal(output, *output.GetAllocator(), generation);
    }
}

ACS_TEST(AtmosphereBakeExecution, InvalidRecordedImageDoesNotChangeCommandsOrResources)
{
    // 無効な要求がGPU操作へ進まないことを観測する。
    ADeferredBakeDevice device;
    // 本物の入口へ要求を渡す製品オブジェクト。
    CSkyAtmosphere atmosphere;
    // 未提出の呼出側の命令口。
    ADeferredBakeCommand command(device);
    // 検査用の既定大気条件。
    const FAtmosphereParams parameters{};
    // 資源初期化の成否。
    const auto initialized = atmosphere.Init(device);
    EXPECT_TRUE(initialized.IsOk());
    if (initialized.IsErr()) return;
    // 初期化時に必要だった画像作成数。
    const u32 initialTextures = device.TextureFactoryCalls;
    command.Begin();
    EXPECT_TRUE(atmosphere.RecordEquirectAtAltitude(device, command, parameters, 0u, kBakeHeight, 2.0f) == nullptr);
    EXPECT_TRUE(atmosphere.RecordEquirectAtAltitude(device, command, parameters, 65536u, 65536u, 2.0f) == nullptr);
    EXPECT_EQ(device.TextureFactoryCalls, initialTextures);
    EXPECT_EQ(device.BufferUpdates, 0u);
    EXPECT_EQ(device.ReadAttempts, 0u);
    EXPECT_EQ(command.Statistics().dispatch_calls, u64{0});
    EXPECT_EQ(command.BeginCalls, 1u);
    EXPECT_EQ(command.EndCalls, 0u);
    EXPECT_EQ(command.SubmitCalls, 0u);
}

#endif
