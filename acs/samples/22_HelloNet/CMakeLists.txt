# Hello Net: TCP echo サーバ + クライアント (同一プロセスで両方動かす)
#
# 構成:
#   main.cpp            - hellonet::HelloNetApp::Run() を呼ぶだけのエントリ
#   HelloNetApp.{h,cpp} - 一連のフロー (Init → サーバ起動 → 送受信 → Shutdown)
#   EchoServer.{h,cpp}  - Thread::Spawn から呼ぶサーバ本体
add_executable(hello_net
    main.cpp
    HelloNetApp.cpp
    EchoServer.cpp
    HelloNetApp.h
    EchoServer.h
)
acs_apply_compiler_options(hello_net)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "HelloNetApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_net PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_net PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Container
    ACS::Network
)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
