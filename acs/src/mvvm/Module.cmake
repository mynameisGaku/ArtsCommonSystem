acs_module(
    NAME    Mvvm
    TYPE    Runtime
    SOURCES
        Mvvm.cpp
        Convert.cpp
        ImguiBindings.cpp
    HEADERS
        Observable.h
        ObservableArray.h
        Binder.h
        Derived.h
        Command.h
        Convert.h
        ViewModel.h
        ImguiBindings.h
    PUBLIC_DEPS
        Foundation
        Container
        Math
        Imgui
)

# ImGui アダプタはオプション feature。Imgui モジュール側の WITH_ACS_IMGUI に依存する。
acs_module_feature(MODULE Mvvm NAME IMGUI_BINDINGS
    DEFINE MVVM_IMGUI_BINDINGS
    DESCRIPTION "Provide ImGui-based binding helpers (acs::mvvm::imgui::Bind*)"
    DEFAULT ON)
