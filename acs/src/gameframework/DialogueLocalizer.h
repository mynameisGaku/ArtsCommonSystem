// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Forward.h"

namespace acs::game {

// 前方宣言: ヘッダ依存を最小化 (LocalizationDirector.h を巻き込まない)
/**
 * 1 行ぶんのローカライズ可能ダイアログメタ。
 *
 * @details
 * 本文そのものは持たず、key 経由で CLocalizationDirector から取得する。
 * speaker_id も翻訳キー扱いで、const char* メンバは非所有 (literal / 長寿命
 * バッファ前提)。
 */
struct FLocalizedDialogueLine {
    /** 発話者名の翻訳 key (nullptr 可 = ナレーション)。 */
    const char* speaker_id    = nullptr;

    /** 本文の翻訳 key (nullptr 可 = 空行)。 */
    const char* line_key      = nullptr;

    /** タイプ速度 (FDialogueLine と同義、<=0 で瞬時)。 */
    f32         type_speed_cps = 30.0f;
};

/**
 * 1 つの選択肢メタ。
 *
 * @details
 * 表示テキストは翻訳 key 経由で取得する。next_line_index が範囲外 (>= LineCount)
 * なら「ダイアログ終了」を表現する (FDialogueChoice::next_line_index と同契約)。
 */
struct FLocalizedDialogueChoice {
    /** 選択肢表示の翻訳 key。 */
    const char* text_key        = nullptr;

    /** 選択時のジャンプ先 line index (範囲外で「終了」を表現)。 */
    u32         next_line_index = 0xFFFFFFFFu;
};

/**
 * Dialogue と Localization を疎結合で繋ぐ橋渡し helper。
 *
 * @details
 * 行 / 選択肢メタ (翻訳 key のみ) を蓄積し、StartFromLine で現 locale の翻訳
 * テキストを解決して callback に流す。本文は持たず、CLocalizationDirector を
 * 所有もしない (SetLocalizer で外部参照を差し込み、null で detach)。進行状態は
 * 持たず、非コピー・非ムーブ。
 */
class CDialogueLocalizer {
public:
    /**
     * 現 locale で解決した speaker / text を受け取る callback の型。
     *
     * @details
     * std::function は使わず関数ポインタ + user pointer 形式 (GameFramework 既存
     * callback 規約と整合)。speaker / text とも常に非 nullptr (未翻訳時は key 自身、
     * Localizer 未設定時は speaker_id / line_key そのもの or 空文字が渡る)。
     */
    using BindCallback = void(*)(void* user, const char* speaker, const char* text, f32 cps) noexcept;

    /** 空状態で構築する。 */
    CDialogueLocalizer()  noexcept = default;

    /** 破棄する (Localizer は非所有なので解放しない)。 */
    ~CDialogueLocalizer() noexcept = default;

    /** コピー禁止 (state holder の唯一性のため)。 */
    CDialogueLocalizer(const CDialogueLocalizer&)            = delete;

    /** コピー代入も禁止。 */
    CDialogueLocalizer& operator=(const CDialogueLocalizer&) = delete;

    /** ムーブ禁止 (state holder の唯一性のため)。 */
    CDialogueLocalizer(CDialogueLocalizer&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CDialogueLocalizer& operator=(CDialogueLocalizer&&)      = delete;

    /**
     * 行を末尾に追加する。
     *
     * @details 挿入順に index が振られる (0,1,2,...)。
     * @param line 追加するローカライズ可能ダイアログ行。
     */
    void RegisterLine(const FLocalizedDialogueLine& line) noexcept;

    /**
     * at_line_index 行に紐づく選択肢群を登録する。
     *
     * @details
     * 同じ line に複数回登録すると 2 回目以降は無視 (= 上書き禁止)。
     * count == 0 / choices == nullptr / at_line_index 範囲外 は no-op
     * (CDialogueSystem::AddChoices と同契約)。
     * @param at_line_index 選択肢を紐づける行の index。
     * @param choices 登録する選択肢配列。
     * @param count choices の要素数。
     */
    void RegisterChoice(u32 at_line_index,
                        const FLocalizedDialogueChoice* choices, u32 count) noexcept;

    /**
     * 翻訳ソースを差し込む。
     *
     * @details nullptr で detach (以降は key 自身が返る挙動)。所有しないので寿命は呼び出し側保証。
     * @param loc 翻訳辞書 (nullptr で detach)。
     */
    void SetLocalizer(CLocalizationDirector* loc) noexcept;

    /**
     * 指定 line index の speaker / 本文を現 locale で解決し cb を発火する。
     *
     * @details
     * line_index が範囲外 / cb == nullptr の場合は no-op。解決結果が空翻訳でも
     * 空文字 "" を渡し、callback 側で nullptr チェックを書かなくて済むようにする。
     * @param line_index 解決する行の index。
     * @param cb 解決結果を受け取る callback。
     * @param user cb に渡される文脈ポインタ。
     */
    void StartFromLine(u32 line_index, BindCallback cb, void* user) noexcept;

    /**
     * 登録済み行数を返す。
     *
     * @return 登録済み行数。
     */
    u32 LineCount() const noexcept;

    /**
     * 全行 / 全選択肢を破棄する。
     *
     * @details Localizer 参照は保持する (= 同 localizer で別シナリオを再構築する想定が大半)。
     */
    void Clear() noexcept;

private:
    /**
     * line_index 直後に提示する選択肢群の範囲記録 (CDialogueSystem::FChoicesAt と同じ形)。
     */
    struct FChoicesAt {
        /** 選択肢を紐づける行の index。 */
        u32 line_index   = 0;

        /** m_AllChoices 内の先頭 index。 */
        u32 choice_start = 0;

        /** この行に紐づく選択肢の本数。 */
        u32 choice_count = 0;
    };

    /** 登録済みダイアログ行 (挿入順)。 */
    TArray<FLocalizedDialogueLine>   m_Lines;

    /** 行ごとの選択肢範囲記録 (line_index 昇順想定、線形検索)。 */
    TArray<FChoicesAt>               m_ChoicesAt;

    /** 全選択肢をフラットに保持する配列。 */
    TArray<FLocalizedDialogueChoice> m_AllChoices;

    /** 翻訳辞書 (非所有、null で detach)。 */
    CLocalizationDirector* m_Localizer = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FDialogueLocalizer = CDialogueLocalizer;

} // namespace acs::game
