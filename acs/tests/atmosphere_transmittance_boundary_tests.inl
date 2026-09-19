// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_TESTS_ATMOSPHERE_TRANSMITTANCE_BOUNDARY_TESTS_INL
#define ACS_TESTS_ATMOSPHERE_TRANSMITTANCE_BOUNDARY_TESTS_INL

// 復元済み共通部の媒質本文だけを試験用にする。宣言の欠落・重複・形式不一致では入力を変更しない。
static bool ReplaceTransBoundaryMedium_Internal(FString& common, bool constantMedium)
{
    // 元の文字列を借用し、最後まで検査できた場合だけ置換結果へ所有権を移す。
    const char* cursor = common.Data();
    // コメントと改行を除いた直前の字句。戻り値の型まで確認する。
    FStringView previous;
    // 現在の字句。引用符内やコメント内の偽宣言は既存の字句処理で除かれる。
    FStringView token;
    // 関数の外側にある一意の宣言だけを対象とする。
    i32 depth = 0;
    // 媒質関数の開始波括弧の直後。まだ発見していない場合は0。
    usize begin = 0u;
    // 媒質関数の終了波括弧の位置。波括弧自体とその後の関数は保存する。
    usize end = 0u;
    // 引数宣言が変わった場合、古い引数名を使う試験を黙って継続しない。
    constexpr const char* signature[] = {"(","float","altKm",",","out","float3","sR",",","out","float","sM",",","out","float3","ext",")","{"};
    for (;;) {
        if (!ReadAtmosphereSourceToken_Internal(cursor, token)) return false;
        if (token.IsEmpty()) break;
        if (token == FStringView("\n")) continue;
        if (token == FStringView("#")) return false;
        if (depth == 0 && token == FStringView("SampleMedium")) {
            if (begin != 0u || previous != FStringView("void")) return false;
            for (usize index = 0u; index < sizeof(signature) / sizeof(signature[0]); ++index) {
                do {
                    if (!ReadAtmosphereSourceToken_Internal(cursor, token)) return false;
                } while (token == FStringView("\n"));
                if (token != FStringView(signature[index])) return false;
            }
            begin = static_cast<usize>(cursor - common.Data());
            depth = 1;
            while (depth != 0) {
                if (!ReadAtmosphereSourceToken_Internal(cursor, token) || token.IsEmpty() || token == FStringView("#")) return false;
                if (token == FStringView("{")) ++depth;
                if (token == FStringView("}")) --depth;
            }
            end = static_cast<usize>(token.Data() - common.Data());
            previous = {};
            continue;
        }
        if (token == FStringView("{")) ++depth;
        if (token == FStringView("}") && --depth < 0) return false;
        previous = token;
    }
    if (begin == 0u || end < begin || depth != 0) return false;
    // 試験媒質以外の文字列を一字も変えない。半径、逆写像、交差判定を製品から保持する。
    FString result(FStringView(common.Data(), begin));
    result.Append(constantMedium ? "\n  sR=float3(0.0,0.0,0.0); sM=0.0; ext=float3(0.0009765625,0.0009765625,0.0009765625);\n" : "\n  sR=float3(0.0,0.0,0.0); sM=0.0; ext=float3(0.0,0.0,0.0);\n");
    result.Append(common.Data() + end);
    common = Move(result);
    return true;
}

// 同じソースから共通部とCSTrans入口全体を復元する。媒質以外の改変や復元失敗の代用はしない。
static FString BuildTransBoundaryShader_Internal(const FString& product, bool constantMedium)
{
    // 条件分岐や重複宣言も検査する既存の復元処理を使う。
    FString common = ReadAtmosphereCommonShader_Internal(product);
    // UV生成を含む入口全体。検査用の座標入力や別の入口へ置き換えない。
    const FString body = RestoreAtmosphereShaderSource_Internal(product, true, "kTransCS");
    if (common.IsEmpty() || body.IsEmpty() || !ReplaceTransBoundaryMedium_Internal(common, constantMedium)) return {};
    common.Append(body.View());
    return common;
}

