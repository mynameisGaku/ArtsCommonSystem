// SPDX-License-Identifier: Apache-2.0
// Win32 ウィンドウ（イベント駆動、初学者向け簡易 API）
//
// 使い方:
//   FWindowConfig cfg;
//   cfg.title  = L"MyGame";
//   cfg.width  = 1280;
//   cfg.height = 720;
//   auto wr = FWindow::Create(cfg);
//   if (wr.IsErr()) return -1;
//   FWindow& w = wr.Value();
//
//   while (!w.ShouldClose()) {
//       w.PollEvents();
//       // ゲーム処理
//   }
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "platform/Event.h"

namespace acs {

/** ウィンドウ生成時のオプション (FWindow::Create に渡す)。 */
struct FWindowConfig {
    /** タイトルバーに表示する文字列。 */
    const wchar_t* title       = L"ACS Window";

    /** クライアント領域の初期幅 (px)。 */
    u32            width       = 1280;

    /** クライアント領域の初期高さ (px)。 */
    u32            height      = 720;

    /** ユーザーによるサイズ変更を許可するか (false で WS_THICKFRAME / 最大化を外す)。 */
    bool           resizable   = true;

    /** 全画面で開始するか (現状の Create では未使用、後段の SetFullscreen で切替)。 */
    bool           fullscreen  = false;

    /** 垂直同期を希望するか (Swapchain 側へのヒント。FWindow 自体は参照しない)。 */
    bool           vsync_hint  = true;

    /**
     * Show the top-level window after creation.
     *
     * Hidden windows are used by authenticated, bounded package startup
     * smoke tests. Skipping ShowWindow keeps the test from activating,
     * focusing, or covering an interactive editor session while preserving
     * a real HWND/swapchain startup path.
     */
    bool           visible     = true;
};

/**
 * イベントコールバックの関数ポインタ型。
 *
 * @details PollEvents の処理中に発生したイベントごとに同期的に呼ばれる。
 * @param user SetEventCallback で登録した任意のユーザーポインタ。
 * @param e 発生したイベント。
 */
using EventCallback = void (*)(void* user, const FEvent& e);

/**
 * Win32 ウィンドウ (イベント駆動、初学者向けの簡易 API)。
 *
 * @details
 * HWND を単独所有する non-copy・move-only 型。Create でウィンドウを開き、毎フレーム
 * PollEvents でメッセージキューを処理して登録済みコールバックへイベントを流す。
 * 内部の WindowProc が HWND の prop に this を紐付けるため、ムーブ時はその prop を
 * 張り替える。
 */
class FWindow {
public:
    /** 空のウィンドウを構築する (HWND なし。実体は Create で得る)。 */
    FWindow() noexcept = default;

    /** ウィンドウを破棄する (HWND の prop を外し DestroyWindow する)。 */
    ~FWindow() noexcept;

    /** コピー禁止 (HWND を単独所有するため)。 */
    FWindow(const FWindow&) = delete;

    /** コピー代入も禁止。 */
    FWindow& operator=(const FWindow&) = delete;

    /**
     * ムーブ構築する (HWND の所有権と全状態を奪い、prop の this を張り替える)。
     *
     * @param o ムーブ元 (奪われて空状態になる)。
     */
    FWindow(FWindow&& o) noexcept;

    /**
     * ムーブ代入する (自分の HWND を破棄してから o の所有権と全状態を奪う)。
     *
     * @param o ムーブ元 (奪われて空状態になる)。
     * @return 自身への参照。
     */
    FWindow& operator=(FWindow&& o) noexcept;

    /**
     * ウィンドウを生成して表示する。
     *
     * @details
     * 初回呼び出しでウィンドウクラスを登録し、クライアント領域が cfg.width×cfg.height に
     * なるよう外接矩形を計算して CreateWindowExW する。生成後に ShowWindow まで行う。
     * @param cfg 生成オプション。
     * @return 成功した FWindow、クラス登録または CreateWindowExW 失敗時は OS エラー。
     */
    static TResult<FWindow> Create(const FWindowConfig& cfg) noexcept;

