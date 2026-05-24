// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K (editor_core) — AssetBrowser (Phase 21a editor 共通基盤)
//
// プロジェクト `assets/` 配下のファイルツリーを ImGui で参照 + 各種 panel
// (ModelViewer / TilemapEditor / ParticleEditor 等) に Drag & Drop 経由で
// 「アセットパス」を供給する Unity の Project Window / Godot の FileSystem
// Dock 相当の基盤パネル。
//
// 役割:
//   ・assets/ ルートを起点にディレクトリを再帰列挙し、左ペインに tree、
//     右ペインに current directory のエントリ一覧を表示する。
//   ・各エントリは ImGui の Drag Source として「ASSET_PATH」 payload を提供。
//     受け取り側 (= ModelViewer 等の panel) は BeginDragDropTarget で
//     `AcceptDragDropPayload("ASSET_PATH")` するだけで wchar_t* を取り出せる。
//   ・拡張子からアセット種別 (EAssetKind) を判定し、kind フィルタ + アイコン
//     表示の足がかりにする。
//
// 使い方:
//   AssetBrowser browser;
//   browser.Init(L"assets");
//   // 毎フレーム
//   browser.DrawUI();
//   // 他 panel 側:
//   //   if (ImGui::BeginDragDropTarget()) {
//   //       const ImGuiPayload* p =
//   //           ImGui::AcceptDragDropPayload("ASSET_PATH");
//   //       if (p) {
//   //           const wchar_t* path = *static_cast<const wchar_t* const*>(p->Data);
//   //           ...
//   //       }
//   //       ImGui::EndDragDropTarget();
//   //   }
//
// 設計選択 (Phase 21a editor_core):
//   ・**非コピー / 非ムーブ**: 内部 Array<AssetEntry> + 文字列バッファ pool の
//     所有を曖昧にしない (HierarchyPanel / InspectorPanel と同じ規約)。
//   ・**全 noexcept**: ACS 規約。エラーは index out-of-range / 列挙失敗を
//     no-op (= 空ツリー) で表現する。
//   ・**STL 不使用**: ファイル列挙結果は `acs::Array<AssetEntry>`、文字列は
//     Array<wchar_t> + Array<char> の linear pool に積む方式 (= path / short_name
//     の生存期間を Pool の clear/再生成で揃え、AssetEntry はオフセットではなく
//     stabilize された pointer をそのまま持つ。再 Refresh で全部使い直す)。
//   ・**ImGui ヘッダは .cpp 側のみ**: ヘッダから imgui 依存を漏らさない方針
//     (ParticleEditorPanel / HierarchyPanel と同じ)。
//   ・**FileSystem 経由ではなく FindFirstFileW を .cpp 内で直接使う**: 現状
//     `platform/FileSystem.h` にはディレクトリ列挙 API が無い (ReadAllBytes /
//     FileSize / Exists のみ)。将来 FileSystem に `EnumerateDirectory` が
//     追加されたらここを差し替える。
//   ・**Drag payload は wchar_t* 直渡し**: payload identifier は
//     `"ASSET_PATH"` (ImGui 仕様: 32 文字以内)。payload data は wchar_t*
//     1 個 (= `sizeof(wchar_t*)` 8 bytes)。受け側は memcpy で取り出すこと
//     推奨 (Hierarchy の Node2D* 受け渡しと同じパターン)。pointer 寿命は
//     「次の Refresh まで」(= 文字列 pool が再生成されない間) を保証する。
//   ・**callback は raw 関数ポインタ + void* user**: ACS は std::function を
//     使えないため、ParticleEditorPanel / InspectorPanel と同形の C スタイル
//     callback を提供。
//   ・**EAssetKind は拡張子 lookup の 1 階層**: `.png/.jpg/.tga` → Texture、
//     `.mdl/.fbx/.gltf/.glb` → Mesh、`.ttf/.otf` → Font、`.wav/.ogg/.mp3` →
//     Audio、`.mat`/`.material` → Material、`.fx`/`.particle` → Particle、
//     `.anim` → Animation、`.bt` → BehaviorTree、`.tilemap`/`.tmx` → Tilemap、
//     `.prefab` → Prefab、`.cine` → Cinematic、`.scene` → Scene、未知は Other。
//     大文字小文字無視。
//
// 範囲外 (将来):
//   ・Thumbnail プレビュー (Texture / Mesh 用 preview image 表示)
//   ・Favorites / pin
//   ・Search box (substring match)
//   ・New folder / rename / delete (in-place editing)
//   ・AssetPack (.acpak) overlay 表示 (Pillar G AssetPack 統合時)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game::editor_core {

