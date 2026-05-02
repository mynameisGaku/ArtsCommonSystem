acs_module(
    NAME    Render
    TYPE    Runtime
    SOURCES
        Renderer.cpp
        Dx12/Dx12Device.cpp
        Dx12/Dx12Swapchain.cpp
        Dx12/Dx12CommandList.cpp
    HEADERS
        RhiTypes.h
        IRhiDevice.h
        IRhiSwapchain.h
        IRhiCommandList.h
        Renderer.h
        Dx12/Dx12Common.h
        Dx12/Dx12Device.h
        Dx12/Dx12Swapchain.h
        Dx12/Dx12CommandList.h
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