    /**
     * 溜まったメッセージを処理する (メインループで毎フレーム呼ぶ)。
     *
     * @details PeekMessage によるノンブロッキング処理で、各メッセージは WindowProc 経由で
     * イベントへ変換されコールバックへ届く。
     */
    void PollEvents() noexcept;

    /**
     * 閉じる要求が来ているかを返す。
     *
     * @return × ボタン押下や Close 呼び出しで閉じる要求が立っていれば true。
     */
    bool ShouldClose() const noexcept { return m_ShouldClose; }

    /** 閉じる要求を立てる (ゲームコード側からウィンドウを閉じたいとき)。 */
    void Close() noexcept { m_ShouldClose = true; }

    /**
     * クライアント領域の幅を返す。
     *
     * @return 現在の幅 (px)。
     */
    u32 Width()  const noexcept { return m_Width; }

    /**
     * クライアント領域の高さを返す。
     *
     * @return 現在の高さ (px)。
     */
    u32 Height() const noexcept { return m_Height; }

    /**
     * ネイティブの HWND を返す (Render モジュールが Swapchain 生成に使う)。
     *
     * @return HWND を void* で包んだもの (未生成なら nullptr)。
     */
    void* NativeHandle() const noexcept { return m_Hwnd; }

    /**
     * イベント通知先のコールバックを登録する。
     *
     * @param cb イベントごとに呼ばれる関数 (nullptr で無効化)。
     * @param user cb に毎回渡される任意のユーザーポインタ。
     */
    void SetEventCallback(EventCallback cb, void* user) noexcept;

    /**
     * タイトルバーの文字列を変更する (FPS 表示など)。
     *
     * @param title 新しいタイトル文字列。
     */
    void SetTitle(const wchar_t* title) noexcept;

    /**
     * ボーダーレス全画面の ON/OFF を切り替える。
     *
     * @details
     * ON にする際は現在の窓矩形・スタイルを保存し、最寄りモニタ全体を覆う WS_POPUP 窓へ
     * 変更する (GetMonitorInfoW 失敗時は状態を変えず中断)。OFF で保存値から復元する。
     * 現在値と同じ on を渡した場合や HWND 未生成の場合は何もしない。
     * @param on true で全画面化、false で元のウィンドウへ復元。
     */
    void SetFullscreen(bool on) noexcept;

    /**
     * 全画面状態かを返す。
     *
     * @return 全画面なら true。
     */
    bool IsFullscreen() const noexcept { return m_Fullscreen; }

    /**
     * 内部用: WindowProc から 1 件のイベントを発行する。
     *
     * @param e 登録済みコールバックへ渡すイベント。
     */
    void DispatchEvent_Internal(const FEvent& e) noexcept;

    /**
     * 内部用: WindowProc からのサイズ更新を反映する。
     *
     * @param w 新しいクライアント領域の幅 (px)。
     * @param h 新しいクライアント領域の高さ (px)。
     */
    void UpdateSize_Internal(u32 w, u32 h) noexcept { m_Width = w; m_Height = h; }

private:
    /** ネイティブウィンドウハンドル (HWND を void* で保持。未生成なら nullptr)。 */
    void*           m_Hwnd          = nullptr;

    /** クライアント領域の幅 (px)。 */
    u32             m_Width         = 0;

    /** クライアント領域の高さ (px)。 */
    u32             m_Height        = 0;

    /** 閉じる要求フラグ (ShouldClose が返す値)。 */
    bool            m_ShouldClose  = false;

    /** イベント通知先コールバック (未登録なら nullptr)。 */
    EventCallback   m_Callback      = nullptr;

    /** コールバックへ毎回渡すユーザーポインタ。 */
    void*           m_CallbackUser = nullptr;

    /** 全画面状態 (ムーブ時も保存矩形・スタイルごと引き継ぐ)。 */
    bool            m_Fullscreen    = false;

    /** 全画面化前の GWL_STYLE (復元用)。 */
    i32             m_SavedStyle   = 0;

    /** 全画面化前の窓矩形 (l,t,r,b の 4 要素、復元用)。 */
    i32             m_SavedRect[4] {};
};

} // namespace acs
