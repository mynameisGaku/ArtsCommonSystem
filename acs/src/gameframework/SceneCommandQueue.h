// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — SceneCommandQueue (deferred command queue / editor 連携)
//
// 走査中 (OnUpdate / OnDraw) に発火された「シーン構造変更」等の要求をフレーム末
// で順次実行するための遅延実行キュー。`SceneManager` の `_ApplyPending` と同じ
// 哲学 (= 走査中の構造変更を避け、安全なフレーム境界で適用する) を、より粒度の
// 細かいコマンド単位に拡張したもの。editor から「ノード追加」「コンポーネント
// 差し替え」等を OnUpdate のループ走査中に呼んでも、Flush 時に integrity を
// 保ったまま適用される。
//
// 使い方:
//   class GameplayScene : public Scene {
//       SceneCommandQueue _cmds;
//
//       void OnUpdate(f32 dt) noexcept override {
//           // 走査中に node 削除を要求しても安全 (Flush でまとめて実行)
//           if (Input::IsKeyPressed(Key::Delete)) {
//               _cmds.Enqueue("DeleteSelected", &GameplayScene::DeleteSelected, this);
//           }
//           // 同 label が既にキュー上に居れば denounce (連打抑制)
//           if (held) {
//               _cmds.EnqueueIfAbsent("Refresh", &GameplayScene::RefreshUi, this, /*priority=*/50);
//           }
//       }
//       void OnDraw() noexcept override {
//           // ImGui ボタン処理中などからも安全に enqueue できる
//       }
//
//       // フレーム末に Game / Scene 側で 1 回 Flush する。
//       static void DeleteSelected(void* self) noexcept { /* ... */ }
//       static void RefreshUi    (void* self) noexcept { /* ... */ }
//   };
//
// 設計選択 (Pillar A polish):
//   ・**deferred 実行**: 走査中の構造変更を避けるため、Flush までは実行しない。
//     `SceneManager` の pending op (1 個) と違い、複数 command を保持・優先度
//     付きで順序付け実行する。
//   ・**function pointer + void* user**: ACS 規約 (std::function 不使用、heap
//     allocation / RTTI / 例外を持ち込まない)。
//   ・**const char* label**: 文字列リテラル前提、本クラスは複製しない (DebugOverlay
//     の watch 列と同じ方針)。同一性比較は pointer 一致 → fallback で strcmp。
//     `<string>` 禁止 (STL 不使用)。
//   ・**priority 昇順実行**: 同 priority 内では Enqueue 順 (= 安定ソート)。
//     editor からの "削除" → "再構築" のような順序依存を表現可能。
//   ・**one_shot vs repeating**: Flush 後に one_shot は削除、repeating は残す。
//     debug overlay の定期チェックや editor の continuous validation で使用。
//   ・**EnqueueIfAbsent (denounce)**: 同 label の既存 command があれば no-op。
//     入力連打 / リサイズイベント連発で同じ作業が積み上がるのを防ぐ。
//   ・**Cancel**: label 一致の全 command を削除 (one_shot/repeating 両方)。
//   ・**Flush 中の Enqueue 安全性**: SceneEventBus と同じく、走査 size を最初に
//     スナップショットして固定範囲のみ実行する。Flush 中に追加された command は
//     次回 Flush で初めて実行される。Flush 中に同 slot が PushBack で再 alloc を
//     起こしても、fn / user / one_shot を local にコピーしてから呼ぶことで安全。
//   ・**非コピー・非ムーブ**: Scene にメンバとして埋め込む前提、所有権の
//     ambiguity を持ち込まない。
//   ・**STL 不使用 / 全 noexcept**: ACS 規約。`acs::Array<CommandRecord>` で持つ。
//
// 範囲外 (将来フェーズで):
//   ・スレッドセーフ (現状は同一スレッド前提、editor が別スレッドから enqueue する
//     なら mutex を内蔵するか SPSC ring に置き換える必要あり)
//   ・command 履歴 / undo (editor の undo stack は別レイヤで持つべき)
//   ・cross-scene broadcast (SceneEventBus と同じく Scene を越えるのは将来課題)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// コマンド本体。user は Enqueue 時に渡したコンテキストポインタ (this 想定)。
using SceneCommandFn = void(*)(void* user) noexcept;

// キュー内の 1 エントリ。label / fn / user は呼出し側所有 (本クラスは複製しない)。
struct CommandRecord {
    const char* label    = nullptr;
    SceneCommandFn   fn       = nullptr;
    void*       user     = nullptr;
    bool        one_shot = true;
    u32         priority = 100u;
};

class SceneCommandQueue {
public:
    SceneCommandQueue() noexcept = default;
    ~SceneCommandQueue() noexcept = default;

    // 非コピー・非ムーブ (発火中の参照との競合を防ぐ)
    SceneCommandQueue(const SceneCommandQueue&)            = delete;
    SceneCommandQueue& operator=(const SceneCommandQueue&) = delete;
    SceneCommandQueue(SceneCommandQueue&&)                 = delete;
    SceneCommandQueue& operator=(SceneCommandQueue&&)      = delete;

    // ----- enqueue -----

    // 末尾に command を追加。同 label が既に居ても重ねて入れる (Enqueue 何度でも)。
    // fn == nullptr / label == nullptr は警告ログを出して no-op。
    void Enqueue(const char* label, SceneCommandFn fn, void* user,
                 u32 priority = 100u, bool one_shot = true) noexcept;

    // 同 label が既にキュー上に居れば no-op (denounce)。連打抑制 / Flush 待ちの
    // 重複要求を 1 つに畳む用途。priority/one_shot は新規追加時のみ反映 (既存は変更しない)。
    void EnqueueIfAbsent(const char* label, SceneCommandFn fn, void* user,
                         u32 priority = 100u) noexcept;

    // ----- cancel / inspect -----

    // label に一致する全 command を削除 (one_shot/repeating 両方)。label == nullptr は no-op。
    void Cancel(const char* label) noexcept;

    // priority 昇順で全 command を実行 (同 priority 内は Enqueue 順 = 安定)。
    // 実行後 one_shot は削除、repeating は残す。Flush 中の Enqueue は次回反映。
    void Flush() noexcept;

    // 現在キュー上の command 数 (one_shot + repeating の合計)。
    u32 PendingCount() const noexcept;

    // label に一致する command が 1 つ以上居るかどうか。label == nullptr は false。
    bool Contains(const char* label) const noexcept;

    // 全 command を破棄 (callback は呼ばない)。Scene::OnExit 等で使う。
    void ClearAll() noexcept;

private:
    // Flush の安定 priority 昇順ソート (insertion sort、N が小さい想定)。
    void StableSortByPriority() noexcept;

    Array<CommandRecord> _records;
};

} // namespace acs::game
