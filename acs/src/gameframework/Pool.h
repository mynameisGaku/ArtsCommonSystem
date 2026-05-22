// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — Pool<T> (再利用プール)
//
// 弾 / パーティクル / 一時敵など、「短命オブジェクトを毎フレーム大量に生成・破棄
// する」用途のためのオブジェクトプール。new/delete を避けて GC ヒッチ・アロケータ
// 断片化・キャッシュミスを抑える。
//
// 設計方針:
//   ・固定容量。Init(capacity) 後はリサイズしない (確定的なメモリ使用量)。
//     → リアルタイムゲームは「最悪ケース」を見積もり、それを上限として動くのが
//        安全。実行時に勝手に伸びるとフレーム落ちの原因になる。
//   ・要素 T は default 構築で「全分」事前に作る。実体は常に存在し、
//     _active[i] = 0/1 で「使用中か否か」を表すだけ。
//     → Acquire / Release で T のコンストラクタ / デストラクタは走らない。
//        Reset 等の「再初期化」はユーザー側責任 (典型的には Spawn 関数を別に持つ)。
//   ・Acquire は線形探索 (O(N))。理由:
//      1) 大半のゲーム用途では capacity ≤ 数千、毎フレーム数十回の Acquire 程度
//         なので、N×回数 が L1 で完結する。
//      2) フラグ配列が u8 の連続メモリなので、SIMD/プリフェッチが最大限効く。
//      3) free-list 化は次穴を O(1) で取れるが、ポインタの追跡でキャッシュが汚れる。
//         Phase 2 でプロファイル次第で free-list 化を検討 (TODO)。
//   ・ForEachActive は内部ループで _active==1 のものだけに fn を呼ぶ。
//     update / draw を呼ぶ典型用途を想定。
//   ・非コピー / 非ムーブ (所有権の事故防止 + ポインタを Acquire で配るため
//     ムーブで _data アドレスが変わると外部の T* が無効化される)。
//
// 使い方:
//   struct Bullet { f32 x, y, vx, vy; bool alive; };
//   acs::game::Pool<Bullet> bullets;
//   bullets.Init(1024);
//
//   if (Bullet* b = bullets.Acquire()) {
//       b->x = px; b->y = py; b->vx = vx; b->vy = vy; b->alive = true;
//   }
//
//   bullets.ForEachActive([dt](Bullet& b) {
//       b.x += b.vx * dt;
//       b.y += b.vy * dt;
//   });
//
//   // 帰還条件を満たした弾を返す:
//   //   bullets.Release(&b);  // ForEachActive 内では index が必要、別 API 予定
//
// 注意:
//   ・Release(nullptr) や、このプールが管理していないポインタを渡すと no-op
//     (= ポインタ範囲チェックで弾く)。
//   ・Acquire が返した T* は次の ResetAll() / Pool 破棄まで安定 (固定容量なので
//     再配置されない)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

template<typename T>
class Pool {
public:
    Pool() noexcept = default;

    // 非コピー / 非ムーブ: Acquire で配った T* がムーブで無効化される事故を防ぐ。
    Pool(const Pool&)            = delete;
    Pool& operator=(const Pool&) = delete;
    Pool(Pool&&)                 = delete;
    Pool& operator=(Pool&&)      = delete;

    // 固定容量を予約。要素 T は default 構築される。
    // 二度目以降の呼び出しは no-op (固定容量ポリシー)。
    void Init(u32 capacity) noexcept {
        if (_capacity != 0) return;     // 既に初期化済み
        if (capacity == 0)  return;
        _items.Resize(static_cast<usize>(capacity));
        _active.Resize(static_cast<usize>(capacity));
        for (usize i = 0; i < _active.Size(); ++i) _active[i] = 0;
        _capacity     = capacity;
        _active_count = 0;
    }

    // 空きスロットを 1 個確保して返す。空きが無ければ nullptr。
    // 返された T* は次の ResetAll() / Pool 破棄まで安定。
    // 中身は前回使用時の値が残るので、ユーザー側で再初期化すること。
    T* Acquire() noexcept {
        if (_active_count >= _capacity) return nullptr;
        const usize n = _active.Size();
        for (usize i = 0; i < n; ++i) {
            if (_active[i] == 0) {
                _active[i] = 1;
                ++_active_count;
                return &_items[i];
            }
        }
        return nullptr;     // 理論上ここには来ない (_active_count < _capacity なので)
    }

    // p をプールに返す。nullptr / 範囲外ポインタ / 既に解放済みは no-op。
    void Release(T* p) noexcept {
        if (p == nullptr) return;
        if (_items.Size() == 0) return;
        T* base = _items.Data();
        if (p < base) return;
        const usize idx = static_cast<usize>(p - base);
        if (idx >= _items.Size()) return;
        if (_active[idx] == 0) return;  // 二重 Release 防止
        _active[idx] = 0;
        --_active_count;
    }

    // 全 active を一括解放。中身の T は触らない。
    void ResetAll() noexcept {
        for (usize i = 0; i < _active.Size(); ++i) _active[i] = 0;
        _active_count = 0;
    }

    u32 ActiveCount() const noexcept { return _active_count; }
    u32 Capacity()    const noexcept { return _capacity; }

    // active な要素を fn(T&) で巡回。
    // fn 内で Acquire/Release を呼ぶのは未定義動作 (反復中に _active を書き換える)。
    template<typename F>
    void ForEachActive(F&& fn) noexcept {
        const usize n = _active.Size();
        for (usize i = 0; i < n; ++i) {
            if (_active[i] != 0) fn(_items[i]);
        }
    }

private:
    Array<T>  _items{};         // 容量分の T 実体 (default 構築済み)
    Array<u8> _active{};        // _active[i] == 1 なら使用中
    u32       _capacity     = 0;
    u32       _active_count = 0;
};

} // namespace acs::game
