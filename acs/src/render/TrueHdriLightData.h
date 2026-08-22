// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_RENDER_TRUE_HDRI_LIGHT_DATA_H
#define ACS_RENDER_TRUE_HDRI_LIGHT_DATA_H

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs {

/** TrueHDRIのLightDataから得た、画像と直射光を一致させる測定値。 */
class FTrueHdriLightData {
public:
    /** 値を複製する。 */
    FTrueHdriLightData(const FTrueHdriLightData&) noexcept = default;

    /** 値を移動する。 */
    FTrueHdriLightData(FTrueHdriLightData&&) noexcept = default;

    /** 値を複製代入する。 */
    FTrueHdriLightData& operator=(const FTrueHdriLightData&) noexcept = default;

    /** 値を移動代入する。 */
    FTrueHdriLightData& operator=(FTrueHdriLightData&&) noexcept = default;

    /**
     * TrueHDRIのLightData CSVからsRGB測定値を読み取る。
     *
     * @param csv_data CSV全体の先頭。NUL終端は不要。
     * @param csv_size CSVのバイト数。
     * @return 必須行が一意で数値と画像座標が整合すれば測定値、それ以外はAssetエラー。
     */
    static TResult<FTrueHdriLightData> ParseSrgbCsv(const char* csv_data, usize csv_size) noexcept;

    /** 画像内の太陽中心U座標を0～100%で返す。 */
    f32 PanoramaUPercent() const noexcept
    {
        return m_PanoramaUvPercent.x;
    }

    /** 画像内の太陽中心V座標を0～100%で返す。 */
    f32 PanoramaVPercent() const noexcept
    {
        return m_PanoramaUvPercent.y;
    }

    /** 太陽高度を度で返す。 */
    f32 AltitudeDegrees() const noexcept
    {
        return m_AltitudeDegrees;
    }

    /** 太陽方位を度で返す。0度は+Z、90度は+Xを指す。 */
    f32 AzimuthDegrees() const noexcept
    {
        return m_AzimuthDegrees;
    }

    /** ガンマ変換していない線形sRGB色照度を返す。 */
    FVec3 ColorIlluminanceLux() const noexcept
    {
        return m_ColorIlluminanceLux;
    }

    /** 線形sRGB色照度の最大成分を測定基準照度として返す。 */
    f32 PeakIlluminanceLux() const noexcept;

    /**
     * HDR画像へ加えるY軸回転と同じ回転を適用し、表面から太陽へ向かう単位方向を求める。
     *
     * @param environment_yaw_degrees HDR画像へ加えるY軸回転角。正なら+Zから+Xへ回す。
     * @return 有限で実用範囲内の角度なら太陽方向、それ以外はMathエラー。
     */
    TResult<FVec3> ResolveSunDirection(f32 environment_yaw_degrees) const noexcept;

    /**
     * 測定照度をACSの単位なし平行光強度へ明示的に換算する。
     *
     * @param ref_lux ref_intensityに対応させる基準照度。
     * @param ref_intensity 基準照度時にACSへ渡す平行光強度。
     * @return 入力が有限かつ基準照度が正なら線形光色、それ以外はMathエラー。
     */
    TResult<FVec3> ResolveDirectionalLightColor(f32 ref_lux, f32 ref_intensity) const noexcept;

private:
    /** 検証済みの測定値からのみ構築する。 */
    FTrueHdriLightData(FVec2 uv_percent, f32 altitude, f32 azimuth, FVec3 color_lux) noexcept;

    /** 太陽中心のパノラマUV座標。単位は%で、左上が0。 */
    FVec2 m_PanoramaUvPercent{};

    /** 地平線を0度とした太陽高度。 */
    f32 m_AltitudeDegrees = 0.0f;

    /** +Zを0度、+Xを90度とした太陽方位。 */
    f32 m_AzimuthDegrees = 0.0f;

    /** ガンマ変換していない線形sRGB色照度。 */
    FVec3 m_ColorIlluminanceLux{};
};

} // namespace acs

#endif // ACS_RENDER_TRUE_HDRI_LIGHT_DATA_H
