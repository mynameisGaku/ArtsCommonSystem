# AssetPack — `.acpak` アーカイブの読み書きを担う独立エンジンモジュール。
#
# GameFramework Pillar G (`acs::game::IAssetPackReader` / `IAssetPackWriter`) は
# あくまでシーム (純粋仮想 I/F) を提供するだけで、本物のフォーマット (magic /
# version / file table / 圧縮 / 暗号化) を扱う実体はここに置く。
#
# Phase 1 (本実装):
#   ・`.acpak` v1 raw bytes フォーマット (magic + header + file table + 生データ)
#   ・CRC32 (poly 0xEDB88320) によるエントリ毎の完全性検証
#   ・Win32 CreateFileW / ReadFile / WriteFile / CloseHandle で I/O
#   ・flags = 0 のみ機能、Encrypted / Compressed は NotImplemented (Phase 2)
#
# Phase 2 (後):
#   ・AES-256-GCM (Windows CNG / bcrypt) による暗号化 + 認証タグ検証
#   ・LZ4 による圧縮 (compress-then-encrypt)
#   ・暗号化 TOC / ハッシュ化パス / overlay (パッチ pak) 対応
#
# 利用者の include パス:
#   #include "assetpack/AcpakFormat.h"
#   #include "assetpack/AcpakReader.h"
#   #include "assetpack/AcpakWriter.h"
#
# モジュール命名: ACS::AssetPack
# 名前空間:       acs::assetpack
acs_module(
    NAME    AssetPack
    TYPE    Runtime
    SOURCES
        AcpakReader.cpp
        AcpakWriter.cpp
    HEADERS
        AcpakFormat.h
        AcpakReader.h
        AcpakWriter.h
    PUBLIC_DEPS
        Foundation
        Container
        Memory
        Platform
)
