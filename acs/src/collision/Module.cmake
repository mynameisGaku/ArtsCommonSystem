# SPDX-License-Identifier: Apache-2.0
# Collision module — アセット (スプライト alpha / 3D メッシュ) から衝突形状を
# 生成するジオメトリ処理。純 CPU・依存軽量なので常時ビルド。
#
# 名前空間 acs、CMake target ACS::Collision。
acs_module(
    NAME    Collision
    TYPE    Runtime
    SOURCES
        SpriteCollider.cpp
        MeshCollider.cpp
    HEADERS
        SpriteCollider.h
        MeshCollider.h
    PUBLIC_DEPS
        Foundation
        Container
        Math
        Asset
)
