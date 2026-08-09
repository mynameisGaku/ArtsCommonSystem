// SPDX-License-Identifier: Apache-2.0

#include "app/Application.h"
#include "asset/MeshAsset.h"
#include "foundation/Log.h"
#include "math/Camera.h"
#include "math/Math.h"
#include "memory/SharedPtr.h"
#include "render/Blit.h"
#include "render/Dx12/Dx12Device.h"
#include "render/Dx12/Dx12Swapchain.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiDevice.h"
#include "render/IRhiSwapchain.h"
#include "render/IRhiTexture.h"
#include "render/PostProcess.h"
#include "render/RenderAssets.h"
#include "render/RhiTypes.h"
#include "render/WaterSurface3D.h"

#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace acs;

/** evidence 行列の一つを表す固定実行モード。 */
enum class EFixtureMode {
    R0,
    B0,
    R1,
    B1,
    C1,
};

/** 起動引数から得た、1 fresh process 分の固定条件。 */
struct FFixtureInvocation {
    EFixtureMode mode = EFixtureMode::R0;
    std::string configuration;
    std::filesystem::path output_root;
};

/** raw DX12 adapter を識別して各 process 間の一致を検証する値。 */
struct FAdapterMetadata {
    char luid[32]{};
    char name[256]{};
    u32 vendor_id = 0u;
    u32 device_id = 0u;
};

constexpr u32 kViewportWidth = 512u;
constexpr u32 kViewportHeight = 288u;
constexpr u32 kWarmupFrameCount = 8u;
constexpr f32 kFixedDeltaSeconds = 1.0f / 60.0f;
constexpr f32 kTaaBlendFactor = 0.10f;
/** BGRA8 artifact の 1 pixel を構成する byte 数。 */
constexpr u32 kRawBytesPerPixel = 4u;
/** engine 設定値ではなく fixture 固有の Q/無 jitter 契約を表すラベル。 */
constexpr const char* kQualityQContractLabel = "fixture-contract:q1-no-jitter";
constexpr const char* kQualityQSource = "fixture-contract-label";
constexpr FVec3 kCameraPosition{0.0f, 4.4f, -8.2f};
constexpr FVec3 kCameraTarget{0.0f, -0.10f, 0.0f};
constexpr FVec3 kWakeStart{-2.4f, 0.0f, -0.75f};
constexpr FVec3 kWakeEnd{2.4f, 0.0f, 0.75f};

/** mode 名を artifact と manifest 用の安定文字列へ変換する。 */
const char* ModeName(EFixtureMode mode) noexcept
{
    switch (mode) {
    case EFixtureMode::R0:
        return "R0";
    case EFixtureMode::B0:
        return "B0";
    case EFixtureMode::R1:
        return "R1";
    case EFixtureMode::B1:
        return "B1";
    case EFixtureMode::C1:
        return "C1";
    }
    return "invalid";
}

/** 指定 mode が temporal history を有効にするか判定する。 */
bool UsesTaa(EFixtureMode mode) noexcept
{
    return mode == EFixtureMode::B0 || mode == EFixtureMode::B1 || mode == EFixtureMode::C1;
}

/** 指定 mode が wake を注入するか判定する。 */
bool InjectsWake(EFixtureMode mode) noexcept
{
    return mode == EFixtureMode::R1 || mode == EFixtureMode::B1 || mode == EFixtureMode::C1;
}

/** C1 だけで注入直前に history を無効化する。 */
bool InvalidatesHistoryBeforeWake(EFixtureMode mode) noexcept
{
    return mode == EFixtureMode::C1;
}

/** mode 文字列を enum に変換し、未知値は拒否する。 */
bool ParseMode(const wchar_t* value, EFixtureMode& output) noexcept
{
    if (std::wcscmp(value, L"R0") == 0) {
        output = EFixtureMode::R0;
        return true;
    }
    if (std::wcscmp(value, L"B0") == 0) {
        output = EFixtureMode::B0;
        return true;
    }
    if (std::wcscmp(value, L"R1") == 0) {
        output = EFixtureMode::R1;
        return true;
    }
    if (std::wcscmp(value, L"B1") == 0) {
        output = EFixtureMode::B1;
        return true;
    }
    if (std::wcscmp(value, L"C1") == 0) {
        output = EFixtureMode::C1;
        return true;
    }
    return false;
}

