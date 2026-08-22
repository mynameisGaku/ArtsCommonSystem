// SPDX-License-Identifier: Apache-2.0
#include "render/TrueHdriLightData.h"

#include "container/StringView.h"
#include "foundation/Error.h"
#include "math/Math.h"

namespace acs {
namespace {

/** 受理するLightData CSVの最大バイト数。 */
constexpr usize kMaximumCsvBytes = 64u * 1024u;

/** 単精度浮動小数の最大有限値。 */
constexpr f32 kMaximumFiniteFloat = 3.402823466e+38f;

/** 測定値として受理する最大色照度。 */
constexpr f32 kMaximumIlluminanceLux = 1.0e+9f;

/** Photoshop座標と角度行の丸め差に許す角度。 */
constexpr f32 kDirectionAgreementDegrees = 0.05f;

/** CSV内で現在解釈している区画。 */
enum class ETrueHdriCsvSection : u8 {
    /** 必須区画の外側。 */
    Other,
    /** 太陽方向の区画。 */
    Direction,
    /** sRGB色照度の区画。 */
    Srgb
};

/** CSVを最後まで読んでから一括検証する一時状態。 */
struct FTrueHdriCsvState {
    /** Photoshop上の太陽中心座標。 */
    FVec2 PanoramaUvPercent{};
    /** 太陽高度。 */
    f32 AltitudeDegrees = 0.0f;
    /** 太陽方位。 */
    f32 AzimuthDegrees = 0.0f;
    /** color illuminance行のRGB。 */
    FVec3 ColorIlluminance{};
    /** color illuminance行の倍率。 */
    f32 ColorScale = 0.0f;
    /** normarized color illminance行のRGB。 */
    FVec3 NormalizedColor{};
    /** normarized color illminance行の照度尺度。 */
    f32 NormalizedScale = 0.0f;
    /** Photoshop座標を取得済みか。 */
    bool HasPanoramaUv = false;
    /** 高度と方位を取得済みか。 */
    bool HasAngles = false;
    /** sRGB列見出しを取得済みか。 */
    bool HasSrgbHeader = false;
    /** 色照度を取得済みか。 */
    bool HasColorIlluminance = false;
    /** 正規化色を取得済みか。 */
    bool HasNormalizedColor = false;
};

/** ASCII空白を前後から除いた非所有ビューを返す。 */
FStringView TrimAscii_Internal(FStringView text) noexcept
{
    /** 残す先頭位置。 */
    usize first = 0u;
    /** 残す末尾の次。 */
    usize last = text.Size();
    while (first < last && (text[first] == ' ' || text[first] == '\t'))
        ++first;
    while (last > first && (text[last - 1u] == ' ' || text[last - 1u] == '\t'))
        --last;
    return text.SubView(first, last - first);
}

/** 次の1行をCRと前後空白を除いて返す。 */
bool ReadLine_Internal(FStringView text, usize& cursor, FStringView& out_line) noexcept
{
    if (cursor >= text.Size()) return false;
    /** 行の先頭位置。 */
    const usize first = cursor;
    while (cursor < text.Size() && text[cursor] != '\n')
        ++cursor;
    /** 改行を除いた行末の次。 */
    usize last = cursor;
    if (cursor < text.Size()) ++cursor;
    if (last > first && text[last - 1u] == '\r') --last;
    out_line = TrimAscii_Internal(text.SubView(first, last - first));
    return true;
}

/** 引用符を使わないLightData行を固定長フィールドへ分解する。 */
bool SplitCsvRow_Internal(FStringView line, FStringView* fields, u32 capacity, u32& out_count) noexcept
{
    out_count = 0u;
    /** 現在のフィールド先頭。 */
    usize first = 0u;
    for (usize index = 0u; index <= line.Size(); ++index) {
        if (index < line.Size() && line[index] == '"') return false;
        if (index < line.Size() && line[index] != ',') continue;
        if (out_count >= capacity) return false;
        fields[out_count++] = TrimAscii_Internal(line.SubView(first, index - first));
        first = index + 1u;
    }
    return true;
}

/** CSV行の先頭フィールドだけを返す。 */
FStringView RowKey_Internal(FStringView line) noexcept
{
    /** 最初の区切り位置。 */
    const usize comma = line.Find(',');
    return TrimAscii_Internal(comma == FStringView::kNpos ? line : line.SubView(0u, comma));
}

/** 長さ付きASCII小数をロケールやNUL終端へ依存せず単精度値へ変換する。 */
bool ParseFloat_Internal(FStringView field, f32& out_value) noexcept
{
    field = TrimAscii_Internal(field);
    if (field.IsEmpty()) return false;

    /** 現在読む文字位置。 */
    usize index = 0u;
    /** 負号があったか。 */
    bool negative = false;
    if (field[index] == '+' || field[index] == '-') {
        negative = field[index] == '-';
        ++index;
    }

    /** 10進仮数。 */
    f64 value = 0.0;
    /** 整数部または小数部に数字があったか。 */
    bool has_digit = false;
    while (index < field.Size() && field[index] >= '0' && field[index] <= '9') {
        has_digit = true;
        value = value * 10.0 + static_cast<f64>(field[index] - '0');
        if (value > static_cast<f64>(kMaximumFiniteFloat)) return false;
        ++index;
    }

    if (index < field.Size() && field[index] == '.') {
        ++index;
        /** 次の小数桁へ進む倍率。 */
        f64 fraction_scale = 0.1;
        while (index < field.Size() && field[index] >= '0' && field[index] <= '9') {
            has_digit = true;
            value += static_cast<f64>(field[index] - '0') * fraction_scale;
            fraction_scale *= 0.1;
            ++index;
        }
    }
    if (!has_digit) return false;

    /** 10進指数。 */
    i32 exponent = 0;
    if (index < field.Size() && (field[index] == 'e' || field[index] == 'E')) {
        ++index;
        /** 指数が負か。 */
        bool exponent_negative = false;
        if (index < field.Size() && (field[index] == '+' || field[index] == '-')) {
            exponent_negative = field[index] == '-';
            ++index;
        }
        /** 指数部に数字があったか。 */
        bool has_exponent_digit = false;
        while (index < field.Size() && field[index] >= '0' && field[index] <= '9') {
            has_exponent_digit = true;
            if (exponent > 100) return false;
            exponent = exponent * 10 + static_cast<i32>(field[index] - '0');
            ++index;
        }
        if (!has_exponent_digit) return false;
        if (exponent_negative) exponent = -exponent;
    }
    if (index != field.Size() || exponent < -45 || exponent > 38) return false;

    if (exponent > 0) {
        for (i32 step = 0; step < exponent; ++step) {
            value *= 10.0;
            if (value > static_cast<f64>(kMaximumFiniteFloat)) return false;
        }
    } else {
        for (i32 step = 0; step > exponent; --step)
            value *= 0.1;
    }

    if (negative) value = -value;
    if (value < -static_cast<f64>(kMaximumFiniteFloat) || value > static_cast<f64>(kMaximumFiniteFloat)) return false;
    out_value = static_cast<f32>(value);
    return out_value == out_value && out_value >= -kMaximumFiniteFloat && out_value <= kMaximumFiniteFloat;
}

/** 3成分の最大値を返す。 */
f32 MaximumComponent_Internal(FVec3 value) noexcept
{
    /** XとYの大きい方。 */
    const f32 xy = value.x > value.y ? value.x : value.y;
    return xy > value.z ? xy : value.z;
}

/** 角度差を-180～180度へ折り返す。 */
f32 WrappedAngleDifference_Internal(f32 first_degrees, f32 second_degrees) noexcept
{
    /** 折り返す前の角度差。 */
    f32 difference = Mod(first_degrees - second_degrees, 360.0f);
    if (difference > 180.0f) difference -= 360.0f;
    if (difference < -180.0f) difference += 360.0f;
    return difference;
}

/** 値が指定した有限範囲内か返す。 */
bool IsFiniteRange_Internal(f32 value, f32 minimum, f32 maximum) noexcept
{
    return value == value && value >= minimum && value <= maximum;
}

/** sRGB列見出しが公式の固定順か返す。 */
bool IsSrgbHeader_Internal(const FStringView* fields, u32 count) noexcept
{
    if (count != 5u || !fields[0].IsEmpty()) return false;
    /** 色列が線形sRGBの固定順か。 */
    const bool colors_match = fields[1].Equals("R") && fields[2].Equals("G") && fields[3].Equals("B");
    return colors_match && fields[4].Equals("scale");
}

/** 3色と倍率を持つ行を読み取る。 */
bool ParseColorRow_Internal(const FStringView* fields, u32 count, FVec3& out_rgb, f32& out_scale) noexcept
{
    if (count != 5u) return false;
    /** 失敗時に出力を変更しない一時色。 */
    FVec3 parsed_rgb{};
    /** 失敗時に出力を変更しない一時倍率。 */
    f32 parsed_scale = 0.0f;
    /** 全数値を読み取れたか。 */
    const bool parsed_red = ParseFloat_Internal(fields[1], parsed_rgb.x);
    const bool parsed_green = ParseFloat_Internal(fields[2], parsed_rgb.y);
    const bool parsed_blue = ParseFloat_Internal(fields[3], parsed_rgb.z);
    const bool parsed_scale_value = ParseFloat_Internal(fields[4], parsed_scale);
    if (!parsed_red || !parsed_green || !parsed_blue || !parsed_scale_value) return false;
    out_rgb = parsed_rgb;
    out_scale = parsed_scale;
    return true;
}

/** 公式の誤記と将来の修正版のどちらの正規化色行名か返す。 */
bool IsNormalizedColorKey_Internal(FStringView key) noexcept
{
    return key.Equals("normarized color illminance") || key.Equals("normalized color illuminance");
}

/** 公式LightData内の既知行を一時状態へ読み取る。 */
bool ParseKnownRow_Internal(ETrueHdriCsvSection section, FStringView line, FTrueHdriCsvState& state) noexcept
{
    /** 行の識別名。 */
    const FStringView key = RowKey_Internal(line);
    /** 固定最大数のCSVフィールド。 */
    FStringView fields[5]{};
    /** 実際に得たフィールド数。 */
    u32 field_count = 0u;
    /** 太陽方向区画の行か。 */
    const bool is_direction = section == ETrueHdriCsvSection::Direction;
    /** sRGB区画の行か。 */
    const bool is_srgb = section == ETrueHdriCsvSection::Srgb;

    if (is_direction && key.Equals("photoshopXY(panorama uv[%])")) {
        /** 重複せず3列へ分解できたか。 */
        const bool row_ready = !state.HasPanoramaUv && SplitCsvRow_Internal(line, fields, 5u, field_count);
        if (!row_ready || field_count != 3u) return false;
        /** 失敗時に状態を変更しない一時UV。 */
        FVec2 parsed_uv{};
        /** UとVを両方読み取れたか。 */
        const bool parsed_u = ParseFloat_Internal(fields[1], parsed_uv.x);
        const bool parsed_v = ParseFloat_Internal(fields[2], parsed_uv.y);
        if (!parsed_u || !parsed_v) return false;
        state.PanoramaUvPercent = parsed_uv;
        state.HasPanoramaUv = true;
    } else if (is_direction && key.Equals("altitude_azimuth")) {
        /** 重複せず3列へ分解できたか。 */
        const bool row_ready = !state.HasAngles && SplitCsvRow_Internal(line, fields, 5u, field_count);
        if (!row_ready || field_count != 3u) return false;
        /** 失敗時に状態を変更しない一時高度。 */
        f32 parsed_altitude = 0.0f;
        /** 失敗時に状態を変更しない一時方位。 */
        f32 parsed_azimuth = 0.0f;
        /** 高度と方位を両方読み取れたか。 */
        const bool parsed_altitude_ok = ParseFloat_Internal(fields[1], parsed_altitude);
        const bool parsed_azimuth_ok = ParseFloat_Internal(fields[2], parsed_azimuth);
        if (!parsed_altitude_ok || !parsed_azimuth_ok) return false;
        state.AltitudeDegrees = parsed_altitude;
        state.AzimuthDegrees = parsed_azimuth;
        state.HasAngles = true;
    } else if (is_srgb && key.IsEmpty()) {
        /** 重複せず列見出しへ分解できたか。 */
        const bool row_ready = !state.HasSrgbHeader && SplitCsvRow_Internal(line, fields, 5u, field_count);
        if (!row_ready || !IsSrgbHeader_Internal(fields, field_count)) return false;
        state.HasSrgbHeader = true;
    } else if (is_srgb && key.Equals("color illuminance")) {
        /** 重複せず色照度行へ分解できたか。 */
        const bool row_ready = !state.HasColorIlluminance && SplitCsvRow_Internal(line, fields, 5u, field_count);
        if (!row_ready) return false;
        /** 失敗時に状態を変更しない一時色。 */
        FVec3 parsed_color{};
        /** 失敗時に状態を変更しない一時倍率。 */
        f32 parsed_scale = 0.0f;
        if (!ParseColorRow_Internal(fields, field_count, parsed_color, parsed_scale)) return false;
        state.ColorIlluminance = parsed_color;
        state.ColorScale = parsed_scale;
        state.HasColorIlluminance = true;
    } else if (is_srgb && IsNormalizedColorKey_Internal(key)) {
        /** 重複せず正規化色行へ分解できたか。 */
        const bool row_ready = !state.HasNormalizedColor && SplitCsvRow_Internal(line, fields, 5u, field_count);
        if (!row_ready) return false;
        /** 失敗時に状態を変更しない一時色。 */
        FVec3 parsed_color{};
        /** 失敗時に状態を変更しない一時尺度。 */
        f32 parsed_scale = 0.0f;
        if (!ParseColorRow_Internal(fields, field_count, parsed_color, parsed_scale)) return false;
        state.NormalizedColor = parsed_color;
        state.NormalizedScale = parsed_scale;
        state.HasNormalizedColor = true;
    }
    return true;
}

/** 必須行と各行の物理的・座標的な整合を検証し、倍率適用済み色照度を返す。 */
TResult<FVec3> ValidateState_Internal(const FTrueHdriCsvState& state) noexcept
{
    /** 方向を解決できる必須行が揃っているか。 */
    const bool has_direction = state.HasPanoramaUv && state.HasAngles;
    if (!has_direction) return ACS_ERR(Asset, 830, "TrueHDRIの方向行が不足しています");
    /** sRGB色照度を解決できる必須行が揃っているか。 */
    const bool has_srgb = state.HasSrgbHeader && state.HasColorIlluminance && state.HasNormalizedColor;
    if (!has_srgb) return ACS_ERR(Asset, 831, "TrueHDRIのsRGB色照度行が不足しています");
    /** 各方向値が公式形式の有限範囲内か。 */
    const bool valid_u = IsFiniteRange_Internal(state.PanoramaUvPercent.x, 0.0f, 100.0f);
    const bool valid_v = IsFiniteRange_Internal(state.PanoramaUvPercent.y, 0.0f, 100.0f);
    const bool valid_altitude = IsFiniteRange_Internal(state.AltitudeDegrees, -90.0f, 90.0f);
    const bool valid_azimuth = IsFiniteRange_Internal(state.AzimuthDegrees, -360.0f, 360.0f);
    if (!valid_u || !valid_v || !valid_altitude || !valid_azimuth)
        return ACS_ERR(Asset, 832, "TrueHDRIの方向値が範囲外です");

    /** 画像V座標から独立に復元した太陽高度。 */
    const f32 altitude_from_panorama = (50.0f - state.PanoramaUvPercent.y) * 1.8f;
    /** 画像U座標から独立に復元した太陽方位。 */
    const f32 azimuth_from_panorama = (state.PanoramaUvPercent.x - 50.0f) * 3.6f;
    /** 画像座標と角度行の高度差。 */
    const f32 altitude_difference = Abs(altitude_from_panorama - state.AltitudeDegrees);
    /** 経度継ぎ目を折り返した方位差。 */
    const f32 azimuth_difference = Abs(WrappedAngleDifference_Internal(azimuth_from_panorama, state.AzimuthDegrees));
    if (altitude_difference > kDirectionAgreementDegrees || azimuth_difference > kDirectionAgreementDegrees)
        return ACS_ERR(Asset, 833, "TrueHDRIの画像座標と太陽角度が一致しません");

    /** raw行の倍率が正の有限値か。 */
    const bool valid_color_scale = IsFiniteRange_Internal(state.ColorScale, 0.0f, kMaximumIlluminanceLux) &&
                                   state.ColorScale > 0.0f;
    /** raw行の各色成分が非負の有限値か。 */
    const bool valid_red = IsFiniteRange_Internal(state.ColorIlluminance.x, 0.0f, kMaximumIlluminanceLux);
    const bool valid_green = IsFiniteRange_Internal(state.ColorIlluminance.y, 0.0f, kMaximumIlluminanceLux);
    const bool valid_blue = IsFiniteRange_Internal(state.ColorIlluminance.z, 0.0f, kMaximumIlluminanceLux);
    if (!valid_color_scale || !valid_red || !valid_green || !valid_blue)
        return ACS_ERR(Asset, 834, "TrueHDRIの色照度が範囲外です");

    /** 行倍率を適用した赤色照度。 */
    const f32 red_lux = state.ColorIlluminance.x * state.ColorScale;
    /** 行倍率を適用した緑色照度。 */
    const f32 green_lux = state.ColorIlluminance.y * state.ColorScale;
    /** 行倍率を適用した青色照度。 */
    const f32 blue_lux = state.ColorIlluminance.z * state.ColorScale;
    /** 行倍率を適用した線形sRGB色照度。 */
    const FVec3 color_illuminance_lux{red_lux, green_lux, blue_lux};
    /** 正規化の基準となる最大色照度。 */
    const f32 peak_illuminance_lux = MaximumComponent_Internal(color_illuminance_lux);
    /** 倍率適用後の各成分と最大値が安全か。 */
    const bool valid_scaled_red = IsFiniteRange_Internal(red_lux, 0.0f, kMaximumIlluminanceLux);
    const bool valid_scaled_green = IsFiniteRange_Internal(green_lux, 0.0f, kMaximumIlluminanceLux);
    const bool valid_scaled_blue = IsFiniteRange_Internal(blue_lux, 0.0f, kMaximumIlluminanceLux);
    if (!valid_scaled_red || !valid_scaled_green || !valid_scaled_blue || peak_illuminance_lux <= 0.0f)
        return ACS_ERR(Asset, 834, "TrueHDRIの色照度倍率が範囲外です");

    /** 正規化色の各成分が0～1の丸め許容範囲内か。 */
    const bool valid_normalized_red = IsFiniteRange_Internal(state.NormalizedColor.x, 0.0f, 1.001f);
    const bool valid_normalized_green = IsFiniteRange_Internal(state.NormalizedColor.y, 0.0f, 1.001f);
    const bool valid_normalized_blue = IsFiniteRange_Internal(state.NormalizedColor.z, 0.0f, 1.001f);
    /** 正規化行の照度尺度が正の有限値か。 */
    const bool valid_normalized_scale = IsFiniteRange_Internal(state.NormalizedScale, 0.0f, kMaximumIlluminanceLux) &&
                                        state.NormalizedScale > 0.0f;
    if (!valid_normalized_red || !valid_normalized_green || !valid_normalized_blue || !valid_normalized_scale)
        return ACS_ERR(Asset, 835, "TrueHDRIの正規化色照度が範囲外です");

    /** raw行から復元した単位色。 */
    const f32 expected_red = red_lux / peak_illuminance_lux;
    /** raw行から復元した緑単位色。 */
    const f32 expected_green = green_lux / peak_illuminance_lux;
    /** raw行から復元した青単位色。 */
    const f32 expected_blue = blue_lux / peak_illuminance_lux;
    /** 公式CSVが小数3桁へ丸めることを考慮した色差上限。 */
    constexpr f32 kNormalizedTolerance = 0.002f;
    /** 照度尺度の丸め差上限。 */
    const f32 scale_tolerance = peak_illuminance_lux * 0.00002f + 0.01f;
    /** raw行と正規化行の各色差が丸め許容内か。 */
    const bool red_matches = Abs(expected_red - state.NormalizedColor.x) <= kNormalizedTolerance;
    const bool green_matches = Abs(expected_green - state.NormalizedColor.y) <= kNormalizedTolerance;
    const bool blue_matches = Abs(expected_blue - state.NormalizedColor.z) <= kNormalizedTolerance;
    const bool scale_matches = Abs(peak_illuminance_lux - state.NormalizedScale) <= scale_tolerance;
    if (!red_matches || !green_matches || !blue_matches || !scale_matches)
        return ACS_ERR(Asset, 836, "TrueHDRIの色照度行どうしが一致しません");
    return color_illuminance_lux;
}

} // namespace

FTrueHdriLightData::FTrueHdriLightData(FVec2 uv_percent, f32 altitude, f32 azimuth, FVec3 color_lux) noexcept
    : m_PanoramaUvPercent(uv_percent),
      m_AltitudeDegrees(altitude),
      m_AzimuthDegrees(azimuth),
      m_ColorIlluminanceLux(color_lux)
{
}

TResult<FTrueHdriLightData> FTrueHdriLightData::ParseSrgbCsv(const char* csv_data, usize csv_size) noexcept
{
    /** 入力ポインターとバイト数が走査可能か。 */
    const bool valid_input = csv_data != nullptr && csv_size > 0u && csv_size <= kMaximumCsvBytes;
    if (!valid_input) return ACS_ERR(Asset, 828, "TrueHDRI CSVの入力サイズが不正です");
    for (usize index = 0u; index < csv_size; ++index) {
        if (csv_data[index] == '\0') return ACS_ERR(Asset, 829, "TrueHDRI CSVに埋め込みNULがあります");
    }

    /** UTF-8 BOMを除いた先頭位置。 */
    usize text_offset = 0u;
    /** UTF-8 BOMが先頭にあるか。 */
    const bool has_bom_prefix = csv_size >= 3u && static_cast<u8>(csv_data[0]) == 0xefu;
    const bool has_utf8_bom = has_bom_prefix && static_cast<u8>(csv_data[1]) == 0xbbu &&
                              static_cast<u8>(csv_data[2]) == 0xbfu;
    if (has_utf8_bom) text_offset = 3u;
    /** 実際に行走査するCSV本文。 */
    const FStringView text{csv_data + text_offset, csv_size - text_offset};
    /** 現在の区画。 */
    ETrueHdriCsvSection section = ETrueHdriCsvSection::Other;
    /** 検証前の一時測定値。 */
    FTrueHdriCsvState state{};
    /** 次に読むバイト位置。 */
    usize cursor = 0u;
    /** 現在行。 */
    FStringView line{};

    while (ReadLine_Internal(text, cursor, line)) {
        if (line.IsEmpty()) continue;
        if (line.Equals("----Light direction----")) {
            section = ETrueHdriCsvSection::Direction;
            continue;
        }
        if (line.Equals("----Light color sRGB----")) {
            section = ETrueHdriCsvSection::Srgb;
            continue;
        }
        if (line.StartsWith("----") && line.EndsWith("----")) {
            section = ETrueHdriCsvSection::Other;
            continue;
        }
        if (!ParseKnownRow_Internal(section, line, state)) return ACS_ERR(Asset, 837, "TrueHDRI CSVの既知行が不正です");
    }
    /** 全必須行の相互検証と倍率適用結果。 */
    TResult<FVec3> validated_color = ValidateState_Internal(state);
    if (validated_color.IsErr()) return validated_color.Error();
    /** 非公開コンストラクターへ渡す検証済みUV。 */
    const FVec2 uv = state.PanoramaUvPercent;
    /** 非公開コンストラクターへ渡す検証済み高度。 */
    const f32 altitude = state.AltitudeDegrees;
    /** 非公開コンストラクターへ渡す検証済み方位。 */
    const f32 azimuth = state.AzimuthDegrees;
    /** 非公開コンストラクターへ渡す検証済み色照度。 */
    const FVec3 color = validated_color.Value();
    return FTrueHdriLightData{uv, altitude, azimuth, color};
}

f32 FTrueHdriLightData::PeakIlluminanceLux() const noexcept
{
    return MaximumComponent_Internal(m_ColorIlluminanceLux);
}

TResult<FVec3> FTrueHdriLightData::ResolveSunDirection(f32 environment_yaw_degrees) const noexcept
{
    /** 三角関数の精度を維持できる有限な回転角か。 */
    const bool valid_yaw = IsFiniteRange_Internal(environment_yaw_degrees, -1000000.0f, 1000000.0f);
    if (!valid_yaw) return ACS_ERR(Math, 838, "TrueHDRIの環境回転角が不正です");
    /** 周回を除いた太陽方位。 */
    const f32 azimuth_radians = ToRadians(Mod(m_AzimuthDegrees + environment_yaw_degrees, 360.0f));
    /** 太陽高度のラジアン値。 */
    const f32 altitude_radians = ToRadians(m_AltitudeDegrees);
    /** 水平面へ射影した方向の長さ。 */
    const f32 horizontal_length = Cos(altitude_radians);
    /** 表面から太陽へ向かうX成分。 */
    const f32 x = Sin(azimuth_radians) * horizontal_length;
    /** 表面から太陽へ向かうY成分。 */
    const f32 y = Sin(altitude_radians);
    /** 表面から太陽へ向かうZ成分。 */
    const f32 z = Cos(azimuth_radians) * horizontal_length;
    return FVec3{x, y, z};
}

TResult<FVec3> FTrueHdriLightData::ResolveDirectionalLightColor(f32 ref_lux, f32 ref_intensity) const noexcept
{
    /** 基準照度が正の有限値か。 */
    const bool valid_reference_lux = IsFiniteRange_Internal(ref_lux, 0.0f, kMaximumFiniteFloat) && ref_lux > 0.0f;
    /** ACS側の基準強度が非負の有限値か。 */
    const bool valid_reference_intensity = IsFiniteRange_Internal(ref_intensity, 0.0f, kMaximumFiniteFloat);
    if (!valid_reference_lux || !valid_reference_intensity)
        return ACS_ERR(Math, 839, "TrueHDRIの照度換算基準が不正です");
    /** 測定照度をACS強度へ移す倍率。 */
    const f64 measured_peak = static_cast<f64>(PeakIlluminanceLux());
    /** ACS側の基準強度を倍精度へ広げた値。 */
    const f64 reference_intensity = static_cast<f64>(ref_intensity);
    /** 基準照度を倍精度へ広げた値。 */
    const f64 reference_lux = static_cast<f64>(ref_lux);
    /** 測定照度をACS強度へ移す倍率。 */
    const f64 render_scale = measured_peak * reference_intensity / reference_lux;
    /** 単精度の光色として返せる倍率か。 */
    const bool valid_render_scale = render_scale >= 0.0 && render_scale <= static_cast<f64>(kMaximumFiniteFloat);
    if (!valid_render_scale) return ACS_ERR(Math, 840, "TrueHDRIの照度換算結果が表現範囲外です");
    /** 色だけを保つための測定照度逆数。 */
    const f32 inverse_peak = 1.0f / PeakIlluminanceLux();
    /** ACSへ渡す単位なし強度。 */
    const f32 intensity = static_cast<f32>(render_scale);
    /** 換算済みの赤成分。 */
    const f32 red = m_ColorIlluminanceLux.x * inverse_peak * intensity;
    /** 換算済みの緑成分。 */
    const f32 green = m_ColorIlluminanceLux.y * inverse_peak * intensity;
    /** 換算済みの青成分。 */
    const f32 blue = m_ColorIlluminanceLux.z * inverse_peak * intensity;
    return FVec3{red, green, blue};
}

} // namespace acs
