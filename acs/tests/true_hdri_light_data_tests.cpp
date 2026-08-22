// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "render/TrueHdriLightData.h"

using namespace acs;

namespace {

/** 方位が画像中央をまたぐ昼の公式LightData相当CSV。 */
constexpr char kDaylightCsv[] = "----Light direction----\r\n"
                                "photoshopXY(panorama uv[%]),49.967,31.885\r\n"
                                "altitude_azimuth,32.607,359.88\r\n"
                                "----Light color sRGB----\r\n"
                                ",R,G,B,scale\r\n"
                                "color illuminance,110854.344,94286.023,78994.93,1\r\n"
                                "normarized color illminance,1.0,0.851,0.713,110854.344\r\n"
                                "----Light color rec.2020----\r\n"
                                ",R,G,B,scale\r\n"
                                "color illuminance,104028.273,95256.125,80846.094,1\r\n";

/** 夕方の非対称な色照度を持つ公式LightData相当CSV。 */
constexpr char kEveningCsv[] = "----Light direction----\n"
                               "photoshopXY(panorama uv[%]),50.008,47.639\n"
                               "altitude_azimuth,4.251,0.028\n"
                               "----Light color sRGB----\n"
                               ",R,G,B,scale\n"
                               "color illuminance,26241.815,13871.951,2472.481,1\n"
                               "normarized color illminance,1.0,0.529,0.094,26241.815";

/** 緑成分が最大となる正午付近の公式LightData相当CSV。 */
constexpr char kNoonCsv[] = "----Light direction----\n"
                            "photoshopXY(panorama uv[%]),49.96,12.679\n"
                            "altitude_azimuth,67.178,359.856\n"
                            "----Light color sRGB----\n"
                            ",R,G,B,scale\n"
                            "color illuminance,130530.899,130651.25,114040.724,1\n"
                            "normarized color illminance,0.999,1.0,0.873,130651.25\n";

/** 文字列リテラルの終端NULを除いたバイト数を返す。 */
template<usize Size>
constexpr usize CsvSize_Internal(const char (&)[Size]) noexcept
{
    return Size - 1u;
}

} // namespace

ACS_TEST(TrueHdriLightData, ParsesOfficialRowsWithoutAllocationOrNullTermination)
{
    /** EitaiBridge公式値と同じ構成の解析結果。 */
    TResult<FTrueHdriLightData> parsed = FTrueHdriLightData::ParseSrgbCsv(kDaylightCsv, CsvSize_Internal(kDaylightCsv));
    EXPECT_TRUE(parsed.IsOk());
    if (parsed.IsErr()) return;

    /** 解析済みの測定値。 */
    const FTrueHdriLightData& data = parsed.Value();
    EXPECT_NEAR(data.PanoramaUPercent(), 49.967f, 1.0e-5f);
    EXPECT_NEAR(data.PanoramaVPercent(), 31.885f, 1.0e-5f);
    EXPECT_NEAR(data.AltitudeDegrees(), 32.607f, 1.0e-5f);
    EXPECT_NEAR(data.AzimuthDegrees(), 359.88f, 1.0e-4f);
    EXPECT_NEAR(data.ColorIlluminanceLux().x, 110854.344f, 0.01f);
    EXPECT_NEAR(data.ColorIlluminanceLux().y, 94286.023f, 0.01f);
    EXPECT_NEAR(data.ColorIlluminanceLux().z, 78994.93f, 0.01f);
    EXPECT_NEAR(data.PeakIlluminanceLux(), 110854.344f, 0.01f);
}

