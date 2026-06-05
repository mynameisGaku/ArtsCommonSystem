// SPDX-License-Identifier: Apache-2.0
// Deterministic local matchmaker backend for acs::game::IMatchmaker.
#pragma once

#include "gameframework/BackendClient.h"

namespace acs::localmatch {

/**
 * ローカル matchmaker の挙動を調整する設定値。
 */
struct FLocalMatchmakerConfig {
    /** マッチ成立を許す rating 差の上限 (この値を超える相手とはマッチしない)。 */
    acs::u32 MaxRatingDelta = 150;
};

/**
 * プロセス内で完結する決定論的なローカル matchmaker 実装。
 *
 * @details
 * acs::game::IMatchmaker を固定長スロット (kMaxTickets) のプールだけで実装する。
 * StartSearch で空き / 退役済みスロットに ticket を発行し、同 mode かつ rating 差が
 * MaxRatingDelta 以内の Searching な相手が居れば双方を Matched にする。終端状態
 * (Cancelled / 双方 Accept 済み) に入ったスロットは退役させ、満杯時に最古の退役
 * スロットを再利用することで ticket リークを防ぐ。ネットワークは一切使わない。
 */
class FLocalMatchmaker final : public acs::game::IMatchmaker {
public:
    /** 既定設定 (MaxRatingDelta=150) で構築する。 */
    FLocalMatchmaker() noexcept = default;

    /**
     * 設定を指定して構築する。
     *
     * @param config 適用する matchmaker 設定。
     */
    explicit FLocalMatchmaker(const FLocalMatchmakerConfig& config) noexcept;

    /** 破棄する (保持リソースは固定長配列のみ)。 */
    ~FLocalMatchmaker() noexcept override = default;

    /** コピー禁止 (ticket プールを単独所有するため)。 */
    FLocalMatchmaker(const FLocalMatchmaker&)            = delete;

    /** コピー代入も禁止。 */
    FLocalMatchmaker& operator=(const FLocalMatchmaker&) = delete;

