acs_module(
    NAME    Network
    TYPE    Runtime
    SOURCES
        Network.cpp
        TcpListener.cpp
        TcpConnection.cpp
        UdpSocket.cpp
    HEADERS
        IpAddress.h
        Network.h
        TcpListener.h
        TcpConnection.h
        UdpSocket.h
    PUBLIC_DEPS
        Foundation
        Memory
        FContainer
    LINK_PUBLIC
        ws2_32
)
