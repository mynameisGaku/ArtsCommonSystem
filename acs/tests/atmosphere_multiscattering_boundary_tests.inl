// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_TESTS_ATMOSPHERE_MULTISCATTERING_BOUNDARY_TESTS_INL
#define ACS_TESTS_ATMOSPHERE_MULTISCATTERING_BOUNDARY_TESTS_INL

// 試験用HLSLの指定文字列を一か所だけ置換する。空の検索語、欠落、重複では入力を変更しない。
static bool ReplaceMultiBoundaryOnce_Internal(FString& source, const char* needle, const char* replacement)
{
    if (!needle || !replacement || !*needle) return false;
    // 置換前の所有文字列を借用し、結果の構築後にだけ所有権を移す。
    const char* position = ::strstr(source.Data(), needle);
    if (!position || ::strstr(position + 1u, needle)) return false;
    // 検索語の前後は一字も変えず保持する。
    FString result(FStringView(source.Data(), static_cast<usize>(position - source.Data())));
    result.Append(replacement);
    result.Append(position + ::strlen(needle));
    source = Move(result);
    return true;
}

// 同じ製品スナップショットから真空・地表入力のCSMultiを作る。設定箇所の形式変更は失敗にする。
static FString BuildMultiBoundaryVacuumShader_Internal(const FString& product)
{
    // 媒質の係数だけを0にする。密度分布、交差判定、積分処理は製品の本文を使う。
    FString common = ReadAtmosphereCommonShader_Internal(product);
    // 入力生成以外の入口全体を保持するため、参照関数だけの抽出はしない。
    FString body = RestoreAtmosphereShaderSource_Internal(product, true, "kMultiCS");
    if (common.IsEmpty() || body.IsEmpty()) return {};
    if (!ReplaceMultiBoundaryOnce_Internal(common, "static const float3 kRayS  = float3(5.802, 13.558, 33.1) * 0.001;\n", "static const float3 kRayS  = float3(0.0,0.0,0.0);\n")) return {};
    if (!ReplaceMultiBoundaryOnce_Internal(common, "static const float  kMieS  = 3.996 * 0.001;\n", "static const float  kMieS  = 0.0;\n")) return {};
    if (!ReplaceMultiBoundaryOnce_Internal(common, "static const float  kMieE  = 4.4 * 0.001;\n", "static const float  kMieE  = 0.0;\n")) return {};
    if (!ReplaceMultiBoundaryOnce_Internal(common, "static const float3 kOzoneA = float3(0.650, 1.881, 0.085) * 0.001;\n", "static const float3 kOzoneA = float3(0.0,0.0,0.0);\n")) return {};
    // 表の配置には依存せず、全画素を地表半径に固定し、太陽の余弦だけ入力画像から受け取る。
    constexpr const char* input = "  float2 uv=(float2(id.xy)+0.5)/float2(W,H);\n  float cosSun=uv.x*2.0-1.0;\n  float r=kBottom + uv.y*(kTop-kBottom);\n";
    // yは地表反射を無効にする対照専用。通常側の入口はyを参照しない。
    constexpr const char* replacement = "  float4 boundaryCase=multiBoundaryInput.Load(int3(id.xy,0));\n  float cosSun=boundaryCase.x;\n  float r=kBottom;\n";
    if (!ReplaceMultiBoundaryOnce_Internal(body, input, replacement)) return {};
    common.Append("// xは太陽天頂角の余弦、yは地表反射の対照用許可値。\nTexture2D<float4> multiBoundaryInput : register(t1);\n");
    common.Append(body.View());
    return common;
}

// 唯一箇所の置換が失敗時に元の文字列を壊さず、重複や欠落を勝手に採用しないことを検査する。
ACS_TEST(Atmosphere, MultiBoundaryReplacementRequiresUniqueMatch)
{
    // 前後の保持も確認できる最小の置換対象。
    FString source("before NEEDLE after");
    EXPECT_TRUE(ReplaceMultiBoundaryOnce_Internal(source, "NEEDLE", "value"));
    EXPECT_TRUE(source == FStringView("before value after"));
    EXPECT_FALSE(ReplaceMultiBoundaryOnce_Internal(source, "missing", "value"));
    EXPECT_FALSE(ReplaceMultiBoundaryOnce_Internal(source, "", "value"));
    EXPECT_FALSE(ReplaceMultiBoundaryOnce_Internal(source, nullptr, "value"));
    EXPECT_FALSE(ReplaceMultiBoundaryOnce_Internal(source, "value", nullptr));
    EXPECT_TRUE(source == FStringView("before value after"));
    // 一致が二つあれば部分置換せず、元の文字列のまま拒否する。
    FString duplicate("NEEDLE middle NEEDLE");
    EXPECT_FALSE(ReplaceMultiBoundaryOnce_Internal(duplicate, "NEEDLE", "value"));
    EXPECT_TRUE(duplicate == FStringView("NEEDLE middle NEEDLE"));
    // 開始位置が重なる二つの一致も、一意として扱わない。
    FString overlap("aaa");
    EXPECT_FALSE(ReplaceMultiBoundaryOnce_Internal(overlap, "aa", "value"));
    EXPECT_TRUE(overlap == FStringView("aaa"));
}

