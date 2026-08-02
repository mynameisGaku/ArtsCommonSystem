// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K (editor_core) — CAssetBrowser (editor 共通基盤)
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
//   CAssetBrowser browser;
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
// 設計選択:
//   ・**非コピー / 非ムーブ**: 内部 TArray<FAssetEntry> + 文字列バッファ pool の
//     所有を曖昧にしない (AHierarchyPanel / AInspectorPanel と同じ規約)。
//   ・**全 noexcept**: ACS 規約。エラーは index out-of-range / 列挙失敗を
//     no-op (= 空ツリー) で表現する。
//   ・**STL 不使用**: ファイル列挙結果は `acs::TArray<FAssetEntry>`、文字列は
//     TArray<wchar_t> + TArray<char> の linear pool に積む方式 (= path / short_name
//     の生存期間を TPool の clear/再生成で揃え、FAssetEntry はオフセットではなく
//     stabilize された pointer をそのまま持つ。再 Refresh で全部使い直す)。
//   ・**ImGui ヘッダは .cpp 側のみ**: ヘッダから imgui 依存を漏らさない方針
//     (AParticleEditorPanel / AHierarchyPanel と同じ)。
//   ・**CFileSystem 経由ではなく FindFirstFileW を .cpp 内で直接使う**: 現状
//     `platform/FileSystem.h` にはディレクトリ列挙 API が無い (ReadAllBytes /
//     FileSize / Exists のみ)。将来 CFileSystem に `EnumerateDirectory` が
//     追加されたらここを差し替える。
//   ・**Drag payload は wchar_t* 直渡し**: payload identifier は
//     `"ASSET_PATH"` (ImGui 仕様: 32 文字以内)。payload data は wchar_t*
//     1 個 (= `sizeof(wchar_t*)` 8 bytes)。受け側は memcpy で取り出すこと
//     推奨 (Hierarchy の ANode* 受け渡しと同じパターン)。pointer 寿命は
//     「次の Refresh まで」(= 文字列 pool が再生成されない間) を保証する。
//   ・**callback は raw 関数ポインタ + void* user**: ACS は std::function を
//     使えないため、AParticleEditorPanel / AInspectorPanel と同形の C スタイル
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

/**
 * 拡張子から推測されるアセット種別。
 *
 * @details
 * Unknown は未分類 (= 拡張子が知られていない、もしくは ClassifyByExtension にとって
 * path == nullptr / 空文字)。Other は既知拡張子だがどの分類にも当てはまらない汎用
 * ファイル (= 利用側が自由に拡張できるよう枠だけ確保)。E prefix は ACS 規約。
 */
enum class EAssetKind : u8 {
    /** 未分類 (拡張子不明、または path が nullptr / 空文字)。 */
    Unknown      = 0,

    /** テクスチャ (.png .jpg .jpeg .tga .bmp .dds .ktx .hdr)。 */
    Texture      = 1,

    /** メッシュ (.mdl .fbx .gltf .glb .obj)。 */
    Mesh         = 2,

    /** フォント (.ttf .otf)。 */
    Font          = 3,

    /** オーディオ (.wav .ogg .mp3 .flac)。 */
    Audio        = 4,

    /** マテリアル (.mat .material)。 */
    Material     = 5,

    /** パーティクル (.fx .particle)。 */
    Particle     = 6,

    /** アニメーション (.anim)。 */
    Animation     = 7,

    /** ビヘイビアツリー (.bt)。 */
    BehaviorTree  = 8,

    /** タイルマップ (.tilemap .tmx)。 */
    Tilemap       = 9,

    /** プレハブ (.prefab)。 */
    Prefab       = 10,

    /** シネマティック (.cine)。 */
    Cinematic    = 11,

    /** シーン (.scene)。 */
    Scene        = 12,

    /** 既知だがどれにも該当しない汎用ファイル (現状未使用 / 利用側拡張用)。 */
    Other        = 13,
};

/**
 * 列挙された 1 件のディレクトリ / ファイルを表すエントリ。
 *
 * @details
 * path / short_name は CAssetBrowser の内部 pool 内のメモリを参照する。寿命は
 * 「次の Refresh() を呼ぶまで」のみ有効 (= 次回再列挙時に pool がクリアされ pointer は
 * 無効化される)。コピーして保存する必要があれば利用側でバッファに退避すること。
 */
