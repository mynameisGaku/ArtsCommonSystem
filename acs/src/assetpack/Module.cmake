# AssetPack — `.acpak` アーカイブの読み書きを担う独立エンジンモジュール。
#
# GameFramework Pillar G (`acs::game::IAssetPackReader` / `IAssetPackWriter`) は
# あくまでシーム (純粋仮想 I/F) を提供するだけで、本物のフォーマット (magic /
# version / file table / 圧縮 / 暗号化) を扱う実体はここに置く。
#
# Phase 1 (実装済):
#   ・`.acpak` v1 raw bytes フォーマット (magic + header + file table + 生データ)
#   ・CRC32 (poly 0xEDB88320) によるエントリ毎の完全性検証
#   ・Win32 CreateFileW / ReadFile / WriteFile / CloseHandle で I/O
#
# Phase 2 (実装済):
#   ・AES-256-GCM (Windows CNG / bcrypt) による per-entry 暗号化 + 認証タグ検証
#   ・PBKDF2-HMAC-SHA256, 10000 iterations による鍵導出 (AcpakCrypto::DeriveKey)
#   ・LZ4 block format 自前実装による per-entry 圧縮 (compress-then-encrypt)
#   ・AcpakFileEntry に cipher_nonce[12] + cipher_tag[16] 追加 (下位互換あり)
#   ・v1 で書いた flags=0 の .acpak はそのまま読める
#
# Phase 3+ (TODO):
#   ・暗号化 TOC / ハッシュ化パス / overlay (パッチ pak) 対応
#   ・per-entry "97%" 圧縮安全規則 (LZ4 を試して効果がなければ生格納)
#   ・AES-GCM AAD に path_hash / size を渡して cut-and-paste 攻撃対策
#
# 利用者の include パス:
#   #include "assetpack/AcpakFormat.h"
#   #include "assetpack/AcpakReader.h"
#   #include "assetpack/AcpakWriter.h"
#   #include "assetpack/AcpakCrypto.h"   (Phase 2: 暗号化鍵導出 / AES-GCM)
#   #include "assetpack/AcpakLz4.h"      (Phase 2: LZ4 圧縮 — 自前実装)
#
# モジュール命名: ACS::AssetPack
# 名前空間:       acs::assetpack
acs_module(
    NAME    AssetPack
    TYPE    Runtime
    SOURCES
        AcpakReader.cpp
        AcpakWriter.cpp
        AcpakGameBridge.cpp
        AcpakDefault.cpp
        AcpakCrypto.cpp
        AcpakLz4.cpp
    HEADERS
        AcpakFormat.h
        AcpakReader.h
        AcpakWriter.h
        AcpakGameBridge.h
        AcpakCrypto.h
        AcpakLz4.h
    PUBLIC_DEPS
        Foundation
        Container
        Memory
        Platform
        GameFramework
    # Bcrypt.lib は AcpakCrypto.cpp が AES-256-GCM / PBKDF2 / CSPRNG に使う。
    # OS 同梱なので third_party 依存は増えない (Windows SDK だけで完結)。
    LINK_PRIVATE
        Bcrypt
)
