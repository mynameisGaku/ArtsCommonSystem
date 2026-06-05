// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FSceneCommandQueue (deferred command queue / editor 連携)
//
// 走査中 (OnUpdate / OnDraw) に発火された「シーン構造変更」等の要求をフレーム末
// で順次実行するための遅延実行キュー。`FSceneManager` の `_ApplyPending` と同じ
// 哲学 (= 走査中の構造変更を避け、安全なフレーム境界で適用する) を、より粒度の
// 細かいコマンド単位に拡張したもの。editor から「ノード追加」「コンポーネント
// 差し替え」等を OnUpdate のループ走査中に呼んでも、Flush 時に integrity を
// 保ったまま適用される。
//
// 使い方:
//   class GameplayScene : public Scene {
//       FSceneCommandQueue m_Cmds;
//
//       void OnUpdate(f32 dt) noexcept override {
//           // 走査中に node 削除を要求しても安全 (Flush でまとめて実行)
//           if (Input::IsKeyPressed(EKey::Delete)) {
//               m_Cmds.Enqueue("DeleteSelected", &GameplayScene::DeleteSelected, this);
//           }
//           // 同 label が既にキュー上に居れば denounce (連打抑制)
//           if (held) {
//               m_Cmds.EnqueueIfAbsent("Refresh", &GameplayScene::RefreshUi, this, /*priority=*/50);
//           }
//       }
//       void OnDraw() noexcept override {
//           // ImGui ボタン処理中などからも安全に enqueue できる
//       }
//
//       // フレーム末に FGame / Scene 側で 1 回 Flush する。
//       static void DeleteSelected(void* self) noexcept { /* ... */ }
//       static void RefreshUi    (void* self) noexcept { /* ... */ }
//   };
//
// 設計選択 (Pillar A polish):
//   ・**deferred 実行**: 走査中の構造変更を避けるため、Flush までは実行しない。
//     `FSceneManager` の pending op (1 個) と違い、複数 command を保持・優先度
//     付きで順序付け実行する。
//   ・**function pointer + void* user**: ACS 規約 (std::function 不使用、heap
//     allocation / RTTI / 例外を持ち込まない)。
//   ・**const char* label**: 文字列リテラル前提、本クラスは複製しない (FDebugOverlay
//     の watch 列と同じ方針)。同一性比較は pointer 一致 → fallback で strcmp。
//     `<string>` 禁止 (STL 不使用)。
//   ・**priority 昇順実行**: 同 priority 内では Enqueue 順 (= 安定ソート)。
//     editor からの "削除" → "再構築" のような順序依存を表現可能。
//   ・**one_shot vs repeating**: Flush 後に one_shot は削除、repeating は残す。
//     debug overlay の定期チェックや editor の continuous validation で使用。
//   ・**EnqueueIfAbsent (denounce)**: 同 label の既存 command があれば no-op。
//     入力連打 / リサイズイベント連発で同じ作業が積み上がるのを防ぐ。
//   ・**Cancel**: label 一致の全 command を削除 (one_shot/repeating 両方)。
//   ・**Flush 中の Enqueue 安全性**: FSceneEventBus と同じく、走査 size を最初に
//     スナップショットして固定範囲のみ実行する。Flush 中に追加された command は
//     次回 Flush で初めて実行される。Flush 中に同 slot が PushBack で再 alloc を
//     起こしても、fn / user / one_shot を local にコピーしてから呼ぶことで安全。
//   ・**非コピー・非ムーブ**: Scene にメンバとして埋め込む前提、所有権の
//     ambiguity を持ち込まない。
//   ・**STL 不使用 / 全 noexcept**: ACS 規約。`acs::TArray<CommandRecord>` で持つ。
//
// 範囲外:
//   ・スレッドセーフ (現状は同一スレッド前提、editor が別スレッドから enqueue する
//     なら mutex を内蔵するか SPSC ring に置き換える必要あり)
//   ・command 履歴 / undo (editor の undo stack は別レイヤで持つべき)
//   ・cross-scene broadcast (FSceneEventBus と同じく Scene を越えるのは将来課題)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * 遅延実行コマンドの本体となる関数ポインタ型。
 *
 * @details user は Enqueue 時に渡したコンテキストポインタ (this 想定)。STL の
 * std::function は使わない (ACS 規約: heap allocation / RTTI / 例外を持ち込まない)。
 * @param user Enqueue 時に渡したコンテキストポインタ。
 */
using SceneCommandFn = void(*)(void* user) noexcept;

/**
 * キュー内の 1 コマンドエントリ。
 *
 * @details label / fn / user は呼出し側所有で、本クラスは複製しない。
 */
