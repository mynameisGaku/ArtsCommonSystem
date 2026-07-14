// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — FHotReload (seam)
//
// 開発時のアセット hot reload を行う「シーム (seam)」。Watcher にディレクトリ /
// ファイルを登録 → 別 layer (実 FS API、Windows なら `ReadDirectoryChangesW` を
// Async 起動) が変更を検出 → コールバック群へ `HotReloadEvent` を配信、という構成。
//
// 本ヘッダでは:
//   ・watched パスのリスト
//   ・コールバック登録の集中点
//   ・pending event の FIFO バッファ + 取り出し API
// だけを提供する。実 FS poll は `Tick(f32)` 内に TODO 化されており、プラット
// フォーム固有 watcher を接続する。本ヘッダ層では public な「外部 push」API は
// 提供しない (実装は `INotify` 等の内部 helper として追加する)。
//
// Ship build (`ACS_GAME_SHIPPING` 定義時) では:
//   ・全 public メソッドが no-op になる (ヘッダで `#ifdef` 分岐、cpp 側も同様)
//   ・内部 TArray は空のまま、event は 1 つも届かない
//   ・呼び出し側 (Editor / debug overlay 等) が `ACS_GAME_SHIPPING` で gate されて
//     いることを前提に、最小限のシンボルだけ残す
//   これは Pillar K の「dev tool は Ship build で完全に消える」方針を満たすため。
//
// 使い方:
//   class AssetSystem {
//       acs::game::HotReloadWatcher m_Watcher;
//       static void OnReload(void* user, const HotReloadEvent& ev) noexcept {
//           auto* self = static_cast<AssetSystem*>(user);
//           if (ev.removed) self->Drop(ev.file_path);
//           else            self->Reload(ev.file_path, ev.modified_timestamp);
//       }
//       void Boot() noexcept {
//           m_Watcher.Init();
//           m_Watcher.WatchDirectory("Assets/Textures", /*recursive=*/true);
//           m_Watcher.WatchFile("Assets/Config/FGame.toml");
//           m_Watcher.RegisterCallback(&OnReload, this);
//       }
//       void Update(f32 dt) noexcept {
//           m_Watcher.Tick(dt);  // 内部 poll → callback dispatch
//           // (現状 Tick は何もしない — pending event の手動 drain は下の通り)
//           HotReloadEvent ev;
//           while (m_Watcher.ConsumeNextEvent(ev)) { OnReload(this, ev); }
//       }
//       void Shutdown() noexcept { m_Watcher.Shutdown(); }
//   };
//
// 設計選択:
//   ・**STL 不使用**: `acs::TArray<...>` 3 本 (watched paths / callbacks / events) のみ。
//   ・**path 文字列は caller 所有 (借用)**: `const char*` を TArray に保持するだけ。
//     <string> 禁止。リテラル想定。動的文字列は呼び出し側が watcher 寿命中保持する。
//   ・**コールバックは関数ポインタ + void* user**: `std::function` は heap / RTTI /
//     例外を呼び込むため一切採用しない (FDevConsole / FSceneEventBus と同じ規約)。
//   ・**コールバックは重複登録不可**: (cb, user) ペアが完全一致なら no-op で弾く。
//     UI 側のライフサイクル誤りで二重登録 → 二重 dispatch を防ぐ。
//   ・**非コピー・非ムーブ**: 内部 TArray<const char*> / TArray<HotReloadEvent> の
//     所有権を曖昧にしないため (FDevConsole / FInspectorSeam と同じ方針)。
//   ・**全 noexcept**: ACS 規約。エラーは log のみ。
//   ・**event は POD**: `HotReloadEvent` は trivially copyable (path は借用 const char*)。
//     ConsumeNextEvent は TArray の先頭を out へコピー → 1 要素 shift-left。FIFO 順を
//     保つため swap-remove ではなく shift。event 数は <= 数十を想定。
//   ・**ship build no-op**: `ACS_GAME_SHIPPING` 定義時はヘッダ側で本体 class を
//     「空シェル」に置き換える。呼び出し側コードを `#ifdef` だらけにしないため、
//     class / 関数シグネチャ自体は残し、戻り値は安全な既定値を返す。
//
// 範囲外:
//   ・実 FS watcher: Windows `ReadDirectoryChangesW`、POSIX `inotify` / `kqueue`、
//     macOS `FSEvents`。共通 I/F は `IFileSystemWatcher` として `platform/` 配下に
//     置き、`HotReloadWatcher` がこれを 1 枚被せる形になる予定。
//   ・debounce (連続 save / save → swap-rename のような 100ms 内のイベント集約)
//   ・recursive 解除 (現状は WatchDirectory の recursive=true/false を「希望」として
//     記録するのみ。実 watcher 接続時に意味付与)
//   ・asset → file path の逆マッピング (どの asset id が落ちたかの問い合わせ)
//   ・hot reload 中の整合性ロック (描画スレッドが reload 対象を読んでいる最中の
//     差し替えは未定義、job system と協調する想定)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#ifndef ACS_GAME_SHIPPING
#include "container/String.h"     // FString — OS から払い出された path を UTF-8 で所有
#include "memory/UniquePtr.h"     // TUniquePtr<WatchEntry> — OVERLAPPED アドレス安定化
#endif

