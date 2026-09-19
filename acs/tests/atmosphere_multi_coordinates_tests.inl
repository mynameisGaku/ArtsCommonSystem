// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_TESTS_ATMOSPHERE_MULTI_COORDINATES_TESTS_INL
#define ACS_TESTS_ATMOSPHERE_MULTI_COORDINATES_TESTS_INL

// 改行とコメントを除く一字句を読む。未終端や入力の終端では失敗する。
static bool ReadMultiCoordinateToken_Internal(const char*& cursor, FStringView& token)
{
    do {
        if (!ReadAtmosphereSourceToken_Internal(cursor, token) || token.IsEmpty()) return false;
    } while (token == FStringView("\n"));
    return true;
}

// 復元済み本文から唯一のCSMulti定義を取り出す。宣言だけ、重複、未終端は拒否する。
static FString ExtractMultiCoordinateEntry_Internal(const FString& body)
{
    // 戻り値と入口名を字句で照合し、コメント内や似た名前を採用しない。
    constexpr const char* signature[] = {"void", "CSMulti", "("};
    // 元の文字列を借用する読み取り位置と定義の先頭。
    const char* cursor = body.Data();
    const char* begin = nullptr;
    // 宣言の一致数と、関数の波括弧の深さ。
    u32 matched = 0u;
    u32 depth = 0u;
    // 二重定義を検出するため、一つ抽出した後も末尾まで調べる。
    FString result;
    // 現在の字句。引用符内は既存の厳密な読み取りが一単位として扱う。
    FStringView token;
    while (ReadAtmosphereSourceToken_Internal(cursor, token) && !token.IsEmpty()) {
        if (token == FStringView("#")) return {};
        if (matched < 3u) {
            if (token == FStringView("\n")) continue;
            if (token == FStringView(signature[matched])) {
                if (matched == 0u) begin = cursor - token.Size();
                if (++matched == 3u && !result.IsEmpty()) return {};
            } else {
                matched = token == FStringView("void") ? 1u : 0u;
                if (matched == 1u) begin = cursor - token.Size();
            }
            continue;
        }
        if (token == FStringView("{")) ++depth;
        else if (token == FStringView("}")) {
            if (depth == 0u) return {};
            if (--depth == 0u) {
                result = FString(FStringView(begin, static_cast<usize>(cursor - begin)));
                matched = 0u;
            }
        } else if (depth == 0u && token == FStringView(";")) return {};
    }
    return matched == 0u && depth == 0u && token.IsEmpty() ? result : FString{};
}

// 指定した変数の初期化文を一つ読む。式は評価も置換もせず、完全な元の文字範囲を返す。
static FString ReadMultiCoordinateDeclaration_Internal(const char*& cursor, const char* type, const char* name)
{
    // 先頭の型までの空白とコメントは、文の抽出範囲に含めなくてよい。
    FStringView token;
    if (!ReadMultiCoordinateToken_Internal(cursor, token) || token != FStringView(type)) return {};
    // 参照中の元文字列は抽出完了まで呼び出し元が保持する。
    const char* begin = cursor - token.Size();
    if (!ReadMultiCoordinateToken_Internal(cursor, token) || token != FStringView(name)) return {};
    if (!ReadMultiCoordinateToken_Internal(cursor, token) || token != FStringView("=")) return {};
    // 複数文や制御構造の混入を拒否し、式中の括弧も最後まで確認する。
    i32 parentheses = 0;
    i32 brackets = 0;
    bool expression = false;
    while (ReadMultiCoordinateToken_Internal(cursor, token)) {
        if (token == FStringView(";") && parentheses == 0 && brackets == 0) return expression ? FString(FStringView(begin, static_cast<usize>(cursor - begin))) : FString{};
        if (token == FStringView(";") || token == FStringView("{") || token == FStringView("}") || token == FStringView("#") || token[0] == '"' || token[0] == '\'') return {};
        if (token == FStringView("(")) ++parentheses;
        if (token == FStringView(")") && --parentheses < 0) return {};
        if (token == FStringView("[")) ++brackets;
        if (token == FStringView("]") && --brackets < 0) return {};
        expression = true;
    }
    return {};
}

// CSMulti直下にあるuv、cosSun、rの連続した三文だけを抽出する。生成式の固定コピーは持たない。
static FString ExtractMultiCoordinateStatements_Internal(const FString& body)
{
    // C++の厳密復元後も、別関数内の同名変数を生成入口の変数と誤認しない。
    const FString entry = ExtractMultiCoordinateEntry_Internal(body);
    if (entry.IsEmpty()) return {};
    // 抽出した入口だけを読み進める位置。
    const char* cursor = entry.Data();
    // 波括弧の深さ1だけを調べ、条件分岐内の断片を単独では実行しない。
    u32 depth = 0u;
    // 完全な三文を見つけた後も、同じ組の重複を拒否する。
    FString result;
    // コメントを除いた現在の字句。
    FStringView token;
    while (ReadAtmosphereSourceToken_Internal(cursor, token) && !token.IsEmpty()) {
        if (token == FStringView("{")) ++depth;
        else if (token == FStringView("}")) {
            if (depth == 0u) return {};
            --depth;
        } else if (depth == 1u && token == FStringView("float2")) {
            // 名前を確認してから局所カーソルで三文を読み、途中だけを採用しない。
            const char* probe = cursor;
            // 型に続く変数名。別のfloat2変数は対象にしない。
            FStringView name;
            if (!ReadMultiCoordinateToken_Internal(probe, name)) return {};
            if (name != FStringView("uv")) continue;
            if (!result.IsEmpty()) return {};
            probe = cursor - token.Size();
            // 型と名前は契約、右辺は常に現在の製品の式である。
            const FString uv = ReadMultiCoordinateDeclaration_Internal(probe, "float2", "uv");
            const FString cosine = ReadMultiCoordinateDeclaration_Internal(probe, "float", "cosSun");
            const FString radius = ReadMultiCoordinateDeclaration_Internal(probe, "float", "r");
            if (uv.IsEmpty() || cosine.IsEmpty() || radius.IsEmpty()) return {};
            result.Append(uv.View());
            result.Append('\n');
            result.Append(cosine.View());
            result.Append('\n');
            result.Append(radius.View());
            result.Append('\n');
            cursor = probe;
        }
    }
    return depth == 0u && token.IsEmpty() ? result : FString{};
}

