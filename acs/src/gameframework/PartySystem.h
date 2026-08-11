// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"

namespace acs::game {

/**
 * フレンド 1 件分。
 *
 * @details
 * platform_id / display_name は両方 const char* 非所有で、寿命は呼び出し側が保証する
 * (文字列リテラル or 永続バッファ、Pillar O CEntitlementRegistry と同じポリシー)。
 */
struct FFriend {
    /** SDK 固有のユーザー識別子 (例 "steam:..." / "epic:..." / PSN account_id)。 */
    const char* platform_id        = nullptr;

    /** 表示名 (寿命は呼び出し側保証)。 */
    const char* display_name       = nullptr;

    /** オンライン中か。 */
    bool        online             = false;

    /** 同タイトル起動中か (Now Playing 用)。 */
    bool        playing_same_game  = false;
};

/**
 * パーティメンバ 1 件分。
 *
 * @details
 * is_leader は招待・キック権限を持つかの判定に使う想定 (本 system は権限チェックせず
 * フラグ保持のみ)。is_ready はロビー UI の準備完了表示用 (開始判定は呼び出し側)。
 */
struct FPartyMember {
    /** SDK 固有 ID (platform_id と同形式想定、非所有)。 */
    const char* player_id     = nullptr;

    /** 表示名 (非所有)。 */
    const char* display_name  = nullptr;

    /** リーダー権限フラグ。 */
    bool        is_leader     = false;

    /** ready up 済か。 */
    bool        is_ready      = false;
};

/**
 * パーティ状態 (state machine)。
 *
 * @details
 * 遷移は Solo --[CreateParty/JoinParty]--> Joining --(success)--> InParty、
 * timeout/err なら Joining --> Solo。InParty --[LeaveParty]--> Leaving --(complete)--> Solo。
 * Joining / Leaving は非同期 SDK 呼び出し完了待ちの中間状態。
 */
enum class EPartyState : u8 {
    /** パーティ非所属 (初期値)。 */
    Solo     = 0,

    /** パーティ所属中。 */
    InParty  = 1,

    /** 参加要求送信済 / 応答待ち。 */
    Joining  = 2,

    /** 離脱要求送信済 / 応答待ち。 */
    Leaving  = 3,
};

/**
 * パーティ state machine とフレンドリストを扱うプラットフォーム非依存の窓口。
 *
 * @details
 * ローカルの Solo / InParty / Joining / Leaving 状態遷移とメンバ・フレンド roster
 * 管理を担う。実 SDK 接続 (ISteamworksBridge / EOS / PSN / Xbox / NSO) は seam として
 * 別レイヤが担当し、Joining / Leaving は Tick で仮想完了する。文字列はすべて const char*
 * 非所有で寿命は呼び出し側が保証する。
 */
class CPartySystem {
public:
    /** 空状態 (Solo) で構築する。 */
    CPartySystem()  noexcept = default;

    /** 破棄する。 */
    ~CPartySystem() noexcept = default;

    /** コピー禁止 (長寿命 1 個運用で state 分裂を避けるため)。 */
    CPartySystem(const CPartySystem&)            = delete;

    /** コピー代入も禁止。 */
    CPartySystem& operator=(const CPartySystem&) = delete;

