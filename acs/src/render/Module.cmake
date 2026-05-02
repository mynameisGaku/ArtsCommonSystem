acs_module(
    NAME    Render
    TYPE    Runtime
    SOURCES
        Renderer.cpp
        Dx12/Dx12Device.cpp
        Dx12/Dx12Swapchain.cpp
        Dx12/Dx12CommandList.cpp
        Dx12/Dx12Buffer.cpp
        Dx12/Dx12Shader.cpp
        Dx12/Dx12Pipeline.cpp
    HEADERS
        RhiTypes.h
        IRhiDevice.h
        IRhiSwapchain.h
        IRhiCommandList.h
        IRhiBuffer.h
        IRhiShader.h
        IRhiPipeline.h
        Renderer.h
        Dx12/Dx12Common.h
        Dx12/Dx12Device.h
        Dx12/Dx12Swapchain.h
        Dx12/Dx12CommandList.h
        Dx12/Dx12Buffer.h
        Dx12/Dx12Shader.h
        Dx12/Dx12Pipeline.h
    PUBLIC_DEPS
        Foundation
        Memory
        Container
        Math
        Platform
    LINK_PUBLIC
        d3d12
        dxgi
        d3dcompiler
        dxguid
)

acs_module_feature(MODULE Render NAME DX12_RAW
    DEFINE RENDER_DX12_RAW
    DESCRIPTION "Use raw DirectX 12 backend"
    DEFAULT ON)
