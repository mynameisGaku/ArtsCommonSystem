// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — SceneCommandQueue 実装 (deferred command queue)
//
// 設計メモ:
//  ・label 比較は pointer 一致 → 不一致なら strcmp で fallback。文字列リテラル前提
//    だが、複数 TU で同一リテラルが別アドレスに置かれるケース (= /GF 未指定など) を
//    救済する。DebugOverlay の watch 列と同じ方針。
//  ・Flush 中のコールバックが新規 Enqueue を呼んだ場合、_records の PushBack で
//    reference invalidation が起きる可能性がある。ループは index で回し、走査 size
//    は最初にスナップショット、各反復で fn / user / one_shot / label を local にコピー
//    してから呼ぶ。新規追加された command は今回の Flush では実行しない。
//  ・priority 昇順ソートは insertion sort で安定 (同 priority は Enqueue 順を保存)。
//    SceneCommandQueue は editor / 連打抑制が想定 N、現実的に数〜数十の想定なので
//    O(N^2) で十分。N が大きくなったら merge sort に差し替え可能。
//  ・Flush 後の物理削除は「実行済 one_shot を後ろから順に PopBack または RemoveAtSwap」
//    で行うと安定順序が崩れるため、新しい Array に「残す要素」だけを書き戻す方式を
//    採用する (空間効率より単純さ優先、N が小さいので問題なし)。
//  ・全 noexcept、STL 不使用、`<string>` 禁止。`<cstring>` (strcmp) のみ許可。
#include "gameframework/SceneCommandQueue.h"

#include "foundation/Log.h"
#include "foundation/Move.h"

#include <cstring>   // strcmp

namespace acs::game {

namespace {

// label の同一性比較。pointer 一致を最初に試して strcmp に fallback。
// 両方 nullptr / 片方 nullptr の扱いは「片方 nullptr なら不一致、両 nullptr なら不一致」
// (= label == nullptr を有効 key と見なさない設計、API 側で nullptr ガード済み)。
bool LabelEquals(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    if (a == b) return true;
    return ::strcmp(a, b) == 0;
}

} // namespace

// ============================================================================
// enqueue
// ============================================================================

void SceneCommandQueue::Enqueue(const char* label, SceneCommandFn fn, void* user,
                                 u32 priority, bool one_shot) noexcept {
    if (label == nullptr) {
        ACS_LOG_WARN("SceneCommandQueue::Enqueue: null label ignored");
        return;
    }
    if (fn == nullptr) {
        ACS_LOG_WARN("SceneCommandQueue::Enqueue: null fn for label '%s' ignored", label);
        return;
    }

    CommandRecord r;
    r.label    = label;
    r.fn       = fn;
    r.user     = user;
    r.priority = priority;
    r.one_shot = one_shot;
    _records.PushBack(r);
}

void SceneCommandQueue::EnqueueIfAbsent(const char* label, SceneCommandFn fn, void* user,
                                         u32 priority) noexcept {
    if (label == nullptr) {
        ACS_LOG_WARN("SceneCommandQueue::EnqueueIfAbsent: null label ignored");
        return;
    }
    if (fn == nullptr) {
        ACS_LOG_WARN("SceneCommandQueue::EnqueueIfAbsent: null fn for label '%s' ignored", label);
        return;
    }

    // 同 label が既存なら no-op (denounce)
    const usize n = _records.Size();
    for (usize i = 0; i < n; ++i) {
        if (LabelEquals(_records[i].label, label)) return;
    }

    // 新規追加 (priority / one_shot は EnqueueIfAbsent では one_shot=true 固定 — 仕様)
    CommandRecord r;
    r.label    = label;
    r.fn       = fn;
    r.user     = user;
    r.priority = priority;
    r.one_shot = true;
    _records.PushBack(r);
}

// ============================================================================
// cancel / inspect
// ============================================================================

void SceneCommandQueue::Cancel(const char* label) noexcept {
    if (label == nullptr) return;
    // 安定削除: write index で前から詰める (順序保存)。
    // Flush 中に呼ばれても安全 (size 0 への縮小は走査側でガード済み)。
    usize w = 0;
    const usize n = _records.Size();
    for (usize r = 0; r < n; ++r) {
        if (!LabelEquals(_records[r].label, label)) {
            if (w != r) _records[w] = _records[r];
            ++w;
        }
    }
    while (_records.Size() > w) _records.PopBack();
}

u32 SceneCommandQueue::PendingCount() const noexcept {
    return static_cast<u32>(_records.Size());
}

bool SceneCommandQueue::Contains(const char* label) const noexcept {
    if (label == nullptr) return false;
    const usize n = _records.Size();
    for (usize i = 0; i < n; ++i) {
        if (LabelEquals(_records[i].label, label)) return true;
    }
    return false;
}

void SceneCommandQueue::ClearAll() noexcept {
    _records.Clear();
}

// ============================================================================
// Flush
// ============================================================================

void SceneCommandQueue::StableSortByPriority() noexcept {
    // insertion sort (stable)。N は小規模想定 (editor の保留要求や連打抑制で
    // 通常 < 数十)。同 priority は Enqueue 順 (= 元の index 順) を保存する。
    const usize n = _records.Size();
    for (usize i = 1; i < n; ++i) {
        CommandRecord key = _records[i];
        usize j = i;
        // j-1 の priority が key より厳密に大きい間だけ右へずらす (= 安定)。
        while (j > 0 && _records[j - 1].priority > key.priority) {
            _records[j] = _records[j - 1];
            --j;
        }
        _records[j] = key;
    }
}

void SceneCommandQueue::Flush() noexcept {
    if (_records.IsEmpty()) return;

    // 1. priority 昇順に並べる (同 priority は安定 = Enqueue 順)
    StableSortByPriority();

    // 2. 走査範囲を Flush 開始時点の size で固定。Flush 中に Enqueue された
    //    command は今回実行しない (次回 Flush で実行)。SceneEventBus と同じ
    //    再エントランシ安全性パターン。
    const usize snapshot_size = _records.Size();

    // 3. 「保持する command」 (= 再エントラント追加 + 実行後 repeating) を新 Array に
    //    積みなおす。snapshot 範囲の repeating は実行後に残す、one_shot は破棄、
    //    snapshot 範囲外 (Flush 中に追加) は実行せず全て残す。
    //    安定順序を維持するため、新 Array で書き戻す方式を採用 (PopBack/Swap は
    //    順序破壊が起き得るため避ける)。
    Array<CommandRecord> retained;

    for (usize i = 0; i < snapshot_size; ++i) {
        // ループ内で _records が再 alloc される可能性に備え、各反復で値をコピー。
        const CommandRecord rec = _records[i];

        // 実行はコピーしたフィールドで行う (_records 側が動いても影響を受けない)。
        if (rec.fn != nullptr) {
            rec.fn(rec.user);
        }

        // repeating は次回 Flush に持ち越し
        if (!rec.one_shot) {
            retained.PushBack(rec);
        }
    }

    // 4. Flush 中に追加された command (snapshot_size 以降) を retained に連結。
    //    順序は Enqueue 順を保つ (priority ソートは次回 Flush で行う)。
    const usize now_size = _records.Size();
    for (usize i = snapshot_size; i < now_size; ++i) {
        retained.PushBack(_records[i]);
    }

    // 5. _records を retained で置換 (ムーブ)。
    _records = Move(retained);
}

} // namespace acs::game
