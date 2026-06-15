// SPDX-License-Identifier: Apache-2.0
// ACS_CLASS / ACS_PROPERTY — UE 風のリフレクションマーカー。
//
// 使い方:
//   ACS_CLASS()
//   class MYGAME_API FMover : public acs::game::FComponent2D {
//   public:
//       ACS_GAME_COMPONENT_KIND(FMover)
//       ACS_PROPERTY() float speed = 3.0f;     // インスペクタに出すプロパティ
//       void OnUpdate(acs::f32 dt) noexcept override {}
//   };
//
// これらのマクロ自体は «何も生成しない» (UHT の UCLASS/UPROPERTY と同じ)。エディタの
// リフレクション・コードジェネレータが Source/*.h を走査してこのマーカーを見つけ、
// ACS_REGISTER_COMPONENT(FMover, ACS_RPROP_F("speed", 3.0f)) 相当の登録コードを自動生成する。
// 従来の手書き ACS_REGISTER_COMPONENT も引き続き使える (併用可)。
#pragma once

#define ACS_CLASS(...)
#define ACS_PROPERTY(...)
// ACS_FUNCTION(BlueprintCallable, CallInEditor, ...) — 引数なし void メソッドに付けると、
// codegen が ACS_REGISTER_METHOD 相当を生成し、エディタのボタンや将来の Blueprint から呼べる。
#define ACS_FUNCTION(...)
