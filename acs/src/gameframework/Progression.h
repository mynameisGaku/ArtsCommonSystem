// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — CProgression (XP / Level / Milestones / Unlocks)
//
// 「累計 XP を加算するとレベルが上がり、所定の XP 閾値を越えると Milestone を
// 達成 → コンテンツが Unlock される」というジャンルを問わず広く使われる進行
// システムを 1 クラスにまとめた小型マネージャ。
//
// 想定する位置付け:
//   ・Pillar S (CAchievementManager) との違い:
//     - Achievement は「最終的に解除/未解除の 2 値フラグ」を管理する SDK 連動装置。
//       Storefront 配信プラットフォームへ片方向送信する責務まで含む。
//     - CProgression は「累積 XP に対してレベルとマイルストーンが線形に増える」
//       純粋にゲーム内で完結する進行カウンタ。プラットフォーム SDK には依存しない。
//   ・Unlock 連携:
//     - 各 FMilestoneDef に `unlock_content_id` を持たせる。CProgression 自身は
//       コンテンツ解放の実体を持たず、達成時に MilestoneCallback でゲーム側へ通知する。
//       受け取った側が CEntitlementRegistry / ContentManager / CAchievementManager
//       に橋渡しする責務。
//
// 使い方:
//   CProgression p;
//
//   // 起動時にゲーム側でマイルストーンを 1 度ずつ登録。
//   p.RegisterMilestone({ "ms.level_5",  "Lv.5 到達",   31,   "content.weapon_b" });
//   p.RegisterMilestone({ "ms.level_10", "Lv.10 到達",  1023, "content.area_2"   });
//   p.RegisterMilestone({ "ms.veteran",  "Veteran",     16383,"content.title_x"  });
//
//   // (任意) 達成時 callback を登録。ゲーム側で SE 再生・トースト UI 等。
//   p.SetOnAchievedCallback(&OnMilestoneAchieved, /*user*/ &game);
//
//   // ゲームプレイ中の XP 加算 (敵撃破・クエスト完了・収集物等で呼ぶ)。
//   p.AwardXp(50);
//   p.AwardXp(20);
//
//   // UI 表示。
//   u32 lv  = p.CurrentLevel();
//   u32 xp  = p.CurrentXp();
//   u32 cur = p.AchievedCount();
//   u32 tot = p.MilestoneCount();
//
// 設計選択:
//   ・**累計 XP は u32**:
//     - 64bit にする必要があるほど一回のセッションで XP を稼ぐタイトルは稀。
//       u32 max = 約 42 億で実用上の上限を遥かに超える。オーバーフロー時は
//       max にクランプする (足し算サイレントラップを避ける)。
//   ・**レベル計算は floor(log2(xp + 1))**:
//     - 単純な log2 ベース計算なので、初期は急成長 → 中盤以降は緩やかに、という
//       典型的な RPG 風カーブを近似できる。+1 補正で xp=0 のとき level=0 になる。
//     - 浮動小数 log2 を使わず、整数ビット走査で算出 (.cpp 側のループ実装で
//       プラットフォーム非依存)。
//     - XP <= 0 → Level 0、XP=1 → Level 1、XP=3 → 2、XP=7 → 3、XP=15 → 4、…
//     - 必要なら将来 `SetLevelCurve(callback)` で差し替え可能だが、今は YAGNI。
//   ・**FMilestoneDef / FMilestoneState を別配列で持つ**:
//     - CAchievementManager と同じ pattern。Def は immutable な定義 (id /
//       display_name / required_xp / unlock_content_id)、FMilestoneState は実行時の
//       達成状態 (id / achieved / achieved_timestamp)。1:1 対応で同 index を共有。
//   ・**所有しない const char***:
//     - id / display_name / unlock_content_id は呼出側 (ゲームコード or リソース
//       バンドル) が保証する static lifetime の文字列リテラルを想定。
//       CProgression 側ではコピーしない (STL <string> 禁止方針)。
//   ・**重複登録は黙って弾く**:
//     - 同 id を 2 度 RegisterMilestone しても 2 回目は no-op。
//       他 Manager 系 (CEntitlementRegistry / Achievement) と同じ防御方針。
//   ・**線形検索**:
//     - Milestone 件数は 1 タイトルで通常 10〜100 程度。TArray<T> の per-byte
//       文字列比較 + 線形走査で十分。
//   ・**MilestoneCallback は関数ポインタ + user data**:
//     - CTriggerWorld2D / CSceneTimer と同じ pattern。`std::function` は STL 禁止
//       方針で使えないので、`void(*)(void*,...)` で固定。1 種類のみ (達成時)
//       なので命名は `MilestoneCallback`。
//   ・**全 noexcept、非コピー・非ムーブ**:
//     - 他 Manager 系と統一。誤って値渡しされて進捗が分裂すると検知し辛い。
//   ・**STL 不使用、`<string>` 禁止**:
//     - ACS 全体方針。文字列は const char* 非所有のみ。
//
// 範囲外:
//   ・XP 倍率・経験値テーブル差し替え (今はハードコード log2 ベース)
//   ・SDK 統合 (Steamworks 等は CAchievementManager 側で扱う)
//   ・seasonal reset / prestige system (別 API として追加検討)
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"

