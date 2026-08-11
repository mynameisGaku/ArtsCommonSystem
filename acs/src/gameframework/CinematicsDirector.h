// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Forward.h"
#include "math/Vec.h"

namespace acs::game {

class CCinematicPlayer;
namespace cinetimeline {
class CCinematicTimelineDocument;
}

/**
 * タイムライン上の各 keyframe が何を起こすかを表す種別。
 *
 * @details kind ごとに FTimelineKeyframe::payload の異なるフィールドが意味を持つ。
 */
enum class ETimelineTrackKind : u8 {
    /** 何もしない (時間進行マーカー)。payload は触らない。 */
    Wait         = 0,

    /** カメラ移動を要求 (target_pos / zoom / duration を caller に渡す)。 */
    MoveCamera   = 1,

    /** ダイアログ行表示を要求 (line_id を caller に渡す)。 */
    ShowDialogue = 2,

    /** BGM 切替を要求 (music_id / fade を caller に渡す)。 */
    PlayMusic    = 3,

    /** 汎用イベント発火を要求 (event_id を caller に渡す、フラグ立て等)。 */
    FireEvent    = 4,
};

/**
 * タイムライン上の 1 つの keyframe (発火時刻 + kind + kind 別 payload)。
 *
 * @details payload は active な kind に対応するフィールドのみが意味を持つ C union。
 */
struct FTimelineKeyframe {
    /** タイムライン上の発火時刻 [秒]。 */
    f32               time_sec = 0.0f;

    /** この keyframe が起こす種別 (どの payload を使うかを決める)。 */
    ETimelineTrackKind kind     = ETimelineTrackKind::Wait;

    /** kind 別 payload (active な kind のフィールドのみ有効)。 */
    union FPayload {
        /** MoveCamera 用 payload。 */
        struct {
            /** カメラを向けたい world 座標。 */
            FVec2 target_pos;

            /** 目標 zoom 倍率 (1.0 = 等倍)。 */
            f32  zoom;

            /** カメラ移動にかける秒数 (caller が CTweenManager 等で消化)。 */
            f32  duration;
        } camera;

        /** ShowDialogue 用 payload。 */
        struct {
            /** ダイアログ行 ID (literal / バンドル参照、所有しない)。 */
            const char* line_id;
        } dialogue;

        /** PlayMusic 用 payload。 */
        struct {
            /** BGM トラック ID (literal / バンドル参照、所有しない)。 */
            const char* music_id;

            /** フェード秒数 (caller が CAudioDirector に渡す)。 */
            f32         fade;
        } music;

        /** FireEvent 用 payload。 */
        struct {
            /** 汎用イベント ID (caller 側で hash や enum cast 想定)。 */
            u32 event_id;
        } event;

        /** デフォルトは event を 0 で初期化する。 */
        FPayload() noexcept : event{0} {}
    } payload;

    /** Wait kind / 全 payload 0 の keyframe を構築する。 */
    FTimelineKeyframe() noexcept = default;
};

/**
 * MoveCamera keyframe の発火 callback の型。
 *
 * @param user SetCameraCallback で渡したコンテキスト (this 想定)。
 * @param target_pos カメラを向けたい world 座標。
 * @param zoom 目標 zoom 倍率。
 * @param duration カメラ移動にかける秒数。
 */
using CameraCallbackFn   = void(*)(void* user, FVec2 target_pos, f32 zoom, f32 duration) noexcept;

/**
 * ShowDialogue keyframe の発火 callback の型。
 *
 * @param user SetDialogueCallback で渡したコンテキスト (this 想定)。
 * @param line_id 表示するダイアログ行 ID。
 */
using DialogueCallbackFn = void(*)(void* user, const char* line_id) noexcept;

/**
 * PlayMusic keyframe の発火 callback の型。
 *
 * @param user SetMusicCallback で渡したコンテキスト (this 想定)。
 * @param music_id 切り替える BGM トラック ID。
 * @param fade フェード秒数。
 */
using MusicCallbackFn    = void(*)(void* user, const char* music_id, f32 fade) noexcept;

/**
 * FireEvent keyframe の発火 callback の型。
 *
 * @param user SetEventCallback で渡したコンテキスト (this 想定)。
 * @param event_id 発火する汎用イベント ID。
 */
using EventCallbackFn    = void(*)(void* user, u32 event_id) noexcept;

/**
 * タイムライン上の keyframe を時間順に発火していく cutscene driver。
 *
 * @details
 * ストーリーシーン / オープニング / ボス導入演出等で「○秒でカメラ移動」「△秒で
 * セリフ」「□秒で BGM フェードイン」を宣言的に組み立てる薄い state holder。自身は
 * カメラ / 音 / 描画を直接いじらず、kind 別の関数ポインタ callback 経由で発火する
 * (副作用ゼロ / type-erase)。AddKeyframe は内部で time_sec 昇順 (stable) を維持し、
 * Tick で経過時刻に達した keyframe を 1 度だけ発火する。Skip は残り全部を一気に
 * 発火する。state の唯一性のため非コピー・非ムーブ。
 */
class CCinematicsDirector {
public:
    /** 空のタイムラインで構築する (停止状態、keyframe なし)。 */
    CCinematicsDirector() noexcept = default;

