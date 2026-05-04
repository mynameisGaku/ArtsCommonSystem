acs_third_party_miniaudio()

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
    LINK_PRIVATE
        acs_third_party::miniaudio
)

# miniaudio は single-header で内部の OS API (WASAPI/ALSA/CoreAudio) を選択する。
# Windows では XAudio2 ではなく WASAPI が使われる。
acs_module_feature(MODULE Audio NAME MINIAUDIO
    DEFINE AUDIO_MINIAUDIO
    DESCRIPTION "Use miniaudio backend (single-header, cross-platform)"
    DEFAULT ON)
