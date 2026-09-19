// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_TESTS_ATMOSPHERE_BAKE_RECORD_GPU_TESTS_INL
#define ACS_TESTS_ATMOSPHERE_BAKE_RECORD_GPU_TESTS_INL

#include "render/IRhiCommandList.h"
#include "memory/UniquePtr.h"

#if !WITH_RENDER_DILIGENT
namespace {

// 一連の実GPU試験の資源を所有する。完了不明の失敗では、GPU参照先を解放しない。
struct FAtmosphereBakeRecordGpuResources {
    // 命令列と大気より後に破棄する、今回だけの描画デバイス。
    TUniquePtr<IRhiDevice> Device;
    // 借用画像を含む全ての大気資源の所有者。
    TUniquePtr<CSkyAtmosphere> Atmosphere;
    // この試験が開始・終了・提出を担当する専用命令列。
    TUniquePtr<IRhiCommandList> Command;
    // 提出やコピーが完了したと確認できるまでは、早期returnでも全資源を保持する。
    bool CompletionUnconfirmed = false;

    // 完了確認済みの正常系だけ解放する。失敗時の保持は試験プロセス終了までの意図的な措置。
    ~FAtmosphereBakeRecordGpuResources() noexcept
    {
        if (CompletionUnconfirmed) {
            test::RecordInfo(FSourceLoc::Current(), "大気の実GPU試験: 完了を確認できないため、デバイス・命令列・大気資源を解放せず保持する。");
            (void)Command.Release();
            (void)Atmosphere.Release();
            (void)Device.Release();
            return;
        }
        // 部分初期化の失敗も、GPUから参照されていなければ通常どおり片付ける。
        if (Atmosphere) Atmosphere->Shutdown();
    }
};

// 8画素単位の計算境界と、読み戻しの行詰めを同時に通す幅。
static constexpr u32 kBakeRecordGpuWidth = 9u;
// 最終行も含む全画素を検査する画像高さ。
static constexpr u32 kBakeRecordGpuHeight = 3u;
// RGBA全成分の数。先頭画素だけでは部分生成・部分コピーを見逃す。
static constexpr u32 kBakeRecordGpuElementCount = kBakeRecordGpuWidth * kBakeRecordGpuHeight * 4u;

// 製品の記録口を呼び、試験側で提出・待機・読み戻しを行う。失敗時は以後の再記録を禁止する。
static bool ReadRecordedAtmosphereGpu_Internal(FAtmosphereBakeRecordGpuResources& resources, const FAtmosphereParams& parameters, f32 altitude, f32 (&pixels)[kBakeRecordGpuElementCount], u32 sample)
{
    // この資源群は前回の読み戻し完了後にしか再利用しない。
    EXPECT_FALSE(resources.CompletionUnconfirmed);
    if (resources.CompletionUnconfirmed) return false;
    // 一部しかコピーされない失敗も、有限性だけで通さず負値・アルファ検査で検出する。
    for (u32 element = 0u; element < kBakeRecordGpuElementCount; ++element) pixels[element] = -1.0f;
    // 成功した読み戻しまで保持する。記録口の回帰で内部提出された場合にも途中解放しない。
    resources.CompletionUnconfirmed = true;
    resources.Command->Begin();
    resources.Command->ResetStatistics();
    // 所有権は大気本体に残す。次の記録より前に、この画像の全参照命令を完了させる。
    IRhiTexture* image = resources.Atmosphere->RecordEquirectAtAltitude(*resources.Device, *resources.Command, parameters, kBakeRecordGpuWidth, kBakeRecordGpuHeight, altitude);
    // 記録数は完了の証拠ではない。毎回の透過率表・多重散乱表・画像生成の省略だけを検出する。
    const FRhiCommandStatistics statistics = resources.Command->Statistics();
    EXPECT_EQ(statistics.dispatch_calls, 3u);
    EXPECT_EQ(statistics.draw_calls, 0u);
    EXPECT_TRUE(image != nullptr);
    // 誤った寸法や形式を固定長のCPU領域へコピーしないため、提出前に記述情報を確認する。
    const bool matchingImage = image != nullptr && image->Width() == kBakeRecordGpuWidth && image->Height() == kBakeRecordGpuHeight && image->PixelFormat() == EFormat::R32G32B32A32_Float && image->MipLevels() == 1u && image->ArraySize() == 1u && image->SampleCount() == 1u && !image->IsCubemap();
    EXPECT_TRUE(matchingImage);
    // nullptrや統計不一致でも、既に記録された可能性のある処理を放置して資源破棄へ進まない。
    resources.Command->End();
    // falseは未実行を意味しない。待機も行うが、失敗した提出を合格へ読み替えない。
    const bool submitted = resources.Command->Submit();
    resources.Device->WaitIdle();
    // WaitIdleは戻り値を持たないため、少なくともデバイス喪失を別に確認する。
    const bool operational = resources.Device->IsOperational();
    test::RecordInfo(FSourceLoc::Current(), "大気の実GPU試験: 条件=%u 計算命令=%llu 提出=%u 動作継続=%u", sample, static_cast<unsigned long long>(statistics.dispatch_calls), submitted ? 1u : 0u, operational ? 1u : 0u);
    EXPECT_TRUE(submitted);
    EXPECT_TRUE(operational);
    if (!submitted || !operational || !matchingImage) return false;
    // この同期コピーもGPU処理を提出するため、その失敗時は大気・命令列・デバイスを保持する。
    const bool read = resources.Device->ReadTexture(*image, pixels, static_cast<u32>(sizeof(pixels)));
    // コピー中にデバイスを失った結果を、有効な画像として採用しない。
    const bool operationalAfterRead = resources.Device->IsOperational();
    EXPECT_TRUE(read);
    EXPECT_TRUE(operationalAfterRead);
    if (!read || !operationalAfterRead) return false;
    // RawDX12の同じキュー上で、生成の後に置いた同期コピーまで成功した場合に限り再利用する。
    resources.CompletionUnconfirmed = false;
    return statistics.dispatch_calls == 3u && statistics.draw_calls == 0u;
}

// 全27画素の値を検査する。これは物理精度や画質の受入ではなく、生成・提出・読み戻しの接続試験。
static bool ValidateRecordedAtmosphereGpu_Internal(const f32 (&pixels)[kBakeRecordGpuElementCount], bool dark, u32 sample)
{
    // 非有限値を検出した後は、差の比較でNaNを見逃さないよう呼出側の比較を止める。
    bool valid = true;
    // 明るい条件で全画像が黒になる失敗を検出する。特定の画素の物理的な期待輝度は規定しない。
    f64 totalRadiance = 0.0;
    for (u32 pixel = 0u; pixel < kBakeRecordGpuWidth * kBakeRecordGpuHeight; ++pixel) {
        // RGBA全成分が有限で、未書込みのアルファが残っていないことを確認する。
        const bool finite = IsFiniteProbeValue_Internal(pixels[pixel * 4u]) && IsFiniteProbeValue_Internal(pixels[pixel * 4u + 1u]) && IsFiniteProbeValue_Internal(pixels[pixel * 4u + 2u]) && IsFiniteProbeValue_Internal(pixels[pixel * 4u + 3u]);
        EXPECT_TRUE(finite);
        EXPECT_EQ(pixels[pixel * 4u + 3u], 1.0f);
        valid = valid && finite && pixels[pixel * 4u + 3u] == 1.0f;
        for (u32 channel = 0u; channel < 3u; ++channel) {
            // 太陽光ゼロでも有限性・アルファ・全成分書込みを省略しない。
            const f32 value = pixels[pixel * 4u + channel];
            EXPECT_TRUE(value >= 0.0f);
            if (dark) EXPECT_EQ(value, 0.0f);
            valid = valid && value >= 0.0f && (!dark || value == 0.0f);
            totalRadiance += static_cast<f64>(value);
        }
        test::RecordInfo(FSourceLoc::Current(), "大気の実GPU試験: 条件=%u 画素=%u RGBA=(%.9g,%.9g,%.9g,%.9g)", sample, pixel, pixels[pixel * 4u], pixels[pixel * 4u + 1u], pixels[pixel * 4u + 2u], pixels[pixel * 4u + 3u]);
    }
    if (!dark) {
        EXPECT_TRUE(totalRadiance > 0.0);
        valid = valid && totalRadiance > 0.0;
    }
    return valid;
}

} // 無名名前空間