struct FAssetEntry {
    /** assets/ ルートからの相対パス (区切りは `\\`)。 */
    const wchar_t* path             = nullptr;

    /** path の末尾セグメント (UTF-8)。 */
    const char*    short_name       = nullptr;

    /** 拡張子から推測したアセット種別 (ディレクトリは Unknown)。 */
    EAssetKind     kind             = EAssetKind::Unknown;

    /** ファイルサイズ (バイト、is_directory == true なら 0)。 */
    u64            file_size_bytes  = 0;

    /** 最終更新時刻 (Win32 FILETIME 100ns 単位)。 */
    u64            last_modified    = 0;

    /** ディレクトリなら true。 */
    bool           is_directory     = false;
};

/**
 * assets/ ディレクトリツリーを ImGui で参照し Drag Source としてパスを供給するパネル。
 *
 * @details
 * Unity の Project Window / Godot の FileSystem Dock 相当の基盤パネル。assets/ ルートを
 * 起点に左ペインへ tree、右ペインへ current directory のエントリ一覧を表示し、各エントリは
 * "ASSET_PATH" payload (wchar_t*) を提供する Drag Source となる。非コピー / 非ムーブ、
 * 全 noexcept、STL 不使用で、文字列は内部 pool に積み FAssetEntry はそこへの pointer を持つ
 * (寿命は次の Refresh まで)。ImGui / Win32 列挙依存は .cpp 側に閉じる。
 */
class CAssetBrowser {
public:
    /**
     * アセット選択変更を通知する callback の型。
     *
     * @details user は SetOnAssetSelectedCallback の第二引数がそのまま戻る (closure 代替)。path の寿命は次の Refresh まで。
     * @param user 登録時に渡した任意ポインタ。
     * @param path 選択されたアセットの相対パス。
     * @param kind 選択されたアセットの種別。
     */
    using AssetSelectedCallback      = void (*)(void* user,
                                                 const wchar_t* path,
                                                 EAssetKind kind) noexcept;

    /**
     * アセットのダブルクリックを通知する callback の型。
     *
     * @details user は SetOnAssetDoubleClickedCallback の第二引数がそのまま戻る。path の寿命は次の Refresh まで。
     * @param user 登録時に渡した任意ポインタ。
     * @param path ダブルクリックされたアセットの相対パス。
     * @param kind ダブルクリックされたアセットの種別。
     */
    using AssetDoubleClickedCallback = void (*)(void* user,
                                                 const wchar_t* path,
                                                 EAssetKind kind) noexcept;

    /** 空状態で構築する (列挙は Init で行う)。 */
    CAssetBrowser() noexcept = default;

    /** デストラクタ (pool / TArray は各自のデストラクタが解放)。 */
    ~CAssetBrowser() noexcept = default;

    /** コピー禁止 (内部 TArray / pool / callback 状態の所有を曖昧にしないため)。 */
    CAssetBrowser(const CAssetBrowser&)            = delete;

    /** コピー代入も禁止。 */
    CAssetBrowser& operator=(const CAssetBrowser&) = delete;