namespace acs::game {

/**
 * 1 件の hot reload イベント (FS watcher → callback への引数)。
 *
 * @details
 * trivially copyable な POD。file_path の所有権は watcher 内部 (watched paths
 * 配列のエントリ) もしくは FS watcher が払い出すバッファにあり、いずれにせよ
 * callback 呼び出しスコープ中のみ valid。callback が長く保持したい場合は内部で
 * コピーする責務がある (本ヘッダ層は FString による複製を行わない)。
 */
struct HotReloadEvent {
    /** null 終端の変更パス (caller / watcher 所有、呼び出しスコープ中のみ valid)。 */
    const char* file_path         = nullptr;

    /** 変更検出時刻 (OS 由来の値が入る)。 */
    u64         modified_timestamp = 0;

    /** true: ファイル削除イベント、false: 更新イベント。 */
    bool        removed            = false;
};

/**
 * hot reload コールバックの型。
 *
 * @details ACS 規約により全 noexcept、関数ポインタのみ採用 (FDevConsole 等と同規約)。
 * @param user Register 時に渡したコンテキストポインタ (this 想定)。
 * @param ev このイベントの詳細 (呼び出しスコープ中のみ valid)。
 */
using HotReloadCallback = void(*)(void* user, const HotReloadEvent& ev) noexcept;

/**
 * 監視ディレクトリ 1 件あたりの OS watcher 状態 (前方宣言)。
 *
 * @details
 * Windows では ReadDirectoryChangesW の HANDLE + OVERLAPPED + 受信バッファを持つ。
 * OVERLAPPED のアドレスは I/O 発行から完了まで安定している必要があるため、本体は
 * `.cpp` 側で定義し、ここでは前方宣言だけに留めて TUniquePtr で個別 heap 確保する
 * (TArray 再確保による移動を回避)。
 */
struct WatchEntry;

/**
 * 開発時のファイル変更を監視し、登録済みコールバック群へ dispatch するハブ。
 *
 * @details
 * watched パスのリスト・コールバック登録の集中点・pending event の FIFO バッファを
 * 提供する。実 FS poll は Tick(f32) 内でプラットフォーム固有 watcher を駆動する。
 * Ship build (ACS_GAME_SHIPPING 定義時) では全 public メソッドが no-op になり、
 * event は 1 つも届かない (dev tool を Ship build で完全に消す方針)。非コピー・
 * 非ムーブで内部 TArray 3 本の所有を曖昧にしない。
 */
class HotReloadWatcher {
public:
    /**
     * 空状態で構築する (OS watcher ハンドルは Init で開く)。
     *
     * @details WatchEntry の完全型が見える `.cpp` で定義し、コンストラクタの
     * 失敗後始末が不完全型の TUniquePtr 破棄を外部 TU で実体化しないようにする。
     */
    HotReloadWatcher() noexcept;

    /**
     * 破棄する (out-of-line)。
     *
     * @details
     * TUniquePtr<WatchEntry> の解放には完全型が要るが WatchEntry は `.cpp` でのみ
     * 完全になるため、デストラクタは out-of-line で定義する (ship build では空)。
     */
    ~HotReloadWatcher() noexcept;

    /** コピー禁止 (内部 TArray 3 本の所有を曖昧にしないため)。 */
    HotReloadWatcher(const HotReloadWatcher&)            = delete;

    /** コピー代入も禁止。 */
    HotReloadWatcher& operator=(const HotReloadWatcher&) = delete;

    /** ムーブ禁止。 */
    HotReloadWatcher(HotReloadWatcher&&)                 = delete;

    /** ムーブ代入も禁止。 */
    HotReloadWatcher& operator=(HotReloadWatcher&&)      = delete;

    /** 初期化する (OS watcher ハンドルを開く予約点。多重呼び出し可)。 */
    void Init() noexcept;