/** Debug/Release の構成名だけを受け取り、比較対象の構成を固定する。 */
bool ParseConfiguration(const wchar_t* value, std::string& output) noexcept
{
    if (std::wcscmp(value, L"Debug") == 0) {
        output = "Debug";
        return true;
    }
    if (std::wcscmp(value, L"Release") == 0) {
        output = "Release";
        return true;
    }
    return false;
}

/** --mode、--configuration、--out を過不足なく受け取り、曖昧な起動を拒否する。 */
bool ParseInvocation(FFixtureInvocation& output) noexcept
{
    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == nullptr) return false;

    bool found_mode = false;
    bool found_configuration = false;
    bool found_output = false;
    bool valid = true;
    for (int index = 1; index < argument_count && valid; ++index) {
        const wchar_t* argument = arguments[index];
        if (std::wcscmp(argument, L"--mode") == 0) {
            if (found_mode || index + 1 >= argument_count || !ParseMode(arguments[++index], output.mode)) {
                valid = false;
                continue;
            }
            found_mode = true;
        } else if (std::wcscmp(argument, L"--configuration") == 0) {
            if (found_configuration || index + 1 >= argument_count || !ParseConfiguration(arguments[++index], output.configuration)) {
                valid = false;
                continue;
            }
            found_configuration = true;
        } else if (std::wcscmp(argument, L"--out") == 0) {
            if (found_output || index + 1 >= argument_count) {
                valid = false;
                continue;
            }
            output.output_root = std::filesystem::path(arguments[++index]);
            found_output = !output.output_root.empty();
        } else {
            valid = false;
        }
    }
    LocalFree(arguments);
    return valid && found_mode && found_configuration && found_output;
}

/** 新規 artifact root が存在し、空の通常 directory であることを確認する。 */
bool IsEmptyOutputDirectory(const std::filesystem::path& path) noexcept
{
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error || !std::filesystem::is_directory(path, error) || error) {
        return false;
    }
    std::filesystem::directory_iterator iterator(path, error);
    return !error && iterator == std::filesystem::directory_iterator{};
}

/** BGRA8 の viewport 寸法を密な raw byte 数へ安全に変換する。 */
bool ExpectedRawByteCount(u32 width, u32 height, usize& output) noexcept
{
    const u64 byte_count = static_cast<u64>(width) * static_cast<u64>(height) * kRawBytesPerPixel;
    if (width == 0u || height == 0u || byte_count > static_cast<u64>(std::numeric_limits<usize>::max())) {
        return false;
    }
    output = static_cast<usize>(byte_count);
    return true;
}

/** JSON 文字列へ必要最小限のエスケープを適用する。 */
std::string EscapeJson(const char* source)
{
    std::string result;
    if (source == nullptr) return result;
    for (const char* character = source; *character != '\0'; ++character) {
        switch (*character) {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result += *character;
            break;
        }
    }
    return result;
}

/** 有限 float を locale 非依存の JSON 数値へ変換する。 */
std::string JsonNumber(f32 value)
{
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    return std::string(buffer);
}

/** 生成済み raw frame を byte 単位で書き出す。 */
bool WriteBinaryFile(const std::filesystem::path& path, const std::vector<u8>& bytes) noexcept
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) return false;
    if (!bytes.empty()) stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}

/** metadata の UTF-8 JSON を空 root へ一度だけ書き出す。 */
bool WriteTextFile(const std::filesystem::path& path, const std::string& text) noexcept
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) return false;
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return stream.good();
}

/** AMeshAsset の local XZ grid を fixture 専用の固定解像度で構築する。 */
TSharedPtr<AMeshAsset> MakeEvidenceWaterGrid() noexcept
{
    constexpr u32 kColumns = 96u;
    constexpr u32 kRows = 96u;
    constexpr f32 kExtent = 7.0f;
    auto mesh = MakeShared<AMeshAsset>();
    if (!mesh) return TSharedPtr<AMeshAsset>();

    auto& vertices = mesh->Vertices();
    auto& indices = mesh->Indices();
    vertices.Reserve((kColumns + 1u) * (kRows + 1u));
    indices.Reserve(kColumns * kRows * 6u);
    for (u32 row = 0u; row <= kRows; ++row) {
        const f32 v = static_cast<f32>(row) / static_cast<f32>(kRows);
        for (u32 column = 0u; column <= kColumns; ++column) {
            const f32 u = static_cast<f32>(column) / static_cast<f32>(kColumns);
            FMeshVertex vertex{};
            vertex.position = FVec3{-kExtent + 2.0f * kExtent * u, 0.0f, -kExtent + 2.0f * kExtent * v};
            vertex.normal = FVec3{0.0f, 1.0f, 0.0f};
            vertex.u = u;
            vertex.v = 1.0f - v;
            vertices.Add(vertex);
        }
    }

    const u32 row_stride = kColumns + 1u;
    for (u32 row = 0u; row < kRows; ++row) {
        for (u32 column = 0u; column < kColumns; ++column) {
            const u32 top_left = row * row_stride + column;
            const u32 top_right = top_left + 1u;
            const u32 bottom_left = top_left + row_stride;
            const u32 bottom_right = bottom_left + 1u;
            indices.Add(top_left);
            indices.Add(top_right);
            indices.Add(bottom_right);
            indices.Add(top_left);
            indices.Add(bottom_right);
            indices.Add(bottom_left);
        }
    }

    FSubMesh submesh{};
    submesh.first_index = 0u;
    submesh.index_count = static_cast<u32>(indices.Num());
    mesh->SubMeshes().Add(submesh);
    return mesh;
}

