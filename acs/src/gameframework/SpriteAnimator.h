// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Math.h"

namespace acs::game {

/**
 * スプライトアニメの再生モード。
 *
 * @details N 枚の frame に対する index 進行の仕方を決める。
 */
enum class EPlayMode : u8 {
    /** 0→N-1→0→N-1→… と循環する。 */
    Loop     = 0,

    /** 0→N-1→0→… と端で折り返す (両端は 1 度ずつ)。 */
    PingPong = 1,

    /** 0→N-1 で停止し IsFinished() = true になる。 */
    Once     = 2,
};

/**
 * 時間から現在 frame index を算出するアニメ時間管理クラス。
 *
 * @details
 * 画像 asset / 描画 API には触れず、利用者が CurrentFrame() を取り出して自分の
 * Sprite/Quad に適用する責務分離型。floor(m_Elapsed * m_Fps) で frame を求め、
 * Loop/PingPong/Once で index の進行を変える。m_Elapsed は周期長で wrap (Loop/PingPong)
 * または末尾でクランプ (Once) し、長時間プレイでの f32 精度ロスを防ぐ。frame event は
 * std::function を使わず関数ポインタ + user 引数で実装する。
 */
class CSpriteAnimator {
public:
    /** frame に進入した瞬間に呼ぶコールバックの型 (capture 無し関数ポインタ + user)。 */
    using FrameEventFn = void(*)(void* user) noexcept;

    /** 空状態で構築する (frame=1, fps=1, 停止)。 */
    CSpriteAnimator() noexcept = default;

    /** デストラクタ (特別な後始末なし)。 */
    ~CSpriteAnimator() noexcept = default;

    /** コピー禁止 (アニメ状態を不意に複製しないため)。 */
    CSpriteAnimator(const CSpriteAnimator&)            = delete;

    /** コピー代入も禁止。 */
    CSpriteAnimator& operator=(const CSpriteAnimator&) = delete;

