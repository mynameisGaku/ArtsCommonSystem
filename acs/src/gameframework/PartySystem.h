// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar T — FPartySystem (パーティ state machine + フレンドリスト seam)
//
// 役割:
//   1〜N 人のローカルプレイヤーをまとめた「パーティ」状態と、簡易フレンドリストを
//   ゲームロジック側から扱う窓口。マッチング前のロビー、Co-op 入室前の集合場所、
//   ストアでのフレンド表示などを単一の API で扱う。実プラットフォーム接続は
//   Pillar S = Storefront 側 (`FSteamworksBridge` / EOS / PSN / Xbox / NSO) が
//   アダプタとなり、結果を本 system に流し込む。FPartySystem 自体は **プラット
//   フォーム非依存**。
//
// 設計上の倫理方針 (cross-platform party + 児童保護):
//   ・cross-play party は **プラットフォームごとの規約** に従う前提。例えば iOS と
//     Switch 同士の文字チャットは双方プラットフォームの審査次第。本 system は
//     state machine とメンバ管理だけを担い、チャット / モデレーションは別レイヤ。
//   ・**moderation / blocking は別モジュール** (将来 `Moderation` / `BlockList` を
//     新設)。本 system は誰でも招待可能な原始 API を提供し、ブロック判定は
//     上位レイヤで InviteFriend の戻り値に乗せる責任分離。
//   ・**under-18 デフォルト**: 未成年アカウントが既知の場合 (Pillar S 経由で取得)、
//     呼び出し側が「招待を受け付けない / 文字チャット無効」等のポリシーをかぶせる
//     ことを想定。本 system はフラグを持たず、強制機構も入れない (各プラットフォーム
//     ごとに年齢推定 API が異なるため一律ルール化が危険)。
//
// 使い方 (典型例):
//   FPartySystem ps;
//   ps.AddFriend({ "steam:76561198000000001", "alice",  true,  true  });
//   ps.AddFriend({ "epic:abc123",              "bob",    false, false });
//
//   auto r = ps.CreateParty("my_squad");
//   if (r) {
//       ps.InviteFriend("steam:76561198000000001");
//       // ... Pillar S 側で invite 送信、相手が accept すると外部から
//       //     メンバ追加を流し込む API が呼ばれる (本フェーズでは省略)
//   }
//   // each frame:
//   ps.Tick(dt);  // joining / leaving の timeout 処理
//
// 設計選択 (Phase T-1 スケルトン):
//   ・**ローカル state は完全実装**: Solo / InParty / Joining / Leaving の遷移、
//     メンバ追加・削除、リーダー / ready flag、フレンドリスト管理はすべて動く。
//   ・**プラットフォーム接続は TODO**: FSteamworksBridge 等を介した実 invite 送信、
//     PSN cross-gen party、Xbox party、Nintendo Switch friend code の解決は
//     Phase T-2 以降で接続。本 system は seam として const char* (platform id) を
//     受けるだけで、解釈は bridge 側が行う。
//   ・**const char* 非所有**: 規約通り <string> 不使用。party_name / party_id /
//     friend_id / display_name の寿命は呼び出し側 (文字列リテラル or 長寿命
//     バッファ) が保証する。SDK 側で動的取得した名前は呼び出し側で永続バッファに
//     コピーして渡す責任を負う。
//   ・**最大メンバ数は固定無し**: TArray で動的拡張、現実的に 2〜8 人が大半なので
//     線形走査で十分。Pillar S の plat 制約 (例 PSN は 100 人 group 等) は bridge
//     側で弾く。
//   ・**コピー / ムーブ禁止**: party system は通常 1 つの長寿命オブジェクトで運用。
//     誤って値渡しされて state が分裂すると詰むため、最初から非コピー・非ムーブ。
//   ・**全 noexcept**: ACS 全体方針 (TResult<T, FErrorCode> + bool 戻り値)。
//
// 範囲外 (Phase T-2 以降):
//   ・実 SDK 接続 (FSteamworksBridge / EOS / PSN / Xbox / NSO の invite/accept 送受信)
//   ・voice chat / text chat (Pillar H Audio + 別チャットモジュール)
//   ・moderation / blocking / report (別モジュール)
//   ・matchmaking (Pillar V Backend Services 側)
//   ・persistent group / クラン機能 (本 system は ephemeral party のみ扱う)
//   ・cross-progression token 連携 (Pillar S 経由)
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"