// 製品Initと記録口を直接使うRawDX12試験。シェーダー抽出・差替え・CPUによる生成の代用はしない。
ACS_TEST(Atmosphere, RecordEquirectRawDx12SubmitsReadsAndRetriesWholeImage)
{
    // 途中失敗でも、完了不明の参照資源はまとめて保持する。
    FAtmosphereBakeRecordGpuResources resources;
    // 実GPUが使えない場合は失敗とし、未実行を合格として扱わない。
    FDeviceConfig configuration{};
    configuration.backend = ERhiBackendKind::D3D12;
    // 製品と同じデバイス生成口。Diligent構成では、このRaw専用試験自体を登録しない。
    auto deviceResult = CreateRhiDevice(configuration);
    EXPECT_TRUE(deviceResult.IsOk());
    if (deviceResult.IsErr()) return;
    resources.Device = Move(deviceResult.Value());
    test::RecordInfo(FSourceLoc::Current(), "大気の実GPU試験: 描画基盤=%s GPU=%s。物理精度・画質・他の描画基盤の受入ではない。", resources.Device->BackendName(), resources.Device->AdapterName());
    EXPECT_TRUE(::strcmp(resources.Device->BackendName(), "DX12") == 0);
    if (::strcmp(resources.Device->BackendName(), "DX12") != 0) return;
    // 専用命令列の寿命もデバイスと同じ所有群で管理する。
    auto commandResult = CreateRhiCommandList(*resources.Device);
    EXPECT_TRUE(commandResult.IsOk());
    if (commandResult.IsErr()) return;
    resources.Command = Move(commandResult.Value());
    resources.Atmosphere = MakeUnique<CSkyAtmosphere>();
    EXPECT_TRUE(static_cast<bool>(resources.Atmosphere));
    if (!resources.Atmosphere) return;
    // 実際の製品初期化で全てのシェーダーと参照表を作る。
    const auto initialized = resources.Atmosphere->Init(*resources.Device, EFormat::R16G16B16A16_Float);
    EXPECT_TRUE(initialized.IsOk());
    if (initialized.IsErr()) return;
    EXPECT_TRUE(resources.Atmosphere->Ready());
    // 新規作成した大気であり先行描画はない。最初の記録前にも動作状態を確認する。
    EXPECT_TRUE(resources.Device->IsOperational());
    if (!resources.Atmosphere->Ready() || !resources.Device->IsOperational()) return;
    // 日中の有色入射量。全成分を変えて、古い定数バッファを再使用する誤りを検出する。
    FAtmosphereParams parameters{};
    parameters.sun_dir = FVec3{0.0f, 1.0f, 0.0f};
    parameters.ground_albedo = FVec3{0.1f, 0.12f, 0.1f};
    // 非ゼロ高度により、地表へ向かう行にも有限の光路を持たせる。単位はm。
    constexpr f32 altitude = 1000.0f;
    // 基準・2倍・ゼロ・基準再試行。条件ごとの読み戻しまで終えてから次を記録する。
    constexpr f32 strengths[] = {1.0f, 2.0f, 0.0f, 1.0f};
    // 全画像を保持し、再試行・倍率・暗条件を先頭画素だけで判断しない。
    f32 results[4][kBakeRecordGpuElementCount]{};
    for (u32 sample = 0u; sample < 4u; ++sample) {
        parameters.sun_intensity = FVec3{strengths[sample], strengths[sample] * 0.5f, strengths[sample] * 0.25f};
        if (!ReadRecordedAtmosphereGpu_Internal(resources, parameters, altitude, results[sample], sample)) return;
        if (!ValidateRecordedAtmosphereGpu_Internal(results[sample], sample == 2u, sample)) return;
    }
    // 光輸送は固定した媒質・反射率に対し入射量へ線形応答する。2倍は入力の二進丸めも増やさない。
    // この許容差は接続検査用の単精度相対32刻みと絶対1e-7であり、放射輝度の物理誤差保証ではない。
    constexpr f64 relativeTolerance = 32.0 / 8388608.0;
    // ゼロ近傍の比較が丸め差だけで失敗しないための接続検査用の絶対幅。
    constexpr f64 absoluteTolerance = 1.0e-7;
    for (u32 pixel = 0u; pixel < kBakeRecordGpuWidth * kBakeRecordGpuHeight; ++pixel) {
        for (u32 channel = 0u; channel < 3u; ++channel) {
            // 事前に全画像の有限性を検査した値だけを使う。
            const f64 baseline = static_cast<f64>(results[0][pixel * 4u + channel]);
            // 全要素を比べ、前回の明るい画像を返すだけの実装を倍率と暗条件で拒否する。
            const f64 doubled = static_cast<f64>(results[1][pixel * 4u + channel]);
            // 暗条件の後に同じ入力へ戻して、同一資源と命令列の再利用を確認する。
            const f64 retried = static_cast<f64>(results[3][pixel * 4u + channel]);
            EXPECT_TRUE(::fabs(doubled - baseline * 2.0) <= absoluteTolerance + baseline * 2.0 * relativeTolerance);
            EXPECT_TRUE(::fabs(retried - baseline) <= absoluteTolerance + baseline * relativeTolerance);
        }
    }
}
#endif

#endif