// 四隅の光路を球の幾何だけから与える。順番は地表鉛直・地表水平・上端外向き・上端地表接線。
// Hは直角三角形の辺: sqrt((6460-6360)*(6460+6360)) km。製品のUV式や交差関数を呼ばない。
static f64 TransBoundaryExpectedCorner_Internal(u32 corner, bool constantMedium)
{
    // 地表での水平距離H。上端から地表への接線は、往復するため2Hになる。
    constexpr f64 horizonDistance = 1132.2543883774529;
    // 外向きの上端には媒質がなく、光路長は厳密に0。
    constexpr f64 distances[4] = {100.0,horizonDistance,0.0,2.0 * horizonDistance};
    if (corner >= 4u) return -1.0;
    return constantMedium ? ::exp(-distances[corner] / 1024.0) : 1.0;
}

// 四隅を単純補間する誤実装と区別する固定点の解析値。半径の二乗と接線三角形を倍精度で別計算した距離を使う。
static f64 TransBoundaryExpectedInterior_Internal(u32 sample, bool constantMedium)
{
    // 画素(0,32),(127,32),(192,17),(64,47)の距離km。現シェーダーの逆写像やGPU出力から取得しない。
    constexpr f64 distances[] = {74.050160391686404,887.50636098179439,1105.4601300654017,529.2464222845008};
    if (sample >= 4u) return -1.0;
    return constantMedium ? ::exp(-distances[sample] / 1024.0) : 1.0;
}

// RGBA32Fの境界診断用の絶対許容差。本番RGBA16Fの丸め幅を含めない。
// 半径近傍の単精度刻み2^-11 kmを16個分と、64演算分の丸め、指数関数の4刻みを見込む。
// これは診断用の誤差予算であり、全コンパイラーの誤差保証ではない。逸脱時は原因を再調査する。
static constexpr f64 kTransBoundaryFloatTolerance = 1.0 / 65536.0;
static_assert(16.0 / (2048.0 * 1024.0) + 64.0 / (8388608.0 * 2.718281828459045) + 4.0 / 8388608.0 < kTransBoundaryFloatTolerance, "単精度診断の誤差予算を明示する");
static_assert(kTransBoundaryFloatTolerance < 1.0 / 4096.0, "半精度の半刻みを許容差へ混ぜない");
static_assert(kTransBoundaryFloatTolerance < 0.001 / 32.0, "旧中心座標の四隅のずれを丸め誤差として認めない");

// 一つの画素が同じ媒質の全経路契約を満たすか調べる。未書込み、零化、負値、透過率超過は失敗。
static bool TransBoundaryPixelValid_Internal(const FVec4& value, bool constantMedium)
{
    if (!IsFiniteProbeValue_Internal(value.x) || !IsFiniteProbeValue_Internal(value.y) || !IsFiniteProbeValue_Internal(value.z) || !IsFiniteProbeValue_Internal(value.w)) return false;
    if (value.w != 1.0f || value.x != value.y || value.x != value.z) return false;
    if (!constantMedium) return value.x == 1.0f;
    // 表内で最長なのは上端から地表へ接する2Hの光路。どの画素も惑星内部へ入らない。
    const f64 minimum = TransBoundaryExpectedCorner_Internal(3u, true) - kTransBoundaryFloatTolerance;
    return value.x >= minimum && value.x <= 1.0f;
}

// 有限性と全経路契約を先に確認する。NaNが近似比較をすり抜けることを防ぐ。
static bool TransBoundaryCornerMatches_Internal(const FVec4& value, u32 corner, bool constantMedium)
{
    if (corner >= 4u || !TransBoundaryPixelValid_Internal(value, constantMedium)) return false;
    return ::fabs(static_cast<f64>(value.x) - TransBoundaryExpectedCorner_Internal(corner, constantMedium)) <= kTransBoundaryFloatTolerance;
}