    /** ムーブ禁止。 */
    CPartySystem(CPartySystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CPartySystem& operator=(CPartySystem&&)      = delete;

    /**
     * 新規パーティを作成する (Solo 状態のみ受理)。
     *
     * @details 状態を Joining に遷移し、seam の仮想完了モードで Tick が InParty へ自動遷移する。
     * @param party_name パーティ名 (const char* 非所有、寿命は呼び出し側保証)。
     * @return 成功なら Ok、状態不正 / party_name == nullptr ならエラー。
     */
    TResult<void> CreateParty(const char* party_name) noexcept;

    /**
     * 既存パーティに参加する (Solo 状態のみ受理)。
     *
     * @details 状態を Joining に遷移し、Tick で timeout を監視する。
     * @param party_id 招待で受け取った SDK 固有のパーティ ID (非所有)。
     * @return 成功なら Ok、状態不正 / party_id == nullptr ならエラー。
     */
    TResult<void> JoinParty(const char* party_id) noexcept;

    /**
     * 現在のパーティから離脱する (InParty 状態のみ受理)。
     *
     * @details 状態を Leaving に遷移し、Tick で完了して Solo に戻る。
     * @return 成功なら Ok、InParty 以外ならエラー。
     */
    TResult<void> LeaveParty() noexcept;

    /**
     * フレンドにパーティ招待を送る (InParty 状態のみ受理)。
     *
     * @details リーダー権限判定は呼び出し側の責任 (本 system はフラグ参照のみ)。実 invite 送信は Storefront 側 (ISteamworksBridge 等) が担当。
     * @param friend_id 招待先の SDK 固有 ID (非所有)。
     * @return 成功なら Ok、状態不正 / friend_id == nullptr ならエラー。
     */
    TResult<void> InviteFriend(const char* friend_id) noexcept;

    /**
     * パーティ所属中 (InParty) かを返す。
     *
     * @return InParty 状態なら true。
     */
    bool       IsInParty()    const noexcept { return _state == EPartyState::InParty; }

    /**
     * 現在のパーティ状態を返す。
     *
     * @return 現在の EPartyState。
     */
    EPartyState State()        const noexcept { return _state; }

    /** FindMember() が「見つからない」を表す番兵 index (u32 全 1)。GetMember() に渡しても安全に nullptr を返す。 */
    static constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

    /**
     * パーティ内メンバ数 (自分含む) を返す。
     *
     * @return メンバ数 (Solo 状態は 0)。
     */
    u32                MemberCount() const noexcept;

    /**
     * メンバ生バッファの先頭を返す。
     *
     * @details MemberCount() 件が有効。Leave / Add 系の操作で無効化される。
     * @return メンバ配列の先頭ポインタ。
     */
    const FPartyMember* Members()     const noexcept;

    /**
     * メンバを 1 件追加する (自分含むローカル roster)。
     *
     * @details SDK 側で accept された相手をローカル roster に流し込む想定。同 player_id の二重追加は上書きせず弾く。
     * @param member 追加するメンバ。
     * @return 成功なら Ok、player_id == nullptr は Generic+8、重複は Generic+9。
     */
    TResult<void> AddMember(const FPartyMember& member) noexcept;

    /**
     * player_id 一致のメンバを削除する (順序を保って前詰め)。
     *
     * @param player_id 削除対象の player_id。
     * @return 削除したら true、見つからない / nullptr なら false (no-op)。
     */
    bool RemoveMember(const char* player_id) noexcept;

    /**
     * index 番のメンバへの非所有ポインタを返す。
     *
     * @param index メンバの index。
     * @return メンバへのポインタ (範囲外 / kInvalidIndex は nullptr)。
     */
    const FPartyMember* GetMember(u32 index) const noexcept;

    /**
     * player_id 一致のメンバが居るかを返す。
     *
     * @param player_id 探す player_id。
     * @return 居れば true (nullptr は false)。
     */
    bool HasMember(const char* player_id) const noexcept;

    /**
     * player_id 一致のメンバの index を返す。
     *
     * @details 戻り値は GetMember() にそのまま渡せる。
     * @param player_id 探す player_id。
     * @return 一致メンバの index (見つからない / nullptr は kInvalidIndex)。
     */
    u32 FindMember(const char* player_id) const noexcept;

    /**
     * フレームごとの状態更新を行う。
     *
     * @details Joining / Leaving の timeout 監視と仮想 SDK 完了の状態遷移を進める。呼ばないと Joining のまま停滞する。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * フレンドを 1 件登録する。
     *
     * @details SDK の friend list 取得結果を Storefront 側から流し込む想定。platform_id == nullptr は no-op で防御。
     * @param f 登録するフレンド。
     */
    void AddFriend(const FFriend& f) noexcept;

    /**
     * フレンド件数を返す (オンライン / オフライン両方含む)。
     *
     * @return フレンド数。
     */
    u32           FriendCount() const noexcept;

    /**
     * フレンド生バッファの先頭を返す。
     *
     * @details FriendCount() 件が有効。AddFriend で無効化される。
     * @return フレンド配列の先頭ポインタ。
     */
    const FFriend* Friends()     const noexcept;

private:
    /**
     * 自分自身をリーダーとしてメンバリストの先頭に登録する。
     *
     * @details
     * CreateParty / JoinParty 完了時の仮想 SDK 完了ハンドラの一部。現状は固定文字列の
     * 仮想プレイヤー "self" を入れる (Storefront 経由で自分の platform_id を取得して差し替える想定)。
     */
    void EmplaceSelfAsLeader() noexcept;

    /** 現在のパーティ状態。 */
    EPartyState         _state          = EPartyState::Solo;

    /** CreateParty 時に保持するパーティ名 (非所有)。 */
    const char*        m_PartyName     = nullptr;

    /** JoinParty 時に保持するパーティ ID (非所有)。 */
    const char*        m_PartyId       = nullptr;

    /** Joining / Leaving の経過秒 (仮想 SDK 完了の計時)。 */
    f32                m_PendingTimer  = 0.0f;

    /** パーティメンバの roster。 */
    TArray<FPartyMember> m_Members;

    /** フレンドリスト。 */
    TArray<FFriend>      m_Friends;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FPartySystem = CPartySystem;

} // namespace acs::game