// 多重散乱表の物理座標を生成する実三文と、試験専用の線形場だけをGPU上で実行する。
static FString BuildMultiCoordinateGeneration_Internal(const FString& product, u32 field)
{
    // 定数も座標式も、一度読んだ同じ製品スナップショットを使う。
    FString result = ReadAtmosphereCommonShader_Internal(product);
    // C++文字列を厳密復元した現在の生成入口。
    const FString body = RestoreAtmosphereShaderSource_Internal(product, true, "kMultiCS");
    // 型・名前を検証し、右辺を一切変更していない三つの初期化文。
    const FString coordinates = ExtractMultiCoordinateStatements_Internal(body);
    if (result.IsEmpty() || coordinates.IsEmpty() || field > 5u) return {};
    result.Append("\nRWTexture2D<float4> coordinateTable : register(u0);\n[numthreads(8,8,1)] void CSCoordinateGeneration(uint3 id : SV_DispatchThreadID){\n  const uint W=32,H=32; if(id.x>=W||id.y>=H) return;\n");
    if (field == 3u) {
        // 旧中心配置への退行は試験内だけで行う。cosSunとrは現在の製品の二文を保持する。
        const char* remaining = coordinates.Data();
        // 唯一のuv初期化文の末尾まで進み、後続二文の範囲を確定する。
        const FString uv = ReadMultiCoordinateDeclaration_Internal(remaining, "float2", "uv");
        if (uv.IsEmpty()) return {};
        result.Append("float2 uv=(float2(id.xy)+0.5)/float2(W,H);\n");
        result.Append(remaining);
    } else if (field == 4u || field == 5u) {
        // 太陽側は現在の二文を保持し、半径だけを不完全修正へ差し替える。
        const char* remaining = coordinates.Data();
        // 正常なuvとcosSunは製品と同じ文字範囲のまま使う。
        const FString uv = ReadMultiCoordinateDeclaration_Internal(remaining, "float2", "uv");
        const FString cosine = ReadMultiCoordinateDeclaration_Internal(remaining, "float", "cosSun");
        // 元の半径文も完全に読めなければ、部分抽出した対照を作らない。
        const FString radius = ReadMultiCoordinateDeclaration_Internal(remaining, "float", "r");
        if (uv.IsEmpty() || cosine.IsEmpty() || radius.IsEmpty()) return {};
        result.Append(uv.View());
        result.Append('\n');
        result.Append(cosine.View());
        result.Append('\n');
        if (field == 4u) result.Append("float r=kBottom+((float(id.y)+0.5)/float(H))*(kTop-kBottom);\n");
        else result.Append("float r=kBottom;\n");
    } else result.Append(coordinates.View());
    // xは高度km、yは太陽天頂角の余弦。zにも両軸を異なる係数で入れる。
    if (field != 1u && field != 2u) result.Append("  float h=r-kBottom; float c=cosSun;\n");
    // 定数化しても合格しないことを実GPUで調べる。値は全て二進数で厳密に表現できる。
    else if (field == 1u) result.Append("  float h=13.0; float c=0.125;\n");
    // 軸交換の対照。二つの物理量を、それぞれの値域へ戻してから格納する。
    else result.Append("  float h=(cosSun+1.0)*50.0; float c=(r-kBottom)*0.02-1.0;\n");
    // wには生成した生の半径kmを保持する。実SampleMultiが読むrgbと、半径の直接検査を分ける。
    result.Append("  coordinateTable[id.xy]=float4(h,c,h*0.25+c*8.0+3.0,r);\n}\n");
    return result;
}

// 空画像または空気遠近法の実SampleMultiを保持し、生成済みGPU画像を物理点から参照する。
static FString BuildMultiCoordinateLookup_Internal(const FString& product, const char* consumer)
{
    // 空気遠近法だけは、生文字列と別関数の補間を使う。
    const bool aerial = ::strcmp(consumer, "kApCS") == 0;
    if (!aerial && ::strcmp(consumer, "kBakeCS") != 0) return {};
    // 共通定数を手書きし直さず、同一スナップショットから使う。
    FString result = ReadAtmosphereCommonShader_Internal(product);
    // 選択した実利用者の本文全体。
    const FString body = RestoreAtmosphereShaderSource_Internal(product, true, consumer, aerial);
    // 半径と余弦から多重散乱表を引く実関数。
    const FString lookup = ExtractAtmosphereLookup_Internal(body, "SampleMulti");
    if (result.IsEmpty() || body.IsEmpty() || lookup.IsEmpty()) return {};
    // 多重散乱表のt1は製品と同じ。t0だけを試験用の物理点入力へ使う。
    result.Append("\nTexture2D<float4> coordinateInput : register(t0);\nTexture2D<float4> multiLut : register(t1);\nRWTexture2D<float4> coordinateOutput : register(u0);\n");
    if (aerial) {
        // 補間式は試験へ複製せず、現在の実関数を一字も変えず抽出する。
        const FString load = ExtractAtmosphereLookup_Internal(body, "LoadMultiBilinear");
        if (load.IsEmpty()) return {};
        result.Append(load.View());
        result.Append('\n');
    }
    result.Append(lookup.View());
    result.Append("\n[numthreads(8,8,1)] void CSCoordinateLookup(uint3 id : SV_DispatchThreadID){ uint W,H; coordinateOutput.GetDimensions(W,H); if(id.x>=W||id.y>=H) return; float2 q=coordinateInput.Load(int3(id.xy,0)).xy; coordinateOutput[id.xy]=float4(SampleMulti(q.x,q.y),1.0); }\n");
    return result;
}

