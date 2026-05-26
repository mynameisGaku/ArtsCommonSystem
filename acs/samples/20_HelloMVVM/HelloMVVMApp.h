// SPDX-License-Identifier: Apache-2.0
// HelloMVVM — FApplication 派生クラス。
// PlayerVM を保持し、ImGui で MVVM の主要機能 (FObservable / Binder / FDerived /
// FObservableArray / FCommand) を 4 セクションに分けて紹介する。
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

class HelloMVVMApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;
    void OnEvent(const acs::FEvent& e) noexcept override;

private:
    acs::FImGuiCtx                                       _imgui;
    PlayerVM                                            _vm;
    acs::FObservable<acs::f32>                           _hp_mirror{};
    acs::FObservable<acs::FString>                        _hp_text  { acs::FString{} };
    acs::TUniquePtr<acs::FOneWayBinder<acs::f32>>         _hp_mirror_binder;
    acs::TUniquePtr<acs::FOneWayConvertBinder<acs::f32, acs::FString>> _hp_text_binder;
    acs::FDerived<acs::f32, acs::f32>*                   _ratio = nullptr;
    char                                                _name_buf[64] = "勇者";
};

} // namespace hellomvvm
