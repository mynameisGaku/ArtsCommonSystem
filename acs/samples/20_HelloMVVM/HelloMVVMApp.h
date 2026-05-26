// SPDX-License-Identifier: Apache-2.0
// HelloMVVM — Application 派生クラス。
// PlayerVM を保持して 4 つの ImGui CollapsingHeader (5 分入門 / Binder / Derived /
// Array + Command) で MVVM の主要機能を順番に紹介する。
#pragma once

#include "app/Application.h"
#include "imgui/ImGuiContext.h"

#include "mvvm/ViewModel.h"
#include "mvvm/Derived.h"
#include "mvvm/ObservableArray.h"
#include "mvvm/Command.h"
#include "mvvm/ImguiBindings.h"
#include "container/String.h"
#include "memory/UniquePtr.h"

#include "PlayerVM.h"

namespace hellomvvm {

class HelloMVVMApp : public acs::Application {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;
    void OnEvent(const acs::Event& e) noexcept override;

private:
    acs::ImGuiCtx                                       _imgui;
    PlayerVM                                            _vm;
    acs::Observable<acs::f32>                           _hp_mirror{};
    acs::Observable<acs::String>                        _hp_text  { acs::String{} };
    acs::UniquePtr<acs::OneWayBinder<acs::f32>>         _hp_mirror_binder;
    acs::UniquePtr<acs::OneWayConvertBinder<acs::f32, acs::String>> _hp_text_binder;
    acs::Derived<acs::f32, acs::f32>*                   _ratio = nullptr;
    char                                                _name_buf[64] = "勇者";
};

} // namespace hellomvvm