// ---- アセット種別 -------------------------------------------------------
// 拡張子から推測されるアセット種別。Unknown は未分類 (= 拡張子が知られていない、
// もしくは ClassifyByExtension にとって path == nullptr / 空文字)。Other は
// 既知拡張子だがどの分類にも当てはまらない汎用ファイル (= `.txt` 等の場合に
// 利用側が自由に拡張できるよう枠だけ確保)。E prefix は ACS Phase 19a 規約。
enum class EAssetKind : u8 {
    Unknown      = 0,
    Texture      = 1,   // .png .jpg .jpeg .tga .bmp .dds .ktx .hdr
    Mesh         = 2,   // .mdl .fbx .gltf .glb .obj
    Font         = 3,   // .ttf .otf
    Audio        = 4,   // .wav .ogg .mp3 .flac
    Material     = 5,   // .mat .material
    Particle     = 6,   // .fx .particle
    Animation    = 7,   // .anim
    BehaviorTree = 8,   // .bt
    Tilemap      = 9,   // .tilemap .tmx
    Prefab       = 10,  // .prefab
    Cinematic    = 11,  // .cine
    Scene        = 12,  // .scene
    Other        = 13,  // 既知だがどれにも該当しない (現状未使用 / 利用側拡張用)
};

// ---- AssetEntry — 列挙された 1 件のディレクトリ / ファイル ------------
// `path` / `short_name` は AssetBrowser の内部 pool 内のメモリを参照する。
// 寿命は「次の Refresh() を呼ぶまで」のみ有効 (= 次回再列挙時に pool が
// クリアされ pointer は無効化される)。コピーして保存する必要があれば
// 利用側でバッファに退避すること。
struct AssetEntry {
    const wchar_t* path             = nullptr;  // assets/ ルートからの相対パス (区切りは `\\`)
    const char*    short_name       = nullptr;  // path の末尾セグメント (UTF-8)
    EAssetKind     kind             = EAssetKind::Unknown;
    u64            file_size_bytes  = 0;        // is_directory == true なら 0
    u64            last_modified    = 0;        // Win32 FILETIME 100ns 単位
    bool           is_directory     = false;
};

// ---------------------------------------------------------------------------
// AssetBrowser — assets/ ディレクトリツリー + ImGui 描画 + Drag Source
// ---------------------------------------------------------------------------
class AssetBrowser {
public:
    // 選択 / ダブルクリック通知 callback。`user` は SetOn*Callback の第二引数
    // で渡したポインタがそのまま戻る (closure 代替)。`path` の寿命は
    // 「次の Refresh() まで」(= AssetEntry::path と同じルール)。
    using AssetSelectedCallback      = void (*)(void* user,
                                                 const wchar_t* path,
                                                 EAssetKind kind) noexcept;
    using AssetDoubleClickedCallback = void (*)(void* user,
                                                 const wchar_t* path,
                                                 EAssetKind kind) noexcept;

    AssetBrowser() noexcept = default;
    ~AssetBrowser() noexcept = default;

    // 非コピー・非ムーブ: 内部 Array / Pool / callback 状態の所有を曖昧に
    // しない (ACS 規約)。
    AssetBrowser(const AssetBrowser&)            = delete;
    AssetBrowser& operator=(const AssetBrowser&) = delete;
    AssetBrowser(AssetBrowser&&)                 = delete;
    AssetBrowser& operator=(AssetBrowser&&)      = delete;

