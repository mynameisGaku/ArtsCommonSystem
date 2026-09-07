// SPDX-License-Identifier: Apache-2.0
#include "foundation/Log.h"
#include "foundation/Platform.h"
#include "math/Camera.h"
#include "memory/MemorySystem.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiDevice.h"
#include "render/IRhiTexture.h"
#include "render/Sky.h"
#include <cstdio>

using namespace acs;

namespace {

// 形状の原因を分離する診断寸法。実画面の画質・速度の受入れ寸法ではない。
constexpr u32 kWidth = 320u;
constexpr u32 kHeight = 180u;
constexpr u32 kPixels = kWidth * kHeight;
// 大きな読戻し領域をスタックへ置かない。単一実行中だけ使う診断用領域。
FVec4 g_Color[kPixels]{};
FVec2 g_Depth[kPixels]{};
FVec2 g_NativeDepth[kPixels]{};
FVec2 g_PreviousDepth[kPixels]{};

// PFMを新規作成し、上書きを拒否する。行を下から保存し、GPUの上下向きを保つ。
bool WriteImage_Internal(const char* directory, u32 view, const char* mode, u32 frame, const char* channel, u32 componentCount) noexcept
{
    // 入力は実行者が指定する既存フォルダー。各画像名は固定の診断条件から作る。
    char path[2048]{};
    if (::sprintf_s(path, "%s/view%u_%s_%02u_%s.pfm", directory, view, mode, frame, channel) <= 0) return false;
    // 出力済み証拠を壊さず、既存ファイルなら明示的に失敗する。
    const HANDLE file = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char header[64]{};
    const int headerLength = ::sprintf_s(header, "%s\n%u %u\n-1.0\n", componentCount == 3u ? "PF" : "Pf", kWidth, kHeight);
    DWORD written = 0u;
    bool success = headerLength > 0 && ::WriteFile(file, header, static_cast<DWORD>(headerLength), &written, nullptr) && written == static_cast<DWORD>(headerLength);
    // RGBは黒背景への合成済み放射輝度。不透明度と距離は別画像で保存する。
    f32 row[kWidth * 3u]{};
    for (u32 y = kHeight; success && y > 0u; --y) {
        for (u32 x = 0u; x < kWidth; ++x) {
            const u32 pixel = (y - 1u) * kWidth + x;
            if (componentCount == 3u) {
                row[x * 3u] = g_Color[pixel].x;
                row[x * 3u + 1u] = g_Color[pixel].y;
                row[x * 3u + 2u] = g_Color[pixel].z;
            } else {
                row[x] = channel[0] == 'a' ? g_Depth[pixel].y : g_Depth[pixel].x;
            }
        }
        const DWORD rowBytes = kWidth * componentCount * sizeof(f32);
        success = ::WriteFile(file, row, rowBytes, &written, nullptr) && written == rowBytes;
    }
    return ::CloseHandle(file) && success;
}

// 同じ太陽・媒質・視線で等倍と時間再構成を比較する。生成、提出、読戻し失敗は非0を返す。
int RunProbe_Internal(const char* directory) noexcept
{
    FDeviceConfig deviceConfiguration{};
    auto deviceResult = CreateRhiDevice(deviceConfiguration);
    if (deviceResult.IsErr()) return 10;
    auto device = Move(deviceResult.Value());
    auto commandResult = CreateRhiCommandList(*device);
    if (commandResult.IsErr()) return 11;
    auto command = Move(commandResult.Value());
    CVolumetricClouds clouds;
    if (clouds.Init(*device, EFormat::R32G32B32A32_Float).IsErr()) return 12;
    clouds.SetLayer(FVolumetricCloudLayer{1500.0f, 4000.0f, 0.035f});
    FVolumetricCloudWeather weather{};
    weather.CloudTypeInfluence = 0.0f;
    weather.PrecipitationInfluence = 0.0f;
    clouds.SetWeather(weather);
    FVolumetricCloudRange range{};
    range.MaxDistance = 30000.0f;
    clouds.SetRange(range);
    FVolumetricCloudLighting lighting{};
    lighting.ViewExtinction = 1.0f;
    lighting.LightExtinction = 1.0f;
    lighting.SunScatteringLuminanceScale = 1.0f;
    lighting.PowderStrength = 0.30f;
    clouds.SetLighting(lighting);
    // 大気合成と色調変換を含めない。両モードには同じ有限の照明入力を使う。
    const FVec3 sun = Normalize(FVec3{0.3f, 0.8f, 0.4f});
    const FVec3 sunRadiance{22.0f, 22.0f, 22.0f};
    const FVec3 skyRadiance{0.7f, 0.8f, 0.95f};
    FTextureDesc colorDescription{};
    colorDescription.width = kWidth;
    colorDescription.height = kHeight;
    colorDescription.format = EFormat::R32G32B32A32_Float;
    colorDescription.is_render_target = true;
    auto color = CreateRhiTexture(*device, colorDescription);
    // 幾何遮蔽物を置かず、全画素を遠面深度1とする。
    const FVec4 sceneDepth{1.0f, 1.0f, 1.0f, 1.0f};
    FTextureDesc depthDescription{};
    depthDescription.width = 1u;
    depthDescription.height = 1u;
    depthDescription.format = EFormat::R32G32B32A32_Float;
    depthDescription.initial_data = &sceneDepth;
    depthDescription.initial_data_size = sizeof(sceneDepth);
    auto depth = CreateRhiTexture(*device, depthDescription);
    if (color.IsErr() || depth.IsErr()) return 13;
    ::printf("cloud_probe settings width=%u height=%u time=0 coverage=0.42 density=1.6 wind=1 reference=false atmosphere=false\n", kWidth, kHeight);
    ::fflush(stdout);
    for (u32 view = 0u; view < 4u; ++view) {
        // 既存の通常積雲プロジェクトと同じ軌道条件。view3も被覆率を増やさず検査する。
        const f32 pitch = view == 0u ? -1.20f : (view == 2u ? 1.45f : 0.0f);
        const f32 distance = view == 0u ? 2800.0f : (view == 2u ? 8500.0f : 200.0f);
        const FVec3 target = view == 3u ? FVec3{6500.0f, 2750.0f, 9500.0f} : FVec3{4000.0f, view == 2u ? 2500.0f : 2700.0f, 10000.0f};
        const FVec3 forward{0.0f, -Sin(pitch), Cos(pitch)};
        const FVec3 eye = target - forward * distance;
        CCamera camera;
        camera.SetLookDirection(eye, forward);
        camera.SetPerspective(55.0f * kDeg2Rad, static_cast<f32>(kWidth) / kHeight, 0.05f, distance * 200.0f + 1000.0f);
        const FMat4 inverse = BuildCameraRelativeInverseViewProjection(camera.View(), camera.Projection());
        for (u32 mode = 0u; mode < 2u; ++mode) {
            const char* name = mode == 0u ? "native" : "temporal";
            const u32 frames = mode == 0u ? 2u : 32u;
            // 参照モードは歩進や自己影も変えるため使わず、追跡画素だけを変える。
            if (!clouds.EnsureSize(*device, kWidth, kHeight, mode == 0u ? 4.0f : 1.0f, false)) return 14;
            clouds.InvalidateHistory();
            // 描画前の密度生成は正常な提出として進め、比較する描画回数には含めない。
            u32 preparationCount = 0u;
            for (u32 frame = 1u; frame <= frames;) {
                command->Begin();
                clouds.RenderComputeCameraRelative(*command, inverse, eye, sun, sunRadiance, skyRadiance, 0.42f, 1.6f, 1.0f, 0.0f);
                if (!clouds.RecordedCloudFramePending()) {
                    const auto reason = clouds.LastFrameWorkload().skip_reason;
                    const bool preparing = clouds.RecordedFramePending() && reason == EVolumetricCloudFrameSkipReason::PreparingDensityFields;
                    command->End();
                    const bool prepared = preparing && command->Submit();
                    clouds.ResolveRecordedFrameSubmission(prepared);
                    if (!prepared || ++preparationCount > 16u) return 15;
                    device->WaitIdle();
                    ::printf("cloud_probe preparation=%u\n", preparationCount);
                    ::fflush(stdout);
                    continue;
                }
                command->BeginRenderToTexture(*color.Value(), FClearColor{0.0f, 0.0f, 0.0f, 0.0f});
                clouds.Composite(*command, *depth.Value(), kWidth, kHeight);
                command->EndRenderToTexture(*color.Value());
                command->End();
                const bool submitted = command->Submit();
                clouds.ResolveRecordedFrameSubmission(submitted);
                if (!submitted || clouds.ResolvedDepth() == nullptr) return 16;
                device->WaitIdle();
                if (!device->ReadTexture(*clouds.ResolvedDepth(), g_Depth, sizeof(g_Depth)) || !device->ReadTexture(*color.Value(), g_Color, sizeof(g_Color))) return 17;
                // 比較値は画質合格のしきい値ではなく、原因切り分け用の観測値。
                f64 alphaSum = 0.0;
                f64 nativeError = 0.0;
                f64 frameError = 0.0;
                u32 opaquePixels = 0u;
                u32 nearPixels = 0u;
                for (u32 pixel = 0u; pixel < kPixels; ++pixel) {
                    const FVec2 value = g_Depth[pixel];
                    if (!(value.y >= 0.0f && value.y <= 1.0f && value.x >= 0.0f && value.x <= 250001.0f)) return 18;
                    const FVec4 rgb = g_Color[pixel];
                    if (!(rgb.x >= 0.0f && rgb.x <= 1.0e10f && rgb.y >= 0.0f && rgb.y <= 1.0e10f && rgb.z >= 0.0f && rgb.z <= 1.0e10f)) return 19;
                    alphaSum += value.y;
                    nativeError += Abs(value.y - g_NativeDepth[pixel].y);
                    if (frame > 1u) frameError += Abs(value.y - g_PreviousDepth[pixel].y);
                    opaquePixels += value.y > 0.99f ? 1u : 0u;
                    nearPixels += value.y > 0.5f && value.x < 100.0f ? 1u : 0u;
                    g_PreviousDepth[pixel] = value;
                    if (mode == 0u) g_NativeDepth[pixel] = value;
                }
                const auto& work = clouds.LastFrameWorkload();
                // 指定名ではなく実際の描画方策を検証し、3/4解像度を16位相と誤認しない。
                const u32 divisor = mode == 0u ? 1u : 4u;
                if (work.temporal_super_resolution != (mode != 0u) || work.trace_width != kWidth / divisor || work.trace_height != kHeight / divisor || work.composite_draws != 1u) return 21;
                ::printf("cloud_probe view=%u mode=%s frame=%u mean_alpha=%.8f opaque=%u near=%u native_mae=%.8f frame_mae=%.8f\n", view, name, frame, alphaSum / kPixels, opaquePixels, nearPixels, mode == 0u ? 0.0 : nativeError / kPixels, frameError / kPixels);
                if (frame == 1u) ::printf("cloud_probe work trace=%ux%u temporal=%u shadows=%u\n", work.trace_width, work.trace_height, work.temporal_super_resolution ? 1u : 0u, work.shadow_cache_dispatches);
                ::fflush(stdout);
                if (frame == 1u || frame == 2u || frame == 16u || frame == 32u) {
                    if (!WriteImage_Internal(directory, view, name, frame, "alpha", 1u) || !WriteImage_Internal(directory, view, name, frame, "distance", 1u) || !WriteImage_Internal(directory, view, name, frame, "radiance", 3u)) return 20;
                }
                ++frame;
            }
        }
    }
    clouds.Shutdown();
    return 0;
}

} // namespace

// 既存の空の出力フォルダーを一つ渡す。GPUが使えない場合も成功扱いにはしない。
int main(int argumentCount, char** arguments)
{
    if (argumentCount != 2) { ::fprintf(stderr, "usage: acs_cloud_render_probe existing-empty-output-directory\n"); return 2; }
    FLogConfig logging{};
    logging.console = true;
    logging.debug_output = false;
    CLogger::Init(logging);
    const auto memory = CMemorySystem::Init(CMemorySystem::DefaultConfig());
    const int result = memory.IsOk() ? RunProbe_Internal(arguments[1]) : 3;
    if (memory.IsOk()) CMemorySystem::Shutdown();
    ::printf("cloud_probe completed result=%d\n", result);
    CLogger::Shutdown();
    return result;
}
