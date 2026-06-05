// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — AppState
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

/**
 * シーン跨ぎで生存する型消去の永続状態スロット。
 *
 * @details
 * FGame が 1 つ保持し、任意のシーンから GetGame().AppState<T>() で取り出せる。
 * RTTI 不使用で型 ID は template static const int のアドレスを使い (T ごとに別アドレス)、
 * FAllocator は DefaultAllocator 固定。wrong-type Get は例外なしで nullptr を返す。
 */
class FAppStateSlot {
public:
    /** 空のスロットを構築する (値は未設定)。 */
    FAppStateSlot() noexcept = default;

    /** 破棄する (保持中の値があれば Reset で解放)。 */
    ~FAppStateSlot() noexcept { Reset(); }

    /** コピー禁止 (型消去した単一所有スロットのため)。 */
    FAppStateSlot(const FAppStateSlot&)            = delete;

    /** コピー代入も禁止。 */
    FAppStateSlot& operator=(const FAppStateSlot&) = delete;

    /**
     * 既存スロットを破棄して T を in-place 構築する。
     *
     * @tparam T 構築する型。
     * @tparam Args T のコンストラクタ引数型。
     * @param args T のコンストラクタへ転送する引数。
     * @return 構築した T への参照。
     */
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

    /**
     * 保持中の値を型 T として取り出す。
     *
     * @tparam T 取り出す型。
     * @return T へのポインタ (未設定または型不一致なら nullptr)。
     */
    template<typename T>
    T* Get() noexcept {
        if (m_TypeId != TypeId<T>()) return nullptr;
        return static_cast<T*>(m_Data);
    }

    /** 保持中の値を破棄してスロットを空にする (多重呼び出し安全)。 */
    void Reset() noexcept {
        if (m_Destroy != nullptr && m_Data != nullptr && m_Alloc != nullptr) {
            m_Destroy(m_Data, *m_Alloc);
        }
        m_Data    = nullptr;
        m_Alloc   = nullptr;
        m_TypeId = nullptr;
        m_Destroy = nullptr;
    }

    /**
     * 値が設定されているかを返す。
     *
     * @return 値が保持されていれば true。
     */
    bool IsSet() const noexcept { return m_Data != nullptr; }

private:
    /**
     * 型 T 一意の ID を返す (static int のアドレス、T ごとに別 instantiation = 別アドレス)。
     *
     * @details ACS 規約: RTTI 不使用、type_info 不使用。
     * @tparam T ID を求める型。
     * @return T を一意に表すポインタ。
     */
    template<typename T>
    static const void* TypeId() noexcept {
        static const int s_tag = 0;
        return static_cast<const void*>(&s_tag);
    }

    /** 保持中の値への型消去ポインタ。 */
    void*       m_Data    = nullptr;

    /** 値を確保したアロケータ (Reset で解放に使う)。 */
    FAllocator*  m_Alloc   = nullptr;

    /** 保持中の値の型 ID (TypeId<T>() の戻り値)。 */
    const void* m_TypeId = nullptr;

    /** 値を破棄する型消去デストラクタ関数。 */
    void(*m_Destroy)(void*, FAllocator&) noexcept = nullptr;
};

} // namespace acs::game