// 物理点の線形場を倍精度で直接評価する。画素座標、生成UV、製品の補間式は使わない。
static void MultiCoordinateExpected_Internal(const FVec4& input, u32 field, f64 (&expected)[3])
{
    // GPUへ渡したf32半径そのものを基準にし、入力の丸めを生成側の誤差へ混ぜない。
    const f64 height = static_cast<f64>(input.x) - 6360.0;
    const f64 cosine = static_cast<f64>(input.y);
    expected[0] = field == 1u ? 13.0 : (field == 2u ? (cosine + 1.0) * 50.0 : height);
    expected[1] = field == 1u ? 0.125 : (field == 2u ? height / 50.0 - 1.0 : cosine);
    if (field == 3u || field == 4u) {
        // 旧配置の独立な閉形式: 対象の軸が中心へ31/32倍に縮む。これは退行対照だけの期待値。
        // 重みや画素の選択式を複製せず、線形場の端点が1/64ずつ欠けることから導く。
        expected[0] = 50.0 + (height - 50.0) * (31.0 / 32.0);
        // 高度だけ戻す不完全修正では太陽余弦を縮めない。
        if (field == 3u) expected[1] = cosine * (31.0 / 32.0);
    }
    // 全行地表固定の対照も、太陽側を正常に保つ。
    if (field == 5u) expected[0] = 0.0;
    // zはkm単位。高度の1/4、余弦に8kmを掛けた値、3kmの和である。
    expected[2] = expected[0] / 4.0 + expected[1] * 8.0 + 3.0;
}

// 半径6360～6460kmでのf32間隔は2^-11km。rの丸めが最大2^-12km残り、r-kBottomも最適化時には元の積へ戻り得る。
// 生成の正規化・積・参照座標・二段補間には、単位区間のf32間隔2^-23を32回分、100kmへ換算して見積もる。
// 32*2^-23*100km < 2^-11kmを追加丸めへ配分し、半径丸めとの合計より大きい2^-10kmを事前上限にする。
// これは1格子幅100/31kmの約1/3300であり、半画素の位置ずれを吸収しない。超過時に許容値を後付け変更しない。
static constexpr f64 kMultiCoordinateHeightTolerance = 1.0 / 1024.0;
// 余弦は無次元。軸交換対照の高度誤差を1/50に換算した2^-10/50と、追加演算用2^-18の和より大きい2^-15。
static constexpr f64 kMultiCoordinateCosineTolerance = 1.0 / 32768.0;
// zの配分は高度誤差/4 + 8*余弦誤差 + 追加演算用2^-12km < 2^-10km。
static constexpr f64 kMultiCoordinateMixedTolerance = 1.0 / 1024.0;
static_assert(1.0 / 4096.0 + 32.0 * 100.0 / 8388608.0 < kMultiCoordinateHeightTolerance, "半径の丸めと追加演算の事前配分を覆う");
static_assert(kMultiCoordinateHeightTolerance / 50.0 + 1.0 / 262144.0 < kMultiCoordinateCosineTolerance, "軸交換でも高度の丸めを余弦の単位へ換算する");
static_assert(kMultiCoordinateHeightTolerance / 4.0 + 8.0 * kMultiCoordinateCosineTolerance + 1.0 / 4096.0 < kMultiCoordinateMixedTolerance, "混合成分の誤差配分を事前に確認する");
static_assert(kMultiCoordinateHeightTolerance < 0.78125 && kMultiCoordinateCosineTolerance < 0.0078125, "二つの非対称物理点に残る旧配置の誤差を許容しない");
static_assert(kMultiCoordinateHeightTolerance < 0.390625 && kMultiCoordinateHeightTolerance < 25.0, "高度だけ旧配置と全行地表固定も高度成分だけで拒否する");

// 読戻し値が全成分で有限かつ独立解析値に一致するかを判定する。NaNや未書込みを誤合格させない。
static bool MultiCoordinateMatches_Internal(const FVec4& value, const f64 (&expected)[3])
{
    return IsFiniteProbeValue_Internal(value.x) && IsFiniteProbeValue_Internal(value.y) && IsFiniteProbeValue_Internal(value.z) && IsFiniteProbeValue_Internal(value.w) && value.w == 1.0f && ::fabs(static_cast<f64>(value.x) - expected[0]) <= kMultiCoordinateHeightTolerance && ::fabs(static_cast<f64>(value.y) - expected[1]) <= kMultiCoordinateCosineTolerance && ::fabs(static_cast<f64>(value.z) - expected[2]) <= kMultiCoordinateMixedTolerance;
}

// 全表の未書込み・範囲外・混合成分の破損を検査する。派生量の上下限にも事前の丸め誤差を適用する。
static bool MultiCoordinateTableValueValid_Internal(const FVec4& value)
{
    // 座標の正否は物理点からの参照と端点検査へ委ね、ここでは格納形式と値域だけを判定する。
    const f64 expected[3] = {static_cast<f64>(value.x),static_cast<f64>(value.y),static_cast<f64>(value.x) / 4.0 + static_cast<f64>(value.y) * 8.0 + 3.0};
    // wは半径なので厳密な範囲、xとyは最適化で再結合され得るため既定の誤差配分を使う。
    return MultiCoordinateMatches_Internal(FVec4{value.x,value.y,value.z,1.0f}, expected) && IsFiniteProbeValue_Internal(value.w) && value.x >= -kMultiCoordinateHeightTolerance && value.x <= 100.0 + kMultiCoordinateHeightTolerance && value.y >= -1.0 - kMultiCoordinateCosineTolerance && value.y <= 1.0 + kMultiCoordinateCosineTolerance && value.w >= 6360.0f && value.w <= 6460.0f;
}

// 生成表の下端・上端を直接検査する。生の半径は厳密一致、派生した高度は事前に配分した丸め誤差で検査する。
static bool MultiCoordinateBoundaryMatches_Internal(const FVec4& value, bool upper, u32 field)
{
    // 対照自身の正しい端点と、製品に要求する端点を同じ比較器で区別する。
    const f32 radius = field == 3u || field == 4u ? (upper ? 6458.4375f : 6361.5625f) : (upper && field != 5u ? 6460.0f : 6360.0f);
    // 定数化・軸交換だけはxを高度以外へ変更している。wはどの場でも生成した実半径である。
    // preciseでないHLSLではr-kBottomが再結合されるため、格納したrの減算と同じ丸めになるとは限らない。
    const bool heightMatches = field == 1u || field == 2u || ::fabs(static_cast<f64>(value.x) - (static_cast<f64>(radius) - 6360.0)) <= kMultiCoordinateHeightTolerance;
    return IsFiniteProbeValue_Internal(value.x) && IsFiniteProbeValue_Internal(value.y) && IsFiniteProbeValue_Internal(value.z) && IsFiniteProbeValue_Internal(value.w) && value.w == radius && heightMatches;
}

