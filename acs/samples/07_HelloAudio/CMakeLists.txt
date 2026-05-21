# Hello Audio: ファイルから音を読んで再生する例
add_executable(hello_audio WIN32 main.cpp)
acs_apply_compiler_options(hello_audio)
target_link_libraries(hello_audio PRIVATE
    ACS::Game
    ACS::Audio   # XAudio2 による音声再生（標準セットに含まれない追加モジュール）
)
