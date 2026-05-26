# acs_assetpack — `.acpak` v1 アーカイブ操作 CLI ツール。
#
# ゲーム本体とは独立した CONSOLE 実行ファイル。配布ビルドから ZIP 化される
# ようなものではなく、アセットの pack / unpack / verify / info / list をする
# 開発時ユーティリティ。
#
# Phase 1 では raw bytes + CRC32 のみ操作する。Phase 2 で FAcpakCrypto /
# FAcpakLz4 が AssetPack モジュールに追加された後は --encrypt / --compress /
# --key オプションが実機能を持つ (本 CLI 側のコード変更は不要)。
#
# 使い方:
#   acs_assetpack pack ./assets game.acpak --compress
#   acs_assetpack list game.acpak
#   acs_assetpack unpack game.acpak ./extracted
# acs_module(NAME AssetPack) が lib target `acs_assetpack` (lowercase) を作るため、
# CLI 側は internal target 名を `acs_assetpack_cli` にして衝突回避。
# OUTPUT_NAME で実行ファイル名は意図通り `acs_assetpack.exe` のまま。
add_executable(acs_assetpack_cli main.cpp)
set_target_properties(acs_assetpack_cli PROPERTIES
    OUTPUT_NAME acs_assetpack   # 実行ファイル名は acs_assetpack(.exe)
    WIN32_EXECUTABLE OFF        # CONSOLE app (stdout / stderr を出す)
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
acs_apply_compiler_options(acs_assetpack_cli)
target_link_libraries(acs_assetpack_cli PRIVATE
    ACS::AssetPack
    ACS::Foundation
    ACS::Container
    ACS::Memory
    ACS::Platform
    Bcrypt.lib    # Phase 24 FAcpakCrypto (AES-256-GCM via Windows CNG)
    Shell32.lib)  # CommandLineToArgvW (UTF-16 引数 parse)
