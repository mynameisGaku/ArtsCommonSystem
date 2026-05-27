// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — FHotReload (Phase 2 seam)
//
// 開発時のアセット hot reload を行う「シーム (seam)」。Watcher にディレクトリ /
// ファイルを登録 → 別 layer (Phase K-3 で実 FS API、Windows なら
// `ReadDirectoryChangesW` を Async 起動) が変更を検出 → コールバック群へ
// `HotReloadEvent` を配信、という構成。
//
// 本ヘッダ (Phase 2) では:
//   ・watched パスのリスト
//   ・コールバック登録の集中点
//   ・pending event の FIFO バッファ + 取り出し API
// だけを提供する。実 FS poll は `Tick(f32)` 内に TODO 化されており、Phase K-3 で
// プラットフォーム固有 watcher を接続する。それまでは event は外部から
// (テストや bridge コード) 直接 buffer に push する想定もできるが、本ヘッダ層では
// public な「外部 push」API は提供しない (実装は Phase K-3 で `INotify` 等の
// 内部 helper として追加する)。
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
//           m_Watcher.Tick(dt);  // Phase K-3 で内部 poll → callback dispatch
//           // (Phase K-2 では Tick は何もしない — pending event の手動 drain は下の通り)
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
//     保つため swap-remove ではなく shift。Phase K-3 までは event 数は <= 数十を想定。
//   ・**ship build no-op**: `ACS_GAME_SHIPPING` 定義時はヘッダ側で本体 class を
//     「空シェル」に置き換える。呼び出し側コードを `#ifdef` だらけにしないため、
//     class / 関数シグネチャ自体は残し、戻り値は安全な既定値を返す。
//
// 範囲外 (Phase K-3 以降で):
//   ・実 FS watcher: Windows `ReadDirectoryChangesW`、POSIX `inotify` / `kqueue`、
//     macOS `FSEvents`。共通 I/F は `IFileSystemWatcher` として `platform/` 配下に
//     置き、`HotReloadWatcher` がこれを 1 枚被せる形になる予定。
//   ・debounce (連続 save / save → swap-rename のような 100ms 内のイベント集約)
//   ・recursive 解除 (現状は WatchDirectory の recursive=true/false を「希望」として
//     記録するのみ。実 watcher 接続時に意味付与)
//   ・asset → file path の逆マッピング (どの asset id が落ちたかの問い合わせ)
//   ・hot reload 中の整合性ロック (描画スレッドが reload 対象を読んでいる最中の
//     差し替えは未定義、Phase K-3 で job system と協調)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// ---- イベント -----------------------------------------------------------
// 1 件の hot reload イベント。FS watcher → callback への引数。
// `file_path` の所有権は watcher 内部 (watched paths 配列のエントリを参照する)
// もしくは Phase K-3 の FS watcher が払い出す bufer に寄る。いずれにせよ
// callback 呼び出しスコープ中のみ valid。callback が長く保持したい場合は
// 内部でコピーする責務がある (本ヘッダ層は <string> 禁止のため複製しない)。
struct HotReloadEvent {
    const char* file_path         = nullptr;  // null 終端 path (caller / watcher 所有)
    u64         modified_timestamp = 0;       // 変更検出時刻 (Phase K-3 で OS 由来値を入れる)
    bool        removed            = false;   // true: ファイル削除イベント、false: 更新
};

// コールバック型。ACS 規約により全 noexcept、関数ポインタのみ採用。
//   - user : Register 時に渡したコンテキストポインタ (this 想定)
//   - ev   : このイベントの詳細。呼び出しスコープ中のみ valid。
using HotReloadCallback = void(*)(void* user, const HotReloadEvent& ev) noexcept;

// ---- Watcher ------------------------------------------------------------
// 開発時のファイル変更を監視し、登録済みコールバック群に dispatch するためのハブ。
// Phase 2 では「登録 / 列挙 / event FIFO」だけを実装し、実 FS poll は Phase K-3 へ。
class HotReloadWatcher {
public:
    HotReloadWatcher() noexcept = default;
    ~HotReloadWatcher() noexcept = default;

    // 非コピー・非ムーブ: 内部 TArray 3 本の所有を曖昧にしない。
    HotReloadWatcher(const HotReloadWatcher&)            = delete;
    HotReloadWatcher& operator=(const HotReloadWatcher&) = delete;
    HotReloadWatcher(HotReloadWatcher&&)                 = delete;
    HotReloadWatcher& operator=(HotReloadWatcher&&)      = delete;

    // ----- ライフサイクル -----
    // 初期化。現状は何もしない (将来 Phase K-3 で OS watcher ハンドルを開く予定の
    // 予約点)。多重呼び出し可。
    void Init() noexcept;

    // 後始末。watched paths / callbacks / pending events を全クリア。
    // Phase K-3 では OS watcher ハンドルも閉じる予定。多重呼び出し可。
    void Shutdown() noexcept;

    // ----- 監視対象登録 -----
    // ディレクトリを監視対象に追加。recursive=true ならサブディレクトリも含めて
    // 監視する希望を立てる (実 watcher 接続は Phase K-3)。
    // dir_path は caller 所有 (リテラル / 永続バッファ前提)、null は無視。
    // 重複登録は no-op (path 文字列の `strcmp` で一致判定)。
    void WatchDirectory(const char* dir_path, bool recursive = true) noexcept;

    // 単一ファイルを監視対象に追加。WatchDirectory との違いは「単一 path のみ」と
    // いう意図表明だけで、内部的には watched paths に積むだけ。
    // file_path は caller 所有、null は無視。重複登録は no-op。
    void WatchFile(const char* file_path) noexcept;

    // 指定 path を監視対象から外す。完全一致 (`strcmp` == 0) のみ削除。
    // 未登録 / null は no-op。
    void Unwatch(const char* path) noexcept;

    // ----- コールバック登録 -----
    // (cb, user) のペアを登録。同一 (cb, user) の重複登録は no-op で弾く。
    // cb が null の場合は登録せず警告ログ。
    void RegisterCallback(HotReloadCallback cb, void* user) noexcept;

    // ----- 駆動 -----
    // 1 フレーム進める。Phase 2 スタブ: 何もしない。
    // Phase K-3 で OS watcher の poll → pending events への push →
    // 登録済み callback への dispatch を行う。
    void Tick(f32 dt) noexcept;

    // ----- 状態取得 -----
    // 現在監視中の path 数。
    u32 WatchedCount() const noexcept;

    // pending event バッファに溜まった未処理 event 数。
    u32 PendingEventCount() const noexcept;

    // pending event を 1 件取り出す (FIFO)。out に書き込んで true、空なら false。
    // 取り出した event は内部 buffer から物理削除される。
    bool ConsumeNextEvent(HotReloadEvent& out) noexcept;

    // pending event を全クリア (callback dispatch 済みかは問わない)。
    void ClearEvents() noexcept;

private:
#ifndef ACS_GAME_SHIPPING
    // コールバックエントリ。POD で trivially copyable。
    struct CallbackEntry {
        HotReloadCallback cb   = nullptr;
        void*             user = nullptr;
    };

    TArray<const char*>      m_WatchedPaths;    // caller 所有の path を借用保持
    TArray<CallbackEntry>    m_Callbacks;        // (cb, user) ペア集合
    TArray<HotReloadEvent>   m_PendingEvents;   // FIFO バッファ、Tick で push、Consume で pop
#endif
};

} // namespace acs::game
