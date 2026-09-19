// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_TESTS_ATMOSPHERE_PLANET_SHADOW_INTEGRATION_TESTS_INL
#define ACS_TESTS_ATMOSPHERE_PLANET_SHADOW_INTEGRATION_TESTS_INL

// 現在の製品CSBake全体を実行し、夜の直射遮蔽、昼の散乱、遮蔽を外した対照を検査する。
// sky_ground_render_tests.cpp末尾から取り込み、既存の復元・有限値判定・RHIを共有する。
ACS_TEST(Atmosphere, WholeBakeEntryRejectsPlanetShadowLeak)
{
    // 共通部と入口を同じ製品スナップショットから復元する。
    const FString product = ReadRenderSource_Internal(L"../src/render/Atmosphere.cpp");
    EXPECT_TRUE(!product.IsEmpty());
    if (product.IsEmpty()) return;
    // 関数を抽出せず、共通部にkBakeCSの全本文をそのまま連結する。
    FString source = ReadAtmosphereCommonShader_Internal(product);
    // 製品の座標生成、積分、地表処理、出力を含む本文。
    const FString body = RestoreAtmosphereShaderSource_Internal(product, true, "kBakeCS");
    EXPECT_TRUE(!source.IsEmpty());
    EXPECT_TRUE(!body.IsEmpty());
    if (source.IsEmpty() || body.IsEmpty()) return;
    source.Append(body.View());

    // 積分点の直射参照だけを退行させる。一致なし・重複は試験失敗にする。
    constexpr const char* mutationNeedle = "float3 Tsun=SampleTrans(P,sd);";
    // 製品ファイルへ書かず、復元済みの全HLSL中で唯一の一致位置を探す。
    const char* mutationPoint = ::strstr(source.Data(), mutationNeedle);
    // 同じ本文に複数の置換候補があれば、勝手に対象を選ばない。
    const bool uniqueMutation = mutationPoint && !::strstr(mutationPoint + ::strlen(mutationNeedle), mutationNeedle);
    EXPECT_TRUE(uniqueMutation);
    if (!uniqueMutation) return;
    // 一時文字列だけの厳密な一か所置換。製品の入口名や他の処理は保持する。
    FString mutatedSource(FStringView(source.Data(), static_cast<usize>(mutationPoint - source.Data())));
    mutatedSource.Append("float3 Tsun=SampleTransUnoccluded(r,muSun);");
    mutatedSource.Append(mutationPoint + ::strlen(mutationNeedle));

    // 8x8の製品スレッド群二つで全画素を覆う。
    constexpr u32 width = 16u;
    // 偶数行なので地平線上の画素はなく、上半分64画素は確実に上半球。
    constexpr u32 height = 8u;
    // 半精度変換を挟まないRGBA32Fの読戻しサイズ。
    constexpr u32 byteCount = width * height * 4u * sizeof(f32);
    static_assert(sizeof(FVec4) == 4u * sizeof(f32), "RGBA32Fと定数バッファには連続したfloat4を使う");
    // 上半球の水平距離はsqrt(6460^2-6360^2)=約1132.25km以下で、全点が夜の地球影内。
    static_assert(6460.0 * 6460.0 - 6360.0 * 6360.0 < 6360.0 * 6360.0, "大気中の上向き視線は地球影の円柱内に収まる");
    // 下半球は地表から内向きで、現製品では最初のalt<0で積分を打ち切る。
    // 地表反射と多重散乱も0に固定するため、こちらも夜のRGBは厳密に0。

    // 透過率は製品と同寸法の既知定数。生成側のLUT積分誤差を持ち込まない。
    TArray<FVec4> transValues;
    transValues.SetNum(256u * 64u);
    // 全画素を正の透過率にし、表の暗い端によって遮蔽欠落が隠れることを防ぐ。
    for (FVec4& value : transValues) value = FVec4{0.25f,0.5f,0.75f,1.0f};
    // 多重散乱は寄与0。夜の非ゼロ値を直射漏れとして判定できる。
    TArray<FVec4> multiValues;
    multiValues.SetNum(32u * 32u);
    // アルファは参照表の有効画素を表す固定値。
    for (FVec4& value : multiValues) value = FVec4{0.0f,0.0f,0.0f,1.0f};
    // GPU出力とCPU読戻し先の両方に未書込みを残す。
    TArray<FVec4> unwritten;
    unwritten.SetNum(width * height);
    // 算術による生成を避けたIEEE 754のNaN。
    const f32 nan = ProbeFloatFromBits_Internal(0x7fc00000u);
    // 全RGBA成分を初期化し、一部分だけの書込みも失敗させる。
    for (FVec4& value : unwritten) value = FVec4{nan,nan,nan,nan};

    // デバイスを先に作り、所有する全資源より後に破棄する。
    FDeviceConfig configuration{};
    // GPUが使えない場合も試験を合格・省略にしない。
    auto device = CreateRhiDevice(configuration);
    EXPECT_TRUE(device.IsOk());
    if (device.IsErr()) return;
    test::RecordInfo(FSourceLoc::Current(), "whole_bake_gpu backend=%s adapter=%s width=%u height=%u", device.Value()->BackendName(), device.Value()->AdapterName(), width, height);
    // 製品のt0へ渡す透過率表。初期値のCPU配列も全実行まで保持する。
    FTextureDesc transDescription{};
    transDescription.width = 256u;
    transDescription.height = 64u;
    transDescription.format = EFormat::R32G32B32A32_Float;
    transDescription.initial_data = transValues.GetData();
    transDescription.initial_data_size = transValues.Num() * sizeof(FVec4);
    // 各シェーダー形式と昼夜で共用する読み取り専用の表。
    auto transTexture = CreateRhiTexture(*device.Value(), transDescription);
    EXPECT_TRUE(transTexture.IsOk());
    if (transTexture.IsErr()) return;
    // 製品のt1へ渡す多重散乱表。
    FTextureDesc multiDescription = transDescription;
    multiDescription.width = 32u;
    multiDescription.height = 32u;
    multiDescription.initial_data = multiValues.GetData();
    multiDescription.initial_data_size = multiValues.Num() * sizeof(FVec4);
    // 書込み先とは別資源なので、常に0の入力として参照できる。
    auto multiTexture = CreateRhiTexture(*device.Value(), multiDescription);
    EXPECT_TRUE(multiTexture.IsOk());
    if (multiTexture.IsErr()) return;

#if !WITH_RENDER_DILIGENT
    // RawDX12では旧形式と新形式の両方を使う。
    constexpr u32 variantCount = 2u;
#else
    // 他の描画基盤は既存形式だけを検証し、新形式の成功とは数えない。
    constexpr u32 variantCount = 1u;
#endif
    // 形式ごとの製品夜・製品昼・変異夜の完了数。未実行を成功として集計しない。
    u32 completed[variantCount]{};
    // 既定のSM5.1と明示指定のSM6をそれぞれコンパイル・実行する。
    for (u32 variant = 0u; variant < variantCount; ++variant) {
        // 記録する要求形式。既定側のtargetはnullptrのままにする。
        const char* targetName = variant == 0u ? "default(cs_5_1)" : "cs_6_0";
        // 製品HLSLと一か所だけ退行させたHLSLを、同じ入口・結合で比較する。
        for (u32 mutation = 0u; mutation < 2u; ++mutation) {
            // 一度の同期コンパイル結果を、製品側の昼夜で共有する。
            FShaderDesc shaderDescription{};
            shaderDescription.stage = EShaderStage::Compute;
            shaderDescription.hlsl_source = mutation == 0u ? source.Data() : mutatedSource.Data();
            shaderDescription.entry_point = "CSBake";
            shaderDescription.debug_name = mutation == 0u ? "Atmo.WholeBakePlanetShadow" : "Atmo.WholeBakePlanetShadowMutation";
            if (variant != 0u) shaderDescription.target = "cs_6_0";
            test::RecordInfo(FSourceLoc::Current(), "whole_bake_compile target=%s mutation=%u entry=CSBake", targetName, mutation);
            // コンパイル失敗は記録し、残りの形式・対照の検証を続ける。
            auto shader = CreateRhiShader(*device.Value(), shaderDescription);
            EXPECT_TRUE(shader.IsOk());
            if (shader.IsErr()) continue;
            // 製品と同じb0/t0/t1/u0とHLSL名で結び付ける。
            FComputePipelineDesc pipelineDescription{};
            pipelineDescription.cs = shader.Value().Get();
            pipelineDescription.cbuffer_slots = 1u;
            pipelineDescription.cbuffer_names[0] = "AtmoCB";
            pipelineDescription.srv_slots = 2u;
            pipelineDescription.srv_names[0] = "transLut";
            pipelineDescription.srv_names[1] = "multiLut";
            pipelineDescription.uav_slots = 1u;
            pipelineDescription.uav_names[0] = "bakeOut";
            // シェーダーより先に破棄される、今回の全入口用のパイプライン。
            auto pipeline = CreateRhiComputePipeline(*device.Value(), pipelineDescription);
            EXPECT_TRUE(pipeline.IsOk());
            if (pipeline.IsErr()) continue;
            // 製品は夜・昼、変異は夜だけを実行する。
            const u32 runCount = mutation == 0u ? 2u : 1u;
            // 各条件は別のNaN画像と定数バッファを使い、前の結果の流用を防ぐ。
            for (u32 run = 0u; run < runCount; ++run) {
                // 地表高度0、白色の単位入射、黒い地表。昼だけ太陽方向を反転する。
                const FVec4 constants[3] = {{0.0f,run == 0u ? -1.0f : 1.0f,0.0f,0.0f},{1.0f,1.0f,1.0f,0.0f},{0.0f,0.0f,0.0f,0.0f}};
                static_assert(sizeof(constants) == 48u, "製品AtmoCBのfloat4三個と一致させる");
                // 失敗と計測を形式・条件ごとに追える名称。
                const char* caseName = mutation != 0u ? "mutation_night" : (run == 0u ? "night" : "day");
                // フレームごとの定数領域は既存RHIのUpdateで更新する。
                FBufferDesc bufferDescription{};
                bufferDescription.size = sizeof(constants);
                bufferDescription.usage = EBufferUsage::Uniform;
                bufferDescription.cpu_writable = true;
                // この条件のコマンドリストより長く生存する定数バッファ。
                auto buffer = CreateRhiBuffer(*device.Value(), bufferDescription);
                EXPECT_TRUE(buffer.IsOk());
                if (buffer.IsErr()) continue;
                // 初期値も読戻しもRGBA32F。毎回NaNから始める。
                FTextureDesc outputDescription{};
                outputDescription.width = width;
                outputDescription.height = height;
                outputDescription.format = EFormat::R32G32B32A32_Float;
                outputDescription.is_uav = true;
                outputDescription.initial_data = unwritten.GetData();
                outputDescription.initial_data_size = byteCount;
                // 前条件の書込み済み画像を再利用しない。
                auto outputTexture = CreateRhiTexture(*device.Value(), outputDescription);
                EXPECT_TRUE(outputTexture.IsOk());
                if (outputTexture.IsErr()) continue;
                // 最後に作り、完了待ち後に参照先の資源より先に破棄する。
                auto command = CreateRhiCommandList(*device.Value());
                EXPECT_TRUE(command.IsOk());
                if (command.IsErr()) continue;
                buffer.Value()->Update(constants, sizeof(constants));
                command.Value()->Begin();
                command.Value()->SetComputePipeline(*pipeline.Value());
                command.Value()->SetConstantBuffer(0u, *buffer.Value());
                // SRV/UAVへの状態変更と書込みの順序付けは既存RHIが行う。
                command.Value()->SetTexture(0u, *transTexture.Value());
                command.Value()->SetTexture(1u, *multiTexture.Value());
                command.Value()->BindUav(0u, *outputTexture.Value());
                command.Value()->Dispatch(width / 8u, height / 8u, 1u);
                command.Value()->End();
                // CPUのSubmit直前からWaitIdle復帰直後まで。準備・読戻し時間は含めない。
                const f64 begin = CClock::SecondsSinceStartup();
                // 失敗しても待ってから資源を解放し、未確認の値を合否へ使わない。
                const bool submitted = command.Value()->Submit();
                device.Value()->WaitIdle();
                // CPU経過時間のみを記録し、GPU timestampやfpsとは扱わない。
                const f64 milliseconds = (CClock::SecondsSinceStartup() - begin) * 1000.0;
                test::RecordInfo(FSourceLoc::Current(), "whole_bake_submit target=%s case=%s submitted=%u cpu_submit_wait_ms=%.6f", targetName, caseName, submitted ? 1u : 0u, milliseconds);
                EXPECT_TRUE(submitted);
                if (!submitted) continue;
                // 読戻し先もNaNで埋め、部分的な転送を有限値として受け入れない。
                TArray<FVec4> values;
                values.SetNum(width * height);
                ::memcpy(values.GetData(), unwritten.GetData(), byteCount);
                // WaitIdle済みの画像を既存RHIの状態遷移付き読戻しで取得する。
                const bool read = device.Value()->ReadTexture(*outputTexture.Value(), values.GetData(), byteCount);
                EXPECT_TRUE(read);
                if (!read) continue;
                ++completed[variant];

                // 無効値・負値・アルファ違反の画素数。変異も同じ基礎条件を必須とする。
                u32 invalidPixels = 0u;
                // 夜側で厳密な0から外れた画素数。微小な漏れにも許容誤差を設けない。
                u32 nonzeroPixels = 0u;
                // 昼対照でRGBの全色が正でない上半球画素数。
                u32 unlitUpperPixels = 0u;
                // 変異の漏れは有限・非負の有効な上半球画素で検出する。
                u32 leakingUpperPixels = 0u;
                // 同じ違反を全画素分出力せず、最初の画素だけ詳細を残す。
                bool reportedFailure = false;
                // 条件ごとに全128画素のRGBAを調べる。
                for (u32 pixel = 0u; pixel < width * height; ++pixel) {
                    // 読み戻した一画素の値。
                    const FVec4 value = values[pixel];
                    // NaNを近似比較で見逃さず、アルファの書込みも確認する。
                    const bool valid = IsFiniteProbeValue_Internal(value.x) && IsFiniteProbeValue_Internal(value.y) && IsFiniteProbeValue_Internal(value.z) && IsFiniteProbeValue_Internal(value.w) && value.x >= 0.0f && value.y >= 0.0f && value.z >= 0.0f && value.w == 1.0f;
                    // 製品の画素中心とthetaの対応では前半の行が上半球。
                    const bool upper = pixel / width < height / 2u;
                    // alphaだけの正常書込みや黒一色の実装を昼対照で拒否する。
                    const bool positiveRgb = value.x > 0.0f && value.y > 0.0f && value.z > 0.0f;
                    // 遮蔽を省いたとき、一色でも漏れれば夜の期待値に違反する。
                    const bool nonzero = value.x != 0.0f || value.y != 0.0f || value.z != 0.0f;
                    if (!valid) ++invalidPixels;
                    if (nonzero) ++nonzeroPixels;
                    if (upper && (!valid || !positiveRgb)) ++unlitUpperPixels;
                    if (upper && valid && nonzero) ++leakingUpperPixels;
                    // 変異の正の漏れは期待する対照結果なので、試験失敗として記録しない。
                    const bool failure = !valid || (mutation == 0u && (run == 0u ? nonzero : (upper && !positiveRgb)));
                    if (failure && !reportedFailure) {
                        test::RecordInfo(FSourceLoc::Current(), "whole_bake_pixel target=%s case=%s xy=(%u,%u) rgba=(%.9g,%.9g,%.9g,%.9g)", targetName, caseName, pixel % width, pixel / width, value.x, value.y, value.z, value.w);
                        reportedFailure = true;
                    }
                }
                test::RecordInfo(FSourceLoc::Current(), "whole_bake_result target=%s case=%s submitted=1 readback=1 invalid_pixels=%u nonzero_rgb_pixels=%u unlit_upper_pixels=%u leaking_upper_pixels=%u", targetName, caseName, invalidPixels, nonzeroPixels, unlitUpperPixels, leakingUpperPixels);
                EXPECT_EQ(invalidPixels, 0u);
                if (mutation != 0u) EXPECT_TRUE(leakingUpperPixels > 0u);
                else if (run == 0u) EXPECT_EQ(nonzeroPixels, 0u);
                else EXPECT_EQ(unlitUpperPixels, 0u);
            }
        }
    }
    // 各対応形式で三つの提出と読戻しが完了していなければ失敗にする。
    for (u32 variant = 0u; variant < variantCount; ++variant) EXPECT_EQ(completed[variant], 3u);
}

#endif
