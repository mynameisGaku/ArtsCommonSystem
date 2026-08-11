// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "memory/New.h"
#include "container/Array.h"
#include "ecs/World.h"

namespace acs {

/**
 * CWorld スナップショットの固定容量リングバッファ (rollback netcode の状態履歴)。
 *
 * @details
 * 直近 capacity tick 分の CWorld 状態を tick % capacity の slot に保持する。
 * SaveFrame で現在状態を退避し、RestoreFrame で保存済み tick へ巻き戻す。
 * 保存する CWorld の全コンポーネント型はコピー構築可能である必要がある
 * (CWorld::CopyFrom の契約)。non-copy / non-move 型。
 */
class FRollbackBuffer {
public:
    /** 未初期化 (容量 0) で構築する。使用前に Init を呼ぶ。 */
    FRollbackBuffer() noexcept = default;

    /** 破棄する (保持中の全スナップショット CWorld を解放)。 */
    ~FRollbackBuffer() noexcept { Shutdown(); }

    /** コピー禁止 (CWorld 履歴の誤複製を防ぐ)。 */
    FRollbackBuffer(const FRollbackBuffer&)            = delete;

    /** コピー代入も禁止。 */
    FRollbackBuffer& operator=(const FRollbackBuffer&) = delete;

    /** ムーブ禁止 (長寿命の単独所有オブジェクトのため)。 */
    FRollbackBuffer(FRollbackBuffer&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FRollbackBuffer& operator=(FRollbackBuffer&&)      = delete;

    /**
     * capacity 個のスナップショット slot を確保する。
     *
     * @details
     * 既に初期化済みなら一度 Shutdown してから確保し直す (保存済み履歴は失われる)。
     * 途中で確保に失敗した場合は確保済み分を解放し、未初期化状態で false を返す
     * (部分状態は残さない)。
     * @param capacity 保持する tick 数 (1 以上)。
     * @return 全 slot を確保できたら true。capacity == 0 または OOM は false。
     */
    bool Init(u32 capacity) noexcept
    {
        Shutdown();
        if (capacity == 0) return false;
        if (!m_Slots.TrySetNum(capacity)) return false;
        for (u32 i = 0; i < capacity; ++i) {
            m_Slots[i].world = New<CWorld>(*m_Slots.GetAllocator());
            if (m_Slots[i].world == nullptr) {
                Shutdown();
                return false;
            }
        }
        return true;
    }

    /**
     * 全スナップショットを解放し、未初期化状態へ戻す。
     *
     * @details 繰り返し呼んでも安全。再利用するには Init を呼び直す。
     */
    void Shutdown() noexcept
    {
        for (usize i = 0; i < m_Slots.Num(); ++i) {
            if (m_Slots[i].world) {
                Delete(*m_Slots.GetAllocator(), m_Slots[i].world);
                m_Slots[i].world = nullptr;
            }
        }
        m_Slots = TArray<FSlot>{*m_Slots.GetAllocator()};
    }

    /**
     * 保存済みフレームだけを全て無効化する (slot の CWorld 器は保持)。
     *
     * @details セッション跨ぎで tick が 0 に戻るときなど、古い tick の履歴が
     * 偶然一致して復元されるのを防ぐ。容量は変わらない。
     */
    void InvalidateAll() noexcept
    {
        for (usize i = 0; i < m_Slots.Num(); ++i) m_Slots[i].valid = false;
    }

    /**
     * world の現在状態を tick のスナップショットとして保存する。
     *
     * @details
     * slot は tick % capacity で決まり、そこに残っていた古い tick の履歴は
     * 上書きされる (リングの自然な追い出し)。複製に失敗した場合はその slot を
     * 無効化して false を返す (壊れた状態を後で復元させない)。
     * @param tick この状態が属するフレーム番号。
     * @param world 保存する CWorld。
     * @return 保存できたら true。未初期化・複製失敗 (非コピー型 / OOM) は false。
     */
    bool SaveFrame(u32 tick, const CWorld& world) noexcept
    {
        if (m_Slots.IsEmpty()) return false;
        FSlot& slot = m_Slots[tick % m_Slots.Num()];
        slot.valid = false;   // 複製中に失敗しても古い履歴を残さない
        if (!slot.world->CopyFrom(world)) return false;
        slot.tick  = tick;
        slot.valid = true;
        return true;
    }

    /**
     * 保存済みの tick スナップショットを world へ復元する (巻き戻し)。
     *
     * @details slot に残っている履歴の tick が一致する場合のみ復元する。
     * 容量を超えて上書き済みの古い tick は false になる。復元後も履歴 slot は
     * 有効なまま残る (同じ tick へ複数回巻き戻せる)。
     * @param tick 巻き戻したいフレーム番号。
     * @param world 復元先の CWorld。
     * @return 復元できたら true。履歴なし・tick 不一致・複製失敗は false。
     */
    bool RestoreFrame(u32 tick, CWorld& world) const noexcept
    {
        if (m_Slots.IsEmpty()) return false;
        const FSlot& slot = m_Slots[tick % m_Slots.Num()];
        if (!slot.valid || slot.tick != tick) return false;
        return world.CopyFrom(*slot.world);
    }

    /**
     * tick のスナップショットが復元可能かを返す。
     *
     * @param tick 判定するフレーム番号。
     * @return 保存済みで上書きされていなければ true。
     */
    bool HasFrame(u32 tick) const noexcept
    {
        if (m_Slots.IsEmpty()) return false;
        const FSlot& slot = m_Slots[tick % m_Slots.Num()];
        return slot.valid && slot.tick == tick;
    }

    /**
     * 保持できる tick 数を返す。
     *
     * @return Init で確保した slot 数 (未初期化なら 0)。
     */
    u32 Capacity() const noexcept { return static_cast<u32>(m_Slots.Num()); }

    /**
     * 現在有効なスナップショット数を返す。
     *
     * @return valid な slot の個数。
     */
    u32 SavedCount() const noexcept
    {
        u32 n = 0;
        for (usize i = 0; i < m_Slots.Num(); ++i) {
            if (m_Slots[i].valid) ++n;
        }
        return n;
    }

private:
    /** 1 スナップショット slot (CWorld の器 + どの tick の状態か)。 */
    struct FSlot {
        /** スナップショットの器 (Init で確保し Shutdown まで使い回す)。 */
        CWorld* world = nullptr;

        /** 保存時の tick (valid のときのみ意味を持つ)。 */
        u32    tick  = 0;

        /** この slot に復元可能な履歴が入っているか。 */
        bool   valid = false;
    };

    /** リング本体 (tick % Size() で添字)。 */
    TArray<FSlot> m_Slots;
};

} // namespace acs
