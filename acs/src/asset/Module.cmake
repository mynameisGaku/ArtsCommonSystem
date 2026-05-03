# Asset module — 各種フォーマットローダ（PNG/JPG/BMP/TGA/HDR/...,
# glTF/GLB/OBJ/FBX, WAV/MP3/FLAC/OGG, テキスト系）。
# サードパーティはすべて FetchContent (cmake/ACSThirdParty.cmake) 経由。
acs_third_party_stb()
acs_third_party_cgltf()
acs_third_party_ufbx()
acs_third_party_drlibs()

acs_module(
    NAME    Asset
    TYPE    Runtime
    SOURCES
        AssetRegistry.cpp
        BinaryAsset.cpp
        TextAsset.cpp
        ImageAsset.cpp
        MeshAsset.cpp
        MeshPrimitive.cpp
        SkinnedMesh.cpp
        AudioAsset.cpp
    HEADERS
        AssetId.h
        Asset.h
        IAssetLoader.h
        AssetRegistry.h
        AssetFuture.h
        BinaryAsset.h
        TextAsset.h
        ImageAsset.h
        MeshAsset.h
        MeshPrimitive.h
        SkinnedMesh.h
        AudioAsset.h
    PUBLIC_DEPS
        Foundation
        Memory
        Container
        Threading
        Math
        Platform
    LINK_PRIVATE
        acs_third_party::stb
        acs_third_party::cgltf
        acs_third_party::ufbx
        acs_third_party::drlibs
)
