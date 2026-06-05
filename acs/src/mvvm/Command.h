// SPDX-License-Identifier: Apache-2.0
// Command — VM のアクション (ボタン等) を FViewModel 側に置くためのヘルパ
//
// 使い方:
//   class PlayerVM : public FViewModel {
//   public:
//       Observable<f32> hp { 100.0f };
//
//       Command attack {
//           [](void* user){ static_cast<PlayerVM*>(user)->hp.Set(0); },
//           this
//       };
//
//       // can_execute を Observable<bool> で外から差し替えるなら 3 引数版:
//       Command revive {
//           [](void* u){ static_cast<PlayerVM*>(u)->hp.Set(100); },
//           this,
//           &is_dead   // この Observable<bool> が true のときだけ実行可能
//       };
//
//       Observable<bool> is_dead { false };
//   };
//
// 設計:
//   ・can_execute はオプション (常時実行可能がデフォルト)
//   ・can_execute は Observable<bool>* を取る (生存期間は呼び出し元責任)
//   ・Execute は can_execute=false なら no-op
//   ・ImGui アダプタの BindCommand(label, cmd) でボタン化 + 自動 grayout
#pragma once

#include "foundation/Types.h"
#include "mvvm/Observable.h"

namespace acs {

/**
 * ボタン等の VM アクションを表すコマンド。
 *
 * @details
 * 実行関数 (関数ポインタ + void* user) を保持し、Execute で起動する。任意で
 * Observable<bool>* を can_execute 条件として持ち、false の間は Execute が no-op
 * になり UI 側は grayout できる。can_execute Observable の生存期間は呼び出し元責任。
 */
class Command {
public:
    /** 実行関数型 (Command 構築時の user を受け取る)。 */
    using ExecFn = void (*)(void* user);

    /**
     * 常時実行可能なコマンドを構築する。
     *
     * @param fn 実行する関数。
     * @param user 実行関数へ渡す任意ポインタ。
     */
    Command(ExecFn fn, void* user) noexcept
        : m_Fn(fn), m_User(user), m_CanExecute(nullptr) {}

    /**
     * 条件付きで実行可能なコマンドを構築する。
     *
     * @details can_execute が false の間は Execute() が no-op になる。
     * @param fn 実行する関数。
     * @param user 実行関数へ渡す任意ポインタ。
     * @param can_execute 実行可否を持つ Observable<bool> (生存期間は呼び出し元責任)。
     */
    Command(ExecFn fn, void* user, Observable<bool>* can_execute) noexcept
        : m_Fn(fn), m_User(user), m_CanExecute(can_execute) {}

    /**
     * コマンドを実行する (実行不可・無効化済みなら何もしない)。
     */
    void Execute() noexcept {
        if (!m_Fn) return;
        if (m_CanExecute && !m_CanExecute->Get()) return;
        m_Fn(m_User);
    }

    /**
     * 現在実行可能かを返す (UI の grayout 判定に使う)。
     *
     * @return 実行関数があり can_execute が無いか true なら true。
     */
    bool CanExecute() const noexcept {
        return m_Fn && (!m_CanExecute || m_CanExecute->Get());
    }

    /**
     * コマンドを無効化する (実行関数と user をクリアする)。
     *
     * @details VM の dtor で呼んでダングリング呼び出しを防ぐ用途。
     */
    void Invalidate() noexcept { m_Fn = nullptr; m_User = nullptr; }

    /**
     * can_execute 条件の Observable を返す (BindCommand が監視に使う)。
     *
     * @return can_execute の Observable<bool> ポインタ (無ければ nullptr)。
     */
    Observable<bool>* CanExecuteSource() const noexcept { return m_CanExecute; }

private:
    /** 実行する関数 (Invalidate で nullptr になる)。 */
    ExecFn            m_Fn          = nullptr;

    /** 実行関数へ渡す user ポインタ。 */
    void*             m_User        = nullptr;

    /** 実行可否を持つ Observable<bool> (無ければ常時実行可能)。 */
    Observable<bool>* m_CanExecute = nullptr;
};

} // namespace acs