// 四隅以外の固定点も同じ許容差で検査する。点が範囲外、未書込み、非有限の場合は拒否する。
static bool TransBoundaryInteriorMatches_Internal(const FVec4& value, u32 sample, bool constantMedium)
{
    if (sample >= 4u || !TransBoundaryPixelValid_Internal(value, constantMedium)) return false;
    return ::fabs(static_cast<f64>(value.x) - TransBoundaryExpectedInterior_Internal(sample, constantMedium)) <= kTransBoundaryFloatTolerance;
}

// 媒質の置換は一意な関数本文だけに限定し、欠落・重複・壊れた宣言を拒否する。
ACS_TEST(Atmosphere, TransBoundaryRestorationPreservesWholeEntry)
{
    // コメント内の偽宣言と波括弧を無視し、実際の関数の範囲だけを抽出させる。
    constexpr const char* declaration = "void SampleMedium(float altKm, out float3 sR, out float sM, out float3 ext)";
    // 周辺を保持できたことも比較するための先頭部分。
    const FString prefix("// void SampleMedium() { fake }\nfloat before=7;\n");
    // 別関数の本文と名前は変更してはならない。
    const FString suffix("\nfloat After(){return 9;}\n");
    // 入れ子やコメントのある媒質本文も、一つの本文として扱う。
    FString common(prefix);
    common.Append(declaration);
    common.Append("{ if(altKm>0){sR=1;} /* } */ sM=2;ext=3; }");
    common.Append(suffix.View());
    for (u32 medium = 0u; medium < 2u; ++medium) {
        // 真空と一定消散が独立した同じ入力から作られることを検査する。
        FString replaced(common);
        EXPECT_TRUE(ReplaceTransBoundaryMedium_Internal(replaced, medium != 0u));
        // 媒質以外の全文が一致することを、置換結果の前後も含めて検査する。
        FString expected(prefix);
        expected.Append(declaration);
        expected.Append(medium == 0u ? "{\n  sR=float3(0.0,0.0,0.0); sM=0.0; ext=float3(0.0,0.0,0.0);\n}" : "{\n  sR=float3(0.0,0.0,0.0); sM=0.0; ext=float3(0.0009765625,0.0009765625,0.0009765625);\n}");
        expected.Append(suffix.View());
        EXPECT_TRUE(replaced == expected.View());
    }
    // 欠落、宣言のみ、型変更、未終端、条件付き宣言、未終端コメントを含む拒否例。
    const char* invalid[] = {"// void SampleMedium() {}\n", "void SampleMedium(float altKm, out float3 sR, out float sM, out float3 ext);", "float SampleMedium(float altKm, out float3 sR, out float sM, out float3 ext){}", "void SampleMedium(float altKm, out float3 sR, out float sM, out float3 ext){", "#if 1\nvoid SampleMedium(float altKm, out float3 sR, out float sM, out float3 ext){}\n#endif", "void SampleMedium(float altKm, out float3 sR, out float sM, out float3 ext){} /*"};
    for (const char* invalidSource : invalid) {
        // 拒否した入力は部分的にも変更しない。
        FString rejected(invalidSource);
        EXPECT_FALSE(ReplaceTransBoundaryMedium_Internal(rejected, true));
        EXPECT_TRUE(rejected == FStringView(invalidSource));
    }
    // 同名関数を二つ持つ入力を、一方だけ置換して採用してはならない。
    FString duplicate(common);
    duplicate.Append(common.View());
    // 拒否後の比較用に、元の文字列を所有する。
    const FString duplicateOriginal(duplicate);
    EXPECT_FALSE(ReplaceTransBoundaryMedium_Internal(duplicate, false));
    EXPECT_TRUE(duplicate == duplicateOriginal.View());

    // 一回の読取りから、同じ製品の共通部と入口を復元する。
    const FString product = ReadRenderSource_Internal(L"../src/render/Atmosphere.cpp");
    // 比較用にも既存の厳密な復元処理を使い、入口に試験の式を混ぜない。
    const FString body = RestoreAtmosphereShaderSource_Internal(product, true, "kTransCS");
    EXPECT_TRUE(!body.IsEmpty());
    if (body.IsEmpty()) return;
    for (u32 medium = 0u; medium < 2u; ++medium) {
        // 完成したシェーダーの末尾は、復元したCSTrans本文全体と完全一致しなければならない。
        const FString source = BuildTransBoundaryShader_Internal(product, medium != 0u);
        EXPECT_TRUE(source.Size() >= body.Size());
        if (source.Size() < body.Size()) continue;
        EXPECT_TRUE(FStringView(source.Data() + source.Size() - body.Size(), body.Size()) == body.View());
    }
    EXPECT_TRUE(BuildTransBoundaryShader_Internal(FString{}, false).IsEmpty());
}

