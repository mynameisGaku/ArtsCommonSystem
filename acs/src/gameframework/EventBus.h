// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Forward.h"
#include "gameframework/Subsystem.h"
#include "gameframework/Reflect.h"   // AcsTypeHash (イベント名 → 安定 ID)

namespace acs::game {

/**
 * 名前付きイベントの pub/sub を行う World サブシステム(オブジェクト間の疎結合通信)。
 */
class AEventBus : public ASubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(FEventBus)

    /** イベント識別子(名前の FNV-1a ハッシュ)。 */
    using EventId   = u32;

    /** イベントハンドラ(listener=Subscribe で渡したコンテキスト、payload=Publish のデータ)。 */
    using HandlerFn = void (*)(void* listener, const void* payload);

    /** イベント名 → 安定 ID(コンパイル時)。Publish/Subscribe で同じ名前を使う。 */
    static constexpr EventId Id(const char* name) noexcept { return AcsTypeHash(name); }

    /**
     * イベント ev を購読する。発火時に fn(listener, payload) が呼ばれる。
     *
     * @param ev       購読するイベント ID(AEventBus::Id("...") )。
     * @param fn       ハンドラ(captureless。null は無視)。
     * @param listener ハンドラへ渡すコンテキスト(通常は this)。
     * @return 解除用ハンドル(>=0)。失敗で -1。
     */
    int Subscribe(EventId ev, HandlerFn fn, void* listener = nullptr) noexcept {
        if (fn == nullptr) return -1;
        for (u32 i = 0; i < m_Subs.Num(); ++i) {                 // 空きスロット再利用
            if (m_Subs[i].fn == nullptr) { m_Subs[i] = FSub{ ev, fn, listener }; return static_cast<int>(i); }
        }
        m_Subs.Add(FSub{ ev, fn, listener });
        return static_cast<int>(m_Subs.Num() - 1);
    }

    /** Subscribe が返したハンドルで購読を解除する。 */
    void Unsubscribe(int handle) noexcept {
        if (handle >= 0 && static_cast<u32>(handle) < m_Subs.Num()) m_Subs[static_cast<u32>(handle)].fn = nullptr;
    }

    /**
     * イベント ev を発火し、購読中の全ハンドラを payload 付きで呼ぶ。
     *
     * @details ハンドラ内からの Subscribe/Unsubscribe にも耐える(各スロットを «値コピー» してから
     * 呼ぶので、配列が再確保されても安全。発火中に追加された購読は呼ばれる場合がある)。
     * @param ev      発火するイベント ID。
     * @param payload ハンドラへ渡すデータ(任意、既定 nullptr)。
     */
    void Publish(EventId ev, const void* payload = nullptr) noexcept {
        for (u32 i = 0; i < m_Subs.Num(); ++i) {
            const FSub s = m_Subs[i];   // 値コピー(ハンドラ内で m_Subs が再確保されても安全)
            if (s.ev == ev && s.fn != nullptr) s.fn(s.listener, payload);
        }
    }

    /** 現在の購読数(空きスロット含む。デバッグ/検証用)。 */
    u32 SubscriptionSlots() const noexcept { return static_cast<u32>(m_Subs.Num()); }

    void OnDeinitialize() noexcept override { m_Subs.Reset(); }

private:
    struct FSub { EventId ev; HandlerFn fn; void* listener; };
    TArray<FSub> m_Subs;
};

} // namespace acs::game