    /** ムーブ禁止。 */
    FLocalMatchmaker(FLocalMatchmaker&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FLocalMatchmaker& operator=(FLocalMatchmaker&&)      = delete;

    /**
     * マッチ検索を開始し、新しい ticket を発行する。
     *
     * @details
     * 空き / 退役済みスロットを 1 つ確保して Searching 状態の ticket を作り、同 mode
     * かつ rating 差が MaxRatingDelta 以内の Searching な相手が既に居れば双方を即時に
     * Matched にする。mode が nullptr / 空文字なら BadArgument、プールが真に満杯なら
     * LocalMatchFull エラーを返す。
     * @param mode 検索モードの文字列キー (内部に最大 63 文字をコピー)。
     * @param elo_hint 呼出側が知っている rating 推定値。
     * @return 成功なら発行された MatchTicket、失敗ならエラー。
     */
    acs::TResult<acs::game::MatchTicket> StartSearch(const char* mode,
                                                      acs::u32 elo_hint) noexcept override;

    /**
     * 検索をキャンセルする。
     *
     * @details
     * 対象を Cancelled にして退役させる。マッチ済みだった相手がまだ Accept 前なら
     * Searching に戻して再マッチ可能にする。未知 / 無効 ticket は InvalidTicket エラー。
     * @param ticket キャンセル対象の ticket。
     * @return 成功なら空の TResult、無効 ticket ならエラー。
     */
    acs::TResult<void>                   CancelSearch(acs::game::MatchTicket ticket) noexcept override;

    /**
     * 現在の検索状態を返す (副作用なし)。
     *
     * @param ticket 状態を引く ticket。
     * @return 対応する EMatchStatus (未知 / 無効 ticket は Failed)。
     */
    acs::game::EMatchStatus              PollStatus(acs::game::MatchTicket ticket) noexcept override;

    /**
     * Matched 状態の ticket を確定する。
     *
     * @details
     * 対象に Accept フラグを立て、相手も Accept 済みなら双方を退役させてマッチ確定と
     * する (Status は Matched のまま残るので PollStatus は確定済みを返し続ける)。
     * 未知 / 無効 ticket は InvalidTicket、Matched でない ticket は WrongState エラー。
     * @param ticket 確定する ticket。
     * @return 成功なら空の TResult、無効 ticket / 状態不正ならエラー。
     */
    acs::TResult<void>                   AcceptMatch(acs::game::MatchTicket ticket) noexcept override;

    /** 全スロットを初期化し、ticket / retire シーケンスと有効数を 0 に戻す。 */
    void Clear() noexcept;

    /**
     * 現在アクティブな (退役していない生存中の) ticket 数を返す。
     *
     * @return 生存中 ticket の数。
     */
    acs::u32 ActiveTicketCount() const noexcept { return m_ActiveCount; }

private:
    /**
     * 1 件分のマッチ検索状態を保持する内部スロット。
     */
    struct FEntry {
        /** このスロットに発行された ticket (m_Opaque=0 は未発行)。 */
        acs::game::MatchTicket Ticket{};

        /** 現在の検索状態。 */
        acs::game::EMatchStatus Status = acs::game::EMatchStatus::Failed;

        /** 検索モードの文字列 (StartSearch でコピー、最大 63 文字 + 終端)。 */
        char Mode[64] = {};

        /** 呼出側から渡された rating 推定値。 */
        acs::u32 EloHint = 0;

        /** マッチ済み相手の ticket 値 (0 = 相手なし)。 */
        acs::u64 PartnerTicket = 0;

        /**
         * 退役 (Retire) シーケンス番号。
         *
         * @details
         * 終端状態 (Cancelled / 双方 Accept 済み) に入った瞬間に非ゼロ値を割り当てる。
         * bActive=true のまま退役することで PollStatus は終端 Status を返し続けつつ、
         * スロットが満杯のときに最古の退役スロットを FindFreeEntry が再利用できる
         * (= ticket リーク防止)。値が小さいほど古い (m_NextRetire の昇順)。
         * 0 は「未退役 (生存中)」。
         */
        acs::u64 RetireSeq = 0;

        /** スロットが使用中 (発行済みで未 Clear) かを表すフラグ。 */
        bool bActive = false;

        /** このスロットの呼出側が AcceptMatch 済みかを表すフラグ。 */
        bool bAccepted = false;
    };

    /** ticket プールのスロット数の上限。 */
    static constexpr acs::u32 kMaxTickets = 32;

    /** プール満杯エラーのサブコード。 */
    static constexpr acs::u16 kSubLocalMatchFull = 2201;

    /** 未知 / 無効 ticket エラーのサブコード。 */
    static constexpr acs::u16 kSubLocalMatchInvalidTicket = 2202;

    /** 状態不正 (Matched でない ticket を Accept 等) エラーのサブコード。 */
    static constexpr acs::u16 kSubLocalMatchWrongState = 2203;

    /**
     * NUL 終端文字列同士が等しいかを比較する。
     *
     * @param a 比較対象その 1 (nullptr なら不一致)。
     * @param b 比較対象その 2 (nullptr なら不一致)。
     * @return 両文字列が完全一致なら true。
     */
    static bool StrEq(const char* a, const char* b) noexcept;

    /**
     * src を dst へ NUL 終端を保証してコピーする。
     *
     * @details dst_size が 0 や src が nullptr のときは dst を空文字にして戻る。
     * @param dst コピー先バッファ。
     * @param dst_size dst の総バイト数 (終端含む)。
     * @param src コピー元文字列 (nullptr 可)。
     */
    static void CopyText(char* dst, acs::usize dst_size, const char* src) noexcept;

    /**
     * ticket に対応するアクティブなスロットを探す。
     *
     * @param ticket 探す ticket (無効値は nullptr)。
     * @return 一致した bActive なスロット (無ければ nullptr)。
     */
    FEntry* FindEntry(acs::game::MatchTicket ticket) noexcept;

    /**
     * ticket に対応するアクティブなスロットを探す (const 版)。
     *
     * @param ticket 探す ticket (無効値は nullptr)。
     * @return 一致した bActive なスロットへの const ポインタ (無ければ nullptr)。
     */
    const FEntry* FindEntry(acs::game::MatchTicket ticket) const noexcept;

    /**
     * 新規 ticket を入れられるスロットを確保する。
     *
     * @details
     * まず未使用 (bActive=false) のスロットを優先し、無ければ退役済み (RetireSeq!=0)
     * のうち最古のものを再利用する。退役スロットも無ければ真に満杯。
     * @return 使用可能なスロット (満杯なら nullptr)。
     */
    FEntry* FindFreeEntry() noexcept;

    /**
     * 指定 mode / rating とマッチ可能な Searching 中のスロットを探す。
     *
     * @param mode マッチ条件のモード文字列。
     * @param elo_hint マッチ条件の rating 推定値。
     * @return 条件を満たす最初の Searching スロット (無ければ nullptr)。
     */
    FEntry* FindCompatibleEntry(const char* mode, acs::u32 elo_hint) noexcept;

    /**
     * 2 つの rating がマッチ許容差以内かを返す。
     *
     * @param a 一方の rating。
     * @param b もう一方の rating。
     * @return 差が m_MaxRatingDelta 以下なら true。
     */
    bool IsRatingCompatible(acs::u32 a, acs::u32 b) const noexcept;

    /**
     * 終端状態に入ったスロットを退役させてスロットを再利用可能にする。
     *
     * @details
     * m_NextRetire を割り当てて RetireSeq を非ゼロにし、m_ActiveCount を 1 回だけ
     * 減算する。既に退役済みなら何もしない (冪等)。bActive は true のまま残すので
     * 後続スロットが満杯になるまで PollStatus は終端 Status を返し続けられる。
     * @param entry 退役させるスロット。
     */
    void RetireEntry(FEntry& entry) noexcept;

    /** ticket プール (固定長スロット配列)。 */
    FEntry m_Entries[kMaxTickets] = {};

    /** 次に発行する ticket の不透明値 (1 から単調増加)。 */
    acs::u64 m_NextTicket = 1;

    /** 次に割り当てる退役シーケンス番号 (1 から単調増加)。 */
    acs::u64 m_NextRetire = 1;

    /** 生存中 (退役していない) ticket の数。 */
    acs::u32 m_ActiveCount = 0;

    /** マッチ成立を許す rating 差の上限。 */
    acs::u32 m_MaxRatingDelta = 150;
};

/**
 * プロセス共有の既定 FLocalMatchmaker singleton を返す。
 *
 * @details matchmaker provider に登録する provider 実体として使う。
 * @return ローカル matchmaker singleton への参照。
 */
acs::game::IMatchmaker& GetDefaultLocalMatchmaker() noexcept;

/**
 * gameframework の matchmaker provider に本実装を既定として登録する。
 *
 * @details
 * アプリ起動時に一度だけ呼ぶと、以降 acs::game::GetDefaultMatchmaker() が
 * GetDefaultLocalMatchmaker() の返すローカル matchmaker (プロセス共有 singleton)
 * を返すようになる。これにより上位コードは backend 非依存に
 * GetDefaultMatchmaker() だけで実 matchmaker を取得できる。
 */
void InstallLocalMatchmakerAsDefault() noexcept;

} // namespace acs::localmatch
