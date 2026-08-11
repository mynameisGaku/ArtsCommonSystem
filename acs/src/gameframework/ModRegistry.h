// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "container/Array.h"

namespace acs::game {

/**
 * 1 Mod 分のメタデータ。
 *
 * @details
 * 文字列フィールドは「呼び出し側が所有する」前提で nullptr 許容にしている
 * (id は Mod 一意キーで空文字や nullptr は Register 時にスキップ、name は表示用、
 * pack_path は .acpak のパスで nullptr なら path 未指定)。version は
 * (major << 24) | (minor << 16) | patch エンコーディングを想定するが、Registry
 * 側は不透明に扱う (比較のみ)。
 */
struct FModInfo {
    /** Mod 一意キー (外部所有。空文字や nullptr は Register 時にスキップ)。 */
    const char* id         = nullptr;

    /** 表示用の名前 (外部所有。UI が pull する。nullptr 可)。 */
    const char* name       = nullptr;

    /** バージョン ((major << 24) | (minor << 16) | patch、Registry は不透明扱い)。 */
    u32         version    = 0;

    /** load 順 (昇順。小さい方を先に load、大きい方が後で上書き)。 */
    i32         load_order = 0;

    /** 有効フラグ (true で mount 対象)。 */
    bool        enabled    = false;

    /** .acpak のファイルパス (外部所有。nullptr なら path 未指定)。 */
    const char* pack_path  = nullptr;
};

/**
 * ユーザー Mod の登録・有効化・並び順を管理する薄いレジストリ。
 *
 * @details
 * 各 Mod を FModInfo の内部 TArray に保持し、load_order 昇順に並べる。文字列は
 * 所有せず呼び出し側の寿命に依存する。1 ゲーム寿命に 1 インスタンスのみを想定し
 * non-copy / non-move とする。
 */
class CModRegistry {
public:
    /** 空のレジストリを構築する。 */
    CModRegistry() noexcept = default;

    /** 破棄する (内部 TArray が解放される)。 */
    ~CModRegistry() noexcept = default;

    /** コピー禁止 (active な Registry を一意にするため)。 */
    CModRegistry(const CModRegistry&)            = delete;

    /** コピー代入も禁止。 */
    CModRegistry& operator=(const CModRegistry&) = delete;

    /** ムーブ禁止。 */
    CModRegistry(CModRegistry&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CModRegistry& operator=(CModRegistry&&)      = delete;

    /**
     * Mod を内部リストの末尾に登録する。
     *
     * @details
     * info を内部 TArray に浅くコピーして追加する (指す文字列バッファの寿命は
     * 呼び出し側が保証する)。id == nullptr のエントリは無視する (警告ログのみ)。
     * @param info 登録する Mod のメタデータ。
     */
    void Register(const FModInfo& info) noexcept;

    /**
     * id に一致する Mod を有効化する。
     *
     * @param mod_id 有効化する Mod の一意キー。
     * @return 見つかって enabled を立てたら true。
     */
    bool Enable (const char* mod_id) noexcept;

    /**
     * id に一致する Mod を無効化する。
     *
     * @param mod_id 無効化する Mod の一意キー。
     * @return 見つかって enabled を落としたら true。
     */
    bool Disable(const char* mod_id) noexcept;

    /**
     * 指定 Mod の load_order を変更する (sort は別途呼び出し側で実行)。
     *
     * @details 見つからなくても黙って no-op (UI 側で都度同期する想定)。
     * @param mod_id 対象 Mod の一意キー。
     * @param order 新しい load 順。
     */
    void SetLoadOrder(const char* mod_id, i32 order) noexcept;

    /**
     * 登録済み Mod の件数を返す。
     *
     * @return Mod の件数。
     */
    u32                Count() const noexcept;

    /**
     * id に一致する Mod を検索する。
     *
     * @param mod_id 探す Mod の一意キー。
     * @return 見つかった FModInfo へのポインタ (無ければ nullptr)。
     */
    const FModInfo*     Find (const char* mod_id) const noexcept;

    /**
     * 登録済み Mod の生バッファ先頭を返す。
     *
     * @return FModInfo 配列の先頭 (長さは Count() で確定)。
     */
    const FModInfo*     All  () const noexcept;

    /**
     * load_order 昇順に並べ替える (同値は登録順を保つ安定 sort)。
     */
    void SortByLoadOrder() noexcept;

    /**
     * 登録済み Mod を全て削除する。
     */
    void Clear() noexcept;

private:
    /**
     * 2 つの id 文字列が等しいかを返す (両者 nullptr 安全)。
     *
     * @param a 比較する id (nullptr 可)。
     * @param b 比較する id (nullptr 可)。
     * @return 文字列として一致すれば true。
     */
    static bool IdEquals(const char* a, const char* b) noexcept;

    /** 登録済み Mod の配列 (登録順、SortByLoadOrder で並べ替え)。 */
    TArray<FModInfo> m_Mods;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FModRegistry = CModRegistry;

} // namespace acs::game
