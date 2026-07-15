// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G — FAssetPack 実装 (Stub のみ)
//
// 本 .cpp では `IAssetPackReader` / `IAssetPackWriter` の I/F 自体は提供せず、
// FAssetPack モジュール未統合ビルドでも常に使える `FAssetPackReaderStub` /
// `FAssetPackWriterStub` のみを実装する。実 AES-256-GCM (Windows CNG / bcrypt) +
// LZ4 圧縮を伴う `GoldenAssetPackReader` / `GoldenAssetPackWriter` は重い暗号 /
// 圧縮依存を含むため、別モジュール (将来の `ACS::FAssetPack` = `src/assetpack/`)
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
#include "threading/Atomic.h"

namespace acs::game {

/** NotImplemented を返す stub 実装。 */
TResult<void> FAssetPackReaderStub::Mount(const char* pack_path) noexcept {
    (void)pack_path;  // 未使用引数 (Stub なので no-op)
    return ACS_ERR(Generic, kSubAssetPackNotImplemented,
                   "FAssetPackReaderStub: Mount is not implemented (link real FAssetPack module)");
}

/** 副作用なしの no-op stub 実装。 */
void FAssetPackReaderStub::Unmount() noexcept {
    // Mount に成功し得ない Stub では何もすることがない。
    // Mount() 前に呼ばれても安全 (no-op)。
}

/** NotImplemented を返す stub 実装。 */
TResult<u32> FAssetPackReaderStub::FileCount() noexcept {
    return TResult<u32>(ACS_ERR(Generic, kSubAssetPackNotImplemented,
                               "FAssetPackReaderStub: FileCount is not implemented (link real FAssetPack module)"));
}

/** NotImplemented を返す stub 実装。 */
TResult<const char*> FAssetPackReaderStub::FileName(u32 index) noexcept {
    (void)index;
    return TResult<const char*>(ACS_ERR(Generic, kSubAssetPackNotImplemented,
                                       "FAssetPackReaderStub: FileName is not implemented (link real FAssetPack module)"));
}

/** NotImplemented を返す stub 実装。 */
TResult<u64> FAssetPackReaderStub::FileSize(const char* name) noexcept {
    (void)name;
    return TResult<u64>(ACS_ERR(Generic, kSubAssetPackNotImplemented,
                               "FAssetPackReaderStub: FileSize is not implemented (link real FAssetPack module)"));
}

/** NotImplemented を返す stub 実装。 */
TResult<void> FAssetPackReaderStub::ReadFile(const char* name, u8* out_buffer, u64 buffer_size) noexcept {
    (void)name;
    (void)out_buffer;
    (void)buffer_size;
    return ACS_ERR(Generic, kSubAssetPackNotImplemented,
                   "FAssetPackReaderStub: ReadFile is not implemented (link real FAssetPack module)");
}

/** NotImplemented を返す stub 実装。 */
TResult<void> FAssetPackWriterStub::BeginPack(const char* output_path) noexcept {
    (void)output_path;
    return ACS_ERR(Generic, kSubAssetPackNotImplemented,
                   "FAssetPackWriterStub: BeginPack is not implemented (link real FAssetPack module)");
}

/** NotImplemented を返す stub 実装。 */
TResult<void> FAssetPackWriterStub::AddFile(const char* virtual_name, const u8* data, u64 size) noexcept {
    (void)virtual_name;
    (void)data;
    (void)size;
    return ACS_ERR(Generic, kSubAssetPackNotImplemented,
                   "FAssetPackWriterStub: AddFile is not implemented (link real FAssetPack module)");
}

/** NotImplemented を返す stub 実装。 */
TResult<void> FAssetPackWriterStub::FinishPack() noexcept {
    return ACS_ERR(Generic, kSubAssetPackNotImplemented,
                   "FAssetPackWriterStub: FinishPack is not implemented (link real FAssetPack module)");
}

/** 既定 stub の Reader (Meyer's singleton) を返す。 */
IAssetPackReader& GetReaderStub() noexcept {
    // C++11 以降、関数スコープ static の初期化は thread-safe。
    static FAssetPackReaderStub m_Instance;
    return m_Instance;
}

/** 既定 stub の Writer (Meyer's singleton) を返す。 */
IAssetPackWriter& GetWriterStub() noexcept {
    static FAssetPackWriterStub m_Instance;
    return m_Instance;
}

namespace {
/** 既定 Reader provider (実 backend の Install* で登録。未登録なら nullptr)。 */
TAtomic<AssetPackReaderProvider> g_ReaderProvider{nullptr};

/** 既定 Writer provider (実 backend の Install* で登録。未登録なら nullptr)。 */
TAtomic<AssetPackWriterProvider> g_WriterProvider{nullptr};
} // namespace

/** 既定 Reader provider を登録する (nullptr で stub に戻す)。 */
void SetAssetPackReaderProvider(AssetPackReaderProvider Provider) noexcept
{
    g_ReaderProvider.Store(Provider, EMemoryOrder::Release);
}

/** provider 登録済みならその実装、未登録なら GetReaderStub() を返す。 */
IAssetPackReader& GetDefaultAssetPackReader() noexcept
{
    const AssetPackReaderProvider Provider = g_ReaderProvider.Load(EMemoryOrder::Acquire);
    return Provider ? Provider() : GetReaderStub();
}

/** 既定 Writer provider を登録する (nullptr で stub に戻す)。 */
void SetAssetPackWriterProvider(AssetPackWriterProvider Provider) noexcept
{
    g_WriterProvider.Store(Provider, EMemoryOrder::Release);
}

/** provider 登録済みならその実装、未登録なら GetWriterStub() を返す。 */
IAssetPackWriter& GetDefaultAssetPackWriter() noexcept
{
    const AssetPackWriterProvider Provider = g_WriterProvider.Load(EMemoryOrder::Acquire);
    return Provider ? Provider() : GetWriterStub();
}

} // namespace acs::game