ACS_TEST(TrueHdriLightData, AppliesOneYawConventionToPanoramaSunAndDirectionalLight)
{
    /** 夕方測定値の解析結果。 */
    TResult<FTrueHdriLightData> parsed = FTrueHdriLightData::ParseSrgbCsv(kEveningCsv, CsvSize_Internal(kEveningCsv));
    EXPECT_TRUE(parsed.IsOk());
    if (parsed.IsErr()) return;

    /** 画像を回転しない太陽方向。 */
    TResult<FVec3> unrotated = parsed.Value().ResolveSunDirection(0.0f);
    /** 画像を+90度回転した太陽方向。 */
    TResult<FVec3> rotated = parsed.Value().ResolveSunDirection(90.0f);
    EXPECT_TRUE(unrotated.IsOk());
    EXPECT_TRUE(rotated.IsOk());
    if (unrotated.IsErr() || rotated.IsErr()) return;
    EXPECT_NEAR(unrotated.Value().x, 0.000487f, 2.0e-5f);
    EXPECT_NEAR(unrotated.Value().y, 0.07412f, 2.0e-4f);
    EXPECT_NEAR(unrotated.Value().z, 0.99725f, 2.0e-4f);
    EXPECT_NEAR(rotated.Value().x, unrotated.Value().z, 2.0e-5f);
    EXPECT_NEAR(rotated.Value().y, unrotated.Value().y, 2.0e-5f);
    EXPECT_NEAR(rotated.Value().z, -unrotated.Value().x, 2.0e-5f);

    /** 測定ピーク照度を既定のACS太陽強度へ対応させた光色。 */
    const f32 measured_peak = parsed.Value().PeakIlluminanceLux();
    TResult<FVec3> light_color = parsed.Value().ResolveDirectionalLightColor(measured_peak, 2.35f);
    EXPECT_TRUE(light_color.IsOk());
    if (light_color.IsErr()) return;
    EXPECT_NEAR(light_color.Value().x, 2.35f, 1.0e-5f);
    EXPECT_NEAR(light_color.Value().y, 1.242f, 0.002f);
    EXPECT_NEAR(light_color.Value().z, 0.221f, 0.002f);
}

ACS_TEST(TrueHdriLightData, UsesTheActualMaximumColorChannelAsMeasuredScale)
{
    /** 緑成分が最大となる正午測定値。 */
    TResult<FTrueHdriLightData> parsed = FTrueHdriLightData::ParseSrgbCsv(kNoonCsv, CsvSize_Internal(kNoonCsv));
    EXPECT_TRUE(parsed.IsOk());
    if (parsed.IsErr()) return;
    EXPECT_NEAR(parsed.Value().PeakIlluminanceLux(), 130651.25f, 0.01f);

    /** 測定ピークを1へ正規化した線形光色。 */
    TResult<FVec3> normalized = parsed.Value().ResolveDirectionalLightColor(parsed.Value().PeakIlluminanceLux(), 1.0f);
    EXPECT_TRUE(normalized.IsOk());
    if (normalized.IsErr()) return;
    EXPECT_NEAR(normalized.Value().x, 0.99908f, 0.00002f);
    EXPECT_NEAR(normalized.Value().y, 1.0f, 0.00001f);
    EXPECT_NEAR(normalized.Value().z, 0.87286f, 0.00002f);
}

