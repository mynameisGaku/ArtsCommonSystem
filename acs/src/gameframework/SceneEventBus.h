// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — SceneEventBus (Phase 1 polish)
//
// シーン内 component / system 間の type-erased pub/sub。同一 Scene に居る
// オブジェクト同士が「相手の存在」を知らずに通知をやり取りするための薄い
// メッセージング層。ActionId と同じ compile-time FNV-1a hash で event 名を
// u32 に畳み、ハンドラは関数ポインタ + void* user で type-erase する
// (`std::function` 不使用、STL 不使用)。
//
// 使い方:
//   class EnemySpawner : public Component {
//       u32 _sub = 0;
//       void OnEnter() noexcept override {
//           _sub = Scene()->Events().Subscribe(
//               EventId("PlayerDied"), &OnPlayerDied, this);
//       }
//       void OnExit() noexcept override {
//           Scene()->Events().Unsubscribe(_sub);
//       }
//       static void OnPlayerDied(void* user,
//                                const void* /*payload*/, u32 /*size*/) noexcept {
//           static_cast<EnemySpawner*>(user)->FreezeSpawning();
//       }
//   };
//
//   // 別 component から:
//   PlayerDiedPayload p{ pos, cause };
//   Scene()->Events().Publish(EventId("PlayerDied"), &p, sizeof(p));
//
// 設計選択 (Phase 1 polish):
//   ・**compile-time hash**: EventId は `constexpr` FNV-1a で生成、`EventId("name")`
//     は配置で完結 (実行時 string compare なし)。InputMap::ActionHash と同一の
//     FNV-1a を採用し、行儀よく u32 衝突は実用上無視。
//   ・**function pointer + void* user**: Sequence::Call と同じ ACS 規約。
//     `std::function` を持ち込むと heap allocation / RTTI / 例外の連鎖が起きる
//     ためフレームワーク層では一切採用しない。
//   ・**handle ベースの Unsubscribe**: Subscribe 毎にユニークな u32 handle を
//     払い出し、削除は Entry を mark-inactive。Publish 走査時に inactive を
//     スキップ。Publish 中の Unsubscribe / Subscribe を安全にするため
//     **vector の物理削除はしない** (再エントランシ安全)。
//   ・**Publish 中の Subscribe 安全性**: PushBack で再 alloc が起きると
//     走査中の参照が無効化されるため、Publish の走査は size を最初に
//     キャプチャしてその範囲のみ呼ぶ。Publish 中に追加された subscriber は
//     次回以降の Publish で初めて呼ばれる (一般的な pub/sub セマンティクス)。
//   ・**非コピー / 非ムーブ**: Scene にメンバとして埋め込む前提、所有権の
//     ambiguity を持ち込まない。
//
// 範囲外 (将来フェーズで):
//   ・優先順位付き subscriber (現状は登録順)
//   ・event filter / wildcard
//   ・cross-scene broadcast (Scene を越えるのは GlobalEventBus が出来てから)
//   ・event 履歴 / replay (determinism Pillar との連携が必要)
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "container/Array.h"

namespace acs::game {

// Compile-time FNV-1a hash (32bit). InputMap::ActionHash と同一。
constexpr u32 EventHash(const char* s) noexcept {
    u32 h = 2166136261u;
    while (*s != '\0') {
        h ^= static_cast<u32>(static_cast<unsigned char>(*s));
        h *= 16777619u;
        ++s;
    }
    return h;
}

// イベント識別子。文字列リテラルから constexpr で生成、内部は u32。
struct EventId {
    u32 value = 0;

    constexpr EventId() noexcept = default;
    constexpr explicit EventId(u32 v) noexcept : value(v) {}
    constexpr EventId(const char* name) noexcept : value(EventHash(name)) {}

    constexpr bool operator==(EventId o) const noexcept { return value == o.value; }
    constexpr bool operator!=(EventId o) const noexcept { return value != o.value; }
};

// ハンドラ関数の型。payload は type-erased、サイズは呼び出し側の責任で検証。
//   - user        : Subscribe 時に渡したコンテキストポインタ (this 想定)
//   - payload     : Publish で渡されたバイト列の先頭 (null 可)
//   - payload_size: payload のバイト長 (0 可)
using HandlerFn = void(*)(void* user, const void* payload, u32 payload_size) noexcept;

class SceneEventBus {
public:
    SceneEventBus() noexcept = default;
    ~SceneEventBus() noexcept = default;

    // 非コピー・非ムーブ (Scene にメンバとして埋め込む前提)
    SceneEventBus(const SceneEventBus&)            = delete;
    SceneEventBus& operator=(const SceneEventBus&) = delete;
    SceneEventBus(SceneEventBus&&)                 = delete;
    SceneEventBus& operator=(SceneEventBus&&)      = delete;

    // ハンドラを登録。戻り値は Unsubscribe 用 handle (0 = invalid)。
    // fn が nullptr の場合は登録せず 0 を返す (Warn ログ)。
    u32 Subscribe(EventId id, HandlerFn fn, void* user) noexcept;

    // handle を無効化。未知 / 既無効 handle は no-op。
    // Publish 中に呼んでも安全 (Entry を物理削除しない)。
    void Unsubscribe(u32 handle) noexcept;

    // 指定 event の active subscriber 全員を登録順に呼ぶ。
    // payload は呼び出し中のみ有効な参照として扱う (handler 側でコピーは禁止しない)。
    // 走査範囲は呼び出し時点の Size で固定 — Publish 中に Subscribe された
    // ハンドラは次回以降の Publish で呼ばれる。
    void Publish(EventId id, const void* payload = nullptr, u32 payload_size = 0) noexcept;

    // 指定 event の active subscriber 数 (debug / test 用)。
    u32 SubscriberCount(EventId id) const noexcept;

    // 全 subscription を破棄。Scene::OnExit 等で使う。
    void ClearAll() noexcept;

private:
    struct Entry {
        EventId   id;
        HandlerFn fn     = nullptr;
        void*     user   = nullptr;
        u32       handle = 0;
        bool      active = false;
    };

    TArray<Entry> _entries;
    u32          _next_handle = 1u;  // 0 は invalid 予約
};

} // namespace acs::game