/** raw DX12 device から LUID と PCI vendor/device を取得する。 */
bool QueryAdapterMetadata(CDx12Device& device, FAdapterMetadata& output) noexcept
{
    ID3D12Device* native_device = device.D3DDevice();
    IDXGIFactory6* factory = device.DxgiFactory();
    if (native_device == nullptr || factory == nullptr) return false;

    IDXGIAdapter1* adapter = nullptr;
    const LUID luid = native_device->GetAdapterLuid();
    if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) || adapter == nullptr) return false;

    DXGI_ADAPTER_DESC1 descriptor{};
    const HRESULT result = adapter->GetDesc1(&descriptor);
    adapter->Release();
    if (FAILED(result)) return false;

    std::snprintf(output.luid, sizeof(output.luid), "%08X:%08X", static_cast<u32>(descriptor.AdapterLuid.HighPart), descriptor.AdapterLuid.LowPart);
    output.vendor_id = descriptor.VendorId;
    output.device_id = descriptor.DeviceId;
    const int converted = WideCharToMultiByte(CP_UTF8, 0, descriptor.Description, -1, output.name, static_cast<int>(sizeof(output.name)), nullptr, nullptr);
    if (converted == 0) std::snprintf(output.name, sizeof(output.name), "%s", "unknown");
    return true;
}