namespace acs::game {

/**
 * CProgression の永続化固有エラー subcode。
 *
 * @details
 * CSaveArchive の subcode はそのまま伝搬し、CProgression payload 内部 schema の検証失敗だけを
 * 200番台で表す。
 */
enum class EProgressionPersistenceSubCode : u32 {
    /** entry数、固定値、payload長など内部schemaが不正。 */
    kSubMalformedPayload       = 200,

    /** 登録数または保存entry数が安全上限を超えた。 */
    kSubMilestoneLimitExceeded = 201,

    /** 保存・読込・staging用bufferを確保できなかった。 */
    kSubAllocationFailed       = 202,

    /** definition/state配列の1:1不変条件が崩れている。 */
    kSubStateInvariant         = 203,
};

/**
 * 起動時に 1 回登録される immutable なマイルストーン定義。
 *
 * @details required_xp 以上に累計 XP が達した瞬間に達成扱いになる。文字列は非所有
 * (呼出側が static lifetime を保証する文字列リテラル想定)。
 */
struct FMilestoneDef {
    /** 検索 / 永続化のキー (文字列リテラル想定、非所有)。 */
    const char* id                = nullptr;

    /** UI 表示用の名前 (リスト・トースト等、非所有)。 */
    const char* display_name      = nullptr;

    /** 達成に必要な累計 XP (これ以上で達成扱い)。 */
    u32         required_xp       = 0;

    /** 達成時に解放されるコンテンツの ID (nullptr 許容、演出のみ)。 */
    const char* unlock_content_id = nullptr;
};

/**
 * 実行時に変化する 1 マイルストーンの状態。
 *
 * @details FMilestoneDef と同 index 位置で 1:1 対応する。
 */
struct FMilestoneState {
    /** 対応する FMilestoneDef の id を参照コピーした値 (検索 / 表示用)。 */
    const char* id                 = nullptr;

    /** 達成済みフラグ。 */
    bool        achieved           = false;

    /** 達成時の FClock::MillisSinceStartup() (起動からの ms、0 は未達成/未取得)。 */
    u64         achieved_timestamp = 0;
};

/**
 * マイルストーン達成時に 1 回呼ばれる callback 型。
 *
 * @details user は SetOnAchievedCallback で渡した値、milestone_id は登録時の id
 * (リテラル) がそのまま渡る。
 */
using MilestoneCallback = void(*)(void* user, const char* milestone_id) noexcept;

/**
 * 累計 XP / レベル / マイルストーン / アンロックを束ねる進行システム。
 *
 * @details
 * AwardXp で累計 XP を加算するとレベルが floor(log2(xp+1)) で上がり、所定の閾値を
 * 越えると milestone が達成され、登録済み callback でゲーム側へ通知する。CProgression
 * 自身はコンテンツ解放の実体を持たず、プラットフォーム SDK にも依存しない。
 * FMilestoneDef (immutable 定義) と FMilestoneState (実行時状態) を同 index で
 * 1:1 に持つ。全 noexcept、非コピー・非ムーブ、STL 不使用 (文字列は const char* 非所有)。
 */
class CProgression {
public:
    /** 保存・復元できる milestone 件数の安全上限。 */
    static constexpr u32 kMaxPersistedMilestones = 4096u;

    /** 空状態で構築する (XP=0、milestone なし)。 */
    CProgression()  noexcept = default;

    /** 破棄する (非所有文字列のみ保持するため何もしない)。 */
    ~CProgression() noexcept = default;

    /** コピー禁止 (進捗が分裂するのを防ぐため)。 */
    CProgression(const CProgression&)            = delete;

    /** コピー代入も禁止。 */
    CProgression& operator=(const CProgression&) = delete;

