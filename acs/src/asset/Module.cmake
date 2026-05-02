acs_module(
    NAME    Asset
    TYPE    Runtime
    SOURCES
        AssetRegistry.cpp
        BinaryAsset.cpp
    HEADERS
        AssetId.h
        Asset.h
        IAssetLoader.h
        AssetRegistry.h
        BinaryAsset.h
    PUBLIC_DEPS
        Foundation
        Memory
        Container
        Threading
        Platform
)