    // 初期化: `root_directory` (UTF-16 wchar_t) を assets/ ルートとして記録し、
    // 初回 Refresh() を実行する。多重 Init 可能 (= 別 root への切替に使える)。
    // nullptr / 空文字を渡した場合は L"assets" を既定として採用する。
    void Init(const wchar_t* root_directory) noexcept;

    // 後片付け: 文字列 pool / Array / callback を全解放。多重 Shutdown 可能。
    void Shutdown() noexcept;

    // current_directory 配下を rescan する。pool は丸ごと再構築されるため
    // 既存 AssetEntry::path / short_name は無効化される。呼び出し側は
    // Refresh() 越しに pointer を保持しないこと。
    void Refresh() noexcept;

    // メイン ImGui window 描画。`Begin("Asset Browser")` 1 window で完結。
    // レイアウト:
    //   ┌─ "Asset Browser" window ────────────────────────────────────┐
    //   │ [Refresh] [Up]  Path: <current>     [Filter: <kind combo>]   │
    //   │ ┌─ left (tree) ─┐ ┌─ right (list) ──────────────────────┐    │
    //   │ │ ▼ assets       │ │ [DIR ] Folder0                       │    │
    //   │ │   ▼ textures   │ │ [TEX ] hero.png        12.3 KB       │    │
    //   │ │     hero.png   │ │ [MESH] level.glb        4.5 MB       │    │
    //   │ │   meshes       │ │ ...                                   │    │
    //   │ └────────────────┘ └───────────────────────────────────────┘    │
    //   └───────────────────────────────────────────────────────────────┘
    // 各エントリは Drag Source ("ASSET_PATH" / wchar_t*)。
    void DrawUI() noexcept;

    // current directory に存在する entry の数 (ディレクトリ + ファイル合算)。
    u32 EntryCount() const noexcept;

    // index 番目の entry。範囲外は nullptr。pointer 寿命は次回 Refresh まで。
    const AssetEntry* GetEntry(u32 index) const noexcept;

    // 現在表示中のディレクトリ (assets/ ルートからの相対パス、wchar_t)。
    // root 直下では L"" を返す (= 空文字)。
    const wchar_t* CurrentDirectory() const noexcept;

    // current directory を変更する。`path` は assets/ ルートからの相対パスを
    // 想定 (空文字でルートに戻る)。内部で必ず Refresh() を呼ぶ。
    // root_directory + path がディレクトリ存在しない場合は no-op (= 既存
    // current_directory のまま)。
    void SetCurrentDirectory(const wchar_t* path) noexcept;

    // 現在選択中の AssetEntry::path。未選択は nullptr。
    // 寿命は次回 Refresh まで。
    const wchar_t* SelectedAssetPath() const noexcept;

    // 現在選択中の AssetEntry::kind。未選択は EAssetKind::Unknown。
    EAssetKind SelectedAssetKind() const noexcept;

    // 選択変更通知 callback を登録 / 解除 (nullptr で解除)。
    void SetOnAssetSelectedCallback(AssetSelectedCallback cb, void* user) noexcept;

    // ダブルクリック (ディレクトリは Navigate、ファイルは "Open" 相当) 通知
    // callback を登録 / 解除。
    void SetOnAssetDoubleClickedCallback(AssetDoubleClickedCallback cb, void* user) noexcept;

    // kind フィルタを設定する。EAssetKind::Unknown を渡すとフィルタ解除
    // (= 全 entry 表示)。ディレクトリ entry はフィルタの影響を受けない
    // (= 常に表示) ことに注意。
    void SetFilterByKind(EAssetKind kind) noexcept;

    // 拡張子から EAssetKind を判定する static ヘルパ。`path == nullptr` /
    // 拡張子無しは EAssetKind::Unknown を返す。大文字小文字無視。
    static EAssetKind ClassifyByExtension(const wchar_t* path) noexcept;