/** final Present state の swapchain buffer を BGRA8 dense rows として同期 readback する。 */
bool ReadBackSwapchainBuffer(CDx12Device& device, FDx12Swapchain& swapchain, u32 buffer_index, std::vector<u8>& output) noexcept
{
    ID3D12Device* native_device = device.D3DDevice();
    ID3D12Resource* source = swapchain.BackBuffer(buffer_index);
    if (native_device == nullptr || source == nullptr) return false;

    const D3D12_RESOURCE_DESC source_description = source->GetDesc();
    /** dense BGRA8 readback が扱える resource 記述かを項目別に判定する。 */
    const bool has_supported_dimension = source_description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    const bool has_supported_format = source_description.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    const bool has_supported_samples = source_description.SampleDesc.Count == 1u;
    const bool has_supported_width = source_description.Width > 0u && source_description.Width <= std::numeric_limits<u32>::max();
    const bool has_supported_height = source_description.Height > 0u && source_description.Height <= std::numeric_limits<u32>::max();
    const bool has_supported_description = has_supported_dimension && has_supported_format && has_supported_samples && has_supported_width && has_supported_height;
    if (!has_supported_description) {
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT row_count = 0u;
    UINT64 row_bytes = 0u;
    UINT64 total_bytes = 0u;
    native_device->GetCopyableFootprints(&source_description, 0u, 1u, 0u, &footprint, &row_count, &row_bytes, &total_bytes);
    const u32 width = static_cast<u32>(source_description.Width);
    const u32 height = source_description.Height;
    usize expected_raw_byte_count = 0u;
    if (!ExpectedRawByteCount(width, height, expected_raw_byte_count)) {
        return false;
    }
    const u64 dense_row_bytes = static_cast<u64>(width) * kRawBytesPerPixel;
    const u64 dense_total_bytes = static_cast<u64>(expected_raw_byte_count);
    /** GPU footprint が全 dense row と process address space を満たすか判定する。 */
    const bool has_copy_storage = total_bytes > 0u;
    const bool has_all_rows = row_count >= height;
    const bool has_dense_source_rows = row_bytes >= dense_row_bytes && footprint.Footprint.RowPitch >= dense_row_bytes;
    const bool fits_process_address_space = dense_total_bytes <= static_cast<u64>(std::numeric_limits<usize>::max());
    const bool has_supported_footprint = has_copy_storage && has_all_rows && has_dense_source_rows && fits_process_address_space;
    if (!has_supported_footprint) {
        return false;
    }

    D3D12_HEAP_PROPERTIES heap_properties{};
    heap_properties.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_description{};
    readback_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_description.Width = total_bytes;
    readback_description.Height = 1u;
    readback_description.DepthOrArraySize = 1u;
    readback_description.MipLevels = 1u;
    readback_description.Format = DXGI_FORMAT_UNKNOWN;
    readback_description.SampleDesc.Count = 1u;
    readback_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* readback = nullptr;
    if (FAILED(native_device->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &readback_description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) || readback == nullptr) {
        return false;
    }

    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* command_list = nullptr;
    bool success = SUCCEEDED(native_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
                   SUCCEEDED(native_device->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&command_list)));
    if (success) {
        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = source;
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1u, &to_copy);

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION source_location{};
        source_location.pResource = source;
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source_location.SubresourceIndex = 0u;
        command_list->CopyTextureRegion(&destination, 0u, 0u, 0u, &source_location, nullptr);

        D3D12_RESOURCE_BARRIER to_present = to_copy;
        to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        command_list->ResourceBarrier(1u, &to_present);

        success = SUCCEEDED(command_list->Close());
        if (success) {
            const u64 fence_value = device.ExecuteOneOffGraphicsCommandList(command_list);
            success = fence_value != 0u;
            if (success) {
                device.WaitForFenceValue(fence_value);
            } else {
                command_list = nullptr;
                allocator = nullptr;
                readback = nullptr;
            }
        }
    }

    void* mapped = nullptr;
    D3D12_RANGE read_range{0u, static_cast<SIZE_T>(total_bytes)};
    if (success && SUCCEEDED(readback->Map(0u, &read_range, &mapped)) && mapped != nullptr) {
        output.resize(expected_raw_byte_count);
        const auto* source_bytes = static_cast<const u8*>(mapped) + static_cast<usize>(footprint.Offset);
        for (u32 row = 0u; row < height; ++row) {
            std::memcpy(output.data() + static_cast<usize>(row) * dense_row_bytes, source_bytes + static_cast<usize>(row) * footprint.Footprint.RowPitch, static_cast<usize>(dense_row_bytes));
        }
        D3D12_RANGE no_write{0u, 0u};
        readback->Unmap(0u, &no_write);
    } else {
        success = false;
    }

    if (command_list != nullptr) command_list->Release();
    if (allocator != nullptr) allocator->Release();
    if (readback != nullptr) readback->Release();
    return success;
}

/** matching input 条件だけで water TAA matrix の 1 cell を描画・capture するアプリ。 */
class CWaterTaaEvidenceApp final : public CApplication {
public:
    /** 1 mode の固定起動条件を保持して app を構築する。 */
    explicit CWaterTaaEvidenceApp(const FFixtureInvocation& invocation) noexcept
        : m_Invocation(invocation)
    {
    }

