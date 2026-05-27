// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — AppState (Phase 2)
//
// シーン跨ぎで生存する型消去の永続状態スロット。FGame が 1 つ保持し、
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
//   ・FAllocator はデフォルト固定。ACS の New/Delete を使う。
//   ・1 FGame あたり 1 個。複数の独立した状態が欲しい場合は struct にまとめる。
//   ・wrong-type Get は nullptr を返す (例外なし、ACS 流)。
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"          // Forward
#include "memory/Allocator.h"
#include "memory/New.h"               // New<T> / Delete

namespace acs::game {

class FAppStateSlot {
public:
    FAppStateSlot() noexcept = default;
    ~FAppStateSlot() noexcept { Reset(); }

    FAppStateSlot(const FAppStateSlot&)            = delete;
    FAppStateSlot& operator=(const FAppStateSlot&) = delete;

    // 既存スロットを破棄して T を in-place 構築。戻り値は参照。
    template<typename T, typename... Args>
    T& Emplace(Args&&... args) noexcept {
        Reset();
        FAllocator& a = DefaultAllocator();
        T* p = New<T>(a, Forward<Args>(args)...);
        m_Data    = p;
        m_Alloc   = &a;
        m_TypeId = TypeId<T>();
        m_Destroy = +[](void* ptr, FAllocator& al) noexcept {
            T* tp = static_cast<T*>(ptr);
            Delete(al, tp);
        };
        return *p;
    }

    // 型 T として取り出す。未設定 or 型不一致なら nullptr。
    template<typename T>
    T* Get() noexcept {
        if (m_TypeId != TypeId<T>()) return nullptr;
        return static_cast<T*>(m_Data);
    }

    void Reset() noexcept {
        if (m_Destroy != nullptr && m_Data != nullptr && m_Alloc != nullptr) {
            m_Destroy(m_Data, *m_Alloc);
        }
        m_Data    = nullptr;
        m_Alloc   = nullptr;
        m_TypeId = nullptr;
        m_Destroy = nullptr;
    }

    bool IsSet() const noexcept { return m_Data != nullptr; }

private:
    // 型 T 一意 ID = static int のアドレス (T ごとに別 instantiation = 別アドレス)。
    // ACS 規約: RTTI 不使用、type_info 不使用。
    template<typename T>
    static const void* TypeId() noexcept {
        static const int s_tag = 0;
        return static_cast<const void*>(&s_tag);
    }

    void*       m_Data    = nullptr;
    FAllocator*  m_Alloc   = nullptr;
    const void* m_TypeId = nullptr;
    void(*m_Destroy)(void*, FAllocator&) noexcept = nullptr;
};

} // namespace acs::game
