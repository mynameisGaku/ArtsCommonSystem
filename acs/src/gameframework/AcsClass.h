// SPDX-License-Identifier: Apache-2.0
// ACS_CLASS / ACS_PROPERTY — ACS のリフレクションマーカー。
//
// 使い方:
//   ACS_CLASS()
//   class MYGAME_API AMover : public acs::game::AComponent {
//   public:
//       ACS_GAME_COMPONENT_KIND(AMover)
//       ACS_PROPERTY() float speed = 3.0f;     // インスペクタに出すプロパティ
//       void OnUpdate(acs::f32 dt) noexcept override {}
//   };
//
// これらのマクロ自体は «何も生成しない»。エディタの
// リフレクション・コードジェネレータが Source/*.h を走査してこのマーカーを見つけ、
// ACS_REGISTER_COMPONENT(AMover, ACS_RPROP_F("speed", 3.0f)) 相当の登録コードを自動生成する。
// 従来の手書き ACS_REGISTER_COMPONENT も引き続き使える (併用可)。
#pragma once

#define ACS_CLASS(...)
#define ACS_PROPERTY(...)
// ACS_FUNCTION(...) は反射対象メソッドと公開指定子を示す無処理マクロ。
// ゲーム側の ACS_CLASS 内にある引数なし void メソッドは codegen が登録コードを生成し、Engine 組込み型は ReflectCatalog で登録する。
// 登録後は指定子に応じてエディタのボタンや Blueprint グラフから呼び出せる。
#define ACS_FUNCTION(...)
