# Easy module — 初学者向けの「簡単モード」ファサード。
# acs::FApplication や RHI を内部に隠し、手続き的な関数だけで 2D ゲームを
# 書けるようにする薄いレイヤ（DX ライブラリのような使い心地）。
acs_module(
    NAME    Easy
    TYPE    Runtime
    SOURCES Easy.cpp
    HEADERS Easy.h
    PUBLIC_DEPS
        Foundation
        Threading
        Memory
        Container
        Math
        Platform
        Asset
        Render
        Audio
)