struct CommandRecord {
    /** 同一性比較に使うラベル (文字列リテラル前提、複製しない)。 */
    const char* label    = nullptr;

    /** 実行するコマンド本体。 */
    SceneCommandFn   fn       = nullptr;

    /** fn に渡すコンテキストポインタ (this 想定)。 */
    void*       user     = nullptr;

    /** true なら Flush で 1 回実行後に削除、false なら毎 Flush 残す。 */
    bool        one_shot = true;

    /** 実行順 (昇順、低いほど先に実行。既定 100)。 */
    u32         priority = 100u;
};

/**
 * 走査中に発火した構造変更要求をフレーム末で順次実行する遅延実行キュー。
 *
 * @details
 * OnUpdate / OnDraw の走査中に editor 等が「ノード追加」「コンポーネント差し替え」を
 * 要求しても、Flush 時に integrity を保ったまま priority 昇順で適用する。FSceneManager の
 * 単一 pending op と違い複数 command を保持し、one_shot/repeating・denounce・cancel を持つ。
 * 非コピー・非ムーブで、関数ポインタ + void* user により STL を使わず全 noexcept で実装する。
 */
class FSceneCommandQueue {
public:
    /** 空のキューを構築する。 */
    FSceneCommandQueue() noexcept = default;

    /** キューを破棄する (保留 command の callback は呼ばない)。 */
    ~FSceneCommandQueue() noexcept = default;

    /** コピー禁止 (発火中の参照との競合を防ぐため)。 */
    FSceneCommandQueue(const FSceneCommandQueue&)            = delete;

    /** コピー代入も禁止。 */
    FSceneCommandQueue& operator=(const FSceneCommandQueue&) = delete;

    /** ムーブ禁止 (Scene 埋め込み前提、所有権の曖昧さを持ち込まない)。 */
    FSceneCommandQueue(FSceneCommandQueue&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FSceneCommandQueue& operator=(FSceneCommandQueue&&)      = delete;

    /**
     * 末尾に command を追加する。
     *
     * @details 同 label が既に居ても重ねて入れる。fn / label が nullptr なら警告ログを出して no-op。
     * @param label 同一性比較に使うラベル (文字列リテラル前提)。
     * @param fn 実行するコマンド本体。
     * @param user fn に渡すコンテキストポインタ。
     * @param priority 実行順 (昇順、既定 100)。
     * @param one_shot true なら Flush で 1 回だけ実行 (既定 true)。
     */
    void Enqueue(const char* label, SceneCommandFn fn, void* user,
                 u32 priority = 100u, bool one_shot = true) noexcept;

    /**
     * 同 label が既にキュー上に居なければ追加する (denounce)。
     *
     * @details 連打抑制 / Flush 待ちの重複要求を 1 つに畳む用途。既存があれば no-op で、
     * priority は新規追加時のみ反映する (one_shot は常に true 固定)。
     * @param label 同一性比較に使うラベル。
     * @param fn 実行するコマンド本体。
     * @param user fn に渡すコンテキストポインタ。
     * @param priority 新規追加時の実行順 (既定 100)。
     */
    void EnqueueIfAbsent(const char* label, SceneCommandFn fn, void* user,
                         u32 priority = 100u) noexcept;

    /**
     * label に一致する全 command を削除する (one_shot/repeating 両方)。
     *
     * @param label 削除対象のラベル (nullptr なら no-op)。
     */
    void Cancel(const char* label) noexcept;

    /**
     * priority 昇順で全 command を実行する。
     *
     * @details 同 priority 内は Enqueue 順 (安定)。実行後 one_shot は削除、repeating は残す。
     * Flush 中に Enqueue された command は今回実行せず次回 Flush に持ち越す (再エントランシ安全)。
     */
    void Flush() noexcept;

    /**
     * 現在キュー上にある command 数を返す。
     *
     * @return one_shot + repeating の合計数。
     */
    u32 PendingCount() const noexcept;

    /**
     * label に一致する command が 1 つ以上あるかを返す。
     *
     * @param label 検索するラベル (nullptr なら false)。
     * @return 一致する command があれば true。
     */
    bool Contains(const char* label) const noexcept;

    /**
     * 全 command を破棄する (callback は呼ばない)。
     *
     * @details Scene::OnExit 等で使う。
     */
    void ClearAll() noexcept;

private:
    /**
     * 全 command を priority 昇順に安定ソートする (insertion sort、N が小さい想定)。
     *
     * @details 同 priority は Enqueue 順 (元の index 順) を保存する。
     */
    void StableSortByPriority() noexcept;

    /** 保持中の command 列。 */
    TArray<CommandRecord> m_Records;
};

} // namespace acs::game
