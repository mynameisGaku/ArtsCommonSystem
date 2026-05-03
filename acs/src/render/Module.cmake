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
    SpriteBatch.cpp
    Font.cpp
    Sky.cpp
    Particles.cpp
    SkinnedShader.cpp
    ShadowMap.cpp
    PostProcess.cpp
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
    SpriteBatch.h
    Font.h
    Sky.h
    Particles.h
    SkinnedShader.h
    ShadowMap.h
    PostProcess.h
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
    list(APPEND _acs_render_headers
        Dx12/Dx12Common.h
        Dx12/Dx12Device.h
        Dx12/Dx12Swapchain.h
        Dx12/Dx12CommandList.h
        Dx12/Dx12Buffer.h
        Dx12/Dx12Shader.h
        Dx12/Dx12Pipeline.h
        Dx12/Dx12Texture.h
    )
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
    list(APPEND _acs_render_headers
        Diligent/DiligentCommon.h
        Diligent/DiligentDevice.h
        Diligent/DiligentSwapchain.h
        Diligent/DiligentCommandList.h
        Diligent/DiligentBuffer.h
        Diligent/DiligentShader.h
        Diligent/DiligentPipeline.h
        Diligent/DiligentTexture.h
        Diligent/DiligentMemoryAdapter.h
    )
endif()

set(_acs_render_link_public d3d12 dxgi d3dcompiler dxguid)
set(_acs_render_link_private acs_third_party::stb)

if(ACS_RENDER_DILIGENT)
    list(APPEND _acs_render_link_public acs_third_party::diligent_core)
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
    LINK_PUBLIC  ${_acs_render_link_public}
)

acs_module_feature(MODULE Render NAME DX12_RAW
    DEFINE RENDER_DX12_RAW
    DESCRIPTION "Use raw DirectX 12 backend"
    DEFAULT ${ACS_RENDER_DX12_RAW})

acs_module_feature(MODULE Render NAME DILIGENT
    DEFINE RENDER_DILIGENT
    DESCRIPTION "Use Diligent Engine as RHI backend (DX12/Vulkan/Metal cross-platform)"
    DEFAULT ${ACS_RENDER_DILIGENT})
