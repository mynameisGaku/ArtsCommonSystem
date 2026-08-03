// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — CSceneEventBus
//
// シーン内 component / system 間の type-erased pub/sub。同一 AScene に居る
// オブジェクト同士が「相手の存在」を知らずに通知をやり取りするための薄い
// メッセージング層。FActionId と同じ compile-time FNV-1a hash で event 名を
// u32 に畳み、ハンドラは関数ポインタ + void* user で type-erase する
// (`std::function` 不使用、STL 不使用)。
//
// 使い方:
//   class AEnemySpawner : public AComponent {
//   public:
//       void BindEvents(CSceneEventBus& events) noexcept {
//           m_Events = &events;
//           m_Sub = events.Subscribe(
//               FEventId("PlayerDied"), &OnPlayerDied, this);
//       }
//       void OnDetach() noexcept override {
//           if (m_Events != nullptr) m_Events->Unsubscribe(m_Sub);
//           m_Events = nullptr;
//           m_Sub = 0;
//       }
//   private:
//       static void OnPlayerDied(void* user,
//                                const void* /*payload*/, u32 /*size*/) noexcept {
//           static_cast<AEnemySpawner*>(user)->FreezeSpawning();
//       }
//       CSceneEventBus* m_Events = nullptr;
//       u32 m_Sub = 0;
//   };
//
//   // AScene 等が所有する bus を BindEvents で明示注入し、別 component から publish:
//   FPlayerDiedPayload p{ pos, cause };
//   events.Publish(FEventId("PlayerDied"), &p, sizeof(p));
//
// 設計選択:
//   ・**compile-time hash**: FEventId は `constexpr` FNV-1a で生成、`FEventId("name")`
//     は配置で完結 (実行時 string compare なし)。FInputMap::ActionHash と同一の
//     FNV-1a を採用し、行儀よく u32 衝突は実用上無視。
//   ・**function pointer + void* user**: FSequence::Call と同じ ACS 規約。
//     `std::function` を持ち込むと heap allocation / RTTI / 例外の連鎖が起きる
//     ためフレームワーク層では一切採用しない。
//   ・**handle ベースの Unsubscribe**: Subscribe 毎にユニークな u32 handle を
//     払い出し、削除は FEntry を mark-inactive。Publish 走査時に inactive を
//     スキップ。Publish 中の Unsubscribe / Subscribe を安全にするため
//     **vector の物理削除はしない** (再エントランシ安全)。
//   ・**Publish 中の Subscribe 安全性**: Add で再 alloc が起きると
//     走査中の参照が無効化されるため、Publish の走査は size を最初に
//     キャプチャしてその範囲のみ呼ぶ。Publish 中に追加された subscriber は
//     次回以降の Publish で初めて呼ばれる (一般的な pub/sub セマンティクス)。
//   ・**非コピー / 非ムーブ**: AScene にメンバとして埋め込む前提、所有権の
//     ambiguity を持ち込まない。
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "container/Array.h"

namespace acs::game {

/**
 * イベント名を 32bit に畳む compile-time FNV-1a ハッシュ。
 *
 * @details FInputMap::ActionHash と同一の FNV-1a を採用する。実行時の文字列比較を避けるため、
 * FEventId の生成は constexpr で完結する。
 * @param s ハッシュ対象の NUL 終端文字列。
 * @return 文字列の FNV-1a ハッシュ値 (32bit)。
 */
constexpr u32 EventHash(const char* s) noexcept {
    u32 h = 2166136261u;
    while (*s != '\0') {
        h ^= static_cast<u32>(static_cast<unsigned char>(*s));
        h *= 16777619u;
        ++s;
    }
    return h;
}

/**
 * イベント識別子。
 *
 * @details 文字列リテラルから constexpr (EventHash) で生成し、内部は u32 のハッシュ値で保持する。
 */
struct FEventId {
    /** イベント名の FNV-1a ハッシュ値。 */
    u32 value = 0;

    /** 無効値 (value == 0) で構築する。 */
    constexpr FEventId() noexcept = default;

    /**
     * ハッシュ値を直接指定して構築する。
     *
     * @param v 設定するハッシュ値。
     */
    constexpr explicit FEventId(u32 v) noexcept : value(v) {}

    /**
     * イベント名文字列から構築する (constexpr で EventHash を適用)。
     *
     * @param name イベント名の NUL 終端文字列。
     */
    constexpr FEventId(const char* name) noexcept : value(EventHash(name)) {}

    /**
     * 2 つの FEventId が同一かを返す。
     *
     * @param o 比較対象の FEventId。
     * @return ハッシュ値が等しければ true。
     */
    constexpr bool operator==(FEventId o) const noexcept { return value == o.value; }

