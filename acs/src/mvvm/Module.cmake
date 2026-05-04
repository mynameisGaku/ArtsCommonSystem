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

# ImGui アダプタは ACS_MVVM_IMGUI_BINDINGS が ON のときだけ追加
if(ACS_MVVM_IMGUI_BINDINGS)
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
