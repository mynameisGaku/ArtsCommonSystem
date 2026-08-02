// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — CCinematicsDirector (timeline-based cutscene)
//
// タイムライン上に並べた keyframe を時間順に発火していく cutscene driver。
// ストーリーシーン / オープニング / ボス導入演出 / イベントムービー等で
// 「○秒で カメラ移動」「△秒でセリフ」「□秒で BGM フェードイン」を
// 1 つのタイムラインとして宣言的に組み立てるための薄い state holder。
//
// 設計選択 (Pillar R):
//   ・**CCinematicsDirector 自身は描画 / カメラ / 音 を直接いじらない**:
//      CDamageFeedback と同じ「副作用ゼロ / pull or callback」方針。発火は
//      関数ポインタ + void* user で type-erase した callback 経由で行い、
//      実際のカメラ移動 / ダイアログ起動 / BGM 切替は caller (AScene / UI 層 /
//      CAudioDirector) の責任。これで GameFramework から CRenderer / CCamera /
//      Audio / Dialogue への直接依存を切る。
//   ・**FTimelineKeyframe は POD union**: STL の variant は使えないので、
//      payload を C 風 union で持つ。各 track kind が必要なフィールドだけを
//      触る。Wait は payload を触らない (時間進行のみ)。
//   ・**time 順に内部で sort**: AddKeyframe は時系列で挿入順を強制しない —
//      シナリオ作成側が宣言順に書きやすいよう、内部で stable な挿入 sort を
//      維持する (要素数 N は典型的に < 100 なので O(N) で十分)。
//   ・**発火は一度のみ**: m_LastFiredIndex で「次に発火する keyframe」を
//      追跡し、time >= kf.time_sec になった時点で 1 度だけ callback を呼ぶ。
//      Skip() は残り全部を一気に発火 + m_Time を末尾に進める。
//   ・**callback は kind ごとに型を分ける**: 全 kind を 1 つの汎用 callback
//      で受けると caller 側が分岐 + cast を書くことになり typo の温床。
//      kind 別に signature を分けて「カメラ用」「ダイアログ用」「音楽用」
//      「汎用イベント用」の 4 種を提供する。
//   ・**Wait は時間進行のみ**: payload も callback も無い。タイムライン上の
//      「次の発火まで間を空ける」マーカーとして機能する (= 視覚的に gap を
//      表現するための便宜キーフレーム)。任意順で呼び出される普通の keyframe
//      は単に time_sec が大きいので発火が遅れるだけだが、Wait は明示的な
//      「ここは何もしない」を表現することで debug 時の意図伝達を助ける。
//   ・**TotalDuration() は最大 time_sec**: 何秒で終わるかを caller が知るため
//      の純粋なヘルパ。発火順とは独立に max を取るだけなので O(N)。
//   ・**Pause / Resume は m_Playing フラグのみ**: Tick は m_Playing == false で
//      no-op。m_Time は保存。Stop は m_Playing=false + m_Time=0 + m_LastFired=0。
//   ・**Skip 中の callback 発火**: Skip は「残り全 keyframe を即座に発火」を
//      意味するので、Skip 内で callback を呼ぶ。発火順は time 昇順を維持。
//   ・**非コピー・非ムーブ**: state の唯一性 (現在 m_Time / m_LastFiredIndex)
//      を担保するため機械的に禁止。AScene にメンバとして 1 個埋め込む想定。
//
// 使い方:
//   class AOpeningScene : public AScene {
//       CCinematicsDirector m_Cine;
//       void OnEnter() noexcept override {
//           FTimelineKeyframe kf;
//           kf.time_sec = 0.0f;
//           kf.kind     = ETimelineTrackKind::PlayMusic;
//           kf.payload.music = {"opening_theme", 1.5f};
//           m_Cine.AddKeyframe(kf);
//
//           kf.time_sec = 2.0f;
//           kf.kind     = ETimelineTrackKind::MoveCamera;
//           kf.payload.camera = {FVec2{100, 200}, 1.5f, 3.0f};
//           m_Cine.AddKeyframe(kf);
//
//           kf.time_sec = 3.0f;
//           kf.kind     = ETimelineTrackKind::ShowDialogue;
//           kf.payload.dialogue = {"line_intro_001"};
//           m_Cine.AddKeyframe(kf);
//
//           m_Cine.SetCameraCallback(&AOpeningScene::DoMoveCamera, this);
//           m_Cine.SetDialogueCallback(&AOpeningScene::DoShowDialogue, this);
//           m_Cine.SetMusicCallback(&AOpeningScene::DoPlayMusic, this);
//           m_Cine.Play();
//       }
//       void OnUpdate(f32 dt) noexcept override { m_Cine.Tick(dt); }
//   };
//
// 範囲外:
//   ・並列タイムライン (現状は単一タイムラインのみ、複数を別 Director で運用)
//   ・keyframe の補間 / カーブ (現状は単発発火、補間は callback 側で CTweenManager に任せる)
//   ・タイムラインの巻き戻し / scrub (オーサリング用、ランタイムには不要)
//   ・条件分岐タイムライン (CDialogueSystem の choices で代用)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Forward.h"
#include "math/Vec.h"

namespace acs::game {

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
     * keyframe を追加する。
     *
     * @details
     * 内部で time_sec 昇順 (同時刻は登録順の stable) を維持するよう挿入位置を決める。
     * time_sec < 0 は 0 に clamp して受け入れる。
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
    u32 KeyframeCount()  const noexcept { return static_cast<u32>(m_Keyframes.Size()); }

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