    /**
     * 後始末する (多重呼び出し可)。
     *
     * @details watched paths / callbacks / pending events を全クリアし、OS watcher
     * ハンドルも閉じる。
     */
    void Shutdown() noexcept;

    /**
     * ディレクトリを監視対象に追加する。
     *
     * @details
     * dir_path は caller 所有 (リテラル / 永続バッファ前提)、null は無視。重複登録は
     * 文字列一致判定で no-op になる。
     * @param dir_path 監視するディレクトリのパス (caller 所有)。
     * @param recursive true ならサブディレクトリも含めて監視する希望を立てる。
     */
    void WatchDirectory(const char* dir_path, bool recursive = true) noexcept;

    /**
     * 単一ファイルを監視対象に追加する。
     *
     * @details
     * WatchDirectory との違いは「単一 path のみ」という意図表明だけで、内部的には
     * watched paths に積む。file_path は caller 所有、null は無視、重複登録は no-op。
     * @param file_path 監視するファイルのパス (caller 所有)。
     */
    void WatchFile(const char* file_path) noexcept;

    /**
     * 指定 path を監視対象から外す。
     *
     * @details 文字列の完全一致のみ削除。未登録 / null は no-op。
     * @param path 監視解除するパス。
     */
    void Unwatch(const char* path) noexcept;

    /**
     * (cb, user) のペアをコールバックとして登録する。
     *
     * @details 同一 (cb, user) の重複登録は no-op で弾く。cb が null なら登録せず警告ログ。
     * @param cb 変更検出時に呼ばれるコールバック。
     * @param user cb に渡すコンテキストポインタ (所有しない)。
     */
    void RegisterCallback(HotReloadCallback cb, void* user) noexcept;

    /**
     * 1 フレーム進める。
     *
     * @details OS watcher の poll → pending events への push → 登録済み callback への
     * dispatch を行う。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 現在監視中の path 数を返す。
     *
     * @return 監視中の path 数。
     */
    u32 WatchedCount() const noexcept;

    /**
     * pending event バッファに溜まった未処理 event 数を返す。
     *
     * @return 未処理 event 数。
     */
    u32 PendingEventCount() const noexcept;

    /**
     * pending event を 1 件取り出す (FIFO)。
     *
     * @details 取り出した event は内部 buffer から物理削除される。
     * @param out 取り出した event の書き込み先。
     * @return event を取り出せたら true、空なら false。
     */
    bool ConsumeNextEvent(HotReloadEvent& out) noexcept;

    /** pending event を全クリアする (callback dispatch 済みかは問わない)。 */
    void ClearEvents() noexcept;

private:
#ifndef ACS_GAME_SHIPPING
    /**
     * pending event 先頭 1 件を物理削除する。
     *
     * @details m_PendingEvents / m_EventPaths を lockstep で先頭 shift-left する。
     * FIFO 順を保つため swap ではなく shift を使う。
     */
    void RemoveFrontEventPair() noexcept;

    /** コールバックエントリ (POD、trivially copyable)。 */
    struct CallbackEntry {
        /** 呼び出すコールバック関数ポインタ。 */
        HotReloadCallback cb   = nullptr;

        /** cb に渡すコンテキストポインタ。 */
        void*             user = nullptr;
    };

    /** caller 所有の監視 path を借用保持する配列。 */
    TArray<const char*>      m_WatchedPaths;

    /** 登録済み (cb, user) ペアの集合。 */
    TArray<CallbackEntry>    m_Callbacks;

    /** pending event の FIFO バッファ (Tick で push、Consume で pop)。 */
    TArray<HotReloadEvent>   m_PendingEvents;

    /**
     * pending event が指す path 文字列の実体 (OS 由来の WCHAR を UTF-8 化して所有)。
     *
     * @details
     * m_PendingEvents と常に lockstep: Tick で同時 push、Consume / dispatch で同時に
     * 先頭 shift、Clear で同時消し。file_path は cache せず Consume / dispatch 時に
     * m_EventPaths[0].Data() から解決する (TArray 再確保で SSO 文字列のアドレスが
     * 動いても dangling しないため)。
     */
    TArray<FString>          m_EventPaths;

    /**
     * OS watcher 状態 (WatchDirectory ごとに 1 entry)。
     *
     * @details Shutdown で HANDLE を閉じる。OVERLAPPED のアドレス安定化のため
     * TUniquePtr で個別 heap 確保する。
     */
    TArray<TUniquePtr<WatchEntry>> m_Watchers;
#endif
};

} // namespace acs::game
