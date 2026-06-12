// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — エンジン同梱型のリフレクションカタログ (エントリ)
// -----------------------------------------------------------------------------
// ReflectCatalog.cpp が、エンジンが標準で持つ型 (Component / System / Scene /
// Asset / 主要 enum) を「型ヘッダを書き換えずに」FTypeRegistry へ一括登録する。
//
// 静的初期化の確実化:
//   登録は ACS_REGISTER* マクロの静的初期化子で行うが、静的ライブラリ
//   (acs_gameframework) に置いた初期化子はリンカに落とされ得る。そこで利用側
//   (テスト / エンジン起動 / editor ABI) は起動時に 1 度 AcsRegisterEngineTypes()
//   を呼び、カタログ TU を確実にリンクへ引き込むこと。冪等。
// =============================================================================
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/**
 * エンジン同梱型のリフレクション登録を確定する (冪等)。
 *
 * @details
 * この関数を ODR-use することでカタログ TU がリンクへ引き込まれ、同 TU 内の
 * ACS_REGISTER* 自動登録子が静的初期化時に走る。複数回呼んでも二重登録は起きない
 * (FTypeRegistry::Register が ID で重複排除する)。
 * @return 呼び出し後の FTypeRegistry の総登録型数。
 */
u32 AcsRegisterEngineTypes() noexcept;

} // namespace acs::game