    // ---- 公開定数 -------------------------------------------------------
    // Drag & Drop payload identifier (ImGui 仕様: 32 文字以内)。
    static constexpr const char* kDragDropPayloadId = "ASSET_PATH";

    // パス pool / 名前 pool の初期容量 (= 1 文字あたりのバイト数 × 平均長 ×
    // 期待 entry 数)。assets/ ルートでよく取られる 256〜1024 entry / 平均
    // 64 文字 を見越して 64 KB を確保する (足りなければ Array が自動成長)。
    static constexpr usize kInitialPathPoolBytes = 64u * 1024u;

    // 1 entry あたりの最大 path 長 (wchar_t 単位、終端含まず)。FindFirstFileW
    // が返す MAX_PATH (260) + 親パス長を考慮した余裕値。
    static constexpr usize kMaxPathChars = 512u;

private:
    // root_directory 文字列の保持 (Init 呼び出しでコピー)。最大長 kMaxPathChars。
    wchar_t                _root_directory[kMaxPathChars] = {};

    // current_directory 文字列 (root からの相対、`\\` 区切り)。root 直下では空文字。
    wchar_t                _current_directory[kMaxPathChars] = {};

    // current_directory 配下を rescan した結果の AssetEntry 群。
    // entry の path / short_name pointer は下の pool 内を指している。
    Array<AssetEntry>      _entries {};

    // 文字列 pool (wchar_t 用 = path、char 用 = short_name)。Refresh で
    // Clear & 再構築。Reserve しておけば address 安定性が確保される
    // (= 一度 Reserve した capacity を超えなければ Grow しない)。
    Array<wchar_t>         _path_pool {};
    Array<char>            _name_pool {};

    // 選択中 entry の index (= _entries 内の位置)。-1 で未選択。
    // i32 を採用するのは ParticleEditorPanel と同じ規約 (-1 = 「なし」)。
    i32                    _selected_index    = -1;

    // kind フィルタ。EAssetKind::Unknown は「フィルタなし」を意味する。
    EAssetKind             _filter_kind       = EAssetKind::Unknown;

    // 選択 / ダブルクリック通知 callback。
    AssetSelectedCallback       _on_selected_cb       = nullptr;
    void*                       _on_selected_user     = nullptr;
    AssetDoubleClickedCallback  _on_double_clicked_cb = nullptr;
    void*                       _on_double_clicked_user = nullptr;

    // ---- 内部ヘルパ (実装詳細は .cpp 側) --------------------------------
    // current_directory 配下を Win32 FindFirstFileW で列挙して
    // `_entries` / `_path_pool` / `_name_pool` を再構築。
    void RebuildEntries() noexcept;

    // root_directory + sub (relative) を 1 つのフルパスに合成する。
    // out_buf に書き込む (cap は wchar_t 単位の容量、終端含む)。
    void BuildFullPath(const wchar_t* sub, wchar_t* out_buf, usize cap) const noexcept;

    // _path_pool に wchar_t 文字列を末尾追加し、書き込み開始の **offset** を返す。
    // pointer を返さないのは、列挙ループ中の Reserve / Grow で過去に取った
    // pointer が無効化されるのを避けるため (絶対 pointer は列挙完走後に
    // `&_path_pool[offset]` で resolve する 2 段構え)。
    usize AppendPathOffset(const wchar_t* src) noexcept;

    // _name_pool に UTF-8 文字列を末尾追加し、書き込み開始の offset を返す
    // (AppendPathOffset と同じ理由で pointer を返さない)。
    usize AppendNameOffset(const char* src) noexcept;

    // 左 tree ペイン描画 (再帰)。引数 `rel_dir` は assets/ ルートからの相対パス。
    void DrawTreeRecursive(const wchar_t* rel_dir, u32 depth) noexcept;

    // 右 list ペイン描画 (current_directory の `_entries` を表示)。
    void DrawList() noexcept;
};

} // namespace acs::game::editor_core
