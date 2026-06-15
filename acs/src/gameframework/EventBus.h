// SPDX-License-Identifier: Apache-2.0
// GameFramework — FEventBus (ワールド内オブジェクト間の疎結合な通信)
//
// 「名前付きイベントの pub/sub」を提供する World サブシステム。発火側と購読側が
// «互いを知らずに» やり取りできる(プレイヤー死亡・スコア加算・ドア開放 等)。
// 直接参照(ObjectRef)が «1対1の固い結線» なのに対し、こちらは «1対多の緩い通知»。
//
// ACS 規約: std::function / std::string 不使用。イベントはコンパイル時ハッシュ ID、
// ハンドラは «関数ポインタ + listener コンテキスト»(captureless ラムダ or 静的関数)。
//
// 使い方:
//   // 購読 (例: コンポーネントの OnAttach で)
//   auto* bus = GetSubsystem<acs::game::FEventBus>();
//   m_Sub = bus->Subscribe(acs::game::FEventBus::Id("PlayerDied"),
//       [](void* self, const void* payload) noexcept {
//           static_cast<MyComp*>(self)->OnPlayerDied();
//       }, this);
//   // 発火 (どこからでも)
//   bus->Publish(acs::game::FEventBus::Id("PlayerDied"));
//   // 解除 (OnDetach 等で)
//   bus->Unsubscribe(m_Sub);
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Subsystem.h"
#include "gameframework/Reflect.h"   // AcsTypeHash (イベント名 → 安定 ID)

namespace acs::game {

/**
 * 名前付きイベントの pub/sub を行う World サブシステム(オブジェクト間の疎結合通信)。
 */
class FEventBus : public FSubsystem {
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
     * @param ev       購読するイベント ID(FEventBus::Id("...") )。
     * @param fn       ハンドラ(captureless。null は無視)。
     * @param listener ハンドラへ渡すコンテキスト(通常は this)。
     * @return 解除用ハンドル(>=0)。失敗で -1。
     */
    int Subscribe(EventId ev, HandlerFn fn, void* listener = nullptr) noexcept {
        if (fn == nullptr) return -1;
        for (u32 i = 0; i < m_Subs.Size(); ++i) {                 // 空きスロット再利用
            if (m_Subs[i].fn == nullptr) { m_Subs[i] = Sub{ ev, fn, listener }; return static_cast<int>(i); }
        }
        m_Subs.PushBack(Sub{ ev, fn, listener });
        return static_cast<int>(m_Subs.Size() - 1);
    }

    /** Subscribe が返したハンドルで購読を解除する。 */
    void Unsubscribe(int handle) noexcept {
        if (handle >= 0 && static_cast<u32>(handle) < m_Subs.Size()) m_Subs[static_cast<u32>(handle)].fn = nullptr;
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
        for (u32 i = 0; i < m_Subs.Size(); ++i) {
            const Sub s = m_Subs[i];   // 値コピー(ハンドラ内で m_Subs が再確保されても安全)
            if (s.ev == ev && s.fn != nullptr) s.fn(s.listener, payload);
        }
    }

    /** 現在の購読数(空きスロット含む。デバッグ/検証用)。 */
    u32 SubscriptionSlots() const noexcept { return static_cast<u32>(m_Subs.Size()); }

    void OnDeinitialize() noexcept override { m_Subs.Clear(); }

private:
    struct Sub { EventId ev; HandlerFn fn; void* listener; };
    TArray<Sub> m_Subs;
};

} // namespace acs::game