// 解析値の判定自体が定数画像や未書込みを受け入れないことを、GPUとは独立に確認する。
ACS_TEST(Atmosphere, TransBoundaryOracleRejectsConstantAndInvalidPixels)
{
    // 幾何による経路長とBeerの法則を別計算した固定期待値。製品のUV・交差式から作らない。
    constexpr f64 expected[4] = {0.9069606178873836,0.3309734308540778,1.0,0.10954341193131906};
    for (u32 corner = 0u; corner < 4u; ++corner) {
        EXPECT_TRUE(::fabs(TransBoundaryExpectedCorner_Internal(corner, true) - expected[corner]) < 1.0e-14);
        // 出力と同じ単精度へ丸めても、独立した解析値判定を通ることを確認する。
        const f32 scalar = static_cast<f32>(expected[corner]);
        EXPECT_TRUE(TransBoundaryCornerMatches_Internal(FVec4{scalar,scalar,scalar,1.0f}, corner, true));
    }
    // 全体を一つの解析値で塗る四つの候補と、黒一色。どれも四隅すべてには一致しない。
    constexpr f32 constants[5] = {0.0f,1.0f,0.9069606f,0.3309734f,0.1095434f};
    for (f32 scalar : constants) {
        // 同じ値を全画素へ書くごまかしを受け入れる隅の数。
        u32 matches = 0u;
        for (u32 corner = 0u; corner < 4u; ++corner) if (TransBoundaryCornerMatches_Internal(FVec4{scalar,scalar,scalar,1.0f}, corner, true)) ++matches;
        EXPECT_TRUE(matches < 4u);
    }
    // 中間の四点も固定解析値と照合し、四隅だけ合う距離場を合格にしない。
    constexpr f64 interiorExpected[] = {0.93023818775854161,0.42033409037075969,0.33974806943858671,0.59640088674568115};
    for (u32 sample = 0u; sample < 4u; ++sample) {
        EXPECT_TRUE(::fabs(TransBoundaryExpectedInterior_Internal(sample, true) - interiorExpected[sample]) < 1.0e-14);
        // 正しく丸めた有限の画素は受け入れる。
        const f32 scalar = static_cast<f32>(interiorExpected[sample]);
        EXPECT_TRUE(TransBoundaryInteriorMatches_Internal(FVec4{scalar,scalar,scalar,1}, sample, true));
    }
    // 左下100kmと左上0kmの距離を行番号で線形補間する誤り。値域には入るが(0,32)の幾何とは違う。
    const f32 bilinearOnly = static_cast<f32>(::exp(-(100.0 * 31.0 / 63.0) / 1024.0));
    EXPECT_TRUE(TransBoundaryPixelValid_Internal(FVec4{bilinearOnly,bilinearOnly,bilinearOnly,1}, true));
    EXPECT_FALSE(TransBoundaryInteriorMatches_Internal(FVec4{bilinearOnly,bilinearOnly,bilinearOnly,1}, 0u, true));
    // NaN・正負の無限大はRGBAのどの成分でも拒否する。
    constexpr u32 invalidBits[3] = {0x7fc00000u,0x7f800000u,0xff800000u};
    for (u32 bits : invalidBits) {
        for (u32 channel = 0u; channel < 4u; ++channel) {
            // 上端外向きは両媒質で1なので、同じ正しい画素から一成分ずつ壊す。
            FVec4 value{1.0f,1.0f,1.0f,1.0f};
            // 数値変換を避け、既存補助処理で不正値を構成する。
            const f32 invalid = ProbeFloatFromBits_Internal(bits);
            if (channel == 0u) value.x = invalid;
            else if (channel == 1u) value.y = invalid;
            else if (channel == 2u) value.z = invalid;
            else value.w = invalid;
            EXPECT_FALSE(TransBoundaryCornerMatches_Internal(value, 2u, false));
            EXPECT_FALSE(TransBoundaryCornerMatches_Internal(value, 2u, true));
        }
    }
    EXPECT_FALSE(TransBoundaryPixelValid_Internal(FVec4{0,0,0,1}, true));
    EXPECT_FALSE(TransBoundaryPixelValid_Internal(FVec4{-1,-1,-1,1}, true));
    EXPECT_FALSE(TransBoundaryPixelValid_Internal(FVec4{1.001f,1.001f,1.001f,1}, true));
    EXPECT_FALSE(TransBoundaryPixelValid_Internal(FVec4{1,1,1,0}, true));
    EXPECT_FALSE(TransBoundaryCornerMatches_Internal(FVec4{1,1,1,1}, 4u, true));
}