    /**
     * 2 つの FEventId が異なるかを返す。
     *
     * @param o 比較対象の FEventId。
     * @return ハッシュ値が異なれば true。
     */
    constexpr bool operator!=(FEventId o) const noexcept { return value != o.value; }
};

/**
 * イベントハンドラ関数の型 (type-erased、std::function 不使用)。
 *
 * @details payload は type-erased で、そのサイズ妥当性の検証は呼び出し側の責任。
 * @param user Subscribe 時に渡したコンテキストポインタ (this 想定)。
 * @param payload Publish で渡されたバイト列の先頭 (null 可)。
 * @param payload_size payload のバイト長 (0 可)。
 */
using HandlerFn = void(*)(void* user, const void* payload, u32 payload_size) noexcept;

/**
 * シーン内 component / system 間の type-erased pub/sub バス。
 *
 * @details
 * 同一 AScene のオブジェクト同士が「相手の存在」を知らずに通知をやり取りするための薄い
 * メッセージング層。FEventId (compile-time FNV-1a ハッシュ) で event を識別し、ハンドラは
 * 関数ポインタ + void* user で type-erase する。Subscribe ごとにユニークな u32 handle を
 * 払い出し、Unsubscribe は FEntry を mark-inactive するのみで物理削除しないため、Publish 中の
 * Subscribe / Unsubscribe を安全に行える。AScene に埋め込む前提の非コピー・非ムーブ型。
 */
class CSceneEventBus {
public:
    /** 空のイベントバスを構築する。 */
    CSceneEventBus() noexcept = default;

    /** イベントバスを破棄する。 */
    ~CSceneEventBus() noexcept = default;

    /** コピー禁止 (AScene にメンバとして埋め込む前提)。 */
    CSceneEventBus(const CSceneEventBus&)            = delete;

    /** コピー代入も禁止。 */
    CSceneEventBus& operator=(const CSceneEventBus&) = delete;

    /** ムーブ禁止 (所有権の ambiguity を持ち込まないため)。 */
    CSceneEventBus(CSceneEventBus&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CSceneEventBus& operator=(CSceneEventBus&&)      = delete;

    /**
     * 指定 event にハンドラを登録する。
     *
     * @details 既存の inactive FEntry を再利用するか末尾に追加し、毎回ユニークな handle を払い出す。
     * fn が nullptr の場合は登録せず 0 を返す (Warn ログ)。
     * @param id 購読するイベント識別子。
     * @param fn 呼び出すハンドラ関数 (nullptr 不可)。
     * @param user ハンドラに渡すコンテキストポインタ。
     * @return Unsubscribe に使う handle (0 = invalid)。
     */
    u32 Subscribe(FEventId id, HandlerFn fn, void* user) noexcept;

    /**
     * 指定した handle の購読を無効化する。
     *
     * @details FEntry を mark-inactive するのみで物理削除しないため、Publish 中に呼んでも安全。
     * 未知 / 既に無効な handle は no-op。
     * @param handle Subscribe が返した handle。
     */
    void Unsubscribe(u32 handle) noexcept;

    /**
     * 指定 event の active subscriber 全員を登録順に呼ぶ。
     *
     * @details payload は呼び出し中のみ有効な参照として扱う。走査範囲は呼び出し時点の Size で
     * 固定するため、Publish 中に Subscribe されたハンドラは次回以降の Publish で初めて呼ばれる。
     * @param id 発行するイベント識別子。
     * @param payload ハンドラに渡すバイト列の先頭 (既定 nullptr)。
     * @param payload_size payload のバイト長 (既定 0)。
     */
    void Publish(FEventId id, const void* payload = nullptr, u32 payload_size = 0) noexcept;

    /**
     * 指定 event の active subscriber 数を返す (debug / test 用)。
     *
     * @param id 数える対象のイベント識別子。
     * @return その event に登録された active なハンドラ数。
     */
    u32 SubscriberCount(FEventId id) const noexcept;

    /** 全 subscription を破棄する (AScene::OnExit 等で使う)。 */
    void ClearAll() noexcept;

private:
    /**
     * 1 件の購読登録を表すエントリ。
     *
     * @details active=false の FEntry は論理削除済みで、再利用または Publish 走査でスキップされる。
     */
    struct FEntry {
        /** 購読対象のイベント識別子。 */
        FEventId   id;

        /** 呼び出すハンドラ関数。 */
        HandlerFn fn     = nullptr;

        /** ハンドラに渡すコンテキストポインタ。 */
        void*     user   = nullptr;

        /** この購読を識別する handle。 */
        u32       handle = 0;

        /** 有効フラグ (false なら論理削除済み)。 */
        bool      active = false;
    };

    /** 全購読エントリ (物理削除せず active フラグで管理)。 */
    TArray<FEntry> m_Entries;

    /** 次に払い出す handle (0 は invalid 予約)。 */
    u32          m_NextHandle = 1u;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FSceneEventBus = CSceneEventBus;

} // namespace acs::game