// 三文の変更には追従するが、不完全・別関数・コメント・重複を生成式として使わないことを検査する。
ACS_TEST(Atmosphere, MultiCoordinateRecoveryKeepsCurrentExpressions)
{
    // 元の半画素式を期待値へ固定しないため、異なる右辺もそのまま抽出できることを要求する。
    constexpr const char* statements = "float2 uv=float2(id.xy)/float2(W-1,H-1);\nfloat cosSun=uv.x*2.0-1.0;\nfloat r=kBottom+uv.y*(kTop-kBottom);\n";
    // 入口と無関係な偽宣言は、字句として除外される。
    FString body("/* void CSMulti(){float2 uv=bad;} */\nvoid Other(){float2 uv=0;}\nvoid CSMulti(uint3 id:SV_DispatchThreadID){\n");
    body.Append(statements);
    body.Append("float3 unused=0; }");
    EXPECT_TRUE(ExtractMultiCoordinateStatements_Internal(body) == FStringView(statements));
    // 抽出関数は生成式を勝手に修正せず、式中のコメントも保持する。
    const FString centered("void CSMulti(uint3 id:SV_DispatchThreadID){float2 uv=(float2(id.xy) /* keep */ +0.5)/float2(W,H); float cosSun=2*uv.x-1; float r=kBottom+100*uv.y;}");
    EXPECT_TRUE(ExtractMultiCoordinateStatements_Internal(centered) == FStringView("float2 uv=(float2(id.xy) /* keep */ +0.5)/float2(W,H);\nfloat cosSun=2*uv.x-1;\nfloat r=kBottom+100*uv.y;\n"));
    // 欠落、分岐内だけの宣言、間への別文混入、複数定義を拒否する。
    constexpr const char* invalid[] = {"void Other(){float2 uv=0; float cosSun=0; float r=0;}", "void CSMulti(){float2 uv=0; float cosSun=0;}", "void CSMulti(){if(true){float2 uv=0; float cosSun=0; float r=0;}}", "void CSMulti(){float2 uv=0; return; float cosSun=0; float r=0;}", "void CSMulti(){float2 uv=0; float cosSun=0; float r=(0;}", "void CSMulti(){float2 uv=0; float cosSun=0; float r=0;", "void CSMulti(){float2 uv=0; float cosSun=0; float r=0;} void CSMulti(){}", "void CSMulti(){float2 uv=0; float cosSun=0; float r=0; float2 uv=1; float cosSun=1; float r=1;}", "#if 0\nvoid CSMulti(){float2 uv=0; float cosSun=0; float r=0;}\n#endif\n"};
    for (const char* source : invalid) EXPECT_TRUE(ExtractMultiCoordinateStatements_Internal(FString(source)).IsEmpty());
    // 実製品も復元可能でなければ、数値試験へ進む前に失敗させる。
    const FString product = ReadRenderSource_Internal(L"../src/render/Atmosphere.cpp");
    for (u32 field = 0u; field < 6u; ++field) EXPECT_TRUE(!BuildMultiCoordinateGeneration_Internal(product, field).IsEmpty());
    EXPECT_TRUE(!BuildMultiCoordinateLookup_Internal(product, "kBakeCS").IsEmpty());
    EXPECT_TRUE(!BuildMultiCoordinateLookup_Internal(product, "kApCS").IsEmpty());
    // 通常生成が製品の三文そのものを含み、旧配置対照がuv以外を保持することも確認する。
    const FString coordinates = ExtractMultiCoordinateStatements_Internal(RestoreAtmosphereShaderSource_Internal(product, true, "kMultiCS"));
    const FString normal = BuildMultiCoordinateGeneration_Internal(product, 0u);
    const FString oldCentered = BuildMultiCoordinateGeneration_Internal(product, 3u);
    EXPECT_TRUE(!coordinates.IsEmpty());
    if (!coordinates.IsEmpty()) {
        EXPECT_TRUE(::strstr(normal.Data(), coordinates.Data()) != nullptr);
        // 現製品の右辺が既に修正済みでも、旧式はこの一か所だけ試験内に戻せる。
        const char* remaining = coordinates.Data();
        const FString uv = ReadMultiCoordinateDeclaration_Internal(remaining, "float2", "uv");
        EXPECT_TRUE(!uv.IsEmpty());
        EXPECT_TRUE(::strstr(oldCentered.Data(), "float2 uv=(float2(id.xy)+0.5)/float2(W,H);\n") != nullptr);
        if (!uv.IsEmpty()) EXPECT_TRUE(::strstr(oldCentered.Data(), remaining) != nullptr);
        // 不完全修正の対照も、太陽側の二文が改変されていないことを要求する。
        const FString cosine = ReadMultiCoordinateDeclaration_Internal(remaining, "float", "cosSun");
        for (u32 field = 4u; field < 6u; ++field) {
            // 高度だけを旧配置へ戻す場合と、全行を地表に固定する場合。
            const FString incomplete = BuildMultiCoordinateGeneration_Internal(product, field);
            EXPECT_TRUE(!uv.IsEmpty() && !cosine.IsEmpty());
            if (!uv.IsEmpty() && !cosine.IsEmpty()) {
                EXPECT_TRUE(::strstr(incomplete.Data(), uv.Data()) != nullptr);
                EXPECT_TRUE(::strstr(incomplete.Data(), cosine.Data()) != nullptr);
            }
            EXPECT_TRUE(::strstr(incomplete.Data(), field == 4u ? "float r=kBottom+((float(id.y)+0.5)/float(H))*(kTop-kBottom);" : "float r=kBottom;") != nullptr);
        }
    }
}