// 製品CSTrans全体で256x64を実行する先行試験。親が共通補助処理の後へ取り込み、旧製品の失敗を確認する。
// RGBA32F診断だけを行う。本番RGBA16Fの量子化・配布物・参照側までの受け入れ試験とは分離する。
ACS_TEST(Atmosphere, WholeTransEntryKeepsUnoccludedBoundaryTransmittance)
{
    // 一度の読取りを全形式・両媒質で共有し、並行編集前後の入口と共通部を混ぜない。
    const FString product = ReadRenderSource_Internal(L"../src/render/Atmosphere.cpp");
    // 真空と一定消散を同じ入口から作り、出力の定数化では両方に合格できないようにする。
    const FString sources[2] = {BuildTransBoundaryShader_Internal(product, false),BuildTransBoundaryShader_Internal(product, true)};
    EXPECT_TRUE(!sources[0].IsEmpty());
    EXPECT_TRUE(!sources[1].IsEmpty());
    if (sources[0].IsEmpty() || sources[1].IsEmpty()) return;
    // 製品の固定寸法と同じ幅。入口内の上限判定も変更しない。
    constexpr u32 width = 256u;
    // 全高度の行。接線を含む右端列も省略しない。
    constexpr u32 height = 64u;
    // 初期化、提出、読戻し、全画素検査が共有する要素数。
    constexpr u32 pixelCount = width * height;
    // 半精度変換を挟まない全RGBA32Fの転送量。
    constexpr u32 byteCount = pixelCount * sizeof(FVec4);
    static_assert(sizeof(FVec4) == 4u * sizeof(f32), "読戻しは連続したRGBA32Fを使う");
    // 四隅の番地。期待値の計算に製品のUVや逆写像を使わない。
    constexpr u32 cornerPixels[4] = {0u,width - 1u,(height - 1u) * width,pixelCount - 1u};
    // 高さの非線形な配置を調べる固定点。四隅だけの線形距離場を拒否する。
    constexpr u32 interiorPixels[4] = {32u * width,32u * width + 127u,17u * width + 192u,47u * width + 64u};
    // GPU画像とCPU読戻し先の両方に置く未書込みの印。
    const f32 nan = ProbeFloatFromBits_Internal(0x7fc00000u);
    // 各提出で独立した出力画像を作るための初期値。
    TArray<FVec4> unwritten;
    unwritten.SetNum(pixelCount);
    for (FVec4& value : unwritten) value = FVec4{nan,nan,nan,nan};
    // 全資源より先にデバイスを作り、全提出と資源の破棄が終わるまで保持する。
    FDeviceConfig configuration{};
    // GPUやシェーダー作成の失敗を試験省略として扱わない。
    auto device = CreateRhiDevice(configuration);
    EXPECT_TRUE(device.IsOk());
    if (device.IsErr()) return;
    test::RecordInfo(FSourceLoc::Current(), "trans_boundary_gpu backend=%s adapter=%s format=RGBA32F width=%u height=%u tolerance=%.12g", device.Value()->BackendName(), device.Value()->AdapterName(), width, height, kTransBoundaryFloatTolerance);
#if !WITH_RENDER_DILIGENT
    // RawDX12ではSM5.1とSM6を明示指定し、両方の提出と読戻しを必須にする。
    constexpr u32 variantCount = 2u;
#else
    // 既存のDiligent経路はSM5.1だけ。未実行のSM6を合格として数えない。
    constexpr u32 variantCount = 1u;
#endif
    // 途中の失敗をcontinueで隠さないための形式別の完了数。
    u32 completed[variantCount]{};
    for (u32 variant = 0u; variant < variantCount; ++variant) {
        // 明示した形式を診断ログにも残す。
        const char* target = variant == 0u ? "cs_5_1" : "cs_6_0";
        for (u32 medium = 0u; medium < 2u; ++medium) {
            // 試験媒質は真空か、単精度でも正確な1/1024 km^-1の一定消散。
            const bool constantMedium = medium != 0u;
            // 入口名と出力資源名は製品そのものを使う。
            FShaderDesc shaderDescription{};
            shaderDescription.stage = EShaderStage::Compute;
            shaderDescription.hlsl_source = sources[medium].Data();
            shaderDescription.entry_point = "CSTrans";
            shaderDescription.target = target;
            shaderDescription.debug_name = constantMedium ? "Atmo.TransBoundaryConstant32" : "Atmo.TransBoundaryVacuum32";
            test::RecordInfo(FSourceLoc::Current(), "trans_boundary_compile target=%s medium=%s entry=CSTrans", target, constantMedium ? "constant" : "vacuum");
            // 一方で失敗しても他方の形式と媒質の診断を続ける。
            auto shader = CreateRhiShader(*device.Value(), shaderDescription);
            EXPECT_TRUE(shader.IsOk());
            if (shader.IsErr()) continue;
            // CSTransは出力u0だけを持つ。検査用の入力や追加の入口を設けない。
            FComputePipelineDesc pipelineDescription{};
            pipelineDescription.cs = shader.Value().Get();
            pipelineDescription.uav_slots = 1u;
            pipelineDescription.uav_names[0] = "transOut";
            // シェーダーより後に作り、それより先に破棄する。
            auto pipeline = CreateRhiComputePipeline(*device.Value(), pipelineDescription);
            EXPECT_TRUE(pipeline.IsOk());
            if (pipeline.IsErr()) continue;
            // 別の形式・媒質で書いた値を流用しない、新規のNaN画像。
            FTextureDesc outputDescription{};
            outputDescription.width = width;
            outputDescription.height = height;
            outputDescription.format = EFormat::R32G32B32A32_Float;
            outputDescription.is_uav = true;
            outputDescription.initial_data = unwritten.GetData();
            outputDescription.initial_data_size = byteCount;
            // 初期値の所有配列は提出完了まで保持する。
            auto outputTexture = CreateRhiTexture(*device.Value(), outputDescription);
            EXPECT_TRUE(outputTexture.IsOk());
            if (outputTexture.IsErr()) continue;
            // コマンドは参照する全資源より後に作る。
            auto command = CreateRhiCommandList(*device.Value());
            EXPECT_TRUE(command.IsOk());
            if (command.IsErr()) continue;
            command.Value()->Begin();
            command.Value()->SetComputePipeline(*pipeline.Value());
            command.Value()->BindUav(0u, *outputTexture.Value());
            command.Value()->Dispatch(width / 8u, height / 8u, 1u);
            command.Value()->End();
            // 提出失敗の場合も完了待ちを経てから資源を解放する。
            const bool submitted = command.Value()->Submit();
            device.Value()->WaitIdle();
            EXPECT_TRUE(submitted);
            if (!submitted) continue;
            // CPU側にもNaNを残し、部分転送を成功扱いにしない。
            TArray<FVec4> values;
            values.SetNum(pixelCount);
            ::memcpy(values.GetData(), unwritten.GetData(), byteCount);
            // 失敗時に零や前回値を返す代用処理を設けない。
            const bool read = device.Value()->ReadTexture(*outputTexture.Value(), values.GetData(), byteCount);
            EXPECT_TRUE(read);
            if (!read) continue;
            ++completed[variant];
            // 全画素の未書込み・非有限・範囲外・アルファ不正を集計する。
            u32 invalidPixels = 0u;
            // 接線列が零化する問題を、他の画素とは別に診断する。
            u32 invalidTangentPixels = 0u;
            // 大量の同じ失敗を表示しないため、全体の最初の不正値だけを詳報する。
            bool reportedInvalid = false;
            for (u32 pixel = 0u; pixel < pixelCount; ++pixel) {
                // 四隅以外にも非遮蔽経路の正の透過率と有限性を要求する。
                const FVec4 value = values[pixel];
                if (TransBoundaryPixelValid_Internal(value, constantMedium)) continue;
                ++invalidPixels;
                if (pixel % width == width - 1u) ++invalidTangentPixels;
                if (!reportedInvalid) {
                    test::RecordInfo(FSourceLoc::Current(), "trans_boundary_invalid target=%s medium=%s xy=(%u,%u) rgba=(%.9g,%.9g,%.9g,%.9g)", target, constantMedium ? "constant" : "vacuum", pixel % width, pixel / width, value.x, value.y, value.z, value.w);
                    reportedInvalid = true;
                }
            }
            // 座標生成が違う場合、有限で範囲内の値でも四隅の解析値には一致しない。
            u32 matchedCorners = 0u;
            for (u32 corner = 0u; corner < 4u; ++corner) {
                // 角ごとの失敗は別々に記録し、高度軸と角度軸の問題を切り分ける。
                const FVec4 value = values[cornerPixels[corner]];
                if (TransBoundaryCornerMatches_Internal(value, corner, constantMedium)) {
                    ++matchedCorners;
                } else {
                    test::RecordInfo(FSourceLoc::Current(), "trans_boundary_corner target=%s medium=%s corner=%u xy=(%u,%u) expected=%.12g rgba=(%.9g,%.9g,%.9g,%.9g)", target, constantMedium ? "constant" : "vacuum", corner, cornerPixels[corner] % width, cornerPixels[corner] / width, TransBoundaryExpectedCorner_Internal(corner, constantMedium), value.x, value.y, value.z, value.w);
                }
            }
            // 四隅以外の非対称な固定点も、実入口の出力と独立解析値を比較する。
            u32 matchedInteriors = 0u;
            for (u32 sample = 0u; sample < 4u; ++sample) {
                // 値域が正常でも、誤った距離に対応する透過率なら失敗にする。
                const FVec4 value = values[interiorPixels[sample]];
                if (TransBoundaryInteriorMatches_Internal(value, sample, constantMedium)) ++matchedInteriors;
                else test::RecordInfo(FSourceLoc::Current(), "trans_boundary_interior target=%s medium=%s sample=%u xy=(%u,%u) expected=%.12g rgba=(%.9g,%.9g,%.9g,%.9g)", target, constantMedium ? "constant" : "vacuum", sample, interiorPixels[sample] % width, interiorPixels[sample] / width, TransBoundaryExpectedInterior_Internal(sample, constantMedium), value.x, value.y, value.z, value.w);
            }
            test::RecordInfo(FSourceLoc::Current(), "trans_boundary_result target=%s medium=%s format=RGBA32F submitted=1 readback=1 checked_pixels=%u invalid_pixels=%u invalid_tangent_pixels=%u matched_corners=%u matched_interiors=%u", target, constantMedium ? "constant" : "vacuum", pixelCount, invalidPixels, invalidTangentPixels, matchedCorners, matchedInteriors);
            EXPECT_EQ(invalidPixels, 0u);
            EXPECT_EQ(invalidTangentPixels, 0u);
            EXPECT_EQ(matchedCorners, 4u);
            EXPECT_EQ(matchedInteriors, 4u);
        }
    }
    // 各形式で真空と一定消散の両方が提出・読戻しまで到達したことを要求する。
    for (u32 variant = 0u; variant < variantCount; ++variant) EXPECT_EQ(completed[variant], 2u);
}

#endif
