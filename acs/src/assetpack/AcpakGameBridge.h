// SPDX-License-Identifier: Apache-2.0
// Concrete GameFramework bridge for `.acpak`.
#pragma once

#include "assetpack/AcpakReader.h"
#include "assetpack/AcpakWriter.h"
#include "gameframework/AssetPack.h"

namespace acs::assetpack {

class FAcpakGameReader final : public acs::game::IAssetPackReader {
public:
    FAcpakGameReader() noexcept = default;
    ~FAcpakGameReader() noexcept override = default;

    FAcpakGameReader(const FAcpakGameReader&)            = delete;
    FAcpakGameReader& operator=(const FAcpakGameReader&) = delete;
    FAcpakGameReader(FAcpakGameReader&&)                 = delete;
    FAcpakGameReader& operator=(FAcpakGameReader&&)      = delete;

    acs::TResult<void>        Mount(const char* PackPath) noexcept override;
    void                      Unmount() noexcept override;
    bool                      IsMounted() const noexcept override;
    acs::TResult<acs::u32>    FileCount() noexcept override;
    acs::TResult<const char*> FileName(acs::u32 Index) noexcept override;
    acs::TResult<acs::u64>    FileSize(const char* Name) noexcept override;
    acs::TResult<void>        ReadFile(const char* Name,
                                       acs::u8* OutBuffer,
                                       acs::u64 BufferSize) noexcept override;

private:
    static constexpr acs::u32 kPathCapacity = 512;

    acs::TResult<const wchar_t*> ToWideScratch(const char* Text) noexcept;
    acs::TResult<const char*> ToUtf8Scratch(const wchar_t* Text) noexcept;

    FAcpakReader m_Reader;
    wchar_t m_WideScratch[kPathCapacity] = {};
    char m_Utf8Scratch[kPathCapacity] = {};
};

class FAcpakGameWriter final : public acs::game::IAssetPackWriter {
public:
    FAcpakGameWriter() noexcept = default;
    ~FAcpakGameWriter() noexcept override = default;

    FAcpakGameWriter(const FAcpakGameWriter&)            = delete;
    FAcpakGameWriter& operator=(const FAcpakGameWriter&) = delete;
    FAcpakGameWriter(FAcpakGameWriter&&)                 = delete;
    FAcpakGameWriter& operator=(FAcpakGameWriter&&)      = delete;

    acs::TResult<void> BeginPack(const char* OutputPath) noexcept override;
    acs::TResult<void> AddFile(const char* VirtualName,
                               const acs::u8* Data,
                               acs::u64 Size) noexcept override;
    acs::TResult<void> FinishPack() noexcept override;

private:
    static constexpr acs::u32 kPathCapacity = 512;

    acs::TResult<const wchar_t*> ToWideScratch(const char* Text) noexcept;

    FAcpakWriter m_Writer;
    wchar_t m_WideScratch[kPathCapacity] = {};
};

// ---- 既定 provider 結線 (gameframework seam への委譲点) -------------------
// gameframework (`acs::game::IAssetPackReader` / `IAssetPackWriter`) は本 backend
// モジュールに依存できないため、結線は backend 側 (本モジュール) から
// `acs::game::SetAssetPackReaderProvider()` / `SetAssetPackWriterProvider()` を
// 呼んで行う。アプリは起動時に一度 `InstallAcpakReaderAsDefault()` /
// `InstallAcpakWriterAsDefault()` を呼ぶだけで、以降は backend 非依存に
// `acs::game::GetDefaultAssetPackReader()` / `GetDefaultAssetPackWriter()` で
// 実 `.acpak` Reader/Writer を取得できる。

// プロセス共有の既定 Reader を返す (Meyers singleton)。
acs::game::IAssetPackReader& GetDefaultAcpakReader() noexcept;

// `acs::game::SetAssetPackReaderProvider(&GetDefaultAcpakReader)` を呼ぶ。
void InstallAcpakReaderAsDefault() noexcept;

// プロセス共有の既定 Writer を返す (Meyers singleton)。
acs::game::IAssetPackWriter& GetDefaultAcpakWriter() noexcept;

// `acs::game::SetAssetPackWriterProvider(&GetDefaultAcpakWriter)` を呼ぶ。
void InstallAcpakWriterAsDefault() noexcept;

} // namespace acs::assetpack
