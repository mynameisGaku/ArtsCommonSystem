# SPDX-License-Identifier: Apache-2.0
# MlOnnx module - real ONNX Runtime backend for acs::game::IMlRuntime.

if(NOT ACS_BUILD_ML_ONNX)
    return()
endif()

include(ACSThirdParty)
acs_third_party_onnxruntime()

acs_module(
    NAME    MlOnnx
    TYPE    Runtime
    SOURCES
        OnnxMlRuntime.cpp
    HEADERS
        OnnxMlRuntime.h
    PUBLIC_DEPS
        Foundation
        GameFramework
    LINK_PUBLIC
        acs_third_party_onnxruntime
)