    /** capture と metadata の双方が成功したときだけ true を返す。 */
    bool Succeeded() const noexcept
    {
        return m_Succeeded;
    }

protected:
    /** GPU resource と不変 camera/water/post 条件を初期化する。 */
    void OnStart() noexcept override
    {
        IRhiDevice* device = GetRenderer().Device();
        IRhiSwapchain* swapchain = GetRenderer().Swapchain();
        if (device == nullptr || swapchain == nullptr) {
            Fail("renderer initialization failed");
            return;
        }
        if (std::strcmp(device->BackendName(), "DX12") != 0) {
            Fail("fixture requires raw DX12");
            return;
        }

        auto* dx12_device = static_cast<CDx12Device*>(device);
        if (!QueryAdapterMetadata(*dx12_device, m_Adapter)) {
            Fail("adapter metadata query failed");
            return;
        }

        const u32 width = swapchain->Width();
        const u32 height = swapchain->Height();
        if (width == 0u || height == 0u) {
            Fail("zero viewport");
            return;
        }

        auto post_result = m_Post.Init(*device, width, height, GetRenderer().ColorFormat());
        if (post_result.IsErr()) {
            Fail("post-process initialization failed");
            return;
        }
        auto water_result = m_Water.Init(*device, m_Post.HdrFormat(), GetRenderer().DepthFormat());
        if (water_result.IsErr()) {
            Fail("water initialization failed");
            return;
        }
        auto blit_result = m_Blit.Init(*device, m_Post.HdrFormat());
        if (blit_result.IsErr()) {
            Fail("blit initialization failed");
            return;
        }

        FTextureDesc scene_copy_description{};
        scene_copy_description.width = width;
        scene_copy_description.height = height;
        scene_copy_description.format = m_Post.HdrFormat();
        scene_copy_description.is_render_target = true;
        auto scene_copy_result = CreateRhiTexture(*device, scene_copy_description);
        if (scene_copy_result.IsErr()) {
            Fail("scene-copy allocation failed");
            return;
        }
        m_SceneCopy = Move(scene_copy_result.Value());

        auto mesh = MakeEvidenceWaterGrid();
        if (!mesh) {
            Fail("water mesh allocation failed");
            return;
        }
        auto upload_result = UploadMesh(*device, *mesh, m_WaterMesh);
        if (upload_result.IsErr()) {
            Fail("water mesh upload failed");
            return;
        }

        FWaterSurface3DParams water_parameters{};
        water_parameters.shallow_color = FVec3{0.050f, 0.355f, 0.480f};
        water_parameters.deep_color = FVec3{0.003f, 0.030f, 0.110f};
        water_parameters.absorption = FVec3{0.390f, 0.145f, 0.042f};
        water_parameters.scattering = FVec3{0.020f, 0.060f, 0.105f};
        water_parameters.phase_anisotropy = 0.62f;
        water_parameters.roughness = 0.090f;
        water_parameters.normal_strength = 0.88f;
        water_parameters.normal_tiling = 0.085f;
        water_parameters.refraction_strength = 0.72f;
        water_parameters.optical_depth = 1.15f;
        water_parameters.wave_amplitude = 0.105f;
        water_parameters.wave_scale = 0.88f;
        water_parameters.wave_speed = 0.72f;
        water_parameters.ripple_lifetime = 4.5f;
        water_parameters.ripple_damping = 0.66f;
        water_parameters.foam_intensity = 0.72f;
        m_Water.SetParams(water_parameters);
        m_Water.SetEnvironment(FVec3{0.16f, 0.30f, 0.52f}, FVec3{0.46f, 0.33f, 0.24f}, FVec3{0.015f, 0.020f, 0.030f});

        const f32 aspect = static_cast<f32>(width) / static_cast<f32>(height);
        m_Camera.SetPerspective(52.0f * kDeg2Rad, aspect, 0.10f, 120.0f);
        m_Camera.SetLookAt(kCameraPosition, kCameraTarget, FVec3::Up());

        m_PostParams.bloom_enabled = false;
        m_PostParams.exposure = 1.0f;
        m_PostParams.tonemap_kind = 2;
        m_PostParams.vignette_intensity = 0.0f;
        m_PostParams.vignette_radius = 1.0f;
        m_PostParams.chromatic_aberration = 0.0f;
        m_PostParams.grain_intensity = 0.0f;
        m_PostParams.grain_time = 0.0f;
        m_PostParams.cas_strength = 0.0f;
        m_PostParams.auto_exposure_enabled = false;
        m_PostParams.delta_time = kFixedDeltaSeconds;
        m_PostParams.taa_enabled = UsesTaa(m_Invocation.mode);
        m_PostParams.taa_blend_factor = kTaaBlendFactor;
        m_WakeEnabled = InjectsWake(m_Invocation.mode);
        m_Ready = true;
    }

    /** 実時間を使わず、全 mode で同じ update -> inject -> render 順を維持する。 */
    void OnUpdate(f32) noexcept override
    {
        if (!m_Ready) return;

        m_Water.Update(kFixedDeltaSeconds);
        if (m_UpdateFrame == kWarmupFrameCount) {
            if (InvalidatesHistoryBeforeWake(m_Invocation.mode)) {
                if (!m_PostParams.taa_enabled || !m_WakeEnabled) {
                    Fail("C1 history invalidation requires enabled TAA and wake");
                    return;
                }
                m_Post.InvalidateTaaHistory();
                m_HistoryInvalidatedImmediatelyBeforeWake = true;
            }
            if (m_WakeEnabled) {
                m_AcceptedWakeSamples = m_Water.AddWakeSegment(kWakeStart, kWakeEnd, kFixedDeltaSeconds, 0.24f, 0.24f, 0.22f);
                if (m_AcceptedWakeSamples == 0u) {
                    Fail("wake injection was rejected");
                    return;
                }
            }
            m_CaptureArmed = true;
        }
        ++m_UpdateFrame;
    }

