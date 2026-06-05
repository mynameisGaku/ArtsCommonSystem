// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// AcpakDefault — FAcpakGameReader / FAcpakGameWriter を gameframework の既定
// AssetPack provider へ結線する
// -----------------------------------------------------------------------------
// gameframework は ACS::AssetPack に依存できない (循環依存) ため、結線は backend
// 側 (本 TU) から `acs::game::SetAssetPackReaderProvider()` /
// `SetAssetPackWriterProvider()` を呼んで行う。アプリは起動時に一度
// `acs::assetpack::InstallAcpakReaderAsDefault()` /
// `InstallAcpakWriterAsDefault()` を呼ぶだけで、以降は backend 非依存に
// `acs::game::GetDefaultAssetPackReader()` / `GetDefaultAssetPackWriter()` で
// 実 `.acpak` Reader/Writer を取得できる。
//
// provider が返す Reader/Writer はプロセス共有 singleton。FAcpakGameReader /
// FAcpakGameWriter は default 構築でき、別途 Init() を要さない (利用側が
// Mount()/BeginPack() でライフサイクルを開始する) ため、Lua backend のような
// lazy-init は不要。function-local static の構築自体が thread-safe。
// =============================================================================
#include "assetpack/AcpakGameBridge.h"

namespace acs::assetpack {

/** プロセス共有の既定 Acpak Reader を返す (Meyers singleton)。 */
acs::game::IAssetPackReader& GetDefaultAcpakReader() noexcept {
    // Meyers singleton。プロセス内 1 個の Reader を既定として共有する。
    // 利用側が Mount()/Unmount() でマウント状態を管理する。
    static FAcpakGameReader s_reader;
    return s_reader;
}

/** GetDefaultAcpakReader を gameframework の既定 Reader provider として登録する。 */
void InstallAcpakReaderAsDefault() noexcept {
    acs::game::SetAssetPackReaderProvider(&GetDefaultAcpakReader);
}

/** プロセス共有の既定 Acpak Writer を返す (Meyers singleton)。 */
acs::game::IAssetPackWriter& GetDefaultAcpakWriter() noexcept {
    // Meyers singleton。プロセス内 1 個の Writer を既定として共有する。
    // 利用側が BeginPack()/FinishPack() で書き込みライフサイクルを管理する。
    static FAcpakGameWriter s_writer;
    return s_writer;
}

/** GetDefaultAcpakWriter を gameframework の既定 Writer provider として登録する。 */
void InstallAcpakWriterAsDefault() noexcept {
    acs::game::SetAssetPackWriterProvider(&GetDefaultAcpakWriter);
}

} // namespace acs::assetpack
