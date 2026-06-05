// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — TPool<T> (再利用プール)
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
//     m_Active[i] = 0/1 で「使用中か否か」を表すだけ。
//     → Acquire / Release で T のコンストラクタ / デストラクタは走らない。
//        Reset 等の「再初期化」はユーザー側責任 (典型的には Spawn 関数を別に持つ)。
//   ・Acquire は線形探索 (O(N))。理由:
//      1) 大半のゲーム用途では capacity ≤ 数千、毎フレーム数十回の Acquire 程度
//         なので、N×回数 が L1 で完結する。
//      2) フラグ配列が u8 の連続メモリなので、SIMD/プリフェッチが最大限効く。
//      3) free-list 化は次穴を O(1) で取れるが、ポインタの追跡でキャッシュが汚れる。
//         プロファイル次第で free-list 化を検討 (TODO)。
//   ・ForEachActive は内部ループで m_Active==1 のものだけに fn を呼ぶ。
//     update / draw を呼ぶ典型用途を想定。
//   ・非コピー / 非ムーブ (所有権の事故防止 + ポインタを Acquire で配るため
//     ムーブで m_Data アドレスが変わると外部の T* が無効化される)。
//
// 使い方:
//   struct Bullet { f32 x, y, vx, vy; bool alive; };
//   acs::game::TPool<Bullet> bullets;
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
//   ・Acquire が返した T* は次の ResetAll() / TPool 破棄まで安定 (固定容量なので
//     再配置されない)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * 短命オブジェクトを再利用する固定容量オブジェクトプール。
 *
 * @details
 * 弾・パーティクル・一時敵など毎フレーム大量に生成・破棄する用途で new/delete を
 * 避け、GC ヒッチ・アロケータ断片化・キャッシュミスを抑える。Init(capacity) で
 * 全要素を default 構築して以後リサイズせず、m_Active[i]=0/1 で使用中か否かだけを
 * 切り替える (Acquire/Release で T のコンストラクタ・デストラクタは走らない)。
 * Acquire は空きスロットを線形探索する。Acquire で配った T* がムーブで無効化される
 * のを防ぐため non-copy / non-move。
 *
 * @tparam T プールが保持する要素型 (default 構築可能であること)。
 */
template<typename T>
class TPool {
public:
    /** 空状態で構築する (実体は Init で確保)。 */
    TPool() noexcept = default;

    /** コピー禁止 (要素を単独所有し、配った T* の安定性を保つため)。 */
    TPool(const TPool&)            = delete;

    /** コピー代入も禁止。 */
    TPool& operator=(const TPool&) = delete;

    /** ムーブ禁止 (Acquire で配った T* がムーブで無効化されるのを防ぐため)。 */
    TPool(TPool&&)                 = delete;

    /** ムーブ代入も禁止。 */
    TPool& operator=(TPool&&)      = delete;

    /**
     * 固定容量を予約し、要素 T を全分 default 構築する。
     *
     * @details 既に初期化済み、または capacity == 0 の呼び出しは no-op (固定容量ポリシー)。
     * @param capacity 予約するスロット数。
     */
    void Init(u32 capacity) noexcept {
        if (m_Capacity != 0) return;     // 既に初期化済み
        if (capacity == 0)  return;
        m_Items.Resize(static_cast<usize>(capacity));
        m_Active.Resize(static_cast<usize>(capacity));
        for (usize i = 0; i < m_Active.Size(); ++i) m_Active[i] = 0;
        m_Capacity     = capacity;
        m_ActiveCount = 0;
    }

    /**
     * 空きスロットを 1 個確保して返す。
     *
     * @details
     * 返した T* は次の ResetAll() / TPool 破棄まで安定 (固定容量なので再配置されない)。
     * 中身は前回使用時の値が残るので、ユーザー側で再初期化すること。
     * @return 確保したスロットへのポインタ (空きが無ければ nullptr)。
     */
    T* Acquire() noexcept {
        if (m_ActiveCount >= m_Capacity) return nullptr;
        const usize n = m_Active.Size();
        for (usize i = 0; i < n; ++i) {
            if (m_Active[i] == 0) {
                m_Active[i] = 1;
                ++m_ActiveCount;
                return &m_Items[i];
            }
        }
        return nullptr;     // 理論上ここには来ない (m_ActiveCount < m_Capacity なので)
    }

    /**
     * p をプールに返して使用中フラグを下ろす。
     *
     * @details nullptr / プール範囲外のポインタ / 既に解放済みは no-op (ポインタ範囲で弾く)。
     * @param p 返却するスロットへのポインタ。
     */
    void Release(T* p) noexcept {
        if (p == nullptr) return;
        if (m_Items.Size() == 0) return;
        T* base = m_Items.Data();
        if (p < base) return;
        const usize idx = static_cast<usize>(p - base);
        if (idx >= m_Items.Size()) return;
        if (m_Active[idx] == 0) return;  // 二重 Release 防止
        m_Active[idx] = 0;
        --m_ActiveCount;
    }

    /** 全 active スロットを一括解放する (中身の T は触らない)。 */
    void ResetAll() noexcept {
        for (usize i = 0; i < m_Active.Size(); ++i) m_Active[i] = 0;
        m_ActiveCount = 0;
    }

    /**
     * 現在使用中のスロット数を返す。
     *
     * @return active なスロット数。
     */
    u32 ActiveCount() const noexcept { return m_ActiveCount; }

    /**
     * プールの固定容量を返す。
     *
     * @return Init で予約したスロット総数。
     */
    u32 Capacity()    const noexcept { return m_Capacity; }

    /**
     * active な要素を fn(T&) で巡回する。
     *
     * @details fn 内で Acquire/Release を呼ぶのは未定義動作 (反復中に m_Active を書き換えるため)。
     * @tparam F T& を受ける呼び出し可能型。
     * @param fn 各 active 要素に適用する関数。
     */
    template<typename F>
    void ForEachActive(F&& fn) noexcept {
        const usize n = m_Active.Size();
        for (usize i = 0; i < n; ++i) {
            if (m_Active[i] != 0) fn(m_Items[i]);
        }
    }

private:
    /** 容量分の T 実体 (default 構築済み、常に存在する)。 */
    TArray<T>  m_Items{};

    /** 各スロットの使用中フラグ (m_Active[i] == 1 なら使用中)。 */
    TArray<u8> m_Active{};

    /** 固定容量 (Init で確定、以後不変)。 */
    u32       m_Capacity     = 0;

    /** 現在 active なスロット数。 */
    u32       m_ActiveCount = 0;
};

} // namespace acs::game