// 解析値・比較器の対照。定数化、軸交換、不完全な高度修正、無効値をGPU試験とは別に拒否する。
ACS_TEST(Atmosphere, MultiCoordinateOracleRejectsConstantAndAxisSwap)
{
    // 二つとも正確に表せる非対称な物理点。高度25km/余弦0と高度62.5km/余弦-0.25。
    constexpr FVec4 points[] = {{6385.0f,0.0f,0.0f,1.0f},{6422.5f,-0.25f,0.0f,1.0f}};
    // 補間式によらない、線形場の手計算結果。
    constexpr f64 reference[2][3] = {{25.0,0.0,9.25},{62.5,-0.25,16.625}};
    // NaNと正負無限大を、比較する全成分へ一つずつ入れる。
    constexpr u32 invalidBits[] = {0x7fc00000u,0x7f800000u,0xff800000u};
    for (u32 sample = 0u; sample < 2u; ++sample) {
        // 算式の期待値自体も固定した解析値へ照合する。
        f64 expected[3]{};
        MultiCoordinateExpected_Internal(points[sample], 0u, expected);
        for (u32 channel = 0u; channel < 3u; ++channel) EXPECT_EQ(expected[channel], reference[sample][channel]);
        // 正しいRGBA値だけは受け入れる。
        const FVec4 correct{static_cast<f32>(expected[0]),static_cast<f32>(expected[1]),static_cast<f32>(expected[2]),1.0f};
        EXPECT_TRUE(MultiCoordinateMatches_Internal(correct, expected));
        EXPECT_FALSE(MultiCoordinateMatches_Internal(FVec4{0,0,0,1}, expected));
        for (u32 field = 1u; field < 6u; ++field) {
            // 対照の解析値は有限でも、通常場の解析値とは必ず区別される。
            f64 changed[3]{};
            MultiCoordinateExpected_Internal(points[sample], field, changed);
            EXPECT_FALSE(MultiCoordinateMatches_Internal(FVec4{static_cast<f32>(changed[0]),static_cast<f32>(changed[1]),static_cast<f32>(changed[2]),1.0f}, expected));
            if (field == 3u) {
                // 旧配置の手計算結果も固定する。対照の検出成功は通常場の正しさとは数えない。
                constexpr f64 oldCentered[2][3] = {{25.78125,0.0,9.4453125},{62.109375,-0.2421875,16.58984375}};
                for (u32 channel = 0u; channel < 3u; ++channel) EXPECT_EQ(changed[channel], oldCentered[sample][channel]);
            } else if (field == 4u || field == 5u) {
                // 太陽余弦だけ正しくても、高度の誤りは独立成分と混合成分の両方に残る。
                constexpr f64 oldHeight[2][3] = {{25.78125,0.0,9.4453125},{62.109375,-0.25,16.52734375}};
                constexpr f64 allGround[2][3] = {{0.0,0.0,3.0},{0.0,-0.25,1.0}};
                for (u32 channel = 0u; channel < 3u; ++channel) EXPECT_EQ(changed[channel], field == 4u ? oldHeight[sample][channel] : allGround[sample][channel]);
            }
        }
        for (u32 bits : invalidBits) {
            for (u32 channel = 0u; channel < 4u; ++channel) {
                // FVec4の内部配置に配列アクセスを仮定せず、既知のfloat配列から組み立てる。
                f32 components[] = {correct.x,correct.y,correct.z,correct.w};
                components[channel] = ProbeFloatFromBits_Internal(bits);
                EXPECT_FALSE(MultiCoordinateMatches_Internal(FVec4{components[0],components[1],components[2],components[3]}, expected));
            }
        }
    }
    // 直接読戻しの比較器にも対照を与える。高度だけの旧配置は両端、全行地表固定は上端で拒否する。
    EXPECT_TRUE(MultiCoordinateBoundaryMatches_Internal(FVec4{0,0,3,6360}, false, 0u));
    EXPECT_TRUE(MultiCoordinateBoundaryMatches_Internal(FVec4{100,0,28,6460}, true, 0u));
    // 派生した高度だけの1段階の丸めは認めるが、生の半径が1段階ずれた結果は認めない。
    EXPECT_TRUE(MultiCoordinateBoundaryMatches_Internal(FVec4{100.0f - 1.0f / 131072.0f,0,28,6460}, true, 0u));
    EXPECT_FALSE(MultiCoordinateBoundaryMatches_Internal(FVec4{100,0,28,6460.0f - 1.0f / 2048.0f}, true, 0u));
    EXPECT_FALSE(MultiCoordinateBoundaryMatches_Internal(FVec4{1.5625f,0,3.390625f,6361.5625f}, false, 0u));
    EXPECT_FALSE(MultiCoordinateBoundaryMatches_Internal(FVec4{98.4375f,0,27.609375f,6458.4375f}, true, 0u));
    EXPECT_FALSE(MultiCoordinateBoundaryMatches_Internal(FVec4{0,0,3,6360}, true, 0u));
    // 生の半径だけ、または高度だけを都合よく端点に偽装しても拒否する。
    EXPECT_FALSE(MultiCoordinateBoundaryMatches_Internal(FVec4{98.4375f,0,27.609375f,6460}, true, 0u));
    EXPECT_FALSE(MultiCoordinateBoundaryMatches_Internal(FVec4{100,0,28,6458.4375f}, true, 0u));
    // 正負の丸めを境界比較だけでなく、GPU読戻しに使う全表の判定にも通す。
    constexpr f32 directions[] = {-1.0f, 1.0f};
    for (f32 direction : directions) {
        // 上端100kmの隣接値と、それに対応する混合成分。生の半径は不変。
        const f32 roundedHeight = 100.0f + direction / 131072.0f;
        const FVec4 rounded{roundedHeight,-1.0f,roundedHeight * 0.25f - 5.0f,6460.0f};
        EXPECT_TRUE(MultiCoordinateBoundaryMatches_Internal(rounded, true, 0u));
        EXPECT_TRUE(MultiCoordinateTableValueValid_Internal(rounded));
    }
    EXPECT_FALSE(MultiCoordinateTableValueValid_Internal(FVec4{100.03125f,0,28.0078125f,6460}));
    EXPECT_FALSE(MultiCoordinateTableValueValid_Internal(FVec4{-0.03125f,0,2.9921875f,6360}));
    EXPECT_FALSE(MultiCoordinateTableValueValid_Internal(FVec4{100,0,28,6460.0f + 1.0f / 2048.0f}));
    EXPECT_FALSE(MultiCoordinateTableValueValid_Internal(FVec4{0,0,3,6360.0f - 1.0f / 2048.0f}));
}