    /** ムーブ禁止 (進捗が分裂するのを防ぐため)。 */
    CProgression(CProgression&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CProgression& operator=(CProgression&&)      = delete;

    /**
     * マイルストーン定義を登録する (起動時に 1 度ずつ)。
     *
     * @details
     * 同 id の2重登録、永続化hashが既存定義と衝突する定義、def.id == nullptr、
     * kMaxPersistedMilestones 超過は no-op。確保失敗時も既存定義・状態を保持する。
     * @param def 登録する immutable なマイルストーン定義。
     */
    void RegisterMilestone(const FMilestoneDef& def) noexcept;

    /**
     * 累計 XP に amount を加算し、各 milestone の達成判定を行う。
     *
     * @details 加算結果が u32 を超える場合は max にクランプ。amount = 0 は no-op
     * (callback も発火しない)。
     * @param amount 加算する XP 量。
     */
    void AwardXp(u32 amount) noexcept;

    /**
     * 現在の累計 XP を返す。
     *
     * @return 累計 XP。
     */
    u32 CurrentXp()    const noexcept;

    /**
     * 現在のレベルを返す。
     *
     * @details floor(log2(xp + 1))。xp = 0 → 0、xp = 1 → 1、xp = 3 → 2、xp = 7 → 3、…
     * @return 現在のレベル。
     */
    u32 CurrentLevel() const noexcept;

    /**
     * 指定 id のマイルストーンが達成済みかを返す。
     *
     * @param id 照会する milestone の id。
     * @return 達成済みなら true (id == nullptr / 未登録 id は false)。
     */
    bool IsMilestoneAchieved(const char* id) const noexcept;

    /**
     * 登録済みマイルストーンの総数を返す。
     *
     * @return 登録総数 (UI の "5/20 unlocked" 表示用)。
     */
    u32 MilestoneCount() const noexcept;

    /**
     * 達成済みマイルストーンの数を返す。
     *
     * @return 達成数。
     */
    u32 AchievedCount()  const noexcept;

    /**
     * 指定 id のマイルストーン状態を返す。
     *
     * @details 返却ポインタは次の RegisterMilestone() / ResetProgress() で無効化される。
     * @param id 照会する milestone の id。
     * @return 状態へのポインタ (見つからなければ nullptr)。
     */
    const FMilestoneState* GetState(const char* id) const noexcept;

    /**
     * 全マイルストーン状態の連続バッファを返す。
     *
     * @details 返却ポインタは MilestoneCount() 件の連続バッファ、RegisterMilestone()
     * / ResetProgress() で無効化される。
     * @param out_count バッファの件数を書き出す先。
     * @return 状態配列の先頭ポインタ。
     */
    const FMilestoneState* AllStates(u32& out_count) const noexcept;

    /**
     * 累計 XP を 0 に、全 milestone を未達成に戻す (定義は保持)。
     *
     * @details 出荷ビルドでは UI から呼ばないこと (デバッグメニュー / NewGame+ 想定)。
     */
    void ResetProgress() noexcept;

    /**
     * 達成時 callback を登録 / 差し替え / 解除する。
     *
     * @details 設定された callback は AwardXp 内で「未達成 → 達成」遷移を起こした
     * milestone ごとに 1 回呼ばれる。
     * @param cb 登録する callback (nullptr で解除)。
     * @param user callback に渡す user data。
     */
    void SetOnAchievedCallback(MilestoneCallback cb, void* user) noexcept;

    /**
     * 累計 XP と達成済み milestone を保存する。
     *
     * @details
     * CSaveArchive バイナリ (CRC32、256 MiB上限、atomic replace) で書き出す。
     * milestone は id の FNV-1a hash をキーにするので登録順に非依存。件数・サイズの
     * overflow と一時buffer確保をchecked処理し、失敗時は既存保存ファイルを保持する。
     * @param file_path 書き出し先のファイルパス。
     * @return 成功なら空の TResult、IO 失敗ならエラー。
     */
    TResult<void> Save(const wchar_t* file_path) noexcept;

    /**
     * 保存した進捗を復元する。
     *
     * @details
     * 同じ FMilestoneDef 群を RegisterMilestone した後に呼ぶと、XP と各 milestone の
     * 達成状態を復元する。全payloadを検証・stagingしてから一括反映するため、version/CRC/
     * schema/OOMのどの失敗でも現在の進捗は変更しない。未登録 hash は無視する。
     * @param file_path 読み込むファイルパス。
     * @return 成功なら空の TResult、IO / 検証失敗ならエラー。
     */
    TResult<void> Load(const wchar_t* file_path) noexcept;

private:
    /**
     * id 文字列を m_Defs から線形検索する。
     *
     * @param id 検索する milestone の id。
     * @return 見つかった index、未検出 / id == nullptr なら -1。
     */
    isize FindIndex(const char* id) const noexcept;

    /** 累計 XP (AwardXp で加算、ResetProgress で 0)。 */
    u32 m_Xp = 0;

    /** マイルストーン定義 (immutable、FMilestoneState と 1:1 対応)。 */
    TArray<FMilestoneDef>   m_Defs;

    /** マイルストーン実行時状態 (Def と同 index で 1:1 対応)。 */
    TArray<FMilestoneState> m_States;

    /** 達成時 callback (nullptr で未設定)。 */
    MilestoneCallback m_OnAchieved      = nullptr;

    /** 達成時 callback に渡す user data。 */
    void*             m_OnAchievedUser = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FProgression = CProgression;

} // namespace acs::game