    /** ムーブ禁止。 */
    CAssetBrowser(CAssetBrowser&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CAssetBrowser& operator=(CAssetBrowser&&)      = delete;

    /**
     * root_directory を assets/ ルートとして記録し初回 Refresh を実行する。
     *
     * @details 多重 Init 可能 (= 別 root への切替に使える)。
     * @param root_directory assets/ ルートのパス (UTF-16、nullptr / 空文字なら L"assets" を採用)。
     */
    void Init(const wchar_t* root_directory) noexcept;

    /** 文字列 pool / TArray / callback を全解放する (多重 Shutdown 可能)。 */
    void Shutdown() noexcept;

    /**
     * current_directory 配下を rescan する。
     *
     * @details pool が丸ごと再構築されるため既存 FAssetEntry::path / short_name は無効化される。呼び出し側は Refresh 越しに pointer を保持しないこと。
     */
    void Refresh() noexcept;

    /**
     * メインの ImGui window を描画する。
     *
     * @details
     * Begin("Asset Browser") 1 window で完結し、上部にツールバー (Refresh / Up / Path / Filter)、
     * 左に tree ペイン、右に current directory の entry 一覧を表示する。各エントリは
     * "ASSET_PATH" payload (wchar_t*) の Drag Source となる。
     */
    void DrawUI() noexcept;

    /**
     * current directory に存在する entry の数を返す。
     *
     * @return ディレクトリ + ファイルの合算件数。
     */
    u32 EntryCount() const noexcept;

    /**
     * index 番目の entry を返す。
     *
     * @param index 取得する entry のインデックス。
     * @return index 番目の entry (範囲外は nullptr)。pointer 寿命は次回 Refresh まで。
     */
    const FAssetEntry* GetEntry(u32 index) const noexcept;

    /**
     * 現在表示中のディレクトリを返す。
     *
     * @return assets/ ルートからの相対パス (root 直下では L"" = 空文字)。
     */
    const wchar_t* CurrentDirectory() const noexcept;

    /**
     * current directory を変更する。
     *
     * @details 内部で必ず Refresh を呼ぶ。root_directory + path がディレクトリとして存在しない場合は no-op (= 既存 current_directory のまま)。
     * @param path assets/ ルートからの相対パス (nullptr / 空文字でルートに戻る)。
     */
    void SetCurrentDirectory(const wchar_t* path) noexcept;

    /**
     * 現在選択中のアセットパスを返す。
     *
     * @return 選択中アセットの相対パス (未選択は nullptr)。寿命は次回 Refresh まで。
     */
    const wchar_t* SelectedAssetPath() const noexcept;

    /**
     * 現在選択中のアセット種別を返す。
     *
     * @return 選択中アセットの EAssetKind (未選択は EAssetKind::Unknown)。
     */
    EAssetKind SelectedAssetKind() const noexcept;

    /**
     * 選択変更通知 callback を登録 / 解除する。
     *
     * @param cb 登録する callback (nullptr で解除)。
     * @param user callback に渡す任意ポインタ。
     */
    void SetOnAssetSelectedCallback(AssetSelectedCallback cb, void* user) noexcept;

    /**
     * ダブルクリック通知 callback を登録 / 解除する。
     *
     * @details ディレクトリは Navigate、ファイルは "Open" 相当の操作時に呼ばれる。
     * @param cb 登録する callback (nullptr で解除)。
     * @param user callback に渡す任意ポインタ。
     */
    void SetOnAssetDoubleClickedCallback(AssetDoubleClickedCallback cb, void* user) noexcept;

    /**
     * kind フィルタを設定する。
     *
     * @details EAssetKind::Unknown を渡すとフィルタ解除 (= 全 entry 表示)。ディレクトリ entry はフィルタの影響を受けず常に表示される。
     * @param kind フィルタする種別 (Unknown でフィルタ解除)。
     */
    void SetFilterByKind(EAssetKind kind) noexcept;

    /**
     * 拡張子から EAssetKind を判定する static ヘルパ。
     *
     * @details 大文字小文字無視。
     * @param path 判定対象のパス。
     * @return 推測した EAssetKind (path == nullptr / 拡張子無しは EAssetKind::Unknown)。
     */
    static EAssetKind ClassifyByExtension(const wchar_t* path) noexcept;

    /** Drag & Drop payload identifier (ImGui 仕様: 32 文字以内)。 */
    static constexpr const char* kDragDropPayloadId = "ASSET_PATH";

    /**
     * パス pool / 名前 pool の初期容量 (バイト)。
     *
     * @details assets/ ルートでよく取られる 256〜1024 entry / 平均 64 文字を見越して 64 KB を確保する (足りなければ TArray が自動成長)。
     */
    static constexpr usize kInitialPathPoolBytes = 64u * 1024u;

    /**
     * 1 entry あたりの最大 path 長 (wchar_t 単位、終端含まず)。
     *
     * @details FindFirstFileW が返す MAX_PATH (260) + 親パス長を考慮した余裕値。
     */
    static constexpr usize kMaxPathChars = 512u;

private:
    /** assets/ ルートのパス (Init でコピー、最大長 kMaxPathChars)。 */
    wchar_t                m_RootDirectory[kMaxPathChars] = {};

    /** 現在表示中のディレクトリ (root からの相対、`\\` 区切り、root 直下では空文字)。 */
    wchar_t                m_CurrentDirectory[kMaxPathChars] = {};

    /**
     * current_directory 配下を rescan した結果の FAssetEntry 群。
     *
     * @details entry の path / short_name pointer は下の文字列 pool 内を指す。
     */
    TArray<FAssetEntry>      m_Entries {};

    /**
     * path 文字列 (wchar_t) を積む pool。
     *
     * @details Refresh で Clear & 再構築。Reserve した capacity を超えなければ Grow せず address 安定性が確保される。
     */
    TArray<wchar_t>         m_PathPool {};

    /**
     * short_name 文字列 (UTF-8 char) を積む pool。
     *
     * @details m_PathPool と同じく Refresh で Clear & 再構築する。
     */
    TArray<char>            m_NamePool {};

    /** 選択中 entry の index (m_Entries 内の位置、-1 で未選択)。 */
    i32                    m_SelectedIndex    = -1;

    /** kind フィルタ (EAssetKind::Unknown は「フィルタなし」)。 */
    EAssetKind             m_FilterKind       = EAssetKind::Unknown;

    /** 選択変更通知 callback (未登録は nullptr)。 */
    AssetSelectedCallback       m_OnSelectedCb       = nullptr;

    /** 選択変更通知 callback に渡す任意ポインタ。 */
    void*                       m_OnSelectedUser     = nullptr;

    /** ダブルクリック通知 callback (未登録は nullptr)。 */
    AssetDoubleClickedCallback  m_OnDoubleClickedCb = nullptr;

    /** ダブルクリック通知 callback に渡す任意ポインタ。 */
    void*                       m_OnDoubleClickedUser = nullptr;

    /**
     * current_directory 配下を Win32 FindFirstFileW で列挙して内部状態を再構築する。
     *
     * @details m_Entries / m_PathPool / m_NamePool を作り直す。
     */
    void RebuildEntries() noexcept;

    /**
     * root_directory + sub を 1 つのフルパスに合成して out_buf に書き込む。
     *
     * @param sub root からの相対パス (空 / nullptr ならルート直下)。
     * @param out_buf 書き込み先バッファ。
     * @param cap out_buf の容量 (wchar_t 単位、終端含む)。
     */
    void BuildFullPath(const wchar_t* sub, wchar_t* out_buf, usize cap) const noexcept;

    /**
     * m_PathPool に wchar_t 文字列を末尾追加し書き込み開始の offset を返す。
     *
     * @details
     * pointer を返さないのは、列挙ループ中の Reserve / Grow で過去に取った pointer が
     * 無効化されるのを避けるため (絶対 pointer は列挙完走後に &m_PathPool[offset] で resolve する 2 段構え)。
     * @param src 追加する wchar_t 文字列。
     * @return 書き込み開始位置の offset (m_PathPool 先頭からの要素数)。
     */
    usize AppendPathOffset(const wchar_t* src) noexcept;

    /**
     * m_NamePool に UTF-8 文字列を末尾追加し書き込み開始の offset を返す。
     *
     * @details AppendPathOffset と同じ理由で pointer ではなく offset を返す。
     * @param src 追加する UTF-8 文字列。
     * @return 書き込み開始位置の offset (m_NamePool 先頭からのバイト数)。
     */
    usize AppendNameOffset(const char* src) noexcept;

    /**
     * 左 tree ペインを再帰描画する。
     *
     * @param rel_dir assets/ ルートからの相対パス (空文字でルート)。
     * @param depth 現在の再帰深度 (32 で打ち切り)。
     */
    void DrawTreeRecursive(const wchar_t* rel_dir, u32 depth) noexcept;

    /** 右 list ペイン (current_directory の m_Entries) を描画する。 */
    void DrawList() noexcept;
};

using FAssetBrowser = CAssetBrowser;

} // namespace acs::game::editor_core