// 実生成座標→GPU上の線形場→実SampleMultiを一続きで調べる。透過率表の座標・積分は試験しない。
ACS_TEST(Atmosphere, MultiGeneratedCoordinatesMatchPhysicalLookup)
{
    // 全シェーダーを同じ一回の読取りから復元し、古い診断用コピーを製品と扱わない。
    const FString product = ReadRenderSource_Internal(L"../src/render/Atmosphere.cpp");
    EXPECT_TRUE(!product.IsEmpty());
    if (product.IsEmpty()) return;
    // 表は製品の多重散乱と同じ32角。出力は全格子点1024個と、特別な物理点の反復256個。
    constexpr u32 tableSize = 32u;
    constexpr u32 width = 32u;
    constexpr u32 height = 40u;
    constexpr u32 gridCount = tableSize * tableSize;
    constexpr u32 pixelCount = width * height;
    static_assert(sizeof(FVec4) == 4u * sizeof(f32), "RGBA32Fの全成分を連続して読み戻す");
    // 先頭二条件は対照が必ず拒否される物理点。残りは端点、端上、内部、既存の画素中心上の点。
    constexpr FVec4 points[] = {{6385.0f,0.0f,0,1},{6422.5f,-0.25f,0,1},{6360.0f,-1.0f,0,1},{6360.0f,1.0f,0,1},{6460.0f,-1.0f,0,1},{6460.0f,1.0f,0,1},{6360.0f,0.375f,0,1},{6460.0f,-0.625f,0,1},{6372.5f,-1.0f,0,1},{6447.5f,1.0f,0,1},{6410.0f,0.0f,0,1},{6397.5f,0.625f,0,1},{6361.5625f,-0.96875f,0,1},{6458.4375f,0.96875f,0,1},{6386.5625f,0.34375f,0,1},{6433.4375f,-0.53125f,0,1}};
    // 条件の反復数を固有の試験点数として数えない。
    constexpr u32 pointCount = sizeof(points) / sizeof(points[0]);
    static_assert((pixelCount - gridCount) % pointCount == 0u, "各物理点を同じ回数ずつ配置する");
    // CPUは照会する物理点だけを用意し、生成表はアップロードしない。
    TArray<FVec4> inputs;
    inputs.SetNum(pixelCount);
    for (u32 pixel = 0u; pixel < gridCount; ++pixel) {
        // 閉区間の全格子点を独立に指定する。期待値はこの入力の線形場であり、生成UVの複製ではない。
        const f64 altitude = 100.0 * static_cast<f64>(pixel / tableSize) / 31.0;
        const f64 cosine = -1.0 + 2.0 * static_cast<f64>(pixel % tableSize) / 31.0;
        inputs[pixel] = FVec4{static_cast<f32>(6360.0 + altitude),static_cast<f32>(cosine),0.0f,1.0f};
    }
    for (u32 pixel = gridCount; pixel < pixelCount; ++pixel) inputs[pixel] = points[(pixel - gridCount) % pointCount];
    // GPU画像とCPU読戻し先をともにNaNで初期化する。
    TArray<FVec4> unwritten;
    unwritten.SetNum(pixelCount);
    const f32 nan = ProbeFloatFromBits_Internal(0x7fc00000u);
    for (FVec4& value : unwritten) value = FVec4{nan,nan,nan,nan};
    // 全資源の所有者。資源より先に作り、最後に破棄する。
    FDeviceConfig configuration{};
    // GPUがない場合も省略合格にせず、作成失敗として記録する。
    auto device = CreateRhiDevice(configuration);
    EXPECT_TRUE(device.IsOk());
    if (device.IsErr()) return;
    test::RecordInfo(FSourceLoc::Current(), "multi_coordinate_gpu backend=%s adapter=%s grid_points=%u extra_points=%u output_pixels=%u height_tolerance_km=%.12g cosine_tolerance=%.12g mixed_tolerance_km=%.12g", device.Value()->BackendName(), device.Value()->AdapterName(), gridCount, pointCount, pixelCount, kMultiCoordinateHeightTolerance, kMultiCoordinateCosineTolerance, kMultiCoordinateMixedTolerance);
    // 半径kmと余弦を実行時に読み、照会をコンパイラーの定数評価へ置換させない。
    FTextureDesc inputDescription{};
    inputDescription.width = width;
    inputDescription.height = height;
    inputDescription.format = EFormat::R32G32B32A32_Float;
    inputDescription.initial_data = inputs.GetData();
    inputDescription.initial_data_size = pixelCount * sizeof(FVec4);
    // 全形式・利用者へ共通に渡す物理点画像。
    auto inputTexture = CreateRhiTexture(*device.Value(), inputDescription);
    EXPECT_TRUE(inputTexture.IsOk());
    if (inputTexture.IsErr()) return;
    // 空画像と空気遠近法の実参照をそれぞれ同じ生成表へ接続する。
    constexpr const char* consumers[] = {"kBakeCS", "kApCS"};
    // 完了数の配列も実際の利用者数から決める。
    constexpr u32 consumerCount = sizeof(consumers) / sizeof(consumers[0]);
#if !WITH_RENDER_DILIGENT
    // RawDX12は既定SM5.1と明示SM6の両方。
    constexpr u32 variantCount = 2u;
#else
    // Diligentは既存の一形式だけを使い、未対応形式を合格として数えない。
    constexpr u32 variantCount = 1u;
#endif
    // 生成の提出・全表読戻しと、各参照の提出・全出力読戻しの完了数を別々に記録する。
    u32 generated[variantCount]{};
    u32 completed[variantCount][consumerCount]{};
    for (u32 variant = 0u; variant < variantCount; ++variant) {
        // 実際に要求する形式名。既定側は既存のnull指定を維持する。
        const char* targetName = variant == 0u ? "default(cs_5_1)" : "cs_6_0";
        for (u32 field = 0u; field < 6u; ++field) {
            // 通常、定数化、軸交換、両軸旧配置、高度だけ旧配置、全行地表固定。
            const FString generationSource = BuildMultiCoordinateGeneration_Internal(product, field);
            EXPECT_TRUE(!generationSource.IsEmpty());
            if (generationSource.IsEmpty()) continue;
            // 表の生成を実行する形式・入口・元コード。
            FShaderDesc generationDescription{};
            generationDescription.stage = EShaderStage::Compute;
            generationDescription.hlsl_source = generationSource.Data();
            generationDescription.entry_point = "CSCoordinateGeneration";
            generationDescription.debug_name = "Atmo.MultiCoordinateGeneration";
            if (variant != 0u) generationDescription.target = "cs_6_0";
            // 同期コンパイル後も、パイプラインの破棄まで保持する。
            auto generationShader = CreateRhiShader(*device.Value(), generationDescription);
            EXPECT_TRUE(generationShader.IsOk());
            if (generationShader.IsErr()) continue;
            // 表の生成には入力SRVを使わず、座標計算は必ずGPUで行う。
            FComputePipelineDesc generationPipelineDescription{};
            generationPipelineDescription.cs = generationShader.Value().Get();
            generationPipelineDescription.uav_slots = 1u;
            generationPipelineDescription.uav_names[0] = "coordinateTable";
            // 生成画像への書込みだけを結合する既存所有型。
            auto generationPipeline = CreateRhiComputePipeline(*device.Value(), generationPipelineDescription);
            EXPECT_TRUE(generationPipeline.IsOk());
            if (generationPipeline.IsErr()) continue;
            // 各形式・場ごとに新しいNaN画像を使い、前回の結果で未書込みを隠さない。
            FTextureDesc tableDescription{};
            tableDescription.width = tableSize;
            tableDescription.height = tableSize;
            tableDescription.format = EFormat::R32G32B32A32_Float;
            tableDescription.is_uav = true;
            tableDescription.initial_data = unwritten.GetData();
            tableDescription.initial_data_size = gridCount * sizeof(FVec4);
            // この資源を生成後の参照にもそのまま使う。
            auto tableTexture = CreateRhiTexture(*device.Value(), tableDescription);
            EXPECT_TRUE(tableTexture.IsOk());
            if (tableTexture.IsErr()) continue;
            // 生成の提出と待機が完了するまで、全参照先を生存させる。
            auto generationCommand = CreateRhiCommandList(*device.Value());
            EXPECT_TRUE(generationCommand.IsOk());
            if (generationCommand.IsErr()) continue;
            generationCommand.Value()->Begin();
            generationCommand.Value()->SetComputePipeline(*generationPipeline.Value());
            generationCommand.Value()->BindUav(0u, *tableTexture.Value());
            generationCommand.Value()->Dispatch(tableSize / 8u, tableSize / 8u, 1u);
            generationCommand.Value()->End();
            // 失敗時も待機し、GPUに参照される可能性のある資源を早く解放しない。
            const bool generationSubmitted = generationCommand.Value()->Submit();
            device.Value()->WaitIdle();
            EXPECT_TRUE(generationSubmitted);
            if (!generationSubmitted) continue;
            // 生成表も全1024画素を読む。ただしCPUからの再アップロードは一切しない。
            TArray<FVec4> tableValues;
            tableValues.SetNum(gridCount);
            ::memcpy(tableValues.GetData(), unwritten.GetData(), gridCount * sizeof(FVec4));
            // 一部だけ読めた場合は残りのNaNで失敗させる。
            const bool tableRead = device.Value()->ReadTexture(*tableTexture.Value(), tableValues.GetData(), gridCount * sizeof(FVec4));
            EXPECT_TRUE(tableRead);
            if (!tableRead) continue;
            ++generated[variant];
            // 生成未書込み・範囲外と、格納した線形場自体の破損を全画素で検出する。
            u32 invalidTablePixels = 0u;
            // 各列の下端・上端を、表参照を経ずに生の半径と高度で調べる。
            u32 boundaryPixels = 0u;
            u32 boundaryFailures = 0u;
            // 不完全修正の対照を、製品用の端点条件が確実に拒否する画素数。
            u32 ordinaryBoundaryRejections = 0u;
            for (u32 pixel = 0u; pixel < gridCount; ++pixel) {
                // 生成した一画素の全成分。
                const FVec4 value = tableValues[pixel];
                // UVをCPU計算せず、格納形式・値域の判定をCPU対照と共有する。
                const bool valid = MultiCoordinateTableValueValid_Internal(value);
                if (!valid) ++invalidTablePixels;
                if (pixel < tableSize || pixel >= gridCount - tableSize) {
                    // 上端の行であるか。各端の全32列を検査し、代表一画素だけにしない。
                    const bool upper = pixel >= gridCount - tableSize;
                    const bool boundaryMatches = MultiCoordinateBoundaryMatches_Internal(value, upper, field);
                    ++boundaryPixels;
                    if (!boundaryMatches) {
                        // 端点のずれが半径か派生した高度かを区別できるよう、最初の不一致を残す。
                        if (boundaryFailures == 0u) test::RecordInfo(FSourceLoc::Current(), "multi_coordinate_boundary target=%s field=%u xy=(%u,%u) upper=%u rgba=(%.12g,%.12g,%.12g,%.12g)", targetName, field, pixel % tableSize, pixel / tableSize, upper ? 1u : 0u, value.x, value.y, value.z, value.w);
                        ++boundaryFailures;
                    }
                    if (valid && boundaryMatches && !MultiCoordinateBoundaryMatches_Internal(value, upper, 0u)) ++ordinaryBoundaryRejections;
                }
            }
            EXPECT_EQ(invalidTablePixels, 0u);
            EXPECT_EQ(boundaryPixels, 2u * tableSize);
            EXPECT_EQ(boundaryFailures, 0u);
            if (field == 0u) EXPECT_EQ(ordinaryBoundaryRejections, 0u);
            else if (field == 3u || field == 4u) EXPECT_EQ(ordinaryBoundaryRejections, 2u * tableSize);
            else if (field == 5u) EXPECT_EQ(ordinaryBoundaryRejections, tableSize);
            test::RecordInfo(FSourceLoc::Current(), "multi_coordinate_generation target=%s field=%u submitted=1 readback=1 pixels=%u invalid=%u boundary_pixels=%u boundary_failures=%u ordinary_boundary_rejections=%u", targetName, field, gridCount, invalidTablePixels, boundaryPixels, boundaryFailures, ordinaryBoundaryRejections);
            for (u32 consumer = 0u; consumer < consumerCount; ++consumer) {
                // 実SampleMultiと、空気遠近法だけが使う実LoadMultiBilinearを復元する。
                const FString lookupSource = BuildMultiCoordinateLookup_Internal(product, consumers[consumer]);
                EXPECT_TRUE(!lookupSource.IsEmpty());
                if (lookupSource.IsEmpty()) continue;
                // 座標入力と実SampleMultiを接続した診断入口。
                FShaderDesc lookupDescription{};
                lookupDescription.stage = EShaderStage::Compute;
                lookupDescription.hlsl_source = lookupSource.Data();
                lookupDescription.entry_point = "CSCoordinateLookup";
                lookupDescription.debug_name = "Atmo.MultiCoordinateLookup";
                if (variant != 0u) lookupDescription.target = "cs_6_0";
                // 参照用パイプラインより長く生存するコンパイル結果。
                auto lookupShader = CreateRhiShader(*device.Value(), lookupDescription);
                EXPECT_TRUE(lookupShader.IsOk());
                if (lookupShader.IsErr()) continue;
                // 実生成した同じ画像をt1へ結合し、RHIへUAV→SRVの遷移を委ねる。
                FComputePipelineDesc lookupPipelineDescription{};
                lookupPipelineDescription.cs = lookupShader.Value().Get();
                lookupPipelineDescription.srv_slots = 2u;
                lookupPipelineDescription.srv_names[0] = "coordinateInput";
                lookupPipelineDescription.srv_names[1] = "multiLut";
                lookupPipelineDescription.uav_slots = 1u;
                lookupPipelineDescription.uav_names[0] = "coordinateOutput";
                // 利用者ごとの実参照に対応する結合情報。
                auto lookupPipeline = CreateRhiComputePipeline(*device.Value(), lookupPipelineDescription);
                EXPECT_TRUE(lookupPipeline.IsOk());
                if (lookupPipeline.IsErr()) continue;
                // 結果画像も利用者ごとにNaNから始める。
                FTextureDesc outputDescription = inputDescription;
                outputDescription.is_uav = true;
                outputDescription.initial_data = unwritten.GetData();
                // 未書込みを前の利用者の結果で埋めない新規画像。
                auto outputTexture = CreateRhiTexture(*device.Value(), outputDescription);
                EXPECT_TRUE(outputTexture.IsOk());
                if (outputTexture.IsErr()) continue;
                // 表の生成完了後に参照を記録する。待機後に他資源より先に破棄する。
                auto command = CreateRhiCommandList(*device.Value());
                EXPECT_TRUE(command.IsOk());
                if (command.IsErr()) continue;
                command.Value()->Begin();
                command.Value()->SetComputePipeline(*lookupPipeline.Value());
                command.Value()->SetTexture(0u, *inputTexture.Value());
                command.Value()->SetTexture(1u, *tableTexture.Value());
                command.Value()->BindUav(0u, *outputTexture.Value());
                command.Value()->Dispatch(width / 8u, height / 8u, 1u);
                command.Value()->End();
                // 提出成功と完了待ちを経た画像だけを合否に使う。
                const bool submitted = command.Value()->Submit();
                device.Value()->WaitIdle();
                EXPECT_TRUE(submitted);
                if (!submitted) continue;
                // CPU側もNaNから始め、部分転送を検出する。
                TArray<FVec4> values;
                values.SetNum(pixelCount);
                ::memcpy(values.GetData(), unwritten.GetData(), pixelCount * sizeof(FVec4));
                // 全画素のRGBAを検査できた提出だけを完了数へ入れる。
                const bool read = device.Value()->ReadTexture(*outputTexture.Value(), values.GetData(), pixelCount * sizeof(FVec4));
                EXPECT_TRUE(read);
                if (!read) continue;
                ++completed[variant][consumer];
                // 自身の場への一致と、通常場としての誤合格を分離して集計する。
                u32 fieldFailures = 0u;
                u32 ordinaryMatches = 0u;
                u32 rejectedAsymmetric = 0u;
                bool reportedFailure = false;
                for (u32 pixel = 0u; pixel < pixelCount; ++pixel) {
                    // 生成画素番号やGPU表の値を使わず、入力した物理点だけから期待値を決める。
                    f64 expected[3]{};
                    f64 ordinary[3]{};
                    MultiCoordinateExpected_Internal(inputs[pixel], field, expected);
                    MultiCoordinateExpected_Internal(inputs[pixel], 0u, ordinary);
                    // 対照も自身の解析値には一致する必要がある。
                    const bool matches = MultiCoordinateMatches_Internal(values[pixel], expected);
                    // 誤った対照が通常場として通っていないかを独立に数える。
                    const bool ordinaryMatch = MultiCoordinateMatches_Internal(values[pixel], ordinary);
                    if (!matches) ++fieldFailures;
                    if (ordinaryMatch) ++ordinaryMatches;
                    // 反復画素にも同じ条件を適用する。対照の無効値は「検出成功」へ数えない。
                    const bool asymmetric = pixel >= gridCount && (pixel - gridCount) % pointCount < 2u;
                    if (field != 0u && asymmetric && matches && !ordinaryMatch) ++rejectedAsymmetric;
                    if (!matches && !reportedFailure) {
                        // 同じ不一致を大量表示せず、最初の画素だけ詳しく残す。
                        const FVec4 value = values[pixel];
                        test::RecordInfo(FSourceLoc::Current(), "multi_coordinate_pixel target=%s consumer=%s field=%u pixel=%u height_km=%.12g cosine=%.12g expected=(%.12g,%.12g,%.12g) rgba=(%.9g,%.9g,%.9g,%.9g)", targetName, consumers[consumer], field, pixel, static_cast<f64>(inputs[pixel].x) - 6360.0, static_cast<f64>(inputs[pixel].y), expected[0], expected[1], expected[2], value.x, value.y, value.z, value.w);
                        reportedFailure = true;
                    }
                }
                test::RecordInfo(FSourceLoc::Current(), "multi_coordinate_result target=%s consumer=%s field=%u submitted=1 readback=1 checked_pixels=%u field_failures=%u ordinary_matches=%u rejected_asymmetric=%u", targetName, consumers[consumer], field, pixelCount, fieldFailures, ordinaryMatches, rejectedAsymmetric);
                EXPECT_EQ(fieldFailures, 0u);
                if (field == 0u) EXPECT_EQ(ordinaryMatches, pixelCount);
                else {
                    EXPECT_TRUE(ordinaryMatches < pixelCount);
                    EXPECT_EQ(rejectedAsymmetric, 2u * ((pixelCount - gridCount) / pointCount));
                }
            }
        }
    }
    // どれか一形式・一利用者・一対照の未実行でも失敗にする。
    for (u32 variant = 0u; variant < variantCount; ++variant) {
        EXPECT_EQ(generated[variant], 6u);
        for (u32 consumer = 0u; consumer < consumerCount; ++consumer) EXPECT_EQ(completed[variant][consumer], 6u);
    }
}

#endif