namespace acs::game {

// フレンド 1 件分。`platform_id` は SDK 固有のユーザー識別子 (例 "steam:..." /
// "epic:..." / PSN account_id 等)、`display_name` は表示名。両方 const char*
// 非所有 (寿命は呼び出し側保証、Pillar O FEntitlement と同じポリシー)。
struct Friend {
    const char* platform_id        = nullptr;  // SDK 固有 ID (文字列リテラル / 永続バッファ)
    const char* display_name       = nullptr;  // 表示名 (寿命は呼び出し側保証)
    bool        online             = false;    // オンライン中か
    bool        playing_same_game  = false;    // 同タイトル起動中か (Now Playing 用)
};

// パーティメンバ 1 件分。`is_leader` は招待・キック権限を持つかの判定に使われる
// 想定 (本 system 側は権限チェックを行わず、フラグ保持のみ)。`is_ready` は
// ロビー UI の準備完了表示用 (マッチング開始判定は呼び出し側)。
struct PartyMember {
    const char* player_id     = nullptr;  // SDK 固有 ID (platform_id と同形式想定)
    const char* display_name  = nullptr;  // 表示名
    bool        is_leader     = false;    // リーダー権限フラグ
    bool        is_ready      = false;    // ready up 済か
};

// パーティ状態。state machine の遷移は以下:
//   Solo  --[CreateParty / JoinParty]--> Joining --(success)--> InParty
//                                              \-(timeout/err)-> Solo
//   InParty --[LeaveParty]--> Leaving --(complete)--> Solo
//   ※ Joining / Leaving は非同期 SDK 呼び出し完了待ちの中間状態。
enum class EPartyState : u8 {
    Solo     = 0,  // パーティ非所属 (初期値)
    InParty  = 1,  // パーティ所属中
    Joining  = 2,  // 参加要求送信済 / 応答待ち
    Leaving  = 3,  // 離脱要求送信済 / 応答待ち
};

class FPartySystem {
public:
    FPartySystem()  noexcept = default;
    ~FPartySystem() noexcept = default;

    // 通常は長寿命 1 個運用。誤コピーで state 分裂を避けるため非コピー・非ムーブ。
    FPartySystem(const FPartySystem&)            = delete;
    FPartySystem& operator=(const FPartySystem&) = delete;
    FPartySystem(FPartySystem&&)                 = delete;
    FPartySystem& operator=(FPartySystem&&)      = delete;

    // ----- party 操作 -----
    // 新規パーティ作成。Solo 状態のみ受理。party_name は const char* 非所有
    // (寿命は呼び出し側保証)。SDK 接続は seam として Joining → InParty に
    // 自動遷移する仮想完了モードで動作 (Phase T-2 で SDK 呼び出しに差し替え)。
    TResult<void> CreateParty(const char* party_name) noexcept;

    // 既存パーティに参加。Solo 状態のみ受理。party_id は招待で受け取った
    // SDK 固有 ID。状態を Joining に遷移、Tick で timeout 監視。
    TResult<void> JoinParty(const char* party_id) noexcept;

    // 現在のパーティから離脱。InParty 状態のみ受理。状態を Leaving に遷移。
    TResult<void> LeaveParty() noexcept;

    // フレンドにパーティ招待を送る。InParty 状態のみ受理、リーダー権限は
    // 呼び出し側で判定する責任 (本 system はフラグ参照のみ)。実 invite 送信は
    // Pillar S = Storefront 側 (FSteamworksBridge 等) が担当。
    TResult<void> InviteFriend(const char* friend_id) noexcept;

    // ----- state query -----
    bool       IsInParty()    const noexcept { return _state == EPartyState::InParty; }
    EPartyState State()        const noexcept { return _state; }

    // ----- メンバアクセス -----
    // パーティ内メンバ数 (自分含む)。Solo 状態は 0。
    u32                MemberCount() const noexcept;

    // メンバ生バッファ (MemberCount() 件)。Leave / Add 系で無効化される。
    const PartyMember* Members()     const noexcept;

    // ----- フレームごとの状態更新 -----
    // Joining / Leaving の timeout 監視 + 仮想 SDK 完了の状態遷移。
    // Tick を呼ばないと Joining 状態のまま停滞する点に注意。
    void Tick(f32 dt) noexcept;

    // ----- フレンドリスト -----
    // フレンド 1 件登録。SDK の friend list 取得結果を Pillar S 側で
    // 流し込む想定。platform_id == nullptr は no-op で防御。
    void AddFriend(const Friend& f) noexcept;

    // フレンド件数 (オンライン / オフライン両方含む)。
    u32           FriendCount() const noexcept;

    // フレンド生バッファ (FriendCount() 件)。AddFriend で無効化される。
    const Friend* Friends()     const noexcept;

private:
    // ---- 内部ヘルパ ----
    // 自分自身をリーダーとしてメンバリストの先頭に登録する。CreateParty 完了時
    // および JoinParty 完了時に呼ばれる仮想 SDK 完了ハンドラの一部。
    // 仮想プレイヤーとして "self" を入れる (Pillar T-2 で Pillar S の自分の
    // platform_id を取得して差し替える)。
    void EmplaceSelfAsLeader() noexcept;

    EPartyState         _state          = EPartyState::Solo;
    const char*        m_PartyName     = nullptr;   // CreateParty 時に保持 (非所有)
    const char*        m_PartyId       = nullptr;   // JoinParty 時に保持 (非所有)
    f32                m_PendingTimer  = 0.0f;      // Joining / Leaving 経過秒
    TArray<PartyMember> m_Members;
    TArray<Friend>      m_Friends;
};

} // namespace acs::game
