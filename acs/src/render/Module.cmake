# stb (truetype) はフォントのアトラス焼きで使うので Render でも fetch する
acs_third_party_stb()

# トップレベルの ACS_RENDER_DILIGENT が ON のときだけ Diligent を fetch する。
if(ACS_RENDER_DILIGENT)
    acs_third_party_diligent()
endif()

# 共通 + Dx12 ソース（Dx12 は ACS_Render_DX12_RAW のときのみ追加）
set(_acs_render_sources
    Renderer.cpp
    RenderAssets.cpp
    StandardShader.cpp
    PbrShader.cpp
    SpriteBatch.cpp
    Font.cpp
    Sky.cpp
    Particles.cpp
    SkinnedShader.cpp
    ShadowMap.cpp
    PostProcess.cpp
    Ibl.cpp
    Atmosphere.cpp
    Ssr.cpp
    Ssao.cpp
    Ssgi.cpp
    MotionVector.cpp
)
set(_acs_render_headers
    RhiTypes.h
    IRhiDevice.h
    IRhiSwapchain.h
    IRhiCommandList.h
    IRhiBuffer.h
    IRhiShader.h
    IRhiPipeline.h
    IRhiTexture.h
    IRhiSampler.h
    Renderer.h
    RenderAssets.h
    StandardShader.h
    PbrShader.h
    SpriteBatch.h
    Font.h
    Sky.h
    Particles.h
    SkinnedShader.h
    ShadowMap.h
    PostProcess.h
    Ibl.h
    Atmosphere.h
    Ssr.h
    Ssao.h
    Ssgi.h
    MotionVector.h
)

if(ACS_RENDER_DX12_RAW)
    list(APPEND _acs_render_sources
        Dx12/Dx12Device.cpp
        Dx12/Dx12Swapchain.cpp
        Dx12/Dx12CommandList.cpp
        Dx12/Dx12Buffer.cpp
        Dx12/Dx12Shader.cpp
        Dx12/Dx12Pipeline.cpp
        Dx12/Dx12Texture.cpp
    )
    # 注: Dx12/*.h はバックエンド内部実装専用。public HEADERS には載せない。
    # (load via .cpp 経由のみ。IRhi* インターフェイスだけが外向き)
endif()

if(ACS_RENDER_DILIGENT)
    list(APPEND _acs_render_sources
        Diligent/DiligentDevice.cpp
        Diligent/DiligentSwapchain.cpp
        Diligent/DiligentCommandList.cpp
        Diligent/DiligentBuffer.cpp
        Diligent/DiligentShader.cpp
        Diligent/DiligentPipeline.cpp
        Diligent/DiligentTexture.cpp
        Diligent/DiligentMemoryAdapter.cpp
        Diligent/DiligentBackend.cpp
    )
    # Diligent/*.h も public HEADERS には載せない (バックエンド内部)
endif()

# DX12 / Diligent ライブラリは PRIVATE にして外向きに leak させない。
# IRhi* インターフェイスだけが Render の public 顔。
set(_acs_render_link_private acs_third_party::stb)
if(ACS_RENDER_DX12_RAW)
    list(APPEND _acs_render_link_private d3d12 dxgi d3dcompiler dxguid)
endif()
if(ACS_RENDER_DILIGENT)
    list(APPEND _acs_render_link_private acs_third_party::diligent_core)
endif()

acs_module(
    NAME    Render
    TYPE    Runtime
    SOURCES ${_acs_render_sources}
    HEADERS ${_acs_render_headers}
    PUBLIC_DEPS
        Foundation
        Memory
        Container
        Math
        Platform
        Asset
    LINK_PRIVATE ${_acs_render_link_private}
)

acs_module_feature(MODULE Render NAME DX12_RAW
    DEFINE RENDER_DX12_RAW
    DESCRIPTION "Use raw DirectX 12 backend"
    DEFAULT ${ACS_RENDER_DX12_RAW})

acs_module_feature(MODULE Render NAME DILIGENT
    DEFINE RENDER_DILIGENT
    DESCRIPTION "Use Diligent Engine as RHI backend (DX12/Vulkan/Metal cross-platform)"
    DEFAULT ${ACS_RENDER_DILIGENT})

# Diligent の Vulkan バックエンドビルド有無 (DiligentDevice.cpp が #if ACS_DILIGENT_VULKAN で判別)
# acs_module_feature は WITH_<DEFINE> を生やすが、ここでは生 ACS_DILIGENT_VULKAN という名前で
# define したいので別途 target_compile_definitions ... と書きたいところだが、
# 既存パターンに揃えて feature 経由で WITH_RENDER_DILIGENT_VULKAN を生やし、
# DiligentDevice.cpp は ACS_DILIGENT_VULKAN もしくは WITH_RENDER_DILIGENT_VULKAN の
# どちらでも見られるよう書く方針にする。
acs_module_feature(MODULE Render NAME DILIGENT_VULKAN
    DEFINE RENDER_DILIGENT_VULKAN
    DESCRIPTION "Build Diligent Vulkan backend (only meaningful with ACS_RENDER_DILIGENT=ON)"
    DEFAULT ${ACS_DILIGENT_VULKAN})
