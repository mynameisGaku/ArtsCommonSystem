# Hello Net: TCP echo サーバ + クライアント（同一プロセスで両方動かす）
add_executable(hello_net main.cpp)
acs_apply_compiler_options(hello_net)
target_link_libraries(hello_net PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Container
    ACS::Network
)
