// SPDX-License-Identifier: Apache-2.0
#include "assetpack/AcpakGameBridge.h"
#include "memory/SystemAllocator.h"

namespace acs::assetpack {

namespace {

/** 既定 Reader/Writer と同じ長さだけ生存する専用 allocator を束ねる。 */
struct FDefaultAcpakState {
    FDefaultAcpakState() noexcept : reader(allocator), writer(allocator)
    {
    }

    // backend を先に破棄してから allocator を破棄する宣言順にする。
    acs::CSystemAllocator allocator;
    CAcpakGameReader reader;
    CAcpakGameWriter writer;
};

FDefaultAcpakState& GetDefaultAcpakState() noexcept
{
    static FDefaultAcpakState s_state;
    return s_state;
}

} // namespace

/** プロセス共有 state が所有する既定 Acpak Reader を返す。 */
acs::game::IAssetPackReader& GetDefaultAcpakReader() noexcept {
    // マウント状態は同じ Reader instance に保持される。
    return GetDefaultAcpakState().reader;
}

/** GetDefaultAcpakReader を gameframework の既定 Reader provider として登録する。 */
void InstallAcpakReaderAsDefault() noexcept {
    acs::game::SetAssetPackReaderProvider(&GetDefaultAcpakReader);
}

/** プロセス共有 state が所有する既定 Acpak Writer を返す。 */
acs::game::IAssetPackWriter& GetDefaultAcpakWriter() noexcept {
    // 書き込み状態は同じ Writer instance に保持される。
    return GetDefaultAcpakState().writer;
}

/** GetDefaultAcpakWriter を gameframework の既定 Writer provider として登録する。 */
void InstallAcpakWriterAsDefault() noexcept {
    acs::game::SetAssetPackWriterProvider(&GetDefaultAcpakWriter);
}

} // namespace acs::assetpack
