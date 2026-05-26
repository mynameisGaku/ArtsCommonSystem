// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — AppState (Phase 2)
//
// シーン跨ぎで生存する型消去の永続状態スロット。Game が 1 つ保持し、
// 任意のシーンから `GetGame().AppState<T>()` で取り出せる。
//
// 使い方:
//   struct PlayerProfile { int hi_score = 0; };
//
//   // 起動時 (OnStart や InitialScene::OnEnter で):
//   GetGame().EmplaceAppState<PlayerProfile>();
//
//   // 任意のシーンで:
//   auto* prof = GetGame().AppState<PlayerProfile>();
//   if (prof) prof->hi_score = 9999;
//
// 設計:
//   ・RTTI 不使用。型 ID は `template static const int` のアドレスを使う
//     (各 T インスタンス化で別アドレス = 一意 ID)。
//   ・Allocator はデフォルト固定。ACS の New/Delete を使う。
//   ・1 Game あたり 1 個。複数の独立した状態が欲しい場合は struct にまとめる。
//   ・wrong-type Get は nullptr を返す (例外なし、ACS 流)。
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"          // Forward
#include "memory/Allocator.h"
#include "memory/New.h"               // New<T> / Delete

namespace acs::game {

class AppStateSlot {
public:
    AppStateSlot() noexcept = default;
    ~AppStateSlot() noexcept { Reset(); }

    AppStateSlot(const AppStateSlot&)            = delete;
    AppStateSlot& operator=(const AppStateSlot&) = delete;

    // 既存スロットを破棄して T を in-place 構築。戻り値は参照。
    template<typename T, typename... Args>
    T& Emplace(Args&&... args) noexcept {
        Reset();
        Allocator& a = DefaultAllocator();
        T* p = New<T>(a, Forward<Args>(args)...);
        _data    = p;
        _alloc   = &a;
        _type_id = TypeId<T>();
        _destroy = +[](void* ptr, Allocator& al) noexcept {
            T* tp = static_cast<T*>(ptr);
            Delete(al, tp);
        };
        return *p;
    }

    // 型 T として取り出す。未設定 or 型不一致なら nullptr。
    template<typename T>
    T* Get() noexcept {
        if (_type_id != TypeId<T>()) return nullptr;
        return static_cast<T*>(_data);
    }

    void Reset() noexcept {
        if (_destroy != nullptr && _data != nullptr && _alloc != nullptr) {
            _destroy(_data, *_alloc);
        }
        _data    = nullptr;
        _alloc   = nullptr;
        _type_id = nullptr;
        _destroy = nullptr;
    }

    bool IsSet() const noexcept { return _data != nullptr; }

private:
    // 型 T 一意 ID = static int のアドレス (T ごとに別 instantiation = 別アドレス)。
    // ACS 規約: RTTI 不使用、type_info 不使用。
    template<typename T>
    static const void* TypeId() noexcept {
        static const int s_tag = 0;
        return static_cast<const void*>(&s_tag);
    }

    void*       _data    = nullptr;
    Allocator*  _alloc   = nullptr;
    const void* _type_id = nullptr;
    void(*_destroy)(void*, Allocator&) noexcept = nullptr;
};

} // namespace acs::game