    /** HDR water frame を TAA/tonemap し、armed frame の final swapchain を readback する。 */
    bool OnCustomFrame() noexcept override
    {
        if (!m_Ready) return true;

        IRhiCommandList* command_list = GetRenderer().CommandList();
        IRhiSwapchain* swapchain = GetRenderer().Swapchain();
        IRhiTexture* hdr = m_Post.HdrRenderTarget();
        if (command_list == nullptr || swapchain == nullptr || hdr == nullptr || !m_SceneCopy) {
            Fail("render resources became unavailable");
            return true;
        }

        const FMat4 view_projection = m_Camera.ViewProjection();
        m_PostParams.taa_view_proj_no_jitter = view_projection;
        m_PostParams.taa_prev_view_proj_no_jitter = view_projection;
        m_PostParams.taa_camera_position = kCameraPosition;

        const u32 buffer_index = swapchain->AcquireNextImage();
        command_list->Begin();
        command_list->BeginRenderToTexture(*hdr, FClearColor{0.025f, 0.060f, 0.095f, 1.0f}, nullptr, 1.0f);
        FViewport viewport{};
        viewport.width = static_cast<f32>(hdr->Width());
        viewport.height = static_cast<f32>(hdr->Height());
        command_list->SetViewport(viewport);
        FScissorRect scissor{};
        scissor.right = static_cast<i32>(hdr->Width());
        scissor.bottom = static_cast<i32>(hdr->Height());
        command_list->SetScissor(scissor);
        command_list->EndRenderToTexture(*hdr);

        m_Blit.Copy(*command_list, *hdr, *m_SceneCopy);
        command_list->BeginRenderToTextureLoad(*hdr, nullptr);
        command_list->SetViewport(viewport);
        command_list->SetScissor(scissor);
        m_Water.SetFrame(view_projection, kCameraPosition, hdr->Width(), hdr->Height(), FVec3{-0.42f, 0.82f, -0.38f}, FVec3{4.9f, 4.3f, 3.7f});
        m_Water.DrawMesh(*command_list, m_WaterMesh, FMat4::Identity(), m_SceneCopy.Get());
        command_list->EndRenderToTexture(*hdr);

        m_Post.Render(*command_list, *swapchain, buffer_index, m_PostParams);
        command_list->End();
        if (!command_list->Submit()) {
            Fail("render submission failed");
            return true;
        }

        if (m_CaptureArmed && !CaptureAndWrite(*swapchain, buffer_index)) return true;
        if (!swapchain->Present()) {
            Fail("present failed");
            return true;
        }

        if (m_CaptureArmed) {
            m_Succeeded = true;
            Quit();
        }
        return true;
    }

    /** resource を GPU idle 後に逆順で解放する。 */
    void OnShutdown() noexcept override
    {
        if (GetRenderer().Device() != nullptr) GetRenderer().Device()->WaitIdle();
        m_SceneCopy.Reset();
        m_WaterMesh = FGpuMesh{};
        m_Blit.Shutdown();
        m_Water.Shutdown();
        m_Post.Shutdown();
    }

private:
    /** app を失敗終了へ移し、最初の原因だけをログへ残す。 */
    void Fail(const char* message) noexcept
    {
        if (!m_Failed) {
            ACS_LOG_ERROR("WaterTaaEvidence: %s", message);
            m_Failed = true;
        }
        Quit();
    }

    /** raw BGRA8 と mode/config metadata を空 output root へ書き出す。 */
    bool CaptureAndWrite(IRhiSwapchain& swapchain, u32 buffer_index) noexcept
    {
        if (!ValidateModeCaptureState()) return false;

        auto* dx12_device = static_cast<CDx12Device*>(GetRenderer().Device());
        auto* dx12_swapchain = static_cast<FDx12Swapchain*>(&swapchain);
        std::vector<u8> pixels;
        if (!ReadBackSwapchainBuffer(*dx12_device, *dx12_swapchain, buffer_index, pixels)) {
            Fail("swapchain readback failed");
            return false;
        }
        usize expected_raw_byte_count = 0u;
        /** viewport 契約の計算成功と実 readback byte 数の一致を分けて判定する。 */
        const bool has_expected_raw_byte_count = ExpectedRawByteCount(swapchain.Width(), swapchain.Height(), expected_raw_byte_count);
        const bool raw_byte_count_matches = has_expected_raw_byte_count && pixels.size() == expected_raw_byte_count;
        if (!raw_byte_count_matches) {
            Fail("raw byte count does not match viewport BGRA8 contract");
            return false;
        }

        const std::filesystem::path raw_path = m_Invocation.output_root / L"frame.bgra8";
        const std::filesystem::path metadata_path = m_Invocation.output_root / L"metadata.json";
        if (!WriteBinaryFile(raw_path, pixels)) {
            Fail("raw artifact write failed");
            return false;
        }
        if (!WriteTextFile(metadata_path, BuildMetadataJson(pixels.size()))) {
            Fail("metadata artifact write failed");
            return false;
        }
        return true;
    }

