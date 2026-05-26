// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R Phase 2 — FCinematicsDirector (timeline-based cutscene)
//
// タイムライン上に並べた keyframe を時間順に発火していく cutscene driver。
// ストーリーシーン / オープニング / ボス導入演出 / イベントムービー等で
// 「○秒で カメラ移動」「△秒でセリフ」「□秒で BGM フェードイン」を
// 1 つのタイムラインとして宣言的に組み立てるための薄い state holder。
//
// 設計選択 (Pillar R Phase 2):
//   ・**FCinematicsDirector 自身は描画 / カメラ / 音 を直接いじらない**:
//      FDamageFeedback と同じ「副作用ゼロ / pull or callback」方針。発火は
//      関数ポインタ + void* user で type-erase した callback 経由で行い、
//      実際のカメラ移動 / ダイアログ起動 / BGM 切替は caller (FScene / UI 層 /
//      FAudioDirector) の責任。これで GameFramework から FRenderer / FCamera /
//      Audio / Dialogue への直接依存を切る。
//   ・**FTimelineKeyframe は POD union**: STL の variant は使えないので、
//      payload を C 風 union で持つ。各 track kind が必要なフィールドだけを
//      触る。Wait は payload を触らない (時間進行のみ)。
//   ・**time 順に内部で sort**: AddKeyframe は時系列で挿入順を強制しない —
//      シナリオ作成側が宣言順に書きやすいよう、内部で stable な挿入 sort を
//      維持する (要素数 N は典型的に < 100 なので O(N) で十分)。
//   ・**発火は一度のみ**: _last_fired_index で「次に発火する keyframe」を
//      追跡し、time >= kf.time_sec になった時点で 1 度だけ callback を呼ぶ。
//      Skip() は残り全部を一気に発火 + _time を末尾に進める。
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
//   ・**Pause / Resume は _playing フラグのみ**: Tick は _playing == false で
//      no-op。_time は保存。Stop は _playing=false + _time=0 + _last_fired=0。
//   ・**Skip 中の callback 発火**: Skip は「残り全 keyframe を即座に発火」を
//      意味するので、Skip 内で callback を呼ぶ。発火順は time 昇順を維持。
//   ・**非コピー・非ムーブ**: state の唯一性 (現在 _time / _last_fired_index)
//      を担保するため機械的に禁止。FScene にメンバとして 1 個埋め込む想定。
//
// 使い方:
//   class OpeningScene : public FScene {
//       FCinematicsDirector _cine;
//       void OnEnter() noexcept override {
//           FTimelineKeyframe kf;
//           kf.time_sec = 0.0f;
//           kf.kind     = ETimelineTrackKind::PlayMusic;
//           kf.payload.music = {"opening_theme", 1.5f};
//           _cine.AddKeyframe(kf);
//
//           kf.time_sec = 2.0f;
//           kf.kind     = ETimelineTrackKind::MoveCamera;
//           kf.payload.camera = {FVec2{100, 200}, 1.5f, 3.0f};
//           _cine.AddKeyframe(kf);
//
//           kf.time_sec = 3.0f;
//           kf.kind     = ETimelineTrackKind::ShowDialogue;
//           kf.payload.dialogue = {"line_intro_001"};
//           _cine.AddKeyframe(kf);
//
//           _cine.SetCameraCallback(&OpeningScene::DoMoveCamera, this);
//           _cine.SetDialogueCallback(&OpeningScene::DoShowDialogue, this);
//           _cine.SetMusicCallback(&OpeningScene::DoPlayMusic, this);
//           _cine.Play();
//       }
//       void OnUpdate(f32 dt) noexcept override { _cine.Tick(dt); }
//   };
//
// 範囲外 (将来フェーズで):
//   ・並列タイムライン (現状は単一タイムラインのみ、複数を別 Director で運用)
//   ・keyframe の補間 / カーブ (現状は単発発火、補間は callback 側で Tween に任せる)
//   ・タイムラインの巻き戻し / scrub (オーサリング用、ランタイムには不要)
//   ・条件分岐タイムライン (FDialogueSystem の choices で代用)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs::game {

// タイムライン上の各 keyframe が何を起こすか。
//   Wait         : 何もしない (時間進行マーカー)
//   MoveCamera   : カメラ移動を要求 (target_pos / zoom / duration を caller に渡す)
//   ShowDialogue : ダイアログ行表示を要求 (line_id を caller に渡す)
//   PlayMusic    : BGM 切替を要求 (music_id / fade を caller に渡す)
//   FireEvent    : 汎用イベント発火 (event_id を caller に渡す、フラグ立て等)
enum class ETimelineTrackKind : u8 {
    Wait         = 0,
    MoveCamera   = 1,
    ShowDialogue = 2,
    PlayMusic    = 3,
    FireEvent    = 4,
};

// 1 つの keyframe。発火時刻 + kind + kind 別 payload (C union)。
// payload は active な kind に対応するフィールドのみが意味を持つ。
struct FTimelineKeyframe {
    f32               time_sec = 0.0f;                  // タイムライン上の発火時刻 [秒]
    ETimelineTrackKind kind     = ETimelineTrackKind::Wait;