ACS_TEST(TrueHdriLightData, RejectsMissingDuplicateNonFiniteAndContradictoryRows)
{
    /** 正規化行が無いCSV。 */
    constexpr char missing_row[] = "----Light direction----\nphotoshopXY(panorama uv[%]),50,25\naltitude_azimuth,45,0\n"
                                   "----Light color sRGB----\n,R,G,B,scale\ncolor illuminance,100,90,80,1\n";
    /** 同じ方向行が重複したCSV。 */
    constexpr char duplicate_row[] = "----Light direction----\nphotoshopXY(panorama uv[%]),50,25\nphotoshopXY(panorama "
                                     "uv[%]),50,25\naltitude_azimuth,45,0\n"
                                     "----Light color sRGB----\n,R,G,B,scale\n"
                                     "color illuminance,100,90,80,1\n"
                                     "normarized color illminance,1,.9,.8,100\n";
    /** 非有限値表現を含むCSV。 */
    constexpr char non_finite[] = "----Light direction----\n"
                                  "photoshopXY(panorama uv[%]),50,nan\n"
                                  "altitude_azimuth,45,0\n"
                                  "----Light color sRGB----\n,R,G,B,scale\n"
                                  "color illuminance,100,90,80,1\n"
                                  "normarized color illminance,1,.9,.8,100\n";
    /** 画像上の高度と角度行が食い違うCSV。 */
    constexpr char contradictory[] =
        "----Light direction----\nphotoshopXY(panorama uv[%]),50,25\naltitude_azimuth,20,0\n"
        "----Light color sRGB----\n,R,G,B,scale\n"
        "color illuminance,100,90,80,1\n"
        "normarized color illminance,1,.9,.8,100\n";

    EXPECT_TRUE(FTrueHdriLightData::ParseSrgbCsv(missing_row, CsvSize_Internal(missing_row)).IsErr());
    EXPECT_TRUE(FTrueHdriLightData::ParseSrgbCsv(duplicate_row, CsvSize_Internal(duplicate_row)).IsErr());
    EXPECT_TRUE(FTrueHdriLightData::ParseSrgbCsv(non_finite, CsvSize_Internal(non_finite)).IsErr());
    EXPECT_TRUE(FTrueHdriLightData::ParseSrgbCsv(contradictory, CsvSize_Internal(contradictory)).IsErr());
}

ACS_TEST(TrueHdriLightData, RejectsInvalidRuntimeCalibrationWithoutProducingLight)
{
    /** 正常な測定値の解析結果。 */
    TResult<FTrueHdriLightData> parsed = FTrueHdriLightData::ParseSrgbCsv(kDaylightCsv, CsvSize_Internal(kDaylightCsv));
    EXPECT_TRUE(parsed.IsOk());
    if (parsed.IsErr()) return;
    EXPECT_TRUE(parsed.Value().ResolveSunDirection(1000001.0f).IsErr());
    EXPECT_TRUE(parsed.Value().ResolveDirectionalLightColor(0.0f, 2.35f).IsErr());
    EXPECT_TRUE(parsed.Value().ResolveDirectionalLightColor(100000.0f, -1.0f).IsErr());
}

ACS_TEST(TrueHdriLightData, BoundsShortRowsAndAcceptsUtf8Bom)
{
    /** 列見出しが途中で終わるCSV。 */
    constexpr char short_header[] = "----Light direction----\n"
                                    "photoshopXY(panorama uv[%]),50,25\n"
                                    "altitude_azimuth,45,0\n"
                                    "----Light color sRGB----\n,R\n"
                                    "color illuminance,100,90,80,1\n"
                                    "normarized color illminance,1,.9,.8,100\n";
    /** 単精度の有限範囲を越える数値を含むCSV。 */
    constexpr char overflow_number[] = "----Light direction----\n"
                                       "photoshopXY(panorama uv[%]),50,25\n"
                                       "altitude_azimuth,45,0\n"
                                       "----Light color sRGB----\n,R,G,B,scale\n"
                                       "color illuminance,1e39,90,80,1\n"
                                       "normarized color illminance,1,.9,.8,100\n";
    /** UTF-8 BOM付きで内容は正常なCSV。 */
    constexpr char bom_csv[] = "\xEF\xBB\xBF"
                               "----Light direction----\n"
                               "photoshopXY(panorama uv[%]),50,25\n"
                               "altitude_azimuth,45,0\n"
                               "----Light color sRGB----\n,R,G,B,scale\n"
                               "color illuminance,100,90,80,1\n"
                               "normarized color illminance,1,.9,.8,100\n";

    EXPECT_TRUE(FTrueHdriLightData::ParseSrgbCsv(short_header, CsvSize_Internal(short_header)).IsErr());
    EXPECT_TRUE(FTrueHdriLightData::ParseSrgbCsv(overflow_number, CsvSize_Internal(overflow_number)).IsErr());
    EXPECT_TRUE(FTrueHdriLightData::ParseSrgbCsv(bom_csv, CsvSize_Internal(bom_csv)).IsOk());
}
