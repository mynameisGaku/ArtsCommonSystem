## HelloAudio — ファイルから音を読んで再生する例。
##
## 構成:
##   main.cpp               - ACS_DEFINE_MAIN(HelloAudioApp) のみ
##   HelloAudioApp.{h,cpp}  - Application 派生クラス
add_executable(hello_audio WIN32
    main.cpp
    HelloAudioApp.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    HelloAudioApp.h
)
acs_apply_compiler_options(hello_audio)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を
# "HelloAudioApp.h" のように local include で書けるようにする)
target_include_directories(hello_audio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_audio PRIVATE
    ACS::Game
    ACS::Audio   # XAudio2 による音声再生（標準セットに含まれない追加モジュール）
)