    union Payload {
        struct {
            FVec2 target_pos;   // カメラを向けたい world 座標
            f32  zoom;         // 目標 zoom 倍率 (1.0 = 等倍)
            f32  duration;     // カメラ移動にかける秒数 (caller が Tween 等で消化)
        } camera;
        struct {
            const char* line_id; // ダイアログ行 ID (literal / バンドル参照、所有しない)
        } dialogue;
        struct {
            const char* music_id; // BGM トラック ID (literal / バンドル参照、所有しない)
            f32         fade;     // フェード秒数 (caller が FAudioDirector に渡す)
        } music;
        struct {
            u32 event_id;        // 汎用イベント ID (caller 側で hash や enum cast 想定)
        } event;

        Payload() noexcept : event{0} {}  // デフォルトは event を 0 で初期化
    } payload;

    FTimelineKeyframe() noexcept = default;
};

// 各 track 種別の発火 callback signature。
// すべて noexcept で user は SetXxxCallback 時に渡したコンテキスト (this 想定)。
using CameraCallbackFn   = void(*)(void* user, FVec2 target_pos, f32 zoom, f32 duration) noexcept;
using DialogueCallbackFn = void(*)(void* user, const char* line_id) noexcept;
using MusicCallbackFn    = void(*)(void* user, const char* music_id, f32 fade) noexcept;
using EventCallbackFn    = void(*)(void* user, u32 event_id) noexcept;

class FCinematicsDirector {
public:
    FCinematicsDirector() noexcept = default;
    ~FCinematicsDirector() noexcept = default;

    // 非コピー・非ムーブ (state の唯一性を機械的に担保)
    FCinematicsDirector(const FCinematicsDirector&)            = delete;
    FCinematicsDirector& operator=(const FCinematicsDirector&) = delete;
    FCinematicsDirector(FCinematicsDirector&&)                 = delete;
    FCinematicsDirector& operator=(FCinematicsDirector&&)      = delete;

    // ----- セットアップ -----
    // keyframe を追加。内部で time_sec 昇順 (安定) を維持するよう挿入位置を決める。
    // time_sec < 0 は 0 に clamp して受け入れる。
    void AddKeyframe(const FTimelineKeyframe& kf) noexcept;

    // 全 keyframe / 状態を破棄。FScene::OnExit 等で使う。
    void Clear() noexcept;

    // ----- 再生制御 -----
    // 先頭から再生開始 (Stop されていれば _time=0 から、Pause からは Resume)。
    // keyframe が空でも _playing=true にはなる (実害なし)。
    void Play() noexcept;

    // 一時停止 (_time / _last_fired_index は保持)。
    void Pause() noexcept;

    // 完全停止 (_playing=false, _time=0, _last_fired_index=0)。
    void Stop() noexcept;

    // 残り全 keyframe を即座に順番に発火し、_time を TotalDuration() に進める。
    // 終了したシネマティクスを「ボタン押下でスキップ」した時の正攻法。
    void Skip() noexcept;

    bool IsPlaying()  const noexcept { return _playing; }
    // 全 keyframe を発火し終わったか (= _last_fired_index == KeyframeCount())
    bool IsFinished() const noexcept;

    // ----- フレーム更新 -----
    // dt 秒進める。_playing == false / dt <= 0 は no-op。
    // _time += dt し、time_sec <= _time な未発火 keyframe を時刻昇順に発火する。
    void Tick(f32 dt) noexcept;

    // ----- accessors -----
    f32 CurrentTime()    const noexcept { return _time; }
    f32 TotalDuration()  const noexcept;
    u32 KeyframeCount()  const noexcept { return static_cast<u32>(_keyframes.Size()); }

    // ----- callback 登録 -----
    // 各 kind の発火コールバックを設定。cb == nullptr で「未登録」にできる
    // (= 該当 kind の発火は no-op になる、警告は出さない)。
    void SetCameraCallback  (CameraCallbackFn   cb, void* user) noexcept;
    void SetDialogueCallback(DialogueCallbackFn cb, void* user) noexcept;
    void SetMusicCallback   (MusicCallbackFn    cb, void* user) noexcept;
    void SetEventCallback   (EventCallbackFn    cb, void* user) noexcept;

private:
    // _last_fired_index 以降で time_sec <= _time な keyframe を全て発火し、
    // _last_fired_index を進める。Tick と Skip の共通処理。
    void FireUpTo(f32 up_to_time) noexcept;

    // kf を kind に応じた callback で発火 (Wait は no-op)。
    void FireOne(const FTimelineKeyframe& kf) noexcept;

    // 全 keyframe (time_sec 昇順、stable sort 維持)
    TArray<FTimelineKeyframe> _keyframes;

    // 現在のタイムライン時刻 [秒]、Play 開始時に 0 (Resume 時は維持)
    f32 _time = 0.0f;

    // 次に発火する keyframe の index (= 既に発火済みの個数)
    u32 _last_fired_index = 0u;

    // 再生中フラグ。Pause で false、Play で true、Stop で false。
    bool _playing = false;

    // ---- callback ----
    CameraCallbackFn   _camera_cb   = nullptr;
    void*              _camera_user = nullptr;
    DialogueCallbackFn _dialogue_cb   = nullptr;
    void*              _dialogue_user = nullptr;
    MusicCallbackFn    _music_cb    = nullptr;
    void*              _music_user  = nullptr;
    EventCallbackFn    _event_cb    = nullptr;
    void*              _event_user  = nullptr;
};

} // namespace acs::game