// 地表から内向きの距離0でもLambert反射が方向平均へ入ることを、製品CSMulti全体で検査する。
// sky_ground_render_tests.cppの既存補助処理より後から取り込む。製品修正前の失敗確認は親が行う。
ACS_TEST(Atmosphere, WholeMultiEntryKeepsZeroDistanceGroundReflection)
{
    // 一回の読取りを共有し、並行編集前後の共通部と入口を混ぜない。
    const FString product = ReadRenderSource_Internal(L"../src/render/Atmosphere.cpp");
    EXPECT_TRUE(!product.IsEmpty());
    if (product.IsEmpty()) return;
    // 真空係数と入力だけを設定した通常側。tGroundとtMaxの分岐は一切変更しない。
    const FString source = BuildMultiBoundaryVacuumShader_Internal(product);
    EXPECT_TRUE(!source.IsEmpty());
    if (source.IsEmpty()) return;
    // 地表分岐を入力y=0で無効にする検出力の対照。元の入口や媒質係数を追加で変えない。
    FString noGroundSource(source);
    // 条件は画像から読むため、対照側だけ資源宣言が定数最適化で消えることも避けられる。
    const bool mutationReady = ReplaceMultiBoundaryOnce_Internal(noGroundSource, "    if(hitGround){", "    if(hitGround && boundaryCase.y>0.0){");
    EXPECT_TRUE(mutationReady);
    if (!mutationReady) return;

    // 製品の固定寸法をすべて実行し、端や未書込み画素を検査から除外しない。
    constexpr u32 width = 32u;
    // 8x8の群を4x4個提出する。
    constexpr u32 height = 32u;
    // 半精度変換を挟まない読戻しの全バイト数。
    constexpr u32 byteCount = width * height * sizeof(FVec4);
    static_assert(sizeof(FVec4) == 4u * sizeof(f32), "画像入出力には連続したRGBA32Fを使う");
    // 独立解析値: 真空では体積寄与0、透過率1、散乱の戻り率0。下半球だけが反射する。
    // Lambert面の方向別放射輝度は0.3/piで、球面平均は(2*pi)/(4*pi)倍になる。
    // 製品の64方向もy<0が32個で接線方向はないため、この条件では半球平均が厳密に半分。
    constexpr f64 expectedDay = 0.3 / (2.0 * 3.14159265358979323846);
    // 32回の単精度加算と除算の丸めを許す絶対誤差。0や1方向分の欠落は許容しない。
    constexpr f64 tolerance = 1.0e-6;
    static_assert(tolerance < expectedDay / 64.0, "黒一色や地表方向の欠落を丸め誤差として認めない");

    // 透過率の生成・補間誤差を持ち込まない全成分1の既知画像。
    TArray<FVec4> transValues;
    transValues.SetNum(256u * 64u);
    for (FVec4& value : transValues) value = FVec4{1.0f,1.0f,1.0f,1.0f};
    // 各行に真上の太陽と真下の太陽を交互配置し、両条件を全域で繰り返す。
    TArray<FVec4> inputValues;
    inputValues.SetNum(width * height);
    for (u32 pixel = 0u; pixel < width * height; ++pixel) inputValues[pixel] = FVec4{pixel % 2u == 0u ? 1.0f : -1.0f,0.0f,0.0f,1.0f};
    // GPU画像とCPU読戻し先をともに未書込みのNaNで初期化する。
    TArray<FVec4> unwritten;
    unwritten.SetNum(width * height);
    // 数値演算を挟まず、既存の補助処理でNaNを構成する。
    const f32 nan = ProbeFloatFromBits_Internal(0x7fc00000u);
    for (FVec4& value : unwritten) value = FVec4{nan,nan,nan,nan};

    // デバイスを全資源より先に作り、全ての完了・破棄より後まで保持する。
    FDeviceConfig configuration{};
    // 未使用のGPUを理由に、この境界試験を合格扱いや省略扱いにはしない。
    auto device = CreateRhiDevice(configuration);
    EXPECT_TRUE(device.IsOk());
    if (device.IsErr()) return;
    test::RecordInfo(FSourceLoc::Current(), "multi_boundary_gpu backend=%s adapter=%s width=%u height=%u expected_day=%.12g expected_night=0 tolerance=%.9g", device.Value()->BackendName(), device.Value()->AdapterName(), width, height, expectedDay, tolerance);
    // 製品がt0で参照する透過率表と同寸法・同じRGBA32Fを使う。
    FTextureDesc transDescription{};
    transDescription.width = 256u;
    transDescription.height = 64u;
    transDescription.format = EFormat::R32G32B32A32_Float;
    transDescription.initial_data = transValues.GetData();
    transDescription.initial_data_size = transValues.Num() * sizeof(FVec4);
    // 通常側と対照側が共有する、読み取り専用の透過率画像。
    auto transTexture = CreateRhiTexture(*device.Value(), transDescription);
    EXPECT_TRUE(transTexture.IsOk());
    if (transTexture.IsErr()) return;
    // 検査条件だけをt1へ渡す。表の配置を決める製品側の係数とは切り離す。
    FTextureDesc inputDescription = transDescription;
    inputDescription.width = width;
    inputDescription.height = height;
    inputDescription.initial_data = inputValues.GetData();
    inputDescription.initial_data_size = byteCount;
    // 入力値の所有配列は全提出の完了まで生存させる。
    auto inputTexture = CreateRhiTexture(*device.Value(), inputDescription);
    EXPECT_TRUE(inputTexture.IsOk());
    if (inputTexture.IsErr()) return;

#if !WITH_RENDER_DILIGENT
    // RawDX12では既定のSM5.1と明示指定のSM6の両方を検査する。
    constexpr u32 variantCount = 2u;
#else
    // Diligentは既存方式の一形式だけを対象にし、未実行の形式を成功として数えない。
    constexpr u32 variantCount = 1u;
#endif
    // 通常側と対照側の提出・読戻し完了数。途中失敗を黙って飛ばさない。
    u32 completed[variantCount]{};
    for (u32 variant = 0u; variant < variantCount; ++variant) {
        // 既定形式は既存試験に合わせ、targetをnullptrのまま渡す。
        const char* targetName = variant == 0u ? "default(cs_5_1)" : "cs_6_0";
        for (u32 mutation = 0u; mutation < 2u; ++mutation) {
            // 計算本文を保持した製品入口をコンパイルする。
            FShaderDesc shaderDescription{};
            shaderDescription.stage = EShaderStage::Compute;
            shaderDescription.hlsl_source = mutation == 0u ? source.Data() : noGroundSource.Data();
            shaderDescription.entry_point = "CSMulti";
            shaderDescription.debug_name = mutation == 0u ? "Atmo.MultiBoundaryVacuum" : "Atmo.MultiBoundaryNoGround";
            if (variant != 0u) shaderDescription.target = "cs_6_0";
            test::RecordInfo(FSourceLoc::Current(), "multi_boundary_compile target=%s mutation=%u entry=CSMulti", targetName, mutation);
            // 一方の形式が失敗しても記録し、もう一方の診断は続ける。
            auto shader = CreateRhiShader(*device.Value(), shaderDescription);
            EXPECT_TRUE(shader.IsOk());
            if (shader.IsErr()) continue;
            // t0/u0は製品と同じ名前。t1だけが明示入力用の追加資源。
            FComputePipelineDesc pipelineDescription{};
            pipelineDescription.cs = shader.Value().Get();
            pipelineDescription.srv_slots = 2u;
            pipelineDescription.srv_names[0] = "transLut";
            pipelineDescription.srv_names[1] = "multiBoundaryInput";
            pipelineDescription.uav_slots = 1u;
            pipelineDescription.uav_names[0] = "msOut";
            // シェーダーより後に作り、その手前で破棄する。
            auto pipeline = CreateRhiComputePipeline(*device.Value(), pipelineDescription);
            EXPECT_TRUE(pipeline.IsOk());
            if (pipeline.IsErr()) continue;
            // 各形式・対照を新しいNaN画像へ出力し、前回の値を流用しない。
            FTextureDesc outputDescription{};
            outputDescription.width = width;
            outputDescription.height = height;
            outputDescription.format = EFormat::R32G32B32A32_Float;
            outputDescription.is_uav = true;
            outputDescription.initial_data = unwritten.GetData();
            outputDescription.initial_data_size = byteCount;
            // 完了待ちと読戻しまで保持する今回だけの出力先。
            auto outputTexture = CreateRhiTexture(*device.Value(), outputDescription);
            EXPECT_TRUE(outputTexture.IsOk());
            if (outputTexture.IsErr()) continue;
            // コマンドを最後に作り、参照する全資源より先に破棄する。
            auto command = CreateRhiCommandList(*device.Value());
            EXPECT_TRUE(command.IsOk());
            if (command.IsErr()) continue;
            command.Value()->Begin();
            command.Value()->SetComputePipeline(*pipeline.Value());
            command.Value()->SetTexture(0u, *transTexture.Value());
            command.Value()->SetTexture(1u, *inputTexture.Value());
            command.Value()->BindUav(0u, *outputTexture.Value());
            command.Value()->Dispatch(width / 8u, height / 8u, 1u);
            command.Value()->End();
            // 提出が失敗しても、既存RHIの完了待ちを経て資源を解放する。
            const bool submitted = command.Value()->Submit();
            device.Value()->WaitIdle();
            EXPECT_TRUE(submitted);
            if (!submitted) continue;
            // CPU側にも未書込みを残し、部分的な転送を成功扱いにしない。
            TArray<FVec4> values;
            values.SetNum(width * height);
            ::memcpy(values.GetData(), unwritten.GetData(), byteCount);
            // 読戻し不能を値の0や合格へ置き換えない。
            const bool read = device.Value()->ReadTexture(*outputTexture.Value(), values.GetData(), byteCount);
            EXPECT_TRUE(read);
            if (!read) continue;
            ++completed[variant];

            // 数値不正と解析値不一致を分け、最初の失敗だけ詳細を記録する。
            u32 invalidPixels = 0u;
            // 通常側の昼512画素が解析値に一致した件数。
            u32 matchedDayPixels = 0u;
            // 通常側の夜または地表なし対照で、厳密な0に一致した件数。
            u32 matchedZeroPixels = 0u;
            // 不一致の詳細を全画素で重複出力しない。
            bool reportedFailure = false;
            for (u32 pixel = 0u; pixel < width * height; ++pixel) {
                // 地表なし対照では昼夜とも0、それ以外は入力と同じ交互配置。
                const bool day = mutation == 0u && pixel % 2u == 0u;
                // 検査対象のRGBA。有限性は全画素・全成分で必須とする。
                const FVec4 value = values[pixel];
                // NaNが近似比較をすり抜けないよう、先に有限性とアルファを検査する。
                const bool valid = IsFiniteProbeValue_Internal(value.x) && IsFiniteProbeValue_Internal(value.y) && IsFiniteProbeValue_Internal(value.z) && IsFiniteProbeValue_Internal(value.w) && value.x >= 0.0f && value.y >= 0.0f && value.z >= 0.0f && value.w == 1.0f;
                // 昼は全色が解析値へ一致し、夜と対照は微小な漏れも認めない。
                const bool matches = valid && (day ? (::fabs(static_cast<f64>(value.x) - expectedDay) <= tolerance && ::fabs(static_cast<f64>(value.y) - expectedDay) <= tolerance && ::fabs(static_cast<f64>(value.z) - expectedDay) <= tolerance) : (value.x == 0.0f && value.y == 0.0f && value.z == 0.0f));
                if (!valid) ++invalidPixels;
                if (matches && day) ++matchedDayPixels;
                if (matches && !day) ++matchedZeroPixels;
                if (!matches && !reportedFailure) {
                    test::RecordInfo(FSourceLoc::Current(), "multi_boundary_pixel target=%s mutation=%u xy=(%u,%u) expected=%.12g rgba=(%.9g,%.9g,%.9g,%.9g)", targetName, mutation, pixel % width, pixel / width, day ? expectedDay : 0.0, value.x, value.y, value.z, value.w);
                    reportedFailure = true;
                }
            }
            test::RecordInfo(FSourceLoc::Current(), "multi_boundary_result target=%s mutation=%u submitted=1 readback=1 invalid_pixels=%u matched_day_pixels=%u matched_zero_pixels=%u", targetName, mutation, invalidPixels, matchedDayPixels, matchedZeroPixels);
            EXPECT_EQ(invalidPixels, 0u);
            EXPECT_EQ(matchedDayPixels, mutation == 0u ? width * height / 2u : 0u);
            EXPECT_EQ(matchedZeroPixels, mutation == 0u ? width * height / 2u : width * height);
        }
    }
    // 各形式で通常側と対照側の両方が提出・読戻しまで到達したことを要求する。
    for (u32 variant = 0u; variant < variantCount; ++variant) EXPECT_EQ(completed[variant], 2u);
}

#endif
