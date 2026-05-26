acs_module(
    NAME    Container
    TYPE    Runtime
    SOURCES
        FString.cpp
        Hash.cpp
    HEADERS
        TSpan.h
        TArray.h
        FStringView.h
        FString.h
        Hash.h
        THashMap.h
    PUBLIC_DEPS
        Foundation
        Memory
)

acs_module_feature(MODULE Container NAME HASHMAP
    DEFINE CONTAINER_HASHMAP
    DESCRIPTION "Include Robin-Hood THashMap"
    DEFAULT ON)

acs_module_feature(MODULE Container NAME STRING_SSO
    DEFINE CONTAINER_STRING_SSO
    DESCRIPTION "Use small-string optimization for FString (22 inline bytes)"
    DEFAULT ON)
