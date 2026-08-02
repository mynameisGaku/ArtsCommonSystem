// SPDX-License-Identifier: Apache-2.0
// HelloAssetPackBridge - CAssetPackBridgeDemoApp implementation.
#include "AssetPackBridgeDemoApp.h"

#include "assetpack/AcpakGameBridge.h"

#include <cstdio>

namespace helloassetpack {

namespace {

bool BytesEqual(const acs::u8* A, const acs::u8* B, acs::u64 Size) noexcept {
    for (acs::u64 i = 0; i < Size; ++i) {
        if (A[i] != B[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

int CAssetPackBridgeDemoApp::Run() noexcept {
    std::puts("=== ACS HelloAssetPackBridge ===");
    std::puts("backend: REAL .acpak bridge (CAcpakGameReader/Writer)");

    static constexpr const char* kPackPath = "hello_asset_pack_bridge.acpak";
    static constexpr const char* kFileName = "data/greeting.txt";
    static constexpr acs::u8 kPayload[] = {
        'A', 'C', 'S', ' ', 'a', 's', 's', 'e', 't', ' ', 'p', 'a', 'c', 'k', '\n'
    };

    acs::assetpack::CAcpakGameWriter Writer;
    auto Begin = Writer.BeginPack(kPackPath);
    auto Add = Begin.IsOk()
        ? Writer.AddFile(kFileName, kPayload, sizeof(kPayload))
        : acs::TResult<void>(Begin.Error());
    auto Finish = Add.IsOk()
        ? Writer.FinishPack()
        : acs::TResult<void>(Add.Error());

    acs::assetpack::CAcpakGameReader Reader;
    auto Mount = Finish.IsOk()
        ? Reader.Mount(kPackPath)
        : acs::TResult<void>(Finish.Error());
    auto Count = Mount.IsOk()
        ? Reader.FileCount()
        : acs::TResult<acs::u32>(Mount.Error());
    auto Name = Count.IsOk()
        ? Reader.FileName(0)
        : acs::TResult<const char*>(Count.Error());
    auto Size = Name.IsOk()
        ? Reader.FileSize(kFileName)
        : acs::TResult<acs::u64>(Name.Error());

    acs::u8 Readback[sizeof(kPayload)] = {};
    auto Read = Size.IsOk()
        ? Reader.ReadFile(kFileName, Readback, sizeof(Readback))
        : acs::TResult<void>(Size.Error());

    const bool bWriteOk = Begin.IsOk() && Add.IsOk() && Finish.IsOk();
    const bool bReadOk = Mount.IsOk()
                      && Count.IsOk()
                      && Count.Value() == 1
                      && Name.IsOk()
                      && Size.IsOk()
                      && Size.Value() == sizeof(kPayload)
                      && Read.IsOk()
                      && BytesEqual(kPayload, Readback, sizeof(kPayload));

    std::printf("write : begin=%s add=%s finish=%s\n",
                Begin.IsOk() ? "ok" : "fail",
                Add.IsOk() ? "ok" : "fail",
                Finish.IsOk() ? "ok" : "fail");
    std::printf("read  : mount=%s count=%u name=%s size=%llu read=%s\n",
                Mount.IsOk() ? "ok" : "fail",
                Count.IsOk() ? static_cast<unsigned>(Count.Value()) : 0u,
                Name.IsOk() ? Name.Value() : "(none)",
                Size.IsOk() ? static_cast<unsigned long long>(Size.Value()) : 0ull,
                Read.IsOk() ? "ok" : "fail");

    Reader.Unmount();

    const bool bOk = bWriteOk && bReadOk;
    std::puts(bOk ? "result: ALL PASS" : "result: FAILED");
    return bOk ? 0 : 2;
}

} // namespace helloassetpack
