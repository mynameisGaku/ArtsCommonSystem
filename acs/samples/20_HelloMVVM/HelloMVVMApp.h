// SPDX-License-Identifier: Apache-2.0
// HelloMVVM — FApplication 派生クラス。
// PlayerVM を保持し、ImGui で MVVM の主要機能 (Observable / Binder / Derived /
// ObservableArray / Command) を 4 セクションに分けて紹介する。
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
    void OnEvent(const acs::Event& e) noexcept override;

private:
    acs::ImGuiCtx                                       m_Imgui;
    PlayerVM                                            m_Vm;
    acs::Observable<acs::f32>                           m_HpMirror{};
    acs::Observable<acs::FString>                        m_HpText  { acs::FString{} };
    acs::TUniquePtr<acs::OneWayBinder<acs::f32>>         m_HpMirrorBinder;
    acs::TUniquePtr<acs::OneWayConvertBinder<acs::f32, acs::FString>> m_HpTextBinder;
    acs::Derived<acs::f32, acs::f32>*                   m_Ratio = nullptr;
    char                                                m_NameBuf[64] = "勇者";
};

} // namespace hellomvvm
