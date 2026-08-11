// SPDX-License-Identifier: Apache-2.0
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

    /** 達成時の CClock::MillisSinceStartup() (起動からの ms、0 は未達成/未取得)。 */
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