    /** ムーブ禁止。 */
    CSpriteAnimator(CSpriteAnimator&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CSpriteAnimator& operator=(CSpriteAnimator&&)      = delete;

    /**
     * 再生パラメータを初期化する。
     *
     * @details
     * frame_count==0 は 1、fps<=0 は 1.0 へフォールバックする。経過時間・現在 frame・
     * 再生/終了フラグはリセットするが、登録済みの frame event は維持される。
     * @param frame_count 総 frame 数 (0 なら 1 に補正)。
     * @param fps 毎秒の frame 切替数 (0 以下なら 1.0 に補正)。
     * @param mode 再生モード (既定 Loop)。
     */
    void Init(u32 frame_count, f32 fps, EPlayMode mode = EPlayMode::Loop) noexcept;

    /**
     * 再生を開始する。
     *
     * @details Once が末尾で終了済みのときは先頭へ巻き戻してから再生する (「もう一回」挙動)。
     */
    void Play()  noexcept;

    /** 位置を維持して一時停止する。 */
    void Pause() noexcept;

    /** 先頭に戻して停止する (経過時間・終了フラグもリセット)。 */
    void Stop()  noexcept;

    /**
     * 再生中かを返す。
     *
     * @return 再生中なら true。
     */
    bool IsPlaying() const noexcept { return m_Playing; }

    /**
     * 経過時間を進めて frame を更新する。
     *
     * @details
     * dt<=0 / fps<=0 / frame_count==0 は no-op。m_Elapsed を周期内に wrap (Loop/PingPong)
     * または末尾でクランプ (Once、到達時に自動停止) してから frame を再計算し、
     * 跨いだ frame の event を発火する。
     * @param dt 経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 現在の frame index を返す。
     *
     * @return 現在の frame index。
     */
    u32  CurrentFrame()    const noexcept { return m_CurrentFrame; }

    /**
     * Once 再生が末尾で終了したかを返す。
     *
     * @return 終了済みなら true。
     */
    bool IsFinished()      const noexcept { return m_Finished; }

    /**
     * [0, 1] のフレーム進行率を返す。
     *
     * @details Loop/PingPong は周期内の進行率、Once は 0→1 の単調進行。
     * @return 進行率 (0 以上 1 以下)。
     */
    f32  NormalizedTime()  const noexcept;

    /**
     * 指定 frame へ強制シークする。
     *
     * @details
     * frame_count を超える値は末尾にクランプする。m_Elapsed は新 frame の先頭時刻に
     * 合わせて再計算され、終了フラグは解除される (Once の末尾シークでも Tick まで未確定)。
     * @param i シーク先 frame index。
     */
    void SetCurrentFrame(u32 i) noexcept;

    /**
     * fps を差し替える。
     *
     * @details
     * 負値は 0 にクランプ (0 fps はフリーズ相当)。m_Elapsed は維持されるが新 fps で
     * 再評価されるため、見かけ上の CurrentFrame は次 Tick から変わる。
     * @param fps 新しい毎秒 frame 切替数。
     */
    void SetFps(f32 fps) noexcept;

    /**
     * 現在の fps を返す。
     *
     * @return 毎秒の frame 切替数。
     */
    f32      Fps()        const noexcept { return m_Fps; }

    /**
     * 総 frame 数を返す。
     *
     * @return frame 数。
     */
    u32      FrameCount() const noexcept { return m_FrameCount; }

    /**
     * 現在の再生モードを返す。
     *
     * @return EPlayMode。
     */
    EPlayMode Mode()       const noexcept { return m_Mode; }

    /**
     * 指定 frame に進入した瞬間 1 度だけ呼ぶコールバックを登録する。
     *
     * @details
     * 同じ frame に複数登録すると登録順に発火し、重複は許容する。cb==nullptr または
     * frame が frame_count 以上の登録は無視する (debug ビルドでも crash させない)。
     * @param frame 発火させる frame index。
     * @param cb 呼び出すコールバック (関数ポインタ)。
     * @param user cb に渡す user データ。
     */
    void AddFrameEvent(u32 frame, FrameEventFn cb, void* user) noexcept;

private:
    /**
     * 1 件の frame event 登録 (発火対象 frame とコールバック)。
     */
    struct FFrameEvent {
        /** 発火対象の frame index。 */
        u32          frame = 0;

        /** 進入時に呼ぶコールバック。 */
        FrameEventFn cb    = nullptr;

        /** コールバックに渡す user データ。 */
        void*        user  = nullptr;
    };

    /**
     * m_Elapsed から周期を考慮した論理 frame index を計算する。
     *
     * @details Loop は mod、PingPong は折り返し、Once は末尾クランプ。
     * @param elapsed 評価する経過秒。
     * @return [0, frame_count) に収まる frame index。
     */
    u32 ComputeFrame(f32 elapsed) const noexcept;

    /**
     * prev → next の frame 変化に対応する frame event を発火する。
     *
     * @details next に一致する登録イベントを発火する (Loop の周回・低 fps の wrap も考慮)。
     * @param prev 直前の frame index。
     * @param next 新しい frame index。
     * @param wrapped この Tick で周期を跨いだか。
     */
    void FireEventsBetween(u32 prev, u32 next, bool wrapped) noexcept;

    /** 総 frame 数。 */
    u32       m_FrameCount   = 1;

    /** 毎秒の frame 切替数。 */
    f32       m_Fps           = 1.0f;

    /** 再生モード。 */
    EPlayMode  m_Mode          = EPlayMode::Loop;

    /** 再生開始からの累積経過秒 (周期内に wrap/クランプ)。 */
    f32       m_Elapsed       = 0.0f;

    /** 現在の frame index。 */
    u32       m_CurrentFrame = 0;

    /** 再生中フラグ。 */
    bool      m_Playing       = false;

    /** Once 末尾到達の終了フラグ。 */
    bool      m_Finished      = false;

    /** 登録済みの frame event 一覧。 */
    TArray<FFrameEvent> m_Events;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FSpriteAnimator = CSpriteAnimator;

} // namespace acs::game
