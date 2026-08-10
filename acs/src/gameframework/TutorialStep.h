// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace acs::game {

/**
 * チュートリアル表示と進行条件を表す 1 ステップ分の値。
 *
 * 文字列は所有せず、呼び出し側がフロー利用中の寿命を保証する。
 */
struct FTutorialStep {
    /** NotifyAction が照合する識別子。nullptr の場合は操作通知で完了しない。 */
    const char* id                  = nullptr;

    /** 呼び出し側の UI が表示する文字列。nullptr を許容する。 */
    const char* message             = nullptr;

    /** 呼び出し側が解釈する強調対象の識別子。nullptr を許容する。 */
    const char* highlight_target    = nullptr;

    /** true の場合は一致する操作通知または明示前進を待つ。 */
    bool        require_user_action = false;
};

} // namespace acs::game