    /** mode ごとの TAA、wake、history の観測前提を artifact 作成前に検査する。 */
    bool ValidateModeCaptureState() noexcept
    {
        if (m_PostParams.taa_enabled != UsesTaa(m_Invocation.mode)) {
            Fail("TAA mode contract changed before capture");
            return false;
        }
        if (m_WakeEnabled != InjectsWake(m_Invocation.mode)) {
            Fail("wake mode contract changed before capture");
            return false;
        }
        if (m_HistoryInvalidatedImmediatelyBeforeWake != InvalidatesHistoryBeforeWake(m_Invocation.mode)) {
            Fail("history invalidation mode contract changed before capture");
            return false;
        }
        if (m_WakeEnabled != (m_AcceptedWakeSamples > 0u)) {
            Fail("wake acceptance mode contract changed before capture");
            return false;
        }
        return true;
    }

    /** script が全 fresh process の同一性を照合できる JSON を作る。 */
    std::string BuildMetadataJson(usize raw_byte_count)
    {
        const FWaterSurface3DParams& water = m_Water.Params();
        IRhiSwapchain* swapchain = GetRenderer().Swapchain();
        std::string json;
        json += "{\n";
        json += "  \"schema\": \"acs.water_taa_evidence.v1\",\n";
        json += "  \"mode\": \"" + std::string(ModeName(m_Invocation.mode)) + "\",\n";
        json += "  \"configuration\": \"" + EscapeJson(m_Invocation.configuration.c_str()) + "\",\n";
        json += "  \"taa_enabled\": " + std::string(m_PostParams.taa_enabled ? "true" : "false") + ",\n";
        json += "  \"wake_enabled\": " + std::string(m_WakeEnabled ? "true" : "false") + ",\n";
        json += "  \"history_invalidated_immediately_before_wake\": " + std::string(m_HistoryInvalidatedImmediatelyBeforeWake ? "true" : "false") + ",\n";
        json += "  \"backend\": \"DX12\",\n";
        json += "  \"process_id\": " + std::to_string(static_cast<u32>(GetCurrentProcessId())) + ",\n";
        json += "  \"adapter_luid\": \"" + EscapeJson(m_Adapter.luid) + "\",\n";
        json += "  \"adapter_vendor_id\": " + std::to_string(m_Adapter.vendor_id) + ",\n";
        json += "  \"adapter_device_id\": " + std::to_string(m_Adapter.device_id) + ",\n";
        json += "  \"adapter_name\": \"" + EscapeJson(m_Adapter.name) + "\",\n";
        json += "  \"quality_q\": \"" + std::string(kQualityQContractLabel) + "\",\n";
        json += "  \"quality_q_source\": \"" + std::string(kQualityQSource) + "\",\n";
        json += "  \"halton_sequence_length\": 1,\n";
        json += "  \"halton_index\": 0,\n";
        json += "  \"halton_jitter_pixels\": [0.0, 0.0],\n";
        json += "  \"warmup_frame_count\": " + std::to_string(kWarmupFrameCount) + ",\n";
        json += "  \"capture_frame_count\": " + std::to_string(m_UpdateFrame) + ",\n";
        json += "  \"viewport\": {\"width\": " + std::to_string(swapchain->Width()) + ", \"height\": " + std::to_string(swapchain->Height()) + "},\n";
        json += "  \"timing\": {\"fixed_delta_seconds\": " + JsonNumber(kFixedDeltaSeconds) + ", \"time_source\": \"fixture-fixed\"},\n";
        json += "  \"camera\": {\"position\": [" + JsonNumber(kCameraPosition.x) + ", " + JsonNumber(kCameraPosition.y) + ", " + JsonNumber(kCameraPosition.z) + "], \"target\": [" + JsonNumber(kCameraTarget.x) + ", " + JsonNumber(kCameraTarget.y) + ", " + JsonNumber(kCameraTarget.z) + "], \"jittered\": false},\n";
        json += "  \"water\": {\"shallow_color\": [" + JsonNumber(water.shallow_color.x) + ", " + JsonNumber(water.shallow_color.y) + ", " + JsonNumber(water.shallow_color.z) + "], \"deep_color\": [" + JsonNumber(water.deep_color.x) + ", " + JsonNumber(water.deep_color.y) + ", " + JsonNumber(water.deep_color.z) + "], \"roughness\": " + JsonNumber(water.roughness) + ", \"normal_strength\": " + JsonNumber(water.normal_strength) + ", \"normal_tiling\": " + JsonNumber(water.normal_tiling) + ", \"refraction_strength\": " + JsonNumber(water.refraction_strength) + ", \"wave_amplitude\": " + JsonNumber(water.wave_amplitude) + ", \"wave_scale\": " + JsonNumber(water.wave_scale) + ", \"wave_speed\": " + JsonNumber(water.wave_speed) + ", \"ripple_lifetime\": " + JsonNumber(water.ripple_lifetime) + ", \"ripple_damping\": " + JsonNumber(water.ripple_damping) + ", \"foam_intensity\": " + JsonNumber(water.foam_intensity) + "},\n";
        json += "  \"wake\": {\"start\": [" + JsonNumber(kWakeStart.x) + ", " + JsonNumber(kWakeStart.y) + ", " + JsonNumber(kWakeStart.z) + "], \"end\": [" + JsonNumber(kWakeEnd.x) + ", " + JsonNumber(kWakeEnd.y) + ", " + JsonNumber(kWakeEnd.z) + "], \"accepted_samples\": " + std::to_string(m_AcceptedWakeSamples) + "},\n";
        json += "  \"order\": \"fixed-update; C1-invalidate-history-immediately-before-wake; add-wake; render-water; resolve-post; readback-final-swapchain\",\n";
        json += "  \"raw_file\": \"frame.bgra8\",\n";
        json += "  \"raw_format\": \"B8G8R8A8_UNORM\",\n";
        json += "  \"raw_bytes\": " + std::to_string(raw_byte_count) + "\n";
        json += "}\n";
        return json;
    }