    /** 破棄する (非所有データのみ保持のため特別な後始末なし)。 */
    ~CCinematicsDirector() noexcept = default;

    /** コピー禁止 (state の唯一性を機械的に担保)。 */
    CCinematicsDirector(const CCinematicsDirector&)            = delete;

    /** コピー代入も禁止。 */
    CCinematicsDirector& operator=(const CCinematicsDirector&) = delete;

    /** ムーブ禁止。 */
    CCinematicsDirector(CCinematicsDirector&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CCinematicsDirector& operator=(CCinematicsDirector&&)      = delete;

    /**
     * 有効な keyframe を追加し、成功可否を返す。
     *
     * @details 非有限時刻、再生中、または進行後は追加せず false を返す。
     * 負の有限時刻は 0 に丸め、同時刻は登録順を保つ。確保失敗時も配列を変えない。
     * @param kf 追加する keyframe (内部にコピーされる)。
     * @return 追加できた場合 true、入力または状態が不正か確保に失敗した場合 false。
     */
    bool TryAddKeyframe(const FTimelineKeyframe& kf) noexcept;

    /**
     * keyframe を追加する。
     *
     * @details TryAddKeyframe の結果を返さず、追加できない入力や状態を無視する。
     * @param kf 追加する keyframe (内部にコピーされる)。
     */
    void AddKeyframe(const FTimelineKeyframe& kf) noexcept;

    /**
     * 全 keyframe と再生状態を破棄する。
     *
     * @details AScene::OnExit 等で使う。
     */
    void Clear() noexcept;

    /**
     * 再生を開始 / 再開する。
     *
     * @details
     * Stop 後なら m_Time=0 から、Pause 後なら現在時刻から Resume する (m_Playing=true)。
     * keyframe が空でも再生中にはなる (実害なし)。
     */
    void Play() noexcept;

    /** 一時停止する (m_Time / 発火位置は保持)。 */
    void Pause() noexcept;

    /** 完全停止する (再生フラグ off、時刻と発火位置を 0 にリセット)。 */
    void Stop() noexcept;

    /**
     * 残り全 keyframe を即座に時刻昇順で発火し、時刻を末尾に進める。
     *
     * @details 終了済みシネマティクスを「ボタン押下でスキップ」する時の正攻法。
     */
    void Skip() noexcept;

    /**
     * 再生中かを返す。
     *
     * @return 再生中なら true。
     */
    bool IsPlaying()  const noexcept { return m_Playing; }

    /**
     * 全 keyframe を発火し終わったかを返す。
     *
     * @return 発火済み個数が keyframe 数以上なら true。
     */
    bool IsFinished() const noexcept;

    /**
     * タイムラインを dt 秒進める。
     *
     * @details
     * m_Playing == false / dt <= 0 は no-op。時刻を進め、達した未発火 keyframe を
     * 時刻昇順に発火する。
     * @param dt 進める秒数。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 現在のタイムライン時刻を返す。
     *
     * @return 現在時刻 [秒]。
     */
    f32 CurrentTime()    const noexcept { return m_Time; }

    /**
     * タイムライン全体の長さを返す。
     *
     * @return 最後の keyframe の time_sec (keyframe が無ければ 0)。
     */
    f32 TotalDuration()  const noexcept;

    /**
     * 登録済 keyframe 数を返す。
     *
     * @return keyframe の総数。
     */
    u32 KeyframeCount()  const noexcept { return static_cast<u32>(m_Keyframes.Num()); }

    /**
     * MoveCamera 発火 callback を設定する。
     *
     * @details cb == nullptr で未登録にできる (該当 kind の発火は no-op、警告なし)。
     * @param cb 設定する callback (nullptr で解除)。
     * @param user callback に渡すコンテキスト。
     */
    void SetCameraCallback  (CameraCallbackFn   cb, void* user) noexcept;

    /**
     * ShowDialogue 発火 callback を設定する。
     *
     * @details cb == nullptr で未登録にできる (該当 kind の発火は no-op)。
     * @param cb 設定する callback (nullptr で解除)。
     * @param user callback に渡すコンテキスト。
     */
    void SetDialogueCallback(DialogueCallbackFn cb, void* user) noexcept;

    /**
     * PlayMusic 発火 callback を設定する。
     *
     * @details cb == nullptr で未登録にできる (該当 kind の発火は no-op)。
     * @param cb 設定する callback (nullptr で解除)。
     * @param user callback に渡すコンテキスト。
     */
    void SetMusicCallback   (MusicCallbackFn    cb, void* user) noexcept;

    /**
     * FireEvent 発火 callback を設定する。
     *
     * @details cb == nullptr で未登録にできる (該当 kind の発火は no-op)。
     * @param cb 設定する callback (nullptr で解除)。
     * @param user callback に渡すコンテキスト。
     */
    void SetEventCallback   (EventCallbackFn    cb, void* user) noexcept;

private:
    friend class CCinematicPlayer;
    friend class cinetimeline::CCinematicTimelineDocument;

    // 同じアロケータで構築した列を受け取り、再生状態と時刻を初期化して置き換えます。
    void TryResetKeyframes(TArray<FTimelineKeyframe>&& keyframes) noexcept;

    /** CCinematicPlayerが構築した時刻順の列へ置き換えます。文字列参照は同Playerが保持するACinematicAssetの所有期間中だけ有効です。再生中、時刻または発火位置の進行後、時刻が非有限値・負値・降順ではfalseを返し、既存列を変更しません。 */
    bool TryReplaceKeyframes(TArray<FTimelineKeyframe>&& keyframes) noexcept;

    /**
     * 発火位置以降で時刻条件を満たす keyframe を全て発火し、発火位置を進める。
     *
     * @details Tick と Skip の共通処理。
     * @param up_to_time この時刻以下 (time_sec <= up_to_time) の keyframe を発火する。
     */
    void FireUpTo(f32 up_to_time) noexcept;

    /**
     * 1 つの keyframe を kind に応じた callback で発火する。
     *
     * @details Wait は no-op。該当 callback が未登録なら何もしない。
     * @param kf 発火する keyframe。
     */
    void FireOne(const FTimelineKeyframe& kf) noexcept;

    /** 全 keyframe (time_sec 昇順、stable sort 維持)。 */
    TArray<FTimelineKeyframe> m_Keyframes;

    /** 現在のタイムライン時刻 [秒] (Play 開始時に 0、Resume 時は維持)。 */
    f32 m_Time = 0.0f;

    /** 次に発火する keyframe の index (= 既に発火済みの個数)。 */
    u32 m_LastFiredIndex = 0u;

    /** 再生中フラグ (Play で true、Pause / Stop で false)。 */
    bool m_Playing = false;

    /** MoveCamera 発火 callback (nullptr で未登録)。 */
    CameraCallbackFn   m_CameraCb   = nullptr;

    /** Camera callback に渡すコンテキスト。 */
    void*              m_CameraUser = nullptr;

    /** ShowDialogue 発火 callback (nullptr で未登録)。 */
    DialogueCallbackFn m_DialogueCb   = nullptr;

    /** Dialogue callback に渡すコンテキスト。 */
    void*              m_DialogueUser = nullptr;

    /** PlayMusic 発火 callback (nullptr で未登録)。 */
    MusicCallbackFn    m_MusicCb    = nullptr;

    /** Music callback に渡すコンテキスト。 */
    void*              m_MusicUser  = nullptr;

    /** FireEvent 発火 callback (nullptr で未登録)。 */
    EventCallbackFn    m_EventCb    = nullptr;

    /** Event callback に渡すコンテキスト。 */
    void*              m_EventUser  = nullptr;
};

} // namespace acs::game
