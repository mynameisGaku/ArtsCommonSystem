// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "render/PipelineStateKey.h"

namespace acs {

/** 固定容量かつ確保不要の PSO キー intern 表。 */
template<u32 Capacity>
class TPipelineStateKeyCache {
    static_assert(Capacity > 0u);

public:
    /**
     * 登録済みキーを検索する。
     *
     * @param key 検索する PSO キー。
     * @param identifier 登録済み識別子。
     * @return 見つかった場合は true。
     */
    bool Find(const FPipelineStateKey& key, u32& identifier) const noexcept {
        /** open addressing の開始 slot。 */
        const u32 start = static_cast<u32>(key.primary % Capacity);
        for (u32 probe = 0u; probe < Capacity; ++probe) {
            /** 現在照合する slot。 */
            const u32 slot = (start + probe) % Capacity;
            /** 現在照合する entry。 */
            const FEntry& entry = m_Entries[slot];
            if (!entry.occupied) return false;
            if (entry.key == key) {
                identifier = entry.identifier;
                return true;
            }
        }
        return false;
    }

    /**
     * キーを検索し、未登録なら新しい識別子で登録する。
     *
     * @param key 検索または登録する PSO キー。
     * @param identifier 既存または新規の識別子。
     * @param found 既存キーが見つかった場合は true。
     * @return 検索または登録できた場合は true、満杯なら false。
     */
    bool FindOrIntern(const FPipelineStateKey& key, u32& identifier, bool& found) noexcept {
        /** 線形探索の開始位置。 */
        const u32 start = static_cast<u32>(key.primary % Capacity);
        for (u32 probe = 0u; probe < Capacity; ++probe) {
            /** 今回検査する表の位置。 */
            const u32 slot = (start + probe) % Capacity;
            /** 今回検査する登録要素。 */
            FEntry& entry = m_Entries[slot];
            if (!entry.occupied) {
                entry.key = key;
                entry.identifier = m_NextIdentifier++;
                entry.occupied = true;
                identifier = entry.identifier;
                found = false;
                ++m_Size;
                return true;
            }
            if (entry.key == key) {
                identifier = entry.identifier;
                found = true;
                return true;
            }
        }
        return false;
    }

    /** 登録済みキー数を返す。 */
    u32 Size() const noexcept { return m_Size; }

    /** 登録済みキーを全て定数時間相当の固定表初期化で破棄する。 */
    void Reset() noexcept {
        for (u32 index = 0u; index < Capacity; ++index) m_Entries[index] = FEntry{};
        m_NextIdentifier = 0u;
        m_Size = 0u;
    }

private:
    /** 一つの登録済み PSO キー。 */
    struct FEntry {
        /** 登録した PSO キー。 */
        FPipelineStateKey key{};
        /** 呼び出し側へ返す識別子。 */
        u32 identifier = 0u;
        /** この位置が使用済みなら true。 */
        bool occupied = false;
    };

    /** open addressing で検索する固定表。 */
    FEntry m_Entries[Capacity]{};
    /** 次に発行する識別子。 */
    u32 m_NextIdentifier = 0u;
    /** 登録済みキー数。 */
    u32 m_Size = 0u;
};

} // namespace acs
