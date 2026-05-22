// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G — AssetPack 実装 (Stub のみ)
//
// 本 .cpp では `IAssetPackReader` / `IAssetPackWriter` の I/F 自体は提供せず、
// AssetPack モジュール未統合ビルドでも常に使える `AssetPackReaderStub` /
// `AssetPackWriterStub` のみを実装する。実 AES-256-GCM (Windows CNG / bcrypt) +
// LZ4 圧縮を伴う `GoldenAssetPackReader` / `GoldenAssetPackWriter` は重い暗号 /
// 圧縮依存を含むため、別モジュール (将来の `ACS::AssetPack` = `src/assetpack/`)
// で独立に実装し、本ファイルには持ち込まない。
//
// 設計のポイント:
//   ・Stub は副作用ゼロ。Mount() は成功を返さず NotImplemented を返す
//     (= 開発中のうっかり「stub のままで pak を期待する」コードを早期検知)。
//   ・全 API は `ACS_ERR(Generic, kSubAssetPackNotImplemented, ...)` を返し、
//     呼び出し側でフォールバック (= バラのファイルから読む) を選択させる。
//   ・`GetReaderStub()` / `GetWriterStub()` は Meyer's singleton。スレッド初回
//     構築は C++11 以降の規格で保証されているため、追加同期は不要。

#include "gameframework/AssetPack.h"

#include "foundation/Error.h"

namespace acs::game {

// ---- Stub: Reader -------------------------------------------------------

Result<void> AssetPackReaderStub::Mount(const char* pack_path) noexcept {
    (void)pack_path;  // 未使用引数 (Stub なので no-op)
    return ACS_ERR(Generic, kSubAssetPackNotImplemented,
                   "AssetPackReaderStub: Mount is not implemented (link real AssetPack module)");
}

void AssetPackReaderStub::Unmount() noexcept {
    // Mount に成功し得ない Stub では何もすることがない。
    // Mount() 前に呼ばれても安全 (no-op)。
}

Result<u32> AssetPackReaderStub::FileCount() noexcept {
    return Result<u32>(ACS_ERR(Generic, kSubAssetPackNotImplemented,
                               "AssetPackReaderStub: FileCount is not implemented (link real AssetPack module)"));
}

Result<const char*> AssetPackReaderStub::FileName(u32 index) noexcept {
    (void)index;
    return Result<const char*>(ACS_ERR(Generic, kSubAssetPackNotImplemented,
                                       "AssetPackReaderStub: FileName is not implemented (link real AssetPack module)"));
}

Result<u64> AssetPackReaderStub::FileSize(const char* name) noexcept {
    (void)name;
    return Result<u64>(ACS_ERR(Generic, kSubAssetPackNotImplemented,
                               "AssetPackReaderStub: FileSize is not implemented (link real AssetPack module)"));
}

Result<void> AssetPackReaderStub::ReadFile(const char* name, u8* out_buffer, u64 buffer_size) noexcept {
    (void)name;
    (void)out_buffer;
    (void)buffer_size;
    return ACS_ERR(Generic, kSubAssetPackNotImplemented,
                   "AssetPackReaderStub: ReadFile is not implemented (link real AssetPack module)");
}

// ---- Stub: Writer -------------------------------------------------------

Result<void> AssetPackWriterStub::BeginPack(const char* output_path) noexcept {
    (void)output_path;
    return ACS_ERR(Generic, kSubAssetPackNotImplemented,
                   "AssetPackWriterStub: BeginPack is not implemented (link real AssetPack module)");
}

Result<void> AssetPackWriterStub::AddFile(const char* virtual_name, const u8* data, u64 size) noexcept {
    (void)virtual_name;
    (void)data;
    (void)size;
    return ACS_ERR(Generic, kSubAssetPackNotImplemented,
                   "AssetPackWriterStub: AddFile is not implemented (link real AssetPack module)");
}

Result<void> AssetPackWriterStub::FinishPack() noexcept {
    return ACS_ERR(Generic, kSubAssetPackNotImplemented,
                   "AssetPackWriterStub: FinishPack is not implemented (link real AssetPack module)");
}

// ---- static singleton accessors -----------------------------------------

IAssetPackReader& GetReaderStub() noexcept {
    // C++11 以降、関数スコープ static の初期化は thread-safe。
    static AssetPackReaderStub _instance;
    return _instance;
}

IAssetPackWriter& GetWriterStub() noexcept {
    static AssetPackWriterStub _instance;
    return _instance;
}

} // namespace acs::game
