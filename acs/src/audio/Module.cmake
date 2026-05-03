acs_module(
    NAME    Audio
    TYPE    Runtime
    SOURCES
        AudioEngine.cpp
        SoundHandle.cpp
    HEADERS
        AudioEngine.h
        SoundHandle.h
    PUBLIC_DEPS
        Foundation
        Memory
        Container
        Threading
        Asset
    LINK_PUBLIC
        xaudio2
        ole32
)

acs_module_feature(MODULE Audio NAME XAUDIO2
    DEFINE AUDIO_XAUDIO2
    DESCRIPTION "Use XAudio2 backend"
    DEFAULT ON)
