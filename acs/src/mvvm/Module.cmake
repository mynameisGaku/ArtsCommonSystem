# コアソース (Imgui に依存しない)
set(_acs_mvvm_sources Mvvm.cpp Convert.cpp)
set(_acs_mvvm_headers
    Observable.h
    ObservableArray.h
    Binder.h
    Derived.h
    Command.h
    Convert.h
    ViewModel.h
)
set(_acs_mvvm_public_deps Foundation Memory Container Math Threading)

# ImGui アダプタは ACS_MVVM_IMGUI_BINDINGS が ON かつ Imgui module が build
# 可能 (= ACS_RENDER_DX12_RAW=ON) のときだけ追加。Diligent 単独 build では
# Imgui module が skip されるので自動で外す。
if(ACS_MVVM_IMGUI_BINDINGS AND ACS_RENDER_DX12_RAW)
    list(APPEND _acs_mvvm_sources ImguiBindings.cpp)
    list(APPEND _acs_mvvm_headers ImguiBindings.h)
    list(APPEND _acs_mvvm_public_deps Imgui)
endif()

acs_module(
    NAME    Mvvm
    TYPE    Runtime
    SOURCES ${_acs_mvvm_sources}
    HEADERS ${_acs_mvvm_headers}
    PUBLIC_DEPS ${_acs_mvvm_public_deps}
)

acs_module_feature(MODULE Mvvm NAME IMGUI_BINDINGS
    DEFINE MVVM_IMGUI_BINDINGS
    DESCRIPTION "Provide ImGui-based binding helpers (acs::mvvm::imgui::Bind*)"
    DEFAULT ${ACS_MVVM_IMGUI_BINDINGS})