    /** fresh process の CLI 条件。 */
    FFixtureInvocation m_Invocation;

    /** final output へ到達する scene/post resource。 */
    CPostProcess m_Post;
    CWaterSurface3D m_Water;
    CBlit m_Blit;
    FGpuMesh m_WaterMesh;
    TUniquePtr<IRhiTexture> m_SceneCopy;
    CCamera m_Camera;
    FPostProcessParams m_PostParams;
    FAdapterMetadata m_Adapter;

    /** fixed update と capture の状態。 */
    u32 m_UpdateFrame = 0u;
    u32 m_AcceptedWakeSamples = 0u;
    bool m_Ready = false;
    bool m_CaptureArmed = false;
    bool m_WakeEnabled = false;
    bool m_HistoryInvalidatedImmediatelyBeforeWake = false;
    bool m_Failed = false;
    bool m_Succeeded = false;
};

/** fixture window の寸法と非対話 timing を固定する。 */
FAppConfig MakeFixtureConfig() noexcept
{
    FAppConfig configuration{};
    configuration.title = L"ACS Water TAA Evidence";
    configuration.width = kViewportWidth;
    configuration.height = kViewportHeight;
    configuration.resizable = false;
    configuration.vsync = false;
    configuration.start_clear = false;
    configuration.log_severity = ELogSeverity::Info;
    return configuration;
}

/** command line と output root を検証して fixture process を実行する。 */
int RunFixture() noexcept
{
    FFixtureInvocation invocation{};
    if (!ParseInvocation(invocation)) {
        std::fwprintf(stderr, L"usage: --mode R0|B0|R1|B1|C1 --configuration Debug|Release --out <empty-directory>\n");
        return 2;
    }
    if (!IsEmptyOutputDirectory(invocation.output_root)) {
        std::fwprintf(stderr, L"--out must name an existing empty artifact directory\n");
        return 2;
    }

    CWaterTaaEvidenceApp application(invocation);
    const int application_result = application.Run(MakeFixtureConfig());
    return application_result == 0 && application.Succeeded() ? 0 : 1;
}

} // namespace

int main()
{
    return RunFixture();
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return RunFixture();
}
